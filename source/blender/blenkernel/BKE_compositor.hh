/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#pragma once

#include <string>

#include "BLI_set.hh"

namespace blender {

struct Scene;
struct ViewLayer;
struct bContext;
struct DepsNodeHandle;
struct bNode;
struct bNodeTree;

namespace bke::compositor {

/* Get the set of all passes used by the compositor for the given view layer, identified by their
 * pass names. This might be a superset of the passes actually supported by the render engine, in
 * which case, the compositor will return an invalid output and issue a warning. */
Set<std::string> get_used_passes(const Scene &scene, const ViewLayer *view_layer);

/* Checks if the viewport compositor is currently being used. This is similar to
 * DRWContext::is_viewport_compositor_enabled but checks all 3D views. */
bool is_viewport_compositor_used(const bContext &context);

/* Note: Links to the File Output node do not guarantee it will write a result to disk, e.g. if
 * Menu Switch nodes exists but it's a good estimation without evaluating the node tree. */
bool node_tree_has_linked_file_output(const bNodeTree *node_tree);

/* Resolve the Render Layers source represented by a Deep File Output node. Deep output bypasses
 * regular input evaluation, so unsupported link shapes must not fall back to unrelated data. */
bool deep_output_target_from_node(const bNode &node,
                                  const Scene &default_scene,
                                  const Scene **r_scene,
                                  int *r_view_layer_id,
                                  bool *r_alpha_only);

/* Add the depsgraph relations needed by the compositor node tree of the given scene. A handle for
 * the compositor output depsgraph node is given to be the target of the relation. */
void add_depsgraph_relations(Scene &scene, DepsNodeHandle *compositor_output_depsgraph_node);

}  // namespace bke::compositor
}  // namespace blender
