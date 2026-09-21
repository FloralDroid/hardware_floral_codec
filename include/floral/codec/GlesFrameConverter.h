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

#include "floral/codec/MinigbmDmaBuf.h"

#include <C2Buffer.h>
#include <C2Component.h>

#include <memory>

namespace floral::codec {

// Converts an RGB Codec2 GraphicBlock into a linear NV12 DMA-BUF without
// mapping either buffer on the CPU.
class GlesFrameConverter final {
public:
  GlesFrameConverter();
  ~GlesFrameConverter();

  GlesFrameConverter(const GlesFrameConverter &) = delete;
  GlesFrameConverter &operator=(const GlesFrameConverter &) = delete;

  c2_status_t Initialize();
  c2_status_t Convert(const C2ConstGraphicBlock &source,
                      const MinigbmDmaBuf &destination);
  void Reset();

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace floral::codec
