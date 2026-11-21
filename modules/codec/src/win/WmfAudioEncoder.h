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

#ifndef CORAL_CODEC_WMF_AUDIO_ENCODER_H
#define CORAL_CODEC_WMF_AUDIO_ENCODER_H

#include <codec/MediaCodec.h>

#include "WmfTransform.h"

#include <media/Configuration.h>
#include <media/Format.h>

#include <deque>
#include <memory>

namespace coral {

/**
 * The implementation of the AudioEncoder based on the Windows Media Foundation transforms, it
 * supports the audio codecs which are available on the Windows platform (aac, mp3 ...).
 */
class WmfAudioEncoder final : public AudioEncoder {
public:
  explicit WmfAudioEncoder(AudioFormatType codecType);

  ~WmfAudioEncoder() override;

  bool configure(const std::shared_ptr<MediaFormat>& format) override;

  void start() override;

  void stop() override;

  void flush() override;

  void release() override;

  std::shared_ptr<AVSampleBuffer> dequeueInputBuffer() override;

  void queueInputBuffer(std::shared_ptr<AVSampleBuffer> inputBuffer) override;

  std::shared_ptr<AVPacketBuffer> dequeueOutputBuffer() override;

  void releaseOutputBuffer(const std::shared_ptr<AVPacketBuffer>& outputBuffer,
    bool render) override;

  void signalEndOfInputStream() override;

  [[nodiscard]] const std::shared_ptr<MediaFormat>& getOutputFormat() const override {
    return mOutputFormat;
  }

  [[nodiscard]] const char* getName() const override {
    return "WmfAudioEncoder";
  }

private:
  bool createCodec();

  bool createOutputMediaType();

  bool createInputMediaType();

  bool pushInputSamples();

  /**
 * Updates the reported output format, returns false when the format is not available yet.
 */
  bool refreshOutputFormat();

  /**
 * Fills the packet buffer with the encoded data of the sample.
 */
  bool fillOutputBuffer(const std::shared_ptr<AVPacketBuffer>& packetBuffer,
    const Microsoft::WRL::ComPtr<IMFSample>& sample);

  std::shared_ptr<AVPacketBuffer> createEndOfStreamBuffer();

  static int64_t sampleTimeUs(const Microsoft::WRL::ComPtr<IMFSample>& sample);

  static constexpr int DEFAULT_MAX_INPUT_SIZE = 8192;
  static constexpr int DEFAULT_BIT_RATE = 128000;

  AudioFormatType mCodecType;
  GUID mOutputSubType{};

  bool mConfigured = false;
  bool mInputDequeued = false;
  bool mInputEndOfStream = false;
  bool mOutputEndOfStream = false;
  bool mDrainRequested = false;
  bool mFormatReported = false;

  WmfTransformContext mTransform;

  AudioConfiguration mInputConfig;
  std::shared_ptr<MediaFormat> mInputFormat;
  std::shared_ptr<MediaFormat> mOutputFormat;

  std::shared_ptr<AVSampleBuffer> mSampleBuffer;
  std::shared_ptr<AVPacketBuffer> mPacketBuffer;

  std::deque<Microsoft::WRL::ComPtr<IMFSample>> mPendingInputSamples;
  std::deque<Microsoft::WRL::ComPtr<IMFSample>> mPendingOutputSamples;
  bool mWmfStarted = false;
};

} // namespace coral

#endif //CORAL_CODEC_WMF_AUDIO_ENCODER_H
