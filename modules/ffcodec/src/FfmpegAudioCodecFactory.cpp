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

#include <ffcodec/FfmpegAudioCodecFactory.h>

#include <ffcodec/FfmpegAudioDecoder.h>
#include <ffcodec/FfmpegAudioEncoder.h>

#include <media/Utilities.h>

namespace coral {

FfmpegAudioCodecFactory::FfmpegAudioCodecFactory() = default;

FfmpegAudioCodecFactory::~FfmpegAudioCodecFactory() = default;

bool FfmpegAudioCodecFactory::isSupportedCodec(const AudioFormatType codecType, bool decoder) {
  return codecType != AudioFormatType::kAudioFormatTypeInvalid;
}

std::unique_ptr<AudioDecoder>
FfmpegAudioCodecFactory::createDecoder(const std::string &mimeType, bool outputFloat) {
  if (!isSupportedCodec(MediaUtils::convertToAudioFormatType(mimeType), true)) {
    return {};
  }
  return std::make_unique<FfmpegAudioDecoder>(outputFloat);
}

std::unique_ptr<AudioEncoder>
FfmpegAudioCodecFactory::createEncoder(const AudioFormatType codecType, const CodecManufacturer manufacturer) {
  if (!isSupportedCodec(codecType, false) || manufacturer != CodecManufacturer::kSoftware) {
    return {};
  }
  return std::make_unique<FfmpegAudioEncoder>();
}

} // namespace coral
