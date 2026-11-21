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

#ifndef CORAL_CODEC_FFMPEG_VIDEO_ENCODER_H
#define CORAL_CODEC_FFMPEG_VIDEO_ENCODER_H

#include <codec/MediaCodec.h>

#include <media/Format.h>
#include <media/PacketBuffer.h>
#include <media/PixelBuffer.h>

#include "private/ffcodec/FfmpegUtils.h"

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace coral {

/**
 * The implementation of the VideoEncoder based on Ffmpeg
 */
class FfmpegVideoEncoder final : public VideoEncoder,
                                 public AVPacketBuffer::Recycler,
                                 public AVPixelBuffer::Owner<AVPixelBuffer> {
public:

  FfmpegVideoEncoder();

  ~FfmpegVideoEncoder() override;

  bool configure(const std::shared_ptr<MediaFormat>& format,  const std::shared_ptr<VideoSurface>& surface) override;

  void start() override;

  void stop() override;

  void flush() override;

  void release() override;

  std::shared_ptr<AVPixelBuffer> dequeueInputBuffer() override;

  void queueInputBuffer(std::shared_ptr<AVPixelBuffer> buffer) override;

  std::shared_ptr<AVPacketBuffer> dequeueOutputBuffer() override;

  void releaseOutputBuffer(std::shared_ptr<AVPacketBuffer> const &outputBuffer, bool render) override;

  void signalEndOfInputStream() override;

  void recycleBuffer(std::shared_ptr<AVPixelBuffer> inputBuffer) override;

  void recycleBuffer(std::shared_ptr<AVPacketBuffer> packetBuffer) override;

  const std::shared_ptr<MediaFormat>& getOutputFormat() const override {
    return mediaFormat;
  }

  const char* getName() const override {
    return "FfmpegVideoEncoder";
  }

private:
  void run() {
    running = true;

    prepareFormat();
    prepareCodecConfig();

    while (encode()) {
      // Do nothing.
    }
  }

  bool encode();

  void maybeNotifyError() {
    // Do nothing
  }

  void maybeNotifyProcessLoop() {
    if (canProcessBuffer()) {
      notify_cv_.notify_all();
    }
  }

  [[nodiscard]] bool canProcessBuffer() const {
    return !queuedInputBuffers.empty() && availableOutputBufferCount > 0;
  }

  std::shared_ptr<AVPacketBuffer> dequeueAvailableOutputBuffer() {
    std::unique_lock<std::mutex> lock(pending_lock_);
    return availableOutputBuffers[--availableOutputBufferCount];
  }

  void queueOutputBuffer(std::shared_ptr<AVPacketBuffer> outputBuffer) {
    std::unique_lock<std::mutex> lock(pending_lock_);
    if (outputBuffer) {
      queuedOutputBuffers.push_back(std::move(outputBuffer));
    }
  }

  bool initEncoder();

  void prepareFormat();

  void prepareCodecConfig();

  void flushInternal();

  void clearBufferQueuesLocked();

  void encodeFrame(AVFrame* frame);

  static void releaseCodecContext(AVCodecContext* codecContext);

  // The deleter for AVCodecContext
  struct AVCodecContextDeleter {
    void operator()(AVCodecContext* codecContext) const {
      FfmpegVideoEncoder::releaseCodecContext(codecContext);
    }
  };

  static constexpr int DEFAULT_MAX_BUFFER_COUNT = 16;
  static constexpr int DEFAULT_FRAME_RATE = 30;

  mutable std::condition_variable notify_cv_;
  mutable std::mutex pending_lock_;
  std::unique_ptr<std::thread> thread;
  bool running;
  bool requestEof;
  bool flushed;

  std::unique_ptr<AVCodecContext, AVCodecContextDeleter> pCodecContext;
  std::shared_ptr<MediaFormat> mediaFormat;

  std::unique_ptr<AVFrame, AVFrameDeleter> cacheFrame;
  std::unique_ptr<AVPacket, AVPacketDeleter> cachePacket;

  std::deque<std::shared_ptr<AVPixelBuffer>> queuedInputBuffers;
  std::deque<std::shared_ptr<AVPacketBuffer>> queuedOutputBuffers;
  std::deque<std::shared_ptr<AVPixelBuffer>> availableInputBuffers;
  std::vector<std::shared_ptr<AVPacketBuffer>> availableOutputBuffers;
  int availableOutputBufferCount = DEFAULT_MAX_BUFFER_COUNT;
};

} // namespace coral

#endif // CORAL_CODEC_FFMPEG_VIDEO_ENCODER_H
