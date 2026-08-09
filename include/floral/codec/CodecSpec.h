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

#include <C2Component.h>

#include <cstdint>
#include <string>
#include <vector>

namespace floral::codec {

enum class CodecDirection {
  kDecode,
  kEncode,
};

struct CodecSpec {
  const char *component_name;
  const char *media_type;
  const char *ffmpeg_name;
  int codec_id;
  CodecDirection direction;
  std::vector<int> va_profiles;
  uint32_t max_width;
  uint32_t max_height;
};

const std::vector<CodecSpec> &GetCodecSpecs();

inline C2Component::kind_t ToC2Kind(CodecDirection direction) {
  return direction == CodecDirection::kEncode ? C2Component::KIND_ENCODER
                                              : C2Component::KIND_DECODER;
}

} // namespace floral::codec
