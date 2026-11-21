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

#ifndef CORAL_CODEC_WMF_AUDIO_FILE_READER_H
#define CORAL_CODEC_WMF_AUDIO_FILE_READER_H

#include <codec/AudioFileReader.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace coral {

/**
* The implementation of the AudioFileReader based on the Windows Media Foundation source reader.
*
* The reader negotiates a 16 bit PCM output format, so the sample format reported by
* {@link getSampleFormat()} is either {@link AudioSampleFormat::kInt16} or
* {@link AudioSampleFormat::kFloat}.
*/
class WmfAudioFileReader final : public AudioFileReader {
public:
  /**
 * Creates the reader when the url is a media file which is supported by the platform, returns
 * nullptr otherwise.
 */
  static std::unique_ptr<WmfAudioFileReader> sniff(const std::string& inputUrl);

  explicit WmfAudioFileReader(std::string inputUrl);

  ~WmfAudioFileReader() override;

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

  [[nodiscard]] AudioSampleFormat getSampleFormat() const override;

  bool open(Error** outError) override;

  void seekTo(int64_t framePosition) override;

  void close() override;

  int64_t readIntoBuffer(uint8_t* outBuffer, int64_t bufferCapacity, Error** outError) override;

  static const char* getName() {
    return "WmfAudioFileReader";
  }

private:
  bool negotiatePcmOutput(Error** outError) const;

  bool refreshFormatInfo(Error** outError);

  std::string mInputUrl;

  Microsoft::WRL::ComPtr<IMFSourceReader> mReader;
  Microsoft::WRL::ComPtr<IMFMediaType> mCurrentOutputType;

  /**
 * The part of the last sample which did not fit into the reader buffer.
 */
  std::vector<uint8_t> mPendingData;
  size_t mPendingOffset = 0;

  int64_t mSampleRate = 0;
  int32_t mChannelCount = 0;
  int32_t mBitsPerSample = 0;
  int32_t mBytesPerFrame = 0;
  int64_t mSampleLength = 0;
  int64_t mFramePosition = 0;
  bool mOpened = false;
  bool mWmfStarted = false;
  bool mIsFloat = false;
};

} // namespace coral

#endif //CORAL_CODEC_WMF_AUDIO_FILE_READER_H
