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

#ifndef CORAL_CODEC_SIMPLE_VIDEO_DECODER_H
#define CORAL_CODEC_SIMPLE_VIDEO_DECODER_H

#include "private/ffcodec/FfmpegUtils.h"
#include "private/ffcodec/SimpleDecoder.h"

#include <media/PacketBuffer.h>
#include <media/PixelBuffer.h>

#include <memory>

namespace coral {

/**
 * The implementation of the SimpleDecoder based on Ffmpeg
 */
class SimpleVideoDecoder final : public SimpleDecoder<AVPacketBuffer, AVPixelBuffer>,
                                 public AVPixelBuffer::Owner<AVPixelBuffer> {
public:

private:
  /** The number of input and output buffers. */
  static constexpr int NUM_BUFFERS = 8;

  static constexpr int CODEC_ERROR_INVALID_DATA = -1;
  static constexpr int CODEC_ERROR_OTHER = -2;
  static constexpr int CODEC_ERROR_EOF = -3;

  static constexpr int DEFAULT_MAX_BUFFER_COUNT = 8;
  static constexpr int DEFAULT_FRAME_RATE = 30;

  // The deleter for AVCodecContext
  struct AVCodecContextDeleter {
    void operator()(AVCodecContext *codecContext) const {
      SimpleVideoDecoder::releaseCodecContext(codecContext);
    }
  };

  std::shared_ptr<MediaFormat> mInputFormat;
  std::shared_ptr<MediaFormat> mOutputFormat;

  std::unique_ptr<AVCodecContext, AVCodecContextDeleter> mCodecContext;
  std::unique_ptr<SwsContext, SwsContextDeleter> pSwsContext;
  std::unique_ptr<AVFrame, AVFrameDeleter> mCacheFrame;
  std::unique_ptr<AVPacket, AVPacketDeleter> mCachePacket;

public:
  SimpleVideoDecoder();

  void configure(const std::shared_ptr<MediaFormat> &format) override;

  void recycleBuffer(std::shared_ptr<AVPixelBuffer> outputBuffer) override;

  const std::shared_ptr<MediaFormat> &getOutputFormat() const {
    return mOutputFormat;
  }

  const char *getName() const override {
    return "FfmpegVideoDecoder";
  }

protected:
  std::shared_ptr<AVPacketBuffer> createInputBuffer() override;

  std::shared_ptr<AVPixelBuffer> createOutputBuffer() override;

  std::optional<Error> decodeBuffer(const std::shared_ptr<AVPacketBuffer> &inputBuffer,
      const std::shared_ptr<AVPixelBuffer> &outputBuffer,
      bool reset) override;

  void onRelease() override;

private:
  void initDecoder(const std::shared_ptr<MediaFormat> &format);

  void resetDecoder();

  int decodePacket(AVPacket *inputPacket, const std::shared_ptr<AVPixelBuffer> &outputBuffer);

  static void releaseCodecContext(AVCodecContext *context);
};

} // namespace coral

#endif // CORAL_CODEC_SIMPLE_VIDEO_DECODER_H
