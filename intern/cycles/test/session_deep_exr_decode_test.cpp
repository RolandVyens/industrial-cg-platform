/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include <OpenImageIO/deepdata.h>
#include <OpenImageIO/imagebuf.h>
#include <OpenImageIO/imagebufalgo.h>
#include <json.hpp>

#include "testing/testing.h"
#include "util/defines.h"

CCL_NAMESPACE_BEGIN
namespace {
/* Independent, test-only standard-Beer decoder. No renderer fit, capture or
 * representation helper is imported. Each real stored sample is read once;
 * sorted cuts then merge with the sample sequence rather than rescanning it. */
struct DecodeValue {
  double rgb[3] = {};
  double log_t = 0;
};
struct DecodeSample {
  DecodeValue value;
  double alpha, front, back;
};
struct DecodeWitness {
  int x, y, width, height, fields;
  std::vector<double> values;
};
DecodeValue decode_over(const DecodeValue &front, const DecodeValue &back)
{
  DecodeValue result;
  const double t = std::exp(front.log_t);
  for (int c = 0; c < 3; c++) result.rgb[c] = front.rgb[c] + t * back.rgb[c];
  result.log_t = front.log_t + back.log_t;
  return result;
}
DecodeValue decode_fraction(const DecodeSample &sample, const double fraction)
{
  if (fraction == 1) return sample.value;
  if (fraction == 0) return {};
  DecodeValue result;
  result.log_t = sample.value.log_t * fraction;
  const double weight = sample.alpha > 0 ? -std::expm1(result.log_t) / sample.alpha : fraction;
  for (int c = 0; c < 3; c++) result.rgb[c] = sample.value.rgb[c] * weight;
  return result;
}
int decode_channel(const OIIO::ImageSpec &spec, const std::string &name)
{
  for (int c = 0; c < spec.nchannels; c++) {
    const auto &full = spec.channelnames[c];
    const size_t dot = full.rfind('.');
    const std::string leaf = dot == std::string::npos ? full : full.substr(dot + 1);
    if (leaf == name || (name == "R" && leaf == "red") ||
        (name == "G" && leaf == "green") || (name == "B" && leaf == "blue") ||
        (name == "A" && leaf == "alpha")) return c;
  }
  return -1;
}
}  // namespace

TEST(DeepEXRDecode, opaque_volume_standard_split)
{
  DecodeSample sample{};
  sample.front = 10;
  sample.back = 12;
  sample.alpha = 1;
  sample.value.log_t = -std::numeric_limits<double>::infinity();
  sample.value.rgb[0] = .25;
  sample.value.rgb[1] = .5;
  sample.value.rgb[2] = .75;
  /* OpenEXR's opaque-volume limit is opaque at every positive fraction,
   * not a point at ZBack. Both split parts are opaque and over reconstructs
   * the original sample. The exact front remains an empty prefix. */
  const auto empty = decode_fraction(sample, 0);
  EXPECT_EQ(empty.log_t, 0);
  for (double c : empty.rgb) EXPECT_EQ(c, 0);
  for (const double f : {.01, .25, .5, .75, .99}) {
    const auto front = decode_fraction(sample, f);
    const auto back = decode_fraction(sample, 1 - f);
    EXPECT_EQ(std::exp(front.log_t), 0);
    EXPECT_EQ(std::exp(back.log_t), 0);
    const auto rebuilt = decode_over(front, back);
    EXPECT_EQ(std::exp(rebuilt.log_t), 0);
    for (int c = 0; c < 3; ++c) EXPECT_EQ(rebuilt.rgb[c], sample.value.rgb[c]);
  }
}

TEST(DeepEXRDecode, actual_output)
{
  const char *input = std::getenv("CYCLES_TWO_MODE_DECODE_FILE");
  const char *destination = std::getenv("CYCLES_TWO_MODE_DECODE_ROOT");
  const char *cut_text = std::getenv("CYCLES_TWO_MODE_DECODE_CUTS");
  if (!input || !destination || !cut_text)
    GTEST_SKIP() << "Actual rendered EXR, exclusive output root and frozen cuts are required";
  const auto started = std::chrono::steady_clock::now();
  const std::filesystem::path root(destination);
  const auto stats_path = root / "native-report.json";
  const auto cut_path = root / "cut.exr", flat_path = root / "flatten.exr";
  ASSERT_TRUE(std::filesystem::is_directory(root));
  ASSERT_FALSE(std::filesystem::exists(stats_path));
  ASSERT_FALSE(std::filesystem::exists(cut_path));
  ASSERT_FALSE(std::filesystem::exists(flat_path));
  std::vector<double> cuts;
  std::istringstream stream(cut_text);
  std::string token;
  while (std::getline(stream, token, ',')) {
    const double cut = std::stod(token);
    ASSERT_TRUE(std::isfinite(cut));
    cuts.push_back(cut);
  }
  ASSERT_TRUE(std::is_sorted(cuts.begin(), cuts.end()));
  const char *part = std::getenv("CYCLES_TWO_MODE_DECODE_PART");
  const int subimage = part ? std::stoi(part) : 0;
  OIIO::ImageBuf image(input, subimage, 0);
  ASSERT_TRUE(image.read(subimage, 0, true)) << image.geterror();
  const auto &spec = image.spec();
  ASSERT_TRUE(spec.deep);
  ASSERT_EQ(spec.depth, 1);
  const auto *deep = image.deepdata();
  ASSERT_NE(deep, nullptr);
  const std::array<int, 6> channels = {decode_channel(spec, "R"), decode_channel(spec, "G"),
      decode_channel(spec, "B"), decode_channel(spec, "A"), decode_channel(spec, "Z"),
      decode_channel(spec, "ZBack")};
  for (int c = 3; c < 6; c++) ASSERT_GE(channels[c], 0);
  ASSERT_EQ(deep->channeltype(channels[4]), OIIO::TypeDesc::FLOAT);
  ASSERT_EQ(deep->channeltype(channels[5]), OIIO::TypeDesc::FLOAT);
  ASSERT_EQ(spec.get_string_attribute("compression"), "zips");
  const uint64_t pixels = uint64_t(spec.width) * spec.height;
  ASSERT_EQ(uint64_t(deep->pixels()), pixels);
  std::vector<DecodeWitness> witnesses;
  std::vector<int> owners;
  int min_x = 0, max_y = 0;
  const char *witness_path = std::getenv("CYCLES_TWO_MODE_DECODE_WITNESS");
  if (witness_path) {
    std::ifstream input_metadata(witness_path);
    ASSERT_TRUE(input_metadata.is_open());
    nlohmann::json metadata;
    input_metadata >> metadata;
    ASSERT_FALSE(metadata.empty());
    int min_y = INT_MAX, max_x = INT_MIN;
    min_x = INT_MAX; max_y = INT_MIN;
    for (const auto &entry : metadata) {
      DecodeWitness witness;
      witness.x = entry.at("x"); witness.y = entry.at("y");
      witness.width = entry.at("width"); witness.height = entry.at("height");
      witness.fields = int(entry.at("fields").size());
      ASSERT_TRUE(witness.fields == 1 || witness.fields == 7);
      ASSERT_GT(witness.width, 0); ASSERT_GT(witness.height, 0);
      ASSERT_LE(witness.width, spec.width); ASSERT_LE(witness.height, spec.height);
      ASSERT_EQ(entry.at("cuts").get<std::vector<double>>(), cuts);
      const size_t elements = size_t(witness.width) * witness.height * cuts.size() * witness.fields;
      const auto binary = entry.at("binary").get<std::string>();
      ASSERT_GE(std::filesystem::file_size(binary), elements * sizeof(double));
      witness.values.resize(elements);
      std::ifstream data(binary, std::ios::binary);
      ASSERT_TRUE(data.is_open());
      data.read(reinterpret_cast<char *>(witness.values.data()), std::streamsize(elements * sizeof(double)));
      ASSERT_EQ(size_t(data.gcount()), elements * sizeof(double));
      min_x = std::min(min_x, witness.x); min_y = std::min(min_y, witness.y);
      max_x = std::max(max_x, witness.x + witness.width);
      max_y = std::max(max_y, witness.y + witness.height);
      witnesses.push_back(std::move(witness));
    }
    ASSERT_EQ(max_x - min_x, spec.width); ASSERT_EQ(max_y - min_y, spec.height);
    owners.assign(size_t(pixels), -1);
    for (size_t i = 0; i < witnesses.size(); i++) {
      const auto &witness = witnesses[i];
      for (int y = 0; y < witness.height; y++) {
        const int row = max_y - 1 - witness.y - y;
        for (int x = 0; x < witness.width; x++) {
          const size_t pixel = size_t(row) * spec.width + witness.x - min_x + x;
          ASSERT_LT(pixel, owners.size()); ASSERT_EQ(owners[pixel], -1);
          owners[pixel] = int(i);
        }
      }
    }
    ASSERT_TRUE(std::all_of(owners.begin(), owners.end(), [](int owner) { return owner >= 0; }));
  }
  std::vector<double> alpha_source_error(cuts.size(), 0), rgb_source_error(cuts.size(), 0);
  std::vector<uint64_t> source_pixels(cuts.size(), 0);
  uint64_t total = 0;
  uint64_t opaque_volume_samples = 0, opaque_volume_pixels = 0;
  double max_opaque_volume_extent = 0;
  std::vector<uint64_t> opaque_volume_intersections(cuts.size(), 0);
  double residual = 0, full_max = 0;
  size_t selected = 0;
  for (size_t c = 1; c < cuts.size(); c++)
    if (std::abs(cuts[c] - 12.38) < std::abs(cuts[selected] - 12.38)) selected = c;
  std::vector<float> slice(cuts.empty() ? 0 : size_t(pixels) * 4, 0);
  std::vector<DecodeSample> samples;
  std::vector<DecodeValue> suffix;
  for (uint64_t pixel = 0; pixel < pixels; pixel++) {
    bool pixel_has_opaque_volume = false;
    const uint32_t count = deep->samples(pixel);
    total += count;
    samples.resize(count); suffix.resize(size_t(count) + 1);
    double previous = -std::numeric_limits<double>::infinity();
    for (uint32_t s = 0; s < count; s++) {
      auto &sample = samples[s];
      double values[6];
      for (int c = 0; c < 6; c++) {
        values[c] = channels[c] < 0 ? 0 : deep->deep_value(pixel, channels[c], s);
        ASSERT_TRUE(std::isfinite(values[c])) << "pixel=" << pixel << " sample=" << s;
      }
      sample.alpha = values[3]; sample.front = values[4]; sample.back = values[5];
      ASSERT_TRUE(sample.alpha >= 0 && sample.alpha <= 1 && sample.front >= previous &&
                  sample.front >= 0 && sample.back >= sample.front)
          << "pixel=" << pixel << " sample=" << s;
      /* Legal OpenEXR opaque volume. Density detail is lost, so report it for
       * quality analysis instead of rejecting a format-valid sample. */
      if (sample.back > sample.front && sample.alpha == 1) {
        ++opaque_volume_samples;
        pixel_has_opaque_volume = true;
        max_opaque_volume_extent = std::max(max_opaque_volume_extent, sample.back - sample.front);
      }
      previous = sample.back;
      for (int c = 0; c < 3; c++) sample.value.rgb[c] = values[c];
      sample.value.log_t = sample.alpha == 1 ? -std::numeric_limits<double>::infinity() :
                                               std::log1p(-sample.alpha);
    }
    opaque_volume_pixels += pixel_has_opaque_volume;
    DecodeValue full;
    for (const auto &sample : samples) full = decode_over(full, sample.value);
    suffix[count] = {};
    for (uint32_t s = count; s-- > 0;) suffix[s] = decode_over(samples[s].value, suffix[s + 1]);
    for (int c = 0; c < 3; c++) {
      ASSERT_TRUE(std::isfinite(full.rgb[c]));
      full_max = std::max(full_max, std::abs(full.rgb[c]));
    }
    full_max = std::max(full_max, -std::expm1(full.log_t));
    DecodeValue prefix;
    uint32_t at = 0;
    for (size_t c = 0; c < cuts.size(); c++) {
      while (at < count && samples[at].back <= cuts[c]) {
        prefix = decode_over(prefix, samples[at].value); at++;
      }
      DecodeValue before = prefix, after = suffix[at];
      if (at < count && samples[at].front < cuts[c] &&
          samples[at].back > samples[at].front) {
        if (samples[at].alpha == 1) ++opaque_volume_intersections[c];
        const double f = (cuts[c] - samples[at].front) / (samples[at].back - samples[at].front);
        before = decode_over(prefix, decode_fraction(samples[at], f));
        after = decode_over(decode_fraction(samples[at], 1 - f), suffix[at + 1]);
      }
      const auto rebuilt = decode_over(before, after);
      for (int channel = 0; channel < 3; channel++)
        residual = std::max(residual, std::abs(rebuilt.rgb[channel] - full.rgb[channel]));
      residual = std::max(residual, std::abs(std::expm1(rebuilt.log_t) - std::expm1(full.log_t)));
      if (!witnesses.empty()) {
        const auto &witness = witnesses[owners[size_t(pixel)]];
        const size_t wx = size_t(int(pixel % spec.width) + min_x - witness.x);
        const size_t wy = size_t(max_y - 1 - int(pixel / spec.width) - witness.y);
        const auto *reference = witness.values.data() +
            ((wy * witness.width + wx) * cuts.size() + c) * witness.fields;
        ASSERT_FALSE(std::isnan(reference[0]));
        alpha_source_error[c] = std::max(alpha_source_error[c],
            std::abs(std::expm1(before.log_t) - std::expm1(reference[0])));
        if (witness.fields == 7 && channels[0] >= 0) {
          const int offset = deep->channeltype(channels[3]) == OIIO::TypeDesc::HALF ? 1 : 4;
          for (int rgb = 0; rgb < 3; rgb++) {
            ASSERT_TRUE(std::isfinite(reference[offset + rgb]));
            rgb_source_error[c] = std::max(rgb_source_error[c],
                std::abs(before.rgb[rgb] - reference[offset + rgb]));
          }
        }
        source_pixels[c]++;
      }
      if (c == selected) {
        for (int channel = 0; channel < 3; channel++) slice[4 * pixel + channel] = float(before.rgb[channel]);
        slice[4 * pixel + 3] = float(-std::expm1(before.log_t));
      }
    }
  }
  ASSERT_LE(residual, 1e-8 * (1 + full_max));
  if (!cuts.empty()) {
    OIIO::ImageSpec output(spec.width, spec.height, 4, OIIO::TypeDesc::FLOAT);
    output.x = spec.x; output.y = spec.y;
    output.full_x = spec.full_x; output.full_y = spec.full_y;
    output.full_width = spec.full_width; output.full_height = spec.full_height;
    OIIO::ImageBuf cut_image(output, OIIO::span<float>(slice));
    ASSERT_TRUE(cut_image.write(cut_path.string())) << cut_image.geterror();
  }
  OIIO::ImageBuf flattened;
  ASSERT_TRUE(OIIO::ImageBufAlgo::flatten(flattened, image)) << flattened.geterror();
  ASSERT_TRUE(flattened.write(flat_path.string())) << flattened.geterror();
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  std::ofstream report(stats_path);
  ASSERT_TRUE(report.is_open());
  report << std::setprecision(17) << "{\"pixels_checked\":" << pixels
         << ",\"samples_checked\":" << total << ",\"max_recomposition_residual\":" << residual
         << ",\"opaque_volume_samples\":" << opaque_volume_samples
         << ",\"opaque_volume_pixels\":" << opaque_volume_pixels
         << ",\"max_opaque_volume_extent\":" << max_opaque_volume_extent
         << ",\"elapsed_seconds\":" << seconds << ",\"cut_diagnostics\":[";
  for (size_t c = 0; c < cuts.size(); c++) {
    if (c) report << ',';
    report << "{\"cut\":" << cuts[c] << ",\"max_alpha_source_difference\":" << alpha_source_error[c]
           << ",\"max_rgb_source_difference\":" << rgb_source_error[c]
           << ",\"opaque_volume_intersections\":" << opaque_volume_intersections[c]
           << ",\"pixels\":" << source_pixels[c] << '}';
  }
  report << "]}\n";
  report.close();
  ASSERT_FALSE(report.fail());
}
CCL_NAMESPACE_END
