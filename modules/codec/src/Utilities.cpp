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

#include <codec/Utilities.h>
#include <media/Utilities.h>

namespace coral {

CodecManufacturer CodecUtils::getAudioCodecManufacturer(const std::string& mimeType, const bool encoder) {
  if (!encoder) {
    // When decoding, we support hardware decoding if the mime type is one of the types
    auto manufacturer = CodecManufacturer::kSoftware;
    if (MimeTypes::getMediaType(mimeType) == MediaType::kMediaTypeAudio) {
      if (MediaUtils::convertToAudioFormatType(mimeType) != AudioFormatType::kAudioFormatTypeInvalid) {
        manufacturer = CodecManufacturer::kHardware;
      }
    }
    return manufacturer;
  }
  // When encoding, we only support hardware encoding when the mime type is "audio/mp4a-latm"
  if (mimeType.find(MimeTypes::AUDIO_AAC) != std::string::npos) {
    return CodecManufacturer::kHardware;
  }
  return CodecManufacturer::kSoftware;
}

CodecManufacturer CodecUtils::getAudioCodecManufacturer(const AudioFormatType formatType, const bool encoder) {
  if (!encoder) {
    // When decoding, we support hardware decoding if the mime type is one of the types
    auto manufacturer = CodecManufacturer::kSoftware;
    const auto mimeType = MediaUtils::convertToMimeType(formatType);
    if (!mimeType.empty() && mimeType.find("audio") != std::string::npos) {
      manufacturer = CodecManufacturer::kHardware;
    }
    return manufacturer;
  }
  if (formatType == AudioFormatType::kAudioFormatTypeMpeg4AAC) {
    return CodecManufacturer::kHardware;
  }
  return CodecManufacturer::kSoftware;
}

CodecManufacturer CodecUtils::getVideoCodecManufacturer(const std::string& mimeType, const bool encoder) {
  if (!encoder) {
    // When decoding, we support hardware decoding if the mime type is one of the types
    auto manufacturer = CodecManufacturer::kSoftware;
    if (MimeTypes::getMediaType(mimeType) == MediaType::kMediaTypeVideo) {
      if (MediaUtils::convertToVideoCodecType(mimeType) != VideoCodecType::kVideoCodecTypeInvalid) {
        manufacturer = CodecManufacturer::kHardware;
      }
    }
    return manufacturer;
  }
  // When encoding, we only support hardware encoding when the mime type is "video/avc" or "video/hevc"
  if (mimeType.find(MimeTypes::VIDEO_H264) != std::string::npos ||
    mimeType.find(MimeTypes::VIDEO_H265) != std::string::npos) {
    return CodecManufacturer::kHardware;
  }
  return CodecManufacturer::kSoftware;
}
} // namespace coral
