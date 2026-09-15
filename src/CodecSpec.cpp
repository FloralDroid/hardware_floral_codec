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

#include "floral/codec/CodecSpec.h"

#include "Backend.h"

#include <media/stagefright/foundation/MediaDefs.h>
#if defined(FLORAL_CODEC_BACKEND_VAAPI)
#include <va/va.h>
#define VA_PROFILES(...) {__VA_ARGS__}
#else
#define VA_PROFILES(...) {}
#endif

extern "C" {
#include <libavcodec/codec_id.h>
}

namespace floral::codec {
namespace {

constexpr uint32_t kDefaultMaxWidth = 4096;
constexpr uint32_t kDefaultMaxHeight = 4096;

#if defined(FLORAL_CODEC_BACKEND_V4L2_M2M)
constexpr char kAvcEncoder[] = "h264_v4l2m2m";
constexpr char kAvcDecoder[] = "h264_v4l2m2m";
constexpr char kHevcEncoder[] = "hevc_v4l2m2m";
constexpr char kHevcDecoder[] = "hevc_v4l2m2m";
constexpr char kVp8Encoder[] = "vp8_v4l2m2m";
constexpr char kVp8Decoder[] = "vp8_v4l2m2m";
constexpr char kVp9Decoder[] = "vp9_v4l2m2m";
constexpr char kMpeg2Decoder[] = "mpeg2_v4l2m2m";
#else
constexpr char kAvcEncoder[] = "h264_vaapi";
constexpr char kAvcDecoder[] = "h264";
constexpr char kHevcEncoder[] = "hevc_vaapi";
constexpr char kHevcDecoder[] = "hevc";
constexpr char kVp8Encoder[] = "vp8_vaapi";
constexpr char kVp8Decoder[] = "vp8";
constexpr char kVp9Encoder[] = "vp9_vaapi";
constexpr char kVp9Decoder[] = "vp9";
constexpr char kAv1Encoder[] = "av1_vaapi";
constexpr char kAv1Decoder[] = "av1";
constexpr char kMpeg2Encoder[] = "mpeg2_vaapi";
constexpr char kMpeg2Decoder[] = "mpeg2video";
#endif

} // namespace

const std::vector<CodecSpec> &GetCodecSpecs() {
  // Profiles are deliberately limited to 8-bit output until the Android
  // P010 GraphicBuffer path is implemented end to end.
  static const std::vector<CodecSpec> specs = {
      {"c2.floral.avc.encoder",
       android::MEDIA_MIMETYPE_VIDEO_AVC,
       kAvcEncoder,
       AV_CODEC_ID_H264,
       CodecDirection::kEncode,
       VA_PROFILES(VAProfileH264High, VAProfileH264Main,
                   VAProfileH264ConstrainedBaseline),
       kDefaultMaxWidth,
       kDefaultMaxHeight},
      {"c2.floral.avc.decoder",
       android::MEDIA_MIMETYPE_VIDEO_AVC,
       kAvcDecoder,
       AV_CODEC_ID_H264,
       CodecDirection::kDecode,
       VA_PROFILES(VAProfileH264High, VAProfileH264Main,
                   VAProfileH264ConstrainedBaseline),
       kDefaultMaxWidth,
       kDefaultMaxHeight},
      {"c2.floral.hevc.encoder",
       android::MEDIA_MIMETYPE_VIDEO_HEVC,
       kHevcEncoder,
       AV_CODEC_ID_HEVC,
       CodecDirection::kEncode,
       VA_PROFILES(VAProfileHEVCMain),
       kDefaultMaxWidth,
       kDefaultMaxHeight},
      {"c2.floral.hevc.decoder",
       android::MEDIA_MIMETYPE_VIDEO_HEVC,
       kHevcDecoder,
       AV_CODEC_ID_HEVC,
       CodecDirection::kDecode,
       VA_PROFILES(VAProfileHEVCMain),
       kDefaultMaxWidth,
       kDefaultMaxHeight},
      {"c2.floral.vp8.encoder",
       android::MEDIA_MIMETYPE_VIDEO_VP8,
       kVp8Encoder,
       AV_CODEC_ID_VP8,
       CodecDirection::kEncode,
       VA_PROFILES(VAProfileVP8Version0_3),
       kDefaultMaxWidth,
       kDefaultMaxHeight},
      {"c2.floral.vp8.decoder",
       android::MEDIA_MIMETYPE_VIDEO_VP8,
       kVp8Decoder,
       AV_CODEC_ID_VP8,
       CodecDirection::kDecode,
       VA_PROFILES(VAProfileVP8Version0_3),
       kDefaultMaxWidth,
       kDefaultMaxHeight},
#if defined(FLORAL_CODEC_BACKEND_VAAPI)
      {"c2.floral.vp9.encoder",
       android::MEDIA_MIMETYPE_VIDEO_VP9,
       kVp9Encoder,
       AV_CODEC_ID_VP9,
       CodecDirection::kEncode,
       VA_PROFILES(VAProfileVP9Profile0),
       kDefaultMaxWidth,
       kDefaultMaxHeight},
#endif
      {"c2.floral.vp9.decoder",
       android::MEDIA_MIMETYPE_VIDEO_VP9,
       kVp9Decoder,
       AV_CODEC_ID_VP9,
       CodecDirection::kDecode,
       VA_PROFILES(VAProfileVP9Profile0),
       kDefaultMaxWidth,
       kDefaultMaxHeight},
#if defined(FLORAL_CODEC_BACKEND_VAAPI)
      {"c2.floral.av1.encoder",
       android::MEDIA_MIMETYPE_VIDEO_AV1,
       kAv1Encoder,
       AV_CODEC_ID_AV1,
       CodecDirection::kEncode,
       VA_PROFILES(VAProfileAV1Profile0),
       kDefaultMaxWidth,
       kDefaultMaxHeight},
      {"c2.floral.av1.decoder",
       android::MEDIA_MIMETYPE_VIDEO_AV1,
       kAv1Decoder,
       AV_CODEC_ID_AV1,
       CodecDirection::kDecode,
       VA_PROFILES(VAProfileAV1Profile0),
       kDefaultMaxWidth,
       kDefaultMaxHeight},
      {"c2.floral.mpeg2.encoder",
       android::MEDIA_MIMETYPE_VIDEO_MPEG2,
       kMpeg2Encoder,
       AV_CODEC_ID_MPEG2VIDEO,
       CodecDirection::kEncode,
       VA_PROFILES(VAProfileMPEG2Main, VAProfileMPEG2Simple),
       kDefaultMaxWidth,
       kDefaultMaxHeight},
#endif
      {"c2.floral.mpeg2.decoder",
       android::MEDIA_MIMETYPE_VIDEO_MPEG2,
       kMpeg2Decoder,
       AV_CODEC_ID_MPEG2VIDEO,
       CodecDirection::kDecode,
       VA_PROFILES(VAProfileMPEG2Main, VAProfileMPEG2Simple),
       kDefaultMaxWidth,
       kDefaultMaxHeight},
  };
  return specs;
}

} // namespace floral::codec
