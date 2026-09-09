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

#include "floral/codec/MinigbmBuffer.h"

#include <cros_gralloc/cros_gralloc_handle.h>
#include <drm_fourcc.h>
#include <drv.h>
#include <va/va_drmcommon.h>

#include <algorithm>
#include <limits>
#include <sys/stat.h>

namespace floral::codec {
namespace {

constexpr uint32_t kCrosGrallocMagic = 0xABCDDCBA;

uint32_t ToVaFourcc(uint32_t drmFormat) {
  switch (drmFormat) {
    case DRM_FORMAT_ABGR8888:
      return VA_FOURCC_RGBA;
    case DRM_FORMAT_XBGR8888:
      return VA_FOURCC_RGBX;
    case DRM_FORMAT_ARGB8888:
      return VA_FOURCC_BGRA;
    case DRM_FORMAT_XRGB8888:
      return VA_FOURCC_BGRX;
    case DRM_FORMAT_NV12:
      return VA_FOURCC_NV12;
    case DRM_FORMAT_NV21:
      return VA_FOURCC_NV21;
    case DRM_FORMAT_P010:
      return VA_FOURCC_P010;
    default:
      return 0;
  }
}

bool IsValidMinigbmHandle(const native_handle_t *handle,
                          const cros_gralloc_handle **out) {
  if (handle == nullptr || handle->version != sizeof(native_handle_t) ||
      handle->numFds <= 0 || handle->numFds > DRV_MAX_FDS ||
      handle->numInts < 0) {
    return false;
  }
  const auto *cros = reinterpret_cast<const cros_gralloc_handle *>(handle);
  if (cros->magic != kCrosGrallocMagic || cros->id == 0 ||
      cros->num_planes == 0 ||
      cros->num_planes > DRV_MAX_PLANES ||
      cros->num_planes > static_cast<uint32_t>(handle->numFds)) {
    return false;
  }
  for (uint32_t plane = 0; plane < cros->num_planes; ++plane) {
    if (cros->fds[plane] < 0 || cros->strides[plane] == 0) {
      return false;
    }
  }
  *out = cros;
  return true;
}

bool FitsUint32(uint64_t value) {
  return value <= std::numeric_limits<uint32_t>::max();
}

}  // namespace

bool GetMinigbmVaDescriptor(const native_handle_t *handle, uint32_t width,
                            uint32_t height,
                            VADRMPRIMESurfaceDescriptor *descriptor,
                            uint32_t *vaFourcc, uint64_t *bufferId) {
  const cros_gralloc_handle *cros = nullptr;
  if (descriptor == nullptr || vaFourcc == nullptr || bufferId == nullptr ||
      !IsValidMinigbmHandle(handle, &cros) || cros->width != width ||
      cros->height != height || cros->format_modifier == DRM_FORMAT_MOD_INVALID) {
    return false;
  }
  const uint32_t fourcc = ToVaFourcc(cros->format);
  if (fourcc == 0) {
    return false;
  }

  *descriptor = {};
  descriptor->fourcc = fourcc;
  descriptor->width = width;
  descriptor->height = height;
  descriptor->num_layers = 1;
  descriptor->layers[0].drm_format = cros->format;
  descriptor->layers[0].num_planes = cros->num_planes;
  struct stat objectStats[DRV_MAX_PLANES]{};
  for (uint32_t plane = 0; plane < cros->num_planes; ++plane) {
    struct stat planeStats{};
    if (cros->sizes[plane] == 0 || fstat(cros->fds[plane], &planeStats) != 0) {
      return false;
    }
    uint32_t object = 0;
    for (; object < descriptor->num_objects; ++object) {
      if (planeStats.st_dev == objectStats[object].st_dev &&
          planeStats.st_ino == objectStats[object].st_ino) {
        break;
      }
    }
    if (object == descriptor->num_objects) {
      if (object >= DRV_MAX_PLANES) {
        return false;
      }
      descriptor->objects[object].fd = cros->fds[plane];
      descriptor->objects[object].size = cros->sizes[plane];
      descriptor->objects[object].drm_format_modifier = cros->format_modifier;
      objectStats[object] = planeStats;
      ++descriptor->num_objects;
    }
    const uint64_t planeEnd = static_cast<uint64_t>(cros->offsets[plane]) +
                              cros->sizes[plane];
    const uint64_t dmaBufSize = objectStats[object].st_size > 0
                                    ? static_cast<uint64_t>(objectStats[object].st_size)
                                    : 0;
    const uint64_t objectSize = std::max<uint64_t>(
        descriptor->objects[object].size, std::max(planeEnd, dmaBufSize));
    if (!FitsUint32(objectSize)) {
      return false;
    }
    descriptor->objects[object].size = static_cast<uint32_t>(objectSize);
    descriptor->layers[0].object_index[plane] = object;
    descriptor->layers[0].offset[plane] = cros->offsets[plane];
    descriptor->layers[0].pitch[plane] = cros->strides[plane];
  }
  *vaFourcc = fourcc;
  *bufferId = cros->id;
  return true;
}

}  // namespace floral::codec
