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

#include "WmfAudioFileReader.h"

#include "WmfMediaUtils.h"

#include <media/Definitions.h>
#include <minilog/log.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace coral {

std::unique_ptr<WmfAudioFileReader> WmfAudioFileReader::sniff(const std::string& inputUrl) {
  auto reader = std::make_unique<WmfAudioFileReader>(inputUrl);
  if (!reader->open(nullptr)) {
    return nullptr;
  }
  if (reader->getSampleRate() <= 0 || reader->getChannelCount() <= 0) {
    reader->close();
    return nullptr;
  }
  return reader;
}

WmfAudioFileReader::WmfAudioFileReader(std::string inputUrl)
  : mInputUrl(std::move(inputUrl)) {
  mWmfStarted = WmfMediaUtils::startup();
}

WmfAudioFileReader::~WmfAudioFileReader() {
  close();
  if (mWmfStarted) {
    WmfMediaUtils::shutdown();
  }
}

AudioSampleFormat WmfAudioFileReader::getSampleFormat() const {
  if (mIsFloat) {
    return AudioSampleFormat::kFloat;
  }
  if (mBitsPerSample == 16) {
    return AudioSampleFormat::kInt16;
  }
  if (mBitsPerSample == 24) {
    return AudioSampleFormat::kInt24;
  }
  if (mBitsPerSample == 32) {
    return AudioSampleFormat::kInt32;
  }
  return AudioSampleFormat::kInvalid;
}

bool WmfAudioFileReader::open(Error** outError) {
  if (mOpened) {
    return true;
  }
  if (!mWmfStarted) {
    handleError(outError, ErrCode::kNotInit, "The media foundation platform is not available");
    return false;
  }

  const std::wstring wUrl = WmfMediaUtils::utf8ToWide(mInputUrl);
  if (wUrl.empty()) {
    handleError(outError, ErrCode::kInvalidArgument, "The url is empty", mInputUrl.c_str());
    return false;
  }

  Microsoft::WRL::ComPtr<IMFAttributes> attrs;
  auto result = ::MFCreateAttributes(&attrs, 2);
  if (FAILED(result)) {
    WmfMediaUtils::handleHresult(outError, result, "MFCreateAttributes failed");
    return false;
  }
  attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);

  result = ::MFCreateSourceReaderFromURL(wUrl.c_str(), attrs.Get(), &mReader);
  if (FAILED(result) || !mReader) {
    WmfMediaUtils::handleHresult(outError, result, "MFCreateSourceReaderFromURL failed",
      mInputUrl);
    return false;
  }

  // selects the first audio stream only.
  mReader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
  mReader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);

  if (!negotiatePcmOutput(outError)) {
    close();
    return false;
  }
  if (!refreshFormatInfo(outError)) {
    close();
    return false;
  }

  // The total time is converted to the total number of sample frames.
  PROPVARIANT var;
  ::PropVariantInit(&var);
  if (SUCCEEDED(
    mReader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &var)) &&
      var.vt == VT_UI8 && mSampleRate > 0) {
    // MF_PD_DURATION is expressed in 100ns units.
    const auto duration100ns = static_cast<int64_t>(var.uhVal.QuadPart);
    mSampleLength = duration100ns * mSampleRate / 10000000LL;
  } else {
    mSampleLength = Definitions::kInvalidValue;
  }
  ::PropVariantClear(&var);

  mFramePosition = 0;
  mPendingData.clear();
  mPendingOffset = 0;
  mOpened = true;
  return true;
}

void WmfAudioFileReader::seekTo(const int64_t framePosition) {
  if (!mReader || mSampleRate <= 0) {
    return;
  }
  const auto target = std::max<int64_t>(framePosition, 0);

  PROPVARIANT var;
  ::PropVariantInit(&var);
  var.vt = VT_I8;
  var.hVal.QuadPart = target * 10000000LL / mSampleRate;

  const auto result = mReader->SetCurrentPosition(GUID_NULL, var);
  ::PropVariantClear(&var);
  if (FAILED(result)) {
    MINILOG_ERROR("failed to seek to the frame {}: {}", target,
      WmfMediaUtils::hresultToString(result));
    return;
  }

  // the buffered data belongs to the previous position.
  mPendingData.clear();
  mPendingOffset = 0;
  mFramePosition = target;
}

void WmfAudioFileReader::close() {
  mCurrentOutputType.Reset();
  mReader.Reset();
  mPendingData.clear();
  mPendingOffset = 0;
  mOpened = false;
}

int64_t WmfAudioFileReader::readIntoBuffer(uint8_t* outBuffer, const int64_t bufferCapacity,
  Error** outError) {
  if (!mOpened || !mReader) {
    handleError(outError, ErrCode::kNotInit, "The reader is not open");
    return Definitions::kInvalidValue;
  }
  if (!outBuffer || bufferCapacity <= 0) {
    handleError(outError, ErrCode::kInvalidArgument, "The buffer is invalid");
    return Definitions::kInvalidValue;
  }
  if (mBytesPerFrame <= 0) {
    handleError(outError, ErrCode::kUnknown, "The output format is invalid");
    return Definitions::kInvalidValue;
  }

  const auto capacity = static_cast<size_t>(bufferCapacity);
  size_t bytesWritten = 0;

  // Delivers the data which was left over by the previous read first.
  if (mPendingOffset < mPendingData.size()) {
    const auto available = mPendingData.size() - mPendingOffset;
    auto toCopy = std::min(available, capacity) / mBytesPerFrame * mBytesPerFrame;
    if (toCopy > 0) {
      memcpy(outBuffer, mPendingData.data() + mPendingOffset, toCopy);
      mPendingOffset += toCopy;
      bytesWritten += toCopy;
    }
    if (mPendingOffset >= mPendingData.size()) {
      mPendingData.clear();
      mPendingOffset = 0;
    }
  }

  while (bytesWritten < capacity) {
    DWORD streamFlags = 0;
    LONGLONG timestamp = 0;
    Microsoft::WRL::ComPtr<IMFSample> sample;
    const auto result = mReader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr,
      &streamFlags, &timestamp, &sample);

    if (FAILED(result)) {
      WmfMediaUtils::handleHresult(outError, result, "IMFSourceReader::ReadSample failed");
      break;
    }
    if ((streamFlags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
      break;
    }
    if ((streamFlags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0) {
      // the buffered data belongs to the previous format.
      mPendingData.clear();
      mPendingOffset = 0;
      if (!refreshFormatInfo(outError)) {
        break;
      }
      if (mBytesPerFrame <= 0) {
        break;
      }
    }
    if (sample == nullptr) {
      continue;
    }

    Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
    const auto convertResult = sample->ConvertToContiguousBuffer(&buffer);
    if (FAILED(convertResult) || buffer == nullptr) {
      WmfMediaUtils::handleHresult(outError, convertResult, "ConvertToContiguousBuffer failed");
      break;
    }

    BYTE* data = nullptr;
    DWORD maxLen = 0;
    DWORD curLen = 0;
    const auto lockResult = buffer->Lock(&data, &maxLen, &curLen);
    if (FAILED(lockResult)) {
      WmfMediaUtils::handleHresult(outError, lockResult, "IMFMediaBuffer::Lock failed");
      break;
    }

    auto remaining = capacity - bytesWritten;
    auto toCopy = std::min<size_t>(curLen, remaining);
    toCopy = toCopy / mBytesPerFrame * mBytesPerFrame;

    if (toCopy > 0) {
      memcpy(outBuffer + bytesWritten, data, toCopy);
      bytesWritten += toCopy;
    }
    if (toCopy < curLen) {
      // The sample does not fit into the buffer, the rest of it is delivered by the next read.
      mPendingData.assign(data + toCopy, data + curLen);
      mPendingOffset = 0;
      buffer->Unlock();
      break;
    }
    buffer->Unlock();
  }

  const auto frames = static_cast<int64_t>(bytesWritten / mBytesPerFrame);
  mFramePosition += frames;
  return frames;
}

bool WmfAudioFileReader::negotiatePcmOutput(Error** outError) const {
  Microsoft::WRL::ComPtr<IMFMediaType> outType;
  auto result = ::MFCreateMediaType(&outType);
  if (FAILED(result)) {
    WmfMediaUtils::handleHresult(outError, result, "MFCreateMediaType failed");
    return false;
  }

  outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
  outType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
  result = mReader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr,
    outType.Get());
  if (FAILED(result)) {
    WmfMediaUtils::handleHresult(outError, result, "SetCurrentMediaType(PCM) failed");
    return false;
  }
  return true;
}

bool WmfAudioFileReader::refreshFormatInfo(Error** outError) {
  const auto result = mReader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM,
    &mCurrentOutputType);
  if (FAILED(result) || !mCurrentOutputType) {
    WmfMediaUtils::handleHresult(outError, result, "GetCurrentMediaType failed");
    return false;
  }

  UINT32 channels = 0, sampleRate = 0, bits = 0;
  mCurrentOutputType->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
  mCurrentOutputType->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sampleRate);
  mCurrentOutputType->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);

  GUID subType{};
  mCurrentOutputType->GetGUID(MF_MT_SUBTYPE, &subType);

  if (channels == 0 || sampleRate == 0 || bits == 0) {
    handleError(outError, ErrCode::kUnknown, "The output format is invalid");
    return false;
  }

  mChannelCount = static_cast<int32_t>(channels);
  mSampleRate = static_cast<int64_t>(sampleRate);
  mBitsPerSample = static_cast<int32_t>(bits);
  mIsFloat = subType == MFAudioFormat_Float;
  mBytesPerFrame = mChannelCount * (mBitsPerSample / 8);
  return mBytesPerFrame > 0;
}

} // namespace coral
