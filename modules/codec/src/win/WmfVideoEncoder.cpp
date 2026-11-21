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

#include "WmfVideoEncoder.h"

#include "WmfMediaUtils.h"

#include <media/Definitions.h>
#include <minilog/log.h>

#include <codecapi.h>
#include <strmif.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace coral {

namespace {

/**
 * The pixel formats which are accepted as the encoder input, ordered by the preference.
 */
const GUID kPreferredInputSubTypes[] = {
  MFVideoFormat_NV12,
  MFVideoFormat_IYUV,
  MFVideoFormat_YV12,
};

void copyPlane(uint8_t* destination, const int destinationStride,
  const uint8_t* source, const int sourceStride, const int rows, const int rowBytes) {
  for (int row = 0; row < rows; ++row) {
    memcpy(destination + static_cast<ptrdiff_t>(row) * destinationStride,
      source + static_cast<ptrdiff_t>(row) * sourceStride, static_cast<size_t>(rowBytes));
  }
}

} // namespace anonymous

WmfVideoEncoder::WmfVideoEncoder(const VideoCodecType codecType)
  : mCodecType(codecType) {
  mWmfStarted = WmfMediaUtils::startup();
}

WmfVideoEncoder::~WmfVideoEncoder() {
  release();
  if (mWmfStarted) {
    WmfMediaUtils::shutdown();
  }
}

bool WmfVideoEncoder::configure(const std::shared_ptr<MediaFormat>& format,
  const std::shared_ptr<VideoSurface>& surface) {
  if (!format) {
    MINILOG_ERROR("failed to configure the video encoder: the format is null");
    return false;
  }
  if (surface != nullptr && surface->isValid()) {
    MINILOG_WARN("the audio/video surface is not supported by the Windows video encoder");
  }

  auto codecType = format->getVideoCodecType();
  if (codecType == VideoCodecType::kVideoCodecTypeInvalid) {
    codecType = mCodecType;
  }
  if (codecType == VideoCodecType::kVideoCodecTypeInvalid) {
    MINILOG_ERROR("failed to configure the video encoder: unknown video codec type");
    return false;
  }
  mCodecType = codecType;

  if (!WmfMediaUtils::videoCodecTypeToSubtype(mCodecType, mOutputSubType)) {
    MINILOG_ERROR("failed to configure the video encoder: unsupported video codec type: {}",
      convertToText(mCodecType));
    return false;
  }
  if (!WmfMediaUtils::isTransformAvailable(true, mOutputSubType, false)) {
    MINILOG_ERROR("no video encoder is available for the codec type: {}",
      convertToText(mCodecType));
    return false;
  }

  mWidth = format->getWidth() != MediaFormat::NO_VALUE ? format->getWidth() : 0;
  mHeight = format->getHeight() != MediaFormat::NO_VALUE ? format->getHeight() : 0;
  if (mWidth <= 0 || mHeight <= 0) {
    MINILOG_ERROR("failed to configure the video encoder: the frame size is unknown");
    return false;
  }
  // The 4:2:0 encoders require an even frame size.
  mWidth &= ~1;
  mHeight &= ~1;

  mFrameRate = format->getFrameRate() != MediaFormat::NO_VALUE &&
               format->getFrameRate() > 0
                 ? format->getFrameRate()
                 : DEFAULT_FRAME_RATE;

  mInputFormat = format;
  if (!createCodec() || !createOutputMediaType() || !createInputMediaType()) {
    release();
    return false;
  }

  if (!refreshOutputFormat()) {
    MINILOG_WARN("the video encoder output format is not available before the first frame");
  }

  mPacketBuffer = std::make_shared<AVPacketBuffer>(MediaType::kMediaTypeVideo);

  mInputEndOfStream = false;
  mOutputEndOfStream = false;
  mDrainRequested = false;
  mInputDequeued = false;
  mFormatReported = false;
  mFrameIndex = 0;
  mConfigured = true;
  return true;
}

bool WmfVideoEncoder::createCodec() {
  auto transform = WmfMediaUtils::createTransform(true, mOutputSubType, false);
  if (transform == nullptr) {
    MINILOG_ERROR("failed to create the video encoder transform");
    return false;
  }
  // The writer records the encoded samples in the order the encoder produces them and the muxer
  // writes them in presentation order. A B-frame is presented before the frame it is decoded after,
  // so the two orders differ and the muxer would reorder a valid stream into an undecodable one.
  // The encoder is asked for no B-frame, which keeps decode order and presentation order the same.
  Microsoft::WRL::ComPtr<ICodecAPI> codecApi;
  if (SUCCEEDED(transform->QueryInterface(IID_PPV_ARGS(&codecApi))) && codecApi != nullptr) {
    VARIANT value;
    VariantInit(&value);
    value.vt = VT_UI4;
    value.ulVal = 0;
    const auto bFrames = codecApi->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &value);
    if (FAILED(bFrames)) {
      MINILOG_WARN("the video encoder does not take the number of B-frames");
    }
    VariantClear(&value);
  }

  mTransform.attach(std::move(transform));
  return true;
}

bool WmfVideoEncoder::createOutputMediaType() {
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

  mediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  mediaType->SetGUID(MF_MT_SUBTYPE, mOutputSubType);
  mediaType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  mediaType->SetUINT64(MF_MT_FRAME_SIZE,
    ::Pack2UINT32AsUINT64(static_cast<UINT32>(mWidth),
      static_cast<UINT32>(mHeight)));
  mediaType->SetUINT64(MF_MT_FRAME_RATE,
    ::Pack2UINT32AsUINT64(static_cast<UINT32>(mFrameRate), 1));
  mediaType->SetUINT64(MF_MT_PIXEL_ASPECT_RATIO, ::Pack2UINT32AsUINT64(1, 1));
  mediaType->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(bitrate));

  result = mTransform.setOutputType(mediaType.Get());
  if (FAILED(result)) {
    WmfMediaUtils::handleHresult(nullptr, result, "failed to set the video encoder output type");
    return false;
  }
  return true;
}

bool WmfVideoEncoder::createInputMediaType() {
  // Picks the first pixel format which is offered by the encoder.
  Microsoft::WRL::ComPtr<IMFMediaType> selected;
  for (const auto& subType: kPreferredInputSubTypes) {
    for (DWORD index = 0;; ++index) {
      Microsoft::WRL::ComPtr<IMFMediaType> candidate;
      if (FAILED(mTransform.get()->GetInputAvailableType(0, index, &candidate)) ||
        candidate == nullptr) {
        break;
      }
      GUID candidateSubType{};
      if (FAILED(candidate->GetGUID(MF_MT_SUBTYPE, &candidateSubType)) ||
        candidateSubType != subType) {
        continue;
      }
      // The frame size and the frame rate of the input type must match the output type.
      candidate->SetUINT64(MF_MT_FRAME_SIZE,
        ::Pack2UINT32AsUINT64(static_cast<UINT32>(mWidth),
          static_cast<UINT32>(mHeight)));
      candidate->SetUINT64(MF_MT_FRAME_RATE,
        ::Pack2UINT32AsUINT64(static_cast<UINT32>(mFrameRate), 1));
      candidate->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
      if (SUCCEEDED(mTransform.setInputType(candidate.Get()))) {
        selected = candidate;
        break;
      }
    }
    if (selected != nullptr) {
      break;
    }
  }
  if (selected == nullptr) {
    MINILOG_ERROR("the video encoder does not offer a supported input pixel format");
    return false;
  }

  GUID subType{};
  selected->GetGUID(MF_MT_SUBTYPE, &subType);
  mInputSubType = subType;
  mSwapChromaPlanes = subType == MFVideoFormat_YV12;

  if (subType == MFVideoFormat_NV12) {
    mInputBuffer = std::make_shared<NV12PixelBuffer>(mWidth, mHeight);
  } else if (subType == MFVideoFormat_IYUV || subType == MFVideoFormat_YV12) {
    mInputBuffer = std::make_shared<I420PixelBuffer>(mWidth, mHeight);
  } else {
    MINILOG_ERROR("the video encoder input pixel format is not supported");
    return false;
  }
  return true;
}

bool WmfVideoEncoder::refreshOutputFormat() {
  Microsoft::WRL::ComPtr<IMFMediaType> outputType;
  if (FAILED(mTransform.getOutputType(&outputType)) || outputType == nullptr) {
    return false;
  }
  const auto format = WmfMediaUtils::convertToMediaFormat(outputType.Get(), true);
  if (format == nullptr) {
    return false;
  }
  format->setVideoCodecType(mCodecType);
  format->setWidth(mWidth);
  format->setHeight(mHeight);
  format->setFrameRate(mFrameRate);
  mOutputFormat = format;
  MINILOG_DEBUG("video encoder output format: {}", mOutputFormat->toString());
  return true;
}

void WmfVideoEncoder::start() {
  if (!mTransform.isValid()) {
    return;
  }
  mTransform.notifyStartOfStream();
}

void WmfVideoEncoder::stop() {
  if (mTransform.isValid()) {
    mTransform.flush();
  }
}

void WmfVideoEncoder::flush() {
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

void WmfVideoEncoder::release() {
  mTransform.release();
  mTransform.reset();
  mPendingInputSamples.clear();
  mPendingOutputSamples.clear();
  mInputBuffer.reset();
  mPacketBuffer.reset();
  mInputDequeued = false;
  mInputEndOfStream = true;
  mOutputEndOfStream = true;
  mFormatReported = false;
  mConfigured = false;
}

std::shared_ptr<AVPixelBuffer> WmfVideoEncoder::dequeueInputBuffer() {
  if (!mConfigured || mInputEndOfStream || mInputDequeued || mInputBuffer == nullptr) {
    return nullptr;
  }
  mInputDequeued = true;
  mInputBuffer->clear();
  return mInputBuffer;
}

void WmfVideoEncoder::queueInputBuffer(std::shared_ptr<AVPixelBuffer> inputBuffer) {
  if (!inputBuffer || !mTransform.isValid()) {
    return;
  }
  mInputDequeued = false;

  if (inputBuffer->isEndOfStream()) {
    mInputEndOfStream = true;
    return;
  }

  Microsoft::WRL::ComPtr<IMFSample> sample;
  const auto timeUs = inputBuffer->timeUs() != Definitions::kTimeUnset
                        ? inputBuffer->timeUs()
                        : mFrameIndex * Definitions::kMicrosPerSecond / mFrameRate;
  if (!createFrameSample(inputBuffer, sample, timeUs)) {
    return;
  }
  ++mFrameIndex;

  const auto result = mTransform.processInput(sample.Get());
  if (result == MF_E_NOTACCEPTING) {
    mPendingInputSamples.emplace_back(sample);
    return;
  }
  if (FAILED(result)) {
    WmfMediaUtils::handleHresult(nullptr, result, "the video encoder rejected the input frame");
  }
}

bool WmfVideoEncoder::createFrameSample(const std::shared_ptr<AVPixelBuffer>& frame,
  Microsoft::WRL::ComPtr<IMFSample>& sample,
  const int64_t timeUs) {
  const auto width = mWidth;
  const auto height = mHeight;
  const auto chromaWidth = (width + 1) / 2;
  const auto chromaHeight = (height + 1) / 2;

  std::vector<uint8_t> data;
  if (mInputSubType == MFVideoFormat_NV12) {
    const auto nv12 = frame->asNV12PixelBuffer();
    if (nv12 == nullptr) {
      MINILOG_ERROR("the nv12 frame buffer is expected by the video encoder");
      return false;
    }
    data.resize(static_cast<size_t>(nv12->strideY()) * height +
                static_cast<size_t>(nv12->strideUV()) * chromaHeight);
    copyPlane(data.data(), nv12->strideY(), nv12->dataY(), nv12->strideY(), height,
      std::min(width, nv12->strideY()));
    copyPlane(data.data() + static_cast<ptrdiff_t>(nv12->strideY()) * height, nv12->strideUV(),
      nv12->dataUV(), nv12->strideUV(), chromaHeight,
      std::min(chromaWidth * 2, nv12->strideUV()));
  } else {
    const auto i420 = frame->asI420PixelBuffer();
    if (i420 == nullptr) {
      MINILOG_ERROR("the i420 frame buffer is expected by the video encoder");
      return false;
    }
    const auto strideY = i420->strideY();
    const auto strideU = i420->strideU();
    const auto strideV = i420->strideV();
    data.resize(static_cast<size_t>(strideY) * height +
                static_cast<size_t>(strideU + strideV) * chromaHeight);
    copyPlane(data.data(), strideY, i420->dataY(), strideY, height, std::min(width, strideY));
    auto* u = data.data() + static_cast<ptrdiff_t>(strideY) * height;
    auto* v = u + static_cast<ptrdiff_t>(strideU) * chromaHeight;
    if (mSwapChromaPlanes) {
      copyPlane(v, strideV, i420->dataU(), strideU, chromaHeight, std::min(chromaWidth, strideU));
      copyPlane(u, strideU, i420->dataV(), strideV, chromaHeight, std::min(chromaWidth, strideV));
    } else {
      copyPlane(u, strideU, i420->dataU(), strideU, chromaHeight, std::min(chromaWidth, strideU));
      copyPlane(v, strideV, i420->dataV(), strideV, chromaHeight, std::min(chromaWidth, strideV));
    }
  }

  sample = WmfMediaUtils::createSample(data.data(), data.size(), timeUs, AVBuffer::kNone);
  if (sample == nullptr) {
    return false;
  }
  sample->SetSampleDuration(
    static_cast<LONGLONG>(Definitions::kMicrosPerSecond / mFrameRate) * 10);
  return true;
}

bool WmfVideoEncoder::pushInputSamples() {
  while (!mPendingInputSamples.empty()) {
    const auto result = mTransform.processInput(mPendingInputSamples.front().Get());
    if (result == MF_E_NOTACCEPTING) {
      return false;
    }
    mPendingInputSamples.pop_front();
    if (FAILED(result)) {
      WmfMediaUtils::handleHresult(nullptr, result, "the video encoder rejected the input frame");
    }
  }
  return true;
}

std::shared_ptr<AVPacketBuffer> WmfVideoEncoder::createEndOfStreamBuffer() {
  mPacketBuffer->clear();
  mPacketBuffer->setFlags(AVBufferFlags::kEndOfStream);
  return mPacketBuffer;
}

std::shared_ptr<AVPacketBuffer> WmfVideoEncoder::dequeueOutputBuffer() {
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
        MINILOG_DEBUG("the video encoder changed its output type");
        refreshOutputFormat();
        if (!mFormatReported) {
          mFormatReported = true;
          mPacketBuffer->clear();
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
          WmfMediaUtils::handleHresult(nullptr, result, "failed to encode the video frame");
        }
        return nullptr;
      }
    }

    if (!mFormatReported) {
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

bool WmfVideoEncoder::fillOutputBuffer(const std::shared_ptr<AVPacketBuffer>& packetBuffer,
  const Microsoft::WRL::ComPtr<IMFSample>& sample) {
  Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
  auto result = sample->ConvertToContiguousBuffer(&buffer);
  if (FAILED(result) || buffer == nullptr) {
    WmfMediaUtils::handleHresult(nullptr, result, "the encoded frame has no contiguous buffer");
    return false;
  }

  BYTE* data = nullptr;
  DWORD maxLength = 0;
  DWORD currentLength = 0;
  result = buffer->Lock(&data, &maxLength, &currentLength);
  if (FAILED(result)) {
    WmfMediaUtils::handleHresult(nullptr, result, "failed to lock the encoded video buffer");
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

  UINT32 cleanPoint = 0;
  if (!FAILED(sample->GetUINT32(MFSampleExtension_CleanPoint, &cleanPoint)) && cleanPoint) {
    packetBuffer->addFlag(AVBufferFlags::kKeyFrame);
  }
  buffer->Unlock();
  return true;
}

int64_t WmfVideoEncoder::sampleTimeUs(const Microsoft::WRL::ComPtr<IMFSample>& sample) {
  LONGLONG time = 0;
  if (FAILED(sample->GetSampleTime(&time))) {
    return Definitions::kTimeUnset;
  }
  return time / 10; // 100ns to us
}

void WmfVideoEncoder::releaseOutputBuffer(const std::shared_ptr<AVPacketBuffer>& outputBuffer,
  const bool render) {
  // the packet buffer is owned by the encoder and is reused.
}

void WmfVideoEncoder::signalEndOfInputStream() {
  mInputEndOfStream = true;
}

} // namespace coral
