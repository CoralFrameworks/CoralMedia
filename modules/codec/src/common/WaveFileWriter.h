/*
 * Copyright (C) 2024 The Coral Project
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

#ifndef CORAL_CODEC_WAVE_FILE_WRITER_H
#define CORAL_CODEC_WAVE_FILE_WRITER_H

#include <codec/AudioFileWriter.h>

#include <string>

namespace coral {

/**
 * The implementation of the AudioFileWriter to write pcm data to a wav file.
 */
class WaveFileWriter final : public AudioFileWriter {
public:
  static std::unique_ptr<WaveFileWriter>
  sniff(const std::string& outputUrl, const std::shared_ptr<MediaFormat>& format);

  explicit WaveFileWriter(const std::string& outputUrl,
      uint64_t sampleRate,
      uint32_t channelCount,
      AudioSampleFormat sampleFormat) noexcept;

  ~WaveFileWriter() override;

  bool open(Error** outError) override;

  void close() override;

  int64_t writeFromPCMBuffer(void* inputBuffer, size_t numFrames, Error** outError) override;

  int64_t getSampleLength() const override;

  int64_t getFramePosition() const override;

  static const char* getName() { return "AudioWavFileWriter"; }

private:
  template <typename T>
  void writeIntSamples(const void *buf, size_t n, float scale) {
    auto *p = static_cast<const T *>(buf);
    for (size_t i = 0; i < n; ++i) {
      write(static_cast<float>(p[i]) * scale);
    }
  }

  /** Write single audio data value to the WAV file. */
  void write(float value);

  /**
   * Write a buffer to the WAV file.
   */
  void write(const float* buffer, size_t startSample, size_t numSamples);

  /**
   * Write a 32-bit integer to the stream in Little Endian format.
   */
  void writeIntLittle(int32_t n);

  /**
   * Write a 16-bit integer to the stream in Little Endian format.
   */
  void writeShortLittle(int16_t n);

  /**
   * Write a 'fmt' chunk to the WAV file containing the given information.
   */
  void writeFormatChunk();

  /**
   * Write a 'data' chunk header to the WAV file. This should be followed by
   * call to writeShortLittle() to write the data to the chunk.
   *
   * @param dataSize the size of the data chunk in bytes.
   */
  void writeDataChunkHeader(int64_t dataSize);

  /**
   * Write a simple WAV header for PCM data.
   */
  void writeHeader();

  // Write lower 8 bits. Upper bits ignored.
  void writeByte(uint8_t b);

  void writePCM24(float value);

  void writePCM16(float value);

  /**
   * Write a 'RIFF' file header and a 'WAVE' ID to the WAV file.
   *
   * @param fileLength the size of the file in bytes, excluding the 'RIFF' identifier
   *                   and the length field itself.
   */
  void writeRiffHeader(int64_t fileLength);

  void updateHeaderInformation();

  int32_t getBytesPerFrame() const {
    return mBytesPerFrame;
  }

  /**
   * For stereo, set this to 2. Default is mono = 1.
   * Also known as ChannelCount
   */
  int32_t getSamplesPerFrame() const {
    return getChannelCount();
  }

  /**
   * Only 16 or 24 bit samples supported at the moment. Default is 16.
   */
  int32_t getBitsPerSample() const {
    return mBitsPerSample;
  }

  static constexpr int32_t PCM24_MIN = -(1 << 23);
  static constexpr int32_t PCM24_MAX = (1 << 23) - 1;

  static constexpr int BITS_PER_BYTE = 8;
  static constexpr int WAVE_FORMAT_PCM = 1;

  //!< The size of the canonical wav header: 'RIFF', 'fmt ' (16 bytes) and 'data'.
  static constexpr int64_t WAV_HEADER_SIZE = 44;
  //!< The size of the 'RIFF' and 'WAVE' identifiers.
  static constexpr int64_t RIFF_HEADER_SIZE = 8;

  std::string mOutputUrl;
  FILE* mFile;
  int32_t mBitsPerSample;
  int32_t mBytesPerFrame;
  int64_t bytesWritten = 0;
  bool headerWritten = false;
};

} // namespace coral

#endif // CORAL_CODEC_WAVE_FILE_WRITER_H
