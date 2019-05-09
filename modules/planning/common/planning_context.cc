/******************************************************************************
 * Copyright 2017 The Apollo Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/

#include "modules/planning/common/planning_context.h"

#include "modules/planning/common/planning_gflags.h"

namespace apollo {
namespace planning {

// PlanningContext::PlanningStatus PlanningContext::planning_status_;
// PlanningContext::SidePassInfo PlanningContext::side_pass_info_;
// PlanningContext::FallBackInfo PlanningContext::fallback_info_;
// PlanningContext::OpenSpaceInfo PlanningContext::open_space_info_;

PlanningContext::PlanningContext() {}

void PlanningContext::Init() {}

void PlanningContext::Clear() {
  planning_status_.Clear();
  side_pass_info_ = {};
  fallback_info_ = {};
  open_space_info_ = {};
}

// int PlanningContext::front_static_obstacle_cycle_counter_ = 0;
// std::string PlanningContext::front_static_obstacle_id_ = "";
// int PlanningContext::able_to_use_self_lane_counter_ = 0;

// bool PlanningContext::is_in_path_lane_borrow_scenario_ = false;

}  // namespace planning
}  // namespace apollo
