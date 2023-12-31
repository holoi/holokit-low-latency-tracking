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
#ifndef CARDBOARD_SDK_HEAD_TRACKER_H_
#define CARDBOARD_SDK_HEAD_TRACKER_H_

#include <array>
#include <memory>
#include <mutex>  // NOLINT

#include "include/cardboard.h"
#include "sensors/accelerometer_data.h"
#include "sensors/gyroscope_data.h"
#include "sensors/sensor_event_producer.h"
#include "sensors/sensor_fusion_ekf.h"
#include "util/rotation.h"

// Aryzon 6DoF
#include "sixdof/rotation_data.h"
#include "sixdof/position_data.h"

namespace cardboard {

// HeadTracker encapsulates pose tracking by connecting sensors
// to SensorFusion.
// This pose tracker reports poses in display space.
class HeadTracker {
 public:
  HeadTracker();
  virtual ~HeadTracker();

  // Pauses tracking and sensors.
  void Pause();

  // Resumes tracking and sensors.
  void Resume();

  // Gets the predicted pose for a given timestamp.
  void GetPose(int64_t timestamp_ns,
               CardboardViewportOrientation viewport_orientation,
               std::array<float, 3>& out_position,
               std::array<float, 4>& out_orientation);

  // Recenters the head tracker.
  void Recenter();

  // Function to be called when receiving SixDoFData.
  //
  // Aryzon 6DoF
  // @param event sensor event.
  void AddSixDoFData(int64_t timestamp_ns, float* position, float* orientation);
    
 private:
  // Function called when receiving AccelerometerData.
  //
  // @param event sensor event.
  void OnAccelerometerData(const AccelerometerData& event);

  // Function called when receiving GyroscopeData.
  //
  // @param event sensor event.
  void OnGyroscopeData(const GyroscopeData& event);

  // Registers this as a listener for data from the accel and gyro sensors. This
  // is useful for informing the sensors that they may need to start polling for
  // data.
  void RegisterCallbacks();
  // Unregisters this as a listener for data from the accel and gyro sensors.
  // This is useful for informing the sensors that they may be able to stop
  // polling for data.
  void UnregisterCallbacks();

  // Gets the predicted rotation for a given timestamp and viewport orientation.
  Rotation GetRotation(CardboardViewportOrientation viewport_orientation,
                       int64_t timestamp_ns) const;

  std::atomic<bool> is_tracking_;
  // Sensor Fusion object that stores the internal state of the filter.
  std::unique_ptr<SensorFusionEkf> sensor_fusion_;
  // Latest gyroscope data.
  GyroscopeData latest_gyroscope_data_;

  // Event providers supplying AccelerometerData and GyroscopeData to the
  // detector.
  std::shared_ptr<SensorEventProducer<AccelerometerData>> accel_sensor_;
  std::shared_ptr<SensorEventProducer<GyroscopeData>> gyro_sensor_;

  // Callback functions registered to the input SingleTypeEventProducer.
  std::function<void(AccelerometerData)> on_accel_callback_;
  std::function<void(GyroscopeData)> on_gyro_callback_;

  // @{ Hold rotations to adapt the pose estimation to the viewport and head
  // poses. Use the following indexing for each viewport orientation:
  // [0]: Landscape left.
  // [1]: Landscape right.
  // [2]: Portrait.
  // [3]: Portrait upside down.
  static const std::array<Rotation, 4> kSensorToDisplayRotations;
  static const std::array<Rotation, 4> kEkfToHeadTrackerRotations;
  // @}

  // Contains the necessary rotations to account for changes in reported head
  // pose when the tracker starts/resets in a certain viewport and then changes
  // to another.
  //
  // The rows contain the current viewport orientation, the columns contain the
  // transformed viewport orientation. See below:
  //
  // @code
  // kViewportChangeRotationCompensation[current_viewport_orientation]
  //                                    [new_viewport_orientation]
  // @endcode
  //
  // Roll angle needs to change. The following table shows the correction angle
  // for each combination:
  //
  // | Current\New     | LL  | LR  |  P  | PUD |
  // |-----------------|-----|-----|-----|-----|
  // | Landscape Left  | 0   | π   |-π/2 | π/2 |
  // | Landscape Right | π   | 0   | π/2 |-π/2 |
  // | Portrait        | π/2 |-π/2 | 0   | π   |
  // | Portrait UD     |-π/2 | π/2 | π   | 0   |
  static const std::array<std::array<Rotation, 4>, 4>
      kViewportChangeRotationCompensation;

  // Orientation of the viewport. It is initialized in the first call of
  // GetPose().
  CardboardViewportOrientation viewport_orientation_;

  // Tells wheter the attribute viewport_orientation_ has been initialized or
  // not.
  bool is_viewport_orientation_initialized_;
    
  // Aryzon 6DoF
  RotationData *rotation_data_;
  PositionData *position_data_;
    
  Rotation ekf_to_sixDoF_;
  Rotation smooth_ekf_to_sixDoF_;
  Rotation ekf_to_head_tracker_;
  
  float steady_frames_;
  Rotation steady_start_;
};

}  // namespace cardboard

#endif  // CARDBOARD_SDK_HEAD_TRACKER_H_
