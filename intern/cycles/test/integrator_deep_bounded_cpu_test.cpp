/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */

#include <cmath>
#include <cstring>
#include <gtest/gtest.h>
#include <thread>

#include "device/device.h"
#include "integrator/deep_bounded_cpu.h"
#include "session/buffers.h"
#include "session/deep_frame_production.h"

namespace {
using std::isfinite;
#define __device__
#define __forceinline__ inline
inline uint32_t atomicOr(uint32_t *p, const uint32_t value)
{
  const uint32_t old = *p;
  *p |= value;
  return old;
}
#include "kernel/integrator/deep_bounded_records.h"
#undef __forceinline__
#undef __device__

template<bool Physical>
void point(const DeepBoundedBinding &b,
           const float depth,
           const float red = 3)
{
  DeepBoundedRecord<Physical> r = {};
  r.z = r.z_back = depth;
  r.alpha = 1;
  r.object = DEEP_BOUNDED_ELIGIBLE | 3u;
  r.shader = 2;
  r.normal = deep_bounded_pack_normal(0, 0, 1);
  if constexpr (Physical) {
    r.rgb[0] = red;
    r.rgb[1] = 2;
    r.rgb[2] = 1;
  }
  deep_bounded_store(b.raw, 0, r);
  *deep_bounded_ptr<uint32_t>(b.headers) = DEEP_BOUNDED_VALID | 1;
}
float value(const ccl::DeepBoundedHostPayload &p, const size_t offset)
{
  float v;
  std::memcpy(&v, p.planes.data() + offset, sizeof(v));
  return v;
}
}  // namespace

CCL_NAMESPACE_BEGIN
namespace {
class CPUConsumer final : public DeepFrameCaptureConsumer {
 public:
  CPUConsumer(const bool physical = false, const size_t work_bytes = 32u << 20)
      : host(64u << 20), work(work_bytes), demand{!physical, true}
  {
    DeepFrozenConsumer c;
    c.id = 1;
    c.config.precision = DeepCanonicalPrecision::Float;
    configs.push_back(c);
  }
  const DeepMemoryBudget &memory_budget() const override
  {
    return host;
  }
  const DeepMemoryBudget &device_memory_budget() const override
  {
    return work;
  }
  bool uses_cpu_processing() const override
  {
    return true;
  }
  std::span<const DeepFrozenConsumer> bounded_consumers() const override
  {
    return configs;
  }
  DeepCaptureDemand capture_demand() const override
  {
    return demand;
  }
  void abort_native_capture(uint64_t, const std::string &) override
  {
    legacy = true;
  }
  bool capture_cancelled() const override
  {
    return cancelled;
  }
  bool publish_bounded_tile(DeepBoundedHostTile &&tile) override
  {
    if (cancelled || reject_publish)
      return false;
    published = std::make_unique<DeepBoundedHostTile>(std::move(tile));
    return true;
  }
  DeepMemoryBudget host, work;
  DeepCaptureDemand demand;
  std::vector<DeepFrozenConsumer> configs;
  std::unique_ptr<DeepBoundedHostTile> published;
  bool legacy = false, cancelled = false, reject_publish = false;
};

class DeepBoundedCPU : public testing::Test {
 protected:
  void SetUp() override
  {
    DeviceInfo info;
    info.type = DEVICE_CPU;
    info.cpu_threads = 2;
    info.id = "CPU";
    device = Device::create(info, stats, profiler, true);
    ASSERT_TRUE(device);
    ASSERT_FALSE(device->have_error());
    params.width = params.window_width = 1;
    params.height = params.window_height = 1;
  }
  Stats stats;
  Profiler profiler;
  unique_ptr<Device> device;
  BufferParams params;
};

TEST_F(DeepBoundedCPU, BeautyUsesNativeCombinedEvenWhenDenoisedPassExists)
{
  params.exposure = 2;
  BufferPass noisy, denoised, samples;
  noisy.type = denoised.type = PASS_COMBINED;
  denoised.mode = PassMode::DENOISED;
  samples.type = PASS_SAMPLE_COUNT;
  noisy.offset = denoised.offset = samples.offset = 0;
  params.passes = {noisy, denoised, samples};
  params.update_passes();
  RenderBuffers buffers(device.get());
  buffers.reset(params);
  std::fill_n(buffers.buffer.data(), buffers.buffer.size(), 0.0f);
  float *data = buffers.buffer.data();
  const int native = params.get_pass_offset(PASS_COMBINED, PassMode::NOISY);
  const int filtered = params.get_pass_offset(PASS_COMBINED, PassMode::DENOISED);
  for (int c = 0; c < 3; ++c) {
    data[native + c] = float((c + 1) * 4);
    data[filtered + c] = 100.0f;
  }
  const uint32_t population = 4;
  std::memcpy(data + params.get_pass_offset(PASS_SAMPLE_COUNT), &population, sizeof(population));
  DeepMemoryBudget budget(1024);
  std::vector<float, DeepBudgetAllocator<float>> beauty{DeepBudgetAllocator<float>(budget)};
  ASSERT_TRUE(deep_extract_bounded_beauty(buffers, {0, 0, 1, 1}, beauty));
  ASSERT_EQ(beauty.size(), 3u);
  EXPECT_FLOAT_EQ(beauty[0], 2);
  EXPECT_FLOAT_EQ(beauty[1], 4);
  EXPECT_FLOAT_EQ(beauty[2], 6);

  std::fill_n(data + native, 3, 0.0f);
  ASSERT_TRUE(deep_extract_bounded_beauty(buffers, {0, 0, 1, 1}, beauty));
  EXPECT_FLOAT_EQ(beauty[0], 0);
  EXPECT_FLOAT_EQ(beauty[1], 0);
  EXPECT_FLOAT_EQ(beauty[2], 0);
}

TEST_F(DeepBoundedCPU, BeautyRejectsDenoisedOnlySource)
{
  BufferPass denoised, samples;
  denoised.type = PASS_COMBINED;
  denoised.mode = PassMode::DENOISED;
  samples.type = PASS_SAMPLE_COUNT;
  denoised.offset = samples.offset = 0;
  params.passes = {denoised, samples};
  params.update_passes();
  RenderBuffers buffers(device.get());
  buffers.reset(params);
  DeepMemoryBudget budget(1024);
  std::vector<float, DeepBudgetAllocator<float>> beauty{DeepBudgetAllocator<float>(budget)};
  EXPECT_FALSE(deep_extract_bounded_beauty(buffers, {0, 0, 1, 1}, beauty));
  EXPECT_TRUE(beauty.empty());
}

TEST_F(DeepBoundedCPU, CheckedAdmissionUsesCPUResourcesAndNoSampleHistory)
{
  CPUConsumer simple, physical(true);
  DeepCPUAdmission a, b;
  ASSERT_TRUE(deep_cpu_admission(simple, 64, 2, a));
  ASSERT_TRUE(deep_cpu_admission(physical, 64, 2, b));
  EXPECT_GT(b.arena, a.arena);
  EXPECT_GT(a.peak, a.arena);
  EXPECT_FALSE(deep_cpu_admission(simple, 0, 2, a));
  EXPECT_FALSE(deep_cpu_admission(simple, 1u << 30, 2, a));
  EXPECT_FALSE(deep_cpu_admission(simple, 1, 0, a));
}

TEST_F(DeepBoundedCPU, HighSampleSurfaceRemainsOneGroupedPointWithoutRGBCache)
{
  CPUConsumer c;
  DeepBoundedCPUWork w(*device, c);
  ASSERT_TRUE(w.configure(params, params, 2, false, 0));
  EXPECT_EQ(w.policy(), 2u);
  for (int sample = 0; sample < 1024; ++sample) {
    const auto *b = w.begin_sample(0, 0, 0, sample);
    ASSERT_TRUE(b);
    EXPECT_EQ(b->no_anchor, 0u);
    point<false>(*b, 8 + float(sample % 10) * .0001f);
    ASSERT_TRUE(w.end_sample(0));
  }
  ASSERT_TRUE(w.prepare_tile());
  ASSERT_TRUE(w.close_tile());
  ASSERT_TRUE(w.drain());
  ASSERT_TRUE(c.published);
  const auto &p = *c.published->consumers[0];
  EXPECT_EQ(p.count, 1u);
  EXPECT_TRUE(p.simple_rgb);
  EXPECT_FALSE(p.physical_rgb);
  EXPECT_FLOAT_EQ(value(p, 8), 1);
  EXPECT_FLOAT_EQ(value(p, 12), 1);
  EXPECT_FALSE(c.legacy);
}

TEST_F(DeepBoundedCPU, SingleCollectorFitsBoundedWorkBudget)
{
  CPUConsumer c(false, 1u << 20);
  params.width = params.window_width = 128;
  const size_t before = c.work.used();
  {
    DeepBoundedCPUWork w(*device, c);
    /* The former16 collectors exceed this quota before any sample history.
     * Exercise the real allocation/charge seam, not just a byte formula. */
    ASSERT_TRUE(w.configure(params, params, 2, false, 0));
    EXPECT_GT(c.work.used(), before);
    EXPECT_LT(c.work.used() - before, 1u << 20);
  }
  EXPECT_EQ(c.work.used(), before);
}

TEST_F(DeepBoundedCPU, SimpleZeroOpacityVolumeRetainsBeautyCarrierWithoutRGBCache)
{
  CPUConsumer c;
  DeepBoundedCPUWork w(*device, c);
  ASSERT_TRUE(w.configure(params, params, 2, false, 0));
  for (int sample = 0; sample < 32; ++sample) {
    const auto *b = w.begin_sample(0, 0, 0, sample);
    ASSERT_TRUE(b);
    EXPECT_EQ(b->no_anchor, 0u);
    DeepBoundedRecord<false> r = {};
    r.z = 9.0693483353f;
    r.z_back = 12.3965368271f;
    r.object = DEEP_BOUNDED_NONSURFACE | DEEP_BOUNDED_ELIGIBLE;
    deep_bounded_store(b->raw, 0, r);
    *deep_bounded_ptr<uint32_t>(b->headers) = DEEP_BOUNDED_VALID | 1;
    ASSERT_TRUE(w.end_sample(0));
  }
  ASSERT_TRUE(w.close_tile());
  ASSERT_TRUE(w.drain());
  ASSERT_TRUE(c.published);
  const auto &p = *c.published->consumers[0];
  ASSERT_EQ(p.count, 1u);
  EXPECT_TRUE(p.simple_rgb);
  EXPECT_FALSE(p.physical_rgb);
  EXPECT_FLOAT_EQ(value(p, 0), 9.0693483353f);
  EXPECT_FLOAT_EQ(value(p, 4), 12.3965368271f);
  EXPECT_FLOAT_EQ(value(p, 8), 0);
  /* Negative summary names the surviving real interval for final Beauty. */
  EXPECT_FLOAT_EQ(value(p, 12), -1);
}

TEST_F(DeepBoundedCPU, PhysicalRGBAndAlphaConsumerShareTypedPayload)
{
  CPUConsumer c(true);
  auto alpha = c.configs[0];
  alpha.id = 2;
  alpha.config.alpha_only = true;
  c.configs.push_back(alpha);
  DeepBoundedCPUWork w(*device, c);
  ASSERT_TRUE(w.configure(params, params, 2, true, 0));
  for (int s = 0; s < 128; ++s) {
    const auto *b = w.begin_sample(0, 0, 0, s);
    ASSERT_TRUE(b);
    ASSERT_NE(b->no_anchor, 0u);
    point<true>(*b, 8, (s & 1) ? 4 : 2);
    ASSERT_TRUE(w.end_sample(0));
  }
  ASSERT_TRUE(w.close_tile());
  ASSERT_TRUE(w.drain());
  const auto &a = c.published->consumers[0];
  const auto &b = c.published->consumers[1];
  ASSERT_EQ(a, b);
  ASSERT_EQ(a->count, 1u);
  EXPECT_TRUE(a->physical_rgb);
  EXPECT_FALSE(a->simple_rgb);
  EXPECT_NEAR(value(*a, 8), 3, 1e-5f);
  EXPECT_NEAR(value(*a, 12), 2, 1e-5f);
  EXPECT_NEAR(value(*a, 16), 1, 1e-5f);
  EXPECT_FLOAT_EQ(value(*a, 20), 1);
}

TEST_F(DeepBoundedCPU, ValidEmptyCameraDilutesPopulationButSkippedCameraDoesNot)
{
  CPUConsumer c;
  DeepBoundedCPUWork w(*device, c);
  ASSERT_TRUE(w.configure(params, params, 2, false, 0));
  const auto *b = w.begin_sample(0, 0, 0, 0);
  ASSERT_TRUE(b);
  point<false>(*b, 8);
  ASSERT_TRUE(w.end_sample(0));
  b = w.begin_sample(0, 0, 0, 1);
  ASSERT_TRUE(b);
  *deep_bounded_ptr<uint32_t>(b->headers) = DEEP_BOUNDED_VALID;
  ASSERT_TRUE(w.end_sample(0));
  b = w.begin_sample(0, 0, 0, 2);
  ASSERT_TRUE(b);
  /* No camera-success header: adaptive skip contributes no population. */
  ASSERT_TRUE(w.end_sample(0));
  ASSERT_TRUE(w.close_tile());
  ASSERT_TRUE(w.drain());
  const auto &p = *c.published->consumers[0];
  ASSERT_EQ(p.count, 1u);
  EXPECT_FLOAT_EQ(value(p, 8), .5f);
}

TEST_F(DeepBoundedCPU, UnanchoredPhysicalColourPreventsPublication)
{
  CPUConsumer c(true);
  DeepBoundedCPUWork w(*device, c);
  ASSERT_TRUE(w.configure(params, params, 2, false, 0));
  const auto *b = w.begin_sample(0, 0, 0, 0);
  ASSERT_TRUE(b);
  *deep_bounded_ptr<uint32_t>(b->headers) = DEEP_BOUNDED_VALID;
  deep_bounded_ptr<float>(b->no_anchor)[0] = 1;
  EXPECT_FALSE(w.end_sample(0));
  EXPECT_NE(w.error_flags() & DEEP_BOUNDED_NO_SOURCE, 0u);
  EXPECT_FALSE(w.good());
  EXPECT_FALSE(w.close_tile());
  EXPECT_FALSE(c.published);
}

TEST_F(DeepBoundedCPU, SmallBudgetDoesNotReduceCapacityOrLeak)
{
  CPUConsumer c(false, 1024);
  const size_t host = c.host.used(), work = c.work.used();
  {
    DeepBoundedCPUWork w(*device, c);
    EXPECT_FALSE(w.configure(params, params, 2, false, 0));
  }
  EXPECT_EQ(c.host.used(), host);
  EXPECT_EQ(c.work.used(), work);
  EXPECT_FALSE(c.published);
}

TEST_F(DeepBoundedCPU, CancellationRetiresUnpublishedWorkingMemory)
{
  CPUConsumer c;
  const size_t host = c.host.used(), work = c.work.used();
  {
    DeepBoundedCPUWork w(*device, c);
    ASSERT_TRUE(w.configure(params, params, 2, false, 0));
    const auto *b = w.begin_sample(0, 0, 0, 0);
    ASSERT_TRUE(b);
    point<false>(*b, 8);
    ASSERT_TRUE(w.end_sample(0));
    ASSERT_TRUE(w.close_tile());
    c.cancelled = true;
    EXPECT_FALSE(w.drain());
    EXPECT_FALSE(c.published);
  }
  EXPECT_EQ(c.host.used(), host);
  EXPECT_EQ(c.work.used(), work);
}

TEST_F(DeepBoundedCPU, TwoPixelOwnersHaveIndependentRawAndCollectorStorage)
{
  CPUConsumer c;
  params.width = params.window_width = 2;
  DeepBoundedCPUWork w(*device, c);
  ASSERT_TRUE(w.configure(params, params, 2, false, 0));
  auto run = [&](const uint32_t worker) {
    for (int s = 0; s < 64; ++s) {
      const auto *b = w.begin_sample(worker, int(worker), 0, s);
      EXPECT_TRUE(b);
      if (!b)
        return;
      point<false>(*b, 8 + float(worker));
      EXPECT_TRUE(w.end_sample(worker));
    }
  };
  std::thread a(run, 0), b(run, 1);
  a.join();
  b.join();
  ASSERT_TRUE(w.close_tile());
  ASSERT_TRUE(w.drain());
  const auto &p = *c.published->consumers[0];
  ASSERT_EQ(p.count, 2u);
  EXPECT_EQ(p.offsets[1], 1u);
  EXPECT_FLOAT_EQ(value(p, 0), 8);
  EXPECT_FLOAT_EQ(value(p, 4), 9);
}
}  // namespace
CCL_NAMESPACE_END
