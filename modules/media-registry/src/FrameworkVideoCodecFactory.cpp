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

#include "FrameworkVideoCodecFactory.h"

#include <media/Utilities.h>

#if defined(CORAL_SUPPORTS_FFMPEG)
#include <ffcodec/FfmpegVideoCodecFactory.h>
#endif

using namespace coral;

FrameworkVideoCodecFactory::FrameworkVideoCodecFactory() {
  builtinFactory = CreateBuiltinVideoCodecFactory();
#if defined(CORAL_SUPPORTS_FFMPEG)
  softwareFactory = std::make_unique<FfmpegVideoCodecFactory>();
#endif
}

FrameworkVideoCodecFactory::~FrameworkVideoCodecFactory() = default;

void FrameworkVideoCodecFactory::setSoftwareFactory(std::unique_ptr<VideoCodecFactory> factory) {
  softwareFactory = std::move(factory);
}

bool FrameworkVideoCodecFactory::isSupportedCodec(VideoCodecType codecType, bool decoder) {
  if (builtinFactory->isSupportedCodec(codecType, decoder) ||
      (softwareFactory && softwareFactory->isSupportedCodec(codecType, decoder))) {
    return true;
  }
  return false;
}

std::unique_ptr<VideoDecoder>
FrameworkVideoCodecFactory::createVideoDecoder(const VideoCodecType codecType, const CodecManufacturer manufacturer) {
  std::unique_ptr<VideoDecoder> decoder = nullptr;
  if (builtinFactory->isSupportedCodec(codecType, true)) {
    decoder = builtinFactory->createVideoDecoder(codecType, manufacturer);
  }
  if (!decoder && softwareFactory && softwareFactory->isSupportedCodec(codecType, true)) {
    decoder = softwareFactory->createVideoDecoder(codecType, manufacturer);
  }
  return decoder;
}

std::unique_ptr<VideoEncoder>
FrameworkVideoCodecFactory::createVideoEncoder(const VideoCodecType codecType, const CodecManufacturer manufacturer,
  const std::shared_ptr<SurfaceHolder>& surfaceHolder) {
  std::unique_ptr<VideoEncoder> encoder = nullptr;
  if (builtinFactory->isSupportedCodec(codecType, false)) {
    encoder = builtinFactory->createVideoEncoder(codecType, manufacturer, surfaceHolder);
  }
  if (!encoder && softwareFactory && softwareFactory->isSupportedCodec(codecType, false)) {
    encoder = softwareFactory->createVideoEncoder(codecType, manufacturer, surfaceHolder);
  }
  return encoder;
}
