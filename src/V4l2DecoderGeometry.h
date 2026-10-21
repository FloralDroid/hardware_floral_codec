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

#include <cerrno>
#include <cstdint>
#include <linux/videodev2.h>

namespace floral::codec {

struct V4l2DecoderCaptureGeometry {
  v4l2_format format{};
  v4l2_rect visible{};
};

inline bool DecoderVisibleRectFits(const v4l2_rect &rect, uint32_t width,
                                   uint32_t height) {
  return rect.left >= 0 && rect.top >= 0 && rect.width > 0 && rect.height > 0 &&
         static_cast<uint32_t>(rect.left) <= width &&
         static_cast<uint32_t>(rect.top) <= height &&
         rect.width <= width - static_cast<uint32_t>(rect.left) &&
         rect.height <= height - static_cast<uint32_t>(rect.top);
}

// ioctlCall takes (request, argument) and preserves ioctl's errno convention.
// Keep the source-change crop separate from the padded memory geometry: Venus
// S_FMT resets its crop to the supplied dimensions, even for the same format.
template <typename IoctlCall>
int NegotiateV4l2DecoderCapture(IoctlCall ioctlCall,
                              V4l2DecoderCaptureGeometry *output) {
  if (output == nullptr) {
    errno = EINVAL;
    return -1;
  }
  V4l2DecoderCaptureGeometry geometry;
  geometry.format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  if (ioctlCall(VIDIOC_G_FMT, &geometry.format) < 0) {
    return -1;
  }
  const uint32_t width = geometry.format.fmt.pix_mp.width;
  const uint32_t height = geometry.format.fmt.pix_mp.height;
  bool hasVisibleRect = false;
  const uint32_t targets[] = {V4L2_SEL_TGT_COMPOSE,
                              V4L2_SEL_TGT_COMPOSE_DEFAULT};
  for (uint32_t target : targets) {
    v4l2_selection selection{};
    selection.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    selection.target = target;
    if (ioctlCall(VIDIOC_G_SELECTION, &selection) == 0) {
      geometry.visible = selection.r;
      hasVisibleRect = true;
      break;
    }
    if (errno != EINVAL && errno != ENOTTY) {
      return -1;
    }
  }

  // SOURCE_CHANGE already provides the required NV12 format. Re-applying it
  // would overwrite the driver's stream-derived visible size with padding.
  if (geometry.format.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV12 ||
      geometry.format.fmt.pix_mp.num_planes != 1) {
    geometry.format = {};
    geometry.format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    geometry.format.fmt.pix_mp.width = width;
    geometry.format.fmt.pix_mp.height = height;
    geometry.format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    geometry.format.fmt.pix_mp.field = V4L2_FIELD_NONE;
    geometry.format.fmt.pix_mp.num_planes = 1;
    if (ioctlCall(VIDIOC_S_FMT, &geometry.format) < 0) {
      return -1;
    }
  }
  const auto &format = geometry.format.fmt.pix_mp;
  if (format.pixelformat != V4L2_PIX_FMT_NV12 || format.num_planes != 1) {
    errno = ENOTSUP;
    return -1;
  }
  if (!hasVisibleRect) {
    geometry.visible = {0, 0, format.width, format.height};
  }
  if (format.plane_fmt[0].bytesperline < format.width ||
      format.plane_fmt[0].sizeimage == 0 ||
      !DecoderVisibleRectFits(geometry.visible, format.width, format.height)) {
    errno = EPROTO;
    return -1;
  }
  *output = geometry;
  return 0;
}

} // namespace floral::codec
