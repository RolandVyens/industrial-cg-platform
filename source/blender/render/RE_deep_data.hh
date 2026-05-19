/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup render
 */

#pragma once

#include <vector>

#include "IMB_deep_sample.hh"

namespace blender {

/**
 * Owned deep EXR sample storage for RenderResult and compositor access.
 */
struct RenderDeepData {
  std::vector<std::vector<DeepSample>> pixels;
};

}  // namespace blender
