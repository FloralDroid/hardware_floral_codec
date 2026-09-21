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

#define LOG_TAG "FloralV4l2Codec"

#include "floral/codec/V4l2CodecSession.h"

#include "floral/codec/MinigbmDmaBuf.h"
#include "floral/codec/VulkanFrameConverter.h"

#include <C2AllocatorGralloc.h>
#include <C2PlatformSupport.h>
#include <android-base/unique_fd.h>
#include <cutils/native_handle.h>
#include <drm_fourcc.h>
#include <hardware/gralloc.h>
#include <hardware/gralloc1.h>
#include <linux/videodev2.h>
#include <log/log.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <optional>
#include <poll.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace floral::codec {
namespace {

constexpr uint32_t kQueueBufferCount = 8;
constexpr uint32_t kDecoderCaptureSlack = 4;
constexpr uint32_t kCompressedBufferSize = 2 * 1024 * 1024;
constexpr int kBlockingTimeoutMs = 5000;
constexpr c2_nsecs_t kFenceTimeoutNs = 5'000'000'000LL;

int Ioctl(int fd, unsigned long request, void *argument) {
  int result = 0;
  do {
    result = ioctl(fd, request, argument);
  } while (result < 0 && errno == EINTR);
  return result;
}

uint32_t DeviceCapabilities(const v4l2_capability &capability) {
  return (capability.capabilities & V4L2_CAP_DEVICE_CAPS) != 0
             ? capability.device_caps
             : capability.capabilities;
}

uint32_t CompressedFormat(const CodecSpec &spec) {
  switch (spec.codec) {
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

timeval FrameIndexToTimestamp(uint64_t frameIndex) {
  timeval timestamp{};
  timestamp.tv_sec = static_cast<time_t>(frameIndex / 1'000'000);
  timestamp.tv_usec = static_cast<suseconds_t>(frameIndex % 1'000'000);
  return timestamp;
}

uint64_t TimestampToFrameIndex(const timeval &timestamp) {
  if (timestamp.tv_sec < 0 || timestamp.tv_usec < 0) {
    return 0;
  }
  return static_cast<uint64_t>(timestamp.tv_sec) * 1'000'000ULL +
         static_cast<uint64_t>(timestamp.tv_usec);
}

struct NativeHandleDeleter {
  void operator()(native_handle_t *handle) const {
    // UnwrapNativeCodec2GrallocHandle returns a non-owning handle whose fds
    // remain owned by the C2 block.
    if (handle != nullptr) {
      native_handle_delete(handle);
    }
  }
};

using NativeHandle = std::unique_ptr<native_handle_t, NativeHandleDeleter>;

struct MappedBuffer {
  MappedBuffer() = default;
  MappedBuffer(void *addressValue, size_t lengthValue)
      : address(addressValue), length(lengthValue) {}
  ~MappedBuffer() { Reset(); }

  MappedBuffer(MappedBuffer &&other) noexcept
      : address(std::exchange(other.address, nullptr)),
        length(std::exchange(other.length, 0)) {}

  MappedBuffer &operator=(MappedBuffer &&other) noexcept {
    if (this != &other) {
      Reset();
      address = std::exchange(other.address, nullptr);
      length = std::exchange(other.length, 0);
    }
    return *this;
  }

  MappedBuffer(const MappedBuffer &) = delete;
  MappedBuffer &operator=(const MappedBuffer &) = delete;

  void Reset() {
    if (address != nullptr) {
      munmap(address, length);
      address = nullptr;
      length = 0;
    }
  }

  void *address = nullptr;
  size_t length = 0;
};

bool SameDmaBufObject(int first, int second) {
  struct stat firstStat {};
  struct stat secondStat {};
  return fstat(first, &firstStat) == 0 && fstat(second, &secondStat) == 0 &&
         firstStat.st_dev == secondStat.st_dev &&
         firstStat.st_ino == secondStat.st_ino;
}

bool IsLinearNv12(const MinigbmDmaBuf &buffer) {
  return buffer.drm_format == DRM_FORMAT_NV12 &&
         buffer.modifier == DRM_FORMAT_MOD_LINEAR && buffer.plane_count == 2 &&
         buffer.planes[0].offset == 0 &&
         buffer.planes[0].stride == buffer.planes[1].stride &&
         SameDmaBufObject(buffer.planes[0].fd, buffer.planes[1].fd) &&
         buffer.total_size <= std::numeric_limits<uint32_t>::max();
}

} // namespace

struct V4l2CodecSession::Impl {
  struct EncoderInputSlot {
    std::shared_ptr<C2Buffer> owner;
    std::shared_ptr<C2GraphicBlock> converted_block;
    bool queued = false;
  };

  struct DecoderInputSlot {
    bool queued = false;
  };

  struct DecoderCaptureSlot {
    std::shared_ptr<C2GraphicBlock> block;
    bool queued = false;
  };

  Impl(CodecSpec codecSpec, std::string devicePath)
      : spec(std::move(codecSpec)), device_path(std::move(devicePath)) {}

  ~Impl() { Close(); }

  c2_status_t Error(const char *operation) const {
    ALOGE("%s for %s on %s failed: %s", operation, spec.component_name,
          device_path.c_str(), std::strerror(errno));
    return C2_CORRUPTED;
  }

  c2_status_t Open(const V4l2EncoderSettings *settings) {
    Close();
    if (device_path.empty() || CompressedFormat(spec) == 0) {
      return C2_BAD_VALUE;
    }
    fd.reset(open(device_path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC));
    if (fd.get() < 0) {
      return Error("opening V4L2 codec device");
    }

    v4l2_capability capability{};
    if (Ioctl(fd.get(), VIDIOC_QUERYCAP, &capability) < 0) {
      return Error("querying V4L2 capabilities");
    }
    const uint32_t caps = DeviceCapabilities(capability);
    if ((caps & V4L2_CAP_STREAMING) == 0 ||
        (caps & V4L2_CAP_VIDEO_M2M_MPLANE) == 0) {
      ALOGE("%s does not expose a streaming multi-planar M2M device",
            device_path.c_str());
      return C2_OMITTED;
    }

    if (spec.direction == CodecDirection::kEncode) {
      if (settings == nullptr || settings->width == 0 ||
          settings->height == 0 || settings->bitrate == 0 ||
          settings->frame_rate == 0) {
        return C2_BAD_VALUE;
      }
      encoder_settings = *settings;
      const c2_status_t result = frame_converter.Initialize();
      if (result != C2_OK) {
        Close();
        return result;
      }
      return C2_OK;
    }
    return ConfigureDecoderOutput();
  }

  void Close() {
    if (fd.get() >= 0) {
      StreamOff(V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, &output_streaming);
      StreamOff(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, &capture_streaming);
    }
    encoder_pending.clear();
    decoder_pending.clear();
    encoder_inputs.clear();
    decoder_inputs.clear();
    decoder_capture.clear();
    output_mmaps.clear();
    capture_mmaps.clear();
    frame_converter.Reset();
    fd.reset();
    encoder_configured = false;
    decoder_capture_configured = false;
    decoder_capture_setup_pending = false;
    output_streaming = false;
    capture_streaming = false;
    draining = false;
    decoder_received_input = false;
    current_bitrate = 0;
    raw_size = 0;
    raw_stride = 0;
    coded_size = 0;
    capture_width = 0;
    capture_height = 0;
    visible_width = 0;
    visible_height = 0;
  }

  c2_status_t Flush() {
    const std::optional<V4l2EncoderSettings> settings = encoder_settings;
    Close();
    return Open(settings.has_value() ? &*settings : nullptr);
  }

  void StreamOff(v4l2_buf_type type, bool *streaming) {
    if (streaming == nullptr || !*streaming) {
      return;
    }
    if (Ioctl(fd.get(), VIDIOC_STREAMOFF, &type) < 0 && errno != EINVAL) {
      ALOGW("VIDIOC_STREAMOFF type %u on %s failed: %s", type,
            device_path.c_str(), std::strerror(errno));
    }
    *streaming = false;
  }

  c2_status_t StreamOn(v4l2_buf_type type, bool *streaming) {
    if (*streaming) {
      return C2_OK;
    }
    if (Ioctl(fd.get(), VIDIOC_STREAMON, &type) < 0) {
      return Error("starting V4L2 queue");
    }
    *streaming = true;
    return C2_OK;
  }

  c2_status_t SetFormat(v4l2_buf_type type, uint32_t pixelFormat,
                        uint32_t width, uint32_t height, uint32_t stride,
                        uint32_t size, v4l2_format *result) {
    v4l2_format format{};
    format.type = type;
    format.fmt.pix_mp.width = width;
    format.fmt.pix_mp.height = height;
    format.fmt.pix_mp.pixelformat = pixelFormat;
    format.fmt.pix_mp.field = V4L2_FIELD_NONE;
    format.fmt.pix_mp.num_planes = 1;
    format.fmt.pix_mp.plane_fmt[0].bytesperline = stride;
    format.fmt.pix_mp.plane_fmt[0].sizeimage = size;
    if (Ioctl(fd.get(), VIDIOC_S_FMT, &format) < 0) {
      return Error("setting V4L2 format");
    }
    if (format.fmt.pix_mp.pixelformat != pixelFormat ||
        format.fmt.pix_mp.num_planes != 1) {
      ALOGE("%s changed requested V4L2 format or plane count",
            device_path.c_str());
      return C2_OMITTED;
    }
    if (result != nullptr) {
      *result = format;
    }
    return C2_OK;
  }

  c2_status_t RequestBuffers(v4l2_buf_type type, v4l2_memory memory,
                             uint32_t requested, uint32_t *actual) {
    v4l2_requestbuffers request{};
    request.type = type;
    request.memory = memory;
    request.count = requested;
    if (Ioctl(fd.get(), VIDIOC_REQBUFS, &request) < 0) {
      return Error("requesting V4L2 buffers");
    }
    if (requested != 0 && request.count == 0) {
      ALOGE("%s allocated no V4L2 buffers", device_path.c_str());
      return C2_NO_MEMORY;
    }
    if (actual != nullptr) {
      *actual = request.count;
    }
    return C2_OK;
  }

  c2_status_t MapBuffers(v4l2_buf_type type, uint32_t count,
                         std::vector<MappedBuffer> *buffers) {
    buffers->clear();
    buffers->reserve(count);
    for (uint32_t index = 0; index < count; ++index) {
      v4l2_plane plane{};
      v4l2_buffer buffer{};
      buffer.type = type;
      buffer.memory = V4L2_MEMORY_MMAP;
      buffer.index = index;
      buffer.length = 1;
      buffer.m.planes = &plane;
      if (Ioctl(fd.get(), VIDIOC_QUERYBUF, &buffer) < 0) {
        return Error("querying V4L2 buffer");
      }
      void *address = mmap(nullptr, plane.length, PROT_READ | PROT_WRITE,
                           MAP_SHARED, fd.get(), plane.m.mem_offset);
      if (address == MAP_FAILED) {
        return Error("mapping V4L2 buffer");
      }
      buffers->emplace_back(address, plane.length);
    }
    return C2_OK;
  }

  bool ControlSupported(uint32_t id) const {
    v4l2_queryctrl query{};
    query.id = id;
    return Ioctl(fd.get(), VIDIOC_QUERYCTRL, &query) == 0 &&
           (query.flags & V4L2_CTRL_FLAG_DISABLED) == 0;
  }

  c2_status_t SetControl(uint32_t id, int32_t value, bool required) {
    if (!ControlSupported(id)) {
      if (required) {
        ALOGE("required V4L2 control %#x is unavailable on %s", id,
              device_path.c_str());
        return C2_OMITTED;
      }
      return C2_OK;
    }
    v4l2_control control{};
    control.id = id;
    control.value = value;
    return Ioctl(fd.get(), VIDIOC_S_CTRL, &control) == 0
               ? C2_OK
               : Error("setting V4L2 control");
  }

  c2_status_t ConfigureEncoder(const MinigbmDmaBuf &input) {
    if (!IsLinearNv12(input)) {
      ALOGE("%s requires a linear single-object NV12 DMA-BUF input",
            spec.component_name);
      return C2_OMITTED;
    }
    v4l2_format rawFormat{};
    c2_status_t result =
        SetFormat(V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, V4L2_PIX_FMT_NV12,
                  encoder_settings->width, encoder_settings->height,
                  input.planes[0].stride,
                  static_cast<uint32_t>(input.total_size), &rawFormat);
    if (result != C2_OK) {
      return result;
    }
    raw_stride = rawFormat.fmt.pix_mp.plane_fmt[0].bytesperline;
    raw_size = rawFormat.fmt.pix_mp.plane_fmt[0].sizeimage;
    if (raw_stride != input.planes[0].stride || raw_size > input.total_size) {
      ALOGE("Venus NV12 layout (%u/%u) does not match minigbm (%u/%llu)",
            raw_stride, raw_size, input.planes[0].stride,
            static_cast<unsigned long long>(input.total_size));
      return C2_OMITTED;
    }

    v4l2_format codedFormat{};
    result =
        SetFormat(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, CompressedFormat(spec),
                  encoder_settings->width, encoder_settings->height, 0,
                  kCompressedBufferSize, &codedFormat);
    if (result != C2_OK) {
      return result;
    }
    coded_size = codedFormat.fmt.pix_mp.plane_fmt[0].sizeimage;

    result = SetControl(V4L2_CID_MPEG_VIDEO_BITRATE,
                        static_cast<int32_t>(encoder_settings->bitrate), true);
    if (result != C2_OK) {
      return result;
    }
    current_bitrate = encoder_settings->bitrate;
    result =
        SetControl(V4L2_CID_MPEG_VIDEO_GOP_SIZE,
                   static_cast<int32_t>(encoder_settings->gop_size), false);
    if (result != C2_OK) {
      return result;
    }
    result = SetControl(V4L2_CID_MPEG_VIDEO_B_FRAMES, 0, false);
    if (result != C2_OK) {
      return result;
    }
#ifdef V4L2_CID_MPEG_VIDEO_BITRATE_MODE
    result = SetControl(V4L2_CID_MPEG_VIDEO_BITRATE_MODE,
                        encoder_settings->constant_bitrate
                            ? V4L2_MPEG_VIDEO_BITRATE_MODE_CBR
                            : V4L2_MPEG_VIDEO_BITRATE_MODE_VBR,
                        false);
    if (result != C2_OK) {
      return result;
    }
#endif
#if defined(V4L2_CID_MPEG_VIDEO_HEADER_MODE) &&                                \
    defined(V4L2_MPEG_VIDEO_HEADER_MODE_JOINED_WITH_1ST_FRAME)
    result =
        SetControl(V4L2_CID_MPEG_VIDEO_HEADER_MODE,
                   V4L2_MPEG_VIDEO_HEADER_MODE_JOINED_WITH_1ST_FRAME, false);
    if (result != C2_OK) {
      return result;
    }
#endif

    v4l2_streamparm streamParameters{};
    streamParameters.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    streamParameters.parm.output.timeperframe.numerator = 1;
    streamParameters.parm.output.timeperframe.denominator =
        encoder_settings->frame_rate;
    if (Ioctl(fd.get(), VIDIOC_S_PARM, &streamParameters) < 0) {
      return Error("setting encoder frame rate");
    }

    uint32_t count = 0;
    result = RequestBuffers(V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
                            V4L2_MEMORY_DMABUF, kQueueBufferCount, &count);
    if (result != C2_OK) {
      return result;
    }
    encoder_inputs.resize(count);

    result = RequestBuffers(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                            V4L2_MEMORY_MMAP, kQueueBufferCount, &count);
    if (result != C2_OK) {
      return result;
    }
    result =
        MapBuffers(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, count, &capture_mmaps);
    if (result != C2_OK) {
      return result;
    }
    for (uint32_t index = 0; index < count; ++index) {
      result = QueueMmapCapture(index);
      if (result != C2_OK) {
        return result;
      }
    }
    result = StreamOn(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, &capture_streaming);
    if (result != C2_OK) {
      return result;
    }
    result = StreamOn(V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, &output_streaming);
    if (result == C2_OK) {
      encoder_configured = true;
      ALOGI("%s uses zero-copy NV12 DMA-BUF input on %s", spec.component_name,
            device_path.c_str());
    }
    return result;
  }

  c2_status_t QueueMmapCapture(uint32_t index) {
    v4l2_plane plane{};
    plane.length = static_cast<uint32_t>(capture_mmaps[index].length);
    v4l2_buffer buffer{};
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buffer.memory = V4L2_MEMORY_MMAP;
    buffer.index = index;
    buffer.length = 1;
    buffer.m.planes = &plane;
    return Ioctl(fd.get(), VIDIOC_QBUF, &buffer) == 0
               ? C2_OK
               : Error("queueing encoded capture buffer");
  }

  c2_status_t DequeueEncoderInputs() {
    while (true) {
      v4l2_plane plane{};
      v4l2_buffer buffer{};
      buffer.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
      buffer.memory = V4L2_MEMORY_DMABUF;
      buffer.length = 1;
      buffer.m.planes = &plane;
      if (Ioctl(fd.get(), VIDIOC_DQBUF, &buffer) < 0) {
        return errno == EAGAIN ? C2_OK : Error("dequeueing encoder input");
      }
      if (buffer.index >= encoder_inputs.size()) {
        return C2_CORRUPTED;
      }
      encoder_inputs[buffer.index].owner.reset();
      encoder_inputs[buffer.index].converted_block.reset();
      encoder_inputs[buffer.index].queued = false;
    }
  }

  c2_status_t PumpEncoderCapture() {
    while (true) {
      v4l2_plane plane{};
      v4l2_buffer buffer{};
      buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
      buffer.memory = V4L2_MEMORY_MMAP;
      buffer.length = 1;
      buffer.m.planes = &plane;
      if (Ioctl(fd.get(), VIDIOC_DQBUF, &buffer) < 0) {
        return errno == EAGAIN || (errno == EPIPE && draining)
                   ? C2_OK
                   : Error("dequeueing encoder output");
      }
      if (buffer.index >= capture_mmaps.size() ||
          plane.data_offset > capture_mmaps[buffer.index].length ||
          plane.bytesused >
              capture_mmaps[buffer.index].length - plane.data_offset) {
        return C2_CORRUPTED;
      }
      V4l2EncodedFrame frame;
      frame.frame_index = TimestampToFrameIndex(buffer.timestamp);
      frame.key_frame = (buffer.flags & V4L2_BUF_FLAG_KEYFRAME) != 0;
      frame.end_of_stream = (buffer.flags & V4L2_BUF_FLAG_LAST) != 0;
      const auto *begin =
          static_cast<const uint8_t *>(capture_mmaps[buffer.index].address) +
          plane.data_offset;
      frame.data.assign(begin, begin + plane.bytesused);
      if (!frame.data.empty() || frame.end_of_stream) {
        encoder_pending.push_back(std::move(frame));
      }
      if ((buffer.flags & V4L2_BUF_FLAG_LAST) == 0) {
        c2_status_t result = QueueMmapCapture(buffer.index);
        if (result != C2_OK) {
          return result;
        }
      }
    }
  }

  c2_status_t PumpEncoder() {
    c2_status_t result = DequeueEncoderInputs();
    return result == C2_OK ? PumpEncoderCapture() : result;
  }

  c2_status_t WaitForDevice(short events) {
    pollfd descriptor{fd.get(), events, 0};
    int result = 0;
    do {
      result = poll(&descriptor, 1, kBlockingTimeoutMs);
    } while (result < 0 && errno == EINTR);
    if (result == 0) {
      ALOGE("timed out waiting for %s", spec.component_name);
      return C2_TIMED_OUT;
    }
    return result > 0 ? C2_OK : Error("polling V4L2 codec");
  }

  c2_status_t QueueEncoderFrame(const std::shared_ptr<C2BlockPool> &pool,
                                const std::shared_ptr<C2Buffer> &owner,
                                uint64_t frameIndex, bool requestSync,
                                uint32_t bitrate) {
    if (owner == nullptr || owner->data().type() != C2BufferData::GRAPHIC ||
        !encoder_settings.has_value()) {
      return C2_BAD_VALUE;
    }
    const std::vector<C2ConstGraphicBlock> graphicBlocks =
        owner->data().graphicBlocks();
    if (graphicBlocks.empty()) {
      return C2_BAD_VALUE;
    }
    const C2ConstGraphicBlock &block = graphicBlocks.front();
    c2_status_t result = block.fence().wait(kFenceTimeoutNs);
    if (result != C2_OK) {
      ALOGE("waiting for encoder input fence failed: %d", result);
      return result;
    }

    NativeHandle handle(
        android::UnwrapNativeCodec2GrallocHandle(block.handle()));
    MinigbmDmaBuf dmaBuf;
    if (handle == nullptr ||
        !GetMinigbmDmaBuf(handle.get(), encoder_settings->width,
                          encoder_settings->height, &dmaBuf)) {
      ALOGE("failed to unwrap encoder input as a minigbm DMA-BUF");
      return C2_OMITTED;
    }
    std::shared_ptr<C2GraphicBlock> convertedBlock;
    if (!IsLinearNv12(dmaBuf)) {
      if (pool == nullptr) {
        return C2_NO_INIT;
      }
      // SW_READ forces minigbm to allocate a linear NV12 object. Neither the
      // source nor this destination is mapped by the CPU.
      const C2MemoryUsage usage = {
          C2MemoryUsage::CPU_READ | GRALLOC1_CONSUMER_USAGE_VIDEO_ENCODER, 0};
      result = pool->fetchGraphicBlock(
          encoder_settings->width, encoder_settings->height,
          HAL_PIXEL_FORMAT_YCBCR_420_888, usage, &convertedBlock);
      if (result != C2_OK) {
        return result;
      }
      NativeHandle convertedHandle(
          android::UnwrapNativeCodec2GrallocHandle(convertedBlock->handle()));
      if (convertedHandle == nullptr ||
          !GetMinigbmDmaBuf(convertedHandle.get(), encoder_settings->width,
                            encoder_settings->height, &dmaBuf) ||
          !IsLinearNv12(dmaBuf)) {
        ALOGE("failed to allocate a linear NV12 encoder DMA-BUF");
        return C2_OMITTED;
      }
      result = frame_converter.Convert(block, dmaBuf);
      if (result != C2_OK) {
        return result;
      }
    }
    if (!encoder_configured) {
      result = ConfigureEncoder(dmaBuf);
      if (result != C2_OK) {
        return result;
      }
    }
    if (!IsLinearNv12(dmaBuf) || dmaBuf.planes[0].stride != raw_stride ||
        dmaBuf.total_size < raw_size) {
      ALOGE("encoder input DMA-BUF layout changed after configuration");
      return C2_BAD_VALUE;
    }

    result = PumpEncoder();
    if (result != C2_OK) {
      return result;
    }
    auto freeSlot =
        std::find_if(encoder_inputs.begin(), encoder_inputs.end(),
                     [](const EncoderInputSlot &slot) { return !slot.queued; });
    if (freeSlot == encoder_inputs.end()) {
      result = WaitForDevice(POLLIN | POLLOUT);
      if (result != C2_OK) {
        return result;
      }
      result = PumpEncoder();
      if (result != C2_OK) {
        return result;
      }
      freeSlot = std::find_if(
          encoder_inputs.begin(), encoder_inputs.end(),
          [](const EncoderInputSlot &slot) { return !slot.queued; });
      if (freeSlot == encoder_inputs.end()) {
        return C2_BLOCKING;
      }
    }

    if (bitrate != current_bitrate) {
      result = SetControl(V4L2_CID_MPEG_VIDEO_BITRATE,
                          static_cast<int32_t>(bitrate), true);
      if (result != C2_OK) {
        return result;
      }
      current_bitrate = bitrate;
    }
    if (requestSync) {
      result = SetControl(V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, 1, true);
      if (result != C2_OK) {
        return result;
      }
    }

    const uint32_t index =
        static_cast<uint32_t>(freeSlot - encoder_inputs.begin());
    v4l2_plane plane{};
    plane.m.fd = dmaBuf.planes[0].fd;
    plane.length = static_cast<uint32_t>(dmaBuf.total_size);
    plane.bytesused = raw_size;
    v4l2_buffer buffer{};
    buffer.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    buffer.memory = V4L2_MEMORY_DMABUF;
    buffer.index = index;
    buffer.length = 1;
    buffer.m.planes = &plane;
    buffer.timestamp = FrameIndexToTimestamp(frameIndex);
    if (Ioctl(fd.get(), VIDIOC_QBUF, &buffer) < 0) {
      return Error("queueing zero-copy encoder input");
    }
    freeSlot->owner = owner;
    freeSlot->converted_block = std::move(convertedBlock);
    freeSlot->queued = true;
    return C2_OK;
  }

  c2_status_t DequeueEncoderFrame(V4l2EncodedFrame *output, bool wait) {
    if (output == nullptr) {
      return C2_BAD_VALUE;
    }
    if (!encoder_pending.empty()) {
      *output = std::move(encoder_pending.front());
      encoder_pending.pop_front();
      return C2_OK;
    }
    if (!encoder_configured) {
      return C2_NOT_FOUND;
    }
    c2_status_t result = PumpEncoder();
    if (result != C2_OK) {
      return result;
    }
    if (encoder_pending.empty() && wait) {
      result = WaitForDevice(POLLIN | POLLOUT);
      if (result != C2_OK) {
        return result;
      }
      result = PumpEncoder();
      if (result != C2_OK) {
        return result;
      }
    }
    if (encoder_pending.empty()) {
      return C2_NOT_FOUND;
    }
    *output = std::move(encoder_pending.front());
    encoder_pending.pop_front();
    return C2_OK;
  }

  c2_status_t StartEncoderDrain() {
    if (draining) {
      return C2_OK;
    }
    if (!encoder_configured) {
      V4l2EncodedFrame frame;
      frame.end_of_stream = true;
      encoder_pending.push_back(std::move(frame));
      draining = true;
      return C2_OK;
    }
    v4l2_encoder_cmd command{};
    command.cmd = V4L2_ENC_CMD_STOP;
    if (Ioctl(fd.get(), VIDIOC_ENCODER_CMD, &command) < 0) {
      return Error("stopping V4L2 encoder");
    }
    draining = true;
    return C2_OK;
  }

  c2_status_t ConfigureDecoderOutput() {
    v4l2_event_subscription subscription{};
    subscription.type = V4L2_EVENT_SOURCE_CHANGE;
    if (Ioctl(fd.get(), VIDIOC_SUBSCRIBE_EVENT, &subscription) < 0) {
      return Error("subscribing to decoder source changes");
    }
    v4l2_format codedFormat{};
    c2_status_t result =
        SetFormat(V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, CompressedFormat(spec),
                  1280, 720, 0, kCompressedBufferSize, &codedFormat);
    if (result != C2_OK) {
      return result;
    }
    coded_size = codedFormat.fmt.pix_mp.plane_fmt[0].sizeimage;
    uint32_t count = 0;
    result = RequestBuffers(V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, V4L2_MEMORY_MMAP,
                            kQueueBufferCount, &count);
    if (result != C2_OK) {
      return result;
    }
    result =
        MapBuffers(V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, count, &output_mmaps);
    if (result != C2_OK) {
      return result;
    }
    decoder_inputs.resize(count);
    return StreamOn(V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, &output_streaming);
  }

  c2_status_t DequeueDecoderInputs() {
    while (true) {
      v4l2_plane plane{};
      v4l2_buffer buffer{};
      buffer.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
      buffer.memory = V4L2_MEMORY_MMAP;
      buffer.length = 1;
      buffer.m.planes = &plane;
      if (Ioctl(fd.get(), VIDIOC_DQBUF, &buffer) < 0) {
        return errno == EAGAIN ? C2_OK : Error("dequeueing decoder input");
      }
      if (buffer.index >= decoder_inputs.size()) {
        return C2_CORRUPTED;
      }
      decoder_inputs[buffer.index].queued = false;
    }
  }

  c2_status_t QueueDecoderPacket(const uint8_t *data, size_t size,
                                 uint64_t frameIndex) {
    if (data == nullptr || size == 0 || size > coded_size) {
      return C2_BAD_VALUE;
    }
    c2_status_t result = DequeueDecoderInputs();
    if (result != C2_OK) {
      return result;
    }
    auto freeSlot =
        std::find_if(decoder_inputs.begin(), decoder_inputs.end(),
                     [](const DecoderInputSlot &slot) { return !slot.queued; });
    if (freeSlot == decoder_inputs.end()) {
      return C2_BLOCKING;
    }
    const uint32_t index =
        static_cast<uint32_t>(freeSlot - decoder_inputs.begin());
    if (size > output_mmaps[index].length) {
      return C2_BAD_VALUE;
    }
    std::memcpy(output_mmaps[index].address, data, size);

    v4l2_plane plane{};
    plane.length = static_cast<uint32_t>(output_mmaps[index].length);
    plane.bytesused = static_cast<uint32_t>(size);
    v4l2_buffer buffer{};
    buffer.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    buffer.memory = V4L2_MEMORY_MMAP;
    buffer.index = index;
    buffer.length = 1;
    buffer.m.planes = &plane;
    buffer.timestamp = FrameIndexToTimestamp(frameIndex);
    if (Ioctl(fd.get(), VIDIOC_QBUF, &buffer) < 0) {
      return Error("queueing decoder bitstream");
    }
    freeSlot->queued = true;
    decoder_received_input = true;
    return C2_OK;
  }

  void ResetDecoderCapture() {
    StreamOff(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, &capture_streaming);
    decoder_capture.clear();
    uint32_t ignored = 0;
    if (fd.get() >= 0 &&
        RequestBuffers(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, V4L2_MEMORY_DMABUF,
                       0, &ignored) != C2_OK) {
      ALOGW("failed to release decoder capture buffers on %s",
            device_path.c_str());
    }
    decoder_capture_configured = false;
  }

  c2_status_t BeginDecoderCaptureSetup() {
    ResetDecoderCapture();
    v4l2_format format{};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (Ioctl(fd.get(), VIDIOC_G_FMT, &format) < 0) {
      return Error("querying decoder capture format");
    }
    const uint32_t decodedWidth = format.fmt.pix_mp.width;
    const uint32_t decodedHeight = format.fmt.pix_mp.height;
    c2_status_t result =
        SetFormat(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, V4L2_PIX_FMT_NV12,
                  decodedWidth, decodedHeight, 0, 0, &format);
    if (result != C2_OK) {
      return result;
    }
    capture_width = format.fmt.pix_mp.width;
    capture_height = format.fmt.pix_mp.height;
    raw_stride = format.fmt.pix_mp.plane_fmt[0].bytesperline;
    raw_size = format.fmt.pix_mp.plane_fmt[0].sizeimage;
    visible_width = capture_width;
    visible_height = capture_height;

    v4l2_selection selection{};
    selection.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    selection.target = V4L2_SEL_TGT_COMPOSE;
    if (Ioctl(fd.get(), VIDIOC_G_SELECTION, &selection) == 0 &&
        selection.r.width > 0 && selection.r.height > 0) {
      visible_width = std::min<uint32_t>(selection.r.width, capture_width);
      visible_height = std::min<uint32_t>(selection.r.height, capture_height);
    }

    uint32_t minimum = 4;
    v4l2_control control{};
    control.id = V4L2_CID_MIN_BUFFERS_FOR_CAPTURE;
    if (Ioctl(fd.get(), VIDIOC_G_CTRL, &control) == 0 && control.value > 0) {
      minimum = static_cast<uint32_t>(control.value);
    }
    uint32_t count = 0;
    result =
        RequestBuffers(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, V4L2_MEMORY_DMABUF,
                       minimum + kDecoderCaptureSlack, &count);
    if (result != C2_OK) {
      return result;
    }
    if (count < minimum) {
      ALOGE("%s returned %u capture buffers but requires %u",
            device_path.c_str(), count, minimum);
      return C2_NO_MEMORY;
    }
    decoder_capture.resize(count);
    decoder_capture_setup_pending = true;
    return C2_OK;
  }

  c2_status_t GetDecoderDmaBuf(const std::shared_ptr<C2GraphicBlock> &block,
                               MinigbmDmaBuf *dmaBuf) const {
    NativeHandle handle(
        android::UnwrapNativeCodec2GrallocHandle(block->handle()));
    if (handle == nullptr ||
        !GetMinigbmDmaBuf(handle.get(), capture_width, capture_height,
                          dmaBuf) ||
        !IsLinearNv12(*dmaBuf) || dmaBuf->planes[0].stride != raw_stride ||
        dmaBuf->total_size < raw_size) {
      ALOGE(
          "Codec2 decoder output does not match the Venus NV12 DMA-BUF layout");
      return C2_OMITTED;
    }
    return C2_OK;
  }

  c2_status_t
  QueueDecoderCapture(uint32_t index,
                      const std::shared_ptr<C2GraphicBlock> &block) {
    MinigbmDmaBuf dmaBuf;
    c2_status_t result = GetDecoderDmaBuf(block, &dmaBuf);
    if (result != C2_OK) {
      return result;
    }
    v4l2_plane plane{};
    plane.m.fd = dmaBuf.planes[0].fd;
    plane.length = static_cast<uint32_t>(dmaBuf.total_size);
    v4l2_buffer buffer{};
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buffer.memory = V4L2_MEMORY_DMABUF;
    buffer.index = index;
    buffer.length = 1;
    buffer.m.planes = &plane;
    if (Ioctl(fd.get(), VIDIOC_QBUF, &buffer) < 0) {
      return Error("queueing zero-copy decoder output");
    }
    decoder_capture[index].block = block;
    decoder_capture[index].queued = true;
    return C2_OK;
  }

  c2_status_t
  ProgressDecoderCaptureSetup(const std::shared_ptr<C2BlockPool> &pool) {
    if (!decoder_capture_setup_pending) {
      return C2_OK;
    }
    if (pool == nullptr) {
      return C2_NO_INIT;
    }
    for (uint32_t index = 0; index < decoder_capture.size(); ++index) {
      if (decoder_capture[index].queued) {
        continue;
      }
      std::shared_ptr<C2GraphicBlock> block;
      const C2MemoryUsage usage = {C2MemoryUsage::CPU_READ |
                                       GRALLOC_USAGE_HW_TEXTURE |
                                       GRALLOC_USAGE_HW_COMPOSER,
                                   GRALLOC1_PRODUCER_USAGE_VIDEO_DECODER};
      const c2_status_t result = pool->fetchGraphicBlock(
          capture_width, capture_height, HAL_PIXEL_FORMAT_YCBCR_420_888, usage,
          &block);
      if (result != C2_OK) {
        return result;
      }
      const c2_status_t queueResult = QueueDecoderCapture(index, block);
      if (queueResult != C2_OK) {
        return queueResult;
      }
    }
    c2_status_t result =
        StreamOn(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, &capture_streaming);
    if (result == C2_OK) {
      decoder_capture_setup_pending = false;
      decoder_capture_configured = true;
      ALOGI("%s uses zero-copy NV12 DMA-BUF output on %s", spec.component_name,
            device_path.c_str());
    }
    return result;
  }

  c2_status_t HandleDecoderEvents(const std::shared_ptr<C2BlockPool> &pool) {
    while (true) {
      v4l2_event event{};
      if (Ioctl(fd.get(), VIDIOC_DQEVENT, &event) < 0) {
        if (errno != EAGAIN) {
          return Error("dequeueing V4L2 event");
        }
        break;
      }
      if (event.type == V4L2_EVENT_SOURCE_CHANGE &&
          (event.u.src_change.changes & V4L2_EVENT_SRC_CH_RESOLUTION) != 0) {
        c2_status_t result = BeginDecoderCaptureSetup();
        if (result != C2_OK) {
          return result;
        }
      }
    }
    return ProgressDecoderCaptureSetup(pool);
  }

  c2_status_t RefillDecoderCapture(const std::shared_ptr<C2BlockPool> &pool,
                                   uint32_t index) {
    std::shared_ptr<C2GraphicBlock> block;
    const C2MemoryUsage usage = {C2MemoryUsage::CPU_READ |
                                     GRALLOC_USAGE_HW_TEXTURE |
                                     GRALLOC_USAGE_HW_COMPOSER,
                                 GRALLOC1_PRODUCER_USAGE_VIDEO_DECODER};
    const c2_status_t result =
        pool->fetchGraphicBlock(capture_width, capture_height,
                                HAL_PIXEL_FORMAT_YCBCR_420_888, usage, &block);
    return result == C2_OK ? QueueDecoderCapture(index, block) : result;
  }

  c2_status_t PumpDecoderCapture(const std::shared_ptr<C2BlockPool> &pool) {
    if (!decoder_capture_configured) {
      return C2_OK;
    }
    while (true) {
      v4l2_plane plane{};
      v4l2_buffer buffer{};
      buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
      buffer.memory = V4L2_MEMORY_DMABUF;
      buffer.length = 1;
      buffer.m.planes = &plane;
      if (Ioctl(fd.get(), VIDIOC_DQBUF, &buffer) < 0) {
        return errno == EAGAIN || (errno == EPIPE && draining)
                   ? C2_OK
                   : Error("dequeueing decoder output");
      }
      if (buffer.index >= decoder_capture.size() ||
          decoder_capture[buffer.index].block == nullptr) {
        return C2_CORRUPTED;
      }

      V4l2DecodedFrame frame;
      frame.block = std::move(decoder_capture[buffer.index].block);
      frame.frame_index = TimestampToFrameIndex(buffer.timestamp);
      frame.width = visible_width;
      frame.height = visible_height;
      frame.end_of_stream = (buffer.flags & V4L2_BUF_FLAG_LAST) != 0;
      decoder_capture[buffer.index].queued = false;
      if (plane.bytesused != 0) {
        decoder_pending.push_back(std::move(frame));
      } else if (frame.end_of_stream) {
        decoder_pending.push_back(std::move(frame));
      }

      if ((buffer.flags & V4L2_BUF_FLAG_LAST) == 0) {
        c2_status_t result = RefillDecoderCapture(pool, buffer.index);
        if (result != C2_OK) {
          return result;
        }
      }
    }
  }

  c2_status_t PumpDecoder(const std::shared_ptr<C2BlockPool> &pool) {
    c2_status_t result = HandleDecoderEvents(pool);
    if (result != C2_OK && result != C2_BLOCKING) {
      return result;
    }
    const c2_status_t inputResult = DequeueDecoderInputs();
    if (inputResult != C2_OK) {
      return inputResult;
    }
    if (result == C2_BLOCKING) {
      return result;
    }
    return PumpDecoderCapture(pool);
  }

  c2_status_t DequeueDecoderFrame(const std::shared_ptr<C2BlockPool> &pool,
                                  V4l2DecodedFrame *output, bool wait) {
    if (output == nullptr) {
      return C2_BAD_VALUE;
    }
    c2_status_t result = PumpDecoder(pool);
    if (result != C2_OK && result != C2_BLOCKING) {
      return result;
    }
    if (decoder_pending.empty() && wait) {
      result = WaitForDevice(POLLIN | POLLOUT | POLLPRI);
      if (result != C2_OK) {
        return result;
      }
      result = PumpDecoder(pool);
      if (result != C2_OK && result != C2_BLOCKING) {
        return result;
      }
    }
    if (decoder_pending.empty()) {
      return result == C2_BLOCKING ? C2_BLOCKING : C2_NOT_FOUND;
    }
    *output = std::move(decoder_pending.front());
    decoder_pending.pop_front();
    return C2_OK;
  }

  c2_status_t StartDecoderDrain() {
    if (draining) {
      return C2_OK;
    }
    if (!decoder_received_input) {
      V4l2DecodedFrame frame;
      frame.end_of_stream = true;
      decoder_pending.push_back(std::move(frame));
      draining = true;
      return C2_OK;
    }
    v4l2_decoder_cmd command{};
    command.cmd = V4L2_DEC_CMD_STOP;
    if (Ioctl(fd.get(), VIDIOC_DECODER_CMD, &command) < 0) {
      return Error("stopping V4L2 decoder");
    }
    draining = true;
    return C2_OK;
  }

  CodecSpec spec;
  std::string device_path;
  android::base::unique_fd fd;
  std::optional<V4l2EncoderSettings> encoder_settings;
  VulkanFrameConverter frame_converter;

  std::vector<EncoderInputSlot> encoder_inputs;
  std::vector<DecoderInputSlot> decoder_inputs;
  std::vector<DecoderCaptureSlot> decoder_capture;
  std::vector<MappedBuffer> output_mmaps;
  std::vector<MappedBuffer> capture_mmaps;
  std::deque<V4l2EncodedFrame> encoder_pending;
  std::deque<V4l2DecodedFrame> decoder_pending;

  bool encoder_configured = false;
  bool decoder_capture_configured = false;
  bool decoder_capture_setup_pending = false;
  bool output_streaming = false;
  bool capture_streaming = false;
  bool draining = false;
  bool decoder_received_input = false;
  uint32_t current_bitrate = 0;
  uint32_t raw_size = 0;
  uint32_t raw_stride = 0;
  uint32_t coded_size = 0;
  uint32_t capture_width = 0;
  uint32_t capture_height = 0;
  uint32_t visible_width = 0;
  uint32_t visible_height = 0;
};

V4l2CodecSession::V4l2CodecSession(CodecSpec spec, std::string devicePath)
    : impl_(std::make_unique<Impl>(std::move(spec), std::move(devicePath))) {}

V4l2CodecSession::~V4l2CodecSession() = default;

c2_status_t V4l2CodecSession::Open(const V4l2EncoderSettings *encoderSettings) {
  return impl_->Open(encoderSettings);
}

void V4l2CodecSession::Close() { impl_->Close(); }

c2_status_t V4l2CodecSession::Flush() { return impl_->Flush(); }

c2_status_t
V4l2CodecSession::QueueEncoderFrame(const std::shared_ptr<C2BlockPool> &pool,
                                    const std::shared_ptr<C2Buffer> &buffer,
                                    uint64_t frameIndex, bool requestSync,
                                    uint32_t bitrate) {
  return impl_->QueueEncoderFrame(pool, buffer, frameIndex, requestSync,
                                  bitrate);
}

c2_status_t V4l2CodecSession::DequeueEncoderFrame(V4l2EncodedFrame *output,
                                                  bool wait) {
  return impl_->DequeueEncoderFrame(output, wait);
}

c2_status_t V4l2CodecSession::StartEncoderDrain() {
  return impl_->StartEncoderDrain();
}

c2_status_t V4l2CodecSession::QueueDecoderPacket(const uint8_t *data,
                                                 size_t size,
                                                 uint64_t frameIndex) {
  return impl_->QueueDecoderPacket(data, size, frameIndex);
}

c2_status_t
V4l2CodecSession::DequeueDecoderFrame(const std::shared_ptr<C2BlockPool> &pool,
                                      V4l2DecodedFrame *output, bool wait) {
  return impl_->DequeueDecoderFrame(pool, output, wait);
}

c2_status_t V4l2CodecSession::StartDecoderDrain() {
  return impl_->StartDecoderDrain();
}

} // namespace floral::codec
