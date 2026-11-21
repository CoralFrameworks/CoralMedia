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

#ifndef CORAL_AUDIO_AUDIO_FILE_WRITER_H
#define CORAL_AUDIO_AUDIO_FILE_WRITER_H

#include <codec/Error.h>
#include <media/Format.h>

#include <memory>
#include <string>

namespace coral {

class AudioFileWriter {
public:
  virtual ~AudioFileWriter() noexcept;

  /**
   * Creates an AudioFileWriter with the url
   *
   * @param url the specified output url to write audio data
   * @param fileType the specified file type of the url.
   * @param format the format of the file.
   *
   * @return nullptr if the AudioFileWriter is failed to create and init.
   */
  static std::unique_ptr<AudioFileWriter>
  CreateWithUrl(const std::string& url, const MediaFileType& fileType, const std::shared_ptr<MediaFormat>& format);

  /**
   * Returns the writer's processing audio sample format
   */
  [[nodiscard]] AudioSampleFormat getSampleFormat() const {
    return mSampleFormat;
  }

  /**
   * Returns the writer's processing sample rate.
   */
  [[nodiscard]] uint64_t getSampleRate() const {
    return mSampleRate;
  }

  /**
   * Returns the writer's processing channel count.
   */
  [[nodiscard]] uint32_t getChannelCount() const {
    return mChannelCount;
  }

  /**
   * Opens the file reader before we read audio data.
   */
  virtual bool open(Error** outError) = 0;

  /**
   * Closes all kept resources.
   */
  virtual void close() = 0;

  /**
   * Writes interleaved pcm data to the audio file.
   *
   * @param inputBuffer the audio pcm data buffer to write. Its format must match the writer's processing format.
   * @param numFrames   the number of frames to write.
   * @param outError    the output error pointer to store.
   *
   * @return the real number of valid sample frames in the buffer. Returns negative values when error occurs.
   *
   * @discussion
   * We support only little-endian storage; for the Int24 format, the application layer must store the data
   * in an array and ensure the value is aligned to the lower bits of an `int32_t`.
   */
  virtual int64_t writeFromPCMBuffer(void* inputBuffer, size_t numFrames, Error** outError) = 0;

  /**
   * The number of sample frames in the file.
   *
   * Note: this can be expensive to compute for the first time.
   */
  [[nodiscard]] virtual int64_t getSampleLength() const = 0;

  /**
   * The position of sample frames in the file at which the next write will occur.
   */
  [[nodiscard]] virtual int64_t getFramePosition() const = 0;

protected:
  explicit AudioFileWriter(const AudioSampleFormat sampleFormat,
    const uint64_t sampleRate,
    const uint32_t channelCount) noexcept
    : mSampleFormat(sampleFormat),
      mSampleRate(sampleRate),
      mChannelCount(channelCount) {
  }

  static void handleError(Error** outError, const char* errorMsg) {
    handleError(outError, ErrCode::kUnknown, errorMsg, nullptr);
  }

  static void handleError(Error** outError, const char* errorMsg, const char* description) {
    handleError(outError, ErrCode::kUnknown, errorMsg, description);
  }

  static void handleError(Error** outError, ErrCode errorCode, const char* errorMsg, const char* description);

private:
  static void logError(ErrCode code, const char* errorMsg, const char* description);

  AudioSampleFormat mSampleFormat;
  uint64_t mSampleRate;
  uint32_t mChannelCount;
};

} // namespace coral

#endif // CORAL_AUDIO_AUDIO_FILE_WRITER_H
