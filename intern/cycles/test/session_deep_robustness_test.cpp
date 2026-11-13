/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */

/* Host contracts crossing the real Production/Pipeline seam. These witnesses
 * do not qualify GPU DMA, multiple physical devices or native renderer paths. */
#include "device/deep_queue_memory.h"
#include "device/device.h"
#include "integrator/deep_bounded_publication.h"
#include "integrator/deep_bounded_plan.h"
#include "integrator/deep_bounded_layout.h"
#include "session/buffers.h"
#include "testing/testing.h"
#include "util/profiling.h"
#include "util/stats.h"
#include <array>
#include <cstring>
#include <thread>

CCL_NAMESPACE_BEGIN
namespace {

DeepFrameKey robust_frame() { return {912, 2, 3, 4, 2, 0, 1, 1}; }
const std::array<uint32_t, 1> ids{1};

DeepFrozenConsumer consumer()
{
  return {1, 17, 1, 0,
          deep_production_encode_config(0.01, 0.01, DeepCanonicalPrecision::Float, false)};
}

std::unique_ptr<DeepBoundedHostTile> tile(const DeepMemoryBudget &budget, const int x,
                                        const bool deferred = false)
{
  auto result = std::make_unique<DeepBoundedHostTile>(budget);
  result->window = {x, 0, 1, 1};
  auto payload = std::allocate_shared<DeepBoundedHostPayload>(
      DeepBudgetAllocator<DeepBoundedHostPayload>(budget), budget);
  payload->count = 1;
  payload->simple_rgb = true;
  payload->offsets.resize(2);
  payload->offsets[0] = 0;
  payload->offsets[1] = 1;
  payload->planes.resize(4 * sizeof(float));
  const float data[] = {2, 2, 0.5f, 0.5f};
  std::memcpy(payload->planes.data(), data, sizeof(data));
  result->consumers.push_back(payload);
  if (!deferred) result->beauty.assign({0.25f, 0.5f, 0.75f});
  return result;
}

template<typename T>
concept HasDirectReferenceTileClose = requires(T &owner, std::span<const uint32_t> counts) {
  owner.close_native_tile(0, 0, 1, 1, counts, 1.0f);
};
static_assert(!HasDirectReferenceTileClose<DeepFrameCaptureConsumer>);
static_assert(!HasDirectReferenceTileClose<DeepFrameProduction>);
static_assert(HasDirectReferenceTileClose<DeepReferenceCapture>);

template<typename T>
concept HasDirectReferenceBegin = requires(T &owner, const DeepNativeCaptureDomain &domain) {
  owner.begin_native_capture(domain);
};
template<typename T>
concept HasDirectReferenceComplete = requires(T &owner, DeepNativeCaptureBatch &&batch) {
  owner.complete_native_capture(std::move(batch));
};
static_assert(!HasDirectReferenceBegin<DeepFrameCaptureConsumer>);
static_assert(!HasDirectReferenceBegin<DeepFrameProduction>);
static_assert(HasDirectReferenceBegin<DeepReferenceCapture>);
static_assert(!HasDirectReferenceComplete<DeepFrameCaptureConsumer>);
static_assert(!HasDirectReferenceComplete<DeepFrameProduction>);
static_assert(HasDirectReferenceComplete<DeepReferenceCapture>);


TEST(DeepRobustness, bounded_exposes_no_reference_capture_before_or_after_enable)
{
  DeepFrameProduction bounded(robust_frame(), {0, 0, 1, 1}, 4 << 20, ids,
                               false, {}, DeepProductionRoute::Bounded);
  EXPECT_EQ(bounded.reference_capture(), nullptr);
  DeepFrameCaptureConsumer &active = bounded;
  EXPECT_EQ(active.reference_capture(), nullptr);
  const auto config = consumer();
  ASSERT_TRUE(bounded.configure_consumers({&config, 1}));
  ASSERT_TRUE(bounded.enable_cpu_processing());
  EXPECT_EQ(active.reference_capture(), nullptr);

  DeepFrameProduction reference(robust_frame(), {0, 0, 1, 1}, 4 << 20, ids);
  ASSERT_NE(reference.reference_capture(), nullptr);
  const uint32_t counts[] = {1};
  EXPECT_FALSE(reference.reference_capture()->close_native_tile(0, 0, 1, 1, counts, 1));
  EXPECT_EQ(reference.error().code, DeepErrorCode::PopulationMismatch);
  ASSERT_NE(reference.reference_capture(), nullptr);
  EXPECT_FALSE(reference.reference_capture()->close_native_tile(0, 0, 1, 1, counts, 1));
}

TEST(DeepRobustness, bounded_creation_has_no_reference_curve_array)
{
  DeepFrameProduction bounded(robust_frame(), {0, 0, 128, 128}, 4 << 20, ids,
                                false, {}, DeepProductionRoute::Bounded);
  DeepFrameProduction reference(robust_frame(), {0, 0, 128, 128}, 4 << 20, ids);
  ASSERT_TRUE(bounded.error());
  ASSERT_TRUE(reference.error());
  EXPECT_TRUE(bounded.curves().empty());
  EXPECT_EQ(reference.curves().size(), 128u * 128u);
  EXPECT_GE(reference.memory_budget().used() - bounded.memory_budget().used(),
            128u * 128u * sizeof(DeepNativeCurve));
}

TEST(DeepRobustness, bounded_rejects_unsupported_rgb_but_reference_accepts)
{
  auto config = consumer();
  config.config.rgb_relative_tolerance = 0.125;
  DeepFrameProduction bounded(robust_frame(), {0, 0, 1, 1}, 4 << 20, ids,
                                false, {}, DeepProductionRoute::Bounded);
  ASSERT_TRUE(bounded.configure_consumers({&config, 1}));
  EXPECT_FALSE(bounded.enable_gpu_processing());
  EXPECT_FALSE(bounded.error());
  DeepFrameProduction reference(robust_frame(), {0, 0, 1, 1}, 4 << 20, ids);
  EXPECT_TRUE(reference.configure_consumers({&config, 1}));
  EXPECT_TRUE(reference.error());
}

TEST(DeepRobustness, bounded_fixed_storage_scales_by_two_indices_only)
{
  auto config = consumer();
  DeepFrameProduction small(robust_frame(), {0, 0, 64, 1}, 4 << 20, ids,
                               false, {}, DeepProductionRoute::Bounded);
  DeepFrameProduction larger(robust_frame(), {0, 0, 128, 1}, 4 << 20, ids,
                                false, {}, DeepProductionRoute::Bounded);
  ASSERT_TRUE(small.configure_consumers({&config, 1}));
  ASSERT_TRUE(larger.configure_consumers({&config, 1}));
  EXPECT_EQ(larger.pipeline().bytes_used() - small.pipeline().bytes_used(), 64u * 16u);
  ASSERT_TRUE(larger.enable_cpu_processing());
  EXPECT_EQ(larger.pipeline().bytes_used() - small.pipeline().bytes_used(), 64u * 16u);
}

TEST(DeepRobustness, bounded_rows_generate_window_identity_and_keep_lease_alive)
{
  DeepCanonicalRowLease rows;
  {
    DeepFramePipeline pipeline(4 << 20);
    ASSERT_TRUE(pipeline.begin_frame(robust_frame(), {-2, 4, 2, 1}, ids, true));
    ASSERT_TRUE(pipeline.enable_device_encoding());
    ASSERT_TRUE(pipeline.close_bounded_device_tile(robust_frame(), 1, {-2, 4, 2, 1}));
    ASSERT_TRUE(pipeline.acquire_closed_rows(ids[0], 4, 1, rows));
    ASSERT_EQ(rows.pixels().size(), 2u);
  }
  EXPECT_EQ(rows.pixels()[0].x, -2);
  EXPECT_EQ(rows.pixels()[1].x, -1);
  EXPECT_EQ(rows.pixels()[1].y, 4);
  EXPECT_EQ(rows.pixels()[1].native_population, 0u);
  EXPECT_FALSE(rows.pixels()[1].knots);
}

TEST(DeepRobustness, actual_encoding_demand_and_asymmetric_retained_banks)
{
  auto config = consumer();
  config.config.alpha_only = true;
  config.config.precision = DeepCanonicalPrecision::Half;
  const auto plan = deep_bounded_encoding_plan({&config, 1}, DeepBoundedPolicy::ALPHA);
  EXPECT_EQ(plan.banks, 1u);
  DeepBoundedAdmission a;
  ASSERT_TRUE(DeepBoundedAdmission::calculate(128 * 128, 1, 1966080,
      DeepBoundedPolicy::ALPHA, true, 1, a, false, plan));
  EXPECT_EQ(a.encoded, 5308432u);
  EXPECT_EQ(a.encoded_b, 0u);
  EXPECT_LT(a.collector, 13u << 20);
  DeepBoundedDeviceAdmission device;
  device.native_states = 1966080;
  device.retained = a;
  /* A previously allocated B remains charged at its own capacity, without
   * pretending A also grew to B's size. */
  device.retained.encoded_b = 16u << 20;
  device.retained.known_total += device.retained.encoded_b;
  size_t resident;
  ASSERT_TRUE(deep_bounded_resident(a, device.retained, resident));
  EXPECT_EQ(resident, device.retained.known_total);
  EXPECT_TRUE(deep_bounded_choose_devices(128 * 128, 1, DeepBoundedPolicy::ALPHA,
      true, 1, 0, std::span(&device, 1), plan).roots);

  std::array<DeepFrozenConsumer, 2> mixed{config, config};
  mixed[0].config.alpha_only = false; /* RGBA HALF:8B component payload. */
  mixed[1].config.precision = DeepCanonicalPrecision::Float; /* Alpha FLOAT:4B. */
  const auto formats = deep_bounded_encoding_plan(mixed, DeepBoundedPolicy::PHYSICAL);
  EXPECT_EQ(formats.banks, 2u);
  EXPECT_EQ(formats.rgba_bytes, 8u);
  EXPECT_EQ(formats.summary_bytes, 0u);
}

TEST(DeepRobustness, two_producers_handoff_to_real_single_owner_exactly_once)
{
  DeepFrameProduction production(robust_frame(), {0, 0, 2, 1}, 4 << 20, ids,
                                   false, {}, DeepProductionRoute::Bounded);
  auto config = consumer();
  ASSERT_TRUE(production.configure_consumers({&config, 1}));
  ASSERT_TRUE(production.enable_cpu_processing());
  std::array<DeepBoundedPublicationSlot, 2> slots;
  std::array<bool, 2> staged{};
  const auto produce = [&](const int i) {
    auto value = tile(production.memory_budget(), i);
    staged[i] = slots[i].stage(value) && !value;
  };
  std::thread first(produce, 0), second(produce, 1);
  first.join(); second.join();
  ASSERT_TRUE(staged[0] && staged[1]);
  auto extra = tile(production.memory_budget(), 0);
  EXPECT_FALSE(slots[0].stage(extra));
  EXPECT_TRUE(extra);
  ASSERT_TRUE(slots[0].publish(production));
  ASSERT_TRUE(slots[1].publish(production));
  EXPECT_FALSE(slots[0].pending());
  EXPECT_TRUE(slots[0].publish(production));
  ASSERT_TRUE(production.finish_inputs());
  DeepConsumerToken token;
  ASSERT_TRUE(production.consumer_token(config.source_key, config.config, config.compression, token));
  DeepCanonicalEncodeReport report;
  ASSERT_TRUE(production.encoding_report(token, report));
  EXPECT_EQ(report.output_samples, 2u);
}

TEST(DeepRobustness, aggregate_admission_rejects_oversized_tile_and_accepts_smaller)
{
  std::array<DeepBoundedDeviceAdmission, 2> devices;
  for (auto &d : devices) {
    d.native_states = 524288;
    d.compact_identity = true;
    d.retained.control = 0;
  }
  const auto admit = [&](const uint32_t p) {
    return deep_bounded_choose_devices(p, 512, DeepBoundedPolicy::SIMPLE, true, 1,
                                       size_t(1) << 30, devices);
  };
  EXPECT_FALSE(admit(1024 * 1024).roots);
  EXPECT_TRUE(admit(128 * 128).roots);
  devices[1].physical_capacity = 1;
  EXPECT_FALSE(admit(128 * 128).roots);
  EXPECT_FALSE(deep_bounded_choose_devices(UINT32_MAX, UINT32_MAX,
      DeepBoundedPolicy::PHYSICAL, true, UINT32_MAX, SIZE_MAX, devices).roots);
}

TEST(DeepRobustness, retained_admission_does_not_charge_existing_arenas_twice)
{
  std::array<DeepBoundedDeviceAdmission, 2> devices;
  size_t used = 0;
  for (auto &d : devices) {
    d.native_states = 524288;
    d.compact_identity = true;
    ASSERT_TRUE(DeepBoundedAdmission::calculate(128 * 128, 1, d.native_states,
        DeepBoundedPolicy::SIMPLE, true, 1, d.retained, true));
    used += d.retained.known_total;
  }
  EXPECT_TRUE(deep_bounded_choose_devices(128 * 128, 1, DeepBoundedPolicy::SIMPLE,
                                           true, 1, 0, devices).roots);
  EXPECT_FALSE(deep_bounded_choose_devices(256 * 256, 1, DeepBoundedPolicy::SIMPLE,
      true, 1, 0, devices).roots);
}

TEST(DeepRobustness, family_owner_respects_format_and_alpha_dominance)
{
  std::array<DeepFrozenConsumer, 3> configs{consumer(), consumer(), consumer()};
  configs[0].config.alpha_only = true;
  configs[2].config.precision = DeepCanonicalPrecision::Half;
  EXPECT_EQ(deep_bounded_family_count(configs), 1u);
  EXPECT_EQ(deep_bounded_encoding_owner(configs, 0), 1u);
  EXPECT_EQ(deep_bounded_encoding_owner(configs, 1), 1u);
  EXPECT_EQ(deep_bounded_encoding_owner(configs, 2), 2u);
  configs[2].config.alpha_tolerance = 0.1;
  EXPECT_EQ(deep_bounded_family_count(configs), 2u);
}

TEST(DeepRobustness, queue_credit_outlives_frames_and_failed_charge_rolls_back)
{
  const size_t before = DeepQueueAuxiliaryBudget::resident_bytes();
  DeepQueueAuxiliaryBudget queue(128);
  {
    DeepQueueFrameReservation frame(1 << 20);
    EXPECT_EQ(frame.payload_quota() + frame.reserved_bytes(), frame.total_quota());
    EXPECT_EQ(queue.allocate_with(64, []() -> void * { return nullptr; }), nullptr);
    EXPECT_EQ(DeepQueueAuxiliaryBudget::resident_bytes(), before);
    EXPECT_THROW(queue.allocate_with(64, []() -> void * { throw std::bad_alloc(); }), std::bad_alloc);
    EXPECT_EQ(DeepQueueAuxiliaryBudget::resident_bytes(), before);
    ASSERT_TRUE(queue.acquire(64));
    EXPECT_FALSE(queue.acquire(65));
    EXPECT_EQ(DeepQueueAuxiliaryBudget::resident_bytes(), before + 64);
    queue.relinquish(64); // Same rollback used on CUDA allocation failure.
    EXPECT_EQ(DeepQueueAuxiliaryBudget::resident_bytes(), before);
    DeepQueueAuxiliaryBudget late(128);
    EXPECT_FALSE(late.acquire(1));
  }
  EXPECT_TRUE(queue.acquire(128));
  queue.relinquish(128);
  EXPECT_EQ(DeepQueueAuxiliaryBudget::resident_bytes(), before);
}

TEST(DeepRobustness, cancelled_real_production_keeps_failed_slot_owned_until_retirement)
{
  DeepFrameProduction production(robust_frame(), {0, 0, 1, 1}, 4 << 20, ids,
                                   false, {}, DeepProductionRoute::Bounded);
  auto config = consumer();
  ASSERT_TRUE(production.configure_consumers({&config, 1}));
  ASSERT_TRUE(production.enable_cpu_processing());
  production.pipeline().cancel();
  const size_t before = production.memory_budget().used();
  {
    DeepBoundedPublicationSlot slot;
    auto value = tile(production.memory_budget(), 0);
    ASSERT_TRUE(slot.stage(value));
    EXPECT_FALSE(slot.publish(production));
    EXPECT_TRUE(slot.pending());
  }
  EXPECT_EQ(production.memory_budget().used(), before);
}

TEST(DeepRobustness, late_queue_extends_frame_credit_and_rolls_back_when_other_frame_is_full)
{
  DeepQueueAuxiliaryBudget original(128);
  DeepQueueFrameReservation first(1 << 20);
  DeepMemoryBudget first_budget(first.payload_quota());
  first.bind_payload_budget(first_budget);
  const size_t first_base = first_budget.used();
  DeepQueueAuxiliaryBudget late(128);
  ASSERT_TRUE(late.acquire(1));
  EXPECT_EQ(first.extra_credit_bytes(), 128u);
  EXPECT_EQ(first_budget.reserved_credit(), 128u);
  EXPECT_EQ(first_budget.used(), first_base + 128);
  late.relinquish(1);
  EXPECT_TRUE(late.acquire(1));
  EXPECT_EQ(first_budget.used(), first_base + 128); // Reuse does not reserve twice.
  late.relinquish(1);
  {
    DeepQueueFrameReservation second(1 << 20);
    DeepMemoryBudget second_budget(second.payload_quota());
    second.bind_payload_budget(second_budget);
    /* Newest (second) frame reserves first; the older full frame rejects,
     * exercising rollback of an already-acquired second-frame credit. */
    const size_t occupied = first_budget.quota() - first_budget.used();
    first_budget.acquire(occupied);
    DeepQueueAuxiliaryBudget denied(128);
    const size_t credit_before = first_budget.reserved_credit();
    EXPECT_FALSE(denied.acquire(1));
    EXPECT_EQ(first_budget.reserved_credit(), credit_before);
    EXPECT_EQ(second_budget.reserved_credit(), 0u);
    first_budget.relinquish(occupied);
  }
}

TEST(DeepRobustness, deferred_standalone_lease_keeps_final_beauty_after_production)
{
  Stats stats;
  Profiler profiler;
  DeviceInfo info;
  info.type = DEVICE_CPU;
  info.id = "CPU";
  info.cpu_threads = 1;
  auto device = Device::create(info, stats, profiler, true);
  ASSERT_TRUE(device);
  RenderBuffers buffers(device.get());
  BufferParams params;
  params.width = params.height = params.window_width = params.window_height = 1;
  BufferPass beauty, samples;
  beauty.type = PASS_COMBINED; beauty.offset = 0;
  samples.type = PASS_SAMPLE_COUNT; samples.offset = 4;
  params.passes.push_back(beauty); params.passes.push_back(samples);
  params.exposure = 1.0f;
  params.update_passes();
  buffers.reset(params);
  const uint32_t population = 1;
  const float rgb[] = {0.25f, 0.5f, 0.75f};
  std::memcpy(buffers.buffer.data(), rgb, sizeof(rgb));
  std::memcpy(buffers.buffer.data() + params.get_pass_offset(PASS_SAMPLE_COUNT),
              &population, sizeof(population));
  DeepEncodedPixelLease survivor;
  {
    DeepFrameProduction production(robust_frame(), {0, 0, 1, 1}, 4 << 20, ids,
                                     false, {}, DeepProductionRoute::Bounded);
    auto config = consumer();
    ASSERT_TRUE(production.configure_consumers({&config, 1}));
    ASSERT_TRUE(production.enable_cpu_processing());
    production.defer_bounded_beauty();
    auto value = tile(production.memory_budget(), 0, true);
    ASSERT_TRUE(production.publish_bounded_tile(std::move(*value)));
    ASSERT_TRUE(production.set_final_bounded_beauty(buffers));
    ASSERT_TRUE(production.finish_inputs());
    DeepConsumerToken token;
    ASSERT_TRUE(production.consumer_token(config.source_key, config.config, config.compression, token));
    ASSERT_TRUE(production.encoded_pixel(token, 0, survivor));
  }
  DeepEncodedPixelLease moved = std::move(survivor);
  EXPECT_FLOAT_EQ(moved.sample(0).r, 0.25f);
  EXPECT_FLOAT_EQ(moved.sample(0).g, 0.5f);
  EXPECT_FLOAT_EQ(moved.sample(0).b, 0.75f);
  moved = {};
}

}  // namespace
CCL_NAMESPACE_END
