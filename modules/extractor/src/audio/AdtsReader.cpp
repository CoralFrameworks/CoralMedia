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

#include <extractor/audio/AdtsReader.h>

namespace coral {

namespace {

/**
 * The sample rates of the AAC streams, indexed by the sampling frequency index.
 */
constexpr int kSampleRates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                22050, 16000, 12000, 11025, 8000,  7350};

constexpr int kSampleRateCount = static_cast<int>(sizeof(kSampleRates) / sizeof(kSampleRates[0]));

} // namespace anonymous

bool AVAdtsHeader::setForHeaderData(const uint8_t data[], const int offset) {
  // The sync word is 12 bits long: 0xFFF.
  if (data[offset] != 0xFF || (data[offset + 1] & 0xF0) != 0xF0) {
    return false;
  }
  // The layer must be 0.
  if ((data[offset + 1] & 0x06) != 0) {
    return false;
  }

  protectionAbsent = (data[offset + 1] & 0x01) == 1;

  profile = ((data[offset + 2] & 0xC0) >> 6) + 1;
  samplingFrequencyIndex_ = (data[offset + 2] & 0x3C) >> 2;
  if (samplingFrequencyIndex_ >= kSampleRateCount) {
    return false;
  }
  channelConfig = ((data[offset + 2] & 0x01) << 2) | ((data[offset + 3] & 0xC0) >> 6);
  if (channelConfig == 0) {
    // The channel configuration is described by a program config element, which this extractor
    // does not parse.
    return false;
  }

  frameLength_ = ((data[offset + 3] & 0x03) << 11) | (data[offset + 4] << 3) |
                 ((data[offset + 5] & 0xE0) >> 5);
  if (frameLength_ < kHeaderSize) {
    return false;
  }
  numberOfRawDataBlocks_ = (data[offset + 6] & 0x03) + 1;
  sampleLength_ = frameLength_ - (protectionAbsent ? kHeaderSize : kHeaderSizeWithCrc);
  return sampleLength_ > 0;
}

int AVAdtsHeader::getSampleRate() const {
  if (samplingFrequencyIndex_ < 0 || samplingFrequencyIndex_ >= kSampleRateCount) {
    return -1;
  }
  return kSampleRates[samplingFrequencyIndex_];
}

} // namespace coral
