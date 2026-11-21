/*
 * Copyright (C) 2026 The Coral Project
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

#include <extractor/Utility.h>
#include <extractor/audio/MpegAudio.h>

#include <media/Types.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

using coral::AVBitReader;
using coral::AVDataBuffer;
using coral::AVMpegAudioHeader;
using coral::AVUtil;

TEST(AVUtilTest, ReadsBigEndianIntegers) {
  const uint8_t data[] = {0x12, 0x34, 0x56, 0x78, 0x9A};
  EXPECT_EQ(0x12345678u, AVUtil::readUnsignedInt(data, 0));
  EXPECT_EQ(0x123456u, AVUtil::readUnsignedInt24(data, 0));
  EXPECT_EQ(0x345678u, AVUtil::readUnsignedInt24(data, 1));
  EXPECT_EQ(0x1234u, AVUtil::readUnsignedShort(data, 0));
}

TEST(AVUtilTest, ConvertsAScaleToMicroseconds) {
  EXPECT_EQ(1000000, AVUtil::scaleToUs(44100, 44100));
  EXPECT_EQ(500000, AVUtil::scaleToUs(22050, 44100));
  EXPECT_EQ(coral::Definitions::kTimeUnset, AVUtil::scaleToUs(1, 0));
}

TEST(AVUtilTest, ScaleLargeValueKeepsThePrecision) {
  EXPECT_EQ(1000, AVUtil::scaleLargeValue(1, 1, 1000));
  EXPECT_EQ(44100, AVUtil::scaleLargeValue(44100, 44100, 44100));
}

TEST(AVUtilTest, GcdAndLcm) {
  EXPECT_EQ(4, AVUtil::gcd(12, 8));
  EXPECT_EQ(1, AVUtil::gcd(0, 5));
  EXPECT_EQ(24, AVUtil::lcm(12, 8, 1000));
  EXPECT_EQ(10, AVUtil::lcm(10, 5, 10));
}

TEST(AVUtilTest, ConvertsLengthPrefixedNalUnitsToAnnexB) {
  // Two NAL units prefixed by their four byte length, as a sample of an MP4 file stores them.
  const std::vector<uint8_t> sample = {
    0x00, 0x00, 0x00, 0x03, 0x65, 0x88, 0x80,
    0x00, 0x00, 0x00, 0x02, 0x41, 0x9A,
  };

  const size_t convertedSize = AVUtil::annexBSize(sample.data(), sample.size(), 4);
  ASSERT_EQ(sample.size(), convertedSize);

  std::vector<uint8_t> converted(convertedSize);
  AVUtil::toAnnexB(sample.data(), sample.size(), 4, converted.data());

  const std::vector<uint8_t> expected = {
    0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x80,
    0x00, 0x00, 0x00, 0x01, 0x41, 0x9A,
  };
  EXPECT_EQ(expected, converted);
}

TEST(AVUtilTest, ConvertsTwoByteLengthPrefixesAsWell) {
  // A configuration record can declare a prefix of any size from one to four bytes.
  const std::vector<uint8_t> sample = {
    0x00, 0x03, 0x65, 0x88, 0x80,
    0x00, 0x02, 0x41, 0x9A,
  };

  const size_t convertedSize = AVUtil::annexBSize(sample.data(), sample.size(), 2);
  ASSERT_EQ(sample.size() + 4, convertedSize);

  std::vector<uint8_t> converted(convertedSize);
  AVUtil::toAnnexB(sample.data(), sample.size(), 2, converted.data());

  const std::vector<uint8_t> expected = {
    0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x80,
    0x00, 0x00, 0x00, 0x01, 0x41, 0x9A,
  };
  EXPECT_EQ(expected, converted);
}

TEST(AVUtilTest, DoesNotConvertASampleThatIsAlreadyFramedWithStartCodes) {
  // The frames of a decoder are given start codes: converting them again would corrupt them.
  const std::vector<uint8_t> sample = {0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x80};
  EXPECT_EQ(0u, AVUtil::annexBSize(sample.data(), sample.size(), 4));

  const std::vector<uint8_t> shortStartCode = {0x00, 0x00, 0x01, 0x67, 0x4D, 0x40};
  EXPECT_EQ(0u, AVUtil::annexBSize(shortStartCode.data(), shortStartCode.size(), 4));
}

TEST(AVUtilTest, DoesNotConvertASampleWhoseLengthsDoNotDescribeIt) {
  // The lengths of the prefixes have to describe the whole sample, otherwise the sample is not length
  // prefixed and converting it would produce garbage.
  const std::vector<uint8_t> tooLong = {0x00, 0x00, 0x00, 0x10, 0x65, 0x88};
  EXPECT_EQ(0u, AVUtil::annexBSize(tooLong.data(), tooLong.size(), 4));

  const std::vector<uint8_t> emptyUnit = {0x00, 0x00, 0x00, 0x00, 0x65, 0x88};
  EXPECT_EQ(0u, AVUtil::annexBSize(emptyUnit.data(), emptyUnit.size(), 4));

  const std::vector<uint8_t> trailingBytes = {0x00, 0x00, 0x00, 0x02, 0x65, 0x88, 0x80};
  EXPECT_EQ(0u, AVUtil::annexBSize(trailingBytes.data(), trailingBytes.size(), 4));

  // A prefix that is not a size at all, e.g. the length size the container declared is wrong.
  EXPECT_EQ(0u, AVUtil::annexBSize(trailingBytes.data(), trailingBytes.size(), 0));
}

TEST(AVUtilTest, IndexOfFindsAFourCc) {
  const uint8_t data[] = {'a', 'b', 'c', 'd', 'm', 'o', 'o', 'v', 'x'};
  EXPECT_EQ(4, AVUtil::indexOf(data, sizeof(data), 0x6D6F6F76));
  EXPECT_EQ(-1, AVUtil::indexOf(data, sizeof(data), 0x66747970));
  EXPECT_EQ(-1, AVUtil::indexOf(data, 2, 0x61626364));
}

TEST(AVUtilTest, StartsWithChecksTheFirstFourBytes) {
  const uint8_t data[] = {'f', 't', 'y', 'p', 'i', 's', 'o', 'm'};
  EXPECT_TRUE(AVUtil::startsWith(data, sizeof(data), 0x66747970));
  EXPECT_FALSE(AVUtil::startsWith(data, sizeof(data), 0x69736F6D));
  EXPECT_FALSE(AVUtil::startsWith(data, 2, 0x66747970));
}

// ==================================================================================================
// AVBitReader
// ==================================================================================================

TEST(AVBitReaderTest, ReadsBitsAcrossByteBoundaries) {
  const uint8_t data[] = {0b10110010, 0b01011100};
  AVBitReader reader(data, sizeof(data));

  EXPECT_EQ(0b101u, reader.readBits(3));
  EXPECT_EQ(0b10010u, reader.readBits(5));
  EXPECT_EQ(0b01011u, reader.readBits(5));
  EXPECT_EQ(13, reader.getPosition());
  EXPECT_EQ(0b100u, reader.readBits(3));
}

TEST(AVBitReaderTest, ReadsAThirtyTwoBitValue) {
  const uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF};
  AVBitReader reader(data, sizeof(data));
  EXPECT_EQ(0xDEADBEEFu, reader.readBits(32));
}

TEST(AVBitReaderTest, ReadsSignedValues) {
  const uint8_t data[] = {0b11111111, 0b00000001};
  AVBitReader reader(data, sizeof(data));
  EXPECT_EQ(-1, reader.readSignedBits(4));
  EXPECT_EQ(-1, reader.readSignedBits(4));
  EXPECT_EQ(1, reader.readBits(8));
}

TEST(AVBitReaderTest, ReportsTheAvailableBits) {
  const uint8_t data[] = {0x00, 0x00};
  AVBitReader reader(data, sizeof(data));
  EXPECT_EQ(16, reader.getTotalBits());
  EXPECT_TRUE(reader.canReadBits(16));
  EXPECT_FALSE(reader.canReadBits(17));
  reader.skipBits(10);
  EXPECT_TRUE(reader.canReadBits(6));
  EXPECT_FALSE(reader.canReadBits(7));
}

TEST(AVBitReaderTest, ReadingPastTheEndReturnsZero) {
  const uint8_t data[] = {0xAB};
  AVBitReader reader(data, sizeof(data));
  EXPECT_EQ(0xABu, reader.readBits(8));
  EXPECT_EQ(0u, reader.readBits(8));
}

TEST(AVBitReaderTest, ReadsTheAudioSpecificConfigOfAnAacLcStereoStream) {
  // An AudioSpecificConfig of AAC LC at 44100 Hz with two channels: 0x12 0x10.
  const uint8_t data[] = {0x12, 0x10};
  AVBitReader reader(data, sizeof(data));

  const int audioObjectType = static_cast<int>(reader.readBits(5));
  const int samplingFrequencyIndex = static_cast<int>(reader.readBits(4));
  const int channelConfiguration = static_cast<int>(reader.readBits(4));

  EXPECT_EQ(2, audioObjectType);
  EXPECT_EQ(4, samplingFrequencyIndex);
  EXPECT_EQ(2, channelConfiguration);
}

// ==================================================================================================
// AVMpegAudioHeader
// ==================================================================================================

TEST(AVMpegAudioHeaderTest, ParsesAnMpeg1Layer3Header) {
  AVMpegAudioHeader header;
  // MPEG-1, layer 3, no CRC, 128 kbit/s, 44100 Hz, no padding, stereo.
  ASSERT_TRUE(header.setForHeaderData(0xFFFB9000));

  EXPECT_EQ(AVMpegAudioHeader::Version::kVersion1, header.version);
  EXPECT_EQ(3, header.layer);
  EXPECT_EQ(128000, header.bitrate);
  EXPECT_EQ(44100, header.sampleRate);
  EXPECT_FALSE(header.padding);
  EXPECT_EQ(2, header.channels);
  EXPECT_EQ(417, header.getFrameSize());
  EXPECT_EQ(1152, header.getSamplesPerFrame());
  EXPECT_EQ(std::string(coral::MimeTypes::AUDIO_MPEG), header.getMimeType());
}

TEST(AVMpegAudioHeaderTest, ParsesAnMpeg2Layer3Header) {
  AVMpegAudioHeader header;
  // MPEG-2, layer 3, 128 kbit/s, 22050 Hz, stereo.
  ASSERT_TRUE(header.setForHeaderData(0xFFF3C000));

  EXPECT_EQ(AVMpegAudioHeader::Version::kVersion2, header.version);
  EXPECT_EQ(3, header.layer);
  EXPECT_EQ(128000, header.bitrate);
  EXPECT_EQ(22050, header.sampleRate);
  EXPECT_EQ(2, header.channels);
  EXPECT_EQ(576, header.getSamplesPerFrame());
  // 72 * 128000 / 22050 = 417 bytes.
  EXPECT_EQ(417, header.getFrameSize());
}

TEST(AVMpegAudioHeaderTest, RejectsAnInvalidSyncWord) {
  AVMpegAudioHeader header;
  EXPECT_FALSE(header.setForHeaderData(0x00FB9000));
}

TEST(AVMpegAudioHeaderTest, RejectsTheReservedVersion) {
  AVMpegAudioHeader header;
  EXPECT_FALSE(header.setForHeaderData(0xFFEB9000));
}

TEST(AVMpegAudioHeaderTest, RejectsTheReservedLayer) {
  AVMpegAudioHeader header;
  EXPECT_FALSE(header.setForHeaderData(0xFFF99000));
}

TEST(AVMpegAudioHeaderTest, RejectsTheReservedSampleRate) {
  AVMpegAudioHeader header;
  // The sample rate index is 3, which is reserved.
  EXPECT_FALSE(header.setForHeaderData(0xFFFBBC00));
}

TEST(AVMpegAudioHeaderTest, PaddingIncreasesTheFrameSize) {
  AVMpegAudioHeader header;
  ASSERT_TRUE(header.setForHeaderData(0xFFFB9200));
  EXPECT_EQ(418, header.getFrameSize());
}

TEST(AVMpegAudioHeaderTest, CompatibilityComparesTheStreamParameters) {
  AVMpegAudioHeader first;
  AVMpegAudioHeader second;
  ASSERT_TRUE(first.setForHeaderData(0xFFFB9000));
  ASSERT_TRUE(second.setForHeaderData(0xFFFB9200));
  EXPECT_TRUE(first.isCompatible(second));

  AVMpegAudioHeader third;
  ASSERT_TRUE(third.setForHeaderData(0xFFF38000));
  EXPECT_FALSE(first.isCompatible(third));
}
