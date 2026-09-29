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

#include "floral/codec/CodecSpec.h"
#include "floral/codec/H264Level.h"

#include <C2Buffer.h>
#include <C2Component.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace floral::codec {

struct V4l2EncoderSettings {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t bitrate = 0;
  uint32_t frame_rate = 0;
  uint32_t gop_size = 0;
  bool constant_bitrate = false;
  H264Profile h264_profile = H264Profile::kHigh;
};

struct V4l2EncodedFrame {
  std::vector<uint8_t> data;
  uint64_t frame_index = 0;
  bool key_frame = false;
  bool end_of_stream = false;
};

struct V4l2DecodedFrame {
  std::shared_ptr<C2GraphicBlock> block;
  uint64_t frame_index = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  bool end_of_stream = false;
};

class V4l2CodecSession {
public:
  V4l2CodecSession(CodecSpec spec, std::string devicePath);
  ~V4l2CodecSession();

  V4l2CodecSession(const V4l2CodecSession &) = delete;
  V4l2CodecSession &operator=(const V4l2CodecSession &) = delete;

  c2_status_t Open(const V4l2EncoderSettings *encoderSettings);
  void Close();
  c2_status_t Flush();

  c2_status_t QueueEncoderFrame(
      const std::shared_ptr<C2BlockPool> &conversionPool,
      const std::shared_ptr<C2Buffer> &buffer, uint64_t frameIndex,
      int64_t timestampUs, bool requestSync, uint32_t bitrate);
  c2_status_t DequeueEncoderFrame(V4l2EncodedFrame *output, bool wait);
  c2_status_t StartEncoderDrain();

  c2_status_t QueueDecoderPacket(const uint8_t *data, size_t size,
                                 uint64_t frameIndex);
  c2_status_t WaitForDecoderProgress(int timeoutMs);
  c2_status_t DequeueDecoderFrame(const std::shared_ptr<C2BlockPool> &pool,
                                  V4l2DecodedFrame *output, bool wait);
  c2_status_t StartDecoderDrain();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace floral::codec
