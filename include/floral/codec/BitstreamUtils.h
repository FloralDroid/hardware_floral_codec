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

#include <cstddef>
#include <cstdint>
#include <vector>

namespace floral::codec {

enum class BitstreamCodec {
  kAvc,
  kHevc,
};

// Converts Android's codec configuration records or raw NAL units to the
// Annex-B form accepted by the FFmpeg decoder. The detected length size is
// returned for subsequent length-prefixed access units.
bool NormalizeCodecConfig(BitstreamCodec codec, const uint8_t *data,
                          size_t size, uint8_t preferredNalLengthSize,
                          std::vector<uint8_t> *output,
                          uint8_t *detectedNalLengthSize);

// Converts a length-prefixed access unit to Annex-B. Existing Annex-B input is
// validated and copied. Raw single-NAL input is accepted for compatibility
// with producers that omit both a start code and a length prefix.
bool NormalizeAccessUnit(BitstreamCodec codec, const uint8_t *data,
                         size_t size, uint8_t preferredNalLengthSize,
                         std::vector<uint8_t> *output,
                         uint8_t *detectedNalLengthSize);

} // namespace floral::codec
