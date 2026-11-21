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

#ifndef CORAL_CODEC_FFMPEG_VIDEO_DECODER_H
#define CORAL_CODEC_FFMPEG_VIDEO_DECODER_H

#include <codec/MediaCodec.h>

#include "private/ffcodec/SimpleVideoDecoder.h"

#include <memory>

namespace coral {

class FfmpegVideoDecoder final: public VideoDecoder {
public:
  FfmpegVideoDecoder();

  ~FfmpegVideoDecoder() override;

  bool configure(const std::shared_ptr<MediaFormat>& format, const std::shared_ptr<VideoSurface>& surface) override;

  void start() override;

  void stop() override;

  void flush() override;

  void release() override;

  std::shared_ptr<AVPacketBuffer> dequeueInputBuffer() override;

  void queueInputBuffer(std::shared_ptr<AVPacketBuffer> inputBuffer) override;

  std::shared_ptr<AVPixelBuffer> dequeueOutputBuffer() override;

  void releaseOutputBuffer(const std::shared_ptr<AVPixelBuffer>& frameBuffer, bool render) override;

  void signalEndOfInputStream() override;

  const std::shared_ptr<MediaFormat>&getOutputFormat() const override;

  char const *getName() const override;

  void setOutputStartTimeUs(int64_t outputStartTimeUs) override;

private:
  std::unique_ptr<SimpleVideoDecoder> decoder;
};

} // namespace coral

#endif //CORAL_CODEC_FFMPEG_VIDEO_DECODER_H
