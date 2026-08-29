/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include <cmath>
#include <cstdio>
#include <cstring>

#include "DEG_depsgraph_query.hh"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "RNA_prototypes.hh"
#include "BKE_image_format.hh"
#include "BKE_main.hh"
#include "BKE_path_templates.hh"
#include "BKE_report.hh"

#include "device/device.h"

#include "scene/background.h"
#include "scene/bake.h"
#include "scene/camera.h"
#include "scene/film.h"
#include "scene/integrator.h"
#include "scene/light.h"
#include "scene/mesh.h"
#include "scene/object.h"
#include "scene/scene.h"
#include "scene/shader.h"
#include "scene/stats.h"

#include "session/buffers.h"
#include "session/deep_buffers.h"
#include "session/deep_output_driver.h"
#include "session/session.h"

#include "IMB_imbuf.hh"
#include "IMB_openexr.hh"
#include "IMB_imbuf_types.hh"
#include "RE_deep_data.hh"
#include "RE_pipeline.h"

#include "util/hash.h"
#include "util/log.h"
#include "util/murmurhash.h"
#include "util/path.h"
#include "util/progress.h"
#include "util/time.h"
#include "util/vector.h"

#include "blender/display_driver.h"
#include "blender/output_driver.h"
#include "blender/session.h"
#include "blender/sync.h"
#include "blender/util.h"

CCL_NAMESPACE_BEGIN

namespace {

bool image_format_is_exr_overscan_target(const blender::ImageFormatData &image_format)
{
  return ELEM(image_format.imtype,
              blender::R_IMF_IMTYPE_OPENEXR,
              blender::R_IMF_IMTYPE_MULTILAYER,
              blender::R_IMF_IMTYPE_DEEP_EXR);
}

bool viewport_allows_overscan(blender::Scene *b_scene,
                              blender::View3D *b_v3d,
                              blender::RegionView3D *b_rv3d)
{
  UNUSED_VARS(b_scene, b_v3d, b_rv3d);
  return false;
}

bool offline_render_needs_overscan(blender::Scene *b_scene, const blender::RenderData *b_render)
{
  const bool has_scene = b_scene != nullptr;
  const bool has_render = b_render != nullptr;
  if (!has_scene || !has_render) {
    return false;
  }

  blender::PointerRNA scene_rna_ptr = RNA_id_pointer_create(&b_scene->id);
  blender::PointerRNA cscene = RNA_pointer_get(&scene_rna_ptr, "cycles");
  if (!RNA_boolean_get(&cscene, "use_overscan")) {
    return false;
  }

  if ((b_render->mode & blender::R_BORDER) != 0) {
    return false;
  }

  const bool is_direct_exr = has_render && image_format_is_exr_overscan_target(b_render->im_format);
  const bool has_exr_file_output = has_scene && blender::RE_scene_has_exr_file_output(b_scene);
  return is_direct_exr || has_exr_file_output;
}

bool buffer_params_has_overscan(const BufferParams &buffer_params)
{
  return buffer_params.window_x != 0 || buffer_params.window_y != 0 ||
         buffer_params.width > buffer_params.full_width ||
         buffer_params.height > buffer_params.full_height ||
         buffer_params.full_x < 0 || buffer_params.full_y < 0 ||
         (buffer_params.full_x + buffer_params.width) > buffer_params.full_width ||
         (buffer_params.full_y + buffer_params.height) > buffer_params.full_height;
}

struct RenderDisplayWindow {
  bool has_display_window = false;
  int display_width = 0;
  int display_height = 0;
  int display_offset_x = 0;
  int display_offset_y = 0;
  int data_offset_x = 0;
  int data_offset_y = 0;
};

RenderDisplayWindow render_display_window_from_buffer_params(const BufferParams &buffer_params)
{
  RenderDisplayWindow display_window;
  if (!buffer_params_has_overscan(buffer_params)) {
    return display_window;
  }

  display_window.has_display_window = true;
  display_window.display_width = buffer_params.full_width;
  display_window.display_height = buffer_params.full_height;
  display_window.display_offset_x = 0;
  display_window.display_offset_y = 0;
  display_window.data_offset_x = buffer_params.full_x + min(buffer_params.window_x, 0);
  const int data_bottom = buffer_params.full_y + min(buffer_params.window_y, 0);
  /* Cycles buffer coordinates use a bottom-left origin, while OpenEXR windows use a top-left
   * origin. Account for the data height so cropped overscan is placed in the correct region. */
  display_window.data_offset_y = buffer_params.full_height - (data_bottom + buffer_params.height);
  return display_window;
}

bool render_output_filepath_for_still(const blender::Main *b_main,
                                      const blender::Scene *b_scene,
                                      const blender::RenderData *b_render,
                                      char filepath[FILE_MAX])
{
  if (!b_main || !b_scene || !b_render) {
    return false;
  }

  const char *relbase = blender::BKE_main_blendfile_path(b_main);
  blender::bke::path_templates::VariableMap template_variables;
  blender::BKE_add_template_variables_general(template_variables, &b_scene->id);
  blender::BKE_add_template_variables_for_render_path(template_variables, *b_scene);

  const blender::Vector<blender::bke::path_templates::Error> errors =
      blender::BKE_image_path_from_imformat(filepath,
                                            b_render->pic,
                                            relbase,
                                            &template_variables,
                                            b_scene->r.cfra,
                                            &b_render->im_format,
                                            (b_render->scemode & blender::R_EXTENSION) != 0,
                                            false,
                                            nullptr);
  return errors.is_empty();
}

void render_result_resize_for_overscan(blender::RenderResult *render_result,
                                       const BufferParams &buffer_params)
{
  if (!render_result || !buffer_params_has_overscan(buffer_params)) {
    return;
  }

  render_result->rectx = buffer_params.width;
  render_result->recty = buffer_params.height;
  render_result->passes_allocated = false;

  if (render_result->ibuf) {
    blender::IMB_freeImBuf(render_result->ibuf);
    render_result->ibuf = nullptr;
  }

  for (blender::RenderView *render_view = static_cast<blender::RenderView *>(render_result->views.first);
       render_view;
       render_view = render_view->next)
  {
    if (render_view->ibuf) {
      blender::IMB_freeImBuf(render_view->ibuf);
      render_view->ibuf = nullptr;
    }
  }

  for (blender::RenderLayer *render_layer = static_cast<blender::RenderLayer *>(render_result->layers.first);
       render_layer;
       render_layer = render_layer->next)
  {
    render_layer->rectx = buffer_params.width;
    render_layer->recty = buffer_params.height;

    for (blender::RenderPass *render_pass = static_cast<blender::RenderPass *>(render_layer->passes.first);
         render_pass;
         render_pass = render_pass->next)
    {
      render_pass->rectx = buffer_params.width;
      render_pass->recty = buffer_params.height;

      if (render_pass->ibuf) {
        blender::IMB_freeImBuf(render_pass->ibuf);
        render_pass->ibuf = nullptr;
      }
    }
  }
}

void render_result_apply_display_window(blender::RenderResult *render_result,
                                        const BufferParams &buffer_params)
{
  if (!render_result || !buffer_params_has_overscan(buffer_params)) {
    return;
  }

  /* Render passes allocate image buffers lazily. Ensure they exist before attaching the OpenEXR
   * display/data window metadata that compositor File Output nodes preserve. */
  blender::RE_render_result_passes_allocated_ensure(render_result);

  auto apply_to_image_buffer = [&](blender::ImBuf *image_buffer) {
    if (!image_buffer) {
      return;
    }

    const RenderDisplayWindow display_window = render_display_window_from_buffer_params(buffer_params);
    image_buffer->flags |= blender::ImBufFlags::HasDisplayWindow;
    image_buffer->display_size[0] = display_window.display_width;
    image_buffer->display_size[1] = display_window.display_height;
    image_buffer->display_offset[0] = display_window.display_offset_x;
    image_buffer->display_offset[1] = display_window.display_offset_y;
    image_buffer->data_offset[0] = display_window.data_offset_x;
    image_buffer->data_offset[1] = display_window.data_offset_y;
  };

  for (blender::RenderView *render_view = static_cast<blender::RenderView *>(render_result->views.first);
       render_view;
       render_view = render_view->next)
  {
    apply_to_image_buffer(render_view->ibuf);
  }

  for (blender::RenderLayer *render_layer = static_cast<blender::RenderLayer *>(render_result->layers.first);
       render_layer;
       render_layer = render_layer->next)
  {
    if (render_layer->deep_data) {
      const RenderDisplayWindow display_window = render_display_window_from_buffer_params(buffer_params);
      render_layer->deep_data->has_display_window = display_window.has_display_window;
      render_layer->deep_data->display_size[0] = display_window.display_width;
      render_layer->deep_data->display_size[1] = display_window.display_height;
      render_layer->deep_data->display_offset[0] = display_window.display_offset_x;
      render_layer->deep_data->display_offset[1] = display_window.display_offset_y;
      render_layer->deep_data->data_offset[0] = display_window.data_offset_x;
      render_layer->deep_data->data_offset[1] = display_window.data_offset_y;
    }

    for (blender::RenderPass *render_pass = static_cast<blender::RenderPass *>(render_layer->passes.first);
         render_pass;
         render_pass = render_pass->next)
    {
      apply_to_image_buffer(render_pass->ibuf);
    }
  }

  if (render_result->deep_data) {
    const RenderDisplayWindow display_window = render_display_window_from_buffer_params(buffer_params);
    render_result->deep_data->has_display_window = display_window.has_display_window;
    render_result->deep_data->display_size[0] = display_window.display_width;
    render_result->deep_data->display_size[1] = display_window.display_height;
    render_result->deep_data->display_offset[0] = display_window.display_offset_x;
    render_result->deep_data->display_offset[1] = display_window.display_offset_y;
    render_result->deep_data->data_offset[0] = display_window.data_offset_x;
    render_result->deep_data->data_offset[1] = display_window.data_offset_y;
  }
}

}  // namespace

DeviceTypeMask BlenderSession::device_override = DEVICE_MASK_ALL;
bool BlenderSession::headless = false;
bool BlenderSession::print_render_stats = false;

BlenderSession::BlenderSession(blender::RenderEngine &b_engine,
                               blender::UserDef &b_userpref,
                               blender::Main &b_data,
                               bool preview_osl)
    : session(nullptr),
      scene(nullptr),
      sync(nullptr),
      b_engine(b_engine),
      b_userpref(b_userpref),
      b_data(&b_data),
      b_render(RE_engine_get_render_data(b_engine.re)),
      b_depsgraph(nullptr),
      b_scene(nullptr),
      b_screen(nullptr),
      b_v3d(nullptr),
      b_rv3d(nullptr),
      width(0),
      height(0),
      pixelsize(1.0f),
      preview_osl(preview_osl),
      python_thread_state(nullptr),
      use_developer_ui(b_userpref.experimental.use_cycles_debug &&
                       (b_userpref.flag & blender::USER_DEVELOPER_UI) != 0)
{
  /* offline render */
  background = true;
  last_redraw_time = 0.0;
  start_resize_time = 0.0;
  last_status_time = 0.0;
}

BlenderSession::BlenderSession(blender::RenderEngine &b_engine,
                               blender::UserDef &b_userpref,
                               blender::Main &b_data,
                               blender::bScreen *b_screen,
                               blender::View3D *b_v3d,
                               blender::RegionView3D *b_rv3d,
                               const int width,
                               const int height)
    : session(nullptr),
      scene(nullptr),
      sync(nullptr),
      b_engine(b_engine),
      b_userpref(b_userpref),
      b_data(&b_data),
      b_render(nullptr),
      b_depsgraph(nullptr),
      b_scene(nullptr),
      b_screen(b_screen),
      b_v3d(b_v3d),
      b_rv3d(b_rv3d),
      width(width),
      height(height),
      pixelsize(blender::U.pixelsize),
      preview_osl(false),
      python_thread_state(nullptr),
      use_developer_ui(b_userpref.experimental.use_cycles_debug &&
                       (b_userpref.flag & blender::USER_DEVELOPER_UI) != 0)
{
  /* 3d view render */
  background = false;
  last_redraw_time = 0.0;
  start_resize_time = 0.0;
  last_status_time = 0.0;
}

BlenderSession::~BlenderSession()
{
  free_session();
}

void BlenderSession::create_session()
{
  const SessionParams session_params = BlenderSync::get_session_params(
      b_engine, b_userpref, *b_scene, background, pixelsize);
  const SceneParams scene_params = BlenderSync::get_scene_params(
      b_userpref, *b_data, *b_scene, background, use_developer_ui);
  const bool session_pause = BlenderSync::get_session_pause(*b_scene, background);

  /* reset status/progress */
  last_status = "";
  last_error = "";
  last_progress = -1.0;
  start_resize_time = 0.0;

  /* create session */
  session = make_unique<Session>(session_params, scene_params);
  session->progress.set_update_callback([this] { tag_redraw(); });
  session->progress.set_cancel_callback([this] { test_cancel(); });
  session->set_pause(session_pause);

  /* create scene */
  scene = session->scene.get();
  scene->name = BKE_id_name(b_scene->id);

  /* create sync */
  sync = make_unique<BlenderSync>(
      b_engine, *b_data, *b_scene, scene, !background, use_developer_ui, session->progress);
  if (b_v3d) {
    const bool use_viewport_overscan = viewport_allows_overscan(b_scene, b_v3d, b_rv3d);
    sync->sync_view(b_v3d, b_rv3d, width, height, use_viewport_overscan);
  }
  else {
    sync->sync_camera(*b_render, width, height, "", false);
  }

  /* set buffer parameters */
  const BufferParams buffer_params = BlenderSync::get_buffer_params(
      b_v3d,
      b_rv3d,
      b_scene,
      scene->camera,
      width,
      height,
      viewport_allows_overscan(b_scene, b_v3d, b_rv3d));
  session->reset(session_params, buffer_params);

  /* Viewport and preview (as in, material preview) does not do tiled rendering, so can inform
   * engine that no tracking of the tiles state is needed.
   * The offline rendering will make a decision when tile is being written. The penalty of asking
   * the engine to keep track of tiles state is minimal, so there is nothing to worry about here
   * about possible single-tiled final render. */
  if ((b_engine.flag & blender::RE_ENGINE_PREVIEW) == 0 && !b_v3d) {
    b_engine.flag |= blender::RE_ENGINE_HIGHLIGHT_TILES;
  }
}

void BlenderSession::reset_session(blender::Main &b_data, blender::Depsgraph &b_depsgraph)
{
  /* Update data, scene and depsgraph pointers. These can change after undo. */
  this->b_data = &b_data;
  this->b_depsgraph = &b_depsgraph;
  this->b_scene = DEG_get_evaluated_scene(&b_depsgraph);
  if (sync) {
    sync->reset(*this->b_data, *this->b_scene);
  }

  if (preview_osl) {
    blender::PointerRNA scene_rna_ptr = RNA_id_pointer_create(&b_scene->id);
    blender::PointerRNA cscene = RNA_pointer_get(&scene_rna_ptr, "cycles");
    RNA_boolean_set(&cscene, "shading_system", preview_osl);
  }

  if (b_v3d) {
    this->b_render = &b_scene->r;
  }
  else {
    this->b_render = RE_engine_get_render_data(b_engine.re);
    width = render_resolution_x(*b_render);
    height = render_resolution_y(*b_render);
  }

  const bool is_new_session = (session == nullptr);
  if (is_new_session) {
    /* Initialize session and remember it was just created so not to
     * re-create it below.
     */
    create_session();
  }

  if (b_v3d) {
    /* NOTE: We need to create session, but all the code from below
     * will make viewport render to stuck on initialization.
     */
    return;
  }

  const SessionParams session_params = BlenderSync::get_session_params(
      b_engine, b_userpref, *b_scene, background, pixelsize);
  const SceneParams scene_params = BlenderSync::get_scene_params(
      b_userpref, b_data, *b_scene, background, use_developer_ui);

  if (scene->params.modified(scene_params) || session->params.modified(session_params) ||
      (this->b_render->mode & blender::R_PERSISTENT_DATA) == 0)
  {
    /* if scene or session parameters changed, it's easier to simply re-create
     * them rather than trying to distinguish which settings need to be updated
     */
    if (!is_new_session) {
      free_session();
      create_session();
    }
    return;
  }

  session->progress.reset();

  /* peak memory usage should show current render peak, not peak for all renders
   * made by this render session
   */
  session->stats.mem_peak = session->stats.mem_used;

  if (is_new_session) {
    /* Sync object should be re-created for new scene. */
    sync = make_unique<BlenderSync>(
        b_engine, b_data, *b_scene, scene, !background, use_developer_ui, session->progress);
  }
  else {
    /* Sync recalculations to do just the required updates. */
    sync->sync_recalc(b_depsgraph, b_screen, b_v3d, b_rv3d);
  }

  sync->sync_camera(*b_render, width, height, "", false);

  const BufferParams buffer_params = BlenderSync::get_buffer_params(
      nullptr, nullptr, b_scene, scene->camera, width, height, false);
  session->reset(session_params, buffer_params);

  /* reset time */
  start_resize_time = 0.0;

  {
    const thread_scoped_lock lock(draw_state_.mutex);
    draw_state_.last_pass_index = -1;
  }
}

void BlenderSession::free_session()
{
  if (session) {
    session->cancel(true);
  }

  sync.reset();
  session.reset();

  display_driver_ = nullptr;
}

void BlenderSession::full_buffer_written(string_view filename)
{
  full_buffer_files_.emplace_back(filename);
}

static void add_cryptomatte_layer(blender::RenderResult &b_rr, string name, string manifest)
{
  const string identifier = string_printf("%08x",
                                          util_murmur_hash3(name.c_str(), name.length(), 0));
  const string prefix = "cryptomatte/" + identifier.substr(0, 7) + "/";

  render_add_metadata(b_rr, prefix + "name", name);
  render_add_metadata(b_rr, prefix + "hash", "MurmurHash3_32");
  render_add_metadata(b_rr, prefix + "conversion", "uint32_to_float32");
  render_add_metadata(b_rr, prefix + "manifest", manifest);
}

void BlenderSession::stamp_view_layer_metadata(Scene *scene, const string &view_layer_name)
{
  blender::RenderResult *b_rr = RE_engine_get_result(&b_engine);
  const string prefix = "cycles." + view_layer_name + ".";

  /* Configured number of samples for the view layer. */
  BKE_render_result_stamp_data(
      b_rr, (prefix + "samples").c_str(), to_string(session->params.samples).c_str());

  /* Store ranged samples information. */
  /* TODO(sergey): Need to bring this information back. */
#if 0
  if (session->tile_manager.range_num_samples != -1) {
    b_rr.stamp_data_add_field((prefix + "range_start_sample").c_str(),
                              to_string(session->tile_manager.range_start_sample).c_str());
    b_rr.stamp_data_add_field((prefix + "range_num_samples").c_str(),
                              to_string(session->tile_manager.range_num_samples).c_str());
  }
#endif

  /* Write cryptomatte metadata. */
  if (scene->film->get_cryptomatte_passes() & CRYPT_OBJECT) {
    add_cryptomatte_layer(*b_rr,
                          view_layer_name + ".CryptoObject",
                          scene->object_manager->get_cryptomatte_objects(scene));
  }
  if (scene->film->get_cryptomatte_passes() & CRYPT_MATERIAL) {
    add_cryptomatte_layer(*b_rr,
                          view_layer_name + ".CryptoMaterial",
                          scene->shader_manager->get_cryptomatte_materials(scene));
  }
  if (scene->film->get_cryptomatte_passes() & CRYPT_ASSET) {
    add_cryptomatte_layer(*b_rr,
                          view_layer_name + ".CryptoAsset",
                          scene->object_manager->get_cryptomatte_assets(scene));
  }

  /* Store synchronization and bare-render times. */
  double total_time;
  double render_time;
  session->progress.get_time(total_time, render_time);
  BKE_render_result_stamp_data(
      b_rr, (prefix + "total_time").c_str(), time_human_readable_from_seconds(total_time).c_str());
  BKE_render_result_stamp_data(b_rr,
                               (prefix + "render_time").c_str(),
                               time_human_readable_from_seconds(render_time).c_str());
  BKE_render_result_stamp_data(b_rr,
                               (prefix + "synchronization_time").c_str(),
                               time_human_readable_from_seconds(total_time - render_time).c_str());
}

void BlenderSession::render(blender::Depsgraph &b_depsgraph_)
{
  b_depsgraph = &b_depsgraph_;
  direct_deep_without_compositor_ = false;
  skip_full_buffer_readback_for_background_direct_deep_ = false;

  if (session->progress.get_cancel()) {
    update_status_progress();
    return;
  }

  /* Create driver to write out render results. */
  ensure_display_driver_if_needed();
  auto output_driver = make_unique<BlenderOutputDriver>(b_engine);
  blender_output_driver = output_driver.get();  /* Store for deep recolor access. */
  session->set_output_driver(std::move(output_driver));

  /* Note: Deep output driver setup is done AFTER sync_data() to ensure
   * kernel pointers are properly synced. See the setup block around line 414. */

  session->full_buffer_written_cb = [&](string_view filename) { full_buffer_written(filename); };

  blender::ViewLayer &b_view_layer = *DEG_get_evaluated_view_layer(b_depsgraph);

  /* get buffer parameters */
  const SessionParams session_params = BlenderSync::get_session_params(
      b_engine, b_userpref, *b_scene, background, pixelsize);

  /* temporary render result to find needed passes and views */
  blender::RenderResult *b_rr = RE_engine_begin_result(
      &b_engine, 0, 0, 1, 1, b_view_layer.name, nullptr);
  blender::RenderLayer *b_rlay = static_cast<blender::RenderLayer *>(b_rr->layers.first);

  {
    const thread_scoped_lock lock(draw_state_.mutex);
    b_rlay_name = b_view_layer.name;

    /* Signal that the display pass is to be updated. */
    draw_state_.last_pass_index = -1;
  }

  /* Compute render passes and film settings. */
  sync->sync_render_passes(*b_rlay, b_view_layer);

  const int num_views = b_rr->views.count();
  const bool is_multi_view = (num_views > 1);
  bool use_overscan = offline_render_needs_overscan(b_scene, b_render);
  if (use_overscan && is_multi_view) {
    RE_engine_report(
        &b_engine, blender::RPT_WARNING, "EXR overscan is not supported with multi-view rendering");
    use_overscan = false;
  }

  BufferParams buffer_params = BlenderSync::get_buffer_params(
      nullptr, nullptr, b_scene, scene->camera, width, height, use_overscan);
  render_result_resize_for_overscan(RE_engine_get_result(&b_engine), buffer_params);
  session->reset(session_params, buffer_params);

  blender::Scene *input_scene = DEG_get_input_scene(b_depsgraph);
  blender::Scene *evaluated_scene = DEG_get_evaluated_scene(b_depsgraph);
  const BlenderDeepOutputRequirements deep_output = BlenderSync::get_deep_output_requirements(
      *input_scene, *evaluated_scene, false);
  const bool compositor_needs_deep = deep_output.compositor;
  const bool is_deep_exr_format = deep_output.direct;
  const bool direct_deep_without_compositor = is_deep_exr_format &&
                                              !compositor_needs_deep;
  const bool need_deep_output = deep_output.needed();
  direct_deep_without_compositor_ = direct_deep_without_compositor;
  skip_full_buffer_readback_for_background_direct_deep_ = background &&
                                                          direct_deep_without_compositor &&
                                                          !is_multi_view;
  std::string direct_deep_filepath;
  if (is_deep_exr_format) {
    char deep_filepath_cstr[FILE_MAX];
    if (render_output_filepath_for_still(b_data, evaluated_scene, b_render, deep_filepath_cstr)) {
      direct_deep_filepath = deep_filepath_cstr;
    }
    else {
      direct_deep_filepath = std::string(b_render->pic);
      const size_t dot_pos = direct_deep_filepath.rfind('.');
      if (dot_pos == std::string::npos || direct_deep_filepath.substr(dot_pos) != ".exr") {
        if (dot_pos != std::string::npos) {
          direct_deep_filepath = direct_deep_filepath.substr(0, dot_pos);
        }
        direct_deep_filepath += ".exr";
      }
    }
  }
  bool deep_output_blocked = false;
  bool deep_output_error_reported = false;

  for (const auto [view_index, b_view] : b_rr->views.enumerate()) {
    b_rview_name = b_view.name;

    buffer_params.layer = b_view_layer.name;
    buffer_params.view = b_rview_name;

    /* set the current view */
    RE_engine_active_view_set(&b_engine, b_rview_name.c_str());

    /* Force update in this case, since the camera transform on each frame changes
     * in different views. This could be optimized by somehow storing the animated
     * camera transforms separate from the fixed stereo transform. */
    if ((scene->need_motion() != Scene::MOTION_NONE) && view_index > 0) {
      sync->tag_update();
    }

    /* update scene */
    sync->sync_camera(*b_render, width, height, b_rview_name.c_str(), use_overscan);
    sync->sync_data(*b_render,
                    *b_depsgraph,
                    b_screen,
                    b_v3d,
                    b_rv3d,
                    width,
                    height,
                    &python_thread_state,
                    session_params.denoise_device);

    /* Create the Deep output driver after Film synchronization has applied output demand. */

    if (is_multi_view && need_deep_output) {
      if (!deep_output_error_reported) {
        RE_engine_report(
            &b_engine,
            blender::RPT_WARNING,
            "Deep EXR output is not supported with multi-view rendering");
        deep_output_error_reported = true;
      }
      deep_output_blocked = true;
    }

    if (need_deep_output && !deep_output_blocked) {
      DeepOutputDriver *deep_driver = session->get_deep_output_driver();
      if (!deep_driver) {
        auto new_driver = make_unique<DeepOutputDriver>(session->device.get());
        new_driver->set_enabled(true);

        /* Set callback for deep EXR writing. */
        new_driver->set_write_callback(
            [](const std::vector<std::vector<blender::DeepSample>> &deep_data,
               int w,
               int h,
               const std::string &filepath,
               int compression,
               bool use_half_float,
               float deep_merge_tolerance,
               float deep_alpha_merge_tolerance,
               bool has_display_window,
               int display_width,
               int display_height,
               int display_offset_x,
               int display_offset_y,
               int data_offset_x,
               int data_offset_y) -> bool {
              return blender::IMB_exr_save_deep(
                  deep_data,
                  w,
                  h,
                  filepath.c_str(),
                  compression,
                  use_half_float,
                  false,
                  has_display_window,
                  display_width,
                  display_height,
                  display_offset_x,
                  display_offset_y,
                  data_offset_x,
                  data_offset_y,
                  deep_merge_tolerance,
                  deep_alpha_merge_tolerance);
            });

        session->set_deep_output_driver(std::move(new_driver));
        deep_driver = session->get_deep_output_driver();
      }

      if (deep_driver) {
        const auto &image_format = evaluated_scene ? evaluated_scene->r.im_format :
                                                     b_render->im_format;
        constexpr float default_deep_merge_tolerance = 0.01f;
        float depth_merge_tolerance = image_format.deep_merge_tolerance;
        const float alpha_merge_tolerance = image_format.deep_alpha_merge_tolerance;
        if (depth_merge_tolerance <= 0.0f && alpha_merge_tolerance > 0.0f) {
          depth_merge_tolerance = default_deep_merge_tolerance;
        }
        deep_driver->set_merge_threshold(depth_merge_tolerance);
        deep_driver->set_alpha_merge_threshold(alpha_merge_tolerance);
        deep_driver->set_compression(image_format.exr_codec);
        deep_driver->set_use_half_float(image_format.depth == blender::R_IMF_CHAN_DEPTH_16);
        const RenderDisplayWindow display_window = render_display_window_from_buffer_params(
            buffer_params);
        deep_driver->set_display_window(display_window.has_display_window,
                                        display_window.display_width,
                                        display_window.display_height,
                                        display_window.display_offset_x,
                                        display_window.display_offset_y,
                                        display_window.data_offset_x,
                                        display_window.data_offset_y);

        const int requested_deep_samples = scene->film->get_deep_max_samples();
        const int max_deep_samples = deep_effective_max_samples(scene->film, scene->integrator);
        if (scene->integrator && scene->integrator->get_volume_ray_marching() &&
            max_deep_samples > requested_deep_samples)
        {
          LOG_INFO << "Deep EXR: increasing max samples from " << requested_deep_samples << " to "
                   << max_deep_samples << " for ray-marched volumes";
        }

        const int deep_width = buffer_params.width;
        const int deep_height = buffer_params.height;
        const bool needs_reset = (deep_driver->get_width() != deep_width ||
                                  deep_driver->get_height() != deep_height ||
                                  deep_driver->get_max_samples_per_pixel() != max_deep_samples);
        if (needs_reset) {
          deep_driver->reset(deep_width, deep_height, max_deep_samples);
        }
        else {
          deep_driver->clear_device_buffers();
        }
      }
    }

    /* At the moment we only free if we are not doing multi-view
     * (or if we are rendering the last view). See #58142/D4239 for discussion.
     */
    const bool can_free_cache = (view_index == num_views - 1);
    if (can_free_cache) {
      sync->free_data_after_sync(*b_depsgraph);
    }

    builtin_images_load();

    /* Attempt to free all data which is held by Blender side, since at this
     * point we know that we've got everything to render current view layer.
     */
    if (can_free_cache) {
      free_blender_memory_if_possible();
    }

    /* Make sure all views have different noise patterns. - hardcoded value just to make it
     * random
     */
    if (view_index != 0) {
      int seed = scene->integrator->get_seed();
      seed += hash_uint2(seed, hash_uint2(view_index * 0xdeadbeef, 0));
      scene->integrator->set_seed(seed);
    }

    /* Update number of samples per layer. */
    const int samples = sync->get_layer_samples();
    const bool bound_samples = sync->get_layer_bound_samples();

    SessionParams effective_session_params = session_params;
    if (samples != 0 && (!bound_samples || (samples < session_params.samples))) {
      effective_session_params.samples = samples;
    }

    /* Update session itself. */
    session->reset(effective_session_params, buffer_params);

    /* render */
    if ((b_engine.flag & blender::RE_ENGINE_PREVIEW) == 0 && background && print_render_stats) {
      scene->enable_update_stats();
    }

    session->start();
    session->wait();

    if ((b_engine.flag & blender::RE_ENGINE_PREVIEW) == 0 && background && print_render_stats) {
      RenderStats stats;
      session->collect_statistics(&stats);
      printf("Render statistics:\n%s\n", stats.full_report().c_str());
    }

    if (session->progress.get_cancel()) {
      break;
    }
  }

  /* Finalize deep output if enabled or Deep EXR format selected - write deep EXR file.
   * Also finalize for compositor Deep EXR File Output nodes. */
  const bool finalize_deep = (is_deep_exr_format || compositor_needs_deep) && !deep_output_blocked;
  if (finalize_deep && !session->progress.get_cancel()) {
    DeepOutputDriver *deep_driver = session->get_deep_output_driver();
    if (deep_driver && deep_driver->is_enabled()) {
      const float *combined_data = nullptr;
      int combined_w = 0;
      int combined_h = 0;
      const float *sample_count_data = nullptr;
      int sample_count_w = 0;
      int sample_count_h = 0;

      /* Get beauty (Combined pass) for deep recolor.
       * Prefer output driver capture, fallback to RenderResult. */
      vector<float> combined_local;
      vector<float> sample_count_local;
      if (blender_output_driver) {
        combined_data = blender_output_driver->get_combined_pass(combined_w, combined_h);
        sample_count_data = blender_output_driver->get_sample_count_pass(sample_count_w,
                                                                         sample_count_h);
      }
      blender::RenderResult *beauty_result = RE_engine_get_result(&b_engine);
      if ((!combined_data || !sample_count_data) && beauty_result) {
        blender::RenderLayer *beauty_layer = RE_GetRenderLayer(beauty_result, b_rlay_name.c_str());
        if (!beauty_layer) {
          beauty_layer = static_cast<blender::RenderLayer *>(beauty_result->layers.first);
        }
        if (beauty_layer) {
          for (blender::RenderPass *b_pass = static_cast<blender::RenderPass *>(
                   beauty_layer->passes.first);
               b_pass;
               b_pass = b_pass->next)
          {
            if (b_pass->name && b_pass->ibuf && b_pass->ibuf->float_buffer.data) {
              const int w = b_pass->rectx;
              const int h = b_pass->recty;
              if (w <= 0 || h <= 0) {
                continue;
              }

              if (!combined_data && strcmp(b_pass->name, "Combined") == 0 && b_pass->channels == 4)
              {
                const size_t size = static_cast<size_t>(w) * h * 4;
                combined_local.assign(b_pass->ibuf->float_buffer.data,
                                      b_pass->ibuf->float_buffer.data + size);
                combined_data = combined_local.data();
                combined_w = w;
                combined_h = h;
              }
              else if (!sample_count_data && strcmp(b_pass->name, "Debug Sample Count") == 0 &&
                       b_pass->channels == 1)
              {
                const size_t size = static_cast<size_t>(w) * h;
                sample_count_local.assign(b_pass->ibuf->float_buffer.data,
                                          b_pass->ibuf->float_buffer.data + size);
                sample_count_data = sample_count_local.data();
                sample_count_w = w;
                sample_count_h = h;
              }
            }
          }
        }

        if (!combined_data && beauty_result->views.first) {
          blender::RenderView *beauty_view = static_cast<blender::RenderView *>(
              beauty_result->views.first);
          if (beauty_view->ibuf && beauty_view->ibuf->float_buffer.data) {
            const int w = beauty_view->ibuf->x;
            const int h = beauty_view->ibuf->y;
            if (w > 0 && h > 0) {
              const size_t size = static_cast<size_t>(w) * h * 4;
              combined_local.assign(beauty_view->ibuf->float_buffer.data,
                                    beauty_view->ibuf->float_buffer.data + size);
              combined_data = combined_local.data();
              combined_w = w;
              combined_h = h;
            }
          }
        }
      }

      if (combined_data) {
        deep_driver->set_beauty_buffer(combined_data, combined_w, combined_h);
      }
      /* Blender's Debug Sample Count pass is normalized by the active render sample limit.
       * Convert it back to absolute per-pixel counts before deep edge reconstruction. */
      const float sample_count_scale = max(float(session->params.samples), 1.0f);
      if (sample_count_data) {
        deep_driver->set_sample_count_buffer(
            sample_count_data, sample_count_w, sample_count_h, sample_count_scale);
      }

      if (is_deep_exr_format) {
        deep_driver->finalize_deep_output(direct_deep_filepath);
        if (!compositor_needs_deep) {
          /* Direct-only Deep EXR no longer needs the large processed cache or resolved beauty /
           * sample-count host buffers after the file has been written. Release them here while
           * the driver object itself still stays alive for the normal session teardown. This
           * mirrors the compositor lifetime fix without destroying the driver mid-frame. */
          deep_driver->release_temporary_host_caches();
        }
      }

      const bool render_result_needs_processed_deep = compositor_needs_deep;
      if (render_result_needs_processed_deep) {
        /* Store processed deep data in RenderResult.
         *
         * The compositor needs access to deep data via RenderResult.deep_data.
         * Direct scene-output Deep EXR is written directly by DeepOutputDriver. */
        blender::RenderResult *render_result = RE_engine_get_result(&b_engine);
        if (render_result) {
          unique_ptr<std::vector<std::vector<blender::DeepSample>>> processed_data(
              deep_driver->get_unmerged_processed_deep_data());
          if (processed_data) {
            unique_ptr<blender::RenderDeepData> converted_data =
                make_unique<blender::RenderDeepData>();
            converted_data->pixels = std::move(*processed_data);

            blender::RenderLayer *deep_layer = RE_GetRenderLayer(render_result, b_rlay_name.c_str());
            if (!deep_layer) {
              deep_layer = static_cast<blender::RenderLayer *>(render_result->layers.first);
            }

            if (deep_layer) {
              if (deep_layer->deep_data && deep_layer->deep_data_owned) {
                delete deep_layer->deep_data;
              }
              deep_layer->deep_data = converted_data.release();
              deep_layer->deep_width = deep_driver->get_width();
              deep_layer->deep_height = deep_driver->get_height();
              deep_layer->deep_data_owned = true;

              if (render_result->deep_data && render_result->deep_data_owned) {
                delete render_result->deep_data;
              }
              render_result->deep_data = deep_layer->deep_data;
              render_result->deep_width = deep_layer->deep_width;
              render_result->deep_height = deep_layer->deep_height;
              render_result->deep_data_owned = false;
            }
          }
        }
      }

      if (compositor_needs_deep) {
        /* Scene-compositing renders still need the driver object to survive until compositor
         * execution is finished, but once the deep payload has been copied into Blender-owned
         * RenderResult storage we can release the large temporary host caches. Keep direct-only
         * Deep EXR on the older path for now; its lifetime still needs a separate pass. */
        deep_driver->release_temporary_host_caches();
      }
    }
  }

  const bool apply_render_result_display_window = use_overscan &&
                                                  !(is_deep_exr_format && !compositor_needs_deep);
  if (apply_render_result_display_window) {
    render_result_apply_display_window(RE_engine_get_result(&b_engine), buffer_params);
  }

  /* add metadata */
  stamp_view_layer_metadata(scene, b_rlay_name);

  /* free result without merging */
  RE_engine_end_result(&b_engine, b_rr, true, false, false);

  /* When tiled rendering is used there will be no "write" done for the tile. Forcefully clear
   * highlighted tiles now, so that the highlight will be removed while processing full frame
   * from file. */
  RE_engine_tile_highlight_clear_all(&b_engine);

  double total_time;
  double render_time;
  session->progress.get_time(total_time, render_time);
  LOG_INFO << "Total render time: " << total_time;
  LOG_INFO << "Render time (without synchronization): " << render_time;
}

void BlenderSession::render_frame_finish()
{
  /* Processing of all layers and views is done. Clear the strings so that we can communicate
   * progress about reading files and denoising them. */
  b_rlay_name = "";
  b_rview_name = "";

  if ((b_render->mode & blender::R_PERSISTENT_DATA) == 0) {
    /* Free the sync object so that it can properly dereference nodes from the scene graph before
     * the graph is freed. */
    sync.reset();

    session->device_free();
  }

  const bool skip_full_buffer_readback = skip_full_buffer_readback_for_background_direct_deep_;
  if (!skip_full_buffer_readback) {
    for (const string_view filename : full_buffer_files_) {
      session->process_full_buffer_from_disk(filename);
      if (check_and_report_session_error()) {
        break;
      }
    }
  }

  for (const string_view filename : full_buffer_files_) {
    path_remove(filename);
  }

  /* Clear output driver. */
  session->set_output_driver(nullptr);
  session->full_buffer_written_cb = nullptr;

  /* The display driver is the source of drawing context for both drawing and possible graphics
   * interoperability objects in the path trace. Once the frame is finished the OpenGL context
   * might be freed form Blender side. Need to ensure that all GPU resources are freed prior to
   * that point.
   * Ideally would only do this when OpenGL context is actually destroyed, but there is no way to
   * know when this happens (at least in the code at the time when this comment was written).
   * The penalty of re-creating resources on every frame is unlikely to be noticed. */
  display_driver_ = nullptr;
  session->set_display_driver(nullptr);

  /* All the files are handled.
   * Clear the list so that this session can be re-used by Persistent Data. */
  full_buffer_files_.clear();
}

static bool bake_setup_pass(Scene *scene, const string &bake_type, const int bake_filter)
{
  Integrator *integrator = scene->integrator;
  Film *film = scene->film;

  const bool filter_direct = (bake_filter & blender::R_BAKE_PASS_FILTER_DIRECT) != 0;
  const bool filter_indirect = (bake_filter & blender::R_BAKE_PASS_FILTER_INDIRECT) != 0;
  const bool filter_color = (bake_filter & blender::R_BAKE_PASS_FILTER_COLOR) != 0;

  PassType type = PASS_NONE;
  bool use_direct_light = false;
  bool use_indirect_light = false;
  bool include_albedo = false;

  /* Data passes. */
  if (bake_type == "POSITION") {
    type = PASS_POSITION;
  }
  else if (bake_type == "NORMAL") {
    type = PASS_NORMAL;
  }
  else if (bake_type == "UV") {
    type = PASS_UV;
  }
  else if (bake_type == "ROUGHNESS") {
    type = PASS_ROUGHNESS;
  }
  else if (bake_type == "EMIT") {
    type = PASS_EMISSION;
  }
  /* Environment pass. */
  else if (bake_type == "ENVIRONMENT") {
    type = PASS_BACKGROUND;
  }
  /* AO pass. */
  else if (bake_type == "AO") {
    type = PASS_AO;
  }
  /* Shadow pass. */
  else if (bake_type == "SHADOW") {
    /* Bake as combined pass, together with marking the object as a shadow catcher. */
    type = PASS_SHADOW_CATCHER;
    film->set_use_approximate_shadow_catcher(true);

    use_direct_light = true;
    use_indirect_light = true;
    include_albedo = true;

    integrator->set_use_diffuse(true);
    integrator->set_use_glossy(true);
    integrator->set_use_transmission(true);
    integrator->set_use_emission(true);
  }
  /* Combined pass. */
  else if (bake_type == "COMBINED") {
    type = PASS_COMBINED;
    film->set_use_approximate_shadow_catcher(true);

    use_direct_light = filter_direct;
    use_indirect_light = filter_indirect;
    include_albedo = filter_color;

    integrator->set_use_diffuse((bake_filter & blender::R_BAKE_PASS_FILTER_DIFFUSE) != 0);
    integrator->set_use_glossy((bake_filter & blender::R_BAKE_PASS_FILTER_GLOSSY) != 0);
    integrator->set_use_transmission((bake_filter & blender::R_BAKE_PASS_FILTER_TRANSM) != 0);
    integrator->set_use_emission((bake_filter & blender::R_BAKE_PASS_FILTER_EMIT) != 0);
  }
  /* Light component passes. */
  else if ((bake_type == "DIFFUSE") || (bake_type == "GLOSSY") || (bake_type == "TRANSMISSION")) {
    use_direct_light = filter_direct;
    use_indirect_light = filter_indirect;
    include_albedo = filter_color;

    integrator->set_use_diffuse(bake_type == "DIFFUSE");
    integrator->set_use_glossy(bake_type == "GLOSSY");
    integrator->set_use_transmission(bake_type == "TRANSMISSION");

    if (bake_type == "DIFFUSE") {
      if (filter_direct && filter_indirect) {
        type = PASS_DIFFUSE;
      }
      else if (filter_direct) {
        type = PASS_DIFFUSE_DIRECT;
      }
      else if (filter_indirect) {
        type = PASS_DIFFUSE_INDIRECT;
      }
      else {
        type = PASS_DIFFUSE_COLOR;
      }
    }
    else if (bake_type == "GLOSSY") {
      if (filter_direct && filter_indirect) {
        type = PASS_GLOSSY;
      }
      else if (filter_direct) {
        type = PASS_GLOSSY_DIRECT;
      }
      else if (filter_indirect) {
        type = PASS_GLOSSY_INDIRECT;
      }
      else {
        type = PASS_GLOSSY_COLOR;
      }
    }
    else if (bake_type == "TRANSMISSION") {
      if (filter_direct && filter_indirect) {
        type = PASS_TRANSMISSION;
      }
      else if (filter_direct) {
        type = PASS_TRANSMISSION_DIRECT;
      }
      else if (filter_indirect) {
        type = PASS_TRANSMISSION_INDIRECT;
      }
      else {
        type = PASS_TRANSMISSION_COLOR;
      }
    }
  }

  if (type == PASS_NONE) {
    return false;
  }

  /* Create pass. */
  Pass *pass = scene->create_node<Pass>();
  pass->set_name(ustring("Combined"));
  pass->set_type(type);
  pass->set_include_albedo(include_albedo);

  /* Disable direct indirect light for performance when not needed. */
  integrator->set_use_direct_light(use_direct_light);
  integrator->set_use_indirect_light(use_indirect_light);

  /* Disable denoiser if the pass does not support it.
   * For the passes which support denoising follow the user configuration. */
  const PassInfo pass_info = Pass::get_info(type);
  if (integrator->get_use_denoise() && !pass_info.support_denoise) {
    integrator->set_use_denoise(false);
  }

  return true;
}

void BlenderSession::bake(blender::Depsgraph &b_depsgraph_,
                          blender::Object &b_object,
                          const string &bake_type,
                          const int bake_filter,
                          const int bake_width,
                          const int bake_height)
{
  b_depsgraph = &b_depsgraph_;

  /* Get session parameters. */
  const SessionParams session_params = BlenderSync::get_session_params(
      b_engine, b_userpref, *b_scene, background, pixelsize);

  /* Initialize bake manager, before we load the baking kernels. */
  scene->bake_manager->set_baking(scene, true);

  session->set_display_driver(nullptr);
  session->set_output_driver(make_unique<BlenderOutputDriver>(b_engine));
  session->full_buffer_written_cb = [&](string_view filename) { full_buffer_written(filename); };

  /* Sync scene. */
  sync->set_bake_target(b_object);
  sync->sync_camera(*b_render, width, height, "", false);
  sync->sync_data(*b_render,
                  *b_depsgraph,
                  b_screen,
                  b_v3d,
                  b_rv3d,
                  width,
                  height,
                  &python_thread_state,
                  session_params.denoise_device);

  /* Save the current state of the denoiser, as it might be disabled by the pass configuration
   * (for passed which do not support denoising). */
  Integrator *integrator = scene->integrator;
  const bool was_denoiser_enabled = integrator->get_use_denoise();

  /* Add render pass that we want to bake, and name it Combined so that it is
   * used as that on the Blender side. */
  if (!bake_setup_pass(scene, bake_type, bake_filter)) {
    session->cancel(true);
  }

  /* Always use transparent background for baking. */
  scene->background->set_transparent(true);

  if (!session->progress.get_cancel()) {
    /* Load built-in images from Blender. */
    builtin_images_load();
  }

  /* Object might have been disabled for rendering or excluded in some
   * other way, in that case Blender will report a warning afterwards. */
  Object *bake_object = nullptr;
  if (!session->progress.get_cancel()) {
    for (Object *ob : scene->objects) {
      if (ob->get_is_bake_target()) {
        bake_object = ob;
        break;
      }
    }
  }

  /* For the shadow pass, temporarily mark the object as a shadow catcher. */
  const bool was_shadow_catcher = (bake_object) ? bake_object->get_is_shadow_catcher() : false;
  if (bake_object && bake_type == "SHADOW") {
    bake_object->set_is_shadow_catcher(true);
  }

  if (bake_object && !session->progress.get_cancel()) {
    /* Get buffer parameters. */
    BufferParams buffer_params;
    buffer_params.width = bake_width;
    buffer_params.height = bake_height;
    buffer_params.window_width = bake_width;
    buffer_params.window_height = bake_height;
    /* Unique layer name for multi-image baking. */
    buffer_params.layer = string_printf("bake_%d\n", bake_id++);

    /* Update session. */
    session->reset(session_params, buffer_params);

    session->progress.set_update_callback([this] { update_bake_progress(); });
  }

  /* Perform bake. Check cancel to avoid crash with incomplete scene data. */
  if (bake_object && !session->progress.get_cancel()) {
    session->start();
    session->wait();
  }

  /* Restore object state. */
  if (bake_object) {
    bake_object->set_is_shadow_catcher(was_shadow_catcher);
  }

  /* Restore the state of denoiser to before it was possibly disabled by the pass, so that the
   * next baking pass can use the original value. */
  integrator->set_use_denoise(was_denoiser_enabled);
}

void BlenderSession::synchronize(blender::Depsgraph &b_depsgraph_)
{
  /* only used for viewport render */
  if (!b_v3d) {
    return;
  }

  /* on session/scene parameter changes, we recreate session entirely */
  const SessionParams session_params = BlenderSync::get_session_params(
      b_engine, b_userpref, *b_scene, background, pixelsize);
  const SceneParams scene_params = BlenderSync::get_scene_params(
      b_userpref, *b_data, *b_scene, background, use_developer_ui);
  const bool session_pause = BlenderSync::get_session_pause(*b_scene, background);

  if (session->params.modified(session_params) || scene->params.modified(scene_params)) {
    free_session();
    create_session();
  }

  ensure_display_driver_if_needed();

  /* increase samples and render time, but never decrease */
  session->set_samples(session_params.samples);
  session->set_time_limit(session_params.time_limit);
  session->set_pause(session_pause);

  /* copy recalc flags, outside of mutex so we can decide to do the real
   * synchronization at a later time to not block on running updates */
  sync->sync_recalc(b_depsgraph_, b_screen, b_v3d, b_rv3d);

  /* don't do synchronization if on pause */
  if (session_pause) {
    tag_update();
    return;
  }

  /* try to acquire mutex. if we don't want to or can't, come back later */
  if (!session->ready_to_reset() || !session->scene->mutex.try_lock()) {
    tag_update();
    return;
  }

  /* data and camera synchronize */
  b_depsgraph = &b_depsgraph_;

  sync->sync_data(*b_render,
                  *b_depsgraph,
                  b_screen,
                  b_v3d,
                  b_rv3d,
                  width,
                  height,
                  &python_thread_state,
                  session_params.denoise_device);

  if (b_rv3d) {
    const bool use_viewport_overscan = viewport_allows_overscan(b_scene, b_v3d, b_rv3d);
    sync->sync_view(b_v3d, b_rv3d, width, height, use_viewport_overscan);
  }
  else {
    sync->sync_camera(*b_render, width, height, "", false);
  }

  /* get buffer parameters */
  const BufferParams buffer_params = BlenderSync::get_buffer_params(
      b_v3d,
      b_rv3d,
      b_scene,
      scene->camera,
      width,
      height,
      viewport_allows_overscan(b_scene, b_v3d, b_rv3d));

  /* reset if needed */
  if (scene->need_reset()) {
    session->reset(session_params, buffer_params);

    /* After session reset, so device is not accessing image data anymore. */
    builtin_images_load();

    /* reset time */
    start_resize_time = 0.0;
  }

  /* unlock */
  session->scene->mutex.unlock();

  /* Start rendering thread, if it's not running already. Do this
   * after all scene data has been synced at least once. */
  session->start();
}

void BlenderSession::draw(blender::bScreen &b_screen, blender::SpaceImage &space_image)
{
  if (!session || !session->scene) {
    /* Offline render drawing does not force the render engine update, which means it's possible
     * that the Session is not created yet. */
    return;
  }

  const thread_scoped_lock lock(draw_state_.mutex);

  const int pass_index = space_image.iuser.pass;
  if (pass_index != draw_state_.last_pass_index) {
    blender::RenderPass *b_display_pass = RE_engine_pass_by_index_get(
        &b_engine, b_rlay_name.c_str(), pass_index);
    if (!b_display_pass) {
      return;
    }

    Scene *scene = session->scene.get();

    const thread_scoped_lock lock(scene->mutex);

    const Pass *pass = Pass::find(scene->passes, b_display_pass->name);
    if (!pass) {
      return;
    }

    scene->film->set_display_pass(pass->get_type());

    draw_state_.last_pass_index = pass_index;
  }

  if (display_driver_) {
    blender::PointerRNA space_image_rna_ptr = RNA_pointer_create_id_subdata(
        b_screen.id, blender::RNA_SpaceImageEditor, &space_image);
    float zoom[2];
    RNA_float_get_array(&space_image_rna_ptr, "zoom", zoom);
    display_driver_->set_zoom(zoom[0], zoom[1]);
  }

  session->draw();
}

void BlenderSession::view_draw(const int w, const int h)
{
  /* pause in redraw in case update is not being called due to final render */
  session->set_pause(BlenderSync::get_session_pause(*b_scene, background));

  /* Update navigating state. */
  const bool dimensions_changed = (width != w || height != h || pixelsize != blender::U.pixelsize);
  const bool is_navigating = region_view3d_navigating_or_transforming(b_rv3d) ||
                             dimensions_changed;
  session->set_navigating(is_navigating);

  /* before drawing, we verify camera and viewport size changes, because
   * we do not get update callbacks for those, we must detect them here */
  if (session->ready_to_reset()) {
    bool reset = false;

    /* If dimensions changed, reset. We need to check pixel size here because
     * it's only valid during drawing, as it can change per window. */
    if (dimensions_changed) {
      if (start_resize_time == 0.0) {
        /* don't react immediately to resizes to avoid flickery resizing
         * of the viewport, and some window managers changing the window
         * size temporarily on unminimize */
        start_resize_time = time_dt();
        tag_redraw();
      }
      else if (time_dt() - start_resize_time < 0.2) {
        tag_redraw();
      }
      else {
        width = w;
        height = h;
        pixelsize = blender::U.pixelsize;
        reset = true;
      }
    }

    /* try to acquire mutex. if we can't, come back later */
    if (!session->scene->mutex.try_lock()) {
      tag_update();
    }
    else {
      /* update camera from 3d view */

      const bool use_viewport_overscan = viewport_allows_overscan(b_scene, b_v3d, b_rv3d);
      sync->sync_view(b_v3d, b_rv3d, width, height, use_viewport_overscan);

      if (scene->camera->is_modified()) {
        reset = true;
      }

      session->scene->mutex.unlock();
    }

    /* reset if requested */
    if (reset) {
      const SessionParams session_params = BlenderSync::get_session_params(
          b_engine, b_userpref, *b_scene, background, pixelsize);
      const BufferParams buffer_params = BlenderSync::get_buffer_params(
          b_v3d,
          b_rv3d,
          b_scene,
          scene->camera,
          width,
          height,
          viewport_allows_overscan(b_scene, b_v3d, b_rv3d));
      const bool session_pause = BlenderSync::get_session_pause(*b_scene, background);

      if (session_pause == false) {
        session->reset(session_params, buffer_params);
        start_resize_time = 0.0;
      }
    }
  }
  else {
    tag_update();
  }

  /* update status and progress for 3d view draw */
  update_status_progress();

  /* draw */
  session->draw();
}

void BlenderSession::get_status(string &status, string &substatus)
{
  session->progress.get_status(status, substatus);
}

void BlenderSession::get_progress(double &progress, double &total_time, double &render_time)
{
  session->progress.get_time(total_time, render_time);
  progress = session->progress.get_progress();
}

void BlenderSession::update_bake_progress()
{
  const double progress = session->progress.get_progress();

  if (progress != last_progress) {
    RE_engine_update_progress(&b_engine, (float)progress);
    last_progress = progress;
  }
}

void BlenderSession::update_status_progress()
{
  string timestatus;
  string status;
  string substatus;
  get_status(status, substatus);
  if (background && !substatus.empty()) {
    status += " | " + substatus;
  }

  double progress;
  double total_time;
  double render_time;
  get_progress(progress, total_time, render_time);

  const float mem_used = (float)session->stats.mem_used / 1024.0f / 1024.0f;
  const float mem_peak = (float)session->stats.mem_peak / 1024.0f / 1024.0f;
  if (background) {

    if (progress > 0) {
      const double remaining_time = session->get_estimated_remaining_time();
      if (remaining_time > 0) {
        timestatus = "Remaining: " + time_human_readable_from_seconds(remaining_time) + " | ";
      }
    }

    timestatus += string_printf("Mem: %dM | ", (int)ceilf(mem_used));
  }

  const double current_time = time_dt();
  /* When rendering in a window, redraw the status at least once per second to keep things
   * up to date. For headless rendering, only report when something significant changes to
   * keep the console output readable. */
  if (status != last_status || (!headless && (current_time - last_status_time) > 1.0)) {
    RE_engine_update_stats(&b_engine, "", (timestatus + status).c_str());
    RE_engine_update_memory_stats(&b_engine, mem_used, mem_peak);
    last_status = status;
    last_status_time = current_time;
  }
  if (progress != last_progress) {
    RE_engine_update_progress(&b_engine, (float)progress);
    last_progress = progress;
  }

  check_and_report_session_error();
}

bool BlenderSession::check_and_report_session_error()
{
  if (!session->progress.get_error()) {
    return false;
  }

  const string error = session->progress.get_error_message();
  if (error != last_error) {
    /* TODO(sergey): Currently C++ RNA API doesn't let us to use mnemonic name for the variable.
     * Would be nice to have this figured out.
     *
     * For until then, 1 << 5 means RPT_ERROR. */
    RE_engine_report(&b_engine, 1 << 5, error.c_str());
    RE_engine_set_error_message(&b_engine, error.c_str());
    last_error = error;
  }

  return true;
}

void BlenderSession::tag_update()
{
  /* tell blender that we want to get another update callback */
  b_engine.flag |= blender::RE_ENGINE_DO_UPDATE;
}

void BlenderSession::tag_redraw()
{
  if (background) {
    /* update stats and progress, only for background here because
     * in 3d view we do it in draw for thread safety reasons */
    update_status_progress();

    /* offline render, redraw if timeout passed */
    if (time_dt() - last_redraw_time > 1.0) {
      b_engine.flag |= blender::RE_ENGINE_DO_DRAW;
      last_redraw_time = time_dt();
    }
  }
  else {
    /* tell blender that we want to redraw */
    b_engine.flag |= blender::RE_ENGINE_DO_DRAW;
  }
}

void BlenderSession::test_cancel()
{
  /* test if we need to cancel rendering */
  if (background) {
    if (RE_engine_test_break(&b_engine)) {
      session->progress.set_cancel("Cancelled");
    }
  }
}

void BlenderSession::free_blender_memory_if_possible()
{
  if (!background) {
    /* During interactive render we can not free anything: attempts to save
     * memory would cause things to be allocated and evaluated for every
     * updated sample.
     */
    return;
  }
  RE_engine_free_blender_memory(&b_engine);
}

void BlenderSession::ensure_display_driver_if_needed()
{
  if (display_driver_) {
    /* Driver is already created. */
    return;
  }

  if (headless) {
    /* No display needed for headless. */
    return;
  }

  if ((b_engine.flag & blender::RE_ENGINE_PREVIEW) != 0) {
    /* TODO(sergey): Investigate whether DisplayDriver can be used for the preview as well. */
    return;
  }

  unique_ptr<BlenderDisplayDriver> display_driver = make_unique<BlenderDisplayDriver>(
      b_engine, *b_scene, b_rv3d, background);
  display_driver_ = display_driver.get();
  session->set_display_driver(std::move(display_driver));
}

CCL_NAMESPACE_END
