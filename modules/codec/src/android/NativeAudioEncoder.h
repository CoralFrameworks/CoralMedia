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
#ifndef CORAL_CODEC_ANDROID_NATIVE_AUDIO_ENCODER_H
#define CORAL_CODEC_ANDROID_NATIVE_AUDIO_ENCODER_H

#include <codec/MediaCodec.h>

#include <media/Configuration.h>
#include <media/Format.h>

#if defined(__ANDROID__)
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#endif

#include <atomic>
#include <memory>

namespace coral {

/**
 * The implementation of the AudioEncoder based on AMediaCodec
 */
class NativeAudioEncoder final : public AudioEncoder {
private:
  static constexpr int INDEX_UNSET = -1;
  static constexpr int DEFAULT_BIT_RATE = 128000;
  static constexpr int DEFAULT_MAX_INPUT_SIZE = 8192;
  static constexpr int TIME_OUT_US = 1000;

  struct AMediaCodecDeleter {
    void operator()(AMediaCodec* codec) const {
      if (codec != nullptr) {
        AMediaCodec_stop(codec);
        AMediaCodec_delete(codec);
      }
    }
  };

  struct AMediaFormatDeleter {
    void operator()(AMediaFormat* mediaFormat) const {
      if (mediaFormat != nullptr) {
        AMediaFormat_delete(mediaFormat);
      }
    }
  };

  AudioConfiguration inputConfig{};

  std::unique_ptr<AMediaCodec, AMediaCodecDeleter> audioCodec;
  std::unique_ptr<AMediaFormat, AMediaFormatDeleter> codecFormat;
  std::shared_ptr<MediaFormat> outputFormat;

  std::shared_ptr<AVPacketBuffer> packetBuffer;
  std::shared_ptr<AVSampleBuffer> sampleBuffer;
  std::shared_ptr<AVSampleBuffer> dequeuedInputBuffer;

  std::atomic_bool mRunning{ false };

  int inputIndex;
  bool inputEof;
  bool outputEof;
  int64_t presentationTimeUs;

public:
  NativeAudioEncoder();

  bool configure(const std::shared_ptr<MediaFormat>& format) override;

  void start() override;

  void stop() override;

  std::shared_ptr<AVSampleBuffer> dequeueInputBuffer() override;

  void queueInputBuffer(std::shared_ptr<AVSampleBuffer> inputBuffer) override;

  std::shared_ptr<AVPacketBuffer> dequeueOutputBuffer() override;

  void releaseOutputBuffer(const std::shared_ptr<AVPacketBuffer>& outputBuffer, bool render) override;

  void signalEndOfInputStream() override;

  void flush() override;

  void release() override;

  [[nodiscard]] const std::shared_ptr<MediaFormat>& getOutputFormat() const override {
    return outputFormat;
  }

  [[nodiscard]] const char* getName() const override {
    return "NdkAudioEncoder";
  }
};

} // namespace coral

#endif //CORAL_CODEC_ANDROID_NATIVE_AUDIO_ENCODER_H
