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

#define LOG_TAG "FloralCodec2Vulkan"

#include "floral/codec/VulkanFrameConverter.h"

#include "RgbToNv12Shader.h"

#include <C2AllocatorGralloc.h>
#include <android-base/unique_fd.h>
#include <drm_fourcc.h>
#include <log/log.h>
#include <vulkan/vulkan.h>

#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <unordered_map>
#include <vector>

namespace floral::codec {
namespace {

constexpr std::array<const char *, 3> kRequiredDeviceExtensions = {
    VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
};
constexpr size_t kSourceCacheCapacity = 8;
constexpr size_t kDestinationCacheCapacity = 10;

struct BufferSignature {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t drm_format = 0;
  uint64_t modifier = 0;
  uint64_t total_size = 0;
  uint32_t plane_count = 0;
  std::array<uint32_t, kMaxDmaBufPlanes> strides{};
  std::array<uint32_t, kMaxDmaBufPlanes> offsets{};
  std::array<uint32_t, kMaxDmaBufPlanes> sizes{};

  bool operator==(const BufferSignature &other) const {
    return width == other.width && height == other.height &&
           drm_format == other.drm_format && modifier == other.modifier &&
           total_size == other.total_size &&
           plane_count == other.plane_count && strides == other.strides &&
           offsets == other.offsets && sizes == other.sizes;
  }
};

BufferSignature MakeBufferSignature(const MinigbmDmaBuf &buffer) {
  BufferSignature signature;
  signature.width = buffer.width;
  signature.height = buffer.height;
  signature.drm_format = buffer.drm_format;
  signature.modifier = buffer.modifier;
  signature.total_size = buffer.total_size;
  signature.plane_count = buffer.plane_count;
  for (uint32_t plane = 0; plane < buffer.plane_count; ++plane) {
    signature.strides[plane] = buffer.planes[plane].stride;
    signature.offsets[plane] = buffer.planes[plane].offset;
    signature.sizes[plane] = buffer.planes[plane].size;
  }
  return signature;
}

struct NativeHandleDeleter {
  void operator()(native_handle_t *handle) const {
    if (handle != nullptr) {
      native_handle_delete(handle);
    }
  }
};

using NativeHandle = std::unique_ptr<native_handle_t, NativeHandleDeleter>;

bool IsRgbFormat(uint32_t format) {
  switch (format) {
  case DRM_FORMAT_ABGR8888:
  case DRM_FORMAT_ARGB8888:
  case DRM_FORMAT_XBGR8888:
  case DRM_FORMAT_XRGB8888:
  case DRM_FORMAT_RGB565:
    return true;
  default:
    return false;
  }
}

VkFormat VulkanFormatForDrm(uint32_t format) {
  switch (format) {
  case DRM_FORMAT_ABGR8888:
  case DRM_FORMAT_XBGR8888:
    return VK_FORMAT_R8G8B8A8_UNORM;
  case DRM_FORMAT_ARGB8888:
  case DRM_FORMAT_XRGB8888:
    return VK_FORMAT_B8G8R8A8_UNORM;
  case DRM_FORMAT_RGB565:
    return VK_FORMAT_R5G6B5_UNORM_PACK16;
  default:
    return VK_FORMAT_UNDEFINED;
  }
}

bool SameDmaBufObject(int first, int second) {
  struct stat firstStat {};
  struct stat secondStat {};
  return fstat(first, &firstStat) == 0 && fstat(second, &secondStat) == 0 &&
         firstStat.st_dev == secondStat.st_dev &&
         firstStat.st_ino == secondStat.st_ino;
}

bool SupportsExtensions(VkPhysicalDevice device) {
  uint32_t count = 0;
  if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr) !=
      VK_SUCCESS) {
    return false;
  }
  std::vector<VkExtensionProperties> extensions(count);
  if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count,
                                           extensions.data()) != VK_SUCCESS) {
    return false;
  }
  for (const char *required : kRequiredDeviceExtensions) {
    bool found = false;
    for (const VkExtensionProperties &extension : extensions) {
      if (std::strcmp(extension.extensionName, required) == 0) {
        found = true;
        break;
      }
    }
    if (!found) {
      ALOGE("required Vulkan extension %s is unavailable", required);
      return false;
    }
  }
  return true;
}

bool CheckedPlaneRange(uint32_t offset, uint32_t stride, uint32_t rows,
                       uint64_t totalSize, VkDeviceSize *range) {
  if (range == nullptr || rows == 0 || stride == 0) {
    return false;
  }
  const uint64_t length = static_cast<uint64_t>(stride) * rows;
  if (offset > totalSize || length > totalSize - offset) {
    return false;
  }
  *range = static_cast<VkDeviceSize>(length);
  return true;
}

struct ConversionParameters {
  uint32_t width;
  uint32_t height;
  uint32_t y_stride;
  uint32_t uv_stride;
};

} // namespace

class VulkanFrameConverter::Impl {
public:
  ~Impl() { Reset(); }

  c2_status_t Initialize() {
    if (device_ != VK_NULL_HANDLE) {
      return C2_OK;
    }

    VkApplicationInfo applicationInfo{};
    applicationInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    applicationInfo.pApplicationName = "Floral Codec2 RGB converter";
    applicationInfo.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo instanceInfo{};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &applicationInfo;
    if (!Check(vkCreateInstance(&instanceInfo, nullptr, &instance_),
               "creating a Vulkan instance")) {
      Reset();
      return C2_OMITTED;
    }

    uint32_t physicalDeviceCount = 0;
    if (!Check(vkEnumeratePhysicalDevices(instance_, &physicalDeviceCount,
                                          nullptr),
               "enumerating Vulkan physical devices") ||
        physicalDeviceCount == 0) {
      Reset();
      return C2_OMITTED;
    }
    std::vector<VkPhysicalDevice> physicalDevices(physicalDeviceCount);
    if (!Check(vkEnumeratePhysicalDevices(instance_, &physicalDeviceCount,
                                          physicalDevices.data()),
               "enumerating Vulkan physical devices")) {
      Reset();
      return C2_OMITTED;
    }
    for (VkPhysicalDevice candidate : physicalDevices) {
      uint32_t queueCount = 0;
      vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueCount, nullptr);
      std::vector<VkQueueFamilyProperties> queues(queueCount);
      vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueCount,
                                               queues.data());
      for (uint32_t queue = 0; queue < queueCount; ++queue) {
        if ((queues[queue].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0 &&
            SupportsExtensions(candidate)) {
          physical_device_ = candidate;
          queue_family_ = queue;
          break;
        }
      }
      if (physical_device_ != VK_NULL_HANDLE) {
        break;
      }
    }
    if (physical_device_ == VK_NULL_HANDLE) {
      ALOGE("no Vulkan device supports the external-memory compute path");
      Reset();
      return C2_OMITTED;
    }

    vkGetPhysicalDeviceProperties(physical_device_, &device_properties_);
    vkGetPhysicalDeviceMemoryProperties(physical_device_, &memory_properties_);
    VkPhysicalDeviceFeatures supportedFeatures{};
    vkGetPhysicalDeviceFeatures(physical_device_, &supportedFeatures);
    if (supportedFeatures.shaderStorageImageExtendedFormats != VK_TRUE) {
      ALOGE("Vulkan extended storage image formats are unavailable");
      Reset();
      return C2_OMITTED;
    }
    VkFormatProperties yFormatProperties{};
    VkFormatProperties uvFormatProperties{};
    vkGetPhysicalDeviceFormatProperties(physical_device_, VK_FORMAT_R8_UINT,
                                        &yFormatProperties);
    vkGetPhysicalDeviceFormatProperties(physical_device_, VK_FORMAT_R8G8_UINT,
                                        &uvFormatProperties);
    if ((yFormatProperties.bufferFeatures &
         VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT) == 0 ||
        (uvFormatProperties.bufferFeatures &
         VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT) == 0) {
      ALOGE("Vulkan storage texel buffers do not support NV12 plane formats");
      Reset();
      return C2_OMITTED;
    }

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = queue_family_;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkDeviceCreateInfo deviceInfo{};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    VkPhysicalDeviceFeatures enabledFeatures{};
    enabledFeatures.shaderStorageImageExtendedFormats = VK_TRUE;
    deviceInfo.pEnabledFeatures = &enabledFeatures;
    deviceInfo.enabledExtensionCount = kRequiredDeviceExtensions.size();
    deviceInfo.ppEnabledExtensionNames = kRequiredDeviceExtensions.data();
    if (!Check(vkCreateDevice(physical_device_, &deviceInfo, nullptr, &device_),
               "creating a Vulkan device")) {
      Reset();
      return C2_OMITTED;
    }
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
    get_memory_fd_properties_ =
        reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(
            vkGetDeviceProcAddr(device_, "vkGetMemoryFdPropertiesKHR"));
    if (get_memory_fd_properties_ == nullptr) {
      ALOGE("Vulkan external-memory entry points are unavailable");
      Reset();
      return C2_OMITTED;
    }

    if (!CreatePipelineObjects() || !CreateCommandObjects()) {
      Reset();
      return C2_OMITTED;
    }
    ALOGI("Vulkan RGB-to-NV12 zero-copy conversion is available on %s",
          device_properties_.deviceName);
    return C2_OK;
  }

  void Reset() {
    if (device_ != VK_NULL_HANDLE) {
      (void)vkDeviceWaitIdle(device_);
      if (source_cache_misses_ != 0 || destination_cache_misses_ != 0) {
        ALOGI("Vulkan DMA-BUF cache: source=%" PRIu64 "/%" PRIu64
              " hit/miss, destination=%" PRIu64 "/%" PRIu64 " hit/miss",
              source_cache_hits_, source_cache_misses_,
              destination_cache_hits_, destination_cache_misses_);
      }
      for (auto &[id, resource] : source_resources_) {
        (void)id;
        DestroySource(&resource);
      }
      for (auto &[id, resource] : destination_resources_) {
        (void)id;
        DestroyDestination(&resource);
      }
      vkDestroyFence(device_, fence_, nullptr);
      vkDestroyCommandPool(device_, command_pool_, nullptr);
      vkDestroyPipeline(device_, pipeline_, nullptr);
      vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
      vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
      vkDestroyDescriptorSetLayout(device_, descriptor_set_layout_, nullptr);
      vkDestroySampler(device_, sampler_, nullptr);
      vkDestroyDevice(device_, nullptr);
    }
    if (instance_ != VK_NULL_HANDLE) {
      vkDestroyInstance(instance_, nullptr);
    }
    source_resources_.clear();
    destination_resources_.clear();
    instance_ = VK_NULL_HANDLE;
    physical_device_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
    queue_ = VK_NULL_HANDLE;
    queue_family_ = 0;
    device_properties_ = {};
    memory_properties_ = {};
    sampler_ = VK_NULL_HANDLE;
    descriptor_set_layout_ = VK_NULL_HANDLE;
    descriptor_pool_ = VK_NULL_HANDLE;
    descriptor_set_ = VK_NULL_HANDLE;
    pipeline_layout_ = VK_NULL_HANDLE;
    pipeline_ = VK_NULL_HANDLE;
    command_pool_ = VK_NULL_HANDLE;
    command_buffer_ = VK_NULL_HANDLE;
    fence_ = VK_NULL_HANDLE;
    get_memory_fd_properties_ = nullptr;
    cache_use_counter_ = 0;
    source_cache_hits_ = 0;
    source_cache_misses_ = 0;
    destination_cache_hits_ = 0;
    destination_cache_misses_ = 0;
  }

  c2_status_t Convert(const C2ConstGraphicBlock &source,
                      const MinigbmDmaBuf &destination) {
    if (Initialize() != C2_OK) {
      return C2_OMITTED;
    }
    NativeHandle handle(
        android::UnwrapNativeCodec2GrallocHandle(source.handle()));
    MinigbmDmaBuf input;
    if (handle == nullptr ||
        !GetMinigbmDmaBuf(handle.get(), source.width(), source.height(),
                          &input) ||
        !IsRgbFormat(input.drm_format) || input.plane_count != 1) {
      ALOGE("Vulkan conversion requires a single-plane RGB minigbm input");
      return C2_OMITTED;
    }
    if (!ValidateDestination(source, destination)) {
      return C2_BAD_VALUE;
    }

    SourceResource *sourceResource = nullptr;
    DestinationResource *destinationResource = nullptr;
    c2_status_t result = GetSourceResource(input, &sourceResource);
    if (result != C2_OK) {
      return result;
    }
    result = GetDestinationResource(destination, &destinationResource);
    if (result != C2_OK) {
      return result;
    }

    VkDescriptorImageInfo sourceDescriptor{};
    sourceDescriptor.sampler = sampler_;
    sourceDescriptor.imageView = sourceResource->view;
    sourceDescriptor.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    std::array<VkWriteDescriptorSet, 3> writes{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = descriptor_set_;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &sourceDescriptor;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = descriptor_set_;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
    writes[1].pTexelBufferView = &destinationResource->y_view;
    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = descriptor_set_;
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
    writes[2].pTexelBufferView = &destinationResource->uv_view;
    vkUpdateDescriptorSets(device_, writes.size(), writes.data(), 0, nullptr);

    return Dispatch(*sourceResource, *destinationResource, destination);
  }

private:
  struct SourceResource {
    BufferSignature signature;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_PREINITIALIZED;
    uint64_t last_used = 0;
  };

  struct DestinationResource {
    BufferSignature signature;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkBufferView y_view = VK_NULL_HANDLE;
    VkBufferView uv_view = VK_NULL_HANDLE;
    uint64_t last_used = 0;
  };

  bool Check(VkResult result, const char *operation) const {
    if (result == VK_SUCCESS) {
      return true;
    }
    ALOGE("%s failed with Vulkan result %d", operation, result);
    return false;
  }

  bool CreatePipelineObjects() {
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (!Check(vkCreateSampler(device_, &samplerInfo, nullptr, &sampler_),
               "creating the conversion sampler")) {
      return false;
    }

    std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = bindings.size();
    layoutInfo.pBindings = bindings.data();
    if (!Check(vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr,
                                           &descriptor_set_layout_),
               "creating the conversion descriptor layout")) {
      return false;
    }

    const std::array<VkDescriptorPoolSize, 2> poolSizes = {{
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
        {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 2},
    }};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = poolSizes.size();
    poolInfo.pPoolSizes = poolSizes.data();
    if (!Check(vkCreateDescriptorPool(device_, &poolInfo, nullptr,
                                      &descriptor_pool_),
               "creating the conversion descriptor pool")) {
      return false;
    }
    VkDescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocateInfo.descriptorPool = descriptor_pool_;
    allocateInfo.descriptorSetCount = 1;
    allocateInfo.pSetLayouts = &descriptor_set_layout_;
    if (!Check(
            vkAllocateDescriptorSets(device_, &allocateInfo, &descriptor_set_),
            "allocating the conversion descriptor set")) {
      return false;
    }

    VkPushConstantRange pushConstants{};
    pushConstants.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushConstants.size = sizeof(ConversionParameters);
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &descriptor_set_layout_;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstants;
    if (!Check(vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr,
                                      &pipeline_layout_),
               "creating the conversion pipeline layout")) {
      return false;
    }

    VkShaderModuleCreateInfo shaderInfo{};
    shaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shaderInfo.codeSize = sizeof(kRgbToNv12Shader);
    shaderInfo.pCode = kRgbToNv12Shader;
    VkShaderModule shader = VK_NULL_HANDLE;
    if (!Check(vkCreateShaderModule(device_, &shaderInfo, nullptr, &shader),
               "creating the conversion shader")) {
      return false;
    }
    VkPipelineShaderStageCreateInfo stageInfo{};
    stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stageInfo.module = shader;
    stageInfo.pName = "main";
    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage = stageInfo;
    pipelineInfo.layout = pipeline_layout_;
    const VkResult pipelineResult = vkCreateComputePipelines(
        device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline_);
    vkDestroyShaderModule(device_, shader, nullptr);
    return Check(pipelineResult, "creating the conversion pipeline");
  }

  bool CreateCommandObjects() {
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queue_family_;
    if (!Check(vkCreateCommandPool(device_, &poolInfo, nullptr, &command_pool_),
               "creating the conversion command pool")) {
      return false;
    }
    VkCommandBufferAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocateInfo.commandPool = command_pool_;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = 1;
    if (!Check(
            vkAllocateCommandBuffers(device_, &allocateInfo, &command_buffer_),
            "allocating the conversion command buffer")) {
      return false;
    }
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    return Check(vkCreateFence(device_, &fenceInfo, nullptr, &fence_),
                 "creating the conversion fence");
  }

  bool ValidateDestination(const C2ConstGraphicBlock &source,
                           const MinigbmDmaBuf &destination) const {
    if (destination.drm_format != DRM_FORMAT_NV12 ||
        destination.modifier != DRM_FORMAT_MOD_LINEAR ||
        destination.plane_count != 2 || source.width() != destination.width ||
        source.height() != destination.height ||
        (destination.width & 1u) != 0 || (destination.height & 1u) != 0 ||
        destination.planes[0].stride != destination.planes[1].stride ||
        (destination.planes[1].stride & 1u) != 0 ||
        !SameDmaBufObject(destination.planes[0].fd, destination.planes[1].fd)) {
      ALOGE("Vulkan conversion requires one linear, even-sized NV12 DMA-BUF");
      return false;
    }
    return true;
  }

  uint32_t FindMemoryType(uint32_t bits) const {
    for (uint32_t index = 0; index < memory_properties_.memoryTypeCount;
         ++index) {
      if ((bits & (1u << index)) != 0) {
        return index;
      }
    }
    return std::numeric_limits<uint32_t>::max();
  }

  c2_status_t GetSourceResource(const MinigbmDmaBuf &input,
                                SourceResource **resource) {
    if (resource == nullptr) {
      return C2_BAD_VALUE;
    }
    const BufferSignature signature = MakeBufferSignature(input);
    auto found = source_resources_.find(input.buffer_id);
    if (found != source_resources_.end()) {
      if (found->second.signature == signature) {
        found->second.last_used = ++cache_use_counter_;
        ++source_cache_hits_;
        *resource = &found->second;
        return C2_OK;
      }
      ALOGW("RGB buffer ID %" PRIu64 " changed layout; reimporting",
            input.buffer_id);
      DestroySource(&found->second);
      source_resources_.erase(found);
    }
    if (source_resources_.size() >= kSourceCacheCapacity) {
      EvictOldestSource();
    }

    auto [inserted, unused] = source_resources_.try_emplace(input.buffer_id);
    (void)unused;
    inserted->second.signature = signature;
    inserted->second.last_used = ++cache_use_counter_;
    ++source_cache_misses_;
    const c2_status_t result = ImportSource(input, &inserted->second);
    if (result != C2_OK) {
      DestroySource(&inserted->second);
      source_resources_.erase(inserted);
      return result;
    }
    *resource = &inserted->second;
    return C2_OK;
  }

  c2_status_t GetDestinationResource(const MinigbmDmaBuf &destination,
                                     DestinationResource **resource) {
    if (resource == nullptr) {
      return C2_BAD_VALUE;
    }
    const BufferSignature signature = MakeBufferSignature(destination);
    auto found = destination_resources_.find(destination.buffer_id);
    if (found != destination_resources_.end()) {
      if (found->second.signature == signature) {
        found->second.last_used = ++cache_use_counter_;
        ++destination_cache_hits_;
        *resource = &found->second;
        return C2_OK;
      }
      ALOGW("NV12 buffer ID %" PRIu64 " changed layout; reimporting",
            destination.buffer_id);
      DestroyDestination(&found->second);
      destination_resources_.erase(found);
    }
    if (destination_resources_.size() >= kDestinationCacheCapacity) {
      EvictOldestDestination();
    }

    auto [inserted, unused] =
        destination_resources_.try_emplace(destination.buffer_id);
    (void)unused;
    inserted->second.signature = signature;
    inserted->second.last_used = ++cache_use_counter_;
    ++destination_cache_misses_;
    const c2_status_t result =
        ImportDestination(destination, &inserted->second);
    if (result != C2_OK) {
      DestroyDestination(&inserted->second);
      destination_resources_.erase(inserted);
      return result;
    }
    *resource = &inserted->second;
    return C2_OK;
  }

  void EvictOldestSource() {
    auto oldest = source_resources_.end();
    for (auto current = source_resources_.begin();
         current != source_resources_.end(); ++current) {
      if (oldest == source_resources_.end() ||
          current->second.last_used < oldest->second.last_used) {
        oldest = current;
      }
    }
    if (oldest != source_resources_.end()) {
      DestroySource(&oldest->second);
      source_resources_.erase(oldest);
    }
  }

  void EvictOldestDestination() {
    auto oldest = destination_resources_.end();
    for (auto current = destination_resources_.begin();
         current != destination_resources_.end(); ++current) {
      if (oldest == destination_resources_.end() ||
          current->second.last_used < oldest->second.last_used) {
        oldest = current;
      }
    }
    if (oldest != destination_resources_.end()) {
      DestroyDestination(&oldest->second);
      destination_resources_.erase(oldest);
    }
  }

  c2_status_t ImportSource(const MinigbmDmaBuf &input,
                           SourceResource *resource) {
    const VkFormat format = VulkanFormatForDrm(input.drm_format);
    if (format == VK_FORMAT_UNDEFINED ||
        input.modifier == DRM_FORMAT_MOD_INVALID) {
      ALOGE("DRM format %#x or modifier %#" PRIx64
            " cannot be imported as a Vulkan image",
            input.drm_format, input.modifier);
      return C2_OMITTED;
    }

    VkSubresourceLayout planeLayout{};
    planeLayout.offset = input.planes[0].offset;
    planeLayout.size = input.planes[0].size;
    planeLayout.rowPitch = input.planes[0].stride;
    VkImageDrmFormatModifierExplicitCreateInfoEXT modifierInfo{};
    modifierInfo.sType =
        VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
    modifierInfo.drmFormatModifier = input.modifier;
    modifierInfo.drmFormatModifierPlaneCount = 1;
    modifierInfo.pPlaneLayouts = &planeLayout;
    VkExternalMemoryImageCreateInfo externalInfo{};
    externalInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    externalInfo.pNext = &modifierInfo;
    externalInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.pNext = &externalInfo;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {input.width, input.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
    if (!Check(vkCreateImage(device_, &imageInfo, nullptr, &resource->image),
               "creating the RGB import image")) {
      return C2_OMITTED;
    }

    android::base::unique_fd importFd(dup(input.planes[0].fd));
    if (importFd.get() < 0) {
      ALOGE("duplicating the RGB DMA-BUF failed: %s", std::strerror(errno));
      return C2_CORRUPTED;
    }
    VkMemoryFdPropertiesKHR fdProperties{};
    fdProperties.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
    if (!Check(get_memory_fd_properties_(
                   device_, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                   importFd.get(), &fdProperties),
               "querying RGB DMA-BUF memory")) {
      return C2_OMITTED;
    }
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device_, resource->image, &requirements);
    const uint32_t memoryType = FindMemoryType(requirements.memoryTypeBits &
                                               fdProperties.memoryTypeBits);
    if (memoryType == std::numeric_limits<uint32_t>::max()) {
      ALOGE("the RGB DMA-BUF has no compatible Vulkan memory type");
      return C2_OMITTED;
    }
    VkMemoryDedicatedAllocateInfo dedicatedInfo{};
    dedicatedInfo.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicatedInfo.image = resource->image;
    VkImportMemoryFdInfoKHR importInfo{};
    importInfo.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
    importInfo.pNext = &dedicatedInfo;
    importInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    const int importFdValue = importFd.release();
    importInfo.fd = importFdValue;
    VkMemoryAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocateInfo.pNext = &importInfo;
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex = memoryType;
    // A successful external-memory import consumes the FD inside this call.
    // Drop unique_fd ownership first so Android fdsan does not see the
    // driver's close as a close by the wrong owner.
    if (!Check(vkAllocateMemory(device_, &allocateInfo, nullptr,
                                &resource->memory),
               "importing RGB DMA-BUF memory")) {
      close(importFdValue);
      return C2_OMITTED;
    }
    if (!Check(vkBindImageMemory(device_, resource->image, resource->memory, 0),
               "binding RGB DMA-BUF memory")) {
      return C2_OMITTED;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = resource->image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    if (!Check(vkCreateImageView(device_, &viewInfo, nullptr, &resource->view),
               "creating the RGB image view")) {
      return C2_OMITTED;
    }
    return C2_OK;
  }

  c2_status_t ImportDestination(const MinigbmDmaBuf &destination,
                                DestinationResource *resource) {
    VkDeviceSize yRange = 0;
    VkDeviceSize uvRange = 0;
    if (!CheckedPlaneRange(destination.planes[0].offset,
                           destination.planes[0].stride, destination.height,
                           destination.total_size, &yRange) ||
        !CheckedPlaneRange(destination.planes[1].offset,
                           destination.planes[1].stride, destination.height / 2,
                           destination.total_size, &uvRange) ||
        destination.planes[0].offset %
                device_properties_.limits.minTexelBufferOffsetAlignment !=
            0 ||
        destination.planes[1].offset %
                device_properties_.limits.minTexelBufferOffsetAlignment !=
            0 ||
        yRange > device_properties_.limits.maxTexelBufferElements ||
        uvRange / 2 > device_properties_.limits.maxTexelBufferElements) {
      ALOGE("the NV12 DMA-BUF layout cannot be addressed as Vulkan texels");
      return C2_OMITTED;
    }

    VkExternalMemoryBufferCreateInfo externalInfo{};
    externalInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
    externalInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.pNext = &externalInfo;
    bufferInfo.size = destination.total_size;
    bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!Check(vkCreateBuffer(device_, &bufferInfo, nullptr, &resource->buffer),
               "creating the NV12 import buffer")) {
      return C2_OMITTED;
    }

    android::base::unique_fd importFd(dup(destination.planes[0].fd));
    if (importFd.get() < 0) {
      ALOGE("duplicating the NV12 DMA-BUF failed: %s", std::strerror(errno));
      return C2_CORRUPTED;
    }
    VkMemoryFdPropertiesKHR fdProperties{};
    fdProperties.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
    if (!Check(get_memory_fd_properties_(
                   device_, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                   importFd.get(), &fdProperties),
               "querying NV12 DMA-BUF memory")) {
      return C2_OMITTED;
    }
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_, resource->buffer, &requirements);
    const uint32_t memoryType = FindMemoryType(requirements.memoryTypeBits &
                                               fdProperties.memoryTypeBits);
    if (memoryType == std::numeric_limits<uint32_t>::max()) {
      ALOGE("the NV12 DMA-BUF has no compatible Vulkan memory type");
      return C2_OMITTED;
    }
    VkImportMemoryFdInfoKHR importInfo{};
    importInfo.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
    VkMemoryDedicatedAllocateInfo dedicatedInfo{};
    dedicatedInfo.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicatedInfo.buffer = resource->buffer;
    importInfo.pNext = &dedicatedInfo;
    importInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    const int importFdValue = importFd.release();
    importInfo.fd = importFdValue;
    VkMemoryAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocateInfo.pNext = &importInfo;
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex = memoryType;
    // A successful external-memory import consumes the FD inside this call.
    // Drop unique_fd ownership first so Android fdsan does not see the
    // driver's close as a close by the wrong owner.
    if (!Check(vkAllocateMemory(device_, &allocateInfo, nullptr,
                                &resource->memory),
               "importing NV12 DMA-BUF memory")) {
      close(importFdValue);
      return C2_OMITTED;
    }
    if (!Check(
            vkBindBufferMemory(device_, resource->buffer, resource->memory, 0),
            "binding NV12 DMA-BUF memory")) {
      return C2_OMITTED;
    }

    VkBufferViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
    viewInfo.buffer = resource->buffer;
    viewInfo.format = VK_FORMAT_R8_UINT;
    viewInfo.offset = destination.planes[0].offset;
    viewInfo.range = yRange;
    if (!Check(
            vkCreateBufferView(device_, &viewInfo, nullptr, &resource->y_view),
            "creating the NV12 luma view")) {
      return C2_OMITTED;
    }
    viewInfo.format = VK_FORMAT_R8G8_UINT;
    viewInfo.offset = destination.planes[1].offset;
    viewInfo.range = uvRange;
    if (!Check(
            vkCreateBufferView(device_, &viewInfo, nullptr, &resource->uv_view),
            "creating the NV12 chroma view")) {
      return C2_OMITTED;
    }
    return C2_OK;
  }

  c2_status_t Dispatch(SourceResource &source,
                       const DestinationResource &destinationResource,
                       const MinigbmDmaBuf &destination) {
    if (!Check(vkResetFences(device_, 1, &fence_),
               "resetting the conversion fence") ||
        !Check(vkResetCommandBuffer(command_buffer_, 0),
               "resetting the conversion command buffer")) {
      return C2_CORRUPTED;
    }
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!Check(vkBeginCommandBuffer(command_buffer_, &beginInfo),
               "beginning the conversion command buffer")) {
      return C2_CORRUPTED;
    }

    VkImageMemoryBarrier sourceAcquire{};
    sourceAcquire.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    sourceAcquire.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    sourceAcquire.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    sourceAcquire.oldLayout = source.layout;
    sourceAcquire.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    sourceAcquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    sourceAcquire.dstQueueFamilyIndex = queue_family_;
    sourceAcquire.image = source.image;
    sourceAcquire.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    sourceAcquire.subresourceRange.levelCount = 1;
    sourceAcquire.subresourceRange.layerCount = 1;
    VkBufferMemoryBarrier destinationAcquire{};
    destinationAcquire.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    destinationAcquire.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    destinationAcquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    destinationAcquire.dstQueueFamilyIndex = queue_family_;
    destinationAcquire.buffer = destinationResource.buffer;
    destinationAcquire.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1,
                         &destinationAcquire, 1, &sourceAcquire);

    const ConversionParameters parameters = {
        destination.width, destination.height, destination.planes[0].stride,
        destination.planes[1].stride};
    vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                      pipeline_);
    vkCmdBindDescriptorSets(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline_layout_, 0, 1, &descriptor_set_, 0,
                            nullptr);
    vkCmdPushConstants(command_buffer_, pipeline_layout_,
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(parameters),
                       &parameters);
    vkCmdDispatch(command_buffer_, (destination.width + 15) / 16,
                  (destination.height + 15) / 16, 1);

    VkImageMemoryBarrier sourceRelease{};
    sourceRelease.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    sourceRelease.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    sourceRelease.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    sourceRelease.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    sourceRelease.srcQueueFamilyIndex = queue_family_;
    sourceRelease.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    sourceRelease.image = source.image;
    sourceRelease.subresourceRange = sourceAcquire.subresourceRange;
    VkBufferMemoryBarrier destinationRelease{};
    destinationRelease.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    destinationRelease.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    destinationRelease.srcQueueFamilyIndex = queue_family_;
    destinationRelease.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    destinationRelease.buffer = destinationResource.buffer;
    destinationRelease.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 1,
                         &destinationRelease, 1, &sourceRelease);
    if (!Check(vkEndCommandBuffer(command_buffer_),
               "ending the conversion command buffer")) {
      return C2_CORRUPTED;
    }

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &command_buffer_;
    if (!Check(vkQueueSubmit(queue_, 1, &submitInfo, fence_),
               "submitting RGB-to-NV12 conversion")) {
      return C2_CORRUPTED;
    }
    // V4L2 has no input fence in this API, so wait without CPU mapping or
    // copying before queueing the same DMA-BUF to Venus.
    if (!Check(vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX),
               "waiting for RGB-to-NV12 conversion")) {
      (void)vkDeviceWaitIdle(device_);
      return C2_CORRUPTED;
    }
    source.layout = VK_IMAGE_LAYOUT_GENERAL;
    return C2_OK;
  }

  void DestroySource(SourceResource *resource) const {
    if (device_ != VK_NULL_HANDLE) {
      vkDestroyImageView(device_, resource->view, nullptr);
      vkDestroyImage(device_, resource->image, nullptr);
      vkFreeMemory(device_, resource->memory, nullptr);
    }
    *resource = {};
  }

  void DestroyDestination(DestinationResource *resource) const {
    if (device_ != VK_NULL_HANDLE) {
      vkDestroyBufferView(device_, resource->uv_view, nullptr);
      vkDestroyBufferView(device_, resource->y_view, nullptr);
      vkDestroyBuffer(device_, resource->buffer, nullptr);
      vkFreeMemory(device_, resource->memory, nullptr);
    }
    *resource = {};
  }

  VkInstance instance_ = VK_NULL_HANDLE;
  VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  uint32_t queue_family_ = 0;
  VkPhysicalDeviceProperties device_properties_{};
  VkPhysicalDeviceMemoryProperties memory_properties_{};
  VkSampler sampler_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout descriptor_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet descriptor_set_ = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  VkCommandPool command_pool_ = VK_NULL_HANDLE;
  VkCommandBuffer command_buffer_ = VK_NULL_HANDLE;
  VkFence fence_ = VK_NULL_HANDLE;
  PFN_vkGetMemoryFdPropertiesKHR get_memory_fd_properties_ = nullptr;
  std::unordered_map<uint64_t, SourceResource> source_resources_;
  std::unordered_map<uint64_t, DestinationResource> destination_resources_;
  uint64_t cache_use_counter_ = 0;
  uint64_t source_cache_hits_ = 0;
  uint64_t source_cache_misses_ = 0;
  uint64_t destination_cache_hits_ = 0;
  uint64_t destination_cache_misses_ = 0;
};

VulkanFrameConverter::VulkanFrameConverter()
    : impl_(std::make_unique<Impl>()) {}

VulkanFrameConverter::~VulkanFrameConverter() = default;

c2_status_t VulkanFrameConverter::Initialize() { return impl_->Initialize(); }

c2_status_t VulkanFrameConverter::Convert(const C2ConstGraphicBlock &source,
                                          const MinigbmDmaBuf &destination) {
  return impl_->Convert(source, destination);
}

void VulkanFrameConverter::Reset() { impl_->Reset(); }

} // namespace floral::codec
