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

#include "FrameworkAudioCodecFactory.h"

#include <media/Utilities.h>

#if defined(CORAL_SUPPORTS_FFMPEG)
#include <ffcodec/FfmpegAudioCodecFactory.h>
#endif

using namespace coral;

FrameworkAudioCodecFactory::FrameworkAudioCodecFactory() {
  builtinFactory = CreateBuiltinAudioCodecFactory();
#if defined(CORAL_SUPPORTS_FFMPEG)
  softwareFactory = std::make_unique<FfmpegAudioCodecFactory>();
#endif
}

FrameworkAudioCodecFactory::~FrameworkAudioCodecFactory() = default;

void FrameworkAudioCodecFactory::setSoftwareCodecFactory(std::unique_ptr<AudioCodecFactory> factory) {
  softwareFactory = std::move(factory);
}

bool FrameworkAudioCodecFactory::isSupportedCodec(const AudioFormatType codecType, const bool decoder) {
  if (builtinFactory->isSupportedCodec(codecType, decoder) ||
      (softwareFactory && softwareFactory->isSupportedCodec(codecType, decoder))) {
    return true;
  }
  return false;
}

std::unique_ptr<AudioDecoder>
FrameworkAudioCodecFactory::createDecoder(const std::string& mimeType, const bool outputFloat) {
  std::unique_ptr<AudioDecoder> decoder = nullptr;
  const auto codecType = MediaUtils::convertToAudioFormatType(mimeType);
  if (builtinFactory->isSupportedCodec(codecType, true)) {
    decoder = builtinFactory->createDecoder(mimeType, outputFloat);
  }
  if (!decoder && softwareFactory && softwareFactory->isSupportedCodec(codecType, true)) {
    decoder = softwareFactory->createDecoder(mimeType, outputFloat);
  }
  return decoder;
}

std::unique_ptr<AudioEncoder>
FrameworkAudioCodecFactory::createEncoder(const AudioFormatType codecType, const CodecManufacturer manufacturer) {
  std::unique_ptr<AudioEncoder> encoder = nullptr;
  if (builtinFactory->isSupportedCodec(codecType, false)) {
    encoder = builtinFactory->createEncoder(codecType, manufacturer);
  }
  if (!encoder && softwareFactory && softwareFactory->isSupportedCodec(codecType, false)) {
    return softwareFactory->createEncoder(codecType, manufacturer);
  }
  return encoder;
}
