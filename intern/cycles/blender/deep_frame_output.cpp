/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */
#include "blender/deep_frame_output.h"
#include "kernel/integrator/deep_bounded_types.h"

#include "IMB_openexr.hh"
#include "BLI_path_utils.hh"
#include <bit>
#include <cstring>
#include "util/half.h"
#include "util/md5.h"
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <mutex>

CCL_NAMESPACE_BEGIN

namespace {
class CanonicalFileSink : public DeepOutputSink {
 public:
  CanonicalFileSink(DeepFrameProduction &production,
                    DeepRasterWindow raster,
                    BlenderDeepFrameWindow display,
                    const char *path,
                    int compression,
                    const DeepCanonicalEncodeConfig &config,
                    const DeepConsumerToken &token)
      : production_(production),
        budget_(production.memory_budget()),
        raster_(raster),
        display_(display),
        path_(path),
        compression_(compression),
        config_(config),
        token_(token),
        packed_row_lease_(production.memory_budget()),
        packed_masks_(DeepBudgetAllocator<uint32_t>(production.memory_budget()))
  {
    const char *borrow = getenv("CYCLES_TEST_DEEP_ROW_BORROW");
    borrow_row_ = !borrow || std::strcmp(borrow, "0") != 0;
  }
  ~CanonicalFileSink() override
  {
    budget_.relinquish(charged_);
  }
  DeepStatus begin(const DeepFrameKey &, const DeepRasterWindow &) override
  {
    return {};
  }
  DeepStatus write_rows(const DeepCanonicalRowLease &rows) override
  {
    if (packed_counts_ && packed_pixels_) {
      return pack_bounded_row(rows);
    }
    if (!row_ || rows.row_count() != 1 || rows.pixels().size() != raster_.width)
      return DeepStatus::fail(DeepErrorCode::InvalidLayout,
                              "Canonical writer requires its borrowed single row");
    for (const DeepCanonicalPixel &pixel : rows.pixels()) {
      const size_t index = size_t(pixel.y - raster_.y) * raster_.width + pixel.x - raster_.x;
      const size_t column = size_t(pixel.x - raster_.x);
      if (index >= size_t(raster_.width) * raster_.height || column >= row_->size())
        return DeepStatus::fail(DeepErrorCode::InvalidLayout,
                                "Canonical output pixel outside frame");
      DeepEncodedPixelLease encoded;
      if (const DeepStatus status = production_.encoded_pixel(token_, index, encoded); !status)
        return status;
      const size_t count = encoded.size();
      encoded_samples_ += count;
      auto &destination = (*row_)[column];
      charge(count, sizeof(blender::DeepSample));
      destination.reserve(count);
      if (destination.capacity() > count)
        charge(destination.capacity() - count, sizeof(blender::DeepSample));
      for (size_t i = 0; i < count; ++i) {
        const auto sample = encoded.sample(i);
        destination.push_back({sample.r, sample.g, sample.b, sample.a, sample.z, sample.z_back});
      }
      if (deep_frame_pipeline_requested(getenv("CYCLES_EXPERIMENTAL_DEEP_FRAME_VERIFY_PAYLOAD"))) {
        const uint64_t count = destination.size();
        hash_.append(reinterpret_cast<const uint8_t *>(&count), sizeof(count));
        for (const auto &sample : destination) {
          const float rgba[6] = {sample.r, sample.g, sample.b, sample.a, sample.z, sample.z_back};
          hash_.append(reinterpret_cast<const uint8_t *>(rgba + (config_.alpha_only ? 3 : 0)),
                       (config_.alpha_only ? 3 : 6) * sizeof(float));
        }
      }
    }
    return {};
  }
  DeepStatus write_file(const uint32_t consumer_id, DeepFramePipeline &pipeline)
  {
    if (const auto status = production_.encoding_report(token_, proof_); !status)
      return status;
    if ((!proof_.normalized_checked && !proof_.gpu_grid_checked && !proof_.bounded_payload_checked) || !proof_.quantized_checked)
      return DeepStatus::fail(DeepErrorCode::Incomplete,
                              "Encoded consumer has no complete decoder proof");
    encoded_knots_ = proof_.input_knots;
    merge_candidates_ = proof_.merge_candidates;
    merged_intervals_ = proof_.merged_intervals;
    for (int c = 0; c < 4; c++) {
      maximum_error_[c] = proof_.total_error[c];
      maximum_budget_ratio_[c] = proof_.maximum_budget_ratio[c];
    }
    blender::DeepExrWriteParams params;
    params.width = raster_.width;
    params.height = raster_.height;
    params.compression = compression_;
    params.use_half_float = config_.precision == DeepCanonicalPrecision::Half;
    params.alpha_only = config_.alpha_only;
    params.has_display_window = display_.enabled;
    params.display_width = display_.width;
    params.display_height = display_.height;
    params.display_offset_x = display_.offset_x;
    params.display_offset_y = display_.offset_y;
    params.data_offset_x = display_.data_x;
    params.data_offset_y = display_.data_y;
    DeepStatus reader_status;
    auto reader = [&](const int native_y, std::vector<std::vector<blender::DeepSample>> &row) {
      try {
        /* OpenEXR has finished borrowing the previous row before requesting this
         * one. Release its input allocation and ledger together, without keeping
         * frame-sized encoded vectors or frame-sized row-lease metadata. */
        std::vector<std::vector<blender::DeepSample>>().swap(row);
        budget_.relinquish(charged_);
        charged_ = 0;
        charge(raster_.width, sizeof(std::vector<blender::DeepSample>));
        row.reserve(raster_.width);
        if (row.capacity() > raster_.width)
          charge(row.capacity() - raster_.width, sizeof(std::vector<blender::DeepSample>));
        row.resize(raster_.width);
        DeepCanonicalRowLease rows;
        reader_status = pipeline.acquire_closed_rows(consumer_id, raster_.y + native_y, 1, rows);
        if (reader_status) {
          row_ = &row;
          reader_status = pipeline.consume_rows(rows, *this);
          row_ = nullptr;
        }
        return bool(reader_status);
      }
      catch (const std::bad_alloc &) {
        row_ = nullptr;
        reader_status = DeepStatus::fail(DeepErrorCode::BudgetExceeded,
                                         "Canonical output row exceeds shared quota: " + path_);
        return false;
      }
    };
    size_t writer_live = 0, writer_peak = 0;
    std::mutex writer_memory;
    bool writer_budget_denied = false;
    auto acquire = [&](const size_t bytes) {
      const std::lock_guard lock(writer_memory);
      try {
        budget_.acquire(bytes);
      }
      catch (const std::bad_alloc &) {
        writer_budget_denied = true;
        throw;
      }
      writer_live += bytes;
      writer_peak = std::max(writer_peak, writer_live);
    };
    auto release = [&](const size_t bytes) {
      const std::lock_guard lock(writer_memory);
      writer_live -= bytes;
      budget_.relinquish(bytes);
    };
    const blender::DeepExrMemoryCallbacks memory{acquire, release};
    blender::DeepExrWriteReport writer_report;
    auto packed_reader = [&](const int native_y,
                             std::pmr::vector<unsigned char> &counts,
                             std::pmr::vector<unsigned char> &pixels) {
      DeepCanonicalRowLease rows;
      reader_status = pipeline.acquire_closed_rows(consumer_id, raster_.y + native_y, 1, rows);
      if (reader_status) {
        packed_counts_ = &counts;
        packed_pixels_ = &pixels;
        reader_status = pipeline.consume_rows(rows, *this);
        packed_counts_ = packed_pixels_ = nullptr;
      }
      return bool(reader_status);
    };
    file_finished_ = proof_.bounded_payload_checked ?
                         blender::IMB_exr_save_deep_packed_rows(
                             packed_reader, params, path_.c_str(), &memory, &writer_report) :
                         blender::IMB_exr_save_deep_rows(
                             reader, params, path_.c_str(), &memory, &writer_report);
    /* The backend has destroyed its borrowed row before returning. */
    row_ = nullptr;
    budget_.relinquish(charged_);
    charged_ = 0;
    if (deep_frame_pipeline_requested(getenv("CYCLES_EXPERIMENTAL_DEEP_FRAME_VERIFY_PAYLOAD")))
      std::fprintf(stderr,
                   "DEEP_FRAME_OUTPUT_ROW_ALLOCATION path=%s owned_peak=%llu owned_remaining=%llu "
                   "library_private_accounted=%d library_peak=%llu library_remaining=%llu "
                   "close_checked=%d failure=%d\n",
                   path_.c_str(),
                   static_cast<unsigned long long>(writer_peak),
                   static_cast<unsigned long long>(writer_live),
                   int(writer_report.library_private_accounted),
                   static_cast<unsigned long long>(writer_report.library_peak),
                   static_cast<unsigned long long>(writer_report.library_remaining),
                   int(writer_report.close_checked),
                   int(writer_report.failure));
    std::fprintf(stderr,
                 "DEEP_FRAME_OUTPUT_WRITER_TIME path=%s total=%.9f row_read=%.9f pack=%.9f "
                 "compress_write=%.9f finish=%.9f stages_nested_in_total=1\n",
                 path_.c_str(),
                 writer_report.total_seconds,
                 writer_report.read_seconds,
                 writer_report.pack_seconds,
                 writer_report.compress_write_seconds,
                 writer_report.finish_seconds);
    /* A later footer allocation/close failure cannot replace the original
     * reader or I/O failure. The backend report retains actual first cause. */
    if (writer_report.failure == blender::DeepExrWriteFailure::Reader && !reader_status)
      return reader_status;
    if (writer_report.failure == blender::DeepExrWriteFailure::Memory)
      return DeepStatus::fail(
          DeepErrorCode::BudgetExceeded,
          std::string(writer_budget_denied ? "Canonical writer exceeds shared quota: " :
                                            "Canonical writer allocation failed: ") +
              writer_report.message + ": " + path_);
    if (!file_finished_)
      return DeepStatus::fail(DeepErrorCode::OutputFailure,
                              "Canonical EXR write/close failed: " +
                                  std::string(writer_report.message) + ": " + path_);
    if (writer_live || writer_report.library_remaining ||
        !writer_report.library_private_accounted || !writer_report.close_checked)
      return DeepStatus::fail(DeepErrorCode::OutputFailure,
                              "Canonical writer ownership/checked-close proof incomplete: " +
                                  path_);
    return {};
  }
  DeepStatus finish_checked() override
  {
    if (!file_finished_)
      return DeepStatus::fail(DeepErrorCode::OutputFailure,
                              "Canonical file has not passed checked close: " + path_);
    if (encoded_samples_ != proof_.output_samples || pruned_samples_ > encoded_samples_)
      return DeepStatus::fail(DeepErrorCode::InvalidCount,
                              "Encoded consumer sample count changed during output: " + path_);
    std::fprintf(
        stderr,
        "DEEP_FRAME_OUTPUT_MEMORY path=%s host_peak=%llu host_used=%llu host_quota=%llu\n",
        path_.c_str(),
        static_cast<unsigned long long>(budget_.peak()),
        static_cast<unsigned long long>(budget_.used()),
        static_cast<unsigned long long>(budget_.quota()));
    std::fprintf(stderr,
                 "DEEP_FRAME_CHECKED_OUTPUT path=%s alpha_only=%d knots=%llu samples=%llu "
                 "bytes_used=%llu encoded_samples=%llu pruned_empty_samples=%llu\n",
                 path_.c_str(),
                 int(config_.alpha_only),
                 static_cast<unsigned long long>(encoded_knots_),
                 static_cast<unsigned long long>(encoded_samples_ - pruned_samples_),
                 static_cast<unsigned long long>(budget_.used()),
                 static_cast<unsigned long long>(encoded_samples_),
                 static_cast<unsigned long long>(pruned_samples_));
    if (deep_frame_pipeline_requested(getenv("CYCLES_EXPERIMENTAL_DEEP_FRAME_VERIFY_PAYLOAD"))) {
      if (proof_.bounded_payload_checked)
        std::fprintf(stderr,
                     "DEEP_BOUNDED_FRAME_ENCODING path=%s alpha_only=%d samples=%llu "
                     "validation=bounded_schema7_payload quantized_checked=1 "
                     "written_samples=%llu pruned_empty_samples=%llu\n",
                     path_.c_str(), int(config_.alpha_only),
                     static_cast<unsigned long long>(encoded_samples_),
                     static_cast<unsigned long long>(encoded_samples_ - pruned_samples_),
                     static_cast<unsigned long long>(pruned_samples_));
      else if (proof_.gpu_grid_checked)
        std::fprintf(stderr,
                     "DEEP_GPU_FRAME_ENCODING path=%s alpha_only=%d alpha_budget=%.9g "
                     "rgb_absolute=%.9g rgb_relative=%.9g samples=%llu "
                     "validation=adaptive_device_grid quantized_checked=1 continuous_certificate=0\n",
                     path_.c_str(), int(config_.alpha_only), config_.alpha_tolerance,
                     config_.rgb_absolute_tolerance, config_.rgb_relative_tolerance,
                     static_cast<unsigned long long>(encoded_samples_));
      else std::fprintf(stderr,
                   "DEEP_FRAME_ENCODING_PROOF path=%s alpha_only=%d alpha_budget=%.17g "
                   "rgb_absolute=%.17g rgb_relative=%.17g error_t=%.17g error_r=%.17g "
                   "error_g=%.17g error_b=%.17g ratio_t=%.17g ratio_r=%.17g ratio_g=%.17g "
                   "ratio_b=%.17g merge_candidates=%llu merged_intervals=%llu\n",
                   path_.c_str(),
                   int(config_.alpha_only),
                   config_.alpha_tolerance,
                   config_.rgb_absolute_tolerance,
                   config_.rgb_relative_tolerance,
                   maximum_error_[0],
                   maximum_error_[1],
                   maximum_error_[2],
                   maximum_error_[3],
                   maximum_budget_ratio_[0],
                   maximum_budget_ratio_[1],
                   maximum_budget_ratio_[2],
                   maximum_budget_ratio_[3],
                   static_cast<unsigned long long>(merge_candidates_),
                   static_cast<unsigned long long>(merged_intervals_));
      std::fprintf(stderr,
                   "DEEP_FRAME_PAYLOAD path=%s alpha_only=%d md5=%s order=exr\n",
                   path_.c_str(),
                   int(config_.alpha_only),
                   hash_.get_hex().c_str());
    }
    return {};
  }

 private:
  DeepStatus pack_bounded_row(const DeepCanonicalRowLease &rows)
  {
    if (rows.row_count() != 1 || rows.pixels().size() != raster_.width) {
      return DeepStatus::fail(DeepErrorCode::InvalidLayout, "Bounded writer requires one row");
    }
    auto &counts = *packed_counts_;
    const int64_t row = int64_t(rows.first_row()) - raster_.y;
    if (row < 0 || uint64_t(row) >= raster_.height) {
      return DeepStatus::fail(DeepErrorCode::InvalidLayout, "Bounded writer row outside frame");
    }
    if (borrow_row_) {
      struct Release {
        DeepEncodedRowLease &row;
        ~Release() { row.release(); }
      } release{packed_row_lease_};
      if (const auto status = production_.encoded_row(
              token_, size_t(row) * raster_.width, raster_.width, packed_row_lease_); !status)
        return status;
      return pack_bounded_pixels(packed_row_lease_.pixels());
    }
    std::pmr::vector<DeepEncodedPixelLease> leases(counts.get_allocator().resource());
    leases.resize(raster_.width);
    if (const auto status = production_.encoded_pixels(token_, size_t(row) * raster_.width, leases);
        !status)
      return status;
    return pack_bounded_pixels(std::span<DeepEncodedPixelLease>(leases));
  }

  template<typename Pixel> DeepStatus pack_bounded_pixels(const std::span<Pixel> leases)
  {
    const size_t channel_bytes = config_.precision == DeepCanonicalPrecision::Half ? 2 : 4;
    const size_t sample_bytes = 8 + (config_.alpha_only ? 1 : 4) * channel_bytes;
    auto &counts = *packed_counts_;
    auto &data = *packed_pixels_;
    counts.resize(size_t(raster_.width) * 4);
    const bool verify = deep_frame_pipeline_requested(
        getenv("CYCLES_EXPERIMENTAL_DEEP_FRAME_VERIFY_PAYLOAD"));
    const auto write_bits = [](unsigned char *dst, uint32_t value, const size_t width) {
      for (size_t b = 0; b < width; ++b) {
        dst[b] = static_cast<unsigned char>(value >> (8 * b));
      }
    };
    size_t total = 0, encoded = 0;
    const bool include_color = !config_.alpha_only;
    packed_masks_.resize(leases.size());
    for (size_t column = 0; column < leases.size(); ++column) {
      auto &lease = leases[column];
      if (!config_.alpha_only) {
        if (const auto status = lease.prepare_packed_color(); !status) {
          return status;
        }
      }
      if (lease.size() > DEEP_BOUNDED_K) {
        return DeepStatus::fail(DeepErrorCode::InvalidCount, "Bounded Deep pixel exceeds K32");
      }
      const uint32_t mask = lease.nonzero_mask(include_color);
      packed_masks_[column] = mask;
      const size_t count = std::popcount(mask);
      if (count > size_t(INT32_MAX) - total) {
        return DeepStatus::fail(DeepErrorCode::InvalidCount, "Bounded Deep row exceeds EXR counts");
      }
      encoded += lease.size();
      total += count;
      write_bits(counts.data() + column * 4, uint32_t(total), 4);
    }
    if (total > SIZE_MAX / sample_bytes) {
      return DeepStatus::fail(DeepErrorCode::InvalidCount, "Bounded Deep row byte count overflow");
    }
    data.resize(total * sample_bytes);
    encoded_samples_ += encoded;
    pruned_samples_ += encoded - total;
    size_t record = 0;
    const char *bulk_option = getenv("CYCLES_TEST_DEEP_BULK_ROW_PACK");
    const bool bulk = !bulk_option || deep_frame_pipeline_requested(bulk_option);
    const size_t z_base = total * (config_.alpha_only ? 1 : 4) * channel_bytes;
    for (size_t column = 0; column < leases.size(); ++column) {
      const auto &lease = leases[column];
      uint32_t mask = packed_masks_[column];
      const uint64_t count = std::popcount(mask);
      if (bulk && !verify) {
        if (!count) {
          continue;
        }
        unsigned char *channels[6] = {};
        channels[0] = data.data() + record * channel_bytes;
        if (!config_.alpha_only) {
          for (int c = 1; c <= 3; ++c) {
            channels[c] = data.data() + (total * c + record) * channel_bytes;
          }
        }
        channels[4] = data.data() + z_base + record * 4;
        channels[5] = data.data() + z_base + (total + record) * 4;
        if (count == lease.size()) {
          lease.pack_channels(channels, include_color);
        }
        else {
          lease.pack_channels_masked(channels, include_color, mask);
        }
        record += count;
        continue;
      }
      if (verify) {
        hash_.append(reinterpret_cast<const uint8_t *>(&count), sizeof(count));
      }
      for (; mask; mask &= mask - 1, ++record) {
        uint32_t bits[6] = {};
        lease.packed_bits(std::countr_zero(mask), include_color, bits);
        write_bits(data.data() + record * channel_bytes, bits[0], channel_bytes);
        if (!config_.alpha_only) {
          for (int c = 1; c <= 3; ++c) {
            write_bits(data.data() + (total * c + record) * channel_bytes, bits[c], channel_bytes);
          }
        }
        write_bits(data.data() + z_base + record * 4, bits[4], 4);
        write_bits(data.data() + z_base + (total + record) * 4, bits[5], 4);
        if (verify) {
          const auto color = [&](int c) {
            return channel_bytes == 2 ? half_to_float(half(uint16_t(bits[c]))) :
                                        std::bit_cast<float>(bits[c]);
          };
          const float values[] = {color(3), color(2), color(1), color(0),
                                  std::bit_cast<float>(bits[4]), std::bit_cast<float>(bits[5])};
          hash_.append(reinterpret_cast<const uint8_t *>(values + (config_.alpha_only ? 3 : 0)),
                       (config_.alpha_only ? 3 : 6) * sizeof(float));
        }
      }
    }
    if (record != total) {
      return DeepStatus::fail(DeepErrorCode::InvalidCount, "Bounded Deep row count changed in packing");
    }
    return {};
  }

  void charge(size_t count, size_t item_bytes)
  {
    if (count > std::numeric_limits<size_t>::max() / item_bytes)
      throw std::bad_alloc();
    const size_t bytes = count * item_bytes;
    if (bytes > std::numeric_limits<size_t>::max() - charged_)
      throw std::bad_alloc();
    budget_.acquire(bytes);
    charged_ += bytes;
  }
  DeepFrameProduction &production_;
  DeepMemoryBudget budget_;
  DeepRasterWindow raster_;
  BlenderDeepFrameWindow display_;
  std::string path_;
  int compression_;
  DeepCanonicalEncodeConfig config_;
  DeepConsumerToken token_;
  DeepCanonicalEncodeReport proof_;
  size_t charged_ = 0;
  uint64_t encoded_knots_ = 0, encoded_samples_ = 0, pruned_samples_ = 0;
  uint64_t merge_candidates_ = 0, merged_intervals_ = 0;
  double maximum_error_[4] = {}, maximum_budget_ratio_[4] = {};
  std::vector<std::vector<blender::DeepSample>> *row_ = nullptr;
  std::pmr::vector<unsigned char> *packed_counts_ = nullptr, *packed_pixels_ = nullptr;
  DeepEncodedRowLease packed_row_lease_;
  std::vector<uint32_t, DeepBudgetAllocator<uint32_t>> packed_masks_;
  bool borrow_row_ = true;
  MD5Hash hash_;
  bool file_finished_ = false;
};
}  // namespace

BlenderDeepFrameOutput::BlenderDeepFrameOutput(std::shared_ptr<DeepFrameProduction> production,
                                               DeepRasterWindow raster,
                                               BlenderDeepFrameWindow display,
                                               const DeepFrameKey &frame)
    : frame_(frame), production_(std::move(production)), raster_(raster), display_(display)
{
}

bool BlenderDeepFrameOutput::write(const char *path,
                                   int compression,
                                   bool half,
                                   bool alpha_only,
                                   float depth,
                                   float alpha,
                                   std::string &error,
                                   const uint64_t consumer_key,
                                   const char * /*view_name*/)
{
  uint64_t key = consumer_key;
  if (!key) {
    for (const auto &consumer : production_->frozen_consumers()) {
      if (consumer.source_key == DEEP_DIRECT_CONSUMER_KEY)
        continue;
      if (key) {
        error = "Deep output requires its frozen node instance key";
        return false;
      }
      key = consumer.source_key;
    }
  }
  return write_consumer(key, path, compression, half, alpha_only, depth, alpha, error);
}
bool BlenderDeepFrameOutput::write_direct(
    const char *path, int compression, bool half, float depth, float alpha, std::string &error)
{
  return write_consumer(
      DEEP_DIRECT_CONSUMER_KEY, path, compression, half, false, depth, alpha, error);
}
bool BlenderDeepFrameOutput::write_consumer(uint64_t source_key,
                                            const char *path,
                                            int compression,
                                            bool half,
                                            bool alpha_only,
                                            float depth,
                                            float alpha,
                                            std::string &error)
{
  const DeepCanonicalEncodeConfig config = deep_production_encode_config(
      depth,
      alpha,
      half ? DeepCanonicalPrecision::Half : DeepCanonicalPrecision::Float,
      alpha_only);
  DeepConsumerToken token;
  const DeepStatus status = production_->consumer_token(source_key, config, compression, token);
  if (!status) {
    production_->pipeline().check_producer_status(status);
    error = status.message;
    return false;
  }
  return write_checked_consumer(path, compression, config, token, error);
}

bool BlenderDeepFrameOutput::write_checked_consumer(const char *path,
                                                    const int compression,
                                                    const DeepCanonicalEncodeConfig &config,
                                                    const DeepConsumerToken &token,
                                                    std::string &error)
{
  if (deep_frame_pipeline_requested(getenv("CYCLES_EXPERIMENTAL_DEEP_FRAME_VERIFY_PAYLOAD")))
    std::fprintf(stderr,
                 "DEEP_FRAME_RGB_BUDGET abs=%.17g rel=%.17g\n",
                 config.rgb_absolute_tolerance,
                 config.rgb_relative_tolerance);
  auto &pipeline = production_->pipeline();
  DeepStatus status;
  if (pipeline.consumer_closed(token.id))
    status = pipeline.replay_consumer(token.id);
  if (!status) {
    pipeline.check_producer_status(status);
    error = status.message;
    return false;
  }
  CanonicalFileSink sink(*production_, raster_, display_, path, compression, config, token);
  status = sink.write_file(token.id, pipeline);
  if (!status)
    status = pipeline.check_producer_status(status);
  if (status)
    status = pipeline.close_consumer(token.id, sink);
  if (status && pipeline.all_consumers_closed())
    status = pipeline.finish_frame();
  if (!status) {
    error = status.message;
    return false;
  }
  return true;
}

bool BlenderDeepFrameOutput::finish(std::string &error)
{
  if (production_->pipeline().state() == DeepFrameState::Complete)
    return true;
  DeepStatus status = production_->pipeline().finish_frame();
  if (!status)
    error = status.message;
  return bool(status);
}


bool BlenderDeepFrameOutput::finish_inputs(std::string &error)
{
  if (inputs_sealed_)
    return true;
  const DeepStatus status = production_->finish_inputs();
  if (!status)
    error = status.message;
  inputs_sealed_ = bool(status);
  return inputs_sealed_;
}
void BlenderDeepFrameOutput::cancel()
{
  production_->pipeline().cancel();
}

BlenderDeepFrameViews::BlenderDeepFrameViews(
    const DeepMemoryBudget &budget, std::shared_ptr<BlenderDeepOutputAddresses> addresses)
    : views_(budget), addresses_(std::move(addresses))
{
}

bool BlenderDeepFrameViews::fail(const std::string &message, std::string &error)
{
  addresses_->fail(message, error);
  if (first_error_.empty())
    first_error_ = error;
  error = first_error_;
  cancel_unlocked();
  return false;
}
bool BlenderDeepFrameViews::register_view(const std::string_view name,
                                          const std::string_view path,
                                          std::string &error)
{
  const std::lock_guard lock(mutex_);
  if (path.size() >= size_t(1024))
    return fail("Deep view output path exceeds native bounds", error);
  std::array<char, 1024> checked = {};
  std::copy(path.begin(), path.end(), checked.begin());
  if (!path.empty())
    for (const auto &entry : views_.entries())
      if (entry.direct_path[0] &&
          blender::BLI_path_cmp_normalized(entry.direct_path.data(), checked.data()) == 0)
        return fail("Deep views require distinct direct output paths", error);
  const DeepStatus status = views_.register_view(name, path);
  return status ? true : fail(status.message, error);
}
bool BlenderDeepFrameViews::bind_view(const std::string_view name,
                                      std::shared_ptr<BlenderDeepFrameOutput> output,
                                      std::string &error)
{
  const std::lock_guard lock(mutex_);
  const DeepStatus status = views_.bind_view(name, std::move(output));
  return status ? true : fail(status.message, error);
}
bool BlenderDeepFrameViews::seal_inputs(std::string &error)
{
  const std::lock_guard lock(mutex_);
  if (!first_error_.empty())
    return fail(first_error_, error);
  for (const auto &entry : views_.entries())
    if (!entry.output || !entry.output->inputs_sealed())
      return fail("Every Deep view must complete inputs before output", error);
  const DeepStatus status = views_.seal();
  if (!status)
    return fail(status.message, error);
  sealed_ = true;
  return true;
}
bool BlenderDeepFrameViews::claim(const BlenderDeepFrameOutput &owner,
                                  const uint64_t consumer,
                                  const char *path,
                                  const int compression,
                                  const DeepCanonicalEncodeConfig &config,
                                  std::string &error)
{
  if (!sealed_ || !first_error_.empty())
    return fail("Deep view inputs are incomplete or failed", error);
  if (!addresses_->claim({owner.frame_key(), config, compression}, consumer, path, error))
    return fail(error, error);
  return true;
}
bool BlenderDeepFrameViews::write_direct(const int compression,
                                         const bool half,
                                         const float depth,
                                         const float alpha,
                                         std::string &error)
{
  const std::lock_guard lock(mutex_);
  for (const auto &entry : views_.entries()) {
    if (!entry.direct_path[0])
      continue;
    const auto config = deep_production_encode_config(
        depth, alpha, half ? DeepCanonicalPrecision::Half : DeepCanonicalPrecision::Float, false);
    if (!claim(*entry.output, DEEP_DIRECT_CONSUMER_KEY, entry.direct_path.data(), compression, config, error))
      return false;
    if (!entry.output->write_direct(
            entry.direct_path.data(), compression, half, depth, alpha, error))
      return fail(error, error);
  }
  return true;
}
bool BlenderDeepFrameViews::save_direct(const char *path,
                                        const int compression,
                                        const bool half,
                                        const float depth,
                                        const float alpha,
                                        std::string &error,
                                        const char *view_name)
{
  const std::lock_guard lock(mutex_);
  const auto output = views_.find(view_name ? view_name : "");
  if (!output) {
    error = "Missing exact native Deep view source";
    return false;
  }
  const auto config = deep_production_encode_config(
      depth, alpha, half ? DeepCanonicalPrecision::Half : DeepCanonicalPrecision::Float, false);
  DeepConsumerToken token;
  const DeepStatus status = output->production_->consumer_token(
      DEEP_DIRECT_CONSUMER_KEY, config, compression, token);
  if (!status) {
    error = status.message;
    return false;
  }
  if (!claim(*output, DEEP_DIRECT_CONSUMER_KEY, path, compression, config, error))
    return false;
  if (!output->write_checked_consumer(path, compression, config, token, error))
    return fail(error, error);
  return true;
}
bool BlenderDeepFrameViews::write(const char *path,
                                  const int compression,
                                  const bool half,
                                  const bool alpha_only,
                                  const float depth,
                                  const float alpha,
                                  std::string &error,
                                  const uint64_t consumer,
                                  const char *view_name)
{
  const std::lock_guard lock(mutex_);
  const auto output = views_.find(view_name ? view_name : "");
  if (!output)
    return fail("Missing exact native Deep view source", error);
  const auto config = deep_production_encode_config(
      depth, alpha, half ? DeepCanonicalPrecision::Half : DeepCanonicalPrecision::Float, alpha_only);
  if (!claim(*output, consumer, path, compression, config, error))
    return false;
  if (!output->write(path, compression, half, alpha_only, depth, alpha, error, consumer, view_name))
    return fail(error, error);
  return true;
}
bool BlenderDeepFrameViews::finish(std::string &error)
{
  const std::lock_guard lock(mutex_);
  if (!sealed_ || !first_error_.empty())
    return fail("Deep view collection cannot finish incomplete inputs", error);
  if (!addresses_->healthy(error))
    return fail(error, error);
  for (const auto &entry : views_.entries())
    if (!entry.output->finish(error))
      return fail(error, error);
  return true;
}
void BlenderDeepFrameViews::cancel()
{
  const std::lock_guard lock(mutex_);
  cancel_unlocked();
}
void BlenderDeepFrameViews::cancel_unlocked()
{
  for (const auto &entry : views_.entries())
    if (entry.output)
      entry.output->cancel();
}


BlenderDeepOutputAddresses::BlenderDeepOutputAddresses(const DeepMemoryBudget &budget)
    : addresses_(DeepBudgetAllocator<Address>(budget))
{
}
bool BlenderDeepOutputAddresses::fail_unlocked(const std::string &message, std::string &error)
{
  if (first_error_.empty())
    first_error_ = message;
  error = first_error_;
  return false;
}
bool BlenderDeepOutputAddresses::fail(const std::string &message, std::string &error)
{
  const std::lock_guard lock(mutex_);
  return fail_unlocked(message, error);
}
bool BlenderDeepOutputAddresses::healthy(std::string &error)
{
  const std::lock_guard lock(mutex_);
  if (first_error_.empty())
    return true;
  error = first_error_;
  return false;
}
bool BlenderDeepOutputAddresses::claim(const DeepOutputIdentity &identity,
                                      const uint64_t consumer,
                                      const char *path,
                                      std::string &error)
{
  const std::lock_guard lock(mutex_);
  if (!first_error_.empty()) {
    error = first_error_;
    return false;
  }
  if (!path || strlen(path) >= Address().path.size())
    return fail_unlocked("Deep consumer output path exceeds native bounds", error);
  for (const auto &address : addresses_) {
    if (blender::BLI_path_cmp_normalized(address.path.data(), path) != 0)
      continue;
    if (!(address.identity.frame == identity.frame))
      return fail_unlocked("Deep output path belongs to another frame/view/layer", error);
    if (address.consumer_key == consumer || address.identity == identity)
      return true; /* Every consumer still validates its token and performs a real checked write. */
    return fail_unlocked("Deep output path has incompatible consumer settings", error);
  }
  try {
    Address address;
    address.identity = identity;
    address.consumer_key = consumer;
    std::copy_n(path, strlen(path), address.path.begin());
    addresses_.push_back(address);
    return true;
  }
  catch (const std::bad_alloc &) {
    return fail_unlocked("Deep output-address registry quota exhausted", error);
  }
}

CCL_NAMESPACE_END
