/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <memory>
#include <string>

#include "BLI_assert.h"
#include "BLI_listbase.h"
#include "BLI_map.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector_types.hh"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_utildefines.h"

#include "MEM_guardedalloc.h"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "DNA_scene_types.h"
#include "DNA_windowmanager_types.h"

#include "BKE_image.hh"
#include "BKE_image_save.hh"
#include "BKE_report.hh"
#include "BKE_scene.hh"

#include "RE_pipeline.h"

#include "COM_render_context.hh"

namespace blender::compositor {

/* ------------------------------------------------------------------------------------------------
 * File Output
 */

FileOutput::FileOutput(const std::string &path,
                       const ImageFormatData &format,
                       int2 size,
                       bool save_as_render,
                       bool has_display_window,
                       int2 display_size,
                       int2 display_offset,
                       int2 data_offset)
    : path_(path),
      format_(format),
      save_as_render_(save_as_render),
      has_display_window_(has_display_window),
      display_size_(display_size),
      display_offset_(display_offset),
      data_offset_(data_offset)
{
  render_result_ = MEM_new<RenderResult>("Temporary Render Result For File Output");

  render_result_->rectx = size.x;
  render_result_->recty = size.y;

  /* NOTE: set dummy values which will won't be used unless overwritten.
   * When `save_as_render` is set, this is overwritten by the scenes PPM setting.
   * We *could* support setting the DPI in the file output node too. */
  render_result_->ppm[0] = 0.0;
  render_result_->ppm[1] = 0.0;

  /* File outputs are always single layer, as images are actually stored in passes on that single
   * layer. Create a single unnamed layer to add the passes to. A single unnamed layer is treated
   * by the EXR writer as a special case where the channel names take the form:
   *   <pass-name>.<view-name>.<channel-id>
   * Otherwise, the layer name would have preceded in the pass name in yet another section. */
  RenderLayer *render_layer = MEM_new<RenderLayer>("Render Layer For File Output.");
  BLI_addtail(&render_result_->layers, render_layer);
  render_layer->name[0] = '\0';

  /* File outputs do not support previews. */
  format_.flag &= ~R_IMF_FLAG_PREVIEW_JPG;
}

FileOutput::~FileOutput()
{
  RE_FreeRenderResult(render_result_);
}

void FileOutput::assign_display_window(ImBuf *image_buffer) const
{
  if (!has_display_window_) {
    return;
  }

  image_buffer->flags |= IB_has_display_window;
  image_buffer->display_size[0] = display_size_.x;
  image_buffer->display_size[1] = display_size_.y;
  image_buffer->display_offset[0] = display_offset_.x;
  image_buffer->display_offset[1] = display_offset_.y;
  image_buffer->data_offset[0] = data_offset_.x;
  image_buffer->data_offset[1] = data_offset_.y;
}

void FileOutput::add_view(const char *view_name)
{
  /* Empty views can only be added for EXR images. */
  BLI_assert(ELEM(format_.imtype, R_IMF_IMTYPE_OPENEXR, R_IMF_IMTYPE_MULTILAYER));

  RenderView *render_view = MEM_new<RenderView>("Render View For File Output.");
  BLI_addtail(&render_result_->views, render_view);
  STRNCPY_UTF8(render_view->name, view_name);
}

void FileOutput::add_view(const char *view_name, int channels, float *buffer)
{
  RenderView *render_view = MEM_new<RenderView>("Render View For File Output.");
  BLI_addtail(&render_result_->views, render_view);
  STRNCPY_UTF8(render_view->name, view_name);

  render_view->ibuf = IMB_allocImBuf(
      render_result_->rectx, render_result_->recty, channels * 8, 0);
  render_view->ibuf->channels = channels;
  this->assign_display_window(render_view->ibuf);
  IMB_assign_float_buffer(render_view->ibuf, buffer, IB_TAKE_OWNERSHIP);
}

void FileOutput::add_pass(const char *pass_name,
                          const char *view_name,
                          const char *channels,
                          float *buffer)
{
  /* Passes can only be added for EXR images. */
  BLI_assert(ELEM(format_.imtype, R_IMF_IMTYPE_OPENEXR, R_IMF_IMTYPE_MULTILAYER));

  RenderLayer *render_layer = static_cast<RenderLayer *>(render_result_->layers.first);
  RenderPass *render_pass = MEM_new<RenderPass>("Render Pass For File Output.");
  BLI_addtail(&render_layer->passes, render_pass);
  STRNCPY(render_pass->name, pass_name);
  STRNCPY(render_pass->view, view_name);
  STRNCPY(render_pass->chan_id, channels);

  const int channels_count = BLI_strnlen(channels, 4);
  render_pass->rectx = render_result_->rectx;
  render_pass->recty = render_result_->recty;
  render_pass->channels = channels_count;

  render_pass->ibuf = IMB_allocImBuf(
      render_result_->rectx, render_result_->recty, channels_count * 8, 0);
  render_pass->ibuf->channels = channels_count;
  copy_v2_v2_db(render_pass->ibuf->ppm, render_result_->ppm);
  this->assign_display_window(render_pass->ibuf);
  IMB_assign_float_buffer(render_pass->ibuf, buffer, IB_TAKE_OWNERSHIP);
}

void FileOutput::add_meta_data(std::string key, std::string value)
{
  meta_data_.add(key, value);
}

void FileOutput::save(Scene *scene)
{
  ReportList reports;
  BKE_reports_init(&reports, RPT_STORE);

  /* Add scene stamp data as meta data as well as the custom meta data. */
  BKE_render_result_stamp_info(scene, nullptr, render_result_, false);
  for (const auto &field : meta_data_.items()) {
    BKE_render_result_stamp_data(render_result_, field.key.c_str(), field.value.c_str());
  }

  /* NOTE: without this the file will be written without any density information.
   * So always write this. */
  if (save_as_render_ || true) {
    BKE_scene_ppm_get(&scene->r, render_result_->ppm);
  }

  BKE_image_render_write(
      &reports, render_result_, scene, true, path_.c_str(), &format_, save_as_render_);

  BKE_reports_free(&reports);
}

/* ------------------------------------------------------------------------------------------------
 * Render Context
 */

FileOutput &RenderContext::get_file_output(std::string path,
                                           ImageFormatData format,
                                           int2 size,
                                           bool save_as_render,
                                           bool has_display_window,
                                           int2 display_size,
                                           int2 display_offset,
                                           int2 data_offset)
{
  return *file_outputs_.lookup_or_add_cb(path, [&]() {
    return std::make_unique<FileOutput>(
        path, format, size, save_as_render, has_display_window, display_size, display_offset, data_offset);
  });
}

void RenderContext::save_file_outputs(Scene *scene)
{
  for (std::unique_ptr<FileOutput> &file_output : file_outputs_.values()) {
    file_output->save(scene);
  }
}

}  // namespace blender::compositor
