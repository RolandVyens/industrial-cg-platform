/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "RE_pipeline.h"

namespace blender {
struct RenderEngine;

/* Local and active parent-compositor consumers. The caller filters target
 * Scene/ViewLayer; authored node keys, precision and tolerances are preserved. */
void RE_engine_foreach_deep_exr_file_output(
    const RenderEngine *engine,
    Scene *scene,
    FunctionRef<void(const RenderDeepOutputConsumer &)> callback);
}  // namespace blender
