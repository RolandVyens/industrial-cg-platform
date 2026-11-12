/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "integrator/path_trace.h"

#include <cstring>

#include "device/cpu/device.h"
#include "device/device.h"

#include "integrator/deep_frame_capture.h"
#include "integrator/deep_bounded_plan.h"
#include "integrator/pass_accessor.h"
#include "integrator/path_trace_display.h"
#include "integrator/path_trace_tile.h"
#include "integrator/render_scheduler.h"

#include "scene/pass.h"
#include "scene/scene.h"

#include "session/display_driver.h"
#include "session/tile.h"

#include "util/log.h"
#include "util/progress.h"
#include "util/scoped_defer.h"
#include "util/tbb.h"
#include "util/time.h"

CCL_NAMESPACE_BEGIN

namespace {

uint32_t pass_float_as_uint(const float value)
{
  uint32_t result;
  memcpy(&result, &value, sizeof(result));
  return result;
}

float combined_transparency_to_alpha(const float transparency)
{
  return min(max(1.0f - transparency, 0.0f), 1.0f);
}

}  // namespace

struct PathTrace::BoundedNativeFilm {
  BoundedNativeFilm(Device &device, const DeepMemoryBudget &budget, const size_t bytes)
      : buffer(&device), budget(budget), bytes(bytes)
  {
    budget.acquire(bytes);
  }

  ~BoundedNativeFilm()
  {
    buffer.buffer.free();
    budget.relinquish(bytes);
  }

  RenderBuffers buffer;
  DeepMemoryBudget budget;
  size_t bytes;
  uint64_t written_pixels = 0;
};

PathTrace::PathTrace(Device *device,
                     Device *denoise_device,
                     Film *film,
                     DeviceScene *device_scene,
                     RenderScheduler &render_scheduler,
                     TileManager &tile_manager)
    : device_(device),
      denoise_device_(denoise_device),
      film_(film),
      device_scene_(device_scene),
      render_scheduler_(render_scheduler),
      tile_manager_(tile_manager)
{
  DCHECK_NE(device_, nullptr);

  {
    vector<DeviceInfo> cpu_devices;
    device_cpu_info(cpu_devices);

    cpu_device_ = device_cpu_create(
        cpu_devices[0], device->stats, device->profiler, device_->headless);
  }

  /* Create path tracing work in advance, so that it can be reused by incremental sampling as much
   * as possible. */
  device_->foreach_device([&](Device *path_trace_device) {
    unique_ptr<PathTraceWork> work = PathTraceWork::create(
        path_trace_device, film, device_scene, &render_cancel_.is_requested);
    if (work) {
      path_trace_works_.emplace_back(std::move(work));
    }
  });

  work_balance_infos_.resize(path_trace_works_.size());
  work_balance_do_initial(work_balance_infos_);

  render_scheduler.set_need_schedule_rebalance(path_trace_works_.size() > 1);
}

PathTrace::~PathTrace()
{
  destroy_gpu_resources();
}

void PathTrace::load_kernels()
{
  if (denoiser_) {
    /* Activate graphics interop while denoiser device is created, so that it can choose a device
     * that supports interop for faster display updates. */
    if (display_ && path_trace_works_.size() > 1) {
      display_->graphics_interop_activate();
    }

    denoiser_->load_kernels(progress_);

    if (display_ && path_trace_works_.size() > 1) {
      display_->graphics_interop_deactivate();
    }
  }
}

void PathTrace::alloc_work_memory()
{
  for (auto &path_trace_work : path_trace_works_) {
    path_trace_work->alloc_work_memory();
  }
}

bool PathTrace::ready_to_reset()
{
  /* The logic here is optimized for the best feedback in the viewport, which implies having a GPU
   * display. Of there is no such display, the logic here will break. */
  DCHECK(display_);

  /* The logic here tries to provide behavior which feels the most interactive feel to artists.
   * General idea is to be able to reset as quickly as possible, while still providing interactive
   * feel.
   *
   * If the render result was ever drawn after previous reset, consider that reset is now possible.
   * This way camera navigation gives the quickest feedback of rendered pixels, regardless of
   * whether CPU or GPU drawing pipeline is used.
   *
   * Consider reset happening after redraw "slow" enough to not clog anything. This is a bit
   * arbitrary, but seems to work very well with viewport navigation in Blender. */

  if (did_draw_after_reset_) {
    return true;
  }

  return false;
}

void PathTrace::reset(const BufferParams &full_params,
                      const BufferParams &big_tile_params,
                      const bool reset_rendering)
{
  if (big_tile_params_.modified(big_tile_params)) {
    big_tile_params_ = big_tile_params;
    render_state_.need_reset_params = true;
  }

  full_params_ = full_params;

  /* NOTE: GPU display checks for buffer modification and avoids unnecessary re-allocation.
   * It is requires to inform about reset whenever it happens, so that the redraw state tracking is
   * properly updated. */
  if (display_) {
    display_->reset(big_tile_params, reset_rendering);
  }

  render_state_.has_denoised_result = false;
  render_state_.tile_written = false;

  did_draw_after_reset_ = false;
}

void PathTrace::device_free()
{
  /* Free render buffers used by the path trace work to reduce memory peak. */
  BufferParams empty_params;
  empty_params.pass_stride = 0;
  empty_params.update_offset_stride();
  for (auto &&path_trace_work : path_trace_works_) {
    path_trace_work->get_render_buffers()->reset(empty_params);
  }
  render_state_.need_reset_params = true;
}

void PathTrace::set_progress(Progress *progress)
{
  progress_ = progress;
}

void PathTrace::render(const RenderWork &render_work)
{
  /* Indicate that rendering has started and that it can be requested to cancel. */
  {
    const thread_scoped_lock lock(render_cancel_.mutex);
    if (render_cancel_.is_requested) {
      return;
    }
    render_cancel_.is_rendering = true;
  }

  render_pipeline(render_work);

  /* Indicate that rendering has finished, making it so thread which requested `cancel()` can carry
   * on. */
  {
    const thread_scoped_lock lock(render_cancel_.mutex);
    render_cancel_.is_rendering = false;
    render_cancel_.condition.notify_one();
  }
}

void PathTrace::render_pipeline(RenderWork render_work)
{
  /* NOTE: Only check for "instant" cancel here. The user-requested cancel via progress is
   * checked in Session and the work in the event of cancel is to be finished here. */

  const bool deep_capture_trace = (deep_frame_consumer_ && deep_frame_consumer_->uses_bounded_processing()) &&
                                  getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_RESIDENT_CAPTURE_TRACE") !=
                                      nullptr;
  const double deep_pipeline_start = deep_capture_trace ? time_dt() : 0.0;
  double deep_stage_start = deep_pipeline_start;
  const auto trace_stage = [&](const char *stage) {
    if (deep_capture_trace) {
      const double now = time_dt();
      std::fprintf(stderr,
                   "DEEP_PIPELINE stage=%s total_ms=%.6f\n",
                   stage,
                   (now - deep_stage_start) * 1000);
      deep_stage_start = now;
    }
  };
  SCOPED_DEFER(if (deep_capture_trace) {
    std::fprintf(stderr,
                 "DEEP_PIPELINE stage=work_total total_ms=%.6f\n",
                 (time_dt() - deep_pipeline_start) * 1000);
  });

  render_scheduler_.set_need_schedule_cryptomatte(device_scene_->data.film.cryptomatte_passes !=
                                                  0);

  render_scheduler_.report_work_begin(render_work);

  init_render_buffers(render_work);
  trace_stage("init_buffers");

  rebalance(render_work);
  trace_stage("rebalance");
  /* Initialize kernel execution after deep buffer sync so that CPU thread-local kernel globals
   * have the updated deep buffer pointers. This is critical for mixed CPU/GPU rendering where
   * rebalance can change slice layouts and deep buffer allocations. Keep the matching deinit
   * scoped so early returns unwind cleanly. */
  render_init_kernel_execution();
  SCOPED_DEFER(render_deinit_kernel_execution());

  /* Reset sample limit. */
  render_scheduler_.set_limit_samples_per_update(0);

  /* Prepare all per-thread guiding structures before we start with the next rendering
   * iteration/progression. */
  const bool use_guiding = device_scene_->data.integrator.use_guiding;
  if (use_guiding) {
    guiding_prepare_structures();
  }

  const bool has_volume = device_scene_->data.integrator.use_volumes;
  if (has_volume) {
    const uint num_rendered_samples = render_scheduler_.get_num_rendered_samples();
    const uint limit = next_power_of_two(num_rendered_samples) - num_rendered_samples;
    render_scheduler_.set_limit_samples_per_update(limit);
  }

  trace_stage("prepare");
  const double deep_trace_start = deep_capture_trace ? time_dt() : 0.0;
  path_trace(render_work);
  if (deep_capture_trace) {
    std::fprintf(stderr, "DEEP_PATH_TRACE total_ms=%.6f\n", (time_dt() - deep_trace_start) * 1000);
  }
  trace_stage("path_trace");
  if (render_cancel_.is_requested) {
    return;
  }

  trace_stage("flush_deep");

  /* Update the guiding field using the training data/samples collected during the rendering
   * iteration/progression. */
  const bool train_guiding = device_scene_->data.integrator.train_guiding;
  if (use_guiding && train_guiding) {
    guiding_update_structures();
  }

  adaptive_sample(render_work);
  if (render_cancel_.is_requested) {
    return;
  }

  cryptomatte_postprocess(render_work);
  if (render_cancel_.is_requested) {
    return;
  }

  denoise(render_work);
  if (render_cancel_.is_requested) {
    return;
  }

  denoise_volume_guiding_buffers(render_work, has_volume);
  if (render_cancel_.is_requested) {
    return;
  }

  trace_stage("postprocess");
  write_tile_buffer(render_work);
  trace_stage("write_tile");
  update_display(render_work);

  progress_update_if_needed(render_work);

  trace_stage("display_progress");
  finalize_full_buffer_on_disk(render_work);
  trace_stage("finalize");
}

void PathTrace::render_init_kernel_execution()
{
  const bool multi_deep = deep_frame_consumer_ && deep_frame_consumer_->uses_gpu_processing() &&
                          path_trace_works_.size() > 1;
  int64_t window[4] = {};
  bool admit = false;
  if (multi_deep) {
    window[0] = int64_t(big_tile_params_.full_x) + big_tile_params_.window_x;
    window[1] = int64_t(big_tile_params_.full_y) + big_tile_params_.window_y;
    window[2] = big_tile_params_.window_width;
    window[3] = big_tile_params_.window_height;
    admit = !deep_aggregate_admitted_ ||
        deep_aggregate_resolution_ != render_state_.resolution_divider ||
        !std::equal(std::begin(window), std::end(window), std::begin(deep_aggregate_window_));
  }
  if (admit)
  {
    const auto devices = deep_bounded_admission_devices();
    const uint64_t pixels = uint64_t(big_tile_params_.window_width) * big_tile_params_.window_height;
    const auto &budget = deep_frame_consumer_->device_memory_budget();
    const auto policy = DeepBoundedPolicy(device_->deep_bounded_policy());
    const uint32_t families = deep_bounded_family_count(deep_frame_consumer_->bounded_consumers());
    const bool holdout = device_scene_->data.background.transparent != 0;
    const auto admitted = pixels < (1u << 30) ? deep_bounded_choose_devices(
        uint32_t(pixels), deep_frame_consumer_->bounded_max_samples(), policy, holdout,
        families, budget.quota() - budget.used(), devices) : DeepBoundedAdmission{};
    if (!admitted.roots) {
      progress_->set_error("Deep aggregate device budget cannot admit this Tile");
      return;
    }
    size_t remaining = budget.quota() - budget.used();
    /* First reserve every mandatory arena. Optional maps may consume only the
     * remaining shared and per-device physical capacity, never another Work's minimum. */
    for (const auto &device : devices) {
      DeepBoundedAdmission a;
      size_t cap;
      if (!DeepBoundedAdmission::calculate(uint32_t(pixels), admitted.packet_samples,
              device.native_states, policy, holdout, families, a, device.compact_identity) ||
          !deep_bounded_resident(a, device.retained, cap) ||
          cap < device.retained.known_total || cap - device.retained.known_total > remaining)
      {
        progress_->set_error("Deep aggregate arena reservation changed before initialization");
        return;
      }
      remaining -= cap - device.retained.known_total;
    }
    for (size_t i = 0; i < devices.size(); ++i) {
      DeepBoundedAdmission a;
      size_t cap;
      if (!DeepBoundedAdmission::calculate(uint32_t(pixels), admitted.packet_samples,
              devices[i].native_states, policy, holdout, families, a, devices[i].compact_identity) ||
          !deep_bounded_resident(a, devices[i].retained, cap))
      {
        progress_->set_error("Deep aggregate arena size overflow");
        return;
      }
      const auto &device = devices[i];
      const size_t growth = cap - device.retained.known_total;
      const size_t map_growth = device.adaptive_target > device.adaptive_bytes ?
          device.adaptive_target - device.adaptive_bytes : 0;
      const bool allow_map = growth <= device.physical_capacity &&
          map_growth <= device.physical_capacity - growth && map_growth <= remaining;
      const size_t auxiliary = allow_map ? map_growth : 0;
      remaining -= auxiliary;
      path_trace_works_[i]->set_deep_bounded_admission_limit(admitted.packet_samples, cap, auxiliary);
    }
  }
  for (auto &&path_trace_work : path_trace_works_) {
    path_trace_work->init_execution();
  }
  if (admit && !progress_->get_error()) {
    for (const auto &work : path_trace_works_) {
      if (work->get_device()->have_error()) return;
    }
    std::copy(std::begin(window), std::end(window), std::begin(deep_aggregate_window_));
    deep_aggregate_resolution_ = render_state_.resolution_divider;
    deep_aggregate_admitted_ = true;
    /* Every mandatory arena is now charged. Later slice rebalance may grow its
     * optional map using real remaining memory, without another owner's future
     * mandatory allocation to protect. Work.configure caches the same Tile. */
    for (auto &work : path_trace_works_)
      work->set_deep_bounded_admission_limit(UINT32_MAX, SIZE_MAX, SIZE_MAX);
  }
}

void PathTrace::render_deinit_kernel_execution()
{
  for (auto &&path_trace_work : path_trace_works_) {
    path_trace_work->deinit_execution();
  }
}

/* TODO(sergey): Look into `std::function` rather than using a template. Should not be a
 * measurable performance impact at runtime, but will make compilation faster and binary somewhat
 * smaller. */
template<typename Callback>
static void foreach_sliced_buffer_params(const vector<unique_ptr<PathTraceWork>> &path_trace_works,
                                         const vector<WorkBalanceInfo> &work_balance_infos,
                                         const BufferParams &buffer_params,
                                         const int overscan,
                                         const Callback &callback)
{
  const int num_works = path_trace_works.size();
  const int window_height = buffer_params.window_height;

  int current_y = 0;
  for (int i = 0; i < num_works; ++i) {
    const double weight = work_balance_infos[i].weight;
    const int slice_window_full_y = buffer_params.full_y + buffer_params.window_y + current_y;
    const int slice_window_height = max(lround(window_height * weight), 1);

    /* Disallow negative values to deal with situations when there are more compute devices than
     * scan-lines. */
    const int remaining_window_height = max(0, window_height - current_y);

    BufferParams slice_params = buffer_params;

    slice_params.full_y = max(slice_window_full_y - overscan, buffer_params.full_y);
    slice_params.window_y = slice_window_full_y - slice_params.full_y;

    if (i < num_works - 1) {
      slice_params.window_height = min(slice_window_height, remaining_window_height);
    }
    else {
      slice_params.window_height = remaining_window_height;
    }

    slice_params.height = slice_params.window_y + slice_params.window_height + overscan;
    slice_params.height = min(slice_params.height,
                              buffer_params.height + buffer_params.full_y - slice_params.full_y);

    slice_params.update_offset_stride();

    callback(path_trace_works[i].get(), slice_params);

    current_y += slice_params.window_height;
  }
}

void PathTrace::update_allocated_work_buffer_params()
{
  const int overscan = tile_manager_.get_tile_overscan();
  foreach_sliced_buffer_params(path_trace_works_,
                               work_balance_infos_,
                               big_tile_params_,
                               overscan,
                               [](PathTraceWork *path_trace_work, const BufferParams &params) {
                                 RenderBuffers *buffers = path_trace_work->get_render_buffers();
                                 buffers->reset(params);
                               });
}

static BufferParams scale_buffer_params(const BufferParams &params, const float resolution_divider)
{
  BufferParams scaled_params = params;

  scaled_params.width = max(1, int(params.width / resolution_divider));
  scaled_params.height = max(1, int(params.height / resolution_divider));

  scaled_params.window_x = int(params.window_x / resolution_divider);
  scaled_params.window_y = int(params.window_y / resolution_divider);
  scaled_params.window_width = max(1, int(params.window_width / resolution_divider));
  scaled_params.window_height = max(1, int(params.window_height / resolution_divider));

  scaled_params.full_x = int(params.full_x / resolution_divider);
  scaled_params.full_y = int(params.full_y / resolution_divider);
  scaled_params.full_width = max(1, int(params.full_width / resolution_divider));
  scaled_params.full_height = max(1, int(params.full_height / resolution_divider));

  scaled_params.update_offset_stride();

  return scaled_params;
}

void PathTrace::update_effective_work_buffer_params(const RenderWork &render_work)
{
  const float denoised_resolution_divider = render_work.denoised_resolution_divider;
  const float resolution_divider = render_work.resolution_divider / denoised_resolution_divider;

  const BufferParams denoised_big_tile_params = scale_buffer_params(big_tile_params_,
                                                                    denoised_resolution_divider);
  const BufferParams scaled_big_tile_params = scale_buffer_params(denoised_big_tile_params,
                                                                  resolution_divider);

  const int overscan = tile_manager_.get_tile_overscan();

  foreach_sliced_buffer_params(
      path_trace_works_,
      work_balance_infos_,
      denoised_big_tile_params,
      overscan,
      [&](PathTraceWork *path_trace_work, const BufferParams params) {
        /* Scale down the sliced buffer parameters again that were scaled by denoising upscale
         * factor above. This should match the values that would occur when slicing
         * 'scaled_big_tile_params' directly. */
        const BufferParams scaled_params = scale_buffer_params(params, resolution_divider);
        path_trace_work->set_effective_buffer_params(
            scaled_big_tile_params, scaled_params, denoised_big_tile_params, params);
      });

  render_state_.effective_big_tile_params = scaled_big_tile_params;
  render_state_.effective_denoised_big_tile_params = denoised_big_tile_params;
}

void PathTrace::update_work_buffer_params_if_needed(const RenderWork &render_work)
{
  if (render_state_.need_reset_params) {
    update_allocated_work_buffer_params();
  }

  if (render_state_.need_reset_params ||
      render_state_.resolution_divider != render_work.resolution_divider)
  {
    update_effective_work_buffer_params(render_work);
  }

  render_state_.resolution_divider = render_work.resolution_divider;
  render_state_.need_reset_params = false;
}

void PathTrace::init_render_buffers(const RenderWork &render_work)
{
  update_work_buffer_params_if_needed(render_work);

  /* Handle initialization scheduled by the render scheduler. */
  if (render_work.init_render_buffers) {
    parallel_for_each(path_trace_works_, [&](unique_ptr<PathTraceWork> &path_trace_work) {
      path_trace_work->zero_render_buffers();
    });

    tile_buffer_read();
  }
}

void PathTrace::path_trace(RenderWork &render_work)
{
  if (!render_work.path_trace.num_samples) {
    return;
  }

  LOG_DEBUG << "Will path trace " << render_work.path_trace.num_samples
            << " samples at the resolution divider " << render_work.resolution_divider;

  const double start_time = time_dt();

  const int num_works = path_trace_works_.size();

  thread_capture_fp_settings();

  parallel_for(0, num_works, [&](int i) {
    const double work_start_time = time_dt();
    const int num_samples = render_work.path_trace.num_samples;

    PathTraceWork *path_trace_work = path_trace_works_[i].get();
    if (path_trace_work->get_device()->have_error()) {
      return;
    }

    path_trace_work->set_progress(progress_);
    PathTraceWork::RenderStatistics statistics;
    path_trace_work->render_samples(statistics,
                                    render_work.path_trace.start_sample,
                                    num_samples,
                                    render_work.path_trace.sample_offset);

    DCHECK(isfinite(statistics.occupancy));

    const double work_time = time_dt() - work_start_time;
    work_balance_infos_[i].time_spent += work_time;
    work_balance_infos_[i].occupancy = statistics.occupancy;

    LOG_INFO << "Rendered " << num_samples << " samples in " << work_time << " seconds ("
             << work_time / num_samples
             << " seconds per sample), occupancy: " << statistics.occupancy;
  });

  /* render_samples may retire the preceding Tile's copy to reuse scratch.
   * Publish only here, after all device workers joined; no shared writes inside
   * their retirement path and no new DMA wait at this checkpoint. */
  if (deep_frame_consumer_ && deep_frame_consumer_->uses_gpu_processing() &&
      !is_cancel_requested()) {
    for (auto &work : path_trace_works_) {
      if (!work->publish_ready_deep_tiles()) {
        progress_->set_error("Bounded Deep owner publication failed");
        return;
      }
    }
  }

  float occupancy_accum = 0.0f;
  for (const WorkBalanceInfo &balance_info : work_balance_infos_) {
    occupancy_accum += balance_info.occupancy;
  }
  const float occupancy = occupancy_accum / num_works;
  render_scheduler_.report_path_trace_occupancy(render_work, occupancy);

  render_scheduler_.report_path_trace_time(
      render_work, time_dt() - start_time, is_cancel_requested());
}

void PathTrace::adaptive_sample(RenderWork &render_work)
{
  if (!render_work.adaptive_sampling.filter) {
    return;
  }

  bool did_reschedule_on_idle = false;

  while (true) {
    LOG_DEBUG << "Will filter adaptive stopping buffer, threshold "
              << render_work.adaptive_sampling.threshold;
    if (render_work.adaptive_sampling.reset) {
      LOG_DEBUG << "Will re-calculate convergency flag for currently converged pixels.";
    }

    const double start_time = time_dt();

    uint num_active_pixels = 0;
    parallel_for_each(path_trace_works_, [&](unique_ptr<PathTraceWork> &path_trace_work) {
      const uint num_active_pixels_in_work =
          path_trace_work->adaptive_sampling_converge_filter_count_active(
              render_work.adaptive_sampling.threshold, render_work.adaptive_sampling.reset);
      if (num_active_pixels_in_work) {
        atomic_add_and_fetch_u(&num_active_pixels, num_active_pixels_in_work);
      }
    });

    render_scheduler_.report_adaptive_filter_time(
        render_work, time_dt() - start_time, is_cancel_requested());

    if (num_active_pixels == 0) {
      LOG_DEBUG << "All pixels converged.";
      if (!render_scheduler_.render_work_reschedule_on_converge(render_work)) {
        break;
      }
      LOG_DEBUG << "Continuing with lower threshold.";
    }
    else if (did_reschedule_on_idle) {
      break;
    }
    else if (num_active_pixels < 128 * 128) {
      /* NOTE: The hardcoded value of 128^2 is more of an empirical value to keep GPU busy so that
       * there is no performance loss from the progressive noise floor feature.
       *
       * A better heuristic is possible here: for example, use maximum of 128^2 and percentage of
       * the final resolution. */
      if (!render_scheduler_.render_work_reschedule_on_idle(render_work)) {
        LOG_DEBUG << "Rescheduling is not possible: final threshold is reached.";
        break;
      }
      LOG_DEBUG << "Rescheduling lower threshold.";
      did_reschedule_on_idle = true;
    }
    else {
      break;
    }
  }
}

void PathTrace::set_denoiser_params(const DenoiseParams &params)
{
  if (!params.use) {
    denoiser_.reset();
    render_scheduler_.set_denoiser_params(params);
    return;
  }

  GraphicsInteropDevice interop_device;
  if (display_) {
    interop_device = display_->graphics_interop_get_device();
  }

  Device *effective_denoise_device;
  Device *cpu_fallback_device = cpu_device_.get();
  const DenoiseParams effective_denoise_params = get_effective_denoise_params(
      denoise_device_, cpu_fallback_device, params, interop_device, effective_denoise_device);

  bool need_to_recreate_denoiser = false;
  if (denoiser_) {
    const DenoiseParams old_denoiser_params = denoiser_->get_params();

    const bool is_cpu_denoising = old_denoiser_params.type == DENOISER_OPENIMAGEDENOISE &&
                                  old_denoiser_params.use_gpu == false;
    const bool requested_gpu_denoising = effective_denoise_params.type == DENOISER_OPTIX ||
                                         (effective_denoise_params.type ==
                                              DENOISER_OPENIMAGEDENOISE &&
                                          effective_denoise_params.use_gpu == true);
    if (requested_gpu_denoising && is_cpu_denoising &&
        effective_denoise_device->info.type == DEVICE_CPU)
    {
      /* It won't be possible to use GPU denoising when according to user settings we have
       * only CPU as available denoising device. So we just exiting early to avoid
       * unnecessary denoiser recreation or parameters update. */
      return;
    }

    const bool is_same_denoising_device_type = old_denoiser_params.use_gpu ==
                                               effective_denoise_params.use_gpu;
    /* Optix Denoiser is not supporting CPU devices, so use_gpu option is not
     * shown in the UI and changes in the option value should not be checked. */
    if (old_denoiser_params.type == effective_denoise_params.type &&
        (is_same_denoising_device_type || effective_denoise_params.type == DENOISER_OPTIX))
    {
      denoiser_->set_params(effective_denoise_params);
    }
    else {
      need_to_recreate_denoiser = true;
    }
  }
  else {
    /* if there is no denoiser and param.use is true, then we need to create it. */
    need_to_recreate_denoiser = true;
  }

  if (need_to_recreate_denoiser) {
    denoiser_ = Denoiser::create(
        effective_denoise_device, cpu_fallback_device, effective_denoise_params, interop_device);

    if (denoiser_) {
      /* Only take into account the "immediate" cancel to have interactive rendering responding to
       * navigation as quickly as possible, but allow to run denoiser after user hit Escape key
       * while doing offline rendering. */
      denoiser_->is_cancelled_cb = [this]() { return render_cancel_.is_requested; };
    }
  }

  /* Use actual parameters, if available */
  if (denoise_device_ && denoiser_) {
    render_scheduler_.set_denoiser_params(denoiser_->get_params());
  }
  else {
    render_scheduler_.set_denoiser_params(effective_denoise_params);
  }
}

void PathTrace::set_adaptive_sampling(const AdaptiveSampling &adaptive_sampling)
{
  render_scheduler_.set_adaptive_sampling(adaptive_sampling);
}

void PathTrace::cryptomatte_postprocess(const RenderWork &render_work)
{
  if (!render_work.cryptomatte.postprocess) {
    return;
  }
  LOG_DEBUG << "Perform cryptomatte work.";

  parallel_for_each(path_trace_works_, [&](unique_ptr<PathTraceWork> &path_trace_work) {
    path_trace_work->cryptomatte_postproces();
  });
}

void PathTrace::denoise(const RenderWork &render_work)
{
  if (!render_work.tile.denoise) {
    return;
  }

  if (!denoiser_) {
    /* Denoiser was not configured, so nothing to do here. */
    return;
  }

  LOG_DEBUG << "Perform denoising work.";

  const double start_time = time_dt();

  RenderBuffers *buffer_to_denoise = nullptr;
  bool allow_inplace_modification = false;

  Device *denoiser_device = denoiser_->get_denoiser_device();
  if (path_trace_works_.size() > 1 && denoiser_device && !big_tile_denoise_work_) {
    big_tile_denoise_work_ = PathTraceWork::create(denoiser_device, film_, device_scene_, nullptr);
  }

  if (big_tile_denoise_work_) {
    big_tile_denoise_work_->set_effective_buffer_params(
        render_state_.effective_big_tile_params,
        render_state_.effective_big_tile_params,
        render_state_.effective_denoised_big_tile_params,
        render_state_.effective_denoised_big_tile_params);

    buffer_to_denoise = big_tile_denoise_work_->get_render_buffers();
    buffer_to_denoise->reset(render_state_.effective_denoised_big_tile_params);

    copy_to_render_buffers(buffer_to_denoise);

    allow_inplace_modification = true;
  }
  else {
    DCHECK_EQ(path_trace_works_.size(), 1);

    buffer_to_denoise = path_trace_works_.front()->get_render_buffers();
  }

  if (denoiser_->denoise_buffer(render_state_.effective_big_tile_params,
                                render_state_.effective_denoised_big_tile_params,
                                buffer_to_denoise,
                                get_num_samples_in_buffer(),
                                allow_inplace_modification,
                                device_scene_->data.integrator.pixel_jitter))
  {
    render_state_.has_denoised_result = true;
  }

  render_scheduler_.report_denoise_time(render_work, time_dt() - start_time);
}

void PathTrace::denoise_volume_guiding_buffers(const RenderWork &render_work,
                                               const bool has_volume)
{
  if (!has_volume || !render_work.volume_guiding_denoise) {
    return;
  }

  LOG_DEBUG << "Denoise volume guiding buffers.";

  const double start_time = time_dt();

  /* TODO: in the multi-GPU case, we can denoise on one device and copy to the rest, instead of
   * denoising on each device separately. */
  parallel_for_each(path_trace_works_, [&](unique_ptr<PathTraceWork> &path_trace_work) {
    path_trace_work->denoise_volume_guiding_buffers();
  });

  render_scheduler_.report_volume_guiding_denoise_time(render_work, time_dt() - start_time);
}

void PathTrace::set_output_driver(unique_ptr<OutputDriver> driver)
{
  output_driver_ = std::move(driver);
}

void PathTrace::set_display_driver(unique_ptr<DisplayDriver> driver)
{
  /* The display driver is the source of the drawing context which might be used by
   * path trace works. Make sure there is no graphics interop using resources from
   * the old display, as it might no longer be available after this call. */
  destroy_gpu_resources();

  if (driver) {
    display_ = make_unique<PathTraceDisplay>(std::move(driver));
  }
  else {
    display_ = nullptr;
  }
}


bool PathTrace::set_deep_frame_capture_consumer(DeepFrameCaptureConsumer *consumer)
{
  deep_aggregate_admitted_ = false;
  if (consumer && ((!consumer->uses_bounded_processing() && path_trace_works_.size() != 1) ||
                   path_trace_works_.empty()))
  {
    progress_->set_error("Deep Frame requires compatible native work");
    return false;
  }
  if (!consumer && deep_frame_consumer_ && deep_frame_consumer_->uses_bounded_processing() &&
      !progress_->get_error() && !progress_->get_cancel())
  {
    for (auto &work : path_trace_works_) {
      if (!work->drain_deep_tiles()) {
        const string error = deep_frame_consumer_->capture_error_message();
        progress_->set_error(error.empty() ? "Bounded Deep final readback failed" : error);
        break;
      }
    }
  }
  for (auto &work : path_trace_works_) {
    if (!work->set_deep_frame_capture_consumer(consumer)) {
      for (auto &bound : path_trace_works_) {
        bound->set_deep_frame_capture_consumer(nullptr);
      }
      deep_frame_consumer_ = nullptr;
      progress_->set_error("Native work rejected Deep Frame capture");
      return false;
    }
  }
  deep_frame_consumer_ = consumer;
  bounded_native_film_.reset();
  retain_bounded_native_film_ = false;
  return true;
}

vector<DeepBoundedDeviceAdmission> PathTrace::deep_bounded_admission_devices() const
{
  vector<DeepBoundedDeviceAdmission> result;
  if (path_trace_works_.size() > 1) {
    result.reserve(path_trace_works_.size());
    for (const auto &work : path_trace_works_)
      result.push_back(work->deep_bounded_admission_snapshot());
  }
  return result;
}

int2 PathTrace::deep_bounded_camera_limits() const
{
  int2 limits = make_int2(INT_MAX, 0);
  for (const auto &work : path_trace_works_) {
    const int2 local = work->deep_bounded_camera_limits();
    limits.x = std::min(limits.x, local.x);
    limits.y = std::max(limits.y, local.y);
  }
  return limits;
}

void PathTrace::zero_display()
{
  if (display_) {
    display_->zero();
  }
}

void PathTrace::draw()
{
  if (!display_) {
    return;
  }

  did_draw_after_reset_ |= display_->draw();
}

void PathTrace::flush_display()
{
  if (!display_) {
    return;
  }

  display_->flush();
}

void PathTrace::update_display(const RenderWork &render_work)
{
  if (!render_work.display.update) {
    return;
  }

  if (!display_ && !output_driver_) {
    LOG_DEBUG << "Ignore display update.";
    return;
  }

  if (full_params_.width == 0 || full_params_.height == 0) {
    LOG_DEBUG << "Skipping PathTraceDisplay update due to 0 size of the render buffer.";
    return;
  }

  const double start_time = time_dt();

  if (output_driver_) {
    LOG_DEBUG << "Invoke buffer update callback.";

    const PathTraceTile tile(*this);
    output_driver_->update_render_tile(tile);
  }

  if (display_) {
    LOG_DEBUG << "Perform copy to GPUDisplay work.";

    const PassType pass_type = film_->get_display_pass();
    const bool show_denoised =
        ((render_work.display.use_denoised_result && has_denoised_result() &&
          big_tile_params_.get_pass_offset(pass_type, PassMode::DENOISED) != PASS_UNUSED) ||
         is_volume_guiding_pass(pass_type));

    const int texture_width = show_denoised ?
                                  render_state_.effective_denoised_big_tile_params.window_width :
                                  render_state_.effective_big_tile_params.window_width;
    const int texture_height = show_denoised ?
                                   render_state_.effective_denoised_big_tile_params.window_height :
                                   render_state_.effective_big_tile_params.window_height;
    if (!display_->update_begin(texture_width, texture_height)) {
      LOG_ERROR << "Error beginning GPUDisplay update.";
      return;
    }

    const PassMode pass_mode = show_denoised ? PassMode::DENOISED : PassMode::NOISY;

    /* TODO(sergey): When using multi-device rendering map the GPUDisplay once and copy data from
     * all works in parallel. */
    const int num_samples = get_num_samples_in_buffer();
    if (big_tile_denoise_work_ && render_state_.has_denoised_result) {
      big_tile_denoise_work_->copy_to_display(display_.get(), pass_mode, num_samples);
    }
    else {
      for (auto &&path_trace_work : path_trace_works_) {
        path_trace_work->copy_to_display(display_.get(), pass_mode, num_samples);
      }
    }

    display_->update_end();
  }

  render_scheduler_.report_display_update_time(render_work, time_dt() - start_time);
}

void PathTrace::rebalance(const RenderWork &render_work)
{
  if (!render_work.rebalance) {
    return;
  }

  const int num_works = path_trace_works_.size();

  if (num_works == 1) {
    LOG_DEBUG << "Ignoring rebalance work due to single device render.";
    return;
  }

  const double start_time = time_dt();

  if (LOG_IS_ON(LOG_LEVEL_DEBUG)) {
    LOG_DEBUG << "Perform rebalance work.";
    LOG_DEBUG << "Per-device path tracing time (seconds):";
    for (int i = 0; i < num_works; ++i) {
      LOG_DEBUG << path_trace_works_[i]->get_device()->info.description << ": "
                << work_balance_infos_[i].time_spent;
    }
  }

  const bool did_rebalance = work_balance_do_rebalance(work_balance_infos_);

  if (LOG_IS_ON(LOG_LEVEL_DEBUG)) {
    LOG_DEBUG << "Calculated per-device weights for works:";
    for (int i = 0; i < num_works; ++i) {
      LOG_DEBUG << path_trace_works_[i]->get_device()->info.description << ": "
                << work_balance_infos_[i].weight;
    }
  }

  if (!did_rebalance) {
    LOG_DEBUG << "Balance in path trace works did not change.";
    render_scheduler_.report_rebalance_time(render_work, time_dt() - start_time, false);
    return;
  }

  vector<BufferParams> old_slices;
  if (deep_frame_consumer_ && deep_frame_consumer_->uses_bounded_processing() &&
      render_scheduler_.get_num_rendered_samples() > render_work.path_trace.num_samples)
  {
    old_slices.reserve(num_works);
    for (const auto &work : path_trace_works_) {
      old_slices.push_back(work->get_effective_buffer_params());
    }
  }
  RenderBuffers big_tile_cpu_buffers(cpu_device_.get());
  big_tile_cpu_buffers.reset(render_state_.effective_big_tile_params);

  copy_to_render_buffers(&big_tile_cpu_buffers);

  render_state_.need_reset_params = true;
  update_work_buffer_params_if_needed(render_work);

  for (size_t source = 0; source < old_slices.size(); ++source) {
    const auto &old = old_slices[source];
    const int old_y = old.full_y + old.window_y;
    for (size_t destination = 0; destination < old_slices.size(); ++destination) {
      if (source == destination) {
        continue;
      }
      const auto &now = path_trace_works_[destination]->get_effective_buffer_params();
      const int new_y = now.full_y + now.window_y;
      const int y = std::max(old_y, new_y);
      const int end = std::min(old_y + old.window_height, new_y + now.window_height);
      if (end > y && !path_trace_works_[source]->migrate_bounded_deep(
                         *path_trace_works_[destination], now.full_x + now.window_x, y,
                         now.window_width, end - y))
      {
        progress_->set_error("Bounded Deep collector migration failed");
        return;
      }
    }
  }

  copy_from_render_buffers(&big_tile_cpu_buffers);

  render_scheduler_.report_rebalance_time(render_work, time_dt() - start_time, true);
}


void PathTrace::write_tile_buffer(const RenderWork &render_work)
{
  if (!render_work.tile.write) {
    return;
  }

  LOG_DEBUG << "Write tile result.";

  render_state_.tile_written = true;

  const bool has_multiple_tiles = tile_manager_.has_multiple_tiles();

  if (deep_frame_consumer_ && deep_frame_consumer_->uses_bounded_processing() &&
      !deep_frame_consumer_->capture_cancelled())
  {
    if (std::any_of(path_trace_works_.begin(), path_trace_works_.end(), [](const auto &work) {
          return !work->close_deep_tile();
        }))
    {
      if (render_cancel_.is_requested || progress_->get_cancel()) return;
      const string message = deep_frame_consumer_->capture_error_message();
      progress_->set_error(message.empty() ? "Deep final tile processing failed" : message);
      return;
    }
  }
  if (deep_frame_consumer_ && !deep_frame_consumer_->uses_bounded_processing() &&
      !deep_frame_consumer_->capture_cancelled())
  {
    if (!copy_render_tile_from_device()) {
      progress_->set_error("Failed to copy raw final Deep film counts");
      return;
    }
    RenderBuffers *buffers = path_trace_works_.front()->get_render_buffers();
    const BufferParams &p = buffers->params;
    const int offset = p.get_pass_offset(PASS_SAMPLE_COUNT);
    const char *raw_option = getenv("CYCLES_EXPERIMENTAL_DEEP_FRAME_VERIFY_RAW_COMBINED");
    const bool raw_witness = raw_option && strcmp(raw_option, "1") == 0;
    const int combined_offset = p.get_pass_offset(PASS_COMBINED);
    const int2 size = make_int2(p.window_width, p.window_height);
    const int2 origin = make_int2(p.full_x + p.window_x, p.full_y + p.window_y);
    try {
      std::vector<uint32_t, DeepBudgetAllocator<uint32_t>> counts(
          size_t(size.x) * size.y,
          0,
          DeepBudgetAllocator<uint32_t>(deep_frame_consumer_->memory_budget()));
      if (offset == PASS_UNUSED || p.stride <= 0 || p.pass_stride <= 0 ||
          (raw_witness && combined_offset == PASS_UNUSED) ||
          buffers->buffer.size() < size_t(p.stride) * p.height * p.pass_stride)
      {
        progress_->set_error("Missing raw SampleCount storage for final Deep tile");
        return;
      }
      for (int y = 0; y < p.window_height; y++) {
        for (int x = 0; x < p.window_width; x++) {
          const int lx = p.window_x + x, ly = p.window_y + y;
          const int dx = p.full_x + lx - origin.x, dy = p.full_y + ly - origin.y;
          if (lx < 0 || ly < 0 || lx >= p.width || ly >= p.height || dx < 0 || dy < 0 ||
              dx >= size.x || dy >= size.y)
          {
            progress_->set_error("Final Deep tile film layout mismatch");
            return;
          }
          const float *pixel = buffers->buffer.data() +
                               (size_t(ly) * p.stride + lx) * p.pass_stride;
          counts[size_t(dy) * size.x + dx] = pass_float_as_uint(pixel[offset]);
          if (raw_witness) {
            /* Validation-only independent raw Film witness. Blender's displayed
             * Combined may redirect to catcher matte. This value never enters
             * source capture, curve reduction, clamp or encoding. */
            const uint32_t population = counts[size_t(dy) * size.x + dx];
            const float scale = population ? 1.0f / float(population) : 0;
            std::fprintf(
                stderr,
                "DEEP_FRAME_RAW_COMBINED x=%d y=%d samples=%u r=%.9g g=%.9g b=%.9g "
                "a=%.9g\n",
                origin.x + dx,
                origin.y + dy,
                population,
                double(pixel[combined_offset] * scale * p.exposure),
                double(pixel[combined_offset + 1] * scale * p.exposure),
                double(pixel[combined_offset + 2] * scale * p.exposure),
                double(combined_transparency_to_alpha(pixel[combined_offset + 3] * scale)));
          }
        }
      }
      auto *reference = deep_frame_consumer_->reference_capture();
      if (!reference || !reference->close_native_tile(
                            origin.x, origin.y, size.x, size.y, counts, p.exposure))
      {
        const string message = deep_frame_consumer_->capture_error_message();
        progress_->set_error(message.empty() ?
                                 "Deep Frame final tile population/curve validation failed" :
                                 message);
        return;
      }
    }
    catch (const std::bad_alloc &) {
      progress_->set_error("Deep Frame raw film count quota exhausted");
      return;
    }
  }

  if (getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_VOLUME_PROFILE") &&
      getenv("CYCLES_EXPERIMENTAL_DEEP_GPU_RESIDENT_CAPTURE_TRACE"))
  {
    std::fprintf(
        stderr, "DEEP_GPU_PROFILE_ROUTE write_tile multiple=%d\n", int(has_multiple_tiles));
  }


  /* Write render tile result, but only if not using tiled rendering.
   *
   * Tiles are written to a file during rendering, and written to the software at the end
   * of rendering (wither when all tiles are finished, or when rendering was requested to be
   * canceled).
   *
   * Important thing is: tile should be written to the software via callback only once. */
  if (!has_multiple_tiles) {
    LOG_DEBUG << "Write tile result via buffer write callback.";
    const bool simple_beauty = deep_frame_consumer_ &&
                               deep_frame_consumer_->uses_bounded_processing() &&
                               deep_frame_consumer_->capture_demand().simple_mode &&
                               deep_frame_consumer_->capture_demand().needs_rgb;
    if (simple_beauty) {
      if (!copy_render_tile_from_device()) {
        progress_->set_error("Cannot share native Film with bounded Deep");
        return;
      }
      for (const auto &work : path_trace_works_) {
        if (!work->set_bounded_deep_beauty(*work->get_render_buffers())) {
          progress_->set_error("Cannot attach native Film Beauty to bounded Deep");
          return;
        }
      }
    }
    tile_buffer_write(simple_beauty);
  }
  /* Write tile to disk, so that the render work's render buffer can be re-used for the next tile.
   */
  else {
    LOG_DEBUG << "Write tile result to disk.";
    tile_buffer_write_to_disk();
  }
}

void PathTrace::finalize_full_buffer_on_disk(const RenderWork &render_work)
{
  if (!render_work.full.write) {
    return;
  }

  LOG_DEBUG << "Handle full-frame render buffer work.";

  if (bounded_native_film_) {
    auto film = std::move(bounded_native_film_);
    if (progress_->get_cancel() || progress_->get_error()) {
      return;
    }
    if (film->written_pixels != uint64_t(full_params_.width) * full_params_.height) {
      progress_->set_error("Bounded Deep native Film has incomplete tile coverage");
      return;
    }
    auto &params = film->buffer.params;
    params.window_x = params.window_y = 0;
    params.window_width = params.width;
    params.window_height = params.height;
    full_frame_state_.render_buffers = &film->buffer;
    SCOPED_DEFER(full_frame_state_.render_buffers = nullptr);
    render_state_.has_denoised_result = false;
    tile_buffer_write(true);
    return;
  }

  if (!tile_manager_.has_written_tiles()) {
    LOG_DEBUG << "No tiles on disk.";
    return;
  }

  /* Make sure writing to the file is fully finished.
   * This will include writing all possible missing tiles, ensuring validness of the file. */
  tile_manager_.finish_write_tiles();

  /* NOTE: The rest of full-frame post-processing (such as full-frame denoising) will be done after
   * all scenes and layers are rendered by the Session (which happens after freeing Session memory,
   * so that we never hold scene and full-frame buffer in memory at the same time). */
}

void PathTrace::cancel()
{
  thread_scoped_lock lock(render_cancel_.mutex);

  render_cancel_.is_requested = true;

  while (render_cancel_.is_rendering) {
    render_cancel_.condition.wait(lock);
  }

  render_cancel_.is_requested = false;
}

int PathTrace::get_num_samples_in_buffer()
{
  return render_scheduler_.get_num_rendered_samples();
}

bool PathTrace::is_cancel_requested()
{
  if (render_cancel_.is_requested) {
    return true;
  }

  if (progress_ != nullptr) {
    if (progress_->get_cancel()) {
      return true;
    }
  }

  return false;
}

void PathTrace::tile_buffer_write(const bool copied_from_device)
{
  if (!output_driver_) {
    return;
  }

  const PathTraceTile tile(*this, copied_from_device);
  output_driver_->write_render_tile(tile);
}

void PathTrace::tile_buffer_read()
{
  if (!device_scene_->data.bake.use) {
    return;
  }

  if (!output_driver_) {
    return;
  }

  /* Read buffers back from device. */
  parallel_for_each(path_trace_works_, [&](unique_ptr<PathTraceWork> &path_trace_work) {
    path_trace_work->copy_render_buffers_from_device();
  });

  /* Read (subset of) passes from output driver. */
  const PathTraceTile tile(*this);
  if (output_driver_->read_render_tile(tile)) {
    /* Copy buffers to device again. */
    parallel_for_each(path_trace_works_, [](unique_ptr<PathTraceWork> &path_trace_work) {
      path_trace_work->copy_render_buffers_to_device();
    });
  }
}


void PathTrace::tile_buffer_write_to_disk()
{
  /* Sample count pass is required to support per-tile partial results stored in the file. */
  DCHECK_NE(big_tile_params_.get_pass_offset(PASS_SAMPLE_COUNT), PASS_UNUSED);

  const int num_rendered_samples = render_scheduler_.get_num_rendered_samples();

  if (num_rendered_samples == 0) {
    /* The tile has zero samples, no need to write it. */
    return;
  }

  /* Get access to the CPU-side render buffers of the current big tile. */
  RenderBuffers *buffers;
  RenderBuffers big_tile_cpu_buffers(cpu_device_.get());

  if (path_trace_works_.size() == 1) {
    path_trace_works_[0]->copy_render_buffers_from_device();
    buffers = path_trace_works_[0]->get_render_buffers();
  }
  else {
    big_tile_cpu_buffers.reset(render_state_.effective_big_tile_params);
    copy_to_render_buffers(&big_tile_cpu_buffers);

    buffers = &big_tile_cpu_buffers;
  }

  if (deep_frame_consumer_ && deep_frame_consumer_->uses_bounded_processing() &&
      deep_frame_consumer_->capture_demand().simple_mode &&
      deep_frame_consumer_->capture_demand().needs_rgb)
  {
    for (const auto &work : path_trace_works_) {
      if (!work->set_bounded_deep_beauty(*buffers)) {
        progress_->set_error("Cannot share tiled native Film Beauty with bounded Deep");
        return;
      }
    }
  }
  if (retain_bounded_native_film_ && !(denoiser_ && denoiser_->get_params().use)) {
    if (!stage_bounded_native_film(*buffers)) {
      progress_->set_error("Failed to stage bounded Deep native Film");
    }
    return;
  }
  if (!tile_manager_.write_tile(*buffers)) {
    device_->set_error("Error writing tile to file");
  }
}

bool PathTrace::stage_bounded_native_film(const RenderBuffers &tile)
{
  const auto &src = tile.params;
  const auto &dst = full_params_;
  const int64_t x = int64_t(src.full_x) + src.window_x - dst.full_x;
  const int64_t y = int64_t(src.full_y) + src.window_y - dst.full_y;
  if (x < 0 || y < 0 || src.window_x < 0 || src.window_y < 0 ||
      src.window_width <= 0 || src.window_height <= 0 ||
      x + src.window_width > dst.width || y + src.window_height > dst.height ||
      src.window_x + src.window_width > src.width || src.window_y + src.window_height > src.height ||
      src.pass_stride <= 0 || src.pass_stride != dst.pass_stride ||
      !tile.buffer.data() ||
      tile.buffer.size() < size_t(src.width) * src.height * src.pass_stride ||
      dst.width > INT_MAX / dst.pass_stride)
  {
    return false;
  }

  try {
    if (!bounded_native_film_) {
      const size_t bytes = size_t(dst.width) * dst.height * dst.pass_stride * sizeof(float);
      auto film = make_unique<BoundedNativeFilm>(
          *cpu_device_, deep_frame_consumer_->memory_budget(), bytes);
      film->buffer.reset(dst);
      bounded_native_film_ = std::move(film);
    }
    auto &film = *bounded_native_film_;
    const size_t dst_stride = size_t(dst.width) * dst.pass_stride;
    const size_t src_stride = size_t(src.width) * src.pass_stride;
    const size_t row = size_t(src.window_width) * src.pass_stride;
    const float *input = tile.buffer.data() + size_t(src.window_y) * src_stride +
                         size_t(src.window_x) * src.pass_stride;
    float *output = film.buffer.buffer.data() + size_t(y) * dst_stride +
                    size_t(x) * dst.pass_stride;
    for (int j = 0; j < src.window_height; ++j) {
      std::memcpy(output + size_t(j) * dst_stride, input + size_t(j) * src_stride,
                  row * sizeof(float));
    }
    film.written_pixels += uint64_t(src.window_width) * src.window_height;
    return true;
  }
  catch (const std::bad_alloc &) {
    return false;
  }
}

void PathTrace::progress_update_if_needed(const RenderWork &render_work)
{
  if (progress_ != nullptr) {
    const int2 tile_size = get_render_tile_size();
    const uint64_t num_samples_added = uint64_t(tile_size.x) * tile_size.y *
                                       render_work.path_trace.num_samples;
    const int current_sample = render_work.path_trace.start_sample +
                               render_work.path_trace.num_samples -
                               render_work.path_trace.sample_offset;
    progress_->add_samples(num_samples_added, current_sample);
  }

  if (progress_update_cb) {
    progress_update_cb();
  }
}

void PathTrace::progress_set_status(const string &status, const string &substatus)
{
  if (progress_ != nullptr) {
    progress_->set_status(status, substatus);
  }
}

void PathTrace::copy_to_render_buffers(RenderBuffers *render_buffers)
{
  parallel_for_each(path_trace_works_,
                    [&render_buffers](unique_ptr<PathTraceWork> &path_trace_work) {
                      path_trace_work->copy_to_render_buffers(render_buffers);
                    });
  render_buffers->copy_to_device();
}

void PathTrace::copy_from_render_buffers(RenderBuffers *render_buffers)
{
  render_buffers->copy_from_device();
  parallel_for_each(path_trace_works_,
                    [&render_buffers](unique_ptr<PathTraceWork> &path_trace_work) {
                      path_trace_work->copy_from_render_buffers(render_buffers);
                    });
}

bool PathTrace::copy_render_tile_from_device()
{
  if (full_frame_state_.render_buffers) {
    /* Full-frame buffer is always allocated on CPU. */
    return true;
  }

  if (big_tile_denoise_work_ && render_state_.has_denoised_result) {
    return big_tile_denoise_work_->copy_render_buffers_from_device();
  }

  bool success = true;

  parallel_for_each(path_trace_works_, [&](unique_ptr<PathTraceWork> &path_trace_work) {
    if (!success) {
      return;
    }
    if (!path_trace_work->copy_render_buffers_from_device()) {
      success = false;
    }
  });

  return success;
}

static string get_layer_view_name(const RenderBuffers &buffers)
{
  string result;

  if (!buffers.params.layer.empty()) {
    result += string(buffers.params.layer);
  }

  if (!buffers.params.view.empty()) {
    if (!result.empty()) {
      result += ", ";
    }
    result += string(buffers.params.view);
  }

  return result;
}

void PathTrace::process_full_buffer_from_disk(string_view filename,
                                              DeepFrameCaptureConsumer *beauty_consumer)
{
  LOG_DEBUG << "Processing full frame buffer file " << filename;

  progress_set_status("Reading full buffer from disk");

  RenderBuffers full_frame_buffers(cpu_device_.get());

  DenoiseParams denoise_params;
  if (!tile_manager_.read_full_buffer_from_disk(filename, &full_frame_buffers, &denoise_params)) {
    const string error_message = "Error reading tiles from file";
    if (progress_) {
      progress_->set_error(error_message);
      progress_->set_cancel(error_message);
    }
    else {
      LOG_ERROR << error_message;
    }
    return;
  }

  const string layer_view_name = get_layer_view_name(full_frame_buffers);

  render_state_.has_denoised_result = false;

  if (denoise_params.use && denoiser_ && !progress_->get_cancel()) {
    progress_set_status(layer_view_name, "Denoising");

    /* If GPU should be used is not based on file metadata. */
    denoise_params.use_gpu = render_scheduler_.is_denoiser_gpu_used();

    /* Re-use the denoiser as much as possible, avoiding possible device re-initialization.
     *
     * It will not conflict with the regular rendering as:
     *  - Rendering is supposed to be finished here.
     *  - The next rendering will go via Session's `run_update_for_next_iteration` which will
     *    ensure proper denoiser is used. */
    set_denoiser_params(denoise_params);

    /* Number of samples doesn't matter too much, since the samples count pass will be used. */
    denoiser_->denoise_buffer(
        full_frame_buffers.params, full_frame_buffers.params, &full_frame_buffers, 0, false);

    render_state_.has_denoised_result = true;
  }

  /* Full-buffer replay writes the whole loaded physical buffer back to Blender as one tile.
   * Normalize the replay window to the loaded buffer extent so pass accessors do not apply the
   * display-window offset a second time. */
  full_frame_buffers.params.window_x = 0;
  full_frame_buffers.params.window_y = 0;
  full_frame_buffers.params.window_width = full_frame_buffers.params.width;
  full_frame_buffers.params.window_height = full_frame_buffers.params.height;

  if (beauty_consumer && !beauty_consumer->set_final_bounded_beauty(full_frame_buffers)) {
    progress_->set_error(beauty_consumer->capture_error_message());
    return;
  }
  full_frame_state_.render_buffers = &full_frame_buffers;

  progress_set_status(layer_view_name, "Finishing");

  /* Write the full result pretending that there is a single tile.
   * Requires some state change, but allows to use same communication API with the software. */
  tile_buffer_write();

  full_frame_state_.render_buffers = nullptr;
}

int PathTrace::get_num_render_tile_samples() const
{
  if (full_frame_state_.render_buffers) {
    return full_frame_state_.render_buffers->params.samples;
  }

  return render_scheduler_.get_num_rendered_samples();
}

bool PathTrace::get_render_tile_pixels(const PassAccessor &pass_accessor,
                                       const PassAccessor::Destination &destination)
{
  if (full_frame_state_.render_buffers) {
    return pass_accessor.get_render_tile_pixels(full_frame_state_.render_buffers, destination);
  }

  if (big_tile_denoise_work_ && render_state_.has_denoised_result) {
    /* Only use the big tile denoised buffer to access the denoised passes.
     * The guiding passes are allowed to be modified in-place for the needs of the denoiser,
     * so copy those from the original devices buffers. */
    if (pass_accessor.get_pass_access_info().mode == PassMode::DENOISED) {
      return big_tile_denoise_work_->get_render_tile_pixels(pass_accessor, destination);
    }
  }

  bool success = true;

  parallel_for_each(path_trace_works_, [&](unique_ptr<PathTraceWork> &path_trace_work) {
    if (!success) {
      return;
    }
    if (!path_trace_work->get_render_tile_pixels(pass_accessor, destination)) {
      success = false;
    }
  });

  return success;
}

bool PathTrace::set_render_tile_pixels(PassAccessor &pass_accessor,
                                       const PassAccessor::Source &source)
{
  bool success = true;

  parallel_for_each(path_trace_works_, [&](unique_ptr<PathTraceWork> &path_trace_work) {
    if (!success) {
      return;
    }
    if (!path_trace_work->set_render_tile_pixels(pass_accessor, source)) {
      success = false;
    }
  });

  return success;
}

int2 PathTrace::get_render_tile_size() const
{
  if (full_frame_state_.render_buffers) {
    return make_int2(full_frame_state_.render_buffers->params.window_width,
                     full_frame_state_.render_buffers->params.window_height);
  }

  const Tile &tile = tile_manager_.get_current_tile();
  return make_int2(tile.window_width, tile.window_height);
}

int2 PathTrace::get_render_tile_offset() const
{
  if (full_frame_state_.render_buffers) {
    return make_int2(0, 0);
  }

  const Tile &tile = tile_manager_.get_current_tile();
  return make_int2(tile.x + tile.window_x, tile.y + tile.window_y);
}

int2 PathTrace::get_render_size() const
{
  return tile_manager_.get_size();
}

const BufferParams &PathTrace::get_render_tile_params() const
{
  if (full_frame_state_.render_buffers) {
    return full_frame_state_.render_buffers->params;
  }

  return big_tile_params_;
}

bool PathTrace::has_denoised_result() const
{
  return render_state_.has_denoised_result;
}

void PathTrace::destroy_gpu_resources()
{
  /* Destroy any GPU resource which was used for graphics interop.
   * Need to have access to the PathTraceDisplay as it is the only source of drawing context which
   * is used for interop. */
  if (display_) {
    for (auto &&path_trace_work : path_trace_works_) {
      path_trace_work->destroy_gpu_resources(display_.get());
    }

    if (big_tile_denoise_work_) {
      big_tile_denoise_work_->destroy_gpu_resources(display_.get());
    }
  }
}

/* --------------------------------------------------------------------
 * Report generation.
 */

static const char *device_type_for_description(const DeviceType type)
{
  switch (type) {
    case DEVICE_NONE:
      return "None";

    case DEVICE_CPU:
      return "CPU";
    case DEVICE_CUDA:
      return "CUDA";
    case DEVICE_OPTIX:
      return "OptiX";
    case DEVICE_HIP:
      return "HIP";
    case DEVICE_HIPRT:
      return "HIPRT";
    case DEVICE_ONEAPI:
      return "oneAPI";
    case DEVICE_DUMMY:
      return "Dummy";
    case DEVICE_MULTI:
      return "Multi";
    case DEVICE_METAL:
      return "Metal";
  }

  return "UNKNOWN";
}

/* Construct description of the device which will appear in the full report. */
/* TODO(sergey): Consider making it more reusable utility. */
static string full_device_info_description(const DeviceInfo &device_info)
{
  string full_description = device_info.description;

  full_description += " (" + string(device_type_for_description(device_info.type)) + ")";

  if (device_info.display_device) {
    full_description += " (display)";
  }

  if (device_info.type == DEVICE_CPU) {
    full_description += " (" + to_string(device_info.cpu_threads) + " threads)";
  }

  full_description += " [" + device_info.id + "]";

  return full_description;
}

/* Construct string which will contain information about devices, possibly multiple of the devices.
 *
 * In the simple case the result looks like:
 *
 *   Message: Full Device Description
 *
 * If there are multiple devices then the result looks like:
 *
 *   Message: Full First Device Description
 *            Full Second Device Description
 *
 * Note that the newlines are placed in a way so that the result can be easily concatenated to the
 * full report. */
static string device_info_list_report(const string &message, const DeviceInfo &device_info)
{
  string result = "\n" + message + ": ";
  const string pad(message.length() + 2, ' ');

  if (device_info.multi_devices.empty()) {
    result += full_device_info_description(device_info) + "\n";
    result += pad +
              "    Hardware Ray-Tracing: " + (device_info.use_hardware_raytracing ? "On" : "Off") +
              "\n";
    return result;
  }

  bool is_first = true;
  for (const DeviceInfo &sub_device_info : device_info.multi_devices) {
    if (!is_first) {
      result += pad;
    }

    result += full_device_info_description(sub_device_info) + "\n";
    result += pad + "    Hardware Ray-Tracing: " +
              (sub_device_info.use_hardware_raytracing ? "On" : "Off") + "\n";

    is_first = false;
  }

  return result;
}

static string path_trace_devices_report(const vector<unique_ptr<PathTraceWork>> &path_trace_works)
{
  DeviceInfo device_info;
  device_info.type = DEVICE_MULTI;

  for (auto &&path_trace_work : path_trace_works) {
    device_info.multi_devices.push_back(path_trace_work->get_device()->info);
  }

  return device_info_list_report("Path tracing on", device_info);
}

static string denoiser_device_report(const Denoiser *denoiser)
{
  if (!denoiser) {
    return "";
  }

  if (!denoiser->get_params().use) {
    return "";
  }

  const Device *denoiser_device = denoiser->get_denoiser_device();
  if (!denoiser_device) {
    return "";
  }

  return device_info_list_report("Denoising on", denoiser_device->info);
}

string PathTrace::full_report() const
{
  string result = "Full path tracing report:\n";

  result += path_trace_devices_report(path_trace_works_);
  result += denoiser_device_report(denoiser_.get());

  /* Report from the render scheduler, which includes:
   * - Render mode (interactive, offline, headless)
   * - Adaptive sampling and denoiser parameters
   * - Breakdown of timing. */
  result += render_scheduler_.full_report();

  return result;
}

void PathTrace::set_guiding_params(const GuidingParams &guiding_params, const bool reset)
{
#if defined(WITH_PATH_GUIDING)
  if (guiding_params_.modified(guiding_params)) {
    guiding_params_ = guiding_params;

#  if !(OPENPGL_VERSION_MAJOR == 0 && OPENPGL_VERSION_MINOR <= 5)
#    define OPENPGL_USE_FIELD_CONFIG
#  endif

    if (guiding_params_.use) {
#  ifdef OPENPGL_USE_FIELD_CONFIG
      openpgl::cpp::FieldConfig field_config;
#  else
      PGLFieldArguments field_args;
#  endif
      switch (guiding_params_.type) {
        default:
        /* Parallax-aware von Mises-Fisher mixture models. */
        case GUIDING_TYPE_PARALLAX_AWARE_VMM: {
#  ifdef OPENPGL_USE_FIELD_CONFIG
          field_config.Init(
              PGL_SPATIAL_STRUCTURE_TYPE::PGL_SPATIAL_STRUCTURE_KDTREE,
              PGL_DIRECTIONAL_DISTRIBUTION_TYPE::PGL_DIRECTIONAL_DISTRIBUTION_PARALLAX_AWARE_VMM,
              guiding_params.deterministic);
#  else
          pglFieldArgumentsSetDefaults(
              field_args,
              PGL_SPATIAL_STRUCTURE_TYPE::PGL_SPATIAL_STRUCTURE_KDTREE,
              PGL_DIRECTIONAL_DISTRIBUTION_TYPE::PGL_DIRECTIONAL_DISTRIBUTION_PARALLAX_AWARE_VMM);
#  endif
          break;
        }
        /* Directional quad-trees. */
        case GUIDING_TYPE_DIRECTIONAL_QUAD_TREE: {
#  ifdef OPENPGL_USE_FIELD_CONFIG
          field_config.Init(
              PGL_SPATIAL_STRUCTURE_TYPE::PGL_SPATIAL_STRUCTURE_KDTREE,
              PGL_DIRECTIONAL_DISTRIBUTION_TYPE::PGL_DIRECTIONAL_DISTRIBUTION_QUADTREE,
              guiding_params.deterministic);
#  else
          pglFieldArgumentsSetDefaults(
              field_args,
              PGL_SPATIAL_STRUCTURE_TYPE::PGL_SPATIAL_STRUCTURE_KDTREE,
              PGL_DIRECTIONAL_DISTRIBUTION_TYPE::PGL_DIRECTIONAL_DISTRIBUTION_QUADTREE);
#  endif
          break;
        }
        /* von Mises-Fisher mixture models. */
        case GUIDING_TYPE_VMM: {
#  ifdef OPENPGL_USE_FIELD_CONFIG
          field_config.Init(PGL_SPATIAL_STRUCTURE_TYPE::PGL_SPATIAL_STRUCTURE_KDTREE,
                            PGL_DIRECTIONAL_DISTRIBUTION_TYPE::PGL_DIRECTIONAL_DISTRIBUTION_VMM,
                            guiding_params.deterministic);
#  else
          pglFieldArgumentsSetDefaults(
              field_args,
              PGL_SPATIAL_STRUCTURE_TYPE::PGL_SPATIAL_STRUCTURE_KDTREE,
              PGL_DIRECTIONAL_DISTRIBUTION_TYPE::PGL_DIRECTIONAL_DISTRIBUTION_VMM);
#  endif
          break;
        }
      }
#  ifdef OPENPGL_USE_FIELD_CONFIG
      field_config.SetSpatialStructureArgMaxDepth(16);
#  else
      field_args.deterministic = guiding_params.deterministic;
      reinterpret_cast<PGLKDTreeArguments *>(field_args.spatialSturctureArguments)->maxDepth = 16;
#  endif
      openpgl::cpp::Device *guiding_device = static_cast<openpgl::cpp::Device *>(
          device_->get_guiding_device());
      if (guiding_device) {
        guiding_sample_data_storage_ = make_unique<openpgl::cpp::SampleStorage>();
#  ifdef OPENPGL_USE_FIELD_CONFIG
        guiding_field_ = make_unique<openpgl::cpp::Field>(guiding_device, field_config);
#  else
        guiding_field_ = make_unique<openpgl::cpp::Field>(guiding_device, field_args);
#  endif
      }
      else {
        guiding_sample_data_storage_ = nullptr;
        guiding_field_ = nullptr;
      }
    }
    else {
      guiding_sample_data_storage_ = nullptr;
      guiding_field_ = nullptr;
    }
  }
  else if (reset) {
    if (guiding_field_) {
      guiding_field_->Reset();
    }
  }
#else
  (void)guiding_params;
  (void)reset;
#endif
}

void PathTrace::guiding_prepare_structures()
{
#if defined(WITH_PATH_GUIDING)
  const bool train = (guiding_params_.training_samples == 0) ||
                     (guiding_field_->GetIteration() < guiding_params_.training_samples);

  for (auto &&path_trace_work : path_trace_works_) {
    path_trace_work->guiding_init_kernel_globals(
        guiding_field_.get(), guiding_sample_data_storage_.get(), train);
  }

  if (train) {
    /* For training the guiding distribution we need to force the number of samples
     * per update to be limited, for reproducible results and reasonable training size.
     *
     * Idea: we could stochastically discard samples with a probability of 1/num_samples_per_update
     * we can then update only after the num_samples_per_update iterations are rendered. */
    render_scheduler_.set_limit_samples_per_update(4);
  }
  else {
    render_scheduler_.set_limit_samples_per_update(0);
  }
#endif
}

void PathTrace::guiding_update_structures()
{
#if defined(WITH_PATH_GUIDING)
  LOG_DEBUG << "Update path guiding structures";

  LOG_TRACE << "Number of surface samples: " << guiding_sample_data_storage_->GetSizeSurface();
  LOG_TRACE << "Number of volume samples: " << guiding_sample_data_storage_->GetSizeVolume();

  const size_t num_valid_samples = guiding_sample_data_storage_->GetSizeSurface() +
                                   guiding_sample_data_storage_->GetSizeVolume();

  /* we wait until we have at least 1024 samples */
  if (num_valid_samples >= 1024) {
    guiding_field_->Update(*guiding_sample_data_storage_);
    guiding_update_count++;

    LOG_TRACE << "Path guiding field valid: " << guiding_field_->Validate();

    guiding_sample_data_storage_->Clear();
  }
#endif
}

CCL_NAMESPACE_END
