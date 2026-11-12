/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include <cmath>
#include <cstring>

#include "device/cpu/device.h"
#include "device/device.h"
#include "integrator/deep_bounded_cpu.h"
#include "integrator/deep_bounded_work.h"
#include "integrator/deep_native_capacity.h"
#include "integrator/path_trace.h"
#include "scene/background.h"
#include "scene/camera.h"
#include "scene/image.h"
#include "scene/integrator.h"
#include "scene/light.h"
#include "scene/mesh.h"
#include "scene/object.h"
#include "scene/scene.h"
#include "scene/shader_graph.h"
#include "session/buffers.h"
#include "session/deep_buffers.h"
#include "session/display_driver.h"
#include "session/output_driver.h"
#include "session/session.h"

#include "util/log.h"
#include "util/math.h"
#include "util/string.h"
#include "util/system.h"
#include "util/task.h"
#include "util/time.h"

CCL_NAMESPACE_BEGIN

namespace {

bool buffer_params_has_expanded_data_window(const BufferParams &buffer_params)
{
  return buffer_params.window_x != 0 || buffer_params.window_y != 0 ||
         buffer_params.width > buffer_params.full_width ||
         buffer_params.height > buffer_params.full_height || buffer_params.full_x < 0 ||
         buffer_params.full_y < 0 ||
         (buffer_params.full_x + buffer_params.width) > buffer_params.full_width ||
         (buffer_params.full_y + buffer_params.height) > buffer_params.full_height;
}

bool buffer_params_use_data_window_camera_resolution(const BufferParams &buffer_params)
{
  if (!buffer_params_has_expanded_data_window(buffer_params)) {
    return false;
  }

  /* A negative window offset marks full-frame overscan whose camera space expands with the
   * physical buffer. Cropped and viewport buffers instead keep global pixel coordinates in the
   * original full-frame camera space, even when their padded data window extends outside it. */
  return buffer_params.window_x < 0 || buffer_params.window_y < 0;
}

}  // namespace

Session::Session(const SessionParams &params_, const SceneParams &scene_params)
    : params(params_),
      eviction_manager_(params_.background),
      render_scheduler_(tile_manager_, params)
{
  TaskScheduler::init(params.threads);

  delayed_reset_.do_reset = false;

  pause_ = false;
  new_work_added_ = false;

  device = Device::create(params.device, stats, profiler, params_.headless);

  /* Qualification-only switch: run the Alpha module with no capture binding to
   * distinguish mode specialization from work done by the producer. */
  if (params_.headless && device->info.type == DEVICE_OPTIX) {
    const char *mode = getenv("CYCLES_TEST_DEEP_ALPHA_MODULE_WITHOUT_CAPTURE");
    if (mode && (std::strcmp(mode, "1") == 0 || std::strcmp(mode, "compute") == 0 ||
                 std::strcmp(mode, "optix") == 0 || std::strncmp(mode, "compute-", 8) == 0))
    {
      device->set_deep_bounded_policy(1);
    }
  }

  if (device->have_error()) {
    progress.set_error(device->error_message());
  }

  scene = make_unique<Scene>(scene_params, device.get());

  if (params.device == params.denoise_device) {
    /* Reuse render device. */
  }
  else {
    denoise_device_ = Device::create(params.denoise_device, stats, profiler, params_.headless);

    if (denoise_device_->have_error()) {
      progress.set_error(denoise_device_->error_message());
    }
  }

  /* Configure path tracer. */
  path_trace_ = make_unique<PathTrace>(device.get(),
                                       denoise_device(),
                                       scene->film,
                                       &scene->dscene,
                                       render_scheduler_,
                                       tile_manager_);
  path_trace_->set_progress(&progress);
  path_trace_->progress_update_cb = [&]() { update_status_time(); };

  tile_manager_.full_buffer_written_cb = [&](string_view filename) {
    if (!full_buffer_written_cb) {
      return;
    }
    full_buffer_written_cb(filename);
  };

  /* Create session thread. */
  session_thread_ = make_unique<thread>([this] { thread_run(); });
}

Session::~Session()
{
  /* Cancel any ongoing render operation. */
  cancel();

  /* Signal session thread to end. */
  {
    const thread_scoped_lock session_thread_lock(session_thread_mutex_);
    session_thread_state_ = SESSION_THREAD_END;
  }
  session_thread_cond_.notify_all();

  /* Destroy session thread. */
  session_thread_->join();
  session_thread_.reset();

  /* Destroy path tracer, before the device. This is needed because destruction might need to
   * access device for device memory free.
   * TODO(sergey): Convert device to be unique_ptr, and rely on C++ to destruct objects in the
   * pre-defined order. */
  path_trace_.reset();

  /* Destroy scene and device. */
  scene.reset();
  denoise_device_.reset();
  device.reset();

  /* Stop task scheduler. */
  TaskScheduler::exit();
}

void Session::start()
{
  {
    /* Signal session thread to start rendering. */
    const thread_scoped_lock session_thread_lock(session_thread_mutex_);
    if (session_thread_state_ == SESSION_THREAD_RENDER) {
      /* Already rendering, nothing to do. */
      return;
    }
    session_thread_state_ = SESSION_THREAD_RENDER;
  }

  session_thread_cond_.notify_all();
}

void Session::cancel(bool quick)
{
  /* Cancel any long running device operations (e.g. shader compilations). */
  device->cancel();

  /* Check if session thread is rendering. */
  const bool rendering = is_session_thread_rendering();

  if (rendering) {
    /* Cancel path trace operations. */
    if (quick && path_trace_) {
      path_trace_->cancel();
    }

    /* Cancel other operations. */
    progress.set_cancel("Exiting");

    /* Signal unpause in case the render was paused. */
    {
      const thread_scoped_lock pause_lock(pause_mutex_);
      pause_ = false;
    }
    pause_cond_.notify_all();

    /* Wait for render thread to be cancelled or finished. */
    wait();
  }
}

bool Session::ready_to_reset()
{
  return path_trace_->ready_to_reset();
}

void Session::run_main_render_loop()
{
  path_trace_->zero_display();

  while (true) {
    RenderWork render_work = run_update_for_next_iteration();

    const bool did_cancel = progress.get_cancel();

    if (!render_work) {
      if (LOG_IS_ON(LOG_LEVEL_INFO)) {
        if (did_cancel) {
          LOG_INFO << "Rendering was canceled.";
        }
        else {
          double total_time;
          double render_time;
          progress.get_time(total_time, render_time);
          LOG_INFO << "Rendering in main loop is done in " << render_time << " seconds.";
          LOG_INFO << path_trace_->full_report();
        }
      }

      if (params.background) {
        /* if no work left and in background mode, we can stop immediately. */
        progress.set_status("Finished");
        break;
      }
    }

    if (did_cancel) {
      render_scheduler_.render_work_reschedule_on_cancel(render_work);
      if (!render_work) {
        break;
      }
    }
    else if (run_wait_for_work(render_work)) {
      continue;
    }

    /* Stop rendering if error happened during scene update or other step of preparing scene
     * for render. */
    if (device->have_error()) {
      progress.set_error(device->error_message());
      break;
    }

    {
      /* buffers mutex is locked entirely while rendering each
       * sample, and released/reacquired on each iteration to allow
       * reset and draw in between */
      const thread_scoped_lock buffers_lock(buffers_mutex_);

      /* update status and timing */
      update_status_time();

      /* render */
      path_trace_->render(render_work);

      /* update status and timing */
      update_status_time();

      /* Stop rendering if error happened during path tracing. */
      if (device->have_error()) {
        progress.set_error(device->error_message());
        break;
      }
    }

    progress.set_update();

    if (did_cancel) {
      break;
    }
  }
}

void Session::thread_run()
{
  while (true) {
    {
      thread_scoped_lock session_thread_lock(session_thread_mutex_);

      if (session_thread_state_ == SESSION_THREAD_WAIT) {
        /* Continue waiting for any signal from the main thread. */
        session_thread_cond_.wait(session_thread_lock);
        continue;
      }
      if (session_thread_state_ == SESSION_THREAD_END) {
        /* End thread immediately. */
        break;
      }
    }

    /* Execute a render. */
    thread_render();

    /* Go back from rendering to waiting. */
    {
      const thread_scoped_lock session_thread_lock(session_thread_mutex_);
      if (session_thread_state_ == SESSION_THREAD_RENDER) {
        session_thread_state_ = SESSION_THREAD_WAIT;
      }
    }
    session_thread_cond_.notify_all();
  }

  /* Flush any remaining operations and destroy display driver here. This ensure
   * graphics API resources are created and destroyed all in the session thread,
   * which can avoid problems contexts and multiple threads. */
  path_trace_->flush_display();
  path_trace_->set_display_driver(nullptr);
}

void Session::thread_render()
{
  if (params.use_profiling && (params.device.type == DEVICE_CPU)) {
    profiler.start();
  }

  /* session thread loop */
  progress.set_status("Waiting for render to start");

  /* run */
  if (!progress.get_cancel()) {
    /* reset number of rendered samples */
    progress.reset_sample();

    run_main_render_loop();
  }

  profiler.stop();

  /* progress update */
  if (progress.get_cancel()) {
    progress.set_status(progress.get_cancel_message());
  }
  else {
    progress.set_update();
  }
}

bool Session::is_session_thread_rendering()
{
  const thread_scoped_lock session_thread_lock(session_thread_mutex_);
  return (session_thread_state_ == SESSION_THREAD_RENDER);
}

RenderWork Session::run_update_for_next_iteration()
{
  RenderWork render_work;

  thread_scoped_lock scene_lock(scene->mutex);

  /* Perform delayed reset if requested. */
  const bool reset_buffers = delayed_reset_buffer_params();

  /* The pending flag was consumed above. Preserve this reset when updating the
   * integrator so a new render can lower its sample count after cancellation. */
  const bool reset_scene = update_scene(reset_buffers);

  /* Update buffers for new parameters. After scene update which influences the passes used. */
  bool have_tiles = true;
  bool switched_to_new_tile = false;

  if (reset_buffers) {
    update_buffers_for_params();

    if (deep_frame_capture_consumer_ && deep_frame_capture_consumer_->uses_bounded_processing()) {
      const int2 tile_size = get_effective_tile_size();
      const size_t devices = std::max(size_t(1), device->info.multi_devices.size());
      bool retain_native_film = false;
      if (!deep_bounded_admit_host(*deep_frame_capture_consumer_,
                                   buffer_params_,
                                   size_t(tile_size.x) * tile_size.y,
                                   devices,
                                   system_available_ram(),
                                   retain_native_film,
                                   deep_frame_capture_consumer_->uses_cpu_processing() ?
                                       size_t(device->info.cpu_threads) :
                                       0))
      {
        progress.set_error("Deep host quota cannot admit this frame and its output consumers");
        return render_work;
      }
      path_trace_->set_retain_bounded_native_film(retain_native_film);
    }

    /* After reset make sure the tile manager is at the first big tile. */
    have_tiles = tile_manager_.next();
    switched_to_new_tile = true;

    eviction_manager_.reset();
  }

  /* Update denoiser settings. */
  {
    const DenoiseParams denoise_params = scene->integrator->get_denoise_params();
    path_trace_->set_denoiser_params(denoise_params);
  }

  /* Update adaptive sampling. */
  {
    const AdaptiveSampling adaptive_sampling = scene->integrator->get_adaptive_sampling();
    path_trace_->set_adaptive_sampling(adaptive_sampling);
  }

  /* Update path guiding. */
  {
    const GuidingParams guiding_params = scene->integrator->get_guiding_params(device.get());
    const bool guiding_reset = (guiding_params.use) ? reset_scene : false;
    path_trace_->set_guiding_params(guiding_params, guiding_reset);
  }

  render_scheduler_.set_sample_params(params.samples,
                                      params.use_sample_subset,
                                      params.sample_subset_offset,
                                      params.sample_subset_length);
  render_scheduler_.set_time_limit(params.time_limit);

  while (have_tiles) {
    render_work = render_scheduler_.get_render_work();
    if (render_work) {
      break;
    }

    progress.add_finished_tile(false);

    have_tiles = tile_manager_.next();
    if (have_tiles) {
      render_scheduler_.reset_for_next_tile();
      switched_to_new_tile = true;
    }
  }

  /* Evict unused image tiles periodically. */
  if (eviction_manager_.need_eviction(!render_work, switched_to_new_tile)) {
    scene->image_manager->evict_unused(device.get(), scene.get());
  }

  if (render_work) {
    const scoped_timer update_timer;

    if (switched_to_new_tile) {
      BufferParams tile_params = buffer_params_;

      const Tile &tile = tile_manager_.get_current_tile();

      tile_params.width = tile.width;
      tile_params.height = tile.height;

      tile_params.window_x = tile.window_x;
      tile_params.window_y = tile.window_y;
      tile_params.window_width = tile.window_width;
      tile_params.window_height = tile.window_height;

      tile_params.full_x = tile.x + buffer_params_.full_x;
      tile_params.full_y = tile.y + buffer_params_.full_y;
      tile_params.full_width = buffer_params_.full_width;
      tile_params.full_height = buffer_params_.full_height;

      tile_params.update_offset_stride();

      path_trace_->reset(buffer_params_, tile_params, reset_buffers);
    }

    /* Update camera if dimensions changed for progressive render. the camera
     * knows nothing about progressive or cropped rendering, it just gets the
     * image dimensions passed in. */
    const float resolution = render_work.resolution_divider;
    const bool use_data_window_resolution = buffer_params_use_data_window_camera_resolution(
        buffer_params_);
    const int width = max(
        1,
        int((use_data_window_resolution ? buffer_params_.width : buffer_params_.full_width) /
            resolution));
    const int height = max(
        1,
        int((use_data_window_resolution ? buffer_params_.height : buffer_params_.full_height) /
            resolution));

    scene->update_camera_resolution(progress, width, height);

    /* Unlock scene mutex before loading denoiser kernels, since that may attempt to activate
     * graphics interop, which can deadlock when the scene mutex is still being held. */
    scene_lock.unlock();

    path_trace_->load_kernels();
    path_trace_->alloc_work_memory();

    /* Wait for device to be ready (e.g. finish any background compilations). */
    string device_status;
    while (!device->is_ready(device_status)) {
      progress.set_status(device_status);
      if (progress.get_cancel()) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    progress.add_skip_time(update_timer, params.background);
  }

  return render_work;
}

bool Session::run_wait_for_work(const RenderWork &render_work)
{
  /* In an offline rendering there is no pause, and no tiles will mean the job is fully done. */
  if (params.background) {
    return false;
  }

  thread_scoped_lock pause_lock(pause_mutex_);

  if (!pause_ && render_work) {
    /* Rendering is not paused and there is work to be done. No need to wait for anything. */
    return false;
  }

  const bool no_work = !render_work;
  update_status_time(pause_, no_work);

  /* Only leave the loop when rendering is not paused. But even if the current render is
   * un-paused but there is nothing to render keep waiting until new work is added. */
  while (!progress.get_cancel()) {
    const scoped_timer pause_timer;

    if (!pause_ && (render_work || new_work_added_ || delayed_reset_.do_reset)) {
      break;
    }

    const std::chrono::milliseconds wait_time = eviction_manager_.wait_time(!render_work);
    if (wait_time == std::chrono::milliseconds::zero()) {
      /* Break out of the loop for cache eviction. */
      break;
    }

    /* Wait for either pause state changed, extra samples added to render, or idle
     * timer before performing eviction. */
    if (wait_time == std::chrono::milliseconds::max()) {
      pause_cond_.wait(pause_lock);
    }
    else {
      pause_cond_.wait_for(pause_lock, wait_time);
    }

    if (pause_) {
      progress.add_skip_time(pause_timer, params.background);
    }

    update_status_time(pause_, no_work);
    progress.set_update();
  }

  new_work_added_ = false;

  return no_work;
}

void Session::draw()
{
  path_trace_->draw();
}

int2 Session::get_effective_tile_size() const
{
  const int image_width = buffer_params_.width;
  const int image_height = buffer_params_.height;

  if (deep_frame_capture_consumer_) {
    if (deep_frame_capture_consumer_->uses_bounded_processing()) {
      if (!params.use_auto_tile) {
        return make_int2(image_width, image_height);
      }
      const int requested = tile_manager_.compute_render_tile_size(params.tile_size);
      if (deep_frame_capture_consumer_->uses_cpu_processing()) {
        const auto &budget = deep_frame_capture_consumer_->device_memory_budget();
        const size_t available = budget.quota() - budget.used();
        const uint32_t threads = uint32_t(std::max(device->info.cpu_threads, 1));
        const int block = TileManager::IMAGE_TILE_SIZE;
        const auto fits = [&](const int2 size) {
          const uint64_t p = uint64_t(size.x) * size.y;
          DeepCPUAdmission admission;
          return p < (1u << 30) &&
                 deep_cpu_admission(
                     *deep_frame_capture_consumer_, uint32_t(p), threads, admission) &&
                 admission.peak <= available;
        };
        for (int side = requested; side > 0;) {
          const int2 square = make_int2(std::min(side, image_width), std::min(side, image_height));
          if (fits(square)) {
            for (const int columns : {3, 2}) {
              const int2 rectangle = make_int2(std::min({columns * block, requested, image_width}),
                                               std::min({block, requested, image_height}));
              if (uint64_t(rectangle.x) * rectangle.y <= uint64_t(square.x) * square.y)
                break;
              if (fits(rectangle))
                return rectangle;
            }
            return square;
          }
          side = side > block ? ((side - 1) / block) * block : side / 2;
        }
        /* The owner produces the explicit minimum-working-set failure. */
        return make_int2(1, 1);
      }
      const int2 limits = path_trace_->deep_bounded_camera_limits();
      const auto policy = DeepBoundedPolicy(device->deep_bounded_policy());
      const auto &budget = deep_frame_capture_consumer_->device_memory_budget();
      const uint32_t families = deep_bounded_family_count(deep_frame_capture_consumer_->bounded_consumers());
      size_t total = 0, free = 0;
      device->get_device_memory_info(total, free);
      const size_t quota = budget.quota() - budget.used();
      const size_t available = total ? std::min(quota, free) : quota;
      const uint32_t max_samples = deep_frame_capture_consumer_->bounded_max_samples();
      const auto devices = path_trace_->deep_bounded_admission_devices();
      const auto admit = [&](const uint32_t pixels) {
        return devices.empty() ? deep_bounded_choose_packet(
            pixels, max_samples, uint32_t(limits.x), policy,
            buffer_params_.use_transparent_background, families, available) :
            deep_bounded_choose_devices(pixels, max_samples, policy,
                buffer_params_.use_transparent_background, families, quota, devices);
      };
      const int block = TileManager::IMAGE_TILE_SIZE;
      const auto prefer_rectangular = [&](const int2 square) {
        /* Larger native waves amortize Work joins. Keep an eight-sample
         * admission floor so a wider Tile does not fragment every packet. */
        for (const int columns : {3, 2}) {
          const int2 size = make_int2(std::min({columns * block, requested, image_width}),
                                     std::min({block, requested, image_height}));
          const uint32_t pixels = uint32_t(size.x) * size.y;
          if (pixels <= uint64_t(square.x) * square.y) {
            break;
          }
          const auto rectangle = admit(pixels);
          if (rectangle.roots && rectangle.packet_samples >= std::min(max_samples, 8u)) {
            return size;
          }
        }
        return square;
      };
      DeepBoundedAdmission best;
      int2 best_size = make_int2(1, 1);
      const char *largest_tile = getenv("CYCLES_TEST_DEEP_LARGEST_ADMITTED_TILE");
      const bool test_largest_tile = params.headless && largest_tile &&
                                     std::strcmp(largest_tile, "1") == 0;
      for (int side = requested; side > 0;) {
        const int2 size = make_int2(std::min(side, image_width), std::min(side, image_height));
        const uint64_t pixels = uint64_t(size.x) * size.y;
        if (pixels && pixels < (uint64_t(1) << 30)) {
          const auto candidate = admit(uint32_t(pixels));
          if (candidate.roots &&
              (test_largest_tile || candidate.roots >= uint32_t(limits.y)))
          {
            return test_largest_tile ? size : prefer_rectangular(size);
          }
          if (candidate.roots > best.roots) {
            best = candidate;
            best_size = size;
          }
        }
        side = side > block ? ((side - 1) / block) * block : side / 2;
      }
      return test_largest_tile ? best_size : prefer_rectangular(best_size);
    }
    DeepNativeCaptureCapacity one;
    const bool gpu = deep_frame_capture_consumer_->uses_bounded_processing();
    const bool rgb = deep_frame_capture_consumer_->capture_demand().capture_rgb();
    const uint32_t stack_size = scene->dscene.data.volume_stack_size ?
                                    scene->dscene.data.volume_stack_size : MAX_VOLUME_STACK_SIZE;
    if (!deep_native_capture_capacity(1, 0, true, true, false, one, true, rgb, !gpu, gpu,
                                      stack_size))
      return make_int2(1, 1);
    const size_t quota = deep_frame_capture_consumer_->device_memory_budget().quota();
    constexpr size_t index_reserve = 16 << 20;
    size_t maximum_pixels = quota > index_reserve ? (quota - index_reserve) / one.device : 1;
    if (deep_frame_capture_consumer_->uses_bounded_processing()) {
      /* Keep room for a joined producer page and a continuation transaction.
       * Final AA windows are separately admitted from actual archived counts. */
      maximum_pixels = std::max(size_t(1), maximum_pixels / 4);
      const auto &host = deep_frame_capture_consumer_->memory_budget();
      const size_t host_available = host.quota() - host.used();
      const size_t raw_per_camera = sizeof(DeepCompactCamera) + 2 * sizeof(DeepCompactPrimitive) +
                                   (rgb ? 2 * sizeof(DeepCompactColourSource) : 0);
      const size_t samples = size_t(std::max(1, params.samples));
      const size_t host_pixels = host_available / 2 / raw_per_camera / samples;
      maximum_pixels = std::max(size_t(1), std::min(maximum_pixels, host_pixels));
    }
    const int halo = scene->integrator->get_adaptive_sampling().use ? 8 : 0;
    const int cap = deep_frame_capture_consumer_->uses_bounded_processing() ? 256 : 64;
    const int side = std::max(1, std::min(cap, int(std::sqrt(double(maximum_pixels)))) - halo);
    const int requested = params.use_auto_tile ?
                              tile_manager_.compute_render_tile_size(params.tile_size) :
                              std::max(image_width, image_height);
    int bounded = std::min(requested, side);
    /* The native temporary EXR uses IMAGE_TILE_SIZE blocks for big tiles.
     * Round down to keep both budget admission and writer alignment. A tile
     * smaller than that block defines its own file block size instead. */
    if (bounded > TileManager::IMAGE_TILE_SIZE)
      bounded = (bounded / TileManager::IMAGE_TILE_SIZE) * TileManager::IMAGE_TILE_SIZE;
    std::fprintf(stderr,
                 "DEEP_FRAME_NATIVE_TILE side=%d halo=%d requested=%d device_quota=%llu\n",
                 bounded,
                 halo,
                 requested,
                 static_cast<unsigned long long>(quota));
    return make_int2(std::min(image_width, bounded), std::min(image_height, bounded));
  }

  if (!params.use_auto_tile) {
    return make_int2(image_width, image_height);
  }

  const int64_t image_area = static_cast<int64_t>(image_width) * image_height;

  /* TODO(sergey): Take available memory into account for non-deep renders so we can
   * avoid tiling when there is enough memory. */
  /* Deep EXR uses a user-configured tile budget. */

  int tile_size = tile_manager_.compute_render_tile_size(params.tile_size);
  bool deep_tile_clamp_active = false;

  if (scene && scene->film->get_use_deep_output() && params.deep_tile_budget_mb > 0) {
    constexpr bool bounded_pool = false;
    const int max_samples = deep_effective_capture_samples(
        scene->film, scene->integrator, params.samples);

    size_t bytes_per_pixel = 0;
    const bool sparse_capture = getenv("CYCLES_EXPERIMENTAL_DEEP_VOLUME_PROFILE") != nullptr;
    const bool size_valid = sparse_capture ?
                                deep_compute_sparse_buffer_bytes(
                                    1, 1, bounded_pool ? 1 : max_samples, bytes_per_pixel) :
                                deep_compute_buffer_bytes(1, 1, max_samples, bytes_per_pixel);
    if (size_valid && bytes_per_pixel > 0) {
      const size_t deep_budget_bytes = static_cast<size_t>(params.deep_tile_budget_mb) * 1024 *
                                       1024;
      const size_t max_pixels = deep_budget_bytes / bytes_per_pixel;
      if (max_pixels > 0) {
        const double max_pixels_double = static_cast<double>(max_pixels);
        const int max_tile_unaligned = max(
            8, static_cast<int>(std::floor(std::sqrt(max_pixels_double))));
        int max_tile = max_tile_unaligned;
        if (bounded_pool) {
          /* Geometry no longer reserves the worst-case step count at every pixel. Camera

           * * admission drains the bounded append pool before reuse. Keep frame workspace bounded

           * * too; this changes execution batches, never the user's event storage budget. */
          int geometry_limit = 768;
          if (const char *value = getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_RENDER_TILE_SIZE")) {
            geometry_limit = clamp(atoi(value), 64, 2048);
          }
          max_tile = min(max_tile, geometry_limit);
        }
        if (max_tile >= TileManager::IMAGE_TILE_SIZE) {
          max_tile = (max_tile / TileManager::IMAGE_TILE_SIZE) * TileManager::IMAGE_TILE_SIZE;
        }

        if (max_tile < tile_size) {
          deep_tile_clamp_active = true;
          tile_size = max_tile;

          size_t estimated_bytes = 0;
          int estimated_samples = 0;
          const string budget_str = string_human_readable_size(deep_budget_bytes);
          const bool estimate_valid = bounded_pool ?
                                          deep_bounded_sparse_capture_samples(tile_size,
                                                                              tile_size,
                                                                              max_samples,
                                                                              deep_budget_bytes,
                                                                              estimated_samples,
                                                                              estimated_bytes) :
                                      sparse_capture ?
                                          deep_compute_sparse_buffer_bytes(
                                              tile_size, tile_size, max_samples, estimated_bytes) :
                                          deep_compute_buffer_bytes(
                                              tile_size, tile_size, max_samples, estimated_bytes);
          if (estimate_valid) {
            const string estimated_str = string_human_readable_size(estimated_bytes);
            LOG_INFO << "Deep EXR: clamping tile size to " << tile_size << " (budget "
                     << budget_str << ", estimated " << estimated_str << ", max samples "
                     << max_samples << ")";
          }
          else {
            /* Overflow while estimating deep buffer bytes. */
            LOG_INFO << "Deep EXR: clamping tile size to " << tile_size << " (budget "
                     << budget_str << ", max samples " << max_samples << ")";
          }
        }
      }
    }
  }

  const int64_t actual_tile_area = static_cast<int64_t>(tile_size) * tile_size;

  if (!deep_tile_clamp_active && actual_tile_area >= image_area &&
      image_width <= TileManager::MAX_TILE_SIZE && image_height <= TileManager::MAX_TILE_SIZE)
  {
    return make_int2(image_width, image_height);
  }

  return make_int2(tile_size, tile_size);
}

bool Session::delayed_reset_buffer_params()
{
  /* Reset buffer parameters, delayed from when we got the reset call so we can complete
   * rendering the sample. Otherwise e.g. viewport navigation might reset without ever
   * finishing anything. */
  const thread_scoped_lock reset_lock(delayed_reset_.mutex);
  if (!delayed_reset_.do_reset) {
    return false;
  }

  const thread_scoped_lock buffers_lock(buffers_mutex_);
  delayed_reset_.do_reset = false;

  params = delayed_reset_.session_params;
  buffer_params_ = delayed_reset_.buffer_params;

  /* Store parameters used for buffers access outside of scene graph. */
  buffer_params_.samples = min(params.samples, Integrator::MAX_SAMPLES);
  buffer_params_.exposure = scene->film->get_exposure();
  buffer_params_.use_approximate_shadow_catcher =
      scene->film->get_use_approximate_shadow_catcher();
  buffer_params_.use_transparent_background = scene->background->get_transparent();

  /* Tile and work scheduling. */
  tile_manager_.reset_scheduling(buffer_params_, get_effective_tile_size());

  return true;
}

void Session::update_buffers_for_params()
{
  render_scheduler_.set_sample_params(params.samples,
                                      params.use_sample_subset,
                                      params.sample_subset_offset,
                                      params.sample_subset_length);
  render_scheduler_.reset(buffer_params_);

  /* Update for new state of scene and passes. */
  buffer_params_.update_passes(scene->passes);
  tile_manager_.update(buffer_params_, scene.get());

  /* Update temp directory on reset.
   * This potentially allows to finish the existing rendering with a previously configure
   * temporary
   * directory in the host software and switch to a new temp directory when new render starts. */
  tile_manager_.set_temp_dir(params.temp_dir);

  /* Progress. */
  progress.reset_sample();
  progress.set_total_pixel_samples(static_cast<uint64_t>(buffer_params_.width) *
                                   buffer_params_.height * buffer_params_.samples);

  if (!params.background) {
    progress.set_start_time();
  }
  const double time_limit = params.time_limit * ((double)tile_manager_.get_num_tiles());
  progress.set_render_start_time();
  progress.set_time_limit(time_limit);
}

void Session::reset(const SessionParams &session_params, const BufferParams &buffer_params)
{
  {
    const thread_scoped_lock reset_lock(delayed_reset_.mutex);
    const thread_scoped_lock pause_lock(pause_mutex_);

    delayed_reset_.do_reset = true;
    delayed_reset_.session_params = session_params;
    delayed_reset_.buffer_params = buffer_params;

    scene->scene_updated_while_loading_kernels = true;

    path_trace_->cancel();
  }

  pause_cond_.notify_all();
}

void Session::set_samples(const int samples)
{
  if (samples == params.samples) {
    return;
  }

  params.samples = samples;

  {
    const thread_scoped_lock pause_lock(pause_mutex_);
    new_work_added_ = true;
  }

  pause_cond_.notify_all();
}

void Session::set_time_limit(const double time_limit)
{
  if (time_limit == params.time_limit) {
    return;
  }

  params.time_limit = time_limit;

  {
    const thread_scoped_lock pause_lock(pause_mutex_);
    new_work_added_ = true;
  }

  pause_cond_.notify_all();
}

void Session::set_pause(bool pause)
{
  bool notify = false;

  {
    const thread_scoped_lock pause_lock(pause_mutex_);

    if (pause != pause_) {
      pause_ = pause;
      notify = true;
    }
  }

  if (is_session_thread_rendering()) {
    if (notify) {
      pause_cond_.notify_all();
    }
  }
  else if (pause_) {
    update_status_time(pause_);
  }
}

void Session::set_navigating(bool navigating)
{
  eviction_manager_.set_navigating(navigating);
}

void Session::set_output_driver(unique_ptr<OutputDriver> driver)
{
  path_trace_->set_output_driver(std::move(driver));
}

void Session::set_display_driver(unique_ptr<DisplayDriver> driver)
{
  path_trace_->set_display_driver(std::move(driver));
}


bool Session::set_deep_frame_capture_consumer(DeepFrameCaptureConsumer *consumer)
{
  const uint32_t old_policy = device->deep_bounded_policy();
  const DeepCaptureDemand demand = consumer ? consumer->capture_demand() : DeepCaptureDemand{};
  uint32_t policy = 0;
  if (consumer && consumer->uses_bounded_processing()) {
    policy = !demand.needs_rgb ? 1u : demand.simple_mode ? 2u : 3u;
  }
  device->set_deep_bounded_policy(policy);
  if (!path_trace_->set_deep_frame_capture_consumer(consumer)) {
    device->set_deep_bounded_policy(old_policy);
    return false;
  }
  deep_frame_capture_consumer_ = consumer;
  if (old_policy != policy && scene) {
    scene->film->tag_modified();
  }
  return true;
}


double Session::get_estimated_remaining_time() const
{
  const double completed = progress.get_progress();
  if (completed == 0.0) {
    return 0.0;
  }

  double total_time;
  double render_time;
  progress.get_time(total_time, render_time);
  double remaining = (1.0 - (double)completed) * (render_time / (double)completed);

  const double time_limit = render_scheduler_.get_time_limit() *
                            ((double)tile_manager_.get_num_tiles());
  if (time_limit != 0.0) {
    remaining = min(remaining, max(time_limit - render_time, 0.0));
  }

  return remaining;
}

void Session::wait()
{
  /* Wait until session thread either is waiting or ending. */
  while (true) {
    thread_scoped_lock session_thread_lock(session_thread_mutex_);
    if (session_thread_state_ != SESSION_THREAD_RENDER) {
      break;
    }
    session_thread_cond_.wait(session_thread_lock);
  }
}

bool Session::update_scene(const bool reset_samples)
{
  /* Update number of samples in the integrator.
   * Ideally this would need to happen once in `Session::set_samples()`, but the issue there is
   * the initial configuration when Session is created where the `set_samples()` is not used.
   *
   * NOTE: Unless reset was requested only allow increasing number of samples. */
  if (reset_samples || scene->integrator->get_aa_samples() < params.samples) {
    scene->integrator->set_aa_samples(params.samples);
  }

  scene->integrator->set_use_sample_subset(params.use_sample_subset);
  scene->integrator->set_sample_subset_offset(params.sample_subset_offset);
  scene->integrator->set_sample_subset_length(params.sample_subset_length);

  /* When multiple tiles are used SAMPLE_COUNT pass is used to keep track of possible partial
   * tile results. Deep output also needs per-pixel sample counts to reconstruct hard-surface
   * edge coverage after sorting deep hits by depth. */
  scene->film->set_use_sample_count(tile_manager_.has_multiple_tiles() ||
                                    scene->film->get_use_deep_output());

  const bool reset = scene->need_reset(false);

  if (scene->update(progress)) {
    profiler.reset(scene->shaders.size(), scene->objects.size());
  }

  return reset;
}

static string status_append(const string &status, const string &suffix)
{
  string prefix = status;
  if (!prefix.empty()) {
    prefix += ", ";
  }
  return prefix + suffix;
}

void Session::update_status_time(bool show_pause, bool show_done)
{
  string status;
  string substatus;

  const int current_tile = progress.get_rendered_tiles();
  const int num_tiles = tile_manager_.get_num_tiles();

  const int current_sample = progress.get_current_sample();
  const int num_samples = render_scheduler_.get_num_samples();

  /* TIle. */
  if (tile_manager_.has_multiple_tiles()) {
    substatus = status_append(substatus,
                              string_printf("Rendered %d/%d Tiles", current_tile, num_tiles));
  }

  /* Sample. */
  if (!params.background && num_samples == Integrator::MAX_SAMPLES) {
    substatus = status_append(substatus, string_printf("Sample %d", current_sample));
  }
  else {
    substatus = status_append(substatus,
                              string_printf("Sample %d/%d", current_sample, num_samples));
  }

  /* Append any device-specific status (such as background kernel optimization) */
  string device_status;
  if (device->is_ready(device_status) && !device_status.empty()) {
    substatus += string_printf(" (%s)", device_status.c_str());
  }

  /* TODO(sergey): Denoising status from the path trace. */

  if (show_pause) {
    status = "Rendering Paused";
  }
  else if (show_done) {
    status = "Rendering Done";
    progress.set_end_time(); /* Save end time so that further calls to get_time are accurate. */
  }
  else {
    status = substatus;
    substatus.clear();
  }

  progress.set_status(status, substatus);
}

void Session::device_free()
{
  scene->device_free();
  path_trace_->device_free();
}

void Session::collect_statistics(RenderStats *render_stats)
{
  scene->collect_statistics(render_stats);
  if (params.use_profiling && (params.device.type == DEVICE_CPU)) {
    render_stats->collect_profiling(scene.get(), profiler);
  }
}

/* --------------------------------------------------------------------
 * Full-frame on-disk storage.
 */

void Session::process_full_buffer_from_disk(string_view filename,
                                            DeepFrameCaptureConsumer *beauty_consumer)
{
  path_trace_->process_full_buffer_from_disk(filename, beauty_consumer);
}

CCL_NAMESPACE_END
