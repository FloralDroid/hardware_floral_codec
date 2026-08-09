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

#define LOG_TAG "FloralCodec2"

#include "floral/codec/FloralCodecComponent.h"

#include <C2PlatformSupport.h>
#include <SimpleC2Component.h>
#include <SimpleC2Interface.h>
#include <hardware/gralloc.h>
#include <log/log.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
}

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace floral::codec {
namespace {

constexpr uint32_t kDefaultWidth = 1280;
constexpr uint32_t kDefaultHeight = 720;
constexpr uint32_t kMaxPictureDimension = 4096;
constexpr uint32_t kDefaultBitrate = 4'000'000;
constexpr float kDefaultFrameRate = 30.0f;
constexpr uint32_t kMinInputBufferSize = 2 * 1024 * 1024;

uint8_t ClampByte(int value) {
  return static_cast<uint8_t>(std::clamp(value, 0, 255));
}

std::string AvError(int error) {
  char text[AV_ERROR_MAX_STRING_SIZE]{};
  if (av_strerror(error, text, sizeof(text)) < 0) {
    return "FFmpeg error " + std::to_string(error);
  }
  return text;
}

class FloralCodecInterface : public android::SimpleInterface<void>::BaseParams {
public:
  struct EncoderSettings {
    uint32_t width;
    uint32_t height;
    uint32_t bitrate;
    float frame_rate;
    int64_t sync_interval_us;
    bool request_sync;
  };

  FloralCodecInterface(const std::shared_ptr<C2ReflectorHelper> &helper,
                       const CodecSpec &spec)
      : android::SimpleInterface<void>::BaseParams(
            helper, spec.component_name, ToC2Kind(spec.direction),
            C2Component::DOMAIN_VIDEO, spec.media_type),
        spec_(spec) {
    noPrivateBuffers();
    noInputReferences();
    noOutputReferences();
    noInputLatency();
    noTimeStretch();
    setDerivedInstance(this);

    addParameter(DefineParam(mAttrib, C2_PARAMKEY_COMPONENT_ATTRIBUTES)
                     .withConstValue(new C2ComponentAttributesSetting(
                         C2Component::ATTRIB_IS_TEMPORAL))
                     .build());

    if (spec.direction == CodecDirection::kEncode) {
      AddEncoderParameters();
    } else {
      AddDecoderParameters();
    }
  }

  EncoderSettings GetEncoderSettings() const {
    Lock lock = this->lock();
    return EncoderSettings{
        mInputSize->width,       mInputSize->height,
        mBitrate->value,         mFrameRate->value,
        mSyncFramePeriod->value, mRequestSync->value == C2_TRUE};
  }

  void ClearSyncRequest() {
    C2StreamRequestSyncFrameTuning::output clear(0u, C2_FALSE);
    std::vector<std::unique_ptr<C2SettingResult>> failures;
    (void)config({&clear}, C2_MAY_BLOCK, &failures);
  }

  c2_status_t UpdateOutputSize(uint32_t width, uint32_t height,
                               std::vector<std::unique_ptr<C2Param>> *updates) {
    C2StreamPictureSizeInfo::output size(0u, width, height);
    std::vector<std::unique_ptr<C2SettingResult>> failures;
    c2_status_t result = config({&size}, C2_MAY_BLOCK, &failures);
    if (result == C2_OK && updates != nullptr) {
      updates->push_back(C2Param::Copy(size));
    }
    return result;
  }

private:
  static C2R
  EncoderSizeSetter(bool, const C2P<C2StreamPictureSizeInfo::input> &oldValue,
                    C2P<C2StreamPictureSizeInfo::input> &value) {
    C2R result = C2R::Ok();
    if (!value.F(value.v.width).supportsAtAll(value.v.width)) {
      value.set().width = oldValue.v.width;
      result =
          result.plus(C2SettingResultBuilder::BadValue(value.F(value.v.width)));
    }
    if (!value.F(value.v.height).supportsAtAll(value.v.height)) {
      value.set().height = oldValue.v.height;
      result = result.plus(
          C2SettingResultBuilder::BadValue(value.F(value.v.height)));
    }
    return result;
  }

  static C2R
  DecoderSizeSetter(bool, const C2P<C2StreamPictureSizeInfo::output> &oldValue,
                    C2P<C2StreamPictureSizeInfo::output> &value) {
    C2R result = C2R::Ok();
    if (!value.F(value.v.width).supportsAtAll(value.v.width)) {
      value.set().width = oldValue.v.width;
      result =
          result.plus(C2SettingResultBuilder::BadValue(value.F(value.v.width)));
    }
    if (!value.F(value.v.height).supportsAtAll(value.v.height)) {
      value.set().height = oldValue.v.height;
      result = result.plus(
          C2SettingResultBuilder::BadValue(value.F(value.v.height)));
    }
    return result;
  }

  static C2R BitrateSetter(bool, C2P<C2StreamBitrateInfo::output> &value) {
    value.set().value = std::max<uint32_t>(value.v.value, 16'000);
    return C2R::Ok();
  }

  static C2R
  MaxOutputSizeSetter(bool, C2P<C2StreamMaxPictureSizeTuning::output> &value,
                      const C2P<C2StreamPictureSizeInfo::output> &size) {
    value.set().width =
        c2_min(c2_max(value.v.width, size.v.width), kMaxPictureDimension);
    value.set().height =
        c2_min(c2_max(value.v.height, size.v.height), kMaxPictureDimension);
    return C2R::Ok();
  }

  static C2R
  EncoderProfileLevelSetter(bool,
                            C2P<C2StreamProfileLevelInfo::output> &value) {
    (void)value;
    return C2R::Ok();
  }

  static C2R
  DecoderProfileLevelSetter(bool, C2P<C2StreamProfileLevelInfo::input> &value) {
    (void)value;
    return C2R::Ok();
  }

  static C2R
  MaxInputSizeSetter(bool, C2P<C2StreamMaxBufferSizeInfo::input> &value,
                     const C2P<C2StreamMaxPictureSizeTuning::output> &maxSize) {
    value.set().value = std::max<uint32_t>(
        kMinInputBufferSize,
        ((maxSize.v.width + 15) / 16) * ((maxSize.v.height + 15) / 16) * 192);
    return C2R::Ok();
  }

  void AddEncoderParameters() {
    addParameter(DefineParam(mUsage, C2_PARAMKEY_INPUT_STREAM_USAGE)
                     .withConstValue(new C2StreamUsageTuning::input(
                         0u, static_cast<uint64_t>(C2MemoryUsage::CPU_READ)))
                     .build());
    addParameter(
        DefineParam(mInputSize, C2_PARAMKEY_PICTURE_SIZE)
            .withDefault(new C2StreamPictureSizeInfo::input(0u, kDefaultWidth,
                                                            kDefaultHeight))
            .withFields(
                {C2F(mInputSize, width).inRange(64, spec_.max_width, 2),
                 C2F(mInputSize, height).inRange(64, spec_.max_height, 2)})
            .withSetter(EncoderSizeSetter)
            .build());
    addParameter(
        DefineParam(mFrameRate, C2_PARAMKEY_FRAME_RATE)
            .withDefault(
                new C2StreamFrameRateInfo::output(0u, kDefaultFrameRate))
            .withFields({C2F(mFrameRate, value).inRange(1.0f, 240.0f)})
            .withSetter(android::Setter<
                        decltype(*mFrameRate)>::NonStrictValueWithNoDeps)
            .build());
    addParameter(
        DefineParam(mBitrate, C2_PARAMKEY_BITRATE)
            .withDefault(new C2StreamBitrateInfo::output(0u, kDefaultBitrate))
            .withFields({C2F(mBitrate, value).inRange(16'000, 200'000'000)})
            .withSetter(BitrateSetter)
            .build());
    addParameter(
        DefineParam(mBitrateMode, C2_PARAMKEY_BITRATE_MODE)
            .withDefault(new C2StreamBitrateModeTuning::output(
                0u, C2Config::BITRATE_VARIABLE))
            .withFields({C2F(mBitrateMode, value)
                             .oneOf({C2Config::BITRATE_CONST,
                                     C2Config::BITRATE_VARIABLE})})
            .withSetter(android::Setter<
                        decltype(*mBitrateMode)>::NonStrictValueWithNoDeps)
            .build());
    addParameter(
        DefineParam(mRequestSync, C2_PARAMKEY_REQUEST_SYNC_FRAME)
            .withDefault(
                new C2StreamRequestSyncFrameTuning::output(0u, C2_FALSE))
            .withFields({C2F(mRequestSync, value).oneOf({C2_FALSE, C2_TRUE})})
            .withSetter(android::Setter<
                        decltype(*mRequestSync)>::NonStrictValueWithNoDeps)
            .build());
    addParameter(
        DefineParam(mSyncFramePeriod, C2_PARAMKEY_SYNC_FRAME_INTERVAL)
            .withDefault(
                new C2StreamSyncFrameIntervalTuning::output(0u, 2'000'000))
            .withFields({C2F(mSyncFramePeriod, value).any()})
            .withSetter(android::Setter<
                        decltype(*mSyncFramePeriod)>::StrictValueWithNoDeps)
            .build());
    addParameter(DefineParam(mEncoderProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
                     .withDefault(new C2StreamProfileLevelInfo::output(
                         0u, C2Config::PROFILE_UNUSED, C2Config::LEVEL_UNUSED))
                     .withFields({C2F(mEncoderProfileLevel, profile).any(),
                                  C2F(mEncoderProfileLevel, level).any()})
                     .withSetter(EncoderProfileLevelSetter)
                     .build());
  }

  void AddDecoderParameters() {
    addParameter(
        DefineParam(mOutputSize, C2_PARAMKEY_PICTURE_SIZE)
            .withDefault(new C2StreamPictureSizeInfo::output(0u, kDefaultWidth,
                                                             kDefaultHeight))
            .withFields(
                {C2F(mOutputSize, width).inRange(64, spec_.max_width, 2),
                 C2F(mOutputSize, height).inRange(64, spec_.max_height, 2)})
            .withSetter(DecoderSizeSetter)
            .build());
    addParameter(
        DefineParam(mMaxOutputSize, C2_PARAMKEY_MAX_PICTURE_SIZE)
            .withDefault(new C2StreamMaxPictureSizeTuning::output(
                0u, spec_.max_width, spec_.max_height))
            .withFields(
                {C2F(mMaxOutputSize, width).inRange(64, spec_.max_width, 2),
                 C2F(mMaxOutputSize, height).inRange(64, spec_.max_height, 2)})
            .withSetter(MaxOutputSizeSetter, mOutputSize)
            .build());
    addParameter(DefineParam(mMaxInputSize, C2_PARAMKEY_INPUT_MAX_BUFFER_SIZE)
                     .withDefault(new C2StreamMaxBufferSizeInfo::input(
                         0u, kMinInputBufferSize))
                     .withFields({C2F(mMaxInputSize, value).any()})
                     .calculatedAs(MaxInputSizeSetter, mMaxOutputSize)
                     .build());
    addParameter(DefineParam(mPixelFormat, C2_PARAMKEY_PIXEL_FORMAT)
                     .withConstValue(new C2StreamPixelFormatInfo::output(
                         0u, HAL_PIXEL_FORMAT_YCBCR_420_888))
                     .build());
    addParameter(DefineParam(mDecoderProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
                     .withDefault(new C2StreamProfileLevelInfo::input(
                         0u, C2Config::PROFILE_UNUSED, C2Config::LEVEL_UNUSED))
                     .withFields({C2F(mDecoderProfileLevel, profile).any(),
                                  C2F(mDecoderProfileLevel, level).any()})
                     .withSetter(DecoderProfileLevelSetter)
                     .build());
  }

  CodecSpec spec_;
  std::shared_ptr<C2StreamUsageTuning::input> mUsage;
  std::shared_ptr<C2StreamPictureSizeInfo::input> mInputSize;
  std::shared_ptr<C2StreamPictureSizeInfo::output> mOutputSize;
  std::shared_ptr<C2StreamMaxPictureSizeTuning::output> mMaxOutputSize;
  std::shared_ptr<C2StreamMaxBufferSizeInfo::input> mMaxInputSize;
  std::shared_ptr<C2StreamPixelFormatInfo::output> mPixelFormat;
  std::shared_ptr<C2StreamFrameRateInfo::output> mFrameRate;
  std::shared_ptr<C2StreamBitrateInfo::output> mBitrate;
  std::shared_ptr<C2StreamBitrateModeTuning::output> mBitrateMode;
  std::shared_ptr<C2StreamRequestSyncFrameTuning::output> mRequestSync;
  std::shared_ptr<C2StreamSyncFrameIntervalTuning::output> mSyncFramePeriod;
  std::shared_ptr<C2StreamProfileLevelInfo::input> mDecoderProfileLevel;
  std::shared_ptr<C2StreamProfileLevelInfo::output> mEncoderProfileLevel;
};

class FfmpegSession {
public:
  FfmpegSession(CodecSpec spec, std::string devicePath)
      : spec_(std::move(spec)), device_path_(std::move(devicePath)) {}

  ~FfmpegSession() { Close(); }

  c2_status_t
  Open(const FloralCodecInterface::EncoderSettings *encoderSettings) {
    Close();
    int result =
        av_hwdevice_ctx_create(&device_context_, AV_HWDEVICE_TYPE_VAAPI,
                               device_path_.c_str(), nullptr, 0);
    if (result < 0) {
      return Fail("creating VA-API device", result);
    }

    const AVCodec *codec = nullptr;
    if (spec_.direction == CodecDirection::kEncode) {
      codec = avcodec_find_encoder_by_name(spec_.ffmpeg_name);
    } else {
      codec = avcodec_find_decoder_by_name(spec_.ffmpeg_name);
      if (codec == nullptr) {
        codec = avcodec_find_decoder(static_cast<AVCodecID>(spec_.codec_id));
      }
    }
    if (codec == nullptr) {
      ALOGE("%s is unavailable", spec_.ffmpeg_name);
      return C2_NOT_FOUND;
    }

    context_ = avcodec_alloc_context3(codec);
    if (context_ == nullptr) {
      return C2_NO_MEMORY;
    }
    context_->hw_device_ctx = av_buffer_ref(device_context_);
    context_->opaque = this;

    if (spec_.direction == CodecDirection::kEncode) {
      if (encoderSettings == nullptr) {
        return C2_BAD_VALUE;
      }
      result = ConfigureEncoder(*encoderSettings);
      if (result < 0) {
        return Fail("configuring encoder", result);
      }
    } else {
      context_->get_format = SelectHardwareFormat;
      context_->pkt_timebase = AVRational{1, 1'000'000};
    }

    result = avcodec_open2(context_, codec, nullptr);
    if (result < 0) {
      return Fail("opening codec", result);
    }
    frame_ = av_frame_alloc();
    software_frame_ = av_frame_alloc();
    packet_ = av_packet_alloc();
    return frame_ != nullptr && software_frame_ != nullptr && packet_ != nullptr
               ? C2_OK
               : C2_NO_MEMORY;
  }

  void Flush() {
    if (context_ != nullptr) {
      avcodec_flush_buffers(context_);
    }
    config_sent_ = false;
  }

  void Close() {
    av_packet_free(&packet_);
    av_frame_free(&software_frame_);
    av_frame_free(&frame_);
    avcodec_free_context(&context_);
    av_buffer_unref(&frames_context_);
    av_buffer_unref(&device_context_);
    config_sent_ = false;
  }

  AVCodecContext *context() const { return context_; }
  AVFrame *frame() const { return frame_; }
  AVFrame *softwareFrame() const { return software_frame_; }
  AVPacket *packet() const { return packet_; }
  bool configSent() const { return config_sent_; }
  void markConfigSent() { config_sent_ = true; }
  const CodecSpec &spec() const { return spec_; }

  c2_status_t PrepareEncoderFrame(const C2GraphicView &view,
                                  uint64_t frameIndex, bool requestSync) {
    if (context_ == nullptr || frames_context_ == nullptr) {
      return C2_NO_INIT;
    }
    av_frame_unref(software_frame_);
    software_frame_->format = AV_PIX_FMT_NV12;
    software_frame_->width = context_->width;
    software_frame_->height = context_->height;
    int result = av_frame_get_buffer(software_frame_, 32);
    if (result < 0) {
      return Fail("allocating encoder staging frame", result);
    }
    result = CopyGraphicViewToNv12(view, software_frame_);
    if (result != 0) {
      return C2_BAD_VALUE;
    }

    av_frame_unref(frame_);
    frame_->format = AV_PIX_FMT_VAAPI;
    frame_->width = context_->width;
    frame_->height = context_->height;
    frame_->hw_frames_ctx = av_buffer_ref(frames_context_);
    if (frame_->hw_frames_ctx == nullptr) {
      return C2_NO_MEMORY;
    }
    result = av_hwframe_get_buffer(frames_context_, frame_, 0);
    if (result < 0) {
      return Fail("allocating VA-API frame", result);
    }
    result = av_hwframe_transfer_data(frame_, software_frame_, 0);
    if (result < 0) {
      return Fail("uploading VA-API frame", result);
    }
    frame_->pts = static_cast<int64_t>(frameIndex);
    if (requestSync) {
      frame_->pict_type = AV_PICTURE_TYPE_I;
    }
    return C2_OK;
  }

  c2_status_t DownloadDecodedFrame(AVFrame **output) {
    if (frame_->format != AV_PIX_FMT_VAAPI) {
      *output = frame_;
      return C2_OK;
    }
    av_frame_unref(software_frame_);
    const int result = av_hwframe_transfer_data(software_frame_, frame_, 0);
    if (result < 0) {
      return Fail("downloading VA-API frame", result);
    }
    *output = software_frame_;
    return C2_OK;
  }

private:
  static AVPixelFormat SelectHardwareFormat(AVCodecContext *,
                                            const AVPixelFormat *formats) {
    for (const AVPixelFormat *format = formats; *format != AV_PIX_FMT_NONE;
         ++format) {
      if (*format == AV_PIX_FMT_VAAPI) {
        return *format;
      }
    }
    return AV_PIX_FMT_NONE;
  }

  int ConfigureEncoder(const FloralCodecInterface::EncoderSettings &settings) {
    context_->width = static_cast<int>(settings.width);
    context_->height = static_cast<int>(settings.height);
    context_->time_base = AVRational{1, 1'000'000};
    context_->framerate =
        AVRational{static_cast<int>(std::round(settings.frame_rate)), 1};
    context_->pix_fmt = AV_PIX_FMT_VAAPI;
    context_->bit_rate = settings.bitrate;
    context_->rc_max_rate = settings.bitrate;
    context_->rc_buffer_size = std::max<uint32_t>(settings.bitrate / 2, 1);
    context_->gop_size = std::max<int>(
        1, static_cast<int>(settings.frame_rate * settings.sync_interval_us /
                            1'000'000));
    context_->max_b_frames = 0;
    context_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER | AV_CODEC_FLAG_LOW_DELAY;

    frames_context_ = av_hwframe_ctx_alloc(device_context_);
    if (frames_context_ == nullptr) {
      return AVERROR(ENOMEM);
    }
    auto *frames = reinterpret_cast<AVHWFramesContext *>(frames_context_->data);
    frames->format = AV_PIX_FMT_VAAPI;
    frames->sw_format = AV_PIX_FMT_NV12;
    frames->width = context_->width;
    frames->height = context_->height;
    frames->initial_pool_size = 8;
    int result = av_hwframe_ctx_init(frames_context_);
    if (result < 0) {
      return result;
    }
    context_->hw_frames_ctx = av_buffer_ref(frames_context_);
    if (context_->hw_frames_ctx == nullptr) {
      return AVERROR(ENOMEM);
    }
    // A depth of one keeps screen capture latency predictable across drivers.
    if (context_->priv_data != nullptr) {
      (void)av_opt_set_int(context_->priv_data, "async_depth", 1, 0);
    }
    return 0;
  }

  static int CopyGraphicViewToNv12(const C2GraphicView &view,
                                   AVFrame *destination) {
    if (view.width() < static_cast<uint32_t>(destination->width) ||
        view.height() < static_cast<uint32_t>(destination->height)) {
      return -1;
    }
    const C2PlanarLayout &layout = view.layout();
    const uint32_t width = static_cast<uint32_t>(destination->width);
    const uint32_t height = static_cast<uint32_t>(destination->height);
    if (layout.type == C2PlanarLayout::TYPE_YUV) {
      for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
          destination->data[0][y * destination->linesize[0] + x] =
              view.data()[C2PlanarLayout::PLANE_Y]
                         [y * layout.planes[C2PlanarLayout::PLANE_Y].rowInc +
                          x * layout.planes[C2PlanarLayout::PLANE_Y].colInc];
        }
      }
      for (uint32_t y = 0; y < height / 2; ++y) {
        for (uint32_t x = 0; x < width / 2; ++x) {
          destination->data[1][y * destination->linesize[1] + x * 2] =
              view.data()[C2PlanarLayout::PLANE_U]
                         [y * layout.planes[C2PlanarLayout::PLANE_U].rowInc +
                          x * layout.planes[C2PlanarLayout::PLANE_U].colInc];
          destination->data[1][y * destination->linesize[1] + x * 2 + 1] =
              view.data()[C2PlanarLayout::PLANE_V]
                         [y * layout.planes[C2PlanarLayout::PLANE_V].rowInc +
                          x * layout.planes[C2PlanarLayout::PLANE_V].colInc];
        }
      }
      return 0;
    }
    if (layout.type != C2PlanarLayout::TYPE_RGB &&
        layout.type != C2PlanarLayout::TYPE_RGBA) {
      return -1;
    }

    const auto sample = [&view, &layout](uint32_t plane, uint32_t x,
                                         uint32_t y) {
      return view.data()[plane][y * layout.planes[plane].rowInc +
                                x * layout.planes[plane].colInc];
    };
    for (uint32_t y = 0; y < height; ++y) {
      for (uint32_t x = 0; x < width; ++x) {
        const int r = sample(C2PlanarLayout::PLANE_R, x, y);
        const int g = sample(C2PlanarLayout::PLANE_G, x, y);
        const int b = sample(C2PlanarLayout::PLANE_B, x, y);
        destination->data[0][y * destination->linesize[0] + x] =
            ClampByte(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
      }
    }
    for (uint32_t y = 0; y < height; y += 2) {
      for (uint32_t x = 0; x < width; x += 2) {
        int r = 0;
        int g = 0;
        int b = 0;
        for (uint32_t dy = 0; dy < 2; ++dy) {
          for (uint32_t dx = 0; dx < 2; ++dx) {
            r += sample(C2PlanarLayout::PLANE_R, x + dx, y + dy);
            g += sample(C2PlanarLayout::PLANE_G, x + dx, y + dy);
            b += sample(C2PlanarLayout::PLANE_B, x + dx, y + dy);
          }
        }
        r /= 4;
        g /= 4;
        b /= 4;
        uint8_t *uv =
            destination->data[1] + (y / 2) * destination->linesize[1] + x;
        uv[0] = ClampByte(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
        uv[1] = ClampByte(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
      }
    }
    return 0;
  }

  c2_status_t Fail(const char *operation, int error) const {
    ALOGE("%s for %s failed: %s", operation, spec_.component_name,
          AvError(error).c_str());
    return C2_CORRUPTED;
  }

  CodecSpec spec_;
  std::string device_path_;
  AVBufferRef *device_context_ = nullptr;
  AVBufferRef *frames_context_ = nullptr;
  AVCodecContext *context_ = nullptr;
  AVFrame *frame_ = nullptr;
  AVFrame *software_frame_ = nullptr;
  AVPacket *packet_ = nullptr;
  bool config_sent_ = false;
};

class FloralCodecComponent : public android::SimpleC2Component {
public:
  FloralCodecComponent(const CodecSpec &spec, std::string devicePath,
                       c2_node_id_t id,
                       const std::shared_ptr<FloralCodecInterface> &interface)
      : android::SimpleC2Component(
            std::make_shared<android::SimpleInterface<FloralCodecInterface>>(
                spec.component_name, id, interface)),
        spec_(spec), interface_(interface),
        session_(spec, std::move(devicePath)) {}

  ~FloralCodecComponent() override { onRelease(); }

  c2_status_t onInit() override {
    signalled_error_ = false;
    signalled_eos_ = false;
    if (spec_.direction == CodecDirection::kEncode) {
      const FloralCodecInterface::EncoderSettings settings =
          interface_->GetEncoderSettings();
      return session_.Open(&settings);
    }
    return session_.Open(nullptr);
  }

  c2_status_t onStop() override {
    session_.Close();
    signalled_error_ = false;
    signalled_eos_ = false;
    return C2_OK;
  }

  void onReset() override { (void)onStop(); }

  void onRelease() override { session_.Close(); }

  c2_status_t onFlush_sm() override {
    session_.Flush();
    signalled_error_ = false;
    signalled_eos_ = false;
    return C2_OK;
  }

  void process(const std::unique_ptr<C2Work> &work,
               const std::shared_ptr<C2BlockPool> &pool) override {
    work->result = C2_OK;
    work->workletsProcessed = 0u;
    work->worklets.front()->output.flags = work->input.flags;
    work->worklets.front()->output.buffers.clear();
    work->worklets.front()->output.ordinal = work->input.ordinal;

    if (signalled_error_ || signalled_eos_) {
      work->result = C2_BAD_VALUE;
      work->workletsProcessed = 1u;
      return;
    }

    const bool eos = (work->input.flags & C2FrameData::FLAG_END_OF_STREAM) != 0;
    c2_status_t result = spec_.direction == CodecDirection::kEncode
                             ? ProcessEncoder(work, pool, eos)
                             : ProcessDecoder(work, pool, eos);
    if (result != C2_OK) {
      signalled_error_ = true;
      work->result = result;
      work->workletsProcessed = 1u;
    }
  }

  c2_status_t drain(uint32_t drainMode,
                    const std::shared_ptr<C2BlockPool> &pool) override {
    if (drainMode == NO_DRAIN) {
      return C2_OK;
    }
    if (drainMode == DRAIN_CHAIN) {
      return C2_OMITTED;
    }
    if (spec_.direction == CodecDirection::kEncode) {
      if (drainMode == DRAIN_COMPONENT_WITH_EOS) {
        int sendResult = avcodec_send_frame(session_.context(), nullptr);
        if (sendResult == AVERROR(EAGAIN)) {
          c2_status_t result = DrainEncoder(nullptr, pool, false);
          if (result != C2_OK) {
            return result;
          }
          sendResult = avcodec_send_frame(session_.context(), nullptr);
        }
        if (sendResult < 0 && sendResult != AVERROR_EOF) {
          return C2_CORRUPTED;
        }
        c2_status_t result = DrainEncoder(nullptr, pool, true);
        session_.Flush();
        return result;
      }
      return DrainEncoder(nullptr, pool, false);
    }
    if (drainMode == DRAIN_COMPONENT_WITH_EOS) {
      int sendResult = avcodec_send_packet(session_.context(), nullptr);
      if (sendResult == AVERROR(EAGAIN)) {
        c2_status_t result = DrainDecoder(nullptr, pool, false);
        if (result != C2_OK) {
          return result;
        }
        sendResult = avcodec_send_packet(session_.context(), nullptr);
      }
      if (sendResult < 0 && sendResult != AVERROR_EOF) {
        return C2_CORRUPTED;
      }
      c2_status_t result = DrainDecoder(nullptr, pool, true);
      session_.Flush();
      return result;
    }
    return DrainDecoder(nullptr, pool, false);
  }

private:
  struct PendingOutput {
    uint64_t frame_index = 0;
    std::shared_ptr<C2Buffer> buffer;
    std::vector<std::unique_ptr<C2Param>> updates;
  };

  c2_status_t ProcessEncoder(const std::unique_ptr<C2Work> &work,
                             const std::shared_ptr<C2BlockPool> &pool,
                             bool eos) {
    if (!work->input.buffers.empty()) {
      const std::shared_ptr<C2Buffer> &input = work->input.buffers.front();
      if (input->data().type() != C2BufferData::GRAPHIC ||
          input->data().graphicBlocks().empty()) {
        return C2_BAD_VALUE;
      }
      const C2GraphicView view =
          input->data().graphicBlocks().front().map().get();
      if (view.error() != C2_OK) {
        return view.error();
      }

      const FloralCodecInterface::EncoderSettings settings =
          interface_->GetEncoderSettings();
      c2_status_t result = session_.PrepareEncoderFrame(
          view, work->input.ordinal.frameIndex.peekull(),
          settings.request_sync);
      if (result != C2_OK) {
        return result;
      }
      if (settings.request_sync) {
        interface_->ClearSyncRequest();
      }
      const int sendResult =
          avcodec_send_frame(session_.context(), session_.frame());
      if (sendResult < 0) {
        ALOGE("avcodec_send_frame failed: %s", AvError(sendResult).c_str());
        return C2_CORRUPTED;
      }
      if (!eos) {
        result = DrainEncoder(work.get(), pool, false);
        if (result != C2_OK) {
          return result;
        }
      }
    }

    if (eos) {
      int sendResult = avcodec_send_frame(session_.context(), nullptr);
      if (sendResult == AVERROR(EAGAIN)) {
        c2_status_t result = DrainEncoder(work.get(), pool, false);
        if (result != C2_OK) {
          return result;
        }
        sendResult = avcodec_send_frame(session_.context(), nullptr);
      }
      if (sendResult < 0 && sendResult != AVERROR_EOF) {
        return C2_CORRUPTED;
      }
      c2_status_t result = DrainEncoder(work.get(), pool, true);
      if (result != C2_OK) {
        return result;
      }
      if (work->workletsProcessed != 0u) {
        work->worklets.front()->output.flags =
            C2FrameData::flags_t(C2FrameData::FLAG_END_OF_STREAM);
      } else {
        FinishEmptyWork(work.get(), true);
      }
      signalled_eos_ = true;
    } else if (work->input.buffers.empty()) {
      FinishEmptyWork(work.get(), false);
    }
    return C2_OK;
  }

  c2_status_t DrainEncoder(C2Work *currentWork,
                           const std::shared_ptr<C2BlockPool> &pool,
                           bool draining) {
    PendingOutput pending;
    bool hasPending = false;
    while (true) {
      AVPacket *packet = session_.packet();
      av_packet_unref(packet);
      const int receiveResult =
          avcodec_receive_packet(session_.context(), packet);
      if (receiveResult == AVERROR(EAGAIN) || receiveResult == AVERROR_EOF) {
        break;
      }
      if (receiveResult < 0) {
        ALOGE("avcodec_receive_packet failed: %s",
              AvError(receiveResult).c_str());
        return C2_CORRUPTED;
      }

      std::shared_ptr<C2LinearBlock> block;
      const C2MemoryUsage usage = {C2MemoryUsage::CPU_READ,
                                   C2MemoryUsage::CPU_WRITE};
      c2_status_t result = pool->fetchLinearBlock(packet->size, usage, &block);
      if (result != C2_OK) {
        return result;
      }
      C2WriteView view = block->map().get();
      if (view.error() != C2_OK) {
        return view.error();
      }
      std::memcpy(view.data(), packet->data, packet->size);
      std::shared_ptr<C2Buffer> buffer = C2Buffer::CreateLinearBuffer(
          block->share(0, packet->size, C2Fence()));
      if ((packet->flags & AV_PKT_FLAG_KEY) != 0) {
        buffer->setInfo(std::make_shared<C2StreamPictureTypeMaskInfo::output>(
            0u, C2Config::SYNC_FRAME));
      }

      std::vector<std::unique_ptr<C2Param>> configUpdates;
      if (!session_.configSent() && session_.context()->extradata != nullptr &&
          session_.context()->extradata_size > 0) {
        std::unique_ptr<C2StreamInitDataInfo::output> config =
            C2StreamInitDataInfo::output::AllocUnique(
                session_.context()->extradata_size, 0u);
        if (config == nullptr) {
          return C2_NO_MEMORY;
        }
        std::memcpy(config->m.value, session_.context()->extradata,
                    session_.context()->extradata_size);
        configUpdates.push_back(std::move(config));
        session_.markConfigSent();
      }
      const uint64_t frameIndex =
          packet->pts == AV_NOPTS_VALUE
              ? (currentWork == nullptr
                     ? 0
                     : currentWork->input.ordinal.frameIndex.peekull())
              : static_cast<uint64_t>(packet->pts);
      if (hasPending) {
        FinishOutput(pending.frame_index, currentWork, pending.buffer,
                     std::move(pending.updates), C2FrameData::flags_t(0));
      }
      pending.frame_index = frameIndex;
      pending.buffer = std::move(buffer);
      pending.updates = std::move(configUpdates);
      hasPending = true;
    }
    if (hasPending) {
      FinishOutput(pending.frame_index, currentWork, pending.buffer,
                   std::move(pending.updates),
                   draining ? C2FrameData::FLAG_END_OF_STREAM
                            : C2FrameData::flags_t(0));
    }
    return C2_OK;
  }

  c2_status_t ProcessDecoder(const std::unique_ptr<C2Work> &work,
                             const std::shared_ptr<C2BlockPool> &pool,
                             bool eos) {
    if (!work->input.buffers.empty()) {
      const std::shared_ptr<C2Buffer> &input = work->input.buffers.front();
      if (input->data().type() != C2BufferData::LINEAR ||
          input->data().linearBlocks().empty()) {
        return C2_BAD_VALUE;
      }
      const C2ReadView view = input->data().linearBlocks().front().map().get();
      if (view.error() != C2_OK) {
        return view.error();
      }
      AVPacket *packet = session_.packet();
      av_packet_unref(packet);
      const int allocationResult = av_new_packet(packet, view.capacity());
      if (allocationResult < 0) {
        return C2_NO_MEMORY;
      }
      std::memcpy(packet->data, view.data(), view.capacity());
      packet->pts =
          static_cast<int64_t>(work->input.ordinal.frameIndex.peekull());
      packet->dts = packet->pts;
      const int sendResult = avcodec_send_packet(session_.context(), packet);
      if (sendResult < 0) {
        ALOGE("avcodec_send_packet failed: %s", AvError(sendResult).c_str());
        return C2_CORRUPTED;
      }
      if (!eos) {
        c2_status_t result = DrainDecoder(work.get(), pool, false);
        if (result != C2_OK) {
          return result;
        }
      }
    }

    if (eos) {
      int sendResult = avcodec_send_packet(session_.context(), nullptr);
      if (sendResult == AVERROR(EAGAIN)) {
        c2_status_t result = DrainDecoder(work.get(), pool, false);
        if (result != C2_OK) {
          return result;
        }
        sendResult = avcodec_send_packet(session_.context(), nullptr);
      }
      if (sendResult < 0 && sendResult != AVERROR_EOF) {
        return C2_CORRUPTED;
      }
      c2_status_t result = DrainDecoder(work.get(), pool, true);
      if (result != C2_OK) {
        return result;
      }
      if (work->workletsProcessed != 0u) {
        work->worklets.front()->output.flags =
            C2FrameData::flags_t(C2FrameData::FLAG_END_OF_STREAM);
      } else {
        FinishEmptyWork(work.get(), true);
      }
      signalled_eos_ = true;
    } else if (work->input.buffers.empty()) {
      FinishEmptyWork(work.get(), false);
    }
    return C2_OK;
  }

  c2_status_t DrainDecoder(C2Work *currentWork,
                           const std::shared_ptr<C2BlockPool> &pool,
                           bool draining) {
    PendingOutput pending;
    bool hasPending = false;
    while (true) {
      AVFrame *frame = session_.frame();
      av_frame_unref(frame);
      const int receiveResult =
          avcodec_receive_frame(session_.context(), frame);
      if (receiveResult == AVERROR(EAGAIN) || receiveResult == AVERROR_EOF) {
        return C2_OK;
      }
      if (receiveResult < 0) {
        ALOGE("avcodec_receive_frame failed: %s",
              AvError(receiveResult).c_str());
        return C2_CORRUPTED;
      }

      AVFrame *outputFrame = nullptr;
      c2_status_t result = session_.DownloadDecodedFrame(&outputFrame);
      if (result != C2_OK) {
        return result;
      }
      if (outputFrame->format != AV_PIX_FMT_NV12 &&
          outputFrame->format != AV_PIX_FMT_YUV420P) {
        ALOGE("unsupported decoded pixel format %d", outputFrame->format);
        return C2_BAD_VALUE;
      }

      std::shared_ptr<C2GraphicBlock> block;
      const C2MemoryUsage usage = {C2MemoryUsage::CPU_READ,
                                   C2MemoryUsage::CPU_WRITE};
      result = pool->fetchGraphicBlock(outputFrame->width, outputFrame->height,
                                       HAL_PIXEL_FORMAT_YCBCR_420_888, usage,
                                       &block);
      if (result != C2_OK) {
        return result;
      }
      C2GraphicView view = block->map().get();
      if (view.error() != C2_OK) {
        return view.error();
      }
      result = CopyFrameToGraphicView(outputFrame, &view);
      if (result != C2_OK) {
        return result;
      }

      std::vector<std::unique_ptr<C2Param>> updates;
      result = interface_->UpdateOutputSize(outputFrame->width,
                                            outputFrame->height, &updates);
      if (result != C2_OK) {
        return result;
      }
      std::shared_ptr<C2Buffer> buffer =
          C2Buffer::CreateGraphicBuffer(block->share(
              C2Rect(outputFrame->width, outputFrame->height), C2Fence()));
      const int64_t timestamp =
          outputFrame->best_effort_timestamp != AV_NOPTS_VALUE
              ? outputFrame->best_effort_timestamp
              : outputFrame->pts;
      const uint64_t frameIndex =
          timestamp == AV_NOPTS_VALUE
              ? (currentWork == nullptr
                     ? 0
                     : currentWork->input.ordinal.frameIndex.peekull())
              : static_cast<uint64_t>(timestamp);
      if (hasPending) {
        FinishOutput(pending.frame_index, currentWork, pending.buffer,
                     std::move(pending.updates), C2FrameData::flags_t(0));
      }
      pending.frame_index = frameIndex;
      pending.buffer = std::move(buffer);
      pending.updates = std::move(updates);
      hasPending = true;
    }
    if (hasPending) {
      FinishOutput(pending.frame_index, currentWork, pending.buffer,
                   std::move(pending.updates),
                   draining ? C2FrameData::FLAG_END_OF_STREAM
                            : C2FrameData::flags_t(0));
    }
    return C2_OK;
  }

  static c2_status_t CopyFrameToGraphicView(const AVFrame *frame,
                                            C2GraphicView *view) {
    const C2PlanarLayout &layout = view->layout();
    if (layout.type != C2PlanarLayout::TYPE_YUV ||
        view->width() < static_cast<uint32_t>(frame->width) ||
        view->height() < static_cast<uint32_t>(frame->height)) {
      return C2_BAD_VALUE;
    }
    for (int y = 0; y < frame->height; ++y) {
      for (int x = 0; x < frame->width; ++x) {
        view->data()[C2PlanarLayout::PLANE_Y]
                    [y * layout.planes[C2PlanarLayout::PLANE_Y].rowInc +
                     x * layout.planes[C2PlanarLayout::PLANE_Y].colInc] =
            frame->data[0][y * frame->linesize[0] + x];
      }
    }
    for (int y = 0; y < frame->height / 2; ++y) {
      for (int x = 0; x < frame->width / 2; ++x) {
        const uint8_t u = frame->format == AV_PIX_FMT_NV12
                              ? frame->data[1][y * frame->linesize[1] + x * 2]
                              : frame->data[1][y * frame->linesize[1] + x];
        const uint8_t v =
            frame->format == AV_PIX_FMT_NV12
                ? frame->data[1][y * frame->linesize[1] + x * 2 + 1]
                : frame->data[2][y * frame->linesize[2] + x];
        view->data()[C2PlanarLayout::PLANE_U]
                    [y * layout.planes[C2PlanarLayout::PLANE_U].rowInc +
                     x * layout.planes[C2PlanarLayout::PLANE_U].colInc] = u;
        view->data()[C2PlanarLayout::PLANE_V]
                    [y * layout.planes[C2PlanarLayout::PLANE_V].rowInc +
                     x * layout.planes[C2PlanarLayout::PLANE_V].colInc] = v;
      }
    }
    return C2_OK;
  }

  void FinishOutput(uint64_t frameIndex, C2Work *currentWork,
                    const std::shared_ptr<C2Buffer> &buffer,
                    std::vector<std::unique_ptr<C2Param>> updates,
                    C2FrameData::flags_t flags) {
    if (currentWork != nullptr &&
        currentWork->input.ordinal.frameIndex.peekull() == frameIndex) {
      // The caller owns currentWork, so mirror the SimpleC2 finish callback
      // manually.
      currentWork->worklets.front()->output.flags = flags;
      currentWork->worklets.front()->output.buffers.clear();
      currentWork->worklets.front()->output.buffers.push_back(buffer);
      currentWork->worklets.front()->output.configUpdate = std::move(updates);
      currentWork->worklets.front()->output.ordinal =
          currentWork->input.ordinal;
      currentWork->workletsProcessed = 1u;
    } else {
      auto updatesHolder =
          std::make_shared<std::vector<std::unique_ptr<C2Param>>>(
              std::move(updates));
      auto fill = [buffer, flags, updatesHolder](
                      const std::unique_ptr<C2Work> &target) mutable {
        target->worklets.front()->output.flags = flags;
        target->worklets.front()->output.buffers.clear();
        target->worklets.front()->output.buffers.push_back(buffer);
        target->worklets.front()->output.configUpdate =
            std::move(*updatesHolder);
        target->worklets.front()->output.ordinal = target->input.ordinal;
        target->workletsProcessed = 1u;
      };
      finish(frameIndex, std::move(fill));
    }
  }

  static void FinishEmptyWork(C2Work *work, bool eos) {
    if (work == nullptr || work->workletsProcessed != 0u) {
      return;
    }
    work->worklets.front()->output.flags =
        eos ? C2FrameData::FLAG_END_OF_STREAM : C2FrameData::flags_t(0);
    work->worklets.front()->output.buffers.clear();
    work->worklets.front()->output.ordinal = work->input.ordinal;
    work->workletsProcessed = 1u;
  }

  CodecSpec spec_;
  std::shared_ptr<FloralCodecInterface> interface_;
  FfmpegSession session_;
  bool signalled_error_ = false;
  bool signalled_eos_ = false;
};

} // namespace

c2_status_t
CreateFloralCodecComponent(const CodecSpec &spec, const std::string &devicePath,
                           const std::shared_ptr<C2ReflectorHelper> &reflector,
                           c2_node_id_t id,
                           std::shared_ptr<C2Component> *component,
                           std::function<void(C2Component *)> deleter) {
  if (component == nullptr) {
    return C2_BAD_VALUE;
  }
  auto interface = std::make_shared<FloralCodecInterface>(reflector, spec);
  *component = std::shared_ptr<C2Component>(
      new FloralCodecComponent(spec, devicePath, id, interface),
      std::move(deleter));
  return C2_OK;
}

c2_status_t CreateFloralCodecInterface(
    const CodecSpec &spec, const std::shared_ptr<C2ReflectorHelper> &reflector,
    c2_node_id_t id, std::shared_ptr<C2ComponentInterface> *interface,
    std::function<void(C2ComponentInterface *)> deleter) {
  if (interface == nullptr) {
    return C2_BAD_VALUE;
  }
  auto implementation = std::make_shared<FloralCodecInterface>(reflector, spec);
  *interface = std::shared_ptr<C2ComponentInterface>(
      new android::SimpleInterface<FloralCodecInterface>(spec.component_name,
                                                         id, implementation),
      std::move(deleter));
  return C2_OK;
}

} // namespace floral::codec
