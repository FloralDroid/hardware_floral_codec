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

#include "floral/codec/MinigbmDmaBuf.h"

#include <cros_gralloc/cros_gralloc_handle.h>

namespace floral::codec {
namespace {

constexpr uint32_t kCrosGrallocMagic = 0xABCDDCBA;

bool IsValidHandle(const native_handle_t *handle,
                   const cros_gralloc_handle **output) {
  if (handle == nullptr || output == nullptr ||
      handle->version != sizeof(native_handle_t) || handle->numFds <= 0 ||
      handle->numFds > DRV_MAX_FDS || handle->numInts < 0) {
    return false;
  }
  const auto *cros = reinterpret_cast<const cros_gralloc_handle *>(handle);
  if (cros->magic != kCrosGrallocMagic || cros->id == 0 ||
      cros->num_planes == 0 || cros->num_planes > kMaxDmaBufPlanes ||
      cros->num_planes > static_cast<uint32_t>(handle->numFds)) {
    return false;
  }
  for (uint32_t plane = 0; plane < cros->num_planes; ++plane) {
    if (cros->fds[plane] < 0 || cros->strides[plane] == 0 ||
        cros->sizes[plane] == 0) {
      return false;
    }
  }
  *output = cros;
  return true;
}

} // namespace

bool GetMinigbmDmaBuf(const native_handle_t *handle, uint32_t width,
                      uint32_t height, MinigbmDmaBuf *output) {
  const cros_gralloc_handle *cros = nullptr;
  if (output == nullptr || !IsValidHandle(handle, &cros) ||
      cros->width != width || cros->height != height || cros->total_size == 0) {
    return false;
  }

  *output = {};
  output->width = cros->width;
  output->height = cros->height;
  output->drm_format = cros->format;
  output->modifier = cros->format_modifier;
  output->total_size = cros->total_size;
  output->buffer_id = cros->id;
  output->plane_count = cros->num_planes;
  for (uint32_t plane = 0; plane < cros->num_planes; ++plane) {
    output->planes[plane] = {cros->fds[plane], cros->strides[plane],
                             cros->offsets[plane], cros->sizes[plane]};
  }
  return true;
}

} // namespace floral::codec
