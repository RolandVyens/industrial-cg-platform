/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup imbuf
 */

#pragma once

namespace blender {

/* Sample data for deep EXR output. */
struct DeepSample {
  float r, g, b, a;
  float z;      /* Front depth. */
  float z_back; /* Back depth. */
};

}  // namespace blender
