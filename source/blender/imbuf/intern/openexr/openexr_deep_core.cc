/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup openexr
 *
 * Synchronous, quota-owned serialization of already fitted canonical rows.
 */

#include "openexr_deep_core.hh"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory_resource>
#include <mutex>
#include <type_traits>

#include <Imath/half.h>
#include <OpenEXR/openexr.h>

#include "BLI_fileops.h"
#include "BLI_task.h"
#include "BLI_task.hh"
#include "DNA_scene_types.h"

namespace blender {
namespace {

class WallTime {
  double &seconds_;
  std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();

 public:
  explicit WallTime(double &seconds) : seconds_(seconds) {}
  ~WallTime()
  {
    seconds_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
  }
};

class CoreMemory;
thread_local CoreMemory *active_memory = nullptr;

class CoreMemory : public std::pmr::memory_resource {
  struct alignas(std::max_align_t) Allocation {
    CoreMemory *owner;
    size_t total;
    bool library;
  };
  const DeepExrMemoryCallbacks *callbacks_;
  DeepExrWriteReport &report_;
  CoreMemory *previous_;
  bool bound_;
  size_t library_live_ = 0;

 public:
  DeepExrCoreStream *stream = nullptr;

  CoreMemory(const DeepExrMemoryCallbacks *callbacks,
             DeepExrWriteReport &report,
             const bool bind = true)
      : callbacks_(callbacks), report_(report), previous_(active_memory), bound_(bind)
  {
    if (bound_) {
      active_memory = this;
    }
  }
  ~CoreMemory() override
  {
    if (bound_) {
      active_memory = previous_;
    }
  }

  class Use {
    CoreMemory *previous_ = active_memory;
   public:
    explicit Use(CoreMemory &memory) { active_memory = &memory; }
    ~Use() { active_memory = previous_; }
  };

  const DeepExrMemoryCallbacks *callbacks() const { return callbacks_; }
  void fail(const DeepExrWriteFailure failure, const char *message) noexcept
  {
    if (report_.failure == DeepExrWriteFailure::None) {
      report_.failure = failure;
      std::snprintf(report_.message, sizeof(report_.message), "%s", message);
    }
  }
  bool check(const exr_result_t result) noexcept
  {
    if (result == EXR_ERR_SUCCESS) {
      return true;
    }
    fail(result == EXR_ERR_OUT_OF_MEMORY ? DeepExrWriteFailure::Memory :
                                           DeepExrWriteFailure::Codec,
         exr_get_default_error_message(result));
    return false;
  }
  void *allocate_bytes(const size_t bytes, const bool library)
  {
    if (bytes > std::numeric_limits<size_t>::max() - sizeof(Allocation)) {
      fail(DeepExrWriteFailure::Memory, "Deep output allocation size overflow");
      throw std::bad_alloc();
    }
    const size_t total = bytes + sizeof(Allocation);
    try {
      if (callbacks_) {
        callbacks_->acquire(total);
      }
    }
    catch (...) {
      fail(DeepExrWriteFailure::Memory, "Deep output allocation denied by shared quota");
      throw;
    }
    auto *header = static_cast<Allocation *>(std::malloc(total));
    if (!header) {
      if (callbacks_) {
        callbacks_->release(total);
      }
      fail(DeepExrWriteFailure::Memory, "Deep output host allocation failed");
      throw std::bad_alloc();
    }
    *header = {this, total, library};
    if (library) {
      library_live_ += total;
      report_.library_peak = std::max(report_.library_peak, library_live_);
    }
    return header + 1;
  }
  static void free_bytes(void *ptr) noexcept
  {
    if (!ptr) {
      return;
    }
    auto *header = static_cast<Allocation *>(ptr) - 1;
    CoreMemory &owner = *header->owner;
    if (header->library) {
      owner.library_live_ -= header->total;
    }
    if (owner.callbacks_) {
      owner.callbacks_->release(header->total);
    }
    std::free(header);
  }
  void report_remaining() noexcept
  {
    report_.library_remaining = library_live_;
    report_.library_private_accounted = true;
    if (library_live_) {
      fail(DeepExrWriteFailure::Memory, "Deep output library retained an owned allocation");
    }
  }

 private:
  void *do_allocate(const size_t bytes, const size_t alignment) override
  {
    if (alignment > alignof(std::max_align_t)) {
      fail(DeepExrWriteFailure::Memory, "Unsupported deep output scratch alignment");
      throw std::bad_alloc();
    }
    return allocate_bytes(bytes, false);
  }
  void do_deallocate(void *ptr, size_t, size_t) override
  {
    free_bytes(ptr);
  }
  bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override
  {
    return this == &other;
  }
};

void *core_allocate(const size_t bytes) noexcept
{
  /* The installed Core codecs execute synchronously on this context's thread.
   * Allocation headers retain the actual owner, including across nested calls. */
  if (!active_memory) {
    return nullptr;
  }
  try {
    return active_memory->allocate_bytes(bytes, true);
  }
  catch (...) {
    return nullptr; /* No C++ exception may cross the Core C ABI. */
  }
}

void core_error(exr_const_context_t, const exr_result_t code, const char *message) noexcept
{
  if (active_memory) {
    active_memory->fail(code == EXR_ERR_OUT_OF_MEMORY ? DeepExrWriteFailure::Memory :
                                                        DeepExrWriteFailure::Codec,
                        message);
  }
}

int64_t core_write(exr_const_context_t,
                   void *userdata,
                   const void *buffer,
                   const uint64_t bytes,
                   const uint64_t offset,
                   exr_stream_error_func_ptr_t) noexcept
{
  auto &memory = *static_cast<CoreMemory *>(userdata);
  if (bytes > uint64_t(std::numeric_limits<int64_t>::max()) ||
      bytes > std::numeric_limits<size_t>::max() ||
      offset > uint64_t(std::numeric_limits<int64_t>::max()) - bytes)
  {
    memory.fail(DeepExrWriteFailure::Io, "Deep output file offset overflow");
    return -1;
  }
  try {
    if (memory.stream->write(offset, buffer, size_t(bytes))) {
      return int64_t(bytes);
    }
  }
  catch (...) {
    /* A private stream implementation must also never unwind through Core. */
  }
  memory.fail(DeepExrWriteFailure::Io, "Deep output stream write failed");
  return -1;
}

class StdioStream : public DeepExrCoreStream {
  FILE *file_ = nullptr;
  uint64_t position_ = 0;
  std::mutex mutex_;
  std::pmr::vector<char> buffer_;
  CoreMemory &memory_;

 public:
  explicit StdioStream(CoreMemory &memory) : buffer_(&memory), memory_(memory) {}
  ~StdioStream() override
  {
    if (file_) {
      std::fclose(file_);
    }
  }
  bool open(const char *path, const bool buffered)
  {
    if (!BLI_file_ensure_parent_dir_exists(path)) {
      return false;
    }
    file_ = BLI_fopen(path, "wb");
    if (!file_) return false;
    if (buffered) {
      try {
        buffer_.resize(32768);
      }
      catch (const std::bad_alloc &) {
        memory_.fail(DeepExrWriteFailure::Memory, "Deep file buffering exceeds shared quota");
        return false;
      }
      return std::setvbuf(file_, buffer_.data(), _IOFBF, buffer_.size()) == 0;
    }
    return std::setvbuf(file_, nullptr, _IONBF, 0) == 0;
  }
  bool write(const uint64_t offset, const void *data, const size_t bytes) override
  {
    const std::lock_guard lock(mutex_);
    if (!file_ || (offset != position_ && BLI_fseek(file_, int64_t(offset), SEEK_SET) != 0)) {
      return false;
    }
    const size_t written = bytes ? std::fwrite(data, 1, bytes, file_) : 0;
    position_ = offset + written;
    return written == bytes && !std::ferror(file_);
  }
  bool finish_checked() override
  {
    const std::lock_guard lock(mutex_);
    if (!file_) {
      return false;
    }
    const bool flushed = std::fflush(file_) == 0 && !std::ferror(file_);
    FILE *file = file_;
    file_ = nullptr;
    const bool closed = std::fclose(file) == 0;
    return flushed && closed;
  }
};

struct CoreHandles {
  CoreMemory &memory;
  exr_context_t context = nullptr;
  exr_encode_pipeline_t pipeline = EXR_ENCODE_PIPELINE_INITIALIZER;

  void finish() noexcept
  {
    if (context) {
      /* packed_buffer borrows PMR storage; the remaining pipeline buffers are
       * owned by Core and retire through the allocation headers above. */
      pipeline.packed_buffer = nullptr;
      pipeline.packed_alloc_size = 0;
      pipeline.sample_count_table = nullptr;
      memory.check(exr_encoding_destroy(context, &pipeline));
      memory.check(exr_finish(&context));
    }
  }
  ~CoreHandles()
  {
    finish();
  }
};

bool window(const int x, const int y, const int width, const int height, exr_attr_box2i_t &box)
{
  const int64_t xmax = int64_t(x) + width - 1, ymax = int64_t(y) + height - 1;
  if (width <= 0 || height <= 0 || xmax > INT32_MAX || ymax > INT32_MAX) {
    return false;
  }
  box.min.x = x;
  box.min.y = y;
  box.max.x = int32_t(xmax);
  box.max.y = int32_t(ymax);
  return true;
}

void store32(unsigned char *out, const uint32_t value)
{
  for (int byte = 0; byte < 4; ++byte) {
    out[byte] = static_cast<unsigned char>(value >> (byte * 8));
  }
}

bool pack_row(const std::vector<std::vector<DeepSample>> &row,
              const DeepExrWriteParams &params,
              CoreMemory &memory,
              std::pmr::vector<unsigned char> &counts,
              std::pmr::vector<unsigned char> &pixels)
{
  if (row.size() != size_t(params.width)) {
    memory.fail(DeepExrWriteFailure::InvalidInput, "Deep row width changed during output");
    return false;
  }
  const size_t sample_bytes = (params.alpha_only ? 1 : 4) * (params.use_half_float ? 2 : 4) + 8;
  uint64_t samples = 0;
  for (const auto &pixel : row) {
    if (pixel.size() > uint64_t(INT32_MAX) - samples) {
      /* Deep sample count tables use signed 32-bit cumulative counts. */
      memory.fail(DeepExrWriteFailure::InvalidInput, "Deep row cumulative sample count overflow");
      return false;
    }
    samples += pixel.size();
    for (const DeepSample &s : pixel) {
      if (!std::isfinite(s.r) || !std::isfinite(s.g) || !std::isfinite(s.b) ||
          !std::isfinite(s.a) || !std::isfinite(s.z) || !std::isfinite(s.z_back) || s.a < 0 ||
          s.a > 1 || s.z_back < s.z ||
          (params.use_half_float && !params.alpha_only &&
           (std::abs(s.r) > 65504 || std::abs(s.g) > 65504 || std::abs(s.b) > 65504)))
      {
        memory.fail(DeepExrWriteFailure::InvalidInput, "Invalid deep row sample or HALF overflow");
        return false;
      }
    }
  }
  if (samples > std::numeric_limits<size_t>::max() / sample_bytes ||
      row.size() > std::numeric_limits<size_t>::max() / 4)
  {
    memory.fail(DeepExrWriteFailure::InvalidInput, "Deep row packed byte size overflow");
    return false;
  }
  counts.resize(row.size() * 4);
  pixels.resize(size_t(samples) * sample_bytes);
  uint32_t cumulative = 0;
  for (size_t x = 0; x < row.size(); ++x) {
    cumulative += uint32_t(row[x].size());
    store32(counts.data() + x * 4, cumulative);
  }
  size_t offset = 0;
  /* EXR channel order is lexical: A, B, G, R, Z, ZBack. Retain input sample
   * order, including equal-depth ties; this boundary never merges/refits. */
  for (int channel = 0; channel < 6; ++channel) {
    if (params.alpha_only && channel >= 1 && channel <= 3) {
      continue;
    }
    for (const auto &pixel : row) {
      for (const DeepSample &s : pixel) {
        const float values[] = {s.a, s.b, s.g, s.r, s.z, s.z_back};
        if (params.use_half_float && channel < 4) {
          const uint16_t bits = IMATH_NAMESPACE::half(values[channel]).bits();
          pixels[offset++] = static_cast<unsigned char>(bits);
          pixels[offset++] = static_cast<unsigned char>(bits >> 8);
        }
        else {
          store32(pixels.data() + offset, std::bit_cast<uint32_t>(values[channel]));
          offset += 4;
        }
      }
    }
  }
  return true;
}

bool read_packed_row(DeepExrPackedRowReader reader,
                     const int y,
                     const DeepExrWriteParams &params,
                     CoreMemory &memory,
                     std::pmr::vector<unsigned char> &counts,
                     std::pmr::vector<unsigned char> &pixels)
{
  if (!reader(y, counts, pixels) || counts.size() != size_t(params.width) * 4) {
    memory.fail(DeepExrWriteFailure::Reader, "Packed Deep row reader failed");
    return false;
  }
  const unsigned char *last = counts.data() + counts.size() - 4;
  uint32_t total = 0;
  for (int b = 0; b < 4; ++b) {
    total |= uint32_t(last[b]) << (8 * b);
  }
  const size_t sample_bytes = 8 + (params.alpha_only ? 1 : 4) *
                                    (params.use_half_float ? 2 : 4);
  if (total > INT32_MAX || uint64_t(total) * sample_bytes != pixels.size()) {
    memory.fail(DeepExrWriteFailure::InvalidInput, "Packed Deep row byte count mismatch");
    return false;
  }
  return true;
}

struct PackedCodecPosition {
  PackedCodecPosition(const DeepExrMemoryCallbacks *callbacks, exr_context_t context)
      : memory(callbacks, report, false),
        context(context),
        counts(&memory),
        pixels(&memory),
        compressed_counts(&memory)
  {
  }

  ~PackedCodecPosition()
  {
    finish();
  }

  void finish()
  {
    if (initialized) {
      const CoreMemory::Use scope(memory);
      pipeline.packed_buffer = nullptr;
      pipeline.packed_alloc_size = 0;
      pipeline.sample_count_table = nullptr;
      memory.check(exr_encoding_destroy(context, &pipeline));
      initialized = false;
    }
    memory.report_remaining();
  }

  void compress(const int part)
  {
    const CoreMemory::Use scope(memory);
    const exr_result_t result = initialized ?
                                    exr_encoding_update(context, part, &chunk, &pipeline) :
                                    exr_encoding_initialize(context, part, &chunk, &pipeline);
    initialized = true;
    if (!memory.check(result)) {
      return;
    }
    pipeline.sample_count_table = nullptr;
    pipeline.packed_buffer = counts.data();
    pipeline.packed_alloc_size = 0;
    pipeline.packed_bytes = counts.size();
    if (!memory.check(exr_compress_chunk(&pipeline))) {
      return;
    }
    const auto *begin = static_cast<const unsigned char *>(pipeline.compressed_buffer);
    compressed_counts.assign(begin, begin + pipeline.compressed_bytes);
    if (!pixels.empty()) {
      pipeline.packed_buffer = pixels.data();
      pipeline.packed_bytes = pixels.size();
      memory.check(exr_compress_chunk(&pipeline));
    }
  }

  DeepExrWriteReport report;
  CoreMemory memory;
  exr_context_t context;
  exr_chunk_info_t chunk{};
  exr_encode_pipeline_t pipeline = EXR_ENCODE_PIPELINE_INITIALIZER;
  std::pmr::vector<unsigned char> counts, pixels, compressed_counts;
  bool initialized = false;
};

bool write_packed_compressed(CoreHandles &core,
                             DeepExrPackedRowReader reader,
                             const DeepExrWriteParams &params,
                             const exr_attr_box2i_t &data,
                             const int part,
                             DeepExrWriteReport &report)
{
  /* Reserve the earliest row first and keep a fixed number of complete rows in flight.
   * OpenEXR stream writes and the sink's row leases remain on the caller thread. */
  const auto *callbacks = core.memory.callbacks();
  const auto context = core.context;
  const char *requested = std::getenv("CYCLES_TEST_DEEP_CODEC_ROWS");
  const int positions = requested && std::strcmp(requested, "4") == 0 ? 4 : 16;
  std::array<PackedCodecPosition, 32> slots = {
      PackedCodecPosition(callbacks, context),
      PackedCodecPosition(callbacks, context),
      PackedCodecPosition(callbacks, context),
      PackedCodecPosition(callbacks, context),
      PackedCodecPosition(callbacks, context),
      PackedCodecPosition(callbacks, context),
      PackedCodecPosition(callbacks, context),
      PackedCodecPosition(callbacks, context),
      PackedCodecPosition(callbacks, context),
      PackedCodecPosition(callbacks, context),
      PackedCodecPosition(callbacks, context),
      PackedCodecPosition(callbacks, context),
      PackedCodecPosition(callbacks, context),
      PackedCodecPosition(callbacks, context),
      PackedCodecPosition(callbacks, context),
      PackedCodecPosition(callbacks, context),
      PackedCodecPosition(callbacks, context),
      PackedCodecPosition(callbacks, context),
      PackedCodecPosition(callbacks, context),
      PackedCodecPosition(callbacks, context),
      PackedCodecPosition(callbacks, context),
      PackedCodecPosition(callbacks, context),
      PackedCodecPosition(callbacks, context),
      PackedCodecPosition(callbacks, context),
      PackedCodecPosition(callbacks, context),
      PackedCodecPosition(callbacks, context),
      PackedCodecPosition(callbacks, context),
      PackedCodecPosition(callbacks, context),
      PackedCodecPosition(callbacks, context),
      PackedCodecPosition(callbacks, context),
      PackedCodecPosition(callbacks, context),
      PackedCodecPosition(callbacks, context)};
  struct Job { PackedCodecPosition *slot; int part; };
  std::array<Job, 32> jobs{};
  struct PoolOwner {
    TaskPool *pool = BLI_task_pool_create(nullptr, TASK_PRIORITY_HIGH);
    ~PoolOwner()
    {
      /* Every failure joins before jobs, buffers and codec handles retire. */
      BLI_task_pool_work_and_wait(pool);
      BLI_task_pool_free(pool);
    }
  } tasks;
  const auto submit = [&](const int bank, const int count) {
    for (int i = 0; i < count; ++i) {
      const int index = bank * positions + i;
      jobs[index] = {&slots[index], part};
      BLI_task_pool_push(tasks.pool, [](TaskPool *, void *data) {
        auto &job = *static_cast<Job *>(data);
        try { job.slot->compress(job.part); }
        catch (const std::bad_alloc &) {
          job.slot->memory.fail(DeepExrWriteFailure::Memory,
                               "Deep private codec quota exhausted");
        }
      }, &jobs[index], false, nullptr);
    }
  };
  int pending_bank = 0, pending_count = 0, pending_first = 0;
  std::chrono::steady_clock::time_point pending_start;
  const auto retire = [&]() {
    if (!pending_count) return true;
    BLI_task_pool_work_and_wait(tasks.pool);
    const unsigned char empty = 0;
    for (int i = 0; i < pending_count; ++i) {
      const auto &slot = slots[pending_bank * positions + i];
      if (slot.report.failure != DeepExrWriteFailure::None) {
        core.memory.fail(slot.report.failure, slot.report.message);
        return false;
      }
      const bool has_pixels = !slot.pixels.empty();
      if (!core.memory.check(exr_write_deep_scanline_chunk(
              context, part, data.min.y + pending_first + i,
              has_pixels ? slot.pipeline.compressed_buffer : &empty,
              has_pixels ? slot.pipeline.compressed_bytes : 0, slot.pixels.size(),
              slot.compressed_counts.data(), slot.compressed_counts.size())))
      {
        return false;
      }
    }
    /* Submission-to-write span overlaps caller packing of the other bank.
     * These diagnostic wall fields must not be added as disjoint stages. */
    report.compress_write_seconds += std::chrono::duration<double>(
        std::chrono::steady_clock::now() - pending_start).count();
    pending_count = 0;
    return true;
  };
  int bank = 0;
  for (int first = 0; first < params.height; first += positions) {
    const int count = std::min(positions, params.height - first);
    for (int i = 0; i < count; ++i) {
      const WallTime elapsed(report.pack_seconds);
      auto &slot = slots[bank * positions + i];
      if (!read_packed_row(reader, params.height - 1 - first - i, params, core.memory,
                           slot.counts, slot.pixels) ||
          !core.memory.check(exr_write_scanline_chunk_info(context, part, data.min.y + first + i,
                                                            &slot.chunk)))
      {
        return false;
      }
    }
    if (!retire()) return false;
    pending_bank = bank;
    pending_count = count;
    pending_first = first;
    pending_start = std::chrono::steady_clock::now();
    submit(bank, count);
    bank ^= 1;
  }
  if (!retire()) return false;
  for (auto &slot : slots) {
    slot.finish();
    report.library_peak = std::max(report.library_peak, slot.report.library_peak);
    if (slot.report.failure != DeepExrWriteFailure::None) {
      core.memory.fail(slot.report.failure, slot.report.message);
      return false;
    }
  }
  return true;
}

template<typename Reader>
bool write_rows(CoreHandles &core,
                Reader reader,
                const DeepExrWriteParams &params,
                const exr_attr_box2i_t &data,
                const exr_compression_t compression,
                DeepExrWriteReport &report)
{
  CoreMemory &memory = core.memory;
  const exr_context_t context = core.context;
  int part = -1;
  exr_attr_box2i_t display;
  if (!window(params.has_display_window ? params.display_offset_x : 0,
              params.has_display_window ? params.display_offset_y : 0,
              params.has_display_window ? params.display_width : params.width,
              params.has_display_window ? params.display_height : params.height,
              display))
  {
    memory.fail(DeepExrWriteFailure::InvalidInput, "Invalid deep display window");
    return false;
  }
  const exr_attr_v2f_t center = {0, 0};
  if (!memory.check(exr_add_part(context, nullptr, EXR_STORAGE_DEEP_SCANLINE, &part)) ||
      !memory.check(exr_initialize_required_attr(
          context, part, &display, &data, 1, &center, 1, EXR_LINEORDER_INCREASING_Y, compression)))
  {
    return false;
  }
  if constexpr (std::is_same_v<Reader, DeepExrPackedRowReader>) {
    /* A faster lossless encoder; channel values and the selected ZIPS format
     * are unchanged. Include the resulting file size and I/O in frame timing. */
    if (compression == EXR_COMPRESSION_ZIPS &&
        !memory.check(exr_set_zip_compression_level(context, part, 1)))
    {
      return false;
    }
  }
  const char *names[] = {"A", "B", "G", "R", "Z", "ZBack"};
  for (int channel = 0; channel < 6; ++channel) {
    if (params.alpha_only && channel >= 1 && channel <= 3) {
      continue;
    }
    if (!memory.check(exr_add_channel(context,
                                      part,
                                      names[channel],
                                      params.use_half_float && channel < 4 ? EXR_PIXEL_HALF :
                                                                             EXR_PIXEL_FLOAT,
                                      EXR_PERCEPTUALLY_LOGARITHMIC,
                                      1,
                                      1)))
    {
      return false;
    }
  }
  if (!memory.check(exr_write_header(context))) {
    return false;
  }
  if constexpr (std::is_same_v<Reader, DeepExrPackedRowReader>) {
    if (compression != EXR_COMPRESSION_NONE) {
      return write_packed_compressed(core, reader, params, data, part, report);
    }
  }
  std::vector<std::vector<DeepSample>> row;
  std::pmr::vector<unsigned char> counts(&memory), pixels(&memory), compressed_counts(&memory);
  for (int file_y = 0; file_y < params.height; ++file_y) {
    if constexpr (std::is_same_v<Reader, DeepExrPackedRowReader>) {
      const WallTime elapsed(report.pack_seconds);
      if (!read_packed_row(reader, params.height - 1 - file_y, params, memory, counts, pixels)) {
        return false;
      }
    }
    else {
      {
        const WallTime elapsed(report.read_seconds);
        if (!reader(params.height - 1 - file_y, row)) {
          memory.fail(DeepExrWriteFailure::Reader, "Deep row reader failed");
          return false;
        }
      }
      {
        const WallTime elapsed(report.pack_seconds);
        if (!pack_row(row, params, memory, counts, pixels)) {
          return false;
        }
      }
    }
    const WallTime elapsed(report.compress_write_seconds);
    const int y = data.min.y + file_y;
    exr_chunk_info_t chunk{};
    if (!memory.check(exr_write_scanline_chunk_info(context, part, y, &chunk)) ||
        !memory.check(file_y == 0 ?
                          exr_encoding_initialize(context, part, &chunk, &core.pipeline) :
                          exr_encoding_update(context, part, &chunk, &core.pipeline)))
    {
      return false;
    }
    const void *pixel_data = pixels.data(), *count_data = counts.data();
    size_t pixel_bytes = pixels.size(), count_bytes = counts.size();
    if (compression != EXR_COMPRESSION_NONE) {
      /* Core's generic Deep pack/table path is incomplete. Its public codec
       * helper is complete for opaque buffers: compress the count table and
       * packed channels separately, then use the checked Deep chunk API. */
      auto &pipeline = core.pipeline;
      pipeline.sample_count_table = nullptr;
      pipeline.packed_buffer = counts.data();
      pipeline.packed_alloc_size = 0;
      pipeline.packed_bytes = counts.size();
      if (!memory.check(exr_compress_chunk(&pipeline))) {
        return false;
      }
      const auto *begin = static_cast<const unsigned char *>(pipeline.compressed_buffer);
      compressed_counts.assign(begin, begin + pipeline.compressed_bytes);
      count_data = compressed_counts.data();
      count_bytes = compressed_counts.size();
      if (!pixels.empty()) {
        pipeline.packed_buffer = pixels.data();
        pipeline.packed_bytes = pixels.size();
        if (!memory.check(exr_compress_chunk(&pipeline))) {
          return false;
        }
        pixel_data = pipeline.compressed_buffer;
        pixel_bytes = pipeline.compressed_bytes;
      }
    }
    const unsigned char empty = 0;
    if (!memory.check(exr_write_deep_scanline_chunk(context,
                                                    part,
                                                    y,
                                                    pixel_bytes ? pixel_data : &empty,
                                                    pixel_bytes,
                                                    pixels.size(),
                                                    count_data,
                                                    count_bytes)))
    {
      return false;
    }
  }
  return true;
}

}  // namespace

template<typename Reader>
static bool save_deep_rows_core(Reader reader,
                                 const DeepExrWriteParams &params,
                                 const char *filepath,
                                 const DeepExrMemoryCallbacks *callbacks,
                                 DeepExrWriteReport *output_report,
                                 DeepExrCoreStream *stream)
{
  DeepExrWriteReport local_report;
  DeepExrWriteReport &report = output_report ? *output_report : local_report;
  report = {};
  const WallTime total_time(report.total_seconds);
  CoreMemory memory(callbacks, report);
  exr_compression_t compression;
  switch (params.compression) {
    case R_IMF_EXR_CODEC_NONE:
      compression = EXR_COMPRESSION_NONE;
      break;
    case R_IMF_EXR_CODEC_RLE:
      compression = EXR_COMPRESSION_RLE;
      break;
    case R_IMF_EXR_CODEC_ZIPS:
      compression = EXR_COMPRESSION_ZIPS;
      break;
    default:
      memory.fail(DeepExrWriteFailure::InvalidInput, "Unsupported canonical Deep compression");
      return false;
  }
  exr_attr_box2i_t data, display;
  if (!filepath || !filepath[0] ||
      !window(params.has_display_window ? params.data_offset_x : 0,
              params.has_display_window ? params.data_offset_y : 0,
              params.width,
              params.height,
              data))
  {
    memory.fail(DeepExrWriteFailure::InvalidInput, "Invalid deep output path or data window");
    return false;
  }
  if (!window(params.has_display_window ? params.display_offset_x : 0,
              params.has_display_window ? params.display_offset_y : 0,
              params.has_display_window ? params.display_width : params.width,
              params.has_display_window ? params.display_height : params.height,
              display))
  {
    memory.fail(DeepExrWriteFailure::InvalidInput, "Invalid deep display window");
    return false;
  }
  StdioStream file(memory);
  if (!stream) {
    if (!file.open(filepath, std::is_same_v<Reader, DeepExrPackedRowReader>)) {
      memory.fail(DeepExrWriteFailure::Io, "Unable to open Deep output stream");
      return false;
    }
    stream = &file;
  }
  memory.stream = stream;
  {
    CoreHandles core{memory};
    try {
      exr_context_initializer_t init = EXR_DEFAULT_CONTEXT_INITIALIZER;
      init.flags |= EXR_CONTEXT_FLAG_WRITE_LEGACY_HEADER;
      init.alloc_fn = core_allocate;
      init.free_fn = CoreMemory::free_bytes;
      init.error_handler_fn = core_error;
      init.user_data = &memory;
      init.write_fn = core_write;
      if (memory.check(exr_start_write(&core.context, filepath, EXR_WRITE_FILE_DIRECTLY, &init))) {
        write_rows(core, reader, params, data, compression, report);
      }
    }
    catch (const std::bad_alloc &) {
      memory.fail(DeepExrWriteFailure::Memory, "Deep row output allocation failed");
    }
    catch (...) {
      memory.fail(DeepExrWriteFailure::Reader, "Deep row output callback failed");
    }
    const WallTime elapsed(report.finish_seconds);
    core.finish(); /* Includes the offset table/footer; not an implicit close. */
  }
  try {
    const WallTime elapsed(report.finish_seconds);
    report.close_checked = stream->finish_checked();
  }
  catch (...) {
    report.close_checked = false;
  }
  if (!report.close_checked) {
    memory.fail(DeepExrWriteFailure::Io, "Deep output flush/close failed");
  }
  memory.report_remaining();
  return report.failure == DeepExrWriteFailure::None && report.close_checked;
}

bool imb_exr_save_deep_rows_core(DeepExrRowReader reader,
                                 const DeepExrWriteParams &params,
                                 const char *filepath,
                                 const DeepExrMemoryCallbacks *callbacks,
                                 DeepExrWriteReport *report,
                                 DeepExrCoreStream *stream)
{
  return save_deep_rows_core(reader, params, filepath, callbacks, report, stream);
}

bool imb_exr_save_deep_packed_rows_core(DeepExrPackedRowReader reader,
                                        const DeepExrWriteParams &params,
                                        const char *filepath,
                                        const DeepExrMemoryCallbacks *memory,
                                        DeepExrWriteReport *report)
{
  return save_deep_rows_core(reader, params, filepath, memory, report, nullptr);
}

}  // namespace blender
