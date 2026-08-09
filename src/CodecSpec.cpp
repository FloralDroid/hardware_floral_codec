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

#include <media/stagefright/foundation/MediaDefs.h>
#include <va/va.h>

extern "C" {
#include <libavcodec/codec_id.h>
}

namespace floral::codec {
namespace {

constexpr uint32_t kDefaultMaxWidth = 4096;
constexpr uint32_t kDefaultMaxHeight = 4096;

} // namespace

const std::vector<CodecSpec> &GetCodecSpecs() {
  // Profiles are deliberately limited to 8-bit output until the Android
  // P010 GraphicBuffer path is implemented end to end.
  static const std::vector<CodecSpec> specs = {
      {"c2.floral.avc.encoder",
       android::MEDIA_MIMETYPE_VIDEO_AVC,
       "h264_vaapi",
       AV_CODEC_ID_H264,
       CodecDirection::kEncode,
       {VAProfileH264High, VAProfileH264Main, VAProfileH264ConstrainedBaseline},
       kDefaultMaxWidth,
       kDefaultMaxHeight},
      {"c2.floral.avc.decoder",
       android::MEDIA_MIMETYPE_VIDEO_AVC,
       "h264",
       AV_CODEC_ID_H264,
       CodecDirection::kDecode,
       {VAProfileH264High, VAProfileH264Main, VAProfileH264ConstrainedBaseline},
       kDefaultMaxWidth,
       kDefaultMaxHeight},
      {"c2.floral.hevc.encoder",
       android::MEDIA_MIMETYPE_VIDEO_HEVC,
       "hevc_vaapi",
       AV_CODEC_ID_HEVC,
       CodecDirection::kEncode,
       {VAProfileHEVCMain},
       kDefaultMaxWidth,
       kDefaultMaxHeight},
      {"c2.floral.hevc.decoder",
       android::MEDIA_MIMETYPE_VIDEO_HEVC,
       "hevc",
       AV_CODEC_ID_HEVC,
       CodecDirection::kDecode,
       {VAProfileHEVCMain},
       kDefaultMaxWidth,
       kDefaultMaxHeight},
      {"c2.floral.vp8.encoder",
       android::MEDIA_MIMETYPE_VIDEO_VP8,
       "vp8_vaapi",
       AV_CODEC_ID_VP8,
       CodecDirection::kEncode,
       {VAProfileVP8Version0_3},
       kDefaultMaxWidth,
       kDefaultMaxHeight},
      {"c2.floral.vp8.decoder",
       android::MEDIA_MIMETYPE_VIDEO_VP8,
       "vp8",
       AV_CODEC_ID_VP8,
       CodecDirection::kDecode,
       {VAProfileVP8Version0_3},
       kDefaultMaxWidth,
       kDefaultMaxHeight},
      {"c2.floral.vp9.encoder",
       android::MEDIA_MIMETYPE_VIDEO_VP9,
       "vp9_vaapi",
       AV_CODEC_ID_VP9,
       CodecDirection::kEncode,
       {VAProfileVP9Profile0},
       kDefaultMaxWidth,
       kDefaultMaxHeight},
      {"c2.floral.vp9.decoder",
       android::MEDIA_MIMETYPE_VIDEO_VP9,
       "vp9",
       AV_CODEC_ID_VP9,
       CodecDirection::kDecode,
       {VAProfileVP9Profile0},
       kDefaultMaxWidth,
       kDefaultMaxHeight},
      {"c2.floral.av1.encoder",
       android::MEDIA_MIMETYPE_VIDEO_AV1,
       "av1_vaapi",
       AV_CODEC_ID_AV1,
       CodecDirection::kEncode,
       {VAProfileAV1Profile0},
       kDefaultMaxWidth,
       kDefaultMaxHeight},
      {"c2.floral.av1.decoder",
       android::MEDIA_MIMETYPE_VIDEO_AV1,
       "av1",
       AV_CODEC_ID_AV1,
       CodecDirection::kDecode,
       {VAProfileAV1Profile0},
       kDefaultMaxWidth,
       kDefaultMaxHeight},
      {"c2.floral.mpeg2.encoder",
       android::MEDIA_MIMETYPE_VIDEO_MPEG2,
       "mpeg2_vaapi",
       AV_CODEC_ID_MPEG2VIDEO,
       CodecDirection::kEncode,
       {VAProfileMPEG2Main, VAProfileMPEG2Simple},
       kDefaultMaxWidth,
       kDefaultMaxHeight},
      {"c2.floral.mpeg2.decoder",
       android::MEDIA_MIMETYPE_VIDEO_MPEG2,
       "mpeg2video",
       AV_CODEC_ID_MPEG2VIDEO,
       CodecDirection::kDecode,
       {VAProfileMPEG2Main, VAProfileMPEG2Simple},
       kDefaultMaxWidth,
       kDefaultMaxHeight},
  };
  return specs;
}

} // namespace floral::codec
