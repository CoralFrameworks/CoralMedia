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

#ifndef CORAL_CODEC_ANDROID_NATIVE_VIDEO_ENCODER_H
#define CORAL_CODEC_ANDROID_NATIVE_VIDEO_ENCODER_H

#include <codec/MediaCodec.h>
#include <media/PacketBuffer.h>

#if defined(ANDROID) || defined(__ANDROID__)
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#endif

#include <chrono>
#include <memory>

namespace coral {

/**
 * The implementation of the VideoEncoder based on AMediaCodec
 */
class NativeVideoEncoder final : public VideoEncoder {
private:
  static constexpr int INDEX_UNSET = -1;
  static constexpr int DEFAULT_FRAME_RATE = 30;
  static constexpr int I_FRAME_INTERVAL = 1;
  static constexpr int TIME_OUT_US = 1000;
  static constexpr float DEFAULT_BITRATE_SCALE = 2.25;

  struct AMediaCodecDeleter {
    void operator()(AMediaCodec* codec) const {
      if (codec != nullptr) {
        AMediaCodec_stop(codec);
        AMediaCodec_delete(codec);
      }
    }
  };

  std::string phoneType;
  std::string cpuType;

  std::shared_ptr<SurfaceHolder> surfaceHolder;
  std::unique_ptr<AMediaCodec, AMediaCodecDeleter> videoCodec;
  std::shared_ptr<MediaFormat> outputFormat;

  std::shared_ptr<AVPacketBuffer> packetBuffer;
  std::shared_ptr<AVPixelBuffer> sampleBuffer;
  std::shared_ptr<AVPixelBuffer> dequeuedInputBuffer;

  int inputIndex;
  bool inputEof;
  bool outputEof;
  /**
   * The codec is not required to return its remaining packets and its EOS flag in the same call that
   * signals the input's end. Treating the first empty `dequeueOutputBuffer` after the signal as EOS
   * drops every packet the codec still holds, so EOS is forced only after this deadline.
   */
  std::chrono::steady_clock::time_point eofDeadline{};

public:
  explicit NativeVideoEncoder(const std::shared_ptr<SurfaceHolder>& surfaceHolder) noexcept;

  ~NativeVideoEncoder() override;

  bool configure(const std::shared_ptr<MediaFormat>& format, const std::shared_ptr<VideoSurface>& surface) override;

  void start() override;

  void stop() override;

  std::shared_ptr<AVPixelBuffer> dequeueInputBuffer() override;

  void queueInputBuffer(std::shared_ptr<AVPixelBuffer> inputBuffer) override;

  std::shared_ptr<AVPacketBuffer> dequeueOutputBuffer() override;

  void releaseOutputBuffer(std::shared_ptr<AVPacketBuffer> const &outputBuffer, bool render) override;

  void signalEndOfInputStream() override;

  void flush() override;

  void release() override;

  [[nodiscard]] const std::shared_ptr<MediaFormat>& getOutputFormat() const override {
    return outputFormat;
  }

  [[nodiscard]] const char* getName() const override {
    return "NdkVideoEncoder";
  }

private:
  [[nodiscard]] bool isMtkCpu() const { return cpuType.compare(0, 2, "mt") == 0; }
};

} // namespace coral

#endif // CORAL_CODEC_ANDROID_NATIVE_VIDEO_ENCODER_H
