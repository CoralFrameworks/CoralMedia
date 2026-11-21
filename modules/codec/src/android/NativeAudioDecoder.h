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
#ifndef CORAL_CODEC_ANDROID_NATIVE_AUDIO_DECODER_H
#define CORAL_CODEC_ANDROID_NATIVE_AUDIO_DECODER_H

#include <codec/MediaCodec.h>

#include <media/Configuration.h>
#include <media/Format.h>

#if defined(__ANDROID__)
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#endif

#include <deque>
#include <memory>
#include <mutex>

namespace coral {

/**
 * The implementation of the AudioDecoder based on AMediaCodec
 */
class NativeAudioDecoder final : public AudioDecoder,
                                 public AVSampleBuffer::Owner<AVSampleBuffer> {
private:
  static constexpr int INDEX_UNSET = -1;
  static constexpr int DEFAULT_BIT_RATE = 128000;

  struct AMediaCodecDeleter {
    void operator()(AMediaCodec* codec) const {
      if (codec != nullptr) {
        AMediaCodec_stop(codec);
        AMediaCodec_delete(codec);
      }
    }
  };

  mutable std::mutex pending_lock_;

  std::unique_ptr<AMediaCodec, AMediaCodecDeleter> audioCodec;
  std::shared_ptr<MediaFormat> outputFormat;
  AudioConfiguration outputConfig;

  std::shared_ptr<AVPacketBuffer> packetBuffer;
  std::shared_ptr<AVPacketBuffer> dequeuedInputBuffer;

  std::deque<std::shared_ptr<AVSampleBuffer>> availableOutputBuffers;

  int inputIndex;
  bool inputEof;
  bool outputEof;
  int64_t outputStartTimeUs;

public:
  NativeAudioDecoder();

  void setOutputStartTimeUs(int64_t outputStartTimeUs) override;

  bool configure(const std::shared_ptr<MediaFormat>& format) override;

  void start() override;

  void stop() override;

  void flush() override;

  void release() override;

  std::shared_ptr<AVPacketBuffer> dequeueInputBuffer() override;

  void queueInputBuffer(std::shared_ptr<AVPacketBuffer> inputBuffer) override;

  std::shared_ptr<AVSampleBuffer> dequeueOutputBuffer() override;

  void signalEndOfInputStream() override;

  void recycleBuffer(std::shared_ptr<AVSampleBuffer> decodedBuffer) override;

  void releaseOutputBuffer(const std::shared_ptr<AVSampleBuffer>& outputBuffer, bool render) override;

  [[nodiscard]] const std::shared_ptr<MediaFormat>& getOutputFormat() const override {
    return outputFormat;
  }

  [[nodiscard]] const char* getName() const override {
    return "NdkAudioDecoder";
  }

private:
  void parseOutputFormat(AMediaFormat* mediaFormat);

  [[nodiscard]] bool isAtLeastOutputStartTimeUs(int64_t timeUs) const {
    return outputStartTimeUs == Definitions::kTimeUnset || timeUs >= outputStartTimeUs;
  }

  std::shared_ptr<AVSampleBuffer> dequeueAvailableOutputBuffer();
};

} // namespace coral

#endif //CORAL_CODEC_ANDROID_NATIVE_AUDIO_DECODER_H
