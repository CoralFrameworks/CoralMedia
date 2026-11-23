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

#ifndef CORAL_CODEC_DARWIN_AUDIO_DECODER_H
#define CORAL_CODEC_DARWIN_AUDIO_DECODER_H

#if defined(__APPLE__)

#include <codec/MediaCodec.h>

#include "DarwinMediaUtils.h"

#include <media/Configuration.h>
#include <media/Format.h>

#include <AudioToolbox/AudioToolbox.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

namespace coral {

/**
 * The AudioToolbox based audio decoder.
 *
 * The decoder is an `AudioConverter` which decompresses the compressed packets of the coral format
 * into interleaved linear PCM (16 bit signed or 32 bit float). The codecs the platform provides are
 * aac, alac, mp3, ac3, flac and opus, whose compressed stream descriptions and magic cookies
 * {@link darwin::makeCompressedFormat} and {@link darwin::audioDecoderMagicCookie} build. For aac the
 * decompression cookie must be the ESDS descriptor, a bare AudioSpecificConfig is rejected by the
 * converter.
 *
 * The converter pulls its input through a callback, and the callback of a decompressor has to hand
 * over every packet it is asked for in a single call: supplying one packet per call fails with
 * `'bada'` after a few packets. The queued packets are therefore gathered into one contiguous
 * scratch buffer and the complete buffer is supplied in one call.
 */
class DarwinAudioDecoder final : public AudioDecoder,
                                 public AVSampleBuffer::Owner<AVSampleBuffer> {
public:
  explicit DarwinAudioDecoder(bool outputFloat);

  ~DarwinAudioDecoder() override;

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

  void setOutputStartTimeUs(int64_t outputStartTimeUs) override;

  void recycleBuffer(std::shared_ptr<AVSampleBuffer> decodedBuffer) override;

  [[nodiscard]] const std::shared_ptr<MediaFormat>& getOutputFormat() const override {
    return mOutputFormat;
  }

  [[nodiscard]] const char* getName() const override {
    return "DarwinAudioDecoder";
  }

private:
  /** One compressed packet which waits to be decoded. */
  struct Packet {
    std::vector<uint8_t> data;
    int64_t timeUs = Definitions::kTimeUnset;
  };

  /**
   * Reads the output stream description the converter settled on.
   *
   * The decoder may change the sample rate and the channel count, e.g. the platform decoder of a
   * HE-AAC stream reports twice the sample rate of the compressed stream, so the reported output
   * format comes from the converter and not from the request.
   */
  bool readOutputDescription();

  /** Returns a cleared sample buffer, from the pool when one is available. */
  std::shared_ptr<AVSampleBuffer> obtainOutputBuffer();

  /** Wraps the decoded pcm of the scratch buffer into a sample buffer. */
  std::shared_ptr<AVSampleBuffer> createOutputBuffer(int64_t timeUs, UInt32 frameCount,
    size_t byteCount);

  /** Decodes a batch from the queued packets, null when the converter produced no frames. */
  std::shared_ptr<AVSampleBuffer> decodeQueuedPackets();

  /** Flushes the frames the converter still buffers, used once the input ended. */
  std::shared_ptr<AVSampleBuffer> drainConverter();

  /**
   * Returns the number of frames one queued packet decodes to.
   *
   * The compressed description states it for the codecs with a fixed frame size. A codec which does
   * not (flac, opus ...) declares it through the timestamps of the queued packets.
   */
  UInt32 resolveFramesPerPacket();

  /** Resets the state the input callback reads. */
  void resetInputProcState();

  [[nodiscard]] bool isAtLeastOutputStartTime(int64_t timeUs) const {
    return mOutputStartTimeUs == Definitions::kTimeUnset || timeUs == Definitions::kTimeUnset ||
           timeUs >= mOutputStartTimeUs;
  }

  /**
   * The `AudioConverterComplexInputDataProc` of the decompressor.
   *
   * It supplies all the packets of the current batch in one call and reports no data on every
   * following call, see the class comment.
   */
  static OSStatus inputProc(AudioConverterRef converter, UInt32* ioNumberDataPackets,
    AudioBufferList* ioData, AudioStreamPacketDescription** outDataPacketDescription,
    void* inUserData);

  static constexpr int DEFAULT_MAX_INPUT_SIZE = 64 * 1024;
  static constexpr size_t MINIMUM_OUTPUT_FRAMES = 4096;
  static constexpr size_t MAXIMUM_OUTPUT_FRAMES = 1 << 20;
  static constexpr size_t MAXIMUM_INPUT_BATCH_BYTES = 8 * 1024 * 1024;
  static constexpr UInt32 DEFAULT_INPUT_FRAMES_PER_PACKET = 1024;

  /** The number of frames one packet carries when the codec does not declare a fixed size. */
  [[nodiscard]] static UInt32 fallbackFramesPerPacket(AudioFormatType formatType);
  static constexpr size_t MAXIMUM_POOLED_BUFFERS = 4;

  AudioConverterRef mConverter = nullptr;
  bool mOutputFloat;
  bool mConfigured = false;
  bool mInputDequeued = false;
  bool mInputEndOfStream = false;
  bool mOutputEndOfStream = false;

  AudioStreamBasicDescription mInputAsbd{};
  int mOutputSampleRate = 0;
  int mOutputChannelCount = 0;
  int mBytesPerFrame = 0;
  UInt32 mOutputFramesPerPacket = 1;
  UInt32 mInputFramesPerPacket = 0;
  /**
   * Whether the frame size of a packet is not declared by the codec and is derived from the packet
   * timestamps. FLAC and Opus store blocks of different sizes; the ASBD still has to name a value
   * because `AudioConverterNew` rejects a zero.
   */
  bool mVariableFramesPerPacket = false;

  std::shared_ptr<MediaFormat> mInputFormat;
  std::shared_ptr<MediaFormat> mOutputFormat;
  AudioConfiguration mOutputConfig;

  std::shared_ptr<AVPacketBuffer> mInputBuffer;
  std::deque<Packet> mPackets;

  int64_t mOutputStartTimeUs = Definitions::kTimeUnset;
  int64_t mNextOutputTimeUs = Definitions::kTimeUnset;

  // The state of the decompression input callback, valid during one fill call.
  std::vector<uint8_t> mInputScratch;
  std::vector<AudioStreamPacketDescription> mInputDescriptions;
  UInt32 mInputPacketCount = 0;
  UInt32 mInputChannelCount = 0;
  bool mInputProcSupplied = false;

  std::vector<uint8_t> mOutputScratch;
  std::deque<std::shared_ptr<AVSampleBuffer>> mAvailableBuffers;
  mutable std::mutex mBufferLock;
};

} // namespace coral

#endif // defined(__APPLE__)

#endif // CORAL_CODEC_DARWIN_AUDIO_DECODER_H
