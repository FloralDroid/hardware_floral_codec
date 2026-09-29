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

#include "V4l2EncoderMetadata.h"

#include <gtest/gtest.h>

namespace floral::codec {
namespace {

TEST(V4l2EncoderMetadataTest, PreservesActualPresentationTime) {
  timeval timestamp{};
  ASSERT_TRUE(MakeV4l2EncoderTimestamp(848'749'918, &timestamp));
  EXPECT_EQ(848, timestamp.tv_sec);
  EXPECT_EQ(749'918, timestamp.tv_usec);
  ASSERT_TRUE(MakeV4l2EncoderTimestamp(848'849'918, &timestamp));
  EXPECT_EQ(848, timestamp.tv_sec);
  EXPECT_EQ(849'918, timestamp.tv_usec);
}

TEST(V4l2EncoderMetadataTest, NormalizesSecondBoundaryAndZero) {
  timeval timestamp{};
  ASSERT_TRUE(MakeV4l2EncoderTimestamp(0, &timestamp));
  EXPECT_EQ(0, timestamp.tv_sec);
  EXPECT_EQ(0, timestamp.tv_usec);
  ASSERT_TRUE(MakeV4l2EncoderTimestamp(1'000'001, &timestamp));
  EXPECT_EQ(1, timestamp.tv_sec);
  EXPECT_EQ(1, timestamp.tv_usec);
}

TEST(V4l2EncoderMetadataTest, RejectsInvalidOrOverflowingTimestamp) {
  timeval timestamp{};
  EXPECT_FALSE(MakeV4l2EncoderTimestamp(-1, &timestamp));
  EXPECT_FALSE(MakeV4l2EncoderTimestamp(0, nullptr));
  const int64_t limit = std::numeric_limits<int64_t>::max() / 1'000;
  EXPECT_EQ(limit / 1'000'000 <= std::numeric_limits<time_t>::max(),
            MakeV4l2EncoderTimestamp(limit, &timestamp));
  EXPECT_FALSE(MakeV4l2EncoderTimestamp(limit + 1, &timestamp));
}

TEST(V4l2EncoderMetadataTest, AssociatesOutputWithWorkIndexNotTimestamp) {
  std::vector<V4l2EncoderFrameMetadata> frames = {
      {{848, 749'918}, 0}, {{848, 849'918}, 99}};
  EXPECT_EQ(99u, TakeV4l2EncoderFrameIndex(&frames, {848, 849'918}));
  EXPECT_EQ(0u, TakeV4l2EncoderFrameIndex(&frames, {848, 749'918}));
  EXPECT_TRUE(frames.empty());
}

TEST(V4l2EncoderMetadataTest, RepeatedPresentationTimesRemainFifo) {
  std::vector<V4l2EncoderFrameMetadata> frames = {
      {{0, 0}, 2}, {{0, 0}, 7}};
  EXPECT_EQ(2u, TakeV4l2EncoderFrameIndex(&frames, {0, 0}));
  EXPECT_EQ(7u, TakeV4l2EncoderFrameIndex(&frames, {0, 0}));
}

TEST(V4l2EncoderMetadataTest, UnknownTimestampDoesNotConsumeAnotherWork) {
  std::vector<V4l2EncoderFrameMetadata> frames = {{{1, 0}, 8}};
  EXPECT_FALSE(TakeV4l2EncoderFrameIndex(&frames, {0, 0}).has_value());
  ASSERT_EQ(1u, frames.size());
  EXPECT_EQ(8u, frames.front().frame_index);
}

TEST(V4l2EncoderMetadataTest, ReusesReservedStorage) {
  std::vector<V4l2EncoderFrameMetadata> frames;
  frames.reserve(8);
  const auto *storage = frames.data();
  for (uint64_t iteration = 0; iteration < 100; ++iteration) {
    for (uint64_t index = 0; index < 8; ++index) {
      frames.push_back({{0, static_cast<suseconds_t>(index)}, iteration * 8 + index});
    }
    for (uint64_t index = 0; index < 8; ++index) {
      EXPECT_EQ(iteration * 8 + index,
                TakeV4l2EncoderFrameIndex(&frames,
                                         {0, static_cast<suseconds_t>(index)}));
    }
    EXPECT_EQ(storage, frames.data());
  }
}

} // namespace
} // namespace floral::codec
