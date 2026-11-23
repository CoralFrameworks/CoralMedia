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

#ifndef CORAL_CODEC_DARWIN_AUDIO_FILE_READER_H
#define CORAL_CODEC_DARWIN_AUDIO_FILE_READER_H

#if defined(__APPLE__)

#include <codec/AudioFileReader.h>

#include <media/Types.h>

#include <AudioToolbox/AudioToolbox.h>

#include <cstdint>
#include <memory>
#include <string>

namespace coral {

/**
 * The AudioToolbox based AudioFileReader.
 *
 * The reader opens any audio file the platform knows (m4a/aac, mp3, flac, wav ...) through an
 * `ExtAudioFile`. The platform converts the stored audio to interleaved 16 bit signed PCM while it
 * is read, so {@link getSampleFormat} always reports {@link AudioSampleFormat::kInt16}.
 */
class DarwinAudioFileReader final : public AudioFileReader {
public:
  /**
   * Creates the reader when the url is an audio file which is supported by the platform, returns
   * null otherwise.
   */
  static std::unique_ptr<DarwinAudioFileReader> sniff(const std::string& inputUrl);

  explicit DarwinAudioFileReader(std::string inputUrl);

  ~DarwinAudioFileReader() override;

  [[nodiscard]] int64_t getSampleLength() const override {
    return mSampleLength;
  }

  [[nodiscard]] int64_t getSampleRate() const override {
    return mSampleRate;
  }

  [[nodiscard]] int32_t getChannelCount() const override {
    return mChannelCount;
  }

  [[nodiscard]] int64_t getFramePosition() const override {
    return mFramePosition;
  }

  [[nodiscard]] AudioSampleFormat getSampleFormat() const override {
    return mSampleFormat;
  }

  bool open(Error** outError) override;

  void seekTo(int64_t framePosition) override;

  void close() override;

  int64_t readIntoBuffer(uint8_t* outBuffer, int64_t bufferCapacity, Error** outError) override;

  static const char* getName() {
    return "DarwinAudioFileReader";
  }

private:
  /** Reads the sample rate, the channel count and the length of the file. */
  bool readFormatInfo(Error** outError);

  std::string mInputUrl;

  ExtAudioFileRef mExtFile = nullptr;

  int64_t mSampleRate = 0;
  int32_t mChannelCount = 0;
  int32_t mBytesPerFrame = 0;
  int64_t mSampleLength = 0;
  int64_t mFramePosition = 0;
  AudioSampleFormat mSampleFormat = AudioSampleFormat::kInvalid;
  bool mOpened = false;
};

} // namespace coral

#endif // defined(__APPLE__)

#endif // CORAL_CODEC_DARWIN_AUDIO_FILE_READER_H
