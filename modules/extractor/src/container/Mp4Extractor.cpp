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

#include <extractor/container/Mp4Extractor.h>

#include <extractor/Utility.h>
#include <extractor/text/SubtitleParser.h>

#include <extractor/text/cea708/Cea708Sei.h>

#include <media/MetadataEntries.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>

namespace coral {

namespace {

// ==================================================================================================
// Four character codes, see ISO/IEC 14496-12 and the registered box types of the MP4 registration
// authority. The identifiers are the ones that ExoPlayer keeps in its {@code Atom} class.
// ==================================================================================================

/** The container and metadata boxes, used by the sniffer and by the top level parser. */
constexpr uint32_t kFtyp = 0x66747970; // "ftyp"
constexpr uint32_t kMoov = 0x6D6F6F76; // "moov"
constexpr uint32_t kMdat = 0x6D646174; // "mdat"
constexpr uint32_t kFree = 0x66726565; // "free"
constexpr uint32_t kWide = 0x77696465; // "wide"
constexpr uint32_t kMoof = 0x6D6F6F66; // "moof"
constexpr uint32_t kUdta = 0x75647461; // "udta"
constexpr uint32_t kMeta = 0x6D657461; // "meta"
constexpr uint32_t kPssh = 0x70737368; // "pssh"
constexpr uint32_t kEmsg = 0x656D7367; // "emsg"
constexpr uint32_t kKeys = 0x6B657973; // "keys"
constexpr uint32_t kIlst = 0x696C7374; // "ilst"
constexpr uint32_t kData = 0x64617461; // "data"
constexpr uint32_t kChpl = 0x6368706C; // "chpl"
constexpr uint32_t kUuid = 0x75756964; // "uuid"
constexpr uint32_t kXyz = 0xA978797A;   // "(c)xyz"

/** The registered UUID of the XMP metadata box that a movie may carry. */
constexpr uint8_t kXmpUuid[16] = { 0xBE, 0x7A, 0xCF, 0xCB, 0x97, 0xA9, 0x42, 0xE8,
                                   0x9C, 0x71, 0x99, 0x94, 0x91, 0xE3, 0xAF, 0xAC };

/** The largest {@code uuid} box that the extractor buffers, so that a malformed size cannot allocate. */
constexpr uint64_t kMaxUuidPayloadSize = 4 * 1024 * 1024;
/** The largest {@code emsg} box that is copied into a metadata sample. */
constexpr uint64_t kMaxEmsgPayloadSize = 8 * 1024 * 1024;

/** The boxes of the movie box. */
constexpr uint32_t kMvhd = 0x6D766864; // "mvhd"
constexpr uint32_t kTrak = 0x7472616B; // "trak"
constexpr uint32_t kTkhd = 0x746B6864; // "tkhd"
constexpr uint32_t kTref = 0x74726566; // "tref"
constexpr uint32_t kEdts = 0x65647473; // "edts"
constexpr uint32_t kElst = 0x656C7374; // "elst"
constexpr uint32_t kMdia = 0x6D646961; // "mdia"
constexpr uint32_t kMdhd = 0x6D646864; // "mdhd"
constexpr uint32_t kHdlr = 0x68646C72; // "hdlr"
constexpr uint32_t kMinf = 0x6D696E66; // "minf"
constexpr uint32_t kDinf = 0x64696E66; // "dinf"
constexpr uint32_t kStbl = 0x7374626C; // "stbl"
constexpr uint32_t kMvex = 0x6D766578; // "mvex"
constexpr uint32_t kTrex = 0x74726578; // "trex"

/** The boxes of the sample table box. */
constexpr uint32_t kStsd = 0x73747364; // "stsd"
constexpr uint32_t kStts = 0x73747473; // "stts"
constexpr uint32_t kCtts = 0x63747473; // "ctts"
constexpr uint32_t kStsc = 0x73747363; // "stsc"
constexpr uint32_t kStsz = 0x7374737A; // "stsz"
constexpr uint32_t kStz2 = 0x73747A32; // "stz2"
constexpr uint32_t kStco = 0x7374636F; // "stco"
constexpr uint32_t kCo64 = 0x636F3634; // "co64"
constexpr uint32_t kStss = 0x73747373; // "stss"

/** The boxes of a movie fragment. */
constexpr uint32_t kTraf = 0x74726166; // "traf"
constexpr uint32_t kTfhd = 0x74666864; // "tfhd"
constexpr uint32_t kTfdt = 0x74666474; // "tfdt"
constexpr uint32_t kTrun = 0x7472756E; // "trun"

/** The sample group boxes that carry the recovery point of a QuickTime audio sample entry. */
constexpr uint32_t kSgpd = 0x73677064;             // "sgpd"
constexpr uint32_t kSbgp = 0x73626770;             // "sbgp"
constexpr uint32_t kRollGroupingType = 0x726F6C6C; // "roll"

/** The configuration boxes of the sample entries. */
constexpr uint32_t kEsds = 0x65736473; // "esds"
constexpr uint32_t kAvcC = 0x61766343; // "avcC"
constexpr uint32_t kHvcC = 0x68766343; // "hvcC"
constexpr uint32_t kAv1C = 0x61763143; // "av1C"
constexpr uint32_t kDOps = 0x644F7073; // "dOps"
constexpr uint32_t kDFla = 0x64664C61; // "dfLa"
constexpr uint32_t kAlac = 0x616C6163; // "alac"
constexpr uint32_t kBtrt = 0x62747274; // "btrt"
constexpr uint32_t kWave = 0x77617665; // "wave"
constexpr uint32_t kEnda = 0x656E6461; // "enda"

/** The sample entry types that the extractor can describe. */
constexpr uint32_t kMp4a = 0x6D703461; // "mp4a"
constexpr uint32_t kAc3 = 0x61632D33;  // "ac-3"
constexpr uint32_t kEc3 = 0x65632D33;  // "ec-3"
constexpr uint32_t kAc4 = 0x61632D34;  // "ac-4"
constexpr uint32_t kOpus = 0x4F707573; // "Opus"
constexpr uint32_t kFlac = 0x664C6143; // "fLaC"
constexpr uint32_t kAvc1 = 0x61766331; // "avc1"
constexpr uint32_t kAvc3 = 0x61766333; // "avc3"
constexpr uint32_t kHvc1 = 0x68766331; // "hvc1"
constexpr uint32_t kHev1 = 0x68657631; // "hev1"
constexpr uint32_t kAv01 = 0x61763031; // "av01"
constexpr uint32_t kVp08 = 0x76703038; // "vp08"
constexpr uint32_t kVp09 = 0x76703039; // "vp09"
constexpr uint32_t kMp4v = 0x6D703476; // "mp4v"
constexpr uint32_t kS263 = 0x73323633; // "s263"
constexpr uint32_t kDvh1 = 0x64766831; // "dvh1"
constexpr uint32_t kDvhe = 0x64766865; // "dvhe"
constexpr uint32_t kSowt = 0x736F7774; // "sowt"
constexpr uint32_t kTwos = 0x74776F73; // "twos"
/** The sample entry of a 3GPP timed text track. */
constexpr uint32_t kTx3g = 0x74783367; // "tx3g"
/** The sample entry of a WebVTT track. */
constexpr uint32_t kWvtt = 0x77767474; // "wvtt"
/** The sample entry of a TTML track. */
constexpr uint32_t kStpp = 0x73747070; // "stpp"
/** The sample entry of a QuickTime CEA-608 track. */
constexpr uint32_t kC608 = 0x63363038; // "c608"
/** The sample entry of a CEA-708 track. */
constexpr uint32_t kC708 = 0x63373038; // "c708"
/** The sample entry of an ID3 timed metadata track. */
constexpr uint32_t kId3 = 0x69643320; // "id3 "
/** The caption boxes a CEA sample can wrap its cc_data in. */
constexpr uint32_t kCdat = 0x63646174; // "cdat"
constexpr uint32_t kCcdp = 0x63636470; // "ccdp"
/** The caption service and language box of a CEA caption sample entry. */
constexpr uint32_t kClcp = 0x636C6370; // "clcp"
constexpr uint32_t kD263 = 0x64323633; // "d263"
constexpr uint32_t kColr = 0x636F6C72; // "colr"
constexpr uint32_t kMdcv = 0x6D646376; // "mdcv"
constexpr uint32_t kClli = 0x636C6C69; // "clli"
constexpr uint32_t kDvcC = 0x64766343; // "dvcC"
constexpr uint32_t kDvvC = 0x64767643; // "dvvC"

/** The handler types that the extractor exposes. */
constexpr uint32_t kHandlerAudio = 0x736F756E; // "soun"
constexpr uint32_t kHandlerVideo = 0x76696465; // "vide"
constexpr uint32_t kHandlerText = 0x74657874;  // "text"
constexpr uint32_t kHandlerSubtitle = 0x7362746C; // "sbtl"
constexpr uint32_t kHandlerSubtitleAlt = 0x73756274; // "subt"
/** The handler type of a timed metadata track. */
constexpr uint32_t kHandlerMetadata = 0x6D657461; // "meta"

/** The tags of the MPEG-4 elementary stream descriptors of an esds box. */
constexpr uint32_t kEsDescriptorTag = 0x03;
constexpr uint32_t kDecoderConfigDescriptorTag = 0x04;
constexpr uint32_t kDecoderSpecificInfoTag = 0x05;

/** The object type indications that map to an audio MIME type, see RFC 6381. */
constexpr uint32_t kObjectTypeAac = 0x40;
constexpr uint32_t kObjectTypeMpegAudio = 0x69;
constexpr uint32_t kObjectTypeMpegAudioAlt = 0x6B;
constexpr uint32_t kObjectTypeAc3 = 0xA5;
constexpr uint32_t kObjectTypeEAc3 = 0xA6;
constexpr uint32_t kObjectTypeDts = 0xDD;
constexpr uint32_t kObjectTypePcm = 0xE1;

// ==================================================================================================
// Bounds. Every size that comes from the container is validated against one of them, or against the
// length of the input, before it is used to allocate memory.
// ==================================================================================================

/** The largest movie box that is buffered to be parsed. */
constexpr uint64_t kMaxMoovPayloadSize = 16 * 1024 * 1024;
/** The largest movie fragment box that is buffered to be parsed. */
constexpr uint64_t kMaxMoofPayloadSize = 8 * 1024 * 1024;
/** The largest number of samples that one movie fragment may declare. */
constexpr uint64_t kMaxFragmentSamples = 200000;
/** The largest number of samples that one track may describe. */
constexpr uint64_t kMaxSamplesPerTrack = 4000000;
/** The largest sample that is copied into a buffer. */
constexpr uint64_t kMaxSampleSize = 32 * 1024 * 1024;
/** The largest number of sample descriptions of one track. */
constexpr uint64_t kMaxSampleDescriptions = 32;
/** The largest number of edit list entries of one track. */
constexpr uint64_t kMaxEditListEntries = 32;
/** The largest number of seek points that the seek map declares. */
constexpr uint64_t kMaxSeekPoints = 200000;
/** The largest number of bytes that one call of read skips. */
constexpr long kMaxSkipStep = 1024 * 1024;

/** The flag of a sample run that marks a sample as a non sync sample. */
constexpr uint32_t kSampleFlagIsNonSync = 0x00010000;
/** The one of the rotation matrix, in 16.16 fixed point. */
constexpr int32_t kFixedPointOne = 0x00010000;
/** The minus one of the rotation matrix, in 16.16 fixed point. */
constexpr int32_t kFixedPointMinusOne = -0x00010000;

// ==================================================================================================
// Helpers that read big endian values, bounded by the limit of the buffer.
// ==================================================================================================

uint8_t loadUInt8(const AVDataBuffer& data, const uint64_t offset) {
  if (offset >= data.limit()) {
    return 0;
  }
  return data.array()[offset];
}

uint16_t loadUInt16(const AVDataBuffer& data, const uint64_t offset) {
  if (offset + 2 > data.limit()) {
    return 0;
  }
  return AVUtil::readUnsignedShort(data.array(), static_cast<int>(offset));
}

MdtaMetadataEntry::ValueType mdtaValueType(const uint32_t typeCode) {
  switch (typeCode) {
    case 1:
      return MdtaMetadataEntry::ValueType::kUtf8;
    case 21:
      return MdtaMetadataEntry::ValueType::kInteger;
    case 23:
      return MdtaMetadataEntry::ValueType::kFloat;
    case 75:
      return MdtaMetadataEntry::ValueType::kUnsignedInt8;
    case 78:
      return MdtaMetadataEntry::ValueType::kUnsignedInt64;
    default:
      return MdtaMetadataEntry::ValueType::kBinary;
  }
}

uint32_t loadUInt32(const AVDataBuffer& data, const uint64_t offset) {
  if (offset + 4 > data.limit()) {
    return 0;
  }
  return AVUtil::readUnsignedInt(data.array(), static_cast<int>(offset));
}

int32_t loadInt32(const AVDataBuffer& data, const uint64_t offset) {
  return static_cast<int32_t>(loadUInt32(data, offset));
}

uint64_t loadUInt64(const AVDataBuffer& data, const uint64_t offset) {
  return (static_cast<uint64_t>(loadUInt32(data, offset)) << 32) | loadUInt32(data, offset + 4);
}

double loadDouble(const AVDataBuffer& data, const uint64_t offset) {
  const uint64_t bits = loadUInt64(data, offset);
  double value = 0.0;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

/**
 * Copies a range of the buffer, which is only read when the whole range is inside the buffer.
 */
std::vector<uint8_t> loadBytes(const AVDataBuffer& data, const uint64_t offset, const uint64_t size) {
  std::vector<uint8_t> out;
  const uint64_t limit = data.limit();
  if (size == 0 || offset > limit || size > limit - offset) {
    return out;
  }
  out.resize(static_cast<size_t>(size));
  std::memcpy(out.data(), data.array() + offset, static_cast<size_t>(size));
  return out;
}

/**
 * Reads the tag and the expandable length of the descriptor that starts at {@code cursor}, and moves
 * the cursor past them. Mirrors the descriptor reading of {@code AtomParsers.parseEsds}.
 */
bool readDescriptorHeader(const AVDataBuffer& data, uint64_t& cursor, const uint64_t end,
                          uint32_t& tag, uint64_t& length) {
  if (cursor + 2 > end) {
    return false;
  }
  tag = loadUInt8(data, cursor);
  cursor += 1;
  length = 0;
  for (int i = 0; i < 4; ++i) {
    if (cursor >= end) {
      return false;
    }
    const uint8_t byte = loadUInt8(data, cursor);
    cursor += 1;
    length = (length << 7) | static_cast<uint64_t>(byte & 0x7F);
    if ((byte & 0x80) == 0) {
      break;
    }
  }
  return true;
}

std::string fourCcToString(const uint32_t fourCc) {
  char chars[4];
  chars[0] = static_cast<char>((fourCc >> 24) & 0xFF);
  chars[1] = static_cast<char>((fourCc >> 16) & 0xFF);
  chars[2] = static_cast<char>((fourCc >> 8) & 0xFF);
  chars[3] = static_cast<char>(fourCc & 0xFF);
  std::string out;
  for (const char value : chars) {
    if (value == '\0') {
      break;
    }
    out.push_back(value);
  }
  return out;
}

std::string hexByte(const int value) {
  static const char kDigits[] = "0123456789abcdef";
  std::string out;
  out.push_back(kDigits[(value >> 4) & 0x0F]);
  out.push_back(kDigits[value & 0x0F]);
  return out;
}

std::string twoDigits(const int value) {
  std::string out;
  if (value < 10) {
    out.push_back('0');
  }
  out += std::to_string(value);
  return out;
}

bool isAudioSampleEntry(const uint32_t type) {
  return type == kMp4a || type == kAc3 || type == kEc3 || type == kAc4 || type == kOpus ||
         type == kFlac || type == kAlac || type == kSowt || type == kTwos;
}

bool isVideoSampleEntry(const uint32_t type) {
  return type == kAvc1 || type == kAvc3 || type == kHvc1 || type == kHev1 || type == kAv01 ||
         type == kVp08 || type == kVp09 || type == kMp4v || type == kS263 || type == kDvh1 ||
         type == kDvhe;
}

/** Returns whether the sample entry describes a subtitle track. */
bool isSubtitleSampleEntry(const uint32_t type) {
  return type == kTx3g || type == kWvtt || type == kStpp || type == kC608 || type == kC708;
}

/** Returns whether the sample entry describes a timed metadata track. */
bool isMetadataSampleEntry(const uint32_t type) {
  return type == kId3;
}

/** Returns the MIME type of the samples of a subtitle sample entry. */
std::string mimeTypeForSubtitleSampleEntry(const uint32_t type) {
  switch (type) {
    case kTx3g:
      return MimeTypes::APPLICATION_TX3G;
    case kWvtt:
      return MimeTypes::APPLICATION_MP4VTT;
    case kStpp:
      return MimeTypes::APPLICATION_TTML;
    case kC608:
      return MimeTypes::APPLICATION_MP4CEA608;
    case kC708:
      return MimeTypes::APPLICATION_CEA708;
    default:
      return std::string();
  }
}

std::string mimeTypeForSampleEntry(const uint32_t type) {
  switch (type) {
    case kMp4a:
      return MimeTypes::AUDIO_AAC;
    case kAc3:
      return MimeTypes::AUDIO_AC3;
    case kEc3:
      return MimeTypes::AUDIO_E_AC3;
    case kAc4:
      return MimeTypes::AUDIO_AC4;
    case kOpus:
      return MimeTypes::AUDIO_OPUS;
    case kFlac:
      return MimeTypes::AUDIO_FLAC;
    case kAlac:
      return MimeTypes::AUDIO_ALAC;
    case kSowt:
    case kTwos:
      return MimeTypes::AUDIO_RAW;
    case kAvc1:
    case kAvc3:
      return MimeTypes::VIDEO_H264;
    case kHvc1:
    case kHev1:
      return MimeTypes::VIDEO_H265;
    case kAv01:
      return MimeTypes::VIDEO_AV1;
    case kVp08:
      return MimeTypes::VIDEO_VP8;
    case kVp09:
      return MimeTypes::VIDEO_VP9;
    case kMp4v:
      return MimeTypes::VIDEO_MP4V;
    case kS263:
      return MimeTypes::VIDEO_H263;
    case kDvh1:
    case kDvhe:
      return MimeTypes::VIDEO_DOLBY_VISION;
    default:
      return std::string();
  }
}

std::string mimeTypeForObjectType(const uint32_t objectType) {
  switch (objectType) {
    case kObjectTypeAac:
      return MimeTypes::AUDIO_AAC;
    case kObjectTypeMpegAudio:
    case kObjectTypeMpegAudioAlt:
      return MimeTypes::AUDIO_MPEG;
    case kObjectTypeAc3:
      return MimeTypes::AUDIO_AC3;
    case kObjectTypeEAc3:
      return MimeTypes::AUDIO_E_AC3;
    case kObjectTypeDts:
      return MimeTypes::AUDIO_DTS;
    case kObjectTypePcm:
      return MimeTypes::AUDIO_RAW;
    default:
      return std::string();
  }
}

} // namespace anonymous

// ==================================================================================================
// Sniffing
// ==================================================================================================

bool AVMp4Extractor::isMp4Box(const uint32_t fourCc) {
  return fourCc == kFtyp || fourCc == kMoov || fourCc == kMdat || fourCc == kFree ||
         fourCc == kWide;
}

int AVMp4Extractor::sniff(AVExtractorInput& input) {
  sniffFailureReason_.clear();
  input.resetPeekPosition();

  uint8_t scratch[4096];
  uint8_t previous[3] = {0, 0, 0};
  bool certain = false;
  bool possible = false;
  long position = 0;

  while (position < kSniffPeekLimit) {
    size_t wanted = sizeof(scratch);
    if (static_cast<long>(wanted) > kSniffPeekLimit - position) {
      wanted = static_cast<size_t>(kSniffPeekLimit - position);
    }
    const long peeked = input.peek(scratch, 0, wanted);
    if (peeked == kEndOfInput) {
      break;
    }
    const size_t chunkSize = static_cast<size_t>(peeked);

    // The three bytes that precede the chunk are prepended so that a four character code that
    // straddles the boundary of two peeks is still found.
    uint8_t window[3 + sizeof(scratch)];
    std::memcpy(window, previous, 3);
    std::memcpy(window + 3, scratch, chunkSize);
    const long base = position - 3;
    for (size_t i = 0; i + 4 <= chunkSize + 3; ++i) {
      const uint32_t fourCc = AVUtil::readUnsignedInt(window, static_cast<int>(i));
      if (!isMp4Box(fourCc)) {
        continue;
      }
      const long offset = base + static_cast<long>(i);
      if (offset == 4) {
        certain = true;
      } else if (offset >= 0) {
        possible = true;
      }
    }
    if (certain) {
      break;
    }

    if (chunkSize >= 3) {
      std::memcpy(previous, window + chunkSize, 3);
    } else {
      previous[0] = previous[1];
      previous[1] = previous[2];
      previous[2] = window[3];
    }
    position += static_cast<long>(chunkSize);
    if (chunkSize < wanted) {
      // The peek returned a short chunk: the end of the input was reached.
      break;
    }
  }

  input.resetPeekPosition();
  if (certain) {
    return AVExtractorsFactory::kSniffResultCertain;
  }
  if (possible) {
    return AVExtractorsFactory::kSniffResultPossible;
  }
  sniffFailureReason_ = "The input does not start with an ISO base media box.";
  return AVExtractorsFactory::kSniffResultNone;
}

// ==================================================================================================
// The extractor contract
// ==================================================================================================

void AVMp4Extractor::init(AVExtractorOutput& output) {
  output_ = &output;
  state_ = State::kReadingAtomHeader;
  payload_.reset(0);
  atomType_ = 0;
  atomPayloadRemaining_ = 0;
  atomStartPosition_ = 0;
  inputLength_ = -1;
  endOfInput_ = false;
  tracks_.clear();
  trackExtendsDefaults_.clear();
  movieMetadata_.clear();
  topLevelResumePosition_ = 0;
  movieTimescale_ = 0;
  movieDurationUs_ = Definitions::kTimeUnset;
  seekMapEmitted_ = false;
  movieBoxParsed_ = false;
  tracksEnded_ = false;
  position_ = 0;
}

AVExtractor::Result AVMp4Extractor::read(AVExtractorInput& input) {
  if (output_ == nullptr) {
    // init was not called: there is nothing to deliver the samples to.
    position_ = input.getPosition();
    return kResultEndOfInput;
  }
  if (endOfInput_) {
    return finish(input);
  }

  bool progress = false;
  while (!progress) {
    switch (state_) {
      case State::kReadingAtomHeader: {
        if (!readAtomHeader(input)) {
          return finish(input);
        }
        progress = true;
        break;
      }
      case State::kSkippingAtomPayload: {
        if (!skipAtomPayload(input)) {
          return finish(input);
        }
        progress = true;
        break;
      }
      case State::kReadingAtomPayload: {
        if (!readAtomPayload(input)) {
          return finish(input);
        }
        progress = true;
        break;
      }
      case State::kReadingSample: {
        if (readSample(input)) {
          progress = true;
        } else if (movieBoxParsed_ && trackExtendsDefaults_.empty()) {
          // Every sample that the movie box describes was delivered. The scan of the top level boxes
          // resumes after the last box of the movie, where trailers such as the XMP uuid box may
          // follow: the samples left the input inside the media data, which is not a box boundary.
          if (topLevelResumePosition_ > static_cast<uint64_t>(input.getPosition())) {
            if (!input.setPosition(static_cast<long>(topLevelResumePosition_), false)) {
              return finish(input);
            }
            progress = true;
          }
          state_ = State::kReadingAtomHeader;
          break;
        } else {
          // No track has a pending sample: the box parser takes over again, to find the movie fragments
          // that follow, until the end of the input is reached.
          state_ = State::kReadingAtomHeader;
        }
        break;
      }
    }
  }
  position_ = input.getPosition();
  return kResultContinue;
}

bool AVMp4Extractor::seek(const long position, const int64_t timeUs) {
  // The reader moves the input itself and then calls read again; the extractor only has to forget
  // the samples that were consumed and to move its cursors to the new position.
  state_ = State::kReadingAtomHeader;
  atomType_ = 0;
  atomPayloadRemaining_ = 0;
  payload_.reset(0);
  endOfInput_ = false;
  tracksEnded_ = false;
  for (Track& track : tracks_) {
    uint64_t index = 0;
    if (track.sampleTableValid && track.sampleCount > 0) {
      if (timeUs >= 0) {
        index = findSampleIndexForTime(track, timeUs);
      } else if (position >= 0) {
        index = findSampleIndexForPosition(track, position);
      }
    }
    setSampleIndex(track, index);
    // The samples of the movie fragment that is being read are dropped when they precede the
    // requested position: the reader already passed them.
    while (!track.fragments.empty()) {
      const FragmentSample& sample = track.fragments.front();
      const bool beforeTime =
        timeUs >= 0 && sample.timeUs != Definitions::kTimeUnset && sample.timeUs < timeUs;
      const bool beforePosition = timeUs < 0 && position >= 0 &&
                                  sample.offset + sample.size <= static_cast<uint64_t>(position);
      if (!beforeTime && !beforePosition) {
        break;
      }
      track.fragments.pop_front();
    }
  }

  position_ = position >= 0 ? position : 0;
  // The samples of a movie are located by the sample table, not by walking the atoms: the reader
  // restarts in the middle of the media data, where there is no atom header to read. The atom
  // parser only takes over again once every pending sample was delivered.
  state_ = hasPendingSamples() ? State::kReadingSample : State::kReadingAtomHeader;
  return true;
}

AVExtractor::Result AVMp4Extractor::finish(AVExtractorInput& input) {
  endOfInput_ = true;
  if (output_ != nullptr && !tracksEnded_) {
    tracksEnded_ = true;
    for (Track& track : tracks_) {
      track.nextSampleIndex = track.sampleCount;
      track.fragments.clear();
      track.currentSampleValid = false;
      if (track.output != nullptr) {
        track.output->endTrack();
      }
      if (track.captionOutput != nullptr) {
        track.captionOutput->endTrack();
      }
    }
  }
  if (!seekMapEmitted_) {
    // The container never declared a seek map, e.g. because it has no movie box: declare an
    // unseekable one so that the duration is still reported.
    emitSeekMap();
  }
  position_ = input.getPosition();
  return kResultEndOfInput;
}

// ==================================================================================================
// Streaming box reading
// ==================================================================================================

bool AVMp4Extractor::readAtomHeader(AVExtractorInput& input) {
  uint8_t header[8];
  if (!input.readFully(header, 0, sizeof(header))) {
    endOfInput_ = true;
    return false;
  }
  const uint64_t boxStart = static_cast<uint64_t>(input.getPosition()) - sizeof(header);
  uint64_t boxSize = AVUtil::readUnsignedInt(header, 0);
  atomType_ = AVUtil::readUnsignedInt(header, 4);
  uint64_t headerSize = 8;

  if (boxSize == 1) {
    uint8_t large[8];
    if (!input.readFully(large, 0, sizeof(large))) {
      endOfInput_ = true;
      return false;
    }
    boxSize = (static_cast<uint64_t>(AVUtil::readUnsignedInt(large, 0)) << 32) |
              static_cast<uint64_t>(AVUtil::readUnsignedInt(large, 4));
    headerSize = 16;
  } else if (boxSize == 0) {
    // The box extends to the end of the input.
    const long length = input.getLength();
    if (length < 0 || static_cast<uint64_t>(length) <= boxStart) {
      endOfInput_ = true;
      return false;
    }
    boxSize = static_cast<uint64_t>(length) - boxStart;
  }

  const long length = input.getLength();
  inputLength_ = length;
  if (length >= 0) {
    const uint64_t available = static_cast<uint64_t>(length) - boxStart;
    if (boxSize > available) {
      boxSize = available;
    }
  }
  if (boxSize < headerSize) {
    // A malformed box size: the input cannot be parsed any further.
    endOfInput_ = true;
    return false;
  }

  atomStartPosition_ = boxStart;
  atomPayloadStartPosition_ = boxStart + headerSize;
  atomPayloadRemaining_ = boxSize - headerSize;
  topLevelResumePosition_ = boxStart + boxSize;

  if (atomPayloadRemaining_ == 0) {
    onBoxFinished();
    return true;
  }
  if (atomType_ == kMoov || atomType_ == kMoof) {
    const uint64_t maxPayload = atomType_ == kMoov ? kMaxMoovPayloadSize : kMaxMoofPayloadSize;
    if (atomPayloadRemaining_ <= maxPayload) {
      state_ = State::kReadingAtomPayload;
      return true;
    }
  }
  if (atomType_ == kEmsg && atomPayloadRemaining_ <= kMaxEmsgPayloadSize) {
    state_ = State::kReadingAtomPayload;
    return true;
  }
  if (atomType_ == kUuid && atomPayloadRemaining_ <= kMaxUuidPayloadSize) {
    // The XMP metadata of the movie is a top level uuid box, whose payload is buffered to be kept.
    state_ = State::kReadingAtomPayload;
    return true;
  }
  // Every other box is skipped without being buffered, which covers the recognized top level boxes
  // that the extractor does not need, i.e. ftyp, free, wide, skip, styp, sidx, mdat, mfra, udta,
  // meta, pssh and uuid, as well as the boxes it does not know at all. A movie box that is too
  // large to be buffered, or too large for the input it claims to describe, is skipped the same
  // way, so that a malformed size can never allocate an unbounded buffer.
  state_ = State::kSkippingAtomPayload;
  return true;
}

bool AVMp4Extractor::skipAtomPayload(AVExtractorInput& input) {
  if (atomPayloadRemaining_ > 0) {
    const uint64_t remaining = atomPayloadRemaining_;
    const uint64_t step = std::min<uint64_t>(remaining, static_cast<uint64_t>(kMaxSkipStep));
    if (!input.skipFully(static_cast<long>(step))) {
      atomPayloadRemaining_ = 0;
      endOfInput_ = true;
      return false;
    }
    atomPayloadRemaining_ = remaining - step;
  }
  if (atomPayloadRemaining_ == 0) {
    onBoxFinished();
  }
  return true;
}

bool AVMp4Extractor::readAtomPayload(AVExtractorInput& input) {
  const uint64_t size = atomPayloadRemaining_;
  if (size == 0) {
    onBoxFinished();
    return true;
  }
  // The size was validated against the length of the input and against the bounds of the extractor
  // before this state was entered, therefore the buffer is never grown by an unchecked field.
  if (!input.readFully(payload_, static_cast<size_t>(size))) {
    atomPayloadRemaining_ = 0;
    endOfInput_ = true;
    return false;
  }
  payload_.truncate(0, static_cast<size_t>(size));
  atomPayloadRemaining_ = 0;

  if (atomType_ == kMoov) {
    parseMoov(payload_, 0, size);
  } else if (atomType_ == kMoof) {
    parseMoof(payload_, 0, size, atomStartPosition_);
  } else if (atomType_ == kEmsg) {
    parseEmsg(payload_, 0, size, atomPayloadStartPosition_);
  } else if (atomType_ == kUuid) {
    parseUuid(payload_, 0, size);
  }
  onBoxFinished();
  return true;
}

void AVMp4Extractor::onBoxFinished() {
  state_ = hasPendingSamples() ? State::kReadingSample : State::kReadingAtomHeader;
}

// ==================================================================================================
// Buffered box parsing
// ==================================================================================================

AVMp4Extractor::BoxHeader AVMp4Extractor::readBoxHeader(const AVDataBuffer& data,
                                                        const uint64_t offset, const uint64_t end) {
  BoxHeader header;
  const uint64_t limit = std::min<uint64_t>(end, data.limit());
  if (offset + 8 > limit) {
    return header;
  }
  const uint64_t declaredSize = loadUInt32(data, offset);
  header.type = loadUInt32(data, offset + 4);
  uint64_t boxSize = declaredSize;
  uint64_t headerSize = 8;
  if (declaredSize == 1) {
    if (offset + 16 > limit) {
      return header;
    }
    boxSize = loadUInt64(data, offset + 8);
    headerSize = 16;
  } else if (declaredSize == 0) {
    // The box extends to the end of its parent.
    boxSize = limit - offset;
  }
  if (boxSize < headerSize) {
    return header;
  }
  header.payloadOffset = offset + headerSize;
  if (header.payloadOffset > limit) {
    return header;
  }
  header.nextOffset = offset + boxSize;
  if (header.nextOffset > limit) {
    // A box that claims to be larger than its parent: the size of the parent is authoritative.
    header.nextOffset = limit;
  }
  header.payloadSize = header.nextOffset - header.payloadOffset;
  header.valid = header.nextOffset > offset;
  return header;
}

void AVMp4Extractor::parseMoov(AVDataBuffer& data, const uint64_t offset, const uint64_t size) {
  if (movieBoxParsed_) {
    // The movie box was parsed already: the samples it describes were delivered, and parsing it again
    // would declare the same tracks a second time, with the sample cursors of a movie that was read.
    return;
  }
  const uint64_t end = offset + size;
  uint64_t cursor = offset;
  while (cursor + 8 <= end) {
    const BoxHeader box = readBoxHeader(data, cursor, end);
    if (!box.valid) {
      break;
    }
    switch (box.type) {
      case kMvhd: {
        if (box.payloadSize >= 4) {
          const uint32_t version = loadUInt32(data, box.payloadOffset) >> 24;
          uint64_t field = box.payloadOffset + 4;
          uint64_t duration = 0;
          if (version == 1) {
            field += 16;
            if (field + 12 <= end) {
              movieTimescale_ = static_cast<int64_t>(loadUInt32(data, field));
              duration = loadUInt64(data, field + 4);
            }
          } else {
            field += 8;
            if (field + 8 <= end) {
              movieTimescale_ = static_cast<int64_t>(loadUInt32(data, field));
              duration = loadUInt32(data, field + 4);
            }
          }
          if (movieTimescale_ > 0 && duration > 0 && duration != 0xFFFFFFFFull) {
            movieDurationUs_ = AVUtil::scaleToUs(static_cast<int64_t>(duration), movieTimescale_);
          }
        }
        break;
      }
      case kTrak: {
        parseTrak(data, box.payloadOffset, box.payloadSize);
        break;
      }
      case kMvex: {
        parseMvex(data, box.payloadOffset, box.payloadSize);
        break;
      }
      case kUdta: {
        parseUdta(data, box.payloadOffset, box.payloadSize);
        break;
      }
      case kMeta:
      case kPssh: {
        // A movie level meta box is unusual, and encryption is not supported.
        break;
      }
      default: {
        break;
      }
    }
    if (box.nextOffset <= cursor) {
      break;
    }
    cursor = box.nextOffset;
  }
  createTracksAndSeekMap();
  movieBoxParsed_ = true;
}

void AVMp4Extractor::parseTrak(AVDataBuffer& data, const uint64_t offset, const uint64_t size) {
  Track track;
  const uint64_t end = offset + size;
  uint64_t cursor = offset;
  while (cursor + 8 <= end) {
    const BoxHeader box = readBoxHeader(data, cursor, end);
    if (!box.valid) {
      break;
    }
    switch (box.type) {
      case kTkhd: {
        parseTkhd(data, box.payloadOffset, box.payloadSize, track);
        break;
      }
      case kEdts: {
        parseEdts(data, box.payloadOffset, box.payloadSize, track);
        break;
      }
      case kMdia: {
        parseMdia(data, box.payloadOffset, box.payloadSize, track);
        break;
      }
      case kTref:
      case kUdta:
      case kMeta:
      case kPssh: {
        // Track references, user data and protection metadata are not needed.
        break;
      }
      default: {
        break;
      }
    }
    if (box.nextOffset <= cursor) {
      break;
    }
    cursor = box.nextOffset;
  }

  bool supportedDescription = false;
  bool descriptionIsVideo = false;
  bool descriptionIsSubtitle = false;
  bool descriptionIsMetadata = false;
  for (const SampleDescription& description : track.sampleDescriptions) {
    if (description.supported) {
      supportedDescription = true;
      descriptionIsVideo = description.video;
      descriptionIsSubtitle = description.subtitle;
      descriptionIsMetadata = description.metadata;
      break;
    }
  }
  if (!supportedDescription) {
    // The codec of the track cannot be described: the track is skipped rather than exposed with a
    // made up format.
    return;
  }
  if (descriptionIsSubtitle) {
    // The sample entry decides for a subtitle: a track whose handler names a different player is
    // still a subtitle track.
    track.mediaType = MediaType::kMediaTypeSubtitle;
  } else if (descriptionIsMetadata) {
    track.mediaType = MediaType::kMediaTypeMetadata;
  } else if (track.mediaType == MediaType::kMediaTypeInvalid) {
    track.mediaType = descriptionIsVideo ? MediaType::kMediaTypeVideo : MediaType::kMediaTypeAudio;
  } else if (descriptionIsVideo != (track.mediaType == MediaType::kMediaTypeVideo)) {
    // A handler that disagrees with the sample entry describes a track whose samples the parser
    // cannot interpret, e.g. a video entry under a text handler.
    return;
  }
  if (track.mediaType != MediaType::kMediaTypeAudio &&
      track.mediaType != MediaType::kMediaTypeVideo &&
      track.mediaType != MediaType::kMediaTypeSubtitle &&
      track.mediaType != MediaType::kMediaTypeMetadata) {
    // An unknown handler whose samples are not described: not exposed.
    return;
  }

  track.supported = true;
  finalizeTrack(track);
  track.index = static_cast<int>(tracks_.size());
  tracks_.push_back(std::move(track));
}

void AVMp4Extractor::parseTkhd(AVDataBuffer& data, const uint64_t offset, const uint64_t size,
                               Track& track) {
  const uint64_t end = offset + size;
  if (size < 4) {
    return;
  }
  const uint32_t version = loadUInt32(data, offset) >> 24;
  uint64_t cursor = offset + 4;
  cursor += version == 1 ? 16 : 8; // creation time and modification time
  if (cursor + 24 > end) {
    return;
  }
  track.trackId = loadUInt32(data, cursor);
  cursor += 4;                    // track_ID
  cursor += 4;                    // reserved
  cursor += version == 1 ? 8 : 4; // duration
  cursor += 8;                    // reserved
  cursor += 8;                    // layer, alternate group, volume, reserved
  if (cursor + 36 + 8 > end) {
    return;
  }
  // The transformation matrix is stored in 16.16 fixed point, and only the four entries that carry
  // the rotation are of interest.
  const int32_t a = loadInt32(data, cursor);
  const int32_t b = loadInt32(data, cursor + 4);
  const int32_t c = loadInt32(data, cursor + 12);
  const int32_t d = loadInt32(data, cursor + 16);
  if (a == 0 && b == kFixedPointOne && c == kFixedPointMinusOne && d == 0) {
    track.rotation = 90;
  } else if (a == 0 && b == kFixedPointMinusOne && c == kFixedPointOne && d == 0) {
    track.rotation = 270;
  } else if (a == kFixedPointMinusOne && b == 0 && c == 0 && d == kFixedPointMinusOne) {
    track.rotation = 180;
  }
}

void AVMp4Extractor::parseEdts(AVDataBuffer& data, const uint64_t offset, const uint64_t size,
                               Track& track) {
  const uint64_t end = offset + size;
  uint64_t cursor = offset;
  while (cursor + 8 <= end) {
    const BoxHeader box = readBoxHeader(data, cursor, end);
    if (!box.valid) {
      break;
    }
    if (box.type == kElst && box.payloadSize >= 8) {
      const uint32_t version = loadUInt32(data, box.payloadOffset) >> 24;
      const uint64_t entryCount = loadUInt32(data, box.payloadOffset + 4);
      const uint64_t entrySize = version == 1 ? 20 : 12;
      const uint64_t available = box.payloadSize - 8;
      if (entryCount <= available / entrySize && entryCount <= kMaxEditListEntries) {
        uint64_t entry = box.payloadOffset + 8;
        for (uint64_t i = 0; i < entryCount; ++i) {
          EditListEntry edit;
          if (version == 1) {
            edit.segmentDuration = static_cast<int64_t>(loadUInt64(data, entry));
            edit.mediaTime = static_cast<int64_t>(loadUInt64(data, entry + 8));
            edit.mediaRate = loadInt32(data, entry + 16);
            entry += 20;
          } else {
            edit.segmentDuration = static_cast<int64_t>(loadUInt32(data, entry));
            edit.mediaTime = static_cast<int64_t>(loadInt32(data, entry + 4));
            edit.mediaRate = loadInt32(data, entry + 8);
            entry += 12;
          }
          track.editList.push_back(edit);
        }
      }
    }
    if (box.nextOffset <= cursor) {
      break;
    }
    cursor = box.nextOffset;
  }
}

void AVMp4Extractor::parseMdia(AVDataBuffer& data, const uint64_t offset, const uint64_t size,
                               Track& track) {
  const uint64_t end = offset + size;
  uint64_t cursor = offset;
  while (cursor + 8 <= end) {
    const BoxHeader box = readBoxHeader(data, cursor, end);
    if (!box.valid) {
      break;
    }
    switch (box.type) {
      case kMdhd: {
        parseMdhd(data, box.payloadOffset, box.payloadSize, track);
        break;
      }
      case kHdlr: {
        parseHdlr(data, box.payloadOffset, box.payloadSize, track);
        break;
      }
      case kMinf: {
        parseMinf(data, box.payloadOffset, box.payloadSize, track);
        break;
      }
      default: {
        // The edit name box and the unknown boxes are skipped.
        break;
      }
    }
    if (box.nextOffset <= cursor) {
      break;
    }
    cursor = box.nextOffset;
  }
}

void AVMp4Extractor::parseMdhd(AVDataBuffer& data, const uint64_t offset, const uint64_t size,
                               Track& track) {
  const uint64_t end = offset + size;
  if (size < 4) {
    return;
  }
  const uint32_t version = loadUInt32(data, offset) >> 24;
  uint64_t cursor = offset + 4;
  cursor += version == 1 ? 16 : 8; // creation time and modification time
  uint64_t duration = 0;
  uint64_t timescale = 0;
  if (version == 1) {
    if (cursor + 12 > end) {
      return;
    }
    timescale = loadUInt32(data, cursor);
    duration = loadUInt64(data, cursor + 4);
    cursor += 12;
  } else {
    if (cursor + 8 > end) {
      return;
    }
    timescale = loadUInt32(data, cursor);
    duration = loadUInt32(data, cursor + 4);
    cursor += 8;
  }
  track.timescale = static_cast<int64_t>(timescale);
  if (duration > 0 && duration != 0xFFFFFFFFull && track.timescale > 0) {
    track.durationUs = AVUtil::scaleToUs(static_cast<int64_t>(duration), track.timescale);
  }
  if (cursor + 2 <= end) {
    // The language is three characters of five bits, biased by the ASCII code of 'a'.
    const uint16_t language = loadUInt16(data, cursor);
    bool valid = language != 0;
    std::string parsed;
    for (int shift = 10; shift >= 0 && valid; shift -= 5) {
      const char character = static_cast<char>(((language >> shift) & 0x1F) + 0x60);
      if (character < 'a' || character > 'z') {
        valid = false;
        break;
      }
      parsed.push_back(character);
    }
    if (valid && parsed != "und") {
      track.language = parsed;
    }
  }
}

void AVMp4Extractor::parseHdlr(AVDataBuffer& data, const uint64_t offset, const uint64_t size,
                               Track& track) {
  if (size < 12) {
    return;
  }
  const uint32_t handlerType = loadUInt32(data, offset + 8);
  switch (handlerType) {
    case kHandlerAudio: {
      track.mediaType = MediaType::kMediaTypeAudio;
      break;
    }
    case kHandlerVideo: {
      track.mediaType = MediaType::kMediaTypeVideo;
      break;
    }
    case kHandlerText:
    case kHandlerSubtitle:
    case kHandlerSubtitleAlt: {
      track.mediaType = MediaType::kMediaTypeSubtitle;
      break;
    }
    case kHandlerMetadata: {
      track.mediaType = MediaType::kMediaTypeMetadata;
      break;
    }
    default: {
      // A metadata or unknown handler: the track is not exposed. The type of its sample entry still
      // decides, because a subtitle track declares the handler of the player it was authored for.
      break;
    }
  }
}

void AVMp4Extractor::parseMinf(AVDataBuffer& data, const uint64_t offset, const uint64_t size,
                               Track& track) {
  const uint64_t end = offset + size;
  uint64_t cursor = offset;
  while (cursor + 8 <= end) {
    const BoxHeader box = readBoxHeader(data, cursor, end);
    if (!box.valid) {
      break;
    }
    switch (box.type) {
      case kStbl: {
        parseStbl(data, box.payloadOffset, box.payloadSize, track);
        break;
      }
      case kDinf: {
        // The data reference box is not needed, the extractor reads the input it was given.
        break;
      }
      default: {
        // The media header boxes (vmhd, smhd, hmhd, nmhd) and the unknown boxes are skipped.
        break;
      }
    }
    if (box.nextOffset <= cursor) {
      break;
    }
    cursor = box.nextOffset;
  }
}

void AVMp4Extractor::parseStbl(AVDataBuffer& data, const uint64_t offset, const uint64_t size,
                               Track& track) {
  const uint64_t end = offset + size;
  uint64_t cursor = offset;
  while (cursor + 8 <= end) {
    const BoxHeader box = readBoxHeader(data, cursor, end);
    if (!box.valid) {
      break;
    }
    switch (box.type) {
      case kStsd: {
        parseStsd(data, box.payloadOffset, box.payloadSize, track);
        break;
      }
      case kStts: {
        parseStts(data, box.payloadOffset, box.payloadSize, track);
        break;
      }
      case kCtts: {
        parseCtts(data, box.payloadOffset, box.payloadSize, track);
        break;
      }
      case kStsc: {
        parseStsc(data, box.payloadOffset, box.payloadSize, track);
        break;
      }
      case kStsz: {
        parseStsz(data, box.payloadOffset, box.payloadSize, false, track);
        break;
      }
      case kStz2: {
        parseStsz(data, box.payloadOffset, box.payloadSize, true, track);
        break;
      }
      case kStco: {
        parseStco(data, box.payloadOffset, box.payloadSize, false, track);
        break;
      }
      case kCo64: {
        parseStco(data, box.payloadOffset, box.payloadSize, true, track);
        break;
      }
      case kStss: {
        parseStss(data, box.payloadOffset, box.payloadSize, track);
        break;
      }
      default: {
        // The sample dependency, sample group, shadow, padding and sub sample boxes are skipped.
        break;
      }
    }
    if (box.nextOffset <= cursor) {
      break;
    }
    cursor = box.nextOffset;
  }
}

void AVMp4Extractor::parseStsd(AVDataBuffer& data, const uint64_t offset, const uint64_t size,
                               Track& track) {
  const uint64_t end = offset + size;
  if (size < 8) {
    return;
  }
  const uint64_t entryCount = loadUInt32(data, offset + 4);
  uint64_t cursor = offset + 8;
  uint64_t index = 0;
  while (cursor + 8 <= end && index < entryCount && index < kMaxSampleDescriptions) {
    const uint64_t entrySize = loadUInt32(data, cursor);
    const uint32_t type = loadUInt32(data, cursor + 4);
    if (entrySize < 8 || cursor + entrySize > end) {
      break;
    }
    SampleDescription description;
    if (isAudioSampleEntry(type) || isVideoSampleEntry(type) || isSubtitleSampleEntry(type) ||
        isMetadataSampleEntry(type)) {
      description.video = isVideoSampleEntry(type);
      description.subtitle = isSubtitleSampleEntry(type);
      description.metadata = isMetadataSampleEntry(type);
      description.mimeType = description.subtitle ? mimeTypeForSubtitleSampleEntry(type)
                            : description.metadata ? MimeTypes::APPLICATION_ID3
                                                   : mimeTypeForSampleEntry(type);
      description.codecs = fourCcToString(type);
      if (type == kOpus) {
        description.codecs = "opus";
      } else if (type == kFlac) {
        description.codecs = "flac";
      }
      parseSampleEntry(data, cursor, entrySize, description);
    }
    // An unsupported sample entry is kept as a placeholder, so that the sample description indices
    // of the stsc and trun boxes stay aligned with the entries of the box.
    track.sampleDescriptions.push_back(std::move(description));
    cursor += entrySize;
    ++index;
  }
}

void AVMp4Extractor::parseSampleEntry(AVDataBuffer& data, const uint64_t offset,
                                      const uint64_t size, SampleDescription& description) {
  const uint64_t end = offset + size;
  const uint32_t type = loadUInt32(data, offset + 4);
  if (description.metadata) {
    // An `id3 ` sample entry carries raw ID3 tags; it needs no configuration record.
    description.supported = true;
    return;
  }
  if (description.subtitle) {
    // A subtitle sample entry starts with the six reserved bytes and the data reference index of
    // every sample entry, then describes its own default style.
    if (size < 16) {
      return;
    }
    switch (type) {
      case kTx3g: {
        // The default style of the track, from the display flags to the font table, is what a parser
        // needs to style the samples, and the format carries it as its codec specific data.
        const uint64_t styleOffset = offset + 16;
        if (styleOffset < end) {
          description.codecSpecificData.push_back(loadBytes(data, styleOffset, end - styleOffset));
        }
        description.subsampleOffsetUs = MediaFormat::OFFSET_SAMPLE_RELATIVE;
        description.supported = !description.codecSpecificData.empty();
        break;
      }
      case kWvtt: {
        // The samples of a WebVTT track carry the cue text of the interval of the sample.
        description.subsampleOffsetUs = MediaFormat::OFFSET_SAMPLE_RELATIVE;
        description.supported = true;
        break;
      }
      case kStpp: {
        // The samples of a TTML track state absolute times.
        description.subsampleOffsetUs = 0;
        description.supported = true;
        break;
      }
      case kC608:
      case kC708: {
        // A caption sample wraps its cc_data in a cdat/ccdp box; the parser wants the payload.
        description.subsampleOffsetUs = MediaFormat::OFFSET_SAMPLE_RELATIVE;
        description.supported = true;
        description.stripCaptionEnvelope = true;
        // The optional clcp box states the service, the language and the accessibility flags.
        uint64_t child = offset + 16;
        while (child + 8 <= end) {
          const uint32_t childSize = loadUInt32(data, child);
          if (childSize < 8 || child + childSize > end) {
            break;
          }
          if (loadUInt32(data, child + 4) == kClcp && childSize >= 14) {
            const uint64_t payload = child + 8;
            description.captionServiceNumber = loadUInt8(data, payload);
            description.captionLanguage.assign(
              reinterpret_cast<const char*>(data.array() + payload + 1), 3);
            description.captionEasyReader = loadUInt8(data, payload + 4) != 0;
            description.captionWideAspect = loadUInt8(data, payload + 5) != 0;
          }
          child += childSize;
        }
        break;
      }
      default: {
        break;
      }
    }
    return;
  }
  if (description.video) {
    if (size < 86) {
      return;
    }
    description.width = loadUInt16(data, offset + 32);
    description.height = loadUInt16(data, offset + 34);
    parseVideoSampleEntryChildren(data, offset + 86, end - (offset + 86), description);
    switch (type) {
      case kAvc1:
      case kAvc3:
      case kHvc1:
      case kHev1:
      case kAv01:
      case kMp4v: {
        // The configuration record of the codec is mandatory to describe the samples.
        description.supported = !description.codecSpecificData.empty();
        break;
      }
      case kVp08:
      case kVp09: {
        // VP8 and VP9 in MP4 need no configuration record.
        description.supported = true;
        break;
      }
      case kDvh1:
      case kDvhe: {
        description.supported = !description.codecSpecificData.empty();
        break;
      }
      case kS263: {
        description.supported = true;
        break;
      }
      default: {
        description.supported = false;
        break;
      }
    }
    if (description.dolbyVisionProfile > 0) {
      description.mimeType = MimeTypes::VIDEO_DOLBY_VISION;
      const char* prefix = description.dolbyVisionProfile == 5
                             ? "dvh1"
                             : description.dolbyVisionProfile == 9 ? "dvav" : "dvhe";
      description.codecs = std::string(prefix) + "." +
                           std::to_string(description.dolbyVisionProfile) + "." +
                           std::to_string(description.dolbyVisionLevel);
    }
    return;
  }

  if (size < 36) {
    return;
  }
  const uint32_t version = loadUInt16(data, offset + 16);
  description.channelCount = loadUInt16(data, offset + 24);
  description.sampleRate = static_cast<int>(loadUInt32(data, offset + 32) >> 16);
  uint64_t childrenOffset = offset + 36;
  if (version == 1) {
    // A QuickTime version 1 sample entry stores the packet layout of the stream before its child
    // boxes: the samples per packet, the bytes per packet, the bytes per frame and the bytes per
    // sample.
    if (size < 52) {
      return;
    }
    childrenOffset = offset + 52;
  } else if (version == 2) {
    // A version 2 sample entry stores the sample rate as a double precision value and the channel
    // count as a 32 bit value.
    if (size < 60) {
      return;
    }
    description.sampleRate = static_cast<int>(loadDouble(data, offset + 28));
    description.channelCount = static_cast<int>(loadUInt32(data, offset + 36));
    childrenOffset = offset + 60;
  }
  const uint64_t childrenSize = end > childrenOffset ? end - childrenOffset : 0;
  parseAudioSampleEntryChildren(data, childrenOffset, childrenSize, version, description);
  // The encoder delay and the padding of a QuickTime audio stream are declared by a recovery point
  // sample group, which pairs an sgpd box with an sbgp box.
  parseRollRecoveryGroup(data, childrenOffset, childrenSize, description);

  switch (type) {
    case kMp4a: {
      description.supported = description.sampleRate > 0 && description.channelCount > 0;
      break;
    }
    case kOpus:
    case kFlac:
    case kAlac: {
      description.supported = !description.codecSpecificData.empty() &&
                             description.sampleRate > 0 && description.channelCount > 0;
      break;
    }
    case kAc3:
    case kEc3:
    case kAc4: {
      description.supported = true;
      break;
    }
    case kSowt:
    case kTwos: {
      description.supported = description.sampleRate > 0 && description.channelCount > 0;
      break;
    }
    default: {
      description.supported = false;
      break;
    }
  }
}

void AVMp4Extractor::parseAudioSampleEntryChildren(AVDataBuffer& data, const uint64_t offset,
                                                   const uint64_t size, const uint32_t version,
                                                   SampleDescription& description) {
  const uint64_t end = offset + size;
  uint64_t cursor = offset;
  while (cursor + 8 <= end) {
    const BoxHeader box = readBoxHeader(data, cursor, end);
    if (!box.valid) {
      break;
    }
    switch (box.type) {
      case kEsds: {
        parseEsds(data, box.payloadOffset, box.payloadSize, description);
        break;
      }
      case kDOps: {
        // The Opus specific box is handed over to the decoder as it is stored.
        description.codecSpecificData.assign(1,
                                             loadBytes(data, box.payloadOffset, box.payloadSize));
        break;
      }
      case kDFla: {
        // The FLAC specific box starts with the version and flags of the full box, then the
        // metadata blocks, of which the stream information is the one the decoder needs.
        if (box.payloadSize > 4) {
          description.codecSpecificData.assign(
            1, loadBytes(data, box.payloadOffset + 4, box.payloadSize - 4));
        }
        break;
      }
      case kAlac: {
        const std::vector<uint8_t> config = loadBytes(data, box.payloadOffset, box.payloadSize);
        if (config.size() >= 24) {
          // The Apple lossless magic cookie: the channel count is the tenth byte and the sample
          // rate the four bytes that follow the average bitrate.
          if (description.channelCount <= 0) {
            description.channelCount = config[9];
          }
          if (description.sampleRate <= 0) {
            description.sampleRate = static_cast<int>(AVUtil::readUnsignedInt(config.data(), 20));
          }
        }
        description.codecSpecificData.assign(1, config);
        break;
      }
      case kWave: {
        // A QuickTime sound container: the real configuration boxes are nested in it.
        parseAudioSampleEntryChildren(data, box.payloadOffset, box.payloadSize, version,
                                      description);
        break;
      }
      case kBtrt: {
        if (box.payloadSize >= 12) {
          description.averageBitrate = static_cast<int>(loadUInt32(data, box.payloadOffset + 8));
        }
        break;
      }
      case kEnda: {
        // The endianness flag of a QuickTime audio sample entry: it does not change the format.
        break;
      }
      default: {
        // The sample group boxes are read by parseRollRecoveryGroup, the rest is skipped.
        break;
      }
    }
    if (box.nextOffset <= cursor) {
      break;
    }
    cursor = box.nextOffset;
  }
}

void AVMp4Extractor::parseVideoSampleEntryChildren(AVDataBuffer& data, const uint64_t offset,
                                                   const uint64_t size,
                                                   SampleDescription& description) {
  const uint64_t end = offset + size;
  uint64_t cursor = offset;
  while (cursor + 8 <= end) {
    const BoxHeader box = readBoxHeader(data, cursor, end);
    if (!box.valid) {
      break;
    }
    switch (box.type) {
      case kAvcC: {
        parseAvcC(data, box.payloadOffset, box.payloadSize, description);
        break;
      }
      case kHvcC: {
        parseHvcC(data, box.payloadOffset, box.payloadSize, description);
        break;
      }
      case kAv1C: {
        parseAv1C(data, box.payloadOffset, box.payloadSize, description);
        break;
      }
      case kEsds: {
        // The MPEG-4 visual sample entries store their configuration record in an esds box.
        parseEsds(data, box.payloadOffset, box.payloadSize, description);
        break;
      }
      case kBtrt: {
        if (box.payloadSize >= 12) {
          description.averageBitrate = static_cast<int>(loadUInt32(data, box.payloadOffset + 8));
        }
        break;
      }
      case kColr: {
        parseColr(data, box.payloadOffset, box.payloadSize, description);
        break;
      }
      case kMdcv: {
        parseMdcv(data, box.payloadOffset, box.payloadSize, description);
        break;
      }
      case kClli: {
        parseClli(data, box.payloadOffset, box.payloadSize, description);
        break;
      }
      case kDvcC:
      case kDvvC: {
        parseDolbyVisionConfig(data, box.payloadOffset, box.payloadSize, description);
        break;
      }
      case kD263: {
        parseD263(data, box.payloadOffset, box.payloadSize, description);
        break;
      }
      default: {
        // The pixel aspect ratio and clean aperture boxes (pasp, clap) are not needed.
        break;
      }
    }
    if (box.nextOffset <= cursor) {
      break;
    }
    cursor = box.nextOffset;
  }
}

void AVMp4Extractor::parseEsds(AVDataBuffer& data, const uint64_t offset, const uint64_t size,
                               SampleDescription& description) {
  const uint64_t end = offset + size;
  if (size < 6) {
    return;
  }
  uint64_t cursor = offset + 4; // the version and flags of the full box
  uint32_t tag = 0;
  uint64_t length = 0;
  if (!readDescriptorHeader(data, cursor, end, tag, length) || tag != kEsDescriptorTag) {
    return;
  }
  uint64_t descriptorEnd = cursor + length;
  if (descriptorEnd > end) {
    descriptorEnd = end;
  }
  if (cursor + 3 > descriptorEnd) {
    return;
  }
  // ES_ID (two bytes) then the flags that tell which of the optional fields follow.
  const uint32_t flags = loadUInt8(data, cursor + 2);
  cursor += 3;
  if ((flags & 0x80) != 0) {
    cursor += 2; // dependsOn_ES_ID
  }
  if ((flags & 0x40) != 0) {
    if (cursor < descriptorEnd) {
      cursor += 1 + loadUInt8(data, cursor); // the URL length and the URL itself
    }
  }
  if ((flags & 0x20) != 0) {
    cursor += 2; // OCR_ES_Id
  }

  uint32_t objectType = 0;
  while (cursor + 2 <= descriptorEnd) {
    uint32_t innerTag = 0;
    uint64_t innerLength = 0;
    if (!readDescriptorHeader(data, cursor, descriptorEnd, innerTag, innerLength)) {
      break;
    }
    if (innerLength > descriptorEnd - cursor) {
      break;
    }
    if (innerTag == kDecoderConfigDescriptorTag && innerLength >= 13) {
      objectType = loadUInt8(data, cursor);
      const uint32_t averageBitrate = loadUInt32(data, cursor + 9);
      if (averageBitrate > 0) {
        description.averageBitrate = static_cast<int>(averageBitrate);
      }
      const std::string mimeType = mimeTypeForObjectType(objectType);
      if (!mimeType.empty() &&
          (description.mimeType.empty() || description.mimeType == MimeTypes::AUDIO_AAC)) {
        description.mimeType = mimeType;
      }
      // The decoder specific information of an audio stream is its AudioSpecificConfig.
      uint64_t nested = cursor + 13;
      const uint64_t nestedEnd = cursor + innerLength;
      while (nested + 2 <= nestedEnd) {
        uint32_t nestedTag = 0;
        uint64_t nestedLength = 0;
        if (!readDescriptorHeader(data, nested, nestedEnd, nestedTag, nestedLength)) {
          break;
        }
        if (nestedTag == kDecoderSpecificInfoTag) {
          description.codecSpecificData.assign(1, loadBytes(data, nested, nestedLength));
          break;
        }
        nested += nestedLength;
      }
      break;
    }
    cursor += innerLength;
  }

  std::string codecs;
  if (objectType == kObjectTypeAac) {
    codecs = "mp4a." + hexByte(static_cast<int>(objectType));
  }
  if (!description.video && !description.codecSpecificData.empty()) {
    int sampleRate = 0;
    int channelCount = 0;
    int audioObjectType = 0;
    if (parseAudioSpecificConfig(description.codecSpecificData[0], sampleRate, channelCount,
                                 audioObjectType)) {
      if (description.sampleRate <= 0) {
        description.sampleRate = sampleRate;
      }
      if (description.channelCount <= 0) {
        description.channelCount = channelCount;
      }
      if (!codecs.empty() && audioObjectType > 0) {
        codecs += "." + std::to_string(audioObjectType);
      }
    }
  }
  if (!codecs.empty()) {
    description.codecs = codecs;
  }
}

bool AVMp4Extractor::parseAudioSpecificConfig(const std::vector<uint8_t>& config, int& sampleRate,
                                              int& channelCount, int& audioObjectType) {
  if (config.empty()) {
    return false;
  }
  auto data = std::make_shared<AVDataBuffer>(config.data(), static_cast<uint32_t>(config.size()));
  AVBitReader reader(data);
  if (!reader.canReadBits(5)) {
    return false;
  }
  int type = static_cast<int>(reader.readBits(5));
  if (type == 31) {
    if (!reader.canReadBits(6)) {
      return false;
    }
    type = 32 + static_cast<int>(reader.readBits(6));
  }
  if (!reader.canReadBits(4)) {
    return false;
  }
  const int frequencyIndex = static_cast<int>(reader.readBits(4));
  static const int kSampleRates[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                       22050, 16000, 12000, 11025, 8000,  7350};
  int rate = 0;
  if (frequencyIndex == 15) {
    if (!reader.canReadBits(24)) {
      return false;
    }
    rate = static_cast<int>(reader.readBits(24));
  } else if (frequencyIndex >= 0 && frequencyIndex < 13) {
    rate = kSampleRates[frequencyIndex];
  }
  if (!reader.canReadBits(4)) {
    return false;
  }
  const int channels = static_cast<int>(reader.readBits(4));
  sampleRate = rate;
  channelCount = channels;
  audioObjectType = type;
  return rate > 0 || channels > 0;
}

void AVMp4Extractor::parseAvcC(AVDataBuffer& data, const uint64_t offset, const uint64_t size,
                               SampleDescription& description) {
  const uint64_t end = offset + size;
  if (size < 7) {
    return;
  }
  const int profile = loadUInt8(data, offset + 1);
  const int compatibility = loadUInt8(data, offset + 2);
  const int level = loadUInt8(data, offset + 3);
  description.profile = profile;
  description.level = level;
  // The fifth byte declares the size of the length prefix of the NAL units of the samples, minus one.
  description.nalLengthSize = (loadUInt8(data, offset + 4) & 0x03) + 1;
  description.codecs = "avc1." + hexByte(profile) + hexByte(compatibility) + hexByte(level);

  description.codecSpecificData.clear();
  uint64_t cursor = offset + 5;
  const uint64_t spsCount = loadUInt8(data, cursor) & 0x1F;
  cursor += 1;
  for (uint64_t i = 0; i < spsCount && cursor + 2 <= end; ++i) {
    const uint64_t nalSize = loadUInt16(data, cursor);
    cursor += 2;
    if (nalSize == 0 || cursor + nalSize > end) {
      break;
    }
    // The decoder expects every parameter set to be prefixed by its one byte NAL length.
    std::vector<uint8_t> nal;
    nal.reserve(static_cast<size_t>(nalSize) + 1);
    nal.push_back(static_cast<uint8_t>(nalSize & 0xFF));
    const std::vector<uint8_t> payload = loadBytes(data, cursor, nalSize);
    nal.insert(nal.end(), payload.begin(), payload.end());
    description.codecSpecificData.push_back(std::move(nal));
    cursor += nalSize;
  }
  if (cursor + 1 > end) {
    return;
  }
  const uint64_t ppsCount = loadUInt8(data, cursor);
  cursor += 1;
  for (uint64_t i = 0; i < ppsCount && cursor + 2 <= end; ++i) {
    const uint64_t nalSize = loadUInt16(data, cursor);
    cursor += 2;
    if (nalSize == 0 || cursor + nalSize > end) {
      break;
    }
    std::vector<uint8_t> nal;
    nal.reserve(static_cast<size_t>(nalSize) + 1);
    nal.push_back(static_cast<uint8_t>(nalSize & 0xFF));
    const std::vector<uint8_t> payload = loadBytes(data, cursor, nalSize);
    nal.insert(nal.end(), payload.begin(), payload.end());
    description.codecSpecificData.push_back(std::move(nal));
    cursor += nalSize;
  }
}

void AVMp4Extractor::parseHvcC(AVDataBuffer& data, const uint64_t offset, const uint64_t size,
                               SampleDescription& description) {
  if (size < 23) {
    return;
  }
  // The whole configuration record is handed over to the decoder, as ExoPlayer does.
  description.codecSpecificData.assign(1, loadBytes(data, offset, size));
  // The 22nd byte declares the size of the length prefix of the NAL units of the samples, minus one.
  description.nalLengthSize = (loadUInt8(data, offset + 21) & 0x03) + 1;
  const int profileByte = loadUInt8(data, offset + 1);
  const int profileSpace = (profileByte >> 6) & 0x03;
  const int tierFlag = (profileByte >> 5) & 0x01;
  description.profile = profileByte & 0x1F;
  description.level = loadUInt8(data, offset + 12);
  // The RFC 6381 codec string of an HEVC stream, in the form that
  // CodecSpecificDataUtil.buildHevcCodecString produces.
  std::string codecs = "hvc1.";
  if (profileSpace > 0) {
    codecs.push_back(static_cast<char>('A' + profileSpace - 1));
  }
  codecs += std::to_string(description.profile);
  codecs.push_back('.');
  for (int i = 3; i >= 0; --i) {
    codecs += hexByte(loadUInt8(data, offset + 2 + static_cast<uint64_t>(i)));
  }
  codecs.push_back('.');
  codecs.push_back(tierFlag == 0 ? 'L' : 'H');
  codecs += std::to_string(description.level);
  description.codecs = codecs;
}

void AVMp4Extractor::parseDolbyVisionConfig(AVDataBuffer& data, const uint64_t offset,
  const uint64_t size, SampleDescription& description) {
  if (size < 5) {
    return;
  }
  const uint8_t profileAndLevelHigh = loadUInt8(data, offset + 2);
  description.dolbyVisionProfile = (profileAndLevelHigh >> 1) & 0x7F;
  description.dolbyVisionLevel = ((profileAndLevelHigh & 0x01) << 5) |
                                 ((loadUInt8(data, offset + 3) >> 3) & 0x1F);
}

void AVMp4Extractor::parseColr(AVDataBuffer& data, const uint64_t offset, const uint64_t size,
  SampleDescription& description) {
  if (size < 10) {
    return;
  }
  const uint32_t colorType = loadUInt32(data, offset);
  if (colorType != 0x6E636C78 && colorType != 0x6E636C63) { // nclx / nclc
    return;
  }
  const int primaries = loadUInt16(data, offset + 4);
  const int transfer = loadUInt16(data, offset + 6);
  const bool fullRange = size >= 11 && (loadUInt8(data, offset + 10) & 0x80) != 0;
  description.colorInfo.colorSpace = ColorInfo::isoColorPrimariesToColorSpace(primaries);
  description.colorInfo.colorTransfer =
    ColorInfo::isoTransferCharacteristicsToColorTransfer(transfer);
  description.colorInfo.colorRange =
    fullRange ? ColorInfo::RangeType::kRangeFull : ColorInfo::RangeType::kRangeLimited;
}

void AVMp4Extractor::parseMdcv(AVDataBuffer& data, const uint64_t offset, const uint64_t size,
  SampleDescription& description) {
  if (size < 24) {
    return;
  }
  if (description.colorInfo.hdrStaticInfo.size() < 25) {
    description.colorInfo.hdrStaticInfo.assign(25, 0);
  }
  auto& info = description.colorInfo.hdrStaticInfo;
  for (size_t i = 0; i < 8; ++i) {
    const uint16_t value = loadUInt16(data, offset + i * 2);
    info[1 + i * 2] = static_cast<uint8_t>(value);
    info[2 + i * 2] = static_cast<uint8_t>(value >> 8);
  }
  const uint32_t maxLuminance = loadUInt32(data, offset + 16);
  const uint32_t minLuminance = loadUInt32(data, offset + 20);
  const auto maxValue = static_cast<uint16_t>(
    std::min<uint32_t>(maxLuminance / 10000, std::numeric_limits<uint16_t>::max()));
  const auto minValue = static_cast<uint16_t>(
    std::min<uint32_t>(minLuminance / 10000, std::numeric_limits<uint16_t>::max()));
  info[17] = static_cast<uint8_t>(maxValue);
  info[18] = static_cast<uint8_t>(maxValue >> 8);
  info[19] = static_cast<uint8_t>(minValue);
  info[20] = static_cast<uint8_t>(minValue >> 8);
}

void AVMp4Extractor::parseClli(AVDataBuffer& data, const uint64_t offset, const uint64_t size,
  SampleDescription& description) {
  if (size < 4) {
    return;
  }
  if (description.colorInfo.hdrStaticInfo.size() < 25) {
    description.colorInfo.hdrStaticInfo.assign(25, 0);
  }
  auto& info = description.colorInfo.hdrStaticInfo;
  const uint16_t maxContentLightLevel = loadUInt16(data, offset);
  const uint16_t maxFrameAverageLightLevel = loadUInt16(data, offset + 2);
  info[21] = static_cast<uint8_t>(maxContentLightLevel);
  info[22] = static_cast<uint8_t>(maxContentLightLevel >> 8);
  info[23] = static_cast<uint8_t>(maxFrameAverageLightLevel);
  info[24] = static_cast<uint8_t>(maxFrameAverageLightLevel >> 8);
}

void AVMp4Extractor::parseD263(AVDataBuffer& data, const uint64_t offset, const uint64_t size,
  SampleDescription& description) {
  if (size < 7) {
    return;
  }
  description.level = loadUInt8(data, offset + 5);
  description.profile = loadUInt8(data, offset + 6);
  description.codecs = "s263." + std::to_string(description.profile) + "." +
                       std::to_string(description.level);
}

void AVMp4Extractor::parseAv1C(AVDataBuffer& data, const uint64_t offset, const uint64_t size,
                               SampleDescription& description) {
  if (size < 4) {
    return;
  }
  description.codecSpecificData.assign(1, loadBytes(data, offset, size));
  const int byte1 = loadUInt8(data, offset + 1);
  const int byte2 = loadUInt8(data, offset + 2);
  description.profile = (byte1 >> 5) & 0x07;
  description.level = byte1 & 0x1F;
  const bool highBitDepth = ((byte2 >> 6) & 0x01) != 0;
  const bool twelveBit = ((byte2 >> 5) & 0x01) != 0;
  const int bitDepth = highBitDepth ? (twelveBit ? 12 : 10) : 8;
  description.codecs = "av01." + twoDigits(description.profile) + "." +
                       twoDigits(description.level) + "M." + twoDigits(bitDepth);
}

void AVMp4Extractor::parseRollRecoveryGroup(AVDataBuffer& data, const uint64_t offset,
                                            const uint64_t size, SampleDescription& description) {
  int rollDistance = 0;
  bool hasRollDistance = false;
  uint32_t firstGroupIndex = 0;
  bool hasGroupIndex = false;

  const uint64_t end = offset + size;
  uint64_t cursor = offset;
  while (cursor + 8 <= end) {
    const BoxHeader box = readBoxHeader(data, cursor, end);
    if (!box.valid) {
      break;
    }
    if ((box.type == kSgpd || box.type == kSbgp) && box.payloadSize >= 12 &&
        loadUInt32(data, box.payloadOffset + 4) == kRollGroupingType) {
      const uint32_t version = loadUInt32(data, box.payloadOffset) >> 24;
      if (box.type == kSgpd) {
        // The sample group description declares the roll distance of every group.
        uint64_t field = box.payloadOffset + 8;
        uint64_t entryCount = 0;
        if (version == 1) {
          if (field + 8 > end) {
            break;
          }
          const uint64_t defaultLength = loadUInt32(data, field);
          entryCount = loadUInt32(data, field + 4);
          field += 8;
          if (defaultLength == 0) {
            field += 4; // the description length of the first entry
          }
        } else {
          if (field + 4 > end) {
            break;
          }
          entryCount = loadUInt32(data, field);
          field += 4;
        }
        if (entryCount > 0 && field + 2 <= end) {
          rollDistance = static_cast<int16_t>(loadUInt16(data, field));
          hasRollDistance = true;
        }
      } else {
        // The sample to group box tells which group the first sample belongs to.
        uint64_t field = box.payloadOffset + 8;
        if (version == 1) {
          field += 4; // the grouping type parameter
        }
        if (field + 8 <= end) {
          const uint64_t entryCount = loadUInt32(data, field);
          field += 4;
          if (entryCount > 0 && field + 8 <= end && loadUInt32(data, field) > 0) {
            firstGroupIndex = loadUInt32(data, field + 4);
            hasGroupIndex = true;
          }
        }
      }
    }
    if (box.nextOffset <= cursor) {
      break;
    }
    cursor = box.nextOffset;
  }

  if (!hasRollDistance || !hasGroupIndex || firstGroupIndex == 0) {
    return;
  }
  // A negative roll distance is the number of samples to trim from the start of the stream, a
  // positive one the number of samples to trim from its end.
  if (rollDistance < 0) {
    description.encoderDelay = -rollDistance;
  } else if (rollDistance > 0) {
    description.encoderPadding = rollDistance;
  }
}

void AVMp4Extractor::parseStts(AVDataBuffer& data, const uint64_t offset, const uint64_t size,
                               Track& track) {
  if (size < 8) {
    return;
  }
  const uint64_t entryCount = loadUInt32(data, offset + 4);
  if (entryCount > (size - 8) / 8) {
    return;
  }
  track.timeToSample.clear();
  track.timeToSample.reserve(static_cast<size_t>(entryCount));
  uint64_t cursor = offset + 8;
  for (uint64_t i = 0; i < entryCount; ++i) {
    TimeToSampleEntry entry;
    entry.sampleCount = loadUInt32(data, cursor);
    entry.sampleDelta = loadUInt32(data, cursor + 4);
    track.timeToSample.push_back(entry);
    cursor += 8;
  }
}

void AVMp4Extractor::parseCtts(AVDataBuffer& data, const uint64_t offset, const uint64_t size,
                               Track& track) {
  if (size < 8) {
    return;
  }
  const uint64_t entryCount = loadUInt32(data, offset + 4);
  if (entryCount > (size - 8) / 8) {
    return;
  }
  track.compositionOffsets.clear();
  track.compositionOffsets.reserve(static_cast<size_t>(entryCount));
  uint64_t cursor = offset + 8;
  for (uint64_t i = 0; i < entryCount; ++i) {
    CompositionOffsetEntry entry;
    entry.sampleCount = loadUInt32(data, cursor);
    entry.sampleOffset = loadInt32(data, cursor + 4);
    track.compositionOffsets.push_back(entry);
    cursor += 8;
  }
}

void AVMp4Extractor::parseStsc(AVDataBuffer& data, const uint64_t offset, const uint64_t size,
                               Track& track) {
  if (size < 8) {
    return;
  }
  const uint64_t entryCount = loadUInt32(data, offset + 4);
  if (entryCount > (size - 8) / 12) {
    return;
  }
  std::vector<SampleToChunkEntry> entries;
  entries.reserve(static_cast<size_t>(entryCount));
  uint64_t cursor = offset + 8;
  uint32_t previousFirstChunk = 0;
  for (uint64_t i = 0; i < entryCount; ++i) {
    SampleToChunkEntry entry;
    entry.firstChunk = loadUInt32(data, cursor);
    entry.samplesPerChunk = loadUInt32(data, cursor + 4);
    entry.sampleDescriptionIndex = loadUInt32(data, cursor + 8);
    // The entries must cover the chunks in order, and the first one must start at the first chunk.
    if (entry.firstChunk == 0 ||
        (i == 0 ? entry.firstChunk != 1 : entry.firstChunk <= previousFirstChunk)) {
      return;
    }
    if (entry.samplesPerChunk == 0) {
      return;
    }
    previousFirstChunk = entry.firstChunk;
    entries.push_back(entry);
    cursor += 12;
  }
  track.sampleToChunk = std::move(entries);
}

void AVMp4Extractor::parseStsz(AVDataBuffer& data, const uint64_t offset, const uint64_t size,
                               const bool compact, Track& track) {
  if (size < 12) {
    return;
  }
  if (!compact) {
    const uint32_t constantSize = loadUInt32(data, offset + 4);
    const uint64_t sampleCount = loadUInt32(data, offset + 8);
    if (constantSize != 0) {
      track.constantSampleSize = constantSize;
      track.sampleCount = sampleCount;
      track.sampleSizes.clear();
      return;
    }
    if (sampleCount > (size - 12) / 4 || sampleCount > kMaxSamplesPerTrack) {
      return;
    }
    track.constantSampleSize = 0;
    track.sampleCount = sampleCount;
    track.sampleSizes.resize(static_cast<size_t>(sampleCount));
    uint64_t cursor = offset + 12;
    for (uint64_t i = 0; i < sampleCount; ++i) {
      track.sampleSizes[static_cast<size_t>(i)] = loadUInt32(data, cursor);
      cursor += 4;
    }
    return;
  }

  const uint64_t fieldSize = loadUInt8(data, offset + 7);
  const uint64_t sampleCount = loadUInt32(data, offset + 8);
  uint64_t needed = 0;
  if (fieldSize == 4) {
    needed = (sampleCount + 1) / 2;
  } else if (fieldSize == 8) {
    needed = sampleCount;
  } else if (fieldSize == 16) {
    needed = sampleCount * 2;
  } else {
    return;
  }
  if (needed > size - 12 || sampleCount > kMaxSamplesPerTrack) {
    return;
  }
  track.constantSampleSize = 0;
  track.sampleCount = sampleCount;
  track.sampleSizes.resize(static_cast<size_t>(sampleCount));
  const uint64_t cursor = offset + 12;
  for (uint64_t i = 0; i < sampleCount; ++i) {
    uint32_t value = 0;
    if (fieldSize == 4) {
      const uint8_t byte = loadUInt8(data, cursor + i / 2);
      value = (i % 2 == 0) ? static_cast<uint32_t>(byte >> 4) : static_cast<uint32_t>(byte & 0x0F);
    } else if (fieldSize == 8) {
      value = loadUInt8(data, cursor + i);
    } else {
      value = loadUInt16(data, cursor + i * 2);
    }
    track.sampleSizes[static_cast<size_t>(i)] = value;
  }
}

void AVMp4Extractor::parseStco(AVDataBuffer& data, const uint64_t offset, const uint64_t size,
                               const bool is64Bit, Track& track) {
  if (size < 8) {
    return;
  }
  const uint64_t entryCount = loadUInt32(data, offset + 4);
  const uint64_t entrySize = is64Bit ? 8 : 4;
  if (entryCount > (size - 8) / entrySize) {
    return;
  }
  track.chunkOffsets.clear();
  track.chunkOffsets.reserve(static_cast<size_t>(entryCount));
  uint64_t cursor = offset + 8;
  for (uint64_t i = 0; i < entryCount; ++i) {
    track.chunkOffsets.push_back(is64Bit ? loadUInt64(data, cursor) : loadUInt32(data, cursor));
    cursor += entrySize;
  }
}

void AVMp4Extractor::parseStss(AVDataBuffer& data, const uint64_t offset, const uint64_t size,
                               Track& track) {
  if (size < 8) {
    return;
  }
  const uint64_t entryCount = loadUInt32(data, offset + 4);
  if (entryCount > (size - 8) / 4) {
    return;
  }
  track.syncSamples.clear();
  track.syncSamples.reserve(static_cast<size_t>(entryCount));
  uint64_t cursor = offset + 8;
  for (uint64_t i = 0; i < entryCount; ++i) {
    track.syncSamples.push_back(loadUInt32(data, cursor));
    cursor += 4;
  }
  // An empty sync sample box would mean that no sample can be decoded; such a file is treated as
  // one whose samples are all sync samples.
  track.hasSyncSamples = !track.syncSamples.empty();
}

void AVMp4Extractor::parseMvex(AVDataBuffer& data, const uint64_t offset, const uint64_t size) {
  fragmented_ = true;
  const uint64_t end = offset + size;
  uint64_t cursor = offset;
  while (cursor + 8 <= end) {
    const BoxHeader box = readBoxHeader(data, cursor, end);
    if (!box.valid) {
      break;
    }
    if (box.type == kTrex && box.payloadSize >= 24) {
      TrackExtendsDefaults defaults;
      const uint32_t trackId = loadUInt32(data, box.payloadOffset + 4);
      defaults.sampleDescriptionIndex = loadUInt32(data, box.payloadOffset + 8);
      defaults.sampleDuration = loadUInt32(data, box.payloadOffset + 12);
      defaults.sampleSize = loadUInt32(data, box.payloadOffset + 16);
      defaults.sampleFlags = loadUInt32(data, box.payloadOffset + 20);
      trackExtendsDefaults_[trackId] = defaults;
    }
    if (box.nextOffset <= cursor) {
      break;
    }
    cursor = box.nextOffset;
  }
}

void AVMp4Extractor::parseUdta(AVDataBuffer& data, const uint64_t offset, const uint64_t size) {
  const uint64_t end = offset + size;
  uint64_t cursor = offset;
  while (cursor + 8 <= end) {
    const BoxHeader box = readBoxHeader(data, cursor, end);
    if (!box.valid) {
      break;
    }
    if (box.type == kXyz) {
      // The ISO 6709 location is a 16 bit length, a 16 bit language code and the text.
      if (box.payloadSize >= 4) {
        const uint64_t length = std::min<uint64_t>(loadUInt16(data, box.payloadOffset),
                                                   box.payloadSize - 4);
        const std::string text(reinterpret_cast<const char*>(data.array()) + box.payloadOffset + 4,
                               static_cast<size_t>(length));
        double latitude = 0;
        double longitude = 0;
        if (std::sscanf(text.c_str(), "%lf%lf", &latitude, &longitude) == 2) {
          movieMetadata_.push_back(std::make_shared<Mp4LocationData>(latitude, longitude));
        }
      }
    } else if (box.type == kMeta) {
      parseMetaBox(data, box.payloadOffset, box.payloadSize);
    } else if (box.type == kChpl) {
      parseChpl(data, box.payloadOffset, box.payloadSize);
    }
    if (box.nextOffset <= cursor) {
      break;
    }
    cursor = box.nextOffset;
  }
}

void AVMp4Extractor::parseMetaBox(AVDataBuffer& data, const uint64_t offset, const uint64_t size) {
  // A meta box is a full box, i.e. its children follow its version and flags.
  if (size < 4) {
    return;
  }
  const uint64_t end = offset + size;
  std::vector<std::string> keys;
  uint64_t cursor = offset + 4;
  while (cursor + 8 <= end) {
    const BoxHeader box = readBoxHeader(data, cursor, end);
    if (!box.valid) {
      break;
    }
    if (box.type == kKeys) {
      parseKeys(data, box.payloadOffset, box.payloadSize, keys);
      break;
    }
    if (box.nextOffset <= cursor) {
      break;
    }
    cursor = box.nextOffset;
  }
  if (keys.empty()) {
    return;
  }
  cursor = offset + 4;
  while (cursor + 8 <= end) {
    const BoxHeader box = readBoxHeader(data, cursor, end);
    if (!box.valid) {
      break;
    }
    if (box.type == kIlst) {
      parseIlst(data, box.payloadOffset, box.payloadSize, keys);
      break;
    }
    if (box.nextOffset <= cursor) {
      break;
    }
    cursor = box.nextOffset;
  }
}

void AVMp4Extractor::parseKeys(AVDataBuffer& data, const uint64_t offset, const uint64_t size,
                               std::vector<std::string>& keys) {
  // A keys box is a full box: its version and flags are followed by the number of keys.
  if (size < 8) {
    return;
  }
  const uint64_t end = offset + size;
  const uint64_t count = loadUInt32(data, offset + 4);
  uint64_t cursor = offset + 8;
  for (uint64_t i = 0; i < count; ++i) {
    if (cursor + 8 > end) {
      break;
    }
    const uint64_t entrySize = loadUInt32(data, cursor);
    const uint32_t namespaceType = loadUInt32(data, cursor + 4);
    cursor += 8;
    if (entrySize < 8 || cursor + entrySize - 8 > end) {
      break;
    }
    if (namespaceType == 0x6D647461) {  // "mdta"
      keys.emplace_back(reinterpret_cast<const char*>(data.array()) + cursor,
                        static_cast<size_t>(entrySize - 8));
    }
    cursor += entrySize - 8;
  }
}

void AVMp4Extractor::parseIlst(AVDataBuffer& data, const uint64_t offset, const uint64_t size,
                               const std::vector<std::string>& keys) {
  const uint64_t end = offset + size;
  uint64_t cursor = offset;
  while (cursor + 8 <= end) {
    const BoxHeader item = readBoxHeader(data, cursor, end);
    if (!item.valid) {
      break;
    }
    // The type of an item is the one based index of its key in the keys box.
    if (item.type >= 1 && item.type <= keys.size()) {
      const uint64_t itemEnd = item.payloadOffset + item.payloadSize;
      uint64_t child = item.payloadOffset;
      while (child + 8 <= itemEnd) {
        const BoxHeader box = readBoxHeader(data, child, itemEnd);
        if (!box.valid) {
          break;
        }
        if (box.type == kData && box.payloadSize >= 8) {
          // A data box is a full box whose flags are the type of the value, followed by a locale.
          const uint32_t typeCode = loadUInt32(data, box.payloadOffset) & 0xFFFFFF;
          const uint64_t valueOffset = box.payloadOffset + 8;
          const uint64_t valueSize = box.payloadSize - 8;
          std::vector<uint8_t> value(data.array() + valueOffset, data.array() + valueOffset + valueSize);
          movieMetadata_.push_back(std::make_shared<MdtaMetadataEntry>(
            keys[item.type - 1], std::move(value), mdtaValueType(typeCode)));
        }
        if (box.nextOffset <= child) {
          break;
        }
        child = box.nextOffset;
      }
    }
    if (item.nextOffset <= cursor) {
      break;
    }
    cursor = item.nextOffset;
  }
}

void AVMp4Extractor::parseChpl(AVDataBuffer& data, const uint64_t offset, const uint64_t size) {
  // A chpl box is a full box: version and flags, a reserved word and the number of chapters.
  if (size < 9) {
    return;
  }
  const uint64_t end = offset + size;
  const uint64_t count = data.array()[offset + 8];
  uint64_t cursor = offset + 9;
  std::vector<Mp4Chapter> chapters;
  for (uint64_t i = 0; i < count; ++i) {
    if (cursor + 9 > end) {
      break;
    }
    const uint64_t time100Ns = loadUInt64(data, cursor);
    const uint64_t titleLength = data.array()[cursor + 8];
    cursor += 9;
    if (cursor + titleLength > end) {
      break;
    }
    chapters.emplace_back(static_cast<int64_t>(time100Ns / 10),
                          std::string(reinterpret_cast<const char*>(data.array()) + cursor,
                                      static_cast<size_t>(titleLength)));
    cursor += titleLength;
  }
  movieMetadata_.push_back(std::make_shared<Mp4ChaptersData>(std::move(chapters)));
}

void AVMp4Extractor::parseEmsg(AVDataBuffer& data, const uint64_t offset, const uint64_t size,
                               const uint64_t payloadPosition) {
  if (size < 4) {
    return;
  }
  const uint8_t version = loadUInt8(data, offset);
  size_t cursor = offset + 4;
  uint32_t timescale = 0;
  uint64_t presentationTime = 0;
  if (version == 0) {
    while (cursor < offset + size && loadUInt8(data, cursor) != 0) ++cursor;
    if (cursor >= offset + size) return;
    ++cursor;
    while (cursor < offset + size && loadUInt8(data, cursor) != 0) ++cursor;
    if (cursor + 16 > offset + size) return;
    ++cursor;
    timescale = loadUInt32(data, cursor);
    presentationTime = loadUInt32(data, cursor + 4);
  } else if (version == 1) {
    if (cursor + 20 > offset + size) return;
    timescale = loadUInt32(data, cursor);
    presentationTime = loadUInt64(data, cursor + 4);
  } else {
    return;
  }
  if (timescale == 0) {
    return;
  }
  const int64_t timeUs = AVUtil::scaleToUs(static_cast<int64_t>(presentationTime),
    static_cast<int64_t>(timescale));
  if (emsgOutput_ != nullptr) {
    // The movie box was already parsed and endTracks() ran; the reserved fragmented-movie track
    // accepts the sample directly, without a sample-table entry.
    auto buffer = std::make_shared<AVBlockBuffer>(static_cast<uint32_t>(size));
    buffer->put(data.array() + offset, static_cast<size_t>(size));
    buffer->position(0);
    buffer->limit(static_cast<size_t>(size));
    AVSample sample(emsgTrackIndex_, timeUs, static_cast<uint32_t>(AVSample::kFlagKeyFrame),
      std::move(buffer));
    emsgOutput_->sample(sample);
    return;
  }
  if (emsgTrackIndex_ < 0) {
    Track track;
    track.mediaType = MediaType::kMediaTypeMetadata;
    track.trackId = static_cast<uint32_t>(0x30000 | tracks_.size());
    track.timescale = 1000;
    SampleDescription description;
    description.metadata = true;
    description.supported = true;
    description.mimeType = MimeTypes::APPLICATION_EMSG;
    track.sampleDescriptions.push_back(std::move(description));
    track.supported = true;
    track.index = static_cast<int>(tracks_.size());
    track.maxInputSize = static_cast<int>(size);
    emsgTrackIndex_ = track.index;
    tracks_.push_back(std::move(track));
  }
  Track& track = tracks_[static_cast<size_t>(emsgTrackIndex_)];
  FragmentSample sample;
  sample.offset = payloadPosition;
  sample.size = static_cast<uint32_t>(size);
  sample.timeUs = timeUs;
  sample.flags = static_cast<uint32_t>(AVSample::kFlagKeyFrame);
  sample.sampleDescriptionIndex = 1;
  track.fragments.push_back(sample);
  if (static_cast<int>(size) > track.maxInputSize) {
    track.maxInputSize = static_cast<int>(size);
  }
  if (track.durationUs == Definitions::kTimeUnset || timeUs > track.durationUs) {
    track.durationUs = timeUs;
  }
}

void AVMp4Extractor::parseUuid(AVDataBuffer& data, const uint64_t offset, const uint64_t size) {
  if (size < 16 || std::memcmp(data.array() + offset, kXmpUuid, sizeof(kXmpUuid)) != 0) {
    return;
  }
  const uint64_t valueSize = size - 16;
  std::vector<uint8_t> xmp(data.array() + offset + 16, data.array() + offset + 16 + valueSize);
  movieMetadata_.push_back(std::make_shared<XmpData>(std::move(xmp)));
  applyMovieMetadata();
}

void AVMp4Extractor::applyMovieMetadata() {
  if (movieMetadata_.empty()) {
    return;
  }
  auto metadata = std::make_shared<Metadata>(std::vector<std::shared_ptr<Metadata::Entry>>{});
  metadata->appendedEntries(movieMetadata_);
  for (Track& track : tracks_) {
    if (track.publishedFormat == nullptr) {
      continue;
    }
    std::shared_ptr<MediaFormat> updated = track.publishedFormat->buildUpon();
    updated->setMetadata(metadata);
    track.publishedFormat = updated;
    if (track.output != nullptr) {
      track.output->format(updated);
    }
  }
}
void AVMp4Extractor::parseMoof(AVDataBuffer& data, const uint64_t offset, const uint64_t size,
                               const uint64_t moofStartPosition) {
  const uint64_t end = offset + size;
  uint64_t cursor = offset;
  while (cursor + 8 <= end) {
    const BoxHeader box = readBoxHeader(data, cursor, end);
    if (!box.valid) {
      break;
    }
    if (box.type == kTraf) {
      parseTraf(data, box.payloadOffset, box.payloadSize, moofStartPosition);
    }
    // The sequence number box of the fragment is not needed.
    if (box.nextOffset <= cursor) {
      break;
    }
    cursor = box.nextOffset;
  }
}

void AVMp4Extractor::parseTraf(AVDataBuffer& data, const uint64_t offset, const uint64_t size,
                               const uint64_t moofStartPosition) {
  const uint64_t end = offset + size;

  // The track fragment header declares the track, the defaults of its runs and the base position of
  // its samples.
  uint32_t trackId = 0;
  bool hasTrackId = false;
  bool hasBaseDataOffset = false;
  uint64_t baseDataOffset = moofStartPosition;
  uint32_t descriptionIndex = 0;
  uint32_t defaultDuration = 0;
  uint32_t defaultSize = 0;
  uint32_t defaultFlags = 0;

  uint64_t cursor = offset;
  while (cursor + 8 <= end) {
    const BoxHeader box = readBoxHeader(data, cursor, end);
    if (!box.valid) {
      break;
    }
    if (box.type == kTfhd) {
      if (box.payloadSize < 8) {
        return;
      }
      const uint32_t flags = loadUInt32(data, box.payloadOffset) & 0x00FFFFFF;
      trackId = loadUInt32(data, box.payloadOffset + 4);
      hasTrackId = true;
      uint64_t field = box.payloadOffset + 8;
      if ((flags & 0x000001) != 0 && field + 8 <= end) {
        baseDataOffset = loadUInt64(data, field);
        hasBaseDataOffset = true;
        field += 8;
      }
      if ((flags & 0x000002) != 0 && field + 4 <= end) {
        descriptionIndex = loadUInt32(data, field);
        field += 4;
      }
      if ((flags & 0x000008) != 0 && field + 4 <= end) {
        defaultDuration = loadUInt32(data, field);
        field += 4;
      }
      if ((flags & 0x000010) != 0 && field + 4 <= end) {
        defaultSize = loadUInt32(data, field);
        field += 4;
      }
      if ((flags & 0x000020) != 0 && field + 4 <= end) {
        defaultFlags = loadUInt32(data, field);
      }
      break;
    }
    if (box.nextOffset <= cursor) {
      break;
    }
    cursor = box.nextOffset;
  }
  if (!hasTrackId) {
    return;
  }
  Track* track = findTrack(trackId);
  if (track == nullptr || track->timescale <= 0) {
    // The fragment belongs to a track that is not exposed, e.g. a subtitle track.
    return;
  }

  const auto defaults = trackExtendsDefaults_.find(trackId);
  if (defaults != trackExtendsDefaults_.end()) {
    if (descriptionIndex == 0) {
      descriptionIndex = defaults->second.sampleDescriptionIndex;
    }
    if (defaultDuration == 0) {
      defaultDuration = defaults->second.sampleDuration;
    }
    if (defaultSize == 0) {
      defaultSize = defaults->second.sampleSize;
    }
    if (defaultFlags == 0) {
      defaultFlags = defaults->second.sampleFlags;
    }
  }
  if (descriptionIndex == 0) {
    descriptionIndex = 1;
  }
  if (!hasBaseDataOffset) {
    // ISO/IEC 14496-12: without a base data offset the samples are positioned relative to the start
    // of the movie fragment that contains them.
    baseDataOffset = moofStartPosition;
  }

  // The base media decode time of the fragment, which the runs continue.
  int64_t decodeTime = track->fragmentDecodeTimeKnown ? track->fragmentDecodeTime : 0;
  cursor = offset;
  while (cursor + 8 <= end) {
    const BoxHeader box = readBoxHeader(data, cursor, end);
    if (!box.valid) {
      break;
    }
    if (box.type == kTfdt && box.payloadSize >= 8) {
      const uint32_t version = loadUInt32(data, box.payloadOffset) >> 24;
      decodeTime = version == 1 ? static_cast<int64_t>(loadUInt64(data, box.payloadOffset + 4))
                                : static_cast<int64_t>(loadUInt32(data, box.payloadOffset + 4));
      break;
    }
    if (box.nextOffset <= cursor) {
      break;
    }
    cursor = box.nextOffset;
  }

  const int64_t editStart = editStartTicks(*track);
  const uint64_t inputLength = inputLength_ > 0 ? static_cast<uint64_t>(inputLength_) : 0;

  cursor = offset;
  while (cursor + 8 <= end) {
    const BoxHeader box = readBoxHeader(data, cursor, end);
    if (!box.valid) {
      break;
    }
    if (box.type == kTrun && box.payloadSize >= 8) {
      const uint32_t version = loadUInt32(data, box.payloadOffset) >> 24;
      const uint32_t flags = loadUInt32(data, box.payloadOffset) & 0x00FFFFFF;
      const uint64_t sampleCount = loadUInt32(data, box.payloadOffset + 4);
      uint64_t field = box.payloadOffset + 8;
      int64_t dataOffset = 0;
      if ((flags & 0x000001) != 0 && field + 4 <= end) {
        dataOffset = loadInt32(data, field);
        field += 4;
      }
      bool hasFirstSampleFlags = false;
      uint32_t firstSampleFlags = 0;
      if ((flags & 0x000004) != 0 && field + 4 <= end) {
        firstSampleFlags = loadUInt32(data, field);
        hasFirstSampleFlags = true;
        field += 4;
      }
      const bool hasDurations = (flags & 0x000100) != 0;
      const bool hasSizes = (flags & 0x000200) != 0;
      const bool hasSampleFlags = (flags & 0x000400) != 0;
      const bool hasCompositionOffsets = (flags & 0x000800) != 0;

      uint64_t perSample = 0;
      perSample += hasDurations ? 4 : 0;
      perSample += hasSizes ? 4 : 0;
      perSample += hasSampleFlags ? 4 : 0;
      perSample += hasCompositionOffsets ? 4 : 0;
      const bool fitsBox = perSample == 0 || sampleCount <= (end - field) / perSample;
      // Every sample needs at least one byte of media data, and the number of samples of one movie
      // fragment is bounded so that a malformed box cannot allocate an unbounded queue.
      const bool fitsInput = inputLength == 0 || sampleCount <= inputLength;
      if (sampleCount <= kMaxFragmentSamples && fitsBox && fitsInput) {
        int64_t sampleDecodeTime = decodeTime;
        uint64_t offsetInRun = 0;
        for (uint64_t i = 0; i < sampleCount; ++i) {
          uint32_t duration = defaultDuration;
          uint32_t size = defaultSize;
          uint32_t sampleFlags = defaultFlags;
          int64_t compositionOffset = 0;
          if (hasDurations) {
            duration = loadUInt32(data, field);
            field += 4;
          }
          if (hasSizes) {
            size = loadUInt32(data, field);
            field += 4;
          }
          if (hasSampleFlags) {
            sampleFlags = loadUInt32(data, field);
            field += 4;
          }
          if (hasCompositionOffsets) {
            const int32_t raw = loadInt32(data, field);
            field += 4;
            compositionOffset =
              version == 0 ? static_cast<int64_t>(static_cast<uint32_t>(raw))
                           : static_cast<int64_t>(raw);
          }
          if (i == 0 && hasFirstSampleFlags) {
            sampleFlags = firstSampleFlags;
          }
          const int64_t sampleOffset =
            static_cast<int64_t>(baseDataOffset) + dataOffset + static_cast<int64_t>(offsetInRun);
          const int64_t composedTicks = sampleDecodeTime + compositionOffset;
          if (sampleOffset >= 0 &&
              (inputLength == 0 || static_cast<uint64_t>(sampleOffset) < inputLength)) {
            FragmentSample sample;
            sample.offset = static_cast<uint64_t>(sampleOffset);
            sample.size = size;
            sample.sampleDescriptionIndex = descriptionIndex;
            sample.timeUs = AVUtil::scaleToUs(composedTicks - editStart, track->timescale);
            sample.flags = (sampleFlags & kSampleFlagIsNonSync) == 0
                             ? static_cast<uint32_t>(AVSample::kFlagKeyFrame)
                             : static_cast<uint32_t>(AVSample::kFlagNone);
            // The samples that the edit list trims are skipped, their bytes still count.
            if (composedTicks >= editStart) {
              track->fragments.push_back(sample);
            }
            if (size > static_cast<uint32_t>(track->maxInputSize)) {
              track->maxInputSize = static_cast<int>(size);
            }
          }
          offsetInRun += size;
          sampleDecodeTime += static_cast<int64_t>(duration);
        }
        decodeTime = sampleDecodeTime;
      }
    }
    if (box.nextOffset <= cursor) {
      break;
    }
    cursor = box.nextOffset;
  }

  track->fragmentDecodeTime = decodeTime;
  track->fragmentDecodeTimeKnown = true;
}

// ==================================================================================================
// Track and format management
// ==================================================================================================

AVMp4Extractor::Track* AVMp4Extractor::findTrack(const uint32_t trackId) {
  for (Track& track : tracks_) {
    if (track.trackId == trackId) {
      return &track;
    }
  }
  return nullptr;
}

void AVMp4Extractor::createTracksAndSeekMap() {
  if (output_ == nullptr) {
    return;
  }
  if (fragmented_ && emsgTrackIndex_ < 0) {
    // A fragmented movie can carry top-level emsg boxes after the moov. Reserve the metadata
    // track before endTracks(); parseEmsg() feeds it directly when it sees the boxes.
    Track track;
    track.mediaType = MediaType::kMediaTypeMetadata;
    track.trackId = static_cast<uint32_t>(0x30000 | tracks_.size());
    track.timescale = 1000;
    SampleDescription description;
    description.metadata = true;
    description.supported = true;
    description.mimeType = MimeTypes::APPLICATION_EMSG;
    track.sampleDescriptions.push_back(std::move(description));
    track.supported = true;
    track.index = static_cast<int>(tracks_.size());
    emsgTrackIndex_ = track.index;
    tracks_.push_back(std::move(track));
  }
  for (Track& track : tracks_) {
    if (track.output != nullptr || track.index < 0 || !track.supported) {
      continue;
    }
    track.output = output_->track(track.index, track.mediaType, AVTrackOutput::emptyConsumer());
    if (track.index == emsgTrackIndex_) {
      emsgOutput_ = track.output;
    }
    // The format of a track is known from the sample descriptions the movie box declares, and it is
    // declared before the tracks end: a caller is entitled to have the format of every track once
    // `endTracks` was reported, and a media period cannot prepare a track whose format it does not
    // have. Waiting for the first sample to publish it leaves the period unprepared until then, and
    // never prepares it for a track whose samples are never read. The first sample description is the
    // one the samples use unless their chunk says otherwise, and a sample that uses another one
    // publishes it when it is read.
    publishFormat(track, /* sampleDescriptionIndex= */ 1);
    if (track.mediaType == MediaType::kMediaTypeVideo && !track.sampleDescriptions.empty()) {
      const std::string& mimeType = track.sampleDescriptions.front().mimeType;
      if (mimeType == MimeTypes::VIDEO_H264 || mimeType == MimeTypes::VIDEO_H265) {
        // The captions of an H.264/H.265 sample are SEI units inside the video sample, so the track
        // gets a sibling caption track that receives one sample per access unit with caption data.
        track.captionOutput = output_->track(0x10000 | track.index, MediaType::kMediaTypeSubtitle,
          AVTrackOutput::emptyConsumer());
        auto format = MediaFormat::CreateSubtitleFormat(MimeTypes::APPLICATION_CEA708);
        format->setTrackId(static_cast<int>(0x10000 | track.index));
        format->setContainerMimeType(MimeTypes::APPLICATION_MP4);
        format->setSubsampleOffsetUs(MediaFormat::OFFSET_SAMPLE_RELATIVE);
        format->setCueReplacementBehavior(CueReplacementBehavior::kReplace);
        format->setRoleFlags(kRoleFlagSubtitle);
        // The captions are still part of the video samples, so the track is derived and a muxer
        // skips it rather than writing the same captions twice.
        format->setDerivedTrack(true);
        track.captionOutput->format(format);
      }
    }
  }
  emitSeekMap();
  // Every track of the movie is known once the movie box was parsed, including the tracks that
  // were skipped because their codec is not supported.
  output_->endTracks();
}

void AVMp4Extractor::emitSeekMap() {
  if (output_ == nullptr || seekMapEmitted_) {
    return;
  }
  seekMapEmitted_ = true;

  int64_t durationUs = movieDurationUs_;
  if (durationUs == Definitions::kTimeUnset || durationUs <= 0) {
    durationUs = 0;
    for (const Track& track : tracks_) {
      if (track.durationUs != Definitions::kTimeUnset && track.durationUs > durationUs) {
        durationUs = track.durationUs;
      }
    }
  }

  const Track* seekTrack = findSeekTrack();
  if (seekTrack == nullptr) {
    output_->seekMap(std::make_shared<AVUnseekableSeekMap>(durationUs));
    return;
  }
  std::vector<AVSeekMap::SeekPoint> counted;
  const uint64_t candidates = forEachSeekPoint(*seekTrack, false, 1, counted);
  if (candidates == 0) {
    output_->seekMap(std::make_shared<AVUnseekableSeekMap>(durationUs));
    return;
  }
  uint64_t stride = 1;
  if (candidates > kMaxSeekPoints) {
    // The seek points are spread over the whole container rather than truncated, so that seeking
    // stays accurate at the end of a long movie.
    stride = (candidates + kMaxSeekPoints - 1) / kMaxSeekPoints;
  }
  std::vector<AVSeekMap::SeekPoint> points;
  points.reserve(static_cast<size_t>(candidates / stride + 1));
  forEachSeekPoint(*seekTrack, true, stride, points);
  if (points.empty()) {
    output_->seekMap(std::make_shared<AVUnseekableSeekMap>(durationUs));
    return;
  }
  output_->seekMap(std::make_shared<AVIndexSeekMap>(std::move(points), durationUs, true));
}

uint64_t AVMp4Extractor::forEachSeekPoint(const Track& track, const bool collect,
                                          const uint64_t stride,
                                          std::vector<AVSeekMap::SeekPoint>& points) {
  if (!track.sampleTableValid || track.timescale <= 0 || track.sampleCount == 0 ||
      track.chunkOffsets.empty() || track.sampleToChunk.empty()) {
    return 0;
  }
  const int64_t editStart = editStartTicks(track);
  const uint64_t period = stride == 0 ? 1 : stride;

  uint64_t candidates = 0;
  size_t timeEntryIndex = 0;
  uint64_t timeEntryUsed = 0;
  int64_t ticks = 0;
  size_t chunkEntryIndex = 0;
  uint64_t chunkIndex = 0;
  uint64_t samplesPerChunk = track.sampleToChunk[0].samplesPerChunk;
  uint64_t offset = track.chunkOffsets[0];
  uint64_t index = 0;

  while (index < track.sampleCount && chunkIndex < track.chunkOffsets.size()) {
    for (uint64_t i = 0; i < samplesPerChunk && index < track.sampleCount; ++i, ++index) {
      const bool isChunkStart = i == 0;
      if (track.hasSyncSamples ? isSyncSample(track, index) : isChunkStart) {
        if (collect && (candidates % period) == 0) {
          points.push_back(AVSeekMap::SeekPoint(
            AVUtil::scaleToUs(ticks - editStart, track.timescale), static_cast<long>(offset)));
        }
        ++candidates;
      }
      // Move the decode time cursor past this sample.
      if (timeEntryIndex < track.timeToSample.size()) {
        const TimeToSampleEntry& entry = track.timeToSample[timeEntryIndex];
        ticks += static_cast<int64_t>(entry.sampleDelta);
        timeEntryUsed += 1;
        if (timeEntryUsed >= entry.sampleCount) {
          timeEntryIndex += 1;
          timeEntryUsed = 0;
        }
      }
      offset += sampleSize(track, index);
    }
    ++chunkIndex;
    if (chunkIndex < track.chunkOffsets.size()) {
      while (chunkEntryIndex + 1 < track.sampleToChunk.size() &&
             static_cast<uint64_t>(track.sampleToChunk[chunkEntryIndex + 1].firstChunk) - 1 <=
               chunkIndex) {
        chunkEntryIndex += 1;
      }
      samplesPerChunk = track.sampleToChunk[chunkEntryIndex].samplesPerChunk;
      offset = track.chunkOffsets[chunkIndex];
    }
  }
  return candidates;
}

const AVMp4Extractor::Track* AVMp4Extractor::findSeekTrack() const {
  const Track* fallback = nullptr;
  for (const Track& track : tracks_) {
    if (!track.sampleTableValid || track.sampleCount == 0) {
      continue;
    }
    if (track.mediaType == MediaType::kMediaTypeVideo) {
      return &track;
    }
    if (fallback == nullptr) {
      fallback = &track;
    }
  }
  return fallback;
}

std::shared_ptr<MediaFormat>
AVMp4Extractor::buildFormat(const Track& track, const SampleDescription& description) const {
  std::shared_ptr<MediaFormat> format;
  if (description.metadata) {
    format = MediaFormat::CreateSubtitleFormat(description.mimeType);
    format->setSubsampleOffsetUs(0);
  } else if (description.subtitle) {
    format = MediaFormat::CreateSubtitleFormat(description.mimeType);
    format->setSubsampleOffsetUs(description.subsampleOffsetUs);
    format->setCueReplacementBehavior(cueReplacementBehaviorForMimeType(description.mimeType));
    format->setSelectionFlags(MediaSelectionFlags::kMediaSelectionDefault);
    format->setRoleFlags(kRoleFlagSubtitle);
  } else if (description.video) {
    format = MediaFormat::CreateVideoFormat(description.mimeType, description.width,
                                            description.height);
    format->setPixelFormat(PixelFormat::kFormatI420);
    if (track.rotation != 0) {
      format->setRotation(track.rotation);
    }
    if (description.profile > 0) {
      format->setProfile(description.profile);
    }
    if (description.level > 0) {
      format->setLevel(description.level);
    }
    // The `stts` of the track declares how long its samples are: the most frequent of them is the
    // duration of a frame, which is the rate the composition of an asset schedules its frames with.
    // A fragmented movie that did not declare its sample tables yet, or a track of a variable frame
    // rate whose most frequent duration is not representative, leaves the rate unset rather than
    // reporting one the container did not declare.
    if (track.timescale > 0) {
      const TimeToSampleEntry* frameEntry = nullptr;
      for (const TimeToSampleEntry& entry : track.timeToSample) {
        if (entry.sampleDelta == 0) {
          continue;
        }
        if (frameEntry == nullptr || entry.sampleCount > frameEntry->sampleCount) {
          frameEntry = &entry;
        }
      }
      if (frameEntry != nullptr) {
        const double rate = static_cast<double>(track.timescale) / frameEntry->sampleDelta;
        if (rate >= 1.0 && rate <= 1000.0) {
          format->setFrameRate(static_cast<int>(std::lround(rate)));
        }
      }
    }
  } else {
    format = MediaFormat::CreateAudioFormat(description.mimeType, description.sampleRate,
                                            description.channelCount);
    if (description.encoderDelay > 0) {
      format->setEncoderDelay(description.encoderDelay);
    }
    if (description.encoderPadding > 0) {
      format->setEncoderPadding(description.encoderPadding);
    }
  }
  format->setContainerMimeType(description.subtitle || description.metadata
                                 ? MimeTypes::APPLICATION_MP4
                                 : (description.video ? MimeTypes::VIDEO_MP4 : MimeTypes::AUDIO_MP4));
  format->setTrackId(static_cast<int>(track.trackId));
  format->setDurationUs(track.durationUs);
  if (!track.language.empty()) {
    format->setLanguage(track.language);
  } else if (!description.captionLanguage.empty()) {
    format->setLanguage(description.captionLanguage);
  }
  if (description.captionServiceNumber > 0) {
    format->setCaptionServiceNumber(description.captionServiceNumber);
  }
  uint32_t roleFlags = format->getRoleFlags();
  if (description.captionServiceNumber > 0) {
    roleFlags |= kRoleFlagCaption;
  }
  if (description.captionEasyReader) {
    roleFlags |= kRoleFlagEasyToRead;
  }
  format->setRoleFlags(roleFlags);
  // The largest sample of the track, which the sample tables or the movie fragments declared.
  if (track.maxInputSize > 0) {
    format->setMaxInputSize(track.maxInputSize);
  }
  if (description.averageBitrate > 0) {
    format->setAverageBitrate(description.averageBitrate);
  }
  if (!description.codecs.empty()) {
    format->setCodecs(description.codecs);
  }
  if (description.video &&
      (description.colorInfo.colorSpace != ColorInfo::SpaceType::kSpaceUnknown ||
       description.colorInfo.colorTransfer != ColorInfo::TransferType::kTransferUnknown ||
       description.colorInfo.colorRange != ColorInfo::RangeType::kRangeUnknown ||
       !description.colorInfo.hdrStaticInfo.empty())) {
    format->setColorInfo(description.colorInfo);
  }
  for (size_t i = 0; i < description.codecSpecificData.size(); ++i) {
    format->setCodecSpecificData(static_cast<int>(i), description.codecSpecificData[i]);
  }
  if (!movieMetadata_.empty()) {
    // The Java extractor attaches the file-level metadata to the format of every track.
    auto metadata = std::make_shared<Metadata>(std::vector<std::shared_ptr<Metadata::Entry>>{});
    metadata->appendedEntries(movieMetadata_);
    format->setMetadata(metadata);
  }
  return format;
}

void AVMp4Extractor::emitCaptionSample(Track& track, const std::vector<uint8_t>& ccData,
    const int64_t timeUs) {
  if (track.captionOutput == nullptr || ccData.empty()) {
    return;
  }
  auto buffer = std::make_shared<AVBlockBuffer>(static_cast<uint32_t>(ccData.size()));
  buffer->put(ccData.data(), ccData.size());
  buffer->position(0);
  buffer->limit(ccData.size());
  AVSample sample(track.captionOutput->id(), timeUs, AVSample::kFlagKeyFrame, std::move(buffer));
  track.captionOutput->sample(sample);
}

void AVMp4Extractor::publishFormat(Track& track, const uint32_t sampleDescriptionIndex) {
  if (track.publishedFormat != nullptr && track.publishedDescriptionIndex == sampleDescriptionIndex) {
    return;
  }
  const size_t index = sampleDescriptionIndex > 0 ? sampleDescriptionIndex - 1 : 0;
  if (index >= track.sampleDescriptions.size()) {
    return;
  }
  const SampleDescription& description = track.sampleDescriptions[index];
  if (!description.supported) {
    return;
  }
  track.publishedFormat = buildFormat(track, description);
  track.publishedDescriptionIndex = sampleDescriptionIndex;
  if (track.output != nullptr) {
    track.output->format(track.publishedFormat);
  }
}

int AVMp4Extractor::nalLengthSize(const Track& track, const uint32_t sampleDescriptionIndex) const {
  const size_t index = sampleDescriptionIndex > 0 ? sampleDescriptionIndex - 1 : 0;
  if (index >= track.sampleDescriptions.size()) {
    return 0;
  }
  // The size of the length prefix is declared by the configuration record of the entry, which the
  // sample description parsed while it read its `stsd` box.
  return track.sampleDescriptions[index].nalLengthSize;
}

std::vector<uint8_t> AVMp4Extractor::collectParameterSets(const Track& track,
                                                         const uint32_t sampleDescriptionIndex) const {
  std::vector<uint8_t> parameterSets;
  const size_t index = sampleDescriptionIndex > 0 ? sampleDescriptionIndex - 1 : 0;
  if (index >= track.sampleDescriptions.size()) {
    return parameterSets;
  }
  const SampleDescription& description = track.sampleDescriptions[index];
  if (description.mimeType == MimeTypes::VIDEO_H265) {
    // The codec specific data of an HEVC entry is its configuration record: a 23 byte header and an
    // array of parameter set NAL units per type, each of them prefixed by its length. A key frame
    // carries the VPS, SPS and PPS, which is what the platform decoders read from an Annex B sample.
    if (description.codecSpecificData.empty()) {
      return parameterSets;
    }
    const std::vector<uint8_t>& record = description.codecSpecificData[0];
    constexpr size_t kConfigurationHeaderSize = 23;
    if (record.size() <= kConfigurationHeaderSize || record[0] != 1) {
      return parameterSets;
    }
    size_t offset = kConfigurationHeaderSize;
    const size_t numArrays = record[22];
    for (size_t array = 0; array < numArrays; ++array) {
      if (offset + 3 > record.size()) {
        return parameterSets;
      }
      const auto nalType = static_cast<uint8_t>(record[offset] & 0x3F);
      const size_t count = (static_cast<size_t>(record[offset + 1]) << 8) | record[offset + 2];
      offset += 3;
      for (size_t nal = 0; nal < count; ++nal) {
        if (offset + 2 > record.size()) {
          return parameterSets;
        }
        const size_t length = (static_cast<size_t>(record[offset]) << 8) | record[offset + 1];
        offset += 2;
        if (offset + length > record.size()) {
          return parameterSets;
        }
        if (nalType == 32 || nalType == 33 || nalType == 34) {
          parameterSets.insert(parameterSets.end(), { 0, 0, 0, 1 });
          parameterSets.insert(parameterSets.end(), record.begin() + static_cast<ptrdiff_t>(offset),
            record.begin() + static_cast<ptrdiff_t>(offset + length));
        }
        offset += length;
      }
    }
    return parameterSets;
  }
  if (description.mimeType != MimeTypes::VIDEO_H264) {
    return parameterSets;
  }
  for (const std::vector<uint8_t>& parameterSet : description.codecSpecificData) {
    // The codec specific data of an AVC entry holds every parameter set prefixed by its one byte length.
    if (parameterSet.size() < 2) {
      continue;
    }
    const size_t unitSize = parameterSet[0];
    if (unitSize + 1 != parameterSet.size()) {
      continue;
    }
    parameterSets.insert(parameterSets.end(), {0, 0, 0, 1});
    parameterSets.insert(parameterSets.end(), parameterSet.begin() + 1, parameterSet.end());
  }
  return parameterSets;
}

std::shared_ptr<AVBlockBuffer> AVMp4Extractor::toAnnexBIfNeeded(std::shared_ptr<AVBlockBuffer> sample,
                                                                const Track& track,
                                                                const uint32_t sampleDescriptionIndex,
                                                                const bool keyFrame) const {
  const int lengthSize = nalLengthSize(track, sampleDescriptionIndex);
  if (lengthSize == 0) {
    return sample;
  }
  const size_t size = static_cast<size_t>(sample->limit());
  const size_t convertedSize = AVUtil::annexBSize(sample->array(), size, lengthSize);
  if (convertedSize == 0) {
    return sample;
  }
  // The parameter sets are prepended to a key frame, as the `h264_mp4toannexb` filter does: a decoder
  // that starts reading at one is given them in band.
  const std::vector<uint8_t> parameterSets =
      keyFrame ? collectParameterSets(track, sampleDescriptionIndex) : std::vector<uint8_t>();
  auto converted =
      std::make_shared<AVBlockBuffer>(static_cast<uint32_t>(convertedSize + parameterSets.size()));
  if (!parameterSets.empty()) {
    std::copy(parameterSets.begin(), parameterSets.end(), converted->array());
  }
  AVUtil::toAnnexB(sample->array(), size, lengthSize, converted->array() + parameterSets.size());
  converted->position(0);
  converted->limit(convertedSize + parameterSets.size());
  return converted;
}

void AVMp4Extractor::finalizeTrack(Track& track) {
  bool valid = track.sampleCount > 0 && track.sampleCount <= kMaxSamplesPerTrack &&
               track.timescale > 0 && !track.chunkOffsets.empty() &&
               !track.sampleToChunk.empty() && !track.timeToSample.empty();
  uint64_t tableSampleCount = 0;
  for (const TimeToSampleEntry& entry : track.timeToSample) {
    tableSampleCount += entry.sampleCount;
  }
  if (valid && tableSampleCount != track.sampleCount) {
    valid = false;
  }
  if (valid && track.constantSampleSize == 0 && track.sampleSizes.size() < track.sampleCount) {
    valid = false;
  }
  if (valid) {
    // The sample to chunk entries must map every sample of the track, and the last chunk they cover
    // must exist.
    uint64_t mapped = 0;
    uint64_t chunk = 0;
    for (size_t i = 0; i < track.sampleToChunk.size(); ++i) {
      const uint64_t next =
        i + 1 < track.sampleToChunk.size()
          ? static_cast<uint64_t>(track.sampleToChunk[i + 1].firstChunk) - 1
          : track.chunkOffsets.size();
      if (next > track.chunkOffsets.size() || next <= chunk) {
        valid = false;
        break;
      }
      mapped += (next - chunk) * track.sampleToChunk[i].samplesPerChunk;
      chunk = next;
    }
    if (valid && mapped < track.sampleCount) {
      valid = false;
    }
  }
  if (valid && inputLength_ > 0) {
    const uint64_t length = static_cast<uint64_t>(inputLength_);
    for (const uint64_t chunkOffset : track.chunkOffsets) {
      if (chunkOffset >= length) {
        valid = false;
        break;
      }
    }
  }
  track.sampleTableValid = valid;

  int maxInputSize = 0;
  if (track.constantSampleSize != 0) {
    maxInputSize = static_cast<int>(track.constantSampleSize);
  }
  for (const uint32_t size : track.sampleSizes) {
    if (size > static_cast<uint32_t>(maxInputSize)) {
      maxInputSize = static_cast<int>(size);
    }
  }
  track.maxInputSize = maxInputSize;

  if ((track.durationUs == Definitions::kTimeUnset || track.durationUs <= 0) &&
      track.timescale > 0) {
    int64_t total = 0;
    uint64_t counted = 0;
    for (const TimeToSampleEntry& entry : track.timeToSample) {
      if (counted >= track.sampleCount) {
        break;
      }
      const uint64_t count = std::min<uint64_t>(entry.sampleCount, track.sampleCount - counted);
      total += static_cast<int64_t>(count) * static_cast<int64_t>(entry.sampleDelta);
      counted += count;
    }
    track.durationUs = AVUtil::scaleToUs(total, track.timescale);
  }
}

// ==================================================================================================
// Sample reading
// ==================================================================================================

int64_t AVMp4Extractor::editStartTicks(const Track& track) {
  for (const EditListEntry& entry : track.editList) {
    if (entry.mediaTime >= 0) {
      return entry.mediaTime;
    }
  }
  return 0;
}

bool AVMp4Extractor::readSample(AVExtractorInput& input) {
  while (true) {
    const long currentPosition = input.getPosition();
    const uint64_t position = currentPosition > 0 ? static_cast<uint64_t>(currentPosition) : 0;
    Track* best = nullptr;
    uint64_t bestOffset = 0;

    for (Track& track : tracks_) {
      if (track.output == nullptr) {
        // A metadata track discovered before the movie box has no output yet; it is read after
        // createTracksAndSeekMap() installs one.
        continue;
      }
      updateCurrentSample(track);
      if (!track.currentSampleValid) {
        continue;
      }
      if (best == nullptr || track.currentSampleOffset < bestOffset) {
        best = &track;
        bestOffset = track.currentSampleOffset;
      }
    }
    if (best == nullptr) {
      return false;
    }

    if (bestOffset > position) {
      // The sample that comes first in the file is ahead of the input: skip to it, with a bounded
      // number of bytes per call.
      const uint64_t gap = bestOffset - position;
      const uint64_t step = std::min<uint64_t>(gap, static_cast<uint64_t>(kMaxSkipStep));
      if (!input.skipFully(static_cast<long>(step))) {
        endOfInput_ = true;
        return false;
      }
      return true;
    }

    if (bestOffset < position) {
      // The sample is behind the input, which is the case of a movie whose box follows the media data
      // it describes: the samples were only located once the box was parsed, so the input has to be
      // moved back to them. Giving up on them instead left the media of such a file undelivered, and
      // the playback of it stuck without anything to play.
      if (!input.setPosition(static_cast<long>(bestOffset), false)) {
        // The container cannot be seeked, so the samples that lie behind the input are out of reach:
        // the remaining ones are given up rather than delivered as garbage.
        best->nextSampleIndex = best->sampleCount;
        best->fragments.clear();
        best->currentSampleValid = false;
        continue;
      }
      return true;
    }

    // The sample starts at the current position.
    // A sample that the edit list trims is still delivered: it has to be decoded, because the samples
    // that follow it are predicted from it, but it is not rendered. Dropping it left the decoder
    // without its reference frames, and the whole track decoded to nothing.
    auto sampleFlags = best->currentSampleFlags;
    const bool trimmedByEditList = !best->currentFromFragment &&
                                   best->decodeTime + best->compositionOffset < editStartTicks(*best);
    if (trimmedByEditList) {
      sampleFlags |= static_cast<uint32_t>(AVSample::kFlagDiscard);
    }
    if (best->currentSampleSize == 0 || best->currentSampleSize > kMaxSampleSize ||
        (inputLength_ > 0 && best->currentSampleSize > static_cast<uint64_t>(inputLength_))) {
      advanceSample(*best);
      continue;
    }

    publishFormat(*best, best->currentDescriptionIndex);
    auto buffer = std::make_shared<AVBlockBuffer>(best->currentSampleSize);
    if (!input.readFully(buffer->array(), 0, best->currentSampleSize)) {
      endOfInput_ = true;
      return false;
    }
    buffer->position(0);
    buffer->limit(best->currentSampleSize);
    buffer = toAnnexBIfNeeded(std::move(buffer), *best, best->currentDescriptionIndex,
        (sampleFlags & AVSample::kFlagKeyFrame) != 0);
    const size_t descriptionIndex =
      best->currentDescriptionIndex > 0 ? best->currentDescriptionIndex - 1 : 0;
    if (descriptionIndex < best->sampleDescriptions.size() &&
        best->sampleDescriptions[descriptionIndex].stripCaptionEnvelope) {
      // QuickTime wraps CEA-608 in a cdat box and CEA-708 in a ccdp box; a sample may contain more
      // than one. The parsers consume the bare cc_data of the payload.
      const uint8_t* bytes = buffer->array();
      const size_t size = buffer->limit();
      const auto readUInt32 = [](const uint8_t* data) {
        return (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) |
               (static_cast<uint32_t>(data[2]) << 8) | static_cast<uint32_t>(data[3]);
      };
      std::vector<uint8_t> payload;
      size_t offset = 0;
      bool boxed = false;
      while (offset + 8 <= size) {
        const uint32_t boxSize = readUInt32(bytes + offset);
        const uint32_t boxType = readUInt32(bytes + offset + 4);
        if (boxSize < 8 || offset + boxSize > size || (boxType != kCdat && boxType != kCcdp)) {
          break;
        }
        payload.insert(payload.end(), bytes + offset + 8, bytes + offset + boxSize);
        offset += boxSize;
        boxed = true;
      }
      if (boxed) {
        buffer = std::make_shared<AVBlockBuffer>(payload.data(), static_cast<uint32_t>(payload.size()));
      }
    }
    const int64_t sampleTimeUs = trimmedByEditList ? 0 : best->currentSampleTimeUs;
    if (best->captionOutput != nullptr && descriptionIndex < best->sampleDescriptions.size()) {
      const std::string& mimeType = best->sampleDescriptions[descriptionIndex].mimeType;
      const auto ccData = text::extractCea708CcDataFromAnnexB(buffer->array(), buffer->limit(),
        mimeType == MimeTypes::VIDEO_H265);
      if (!ccData.empty()) {
        emitCaptionSample(*best, ccData, sampleTimeUs);
      }
    }
    if (best->output != nullptr) {
      // A trimmed sample precedes the start of the presented media, which is the start of the
      // timeline: its timestamp is brought back to it, so that the queues and the renderers only see
      // the positions the media presents. It is discarded anyway.
      AVSample sample(best->index, sampleTimeUs, sampleFlags, std::move(buffer));
      best->output->sample(sample);
    }    advanceSample(*best);
    return true;
  }
}

void AVMp4Extractor::setSampleIndex(Track& track, const uint64_t sampleIndex) {
  const uint64_t index = std::min<uint64_t>(sampleIndex, track.sampleCount);
  track.nextSampleIndex = index;

  // The decode time of the sample, and the position of the stts cursor on it.
  track.timeEntryIndex = 0;
  track.timeEntryUsed = 0;
  track.decodeTime = 0;
  uint64_t consumed = 0;
  while (consumed < index && track.timeEntryIndex < track.timeToSample.size()) {
    const TimeToSampleEntry& entry = track.timeToSample[track.timeEntryIndex];
    if (entry.sampleCount == 0) {
      track.timeEntryIndex += 1;
      track.timeEntryUsed = 0;
      continue;
    }
    const uint64_t take = std::min<uint64_t>(entry.sampleCount, index - consumed);
    track.decodeTime += static_cast<int64_t>(take) * static_cast<int64_t>(entry.sampleDelta);
    consumed += take;
    if (take == entry.sampleCount) {
      track.timeEntryIndex += 1;
      track.timeEntryUsed = 0;
    } else {
      track.timeEntryUsed = take;
    }
  }

  // The composition offset of the sample, and the position of the ctts cursor on it.
  track.offsetEntryIndex = 0;
  track.offsetEntryUsed = 0;
  track.compositionOffset = 0;
  consumed = 0;
  while (consumed < index && track.offsetEntryIndex < track.compositionOffsets.size()) {
    const CompositionOffsetEntry& entry = track.compositionOffsets[track.offsetEntryIndex];
    if (entry.sampleCount == 0) {
      track.offsetEntryIndex += 1;
      track.offsetEntryUsed = 0;
      continue;
    }
    const uint64_t take = std::min<uint64_t>(entry.sampleCount, index - consumed);
    consumed += take;
    if (take == entry.sampleCount) {
      track.offsetEntryIndex += 1;
      track.offsetEntryUsed = 0;
    } else {
      track.offsetEntryUsed = take;
    }
  }
  if (track.offsetEntryIndex < track.compositionOffsets.size()) {
    track.compositionOffset = track.compositionOffsets[track.offsetEntryIndex].sampleOffset;
  }

  track.chunkCursorValid = false;
  track.currentSampleValid = false;
}

void AVMp4Extractor::updateCurrentSample(Track& track) {
  track.currentSampleValid = false;
  track.currentFromFragment = false;

  if (track.sampleTableValid && track.nextSampleIndex < track.sampleCount) {
    if (!positionChunkCursor(track, track.nextSampleIndex)) {
      return;
    }
    uint64_t offset = track.chunkOffsets[track.chunkIndex];
    for (uint64_t i = track.chunkFirstSample; i < track.nextSampleIndex; ++i) {
      offset += sampleSize(track, i);
    }
    track.currentSampleOffset = offset;
    track.currentSampleSize = sampleSize(track, track.nextSampleIndex);
    track.currentDescriptionIndex = track.chunkDescriptionIndex;
    const int64_t composedTicks = track.decodeTime + track.compositionOffset;
    track.currentSampleTimeUs =
      AVUtil::scaleToUs(composedTicks - editStartTicks(track), track.timescale);
    uint32_t flags = static_cast<uint32_t>(AVSample::kFlagNone);
    if (isSyncSample(track, track.nextSampleIndex)) {
      flags |= static_cast<uint32_t>(AVSample::kFlagKeyFrame);
    }
    if (track.nextSampleIndex == 0) {
      flags |= static_cast<uint32_t>(AVSample::kFlagFirstSample);
    }
    if (track.nextSampleIndex + 1 == track.sampleCount) {
      flags |= static_cast<uint32_t>(AVSample::kFlagLastSample);
    }
    track.currentSampleFlags = flags;
    track.currentSampleValid = true;
    return;
  }

  if (!track.fragments.empty()) {
    const FragmentSample& sample = track.fragments.front();
    track.currentSampleOffset = sample.offset;
    track.currentSampleSize = sample.size;
    track.currentSampleTimeUs = sample.timeUs;
    track.currentSampleFlags = sample.flags;
    track.currentDescriptionIndex = sample.sampleDescriptionIndex;
    track.currentFromFragment = true;
    track.currentSampleValid = true;
  }
}

void AVMp4Extractor::advanceSample(Track& track) {
  if (track.currentFromFragment) {
    if (!track.fragments.empty()) {
      track.fragments.pop_front();
    }
    track.currentFromFragment = false;
    track.currentSampleValid = false;
    return;
  }

  if (track.timeEntryIndex < track.timeToSample.size()) {
    const TimeToSampleEntry& entry = track.timeToSample[track.timeEntryIndex];
    track.decodeTime += static_cast<int64_t>(entry.sampleDelta);
    track.timeEntryUsed += 1;
    if (track.timeEntryUsed >= entry.sampleCount) {
      track.timeEntryIndex += 1;
      track.timeEntryUsed = 0;
    }
  }
  if (track.offsetEntryIndex < track.compositionOffsets.size()) {
    const CompositionOffsetEntry& entry = track.compositionOffsets[track.offsetEntryIndex];
    track.offsetEntryUsed += 1;
    if (track.offsetEntryUsed >= entry.sampleCount) {
      track.offsetEntryIndex += 1;
      track.offsetEntryUsed = 0;
      track.compositionOffset = track.offsetEntryIndex < track.compositionOffsets.size()
                                  ? track.compositionOffsets[track.offsetEntryIndex].sampleOffset
                                  : 0;
    }
  }
  if (track.nextSampleIndex < track.sampleCount) {
    track.nextSampleIndex += 1;
  }
  track.currentSampleValid = false;
}

uint32_t AVMp4Extractor::sampleSize(const Track& track, const uint64_t sampleIndex) {
  if (track.constantSampleSize != 0) {
    return track.constantSampleSize;
  }
  if (sampleIndex < track.sampleSizes.size()) {
    return track.sampleSizes[static_cast<size_t>(sampleIndex)];
  }
  return 0;
}

bool AVMp4Extractor::isSyncSample(const Track& track, const uint64_t sampleIndex) {
  // Without a sync sample box every sample is a sync sample, as ISO/IEC 14496-12 mandates and as
  // Mp4SampleTable.getSampleSync implements it.
  if (!track.hasSyncSamples || track.syncSamples.empty()) {
    return true;
  }
  if (sampleIndex >= 0xFFFFFFFFull) {
    return false;
  }
  return std::binary_search(track.syncSamples.begin(), track.syncSamples.end(),
                            static_cast<uint32_t>(sampleIndex + 1));
}

bool AVMp4Extractor::positionChunkCursor(Track& track, const uint64_t sampleIndex) {
  if (!track.sampleTableValid || track.chunkOffsets.empty() || track.sampleToChunk.empty()) {
    return false;
  }
  if (!track.chunkCursorValid || sampleIndex < track.chunkFirstSample) {
    // The cursor is only ever moved forward, therefore it is positioned again from the first chunk.
    track.chunkEntryIndex = 0;
    track.chunkIndex = 0;
    track.chunkFirstSample = 0;
    track.chunkDescriptionIndex = 1;
    track.chunkCursorValid = false;
  }
  if (sampleIndex < track.chunkFirstSample) {
    return false;
  }

  while (track.chunkEntryIndex < track.sampleToChunk.size()) {
    if (track.chunkIndex >= track.chunkOffsets.size()) {
      return false;
    }
    const SampleToChunkEntry& entry = track.sampleToChunk[track.chunkEntryIndex];
    const uint64_t perChunk = entry.samplesPerChunk;
    const uint64_t nextEntryFirstChunk =
      track.chunkEntryIndex + 1 < track.sampleToChunk.size()
        ? static_cast<uint64_t>(track.sampleToChunk[track.chunkEntryIndex + 1].firstChunk) - 1
        : track.chunkOffsets.size();
    if (perChunk == 0 || nextEntryFirstChunk <= track.chunkIndex) {
      track.chunkEntryIndex += 1;
      track.chunkIndex = nextEntryFirstChunk;
      continue;
    }
    const uint64_t samplesInEntry = (nextEntryFirstChunk - track.chunkIndex) * perChunk;
    if (track.chunkFirstSample + samplesInEntry > sampleIndex) {
      const uint64_t delta = (sampleIndex - track.chunkFirstSample) / perChunk;
      track.chunkIndex += delta;
      track.chunkFirstSample += delta * perChunk;
      track.chunkDescriptionIndex = entry.sampleDescriptionIndex;
      track.chunkCursorValid = true;
      return track.chunkIndex < track.chunkOffsets.size();
    }
    track.chunkFirstSample += samplesInEntry;
    track.chunkIndex = nextEntryFirstChunk;
    track.chunkEntryIndex += 1;
  }
  return false;
}

uint64_t AVMp4Extractor::findSampleIndexForTime(const Track& track, const int64_t timeUs) {
  if (track.timescale <= 0 || track.timeToSample.empty()) {
    return 0;
  }
  const int64_t editStart = editStartTicks(track);
  const int64_t targetTicks =
    AVUtil::scaleLargeValue(timeUs, AVUtil::kMicrosPerSecond, track.timescale) + editStart;

  uint64_t index = 0;
  int64_t ticks = editStart;
  for (const TimeToSampleEntry& entry : track.timeToSample) {
    if (index >= track.sampleCount) {
      break;
    }
    if (entry.sampleDelta == 0) {
      index += entry.sampleCount;
      continue;
    }
    const int64_t entryTicks =
      static_cast<int64_t>(entry.sampleCount) * static_cast<int64_t>(entry.sampleDelta);
    if (entryTicks > 0 && ticks + entryTicks > targetTicks) {
      const int64_t remaining = targetTicks > ticks ? targetTicks - ticks : 0;
      index += static_cast<uint64_t>(remaining / entry.sampleDelta);
      break;
    }
    ticks += entryTicks;
    index += entry.sampleCount;
  }
  if (index > track.sampleCount) {
    index = track.sampleCount;
  }
  // The decoder has to start on a sync sample: the target is snapped back to the sync sample that
  // precedes it.
  if (track.hasSyncSamples && !track.syncSamples.empty() && index < 0xFFFFFFFFull) {
    const uint32_t oneBased = static_cast<uint32_t>(index + 1);
    const auto it = std::upper_bound(track.syncSamples.begin(), track.syncSamples.end(), oneBased);
    if (it == track.syncSamples.begin()) {
      return 0;
    }
    const uint32_t found = *(it - 1);
    return found > 0 ? static_cast<uint64_t>(found - 1) : 0;
  }
  return index;
}

uint64_t AVMp4Extractor::findSampleIndexForPosition(const Track& track, const long position) {
  if (!track.sampleTableValid || track.sampleCount == 0 || track.chunkOffsets.empty() ||
      track.sampleToChunk.empty()) {
    return 0;
  }
  const uint64_t target = position > 0 ? static_cast<uint64_t>(position) : 0;
  uint64_t sampleIndex = 0;
  size_t chunkEntryIndex = 0;
  for (uint64_t chunk = 0; chunk < track.chunkOffsets.size(); ++chunk) {
    while (chunkEntryIndex + 1 < track.sampleToChunk.size() &&
           static_cast<uint64_t>(track.sampleToChunk[chunkEntryIndex + 1].firstChunk) - 1 <=
             chunk) {
      chunkEntryIndex += 1;
    }
    const uint64_t perChunk = track.sampleToChunk[chunkEntryIndex].samplesPerChunk;
    uint64_t offset = track.chunkOffsets[chunk];
    for (uint64_t i = 0; i < perChunk && sampleIndex + i < track.sampleCount; ++i) {
      if (offset + sampleSize(track, sampleIndex + i) > target) {
        return sampleIndex + i;
      }
      offset += sampleSize(track, sampleIndex + i);
    }
    sampleIndex += perChunk;
    if (sampleIndex >= track.sampleCount) {
      break;
    }
  }
  return track.sampleCount;
}

bool AVMp4Extractor::hasPendingSamples() {
  for (Track& track : tracks_) {
    if (track.output == nullptr) {
      continue;
    }
    updateCurrentSample(track);
    if (track.currentSampleValid) {
      return true;
    }
  }
  return false;
}

} // namespace coral
