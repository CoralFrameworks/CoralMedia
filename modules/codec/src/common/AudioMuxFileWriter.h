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
#ifndef CORAL_CODEC_AUDIO_MUX_FILE_WRITER_H
#define CORAL_CODEC_AUDIO_MUX_FILE_WRITER_H

#include <codec/AudioFileWriter.h>

#include <codec/MediaCodec.h>
#include <codec/MediaMuxer.h>

#include <atomic>
#include <memory>

namespace coral {

/**
 * The implementation of the AudioFileWriter
 */
class AudioMuxFileWriter final : public AudioFileWriter {
public:
  static std::unique_ptr<AudioFileWriter>
  sniff(const std::string& outputUrl, const MediaFileType& fileType, const std::shared_ptr<MediaFormat>& format);

  explicit AudioMuxFileWriter(std::string outputUrl,
    const MediaFileType& fileType,
    const std::shared_ptr<MediaFormat>& format) noexcept;

  bool open(Error** outError) override;

  void close() override;

  int64_t writeFromPCMBuffer(void* inputBuffer, size_t numFrames, Error** outError) override;

  int64_t getSampleLength() const override {
    return mFramePosition;
  }

  int64_t getFramePosition() const override {
    return mFramePosition;
  }

private:
  void tryDequeueOutputBuffer();

  void onFormatChanged(const std::shared_ptr<MediaFormat>& format);

  void onPacketReady(const std::shared_ptr<AVPacketBuffer>& packetBuffer);

  static bool supportAudioEncoding(const AudioFormatType& codecType);

  static bool isMuxerFormatSupported(const MuxerFormat& format);

  static MuxerFormat convertToMuxerFormat(const MediaFileType& fileType);

  static const char* getName() { return "AudioMuxFileWriter"; }

  static constexpr int BITS_PER_BYTE = 8;

  std::string mOutputUrl;
  MediaFileType mFileType;
  std::shared_ptr<MediaFormat> mInputFormat;

  std::unique_ptr<AudioEncoder> mAudioEncoder;
  std::unique_ptr<AVMediaMuxer> mAudioMuxer;
  uint32_t mBytesPerFrame;
  int mTrackIndex;
  std::atomic_bool mMuxerStarted;
  int64_t mFramePosition;
};

} // namespace coral

#endif //CORAL_CODEC_AUDIO_MUX_FILE_WRITER_H
