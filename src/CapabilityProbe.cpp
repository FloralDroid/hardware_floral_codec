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

#define LOG_TAG "FloralCodecProbe"

#include "floral/codec/CapabilityProbe.h"

#include <android-base/properties.h>
#include <log/log.h>
#include <va/va.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
}

#include <algorithm>
#include <utility>

namespace floral::codec {
namespace {

constexpr char kDefaultVaapiDevice[] = "/dev/dri/renderD128";

bool HasUsableConfiguration(VADisplay display, VAProfile profile,
                            CodecDirection direction) {
  const int maxEntrypoints = vaMaxNumEntrypoints(display);
  if (maxEntrypoints <= 0) {
    return false;
  }

  std::vector<VAEntrypoint> entrypoints(static_cast<size_t>(maxEntrypoints));
  int count = 0;
  if (vaQueryConfigEntrypoints(display, profile, entrypoints.data(), &count) !=
      VA_STATUS_SUCCESS) {
    return false;
  }

  const auto supportsDirection = [direction](VAEntrypoint entrypoint) {
    if (direction == CodecDirection::kDecode) {
      return entrypoint == VAEntrypointVLD;
    }
    return entrypoint == VAEntrypointEncSlice ||
           entrypoint == VAEntrypointEncSliceLP;
  };
  for (auto iterator = entrypoints.begin();
       iterator != entrypoints.begin() + count; ++iterator) {
    if (!supportsDirection(*iterator)) {
      continue;
    }

    VAConfigAttrib attribute{VAConfigAttribRTFormat, VA_ATTRIB_NOT_SUPPORTED};
    if (vaGetConfigAttributes(display, profile, *iterator, &attribute, 1) !=
            VA_STATUS_SUCCESS ||
        attribute.value == VA_ATTRIB_NOT_SUPPORTED ||
        (attribute.value & VA_RT_FORMAT_YUV420) == 0) {
      continue;
    }
    VAConfigID config = VA_INVALID_ID;
    if (vaCreateConfig(display, profile, *iterator, &attribute, 1, &config) ==
        VA_STATUS_SUCCESS) {
      vaDestroyConfig(display, config);
      return true;
    }
  }
  return false;
}

bool HasFfmpegCodec(const CodecSpec &spec) {
  if (spec.direction == CodecDirection::kEncode) {
    if (avcodec_find_encoder_by_name(spec.ffmpeg_name) == nullptr) {
      ALOGW("%s: FFmpeg encoder %s is unavailable", spec.component_name,
            spec.ffmpeg_name);
      return false;
    }
    return true;
  }
  const AVCodec *decoder = avcodec_find_decoder_by_name(spec.ffmpeg_name);
  if (decoder == nullptr) {
    decoder = avcodec_find_decoder(static_cast<AVCodecID>(spec.codec_id));
  }
  if (decoder == nullptr) {
    ALOGW("%s: FFmpeg decoder %s is unavailable", spec.component_name,
          spec.ffmpeg_name);
    return false;
  }
  for (int index = 0;; ++index) {
    const AVCodecHWConfig *config = avcodec_get_hw_config(decoder, index);
    if (config == nullptr) {
      break;
    }
    if (config->device_type == AV_HWDEVICE_TYPE_VAAPI &&
        config->pix_fmt == AV_PIX_FMT_VAAPI) {
      return true;
    }
  }
  ALOGW("%s: FFmpeg decoder %s has no VAAPI hardware configuration",
        spec.component_name, decoder->name);
  return false;
}

} // namespace

struct CapabilityProbe::Impl {
  ~Impl() { av_buffer_unref(&device_context); }

  std::string device_path;
  AVBufferRef *device_context = nullptr;
  VADisplay display = nullptr;
};

CapabilityProbe::CapabilityProbe(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

CapabilityProbe::~CapabilityProbe() = default;

std::unique_ptr<CapabilityProbe>
CapabilityProbe::Create(const std::string &devicePath) {
  auto impl = std::make_unique<Impl>();
  impl->device_path = devicePath;
  const int result =
      av_hwdevice_ctx_create(&impl->device_context, AV_HWDEVICE_TYPE_VAAPI,
                             devicePath.c_str(), nullptr, 0);
  if (result < 0 || impl->device_context == nullptr) {
    ALOGW("VA-API device %s is unavailable: %d", devicePath.c_str(), result);
    return nullptr;
  }

  auto *device =
      reinterpret_cast<AVHWDeviceContext *>(impl->device_context->data);
  auto *vaapi = reinterpret_cast<AVVAAPIDeviceContext *>(device->hwctx);
  impl->display = vaapi->display;
  if (impl->display == nullptr) {
    ALOGW("VA-API device %s returned no display", devicePath.c_str());
    return nullptr;
  }
  return std::unique_ptr<CapabilityProbe>(new CapabilityProbe(std::move(impl)));
}

bool CapabilityProbe::Supports(const CodecSpec &spec) const {
  if (!HasFfmpegCodec(spec)) {
    return false;
  }
  const bool supported =
      std::any_of(spec.va_profiles.begin(), spec.va_profiles.end(),
                  [this, &spec](int profile) {
                    return HasUsableConfiguration(
                        impl_->display, static_cast<VAProfile>(profile),
                        spec.direction);
                  });
  if (!supported) {
    ALOGW("%s: VAAPI exposes no usable %s YUV420 configuration",
          spec.component_name,
          spec.direction == CodecDirection::kDecode ? "VLD" : "encode");
  }
  return supported;
}

const std::string &CapabilityProbe::devicePath() const {
  return impl_->device_path;
}

std::string GetVaapiDevicePath() {
  return android::base::GetProperty("ro.boot.floral_vaapi_device",
                                    kDefaultVaapiDevice);
}

std::vector<const CodecSpec *>
GetSupportedCodecSpecs(const CapabilityProbe &probe) {
  std::vector<const CodecSpec *> supported;
  for (const CodecSpec &spec : GetCodecSpecs()) {
    if (probe.Supports(spec)) {
      supported.push_back(&spec);
      ALOGI("enabled %s on %s", spec.component_name,
            probe.devicePath().c_str());
    } else {
      ALOGV("disabled unsupported component %s", spec.component_name);
    }
  }
  return supported;
}

} // namespace floral::codec
