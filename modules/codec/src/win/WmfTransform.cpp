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

#include "WmfTransform.h"

#include "WmfMediaUtils.h"

#include <minilog/log.h>

#include <algorithm>

namespace coral {

namespace {

constexpr DWORD MIN_OUTPUT_BUFFER_SIZE = 1 << 16;

} // namespace anonymous

void WmfTransformContext::attach(Microsoft::WRL::ComPtr<IMFTransform> transform) {
  mTransform = std::move(transform);
  mDraining = false;
}

void WmfTransformContext::reset() {
  release();
  mTransform.Reset();
}

void WmfTransformContext::release() {
  if (mTransform) {
    mTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    mTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    mTransform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
  }
  mDraining = false;
}

HRESULT WmfTransformContext::setInputType(IMFMediaType* mediaType) {
  if (!mTransform) {
    return E_FAIL;
  }
  return mTransform->SetInputType(0, mediaType, 0);
}

HRESULT WmfTransformContext::setOutputType(IMFMediaType* mediaType) {
  if (!mTransform) {
    return E_FAIL;
  }
  return mTransform->SetOutputType(0, mediaType, 0);
}

HRESULT WmfTransformContext::getOutputType(IMFMediaType** mediaType) const {
  if (!mTransform) {
    return E_FAIL;
  }
  return mTransform->GetOutputCurrentType(0, mediaType);
}

Microsoft::WRL::ComPtr<IMFMediaType>
WmfTransformContext::findOutputType(const GUID& majorType, const GUID& subType) const {
  Microsoft::WRL::ComPtr<IMFMediaType> matched;
  if (!mTransform) {
    return matched;
  }
  for (DWORD index = 0;; ++index) {
    Microsoft::WRL::ComPtr<IMFMediaType> candidate;
    const auto result = mTransform->GetOutputAvailableType(0, index, &candidate);
    if (FAILED(result) || candidate == nullptr) {
      break;
    }
    GUID candidateMajor{};
    GUID candidateSub{};
    if (FAILED(candidate->GetGUID(MF_MT_MAJOR_TYPE, &candidateMajor)) ||
        FAILED(candidate->GetGUID(MF_MT_SUBTYPE, &candidateSub))) {
      continue;
    }
    if (candidateMajor == majorType && candidateSub == subType) {
      matched = candidate;
      break;
    }
  }
  return matched;
}

WmfTransformContext::OutputState WmfTransformContext::queryOutputState() const {
  if (!mTransform) {
    return OutputState::kNeedMoreInput;
  }
  DWORD statusFlags = 0;
  const auto result = mTransform->GetOutputStatus(&statusFlags);
  if (result == S_OK) {
    return OutputState::kSampleReady;
  }
  if (result == MF_E_TRANSFORM_NEED_MORE_INPUT) {
    return OutputState::kNeedMoreInput;
  }
  // The transform does not implement the query, the output is pulled directly.
  return OutputState::kUnknown;
}

HRESULT WmfTransformContext::processInput(IMFSample* sample) {
  if (!mTransform) {
    return E_FAIL;
  }
  return mTransform->ProcessInput(0, sample, 0);
}

HRESULT WmfTransformContext::processOutput(Microsoft::WRL::ComPtr<IMFSample>& sample) {
  sample.Reset();
  if (!mTransform) {
    return E_FAIL;
  }

  MFT_OUTPUT_STREAM_INFO streamInfo{};
  auto result = mTransform->GetOutputStreamInfo(0, &streamInfo);
  if (FAILED(result)) {
    return result;
  }

  // When the transform provides its own samples the output sample is created by the transform,
  // otherwise the caller must allocate a sample which is large enough.
  const bool allocateSample = (streamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0;
  Microsoft::WRL::ComPtr<IMFSample> allocated;
  if (allocateSample) {
    const auto bufferSize = std::max<DWORD>(streamInfo.cbSize, MIN_OUTPUT_BUFFER_SIZE);
    Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
    result = ::MFCreateMemoryBuffer(bufferSize, &buffer);
    if (FAILED(result) || buffer == nullptr) {
      return result;
    }
    result = ::MFCreateSample(&allocated);
    if (FAILED(result) || allocated == nullptr) {
      return result;
    }
    allocated->AddBuffer(buffer.Get());
  }

  MFT_OUTPUT_DATA_BUFFER outputBuffer{};
  outputBuffer.dwStreamID = 0;
  outputBuffer.pSample = allocated.Get();

  MFT_OUTPUT_DATA_BUFFER outputBuffers[1] = { outputBuffer };
  DWORD status = 0;
  result = mTransform->ProcessOutput(0, 1, outputBuffers, &status);

  if (outputBuffers[0].pEvents != nullptr) {
    outputBuffers[0].pEvents->Release();
    outputBuffers[0].pEvents = nullptr;
  }

  if (FAILED(result)) {
    return result;
  }

  auto* produced = outputBuffers[0].pSample;
  if (produced == nullptr) {
    return MF_E_TRANSFORM_NEED_MORE_INPUT;
  }
  if (produced == allocated.Get()) {
    sample = allocated;
  } else {
    // The transform provided the sample, take the ownership of the reference.
    sample.Attach(produced);
  }
  return S_OK;
}

void WmfTransformContext::notifyStartOfStream() {
  if (!mTransform) {
    return;
  }
  mTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
  mTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
  mDraining = false;
}

void WmfTransformContext::flush() {
  if (!mTransform) {
    return;
  }
  mTransform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
  mDraining = false;
}

void WmfTransformContext::drain() {
  if (!mTransform) {
    return;
  }
  mTransform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
  mDraining = true;
}

} // namespace coral
