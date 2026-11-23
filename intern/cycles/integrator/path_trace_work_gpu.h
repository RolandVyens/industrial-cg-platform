/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/integrator/state.h"

#include "device/graphics_interop.h"
#include "device/memory.h"
#include "device/queue.h"

#include "integrator/deep_camera_receipt_writer.h"
#include "integrator/deep_frame_capture.h"
#include "integrator/path_trace_work.h"
#include "integrator/work_tile_scheduler.h"

#include "util/vector.h"

#include <optional>

CCL_NAMESPACE_BEGIN

struct KernelWorkTile;
class DeepExpressionJournal;
class DeepControlJournal;

/* Implementation of PathTraceWork which schedules work to the device in tiles which are sized
 * to match device queue's number of path states.
 * This implementation suits best devices which have a lot of integrator states, such as GPU. */
class PathTraceWorkGPU : public PathTraceWork {
 public:
  PathTraceWorkGPU(Device *device,
                   Film *film,
                   DeviceScene *device_scene,
                   const bool *cancel_requested_flag);
  ~PathTraceWorkGPU() override;

  void alloc_work_memory() override;
  void init_execution() override;
  int2 deep_bounded_camera_limits() const override;
  DeepBoundedDeviceAdmission deep_bounded_admission_snapshot() const override;
  void set_deep_bounded_admission_limit(uint32_t samples, size_t bytes, size_t auxiliary_growth) override;

  bool set_deep_capture_group_flush(std::function<bool()> flush,
                                    int max_camera_paths,
                                    int max_ready_paths = 0) override;
  bool set_deep_frame_capture_consumer(DeepFrameCaptureConsumer *consumer) override;
  bool close_deep_tile() override;
  bool prepare_deep_tile() override;
  bool drain_deep_tiles() override;
  bool publish_ready_deep_tiles() override;
  bool migrate_bounded_deep(PathTraceWork &destination, int x, int y,
                             uint32_t width, uint32_t height) override;
  bool set_bounded_deep_beauty(const RenderBuffers &buffers) override;
  void set_deep_beauty_denoised(bool value) override;

  void render_samples(RenderStatistics &statistics,
                      const int start_sample,
                      const int samples_num,
                      const int sample_offset) override;

  void copy_to_display(PathTraceDisplay *display,
                       PassMode pass_mode,
                       const int num_samples) override;
  void destroy_gpu_resources(PathTraceDisplay *display) override;

  bool copy_render_buffers_from_device() override;
  bool copy_render_buffers_to_device() override;
  bool zero_render_buffers() override;

  int adaptive_sampling_converge_filter_count_active(const float threshold, bool reset) override;
  void cryptomatte_postproces() override;
  void denoise_volume_guiding_buffers() override;

 protected:
  void render_bounded_samples(RenderStatistics &statistics,
                              int start_sample,
                              int samples_num,
                              int sample_offset);
  void render_samples_batch(RenderStatistics &statistics,
                            int start_sample,
                            int samples_num,
                            int sample_offset);
  void alloc_integrator_soa();
  void alloc_integrator_queue();
  void alloc_integrator_sorting();
  void alloc_integrator_path_split();

  /* Returns DEVICE_KERNEL_NUM if there are no scheduled kernels. */
  DeviceKernel get_most_queued_kernel() const;
  bool bounded_shadows_can_wait() const;

  void enqueue_reset(bool clear_path_slots = true);

  bool enqueue_work_tiles(bool &finished, bool admit_new_packet = false);
  void enqueue_work_tiles(DeviceKernel kernel,
                          const KernelWorkTile work_tiles[],
                          const int num_work_tiles,
                          const int num_active_paths,
                          const int num_predicted_splits);

  bool enqueue_path_iteration();
  void enqueue_path_iteration(DeviceKernel kernel, const int num_paths_limit = INT_MAX);

  bool update_queue_counter_and_cache();
  bool begin_native_capture(int start_sample, int samples_num, int sample_offset);
  bool finish_native_capture(bool complete);
  bool flush_native_expression_page();
  bool flush_native_root_page();
  bool flush_native_control_page();
  bool flush_native_gpu_pages();
  void release_native_capture_buffers();
  void alloc_camera_receipts();
  void alloc_surface_receipts();
  void alloc_volume_radiance_receipts();
  void alloc_volume_emission_spans();
  bool begin_camera_receipts(int start_sample, int samples_num, int sample_offset);
  bool drain_camera_receipts();
  bool finish_surface_receipts(bool complete);
  bool finish_volume_radiance_receipts(bool complete);
  bool finish_volume_emission_spans(bool complete);

  void compute_queued_paths(DeviceKernel kernel, DeviceKernel queued_kernel);
  void compute_sorted_queued_paths(DeviceKernel queued_kernel, const int num_paths_limit);

  void compact_main_paths(const int num_active_paths);
  void compact_shadow_paths(const bool force = false);
  void compact_paths(const int num_active_paths,
                     const int max_active_path_index,
                     DeviceKernel terminated_paths_kernel,
                     DeviceKernel compact_paths_kernel,
                     DeviceKernel compact_kernel);

  int num_active_main_paths_paths();

  /* Check whether graphics interop can be used for the PathTraceDisplay update. */
  bool should_use_graphics_interop(PathTraceDisplay *display);

  /* Naive implementation of the `copy_to_display()` which performs film conversion on the
   * device, then copies pixels to the host and pushes them to the `display`. */
  void copy_to_display_naive(PathTraceDisplay *display, PassMode pass_mode, const int num_samples);

  /* Implementation of `copy_to_display()` which uses driver's OpenGL/GPU interoperability
   * functionality, avoiding copy of pixels to the host. */
  bool copy_to_display_interop(PathTraceDisplay *display,
                               PassMode pass_mode,
                               const int num_samples);

  /* Synchronously run film conversion kernel and store display result in the given destination. */
  void get_render_tile_film_pixels(const PassAccessor::Destination &destination,
                                   PassMode pass_mode,
                                   int num_samples);

  int adaptive_sampling_convergence_check_count_active(const float threshold, bool reset);
  void enqueue_adaptive_sampling_filter_x();
  void enqueue_adaptive_sampling_filter_y();

  bool has_shadow_catcher() const;

  /* Count how many currently scheduled paths can still split. */
  int shadow_catcher_count_possible_splits();

  /* Kernel properties. */
  bool kernel_uses_sorting(DeviceKernel kernel);
  bool kernel_creates_shadow_paths(DeviceKernel kernel);
  bool kernel_creates_ao_paths(DeviceKernel kernel);
  static bool kernel_is_shadow_path(DeviceKernel kernel);
  int kernel_max_active_main_path_index(DeviceKernel kernel);

  /* Integrator queue. */
  unique_ptr<DeviceQueue> queue_;

  /* Scheduler which gives work to path tracing threads. */
  WorkTileScheduler work_tile_scheduler_;

  /* Integrate state for paths. */
  IntegratorStateGPU integrator_state_gpu_;
  /* SoA arrays for integrator state. */
  vector<unique_ptr<device_memory>> integrator_state_soa_;
  uint64_t integrator_state_soa_kernel_features_;
  bool integrator_state_soa_use_shadow_color_ = false;
  int integrator_state_soa_volume_stack_size_ = 0;
  /* Keep track of number of queued kernels. */
  device_vector<IntegratorQueueCounter> integrator_queue_counter_;
  device_vector<DeepNativeCameraRecord> deep_native_cameras_;
  device_vector<DeepCompactPrimitive> deep_compact_primitives_;
  device_only_memory<DeepCompactRayState> deep_compact_rays_;
  device_only_memory<DeepCompactStackEntry> deep_compact_stacks_;
  device_only_memory<DeepCompactEmissionState> deep_compact_emission_;
  uint32_t deep_native_validation_seen_ = 0;
  bool deep_native_validation_fatal_after_yield_ = false;
  std::shared_ptr<DeepExpressionJournal> deep_native_expression_journal_;
  std::shared_ptr<DeepControlJournal> deep_native_control_journal_;
  device_only_memory<uint64_t> deep_native_volume_checkpoints_;
  device_vector<DeepNativeHomogeneousRecord> deep_native_homogeneous_;
  device_vector<DeepNativeRootRecord> deep_native_roots_;
  device_vector<DeepNativeRootRecord> deep_native_root_resident_;
  device_vector<uint32_t> deep_native_root_pins_;
  device_vector<DeepNativePointRecord> deep_native_points_;
  device_vector<DeepNativeExpressionSource> deep_native_expression_sources_;
  device_vector<DeepNativeExpressionNode> deep_native_expression_nodes_;
  device_vector<DeepNativeExpressionCall> deep_native_expression_calls_;
  device_only_memory<uint32_t> deep_native_camera_indices_;
  device_only_memory<uint32_t> deep_native_root_handles_;
  device_only_memory<uint32_t> deep_native_shadow_camera_indices_;
  device_only_memory<uint32_t> deep_native_shadow_root_handles_;
  DeepFrameCaptureConsumer *deep_frame_capture_consumer_ = nullptr;
  std::shared_ptr<class DeepGPUFrameWork> deep_gpu_frame_work_;
  std::unique_ptr<class DeepBoundedWork> deep_bounded_work_;
  bool deep_bounded_slots_empty_ = false;
  bool deep_bounded_cache_state_ = true;
  std::optional<IntegratorStateGPU> deep_bounded_uploaded_state_;
  bool prefer_bounded_main_queues_ = false;
  int bounded_shadow_queue_limit_ = 0;
  std::optional<float> deep_bounded_projection_;
  double deep_gpu_queue_sync_wall_ = 0, deep_gpu_page_flush_wall_ = 0;
  uint64_t deep_gpu_queue_updates_ = 0, deep_gpu_page_publications_ = 0;
  std::optional<DeepMemoryBudget> deep_native_budget_;
  std::optional<DeepMemoryBudget> deep_native_device_budget_;
  DeepNativeCaptureDomain deep_native_domain_;
  size_t deep_native_allocation_charge_ = 0;
  size_t deep_native_device_charge_ = 0;
  bool deep_frame_capture_requested_ = false;
  bool deep_native_domain_active_ = false;
  uint64_t deep_native_device_generation_ = 0, deep_native_layout_generation_ = 0;
  device_vector<DeepCameraReceipt> deep_camera_receipts_;
  DeepCameraReceiptWriter deep_camera_receipt_writer_;
  bool deep_camera_receipts_enabled_ = false;
  device_vector<DeepSurfaceReceipt> deep_surface_receipts_;
  DeepSurfaceReceiptWriter deep_surface_receipt_writer_;
  bool deep_surface_receipts_enabled_ = false;
  device_vector<DeepVolumeRadianceReceipt> deep_volume_radiance_receipts_;
  DeepVolumeRadianceReceiptWriter deep_volume_radiance_receipt_writer_;
  bool deep_volume_radiance_receipts_enabled_ = false;
  device_vector<DeepVolumeEmissionSpan> deep_volume_emission_spans_;
  DeepVolumeEmissionSpanWriter deep_volume_emission_span_writer_;
  bool deep_volume_emission_spans_enabled_ = false;
  bool deep_independent_optical_enabled_ = false;
  /* Shader sorting. */
  device_vector<int> integrator_shader_sort_counter_;
  device_vector<int> integrator_shader_raytrace_sort_counter_;
  device_vector<int> integrator_shader_sort_prefix_sum_;
  device_vector<int> integrator_shader_sort_partition_key_offsets_;
  /* Path split. */
  device_vector<int> integrator_next_main_path_index_;
  device_vector<int> integrator_next_shadow_path_index_;

  /* Temporary buffer to get an array of queued path for a particular kernel. */
  device_vector<int> queued_paths_;
  device_vector<int> num_queued_paths_;

  /* Temporary buffer for passing work tiles to kernel. */
  device_vector<KernelWorkTile> work_tiles_;

  /* Temporary buffer used by the copy_to_display() whenever graphics interoperability is not
   * available. Is allocated on-demand. */
  device_vector<half4> display_rgba_half_;

  unique_ptr<DeviceGraphicsInterop> device_graphics_interop_;

  /* Cached result of device->should_use_graphics_interop(). */
  bool interop_use_checked_ = false;
  bool interop_use_ = false;

  /* Number of partitions to sort state indices into prior to material sort. */
  int num_sort_partitions_;

  /* Maximum number of concurrent integrator states. */
  int max_num_paths_;

  /* Experimental complete optical wavefront followed by the corresponding Beauty wavefront. */
  bool deep_optical_wavefront_ = false;
  bool deep_optical_phase_ = false;
  bool deep_beauty_coalesce_ = false;
  std::function<bool()> deep_capture_group_flush_;
  int deep_capture_group_limit_ = 0;
  int deep_capture_ready_limit_ = 0;
  int deep_capture_group_admitted_ = 0;

  /* Minimum number of paths which keeps the device bust. If the actual number of paths falls below
   * this value more work will be scheduled. */
  int min_num_active_main_paths_;

  /* Maximum path index, effective number of paths used may be smaller than
   * the size of the integrator_state_ buffer so can avoid iterating over the
   * full buffer. */
  int max_active_main_path_index_;
};

CCL_NAMESPACE_END
