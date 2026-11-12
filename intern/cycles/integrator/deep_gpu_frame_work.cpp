/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "integrator/deep_gpu_frame_work.h"
#include "integrator/deep_optical_work.h"
#include "device/device.h"
#include "session/buffers.h"

#include <cstddef>
#include <cstring>
#include <limits>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

CCL_NAMESPACE_BEGIN

namespace {
constexpr size_t sizes[] = {128, 48, 112, 96, 96, 48, 64, 20, 4, 28, 4, 32, 4,
                            sizeof(DeepCompactPrimitive), sizeof(DeepCompactRayState),
                            sizeof(DeepCompactStackEntry), sizeof(DeepCompactCamera),
                            sizeof(DeepCompactRootColour), sizeof(DeepCompactColourSource)};
static_assert(std::size(sizes) == size_t(DeepGPUResidentKind::Count));
static_assert(sizeof(DeepNativeCameraRecord) == 128);
static_assert(offsetof(DeepNativeCameraRecord, x) == 24);
static_assert(offsetof(DeepNativeCameraRecord, disposition) == 36);
static_assert(offsetof(DeepNativeCameraRecord, origin) == 48);
static_assert(sizeof(DeepNativeHomogeneousRecord) == 112);
static_assert(offsetof(DeepNativeHomogeneousRecord, tau) == 48);
static_assert(offsetof(DeepNativeHomogeneousRecord, post_clamp_rgb) == 72);
static_assert(sizeof(DeepNativePointRecord) == 96);
static_assert(offsetof(DeepNativePointRecord, final_rgb) == 60);
static_assert(offsetof(DeepNativePointRecord, routing) == 28);
static_assert(sizeof(DeepNativeExpressionSource) == 96);
static_assert(offsetof(DeepNativeExpressionSource, raw_rgb) == 32);
static_assert(offsetof(DeepNativeExpressionSource, tau) == 48);
static_assert(sizeof(DeepNativeExpressionNode) == 48);
static_assert(offsetof(DeepNativeExpressionNode, weight) == 28);
static_assert(sizeof(DeepNativeExpressionCall) == 64);
static_assert(offsetof(DeepNativeExpressionCall, clamp_gain) == 40);
static_assert(offsetof(DeepNativeExpressionCall, routing) == 44);
static_assert(sizeof(DeepEncodedPayload) == sizeof(DeepGPUFrameSample));
constexpr uint32_t blocks(const uint32_t n, const uint32_t size = 256)
{
  return n / size + uint32_t(n % size != 0);
}
using Clock = std::chrono::steady_clock;
double elapsed(const Clock::time_point start)
{
  return std::chrono::duration<double>(Clock::now() - start).count();
}
bool tracing()
{
  const char *value = std::getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_CHAIN_TRACE");
  return value && std::strcmp(value, "1") == 0;
}
}  // namespace

DeepGPUResidentBuffer::DeepGPUResidentBuffer(Device *device, const DeepMemoryBudget &host_budget,
                                             const DeepMemoryBudget &device_budget)
    : device_(device), budget_(device_budget), host_budget_(host_budget)
{
}
DeepGPUResidentBuffer::~DeepGPUResidentBuffer() { clear(); }
device_ptr DeepGPUResidentBuffer::pointer() const
{
  return memory_ ? memory_->device_pointer : 0;
}
device_memory &DeepGPUResidentBuffer::memory() { return *memory_; }

bool DeepGPUResidentBuffer::reserve(DeviceQueue &queue, const size_t bytes, const bool preserve)
{
  last_queue_ = &queue;
  if (bytes <= capacity_)
    return true;
  /* Charge the actual replacement peak, including the old allocation until its
   * final D2D copy has joined. There is no mapped-memory or CPU spill escape. */
  const size_t growth = capacity_ <= SIZE_MAX / 2 ? capacity_ * 2 : bytes;
  size_t next = std::max(bytes, growth);
  /* Geometric spare capacity is optional; the required replacement peak is
   * not. A legal append must not fail just because doubling exceeds quota. */
  const size_t available = budget_.quota() - std::min(budget_.used(), budget_.quota());
  if (next > available && bytes <= available) next = bytes;
  try { budget_.acquire(next); }
  catch (const std::bad_alloc &) {
    std::fprintf(stderr, "DEEP_GPU_ALLOCATION_DENIED requested=%llu retained=%llu used=%llu quota=%llu\n",
                 static_cast<unsigned long long>(next), static_cast<unsigned long long>(capacity_),
                 static_cast<unsigned long long>(budget_.used()),
                 static_cast<unsigned long long>(budget_.quota()));
    device_->set_error("Deep GPU resident replacement exceeds the authored device quota");
    return false;
  }
  unique_ptr<device_only_memory<uint8_t>> replacement;
  try {
    host_budget_.acquire(sizeof(device_only_memory<uint8_t>));
  }
  catch (const std::bad_alloc &) {
    budget_.relinquish(next);
    device_->set_error("Deep GPU resident wrapper exceeds host quota");
    return false;
  }
  try {
    replacement = make_unique<device_only_memory<uint8_t>>(device_, "deep_gpu_resident");
    replacement->alloc_to_device(next);
  }
  catch (const std::bad_alloc &) {
    replacement.reset(); host_budget_.relinquish(sizeof(device_only_memory<uint8_t>));
    budget_.relinquish(next);
    device_->set_error("Deep GPU resident allocation failed");
    return false;
  }
  if (device_->have_error() || !replacement->device_pointer || replacement->shared_pointer) {
    replacement.reset(); host_budget_.relinquish(sizeof(device_only_memory<uint8_t>));
    budget_.relinquish(next); return false;
  }
  if (preserve && used_ &&
      (!queue.copy_device_region(*replacement, *memory_, 0, 0, used_) || !queue.synchronize()))
  {
    queue.synchronize_for_state_rebind(); replacement.reset();
    host_budget_.relinquish(sizeof(device_only_memory<uint8_t>));
    budget_.relinquish(next); return false;
  }
  memory_.swap(replacement);
  const bool had_old = bool(replacement);
  replacement.reset();
  if (had_old) host_budget_.relinquish(sizeof(device_only_memory<uint8_t>));
  budget_.relinquish(capacity_);
  capacity_ = next;
  if (!preserve) used_ = 0;
  return true;
}
bool DeepGPUResidentBuffer::append(DeviceQueue &queue, device_memory &source,
                                   const size_t bytes, const size_t source_offset)
{
  if (bytes > SIZE_MAX - used_ || !reserve(queue, used_ + bytes))
    return false;
  if (bytes && !queue.copy_device_region(*memory_, source, used_, source_offset, bytes))
    return false;
  used_ += bytes;
  return true;
}
bool DeepGPUResidentBuffer::zero(DeviceQueue &queue, const size_t bytes)
{
  if (!reserve(queue, bytes, false)) return false;
  if (bytes) queue.zero_to_device(*memory_);
  used_ = bytes;
  return !device_->have_error();
}
bool DeepGPUResidentBuffer::append_raw(DeviceQueue &queue, const void *source, const size_t bytes)
{
  if (bytes > SIZE_MAX - used_ || !reserve(queue, used_ + bytes)) return false;
  if (bytes && !queue.write_device_region(*memory_, used_, source, bytes)) return false;
  used_ += bytes;
  return true;
}
void DeepGPUResidentBuffer::clear()
{
  const bool allocated = bool(memory_);
  if (allocated && last_queue_) last_queue_->synchronize_for_state_rebind();
  memory_.reset();
  if (allocated) host_budget_.relinquish(sizeof(device_only_memory<uint8_t>));
  budget_.relinquish(capacity_);
  used_ = capacity_ = 0;
}

DeepGPUFrameWork::DeepGPUFrameWork(Device *device, DeviceQueue *queue,
                                   const DeepMemoryBudget &host_budget,
                                   const DeepMemoryBudget &device_budget)
    : device_(device), queue_(queue), host_budget_(host_budget), device_budget_(device_budget),
      primitive_spill_(DeepBudgetAllocator<std::shared_ptr<RawBytes>>(host_budget)),
      archives_(DeepBudgetAllocator<std::shared_ptr<ArchivedDomain>>(host_budget)),
      trace_probes_(DeepBudgetAllocator<uint64_t>(host_budget)),
      witness_cuts_(DeepBudgetAllocator<double>(host_budget)),
      pending_simple_(DeepBudgetAllocator<std::shared_ptr<PendingSimpleTile>>(host_budget)),
      preparing_summaries_(DeepBudgetAllocator<RawBytesPtr>(host_budget)),
      controls_(device, "deep_gpu_control", MEM_READ_WRITE),
      native_parameters_(device, "deep_gpu_native_parameters", MEM_READ_WRITE),
      reduce_parameters_(device, "deep_gpu_reduce_parameters", MEM_READ_WRITE),
      grid_parameters_(device, "deep_gpu_grid_parameters", MEM_READ_WRITE),
      encode_parameters_(device, "deep_gpu_encode_parameters", MEM_READ_WRITE),
      consumer_parameters_(device, "deep_gpu_consumer_parameters", MEM_READ_WRITE),
      witness_parameters_(device, "deep_gpu_witness_parameters", MEM_READ_WRITE),
      trace_parameters_(device, "deep_gpu_trace_parameters", MEM_READ_WRITE)
{
  for (auto &buffer : native_) buffer = make_buffer();
  for (auto &buffer : tile_) buffer = make_buffer();
  progress_reserve_ = make_buffer();
  witness_cuts_device_ = make_buffer(); witness_log_ = make_buffer(); witness_rgb_ = make_buffer();
  witness_rays_ = make_buffer();
  trace_records_ = make_buffer(); trace_counter_ = make_buffer(); trace_probes_device_ = make_buffer();
  trace_raw_ = make_buffer(); trace_raw_counter_ = make_buffer();
  trace_colour_ = make_buffer(); trace_colour_counter_ = make_buffer();
  capture_case_ = make_buffer();
  host_budget_.acquire(16);
  try { device_budget_.acquire(16); }
  catch (...) { host_budget_.relinquish(16); throw; }
  try { controls_.alloc(4); controls_.zero_to_device(); }
  catch (...) { host_budget_.relinquish(16); device_budget_.relinquish(16); throw; }
}
DeepGPUFrameWork::~DeepGPUFrameWork()
{
  if (queue_) queue_->synchronize_for_state_rebind();
  const std::array<device_vector<uint8_t> *, 7> metadata = {
      &native_parameters_, &reduce_parameters_, &grid_parameters_, &encode_parameters_,
      &consumer_parameters_, &witness_parameters_, &trace_parameters_};
  for (size_t i = 0; i < metadata.size(); i++) {
    metadata[i]->free();
    host_budget_.relinquish(metadata_charges_[i]);
    device_budget_.relinquish(metadata_charges_[i]);
  }
  controls_.free(); host_budget_.relinquish(16); device_budget_.relinquish(16);
}
DeepGPUFrameWork::Buffer DeepGPUFrameWork::make_buffer()
{
  return std::allocate_shared<DeepGPUResidentBuffer>(DeepBudgetAllocator<DeepGPUResidentBuffer>(host_budget_),
                                                    device_, host_budget_, device_budget_);
}
bool DeepGPUFrameWork::fail(const char *message)
{
  if (!device_->have_error()) device_->set_error(message);
  return false;
}
bool DeepGPUFrameWork::operation(const DeepGPUFrameOperation op,
                                  const DeepGPUFrameDispatch &p,
                                  const uint32_t count, const uint32_t threads)
{
  if (cancel_poll_ && cancel_poll_()) return false;
  last_operation_ = op;
  return queue_->enqueue_deep_frame_operation(op, p, count, threads);
}
bool DeepGPUFrameWork::good()
{
  queue_->copy_from_device(controls_);
  if (!queue_->synchronize() || device_->have_error()) return false;
  if (controls_.data()[0]) {
    char message[192];
    const uint32_t bits = controls_.data()[0];
    const char *reason = bits & DEEP_GPU_FRAME_FORMAT_RANGE ? "FormatRange" :
                         bits & DEEP_GPU_FRAME_NUMERIC_MASS_RANGE ? "NumericMassRange" :
                         bits & DEEP_GPU_FRAME_SIMPLE_ZERO_ALPHA_COLOUR ? "SimpleZeroAlphaColourUnsupported" :
                         bits & DEEP_GPU_FRAME_CAPACITY ? "BudgetOrCountExceeded" : "InvalidCompactInput";
    std::snprintf(message, sizeof(message),
                  "Deep GPU %s: flags=0x%08x last_operation=%u detail=%u",
                  reason, bits, uint32_t(last_operation_), controls_.data()[3]);
    return fail(message);
  }
  return true;
}
bool DeepGPUFrameWork::upload_metadata(const void *source, const size_t bytes,
                                       device_vector<uint8_t> &destination)
{
  const std::array<device_vector<uint8_t> *, 7> metadata = {
      &native_parameters_, &reduce_parameters_, &grid_parameters_, &encode_parameters_,
      &consumer_parameters_, &witness_parameters_, &trace_parameters_};
  size_t index = 0;
  while (index < metadata.size() && metadata[index] != &destination) index++;
  if (index == metadata.size()) return fail("Deep GPU unknown metadata owner");
  if (metadata_charges_[index] != bytes) {
    host_budget_.acquire(bytes);
    try { device_budget_.acquire(bytes); }
    catch (...) { host_budget_.relinquish(bytes); throw; }
    try { destination.alloc(bytes); }
    catch (...) {
      host_budget_.relinquish(bytes); device_budget_.relinquish(bytes); throw;
    }
    host_budget_.relinquish(metadata_charges_[index]);
    device_budget_.relinquish(metadata_charges_[index]);
    metadata_charges_[index] = bytes;
  }
  std::memcpy(destination.data(), source, bytes);
  queue_->copy_to_device(destination);
  return !device_->have_error();
}
bool DeepGPUFrameWork::begin(const DeepNativeCaptureDomain &domain)
{
  if (active_ || !queue_->supports_deep_frame_reduction() || device_->info.type != DEVICE_OPTIX)
    return fail("Deep GPU resident capture requires one joined OptiX domain");
  if ((tile_cameras_ || !archives_.empty()) && (domain.demand.simple_mode != domain_.demand.simple_mode ||
                       domain.demand.needs_rgb != domain_.demand.needs_rgb))
    return fail("Deep mode or colour demand changed inside a frozen tile");
  for (auto &buffer : native_) buffer->set_used(0);
  /* An actual independently charged allocation keeps producer admission from
   * consuming the final continuation/merge transaction. After producers join,
   * only the finishing phase may release and use these bytes. */
  const size_t reserve = std::max(size_t(64 << 10), std::min(size_t(8 << 20), device_budget_.quota() / 16));
  if (!progress_reserve_->reserve(*queue_, reserve, false)) return false;
  domain_ = domain;
  active_ = true;
  return begin_witness();
}
uint32_t DeepGPUFrameWork::native_count(const DeepGPUResidentKind kind) const
{
  return uint32_t(native_[size_t(kind)]->used() / sizes[size_t(kind)]);
}
device_ptr DeepGPUFrameWork::native_pointer(const DeepGPUResidentKind kind) const
{
  return native_[size_t(kind)]->pointer();
}

bool DeepGPUFrameWork::reserve_native_root_colours(const uint32_t count)
{
  auto &buffer = *native_[size_t(DeepGPUResidentKind::RootColour)];
  const uint32_t old = native_count(DeepGPUResidentKind::RootColour);
  if (count <= old) return true;
  if (!buffer.reserve(*queue_, size_t(count) * sizeof(DeepCompactRootColour))) return false;
  DeepGPUFrameDispatch p;
  p.a = buffer.pointer(); p.n = count - old; p.base = old;
  if (!operation(DeepGPUFrameOperation::CompactColourInit, p, blocks(p.n))) return false;
  buffer.set_used(size_t(count) * sizeof(DeepCompactRootColour));
  return true;
}
bool DeepGPUFrameWork::append_native(const DeepGPUResidentKind kind, device_memory &source,
                                     const uint32_t records)
{
  if (!active_ || uint64_t(native_count(kind)) + records > UINT32_MAX)
    return fail("Deep GPU native capture count overflow or missing domain");
  auto &target = *native_[size_t(kind)];
  if (kind == DeepGPUResidentKind::Primitive && records) {
    const size_t page_records = std::max(size_t(1), std::min(size_t(64 << 20),
        device_budget_.quota() / 8) / sizeof(DeepCompactPrimitive));
    for (uint32_t at = 0; at < records;) {
      if (target.used() / sizeof(DeepCompactPrimitive) >= page_records && !spill_native_primitives())
        return false;
      const uint32_t n = uint32_t(std::min(size_t(records - at),
          page_records - target.used() / sizeof(DeepCompactPrimitive)));
      const size_t offset = target.used();
      if (!target.append(*queue_, source, size_t(n) * sizeof(DeepCompactPrimitive),
                           size_t(at) * sizeof(DeepCompactPrimitive))) return false;
      DeepGPUFrameDispatch p;
      p.a = target.pointer() + offset; p.b = controls_.device_pointer;
      p.n = n; p.m = uint32_t(domain_.scheduled_admissions); p.base = tile_cameras_;
      if (!operation(DeepGPUFrameOperation::CompactRebase, p, blocks(n))) return false;
      at += n;
    }
    return true;
  }
  const size_t offset = target.used();
  if (!target.append(*queue_, source, size_t(records) * sizes[size_t(kind)])) return false;
  if (kind == DeepGPUResidentKind::Primitive && records) {
    DeepGPUFrameDispatch p;
    p.a = target.pointer() + offset; p.b = controls_.device_pointer;
    p.n = records; p.m = uint32_t(domain_.scheduled_admissions); p.base = tile_cameras_;
    return operation(DeepGPUFrameOperation::CompactRebase, p, blocks(records));
  }
  return true;
}

bool DeepGPUFrameWork::scan(DeepGPUResidentBuffer &counts, const uint32_t size,
                             DeepGPUResidentBuffer &offsets)
{
  if (!size) return offsets.zero(*queue_, 0);
  DeepNativeCaptureVector<Buffer> levels{DeepBudgetAllocator<Buffer>(host_budget_)},
                                  totals{DeepBudgetAllocator<Buffer>(host_budget_)};
  DeepNativeCaptureVector<uint32_t> sizes_{DeepBudgetAllocator<uint32_t>(host_budget_)};
  device_ptr input = counts.pointer();
  uint32_t n = size;
  for (;;) {
    Buffer output = levels.empty() ?
                        Buffer(&offsets, [](auto *) {},
                               DeepBudgetAllocator<DeepGPUResidentBuffer>(host_budget_)) : make_buffer();
    auto sum = make_buffer();
    if (!output->reserve(*queue_, size_t(n) * 4, false) ||
        !sum->reserve(*queue_, size_t(blocks(n)) * 4, false)) return false;
    DeepGPUFrameDispatch p;
    p.a = input; p.b = output->pointer(); p.c = sum->pointer(); p.n = n;
    if (!operation(DeepGPUFrameOperation::Scan, p, blocks(n))) return false;
    levels.push_back(output); totals.push_back(sum); sizes_.push_back(n);
    if (blocks(n) == 1) break;
    input = sum->pointer(); n = blocks(n);
  }
  for (size_t i = levels.size() - 1; i-- > 0;) {
    DeepGPUFrameDispatch p;
    p.a = levels[i]->pointer(); p.b = levels[i + 1]->pointer(); p.n = sizes_[i];
    if (!operation(DeepGPUFrameOperation::ScanAdd, p, blocks(p.n))) return false;
  }
  /* Local level owners cannot retire before their queued readers finish. */
  return queue_->synchronize();
}

uint32_t DeepGPUFrameWork::total(DeepGPUResidentBuffer &counts, DeepGPUResidentBuffer &offsets,
                                 const uint32_t size)
{
  if (!size) return 0;
  const size_t position = size_t(size - 1) * 4;
  if (!queue_->copy_device_region(controls_, offsets.memory(), 4, position, 4) ||
      !queue_->copy_device_region(controls_, counts.memory(), 8, position, 4) || !good())
    return 0;
  const uint64_t sum = uint64_t(controls_.data()[1]) + controls_.data()[2];
  if (sum > UINT32_MAX) { fail("Deep GPU device prefix count overflow"); return 0; }
  return uint32_t(sum);
}

bool DeepGPUFrameWork::sort(DeepGPUResidentBuffer &keys, DeepGPUResidentBuffer &ids,
                             const uint32_t count)
{
  if (!count) return true;
  size_t total = 0, free = 0;
  device_->get_device_memory_info(total, free);
  const size_t remaining = device_budget_.quota() - device_budget_.used();
  const size_t available = total ? std::min(remaining, free) : remaining;
  if (size_t(count) > available / 12) return sort_spilled(keys, ids, count);
  auto alternate_keys = make_buffer(), alternate_ids = make_buffer();
  if (!alternate_keys->reserve(*queue_, size_t(count) * 8, false) ||
      !alternate_ids->reserve(*queue_, size_t(count) * 4, false)) return false;
  DeepGPUFrameDispatch p;
  p.a = keys.pointer(); p.b = ids.pointer(); p.c = alternate_keys->pointer();
  p.d = alternate_ids->pointer(); p.n = count;
  if (!operation(DeepGPUFrameOperation::SortBlocks, p, blocks(count, 1024))) return false;
  bool alternate = true;
  for (uint64_t width = 1024; width < count; width *= 2) {
    p.a = alternate ? alternate_keys->pointer() : keys.pointer();
    p.b = alternate ? alternate_ids->pointer() : ids.pointer();
    p.c = alternate ? keys.pointer() : alternate_keys->pointer();
    p.d = alternate ? ids.pointer() : alternate_ids->pointer();
    p.width = uint32_t(width);
    if (!operation(DeepGPUFrameOperation::Merge, p, blocks(count))) return false;
    alternate = !alternate;
  }
  if (alternate &&
      (!queue_->copy_device_region(keys.memory(), alternate_keys->memory(), 0, 0, size_t(count) * 8) ||
       !queue_->copy_device_region(ids.memory(), alternate_ids->memory(), 0, 0, size_t(count) * 4)))
    return false;
  return queue_->synchronize();
}

bool DeepGPUFrameWork::sort_spilled(DeepGPUResidentBuffer &keys, DeepGPUResidentBuffer &ids,
                                     const uint32_t count)
{
  /* Host runs are opaque bytes. Their only host arithmetic is counts, offsets
   * and ownership; key comparisons, stable partitions and merges stay on GPU. */
  struct Run {
    explicit Run(const DeepMemoryBudget &budget) : bytes(DeepBudgetAllocator<uint8_t>(budget)) {}
    DeepNativeCaptureVector<uint8_t> bytes;
  };
  using RunPtr = std::shared_ptr<Run>;
  DeepNativeCaptureVector<RunPtr> runs{DeepBudgetAllocator<RunPtr>(host_budget_)};
  size_t total = 0, free = 0;
  device_->get_device_memory_info(total, free);
  const size_t remaining = device_budget_.quota() - device_budget_.used();
  const size_t available = total ? std::min(remaining, free) : remaining;
  const uint32_t capacity = uint32_t(std::min(size_t(65536), available / 96));
  if (!capacity) return fail("Deep sort cannot reserve its minimum GPU merge transaction");
  auto a = make_buffer(), b = make_buffer(), merged = make_buffer();
  auto chunk_keys = make_buffer(), chunk_ids = make_buffer(), receipt = make_buffer();
  const size_t page_bytes = size_t(capacity) * sizeof(DeepCompactSortItem);
  if (!a->reserve(*queue_, page_bytes, false) || !b->reserve(*queue_, page_bytes, false) ||
      !merged->reserve(*queue_, page_bytes, false) || !receipt->zero(*queue_, 8) ||
      !chunk_keys->reserve(*queue_, size_t(capacity) * 8, false) ||
      !chunk_ids->reserve(*queue_, size_t(capacity) * 4, false)) return false;
  for (uint64_t start = 0; start < count; start += capacity) {
    const uint32_t n = uint32_t(std::min(uint64_t(capacity), uint64_t(count) - start));
    if (!queue_->copy_device_region(chunk_keys->memory(), keys.memory(), 0, size_t(start) * 8, size_t(n) * 8) ||
        !queue_->copy_device_region(chunk_ids->memory(), ids.memory(), 0, size_t(start) * 4, size_t(n) * 4) ||
        !sort(*chunk_keys, *chunk_ids, n)) return false;
    auto run = std::allocate_shared<Run>(DeepBudgetAllocator<Run>(host_budget_), host_budget_);
    run->bytes.resize(size_t(n) * sizeof(DeepCompactSortItem));
    DeepGPUFrameDispatch d;
    d.a = chunk_keys->pointer(); d.b = chunk_ids->pointer(); d.c = merged->pointer(); d.n = n;
    if (!operation(DeepGPUFrameOperation::CompactSortPack, d, blocks(n)) ||
        !queue_->read_device_region(merged->memory(), 0, run->bytes.data(), run->bytes.size())) return false;
    runs.push_back(std::move(run));
  }
  chunk_keys.reset(); chunk_ids.reset();
  while (runs.size() > 1) {
    DeepNativeCaptureVector<RunPtr> next{DeepBudgetAllocator<RunPtr>(host_budget_)};
    for (size_t pair = 0; pair < runs.size(); pair += 2) {
      if (pair + 1 == runs.size()) { next.push_back(std::move(runs[pair])); continue; }
      auto &left = runs[pair], &right = runs[pair + 1];
      auto output = std::allocate_shared<Run>(DeepBudgetAllocator<Run>(host_budget_), host_budget_);
      if (left->bytes.size() > SIZE_MAX - right->bytes.size()) return fail("Deep spill run byte count overflow");
      output->bytes.resize(left->bytes.size() + right->bytes.size());
      size_t left_at = 0, right_at = 0, output_at = 0;
      while (output_at < output->bytes.size()) {
        const uint32_t na = uint32_t(std::min(page_bytes, left->bytes.size() - left_at) / sizeof(DeepCompactSortItem));
        const uint32_t nb = uint32_t(std::min(page_bytes, right->bytes.size() - right_at) / sizeof(DeepCompactSortItem));
        const uint32_t n = uint32_t(std::min(uint64_t(capacity), uint64_t(na) + nb));
        if (!n ||
            (na && !queue_->write_device_region(a->memory(), 0, left->bytes.data() + left_at,
                                                 size_t(na) * sizeof(DeepCompactSortItem))) ||
            (nb && !queue_->write_device_region(b->memory(), 0, right->bytes.data() + right_at,
                                                 size_t(nb) * sizeof(DeepCompactSortItem)))) return false;
        DeepGPUFrameDispatch d;
        d.a = a->pointer(); d.b = b->pointer(); d.c = merged->pointer(); d.d = receipt->pointer();
        d.n = na; d.m = nb; d.k = n;
        uint32_t consumed[2] = {};
        if (!operation(DeepGPUFrameOperation::CompactSortMerge, d, blocks(n + 1)) ||
            !queue_->read_device_region(receipt->memory(), 0, consumed, sizeof(consumed)) ||
            consumed[0] > na || consumed[1] > nb || uint64_t(consumed[0]) + consumed[1] != n ||
            !queue_->read_device_region(merged->memory(), 0, output->bytes.data() + output_at,
                                         size_t(n) * sizeof(DeepCompactSortItem)))
          return fail("Deep GPU spilled merge returned an invalid progress receipt");
        left_at += size_t(consumed[0]) * sizeof(DeepCompactSortItem);
        right_at += size_t(consumed[1]) * sizeof(DeepCompactSortItem);
        output_at += size_t(n) * sizeof(DeepCompactSortItem);
      }
      left.reset(); right.reset(); next.push_back(std::move(output));
    }
    runs.swap(next);
  }
  if (runs.empty()) return fail("Deep GPU spilled sort has no final run");
  for (uint64_t start = 0; start < count; start += capacity) {
    const uint32_t n = uint32_t(std::min(uint64_t(capacity), uint64_t(count) - start));
    if (!queue_->write_device_region(a->memory(), 0,
          runs[0]->bytes.data() + size_t(start) * sizeof(DeepCompactSortItem), size_t(n) * sizeof(DeepCompactSortItem)))
      return false;
    DeepGPUFrameDispatch d;
    d.a = a->pointer(); d.b = keys.pointer(); d.c = ids.pointer(); d.n = n; d.base = uint32_t(start);
    if (!operation(DeepGPUFrameOperation::CompactSortUnpack, d, blocks(n))) return false;
  }
  return queue_->synchronize() && good();
}
bool DeepGPUFrameWork::group(DeepGPUResidentBuffer &keys, const uint32_t count,
                              const uint32_t owners, DeepGPUResidentBuffer &ranges)
{
  if (!ranges.zero(*queue_, size_t(owners) * sizeof(DeepGPUFrameRange))) return false;
  DeepGPUFrameDispatch p;
  p.a = keys.pointer(); p.b = ranges.pointer(); p.c = controls_.device_pointer;
  p.n = count; p.m = owners;
  return operation(DeepGPUFrameOperation::Ranges, p, blocks(count));
}

bool DeepGPUFrameWork::resolve_native_radiance()
{
  using K = DeepGPUResidentKind;
  const uint32_t roots = native_count(K::Root), spans = native_count(K::Homogeneous);
  if (uint64_t(roots) + spans > UINT32_MAX)
    return fail("Deep direct colour source count overflow");
  const uint32_t count = roots + spans;
  auto &output = *native_[size_t(K::ColourSource)];
  if (!output.reserve(*queue_, size_t(count) * sizeof(DeepCompactColourSource), false)) return false;
  DeepGPUFrameDispatch p;
  p.a = native_pointer(K::Root); p.b = native_pointer(K::RootColour);
  p.c = native_pointer(K::Homogeneous); p.d = native_pointer(K::Camera);
  p.e = output.pointer(); p.f = controls_.device_pointer;
  p.n = count; p.m = roots; p.k = spans; p.width = native_count(K::Camera);
  p.base = tile_cameras_;
  p.p = domain_.depth_transform[3]; p.q = domain_.depth_transform[7]; p.r = domain_.depth_transform[11];
  if (!operation(DeepGPUFrameOperation::CompactRootRadiance, p, blocks(count))) return false;
  output.set_used(size_t(count) * sizeof(DeepCompactColourSource));
  return true;
}

bool DeepGPUFrameWork::append_optical(device_memory &pieces, const uint32_t count,
                                      device_memory &visibility, const uint32_t visibility_count,
                                      const uint32_t camera_offset, const uint32_t chunk_cameras)
{
  using K = DeepGPUResidentKind;
  const auto append = [&](K kind, K owner, device_memory &source, uint32_t n,
                           DeepGPUFrameOperation op) {
    if (!n) return true;
    auto &target = *native_[size_t(kind)], &owners = *native_[size_t(owner)];
    const size_t old = target.used(), owner_old = owners.used();
    if (size_t(n) > (SIZE_MAX - old) / sizes[size_t(kind)] ||
        !target.reserve(*queue_, old + size_t(n) * sizes[size_t(kind)]) ||
        !owners.reserve(*queue_, owner_old + size_t(n) * 4)) return false;
    DeepGPUFrameDispatch p;
    p.a = source.device_pointer; p.b = target.pointer() + old;
    p.c = owners.pointer() + owner_old; p.d = controls_.device_pointer;
    p.n = n; p.m = chunk_cameras; p.base = tile_cameras_ + camera_offset;
    if (!operation(op, p, blocks(n))) return false;
    target.set_used(old + size_t(n) * sizes[size_t(kind)]);
    owners.set_used(owner_old + size_t(n) * 4);
    return true;
  };
  return append(K::Optical, K::OpticalOwner, pieces, count, DeepGPUFrameOperation::OwnedOptical) &&
         append(K::Visibility, K::VisibilityOwner, visibility, visibility_count,
                  DeepGPUFrameOperation::OwnedVisibility);
}

bool DeepGPUFrameWork::finish_native(DeviceScene *scene, const IntegratorStateGPU &beauty,
                                     const bool *cancel, RenderBuffers *buffers,
                                     const std::function<bool()> &cancel_poll)
{
  const auto start = Clock::now();
  if (!active_) return fail("Deep GPU native domain was not admitted");
  cancel_poll_ = cancel_poll;
  struct CancelScope {
    std::function<bool()> &callback;
    ~CancelScope() { callback = {}; }
  } cancel_scope{cancel_poll_};
  if (!native_count(DeepGPUResidentKind::Camera)) {
    for (size_t i = 1; i <= size_t(DeepGPUResidentKind::Call); i++)
      if (native_[i]->used()) return fail("Deep GPU sources have no admitted camera owner");
    active_ = false; return true;
  }
  if (uint64_t(tile_cameras_) + native_count(DeepGPUResidentKind::Camera) > UINT32_MAX)
    return fail("Deep GPU tile camera identity overflow");
  progress_reserve_->clear();
  if (witness_.cut_count) {
    witness_.cameras = native_pointer(DeepGPUResidentKind::Camera);
    witness_.camera_base = tile_cameras_;
    witness_.camera_count = native_count(DeepGPUResidentKind::Camera);
    if (!upload_metadata(&witness_, sizeof(witness_), witness_parameters_)) return false;
  }
  if (domain_.demand.capture_rgb() && !resolve_native_radiance()) return false;
  const double native_seconds = elapsed(start);
  const auto optical_start = Clock::now();
  uint32_t missing_cameras = 0;
  if (domain_.has_volume) {
    auto needed = make_buffer();
    if (!needed->zero(*queue_, 4)) return false;
    DeepGPUFrameDispatch p;
    p.a = native_pointer(DeepGPUResidentKind::ScalarState); p.b = needed->pointer();
    p.c = native_pointer(DeepGPUResidentKind::Camera);
    p.n = native_count(DeepGPUResidentKind::Camera);
    p.m = uint32_t(domain_.core_width ? domain_.core_x : domain_.x);
    p.k = uint32_t(domain_.core_height ? domain_.core_y : domain_.y);
    p.width = uint32_t(domain_.core_width ? domain_.core_width : domain_.width);
    p.pixel = uint32_t(domain_.core_height ? domain_.core_height : domain_.height);
    if (!operation(DeepGPUFrameOperation::CompactNeedsExtinction, p, blocks(p.n)) ||
        !queue_->read_device_region(needed->memory(), 0, &missing_cameras, sizeof(missing_cameras))) return false;
  }
  {
    DeepOpticalWork optical(device_, queue_, host_budget_, device_budget_);
    if ((missing_cameras && !optical.replay_device(scene, beauty, *this, cancel, buffers, cancel_poll)) || !good())
      return false;
  }
  if (!finish_witness_native()) return false;
  using K = DeepGPUResidentKind;
  const uint32_t camera_count = native_count(K::Camera);
  auto &camera_metadata = *native_[size_t(K::CompactCamera)];
  if (!camera_metadata.reserve(*queue_, size_t(camera_count) * sizeof(DeepCompactCamera), false))
    return false;
  DeepGPUFrameDispatch pack;
  pack.a = native_pointer(K::Camera); pack.b = native_pointer(K::ScalarState);
  pack.c = camera_metadata.pointer(); pack.d = controls_.device_pointer; pack.n = camera_count;
  pack.m = uint32_t(domain_.core_width ? domain_.core_x : domain_.x);
  pack.k = uint32_t(domain_.core_height ? domain_.core_y : domain_.y);
  pack.width = uint32_t(domain_.core_width ? domain_.core_width : domain_.width);
  pack.pixel = uint32_t(domain_.core_height ? domain_.core_height : domain_.height);
  if (!operation(DeepGPUFrameOperation::CompactCameraPack, pack, blocks(camera_count))) return false;
  camera_metadata.set_used(size_t(camera_count) * sizeof(DeepCompactCamera));
  if (!good()) return false;
  for (size_t i = 0; i < native_.size(); i++)
    if (i != size_t(K::CompactCamera) && i != size_t(K::ColourSource) && i != size_t(K::Primitive))
      native_[i]->clear();
  const uint64_t cameras = uint64_t(tile_cameras_) + camera_count;
  const uint64_t primitives = tile_[size_t(K::Primitive)]->used() / sizeof(DeepCompactPrimitive) +
                              native_[size_t(K::Primitive)]->used() / sizeof(DeepCompactPrimitive);
  const uint64_t colours = tile_[size_t(K::ColourSource)]->used() / sizeof(DeepCompactColourSource) +
                           native_[size_t(K::ColourSource)]->used() / sizeof(DeepCompactColourSource);
  size_t memory_total = 0, memory_free = 0;
  device_->get_device_memory_info(memory_total, memory_free);
  const size_t input_bytes = size_t(cameras) * sizeof(DeepCompactCamera) +
      size_t(primitives) * sizeof(DeepCompactPrimitive) + size_t(colours) * sizeof(DeepCompactColourSource);
  const size_t remaining = device_budget_.quota() - device_budget_.used();
  const size_t working_quota = remaining > SIZE_MAX - input_bytes ? SIZE_MAX : remaining + input_bytes;
  const size_t working_free = memory_free > SIZE_MAX - input_bytes ? SIZE_MAX : memory_free + input_bytes;
  const size_t available = memory_total ? std::min(working_quota, working_free) : working_quota;
  const uint64_t scale = domain_.demand.capture_rgb() ? 4 : 1;
  const uint64_t estimate = cameras * 256 * scale + primitives * 640 * scale + colours * 2048;
  if (archives_.empty() && primitive_spill_.empty() && cameras <= UINT32_MAX && estimate <= available / 2) {
    for (const K kind : {K::CompactCamera, K::Primitive, K::ColourSource}) {
      auto &source = *native_[size_t(kind)];
      if (source.used() && !tile_[size_t(kind)]->append(*queue_, source.memory(), source.used())) return false;
    }
    tile_cameras_ = uint32_t(cameras);
  }
  else {
    /* Convert the committed resident prefix to the same raw-byte archive before
     * admitting later work. Preserve original camera IDs by concatenating the
     * older camera prefix first; source order is restored by device sorting. */
    if (tile_cameras_) {
      std::swap(native_[size_t(K::CompactCamera)], tile_[size_t(K::CompactCamera)]);
      auto &current = *tile_[size_t(K::CompactCamera)];
      if (current.used() && !native_[size_t(K::CompactCamera)]->append(*queue_, current.memory(), current.used()))
        return false;
      current.clear();
      auto &old_primitives = *tile_[size_t(K::Primitive)];
      if (old_primitives.used()) {
        auto raw = std::allocate_shared<RawBytes>(DeepBudgetAllocator<RawBytes>(host_budget_), host_budget_);
        raw->bytes.resize(old_primitives.used());
        if (!queue_->read_device_region(old_primitives.memory(), 0, raw->bytes.data(), raw->bytes.size())) return false;
        primitive_spill_.push_back(std::move(raw)); old_primitives.clear();
      }
      auto &old_colours = *tile_[size_t(K::ColourSource)];
      if (old_colours.used() && !native_[size_t(K::ColourSource)]->append(
              *queue_, old_colours.memory(), old_colours.used())) return false;
      old_colours.clear();
    }
    if (!archive_native()) return false;
    tile_cameras_ = 0;
  }
  if (!queue_->synchronize()) return false;
  if (tracing())
    std::fprintf(stderr,
                 "DEEP_GPU_FRAME_NATIVE domain=%llu cameras=%u native_resolve_wall=%.9f "
                 "optical_append_wall=%.9f total_wall=%.9f native_host_packet=0\n",
                 static_cast<unsigned long long>(domain_.id), camera_count,
                 native_seconds, elapsed(optical_start), elapsed(start));
  for (auto &buffer : native_) buffer->clear();
  active_ = false;
  return true;
}

bool DeepGPUFrameWork::build_compact_base(RenderBuffers &buffers, CompactTile &t)
{
  using K = DeepGPUResidentKind;
  const auto &film = buffers.params;
  if (trace_.probe_count) {
    trace_.x = film.full_x + film.window_x; trace_.y = film.full_y + film.window_y;
    trace_.width = film.window_width; trace_.height = film.window_height;
    if (!upload_metadata(&trace_, sizeof(trace_), trace_parameters_)) return false;
  }
  const uint64_t pixels = uint64_t(film.window_width) * film.window_height;
  const size_t primitive_count = tile_[size_t(K::Primitive)]->used() / sizeof(DeepCompactPrimitive);
  const int population_offset = film.get_pass_offset(PASS_SAMPLE_COUNT);
  if (active_ || !tile_cameras_ || pixels > UINT32_MAX || film.window_width <= 0 ||
      film.window_height <= 0 || population_offset == PASS_UNUSED || film.stride <= 0 ||
      film.pass_stride <= 0 || film.window_x < 0 || film.window_y < 0 ||
      film.window_x + film.window_width > film.width || film.window_y + film.window_height > film.height ||
      uint64_t(film.stride) * film.height * film.pass_stride > buffers.buffer.size() ||
      primitive_count > UINT32_MAX || 2ull * primitive_count + tile_cameras_ > UINT32_MAX)
    return fail("Deep compact base has an invalid frozen tile, population or event range");
  t.pixel_count = uint32_t(pixels);
  t.camera_count = tile_cameras_;
  t.primitive_count = uint32_t(primitive_count);
  t.event_count = uint32_t(2ull * primitive_count + tile_cameras_);
  if (tracing())
    std::fprintf(stderr, "DEEP_GPU_BASE_BEGIN pixels=%u cameras=%u primitives=%u events=%u\n",
                 t.pixel_count, t.camera_count, t.primitive_count, t.event_count);
  t.camera_keys = make_buffer(); t.camera_ids = make_buffer(); t.camera_map = make_buffer();
  t.camera_ranges = make_buffer(); t.pixels = make_buffer();
  t.primitive_keys = make_buffer(); t.primitive_ids = make_buffer();
  t.events = make_buffer(); t.event_keys = make_buffer(); t.event_ids = make_buffer();
  t.event_ranges = make_buffer(); t.areas = make_buffer(); t.knots = make_buffer(); t.tail = make_buffer();
  t.projection = make_buffer();
  if (!t.camera_keys->reserve(*queue_, size_t(t.camera_count) * 8, false) ||
      !t.camera_ids->reserve(*queue_, size_t(t.camera_count) * 4, false) ||
      !t.camera_map->reserve(*queue_, size_t(t.camera_count) * 4, false) ||
      !t.pixels->reserve(*queue_, size_t(t.pixel_count) * sizeof(DeepGPUFramePixel), false)) return false;
  DeepGPUFrameDispatch d;
  d.a = tile_[size_t(K::CompactCamera)]->pointer(); d.b = t.camera_keys->pointer();
  d.c = t.camera_ids->pointer(); d.n = t.camera_count;
  d.m = film.window_width; d.k = film.window_height;
  d.base = uint32_t(film.full_x + film.window_x); d.pixel = uint32_t(film.full_y + film.window_y);
  if (!operation(DeepGPUFrameOperation::CompactCameraKeys, d, blocks(t.camera_count)) ||
      !sort(*t.camera_keys, *t.camera_ids, t.camera_count) ||
      !group(*t.camera_keys, t.camera_count, t.pixel_count, *t.camera_ranges)) return false;
  d = {}; d.b = t.camera_keys->pointer(); d.c = t.camera_ids->pointer();
  d.d = t.camera_map->pointer(); d.h = controls_.device_pointer; d.n = t.camera_count;
  if (!operation(DeepGPUFrameOperation::BuildRays, d, blocks(t.camera_count))) return false;
  d = {}; d.a = t.camera_ranges->pointer(); d.b = t.pixels->pointer();
  d.c = buffers.buffer.device_pointer; d.d = controls_.device_pointer;
  d.e = (uint64_t(uint32_t(film.window_x)) << 32) | uint32_t(film.window_y);
  d.n = t.pixel_count; d.m = film.window_width; d.k = film.window_height;
  d.width = film.stride; d.base = film.pass_stride; d.pixel = population_offset;
  if (!operation(DeepGPUFrameOperation::BuildPixels, d, blocks(t.pixel_count))) return false;
  auto &p = t.parameters;
  p.cameras = tile_[size_t(K::CompactCamera)]->pointer();
  p.camera_keys = t.camera_keys->pointer(); p.camera_ids = t.camera_ids->pointer();
  p.camera_map = t.camera_map->pointer(); p.pixels = t.pixels->pointer();
  p.primitives = tile_[size_t(K::Primitive)]->pointer(); p.status = controls_.device_pointer;
  p.camera_count = t.camera_count; p.primitive_count = t.primitive_count;
  p.pixel_count = t.pixel_count; p.event_count = t.event_count;
  if (!t.primitive_keys->reserve(*queue_, size_t(t.primitive_count) * 8, false) ||
      !t.primitive_ids->reserve(*queue_, size_t(t.primitive_count) * 4, false)) return false;
  p.primitive_ids = t.primitive_ids->pointer();
  if (!upload_metadata(&p, sizeof(p), grid_parameters_)) return false;
  d = {}; d.a = grid_parameters_.device_pointer; d.b = t.primitive_keys->pointer();
  if (!operation(DeepGPUFrameOperation::CompactPrimitiveKeys, d, blocks(t.primitive_count)) ||
      !sort(*t.primitive_keys, *t.primitive_ids, t.primitive_count)) return false;
  if (!t.events->reserve(*queue_, size_t(t.event_count) * sizeof(DeepCompactEvent), false) ||
      !t.event_keys->reserve(*queue_, size_t(t.event_count) * 8, false) ||
      !t.event_ids->reserve(*queue_, size_t(t.event_count) * 4, false)) return false;
  p.events = t.events->pointer(); p.event_keys = t.event_keys->pointer(); p.event_ids = t.event_ids->pointer();
  if (!upload_metadata(&p, sizeof(p), grid_parameters_) ||
      !operation(DeepGPUFrameOperation::CompactEvents, d, blocks(t.event_count)) ||
      !sort(*t.event_keys, *t.event_ids, t.event_count) ||
      !operation(DeepGPUFrameOperation::CompactEventKeys, d, blocks(t.event_count)) ||
      !sort(*t.event_keys, *t.event_ids, t.event_count) ||
      !group(*t.event_keys, t.event_count, t.pixel_count, *t.event_ranges)) return false;
  t.primitive_keys.reset(); t.primitive_ids.reset();
  if (!t.areas->reserve(*queue_, size_t(t.event_count) * sizeof(double), false) ||
      !t.tail->reserve(*queue_, size_t(t.pixel_count) * sizeof(double), false) ||
      !t.projection->reserve(*queue_, size_t(t.pixel_count) * 2 * sizeof(float), false) ||
      !t.knots->reserve(*queue_, size_t(t.event_count) * sizeof(DeepCompactKnot), false)) return false;
  p.event_ranges = t.event_ranges->pointer(); p.areas = t.areas->pointer();
  p.knots = t.knots->pointer(); p.tail = t.tail->pointer(); p.projection = t.projection->pointer();
  if (!upload_metadata(&p, sizeof(p), grid_parameters_) ||
      !operation(DeepGPUFrameOperation::CompactSweep, d, t.pixel_count, 256) ||
      !operation(DeepGPUFrameOperation::CompactTail, d, t.pixel_count, 256) ||
      !operation(DeepGPUFrameOperation::CompactReverse, d, t.pixel_count, 256) || !good()) return false;
  /* The immutable base owns T; no consumer returns to this camera/event pass. */
  t.areas.reset(); t.events.reset(); t.event_ids.reset();
  t.camera_ranges.reset();
  tile_[size_t(K::CompactCamera)]->clear();
  return true;
}

bool DeepGPUFrameWork::build_compact_colour(CompactTile &t, Buffer &joined, Buffer &ranges,
                                            uint32_t &joined_events, const float exposure,
                                            const bool alpha_half)
{
  using K = DeepGPUResidentKind;
  const size_t source_size = tile_[size_t(K::ColourSource)]->used() / sizeof(DeepCompactColourSource);
  if (!domain_.demand.capture_rgb() || source_size > UINT32_MAX ||
      uint64_t(t.primitive_count) * 2 + 1 > UINT32_MAX)
    return fail("Deep physical colour has an invalid frozen demand or source count");
  const uint32_t sources = uint32_t(source_size), hash_capacity = 2 * t.primitive_count + 1;
  auto hash = make_buffer(), point_rgb = make_buffer(), cursors = make_buffer();
  auto page = make_buffer(), pieces = make_buffer(), page_controls = make_buffer();
  /* Page size is an execution window, never a cap on source or output quality. */
  const uint32_t page_capacity = std::max(1u, std::min(65536u, std::max(sources, 1024u)));
  if (!hash->zero(*queue_, size_t(hash_capacity) * 4) ||
      !point_rgb->zero(*queue_, size_t(t.primitive_count) * 3 * sizeof(double)) ||
      !cursors->zero(*queue_, size_t(sources) * sizeof(DeepCompactColourCursor)) ||
      !page->reserve(*queue_, size_t(page_capacity) * sizeof(DeepCompactColourPiece), false) ||
      !page_controls->zero(*queue_, 16)) return false;
  DeepGPUFrameDispatch d;
  d.a = t.parameters.primitives; d.b = hash->pointer(); d.c = controls_.device_pointer;
  d.n = t.primitive_count; d.m = hash_capacity;
  if (!operation(DeepGPUFrameOperation::CompactColourHash, d, blocks(d.n))) return false;
  DeepCompactColourParameters colour{};
  colour.base = grid_parameters_.device_pointer;
  colour.sources = tile_[size_t(K::ColourSource)]->pointer();
  colour.cursors = cursors->pointer(); colour.pieces = page->pointer(); colour.hash = hash->pointer();
  colour.point_rgb = point_rgb->pointer(); colour.controls = page_controls->pointer();
  colour.source_count = sources; colour.page_capacity = page_capacity;
  colour.hash_capacity = hash_capacity; colour.exposure = exposure;
  colour.alpha_half = alpha_half;
  if (!upload_metadata(&colour, sizeof(colour), reduce_parameters_)) return false;
  d = {}; d.a = reduce_parameters_.device_pointer;
  uint32_t receipts[4] = {};
  do {
    if (!page_controls->zero(*queue_, sizeof(receipts)) ||
        !operation(DeepGPUFrameOperation::CompactColourPage, d, blocks(sources)) ||
        !queue_->read_device_region(page_controls->memory(), 0, receipts, sizeof(receipts)) ||
        !queue_->synchronize() || !good()) return false;
    if (receipts[0] > page_capacity || (receipts[1] && !receipts[0]))
      return fail("Deep physical colour page made no forward progress");
    if (receipts[0] && !pieces->append(*queue_, page->memory(),
                                       size_t(receipts[0]) * sizeof(DeepCompactColourPiece))) return false;
  } while (receipts[1]);
  const uint64_t pieces_count = pieces->used() / sizeof(DeepCompactColourPiece);
  if (uint64_t(t.event_count) + 2 * pieces_count > UINT32_MAX)
    return fail("Deep physical colour endpoint count overflow");
  joined_events = uint32_t(t.event_count + 2 * pieces_count);
  hash.reset(); cursors.reset(); page.reset(); page_controls.reset();
  auto events = make_buffer(), keys = make_buffer(), ids = make_buffer();
  joined = make_buffer(); ranges = make_buffer();
  if (!events->reserve(*queue_, size_t(joined_events) * sizeof(DeepCompactJoinEvent), false) ||
      !keys->reserve(*queue_, size_t(joined_events) * 8, false) ||
      !ids->reserve(*queue_, size_t(joined_events) * 4, false)) return false;
  d = {}; d.a = grid_parameters_.device_pointer; d.b = pieces->pointer(); d.c = point_rgb->pointer();
  d.d = events->pointer(); d.e = keys->pointer(); d.f = ids->pointer();
  d.g = t.event_keys->pointer(); d.n = joined_events;
  if (!operation(DeepGPUFrameOperation::CompactJoinEvents, d, blocks(joined_events)) ||
      !sort(*keys, *ids, joined_events)) return false;
  d = {}; d.a = events->pointer(); d.b = keys->pointer(); d.c = ids->pointer(); d.n = joined_events;
  if (!operation(DeepGPUFrameOperation::CompactJoinKeys, d, blocks(joined_events)) ||
      !sort(*keys, *ids, joined_events) ||
      !group(*keys, joined_events, t.pixel_count, *ranges)) return false;
  pieces.reset(); point_rgb.reset(); keys.reset();
  /* The device-side fit value has a scalar prefix (40 B) plus six doubles. */
  if (!joined->reserve(*queue_, size_t(joined_events) * 2 * 88, false)) return false;
  d = {}; d.a = grid_parameters_.device_pointer; d.b = events->pointer(); d.c = ids->pointer();
  d.d = ranges->pointer(); d.e = joined->pointer();
  return operation(DeepGPUFrameOperation::CompactJoinSweep, d, t.pixel_count, 256) && good();
}

void DeepGPUFrameWork::discard(const bool archives)
{
  queue_->synchronize_for_state_rebind();
  for (auto &buffer : native_) buffer->clear();
  for (auto &buffer : tile_) buffer->clear();
  progress_reserve_->clear();
  tile_cameras_ = 0; active_ = false;
  if (archives) {
    primitive_spill_.clear(); archives_.clear();
    archive_width_ = archive_height_ = 0;
    witness_log_->clear(); witness_rgb_->clear(); witness_rays_->clear();
    witness_.log_t = witness_.rgb = witness_.rays = 0;
    trace_records_->clear(); trace_counter_->clear();
    trace_raw_->clear(); trace_raw_counter_->clear(); trace_colour_->clear(); trace_colour_counter_->clear();
    capture_case_->clear(); trace_.capture_case = 0;
    trace_.records = trace_.counter = 0;
    trace_.raw_records = trace_.raw_counter = trace_.colour_records = trace_.colour_counter = 0;
    if (!preparing_simple_) { pending_simple_.clear(); preparing_summaries_.clear(); }
  }
}

bool DeepGPUFrameWork::close_tile(RenderBuffers &buffers,
                                   const std::span<const DeepFrozenConsumer> consumers,
                                   DeepGPUFinalTile &output)
{
  const auto start = Clock::now();
  if (consumers.empty() || consumers.size() > UINT32_MAX)
    return fail("Deep GPU tile has no frozen output consumer");
  for (auto &buffer : native_) buffer->clear();
  CompactTile base;
  if (!build_compact_base(buffers, base) ||
      uint64_t(base.pixel_count) * consumers.size() > UINT32_MAX) return false;
  const auto &film = buffers.params;
  const uint32_t slots = base.pixel_count * uint32_t(consumers.size());
  if (preparing_simple_) preparing_summaries_.resize(consumers.size());
  const double base_wall = elapsed(start);
  if (tracing())
    std::fprintf(stderr, "DEEP_GPU_BASE_READY pixels=%u seconds=%.9f\n", base.pixel_count, base_wall);
  DeepNativeCaptureVector<DeepGPUFrameConsumer> config{
      DeepBudgetAllocator<DeepGPUFrameConsumer>(host_budget_)};
  for (size_t i = 0; i < consumers.size(); i++) {
    const auto &c = consumers[i].config;
    if (!std::isfinite(c.alpha_tolerance) || c.alpha_tolerance < 0 || c.alpha_tolerance > 1 ||
        !std::isfinite(c.depth_tolerance) || c.depth_tolerance < 0)
      return fail("Deep output policy contains invalid raw tolerances");
    config.push_back({uint32_t(c.precision == DeepCanonicalPrecision::Half), uint32_t(c.alpha_only),
                      float(c.alpha_tolerance), float(c.depth_tolerance), .006f, .002f});
    for (size_t earlier = 0; earlier < i; earlier++) {
      if (consumers[earlier].config == c) { config.back().reuse = uint32_t(earlier); break; }
    }
  }
  if (!upload_metadata(config.data(), config.size() * sizeof(config[0]), consumer_parameters_))
    return false;
  auto all_ranges = make_buffer();
  if (!all_ranges->reserve(*queue_, size_t(slots) * sizeof(DeepGPUFrameFinalRange), false)) return false;
  /* Only physical RGB families hold joined colour. The two alpha formats can
   * apply their own original-source zero-alpha emission exclusion. */
  std::array<Buffer, 2> joined, joined_ranges;
  std::array<uint32_t, 2> joined_events = {};
  for (uint32_t consumer = 0; consumer < config.size(); consumer++) {
    const auto &policy = config[consumer];
    const size_t range_offset = size_t(consumer) * base.pixel_count * sizeof(DeepGPUFrameFinalRange);
    if (policy.reuse < consumer) {
      DeepGPUFrameDispatch d;
      d.a = all_ranges->pointer() + size_t(policy.reuse) * base.pixel_count * sizeof(DeepGPUFrameFinalRange);
      d.b = all_ranges->pointer() + range_offset; d.c = base.pixels->pointer();
      d.n = base.pixel_count; d.kind = consumer;
      if (!operation(DeepGPUFrameOperation::CompactFinalRanges, d, blocks(base.pixel_count))) return false;
      continue;
    }
    const bool physical = !domain_.demand.simple_mode && !policy.alpha_only;
    if (physical && !joined[policy.half] &&
        !build_compact_colour(base, joined[policy.half], joined_ranges[policy.half],
                               joined_events[policy.half], film.exposure, policy.half)) return false;
    auto states = make_buffer(), page = make_buffer(), page_keys = make_buffer();
    auto receipts = make_buffer(), summaries = make_buffer();
    auto samples = make_buffer(), keys = make_buffer(), ids = make_buffer();
    auto ranges = make_buffer(), packed = make_buffer(), final_ranges = make_buffer();
    /* This is a record page, not one record per pixel. Dense pixels need many
     * output segments; tying capacity to the small AA window causes repeated
     * launches/readbacks for the same resident state. Spend a bounded fraction
     * of actual remaining quota, independently of quality and pixel count. */
    const size_t page_record_bytes = sizeof(DeepGPUFrameSample) + sizeof(uint64_t);
    const size_t remaining = device_budget_.quota() - device_budget_.used();
    const uint32_t page_capacity = uint32_t(std::max(size_t(1024),
        std::min(size_t(65536), remaining / 16 / page_record_bytes)));
    if (!states->reserve(*queue_, size_t(base.pixel_count) * (physical ? 240 : 144), false) ||
        !page->reserve(*queue_, size_t(page_capacity) * sizeof(DeepGPUFrameSample), false) ||
        !page_keys->reserve(*queue_, size_t(page_capacity) * 8, false) ||
        !receipts->zero(*queue_, 16) ||
        !summaries->reserve(*queue_, size_t(base.pixel_count) * sizeof(DeepCompactOutputSummary), false))
      return false;
    DeepCompactEncodeParameters encode{};
    encode.base = grid_parameters_.device_pointer;
    encode.joined = physical ? joined[policy.half]->pointer() : 0;
    encode.input_ranges = physical ? joined_ranges[policy.half]->pointer() : base.event_ranges->pointer();
    encode.policy = consumer_parameters_.device_pointer + size_t(consumer) * sizeof(DeepGPUFrameConsumer);
    encode.states = states->pointer(); encode.samples = page->pointer(); encode.keys = page_keys->pointer();
    encode.controls = receipts->pointer(); encode.summaries = summaries->pointer();
    encode.trace = trace_.probe_count ? trace_parameters_.device_pointer : 0;
    encode.pixel_count = base.pixel_count; encode.page_capacity = page_capacity;
    encode.physical = physical; encode.simple = domain_.demand.simple_mode;
    if (!upload_metadata(&encode, sizeof(encode), encode_parameters_)) return false;
    DeepGPUFrameDispatch d;
    d.a = encode_parameters_.device_pointer;
    d.kind = 1u + uint32_t(physical) + (encode.trace ? 2u : 0u);
    if (!operation(DeepGPUFrameOperation::CompactEncodeInit, d, blocks(base.pixel_count))) return false;
    uint32_t receipt[4] = {};
    uint64_t page_index = 0;
    /* Each pixel is independent and uses no CTA shared state. Small budgeted
     * windows otherwise form only one or two 256-thread blocks, leaving most
     * SMs idle while a few long pixel loops run. Use one warp per CTA here;
     * the separate AA/colour block scans keep their own 256-thread layout. */
    constexpr uint32_t encode_threads = 32;
    if (tracing()) std::fprintf(stderr, "DEEP_GPU_ENCODE_BEGIN consumer=%u pixels=%u capacity=%u\n",
                                consumer, base.pixel_count, page_capacity);
    do {
      if (!receipts->zero(*queue_, sizeof(receipt)) ||
          !operation(DeepGPUFrameOperation::CompactEncodePage, d,
                       blocks(base.pixel_count, encode_threads), encode_threads) ||
          !queue_->read_device_region(receipts->memory(), 0, receipt, sizeof(receipt)) ||
          !queue_->synchronize() || !good()) return false;
      page_index++;
      if (tracing() && (!(page_index & (page_index - 1)) || !receipt[1]))
        std::fprintf(stderr, "DEEP_GPU_ENCODE_PAGE consumer=%u page=%llu records=%u pending=%u work_yields=%u total=%llu seconds=%.9f\n",
                     consumer, static_cast<unsigned long long>(page_index), receipt[0], receipt[1], receipt[2],
                     static_cast<unsigned long long>(samples->used() / sizeof(DeepGPUFrameSample)), elapsed(start));
      if (receipt[0] > page_capacity || (receipt[1] && !receipt[0] && !receipt[2]) ||
          samples->used() / sizeof(DeepGPUFrameSample) + receipt[0] > UINT32_MAX)
        return fail("Deep GPU encoding page has an invalid count or no forward progress");
      if (receipt[0] &&
          (!samples->append(*queue_, page->memory(), size_t(receipt[0]) * sizeof(DeepGPUFrameSample)) ||
           !keys->append(*queue_, page_keys->memory(), size_t(receipt[0]) * 8))) return false;
    } while (receipt[1]);
    const uint32_t count = uint32_t(samples->used() / sizeof(DeepGPUFrameSample));
    if (tracing()) std::fprintf(stderr, "DEEP_GPU_ENCODE_READY consumer=%u records=%u seconds=%.9f\n",
                                consumer, count, elapsed(start));
    states.reset(); page.reset(); page_keys.reset(); receipts.reset();
    if (!ids->reserve(*queue_, size_t(count) * 4, false)) return false;
    /* MakeKeys kind=UINT_MAX assigns identity IDs to already generated keys. */
    d = {}; d.a = keys->pointer(); d.b = keys->pointer(); d.c = ids->pointer();
    d.n = count; d.kind = UINT32_MAX;
    if (!operation(DeepGPUFrameOperation::CompactPayloadIds, d, blocks(count)) ||
        !sort(*keys, *ids, count) || !group(*keys, count, base.pixel_count, *ranges) ||
        !packed->reserve(*queue_, size_t(count) * sizeof(DeepGPUFrameSample), false)) return false;
    d = {}; d.a = samples->pointer(); d.b = ids->pointer(); d.c = packed->pointer();
    d.d = controls_.device_pointer; d.n = count; d.m = sizeof(DeepGPUFrameSample);
    const uint64_t gather_blocks = (uint64_t(count) * sizeof(DeepGPUFrameSample) + 255) / 256;
    if (gather_blocks > UINT32_MAX ||
        !operation(DeepGPUFrameOperation::Gather, d, uint32_t(gather_blocks))) return false;
    samples.reset(); keys.reset(); ids.reset();
    if (!final_ranges->reserve(*queue_, size_t(base.pixel_count) * sizeof(DeepGPUFrameFinalRange), false))
      return false;
    d = {}; d.a = ranges->pointer(); d.b = final_ranges->pointer(); d.c = base.pixels->pointer();
    d.n = base.pixel_count; d.kind = consumer;
    if (!operation(DeepGPUFrameOperation::CompactFinalRanges, d, blocks(base.pixel_count))) return false;
    if (domain_.demand.simple_mode && !policy.alpha_only && preparing_simple_) {
      auto summary = std::allocate_shared<RawBytes>(DeepBudgetAllocator<RawBytes>(host_budget_), host_budget_);
      summary->bytes.resize(size_t(base.pixel_count) * sizeof(DeepCompactOutputSummary));
      if (!queue_->read_device_region(summaries->memory(), 0, summary->bytes.data(), summary->bytes.size()))
        return false;
      preparing_summaries_[consumer] = std::move(summary);
    }
    if (domain_.demand.simple_mode && !policy.alpha_only && !preparing_simple_) {
      const int combined = film.get_pass_offset(PASS_COMBINED,
          beauty_denoised_ ? PassMode::DENOISED : PassMode::NOISY);
      if (combined == PASS_UNUSED || combined < 0 || combined + 3 > film.pass_stride)
        return fail("Deep Simple colour has no complete native Combined source");
      d = {}; d.a = summaries->pointer(); d.b = packed->pointer(); d.c = final_ranges->pointer();
      d.d = buffers.buffer.device_pointer; d.e = base.pixels->pointer(); d.f = controls_.device_pointer;
      d.g = uint64_t(combined); d.h = (uint64_t(uint32_t(film.window_x)) << 32) | uint32_t(film.window_y);
      d.n = base.pixel_count; d.m = film.window_width; d.k = film.pass_stride;
      d.width = film.stride; d.base = count; d.kind = policy.half; d.p = film.exposure;
      if (!operation(DeepGPUFrameOperation::CompactSimpleColour, d, base.pixel_count, 128)) return false;
    }
    const uint64_t previous = output.samples.size();
    if (previous + count > UINT32_MAX || !good()) return fail("Deep final payload count overflow");
    d = {}; d.a = ranges->pointer(); d.b = all_ranges->pointer() + range_offset;
    d.c = base.pixels->pointer(); d.n = base.pixel_count; d.base = uint32_t(previous); d.kind = consumer;
    if (!operation(DeepGPUFrameOperation::CompactFinalRanges, d, blocks(base.pixel_count))) return false;
    output.samples.resize(size_t(previous) + count);
    if (count && !read_final(*packed, size_t(count) * sizeof(DeepGPUFrameSample),
                               output.samples.data() + size_t(previous))) return false;
    bool later_precision = false, later_colour = false;
    for (size_t later = consumer + 1; later < config.size(); later++) {
      if (config[later].reuse < later || config[later].alpha_only) continue;
      later_colour = true;
      later_precision |= config[later].half == policy.half;
    }
    if (physical && !later_precision) { joined[policy.half].reset(); joined_ranges[policy.half].reset(); }
    if (physical && !later_colour) {
      tile_[size_t(DeepGPUResidentKind::ColourSource)]->clear();
      base.camera_keys.reset(); base.camera_ids.reset(); base.camera_map.reset();
    }
  }
  if (!good()) return false;
  output.x = film.full_x + film.window_x; output.y = film.full_y + film.window_y;
  output.width = film.window_width; output.height = film.window_height;
  output.consumers = uint32_t(consumers.size());
  output.ranges.resize(slots);
  if (!read_final(*all_ranges, output.ranges.size() * sizeof(output.ranges[0]), output.ranges.data())) return false;
  if (tracing())
    std::fprintf(stderr, "DEEP_GPU_FRAME_TILE x=%d y=%d width=%d height=%d cameras=%u "
                        "primitives=%u base_events=%u samples=%llu base_wall=%.9f "
                        "total_wall=%.9f simple=%u payload_readback=final_only\n",
                 output.x, output.y, output.width, output.height, base.camera_count, base.primitive_count,
                 base.event_count, static_cast<unsigned long long>(output.samples.size()),
                 base_wall, elapsed(start), uint32_t(domain_.demand.simple_mode));
  discard(false); controls_.zero_to_device();
  return true;
}

bool DeepGPUFrameWork::read_final(DeepGPUResidentBuffer &source, const size_t bytes,
                                  void *destination)
{
  if (!bytes) return true;
  if (bytes > source.capacity()) return fail("Deep GPU final payload range overflow");
  /* This is the only payload readback seam: already encoded final records or
   * their output ranges, never native sources/Optical/reference curves. */
  return queue_->read_device_region(source.memory(), 0, destination, bytes);
}

CCL_NAMESPACE_END
