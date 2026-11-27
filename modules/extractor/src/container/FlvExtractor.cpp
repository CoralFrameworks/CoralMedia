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

#include <extractor/container/FlvExtractor.h>

#include <extractor/Utility.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace coral {

namespace {

/** The number of bytes of the FLV header. */
constexpr int kFlvHeaderSize = 9;

/** The number of bytes of a tag header. */
constexpr int kTagHeaderSize = 11;

/** The type of an audio tag. */
constexpr int kTagTypeAudio = 8;

/** The type of a video tag. */
constexpr int kTagTypeVideo = 9;

/** The type of a script data tag. */
constexpr int kTagTypeScriptData = 18;

/** The identifier of an FLV file. */
constexpr uint32_t kFlvTag = 0x00464C56; // "FLV"

/** The maximum size of a tag payload that is buffered, in bytes. */
constexpr int kMaxTagDataSize = 64 * 1024 * 1024;

/** The audio format identifiers of the FLV audio tag. */
constexpr int kAudioFormatMp3 = 2;
constexpr int kAudioFormatALaw = 7;
constexpr int kAudioFormatMuLaw = 8;
constexpr int kAudioFormatAac = 10;

/** The sampling rate table of the FLV audio tag. */
constexpr int kAudioSamplingRates[] = { 5512, 11025, 22050, 44100 };

/** The AAC packet type of the sequence header. */
constexpr int kAacPacketTypeSequenceHeader = 0;

/** The AAC packet type of a raw AAC frame. */
constexpr int kAacPacketTypeRaw = 1;

/** The video codec identifier of H.264. */
constexpr int kVideoCodecAvc = 7;

/** The frame type of a key frame. */
constexpr int kVideoFrameKeyFrame = 1;

/** The frame type of a video info frame, which carries no sample. */
constexpr int kVideoFrameVideoInfo = 5;

/** The packet type of the AVC sequence header. */
constexpr int kAvcPacketTypeSequenceHeader = 0;

/** The packet type of an AVC NAL unit frame. */
constexpr int kAvcPacketTypeNalu = 1;

/** The sample rates of an AAC stream, indexed by the sampling frequency index. */
constexpr int kAacSampleRates[] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000,
                                    7350 };

/** The maximum depth of an AMF value that the parser follows. */
constexpr int kMaxAmfDepth = 8;

/** Reads an unsigned 16 bit big endian value. */
uint16_t readUInt16(const uint8_t* data) {
  return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);
}

/** Reads an unsigned 32 bit big endian value. */
uint32_t readUInt32(const uint8_t* data) {
  return (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) |
         (static_cast<uint32_t>(data[2]) << 8) | data[3];
}

/** Reads an unsigned 16 bit big endian value from a buffer, without advancing it. */
bool readAmfString(AVDataBuffer& data, std::string& out) {
  if (data.remaining() < 2) {
    return false;
  }
  const uint16_t length = data.readUnsignedShort();
  if (length > data.remaining()) {
    return false;
  }
  const char* const text = reinterpret_cast<const char*>(data.array() + data.position());
  out.assign(text, length);
  data.skipBytes(length);
  return true;
}

/** A value of the AMF encoding that the script tags of an FLV file use. */
struct AmfValue {
  enum class Type {
    kNumber,
    kBoolean,
    kString,
    kObject,
    kEcmaArray,
    kStrictArray,
    kDate,
    kUnsupported,
  };

  Type type = Type::kUnsupported;
  double number = 0.0;
  bool boolean = false;
  std::string string;
  std::map<std::string, AmfValue> object;
  std::vector<AmfValue> array;
};

/**
 * Reads one AMF value, given the type byte that was already consumed.
 */
bool readAmfValue(AVDataBuffer& data, uint8_t type, AmfValue& out, int depth) {
  if (depth > kMaxAmfDepth) {
    return false;
  }
  switch (type) {
    case 0: {
      // number
      if (data.remaining() < 8) {
        return false;
      }
      const uint64_t bits = data.readUnsignedLong();
      double value = 0.0;
      std::memcpy(&value, &bits, sizeof(value));
      out.type = AmfValue::Type::kNumber;
      out.number = value;
      return true;
    }
    case 1: {
      // boolean
      if (data.remaining() < 1) {
        return false;
      }
      out.type = AmfValue::Type::kBoolean;
      out.boolean = data.readUnsignedByte() != 0;
      return true;
    }
    case 2: {
      // string
      out.type = AmfValue::Type::kString;
      return readAmfString(data, out.string);
    }
    case 3: {
      // object
      out.type = AmfValue::Type::kObject;
      while (data.remaining() > 0) {
        std::string key;
        if (!readAmfString(data, key)) {
          return false;
        }
        if (data.remaining() < 1) {
          return false;
        }
        const uint8_t valueType = data.readUnsignedByte();
        if (valueType == 9) {
          return true;
        }
        AmfValue value;
        if (!readAmfValue(data, valueType, value, depth + 1)) {
          return false;
        }
        out.object[std::move(key)] = std::move(value);
      }
      return false;
    }
    case 8: {
      // ECMA array
      if (data.remaining() < 4) {
        return false;
      }
      out.type = AmfValue::Type::kEcmaArray;
      const uint32_t count = data.readUnsignedInt();
      if (count > data.remaining()) {
        return false;
      }
      for (uint32_t i = 0; i < count && data.remaining() > 0; ++i) {
        std::string key;
        if (!readAmfString(data, key)) {
          return false;
        }
        if (data.remaining() < 1) {
          return false;
        }
        const uint8_t valueType = data.readUnsignedByte();
        if (valueType == 9) {
          return true;
        }
        AmfValue value;
        if (!readAmfValue(data, valueType, value, depth + 1)) {
          return false;
        }
        out.object[std::move(key)] = std::move(value);
      }
      return true;
    }
    case 10: {
      // strict array
      if (data.remaining() < 4) {
        return false;
      }
      out.type = AmfValue::Type::kStrictArray;
      const uint32_t count = data.readUnsignedInt();
      if (count > data.remaining()) {
        return false;
      }
      out.array.reserve(count);
      for (uint32_t i = 0; i < count; ++i) {
        if (data.remaining() < 1) {
          return false;
        }
        const uint8_t valueType = data.readUnsignedByte();
        AmfValue value;
        if (!readAmfValue(data, valueType, value, depth + 1)) {
          return false;
        }
        out.array.push_back(std::move(value));
      }
      return true;
    }
    case 11: {
      // date
      if (data.remaining() < 10) {
        return false;
      }
      const uint64_t bits = data.readUnsignedLong();
      double value = 0.0;
      std::memcpy(&value, &bits, sizeof(value));
      data.skipBytes(2); // the time zone and the reserved field
      out.type = AmfValue::Type::kDate;
      out.number = value;
      return true;
    }
    default: {
      out.type = AmfValue::Type::kUnsupported;
      return false;
    }
  }
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

/**
 * Parses the SPS of an H.264 stream and returns its dimensions and the fields that identify the
 * codec.
 */
/** Skips one AVC scaling list (the deltas are signed Exp-Golomb values). */
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

bool parseAvcSps(const uint8_t* sps, const size_t size, int& outWidth, int& outHeight, int& outProfile,
  int& outCompatibility, int& outLevel) {
  if (sps == nullptr || size < 4) {
    return false;
  }
  outProfile = sps[1];
  outCompatibility = sps[2];
  outLevel = sps[3];

  auto buffer = std::make_shared<AVDataBuffer>(sps, static_cast<uint32_t>(size));
  AVBitReader reader(buffer);
  // The NAL header, the profile, the constraint flags and the level.
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
      reader.skipBits(1); // separate_colour_plane_flag
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
      static_cast<void>(readSe(reader)); // offset_for_ref_frame
    }
  }
  static_cast<void>(readUe(reader)); // max_num_ref_frames
  reader.skipBits(1); // gaps_in_frame_num_value_allowed_flag
  const int picWidthInMbsMinus1 = readUe(reader);
  const int picHeightInMapUnitsMinus1 = readUe(reader);
  const int frameMbsOnlyFlag = readBits(reader, 1);
  if (frameMbsOnlyFlag == 0) {
    reader.skipBits(1); // mb_adaptive_frame_field_flag
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
  // The crop offsets of the 4:2:0 format are in units of two luma samples.
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

/** Converts a profile, a compatibility byte and a level to an RFC 6381 codec string. */
std::string buildAvcCodecString(const int profile, const int compatibility, const int level) {
  char codecs[16] = { 0 };
  std::snprintf(codecs, sizeof(codecs), "avc1.%02X%02X%02X", profile & 0xFF, compatibility & 0xFF, level & 0xFF);
  return codecs;
}

} // namespace anonymous

AVFlvExtractor::AVFlvExtractor()
  : headerBuffer_(static_cast<uint32_t>(kFlvHeaderSize)),
    tagHeaderBuffer_(static_cast<uint32_t>(kTagHeaderSize)) {
  tagData_.reset(4096);
}

int AVFlvExtractor::sniff(AVExtractorInput& input) {
  sniffFailureReason_.clear();
  input.resetPeekPosition();
  uint8_t header[kFlvHeaderSize];
  const long peeked = input.peek(header, 0, sizeof(header));
  input.resetPeekPosition();
  if (peeked < 5) {
    sniffFailureReason_ = "The input is shorter than an FLV header.";
    return AVExtractorsFactory::kSniffResultNone;
  }
  if (AVUtil::readUnsignedInt24(header, 0) != kFlvTag) {
    sniffFailureReason_ = "The FLV identifier was not found.";
    return AVExtractorsFactory::kSniffResultNone;
  }
  if ((readUInt16(header + 3) & 0xFA) != 0) {
    sniffFailureReason_ = "The reserved flags of the FLV header are set.";
    return AVExtractorsFactory::kSniffResultNone;
  }
  if (peeked < kFlvHeaderSize) {
    return AVExtractorsFactory::kSniffResultPossible;
  }
  const int dataOffset = static_cast<int>(readUInt32(header + 5));
  if (dataOffset < kFlvHeaderSize || dataOffset > AVExtractor::kSniffPeekLimit) {
    sniffFailureReason_ = "The FLV header declares an invalid data offset.";
    return AVExtractorsFactory::kSniffResultNone;
  }
  // The four bytes that precede the first tag are the size of the previous tag, which is zero.
  input.resetPeekPosition();
  input.skipPeeked(dataOffset);
  uint8_t previousTagSize[4];
  const long tail = input.peek(previousTagSize, 0, sizeof(previousTagSize));
  input.resetPeekPosition();
  if (tail < static_cast<long>(sizeof(previousTagSize))) {
    sniffFailureReason_ = "The first previous tag size was not found.";
    return AVExtractorsFactory::kSniffResultNone;
  }
  if (readUInt32(previousTagSize) != 0) {
    sniffFailureReason_ = "The size of the first previous tag is not zero.";
    return AVExtractorsFactory::kSniffResultNone;
  }
  return AVExtractorsFactory::kSniffResultCertain;
}

void AVFlvExtractor::init(AVExtractorOutput& output) {
  output_ = &output;
  state_ = State::kReadingFlvHeader;
}

bool AVFlvExtractor::readFlvHeader(AVExtractorInput& input) {
  if (!input.readFully(headerBuffer_, kFlvHeaderSize)) {
    return false;
  }
  headerBuffer_.position(0);
  headerBuffer_.skipBytes(4);
  const int flags = headerBuffer_.readUnsignedByte();
  const bool hasAudio = (flags & 0x04) != 0;
  const bool hasVideo = (flags & 0x01) != 0;
  if (hasAudio && audioOutput_ == nullptr) {
    audioOutput_ = output_->track(kTagTypeAudio, MediaType::kMediaTypeAudio, AVTrackOutput::emptyConsumer());
  }
  if (hasVideo && videoOutput_ == nullptr) {
    videoOutput_ = output_->track(kTagTypeVideo, MediaType::kMediaTypeVideo, AVTrackOutput::emptyConsumer());
  }
  output_->endTracks();
  // The header is followed by the data offset and by the four byte size of the previous tag.
  const long dataOffset = static_cast<long>(headerBuffer_.readUnsignedInt());
  const long toSkip = dataOffset - kFlvHeaderSize + 4;
  bytesToNextTagHeader_ = toSkip > 0 ? static_cast<int>(std::min<long>(toSkip, kMaxTagDataSize)) : 0;
  state_ = State::kSkippingToTagHeader;
  return true;
}

bool AVFlvExtractor::readTagHeader(AVExtractorInput& input) {
  if (!input.readFully(tagHeaderBuffer_, kTagHeaderSize)) {
    return false;
  }
  tagHeaderBuffer_.position(0);
  tagType_ = tagHeaderBuffer_.readUnsignedByte();
  tagDataSize_ = static_cast<int>(tagHeaderBuffer_.readUnsignedInt24());
  tagTimestampUs_ = static_cast<int64_t>(tagHeaderBuffer_.readUnsignedInt24());
  tagTimestampUs_ = ((static_cast<int64_t>(tagHeaderBuffer_.readUnsignedByte()) << 24) | tagTimestampUs_) * 1000;
  tagHeaderBuffer_.skipBytes(3); // the stream identifier
  if (tagDataSize_ < 0 || tagDataSize_ > kMaxTagDataSize) {
    return false;
  }
  return true;
}

bool AVFlvExtractor::readTagData(AVExtractorInput& input) {
  if (!input.readFully(tagData_, static_cast<size_t>(tagDataSize_))) {
    return false;
  }
  tagData_.position(0);
  const int64_t timeUs = getCurrentTimestampUs();
  bool wasSample = false;
  switch (tagType_) {
    case kTagTypeAudio: {
      if (audioOutput_ != nullptr) {
        // The seek map of a stream that has no metadata is not known until the first media tag.
        if (!outputSeekMap_) {
          publishSeekMap(false, Definitions::kTimeUnset);
        }
        wasSample = consumeAudioTag(timeUs);
      }
      break;
    }
    case kTagTypeVideo: {
      if (videoOutput_ != nullptr) {
        if (!outputSeekMap_) {
          publishSeekMap(false, Definitions::kTimeUnset);
        }
        wasSample = consumeVideoTag(timeUs);
      }
      break;
    }
    case kTagTypeScriptData: {
      if (!outputSeekMap_) {
        consumeScriptTag();
      }
      break;
    }
    default: {
      break;
    }
  }
  if (!outputFirstSample_ && wasSample) {
    outputFirstSample_ = true;
    mediaTagTimestampOffsetUs_ = durationUs_ == Definitions::kTimeUnset ? -tagTimestampUs_ : 0;
  }
  return true;
}

int64_t AVFlvExtractor::getCurrentTimestampUs() const {
  if (outputFirstSample_) {
    return mediaTagTimestampOffsetUs_ + tagTimestampUs_;
  }
  return durationUs_ == Definitions::kTimeUnset ? 0 : tagTimestampUs_;
}

void AVFlvExtractor::deliverSample(const std::shared_ptr<AVTrackOutput>& track,
  const std::shared_ptr<AVBlockBuffer>& buffer, const size_t size, const int64_t timeUs, const uint32_t flags) {
  if (track == nullptr || buffer == nullptr) {
    return;
  }
  uint32_t sampleFlags = flags;
  if (track == videoOutput_ && !videoFirstSample_) {
    sampleFlags |= AVSample::kFlagFirstSample;
    videoFirstSample_ = true;
  } else if (track == audioOutput_ && !audioFirstSample_) {
    sampleFlags |= AVSample::kFlagFirstSample;
    audioFirstSample_ = true;
  }
  const AVSample sample(track->id(), timeUs, sampleFlags, buffer);
  track->sample(sample);
}

bool AVFlvExtractor::emitAudioSample(const uint8_t* data, const size_t size, const int64_t timeUs,
  const uint32_t flags) {
  if (audioOutput_ == nullptr || data == nullptr || size == 0) {
    return false;
  }
  if (waitingForAudioSeek_) {
    if (timeUs < seekTargetUs_) {
      return false;
    }
    waitingForAudioSeek_ = false;
  }
  auto buffer = std::make_shared<AVBlockBuffer>(static_cast<uint32_t>(size));
  buffer->put(data, size);
  buffer->position(0);
  buffer->limit(size);
  deliverSample(audioOutput_, buffer, size, timeUs, flags);
  return true;
}

bool AVFlvExtractor::emitVideoSample(const uint8_t* data, const size_t size, const int64_t timeUs,
  const uint32_t flags) {
  if (videoOutput_ == nullptr || data == nullptr || size == 0) {
    return false;
  }
  auto buffer = std::make_shared<AVBlockBuffer>(static_cast<uint32_t>(size));
  buffer->put(data, size);
  buffer->position(0);
  buffer->limit(size);
  if (waitingForVideoSeek_) {
    const bool keyFrame = (flags & AVSample::kFlagKeyFrame) != 0;
    if (keyFrame) {
      if (timeUs >= seekTargetUs_) {
        if (pendingKeyframeData_ != nullptr) {
          deliverSample(videoOutput_, pendingKeyframeData_, pendingKeyframeSize_, pendingKeyframeTimeUs_,
            pendingKeyframeFlags_);
          pendingKeyframeData_.reset();
        }
        waitingForVideoSeek_ = false;
        deliverSample(videoOutput_, buffer, size, timeUs, flags);
        return true;
      }
      // Keep the closest key frame that precedes the requested time.
      pendingKeyframeData_ = std::move(buffer);
      pendingKeyframeSize_ = size;
      pendingKeyframeTimeUs_ = timeUs;
      pendingKeyframeFlags_ = flags;
      return false;
    }
    if (pendingKeyframeData_ != nullptr) {
      if (timeUs < seekTargetUs_) {
        return false;
      }
      deliverSample(videoOutput_, pendingKeyframeData_, pendingKeyframeSize_, pendingKeyframeTimeUs_,
        pendingKeyframeFlags_);
      pendingKeyframeData_.reset();
      waitingForVideoSeek_ = false;
    } else {
      return false;
    }
  }
  deliverSample(videoOutput_, buffer, size, timeUs, flags);
  return true;
}

void AVFlvExtractor::publishAudioFormat(const std::string& mimeType, const int sampleRate, const int channelCount,
  const int maxInputSize) {
  auto format = MediaFormat::CreateAudioFormat(mimeType, sampleRate, channelCount);
  format->setContainerMimeType(MimeTypes::VIDEO_FLV);
  format->setMaxInputSize(maxInputSize);
  audioOutput_->format(format);
  audioFormatPublished_ = true;
}

bool AVFlvExtractor::consumeAudioTag(const int64_t timeUs) {
  if (tagData_.remaining() < 1 || audioUnsupported_) {
    return false;
  }
  const uint8_t header = tagData_.readUnsignedByte();
  if (!audioHeaderParsed_) {
    audioFormat_ = (header >> 4) & 0x0F;
    const int sampleRateIndex = (header >> 2) & 0x03;
    if (audioFormat_ == kAudioFormatMp3) {
      const int sampleRate = kAudioSamplingRates[sampleRateIndex];
      publishAudioFormat(MimeTypes::AUDIO_MPEG, sampleRate, 1, 4096);
    } else if (audioFormat_ == kAudioFormatALaw || audioFormat_ == kAudioFormatMuLaw) {
      publishAudioFormat(audioFormat_ == kAudioFormatALaw ? MimeTypes::AUDIO_G711_ALAW : MimeTypes::AUDIO_G711_MLAW,
        8000, 1, 4096);
    } else if (audioFormat_ != kAudioFormatAac) {
      // The codec is not one that this extractor exposes. The tags are skipped so that the video
      // track of the container is still extracted.
      audioUnsupported_ = true;
      return false;
    }
    audioHeaderParsed_ = true;
  }

  if (audioFormat_ == kAudioFormatMp3) {
    return emitAudioSample(tagData_.array() + tagData_.position(), tagData_.remaining(), timeUs,
      AVSample::kFlagKeyFrame);
  }
  if (audioFormat_ == kAudioFormatALaw || audioFormat_ == kAudioFormatMuLaw) {
    return emitAudioSample(tagData_.array() + tagData_.position(), tagData_.remaining(), timeUs,
      AVSample::kFlagKeyFrame);
  }

  // AAC: every tag starts with a packet type that tells a sequence header from raw data.
  if (tagData_.remaining() < 1) {
    return false;
  }
  const int packetType = tagData_.readUnsignedByte();
  if (packetType == kAacPacketTypeSequenceHeader) {
    if (audioFormatPublished_) {
      return false;
    }
    const size_t configSize = tagData_.remaining();
    const uint8_t* const config = tagData_.array() + tagData_.position();
    if (configSize < 2) {
      return false;
    }
    auto bitBuffer = std::make_shared<AVDataBuffer>(config, static_cast<uint32_t>(configSize));
    AVBitReader reader(bitBuffer);
    int audioObjectType = readBits(reader, 5);
    if (audioObjectType == 31) {
      audioObjectType = 32 + readBits(reader, 6);
    }
    const int frequencyIndex = readBits(reader, 4);
    int sampleRate = frequencyIndex >= 0 && frequencyIndex < static_cast<int>(sizeof(kAacSampleRates) /
                                                                              sizeof(kAacSampleRates[0]))
                       ? kAacSampleRates[frequencyIndex]
                       : -1;
    if (frequencyIndex == 15) {
      sampleRate = readBits(reader, 24);
    }
    const int channelCount = readBits(reader, 4);
    if (sampleRate <= 0 || channelCount <= 0) {
      audioUnsupported_ = true;
      return false;
    }
    auto format = MediaFormat::CreateAudioFormat(MimeTypes::AUDIO_AAC, sampleRate, channelCount);
    format->setContainerMimeType(MimeTypes::VIDEO_FLV);
    format->setMaxInputSize(4096);
    format->setCodecs("mp4a.40." + std::to_string(audioObjectType));
    format->setCodecSpecificData(0, config, configSize);
    audioOutput_->format(format);
    audioFormatPublished_ = true;
    return false;
  }
  if (packetType != kAacPacketTypeRaw || !audioFormatPublished_) {
    return false;
  }
  const size_t size = tagData_.remaining();
  return emitAudioSample(tagData_.array() + tagData_.position(), size, timeUs, AVSample::kFlagKeyFrame);
}

bool AVFlvExtractor::publishVideoFormat(const uint8_t* data, const size_t size) {
  if (data == nullptr || size < 7 || data[0] != 1) {
    return false;
  }
  const int lengthSize = (data[4] & 0x03) + 1;
  if (lengthSize == 3) {
    return false;
  }
  std::vector<std::vector<uint8_t>> codecSpecificData;
  std::vector<uint8_t> firstSps;
  size_t offset = 5;
  const size_t spsCount = data[offset] & 0x1F;
  ++offset;
  for (size_t i = 0; i < spsCount; ++i) {
    if (offset + 2 > size) {
      return false;
    }
    const size_t length = readUInt16(data + offset);
    offset += 2;
    if (length == 0 || offset + length > size) {
      return false;
    }
    std::vector<uint8_t> unit = { 0, 0, 0, 1 };
    unit.insert(unit.end(), data + offset, data + offset + length);
    if (firstSps.empty()) {
      firstSps.assign(data + offset, data + offset + length);
    }
    codecSpecificData.push_back(std::move(unit));
    offset += length;
  }
  if (offset >= size) {
    return false;
  }
  const size_t ppsCount = data[offset];
  ++offset;
  for (size_t i = 0; i < ppsCount; ++i) {
    if (offset + 2 > size) {
      return false;
    }
    const size_t length = readUInt16(data + offset);
    offset += 2;
    if (length == 0 || offset + length > size) {
      return false;
    }
    std::vector<uint8_t> unit = { 0, 0, 0, 1 };
    unit.insert(unit.end(), data + offset, data + offset + length);
    codecSpecificData.push_back(std::move(unit));
    offset += length;
  }
  if (codecSpecificData.empty()) {
    return false;
  }

  int width = MediaFormat::NO_VALUE;
  int height = MediaFormat::NO_VALUE;
  int profile = data[1];
  int compatibility = data[2];
  int level = data[3];
  if (!firstSps.empty()) {
    int spsWidth = 0;
    int spsHeight = 0;
    if (parseAvcSps(firstSps.data(), firstSps.size(), spsWidth, spsHeight, profile, compatibility, level)) {
      width = spsWidth;
      height = spsHeight;
    }
  }
  auto format = MediaFormat::CreateVideoFormat(MimeTypes::VIDEO_H264, width, height);
  format->setContainerMimeType(MimeTypes::VIDEO_FLV);
  format->setCodecs(buildAvcCodecString(profile, compatibility, level));
  format->setCodecSpecificData(codecSpecificData);
  videoOutput_->format(format);
  nalUnitLengthFieldLength_ = lengthSize;
  videoFormatPublished_ = true;
  return true;
}

bool AVFlvExtractor::consumeVideoTag(const int64_t timeUs) {
  if (tagData_.remaining() < 1 || videoUnsupported_) {
    return false;
  }
  const uint8_t videoHeader = tagData_.readUnsignedByte();
  frameType_ = (videoHeader >> 4) & 0x0F;
  const int videoCodec = videoHeader & 0x0F;
  if (videoCodec != kVideoCodecAvc) {
    videoUnsupported_ = true;
    return false;
  }
  if (frameType_ == kVideoFrameVideoInfo) {
    return false;
  }
  if (tagData_.remaining() < 4) {
    return false;
  }
  const int packetType = tagData_.readUnsignedByte();
  uint32_t compositionTime = tagData_.readUnsignedInt24();
  if ((compositionTime & 0x800000) != 0) {
    compositionTime |= 0xFF000000;
  }
  const int32_t compositionTimeMs = static_cast<int32_t>(compositionTime);
  int64_t sampleTimeUs = timeUs + static_cast<int64_t>(compositionTimeMs) * 1000;

  if (packetType == kAvcPacketTypeSequenceHeader) {
    if (!videoFormatPublished_) {
      const bool published = publishVideoFormat(tagData_.array() + tagData_.position(), tagData_.remaining());
    }
    return false;
  }
  if (packetType != kAvcPacketTypeNalu || !videoFormatPublished_) {
    return false;
  }
  // The samples that precede the first key frame cannot be decoded, and a player that starts at a
  // seek point expects the samples to start with a key frame.
  if (!hasOutputKeyframe_ && frameType_ != kVideoFrameKeyFrame) {
    return false;
  }
  const uint8_t* const naluData = tagData_.array() + tagData_.position();
  const size_t naluSize = tagData_.remaining();
  const uint32_t flags = frameType_ == kVideoFrameKeyFrame ? AVSample::kFlagKeyFrame : AVSample::kFlagNone;
  hasOutputKeyframe_ = true;
  const size_t convertedSize = AVUtil::annexBSize(naluData, naluSize, nalUnitLengthFieldLength_);
  if (convertedSize == 0) {
    return false;
  }
  std::vector<uint8_t> converted(convertedSize);
  AVUtil::toAnnexB(naluData, naluSize, nalUnitLengthFieldLength_, converted.data());
  return emitVideoSample(converted.data(), convertedSize, sampleTimeUs, flags);
}

bool AVFlvExtractor::parseMetadata() {
  if (tagData_.remaining() < 1) {
    return false;
  }
  const uint8_t nameType = tagData_.readUnsignedByte();
  if (nameType != 2) {
    return false;
  }
  std::string name;
  if (!readAmfString(tagData_, name) || name != "onMetaData") {
    return false;
  }
  if (tagData_.remaining() < 1) {
    return false;
  }
  const uint8_t type = tagData_.readUnsignedByte();
  if (type != 8) {
    return false;
  }
  AmfValue metadata;
  if (!readAmfValue(tagData_, type, metadata, 0)) {
    return false;
  }

  const auto durationIt = metadata.object.find("duration");
  if (durationIt != metadata.object.end() && durationIt->second.type == AmfValue::Type::kNumber &&
      durationIt->second.number > 0.0) {
    durationUs_ = static_cast<int64_t>(durationIt->second.number * AVUtil::kMicrosPerSecond);
  }

  const auto keyFramesIt = metadata.object.find("keyframes");
  if (keyFramesIt != metadata.object.end() && keyFramesIt->second.type == AmfValue::Type::kObject) {
    const auto positionsIt = keyFramesIt->second.object.find("filepositions");
    const auto timesIt = keyFramesIt->second.object.find("times");
    if (positionsIt != keyFramesIt->second.object.end() && timesIt != keyFramesIt->second.object.end() &&
        positionsIt->second.type == AmfValue::Type::kStrictArray &&
        timesIt->second.type == AmfValue::Type::kStrictArray &&
        positionsIt->second.array.size() == timesIt->second.array.size()) {
      for (size_t i = 0; i < timesIt->second.array.size(); ++i) {
        const AmfValue& time = timesIt->second.array[i];
        const AmfValue& position = positionsIt->second.array[i];
        if (time.type != AmfValue::Type::kNumber || position.type != AmfValue::Type::kNumber) {
          keyFrameTimesUs_.clear();
          keyFramePositions_.clear();
          break;
        }
        const int64_t timeUs = static_cast<int64_t>(time.number * AVUtil::kMicrosPerSecond);
        const long positionValue = static_cast<long>(position.number);
        if (timeUs < 0 || positionValue <= 0) {
          continue;
        }
        keyFrameTimesUs_.push_back(timeUs);
        keyFramePositions_.push_back(positionValue);
      }
    }
  }
  if (!keyFrameTimesUs_.empty()) {
    std::vector<AVSeekMap::SeekPoint> points;
    points.reserve(keyFrameTimesUs_.size());
    for (size_t i = 0; i < keyFrameTimesUs_.size(); ++i) {
      if (!points.empty() && keyFrameTimesUs_[i] <= points.back().timeUs) {
        continue;
      }
      points.emplace_back(keyFrameTimesUs_[i], keyFramePositions_[i]);
    }
    if (!points.empty()) {
      output_->seekMap(std::make_shared<AVIndexSeekMap>(std::move(points), durationUs_, true));
      outputSeekMap_ = true;
      return true;
    }
  }
  if (durationUs_ != Definitions::kTimeUnset) {
    publishSeekMap(false, durationUs_);
    return true;
  }
  return false;
}

void AVFlvExtractor::consumeScriptTag() {
  // The metadata is parsed once. A later script tag, e.g. one that carries the current timestamp of
  // a live stream, is not needed.
  if (parseMetadata()) {
    return;
  }
  publishSeekMap(false, Definitions::kTimeUnset);
}

void AVFlvExtractor::publishSeekMap(const bool seekable, const int64_t durationUs) {
  if (seekable && !keyFrameTimesUs_.empty()) {
    std::vector<AVSeekMap::SeekPoint> points;
    points.reserve(keyFrameTimesUs_.size());
    for (size_t i = 0; i < keyFrameTimesUs_.size(); ++i) {
      points.emplace_back(keyFrameTimesUs_[i], keyFramePositions_[i]);
    }
    output_->seekMap(std::make_shared<AVIndexSeekMap>(std::move(points), durationUs, true));
  } else {
    output_->seekMap(std::make_shared<AVUnseekableSeekMap>(durationUs));
  }
  outputSeekMap_ = true;
}

AVExtractor::Result AVFlvExtractor::read(AVExtractorInput& input) {
  position_ = input.getPosition();
  while (true) {
    switch (state_) {
      case State::kReadingFlvHeader: {
        if (!readFlvHeader(input)) {
          finish(input);
          return kResultEndOfInput;
        }
        break;
      }
      case State::kSkippingToTagHeader: {
        if (!input.skipFully(bytesToNextTagHeader_)) {
          finish(input);
          return kResultEndOfInput;
        }
        bytesToNextTagHeader_ = 0;
        state_ = State::kReadingTagHeader;
        break;
      }
      case State::kReadingTagHeader: {
        if (!readTagHeader(input)) {
          finish(input);
          return kResultEndOfInput;
        }
        state_ = State::kReadingTagData;
        break;
      }
      case State::kReadingTagData: {
        if (!readTagData(input)) {
          finish(input);
          return kResultEndOfInput;
        }
        bytesToNextTagHeader_ = 4; // the size of the previous tag
        state_ = State::kSkippingToTagHeader;
        position_ = input.getPosition();
        return kResultContinue;
      }
    }
  }
}

bool AVFlvExtractor::seek(const long position, const int64_t timeUs) {
  const bool restartWithSkip = position == 0 && timeUs > 0;
  if (position == 0) {
    state_ = State::kReadingFlvHeader;
    outputFirstSample_ = false;
  } else {
    // The position of a key frame that the metadata declared points at a tag header.
    state_ = State::kReadingTagHeader;
  }
  bytesToNextTagHeader_ = 0;
  hasOutputKeyframe_ = false;
  tagDataSize_ = 0;
  seekTargetUs_ = timeUs;
  waitingForVideoSeek_ = restartWithSkip;
  waitingForAudioSeek_ = restartWithSkip;
  pendingKeyframeData_.reset();
  pendingKeyframeSize_ = 0;
  pendingKeyframeTimeUs_ = Definitions::kTimeUnset;
  pendingKeyframeFlags_ = 0;
  return true;
}

void AVFlvExtractor::finish(AVExtractorInput& input) {
  position_ = input.getPosition();
  if (!outputSeekMap_ && output_ != nullptr) {
    publishSeekMap(false, durationUs_);
  }
  if (trackEnded_) {
    return;
  }
  if (pendingKeyframeData_ != nullptr && videoOutput_ != nullptr) {
    // The requested time follows the last key frame of the stream, deliver the last one so that a
    // caller that reads the rest of the file still gets a decodable sample.
    deliverSample(videoOutput_, pendingKeyframeData_, pendingKeyframeSize_, pendingKeyframeTimeUs_,
      pendingKeyframeFlags_);
    pendingKeyframeData_.reset();
  }
  if (audioOutput_ != nullptr) {
    audioOutput_->endTrack();
  }
  if (videoOutput_ != nullptr) {
    videoOutput_->endTrack();
  }
  trackEnded_ = true;
}

} // namespace coral
