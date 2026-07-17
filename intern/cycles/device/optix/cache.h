/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "util/string.h"

CCL_NAMESPACE_BEGIN

enum class OptixCachePlatform { Windows, MacOS, Linux };

struct OptixCacheEnvironment {
  string optix_cache_path;
  string local_app_data;
  string home;
  string xdg_cache_home;
};

struct OptixCachePolicy {
  bool use_product_path = false;
  string path;
};

OptixCachePolicy optix_cache_policy_resolve(OptixCachePlatform platform,
                                            const OptixCacheEnvironment &environment);

CCL_NAMESPACE_END
