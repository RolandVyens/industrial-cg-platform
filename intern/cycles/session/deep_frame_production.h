/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <memory>
#include <span>

#include "integrator/deep_frame_capture.h"
#include "session/deep_canonical_encode.h"
#include "session/deep_frame_pipeline.h"
#include "session/deep_native_curve.h"

CCL_NAMESPACE_BEGIN

struct DeepFrameProductionStorage;
namespace deep_production_detail {
struct ReferenceStorage;
}
bool deep_extract_bounded_beauty(const RenderBuffers &buffers,
                                 const DeepRasterWindow &window,
                                 std::vector<float, DeepBudgetAllocator<float>> &beauty);

constexpr uint64_t DEEP_DIRECT_CONSUMER_KEY = UINT64_MAX;
struct DeepFrozenConsumer {
  uint32_t id = 0;
  uint64_t source_key = 0, config_version = 1;
  int compression = 0;
  DeepCanonicalEncodeConfig config;
};
struct DeepConsumerToken {
  DeepFrameKey frame;
  uint32_t id = 0;
  uint64_t config_version = 0;
};

struct DeepEncodedPayload {
  float r, g, b, a, z, z_back;
};
struct DeepBoundedHostPayload {
  explicit DeepBoundedHostPayload(const DeepMemoryBudget &budget, const bool overwrite = false)
      : offsets(budget, overwrite),
        planes(budget, overwrite)
  {
  }

  DeepReceiveBuffer<uint64_t> offsets;
  DeepReceiveBuffer<uint8_t> planes;
  size_t count = 0;
  bool half = false, physical_rgb = false, simple_rgb = false, dense_summary = false;
};

struct DeepBoundedHostTile {
  explicit DeepBoundedHostTile(const DeepMemoryBudget &budget)
      : consumers(DeepBudgetAllocator<std::shared_ptr<const DeepBoundedHostPayload>>(budget)),
        beauty(DeepBudgetAllocator<float>(budget))
  {
  }
  DeepRasterWindow window;
  std::vector<std::shared_ptr<const DeepBoundedHostPayload>,
              DeepBudgetAllocator<std::shared_ptr<const DeepBoundedHostPayload>>> consumers;
  std::vector<float, DeepBudgetAllocator<float>> beauty;
};
struct DeepEncodedTileStorage;
/* Immutable quantized payload, charged until the last borrower releases it. */
class DeepEncodedPixelView {
 public:
  std::span<const DeepEncodedPayload> samples() const;
  size_t size() const { return count_; }
  DeepEncodedPayload sample(size_t index) const;
  DeepStatus prepare_packed_color();
  /* A, B, G, R, Z, ZBack in target channel precision. */
  void packed_bits(size_t index, bool include_color, uint32_t (&channels)[6]) const;
  /* Already quantized bounded input to six disjoint target row planes. */
  void pack_channels(unsigned char *const (&channels)[6], bool include_color) const;
  explicit operator bool() const
  {
    return tile_ != nullptr;
  }

 private:
  const DeepEncodedTileStorage *tile_ = nullptr;
  const DeepBoundedHostPayload *bounded_ = nullptr;
  size_t offset_ = 0, count_ = 0, pixel_ = 0;
  float inverse_alpha_ = 0;
  int color_index_ = -1;
  const float *beauty_ = nullptr;
  friend class DeepFrameProduction;
};

/* Standalone pixels keep their original independent ownership contract. */
class DeepEncodedPixelLease : public DeepEncodedPixelView {
 public:
  explicit operator bool() const { return bool(owner_); }

 private:
  std::shared_ptr<const DeepEncodedTileStorage> owner_;
  std::shared_ptr<const std::vector<float, DeepBudgetAllocator<float>>> final_beauty_owner_;
  friend class DeepFrameProduction;
};

/* Reusable synchronous row workspace. Borrowed views are published only while
 * this row owns every referenced tile; release drops owners but keeps capacity. */
class DeepEncodedRowLease {
 public:
  explicit DeepEncodedRowLease(const DeepMemoryBudget &budget)
      : pixels_(DeepBudgetAllocator<DeepEncodedPixelView>(budget)),
        owners_(DeepBudgetAllocator<std::shared_ptr<const DeepEncodedTileStorage>>(budget))
  {
  }
  DeepEncodedRowLease(const DeepEncodedRowLease &) = delete;
  DeepEncodedRowLease &operator=(const DeepEncodedRowLease &) = delete;
  std::span<DeepEncodedPixelView> pixels() { return {pixels_.data(), published_}; }
  void release()
  {
    published_ = 0;
    owners_.clear();
    beauty_.reset();
  }

 private:
  std::vector<DeepEncodedPixelView, DeepBudgetAllocator<DeepEncodedPixelView>> pixels_;
  std::vector<std::shared_ptr<const DeepEncodedTileStorage>,
              DeepBudgetAllocator<std::shared_ptr<const DeepEncodedTileStorage>>> owners_;
  size_t published_ = 0;
  std::shared_ptr<const std::vector<float, DeepBudgetAllocator<float>>> beauty_;
  friend class DeepFrameProduction;
};

/* Single Host owner for CPU/OptiX bounded publication and explicit reference capture.
 * Route-specific capture, immutable output views and token identity have distinct lifetimes.
 * Reference capture alone reconstructs curves after native Tile closure.
 * All mutations are serialized by the PathTrace owner; no callback is a writer success. */
enum class DeepProductionRoute { Reference, Bounded };

class DeepFrameProduction : public DeepFrameCaptureConsumer {
 public:
  DeepFrameProduction(const DeepFrameKey &frame,
                      const DeepRasterWindow &window,
                      size_t memory_bytes,
                      std::span<const uint32_t> consumers,
                      bool retain_curve_sources = true,
                      const DeepNativeCurvePolicy &curve_policy = {},
                      DeepProductionRoute route = DeepProductionRoute::Reference);
  DeepFrameProduction(const DeepFrameKey &frame,
                      const DeepRasterWindow &window,
                      const DeepMemoryBudget &budget,
                      std::span<const uint32_t> consumers,
                      bool retain_curve_sources = true,
                      const DeepNativeCurvePolicy &curve_policy = {},
                      DeepProductionRoute route = DeepProductionRoute::Reference);
  ~DeepFrameProduction() override;
  DeepFrameProduction(const DeepFrameKey &frame,
                      const DeepRasterWindow &window,
                      const DeepMemoryBudget &host_budget,
                      const DeepMemoryBudget &device_budget,
                      std::span<const uint32_t> consumers,
                      bool retain_curve_sources = true,
                      int2 native_to_canonical = make_int2(0, 0),
                      const DeepNativeCurvePolicy &curve_policy = {},
                      DeepProductionRoute route = DeepProductionRoute::Reference);
  DeepFrameProduction(const DeepFrameProduction &) = delete;
  DeepFrameProduction &operator=(const DeepFrameProduction &) = delete;

  DeepFramePipeline &pipeline();
  const DeepFramePipeline &pipeline() const;
  const DeepStatus &error() const;
  DeepStatus configure_consumers(std::span<const DeepFrozenConsumer> consumers);
  std::span<const DeepFrozenConsumer> frozen_consumers() const;
  DeepStatus consumer_token(uint64_t source_key,
                            const DeepCanonicalEncodeConfig &config,
                            int compression,
                            DeepConsumerToken &token) const;
  DeepStatus validate_consumer_token(const DeepConsumerToken &token) const;
  DeepStatus encoded_pixel(const DeepConsumerToken &token,
                           size_t index,
                           DeepEncodedPixelLease &lease) const;
  /* Contiguous immutable range; validate consumer identity once per request. */
  DeepStatus encoded_pixels(const DeepConsumerToken &token,
                            size_t first,
                            std::span<DeepEncodedPixelLease> leases) const;
  DeepStatus encoded_row(const DeepConsumerToken &token,
                        size_t first,
                        size_t count,
                        DeepEncodedRowLease &row) const;
  DeepStatus encoding_report(const DeepConsumerToken &token,
                             DeepCanonicalEncodeReport &report) const;
  /* Row-major frame window. Entries become nonempty only at final tile closure. */
  std::span<const DeepNativeCurve> curves() const;
  const DeepMemoryBudget &memory_budget() const override;
  const DeepMemoryBudget &device_memory_budget() const override;
  bool enable_gpu_processing() override;
  bool enable_cpu_processing() override;
  bool uses_cpu_processing() const override
  {
    return bounded_processing_ && cpu_processing_;
  }
  bool uses_gpu_processing() const override
  {
    return bounded_processing_ && !cpu_processing_;
  }
  bool uses_bounded_processing() const override
  {
    return bounded_processing_;
  }
  std::span<const DeepFrozenConsumer> bounded_consumers() const override { return frozen_consumers(); }
  DeepCaptureDemand capture_demand() const override;
  void set_bounded_max_samples(const uint32_t samples) { bounded_max_samples_ = samples; }
  uint32_t bounded_max_samples() const override { return bounded_max_samples_; }
  bool publish_bounded_tile(DeepBoundedHostTile &&tile) override;
  void defer_bounded_beauty() override { deferred_beauty_ = true; }
  bool set_final_bounded_beauty(const RenderBuffers &buffers) override;
  bool awaits_final_bounded_beauty() const { return deferred_beauty_ && !final_beauty_; }
  void abort_native_capture(uint64_t domain_id, const std::string &reason) override;
  void cancel_native_capture(uint64_t domain_id) override;
  bool capture_cancelled() const override
  {
    return error().code == DeepErrorCode::Cancelled;
  }
  std::string capture_error_message() const override
  {
    return error().message;
  }
  DeepReferenceCapture *reference_capture() override;
  DeepStatus finish_inputs();
  DeepStatus encode_pixel(size_t index,
                          const DeepCanonicalEncodeConfig &config,
                          DeepCanonicalEncoded &encoded) const;

 private:
  friend struct deep_production_detail::ReferenceStorage;
  bool begin_reference_capture(const DeepNativeCaptureDomain &domain);
  bool complete_reference_capture(DeepNativeCaptureBatch &&batch);
  bool close_reference_native_tile(int x, int y, int width, int height,
                                   std::span<const uint32_t> counts, float exposure);
  DeepStatus encoded_pixel_for_consumer(size_t consumer,
                                       size_t index,
                                       DeepEncodedPixelLease &lease) const;
  void initialize(const DeepFrameKey &frame,
                  const DeepRasterWindow &window,
                  std::span<const uint32_t> consumers);
  bool fail(DeepErrorCode code, const std::string &message);
  DeepFramePipeline pipeline_;
  DeepMemoryBudget device_budget_;
  const int2 native_to_canonical_ = make_int2(0, 0);
  const bool retain_curve_sources_;
  const DeepProductionRoute route_;
  const DeepNativeCurvePolicy curve_policy_;
  std::shared_ptr<DeepFrameProductionStorage> storage_;
  bool bounded_processing_ = false, cpu_processing_ = false, deferred_beauty_ = false;
  bool enable_bounded_processing(bool cpu);
  std::shared_ptr<const std::vector<float, DeepBudgetAllocator<float>>> final_beauty_;
  uint32_t bounded_max_samples_ = 1;
};

CCL_NAMESPACE_END
