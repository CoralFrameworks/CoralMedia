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

#ifndef CORAL_CODEC_WAVE_FILE_READER_H
#define CORAL_CODEC_WAVE_FILE_READER_H

#include <codec/AudioFileReader.h>

namespace coral {

/**
 * The implementation of the AudioFileReader to read pcm data from a wav file.
 */
class WaveFileReader final : public AudioFileReader {
public:
  static std::unique_ptr<WaveFileReader> sniff(const std::string& inputUrl);

  explicit WaveFileReader(std::string inputUrl) noexcept;

  ~WaveFileReader() override;

  [[nodiscard]] int64_t getSampleLength() const override {
    return mSampleLength;
  }

  [[nodiscard]] int64_t getSampleRate() const override {
    return mSampleRate;
  }

  [[nodiscard]] int32_t getChannelCount() const override {
    return mChannels;
  }

  [[nodiscard]] int64_t getFramePosition() const override {
    return mFrameLength;
  }

  [[nodiscard]] AudioSampleFormat getSampleFormat() const override {
    return mSampleFormat;
  }

  bool open(Error** outError) override;

  void seekTo(int64_t framePosition) override;

  void close() override;

  int64_t readIntoBuffer(uint8_t* outBuffer, int64_t bufferCapacity, Error** outError) override;

private:
  static int64_t getFileSize(const std::string& url);

  static const char* getName() { return "AudioWavFileReader"; }

  static bool isWaveFile(const std::string& filename);

  static constexpr int kBitsPerByte = 8;

  struct FileDeleter {
    void operator()(FILE* file) const {
      if (file) {
        fclose(file);
      }
    }
  };

  struct WaveRIFF {
    const char id[4] = { 'R', 'I', 'F', 'F' };
    uint32_t fileLength{};
    const char waveFlag[4] = { 'W', 'A', 'V', 'E' };
  };

  struct WaveFormat {
    const char id[4] = { 'f', 'm', 't', ' ' };
    uint32_t blockSize = 16;
    uint16_t formatTag{};
    uint16_t channels{};
    uint32_t samplesPerSec{};
    uint32_t avgBytesPerSec{};
    uint16_t blockAlign{};
    uint16_t bitsPerSample{};
  };

  struct WaveData {
    const char id[4] = { 'd', 'a', 't', 'a' };
    uint32_t dataLength{};
  };

  std::string mInputUrl;

  int64_t mSampleLength;
  int64_t mFrameLength;

  std::unique_ptr<FILE, FileDeleter> mFile;
  uint32_t mFileLength;
  uint32_t mDataLength;
  int mChannels;
  int mSampleRate;
  int mBitsPerSample;
  int mDataOffset;
  AudioSampleFormat mSampleFormat;
};

} // namespace coral

#endif // CORAL_CODEC_WAVE_FILE_READER_H
