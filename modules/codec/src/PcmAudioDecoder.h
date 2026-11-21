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

#ifndef CORAL_PCM_AUDIO_DECODER_H
#define CORAL_PCM_AUDIO_DECODER_H

#include <codec/MediaCodec.h>

#include <deque>

namespace coral {

/**
 * Passes already decoded linear PCM through to the renderer.
 *
 * <p>A platform extractor may decode a compressed audio stream itself and deliver PCM samples, e.g.
 * the Windows source reader decoding the MP3 audio of an AVI file. The renderer still goes through
 * an audio decoder, so this decoder only turns the input packets into the sample buffers the
 * renderer consumes.
 */
class PcmAudioDecoder final : public AudioDecoder {
public:
  PcmAudioDecoder() = default;
  ~PcmAudioDecoder() override;

  bool configure(const std::shared_ptr<MediaFormat>& format) override;

  void start() override;

  void stop() override;

  void flush() override;

  void release() override;

  std::shared_ptr<AVPacketBuffer> dequeueInputBuffer() override;

  void queueInputBuffer(std::shared_ptr<AVPacketBuffer> inputBuffer) override;

  std::shared_ptr<AVSampleBuffer> dequeueOutputBuffer() override;

  void releaseOutputBuffer(const std::shared_ptr<AVSampleBuffer>& outputBuffer, bool render) override;

  void signalEndOfInputStream() override;

  [[nodiscard]] const std::shared_ptr<MediaFormat>& getOutputFormat() const override;

  [[nodiscard]] const char* getName() const override;

  void setOutputStartTimeUs(int64_t outputStartTimeUs) override;

private:
  std::shared_ptr<MediaFormat> mInputFormat;
  std::shared_ptr<MediaFormat> mOutputFormat;
  std::shared_ptr<AVPacketBuffer> mInputBuffer;
  std::deque<std::shared_ptr<AVSampleBuffer>> mOutputBuffers;
  bool mInputDequeued = false;
  bool mInputEndOfStream = false;
  int64_t mOutputStartTimeUs = Definitions::kTimeUnset;
};

} // namespace coral

#endif // CORAL_PCM_AUDIO_DECODER_H
