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

#ifndef CORAL_CODEC_FFMPEG_AUDIO_ENCODER_H
#define CORAL_CODEC_FFMPEG_AUDIO_ENCODER_H

#include <codec/MediaCodec.h>

#include <media/PacketBuffer.h>
#include <media/SampleBuffer.h>

#include "private/ffcodec/FfmpegUtils.h"

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace coral {

/**
 * The AudioEncoder implementation based on Ffmpeg
 */
class FfmpegAudioEncoder final : public AudioEncoder,
                                 public AVPacketBuffer::Recycler,
                                 public AVSampleBuffer::Owner<AVSampleBuffer> {
public:
  FfmpegAudioEncoder();

  ~FfmpegAudioEncoder() override;

  bool configure(const std::shared_ptr<MediaFormat>& format) override;

  void start() override;

  void stop() override;

  void flush() override;

  void release() override;

  std::shared_ptr<AVSampleBuffer> dequeueInputBuffer() override;

  void queueInputBuffer(std::shared_ptr<AVSampleBuffer> inputBuffer) override;

  std::shared_ptr<AVPacketBuffer> dequeueOutputBuffer() override;

  void releaseOutputBuffer(const std::shared_ptr<AVPacketBuffer>& outputBuffer, bool render) override;

  void signalEndOfInputStream() override;

  void recycleBuffer(std::shared_ptr<AVSampleBuffer> inputBuffer) override;

  void recycleBuffer(std::shared_ptr<AVPacketBuffer> packetBuffer) override;

  const std::shared_ptr<MediaFormat>& getOutputFormat() const override {
    return mediaFormat;
  }

  const char* getName() const override {
    return "FfmpegAudioEncoder";
  }

private:
  void run() {
    released = false;

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

  void maybeNotifyProcessLoop() const {
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

  void clearBufferQueuesLocked();

  bool readFifoAndEncode();

  bool encodeFrame(AVFrame* frame);

  bool convertAndEncodeFrame(AVFrame* frame);

  bool initResample();

  bool initConvertedSamples(uint8_t*** converted_input_samples,
    AVCodecContext* codecContext,
    int frame_size) const;

  bool convertSamples(SwrContext* resampleContext,
    const uint8_t** inputData,
    uint8_t** convertedData,
    int frameSize) const;

  bool addSampleToFifo(AVAudioFifo* fifo, uint8_t** convertedInputSamples, int frame_size) const;

  bool initOutputFrame(AVFrame** frame, AVCodecContext* codecContext, int frame_size) const;

  static void releaseCodecContext(AVCodecContext* codecContext);

  static void releaseBuffer(uint8_t** buffer);

  static constexpr int DEFAULT_MAX_BUFFER_COUNT = 16;

  // The deleter for AVCodecContext
  struct AVCodecContextDeleter {
    void operator()(AVCodecContext* codecContext) const {
      FfmpegAudioEncoder::releaseCodecContext(codecContext);
    }
  };

  mutable std::condition_variable notify_cv_;
  mutable std::mutex pending_lock_;
  std::unique_ptr<std::thread> thread;
  bool flushed;
  bool released;
  bool requestEof;
  int64_t framePosition = 0;

  std::unique_ptr<AVCodecContext, AVCodecContextDeleter> pCodecContext;
  AVAudioFifo* pAudioFifo;
  std::shared_ptr<MediaFormat> mediaFormat;

  std::unique_ptr<AVFrame, AVFrameDeleter> mCacheFrame;
  std::unique_ptr<AVFrame, AVFrameDeleter> mResampleFrame;
  std::unique_ptr<AVPacket, AVPacketDeleter> mCachePacket;

  std::deque<std::shared_ptr<AVSampleBuffer>> queuedInputBuffers;
  std::deque<std::shared_ptr<AVPacketBuffer>> queuedOutputBuffers;
  std::deque<std::shared_ptr<AVSampleBuffer>> availableInputBuffers;
  std::vector<std::shared_ptr<AVPacketBuffer>> availableOutputBuffers;
  int availableOutputBufferCount;
};

} // namespace coral

#endif // CORAL_CODEC_FFMPEG_AUDIO_ENCODER_H
