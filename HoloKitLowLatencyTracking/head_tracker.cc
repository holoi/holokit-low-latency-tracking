/*
 * Copyright 2019 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "head_tracker.h"

#include "include/cardboard.h"
#include "sensors/neck_model.h"
#include "util/logging.h"
#include "util/rotation.h"
#include "util/vector.h"
#include "util/vectorutils.h"

namespace cardboard {

// Aryzon 6DoF
constexpr int kRotationSamples = 10;
constexpr int kPositionSamples = 6;
constexpr int64_t kMaxSixDoFTimeDifference = 200000000; // Maximum time difference between last pose state timestamp and last 6DoF timestamp, if it takes longer than this the last known location of sixdof will be used
constexpr float kReduceBiasRate = 0.05;

const std::array<Rotation, 4> HeadTracker::kEkfToHeadTrackerRotations{
    // LandscapeLeft: This is the same than initializing the rotation from
    // Rotation::FromYawPitchRoll(-M_PI / 2., 0, -M_PI / 2.).
    Rotation::FromQuaternion(Rotation::QuaternionType(0.5, -0.5, -0.5, 0.5)),
    // LandscapeRight: This is the same than initializing the rotation from
    // Rotation::FromYawPitchRoll(M_PI / 2., 0, M_PI / 2.).
    Rotation::FromQuaternion(Rotation::QuaternionType(0.5, 0.5, 0.5, 0.5)),
    // Portrait: This is the same than initializing the rotation from
    // Rotation::FromYawPitchRoll(M_PI / 2., M_PI / 2., M_PI / 2.).
    Rotation::FromQuaternion(Rotation::QuaternionType(0.7071067811865476, 0.,
                                                      0., 0.7071067811865476)),
    // Portrait upside down: This is the same than initializing the rotation
    // from Rotation::FromYawPitchRoll(-M_PI / 2., -M_PI / 2., -M_PI / 2.).
    Rotation::FromQuaternion(Rotation::QuaternionType(
        0., -0.7071067811865476, -0.7071067811865476, 0.))};

const std::array<Rotation, 4> HeadTracker::kSensorToDisplayRotations{
    // LandscapeLeft: This is the same than initializing the rotation from
    // Rotation::FromAxisAndAngle(Vector3(0., 0., 1.), M_PI / 2.).
    Rotation::FromQuaternion(Rotation::QuaternionType(
        0., 0., 0.7071067811865476, 0.7071067811865476)),
    // LandscapeRight: This is the same than initializing the rotation from
    // Rotation::FromAxisAndAngle(Vector3(0., 0., 1.), -M_PI / 2.).
    Rotation::FromQuaternion(Rotation::QuaternionType(
        0., 0., -0.7071067811865476, 0.7071067811865476)),
    // Portrait: This is the same than initializing the rotation from
    // Rotation::FromAxisAndAngle(Vector3(0., 0., 1.), 0.).
    Rotation::FromQuaternion(Rotation::QuaternionType(0., 0., 0., 1.)),
    // PortaitUpsideDown: This is the same than initializing the rotation from
    // Rotation::FromAxisAndAngle(Vector3(0., 0., 1.), M_PI).
    Rotation::FromQuaternion(Rotation::QuaternionType(0., 0., 1., 0.))};

const std::array<std::array<Rotation, 4>, 4>
    HeadTracker::kViewportChangeRotationCompensation{{
        // Landscape left.
        {Rotation::Identity(), Rotation::FromYawPitchRoll(0, 0, M_PI),
         Rotation::FromYawPitchRoll(0, 0, -M_PI / 2),
         Rotation::FromYawPitchRoll(0, 0, M_PI / 2)},
        // Landscape Right.
        {Rotation::FromYawPitchRoll(0, 0, M_PI), Rotation::Identity(),
         Rotation::FromYawPitchRoll(0, 0, M_PI / 2),
         Rotation::FromYawPitchRoll(0, 0, -M_PI / 2)},
        // Portrait.
        {Rotation::FromYawPitchRoll(0, 0, M_PI / 2),
         Rotation::FromYawPitchRoll(0, 0, -M_PI / 2), Rotation::Identity(),
         Rotation::FromYawPitchRoll(0, 0, M_PI)},
        // Portrait Upside Down.
        {Rotation::FromYawPitchRoll(0, 0, -M_PI / 2),
         Rotation::FromYawPitchRoll(0, 0, M_PI / 2),
         Rotation::FromYawPitchRoll(0, 0, M_PI), Rotation::Identity()},
    }};

HeadTracker::HeadTracker()
    : is_tracking_(false),
      sensor_fusion_(new SensorFusionEkf()),
      latest_gyroscope_data_({0, 0, Vector3::Zero()}),
      accel_sensor_(new SensorEventProducer<AccelerometerData>()),
      gyro_sensor_(new SensorEventProducer<GyroscopeData>()),
      // Aryzon 6DoF
      rotation_data_(new RotationData(kRotationSamples)),
      position_data_(new PositionData(kPositionSamples)),
      is_viewport_orientation_initialized_(false) {
  on_accel_callback_ = [&](const AccelerometerData& event) {
    OnAccelerometerData(event);
  };
  on_gyro_callback_ = [&](const GyroscopeData& event) {
    OnGyroscopeData(event);
  };
  ekf_to_sixDoF_ = Rotation::Identity();
  smooth_ekf_to_sixDoF_ = Rotation::Identity();
  steady_start_ = Rotation::Identity();
  steady_frames_ = -1;
}

HeadTracker::~HeadTracker() { UnregisterCallbacks(); }

void HeadTracker::Pause() {
  if (!is_tracking_) {
    return;
  }

  UnregisterCallbacks();

  // Create a gyro event with zero velocity. This effectively stops the
  // prediction.
  GyroscopeData event = latest_gyroscope_data_;
  event.data = Vector3::Zero();

  OnGyroscopeData(event);

  is_tracking_ = false;
}

void HeadTracker::Resume() {
  is_tracking_ = true;
  RegisterCallbacks();
}

void HeadTracker::GetPose(int64_t timestamp_ns,
                          CardboardViewportOrientation viewport_orientation,
                          std::array<float, 3>& out_position,
                          std::array<float, 4>& out_orientation) {

  if (is_viewport_orientation_initialized_ &&
      viewport_orientation != viewport_orientation_) {
      sensor_fusion_->RotateSensorSpaceToStartSpaceTransformation(
                                                                  kViewportChangeRotationCompensation[viewport_orientation_]
                                                                  [viewport_orientation]);
  }
  viewport_orientation_ = viewport_orientation;
  is_viewport_orientation_initialized_ = true;
    
  const RotationState rotation_state = sensor_fusion_->GetLatestRotationState();
  const Rotation unpredicted_rotation = rotation_state.sensor_from_start_rotation;

  const Rotation adjusted_rotation = GetRotation(viewport_orientation, timestamp_ns);
  const Rotation adjusted_unpredicted_rotation = kSensorToDisplayRotations[viewport_orientation] * unpredicted_rotation * kEkfToHeadTrackerRotations[viewport_orientation];

  // Save rotation sample with timestamp to be used in AddSixDoFData()
  rotation_data_->AddSample(adjusted_unpredicted_rotation.GetQuaternion(), rotation_state.timestamp);
    
  if (position_data_->IsValid() && rotation_state.timestamp - position_data_->GetLatestTimestamp() < kMaxSixDoFTimeDifference) {
      
    // 6DoF is recently updated
    const Vector4 orientation = (adjusted_rotation * smooth_ekf_to_sixDoF_).GetQuaternion();
      
    out_orientation[0] = static_cast<float>(orientation[0]);
    out_orientation[1] = static_cast<float>(orientation[1]);
    out_orientation[2] = static_cast<float>(orientation[2]);
    out_orientation[3] = static_cast<float>(orientation[3]);
      
    Vector3 p = position_data_->GetExtrapolatedForTimeStamp(timestamp_ns);
    out_position = {(float)p[0], (float)p[1], (float)p[2]};
  } else {
    // 6DoF is not recently updated
    const Vector4 orientation = adjusted_rotation.GetQuaternion();
      
    out_orientation[0] = static_cast<float>(orientation[0]);
    out_orientation[1] = static_cast<float>(orientation[1]);
    out_orientation[2] = static_cast<float>(orientation[2]);
    out_orientation[3] = static_cast<float>(orientation[3]);
        
    out_position = ApplyNeckModel(out_orientation, 1.0);
    if (position_data_->IsValid()) {
        // Apply last known 6DoF position if 6DoF data was previously added, while still applying neckmodel.
        Vector3 last_known_position_ = position_data_->GetLatestData();
        out_position[0] += (float)last_known_position_[0];
        out_position[1] += (float)last_known_position_[1];
        out_position[2] += (float)last_known_position_[2];
    }
  }
}

Rotation ShortestRotation(Rotation a, Rotation b) {
    
    Vector4 aQ = a.GetQuaternion();
    Vector4 bQ = b.GetQuaternion();
    
    if (Dot(aQ, bQ) < 0) {
        return -a * Rotation::FromQuaternion(-bQ);
    } else {
        return -a * b;
    }
}

// Aryzon 6DoF
void HeadTracker::AddSixDoFData(int64_t timestamp_ns, float* pos, float* orientation) {
  if (!is_tracking_) {
    return;
  }
    if (position_data_->GetLatestTimestamp() != timestamp_ns) {
        position_data_->AddSample(Vector3(pos[0], pos[1], pos[2]), timestamp_ns);
    }
    
    // There will be a difference in rotation between ekf and sixDoF.
    // SixDoF sensor is the 'truth' but is slower then ekf
    // When the device is steady the difference between rotatations is saved
    // smooth_ekf_to_sixDoF is slowly adjusted to smoothly close the gap
    // between ekf and sixDoF. This value is used in GetPose().
    
    if (position_data_->IsValid() && rotation_data_->IsValid()) {
        if ((steady_frames_ == 30 || steady_frames_ < 0) && rotation_data_->GetLatestTimeStamp() > timestamp_ns) {
            // Match rotation timestamps of ekf to sixDoF by interpolating the saved ekf rotations
            // 6DoF timestamp should be before the latest rotation_data timestamp otherwise extrapolation
            // needs to happen which will be less accurate.
            const Rotation ekf_at_time_of_sixDoF = Rotation::FromQuaternion(rotation_data_->GetInterpolatedForTimeStamp(timestamp_ns));
            const Rotation six_DoF_rotation = Rotation::FromQuaternion(Vector4(orientation[0], orientation[1], orientation[2], orientation[3]));
            
            ekf_to_sixDoF_ = ShortestRotation(ekf_at_time_of_sixDoF, six_DoF_rotation);

        } else if (steady_frames_ == 0) {
             steady_start_ = Rotation::FromQuaternion(rotation_data_->GetLatestData());
        }
        
        const Rotation steady_difference = steady_start_ * -Rotation::FromQuaternion(rotation_data_->GetLatestData());
        
        if (steady_difference.GetQuaternion()[3] > 0.9995) {
            steady_frames_ += 1;
        } else {
            steady_frames_ = 0;
        }
        
        const Rotation bias_to_fill =  ShortestRotation(smooth_ekf_to_sixDoF_, ekf_to_sixDoF_);
        Vector3 axis;
        double angle;
        bias_to_fill.GetAxisAndAngle(&axis, &angle);
        
        const Rotation add_to_bias = Rotation::FromAxisAndAngle(axis, angle * kReduceBiasRate);
        
        smooth_ekf_to_sixDoF_ *= add_to_bias;
    }
}

void HeadTracker::Recenter() {
  sensor_fusion_->Reset();
}

void HeadTracker::RegisterCallbacks() {
  accel_sensor_->StartSensorPolling(&on_accel_callback_);
  gyro_sensor_->StartSensorPolling(&on_gyro_callback_);
}

void HeadTracker::UnregisterCallbacks() {
  accel_sensor_->StopSensorPolling();
  gyro_sensor_->StopSensorPolling();
}

void HeadTracker::OnAccelerometerData(const AccelerometerData& event) {
  if (!is_tracking_) {
    return;
  }
  sensor_fusion_->ProcessAccelerometerSample(event);
}

void HeadTracker::OnGyroscopeData(const GyroscopeData& event) {
  if (!is_tracking_) {
    return;
  }
  latest_gyroscope_data_ = event;
  sensor_fusion_->ProcessGyroscopeSample(event);
}

Rotation HeadTracker::GetRotation(
    CardboardViewportOrientation viewport_orientation,
    int64_t timestamp_ns) const {
  const Rotation predicted_rotation =
      sensor_fusion_->PredictRotation(timestamp_ns);

  // In order to update our pose as the sensor changes, we begin with the
  // inverse default orientation (the orientation returned by a reset sensor,
  // i.e. since the last Reset() call), apply the current sensor transformation,
  // and then transform into display space.
  return kSensorToDisplayRotations[viewport_orientation] * predicted_rotation *
         kEkfToHeadTrackerRotations[viewport_orientation];
}

}  // namespace cardboard
