/******************************************************************************
 * Copyright 2019 The Apollo Authors. All Rights Reserved.
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

/**
 * @file
 **/

#include "modules/planning/tasks/deciders/open_space_roi_decider.h"

namespace apollo {
namespace planning {

using apollo::common::ErrorCode;
using apollo::common::Status;
using apollo::common::math::Box2d;
using apollo::common::math::CrossProd;
using apollo::common::math::Polygon2d;
using apollo::common::math::Vec2d;
using apollo::hdmap::HDMapUtil;
using apollo::hdmap::LaneInfoConstPtr;
using apollo::hdmap::LaneSegment;
using apollo::hdmap::ParkingSpaceInfoConstPtr;
using apollo::hdmap::Path;

OpenSpaceRoiDecider::OpenSpaceRoiDecider(const TaskConfig &config)
    : Decider(config) {
  hdmap_ = hdmap::HDMapUtil::BaseMapPtr();
  CHECK_NOTNULL(hdmap_);
  vehicle_params_ =
      apollo::common::VehicleConfigHelper::GetConfig().vehicle_param();
}

Status OpenSpaceRoiDecider::Process(Frame *frame) {
  if (frame == nullptr) {
    const std::string msg =
        "Invalid frame, fail to process the OpenSpaceRoiDecider.";
    AERROR << msg;
    return Status(ErrorCode::PLANNING_ERROR, msg);
  }

  vehicle_state_ = frame->vehicle_state();
  obstacles_by_frame_ = frame->GetObstacleList();
  const auto &routing_request = frame->local_view().routing->routing_request();

  // TODO(Jinyun): support options on creating dead end boundary
  if (routing_request.has_parking_space() &&
      routing_request.parking_space().has_id()) {
    target_parking_spot_id_ = routing_request.parking_space().id().id();
  } else {
    const std::string msg = "Failed to get parking space id from routing";
    AERROR << msg;
    return Status(ErrorCode::PLANNING_ERROR, msg);
  }

  // @brief vector of different obstacle consisting of vertice points.The
  // obstacle and the vertices order are in counter-clockwise order
  std::vector<std::vector<common::math::Vec2d>> roi_parking_boundary;

  if (!GetParkingBoundary(frame, &roi_parking_boundary)) {
    const std::string msg = "Fail to get parking boundary from map";
    AERROR << msg;
    return Status(ErrorCode::PLANNING_ERROR, msg);
  }

  if (!FormulateBoundaryConstraints(roi_parking_boundary, frame)) {
    const std::string msg = "Fail to formulate boundary constraints";
    AERROR << msg;
    return Status(ErrorCode::PLANNING_ERROR, msg);
  }

  return Status::OK();
}

bool OpenSpaceRoiDecider::GetParkingBoundary(
    Frame *const frame,
    std::vector<std::vector<common::math::Vec2d>> *const roi_parking_boundary) {
  // Find parking spot by getting nearestlane
  ParkingSpaceInfoConstPtr target_parking_spot = nullptr;
  std::shared_ptr<Path> nearby_path = nullptr;
  if (!GetParkingSpotFromMap(frame, &target_parking_spot, &nearby_path)) {
    AERROR << "fail to get map info in open space planner";
    return false;
  }

  // left or right of the parking lot is decided when viewing the parking spot
  // open upward
  Vec2d left_top = target_parking_spot->polygon().points().at(3);
  Vec2d left_down = target_parking_spot->polygon().points().at(0);
  Vec2d right_top = target_parking_spot->polygon().points().at(2);
  Vec2d right_down = target_parking_spot->polygon().points().at(1);
  double left_top_s = 0.0;
  double left_top_l = 0.0;
  double right_top_s = 0.0;
  double right_top_l = 0.0;
  double average_l = 0.0;
  if (!(nearby_path->GetProjection(left_top, &left_top_s, &left_top_l) &&
        nearby_path->GetProjection(right_top, &right_top_s, &right_top_l))) {
    AERROR << "fail to get parking spot points' projections on reference line";
    return false;
  }
  average_l = (left_top_l + right_top_l) / 2.0;
  // start or end, left or right of the lane and s is decided by the lane's
  // heading
  double center_line_s = (left_top_s + right_top_s) / 2.0;
  double start_s =
      center_line_s -
      config_.open_space_roi_decider_config().roi_longitudinal_range();
  double end_s =
      center_line_s +
      config_.open_space_roi_decider_config().roi_longitudinal_range();

  std::vector<Vec2d> left_lane_boundary;
  std::vector<Vec2d> right_lane_boundary;
  std::vector<Vec2d> center_lane_boundary;
  std::vector<double> center_lane_s;
  std::vector<double> left_lane_road_width;
  std::vector<double> right_lane_road_width;

  hdmap::MapPathPoint start_point = nearby_path->GetSmoothPoint(start_s);
  double last_check_point_heading = start_point.heading();
  double index = 0.0;
  double check_point_s = start_s;
  while (check_point_s <= end_s) {
    hdmap::MapPathPoint check_point =
        nearby_path->GetSmoothPoint(check_point_s);
    double check_point_heading = check_point.heading();
    if (std::abs(common::math::NormalizeAngle(check_point_heading -
                                              last_check_point_heading)) <
            config_.open_space_roi_decider_config()
                .roi_linesegment_min_angle() &&
        check_point_s != start_s && check_point_s != end_s) {
      index += 1.0;
      check_point_s =
          index *
          config_.open_space_roi_decider_config().roi_linesegment_length();
      check_point_s = check_point_s >= end_s ? end_s : check_point_s;
      last_check_point_heading = check_point_heading;
      continue;
    }
    double point_left_road_width = nearby_path->GetRoadLeftWidth(check_point_s);
    double point_right_road_width =
        nearby_path->GetRoadRightWidth(check_point_s);
    double point_right_vec_cos = std::cos(check_point_heading - M_PI / 2.0);
    double point_right_vec_sin = std::sin(check_point_heading - M_PI / 2.0);
    double point_left_vec_cos = std::cos(check_point_heading + M_PI / 2.0);
    double point_left_vec_sin = std::sin(check_point_heading + M_PI / 2.0);
    Vec2d right_lane_point =
        Vec2d(point_right_road_width * point_right_vec_cos,
              point_right_road_width * point_right_vec_sin);
    right_lane_point = right_lane_point + check_point;
    Vec2d left_lane_point = Vec2d(point_left_road_width * point_left_vec_cos,
                                  point_left_road_width * point_left_vec_sin);
    left_lane_point = left_lane_point + check_point;
    right_lane_boundary.push_back(right_lane_point);
    left_lane_boundary.push_back(left_lane_point);
    center_lane_boundary.push_back(check_point);
    center_lane_s.push_back(check_point_s);
    left_lane_road_width.push_back(point_left_road_width);
    right_lane_road_width.push_back(point_right_road_width);
    if (check_point_s == end_s) {
      break;
    }
    index += 1.0;
    check_point_s =
        index *
        config_.open_space_roi_decider_config().roi_linesegment_length();
    check_point_s = check_point_s >= end_s ? end_s : check_point_s;
    last_check_point_heading = check_point_heading;
  }

  // rotate the points to have the lane to be horizontal to x axis positive
  // direction and scale them base on the origin point
  Vec2d heading_vec = right_top - left_top;
  frame->mutable_open_space_info()->set_origin_heading(heading_vec.Angle());
  frame->mutable_open_space_info()->mutable_origin_point()->set_x(left_top.x());
  frame->mutable_open_space_info()->mutable_origin_point()->set_y(left_top.y());
  const auto &origin_point = frame->open_space_info().origin_point();
  const auto &origin_heading = frame->open_space_info().origin_heading();

  DCHECK_EQ(right_lane_boundary.size(), left_lane_boundary.size());
  DCHECK_EQ(center_lane_boundary.size(), left_lane_boundary.size());
  size_t point_size = right_lane_boundary.size();
  for (size_t i = 0; i < point_size; i++) {
    right_lane_boundary[i] -= origin_point;
    right_lane_boundary[i].SelfRotate(-origin_heading);
    left_lane_boundary[i] -= origin_point;
    left_lane_boundary[i].SelfRotate(-origin_heading);
  }
  left_top -= origin_point;
  left_top.SelfRotate(-origin_heading);
  left_down -= origin_point;
  left_down.SelfRotate(-origin_heading);
  right_top -= origin_point;
  right_top.SelfRotate(-origin_heading);
  right_down -= origin_point;
  right_down.SelfRotate(-origin_heading);

  // If smaller than zero, the parking spot is on the right of the lane
  // Left, right, down or opposite of the boundary is decided when viewing the
  // parking spot upward
  std::vector<Vec2d> boundary_points;
  if (average_l < 0) {
    // if average_l is lower than zero, the parking spot is on the right
    // lane boundary and assume that the lane half width is average_l
    size_t point_size = right_lane_boundary.size();
    for (size_t i = 0; i < point_size; i++) {
      right_lane_boundary[i].SelfRotate(origin_heading);
      right_lane_boundary[i] += origin_point;
      right_lane_boundary[i] -= center_lane_boundary[i];
      right_lane_boundary[i] /= right_lane_road_width[i];
      right_lane_boundary[i] *= (-average_l);
      right_lane_boundary[i] += center_lane_boundary[i];
      right_lane_boundary[i] -= origin_point;
      right_lane_boundary[i].SelfRotate(-origin_heading);
    }

    auto point_left_to_left_top_connor_s = std::lower_bound(
        center_lane_s.begin(), center_lane_s.end(), left_top_s);
    size_t point_left_to_left_top_connor_index =
        std::distance(center_lane_s.begin(), point_left_to_left_top_connor_s);
    point_left_to_left_top_connor_index =
        point_left_to_left_top_connor_index == 0
            ? point_left_to_left_top_connor_index
            : point_left_to_left_top_connor_index - 1;
    auto point_left_to_left_top_connor_itr =
        right_lane_boundary.begin() + point_left_to_left_top_connor_index;
    auto point_right_to_right_top_connor_s = std::upper_bound(
        center_lane_s.begin(), center_lane_s.end(), right_top_s);
    size_t point_right_to_right_top_connor_index =
        std::distance(center_lane_s.begin(), point_right_to_right_top_connor_s);
    auto point_right_to_right_top_connor_itr =
        right_lane_boundary.begin() + point_right_to_right_top_connor_index;

    std::copy(right_lane_boundary.begin(), point_left_to_left_top_connor_itr,
              std::back_inserter(boundary_points));

    std::vector<Vec2d> parking_spot_boundary{left_top, left_down, right_down,
                                             right_top};

    std::copy(parking_spot_boundary.begin(), parking_spot_boundary.end(),
              std::back_inserter(boundary_points));

    std::copy(point_right_to_right_top_connor_itr, right_lane_boundary.end(),
              std::back_inserter(boundary_points));

    std::reverse_copy(left_lane_boundary.begin(), left_lane_boundary.end(),
                      std::back_inserter(boundary_points));

    // reinsert the initial point to the back to from closed loop
    boundary_points.push_back(right_lane_boundary.front());

    // disassemble line into line2d segments
    for (size_t i = 0; i < point_left_to_left_top_connor_index; i++) {
      std::vector<Vec2d> segment{right_lane_boundary[i],
                                 right_lane_boundary[i + 1]};
      roi_parking_boundary->push_back(segment);
    }

    std::vector<Vec2d> left_stitching_segment{
        right_lane_boundary[point_left_to_left_top_connor_index], left_top};
    roi_parking_boundary->push_back(left_stitching_segment);

    std::vector<Vec2d> left_parking_spot_segment{left_top, left_down};
    std::vector<Vec2d> down_parking_spot_segment{left_down, right_down};
    std::vector<Vec2d> right_parking_spot_segment{right_down, right_top};
    roi_parking_boundary->push_back(left_parking_spot_segment);
    roi_parking_boundary->push_back(down_parking_spot_segment);
    roi_parking_boundary->push_back(right_parking_spot_segment);

    std::vector<Vec2d> right_stitching_segment{
        right_top, right_lane_boundary[point_right_to_right_top_connor_index]};
    roi_parking_boundary->push_back(right_stitching_segment);

    size_t right_lane_boundary_last_index = right_lane_boundary.size() - 1;
    for (size_t i = point_right_to_right_top_connor_index;
         i < right_lane_boundary_last_index; i++) {
      std::vector<Vec2d> segment{right_lane_boundary[i],
                                 right_lane_boundary[i + 1]};
      roi_parking_boundary->push_back(segment);
    }

    size_t left_lane_boundary_last_index = left_lane_boundary.size() - 1;
    for (size_t i = left_lane_boundary_last_index; i > 0; i--) {
      std::vector<Vec2d> segment{left_lane_boundary[i],
                                 left_lane_boundary[i - 1]};
      roi_parking_boundary->push_back(segment);
    }

  } else {
    // if average_l is higher than zero, the parking spot is on the left
    // lane boundary and assume that the lane half width is average_l
    size_t point_size = left_lane_boundary.size();
    for (size_t i = 0; i < point_size; i++) {
      left_lane_boundary[i].SelfRotate(origin_heading);
      left_lane_boundary[i] += origin_point;
      left_lane_boundary[i] -= center_lane_boundary[i];
      left_lane_boundary[i] /= left_lane_road_width[i];
      left_lane_boundary[i] *= average_l;
      left_lane_boundary[i] += center_lane_boundary[i];
      left_lane_boundary[i] -= origin_point;
      left_lane_boundary[i].SelfRotate(-origin_heading);
    }
    auto point_right_to_right_top_connor_s = std::lower_bound(
        center_lane_s.begin(), center_lane_s.end(), right_top_s);
    size_t point_right_to_right_top_connor_index =
        std::distance(center_lane_s.begin(), point_right_to_right_top_connor_s);
    point_right_to_right_top_connor_index =
        point_right_to_right_top_connor_index <= 0
            ? point_right_to_right_top_connor_index
            : point_right_to_right_top_connor_index - 1;
    auto point_right_to_right_top_connor_itr =
        left_lane_boundary.begin() + point_right_to_right_top_connor_index;

    auto point_left_to_left_top_connor_s = std::upper_bound(
        center_lane_s.begin(), center_lane_s.end(), left_top_s);
    size_t point_left_to_left_top_connor_index =
        std::distance(center_lane_s.begin(), point_left_to_left_top_connor_s);
    auto point_left_to_left_top_connor_itr =
        left_lane_boundary.begin() + point_left_to_left_top_connor_index;

    std::copy(right_lane_boundary.begin(), right_lane_boundary.end(),
              std::back_inserter(boundary_points));

    std::reverse_copy(point_left_to_left_top_connor_itr,
                      left_lane_boundary.end(),
                      std::back_inserter(boundary_points));

    std::vector<Vec2d> parking_spot_boundary{left_top, left_down, right_down,
                                             right_top};
    std::copy(parking_spot_boundary.begin(), parking_spot_boundary.end(),
              std::back_inserter(boundary_points));

    std::reverse_copy(left_lane_boundary.begin(),
                      point_right_to_right_top_connor_itr,
                      std::back_inserter(boundary_points));

    // reinsert the initial point to the back to from closed loop
    boundary_points.push_back(right_lane_boundary.front());

    // disassemble line into line2d segments
    size_t right_lane_boundary_last_index = right_lane_boundary.size() - 1;
    for (size_t i = 0; i < right_lane_boundary_last_index; i++) {
      std::vector<Vec2d> segment{right_lane_boundary[i],
                                 right_lane_boundary[i + 1]};
      roi_parking_boundary->push_back(segment);
    }

    size_t left_lane_boundary_last_index = left_lane_boundary.size() - 1;
    for (size_t i = left_lane_boundary_last_index;
         i > point_left_to_left_top_connor_index; i--) {
      std::vector<Vec2d> segment{left_lane_boundary[i],
                                 left_lane_boundary[i - 1]};
      roi_parking_boundary->push_back(segment);
    }

    std::vector<Vec2d> left_stitching_segment{
        left_lane_boundary[point_left_to_left_top_connor_index], left_top};
    roi_parking_boundary->push_back(left_stitching_segment);

    std::vector<Vec2d> left_parking_spot_segment{left_top, left_down};
    std::vector<Vec2d> down_parking_spot_segment{left_down, right_down};
    std::vector<Vec2d> right_parking_spot_segment{right_down, right_top};
    roi_parking_boundary->push_back(left_parking_spot_segment);
    roi_parking_boundary->push_back(down_parking_spot_segment);
    roi_parking_boundary->push_back(right_parking_spot_segment);

    std::vector<Vec2d> right_stitching_segment{
        right_top, left_lane_boundary[point_right_to_right_top_connor_index]};
    roi_parking_boundary->push_back(right_stitching_segment);

    for (size_t i = point_right_to_right_top_connor_index; i > 0; --i) {
      std::vector<Vec2d> segment{left_lane_boundary[i],
                                 left_lane_boundary[i - 1]};
      roi_parking_boundary->push_back(segment);
    }
  }

  // Fuse line segments into convex contraints
  if (!FuseLineSegments(roi_parking_boundary)) {
    return false;
  }
  // Get xy boundary
  auto xminmax = std::minmax_element(
      boundary_points.begin(), boundary_points.end(),
      [](const Vec2d &a, const Vec2d &b) { return a.x() < b.x(); });
  auto yminmax = std::minmax_element(
      boundary_points.begin(), boundary_points.end(),
      [](const Vec2d &a, const Vec2d &b) { return a.y() < b.y(); });
  std::vector<double> ROI_xy_boundary{xminmax.first->x(), xminmax.second->x(),
                                      yminmax.first->y(), yminmax.second->y()};
  auto *xy_boundary =
      frame->mutable_open_space_info()->mutable_ROI_xy_boundary();
  xy_boundary->assign(ROI_xy_boundary.begin(), ROI_xy_boundary.end());

  Vec2d vehicle_xy = Vec2d(vehicle_state_.x(), vehicle_state_.y());
  vehicle_xy -= origin_point;
  vehicle_xy.SelfRotate(-origin_heading);
  if (vehicle_xy.x() < ROI_xy_boundary[0] ||
      vehicle_xy.x() > ROI_xy_boundary[1] ||
      vehicle_xy.y() < ROI_xy_boundary[2] ||
      vehicle_xy.y() > ROI_xy_boundary[3]) {
    AERROR << "vehicle outside of xy boundary of parking ROI";
    return false;
  }

  // Get end_pose of the parking spot
  double parking_spot_heading = (left_down - left_top).Angle();
  double end_x = (left_top.x() + right_top.x()) / 2.0;
  double end_y = 0.0;
  const double parking_depth_buffer =
      config_.open_space_roi_decider_config().parking_depth_buffer();
  CHECK_GE(parking_depth_buffer, 0.0);
  const bool parking_inwards =
      config_.open_space_roi_decider_config().parking_inwards();
  const double top_to_down_distance = left_top.y() - left_down.y();
  if (parking_spot_heading > common::math::kMathEpsilon) {
    if (parking_inwards) {
      end_y =
          left_down.y() - (std::max(3.0 * -top_to_down_distance / 4.0,
                                    vehicle_params_.front_edge_to_center()) +
                           parking_depth_buffer);

    } else {
      end_y = left_down.y() - (std::max(-top_to_down_distance / 4.0,
                                        vehicle_params_.back_edge_to_center()) +
                               parking_depth_buffer);
    }
  } else {
    if (parking_inwards) {
      end_y =
          left_down.y() + (std::max(3.0 * top_to_down_distance / 4.0,
                                    vehicle_params_.front_edge_to_center()) +
                           parking_depth_buffer);
    } else {
      end_y = left_down.y() + (std::max(top_to_down_distance / 4.0,
                                        vehicle_params_.back_edge_to_center()) +
                               parking_depth_buffer);
    }
  }

  auto *end_pose =
      frame->mutable_open_space_info()->mutable_open_space_end_pose();
  end_pose->push_back(end_x);
  end_pose->push_back(end_y);
  if (config_.open_space_roi_decider_config().parking_inwards()) {
    end_pose->push_back(parking_spot_heading);
  } else {
    end_pose->push_back(
        common::math::NormalizeAngle(parking_spot_heading + M_PI));
  }
  end_pose->push_back(0.0);

  return true;
}

bool OpenSpaceRoiDecider::GetParkingSpotFromMap(
    Frame *const frame, ParkingSpaceInfoConstPtr *target_parking_spot,
    std::shared_ptr<Path> *nearby_path) {
  if (frame == nullptr) {
    AERROR << "Invalid frame, fail to GetParkingSpotFromMap from frame. ";
    return false;
  }

  auto point = common::util::MakePointENU(
      vehicle_state_.x(), vehicle_state_.y(), vehicle_state_.z());
  LaneInfoConstPtr nearest_lane;
  double vehicle_lane_s = 0.0;
  double vehicle_lane_l = 0.0;
  // Check if last frame lane is avaiable
  const auto &ptr_last_frame = FrameHistory::Instance()->Latest();

  if (ptr_last_frame == nullptr) {
    AERROR << "Last frame failed, fail to GetParkingSpotFromMap from frame "
              "history.";
    return false;
  }

  const auto &previous_open_space_info = ptr_last_frame->open_space_info();
  if (previous_open_space_info.target_parking_lane() != nullptr &&
      previous_open_space_info.target_parking_spot_id() ==
          frame->open_space_info().target_parking_spot_id()) {
    nearest_lane = previous_open_space_info.target_parking_lane();
  } else {
    int status = HDMapUtil::BaseMap().GetNearestLaneWithHeading(
        point, 10.0, vehicle_state_.heading(), M_PI / 2.0, &nearest_lane,
        &vehicle_lane_s, &vehicle_lane_l);
    if (status != 0) {
      AERROR
          << "Getlane failed at OpenSpaceRoiDecider::GetParkingSpotFromMap()";
      return false;
    }
  }
  frame->mutable_open_space_info()->set_target_parking_lane(nearest_lane);
  LaneSegment nearest_lanesegment =
      LaneSegment(nearest_lane, nearest_lane->accumulate_s().front(),
                  nearest_lane->accumulate_s().back());
  std::vector<LaneSegment> segments_vector;
  int next_lanes_num = nearest_lane->lane().successor_id_size();
  if (next_lanes_num != 0) {
    for (int i = 0; i < next_lanes_num; ++i) {
      auto next_lane_id = nearest_lane->lane().successor_id(i);
      segments_vector.push_back(nearest_lanesegment);
      auto next_lane = hdmap_->GetLaneById(next_lane_id);
      LaneSegment next_lanesegment =
          LaneSegment(next_lane, next_lane->accumulate_s().front(),
                      next_lane->accumulate_s().back());
      segments_vector.push_back(next_lanesegment);
      (*nearby_path).reset(new Path(segments_vector));
      SearchTargetParkingSpotOnPath(**nearby_path, target_parking_spot);
      if (*target_parking_spot != nullptr) {
        break;
      }
    }
  } else {
    segments_vector.push_back(nearest_lanesegment);
    (*nearby_path).reset(new Path(segments_vector));
    SearchTargetParkingSpotOnPath(**nearby_path, target_parking_spot);
  }

  if (*target_parking_spot == nullptr) {
    AERROR << "No such parking spot found after searching all path forward "
              "possible";
    return false;
  }

  if (!CheckDistanceToParkingSpot(**nearby_path, *target_parking_spot)) {
    AERROR << "target parking spot found, but too far, distance larger than "
              "pre-defined distance";
    return false;
  }

  return true;
}

void OpenSpaceRoiDecider::SearchTargetParkingSpotOnPath(
    const hdmap::Path &nearby_path,
    ParkingSpaceInfoConstPtr *target_parking_spot) {
  const auto &parking_space_overlaps = nearby_path.parking_space_overlaps();
  for (const auto &parking_overlap : parking_space_overlaps) {
    if (parking_overlap.object_id == target_parking_spot_id_) {
      hdmap::Id id;
      id.set_id(parking_overlap.object_id);
      *target_parking_spot = hdmap_->GetParkingSpaceById(id);
    }
  }
}

bool OpenSpaceRoiDecider::CheckDistanceToParkingSpot(
    const hdmap::Path &nearby_path,
    const hdmap::ParkingSpaceInfoConstPtr &target_parking_spot) {
  Vec2d left_bottom_point = target_parking_spot->polygon().points().at(0);
  Vec2d right_bottom_point = target_parking_spot->polygon().points().at(1);
  double left_bottom_point_s = 0.0;
  double left_bottom_point_l = 0.0;
  double right_bottom_point_s = 0.0;
  double right_bottom_point_l = 0.0;
  double vehicle_point_s = 0.0;
  double vehicle_point_l = 0.0;
  nearby_path.GetNearestPoint(left_bottom_point, &left_bottom_point_s,
                              &left_bottom_point_l);
  nearby_path.GetNearestPoint(right_bottom_point, &right_bottom_point_s,
                              &right_bottom_point_l);
  Vec2d vehicle_vec(vehicle_state_.x(), vehicle_state_.y());
  nearby_path.GetNearestPoint(vehicle_vec, &vehicle_point_s, &vehicle_point_l);
  if (std::abs((left_bottom_point_s + right_bottom_point_s) / 2 -
               vehicle_point_s) <
      config_.open_space_roi_decider_config().parking_start_range()) {
    return true;
  } else {
    return false;
  }
}

bool OpenSpaceRoiDecider::FuseLineSegments(
    std::vector<std::vector<common::math::Vec2d>> *line_segments_vec) {
  constexpr double kEpsilon = 1.0e-8;
  auto cur_segment = line_segments_vec->begin();
  while (cur_segment != line_segments_vec->end() - 1) {
    auto next_segment = cur_segment + 1;
    auto cur_last_point = cur_segment->back();
    auto next_first_point = next_segment->front();
    // Check if they are the same points
    if (cur_last_point.DistanceTo(next_first_point) > kEpsilon) {
      ++cur_segment;
      continue;
    }
    if (cur_segment->size() < 2 || next_segment->size() < 2) {
      AERROR << "Single point line_segments vec not expected";
      return false;
    }
    size_t cur_segments_size = cur_segment->size();
    auto cur_second_to_last_point = cur_segment->at(cur_segments_size - 2);
    auto next_second_point = next_segment->at(1);
    if (CrossProd(cur_second_to_last_point, cur_last_point, next_second_point) <
        0.0) {
      cur_segment->push_back(next_second_point);
      next_segment->erase(next_segment->begin(), next_segment->begin() + 2);
      if (next_segment->empty()) {
        line_segments_vec->erase(next_segment);
      }
    } else {
      ++cur_segment;
    }
  }
  return true;
}

bool OpenSpaceRoiDecider::FormulateBoundaryConstraints(
    const std::vector<std::vector<common::math::Vec2d>> &roi_parking_boundary,
    Frame *const frame) {
  // Gather vertice needed by warm start and distance approach
  if (!LoadObstacleInVertices(roi_parking_boundary, frame)) {
    AERROR << "fail at LoadObstacleInVertices()";
    return false;
  }
  // Transform vertices into the form of Ax>b
  if (!LoadObstacleInHyperPlanes(frame)) {
    AERROR << "fail at LoadObstacleInHyperPlanes()";
    return false;
  }
  return true;
}

bool OpenSpaceRoiDecider::LoadObstacleInVertices(
    const std::vector<std::vector<common::math::Vec2d>> &roi_parking_boundary,
    Frame *const frame) {
  auto *mutable_open_space_info = frame->mutable_open_space_info();
  const auto &open_space_info = frame->open_space_info();
  auto *obstacles_vertices_vec =
      mutable_open_space_info->mutable_obstacles_vertices_vec();
  auto *obstacles_edges_num_vec =
      mutable_open_space_info->mutable_obstacles_edges_num();

  // load vertices for parking boundary (not need to repeat the first
  // vertice to get close hull)
  size_t parking_boundaries_num = roi_parking_boundary.size();
  size_t perception_obstacles_num = 0;

  for (size_t i = 0; i < parking_boundaries_num; ++i) {
    obstacles_vertices_vec->push_back(roi_parking_boundary[i]);
  }

  Eigen::MatrixXi parking_boundaries_obstacles_edges_num(parking_boundaries_num,
                                                         1);
  for (size_t i = 0; i < parking_boundaries_num; i++) {
    CHECK_GT(roi_parking_boundary[i].size(), 1);
    parking_boundaries_obstacles_edges_num(i, 0) =
        static_cast<int>(roi_parking_boundary[i].size()) - 1;
  }

  if (config_.open_space_roi_decider_config().enable_perception_obstacles()) {
    if (perception_obstacles_num == 0) {
      ADEBUG << "no obstacle given by perception";
    }

    // load vertices for perception obstacles(repeat the first vertice at the
    // last to form closed convex hull)
    const auto &origin_point = open_space_info.origin_point();
    const auto &origin_heading = open_space_info.origin_heading();
    for (const auto &obstacle : obstacles_by_frame_->Items()) {
      if (FilterOutObstacle(*frame, *obstacle)) {
        continue;
      }
      ++perception_obstacles_num;

      Box2d original_box = obstacle->PerceptionBoundingBox();
      original_box.Shift(-1.0 * origin_point);
      original_box.LongitudinalExtend(
          config_.open_space_roi_decider_config().perception_obstacle_buffer());
      original_box.LateralExtend(
          config_.open_space_roi_decider_config().perception_obstacle_buffer());

      // TODO(Jinyun): Check correctness of ExpandByDistance() in polygon
      // Polygon2d buffered_box(original_box);
      // buffered_box = buffered_box.ExpandByDistance(
      //     config_.open_space_roi_decider_config().perception_obstacle_buffer());
      // TODO(Runxin): Rotate from origin instead
      // original_box.RotateFromCenter(-1.0 * origin_heading);
      std::vector<Vec2d> vertices_ccw = original_box.GetAllCorners();
      std::vector<Vec2d> vertices_cw;
      while (!vertices_ccw.empty()) {
        auto current_corner_pt = vertices_ccw.back();
        current_corner_pt.SelfRotate(-1.0 * origin_heading);
        vertices_cw.push_back(current_corner_pt);
        vertices_ccw.pop_back();
      }
      // As the perception obstacle is a closed convex set, the first vertice is
      // repeated at the end of the vector to help transform all four edges to
      // inequality constraint
      vertices_cw.push_back(vertices_cw.front());
      obstacles_vertices_vec->push_back(vertices_cw);
    }

    // obstacle boundary box is used, thus the edges are set to be 4
    Eigen::MatrixXi perception_obstacles_edges_num =
        4 * Eigen::MatrixXi::Ones(perception_obstacles_num, 1);
    Eigen::MatrixXi parking_boundaries_obstacles_edges_num(
        parking_boundaries_num, 1);
    for (size_t i = 0; i < parking_boundaries_num; i++) {
      CHECK_GT(roi_parking_boundary[i].size(), 1);
      parking_boundaries_obstacles_edges_num(i, 0) =
          static_cast<int>(roi_parking_boundary[i].size()) - 1;
    }
    obstacles_edges_num_vec->resize(
        parking_boundaries_obstacles_edges_num.rows() +
            perception_obstacles_edges_num.rows(),
        1);
    *(obstacles_edges_num_vec) << parking_boundaries_obstacles_edges_num,
        perception_obstacles_edges_num;
  } else {
    obstacles_edges_num_vec->resize(
        parking_boundaries_obstacles_edges_num.rows(), 1);
    *(obstacles_edges_num_vec) << parking_boundaries_obstacles_edges_num;
  }

  mutable_open_space_info->set_obstacles_num(parking_boundaries_num +
                                             perception_obstacles_num);
  return true;
}

bool OpenSpaceRoiDecider::FilterOutObstacle(const Frame &frame,
                                            const Obstacle &obstacle) {
  if (obstacle.IsVirtual()) {
    return true;
  }

  const auto &open_space_info = frame.open_space_info();
  const auto &origin_point = open_space_info.origin_point();
  const auto &origin_heading = open_space_info.origin_heading();
  const auto &obstacle_box = obstacle.PerceptionBoundingBox();
  auto obstacle_center_xy = obstacle_box.center();

  // xy_boundary in xmin, xmax, ymin, ymax.
  const auto &roi_xy_boundary = open_space_info.ROI_xy_boundary();
  obstacle_center_xy -= origin_point;
  obstacle_center_xy.SelfRotate(-origin_heading);
  if (obstacle_center_xy.x() < roi_xy_boundary[0] ||
      obstacle_center_xy.x() > roi_xy_boundary[1] ||
      obstacle_center_xy.y() < roi_xy_boundary[2] ||
      obstacle_center_xy.y() > roi_xy_boundary[3]) {
    return true;
  }

  // Translate the end pose back to world frame with endpose in x, y, phi, v
  const auto &end_pose = open_space_info.open_space_end_pose();
  Vec2d end_pose_x_y(end_pose[0], end_pose[1]);
  end_pose_x_y.SelfRotate(origin_heading);
  end_pose_x_y += origin_point;

  // Get vehicle state
  Vec2d vehicle_x_y(vehicle_state_.x(), vehicle_state_.y());

  // Use vehicle position and end position to filter out obstacle
  const double vehicle_center_to_obstacle =
      obstacle_box.DistanceTo(vehicle_x_y);
  const double end_pose_center_to_obstacle =
      obstacle_box.DistanceTo(end_pose_x_y);
  const double filtering_distance =
      config_.open_space_roi_decider_config()
          .perception_obstacle_filtering_distance();
  if (vehicle_center_to_obstacle > filtering_distance &&
      end_pose_center_to_obstacle > filtering_distance) {
    return true;
  }
  return false;
}

bool OpenSpaceRoiDecider::LoadObstacleInHyperPlanes(Frame *const frame) {
  *(frame->mutable_open_space_info()->mutable_obstacles_A()) =
      Eigen::MatrixXd::Zero(
          frame->open_space_info().obstacles_edges_num().sum(), 2);
  *(frame->mutable_open_space_info()->mutable_obstacles_b()) =
      Eigen::MatrixXd::Zero(
          frame->open_space_info().obstacles_edges_num().sum(), 1);
  // vertices using H-represetntation
  if (!GetHyperPlanes(
          frame->open_space_info().obstacles_num(),
          frame->open_space_info().obstacles_edges_num(),
          frame->open_space_info().obstacles_vertices_vec(),
          frame->mutable_open_space_info()->mutable_obstacles_A(),
          frame->mutable_open_space_info()->mutable_obstacles_b())) {
    AERROR << "Fail to present obstacle in hyperplane";
    return false;
  }
  return true;
}

bool OpenSpaceRoiDecider::GetHyperPlanes(
    const size_t &obstacles_num, const Eigen::MatrixXi &obstacles_edges_num,
    const std::vector<std::vector<Vec2d>> &obstacles_vertices_vec,
    Eigen::MatrixXd *A_all, Eigen::MatrixXd *b_all) {
  if (obstacles_num != obstacles_vertices_vec.size()) {
    AERROR << "obstacles_num != obstacles_vertices_vec.size()";
    return false;
  }

  A_all->resize(obstacles_edges_num.sum(), 2);
  b_all->resize(obstacles_edges_num.sum(), 1);

  int counter = 0;
  double kEpsilon = 1.0e-5;
  // start building H representation
  for (size_t i = 0; i < obstacles_num; ++i) {
    size_t current_vertice_num = obstacles_edges_num(i, 0);
    Eigen::MatrixXd A_i(current_vertice_num, 2);
    Eigen::MatrixXd b_i(current_vertice_num, 1);

    // take two subsequent vertices, and computer hyperplane
    for (size_t j = 0; j < current_vertice_num; ++j) {
      Vec2d v1 = obstacles_vertices_vec[i][j];
      Vec2d v2 = obstacles_vertices_vec[i][j + 1];

      Eigen::MatrixXd A_tmp(2, 1), b_tmp(1, 1), ab(2, 1);
      // find hyperplane passing through v1 and v2
      if (std::abs(v1.x() - v2.x()) < kEpsilon) {
        if (v2.y() < v1.y()) {
          A_tmp << 1, 0;
          b_tmp << v1.x();
        } else {
          A_tmp << -1, 0;
          b_tmp << -v1.x();
        }
      } else if (std::abs(v1.y() - v2.y()) < kEpsilon) {
        if (v1.x() < v2.x()) {
          A_tmp << 0, 1;
          b_tmp << v1.y();
        } else {
          A_tmp << 0, -1;
          b_tmp << -v1.y();
        }
      } else {
        Eigen::MatrixXd tmp1(2, 2);
        tmp1 << v1.x(), 1, v2.x(), 1;
        Eigen::MatrixXd tmp2(2, 1);
        tmp2 << v1.y(), v2.y();
        ab = tmp1.inverse() * tmp2;
        double a = ab(0, 0);
        double b = ab(1, 0);

        if (v1.x() < v2.x()) {
          A_tmp << -a, 1;
          b_tmp << b;
        } else {
          A_tmp << a, -1;
          b_tmp << -b;
        }
      }

      // store vertices
      A_i.block(j, 0, 1, 2) = A_tmp.transpose();
      b_i.block(j, 0, 1, 1) = b_tmp;
    }

    A_all->block(counter, 0, A_i.rows(), 2) = A_i;
    b_all->block(counter, 0, b_i.rows(), 1) = b_i;
    counter += static_cast<int>(current_vertice_num);
  }
  return true;
}

}  // namespace planning
}  // namespace apollo
