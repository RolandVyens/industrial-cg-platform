/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "testing/testing.h"

#include "device/device.h"
#include "scene/scene.h"
#include "scene/shader_graph.h"
#include "scene/shader_nodes.h"
#include "scene/svm.h"
#include "util/progress.h"
#include "util/stats.h"

CCL_NAMESPACE_BEGIN

class VolumeEmissionDemand : public testing::Test {
 protected:
  Stats stats;
  Profiler profiler;
  unique_ptr<Device> device;
  unique_ptr<Scene> scene;
  Shader *shader = nullptr;
  Progress progress;

  void SetUp() override
  {
    ColorSpaceManager::init_fallback_config();
    device = Device::create(DeviceInfo(), stats, profiler, true);
    scene = make_unique<Scene>(SceneParams(), device.get());
    shader = scene->create_node<Shader>();
    shader->reference();
    shader->set_graph(make_unique<ShaderGraph>());
  }

  void connect_volume(ShaderNode *node)
  {
    shader->graph->connect(node->output("Volume"), shader->graph->output()->input("Volume"));
    shader->has_volume = true;
  }

  void compile()
  {
    shader->graph->finalize(scene.get());
    array<int> nodes;
    SVMCompiler compiler(scene.get(), progress);
    compiler.compile(shader, nodes, 0, nullptr);
    ASSERT_FALSE(nodes.empty());
  }
};

TEST_F(VolumeEmissionDemand, uncompiled_defaults_to_possible_emission)
{
  EXPECT_TRUE(shader->has_volume_emission);
  EXPECT_TRUE(scene->dscene.volume_may_emit);
}

TEST_F(VolumeEmissionDemand, scattering_ignores_surface_and_disconnected_emission)
{
  auto *scatter = shader->graph->create_node<ScatterVolumeNode>();
  connect_volume(scatter);
  auto *surface = shader->graph->create_node<EmissionNode>();
  shader->graph->connect(surface->output("Emission"), shader->graph->output()->input("Surface"));
  shader->has_surface = true;
  shader->graph->create_node<PrincipledVolumeNode>()->set_emission_strength(1.0f);
  compile();
  EXPECT_FALSE(shader->has_volume_emission);
}

TEST_F(VolumeEmissionDemand, reachable_emission_closure_requires_expression)
{
  auto *emission = shader->graph->create_node<EmissionNode>();
  shader->graph->connect(emission->output("Emission"), shader->graph->output()->input("Volume"));
  shader->has_volume = true;
  compile();
  EXPECT_TRUE(shader->has_volume_emission);
}

TEST_F(VolumeEmissionDemand, background_combined_with_volume_requires_expression)
{
  auto *scatter = shader->graph->create_node<ScatterVolumeNode>();
  auto *background = shader->graph->create_node<BackgroundNode>();
  auto *add = shader->graph->create_node<AddClosureNode>();
  shader->graph->connect(scatter->output("Volume"), add->input("Closure1"));
  shader->graph->connect(background->output("Background"), add->input("Closure2"));
  shader->graph->connect(add->output("Closure"), shader->graph->output()->input("Volume"));
  shader->has_volume = true;
  compile();
  EXPECT_TRUE(shader->has_volume_emission);
}

TEST_F(VolumeEmissionDemand, surface_background_does_not_require_volume_expression)
{
  auto *scatter = shader->graph->create_node<ScatterVolumeNode>();
  connect_volume(scatter);
  auto *background = shader->graph->create_node<BackgroundNode>();
  shader->graph->connect(background->output("Background"),
                         shader->graph->output()->input("Surface"));
  shader->has_surface = true;
  compile();
  EXPECT_FALSE(shader->has_volume_emission);
}

TEST_F(VolumeEmissionDemand, principled_exact_zero_strengths_prove_no_emission)
{
  auto *volume = shader->graph->create_node<PrincipledVolumeNode>();
  volume->set_emission_strength(0.0f);
  volume->set_blackbody_intensity(0.0f);
  connect_volume(volume);
  compile();
  EXPECT_FALSE(shader->has_volume_emission);
}

TEST_F(VolumeEmissionDemand, principled_nonzero_emission_and_recompile_to_zero)
{
  auto *volume = shader->graph->create_node<PrincipledVolumeNode>();
  volume->set_emission_strength(1.0f);
  connect_volume(volume);
  compile();
  EXPECT_TRUE(shader->has_volume_emission);
  volume->set_emission_strength(0.0f);
  compile();
  EXPECT_FALSE(shader->has_volume_emission);
  volume->set_blackbody_intensity(1.0f);
  compile();
  EXPECT_TRUE(shader->has_volume_emission);
}

TEST_F(VolumeEmissionDemand, principled_dynamic_emission_requires_expression)
{
  auto *volume = shader->graph->create_node<PrincipledVolumeNode>();
  auto *geometry = shader->graph->create_node<GeometryNode>();
  shader->graph->connect(geometry->output("Position"), volume->input("Emission Strength"));
  connect_volume(volume);
  compile();
  EXPECT_TRUE(shader->has_volume_emission);
}

TEST_F(VolumeEmissionDemand, principled_dynamic_blackbody_requires_expression)
{
  auto *volume = shader->graph->create_node<PrincipledVolumeNode>();
  auto *geometry = shader->graph->create_node<GeometryNode>();
  shader->graph->connect(geometry->output("Position"), volume->input("Blackbody Intensity"));
  connect_volume(volume);
  compile();
  EXPECT_TRUE(shader->has_volume_emission);
}

TEST_F(VolumeEmissionDemand, coefficients_zero_nonzero_and_dynamic)
{
  auto *volume = shader->graph->create_node<VolumeCoefficientsNode>();
  volume->set_emission_coeffs(zero_float3());
  connect_volume(volume);
  compile();
  EXPECT_FALSE(shader->has_volume_emission);
  volume->set_emission_coeffs(make_float3(0.0f, 0.1f, 0.0f));
  compile();
  EXPECT_TRUE(shader->has_volume_emission);
  volume->set_emission_coeffs(zero_float3());
  auto *geometry = shader->graph->create_node<GeometryNode>();
  shader->graph->connect(geometry->output("Position"), volume->input("Emission Coefficients"));
  compile();
  EXPECT_TRUE(shader->has_volume_emission);
}

CCL_NAMESPACE_END
