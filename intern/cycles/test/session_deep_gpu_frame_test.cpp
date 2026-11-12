/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <random>

#include "testing/testing.h"

#include "device/device.h"
#include "device/memory.h"
#include "device/queue.h"
#include "integrator/deep_gpu_frame_work.h"
#include "util/profiling.h"
#include "util/stats.h"

CCL_NAMESPACE_BEGIN

namespace {
struct Input {
  vector<DeepGPUFrameRay> rays;
  vector<DeepGPUFrameOptical> optical;
  vector<DeepGPUFrameVisibility> visibility;
  vector<DeepGPUFrameRadiance> radiance;
  vector<DeepGPUFramePixel> pixels;
  vector<float> cuts;
};

/* Independent double oracle, deliberately test-only. Production enqueues the
 * device interface without constructing any host reference curve. */
double reference(const Input &in, const size_t pixel, const float depth, const bool before)
{
  const auto &p = in.pixels[pixel];
  double sum = 0;
  for (size_t r = p.ray_begin; r < p.ray_begin + p.ray_count; r++) {
    const auto &ray = in.rays[r];
    for (int channel = 0; channel < 3; channel++) {
      const auto tau_at = [&](const double z) {
        double tau = 0;
        for (size_t i = ray.optical_begin; i < ray.optical_begin + ray.optical_count; i++) {
          const auto &part = in.optical[i];
          const double fraction = std::min(1.0, std::max(0.0, (z - part.z) /
                                                               (double(part.z_back) - part.z)));
          const auto &t = part.tau;
          tau += fraction * (channel == 0 ? t.x : channel == 1 ? t.y : t.z);
        }
        return tau;
      };
      double q = 1, holes = 0;
      for (size_t i = ray.visibility_begin; i < ray.visibility_begin + ray.visibility_count; i++) {
        const auto &point = in.visibility[i];
        if (point.z > depth || (before && point.z == depth))
          break;
        const auto &h = point.holdout, &t = point.transmission;
        holes += std::exp(-tau_at(point.z)) * q * (channel == 0 ? h.x : channel == 1 ? h.y : h.z);
        q *= channel == 0 ? t.x : channel == 1 ? t.y : t.z;
      }
      sum += (std::exp(-tau_at(depth)) * q + holes) / 3;
    }
  }
  return sum / p.population;
}

class DeepGPUFrame : public testing::Test {
 protected:
  Stats stats;
  Profiler profiler;
  unique_ptr<Device> device;

  void SetUp() override
  {
    const char *backend = std::getenv("CYCLES_TEST_DEEP_GPU_FRAME");
    if (!backend)
      GTEST_SKIP() << "Explicit OptiX GPU numerical test opt-in required";
    ASSERT_STREQ(backend, "OPTIX");
    ASSERT_NE(std::getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_FRAME_MODULE"), nullptr);
    const auto devices = Device::available_devices(DEVICE_MASK_OPTIX);
    ASSERT_FALSE(devices.empty());
    device = Device::create(devices.front(), stats, profiler, true);
    ASSERT_TRUE(device);
    ASSERT_EQ(device->info.type, DEVICE_OPTIX);
    ASSERT_FALSE(device->have_error()) << device->error_message();
  }

  uint32_t run(const Input &in, vector<DeepGPUFrameTransmission> &result,
                vector<DeepGPUFrameKnot> *values = nullptr, const float exposure = 1)
  {
    device_vector<DeepGPUFrameRay> rays(device.get(), "deep_gpu_test_rays", MEM_READ_WRITE);
    device_vector<DeepGPUFrameOptical> optical(device.get(), "deep_gpu_test_optical", MEM_READ_WRITE);
    device_vector<DeepGPUFrameVisibility> visibility(device.get(), "deep_gpu_test_visibility", MEM_READ_WRITE);
    device_vector<DeepGPUFramePixel> pixels(device.get(), "deep_gpu_test_pixels", MEM_READ_WRITE);
    device_vector<float> cuts(device.get(), "deep_gpu_test_cuts", MEM_READ_WRITE);
    device_vector<DeepGPUFrameFloat3> prefix(device.get(), "deep_gpu_test_prefix", MEM_READ_WRITE);
    device_vector<DeepGPUFrameTransmission> output(device.get(), "deep_gpu_test_output", MEM_READ_WRITE);
    device_vector<uint32_t> status(device.get(), "deep_gpu_test_status", MEM_READ_WRITE);
    device_vector<DeepGPUFrameRadiance> radiance(device.get(), "deep_gpu_test_radiance", MEM_READ_WRITE);
    device_vector<DeepGPUFrameKnot> knots(device.get(), "deep_gpu_test_values", MEM_READ_WRITE);
    const auto upload = [](auto &buffer, const auto &values) {
      if (!values.empty()) {
        buffer.alloc(values.size());
        std::copy(values.begin(), values.end(), buffer.data());
        buffer.copy_to_device();
      }
    };
    upload(rays, in.rays);
    upload(optical, in.optical);
    upload(visibility, in.visibility);
    upload(pixels, in.pixels);
    upload(cuts, in.cuts);
    upload(radiance, in.radiance);
    if (values) { knots.alloc(in.cuts.size()); knots.alloc_to_device(); }
    if (!in.optical.empty()) {
      prefix.alloc(in.optical.size()); prefix.alloc_to_device();
    }
    output.alloc(in.cuts.size()); output.alloc_to_device();
    status.alloc(1);
    status.zero_to_device();
    DeepGPUFrameReduceParameters p{rays.device_pointer, optical.device_pointer,
                                  visibility.device_pointer, pixels.device_pointer,
                                  cuts.device_pointer, prefix.device_pointer,
                                  output.device_pointer, status.device_pointer,
                                  uint32_t(in.rays.size()), uint32_t(in.optical.size()),
                                  uint32_t(in.visibility.size()), uint32_t(in.pixels.size()),
                                  uint32_t(in.cuts.size())};
    p.radiance = radiance.device_pointer; p.radiance_count = uint32_t(in.radiance.size());
    p.values = values ? knots.device_pointer : 0; p.exposure = exposure;
    auto queue = device->gpu_queue_create();
    EXPECT_TRUE(queue->supports_deep_frame_reduction());
    if (!queue->enqueue_deep_frame_reduction(p) || !queue->synchronize()) {
      ADD_FAILURE() << device->error_message();
      return UINT32_MAX;
    }
    status.copy_from_device();
    if (status.data()[0])
      return status.data()[0];
    output.copy_from_device();
    result.assign(output.data(), output.data() + in.cuts.size());
    if (values) {
      knots.copy_from_device();
      values->assign(knots.data(), knots.data() + in.cuts.size());
    }
    return 0;
  }

  void compare(const Input &in)
  {
    vector<DeepGPUFrameTransmission> result;
    ASSERT_EQ(run(in, result), 0u);
    ASSERT_EQ(result.size(), in.cuts.size());
    for (size_t pixel = 0; pixel < in.pixels.size(); pixel++) {
      const auto &p = in.pixels[pixel];
      for (size_t c = p.cut_begin; c < p.cut_begin + p.cut_count; c++) {
        /* This narrow analytic FP32 test tolerance is not a production-global
         * arbitrary-depth error contract. */
        EXPECT_NEAR(result[c].before, reference(in, pixel, in.cuts[c], true), 3e-6);
        EXPECT_NEAR(result[c].after, reference(in, pixel, in.cuts[c], false), 3e-6);
      }
    }
  }
};

TEST_F(DeepGPUFrame, averages_colored_transmittance_and_keeps_vacuum_population)
{
  Input in;
  in.rays = {{0, 1, 0, 0, 0, 0}, {1, 0, 0, 0, 0, 1}};
  in.optical = {{1, 3, {2, 4, 6}}};
  in.pixels = {{0, 2, 0, 6, 2}};
  in.cuts = {0, 1, 1.5f, 2, 3, 10};
  compare(in);
}

TEST_F(DeepGPUFrame, protects_both_sides_of_surface_and_holdout_cuts)
{
  Input in;
  in.rays = {{0, 1, 0, 2, 0, 0}};
  in.optical = {{1, 5, {4, 4, 4}}};
  in.visibility = {{2, {0.25f, 0.5f, 0.75f}, {0.5f, 0.25f, 0.25f}},
                   {3, {0, 0, 0}, {0, 0, 0}}};
  in.pixels = {{0, 1, 0, 7, 1}};
  in.cuts = {0, 1, 2, 2.5f, 3, 4, 6};
  compare(in);
}

TEST_F(DeepGPUFrame, scans_multiple_optical_pages_and_variable_camera_populations)
{
  Input in;
  std::mt19937 rng(0x52d31018);
  const uint32_t populations[] = {1, 5, 32, 65, 129};
  for (uint32_t pixel = 0; pixel < 5; pixel++) {
    DeepGPUFramePixel p{uint32_t(in.rays.size()), populations[pixel],
                        uint32_t(in.cuts.size()), 13, populations[pixel]};
    for (uint32_t r = 0; r < p.ray_count; r++) {
      DeepGPUFrameRay ray{uint32_t(in.optical.size()), 300,
                          uint32_t(in.visibility.size()), 0, pixel, r};
      for (uint32_t part = 0; part < 300; part++) {
        const float t = float(1 + rng() % 100) * 0.0001f;
        in.optical.push_back({1 + part * 0.02f, 1 + part * 0.02f + 0.01f, {t, t * 2, t * 3}});
      }
      in.rays.push_back(ray);
    }
    for (uint32_t cut = 0; cut < p.cut_count; cut++)
      in.cuts.push_back(cut * 0.7f);
    in.pixels.push_back(p);
  }
  compare(in);
}

TEST_F(DeepGPUFrame, rejects_incomplete_or_invalid_device_inputs)
{
  Input base;
  base.rays = {{0, 1, 0, 1, 0, 0}};
  base.optical = {{1, 2, {1, 1, 1}}};
  base.visibility = {{2, {0, 0, 0}, {0, 0, 0}}};
  base.pixels = {{0, 1, 0, 2, 1}};
  base.cuts = {1, 3};
  const auto reject = [&](const Input &in, const uint32_t expected) {
    vector<DeepGPUFrameTransmission> result;
    EXPECT_NE(run(in, result) & expected, 0u);
    EXPECT_TRUE(result.empty());
  };
  auto bad = base;
  bad.rays[0].optical_begin = UINT32_MAX;
  reject(bad, DEEP_GPU_FRAME_INVALID_RANGE);
  bad = base;
  bad.optical[0].tau.y = -1;
  reject(bad, DEEP_GPU_FRAME_INVALID_OPTICAL);
  bad = base;
  bad.optical[0].z_back = 0.5f;
  reject(bad, DEEP_GPU_FRAME_INVALID_OPTICAL);
  bad = base;
  bad.optical[0].tau.z = std::numeric_limits<float>::quiet_NaN();
  reject(bad, DEEP_GPU_FRAME_INVALID_OPTICAL);
  bad = base;
  bad.visibility[0].transmission.x = 1.5f;
  reject(bad, DEEP_GPU_FRAME_INVALID_VISIBILITY);
  bad = base;
  bad.pixels[0].population = 0;
  reject(bad, DEEP_GPU_FRAME_INVALID_POPULATION);
  bad = base;
  bad.rays[0].pixel = 1;
  reject(bad, DEEP_GPU_FRAME_INVALID_POPULATION);
  bad = base;
  bad.cuts[1] = 0;
  reject(bad, DEEP_GPU_FRAME_INVALID_GRID);
}

TEST_F(DeepGPUFrame, evaluates_native_signed_hdr_spans_and_point_limits_on_device)
{
  Input in;
  in.rays = {{0, 1, 0, 0, 0, 0, 0, 2}, {1, 0, 0, 0, 0, 1, 2, 1}};
  in.optical = {{1, 3, {0.5f, 1, 2}}};
  in.radiance = {{1, 3, {0.5f, 1, 2}, {64, -2, 4}},
                 {4, 4, {0, 0, 0}, {-32, 1, 0}},
                 {2, 2, {0, 0, 0}, {4, -1, 2}}};
  in.pixels = {{0, 2, 0, 7, 2}};
  in.cuts = {0, 1, 2, 2.5f, 3, 4, 5};
  vector<DeepGPUFrameTransmission> transmittance;
  vector<DeepGPUFrameKnot> values;
  ASSERT_EQ(run(in, transmittance, &values, 2), 0u);
  ASSERT_EQ(values.size(), in.cuts.size());
  for (size_t i = 0; i < values.size(); i++) {
    for (const bool before : {true, false}) {
      const auto actual = before ? values[i].before : values[i].after;
      double expected[3] = {};
      for (const auto &part : in.radiance) {
        const double z = in.cuts[i];
        const bool point = part.z == part.z_back;
        if (z < part.z || (point && before && z == part.z)) continue;
        const double f = point ? 1 : std::min(1.0, (z - part.z) / (part.z_back - part.z));
        const float tau[3] = {part.tau.x, part.tau.y, part.tau.z};
        const float rgb[3] = {part.rgb.x, part.rgb.y, part.rgb.z};
        for (int c = 0; c < 3; c++)
          expected[c] += rgb[c] * (tau[c] > 0 ? -std::expm1(-tau[c] * f) / -std::expm1(-tau[c]) : f);
      }
      EXPECT_NEAR(actual.r, expected[0], 2e-5);
      EXPECT_NEAR(actual.g, expected[1], 2e-6);
      EXPECT_NEAR(actual.b, expected[2], 2e-6);
    }
  }
}

TEST_F(DeepGPUFrame, resolves_shared_native_expression_calls_without_host_reconstruction)
{
  vector<DeepNativeExpressionSource> sources(2);
  for (uint32_t i = 0; i < 2; i++) {
    sources[i].camera_index = i; sources[i].ordinal = i + 1;
    sources[i].flags = DEEP_NATIVE_EXPR_CAMERA_SPAN; sources[i].z = 1; sources[i].zback = 3;
  }
  sources[0].raw_rgb[0] = 4; sources[0].raw_rgb[1] = 8; sources[0].raw_rgb[2] = 12;
  sources[1].raw_rgb[0] = -2; sources[1].raw_rgb[1] = 0.5f; sources[1].raw_rgb[2] = 16;
  vector<DeepNativeExpressionNode> nodes(3);
  nodes[0].ordinal = 1; nodes[0].operation = DEEP_NATIVE_EXPR_TERM; nodes[0].source = 1;
  nodes[1].ordinal = 2; nodes[1].operation = DEEP_NATIVE_EXPR_SCALE; nodes[1].a = 1;
  nodes[1].weight[0] = 0.5f; nodes[1].weight[1] = 2; nodes[1].weight[2] = 1;
  nodes[2].ordinal = 3; nodes[2].camera_index = 1; nodes[2].operation = DEEP_NATIVE_EXPR_TERM;
  nodes[2].source = 2;
  vector<DeepNativeExpressionCall> calls(3);
  calls[0].head = 2; calls[0].clamp_gain = 0.25f; calls[0].routing = 1;
  calls[1].head = 1; calls[1].clamp_gain = 0.5f; calls[1].routing = 1;
  calls[2].camera_index = 1; calls[2].head = 3; calls[2].clamp_gain = 1; calls[2].routing = 1;
  device_vector<DeepNativeExpressionSource> source(device.get(), "gpu_expr_sources", MEM_READ_WRITE);
  device_vector<DeepNativeExpressionNode> node(device.get(), "gpu_expr_nodes", MEM_READ_WRITE);
  device_vector<DeepNativeExpressionCall> call(device.get(), "gpu_expr_calls", MEM_READ_WRITE);
  device_vector<uint32_t> order(device.get(), "gpu_expr_order", MEM_READ_WRITE);
  device_vector<DeepGPUFrameRange> ranges(device.get(), "gpu_expr_ranges", MEM_READ_WRITE);
  device_vector<DeepGPUFrameFloat3> weights(device.get(), "gpu_expr_weights", MEM_READ_WRITE);
  device_vector<DeepGPUFrameFloat3> source_weights(device.get(), "gpu_expr_source_weights", MEM_READ_WRITE);
  device_vector<DeepGPUFrameRadiance> output(device.get(), "gpu_expr_output", MEM_READ_WRITE);
  device_vector<uint32_t> owners(device.get(), "gpu_expr_owners", MEM_READ_WRITE);
  device_vector<uint32_t> status(device.get(), "gpu_expr_status", MEM_READ_WRITE);
  device_vector<DeepGPUFrameNativeParameters> parameters(device.get(), "gpu_expr_parameters", MEM_READ_WRITE);
  const auto upload = [](auto &buffer, const auto &values) {
    buffer.alloc(values.size()); std::copy(values.begin(), values.end(), buffer.data()); buffer.copy_to_device();
  };
  upload(source, sources); upload(node, nodes); upload(call, calls);
  upload(order, vector<uint32_t>{0, 1, 2});
  upload(ranges, vector<DeepGPUFrameRange>{{0, 2, 0, 0}, {2, 1, 0, 0}});
  weights.alloc(3); weights.zero_to_device(); source_weights.alloc(2); source_weights.zero_to_device();
  output.alloc(2); output.alloc_to_device(); owners.alloc(2); owners.alloc_to_device();
  status.alloc(1); status.zero_to_device();
  DeepGPUFrameNativeParameters p{};
  p.sources = source.device_pointer; p.nodes = node.device_pointer; p.calls = call.device_pointer;
  p.node_order = order.device_pointer; p.node_ranges = ranges.device_pointer;
  p.node_weights = weights.device_pointer; p.source_weights = source_weights.device_pointer;
  p.radiance = output.device_pointer; p.radiance_owners = owners.device_pointer;
  p.status = status.device_pointer; p.camera_count = 2; p.source_count = 2; p.node_count = 3; p.call_count = 3;
  upload(parameters, vector<DeepGPUFrameNativeParameters>{p});
  auto queue = device->gpu_queue_create();
  DeepGPUFrameDispatch d; d.a = parameters.device_pointer;
  ASSERT_TRUE(queue->enqueue_deep_frame_operation(DeepGPUFrameOperation::NativeSeed, d, 1, 256));
  ASSERT_TRUE(queue->enqueue_deep_frame_operation(DeepGPUFrameOperation::NativeReverse, d, 1, 256));
  ASSERT_TRUE(queue->enqueue_deep_frame_operation(DeepGPUFrameOperation::NativeRadiance, d, 1, 256));
  ASSERT_TRUE(queue->synchronize());
  status.copy_from_device(); ASSERT_EQ(status.data()[0], 0u);
  output.copy_from_device(); owners.copy_from_device();
  EXPECT_FLOAT_EQ(output.data()[0].rgb.x, 2.5f);
  EXPECT_FLOAT_EQ(output.data()[0].rgb.y, 8);
  EXPECT_FLOAT_EQ(output.data()[0].rgb.z, 9);
  EXPECT_FLOAT_EQ(output.data()[1].rgb.x, -2);
  EXPECT_FLOAT_EQ(output.data()[1].rgb.y, 0.5f);
  EXPECT_FLOAT_EQ(output.data()[1].rgb.z, 16);
  EXPECT_EQ(owners.data()[0], 0u); EXPECT_EQ(owners.data()[1], 1u);
}

TEST_F(DeepGPUFrame, half_encoding_carries_rounding_and_preserves_tiny_alpha_radiance)
{
  device_vector<DeepGPUFramePixel> pixels(device.get(), "fit_pixels", MEM_READ_WRITE);
  device_vector<float> cuts(device.get(), "fit_cuts", MEM_READ_WRITE);
  device_vector<DeepGPUFrameKnot> values(device.get(), "fit_values", MEM_READ_WRITE);
  device_vector<DeepGPUFrameConsumer> consumers(device.get(), "fit_consumers", MEM_READ_WRITE);
  device_vector<DeepGPUFrameRange> ranges(device.get(), "fit_ranges", MEM_READ_WRITE);
  device_vector<DeepGPUFrameSample> sparse(device.get(), "fit_sparse", MEM_READ_WRITE);
  device_vector<uint32_t> counts(device.get(), "fit_counts", MEM_READ_WRITE);
  device_vector<uint32_t> status(device.get(), "fit_status", MEM_READ_WRITE);
  device_vector<DeepGPUFrameEncodeParameters> parameters(device.get(), "fit_parameters", MEM_READ_WRITE);
  const auto upload = [](auto &buffer, const auto &input) {
    buffer.alloc(input.size()); std::copy(input.begin(), input.end(), buffer.data()); buffer.copy_to_device();
  };
  upload(pixels, vector<DeepGPUFramePixel>{{0, 1, 0, 4, 1}});
  upload(cuts, vector<float>{1, 2, 3, 4});
  upload(values, vector<DeepGPUFrameKnot>{
      {{1, 0, 0, 0}, {.7f, .1f, .1f, .1f}},
      {{.7f, .1f, .1f, .1f}, {.7f, .2f, .2f, .2f}},
      {{.7f, .2f, .2f, .2f}, {.6999998f, .23f, .23f, .23f}},
      {{.6999998f, .23f, .23f, .23f}, {.65f, .4f, .4f, .4f}}});
  upload(consumers, vector<DeepGPUFrameConsumer>{{1, 0, .01f, 0, .006f, 0}});
  ranges.alloc(1); ranges.alloc_to_device(); sparse.alloc(8); sparse.alloc_to_device();
  counts.alloc(1); counts.alloc_to_device(); status.alloc(4); status.zero_to_device();
  DeepGPUFrameEncodeParameters p{pixels.device_pointer, cuts.device_pointer, values.device_pointer,
      consumers.device_pointer, ranges.device_pointer, sparse.device_pointer, 0,
      counts.device_pointer, 0, 0, status.device_pointer, 1, 4, 1, 8};
  upload(parameters, vector<DeepGPUFrameEncodeParameters>{p});
  auto queue = device->gpu_queue_create();
  DeepGPUFrameDispatch d; d.a = parameters.device_pointer;
  ASSERT_TRUE(queue->enqueue_deep_frame_operation(DeepGPUFrameOperation::Fit, d, 1, 32));
  ASSERT_TRUE(queue->synchronize());
  status.copy_from_device(); ASSERT_EQ(status.data()[0], 0u);
  counts.copy_from_device(); ASSERT_GE(counts.data()[0], 4u); ASSERT_LE(counts.data()[0], 8u);
  sparse.copy_from_device();
  double transmission = 1, rgb = 0;
  for (uint32_t i = 0; i < counts.data()[0]; i++) {
    const auto sample = sparse.data()[i];
    rgb += transmission * sample.r;
    transmission *= 1 - double(sample.a);
    ASSERT_GE(sample.z, 1); ASSERT_LE(sample.z_back, 4); ASSERT_LE(sample.z, sample.z_back);
    const uint32_t end = uint32_t(sample.z_back) - 1;
    const auto expected = sample.z == sample.z_back ? values.data()[end].after : values.data()[end].before;
    EXPECT_NEAR(transmission, expected.t, .01);
    EXPECT_NEAR(rgb, expected.r, .006);
  }
  EXPECT_NEAR(transmission, .65, .01); EXPECT_NEAR(rgb, .4, .006);
}

TEST_F(DeepGPUFrame, resident_quota_denial_preserves_previous_allocation_and_releases_it)
{
  DeepMemoryBudget host(1 << 20), gpu(4096);
  const size_t host_baseline = host.used(), gpu_baseline = gpu.used();
  auto queue = device->gpu_queue_create();
  {
    DeepGPUResidentBuffer buffer(device.get(), host, gpu);
    ASSERT_TRUE(buffer.reserve(*queue, 1024));
    const auto address = buffer.pointer();
    EXPECT_FALSE(buffer.reserve(*queue, 8192));
    EXPECT_TRUE(device->have_error());
    EXPECT_EQ(buffer.pointer(), address); EXPECT_EQ(gpu.used(), gpu_baseline + 1024);
  }
  EXPECT_EQ(gpu.used(), gpu_baseline); EXPECT_EQ(host.used(), host_baseline);
}
}  // namespace

CCL_NAMESPACE_END
