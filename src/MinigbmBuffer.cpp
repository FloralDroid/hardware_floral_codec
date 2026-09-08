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
#include <cros_gralloc/cros_gralloc_handle.h>
#include <cutils/native_handle.h>
#include <drm_fourcc.h>
#include <drv.h>
#include <hardware/gralloc.h>
#include <va/va_drmcommon.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

namespace floral::codec {
namespace {

constexpr char kVaapiBufferName[] = "FloralVaapiDecoder";
constexpr uint32_t kCrosGrallocMagic = 0xABCDDCBA;
// cros_gralloc_handle is packed and places the variable native-handle data
// immediately after its fixed fields. Avoid cros_gralloc_helpers.h here: its
// offsetof() expression triggers -Winvalid-offsetof in the Codec2 -Werror build.
constexpr uint32_t kHandleDataSize =
    (sizeof(cros_gralloc_handle) - sizeof(native_handle_t)) / sizeof(int);

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

native_handle_t *CreateMinigbmHandle(
    const VADRMPRIMESurfaceDescriptor &descriptor, uint32_t width,
    uint32_t height, uint64_t usage, uint32_t *pixelStride) {
  if (width == 0 || height == 0 || descriptor.width != width ||
      descriptor.height != height || descriptor.fourcc != VA_FOURCC_NV12 ||
      descriptor.num_objects == 0 || descriptor.num_objects > DRV_MAX_PLANES) {
    return nullptr;
  }
  struct Plane {
    uint32_t object;
    uint32_t offset;
    uint32_t pitch;
  } planes[2]{};
  if (descriptor.num_layers == 1 &&
      descriptor.layers[0].drm_format == DRM_FORMAT_NV12 &&
      descriptor.layers[0].num_planes == 2) {
    for (uint32_t plane = 0; plane < 2; ++plane) {
      planes[plane] = {descriptor.layers[0].object_index[plane],
                       descriptor.layers[0].offset[plane],
                       descriptor.layers[0].pitch[plane]};
    }
  } else if (descriptor.num_layers == 2) {
    uint32_t yLayer = DRV_MAX_PLANES;
    uint32_t uvLayer = DRV_MAX_PLANES;
    for (uint32_t layer = 0; layer < descriptor.num_layers; ++layer) {
      if (descriptor.layers[layer].num_planes != 1) {
        return nullptr;
      }
      if (descriptor.layers[layer].drm_format == DRM_FORMAT_R8) {
        yLayer = layer;
      } else if (descriptor.layers[layer].drm_format == DRM_FORMAT_GR88) {
        uvLayer = layer;
      }
    }
    if (yLayer == DRV_MAX_PLANES || uvLayer == DRV_MAX_PLANES) {
      return nullptr;
    }
    planes[0] = {descriptor.layers[yLayer].object_index[0],
                 descriptor.layers[yLayer].offset[0],
                 descriptor.layers[yLayer].pitch[0]};
    planes[1] = {descriptor.layers[uvLayer].object_index[0],
                 descriptor.layers[uvLayer].offset[0],
                 descriptor.layers[uvLayer].pitch[0]};
  } else {
    return nullptr;
  }
  const uint64_t modifier = descriptor.objects[0].drm_format_modifier;
  if (modifier == DRM_FORMAT_MOD_INVALID) {
    return nullptr;
  }
  if (descriptor.objects[0].fd < 0 || descriptor.objects[0].size == 0 ||
      !FitsUint32(descriptor.objects[0].size)) {
    return nullptr;
  }
  for (uint32_t object = 0; object < descriptor.num_objects; ++object) {
    if (descriptor.objects[object].fd < 0 ||
        descriptor.objects[object].drm_format_modifier != modifier ||
        !FitsUint32(descriptor.objects[object].size)) {
      return nullptr;
    }
  }
  for (uint32_t plane = 0; plane < 2; ++plane) {
    if (planes[plane].object >= descriptor.num_objects ||
        planes[plane].pitch == 0) {
      return nullptr;
    }
  }

  constexpr uint32_t planeCount = 2;

  const size_t bytes =
      (sizeof(cros_gralloc_handle) + sizeof(kVaapiBufferName) + sizeof(int) - 1) &
      ~(sizeof(int) - 1);
  const size_t numInts =
      (bytes - sizeof(native_handle_t)) / sizeof(int) - planeCount;
  if (numInts > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return nullptr;
  }
  native_handle_t *handle = native_handle_create(planeCount, numInts);
  if (handle == nullptr) {
    return nullptr;
  }
  auto *cros = reinterpret_cast<cros_gralloc_handle *>(handle);
  std::memset(reinterpret_cast<char *>(cros) + sizeof(native_handle_t), 0,
              bytes - sizeof(native_handle_t));
  for (size_t index = 0; index < DRV_MAX_FDS; ++index) {
    cros->fds[index] = -1;
  }

  uint64_t totalSize = 0;
  for (uint32_t object = 0; object < descriptor.num_objects; ++object) {
    totalSize += descriptor.objects[object].size;
  }
  for (uint32_t plane = 0; plane < planeCount; ++plane) {
    const uint32_t object = planes[plane].object;
    if (object >= descriptor.num_objects ||
        !FitsUint32(planes[plane].offset)) {
      native_handle_close(handle);
      native_handle_delete(handle);
      return nullptr;
    }
    const int fd = fcntl(descriptor.objects[object].fd, F_DUPFD_CLOEXEC, 0);
    if (fd < 0) {
      native_handle_close(handle);
      native_handle_delete(handle);
      return nullptr;
    }
    cros->fds[plane] = fd;
    cros->strides[plane] = planes[plane].pitch;
    cros->offsets[plane] = planes[plane].offset;
    if (planes[plane].offset >= descriptor.objects[object].size) {
      native_handle_close(handle);
      native_handle_delete(handle);
      return nullptr;
    }
    uint64_t planeSize = descriptor.objects[object].size - planes[plane].offset;
    for (uint32_t next = 0; next < planeCount; ++next) {
      if (planes[next].object != object ||
          planes[next].offset <= planes[plane].offset) {
        continue;
      }
      planeSize = std::min<uint64_t>(
          planeSize, planes[next].offset - planes[plane].offset);
    }
    if (!FitsUint32(planeSize) || planeSize == 0) {
      native_handle_close(handle);
      native_handle_delete(handle);
      return nullptr;
    }
    cros->sizes[plane] = static_cast<uint32_t>(planeSize);
  }

  static std::atomic<uint32_t> nextExternalId{0x80000000u};
  cros->id = nextExternalId.fetch_add(1, std::memory_order_relaxed);
  cros->width = width;
  cros->height = height;
  cros->format = DRM_FORMAT_NV12;
  cros->tiling = modifier == DRM_FORMAT_MOD_LINEAR ? 0 : 1;
  cros->format_modifier = modifier;
  cros->use_flags = BO_USE_HW_VIDEO_DECODER | BO_USE_SCANOUT | BO_USE_TEXTURE;
  cros->magic = kCrosGrallocMagic;
  cros->pixel_stride = cros->strides[0];
  cros->droid_format = HAL_PIXEL_FORMAT_YCBCR_420_888;
  cros->usage = static_cast<int32_t>(usage);
  cros->num_planes = planeCount;
  cros->total_size = totalSize;
  cros->name_offset = kHandleDataSize;
  std::memcpy(&cros->data[cros->name_offset], kVaapiBufferName,
              sizeof(kVaapiBufferName));
  if (pixelStride != nullptr) {
    *pixelStride = cros->pixel_stride;
  }
  return handle;
}

void CloseVaDescriptorFds(VADRMPRIMESurfaceDescriptor *descriptor) {
  if (descriptor == nullptr) {
    return;
  }
  for (uint32_t object = 0; object < descriptor->num_objects; ++object) {
    if (descriptor->objects[object].fd >= 0) {
      close(descriptor->objects[object].fd);
      descriptor->objects[object].fd = -1;
    }
  }
}

}  // namespace floral::codec
