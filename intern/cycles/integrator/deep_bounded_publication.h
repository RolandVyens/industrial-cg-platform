/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "session/deep_frame_production.h"

CCL_NAMESPACE_BEGIN

/* One producer per slot. stage() runs only after that Work's DMA joins;
 * publish() runs only on the PathTrace owner after producer threads join.
 * No queue, allocation, payload copy or lock is introduced by the handoff. */
class DeepBoundedPublicationSlot {
 public:
  bool stage(std::unique_ptr<DeepBoundedHostTile> &source)
  {
    if (pending_ || !source) return false;
    pending_ = std::move(source);
    return true;
  }
  bool publish(DeepFrameCaptureConsumer &consumer)
  {
    if (!pending_) return true;
    if (!consumer.publish_bounded_tile(std::move(*pending_))) return false;
    pending_.reset();
    return true;
  }
  bool pending() const { return bool(pending_); }

 private:
  std::unique_ptr<DeepBoundedHostTile> pending_;
};

CCL_NAMESPACE_END
