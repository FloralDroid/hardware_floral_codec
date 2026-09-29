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

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <sys/time.h>
#include <vector>

namespace floral::codec {

struct V4l2EncoderFrameMetadata {
  timeval timestamp;
  uint64_t frame_index;
};

inline bool MakeV4l2EncoderTimestamp(int64_t timestampUs, timeval *timestamp) {
  // VB2 converts timeval to signed nanoseconds before passing it to HFI.
  if (timestamp == nullptr || timestampUs < 0 ||
      timestampUs > std::numeric_limits<int64_t>::max() / 1'000 ||
      timestampUs / 1'000'000 > std::numeric_limits<time_t>::max()) {
    return false;
  }
  timestamp->tv_sec = static_cast<time_t>(timestampUs / 1'000'000);
  timestamp->tv_usec = static_cast<suseconds_t>(timestampUs % 1'000'000);
  return true;
}

inline std::optional<uint64_t> TakeV4l2EncoderFrameIndex(
    std::vector<V4l2EncoderFrameMetadata> *frames, const timeval &timestamp) {
  const auto found = std::find_if(
      frames->begin(), frames->end(), [&timestamp](const auto &frame) {
        return frame.timestamp.tv_sec == timestamp.tv_sec &&
               frame.timestamp.tv_usec == timestamp.tv_usec;
      });
  if (found == frames->end()) {
    return std::nullopt;
  }
  // Keep FIFO association for repeated PTS; B frames are disabled.
  const uint64_t frameIndex = found->frame_index;
  frames->erase(found);
  return frameIndex;
}

} // namespace floral::codec
