/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "util/vector.h"

CCL_NAMESPACE_BEGIN

class DeepCameraSampleSet {
 public:
  explicit DeepCameraSampleSet(const size_t expected_samples)
  {
    size_t capacity = 8;
    const size_t target_capacity = expected_samples * 2;
    while (capacity < target_capacity) {
      capacity *= 2;
    }
    keys_.assign(capacity, 0);
  }

  bool add(const uint32_t camera_sample)
  {
    const uint64_t key = uint64_t(camera_sample) + 1;
    const size_t mask = keys_.size() - 1;
    size_t slot = size_t(key * 0x9e3779b97f4a7c15ULL) & mask;
    for (;;) {
      if (keys_[slot] == 0) {
        keys_[slot] = key;
        return true;
      }
      if (keys_[slot] == key) {
        return false;
      }
      slot = (slot + 1) & mask;
    }
  }

  size_t capacity() const
  {
    return keys_.size();
  }

  size_t storage_bytes() const
  {
    return keys_.capacity() * sizeof(uint64_t);
  }

 private:
  vector<uint64_t> keys_;
};

CCL_NAMESPACE_END
