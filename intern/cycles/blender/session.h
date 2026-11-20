/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "device/deep_queue_memory.h"

#include <optional>

#include "device/device.h"
#include "session/deep_memory_budget.h"

#include "scene/scene.h"
#include "session/session.h"

#include "util/unique_ptr.h"
#include "util/vector.h"

namespace blender {
struct bScreen;
struct Depsgraph;
struct Main;
struct Object;
struct RegionView3D;
struct RenderData;
struct RenderEngine;
struct Scene;
struct SpaceImage;
struct UserDef;
struct View3D;
}  // namespace blender

CCL_NAMESPACE_BEGIN

struct RenderDisplayWindow {
  bool has_display_window = false;
  int display_width = 0;
  int display_height = 0;
  int display_offset_x = 0;
  int display_offset_y = 0;
  int data_offset_x = 0;
  int data_offset_y = 0;
};

class BlenderOutputDriver;
class BlenderDisplayDriver;
class BlenderSync;
class ImageMetaData;
class Scene;
class Session;

class BlenderDeepOutputAddresses;

struct BlenderDeepFrameBudgets {
  DeepQueueFrameReservation auxiliary;
  DeepMemoryBudget host, device;
  std::shared_ptr<BlenderDeepOutputAddresses> outputs;
  BlenderDeepFrameBudgets(const size_t host_quota, const size_t device_quota)
      : auxiliary(host_quota), host(auxiliary.payload_quota()), device(device_quota)
  {
    auxiliary.bind_payload_budget(host);
  }
};

class BlenderSession {
 public:
  BlenderSession(blender::RenderEngine &b_engine,
                 blender::UserDef &b_userpref,
                 blender::Main &b_data,
                 bool preview_osl);

  BlenderSession(blender::RenderEngine &b_engine,
                 blender::UserDef &b_userpref,
                 blender::Main &b_data,
                 blender::bScreen *b_screen,
                 blender::View3D *b_v3d,
                 blender::RegionView3D *b_rv3d,
                 const int width,
                 int height);

  ~BlenderSession();

  /* session */
  void create_session();
  void free_session();

  void reset_session(blender::Main &b_data, blender::Depsgraph &b_depsgraph);

  /* offline render */
  void render(blender::Depsgraph &b_depsgraph);

  void render_frame_finish();

  void bake(blender::Depsgraph &b_depsgraph_,
            blender::Object &b_object,
            const string &bake_type,
            const int bake_filter,
            const int bake_width,
            const int bake_height);

  void full_buffer_written(string_view filename);
  /* interactive updates */
  void synchronize(blender::Depsgraph &b_depsgraph);

  /* drawing */
  void draw(blender::bScreen &b_screen, blender::SpaceImage &space_image);
  void view_draw(const int w, const int h);
  void tag_redraw();
  void tag_update();
  void get_status(string &status, string &substatus);
  void get_progress(double &progress, double &total_time, double &render_time);
  void test_cancel();
  void update_status_progress();
  void update_bake_progress();

  bool background;
  unique_ptr<Session> session;
  Scene *scene;
  unique_ptr<BlenderSync> sync;
  double last_redraw_time;

  blender::RenderEngine &b_engine;
  blender::UserDef &b_userpref;
  blender::Main *b_data;
  blender::RenderData *b_render;
  blender::Depsgraph *b_depsgraph;
  /* NOTE: Blender's scene might become invalid after call
   * #free_blender_memory_if_possible(). */
  blender::Scene *b_scene;
  blender::bScreen *b_screen;
  blender::View3D *b_v3d;
  blender::RegionView3D *b_rv3d;
  string b_rlay_name;
  string b_rview_name;

  /* Stored for deep recolor access (not owned - owned by session). */
  BlenderOutputDriver *blender_output_driver = nullptr;

  string last_status;
  string last_error;
  double last_progress;
  double last_status_time;

  int width, height;
  float pixelsize;
  bool preview_osl;
  double start_resize_time;
  std::optional<BlenderDeepFrameBudgets> deep_frame_budgets_;
  bool direct_deep_without_compositor_ = false;
  bool skip_full_buffer_readback_for_background_direct_deep_ = false;

  bool use_developer_ui;

  /* Global state which is common for all render sessions created from Blender.
   * Usually denotes command line arguments.
   */
  static DeviceTypeMask device_override;

  /* Blender is running from the command line, no windows are shown and some
   * extra render optimization is possible (possible to free draw-only data and
   * so on.
   */
  static bool headless;

  static bool print_render_stats;

 protected:
  void stamp_view_layer_metadata(Scene *scene, const string &view_layer_name);

  /* Check whether session error happened.
   * If so, it is reported to the render engine and true is returned.
   * Otherwise false is returned. */
  bool check_and_report_session_error();

  void builtin_images_load();

  /* Is used after each render layer synchronization is done with the goal
   * of freeing render engine data which is held from Blender side (for
   * example, dependency graph).
   */
  void free_blender_memory_if_possible();

  void ensure_display_driver_if_needed();

  struct {
    thread_mutex mutex;
    int last_pass_index = -1;
  } draw_state_;

  /* NOTE: The BlenderSession references the display driver. */
  BlenderDisplayDriver *display_driver_ = nullptr;

  vector<string> full_buffer_files_;
  RenderDisplayWindow render_result_display_window_;
  struct DeepDeferredView {
    size_t file_index;
    std::shared_ptr<class DeepFrameProduction> production;
  };
  struct DeepDeferredDirect {
    std::shared_ptr<class BlenderDeepFrameViews> views;
    int compression;
    bool half;
    float depth, alpha;
  };
  vector<DeepDeferredView> deep_deferred_views_;
  vector<DeepDeferredDirect> deep_deferred_direct_;

  int bake_id = 0;
};

CCL_NAMESPACE_END
