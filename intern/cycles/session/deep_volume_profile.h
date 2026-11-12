/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "session/deep_buffers.h"

CCL_NAMESPACE_BEGIN

struct DeepVolumeProfileSample {
  float3 rgb;
  float alpha;
  float z;
  float z_back;
};

/**
 * Resolve the statistical population used to average camera-ray transmittance.
 * Deep buffer saturation does not prove that unretained camera samples contained volume events,
 * so it must not shrink the rendered camera-sample population.
 */
int deep_volume_profile_camera_population(int rendered_camera_samples,
                                          int retained_deep_samples,
                                          int deep_sample_capacity);

/**
 * Reconstruct one deterministic volume profile by averaging the transmittance of camera samples.
 * Returns false when the input does not carry the required volume/camera-sample metadata.
 */
bool deep_build_volume_profile(const DeepSampleData *sample_data,
                               size_t offset,
                               int count,
                               int total_camera_samples,
                               int max_profile_samples,
                               float3 target_rgb,
                               float target_alpha,
                               vector<DeepVolumeProfileSample> &profile);

CCL_NAMESPACE_END
