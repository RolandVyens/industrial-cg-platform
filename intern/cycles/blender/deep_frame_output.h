/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <mutex>

#include "RE_deep_data.hh"
#include "session/deep_frame_production.h"
#include "session/deep_view_registry.h"

CCL_NAMESPACE_BEGIN

struct BlenderDeepFrameWindow {
  bool enabled = false;
  int width = 0, height = 0, offset_x = 0, offset_y = 0, data_x = 0, data_y = 0;
};

class BlenderDeepFrameOutput : public blender::RenderDeepOutputSource {
 public:
  BlenderDeepFrameOutput(std::shared_ptr<DeepFrameProduction> production,
                         DeepRasterWindow raster,
                         BlenderDeepFrameWindow display,
                         const DeepFrameKey &frame);
  bool write(const char *filepath,
             int compression,
             bool half,
             bool alpha_only,
             float depth_tolerance,
             float alpha_tolerance,
             std::string &error,
             uint64_t consumer_key = 0,
             const char *view_name = "") override;
  bool write_direct(const char *filepath,
                    int compression,
                    bool half,
                    float depth_tolerance,
                    float alpha_tolerance,
                    std::string &error);
  bool finish(std::string &error) override;
  const DeepFrameKey &frame_key() const
  {
    return frame_;
  }
  bool finish_inputs(std::string &error);
  bool inputs_sealed() const
  {
    return inputs_sealed_;
  }
  void cancel();

 private:
  friend class BlenderDeepFrameViews;
  bool write_checked_consumer(const char *path,
                              int compression,
                              const DeepCanonicalEncodeConfig &config,
                              const DeepConsumerToken &token,
                              std::string &error);
  bool write_consumer(uint64_t source_key,
                      const char *filepath,
                      int compression,
                      bool half,
                      bool alpha_only,
                      float depth,
                      float alpha,
                      std::string &error);
  DeepFrameKey frame_;
  bool inputs_sealed_ = false;
  std::shared_ptr<DeepFrameProduction> production_;
  DeepRasterWindow raster_;
  BlenderDeepFrameWindow display_;
};

/* Shared across layers/views; records immutable identities without retaining producers. */
class BlenderDeepOutputAddresses {
 public:
  explicit BlenderDeepOutputAddresses(const DeepMemoryBudget &budget);
  bool claim(const DeepOutputIdentity &identity,
             uint64_t consumer,
             const char *path,
             std::string &error);
  bool healthy(std::string &error);
  bool fail(const std::string &message, std::string &error);

 private:
  struct Address {
    DeepOutputIdentity identity;
    uint64_t consumer_key = 0;
    std::array<char, 1024> path = {};
  };
  bool fail_unlocked(const std::string &message, std::string &error);
  std::mutex mutex_;
  std::vector<Address, DeepBudgetAllocator<Address>> addresses_;
  std::string first_error_;
};

/* One render-layer source routes real compositor views to independent children. */
class BlenderDeepFrameViews : public blender::RenderDeepOutputSource {
 public:
  BlenderDeepFrameViews(const DeepMemoryBudget &budget,
                        std::shared_ptr<BlenderDeepOutputAddresses> addresses);
  bool register_view(std::string_view name, std::string_view path, std::string &error);
  bool bind_view(std::string_view name,
                 std::shared_ptr<BlenderDeepFrameOutput> output,
                 std::string &error);
  bool seal_inputs(std::string &error);
  bool write_direct(int compression, bool half, float depth, float alpha, std::string &error);
  bool save_direct(const char *filepath,
                   int compression,
                   bool half,
                   float depth,
                   float alpha,
                   std::string &error,
                   const char *view_name) override;
  bool write(const char *filepath,
             int compression,
             bool half,
             bool alpha_only,
             float depth,
             float alpha,
             std::string &error,
             uint64_t consumer_key = 0,
             const char *view_name = "") override;
  bool finish(std::string &error) override;
  void cancel();

 private:
  void cancel_unlocked();
  bool fail(const std::string &message, std::string &error);
  bool claim(const BlenderDeepFrameOutput &owner,
             uint64_t consumer,
             const char *path,
             int compression,
             const DeepCanonicalEncodeConfig &config,
             std::string &error);
  std::mutex mutex_;
  DeepViewRegistry<BlenderDeepFrameOutput> views_;
  std::shared_ptr<BlenderDeepOutputAddresses> addresses_;
  std::string first_error_;
  bool sealed_ = false;
};

CCL_NAMESPACE_END
