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

#include <ffcodec/FfmpegVideoCodecFactory.h>

#include <ffcodec/FfmpegVideoDecoder.h>
#include <ffcodec/FfmpegVideoEncoder.h>

namespace coral {

FfmpegVideoCodecFactory::FfmpegVideoCodecFactory() = default;

FfmpegVideoCodecFactory::~FfmpegVideoCodecFactory() = default;

bool FfmpegVideoCodecFactory::isSupportedCodec(const VideoCodecType codecType, bool decoder) {
  return codecType != VideoCodecType::kVideoCodecTypeInvalid;
}

std::unique_ptr<VideoDecoder>
FfmpegVideoCodecFactory::createVideoDecoder(const VideoCodecType codecType, const CodecManufacturer manufacturer) {
  if (!isSupportedCodec(codecType, true)) {
    return {};
  }
  return std::make_unique<FfmpegVideoDecoder>();
}

std::unique_ptr<VideoEncoder>
FfmpegVideoCodecFactory::createVideoEncoder(const VideoCodecType codecType, const CodecManufacturer manufacturer,
    std::shared_ptr<SurfaceHolder> const &surfaceHolder) {
  if (!isSupportedCodec(codecType, false) || manufacturer != CodecManufacturer::kSoftware) {
    return {};
  }
  return std::make_unique<FfmpegVideoEncoder>();
}

} // namespace coral
