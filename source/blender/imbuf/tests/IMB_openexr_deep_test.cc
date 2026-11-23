/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

#include <OpenImageIO/deepdata.h>
#include <OpenImageIO/imageio.h>

#include "testing/testing.h"

#include "CLG_log.h"
#include "DNA_scene_types.h"

#include "BLI_path_utils.hh"
#include "BLI_tempfile.hh"
#include "BLI_vector.hh"

#include "IMB_deep_sample.hh"
#include "IMB_imbuf_types.hh"
#include "IMB_openexr.hh"
#include "intern/openexr/openexr_api.h"
#include "intern/openexr/openexr_deep_core.hh"

namespace blender::imbuf::tests {

static std::string test_output_directory()
{
  if (const char *root = std::getenv("BLENDER_DEEP_EXR_TEST_OUTPUT_ROOT")) {
    return root;
  }
  char directory[FILE_MAX];
  BLI_temp_directory_path_get(directory, sizeof(directory));
  return directory;
}

class DeepExrWriteTest : public testing::Test {
 protected:
  static void SetUpTestSuite()
  {
    CLG_init();
    imb_initopenexr();
  }

  static void TearDownTestSuite()
  {
    imb_exitopenexr();
    CLG_exit();
  }
};

TEST_F(DeepExrWriteTest, row_reader_preserves_variable_counts_channels_windows_and_y_order)
{
  const auto serial = std::chrono::steady_clock::now().time_since_epoch().count();
  const char *output_root = std::getenv("BLENDER_DEEP_EXR_TEST_OUTPUT_ROOT");
  char temp_directory[FILE_MAX];
  BLI_temp_directory_path_get(temp_directory, sizeof(temp_directory));
  const std::string directory = output_root ? output_root : temp_directory;
  for (const bool half : {false, true}) {
    for (const bool alpha_only : {false, true}) {
      DeepExrWriteParams params;
      params.width = 2;
      params.height = 3;
      params.use_half_float = half;
      params.alpha_only = alpha_only;
      params.has_display_window = true;
      params.display_width = 7;
      params.display_height = 8;
      params.data_offset_x = -2;
      params.data_offset_y = -1;
      const std::string path = directory + "/deep_rows_" + std::to_string(serial) + "_" +
                               std::to_string(half) + "_" + std::to_string(alpha_only) + ".exr";
      std::vector<int> requested;
      auto reader = [&](const int y, std::vector<std::vector<DeepSample>> &row) {
        requested.push_back(y);
        row.assign(2, {});
        if (y != 1)
          row[0].push_back({float(y + 1), -.125f, .25f, .5f, float(y + 1), float(y + 1)});
        row[1].push_back({.75f, .5f, -.25f, 0, 4, 4});
        row[1].push_back({.125f, .25f, .5f, .25f, 5, 6});
        return true;
      };
      ASSERT_TRUE(IMB_exr_save_deep_rows(reader, params, path.c_str()));
      EXPECT_EQ(requested, (std::vector<int>{2, 1, 0}));
      auto input = OIIO::ImageInput::open(path);
      ASSERT_TRUE(input) << path;
      const auto &spec = input->spec();
      EXPECT_TRUE(spec.deep);
      EXPECT_EQ(spec.x, -2);
      EXPECT_EQ(spec.y, -1);
      EXPECT_EQ(spec.full_width, 7);
      EXPECT_EQ(spec.full_height, 8);
      OIIO::DeepData data;
      ASSERT_TRUE(input->read_native_deep_image(0, 0, data));
      const auto channel = [&](const char *name) {
        return int(std::find(spec.channelnames.begin(), spec.channelnames.end(), name) -
                   spec.channelnames.begin());
      };
      for (int y = 0; y < 3; ++y) {
        const int native_y = 2 - y;
        EXPECT_EQ(data.samples(y * 2), native_y == 1 ? 0 : 1);
        EXPECT_EQ(data.samples(y * 2 + 1), 2);
        if (native_y != 1) {
          EXPECT_EQ(data.deep_value(y * 2, channel("Z"), 0), native_y + 1);
          EXPECT_EQ(data.deep_value(y * 2, channel("A"), 0), .5f);
          if (!alpha_only)
            EXPECT_EQ(data.deep_value(y * 2, channel("R"), 0), native_y + 1);
        }
        EXPECT_EQ(data.deep_value(y * 2 + 1, channel("A"), 0), 0);
        EXPECT_EQ(data.deep_value(y * 2 + 1, channel("ZBack"), 1), 6);
        if (!alpha_only) {
          EXPECT_EQ(data.deep_value(y * 2 + 1, channel("R"), 0), .75f);
          EXPECT_EQ(data.deep_value(y * 2 + 1, channel("B"), 0), -.25f);
        }
      }
      EXPECT_EQ(data.channels(), alpha_only ? 3 : 6);
      EXPECT_TRUE(input->close());
    }
  }
}

TEST_F(DeepExrWriteTest, row_output_creates_missing_nested_parent_directories)
{
  const std::string path = test_output_directory() + "/core_missing_parent_" +
                           std::to_string(
                               std::chrono::steady_clock::now().time_since_epoch().count()) +
                           "/node/alpha.exr";
  DeepExrWriteParams params;
  params.width = params.height = 1;
  params.compression = R_IMF_EXR_CODEC_ZIPS;
  auto reader = [](int, std::vector<std::vector<DeepSample>> &row) {
    row.assign(1, {{.25f, .5f, .125f, .5f, 1, 2}});
    return true;
  };
  DeepExrWriteReport report;
  ASSERT_TRUE(IMB_exr_save_deep_rows(reader, params, path.c_str(), nullptr, &report))
      << report.message;
  EXPECT_TRUE(report.close_checked);
  EXPECT_EQ(report.library_remaining, 0u);
  auto input = OIIO::ImageInput::open(path);
  ASSERT_TRUE(input) << path;
  EXPECT_TRUE(input->spec().deep);
  OIIO::DeepData data;
  ASSERT_TRUE(input->read_native_deep_image(0, 0, data));
  EXPECT_EQ(data.samples(0), 1);
  EXPECT_TRUE(input->close());
}

TEST_F(DeepExrWriteTest, core_rows_match_legacy_decoded_payload_all_codecs_and_precisions)
{
  const std::string output_root = test_output_directory();
  const auto serial = std::chrono::steady_clock::now().time_since_epoch().count();
  constexpr int width = 3, height = 3;
  std::vector<std::vector<DeepSample>> original(width * height);
  for (int y : {0, 2}) {
    for (int x = 0; x < width; ++x) {
      for (int s = 0; s < x * 19; ++s) {
        original[y * width + x].push_back(
            {float(s % 7) * .01001f, -0.0f, -.01234f, .00321f, float(s), float(s) + .5f});
      }
    }
  } /* The middle row is entirely empty, including under ZIP/RLE. */
  for (int codec : {R_IMF_EXR_CODEC_NONE, R_IMF_EXR_CODEC_RLE, R_IMF_EXR_CODEC_ZIPS}) {
    for (bool half : {false, true}) {
      for (bool alpha : {false, true}) {
        DeepExrWriteParams params;
        params.width = width;
        params.height = height;
        params.compression = codec;
        params.use_half_float = half;
        params.alpha_only = alpha;
        params.has_display_window = true;
        params.display_width = 19;
        params.display_height = 23;
        params.display_offset_x = -5;
        params.display_offset_y = -7;
        params.data_offset_x = -9;
        params.data_offset_y = -11;
        const std::string base = std::string(output_root) + "/core_parity_" +
                                 std::to_string(serial) + "_" + std::to_string(codec) + "_" +
                                 std::to_string(half) + "_" + std::to_string(alpha);
        auto reader = [&](int y, std::vector<std::vector<DeepSample>> &row) {
          row.assign(original.begin() + y * width, original.begin() + (y + 1) * width);
          return true;
        };
        DeepExrWriteReport report;
        ASSERT_TRUE(
            IMB_exr_save_deep_rows(reader, params, (base + "_core.exr").c_str(), nullptr, &report))
            << report.message;
        EXPECT_EQ(report.failure, DeepExrWriteFailure::None);
        EXPECT_TRUE(report.close_checked);
        EXPECT_TRUE(report.library_private_accounted);
        EXPECT_GT(report.library_peak, 0u);
        EXPECT_EQ(report.library_remaining, 0u);
        ASSERT_TRUE(IMB_exr_save_deep(original,
                                      width,
                                      height,
                                      (base + "_legacy.exr").c_str(),
                                      codec,
                                      half,
                                      alpha,
                                      true,
                                      19,
                                      23,
                                      -5,
                                      -7,
                                      -9,
                                      -11,
                                      0.0f,
                                      0.0f));
        auto actual = OIIO::ImageInput::open(base + "_core.exr");
        auto expected = OIIO::ImageInput::open(base + "_legacy.exr");
        ASSERT_TRUE(actual);
        ASSERT_TRUE(expected);
        const auto &spec = actual->spec();
        EXPECT_TRUE(spec.deep);
        EXPECT_EQ(spec.x, -9);
        EXPECT_EQ(spec.y, -11);
        EXPECT_EQ(spec.full_x, -5);
        EXPECT_EQ(spec.full_y, -7);
        EXPECT_EQ(spec.full_width, 19);
        EXPECT_EQ(spec.full_height, 23);
        ASSERT_EQ(spec.channelnames, expected->spec().channelnames);
        ASSERT_EQ(spec.channelformats, expected->spec().channelformats);
        OIIO::DeepData a, b;
        ASSERT_TRUE(actual->read_native_deep_image(0, 0, a));
        ASSERT_TRUE(expected->read_native_deep_image(0, 0, b));
        for (int p = 0; p < width * height; ++p) {
          ASSERT_EQ(a.samples(p), b.samples(p));
          for (int s = 0; s < a.samples(p); ++s) {
            for (int c = 0; c < a.channels(); ++c) {
              EXPECT_EQ(std::bit_cast<uint32_t>(a.deep_value(p, c, s)),
                        std::bit_cast<uint32_t>(b.deep_value(p, c, s)));
            }
          }
        }
        EXPECT_FALSE(actual->seek_subimage(1, 0));
        EXPECT_TRUE(actual->close());
        EXPECT_TRUE(expected->close());
      }
    }
  }
}

TEST_F(DeepExrWriteTest, every_actual_allocation_can_be_denied_without_retained_library_bytes)
{
  const std::string root = test_output_directory();
  const auto serial = std::chrono::steady_clock::now().time_since_epoch().count();
  DeepExrWriteParams params;
  params.width = 2;
  params.height = 3;
  params.compression = R_IMF_EXR_CODEC_ZIPS;
  size_t successful_calls = 0;
  for (size_t deny_at = 0; deny_at <= successful_calls; ++deny_at) {
    size_t calls = 0, live = 0, peak = 0;
    bool denied = false;
    auto acquire = [&](size_t bytes) {
      ++calls;
      if (deny_at && calls == deny_at) {
        denied = true;
        throw std::bad_alloc();
      }
      live += bytes;
      peak = std::max(peak, live);
    };
    auto release = [&](size_t bytes) {
      EXPECT_LE(bytes, live);
      live -= bytes;
    };
    DeepExrMemoryCallbacks callbacks{acquire, release};
    auto reader = [](int y, std::vector<std::vector<DeepSample>> &row) {
      row.assign(2, std::vector<DeepSample>(y ? 129 : 1, {.13f, .27f, -.3f, .1f, 1, 2}));
      return true;
    };
    DeepExrWriteReport report;
    const std::string path = std::string(root) + "/core_deny_" + std::to_string(serial) + "_" +
                             std::to_string(deny_at) + ".exr";
    const bool result = IMB_exr_save_deep_rows(reader, params, path.c_str(), &callbacks, &report);
    if (!deny_at) {
      ASSERT_TRUE(result) << report.message;
      successful_calls = calls;
      EXPECT_GT(peak, 668295u); /* Real installed compressor, not guessed reserve. */
      EXPECT_GT(report.library_peak, 668295u);
    }
    else {
      EXPECT_TRUE(denied) << deny_at;
      EXPECT_FALSE(result) << deny_at;
      EXPECT_EQ(report.failure, DeepExrWriteFailure::Memory) << deny_at << report.message;
    }
    EXPECT_EQ(live, 0u) << deny_at;
    EXPECT_EQ(report.library_remaining, 0u) << deny_at;
    EXPECT_TRUE(report.close_checked) << deny_at;
  }
  std::printf("DEEP_CORE_ALLOCATION_DENIALS output_root=%s allocations=%zu\n",
              root.c_str(),
              successful_calls);
}

TEST_F(DeepExrWriteTest, rejects_invalid_window_and_codec_before_any_io_or_row_callback)
{
  class Stream : public DeepExrCoreStream {
   public:
    int writes = 0, closes = 0;
    bool write(uint64_t, const void *, size_t) override
    {
      ++writes;
      return true;
    }
    bool finish_checked() override
    {
      ++closes;
      return true;
    }
  };
  for (int invalid = 0; invalid < 5; ++invalid) {
    DeepExrWriteParams params;
    params.width = 2;
    params.height = 1;
    if (invalid == 0)
      params.width = 0;
    if (invalid == 1)
      params.compression = R_IMF_EXR_CODEC_ZIP;
    if (invalid >= 2) {
      params.has_display_window = true;
      params.display_width = params.display_height = 2;
      if (invalid == 2)
        params.data_offset_x = INT_MAX;
      if (invalid == 3)
        params.display_offset_y = INT_MAX;
      if (invalid == 4)
        params.display_height = 0;
    }
    int calls = 0;
    auto reader = [&](int, std::vector<std::vector<DeepSample>> &) {
      ++calls;
      return true;
    };
    Stream stream;
    DeepExrWriteReport report;
    EXPECT_FALSE(imb_exr_save_deep_rows_core(
        reader, params, "invalid-private-stream", nullptr, &report, &stream));
    EXPECT_EQ(report.failure, DeepExrWriteFailure::InvalidInput);
    EXPECT_FALSE(report.close_checked);
    EXPECT_EQ(calls, 0);
    EXPECT_EQ(stream.writes, 0);
    EXPECT_EQ(stream.closes, 0); /* Stream ownership was never entered. */
  }
}

TEST_F(DeepExrWriteTest, nested_row_writers_restore_allocator_owner_and_release_both_quotas)
{
  const auto serial = std::chrono::steady_clock::now().time_since_epoch().count();
  const std::string base = test_output_directory() + "/core_nested_" + std::to_string(serial);
  size_t outer_live = 0, inner_live = 0;
  auto acquire_outer = [&](size_t n) { outer_live += n; };
  auto release_outer = [&](size_t n) {
    EXPECT_LE(n, outer_live);
    outer_live -= n;
  };
  auto acquire_inner = [&](size_t n) { inner_live += n; };
  auto release_inner = [&](size_t n) {
    EXPECT_LE(n, inner_live);
    inner_live -= n;
  };
  DeepExrMemoryCallbacks outer{acquire_outer, release_outer}, inner{acquire_inner, release_inner};
  DeepExrWriteParams params;
  params.width = params.height = 1;
  params.compression = R_IMF_EXR_CODEC_ZIPS;
  auto inner_reader = [](int, std::vector<std::vector<DeepSample>> &row) {
    row.assign(1, {{.1f, .2f, .3f, .5f, -1, 2}});
    return true;
  };
  auto outer_reader = [&](int y, std::vector<std::vector<DeepSample>> &row) {
    DeepExrWriteReport report;
    const size_t before = outer_live;
    EXPECT_GT(before, 0u);
    const bool written = IMB_exr_save_deep_rows(
        inner_reader, params, (base + "_inner.exr").c_str(), &inner, &report);
    EXPECT_EQ(inner_live, 0u);
    EXPECT_EQ(outer_live, before);
    EXPECT_TRUE(written) << report.message;
    return written && inner_reader(y, row);
  };
  DeepExrWriteReport report;
  EXPECT_TRUE(
      IMB_exr_save_deep_rows(outer_reader, params, (base + "_outer.exr").c_str(), &outer, &report))
      << report.message;
  EXPECT_EQ(outer_live, 0u);
  EXPECT_EQ(inner_live, 0u);
  EXPECT_EQ(report.library_remaining, 0u);
  EXPECT_TRUE(report.close_checked);
}

TEST_F(DeepExrWriteTest, checked_stream_failure_preserves_first_reader_or_io_cause)
{
  class Stream : public DeepExrCoreStream {
   public:
    bool fail_write = false, closed = false;
    bool write(uint64_t, const void *, size_t) override
    {
      return !fail_write;
    }
    bool finish_checked() override
    {
      closed = true;
      return false;
    }
  };
  DeepExrWriteParams params;
  params.width = params.height = 1;
  for (int failure = 0; failure < 3; ++failure) {
    Stream stream;
    stream.fail_write = failure == 2;
    auto reader = [&](int, std::vector<std::vector<DeepSample>> &row) {
      row.assign(1, {{.1f, .2f, .3f, .5f, 1, 2}});
      return failure != 1;
    };
    DeepExrWriteReport report;
    EXPECT_FALSE(imb_exr_save_deep_rows_core(
        reader, params, "private-fault-stream", nullptr, &report, &stream));
    EXPECT_TRUE(stream.closed);
    EXPECT_FALSE(report.close_checked);
    EXPECT_EQ(report.library_remaining, 0u);
    EXPECT_EQ(report.failure,
              failure == 1 ? DeepExrWriteFailure::Reader : DeepExrWriteFailure::Io);
    if (failure == 0)
      EXPECT_STREQ(report.message, "Deep output flush/close failed");
    if (failure == 2)
      EXPECT_STREQ(report.message, "Deep output stream write failed");
  }
}

TEST_F(DeepExrWriteTest, each_header_chunk_and_footer_write_failure_prevents_success)
{
  class Stream : public DeepExrCoreStream {
   public:
    size_t calls = 0, deny_at = 0;
    bool denied = false, closed = false;
    bool write(uint64_t, const void *, size_t) override
    {
      if (++calls == deny_at) {
        denied = true;
        return false;
      }
      return true;
    }
    bool finish_checked() override
    {
      closed = true;
      return true;
    }
  };
  DeepExrWriteParams params;
  params.width = params.height = 2;
  params.compression = R_IMF_EXR_CODEC_ZIPS;
  auto reader = [](int, std::vector<std::vector<DeepSample>> &row) {
    row.assign(2, {{.25f, .5f, .125f, .5f, 1, 2}});
    return true;
  };
  size_t actual_writes = 0;
  for (size_t deny = 0; deny <= actual_writes; ++deny) {
    Stream stream;
    stream.deny_at = deny;
    DeepExrWriteReport report;
    const bool result = imb_exr_save_deep_rows_core(
        reader, params, "private-write-denial", nullptr, &report, &stream);
    if (!deny) {
      ASSERT_TRUE(result) << report.message;
      actual_writes = stream.calls;
      EXPECT_GT(actual_writes, 3u);
    }
    else {
      EXPECT_TRUE(stream.denied) << deny;
      EXPECT_FALSE(result) << deny;
      EXPECT_EQ(report.failure, DeepExrWriteFailure::Io) << deny;
      EXPECT_STREQ(report.message, "Deep output stream write failed");
    }
    EXPECT_TRUE(stream.closed);
    EXPECT_TRUE(report.close_checked);
    EXPECT_EQ(report.library_remaining, 0u);
  }
  std::printf("DEEP_CORE_CHECKED_WRITE_DENIALS actual_writes=%zu\n", actual_writes);
}

TEST_F(DeepExrWriteTest, late_row_reader_shape_and_payload_failure_cannot_complete_file)
{
  const auto serial = std::chrono::steady_clock::now().time_since_epoch().count();
  const char *output_root = std::getenv("BLENDER_DEEP_EXR_TEST_OUTPUT_ROOT");
  char temp_directory[FILE_MAX];
  BLI_temp_directory_path_get(temp_directory, sizeof(temp_directory));
  const std::string directory = output_root ? output_root : temp_directory;
  for (int failure = 0; failure < 4; ++failure) {
    DeepExrWriteParams params;
    params.width = 1;
    params.height = 3;
    params.use_half_float = true;
    int calls = 0;
    auto reader = [&](const int y, std::vector<std::vector<DeepSample>> &row) {
      ++calls;
      row.assign(1, {{.25f, .5f, .125f, .5f, 1, 2}});
      if (y == 1) {
        if (failure == 0)
          return false;
        if (failure == 1)
          row.clear();
        if (failure == 2)
          row[0][0].r = std::numeric_limits<float>::quiet_NaN();
        if (failure == 3)
          row[0][0].r = 65536;
      }
      return true;
    };
    const std::string path = directory + "/deep_rows_failure_" + std::to_string(serial) + "_" +
                             std::to_string(failure) + ".exr";
    EXPECT_FALSE(IMB_exr_save_deep_rows(reader, params, path.c_str()));
    EXPECT_EQ(calls, 2); /* Failure after one real row; no remaining callbacks. */
  }
}

TEST_F(DeepExrWriteTest, row_allocation_callbacks_include_nested_channels_and_release_on_denial)
{
  const auto serial = std::chrono::steady_clock::now().time_since_epoch().count();
  char directory[FILE_MAX];
  BLI_temp_directory_path_get(directory, sizeof(directory));
  for (const size_t quota : {size_t(4096), size_t(1 << 20)}) {
    size_t used = 0, peak = 0;
    bool denied = false;
    auto acquire = [&](const size_t bytes) {
      if (bytes > quota - used) {
        denied = true;
        throw std::bad_alloc();
      }
      used += bytes;
      peak = std::max(peak, used);
    };
    auto release = [&](const size_t bytes) {
      EXPECT_LE(bytes, used);
      used -= bytes;
    };
    const DeepExrMemoryCallbacks memory{acquire, release};
    DeepExrWriteParams params;
    params.width = 3;
    params.height = 4;
    auto reader = [&](int, std::vector<std::vector<DeepSample>> &row) {
      row.assign(3, std::vector<DeepSample>(128, {.125f, .25f, .5f, .01f, 1, 1}));
      return true;
    };
    const std::string path = std::string(directory) + "/deep_row_quota_" + std::to_string(serial) +
                             "_" + std::to_string(quota) + ".exr";
    EXPECT_EQ(IMB_exr_save_deep_rows(reader, params, path.c_str(), &memory), quota > 4096);
    EXPECT_EQ(denied, quota == 4096);
    EXPECT_EQ(used, 0u);
    EXPECT_LE(peak, quota);
    if (quota > 4096)
      EXPECT_GT(peak, 4096u); /* Includes actual nested per-pixel channel allocations. */
  }
}

TEST_F(DeepExrWriteTest, rejects_pixel_vector_size_mismatch_before_writing)
{
  std::vector<std::vector<DeepSample>> deep_data(2);

  EXPECT_FALSE(IMB_exr_save_deep(deep_data, 1, 1, "deep_invalid_pixels.exr", 0, false, false));
}

TEST_F(DeepExrWriteTest, rejects_invalid_and_half_overflow_samples_before_writing)
{
  std::vector<std::vector<DeepSample>> pixels(1);
  pixels[0] = {{1.0f, 0.0f, 0.0f, 0.5f, 2.0f, 1.0f}};
  EXPECT_FALSE(IMB_exr_save_deep(pixels, 1, 1, "deep_invalid_sample.exr", 0, false, false));
  pixels[0][0].z_back = 2.0f;
  pixels[0][0].r = 70000.0f;
  EXPECT_FALSE(IMB_exr_save_deep(pixels, 1, 1, "deep_half_overflow.exr", 0, true, false));
  pixels[0][0].r = std::numeric_limits<float>::quiet_NaN();
  EXPECT_FALSE(IMB_exr_save_deep(pixels, 1, 1, "deep_nonfinite_sample.exr", 0, false, false));
}

TEST_F(DeepExrWriteTest, rejects_half_overflow_created_by_interval_merge)
{
  char directory[FILE_MAX], filepath[FILE_MAX];
  BLI_temp_directory_path_get(directory, sizeof(directory));
  const std::string name = "deep_merged_half_overflow_" +
                           std::to_string(
                               std::chrono::steady_clock::now().time_since_epoch().count()) +
                           ".exr";
  BLI_path_join(filepath, sizeof(filepath), directory, name.c_str());
  std::vector<std::vector<DeepSample>> pixels(1);
  pixels[0] = {{40000.0f, 0.0f, 0.0f, 0.0f, 1.0f, 2.0f}, {40000.0f, 0.0f, 0.0f, 0.0f, 2.0f, 3.0f}};
  std::printf("DEEP_WRITER_NEGATIVE_OUTPUT path=%s\n", filepath);
  EXPECT_FALSE(IMB_exr_save_deep(
      pixels, 1, 1, filepath, 0, true, false, false, 0, 0, 0, 0, 0, 0, 0.01f, 0.01f));
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

  ASSERT_TRUE(IMB_exr_save_deep(
      deep_data, 1, 1, filepath, 0, false, false, false, 0, 0, 0, 0, 0, 0, 0.01f, 0.01f));

  std::unique_ptr<OIIO::ImageInput> image_input = OIIO::ImageInput::open(filepath);
  ASSERT_NE(image_input, nullptr);
  OIIO::DeepData written_data;
  ASSERT_TRUE(image_input->read_native_deep_image(0, 0, written_data));
  EXPECT_EQ(written_data.samples(0), 1);
  image_input->close();
  EXPECT_EQ(deep_data[0].size(), 2u);
}

TEST_F(DeepExrWriteTest, scanline_blocks_preserve_native_samples_and_windows)
{
  /* Multiple complete blocks, a partial final block, empty pixels, skewed sample counts,
   * equal-depth ties, negative data offsets and all channel/merge configurations. */
  constexpr int width = 9, height = 35;
  std::vector<std::vector<DeepSample>> deep_data(width * height);
  for (int pixel = 0; pixel < width * height; pixel++) {
    const int count = pixel % 7 == 0 ? 0 : (pixel % 17 == 0 ? 2049 : 1 + pixel % 31);
    for (int s = 0; s < count; s++) {
      const float z = float((s * 13 + pixel) % 127) * 0.001f + 1.0f;
      deep_data[pixel].push_back({0.0001f * float(s % 19),
                                  -0.0f,
                                  0.0002f * float(pixel % 23),
                                  0.005f + 0.0001f * float(s % 11),
                                  z,
                                  s % 3 == 0 ? z : z + 0.001f});
    }
  }
  const auto original = deep_data;
  const char *old_option = BLI_getenv("BLENDER_EXPERIMENTAL_DEEP_EXR_SCANLINES");
  const bool had_option = old_option != nullptr;
  const std::string old_value = old_option ? old_option : "";
  struct RestoreOption {
    bool present;
    const std::string &value;
    ~RestoreOption()
    {
      BLI_setenv("BLENDER_EXPERIMENTAL_DEEP_EXR_SCANLINES", present ? value.c_str() : nullptr);
    }
  } restore{had_option, old_value};
  char temp_directory[FILE_MAX];
  BLI_temp_directory_path_get(temp_directory, sizeof(temp_directory));
  for (const int compression : {R_IMF_EXR_CODEC_NONE, R_IMF_EXR_CODEC_RLE, R_IMF_EXR_CODEC_ZIPS}) {
    for (const bool half : {false, true}) {
      for (const bool alpha_only : {false, true}) {
        for (const bool merge : {false, true}) {
          std::vector<OIIO::DeepData> written(2);
          std::vector<OIIO::ImageSpec> specs;
          for (int variant = 0; variant < 2; variant++) {
            const std::string name = "deep_blocks_" + std::to_string(compression) + "_" +
                                     std::to_string(half) + "_" + std::to_string(alpha_only) +
                                     "_" + std::to_string(merge) + "_" + std::to_string(variant) +
                                     ".exr";
            char filepath[FILE_MAX];
            BLI_path_join(filepath, sizeof(filepath), temp_directory, name.c_str());
            BLI_setenv("BLENDER_EXPERIMENTAL_DEEP_EXR_SCANLINES", variant ? "16" : "1");
            ASSERT_TRUE(IMB_exr_save_deep(deep_data,
                                          width,
                                          height,
                                          filepath,
                                          compression,
                                          half,
                                          alpha_only,
                                          true,
                                          480,
                                          420,
                                          0,
                                          0,
                                          -7,
                                          -11,
                                          merge ? 0.01f : 0.0f,
                                          0.01f));
            auto input = OIIO::ImageInput::open(filepath);
            ASSERT_NE(input, nullptr);
            specs.push_back(input->spec());
            ASSERT_TRUE(input->read_native_deep_image(0, 0, written[variant]));
            input->close();
          }
          EXPECT_EQ(specs[0].x, -7);
          EXPECT_EQ(specs[0].y, -11);
          EXPECT_EQ(specs[1].x, specs[0].x);
          EXPECT_EQ(specs[1].y, specs[0].y);
          EXPECT_EQ(specs[1].width, width);
          EXPECT_EQ(specs[1].height, height);
          EXPECT_EQ(specs[1].full_width, 480);
          EXPECT_EQ(specs[1].full_height, 420);
          ASSERT_EQ(specs[1].channelnames, specs[0].channelnames);
          for (int pixel = 0; pixel < width * height; pixel++) {
            ASSERT_EQ(written[0].samples(pixel), written[1].samples(pixel));
            for (int sample = 0; sample < written[0].samples(pixel); sample++) {
              for (int channel = 0; channel < specs[0].nchannels; channel++) {
                EXPECT_EQ(std::bit_cast<uint32_t>(written[0].deep_value(pixel, channel, sample)),
                          std::bit_cast<uint32_t>(written[1].deep_value(pixel, channel, sample)));
              }
            }
            ASSERT_EQ(deep_data[pixel].size(), original[pixel].size());
            for (size_t sample = 0; sample < deep_data[pixel].size(); sample++) {
              EXPECT_EQ(std::memcmp(&deep_data[pixel][sample],
                                    &original[pixel][sample],
                                    sizeof(DeepSample)),
                        0);
            }
          }
        }
      }
    }
  }
}

}  // namespace blender::imbuf::tests
