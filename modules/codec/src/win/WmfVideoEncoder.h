/*
 * Copyright (C) 2026 The Coral Project
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

#ifndef CORAL_CODEC_WMF_VIDEO_ENCODER_H
#define CORAL_CODEC_WMF_VIDEO_ENCODER_H

#include <codec/MediaCodec.h>

#include "WmfTransform.h"

#include <media/Configuration.h>
#include <media/Format.h>

#include <deque>
#include <memory>

namespace coral {

/**
 * The implementation of the VideoEncoder based on the Windows Media Foundation transforms.
 *
 * The encoder input frame buffer reported by {@link dequeueInputBuffer} is either of the NV12 or
 * the I420 pixel format, depending on the pixel format which is supported by the encoder.
 */
class WmfVideoEncoder final : public VideoEncoder {
public:
  explicit WmfVideoEncoder(VideoCodecType codecType);

  ~WmfVideoEncoder() override;

  // Makes the single argument overload of the base class visible.
  using VideoEncoder::configure;

  bool configure(const std::shared_ptr<MediaFormat>& format,
    const std::shared_ptr<VideoSurface>& surface) override;

  void start() override;

  void stop() override;

  void flush() override;

  void release() override;

  std::shared_ptr<AVPixelBuffer> dequeueInputBuffer() override;

  void queueInputBuffer(std::shared_ptr<AVPixelBuffer> inputBuffer) override;

  std::shared_ptr<AVPacketBuffer> dequeueOutputBuffer() override;

  void releaseOutputBuffer(const std::shared_ptr<AVPacketBuffer>& outputBuffer, bool render) override;

  void signalEndOfInputStream() override;

  [[nodiscard]] const std::shared_ptr<MediaFormat>& getOutputFormat() const override {
    return mOutputFormat;
  }

  [[nodiscard]] const char* getName() const override {
    return "WmfVideoEncoder";
  }

private:
  bool createCodec();

  bool createOutputMediaType();

  /**
   * Selects the input pixel format supported by the encoder and sets the input media type.
   */
  bool createInputMediaType();

  bool pushInputSamples();

  bool refreshOutputFormat();

  /**
   * Converts the encoder input frame into a media foundation sample.
   */
  bool createFrameSample(const std::shared_ptr<AVPixelBuffer>& frame,
    Microsoft::WRL::ComPtr<IMFSample>& sample,
    int64_t timeUs);

  bool fillOutputBuffer(const std::shared_ptr<AVPacketBuffer>& packetBuffer,
    const Microsoft::WRL::ComPtr<IMFSample>& sample);

  std::shared_ptr<AVPacketBuffer> createEndOfStreamBuffer();

  static int64_t sampleTimeUs(const Microsoft::WRL::ComPtr<IMFSample>& sample);

  static constexpr int DEFAULT_BIT_RATE = 4000000;
  static constexpr int DEFAULT_FRAME_RATE = 30;

  VideoCodecType mCodecType;
  GUID mOutputSubType{};
  GUID mInputSubType{};

  bool mConfigured = false;
  bool mInputDequeued = false;
  bool mInputEndOfStream = false;
  bool mOutputEndOfStream = false;
  bool mDrainRequested = false;
  bool mFormatReported = false;
  //!< Whether the encoder input stores the V plane before the U plane (YV12).
  bool mSwapChromaPlanes = false;

  WmfTransformContext mTransform;

  std::shared_ptr<MediaFormat> mInputFormat;
  std::shared_ptr<MediaFormat> mOutputFormat;

  int mWidth = 0;
  int mHeight = 0;
  int mFrameRate = DEFAULT_FRAME_RATE;

  std::shared_ptr<AVPixelBuffer> mInputBuffer;
  std::shared_ptr<AVPacketBuffer> mPacketBuffer;

  std::deque<Microsoft::WRL::ComPtr<IMFSample>> mPendingInputSamples;
  std::deque<Microsoft::WRL::ComPtr<IMFSample>> mPendingOutputSamples;

  int64_t mFrameIndex = 0;
  bool mWmfStarted = false;
};

} // namespace coral

#endif //CORAL_CODEC_WMF_VIDEO_ENCODER_H
