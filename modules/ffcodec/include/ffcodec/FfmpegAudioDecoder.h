/*
 * Copyright (C) 2022 The Coral Project
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

#ifndef CORAL_CODEC_FFMPEG_AUDIO_DECODER_H
#define CORAL_CODEC_FFMPEG_AUDIO_DECODER_H

#include <codec/MediaCodec.h>

#include "private/ffcodec/SimpleAudioDecoder.h"

#include <memory>

namespace coral {

class FfmpegAudioDecoder final : public AudioDecoder {
public:
  explicit FfmpegAudioDecoder(const bool outputFloat) noexcept
  : FfmpegAudioDecoder(NUM_BUFFERS, NUM_BUFFERS, outputFloat) {
  }

  explicit FfmpegAudioDecoder(int numInputBuffers, int numOutputBuffers, bool outputFloat) noexcept;

  ~FfmpegAudioDecoder() override;

  bool configure(std::shared_ptr<MediaFormat> const &format) override;

  void start() override;

  void stop() override;

  void flush() override;

  void release() override;

  std::shared_ptr<AVPacketBuffer> dequeueInputBuffer() override;

  void queueInputBuffer(std::shared_ptr<AVPacketBuffer> inputBuffer) override;

  std::shared_ptr<AVSampleBuffer> dequeueOutputBuffer() override;

  void releaseOutputBuffer(const std::shared_ptr<AVSampleBuffer>& outputBuffer, bool render) override;

  void signalEndOfInputStream() override;

  std::shared_ptr<MediaFormat> const &getOutputFormat() const override;

  char const *getName() const override;

  void setOutputStartTimeUs(int64_t outputStartTimeUs) override;

private:
  /** The number of input and output buffers. */
  static constexpr int NUM_BUFFERS = 16;

  std::unique_ptr<SimpleAudioDecoder> decoder;
};

} // namespace coral

#endif //CORAL_CODEC_FFMPEG_AUDIO_DECODER_H
