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

#include <gtest/gtest.h>

namespace floral::codec {
namespace {

TEST(H264LevelTest, SelectsLevelForVisiblePortraitFrame) {
  EXPECT_EQ(H264Level::k3_2,
            FindMinimumH264Level(720, 1280, 60, 8'000'000,
                                 H264Profile::kHigh));
}

TEST(H264LevelTest, IncludesCodedStridePadding) {
  EXPECT_EQ(H264Level::k4,
            FindMinimumH264Level(768, 1280, 60, 8'000'000,
                                 H264Profile::kHigh));
}

TEST(H264LevelTest, SelectsLevelForFullHdAtSixtyFramesPerSecond) {
  EXPECT_EQ(H264Level::k4_2,
            FindMinimumH264Level(1920, 1080, 60, 20'000'000,
                                 H264Profile::kHigh));
}

TEST(H264LevelTest, AppliesHighProfileBitrateScaling) {
  EXPECT_EQ(H264Level::k3,
            FindMinimumH264Level(640, 480, 30, 12'500'000,
                                 H264Profile::kHigh));
  EXPECT_EQ(H264Level::k3_1,
            FindMinimumH264Level(640, 480, 30, 12'500'001,
                                 H264Profile::kHigh));
}

TEST(H264LevelTest, RejectsConfigurationsAboveLevelFivePointTwo) {
  EXPECT_FALSE(FindMinimumH264Level(4096, 4096, 60, 200'000'000,
                                    H264Profile::kHigh)
                   .has_value());
}

TEST(H264LevelTest, RejectsInvalidConfiguration) {
  EXPECT_FALSE(FindMinimumH264Level(0, 1280, 60, 8'000'000,
                                    H264Profile::kHigh)
                   .has_value());
}

} // namespace
} // namespace floral::codec
