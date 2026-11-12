/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <functional>

#include "integrator/pass_accessor.h"
#include "integrator/deep_bounded_layout.h"
#include "scene/pass.h"
#include "session/buffers.h"
#include "util/unique_ptr.h"

CCL_NAMESPACE_BEGIN

class BufferParams;
class Device;
class DeviceScene;
class DeepFrameCaptureConsumer;
class Film;
class PathTraceDisplay;
class RenderBuffers;
class Progress;

class PathTraceWork {
 public:
  struct RenderStatistics {
    float occupancy = 1.0f;
  };

  /* Create path trace work which fits best the device.
   *
   * The cancel request flag is used for a cheap check whether cancel is to be performed as soon as
   * possible. This could be, for example, request to cancel rendering on camera navigation in
   * viewport. */
  static unique_ptr<PathTraceWork> create(Device *device,
                                          Film *film,
                                          DeviceScene *device_scene,
                                          const bool *cancel_requested_flag);

  virtual ~PathTraceWork();
  /* Borrow the Session progress source for joined continuation boundaries.
   * Each new RenderWork resets the per-work cancellation observation. */
  void set_progress(Progress *progress);
  bool poll_cancel_requested();

  /* Access the render buffers.
   *
   * Is only supposed to be used by the PathTrace to update buffer allocation and slicing to
   * correspond to the big tile size and relative device performance. */
  RenderBuffers *get_render_buffers();

  /* Set effective parameters of the big tile and the work itself. */
  void set_effective_buffer_params(const BufferParams &effective_big_tile_params,
                                   const BufferParams &effective_buffer_params,
                                   const BufferParams &effective_denoised_big_tile_params,
                                   const BufferParams &effective_denoised_buffer_params);

  /* Check whether the big tile is being worked on by multiple path trace works. */
  bool has_multiple_works() const;

  /* Allocate working memory for execution. Must be called before init_execution(). */
  virtual void alloc_work_memory() {};

  /* Initialize execution of kernels.
   * Will ensure that all device queues are initialized for execution.
   *
   * This method is to be called after any change in the scene. It is not needed to call it prior
   * to an every call of the `render_samples()`. */
  virtual void init_execution() = 0;

  /* Release resources acquired by init_execution(). */
  virtual void deinit_execution() {}

  /* Optional single-GPU capture boundary after every admitted camera group has drained. */
  virtual bool set_deep_capture_group_flush(std::function<bool()> /*flush*/,
                                            const int /*max_camera_paths*/,
                                            const int /*max_ready_paths*/ = 0)
  {
    return false;
  }

  /* Production capture is supported only by an explicitly selected native GPU work. */
  virtual bool set_deep_frame_capture_consumer(DeepFrameCaptureConsumer * /*consumer*/)
  {
    return false;
  }
  virtual bool close_deep_tile()
  {
    return false;
  }
  virtual bool prepare_deep_tile()
  {
    return false;
  }
  /* Single Host owner only, after render_samples workers have joined. */
  virtual bool publish_ready_deep_tiles() { return true; }
  virtual bool drain_deep_tiles()
  {
    return true;
  }
  virtual bool migrate_bounded_deep(PathTraceWork &, int, int, uint32_t, uint32_t)
  {
    return false;
  }
  virtual bool set_bounded_deep_beauty(const RenderBuffers &)
  {
    return false;
  }
  virtual void set_deep_beauty_denoised(bool) {}

  /* Render given number of samples as a synchronous blocking call.
   * The samples are added to the render buffer associated with this work. */
  virtual void render_samples(RenderStatistics &statistics,
                              const int start_sample,
                              const int samples_num,
                              const int sample_offset) = 0;

  /* Copy render result from this work to the corresponding place of the GPU display.
   *
   * The `pass_mode` indicates whether to access denoised or noisy version of the display pass. The
   * noisy pass mode will be passed here when it is known that the buffer does not have denoised
   * passes yet (because denoiser did not run). If the denoised pass is requested and denoiser is
   * not used then this function will fall-back to the noisy pass instead. */
  virtual void copy_to_display(PathTraceDisplay *display,
                               PassMode pass_mode,
                               const int num_samples) = 0;

  virtual void destroy_gpu_resources(PathTraceDisplay *display) = 0;

  /* Copy data from/to given render buffers.
   * Will copy pixels from a corresponding place (from multi-device point of view) of the render
   * buffers, and copy work's render buffers to the corresponding place of the destination. */

  /* Notes:
   * - Copies work's render buffer from the device.
   * - Copies CPU-side buffer of the given buffer
   * - Does not copy the buffer to its device. */
  void copy_to_render_buffers(RenderBuffers *render_buffers);

  /* Notes:
   * - Does not copy given render buffers from the device.
   * - Copies work's render buffer to its device. */
  void copy_from_render_buffers(const RenderBuffers *render_buffers);

  /* Special version of the `copy_from_render_buffers()` which only copies denoised passes from the
   * given render buffers, leaving rest of the passes.
   *
   * Same notes about device copying applies to this call as well. */
  void copy_from_denoised_render_buffers(const RenderBuffers *render_buffers);

  /* Copy render buffers to/from device using an appropriate device queue when needed so that
   * things are executed in order with the `render_samples()`. */
  virtual bool copy_render_buffers_from_device() = 0;
  virtual bool copy_render_buffers_to_device() = 0;

  /* Zero render buffers to/from device using an appropriate device queue when needed so that
   * things are executed in order with the `render_samples()`. */
  virtual bool zero_render_buffers() = 0;

  /* Access pixels rendered by this work and copy them to the corresponding location in the
   * destination.
   *
   * NOTE: Does not perform copy of buffers from the device. Use `copy_render_tile_from_device()`
   * to update host-side data. */
  bool get_render_tile_pixels(const PassAccessor &pass_accessor,
                              const PassAccessor::Destination &destination);

  /* Set pass data for baking. */
  bool set_render_tile_pixels(PassAccessor &pass_accessor, const PassAccessor::Source &source);

  /* Perform convergence test on the render buffer, and filter the convergence mask.
   * Returns number of active pixels (the ones which did not converge yet). */
  virtual int adaptive_sampling_converge_filter_count_active(const float threshold,
                                                             bool reset) = 0;

  /* Denoise Volume Scattering Probability Guiding buffers. */
  virtual void denoise_volume_guiding_buffers() = 0;

  /* Run cryptomatte pass post-processing kernels. */
  virtual void cryptomatte_postproces() = 0;

  /* Cheap-ish request to see whether rendering is requested and is to be stopped as soon as
   * possible, without waiting for any samples to be finished. */
  bool is_cancel_requested() const
  {
    /* NOTE: Rely on the fact that on x86 CPU reading scalar can happen without atomic even in
     * threaded environment. */
    return *cancel_requested_flag_ || progress_cancel_requested_;
  }

  /* Access to the device which is used to path trace this work on. */
  Device *get_device() const
  {
    return device_;
  }

  const BufferParams &get_effective_buffer_params() const
  {
    return effective_buffer_params_;
  }

  virtual DeepBoundedDeviceAdmission deep_bounded_admission_snapshot() const { return {}; }
  virtual void set_deep_bounded_admission_limit(uint32_t, size_t, size_t) {}
  virtual int2 deep_bounded_camera_limits() const { return make_int2(0, 0); }

#if defined(WITH_PATH_GUIDING)
  /* Initializes the per-thread guiding kernel data. */
  virtual void guiding_init_kernel_globals(void * /*unused*/,
                                           void * /*unused*/,
                                           const bool /*unused*/)
  {
  }
#endif

 protected:
  PathTraceWork(Device *device,
                Film *film,
                DeviceScene *device_scene,
                const bool *cancel_requested_flag);

  PassAccessor::PassAccessInfo get_display_pass_access_info(PassMode pass_mode) const;

  /* Get destination which offset and stride are configured so that writing to it will write to a
   * proper location of GPU display texture, taking current tile and device slice into account. */
  PassAccessor::Destination get_display_destination_template(const PathTraceDisplay *display,
                                                             const PassMode mode) const;

  /* Device which will be used for path tracing.
   * Note that it is an actual render device (and never is a multi-device). */
  Device *device_;

  /* Film is used to access display pass configuration for GPU display update.
   * Note that only fields which are not a part of kernel data can be accessed via the Film. */
  Film *film_;

  /* Device side scene storage, that may be used for integrator logic. */
  DeviceScene *device_scene_;

  /* Render buffers where sampling is being accumulated into, allocated for a fraction of the big
   * tile which is being rendered by this work.
   * It also defines possible subset of a big tile in the case of multi-device rendering. */
  unique_ptr<RenderBuffers> buffers_;

  /* Effective parameters of the big tile, and current work render buffer.
   * The latter might be different from `buffers_->params` when there is a resolution divider
   * involved. */
  BufferParams effective_big_tile_params_;
  BufferParams effective_buffer_params_;
  BufferParams effective_denoised_big_tile_params_;
  BufferParams effective_denoised_buffer_params_;

  const bool *cancel_requested_flag_ = nullptr;
  Progress *progress_ = nullptr;
  bool progress_cancel_requested_ = false;
};

CCL_NAMESPACE_END
