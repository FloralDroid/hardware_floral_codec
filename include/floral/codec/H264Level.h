/*
 * Copyright 2026 FloralDroid
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

#pragma once

#include <cstdint>
#include <optional>

namespace floral::codec {

enum class H264Profile {
  kBaseline,
  kConstrainedBaseline,
  kMain,
  kHigh,
};

// Values deliberately match enum v4l2_mpeg_video_h264_level.
enum class H264Level : uint32_t {
  k1 = 0,
  k1B = 1,
  k1_1 = 2,
  k1_2 = 3,
  k1_3 = 4,
  k2 = 5,
  k2_1 = 6,
  k2_2 = 7,
  k3 = 8,
  k3_1 = 9,
  k3_2 = 10,
  k4 = 11,
  k4_1 = 12,
  k4_2 = 13,
  k5 = 14,
  k5_1 = 15,
  k5_2 = 16,
};

std::optional<H264Level>
FindMinimumH264Level(uint32_t codedWidth, uint32_t codedHeight,
                     uint32_t frameRate, uint32_t bitrate,
                     H264Profile profile);

} // namespace floral::codec
