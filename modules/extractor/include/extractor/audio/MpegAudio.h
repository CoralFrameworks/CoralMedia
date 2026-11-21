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

#ifndef CORAL_EXTRACTOR_MPEG_AUDIO_H
#define CORAL_EXTRACTOR_MPEG_AUDIO_H

#include <cstdint>
#include <string>

namespace coral {

/**
 * The header of an MPEG audio frame, i.e. of an MP3 frame or of an ADTS AAC frame.
 *
 * <p>This is the extractor module counterpart of ExoPlayer's {@code MpegAudioUtil.Header}. The two
 * formats share the same sync word, the same version and layer encoding and the same bitrate and
 * sample rate tables, which is why the parsing lives in a single place.
 */
class AVMpegAudioHeader final {
public:
  /** The version of the MPEG stream. */
  enum class Version {
    /** MPEG-1. */
    kVersion1,
    /** MPEG-2. */
    kVersion2,
    /** MPEG-2.5, an unofficial extension of MPEG-2. */
    kVersion25,
  };

  AVMpegAudioHeader() noexcept = default;

  AVMpegAudioHeader(const AVMpegAudioHeader& other) = default;

  AVMpegAudioHeader& operator=(const AVMpegAudioHeader& other) = default;

  ~AVMpegAudioHeader() = default;

  /**
   * Parses a frame header and fills in the fields of this object.
   *
   * @param header The 32 bit header, read most significant byte first, with the 11 sync bits in the
   *               most significant bits.
   *
   * @return {@code true} when the header is valid.
   */
  bool setForHeaderData(uint32_t header);

  /**
   * Returns the size of the frame in bytes, including the header.
   */
  [[nodiscard]] int getFrameSize() const;

  /**
   * Returns the number of PCM samples per channel of one frame.
   */
  [[nodiscard]] int getSamplesPerFrame() const;

  /**
   * Returns the mime type of the audio of a frame with this header.
   */
  [[nodiscard]] std::string getMimeType() const;

  /**
   * Returns whether the two headers describe the same stream.
   */
  [[nodiscard]] bool isCompatible(const AVMpegAudioHeader& other) const {
    return version == other.version && layer == other.layer && sampleRate == other.sampleRate &&
           channels == other.channels;
  }

  /** The version of the MPEG stream. */
  Version version = Version::kVersion1;
  /** The layer of the MPEG audio: 1, 2 or 3. */
  int layer = 0;
  /** Whether the frame is protected by a CRC. */
  bool protectionAbsent = false;
  /** The bitrate of the frame, in bits per second, or -1 for a free format frame. */
  int bitrate = -1;
  /** The sample rate of the frame, in Hz. */
  int sampleRate = -1;
  /** Whether the frame contains padding. */
  bool padding = false;
  /** The number of channels of the frame: 1 for mono, 2 otherwise. */
  int channels = 2;
  /** The channel mode of the frame: 0 stereo, 1 joint stereo, 2 dual channel, 3 mono. */
  int channelMode = 0;

  /** The maximum size of a frame, in bytes. */
  static constexpr int kMaxFrameSize = 4096;

  /** The maximum number of bytes of side data of a frame. */
  static constexpr int kMaxFrameSideDataSize = 32;
};

} // namespace coral

#endif //CORAL_EXTRACTOR_MPEG_AUDIO_H
