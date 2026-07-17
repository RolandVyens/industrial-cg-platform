/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "device/optix/cache.h"

CCL_NAMESPACE_BEGIN

static string optix_cache_path_join(const OptixCachePlatform platform,
                                    const string &root,
                                    const string &relative_path)
{
  if (root.empty()) {
    return string();
  }

  const char separator = platform == OptixCachePlatform::Windows ? '\\' : '/';
  if (root.back() == '/' || root.back() == '\\') {
    return root + relative_path;
  }
  return root + separator + relative_path;
}

OptixCachePolicy optix_cache_policy_resolve(const OptixCachePlatform platform,
                                            const OptixCacheEnvironment &environment)
{
  if (!environment.optix_cache_path.empty()) {
    return {};
  }

  string root;
  string relative_path;
  switch (platform) {
    case OptixCachePlatform::Windows:
      root = environment.local_app_data;
      relative_path = "IndustrialCGPlatform\\Cache\\OptiX";
      break;
    case OptixCachePlatform::MacOS:
      root = environment.home;
      relative_path = "Library/Caches/IndustrialCGPlatform/OptiX";
      break;
    case OptixCachePlatform::Linux:
      if (!environment.xdg_cache_home.empty()) {
        root = environment.xdg_cache_home;
        relative_path = "industrial-cg-platform/optix";
      }
      else {
        root = environment.home;
        relative_path = ".cache/industrial-cg-platform/optix";
      }
      break;
  }

  if (root.empty()) {
    return {};
  }
  return {true, optix_cache_path_join(platform, root, relative_path)};
}

CCL_NAMESPACE_END
