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

#ifndef CORAL_CODEC_DARWIN_AUDIO_ENCODER_H
#define CORAL_CODEC_DARWIN_AUDIO_ENCODER_H

#if defined(__APPLE__)

#include <codec/MediaCodec.h>

#include "DarwinMediaUtils.h"

#include <media/Configuration.h>
#include <media/Format.h>

#include <AudioToolbox/AudioToolbox.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

namespace coral {

/**
 * The AudioToolbox based audio encoder.
 *
 * The encoder is an `AudioConverter` which compresses interleaved linear PCM into the requested
 * codec; the platform provides an aac and an alac encoder. The output format it reports to the
 * caller carries the magic cookie the converter produces: for aac the AudioSpecificConfig, so that
 * a decoder only needs the reported format to decode the packets again.
 *
 * The first packet {@link dequeueOutputBuffer} returns announces the output format, the caller has
 * to look for {@link AVPacketBuffer::isFormatChanged} before it consumes the encoded packets.
 */
class DarwinAudioEncoder final : public AudioEncoder {
public:
  explicit DarwinAudioEncoder(AudioFormatType codecType);

  ~DarwinAudioEncoder() override;

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

  [[nodiscard]] const std::shared_ptr<MediaFormat>& getOutputFormat() const override {
    return mOutputFormat;
  }

  [[nodiscard]] const char* getName() const override {
    return "DarwinAudioEncoder";
  }

private:
  /** One queued piece of interleaved pcm data. */
  struct Chunk {
    std::vector<uint8_t> data;
    size_t offset = 0;
    int64_t timeUs = Definitions::kTimeUnset;
  };

  /** Applies the requested bit rate and builds the reported output format. */
  bool createOutputFormat(const std::shared_ptr<MediaFormat>& format);

  /** Reads the output description and the maximum packet size of the converter. */
  bool readOutputDescription();

  /** Encodes at most one packet, returns null when the converter produced nothing. */
  std::shared_ptr<AVPacketBuffer> encodeNextPacket();

  /** The number of complete pcm frames which still wait in the queue. */
  [[nodiscard]] size_t queuedFrameCount() const;

  /** Completes the tail of the stream with silence so that the encoder can emit its last packet. */
  void padQueuedChunks();

  std::shared_ptr<AVPacketBuffer> createEndOfStreamBuffer();

  /** Resets the state of the compression input callback. */
  void resetInputProcState();

  /**
   * The `AudioConverterComplexInputDataProc` of the compressor.
   *
   * It supplies at most one packet worth of pcm frames per call and reports no data once the queue
   * is empty.
   */
  static OSStatus inputProc(AudioConverterRef converter, UInt32* ioNumberDataPackets,
    AudioBufferList* ioData, AudioStreamPacketDescription** outDataPacketDescription,
    void* inUserData);

  static constexpr int DEFAULT_MAX_PACKET_SIZE = 4096;
  static constexpr int DEFAULT_INPUT_FRAMES_PER_CALL = 1024;
  static constexpr int MAXIMUM_INPUT_FRAMES_PER_CALL = 16384;

  AudioConverterRef mConverter = nullptr;
  AudioFormatType mCodecType;

  bool mConfigured = false;
  bool mInputDequeued = false;
  bool mInputEndOfStream = false;
  bool mOutputEndOfStream = false;
  bool mFormatReported = false;

  int mSampleRate = 0;
  int mChannelCount = 0;
  int mBytesPerFrame = 0;
  int mBitrate = 0;
  UInt32 mMaxPacketSize = DEFAULT_MAX_PACKET_SIZE;
  UInt32 mOutputFramesPerPacket = 0;
  UInt32 mInputFramesPerCall = DEFAULT_INPUT_FRAMES_PER_CALL;

  std::shared_ptr<MediaFormat> mInputFormat;
  std::shared_ptr<MediaFormat> mOutputFormat;
  AudioConfiguration mInputConfig;

  std::shared_ptr<AVSampleBuffer> mInputBuffer;
  std::shared_ptr<AVPacketBuffer> mPacketBuffer;
  std::deque<Chunk> mChunks;
  int64_t mEncodedFrames = 0;
  int64_t mTimeBaseUs = Definitions::kTimeUnset;

  // The state of the compression input callback, valid during one fill call.
  std::vector<uint8_t> mInputScratch;
  UInt32 mFramesConsumed = 0;
  bool mInputProcSupplied = false;
  bool mInputProcCalled = false;

  std::vector<uint8_t> mPacketScratch;
};

} // namespace coral

#endif // defined(__APPLE__)

#endif // CORAL_CODEC_DARWIN_AUDIO_ENCODER_H
