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

#define LOG_TAG "FloralCodecStore"

#include "floral/codec/FloralCodecStore.h"

#include "floral/codec/CapabilityProbe.h"
#include "floral/codec/FloralCodecComponent.h"

#include <log/log.h>

#include <mutex>
#include <utility>

namespace floral::codec {

std::shared_ptr<C2ComponentStore> FloralCodecStore::Create() {
  static std::mutex mutex;
  static std::weak_ptr<C2ComponentStore> weakStore;
  std::lock_guard<std::mutex> lock(mutex);
  std::shared_ptr<C2ComponentStore> store = weakStore.lock();
  if (store == nullptr) {
    store = std::shared_ptr<C2ComponentStore>(new FloralCodecStore());
    weakStore = store;
  }
  return store;
}

FloralCodecStore::FloralCodecStore()
    : reflector_(std::make_shared<C2ReflectorHelper>()),
      device_path_(GetVaapiDevicePath()) {
  std::unique_ptr<CapabilityProbe> probe =
      CapabilityProbe::Create(device_path_);
  if (probe == nullptr) {
    ALOGW("no Floral hardware codecs will be registered");
    return;
  }

  for (const CodecSpec *spec : GetSupportedCodecSpecs(*probe)) {
    specs_.emplace(spec->component_name, spec);
    auto traits = std::make_shared<C2Component::Traits>();
    traits->name = spec->component_name;
    traits->domain = C2Component::DOMAIN_VIDEO;
    traits->kind = ToC2Kind(spec->direction);
    traits->rank = 128;
    traits->mediaType = spec->media_type;
    traits->owner = "vendor";
    traits_.push_back(std::move(traits));
  }
}

C2String FloralCodecStore::getName() const {
  return "android.componentStore.floral";
}

std::vector<std::shared_ptr<const C2Component::Traits>>
FloralCodecStore::listComponents() {
  return traits_;
}

std::shared_ptr<C2ParamReflector> FloralCodecStore::getParamReflector() const {
  return reflector_;
}

c2_status_t FloralCodecStore::createComponent(
    C2String name, std::shared_ptr<C2Component> *const component) {
  if (component == nullptr) {
    return C2_BAD_VALUE;
  }
  component->reset();
  const auto found = specs_.find(name);
  if (found == specs_.end()) {
    return C2_NOT_FOUND;
  }
  return CreateFloralCodecComponent(*found->second, device_path_, reflector_, 0,
                                    component);
}

c2_status_t FloralCodecStore::createInterface(
    C2String name, std::shared_ptr<C2ComponentInterface> *const interface) {
  if (interface == nullptr) {
    return C2_BAD_VALUE;
  }
  interface->reset();
  const auto found = specs_.find(name);
  if (found == specs_.end()) {
    return C2_NOT_FOUND;
  }
  return CreateFloralCodecInterface(*found->second, reflector_, 0, interface);
}

c2_status_t FloralCodecStore::copyBuffer(std::shared_ptr<C2GraphicBuffer>,
                                         std::shared_ptr<C2GraphicBuffer>) {
  return C2_OMITTED;
}

c2_status_t
FloralCodecStore::query_sm(const std::vector<C2Param *> &stackParams,
                           const std::vector<C2Param::Index> &heapParamIndices,
                           std::vector<std::unique_ptr<C2Param>> *const) const {
  return stackParams.empty() && heapParamIndices.empty() ? C2_OK : C2_BAD_INDEX;
}

c2_status_t FloralCodecStore::config_sm(
    const std::vector<C2Param *> &params,
    std::vector<std::unique_ptr<C2SettingResult>> *const) {
  return params.empty() ? C2_OK : C2_BAD_INDEX;
}

c2_status_t FloralCodecStore::querySupportedParams_nb(
    std::vector<std::shared_ptr<C2ParamDescriptor>> *const params) const {
  if (params == nullptr) {
    return C2_BAD_VALUE;
  }
  params->clear();
  return C2_OK;
}

c2_status_t FloralCodecStore::querySupportedValues_sm(
    std::vector<C2FieldSupportedValuesQuery> &fields) const {
  return fields.empty() ? C2_OK : C2_BAD_INDEX;
}

} // namespace floral::codec
