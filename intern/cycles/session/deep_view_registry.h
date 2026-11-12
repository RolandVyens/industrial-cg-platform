/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "session/deep_canonical_encode.h"
#include "session/deep_page_store.h"

CCL_NAMESPACE_BEGIN

/* Consumer identity is still validated separately. Equal immutable source/config/codec
 * produces the same payload even for separate registered node instances. */
struct DeepOutputIdentity {
  DeepFrameKey frame;
  DeepCanonicalEncodeConfig config;
  int compression = 0;
  bool operator==(const DeepOutputIdentity &) const = default;
};

/* Native view metadata is frozen before dispatch; each child is attached once.
 * The complete registry is published only after every view has joined its inputs. */
template<typename Output> class DeepViewRegistry {
 public:
  struct Entry {
    std::array<char, 64> name = {};
    std::array<char, 1024> direct_path = {};
    std::shared_ptr<Output> output;
  };
  explicit DeepViewRegistry(const DeepMemoryBudget &budget)
      : entries_(DeepBudgetAllocator<Entry>(budget))
  {
  }
  DeepStatus register_view(const std::string_view name, const std::string_view direct_path)
  {
    if (binding_started_ || sealed_)
      return DeepStatus::fail(DeepErrorCode::InvalidState, "Deep view registration is frozen");
    if (name.size() >= Entry().name.size() || direct_path.size() >= Entry().direct_path.size() ||
        name.find('\0') != std::string_view::npos ||
        direct_path.find('\0') != std::string_view::npos)
      return DeepStatus::fail(DeepErrorCode::InvalidLayout,
                              "Deep view name/path exceeds native bounds");
    for (const auto &entry : entries_)
      if (name == entry.name.data())
        return DeepStatus::fail(DeepErrorCode::DuplicateSample, "Duplicate native Deep view name");
    try {
      Entry entry;
      std::copy(name.begin(), name.end(), entry.name.begin());
      std::copy(direct_path.begin(), direct_path.end(), entry.direct_path.begin());
      entries_.push_back(std::move(entry));
      return {};
    }
    catch (const std::bad_alloc &) {
      return DeepStatus::fail(DeepErrorCode::BudgetExceeded, "Deep view registry quota exhausted");
    }
  }
  DeepStatus bind_view(const std::string_view name, std::shared_ptr<Output> output)
  {
    if (sealed_ || !output)
      return DeepStatus::fail(DeepErrorCode::InvalidState, "Invalid or sealed Deep view binding");
    Entry *target = nullptr;
    for (auto &entry : entries_) {
      if (name == entry.name.data())
        target = &entry;
      if (entry.output == output)
        return DeepStatus::fail(DeepErrorCode::InvalidReference,
                                "Deep views cannot alias one child");
    }
    if (!target || target->output)
      return DeepStatus::fail(DeepErrorCode::InvalidReference,
                              "Unknown or already bound Deep view");
    target->output = std::move(output);
    binding_started_ = true;
    return {};
  }
  DeepStatus seal()
  {
    if (entries_.empty())
      return DeepStatus::fail(DeepErrorCode::Incomplete, "Deep view collection is empty");
    for (const auto &entry : entries_)
      if (!entry.output)
        return DeepStatus::fail(DeepErrorCode::Incomplete, "Deep view has no completed child");
    binding_started_ = sealed_ = true;
    return {};
  }
  std::shared_ptr<Output> find(const std::string_view name) const
  {
    if (sealed_)
      for (const auto &entry : entries_)
        if (name == entry.name.data())
          return entry.output;
    return {};
  }
  std::span<const Entry> entries() const
  {
    return entries_;
  }

 private:
  std::vector<Entry, DeepBudgetAllocator<Entry>> entries_;
  bool binding_started_ = false;
  bool sealed_ = false;
};

CCL_NAMESPACE_END
