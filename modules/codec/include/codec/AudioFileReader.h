/*
 * Copyright (C) 2023 The Coral Project
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

#ifndef CORAL_CODEC_AUDIO_FILE_READER_H
#define CORAL_CODEC_AUDIO_FILE_READER_H

#include <codec/Error.h>
#include <media/Types.h>

#include <cstdint>
#include <memory>
#include <string>

namespace coral {

/**
 * The AudioFileReader describes how to read audio data to a buffer
 */
class AudioFileReader {
public:
  virtual ~AudioFileReader();

  /**
   * Creates an AudioFileReader with the url
   */
  static std::unique_ptr<AudioFileReader> CreateWithUrl(const std::string& inputUrl);

  /**
   * Returns the number of sample frames in the file.
   */
  [[nodiscard]] virtual int64_t getSampleLength() const = 0;

  /**
   * Returns the sample rate. Available after the method {@link open()} is called.
   */
  [[nodiscard]] virtual int64_t getSampleRate() const = 0;

  /**
   * Returns the channel count. Available after the method {@link open()} is called.
   */
  [[nodiscard]] virtual int32_t getChannelCount() const = 0;

  /**
   * Returns the position of sample frames in the file at which the next read will occur.
   */
  [[nodiscard]] virtual int64_t getFramePosition() const = 0;

  /**
   * Returns the audio sample format.
   *
   * Available after the method {@link open()} is called.
   * If we set the output format, return the format we set.
   */
  [[nodiscard]] virtual AudioSampleFormat getSampleFormat() const = 0;

  /**
   * Opens the file reader before we read audio data.
   */
  virtual bool open(Error** outError) = 0;

  /**
   * Notifies the file reader that a seek has occurred.
   *
   * @param framePosition The frame position to seek
   */
  virtual void seekTo(int64_t framePosition) = 0;

  /**
   * Closes all kept resources.
   */
  virtual void close() = 0;

  /**
   * Reads interleaved pcm data into audio buffer
   *
   * @param outBuffer       the audio buffer to store pcm data.
   * @param bufferCapacity  the size of the audio buffer.
   * @param outError        the output error pointer to store.
   *
   * @return  the real number of valid sample frames in the buffer.
   *          returns negative values when error occurs.
   */
  virtual int64_t readIntoBuffer(uint8_t* outBuffer, int64_t bufferCapacity, Error** outError) = 0;

protected:
  static void handleError(Error** outError, const char* errorMsg, const char* description = "") {
    handleError(outError, ErrCode::kUnknown, errorMsg, description);
  }

  static void handleError(Error** outError, const ErrCode code, const char* errorMsg, const char* description = "") {
    if (outError != nullptr) {
      if (*outError == nullptr) {
        *outError = new Error();
      }
      (*outError)->code = code;
      (*outError)->failureReason = errorMsg;
      (*outError)->description = description;
#ifndef NDEBUG
      logError(code, errorMsg, description);
#endif
    } else {
      logError(code, errorMsg, description);
    }
  }

private:
  static void logError(ErrCode code, const char* errorMsg, const char* description);
};

} // namespace coral

#endif // CORAL_CODEC_AUDIO_FILE_READER_H
