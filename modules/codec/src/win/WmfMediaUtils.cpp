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

#include "WmfMediaUtils.h"

#include <media/Buffer.h>
#include <media/Utilities.h>
#include <minilog/log.h>

#include <atomic>
#include <mutex>
#include <vector>

namespace coral {

namespace {

std::atomic<int> gWmfRefCount{ 0 };
std::atomic<bool> gComInitialized{ false };
std::mutex gWmfMutex;

// ------------------------------------------------------------------------------------------------
// The well known media sub types which are used by the Windows implementation. They are declared
// locally to keep the implementation independent of the Windows SDK version.
// ------------------------------------------------------------------------------------------------

constexpr GUID kAudioSubtypePcm = {
  0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 }
};
constexpr GUID kAudioSubtypeFloat = {
  0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 }
};
constexpr GUID kAudioSubtypeMp3 = {
  0x00000050, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 }
};
constexpr GUID kAudioSubtypeMp3Layer3 = {
  0x00000055, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 }
};
constexpr GUID kAudioSubtypeAac = {
  0x00001610, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 }
};
constexpr GUID kAudioSubtypeAc3 = {
  0x00002000, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 }
};
constexpr GUID kAudioSubtypeFlac = {
  0x0000f1ac, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 }
};

constexpr GUID kVideoSubtypeH264 = {
  0x34363248, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 }
};
constexpr GUID kVideoSubtypeHevc = {
  0x43564548, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 }
};
constexpr GUID kVideoSubtypeMpeg4 = {
  0x5634504d, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 }
};

constexpr GUID kVideoPixelNV12 = {
  0x3231564e, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 }
};
constexpr GUID kVideoPixelIYUV = {
  0x56555949, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 }
};
constexpr GUID kVideoPixelYV12 = {
  0x32315659, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 }
};
constexpr GUID kVideoPixelYUY2 = {
  0x32595559, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 }
};
constexpr GUID kVideoPixelRGB32 = {
  0x00000016, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 }
};
constexpr GUID kVideoPixelARGB32 = {
  0x00000015, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 }
};

/**
 * Returns the mime type text of the media sub type, used only to build readable logs.
 */
std::string subTypeName(const GUID& subType) {
  if (subType == kAudioSubtypePcm) {
    return "pcm";
  }
  if (subType == kAudioSubtypeFloat) {
    return "pcm(float)";
  }
  if (subType == kAudioSubtypeAac) {
    return "aac";
  }
  if (subType == kAudioSubtypeMp3) {
    return "mp3";
  }
  if (subType == kVideoSubtypeH264) {
    return "h264";
  }
  if (subType == kVideoSubtypeHevc) {
    return "hevc";
  }
  return "unknown";
}

} // namespace anonymous

// --------------------------------------------------------------------------------------------------

bool WmfMediaUtils::startup(Error** outError) {
  std::lock_guard<std::mutex> lock(gWmfMutex);
  if (gWmfRefCount.fetch_add(1) == 0) {
    // Several of the media foundation objects are COM objects, the apartment is initialized before
    // the platform itself. When the caller has already initialized a different apartment the
    // initialization is skipped.
    const auto comResult = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(comResult)) {
      gComInitialized.store(true);
    } else if (comResult != RPC_E_CHANGED_MODE) {
      gWmfRefCount.fetch_sub(1);
      handleHresult(outError, comResult, "CoInitializeEx failed");
      return false;
    }

    const auto result = ::MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(result)) {
      if (gComInitialized.exchange(false)) {
        ::CoUninitialize();
      }
      gWmfRefCount.fetch_sub(1);
      handleHresult(outError, result, "MFStartup failed");
      return false;
    }
  }
  return true;
}

void WmfMediaUtils::shutdown() {
  std::lock_guard<std::mutex> lock(gWmfMutex);
  if (gWmfRefCount.fetch_sub(1) == 1) {
    ::MFShutdown();
    if (gComInitialized.exchange(false)) {
      ::CoUninitialize();
    }
  }
}

std::wstring WmfMediaUtils::utf8ToWide(const std::string& value) {
  if (value.empty()) {
    return {};
  }
  const auto length = ::MultiByteToWideChar(CP_UTF8, 0, value.c_str(),
    static_cast<int>(value.size()), nullptr, 0);
  if (length <= 0) {
    return {};
  }
  std::wstring result(static_cast<size_t>(length), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
    result.data(), length);
  return result;
}

std::wstring WmfMediaUtils::toSinkWriterUrl(const std::string& url) {
  const auto wide = utf8ToWide(url);
  // A value with a scheme is a URL the sink writer has to parse itself; anything else is a file path,
  // and Media Foundation only accepts an absolute one.
  if (wide.empty() || url.find("://") != std::string::npos) {
    return wide;
  }
  const auto length = ::GetFullPathNameW(wide.c_str(), 0, nullptr, nullptr);
  if (length == 0) {
    return wide;
  }
  std::wstring absolute(static_cast<size_t>(length), L'\0');
  const auto written = ::GetFullPathNameW(wide.c_str(), length, absolute.data(), nullptr);
  if (written == 0 || written >= length) {
    return wide;
  }
  absolute.resize(static_cast<size_t>(written));
  return absolute;
}

std::string WmfMediaUtils::wideToUtf8(const std::wstring& value) {
  if (value.empty()) {
    return {};
  }
  const auto length = ::WideCharToMultiByte(CP_UTF8, 0, value.c_str(),
    static_cast<int>(value.size()), nullptr, 0, nullptr,
    nullptr);
  if (length <= 0) {
    return {};
  }
  std::string result(static_cast<size_t>(length), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
    result.data(), length, nullptr, nullptr);
  return result;
}

std::string WmfMediaUtils::hresultToString(const HRESULT hr) {
  switch (hr) {
    case S_OK: {
      return "S_OK";
    }
    case E_INVALIDARG: {
      return "E_INVALIDARG";
    }
    case E_OUTOFMEMORY: {
      return "E_OUTOFMEMORY";
    }
    case E_NOTIMPL: {
      return "E_NOTIMPL";
    }
    case MF_E_INVALIDMEDIATYPE: {
      return "MF_E_INVALIDMEDIATYPE";
    }
    case MF_E_INVALIDTYPE: {
      return "MF_E_INVALIDTYPE";
    }
    case MF_E_TRANSFORM_TYPE_NOT_SET: {
      return "MF_E_TRANSFORM_TYPE_NOT_SET";
    }
    case MF_E_TRANSFORM_NEED_MORE_INPUT: {
      return "MF_E_TRANSFORM_NEED_MORE_INPUT";
    }
    case MF_E_TRANSFORM_STREAM_CHANGE: {
      return "MF_E_TRANSFORM_STREAM_CHANGE";
    }
    case MF_E_NO_MORE_TYPES: {
      return "MF_E_NO_MORE_TYPES";
    }
    case MF_E_INVALIDSTREAMNUMBER: {
      return "MF_E_INVALIDSTREAMNUMBER";
    }
    case MF_E_ATTRIBUTENOTFOUND: {
      return "MF_E_ATTRIBUTENOTFOUND";
    }
    case MF_E_UNSUPPORTED_D3D_TYPE: {
      return "MF_E_UNSUPPORTED_D3D_TYPE";
    }
    default: {
      break;
    }
  }
  char buffer[32] = { 0 };
  sprintf_s(buffer, "0x%08lX", static_cast<unsigned long>(hr));
  return buffer;
}

void WmfMediaUtils::handleHresult(Error** outError,
  const HRESULT hr,
  const char* errorMsg,
  const std::string& description) {
  const auto reason = hresultToString(hr);
  MINILOG_ERROR("{}: hr={}, description={}", errorMsg, reason, description);
  if (outError == nullptr) {
    return;
  }
  if (*outError == nullptr) {
    *outError = new Error();
  }
  (*outError)->code = ErrCode::kUnknown;
  (*outError)->failureReason = errorMsg;
  (*outError)->description = reason;
  if (!description.empty()) {
    (*outError)->description.append(": ").append(description);
  }
}

// --------------------------------------------------------------------------------------------------

bool WmfMediaUtils::audioFormatTypeToSubtype(const AudioFormatType type, GUID& subType) {
  switch (type) {
    case AudioFormatType::kAudioFormatTypeLinearPCM: {
      subType = kAudioSubtypePcm;
      return true;
    }
    case AudioFormatType::kAudioFormatTypeMpeg3: {
      subType = kAudioSubtypeMp3;
      return true;
    }
    case AudioFormatType::kAudioFormatTypeMpeg4AAC: {
      subType = kAudioSubtypeAac;
      return true;
    }
    case AudioFormatType::kAudioFormatTypeAC3: {
      subType = kAudioSubtypeAc3;
      return true;
    }
    case AudioFormatType::kAudioFormatTypeFlac: {
      subType = kAudioSubtypeFlac;
      return true;
    }
    default: {
      return false;
    }
  }
}

bool WmfMediaUtils::videoCodecTypeToSubtype(const VideoCodecType type, GUID& subType) {
  switch (type) {
    case VideoCodecType::kVideoCodecTypeH264: {
      subType = kVideoSubtypeH264;
      return true;
    }
    case VideoCodecType::kVideoCodecTypeHevc: {
      subType = kVideoSubtypeHevc;
      return true;
    }
    case VideoCodecType::kVideoCodecTypeMpeg4Video: {
      subType = kVideoSubtypeMpeg4;
      return true;
    }
    default: {
      return false;
    }
  }
}

AudioFormatType WmfMediaUtils::subtypeToAudioFormatType(const GUID& subType) {
  if (subType == kAudioSubtypePcm || subType == kAudioSubtypeFloat) {
    return AudioFormatType::kAudioFormatTypeLinearPCM;
  }
  if (subType == kAudioSubtypeAac) {
    return AudioFormatType::kAudioFormatTypeMpeg4AAC;
  }
  if (subType == kAudioSubtypeMp3) {
    return AudioFormatType::kAudioFormatTypeMpeg3;
  }
  if (subType == kAudioSubtypeAc3) {
    return AudioFormatType::kAudioFormatTypeAC3;
  }
  if (subType == kAudioSubtypeFlac) {
    return AudioFormatType::kAudioFormatTypeFlac;
  }
  return AudioFormatType::kAudioFormatTypeInvalid;
}

VideoCodecType WmfMediaUtils::subtypeToVideoCodecType(const GUID& subType) {
  if (subType == kVideoSubtypeH264) {
    return VideoCodecType::kVideoCodecTypeH264;
  }
  if (subType == kVideoSubtypeHevc) {
    return VideoCodecType::kVideoCodecTypeHevc;
  }
  if (subType == kVideoSubtypeMpeg4) {
    return VideoCodecType::kVideoCodecTypeMpeg4Video;
  }
  return VideoCodecType::kVideoCodecTypeInvalid;
}

std::string WmfMediaUtils::subtypeToMimeType(const GUID& subType) {
  if (subType == kAudioSubtypePcm || subType == kAudioSubtypeFloat) {
    return MimeTypes::AUDIO_RAW;
  }
  if (subType == kAudioSubtypeAac) {
    return MimeTypes::AUDIO_AAC;
  }
  if (subType == kAudioSubtypeMp3 || subType == kAudioSubtypeMp3Layer3) {
    return MimeTypes::AUDIO_MPEG;
  }
  if (subType == kAudioSubtypeAc3) {
    return MimeTypes::AUDIO_AC3;
  }
  if (subType == kAudioSubtypeFlac) {
    return MimeTypes::AUDIO_FLAC;
  }
  if (subType == kVideoSubtypeH264) {
    return MimeTypes::VIDEO_H264;
  }
  if (subType == kVideoSubtypeHevc) {
    return MimeTypes::VIDEO_H265;
  }
  if (subType == kVideoSubtypeMpeg4) {
    return MimeTypes::VIDEO_MP4;
  }
  return {};
}

bool WmfMediaUtils::mimeTypeToSubtype(const std::string& mimeType, GUID& subType) {
  const auto audioFormatType = MediaUtils::convertToAudioFormatType(mimeType);
  if (audioFormatType != AudioFormatType::kAudioFormatTypeInvalid) {
    return audioFormatTypeToSubtype(audioFormatType, subType);
  }
  const auto videoCodecType = MediaUtils::convertToVideoCodecType(mimeType);
  if (videoCodecType != VideoCodecType::kVideoCodecTypeInvalid) {
    return videoCodecTypeToSubtype(videoCodecType, subType);
  }
  return false;
}

AudioSampleFormat WmfMediaUtils::subtypeToSampleFormat(const GUID& subType) {
  if (subType == kAudioSubtypeFloat) {
    return AudioSampleFormat::kFloat;
  }
  if (subType == kAudioSubtypePcm) {
    return AudioSampleFormat::kInt16;
  }
  return AudioSampleFormat::kInvalid;
}

bool WmfMediaUtils::sampleFormatToSubtype(const AudioSampleFormat format, GUID& subType) {
  switch (format) {
    case AudioSampleFormat::kInt16:
    case AudioSampleFormat::kInt24:
    case AudioSampleFormat::kInt32: {
      subType = kAudioSubtypePcm;
      return true;
    }
    case AudioSampleFormat::kFloat: {
      subType = kAudioSubtypeFloat;
      return true;
    }
    default: {
      return false;
    }
  }
}

int WmfMediaUtils::bitsPerSample(const AudioSampleFormat format) {
  return static_cast<int>(MediaUtils::getBytesPerSample(format) * 8);
}

// --------------------------------------------------------------------------------------------------

const GUID& WmfMediaUtils::transformCategory(const bool video, const bool decoder) {
  if (video) {
    return decoder ? MFT_CATEGORY_VIDEO_DECODER : MFT_CATEGORY_VIDEO_ENCODER;
  }
  return decoder ? MFT_CATEGORY_AUDIO_DECODER : MFT_CATEGORY_AUDIO_ENCODER;
}

bool WmfMediaUtils::isTransformAvailable(const bool video, const GUID& subType, const bool decoder) {
  return createTransform(video, subType, decoder) != nullptr;
}

Microsoft::WRL::ComPtr<IMFTransform>
WmfMediaUtils::createTransform(const bool video, const GUID& subType, const bool decoder) {
  const auto category = transformCategory(video, decoder);
  const MFT_REGISTER_TYPE_INFO typeInfo{
    video ? MFMediaType_Video : MFMediaType_Audio,
    subType
  };

  constexpr UINT32 flags = MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER;

  IMFActivate** activates = nullptr;
  UINT32 activateCount = 0;
  const auto result = ::MFTEnumEx(category, flags,
    decoder ? &typeInfo : nullptr,
    decoder ? nullptr : &typeInfo,
    &activates, &activateCount);
  if (FAILED(result) || activates == nullptr || activateCount == 0) {
    if (activates != nullptr) {
      ::CoTaskMemFree(activates);
    }
    MINILOG_DEBUG("no transform available for the codec: {}", subTypeName(subType));
    return nullptr;
  }

  Microsoft::WRL::ComPtr<IMFTransform> transform;
  for (UINT32 i = 0; i < activateCount && transform == nullptr; ++i) {
    UINT32 isAsync = 0;
    activates[i]->GetUINT32(MF_TRANSFORM_ASYNC, &isAsync);
    if (isAsync) {
      // the synchronous mode is used by the implementation, skip the asynchronous transforms.
      continue;
    }
    Microsoft::WRL::ComPtr<IMFTransform> candidate;
    if (SUCCEEDED(activates[i]->ActivateObject(IID_PPV_ARGS(&candidate)))) {
      transform = candidate;
      UINT32 nameLength = 0;
      WCHAR* name = nullptr;
      if (SUCCEEDED(activates[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &nameLength))) {
        MINILOG_DEBUG("the transform {} is used for the codec {}", wideToUtf8(name),
          subTypeName(subType));
        ::CoTaskMemFree(name);
      }
    }
  }

  for (UINT32 i = 0; i < activateCount; ++i) {
    activates[i]->Release();
  }
  ::CoTaskMemFree(activates);
  return transform;
}

// --------------------------------------------------------------------------------------------------

Microsoft::WRL::ComPtr<IMFMediaType>
WmfMediaUtils::createPcmMediaType(const GUID& subType, const int sampleRate,
  const int channelCount) {
  Microsoft::WRL::ComPtr<IMFMediaType> mediaType;
  if (FAILED(::MFCreateMediaType(&mediaType))) {
    return nullptr;
  }

  const auto sampleFormat = subtypeToSampleFormat(subType);
  const auto bits = bitsPerSample(sampleFormat);
  if (bits <= 0) {
    return nullptr;
  }
  const auto blockAlign = channelCount * bits / 8;

  mediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
  mediaType->SetGUID(MF_MT_SUBTYPE, subType);
  mediaType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, static_cast<UINT32>(channelCount));
  mediaType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, static_cast<UINT32>(sampleRate));
  mediaType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, static_cast<UINT32>(bits));
  mediaType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, static_cast<UINT32>(blockAlign));
  mediaType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
    static_cast<UINT32>(sampleRate * blockAlign));
  mediaType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
  return mediaType;
}

Microsoft::WRL::ComPtr<IMFSample>
WmfMediaUtils::createSample(const uint8_t* data, const size_t size, const int64_t timeUs,
  const uint32_t flags) {
  Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
  auto result = ::MFCreateMemoryBuffer(static_cast<DWORD>(size), &buffer);
  if (FAILED(result) || buffer == nullptr) {
    handleHresult(nullptr, result, "MFCreateMemoryBuffer failed");
    return nullptr;
  }

  if (size > 0 && data != nullptr) {
    BYTE* target = nullptr;
    DWORD maxLength = 0;
    result = buffer->Lock(&target, &maxLength, nullptr);
    if (FAILED(result)) {
      handleHresult(nullptr, result, "IMFMediaBuffer::Lock failed");
      return nullptr;
    }
    memcpy(target, data, size);
    buffer->Unlock();
  }

  buffer->SetCurrentLength(static_cast<DWORD>(size));

  Microsoft::WRL::ComPtr<IMFSample> sample;
  result = ::MFCreateSample(&sample);
  if (FAILED(result) || sample == nullptr) {
    handleHresult(nullptr, result, "MFCreateSample failed");
    return nullptr;
  }
  sample->AddBuffer(buffer.Get());
  if (timeUs != Definitions::kTimeUnset) {
    sample->SetSampleTime(timeUs * 10); // microseconds to 100ns
  }
  if (flags != 0) {
    sample->SetUINT32(MFSampleExtension_CleanPoint, (flags & AVBuffer::kKeyFrame) != 0);
    if ((flags & AVBuffer::kEndOfStream) != 0) {
      sample->SetUINT32(MFSampleExtension_Discontinuity, FALSE);
    }
  }
  return sample;
}

void WmfMediaUtils::fillAudioMediaType(IMFMediaType* mediaType,
  const std::shared_ptr<MediaFormat>& format,
  const GUID& subType) {
  mediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
  mediaType->SetGUID(MF_MT_SUBTYPE, subType);
  if (format->getSampleRate() != MediaFormat::NO_VALUE) {
    mediaType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,
      static_cast<UINT32>(format->getSampleRate()));
  }
  if (format->getChannelCount() != MediaFormat::NO_VALUE) {
    mediaType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS,
      static_cast<UINT32>(format->getChannelCount()));
  }
  if (format->getBitrate() != MediaFormat::NO_VALUE && format->getBitrate() > 0) {
    mediaType->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(format->getBitrate()));
  }

  const auto sampleFormat = format->getSampleFormat();
  if (sampleFormat == AudioSampleFormat::kInt16 ||
      sampleFormat == AudioSampleFormat::kInt24 ||
      sampleFormat == AudioSampleFormat::kInt32 ||
      sampleFormat == AudioSampleFormat::kFloat) {
    const auto bits = bitsPerSample(sampleFormat);
    const auto channelCount = format->getChannelCount() != MediaFormat::NO_VALUE
                                ? format->getChannelCount()
                                : 0;
    mediaType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, static_cast<UINT32>(bits));
    if (channelCount > 0) {
      const auto blockAlign = channelCount * bits / 8;
      mediaType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, static_cast<UINT32>(blockAlign));
      mediaType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
        static_cast<UINT32>(format->getSampleRate() * blockAlign));
    }
    mediaType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
  }
}

void WmfMediaUtils::fillVideoMediaType(IMFMediaType* mediaType,
  const std::shared_ptr<MediaFormat>& format,
  const GUID& subType) {
  mediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  mediaType->SetGUID(MF_MT_SUBTYPE, subType);
  mediaType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

  if (format->getWidth() != MediaFormat::NO_VALUE && format->getHeight() !=
      MediaFormat::NO_VALUE) {
    mediaType->SetUINT64(MF_MT_FRAME_SIZE,
      ::Pack2UINT32AsUINT64(static_cast<UINT32>(format->getWidth()),
        static_cast<UINT32>(format->getHeight())));
  }
  if (format->getFrameRate() != MediaFormat::NO_VALUE && format->getFrameRate() > 0) {
    mediaType->SetUINT64(MF_MT_FRAME_RATE,
      ::Pack2UINT32AsUINT64(static_cast<UINT32>(format->getFrameRate()), 1));
  }
  if (format->getBitrate() != MediaFormat::NO_VALUE && format->getBitrate() > 0) {
    mediaType->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(format->getBitrate()));
  }
}

void WmfMediaUtils::fillMediaFormat(IMFMediaType* mediaType,
  const std::shared_ptr<MediaFormat>& format,
  const bool compressed) {
  if (mediaType == nullptr || format == nullptr) {
    return;
  }
  GUID subType{};
  if (FAILED(mediaType->GetGUID(MF_MT_SUBTYPE, &subType))) {
    return;
  }
  const auto mimeType = subtypeToMimeType(subType);
  if (!mimeType.empty()) {
    format->setMimeType(mimeType);
  }

  const auto audioFormatType = subtypeToAudioFormatType(subType);
  if (audioFormatType != AudioFormatType::kAudioFormatTypeInvalid) {
    format->setAudioFormatType(audioFormatType);
  }
  const auto videoCodecType = subtypeToVideoCodecType(subType);
  if (videoCodecType != VideoCodecType::kVideoCodecTypeInvalid) {
    format->setVideoCodecType(videoCodecType);
  }

  if (!compressed) {
    return;
  }

  // codec specific data, it is available only for the compressed media types.
  if (subType == kAudioSubtypeAac) {
    // MF_MT_USER_DATA of the AAC media type contains the HEAACWAVEFORMAT structure
    // without its WAVEFORMATEX part (12 bytes) followed by the AudioSpecificConfig.
    UINT32 length = 0;
    if (SUCCEEDED(mediaType->GetBlobSize(MF_MT_USER_DATA, &length)) && length > 12) {
      std::vector<uint8_t> blob(length, 0);
      if (SUCCEEDED(mediaType->GetBlob(MF_MT_USER_DATA, blob.data(), length, &length)) &&
          length > 12) {
        format->setCodecSpecificData(0, blob.data() + 12, length - 12);
      }
    }
  } else if (subType == kVideoSubtypeH264 || subType == kVideoSubtypeHevc) {
    UINT32 length = 0;
    if (SUCCEEDED(mediaType->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &length)) && length > 0) {
      std::vector<uint8_t> blob(length, 0);
      if (SUCCEEDED(mediaType->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, blob.data(), length, &length)) &&
          length > 0) {
        format->setCodecSpecificData(0, blob.data(), length);
      }
    }
  }
}

std::shared_ptr<MediaFormat>
WmfMediaUtils::convertToMediaFormat(IMFMediaType* mediaType, const bool compressed) {
  if (mediaType == nullptr) {
    return nullptr;
  }

  GUID majorType{};
  GUID subType{};
  if (FAILED(mediaType->GetGUID(MF_MT_MAJOR_TYPE, &majorType)) ||
      FAILED(mediaType->GetGUID(MF_MT_SUBTYPE, &subType))) {
    return nullptr;
  }

  const auto mimeType = subtypeToMimeType(subType);
  if (mimeType.empty()) {
    MINILOG_DEBUG("unsupported media sub type: {}", subTypeName(subType));
    return nullptr;
  }

  std::shared_ptr<MediaFormat> format;
  if (majorType == MFMediaType_Audio) {
    UINT32 sampleRate = 0;
    UINT32 channelCount = 0;
    UINT32 bitsPerSample = 0;
    mediaType->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sampleRate);
    mediaType->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channelCount);
    mediaType->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bitsPerSample);

    format = MediaFormat::CreateAudioFormat(mimeType,
      static_cast<int>(sampleRate),
      static_cast<int>(channelCount));
    if (subType == kAudioSubtypeFloat) {
      format->setSampleFormat(AudioSampleFormat::kFloat);
    } else if (subType == kAudioSubtypePcm) {
      auto sampleFormat = AudioSampleFormat::kInt16;
      if (bitsPerSample == 32) {
        sampleFormat = AudioSampleFormat::kInt32;
      } else if (bitsPerSample == 24) {
        sampleFormat = AudioSampleFormat::kInt24;
      }
      format->setSampleFormat(sampleFormat);
    }
  } else if (majorType == MFMediaType_Video) {
    UINT64 frameSize = 0;
    UINT64 frameRate = 0;
    UINT32 bitrate = 0;
    mediaType->GetUINT64(MF_MT_FRAME_SIZE, &frameSize);
    mediaType->GetUINT64(MF_MT_FRAME_RATE, &frameRate);
    mediaType->GetUINT32(MF_MT_AVG_BITRATE, &bitrate);

    const auto width = static_cast<int>(frameSize >> 32);
    const auto height = static_cast<int>(frameSize & 0xffffffff);
    format = MediaFormat::CreateVideoFormat(mimeType, width, height);
    if (frameRate != 0) {
      const auto numerator = static_cast<int>(frameRate >> 32);
      const auto denominator = static_cast<int>(frameRate & 0xffffffff);
      if (denominator > 0) {
        format->setFrameRate(numerator / denominator);
      }
    }
    if (bitrate > 0) {
      format->setAverageBitrate(static_cast<int>(bitrate));
    }
    format->setPixelFormat(subtypeToPixelFormat(subType));
  } else {
    return nullptr;
  }

  UINT32 averageBitrate = 0;
  if (SUCCEEDED(mediaType->GetUINT32(MF_MT_AVG_BITRATE, &averageBitrate)) && averageBitrate > 0) {
    format->setAverageBitrate(static_cast<int>(averageBitrate));
  }

  fillMediaFormat(mediaType, format, compressed);
  return format;
}

PixelFormat WmfMediaUtils::subtypeToPixelFormat(const GUID& subType) {
  if (subType == kVideoPixelIYUV || subType == kVideoPixelYV12) {
    return PixelFormat::kFormatI420;
  }
  if (subType == kVideoPixelNV12) {
    return PixelFormat::kFormatNV12;
  }
  if (subType == kVideoPixelRGB32 || subType == kVideoPixelARGB32) {
    return PixelFormat::kFormatRGBA;
  }
  return PixelFormat::kFormatInvalid;
}

std::vector<uint8_t> WmfMediaUtils::createAacUserData(
  const std::vector<uint8_t>& audioSpecificConfig) {
  //!< MPEG-4 raw payload, without ADTS headers.
  constexpr uint16_t PAYLOAD_TYPE_RAW = 0;
  //!< AudioProfileLevelIndication of the AAC low complexity profile.
  constexpr uint16_t PROFILE_LEVEL_INDICATION_AAC_LC = 0x29;
  //!< The AudioSpecificConfig follows.
  constexpr uint16_t STRUCT_TYPE_AUDIO_SPECIFIC_CONFIG = 0;

  std::vector<uint8_t> blob(12 + audioSpecificConfig.size(), 0);
  blob[0] = static_cast<uint8_t>(PAYLOAD_TYPE_RAW & 0xFF);
  blob[1] = static_cast<uint8_t>(PAYLOAD_TYPE_RAW >> 8);
  blob[2] = static_cast<uint8_t>(PROFILE_LEVEL_INDICATION_AAC_LC & 0xFF);
  blob[3] = static_cast<uint8_t>(PROFILE_LEVEL_INDICATION_AAC_LC >> 8);
  blob[4] = static_cast<uint8_t>(STRUCT_TYPE_AUDIO_SPECIFIC_CONFIG & 0xFF);
  blob[5] = static_cast<uint8_t>(STRUCT_TYPE_AUDIO_SPECIFIC_CONFIG >> 8);
  if (!audioSpecificConfig.empty()) {
    memcpy(blob.data() + 12, audioSpecificConfig.data(), audioSpecificConfig.size());
  }
  return blob;
}
} // namespace coral
