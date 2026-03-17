/* SPDX-FileCopyrightText: 2021-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <vector>

#include "session/output_driver.h"

namespace blender {
struct RenderEngine;
}

CCL_NAMESPACE_BEGIN

class BlenderOutputDriver : public OutputDriver {
 public:
  explicit BlenderOutputDriver(blender::RenderEngine &b_engine);
  ~BlenderOutputDriver() override;

  void write_render_tile(const Tile &tile) override;
  bool update_render_tile(const Tile &tile) override;
  bool read_render_tile(const Tile &tile) override;

  /* Get captured Combined pass for deep recolor (post-render). */
  const float *get_combined_pass(int &width, int &height) const;

  /* Get captured Debug Sample Count pass for deep coverage reconstruction. */
  const float *get_sample_count_pass(int &width, int &height) const;

 protected:
  blender::RenderEngine &b_engine_;

  /* Captured Combined pass for deep output recolor. */
  std::vector<float> combined_pass_buffer_;
  int combined_width_ = 0;
  int combined_height_ = 0;

  /* Captured Debug Sample Count pass for deep edge coverage reconstruction. */
  std::vector<float> sample_count_pass_buffer_;
  int sample_count_width_ = 0;
  int sample_count_height_ = 0;
};

CCL_NAMESPACE_END
