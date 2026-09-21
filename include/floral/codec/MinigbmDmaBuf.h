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

#include <cutils/native_handle.h>

namespace floral::codec {

constexpr uint32_t kMaxDmaBufPlanes = 4;

struct DmaBufPlane {
  int fd = -1;
  uint32_t stride = 0;
  uint32_t offset = 0;
  uint32_t size = 0;
};

struct MinigbmDmaBuf {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t drm_format = 0;
  uint64_t modifier = 0;
  uint64_t total_size = 0;
  uint64_t buffer_id = 0;
  uint32_t plane_count = 0;
  DmaBufPlane planes[kMaxDmaBufPlanes];
};

// File descriptors in the returned description remain owned by |handle|.
bool GetMinigbmDmaBuf(const native_handle_t *handle, uint32_t width,
                      uint32_t height, MinigbmDmaBuf *output);

} // namespace floral::codec
