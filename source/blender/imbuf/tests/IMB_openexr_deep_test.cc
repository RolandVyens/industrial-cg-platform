/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <vector>

#include <OpenImageIO/deepdata.h>
#include <OpenImageIO/imageio.h>

#include "testing/testing.h"

#include "CLG_log.h"

#include "BLI_path_utils.hh"
#include "BLI_tempfile.h"

#include "IMB_deep_sample.hh"
#include "IMB_openexr.hh"

namespace blender::imbuf::tests {

class DeepExrWriteTest : public testing::Test {
 protected:
  static void SetUpTestSuite()
  {
    CLG_init();
  }

  static void TearDownTestSuite()
  {
    CLG_exit();
  }
};

TEST_F(DeepExrWriteTest, rejects_pixel_vector_size_mismatch_before_writing)
{
  std::vector<std::vector<DeepSample>> deep_data(2);

  EXPECT_FALSE(IMB_exr_save_deep(deep_data, 1, 1, "deep_invalid_pixels.exr", 0, false, false));
}

TEST_F(DeepExrWriteTest, merges_samples_in_bounded_writer_storage)
{
  char temp_directory[FILE_MAX];
  BLI_temp_directory_path_get(temp_directory, sizeof(temp_directory));
  char filepath[FILE_MAX];
  BLI_path_join(filepath, sizeof(filepath), temp_directory, "deep_writer_merge_test.exr");
  std::vector<std::vector<DeepSample>> deep_data(1);
  deep_data[0] = {
      {0.25f, 0.0f, 0.0f, 0.5f, 1.0f, 1.0f},
      {0.0f, 0.25f, 0.0f, 0.5f, 1.001f, 1.001f},
  };

  ASSERT_TRUE(IMB_exr_save_deep(deep_data,
                                1,
                                1,
                                filepath,
                                0,
                                false,
                                false,
                                false,
                                0,
                                0,
                                0,
                                0,
                                0,
                                0,
                                0.01f,
                                0.01f));

  std::unique_ptr<OIIO::ImageInput> image_input = OIIO::ImageInput::open(filepath);
  ASSERT_NE(image_input, nullptr);
  OIIO::DeepData written_data;
  ASSERT_TRUE(image_input->read_native_deep_image(0, 0, written_data));
  EXPECT_EQ(written_data.samples(0), 1);
  image_input->close();
  EXPECT_EQ(deep_data[0].size(), 2u);
}

}  // namespace blender::imbuf::tests
