/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup render
 */

/* Global includes */

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "BLI_math_base.h"
#include "BLI_math_matrix.h"
#include "BLI_rect.h"

#include "DNA_scene_types.h"

#include "BKE_camera.h"

/* this module */
#include "RE_pipeline.h"
#include "render_types.h"

namespace blender {

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

Object *RE_GetCamera(Render *re)
{
  Object *camera = re->camera_override ? re->camera_override : re->scene->camera;
  return BKE_camera_multiview_render(*re->main, re->scene, camera, re->viewname);
}

void RE_SetOverrideCamera(Render *re, Object *cam_ob)
{
  re->camera_override = cam_ob;
}

void RE_SetCamera(Render *re, const Object *cam_ob)
{
  CameraParams params;

  /* setup parameters */
  BKE_camera_params_init(&params);
  BKE_camera_params_from_object(&params, cam_ob);
  BKE_camera_multiview_params(&re->r, &params, cam_ob, re->viewname);

  /* Compute matrix, view-plane, etc. */
  BKE_camera_params_compute_viewplane(&params, re->winx, re->winy, re->r.xasp, re->r.yasp);
  BKE_camera_params_compute_matrix(&params);

  /* extract results */
  copy_m4_m4(re->winmat, params.winmat);
  re->clip_start = params.clip_start;
  re->clip_end = params.clip_end;
  re->viewplane = params.viewplane;
}

void RE_GetCameraWindow(Render *re, const Object *camera, float r_winmat[4][4])
{
  RE_SetCamera(re, camera);
  copy_m4_m4(r_winmat, re->winmat);
}

void RE_GetCameraWindowWithOverscan(const Render *re, float overscan, float r_winmat[4][4])
{
  RE_GetWindowMatrixWithOverscan(
      re->winmat[3][3] != 0.0f, re->clip_start, re->clip_end, re->viewplane, overscan, r_winmat);
}

RenderOverscanPadding RE_overscan_padding_resolve(const bool use_pixel_mode,
                                                  const float percentage,
                                                  const int pixel_left,
                                                  const int pixel_right,
                                                  const int pixel_bottom,
                                                  const int pixel_top,
                                                  const int reference_width,
                                                  const int reference_height)
{
  RenderOverscanPadding padding;
  if (use_pixel_mode) {
    padding.left = max_ii(0, pixel_left);
    padding.right = max_ii(0, pixel_right);
    padding.bottom = max_ii(0, pixel_bottom);
    padding.top = max_ii(0, pixel_top);
    return padding;
  }

  const float overscan = max_ff(0.0f, percentage) / 100.0f;
  const int reference_dimension = max_ii(0, max_ii(reference_width, reference_height));
  const int uniform_padding = max_ii(0, int(ceilf(overscan * reference_dimension)));
  padding.left = uniform_padding;
  padding.right = uniform_padding;
  padding.bottom = uniform_padding;
  padding.top = uniform_padding;
  return padding;
}

void RE_GetCameraModelMatrix(const Render *re, const Object *camera, float r_modelmat[4][4])
{
  BKE_camera_multiview_model_matrix(&re->r, camera, re->viewname, r_modelmat);
}

void RE_GetWindowMatrixWithOverscan(bool is_ortho,
                                    float clip_start,
                                    float clip_end,
                                    rctf viewplane,
                                    float overscan,
                                    float r_winmat[4][4])
{
  CameraParams params;
  params.is_ortho = is_ortho;
  params.clip_start = clip_start;
  params.clip_end = clip_end;
  params.viewplane = viewplane;

  overscan *= max_ff(BLI_rctf_size_x(&params.viewplane), BLI_rctf_size_y(&params.viewplane));

  params.viewplane.xmin -= overscan;
  params.viewplane.xmax += overscan;
  params.viewplane.ymin -= overscan;
  params.viewplane.ymax += overscan;
  BKE_camera_params_compute_matrix(&params);
  copy_m4_m4(r_winmat, params.winmat);
}

void RE_GetViewPlane(Render *re, rctf *r_viewplane, rcti *r_disprect)
{
  *r_viewplane = re->viewplane;

  /* make disprect zero when no border render, is needed to detect changes in 3d view render */
  if (re->r.mode & R_BORDER) {
    *r_disprect = re->disprect;
  }
  else {
    BLI_rcti_init(r_disprect, 0, 0, 0, 0);
  }
}

}  // namespace blender
