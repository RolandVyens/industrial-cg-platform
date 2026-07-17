/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <string>

#include "BKE_blender_version.h"
#include "BKE_gtest_base.hh"

#include "testing/testing.h"

namespace blender::bke::tests {

class BlenderVersionTest : public BlenderGTestBase {};

TEST_F(BlenderVersionTest, product_version_owns_default_and_localized_brand_formatting)
{
  char localized_product_version[128];
  BKE_blender_product_version_string_from_brand(
      localized_product_version, sizeof(localized_product_version), "Localized Product");

  EXPECT_EQ(std::string(BKE_blender_product_version_string()),
            "Blender " + std::string(BKE_blender_version_string()) + " " +
                BLENDER_VERSION_BRAND_SUFFIX);
  EXPECT_EQ(std::string(localized_product_version),
            "Blender " + std::string(BKE_blender_version_string()) + " Localized Product");
}

}  // namespace blender::bke::tests
