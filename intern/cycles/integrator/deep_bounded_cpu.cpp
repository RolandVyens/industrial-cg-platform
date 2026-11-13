/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */

#include "integrator/deep_bounded_cpu.h"

#include <array>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstring>
#include <new>
#include <type_traits>

#include "device/device.h"
#include "integrator/deep_bounded_layout.h"
#include "integrator/deep_bounded_plan.h"
#include "session/buffers.h"
#include "session/deep_frame_production.h"
#include "util/half.h"
#include "util/tbb.h"

namespace {
using std::isfinite;
#define __device__
#define __forceinline__ ccl_always_inline
#define __noinline__ ccl_never_inline
inline uint32_t atomicOr(uint32_t *p, const uint32_t v)
{
  const uint32_t before = *p;
  *p |= v;
  return before;
}
// clang-format off
#include "kernel/integrator/deep_bounded_records.h"
#include "kernel/integrator/deep_bounded_numeric.h"
// clang-format on
#undef __noinline__
#undef __forceinline__
#undef __device__

template<typename T> uint64_t addr(T *p)
{
  return reinterpret_cast<uintptr_t>(p);
}

template<bool Physical> struct alignas(64) CPURoot {
  std::array<float, DEEP_BOUNDED_K> z, z_back;
  std::array<std::conditional_t<Physical, DeepBoundedRGBA, float>, DEEP_BOUNDED_K> rgba;
  std::array<uint32_t, DEEP_BOUNDED_K> object, shader, normal;
  std::conditional_t<Physical, std::array<float, 3>, std::array<float, 0>> pending;
  DeepBoundedPacketControl control = {};
  DeepBoundedBinding binding = {};
  uint32_t header = 0, pixel = 0;
  float mq[2] = {1, 0};

  void bind(const DeepBoundedMergePolicy &merge, const bool holdout, const uint32_t policy)
  {
    binding = {};
    binding.packet_device = addr(&control);
    binding.raw = {addr(z.data()),
                   addr(z_back.data()),
                   addr(rgba.data()),
                   addr(object.data()),
                   addr(shader.data()),
                   addr(normal.data())};
    binding.headers = addr(&header);
    binding.holdout_mq = holdout ? addr(mq) : 0;
    if constexpr (Physical)
      binding.no_anchor = addr(pending.data());
    binding.has_holdout = holdout;
    binding.policy = policy;
    binding.merge = merge;
  }
};
static_assert(std::is_trivially_destructible_v<CPURoot<false>>);
static_assert(std::is_trivially_destructible_v<CPURoot<true>>);

using ccl::deep_bounded_same_family;
using ccl::deep_bounded_encoding_owner;
using ccl::deep_bounded_family_count;

template<bool Physical> struct CPUShape {
  DeepBoundedPlanes collector = {}, models[2] = {};
  uint64_t populations = 0, counts = 0, model_counts[2] = {}, offsets = 0, roots = 0;
  ccl::DeepBoundedArenaLayout layout;

  CPUShape(const uint64_t base, const uint32_t p, const uint32_t threads, const uint32_t families)
  {
    collector = layout.planes(base, size_t(p) * DEEP_BOUNDED_K, Physical);
    counts = base + layout.reserve(size_t(p), 4, 64);
    populations = base + layout.reserve(size_t(p), 4, 64);
    for (uint32_t i = 0; i < std::min(families, 2u); ++i) {
      models[i] = layout.planes(base, size_t(p) * DEEP_BOUNDED_K, Physical);
      model_counts[i] = base + layout.reserve(p, 4);
    }
    offsets = base + layout.reserve(size_t(p) + 1, 8);
    roots = base + layout.reserve(threads, sizeof(CPURoot<Physical>), alignof(CPURoot<Physical>));
  }
};

template<bool Physical>
bool admission(const ccl::DeepFrameCaptureConsumer &consumer,
               const uint32_t pixels,
               const uint32_t threads,
               ccl::DeepCPUAdmission &result)
{
  const auto configs = consumer.bounded_consumers();
  if (!pixels || pixels >= (1u << 30) || !threads || configs.empty())
    return false;
  const CPUShape<Physical> shape(0, pixels, threads, deep_bounded_family_count(configs));
  if (!shape.layout.valid)
    return false;
  result = {};
  result.arena = shape.layout.bytes;
  /* Bounded source-level stack allowance; generated native CPU stack is also
   * part of resource qualification. This credit allocates no dummy buffers. */
  const size_t scratch = sizeof(DeepBoundedRecord<Physical>) *
                             (candidate_capacity + 3 * DEEP_BOUNDED_K) +
                         512;
  if (!ccl::deep_bounded_mul(threads, scratch, result.scratch))
    return false;
  for (size_t i = 0; i < configs.size(); ++i) {
    if (deep_bounded_encoding_owner(configs, i) != i)
      continue;
    const auto &config = configs[i].config;
    const bool rgb = Physical && !config.alpha_only;
    const size_t bytes = ccl::deep_bounded_record_bytes(
        config.precision == ccl::DeepCanonicalPrecision::Half, rgb);
    const size_t summary = !Physical && consumer.capture_demand().needs_rgb && !config.alpha_only ?
                               4 :
                               0;
    size_t value;
    if (!ccl::deep_bounded_mul(pixels, bytes * DEEP_BOUNDED_K + summary + 8, value) ||
        !ccl::deep_bounded_add(value, 8, value) ||
        !ccl::deep_bounded_add(result.output_credit, value, result.output_credit))
      return false;
  }
  if (!Physical && consumer.capture_demand().needs_rgb) {
    size_t beauty;
    if (!ccl::deep_bounded_mul(pixels, 12, beauty) ||
        !ccl::deep_bounded_add(result.output_credit, beauty, result.output_credit))
      return false;
  }
  return ccl::deep_bounded_add(result.arena, result.scratch, result.peak) &&
         ccl::deep_bounded_add(result.peak, result.output_credit, result.peak);
}

/* No value-initialization of hundreds of MiB of unread record planes. */
class CPUArena {
 public:
  CPUArena(const ccl::DeepMemoryBudget &host, const ccl::DeepMemoryBudget &work)
      : host_(host), work_(work)
  {
  }
  ~CPUArena()
  {
    clear();
  }
  void reserve(const size_t bytes)
  {
    if (bytes <= size_)
      return;
    clear();
    host_.acquire(bytes);
    try {
      work_.acquire(bytes);
    }
    catch (...) {
      host_.relinquish(bytes);
      throw;
    }
    try {
      pointer_ = ::operator new(bytes, std::align_val_t(64));
    }
    catch (...) {
      host_.relinquish(bytes);
      work_.relinquish(bytes);
      throw;
    }
    size_ = bytes;
  }
  uint64_t address() const
  {
    return reinterpret_cast<uintptr_t>(pointer_);
  }
  size_t bytes() const
  {
    return size_;
  }

 private:
  void clear()
  {
    if (pointer_) {
      ::operator delete(pointer_, std::align_val_t(64));
      host_.relinquish(size_);
      work_.relinquish(size_);
      pointer_ = nullptr;
      size_ = 0;
    }
  }
  ccl::DeepMemoryBudget host_, work_;
  void *pointer_ = nullptr;
  size_t size_ = 0;
};

template<typename T, typename Buffer>
void store(Buffer &b,
           const size_t at,
           const T &value)
{
  std::memcpy(b.data() + at, &value, sizeof(T));
}

float encode_value(ccl::DeepBoundedHostPayload &payload, const size_t at, const float value)
{
  const float limit = payload.half ? 65504.0f : FLT_MAX;
  const float finite = fminf(fmaxf(value, -limit), limit);
  if (payload.half) {
    const ccl::half h = ccl::float_to_half(finite);
    store(payload.planes, at, uint16_t(h));
    return ccl::half_to_float(h);
  }
  store(payload.planes, at, finite);
  return finite;
}
}  // namespace

CCL_NAMESPACE_BEGIN

struct DeepBoundedCPUStorage {
  DeepBoundedCPUStorage(Device &device, DeepFrameCaptureConsumer &consumer)
      : device(device),
        consumer(consumer),
        host(consumer.memory_budget()),
        work(consumer.device_memory_budget()),
        arena(host, work),
        policy(!consumer.capture_demand().needs_rgb  ? 1u :
               consumer.capture_demand().simple_mode ? 2u :
                                                       3u)
  {
  }
  virtual ~DeepBoundedCPUStorage() = default;
  virtual bool configure(const BufferParams &, const BufferParams &, uint32_t, bool, float) = 0;
  virtual const DeepBoundedBinding *begin(uint32_t, int, int, int) = 0;
  virtual bool end(uint32_t) = 0;
  virtual bool prepare() = 0;
  virtual bool close() = 0;
  bool owns(const int x, const int y) const
  {
    return int64_t(x) >= window.x && int64_t(y) >= window.y &&
           int64_t(x) - window.x < window.width && int64_t(y) - window.y < window.height;
  }
  bool good() const
  {
    return status.load(std::memory_order_relaxed) == 0 && !device.have_error();
  }
  bool drain()
  {
    if (!pending)
      return good();
    if (!good() || consumer.capture_cancelled())
      return false;
    if (!consumer.publish_bounded_tile(std::move(*pending)))
      return false;
    pending.reset();
    return true;
  }
  bool beauty(const RenderBuffers &buffers)
  {
    if (!pending || policy != 2)
      return true;
    return deep_extract_bounded_beauty(buffers, pending->window, pending->beauty);
  }
  Device &device;
  DeepFrameCaptureConsumer &consumer;
  DeepMemoryBudget host, work;
  CPUArena arena;
  uint32_t policy, capacity = 0, pixels = 0, threads = 0;
  DeepRasterWindow window = {};
  DeepBoundedMergePolicy strict = {.01f, .01f, .006f, .002f, 0};
  std::atomic<uint32_t> status{0};
  std::unique_ptr<DeepBoundedHostTile> pending;
  bool prepared = false, closed = false;
};

namespace {
template<bool Physical> class CPUStorage final : public DeepBoundedCPUStorage {
 public:
  CPUStorage(Device &device, DeepFrameCaptureConsumer &consumer)
      : DeepBoundedCPUStorage(device, consumer)
  {
  }

  bool configure(const BufferParams &logical,
                 const BufferParams &slice,
                 const uint32_t count,
                 const bool holdout,
                 const float projection) override
  {
    const int64_t x = std::max(int64_t(logical.full_x) + logical.window_x,
                               int64_t(slice.full_x) + slice.window_x);
    const int64_t y = std::max(int64_t(logical.full_y) + logical.window_y,
                               int64_t(slice.full_y) + slice.window_y);
    const int64_t right = std::min(int64_t(logical.full_x) + logical.window_x +
                                       logical.window_width,
                                   int64_t(slice.full_x) + slice.window_x + slice.window_width);
    const int64_t bottom = std::min(int64_t(logical.full_y) + logical.window_y +
                                        logical.window_height,
                                    int64_t(slice.full_y) + slice.window_y + slice.window_height);
    if (right <= x || bottom <= y || right - x >= INT_MAX || bottom - y >= INT_MAX)
      return false;
    const DeepRasterWindow next{int(x), int(y), uint32_t(right - x), uint32_t(bottom - y)};
    if (threads == count && next.x == window.x && next.y == window.y &&
        next.width == window.width && next.height == window.height)
      return good();
    if (!drain())
      return false;
    const uint64_t p = uint64_t(next.width) * next.height;
    if (p >= (1u << 30))
      return false;
    const uint32_t retained = std::max(capacity, uint32_t(p));
    DeepCPUAdmission need;
    if (!admission<Physical>(consumer, retained, count, need) ||
        need.peak > work.quota() - work.used() + arena.bytes())
      return false;
    arena.reserve(need.arena);
    capacity = retained;
    pixels = uint32_t(p);
    threads = count;
    window = next;
    const auto configs = consumer.bounded_consumers();
    strict.alpha_tolerance = float(configs.front().config.alpha_tolerance);
    strict.depth_tolerance = float(configs.front().config.depth_tolerance);
    for (const auto &c : configs) {
      strict.alpha_tolerance = std::min(strict.alpha_tolerance, float(c.config.alpha_tolerance));
      strict.depth_tolerance = std::min(strict.depth_tolerance, float(c.config.depth_tolerance));
    }
    strict.projection_scale = projection;
    shape = std::make_unique<CPUShape<Physical>>(
        arena.address(), capacity, threads, deep_bounded_family_count(configs));
    auto *counts = deep_bounded_ptr<uint32_t>(shape->counts);
    auto *populations = deep_bounded_ptr<uint32_t>(shape->populations);
    /* CPU ownership is pixel-major: each owner updates its own cache lines,
     * rather than adjacent pixels fighting over every GPU-transposed plane. */
    std::fill_n(counts, size_t(pixels), 0u);
    std::fill_n(populations, size_t(pixels), 0u);
    for (uint32_t i = 0; i < threads; ++i) {
      auto *root = new (deep_bounded_ptr<CPURoot<Physical>>(shape->roots) + i) CPURoot<Physical>;
      root->bind(strict, holdout, policy);
    }
    prepared = closed = false;
    return true;
  }

  const DeepBoundedBinding *begin(const uint32_t worker,
                                  const int x,
                                  const int y,
                                  const int sample) override
  {
    if (worker >= threads || !owns(x, y) || sample < 0 || prepared) {
      status.fetch_or(DEEP_BOUNDED_RANGE, std::memory_order_relaxed);
      return nullptr;
    }
    auto &r = deep_bounded_ptr<CPURoot<Physical>>(shape->roots)[worker];
    r.pixel = uint32_t(y - window.y) * window.width + uint32_t(x - window.x);
    r.header = 0;
    r.mq[0] = 1;
    r.mq[1] = 0;
    if constexpr (Physical)
      r.pending.fill(0);
    r.control = {};
    auto &p = r.binding.packet;
    p = {};
    p.owned_x = x;
    p.owned_y = y;
    p.owned_w = p.owned_h = p.owned_pitch = 1;
    p.plane_pitch = p.root_capacity = p.num_samples = 1;
    p.start_sample = sample;
    p.status_ptr = addr(&r.control.status);
    r.control.packet = p;
    r.control.root_pitch = 1;
    return &r.binding;
  }

  bool end(const uint32_t worker) override
  {
    auto &r = deep_bounded_ptr<CPURoot<Physical>>(shape->roots)[worker];
    if (r.control.status)
      return fail(r.control.status);
    if (!(r.header & DEEP_BOUNDED_VALID))
      return true;
    if constexpr (Physical) {
      if (r.pending[0] != 0 || r.pending[1] != 0 || r.pending[2] != 0)
        return fail(DEEP_BOUNDED_NO_SOURCE);
    }
    const uint32_t count = r.header & DEEP_BOUNDED_COUNT_MASK;
    const uint64_t index = r.pixel;
    uint32_t &n = deep_bounded_ptr<uint32_t>(shape->populations)[index];
    uint32_t &old_count = deep_bounded_ptr<uint32_t>(shape->counts)[index];
    if (count > DEEP_BOUNDED_K || old_count > DEEP_BOUNDED_K || n == UINT32_MAX)
      return fail(DEEP_BOUNDED_POPULATION);
    const float inverse = 1.0f / float(n + 1);
    const Chain dst{shape->collector, index * DEEP_BOUNDED_K, 1, old_count};
    const Chain src{r.binding.raw, 0, 1, count, true};
    old_count = merge_chains<Physical>(
        dst, src, dst, float(n) * inverse, inverse, strict, addr(&r.control.status));
    ++n;
    return !r.control.status || fail(r.control.status);
  }

  bool prepare() override
  {
    if (prepared)
      return good();
    if (!good() || !shape || !pixels)
      return false;
    parallel([&](const uint32_t pixel) {
      uint32_t local_status = 0;
      const uint32_t population = deep_bounded_ptr<const uint32_t>(shape->populations)[pixel];
      const uint32_t count = deep_bounded_ptr<const uint32_t>(shape->counts)[pixel];
      uint32_t result = 0;
      if (!population || count > DEEP_BOUNDED_K) {
        local_status |= DEEP_BOUNDED_POPULATION;
      }
      else {
        const Chain src{shape->collector, uint64_t(pixel) * DEEP_BOUNDED_K, 1, count};
        const Chain dst{shape->models[0], uint64_t(pixel) * DEEP_BOUNDED_K, 1, 0};
        result = scale_chain<Physical>(src, dst, 1.0f, addr(&local_status));
      }
      deep_bounded_ptr<uint32_t>(shape->model_counts[0])[pixel] = result;
      if (local_status)
        fail(local_status);
    });
    prepared = good();
    return prepared;
  }

  bool close() override
  {
    if (closed)
      return good();
    if (!drain() || !prepare())
      return false;
    pending = std::make_unique<DeepBoundedHostTile>(host);
    pending->window = window;
    const auto configs = consumer.bounded_consumers();
    pending->consumers.resize(configs.size());
    for (size_t family = 0; family < configs.size(); ++family) {
      bool first = true;
      for (size_t j = 0; j < family; ++j)
        first &= !deep_bounded_same_family(configs[j].config, configs[family].config);
      if (!first)
        continue;
      DeepBoundedMergePolicy merge = strict;
      const auto &config = configs[family].config;
      merge.alpha_tolerance = float(config.alpha_tolerance);
      merge.depth_tolerance = float(config.depth_tolerance);
      const uint32_t which = merge.alpha_tolerance == strict.alpha_tolerance &&
                                     merge.depth_tolerance == strict.depth_tolerance ?
                                 0u :
                                 1u;
      if (which) {
        parallel([&](const uint32_t pixel) {
          const uint32_t count = deep_bounded_ptr<const uint32_t>(shape->model_counts[0])[pixel];
          DeepBoundedRecord<Physical> candidate[DEEP_BOUNDED_K];
          float prefix = 0;
          uint32_t error = 0;
          for (uint32_t i = 0; i < count; ++i) {
            auto &v = candidate[i];
            v = deep_bounded_load<Physical>(shape->models[0],
                                            uint64_t(pixel) * DEEP_BOUNDED_K + i);
            const float q = (1.0f - prefix) * v.alpha;
            prefix = deep_bounded_unit(prefix + q);
            v.alpha = q;
          }
          deep_bounded_ptr<uint32_t>(shape->model_counts[1])[pixel] = reduce_numeric(
              candidate,
              count,
              shape->models[1],
              uint64_t(pixel) * DEEP_BOUNDED_K,
              1,
              DEEP_BOUNDED_K,
              merge,
              true,
              addr(&error));
          if (error)
            fail(error);
        });
        if (!good())
          return false;
      }
      const auto *counts = deep_bounded_ptr<const uint32_t>(shape->model_counts[which]);
      auto *offsets = deep_bounded_ptr<uint64_t>(shape->offsets);
      offsets[0] = 0;
      for (uint32_t i = 0; i < pixels; ++i) {
        if (counts[i] > DEEP_BOUNDED_K)
          return fail(DEEP_BOUNDED_CAPACITY);
        offsets[i + 1] = offsets[i] + counts[i];
      }
      for (size_t c = 0; c < configs.size(); ++c) {
        if (!deep_bounded_same_family(config, configs[c].config) || deep_bounded_encoding_owner(configs, c) != c)
          continue;
        if (!encode(c, which, offsets))
          return false;
      }
    }
    for (size_t c = 0; c < configs.size(); ++c)
      pending->consumers[c] = pending->consumers[deep_bounded_encoding_owner(configs, c)];
    closed = good();
    return closed;
  }

 private:
  bool fail(const uint32_t error)
  {
    status.fetch_or(error, std::memory_order_relaxed);
    return false;
  }
  template<typename Function> void parallel(Function function)
  {
    tbb::task_arena local{int(threads)};
    local.execute([&]() {
      parallel_for(int64_t(0), int64_t(pixels), [&](const int64_t i) { function(uint32_t(i)); });
    });
  }
  bool encode(const size_t consumer_index, const uint32_t which, const uint64_t *offsets)
  {
    const auto &config = consumer.bounded_consumers()[consumer_index].config;
    auto payload = std::allocate_shared<DeepBoundedHostPayload>(
        DeepBudgetAllocator<DeepBoundedHostPayload>(host), host);
    payload->count = size_t(offsets[pixels]);
    payload->half = config.precision == DeepCanonicalPrecision::Half;
    payload->physical_rgb = Physical && !config.alpha_only;
    payload->simple_rgb = policy == 2 && !config.alpha_only;
    payload->dense_summary = payload->simple_rgb && payload->count >= pixels;
    payload->offsets.assign(offsets, offsets + pixels + 1);
    const size_t width = payload->half ? 2 : 4;
    const size_t components = payload->physical_rgb ? 4 : 1;
    const size_t rgba_offset = payload->count * 8;
    const size_t summary_offset = deep_bounded_summary_offset(
        payload->count, payload->half, payload->physical_rgb);
    payload->planes.resize(summary_offset + deep_bounded_summary_bytes(
        payload->simple_rgb, payload->dense_summary, pixels, payload->count));
    parallel([&](const uint32_t pixel) {
      const uint32_t count = uint32_t(offsets[pixel + 1] - offsets[pixel]);
      float prefix = 0;
      int first_source = -1;
      for (uint32_t i = 0; i < count; ++i) {
        const auto v = deep_bounded_load<Physical>(shape->models[which],
                                                   uint64_t(pixel) * DEEP_BOUNDED_K + i);
        if (!valid_record(v)) {
          fail(DEEP_BOUNDED_NUMERIC);
          return;
        }
        const size_t j = size_t(offsets[pixel]) + i;
        store(payload->planes, j * 4, v.z);
        store(payload->planes, (payload->count + j) * 4, v.z_back);
        const float a = encode_value(
            *payload, rgba_offset + (j * components + (components == 4 ? 3 : 0)) * width, v.alpha);
        if constexpr (Physical) {
          if (payload->physical_rgb) {
            const float inverse = 1.0f / fmaxf(1.0f - prefix, 0x1p-23f);
            for (int c = 0; c < 3; ++c)
              encode_value(*payload, rgba_offset + (j * 4 + c) * width, v.rgb[c] * inverse);
          }
        }
        if (first_source < 0 && deep_bounded_mature_eligible(v.object))
          first_source = int(i);
        prefix = fmaf(1.0f - prefix, a, prefix);
      }
      if (payload->simple_rgb && (payload->dense_summary || count)) {
        const float value = prefix >= FLT_MIN ? prefix :
                            first_source >= 0 ? -float(first_source + 1) :
                                                0;
        const size_t index = payload->dense_summary ? pixel : size_t(offsets[pixel]);
        store(payload->planes, summary_offset + index * 4, value);
      }
    });
    if (!good())
      return false;
    pending->consumers[consumer_index] = std::move(payload);
    return true;
  }
  std::unique_ptr<CPUShape<Physical>> shape;
};
}  // namespace

bool deep_cpu_admission(const DeepFrameCaptureConsumer &consumer,
                        const uint32_t pixels,
                        const uint32_t threads,
                        DeepCPUAdmission &result)
{
  return consumer.capture_demand().capture_rgb() ?
             admission<true>(consumer, pixels, threads, result) :
             admission<false>(consumer, pixels, threads, result);
}

DeepBoundedCPUWork::DeepBoundedCPUWork(Device &device, DeepFrameCaptureConsumer &consumer)
{
  if (consumer.capture_demand().capture_rgb())
    storage_ = std::make_unique<CPUStorage<true>>(device, consumer);
  else
    storage_ = std::make_unique<CPUStorage<false>>(device, consumer);
}
DeepBoundedCPUWork::~DeepBoundedCPUWork() = default;
bool DeepBoundedCPUWork::configure(const BufferParams &logical,
                                   const BufferParams &slice,
                                   const uint32_t threads,
                                   const bool holdout,
                                   const float projection)
{
  return storage_->configure(logical, slice, threads, holdout, projection);
}
bool DeepBoundedCPUWork::owns(const int x, const int y) const
{
  return storage_->owns(x, y);
}
const DeepBoundedBinding *DeepBoundedCPUWork::begin_sample(const uint32_t worker,
                                                           const int x,
                                                           const int y,
                                                           const int sample)
{
  return storage_->begin(worker, x, y, sample);
}
bool DeepBoundedCPUWork::end_sample(const uint32_t worker)
{
  return storage_->end(worker);
}
bool DeepBoundedCPUWork::prepare_tile()
{
  return storage_->prepare();
}
bool DeepBoundedCPUWork::close_tile()
{
  return storage_->close();
}
bool DeepBoundedCPUWork::set_tile_beauty(const RenderBuffers &buffers)
{
  return storage_->beauty(buffers);
}
bool DeepBoundedCPUWork::drain()
{
  return storage_->drain();
}
bool DeepBoundedCPUWork::good() const
{
  return storage_->good();
}
uint32_t DeepBoundedCPUWork::error_flags() const
{
  return storage_->status.load(std::memory_order_relaxed);
}
uint32_t DeepBoundedCPUWork::policy() const
{
  return storage_->policy;
}

CCL_NAMESPACE_END
