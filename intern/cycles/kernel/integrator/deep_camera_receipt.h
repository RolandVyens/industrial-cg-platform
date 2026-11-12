/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "util/types.h"

CCL_NAMESPACE_BEGIN

/* A diagnostic input receipt, separate from Deep output events and Beauty path ownership.
 * Scalar fields give host, CUDA and OptiX the same lossless layout. A failed camera sample
 * contributes to the population but has no valid Ray: never read its uninitialized Ray. */
enum DeepCameraReceiptFlag {
  DEEP_CAMERA_RECEIPT_RAY_VALID = 1,
  DEEP_CAMERA_RECEIPT_ZERO_THROUGHPUT = 2,
  DEEP_CAMERA_RECEIPT_CACHE_RETRY = 4,
};

struct DeepCameraReceipt {
  uint32_t pixel;
  uint32_t sample;
  uint32_t rng_pixel;
  uint32_t rng_offset;
  uint32_t flags;
  int32_t x;
  int32_t y;
  float population_weight;
  float origin[3];
  float direction[3];
  float tmin;
  float tmax;
  float time;
  float dP;
  float dD;
  float throughput[3];
};

static_assert(sizeof(DeepCameraReceipt) == 88, "Camera receipt ABI must match the backing schema");

CCL_NAMESPACE_END
