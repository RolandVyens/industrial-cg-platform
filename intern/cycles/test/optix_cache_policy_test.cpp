/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include <gtest/gtest.h>

#include "device/optix/cache.h"

CCL_NAMESPACE_BEGIN

TEST(OptixCachePolicy, UserPathKeepsOptixOwnedPolicy)
{
  const OptixCacheEnvironment environment = {
      "/user/optix", "C:\\Users\\artist\\AppData\\Local", "/Users/artist", "/cache"};
  const OptixCachePolicy policy = optix_cache_policy_resolve(OptixCachePlatform::Windows,
                                                             environment);

  EXPECT_FALSE(policy.use_product_path);
  EXPECT_TRUE(policy.path.empty());
}

TEST(OptixCachePolicy, WindowsUsesLocalAppData)
{
  const OptixCacheEnvironment environment = {"", "C:\\Users\\artist\\AppData\\Local", "", ""};
  const OptixCachePolicy policy = optix_cache_policy_resolve(OptixCachePlatform::Windows,
                                                             environment);

  EXPECT_TRUE(policy.use_product_path);
  EXPECT_EQ(policy.path, "C:\\Users\\artist\\AppData\\Local\\IndustrialCGPlatform\\Cache\\OptiX");
}

TEST(OptixCachePolicy, MacOSUsesHomeCache)
{
  const OptixCacheEnvironment environment = {"", "", "/Users/artist", ""};
  const OptixCachePolicy policy = optix_cache_policy_resolve(OptixCachePlatform::MacOS,
                                                             environment);

  EXPECT_TRUE(policy.use_product_path);
  EXPECT_EQ(policy.path, "/Users/artist/Library/Caches/IndustrialCGPlatform/OptiX");
}

TEST(OptixCachePolicy, LinuxPrefersXdgCacheHome)
{
  const OptixCacheEnvironment environment = {"", "", "/home/artist", "/var/cache/artist"};
  const OptixCachePolicy policy = optix_cache_policy_resolve(OptixCachePlatform::Linux,
                                                             environment);

  EXPECT_TRUE(policy.use_product_path);
  EXPECT_EQ(policy.path, "/var/cache/artist/industrial-cg-platform/optix");
}

TEST(OptixCachePolicy, LinuxFallsBackToHome)
{
  const OptixCacheEnvironment environment = {"", "", "/home/artist", ""};
  const OptixCachePolicy policy = optix_cache_policy_resolve(OptixCachePlatform::Linux,
                                                             environment);

  EXPECT_TRUE(policy.use_product_path);
  EXPECT_EQ(policy.path, "/home/artist/.cache/industrial-cg-platform/optix");
}

TEST(OptixCachePolicy, MissingPlatformRootDisablesProductPath)
{
  const OptixCacheEnvironment environment;

  EXPECT_FALSE(
      optix_cache_policy_resolve(OptixCachePlatform::Windows, environment).use_product_path);
  EXPECT_FALSE(
      optix_cache_policy_resolve(OptixCachePlatform::MacOS, environment).use_product_path);
  EXPECT_FALSE(
      optix_cache_policy_resolve(OptixCachePlatform::Linux, environment).use_product_path);
}

CCL_NAMESPACE_END
