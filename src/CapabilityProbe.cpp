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

#include "Backend.h"

#include <android-base/properties.h>
#include <log/log.h>
#if defined(FLORAL_CODEC_BACKEND_VAAPI)
#include <va/va.h>
#else
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

extern "C" {
#if defined(FLORAL_CODEC_BACKEND_VAAPI)
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#endif
}

#include <algorithm>
#include <cstring>
#include <dirent.h>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace floral::codec {
namespace {

#if defined(FLORAL_CODEC_BACKEND_VAAPI)
AVCodecID ToAvCodecId(CodecType codec) {
  switch (codec) {
  case CodecType::kAvc:
    return AV_CODEC_ID_H264;
  case CodecType::kHevc:
    return AV_CODEC_ID_HEVC;
  case CodecType::kVp8:
    return AV_CODEC_ID_VP8;
  case CodecType::kVp9:
    return AV_CODEC_ID_VP9;
  case CodecType::kAv1:
    return AV_CODEC_ID_AV1;
  case CodecType::kMpeg2:
    return AV_CODEC_ID_MPEG2VIDEO;
  }
  return AV_CODEC_ID_NONE;
}

struct VaapiDevice {
  VaapiDevice(std::string devicePath, AVBufferRef *deviceContext,
              VADisplay vaDisplay)
      : path(std::move(devicePath)), context(deviceContext), display(vaDisplay) {}

  VaapiDevice(VaapiDevice &&other) noexcept
      : path(std::move(other.path)),
        context(std::exchange(other.context, nullptr)), display(other.display) {}

  VaapiDevice &operator=(VaapiDevice &&) = delete;
  VaapiDevice(const VaapiDevice &) = delete;
  VaapiDevice &operator=(const VaapiDevice &) = delete;
  ~VaapiDevice() { av_buffer_unref(&context); }

  std::string path;
  AVBufferRef *context = nullptr;
  VADisplay display = nullptr;
};

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
    decoder = avcodec_find_decoder(ToAvCodecId(spec.codec));
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
#endif

bool IsNumberedNode(const char *name, const char *prefix) {
  const size_t prefixLength = std::strlen(prefix);
  if (std::strncmp(name, prefix, prefixLength) != 0 ||
      name[prefixLength] == '\0') {
    return false;
  }
  for (const char *digit = name + prefixLength; *digit != '\0'; ++digit) {
    if (*digit < '0' || *digit > '9') {
      return false;
    }
  }
  return true;
}

std::vector<std::string> GetDevicePaths(const std::string &configuredPath,
                                        const char *directoryPath,
                                        const char *prefix) {
  if (!configuredPath.empty()) {
    return {configuredPath};
  }
  std::vector<std::string> paths;
  DIR *directory = opendir(directoryPath);
  if (directory == nullptr) {
    return paths;
  }
  while (dirent *entry = readdir(directory)) {
    if (IsNumberedNode(entry->d_name, prefix)) {
      paths.emplace_back(std::string(directoryPath) + "/" + entry->d_name);
    }
  }
  closedir(directory);
  std::sort(paths.begin(), paths.end());
  return paths;
}

#if defined(FLORAL_CODEC_BACKEND_V4L2_M2M)
std::vector<std::string> GetV4l2DevicePaths(const std::string &devicePath) {
  return GetDevicePaths(devicePath, "/dev", "video");
}

uint32_t DeviceCapabilities(const v4l2_capability &capability) {
  return (capability.capabilities & V4L2_CAP_DEVICE_CAPS) != 0
             ? capability.device_caps
             : capability.capabilities;
}

bool SupportsMultiPlaneQueues(uint32_t capabilities) {
  return (capabilities & V4L2_CAP_STREAMING) != 0 &&
         ((capabilities & V4L2_CAP_VIDEO_M2M_MPLANE) != 0 ||
          ((capabilities & V4L2_CAP_VIDEO_OUTPUT_MPLANE) != 0 &&
           (capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE) != 0));
}

bool HasFormat(int fd, v4l2_buf_type type, uint32_t pixelFormat) {
  v4l2_fmtdesc format{};
  format.type = type;
  for (format.index = 0; ioctl(fd, VIDIOC_ENUM_FMT, &format) == 0;
       ++format.index) {
    if (format.pixelformat == pixelFormat) {
      return true;
    }
  }
  return false;
}

bool HasRawFormat(int fd, v4l2_buf_type type) {
  return HasFormat(fd, type, V4L2_PIX_FMT_NV12);
}

uint32_t GetCompressedFormat(CodecType codec) {
  switch (codec) {
  case CodecType::kAvc:
    return V4L2_PIX_FMT_H264;
  case CodecType::kHevc:
    return V4L2_PIX_FMT_HEVC;
  case CodecType::kMpeg2:
    return V4L2_PIX_FMT_MPEG2;
  case CodecType::kVp8:
    return V4L2_PIX_FMT_VP8;
  case CodecType::kVp9:
    return V4L2_PIX_FMT_VP9;
  case CodecType::kAv1:
    return 0;
  default:
    return 0;
  }
}

bool QueuePairSupports(int fd, v4l2_buf_type outputType,
                       v4l2_buf_type captureType, uint32_t compressedFormat,
                       CodecDirection direction) {
  return direction == CodecDirection::kDecode
             ? HasFormat(fd, outputType, compressedFormat) &&
                   HasRawFormat(fd, captureType)
             : HasRawFormat(fd, outputType) &&
                   HasFormat(fd, captureType, compressedFormat);
}

bool SupportsDmaBuf(int fd, v4l2_buf_type rawType) {
  v4l2_requestbuffers request{};
  request.type = rawType;
  request.memory = V4L2_MEMORY_DMABUF;
  request.count = 0;
  return ioctl(fd, VIDIOC_REQBUFS, &request) == 0;
}

bool DeviceSupports(const std::string &path, const CodecSpec &spec) {
  const int fd = open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }

  v4l2_capability capability{};
  const bool queried = ioctl(fd, VIDIOC_QUERYCAP, &capability) == 0;
  const uint32_t capabilities = queried ? DeviceCapabilities(capability) : 0;
  const uint32_t compressedFormat = GetCompressedFormat(spec.codec);
  const v4l2_buf_type rawType = spec.direction == CodecDirection::kEncode
                                    ? V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE
                                    : V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  const bool supported =
      compressedFormat != 0 && SupportsMultiPlaneQueues(capabilities) &&
      QueuePairSupports(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
                        V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, compressedFormat,
                        spec.direction) &&
      SupportsDmaBuf(fd, rawType);
  close(fd);
  return supported;
}
#endif

} // namespace

struct CapabilityProbe::Impl {
#if defined(FLORAL_CODEC_BACKEND_VAAPI)
  std::vector<VaapiDevice> devices;
#else
  std::vector<std::string> device_paths;
#endif
};

CapabilityProbe::CapabilityProbe(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

CapabilityProbe::~CapabilityProbe() = default;

std::unique_ptr<CapabilityProbe>
CapabilityProbe::Create(const std::string &devicePath) {
  auto impl = std::make_unique<Impl>();
#if defined(FLORAL_CODEC_BACKEND_V4L2_M2M)
  impl->device_paths = GetV4l2DevicePaths(devicePath);
  if (impl->device_paths.empty()) {
    ALOGW("no V4L2 M2M device is available");
    return nullptr;
  }
  return std::unique_ptr<CapabilityProbe>(new CapabilityProbe(std::move(impl)));
#else
  for (const std::string &path :
       GetDevicePaths(devicePath, "/dev/dri", "renderD")) {
    AVBufferRef *context = nullptr;
    const int result = av_hwdevice_ctx_create(
        &context, AV_HWDEVICE_TYPE_VAAPI, path.c_str(), nullptr, 0);
    if (result < 0 || context == nullptr) {
      ALOGV("VA-API device %s is unavailable: %d", path.c_str(), result);
      continue;
    }
    auto *device = reinterpret_cast<AVHWDeviceContext *>(context->data);
    auto *vaapi = reinterpret_cast<AVVAAPIDeviceContext *>(device->hwctx);
    if (vaapi == nullptr || vaapi->display == nullptr) {
      av_buffer_unref(&context);
      continue;
    }
    impl->devices.emplace_back(path, context, vaapi->display);
  }
  if (impl->devices.empty()) {
    ALOGW("no usable VA-API render device is available");
    return nullptr;
  }
  return std::unique_ptr<CapabilityProbe>(new CapabilityProbe(std::move(impl)));
#endif
}

std::optional<std::string>
CapabilityProbe::FindDevicePath(const CodecSpec &spec) const {
#if defined(FLORAL_CODEC_BACKEND_V4L2_M2M)
  const auto device = std::find_if(
      impl_->device_paths.begin(), impl_->device_paths.end(),
      [&spec](const std::string &path) { return DeviceSupports(path, spec); });
  if (device == impl_->device_paths.end()) {
    ALOGW("%s: no V4L2 M2M device exposes the required queue formats",
          spec.component_name);
    return std::nullopt;
  }
  ALOGI("%s: V4L2 M2M codec is available on %s", spec.component_name,
        device->c_str());
  return *device;
#else
  if (!HasFfmpegCodec(spec)) {
    return std::nullopt;
  }
  const auto device = std::find_if(
      impl_->devices.begin(), impl_->devices.end(), [&spec](const VaapiDevice &candidate) {
        return std::any_of(
            spec.va_profiles.begin(), spec.va_profiles.end(),
            [&spec, &candidate](int profile) {
              return HasUsableConfiguration(candidate.display,
                                            static_cast<VAProfile>(profile),
                                            spec.direction);
            });
      });
  if (device == impl_->devices.end()) {
    ALOGW("%s: VAAPI exposes no usable %s YUV420 configuration",
          spec.component_name,
          spec.direction == CodecDirection::kDecode ? "VLD" : "encode");
    return std::nullopt;
  }
  ALOGI("%s: VA-API codec is available on %s", spec.component_name,
        device->path.c_str());
  return device->path;
#endif
}

std::string GetCodecDevicePath() {
#if defined(FLORAL_CODEC_BACKEND_V4L2_M2M)
  return android::base::GetProperty("ro.boot.floral_v4l2_device", "");
#else
  return android::base::GetProperty("ro.boot.floral_vaapi_device", "");
#endif
}

} // namespace floral::codec
