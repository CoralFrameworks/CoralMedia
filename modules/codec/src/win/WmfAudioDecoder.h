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

#ifndef CORAL_CODEC_WMF_AUDIO_DECODER_H
#define CORAL_CODEC_WMF_AUDIO_DECODER_H

#include <codec/MediaCodec.h>

#include "WmfTransform.h"

#include <media/Configuration.h>
#include <media/Format.h>

#include <deque>
#include <memory>
#include <mutex>

namespace coral {
/**
* The implementation of the AudioDecoder based on the Windows Media Foundation transforms, it
* supports the audio codecs which are available on the Windows platform (aac, mp3, ac3, flac ...).
*/
class WmfAudioDecoder final : public AudioDecoder,
                              public AVSampleBuffer::Owner<AVSampleBuffer> {
public:
  explicit WmfAudioDecoder(bool outputFloat);

  ~WmfAudioDecoder() override;

  bool configure(const std::shared_ptr<MediaFormat>& format) override;

  void start() override;

  void stop() override;

  void flush() override;

  void release() override;

  std::shared_ptr<AVPacketBuffer> dequeueInputBuffer() override;

  void queueInputBuffer(std::shared_ptr<AVPacketBuffer> inputBuffer) override;

  std::shared_ptr<AVSampleBuffer> dequeueOutputBuffer() override;

  void releaseOutputBuffer(const std::shared_ptr<AVSampleBuffer>& outputBuffer,
    bool render) override;

  void signalEndOfInputStream() override;

  void setOutputStartTimeUs(int64_t outputStartTimeUs) override;

  void recycleBuffer(std::shared_ptr<AVSampleBuffer> decodedBuffer) override;

  [[nodiscard]] const std::shared_ptr<MediaFormat>& getOutputFormat() const override {
    return mOutputFormat;
  }

  [[nodiscard]] const char* getName() const override {
    return "WmfAudioDecoder";
  }

private:
  /**
 * Creates the media foundation transform of the input codec.
 */
  bool createCodec();

  /**
 * Sets the compressed input media type, including the codec specific data.
 */
  bool createInputMediaType();

  /**
 * Builds the PCM output media type and negotiates it with the transform.
 */
  bool negotiateOutputType();

  /**
 * Pushes the queued input samples to the transform.
 *
 * @return false when the transform does not accept more input data yet.
 */
  bool pushInputSamples();

  /**
 * Creates a PCM sample buffer for the decoded output.
 */
  std::shared_ptr<AVSampleBuffer> obtainOutputBuffer();

  /**
 * Fills the buffer with the decoded pcm data of the sample.
 */
  bool fillOutputBuffer(const std::shared_ptr<AVSampleBuffer>& outputBuffer,
    const Microsoft::WRL::ComPtr<IMFSample>& sample);

  std::shared_ptr<AVSampleBuffer> createEndOfStreamBuffer();

  void discardBuffers();

  [[nodiscard]] bool isAtLeastOutputStartTime(int64_t timeUs) const {
    return mOutputStartTimeUs == Definitions::kTimeUnset || timeUs == Definitions::kTimeUnset ||
           timeUs >= mOutputStartTimeUs;
  }

  static int64_t sampleTimeUs(const Microsoft::WRL::ComPtr<IMFSample>& sample);

  bool mOutputFloat;
  bool mConfigured = false;
  bool mStarted = false;
  bool mInputDequeued = false;
  bool mInputEndOfStream = false;
  bool mOutputEndOfStream = false;
  bool mDrainRequested = false;

  WmfTransformContext mTransform;
  GUID mInputSubType{};

  std::shared_ptr<MediaFormat> mInputFormat;
  std::shared_ptr<MediaFormat> mOutputFormat;
  AudioConfiguration mOutputConfig;

  std::shared_ptr<AVPacketBuffer> mInputBuffer;
  std::deque<Microsoft::WRL::ComPtr<IMFSample>> mPendingInputSamples;

  int64_t mOutputStartTimeUs = Definitions::kTimeUnset;
  bool mWmfStarted = false;

  mutable std::mutex mPendingLock;
  std::deque<std::shared_ptr<AVSampleBuffer>> mAvailableBuffers;
};
} // namespace coral

#endif //CORAL_CODEC_WMF_AUDIO_DECODER_H
