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

#include <extractor/container/MatroskaExtractor.h>

#include <extractor/Utility.h>
#include <extractor/text/SubtitleParser.h>

#include <media/MetadataEntries.h>

#include <algorithm>
#include <functional>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>

namespace coral {

namespace {

/** The identifier of the {@code EBML} element. */
constexpr long kEbml = 0x1A45DFA3;

/** The identifier of the {@code DocType} element. */
constexpr long kDocType = 0x4282;

/** The identifier of the {@code Segment} element. */
constexpr long kSegment = 0x18538067;

/** The identifier of the {@code SeekHead} element. */
constexpr long kSeekHead = 0x114D9B74;

/** The identifier of the {@code Info} element. */
constexpr long kInfo = 0x1549A966;

/** The identifier of the {@code TimecodeScale} element. */
constexpr long kTimecodeScale = 0x2AD7B1;

/** The identifier of the {@code Duration} element. */
constexpr long kDuration = 0x4489;

/** The identifier of the {@code Title} element. */
constexpr long kTitle = 0x7BA9;

/** The identifier of the {@code Cluster} element. */
constexpr long kCluster = 0x1F43B675;

/** The identifier of the {@code Timecode} element of a cluster. */
constexpr long kClusterTimecode = 0xE7;

/** The identifier of the {@code SimpleBlock} element. */
constexpr long kSimpleBlock = 0xA3;

/** The identifier of the {@code BlockGroup} element. */
constexpr long kBlockGroup = 0xA0;

/** The identifier of the {@code Block} element. */
constexpr long kBlock = 0xA1;

/** The identifier of the {@code BlockDuration} element. */
constexpr long kBlockDuration = 0x9B;

/** The identifier of the {@code ReferenceBlock} element. */
constexpr long kReferenceBlock = 0xFB;

/** The identifier of the {@code Cues} element. */
constexpr long kCues = 0x1C53BB6B;

/** The identifier of the {@code CuePoint} element. */
constexpr long kCuePoint = 0xBB;

/** The identifier of the {@code CueTime} element. */
constexpr long kCueTime = 0xB3;

/** The identifier of the {@code CueTrackPositions} element. */
constexpr long kCueTrackPositions = 0xB7;

/** The identifier of the {@code CueTrack} element. */
constexpr long kCueTrack = 0xF7;

/** The identifier of the {@code CueClusterPosition} element. */
constexpr long kCueClusterPosition = 0xF1;

/** The identifier of the {@code Tracks} element. */
constexpr long kTracks = 0x1654AE6B;

/** The identifier of the {@code TrackEntry} element. */
constexpr long kTrackEntry = 0xAE;

/** The identifier of the {@code TrackNumber} element. */
constexpr long kTrackNumber = 0xD7;

/** The identifier of the {@code TrackUid} element. */
constexpr long kTrackUid = 0x73C5;

/** The identifier of the {@code TrackType} element. */
constexpr long kTrackType = 0x83;

/** The identifier of the {@code FlagDefault} element. */
constexpr long kFlagDefault = 0x88;

/** The identifier of the {@code FlagForced} element. */
constexpr long kFlagForced = 0x55AA;

/** The identifier of the {@code FlagLacing} element. */
constexpr long kFlagLacing = 0x9C;

/** The identifier of the {@code DefaultDuration} element. */
constexpr long kDefaultDuration = 0x23E383;

/** The identifier of the {@code Name} element. */
constexpr long kName = 0x536E;

/** The identifier of the {@code Language} element. */
constexpr long kLanguage = 0x22B59C;

/** The identifier of the {@code LanguageIETF} element. */
constexpr long kLanguageIetf = 0x22B59D;

/** The identifier of the {@code CodecID} element. */
constexpr long kCodecId = 0x86;

/** The identifier of the {@code CodecPrivate} element. */
constexpr long kCodecPrivate = 0x63A2;

/** The identifier of the {@code CodecDelay} element. */
constexpr long kCodecDelay = 0x56AA;

/** The identifier of the {@code SeekPreRoll} element. */
constexpr long kSeekPreRoll = 0x56BB;

/** The identifier of the {@code Video} element. */
constexpr long kVideo = 0xE0;

/** The identifier of the {@code FlagInterlaced} element. */
constexpr long kFlagInterlaced = 0x9A;

/** The identifier of the {@code StereoMode} element. */
constexpr long kStereoMode = 0x53B8;

/** The identifier of the {@code AlphaMode} element. */
constexpr long kAlphaMode = 0x53C0;

/** The identifier of the {@code PixelWidth} element. */
constexpr long kPixelWidth = 0xB0;

/** The identifier of the {@code PixelHeight} element. */
constexpr long kPixelHeight = 0xBA;

/** The identifier of the {@code DisplayWidth} element. */
constexpr long kDisplayWidth = 0x54B0;

/** The identifier of the {@code DisplayHeight} element. */
constexpr long kDisplayHeight = 0x54BA;

/** The identifier of the {@code DisplayUnit} element. */
constexpr long kDisplayUnit = 0x54B2;

/** The identifier of the {@code Colour} element. */
constexpr long kColour = 0x55B0;

/** The identifier of the {@code MatrixCoefficients} element. */
constexpr long kMatrixCoefficients = 0x55B1;

/** The identifier of the {@code TransferCharacteristics} element. */
constexpr long kTransferCharacteristics = 0x55BA;

/** The identifier of the {@code Primaries} element. */
constexpr long kPrimaries = 0x55BB;

/** The identifier of the {@code Range} element. */
constexpr long kColourRange = 0x55B9;

/** The identifier of the {@code Projection} element. */
constexpr long kProjection = 0x7670;

/** The identifier of the {@code ProjectionType} element. */
constexpr long kProjectionType = 0x7671;

/** The identifier of the {@code ProjectionPrivate} element. */
constexpr long kProjectionPrivate = 0x7672;

/** The identifier of the {@code Audio} element. */
constexpr long kAudio = 0xE1;

/** The identifier of the {@code SamplingFrequency} element. */
constexpr long kSamplingFrequency = 0xB5;

/** The identifier of the {@code OutputSamplingFrequency} element. */
constexpr long kOutputSamplingFrequency = 0x78B5;

/** The identifier of the {@code Channels} element. */
constexpr long kChannels = 0x9F;

/** The identifier of the {@code BitDepth} element. */
constexpr long kBitDepth = 0x6264;

/** The identifier of the {@code Tags} element. */
constexpr long kTags = 0x1254C367;

/** The identifier of the {@code Chapters} element. */
constexpr long kChapters = 0x1043A770;

/** The identifier of the {@code Tags} element and of the elements it contains. */
constexpr long kTag = 0x7373;
constexpr long kTargets = 0x63C0;
constexpr long kTagTrackUid = 0x63C5;
constexpr long kSimpleTag = 0x67C8;
constexpr long kTagName = 0x45A3;
constexpr long kTagString = 0x4487;
constexpr long kTagLanguage = 0x447A;

/** The identifier of the {@code Chapters} elements. */
constexpr long kEditionEntry = 0x45B9;
constexpr long kChapterAtom = 0xB6;
constexpr long kChapterTimeStart = 0x91;
constexpr long kChapterTimeEnd = 0x92;
constexpr long kChapterDisplay = 0x80;
constexpr long kChapString = 0x85;
constexpr long kChapLanguage = 0x437C;

/** The identifier of the {@code Attachments} element. */
constexpr long kAttachments = 0x1941A469;

/** The default timecode scale of a Matroska container, i.e. one millisecond. */
constexpr int64_t kDefaultTimecodeScale = 1000000;

/** The largest timecode scale that this extractor accepts, as ExoPlayer does. */
constexpr int64_t kMaxTimecodeScale = 1000000000;

/** The largest element that this extractor is willing to buffer, in bytes. */
constexpr long kMaxElementSize = 2 * 1024 * 1024;

/** The largest number of frames that a laced block may hold. */
constexpr size_t kMaxLacedFrameCount = 256;

/** The size of the buffer used to discard the payload of an element, in bytes. */
constexpr uint32_t kSkipBufferSize = 16 * 1024;

/** The {@code TrackType} of a video track. */
constexpr int kTrackTypeVideo = 1;

/** The {@code TrackType} of an audio track. */
constexpr int kTrackTypeAudio = 2;

/** The {@code TrackType} of a subtitle track. */
constexpr int kTrackTypeSubtitle = 17;

/** The format line of the events of an SSA/ASS track, which the container states once. */
constexpr char kSsaDialogueFormat[] =
  "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text";

/** Formats @p timeUs as the timecode of a SubRip cue, i.e. `HH:MM:SS,mmm`. */
std::string formatSubripTimecode(const int64_t timeUs) {
  const int64_t totalMs = std::max<int64_t>(timeUs, 0) / 1000;
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d,%03d",
                static_cast<int>(totalMs / 3600000), static_cast<int>((totalMs / 60000) % 60),
                static_cast<int>((totalMs / 1000) % 60), static_cast<int>(totalMs % 1000));
  return buffer;
}

/** Formats @p timeUs as the timecode of a WebVTT cue, i.e. `HH:MM:SS.mmm`. */
std::string formatWebvttTimecode(const int64_t timeUs) {
  const int64_t totalMs = std::max<int64_t>(timeUs, 0) / 1000;
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d.%03d",
                static_cast<int>(totalMs / 3600000), static_cast<int>((totalMs / 60000) % 60),
                static_cast<int>((totalMs / 1000) % 60), static_cast<int>(totalMs % 1000));
  return buffer;
}

/** Formats @p timeUs as the timecode of an SSA dialogue line, i.e. `H:MM:SS.cc`. */
std::string formatSsaTimecode(const int64_t timeUs) {
  const int64_t totalCs = std::max<int64_t>(timeUs, 0) / 10000;
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%d:%02d:%02d.%02d",
                static_cast<int>(totalCs / 360000), static_cast<int>((totalCs / 6000) % 60),
                static_cast<int>((totalCs / 100) % 60), static_cast<int>(totalCs % 100));
  return buffer;
}

/** The lacing of a block: no lacing at all. */
constexpr int kLacingNone = 0;

/** The lacing of a block: Xiph lacing. */
constexpr int kLacingXiph = 1;

/** The lacing of a block: fixed size lacing. */
constexpr int kLacingFixed = 2;

/** The rotation values, in degrees, that a projection may describe. */
constexpr int kRotations[] = {0, 90, 180, 270};

/** The identifier of an H.265 sequence parameter set. */
constexpr uint8_t kHevcSps = 33;

/** The type of the NAL unit of an H.264 instantaneous decoder refresh picture. */
constexpr uint8_t kH264IdrNal = 5;

/** The number of nanoseconds in a second. */
constexpr int64_t kNanosPerSecond = 1000000000;

/** The number of microseconds in a second. */
constexpr int64_t kMicrosPerSecond = 1000000;

/**
 * Swaps the bytes of a child element into the buffer that the children of an element are parsed from, and
 * restores the bytes of the parent when the scope ends.
 *
 * <p>Every parser of a set of children reads the buffer of its own element, so parsing the children of a
 * nested element from the buffer of its parent consumed the elements that follow it: the {@code Video}
 * element of a track, for instance, used to consume the codec private data that follows it, which left
 * the track without codec specific data, and its samples without the framing their decoder reads.
 *
 * <p>The parent is left in the state it was in whatever the parser does, including an early return.
 */
class ChildElementScope final {
public:
  ChildElementScope(AVDataBuffer& parent, const size_t childSize)
    : parent_(parent), saved_(std::move(parent)) {
    parent_ = AVDataBuffer(saved_.array() + saved_.position(), static_cast<uint32_t>(childSize));
    saved_.skipBytes(childSize);
  }

  ChildElementScope(const ChildElementScope&) = delete;
  ChildElementScope& operator=(const ChildElementScope&) = delete;

  ~ChildElementScope() { parent_ = std::move(saved_); }

private:
  AVDataBuffer& parent_;
  AVDataBuffer saved_;
};

/** A 32 bit float read from a big endian buffer. */
float getBigEndianFloat(const uint8_t data[], const size_t size) {
  // EBML encodes a floating point value on four bytes or on eight: reading the first four bytes of a value
  // that is stored on eight read the sampling frequency of a track as 7.17 instead of 44100, which left its
  // audio at a rate of a few hertz.
  const size_t bytes = size >= 8 ? 8 : 4;
  uint64_t bits = 0;
  for (size_t i = 0; i < bytes; i++) {
    bits = (bits << 8) | (i < size ? data[i] : 0);
  }
  if (bytes == 8) {
    double wide = 0;
    std::memcpy(&wide, &bits, sizeof(wide));
    return static_cast<float>(wide);
  }
  const uint32_t narrow = static_cast<uint32_t>(bits);
  float value = 0;
  std::memcpy(&value, &narrow, sizeof(value));
  return value;
}

/** A 64 bit float read from a big endian buffer. */
double getBigEndianDouble(const uint8_t data[], const size_t size) {
  if (size == 4) {
    return static_cast<double>(getBigEndianFloat(data, size));
  }
  uint64_t bits = 0;
  for (size_t i = 0; i < 8; i++) {
    bits = (bits << 8) | (i < size ? data[i] : 0);
  }
  double value = 0;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

/** Returns whether the buffer holds a valid UTF-8 string. */
bool checkUtf8(const uint8_t data[], const size_t size) {
  size_t i = 0;
  while (i < size) {
    const uint8_t first = data[i];
    if (first < 0x80) {
      i++;
      continue;
    }
    size_t continuation;
    if ((first & 0xE0) == 0xC0) {
      continuation = 1;
    } else if ((first & 0xF0) == 0xE0) {
      continuation = 2;
    } else if ((first & 0xF8) == 0xF0) {
      continuation = 3;
    } else {
      return false;
    }
    if (i + continuation >= size) {
      return false;
    }
    for (size_t j = 1; j <= continuation; j++) {
      if ((data[i + j] & 0xC0) != 0x80) {
        return false;
      }
    }
    i += continuation + 1;
  }
  return true;
}

} // namespace anonymous

// ==================================================================================================
// EBML helpers
// ==================================================================================================

int64_t AVMatroskaExtractor::parseVint(AVDataBuffer& data) {
  if (data.remaining() < 1) {
    return -1;
  }
  const uint8_t first = data.get();
  int length = 1;
  uint8_t mask = 0x80;
  while (length <= 8 && (first & mask) == 0) {
    mask = static_cast<uint8_t>(mask >> 1);
    length++;
  }
  if (length > 8 || static_cast<size_t>(length - 1) > data.remaining()) {
    return -1;
  }
  int64_t value = first;
  for (int i = 1; i < length; i++) {
    value = (value << 8) | data.get();
  }
  return value;
}

int64_t AVMatroskaExtractor::parseSize(AVDataBuffer& data) {
  if (data.remaining() < 1) {
    return -1;
  }
  const uint8_t first = data.get();
  int length = 1;
  uint8_t mask = 0x80;
  while (length <= 8 && (first & mask) == 0) {
    mask = static_cast<uint8_t>(mask >> 1);
    length++;
  }
  if (length > 8 || static_cast<size_t>(length - 1) > data.remaining()) {
    return -1;
  }
  int64_t value = static_cast<int64_t>(first & (mask - 1));
  for (int i = 1; i < length; i++) {
    value = (value << 8) | data.get();
  }
  if (value == (1LL << (7 * length)) - 1) {
    // An all ones size means that the element extends to the end of its parent.
    return -2;
  }
  return value;
}

int64_t AVMatroskaExtractor::parseSignedVint(AVDataBuffer& data) {
  if (data.remaining() < 1) {
    return -1;
  }
  const uint8_t first = data.get();
  int length = 1;
  uint8_t mask = 0x80;
  while (length <= 8 && (first & mask) == 0) {
    mask = static_cast<uint8_t>(mask >> 1);
    length++;
  }
  if (length > 8 || static_cast<size_t>(length - 1) > data.remaining()) {
    return -1;
  }
  int64_t value = static_cast<int64_t>(first & (mask - 1));
  for (int i = 1; i < length; i++) {
    value = (value << 8) | data.get();
  }
  return value - ((1LL << (7 * length)) - 1);
}

int64_t AVMatroskaExtractor::readUnsigned(const uint8_t data[], const size_t size) {
  if (size > 8) {
    return 0;
  }
  int64_t value = 0;
  for (size_t i = 0; i < size; i++) {
    value = (value << 8) | data[i];
  }
  return value;
}

int AVMatroskaExtractor::readBits(AVBitReader& bits, const int count) {
  if (count <= 0 || !bits.canReadBits(count)) {
    return 0;
  }
  return static_cast<int>(bits.readBits(count));
}

int AVMatroskaExtractor::readUe(AVBitReader& bits) {
  int leadingZeroBits = 0;
  while (leadingZeroBits < 31 && bits.canReadBits(leadingZeroBits + 1) &&
         readBits(bits, 1) == 0) {
    leadingZeroBits++;
  }
  if (leadingZeroBits == 0) {
    return 0;
  }
  if (leadingZeroBits >= 31 || !bits.canReadBits(leadingZeroBits)) {
    return 0;
  }
  return bits.readBits(leadingZeroBits) + ((1 << leadingZeroBits) - 1);
}

int64_t AVMatroskaExtractor::peekVint(AVExtractorInput& input, const bool keepMarker,
                                      long* outLength, const long startOffset) {
  input.resetPeekPosition();
  if (startOffset > 0) {
    // The value starts after the bytes of the value that precedes it, e.g. the size of an element
    // starts after the bytes of its identifier.
    input.skipPeeked(startOffset);
  }
  // A peeked byte advances the peek position, so the bytes of a value are read one after the other.
  // Skipping them a second time read every other byte, and every element header was misparsed: the very
  // first one, the EBML header, was rejected and the whole container looked unreadable.
  const int first = input.peekByte();
  if (first == static_cast<int>(kEndOfInput)) {
    input.resetPeekPosition();
    return -1;
  }

  int length = 1;
  uint8_t mask = 0x80;
  while (length <= 8 && (static_cast<uint8_t>(first) & mask) == 0) {
    mask = static_cast<uint8_t>(mask >> 1);
    length++;
  }
  if (length > 8) {
    input.resetPeekPosition();
    return -1;
  }

  int64_t value = keepMarker ? static_cast<int64_t>(static_cast<uint8_t>(first))
                             : static_cast<int64_t>(static_cast<uint8_t>(first) & (mask - 1));
  for (int i = 1; i < length; i++) {
    const int next = input.peekByte();
    if (next == static_cast<int>(kEndOfInput)) {
      input.resetPeekPosition();
      return -1;
    }
    value = (value << 8) | next;
  }
  input.resetPeekPosition();
  if (outLength != nullptr) {
    *outLength = length;
  }
  if (!keepMarker && value == (1LL << (7 * length)) - 1) {
    return -2;
  }
  return value;
}

bool AVMatroskaExtractor::readElementHeader(AVExtractorInput& input, long& outId, long& outSize) {
  const long available = inputEnd_ - input.getPosition();
  if (available < 2) {
    return false;
  }
  long identifierLength = 0;
  const int64_t id = peekVint(input, true, &identifierLength);
  if (id <= 0) {
    return false;
  }
  // The size follows the bytes of the identifier.
  long sizeLength = 0;
  const int64_t size = peekVint(input, false, &sizeLength, identifierLength);
  if (size == -1) {
    return false;
  }
  const long headerLength = identifierLength + sizeLength;
  if (size > 0 && size > available - headerLength) {
    return false;
  }
  // The identifier and the size are consumed, which leaves the read position at the payload of the
  // element: that is where its caller reads it, or skips it. A size of -2, i.e. an element that extends
  // to the end of its parent, is left as it is.
  if (headerLength > 0 && !input.skipFully(headerLength)) {
    return false;
  }
  outId = static_cast<long>(id);
  outSize = static_cast<long>(size);
  return true;
}

bool AVMatroskaExtractor::takeElement(AVExtractorInput& input, const long size,
                                      AVDataBuffer& target) const {
  if (size <= 0) {
    target.reset(0);
    return true;
  }
  if (input.readFully(target, static_cast<size_t>(size))) {
    return true;
  }
  target.reset(0);
  return false;
}

long AVMatroskaExtractor::parentFrameEnd() const {
  return elementFrames_.size() > 1 ? elementFrames_[elementFrames_.size() - 2].second : inputEnd_;
}

long AVMatroskaExtractor::currentElementEnd() const {
  if (elementFrames_.empty()) {
    return elementEnd_ >= 0 ? elementEnd_ : inputEnd_;
  }
  const std::pair<long, long>& frame = elementFrames_.back();
  return frame.second < 0 ? parentFrameEnd() : frame.second;
}

long AVMatroskaExtractor::pushElementFrame(AVExtractorInput& input, const long size) {
  const long start = input.getPosition();
  long end;
  if (size < 0) {
    // The element has an unknown size, i.e. it extends to the end of its parent.
    end = parentFrameEnd();
  } else {
    end = size > std::numeric_limits<long>::max() - start ? std::numeric_limits<long>::max()
                                                          : start + size;
  }
  if (end <= start) {
    return -1;
  }
  elementFrames_.push_back(std::make_pair(start, end));
  elementEnd_ = end;
  return end - start;
}

void AVMatroskaExtractor::popElementFrame() {
  if (!elementFrames_.empty()) {
    elementFrames_.pop_back();
  }
}

bool AVMatroskaExtractor::advanceElement(AVExtractorInput& input) {
  const long end = currentElementEnd();
  popElementFrame();
  const long position = input.getPosition();
  elementEnd_ = end;
  if (position >= end) {
    return true;
  }
  return input.skipFully(end - position);
}

bool AVMatroskaExtractor::skipElement(AVExtractorInput& input) {
  return advanceElement(input);
}

bool AVMatroskaExtractor::isEndOfParent(AVExtractorInput& input) const {
  return elementFrames_.empty() || input.getPosition() >= currentElementEnd();
}

long AVMatroskaExtractor::loadElement(AVExtractorInput& input, const long size) {
  if (size <= 0) {
    element_.reset(0);
    return size;
  }
  if (size > kMaxElementSize || !takeElement(input, size, element_)) {
    return -1;
  }
  return size;
}

// ==================================================================================================
// Extractor contract
// ==================================================================================================

int AVMatroskaExtractor::sniff(AVExtractorInput& input) {
  sniffFailureReason_.clear();
  input.resetPeekPosition();

  uint8_t header[4] = {0, 0, 0, 0};
  const long peeked = input.peek(header, 0, sizeof(header));
  input.resetPeekPosition();

  if (peeked < 1) {
    sniffFailureReason_ = "The input is empty.";
    return AVExtractorsFactory::kSniffResultNone;
  }
  if (peeked == 4) {
    const uint32_t value = (static_cast<uint32_t>(header[0]) << 24) |
                           (static_cast<uint32_t>(header[1]) << 16) |
                           (static_cast<uint32_t>(header[2]) << 8) |
                           static_cast<uint32_t>(header[3]);
    if (value == static_cast<uint32_t>(kEbml)) {
      return AVExtractorsFactory::kSniffResultCertain;
    }
  }
  if (header[0] == 0x1A && peeked >= 2 && header[1] == 0x45) {
    return AVExtractorsFactory::kSniffResultPossible;
  }
  sniffFailureReason_ = "The EBML header identifier was not found.";
  return AVExtractorsFactory::kSniffResultNone;
}

void AVMatroskaExtractor::init(AVExtractorOutput& output) {
  output_ = &output;
  globalMetadata_.clear();
  trackMetadata_.clear();
  tagsParsed_ = false;
  chaptersParsed_ = false;
  if (phase_ == Phase::kInitialize) {
    phase_ = Phase::kReadEbmlHeader;
  }
  scratch_.reset(kSkipBufferSize);
}

AVExtractor::Result AVMatroskaExtractor::read(AVExtractorInput& input) {
  const long length = input.getLength();
  inputEnd_ = length > 0 ? length : std::numeric_limits<long>::max();
  parsePosition_ = input.getPosition();

  Result result = kResultEndOfInput;
  bool madeProgress = false;
  const int maxElements = 8;
  for (int i = 0; i < maxElements; i++) {
    const Phase phaseBefore = phase_;
    switch (phase_) {
      case Phase::kInitialize: {
        phase_ = Phase::kReadEbmlHeader;
        result = kResultContinue;
        break;
      }
      case Phase::kReadEbmlHeader: {
        if (readEbmlHeader(input)) {
          phase_ = Phase::kReadSegment;
          result = kResultContinue;
        } else {
          result = kResultEndOfInput;
        }
        break;
      }
      case Phase::kReadSegment: {
        result = readSegment(input);
        break;
      }
      case Phase::kScanSegment: {
        result = scanForSegment(input) ? kResultContinue : kResultEndOfInput;
        break;
      }
      case Phase::kScanHeaders: {
        result = scanHeaders(input);
        break;
      }
      case Phase::kScanClusterOrder: {
        result = scanClusterOrder(input);
        break;
      }
      case Phase::kFillSeekMap: {
        result = fillSeekMap();
        break;
      }
      case Phase::kReadClusters: {
        result = readCluster(input);
        break;
      }
      case Phase::kSkipContent: {
        return skipContent(input);
      }
    }
    if (result != kResultContinue) {
      return result;
    }
    if (phaseBefore == Phase::kScanHeaders && phase_ == Phase::kReadClusters) {
      // The tracks were declared while the headers were scanned, which is what lets the caller
      // prepare the media. Return before this call starts to read the clusters: the samples would
      // be delivered to consumers the caller has not registered yet, and the reading position
      // would advance past the beginning of the media. The next call starts at the first cluster.
      parsePosition_ = input.getPosition();
      return kResultContinue;
    }
    if (input.getPosition() == parsePosition_) {
      if (phase_ == phaseBefore) {
        // Nothing was read and the state did not advance, so another attempt would read the same bytes.
        break;
      }
      // The state advanced without reading anything, which happens at the end of every cluster: the
      // children of the cluster were read, and the next cluster has to be located. The loop goes on
      // instead of stopping, because a caller that sees no progress reports the load as failed rather
      // than asking for more.
      parsePosition_ = input.getPosition();
      madeProgress = true;
      continue;
    }
    parsePosition_ = input.getPosition();
    madeProgress = true;
  }
  if (madeProgress && result == kResultEndOfInput) {
    return kResultContinue;
  }
  return result;
}

bool AVMatroskaExtractor::seek(const long position, const int64_t timeUs) {
  resetParser();
  pendingSeek_ = true;
  pendingSeekPosition_ = position;
  pendingSeekTimeUs_ = timeUs;
  parsePosition_ = position;
  if (position <= 0) {
    phase_ = Phase::kReadEbmlHeader;
    return true;
  }
  phase_ = Phase::kScanSegment;
  return true;
}

long AVMatroskaExtractor::getPosition() const {
  return parsePosition_;
}

void AVMatroskaExtractor::resetParser() {
  phase_ = Phase::kReadEbmlHeader;
  elementFrames_.clear();
  elementEnd_ = -1;
  parsePosition_ = 0;
  clusterOrder_.clear();
  rawCues_.clear();
  pendingCues_.clear();
  clusterStartPosition_ = -1;
  clusterTimecode_ = 0;
  clusterFramePushed_ = false;
  pendingClusterPosition_ = -1;
  currentTrack_ = nullptr;
  currentBlock_ = nullptr;
  blockFrameSizes_.clear();
  blockFrameCount_ = 1;
  blockContentStart_ = 0;
  blockDurationTicks_ = -1;
  webm_ = false;
}

// ==================================================================================================
// Header parsing
// ==================================================================================================

bool AVMatroskaExtractor::readEbmlHeader(AVExtractorInput& input) {
  long id = 0;
  long size = 0;
  if (!readElementHeader(input, id, size)) {
    return false;
  }
  if (id != kEbml || size <= 0 || size > kMaxElementSize || !takeElement(input, size, element_)) {
    sniffFailureReason_ = "The input does not start with a usable EBML header.";
    return false;
  }
  while (element_.remaining() > 0) {
    const int64_t childId = parseVint(element_);
    const int64_t childSize = parseSize(element_);
    if (childId <= 0 || childSize < 0 || static_cast<uint64_t>(childSize) > element_.remaining()) {
      break;
    }
    if (childId == kDocType) {
      std::vector<uint8_t> docType;
      element_.readBytes(docType, static_cast<size_t>(childSize));
      webm_ = docType.size() == 4 && std::memcmp(docType.data(), "webm", 4) == 0;
    } else {
      element_.skipBytes(static_cast<size_t>(childSize));
    }
  }
  return true;
}

AVExtractor::Result AVMatroskaExtractor::readSegment(AVExtractorInput& input) {
  long id = 0;
  long size = 0;
  if (!readElementHeader(input, id, size)) {
    return kResultEndOfInput;
  }
  if (id != kSegment) {
    sniffFailureReason_ = "The Segment element was not found.";
    return kResultEndOfInput;
  }
  segmentStartPosition_ = input.getPosition();
  if (pushElementFrame(input, size) < 0) {
    popElementFrame();
    segmentStartPosition_ = -1;
    return kResultEndOfInput;
  }
  phase_ = Phase::kScanHeaders;
  return kResultContinue;
}

bool AVMatroskaExtractor::scanForSegment(AVExtractorInput& input) {
  const long maxPosition = inputEnd_;
  const size_t chunkSize = scratch_.capacity();
  uint32_t window = 0;
  int filled = 0;
  while (input.getPosition() + 4 <= maxPosition) {
    const long read = input.read(scratch_.array(), 0, chunkSize);
    if (read == kEndOfInput || read <= 0) {
      return false;
    }
    for (long i = 0; i < read; i++) {
      window = (window << 8) | scratch_.array()[i];
      if (filled < 4) {
        filled++;
      }
      if (filled == 4 && window == static_cast<uint32_t>(kSegment)) {
        const long position = input.getPosition() - (read - i) + 3;
        pendingSeek_ = false;
        return input.setPosition(position, true);
      }
    }
  }
  return false;
}

bool AVMatroskaExtractor::isSegmentChild(const long id) {
  return id == kInfo || id == kTracks || id == kCues || id == kSeekHead || id == kTags ||
         id == kChapters || id == kAttachments || id == kCluster;
}

AVExtractor::Result AVMatroskaExtractor::scanHeaders(AVExtractorInput& input) {
  while (!isEndOfParent(input)) {
    long id = 0;
    long size = 0;
    if (!readElementHeader(input, id, size)) {
      return kResultEndOfInput;
    }
    if (pushElementFrame(input, size) < 0) {
      popElementFrame();
      return kResultEndOfInput;
    }

    if (id == kCluster) {
      // The frame of the cluster is kept, the next phase reads its children.
      clusterStartPosition_ = elementFrames_.back().first;
      clusterTimecode_ = 0;
      clusterFramePushed_ = true;
      if (!headersDeclared_) {
        // The tracks and the seek map of the segment are declared before the first cluster is read: a
        // caller can only prepare the media once it knows them, and a container that keeps its cues
        // after its clusters, which is what every file written by a muxer that streams does, never
        // reaches the end of the segment before its first cluster. The seek map is declared again if a
        // later element, e.g. the cues, describes the media more accurately: a caller keeps the last one.
        const Result declared = fillSeekMap();
        if (declared != kResultContinue) {
          return declared;
        }
      }
      phase_ = Phase::kReadClusters;
      return kResultContinue;
    }

    Result result = kResultContinue;
    if (id == kInfo) {
      result = parseSegmentInfo(input);
    } else if (id == kTracks) {
      result = parseTracks(input);
    } else if (id == kCues) {
      result = rawCues_.empty() ? parseCues(input)
                                : (advanceElement(input) ? kResultContinue : kResultEndOfInput);
    } else if (id == kTags) {
      if (!tagsParsed_ &&
          loadElement(input, currentElementEnd() - input.getPosition()) >= 0) {
        parseTagsElement(element_);
        tagsParsed_ = true;
      }
      result = advanceElement(input) ? kResultContinue : kResultEndOfInput;
    } else if (id == kChapters) {
      if (!chaptersParsed_ &&
          loadElement(input, currentElementEnd() - input.getPosition()) >= 0) {
        parseChaptersElement(element_);
        chaptersParsed_ = true;
      }
      result = advanceElement(input) ? kResultContinue : kResultEndOfInput;
    } else {
      result = skipElement(input) ? kResultContinue : kResultEndOfInput;
    }
    if (result != kResultContinue) {
      return result;
    }
    parsePosition_ = input.getPosition();
  }

  // The children of the Segment were exhausted.
  if (!elementFrames_.empty()) {
    popElementFrame();
  }
  if (pendingSeek_ && !headersDeclared_) {
    // The headers were not scanned yet, the requested time is not resolved.
    pendingSeek_ = false;
    pendingClusterPosition_ = pendingSeekPosition_ > 0 ? pendingSeekPosition_ : 0;
    clusterFramePushed_ = false;
    phase_ = Phase::kReadClusters;
    return kResultContinue;
  }
  if (!clusterOrder_.empty()) {
    phase_ = Phase::kFillSeekMap;
    return kResultContinue;
  }
  phase_ = Phase::kSkipContent;
  return kResultContinue;
}

AVExtractor::Result AVMatroskaExtractor::scanClusterOrder(AVExtractorInput& input) {
  const bool seeking = pendingSeek_;
  while (!isEndOfParent(input)) {
    const long elementStart = input.getPosition();
    long id = 0;
    long size = 0;
    if (!readElementHeader(input, id, size)) {
      return kResultEndOfInput;
    }
    if (pushElementFrame(input, size) < 0) {
      popElementFrame();
      return kResultEndOfInput;
    }
    if (id == kCluster) {
      if (seeking) {
        pendingClusterPosition_ = elementStart;
        pendingSeek_ = false;
        return kResultContinue;
      }
      clusterOrder_.push_back(ClusterPosition{0, elementStart});
      if (!skipElement(input)) {
        return kResultEndOfInput;
      }
      parsePosition_ = input.getPosition();
      continue;
    }
    if (id == kInfo) {
      const Result result = parseSegmentInfo(input);
      if (result != kResultContinue) {
        return result;
      }
    } else if (id == kTracks) {
      const Result result = parseTracks(input);
      if (result != kResultContinue) {
        return result;
      }
    } else if (!skipElement(input)) {
      return kResultEndOfInput;
    }
    parsePosition_ = input.getPosition();
  }
  if (!elementFrames_.empty()) {
    popElementFrame();
  }
  return kResultContinue;
}

void AVMatroskaExtractor::parseTagsElement(AVDataBuffer& data) {
  while (data.remaining() > 0) {
    const int64_t id = parseVint(data);
    const int64_t size = parseSize(data);
    if (id <= 0 || size < 0 || static_cast<uint64_t>(size) > data.remaining()) {
      break;
    }
    AVDataBuffer tag(data.array() + data.position(), static_cast<uint32_t>(size));
    data.skipBytes(static_cast<size_t>(size));
    if (id != kTag) {
      continue;
    }
    int64_t trackUid = 0;
    std::vector<std::shared_ptr<Metadata::Entry>> entries;
    std::function<void(AVDataBuffer&)> parseSimpleTag = [&](AVDataBuffer& simpleTag) {
      std::string name;
      std::string value;
      std::string language = "und";
      while (simpleTag.remaining() > 0) {
        const int64_t childId = parseVint(simpleTag);
        const int64_t childSize = parseSize(simpleTag);
        if (childId <= 0 || childSize < 0 ||
            static_cast<uint64_t>(childSize) > simpleTag.remaining()) {
          break;
        }
        AVDataBuffer child(simpleTag.array() + simpleTag.position(), static_cast<uint32_t>(childSize));
        simpleTag.skipBytes(static_cast<size_t>(childSize));
        if (childId == kTagName) {
          name.assign(reinterpret_cast<const char*>(child.array()), child.limit());
        } else if (childId == kTagString) {
          value.assign(reinterpret_cast<const char*>(child.array()), child.limit());
        } else if (childId == kTagLanguage) {
          language.assign(reinterpret_cast<const char*>(child.array()), child.limit());
        } else if (childId == kSimpleTag) {
          // A nested tag qualifies its parent; its value is kept on its own key, as media3 does.
          parseSimpleTag(child);
        }
      }
      if (!name.empty()) {
        std::string key = name;
        for (char& character : key) {
          if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
          }
        }
        entries.push_back(std::make_shared<TextMetadataEntry>(key, value, language));
      }
    };
    while (tag.remaining() > 0) {
      const int64_t childId = parseVint(tag);
      const int64_t childSize = parseSize(tag);
      if (childId <= 0 || childSize < 0 || static_cast<uint64_t>(childSize) > tag.remaining()) {
        break;
      }
      AVDataBuffer child(tag.array() + tag.position(), static_cast<uint32_t>(childSize));
      tag.skipBytes(static_cast<size_t>(childSize));
      if (childId == kTargets) {
        AVDataBuffer targets = std::move(child);
        while (targets.remaining() > 0) {
          const int64_t targetId = parseVint(targets);
          const int64_t targetSize = parseSize(targets);
          if (targetId <= 0 || targetSize < 0 ||
              static_cast<uint64_t>(targetSize) > targets.remaining()) {
            break;
          }
          if (targetId == kTagTrackUid && targetSize > 0 && targetSize <= 8) {
            trackUid = readUnsigned(targets.array() + targets.position(),
                                    static_cast<size_t>(targetSize));
          }
          targets.skipBytes(static_cast<size_t>(targetSize));
        }
      } else if (childId == kSimpleTag) {
        parseSimpleTag(child);
      }
    }
    if (trackUid == 0) {
      globalMetadata_.insert(globalMetadata_.end(), entries.begin(), entries.end());
    } else {
      auto& targeted = trackMetadata_[trackUid];
      targeted.insert(targeted.end(), entries.begin(), entries.end());
    }
  }
  publishMetadata();
}

void AVMatroskaExtractor::parseChaptersElement(AVDataBuffer& data) {
  std::vector<std::shared_ptr<Metadata::Entry>> chapters;
  std::function<void(AVDataBuffer&)> parseChapterAtom = [&](AVDataBuffer& atom) {
    int64_t startNs = 0;
    std::string title;
    while (atom.remaining() > 0) {
      const int64_t id = parseVint(atom);
      const int64_t size = parseSize(atom);
      if (id <= 0 || size < 0 || static_cast<uint64_t>(size) > atom.remaining()) {
        break;
      }
      AVDataBuffer child(atom.array() + atom.position(), static_cast<uint32_t>(size));
      atom.skipBytes(static_cast<size_t>(size));
      if (id == kChapterTimeStart && size > 0 && size <= 8) {
        startNs = readUnsigned(child.array(), child.limit());
      } else if (id == kChapterDisplay) {
        while (child.remaining() > 0) {
          const int64_t displayId = parseVint(child);
          const int64_t displaySize = parseSize(child);
          if (displayId <= 0 || displaySize < 0 ||
              static_cast<uint64_t>(displaySize) > child.remaining()) {
            break;
          }
          if (displayId == kChapString) {
            title.assign(reinterpret_cast<const char*>(child.array() + child.position()),
                         static_cast<size_t>(displaySize));
          }
          child.skipBytes(static_cast<size_t>(displaySize));
        }
      } else if (id == kChapterAtom) {
        parseChapterAtom(child);
      }
    }
    if (!title.empty()) {
      chapters.push_back(std::make_shared<ChapterMetadataEntry>(startNs / 1000, title));
    }
  };
  while (data.remaining() > 0) {
    const int64_t id = parseVint(data);
    const int64_t size = parseSize(data);
    if (id <= 0 || size < 0 || static_cast<uint64_t>(size) > data.remaining()) {
      break;
    }
    AVDataBuffer child(data.array() + data.position(), static_cast<uint32_t>(size));
    data.skipBytes(static_cast<size_t>(size));
    if (id == kEditionEntry) {
      while (child.remaining() > 0) {
        const int64_t editionId = parseVint(child);
        const int64_t editionSize = parseSize(child);
        if (editionId <= 0 || editionSize < 0 ||
            static_cast<uint64_t>(editionSize) > child.remaining()) {
          break;
        }
        AVDataBuffer editionChild(child.array() + child.position(),
                                  static_cast<uint32_t>(editionSize));
        child.skipBytes(static_cast<size_t>(editionSize));
        if (editionId == kChapterAtom) {
          parseChapterAtom(editionChild);
        }
      }
    }
  }
  globalMetadata_.insert(globalMetadata_.end(), chapters.begin(), chapters.end());
  publishMetadata();
}

void AVMatroskaExtractor::publishMetadata() {
  for (Track& track : tracks_) {
    if (track.output == nullptr) {
      continue;
    }
    std::vector<std::shared_ptr<Metadata::Entry>> entries = globalMetadata_;
    const auto targeted = trackMetadata_.find(track.uid);
    if (targeted != trackMetadata_.end()) {
      entries.insert(entries.end(), targeted->second.begin(), targeted->second.end());
    }
    if (entries.empty()) {
      continue;
    }
    auto metadata = std::make_shared<Metadata>(std::vector<std::shared_ptr<Metadata::Entry>>{});
    metadata->appendedEntries(entries);
    std::shared_ptr<MediaFormat> updated = track.publishedFormat != nullptr
                                             ? track.publishedFormat->buildUpon()
                                             : buildFormat(track);
    updated->setMetadata(metadata);
    track.publishedFormat = updated;
    track.output->format(updated);
  }
}
AVExtractor::Result AVMatroskaExtractor::parseSegmentInfo(AVExtractorInput& input) {
  const long size = loadElement(input, currentElementEnd() - input.getPosition());
  if (size < 0) {
    return skipElement(input) ? kResultContinue : kResultEndOfInput;
  }
  while (element_.remaining() > 0) {
    const int64_t id = parseVint(element_);
    const int64_t childSize = parseSize(element_);
    if (id <= 0 || childSize < 0 || static_cast<uint64_t>(childSize) > element_.remaining()) {
      break;
    }
    if (id == kTimecodeScale) {
      std::vector<uint8_t> value;
      element_.readBytes(value, static_cast<size_t>(childSize));
      const int64_t scale = readUnsigned(value.data(), value.size());
      if (scale > 0 && scale <= kMaxTimecodeScale) {
        timecodeScale_ = scale;
      }
    } else if (id == kDuration) {
      std::vector<uint8_t> value;
      element_.readBytes(value, static_cast<size_t>(childSize));
      const double duration = getBigEndianDouble(value.data(), value.size());
      if (duration > 0 && duration < static_cast<double>(std::numeric_limits<int64_t>::max())) {
        durationTicks_ = static_cast<int64_t>(duration);
      }
    } else if (id == kTitle) {
      std::vector<uint8_t> value;
      element_.readBytes(value, static_cast<size_t>(childSize));
      if (value.size() <= 256 && checkUtf8(value.data(), value.size())) {
        title_.assign(reinterpret_cast<const char*>(value.data()), value.size());
        // The title of the segment is the title of the movie. The Matroska muxer writes it as the
        // `Info::Title` element rather than as a tag, so it is published as a text entry here.
        bool hasTitle = false;
        for (const auto& entry : globalMetadata_) {
          const auto* text = dynamic_cast<const TextMetadataEntry*>(entry.get());
          if (text != nullptr && text->getKey() == "title") {
            hasTitle = true;
            break;
          }
        }
        if (!hasTitle) {
          globalMetadata_.push_back(std::make_shared<TextMetadataEntry>("title", title_));
          publishMetadata();
        }
      }
    } else {
      element_.skipBytes(static_cast<size_t>(childSize));
    }
  }
  return advanceElement(input) ? kResultContinue : kResultEndOfInput;
}

AVExtractor::Result AVMatroskaExtractor::parseTracks(AVExtractorInput& input) {
  // The element is loaded from the input before its children are parsed, and the input is moved past it
  // afterwards: parsing the children of the element that is already buffered, and leaving the frame of
  // the element on the stack, read the payload of the element as if it were a child of its parent.
  const long size = loadElement(input, currentElementEnd() - input.getPosition());
  if (size < 0) {
    return skipElement(input) ? kResultContinue : kResultEndOfInput;
  }
  while (element_.remaining() > 0) {
    const int64_t id = parseVint(element_);
    const int64_t size = parseSize(element_);
    if (id <= 0 || size < 0 || static_cast<uint64_t>(size) > element_.remaining()) {
      break;
    }
    const size_t childSize = static_cast<size_t>(size);
    if (id == kTrackEntry && childSize > 0) {
      AVDataBuffer entry(element_.array() + element_.position(), static_cast<uint32_t>(childSize));
      element_.skipBytes(childSize);
      AVDataBuffer saved = std::move(element_);
      element_ = std::move(entry);
      parseTrackEntry();
      element_ = std::move(saved);
    } else {
      element_.skipBytes(childSize);
    }
  }
  return advanceElement(input) ? kResultContinue : kResultEndOfInput;
}

bool AVMatroskaExtractor::parseTrackEntry() {
  Track track = {};
  track.id = -1;
  track.number = 0;
  track.type = -1;
  track.mediaType = MediaType::kMediaTypeDefault;
  track.defaultDurationNs = 0;
  track.delay.codecDelayNs = 0;
  track.delay.seekPreRollNs = 0;
  track.video.pixelWidth = -1;
  track.video.pixelHeight = -1;
  track.video.displayWidth = -1;
  track.video.displayHeight = -1;
  track.video.displayUnit = -1;
  track.video.alphaMode = -1;
  track.video.interlaced = -1;
  track.video.stereoMode = -1;
  track.video.projectionType = -1;
  track.video.rotationDegrees = 0;
  for (int row = 0; row < 3; row++) {
    for (int column = 0; column < 3; column++) {
      track.video.projectionMatrix[row][column] = (row == column) ? 1.0f : 0.0f;
    }
  }
  track.video.colour.matrixCoefficients = -1;
  track.video.colour.transferCharacteristics = -1;
  track.video.colour.primaries = -1;
  track.video.colour.range = -1;
  track.audio.samplingFrequency = -1.0f;
  track.audio.outputSamplingFrequency = -1.0f;
  track.audio.channelCount = -1;
  track.audio.bitDepth = -1;
  track.isSubtitle = false;
  track.hasFirstSample = false;
  track.lastDurationUs = 0;

  while (element_.remaining() > 0) {
    const int64_t id = parseVint(element_);
    const int64_t size = parseSize(element_);
    if (id <= 0 || size < 0 || static_cast<uint64_t>(size) > element_.remaining()) {
      break;
    }
    const size_t childSize = static_cast<size_t>(size);
    if (id == kCodecId) {
      std::vector<uint8_t> value;
      element_.readBytes(value, childSize);
      if (value.size() <= 128) {
        track.codecId.assign(reinterpret_cast<const char*>(value.data()), value.size());
      }
      continue;
    }
    if (id == kCodecPrivate) {
      if (childSize <= static_cast<size_t>(kMaxElementSize)) {
        element_.readBytes(track.codecPrivate, childSize);
      } else {
        element_.skipBytes(childSize);
      }
      continue;
    }
    if (id == kVideo) {
      const ChildElementScope scope(element_, childSize);
      parseVideo(track.video);
      continue;
    }
    if (id == kAudio) {
      const ChildElementScope scope(element_, childSize);
      parseAudio(track.audio);
      continue;
    }
    if (id == kName || id == kLanguage || id == kLanguageIetf) {
      // The name and the language of the track are not reported by this module.
      element_.skipBytes(childSize);
      continue;
    }
    std::vector<uint8_t> value;
    element_.readBytes(value, childSize);
    static_cast<void>(applyTrackInteger(track, static_cast<long>(id),
                                        readUnsigned(value.data(), value.size())));
  }

  if (track.number <= 0 || track.codecId.empty() || !resolveCodec(track)) {
    return false;
  }
  for (const Track& existing : tracks_) {
    if (existing.number == track.number) {
      return false;
    }
  }
  track.id = static_cast<int>(track.number);
  if (output_ != nullptr) {
    track.output = output_->track(track.id, track.mediaType, AVTrackOutput::emptyConsumer());
    if (track.output != nullptr) {
      track.publishedFormat = buildFormat(track);
      track.output->format(track.publishedFormat);
    }
  }
  tracks_.push_back(std::move(track));
  return true;
}

void AVMatroskaExtractor::parseVideo(VideoData& video) {
  while (element_.remaining() > 0) {
    const int64_t id = parseVint(element_);
    const int64_t size = parseSize(element_);
    if (id <= 0 || size < 0 || static_cast<uint64_t>(size) > element_.remaining()) {
      break;
    }
    const size_t childSize = static_cast<size_t>(size);
    if (id == kProjection) {
      const ChildElementScope scope(element_, childSize);
      parseProjection(video);
      continue;
    }
    if (id == kColour) {
      const ChildElementScope scope(element_, childSize);
      parseColour(video.colour);
      continue;
    }
    std::vector<uint8_t> value;
    element_.readBytes(value, childSize);
    const int64_t number = readUnsigned(value.data(), value.size());
    switch (id) {
      case kPixelWidth: {
        video.pixelWidth = static_cast<int>(number);
        break;
      }
      case kPixelHeight: {
        video.pixelHeight = static_cast<int>(number);
        break;
      }
      case kDisplayWidth: {
        video.displayWidth = static_cast<int>(number);
        break;
      }
      case kDisplayHeight: {
        video.displayHeight = static_cast<int>(number);
        break;
      }
      case kDisplayUnit: {
        video.displayUnit = static_cast<int>(number);
        break;
      }
      case kAlphaMode: {
        video.alphaMode = static_cast<int>(number);
        break;
      }
      case kFlagInterlaced: {
        video.interlaced = static_cast<int>(number);
        break;
      }
      case kStereoMode: {
        video.stereoMode = static_cast<int>(number);
        break;
      }
      default: {
        break;
      }
    }
  }
}

void AVMatroskaExtractor::parseAudio(AudioData& audio) {
  while (element_.remaining() > 0) {
    const int64_t id = parseVint(element_);
    const int64_t size = parseSize(element_);
    if (id <= 0 || size < 0 || static_cast<uint64_t>(size) > element_.remaining()) {
      break;
    }
    const size_t childSize = static_cast<size_t>(size);
    std::vector<uint8_t> value;
    element_.readBytes(value, childSize);
    if (id == kSamplingFrequency) {
      audio.samplingFrequency = getBigEndianFloat(value.data(), value.size());
    } else if (id == kOutputSamplingFrequency) {
      audio.outputSamplingFrequency = getBigEndianFloat(value.data(), value.size());
    } else if (id == kChannels) {
      audio.channelCount = static_cast<int>(readUnsigned(value.data(), value.size()));
    } else if (id == kBitDepth) {
      audio.bitDepth = static_cast<int>(readUnsigned(value.data(), value.size()));
    }
  }
}

void AVMatroskaExtractor::parseProjection(VideoData& video) {
  while (element_.remaining() > 0) {
    const int64_t id = parseVint(element_);
    const int64_t size = parseSize(element_);
    if (id <= 0 || size < 0 || static_cast<uint64_t>(size) > element_.remaining()) {
      break;
    }
    const size_t childSize = static_cast<size_t>(size);
    std::vector<uint8_t> value;
    element_.readBytes(value, childSize);
    if (id == kProjectionType) {
      video.projectionType = static_cast<int>(readUnsigned(value.data(), value.size()));
    } else if (id == kProjectionPrivate && childSize == 9 * sizeof(float)) {
      for (int row = 0; row < 3; row++) {
        for (int column = 0; column < 3; column++) {
          const size_t offset = static_cast<size_t>(row * 3 + column) * sizeof(float);
          video.projectionMatrix[row][column] =
            getBigEndianFloat(value.data() + offset, sizeof(float));
        }
      }
    }
  }
  // The pose of a projection is a transformation matrix, of which only the rotation is reported.
  const float* const matrix = &video.projectionMatrix[0][0];
  int best = 0;
  for (int i = 1; i < 9; i++) {
    if (matrix[i] > matrix[best]) {
      best = i;
    }
  }
  const int rotation = ((best * 90) % 360 + 360) % 360;
  int snapped = 0;
  for (const int candidate : kRotations) {
    if (rotation >= candidate) {
      snapped = candidate;
    }
  }
  video.rotationDegrees = snapped;
}

void AVMatroskaExtractor::parseColour(Colour& colour) {
  while (element_.remaining() > 0) {
    const int64_t id = parseVint(element_);
    const int64_t size = parseSize(element_);
    if (id <= 0 || size < 0 || static_cast<uint64_t>(size) > element_.remaining()) {
      break;
    }
    const size_t childSize = static_cast<size_t>(size);
    std::vector<uint8_t> value;
    element_.readBytes(value, childSize);
    const int number = static_cast<int>(readUnsigned(value.data(), value.size()));
    switch (id) {
      case kMatrixCoefficients: {
        colour.matrixCoefficients = number;
        break;
      }
      case kTransferCharacteristics: {
        colour.transferCharacteristics = number;
        break;
      }
      case kPrimaries: {
        colour.primaries = number;
        break;
      }
      case kColourRange: {
        colour.range = number;
        break;
      }
      default: {
        break;
      }
    }
  }
}

bool AVMatroskaExtractor::applyTrackInteger(Track& track, const long id, const int64_t value) {
  switch (id) {
    case kTrackNumber: {
      track.number = value;
      return true;
    }
    case kTrackUid: {
      // The unique identifier is what the targets of the tags refer to.
      track.uid = value;
      return true;
    }
    case kTrackType: {
      track.type = static_cast<int>(value);
      return true;
    }
    case kDefaultDuration: {
      track.defaultDurationNs = value;
      return true;
    }
    case kCodecDelay: {
      track.delay.codecDelayNs = value;
      return true;
    }
    case kSeekPreRoll: {
      track.delay.seekPreRollNs = value;
      return true;
    }
    case kFlagDefault: {
      track.flagDefault = value != 0;
      return true;
    }
    case kFlagForced: {
      track.flagForced = value != 0;
      return true;
    }
    case kFlagLacing: {
      return true;
    }
    default: {
      return false;
    }
  }
}
// ==================================================================================================
// Codec mapping
// ==================================================================================================

bool AVMatroskaExtractor::resolveCodec(Track& track) {
  const std::string& codecId = track.codecId;
  if (codecId == "V_MPEG4/ISO/AVC") {
    track.mimeType = MimeTypes::VIDEO_H264;
    track.mediaType = MediaType::kMediaTypeVideo;
    int width = -1;
    int height = -1;
    std::vector<uint8_t> csd;
    if (parseAvcCodecPrivate(track.codecPrivate.data(), track.codecPrivate.size(), width, height,
                             csd)) {
      // The configuration record declares the size of the length prefix of the NAL units of the blocks,
      // and it is replaced by the codec specific data that the decoder is configured with.
      if (track.codecPrivate.size() >= 5) {
        track.nalLengthSize = (track.codecPrivate[4] & 0x03) + 1;
      }
      if (!csd.empty()) {
        track.codecPrivate = std::move(csd);
      }
      if (track.video.pixelWidth <= 0 && width > 0) {
        track.video.pixelWidth = width;
      }
      if (track.video.pixelHeight <= 0 && height > 0) {
        track.video.pixelHeight = height;
      }
    }
    if (!track.codecPrivate.empty()) {
      track.codecs = "avc1";
    }
  } else if (codecId == "V_MPEGH/ISO/HEVC") {
    track.mimeType = MimeTypes::VIDEO_H265;
    track.mediaType = MediaType::kMediaTypeVideo;
    int width = -1;
    int height = -1;
    std::string codecs;
    if (parseHevcCodecPrivate(track.codecPrivate.data(), track.codecPrivate.size(), width, height,
                              codecs)) {
      track.codecs = codecs;
      // The configuration record declares the size of the length prefix of the NAL units of the blocks.
      if (track.codecPrivate.size() >= 22) {
        track.nalLengthSize = (track.codecPrivate[21] & 0x03) + 1;
      }
      if (track.video.pixelWidth <= 0 && width > 0) {
        track.video.pixelWidth = width;
      }
      if (track.video.pixelHeight <= 0 && height > 0) {
        track.video.pixelHeight = height;
      }
    }
  } else if (codecId == "V_AV1") {
    track.mimeType = MimeTypes::VIDEO_AV1;
    track.mediaType = MediaType::kMediaTypeVideo;
    std::string codecs;
    if (parseAv1CodecPrivate(track.codecPrivate.data(), track.codecPrivate.size(), codecs)) {
      track.codecs = codecs;
    }
  } else if (codecId == "V_VP8") {
    track.mimeType = MimeTypes::VIDEO_VP8;
    track.mediaType = MediaType::kMediaTypeVideo;
  } else if (codecId == "V_VP9") {
    track.mimeType = MimeTypes::VIDEO_VP9;
    track.mediaType = MediaType::kMediaTypeVideo;
  } else if (codecId == "V_MPEG4/ISO/AP" || codecId == "V_MPEG4/ISO/SP" ||
             codecId == "V_MPEG4/ISO/ASP") {
    track.mimeType = MimeTypes::VIDEO_MP4V;
    track.mediaType = MediaType::kMediaTypeVideo;
  } else if (codecId == "V_MPEG2") {
    track.mimeType = MimeTypes::VIDEO_MPEG2;
    track.mediaType = MediaType::kMediaTypeVideo;
  } else if (codecId == "A_AAC" || codecId == "A_AAC/MPEG2/LC" || codecId == "A_AAC/MPEG4/LC") {
    track.mimeType = MimeTypes::AUDIO_AAC;
    track.mediaType = MediaType::kMediaTypeAudio;
  } else if (codecId == "A_AC3") {
    track.mimeType = MimeTypes::AUDIO_AC3;
    track.mediaType = MediaType::kMediaTypeAudio;
  } else if (codecId == "A_EAC3") {
    track.mimeType = MimeTypes::AUDIO_E_AC3;
    track.mediaType = MediaType::kMediaTypeAudio;
  } else if (codecId == "A_TRUEHD") {
    track.mimeType = MimeTypes::AUDIO_TRUEHD;
    track.mediaType = MediaType::kMediaTypeAudio;
  } else if (codecId == "A_DTS") {
    track.mimeType = MimeTypes::AUDIO_DTS;
    track.mediaType = MediaType::kMediaTypeAudio;
  } else if (codecId == "A_AC4") {
    track.mimeType = MimeTypes::AUDIO_AC4;
    track.mediaType = MediaType::kMediaTypeAudio;
  } else if (codecId == "A_MPEG/L3") {
    track.mimeType = MimeTypes::AUDIO_MPEG;
    track.mediaType = MediaType::kMediaTypeAudio;
  } else if (codecId == "A_MPEG/L2") {
    track.mimeType = MimeTypes::AUDIO_MPEG_L2;
    track.mediaType = MediaType::kMediaTypeAudio;
  } else if (codecId == "A_OPUS") {
    track.mimeType = MimeTypes::AUDIO_OPUS;
    track.mediaType = MediaType::kMediaTypeAudio;
  } else if (codecId == "A_VORBIS") {
    track.mimeType = MimeTypes::AUDIO_VORBIS;
    track.mediaType = MediaType::kMediaTypeAudio;
    if (!parseVorbisCodecPrivate(track.codecPrivate.data(), track.codecPrivate.size(), track)) {
      return false;
    }
  } else if (codecId == "A_FLAC") {
    track.mimeType = MimeTypes::AUDIO_FLAC;
    track.mediaType = MediaType::kMediaTypeAudio;
  } else if (codecId == "A_ALAC") {
    track.mimeType = MimeTypes::AUDIO_ALAC;
    track.mediaType = MediaType::kMediaTypeAudio;
  } else if (codecId == "S_TEXT/UTF8") {
    track.mimeType = MimeTypes::APPLICATION_SUBRIP;
    track.mediaType = MediaType::kMediaTypeSubtitle;
    track.isSubtitle = true;
  } else if (codecId == "S_TEXT/WEBVTT") {
    track.mimeType = MimeTypes::TEXT_VTT;
    track.mediaType = MediaType::kMediaTypeSubtitle;
    track.isSubtitle = true;
  } else if (codecId == "S_TEXT/ASS" || codecId == "S_TEXT/SSA") {
    track.mimeType = MimeTypes::TEXT_SSA;
    track.mediaType = MediaType::kMediaTypeSubtitle;
    track.isSubtitle = true;
  } else if (codecId == "S_HDMV/PGS") {
    track.mimeType = MimeTypes::APPLICATION_PGS;
    track.mediaType = MediaType::kMediaTypeSubtitle;
    track.isSubtitle = true;
  } else if (codecId == "S_VOBSUB") {
    track.mimeType = MimeTypes::APPLICATION_VOBSUB;
    track.mediaType = MediaType::kMediaTypeSubtitle;
    track.isSubtitle = true;
  } else {
    return false;
  }

  if (track.mediaType == MediaType::kMediaTypeVideo && track.type != kTrackTypeVideo) {
    return false;
  }
  if (track.mediaType == MediaType::kMediaTypeAudio && track.type != kTrackTypeAudio) {
    return false;
  }
  if (track.isSubtitle && track.type != kTrackTypeSubtitle) {
    return false;
  }
  return true;
}

std::shared_ptr<MediaFormat> AVMatroskaExtractor::buildFormat(const Track& track) const {
  std::shared_ptr<MediaFormat> format;
  if (track.mediaType == MediaType::kMediaTypeAudio) {
    const int sampleRate = track.audio.samplingFrequency > 0
                             ? static_cast<int>(track.audio.samplingFrequency)
                             : Definitions::DEFAULT_SAMPLE_RATE;
    const int channelCount =
      track.audio.channelCount > 0 ? track.audio.channelCount : Definitions::DEFAULT_CHANNEL_COUNT;
    format = MediaFormat::CreateAudioFormat(track.mimeType, sampleRate, channelCount);
  } else if (track.mediaType == MediaType::kMediaTypeVideo) {
    format = MediaFormat::CreateVideoFormat(track.mimeType, track.video.pixelWidth,
                                            track.video.pixelHeight);
    if (track.video.rotationDegrees != 0) {
      format->setRotation(track.video.rotationDegrees);
    }
    // The {@code DefaultDuration} of the track is the duration of one of its frames, which is the
    // rate a composition of the asset schedules its frames with.
    if (track.defaultDurationNs > 0) {
      const double rate = static_cast<double>(kNanosPerSecond) / track.defaultDurationNs;
      if (rate >= 1.0 && rate <= 1000.0) {
        format->setFrameRate(static_cast<int>(std::lround(rate)));
      }
    }
  } else if (track.isSubtitle) {
    format = MediaFormat::CreateSubtitleFormat(track.mimeType);
    // The extractor synthesizes the prefix that makes a subtitle block a document of its own, and
    // the time that prefix states is relative to the block, so the cue is placed at the time of the
    // sample rather than at the time the data says.
    format->setSubsampleOffsetUs(MediaFormat::OFFSET_SAMPLE_RELATIVE);
    format->setCueReplacementBehavior(cueReplacementBehaviorForMimeType(track.mimeType));
    MediaSelectionFlags selectionFlags = kMediaSelectionFlagNone;
    if (track.flagDefault) {
      selectionFlags = static_cast<MediaSelectionFlags>(selectionFlags | kMediaSelectionDefault);
    }
    if (track.flagForced) {
      selectionFlags = static_cast<MediaSelectionFlags>(selectionFlags | kMediaSelectionForced);
    }
    format->setSelectionFlags(selectionFlags);
    format->setRoleFlags(kRoleFlagSubtitle);
  } else {
    format = MediaFormat::CreateAudioFormat(track.mimeType, Definitions::DEFAULT_SAMPLE_RATE,
                                            Definitions::DEFAULT_CHANNEL_COUNT);
  }

  format->setContainerMimeType(MimeTypes::VIDEO_MATROSKA);
  format->setTrackId(track.id);
  format->setCodecs(track.codecs);
  if (durationTicks_ > 0) {
    format->setDurationUs(
      scaleMicros(durationTicks_));
  }
  if (track.isSubtitle && track.mimeType == MimeTypes::TEXT_SSA && !track.codecPrivate.empty()) {
    // A track that states its header once hands the parser the format line of its events and the
    // header of its styles, in that order, as the codec specific data of the format.
    const std::string formatLine = kSsaDialogueFormat;
    format->setCodecSpecificData(0, std::vector<uint8_t>(formatLine.begin(), formatLine.end()));
    format->setCodecSpecificData(1, track.codecPrivate);
  } else if (!track.codecPrivate.empty()) {
    format->setCodecSpecificData(0, track.codecPrivate);
  }
  if (track.mimeType == MimeTypes::AUDIO_OPUS) {
    const int64_t delayUs = track.delay.codecDelayNs / 1000;
    if (delayUs > 0 && delayUs <= AVUtil::kMaxInt) {
      format->setEncoderDelay(static_cast<int>(delayUs));
    }
  }
  std::vector<std::shared_ptr<Metadata::Entry>> entries = globalMetadata_;
  const auto targeted = trackMetadata_.find(track.uid);
  if (targeted != trackMetadata_.end()) {
    entries.insert(entries.end(), targeted->second.begin(), targeted->second.end());
  }
  if (!entries.empty()) {
    auto metadata = std::make_shared<Metadata>(std::vector<std::shared_ptr<Metadata::Entry>>{});
    metadata->appendedEntries(entries);
    format->setMetadata(metadata);
  }
  return format;
}

bool AVMatroskaExtractor::parseAvcCodecPrivate(const uint8_t* data, const size_t size,
                                               int& outWidth, int& outHeight,
                                               std::vector<uint8_t>& outCsd) {
  if (data == nullptr || size < 7) {
    return false;
  }
  std::vector<uint8_t> sps;
  std::vector<uint8_t> pps;
  size_t offset = 5;
  const size_t spsCount = data[offset] & 0x1F;
  offset++;
  for (size_t i = 0; i < spsCount; i++) {
    if (offset + 2 > size) {
      break;
    }
    const size_t length = (static_cast<size_t>(data[offset]) << 8) | data[offset + 1];
    offset += 2;
    if (offset + length > size) {
      break;
    }
    if (sps.empty() && length > 0) {
      sps.assign(data + offset, data + offset + length);
    }
    offset += length;
  }
  if (offset < size) {
    const size_t ppsCount = data[offset];
    offset++;
    for (size_t i = 0; i < ppsCount; i++) {
      if (offset + 2 > size) {
        break;
      }
      const size_t length = (static_cast<size_t>(data[offset]) << 8) | data[offset + 1];
      offset += 2;
      if (offset + length > size) {
        break;
      }
      if (pps.empty() && length > 0) {
        pps.assign(data + offset, data + offset + length);
      }
      offset += length;
    }
  }
  if (sps.empty() || pps.empty()) {
    return false;
  }
  const std::array<const std::vector<uint8_t>*, 2> units = {&sps, &pps};
  outCsd.clear();
  for (const std::vector<uint8_t>* unit : units) {
    // A parameter set of a configuration record holds its one byte NAL header already, and the start code
    // is all that is missing to frame it for an Annex-B sample: writing the header again, as the
    // identifier of the parameter set, produced parameter sets that no decoder accepts, which left every
    // frame of the track undecodable.
    outCsd.push_back(0);
    outCsd.push_back(0);
    outCsd.push_back(0);
    outCsd.push_back(1);
    outCsd.insert(outCsd.end(), unit->begin(), unit->end());
  }
  int width = -1;
  int height = -1;
  if (parseAvcSps(sps, width, height)) {
    outWidth = width;
    outHeight = height;
  }
  return true;
}

bool AVMatroskaExtractor::parseHevcCodecPrivate(const uint8_t* data, const size_t size,
                                                int& outWidth, int& outHeight,
                                                std::string& outCodecs) {
  if (data == nullptr || size < 23 || data[0] != 1) {
    return false;
  }
  const int profileSpace = data[1] >> 6;
  const bool tierFlag = (data[1] & 0x20) != 0;
  const int profileIdc = data[1] & 0x1F;
  char profileSpaceChar = ' ';
  if (profileSpace == 1) {
    profileSpaceChar = 'A';
  } else if (profileSpace == 2) {
    profileSpaceChar = 'B';
  } else if (profileSpace == 3) {
    profileSpaceChar = 'C';
  }
  const int levelIdc = data[12];
  char codec[24] = {0};
  std::snprintf(codec, sizeof(codec), "hvc1.%c%d.%c%d", profileSpaceChar, profileIdc,
                tierFlag ? 'H' : 'L', levelIdc);
  outCodecs.assign(codec);

  std::vector<uint8_t> sps;
  size_t offset = 23;
  const size_t arrayCount = data[22];
  for (size_t i = 0; i < arrayCount && offset + 3 <= size; i++) {
    const uint8_t nalType = data[offset] & 0x3F;
    offset++;
    const size_t nalCount = (static_cast<size_t>(data[offset]) << 8) | data[offset + 1];
    offset += 2;
    for (size_t j = 0; j < nalCount && offset + 2 <= size; j++) {
      const size_t length = (static_cast<size_t>(data[offset]) << 8) | data[offset + 1];
      offset += 2;
      if (offset + length > size) {
        break;
      }
      if (nalType == kHevcSps && sps.empty() && length > 2) {
        sps.assign(data + offset + 2, data + offset + length);
      }
      offset += length;
    }
  }
  int width = -1;
  int height = -1;
  if (!sps.empty() && parseHevcSps(sps, width, height)) {
    outWidth = width;
    outHeight = height;
  }
  return true;
}

bool AVMatroskaExtractor::parseAv1CodecPrivate(const uint8_t* data, const size_t size,
                                               std::string& outCodecs) {
  if (data == nullptr || size < 5 || data[0] != 0x81 || (data[1] & 0x80) != 0) {
    return false;
  }
  const size_t payloadSize = size - 4;
  if (payloadSize < 1 || (data[4] >> 3) != 1) {
    return false;
  }
  const size_t payloadOffset = ((data[4] >> 2) & 0x01) != 0 ? 2 : 1;
  auto buffer = std::make_shared<AVDataBuffer>(data + 4, static_cast<uint32_t>(payloadSize));
  AVBitReader reader(buffer);
  reader.skipBits(static_cast<int>(payloadOffset) * 8);
  if (!reader.canReadBits(4)) {
    return false;
  }
  const int sequenceProfile = readBits(reader, 3);
  const bool stillPicture = readBits(reader, 1) != 0;
  const bool reducedStillPicture = readBits(reader, 1) != 0;
  if (stillPicture || reducedStillPicture) {
    static_cast<void>(readUe(reader));
  }
  if (readBits(reader, 1) != 0) {
    if (readBits(reader, 1) != 0) {
      static_cast<void>(readUe(reader));
      static_cast<void>(readUe(reader));
    }
    if (readBits(reader, 1) != 0) {
      reader.skipBits(32 + 32);
    }
    reader.skipBits(1);
  }
  static_cast<void>(readBits(reader, 1));
  if (stillPicture) {
    reader.skipBits(5);
  } else {
    static_cast<void>(readUe(reader));
  }
  const int sequenceLevel = readBits(reader, 5);
  const bool tier = readBits(reader, 1) != 0;

  char codec[32] = {0};
  std::snprintf(codec, sizeof(codec), "av01.%d.%02d%c", sequenceProfile, sequenceLevel,
                tier ? 'H' : 'M');
  outCodecs.assign(codec);
  return true;
}

bool AVMatroskaExtractor::parseVorbisCodecPrivate(const uint8_t* data, const size_t size,
                                                  Track& track) {
  if (data == nullptr || size < 5 || data[0] != 2 || data[1] != 3) {
    return false;
  }
  const size_t segmentCount = data[1];
  size_t offset = 2;
  std::array<size_t, 3> segmentSizes = {0, 0, 0};
  for (size_t i = 0; i + 1 < segmentCount; i++) {
    size_t value = 0;
    bool terminated = false;
    while (offset < size) {
      const uint8_t byte = data[offset++];
      value += byte;
      if (byte != 0xFF) {
        terminated = true;
        break;
      }
    }
    if (!terminated) {
      return false;
    }
    segmentSizes[i] = value;
  }
  if (offset > size) {
    return false;
  }
  segmentSizes[segmentCount - 1] = size - offset;
  size_t total = 0;
  for (const size_t segmentSize : segmentSizes) {
    total += segmentSize;
  }
  if (total == 0 || offset + total != size) {
    return false;
  }
  // A Vorbis track needs its three headers, which are all carried by the codec specific data.
  track.codecPrivate.assign(data, data + size);
  return true;
}

bool AVMatroskaExtractor::parseAvcSps(const std::vector<uint8_t>& sps, int& outWidth,
                                      int& outHeight) {
  if (sps.size() < 4) {
    return false;
  }
  auto buffer = std::make_shared<AVDataBuffer>(sps);
  AVBitReader reader(buffer);
  readBits(reader, 8);
  readBits(reader, 8);
  readBits(reader, 8);
  readBits(reader, 8);
  const int chromaFormatIdc = readUe(reader);
  if (chromaFormatIdc == 3) {
    readBits(reader, 1);
  }
  static_cast<void>(readUe(reader));
  static_cast<void>(readUe(reader));
  readBits(reader, 1);
  const int picWidthInMbsMinus1 = readUe(reader);
  const int picHeightInMapUnitsMinus1 = readUe(reader);
  const int frameMbsOnlyFlag = readBits(reader, 1);
  if (frameMbsOnlyFlag == 0) {
    readBits(reader, 1);
  }
  readBits(reader, 1);
  static_cast<void>(readUe(reader));
  const int frameCropLeftOffset = readUe(reader);
  const int frameCropRightOffset = readUe(reader);
  const int frameCropTopOffset = readUe(reader);
  const int frameCropBottomOffset = readUe(reader);
  int width = (picWidthInMbsMinus1 + 1) * 16;
  int height = (2 - frameMbsOnlyFlag) * (picHeightInMapUnitsMinus1 + 1) * 16;
  width -= (frameCropLeftOffset + frameCropRightOffset) * 2;
  height -= (frameCropTopOffset + frameCropBottomOffset) * 2;
  if (width <= 0 || height <= 0 || width > 8192 || height > 8192) {
    return false;
  }
  outWidth = width;
  outHeight = height;
  return true;
}

bool AVMatroskaExtractor::parseHevcSps(const std::vector<uint8_t>& sps, int& outWidth,
                                       int& outHeight) {
  if (sps.size() < 6) {
    return false;
  }
  auto buffer = std::make_shared<AVDataBuffer>(sps);
  AVBitReader reader(buffer);
  if (!reader.canReadBits(23)) {
    return false;
  }
  reader.skipBits(23);
  const int maxSubLayersMinusOne = readBits(reader, 3);
  reader.skipBits(1);
  for (int i = 0; i < maxSubLayersMinusOne; i++) {
    reader.skipBits(2);
  }
  for (int i = maxSubLayersMinusOne; i < 8; i++) {
    reader.skipBits(2);
  }
  static_cast<void>(readUe(reader));
  const int chromaFormatIdc = readUe(reader);
  if (chromaFormatIdc == 3) {
    reader.skipBits(1);
  }
  static_cast<void>(readUe(reader));
  static_cast<void>(readUe(reader));
  reader.skipBits(2);
  const int picWidthInLumaSamples = readUe(reader);
  const int picHeightInLumaSamples = readUe(reader);
  if (picWidthInLumaSamples <= 0 || picHeightInLumaSamples <= 0) {
    return false;
  }
  int width = picWidthInLumaSamples;
  int height = picHeightInLumaSamples;
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

bool AVMatroskaExtractor::isValidUtf8(const uint8_t* data, const size_t size) {
  return checkUtf8(data, size);
}

// ==================================================================================================
// Cues
// ==================================================================================================

AVExtractor::Result AVMatroskaExtractor::parseCues(AVExtractorInput& input) {
  const long size = loadElement(input, currentElementEnd() - input.getPosition());
  if (size < 0) {
    return skipElement(input) ? kResultContinue : kResultEndOfInput;
  }
  while (element_.remaining() > 0) {
    const int64_t id = parseVint(element_);
    const int64_t childSize = parseSize(element_);
    if (id <= 0 || childSize < 0 || static_cast<uint64_t>(childSize) > element_.remaining()) {
      break;
    }
    if (id == kCuePoint && childSize > 0) {
      AVDataBuffer point(element_.array() + element_.position(), static_cast<uint32_t>(childSize));
      element_.skipBytes(static_cast<size_t>(childSize));
      AVDataBuffer saved = std::move(element_);
      element_ = std::move(point);
      parseCuePoint();
      element_ = std::move(saved);
    } else {
      element_.skipBytes(static_cast<size_t>(childSize));
    }
  }
  return advanceElement(input) ? kResultContinue : kResultEndOfInput;
}

void AVMatroskaExtractor::parseCuePoint() {
  int64_t cueTime = -1;
  pendingCues_.clear();
  while (element_.remaining() > 0) {
    const int64_t id = parseVint(element_);
    const int64_t size = parseSize(element_);
    if (id <= 0 || size < 0 || static_cast<uint64_t>(size) > element_.remaining()) {
      break;
    }
    const size_t childSize = static_cast<size_t>(size);
    if (id == kCueTime) {
      std::vector<uint8_t> value;
      element_.readBytes(value, childSize);
      cueTime = readUnsigned(value.data(), value.size());
      continue;
    }
    if (id == kCueTrackPositions && childSize > 0) {
      AVDataBuffer positions(element_.array() + element_.position(),
                             static_cast<uint32_t>(childSize));
      element_.skipBytes(childSize);
      AVDataBuffer saved = std::move(element_);
      element_ = std::move(positions);
      pendingCueTrackNumber_ = 0;
      pendingCueClusterPosition_ = 0;
      parseCueTrackPositions();
      if (cueTime >= 0 && pendingCueTrackNumber_ > 0 && pendingCueClusterPosition_ > 0 &&
          segmentStartPosition_ >= 0) {
        SeekEntry entry = {};
        entry.timeUs = -1;
        entry.position = segmentStartPosition_ + static_cast<long>(pendingCueClusterPosition_);
        pendingCues_.push_back(std::make_pair(cueTime, entry));
      }
      element_ = std::move(saved);
      continue;
    }
    element_.skipBytes(childSize);
  }
  for (const auto& cue : pendingCues_) {
    rawCues_.push_back(cue);
  }
  pendingCues_.clear();
}

void AVMatroskaExtractor::parseCueTrackPositions() {
  while (element_.remaining() > 0) {
    const int64_t id = parseVint(element_);
    const int64_t size = parseSize(element_);
    if (id <= 0 || size < 0 || static_cast<uint64_t>(size) > element_.remaining()) {
      break;
    }
    const size_t childSize = static_cast<size_t>(size);
    std::vector<uint8_t> value;
    element_.readBytes(value, childSize);
    const int64_t number = readUnsigned(value.data(), value.size());
    if (id == kCueTrack) {
      pendingCueTrackNumber_ = number;
    } else if (id == kCueClusterPosition) {
      pendingCueClusterPosition_ = number;
    }
  }
}

AVExtractor::Result AVMatroskaExtractor::fillSeekMap() {
  const int64_t durationUs =
    durationTicks_ > 0 ? scaleMicros(durationTicks_)
                       : Definitions::kTimeUnset;

  std::vector<SeekEntry> entries;
  entries.reserve(rawCues_.size());
  for (const auto& cue : rawCues_) {
    SeekEntry entry = {};
    entry.timeUs = scaleMicros(cue.first);
    entry.position = cue.second.position;
    entries.push_back(entry);
  }
  std::sort(entries.begin(), entries.end(), [](const SeekEntry& left, const SeekEntry& right) {
    return left.timeUs < right.timeUs;
  });

  std::vector<AVSeekMap::SeekPoint> seekPoints;
  bool seekable = false;
  if (!entries.empty()) {
    seekPoints.reserve(entries.size());
    for (const SeekEntry& entry : entries) {
      seekPoints.push_back(AVSeekMap::SeekPoint(entry.timeUs, entry.position));
    }
    seekable = true;
  } else if (clusterOrder_.size() > 1) {
    // The container has no cues, the clusters that were scanned are the seek points.
    std::sort(clusterOrder_.begin(), clusterOrder_.end());
    seekPoints.reserve(clusterOrder_.size());
    for (const ClusterPosition& cluster : clusterOrder_) {
      seekPoints.push_back(AVSeekMap::SeekPoint(
        scaleMicros(cluster.timecode),
        cluster.position));
    }
    seekable = true;
  }
  if (output_ != nullptr) {
    output_->seekMap(
      std::make_shared<AVIndexSeekMap>(std::move(seekPoints), durationUs, seekable));
    // Every track of the segment is known once the header elements were scanned, including the
    // tracks that were skipped because their codec is not supported.
    output_->endTracks();
  }
  headersDeclared_ = true;

  if (pendingSeek_) {
    pendingSeek_ = false;
    pendingClusterPosition_ = findCuePosition(pendingSeekTimeUs_);
    if (pendingClusterPosition_ < 0) {
      pendingClusterPosition_ = findClusterPosition(pendingSeekTimeUs_);
    }
    if (pendingClusterPosition_ < 0) {
      pendingClusterPosition_ = segmentStartPosition_;
    }
  }
  phase_ = Phase::kScanClusterOrder;
  return kResultContinue;
}

long AVMatroskaExtractor::findCuePosition(const int64_t timeUs) const {
  long position = -1;
  int64_t best = Definitions::kTimeUnset;
  for (const auto& cue : rawCues_) {
    const int64_t cueTime = scaleMicros(cue.first);
    if (cueTime <= timeUs && (best == Definitions::kTimeUnset || cueTime >= best)) {
      best = cueTime;
      position = cue.second.position;
    }
  }
  return position;
}

long AVMatroskaExtractor::findClusterPosition(const int64_t timeUs) const {
  long position = -1;
  int64_t best = Definitions::kTimeUnset;
  for (const ClusterPosition& cluster : clusterOrder_) {
    const int64_t clusterTime =
      scaleMicros(cluster.timecode);
    if (clusterTime <= timeUs && (best == Definitions::kTimeUnset || clusterTime >= best)) {
      best = clusterTime;
      position = cluster.position;
    }
  }
  return position;
}

// ==================================================================================================
// Cluster parsing
// ==================================================================================================

AVExtractor::Result AVMatroskaExtractor::readCluster(AVExtractorInput& input) {
  if (pendingSeek_ && !clusterFramePushed_) {
    const long restart =
      pendingClusterPosition_ >= 0 ? pendingClusterPosition_ : segmentStartPosition_;
    pendingSeek_ = false;
    pendingClusterPosition_ = -1;
    parsePosition_ = restart;
    if (restart < 0) {
      phase_ = Phase::kSkipContent;
      return kResultEndOfInput;
    }
    if (!input.setPosition(restart, true)) {
      phase_ = Phase::kSkipContent;
      return kResultEndOfInput;
    }
    long id = 0;
    long size = 0;
    if (!readElementHeader(input, id, size) || id != kCluster) {
      phase_ = Phase::kSkipContent;
      return kResultEndOfInput;
    }
    clusterStartPosition_ = input.getPosition();
    clusterTimecode_ = 0;
    for (Track& track : tracks_) {
      // The samples of every track are delivered again from the restart position.
      track.hasFirstSample = false;
      track.lastDurationUs = 0;
    }
    if (pushElementFrame(input, size) < 0) {
      popElementFrame();
      return kResultEndOfInput;
    }
    clusterFramePushed_ = true;
  }

  while (!isEndOfParent(input)) {
    const long elementStart = input.getPosition();
    long id = 0;
    long size = 0;
    if (!readElementHeader(input, id, size)) {
      return kResultEndOfInput;
    }
    if (size >= 0 && size > currentElementEnd() - input.getPosition() &&
        elementStart > clusterStartPosition_) {
      // The element that follows the cluster reaches past its end: either the cluster has no known size
      // and swallowed the element, or its bytes end a little before the element. The bytes of the header
      // of that element were read to tell, and they are given back here, so that the phase which locates
      // the next cluster reads the element from its start. Without the rewind it read the payload of the
      // element as if it were a child of the segment, and the samples of every further cluster were lost.
      if (!input.setPosition(elementStart, false)) {
        return kResultEndOfInput;
      }
      if (clusterFramePushed_) {
        popElementFrame();
        clusterFramePushed_ = false;
      }
      return kResultContinue;
    }
    if (pushElementFrame(input, size) < 0) {
      popElementFrame();
      return kResultEndOfInput;
    }

    Result result = kResultContinue;
    if (id == kClusterTimecode) {
      if (!takeElement(input, size, element_)) {
        popElementFrame();
        return kResultEndOfInput;
      }
      clusterTimecode_ = readUnsigned(element_.array(), element_.limit());
      result = advanceElement(input) ? kResultContinue : kResultEndOfInput;
    } else if (id == kSimpleBlock) {
      const long contentSize = currentElementEnd() - input.getPosition();
      const int64_t trackNumber = contentSize > 0 ? peekVint(input, false) : -1;
      input.resetPeekPosition();
      Track* const track = findTrack(trackNumber);
      if (track == nullptr) {
        result = skipElement(input) ? kResultContinue : kResultEndOfInput;
      } else {
        result = readBlock(input, *track, false);
      }
    } else if (id == kBlockGroup) {
      // The track number of a group is inside its Block element, not at the start of the group.
      result = readBlockGroup(input);
    } else {
      result = skipElement(input) ? kResultContinue : kResultEndOfInput;
    }
    if (result != kResultContinue) {
      return result;
    }
    parsePosition_ = input.getPosition();
  }

  if (clusterFramePushed_) {
    popElementFrame();
    clusterFramePushed_ = false;
  }
  phase_ = Phase::kScanHeaders;
  return kResultContinue;
}

AVExtractor::Result AVMatroskaExtractor::readBlockGroup(AVExtractorInput& input) {
  // The track number of a block group is not at its start: the group holds a Block element and the
  // elements that qualify it, e.g. its duration. The block is therefore read into a buffer, the
  // elements that follow it are read, and only then is the sample delivered: the duration of a
  // subtitle block decides the timecode of the cue that its text becomes.
  bool hasReferenceBlock = false;
  long blockSize = -1;
  blockDurationTicks_ = -1;

  while (!isEndOfParent(input)) {
    long id = 0;
    long size = 0;
    if (!readElementHeader(input, id, size)) {
      return kResultEndOfInput;
    }
    if (pushElementFrame(input, size) < 0) {
      popElementFrame();
      return kResultEndOfInput;
    }
    if (id == kBlock) {
      blockSize = currentElementEnd() - input.getPosition();
      break;
    }
    if (id == kReferenceBlock) {
      hasReferenceBlock = true;
    }
    if (id == kBlockDuration && size > 0 && size <= 8) {
      uint8_t durationBytes[8] = {};
      if (!input.readFully(durationBytes, 0, static_cast<size_t>(size))) {
        popElementFrame();
        return kResultEndOfInput;
      }
      blockDurationTicks_ = static_cast<int64_t>(readUnsigned(durationBytes, static_cast<uint32_t>(size)));
    }
    if (!advanceElement(input)) {
      return kResultEndOfInput;
    }
  }

  if (blockSize < 0) {
    // The group has no block: nothing to deliver, and the frame of the group is popped here so that
    // the caller keeps reading the children of the cluster.
    popElementFrame();
    return kResultContinue;
  }
  const int64_t trackNumber = blockSize > 0 ? peekVint(input, false) : -1;
  input.resetPeekPosition();
  Track* const track = findTrack(trackNumber);

  AVDataBuffer* block = nullptr;
  if (blockSize > 0) {
    if (blockSize <= kMaxElementSize && takeElement(input, static_cast<long>(blockSize), element_)) {
      block = &element_;
    } else if (takeElement(input, static_cast<long>(blockSize), blockData_)) {
      block = &blockData_;
    }
  }
  // The frame of the block is popped whether or not its content was buffered.
  if (!advanceElement(input)) {
    popElementFrame();
    return kResultEndOfInput;
  }

  // The elements that follow the block qualify it: its duration, and the references that tell
  // whether it is a key frame.
  while (!isEndOfParent(input)) {
    long id = 0;
    long size = 0;
    if (!readElementHeader(input, id, size)) {
      break;
    }
    if (pushElementFrame(input, size) < 0) {
      popElementFrame();
      break;
    }
    if (id == kReferenceBlock) {
      hasReferenceBlock = true;
    }
    if (id == kBlockDuration && size > 0 && size <= 8) {
      uint8_t durationBytes[8] = {};
      if (!input.readFully(durationBytes, 0, static_cast<size_t>(size))) {
        popElementFrame();
        break;
      }
      blockDurationTicks_ = static_cast<int64_t>(readUnsigned(durationBytes, static_cast<uint32_t>(size)));
    }
    if (!skipElement(input)) {
      break;
    }
  }

  Result result = kResultContinue;
  if (track != nullptr && block != nullptr) {
    currentBlock_ = block;
    blockTick_ = 0;
    blockKeyFrame_ = !hasReferenceBlock;
    blockFrameCount_ = 1;
    blockContentStart_ = 0;
    blockFrameSizes_.clear();
    if (parseBlockData()) {
      currentTrack_ = track;
      deliverBlock();
    }
    currentBlock_ = nullptr;
    currentTrack_ = nullptr;
  }
  popElementFrame();
  return result;
}
AVExtractor::Result AVMatroskaExtractor::readBlock(AVExtractorInput& input, const Track& track,
                                                   const bool keyFrame) {
  const long size = currentElementEnd() - input.getPosition();
  AVDataBuffer* buffer = nullptr;
  if (size > 0) {
    if (size <= kMaxElementSize && takeElement(input, size, element_)) {
      buffer = &element_;
    } else if (takeElement(input, size, blockData_)) {
      buffer = &blockData_;
    } else {
      phase_ = Phase::kSkipContent;
      return kResultEndOfInput;
    }
  }

  currentBlock_ = buffer;
  blockTick_ = 0;
  blockKeyFrame_ = keyFrame;
  blockFrameCount_ = 1;
  blockContentStart_ = 0;
  blockFrameSizes_.clear();
  blockDurationTicks_ = -1;

  if (buffer != nullptr && parseBlockData()) {
    currentTrack_ = const_cast<Track*>(&track);
    deliverBlock();
  }
  currentBlock_ = nullptr;
  currentTrack_ = nullptr;
  return advanceElement(input) ? kResultContinue : kResultEndOfInput;
}

bool AVMatroskaExtractor::parseBlockData() {
  AVDataBuffer& data = *currentBlock_;
  if (data.remaining() < 3) {
    return false;
  }
  // A block starts with the number of its track, which the caller peeked at to find the track and which
  // is consumed here: reading the timecode without it read the track number as the high byte of the
  // timecode, which left every sample with a timestamp that was roughly minus the track number, and made
  // the blocks of a track whose number is encoded on several bytes unparsable.
  if (parseVint(data) <= 0) {
    return false;
  }
  blockTick_ = data.readShort();
  const uint8_t flags = data.readUnsignedByte();
  blockKeyFrame_ = blockKeyFrame_ || (flags & 0x80) != 0;
  const int lacing = (flags >> 1) & 0x03;

  if (lacing != kLacingNone) {
    if (data.remaining() < 1) {
      return false;
    }
    blockFrameCount_ = static_cast<size_t>(data.readUnsignedByte()) + 1;
    if (blockFrameCount_ > kMaxLacedFrameCount) {
      return false;
    }
  } else {
    blockFrameCount_ = 1;
  }
  blockFrameSizes_.clear();
  blockFrameSizes_.reserve(blockFrameCount_);

  if (blockFrameCount_ == 1) {
    blockFrameSizes_.push_back(data.remaining());
  } else if (lacing == kLacingFixed) {
    if (data.remaining() % blockFrameCount_ != 0) {
      return false;
    }
    const size_t frameSize = data.remaining() / blockFrameCount_;
    for (size_t i = 0; i < blockFrameCount_; i++) {
      blockFrameSizes_.push_back(frameSize);
    }
  } else if (lacing == kLacingXiph) {
    size_t total = 0;
    for (size_t i = 0; i + 1 < blockFrameCount_; i++) {
      size_t frameSize = 0;
      bool terminated = false;
      while (data.remaining() > 0) {
        const uint8_t value = data.readUnsignedByte();
        frameSize += value;
        if (value != 0xFF) {
          terminated = true;
          break;
        }
      }
      if (!terminated) {
        return false;
      }
      blockFrameSizes_.push_back(frameSize);
      total += frameSize;
    }
    if (data.remaining() < total) {
      return false;
    }
    blockFrameSizes_.push_back(data.remaining() - total);
  } else {
    const int64_t firstSize = parseVint(data);
    if (firstSize < 0) {
      return false;
    }
    int64_t frameSize = firstSize;
    blockFrameSizes_.push_back(static_cast<size_t>(frameSize));
    for (size_t i = 1; i < blockFrameCount_; i++) {
      const int64_t difference = parseSignedVint(data);
      if (difference < 0 && static_cast<uint64_t>(-difference) > static_cast<uint64_t>(frameSize)) {
        return false;
      }
      frameSize += difference;
      if (frameSize < 0) {
        return false;
      }
      blockFrameSizes_.push_back(static_cast<size_t>(frameSize));
    }
  }

  size_t total = 0;
  for (const size_t frameSize : blockFrameSizes_) {
    if (frameSize > data.remaining() || total > data.remaining() - frameSize) {
      return false;
    }
    total += frameSize;
  }
  if (total != data.remaining()) {
    return false;
  }
  blockContentStart_ = data.position();
  return true;
}

void AVMatroskaExtractor::deliverBlock() {
  if (currentTrack_ == nullptr) {
    return;
  }

  size_t offset = blockContentStart_;
  for (size_t i = 0; i < blockFrameSizes_.size(); i++) {
    deliverFrame(offset, blockFrameSizes_[i], i, *currentTrack_);
    offset += blockFrameSizes_[i];
  }
}

std::shared_ptr<AVBlockBuffer> AVMatroskaExtractor::buildSubtitleSample(
    const uint8_t* frame, const size_t frameSize, const int64_t durationUs, const Track& track) {
  const uint8_t* body = frame;
  size_t bodySize = frameSize;
  std::string prefix;
  if (track.codecId == "S_TEXT/UTF8") {
    prefix = "1\n00:00:00,000 --> ";
    prefix += formatSubripTimecode(durationUs);
    prefix += "\n";
  } else if (track.codecId == "S_TEXT/ASS" || track.codecId == "S_TEXT/SSA") {
    // A Matroska ASS block is ReadOrder,Layer,Style,Name,MarginL,MarginR,MarginV,Effect,Text.
    // A dialogue line needs Layer,Start,End before Style and must not carry ReadOrder, so the
    // fields are rearranged rather than prefixed as-is.
    std::vector<std::string> fields;
    size_t position = 0;
    const std::string_view payload(reinterpret_cast<const char*>(frame), frameSize);
    for (int field = 0; field < 8; ++field) {
      const size_t comma = payload.find(',', position);
      if (comma == std::string_view::npos) break;
      fields.emplace_back(payload.substr(position, comma - position));
      position = comma + 1;
    }
    if (fields.size() == 8) {
      prefix = "Dialogue: ";
      prefix += fields[1];
      prefix += ",";
      prefix += formatSsaTimecode(0);
      prefix += ",";
      prefix += formatSsaTimecode(durationUs);
      for (size_t i = 2; i < fields.size(); ++i) {
        prefix += ",";
        prefix += fields[i];
      }
      prefix += ",";
      prefix.append(payload.substr(position));
      body = nullptr;
      bodySize = 0;
    } else {
      prefix = "Dialogue: 0,";
      prefix += formatSsaTimecode(0);
      prefix += ",";
      prefix += formatSsaTimecode(durationUs);
      prefix += ",Default,,0,0,0,,";
    }
  } else if (track.codecId == "S_TEXT/WEBVTT") {
    prefix = "WEBVTT\n\n00:00:00.000 --> ";
    prefix += formatWebvttTimecode(durationUs);
    prefix += "\n";
  } else {
    // An image subtitle, or a format whose parser is not ported yet: the block is delivered as it is.
    return std::make_shared<AVBlockBuffer>(frame, static_cast<uint32_t>(frameSize));
  }

  const size_t totalSize = prefix.size() + bodySize;
  if (totalSize > std::numeric_limits<uint32_t>::max()) {
    return std::make_shared<AVBlockBuffer>(frame, static_cast<uint32_t>(frameSize));
  }
  auto buffer = std::make_shared<AVBlockBuffer>(static_cast<uint32_t>(totalSize));
  std::memcpy(buffer->array(), prefix.data(), prefix.size());
  std::memcpy(buffer->array() + prefix.size(), body, bodySize);
  buffer->position(0);
  buffer->limit(static_cast<uint32_t>(totalSize));
  return buffer;
}

void AVMatroskaExtractor::deliverFrame(const size_t frameOffset, const size_t frameSize,
                                       const size_t frameIndex, Track& track) {
  if (frameSize == 0 || currentBlock_ == nullptr || currentBlock_->array() == nullptr) {
    return;
  }
  const uint8_t* const frame = currentBlock_->array() + frameOffset;

  const bool textSubtitle = track.mimeType == MimeTypes::TEXT_VTT ||
                            track.mimeType == MimeTypes::APPLICATION_SUBRIP ||
                            track.mimeType == MimeTypes::TEXT_SSA ||
                            track.mimeType == MimeTypes::APPLICATION_TTML;
  if (track.isSubtitle && textSubtitle && !isValidUtf8(frame, frameSize)) {
    // A text subtitle payload that is not valid UTF-8 is dropped. PGS/VobSub/DVBSub are binary
    // pictures and must reach their parsers byte for byte.
    return;
  }

  const int64_t frameDurationTicks =
    track.defaultDurationNs > 0
      ? AVUtil::scaleLargeValue(track.defaultDurationNs, kNanosPerSecond, timecodeScale_)
      : 0;
  const int64_t frameTick = blockTick_ + static_cast<int64_t>(frameIndex) * frameDurationTicks;
  int64_t timeUs = scaleMicros(clusterTimecode_ + frameTick);
  if (track.mimeType == MimeTypes::AUDIO_OPUS) {
    timeUs -= track.delay.codecDelayNs / 1000;
  }

  int64_t durationUs;
  if (blockDurationTicks_ > 0) {
    durationUs = scaleMicros(blockDurationTicks_);
  } else if (track.defaultDurationNs > 0) {
    durationUs = track.defaultDurationNs / 1000;
  } else {
    // The duration of the previous block is the best estimate of the current one.
    durationUs = track.lastDurationUs;
  }
  track.lastDurationUs = durationUs;

  auto buffer = track.isSubtitle
                  ? buildSubtitleSample(frame, frameSize, durationUs, track)
                  : toAnnexB(frame, frameSize, track);
  uint32_t flags = blockKeyFrame_ ? AVSample::kFlagKeyFrame : AVSample::kFlagNone;
  if (!track.hasFirstSample) {
    flags |= AVSample::kFlagFirstSample;
    track.hasFirstSample = true;
  }
  if (track.output != nullptr) {
    AVSample sample(track.id, timeUs, flags, std::move(buffer));
    track.output->sample(sample);
  }
}

std::shared_ptr<AVBlockBuffer> AVMatroskaExtractor::toAnnexB(const uint8_t* frame,
                                                             const size_t frameSize,
                                                             const Track& track) const {
  const size_t convertedSize =
    track.nalLengthSize > 0 ? AVUtil::annexBSize(frame, frameSize, track.nalLengthSize) : 0;
  if (convertedSize == 0) {
    return std::make_shared<AVBlockBuffer>(frame, static_cast<uint32_t>(frameSize));
  }
  // The decoders of the platforms read NAL units that are separated by start codes, which is the framing
  // the platform extractor converts its length prefixed samples to with its `h264_mp4toannexb` filter.
  //
  // The parameter sets are prepended to an instantaneous decoder refresh picture, as that filter does:
  // the flag of a block that says that it starts a key frame is not the same thing. A block of a track
  // whose codec is not H.264, and the frames of an H.264 track that follow a key frame, carry the flag
  // without being an IDR frame, and writing the parameter sets in the middle of the stream makes the
  // decoders of the platform set their picture order state up again: the frames are handed out in an
  // order that is not the order of the stream and every one of them is late.
  const bool isIdr = track.mimeType == MimeTypes::VIDEO_H264 &&
                     frameSize > static_cast<size_t>(track.nalLengthSize) &&
                     (frame[track.nalLengthSize] & 0x1F) == kH264IdrNal;
  const bool prefixParameterSets = isIdr && !track.codecPrivate.empty();
  const size_t prefixSize = prefixParameterSets ? track.codecPrivate.size() : 0;
  auto buffer = std::make_shared<AVBlockBuffer>(static_cast<uint32_t>(convertedSize + prefixSize));
  if (prefixSize > 0) {
    std::copy(track.codecPrivate.begin(), track.codecPrivate.end(), buffer->array());
  }
  AVUtil::toAnnexB(frame, frameSize, track.nalLengthSize, buffer->array() + prefixSize);
  buffer->position(0);
  buffer->limit(convertedSize + prefixSize);
  return buffer;
}

int64_t AVMatroskaExtractor::scaleMicros(const int64_t timecode) const {
  // A timecode counts ticks of `scale` nanoseconds, so it is converted to nanoseconds before it is
  // converted to microseconds: scaling it by the microseconds of a second over the nanoseconds of a
  // tick measured every timecode in a unit that is the inverse of the one it is expressed in, which made
  // every timestamp of a container whose ticks are milliseconds a thousand times too short.
  const int64_t scale = timecodeScale_ > 0 ? timecodeScale_ : kDefaultTimecodeScale;
  return AVUtil::scaleLargeValue(timecode * scale, kNanosPerSecond, kMicrosPerSecond);
}

AVExtractor::Result AVMatroskaExtractor::skipContent(AVExtractorInput& input) {
  input.skipFully();
  return kResultEndOfInput;
}

AVMatroskaExtractor::Track* AVMatroskaExtractor::findTrack(const int64_t number) {
  for (Track& track : tracks_) {
    if (track.number == number) {
      return &track;
    }
  }
  return nullptr;
}

} // namespace coral
