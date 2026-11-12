/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */
#include "session/deep_depth_support.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <unordered_map>

CCL_NAMESPACE_BEGIN
template<class T> using DepthSupportVector = std::vector<T, DeepBudgetAllocator<T>>;
namespace {
struct MapLink {
  uint64_t ray, handle;
};
struct SupportLink {
  uint64_t ray, source, handle;
  DeepPhysicalSourceKind kind;
};
auto key(const SupportLink &link)
{
  return std::tuple(link.ray, link.kind, link.source);
}
DeepStatus fail(DeepErrorCode code, const char *message)
{
  return DeepStatus::fail(code, message);
}
DeepStatus model(const DeepFrozenDepthMap &record, DeepCameraDepthMap &output)
{
  output = {};
  DeepCameraDepthSpec spec;
  spec.kind = record.kind;
  for (int c = 0; c < 3; c++) {
    spec.origin[c] = record.origin[c];
    spec.direction[c] = record.direction[c];
    if (record.kind == DeepFrozenDepthKind::Radial)
      spec.camera_origin[c] = record.transform[4 * c + 3];
  }
  spec.tmin = record.tmin;
  spec.tmax = record.tmax;
  spec.affine_origin = record.affine_origin;
  spec.affine_slope = record.affine_slope;
  return output.initialize(spec);
}
}  // namespace

struct DeepDepthSupportStorage {
  DeepDepthSupportStorage(const DeepFrameKey &frame, const DeepMemoryBudget &budget)
      : frame(frame),
        budget(budget),
        maps(DeepBudgetAllocator<MapLink>(budget)),
        supports(DeepBudgetAllocator<SupportLink>(budget)),
        pages(DeepBudgetAllocator<DeepPageLease>(budget))
  {
  }
  DeepFrameKey frame;
  DeepMemoryBudget budget;
  DepthSupportVector<MapLink> maps;
  DepthSupportVector<SupportLink> supports;
  DepthSupportVector<DeepPageLease> pages;
};

namespace {
template<class T>
DeepStatus read(const DeepDepthSupportStorage &storage,
                uint64_t handle,
                DeepRecordKind kind,
                uint64_t ray,
                T &output,
                DeepRecordTag &tag)
{
  output = {};
  const uint32_t page = deep_handle_page(handle);
  const auto found = std::lower_bound(
      storage.pages.begin(), storage.pages.end(), page, [](const auto &lease, uint32_t id) {
        return lease.descriptor()->page_id < id;
      });
  if (found == storage.pages.end() || found->descriptor()->page_id != page ||
      found->descriptor()->kind != kind || !(*found->frame_key() == storage.frame) ||
      !found->is_charged_to(storage.budget))
    return fail(DeepErrorCode::InvalidReference, "Depth support page/frame/kind mismatch");
  std::span<const uint8_t> bytes;
  if (auto status = found->read(handle, tag, bytes); !status)
    return status;
  if (tag.ray != ray || bytes.size() != sizeof(T))
    return fail(DeepErrorCode::InvalidReference, "Depth support ray/record layout mismatch");
  std::memcpy(&output, bytes.data(), sizeof(T));
  return {};
}
}  // namespace

const DeepFrameKey *DeepDepthSupportView::frame_key() const
{
  return storage_ ? &storage_->frame : nullptr;
}
uint64_t DeepDepthSupportView::ray() const
{
  return storage_ ? storage_->maps[map_].ray : 0;
}
size_t DeepDepthSupportView::support_count() const
{
  return storage_ ? count_ : 0;
}
bool DeepDepthSupportView::is_charged_to(const DeepMemoryBudget &budget) const
{
  return storage_ && storage_->budget == budget;
}
DeepStatus DeepDepthSupportView::read_map(DeepFrozenDepthMap &output) const
{
  output = {};
  if (!storage_)
    return fail(DeepErrorCode::InvalidState, "Depth map view is empty");
  DeepRecordTag tag;
  const auto &link = storage_->maps[map_];
  if (auto status = read(*storage_, link.handle, DeepRecordKind::DepthMap, link.ray, output, tag);
      !status)
    return status;
  if (output.id != link.handle || tag.root != output.id)
    return fail(DeepErrorCode::InvalidReference, "Depth map self-reference mismatch");
  return {};
}
DeepStatus DeepDepthSupportView::make_model(DeepCameraDepthMap &output) const
{
  output = {};
  DeepFrozenDepthMap record;
  if (auto status = read_map(record); !status)
    return status;
  return model(record, output);
}
DeepStatus DeepDepthSupportView::read_support(DeepPhysicalSourceKind kind,
                                              uint64_t source,
                                              DeepPhysicalSourceSupport &output) const
{
  output = {};
  if (!storage_)
    return fail(DeepErrorCode::InvalidState, "Physical support view is empty");
  const auto begin = storage_->supports.begin() + first_, end = begin + count_;
  const auto wanted = std::tuple(ray(), kind, source);
  const auto found = std::lower_bound(
      begin, end, wanted, [](const auto &a, const auto &b) { return key(a) < b; });
  if (found == end || key(*found) != wanted)
    return fail(DeepErrorCode::InvalidReference, "Physical source support is missing");
  DeepRecordTag tag;
  if (auto status = read(
          *storage_, found->handle, DeepRecordKind::PhysicalSupport, ray(), output, tag);
      !status)
    return status;
  if (output.depth_map != storage_->maps[map_].handle || output.source != source ||
      output.kind != kind || tag.root != source || tag.ordinal != output.ordinal ||
      tag.flags != output.flags)
    return fail(DeepErrorCode::InvalidReference, "Physical source/map/ordinal identity mismatch");
  return {};
}
DeepStatus DeepDepthSupportTable::view(uint64_t ray, DeepDepthSupportView &output) const
{
  output = {};
  if (!storage_)
    return fail(DeepErrorCode::InvalidState, "Depth support table is unpublished");
  const auto found = std::lower_bound(storage_->maps.begin(),
                                      storage_->maps.end(),
                                      ray,
                                      [](const auto &a, uint64_t b) { return a.ray < b; });
  if (found == storage_->maps.end() || found->ray != ray)
    return fail(DeepErrorCode::InvalidReference, "Depth support ray is missing");
  output.storage_ = storage_;
  output.map_ = found - storage_->maps.begin();
  const auto first = std::lower_bound(storage_->supports.begin(),
                                      storage_->supports.end(),
                                      ray,
                                      [](const auto &a, uint64_t b) { return a.ray < b; });
  const auto end = std::upper_bound(
      first, storage_->supports.end(), ray, [](uint64_t a, const auto &b) { return a < b.ray; });
  output.first_ = first - storage_->supports.begin();
  output.count_ = end - first;
  return {};
}

struct DeepDepthSupportBuildState {
  struct MapValue {
    uint64_t ray;
    DeepFrozenDepthMap record;
  };
  using MapAllocator = DeepBudgetAllocator<std::pair<const uint64_t, MapValue>>;
  DeepDepthSupportBuildState(const DeepFrameKey &frame,
                             const DeepMemoryBudget &budget,
                             size_t bytes)
      : store(frame, budget, bytes),
        data(std::allocate_shared<DeepDepthSupportStorage>(
            DeepBudgetAllocator<DeepDepthSupportStorage>(budget), frame, budget)),
        maps(0, std::hash<uint64_t>(), std::equal_to<uint64_t>(), MapAllocator(budget))
  {
    if (!frame.invocation)
      status = fail(DeepErrorCode::InvalidFrame, "Depth support frame is invalid");
  }
  DeepStatus reject(const DeepStatus &error)
  {
    if (status)
      status = error;
    return status;
  }
  DeepStatus close(uint32_t &page)
  {
    if (!page)
      return {};
    DeepPageLease lease;
    if (auto result = store.seal(page, lease); !result)
      return result;
    data->pages.push_back(lease);
    if (auto result = store.retire(page); !result)
      return result;
    page = 0;
    return {};
  }
  DeepStatus reserve(DeepRecordKind kind, uint64_t &handle)
  {
    uint32_t &page = kind == DeepRecordKind::DepthMap ? map_page : support_page;
    if (!page)
      if (auto result = store.allocate(kind, page); !result)
        return result;
    auto result = store.reserve(page, handle);
    if (result.code == DeepErrorCode::NeedPage) {
      if (result = close(page); !result)
        return result;
      if (result = store.allocate(kind, page); !result)
        return result;
      result = store.reserve(page, handle);
    }
    return result;
  }
  template<class T> DeepStatus write(uint64_t handle, const DeepRecordTag &tag, const T &record)
  {
    auto result = store.write(
        handle, tag, {reinterpret_cast<const uint8_t *>(&record), sizeof(record)});
    if (result)
      result = store.commit(handle);
    if (!result)
      store.abort(handle);
    return result;
  }
  DeepPageStore store;
  std::shared_ptr<DeepDepthSupportStorage> data;
  std::
      unordered_map<uint64_t, MapValue, std::hash<uint64_t>, std::equal_to<uint64_t>, MapAllocator>
          maps;
  uint32_t map_page = 0, support_page = 0;
  DeepStatus status;
  bool finished = false;
};

DeepDepthSupportBuilder::DeepDepthSupportBuilder(const DeepFrameKey &frame,
                                                 const DeepMemoryBudget &budget,
                                                 size_t bytes)
    : state_(std::allocate_shared<DeepDepthSupportBuildState>(
          DeepBudgetAllocator<DeepDepthSupportBuildState>(budget), frame, budget, bytes))
{
}

DeepStatus DeepDepthSupportBuilder::add_map(uint64_t ray,
                                            const DeepFrozenDepthMap &input,
                                            uint64_t &handle)
{
  handle = 0;
  auto &s = *state_;
  if (!s.status)
    return s.status;
  if (s.finished)
    return fail(DeepErrorCode::InvalidState, "Depth support builder is finished");
  if (!ray || input.id)
    return s.reject(
        fail(DeepErrorCode::InvalidReference, "Depth map requires a new ray-owned identity"));
  try {
    DeepCameraDepthMap proof;
    if (auto result = model(input, proof); !result)
      return s.reject(result);
    uint64_t id;
    if (auto result = s.reserve(DeepRecordKind::DepthMap, id); !result)
      return s.reject(result);
    auto record = input;
    record.id = id;
    DeepRecordTag tag;
    tag.ray = ray;
    tag.root = id;
    tag.kind = uint32_t(DeepRecordKind::DepthMap);
    if (auto result = s.write(id, tag, record); !result)
      return s.reject(result);
    s.data->maps.push_back({ray, id});
    s.maps.emplace(id, DeepDepthSupportBuildState::MapValue{ray, record});
    handle = id;
    return {};
  }
  catch (const std::bad_alloc &) {
    return s.reject(fail(DeepErrorCode::BudgetExceeded, "Depth map ownership quota exhausted"));
  }
  catch (const std::length_error &) {
    return s.reject(fail(DeepErrorCode::InvalidCount, "Depth map ownership count overflow"));
  }
}

DeepStatus DeepDepthSupportBuilder::add_support(uint64_t ray,
                                                const DeepPhysicalSourceSupport &input)
{
  auto &s = *state_;
  if (!s.status)
    return s.status;
  if (s.finished)
    return fail(DeepErrorCode::InvalidState, "Depth support builder is finished");
  const auto map = s.maps.find(input.depth_map);
  if (map == s.maps.end() || map->second.ray != ray)
    return s.reject(
        fail(DeepErrorCode::InvalidReference, "Physical support has a foreign depth map"));
  const auto &record = map->second.record;
  const bool infinity = input.flags & DEEP_PHYSICAL_INFINITY;
  const bool world = input.kind == DeepPhysicalSourceKind::Radiance &&
                     (input.flags & DEEP_PHYSICAL_WORLD_RADIANCE);
  if (!infinity && (input.front < record.tmin || (!world && input.back > record.tmax)))
    return s.reject(
        fail(DeepErrorCode::InvalidReference, "Physical support exceeds camera authority"));
  try {
    if (!infinity) {
      DeepCameraDepthMap depth_map;
      if (auto result = model(record, depth_map); !result)
        return s.reject(result);
      if (input.flags & DEEP_PHYSICAL_POINT) {
        double z;
        if (auto result = depth_map.depth(input.front, z); !result)
          return s.reject(result);
        if (std::abs(input.z - z) > 64 * std::numeric_limits<float>::epsilon() * std::max(1., z))
          return s.reject(fail(DeepErrorCode::InvalidReference,
                               "Physical point depth disagrees with frozen camera geometry"));
      }
      else {
        DeepCameraDepthSpan span;
        if (auto result = span.initialize(
                depth_map, input.front, input.back, input.z, input.z_back);
            !result)
          return s.reject(result);
      }
    }
    uint64_t id;
    if (auto result = s.reserve(DeepRecordKind::PhysicalSupport, id); !result)
      return s.reject(result);
    DeepRecordTag tag;
    tag.ray = ray;
    tag.root = input.source;
    tag.ordinal = input.ordinal;
    tag.flags = input.flags;
    tag.kind = uint32_t(DeepRecordKind::PhysicalSupport);
    if (auto result = s.write(id, tag, input); !result)
      return s.reject(result);
    s.data->supports.push_back({ray, input.source, id, input.kind});
    return {};
  }
  catch (const std::bad_alloc &) {
    return s.reject(
        fail(DeepErrorCode::BudgetExceeded, "Physical support ownership quota exhausted"));
  }
  catch (const std::length_error &) {
    return s.reject(
        fail(DeepErrorCode::InvalidCount, "Physical support ownership count overflow"));
  }
}

DeepStatus DeepDepthSupportBuilder::finish(DeepDepthSupportTable &output)
{
  output = {};
  auto &s = *state_;
  if (!s.status)
    return s.status;
  if (s.finished)
    return fail(DeepErrorCode::InvalidState, "Depth support builder is finished");
  try {
    auto &maps = s.data->maps;
    auto &supports = s.data->supports;
    std::sort(
        maps.begin(), maps.end(), [](const auto &a, const auto &b) { return a.ray < b.ray; });
    if (std::adjacent_find(maps.begin(), maps.end(), [](const auto &a, const auto &b) {
          return a.ray == b.ray;
        }) != maps.end())
      return s.reject(fail(DeepErrorCode::InvalidReference, "Duplicate ray depth maps"));
    std::sort(supports.begin(), supports.end(), [](const auto &a, const auto &b) {
      return key(a) < key(b);
    });
    if (std::adjacent_find(supports.begin(), supports.end(), [](const auto &a, const auto &b) {
          return key(a) == key(b);
        }) != supports.end())
      return s.reject(fail(DeepErrorCode::InvalidReference, "Duplicate physical source identity"));
    if (auto result = s.close(s.map_page); !result)
      return s.reject(result);
    if (auto result = s.close(s.support_page); !result)
      return s.reject(result);
    std::sort(s.data->pages.begin(), s.data->pages.end(), [](const auto &a, const auto &b) {
      return a.descriptor()->page_id < b.descriptor()->page_id;
    });
    output.storage_ = s.data;
    s.finished = true;
    return {};
  }
  catch (const std::bad_alloc &) {
    return s.reject(
        fail(DeepErrorCode::BudgetExceeded, "Depth support publication quota exhausted"));
  }
  catch (const std::length_error &) {
    return s.reject(fail(DeepErrorCode::InvalidCount, "Depth support publication count overflow"));
  }
}
CCL_NAMESPACE_END
