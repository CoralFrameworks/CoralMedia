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

#include "WmfAudioEncoder.h"

#include "WmfMediaUtils.h"

#include <media/Definitions.h>
#include <media/Utilities.h>
#include <minilog/log.h>

#include <algorithm>

namespace coral {
WmfAudioEncoder::WmfAudioEncoder(const AudioFormatType codecType)
  : mCodecType(codecType) {
  mWmfStarted = WmfMediaUtils::startup();
}

WmfAudioEncoder::~WmfAudioEncoder() {
  release();
  if (mWmfStarted) {
    WmfMediaUtils::shutdown();
  }
}

bool WmfAudioEncoder::configure(const std::shared_ptr<MediaFormat>& format) {
  if (!format) {
    MINILOG_ERROR("failed to configure the audio encoder: the format is null");
    return false;
  }

  auto codecType = format->getAudioFormatType();
  if (codecType == AudioFormatType::kAudioFormatTypeInvalid ||
      codecType == AudioFormatType::kAudioFormatTypeLinearPCM) {
    codecType = mCodecType;
  }
  if (codecType == AudioFormatType::kAudioFormatTypeInvalid) {
    MINILOG_ERROR("failed to configure the audio encoder: unknown audio format type");
    return false;
  }
  mCodecType = codecType;

  if (!WmfMediaUtils::audioFormatTypeToSubtype(mCodecType, mOutputSubType)) {
    MINILOG_ERROR("failed to configure the audio encoder: unsupported audio format type: {}",
      convertToText(mCodecType));
    return false;
  }
  if (!WmfMediaUtils::isTransformAvailable(false, mOutputSubType, false)) {
    MINILOG_ERROR("no audio encoder is available for the format type: {}",
      convertToText(mCodecType));
    return false;
  }

  mInputFormat = format;
  auto sampleFormat = format->getSampleFormat();
  if (sampleFormat != AudioSampleFormat::kInt16 &&
      sampleFormat != AudioSampleFormat::kFloat &&
      sampleFormat != AudioSampleFormat::kInt32) {
    MINILOG_ERROR("the audio encoder does not support the pcm format: {}",
      convertToText(sampleFormat));
    return false;
  }

  mInputConfig = AudioConfiguration(
    static_cast<uint64_t>(format->getSampleRate()),
    static_cast<uint32_t>(format->getChannelCount()),
    sampleFormat);
  if (mInputConfig.isInvalid()) {
    MINILOG_ERROR("failed to configure the audio encoder: invalid input format");
    return false;
  }

  if (!createCodec() || !createOutputMediaType() || !createInputMediaType()) {
    release();
    return false;
  }

  mSampleBuffer = std::make_shared<AVSampleBuffer>(mInputConfig);
  auto maxInputBufferSize = format->getMaxInputSize();
  maxInputBufferSize = std::max(maxInputBufferSize, DEFAULT_MAX_INPUT_SIZE);
  mSampleBuffer->ensureSpaceForWrite(static_cast<size_t>(maxInputBufferSize));

  mPacketBuffer = std::make_shared<AVPacketBuffer>(MediaType::kMediaTypeAudio);

  mInputEndOfStream = false;
  mOutputEndOfStream = false;
  mDrainRequested = false;
  mInputDequeued = false;
  mFormatReported = false;
  mConfigured = true;
  return true;
}

bool WmfAudioEncoder::createCodec() {
  auto transform = WmfMediaUtils::createTransform(false, mOutputSubType, false);
  if (transform == nullptr) {
    MINILOG_ERROR("failed to create the audio encoder transform");
    return false;
  }
  mTransform.attach(std::move(transform));
  return true;
}

bool WmfAudioEncoder::createOutputMediaType() {
  Microsoft::WRL::ComPtr<IMFMediaType> mediaType;
  auto result = ::MFCreateMediaType(&mediaType);
  if (FAILED(result) || mediaType == nullptr) {
    MINILOG_ERROR("MFCreateMediaType failed");
    return false;
  }

  auto bitrate = mInputFormat->getBitrate();
  if (bitrate == MediaFormat::NO_VALUE || bitrate <= 0) {
    bitrate = DEFAULT_BIT_RATE;
  }

  mediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
  mediaType->SetGUID(MF_MT_SUBTYPE, mOutputSubType);
  mediaType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,
    static_cast<UINT32>(mInputConfig.sampleRate));
  mediaType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, static_cast<UINT32>(mInputConfig.channelCount));
  mediaType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
  mediaType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, static_cast<UINT32>(bitrate / 8));
  mediaType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 0);
  mediaType->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(bitrate));

  if (mOutputSubType == MFAudioFormat_AAC) {
    mediaType->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
    mediaType->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
  }

  result = mTransform.setOutputType(mediaType.Get());
  if (FAILED(result)) {
    WmfMediaUtils::handleHresult(nullptr, result, "failed to set the audio encoder output type");
    return false;
  }
  return true;
}

bool WmfAudioEncoder::createInputMediaType() {
  GUID sampleSubType{};
  if (!WmfMediaUtils::sampleFormatToSubtype(mInputConfig.sampleFormat, sampleSubType)) {
    return false;
  }
  auto mediaType = WmfMediaUtils::createPcmMediaType(sampleSubType,
    static_cast<int>(mInputConfig.sampleRate),
    static_cast<int>(mInputConfig.channelCount));
  if (mediaType == nullptr) {
    return false;
  }
  const auto result = mTransform.setInputType(mediaType.Get());
  if (FAILED(result)) {
    WmfMediaUtils::handleHresult(nullptr, result, "failed to set the audio encoder input type");
    return false;
  }
  return true;
}

bool WmfAudioEncoder::refreshOutputFormat() {
  Microsoft::WRL::ComPtr<IMFMediaType> outputType;
  if (FAILED(mTransform.getOutputType(&outputType)) || outputType == nullptr) {
    return false;
  }
  const auto format = WmfMediaUtils::convertToMediaFormat(outputType.Get(), true);
  if (format == nullptr) {
    return false;
  }
  format->setSampleFormat(mInputConfig.sampleFormat);
  format->setAudioFormatType(mCodecType);
  mOutputFormat = format;
  MINILOG_DEBUG("audio encoder output format: {}", mOutputFormat->toString());
  return true;
}

void WmfAudioEncoder::start() {
  if (!mTransform.isValid()) {
    return;
  }
  mTransform.notifyStartOfStream();
}

void WmfAudioEncoder::stop() {
  if (!mTransform.isValid()) {
    return;
  }
  mTransform.flush();
}

void WmfAudioEncoder::flush() {
  if (mTransform.isValid()) {
    mTransform.flush();
  }
  mPendingInputSamples.clear();
  mPendingOutputSamples.clear();
  mInputDequeued = false;
  mInputEndOfStream = false;
  mOutputEndOfStream = false;
  mDrainRequested = false;
}

void WmfAudioEncoder::release() {
  mTransform.release();
  mTransform.reset();
  mPendingInputSamples.clear();
  mPendingOutputSamples.clear();
  mSampleBuffer.reset();
  mPacketBuffer.reset();
  mInputDequeued = false;
  mInputEndOfStream = true;
  mOutputEndOfStream = true;
  mFormatReported = false;
  mConfigured = false;
}

std::shared_ptr<AVSampleBuffer> WmfAudioEncoder::dequeueInputBuffer() {
  if (!mConfigured || mInputEndOfStream || mInputDequeued || mSampleBuffer == nullptr) {
    return nullptr;
  }
  mInputDequeued = true;
  mSampleBuffer->clear();
  return mSampleBuffer;
}

void WmfAudioEncoder::queueInputBuffer(std::shared_ptr<AVSampleBuffer> inputBuffer) {
  if (!inputBuffer || !mTransform.isValid()) {
    return;
  }
  mInputDequeued = false;

  if (inputBuffer->isEndOfStream()) {
    mInputEndOfStream = true;
    return;
  }

  const auto size = inputBuffer->size();
  if (size == 0) {
    return;
  }

  const auto sample = WmfMediaUtils::createSample(inputBuffer->array(), size,
    inputBuffer->timeUs(), AVBuffer::kNone);
  if (sample == nullptr) {
    return;
  }
  const auto durationUs = MediaUtils::getPcmDurationUs(
    static_cast<int32_t>(mInputConfig.sampleRate),
    static_cast<int32_t>(mInputConfig.channelCount), mInputConfig.sampleFormat, size);
  if (durationUs > 0) {
    sample->SetSampleDuration(durationUs * 10); // us to 100ns
  }

  const auto result = mTransform.processInput(sample.Get());
  if (result == MF_E_NOTACCEPTING) {
    mPendingInputSamples.emplace_back(sample);
    return;
  }
  if (FAILED(result)) {
    WmfMediaUtils::handleHresult(nullptr, result, "the audio encoder rejected the input sample");
  }
}

bool WmfAudioEncoder::pushInputSamples() {
  while (!mPendingInputSamples.empty()) {
    const auto result = mTransform.processInput(mPendingInputSamples.front().Get());
    if (result == MF_E_NOTACCEPTING) {
      return false;
    }
    mPendingInputSamples.pop_front();
    if (FAILED(result)) {
      WmfMediaUtils::handleHresult(nullptr, result,
        "the audio encoder rejected the input sample");
    }
  }
  return true;
}

std::shared_ptr<AVPacketBuffer> WmfAudioEncoder::createEndOfStreamBuffer() {
  mPacketBuffer->clear();
  mPacketBuffer->setFlags(AVBufferFlags::kEndOfStream);
  return mPacketBuffer;
}

std::shared_ptr<AVPacketBuffer> WmfAudioEncoder::dequeueOutputBuffer() {
  if (!mConfigured || !mTransform.isValid() || mPacketBuffer == nullptr) {
    return nullptr;
  }
  if (mOutputEndOfStream) {
    return createEndOfStreamBuffer();
  }

  mPacketBuffer->clear();

  const auto inputsPushed = pushInputSamples();

  while (true) {
    Microsoft::WRL::ComPtr<IMFSample> sample;
    if (!mPendingOutputSamples.empty()) {
      sample = mPendingOutputSamples.front();
      mPendingOutputSamples.pop_front();
    } else {
      if (mTransform.queryOutputState() == WmfTransformContext::OutputState::kNeedMoreInput) {
        if (mInputEndOfStream && inputsPushed) {
          if (!mDrainRequested) {
            mTransform.drain();
            mDrainRequested = true;
            continue;
          }
          mOutputEndOfStream = true;
          return createEndOfStreamBuffer();
        }
        return nullptr;
      }
      const auto result = mTransform.processOutput(sample);
      if (result == MF_E_TRANSFORM_STREAM_CHANGE) {
        MINILOG_DEBUG("the audio encoder changed its output type");
        if (refreshOutputFormat()) {
          mFormatReported = true;
          mPacketBuffer->setFormat(mOutputFormat);
          return mPacketBuffer;
        }
        continue;
      }
      if (result == MF_E_TRANSFORM_NEED_MORE_INPUT) {
        if (mInputEndOfStream && inputsPushed) {
          if (!mDrainRequested) {
            mTransform.drain();
            mDrainRequested = true;
            continue;
          }
          mOutputEndOfStream = true;
          return createEndOfStreamBuffer();
        }
        return nullptr;
      }
      if (FAILED(result) || sample == nullptr) {
        if (FAILED(result)) {
          WmfMediaUtils::handleHresult(nullptr, result, "failed to encode the audio sample");
        }
        return nullptr;
      }
    }

    if (!mFormatReported) {
      // The transform did not signal a stream change, the output format is available once the first
      // sample has been encoded.
      mFormatReported = true;
      if (refreshOutputFormat()) {
        mPendingOutputSamples.push_front(sample);
        mPacketBuffer->clear();
        mPacketBuffer->setFormat(mOutputFormat);
        return mPacketBuffer;
      }
    }

    if (!fillOutputBuffer(mPacketBuffer, sample)) {
      return nullptr;
    }
    return mPacketBuffer;
  }
}

bool WmfAudioEncoder::fillOutputBuffer(const std::shared_ptr<AVPacketBuffer>& packetBuffer,
  const Microsoft::WRL::ComPtr<IMFSample>& sample) {
  Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
  auto result = sample->ConvertToContiguousBuffer(&buffer);
  if (FAILED(result) || buffer == nullptr) {
    WmfMediaUtils::handleHresult(nullptr, result, "the encoded sample has no contiguous buffer");
    return false;
  }

  BYTE* data = nullptr;
  DWORD maxLength = 0;
  DWORD currentLength = 0;
  result = buffer->Lock(&data, &maxLength, &currentLength);
  if (FAILED(result)) {
    WmfMediaUtils::handleHresult(nullptr, result, "failed to lock the encoded audio buffer");
    return false;
  }

  packetBuffer->timeUs(sampleTimeUs(sample));
  packetBuffer->ensureSpaceForWrite(currentLength);
  const auto outputData = packetBuffer->data();
  outputData->position(0);
  if (currentLength > 0) {
    outputData->put(data, currentLength);
  }
  outputData->flip();
  buffer->Unlock();

  UINT32 cleanPoint = 0;
  if (!FAILED(sample->GetUINT32(MFSampleExtension_CleanPoint, &cleanPoint)) && cleanPoint) {
    packetBuffer->addFlag(AVBufferFlags::kKeyFrame);
  }
  return true;
}

int64_t WmfAudioEncoder::sampleTimeUs(const Microsoft::WRL::ComPtr<IMFSample>& sample) {
  LONGLONG time = 0;
  if (FAILED(sample->GetSampleTime(&time))) {
    return Definitions::kTimeUnset;
  }
  return time / 10; // 100ns to us
}

void WmfAudioEncoder::releaseOutputBuffer(const std::shared_ptr<AVPacketBuffer>& outputBuffer,
  const bool render) {
  // the packet buffer is owned by the encoder and is reused.
}

void WmfAudioEncoder::signalEndOfInputStream() {
  mInputEndOfStream = true;
}

} // namespace coral
