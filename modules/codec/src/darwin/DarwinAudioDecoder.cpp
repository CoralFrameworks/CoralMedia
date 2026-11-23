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

#include "DarwinAudioDecoder.h"

#include <media/Definitions.h>
#include <media/PacketBuffer.h>
#include <media/SampleBuffer.h>
#include <media/Types.h>
#include <media/Utilities.h>

#include <minilog/log.h>

#include <algorithm>
#include <cstddef>
#include <utility>

namespace coral {

DarwinAudioDecoder::DarwinAudioDecoder(const bool outputFloat)
  : mOutputFloat(outputFloat) {
}

DarwinAudioDecoder::~DarwinAudioDecoder() {
  release();
}

bool DarwinAudioDecoder::configure(const std::shared_ptr<MediaFormat>& format) {
  release();

  if (format == nullptr) {
    MINILOG_ERROR("failed to configure the audio decoder: the format is null");
    return false;
  }

  const auto mimeType = format->getMimeType();
  if (mimeType.empty() || !MimeTypes::isAudio(mimeType)) {
    MINILOG_ERROR("failed to configure the audio decoder: invalid mime type: {}", mimeType);
    return false;
  }

  auto formatType = format->getAudioFormatType();
  if (formatType == AudioFormatType::kAudioFormatTypeInvalid) {
    formatType = MediaUtils::convertToAudioFormatType(mimeType);
  }
  if (formatType == AudioFormatType::kAudioFormatTypeInvalid ||
      !darwin::isAudioDecoderSupported(formatType)) {
    MINILOG_ERROR("failed to configure the audio decoder: unsupported audio format type: {}",
      mimeType);
    return false;
  }

  const auto sampleRate = format->getSampleRate();
  const auto channelCount = format->getChannelCount();
  if (sampleRate <= 0 || channelCount <= 0) {
    MINILOG_ERROR("failed to configure the audio decoder: invalid format: {}", format->toString());
    return false;
  }

  mInputAsbd = darwin::makeCompressedFormat(formatType, sampleRate, channelCount);
  mInputFramesPerPacket = mInputAsbd.mFramesPerPacket;
  mVariableFramesPerPacket = false;
  if (mInputFramesPerPacket == 0) {
    mVariableFramesPerPacket = true;
    const auto defaultFrames = darwin::defaultFramesPerPacket(formatType);
    // The frame size of a codec which does not declare one is derived from the packet timestamps
    // while the packets are decoded. The ASBD still has to name one: AudioConverterNew rejects a
    // frames per packet of zero.
    mInputFramesPerPacket = defaultFrames > 0 ? static_cast<UInt32>(defaultFrames)
                                              : fallbackFramesPerPacket(formatType);
    mInputAsbd.mFramesPerPacket = mInputFramesPerPacket;
  }
  const auto outputAsbd = darwin::makePcmFormat(sampleRate, channelCount,
    mOutputFloat ? AudioSampleFormat::kFloat : AudioSampleFormat::kInt16);

  auto status = AudioConverterNew(&mInputAsbd, &outputAsbd, &mConverter);
  if (status != noErr || mConverter == nullptr) {
    mConverter = nullptr;
    return darwin::logStatus(status, "AudioConverterNew failed for the audio decoder",
      format->toString());
  }

  // The decompression cookie has to be set before the first decode. For aac it is the ESDS
  // descriptor, the converter rejects a bare AudioSpecificConfig.
  const auto cookie = darwin::audioDecoderMagicCookie(*format);
  if (!cookie.empty()) {
    status = AudioConverterSetProperty(mConverter, kAudioConverterDecompressionMagicCookie,
      static_cast<UInt32>(cookie.size()), cookie.data());
    if (status != noErr) {
      darwin::logStatus(status, "failed to set the audio decoder magic cookie", format->toString());
    }
  } else if (formatType == AudioFormatType::kAudioFormatTypeMpeg4AAC) {
    MINILOG_WARN("the aac decoder has no magic cookie, only a self describing adts stream can be "
                 "decoded");
  }

  if (!readOutputDescription()) {
    release();
    return false;
  }

  mInputFormat = format;
  mOutputFormat = MediaFormat::CreateAudioFormat(MimeTypes::AUDIO_RAW, mOutputSampleRate,
    mOutputChannelCount);
  mOutputFormat->setSampleFormat(mOutputConfig.sampleFormat);
  mOutputFormat->setAudioFormatType(AudioFormatType::kAudioFormatTypeLinearPCM);

  auto maxInputSize = format->getMaxInputSize();
  if (maxInputSize < DEFAULT_MAX_INPUT_SIZE) {
    maxInputSize = DEFAULT_MAX_INPUT_SIZE;
  }
  mInputBuffer = std::make_shared<AVPacketBuffer>(MediaType::kMediaTypeAudio);
  mInputBuffer->ensureSpaceForWrite(static_cast<size_t>(maxInputSize));

  mInputDequeued = false;
  mInputEndOfStream = false;
  mOutputEndOfStream = false;
  mNextOutputTimeUs = Definitions::kTimeUnset;
  mConfigured = true;

  MINILOG_DEBUG("audio decoder input format: {}", mInputFormat->toString());
  MINILOG_DEBUG("audio decoder output format: {}", mOutputFormat->toString());
  return true;
}

bool DarwinAudioDecoder::readOutputDescription() {
  if (mConverter == nullptr) {
    return false;
  }

  AudioStreamBasicDescription actual{};
  UInt32 size = sizeof(actual);
  const auto status = AudioConverterGetProperty(mConverter,
    kAudioConverterCurrentOutputStreamDescription, &size, &actual);
  if (status != noErr) {
    return darwin::logStatus(status, "failed to query the audio decoder output description");
  }

  auto sampleFormat = darwin::sampleFormatOf(actual);
  if (sampleFormat == AudioSampleFormat::kInvalid) {
    // The description does not carry a usable linear pcm flag, the requested format is delivered.
    sampleFormat = mOutputFloat ? AudioSampleFormat::kFloat : AudioSampleFormat::kInt16;
    const auto bitsPerChannel = sampleFormat == AudioSampleFormat::kFloat ? 32 : 16;
    actual.mBytesPerFrame = actual.mChannelsPerFrame * (bitsPerChannel / 8);
    actual.mBytesPerPacket = actual.mBytesPerFrame;
    actual.mFramesPerPacket = 1;
  }

  mOutputConfig = AudioConfiguration(static_cast<uint64_t>(actual.mSampleRate),
    actual.mChannelsPerFrame, sampleFormat);
  if (mOutputConfig.isInvalid()) {
    darwin::logFailure("the audio decoder reported an invalid output format");
    return false;
  }

  mBytesPerFrame = darwin::bytesPerFrameOf(actual);
  if (mBytesPerFrame <= 0) {
    darwin::logFailure("the audio decoder reported an invalid output frame size");
    return false;
  }

  mOutputSampleRate = static_cast<int>(actual.mSampleRate);
  mOutputChannelCount = static_cast<int>(actual.mChannelsPerFrame);
  mOutputFramesPerPacket = actual.mFramesPerPacket > 0 ? actual.mFramesPerPacket : 1;
  return true;
}

void DarwinAudioDecoder::start() {
  if (!mConfigured) {
    return;
  }
  mInputDequeued = false;
  mInputEndOfStream = false;
  mOutputEndOfStream = false;
  mNextOutputTimeUs = Definitions::kTimeUnset;
}

void DarwinAudioDecoder::stop() {
  flush();
}

void DarwinAudioDecoder::flush() {
  if (mConverter != nullptr) {
    // Drops the frames the converter buffers internally.
    AudioConverterReset(mConverter);
  }
  mPackets.clear();
  resetInputProcState();
  mInputDequeued = false;
  mInputEndOfStream = false;
  mOutputEndOfStream = false;
  mNextOutputTimeUs = Definitions::kTimeUnset;
}

void DarwinAudioDecoder::release() {
  if (mConverter != nullptr) {
    AudioConverterDispose(mConverter);
    mConverter = nullptr;
  }
  mPackets.clear();
  mInputBuffer.reset();
  mInputFormat.reset();
  mOutputFormat.reset();
  resetInputProcState();
  mInputScratch.clear();
  mOutputScratch.clear();
  {
    std::lock_guard<std::mutex> lock(mBufferLock);
    mAvailableBuffers.clear();
  }

  mOutputConfig = AudioConfiguration();
  mInputAsbd = AudioStreamBasicDescription{};
  mOutputSampleRate = 0;
  mOutputChannelCount = 0;
  mBytesPerFrame = 0;
  mOutputFramesPerPacket = 1;
  mInputDequeued = false;
  mInputEndOfStream = true;
  mOutputEndOfStream = true;
  mNextOutputTimeUs = Definitions::kTimeUnset;
  mOutputStartTimeUs = Definitions::kTimeUnset;
  mConfigured = false;
}

void DarwinAudioDecoder::resetInputProcState() {
  mInputScratch.clear();
  mInputDescriptions.clear();
  mInputPacketCount = 0;
  mInputChannelCount = 0;
  mInputProcSupplied = false;
}

std::shared_ptr<AVPacketBuffer> DarwinAudioDecoder::dequeueInputBuffer() {
  if (!mConfigured || mInputEndOfStream || mInputDequeued || mInputBuffer == nullptr) {
    return nullptr;
  }
  mInputDequeued = true;
  mInputBuffer->clear();
  return mInputBuffer;
}

void DarwinAudioDecoder::queueInputBuffer(std::shared_ptr<AVPacketBuffer> inputBuffer) {
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
  Packet packet;
  packet.data.assign(data, data + size);
  packet.timeUs = inputBuffer->timeUs();
  mPackets.emplace_back(std::move(packet));
}

OSStatus DarwinAudioDecoder::inputProc(AudioConverterRef converter, UInt32* ioNumberDataPackets,
  AudioBufferList* ioData, AudioStreamPacketDescription** outDataPacketDescription,
  void* inUserData) {
  static_cast<void>(converter);
  auto* decoder = static_cast<DarwinAudioDecoder*>(inUserData);
  if (decoder == nullptr || ioNumberDataPackets == nullptr || ioData == nullptr) {
    if (ioNumberDataPackets != nullptr) {
      *ioNumberDataPackets = 0;
    }
    return noErr;
  }

  // The complete batch was supplied by the previous call; the decompressor may not be asked for the
  // packets one at a time.
  if (decoder->mInputProcSupplied || decoder->mInputPacketCount == 0 ||
      decoder->mInputScratch.empty()) {
    *ioNumberDataPackets = 0;
    return noErr;
  }
  decoder->mInputProcSupplied = true;

  ioData->mNumberBuffers = 1;
  ioData->mBuffers[0].mNumberChannels = decoder->mInputChannelCount;
  ioData->mBuffers[0].mData = decoder->mInputScratch.data();
  ioData->mBuffers[0].mDataByteSize = static_cast<UInt32>(decoder->mInputScratch.size());
  *ioNumberDataPackets = decoder->mInputPacketCount;
  if (outDataPacketDescription != nullptr) {
    *outDataPacketDescription = decoder->mInputDescriptions.data();
  }
  return noErr;
}

std::shared_ptr<AVSampleBuffer> DarwinAudioDecoder::obtainOutputBuffer() {
  std::shared_ptr<AVSampleBuffer> buffer;
  {
    std::lock_guard<std::mutex> lock(mBufferLock);
    if (!mAvailableBuffers.empty()) {
      buffer = std::move(mAvailableBuffers.front());
      mAvailableBuffers.pop_front();
    }
  }
  if (buffer == nullptr) {
    buffer = std::make_shared<AVSampleBuffer>(this, mOutputConfig);
  }
  buffer->setConfiguration(mOutputConfig);
  buffer->clear();
  return buffer;
}

std::shared_ptr<AVSampleBuffer> DarwinAudioDecoder::createOutputBuffer(const int64_t timeUs,
  const UInt32 frameCount, const size_t byteCount) {
  auto outputBuffer = obtainOutputBuffer();
  outputBuffer->init(timeUs, static_cast<uint32_t>(byteCount));
  const auto data = outputBuffer->data();
  data->position(0);
  if (byteCount > 0) {
    data->put(mOutputScratch.data(), byteCount);
  }
  data->flip();
  if (!isAtLeastOutputStartTime(timeUs)) {
    outputBuffer->shouldBeSkipped = true;
  }

  if (mOutputSampleRate > 0) {
    mNextOutputTimeUs = timeUs == Definitions::kTimeUnset
                          ? Definitions::kTimeUnset
                          : timeUs + static_cast<int64_t>(frameCount) *
                                       Definitions::kMicrosPerSecond / mOutputSampleRate;
  }
  return outputBuffer;
}

std::shared_ptr<AVSampleBuffer> DarwinAudioDecoder::dequeueOutputBuffer() {
  if (!mConfigured || mConverter == nullptr) {
    return nullptr;
  }

  if (!mPackets.empty()) {
    return decodeQueuedPackets();
  }
  if (!mInputEndOfStream) {
    return nullptr;
  }

  // The input ended: the frames the converter still buffers are flushed before the stream ends.
  if (!mOutputEndOfStream) {
    auto outputBuffer = drainConverter();
    if (outputBuffer != nullptr) {
      return outputBuffer;
    }
    mOutputEndOfStream = true;
  }

  auto outputBuffer = obtainOutputBuffer();
  outputBuffer->setFlags(AVBufferFlags::kEndOfStream);
  return outputBuffer;
}

UInt32 DarwinAudioDecoder::fallbackFramesPerPacket(const AudioFormatType formatType) {
  switch (formatType) {
    case AudioFormatType::kAudioFormatTypeFlac:
      // The stream description CoreMedia reports for a FLAC track names 1152 frames per packet,
      // which is the block size the converter expects before it reads the frame headers.
      return 1152;
    case AudioFormatType::kAudioFormatTypeOpus:
      return 960;
    case AudioFormatType::kAudioFormatTypeVorbis:
      return 1024;
    default:
      return DEFAULT_INPUT_FRAMES_PER_PACKET;
  }
}

UInt32 DarwinAudioDecoder::resolveFramesPerPacket() {
  if (!mVariableFramesPerPacket || mPackets.size() < 2) {
    return std::max<UInt32>(mInputFramesPerPacket, 1);
  }

  // A codec with a variable frame size (flac, opus ...) reports the number of frames of a packet
  // through its duration, which is the timestamp difference of two consecutive packets.
  const auto firstTimeUs = mPackets[0].timeUs;
  const auto secondTimeUs = mPackets[1].timeUs;
  if (firstTimeUs == Definitions::kTimeUnset || secondTimeUs == Definitions::kTimeUnset ||
      mInputAsbd.mSampleRate <= 0) {
    return std::max<UInt32>(mInputFramesPerPacket, 1);
  }
  const auto deltaUs = secondTimeUs - firstTimeUs;
  if (deltaUs <= 0) {
    return std::max<UInt32>(mInputFramesPerPacket, 1);
  }
  const auto frames = static_cast<UInt32>(static_cast<double>(deltaUs) *
                                          mInputAsbd.mSampleRate / Definitions::kMicrosPerSecond);
  if (frames == 0) {
    return std::max<UInt32>(mInputFramesPerPacket, 1);
  }
  // Rounding down keeps the capacity below what the packets can produce.
  mInputFramesPerPacket = frames;
  return frames;
}

std::shared_ptr<AVSampleBuffer> DarwinAudioDecoder::decodeQueuedPackets() {
  // The decompression input callback supplies the whole batch in one call. The output capacity must
  // not be larger than what the batch can produce: a converter whose output is not full asks for
  // more input, and one whose callback then reports no data does not accept new input until it is
  // reset. Losing the decoder state to a reset is avoided by asking for exactly the expected size.
  for (int attempt = 0; attempt < 2; ++attempt) {
    const auto framesPerPacket = static_cast<size_t>(resolveFramesPerPacket());
    const auto maxPacketsPerCall = std::max<size_t>(1, MAXIMUM_OUTPUT_FRAMES / framesPerPacket);
    auto packetCount = std::min(mPackets.size(), maxPacketsPerCall);

    size_t batchBytes = 0;
    size_t fittingPackets = 0;
    while (fittingPackets < packetCount &&
           batchBytes + mPackets[fittingPackets].data.size() <= MAXIMUM_INPUT_BATCH_BYTES) {
      batchBytes += mPackets[fittingPackets].data.size();
      ++fittingPackets;
    }
    // At least one packet has to be supplied, otherwise the converter would never make progress.
    packetCount = std::clamp<size_t>(fittingPackets, 1, mPackets.size());

    mInputScratch.clear();
    mInputScratch.reserve(batchBytes);
    mInputDescriptions.clear();
    mInputDescriptions.reserve(packetCount);
    for (size_t index = 0; index < packetCount; ++index) {
      const auto& packet = mPackets[index];
      AudioStreamPacketDescription description{};
      description.mStartOffset = static_cast<SInt64>(mInputScratch.size());
      description.mVariableFramesInPacket = 0;
      description.mDataByteSize = static_cast<UInt32>(packet.data.size());
      mInputDescriptions.push_back(description);
      mInputScratch.insert(mInputScratch.end(), packet.data.begin(), packet.data.end());
    }

    const auto firstTimeUs = mPackets.front().timeUs;
    const auto capacityFrames = std::min<size_t>(packetCount * framesPerPacket,
      MAXIMUM_OUTPUT_FRAMES);
    const auto capacityBytes = capacityFrames * static_cast<size_t>(mBytesPerFrame);
    if (mOutputScratch.size() < capacityBytes) {
      mOutputScratch.resize(capacityBytes);
    }

    AudioBufferList outputData{};
    outputData.mNumberBuffers = 1;
    outputData.mBuffers[0].mNumberChannels = static_cast<UInt32>(mOutputChannelCount);
    outputData.mBuffers[0].mData = mOutputScratch.data();
    outputData.mBuffers[0].mDataByteSize = static_cast<UInt32>(capacityBytes);

    mInputPacketCount = static_cast<UInt32>(packetCount);
    mInputChannelCount = static_cast<UInt32>(mInputAsbd.mChannelsPerFrame);
    mInputProcSupplied = false;

    UInt32 outputPackets = static_cast<UInt32>(capacityFrames);
    const auto status = AudioConverterFillComplexBuffer(mConverter, &DarwinAudioDecoder::inputProc,
      this, &outputPackets, &outputData, nullptr);
    const auto supplied = mInputProcSupplied;

    // Only the packets the converter actually took are consumed: it may serve the fill from the
    // frames it buffers and ask for the queued packets on a later call.
    if (supplied) {
      mPackets.erase(mPackets.begin(),
        mPackets.begin() + static_cast<std::ptrdiff_t>(packetCount));
    }
    resetInputProcState();

    const auto outputFrames = static_cast<size_t>(outputPackets) * mOutputFramesPerPacket;
    if (outputFrames > 0) {
      if (status != noErr) {
        // A batch that also produced frames is delivered, the error only concerns the rest.
        MINILOG_DEBUG("the audio decoder reported {} after producing {} frames",
          darwin::osStatusToString(status), outputFrames);
      }

      const auto outputBytes = outputFrames * static_cast<size_t>(mBytesPerFrame);
      auto byteCount = static_cast<size_t>(outputData.mBuffers[0].mDataByteSize);
      if (byteCount == 0 || byteCount > outputBytes) {
        byteCount = outputBytes;
      }
      if (byteCount > capacityBytes) {
        byteCount = capacityBytes;
      }

      const auto outputTimeUs = mNextOutputTimeUs != Definitions::kTimeUnset ? mNextOutputTimeUs
                                                                            : firstTimeUs;
      return createOutputBuffer(outputTimeUs, static_cast<UInt32>(outputFrames), byteCount);
    }

    if (status != noErr) {
      darwin::logStatus(status, "failed to decode the audio packets");
      return nullptr;
    }
    if (!supplied) {
      // The converter did not take the input, it is still in the finished state a previous fill
      // left it in. The reset lets it accept the batch again.
      MINILOG_DEBUG("the audio decoder did not accept the queued packets, resetting the converter");
      AudioConverterReset(mConverter);
      continue;
    }

    if (mNextOutputTimeUs == Definitions::kTimeUnset) {
      mNextOutputTimeUs = firstTimeUs;
    }
    return nullptr;
  }
  return nullptr;
}

std::shared_ptr<AVSampleBuffer> DarwinAudioDecoder::drainConverter() {
  const auto capacityFrames = std::max<size_t>(MINIMUM_OUTPUT_FRAMES,
    static_cast<size_t>(mInputFramesPerPacket));
  const auto capacityBytes = capacityFrames * static_cast<size_t>(mBytesPerFrame);
  if (mOutputScratch.size() < capacityBytes) {
    mOutputScratch.resize(capacityBytes);
  }

  AudioBufferList outputData{};
  outputData.mNumberBuffers = 1;
  outputData.mBuffers[0].mNumberChannels = static_cast<UInt32>(mOutputChannelCount);
  outputData.mBuffers[0].mData = mOutputScratch.data();
  outputData.mBuffers[0].mDataByteSize = static_cast<UInt32>(capacityBytes);

  mInputPacketCount = 0;
  mInputChannelCount = static_cast<UInt32>(mInputAsbd.mChannelsPerFrame);
  mInputProcSupplied = false;

  UInt32 outputPackets = static_cast<UInt32>(capacityFrames);
  const auto status = AudioConverterFillComplexBuffer(mConverter, &DarwinAudioDecoder::inputProc,
    this, &outputPackets, &outputData, nullptr);
  resetInputProcState();

  const auto outputFrames = static_cast<size_t>(outputPackets) * mOutputFramesPerPacket;
  if (outputFrames == 0) {
    if (status != noErr) {
      MINILOG_DEBUG("the audio decoder had no buffered frames: {}",
        darwin::osStatusToString(status));
    }
    return nullptr;
  }

  const auto outputBytes = outputFrames * static_cast<size_t>(mBytesPerFrame);
  auto byteCount = static_cast<size_t>(outputData.mBuffers[0].mDataByteSize);
  if (byteCount == 0 || byteCount > outputBytes) {
    byteCount = outputBytes;
  }
  if (byteCount > capacityBytes) {
    byteCount = capacityBytes;
  }

  const auto outputTimeUs = mNextOutputTimeUs != Definitions::kTimeUnset ? mNextOutputTimeUs : 0;
  return createOutputBuffer(outputTimeUs, static_cast<UInt32>(outputFrames), byteCount);
}

void DarwinAudioDecoder::releaseOutputBuffer(const std::shared_ptr<AVSampleBuffer>& outputBuffer,
  const bool render) {
  static_cast<void>(render);
  if (outputBuffer != nullptr) {
    outputBuffer->release();
  }
}

void DarwinAudioDecoder::recycleBuffer(std::shared_ptr<AVSampleBuffer> decodedBuffer) {
  if (decodedBuffer == nullptr) {
    return;
  }
  decodedBuffer->clear();
  std::lock_guard<std::mutex> lock(mBufferLock);
  if (mAvailableBuffers.size() < MAXIMUM_POOLED_BUFFERS) {
    mAvailableBuffers.push_back(std::move(decodedBuffer));
  }
}

void DarwinAudioDecoder::signalEndOfInputStream() {
  mInputEndOfStream = true;
}

void DarwinAudioDecoder::setOutputStartTimeUs(const int64_t outputStartTimeUs) {
  mOutputStartTimeUs = outputStartTimeUs;
}

} // namespace coral
