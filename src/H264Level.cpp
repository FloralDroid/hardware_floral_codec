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

#include "floral/codec/H264Level.h"

#include <algorithm>
#include <array>
#include <cstdint>

namespace floral::codec {
namespace {

struct LevelLimit {
  H264Level level;
  uint64_t macroblocks_per_second;
  uint32_t macroblocks_per_frame;
  uint32_t maximum_dimension_in_macroblocks;
  uint32_t baseline_bitrate_kbps;
};

// ITU-T H.264 level limits, in ascending order. The dimension constraint is
// the same constraint used by Android's ACodec::getAVCLevelFor().
constexpr std::array<LevelLimit, 17> kLevelLimits = {{
    {H264Level::k1, 1'485, 99, 28, 64},
    {H264Level::k1B, 1'485, 99, 28, 128},
    {H264Level::k1_1, 3'000, 396, 56, 192},
    {H264Level::k1_2, 6'000, 396, 56, 384},
    {H264Level::k1_3, 11'880, 396, 56, 768},
    {H264Level::k2, 11'880, 396, 56, 2'000},
    {H264Level::k2_1, 19'800, 792, 79, 4'000},
    {H264Level::k2_2, 20'250, 1'620, 113, 4'000},
    {H264Level::k3, 40'500, 1'620, 113, 10'000},
    {H264Level::k3_1, 108'000, 3'600, 169, 14'000},
    {H264Level::k3_2, 216'000, 5'120, 202, 20'000},
    {H264Level::k4, 245'760, 8'192, 256, 20'000},
    {H264Level::k4_1, 245'760, 8'192, 256, 50'000},
    {H264Level::k4_2, 522'240, 8'704, 263, 50'000},
    {H264Level::k5, 589'824, 22'080, 420, 135'000},
    {H264Level::k5_1, 983'040, 36'864, 543, 240'000},
    {H264Level::k5_2, 2'073'600, 36'864, 543, 240'000},
}};

uint32_t BitrateScale(H264Profile profile) {
  return profile == H264Profile::kHigh ? 1'250 : 1'000;
}

} // namespace

std::optional<H264Level>
FindMinimumH264Level(uint32_t codedWidth, uint32_t codedHeight,
                     uint32_t frameRate, uint32_t bitrate,
                     H264Profile profile) {
  if (codedWidth == 0 || codedHeight == 0 || frameRate == 0 || bitrate == 0) {
    return std::nullopt;
  }

  const uint64_t widthInMacroblocks = (codedWidth + 15u) / 16u;
  const uint64_t heightInMacroblocks = (codedHeight + 15u) / 16u;
  const uint64_t macroblocksPerFrame =
      widthInMacroblocks * heightInMacroblocks;
  const uint64_t macroblocksPerSecond = macroblocksPerFrame * frameRate;
  const uint64_t equivalentBitrateKbps =
      (static_cast<uint64_t>(bitrate) + BitrateScale(profile) - 1u) /
      BitrateScale(profile);
  const uint64_t maximumDimension =
      std::max(widthInMacroblocks, heightInMacroblocks);

  for (const LevelLimit &limit : kLevelLimits) {
    if (macroblocksPerSecond <= limit.macroblocks_per_second &&
        macroblocksPerFrame <= limit.macroblocks_per_frame &&
        maximumDimension <= limit.maximum_dimension_in_macroblocks &&
        equivalentBitrateKbps <= limit.baseline_bitrate_kbps) {
      return limit.level;
    }
  }
  return std::nullopt;
}

} // namespace floral::codec
