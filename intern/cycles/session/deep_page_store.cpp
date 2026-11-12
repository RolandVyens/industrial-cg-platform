/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "session/deep_page_store.h"
#include "kernel/integrator/deep_frame_capture.h"
#include "kernel/integrator/deep_optical_event.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>

CCL_NAMESPACE_BEGIN

namespace {
enum class Slot : uint8_t { Free, Reserved, Written, Committed, Aborted };
std::atomic<uint64_t> next_page_id{1};

size_t payload_stride(const DeepRecordKind kind)
{
  switch (kind) {
    case DeepRecordKind::OpticalPiece:
      return sizeof(DeepOpticalPiece);
    case DeepRecordKind::VisibilityPoint:
      return sizeof(DeepVisibilityPoint);
    case DeepRecordKind::RadianceTerm:
      return sizeof(DeepRadianceTerm);
    case DeepRecordKind::NativeCurve:
      return sizeof(DeepNativeCurveHeader);
    case DeepRecordKind::DepthKnot:
      return sizeof(DeepDepthKnot);
    case DeepRecordKind::NativeExpressionSource:
      return sizeof(DeepNativeExpressionSource);
    case DeepRecordKind::NativeExpressionNode:
      return sizeof(DeepNativeExpressionNode);
    case DeepRecordKind::NativeExpressionCall:
      return sizeof(DeepNativeExpressionCall);
    case DeepRecordKind::DepthMap:
      return sizeof(DeepFrozenDepthMap);
    case DeepRecordKind::PhysicalSupport:
      return sizeof(DeepPhysicalSourceSupport);
  }
  return 0;
}

template<typename T> T read_payload(const std::span<const uint8_t> bytes)
{
  T value;
  std::memcpy(&value, bytes.data(), sizeof(T));
  return value;
}

bool valid_payload(const DeepRecordKind kind, const std::span<const uint8_t> bytes)
{
  const auto depth = [](const float a, const float b) {
    return std::isfinite(a) && std::isfinite(b) && a >= 0.0f && b >= a;
  };
  if (kind == DeepRecordKind::OpticalPiece) {
    const auto p = read_payload<DeepOpticalPiece>(bytes);
    /* Raw physical pieces may share FLOAT endpoints. Their tau is retained;
     * finite canonical geometry and its projection certificate are checked later. */
    if (!depth(p.z, p.z_back))
      return false;
    for (float tau : p.tau)
      if (!std::isfinite(tau) || tau < 0.0f)
        return false;
  }
  else if (kind == DeepRecordKind::VisibilityPoint) {
    const auto p = read_payload<DeepVisibilityPoint>(bytes);
    if (!depth(p.z, p.z))
      return false;
    for (int c = 0; c < 3; c++) {
      if (!std::isfinite(p.transmission[c]) || !std::isfinite(p.holdout[c]) ||
          p.transmission[c] < 0.0f || p.transmission[c] > 1.0f || p.holdout[c] < 0.0f ||
          p.holdout[c] > 1.0f - p.transmission[c])
        return false;
    }
  }
  else if (kind == DeepRecordKind::RadianceTerm) {
    const auto p = read_payload<DeepRadianceTerm>(bytes);
    if (!depth(p.z, p.z_back) || p.basis > 2 || (p.basis == 0 ? p.z_back != p.z : p.z_back == p.z))
      return false;
    for (int c = 0; c < 3; c++) {
      if (!std::isfinite(p.rgb[c]) || !std::isfinite(p.tau[c]) || p.tau[c] < 0.0f)
        return false;
    }
  }
  else if (kind == DeepRecordKind::NativeCurve) {
    const auto p = read_payload<DeepNativeCurveHeader>(bytes);
    if (!std::isfinite(p.population_weight) || p.population_weight <= 0.0 ||
        p.knot_count > std::numeric_limits<uint64_t>::max() - p.knot_begin)
      return false;
  }
  else if (kind == DeepRecordKind::DepthMap) {
    const auto p = read_payload<DeepFrozenDepthMap>(bytes);
    if (!p.id || p.flags ||
        (p.kind != DeepFrozenDepthKind::Affine && p.kind != DeepFrozenDepthKind::Radial) ||
        !std::isfinite(p.tmin) || !std::isfinite(p.tmax) || p.tmin < 0 || p.tmax < p.tmin ||
        !std::isfinite(p.affine_origin) || !std::isfinite(p.affine_slope) ||
        (p.kind == DeepFrozenDepthKind::Affine && p.affine_slope <= 0) ||
        (p.kind == DeepFrozenDepthKind::Radial && (p.affine_origin != 0 || p.affine_slope != 0)))
      return false;
    double norm = 0;
    for (float value : p.transform)
      if (!std::isfinite(value))
        return false;
    for (int c = 0; c < 3; c++) {
      if (!std::isfinite(p.origin[c]) || !std::isfinite(p.direction[c]))
        return false;
      norm += double(p.direction[c]) * p.direction[c];
    }
    if (!(norm > 0))
      return false;
  }
  else if (kind == DeepRecordKind::PhysicalSupport) {
    const auto p = read_payload<DeepPhysicalSourceSupport>(bytes);
    const bool point = p.flags & DEEP_PHYSICAL_POINT, infinity = p.flags & DEEP_PHYSICAL_INFINITY;
    const bool world = p.flags & DEEP_PHYSICAL_WORLD_RADIANCE;
    if (!p.depth_map || !p.source || p.reserved || p.flags & ~7u ||
        (p.kind != DeepPhysicalSourceKind::Optical &&
         p.kind != DeepPhysicalSourceKind::Visibility &&
         p.kind != DeepPhysicalSourceKind::Radiance) ||
        !std::isfinite(p.front) || !std::isfinite(p.back) || !std::isfinite(p.z) ||
        !std::isfinite(p.z_back) || p.z < 0 || p.z_back < p.z ||
        p.z_back > double(std::numeric_limits<float>::max()) ||
        (world && p.kind != DeepPhysicalSourceKind::Radiance))
      return false;
    if (infinity) {
      if (!point || world || p.kind == DeepPhysicalSourceKind::Optical || p.front != -1 ||
          p.back != -1 || p.z != double(std::numeric_limits<float>::max()) || p.z_back != p.z)
        return false;
    }
    else if (p.front < 0 || p.back > double(std::numeric_limits<float>::max()) ||
             (point ? p.back != p.front || p.z_back != p.z : p.back <= p.front || p.z_back <= p.z))
      return false;
    if ((p.kind == DeepPhysicalSourceKind::Optical && point) ||
        (p.kind == DeepPhysicalSourceKind::Visibility && !point))
      return false;
  }
  else if (kind == DeepRecordKind::DepthKnot) {
    const auto p = read_payload<DeepDepthKnot>(bytes);
    if (!std::isfinite(p.z) || p.z < 0.0)
      return false;
    for (int c = 0; c < 4; c++) {
      if (!std::isfinite(p.before[c]) || !std::isfinite(p.after[c]) ||
          !std::isfinite(p.slope_right[c]))
        return false;
    }
    /* Public knots are normalized curves. Unnormalized AA running sums use a separate
     * internal type, never an unchecked transmission greater than one here. */
    if (p.before[0] < 0.0 || p.before[0] > 1.0 || p.after[0] < 0.0 || p.after[0] > p.before[0] ||
        p.slope_right[0] > 0.0)
      return false;
  }
  if (kind == DeepRecordKind::NativeExpressionSource) {
    const auto p = read_payload<DeepNativeExpressionSource>(bytes);
    if (!depth(p.z, p.zback) || !p.ordinal)
      return false;
    for (int c = 0; c < 3; c++)
      if (!std::isfinite(p.raw_rgb[c]) || !std::isfinite(p.tau[c]) || p.tau[c] < 0)
        return false;
  }
  else if (kind == DeepRecordKind::NativeExpressionNode) {
    const auto p = read_payload<DeepNativeExpressionNode>(bytes);
    if (!p.ordinal || p.operation > DEEP_NATIVE_EXPR_SELECT || p.select_b > 1)
      return false;
    for (const float weight : p.weight)
      if (!std::isfinite(weight))
        return false;
  }
  else if (kind == DeepRecordKind::NativeExpressionCall) {
    const auto p = read_payload<DeepNativeExpressionCall>(bytes);
    if (!p.ordinal || !std::isfinite(p.clamp_gain) || p.clamp_gain < 0 || p.clamp_gain > 1)
      return false;
    for (int c = 0; c < 3; c++)
      if (!std::isfinite(p.pre_clamp_rgb[c]) || !std::isfinite(p.post_clamp_rgb[c]))
        return false;
  }
  return true;
}
}  // namespace

struct DeepStoredPage {
  explicit DeepStoredPage(const DeepMemoryBudget &budget)
      : bytes(DeepBudgetAllocator<uint8_t>(budget)), slots(DeepBudgetAllocator<Slot>(budget))
  {
  }
  DeepFrameKey frame;
  DeepPageDescriptor header = {};
  std::vector<uint8_t, DeepBudgetAllocator<uint8_t>> bytes;
  std::vector<Slot, DeepBudgetAllocator<Slot>> slots;
  bool sealed = false;
};

const DeepPageDescriptor *DeepPageLease::descriptor() const
{
  return page_ ? &page_->header : nullptr;
}

const DeepFrameKey *DeepPageLease::frame_key() const
{
  return page_ ? &page_->frame : nullptr;
}

uint64_t DeepPageLease::valid_handle_at(const uint32_t slot) const
{
  if (!page_ || slot >= page_->header.reserved || page_->slots[slot] != Slot::Committed)
    return 0;
  return deep_page_handle(page_->header.page_id, uint16_t(slot), page_->header.generation);
}

bool DeepPageLease::is_charged_to(const DeepMemoryBudget &budget) const
{
  return page_ && page_->bytes.get_allocator() == DeepBudgetAllocator<uint8_t>(budget);
}

DeepStatus DeepPageLease::read(const uint64_t handle,
                               DeepRecordTag &tag,
                               std::span<const uint8_t> &payload) const
{
  payload = {};
  if (!page_ || deep_handle_page(handle) != page_->header.page_id ||
      deep_handle_generation(handle) != page_->header.generation ||
      deep_handle_slot(handle) >= page_->header.reserved ||
      page_->slots[deep_handle_slot(handle)] != Slot::Committed)
    return DeepStatus::fail(DeepErrorCode::InvalidReference, "Uncommitted or foreign Deep handle");
  const size_t slot = deep_handle_slot(handle), stride = payload_stride(page_->header.kind);
  std::memcpy(
      &tag, page_->bytes.data() + page_->header.tag_offset + slot * sizeof(tag), sizeof(tag));
  payload = {page_->bytes.data() + page_->header.payload_offset + slot * stride, stride};
  return {};
}

DeepPageStore::DeepPageStore(const DeepFrameKey &frame,
                             const size_t budget,
                             const size_t page_bytes)
    : DeepPageStore(frame, DeepMemoryBudget(budget), page_bytes)
{
}
DeepPageStore::DeepPageStore(const DeepFrameKey &frame,
                             const DeepMemoryBudget &budget,
                             const size_t page_bytes)
    : frame_(frame),
      page_bytes_(page_bytes),
      budget_(budget),
      pages_(0,
             std::hash<uint32_t>{},
             std::equal_to<uint32_t>{},
             DeepBudgetAllocator<PageEntry>(budget_))
{
}
DeepPageStore::~DeepPageStore() = default;

DeepStatus DeepPageStore::allocate(const DeepRecordKind kind, uint32_t &id)
{
  id = 0;
  const size_t stride = payload_stride(kind);
  if (!frame_.invocation || !stride || page_bytes_ <= sizeof(DeepPageDescriptor))
    return DeepStatus::fail(DeepErrorCode::InvalidLayout, "Invalid Deep page/frame layout");
  const size_t capacity = (page_bytes_ - sizeof(DeepPageDescriptor)) /
                          (stride + sizeof(DeepRecordTag));
  if (!capacity || capacity > std::numeric_limits<uint16_t>::max() ||
      page_bytes_ > std::numeric_limits<size_t>::max() - capacity - sizeof(DeepStoredPage))
    return DeepStatus::fail(DeepErrorCode::InvalidLayout, "Unrepresentable Deep page capacity");
  const uint64_t page_id = next_page_id.fetch_add(1);
  if (page_id > std::numeric_limits<uint32_t>::max())
    return DeepStatus::fail(DeepErrorCode::InvalidReference, "Deep page identity space exhausted");
  try {
    auto page = std::allocate_shared<DeepStoredPage>(DeepBudgetAllocator<DeepStoredPage>(budget_),
                                                     budget_);
    page->bytes.resize(page_bytes_);
    page->slots.resize(capacity, Slot::Free);
    page->frame = frame_;
    page->header = {uint32_t(page_id),
                    1,
                    kind,
                    uint32_t(capacity),
                    0,
                    0,
                    0,
                    0,
                    0,
                    sizeof(DeepPageDescriptor),
                    sizeof(DeepPageDescriptor) + capacity * sizeof(DeepRecordTag),
                    capacity * stride,
                    frame_.invocation};
    pages_.emplace(uint32_t(page_id), page);
    id = uint32_t(page_id);
  }
  catch (const std::bad_alloc &) {
    return DeepStatus::fail(DeepErrorCode::BudgetExceeded, "Deep page allocation failed");
  }
  return {};
}

DeepStatus DeepPageStore::locate(const uint64_t handle,
                                 std::shared_ptr<DeepStoredPage> &page,
                                 uint16_t &slot) const
{
  const auto found = pages_.find(deep_handle_page(handle));
  if (found == pages_.end() ||
      found->second->header.generation != deep_handle_generation(handle) ||
      deep_handle_slot(handle) >= found->second->header.reserved)
  {
    if (first_error_ == DeepErrorCode::None)
      first_error_ = DeepErrorCode::InvalidReference;
    return DeepStatus::fail(DeepErrorCode::InvalidReference,
                            "Expired or foreign Deep page reference");
  }
  page = found->second;
  slot = deep_handle_slot(handle);
  if (page->sealed)
    return DeepStatus::fail(DeepErrorCode::InvalidState, "Deep page is immutable");
  return {};
}

DeepStatus DeepPageStore::reserve(const uint32_t id, uint64_t &handle)
{
  handle = 0;
  const auto found = pages_.find(id);
  if (found == pages_.end())
    return DeepStatus::fail(DeepErrorCode::InvalidReference, "Unknown Deep page");
  auto &page = *found->second;
  if (page.sealed)
    return DeepStatus::fail(DeepErrorCode::InvalidState, "Deep page is immutable");
  if (page.header.reserved == page.header.capacity)
    return DeepStatus::fail(DeepErrorCode::NeedPage, "Deep append requires another owned page");
  const uint32_t slot = page.header.reserved++;
  page.slots[slot] = Slot::Reserved;
  handle = deep_page_handle(id, uint16_t(slot), page.header.generation);
  return {};
}

DeepStatus DeepPageStore::write(const uint64_t handle,
                                const DeepRecordTag &tag,
                                const std::span<const uint8_t> payload)
{
  std::shared_ptr<DeepStoredPage> page;
  uint16_t slot;
  if (const DeepStatus status = locate(handle, page, slot); !status)
    return status;
  if (page->slots[slot] != Slot::Reserved) {
    if (!page->header.first_error)
      page->header.first_error = uint32_t(DeepErrorCode::InvalidState);
    return DeepStatus::fail(DeepErrorCode::InvalidState, "Deep slot cannot be written twice");
  }
  if (payload.size() != payload_stride(page->header.kind) ||
      tag.kind != uint32_t(page->header.kind))
  {
    if (!page->header.first_error)
      page->header.first_error = uint32_t(DeepErrorCode::InvalidLayout);
    return DeepStatus::fail(DeepErrorCode::InvalidLayout, "Deep payload/tag stride mismatch");
  }
  if (!valid_payload(page->header.kind, payload)) {
    if (!page->header.first_error)
      page->header.first_error = uint32_t(DeepErrorCode::InvalidPayload);
    return DeepStatus::fail(DeepErrorCode::InvalidPayload, "Invalid or nonfinite Deep payload");
  }
  std::memcpy(
      page->bytes.data() + page->header.tag_offset + slot * sizeof(tag), &tag, sizeof(tag));
  std::memcpy(page->bytes.data() + page->header.payload_offset + slot * payload.size(),
              payload.data(),
              payload.size());
  page->slots[slot] = Slot::Written;
  page->header.written++;
  return {};
}

DeepStatus DeepPageStore::commit(const uint64_t handle)
{
  std::shared_ptr<DeepStoredPage> page;
  uint16_t slot;
  if (const DeepStatus status = locate(handle, page, slot); !status)
    return status;
  if (page->slots[slot] != Slot::Written) {
    if (!page->header.first_error)
      page->header.first_error = uint32_t(DeepErrorCode::InvalidState);
    return DeepStatus::fail(DeepErrorCode::InvalidState,
                            "Deep commit requires exactly one complete write");
  }
  page->slots[slot] = Slot::Committed;
  page->header.committed++;
  page->header.valid++;
  return {};
}

DeepStatus DeepPageStore::abort(const uint64_t handle)
{
  std::shared_ptr<DeepStoredPage> page;
  uint16_t slot;
  if (const DeepStatus status = locate(handle, page, slot); !status)
    return status;
  if (page->slots[slot] != Slot::Reserved && page->slots[slot] != Slot::Written) {
    if (!page->header.first_error)
      page->header.first_error = uint32_t(DeepErrorCode::InvalidState);
    return DeepStatus::fail(DeepErrorCode::InvalidState,
                            "Committed/aborted Deep slot cannot be rolled back");
  }
  page->slots[slot] = Slot::Aborted;
  return {};
}

DeepStatus DeepPageStore::seal(const uint32_t id, DeepPageLease &lease)
{
  const auto found = pages_.find(id);
  if (found == pages_.end())
    return DeepStatus::fail(DeepErrorCode::InvalidReference, "Unknown Deep page");
  const auto &page = found->second;
  if (page->header.first_error)
    return DeepStatus::fail(DeepErrorCode(page->header.first_error),
                            "Deep page retains a hard producer error");
  for (uint32_t i = 0; i < page->header.reserved; i++) {
    if (page->slots[i] != Slot::Committed && page->slots[i] != Slot::Aborted)
      return DeepStatus::fail(DeepErrorCode::Incomplete,
                              "Deep producer still owns an uncommitted slot");
  }
  if (!page->sealed &&
      (page->header.kind == DeepRecordKind::DepthKnot ||
       page->header.kind == DeepRecordKind::DepthMap ||
       page->header.kind == DeepRecordKind::PhysicalSupport) &&
      page->header.reserved < page->header.capacity)
  {
    /* Producer capacity is not published sample count. Keep all reserved slot
     * indices, including aborted holes, but release unused canonical storage
     * before the first immutable lease exists. Repeated seal must not move a
     * previously published payload. */
    try {
      const size_t count = page->header.reserved;
      const size_t tag_bytes = count * sizeof(DeepRecordTag);
      const size_t payload_bytes = count * payload_stride(page->header.kind);
      const size_t payload_offset = sizeof(DeepPageDescriptor) + tag_bytes;
      std::vector<uint8_t, DeepBudgetAllocator<uint8_t>> compact(page->bytes.get_allocator());
      std::vector<Slot, DeepBudgetAllocator<Slot>> slots(page->slots.get_allocator());
      compact.resize(payload_offset + payload_bytes);
      slots.assign(page->slots.begin(), page->slots.begin() + count);
      std::memcpy(compact.data() + sizeof(DeepPageDescriptor),
                  page->bytes.data() + page->header.tag_offset,
                  tag_bytes);
      std::memcpy(compact.data() + payload_offset,
                  page->bytes.data() + page->header.payload_offset,
                  payload_bytes);
      page->bytes.swap(compact);
      page->slots.swap(slots);
      page->header.capacity = uint32_t(count);
      page->header.payload_offset = payload_offset;
      page->header.payload_bytes = payload_bytes;
    }
    catch (const std::bad_alloc &) {
      return DeepStatus::fail(DeepErrorCode::BudgetExceeded,
                              "Canonical page sealing compaction exceeds shared quota");
    }
  }
  page->sealed = true;
  std::memcpy(page->bytes.data(), &page->header, sizeof(page->header));
  lease.page_ = page;
  return {};
}

DeepStatus DeepPageStore::retire(const uint32_t id)
{
  const auto found = pages_.find(id);
  if (found == pages_.end())
    return DeepStatus::fail(DeepErrorCode::InvalidReference, "Unknown Deep page");
  if (!found->second->sealed)
    return DeepStatus::fail(DeepErrorCode::Incomplete, "Cannot retire an open Deep producer page");
  pages_.erase(found);
  return {};
}

size_t DeepPageStore::bytes_used() const
{
  return budget_.used();
}
size_t DeepPageStore::budget_bytes() const
{
  return budget_.quota();
}

DeepStatus DeepPageStore::publication_status() const
{
  if (first_error_ != DeepErrorCode::None)
    return DeepStatus::fail(first_error_, "Deep store retains a hard reference error");
  for (const auto &[id, page] : pages_) {
    if (page->header.first_error)
      return DeepStatus::fail(DeepErrorCode(page->header.first_error),
                              "Deep store retains a hard page error");
    if (!page->sealed)
      return DeepStatus::fail(DeepErrorCode::Incomplete,
                              "Deep store has an unfinished producer page");
  }
  return {};
}

CCL_NAMESPACE_END
