/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

/* Private implementation state shared only by the facade, reference capture,
 * and output-view implementations. Mutations remain single Host owner. */
#include "session/deep_frame_production.h"
#include <algorithm>
#include <limits>

CCL_NAMESPACE_BEGIN

template<typename T> using ProductionVector = std::vector<T, DeepBudgetAllocator<T>>;
namespace deep_production_detail {
inline constexpr size_t no_index = std::numeric_limits<size_t>::max();
struct DomainState {
  DeepNativeCaptureDomain binding;
  bool complete = false;
};
struct PixelState {
  size_t ray_head = no_index;
  uint64_t count = 0;
  bool closed = false;
};
struct CapturedRay {
  uint64_t id;
  size_t batch, camera, next_pixel, source_head = no_index;
  uint64_t root = 0;
};
enum class CapturedSourceKind {
  Homogeneous,
  Point,
  OwnedOptical,
  OwnedVisibility,
  ExpressionSource,
  ExpressionNode,
  ExpressionCall
};
struct CapturedSource {
  uint64_t id;
  size_t batch, record, next_ray;
  CapturedSourceKind kind = CapturedSourceKind::Homogeneous;
};

inline bool translated_coordinate(const int value, const int offset, int &output)
{
  const int64_t mapped = int64_t(value) + offset;
  if (mapped < INT32_MIN || mapped > INT32_MAX)
    return false;
  output = int(mapped);
  return true;
}

inline DeepDomainCompletion completion(const DeepFrameKey &frame,
                                const DeepNativeCaptureDomain &domain,
                                const bool joined)
{
  DeepDomainCompletion result;
  result.frame = frame;
  result.domain = domain.id;
  result.device_generation = domain.device_generation;
  result.layout_generation = domain.layout_generation;
  result.native_admitted = domain.camera_written;
  result.producer_drained = joined;
  return result;
}

inline void accumulate_report(DeepCanonicalEncodeReport &total, const DeepCanonicalEncodeReport &report)
{
  for (int c = 0; c < 4; c++) {
    total.representation_error[c] = std::max(total.representation_error[c],
                                             report.representation_error[c]);
    total.encoding_error[c] = std::max(total.encoding_error[c], report.encoding_error[c]);
    total.total_error[c] = std::max(total.total_error[c], report.total_error[c]);
    total.maximum_budget_ratio[c] = std::max(total.maximum_budget_ratio[c],
                                             report.maximum_budget_ratio[c]);
    total.payload_quantization_bound[c] = std::max(total.payload_quantization_bound[c],
                                                   report.payload_quantization_bound[c]);
  }
  total.input_knots += report.input_knots;
  total.output_samples += report.output_samples;
  total.subdivisions += report.subdivisions;
  total.merge_candidates += report.merge_candidates;
  total.merged_intervals += report.merged_intervals;
  total.normalized_checked = report.normalized_checked;
  total.quantized_checked = report.quantized_checked;
}


struct ReferenceStorage final : DeepReferenceCapture {
  ReferenceStorage(const DeepMemoryBudget &budget, DeepFrameProduction &production)
      : owner(production), batches(DeepBudgetAllocator<DeepNativeCaptureBatch>(budget)),
        rays(DeepBudgetAllocator<CapturedRay>(budget)),
        sources(DeepBudgetAllocator<CapturedSource>(budget)),
        curves(DeepBudgetAllocator<DeepNativeCurve>(budget)) {}
  bool begin_native_capture(const DeepNativeCaptureDomain &domain) override;
  bool complete_native_capture(DeepNativeCaptureBatch &&batch) override;
  bool close_native_tile(int x, int y, int width, int height,
                         std::span<const uint32_t> counts, float exposure) override;
  DeepFrameProduction &owner;
  uint64_t next_ray = 1, next_source = 1, active_rays = 0;
  bool exposure_known = false;
  float exposure = 1;
  ProductionVector<DeepNativeCaptureBatch> batches;
  ProductionVector<CapturedRay> rays;
  ProductionVector<CapturedSource> sources;
  ProductionVector<DeepNativeCurve> curves;
};
}  // namespace deep_production_detail

struct DeepEncodedTileStorage {
  struct Range {
    size_t offset, count;
  };
  explicit DeepEncodedTileStorage(const DeepMemoryBudget &budget)
      : ranges(DeepBudgetAllocator<Range>(budget)),
        samples(DeepBudgetAllocator<DeepEncodedPayload>(budget))
  {
  }
  DeepRasterWindow window;
  ProductionVector<Range> ranges;
  ProductionVector<DeepEncodedPayload> samples;
  std::shared_ptr<const DeepBoundedHostTile> bounded;
};

struct DeepFrameProductionStorage {
  explicit DeepFrameProductionStorage(const DeepMemoryBudget &budget)
      : domains(DeepBudgetAllocator<deep_production_detail::DomainState>(budget)),
        pixels(DeepBudgetAllocator<deep_production_detail::PixelState>(budget)),
        consumer_ids(DeepBudgetAllocator<uint32_t>(budget)),
        consumers(DeepBudgetAllocator<DeepFrozenConsumer>(budget)),
        encoded_tiles(DeepBudgetAllocator<std::shared_ptr<const DeepEncodedTileStorage>>(budget)),
        pixel_cache_tiles(DeepBudgetAllocator<size_t>(budget)),
        encoding_reports(DeepBudgetAllocator<DeepCanonicalEncodeReport>(budget))
  {
  }
  bool index(const int32_t x, const int32_t y, size_t &index) const
  {
    const int64_t dx = int64_t(x) - window.x, dy = int64_t(y) - window.y;
    if (dx < 0 || dy < 0 || uint64_t(dx) >= window.width || uint64_t(dy) >= window.height)
      return false;
    index = size_t(dy) * window.width + size_t(dx);
    return true;
  }
  bool tile(const int x, const int y, const int width, const int height, size_t &first) const
  {
    if (width <= 0 || height <= 0 || int64_t(x) + width - 1 > INT32_MAX ||
        int64_t(y) + height - 1 > INT32_MAX)
      return false;
    size_t last;
    return index(x, y, first) &&
           index(int32_t(int64_t(x) + width - 1), int32_t(int64_t(y) + height - 1), last);
  }
  DeepFrameKey frame;
  DeepRasterWindow window;
  uint64_t next_tile = 1;
  std::shared_ptr<deep_production_detail::ReferenceStorage> reference;
  ProductionVector<deep_production_detail::DomainState> domains;
  ProductionVector<deep_production_detail::PixelState> pixels;
  ProductionVector<uint32_t> consumer_ids;
  ProductionVector<DeepFrozenConsumer> consumers;
  ProductionVector<std::shared_ptr<const DeepEncodedTileStorage>> encoded_tiles;
  ProductionVector<size_t> pixel_cache_tiles;
  ProductionVector<DeepCanonicalEncodeReport> encoding_reports;
  bool consumers_frozen = false;
};


CCL_NAMESPACE_END
