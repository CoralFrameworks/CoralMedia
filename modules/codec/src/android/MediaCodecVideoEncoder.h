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

#ifndef CORAL_CODEC_ANDROID_MEDIACODEC_VIDEO_ENCODER_H
#define CORAL_CODEC_ANDROID_MEDIACODEC_VIDEO_ENCODER_H

#include <codec/MediaCodec.h>

#include <media/PacketBuffer.h>

#include <core/jni/android/MediaCodec.h>
#include <core/jni/android/MediaFormat.h>

#include <memory>

namespace coral {

/**
 * The VideoEncoder implementation based on android.media.MediaCodec
 */
class MediaCodecVideoEncoder final : public VideoEncoder {
private:
  static constexpr int INDEX_UNSET = -1;
  static constexpr int DEFAULT_FRAME_RATE = 30;
  static constexpr int I_FRAME_INTERVAL = 1;
  static constexpr int TIME_OUT_US = 1000;
  static constexpr float DEFAULT_BITRATE_SCALE = 2.25;

  std::string phoneType;
  std::string cpuType;

  std::shared_ptr<SurfaceHolder> surfaceHolder;

  std::unique_ptr<JniMediaCodec> videoCodec;
  std::shared_ptr<MediaFormat> outputFormat;

  std::shared_ptr<AVPacketBuffer> packetBuffer;
  std::shared_ptr<AVPixelBuffer> sampleBuffer;
  std::shared_ptr<AVPixelBuffer> dequeuedInputBuffer;

  int inputIndex;
  bool inputEof;
  bool outputEof;

public:
  explicit MediaCodecVideoEncoder(const std::shared_ptr<SurfaceHolder>& surfaceHolder) noexcept;

  ~MediaCodecVideoEncoder() override;

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
    return "MediaCodecVideoEncoder";
  }

private:
  [[nodiscard]] bool isMtkCpu() const { return cpuType.compare(0, 2, "mt") == 0; }
};

} // namespace coral

#endif // CORAL_CODEC_ANDROID_MEDIACODEC_VIDEO_ENCODER_H
