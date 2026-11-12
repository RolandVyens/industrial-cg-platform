/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup render
 */

#pragma once

#include <memory>
#include <cstdint>
#include <string>
#include <vector>

#include "IMB_deep_sample.hh"

namespace blender {

/* Render-owned canonical source. Every real consumer encodes with its own author settings
 * and acknowledges checked file close through this owner. */
class RenderDeepOutputSource {
 public:
  virtual ~RenderDeepOutputSource() = default;
  virtual bool finish(std::string &error) = 0;
  /* Manual saves replay a frozen direct consumer; they cannot create a new approximation. */
  virtual bool save_direct(const char * /*filepath*/,
                           int /*compression*/,
                           bool /*half*/,
                           float /*depth_tolerance*/,
                           float /*alpha_tolerance*/,
                           std::string &error,
                           const char * /*view_name*/)
  {
    error = "Deep manual save requires a retained direct source; render a new frame";
    return false;
  }
  virtual bool write(const char *filepath,
                     int compression,
                     bool half,
                     bool alpha_only,
                     float depth_tolerance,
                     float alpha_tolerance,
                     std::string &error,
                     uint64_t consumer_key = 0,
                     const char *view_name = "") = 0;
};

/**
 * Owned deep EXR sample storage for RenderResult and compositor access.
 */
struct RenderDeepData {
  std::shared_ptr<RenderDeepOutputSource> canonical_source;
  std::vector<std::vector<DeepSample>> pixels;
  bool has_display_window = false;
  int display_size[2] = {0, 0};
  int display_offset[2] = {0, 0};
  int data_offset[2] = {0, 0};
};

}  // namespace blender
