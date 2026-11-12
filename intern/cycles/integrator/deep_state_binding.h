/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <type_traits>
#include <utility>

#include "util/defines.h"

CCL_NAMESPACE_BEGIN

/* One-shot cleanup for borrowed device state. A failed drain forbids pointer mutation;
 * copy and post-drain must both succeed before restoration can be acknowledged.
 * The caller still rejects completion on its latched logical error or cancellation. */
template<typename Drain, typename Copy> class DeepStateBindingGuard {
  static_assert(std::is_nothrow_invocable_r_v<bool, Drain>);
  static_assert(std::is_nothrow_invocable_r_v<bool, Copy>);
  static_assert(std::is_nothrow_move_constructible_v<Drain>);
  static_assert(std::is_nothrow_move_constructible_v<Copy>);

 public:
  DeepStateBindingGuard(Drain drain, Copy copy) noexcept
      : drain_(std::move(drain)), copy_(std::move(copy))
  {
  }
  DeepStateBindingGuard(const DeepStateBindingGuard &) = delete;
  DeepStateBindingGuard &operator=(const DeepStateBindingGuard &) = delete;

  bool restore() noexcept
  {
    if (!armed_)
      return restored_;
    armed_ = false;
    if (!drain_())
      return false;
    const bool copied = copy_();
    const bool joined = drain_();
    restored_ = copied && joined;
    return restored_;
  }

  ~DeepStateBindingGuard()
  {
    restore();
  }

 private:
  Drain drain_;
  Copy copy_;
  bool armed_ = true;
  bool restored_ = false;
};

CCL_NAMESPACE_END
