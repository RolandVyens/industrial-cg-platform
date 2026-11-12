/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <memory>
#include <span>

#include "session/deep_page_store.h"

CCL_NAMESPACE_BEGIN

/* Exact opt-in; demand routing must never fall through to the legacy capture/reducer. */
bool deep_frame_pipeline_requested(const char *option);

struct DeepRasterWindow {
  int32_t x, y;
  uint32_t width, height;
};

struct DeepDomainCompletion {
  DeepFrameKey frame;
  uint64_t domain = 0, device_generation = 0, layout_generation = 0;
  uint32_t attempt = 0;
  uint64_t native_admitted = 0;
  uint64_t pending_primary = 0, pending_shadow = 0, pending_mnee = 0;
  bool producer_drained = false;
};

struct DeepSampleAdmission {
  DeepSampleKey sample;
  uint64_t ray = 0;
};

/* An adapter publishes normalized immutable curves only after checking the complete
 * population, optical coverage and source accounting. M0 tests inject that adapter;
 * these records are not a claim that native GPU capture or curve reduction is connected. */
struct DeepCanonicalPixel {
  int32_t x = 0, y = 0;
  uint64_t native_population = 0;
  DeepPageLease knots;
  std::shared_ptr<const DeepPageSequence> additional_knots;
};

enum class DeepFrameState { Idle, Capturing, InputsSealed, Complete, Failed, Cancelled };
struct DeepCanonicalRows;
struct DeepFrameOwnerState;

class DeepCanonicalRowLease {
 public:
  std::span<const DeepCanonicalPixel> pixels() const;
  const DeepFrameKey *frame_key() const;
  int32_t first_row() const;
  uint32_t row_count() const;
  explicit operator bool() const
  {
    return bool(rows_);
  }

 private:
  std::shared_ptr<const DeepCanonicalRows> rows_;
  uint32_t consumer_ = 0;
  uint64_t serial_ = 0;
  friend class DeepFramePipeline;
};

class DeepOutputSink {
 public:
  virtual ~DeepOutputSink() = default;
  virtual DeepStatus begin(const DeepFrameKey &frame, const DeepRasterWindow &window) = 0;
  virtual DeepStatus write_rows(const DeepCanonicalRowLease &rows) = 0;
  virtual DeepStatus finish_checked() = 0;
};

/* A single host owner per invocation/view/layer. All mutations are serialized; read-only
 * leases can outlive the owner. Caller joins asynchronous writers before consume/close.
 * begin_frame is one-shot: a new render requires a fresh owner and full FrameKey. */
class DeepFramePipeline {
 public:
  explicit DeepFramePipeline(size_t memory_bytes);
  explicit DeepFramePipeline(const DeepMemoryBudget &budget);
  ~DeepFramePipeline();
  DeepFramePipeline(const DeepFramePipeline &) = delete;
  DeepFramePipeline &operator=(const DeepFramePipeline &) = delete;

  DeepStatus begin_frame(const DeepFrameKey &frame,
                         const DeepRasterWindow &window,
                         std::span<const uint32_t> consumers);
  DeepPageStore *pages();
  DeepStatus check_producer_status(const DeepStatus &status);
  DeepStatus enable_device_encoding();
  DeepStatus commit_device_domain(const DeepDomainCompletion &domain);
  DeepStatus close_device_encoded_tile(const DeepFrameKey &frame, uint64_t final_tile_token,
                                        const DeepRasterWindow &tile,
                                        std::span<const DeepCanonicalPixel> pixels);
  DeepStatus close_bounded_device_tile(const DeepFrameKey &frame,
                                       uint64_t token,
                                       const DeepRasterWindow &tile);
  DeepStatus bind_native_domain(const DeepDomainCompletion &domain,
                                const DeepRasterWindow &coverage);
  DeepStatus commit_native_domain(const DeepDomainCompletion &domain,
                                  std::span<const DeepSampleAdmission> samples,
                                  std::span<const DeepPageLease> pages);
  DeepStatus close_render_tile(const DeepFrameKey &frame,
                               uint64_t final_tile_token,
                               const DeepRasterWindow &tile,
                               std::span<const DeepCanonicalPixel> pixels);
  DeepStatus seal_frame_inputs();
  /* Called only after the producer has stored every frozen consumer encoding.
   * Existing leases retain their own page ownership. Future output rows carry identity only. */
  DeepStatus retire_canonical_tile(const DeepRasterWindow &tile);
  DeepStatus canonical_view(DeepCanonicalRowLease &lease);
  DeepStatus acquire_closed_rows(uint32_t consumer,
                                 int32_t first_row,
                                 uint32_t row_count,
                                 DeepCanonicalRowLease &lease);
  DeepStatus consume_rows(const DeepCanonicalRowLease &lease, DeepOutputSink &sink);
  DeepStatus close_consumer(uint32_t consumer, DeepOutputSink &sink);
  bool consumer_closed(uint32_t consumer) const;
  bool all_consumers_closed() const;
  DeepStatus replay_consumer(uint32_t consumer);
  DeepStatus finish_frame();
  DeepStatus cancel();
  DeepFrameState state() const;
  const DeepStatus &error() const;
  size_t bytes_used() const;
  const DeepMemoryBudget &memory_budget() const
  {
    return budget_;
  }

 private:
  DeepStatus fail(DeepErrorCode code, const std::string &message);
  DeepStatus active() const;
  DeepMemoryBudget budget_;
  std::shared_ptr<DeepFrameOwnerState> owner_;
  DeepStatus startup_error_;
};

CCL_NAMESPACE_END
