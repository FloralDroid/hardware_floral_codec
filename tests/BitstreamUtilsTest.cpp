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

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

namespace floral::codec {
namespace {

TEST(BitstreamUtilsTest, ConvertsAvccAccessUnitToAnnexB) {
  const std::vector<uint8_t> avcc = {0, 0, 0, 3, 0x65, 1, 2,
                                     0, 0, 0, 2, 0x41, 3};
  std::vector<uint8_t> annexB;
  uint8_t nalLengthSize = 4;
  ASSERT_TRUE(NormalizeAccessUnit(BitstreamCodec::kAvc, avcc.data(),
                                  avcc.size(), nalLengthSize, &annexB,
                                  &nalLengthSize));
  const std::vector<uint8_t> expected = {0, 0, 0, 1, 0x65, 1, 2,
                                         0, 0, 0, 1, 0x41, 3};
  EXPECT_EQ(expected, annexB);
  EXPECT_EQ(4, nalLengthSize);
}

TEST(BitstreamUtilsTest, ConvertsTwoByteLengthPrefixedAccessUnit) {
  const std::vector<uint8_t> avcc = {0, 3, 0x65, 1, 2, 0, 2, 0x41, 3};
  std::vector<uint8_t> annexB;
  uint8_t nalLengthSize = 2;
  ASSERT_TRUE(NormalizeAccessUnit(BitstreamCodec::kAvc, avcc.data(),
                                  avcc.size(), nalLengthSize, &annexB,
                                  &nalLengthSize));
  const std::vector<uint8_t> expected = {0, 0, 0, 1, 0x65, 1, 2,
                                         0, 0, 0, 1, 0x41, 3};
  EXPECT_EQ(expected, annexB);
  EXPECT_EQ(2, nalLengthSize);
}

TEST(BitstreamUtilsTest, ConvertsHevcAccessUnitToAnnexB) {
  const std::vector<uint8_t> hvcc = {0, 0, 0, 2, 0x40, 1,
                                     0, 0, 0, 2, 0x02, 1};
  std::vector<uint8_t> annexB;
  ASSERT_TRUE(NormalizeAccessUnit(BitstreamCodec::kHevc, hvcc.data(),
                                  hvcc.size(), 4, &annexB, nullptr));
  const std::vector<uint8_t> expected = {0, 0, 0, 1, 0x40, 1,
                                         0, 0, 0, 1, 0x02, 1};
  EXPECT_EQ(expected, annexB);
}

TEST(BitstreamUtilsTest, AcceptsRawSingleNalAccessUnit) {
  const std::vector<uint8_t> raw = {0x65, 1, 2, 3};
  std::vector<uint8_t> annexB;
  ASSERT_TRUE(NormalizeAccessUnit(BitstreamCodec::kAvc, raw.data(), raw.size(),
                                  4, &annexB, nullptr));
  const std::vector<uint8_t> expected = {0, 0, 0, 1, 0x65, 1, 2, 3};
  EXPECT_EQ(expected, annexB);
}

TEST(BitstreamUtilsTest, ExtractsAvccConfigurationRecord) {
  const std::vector<uint8_t> avcc = {
      1, 0x64, 0, 0x33, 0xff, 0xe1, 0, 3, 0x67, 0x64, 0x33,
      1, 0, 2, 0x68, 0xee,
  };
  std::vector<uint8_t> annexB;
  uint8_t nalLengthSize = 0;
  ASSERT_TRUE(NormalizeCodecConfig(BitstreamCodec::kAvc, avcc.data(),
                                   avcc.size(), 4, &annexB,
                                   &nalLengthSize));
  const std::vector<uint8_t> expected = {0, 0, 0, 1, 0x67, 0x64, 0x33,
                                         0, 0, 0, 1, 0x68, 0xee};
  EXPECT_EQ(expected, annexB);
  EXPECT_EQ(4, nalLengthSize);
}

TEST(BitstreamUtilsTest, ExtractsHvccConfigurationRecord) {
  const std::vector<uint8_t> hvcc = {
      1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x78, 0xf0, 0,
      0xfc, 0xfc, 0xf8, 0xf8, 0, 0, 3, 3,
      0xa0, 0, 1, 0, 2, 0x40, 1,
      0xa1, 0, 1, 0, 2, 0x42, 1,
      0xa2, 0, 1, 0, 2, 0x44, 1,
  };
  std::vector<uint8_t> annexB;
  uint8_t nalLengthSize = 0;
  ASSERT_TRUE(NormalizeCodecConfig(BitstreamCodec::kHevc, hvcc.data(),
                                   hvcc.size(), 4, &annexB,
                                   &nalLengthSize));
  const std::vector<uint8_t> expected = {
      0, 0, 0, 1, 0x40, 1,
      0, 0, 0, 1, 0x42, 1,
      0, 0, 0, 1, 0x44, 1,
  };
  EXPECT_EQ(expected, annexB);
  EXPECT_EQ(4, nalLengthSize);
}

TEST(BitstreamUtilsTest, ValidatesAndCopiesAnnexB) {
  const std::vector<uint8_t> input = {0, 0, 1, 0x67, 0x64, 0x33,
                                      0, 0, 0, 1, 0x68, 0xee};
  std::vector<uint8_t> output;
  ASSERT_TRUE(NormalizeAccessUnit(BitstreamCodec::kAvc, input.data(),
                                  input.size(), 4, &output, nullptr));
  const std::vector<uint8_t> expected = {0, 0, 0, 1, 0x67, 0x64, 0x33,
                                         0, 0, 0, 1, 0x68, 0xee};
  EXPECT_EQ(expected, output);
}

TEST(BitstreamUtilsTest, RejectsTruncatedLengthPrefixedNal) {
  const std::vector<uint8_t> malformed = {0, 0, 0, 8, 0x65, 1, 2};
  std::vector<uint8_t> output;
  EXPECT_FALSE(NormalizeAccessUnit(BitstreamCodec::kAvc, malformed.data(),
                                   malformed.size(), 4, &output, nullptr));
}

} // namespace
} // namespace floral::codec
