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

#include "DarwinAudioEncoder.h"

#include <media/Definitions.h>
#include <media/PacketBuffer.h>
#include <media/SampleBuffer.h>
#include <media/Types.h>
#include <media/Utilities.h>

#include <minilog/log.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace coral {

DarwinAudioEncoder::DarwinAudioEncoder(const AudioFormatType codecType)
  : mCodecType(codecType) {
}

DarwinAudioEncoder::~DarwinAudioEncoder() {
  release();
}

bool DarwinAudioEncoder::configure(const std::shared_ptr<MediaFormat>& format) {
  release();

  if (format == nullptr) {
    MINILOG_ERROR("failed to configure the audio encoder: the format is null");
    return false;
  }

  auto codecType = mCodecType;
  if (codecType == AudioFormatType::kAudioFormatTypeInvalid) {
    codecType = format->getAudioFormatType();
  }
  if (codecType == AudioFormatType::kAudioFormatTypeInvalid ||
      codecType == AudioFormatType::kAudioFormatTypeLinearPCM ||
      !darwin::isAudioEncoderSupported(codecType)) {
    MINILOG_ERROR("failed to configure the audio encoder: unsupported audio format type: {}",
      convertToText(codecType));
    return false;
  }
  mCodecType = codecType;

  const auto sampleRate = format->getSampleRate();
  const auto channelCount = format->getChannelCount();
  if (sampleRate <= 0 || channelCount <= 0) {
    MINILOG_ERROR("failed to configure the audio encoder: invalid format: {}", format->toString());
    return false;
  }

  auto sampleFormat = format->getSampleFormat();
  if (sampleFormat != AudioSampleFormat::kInt16 &&
      sampleFormat != AudioSampleFormat::kFloat &&
      sampleFormat != AudioSampleFormat::kInt24 &&
      sampleFormat != AudioSampleFormat::kInt32) {
    MINILOG_DEBUG("the audio encoder does not support the pcm format: {}, using 16 bit signed pcm",
      convertToText(sampleFormat));
    sampleFormat = AudioSampleFormat::kInt16;
  }

  const auto inputAsbd = darwin::makePcmFormat(sampleRate, channelCount, sampleFormat);
  const auto outputAsbd = darwin::makeCompressedFormat(mCodecType, sampleRate, channelCount);
  const auto status = AudioConverterNew(&inputAsbd, &outputAsbd, &mConverter);
  if (status != noErr || mConverter == nullptr) {
    mConverter = nullptr;
    return darwin::logStatus(status, "AudioConverterNew failed for the audio encoder",
      format->toString());
  }

  mInputFormat = format;
  mSampleRate = sampleRate;
  mChannelCount = channelCount;
  mBytesPerFrame = darwin::bytesPerFrameOf(inputAsbd);
  if (mBytesPerFrame <= 0) {
    darwin::logFailure("the audio encoder input format has an invalid frame size",
      format->toString());
    release();
    return false;
  }

  if (!readOutputDescription() || !createOutputFormat(format)) {
    release();
    return false;
  }

  mInputConfig = AudioConfiguration(static_cast<uint64_t>(mSampleRate),
    static_cast<uint32_t>(mChannelCount), sampleFormat);
  mInputBuffer = std::make_shared<AVSampleBuffer>(mInputConfig);
  mPacketBuffer = std::make_shared<AVPacketBuffer>(MediaType::kMediaTypeAudio);

  mInputFramesPerCall = std::max<UInt32>(DEFAULT_INPUT_FRAMES_PER_CALL, mOutputFramesPerPacket);
  mInputFramesPerCall = std::min<UInt32>(mInputFramesPerCall, MAXIMUM_INPUT_FRAMES_PER_CALL);
  mInputScratch.resize(static_cast<size_t>(mInputFramesPerCall) *
                       static_cast<size_t>(mBytesPerFrame));
  mPacketScratch.resize(mMaxPacketSize);

  mInputDequeued = false;
  mInputEndOfStream = false;
  mOutputEndOfStream = false;
  mFormatReported = false;
  mEncodedFrames = 0;
  mTimeBaseUs = Definitions::kTimeUnset;
  mConfigured = true;

  MINILOG_DEBUG("audio encoder input format: {}", mInputFormat->toString());
  MINILOG_DEBUG("audio encoder output format: {}", mOutputFormat->toString());
  return true;
}

bool DarwinAudioEncoder::readOutputDescription() {
  if (mConverter == nullptr) {
    return false;
  }

  AudioStreamBasicDescription actual{};
  UInt32 size = sizeof(actual);
  auto status = AudioConverterGetProperty(mConverter, kAudioConverterCurrentOutputStreamDescription,
    &size, &actual);
  if (status == noErr && actual.mFramesPerPacket > 0) {
    mOutputFramesPerPacket = actual.mFramesPerPacket;
  } else {
    // The codec does not report a fixed number of frames per packet, the number of frames the
    // encoder consumed is used to advance the presentation time instead.
    mOutputFramesPerPacket = 0;
    if (status != noErr) {
      MINILOG_DEBUG("the audio encoder has no fixed output packet size: {}",
        darwin::osStatusToString(status));
    }
  }
  if (mOutputFramesPerPacket == 0) {
    const auto framesPerPacket = darwin::defaultFramesPerPacket(mCodecType);
    if (framesPerPacket > 0) {
      mOutputFramesPerPacket = static_cast<UInt32>(framesPerPacket);
    }
  }

  UInt32 maxPacketSize = 0;
  size = sizeof(maxPacketSize);
  status = AudioConverterGetProperty(mConverter, kAudioConverterPropertyMaximumOutputPacketSize,
    &size, &maxPacketSize);
  if (status != noErr || maxPacketSize == 0) {
    maxPacketSize = DEFAULT_MAX_PACKET_SIZE;
  }
  mMaxPacketSize = std::max<UInt32>(maxPacketSize, DEFAULT_MAX_PACKET_SIZE);
  return true;
}

bool DarwinAudioEncoder::createOutputFormat(const std::shared_ptr<MediaFormat>& format) {
  auto bitrate = format->getAverageBitrate();
  if (bitrate <= 0) {
    bitrate = format->getBitrate();
  }
  if (bitrate <= 0) {
    bitrate = Definitions::DEFAULT_AUDIO_BITRATE;
  }
  mBitrate = bitrate;

  auto value = static_cast<UInt32>(bitrate);
  const auto status = AudioConverterSetProperty(mConverter, kAudioConverterEncodeBitRate,
    sizeof(value), &value);
  if (status != noErr) {
    // A lossless codec has no bit rate, the requested one is only a hint.
    MINILOG_DEBUG("the audio encoder ignored the bit rate {}: {}", bitrate,
      darwin::osStatusToString(status));
  }

  std::vector<uint8_t> cookie;
  UInt32 cookieSize = 0;
  if (AudioConverterGetPropertyInfo(mConverter, kAudioConverterCompressionMagicCookie, &cookieSize,
      nullptr) == noErr && cookieSize > 0) {
    cookie.resize(cookieSize);
    auto size = cookieSize;
    if (AudioConverterGetProperty(mConverter, kAudioConverterCompressionMagicCookie, &size,
        cookie.data()) == noErr) {
      cookie.resize(std::min<size_t>(cookie.size(), size));
    } else {
      cookie.clear();
    }
  }

  if (mCodecType == AudioFormatType::kAudioFormatTypeMpeg4AAC) {
    // The coral formats carry the AudioSpecificConfig of an aac stream, not the ESDS descriptor the
    // encoder produces.
    const auto asc = darwin::ascFromEsds(cookie);
    if (!asc.empty()) {
      cookie = asc;
    }
  }

  const auto mimeType = darwin::mimeTypeOfAudioFormat(mCodecType);
  if (mimeType.empty()) {
    darwin::logFailure("the audio encoder has no mime type", convertToText(mCodecType));
    return false;
  }

  mOutputFormat = MediaFormat::CreateAudioFormat(mimeType, mSampleRate, mChannelCount);
  if (mOutputFormat == nullptr) {
    darwin::logFailure("failed to create the audio encoder output format");
    return false;
  }
  mOutputFormat->setAudioFormatType(mCodecType);
  mOutputFormat->setSampleFormat(AudioSampleFormat::kInt16);
  mOutputFormat->setAverageBitrate(mBitrate);
  if (!cookie.empty()) {
    mOutputFormat->setCodecSpecificData(0, cookie);
  } else {
    MINILOG_DEBUG("the audio encoder did not provide a magic cookie for {}", convertToText(mCodecType));
  }
  return true;
}

void DarwinAudioEncoder::start() {
  if (!mConfigured) {
    return;
  }
  mInputDequeued = false;
  mInputEndOfStream = false;
  mOutputEndOfStream = false;
  mEncodedFrames = 0;
  mTimeBaseUs = Definitions::kTimeUnset;
}

void DarwinAudioEncoder::stop() {
  flush();
}

void DarwinAudioEncoder::flush() {
  if (mConverter != nullptr) {
    // Drops the pcm frames the encoder buffers internally.
    AudioConverterReset(mConverter);
  }
  mChunks.clear();
  resetInputProcState();
  mInputDequeued = false;
  mInputEndOfStream = false;
  mOutputEndOfStream = false;
  mEncodedFrames = 0;
  mTimeBaseUs = Definitions::kTimeUnset;
}

void DarwinAudioEncoder::release() {
  if (mConverter != nullptr) {
    AudioConverterDispose(mConverter);
    mConverter = nullptr;
  }
  mChunks.clear();
  mInputBuffer.reset();
  mPacketBuffer.reset();
  mInputFormat.reset();
  mOutputFormat.reset();
  resetInputProcState();
  mInputScratch.clear();
  mPacketScratch.clear();

  mInputConfig = AudioConfiguration();
  mSampleRate = 0;
  mChannelCount = 0;
  mBytesPerFrame = 0;
  mBitrate = 0;
  mMaxPacketSize = DEFAULT_MAX_PACKET_SIZE;
  mOutputFramesPerPacket = 0;
  mInputFramesPerCall = DEFAULT_INPUT_FRAMES_PER_CALL;
  mEncodedFrames = 0;
  mTimeBaseUs = Definitions::kTimeUnset;
  mInputDequeued = false;
  mInputEndOfStream = true;
  mOutputEndOfStream = true;
  mFormatReported = false;
  mConfigured = false;
}

void DarwinAudioEncoder::resetInputProcState() {
  mFramesConsumed = 0;
  mInputProcSupplied = false;
  mInputProcCalled = false;
}

std::shared_ptr<AVSampleBuffer> DarwinAudioEncoder::dequeueInputBuffer() {
  if (!mConfigured || mInputEndOfStream || mInputDequeued || mInputBuffer == nullptr) {
    return nullptr;
  }
  mInputDequeued = true;
  mInputBuffer->clear();
  return mInputBuffer;
}

void DarwinAudioEncoder::queueInputBuffer(std::shared_ptr<AVSampleBuffer> inputBuffer) {
  if (inputBuffer == nullptr) {
    return;
  }
  mInputDequeued = false;

  if (!mConfigured) {
    return;
  }
  if (inputBuffer->isEndOfStream()) {
    mInputEndOfStream = true;
    return;
  }

  const auto size = inputBuffer->size();
  const auto* data = inputBuffer->array();
  if (size == 0 || data == nullptr) {
    return;
  }

  // The caller keeps the ownership of its buffer, the bytes are copied into the queue.
  Chunk chunk;
  chunk.data.assign(data, data + size);
  chunk.timeUs = inputBuffer->timeUs();
  mChunks.emplace_back(std::move(chunk));
}

OSStatus DarwinAudioEncoder::inputProc(AudioConverterRef converter, UInt32* ioNumberDataPackets,
  AudioBufferList* ioData, AudioStreamPacketDescription** outDataPacketDescription,
  void* inUserData) {
  static_cast<void>(converter);
  if (outDataPacketDescription != nullptr) {
    *outDataPacketDescription = nullptr;
  }

  auto* encoder = static_cast<DarwinAudioEncoder*>(inUserData);
  if (encoder == nullptr || ioNumberDataPackets == nullptr || ioData == nullptr) {
    if (ioNumberDataPackets != nullptr) {
      *ioNumberDataPackets = 0;
    }
    return noErr;
  }

  encoder->mInputProcCalled = true;
  if (encoder->mInputProcSupplied || encoder->mBytesPerFrame <= 0) {
    *ioNumberDataPackets = 0;
    return noErr;
  }
  encoder->mInputProcSupplied = true;

  const auto bytesPerFrame = static_cast<size_t>(encoder->mBytesPerFrame);
  auto framesToSupply = std::min<UInt32>(*ioNumberDataPackets, encoder->mInputFramesPerCall);
  framesToSupply = std::min<UInt32>(framesToSupply,
    static_cast<UInt32>(MAXIMUM_INPUT_FRAMES_PER_CALL));
  if (framesToSupply == 0) {
    *ioNumberDataPackets = 0;
    return noErr;
  }

  const auto wanted = static_cast<size_t>(framesToSupply) * bytesPerFrame;
  auto& scratch = encoder->mInputScratch;
  size_t written = 0;
  while (written < wanted && !encoder->mChunks.empty()) {
    auto& chunk = encoder->mChunks.front();
    const auto available = chunk.data.size() - chunk.offset;
    if (available < bytesPerFrame) {
      // A trailing partial frame cannot be encoded.
      encoder->mChunks.pop_front();
      continue;
    }
    const auto toCopy = std::min(available, wanted - written) / bytesPerFrame * bytesPerFrame;
    std::memcpy(scratch.data() + written, chunk.data.data() + chunk.offset, toCopy);
    chunk.offset += toCopy;
    written += toCopy;
    if (chunk.offset >= chunk.data.size()) {
      encoder->mChunks.pop_front();
    }
  }

  if (written == 0) {
    *ioNumberDataPackets = 0;
    return noErr;
  }

  const auto frames = static_cast<UInt32>(written / bytesPerFrame);
  ioData->mNumberBuffers = 1;
  ioData->mBuffers[0].mNumberChannels = static_cast<UInt32>(encoder->mChannelCount);
  ioData->mBuffers[0].mData = scratch.data();
  ioData->mBuffers[0].mDataByteSize = static_cast<UInt32>(written);
  *ioNumberDataPackets = frames;

  encoder->mFramesConsumed += frames;
  return noErr;
}

std::shared_ptr<AVPacketBuffer> DarwinAudioEncoder::encodeNextPacket() {
  if (mConverter == nullptr || mPacketBuffer == nullptr || mPacketScratch.empty()) {
    return nullptr;
  }

  if (mTimeBaseUs == Definitions::kTimeUnset && mEncodedFrames == 0 && !mChunks.empty()) {
    const auto chunkTimeUs = mChunks.front().timeUs;
    if (chunkTimeUs != Definitions::kTimeUnset) {
      mTimeBaseUs = chunkTimeUs;
    }
  }

  AudioBufferList outputData{};
  outputData.mNumberBuffers = 1;
  outputData.mBuffers[0].mNumberChannels = static_cast<UInt32>(mChannelCount);
  outputData.mBuffers[0].mData = mPacketScratch.data();
  outputData.mBuffers[0].mDataByteSize = mMaxPacketSize;

  mInputProcSupplied = false;
  mInputProcCalled = false;
  mFramesConsumed = 0;
  UInt32 outputPackets = 1;
  const auto status = AudioConverterFillComplexBuffer(mConverter, &DarwinAudioEncoder::inputProc,
    this, &outputPackets, &outputData, nullptr);

  if (outputPackets == 0) {
    if (status != noErr) {
      MINILOG_DEBUG("the audio encoder produced no packet: {}",
        darwin::osStatusToString(status));
    }
    return nullptr;
  }

  auto size = static_cast<size_t>(outputData.mBuffers[0].mDataByteSize);
  if (size == 0) {
    return nullptr;
  }
  size = std::min(size, static_cast<size_t>(mMaxPacketSize));

  mPacketBuffer->ensureSpaceForWrite(size);
  const auto data = mPacketBuffer->data();
  if (data == nullptr) {
    return nullptr;
  }
  data->position(0);
  data->put(mPacketScratch.data(), size);
  data->flip();

  const auto base = mTimeBaseUs != Definitions::kTimeUnset ? mTimeBaseUs : 0;
  mPacketBuffer->timeUs(base + mEncodedFrames * Definitions::kMicrosPerSecond /
                              std::max(1, mSampleRate));
  mPacketBuffer->addFlag(AVBufferFlags::kKeyFrame);

  mEncodedFrames += mOutputFramesPerPacket > 0 ? mOutputFramesPerPacket : mFramesConsumed;
  return mPacketBuffer;
}

std::shared_ptr<AVPacketBuffer> DarwinAudioEncoder::createEndOfStreamBuffer() {
  if (mPacketBuffer == nullptr) {
    return nullptr;
  }
  mPacketBuffer->clear();
  mPacketBuffer->setFlags(AVBufferFlags::kEndOfStream);
  return mPacketBuffer;
}

size_t DarwinAudioEncoder::queuedFrameCount() const {
  if (mBytesPerFrame <= 0) {
    return 0;
  }
  size_t bytes = 0;
  for (const auto& chunk : mChunks) {
    bytes += chunk.data.size() - chunk.offset;
  }
  return bytes / static_cast<size_t>(mBytesPerFrame);
}

void DarwinAudioEncoder::padQueuedChunks() {
  if (mBytesPerFrame <= 0) {
    return;
  }
  const auto frames = queuedFrameCount();
  if (frames == 0 || frames >= mInputFramesPerCall) {
    return;
  }

  Chunk chunk;
  chunk.data.assign(static_cast<size_t>(mInputFramesPerCall - frames) *
                     static_cast<size_t>(mBytesPerFrame), 0);
  chunk.timeUs = Definitions::kTimeUnset;
  mChunks.emplace_back(std::move(chunk));
}

std::shared_ptr<AVPacketBuffer> DarwinAudioEncoder::dequeueOutputBuffer() {
  if (!mConfigured || mConverter == nullptr || mPacketBuffer == nullptr) {
    return nullptr;
  }

  // The consumers of an encoder read the output format from the first packet it returns.
  if (!mFormatReported) {
    mFormatReported = true;
    mPacketBuffer->clear();
    mPacketBuffer->setFormat(mOutputFormat);
    return mPacketBuffer;
  }

  if (mOutputEndOfStream) {
    return createEndOfStreamBuffer();
  }

  mPacketBuffer->clear();

  while (!mChunks.empty() && mChunks.front().offset >= mChunks.front().data.size()) {
    mChunks.pop_front();
  }

  if (mChunks.empty()) {
    if (!mInputEndOfStream) {
      return nullptr;
    }
    // The input ended: the frames the encoder still buffers are flushed, they may complete one more
    // packet. A partial packet is discarded by the converter.
    auto packet = encodeNextPacket();
    if (packet != nullptr) {
      return packet;
    }
    mOutputEndOfStream = true;
    return createEndOfStreamBuffer();
  }

  if (mInputEndOfStream) {
    // The frames of the tail do not fill a packet. The encoder pads its last packet with silence,
    // the padding is appended here so that the tail is not dropped when the input ends.
    padQueuedChunks();
  } else if (queuedFrameCount() < mInputFramesPerCall) {
    // The encoder asks for a complete packet worth of pcm frames. Supplying a partial batch makes it
    // ask for more input, and a converter whose callback then reports no data does not accept new
    // input until it is reset; waiting for the next frames avoids that.
    return nullptr;
  }

  for (int attempt = 0; attempt < 2; ++attempt) {
    auto packet = encodeNextPacket();
    if (packet != nullptr) {
      return packet;
    }
    if (mInputProcCalled) {
      // The encoder took the frames and buffers them until a packet is complete.
      return nullptr;
    }
    // The converter did not run, it is still in the finished state an earlier fill left it in.
    MINILOG_DEBUG("the audio encoder did not accept the queued frames, resetting the converter");
    AudioConverterReset(mConverter);
  }
  return nullptr;
}

void DarwinAudioEncoder::releaseOutputBuffer(const std::shared_ptr<AVPacketBuffer>& outputBuffer,
  const bool render) {
  // The packet buffer is owned by the encoder and reused for every packet.
  static_cast<void>(outputBuffer);
  static_cast<void>(render);
}

void DarwinAudioEncoder::signalEndOfInputStream() {
  mInputEndOfStream = true;
}

} // namespace coral
