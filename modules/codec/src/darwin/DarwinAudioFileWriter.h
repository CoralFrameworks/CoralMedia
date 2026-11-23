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

#ifndef CORAL_CODEC_DARWIN_AUDIO_FILE_WRITER_H
#define CORAL_CODEC_DARWIN_AUDIO_FILE_WRITER_H

#if defined(__APPLE__)

#include <codec/AudioFileWriter.h>

#include "DarwinMediaUtils.h"

#include <media/Format.h>

#include <AudioToolbox/AudioToolbox.h>

#include <cstdint>
#include <memory>
#include <string>

namespace coral {

/**
 * The AudioToolbox based AudioFileWriter.
 *
 * The writer creates an `ExtAudioFile` of the requested container and inserts the platform encoder
 * for the compressed containers: m4a/aac, flac, and wav as a safety net for the pcm wave writer the
 * factory tries first. The pcm data is handed to the file in the sample format of the coral format,
 * the platform converts it to the layout the container stores.
 */
class DarwinAudioFileWriter final : public AudioFileWriter {
public:
  /**
   * Creates the writer when the file type and the pcm format are supported by the platform, returns
   * null otherwise.
   */
  static std::unique_ptr<AudioFileWriter> sniff(const std::string& outputUrl,
    const MediaFileType& fileType,
    const std::shared_ptr<MediaFormat>& format);

  ~DarwinAudioFileWriter() override;

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
    return "DarwinAudioFileWriter";
  }

private:
  DarwinAudioFileWriter(std::string outputUrl,
    const MediaFileType& fileType,
    const std::shared_ptr<MediaFormat>& format) noexcept;

  static bool isSupportedFileType(const MediaFileType& fileType);

  static bool convertToFileType(const MediaFileType& fileType, AudioFileTypeID& fileTypeId);

  static bool isSupportedSampleFormat(AudioSampleFormat sampleFormat);

  /** Builds the stream description the container stores. */
  void createFileFormat(AudioStreamBasicDescription& format) const;

  /** Applies the requested bit rate to the encoder ExtAudioFile created for a compressed file. */
  void applyEncodeBitrate();

  std::string mOutputUrl;
  MediaFileType mFileType;
  std::shared_ptr<MediaFormat> mInputFormat;

  ExtAudioFileRef mExtFile = nullptr;

  int32_t mBytesPerFrame = 0;
  int64_t mFramePosition = 0;
  bool mOpened = false;
};

} // namespace coral

#endif // defined(__APPLE__)

#endif // CORAL_CODEC_DARWIN_AUDIO_FILE_WRITER_H
