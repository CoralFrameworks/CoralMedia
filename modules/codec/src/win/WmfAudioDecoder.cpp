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

#include "WmfAudioDecoder.h"

#include "WmfMediaUtils.h"

#include <media/Definitions.h>
#include <media/Utilities.h>
#include <minilog/log.h>

#include <algorithm>
#include <vector>

namespace coral {
namespace {
constexpr int DEFAULT_MAX_INPUT_SIZE = 64 * 1024;

// The audio sub types which are supported by the decoder.
constexpr GUID kSubTypeAac = {
  0x00001610, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 }
};
} // namespace anonymous

WmfAudioDecoder::WmfAudioDecoder(const bool outputFloat)
  : mOutputFloat(outputFloat) {
  mWmfStarted = WmfMediaUtils::startup();
}

WmfAudioDecoder::~WmfAudioDecoder() {
  release();
  if (mWmfStarted) {
    WmfMediaUtils::shutdown();
  }
}

bool WmfAudioDecoder::configure(const std::shared_ptr<MediaFormat>& format) {
  if (!format) {
    MINILOG_ERROR("failed to configure the audio decoder: the format is null");
    return false;
  }

  const auto mimeType = format->getMimeType();
  if (mimeType.empty() || !MimeTypes::isAudio(mimeType)) {
    MINILOG_ERROR("failed to configure the audio decoder: invalid mime type: {}", mimeType);
    return false;
  }
  if (!WmfMediaUtils::mimeTypeToSubtype(mimeType, mInputSubType)) {
    MINILOG_ERROR("failed to configure the audio decoder: unsupported mime type: {}", mimeType);
    return false;
  }

  mInputFormat = format;

  if (!WmfMediaUtils::isTransformAvailable(false, mInputSubType, true)) {
    MINILOG_ERROR("no audio decoder is available for the mime type: {}", mimeType);
    return false;
  }

  if (!createCodec() || !createInputMediaType()) {
    release();
    return false;
  }

  if (!negotiateOutputType()) {
    release();
    return false;
  }

  auto bufferSize = format->getMaxInputSize();
  if (bufferSize <= 0) {
    bufferSize = DEFAULT_MAX_INPUT_SIZE;
  }
  mInputBuffer = std::make_shared<AVPacketBuffer>(MediaType::kMediaTypeAudio);
  mInputBuffer->ensureSpaceForWrite(static_cast<size_t>(bufferSize));

  mInputEndOfStream = false;
  mOutputEndOfStream = false;
  mDrainRequested = false;
  mInputDequeued = false;
  mConfigured = true;
  return true;
}

bool WmfAudioDecoder::createCodec() {
  auto transform = WmfMediaUtils::createTransform(false, mInputSubType, true);
  if (transform == nullptr) {
    MINILOG_ERROR("failed to create the audio decoder transform");
    return false;
  }
  mTransform.attach(std::move(transform));
  return true;
}

bool WmfAudioDecoder::createInputMediaType() {
  Microsoft::WRL::ComPtr<IMFMediaType> mediaType;
  auto result = ::MFCreateMediaType(&mediaType);
  if (FAILED(result) || mediaType == nullptr) {
    MINILOG_ERROR("MFCreateMediaType failed");
    return false;
  }

  WmfMediaUtils::fillAudioMediaType(mediaType.Get(), mInputFormat, mInputSubType);
  const auto channelCount = mInputFormat->getChannelCount();
  const auto sampleRate = mInputFormat->getSampleRate();
  if (sampleRate > 0 && channelCount > 0) {
    mediaType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
      static_cast<UINT32>(sampleRate * channelCount * 2));
  }
  mediaType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, FALSE);

  if (mInputSubType == kSubTypeAac) {
    const auto& csdList = mInputFormat->csdList();
    if (!csdList.empty() && !csdList[0].empty()) {
      const auto blob = WmfMediaUtils::createAacUserData(csdList[0]);
      mediaType->SetBlob(MF_MT_USER_DATA, blob.data(), static_cast<UINT32>(blob.size()));
    }
    mediaType->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
    mediaType->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
  }

  result = mTransform.setInputType(mediaType.Get());
  if (SUCCEEDED(result)) {
    return true;
  }
  MINILOG_DEBUG(
    "the requested audio decoder input type is rejected: {}, trying the available types",
    WmfMediaUtils::hresultToString(result));

  // Fall back to the input types reported by the transform.
  for (DWORD index = 0;; ++index) {
    Microsoft::WRL::ComPtr<IMFMediaType> candidate;
    if (FAILED(mTransform.get()->GetInputAvailableType(0, index, &candidate)) || candidate ==
        nullptr) {
      break;
    }
    GUID subType{};
    if (FAILED(candidate->GetGUID(MF_MT_SUBTYPE, &subType)) || subType != mInputSubType) {
      continue;
    }
    if (SUCCEEDED(mTransform.setInputType(candidate.Get()))) {
      return true;
    }
  }

  WmfMediaUtils::handleHresult(nullptr, result, "failed to set the audio decoder input type",
    mInputFormat->toString());
  return false;
}

bool WmfAudioDecoder::negotiateOutputType() {
  GUID sampleSubType{};
  if (!WmfMediaUtils::sampleFormatToSubtype(
    mOutputFloat ? AudioSampleFormat::kFloat : AudioSampleFormat::kInt16, sampleSubType)) {
    return false;
  }

  auto sampleRate = mInputFormat->getSampleRate();
  auto channelCount = mInputFormat->getChannelCount();
  if (sampleRate <= 0) {
    sampleRate = Definitions::DEFAULT_SAMPLE_RATE;
  }
  if (channelCount <= 0) {
    channelCount = Definitions::DEFAULT_CHANNEL_COUNT;
  }

  bool accepted = false;
  auto desiredType = WmfMediaUtils::createPcmMediaType(sampleSubType, sampleRate, channelCount);
  if (desiredType != nullptr && SUCCEEDED(mTransform.setOutputType(desiredType.Get()))) {
    accepted = true;
  }
  if (!accepted) {
    // The requested PCM format is not accepted, for example the sample rate of a HE-AAC stream is
    // doubled by the decoder, so the output types offered by the transform are used.
    auto candidate = mTransform.findOutputType(MFMediaType_Audio, sampleSubType);
    if (candidate == nullptr && sampleSubType != MFAudioFormat_Float) {
      candidate = mTransform.findOutputType(MFMediaType_Audio, MFAudioFormat_Float);
    }
    if (candidate != nullptr && SUCCEEDED(mTransform.setOutputType(candidate.Get()))) {
      desiredType = candidate;
      accepted = true;
    }
  }
  if (!accepted) {
    MINILOG_ERROR("failed to negotiate the audio decoder output type");
    return false;
  }

  Microsoft::WRL::ComPtr<IMFMediaType> outputType;
  if (FAILED(mTransform.getOutputType(&outputType)) || outputType == nullptr) {
    outputType = desiredType;
  }

  UINT32 rate = 0;
  UINT32 channels = 0;
  GUID subType{};
  outputType->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
  outputType->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
  outputType->GetGUID(MF_MT_SUBTYPE, &subType);

  auto sampleFormat = WmfMediaUtils::subtypeToSampleFormat(subType);
  if (sampleFormat == AudioSampleFormat::kInvalid) {
    sampleFormat = AudioSampleFormat::kInt16;
  }
  mOutputConfig = AudioConfiguration(rate, channels, sampleFormat);

  mOutputFormat = MediaFormat::CreateAudioFormat(MimeTypes::AUDIO_RAW,
    static_cast<int>(rate),
    static_cast<int>(channels));
  mOutputFormat->setSampleFormat(sampleFormat);
  mOutputFormat->setAudioFormatType(AudioFormatType::kAudioFormatTypeLinearPCM);

  MINILOG_DEBUG("audio decoder output format: {}", mOutputFormat->toString());
  return true;
}

void WmfAudioDecoder::start() {
  if (!mTransform.isValid()) {
    return;
  }
  mTransform.notifyStartOfStream();
  mStarted = true;
}

void WmfAudioDecoder::stop() {
  if (!mTransform.isValid()) {
    return;
  }
  mTransform.flush();
  mStarted = false;
}

void WmfAudioDecoder::flush() {
  if (mTransform.isValid()) {
    mTransform.flush();
  }
  mPendingInputSamples.clear();
  mInputDequeued = false;
  mInputEndOfStream = false;
  mOutputEndOfStream = false;
  mDrainRequested = false;
}

void WmfAudioDecoder::release() {
  mTransform.release();
  mTransform.reset();
  mPendingInputSamples.clear();
  mInputBuffer.reset();
  discardBuffers();
  mInputDequeued = false;
  mInputEndOfStream = true;
  mOutputEndOfStream = true;
  mConfigured = false;
  mStarted = false;
}

void WmfAudioDecoder::discardBuffers() {
  std::lock_guard<std::mutex> lock(mPendingLock);
  mAvailableBuffers.clear();
}

std::shared_ptr<AVPacketBuffer> WmfAudioDecoder::dequeueInputBuffer() {
  if (!mConfigured || mInputEndOfStream || mInputDequeued || mInputBuffer == nullptr) {
    return nullptr;
  }
  mInputDequeued = true;
  mInputBuffer->clear();
  return mInputBuffer;
}

void WmfAudioDecoder::queueInputBuffer(std::shared_ptr<AVPacketBuffer> inputBuffer) {
  if (!inputBuffer || !mTransform.isValid()) {
    return;
  }
  mInputDequeued = false;

  if (inputBuffer->isEndOfStream()) {
    mInputEndOfStream = true;
    return;
  }
  if (inputBuffer->size() == 0) {
    return;
  }

  const auto sample = WmfMediaUtils::createSample(inputBuffer->array(), inputBuffer->size(),
    inputBuffer->timeUs(),
    static_cast<uint32_t>(inputBuffer->isKeyFrame()
                            ? AVBuffer::kKeyFrame
                            : AVBuffer::kNone));
  if (sample == nullptr) {
    return;
  }

  const auto result = mTransform.processInput(sample.Get());
  if (result == MF_E_NOTACCEPTING) {
    // The transform is not able to accept the sample yet, it is pushed before the output is
    // drained the next time.
    mPendingInputSamples.emplace_back(sample);
    return;
  }
  if (FAILED(result)) {
    WmfMediaUtils::handleHresult(nullptr, result, "the audio decoder rejected the input sample");
  }
}

bool WmfAudioDecoder::pushInputSamples() {
  while (!mPendingInputSamples.empty()) {
    const auto result = mTransform.processInput(mPendingInputSamples.front().Get());
    if (result == MF_E_NOTACCEPTING) {
      return false;
    }
    mPendingInputSamples.pop_front();
    if (FAILED(result)) {
      WmfMediaUtils::handleHresult(nullptr, result,
        "the audio decoder rejected the input sample");
    }
  }
  return true;
}

std::shared_ptr<AVSampleBuffer> WmfAudioDecoder::createEndOfStreamBuffer() {
  auto buffer = obtainOutputBuffer();
  buffer->setFlags(AVBufferFlags::kEndOfStream);
  return buffer;
}

std::shared_ptr<AVSampleBuffer> WmfAudioDecoder::obtainOutputBuffer() {
  std::shared_ptr<AVSampleBuffer> sampleBuffer;
  {
    std::lock_guard<std::mutex> lock(mPendingLock);
    if (!mAvailableBuffers.empty()) {
      sampleBuffer = mAvailableBuffers.front();
      mAvailableBuffers.pop_front();
    }
  }
  if (sampleBuffer == nullptr) {
    sampleBuffer = std::make_shared<AVSampleBuffer>(this, mOutputConfig);
  }
  sampleBuffer->setConfiguration(mOutputConfig);
  sampleBuffer->clear();
  return sampleBuffer;
}

std::shared_ptr<AVSampleBuffer> WmfAudioDecoder::dequeueOutputBuffer() {
  if (!mConfigured || !mTransform.isValid()) {
    return nullptr;
  }
  if (mOutputEndOfStream) {
    return createEndOfStreamBuffer();
  }

  const auto inputsPushed = pushInputSamples();

  while (true) {
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

    Microsoft::WRL::ComPtr<IMFSample> sample;
    const auto result = mTransform.processOutput(sample);

    if (result == MF_E_TRANSFORM_STREAM_CHANGE) {
      MINILOG_DEBUG("the audio decoder requested an output type renegotiation");
      if (!negotiateOutputType()) {
        return nullptr;
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
        WmfMediaUtils::handleHresult(nullptr, result, "failed to decode the audio sample");
      }
      return nullptr;
    }

    auto outputBuffer = obtainOutputBuffer();
    if (!fillOutputBuffer(outputBuffer, sample)) {
      return nullptr;
    }
    if (!isAtLeastOutputStartTime(outputBuffer->timeUs())) {
      outputBuffer->shouldBeSkipped = true;
    }
    return outputBuffer;
  }
}

bool WmfAudioDecoder::fillOutputBuffer(const std::shared_ptr<AVSampleBuffer>& outputBuffer,
  const Microsoft::WRL::ComPtr<IMFSample>& sample) {
  Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
  auto result = sample->ConvertToContiguousBuffer(&buffer);
  if (FAILED(result) || buffer == nullptr) {
    WmfMediaUtils::handleHresult(nullptr, result, "the decoded sample has no contiguous buffer");
    return false;
  }

  BYTE* data = nullptr;
  DWORD maxLength = 0;
  DWORD currentLength = 0;
  result = buffer->Lock(&data, &maxLength, &currentLength);
  if (FAILED(result)) {
    WmfMediaUtils::handleHresult(nullptr, result, "failed to lock the decoded audio buffer");
    return false;
  }

  const auto bytesPerFrame = MediaUtils::getBytesPerFrame(mOutputConfig.sampleFormat,
    mOutputConfig.channelCount);
  auto length = currentLength;
  if (bytesPerFrame > 0) {
    length = length / bytesPerFrame * bytesPerFrame;
  }

  outputBuffer->init(sampleTimeUs(sample), static_cast<int>(length));
  const auto outputData = outputBuffer->data();
  outputData->position(0);
  if (length > 0) {
    outputData->put(data, length);
  }
  outputData->flip();
  buffer->Unlock();

  UINT32 discontinuity = 0;
  if (SUCCEEDED(sample->GetUINT32(MFSampleExtension_Discontinuity, &discontinuity)) &&
      discontinuity) {
    outputBuffer->addFlag(AVBufferFlags::kFirstSample);
  }
  return true;
}

int64_t WmfAudioDecoder::sampleTimeUs(const Microsoft::WRL::ComPtr<IMFSample>& sample) {
  LONGLONG time = 0;
  if (FAILED(sample->GetSampleTime(&time))) {
    return Definitions::kTimeUnset;
  }
  return time / 10; // 100ns to us
}

void WmfAudioDecoder::releaseOutputBuffer(const std::shared_ptr<AVSampleBuffer>& outputBuffer,
  const bool render) {
  if (outputBuffer) {
    outputBuffer->release();
  }
}

void WmfAudioDecoder::recycleBuffer(std::shared_ptr<AVSampleBuffer> decodedBuffer) {
  if (!decodedBuffer) {
    return;
  }
  decodedBuffer->clear();
  std::lock_guard<std::mutex> lock(mPendingLock);
  mAvailableBuffers.push_back(std::move(decodedBuffer));
}

void WmfAudioDecoder::signalEndOfInputStream() {
  mInputEndOfStream = true;
}

void WmfAudioDecoder::setOutputStartTimeUs(const int64_t outputStartTimeUs) {
  mOutputStartTimeUs = outputStartTimeUs;
}
} // namespace coral
