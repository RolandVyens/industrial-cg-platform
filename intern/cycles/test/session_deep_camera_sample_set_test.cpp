/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include <cstdint>
#include <limits>

#include "testing/testing.h"

#include "session/deep_camera_sample_set.h"

CCL_NAMESPACE_BEGIN

TEST(DeepCameraSampleSet, sparse_ids_have_bounded_storage)
{
  DeepCameraSampleSet sample_set(64);

  EXPECT_TRUE(sample_set.add(0));
  EXPECT_TRUE(sample_set.add(4095));
  EXPECT_TRUE(sample_set.add(std::numeric_limits<uint32_t>::max()));
  EXPECT_FALSE(sample_set.add(4095));
  EXPECT_EQ(sample_set.capacity(), 128u);
  EXPECT_EQ(sample_set.storage_bytes(), 128u * sizeof(uint64_t));
}

TEST(DeepCameraSampleSet, preserves_first_occurrence_contract)
{
  DeepCameraSampleSet sample_set(4);

  EXPECT_TRUE(sample_set.add(17));
  EXPECT_TRUE(sample_set.add(3));
  EXPECT_FALSE(sample_set.add(17));
  EXPECT_TRUE(sample_set.add(99));
  EXPECT_FALSE(sample_set.add(3));
}

CCL_NAMESPACE_END
