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

#define LOG_TAG "FloralCodec2Vpp"

#include "floral/codec/VaapiFrameConverter.h"

#include "floral/codec/MinigbmBuffer.h"

#include <cutils/native_handle.h>
#include <C2AllocatorGralloc.h>
#include <log/log.h>
#include <va/va.h>
#include <va/va_drmcommon.h>
#include <va/va_vpp.h>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
}

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <memory>
#include <utility>
#include <vector>

namespace floral::codec {
namespace {

constexpr size_t kMaxImportedSurfaceCount = 32;

struct NativeHandleDeleter {
  void operator()(native_handle_t *handle) const {
    // UnwrapNativeCodec2GrallocHandle returns a non-owning handle. Its fds
    // remain owned by the C2 block and must not be closed here.
    if (handle != nullptr) {
      native_handle_delete(handle);
    }
  }
};

using NativeHandle = std::unique_ptr<native_handle_t, NativeHandleDeleter>;

} // namespace

class VaapiFrameConverter::Impl {
public:
  ~Impl() { Reset(); }

  bool Initialize(AVBufferRef *deviceContext, uint32_t width,
                  uint32_t height) {
    Reset();
    if (deviceContext == nullptr || width == 0 || height == 0) {
      return false;
    }

    auto *device = reinterpret_cast<AVHWDeviceContext *>(deviceContext->data);
    if (device == nullptr || device->type != AV_HWDEVICE_TYPE_VAAPI ||
        device->hwctx == nullptr) {
      return false;
    }
    auto *vaapi = reinterpret_cast<AVVAAPIDeviceContext *>(device->hwctx);
    if (vaapi->display == nullptr) {
      return false;
    }

    display_ = vaapi->display;
    width_ = width;
    height_ = height;
    VAStatus status = vaCreateConfig(display_, VAProfileNone,
                                     VAEntrypointVideoProc, nullptr, 0,
                                     &vpp_config_);
    if (status != VA_STATUS_SUCCESS) {
      ALOGW("VAAPI VideoProc config is unavailable: %s", vaErrorStr(status));
      Reset();
      return false;
    }
    status = vaCreateContext(display_, vpp_config_, width_, height_,
                             VA_PROGRESSIVE, nullptr, 0, &vpp_context_);
    if (status != VA_STATUS_SUCCESS) {
      ALOGW("creating VAAPI VideoProc context failed: %s", vaErrorStr(status));
      Reset();
      return false;
    }
    return true;
  }

  Result Convert(const C2ConstGraphicBlock &block, AVFrame *destination) {
    if (display_ == nullptr || destination == nullptr ||
        destination->format != AV_PIX_FMT_VAAPI) {
      return Result::kUnsupported;
    }

    NativeHandle handle(
        android::UnwrapNativeCodec2GrallocHandle(block.handle()));
    if (handle == nullptr) {
      return Result::kUnsupported;
    }

    VADRMPRIMESurfaceDescriptor descriptor{};
    uint32_t vaFourcc = 0;
    uint64_t bufferId = 0;
    if (!GetMinigbmVaDescriptor(handle.get(), width_, height_, &descriptor,
                                &vaFourcc, &bufferId)) {
      return Result::kUnsupported;
    }

    VASurfaceID inputSurface = FindSurface(bufferId);
    if (inputSurface == VA_INVALID_SURFACE) {
      inputSurface = ImportSurface(descriptor, vaFourcc);
      if (inputSurface == VA_INVALID_SURFACE) {
        return Result::kUnsupported;
      }
      CacheSurface(bufferId, inputSurface);
    }

    const VASurfaceID outputSurface = static_cast<VASurfaceID>(
        reinterpret_cast<uintptr_t>(destination->data[3]));
    return ConvertSurface(inputSurface, outputSurface);
  }

  void Reset() {
    if (display_ != nullptr) {
      for (const auto &entry : surfaces_) {
        VASurfaceID surface = entry.surface;
        if (surface != VA_INVALID_SURFACE) {
          (void)vaDestroySurfaces(display_, &surface, 1);
        }
      }
      if (vpp_context_ != VA_INVALID_ID) {
        (void)vaDestroyContext(display_, vpp_context_);
      }
      if (vpp_config_ != VA_INVALID_ID) {
        (void)vaDestroyConfig(display_, vpp_config_);
      }
    }
    surfaces_.clear();
    display_ = nullptr;
    vpp_config_ = VA_INVALID_ID;
    vpp_context_ = VA_INVALID_ID;
    width_ = 0;
    height_ = 0;
    import_error_logged_ = false;
    conversion_error_logged_ = false;
  }

private:
  struct SurfaceEntry {
    uint64_t buffer_id;
    VASurfaceID surface;
  };

  VASurfaceID FindSurface(uint64_t bufferId) const {
    const auto found = std::find_if(
        surfaces_.begin(), surfaces_.end(),
        [bufferId](const SurfaceEntry &entry) {
          return entry.buffer_id == bufferId;
        });
    return found == surfaces_.end() ? VA_INVALID_SURFACE : found->surface;
  }

  void CacheSurface(uint64_t bufferId, VASurfaceID surface) {
    if (surfaces_.size() >= kMaxImportedSurfaceCount) {
      VASurfaceID evicted = surfaces_.front().surface;
      if (evicted != VA_INVALID_SURFACE) {
        (void)vaDestroySurfaces(display_, &evicted, 1);
      }
      surfaces_.erase(surfaces_.begin());
    }
    surfaces_.push_back({bufferId, surface});
  }

  VASurfaceID ImportSurface(const VADRMPRIMESurfaceDescriptor &descriptor,
                            uint32_t vaFourcc) const {
    VASurfaceAttrib attributes[3]{};
    attributes[0].type = VASurfaceAttribMemoryType;
    attributes[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attributes[0].value.type = VAGenericValueTypeInteger;
    attributes[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
    attributes[1].type = VASurfaceAttribExternalBufferDescriptor;
    attributes[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attributes[1].value.type = VAGenericValueTypePointer;
    attributes[1].value.value.p =
        const_cast<void *>(static_cast<const void *>(&descriptor));
    attributes[2].type = VASurfaceAttribPixelFormat;
    attributes[2].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attributes[2].value.type = VAGenericValueTypeInteger;
    attributes[2].value.value.i = vaFourcc;

    VASurfaceID surface = VA_INVALID_SURFACE;
    const VAStatus status =
        vaCreateSurfaces(display_, VA_RT_FORMAT_RGB32, width_, height_, &surface,
                         1, attributes, std::size(attributes));
    if (status != VA_STATUS_SUCCESS) {
      if (!import_error_logged_) {
        ALOGW("importing DRM PRIME input surface failed: %s",
              vaErrorStr(status));
        import_error_logged_ = true;
      }
      return VA_INVALID_SURFACE;
    }
    return surface;
  }

  Result ConvertSurface(VASurfaceID inputSurface,
                        VASurfaceID outputSurface) const {
    VARectangle inputRegion{};
    inputRegion.width = static_cast<uint16_t>(width_);
    inputRegion.height = static_cast<uint16_t>(height_);
    VARectangle outputRegion = inputRegion;

    VAProcPipelineParameterBuffer parameters{};
    parameters.surface = inputSurface;
    parameters.surface_region = &inputRegion;
    parameters.surface_color_standard = VAProcColorStandardSRGB;
    parameters.output_region = &outputRegion;
    parameters.output_background_color = 0xff000000;
    parameters.output_color_standard = VAProcColorStandardBT709;
    parameters.filter_flags = VA_FRAME_PICTURE;

    VABufferID parameterBuffer = VA_INVALID_ID;
    VAStatus status = vaCreateBuffer(
        display_, vpp_context_, VAProcPipelineParameterBufferType,
        sizeof(parameters), 1, &parameters, &parameterBuffer);
    if (status != VA_STATUS_SUCCESS) {
      if (!conversion_error_logged_) {
        ALOGE("creating VAAPI VideoProc parameter buffer failed: %s",
              vaErrorStr(status));
        conversion_error_logged_ = true;
      }
      return Result::kError;
    }

    bool pictureBegun = false;
    status = vaBeginPicture(display_, vpp_context_, outputSurface);
    if (status == VA_STATUS_SUCCESS) {
      pictureBegun = true;
      status = vaRenderPicture(display_, vpp_context_, &parameterBuffer, 1);
    }
    if (pictureBegun) {
      const VAStatus endStatus = vaEndPicture(display_, vpp_context_);
      if (status == VA_STATUS_SUCCESS) {
        status = endStatus;
      }
    }
    (void)vaDestroyBuffer(display_, parameterBuffer);
    if (status != VA_STATUS_SUCCESS) {
      if (!conversion_error_logged_) {
        ALOGE("VAAPI RGB to NV12 conversion failed: %s", vaErrorStr(status));
        conversion_error_logged_ = true;
      }
      return Result::kError;
    }

    // vaEndPicture is non-blocking. Complete the conversion before the input
    // buffer can return to its producer and before the encoder reads output.
    status = vaSyncSurface(display_, outputSurface);
    if (status != VA_STATUS_SUCCESS) {
      if (!conversion_error_logged_) {
        ALOGE("waiting for VAAPI VideoProc output failed: %s",
              vaErrorStr(status));
        conversion_error_logged_ = true;
      }
      return Result::kError;
    }
    return Result::kConverted;
  }

  VADisplay display_ = nullptr;
  VAConfigID vpp_config_ = VA_INVALID_ID;
  VAContextID vpp_context_ = VA_INVALID_ID;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  mutable bool import_error_logged_ = false;
  mutable bool conversion_error_logged_ = false;
  std::vector<SurfaceEntry> surfaces_;
};

VaapiFrameConverter::VaapiFrameConverter() : impl_(std::make_unique<Impl>()) {}

VaapiFrameConverter::~VaapiFrameConverter() = default;

bool VaapiFrameConverter::Initialize(AVBufferRef *deviceContext, uint32_t width,
                                     uint32_t height) {
  return impl_->Initialize(deviceContext, width, height);
}

VaapiFrameConverter::Result
VaapiFrameConverter::Convert(const C2ConstGraphicBlock &block,
                             AVFrame *destination) {
  return impl_->Convert(block, destination);
}

void VaapiFrameConverter::Reset() { impl_->Reset(); }

} // namespace floral::codec
