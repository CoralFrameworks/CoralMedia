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

#ifndef CORAL_CODEC_FFMPEG_AUDIO_FILE_READER_H
#define CORAL_CODEC_FFMPEG_AUDIO_FILE_READER_H

#include <codec/AudioFileReader.h>

#include "private/ffcodec/FfmpegUtils.h"

namespace coral {

/**
 * The implementation of the AudioFileReader based on Ffmpeg.
 */
class FfmpegAudioFileReader final : public AudioFileReader {
public:
  static std::unique_ptr<FfmpegAudioFileReader> sniff(const std::string& inputUrl);

  explicit FfmpegAudioFileReader(AVFormatContext* formatContext) noexcept;

  ~FfmpegAudioFileReader() override;

  int64_t getSampleLength() const override;

  int64_t getSampleRate() const override;

  int32_t getChannelCount() const override;

  int64_t getFramePosition() const override;

  AudioSampleFormat getSampleFormat() const override;

  bool open(Error** outError) override;

  void seekTo(int64_t framePosition) override;

  void close() override;

  int64_t readIntoBuffer(uint8_t* data, int64_t bufferSize, Error** outError) override;

  static const char* getName() { return "FfmpegAudioFileReader"; }

private:
  bool prepareDecoder(int streamIndex, Error** outError);

  bool initResample();

  static bool resampleAndStoreFrame(AVCodecContext* codecContext, AVAudioFifo* fifo, AVFrame* inputFrame);

  static bool initConvertedSamples(uint8_t*** convertedSamples, AVCodecContext* codecContext, int frameSize);

  static bool
  convertSamples(SwrContext* resampleContext, const uint8_t** inputData, uint8_t** convertedData, const int frameSize);

  static bool addSamplesToFifo(AVAudioFifo* fifo, uint8_t** convertedInputSamples, const int frame_size);

  static void releaseBuffer(uint8_t** buffer);

  // The deleter for AVCodecContext
  struct AVCodecContextDeleter {
    void operator()(AVCodecContext* codecContext) const {
      avcodec_free_context(&codecContext);
    }
  };

  AudioSampleFormat mOutputType;
  int64_t mSampleRate;
  int32_t mChannelCount;
  int64_t mSampleLength;
  int64_t mFramePosition;
  std::atomic_bool mReadEof;
  std::atomic_int mStreamIndex;

  std::unique_ptr<AVFormatContext, AVFormatContextDeleter> pFormatContext;
  std::unique_ptr<AVCodecContext, AVCodecContextDeleter> pCodecContext;
  std::unique_ptr<AVAudioFifo, AVAudioFifoDeleter> pAudioFifo;
  std::unique_ptr<AVPacket, AVPacketDeleter> mCachePacket;
  std::unique_ptr<AVFrame, AVFrameDeleter> mCacheFrame;
};

} // namespace coral

#endif // CORAL_CODEC_FFMPEG_AUDIO_FILE_READER_H
