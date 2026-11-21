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

#if defined(ANDROID) || defined(__ANDROID__)

#include "android/MediaCodecVideoDecoder.h"
#include "android/MediaCodecVideoEncoder.h"
#include "android/MediaCodecUtils.h"
#include "android/NativeVideoDecoder.h"
#include "android/NativeVideoEncoder.h"

#endif

#if defined(WINDOWS) || defined(WIN32)

#include "win/WmfHardwareVideoEncoder.h"
#include "win/WmfMediaUtils.h"
#include "win/WmfVideoDecoder.h"
#include "win/WmfVideoEncoder.h"

#endif

#include <minilog/log.h>

namespace coral {

VideoDecoder::~VideoDecoder() = default;

VideoEncoder::~VideoEncoder() = default;

namespace {
/**
 * The default implementation of the VideoCodecFactory
 */
class VideoCodecFactoryImpl final : public VideoCodecFactory {
public:
  bool isSupportedCodec(VideoCodecType codecType, bool decoder) override;

  std::unique_ptr<VideoDecoder>
  createVideoDecoder(VideoCodecType codecType, CodecManufacturer manufacturer) override;

  std::unique_ptr<VideoEncoder>
  createVideoEncoder(VideoCodecType codecType,
      CodecManufacturer manufacturer,
      const std::shared_ptr<SurfaceHolder> &surfaceHolder) override;
};

bool VideoCodecFactoryImpl::isSupportedCodec(const VideoCodecType codecType, bool decoder) {
#if defined(WINDOWS) || defined(WIN32)
  // the media foundation platform must be started before the transforms can be enumerated.
  const WmfMediaScope mediaScope;
#endif
  if (decoder) {
#if defined(ANDROID) || defined(__ANDROID__)
    // android platform decoding supported
    if (MediaCodecUtils::supportVideoDecoding(codecType)) {
      return true;
    }
#endif
#if defined(WINDOWS) || defined(WIN32)
    // windows platform decoding supported
    GUID subType{};
    if (WmfMediaUtils::videoCodecTypeToSubtype(codecType, subType) &&
        WmfMediaUtils::isTransformAvailable(true, subType, true)) {
      return true;
    }
#endif
  } else {
#if defined(ANDROID) || defined(__ANDROID__)
    // android platform encoding supported
    if (MediaCodecUtils::supportVideoDecoding(codecType)) {
      return true;
    }
#endif
#if defined(WINDOWS) || defined(WIN32)
    // windows platform encoding supported
    GUID subType{};
    if (WmfMediaUtils::videoCodecTypeToSubtype(codecType, subType) &&
        WmfMediaUtils::isTransformAvailable(true, subType, false)) {
      return true;
    }
#endif
  }

  return false;
}

std::unique_ptr<VideoDecoder>
VideoCodecFactoryImpl::createVideoDecoder(const VideoCodecType codecType, const CodecManufacturer manufacturer) {
  // checks if we don't support VideoCodecType
  if (!isSupportedCodec(codecType, true)) {
    return nullptr;
  }

#if defined(ANDROID) || defined(__ANDROID__)
  // android platform video decoder
  if (manufacturer == CodecManufacturer::kHardware) {
    if (MediaCodecUtils::supportVideoDecoding(codecType)) {
#if defined(CORAL_SUPPORTS_NDK_MEDIACODEC)
      return std::make_unique<NativeVideoDecoder>();
#endif // CORAL_SUPPORTS_NDK_MEDIACODEC
      return std::make_unique<MediaCodecVideoDecoder>();
    }
  }
#endif

#if defined(WINDOWS) || defined(WIN32)
  // the windows platform video codecs are provided by the media foundation transforms, the
  // manufacturer is not relevant for the system provided codecs.
  static_cast<void>(manufacturer);
  return std::make_unique<WmfVideoDecoder>();
#endif

  return nullptr;
}

std::unique_ptr<VideoEncoder>
VideoCodecFactoryImpl::createVideoEncoder(const VideoCodecType codecType, const CodecManufacturer manufacturer,
    const std::shared_ptr<SurfaceHolder> &surfaceHolder) {
  // checks if we don't support VideoCodecType
  if (!isSupportedCodec(codecType, false)) {
    return nullptr;
  }

#if defined(ANDROID) || defined(__ANDROID__)
  // android platform video encoder
  if (manufacturer == CodecManufacturer::kHardware) {
    if (MediaCodecUtils::supportVideoEncoding(codecType)) {
      // AMediaCodec can't create an input surface object, only ANativeWindow available.
      // A null holder is the input buffer path, where the encoder takes the frames itself.
      if (surfaceHolder == nullptr || surfaceHolder->surfaceType() == SurfaceHolder::kNativeWindow) {
#if defined(CORAL_SUPPORTS_NDK_MEDIACODEC)
        return std::make_unique<NativeVideoEncoder>(surfaceHolder);
#endif
      }
      return std::make_unique<MediaCodecVideoEncoder>(surfaceHolder);
    }
  }
#endif

#if defined(WINDOWS) || defined(WIN32)
  // SurfaceHolder describes an encoder that creates an input Surface (Android MediaCodec). The
  // Windows hardware MFT does not: its GPU device is supplied to configure() as a
  // HardwareVideoSurface, so the holder is legitimately empty here.
  static_cast<void>(surfaceHolder);
  if (manufacturer == CodecManufacturer::kHardware) {
    if (WmfHardwareVideoEncoder::isSupported(codecType)) {
      return std::make_unique<WmfHardwareVideoEncoder>(codecType);
    }
    return nullptr;
  }
  return std::make_unique<WmfVideoEncoder>(codecType);
#endif

  return nullptr;
}

} // namespace anonymous

std::unique_ptr<VideoCodecFactory> CreateBuiltinVideoCodecFactory() {
  return std::make_unique<VideoCodecFactoryImpl>();
}

} // namespace coral
