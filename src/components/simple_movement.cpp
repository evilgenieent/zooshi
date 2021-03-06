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

#include "components/simple_movement.h"
#include "corgi_component_library/transform.h"
#include "fplbase/flatbuffer_utils.h"
#include "fplbase/utilities.h"

CORGI_DEFINE_COMPONENT(fpl::zooshi::SimpleMovementComponent,
                       fpl::zooshi::SimpleMovementData)

namespace fpl {
namespace zooshi {

void SimpleMovementComponent::AddFromRawData(corgi::EntityRef& entity,
                                             const void* raw_data) {
  auto simple_movement_def = static_cast<const SimpleMovementDef*>(raw_data);
  SimpleMovementData* simple_movement_data = AddEntity(entity);
  simple_movement_data->velocity = LoadVec3(simple_movement_def->velocity());
}

void SimpleMovementComponent::UpdateAllEntities(corgi::WorldTime delta_time) {
  for (auto iter = component_data_.begin(); iter != component_data_.end();
       ++iter) {
    corgi::component_library::TransformData* transform_data =
        Data<corgi::component_library::TransformData>(iter->entity);
    SimpleMovementData* simple_movement_data =
        Data<SimpleMovementData>(iter->entity);

    transform_data->position +=
        (simple_movement_data->velocity * static_cast<float>(delta_time)) /
        1000.0f;
  }
}

corgi::ComponentInterface::RawDataUniquePtr
SimpleMovementComponent::ExportRawData(const corgi::EntityRef& entity) const {
  const SimpleMovementData* data = GetComponentData(entity);
  if (data == nullptr) return nullptr;

  flatbuffers::FlatBufferBuilder fbb;
  fplbase::Vec3 velocity(data->velocity.x, data->velocity.y,
                         data->velocity.z);

  fbb.Finish(CreateSimpleMovementDef(fbb, &velocity));
  return fbb.ReleaseBufferPointer();
}

void SimpleMovementComponent::InitEntity(corgi::EntityRef& entity) {
  entity_manager_
      ->AddEntityToComponent<corgi::component_library::TransformComponent>(
          entity);
}

}  // zooshi
}  // fpl
