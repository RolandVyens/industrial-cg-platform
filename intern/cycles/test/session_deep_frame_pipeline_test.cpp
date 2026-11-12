/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>

#include "session/deep_frame_pipeline.h"
#include "session/deep_view_registry.h"
#include "integrator/deep_state_binding.h"
#include "device/device.h"
#include "util/profiling.h"
#include "util/stats.h"
#include "testing/testing.h"

CCL_NAMESPACE_BEGIN
namespace {
DeepFrameKey frame_key(const uint64_t invocation = 1)
{
  return {invocation, 2, 3, 4, 2, 0, 5, 6};
}

template<typename T> std::span<const uint8_t> bytes(const T &payload)
{
  return {reinterpret_cast<const uint8_t *>(&payload), sizeof(payload)};
}

DeepPageLease knot_page(DeepPageStore &store,
                        const uint64_t ray = 1,
                        const double transmission = 0.5)
{
  uint32_t page = 0;
  uint64_t handle = 0;
  DeepPageLease lease;
  DeepDepthKnot knot = {};
  knot.z = 2;
  knot.before[0] = 1;
  knot.after[0] = transmission;
  knot.after[1] = -2; /* Signed RGB is legitimate. */
  DeepRecordTag tag = {ray, ray, 0, 0, uint32_t(DeepRecordKind::DepthKnot), 0};
  EXPECT_TRUE(store.allocate(DeepRecordKind::DepthKnot, page));
  EXPECT_TRUE(store.reserve(page, handle));
  EXPECT_TRUE(store.write(handle, tag, bytes(knot)));
  EXPECT_TRUE(store.commit(handle));
  EXPECT_TRUE(store.seal(page, lease));
  EXPECT_TRUE(store.retire(page));
  return lease;
}

DeepSampleAdmission sample(const int32_t x,
                           const int32_t y,
                           const uint64_t ray,
                           const uint32_t native_sample = 17)
{
  DeepSampleAdmission result;
  result.ray = ray;
  result.sample.x = x;
  result.sample.y = y;
  result.sample.sample = native_sample;
  result.sample.subset_offset = 17;
  return result;
}

DeepDomainCompletion completion(const uint64_t domain, const size_t count)
{
  DeepDomainCompletion result;
  result.frame = frame_key();
  result.domain = domain;
  result.device_generation = 1;
  result.layout_generation = 1;
  result.native_admitted = count;
  result.producer_drained = true;
  return result;
}

DeepStatus commit_domain(DeepFramePipeline &pipeline,
                         const DeepDomainCompletion &domain,
                         const std::span<const DeepSampleAdmission> samples,
                         const std::span<const DeepPageLease> pages)
{
  if (samples.empty())
    return DeepStatus::fail(DeepErrorCode::InvalidLayout, "Test requires sample coverage");
  int32_t left = samples.front().sample.x, right = left;
  int32_t bottom = samples.front().sample.y, top = bottom;
  for (const auto &admission : samples) {
    left = std::min(left, admission.sample.x);
    right = std::max(right, admission.sample.x);
    bottom = std::min(bottom, admission.sample.y);
    top = std::max(top, admission.sample.y);
  }
  const DeepRasterWindow coverage{
      left, bottom, uint32_t(int64_t(right) - left + 1), uint32_t(int64_t(top) - bottom + 1)};
  if (const auto status = pipeline.bind_native_domain(domain, coverage); !status)
    return status;
  return pipeline.commit_native_domain(domain, samples, pages);
}

struct Sink : DeepOutputSink {
  int begins = 0, writes = 0, closes = 0;
  bool fail_write = false, fail_close = false;
  DeepStatus begin(const DeepFrameKey &, const DeepRasterWindow &) override
  {
    begins++;
    return {};
  }
  DeepStatus write_rows(const DeepCanonicalRowLease &rows) override
  {
    writes++;
    EXPECT_FALSE(rows.pixels().empty());
    return fail_write ? DeepStatus::fail(DeepErrorCode::OutputFailure, "injected short write") :
                        DeepStatus{};
  }
  DeepStatus finish_checked() override
  {
    closes++;
    return fail_close ? DeepStatus::fail(DeepErrorCode::OutputFailure, "injected close failure") :
                        DeepStatus{};
  }
};

void complete_one_pixel(DeepFramePipeline &pipeline)
{
  const std::array<uint32_t, 2> consumers{10, 20};
  ASSERT_TRUE(pipeline.begin_frame(frame_key(), {-96, -96, 1, 1}, consumers));
  const auto admission = sample(-96, -96, 1);
  ASSERT_TRUE(commit_domain(pipeline, completion(1, 1), {&admission, 1}, {}));
  DeepCanonicalPixel pixel{-96, -96, 1, knot_page(*pipeline.pages())};
  ASSERT_TRUE(pipeline.close_render_tile(frame_key(), 100, {-96, -96, 1, 1}, {&pixel, 1}));
  ASSERT_TRUE(pipeline.seal_frame_inputs());
}
}  // namespace

TEST(DeepPageStore, reserved_holes_keep_explicit_offsets_and_valid_slots)
{
  DeepPageStore store(frame_key(), 1 << 20, 512);
  uint32_t page;
  ASSERT_TRUE(store.allocate(DeepRecordKind::OpticalPiece, page));
  uint64_t handles[3];
  for (auto &handle : handles)
    ASSERT_TRUE(store.reserve(page, handle));
  DeepRecordTag tag = {7, 8, 0, 9, uint32_t(DeepRecordKind::OpticalPiece), 0};
  DeepOpticalPiece optical = {2, 5, {0, 1, 90}, 0, 0, 0};
  ASSERT_TRUE(store.write(handles[0], tag, bytes(optical)));
  ASSERT_TRUE(store.abort(handles[0]));
  ASSERT_TRUE(store.abort(handles[1]));
  ASSERT_TRUE(store.write(handles[2], tag, bytes(optical)));
  ASSERT_TRUE(store.commit(handles[2]));
  DeepPageLease lease;
  ASSERT_TRUE(store.seal(page, lease));
  EXPECT_EQ(lease.descriptor()->reserved, 3u);
  EXPECT_EQ(lease.descriptor()->written, 2u);
  EXPECT_EQ(lease.descriptor()->valid, 1u);
  EXPECT_EQ(lease.valid_handle_at(0), 0u);
  EXPECT_EQ(lease.valid_handle_at(2), handles[2]);
  DeepRecordTag read_tag;
  std::span<const uint8_t> payload;
  EXPECT_EQ(lease.read(handles[0], read_tag, payload).code, DeepErrorCode::InvalidReference);
  ASSERT_TRUE(lease.read(handles[2], read_tag, payload));
  EXPECT_EQ(read_tag.ordinal, 9u);
  EXPECT_EQ(payload.size(), sizeof(optical));
  DeepOpticalPiece read_optical;
  std::memcpy(&read_optical, payload.data(), sizeof(read_optical));
  EXPECT_EQ(read_optical.tau[2], 90);
}

TEST(DeepPageStore, sealed_canonical_page_releases_unused_capacity_without_changing_handles)
{
  DeepPageStore store(frame_key(), 1 << 20);
  uint32_t page;
  std::array<uint64_t, 3> handles;
  ASSERT_TRUE(store.allocate(DeepRecordKind::DepthKnot, page));
  const size_t open_bytes = store.bytes_used();
  for (auto &handle : handles)
    ASSERT_TRUE(store.reserve(page, handle));
  ASSERT_TRUE(store.abort(handles[1]));
  DeepDepthKnot knot = {};
  knot.z = 3;
  knot.before[0] = 1;
  knot.after[0] = .25;
  knot.after[1] = -2;
  DeepRecordTag tag = {7, 9, 11, 13, uint32_t(DeepRecordKind::DepthKnot), 0};
  for (int i : {0, 2}) {
    ASSERT_TRUE(store.write(handles[i], tag, bytes(knot)));
    ASSERT_TRUE(store.commit(handles[i]));
  }
  DeepPageLease lease;
  ASSERT_TRUE(store.seal(page, lease));
  EXPECT_LT(store.bytes_used(), open_bytes / 4);
  EXPECT_EQ(lease.descriptor()->reserved, 3u);
  EXPECT_EQ(lease.descriptor()->valid, 2u);
  EXPECT_EQ(lease.valid_handle_at(0), handles[0]);
  EXPECT_EQ(lease.valid_handle_at(1), 0u);
  EXPECT_EQ(lease.valid_handle_at(2), handles[2]);
  DeepRecordTag read_tag;
  std::span<const uint8_t> payload;
  ASSERT_TRUE(lease.read(handles[2], read_tag, payload));
  EXPECT_EQ(std::memcmp(&read_tag, &tag, sizeof(tag)), 0);
  EXPECT_EQ(std::memcmp(payload.data(), &knot, sizeof(knot)), 0);
  const uint8_t *published = payload.data();
  DeepPageLease second;
  ASSERT_TRUE(store.seal(page, second));
  ASSERT_TRUE(second.read(handles[2], read_tag, payload));
  EXPECT_EQ(payload.data(), published); /* A published lease is immutable. */
  ASSERT_TRUE(store.retire(page));
  lease = {};
  ASSERT_TRUE(second.read(handles[0], read_tag, payload));
  EXPECT_EQ(std::memcmp(payload.data(), &knot, sizeof(knot)), 0);
  EXPECT_EQ(second.read(handles[1], read_tag, payload).code, DeepErrorCode::InvalidReference);
}

TEST(DeepPageStore, payload_failure_cannot_be_hidden_by_abort_or_rewrite)
{
  DeepPageStore store(frame_key(), 1 << 20, 512);
  uint32_t page;
  uint64_t handle;
  ASSERT_TRUE(store.allocate(DeepRecordKind::OpticalPiece, page));
  ASSERT_TRUE(store.reserve(page, handle));
  DeepOpticalPiece optical = {2, 3, {0, std::numeric_limits<float>::quiet_NaN(), 0}, 0, 0, 0};
  DeepRecordTag tag = {1, 1, 0, 0, uint32_t(DeepRecordKind::OpticalPiece), 0};
  EXPECT_EQ(store.write(handle, tag, bytes(optical)).code, DeepErrorCode::InvalidPayload);
  ASSERT_TRUE(store.abort(handle));
  DeepPageLease lease;
  EXPECT_EQ(store.seal(page, lease).code, DeepErrorCode::InvalidPayload);
  EXPECT_FALSE(lease);
  EXPECT_EQ(store.publication_status().code, DeepErrorCode::InvalidPayload);
}

TEST(DeepPageStore, normalized_transmission_bounds_are_hard_without_rgb_clipping)
{
  for (int bad = 0; bad < 3; bad++) {
    DeepPageStore store(frame_key(), 1 << 20, 512);
    uint32_t page;
    uint64_t handle;
    ASSERT_TRUE(store.allocate(DeepRecordKind::DepthKnot, page));
    ASSERT_TRUE(store.reserve(page, handle));
    DeepDepthKnot knot = {};
    knot.before[0] = bad == 0 ? -1 : 0.5;
    knot.after[0] = bad == 1 ? 2 : 0;
    knot.slope_right[0] = bad == 2 ? 1 : 0;
    DeepRecordTag tag = {1, 1, 0, 0, uint32_t(DeepRecordKind::DepthKnot), 0};
    EXPECT_EQ(store.write(handle, tag, bytes(knot)).code, DeepErrorCode::InvalidPayload);
  }
  DeepPageStore store(frame_key(), 1 << 20, 512);
  EXPECT_TRUE(knot_page(store));
}

TEST(DeepPageStore, need_page_is_recoverable_and_metadata_and_leases_are_charged)
{
  DeepPageStore store(frame_key(), 1 << 20, 512);
  const size_t initial = store.bytes_used();
  uint32_t page;
  ASSERT_TRUE(store.allocate(DeepRecordKind::OpticalPiece, page));
  uint64_t handle;
  for (;;) {
    const DeepStatus status = store.reserve(page, handle);
    if (status.code == DeepErrorCode::NeedPage)
      break;
    ASSERT_TRUE(status);
    ASSERT_TRUE(store.abort(handle));
  }
  DeepPageLease lease;
  ASSERT_TRUE(store.seal(page, lease));
  const size_t allocated = store.bytes_used();
  EXPECT_GT(allocated, initial + 512);
  ASSERT_TRUE(store.retire(page));
  const size_t retained = store.bytes_used();
  EXPECT_GT(retained, initial + 512);
  lease = {};
  EXPECT_LT(store.bytes_used(), retained);
  /* Registry bucket storage remains accounted even after its last entry is retired. */
  EXPECT_GE(store.bytes_used(), initial);
  DeepPageStore tiny(frame_key(), 256, 512);
  EXPECT_EQ(tiny.allocate(DeepRecordKind::OpticalPiece, page).code, DeepErrorCode::BudgetExceeded);
  EXPECT_EQ(page, 0u);
  EXPECT_LE(tiny.bytes_used(), tiny.budget_bytes());
}

TEST(DeepFramePipeline, source_pages_retire_only_after_all_domain_pixels_close)
{
  DeepFramePipeline pipeline(1 << 20);
  const std::array<uint32_t, 2> consumers{10, 20};
  ASSERT_TRUE(pipeline.begin_frame(frame_key(), {0, 0, 3, 1}, consumers));
  auto a = sample(0, 0, 1), b = sample(1, 0, 3);
  uint32_t page;
  uint64_t handle;
  auto &store = *pipeline.pages();
  ASSERT_TRUE(store.allocate(DeepRecordKind::OpticalPiece, page));
  ASSERT_TRUE(store.reserve(page, handle));
  DeepOpticalPiece optical = {1, 2, {1, 1, 1}, 0, 0, 0};
  DeepRecordTag tag = {a.ray, 1, 0, 0, uint32_t(DeepRecordKind::OpticalPiece), 0};
  ASSERT_TRUE(store.write(handle, tag, bytes(optical)));
  ASSERT_TRUE(store.commit(handle));
  DeepPageLease source;
  ASSERT_TRUE(store.seal(page, source));
  ASSERT_TRUE(store.retire(page));
  std::array<DeepSampleAdmission, 2> admissions{a, b};
  ASSERT_TRUE(commit_domain(pipeline, completion(1, 2), admissions, {&source, 1}));
  source = {};
  const size_t capture_bytes = store.bytes_used();
  DeepCanonicalPixel left{0, 0, 1, knot_page(store, 1)};
  ASSERT_TRUE(pipeline.close_render_tile(frame_key(), 100, {0, 0, 1, 1}, {&left, 1}));
  EXPECT_GE(store.bytes_used(), capture_bytes);
  DeepCanonicalPixel right{1, 0, 1, knot_page(store, 3)};
  ASSERT_TRUE(pipeline.close_render_tile(frame_key(), 101, {1, 0, 1, 1}, {&right, 1}));
  EXPECT_LT(store.bytes_used(), capture_bytes / 4);
  /* The gap between consumed ray IDs remains a valid future admission. */
  auto c = sample(2, 0, 2);
  ASSERT_TRUE(commit_domain(pipeline, completion(2, 1), {&c, 1}, {}));
  DeepCanonicalPixel last{2, 0, 1, knot_page(store, 2)};
  ASSERT_TRUE(pipeline.close_render_tile(frame_key(), 102, {2, 0, 1, 1}, {&last, 1}));
  ASSERT_TRUE(pipeline.seal_frame_inputs());
  Sink direct, alpha;
  for (const uint32_t consumer : consumers) {
    DeepCanonicalRowLease rows;
    ASSERT_TRUE(pipeline.acquire_closed_rows(consumer, 0, 1, rows));
    ASSERT_EQ(rows.pixels().size(), 3u);
    EXPECT_EQ(rows.pixels()[0].native_population, 1u);
    auto &sink = consumer == 10 ? direct : alpha;
    ASSERT_TRUE(pipeline.consume_rows(rows, sink));
    ASSERT_TRUE(pipeline.close_consumer(consumer, sink));
  }
  ASSERT_TRUE(pipeline.finish_frame());
}

TEST(DeepFramePipeline, consumed_ray_identity_cannot_be_recycled_into_another_pixel)
{
  for (const uint64_t id : {uint64_t(1), std::numeric_limits<uint64_t>::max()}) {
    DeepFramePipeline pipeline(1 << 20);
    const std::array<uint32_t, 1> consumers{10};
    ASSERT_TRUE(pipeline.begin_frame(frame_key(), {0, 0, 2, 1}, consumers));
    auto first = sample(0, 0, id);
    ASSERT_TRUE(commit_domain(pipeline, completion(1, 1), {&first, 1}, {}));
    DeepCanonicalPixel pixel{0, 0, 1, knot_page(*pipeline.pages(), id)};
    ASSERT_TRUE(pipeline.close_render_tile(frame_key(), 100, {0, 0, 1, 1}, {&pixel, 1}));
    auto duplicate = sample(1, 0, id);
    EXPECT_EQ(commit_domain(pipeline, completion(2, 1), {&duplicate, 1}, {}).code,
              DeepErrorCode::DuplicateSample);
  }
}

TEST(DeepFramePipeline, complete_rows_are_consumed_before_frame_seal_and_both_outputs_close)
{
  DeepFramePipeline pipeline(8 << 20);
  const std::array<uint32_t, 2> consumers{10, 20};
  ASSERT_TRUE(pipeline.begin_frame(frame_key(), {-96, -96, 1, 2}, consumers));
  Sink direct, node;
  DeepCanonicalRowLease unavailable;
  EXPECT_EQ(pipeline.acquire_closed_rows(10, -95, 1, unavailable).code, DeepErrorCode::Incomplete);
  EXPECT_EQ(pipeline.state(), DeepFrameState::Capturing);
  for (int row = 0; row < 2; row++) {
    auto admission = sample(
        -96, -96 + row, row + 1, row ? std::numeric_limits<uint32_t>::max() : 17);
    admission.sample.disposition = row ? DeepCameraDisposition::InvalidCameraBlack :
                                         DeepCameraDisposition::ClearTransparent;
    ASSERT_TRUE(commit_domain(pipeline, completion(row + 1, 1), {&admission, 1}, {}));
    DeepCanonicalPixel pixel{
        -96, -96 + row, 1, knot_page(*pipeline.pages(), row + 1, row ? 0.0 : 1.0)};
    ASSERT_TRUE(
        pipeline.close_render_tile(frame_key(), row + 100, {-96, -96 + row, 1, 1}, {&pixel, 1}));
    for (const uint32_t consumer : consumers) {
      DeepCanonicalRowLease lease;
      ASSERT_TRUE(pipeline.acquire_closed_rows(consumer, pixel.y, 1, lease));
      ASSERT_TRUE(pipeline.consume_rows(lease, consumer == 10 ? direct : node));
    }
    EXPECT_EQ(pipeline.state(), DeepFrameState::Capturing);
  }
  ASSERT_TRUE(pipeline.close_consumer(10, direct));
  ASSERT_TRUE(pipeline.seal_frame_inputs());
  DeepCanonicalRowLease reference;
  ASSERT_TRUE(pipeline.canonical_view(reference));
  EXPECT_EQ(reference.pixels().size(), 2u);
  ASSERT_TRUE(pipeline.close_consumer(20, node));
  ASSERT_TRUE(pipeline.finish_frame());
  EXPECT_EQ(pipeline.state(), DeepFrameState::Complete);
  EXPECT_EQ(direct.begins, 1);
  EXPECT_EQ(direct.writes, 2);
  EXPECT_EQ(direct.closes, 1);
  EXPECT_EQ(node.closes, 1);
}

TEST(DeepFramePipeline, rng_and_subset_changes_do_not_duplicate_native_population)
{
  DeepFramePipeline pipeline(8 << 20);
  const uint32_t consumer = 1;
  ASSERT_TRUE(pipeline.begin_frame(frame_key(), {0, 0, 1, 1}, {&consumer, 1}));
  auto admission = sample(0, 0, 1);
  ASSERT_TRUE(commit_domain(pipeline, completion(1, 1), {&admission, 1}, {}));
  admission.ray = 2;
  admission.sample.rng_offset = 999;
  admission.sample.subset_offset = 0;
  EXPECT_EQ(commit_domain(pipeline, completion(2, 1), {&admission, 1}, {}).code,
            DeepErrorCode::DuplicateSample);
  EXPECT_EQ(pipeline.finish_frame().code, DeepErrorCode::DuplicateSample);
}

TEST(DeepFramePipeline, late_shadow_and_native_count_mismatch_prevent_publication)
{
  for (const bool late_shadow : {false, true}) {
    DeepFramePipeline pipeline(8 << 20);
    const uint32_t consumer = 1;
    ASSERT_TRUE(pipeline.begin_frame(frame_key(), {0, 0, 1, 1}, {&consumer, 1}));
    auto domain = completion(1, late_shadow ? 1 : 2);
    domain.pending_shadow = late_shadow ? 1 : 0;
    const auto admission = sample(0, 0, 1);
    EXPECT_EQ(commit_domain(pipeline, domain, {&admission, 1}, {}).code,
              late_shadow ? DeepErrorCode::Incomplete : DeepErrorCode::PopulationMismatch);
    EXPECT_EQ(pipeline.state(), DeepFrameState::Failed);
  }
}

TEST(DeepFramePipeline, duplicate_final_pixel_and_changed_view_are_hard_errors)
{
  for (const bool changed_view : {false, true}) {
    DeepFramePipeline pipeline(8 << 20);
    const uint32_t consumer = 10;
    ASSERT_TRUE(pipeline.begin_frame(frame_key(), {-96, -96, 1, 1}, {&consumer, 1}));
    const auto admission = sample(-96, -96, 1);
    ASSERT_TRUE(commit_domain(pipeline, completion(1, 1), {&admission, 1}, {}));
    DeepCanonicalPixel pixel{-96, -96, 1, knot_page(*pipeline.pages())};
    ASSERT_TRUE(pipeline.close_render_tile(frame_key(), 100, {-96, -96, 1, 1}, {&pixel, 1}));
    auto frame = frame_key();
    if (changed_view)
      frame.view++;
    EXPECT_EQ(pipeline.close_render_tile(frame, 200, {-96, -96, 1, 1}, {&pixel, 1}).code,
              changed_view ? DeepErrorCode::InvalidFrame : DeepErrorCode::DuplicatePixel);
  }
}

TEST(DeepFramePipeline, checked_close_and_write_failures_remain_frame_errors)
{
  for (const bool close_failure : {false, true}) {
    DeepFramePipeline pipeline(8 << 20);
    complete_one_pixel(pipeline);
    Sink direct, node;
    DeepCanonicalRowLease a, b;
    ASSERT_TRUE(pipeline.acquire_closed_rows(10, -96, 1, a));
    ASSERT_TRUE(pipeline.acquire_closed_rows(20, -96, 1, b));
    ASSERT_TRUE(pipeline.consume_rows(a, direct));
    ASSERT_TRUE(pipeline.close_consumer(10, direct));
    node.fail_write = !close_failure;
    node.fail_close = close_failure;
    const auto write = pipeline.consume_rows(b, node);
    const auto status = close_failure ? pipeline.close_consumer(20, node) : write;
    EXPECT_EQ(status.code, DeepErrorCode::OutputFailure);
    EXPECT_EQ(pipeline.finish_frame().code, DeepErrorCode::OutputFailure);
    EXPECT_EQ(pipeline.state(), DeepFrameState::Failed);
  }
}

TEST(DeepFramePipeline, cancellation_rejects_late_writer_but_keeps_lease_bytes_alive)
{
  DeepCanonicalRowLease lease;
  {
    DeepFramePipeline pipeline(8 << 20);
    complete_one_pixel(pipeline);
    ASSERT_TRUE(pipeline.acquire_closed_rows(10, -96, 1, lease));
    EXPECT_EQ(pipeline.cancel().code, DeepErrorCode::Cancelled);
    Sink sink;
    EXPECT_EQ(pipeline.consume_rows(lease, sink).code, DeepErrorCode::Cancelled);
    EXPECT_EQ(sink.begins, 0);
    EXPECT_EQ(pipeline.finish_frame().code, DeepErrorCode::Cancelled);
  }
  ASSERT_EQ(lease.pixels().size(), 1u);
  const auto page = lease.pixels()[0].knots;
  DeepRecordTag tag;
  std::span<const uint8_t> payload;
  EXPECT_TRUE(page.read(page.valid_handle_at(0), tag, payload));
}

TEST(DeepFramePipeline, overflowing_raster_window_is_rejected_before_allocation)
{
  DeepFramePipeline pipeline(8 << 20);
  const uint32_t consumer = 1;
  EXPECT_EQ(
      pipeline
          .begin_frame(frame_key(), {std::numeric_limits<int32_t>::max(), 0, 2, 1}, {&consumer, 1})
          .code,
      DeepErrorCode::InvalidLayout);
  EXPECT_EQ(pipeline.state(), DeepFrameState::Failed);
}

TEST(DeepFramePipeline, unregistered_reserved_source_page_cannot_disappear_at_frame_seal)
{
  DeepFramePipeline pipeline(8 << 20);
  const uint32_t consumer = 10;
  ASSERT_TRUE(pipeline.begin_frame(frame_key(), {0, 0, 1, 1}, {&consumer, 1}));
  uint32_t pending_page;
  uint64_t pending_handle;
  ASSERT_TRUE(pipeline.pages()->allocate(DeepRecordKind::RadianceTerm, pending_page));
  ASSERT_TRUE(pipeline.pages()->reserve(pending_page, pending_handle));
  const auto admission = sample(0, 0, 1);
  ASSERT_TRUE(commit_domain(pipeline, completion(1, 1), {&admission, 1}, {}));
  DeepCanonicalPixel pixel{0, 0, 1, knot_page(*pipeline.pages())};
  ASSERT_TRUE(pipeline.close_render_tile(frame_key(), 100, {0, 0, 1, 1}, {&pixel, 1}));
  EXPECT_EQ(pipeline.seal_frame_inputs().code, DeepErrorCode::Incomplete);
  EXPECT_EQ(pipeline.state(), DeepFrameState::Failed);
}

TEST(DeepFramePipeline, producer_reference_error_and_repeated_consume_are_sticky)
{
  DeepFramePipeline pipeline(8 << 20);
  complete_one_pixel(pipeline);
  DeepCanonicalRowLease lease;
  ASSERT_TRUE(pipeline.acquire_closed_rows(10, -96, 1, lease));
  Sink sink;
  ASSERT_TRUE(pipeline.consume_rows(lease, sink));
  EXPECT_EQ(pipeline.consume_rows(lease, sink).code, DeepErrorCode::InvalidState);
  EXPECT_EQ(sink.writes, 1);
  EXPECT_EQ(pipeline.finish_frame().code, DeepErrorCode::InvalidState);
}

TEST(DeepFramePipeline, in_flight_domain_blocks_its_final_pixel_even_if_population_matches)
{
  DeepFramePipeline pipeline(8 << 20);
  const uint32_t consumer = 1;
  ASSERT_TRUE(pipeline.begin_frame(frame_key(), {0, 0, 1, 1}, {&consumer, 1}));
  const auto admission = sample(0, 0, 1);
  ASSERT_TRUE(commit_domain(pipeline, completion(1, 1), {&admission, 1}, {}));
  ASSERT_TRUE(pipeline.bind_native_domain(completion(2, 0), {0, 0, 1, 1}));
  DeepCanonicalPixel pixel{0, 0, 1, knot_page(*pipeline.pages())};
  EXPECT_EQ(pipeline.close_render_tile(frame_key(), 100, {0, 0, 1, 1}, {&pixel, 1}).code,
            DeepErrorCode::Incomplete);
  EXPECT_EQ(pipeline.seal_frame_inputs().code, DeepErrorCode::Incomplete);
}

TEST(DeepFramePipeline, unbound_and_changed_generation_domains_are_not_completion_receipts)
{
  for (const bool bound : {false, true}) {
    DeepFramePipeline pipeline(8 << 20);
    const uint32_t consumer = 1;
    ASSERT_TRUE(pipeline.begin_frame(frame_key(), {0, 0, 1, 1}, {&consumer, 1}));
    auto domain = completion(1, 1);
    if (bound) {
      ASSERT_TRUE(pipeline.bind_native_domain(domain, {0, 0, 1, 1}));
      domain.layout_generation++;
    }
    const auto admission = sample(0, 0, 1);
    EXPECT_EQ(pipeline.commit_native_domain(domain, {&admission, 1}, {}).code,
              DeepErrorCode::InvalidReference);
  }
}

TEST(DeepFramePipeline, another_rows_live_domain_does_not_block_completed_row_consumption)
{
  DeepFramePipeline pipeline(8 << 20);
  const uint32_t consumer = 1;
  ASSERT_TRUE(pipeline.begin_frame(frame_key(), {0, 0, 1, 2}, {&consumer, 1}));
  ASSERT_TRUE(pipeline.bind_native_domain(completion(2, 1), {0, 1, 1, 1}));
  const auto admission = sample(0, 0, 1);
  ASSERT_TRUE(commit_domain(pipeline, completion(1, 1), {&admission, 1}, {}));
  DeepCanonicalPixel pixel{0, 0, 1, knot_page(*pipeline.pages())};
  ASSERT_TRUE(pipeline.close_render_tile(frame_key(), 100, {0, 0, 1, 1}, {&pixel, 1}));
  DeepCanonicalRowLease lease;
  ASSERT_TRUE(pipeline.acquire_closed_rows(1, 0, 1, lease));
  Sink sink;
  ASSERT_TRUE(pipeline.consume_rows(lease, sink));
  EXPECT_EQ(pipeline.seal_frame_inputs().code, DeepErrorCode::Incomplete);
}

TEST(DeepFramePipeline, output_exceptions_are_sticky)
{
  struct ThrowingSink : Sink {
    DeepStatus finish_checked() override
    {
      throw std::runtime_error("injected close exception");
    }
  };
  DeepFramePipeline pipeline(8 << 20);
  complete_one_pixel(pipeline);
  DeepCanonicalRowLease lease;
  ASSERT_TRUE(pipeline.acquire_closed_rows(10, -96, 1, lease));
  ThrowingSink sink;
  ASSERT_TRUE(pipeline.consume_rows(lease, sink));
  EXPECT_EQ(pipeline.close_consumer(10, sink).code, DeepErrorCode::OutputFailure);
  EXPECT_EQ(pipeline.finish_frame().code, DeepErrorCode::OutputFailure);
}

TEST(DeepFramePipeline, reference_view_is_not_a_consumer_write_transaction)
{
  DeepFramePipeline pipeline(8 << 20);
  complete_one_pixel(pipeline);
  DeepCanonicalRowLease reference;
  ASSERT_TRUE(pipeline.canonical_view(reference));
  Sink sink;
  EXPECT_EQ(pipeline.consume_rows(reference, sink).code, DeepErrorCode::InvalidReference);
  EXPECT_EQ(sink.begins, 0);
}

TEST(DeepPageStore, foreign_handle_and_unfinished_commit_cannot_publish_success)
{
  DeepPageStore a(frame_key(), 1 << 20, 512), b(frame_key(2), 1 << 20, 512);
  auto lease = knot_page(a);
  EXPECT_EQ(b.commit(lease.valid_handle_at(0)).code, DeepErrorCode::InvalidReference);
  EXPECT_EQ(b.publication_status().code, DeepErrorCode::InvalidReference);
  uint32_t page;
  uint64_t handle;
  ASSERT_TRUE(a.allocate(DeepRecordKind::OpticalPiece, page));
  ASSERT_TRUE(a.reserve(page, handle));
  EXPECT_EQ(a.commit(handle).code, DeepErrorCode::InvalidState);
  ASSERT_TRUE(a.abort(handle));
  EXPECT_EQ(a.seal(page, lease).code, DeepErrorCode::InvalidState);
}

TEST(DeepFramePipeline, production_route_requires_exact_opt_in)
{
  EXPECT_FALSE(deep_frame_pipeline_requested(nullptr));
  EXPECT_FALSE(deep_frame_pipeline_requested(""));
  EXPECT_FALSE(deep_frame_pipeline_requested("0"));
  EXPECT_FALSE(deep_frame_pipeline_requested("10"));
  EXPECT_TRUE(deep_frame_pipeline_requested("1"));
}

TEST(DeepFramePipeline, output_replay_keeps_inputs_sealed_and_requires_fresh_row_transactions)
{
  for (int mutation = 0; mutation < 3; mutation++) {
    DeepFramePipeline pipeline(1 << 20);
    const std::array<uint32_t, 1> consumers{10};
    ASSERT_TRUE(pipeline.begin_frame(frame_key(), {0, 0, 1, 1}, consumers));
    const auto admission = sample(0, 0, 1);
    ASSERT_TRUE(commit_domain(pipeline, completion(1, 1), {&admission, 1}, {}));
    const DeepCanonicalPixel pixel{0, 0, 1, knot_page(*pipeline.pages())};
    ASSERT_TRUE(pipeline.close_render_tile(frame_key(), 1, {0, 0, 1, 1}, {&pixel, 1}));
    ASSERT_TRUE(pipeline.seal_frame_inputs());
    Sink first;
    DeepCanonicalRowLease old_rows;
    ASSERT_TRUE(pipeline.acquire_closed_rows(10, 0, 1, old_rows));
    ASSERT_TRUE(pipeline.consume_rows(old_rows, first));
    ASSERT_TRUE(pipeline.close_consumer(10, first));
    ASSERT_TRUE(pipeline.finish_frame());
    ASSERT_TRUE(pipeline.all_consumers_closed());
    ASSERT_TRUE(pipeline.replay_consumer(10));
    EXPECT_EQ(pipeline.state(), DeepFrameState::InputsSealed);
    EXPECT_FALSE(pipeline.consumer_closed(10));
    EXPECT_FALSE(pipeline.all_consumers_closed());
    Sink second;
    if (mutation == 1) {
      EXPECT_FALSE(pipeline.consume_rows(old_rows, second));
      EXPECT_EQ(pipeline.state(), DeepFrameState::Failed);
      continue;
    }
    if (mutation == 2) {
      EXPECT_FALSE(pipeline.bind_native_domain(completion(2, 1), {0, 0, 1, 1}));
      EXPECT_EQ(pipeline.state(), DeepFrameState::Failed);
      continue;
    }
    DeepCanonicalRowLease fresh;
    ASSERT_TRUE(pipeline.acquire_closed_rows(10, 0, 1, fresh));
    ASSERT_TRUE(pipeline.consume_rows(fresh, second));
    ASSERT_TRUE(pipeline.close_consumer(10, second));
    ASSERT_TRUE(pipeline.finish_frame());
    EXPECT_EQ(second.begins, 1);
    EXPECT_EQ(second.writes, 1);
    EXPECT_EQ(second.closes, 1);
    EXPECT_EQ(first.closes, 1);
  }
}

TEST(DeepFramePipeline, canonical_retirement_preserves_existing_borrowers_and_pixel_identity)
{
  DeepMemoryBudget budget(1 << 20);
  const size_t initial = budget.used();
  DeepCanonicalRowLease held;
  {
    DeepFramePipeline pipeline(budget);
    const std::array<uint32_t, 2> consumers{10, 20};
    ASSERT_TRUE(pipeline.begin_frame(frame_key(), {0, 0, 1, 1}, consumers));
    const auto admission = sample(0, 0, 1);
    ASSERT_TRUE(commit_domain(pipeline, completion(1, 1), {&admission, 1}, {}));
    DeepCanonicalPixel pixel{0, 0, 1, knot_page(*pipeline.pages())};
    ASSERT_TRUE(pipeline.close_render_tile(frame_key(), 1, {0, 0, 1, 1}, {&pixel, 1}));
    pixel = {};
    ASSERT_TRUE(pipeline.acquire_closed_rows(10, 0, 1, held));
    const size_t before = budget.used();
    ASSERT_TRUE(pipeline.retire_canonical_tile({0, 0, 1, 1}));
    EXPECT_LT(budget.used(), before); /* Page-owner map retired; held page remains. */
    ASSERT_TRUE(held.pixels()[0].knots);
    EXPECT_EQ(held.pixels()[0].native_population, 1u);
    ASSERT_TRUE(pipeline.seal_frame_inputs());
    DeepCanonicalRowLease unavailable;
    EXPECT_EQ(pipeline.canonical_view(unavailable).code, DeepErrorCode::InvalidState);
    DeepCanonicalRowLease metadata;
    ASSERT_TRUE(pipeline.acquire_closed_rows(20, 0, 1, metadata));
    ASSERT_EQ(metadata.pixels().size(), 1u);
    EXPECT_FALSE(metadata.pixels()[0].knots);
    EXPECT_EQ(metadata.pixels()[0].native_population, 1u);
  }
  ASSERT_TRUE(held.pixels()[0].knots);
  DeepRecordTag tag;
  std::span<const uint8_t> payload;
  ASSERT_TRUE(
      held.pixels()[0].knots.read(held.pixels()[0].knots.valid_handle_at(0), tag, payload));
  EXPECT_EQ(payload.size(), sizeof(DeepDepthKnot));
  EXPECT_GT(budget.used(), initial);
  held = {};
  EXPECT_EQ(budget.used(), initial);
}

namespace {
struct BindingProbe {
  std::array<bool, 2> drain_results = {true, true};
  bool copy_result = true;
  int drains = 0, copies = 0, events = 0;
  std::array<char, 3> order = {};

  bool drain() noexcept
  {
    if (events < int(order.size()))
      order[events] = 'D';
    ++events;
    const int attempt = drains++;
    return attempt < int(drain_results.size()) && drain_results[attempt];
  }
  bool copy() noexcept
  {
    if (events < int(order.size()))
      order[events] = 'C';
    ++events;
    ++copies;
    return copy_result;
  }
};

auto binding_for(BindingProbe &probe)
{
  return DeepStateBindingGuard([&probe]() noexcept { return probe.drain(); },
                               [&probe]() noexcept { return probe.copy(); });
}
}  // namespace

TEST(DeepFramePipeline, BindingFailedPreDrainDoesNotCopyOrRetry)
{
  BindingProbe probe;
  probe.drain_results[0] = false;
  {
    auto binding = binding_for(probe);
    static_assert(!std::is_copy_constructible_v<decltype(binding)>);
    EXPECT_FALSE(binding.restore());
    EXPECT_FALSE(binding.restore());
  }
  EXPECT_EQ(probe.drains, 1);
  EXPECT_EQ(probe.copies, 0);
  EXPECT_EQ(probe.events, 1);
}

TEST(DeepFramePipeline, BindingFailedCopyStillPostDrains)
{
  BindingProbe probe;
  probe.copy_result = false;
  {
    auto binding = binding_for(probe);
    EXPECT_FALSE(binding.restore());
  }
  EXPECT_EQ(probe.drains, 2);
  EXPECT_EQ(probe.copies, 1);
  EXPECT_EQ(probe.events, 3);
  EXPECT_EQ(probe.order, (std::array<char, 3>{'D', 'C', 'D'}));
}

TEST(DeepFramePipeline, BindingFailedPostDrainCannotAcknowledgeRestoration)
{
  BindingProbe probe;
  probe.drain_results[1] = false;
  {
    auto binding = binding_for(probe);
    EXPECT_FALSE(binding.restore());
    EXPECT_FALSE(binding.restore());
  }
  EXPECT_EQ(probe.drains, 2);
  EXPECT_EQ(probe.copies, 1);
  EXPECT_EQ(probe.events, 3);
}

TEST(DeepFramePipeline, BindingSuccessfulRestoreIsIdempotent)
{
  BindingProbe probe;
  {
    auto binding = binding_for(probe);
    EXPECT_TRUE(binding.restore());
    EXPECT_TRUE(binding.restore());
  }
  EXPECT_EQ(probe.drains, 2);
  EXPECT_EQ(probe.copies, 1);
  EXPECT_EQ(probe.events, 3);
}

TEST(DeepFramePipeline, BindingCleanupRetainsActualDeviceFirstError)
{
  Stats stats;
  Profiler profiler;
  auto device = Device::create(DeviceInfo(), stats, profiler, true);
  ASSERT_TRUE(device);
  device->set_error("first operation failure");
  BindingProbe probe;
  {
    DeepStateBindingGuard binding(
        [&]() noexcept { return probe.drain(); },
        [&]() noexcept {
          device->set_error("secondary cleanup diagnostic");
          return probe.copy();
        });
    EXPECT_TRUE(binding.restore());
  }
  EXPECT_TRUE(device->have_error());
  EXPECT_EQ(device->error_message(), "first operation failure");
  EXPECT_EQ(probe.drains, 2);
  EXPECT_EQ(probe.copies, 1);
  EXPECT_EQ(probe.order, (std::array<char, 3>{'D', 'C', 'D'}));
  EXPECT_FALSE(device->restore_integrator_state(nullptr, 0));
  EXPECT_EQ(device->error_message(), "first operation failure");
}

TEST(DeepFramePipeline, BindingEarlyReturnUsesJoinedCleanup)
{
  BindingProbe probe;
  const auto cancelled_operation = [&]() {
    auto binding = binding_for(probe);
    return false;
  };
  EXPECT_FALSE(cancelled_operation());
  EXPECT_EQ(probe.drains, 2);
  EXPECT_EQ(probe.copies, 1);
  EXPECT_EQ(probe.events, 3);
}

TEST(DeepFramePipeline, BindingExceptionUnwindsThroughJoinedCleanup)
{
  BindingProbe probe;
  EXPECT_THROW(
      {
        auto binding = binding_for(probe);
        throw std::runtime_error("operation callback failed");
      },
      std::runtime_error);
  EXPECT_EQ(probe.drains, 2);
  EXPECT_EQ(probe.copies, 1);
  EXPECT_EQ(probe.order, (std::array<char, 3>{'D', 'C', 'D'}));
}


TEST(DeepFramePipeline, ViewsPublishOnlyCompleteExactNames)
{
  DeepMemoryBudget budget(1 << 20);
  DeepViewRegistry<int> views(budget);
  ASSERT_TRUE(views.register_view("left", "left.exr"));
  ASSERT_TRUE(views.register_view("right", "right.exr"));
  auto left = std::allocate_shared<int>(DeepBudgetAllocator<int>(budget), 1);
  auto right = std::allocate_shared<int>(DeepBudgetAllocator<int>(budget), 2);
  ASSERT_TRUE(views.bind_view("left", left));
  EXPECT_FALSE(views.find("left"));
  EXPECT_FALSE(views.seal());
  ASSERT_TRUE(views.bind_view("right", right));
  ASSERT_TRUE(views.seal());
  EXPECT_EQ(views.find("left"), left);
  EXPECT_EQ(views.find("right"), right);
  EXPECT_FALSE(views.find(""));
  EXPECT_FALSE(views.find("Left"));
  EXPECT_FALSE(views.find("missing"));
}

TEST(DeepFramePipeline, ViewsRejectDuplicateMetadataWithoutMutation)
{
  DeepMemoryBudget budget(1 << 20);
  DeepViewRegistry<int> views(budget);
  ASSERT_TRUE(views.register_view("", "mono.exr"));
  const size_t used = budget.used();
  EXPECT_FALSE(views.register_view("", "different.exr"));
  EXPECT_EQ(views.entries().size(), 1u);
  EXPECT_EQ(budget.used(), used);
  auto mono = std::allocate_shared<int>(DeepBudgetAllocator<int>(budget), 9);
  ASSERT_TRUE(views.bind_view("", mono));
  ASSERT_TRUE(views.seal());
  EXPECT_EQ(views.find(""), mono);
}

TEST(DeepFramePipeline, ViewsRejectChildReplacementAliasAndLateRegistration)
{
  DeepMemoryBudget budget(1 << 20);
  DeepViewRegistry<int> views(budget);
  ASSERT_TRUE(views.register_view("left", "left.exr"));
  ASSERT_TRUE(views.register_view("right", "right.exr"));
  auto left = std::allocate_shared<int>(DeepBudgetAllocator<int>(budget), 1);
  auto right = std::allocate_shared<int>(DeepBudgetAllocator<int>(budget), 2);
  ASSERT_TRUE(views.bind_view("left", left));
  EXPECT_FALSE(views.bind_view("right", left));
  EXPECT_FALSE(views.bind_view("left", right));
  EXPECT_FALSE(views.bind_view("missing", right));
  EXPECT_FALSE(views.bind_view("right", {}));
  EXPECT_FALSE(views.register_view("late", "late.exr"));
  ASSERT_TRUE(views.bind_view("right", right));
  ASSERT_TRUE(views.seal());
  EXPECT_FALSE(views.bind_view("right", right));
  EXPECT_EQ(views.find("left"), left);
  EXPECT_EQ(views.find("right"), right);
}

TEST(DeepFramePipeline, ViewsBoundMetadataWithoutTruncationOrEmbeddedNul)
{
  DeepMemoryBudget budget(1 << 20);
  DeepViewRegistry<int> views(budget);
  const std::string name(63, 'v'), path(1023, 'p');
  ASSERT_TRUE(views.register_view(name, path));
  EXPECT_FALSE(views.register_view(std::string(64, 'n'), "p"));
  EXPECT_FALSE(views.register_view("other", std::string(1024, 'p')));
  EXPECT_FALSE(views.register_view(std::string("a\0b", 3), "p"));
  EXPECT_FALSE(views.register_view("other", std::string("a\0b", 3)));
  ASSERT_EQ(views.entries().size(), 1u);
  EXPECT_EQ(std::string(views.entries()[0].name.data()), name);
  EXPECT_EQ(std::string(views.entries()[0].direct_path.data()), path);
}

TEST(DeepFramePipeline, ViewsQuotaDenialDoesNotPublishPartialMetadata)
{
  DeepMemoryBudget budget(sizeof(DeepViewRegistry<int>::Entry) + 64);
  const size_t initial = budget.used();
  {
    DeepViewRegistry<int> views(budget);
    ASSERT_TRUE(views.register_view("first", "first.exr"));
    const size_t used = budget.used();
    EXPECT_FALSE(views.register_view("denied", "denied.exr"));
    EXPECT_EQ(views.entries().size(), 1u);
    EXPECT_EQ(budget.used(), used);
    EXPECT_LE(budget.peak(), budget.quota());
    EXPECT_FALSE(views.seal());
  }
  EXPECT_EQ(budget.used(), initial);
}

TEST(DeepFramePipeline, ViewsBorrowedChildRetainsItsChargedOwnerAfterRegistryRetires)
{
  DeepMemoryBudget budget(1 << 20);
  const size_t initial = budget.used();
  std::shared_ptr<int> borrowed;
  {
    DeepViewRegistry<int> views(budget);
    ASSERT_TRUE(views.register_view("view", "view.exr"));
    auto producer = std::allocate_shared<int>(DeepBudgetAllocator<int>(budget), 42);
    ASSERT_TRUE(views.bind_view("view", producer));
    ASSERT_TRUE(views.seal());
    borrowed = views.find("view");
    producer.reset();
  }
  ASSERT_TRUE(borrowed);
  EXPECT_EQ(*borrowed, 42);
  EXPECT_GT(budget.used(), initial);
  borrowed.reset();
  EXPECT_EQ(budget.used(), initial);
}

TEST(DeepFramePipeline, ViewsLayersUseOneSharedQuota)
{
  DeepMemoryBudget budget(2 * sizeof(DeepViewRegistry<int>::Entry) + 256);
  const size_t initial = budget.used();
  {
    const DeepMemoryBudget shared = budget;
    DeepViewRegistry<int> first_layer(budget), second_layer(shared);
    ASSERT_TRUE(first_layer.register_view("left", "layer1-left.exr"));
    ASSERT_TRUE(second_layer.register_view("left", "layer2-left.exr"));
    const size_t used = budget.used();
    EXPECT_FALSE(second_layer.register_view("right", "layer2-right.exr"));
    EXPECT_EQ(budget.used(), used);
    EXPECT_EQ(shared.used(), used);
    EXPECT_LE(budget.peak(), budget.quota());
  }
  EXPECT_EQ(budget.used(), initial);
}


TEST(DeepFramePipeline, OutputIdentityIncludesTheCompleteImmutableSource)
{
  const DeepOutputIdentity source{frame_key(), DeepCanonicalEncodeConfig(), 2};
  EXPECT_EQ(source, source);
  for (int field = 0; field < 8; ++field) {
    auto other = source;
    switch (field) {
      case 0: ++other.frame.invocation; break;
      case 1: ++other.frame.scene_revision; break;
      case 2: ++other.frame.camera; break;
      case 3: ++other.frame.policy_revision; break;
      case 4: ++other.frame.frame; break;
      case 5: ++other.frame.subframe_bits; break;
      case 6: ++other.frame.view; break;
      case 7: ++other.frame.view_layer; break;
    }
    EXPECT_NE(source, other);
  }
}

TEST(DeepFramePipeline, OutputIdentityIncludesEveryEncodingAndCodecSetting)
{
  const DeepOutputIdentity source{frame_key(), DeepCanonicalEncodeConfig(), 2};
  for (int field = 0; field < 7; ++field) {
    auto other = source;
    switch (field) {
      case 0: other.config.depth_tolerance *= 0.5; break;
      case 1: other.config.alpha_tolerance *= 0.5; break;
      case 2: other.config.rgb_absolute_tolerance *= 0.5; break;
      case 3: other.config.rgb_relative_tolerance = .002; break;
      case 4: other.config.precision = DeepCanonicalPrecision::Float; break;
      case 5: other.config.alpha_only = true; break;
      case 6: ++other.compression; break;
    }
    EXPECT_NE(source, other);
  }
}

CCL_NAMESPACE_END
