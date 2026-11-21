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

#include <extractor/container/OggExtractor.h>

#include <extractor/DataBuffer.h>
#include <extractor/Utility.h>
#include <extractor/metadata/VorbisCommentParser.h>

#include <media/BlockBuffer.h>
#include <media/Format.h>
#include <media/Types.h>
#include <media/Utilities.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace coral {

namespace {

/** The capture pattern that starts the header of every Ogg page. */
constexpr uint32_t kCapturePattern = 0x4F676753;  // "OggS"

/** The size of the header of a page, without its segment table. */
constexpr int kEmptyPageHeaderSize = 27;

/** The maximum number of segments of a page, i.e. the size of its segment table in bytes. */
constexpr int kMaxSegmentCount = 255;

/** The maximum number of body bytes of a page, i.e. 255 segments of 255 bytes. */
constexpr int kMaxPagePayload = 255 * 255;

/** The maximum number of bytes of a packet that the sniffing inspects. */
constexpr int kMaxVerificationBytes = 8;

/** The header type flag of a page that continues the packet of the previous page. */
constexpr int kHeaderTypeContinued = 0x01;

/** The header type flag of the page that begins a bitstream. */
constexpr int kHeaderTypeBeginOfStream = 0x02;

/** The header type flag of the page that ends a bitstream. */
constexpr int kHeaderTypeEndOfStream = 0x04;

/** The value of the target granule of a stream that is not being seeked. */
constexpr int64_t kNoTargetGranule = -1;

/**
 * The maximum number of page boundaries that the seek map indexes. A longer stream drops every other
 * point rather than growing without bound, see {@link AVOggSeekMap::addPoint}.
 */
constexpr size_t kMaxSeekPoints = 4096;

/**
 * The maximum number of bytes that a single read consumes while it resolves a seek. A caller that
 * stops reading when the reader reports the end of the input observes the samples of the requested
 * position as long as the distance to it is smaller than this budget; a longer seek is spread over
 * several calls.
 */
constexpr long kMaxSeekScanSize = 4 * 1024 * 1024;

/** The signature of a Vorbis identification header, a comment header and a setup header. */
constexpr uint8_t kVorbisIdentification = 0x01;
constexpr uint8_t kVorbisComment = 0x03;
constexpr uint8_t kVorbisSetup = 0x05;
constexpr char kVorbisSignature[] = "vorbis";

/** The signatures of the two Opus headers, as defined by RFC 7845. */
constexpr char kOpusIdHeader[] = "OpusHead";
constexpr char kOpusCommentHeader[] = "OpusTags";

/** The sample rate of an Opus stream, which is always 48000 Hz. */
constexpr int kOpusSampleRate = 48000;

/** The number of samples of the seek pre-roll of an Opus stream. */
constexpr int kOpusSeekPreRollSamples = 3840;

/** The packet type of the Ogg mapping of a FLAC stream, i.e. the byte before its "FLAC" four cc. */
constexpr uint8_t kFlacPacketType = 0x7F;
constexpr uint32_t kFlacSignature = 0x464C4143;

/** The "fLaC" marker that precedes the metadata block of a FLAC stream. */
constexpr uint32_t kFlacStreamMarker = 0x664C6143;

/** The offset of the STREAMINFO block of the Ogg mapping of a FLAC stream. */
constexpr size_t kFlacStreamInfoOffset = 17;

/** The size of a STREAMINFO block, in bytes. */
constexpr size_t kFlacStreamInfoSize = 34;

/** The metadata block type of the seek table of a FLAC stream. */
constexpr int kFlacMetadataTypeSeekTable = 3;

/** The first byte of a FLAC frame, i.e. the packet type of the audio packets of the stream. */
constexpr uint8_t kFlacAudioPacketType = 0xFF;

/** The number of bytes that precede the sample number of a FLAC frame header. */
constexpr int kFlacFrameHeaderSampleNumberOffset = 4;

/** The largest FLAC frame that the extractor is willing to describe. */
constexpr int kMaxFrameSize = 16 * 1024 * 1024;

/** The largest number of metadata values of a Vorbis codebook that is accepted. */
constexpr int64_t kMaxLookupValues = 1 << 24;

/** The largest number of bits that a Vorbis codebook lookup may span. */
constexpr int64_t kMaxLookupBits = 1 << 26;

/**
 * Reads an unsigned little endian integer from a byte array.
 */
uint32_t readLittleEndianUnsignedInt(const uint8_t data[], const int offset) {
  return static_cast<uint32_t>(data[offset]) | (static_cast<uint32_t>(data[offset + 1]) << 8) |
         (static_cast<uint32_t>(data[offset + 2]) << 16) |
         (static_cast<uint32_t>(data[offset + 3]) << 24);
}

/**
 * Reads a signed 64 bit little endian value from a byte array.
 */
int64_t readLittleEndianLong(const uint8_t data[], const int offset) {
  const uint64_t value =
    static_cast<uint64_t>(readLittleEndianUnsignedInt(data, offset)) |
    (static_cast<uint64_t>(readLittleEndianUnsignedInt(data, offset + 4)) << 32);
  return static_cast<int64_t>(value);
}

/**
 * Writes a 32 bit little endian value at the given offset of a byte array.
 */
void writeLittleEndianUnsignedInt(uint8_t data[], const size_t offset, const uint32_t value) {
  data[offset] = static_cast<uint8_t>(value & 0xFF);
  data[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
  data[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
  data[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

/**
 * The header of one Ogg page.
 *
 * <p>This is the extractor module counterpart of ExoPlayer's {@code OggPageHeader}. The segment
 * table of a page is stored in the instance itself, therefore an instance owns everything it
 * describes and needs no destructor work.
 *
 * <p>The granule position of a page is the total number of samples of the bitstream up to the end of
 * the last packet that completes in it, which is what makes the timestamps of the packets exact.
 */
class AVOggPageHeader final {
public:
  AVOggPageHeader() noexcept {
    reset();
  }

  ~AVOggPageHeader() = default;

  AVOggPageHeader(const AVOggPageHeader& other) = delete;

  AVOggPageHeader& operator=(const AVOggPageHeader& other) = delete;

  /** Resets every field, so that the header describes no page at all. */
  void reset() {
    revision_ = 0;
    type_ = 0;
    granulePosition_ = 0;
    streamSerialNumber_ = 0;
    pageSequenceNumber_ = 0;
    pageChecksum_ = 0;
    pageSegmentCount_ = 0;
    headerSize_ = 0;
    bodySize_ = 0;
    laces_.fill(0);
  }

  /**
   * Advances the input to the capture pattern of the next page.
   *
   * <p>The read and the peek positions are left at the capture pattern when it was found, and at the
   * limit, or at the end of the input, when it was not. The counterpart of
   * {@code OggPageHeader.skipToNextPage}.
   *
   * @param input The input to scan.
   * @param limit The position at which the scan stops, or -1 to allow scanning to the end.
   *
   * @return {@code true} when a capture pattern was found.
   */
  bool skipToNextPage(AVExtractorInput& input, const long limit = -1) {
    input.resetPeekPosition();
    uint8_t scratch[4];
    while (limit < 0 || input.getPosition() + 4 < limit) {
      const long peeked = input.peek(scratch, 0, sizeof(scratch));
      if (peeked < static_cast<long>(sizeof(scratch))) {
        break;
      }
      if (AVUtil::readUnsignedInt(scratch, 0) == kCapturePattern) {
        input.resetPeekPosition();
        return true;
      }
      // The capture pattern was not found at this position, advance one byte and look again.
      input.resetPeekPosition();
      if (!input.skipFully(1)) {
        break;
      }
    }
    // Move the read position to the limit, or to the end of the input.
    while (limit < 0 || input.getPosition() < limit) {
      if (input.skip(1) == kEndOfInput) {
        break;
      }
    }
    input.resetPeekPosition();
    return false;
  }

  /**
   * Peeks the header of the page that starts at the current peek position and fills the fields of
   * this instance. The read position is not moved, and the peek position is left after the segment
   * table of the page.
   *
   * <p>The counterpart of {@code OggPageHeader.populate}. The Java implementation reports a header
   * that it cannot parse either by returning false, when it is quiet, or by throwing an exception;
   * this implementation always returns false, because a container that the extractor cannot parse
   * ends the stream rather than failing the whole read.
   *
   * @param input The input to peek from.
   *
   * @return {@code true} when the header was entirely read.
   */
  bool populate(AVExtractorInput& input) {
    reset();
    uint8_t scratch[kEmptyPageHeaderSize];
    input.resetPeekPosition();
    const long peeked = input.peek(scratch, 0, sizeof(scratch));
    if (peeked < static_cast<long>(sizeof(scratch)) ||
        AVUtil::readUnsignedInt(scratch, 0) != kCapturePattern) {
      return false;
    }
    revision_ = scratch[4];
    if (revision_ != 0x00) {
      return false;
    }
    type_ = scratch[5];
    granulePosition_ = readLittleEndianLong(scratch, 6);
    streamSerialNumber_ = readLittleEndianUnsignedInt(scratch, 14);
    pageSequenceNumber_ = readLittleEndianUnsignedInt(scratch, 18);
    pageChecksum_ = readLittleEndianUnsignedInt(scratch, 22);
    pageSegmentCount_ = scratch[26];
    headerSize_ = kEmptyPageHeaderSize + pageSegmentCount_;

    uint8_t laces[kMaxSegmentCount];
    const long peekedLaces = input.peek(laces, 0, static_cast<size_t>(pageSegmentCount_));
    if (peekedLaces < pageSegmentCount_) {
      return false;
    }
    for (int i = 0; i < pageSegmentCount_; ++i) {
      laces_[static_cast<size_t>(i)] = laces[i];
      bodySize_ += laces[i];
    }
    return true;
  }

  /** Returns the header type flags of the page, a combination of the {@code kHeaderType*} values. */
  [[nodiscard]] int type() const { return type_; }

  /** Returns the granule position of the last packet that completes in the page. */
  [[nodiscard]] int64_t granulePosition() const { return granulePosition_; }

  [[nodiscard]] int pageSegmentCount() const { return pageSegmentCount_; }

  /** Returns the size of the header of the page, its segment table included. */
  [[nodiscard]] int headerSize() const { return headerSize_; }

  /** Returns the size of the body of the page. */
  [[nodiscard]] int bodySize() const { return bodySize_; }

  /** Returns the value of the segment at the given index. */
  [[nodiscard]] int lace(const int index) const { return laces_[static_cast<size_t>(index)]; }

private:
  int revision_;
  int type_;
  int64_t granulePosition_;
  int64_t streamSerialNumber_;
  int64_t pageSequenceNumber_;
  int64_t pageChecksum_;
  int pageSegmentCount_;
  int headerSize_;
  int bodySize_;
  std::array<uint8_t, kMaxSegmentCount> laces_;
};

/**
 * Receives the pages that the packet reader consumes, so that the seek map can index them.
 *
 * <p>The interface exists so that the packet reader does not have to know how the pages are indexed,
 * and so that the pages that were consumed before the headers were parsed are simply not indexed.
 * It plays the role of the pages that ExoPlayer's {@code DefaultOggSeeker} reads to build its
 * {@code OggSeekMap}.
 */
class AVOggPageIndexer {
public:
  virtual ~AVOggPageIndexer() = default;

  AVOggPageIndexer(const AVOggPageIndexer& other) = delete;

  AVOggPageIndexer& operator=(const AVOggPageIndexer& other) = delete;

  /**
   * Called for every page that the packet reader starts, before its body is read.
   *
   * @param pageStartPosition The position of the capture pattern of the page.
   * @param granuleBeforePage The granule position of the page that precedes it, i.e. the granule at
   *                          which the first packet that starts in the page begins.
   * @param header The header of the page.
   */
  virtual void onPage(long pageStartPosition, int64_t granuleBeforePage,
                      const AVOggPageHeader& header) = 0;

protected:
  AVOggPageIndexer() = default;
};

/**
 * The seek map of an Ogg container.
 *
 * <p>The map indexes the pages that the extraction visited: every page boundary becomes a seek point
 * whose time is the granule position of the preceding page, converted to microseconds. A seek that
 * resolves to a page boundary therefore resumes at a packet that the codec can decode, and a seek to
 * a time that was not indexed yet resolves to the closest point that precedes it, from which the
 * stream reader skips the packets it does not need.
 *
 * <p>This is the extractor module counterpart of ExoPlayer's {@code OggSeekMap}, the
 * {@code SeekMap} that {@code DefaultOggSeeker} builds. Unlike the Java implementation, which
 * estimates a byte position from the granule of the target and then binary searches the pages of the
 * stream, this implementation interpolates nothing: it indexes the page boundaries it observes and
 * lets the stream reader skip forward, which is what the reading loop of this module requires, see
 * {@link AVOggExtractor::seek}.
 *
 * <p>The map is mutated while the extraction runs, because an Ogg container only reveals its
 * duration once its last page was read. The reader owns the shared pointer it was given and observes
 * the updated values, which is what {@link AVExtractorOutput#seekMap} documents.
 */
class AVOggSeekMap final : public AVSeekMap, public AVOggPageIndexer {
public:
  AVOggSeekMap(const int sampleRate, const long payloadStartPosition) noexcept
    : sampleRate_(sampleRate),
      payloadStartPosition_(payloadStartPosition) {
    // A seek to the beginning of the stream restarts the container, which is the only position that
    // is always known: an Ogg container carries no index of its pages.
    seekPoints_.push_back(SeekPoint(0, 0));
    seekGranules_.push_back(0);
  }

  ~AVOggSeekMap() override = default;

  AVOggSeekMap(const AVOggSeekMap& other) = delete;

  AVOggSeekMap& operator=(const AVOggSeekMap& other) = delete;

  // ================================================================================================
  // The page index
  // ================================================================================================

  void onPage(const long pageStartPosition, const int64_t granuleBeforePage,
              const AVOggPageHeader& header) override {
    lastGranulePosition_ = std::max(lastGranulePosition_, header.granulePosition());
    if ((header.type() & kHeaderTypeEndOfStream) != 0) {
      // The page that ends the stream declares the total number of samples of the stream.
      totalGranules_ = std::max(totalGranules_, header.granulePosition());
    }
    if (pageStartPosition < payloadStartPosition_) {
      // The page belongs to the headers of the stream: seeking into it would deliver a header as if
      // it were a sample.
      return;
    }
    if (pageStartPosition <= seekPoints_.back().position) {
      // The page was indexed already, e.g. because the extraction restarted after a seek.
      return;
    }
    addPoint(convertGranuleToTime(granuleBeforePage), pageStartPosition, granuleBeforePage);
  }

  /**
   * Reports that the end of the input was reached, i.e. that the duration of the stream is the
   * granule position of the last page that was read. The counterpart of the granule that
   * {@code DefaultOggSeeker.readGranuleOfLastPage} reads from the last page of the stream.
   */
  void onEndOfInput() { totalGranules_ = std::max(totalGranules_, lastGranulePosition_); }

  /**
   * Returns the granule at which the packet that starts at the given position begins, i.e. the last
   * granule that the index knows at or before that position.
   */
  [[nodiscard]] int64_t granuleForPosition(const long position) const {
    const auto it = std::upper_bound(
      seekPoints_.begin(), seekPoints_.end(), position,
      [](const long value, const SeekPoint& point) { return value < point.position; });
    if (it == seekPoints_.begin()) {
      return 0;
    }
    const size_t index = static_cast<size_t>(it - seekPoints_.begin()) - 1;
    return index < seekGranules_.size() ? seekGranules_[index] : 0;
  }

  // ================================================================================================
  // AVSeekMap
  // ================================================================================================

  [[nodiscard]] bool isSeekable() const override { return true; }

  [[nodiscard]] int64_t getDurationUs() const override {
    return totalGranules_ > 0 ? convertGranuleToTime(totalGranules_) : Definitions::kTimeUnset;
  }

  [[nodiscard]] const std::vector<SeekPoint>& getSeekPoints() const override {
    return seekPoints_;
  }

  bool getSeekPoints(const int64_t timeUs, SeekPoint& first, SeekPoint& second) const override {
    if (seekPoints_.empty()) {
      return false;
    }
    // The first point is the last one whose time is not greater than the requested time, the second
    // one is the first point after it.
    const auto it = std::upper_bound(
      seekPoints_.begin(), seekPoints_.end(), timeUs,
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

private:
  /** Converts a granule position to a presentation timestamp. */
  [[nodiscard]] int64_t convertGranuleToTime(const int64_t granule) const {
    if (sampleRate_ <= 0) {
      return Definitions::kTimeUnset;
    }
    return granule * AVUtil::kMicrosPerSecond / sampleRate_;
  }

  /**
   * Appends one seek point, keeping the index bounded.
   *
   * <p>A stream that is long enough to fill the index keeps every other point rather than growing
   * without bound: the granularity of a seek becomes coarser, and the stream reader skips the
   * packets between the point and the requested time, which it does anyway when the requested time
   * was never indexed.
   */
  void addPoint(const int64_t timeUs, const long position, const int64_t granule) {
    if (seekPoints_.size() >= kMaxSeekPoints) {
      size_t kept = 0;
      for (size_t i = 0; i < seekPoints_.size(); i += 2, ++kept) {
        seekPoints_[kept] = seekPoints_[i];
        seekGranules_[kept] = seekGranules_[i];
      }
      seekPoints_.resize(kept);
      seekGranules_.resize(kept);
    }
    seekPoints_.push_back(SeekPoint(timeUs, position));
    seekGranules_.push_back(granule);
  }

  int sampleRate_;
  long payloadStartPosition_;
  int64_t totalGranules_ = 0;
  int64_t lastGranulePosition_ = 0;
  std::vector<SeekPoint> seekPoints_;
  /** The granule at which the packet of the seek point of the same index begins. */
  std::vector<int64_t> seekGranules_;
};

/**
 * Reads the packets of an Ogg bitstream, which are the units of data that the codec readers parse.
 *
 * <p>A packet is the concatenation of the segments of one or more pages: a segment that is shorter
 * than 255 bytes ends a packet, and a segment of exactly 255 bytes means that the packet continues
 * in the next page. The class therefore owns the header of the page it is reading and the buffer
 * that accumulates the packet.
 *
 * <p>This is the extractor module counterpart of ExoPlayer's {@code OggPacket}.
 */
class AVOggPacket final {
public:
  AVOggPacket() noexcept
    : packet_(static_cast<uint32_t>(kMaxPagePayload)) {
    reset();
  }

  ~AVOggPacket() = default;

  AVOggPacket(const AVOggPacket& other) = delete;

  AVOggPacket& operator=(const AVOggPacket& other) = delete;

  /** Resets the reader, which discards the packet that was being assembled. */
  void reset() {
    pageHeader_.reset();
    packet_.reset(0);
    currentSegmentIndex_ = -1;
    segmentCount_ = 0;
    populated_ = false;
    previousPageGranule_ = 0;
  }

  /**
   * Sets the granule that precedes the next page that will be read, which a seek uses to keep the
   * granule accounting of the packets exact.
   */
  void setPreviousPageGranule(const int64_t granule) { previousPageGranule_ = granule; }

  /** Sets the receiver of the pages that are consumed, or null to stop indexing them. */
  void setPageIndexer(AVOggPageIndexer* const indexer) { pageIndexer_ = indexer; }

  /**
   * Reads the next packet of the bitstream.
   *
   * <p>The counterpart of {@code OggPacket.populate}. A packet that spans several pages is
   * accumulated across the calls, therefore a call that reports the end of the input must not be
   * followed by another call on the same instance unless the input was repositioned.
   *
   * @param input The input to read from.
   *
   * @return {@code true} when a whole packet was read, in which case {@link #getPayload} holds it.
   */
  bool populate(AVExtractorInput& input) {
    if (populated_) {
      populated_ = false;
      packet_.reset(0);
    }
    while (!populated_) {
      if (currentSegmentIndex_ < 0) {
        // The packet starts in a new page.
        if (!pageHeader_.skipToNextPage(input) || !pageHeader_.populate(input)) {
          return false;
        }
        const long pageStartPosition = input.getPosition();
        if (pageIndexer_ != nullptr) {
          pageIndexer_->onPage(pageStartPosition, previousPageGranule_, pageHeader_);
        }
        previousPageGranule_ = pageHeader_.granulePosition();

        int segmentIndex = 0;
        long bytesToSkip = pageHeader_.headerSize();
        if ((pageHeader_.type() & kHeaderTypeContinued) != 0 && packet_.limit() == 0) {
          // The first packet of the page is the remainder of a packet that started in a page that
          // was not read, e.g. after a seek, and it cannot be decoded on its own: it is discarded.
          bytesToSkip += calculatePacketSize(0);
          segmentIndex += segmentCount_;
        }
        if (!input.skipFully(bytesToSkip)) {
          return false;
        }
        currentSegmentIndex_ = segmentIndex;
      }

      const int size = calculatePacketSize(currentSegmentIndex_);
      if (size == 0 && segmentCount_ == 0) {
        // The segment index is beyond the segment table, which a malformed page may describe.
        return false;
      }
      const int segmentIndex = currentSegmentIndex_ + segmentCount_;
      if (size > 0) {
        const size_t limit = packet_.limit();
        packet_.ensureCapacity(limit + static_cast<size_t>(size));
        if (!input.readFully(packet_.array() + limit, 0, static_cast<size_t>(size))) {
          return false;
        }
        packet_.limit(limit + static_cast<size_t>(size));
        populated_ = pageHeader_.lace(segmentIndex - 1) != 255;
      }
      currentSegmentIndex_ = segmentIndex == pageHeader_.pageSegmentCount() ? -1 : segmentIndex;
    }
    return true;
  }

  /**
   * Returns the payload of the packet that was read, positioned at its first byte. The buffer is
   * reused by the next call, therefore it must not be kept beyond the call that consumes it.
   */
  [[nodiscard]] AVDataBuffer& getPayload() { return packet_; }

private:
  /**
   * Returns the size of the packet that starts at the given segment, and sets {@link segmentCount_}
   * to the number of segments it spans.
   */
  int calculatePacketSize(const int startSegmentIndex) {
    segmentCount_ = 0;
    int size = 0;
    while (startSegmentIndex + segmentCount_ < pageHeader_.pageSegmentCount()) {
      const int segmentLength = pageHeader_.lace(startSegmentIndex + segmentCount_);
      ++segmentCount_;
      size += segmentLength;
      if (segmentLength != 255) {
        // A packet ends at the first segment that is shorter than 255 bytes.
        break;
      }
    }
    return size;
  }

  AVOggPageHeader pageHeader_;
  AVDataBuffer packet_;
  AVOggPageIndexer* pageIndexer_ = nullptr;
  int currentSegmentIndex_ = -1;
  int segmentCount_ = 0;
  bool populated_ = false;
  int64_t previousPageGranule_ = 0;
};

/**
 * A bit reader that reads the bits of a Vorbis setup header in the order in which the encoder wrote
 * them, i.e. from the least significant bit of every byte.
 *
 * <p>This is the extractor module counterpart of ExoPlayer's {@code VorbisBitArray}, which exists
 * because the {@link AVBitReader} of this module reads the most significant bit first, as the codec
 * configuration records of the other containers do. The class does not own the data it reads,
 * therefore the caller must keep the buffer alive.
 */
class AVVorbisBitArray final {
public:
  AVVorbisBitArray(const uint8_t* const data, const size_t size) noexcept
    : data_(data),
      size_(size) {
  }

  ~AVVorbisBitArray() = default;

  AVVorbisBitArray(const AVVorbisBitArray& other) = delete;

  AVVorbisBitArray& operator=(const AVVorbisBitArray& other) = delete;

  /**
   * Reads the given number of bits, the first bit that is read being the least significant bit of
   * the result. A read past the end of the data returns zero bits.
   */
  [[nodiscard]] uint32_t readBits(const int numBits) {
    uint32_t value = 0;
    for (int i = 0; i < numBits; ++i) {
      const auto byteIndex = static_cast<size_t>(bitPosition_) / 8;
      const int bitIndex = bitPosition_ % 8;
      const uint32_t bit =
        byteIndex < size_ ? (static_cast<uint32_t>(data_[byteIndex]) >> bitIndex) & 1 : 0;
      value |= bit << i;
      ++bitPosition_;
    }
    return value;
  }

  /** Reads one bit. */
  [[nodiscard]] bool readBit() { return readBits(1) != 0; }

  /** Skips the given number of bits. */
  void skipBits(const int numBits) { bitPosition_ += numBits; }

  /** Returns the current bit position. */
  [[nodiscard]] int getPosition() const { return bitPosition_; }

private:
  const uint8_t* data_;
  size_t size_;
  int bitPosition_ = 0;
};

/**
 * The parsers of the Vorbis headers and packets.
 *
 * <p>This is the extractor module counterpart of ExoPlayer's {@code VorbisUtil}, which lives in the
 * container module of the Java implementation.
 */
class AVVorbisUtil final {
public:
  /** The identification header of a Vorbis stream. */
  struct IdHeader {
    /** The version of the bitstream, which is 0 for every Vorbis I stream. */
    int version = 0;
    /** The number of audio channels. */
    int channels = 0;
    /** The sample rate, in Hz. */
    int sampleRate = 0;
    /** The maximum bitrate, or {@link MediaFormat::NO_VALUE} when the stream declares none. */
    int bitrateMaximum = MediaFormat::NO_VALUE;
    /** The nominal bitrate, or {@link MediaFormat::NO_VALUE} when the stream declares none. */
    int bitrateNominal = MediaFormat::NO_VALUE;
    /** The minimum bitrate, or {@link MediaFormat::NO_VALUE} when the stream declares none. */
    int bitrateMinimum = MediaFormat::NO_VALUE;
    /** The small block size, in samples. */
    int blockSize0 = 0;
    /** The large block size, in samples. */
    int blockSize1 = 0;
    /** Whether the framing bit of the header is set, which every valid header does. */
    bool framingFlag = false;
    /** The raw bytes of the header, which the decoder reads as its first configuration buffer. */
    std::vector<uint8_t> data;
  };

  /** One of the modes that the setup header declares. */
  struct Mode {
    /** Whether the mode uses the large block size. */
    bool blockFlag = false;
    int windowType = 0;
    int transformType = 0;
    int mapping = 0;
  };

  /**
   * Returns the index of the highest set bit of the given value, as the Vorbis specification
   * defines {@code ilog}.
   */
  static int iLog(int x) {
    int value = 0;
    while (x > 0) {
      ++value;
      x >>= 1;
    }
    return value;
  }

  /**
   * Reads and validates the seven byte prefix of a Vorbis header, i.e. its packet type and the
   * "vorbis" signature. The counterpart of {@code VorbisUtil.verifyVorbisHeaderCapturePattern}.
   */
  static bool verifyHeaderCapturePattern(const int headerType, AVDataBuffer& header) {
    if (header.remaining() < 7) {
      return false;
    }
    if (header.readUnsignedByte() != static_cast<uint8_t>(headerType)) {
      return false;
    }
    for (int i = 0; i < 6; ++i) {
      if (header.readUnsignedByte() != static_cast<uint8_t>(kVorbisSignature[i])) {
        return false;
      }
    }
    return true;
  }

  /**
   * Reads the identification header of a Vorbis stream. The counterpart of
   * {@code VorbisUtil.readVorbisIdentificationHeader}.
   */
  static bool readIdentificationHeader(AVDataBuffer& header, IdHeader& out) {
    if (!verifyHeaderCapturePattern(kVorbisIdentification, header)) {
      return false;
    }
    out.version = static_cast<int>(header.readLittleEndianUnsignedInt());
    out.channels = header.readUnsignedByte();
    out.sampleRate = static_cast<int>(header.readLittleEndianUnsignedInt());
    out.bitrateMaximum = readBitrate(header);
    out.bitrateNominal = readBitrate(header);
    out.bitrateMinimum = readBitrate(header);
    const int blockSizes = header.readUnsignedByte();
    out.blockSize0 = 1 << (blockSizes & 0x0F);
    out.blockSize1 = 1 << ((blockSizes & 0xF0) >> 4);
    out.framingFlag = (header.readUnsignedByte() & 0x01) > 0;
    out.data.assign(header.array(), header.array() + header.limit());

    if (out.version != 0 || !out.framingFlag || out.channels <= 0 || out.sampleRate <= 0) {
      return false;
    }
    // The specification requires 64 <= blockSize0 <= blockSize1 <= 8192, with a ratio of at most 8.
    return out.blockSize0 >= 64 && out.blockSize1 >= out.blockSize0 && out.blockSize1 <= 8192 &&
           out.blockSize1 <= out.blockSize0 * 8;
  }

  /**
   * Reads a Vorbis comment header and appends its vendor and its comments to @p metadata.
   *
   * <p>The counterpart of {@code VorbisUtil.readVorbisCommentHeader}, with the metadata entry that
   * it used to drop: the comment block of an Ogg stream is the same block as the
   * {@code VORBIS_COMMENT} one of FLAC, and the module keeps both.
   */
  static bool readCommentHeader(AVDataBuffer& header, const bool hasMetadataHeader,
                                const bool hasFramingBit,
                                const std::shared_ptr<Metadata>& metadata) {
    if (hasMetadataHeader && !verifyHeaderCapturePattern(kVorbisComment, header)) {
      return false;
    }
    const auto target = metadata != nullptr
                          ? metadata
                          : std::make_shared<Metadata>(std::vector<std::shared_ptr<Metadata::Entry>>{});
    const size_t consumed =
      AVVorbisCommentParser::parse(header.array() + header.position(), header.remaining(), target);
    if (consumed == 0) {
      return false;
    }
    header.skipBytes(consumed);
    if (hasFramingBit && (header.readUnsignedByte() & 0x01) == 0) {
      return false;
    }
    return true;
  }

  /**
   * Reads the modes of a Vorbis setup header, which are what tells the block size of every audio
   * packet. The counterpart of {@code VorbisUtil.readVorbisModes}.
   *
   * <p>The setup header is a bit stream that carries the codebooks, the floors, the residues and the
   * mappings, and the modes are stored at its very end: they can only be reached by walking the
   * whole header, which is what this method does, without decoding any of the components.
   */
  static bool readModes(AVDataBuffer& header, const int channels, std::vector<Mode>& out) {
    if (!verifyHeaderCapturePattern(kVorbisSetup, header)) {
      return false;
    }
    const int numberOfBooks = header.readUnsignedByte() + 1;
    AVVorbisBitArray bits(header.array(), header.limit());
    bits.skipBits(static_cast<int>(header.position() * 8));
    for (int i = 0; i < numberOfBooks; ++i) {
      if (!skipBook(bits)) {
        return false;
      }
    }
    const int timeCount = static_cast<int>(bits.readBits(6)) + 1;
    for (int i = 0; i < timeCount; ++i) {
      if (bits.readBits(16) != 0x00) {
        return false;
      }
    }
    if (!readFloors(bits) || !readResidues(bits) || !readMappings(channels, bits)) {
      return false;
    }
    if (!readModesFromBits(bits, out)) {
      return false;
    }
    // The setup header ends with a framing bit.
    return bits.readBits(1) != 0;
  }

  /**
   * Returns the block size of an audio packet, i.e. the block size of the mode that its first byte
   * selects. The counterpart of {@code VorbisUtil.getPacketBlockSize}.
   */
  static int packetBlockSize(const uint8_t firstByte, const IdHeader& idHeader,
                             const std::vector<Mode>& modes) {
    if (modes.empty()) {
      return idHeader.blockSize0;
    }
    int modeNumber = 0;
    const int iLogModes = iLog(static_cast<int>(modes.size()) - 1);
    if (iLogModes > 0) {
      // The mode number is stored in the first bits of the byte, after the packet type bit.
      modeNumber = (firstByte >> 1) & (0xFF >> (8 - iLogModes));
    }
    if (modeNumber >= static_cast<int>(modes.size())) {
      modeNumber = 0;
    }
    return modes[static_cast<size_t>(modeNumber)].blockFlag ? idHeader.blockSize1
                                                            : idHeader.blockSize0;
  }

  /**
   * Returns the number of samples that an audio packet contributes to the stream.
   *
   * <p>The packets of a Vorbis stream overlap, therefore a packet adds the samples that its block
   * shares with the previous block. The counterpart of {@code VorbisUtil.getSampleCountInPacket}.
   */
  static int sampleCountInPacket(const int packetBlockSize, const int previousPacketBlockSize,
                                 const bool seenFirstAudioPacket) {
    return seenFirstAudioPacket ? (packetBlockSize + previousPacketBlockSize) / 4 : 0;
  }

private:
  AVVorbisUtil() = default;

  ~AVVorbisUtil() = default;

  /** Reads a bitrate field, which is {@link MediaFormat::NO_VALUE} when it is not positive. */
  static int readBitrate(AVDataBuffer& header) {
    const int bitrate = header.readLittleEndianInt();
    return bitrate > 0 ? bitrate : MediaFormat::NO_VALUE;
  }

  /** Walks one codebook of a setup header. The counterpart of {@code VorbisUtil.skipBook}. */
  static bool skipBook(AVVorbisBitArray& bits) {
    if (bits.readBits(24) != 0x564342) {
      return false;
    }
    const int dimensions = static_cast<int>(bits.readBits(16));
    const int entries = static_cast<int>(bits.readBits(24));
    const bool isOrdered = bits.readBits(1) != 0;
    if (!isOrdered) {
      const bool isSparse = bits.readBits(1) != 0;
      for (int i = 0; i < entries; ++i) {
        if (isSparse) {
          if (bits.readBits(1) != 0) {
            bits.skipBits(5);
          }
        } else {
          bits.skipBits(5);
        }
      }
    } else {
      bits.skipBits(5);
      // The lengths of an ordered codebook are a run length encoding, and a run that does not
      // advance would never end.
      int i = 0;
      for (int guard = 0; i < entries; ++guard) {
        if (guard > entries) {
          return false;
        }
        const int count = static_cast<int>(bits.readBits(iLog(entries - i)));
        if (count <= 0) {
          return false;
        }
        i += count;
      }
    }

    const int lookupType = static_cast<int>(bits.readBits(4));
    if (lookupType > 2) {
      return false;
    }
    if (lookupType == 1 || lookupType == 2) {
      bits.skipBits(32);
      bits.skipBits(32);
      const int valueBits = static_cast<int>(bits.readBits(4)) + 1;
      bits.skipBits(1);
      int64_t lookupValuesCount = 0;
      if (lookupType == 1) {
        lookupValuesCount = dimensions != 0 ? mapType1QuantValues(entries, dimensions) : 0;
      } else {
        lookupValuesCount = static_cast<int64_t>(entries) * dimensions;
      }
      if (lookupValuesCount > kMaxLookupValues || lookupValuesCount * valueBits > kMaxLookupBits) {
        // A codebook that describes more values than any setup header does, it cannot be skipped
        // without reading it, and its lookup table is never used by the extractor.
        return false;
      }
      bits.skipBits(static_cast<int>(lookupValuesCount * valueBits));
    }
    return true;
  }

  /**
   * Returns the number of quantization values of a type 1 codebook lookup, as
   * {@code _book_maptype1_quantvals} of the reference implementation computes it.
   */
  static int64_t mapType1QuantValues(const int entries, const int dimension) {
    return static_cast<int64_t>(
      std::floor(std::pow(static_cast<double>(entries), 1.0 / static_cast<double>(dimension))));
  }

  /** Walks the floors of a setup header. The counterpart of {@code VorbisUtil.readFloors}. */
  static bool readFloors(AVVorbisBitArray& bits) {
    const int floorCount = static_cast<int>(bits.readBits(6)) + 1;
    for (int i = 0; i < floorCount; ++i) {
      const int floorType = static_cast<int>(bits.readBits(16));
      if (floorType == 0) {
        bits.skipBits(8);
        bits.skipBits(16);
        bits.skipBits(16);
        bits.skipBits(6);
        bits.skipBits(8);
        const int books = static_cast<int>(bits.readBits(4)) + 1;
        for (int j = 0; j < books; ++j) {
          bits.skipBits(8);
        }
      } else if (floorType == 1) {
        const int partitions = static_cast<int>(bits.readBits(5));
        int maximumClass = -1;
        std::vector<int> partitionClasses(static_cast<size_t>(partitions));
        for (int j = 0; j < partitions; ++j) {
          partitionClasses[static_cast<size_t>(j)] = static_cast<int>(bits.readBits(4));
          maximumClass = std::max(maximumClass, partitionClasses[static_cast<size_t>(j)]);
        }
        std::vector<int> classDimensions(static_cast<size_t>(maximumClass) + 1);
        for (size_t j = 0; j < classDimensions.size(); ++j) {
          classDimensions[j] = static_cast<int>(bits.readBits(3)) + 1;
          const int subclasses = static_cast<int>(bits.readBits(2));
          if (subclasses > 0) {
            bits.skipBits(8);
          }
          for (int k = 0; k < (1 << subclasses); ++k) {
            bits.skipBits(8);
          }
        }
        bits.skipBits(2);
        const int rangeBits = static_cast<int>(bits.readBits(4));
        // Every partition spends rangeBits for each value of the class dimension it selects.
        int count = 0;
        int value = 0;
        for (int j = 0; j < partitions; ++j) {
          count += classDimensions[static_cast<size_t>(partitionClasses[static_cast<size_t>(j)])];
          for (; value < count; ++value) {
            bits.skipBits(rangeBits);
          }
        }
      } else {
        return false;
      }
    }
    return true;
  }

  /** Walks the residues of a setup header. The counterpart of {@code VorbisUtil.readResidues}. */
  static bool readResidues(AVVorbisBitArray& bits) {
    const int residueCount = static_cast<int>(bits.readBits(6)) + 1;
    for (int i = 0; i < residueCount; ++i) {
      const int residueType = static_cast<int>(bits.readBits(16));
      if (residueType > 2) {
        return false;
      }
      bits.skipBits(24);
      bits.skipBits(24);
      bits.skipBits(24);
      const int classifications = static_cast<int>(bits.readBits(6)) + 1;
      bits.skipBits(8);
      std::vector<int> cascade(static_cast<size_t>(classifications));
      for (int j = 0; j < classifications; ++j) {
        const int lowBits = static_cast<int>(bits.readBits(3));
        int highBits = 0;
        if (bits.readBits(1) != 0) {
          highBits = static_cast<int>(bits.readBits(5));
        }
        cascade[static_cast<size_t>(j)] = highBits * 8 + lowBits;
      }
      for (int j = 0; j < classifications; ++j) {
        for (int k = 0; k < 8; ++k) {
          if ((cascade[static_cast<size_t>(j)] & (0x01 << k)) != 0) {
            bits.skipBits(8);
          }
        }
      }
    }
    return true;
  }

  /** Walks the mappings of a setup header. The counterpart of {@code VorbisUtil.readMappings}. */
  static bool readMappings(const int channels, AVVorbisBitArray& bits) {
    const int mappingCount = static_cast<int>(bits.readBits(6)) + 1;
    for (int i = 0; i < mappingCount; ++i) {
      const int mappingType = static_cast<int>(bits.readBits(16));
      if (mappingType != 0) {
        // A mapping type other than 0 is not decodable, the Java implementation skips it as well.
        continue;
      }
      int submaps = 1;
      if (bits.readBits(1) != 0) {
        submaps = static_cast<int>(bits.readBits(4)) + 1;
      }
      if (bits.readBits(1) != 0) {
        const int couplingSteps = static_cast<int>(bits.readBits(8)) + 1;
        for (int j = 0; j < couplingSteps; ++j) {
          bits.skipBits(iLog(channels - 1));
          bits.skipBits(iLog(channels - 1));
        }
      }
      if (bits.readBits(2) != 0x00) {
        return false;
      }
      if (submaps > 1) {
        for (int j = 0; j < channels; ++j) {
          bits.skipBits(4);
        }
      }
      for (int j = 0; j < submaps; ++j) {
        bits.skipBits(8);
        bits.skipBits(8);
        bits.skipBits(8);
      }
    }
    return true;
  }

  /** Reads the modes that end a setup header. The counterpart of {@code VorbisUtil.readModes}. */
  static bool readModesFromBits(AVVorbisBitArray& bits, std::vector<Mode>& out) {
    const int modeCount = static_cast<int>(bits.readBits(6)) + 1;
    out.resize(static_cast<size_t>(modeCount));
    for (int i = 0; i < modeCount; ++i) {
      Mode& mode = out[static_cast<size_t>(i)];
      mode.blockFlag = bits.readBits(1) != 0;
      mode.windowType = static_cast<int>(bits.readBits(16));
      mode.transformType = static_cast<int>(bits.readBits(16));
      mode.mapping = static_cast<int>(bits.readBits(8));
    }
    return true;
  }
};

} // namespace anonymous

/**
 * The reader of one Ogg bitstream, i.e. of one codec carried by an Ogg container.
 *
 * <p>The reader parses the headers of the bitstream, publishes the format of its track, and then
 * delivers the packets that follow as samples, one sample per packet. It owns its packet reader and
 * its seek map, therefore it needs no destructor work beyond the release of those members.
 *
 * <p>This is the extractor module counterpart of ExoPlayer's {@code StreamReader}. Unlike the Java
 * implementation, the reader always parses the headers again when the container is restarted: the
 * Java implementation can skip to the payload of a stream whose headers it already knows, which
 * discards the packets of the page that holds the end of the headers, and this module delivers every
 * packet of the container.
 */
class AVOggStreamReader {
public:
  virtual ~AVOggStreamReader() = default;

  AVOggStreamReader(const AVOggStreamReader& other) = delete;

  AVOggStreamReader& operator=(const AVOggStreamReader& other) = delete;

  /**
   * Initializes the reader, which is called exactly once, before the first {@link #read}.
   *
   * @param output The output that receives the seek map.
   * @param trackOutput The output of the track that receives the samples.
   */
  void init(AVExtractorOutput* output, std::shared_ptr<AVTrackOutput> trackOutput) {
    output_ = output;
    trackOutput_ = std::move(trackOutput);
    reset();
  }

  /**
   * Reads the next packet of the bitstream and delivers it when the target of a pending seek was
   * reached. The counterpart of {@code StreamReader.read}.
   *
   * <p>The call that parses the headers only publishes the format of the track and does not deliver
   * a sample: that call is the one that creates the track, therefore the consumer of the track is
   * resolved after it returns, see {@link AVOggExtractor::read}. A seek, which restarts the container
   * and resolves its target in a single call, is the exception, see {@link #readPayload}.
   */
  AVExtractor::Result read(AVExtractorInput& input) {
    if (state_ == State::kReadHeaders) {
      const AVExtractor::Result result = readHeadersAndUpdateState(input);
      if (result != AVExtractor::kResultContinue || targetGranule_ == kNoTargetGranule) {
        return result;
      }
    }
    if (state_ == State::kEndOfInput) {
      return AVExtractor::kResultEndOfInput;
    }
    return readPayload(input);
  }

  /**
   * Moves the reader to the position of a seek. The counterpart of {@code StreamReader.seek}.
   *
   * @param position The position of the input that the reader restarts from.
   * @param timeUs The time that the delivered samples must reach, or -1 when it is unknown.
   *
   * @return {@code false} when the requested position cannot be reached, i.e. when the headers of the
   *         bitstream were not parsed yet.
   */
  bool seek(const long position, const int64_t timeUs) {
    packet_.reset();
    lengthOfReadPacket_ = 0;
    if (position <= 0) {
      // The container is restarted from its beginning, which is the only position whose parse does
      // not depend on the state of the extractor: the headers are read again, so that the codec
      // configuration and the first packets of the stream are delivered as well.
      reset();
      packet_.setPreviousPageGranule(0);
      targetGranule_ = timeUs >= 0 ? convertTimeToGranule(timeUs) : kNoTargetGranule;
      return true;
    }
    if (state_ == State::kReadHeaders) {
      // The seek map is only published once the headers were parsed, so a reader that resolved a
      // position asked for a position of a stream this extractor does not know yet.
      return false;
    }
    currentGranule_ = seekMap_ != nullptr ? seekMap_->granuleForPosition(position) : 0;
    packet_.setPreviousPageGranule(currentGranule_);
    onSeekEnd(currentGranule_);
    targetGranule_ = timeUs >= 0 ? convertTimeToGranule(timeUs) : kNoTargetGranule;
    state_ = State::kReadPayload;
    return true;
  }

protected:
  /** The data that the headers of a bitstream describe. */
  struct SetupData {
    /** The format of the samples of the bitstream, or null while the headers are incomplete. */
    std::shared_ptr<MediaFormat> format;
  };

  /** The state of the reader, as the states of {@code StreamReader} document them. */
  enum class State {
    /** The headers of the bitstream are read. */
    kReadHeaders,
    /** The payload packets are delivered. */
    kReadPayload,
    /** The end of the input was reached. */
    kEndOfInput,
  };

  AVOggStreamReader() = default;

  /**
   * Reads the headers of one packet.
   *
   * @param packet The payload of the packet, positioned at its first byte.
   * @param position The position of the packet in the input.
   * @param setupData The data that the headers fill in.
   *
   * @return {@code true} when the packet was a header, i.e. when the headers are not complete yet.
   */
  virtual bool readHeaders(AVDataBuffer& packet, long position, SetupData& setupData) = 0;

  /**
   * Prepares the payload of one packet for the decoder.
   *
   * @param packet The payload of the packet, which the implementation may extend.
   *
   * @return The number of granules that the packet adds to the stream, or -1 when the packet carries
   *         no audio.
   */
  virtual int64_t preparePayload(AVDataBuffer& packet) = 0;

  /**
   * Resets the state of the reader, so that the headers are parsed again. The counterpart of
   * {@code StreamReader.reset}, which also accepts to skip the headers.
   */
  virtual void reset() {
    setupData_ = SetupData();
    payloadStartPosition_ = 0;
    state_ = State::kReadHeaders;
    targetGranule_ = kNoTargetGranule;
    currentGranule_ = 0;
  }

  /**
   * Called when a seek ends, with the granule at which the packet that follows starts. The
   * counterpart of {@code StreamReader.onSeekEnd}.
   */
  virtual void onSeekEnd(const int64_t currentGranule) { currentGranule_ = currentGranule; }

  /** Converts a granule position to a presentation timestamp. */
  [[nodiscard]] int64_t convertGranuleToTime(const int64_t granule) const {
    if (sampleRate_ <= 0) {
      return Definitions::kTimeUnset;
    }
    return granule * AVUtil::kMicrosPerSecond / sampleRate_;
  }

  /** Converts a presentation timestamp to a granule position. */
  [[nodiscard]] int64_t convertTimeToGranule(const int64_t timeUs) const {
    if (sampleRate_ <= 0) {
      return 0;
    }
    return sampleRate_ * timeUs / AVUtil::kMicrosPerSecond;
  }

  /** Returns whether one of the headers of the bitstream could not be parsed. */
  [[nodiscard]] bool hasError() const { return error_; }

  /** Reports that one of the headers of the bitstream could not be parsed. */
  void reportError() { error_ = true; }

private:
  /**
   * Reads the headers of the bitstream until the first packet that is not a header is reached, which
   * is left in the packet reader. The counterpart of {@code StreamReader.readHeaders}.
   *
   * @return {@code false} when the end of the input was reached or a header is malformed.
   */
  bool readHeaderPackets(AVExtractorInput& input) {
    while (true) {
      if (!packet_.populate(input)) {
        state_ = State::kEndOfInput;
        return false;
      }
      lengthOfReadPacket_ = input.getPosition() - payloadStartPosition_;
      if (readHeaders(packet_.getPayload(), payloadStartPosition_, setupData_)) {
        payloadStartPosition_ = input.getPosition();
      } else {
        // The current packet is not a header, therefore all the headers were read.
        return !error_;
      }
    }
  }

  /**
   * Reads the headers, publishes the format and the seek map of the track and moves to the payload.
   */
  AVExtractor::Result readHeadersAndUpdateState(AVExtractorInput& input) {
    if (!readHeaderPackets(input) || setupData_.format == nullptr) {
      error_ = true;
      return finish();
    }
    sampleRate_ = setupData_.format->getSampleRate();
    if (sampleRate_ <= 0) {
      error_ = true;
      return finish();
    }
    if (!formatSet_ && trackOutput_ != nullptr) {
      trackOutput_->format(setupData_.format);
      formatSet_ = true;
    }
    if (seekMap_ == nullptr && output_ != nullptr) {
      seekMap_ = std::make_shared<AVOggSeekMap>(sampleRate_, payloadStartPosition_);
      packet_.setPageIndexer(seekMap_.get());
      output_->seekMap(seekMap_);
    }
    state_ = State::kReadPayload;
    return AVExtractor::kResultContinue;
  }

  /**
   * Delivers the packet that the packet reader holds, or the next one, and updates the granule of
   * the stream. The counterpart of {@code StreamReader.readPayload}.
   *
   * <p>The call always consumes input, because the packet that ended the headers was read while the
   * headers were parsed: the sample of that packet is delivered together with the reading of the
   * packet that follows it. The reader of this module reports that the extractor stopped making
   * progress when a call returns without consuming anything, which is what the Java implementation
   * avoids because its reading loop accepts a call that only delivered the packet it had buffered.
   *
   * <p>A seek that is being resolved is driven to its target within the same call, as long as the
   * distance to it stays below {@link kMaxSeekScanSize}: the reader reports the end of the input as
   * soon as the position it asked for was reached, therefore a caller that stops there would
   * otherwise observe no sample at all.
   */
  AVExtractor::Result readPayload(AVExtractorInput& input) {
    const long callStartPosition = input.getPosition();
    long scanned = 0;
    while (true) {
      const long positionBefore = input.getPosition();
      // The packet that ended the headers was read already and is the first payload packet.
      const bool hasPacket = lengthOfReadPacket_ > 0 || packet_.populate(input);
      if (!hasPacket) {
        return finish();
      }
      lengthOfReadPacket_ = 0;
      AVDataBuffer& payload = packet_.getPayload();
      const int64_t granulesInPacket = preparePayload(payload);
      bool delivered = false;
      if (granulesInPacket >= 0 && currentGranule_ + granulesInPacket >= targetGranule_) {
        // The target of a pending seek was reached, or no seek is pending.
        deliverSample(payload, convertGranuleToTime(currentGranule_));
        targetGranule_ = kNoTargetGranule;
        delivered = true;
      }
      // A packet that carries no audio does not advance the granule of the stream.
      currentGranule_ += std::max<int64_t>(granulesInPacket, 0);

      scanned += input.getPosition() - positionBefore;
      const bool progressed = input.getPosition() > callStartPosition;
      if (progressed && (delivered || targetGranule_ == kNoTargetGranule)) {
        return AVExtractor::kResultContinue;
      }
      if (scanned >= kMaxSeekScanSize) {
        // A seek that is still far from its target is spread over several calls.
        return AVExtractor::kResultContinue;
      }
    }
  }

  /** Builds the sample of one packet and delivers it to the track output. */
  void deliverSample(AVDataBuffer& payload, const int64_t timeUs) {
    if (trackOutput_ == nullptr || payload.limit() == 0) {
      return;
    }
    auto buffer = std::make_shared<AVBlockBuffer>(static_cast<uint32_t>(payload.limit()));
    buffer->put(payload.array(), payload.limit());
    buffer->position(0);
    buffer->limit(payload.limit());
    uint32_t flags = AVSample::kFlagKeyFrame;
    if (!firstSampleDelivered_) {
      flags |= AVSample::kFlagFirstSample;
      firstSampleDelivered_ = true;
    }
    AVSample sample(trackOutput_->id(), timeUs, flags, std::move(buffer));
    trackOutput_->sample(sample);
  }

  /**
   * Reports the end of the stream: the duration of the container is published, the track is ended,
   * and a further read reports the end of the input.
   */
  AVExtractor::Result finish() {
    state_ = State::kEndOfInput;
    if (seekMap_ != nullptr) {
      seekMap_->onEndOfInput();
    }
    if (trackOutput_ != nullptr && !trackEnded_) {
      trackOutput_->endTrack();
      trackEnded_ = true;
    }
    return AVExtractor::kResultEndOfInput;
  }

  AVOggPacket packet_;
  /** The output that receives the seek map, owned by the reader. */
  AVExtractorOutput* output_ = nullptr;
  /** The output of the track that receives the samples. */
  std::shared_ptr<AVTrackOutput> trackOutput_;
  /** The seek map of the container, which is owned by the reader once it was published. */
  std::shared_ptr<AVOggSeekMap> seekMap_;
  /** The format and the seeker that the headers of the bitstream described. */
  SetupData setupData_;
  State state_ = State::kReadHeaders;
  /** The granule that the next delivered packet must reach, or {@link kNoTargetGranule}. */
  int64_t targetGranule_ = kNoTargetGranule;
  /** The granule position of the packet that follows the last delivered one. */
  int64_t currentGranule_ = 0;
  /** The position of the first payload packet of the bitstream. */
  long payloadStartPosition_ = 0;
  /** The number of bytes of the header packet that ended the headers. */
  long lengthOfReadPacket_ = 0;
  /** The sample rate of the bitstream, which converts granules to timestamps. */
  int sampleRate_ = 0;
  /** Whether the format of the track was published. */
  bool formatSet_ = false;
  /** Whether the track was ended. */
  bool trackEnded_ = false;
  /** Whether the first sample was delivered. */
  bool firstSampleDelivered_ = false;
  /** Whether a header of the bitstream could not be parsed. */
  bool error_ = false;
};

namespace {

/**
 * The reader of a Vorbis bitstream.
 *
 * <p>This is the extractor module counterpart of ExoPlayer's {@code VorbisReader}. The three
 * headers of the codec are read in order: the identification header declares the sample rate, the
 * channel count and the two block sizes; the comment header is validated and dropped; and the setup
 * header, whose modes tell the block size of every audio packet, is forwarded to the decoder
 * together with the identification header.
 */
class AVVorbisReader final : public AVOggStreamReader {
public:
  AVVorbisReader() = default;

  ~AVVorbisReader() override = default;

  AVVorbisReader(const AVVorbisReader& other) = delete;

  AVVorbisReader& operator=(const AVVorbisReader& other) = delete;

  /**
   * Returns whether the given packet starts the bitstream of a Vorbis stream, i.e. whether it is an
   * identification header. The counterpart of {@code VorbisReader.verifyBitstreamType}.
   */
  static bool verifyBitstreamType(AVDataBuffer& data) {
    return AVVorbisUtil::verifyHeaderCapturePattern(kVorbisIdentification, data);
  }

protected:
  bool readHeaders(AVDataBuffer& packet, const long position, SetupData& setupData) override {
    static_cast<void>(position);
    if (hasSetup_) {
      // Every header was read already, this packet is audio.
      return false;
    }
    if (!readSetupHeaders(packet)) {
      // Either the headers are incomplete, or one of them is malformed: the loop stops in both
      // cases, and the caller of the reader ends the stream when a header is malformed.
      return !hasError();
    }
    std::vector<std::vector<uint8_t>> codecSpecificData;
    codecSpecificData.push_back(idHeader_.data);
    codecSpecificData.push_back(setupHeaderData_);
    auto format = MediaFormat::CreateAudioFormat(MimeTypes::AUDIO_VORBIS, idHeader_.sampleRate,
                                                 idHeader_.channels);
    format->setContainerMimeType(MimeTypes::AUDIO_OGG);
    format->setAverageBitrate(idHeader_.bitrateNominal);
    format->setPeakBitrate(idHeader_.bitrateMaximum);
    format->setCodecSpecificData(codecSpecificData);
    if (commentMetadata_ != nullptr) {
      format->setMetadata(commentMetadata_);
    }
    setupData.format = format;
    return true;
  }

  int64_t preparePayload(AVDataBuffer& packet) override {
    if (packet.limit() == 0) {
      return -1;
    }
    const uint8_t* const data = packet.array();
    if ((data[0] & 0x01) == 0x01) {
      // A Vorbis audio packet has a packet type bit of zero.
      return -1;
    }
    const int blockSize = AVVorbisUtil::packetBlockSize(data[0], idHeader_, modes_);
    const int sampleCount =
      AVVorbisUtil::sampleCountInPacket(blockSize, previousPacketBlockSize_, seenFirstAudioPacket_);
    // The decoder expects the number of samples of the packet to be appended to its payload.
    appendSampleCount(packet, sampleCount);
    seenFirstAudioPacket_ = true;
    previousPacketBlockSize_ = blockSize;
    return sampleCount;
  }

  void reset() override {
    AVOggStreamReader::reset();
    hasIdHeader_ = false;
    hasCommentHeader_ = false;
    hasSetup_ = false;
    idHeader_ = AVVorbisUtil::IdHeader();
    commentMetadata_.reset();
    modes_.clear();
    setupHeaderData_.clear();
    previousPacketBlockSize_ = 0;
    seenFirstAudioPacket_ = false;
  }

  void onSeekEnd(const int64_t currentGranule) override {
    AVOggStreamReader::onSeekEnd(currentGranule);
    // The granule of a packet that a seek resumed at is not zero, therefore the packet is not the
    // first one of the stream and its block overlaps the block of the previous packet.
    seenFirstAudioPacket_ = currentGranule != 0;
    previousPacketBlockSize_ = hasIdHeader_ ? idHeader_.blockSize0 : 0;
  }

private:
  /**
   * Reads one of the three headers of the bitstream.
   *
   * @return {@code true} when the setup header was read, i.e. when the format of the track is known.
   */
  bool readSetupHeaders(AVDataBuffer& packet) {
    if (!hasIdHeader_) {
      if (!AVVorbisUtil::readIdentificationHeader(packet, idHeader_)) {
        reportError();
        return false;
      }
      hasIdHeader_ = true;
      return false;
    }
    if (!hasCommentHeader_) {
      commentMetadata_ =
        std::make_shared<Metadata>(std::vector<std::shared_ptr<Metadata::Entry>>{});
      if (!AVVorbisUtil::readCommentHeader(packet, true, true, commentMetadata_)) {
        reportError();
        return false;
      }
      hasCommentHeader_ = true;
      return false;
    }
    // The third packet is the setup header, whose raw bytes are the second configuration buffer of
    // the decoder, and whose modes tell the block size of every audio packet.
    setupHeaderData_.assign(packet.array(), packet.array() + packet.limit());
    if (!AVVorbisUtil::readModes(packet, idHeader_.channels, modes_) || modes_.empty()) {
      reportError();
      return false;
    }
    hasSetup_ = true;
    return true;
  }

  /** Appends the number of samples of a packet to its payload, as a 32 bit little endian value. */
  static void appendSampleCount(AVDataBuffer& packet, const int sampleCount) {
    const size_t limit = packet.limit();
    packet.setLimit(limit + 4);
    writeLittleEndianUnsignedInt(packet.array(), limit, static_cast<uint32_t>(sampleCount));
  }

  AVVorbisUtil::IdHeader idHeader_;
  std::shared_ptr<Metadata> commentMetadata_;
  std::vector<AVVorbisUtil::Mode> modes_;
  std::vector<uint8_t> setupHeaderData_;
  bool hasIdHeader_ = false;
  bool hasCommentHeader_ = false;
  bool hasSetup_ = false;
  int previousPacketBlockSize_ = 0;
  bool seenFirstAudioPacket_ = false;
};

/**
 * The reader of an Opus bitstream.
 *
 * <p>This is the extractor module counterpart of ExoPlayer's {@code OpusReader}, together with the
 * {@code OpusUtil} parser of the container module. An Opus stream always runs at 48000 Hz, and the
 * duration of a packet is declared by its table of contents byte, therefore the granule of a packet
 * is its duration in samples.
 */
class AVOpusReader final : public AVOggStreamReader {
public:
  AVOpusReader() = default;

  ~AVOpusReader() override = default;

  AVOpusReader(const AVOpusReader& other) = delete;

  AVOpusReader& operator=(const AVOpusReader& other) = delete;

  /**
   * Returns whether the given packet starts the bitstream of an Opus stream. The counterpart of
   * {@code OpusReader.verifyBitstreamType}.
   */
  static bool verifyBitstreamType(AVDataBuffer& data) {
    return startsWith(data, kOpusIdHeader, sizeof(kOpusIdHeader) - 1);
  }

protected:
  bool readHeaders(AVDataBuffer& packet, const long position, SetupData& setupData) override {
    static_cast<void>(position);
    if (startsWith(packet, kOpusIdHeader, sizeof(kOpusIdHeader) - 1)) {
      if (setupData.format != nullptr) {
        // The specification allows a single identification header, but files that repeat it were
        // observed: the repetitions are ignored.
        return true;
      }
      const uint8_t* const data = packet.array();
      const std::vector<uint8_t> header(data, data + packet.limit());
      setupData.format = buildFormat(header);
      return true;
    }
    if (startsWith(packet, kOpusCommentHeader, sizeof(kOpusCommentHeader) - 1)) {
      if (firstCommentHeaderSeen_) {
        // A repeated comment header is ignored, as the Java implementation does.
        return true;
      }
      firstCommentHeaderSeen_ = true;
      packet.skipBytes(sizeof(kOpusCommentHeader) - 1);
      // The comment header of an Opus stream is a Vorbis comment header without its own prefix and
      // without a framing bit.
      auto metadata = std::make_shared<Metadata>(std::vector<std::shared_ptr<Metadata::Entry>>{});
      if (!AVVorbisUtil::readCommentHeader(packet, false, false, metadata)) {
        reportError();
      } else if (setupData.format != nullptr) {
        setupData.format->setMetadata(metadata);
      }
      return true;
    }
    // The identification header starts the stream, therefore the headers are complete.
    return false;
  }

  int64_t preparePayload(AVDataBuffer& packet) override {
    if (packet.limit() == 0) {
      return -1;
    }
    return convertTimeToGranule(packetDurationUs(packet.array(), static_cast<int>(packet.limit())));
  }

  void reset() override {
    AVOggStreamReader::reset();
    firstCommentHeaderSeen_ = false;
  }

private:
  /** Returns whether the packet starts with the given signature. */
  static bool startsWith(const AVDataBuffer& packet, const char* const signature,
                         const size_t length) {
    if (packet.limit() < length || packet.array() == nullptr) {
      return false;
    }
    for (size_t i = 0; i < length; ++i) {
      if (packet.array()[i] != static_cast<uint8_t>(signature[i])) {
        return false;
      }
    }
    return true;
  }

  /**
   * Builds the format of the stream from its identification header, which declares the channel
   * count. The counterpart of {@code OpusUtil.buildInitializationData}: the decoder is given the
   * identification header, the number of samples to skip at the beginning of the stream, and the
   * number of samples to decode before a seek target.
   */
  static std::shared_ptr<MediaFormat> buildFormat(const std::vector<uint8_t>& header) {
    const int channelCount = header.size() > 9 ? header[9] : 0;
    if (channelCount <= 0 || header.size() < 12) {
      return nullptr;
    }
    const int preSkipSamples = (static_cast<int>(header[11]) << 8) | static_cast<int>(header[10]);
    std::vector<std::vector<uint8_t>> codecSpecificData;
    codecSpecificData.push_back(header);
    codecSpecificData.push_back(littleEndianLong(sampleCountToNanoseconds(preSkipSamples)));
    codecSpecificData.push_back(littleEndianLong(sampleCountToNanoseconds(kOpusSeekPreRollSamples)));

    auto format =
      MediaFormat::CreateAudioFormat(MimeTypes::AUDIO_OPUS, kOpusSampleRate, channelCount);
    format->setContainerMimeType(MimeTypes::AUDIO_OGG);
    format->setCodecSpecificData(codecSpecificData);
    return format;
  }

  /** Returns the duration of one packet, in microseconds, as RFC 6716 defines it. */
  static int64_t packetDurationUs(const uint8_t* const data, const int size) {
    const int toc = data[0] & 0xFF;
    int frames = 0;
    switch (toc & 0x3) {
      case 0: {
        frames = 1;
        break;
      }
      case 1:
      case 2: {
        frames = 2;
        break;
      }
      default: {
        frames = size > 1 ? (data[1] & 0x3F) : 0;
        break;
      }
    }
    const int config = toc >> 3;
    const int length = config & 0x3;
    int frameDurationUs = 0;
    if (config >= 16) {
      frameDurationUs = 2500 << length;
    } else if (config >= 12) {
      frameDurationUs = 10000 << (length & 0x1);
    } else if (length == 3) {
      frameDurationUs = 60000;
    } else {
      frameDurationUs = 10000 << length;
    }
    return static_cast<int64_t>(frames) * frameDurationUs;
  }

  /** Converts a sample count at 48000 Hz to nanoseconds. */
  static int64_t sampleCountToNanoseconds(const int64_t sampleCount) {
    return sampleCount * 1000000000 / kOpusSampleRate;
  }

  /** Returns the eight bytes of a 64 bit value in little endian order. */
  static std::vector<uint8_t> littleEndianLong(const int64_t value) {
    std::vector<uint8_t> out(8);
    const auto bits = static_cast<uint64_t>(value);
    for (int i = 0; i < 8; ++i) {
      out[static_cast<size_t>(i)] = static_cast<uint8_t>((bits >> (i * 8)) & 0xFF);
    }
    return out;
  }

  bool firstCommentHeaderSeen_ = false;
};

/**
 * The reader of a FLAC bitstream that is carried by an Ogg container.
 *
 * <p>This is the extractor module counterpart of ExoPlayer's {@code FlacReader}. The first packet of
 * the bitstream is the Ogg mapping of the codec: it declares the version of the mapping and carries
 * the {@code fLaC} marker followed by the STREAMINFO block, which describes the samples of the
 * stream and which the decoder reads as its configuration buffer. The metadata blocks that follow
 * are skipped, and the audio frames are delivered as they are.
 *
 * <p>Unlike the Java implementation, which builds its seek map from the seek table of the stream,
 * the seeking of this extractor is served by the page index of the container, see
 * {@link AVOggSeekMap}: a FLAC stream that declares no seek table is seekable as well.
 */
class AVFlacReader final : public AVOggStreamReader {
public:
  AVFlacReader() = default;

  ~AVFlacReader() override = default;

  AVFlacReader(const AVFlacReader& other) = delete;

  AVFlacReader& operator=(const AVFlacReader& other) = delete;

  /**
   * Returns whether the given packet starts the Ogg mapping of a FLAC stream. The counterpart of
   * {@code FlacReader.verifyBitstreamType}.
   */
  static bool verifyBitstreamType(AVDataBuffer& data) {
    return data.remaining() >= 5 && data.readUnsignedByte() == kFlacPacketType &&
           data.readUnsignedInt() == kFlacSignature;
  }

protected:
  bool readHeaders(AVDataBuffer& packet, const long position, SetupData& setupData) override {
    static_cast<void>(position);
    if (packet.limit() == 0) {
      reportError();
      return false;
    }
    if (!hasStreamInfo_) {
      if (!readMappingHeader(packet, setupData)) {
        reportError();
        return false;
      }
      return true;
    }
    const uint8_t* const data = packet.array();
    if ((data[0] & 0x7F) == kFlacMetadataTypeSeekTable) {
      // The seek table describes the frame of every seek point of the stream, which the page index
      // of the container replaces: the block is validated and dropped.
      return true;
    }
    if (data[0] == kFlacAudioPacketType) {
      // The first audio frame ends the headers of the stream.
      return false;
    }
    // Any other metadata block is dropped as well.
    return true;
  }

  int64_t preparePayload(AVDataBuffer& packet) override {
    if (packet.limit() == 0 || packet.array()[0] != kFlacAudioPacketType) {
      return -1;
    }
    return frameBlockSize(packet);
  }

  void reset() override {
    AVOggStreamReader::reset();
    streamInfo_ = StreamInfo();
    hasStreamInfo_ = false;
  }

private:
  /** The STREAMINFO block of a FLAC stream, as the Ogg mapping of the codec carries it. */
  struct StreamInfo {
    /** The maximum block size of a frame, in samples. */
    int maxBlockSize = 0;
    /** The maximum size of a frame, in bytes. */
    int maxFrameSize = 0;
    /** The sample rate, in Hz. */
    int sampleRate = 0;
    /** The number of channels. */
    int channelCount = 0;
    /** The number of bits per sample. */
    int bitsPerSample = 0;
  };

  /**
   * Reads the mapping header of the stream, i.e. the {@code fLaC} marker and the STREAMINFO block,
   * and builds the format of the track.
   */
  bool readMappingHeader(AVDataBuffer& packet, SetupData& setupData) {
    const size_t size = packet.limit();
    if (size < kFlacStreamInfoOffset + kFlacStreamInfoSize) {
      return false;
    }
    const uint8_t* const data = packet.array();
    // The mapping carries the version of the mapping, the number of header packets of the stream and
    // the "fLaC" marker that precedes the metadata block.
    if (AVUtil::readUnsignedInt(data, 9) != kFlacStreamMarker) {
      return false;
    }
    if (!parseStreamInfo(data + kFlacStreamInfoOffset, kFlacStreamInfoSize, streamInfo_)) {
      return false;
    }
    hasStreamInfo_ = true;

    // The decoder is given the metadata block exactly as the stream carries it, i.e. from the
    // "fLaC" marker to the end of the STREAMINFO block.
    const uint8_t* const metadata = data + 9;
    const size_t metadataSize = size - 9;
    auto format = MediaFormat::CreateAudioFormat(MimeTypes::AUDIO_FLAC, streamInfo_.sampleRate,
                                                 streamInfo_.channelCount);
    format->setContainerMimeType(MimeTypes::AUDIO_OGG);
    format->setMaxInputSize(streamInfo_.maxFrameSize > 0 ? streamInfo_.maxFrameSize : kMaxFrameSize);
    format->setSampleFormat(MediaUtils::getSampleFormat(streamInfo_.bitsPerSample));
    format->setCodecSpecificData(0, metadata, metadataSize);
    setupData.format = format;
    return true;
  }

  /** Parses the 34 bytes of a STREAMINFO block. */
  static bool parseStreamInfo(const uint8_t data[], const size_t size, StreamInfo& out) {
    if (size < kFlacStreamInfoSize) {
      return false;
    }
    out.maxBlockSize = AVUtil::readUnsignedShort(data, 2);
    out.maxFrameSize = static_cast<int>(AVUtil::readUnsignedInt24(data, 7));
    // The next eight bytes hold the sample rate (20 bits), the number of channels (3 bits), the
    // number of bits per sample (5 bits) and the total number of samples (36 bits).
    const uint64_t packed = (static_cast<uint64_t>(AVUtil::readUnsignedInt(data, 10)) << 32) |
                            AVUtil::readUnsignedInt(data, 14);
    out.sampleRate = static_cast<int>((packed >> 44) & 0xFFFFF);
    out.channelCount = static_cast<int>(((packed >> 41) & 0x7) + 1);
    out.bitsPerSample = static_cast<int>(((packed >> 36) & 0x1F) + 1);
    return out.sampleRate > 0 && out.channelCount > 0;
  }

  /**
   * Returns the number of samples of a FLAC frame, which its header declares either directly or
   * through the bytes that follow it, when the frame uses a variable block size.
   */
  static int frameBlockSize(AVDataBuffer& packet) {
    if (packet.limit() < 3) {
      return 0;
    }
    const int blockSizeKey = (packet.array()[2] & 0xFF) >> 4;
    if (blockSizeKey == 6 || blockSizeKey == 7) {
      // A frame whose block size is declared by the header itself carries the number of its first
      // sample, which is a variable length integer, before that declaration.
      packet.position(0);
      packet.skipBytes(kFlacFrameHeaderSampleNumberOffset);
      if (!skipUtf8EncodedNumber(packet)) {
        return 0;
      }
    }
    int samples = 0;
    switch (blockSizeKey) {
      case 1: {
        samples = 192;
        break;
      }
      case 2:
      case 3:
      case 4:
      case 5: {
        samples = 576 << (blockSizeKey - 2);
        break;
      }
      case 6: {
        samples = packet.readUnsignedByte() + 1;
        break;
      }
      case 7: {
        samples = packet.readUnsignedShort() + 1;
        break;
      }
      case 8:
      case 9:
      case 10:
      case 11:
      case 12:
      case 13:
      case 14:
      case 15: {
        samples = 256 << (blockSizeKey - 8);
        break;
      }
      default: {
        samples = 0;
        break;
      }
    }
    packet.position(0);
    return samples;
  }

  /**
   * Skips the variable length integer that a frame with a variable block size carries, and returns
   * whether it is well formed.
   */
  static bool skipUtf8EncodedNumber(AVDataBuffer& packet) {
    if (packet.remaining() < 1) {
      return false;
    }
    const uint8_t first = packet.array()[packet.position()];
    int extraBytes = 0;
    uint8_t mask = 0x80;
    while (extraBytes < 8 && (first & mask) != 0) {
      ++extraBytes;
      mask = static_cast<uint8_t>(mask >> 1);
    }
    if (extraBytes == 8 || packet.remaining() < static_cast<size_t>(1 + extraBytes)) {
      // A leading byte of eight ones is not a valid encoding.
      return false;
    }
    packet.skipBytes(static_cast<size_t>(1 + extraBytes));
    return true;
  }

  StreamInfo streamInfo_;
  bool hasStreamInfo_ = false;
};

} // namespace anonymous

// ==================================================================================================
// AVOggExtractor
// ==================================================================================================

AVOggExtractor::AVOggExtractor() noexcept = default;

AVOggExtractor::~AVOggExtractor() = default;

int AVOggExtractor::sniff(AVExtractorInput& input) {
  sniffFailureReason_.clear();
  if (sniffInternal(input)) {
    return AVExtractorsFactory::kSniffResultCertain;
  }
  if (sniffFailureReason_.empty()) {
    sniffFailureReason_ = "The input does not start with the page of an Ogg bitstream.";
  }
  return AVExtractorsFactory::kSniffResultNone;
}

bool AVOggExtractor::sniffInternal(AVExtractorInput& input) {
  streamReader_.reset();
  AVOggPageHeader header;
  if (!header.populate(input) ||
      (header.type() & kHeaderTypeBeginOfStream) != kHeaderTypeBeginOfStream) {
    input.resetPeekPosition();
    return false;
  }
  const int length = std::min(header.bodySize(), kMaxVerificationBytes);
  if (length <= 0) {
    input.resetPeekPosition();
    sniffFailureReason_ = "The first page of the bitstream is empty.";
    return false;
  }
  uint8_t scratch[kMaxVerificationBytes];
  const long peeked = input.peek(scratch, 0, static_cast<size_t>(length));
  input.resetPeekPosition();
  if (peeked < length) {
    sniffFailureReason_ = "The first page of the bitstream is truncated.";
    return false;
  }

  AVDataBuffer signature(scratch, static_cast<uint32_t>(length));
  if (AVFlacReader::verifyBitstreamType(signature)) {
    streamReader_ = std::make_unique<AVFlacReader>();
  } else {
    signature.position(0);
    if (AVVorbisReader::verifyBitstreamType(signature)) {
      streamReader_ = std::make_unique<AVVorbisReader>();
    } else {
      signature.position(0);
      if (AVOpusReader::verifyBitstreamType(signature)) {
        streamReader_ = std::make_unique<AVOpusReader>();
      }
    }
  }
  if (streamReader_ == nullptr) {
    sniffFailureReason_ =
      "The bitstream of the Ogg container is not a Vorbis, an Opus or a FLAC one.";
    return false;
  }
  return true;
}

void AVOggExtractor::init(AVExtractorOutput& output) {
  output_ = &output;
}

AVExtractor::Result AVOggExtractor::read(AVExtractorInput& input) {
  // The sniffing that selected this extractor created the stream reader of the bitstream; an
  // extractor whose read is called without sniffing recognizes it here.
  if (streamReader_ == nullptr && !sniffInternal(input)) {
    position_ = input.getPosition();
    return kResultEndOfInput;
  }
  if (!streamReaderInitialized_ && output_ != nullptr) {
    // An Ogg container holds a single audio bitstream, therefore the container declares its single
    // track as soon as its codec is known, exactly like the Java implementation does. The track is
    // created here rather than by init because the type of the bitstream is only known once it was
    // sniffed.
    auto trackOutput = output_->track(0, MediaType::kMediaTypeAudio, AVTrackOutput::emptyConsumer());
    output_->endTracks();
    streamReader_->init(output_, std::move(trackOutput));
    streamReaderInitialized_ = true;
  }
  const Result result = streamReader_ != nullptr ? streamReader_->read(input)
                                                 : kResultEndOfInput;
  position_ = input.getPosition();
  return result;
}

bool AVOggExtractor::seek(const long position, const int64_t timeUs) {
  if (streamReader_ == nullptr) {
    // A seek before the container was parsed is left to the parse, which starts at the requested
    // position.
    return true;
  }
  return streamReader_->seek(position, timeUs);
}

} // namespace coral

