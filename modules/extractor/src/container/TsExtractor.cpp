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

#include <extractor/container/TsExtractor.h>

#include <extractor/Utility.h>
#include <extractor/audio/AdtsReader.h>
#include <extractor/audio/MpegAudio.h>
#include <extractor/text/SubtitleParser.h>
#include <extractor/text/cea708/Cea708Sei.h>

#include <media/Types.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace coral {

namespace {

/** The sync byte of a transport packet. */
constexpr uint8_t kSyncByte = 0x47;

/** The PID of the program association table. */
constexpr int kPatPid = 0;

/** The largest PID that a transport stream can use. */
constexpr int kMaxPid = 0x1FFF;

/** The maximum size of a PES packet that is buffered, in bytes. */
constexpr size_t kMaxPesSize = 64U * 1024U * 1024U;

/** The number of bytes of the read buffer. */
constexpr size_t kReadBufferSize = 64U * 1024U;

/** The maximum number of bytes that the transport packet buffer holds. */
constexpr size_t kMaxBufferSize = 512U * 1024U;

/** The duration of the timestamp wrap around, in microseconds. */
constexpr int64_t kPtsWrapUs = (1LL << 33) * 1000000 / 90000;

/** The elementary stream types that the extractor understands. */
constexpr int kStreamTypeMpeg1Video = 0x01;
constexpr int kStreamTypeMpeg2Video = 0x02;
constexpr int kStreamTypeMpegAudio = 0x03;
constexpr int kStreamTypeMpegAudioLsf = 0x04;
constexpr int kStreamTypeAacAdts = 0x0F;
constexpr int kStreamTypeH264 = 0x1B;
constexpr int kStreamTypeH265 = 0x24;
constexpr int kStreamTypeAc3 = 0x81;
constexpr int kStreamTypeEac3 = 0x87;
/** A SCTE-35 splice information section carried as its own PES stream. */
constexpr int kStreamTypeScte35 = 0x86;

/** The identifiers of the registration descriptors that override the stream type. */
constexpr uint32_t kFormatIdentifierAc3 = 0x41432D33; // "AC-3"
constexpr uint32_t kFormatIdentifierEac3 = 0x45414333; // "EAC3"
constexpr uint32_t kFormatIdentifierHevc = 0x48455643; // "HEVC"

/** The sample rates of an AAC stream, indexed by the sampling frequency index. */
constexpr int kAacSampleRates[] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000,
                                    7350 };

/** Reads an unsigned 16 bit big endian value. */
uint16_t readUInt16(const uint8_t* data) {
  return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);
}

/** Reads an unsigned 32 bit big endian value. */
uint32_t readUInt32(const uint8_t* data) {
  return (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) |
         (static_cast<uint32_t>(data[2]) << 8) | data[3];
}

/** Returns whether the given stream type is one that the extractor creates a track for. */
bool isSupportedStreamType(const int streamType) {
  switch (streamType) {
    case kStreamTypeMpeg1Video:
    case kStreamTypeMpeg2Video:
    case kStreamTypeH264:
    case kStreamTypeH265:
    case kStreamTypeAacAdts:
    case kStreamTypeMpegAudio:
    case kStreamTypeMpegAudioLsf:
    case kStreamTypeScte35: {
      return true;
    }
    default: {
      return false;
    }
  }
}

/** Returns the media type of a supported stream type. */
MediaType mediaTypeForStreamType(const int streamType) {
  switch (streamType) {
    case kStreamTypeMpeg1Video:
    case kStreamTypeMpeg2Video:
    case kStreamTypeH264:
    case kStreamTypeH265: {
      return MediaType::kMediaTypeVideo;
    }
    case kStreamTypeAacAdts:
    case kStreamTypeMpegAudio:
    case kStreamTypeMpegAudioLsf: {
      return MediaType::kMediaTypeAudio;
    }
    case kStreamTypeScte35: {
      return MediaType::kMediaTypeMetadata;
    }
    default: {
      return MediaType::kMediaTypeInvalid;
    }
  }
}

/**
 * Returns the stream type of an elementary stream, taking the registration descriptors of the
 * program map table into account.
 */
int resolveStreamType(const int declaredType, const uint8_t* descriptors, const size_t size) {
  int streamType = declaredType;
  size_t offset = 0;
  while (offset + 2 <= size) {
    const int tag = descriptors[offset];
    const int length = descriptors[offset + 1];
    const size_t next = offset + 2 + static_cast<size_t>(length);
    if (next > size) {
      break;
    }
    if (tag == 0x05 && length >= 4) {
      const uint32_t identifier = readUInt32(descriptors + offset + 2);
      if (identifier == kFormatIdentifierAc3) {
        streamType = kStreamTypeAc3;
      } else if (identifier == kFormatIdentifierEac3) {
        streamType = kStreamTypeEac3;
      } else if (identifier == kFormatIdentifierHevc) {
        streamType = kStreamTypeH265;
      }
    } else if (tag == 0x6A) {
      streamType = kStreamTypeAc3;
    } else if (tag == 0x7A) {
      streamType = kStreamTypeEac3;
    } else if (tag == 0x7F && length >= 1) {
      const int extensionTag = descriptors[offset + 2];
      if (extensionTag == 0x15) {
        // AC-4, which this extractor does not expose.
        streamType = 0xAC;
      }
    }
    offset = next;
  }
  return streamType;
}

/**
 * Builds the two bytes of the AudioSpecificConfig of an AAC stream.
 */
uint32_t buildAudioSpecificConfig(const int audioObjectType, const int samplingFrequencyIndex,
  const int channelConfig) {
  return (static_cast<uint32_t>(audioObjectType) << 11) | (static_cast<uint32_t>(samplingFrequencyIndex) << 7) |
         (static_cast<uint32_t>(channelConfig) << 3);
}

/** Reads the bit count of an unsigned Exp-Golomb value. */
int readUe(AVBitReader& bits) {
  int leadingZeroBits = 0;
  while (leadingZeroBits < 31 && bits.canReadBits(leadingZeroBits + 1) && bits.readBits(1) == 0) {
    ++leadingZeroBits;
  }
  if (leadingZeroBits == 0) {
    return 0;
  }
  if (leadingZeroBits >= 31 || !bits.canReadBits(leadingZeroBits)) {
    return 0;
  }
  return static_cast<int>(bits.readBits(leadingZeroBits)) + ((1 << leadingZeroBits) - 1);
}

/** Reads the bit count of a signed Exp-Golomb value. */
int readSe(AVBitReader& bits) {
  const int value = readUe(bits);
  return (value & 1) != 0 ? (value + 1) / 2 : -(value / 2);
}

/** Reads {@code count} bits, or zero when they are not available. */
int readBits(AVBitReader& bits, const int count) {
  if (count <= 0 || !bits.canReadBits(count)) {
    return 0;
  }
  return static_cast<int>(bits.readBits(count));
}

void skipAvcScalingList(AVBitReader& reader, const int size) {
  int lastScale = 8;
  int nextScale = 8;
  for (int i = 0; i < size; ++i) {
    if (nextScale != 0) {
      const int deltaScale = readSe(reader);
      nextScale = (lastScale + deltaScale + 256) % 256;
    }
    lastScale = nextScale == 0 ? lastScale : nextScale;
  }
}

/**
 * Parses the SPS of an H.264 stream and returns its dimensions and the fields that identify the
 * codec.
 */
bool parseAvcSps(const std::vector<uint8_t>& sps, int& outWidth, int& outHeight, int& outProfile, int& outCompatibility,
  int& outLevel) {
  if (sps.size() < 4) {
    return false;
  }
  outProfile = sps[1];
  outCompatibility = sps[2];
  outLevel = sps[3];
  auto buffer = std::make_shared<AVDataBuffer>(sps);
  AVBitReader reader(buffer);
  reader.skipBits(32);
  int chromaFormatIdc = 1;
  const int profileIdc = sps[1];
  const bool highProfile = profileIdc == 100 || profileIdc == 110 || profileIdc == 122 ||
                           profileIdc == 244 || profileIdc == 44 || profileIdc == 83 ||
                           profileIdc == 86 || profileIdc == 118 || profileIdc == 128 ||
                           profileIdc == 138 || profileIdc == 139 || profileIdc == 134 ||
                           profileIdc == 135;
  if (highProfile) {
    chromaFormatIdc = readUe(reader);
    if (chromaFormatIdc == 3) {
      reader.skipBits(1);
    }
    static_cast<void>(readUe(reader)); // bit_depth_luma_minus8
    static_cast<void>(readUe(reader)); // bit_depth_chroma_minus8
    reader.skipBits(1); // qpprime_y_zero_transform_bypass_flag
    if (readBits(reader, 1) != 0) {
      const int scalingListCount = chromaFormatIdc != 3 ? 8 : 12;
      for (int i = 0; i < scalingListCount; ++i) {
        if (readBits(reader, 1) != 0) {
          skipAvcScalingList(reader, i < 6 ? 16 : 64);
        }
      }
    }
  }
  static_cast<void>(readUe(reader)); // log2_max_frame_num_minus4
  const int picOrderCountType = readUe(reader);
  if (picOrderCountType == 0) {
    static_cast<void>(readUe(reader)); // log2_max_pic_order_cnt_lsb_minus4
  } else if (picOrderCountType == 1) {
    reader.skipBits(1); // delta_pic_order_always_zero_flag
    static_cast<void>(readSe(reader)); // offset_for_non_ref_pic
    static_cast<void>(readSe(reader)); // offset_for_top_to_bottom_field
    const int cycleLength = readUe(reader);
    for (int i = 0; i < cycleLength && i < 256; ++i) {
      static_cast<void>(readSe(reader));
    }
  }
  static_cast<void>(readUe(reader)); // max_num_ref_frames
  reader.skipBits(1); // gaps_in_frame_num_value_allowed_flag
  const int picWidthInMbsMinus1 = readUe(reader);
  const int picHeightInMapUnitsMinus1 = readUe(reader);
  const int frameMbsOnlyFlag = readBits(reader, 1);
  if (frameMbsOnlyFlag == 0) {
    reader.skipBits(1);
  }
  reader.skipBits(1); // direct_8x8_inference_flag
  const int frameCroppingFlag = readBits(reader, 1);
  int cropLeft = 0;
  int cropRight = 0;
  int cropTop = 0;
  int cropBottom = 0;
  if (frameCroppingFlag != 0) {
    cropLeft = readUe(reader);
    cropRight = readUe(reader);
    cropTop = readUe(reader);
    cropBottom = readUe(reader);
  }
  const int cropUnitX = chromaFormatIdc == 1 || chromaFormatIdc == 2 ? 2 : 1;
  const int cropUnitY = chromaFormatIdc == 1 ? 2 : 1;
  int width = (picWidthInMbsMinus1 + 1) * 16;
  int height = (2 - frameMbsOnlyFlag) * (picHeightInMapUnitsMinus1 + 1) * 16;
  width -= (cropLeft + cropRight) * cropUnitX;
  height -= (cropTop + cropBottom) * cropUnitY;
  if (width <= 0 || height <= 0 || width > 8192 || height > 8192) {
    return false;
  }
  outWidth = width;
  outHeight = height;
  return true;
}

/**
 * Parses the SPS of an H.265 stream and returns its dimensions.
 */
bool parseHevcSps(const std::vector<uint8_t>& sps, int& outWidth, int& outHeight) {
  if (sps.size() < 8) {
    return false;
  }
  auto buffer = std::make_shared<AVDataBuffer>(sps);
  AVBitReader reader(buffer);
  // The two byte NAL unit header, then sps_video_parameter_set_id and
  // sps_max_sub_layers_minus1, which the profile tier level follows.
  reader.skipBits(16);
  reader.skipBits(4); // sps_video_parameter_set_id
  const int maxSubLayersMinusOne = readBits(reader, 3);
  reader.skipBits(1); // sps_temporal_id_nesting_flag
  // general_profile_space (2), general_tier_flag (1), general_profile_idc (5),
  // general_profile_compatibility_flags (32), general_constraint_indicator_flags (48) and
  // general_level_idc (8).
  reader.skipBits(96);
  for (int i = 0; i < maxSubLayersMinusOne; ++i) {
    const bool profilePresent = readBits(reader, 1) != 0;
    const bool levelPresent = readBits(reader, 1) != 0;
    if (i < maxSubLayersMinusOne - 1) {
      reader.skipBits(2); // the reserved bits of every sub layer but the last one
    }
    if (profilePresent) {
      reader.skipBits(88);
    }
    if (levelPresent) {
      reader.skipBits(8);
    }
  }
  static_cast<void>(readUe(reader)); // sps_seq_parameter_set_id
  const int chromaFormatIdc = readUe(reader);
  if (chromaFormatIdc == 3) {
    reader.skipBits(1); // separate_colour_plane_flag
  }
  const int picWidth = readUe(reader);
  const int picHeight = readUe(reader);
  if (picWidth <= 0 || picHeight <= 0) {
    return false;
  }
  int width = picWidth;
  int height = picHeight;
  if (readBits(reader, 1) != 0) {
    const int left = readUe(reader);
    const int right = readUe(reader);
    const int top = readUe(reader);
    const int bottom = readUe(reader);
    const int subWidthC = chromaFormatIdc == 1 || chromaFormatIdc == 2 ? 2 : 1;
    const int subHeightC = chromaFormatIdc == 1 ? 2 : 1;
    width -= subWidthC * (left + right);
    height -= subHeightC * (top + bottom);
  }
  if (width <= 0 || height <= 0 || width > 16384 || height > 16384) {
    return false;
  }
  outWidth = width;
  outHeight = height;
  return true;
}

/** Converts profile, compatibility and level bytes to an RFC 6381 AVC codec string. */
std::string buildAvcCodecString(const int profile, const int compatibility, const int level) {
  char codecs[16] = { 0 };
  std::snprintf(codecs, sizeof(codecs), "avc1.%02X%02X%02X", profile & 0xFF, compatibility & 0xFF, level & 0xFF);
  return codecs;
}

/** Converts the profile fields of an HEVC SPS to an RFC 6381 codec string. */
std::string buildHevcCodecString(const std::vector<uint8_t>& sps) {
  // The stream carries the two byte NAL unit header before the profile tier level.
  if (sps.size() < 14) {
    return {};
  }
  const int profileSpace = sps[2] >> 6;
  const bool tierFlag = (sps[2] & 0x20) != 0;
  const int profileIdc = sps[2] & 0x1F;
  const int levelIdc = sps[13];
  char profileSpaceChar = ' ';
  if (profileSpace == 1) {
    profileSpaceChar = 'A';
  } else if (profileSpace == 2) {
    profileSpaceChar = 'B';
  } else if (profileSpace == 3) {
    profileSpaceChar = 'C';
  }
  char codecs[32] = { 0 };
  std::snprintf(codecs, sizeof(codecs), "hvc1.%c%d.%c%d", profileSpaceChar, profileIdc, tierFlag ? 'H' : 'L', levelIdc);
  return codecs;
}

/** One NAL unit of an Annex-B byte stream. */
struct NalUnit {
  /** The offset of the first byte of the start code. */
  size_t prefixOffset = 0;
  /** The offset of the NAL unit header, i.e. after the start code. */
  size_t headerOffset = 0;
  /** The offset of the first byte after the NAL unit. */
  size_t endOffset = 0;
  /** The NAL unit type. */
  int type = -1;
};

/** Returns the NAL units of an Annex-B byte stream. */
std::vector<NalUnit> findNalUnits(const uint8_t* data, const size_t size) {
  std::vector<NalUnit> units;
  size_t offset = 0;
  while (offset + 3 <= size) {
    size_t prefixSize = 0;
    if (data[offset] == 0 && data[offset + 1] == 0 && data[offset + 2] == 1) {
      prefixSize = 3;
    } else if (offset + 4 <= size && data[offset] == 0 && data[offset + 1] == 0 && data[offset + 2] == 0 &&
               data[offset + 3] == 1) {
      prefixSize = 4;
    } else {
      ++offset;
      continue;
    }
    const size_t headerOffset = offset + prefixSize;
    size_t end = headerOffset;
    while (end + 3 <= size) {
      if (data[end] == 0 && data[end + 1] == 0 &&
          (data[end + 2] == 1 || (end + 4 <= size && data[end + 2] == 0 && data[end + 3] == 1))) {
        break;
      }
      ++end;
    }
    if (end < headerOffset) {
      end = headerOffset;
    }
    NalUnit unit;
    unit.prefixOffset = offset;
    unit.headerOffset = headerOffset;
    unit.endOffset = end;
    unit.type = headerOffset < size ? data[headerOffset] : -1;
    units.push_back(unit);
    if (end <= offset) {
      ++offset;
    } else {
      offset = end;
    }
  }
  return units;
}

/** Emits one sample to a track. */
void emitSampleToTrack(const std::shared_ptr<AVTrackOutput>& track, const uint8_t* data, const size_t size,
  const int64_t timeUs, const uint32_t flags) {
  if (track == nullptr || data == nullptr || size == 0) {
    return;
  }
  auto buffer = std::make_shared<AVBlockBuffer>(static_cast<uint32_t>(size));
  buffer->put(data, size);
  buffer->position(0);
  buffer->limit(size);
  const AVSample sample(track->id(), timeUs, flags, std::move(buffer));
  track->sample(sample);
}

} // namespace anonymous

/**
 * The seek map of a transport stream, which collects the key frames that were read.
 */
class AVTsSeekMap final : public AVSeekMap {
public:
  [[nodiscard]] bool isSeekable() const override { return !seekPoints_.empty(); }

  [[nodiscard]] int64_t getDurationUs() const override { return durationUs_; }

  [[nodiscard]] const std::vector<SeekPoint>& getSeekPoints() const override { return seekPoints_; }

  bool getSeekPoints(const int64_t timeUs, SeekPoint& first, SeekPoint& second) const override {
    if (seekPoints_.empty()) {
      return false;
    }
    // The index only covers the samples that were read so far. A time beyond its last point
    // cannot be resolved to a position, and the reader has to restart the container so that the
    // extractor can scan forward to the requested time.
    if (timeUs > seekPoints_.back().timeUs) {
      return false;
    }
    const auto it = std::upper_bound(seekPoints_.begin(), seekPoints_.end(), timeUs,
      [](const int64_t value, const SeekPoint& point) { return value < point.timeUs; });
    if (it == seekPoints_.begin()) {
      first = seekPoints_.front();
      second = seekPoints_.front();
      return true;
    }
    first = *(it - 1);
    second = it == seekPoints_.end() ? first : *it;
    return true;
  }

  void setDurationUs(const int64_t durationUs) { durationUs_ = durationUs; }

  void addSeekPoint(const int64_t timeUs, const long position) {
    if (!seekPoints_.empty() && timeUs <= seekPoints_.back().timeUs) {
      return;
    }
    seekPoints_.emplace_back(timeUs, position);
  }

private:
  std::vector<SeekPoint> seekPoints_;
  int64_t durationUs_ = Definitions::kTimeUnset;
};

AVTsExtractor::AVTsExtractor() {
  readBuffer_.resize(kReadBufferSize);
  buffer_.reserve(kMaxBufferSize);
}

int AVTsExtractor::sniff(AVExtractorInput& input) {
  sniffFailureReason_.clear();
  input.resetPeekPosition();
  uint8_t header[static_cast<size_t>(kPacketSize) * kSniffPacketCount];
  const long peeked = input.peek(header, 0, sizeof(header));
  input.resetPeekPosition();
  if (peeked < static_cast<long>(sizeof(header))) {
    sniffFailureReason_ = "The input is shorter than the sniffing window of a transport stream.";
    return AVExtractorsFactory::kSniffResultNone;
  }
  for (int start = 0; start < kPacketSize; ++start) {
    bool matches = true;
    for (int i = 0; i < kSniffPacketCount; ++i) {
      if (header[start + i * kPacketSize] != kSyncByte) {
        matches = false;
        break;
      }
    }
    if (matches) {
      return AVExtractorsFactory::kSniffResultCertain;
    }
  }
  sniffFailureReason_ = "The sync byte pattern of a transport stream was not found.";
  return AVExtractorsFactory::kSniffResultNone;
}

void AVTsExtractor::init(AVExtractorOutput& output) {
  output_ = &output;
  seekMap_ = std::make_shared<AVTsSeekMap>();
  output.seekMap(seekMap_);
}

AVExtractor::Result AVTsExtractor::read(AVExtractorInput& input) {
  position_ = input.getPosition();
  while (true) {
    if (!fillAndParse(input)) {
      finish(input);
      return kResultEndOfInput;
    }
    if (tracksEnded_ && streams_.empty()) {
      finish(input);
      return kResultEndOfInput;
    }
    if (pendingSeekToStart_) {
      // The metadata of the stream generally precedes the first sample, but a transport stream
      // may interleave the program map table with media packets. Re-reading from the start makes
      // sure that the samples that were read before the tracks existed are delivered as well. The
      // read loop of the reader is left at the start, the next call reads the samples with the
      // consumers that were installed once the tracks became known.
      pendingSeekToStart_ = false;
      if (input.resetPosition(0, true)) {
        resetParserState(true, 0);
        position_ = input.getPosition();
        return kResultContinue;
      }
    }
    position_ = input.getPosition();
    return kResultContinue;
  }
}

void AVTsExtractor::appendToBuffer(const uint8_t* data, const size_t size) {
  if (bufferEnd_ + size > buffer_.size()) {
    buffer_.resize(bufferEnd_ + size);
  }
  std::memcpy(buffer_.data() + bufferEnd_, data, size);
  bufferEnd_ += size;
  if (buffer_.size() > kMaxBufferSize) {
    compactBuffer();
  }
}

void AVTsExtractor::compactBuffer() {
  if (bufferStart_ == 0) {
    if (bufferEnd_ == 0) {
      buffer_.clear();
    }
    return;
  }
  buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(bufferStart_));
  bufferStartPosition_ += static_cast<long>(bufferStart_);
  bufferEnd_ -= bufferStart_;
  bufferStart_ = 0;
  if (bufferEnd_ == 0) {
    buffer_.clear();
  }
}

bool AVTsExtractor::findPacket() {
  while (bufferStart_ < bufferEnd_) {
    size_t candidate = bufferStart_;
    bool found = false;
    while (candidate < bufferEnd_) {
      if (buffer_[candidate] == kSyncByte) {
        if (candidate + kPacketSize > bufferEnd_) {
          found = true;
          break;
        }
        if (candidate + kPacketSize == bufferEnd_ || buffer_[candidate + kPacketSize] == kSyncByte) {
          found = true;
          break;
        }
      }
      ++candidate;
    }
    if (!found) {
      // No sync byte at all, the bytes cannot belong to a transport stream.
      bufferStart_ = bufferEnd_;
      return false;
    }
    bufferStart_ = candidate;
    if (bufferEnd_ - bufferStart_ < kPacketSize) {
      return false;
    }
    return true;
  }
  return false;
}

bool AVTsExtractor::fillAndParse(AVExtractorInput& input) {
  const long read = input.read(readBuffer_.data(), 0, readBuffer_.size());
  position_ = input.getPosition();
  if (read == kEndOfInput) {
    return false;
  }
  if (read <= 0) {
    return false;
  }
  appendToBuffer(readBuffer_.data(), static_cast<size_t>(read));
  while (findPacket()) {
    const long packetPosition = bufferStartPosition_ + static_cast<long>(bufferStart_);
    processPacket(buffer_.data() + bufferStart_, packetPosition);
    bufferStart_ += kPacketSize;
    if (pendingSeekToStart_) {
      // The tracks were just declared. The caller is about to restart the input so that the samples
      // which were read before the tracks existed are delivered once; delivering the rest of this
      // buffer first would repeat them after the restart.
      break;
    }
  }
  compactBuffer();
  return true;
}

void AVTsExtractor::parsePat(const uint8_t* section, const size_t size) {
  if (size < 3 || section[0] != 0x00 || (section[1] & 0x80) == 0) {
    return;
  }
  const int sectionLength = ((section[1] & 0x0F) << 8) | section[2];
  const size_t total = 3 + static_cast<size_t>(sectionLength);
  if (total > size || total < 12) {
    return;
  }
  const size_t end = total - 4; // the table ends with a CRC
  for (size_t offset = 8; offset + 4 <= end; offset += 4) {
    const int programNumber = readUInt16(section + offset);
    if (programNumber == 0) {
      continue;
    }
    const int pid = ((section[offset + 2] & 0x1F) << 8) | section[offset + 3];
    if (pmtPid_ < 0) {
      pmtPid_ = pid;
    }
    if (std::find(pmtPids_.begin(), pmtPids_.end(), pid) == pmtPids_.end()) {
      pmtPids_.push_back(pid);
    }
  }
}

void AVTsExtractor::parsePmt(const uint8_t* section, const size_t size) {
  if (tracksEnded_ || size < 3 || section[0] != 0x02 || (section[1] & 0x80) == 0) {
    return;
  }
  const int sectionLength = ((section[1] & 0x0F) << 8) | section[2];
  const size_t total = 3 + static_cast<size_t>(sectionLength);
  if (total > size || total < 16) {
    return;
  }
  pcrPid_ = ((section[8] & 0x1F) << 8) | section[9];
  const int programInfoLength = ((section[10] & 0x0F) << 8) | section[11];
  size_t offset = 12 + static_cast<size_t>(programInfoLength);
  const size_t end = total - 4; // the table ends with a CRC
  while (offset + 5 <= end) {
    const int streamType = section[offset];
    const int pid = ((section[offset + 1] & 0x1F) << 8) | section[offset + 2];
    const int esInfoLength = ((section[offset + 3] & 0x0F) << 8) | section[offset + 4];
    if (offset + 5 + static_cast<size_t>(esInfoLength) > end) {
      break;
    }
    const int resolvedType = resolveStreamType(streamType, section + offset + 5, static_cast<size_t>(esInfoLength));
    if (isSupportedStreamType(resolvedType)) {
      createStream(pid, resolvedType);
    }
    offset += 5 + static_cast<size_t>(esInfoLength);
  }
  if (!tracksEnded_) {
    tracksEnded_ = true;
    if (!streams_.empty() && seekMap_->getSeekPoints().empty()) {
      // The stream starts at position zero, and a seek to a time before the first key frame that
      // was indexed still has to restart the input.
      seekMap_->addSeekPoint(0, 0);
    }
    output_->endTracks();
    if (!streams_.empty()) {
      pendingSeekToStart_ = true;
    }
  }
}

void AVTsExtractor::createStream(const int pid, const int streamType) {
  if (knownStreamPids_.count(pid) != 0) {
    return;
  }
  const MediaType mediaType = mediaTypeForStreamType(streamType);
  if (mediaType == MediaType::kMediaTypeInvalid) {
    return;
  }
  Stream stream;
  stream.pid = pid;
  stream.streamType = streamType;
  stream.mediaType = mediaType;
  stream.output = output_->track(pid, mediaType, AVTrackOutput::emptyConsumer());
  if (streamType == kStreamTypeScte35) {
    auto format = MediaFormat::CreateSubtitleFormat(MimeTypes::APPLICATION_SCTE35);
    format->setTrackId(pid);
    format->setContainerMimeType(MimeTypes::VIDEO_MP2T);
    format->setSubsampleOffsetUs(0);
    stream.output->format(format);
  }
  if (streamType == kStreamTypeH264 || streamType == kStreamTypeH265) {
    // The caption track is declared with the others, before endTracks(), and receives a sample only
    // when an access unit carries SEI caption data.
    ensureCaptionTrack(stream);
  }
  streams_[pid] = std::move(stream);
  knownStreamPids_.insert(pid);
}

void AVTsExtractor::ensureCaptionTrack(Stream& stream) {
  if (stream.captionOutput != nullptr || output_ == nullptr) {
    return;
  }
  const int id = 0x10000 | (stream.pid & 0x1FFF);
  stream.captionOutput =
    output_->track(id, MediaType::kMediaTypeSubtitle, AVTrackOutput::emptyConsumer());
  auto format = MediaFormat::CreateSubtitleFormat(MimeTypes::APPLICATION_CEA708);
  format->setTrackId(id);
  format->setContainerMimeType(MimeTypes::VIDEO_MP2T);
  format->setSubsampleOffsetUs(MediaFormat::OFFSET_SAMPLE_RELATIVE);
  format->setCueReplacementBehavior(CueReplacementBehavior::kReplace);
  format->setRoleFlags(kRoleFlagSubtitle);
  // The captions live in the SEI of the video stream, so the track is derived and a muxer skips it.
  format->setDerivedTrack(true);
  stream.captionOutput->format(format);
  ensureCaption608Track(stream);
}

void AVTsExtractor::ensureCaption608Track(Stream& stream) {
  if (stream.caption608Output != nullptr || output_ == nullptr) {
    return;
  }
  const int id = 0x20000 | (stream.pid & 0x1FFF);
  stream.caption608Output =
    output_->track(id, MediaType::kMediaTypeSubtitle, AVTrackOutput::emptyConsumer());
  auto format = MediaFormat::CreateSubtitleFormat(MimeTypes::APPLICATION_CEA608);
  format->setTrackId(id);
  format->setContainerMimeType(MimeTypes::VIDEO_MP2T);
  format->setSubsampleOffsetUs(MediaFormat::OFFSET_SAMPLE_RELATIVE);
  format->setCueReplacementBehavior(CueReplacementBehavior::kReplace);
  format->setRoleFlags(kRoleFlagSubtitle);
  // The CEA-608 pairs are carried by the SEI of the video stream: the track is derived.
  format->setDerivedTrack(true);
  stream.caption608Output->format(format);
}

void AVTsExtractor::processPacket(const uint8_t* packet, const long position) {
  if (packet[0] != kSyncByte) {
    return;
  }
  if ((packet[1] & 0x80) != 0) {
    // The packet has uncorrectable errors.
    return;
  }
  const bool payloadUnitStart = (packet[1] & 0x40) != 0;
  const int pid = ((packet[1] & 0x1F) << 8) | packet[2];
  const int adaptationFieldControl = (packet[3] >> 4) & 0x03;
  int offset = 4;
  if ((adaptationFieldControl & 0x02) != 0) {
    if (offset >= kPacketSize) {
      return;
    }
    const int adaptationFieldLength = packet[offset];
    offset = 5 + adaptationFieldLength;
  }
  if ((adaptationFieldControl & 0x01) == 0 || offset >= kPacketSize) {
    return;
  }
  const uint8_t* const payload = packet + offset;
  const size_t payloadSize = static_cast<size_t>(kPacketSize - offset);

  if (pid == kPatPid) {
    if (payloadUnitStart && payloadSize > 1) {
      const size_t pointer = payload[0];
      if (1 + pointer < payloadSize) {
        parsePat(payload + 1 + pointer, payloadSize - 1 - pointer);
      }
    }
    return;
  }
  if (std::find(pmtPids_.begin(), pmtPids_.end(), pid) != pmtPids_.end()) {
    if (payloadUnitStart && payloadSize > 1) {
      const size_t pointer = payload[0];
      if (1 + pointer < payloadSize) {
        parsePmt(payload + 1 + pointer, payloadSize - 1 - pointer);
      }
    }
    return;
  }
  const auto it = streams_.find(pid);
  if (it == streams_.end()) {
    return;
  }
  appendPesPayload(it->second, payload, payloadSize, payloadUnitStart, position);
}

void AVTsExtractor::appendPesPayload(Stream& stream, const uint8_t* payload, const size_t size,
  const bool payloadUnitStart, const long position) {
  if (payloadUnitStart) {
    if (stream.pesActive && !stream.pesData.empty()) {
      processPes(stream);
    }
    stream.pesData.clear();
    stream.pesActive = true;
    stream.pesStartPosition = position;
    stream.pesExpectedSize = -1;
  }
  if (!stream.pesActive || size == 0) {
    return;
  }
  if (stream.pesData.size() + size > kMaxPesSize) {
    // The length field of the PES packet cannot be trusted, drop the packet rather than buffering
    // an amount of memory that the stream declares.
    stream.pesActive = false;
    stream.pesData.clear();
    return;
  }
  stream.pesData.insert(stream.pesData.end(), payload, payload + size);
  if (stream.pesExpectedSize < 0 && stream.pesData.size() >= 6) {
    const int packetLength = (static_cast<int>(stream.pesData[4]) << 8) | stream.pesData[5];
    // A packet length of zero means that the PES packet is unbounded and ends with the next one.
    stream.pesExpectedSize = packetLength == 0 ? -1 : packetLength + 6;
  }
  if (stream.pesExpectedSize > 0 && stream.pesData.size() >= static_cast<size_t>(stream.pesExpectedSize)) {
    processPes(stream);
  }
}

void AVTsExtractor::processPes(Stream& stream) {
  if (!stream.pesActive) {
    return;
  }
  stream.pesActive = false;
  std::vector<uint8_t> pes;
  pes.swap(stream.pesData);
  stream.pesData.clear();
  const long pesStartPosition = stream.pesStartPosition;
  stream.pesStartPosition = -1;
  stream.pesExpectedSize = -1;
  if (pes.size() < 9) {
    return;
  }
  if (pes[0] != 0 || pes[1] != 0 || pes[2] != 1) {
    return;
  }
  const int flags = pes[7];
  const int headerLength = pes[8];
  int64_t timeUs = Definitions::kTimeUnset;
  if ((flags & 0x80) != 0 && pes.size() >= 14) {
    const uint64_t pts = (static_cast<uint64_t>(pes[9] & 0x0E) << 29) | (static_cast<uint64_t>(pes[10]) << 22) |
                         (static_cast<uint64_t>(pes[11] & 0xFE) << 14) | (static_cast<uint64_t>(pes[12]) << 7) |
                         (static_cast<uint64_t>(pes[13] & 0xFE) >> 1);
    timeUs = adjustTimestamp(static_cast<int64_t>(pts));
  }
  if (timeUs == Definitions::kTimeUnset) {
    timeUs = stream.lastSampleTimeUs != Definitions::kTimeUnset ? stream.lastSampleTimeUs : 0;
  }
  const size_t bodyOffset = 9 + static_cast<size_t>(headerLength);
  if (bodyOffset > pes.size()) {
    return;
  }
  const uint8_t* const body = pes.data() + bodyOffset;
  const size_t bodySize = pes.size() - bodyOffset;
  if (bodySize == 0) {
    return;
  }
  stream.pesTimeUs = timeUs;
  // addSeekPoint uses the position of the first transport packet of the PES packet.
  stream.pesStartPosition = pesStartPosition;
  switch (stream.streamType) {
    case kStreamTypeMpeg1Video:
    case kStreamTypeMpeg2Video: {
      consumeMpeg2(stream, body, bodySize, timeUs);
      break;
    }
    case kStreamTypeH264: {
      consumeH264(stream, body, bodySize, timeUs);
      break;
    }
    case kStreamTypeH265: {
      consumeH265(stream, body, bodySize, timeUs);
      break;
    }
    case kStreamTypeAacAdts: {
      consumeAdts(stream, body, bodySize, timeUs);
      break;
    }
    case kStreamTypeMpegAudio:
    case kStreamTypeMpegAudioLsf: {
      consumeMpegAudio(stream, body, bodySize, timeUs);
      break;
    }
    case kStreamTypeScte35: {
      emitSample(stream, body, bodySize, timeUs,
        static_cast<uint32_t>(AVSample::kFlagKeyFrame));
      break;
    }
    default: {
      break;
    }
  }
  stream.pesStartPosition = -1;
}

int64_t AVTsExtractor::adjustTimestamp(const int64_t timestamp) const {
  const int64_t timestampUs = timestamp * 1000000 / 90000;
  if (timestampOffsetUs_ == Definitions::kTimeUnset) {
    timestampOffsetUs_ =
      firstSampleTimestampUs_ != Definitions::kTimeUnset ? firstSampleTimestampUs_ - timestampUs : -timestampUs;
  }
  if (lastUnadjustedUs_ != Definitions::kTimeUnset && timestampUs < lastUnadjustedUs_ - kPtsWrapUs / 2) {
    timestampOffsetUs_ += kPtsWrapUs;
  }
  lastUnadjustedUs_ = timestampUs;
  return timestampUs + timestampOffsetUs_;
}

void AVTsExtractor::emitSample(Stream& stream, const uint8_t* data, const size_t size, const int64_t timeUs,
  const uint32_t flags) {
  if (stream.output == nullptr || data == nullptr || size == 0) {
    return;
  }
  auto buffer = std::make_shared<AVBlockBuffer>(static_cast<uint32_t>(size));
  buffer->put(data, size);
  buffer->position(0);
  buffer->limit(size);
  emitSampleBuffer(stream, std::move(buffer), size, timeUs, flags);
}

void AVTsExtractor::emitSampleBuffer(Stream& stream, std::shared_ptr<AVBlockBuffer> buffer, const size_t size,
  const int64_t timeUs, const uint32_t flags) {
  if (stream.output == nullptr || buffer == nullptr) {
    return;
  }
  if (stream.waitingForSeek) {
    if (stream.mediaType == MediaType::kMediaTypeVideo) {
      const bool keyFrame = (flags & AVSample::kFlagKeyFrame) != 0;
      if (keyFrame) {
        if (timeUs >= stream.seekTargetUs) {
          if (stream.pendingKeyframeData != nullptr) {
            deliverSample(stream, stream.pendingKeyframeData, stream.pendingKeyframeSize, stream.pendingKeyframeTimeUs,
              stream.pendingKeyframeFlags);
            stream.pendingKeyframeData.reset();
          }
          stream.waitingForSeek = false;
          deliverSample(stream, buffer, size, timeUs, flags);
          return;
        }
        // Keep the closest key frame that precedes the requested time. The samples of the
        // inter frames that follow it start the decoder, and the player discards the ones that
        // are before the requested position.
        stream.pendingKeyframeData = std::move(buffer);
        stream.pendingKeyframeSize = size;
        stream.pendingKeyframeTimeUs = timeUs;
        stream.pendingKeyframeFlags = flags;
        return;
      }
      if (stream.pendingKeyframeData != nullptr) {
        if (timeUs < stream.seekTargetUs) {
          // The key frame is kept, but the inter frames that precede the requested time are not
          // delivered: the player is going to discard them anyway.
          return;
        }
        deliverSample(stream, stream.pendingKeyframeData, stream.pendingKeyframeSize, stream.pendingKeyframeTimeUs,
          stream.pendingKeyframeFlags);
        stream.pendingKeyframeData.reset();
        stream.waitingForSeek = false;
      } else {
        // An inter frame cannot start the stream, wait for a key frame.
        return;
      }
    } else {
      if (timeUs < stream.seekTargetUs) {
        return;
      }
      stream.waitingForSeek = false;
    }
  }
  deliverSample(stream, buffer, size, timeUs, flags);
}

void AVTsExtractor::deliverSample(Stream& stream, const std::shared_ptr<AVBlockBuffer>& buffer, const size_t size,
  const int64_t timeUs, const uint32_t flags) {
  if (stream.output == nullptr || buffer == nullptr) {
    return;
  }
  const bool first = !stream.hasEmittedSample;
  uint32_t sampleFlags = flags;
  if (first) {
    sampleFlags |= AVSample::kFlagFirstSample;
  }
  emitSampleToTrack(stream.output, buffer->array(), size, timeUs, sampleFlags);
  stream.hasEmittedSample = true;
  stream.lastSampleTimeUs = timeUs;
  if (maxSampleTimeUs_ == Definitions::kTimeUnset || timeUs > maxSampleTimeUs_) {
    maxSampleTimeUs_ = timeUs;
  }
}

void AVTsExtractor::addSeekPoint(Stream& stream, const int64_t timeUs) {
  if (timeUs == Definitions::kTimeUnset) {
    return;
  }
  if (stream.mediaType == MediaType::kMediaTypeVideo) {
    // The index of a transport stream is built from the key frames of its video track. An audio
    // sample is not a place from which the demultiplexer can resume the other track.
    if (stream.pesStartPosition >= 0) {
      seekMap_->addSeekPoint(timeUs, stream.pesStartPosition);
    }
    return;
  }
  if (seekMap_->getSeekPoints().empty()) {
    // An audio only stream has no key frames, the first sample is still a point that a seek can
    // start from.
    seekMap_->addSeekPoint(timeUs, 0);
  }
}

bool AVTsExtractor::publishH264Format(Stream& stream) {
  if (stream.formatPublished) {
    return true;
  }
  if (stream.sps.empty() || stream.pps.empty()) {
    return false;
  }
  if (stream.sps.size() < 4) {
    return false;
  }
  int width = MediaFormat::NO_VALUE;
  int height = MediaFormat::NO_VALUE;
  int profile = stream.sps[1];
  int compatibility = stream.sps[2];
  int level = stream.sps[3];
  int parsedWidth = 0;
  int parsedHeight = 0;
  if (parseAvcSps(stream.sps, parsedWidth, parsedHeight, profile, compatibility, level)) {
    width = parsedWidth;
    height = parsedHeight;
  }
  std::vector<std::vector<uint8_t>> codecSpecificData;
  std::vector<uint8_t> spsUnit = { 0, 0, 0, 1 };
  spsUnit.insert(spsUnit.end(), stream.sps.begin(), stream.sps.end());
  codecSpecificData.push_back(std::move(spsUnit));
  std::vector<uint8_t> ppsUnit = { 0, 0, 0, 1 };
  ppsUnit.insert(ppsUnit.end(), stream.pps.begin(), stream.pps.end());
  codecSpecificData.push_back(std::move(ppsUnit));

  auto format = MediaFormat::CreateVideoFormat(MimeTypes::VIDEO_H264, width, height);
  format->setContainerMimeType(MimeTypes::VIDEO_MP2T);
  format->setCodecs(buildAvcCodecString(profile, compatibility, level));
  format->setCodecSpecificData(codecSpecificData);
  stream.output->format(format);
  stream.formatPublished = true;
  return true;
}

bool AVTsExtractor::publishH265Format(Stream& stream) {
  if (stream.formatPublished) {
    return true;
  }
  if (stream.vps.empty() || stream.sps.empty() || stream.pps.empty()) {
    return false;
  }
  int width = MediaFormat::NO_VALUE;
  int height = MediaFormat::NO_VALUE;
  int parsedWidth = 0;
  int parsedHeight = 0;
  if (parseHevcSps(stream.sps, parsedWidth, parsedHeight)) {
    width = parsedWidth;
    height = parsedHeight;
  }
  std::vector<std::vector<uint8_t>> codecSpecificData;
  const std::vector<uint8_t>* const units[] = { &stream.vps, &stream.sps, &stream.pps };
  for (const std::vector<uint8_t>* const unit: units) {
    std::vector<uint8_t> nal = { 0, 0, 0, 1 };
    nal.insert(nal.end(), unit->begin(), unit->end());
    codecSpecificData.push_back(std::move(nal));
  }
  auto format = MediaFormat::CreateVideoFormat(MimeTypes::VIDEO_H265, width, height);
  format->setContainerMimeType(MimeTypes::VIDEO_MP2T);
  format->setCodecs(buildHevcCodecString(stream.sps));
  format->setCodecSpecificData(codecSpecificData);
  stream.output->format(format);
  stream.formatPublished = true;
  return true;
}

void AVTsExtractor::consumeMpeg2(Stream& stream, const uint8_t* data, const size_t size, const int64_t timeUs) {
  if (data == nullptr || size == 0) {
    return;
  }
  // A sequence header carries the dimensions; the first one starts the format. The PES body is
  // delivered as one sample, and an I picture marks it as a key frame.
  if (!stream.formatPublished) {
    int width = MediaFormat::NO_VALUE;
    int height = MediaFormat::NO_VALUE;
    for (size_t i = 0; i + 7 < size; ++i) {
      if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1 && data[i + 3] == 0xB3) {
        width = (static_cast<int>(data[i + 4]) << 4) | (data[i + 5] >> 4);
        height = ((data[i + 5] & 0x0F) << 8) | data[i + 6];
        break;
      }
    }
    if (width <= 0 || height <= 0) {
      return;
    }
    auto format = MediaFormat::CreateVideoFormat(MimeTypes::VIDEO_MPEG2, width, height);
    format->setContainerMimeType(MimeTypes::VIDEO_MP2T);
    stream.output->format(format);
    stream.formatPublished = true;
  }

  bool keyFrame = false;
  for (size_t i = 0; i + 5 < size; ++i) {
    if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1 && data[i + 3] == 0x00) {
      const int pictureCodingType = (data[i + 5] >> 3) & 0x07;
      if (pictureCodingType == 1) {
        keyFrame = true;
        break;
      }
    }
  }

  auto buffer = std::make_shared<AVBlockBuffer>(static_cast<uint32_t>(size));
  buffer->put(data, size);
  buffer->position(0);
  buffer->limit(size);
  emitSampleBuffer(stream, std::move(buffer), size, timeUs, keyFrame ? AVSample::kFlagKeyFrame : AVSample::kFlagNone);
}

void AVTsExtractor::consumeH264(Stream& stream, const uint8_t* data, const size_t size, const int64_t timeUs) {
  const std::vector<NalUnit> units = findNalUnits(data, size);
  size_t accessUnitStart = 0;
  bool accessUnitHasVcl = false;
  bool accessUnitIsKey = false;
  int emitted = 0;
  std::vector<uint8_t> accessUnitCcData;
  for (const NalUnit& unit: units) {
    const int type = unit.type & 0x1F;
    if (type == 7 && unit.endOffset > unit.headerOffset) {
      stream.sps.assign(data + unit.headerOffset, data + unit.endOffset);
    } else if (type == 8 && unit.endOffset > unit.headerOffset) {
      stream.pps.assign(data + unit.headerOffset, data + unit.endOffset);
    } else if (type == 6 && unit.endOffset > unit.headerOffset) {
      const auto ccData = text::extractCea708CcDataFromSei(
        data + unit.headerOffset, unit.endOffset - unit.headerOffset, false);
      accessUnitCcData.insert(accessUnitCcData.end(), ccData.begin(), ccData.end());
    }
    if (type == 9 && accessUnitHasVcl && unit.prefixOffset > accessUnitStart) {
      publishH264Format(stream);
      const int64_t sampleTimeUs = emitted == 0 ? timeUs : timeUs + static_cast<int64_t>(emitted) * 40000;
      emitSample(stream, data + accessUnitStart, unit.prefixOffset - accessUnitStart, sampleTimeUs,
        accessUnitIsKey ? AVSample::kFlagKeyFrame : AVSample::kFlagNone);
      const auto captions = text::splitCeaCcData(accessUnitCcData.data(), accessUnitCcData.size());
      emitCaptionSample(stream, captions.cea708, sampleTimeUs);
      emitCaption608Sample(stream, captions.cea608, sampleTimeUs);
      accessUnitCcData.clear();
      if (accessUnitIsKey) {
        addSeekPoint(stream, sampleTimeUs);
      }
      ++emitted;
      accessUnitStart = unit.prefixOffset;
      accessUnitHasVcl = false;
      accessUnitIsKey = false;
    }
    if (type >= 1 && type <= 5) {
      accessUnitHasVcl = true;
    }
    if (type == 5) {
      accessUnitIsKey = true;
    }
  }
  if (accessUnitHasVcl && size > accessUnitStart) {
    publishH264Format(stream);
    const int64_t sampleTimeUs = emitted == 0 ? timeUs : timeUs + static_cast<int64_t>(emitted) * 40000;
    emitSample(stream, data + accessUnitStart, size - accessUnitStart, sampleTimeUs,
      accessUnitIsKey ? AVSample::kFlagKeyFrame : AVSample::kFlagNone);
    emitCaptionSample(stream, accessUnitCcData, sampleTimeUs);
    if (accessUnitIsKey) {
      addSeekPoint(stream, sampleTimeUs);
    }
  }
}

void AVTsExtractor::consumeH265(Stream& stream, const uint8_t* data, const size_t size, const int64_t timeUs) {
  const std::vector<NalUnit> units = findNalUnits(data, size);
  size_t accessUnitStart = 0;
  bool accessUnitHasVcl = false;
  bool accessUnitIsKey = false;
  int emitted = 0;
  std::vector<uint8_t> accessUnitCcData;
  for (const NalUnit& unit: units) {
    if (unit.headerOffset + 1 >= unit.endOffset) {
      continue;
    }
    const int type = (data[unit.headerOffset] >> 1) & 0x3F;
    if (type == 32) {
      stream.vps.assign(data + unit.headerOffset, data + unit.endOffset);
    } else if (type == 33) {
      stream.sps.assign(data + unit.headerOffset, data + unit.endOffset);
    } else if (type == 34) {
      stream.pps.assign(data + unit.headerOffset, data + unit.endOffset);
    } else if ((type == 39 || type == 40) && unit.endOffset > unit.headerOffset) {
      const auto ccData = text::extractCea708CcDataFromSei(
        data + unit.headerOffset, unit.endOffset - unit.headerOffset, true);
      accessUnitCcData.insert(accessUnitCcData.end(), ccData.begin(), ccData.end());
    }
    if (type == 35 && accessUnitHasVcl && unit.prefixOffset > accessUnitStart) {
      publishH265Format(stream);
      const int64_t sampleTimeUs = emitted == 0 ? timeUs : timeUs + static_cast<int64_t>(emitted) * 40000;
      emitSample(stream, data + accessUnitStart, unit.prefixOffset - accessUnitStart, sampleTimeUs,
        accessUnitIsKey ? AVSample::kFlagKeyFrame : AVSample::kFlagNone);
      const auto captions = text::splitCeaCcData(accessUnitCcData.data(), accessUnitCcData.size());
      emitCaptionSample(stream, captions.cea708, sampleTimeUs);
      emitCaption608Sample(stream, captions.cea608, sampleTimeUs);
      accessUnitCcData.clear();
      if (accessUnitIsKey) {
        addSeekPoint(stream, sampleTimeUs);
      }
      ++emitted;
      accessUnitStart = unit.prefixOffset;
      accessUnitHasVcl = false;
      accessUnitIsKey = false;
    }
    if (type < 32) {
      // The VCL NAL unit types of H.265 are 0 to 31, the IRAP pictures start at 16.
      accessUnitHasVcl = true;
      if (type >= 16 && type <= 23) {
        accessUnitIsKey = true;
      }
    }
  }
  if (accessUnitHasVcl && size > accessUnitStart) {
    publishH265Format(stream);
    const int64_t sampleTimeUs = emitted == 0 ? timeUs : timeUs + static_cast<int64_t>(emitted) * 40000;
    emitSample(stream, data + accessUnitStart, size - accessUnitStart, sampleTimeUs,
      accessUnitIsKey ? AVSample::kFlagKeyFrame : AVSample::kFlagNone);
    emitCaptionSample(stream, accessUnitCcData, sampleTimeUs);
    if (accessUnitIsKey) {
      addSeekPoint(stream, sampleTimeUs);
    }
  }
}

void AVTsExtractor::emitCaptionSample(Stream& stream, const std::vector<uint8_t>& ccData,
    const int64_t timeUs) {
  if (ccData.empty() || output_ == nullptr) {
    return;
  }
  ensureCaptionTrack(stream);
  if (stream.captionOutput == nullptr) {
    return;
  }
  auto buffer = std::make_shared<AVBlockBuffer>(static_cast<uint32_t>(ccData.size()));
  buffer->put(ccData.data(), ccData.size());
  buffer->position(0);
  buffer->limit(ccData.size());
  AVSample sample(stream.captionOutput->id(), timeUs, AVSample::kFlagKeyFrame, std::move(buffer));
  stream.captionOutput->sample(sample);
}

void AVTsExtractor::emitCaption608Sample(Stream& stream, const std::vector<uint8_t>& bytePairs,
    const int64_t timeUs) {
  if (bytePairs.empty() || output_ == nullptr) {
    return;
  }
  ensureCaption608Track(stream);
  if (stream.caption608Output == nullptr) {
    return;
  }
  auto buffer = std::make_shared<AVBlockBuffer>(static_cast<uint32_t>(bytePairs.size()));
  buffer->put(bytePairs.data(), bytePairs.size());
  buffer->position(0);
  buffer->limit(bytePairs.size());
  AVSample sample(stream.caption608Output->id(), timeUs, AVSample::kFlagKeyFrame,
    std::move(buffer));
  stream.caption608Output->sample(sample);
}

void AVTsExtractor::consumeAdts(Stream& stream, const uint8_t* data, const size_t size, const int64_t timeUs) {
  size_t offset = 0;
  int frameIndex = 0;
  while (offset + 7 <= size) {
    if (data[offset] != 0xFF || (data[offset + 1] & 0xF6) != 0xF0) {
      ++offset;
      continue;
    }
    AVAdtsHeader header;
    if (!header.setForHeaderData(data + offset, 0)) {
      ++offset;
      continue;
    }
    const int frameLength = header.getFrameLength();
    if (frameLength < 7 || offset + static_cast<size_t>(frameLength) > size) {
      break;
    }
    if (!stream.formatPublished) {
      const int sampleRate = header.getSampleRate();
      const int channelCount = header.channelConfig;
      const int audioObjectType = header.profile;
      if (sampleRate <= 0 || channelCount <= 0) {
        break;
      }
      const uint32_t config =
        buildAudioSpecificConfig(audioObjectType, header.getSamplingFrequencyIndex(), channelCount);
      const uint8_t configBytes[2] = { static_cast<uint8_t>((config >> 8) & 0xFF),
                                       static_cast<uint8_t>(config & 0xFF) };
      auto format = MediaFormat::CreateAudioFormat(MimeTypes::AUDIO_AAC, sampleRate, channelCount);
      format->setContainerMimeType(MimeTypes::VIDEO_MP2T);
      format->setCodecs("mp4a.40." + std::to_string(audioObjectType));
      format->setMaxInputSize(8192);
      format->setCodecSpecificData(0, configBytes, sizeof(configBytes));
      stream.output->format(format);
      stream.sampleRate = sampleRate;
      stream.channelCount = channelCount;
      stream.audioObjectType = audioObjectType;
      stream.formatPublished = true;
    }
    const int headerSize = header.protectionAbsent ? AVAdtsHeader::kHeaderSize : AVAdtsHeader::kHeaderSizeWithCrc;
    const size_t payloadSize = static_cast<size_t>(frameLength - headerSize);
    const int samplesPerFrame = header.getSamplesPerFrame();
    const int64_t sampleTimeUs =
      timeUs + static_cast<int64_t>(frameIndex) * samplesPerFrame * 1000000 / std::max(1, stream.sampleRate);
    emitSample(stream, data + offset + headerSize, payloadSize, sampleTimeUs, AVSample::kFlagKeyFrame);
    addSeekPoint(stream, sampleTimeUs);
    offset += static_cast<size_t>(frameLength);
    ++frameIndex;
  }
}

void AVTsExtractor::consumeMpegAudio(Stream& stream, const uint8_t* data, const size_t size, const int64_t timeUs) {
  size_t offset = 0;
  int frameIndex = 0;
  while (offset + 4 <= size) {
    AVMpegAudioHeader header;
    if (!header.setForHeaderData(readUInt32(data + offset))) {
      ++offset;
      continue;
    }
    const int frameSize = header.getFrameSize();
    if (frameSize <= 0 || offset + static_cast<size_t>(frameSize) > size) {
      break;
    }
    if (!stream.formatPublished) {
      if (header.sampleRate <= 0) {
        break;
      }
      auto format = MediaFormat::CreateAudioFormat(header.getMimeType(), header.sampleRate, header.channels);
      format->setContainerMimeType(MimeTypes::VIDEO_MP2T);
      format->setMaxInputSize(AVMpegAudioHeader::kMaxFrameSize);
      if (header.bitrate > 0) {
        format->setAverageBitrate(header.bitrate);
      }
      stream.output->format(format);
      stream.sampleRate = header.sampleRate;
      stream.channelCount = header.channels;
      stream.formatPublished = true;
    }
    const int64_t sampleTimeUs = timeUs + static_cast<int64_t>(frameIndex) * header.getSamplesPerFrame() * 1000000 /
                                 std::max(1, stream.sampleRate);
    emitSample(stream, data + offset, static_cast<size_t>(frameSize), sampleTimeUs, AVSample::kFlagKeyFrame);
    addSeekPoint(stream, sampleTimeUs);
    offset += static_cast<size_t>(frameSize);
    ++frameIndex;
  }
}

void AVTsExtractor::resetParserState(const bool resetTimestampAdjuster, const int64_t timeUs) {
  buffer_.clear();
  bufferStart_ = 0;
  bufferEnd_ = 0;
  bufferStartPosition_ = 0;
  for (auto& entry: streams_) {
    Stream& stream = entry.second;
    stream.pesData.clear();
    stream.pesActive = false;
    stream.pesStartPosition = -1;
    stream.pesExpectedSize = -1;
    stream.pesTimeUs = Definitions::kTimeUnset;
    stream.lastSampleTimeUs = Definitions::kTimeUnset;
    stream.hasEmittedSample = false;
    stream.pendingKeyframeData.reset();
    stream.pendingKeyframeSize = 0;
    stream.pendingKeyframeTimeUs = Definitions::kTimeUnset;
    stream.pendingKeyframeFlags = 0;
    stream.seekTargetUs = -1;
    stream.waitingForSeek = false;
  }
  if (resetTimestampAdjuster) {
    firstSampleTimestampUs_ = timeUs >= 0 ? timeUs : 0;
    timestampOffsetUs_ = Definitions::kTimeUnset;
    lastUnadjustedUs_ = Definitions::kTimeUnset;
  }
}

bool AVTsExtractor::seek(const long position, const int64_t timeUs) {
  pendingSeekToStart_ = false;
  // A seek that resolves to a byte position starts at a key frame that the index knows, and the
  // timestamp adjuster maps that key frame to the requested time. A seek that restarts at zero has
  // no such position: the samples are read from the beginning, timestamps stay on the timeline of
  // the stream, and the samples before the requested time are dropped.
  const bool restartWithSkip = position == 0 && timeUs > 0;
  resetParserState(true, restartWithSkip ? 0 : timeUs);
  for (auto& entry: streams_) {
    Stream& stream = entry.second;
    stream.seekTargetUs = timeUs;
    stream.waitingForSeek = restartWithSkip;
  }
  return true;
}

void AVTsExtractor::finish(AVExtractorInput& input) {
  position_ = input.getPosition();
  // A PES packet that is unbounded, e.g. the video of a transport stream, ends with the end of the
  // input rather than with a length field or with the start of the next packet.
  for (auto& entry: streams_) {
    if (entry.second.pesActive) {
      processPes(entry.second);
    }
    if (entry.second.pendingKeyframeData != nullptr) {
      // A seek beyond the last key frame of the stream still delivers the last key frame, which
      // is the closest decodable sample to the requested time.
      deliverSample(entry.second, entry.second.pendingKeyframeData, entry.second.pendingKeyframeSize,
        entry.second.pendingKeyframeTimeUs, entry.second.pendingKeyframeFlags);
      entry.second.pendingKeyframeData.reset();
      entry.second.waitingForSeek = false;
    }
  }
  if (seekMap_ != nullptr) {
    int64_t durationUs = maxSampleTimeUs_;
    if (durationUs != Definitions::kTimeUnset) {
      // The last sample of a transport stream has no explicit duration; one frame of a typical
      // video stream is a good enough approximation for a container that has no header.
      durationUs += 40000;
    }
    seekMap_->setDurationUs(durationUs);
  }
  if (tracksFinished_) {
    return;
  }
  if (output_ != nullptr) {
    for (auto& entry: streams_) {
      if (entry.second.output != nullptr) {
        entry.second.output->endTrack();
      }
      if (entry.second.captionOutput != nullptr) {
        entry.second.captionOutput->endTrack();
      }
      if (entry.second.caption608Output != nullptr) {
        entry.second.caption608Output->endTrack();
      }
    }
    output_->endTracks();
  }
  tracksFinished_ = true;
}

} // namespace coral
