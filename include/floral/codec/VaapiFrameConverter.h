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

#include <C2Buffer.h>

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
}

#include <cstdint>
#include <memory>

namespace floral::codec {

// Converts importable Android GraphicBuffers into encoder-ready VAAPI frames.
// RGB to NV12 conversion stays on the VAAPI VideoProc pipeline.
class VaapiFrameConverter final {
public:
  enum class Result {
    kConverted,
    kUnsupported,
    kError,
  };

  VaapiFrameConverter();
  ~VaapiFrameConverter();

  VaapiFrameConverter(const VaapiFrameConverter &) = delete;
  VaapiFrameConverter &operator=(const VaapiFrameConverter &) = delete;

  bool Initialize(AVBufferRef *deviceContext, uint32_t width, uint32_t height);
  Result Convert(const C2ConstGraphicBlock &block, AVFrame *destination);
  void Reset();

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace floral::codec
