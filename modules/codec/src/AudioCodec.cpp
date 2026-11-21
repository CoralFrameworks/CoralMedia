/*
 * Copyright (C) 2022 The Coral Project
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

#include <codec/MediaCodecFactory.h>
#include <media/Utilities.h>

#if defined(ANDROID) || defined(__ANDROID__)

#include "android/MediaCodecAudioDecoder.h"
#include "android/MediaCodecAudioEncoder.h"
#include "android/NativeAudioDecoder.h"
#include "android/NativeAudioEncoder.h"
#include "android/MediaCodecUtils.h"

#endif

#if defined(WINDOWS) || defined(WIN32)

#include "win/WmfAudioDecoder.h"
#include "win/WmfAudioEncoder.h"
#include "win/WmfMediaUtils.h"

#endif

#include "PcmAudioDecoder.h"

namespace coral {

namespace {
/**
 * The default implementation of the AudioCodecFactory
 */
class AudioCodecFactoryImpl final : public AudioCodecFactory {
public:
  ~AudioCodecFactoryImpl() override = default;

  bool isSupportedCodec(AudioFormatType codecType, bool decoder) override;

  std::unique_ptr<AudioDecoder>
  createDecoder(const std::string& mimeType, bool outputFloat) override;

  std::unique_ptr<AudioEncoder>
  createEncoder(AudioFormatType codecType, CodecManufacturer manufacturer) override;
};

bool AudioCodecFactoryImpl::isSupportedCodec(const AudioFormatType codecType, bool decoder) {
  if (decoder && codecType == AudioFormatType::kAudioFormatTypeLinearPCM) {
    return true;
  }
#if defined(WINDOWS) || defined(WIN32)
  // the media foundation platform must be started before the transforms can be enumerated.
  const WmfMediaScope mediaScope;
#endif
  if (decoder) {
#if defined(ANDROID) || defined(__ANDROID__)
    // android platform decoding supported
    if (MediaCodecUtils::supportAudioDecoding(codecType)) {
      return true;
    }
#endif
#if defined(WINDOWS) || defined(WIN32)
    // windows platform decoding supported
    GUID subType{};
    if (WmfMediaUtils::audioFormatTypeToSubtype(codecType, subType) &&
        WmfMediaUtils::isTransformAvailable(false, subType, true)) {
      return true;
    }
#endif
  } else {
#if defined(ANDROID) || defined(__ANDROID__)
    // android platform encoding supported
    if (MediaCodecUtils::supportAudioEncoding(codecType)) {
      return true;
    }
#endif
#if defined(WINDOWS) || defined(WIN32)
    // windows platform encoding supported
    GUID subType{};
    if (WmfMediaUtils::audioFormatTypeToSubtype(codecType, subType) &&
        WmfMediaUtils::isTransformAvailable(false, subType, false)) {
      return true;
    }
#endif
  }

  return false;
}

std::unique_ptr<AudioDecoder>
AudioCodecFactoryImpl::createDecoder(const std::string& mimeType, bool outputFloat) {
  if (mimeType == MimeTypes::AUDIO_RAW) {
    return std::make_unique<PcmAudioDecoder>();
  }
#if defined(ANDROID) || defined(__ANDROID__)
  if (!outputFloat
      && isSupportedCodec(MediaUtils::convertToAudioFormatType(mimeType), true)) {
#if defined(CORAL_SUPPORTS_NDK_MEDIACODEC)
    return std::make_unique<NativeAudioDecoder>();
#endif
    return std::make_unique<MediaCodecAudioDecoder>();
  }
#endif

#if defined(WINDOWS) || defined(WIN32)
  if (isSupportedCodec(MediaUtils::convertToAudioFormatType(mimeType), true)) {
    return std::make_unique<WmfAudioDecoder>(outputFloat);
  }
#endif

  return nullptr;
}

std::unique_ptr<AudioEncoder>
AudioCodecFactoryImpl::createEncoder(const AudioFormatType codecType, const CodecManufacturer manufacturer) {
  // checks if we don't support AudioFormatType
  if (!isSupportedCodec(codecType, false)) {
    return nullptr;
  }

#if defined(ANDROID) || defined(__ANDROID__)
  // android platform video encoder
  if (manufacturer == CodecManufacturer::kHardware) {
    if (MediaCodecUtils::supportAudioEncoding(codecType)) {
#if defined(CORAL_SUPPORTS_NDK_MEDIACODEC)
      return std::make_unique<NativeAudioEncoder>();
#endif
      return std::make_unique<MediaCodecAudioEncoder>();
    }
  }
#endif

#if defined(WINDOWS) || defined(WIN32)
  // the windows platform audio encoder is provided by the media foundation transforms, the
  // manufacturer is not relevant for the system provided encoders.
  static_cast<void>(manufacturer);
  return std::make_unique<WmfAudioEncoder>(codecType);
#endif

  return nullptr;
}

} // namespace anonymous

std::unique_ptr<AudioCodecFactory> CreateBuiltinAudioCodecFactory() {
  return std::make_unique<AudioCodecFactoryImpl>();
}

} // namespace coral
