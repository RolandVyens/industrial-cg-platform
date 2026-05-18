/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_node_types.h"

#include "GPU_shader.hh"

#include "COM_context.hh"
#include "COM_render_context.hh"
#include "COM_static_cache_manager.hh"

namespace blender::compositor {

Context::Context(StaticCacheManager &cache_manager) : cache_manager_(cache_manager) {};

Result Context::get_pass(const Scene * /*scene*/, int /*view_layer*/, const char * /*name*/)
{
  compositor::Result invalid_pass = this->create_result(compositor::ResultType::Color);
  invalid_pass.allocate_invalid();
  return invalid_pass;
}

bool Context::get_deep_data(const Scene * /*scene*/,
                            int /*view_layer_id*/,
                            RenderDeepData **r_data,
                            int *r_width,
                            int *r_height) const
{
  if (r_data) {
    *r_data = nullptr;
  }
  if (r_width) {
    *r_width = 0;
  }
  if (r_height) {
    *r_height = 0;
  }

  RenderContext *render_ctx = this->render_context();
  if (render_ctx && render_ctx->has_deep_data()) {
    if (r_data) {
      *r_data = render_ctx->get_deep_data();
    }
    if (r_width) {
      *r_width = render_ctx->get_deep_width();
    }
    if (r_height) {
      *r_height = render_ctx->get_deep_height();
    }
    return true;
  }

  return false;
}

const RenderData &Context::get_render_data() const
{
  return this->get_scene().r;
}

StringRef Context::get_view_name() const
{
  return "";
}

ResultPrecision Context::get_precision() const
{
  return ResultPrecision::Full;
}

void Context::set_info_message(StringRef /*message*/) const {}

bool Context::treat_viewer_as_group_output() const
{
  return false;
}

void Context::populate_meta_data_for_pass(const Scene * /*scene*/,
                                          int /*view_layer_id*/,
                                          const char * /*pass_name*/,
                                          MetaData & /*meta_data*/) const
{
}

RenderContext *Context::render_context() const
{
  return nullptr;
}

nodes::eval_log::NodesEvalLog *Context::nodes_evaluation_log() const
{
  return nullptr;
}

void Context::evaluate_operation_post() const {}

bool Context::is_canceled() const
{
  return false;
}

float Context::get_render_percentage() const
{
  return get_render_data().size / 100.0f;
}

int Context::get_frame_number() const
{
  return get_render_data().cfra;
}

float Context::get_time() const
{
  const float frame_number = float(get_frame_number());
  const float frame_rate = float(get_render_data().frs_sec) /
                           float(get_render_data().frs_sec_base);
  return frame_number / frame_rate;
}

eCompositorDenoiseQaulity Context::get_denoise_quality() const
{
  if (this->render_context()) {
    return static_cast<eCompositorDenoiseQaulity>(
        this->get_render_data().compositor_denoise_final_quality);
  }

  return static_cast<eCompositorDenoiseQaulity>(
      this->get_render_data().compositor_denoise_preview_quality);
}

gpu::Shader *Context::get_shader(const char *info_name, ResultPrecision precision)
{
  return cache_manager().cached_shaders.get(info_name, precision);
}

gpu::Shader *Context::get_shader(const char *info_name)
{
  return get_shader(info_name, get_precision());
}

Result Context::create_result(ResultType type, ResultPrecision precision)
{
  return Result(*this, type, precision);
}

Result Context::create_result(ResultType type)
{
  return create_result(type, get_precision());
}

StaticCacheManager &Context::cache_manager()
{
  return cache_manager_;
}

const Strip *Context::get_strip() const
{
  return nullptr;
}

}  // namespace blender::compositor
