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

#ifndef CORAL_EXTRACTOR_ADTS_READER_H
#define CORAL_EXTRACTOR_ADTS_READER_H

#include <cstdint>

namespace coral {

/**
 * The header of an ADTS frame, i.e. of an AAC frame that is framed for a byte stream.
 *
 * <p>This is the extractor module counterpart of ExoPlayer's {@code AdtsReader} header parsing.
 */
class AVAdtsHeader final {
public:
  AVAdtsHeader() noexcept = default;

  AVAdtsHeader(const AVAdtsHeader& other) = default;

  AVAdtsHeader& operator=(const AVAdtsHeader& other) = default;

  ~AVAdtsHeader() = default;

  /**
   * Parses the seven bytes of a frame header.
   *
   * @param data The buffer holding the header.
   * @param offset The offset of the header in the buffer.
   *
   * @return {@code true} when the header is valid.
   */
  bool setForHeaderData(const uint8_t data[], int offset);

  /**
   * Returns the sample rate index of the AAC stream, i.e. the {@code samplingFrequencyIndex} of the
   * AudioSpecificConfig.
   */
  [[nodiscard]] int getSamplingFrequencyIndex() const { return samplingFrequencyIndex_; }

  /**
   * Returns the sample rate of the AAC stream, in Hz.
   */
  [[nodiscard]] int getSampleRate() const;

  /**
   * Returns the number of bytes of the frame, including the header.
   */
  [[nodiscard]] int getFrameLength() const { return frameLength_; }

  /**
   * Returns the number of bytes of the payload of the frame, excluding the header.
   */
  [[nodiscard]] int getSampleLength() const { return sampleLength_; }

  /**
   * Returns the number of raw data blocks of the frame.
   */
  [[nodiscard]] int getNumberOfRawDataBlocks() const { return numberOfRawDataBlocks_; }

  /**
   * Returns the number of PCM samples per channel of the frame.
   */
  [[nodiscard]] int getSamplesPerFrame() const { return 1024 * numberOfRawDataBlocks_; }

  /** Whether the frame has no CRC. */
  bool protectionAbsent = false;
  /** The MPEG-4 audio object type: 1 for AAC main, 2 for AAC LC, 4 for AAC LTP. */
  int profile = 0;
  /** The channel configuration, 0 when it is described by a program config element. */
  int channelConfig = 0;

  /** The size of the header, in bytes. */
  static constexpr int kHeaderSize = 7;
  /** The size of the header when it is protected by a CRC. */
  static constexpr int kHeaderSizeWithCrc = 9;

private:
  int samplingFrequencyIndex_ = -1;
  int frameLength_ = 0;
  int sampleLength_ = 0;
  int numberOfRawDataBlocks_ = 1;
};

} // namespace coral

#endif //CORAL_EXTRACTOR_ADTS_READER_H
