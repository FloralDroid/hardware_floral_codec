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

#include <memory>
#include <string>
#include <vector>

namespace floral::codec {

class CapabilityProbe {
public:
  static std::unique_ptr<CapabilityProbe> Create(const std::string &devicePath);

  ~CapabilityProbe();

  bool Supports(const CodecSpec &spec) const;
  const std::string &devicePath() const;

private:
  struct Impl;

  explicit CapabilityProbe(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

std::string GetVaapiDevicePath();
std::vector<const CodecSpec *>
GetSupportedCodecSpecs(const CapabilityProbe &probe);

} // namespace floral::codec
