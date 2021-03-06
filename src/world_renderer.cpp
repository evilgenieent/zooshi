// Copyright 2015 Google Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "world_renderer.h"

#include "components/light.h"
#include "components/services.h"
#include "corgi_component_library/transform.h"
#include "fplbase/debug_markers.h"
#include "fplbase/flatbuffer_utils.h"
#include "motive/math/angle.h"

using mathfu::vec2i;
using mathfu::vec2;
using mathfu::vec3;
using mathfu::vec4;
using mathfu::mat3;
using mathfu::mat4;
using mathfu::quat;
using motive::kDegreesToRadians;

namespace fpl {
namespace zooshi {

using corgi::component_library::RenderMeshComponent;
using corgi::component_library::TransformData;
using corgi::EntityRef;

// The texture ID that maps the one the shader is expecting. Any change to
// this constant must be mirrored in shadow_map.glslf_h texture_unit_<id>.
static const int kShadowMapTextureID = 7;

// RGB values should be near-max (1.0) to represent max depth, but the
// DecodeFloatFromRGBA function in shadow_map.glslf_h requires that the values
// are not at the max value.
static const vec4 kShadowMapClearColor = vec4(0.99f, 0.99f, 0.99f, 1.0f);

static const char *kDefinesText[] = {"PHONG_SHADING", "SPECULAR_EFFECT",
                                     "SHADOW_EFFECT", "NORMALS"};
static_assert(FPL_ARRAYSIZE(kDefinesText) == kNumShaderDefines,
              "Need to update kDefinesText");

const char *kEmptyString = "";

void WorldRenderer::Initialize(World *world) {
  int shadow_map_resolution =
      world->config->rendering_config()->shadow_map_resolution();
  shadow_map_.Initialize(
      mathfu::vec2i(shadow_map_resolution, shadow_map_resolution));

  RefreshGlobalShaderDefines(world);
}

void WorldRenderer::RefreshGlobalShaderDefines(World *world) {
  std::vector<std::string> defines_to_add;
  std::vector<std::string> defines_to_omit;
  for (int s = 0; s < kNumShaderDefines; ++s) {
    ShaderDefines shader_define = static_cast<ShaderDefines>(s);
    if (!world->RenderingOptionEnabled(shader_define)) {
      defines_to_omit.push_back(kDefinesText[shader_define]);
    }
  }

  world->asset_manager->ResetGlobalShaderDefines(defines_to_add,
                                                 defines_to_omit);

  PushDebugMarker("ShaderCompile");

  depth_shader_ = world->asset_manager->FindShader("shaders/render_depth");
  depth_skinned_shader_ =
      world->asset_manager->FindShader("shaders/render_depth_skinned");
  textured_shader_ = world->asset_manager->FindShader("shaders/textured");

  depth_shader_->ReloadIfDirty();
  depth_skinned_shader_->ReloadIfDirty();
  textured_shader_->ReloadIfDirty();

  PopDebugMarker();  // ShaderCompile

  world->ResetRenderingDirty();
}

void WorldRenderer::CreateShadowMap(const corgi::CameraInterface &camera,
                                    fplbase::Renderer &renderer, World *world) {
  PushDebugMarker("CreateShadowMap");

  PushDebugMarker("Setup");
  float shadow_map_resolution = static_cast<float>(
      world->config->rendering_config()->shadow_map_resolution());
  float shadow_map_zoom = world->config->rendering_config()->shadow_map_zoom();
  float shadow_map_offset =
      world->config->rendering_config()->shadow_map_offset();
  LightComponent *light_component =
      world->entity_manager.GetComponent<LightComponent>();

  const EntityRef &main_light_entity = light_component->begin()->entity;
  const TransformData *light_transform =
      world->entity_manager.GetComponentData<TransformData>(main_light_entity);
  vec3 light_position = light_transform->position;
  SetLightPosition(light_position);

  float viewport_angle =
      world->config->rendering_config()->shadow_map_viewport_angle() *
      kDegreesToRadians;
  light_camera_.set_viewport_angle(viewport_angle / shadow_map_zoom);
  light_camera_.set_viewport_resolution(
      vec2(shadow_map_resolution, shadow_map_resolution));
  vec3 light_camera_focus =
      camera.position() + camera.facing() * shadow_map_offset;
  light_camera_focus.z = 0;
  vec3 light_facing = light_camera_focus - light_camera_.position();
  light_camera_.set_facing(light_facing.Normalized());

  // Shadow map needs to be cleared to near-white, since that's
  // the maximum (furthest) depth.
  shadow_map_.SetAsRenderTarget();
  renderer.ClearFrameBuffer(kShadowMapClearColor);
  renderer.SetCulling(fplbase::kCullingModeBack);

  depth_shader_->Set(renderer);
  depth_skinned_shader_->Set(renderer);
  // Generate the shadow map:
  // TODO - modify this so that shadowcast is its own render pass
  PopDebugMarker(); // Setup

  for (int pass = 0; pass < corgi::RenderPass_Count; pass++) {
    PushDebugMarker("RenderPass");
    world->render_mesh_component.RenderPass(pass, light_camera_, renderer,
                                            ShaderIndex_Depth);
    PopDebugMarker();
  }

  fplbase::RenderTarget::ScreenRenderTarget(renderer).SetAsRenderTarget();
  PopDebugMarker(); // CreateShadowMap
}

void WorldRenderer::RenderPrep(const corgi::CameraInterface &camera,
                               World *world) {
  world->render_mesh_component.RenderPrep(camera);
}

// Draw the shadow map in the world, so we can see it.
void WorldRenderer::DebugShowShadowMap(const corgi::CameraInterface &camera,
                                       fplbase::Renderer &renderer) {
  fplbase::RenderTarget::ScreenRenderTarget(renderer).SetAsRenderTarget();

  static const mat4 kDebugTextureWorldTransform =
      mat4::FromScaleVector(mathfu::vec3(10.0f, 10.0f, 10.0f));

  const mat4 mvp = camera.GetTransformMatrix() * kDebugTextureWorldTransform;
  const mat4 world_matrix_inverse = kDebugTextureWorldTransform.Inverse();

  renderer.set_camera_pos(world_matrix_inverse * camera.position());
  renderer.set_light_pos(world_matrix_inverse * light_camera_.position());
  renderer.set_model_view_projection(mvp);
  renderer.set_color(vec4(1.0f, 1.0f, 1.0f, 1.0f));

  shadow_map_.BindAsTexture(0);

  textured_shader_->Set(renderer);

  // Render a large quad in the world, with the shadowmap texture on it:
  fplbase::Mesh::RenderAAQuadAlongX(vec3(0.0f, 0.0f, 0.0f),
                                    vec3(10.0f, 0.0f, 10.0f), vec2(1.0f, 0.0f),
                                    vec2(0.0f, 1.0f));
}

void WorldRenderer::SetFogUniforms(fplbase::Shader *shader, World *world) {
  shader->SetUniform("fog_roll_in_dist",
                     world->config->rendering_config()->fog_roll_in_dist());
  shader->SetUniform("fog_max_dist",
                     world->config->rendering_config()->fog_max_dist());
  shader->SetUniform(
      "fog_color",
      LoadColorRGBA(world->config->rendering_config()->fog_color()));
  shader->SetUniform("fog_max_saturation",
                     world->config->rendering_config()->fog_max_saturation());
}

void WorldRenderer::SetLightingUniforms(fplbase::Shader *shader, World *world) {
  LightComponent *light_component =
      world->entity_manager.GetComponent<LightComponent>();
  const EntityRef &main_light_entity = light_component->begin()->entity;
  const LightData *light_data =
      world->entity_manager.GetComponentData<LightData>(main_light_entity);

  if (world->RenderingOptionEnabled(kShadowEffect)) {
    shader->SetUniform("shadow_intensity", light_data->shadow_intensity);
  }
  shader->SetUniform("ambient_material",
                     light_data->ambient_color * light_data->ambient_intensity);
  shader->SetUniform("diffuse_material",
                     light_data->diffuse_color * light_data->diffuse_intensity);
  shader->SetUniform("specular_material", light_data->specular_color *
                                              light_data->specular_intensity);
  shader->SetUniform("shininess", light_data->specular_exponent);
}

void WorldRenderer::RenderShadowMap(const corgi::CameraInterface &camera,
                                    fplbase::Renderer &renderer, World *world) {
  PushDebugMarker("Render ShadowMap");

  PushDebugMarker("Scene Setup");
  if (world->RenderingOptionsDirty()) {
    RefreshGlobalShaderDefines(world);
  }

  float shadow_map_bias = world->config->rendering_config()->shadow_map_bias();
  depth_shader_->SetUniform("bias", shadow_map_bias);
  depth_skinned_shader_->SetUniform("bias", shadow_map_bias);
  PopDebugMarker(); // Scene Setup

  CreateShadowMap(camera, renderer, world);

  PopDebugMarker(); // Render ShadowMap
}

void WorldRenderer::RenderWorld(const corgi::CameraInterface &camera,
                                fplbase::Renderer &renderer, World *world) {
  PushDebugMarker("Render World");

  PushDebugMarker("Scene Setup");
  if (world->RenderingOptionsDirty()) {
    RefreshGlobalShaderDefines(world);
  }

  mat4 camera_transform = camera.GetTransformMatrix();
  renderer.set_color(mathfu::kOnes4f);
  renderer.SetDepthFunction(fplbase::kDepthFunctionLess);
  renderer.set_model_view_projection(camera_transform);

  float texture_repeats =
      world->CurrentLevel()->river_config()->texture_repeats();
  float river_offset = world->river_component.river_offset();

  if (world->RenderingOptionEnabled(kShadowEffect)) {
    world->asset_manager->ForEachShaderWithDefine(
        kDefinesText[kShadowEffect], [&](fplbase::Shader *shader) {
          shader->SetUniform("view_projection", camera_transform);
          shader->SetUniform("light_view_projection",
                             light_camera_.GetTransformMatrix());
        });
  }

  world->asset_manager->ForEachShaderWithDefine(
      "WATER", [&](fplbase::Shader *shader) {
        shader->SetUniform("river_offset", river_offset);
        shader->SetUniform("texture_repeats", texture_repeats);
      });

  world->asset_manager->ForEachShaderWithDefine(
      kDefinesText[kPhongShading],
      [&](fplbase::Shader *shader) { SetLightingUniforms(shader, world); });

  world->asset_manager->ForEachShaderWithDefine(
      "FOG_EFFECT",
      [&](fplbase::Shader *shader) { SetFogUniforms(shader, world); });

  shadow_map_.BindAsTexture(kShadowMapTextureID);
  PopDebugMarker(); // Scene Setup

  if (!world->skip_rendermesh_rendering) {
    for (int pass = 0; pass < corgi::RenderPass_Count; pass++) {
      PushDebugMarker("RenderPass");
      world->render_mesh_component.RenderPass(pass, camera, renderer);
      PopDebugMarker();
    }
  }

  if (world->draw_debug_physics) {
    PushDebugMarker("Debug Draw World");
    world->physics_component.DebugDrawWorld(&renderer, camera_transform);
    PopDebugMarker();
  }

  PushDebugMarker("Text");
  world->render_3d_text_component.RenderAllEntities(camera);
  PopDebugMarker();

  PopDebugMarker(); // Render World
}

}  // zooshi
}  // fpl
