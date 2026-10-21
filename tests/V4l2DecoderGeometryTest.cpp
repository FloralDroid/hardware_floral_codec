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

#include "V4l2DecoderGeometry.h"

#include <gtest/gtest.h>

#include <limits>
#include <vector>

namespace floral::codec {
namespace {

struct CaptureDriver {
  v4l2_format format{};
  v4l2_rect visible{0, 0, 320, 240};
  int get_format_error = 0;
  int compose_error = 0;
  int default_error = 0;
  int set_format_error = 0;
  bool refuse_nv12 = false;
  bool return_two_planes = false;
  std::vector<unsigned long> requests;
  std::vector<uint32_t> targets;

  CaptureDriver() {
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    format.fmt.pix_mp.width = 320;
    format.fmt.pix_mp.height = 256;
    format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    format.fmt.pix_mp.num_planes = 1;
    format.fmt.pix_mp.plane_fmt[0].bytesperline = 384;
    format.fmt.pix_mp.plane_fmt[0].sizeimage = 151552;
  }

  int Ioctl(unsigned long request, void *argument) {
    requests.push_back(request);
    int error = 0;
    if (request == VIDIOC_G_FMT) {
      EXPECT_EQ(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                static_cast<v4l2_format *>(argument)->type);
      error = get_format_error;
      if (!error) {
        *static_cast<v4l2_format *>(argument) = format;
      }
    } else if (request == VIDIOC_G_SELECTION) {
      auto *selection = static_cast<v4l2_selection *>(argument);
      EXPECT_EQ(V4L2_BUF_TYPE_VIDEO_CAPTURE, selection->type);
      targets.push_back(selection->target);
      error = selection->target == V4L2_SEL_TGT_COMPOSE ? compose_error
                                                     : default_error;
      if (!error) {
        selection->r = visible;
      }
    } else if (request == VIDIOC_S_FMT) {
      auto *requested = static_cast<v4l2_format *>(argument);
      EXPECT_EQ(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, requested->type);
      EXPECT_EQ(V4L2_PIX_FMT_NV12, requested->fmt.pix_mp.pixelformat);
      EXPECT_EQ(1, requested->fmt.pix_mp.num_planes);
      error = set_format_error;
      if (!error) {
        // Match Venus: S_FMT overwrites the stream-derived crop with the
        // supplied dimensions, including padding if G_FMT was fed back.
        visible = {0, 0, requested->fmt.pix_mp.width,
                   requested->fmt.pix_mp.height};
        if (!refuse_nv12) {
          format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
        }
        format.fmt.pix_mp.num_planes = return_two_planes ? 2 : 1;
        *requested = format;
      }
    } else {
      ADD_FAILURE() << "unexpected ioctl " << request;
      error = EINVAL;
    }
    errno = error;
    return error ? -1 : 0;
  }

  int Negotiate(V4l2DecoderCaptureGeometry *output) {
    return NegotiateV4l2DecoderCapture(
        [this](unsigned long request, void *argument) {
          return Ioctl(request, argument);
        },
        output);
  }
};

TEST(V4l2DecoderGeometryTest, PreservesSourceCropWithoutRedundantSetFormat) {
  CaptureDriver driver;
  V4l2DecoderCaptureGeometry output;
  ASSERT_EQ(0, driver.Negotiate(&output));
  EXPECT_EQ((std::vector<unsigned long>{VIDIOC_G_FMT, VIDIOC_G_SELECTION}),
            driver.requests);
  EXPECT_EQ(320u, output.format.fmt.pix_mp.width);
  EXPECT_EQ(256u, output.format.fmt.pix_mp.height);
  EXPECT_EQ(384u, output.format.fmt.pix_mp.plane_fmt[0].bytesperline);
  EXPECT_EQ(151552u, output.format.fmt.pix_mp.plane_fmt[0].sizeimage);
  EXPECT_EQ(320u, output.visible.width);
  EXPECT_EQ(240u, output.visible.height);
  EXPECT_EQ(240u, driver.visible.height);
}

TEST(V4l2DecoderGeometryTest, OldReadSetReadSequenceOverwritesVisibleHeight) {
  CaptureDriver driver;
  v4l2_format format{};
  format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  ASSERT_EQ(0, driver.Ioctl(VIDIOC_G_FMT, &format));
  ASSERT_EQ(0, driver.Ioctl(VIDIOC_S_FMT, &format));
  v4l2_selection selection{};
  selection.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  selection.target = V4L2_SEL_TGT_COMPOSE;
  ASSERT_EQ(0, driver.Ioctl(VIDIOC_G_SELECTION, &selection));
  EXPECT_EQ(256u, selection.r.height);
}

TEST(V4l2DecoderGeometryTest, SavesCropBeforeNecessaryFormatConversion) {
  CaptureDriver driver;
  driver.format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12M;
  driver.format.fmt.pix_mp.num_planes = 2;
  V4l2DecoderCaptureGeometry output;
  ASSERT_EQ(0, driver.Negotiate(&output));
  EXPECT_EQ((std::vector<unsigned long>{VIDIOC_G_FMT, VIDIOC_G_SELECTION,
                                        VIDIOC_S_FMT}),
            driver.requests);
  EXPECT_EQ(V4L2_PIX_FMT_NV12, output.format.fmt.pix_mp.pixelformat);
  EXPECT_EQ(240u, output.visible.height);
  EXPECT_EQ(256u, driver.visible.height);
}

TEST(V4l2DecoderGeometryTest, PreservesNonzeroCropOrigin) {
  CaptureDriver driver;
  driver.visible = {8, 4, 304, 240};
  V4l2DecoderCaptureGeometry output;
  ASSERT_EQ(0, driver.Negotiate(&output));
  EXPECT_EQ(8, output.visible.left);
  EXPECT_EQ(4, output.visible.top);
  EXPECT_EQ(304u, output.visible.width);
  EXPECT_EQ(240u, output.visible.height);
}

TEST(V4l2DecoderGeometryTest, UsesComposeDefaultOnlyIfComposeIsUnsupported) {
  for (int error : {EINVAL, ENOTTY}) {
    CaptureDriver driver;
    driver.compose_error = error;
    V4l2DecoderCaptureGeometry output;
    ASSERT_EQ(0, driver.Negotiate(&output));
    EXPECT_EQ((std::vector<uint32_t>{V4L2_SEL_TGT_COMPOSE,
                                     V4L2_SEL_TGT_COMPOSE_DEFAULT}),
              driver.targets);
    EXPECT_EQ(240u, output.visible.height);
  }
}

TEST(V4l2DecoderGeometryTest, FallsBackToCaptureOnlyWithoutSelectionSupport) {
  CaptureDriver driver;
  driver.compose_error = EINVAL;
  driver.default_error = ENOTTY;
  V4l2DecoderCaptureGeometry output;
  ASSERT_EQ(0, driver.Negotiate(&output));
  EXPECT_EQ(320u, output.visible.width);
  EXPECT_EQ(256u, output.visible.height);
}

TEST(V4l2DecoderGeometryTest, UsesNewStreamGeometryNotInitialConfiguredSize) {
  CaptureDriver driver;
  driver.format.fmt.pix_mp.width = 640;
  driver.format.fmt.pix_mp.height = 480;
  driver.format.fmt.pix_mp.plane_fmt[0].bytesperline = 640;
  driver.format.fmt.pix_mp.plane_fmt[0].sizeimage = 460800;
  driver.visible = {0, 0, 640, 360};
  V4l2DecoderCaptureGeometry output;
  ASSERT_EQ(0, driver.Negotiate(&output));
  EXPECT_EQ(640u, output.visible.width);
  EXPECT_EQ(360u, output.visible.height);
}

TEST(V4l2DecoderGeometryTest, DoesNotHideSelectionIoErrorsAsMissingCrop) {
  for (bool useDefault : {false, true}) {
    CaptureDriver driver;
    driver.compose_error = useDefault ? EINVAL : EIO;
    driver.default_error = EIO;
    V4l2DecoderCaptureGeometry output;
    EXPECT_EQ(-1, driver.Negotiate(&output));
    EXPECT_EQ(EIO, errno);
    EXPECT_EQ(0u, output.visible.width);
  }
}

TEST(V4l2DecoderGeometryTest, PropagatesGetFormatFailure) {
  CaptureDriver driver;
  driver.get_format_error = EIO;
  V4l2DecoderCaptureGeometry output;
  EXPECT_EQ(-1, driver.Negotiate(&output));
  EXPECT_EQ(EIO, errno);
  EXPECT_EQ(1u, driver.requests.size());
}

TEST(V4l2DecoderGeometryTest, PropagatesSetFormatFailure) {
  CaptureDriver driver;
  driver.format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12M;
  driver.set_format_error = EBUSY;
  V4l2DecoderCaptureGeometry output;
  EXPECT_EQ(-1, driver.Negotiate(&output));
  EXPECT_EQ(EBUSY, errno);
  EXPECT_EQ(0u, output.visible.width);
}

TEST(V4l2DecoderGeometryTest, RejectsUnsupportedFormatOrPlaneCount) {
  for (bool twoPlanes : {false, true}) {
    CaptureDriver driver;
    driver.format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12M;
    driver.refuse_nv12 = !twoPlanes;
    driver.return_two_planes = twoPlanes;
    V4l2DecoderCaptureGeometry output;
    EXPECT_EQ(-1, driver.Negotiate(&output));
    EXPECT_EQ(ENOTSUP, errno);
  }
}

TEST(V4l2DecoderGeometryTest, RejectsInvalidCropInsteadOfSilentlyClippingIt) {
  const v4l2_rect invalid[] = {
      {-1, 0, 320, 240}, {0, -1, 320, 240}, {0, 0, 0, 240},
      {0, 0, 320, 0},   {1, 0, 320, 240},  {0, 17, 320, 240},
      {321, 0, 1, 1},   {0, 257, 1, 1},
      {1, 0, std::numeric_limits<uint32_t>::max(), 240}};
  for (const auto &rect : invalid) {
    CaptureDriver driver;
    driver.visible = rect;
    V4l2DecoderCaptureGeometry output;
    EXPECT_EQ(-1, driver.Negotiate(&output));
    EXPECT_EQ(EPROTO, errno);
    EXPECT_EQ(0u, output.visible.width);
  }
}

TEST(V4l2DecoderGeometryTest, RejectsInvalidMemoryGeometry) {
  for (bool invalidStride : {false, true}) {
    CaptureDriver driver;
    if (invalidStride) {
      driver.format.fmt.pix_mp.plane_fmt[0].bytesperline = 319;
    } else {
      driver.format.fmt.pix_mp.plane_fmt[0].sizeimage = 0;
    }
    V4l2DecoderCaptureGeometry output;
    EXPECT_EQ(-1, driver.Negotiate(&output));
    EXPECT_EQ(EPROTO, errno);
  }
}

TEST(V4l2DecoderGeometryTest, RejectsNullOutputWithoutIoctl) {
  CaptureDriver driver;
  EXPECT_EQ(-1, driver.Negotiate(nullptr));
  EXPECT_EQ(EINVAL, errno);
  EXPECT_TRUE(driver.requests.empty());
}

} // namespace
} // namespace floral::codec
