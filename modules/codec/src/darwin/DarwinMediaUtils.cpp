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

#if defined(__APPLE__)

#include "DarwinMediaUtils.h"

#include <media/Definitions.h>
#include <media/Types.h>
#include <media/Utilities.h>

#include <TargetConditionals.h>
#include <VideoToolbox/VideoToolbox.h>
#include <minilog/log.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace coral::darwin {

namespace {

constexpr int kDefaultNalLengthSize = 4;
constexpr int kAacFramesPerPacket = 1024;
constexpr int kMpeg3FramesPerPacket = 1152;
constexpr int kAc3FramesPerPacket = 1536;
constexpr int kAlacFramesPerPacket = 4096;
constexpr int kAmrFramesPerPacket = 160;
constexpr int kAmrWbFramesPerPacket = 320;

constexpr int kDefaultAudioBitrate = 128000;

uint16_t readBe16(const uint8_t* data) {
  return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);
}

uint32_t readBe32(const uint8_t* data) {
  return (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) |
         (static_cast<uint32_t>(data[2]) << 8) | data[3];
}

void appendBe16(std::vector<uint8_t>& output, const size_t value) {
  output.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
  output.push_back(static_cast<uint8_t>(value & 0xFF));
}

void appendBe32(std::vector<uint8_t>& output, const uint32_t value) {
  output.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
  output.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
  output.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
  output.push_back(static_cast<uint8_t>(value & 0xFF));
}

/**
 * Appends every NAL unit of an Annex B byte stream, without its start code.
 *
 * A four byte start code has a zero in front of the three byte one, which is not part of the
 * preceding NAL unit and is therefore skipped.
 */
void appendAnnexBNals(const uint8_t* data, const size_t size,
    std::vector<std::vector<uint8_t>>& nals) {
  size_t index = 0;
  while (index + 3 <= size) {
    size_t startCode = std::string::npos;
    size_t startCodeLength = 0;
    for (size_t i = index; i + 3 <= size; ++i) {
      if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
        startCode = i;
        startCodeLength = 3;
        break;
      }
    }
    if (startCode == std::string::npos) {
      break;
    }
    const size_t nalStart = startCode + startCodeLength;
    size_t nextStartCode = std::string::npos;
    for (size_t i = nalStart; i + 3 <= size; ++i) {
      if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
        nextStartCode = i;
        break;
      }
    }
    size_t nalEnd = nextStartCode == std::string::npos ? size : nextStartCode;
    // The trailing zero of a four byte start code belongs to the start code, not to the NAL unit.
    if (nextStartCode != std::string::npos && nalEnd > nalStart && data[nalEnd - 1] == 0) {
      --nalEnd;
    }
    if (nalEnd > nalStart) {
      nals.emplace_back(data + nalStart, data + nalEnd);
    }
    index = nalEnd;
    if (nextStartCode == std::string::npos) {
      break;
    }
  }
}

/**
 * Splits a blob that is a sequence of NAL units prefixed by a length of the given size.
 *
 * @return true when the whole blob is such a sequence, in which case the units were appended.
 */
[[nodiscard]] bool appendLengthPrefixedNals(const std::vector<uint8_t>& blob,
    const int lengthSize, std::vector<std::vector<uint8_t>>& nals) {
  if (lengthSize <= 0 || lengthSize > 4 || blob.size() < static_cast<size_t>(lengthSize) + 1) {
    return false;
  }
  std::vector<std::vector<uint8_t>> units;
  size_t offset = 0;
  while (offset + static_cast<size_t>(lengthSize) <= blob.size()) {
    uint32_t length = 0;
    for (int i = 0; i < lengthSize; ++i) {
      length = (length << 8) | blob[offset + static_cast<size_t>(i)];
    }
    offset += static_cast<size_t>(lengthSize);
    if (length == 0 || offset + length > blob.size()) {
      return false;
    }
    units.emplace_back(blob.begin() + static_cast<ptrdiff_t>(offset),
      blob.begin() + static_cast<ptrdiff_t>(offset + length));
    offset += length;
  }
  if (offset != blob.size() || units.empty()) {
    return false;
  }
  nals.insert(nals.end(), std::make_move_iterator(units.begin()),
    std::make_move_iterator(units.end()));
  return true;
}

/**
 * Splits one codec specific data blob into NAL units, accepting the forms the containers store.
 *
 * The length prefixed forms are tried before the one byte form: a raw parameter set whose first byte
 * happens to be its size, which is the one byte length prefix of a one unit blob, is the only
 * ambiguous case, and a two or four byte length prefix cannot be mistaken for it.
 */
void appendBlobNals(const std::vector<uint8_t>& blob,
    std::vector<std::vector<uint8_t>>& nals) {
  if (blob.empty()) {
    return;
  }
  if (blob.size() >= 4 && blob[0] == 0 && blob[1] == 0 && blob[2] == 0 && blob[3] == 1) {
    appendAnnexBNals(blob.data(), blob.size(), nals);
    return;
  }
  if (blob.size() >= 3 && blob[0] == 0 && blob[1] == 0 && blob[2] == 1) {
    appendAnnexBNals(blob.data(), blob.size(), nals);
    return;
  }
  // The length prefixes the containers use: four bytes (AVCC/HEVC in MP4), two bytes (some
  // Matroska/HEIF writers) and one byte (the extractor module's AVC codec specific data).
  if (appendLengthPrefixedNals(blob, 4, nals) || appendLengthPrefixedNals(blob, 2, nals) ||
      appendLengthPrefixedNals(blob, 1, nals)) {
    return;
  }
  nals.push_back(blob);
}

/** Appends the parameter sets of an `avcC` configuration record, without the record header. */
void appendAvcCParameterSets(const std::vector<uint8_t>& blob,
    std::vector<std::vector<uint8_t>>& nals) {
  if (blob.size() < 7 || blob[0] != 1) {
    return;
  }
  size_t offset = 5;
  const size_t spsCount = blob[offset] & 0x1F;
  offset += 1;
  for (size_t i = 0; i < spsCount && offset + 2 <= blob.size(); ++i) {
    const size_t size = readBe16(blob.data() + offset);
    offset += 2;
    if (size == 0 || offset + size > blob.size()) {
      break;
    }
    nals.emplace_back(blob.begin() + static_cast<ptrdiff_t>(offset),
      blob.begin() + static_cast<ptrdiff_t>(offset + size));
    offset += size;
  }
  if (offset + 1 > blob.size()) {
    return;
  }
  const size_t ppsCount = blob[offset];
  offset += 1;
  for (size_t i = 0; i < ppsCount && offset + 2 <= blob.size(); ++i) {
    const size_t size = readBe16(blob.data() + offset);
    offset += 2;
    if (size == 0 || offset + size > blob.size()) {
      break;
    }
    nals.emplace_back(blob.begin() + static_cast<ptrdiff_t>(offset),
      blob.begin() + static_cast<ptrdiff_t>(offset + size));
    offset += size;
  }
}

/** Appends the parameter sets of an `hvcC` configuration record. */
void appendHvcCParameterSets(const std::vector<uint8_t>& blob,
    std::vector<std::vector<uint8_t>>& nals) {
  constexpr size_t kHeaderSize = 23;
  if (blob.size() <= kHeaderSize || blob[0] != 1) {
    return;
  }
  size_t offset = kHeaderSize;
  const size_t arrayCount = blob[22];
  for (size_t array = 0; array < arrayCount && offset + 3 <= blob.size(); ++array) {
    const size_t count = readBe16(blob.data() + offset + 1);
    offset += 3;
    for (size_t i = 0; i < count && offset + 2 <= blob.size(); ++i) {
      const size_t size = readBe16(blob.data() + offset);
      offset += 2;
      if (size == 0 || offset + size > blob.size()) {
        return;
      }
      nals.emplace_back(blob.begin() + static_cast<ptrdiff_t>(offset),
        blob.begin() + static_cast<ptrdiff_t>(offset + size));
      offset += size;
    }
  }
}

[[nodiscard]] bool isAvcC(const std::vector<uint8_t>& blob) {
  return blob.size() >= 7 && blob[0] == 1;
}

[[nodiscard]] bool isHvcC(const std::vector<uint8_t>& blob) {
  return blob.size() > 23 && blob[0] == 1;
}

/** Writes the expandable length field of an MPEG-4 descriptor, always in four bytes. */
void appendDescriptorLength(std::vector<uint8_t>& output, const size_t length) {
  output.push_back(static_cast<uint8_t>(0x80 | ((length >> 21) & 0x7F)));
  output.push_back(static_cast<uint8_t>(0x80 | ((length >> 14) & 0x7F)));
  output.push_back(static_cast<uint8_t>(0x80 | ((length >> 7) & 0x7F)));
  output.push_back(static_cast<uint8_t>(length & 0x7F));
}

[[nodiscard]] bool readDescriptorLength(const uint8_t*& cursor, const uint8_t* end,
    size_t& length) {
  length = 0;
  for (int i = 0; i < 4; ++i) {
    if (cursor >= end) {
      return false;
    }
    const uint8_t byte = *cursor++;
    length = (length << 7) | (byte & 0x7F);
    if ((byte & 0x80) == 0) {
      return true;
    }
  }
  return true;
}

/**
 * Builds an `avcC` decoder configuration record from H.264 parameter sets.
 *
 * The record is the one the sample description extension atoms of a `CMVideoFormatDescription`
 * hold. It is what lets a format description be created by `CMVideoFormatDescriptionCreate`, the
 * only creation function that takes an extensions dictionary on every release of the SDK, and it is
 * also what carries the parameter sets to the decoders and to the muxer.
 *
 * @return the record, or an empty vector when the parameter sets do not describe one.
 */
[[nodiscard]] std::vector<uint8_t> avcCFromParameterSets(
    const std::vector<std::vector<uint8_t>>& parameterSets) {
  std::vector<std::vector<uint8_t>> sps;
  std::vector<std::vector<uint8_t>> pps;
  for (const auto& nal : parameterSets) {
    if (nal.empty()) {
      continue;
    }
    switch (nal[0] & 0x1F) {
      case 7:
        sps.push_back(nal);
        break;
      case 8:
        pps.push_back(nal);
        break;
      default:
        break;
    }
  }
  if (sps.empty() || pps.empty() || sps[0].size() < 4 || sps.size() > 31 || pps.size() > 255) {
    return {};
  }

  std::vector<uint8_t> record;
  record.reserve(11 + pps.size());
  record.push_back(1); // configurationVersion
  record.push_back(sps[0][1]); // AVCProfileIndication
  record.push_back(sps[0][2]); // profile_compatibility
  record.push_back(sps[0][3]); // AVCLevelIndication
  record.push_back(0xFF); // reserved bits and lengthSizeMinusOne = 3 (four byte lengths)
  record.push_back(static_cast<uint8_t>(0xE0 | sps.size()));
  for (const auto& sequence : sps) {
    appendBe16(record, sequence.size());
    record.insert(record.end(), sequence.begin(), sequence.end());
  }
  record.push_back(static_cast<uint8_t>(pps.size()));
  for (const auto& picture : pps) {
    appendBe16(record, picture.size());
    record.insert(record.end(), picture.begin(), picture.end());
  }
  return record;
}

/** The extensions the muxer and the decoder copy from the coral color information. */
CFMutableDictionaryRef createVideoExtensions(const ColorInfo& colorInfo) {
  CFMutableDictionaryRef extensions = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  if (extensions == nullptr) {
    return nullptr;
  }

  const CFStringRef primaries = [&colorInfo]() -> CFStringRef {
    switch (colorInfo.colorSpace) {
      case ColorInfo::SpaceType::kSpaceBT709:
        return kCMFormatDescriptionColorPrimaries_ITU_R_709_2;
      case ColorInfo::SpaceType::kSpaceBT601:
        return kCVImageBufferColorPrimaries_SMPTE_C;
      case ColorInfo::SpaceType::kSpaceBT2020:
        return kCMFormatDescriptionColorPrimaries_ITU_R_2020;
      case ColorInfo::SpaceType::kSpaceUnknown:
      default:
        return nullptr;
    }
  }();
  if (primaries != nullptr) {
    CFDictionarySetValue(extensions, kCMFormatDescriptionExtension_ColorPrimaries, primaries);
  }

  const CFStringRef transfer = [&colorInfo]() -> CFStringRef {
    switch (colorInfo.colorTransfer) {
      case ColorInfo::TransferType::kTransferSDR:
        return kCMFormatDescriptionTransferFunction_ITU_R_709_2;
      case ColorInfo::TransferType::kTransferST2084:
        return kCMFormatDescriptionTransferFunction_SMPTE_ST_2084_PQ;
      case ColorInfo::TransferType::kTransferHLG:
        return kCMFormatDescriptionTransferFunction_ITU_R_2100_HLG;
      case ColorInfo::TransferType::kTransferSRGB:
        return kCMFormatDescriptionTransferFunction_sRGB;
      case ColorInfo::TransferType::kTransferLinear:
        return kCMFormatDescriptionTransferFunction_Linear;
      case ColorInfo::TransferType::kTransferGamma22:
        return kCMFormatDescriptionTransferFunction_UseGamma;
      case ColorInfo::TransferType::kTransferUnknown:
      default:
        return nullptr;
    }
  }();
  if (transfer != nullptr) {
    CFDictionarySetValue(extensions, kCMFormatDescriptionExtension_TransferFunction, transfer);
  }

  const CFStringRef matrix = [&colorInfo]() -> CFStringRef {
    switch (colorInfo.colorSpace) {
      case ColorInfo::SpaceType::kSpaceBT709:
        return kCMFormatDescriptionYCbCrMatrix_ITU_R_709_2;
      case ColorInfo::SpaceType::kSpaceBT601:
        return kCMFormatDescriptionYCbCrMatrix_ITU_R_601_4;
      case ColorInfo::SpaceType::kSpaceBT2020:
        return kCMFormatDescriptionYCbCrMatrix_ITU_R_2020;
      case ColorInfo::SpaceType::kSpaceUnknown:
      default:
        return nullptr;
    }
  }();
  if (matrix != nullptr) {
    CFDictionarySetValue(extensions, kCMFormatDescriptionExtension_YCbCrMatrix, matrix);
  }

  if (colorInfo.colorRange == ColorInfo::RangeType::kRangeFull) {
    CFDictionarySetValue(extensions, kCMFormatDescriptionExtension_FullRangeVideo,
      kCFBooleanTrue);
  } else if (colorInfo.colorRange == ColorInfo::RangeType::kRangeLimited) {
    CFDictionarySetValue(extensions, kCMFormatDescriptionExtension_FullRangeVideo,
      kCFBooleanFalse);
  }
  return extensions;
}

[[nodiscard]] bool equalExtension(CMFormatDescriptionRef description, CFStringRef key,
    CFStringRef value) {
  if (description == nullptr || key == nullptr || value == nullptr) {
    return false;
  }
  CFTypeRef extension = CMFormatDescriptionGetExtension(description, key);
  return extension != nullptr && CFGetTypeID(extension) == CFStringGetTypeID() &&
         CFEqual(extension, value);
}

} // namespace anonymous

// ==================================================================================================
// Logging
// ==================================================================================================

std::string osStatusToString(const OSStatus status) {
  const auto value = static_cast<uint32_t>(status);
  const char chars[5] = {
    static_cast<char>((value >> 24) & 0xFF),
    static_cast<char>((value >> 16) & 0xFF),
    static_cast<char>((value >> 8) & 0xFF),
    static_cast<char>(value & 0xFF),
    '\0'
  };
  const bool printable = std::all_of(chars, chars + 4, [](const char c) {
    return c >= 32 && c <= 126;
  });
  if (printable) {
    return std::string("'") + chars + "' (" + std::to_string(status) + ")";
  }
  return std::to_string(status);
}

bool logStatus(const OSStatus status, const std::string& message, const std::string& detail) {
  if (detail.empty()) {
    MINILOG_ERROR("{}: {}", message, osStatusToString(status));
  } else {
    MINILOG_ERROR("{}: {} ({})", message, osStatusToString(status), detail);
  }
  return false;
}

void logFailure(const std::string& message, const std::string& detail) {
  if (detail.empty()) {
    MINILOG_ERROR("{}", message);
  } else {
    MINILOG_ERROR("{}: {}", message, detail);
  }
}

// ==================================================================================================
// Codec support and codec identifiers
// ==================================================================================================

bool isVideoDecoderSupported(const VideoCodecType codecType) {
  switch (codecType) {
    case VideoCodecType::kVideoCodecTypeH264:
    case VideoCodecType::kVideoCodecTypeHevc:
    case VideoCodecType::kVideoCodecTypeDolbyVision:
      return true;
#if TARGET_OS_OSX
    // The desktop VideoToolbox still carries the MPEG-4 Part 2, MPEG-2 and H.263 decoders; the iOS
    // VideoToolbox of the same release does not, so claiming them there would keep the registry
    // from falling back to the software codecs.
    case VideoCodecType::kVideoCodecTypeMpeg4Video:
    case VideoCodecType::kVideoCodecTypeMpeg2Video:
    case VideoCodecType::kVideoCodecTypeH263:
      return true;
#endif
    case VideoCodecType::kVideoCodecTypeVP9:
      return VTIsHardwareDecodeSupported(kCMVideoCodecType_VP9);
    case VideoCodecType::kVideoCodecTypeAV1:
      return VTIsHardwareDecodeSupported(kCMVideoCodecType_AV1);
    default:
      return false;
  }
}

bool isVideoEncoderSupported(const VideoCodecType codecType) {
  return codecType == VideoCodecType::kVideoCodecTypeH264 ||
         codecType == VideoCodecType::kVideoCodecTypeHevc;
}

bool isAudioDecoderSupported(const AudioFormatType formatType) {
  switch (formatType) {
    case AudioFormatType::kAudioFormatTypeLinearPCM:
    case AudioFormatType::kAudioFormatTypeAC3:
    case AudioFormatType::kAudioFormatTypeMpeg3:
    case AudioFormatType::kAudioFormatTypeMpeg4AAC:
    case AudioFormatType::kAudioFormatTypeALaw:
    case AudioFormatType::kAudioFormatTypeMLaw:
    case AudioFormatType::kAudioFormatTypeAMR:
    case AudioFormatType::kAudioFormatTypeAMR_WB:
    case AudioFormatType::kAudioFormatTypeFlac:
    case AudioFormatType::kAudioFormatTypeOpus:
    case AudioFormatType::kAudioFormatTypeAlac:
      return true;
    default:
      return false;
  }
}

bool isAudioEncoderSupported(const AudioFormatType formatType) {
  return formatType == AudioFormatType::kAudioFormatTypeMpeg4AAC ||
         formatType == AudioFormatType::kAudioFormatTypeAlac;
}

CMVideoCodecType toCMVideoCodecType(const VideoCodecType codecType) {
  switch (codecType) {
    case VideoCodecType::kVideoCodecTypeH263:
      return kCMVideoCodecType_H263;
    case VideoCodecType::kVideoCodecTypeH264:
      return kCMVideoCodecType_H264;
    case VideoCodecType::kVideoCodecTypeHevc:
      return kCMVideoCodecType_HEVC;
    case VideoCodecType::kVideoCodecTypeAV1:
      return kCMVideoCodecType_AV1;
    case VideoCodecType::kVideoCodecTypeMpeg2Video:
      return kCMVideoCodecType_MPEG2Video;
    case VideoCodecType::kVideoCodecTypeMpeg4Video:
      return kCMVideoCodecType_MPEG4Video;
    case VideoCodecType::kVideoCodecTypeVP9:
      return kCMVideoCodecType_VP9;
    case VideoCodecType::kVideoCodecTypeDolbyVision:
      return kCMVideoCodecType_DolbyVisionHEVC;
    default:
      return static_cast<CMVideoCodecType>(0);
  }
}

VideoCodecType toVideoCodecType(const CMVideoCodecType codecType) {
  if (codecType == kCMVideoCodecType_H263) {
    return VideoCodecType::kVideoCodecTypeH263;
  }
  if (codecType == kCMVideoCodecType_H264) {
    return VideoCodecType::kVideoCodecTypeH264;
  }
  if (codecType == kCMVideoCodecType_HEVC) {
    return VideoCodecType::kVideoCodecTypeHevc;
  }
  if (codecType == kCMVideoCodecType_AV1) {
    return VideoCodecType::kVideoCodecTypeAV1;
  }
  if (codecType == kCMVideoCodecType_MPEG2Video) {
    return VideoCodecType::kVideoCodecTypeMpeg2Video;
  }
  if (codecType == kCMVideoCodecType_MPEG4Video) {
    return VideoCodecType::kVideoCodecTypeMpeg4Video;
  }
  if (codecType == kCMVideoCodecType_VP9) {
    return VideoCodecType::kVideoCodecTypeVP9;
  }
  if (codecType == kCMVideoCodecType_DolbyVisionHEVC) {
    return VideoCodecType::kVideoCodecTypeDolbyVision;
  }
  return VideoCodecType::kVideoCodecTypeInvalid;
}

AudioFormatID toAudioFormatID(const AudioFormatType formatType) {
  switch (formatType) {
    case AudioFormatType::kAudioFormatTypeLinearPCM:
      return kAudioFormatLinearPCM;
    case AudioFormatType::kAudioFormatTypeAC3:
      return kAudioFormatAC3;
    case AudioFormatType::kAudioFormatTypeMpeg3:
      return kAudioFormatMPEGLayer3;
    case AudioFormatType::kAudioFormatTypeMpeg4AAC:
      return kAudioFormatMPEG4AAC;
    case AudioFormatType::kAudioFormatTypeALaw:
      return kAudioFormatALaw;
    case AudioFormatType::kAudioFormatTypeMLaw:
      return kAudioFormatULaw;
    case AudioFormatType::kAudioFormatTypeAMR:
      return kAudioFormatAMR;
    case AudioFormatType::kAudioFormatTypeAMR_WB:
      return kAudioFormatAMR_WB;
    case AudioFormatType::kAudioFormatTypeFlac:
      return kAudioFormatFLAC;
    case AudioFormatType::kAudioFormatTypeOpus:
      return kAudioFormatOpus;
    case AudioFormatType::kAudioFormatTypeAlac:
      return kAudioFormatAppleLossless;
    default:
      return 0;
  }
}

AudioFormatType toAudioFormatType(const AudioFormatID formatID) {
  if (formatID == kAudioFormatLinearPCM) {
    return AudioFormatType::kAudioFormatTypeLinearPCM;
  }
  if (formatID == kAudioFormatAC3) {
    return AudioFormatType::kAudioFormatTypeAC3;
  }
  if (formatID == kAudioFormatMPEGLayer3) {
    return AudioFormatType::kAudioFormatTypeMpeg3;
  }
  if (formatID == kAudioFormatMPEG4AAC || formatID == kAudioFormatMPEG4AAC_HE ||
      formatID == kAudioFormatMPEG4AAC_HE_V2 || formatID == kAudioFormatMPEG4AAC_LD ||
      formatID == kAudioFormatMPEG4AAC_ELD || formatID == kAudioFormatMPEG4AAC_Spatial) {
    return AudioFormatType::kAudioFormatTypeMpeg4AAC;
  }
  if (formatID == kAudioFormatALaw) {
    return AudioFormatType::kAudioFormatTypeALaw;
  }
  if (formatID == kAudioFormatULaw) {
    return AudioFormatType::kAudioFormatTypeMLaw;
  }
  if (formatID == kAudioFormatAMR) {
    return AudioFormatType::kAudioFormatTypeAMR;
  }
  if (formatID == kAudioFormatAMR_WB) {
    return AudioFormatType::kAudioFormatTypeAMR_WB;
  }
  if (formatID == kAudioFormatFLAC) {
    return AudioFormatType::kAudioFormatTypeFlac;
  }
  if (formatID == kAudioFormatOpus) {
    return AudioFormatType::kAudioFormatTypeOpus;
  }
  if (formatID == kAudioFormatAppleLossless) {
    return AudioFormatType::kAudioFormatTypeAlac;
  }
  return AudioFormatType::kAudioFormatTypeInvalid;
}

std::string mimeTypeOfVideoCodec(const VideoCodecType codecType) {
  switch (codecType) {
    case VideoCodecType::kVideoCodecTypeH263:
      return MimeTypes::VIDEO_H263;
    case VideoCodecType::kVideoCodecTypeH264:
      return MimeTypes::VIDEO_H264;
    case VideoCodecType::kVideoCodecTypeHevc:
      return MimeTypes::VIDEO_H265;
    case VideoCodecType::kVideoCodecTypeAV1:
      return MimeTypes::VIDEO_AV1;
    case VideoCodecType::kVideoCodecTypeMpeg2Video:
      return MimeTypes::VIDEO_MPEG2;
    case VideoCodecType::kVideoCodecTypeMpeg4Video:
      return MimeTypes::VIDEO_MP4V;
    case VideoCodecType::kVideoCodecTypeVP9:
      return MimeTypes::VIDEO_VP9;
    case VideoCodecType::kVideoCodecTypeDolbyVision:
      return MimeTypes::VIDEO_DOLBY_VISION;
    default:
      return {};
  }
}

std::string mimeTypeOfAudioFormat(const AudioFormatType formatType) {
  switch (formatType) {
    case AudioFormatType::kAudioFormatTypeLinearPCM:
      return MimeTypes::AUDIO_RAW;
    case AudioFormatType::kAudioFormatTypeAC3:
      return MimeTypes::AUDIO_AC3;
    case AudioFormatType::kAudioFormatTypeMpeg3:
      return MimeTypes::AUDIO_MPEG;
    case AudioFormatType::kAudioFormatTypeMpeg4AAC:
      return MimeTypes::AUDIO_AAC;
    case AudioFormatType::kAudioFormatTypeALaw:
      return MimeTypes::AUDIO_G711_ALAW;
    case AudioFormatType::kAudioFormatTypeMLaw:
      return MimeTypes::AUDIO_G711_MLAW;
    case AudioFormatType::kAudioFormatTypeAMR:
      return MimeTypes::AUDIO_AMR;
    case AudioFormatType::kAudioFormatTypeAMR_WB:
      return MimeTypes::AUDIO_AMR_WB;
    case AudioFormatType::kAudioFormatTypeFlac:
      return MimeTypes::AUDIO_FLAC;
    case AudioFormatType::kAudioFormatTypeOpus:
      return MimeTypes::AUDIO_OPUS;
    case AudioFormatType::kAudioFormatTypeAlac:
      return MimeTypes::AUDIO_ALAC;
    default:
      return {};
  }
}

int defaultFramesPerPacket(const AudioFormatType formatType) {
  switch (formatType) {
    case AudioFormatType::kAudioFormatTypeMpeg4AAC:
      return kAacFramesPerPacket;
    case AudioFormatType::kAudioFormatTypeMpeg3:
      return kMpeg3FramesPerPacket;
    case AudioFormatType::kAudioFormatTypeAC3:
      return kAc3FramesPerPacket;
    case AudioFormatType::kAudioFormatTypeAlac:
      return kAlacFramesPerPacket;
    case AudioFormatType::kAudioFormatTypeAMR:
      return kAmrFramesPerPacket;
    case AudioFormatType::kAudioFormatTypeAMR_WB:
      return kAmrWbFramesPerPacket;
    default:
      return 0;
  }
}

AudioSampleFormat sampleFormatOf(const AudioStreamBasicDescription& format) {
  if ((format.mFormatFlags & kAudioFormatFlagIsFloat) != 0) {
    return AudioSampleFormat::kFloat;
  }
  if ((format.mFormatFlags & kAudioFormatFlagIsSignedInteger) != 0) {
    switch (format.mBitsPerChannel) {
      case 16:
        return AudioSampleFormat::kInt16;
      case 24:
        return AudioSampleFormat::kInt24;
      case 32:
        return AudioSampleFormat::kInt32;
      default:
        break;
    }
  }
  return AudioSampleFormat::kInvalid;
}

int bytesPerFrameOf(const AudioStreamBasicDescription& format) {
  if (format.mChannelsPerFrame == 0) {
    return 0;
  }
  if (format.mBytesPerFrame > 0) {
    return static_cast<int>(format.mBytesPerFrame);
  }
  const auto sampleFormat = sampleFormatOf(format);
  switch (sampleFormat) {
    case AudioSampleFormat::kInt16:
      return static_cast<int>(format.mChannelsPerFrame) * 2;
    case AudioSampleFormat::kInt24:
      return static_cast<int>(format.mChannelsPerFrame) * 3;
    case AudioSampleFormat::kInt32:
    case AudioSampleFormat::kFloat:
      return static_cast<int>(format.mChannelsPerFrame) * 4;
    default:
      return 0;
  }
}

// ==================================================================================================
// Audio stream descriptions
// ==================================================================================================

AudioStreamBasicDescription makePcmFormat(const int sampleRate, const int channelCount,
    const AudioSampleFormat sampleFormat) {
  AudioStreamBasicDescription format{};
  format.mSampleRate = static_cast<Float64>(sampleRate > 0 ? sampleRate
                                                            : Definitions::DEFAULT_SAMPLE_RATE);
  format.mFormatID = kAudioFormatLinearPCM;
  format.mChannelsPerFrame = static_cast<UInt32>(channelCount > 0 ? channelCount
                                                                 : Definitions::DEFAULT_CHANNEL_COUNT);
  format.mFramesPerPacket = 1;
  switch (sampleFormat) {
    case AudioSampleFormat::kInt16:
      format.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
      format.mBitsPerChannel = 16;
      break;
    case AudioSampleFormat::kInt24:
      format.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
      format.mBitsPerChannel = 24;
      break;
    case AudioSampleFormat::kInt32:
      format.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
      format.mBitsPerChannel = 32;
      break;
    case AudioSampleFormat::kFloat:
      format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
      format.mBitsPerChannel = 32;
      break;
    default:
      format.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
      format.mBitsPerChannel = 16;
      break;
  }
  format.mBytesPerFrame =
    static_cast<UInt32>(bytesPerFrameOf(format));
  format.mBytesPerPacket = format.mBytesPerFrame;
  return format;
}

AudioStreamBasicDescription makeCompressedFormat(const AudioFormatType formatType,
    const int sampleRate, const int channelCount) {
  AudioStreamBasicDescription format{};
  format.mSampleRate = static_cast<Float64>(sampleRate > 0 ? sampleRate
                                                            : Definitions::DEFAULT_SAMPLE_RATE);
  format.mFormatID = toAudioFormatID(formatType);
  format.mChannelsPerFrame = static_cast<UInt32>(channelCount > 0 ? channelCount
                                                                 : Definitions::DEFAULT_CHANNEL_COUNT);
  format.mFramesPerPacket = static_cast<UInt32>(std::max(0, defaultFramesPerPacket(formatType)));
  return format;
}

// ==================================================================================================
// Codec specific data
// ==================================================================================================

std::vector<std::vector<uint8_t>> h264ParameterSets(const MediaFormat& format) {
  std::vector<std::vector<uint8_t>> nals;
  for (const auto& blob : format.csdList()) {
    if (isAvcC(blob)) {
      appendAvcCParameterSets(blob, nals);
    } else {
      appendBlobNals(blob, nals);
    }
  }
  std::vector<std::vector<uint8_t>> ordered;
  for (const auto& nal : nals) {
    if (!nal.empty() && ((nal[0] & 0x1F) == 7 || (nal[0] & 0x1F) == 8)) {
      ordered.push_back(nal);
    }
  }
  // SPS before PPS, the order the creation API expects.
  std::vector<std::vector<uint8_t>> sorted;
  for (const auto type : { 7, 8 }) {
    for (const auto& nal : ordered) {
      if ((nal[0] & 0x1F) == type) {
        sorted.push_back(nal);
      }
    }
  }
  return sorted;
}

std::vector<std::vector<uint8_t>> hevcParameterSets(const MediaFormat& format) {
  std::vector<std::vector<uint8_t>> nals;
  for (const auto& blob : format.csdList()) {
    if (isHvcC(blob)) {
      appendHvcCParameterSets(blob, nals);
    } else {
      appendBlobNals(blob, nals);
    }
  }
  std::vector<std::vector<uint8_t>> ordered;
  for (const auto& nal : nals) {
    if (nal.empty()) {
      continue;
    }
    const auto type = static_cast<uint8_t>((nal[0] >> 1) & 0x3F);
    // The parameter sets, and the prefix and suffix SEI NAL units an hvcC record may carry as
    // well: the SEI units of a stream describe its HDR metadata, and the creation API accepts them.
    if (type == 32 || type == 33 || type == 34 || type == 39 || type == 40) {
      ordered.push_back(nal);
    }
  }
  std::vector<std::vector<uint8_t>> sorted;
  for (const auto type : { 32, 33, 34, 39, 40 }) {
    for (const auto& nal : ordered) {
      if ((nal[0] >> 1 & 0x3F) == type) {
        sorted.push_back(nal);
      }
    }
  }
  return sorted;
}

std::vector<uint8_t> hvcCFromParameterSets(
    const std::vector<std::vector<uint8_t>>& parameterSets) {
  std::vector<std::vector<uint8_t>> vps;
  std::vector<std::vector<uint8_t>> sps;
  std::vector<std::vector<uint8_t>> pps;
  std::vector<std::vector<uint8_t>> prefixSei;
  std::vector<std::vector<uint8_t>> suffixSei;
  for (const auto& nal : parameterSets) {
    if (nal.empty()) {
      continue;
    }
    switch ((nal[0] >> 1) & 0x3F) {
      case 32:
        vps.push_back(nal);
        break;
      case 33:
        sps.push_back(nal);
        break;
      case 34:
        pps.push_back(nal);
        break;
      case 39:
        prefixSei.push_back(nal);
        break;
      case 40:
        suffixSei.push_back(nal);
        break;
      default:
        break;
    }
  }
  if (vps.empty() || sps.empty() || pps.empty()) {
    return {};
  }

  uint8_t profileSpace = 0;
  uint8_t tierFlag = 0;
  uint8_t profileIdc = 1;
  uint8_t levelIdc = 120;
  uint32_t compatibility = 0;
  uint64_t constraints = 0;
  if (sps[0].size() >= 15) {
    // The profile_tier_level of the SPS follows its two byte NAL header and the one byte of
    // sps_video_parameter_set_id / sps_max_sub_layers_minus1 / sps_temporal_id_nesting_flag.
    const uint8_t* ptl = sps[0].data() + 3;
    profileSpace = (ptl[0] >> 6) & 0x03;
    tierFlag = (ptl[0] >> 5) & 0x01;
    profileIdc = ptl[0] & 0x1F;
    compatibility = readBe32(ptl + 1);
    for (int i = 0; i < 6; ++i) {
      constraints = (constraints << 8) | ptl[5 + i];
    }
    levelIdc = ptl[11];
  }

  std::vector<uint8_t> record;
  record.push_back(1); // configurationVersion
  record.push_back(static_cast<uint8_t>((profileSpace << 6) | (tierFlag << 5) | profileIdc));
  appendBe32(record, compatibility);
  for (int i = 5; i >= 0; --i) {
    record.push_back(static_cast<uint8_t>((constraints >> (i * 8)) & 0xFF));
  }
  record.push_back(levelIdc);
  appendBe16(record, 0xF000); // min_spatial_segmentation_idc, unknown
  record.push_back(0xFC); // parallelismType
  record.push_back(0xFD); // chromaFormat = 1 (4:2:0), reserved bits set
  record.push_back(0xF8); // bitDepthLumaMinus8 = 0
  record.push_back(0xF8); // bitDepthChromaMinus8 = 0
  appendBe16(record, 0); // avgFrameRate
  record.push_back(static_cast<uint8_t>((1 << 3) | (1 << 2) | 0x03)); // temporal layers + 4 byte lengths

  const std::vector<std::pair<uint8_t, std::vector<std::vector<uint8_t>>>> arrays = {
    { 32, vps },
    { 33, sps },
    { 34, pps },
    { 39, prefixSei },
    { 40, suffixSei },
  };
  size_t arrayCount = 0;
  for (const auto& [type, units] : arrays) {
    static_cast<void>(type);
    if (!units.empty()) {
      ++arrayCount;
    }
  }
  record.push_back(static_cast<uint8_t>(arrayCount));
  for (const auto& [type, units] : arrays) {
    if (units.empty()) {
      continue;
    }
    record.push_back(static_cast<uint8_t>(0x80 | type)); // array_completeness
    appendBe16(record, units.size());
    for (const auto& unit : units) {
      appendBe16(record, unit.size());
      record.insert(record.end(), unit.begin(), unit.end());
    }
  }
  return record;
}

std::vector<uint8_t> ascFromEsds(const std::vector<uint8_t>& esds) {
  if (esds.empty() || esds[0] != 0x03) {
    return {};
  }
  const uint8_t* cursor = esds.data();
  const uint8_t* end = esds.data() + esds.size();

  // The ES descriptor header.
  if (cursor >= end || *cursor++ != 0x03) {
    return {};
  }
  size_t length = 0;
  if (!readDescriptorLength(cursor, end, length)) {
    return {};
  }
  size_t remaining = std::min(length, static_cast<size_t>(end - cursor));
  // ES_ID and the flags byte precede the decoder configuration descriptor.
  if (remaining < 3) {
    return {};
  }
  cursor += 3;
  remaining -= 3;

  while (remaining >= 2) {
    const uint8_t tag = *cursor++;
    --remaining;
    size_t descriptorLength = 0;
    if (!readDescriptorLength(cursor, end, descriptorLength)) {
      return {};
    }
    const size_t payloadAvailable = std::min(descriptorLength,
      static_cast<size_t>(end - cursor));
    if (tag == 0x04) {
      // The decoder configuration descriptor: objectTypeIndication, streamType, buffer size,
      // maxBitrate, avgBitrate, then the decoder specific info descriptor.
      const uint8_t* inner = cursor;
      const uint8_t* innerEnd = cursor + payloadAvailable;
      if (payloadAvailable < 13) {
        return {};
      }
      inner += 13;
      while (inner + 1 < innerEnd) {
        const uint8_t innerTag = *inner++;
        size_t innerLength = 0;
        if (!readDescriptorLength(inner, innerEnd, innerLength)) {
          return {};
        }
        const size_t available = std::min(innerLength,
          static_cast<size_t>(innerEnd - inner));
        if (innerTag == 0x05) {
          return std::vector<uint8_t>(inner, inner + available);
        }
        inner += available;
      }
      return {};
    }
    cursor += payloadAvailable;
    remaining -= std::min(remaining, payloadAvailable);
  }
  return {};
}

std::vector<uint8_t> esdsFromAsc(const std::vector<uint8_t>& asc, const int bitrate,
    const int channelCount) {
  if (asc.empty()) {
    return {};
  }
  const auto rate = bitrate > 0 ? static_cast<uint32_t>(bitrate) : kDefaultAudioBitrate;
  (void) channelCount;

  // The descriptors are built from the inside out, because each length covers its payload.
  std::vector<uint8_t> decoderSpecific;
  decoderSpecific.push_back(0x05);
  appendDescriptorLength(decoderSpecific, asc.size());
  decoderSpecific.insert(decoderSpecific.end(), asc.begin(), asc.end());

  std::vector<uint8_t> decoderConfig;
  decoderConfig.push_back(0x04);
  appendDescriptorLength(decoderConfig, 13 + decoderSpecific.size());
  decoderConfig.push_back(0x40); // objectTypeIndication: MPEG-4 audio
  decoderConfig.push_back(0x14); // streamType: audio, no upstream
  decoderConfig.insert(decoderConfig.end(), { 0x00, 0x18, 0x00 }); // bufferSizeDB
  appendBe32(decoderConfig, rate); // maxBitrate
  appendBe32(decoderConfig, rate); // avgBitrate
  decoderConfig.insert(decoderConfig.end(), decoderSpecific.begin(), decoderSpecific.end());

  std::vector<uint8_t> slConfig;
  slConfig.push_back(0x06);
  appendDescriptorLength(slConfig, 1);
  slConfig.push_back(0x02);

  std::vector<uint8_t> es;
  es.push_back(0x03);
  appendDescriptorLength(es, 3 + decoderConfig.size() + slConfig.size());
  es.insert(es.end(), { 0x00, 0x00, 0x00 }); // ES_ID and flags
  es.insert(es.end(), decoderConfig.begin(), decoderConfig.end());
  es.insert(es.end(), slConfig.begin(), slConfig.end());
  return es;
}

namespace {

[[nodiscard]] std::vector<uint8_t> firstCodecSpecificData(const MediaFormat& format) {
  const auto& csdList = format.csdList();
  if (csdList.empty() || csdList[0].empty()) {
    return {};
  }
  return csdList[0];
}

} // namespace anonymous

std::vector<uint8_t> audioDecoderMagicCookie(const MediaFormat& format) {
  const auto cookie = firstCodecSpecificData(format);
  if (format.getAudioFormatType() != AudioFormatType::kAudioFormatTypeMpeg4AAC) {
    return cookie;
  }
  if (cookie.empty() || cookie[0] == 0x03) {
    return cookie;
  }
  return esdsFromAsc(cookie, format.getAverageBitrate() > 0 ? format.getAverageBitrate()
                                                            : format.getBitrate(),
    format.getChannelCount());
}

std::vector<uint8_t> audioFormatDescriptionCookie(const MediaFormat& format) {
  return audioDecoderMagicCookie(format);
}

// ==================================================================================================
// MediaFormat <-> CMFormatDescription
// ==================================================================================================

std::shared_ptr<MediaFormat> createMediaFormat(CMFormatDescriptionRef description,
    const MediaType mediaType, const std::string& containerMime, const int trackId) {
  if (description == nullptr) {
    return nullptr;
  }

  if (mediaType == MediaType::kMediaTypeVideo ||
      CMFormatDescriptionGetMediaType(description) == kCMMediaType_Video) {
    const auto codecType = toVideoCodecType(CMFormatDescriptionGetMediaSubType(description));
    const auto mimeType = mimeTypeOfVideoCodec(codecType);
    if (codecType == VideoCodecType::kVideoCodecTypeInvalid || mimeType.empty()) {
      return nullptr;
    }
    const auto dimensions = CMVideoFormatDescriptionGetDimensions(
      reinterpret_cast<CMVideoFormatDescriptionRef>(description));
    auto format = MediaFormat::CreateVideoFormat(mimeType, dimensions.width, dimensions.height);
    format->setVideoCodecType(codecType);
    format->setPixelFormat(PixelFormat::kFormatI420);
    format->setContainerMimeType(containerMime);
    format->setTrackId(trackId);
    format->setColorInfo(colorInfoOf(description));

    if (codecType == VideoCodecType::kVideoCodecTypeH264) {
      size_t count = 0;
      int nalLengthSize = kDefaultNalLengthSize;
      const uint8_t* parameterSet = nullptr;
      size_t parameterSetSize = 0;
      if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(description, 0, &parameterSet,
          &parameterSetSize, &count, &nalLengthSize) == noErr) {
        for (size_t i = 0; i < count; ++i) {
          if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(description,
              static_cast<int>(i), &parameterSet, &parameterSetSize, nullptr, nullptr) != noErr) {
            break;
          }
          if (parameterSet == nullptr || parameterSetSize == 0) {
            continue;
          }
          std::vector<uint8_t> entry;
          entry.push_back(static_cast<uint8_t>(parameterSetSize & 0xFF));
          entry.insert(entry.end(), parameterSet, parameterSet + parameterSetSize);
          format->setCodecSpecificData(static_cast<int>(i), entry);
        }
      }
      const auto& csdList = format->csdList();
      if (!csdList.empty() && csdList[0].size() >= 5) {
        // The entry is the SPS prefixed by its one byte length, so the NAL unit starts at index 1:
        // its second byte is profile_idc and its fourth is level_idc.
        format->setProfile(csdList[0][2]);
        format->setLevel(csdList[0][4]);
      }
    } else if (codecType == VideoCodecType::kVideoCodecTypeHevc) {
      const auto* atoms = static_cast<CFDictionaryRef>(CMFormatDescriptionGetExtension(
        description, kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms));
      const auto hvcC = atoms != nullptr
                          ? static_cast<CFDataRef>(CFDictionaryGetValue(atoms, CFSTR("hvcC")))
                          : nullptr;
      if (hvcC != nullptr) {
        const auto* bytes = CFDataGetBytePtr(hvcC);
        const auto size = CFDataGetLength(hvcC);
        if (bytes != nullptr && size > 0) {
          format->setCodecSpecificData(0, bytes, static_cast<size_t>(size));
        }
      } else {
        std::vector<std::vector<uint8_t>> parameterSets;
        const uint8_t* parameterSet = nullptr;
        size_t parameterSetSize = 0;
        size_t count = 0;
        if (CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(description, 0, &parameterSet,
            &parameterSetSize, &count, nullptr) == noErr) {
          for (size_t i = 0; i < count; ++i) {
            if (CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(description,
                static_cast<int>(i), &parameterSet, &parameterSetSize, nullptr, nullptr) !=
                noErr) {
              break;
            }
            if (parameterSet != nullptr && parameterSetSize > 0) {
              parameterSets.emplace_back(parameterSet, parameterSet + parameterSetSize);
            }
          }
        }
        const auto record = hvcCFromParameterSets(parameterSets);
        if (!record.empty()) {
          format->setCodecSpecificData(0, record);
        }
      }
      const auto& csdList = format->csdList();
      if (!csdList.empty() && csdList[0].size() > 12 && csdList[0][0] == 1) {
        // general_profile_idc and general_level_idc of the hvcC record describe the stream.
        format->setProfile(csdList[0][1] & 0x1F);
        format->setLevel(csdList[0][12]);
      }
    }
    return format;
  }

  if (mediaType == MediaType::kMediaTypeAudio ||
      CMFormatDescriptionGetMediaType(description) == kCMMediaType_Audio) {
    const auto* descriptionFormat = CMAudioFormatDescriptionGetStreamBasicDescription(
      reinterpret_cast<CMAudioFormatDescriptionRef>(description));
    if (descriptionFormat == nullptr) {
      return nullptr;
    }
    const auto formatType = toAudioFormatType(descriptionFormat->mFormatID);
    const auto mimeType = mimeTypeOfAudioFormat(formatType);
    if (formatType == AudioFormatType::kAudioFormatTypeInvalid || mimeType.empty()) {
      return nullptr;
    }
    const auto sampleRate = static_cast<int>(descriptionFormat->mSampleRate);
    const auto channelCount = static_cast<int>(descriptionFormat->mChannelsPerFrame);
    if (sampleRate <= 0 || channelCount <= 0) {
      return nullptr;
    }
    auto format = MediaFormat::CreateAudioFormat(mimeType, sampleRate, channelCount);
    format->setAudioFormatType(formatType);
    if (formatType == AudioFormatType::kAudioFormatTypeLinearPCM) {
      // An uncompressed track is decoded by the PCM decoder, which reads the sample format of the
      // track; a compressed one is decoded by the converter and has no PCM sample format.
      auto sampleFormat = sampleFormatOf(*descriptionFormat);
      format->setSampleFormat(sampleFormat == AudioSampleFormat::kInvalid
                                ? AudioSampleFormat::kInt16
                                : sampleFormat);
    } else {
      format->setSampleFormat(AudioSampleFormat::kInvalid);
    }
    format->setContainerMimeType(containerMime);
    format->setTrackId(trackId);

    size_t cookieSize = 0;
    const void* cookie = CMAudioFormatDescriptionGetMagicCookie(
      reinterpret_cast<CMAudioFormatDescriptionRef>(description), &cookieSize);
    if (cookie != nullptr && cookieSize > 0) {
      const auto* bytes = static_cast<const uint8_t*>(cookie);
      if (formatType == AudioFormatType::kAudioFormatTypeMpeg4AAC) {
        const auto asc = ascFromEsds(std::vector<uint8_t>(bytes, bytes + cookieSize));
        if (!asc.empty()) {
          format->setCodecSpecificData(0, asc);
        } else {
          format->setCodecSpecificData(0, bytes, cookieSize);
        }
      } else {
        format->setCodecSpecificData(0, bytes, cookieSize);
      }
    }
    return format;
  }
  return nullptr;
}

CMFormatDescriptionRef createAudioFormatDescription(const MediaFormat& format) {
  const auto formatType = format.getAudioFormatType();
  const auto formatID = toAudioFormatID(formatType);
  if (formatID == 0) {
    logFailure("the audio codec type has no platform identifier",
      convertToText(formatType));
    return nullptr;
  }
  auto asbd = makeCompressedFormat(formatType, format.getSampleRate(), format.getChannelCount());
  const auto cookie = audioFormatDescriptionCookie(format);
  CMAudioFormatDescriptionRef description = nullptr;
  const auto status = CMAudioFormatDescriptionCreate(kCFAllocatorDefault, &asbd, 0, nullptr,
    cookie.size(), cookie.empty() ? nullptr : cookie.data(), nullptr, &description);
  if (status != noErr) {
    logStatus(status, "CMAudioFormatDescriptionCreate failed", format.toString());
    return nullptr;
  }
  return description;
}

CMFormatDescriptionRef createVideoFormatDescription(const MediaFormat& format) {
  auto codecType = format.getVideoCodecType();
  if (codecType == VideoCodecType::kVideoCodecTypeInvalid) {
    codecType = MediaUtils::convertToVideoCodecType(format.getMimeType());
  }
  const auto platformType = toCMVideoCodecType(codecType);
  const auto width = format.getWidth() > 0 ? format.getWidth() : 0;
  const auto height = format.getHeight() > 0 ? format.getHeight() : 0;
  if (platformType == 0) {
    logFailure("the video codec type has no platform identifier", format.toString());
    return nullptr;
  }

  CFMutableDictionaryRef extensions = createVideoExtensions(format.getColorInfo());
  CMVideoFormatDescriptionRef description = nullptr;
  OSStatus status = noErr;
  if (codecType == VideoCodecType::kVideoCodecTypeH264) {
    const auto parameterSets = h264ParameterSets(format);
    if (!parameterSets.empty()) {
      // `CMVideoFormatDescriptionCreateFromH264ParameterSets` has no extensions parameter, so the
      // color information cannot be attached through it. The description is built by the generic
      // creation function instead, with the parameter sets in the sample description extension
      // atoms (`avcC`) and the color information beside them: the decoders and the muxer read the
      // parameter sets from the atoms exactly as they read them from the specialized description.
      const auto avcC = avcCFromParameterSets(parameterSets);
      if (!avcC.empty()) {
        CFMutableDictionaryRef atoms = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
          &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFDataRef avcCData = CFDataCreate(kCFAllocatorDefault, avcC.data(),
          static_cast<CFIndex>(avcC.size()));
        if (atoms != nullptr && avcCData != nullptr) {
          CFDictionarySetValue(atoms, CFSTR("avcC"), avcCData);
          if (extensions == nullptr) {
            extensions = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
              &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
          }
          if (extensions != nullptr) {
            CFDictionarySetValue(extensions,
              kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms, atoms);
          }
        }
        if (avcCData != nullptr) {
          CFRelease(avcCData);
        }
        if (atoms != nullptr) {
          CFRelease(atoms);
        }
        if (extensions != nullptr) {
          status = CMVideoFormatDescriptionCreate(kCFAllocatorDefault, platformType, width, height,
            extensions, &description);
        }
      }
      if (status != noErr || description == nullptr) {
        // The generic creation function could not describe the parameter sets: the specialized one
        // still produces a decodable description, at the cost of the color information.
        if (description != nullptr) {
          CFRelease(description);
          description = nullptr;
        }
        std::vector<const uint8_t*> pointers;
        std::vector<size_t> sizes;
        pointers.reserve(parameterSets.size());
        sizes.reserve(parameterSets.size());
        for (const auto& parameterSet : parameterSets) {
          pointers.push_back(parameterSet.data());
          sizes.push_back(parameterSet.size());
        }
        status = CMVideoFormatDescriptionCreateFromH264ParameterSets(kCFAllocatorDefault,
          pointers.size(), pointers.data(), sizes.data(), kDefaultNalLengthSize, &description);
      }
    } else {
      status = CMVideoFormatDescriptionCreate(kCFAllocatorDefault, platformType, width, height,
        extensions, &description);
    }
  } else if (codecType == VideoCodecType::kVideoCodecTypeHevc ||
             codecType == VideoCodecType::kVideoCodecTypeDolbyVision) {
    const auto parameterSets = hevcParameterSets(format);
    if (!parameterSets.empty()) {
      std::vector<const uint8_t*> pointers;
      std::vector<size_t> sizes;
      pointers.reserve(parameterSets.size());
      sizes.reserve(parameterSets.size());
      for (const auto& parameterSet : parameterSets) {
        pointers.push_back(parameterSet.data());
        sizes.push_back(parameterSet.size());
      }
      status = CMVideoFormatDescriptionCreateFromHEVCParameterSets(kCFAllocatorDefault,
        pointers.size(), pointers.data(), sizes.data(), kDefaultNalLengthSize, extensions,
        &description);
    } else {
      status = CMVideoFormatDescriptionCreate(kCFAllocatorDefault, platformType, width, height,
        extensions, &description);
    }
  } else {
    status = CMVideoFormatDescriptionCreate(kCFAllocatorDefault, platformType, width, height,
      extensions, &description);
  }

  if (extensions != nullptr) {
    CFRelease(extensions);
  }
  if (status != noErr || description == nullptr) {
    logStatus(status, "the video format description could not be created", format.toString());
    return nullptr;
  }
  return description;
}

CMFormatDescriptionRef createFormatDescription(const std::shared_ptr<MediaFormat>& format) {
  if (format == nullptr) {
    return nullptr;
  }
  if (format->isAudio()) {
    return createAudioFormatDescription(*format);
  }
  if (format->isVideo()) {
    return createVideoFormatDescription(*format);
  }
  return nullptr;
}

// ==================================================================================================
// Bitstream helpers
// ==================================================================================================

bool isAnnexB(const uint8_t* data, const size_t size) {
  if (data == nullptr || size < 3) {
    return false;
  }
  if (data[0] == 0 && data[1] == 0 && data[2] == 1) {
    return true;
  }
  return size >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1;
}

namespace {

/**
 * Whether the whole buffer is a sequence of length prefixed NAL units.
 *
 * The length of a NAL unit must be valid for every unit and the units must cover the buffer exactly.
 * This is what tells a length prefixed sample apart from an Annex B byte stream: the first bytes of
 * a small length prefixed frame (`00 00 01 xx`, a NAL unit of 256 to 511 bytes) are byte-identical
 * to a three byte start code, so looking at the first bytes alone would corrupt such a frame. The
 * whole buffer is examined instead, and the caller prefers this form; an Annex B sample practically
 * never describes itself as a valid sequence of length prefixed units.
 */
[[nodiscard]] bool isLengthPrefixed(const uint8_t* data, const size_t size,
    const int lengthSize) {
  if (data == nullptr || size == 0 || lengthSize <= 0 || lengthSize > 4) {
    return false;
  }
  size_t offset = 0;
  int units = 0;
  while (offset + static_cast<size_t>(lengthSize) <= size) {
    uint32_t length = 0;
    for (int i = 0; i < lengthSize; ++i) {
      length = (length << 8) | data[offset + i];
    }
    offset += lengthSize;
    if (length == 0 || offset + length > size) {
      return false;
    }
    offset += length;
    ++units;
  }
  return offset == size && units > 0;
}

} // namespace anonymous

std::vector<uint8_t> toLengthPrefixed(const uint8_t* data, const size_t size,
    const int nalLengthSize) {
  if (data == nullptr || size == 0) {
    return {};
  }
  std::vector<uint8_t> output;
  output.reserve(size + 16);

  const int lengthSize = nalLengthSize > 0 && nalLengthSize <= 4 ? nalLengthSize
                                                                  : kDefaultNalLengthSize;
  // The length prefixed form is checked first: an AVCC frame whose first NAL unit is 256 to 511
  // bytes long begins with the bytes of a three byte start code.
  if (isLengthPrefixed(data, size, lengthSize)) {
    if (lengthSize == 4) {
      return std::vector<uint8_t>(data, data + size);
    }
    size_t offset = 0;
    while (offset < size) {
      uint32_t length = 0;
      for (int i = 0; i < lengthSize; ++i) {
        length = (length << 8) | data[offset + i];
      }
      offset += lengthSize;
      appendBe32(output, length);
      output.insert(output.end(), data + offset, data + offset + length);
      offset += length;
    }
    return output;
  }

  if (isAnnexB(data, size)) {
    std::vector<std::vector<uint8_t>> nals;
    appendAnnexBNals(data, size, nals);
    for (const auto& nal : nals) {
      if (nal.empty()) {
        continue;
      }
      appendBe32(output, static_cast<uint32_t>(nal.size()));
      output.insert(output.end(), nal.begin(), nal.end());
    }
    return output;
  }
  return {};
}

std::vector<uint8_t> toAnnexB(const uint8_t* data, const size_t size,
    const int nalLengthSize) {
  if (data == nullptr || size == 0) {
    return {};
  }
  const int lengthSize = nalLengthSize > 0 && nalLengthSize <= 4 ? nalLengthSize
                                                                  : kDefaultNalLengthSize;
  // The length prefixed form wins over the start code test for the same reason as in
  // toLengthPrefixed: a 256 to 511 byte NAL unit begins with the bytes of a start code.
  if (isLengthPrefixed(data, size, lengthSize)) {
    std::vector<uint8_t> output;
    output.reserve(size + 16);
    size_t offset = 0;
    while (offset < size) {
      uint32_t length = 0;
      for (int i = 0; i < lengthSize; ++i) {
        length = (length << 8) | data[offset + i];
      }
      offset += lengthSize;
      output.insert(output.end(), { 0x00, 0x00, 0x00, 0x01 });
      output.insert(output.end(), data + offset, data + offset + length);
      offset += length;
    }
    return output;
  }
  if (isAnnexB(data, size)) {
    return std::vector<uint8_t>(data, data + size);
  }
  return {};
}

int nalLengthSizeOf(CMVideoFormatDescriptionRef description) {
  if (description == nullptr) {
    return kDefaultNalLengthSize;
  }
  int nalLengthSize = 0;
  size_t count = 0;
  if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(description, 0, nullptr, nullptr, &count,
      &nalLengthSize) == noErr && nalLengthSize > 0 && nalLengthSize <= 4) {
    return nalLengthSize;
  }
  nalLengthSize = 0;
  if (CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(description, 0, nullptr, nullptr, &count,
      &nalLengthSize) == noErr && nalLengthSize > 0 && nalLengthSize <= 4) {
    return nalLengthSize;
  }
  return kDefaultNalLengthSize;
}

// ==================================================================================================
// Misc
// ==================================================================================================

int64_t toTimeUs(const CMTime time) {
  if (!CMTIME_IS_VALID(time)) {
    return Definitions::kTimeUnset;
  }
  const auto converted = CMTimeConvertScale(time, Definitions::kMicrosPerSecond,
    kCMTimeRoundingMethod_RoundHalfAwayFromZero);
  if (!CMTIME_IS_VALID(converted)) {
    return Definitions::kTimeUnset;
  }
  return converted.value;
}

CMTime toCMTime(const int64_t timeUs) {
  return CMTimeMake(timeUs, Definitions::kMicrosPerSecond);
}

std::string formatDescriptionSummary(CMFormatDescriptionRef description) {
  if (description == nullptr) {
    return "none";
  }
  const auto mediaType = CMFormatDescriptionGetMediaType(description);
  const auto subType = CMFormatDescriptionGetMediaSubType(description);
  const auto fourCC = [](const uint32_t value) {
    const char chars[5] = {
      static_cast<char>((value >> 24) & 0xFF),
      static_cast<char>((value >> 16) & 0xFF),
      static_cast<char>((value >> 8) & 0xFF),
      static_cast<char>(value & 0xFF),
      '\0'
    };
    return std::string(chars);
  };
  if (mediaType == kCMMediaType_Video) {
    const auto dimensions = CMVideoFormatDescriptionGetDimensions(
      reinterpret_cast<CMVideoFormatDescriptionRef>(description));
    return fourCC(mediaType) + "/" + fourCC(subType) + " " + std::to_string(dimensions.width) +
           "x" + std::to_string(dimensions.height);
  }
  if (mediaType == kCMMediaType_Audio) {
    const auto* asbd = CMAudioFormatDescriptionGetStreamBasicDescription(
      reinterpret_cast<CMAudioFormatDescriptionRef>(description));
    if (asbd != nullptr) {
      return fourCC(mediaType) + "/" + fourCC(subType) + " " +
             std::to_string(static_cast<int>(asbd->mSampleRate)) + "Hz " +
             std::to_string(asbd->mChannelsPerFrame) + "ch";
    }
  }
  return fourCC(mediaType) + "/" + fourCC(subType);
}

ColorInfo colorInfoOf(CMFormatDescriptionRef description) {
  ColorInfo colorInfo;
  if (description == nullptr) {
    return colorInfo;
  }

  if (equalExtension(description, kCMFormatDescriptionExtension_ColorPrimaries,
      kCMFormatDescriptionColorPrimaries_ITU_R_709_2)) {
    colorInfo.colorSpace = ColorInfo::SpaceType::kSpaceBT709;
  } else if (equalExtension(description, kCMFormatDescriptionExtension_ColorPrimaries,
      kCMFormatDescriptionColorPrimaries_ITU_R_2020)) {
    colorInfo.colorSpace = ColorInfo::SpaceType::kSpaceBT2020;
  } else if (equalExtension(description, kCMFormatDescriptionExtension_ColorPrimaries,
      kCMFormatDescriptionColorPrimaries_SMPTE_C)) {
    colorInfo.colorSpace = ColorInfo::SpaceType::kSpaceBT601;
  }

  if (colorInfo.colorSpace == ColorInfo::SpaceType::kSpaceUnknown) {
    if (equalExtension(description, kCMFormatDescriptionExtension_YCbCrMatrix,
        kCMFormatDescriptionYCbCrMatrix_ITU_R_709_2)) {
      colorInfo.colorSpace = ColorInfo::SpaceType::kSpaceBT709;
    } else if (equalExtension(description, kCMFormatDescriptionExtension_YCbCrMatrix,
        kCMFormatDescriptionYCbCrMatrix_ITU_R_601_4)) {
      colorInfo.colorSpace = ColorInfo::SpaceType::kSpaceBT601;
    } else if (equalExtension(description, kCMFormatDescriptionExtension_YCbCrMatrix,
        kCMFormatDescriptionYCbCrMatrix_ITU_R_2020)) {
      colorInfo.colorSpace = ColorInfo::SpaceType::kSpaceBT2020;
    }
  }

  if (equalExtension(description, kCMFormatDescriptionExtension_TransferFunction,
      kCMFormatDescriptionTransferFunction_SMPTE_ST_2084_PQ)) {
    colorInfo.colorTransfer = ColorInfo::TransferType::kTransferST2084;
  } else if (equalExtension(description, kCMFormatDescriptionExtension_TransferFunction,
      kCMFormatDescriptionTransferFunction_ITU_R_2100_HLG)) {
    colorInfo.colorTransfer = ColorInfo::TransferType::kTransferHLG;
  } else if (equalExtension(description, kCMFormatDescriptionExtension_TransferFunction,
      kCMFormatDescriptionTransferFunction_sRGB)) {
    colorInfo.colorTransfer = ColorInfo::TransferType::kTransferSRGB;
  } else if (equalExtension(description, kCMFormatDescriptionExtension_TransferFunction,
      kCMFormatDescriptionTransferFunction_Linear)) {
    colorInfo.colorTransfer = ColorInfo::TransferType::kTransferLinear;
  } else if (equalExtension(description, kCMFormatDescriptionExtension_TransferFunction,
      kCMFormatDescriptionTransferFunction_UseGamma)) {
    colorInfo.colorTransfer = ColorInfo::TransferType::kTransferGamma22;
  } else if (equalExtension(description, kCMFormatDescriptionExtension_TransferFunction,
      kCMFormatDescriptionTransferFunction_ITU_R_709_2)) {
    colorInfo.colorTransfer = ColorInfo::TransferType::kTransferSDR;
  }

  const auto* fullRange = static_cast<CFBooleanRef>(CMFormatDescriptionGetExtension(description,
    kCMFormatDescriptionExtension_FullRangeVideo));
  if (fullRange != nullptr && CFGetTypeID(fullRange) == CFBooleanGetTypeID()) {
    colorInfo.colorRange = CFBooleanGetValue(fullRange) ? ColorInfo::RangeType::kRangeFull
                                                         : ColorInfo::RangeType::kRangeLimited;
  }
  return colorInfo;
}

} // namespace coral::darwin

#endif // defined(__APPLE__)
