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

#pragma once

#include "floral/codec/CodecSpec.h"

#include <C2Component.h>
#include <util/C2InterfaceHelper.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace floral::codec {

class FloralCodecStore : public C2ComponentStore {
public:
  static std::shared_ptr<C2ComponentStore> Create();

  ~FloralCodecStore() override = default;

  C2String getName() const override;
  std::vector<std::shared_ptr<const C2Component::Traits>>
  listComponents() override;
  std::shared_ptr<C2ParamReflector> getParamReflector() const override;
  c2_status_t
  createComponent(C2String name,
                  std::shared_ptr<C2Component> *const component) override;
  c2_status_t createInterface(
      C2String name,
      std::shared_ptr<C2ComponentInterface> *const interface) override;
  c2_status_t copyBuffer(std::shared_ptr<C2GraphicBuffer> source,
                         std::shared_ptr<C2GraphicBuffer> destination) override;
  c2_status_t query_sm(
      const std::vector<C2Param *> &stackParams,
      const std::vector<C2Param::Index> &heapParamIndices,
      std::vector<std::unique_ptr<C2Param>> *const heapParams) const override;
  c2_status_t config_sm(
      const std::vector<C2Param *> &params,
      std::vector<std::unique_ptr<C2SettingResult>> *const failures) override;
  c2_status_t querySupportedParams_nb(
      std::vector<std::shared_ptr<C2ParamDescriptor>> *const params)
      const override;
  c2_status_t querySupportedValues_sm(
      std::vector<C2FieldSupportedValuesQuery> &fields) const override;

private:
  FloralCodecStore();

  std::shared_ptr<C2ReflectorHelper> reflector_;
  std::string device_path_;
  std::map<C2String, const CodecSpec *> specs_;
  std::vector<std::shared_ptr<const C2Component::Traits>> traits_;
};

} // namespace floral::codec
