/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "session/deep_frame_pipeline.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <limits>
#include <map>
#include <set>

CCL_NAMESPACE_BEGIN

bool deep_frame_pipeline_requested(const char *option)
{
  return option && std::strcmp(option, "1") == 0;
}

namespace {
template<typename T> using BudgetVector = std::vector<T, DeepBudgetAllocator<T>>;
template<typename K, typename V>
using BudgetMap = std::map<K, V, std::less<K>, DeepBudgetAllocator<std::pair<const K, V>>>;
using BudgetSet = std::set<uint64_t, std::less<uint64_t>, DeepBudgetAllocator<uint64_t>>;

bool valid_window(const DeepRasterWindow &window)
{
  return window.width && window.height &&
         int64_t(window.x) + window.width - 1 <= std::numeric_limits<int32_t>::max() &&
         int64_t(window.y) + window.height - 1 <= std::numeric_limits<int32_t>::max() &&
         uint64_t(window.width) * window.height <=
             std::numeric_limits<size_t>::max() / sizeof(DeepCanonicalPixel);
}

struct Consumer {
  Consumer(const uint32_t id, const size_t rows, const DeepMemoryBudget &budget)
      : id(id), rows(rows, 0, DeepBudgetAllocator<uint64_t>(budget))
  {
  }
  uint32_t id;
  /* 0 available, UINT64_MAX consumed, otherwise the unique outstanding lease serial. */
  BudgetVector<uint64_t> rows;
  DeepOutputSink *sink = nullptr;
  bool closed = false;
};

struct BoundDomain {
  DeepDomainCompletion identity;
  DeepRasterWindow coverage;
};

struct DomainSourcePages {
  DomainSourcePages(const DeepRasterWindow &coverage, const DeepMemoryBudget &budget)
      : coverage(coverage), pages(DeepBudgetAllocator<DeepPageLease>(budget))
  {
  }
  DeepRasterWindow coverage;
  BudgetVector<DeepPageLease> pages;
};

template<typename Function> DeepStatus call_sink(Function &&function)
{
  try {
    return function();
  }
  catch (const std::exception &error) {
    return DeepStatus::fail(DeepErrorCode::OutputFailure,
                            std::string("Deep sink exception: ") + error.what());
  }
  catch (...) {
    return DeepStatus::fail(DeepErrorCode::OutputFailure, "Unknown Deep sink exception");
  }
}
}  // namespace

struct DeepFrameOwnerState {
  explicit DeepFrameOwnerState(const DeepMemoryBudget &budget)
      : pixels(DeepBudgetAllocator<DeepCanonicalPixel>(budget)),
        counts(DeepBudgetAllocator<uint64_t>(budget)),
        tile_owners(DeepBudgetAllocator<uint64_t>(budget)),
        consumers(DeepBudgetAllocator<Consumer>(budget)),
        samples(
            std::less<std::pair<size_t, uint32_t>>{},
            DeepBudgetAllocator<std::pair<const std::pair<size_t, uint32_t>, uint64_t>>(budget)),
        rays(std::less<uint64_t>{},
             DeepBudgetAllocator<std::pair<const uint64_t, size_t>>(budget)),
        domains(std::less<uint64_t>{}, DeepBudgetAllocator<uint64_t>(budget)),
        tiles(std::less<uint64_t>{}, DeepBudgetAllocator<uint64_t>(budget)),
        source_page_ids(std::less<uint64_t>{}, DeepBudgetAllocator<uint64_t>(budget)),
        canonical_pages(std::less<uint64_t>{},
                        DeepBudgetAllocator<std::pair<const uint64_t, size_t>>(budget)),
        in_flight(std::less<uint64_t>{},
                  DeepBudgetAllocator<std::pair<const uint64_t, BoundDomain>>(budget)),
        source_pages(DeepBudgetAllocator<DomainSourcePages>(budget))
  {
  }
  DeepFrameKey frame;
  DeepRasterWindow window = {};
  DeepFrameState state = DeepFrameState::Idle;
  DeepStatus error;
  std::shared_ptr<DeepPageStore> pages;
  BudgetVector<DeepCanonicalPixel> pixels;
  BudgetVector<uint64_t> counts, tile_owners;
  BudgetVector<Consumer> consumers;
  BudgetMap<std::pair<size_t, uint32_t>, uint64_t> samples;
  BudgetMap<uint64_t, size_t> rays;
  BudgetSet domains, tiles;
  BudgetSet source_page_ids;
  BudgetMap<uint64_t, size_t> canonical_pages;
  BudgetMap<uint64_t, BoundDomain> in_flight;
  BudgetVector<DomainSourcePages> source_pages;
  bool device_encoding = false;
  uint64_t next_lease = 1;
  bool canonical_retired = false;

  bool index(const int32_t x, const int32_t y, size_t &result) const
  {
    const int64_t dx = int64_t(x) - window.x, dy = int64_t(y) - window.y;
    if (dx < 0 || dy < 0 || uint64_t(dx) >= window.width || uint64_t(dy) >= window.height)
      return false;
    result = size_t(dy) * window.width + size_t(dx);
    return true;
  }
  /* Exact disjoint inclusive identity ranges, retained through frame close.
   * Native production IDs are contiguous; arbitrary admission gaps stay gaps.
   * Unlike per-sample ownership nodes this proof need not grow with every AA ray. */
  bool admit_ray(const uint64_t id)
  {
    auto right = rays.upper_bound(id);
    auto left = right == rays.begin() ? rays.end() : std::prev(right);
    if (left != rays.end() && left->second >= id)
      return false;
    const bool join_left = left != rays.end() && left->second < UINT64_MAX &&
                           left->second + 1 == id;
    const bool join_right = right != rays.end() && id < UINT64_MAX && right->first == id + 1;
    if (join_left) {
      left->second = join_right ? right->second : id;
      if (join_right)
        rays.erase(right);
    }
    else {
      rays.emplace(id, join_right ? right->second : id);
      if (join_right)
        rays.erase(right);
    }
    return true;
  }
  Consumer *consumer(const uint32_t id)
  {
    for (auto &consumer : consumers)
      if (consumer.id == id)
        return &consumer;
    return nullptr;
  }
};

struct DeepCanonicalRows {
  explicit DeepCanonicalRows(const DeepMemoryBudget &budget)
      : pixels(DeepBudgetAllocator<DeepCanonicalPixel>(budget))
  {
  }
  DeepFrameKey frame;
  std::weak_ptr<DeepFrameOwnerState> owner;
  int32_t first = 0;
  uint32_t count = 0;
  BudgetVector<DeepCanonicalPixel> pixels;
};

std::span<const DeepCanonicalPixel> DeepCanonicalRowLease::pixels() const
{
  return rows_ ? std::span<const DeepCanonicalPixel>(rows_->pixels.data(), rows_->pixels.size()) :
                 std::span<const DeepCanonicalPixel>{};
}
const DeepFrameKey *DeepCanonicalRowLease::frame_key() const
{
  return rows_ ? &rows_->frame : nullptr;
}
int32_t DeepCanonicalRowLease::first_row() const
{
  return rows_ ? rows_->first : 0;
}
uint32_t DeepCanonicalRowLease::row_count() const
{
  return rows_ ? rows_->count : 0;
}

DeepFramePipeline::DeepFramePipeline(const size_t memory_bytes) : budget_(memory_bytes) {}
DeepFramePipeline::DeepFramePipeline(const DeepMemoryBudget &budget) : budget_(budget) {}
DeepFramePipeline::~DeepFramePipeline() = default;

DeepStatus DeepFramePipeline::fail(const DeepErrorCode code, const std::string &message)
{
  if (!owner_) {
    if (startup_error_)
      startup_error_ = DeepStatus::fail(code, message);
    return startup_error_;
  }
  if (owner_->state == DeepFrameState::Failed || owner_->state == DeepFrameState::Cancelled)
    return owner_->error;
  owner_->error = DeepStatus::fail(code, message);
  owner_->state = DeepFrameState::Failed;
  return owner_->error;
}

DeepStatus DeepFramePipeline::active() const
{
  if (!owner_)
    return startup_error_ ? DeepStatus::fail(DeepErrorCode::InvalidState, "No Deep frame") :
                            startup_error_;
  if (owner_->state == DeepFrameState::Failed || owner_->state == DeepFrameState::Cancelled)
    return owner_->error;
  if (owner_->state != DeepFrameState::Capturing && owner_->state != DeepFrameState::InputsSealed)
    return DeepStatus::fail(DeepErrorCode::InvalidState, "Deep frame is not active");
  return {};
}

DeepStatus DeepFramePipeline::begin_frame(const DeepFrameKey &frame,
                                          const DeepRasterWindow &window,
                                          const std::span<const uint32_t> consumers)
{
  if (owner_ || !startup_error_)
    return DeepStatus::fail(DeepErrorCode::InvalidState, "Deep owner cannot begin a second frame");
  if (!frame.invocation || !valid_window(window) || consumers.empty())
    return fail(DeepErrorCode::InvalidLayout, "Invalid Deep frame/raster/consumer set");
  try {
    owner_ = std::allocate_shared<DeepFrameOwnerState>(
        DeepBudgetAllocator<DeepFrameOwnerState>(budget_), budget_);
    owner_->frame = frame;
    owner_->window = window;
    owner_->pages = std::allocate_shared<DeepPageStore>(
        DeepBudgetAllocator<DeepPageStore>(budget_), frame, budget_);
    const size_t pixel_count = size_t(window.width) * window.height;
    owner_->pixels.resize(pixel_count);
    owner_->counts.resize(pixel_count, 0);
    owner_->tile_owners.resize(pixel_count, 0);
    for (const uint32_t id : consumers) {
      if (owner_->consumer(id))
        return fail(DeepErrorCode::InvalidLayout, "Duplicate Deep consumer");
      owner_->consumers.emplace_back(id, window.height, budget_);
    }
    owner_->state = DeepFrameState::Capturing;
  }
  catch (const std::bad_alloc &) {
    return fail(DeepErrorCode::BudgetExceeded, "Deep frame quota exhausted");
  }
  return {};
}

DeepPageStore *DeepFramePipeline::pages()
{
  return owner_ ? owner_->pages.get() : nullptr;
}
DeepStatus DeepFramePipeline::check_producer_status(const DeepStatus &status)
{
  if (const DeepStatus running = active(); !running)
    return running;
  if (status || status.code == DeepErrorCode::NeedPage)
    return status;
  return fail(status.code, status.message);
}

DeepStatus DeepFramePipeline::enable_device_encoding()
{
  if (const auto status = active(); !status) return status;
  if (owner_->state != DeepFrameState::Capturing || !owner_->domains.empty() ||
      !owner_->in_flight.empty() || !owner_->tiles.empty())
    return fail(DeepErrorCode::InvalidState, "Device encoding must be fixed before capture");
  owner_->device_encoding = true;
  owner_->canonical_retired = true;
  return {};
}

DeepStatus DeepFramePipeline::commit_device_domain(const DeepDomainCompletion &domain)
{
  if (const auto status = active(); !status) return status;
  if (!owner_->device_encoding || owner_->state != DeepFrameState::Capturing ||
      !(domain.frame == owner_->frame) || owner_->domains.contains(domain.domain))
    return fail(DeepErrorCode::InvalidState, "Invalid device-only Deep domain commit");
  const auto bound = owner_->in_flight.find(domain.domain);
  if (bound == owner_->in_flight.end() ||
      bound->second.identity.device_generation != domain.device_generation ||
      bound->second.identity.layout_generation != domain.layout_generation ||
      bound->second.identity.attempt != domain.attempt)
    return fail(DeepErrorCode::InvalidReference, "Changed or unknown device Deep domain");
  if (!domain.producer_drained || domain.pending_primary || domain.pending_shadow || domain.pending_mnee)
    return fail(DeepErrorCode::Incomplete, "Device Deep producers have not joined");
  try { owner_->domains.insert(domain.domain); }
  catch (const std::bad_alloc &) { return fail(DeepErrorCode::BudgetExceeded, "Device domain registry quota"); }
  owner_->in_flight.erase(bound);
  return {};
}

DeepStatus DeepFramePipeline::close_device_encoded_tile(const DeepFrameKey &frame,
                                                         const uint64_t token,
                                                         const DeepRasterWindow &tile,
                                                         const std::span<const DeepCanonicalPixel> pixels)
{
  if (const auto status = active(); !status) return status;
  if (!owner_->device_encoding || owner_->state != DeepFrameState::Capturing ||
      !(frame == owner_->frame) || !token || owner_->tiles.contains(token) || !valid_window(tile) ||
      uint64_t(tile.width) * tile.height != pixels.size())
    return fail(DeepErrorCode::InvalidState, "Invalid device-encoded final Deep tile");
  for (const auto &[id, domain] : owner_->in_flight) {
    const auto &w = domain.coverage;
    if (int64_t(tile.x) < int64_t(w.x) + w.width && int64_t(w.x) < int64_t(tile.x) + tile.width &&
        int64_t(tile.y) < int64_t(w.y) + w.height && int64_t(w.y) < int64_t(tile.y) + tile.height)
      return fail(DeepErrorCode::Incomplete, "Device final tile has unfinished native domains");
  }
  for (size_t i = 0; i < pixels.size(); i++) {
    const auto &pixel = pixels[i]; size_t index;
    if (pixel.x != int64_t(tile.x) + i % tile.width ||
        pixel.y != int64_t(tile.y) + i / tile.width || !owner_->index(pixel.x, pixel.y, index) ||
        !pixel.native_population || pixel.knots || pixel.additional_knots || owner_->tile_owners[index])
      return fail(DeepErrorCode::InvalidPayload, "Invalid or repeated device final pixel metadata");
  }
  try { owner_->tiles.insert(token); }
  catch (const std::bad_alloc &) { return fail(DeepErrorCode::BudgetExceeded, "Device tile registry quota"); }
  for (const auto &pixel : pixels) {
    size_t index; owner_->index(pixel.x, pixel.y, index);
    owner_->pixels[index] = pixel; owner_->counts[index] = pixel.native_population;
    owner_->tile_owners[index] = token;
  }
  return {};
}

DeepStatus DeepFramePipeline::close_bounded_device_tile(const DeepFrameKey &frame,
                                                        const uint64_t token,
                                                        const DeepRasterWindow &tile)
{
  if (const auto status = active(); !status) {
    return status;
  }
  size_t first, last;
  if (!owner_->device_encoding || owner_->state != DeepFrameState::Capturing ||
      !(frame == owner_->frame) || !token || owner_->tiles.contains(token) ||
      !owner_->in_flight.empty() || !valid_window(tile) ||
      !owner_->index(tile.x, tile.y, first) ||
      !owner_->index(int64_t(tile.x) + tile.width - 1, int64_t(tile.y) + tile.height - 1, last))
  {
    return fail(DeepErrorCode::InvalidState, "Invalid bounded Deep final tile");
  }
  for (uint32_t y = 0; y < tile.height; ++y) {
    for (uint32_t x = 0; x < tile.width; ++x) {
      if (owner_->tile_owners[first + size_t(y) * owner_->window.width + x]) {
        return fail(DeepErrorCode::DuplicatePixel, "Repeated bounded Deep pixel owner");
      }
    }
  }
  try {
    owner_->tiles.insert(token);
  }
  catch (const std::bad_alloc &) {
    return fail(DeepErrorCode::BudgetExceeded, "Bounded Deep tile registry quota");
  }
  for (uint32_t y = 0; y < tile.height; ++y) {
    for (uint32_t x = 0; x < tile.width; ++x) {
      const size_t index = first + size_t(y) * owner_->window.width + x;
      owner_->pixels[index].x = int64_t(tile.x) + x;
      owner_->pixels[index].y = int64_t(tile.y) + y;
      owner_->tile_owners[index] = token;
    }
  }
  return {};
}

DeepStatus DeepFramePipeline::bind_native_domain(const DeepDomainCompletion &domain,
                                                 const DeepRasterWindow &coverage)
{
  if (const DeepStatus status = active(); !status)
    return status;
  if (owner_->state != DeepFrameState::Capturing || !(domain.frame == owner_->frame))
    return fail(DeepErrorCode::InvalidFrame, "Foreign or sealed Deep domain binding");
  size_t first, last;
  if (!domain.domain || !domain.device_generation || !domain.layout_generation ||
      !valid_window(coverage) || !owner_->index(coverage.x, coverage.y, first) ||
      !owner_->index(int32_t(int64_t(coverage.x) + coverage.width - 1),
                     int32_t(int64_t(coverage.y) + coverage.height - 1),
                     last))
    return fail(DeepErrorCode::InvalidLayout, "Invalid Deep domain coverage/identity");
  if (owner_->domains.contains(domain.domain) || owner_->in_flight.contains(domain.domain))
    return fail(DeepErrorCode::InvalidReference, "Deep domain identity already bound/committed");
  for (uint32_t y = 0; y < coverage.height; y++) {
    for (uint32_t x = 0; x < coverage.width; x++) {
      const size_t pixel = first + size_t(y) * owner_->window.width + x;
      if (owner_->tile_owners[pixel])
        return fail(DeepErrorCode::InvalidState, "Deep domain bound after pixel closure");
    }
  }
  try {
    owner_->in_flight.emplace(domain.domain, BoundDomain{domain, coverage});
  }
  catch (const std::bad_alloc &) {
    return fail(DeepErrorCode::BudgetExceeded, "Deep domain binding quota exhausted");
  }
  return {};
}

DeepStatus DeepFramePipeline::commit_native_domain(
    const DeepDomainCompletion &domain,
    const std::span<const DeepSampleAdmission> samples,
    const std::span<const DeepPageLease> pages)
{
  if (const DeepStatus status = active(); !status)
    return status;
  if (owner_->device_encoding)
    return fail(DeepErrorCode::InvalidState, "Device Deep cannot commit host numerical samples");
  if (owner_->state != DeepFrameState::Capturing || !(domain.frame == owner_->frame))
    return fail(DeepErrorCode::InvalidFrame, "Foreign or sealed Deep domain");
  if (!domain.domain || !domain.device_generation || !domain.layout_generation ||
      owner_->domains.contains(domain.domain))
    return fail(DeepErrorCode::InvalidReference, "Invalid or duplicate Deep domain identity");
  const auto bound = owner_->in_flight.find(domain.domain);
  if (bound == owner_->in_flight.end() ||
      bound->second.identity.device_generation != domain.device_generation ||
      bound->second.identity.layout_generation != domain.layout_generation ||
      bound->second.identity.attempt != domain.attempt)
    return fail(DeepErrorCode::InvalidReference,
                "Unbound or changed Deep domain generation/attempt");
  if (!domain.producer_drained || domain.pending_primary || domain.pending_shadow ||
      domain.pending_mnee)
    return fail(DeepErrorCode::Incomplete, "Deep domain producers have not joined");
  if (domain.native_admitted != samples.size())
    return fail(DeepErrorCode::PopulationMismatch,
                "Native admitted sample count differs from Deep domain");
  try {
    BudgetSet domain_rays(std::less<uint64_t>{}, DeepBudgetAllocator<uint64_t>(budget_));
    for (const auto &admission : samples) {
      size_t pixel;
      if (!admission.ray || !owner_->index(admission.sample.x, admission.sample.y, pixel) ||
          uint32_t(admission.sample.disposition) >
              uint32_t(DeepCameraDisposition::InvalidCameraBlack))
        return fail(DeepErrorCode::InvalidPayload, "Invalid Deep camera admission");
      const auto &coverage = bound->second.coverage;
      const int64_t dx = int64_t(admission.sample.x) - coverage.x;
      const int64_t dy = int64_t(admission.sample.y) - coverage.y;
      if (dx < 0 || dy < 0 || uint64_t(dx) >= coverage.width || uint64_t(dy) >= coverage.height)
        return fail(DeepErrorCode::InvalidReference,
                    "Deep sample lies outside bound native domain");
      if (owner_->tile_owners[pixel])
        return fail(DeepErrorCode::InvalidState, "Sample arrived after final pixel closure");
      /* RNG/subset metadata cannot turn the same effective native sample into extra population. */
      if (!owner_->samples.emplace(std::pair(pixel, admission.sample.sample), admission.ray)
               .second ||
          !owner_->admit_ray(admission.ray))
        return fail(DeepErrorCode::DuplicateSample, "Duplicate native sample or ray identity");
      if (owner_->counts[pixel] == std::numeric_limits<uint64_t>::max())
        return fail(DeepErrorCode::InvalidCount, "Deep population counter overflow");
      owner_->counts[pixel]++;
      domain_rays.insert(admission.ray);
    }
    for (const auto &page : pages) {
      if (!page || !(*page.frame_key() == owner_->frame) || !page.is_charged_to(budget_))
        return fail(DeepErrorCode::InvalidFrame, "Foreign Deep capture page");
      const auto &descriptor = *page.descriptor();
      if (!owner_->source_page_ids.insert(descriptor.page_id).second)
        return fail(DeepErrorCode::InvalidReference, "Deep source page published more than once");
      if (descriptor.first_error || descriptor.valid != descriptor.committed ||
          descriptor.committed > descriptor.written || descriptor.written > descriptor.reserved ||
          descriptor.reserved > descriptor.capacity)
        return fail(DeepErrorCode::InvalidCount, "Invalid Deep page publication counts");
      uint32_t seen = 0;
      for (uint32_t slot = 0; slot < descriptor.reserved; slot++) {
        const uint64_t handle = page.valid_handle_at(slot);
        if (!handle)
          continue;
        DeepRecordTag tag;
        std::span<const uint8_t> payload;
        if (const DeepStatus status = page.read(handle, tag, payload); !status)
          return fail(status.code, status.message);
        if (!domain_rays.contains(tag.ray) || tag.attempt != domain.attempt)
          return fail(DeepErrorCode::InvalidReference,
                      "Deep source refers to foreign sample/attempt");
        seen++;
      }
      if (seen != descriptor.valid)
        return fail(DeepErrorCode::InvalidCount, "Deep valid slots differ from published count");
    }
    if (!pages.empty()) {
      DomainSourcePages sources(bound->second.coverage, budget_);
      sources.pages.assign(pages.begin(), pages.end());
      owner_->source_pages.push_back(std::move(sources));
    }
    owner_->domains.insert(domain.domain);
    owner_->in_flight.erase(bound);
  }
  catch (const std::bad_alloc &) {
    return fail(DeepErrorCode::BudgetExceeded, "Deep domain quota exhausted");
  }
  return {};
}

DeepStatus DeepFramePipeline::close_render_tile(const DeepFrameKey &frame,
                                                const uint64_t token,
                                                const DeepRasterWindow &tile,
                                                const std::span<const DeepCanonicalPixel> pixels)
{
  if (const DeepStatus status = active(); !status)
    return status;
  if (owner_->device_encoding)
    return fail(DeepErrorCode::InvalidState, "Device Deep cannot publish CPU canonical curves");
  if (owner_->state != DeepFrameState::Capturing || !(frame == owner_->frame))
    return fail(DeepErrorCode::InvalidFrame, "Foreign or sealed Deep final tile");
  if (!token || !valid_window(tile) || uint64_t(tile.width) * tile.height != pixels.size())
    return fail(DeepErrorCode::InvalidLayout, "Invalid Deep final tile");
  size_t first, last;
  if (!owner_->index(tile.x, tile.y, first) ||
      !owner_->index(int32_t(int64_t(tile.x) + tile.width - 1),
                     int32_t(int64_t(tile.y) + tile.height - 1),
                     last))
    return fail(DeepErrorCode::InvalidLayout, "Deep final tile lies outside frame");
  if (owner_->tiles.contains(token))
    return fail(DeepErrorCode::DuplicatePixel, "Deep final tile token reused");
  for (const auto &[id, domain] : owner_->in_flight) {
    const auto &coverage = domain.coverage;
    if (int64_t(tile.x) < int64_t(coverage.x) + coverage.width &&
        int64_t(coverage.x) < int64_t(tile.x) + tile.width &&
        int64_t(tile.y) < int64_t(coverage.y) + coverage.height &&
        int64_t(coverage.y) < int64_t(tile.y) + tile.height)
      return fail(DeepErrorCode::Incomplete, "Final Deep tile still has an in-flight domain");
  }
  for (size_t i = 0; i < pixels.size(); i++) {
    const auto &pixel = pixels[i];
    const int32_t x = int32_t(int64_t(tile.x) + i % tile.width);
    const int32_t y = int32_t(int64_t(tile.y) + i / tile.width);
    size_t index;
    if (pixel.x != x || pixel.y != y || !owner_->index(x, y, index))
      return fail(DeepErrorCode::InvalidLayout, "Deep final tile pixels are not row-major");
    if (owner_->tile_owners[index])
      return fail(DeepErrorCode::DuplicatePixel, "Deep pixel already has a final owner");
    if (!pixel.native_population || pixel.native_population != owner_->counts[index])
      return fail(DeepErrorCode::PopulationMismatch,
                  "Final Deep population differs from native samples");
    if (!pixel.knots || !(*pixel.knots.frame_key() == frame) ||
        !pixel.knots.is_charged_to(budget_) ||
        pixel.knots.descriptor()->kind != DeepRecordKind::DepthKnot)
      return fail(DeepErrorCode::InvalidReference,
                  "Final Deep pixel has no owned canonical knots");
    if (pixel.additional_knots) {
      for (const DeepPageLease &page : pixel.additional_knots->pages()) {
        if (!page || !(*page.frame_key() == frame) || !page.is_charged_to(budget_) ||
            page.descriptor()->kind != DeepRecordKind::DepthKnot)
          return fail(DeepErrorCode::InvalidReference, "Foreign canonical continuation page");
      }
    }
  }
  try {
    owner_->tiles.insert(token);
    for (const auto &pixel : pixels) {
      size_t index;
      owner_->index(pixel.x, pixel.y, index);
      if (!owner_->canonical_pages.emplace(pixel.knots.descriptor()->page_id, index).second)
        return fail(DeepErrorCode::InvalidReference, "Canonical page assigned to multiple pixels");
      if (pixel.additional_knots) {
        for (const DeepPageLease &page : pixel.additional_knots->pages()) {
          if (!owner_->canonical_pages.emplace(page.descriptor()->page_id, index).second)
            return fail(DeepErrorCode::InvalidReference,
                        "Canonical continuation page assigned more than once");
        }
      }
      owner_->tile_owners[index] = token;
      owner_->pixels[index] = pixel;
      /* The immutable closed-pixel guard now rejects every later admission.
       * Per-sample duplicates no longer require individual tree nodes. Global
       * ray identity is still proven by exact inclusive ranges above. */
      owner_->samples.erase(owner_->samples.lower_bound({index, 0}),
                            owner_->samples.upper_bound({index, UINT32_MAX}));
    }
    std::erase_if(owner_->source_pages, [&](const DomainSourcePages &sources) {
      const auto &coverage = sources.coverage;
      if (int64_t(tile.x) >= int64_t(coverage.x) + coverage.width ||
          int64_t(coverage.x) >= int64_t(tile.x) + tile.width ||
          int64_t(tile.y) >= int64_t(coverage.y) + coverage.height ||
          int64_t(coverage.y) >= int64_t(tile.y) + tile.height)
        return false;
      for (uint32_t y = 0; y < coverage.height; ++y)
        for (uint32_t x = 0; x < coverage.width; ++x) {
          size_t index;
          if (!owner_->index(
                  int32_t(int64_t(coverage.x) + x), int32_t(int64_t(coverage.y) + y), index) ||
              !owner_->tile_owners[index])
            return false;
        }
      return true;
    });
  }
  catch (const std::bad_alloc &) {
    return fail(DeepErrorCode::BudgetExceeded, "Deep tile quota exhausted");
  }
  return {};
}

DeepStatus DeepFramePipeline::retire_canonical_tile(const DeepRasterWindow &tile)
{
  if (const auto status = active(); !status)
    return status;
  if (owner_->state != DeepFrameState::Capturing || !valid_window(tile))
    return fail(DeepErrorCode::InvalidState,
                "Canonical retirement requires a closed capture tile");
  for (uint32_t y = 0; y < tile.height; y++)
    for (uint32_t x = 0; x < tile.width; x++) {
      size_t index;
      if (!owner_->index(int32_t(int64_t(tile.x) + x), int32_t(int64_t(tile.y) + y), index) ||
          !owner_->tile_owners[index] || !owner_->pixels[index].knots)
        return fail(DeepErrorCode::Incomplete,
                    "Canonical retirement precedes final pixel closure");
    }
  for (uint32_t y = 0; y < tile.height; y++)
    for (uint32_t x = 0; x < tile.width; x++) {
      size_t index;
      owner_->index(int32_t(int64_t(tile.x) + x), int32_t(int64_t(tile.y) + y), index);
      auto &pixel = owner_->pixels[index];
      owner_->canonical_pages.erase(pixel.knots.descriptor()->page_id);
      if (pixel.additional_knots)
        for (const auto &page : pixel.additional_knots->pages())
          owner_->canonical_pages.erase(page.descriptor()->page_id);
      pixel.knots = {};
      pixel.additional_knots = {};
    }
  owner_->canonical_retired = true;
  return {};
}

DeepStatus DeepFramePipeline::seal_frame_inputs()
{
  if (const DeepStatus status = active(); !status)
    return status;
  if (owner_->state != DeepFrameState::Capturing)
    return fail(DeepErrorCode::InvalidState, "Deep frame inputs already sealed");
  if (!owner_->in_flight.empty())
    return fail(DeepErrorCode::Incomplete, "Deep frame still has admitted in-flight domains");
  if (std::find(owner_->tile_owners.begin(), owner_->tile_owners.end(), 0) !=
      owner_->tile_owners.end())
    return fail(DeepErrorCode::Incomplete, "Deep frame has unfinished pixels");
  if (const DeepStatus status = owner_->pages->publication_status(); !status)
    return fail(status.code, status.message);
  owner_->state = DeepFrameState::InputsSealed;
  return {};
}

DeepStatus DeepFramePipeline::acquire_closed_rows(const uint32_t consumer_id,
                                                  const int32_t first_row,
                                                  const uint32_t row_count,
                                                  DeepCanonicalRowLease &lease)
{
  if (const DeepStatus status = active(); !status)
    return status;
  auto *consumer = owner_->consumer(consumer_id);
  const int64_t first = int64_t(first_row) - owner_->window.y;
  if (!consumer || consumer->closed || first < 0 || !row_count ||
      uint64_t(first) + row_count > owner_->window.height)
    return fail(DeepErrorCode::InvalidLayout, "Invalid Deep consumer row range");
  if (lease)
    return fail(DeepErrorCode::InvalidState, "Cannot replace an outstanding Deep row lease");
  for (uint32_t r = 0; r < row_count; r++) {
    if (consumer->rows[size_t(first) + r])
      return fail(DeepErrorCode::InvalidState, "Deep consumer row already borrowed/consumed");
    for (uint32_t x = 0; x < owner_->window.width; x++) {
      if (!owner_->tile_owners[(size_t(first) + r) * owner_->window.width + x])
        return DeepStatus::fail(DeepErrorCode::Incomplete, "Deep row is not yet complete");
    }
  }
  if (owner_->next_lease == std::numeric_limits<uint64_t>::max())
    return fail(DeepErrorCode::InvalidCount, "Deep row lease identity exhausted");
  try {
    auto rows = std::allocate_shared<DeepCanonicalRows>(
        DeepBudgetAllocator<DeepCanonicalRows>(budget_), budget_);
    rows->frame = owner_->frame;
    rows->owner = owner_;
    rows->first = first_row;
    rows->count = row_count;
    const auto begin = owner_->pixels.begin() + size_t(first) * owner_->window.width;
    rows->pixels.assign(begin, begin + size_t(row_count) * owner_->window.width);
    lease.rows_ = rows;
    lease.consumer_ = consumer_id;
    lease.serial_ = owner_->next_lease++;
    for (uint32_t r = 0; r < row_count; r++)
      consumer->rows[size_t(first) + r] = lease.serial_;
  }
  catch (const std::bad_alloc &) {
    return fail(DeepErrorCode::BudgetExceeded, "Deep row lease quota exhausted");
  }
  return {};
}

DeepStatus DeepFramePipeline::canonical_view(DeepCanonicalRowLease &lease)
{
  if (const DeepStatus status = active(); !status)
    return status;
  if (owner_->canonical_retired)
    return DeepStatus::fail(DeepErrorCode::InvalidState,
                            "Canonical reference retired after all consumer encodings");
  if (owner_->state != DeepFrameState::InputsSealed)
    return DeepStatus::fail(DeepErrorCode::Incomplete,
                            "Full canonical view requires sealed frame inputs");
  if (lease)
    return fail(DeepErrorCode::InvalidState, "Cannot replace a canonical lease");
  try {
    auto rows = std::allocate_shared<DeepCanonicalRows>(
        DeepBudgetAllocator<DeepCanonicalRows>(budget_), budget_);
    rows->frame = owner_->frame;
    rows->owner = owner_;
    rows->first = owner_->window.y;
    rows->count = owner_->window.height;
    rows->pixels.assign(owner_->pixels.begin(), owner_->pixels.end());
    lease.rows_ = rows;
    /* Reference views are read-only and cannot be submitted as consumer transactions. */
    lease.serial_ = 0;
  }
  catch (const std::bad_alloc &) {
    return fail(DeepErrorCode::BudgetExceeded, "Canonical view quota exhausted");
  }
  return {};
}

DeepStatus DeepFramePipeline::consume_rows(const DeepCanonicalRowLease &lease,
                                           DeepOutputSink &sink)
{
  if (const DeepStatus status = active(); !status)
    return status;
  if (!lease || !lease.serial_ || lease.rows_->owner.lock() != owner_ ||
      !(lease.rows_->frame == owner_->frame))
    return fail(DeepErrorCode::InvalidReference, "Foreign Deep consumer lease");
  auto *consumer = owner_->consumer(lease.consumer_);
  const size_t first = size_t(int64_t(lease.first_row()) - owner_->window.y);
  if (!consumer || consumer->closed || (consumer->sink && consumer->sink != &sink))
    return fail(DeepErrorCode::InvalidState, "Deep consumer changed its output sink");
  for (uint32_t r = 0; r < lease.row_count(); r++)
    if (consumer->rows[first + r] != lease.serial_)
      return fail(DeepErrorCode::InvalidState, "Deep row lease already consumed");
  if (!consumer->sink) {
    consumer->sink = &sink;
    if (const DeepStatus status = call_sink(
            [&] { return sink.begin(owner_->frame, owner_->window); });
        !status)
      return fail(DeepErrorCode::OutputFailure, status.message);
  }
  if (const DeepStatus status = call_sink([&] { return sink.write_rows(lease); }); !status)
    return fail(DeepErrorCode::OutputFailure, status.message);
  for (uint32_t r = 0; r < lease.row_count(); r++)
    consumer->rows[first + r] = std::numeric_limits<uint64_t>::max();
  return {};
}

DeepStatus DeepFramePipeline::close_consumer(const uint32_t id, DeepOutputSink &sink)
{
  if (const DeepStatus status = active(); !status)
    return status;
  auto *consumer = owner_->consumer(id);
  if (!consumer || consumer->closed || consumer->sink != &sink)
    return fail(DeepErrorCode::InvalidState, "Invalid Deep consumer close");
  for (const auto row : consumer->rows)
    if (row != std::numeric_limits<uint64_t>::max())
      return fail(DeepErrorCode::Incomplete, "Deep consumer still has unfinished rows");
  if (const DeepStatus status = call_sink([&] { return sink.finish_checked(); }); !status)
    return fail(DeepErrorCode::OutputFailure, status.message);
  consumer->closed = true;
  return {};
}

bool DeepFramePipeline::consumer_closed(const uint32_t id) const
{
  if (!owner_ || !owner_->error)
    return false;
  const auto *consumer = owner_->consumer(id);
  return consumer && consumer->closed;
}

bool DeepFramePipeline::all_consumers_closed() const
{
  return owner_ && owner_->error && !owner_->consumers.empty() &&
         std::all_of(owner_->consumers.begin(),
                     owner_->consumers.end(),
                     [](const Consumer &consumer) { return consumer.closed; });
}

DeepStatus DeepFramePipeline::replay_consumer(const uint32_t id)
{
  if (!owner_ || !owner_->error)
    return error();
  auto *consumer = owner_->consumer(id);
  if (!consumer || !consumer->closed ||
      (owner_->state != DeepFrameState::InputsSealed && owner_->state != DeepFrameState::Complete))
    return fail(DeepErrorCode::InvalidState,
                "Deep output replay requires a checked, sealed consumer");
  std::fill(consumer->rows.begin(), consumer->rows.end(), 0);
  consumer->sink = nullptr;
  consumer->closed = false;
  /* Only an output attempt reopens. Native inputs stay sealed and old row lease
   * serials cannot be reused; new admission still requires Capturing. */
  owner_->state = DeepFrameState::InputsSealed;
  return {};
}

DeepStatus DeepFramePipeline::finish_frame()
{
  if (const DeepStatus status = active(); !status)
    return status;
  if (owner_->state != DeepFrameState::InputsSealed)
    return fail(DeepErrorCode::Incomplete, "Deep frame inputs not sealed");
  for (const auto &consumer : owner_->consumers)
    if (!consumer.closed)
      return fail(DeepErrorCode::Incomplete, "Deep output has not checked-close completed");
  owner_->state = DeepFrameState::Complete;
  return {};
}

DeepStatus DeepFramePipeline::cancel()
{
  if (const DeepStatus status = active(); !status)
    return status;
  owner_->error = DeepStatus::fail(DeepErrorCode::Cancelled, "Deep frame cancelled");
  owner_->state = DeepFrameState::Cancelled;
  return owner_->error;
}

DeepFrameState DeepFramePipeline::state() const
{
  return owner_ ? owner_->state : (startup_error_ ? DeepFrameState::Idle : DeepFrameState::Failed);
}
const DeepStatus &DeepFramePipeline::error() const
{
  return owner_ ? owner_->error : startup_error_;
}
size_t DeepFramePipeline::bytes_used() const
{
  return budget_.used();
}

CCL_NAMESPACE_END
