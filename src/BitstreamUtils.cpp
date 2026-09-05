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

#include "floral/codec/BitstreamUtils.h"

#include <array>
#include <limits>

namespace floral::codec {
namespace {

constexpr uint8_t kDefaultNalLengthSize = 4;

struct StartCode {
  size_t offset;
  size_t size;
};

bool FindStartCode(const uint8_t *data, size_t size, size_t offset,
                   StartCode *result) {
  if (data == nullptr || result == nullptr) {
    return false;
  }
  for (size_t index = offset; index + 3 <= size; ++index) {
    if (data[index] != 0 || data[index + 1] != 0) {
      continue;
    }
    if (data[index + 2] == 1) {
      *result = {index, 3};
      return true;
    }
    if (index + 4 <= size && data[index + 2] == 0 &&
        data[index + 3] == 1) {
      *result = {index, 4};
      return true;
    }
  }
  return false;
}

bool IsValidNalHeader(BitstreamCodec codec, const uint8_t *data, size_t size) {
  if (data == nullptr || size == 0 || (data[0] & 0x80) != 0) {
    return false;
  }
  if (codec == BitstreamCodec::kAvc) {
    const uint8_t type = data[0] & 0x1f;
    return type != 0 && type <= 23;
  }
  return size >= 2 && (data[1] & 7) != 0 &&
         ((data[0] >> 1) & 0x3f) <= 63;
}

bool AppendNal(const uint8_t *data, size_t size, BitstreamCodec codec,
               std::vector<uint8_t> *output) {
  if (!IsValidNalHeader(codec, data, size) || output == nullptr ||
      output->size() > std::numeric_limits<size_t>::max() - 4 ||
      size > std::numeric_limits<size_t>::max() - output->size() - 4) {
    return false;
  }
  output->insert(output->end(), {0, 0, 0, 1});
  output->insert(output->end(), data, data + size);
  return true;
}

bool NormalizeAnnexB(BitstreamCodec codec, const uint8_t *data, size_t size,
                     std::vector<uint8_t> *output) {
  output->clear();
  StartCode current{};
  if (!FindStartCode(data, size, 0, &current)) {
    return false;
  }
  while (true) {
    const size_t nalStart = current.offset + current.size;
    StartCode next{};
    const bool hasNext = FindStartCode(data, size, nalStart, &next);
    size_t nalEnd = hasNext ? next.offset : size;
    while (nalEnd > nalStart && data[nalEnd - 1] == 0) {
      --nalEnd;
    }
    if (nalEnd <= nalStart ||
        !AppendNal(data + nalStart, nalEnd - nalStart, codec, output)) {
      output->clear();
      return false;
    }
    if (!hasNext) {
      return true;
    }
    current = next;
  }
}

bool ConvertLengthPrefixed(BitstreamCodec codec, const uint8_t *data,
                           size_t size, uint8_t nalLengthSize,
                           std::vector<uint8_t> *output) {
  if (data == nullptr || output == nullptr || nalLengthSize < 1 ||
      nalLengthSize > 4) {
    return false;
  }
  output->clear();
  size_t offset = 0;
  while (offset < size) {
    if (size - offset < nalLengthSize) {
      output->clear();
      return false;
    }
    uint32_t nalSize = 0;
    for (uint8_t index = 0; index < nalLengthSize; ++index) {
      nalSize = (nalSize << 8) | data[offset + index];
    }
    offset += nalLengthSize;
    if (nalSize == 0 || nalSize > size - offset ||
        !AppendNal(data + offset, nalSize, codec, output)) {
      output->clear();
      return false;
    }
    offset += nalSize;
  }
  return !output->empty();
}

bool AppendAvccNal(const uint8_t *data, size_t size,
                   std::vector<uint8_t> *output) {
  return AppendNal(data, size, BitstreamCodec::kAvc, output);
}

bool ParseAvcc(const uint8_t *data, size_t size, std::vector<uint8_t> *output,
               uint8_t *nalLengthSize) {
  if (data == nullptr || output == nullptr || nalLengthSize == nullptr ||
      size < 7 || data[0] != 1 || (data[4] & 0xfc) != 0xfc) {
    return false;
  }
  output->clear();
  const uint8_t lengthSize = static_cast<uint8_t>((data[4] & 3) + 1);
  size_t offset = 6;
  const uint8_t spsCount = data[5] & 0x1f;
  for (uint8_t index = 0; index < spsCount; ++index) {
    if (size - offset < 2) {
      return false;
    }
    const uint16_t nalSize =
        static_cast<uint16_t>((data[offset] << 8) | data[offset + 1]);
    offset += 2;
    if (nalSize > size - offset ||
        !AppendAvccNal(data + offset, nalSize, output)) {
      output->clear();
      return false;
    }
    offset += nalSize;
  }
  if (offset >= size) {
    output->clear();
    return false;
  }
  const uint8_t ppsCount = data[offset++];
  for (uint8_t index = 0; index < ppsCount; ++index) {
    if (size - offset < 2) {
      output->clear();
      return false;
    }
    const uint16_t nalSize =
        static_cast<uint16_t>((data[offset] << 8) | data[offset + 1]);
    offset += 2;
    if (nalSize > size - offset ||
        !AppendAvccNal(data + offset, nalSize, output)) {
      output->clear();
      return false;
    }
    offset += nalSize;
  }
  if (output->empty()) {
    return false;
  }
  *nalLengthSize = lengthSize;
  return true;
}

bool ParseHvcc(const uint8_t *data, size_t size, std::vector<uint8_t> *output,
               uint8_t *nalLengthSize) {
  if (data == nullptr || output == nullptr || nalLengthSize == nullptr ||
      size < 23 || data[0] != 1) {
    return false;
  }
  size_t offset = 21;
  const uint8_t lengthSize = static_cast<uint8_t>((data[offset] & 3) + 1);
  ++offset;
  const uint8_t arrayCount = data[offset++];
  output->clear();
  for (uint8_t array = 0; array < arrayCount; ++array) {
    if (size - offset < 3) {
      output->clear();
      return false;
    }
    ++offset; // array completeness, reserved bit and NAL unit type
    const uint16_t nalCount =
        static_cast<uint16_t>((data[offset] << 8) | data[offset + 1]);
    offset += 2;
    for (uint16_t index = 0; index < nalCount; ++index) {
      if (size - offset < 2) {
        output->clear();
        return false;
      }
      const uint16_t nalSize =
          static_cast<uint16_t>((data[offset] << 8) | data[offset + 1]);
      offset += 2;
      if (nalSize > size - offset ||
          !AppendNal(data + offset, nalSize, BitstreamCodec::kHevc, output)) {
        output->clear();
        return false;
      }
      offset += nalSize;
    }
  }
  if (output->empty()) {
    return false;
  }
  *nalLengthSize = lengthSize;
  return true;
}

bool NormalizeRawNal(BitstreamCodec codec, const uint8_t *data, size_t size,
                     std::vector<uint8_t> *output) {
  output->clear();
  return AppendNal(data, size, codec, output);
}

bool TryLengthSizes(BitstreamCodec codec, const uint8_t *data, size_t size,
                    uint8_t preferredNalLengthSize,
                    std::vector<uint8_t> *output,
                    uint8_t *detectedNalLengthSize) {
  const uint8_t preferred = preferredNalLengthSize >= 1 &&
                                    preferredNalLengthSize <= 4
                                ? preferredNalLengthSize
                                : kDefaultNalLengthSize;
  const std::array<uint8_t, 4> candidates = {preferred, 4, 2, 1};
  std::array<bool, 5> attempted{};
  for (uint8_t candidate : candidates) {
    if (candidate == 0 || attempted[candidate]) {
      continue;
    }
    attempted[candidate] = true;
    if (ConvertLengthPrefixed(codec, data, size, candidate, output)) {
      if (detectedNalLengthSize != nullptr) {
        *detectedNalLengthSize = candidate;
      }
      return true;
    }
  }
  return false;
}

bool Normalize(BitstreamCodec codec, const uint8_t *data, size_t size,
               uint8_t preferredNalLengthSize, std::vector<uint8_t> *output,
               uint8_t *detectedNalLengthSize, bool config) {
  if (data == nullptr || size == 0 || output == nullptr) {
    return false;
  }
  if (config && codec == BitstreamCodec::kAvc && data[0] == 1 &&
      size >= 7) {
    return ParseAvcc(data, size, output, detectedNalLengthSize);
  }
  if (config && codec == BitstreamCodec::kHevc && data[0] == 1 &&
      size >= 23) {
    return ParseHvcc(data, size, output, detectedNalLengthSize);
  }
  if (NormalizeAnnexB(codec, data, size, output)) {
    if (detectedNalLengthSize != nullptr) {
      *detectedNalLengthSize = preferredNalLengthSize >= 1 &&
                                       preferredNalLengthSize <= 4
                                   ? preferredNalLengthSize
                                   : kDefaultNalLengthSize;
    }
    return true;
  }
  if (config) {
    if (TryLengthSizes(codec, data, size, preferredNalLengthSize, output,
                       detectedNalLengthSize)) {
      return true;
    }
    if (NormalizeRawNal(codec, data, size, output)) {
      if (detectedNalLengthSize != nullptr) {
        *detectedNalLengthSize = preferredNalLengthSize >= 1 &&
                                         preferredNalLengthSize <= 4
                                     ? preferredNalLengthSize
                                     : kDefaultNalLengthSize;
      }
      return true;
    }
    return false;
  }
  if (TryLengthSizes(codec, data, size, preferredNalLengthSize, output,
                     detectedNalLengthSize)) {
    return true;
  }
  if (NormalizeRawNal(codec, data, size, output)) {
    if (detectedNalLengthSize != nullptr) {
      *detectedNalLengthSize = preferredNalLengthSize >= 1 &&
                                       preferredNalLengthSize <= 4
                                   ? preferredNalLengthSize
                                   : kDefaultNalLengthSize;
    }
    return true;
  }
  return false;
}

} // namespace

bool NormalizeCodecConfig(BitstreamCodec codec, const uint8_t *data,
                          size_t size, uint8_t preferredNalLengthSize,
                          std::vector<uint8_t> *output,
                          uint8_t *detectedNalLengthSize) {
  return Normalize(codec, data, size, preferredNalLengthSize, output,
                   detectedNalLengthSize, true);
}

bool NormalizeAccessUnit(BitstreamCodec codec, const uint8_t *data,
                         size_t size, uint8_t preferredNalLengthSize,
                         std::vector<uint8_t> *output,
                         uint8_t *detectedNalLengthSize) {
  return Normalize(codec, data, size, preferredNalLengthSize, output,
                   detectedNalLengthSize, false);
}

} // namespace floral::codec
