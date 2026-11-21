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
#ifndef CORAL_CODEC_SIMPLE_AUDIO_DECODER_H
#define CORAL_CODEC_SIMPLE_AUDIO_DECODER_H

#include "private/ffcodec/FfmpegUtils.h"
#include "private/ffcodec/SimpleDecoder.h"

#include <media/Configuration.h>
#include <media/Format.h>
#include <media/PacketBuffer.h>
#include <media/SampleBuffer.h>
#include <media/Types.h>

namespace coral {

/** Ffmpeg audio decoder use own decode thread. */
class SimpleAudioDecoder final : public SimpleDecoder<AVPacketBuffer, AVSampleBuffer>,
                                 public AVSampleBuffer::Owner<AVSampleBuffer> {
private:
  static constexpr int INITIAL_OUTPUT_BUFFER_SIZE_16BIT = 65535;
  static constexpr int INITIAL_OUTPUT_BUFFER_SIZE_32BIT = INITIAL_OUTPUT_BUFFER_SIZE_16BIT * 2;

  static constexpr int CODEC_ERROR_INVALID_DATA = -1;
  static constexpr int CODEC_ERROR_OTHER = -2;
  static constexpr int CODEC_ERROR_EOF = -3;


  // The deleter for AVCodecContext
  struct AVCodecContextDeleter {
    void operator()(AVCodecContext* codecContext) const {
      SimpleAudioDecoder::releaseCodecContext(codecContext);
    }
  };

  std::unique_ptr<AVCodecContext, AVCodecContextDeleter> mCodecContext;
  std::unique_ptr<AVPacket, AVPacketDeleter> cachePacket;
  std::unique_ptr<AVFrame, AVFrameDeleter> cacheFrame;
  std::shared_ptr<MediaFormat> mOutputFormat;
  AudioConfiguration outputConfig;

  bool outputFloat;
  std::string codecName;
  std::vector<uint8_t> extraData;
  int outputBufferSize;
  std::unique_ptr<uint8_t[]> resampledBuffer;

public:
  explicit SimpleAudioDecoder(int numInputBuffers, int numOutputBuffers, bool outputFloat) noexcept;

  void configure(const std::shared_ptr<MediaFormat>& format) override;

  const std::shared_ptr<MediaFormat>& getOutputFormat() const {
    return mOutputFormat;
  }

  /** Returns the channel count of output audio. */
  int getChannelCount() const { return outputConfig.channelCount; }

  /** Returns the sample rate of output audio. */
  int getSampleRate() const { return outputConfig.sampleRate; }

  /** Returns the sample format of output audio. */
  AudioSampleFormat getSampleFormat() const { return outputConfig.sampleFormat; }

  const char* getName() const override {
    return "FfmpegAudioDecoder";
  }

  void recycleBuffer(std::shared_ptr<AVSampleBuffer> outputBuffer) override;

protected:
  std::shared_ptr<AVPacketBuffer> createInputBuffer() override;

  std::shared_ptr<AVSampleBuffer> createOutputBuffer() override;

  std::optional<Error> decodeBuffer(const std::shared_ptr<AVPacketBuffer>& inputBuffer,
      const std::shared_ptr<AVSampleBuffer>& outputBuffer, bool reset) override;

  void onRelease() override;

private:
  static std::vector<uint8_t>
  getExtraData(const std::string& mimeType, const std::vector<std::vector<uint8_t>>& initializationData);

  static std::vector<uint8_t>
  getAlacExtraData(const std::vector<std::vector<uint8_t>>& initializationData);

  static std::vector<uint8_t>
  getVorbisExtraData(const std::vector<std::vector<uint8_t>>& initializationData);

  static void releaseCodecContext(AVCodecContext* context);

  void ffmpegInitialize(const std::string& codecName,
    const std::vector<uint8_t>& extraData,
    bool outputFloat,
    int rawSampleRate,
    int rawChannelCount);

  AVCodecContext* createContext(const AVCodec* codec,
    const uint8_t* extraData,
    int extraSize,
    bool outputFloat,
    int rawSampleRate,
    int rawChannelCount) const;

  void resetDecoder();

  int decodePacket(AVPacket* inputPacket, uint8_t* outputBuffer, int outputSize);

};

} // namespace coral

#endif //CORAL_CODEC_SIMPLE_AUDIO_DECODER_H
