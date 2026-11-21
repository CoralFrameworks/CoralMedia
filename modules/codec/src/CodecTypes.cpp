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

#include <codec/CodecTypes.h>
#include <media/Definitions.h>

namespace coral {

SeekMap::~SeekMap() noexcept = default;

} // namespace coral

using namespace coral;

template<>
const char* convertToText<CodecManufacturer>(const CodecManufacturer manufacturer) {
  switch (manufacturer) {
    case CodecManufacturer::kSoftware: {
      return "software";
    }
    case CodecManufacturer::kHardware: {
      return "hardware";
    }
    default: {
      return "unknown";
    }
  }
}

template<>
const char* convertToText<MuxerFormat>(const MuxerFormat format) {
  switch (format) {
    case MuxerFormat::kMuxerFormatInvalid: {
      return "invalid";
    }
    case MuxerFormat::kMuxerFormatMpeg4: {
      return "mp4";
    }
    case MuxerFormat::kMuxerFormatWebm: {
      return "webm";
    }
    case MuxerFormat::kMuxerFormat3Gpp: {
      return "3gpp";
    }
    case MuxerFormat::kMuxerFormatOgg: {
      return "ogg";
    }
    case MuxerFormat::kMuxerFormatOpus: {
      return "opus";
    }
    case MuxerFormat::kMuxerFormatFlac: {
      return "flac";
    }
    case MuxerFormat::kMuxerFormatH263: {
      return "h263";
    }
    case MuxerFormat::kMuxerFormatH264: {
      return "h264";
    }
  }
  return "Unknown";
}

