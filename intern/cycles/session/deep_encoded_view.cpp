/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */
#include "session/deep_frame_production_internal.h"
#include "session/deep_bounded_format.h"
#include "integrator/deep_gpu_frame_work.h"
#include "session/deep_emission_expression.h"
#include "session/buffers.h"
#include "util/half.h"
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <stdexcept>

CCL_NAMESPACE_BEGIN
using namespace deep_production_detail;

std::span<const DeepEncodedPayload> DeepEncodedPixelView::samples() const
{
  return tile_ && !bounded_ ? std::span<const DeepEncodedPayload>(tile_->samples).subspan(offset_, count_) :
                 std::span<const DeepEncodedPayload>{};
}

DeepEncodedPayload DeepEncodedPixelView::sample(const size_t index) const
{
  if (!bounded_) {
    return tile_->samples[offset_ + index];
  }
  const auto &p = *bounded_;
  const size_t record = offset_ + index;
  const auto load_float = [&](const size_t at) {
    float value;
    std::memcpy(&value, p.planes.data() + at, sizeof(value));
    return value;
  };
  const size_t components = p.physical_rgb ? 4 : 1;
  const auto channel = [&](const size_t c) {
    const size_t n = record * components + c;
    if (!p.half) {
      return load_float(8 * p.count + 4 * n);
    }
    uint16_t bits;
    std::memcpy(&bits, p.planes.data() + 8 * p.count + 2 * n, sizeof(bits));
    return half_to_float(half(bits));
  };
  DeepEncodedPayload value{};
  value.z = load_float(4 * record);
  value.z_back = load_float(4 * (p.count + record));
  value.a = channel(components - 1);
  if (p.physical_rgb) {
    value.r = channel(0);
    value.g = channel(1);
    value.b = channel(2);
  }
  else if (p.simple_rgb) {
    const size_t summary = p.dense_summary ? pixel_ : offset_;
    const float d = load_float(deep_bounded_summary_offset(p.count, p.half) + summary * 4);
    const float factor = d > 0.0f ? value.a / d : float(d == -float(index + 1));
    const float *beauty = beauty_;
    value.r = beauty[0] * factor;
    value.g = beauty[1] * factor;
    value.b = beauty[2] * factor;
  }
  return value;
}

DeepStatus DeepEncodedPixelView::prepare_packed_color()
{
  if (!bounded_ || !bounded_->simple_rgb) {
    return {};
  }
  const auto &p = *bounded_;
  float d = 0;
  if (p.dense_summary || count_) {
    const size_t summary = p.dense_summary ? pixel_ : offset_;
    std::memcpy(&d, p.planes.data() + deep_bounded_summary_offset(p.count, p.half) + summary * 4, 4);
  }
  const float *rgb = beauty_;
  if (!rgb) {
    return DeepStatus::fail(DeepErrorCode::Incomplete, "Final Deep Simple Beauty is pending");
  }
  if (!std::isfinite(d) || !std::isfinite(rgb[0]) || !std::isfinite(rgb[1]) ||
      !std::isfinite(rgb[2]) || d > 1.0f)
  {
    return DeepStatus::fail(DeepErrorCode::InvalidPayload, "Invalid bounded Deep Simple colour");
  }
  if (d > 0) {
    inverse_alpha_ = 1.0f / d;
  }
  else if (d < 0) {
    if (d != std::trunc(d) || -d > float(count_)) {
      return DeepStatus::fail(DeepErrorCode::InvalidPayload, "Invalid bounded Deep colour index");
    }
    color_index_ = int(-d) - 1;
  }
  else if (rgb[0] != 0 || rgb[1] != 0 || rgb[2] != 0) {
    char message[256];
    const auto &window = tile_->bounded->window;
    std::snprintf(message, sizeof(message),
                  "Bounded Deep Simple Beauty has no recorded colour carrier: "
                  "pixel=(%lld,%lld) records=%zu rgb=(%.9g,%.9g,%.9g)",
                  static_cast<long long>(window.x + pixel_ % window.width),
                  static_cast<long long>(window.y + pixel_ / window.width),
                  count_, double(rgb[0]), double(rgb[1]), double(rgb[2]));
    return DeepStatus::fail(DeepErrorCode::InvalidPayload, message);
  }
  return {};
}

void DeepEncodedPixelView::packed_bits(const size_t index,
                                       const bool include_color,
                                       uint32_t (&channels)[6]) const
{
  const auto &p = *bounded_;
  const size_t record = offset_ + index;
  const auto load = [&](const size_t offset, const size_t bytes) {
    uint32_t value = 0;
    std::memcpy(&value, p.planes.data() + offset, bytes);
    return value;
  };
  const size_t bytes = p.half ? 2 : 4;
  const size_t offset = p.count * 8 + record * bytes * (p.physical_rgb ? 4 : 1);
  channels[0] = load(offset + (p.physical_rgb ? 3 * bytes : 0), bytes);
  channels[4] = load(record * 4, 4);
  channels[5] = load((p.count + record) * 4, 4);
  if (!include_color) {
    return;
  }
  if (p.physical_rgb) {
    channels[1] = load(offset + 2 * bytes, bytes);
    channels[2] = load(offset + bytes, bytes);
    channels[3] = load(offset, bytes);
  }
  else if (p.simple_rgb) {
    const float alpha = p.half ? half_to_float(half(uint16_t(channels[0]))) :
                                 std::bit_cast<float>(channels[0]);
    const float weight = inverse_alpha_ > 0 ? std::clamp(alpha * inverse_alpha_, 0.0f, 1.0f) :
                                              float(int(index) == color_index_);
    const float *rgb = beauty_;
    for (int c = 0; c < 3; ++c) {
      const float value = rgb[2 - c] * weight;
      channels[1 + c] = p.half ? uint16_t(float_to_half(std::clamp(value, -65504.0f, 65504.0f))) :
                                 std::bit_cast<uint32_t>(value);
    }
  }
}

void DeepEncodedPixelView::pack_channels(unsigned char *const (&channels)[6],
                                         const bool include_color) const
{
  if (!count_) return;
  const auto &p = *bounded_;
  const size_t bytes = p.half ? 2 : 4;
  if constexpr (std::endian::native != std::endian::little) {
    for (size_t i = 0; i < count_; ++i) {
      uint32_t bits[6] = {};
      packed_bits(i, include_color, bits);
      for (int c = 0; c < 6; ++c) {
        if (!include_color && c > 0 && c < 4) continue;
        const size_t width = c < 4 ? bytes : 4;
        for (size_t b = 0; b < width; ++b) {
          channels[c][i * width + b] = static_cast<unsigned char>(bits[c] >> (8 * b));
        }
      }
    }
    return;
  }
  const unsigned char *input = p.planes.data();
  std::memcpy(channels[4], input + offset_ * 4, count_ * 4);
  std::memcpy(channels[5], input + (p.count + offset_) * 4, count_ * 4);
  input += p.count * 8 + offset_ * bytes * (p.physical_rgb ? 4 : 1);
  if (p.physical_rgb) {
    for (size_t i = 0; i < count_; ++i) {
      const size_t components = include_color ? 4 : 1;
      for (size_t c = 0; c < components; ++c) {
        std::memcpy(channels[c] + i * bytes, input + (4 * i + 3 - c) * bytes, bytes);
      }
    }
    return;
  }
  std::memcpy(channels[0], input, count_ * bytes);
  if (!include_color) return;
  if (!p.simple_rgb) {
    for (int c = 1; c <= 3; ++c) std::memset(channels[c], 0, count_ * bytes);
    return;
  }
  for (size_t i = 0; i < count_; ++i) {
    uint32_t bits = 0;
    std::memcpy(&bits, input + i * bytes, bytes);
    const float alpha = p.half ? half_to_float(half(uint16_t(bits))) : std::bit_cast<float>(bits);
    const float weight = inverse_alpha_ > 0 ? std::clamp(alpha * inverse_alpha_, 0.0f, 1.0f) :
                                              float(int(i) == color_index_);
    for (int c = 0; c < 3; ++c) {
      const float value = beauty_[2 - c] * weight;
      const uint32_t stored = p.half ?
          uint16_t(float_to_half(std::clamp(value, -65504.0f, 65504.0f))) :
          std::bit_cast<uint32_t>(value);
      std::memcpy(channels[1 + c] + i * bytes, &stored, bytes);
    }
  }
}

uint32_t DeepEncodedPixelView::nonzero_mask(const bool include_color) const
{
  if (!count_) {
    return 0;
  }
  const auto &payload = *bounded_;
  const size_t bytes = payload.half ? 2 : 4;
  const size_t components = payload.physical_rgb ? 4 : 1;
  const uint32_t magnitude = payload.half ? 0x7fffu : 0x7fffffffu;
  const unsigned char *input = payload.planes.data() + payload.count * 8 +
                               offset_ * bytes * components;
  const auto nonzero = [&](const unsigned char *channel) {
    uint32_t bits = 0;
    std::memcpy(&bits, channel, bytes);
    return (bits & magnitude) != 0;
  };
  uint32_t mask = 0;
  for (size_t i = 0; i < count_; ++i, input += bytes * components) {
    bool keep = nonzero(input + bytes * (components - 1));
    if (!keep && include_color) {
      if (payload.physical_rgb) {
        keep = nonzero(input) || nonzero(input + bytes) || nonzero(input + 2 * bytes);
      }
      else if (payload.simple_rgb && int(i) == color_index_) {
        uint32_t bits[6] = {};
        packed_bits(i, true, bits);
        keep = ((bits[1] | bits[2] | bits[3]) & magnitude) != 0;
      }
    }
    if (keep) {
      mask |= uint32_t(1) << i;
    }
  }
  return mask;
}

void DeepEncodedPixelView::pack_channels_masked(unsigned char *const (&channels)[6],
                                                const bool include_color,
                                                uint32_t mask) const
{
  const size_t bytes = bounded_->half ? 2 : 4;
  size_t output = 0;
  for (; mask; mask &= mask - 1, ++output) {
    uint32_t bits[6] = {};
    packed_bits(std::countr_zero(mask), include_color, bits);
    for (int c = 0; c < 6; ++c) {
      if (channels[c]) {
        const size_t width = c < 4 ? bytes : 4;
        for (size_t b = 0; b < width; ++b) {
          channels[c][output * width + b] = static_cast<unsigned char>(bits[c] >> (8 * b));
        }
      }
    }
  }
}

DeepStatus DeepFrameProduction::encoded_pixel(const DeepConsumerToken &token,
                                              const size_t index,
                                              DeepEncodedPixelLease &lease) const
{
  lease = {};
  if (const auto status = validate_consumer_token(token); !status)
    return status;
  size_t consumer = 0;
  while (consumer < storage_->consumers.size() && storage_->consumers[consumer].id != token.id)
    consumer++;
  return encoded_pixel_for_consumer(consumer, index, lease);
}

DeepStatus DeepFrameProduction::encoded_pixels(const DeepConsumerToken &token,
                                               const size_t first,
                                               const std::span<DeepEncodedPixelLease> leases) const
{
  if (const auto status = validate_consumer_token(token); !status)
    return status;
  if (first > storage_->pixel_count() || leases.size() > storage_->pixel_count() - first)
    return DeepStatus::fail(DeepErrorCode::InvalidLayout, "Deep encoded range outside frame");
  size_t consumer = 0;
  while (consumer < storage_->consumers.size() && storage_->consumers[consumer].id != token.id)
    consumer++;
  for (size_t i = 0; i < leases.size(); ++i) {
    if (const auto status = encoded_pixel_for_consumer(consumer, first + i, leases[i]); !status)
      return status;
  }
  return {};
}

DeepStatus DeepFrameProduction::encoded_row(const DeepConsumerToken &token,
                                           const size_t first,
                                           const size_t count,
                                           DeepEncodedRowLease &row) const
{
  row.release();
  if (const auto status = validate_consumer_token(token); !status)
    return status;
  if (first > storage_->pixel_count() || count > storage_->pixel_count() - first)
    return DeepStatus::fail(DeepErrorCode::InvalidLayout, "Deep encoded row outside frame");
  struct Rollback {
    DeepEncodedRowLease &row;
    bool complete = false;
    ~Rollback() { if (!complete) row.release(); }
  } rollback{row};
  size_t consumer = 0;
  while (consumer < storage_->consumers.size() && storage_->consumers[consumer].id != token.id)
    consumer++;
  row.pixels_.resize(count);
  /* Worst case: every pixel starts a distinct tile run. Reserve once so growth
   * cannot transiently retain two owner arrays or outlive the shared quota. */
  row.owners_.reserve(count);
  row.beauty_ = final_beauty_;
  size_t previous_tile = no_index, local = 0;
  uint32_t column = uint32_t(first % storage_->window.width);
  for (size_t i = 0; i < count; ++i) {
    const size_t index = first + i;
    if (!storage_->pixel_closed(index))
      return DeepStatus::fail(DeepErrorCode::Incomplete, "Deep encoded pixel not yet published");
    const size_t tile_index = storage_->pixel_cache_tiles[index];
    const auto &tile = storage_->encoded_tiles[tile_index];
    const auto &window = tile->window;
    if (tile_index != previous_tile || column == 0) {
      const int64_t x = int64_t(storage_->window.x) + column;
      const int64_t y = int64_t(storage_->window.y) + index / storage_->window.width;
      local = size_t(y - window.y) * window.width + size_t(x - window.x);
      if (tile_index != previous_tile) row.owners_.push_back(tile);
      previous_tile = tile_index;
    }
    auto &view = row.pixels_[i];
    view = {};
    view.tile_ = tile.get();
    view.pixel_ = local;
    if (tile->bounded) {
      const auto &payload = tile->bounded->consumers[consumer];
      view.bounded_ = payload.get();
      view.offset_ = payload->offsets[local];
      view.count_ = payload->offsets[local + 1] - view.offset_;
      if (payload->simple_rgb) {
        view.beauty_ = final_beauty_ ? final_beauty_->data() + index * 3 :
                        !tile->bounded->beauty.empty() ? tile->bounded->beauty.data() + local * 3 :
                                                         nullptr;
      }
    }
    else {
      const auto &range = tile->ranges[consumer * size_t(window.width) * window.height + local];
      view.offset_ = range.offset;
      view.count_ = range.count;
    }
    ++local;
    if (++column == storage_->window.width) column = 0;
  }
  row.published_ = count;
  rollback.complete = true;
  return {};
}

DeepStatus DeepFrameProduction::encoded_pixel_for_consumer(const size_t consumer,
                                                          const size_t index,
                                                          DeepEncodedPixelLease &lease) const
{
  lease = {};
  if (!storage_->pixel_closed(index))
    return DeepStatus::fail(DeepErrorCode::Incomplete, "Deep encoded pixel not yet published");
  const auto &tile = storage_->encoded_tiles[storage_->pixel_cache_tiles[index]];
  const auto &window = tile->window;
  const int64_t x = int64_t(storage_->window.x) + index % storage_->window.width;
  const int64_t y = int64_t(storage_->window.y) + index / storage_->window.width;
  const size_t local = size_t(y - window.y) * window.width + size_t(x - window.x);
  if (tile->bounded) {
    const auto &payload = tile->bounded->consumers[consumer];
    lease.owner_ = tile;
    lease.tile_ = tile.get();
    lease.bounded_ = payload.get();
    lease.pixel_ = local;
    lease.offset_ = payload->offsets[local];
    lease.count_ = payload->offsets[local + 1] - lease.offset_;
    if (payload->simple_rgb) {
      lease.final_beauty_owner_ = final_beauty_;
      lease.beauty_ = final_beauty_ ? final_beauty_->data() + index * 3 :
                      !tile->bounded->beauty.empty() ? tile->bounded->beauty.data() + local * 3 :
                                                       nullptr;
    }
    return {};
  }
  const auto &range = tile->ranges[consumer * size_t(window.width) * window.height + local];
  lease.owner_ = tile;
  lease.tile_ = tile.get();
  lease.offset_ = range.offset;
  lease.count_ = range.count;
  return {};
}


CCL_NAMESPACE_END
