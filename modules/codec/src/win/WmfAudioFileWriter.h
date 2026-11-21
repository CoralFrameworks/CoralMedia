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

#ifndef CORAL_CODEC_WMF_AUDIO_FILE_WRITER_H
#define CORAL_CODEC_WMF_AUDIO_FILE_WRITER_H

#include <codec/AudioFileWriter.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <memory>
#include <string>

namespace coral {

/**
 * The implementation of the AudioFileWriter based on the Windows Media Foundation sink writer.
 *
 * The sink writer muxes the audio samples into the output file and inserts the required audio
 * encoder when the requested file type contains compressed audio.
 */
class WmfAudioFileWriter final : public AudioFileWriter {
public:
  /**
   * Creates the writer when the file type is supported by the media foundation sink writer.
   */
  static std::unique_ptr<AudioFileWriter> sniff(const std::string& outputUrl,
    const MediaFileType& fileType,
    const std::shared_ptr<MediaFormat>& format);

  ~WmfAudioFileWriter() override;

  bool open(Error** outError) override;

  void close() override;

  int64_t writeFromPCMBuffer(void* inputBuffer, size_t numFrames, Error** outError) override;

  [[nodiscard]] int64_t getSampleLength() const override {
    return mFramePosition;
  }

  [[nodiscard]] int64_t getFramePosition() const override {
    return mFramePosition;
  }

  static const char* getName() {
    return "WmfAudioFileWriter";
  }

private:
  WmfAudioFileWriter(std::string outputUrl,
    const MediaFileType& fileType,
    const std::shared_ptr<MediaFormat>& format) noexcept;

  static bool isSupportedFileType(const MediaFileType& fileType);

  static bool convertToContainerType(const MediaFileType& fileType, GUID& containerType);

  static bool convertToEncodedSubtype(const MediaFileType& fileType, GUID& subType);

  [[nodiscard]] Microsoft::WRL::ComPtr<IMFMediaType> createInputMediaType() const;

  [[nodiscard]] Microsoft::WRL::ComPtr<IMFMediaType> createOutputMediaType() const;

  std::string mOutputUrl;
  MediaFileType mFileType;
  std::shared_ptr<MediaFormat> mInputFormat;

  Microsoft::WRL::ComPtr<IMFSinkWriter> mSinkWriter;
  DWORD mStreamIndex = 0;

  int mBytesPerFrame = 0;
  int64_t mFramePosition = 0;
  bool mOpened = false;
  bool mWmfStarted = false;
};

} // namespace coral

#endif //CORAL_CODEC_WMF_AUDIO_FILE_WRITER_H
