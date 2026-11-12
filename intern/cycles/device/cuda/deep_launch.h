/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <cstdint>
#include <limits>

#include "util/defines.h"

CCL_NAMESPACE_BEGIN

/* Host-only short-wave thresholds. Only the existing independent native shading
 * entries may use this; collective and OptiX entry points keep their own launch. */
struct DeepNativeLaunchScale {
  int one_warp_wave = 0;
  int two_warp_wave = 0;

  void initialize(const int multiprocessors)
  {
    const int64_t sms = multiprocessors > 0 ? multiprocessors : 0;
    const int64_t limit = std::numeric_limits<int>::max();
    one_warp_wave = int(sms * 32 < limit ? sms * 32 : limit);
    two_warp_wave = int(sms * 64 < limit ? sms * 64 : limit);
  }

  int select(const int work_size, const int original_block) const
  {
    if (one_warp_wave == 0 || work_size <= 0 || (original_block != 64 && original_block != 128)) {
      return original_block;
    }
    if (work_size <= one_warp_wave) {
      return 32;
    }
    if (original_block == 128 && work_size <= two_warp_wave) {
      return 64;
    }
    return original_block;
  }
};

CCL_NAMESPACE_END
