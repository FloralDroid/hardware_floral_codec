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

#include <functional>
#include <memory>
#include <string>

namespace floral::codec {

c2_status_t
CreateFloralCodecComponent(const CodecSpec &spec, const std::string &devicePath,
                           const std::shared_ptr<C2ReflectorHelper> &reflector,
                           c2_node_id_t id,
                           std::shared_ptr<C2Component> *component,
                           std::function<void(C2Component *)> deleter =
                               std::default_delete<C2Component>());

c2_status_t CreateFloralCodecInterface(
    const CodecSpec &spec, const std::shared_ptr<C2ReflectorHelper> &reflector,
    c2_node_id_t id, std::shared_ptr<C2ComponentInterface> *interface,
    std::function<void(C2ComponentInterface *)> deleter =
        std::default_delete<C2ComponentInterface>());

} // namespace floral::codec
