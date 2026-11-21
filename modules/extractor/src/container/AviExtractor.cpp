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

#include <extractor/container/AviExtractor.h>

#include <extractor/Utility.h>

#include <media/BlockBuffer.h>
#include <media/Format.h>
#include <media/Types.h>
#include <media/Utilities.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace coral {

namespace {

/**
 * The four character codes of the chunks that this extractor understands.
 *
 * <p>A four character code is read from the file as a little endian integer, which is what the Java
 * implementation does as well: the value of "RIFF" is therefore 0x46464952 and not the value its
 * bytes have when they are read as a big endian integer.
 */
constexpr uint32_t kFourCcRiff = 0x46464952;  // "RIFF"
constexpr uint32_t kFourCcAvi = 0x20495641;   // "AVI "
constexpr uint32_t kFourCcList = 0x5453494C;  // "LIST"
constexpr uint32_t kFourCcHdrl = 0x6C726468;  // "hdrl"
constexpr uint32_t kFourCcStrl = 0x6C727473;  // "strl"
constexpr uint32_t kFourCcStrh = 0x68727473;  // "strh"
constexpr uint32_t kFourCcStrf = 0x66727473;  // "strf"
constexpr uint32_t kFourCcStrn = 0x6E727473;  // "strn"
constexpr uint32_t kFourCcAvih = 0x68697661;  // "avih"
constexpr uint32_t kFourCcMovi = 0x69766F6D;  // "movi"
constexpr uint32_t kFourCcIdx1 = 0x31786469;  // "idx1"
constexpr uint32_t kFourCcIndx = 0x78646E69;  // "indx"
constexpr uint32_t kFourCcJunk = 0x4B4E554A;  // "JUNK"
constexpr uint32_t kFourCcAuds = 0x73647561;  // "auds"
constexpr uint32_t kFourCcVids = 0x73646976;  // "vids"

/** The format tags of the WAVEFORMATEX structures that this extractor understands. */
constexpr int kWaveFormatPcm = 0x0001;
constexpr int kWaveFormatMpegLayer3 = 0x0055;
constexpr int kWaveFormatAac = 0x00FF;
constexpr int kWaveFormatAc3 = 0x2000;
constexpr int kWaveFormatDts = 0x2001;

/** The four character codes of the video compressions that this extractor understands. */
constexpr uint32_t kCompressionMp42 = 0x3234504D;  // "MP42"
constexpr uint32_t kCompressionMp43 = 0x3334504D;  // "MP43"
constexpr uint32_t kCompressionH264 = 0x34363248;  // "H264"
constexpr uint32_t kCompressionAvc1 = 0x31637661;  // "avc1"
constexpr uint32_t kCompressionAvc1Upper = 0x31435641;  // "AVC1"
constexpr uint32_t kCompressionMpeg4 = 0x78766964;  // "divx"
constexpr uint32_t kCompressionMpeg4Upper = 0x58564944;  // "DIVX"
constexpr uint32_t kCompressionDx50 = 0x30355844;  // "DX50"
constexpr uint32_t kCompressionFmp4 = 0x34504D46;  // "FMP4"
constexpr uint32_t kCompressionXvid = 0x64697678;  // "xvid"
constexpr uint32_t kCompressionXvidUpper = 0x44495658;  // "XVID"
constexpr uint32_t kCompression3vid = 0x44495633;  // "3VID"
constexpr uint32_t kCompressionMjpg = 0x47504A4D;  // "MJPG"
constexpr uint32_t kCompressionMjpgLower = 0x67706A6D;  // "mjpg"

/** The largest body of a {@code hdrl} list that this extractor is willing to buffer. */
constexpr long kMaxHdrlBodySize = 16 * 1024 * 1024;

/** The largest chunk that this extractor is willing to buffer. */
constexpr uint32_t kMaxChunkSize = 16 * 1024 * 1024;

/** The largest {@code idx1} index that this extractor is willing to buffer. */
constexpr long kMaxIndexSize = 16 * 1024 * 1024;

/** The flag of an {@code idx1} entry that marks a key frame. */
constexpr uint32_t kAviKeyFrame = 0x10;
constexpr uint8_t kAviIndexTypeChunks = 0;
constexpr uint8_t kAviIndexTypeIndexes = 1;
constexpr uint32_t kAviIndexNonKeyFrame = 0x80000000;

/** The number of bytes of a chunk that a single read consumes at most. */
constexpr long kMaxChunkReadSize = 64 * 1024;

/**
 * The maximum number of bytes that a single read consumes while it resolves a seek. A caller that
 * stops reading when the reader reports the end of the input observes the samples of the requested
 * time as long as the distance to them is smaller than this budget; a longer skip is spread over
 * several calls.
 */
constexpr long kMaxSeekScanSize = 4 * 1024 * 1024;

/**
 * Reads an unsigned little endian integer from a byte array.
 */
uint32_t readLittleEndianUnsignedInt(const uint8_t data[], const int offset) {
  return static_cast<uint32_t>(data[offset]) | (static_cast<uint32_t>(data[offset + 1]) << 8) |
         (static_cast<uint32_t>(data[offset + 2]) << 16) |
         (static_cast<uint32_t>(data[offset + 3]) << 24);
}

/**
 * The header of one chunk of a RIFF file, as it is laid out in a buffered list.
 *
 * <p>This is the extractor module counterpart of the header that ExoPlayer's {@code ListChunk}
 * parses with its {@code ChunkHeaderHolder}.
 */
struct AviChunkHeader {
  /** The type of the chunk. */
  uint32_t type = 0;
  /** The offset of the body of the chunk in the buffer it was read from. */
  size_t bodyOffset = 0;
  /** The size of the body of the chunk, in bytes. */
  size_t bodySize = 0;
  /** The offset of the chunk that follows, its padding included. */
  size_t nextOffset = 0;
  /** Whether the header could be read, i.e. whether the chunk is well formed. */
  bool valid = false;
};

/**
 * Reads the header of the chunk that starts at the given offset of a buffered list, clamped to the
 * end of that list.
 */
AviChunkHeader readChunkHeader(const AVDataBuffer& data, const size_t offset, const size_t end) {
  AviChunkHeader header;
  const size_t available = std::min(end, static_cast<size_t>(data.limit()));
  if (offset + 8 > available) {
    return header;
  }
  header.type = data.getLittleEndianUnsignedInt(offset);
  const auto size = static_cast<size_t>(data.getLittleEndianUnsignedInt(offset + 4));
  header.bodyOffset = offset + 8;
  header.bodySize = size < available - header.bodyOffset ? size : available - header.bodyOffset;
  // The chunks of a RIFF file are word aligned, their padding is not part of their size.
  header.nextOffset = header.bodyOffset + header.bodySize + (header.bodySize & 1);
  if (header.nextOffset > available) {
    header.nextOffset = available;
  }
  header.valid = true;
  return header;
}

/**
 * The {@code AVIMAINHEADER} structure, i.e. the {@code avih} chunk of the {@code hdrl} list.
 *
 * <p>This is the extractor module counterpart of ExoPlayer's {@code AviMainHeaderChunk}.
 */
struct AviMainHeader {
  /** The duration of a frame of the movie, in microseconds. */
  int frameDurationUs = 0;
  /** The flags of the movie. */
  int flags = 0;
  /** The number of frames of the movie, which is usually not the number of frames of a stream. */
  int totalFrames = 0;
  /** The number of streams of the movie. */
  int streams = 0;
};

/**
 * The {@code AVISTREAMHEADER} structure, i.e. the {@code strh} chunk of a stream list.
 *
 * <p>This is the extractor module counterpart of ExoPlayer's {@code AviStreamHeaderChunk}.
 */
struct AviStreamHeader {
  /** The type of the stream, i.e. one of the {@code kFourCcAuds} and {@code kFourCcVids} codes. */
  uint32_t streamType = 0;
  /** The number of units of a chunk, i.e. of the {@code rate} of the stream. */
  int scale = 0;
  /** The number of units per second. */
  int rate = 0;
  /** The number of chunks of the stream, as the stream header declares it. */
  int length = 0;
  /** The largest chunk of the stream, as the stream header declares it. */
  int suggestedBufferSize = 0;
  /** The size of a sample of the stream, or 0 when its chunks have no fixed size. */
  int sampleSize = 0;

  /** Returns whether the stream carries audio. */
  [[nodiscard]] bool isAudio() const { return streamType == kFourCcAuds; }

  /** Returns whether the stream carries video. */
  [[nodiscard]] bool isVideo() const { return streamType == kFourCcVids; }

  /**
   * Returns the duration of the stream in microseconds, which its number of chunks and its rate
   * declare. Mirrors {@code AviStreamHeaderChunk.getDurationUs}.
   */
  [[nodiscard]] int64_t durationUs() const {
    if (rate <= 0 || scale <= 0 || length <= 0) {
      return 0;
    }
    return AVUtil::scaleLargeValue(length, rate,
                                   static_cast<int64_t>(scale) * AVUtil::kMicrosPerSecond);
  }
};

/**
 * The description of the samples of a stream, i.e. the {@code strf} chunk of a stream list.
 *
 * <p>This is the extractor module counterpart of ExoPlayer's {@code StreamFormatChunk}.
 */
struct AviStreamFormat {
  /** Whether the codec of the stream is described by this extractor. */
  bool supported = false;
  /** Whether the stream carries video rather than audio. */
  bool video = false;
  /** The format of the samples of the stream. Never null when {@link supported} is set. */
  std::shared_ptr<MediaFormat> format;
};

/**
 * Parses the {@code AVIMAINHEADER} structure. Mirrors {@code AviMainHeaderChunk.parseFrom}.
 */
AviMainHeader parseMainHeader(const AVDataBuffer& data, const AviChunkHeader& chunk) {
  AviMainHeader header;
  if (chunk.bodySize < 28) {
    return header;
  }
  const size_t offset = chunk.bodyOffset;
  header.frameDurationUs = static_cast<int>(data.getLittleEndianUnsignedInt(offset));
  header.flags = static_cast<int>(data.getLittleEndianUnsignedInt(offset + 12));
  header.totalFrames = static_cast<int>(data.getLittleEndianUnsignedInt(offset + 16));
  header.streams = static_cast<int>(data.getLittleEndianUnsignedInt(offset + 24));
  return header;
}

/**
 * Parses the {@code AVISTREAMHEADER} structure. Mirrors {@code AviStreamHeaderChunk.parseFrom}.
 */
AviStreamHeader parseStreamHeader(const AVDataBuffer& data, const AviChunkHeader& chunk) {
  AviStreamHeader header;
  if (chunk.bodySize < 48) {
    return header;
  }
  const size_t offset = chunk.bodyOffset;
  header.streamType = data.getLittleEndianUnsignedInt(offset);
  header.scale = static_cast<int>(data.getLittleEndianUnsignedInt(offset + 20));
  header.rate = static_cast<int>(data.getLittleEndianUnsignedInt(offset + 24));
  header.length = static_cast<int>(data.getLittleEndianUnsignedInt(offset + 32));
  header.suggestedBufferSize = static_cast<int>(data.getLittleEndianUnsignedInt(offset + 36));
  header.sampleSize = static_cast<int>(data.getLittleEndianUnsignedInt(offset + 44));
  return header;
}

/**
 * Returns the MIME type of an audio stream whose format tag is the given one, or an empty string
 * when the codec is not described by this extractor. Mirrors
 * {@code StreamFormatChunk.getMimeTypeFromTag}.
 */
std::string audioMimeType(const int formatTag) {
  switch (formatTag) {
    case kWaveFormatPcm: {
      return MimeTypes::AUDIO_RAW;
    }
    case kWaveFormatMpegLayer3: {
      return MimeTypes::AUDIO_MPEG;
    }
    case kWaveFormatAac: {
      return MimeTypes::AUDIO_AAC;
    }
    case kWaveFormatAc3: {
      return MimeTypes::AUDIO_AC3;
    }
    case kWaveFormatDts: {
      return MimeTypes::AUDIO_DTS;
    }
    default: {
      return {};
    }
  }
}

/**
 * Returns the MIME type of a video stream whose compression is the given four character code, or an
 * empty string when the codec is not described by this extractor. Mirrors
 * {@code StreamFormatChunk.getMimeTypeFromCompression}.
 */
std::string videoMimeType(const uint32_t compression) {
  switch (compression) {
    case kCompressionMp42: {
      return MimeTypes::VIDEO_MP42;
    }
    case kCompressionMp43: {
      return MimeTypes::VIDEO_MP43;
    }
    case kCompressionH264:
    case kCompressionAvc1:
    case kCompressionAvc1Upper: {
      return MimeTypes::VIDEO_H264;
    }
    case kCompressionMpeg4:
    case kCompressionMpeg4Upper:
    case kCompressionDx50:
    case kCompressionFmp4:
    case kCompressionXvid:
    case kCompressionXvidUpper:
    case kCompression3vid: {
      return MimeTypes::VIDEO_MP4V;
    }
    case kCompressionMjpg:
    case kCompressionMjpgLower: {
      return MimeTypes::VIDEO_MJPEG;
    }
    default: {
      return {};
    }
  }
}

/**
 * Parses the {@code BITMAPINFOHEADER} structure of a video stream. Mirrors the
 * {@code StreamFormatChunk.parseBitmapInfoHeader} helper.
 */
AviStreamFormat parseVideoFormat(const AVDataBuffer& data, const AviChunkHeader& chunk) {
  AviStreamFormat out;
  if (chunk.bodySize < 20) {
    return out;
  }
  const size_t offset = chunk.bodyOffset;
  // The layout of the structure is: the size of the header, the width, the height, the planes and
  // the number of bits per pixel, and then the compression of the frames.
  const int width = static_cast<int>(data.getLittleEndianUnsignedInt(offset + 4));
  const int height = static_cast<int>(data.getLittleEndianUnsignedInt(offset + 8));
  const uint32_t compression = data.getLittleEndianUnsignedInt(offset + 16);
  const std::string mimeType = videoMimeType(compression);
  if (mimeType.empty() || width <= 0 || height <= 0) {
    return out;
  }
  out.supported = true;
  out.video = true;
  out.format = MediaFormat::CreateVideoFormat(mimeType, width, height);
  return out;
}

/**
 * Parses the {@code WAVEFORMATEX} structure of an audio stream. Mirrors the
 * {@code StreamFormatChunk.parseWaveFormatEx} helper.
 */
AviStreamFormat parseAudioFormat(const AVDataBuffer& data, const AviChunkHeader& chunk) {
  AviStreamFormat out;
  if (chunk.bodySize < 16) {
    return out;
  }
  const size_t offset = chunk.bodyOffset;
  const int formatTag = data.getLittleEndianUnsignedShort(offset);
  const std::string mimeType = audioMimeType(formatTag);
  if (mimeType.empty()) {
    return out;
  }
  const int channelCount = data.getLittleEndianUnsignedShort(offset + 2);
  const int sampleRate = static_cast<int>(data.getLittleEndianUnsignedInt(offset + 4));
  const int bitsPerSample = data.getLittleEndianUnsignedShort(offset + 14);
  if (channelCount <= 0 || sampleRate <= 0) {
    return out;
  }
  out.supported = true;
  out.video = false;
  out.format = MediaFormat::CreateAudioFormat(mimeType, sampleRate, channelCount);
  if (mimeType == MimeTypes::AUDIO_RAW && bitsPerSample > 0) {
    out.format->setSampleFormat(MediaUtils::getSampleFormat(bitsPerSample));
  }
  if (mimeType == MimeTypes::AUDIO_AAC && chunk.bodySize >= 18) {
    // The extension of the structure holds the AudioSpecificConfig of the stream.
    const size_t extensionSize = data.getLittleEndianUnsignedShort(offset + 16);
    if (extensionSize > 0 && offset + 18 + extensionSize <= chunk.bodyOffset + chunk.bodySize) {
      out.format->setCodecSpecificData(0, data.array() + offset + 18, extensionSize);
    }
  }
  return out;
}

/**
 * Returns whether every sample of a stream whose samples have the given MIME type is a sync sample.
 *
 * <p>This is the extractor module counterpart of {@code MimeTypes.allSamplesAreSyncSamples}: every
 * audio codec that a RIFF file carries is intra only, i.e. an audio sample never depends on another
 * one, and so is Motion JPEG, whose frames are independent images.
 */
bool allSamplesAreSyncSamples(const std::string& mimeType) {
  return MimeTypes::isAudio(mimeType) || mimeType == MimeTypes::VIDEO_MJPEG;
}

/**
 * Returns the four character code of the chunks of a stream.
 *
 * <p>The identifier of a chunk is the two digits of the identifier of its stream, followed by the
 * two characters of its type: {@code dc} for a compressed video frame, {@code db} for an
 * uncompressed one and {@code wb} for an audio block. This is the extractor module counterpart of
 * {@code ChunkReader.getChunkIdFourCc}.
 */
uint32_t chunkIdFourCc(const int streamId, const bool video, const bool uncompressed) {
  const int tens = streamId / 10;
  const int ones = streamId % 10;
  uint32_t type = (static_cast<uint32_t>('w') << 16) | (static_cast<uint32_t>('b') << 24);
  if (video) {
    type = (static_cast<uint32_t>('d') << 16) |
           (uncompressed ? (static_cast<uint32_t>('b') << 24) : (static_cast<uint32_t>('c') << 24));
  }
  return (static_cast<uint32_t>('0' + ones) << 8) | static_cast<uint32_t>('0' + tens) | type;
}

} // namespace anonymous

/**
 * Reads the chunks that hold the samples of one stream of an AVI file.
 *
 * <p>The reader buffers the chunk it is reading, because the chunks of the {@code movi} list are the
 * units that are delivered as samples, and because a chunk that spans several pages of the input has
 * to be assembled before it can be delivered. The timestamps of the chunks are interpolated from the
 * duration of the stream and from its number of chunks, which is what the Java implementation does:
 * the index of the file, which would give the position of every key frame, is stored after the
 * samples it describes and is therefore not reachable without moving backwards in the input.
 *
 * <p>This is the extractor module counterpart of ExoPlayer's {@code ChunkReader}.
 */
class AVChunkReader {
public:
  AVChunkReader(const int streamId, const AviStreamHeader& streamHeader, const bool video,
                std::shared_ptr<AVTrackOutput> trackOutput, const bool allSamplesAreSyncSamples)
    : video_(video),
      trackOutput_(std::move(trackOutput)),
      allSamplesAreSyncSamples_(allSamplesAreSyncSamples),
      chunkId_(chunkIdFourCc(streamId, video, false)),
      alternativeChunkId_(chunkIdFourCc(streamId, video, true)),
      durationUs_(streamHeader.durationUs()),
      chunkCount_(streamHeader.length > 0 ? streamHeader.length : 1) {
  }

  ~AVChunkReader() = default;

  AVChunkReader(const AVChunkReader& other) = delete;

  AVChunkReader& operator=(const AVChunkReader& other) = delete;

  /** Returns whether this reader handles the chunks that have the given identifier. */
  [[nodiscard]] bool handlesChunkId(const uint32_t chunkId) const {
    return chunkId_ == chunkId || (video_ && alternativeChunkId_ == chunkId);
  }

  /**
   * Returns whether the given identifier is the uncompressed one of a video stream, which is how a
   * file that carries no index marks its key frames.
   */
  [[nodiscard]] bool isUncompressedChunkId(const uint32_t chunkId) const {
    return video_ && chunkId == alternativeChunkId_;
  }

  /** Returns whether the chunks of the stream are compressed video frames. */
  [[nodiscard]] bool isVideo() const { return video_; }

  /**
   * Sets the time that the delivered samples must reach, which a seek uses to skip the samples that
   * precede its target, or -1 when every sample is delivered.
   */
  void setSkipBeforeTimeUs(const int64_t timeUs) { skipBeforeTimeUs_ = timeUs; }

  /** Returns whether the reader is still skipping the samples that precede the target of a seek. */
  [[nodiscard]] bool isSkipping() const { return skipBeforeTimeUs_ >= 0; }

  /** Puts the cursor of the reader back on the first chunk of the stream. */
  void resetCursors() {
    currentChunkIndex_ = 0;
    currentChunkSize_ = 0;
    bytesRemaining_ = 0;
    currentChunk_.reset();
    skipBeforeTimeUs_ = -1;
  }

  /** Prepares for reading a chunk of the given size. */
  void onChunkStart(const uint32_t size, const bool keyFrameChunk, const int64_t indexedTimeUs = -1) {
    currentChunkSize_ = size;
    bytesRemaining_ = static_cast<long>(size);
    currentChunkTimestampUs_ = indexedTimeUs;
    // A chunk of a video stream is a key frame when the file framed it as an uncompressed frame,
    // which is the only description of a key frame that a container without an index carries. The
    // first chunk is always a key frame: the index of the file, which would mark the others, is
    // stored after the samples and is not read before the playback starts.
    currentChunkKeyFrame_ = keyFrameChunk || allSamplesAreSyncSamples_
                            || currentChunkIndex_ == 0;
    currentChunk_ = size > 0 ? std::make_shared<AVBlockBuffer>(size) : nullptr;
  }

  /** Moves the reader to the sample with the given index, for a seek into the index. */
  void setCurrentChunkIndex(const int chunkIndex) {
    currentChunkIndex_ = chunkIndex;
    currentChunkTimestampUs_ = -1;
  }

  /** Returns the timestamp of the sample with the given index. */
  [[nodiscard]] int64_t timestampOf(const int chunkIndex) const {
    return chunkTimestampUs(chunkIndex);
  }

  /**
   * Provides the data of the current chunk to the reader.
   *
   * @return {@code true} when the whole chunk was read, i.e. when it was delivered or dropped.
   */
  bool onChunkData(AVExtractorInput& input) {
    const long toRead = std::min<long>(bytesRemaining_, kMaxChunkReadSize);
    if (toRead > 0) {
      const size_t written = static_cast<size_t>(currentChunkSize_) - static_cast<size_t>(bytesRemaining_);
      const long read = input.read(currentChunk_->array() + written, 0, static_cast<size_t>(toRead));
      if (read == kEndOfInput) {
        // The chunk is truncated, the partial sample cannot be decoded and is dropped.
        currentChunkSize_ = 0;
        bytesRemaining_ = 0;
        currentChunk_.reset();
        ++currentChunkIndex_;
        return true;
      }
      bytesRemaining_ -= read;
    }
    if (bytesRemaining_ > 0) {
      return false;
    }
    deliverChunk();
    return true;
  }

  /** Reports the end of the stream to the track. */
  void endTrack() {
    if (trackOutput_ != nullptr) {
      trackOutput_->endTrack();
    }
  }

private:
  /** Returns the timestamp of the chunk that has the given index. */
  [[nodiscard]] int64_t chunkTimestampUs(const int chunkIndex) const {
    return durationUs_ * chunkIndex / chunkCount_;
  }

  /** Delivers the chunk that was read, or drops it when a seek is skipping to its target. */
  void deliverChunk() {
    const int64_t timeUs = currentChunkTimestampUs_ >= 0
                             ? currentChunkTimestampUs_
                             : chunkTimestampUs(currentChunkIndex_);
    ++currentChunkIndex_;
    if (currentChunk_ == nullptr || trackOutput_ == nullptr) {
      return;
    }
    if (skipBeforeTimeUs_ >= 0 && timeUs < skipBeforeTimeUs_) {
      // The sample precedes the target of a seek that is being resolved.
      currentChunk_.reset();
      return;
    }
    skipBeforeTimeUs_ = -1;
    currentChunk_->position(0);
    currentChunk_->limit(currentChunkSize_);
    uint32_t flags = currentChunkKeyFrame_ ? AVSample::kFlagKeyFrame : AVSample::kFlagNone;
    if (!firstSampleDelivered_) {
      flags |= AVSample::kFlagFirstSample;
      firstSampleDelivered_ = true;
    }
    AVSample sample(trackOutput_->id(), timeUs, flags, currentChunk_);
    trackOutput_->sample(sample);
    // The consumer may keep the buffer of the sample, releasing the reference of the reader is what
    // makes the memory of a chunk reusable.
    currentChunk_.reset();
  }

  bool video_;
  std::shared_ptr<AVTrackOutput> trackOutput_;
  bool allSamplesAreSyncSamples_;
  uint32_t chunkId_;
  uint32_t alternativeChunkId_;
  int64_t durationUs_;
  int chunkCount_;
  int currentChunkIndex_ = 0;
  uint32_t currentChunkSize_ = 0;
  long bytesRemaining_ = 0;
  bool currentChunkKeyFrame_ = false;
  std::shared_ptr<AVBlockBuffer> currentChunk_;
  int64_t currentChunkTimestampUs_ = -1;
  int64_t skipBeforeTimeUs_ = -1;
  bool firstSampleDelivered_ = false;
};

// ==================================================================================================
// AVAviExtractor
// ==================================================================================================

AVAviExtractor::AVAviExtractor() noexcept = default;

AVAviExtractor::~AVAviExtractor() = default;

int AVAviExtractor::sniff(AVExtractorInput& input) {
  sniffFailureReason_.clear();
  input.resetPeekPosition();
  uint8_t scratch[12];
  const long peeked = input.peek(scratch, 0, sizeof(scratch));
  input.resetPeekPosition();
  if (peeked < static_cast<long>(sizeof(scratch))) {
    sniffFailureReason_ = "The input is shorter than a RIFF header.";
    return AVExtractorsFactory::kSniffResultNone;
  }
  if (readLittleEndianUnsignedInt(scratch, 0) != kFourCcRiff) {
    sniffFailureReason_ = "The RIFF identifier was not found.";
    return AVExtractorsFactory::kSniffResultNone;
  }
  if (readLittleEndianUnsignedInt(scratch, 8) != kFourCcAvi) {
    sniffFailureReason_ = "The form type of the RIFF file is not AVI.";
    return AVExtractorsFactory::kSniffResultNone;
  }
  return AVExtractorsFactory::kSniffResultCertain;
}

void AVAviExtractor::init(AVExtractorOutput& output) {
  output_ = &output;
  state_ = State::kSkippingToHdrl;
  currentChunkReader_ = nullptr;
  chunkReaders_.clear();
  hdrlBodySize_ = 0;
  moviStart_ = -1;
  moviDataStart_ = -1;
  moviEnd_ = -1;
  indexedChunks_.clear();
  indexedChunkByPosition_.clear();
  openDmlIndexes_.clear();
  indexAvailable_ = false;
  durationUs_ = Definitions::kTimeUnset;
  skipBeforeTimeUs_ = -1;
  headersParsed_ = false;
  seekMapPublished_ = false;
  trackEnded_ = false;
}

AVExtractor::Result AVAviExtractor::read(AVExtractorInput& input) {
  const long callStartPosition = input.getPosition();
  while (true) {
    const long positionBefore = input.getPosition();
    const Result result = readStep(input);
    position_ = input.getPosition();
    if (result == kResultEndOfInput) {
      return result;
    }
    if (skipBeforeTimeUs_ < 0) {
      // No seek is pending: one step per call, so that the samples are streamed rather than read as
      // fast as the input allows.
      return kResultContinue;
    }
    if (hasDeliveredSample()) {
      // The seek reached its target, the caller observes the samples that follow it.
      skipBeforeTimeUs_ = -1;
      return kResultContinue;
    }
    if (input.getPosition() == positionBefore ||
        input.getPosition() - callStartPosition >= kMaxSeekScanSize) {
      // The step consumed nothing, or the target of the seek is still far away: the rest of the
      // skip is spread over the calls that follow.
      return kResultContinue;
    }
  }
}

bool AVAviExtractor::buildOpenDmlIndex(AVExtractorInput& input, const long moviListStart,
                                       const long moviDataStart) {
  // OpenDML stores one standard index per stream, and a super index that points to the standard
  // indexes of the later RIFF chunks. Both forms use absolute positions, so they are merged into the
  // same table as the idx1 entries.
  indexedChunks_.clear();
  indexedChunkByPosition_.clear();
  std::unordered_map<AVChunkReader*, int> counts;

  const auto addChunk = [&](const uint32_t chunkId, const uint64_t position, const uint32_t size,
                            const bool keyFrame) {
    if (position > static_cast<uint64_t>(std::numeric_limits<long>::max())) {
      return;
    }
    AVChunkReader* const reader = findChunkReader(chunkId);
    if (reader == nullptr || size == 0) {
      return;
    }
    IndexedChunk chunk;
    chunk.reader = reader;
    chunk.sampleIndex = counts[reader]++;
    chunk.timeUs = reader->timestampOf(chunk.sampleIndex);
    chunk.position = static_cast<long>(position);
    chunk.keyFrame = keyFrame;
    indexedChunks_.push_back(chunk);
    indexedChunkByPosition_[chunk.position] = chunk;
  };

  const auto addStandardIndex = [&](const uint32_t chunkId, const uint64_t baseOffset,
                                    const uint8_t* entries, const uint32_t entryCount) {
    for (uint32_t i = 0; i < entryCount; ++i) {
      const size_t offset = static_cast<size_t>(i) * 8;
      const uint32_t entryOffset = readLittleEndianUnsignedInt(entries, static_cast<int>(offset));
      uint32_t entrySize = readLittleEndianUnsignedInt(entries, static_cast<int>(offset + 4));
      const bool keyFrame = (entrySize & kAviIndexNonKeyFrame) == 0;
      entrySize &= ~kAviIndexNonKeyFrame;
      addChunk(chunkId, baseOffset + entryOffset, entrySize, keyFrame);
    }
  };

  for (const auto& raw : openDmlIndexes_) {
    if (raw.indexType == kAviIndexTypeChunks) {
      for (size_t i = 0; i < raw.entryOffsets.size() && i < raw.entrySizes.size(); ++i) {
        uint32_t size = raw.entrySizes[i];
        const bool keyFrame = (size & kAviIndexNonKeyFrame) == 0;
        size &= ~kAviIndexNonKeyFrame;
        addChunk(raw.chunkId, raw.baseOffset + raw.entryOffsets[i], size, keyFrame);
      }
      continue;
    }
    if (raw.indexType != kAviIndexTypeIndexes) {
      continue;
    }
    for (size_t i = 0; i < raw.subIndexOffsets.size(); ++i) {
      const uint64_t subOffset = raw.subIndexOffsets[i];
      if (subOffset > static_cast<uint64_t>(std::numeric_limits<long>::max())) {
        continue;
      }
      if (!input.setPosition(static_cast<long>(subOffset), true)) {
        continue;
      }
      uint8_t header[12] = {0, 0, 0, 0};
      input.resetPeekPosition();
      const long peeked = input.peek(header, 0, sizeof(header));
      input.resetPeekPosition();
      if (peeked < 8 || readLittleEndianUnsignedInt(header, 0) != kFourCcIndx) {
        continue;
      }
      const uint32_t bodySize = readLittleEndianUnsignedInt(header, 4);
      if (bodySize < 24 || bodySize > static_cast<uint32_t>(kMaxIndexSize)) {
        continue;
      }
      if (!input.setPosition(static_cast<long>(subOffset) + 8, true)) {
        continue;
      }
      std::vector<uint8_t> body(bodySize);
      if (!input.readFully(body.data(), 0, bodySize)) {
        continue;
      }
      const uint32_t entryCount = readLittleEndianUnsignedInt(body.data(), 4);
      const uint32_t chunkId = readLittleEndianUnsignedInt(body.data(), 8);
      const uint64_t baseOffset =
        static_cast<uint64_t>(readLittleEndianUnsignedInt(body.data(), 12)) |
        (static_cast<uint64_t>(readLittleEndianUnsignedInt(body.data(), 16)) << 32);
      if (bodySize < 24 + static_cast<size_t>(entryCount) * 8) {
        continue;
      }
      addStandardIndex(chunkId, baseOffset, body.data() + 24, entryCount);
    }
  }

  if (indexedChunks_.empty()) {
    return false;
  }
  long first = std::numeric_limits<long>::max();
  long last = 0;
  for (const auto& chunk : indexedChunks_) {
    first = std::min(first, chunk.position);
    last = std::max(last, chunk.position);
  }
  moviStart_ = moviListStart;
  moviDataStart_ = std::min<long>(moviDataStart, first);
  if (moviEnd_ < 0 || last >= moviEnd_) {
    moviEnd_ = input.getLength() > last ? input.getLength() : last + 1;
  }
  indexAvailable_ = true;
  return true;
}

bool AVAviExtractor::scanIndex(AVExtractorInput& input) {
  const long scanStart = input.getPosition();
  long moviListStart = -1;
  long moviDataStart = -1;
  long moviListEnd = -1;
  long indexBody = -1;
  long indexSize = 0;

  const auto moveTo = [&input](const long position) {
    return input.setPosition(position, true);
  };

  long position = scanStart;
  const long length = input.getLength();
  while (length < 0 || position + 8 <= length) {
    if ((position & 1) == 1) {
      ++position;
    }
    if (!moveTo(position)) {
      return false;
    }
    uint8_t header[12] = {0, 0, 0, 0};
    input.resetPeekPosition();
    const long peeked = input.peek(header, 0, sizeof(header));
    input.resetPeekPosition();
    if (peeked < 8) {
      break;
    }
    const uint32_t type = readLittleEndianUnsignedInt(header, 0);
    const uint32_t size = readLittleEndianUnsignedInt(header, 4);
    const uint32_t listType = peeked >= 12 ? readLittleEndianUnsignedInt(header, 8) : 0;
    const long body = position + 8;
    if (type == kFourCcList && listType == kFourCcMovi) {
      moviListStart = position;
      moviDataStart = position + 12;
      moviListEnd = body + static_cast<long>(size);
      position = moviListEnd;
      continue;
    }
    if (type == kFourCcIdx1) {
      indexBody = body;
      indexSize = size;
    }
    position = body + static_cast<long>(size) + static_cast<long>(size & 1);
  }

  if (moviListStart < 0 || moviDataStart < 0) {
    return false;
  }
  if (!openDmlIndexes_.empty()) {
    if (!buildOpenDmlIndex(input, moviListStart, moviDataStart)) {
      return false;
    }
    return moveTo(moviDataStart) && indexAvailable_;
  }
  if (indexBody < 0 || indexSize <= 0 || indexSize > kMaxIndexSize || (indexSize % 16) != 0) {
    return false;
  }
  if (!moveTo(indexBody)) {
    return false;
  }
  std::vector<uint8_t> indexData(static_cast<size_t>(indexSize));
  if (!input.readFully(indexData.data(), 0, static_cast<size_t>(indexSize))) {
    return false;
  }
  if (!moveTo(moviDataStart)) {
    return false;
  }

  moviStart_ = moviListStart;
  moviDataStart_ = moviDataStart;
  moviEnd_ = moviListEnd;

  std::unordered_map<AVChunkReader*, int> counts;
  indexedChunks_.clear();
  indexedChunkByPosition_.clear();
  for (long offset = 0; offset + 16 <= indexSize; offset += 16) {
    const uint32_t chunkId = readLittleEndianUnsignedInt(indexData.data(), static_cast<int>(offset));
    const uint32_t flags = readLittleEndianUnsignedInt(indexData.data(), static_cast<int>(offset + 4));
    const uint32_t chunkOffset = readLittleEndianUnsignedInt(indexData.data(), static_cast<int>(offset + 8));
    const uint32_t chunkSize = readLittleEndianUnsignedInt(indexData.data(), static_cast<int>(offset + 12));
    const long chunkPosition = moviListStart + 8 + static_cast<long>(chunkOffset);
    if (chunkSize == 0 || chunkPosition < moviDataStart || chunkPosition >= moviListEnd) {
      continue;
    }
    AVChunkReader* const reader = findChunkReader(chunkId);
    if (reader == nullptr) {
      continue;
    }
    IndexedChunk chunk;
    chunk.reader = reader;
    chunk.sampleIndex = counts[reader]++;
    chunk.timeUs = reader->timestampOf(chunk.sampleIndex);
    chunk.position = chunkPosition;
    chunk.keyFrame = (flags & kAviKeyFrame) != 0;
    indexedChunks_.push_back(chunk);
    indexedChunkByPosition_[chunkPosition] = chunk;
  }
  indexAvailable_ = !indexedChunks_.empty();
  return indexAvailable_;
}

AVExtractor::Result AVAviExtractor::readStep(AVExtractorInput& input) {
  switch (state_) {
    case State::kSkippingToHdrl: {
      // The header of the RIFF form is "RIFF", its size and the form type, i.e. twelve bytes that
      // the sniffing already validated.
      if (!input.skipFully(12)) {
        return finish(input);
      }
      state_ = State::kReadingHdrlHeader;
      position_ = input.getPosition();
      return kResultContinue;
    }
    case State::kReadingHdrlHeader: {
      if (!input.readFully(scratch_, 12)) {
        return finish(input);
      }
      const uint32_t type = scratch_.readLittleEndianUnsignedInt();
      const uint32_t size = scratch_.readLittleEndianUnsignedInt();
      const uint32_t listType = scratch_.readLittleEndianUnsignedInt();
      if (type != kFourCcList || listType != kFourCcHdrl || size < 4) {
        return finish(input);
      }
      // The size of the list includes its type, which was read already.
      hdrlBodySize_ = static_cast<long>(size) - 4;
      state_ = State::kReadingHdrlBody;
      position_ = input.getPosition();
      return kResultContinue;
    }
    case State::kReadingHdrlBody: {
      // The body of the list is the only part of the container that is buffered, therefore its size
      // is validated against the length of the input before it is allocated.
      const long length = input.getLength();
      if (hdrlBodySize_ < 0 || hdrlBodySize_ > kMaxHdrlBodySize ||
          (length >= 0 && input.getPosition() + hdrlBodySize_ > length)) {
        return finish(input);
      }
      if (!input.readFully(hdrlBody_, static_cast<size_t>(hdrlBodySize_))) {
        return finish(input);
      }
      parseHdrlBody(hdrlBody_);
      headersParsed_ = true;
      const long afterHdrl = input.getPosition();
      if (scanIndex(input)) {
        std::vector<AVSeekMap::SeekPoint> seekPoints;
        for (const auto& chunk : indexedChunks_) {
          if (chunk.keyFrame && chunk.reader != nullptr && chunk.reader->isVideo()) {
            seekPoints.emplace_back(chunk.timeUs, chunk.position);
          }
        }
        std::sort(seekPoints.begin(), seekPoints.end(),
          [](const AVSeekMap::SeekPoint& left, const AVSeekMap::SeekPoint& right) {
            return left.timeUs < right.timeUs;
          });
        if (output_ != nullptr) {
          output_->seekMap(std::make_shared<AVIndexSeekMap>(
            std::move(seekPoints), durationUs_, !seekPoints.empty()));
        }
        state_ = State::kReadingSamples;
      } else {
        if (!input.setPosition(afterHdrl, true)) {
          return finish(input);
        }
        if (!seekMapPublished_ && output_ != nullptr) {
          // The index of the file is not available: the seek map reports the duration and no seek
          // points, and a seek restarts the container at its beginning.
          output_->seekMap(std::make_shared<AVUnseekableSeekMap>(durationUs_));
        }
        state_ = State::kFindingMoviHeader;
      }
      if (output_ != nullptr) {
        output_->endTracks();
      }
      seekMapPublished_ = true;
      position_ = input.getPosition();
      return kResultContinue;
    }
    case State::kFindingMoviHeader: {
      // The list that holds the samples is found by walking the chunks of the file, which are all
      // skipped: only the header of every chunk is peeked.
      uint8_t scratch[12];
      input.resetPeekPosition();
      const long peeked = input.peek(scratch, 0, sizeof(scratch));
      const uint32_t chunkType = peeked >= 8 ? readLittleEndianUnsignedInt(scratch, 0) : 0;
      // The size of a chunk is an unsigned value that a malformed file may declare larger than the
      // input: the ones that do not fit the position of the input are rejected here.
      const int64_t declaredSize =
        peeked >= 8 ? static_cast<int64_t>(readLittleEndianUnsignedInt(scratch, 4)) : 0;
      const long chunkSize = declaredSize <= std::numeric_limits<long>::max()
                               ? static_cast<long>(declaredSize)
                               : -1;
      const uint32_t listType = peeked >= 12 ? readLittleEndianUnsignedInt(scratch, 8) : 0;
      input.resetPeekPosition();
      if (peeked < 8 || chunkSize < 0) {
        return finish(input);
      }
      if (chunkType == kFourCcRiff) {
        // The header of the form, i.e. the twelve bytes that the parse skipped, is read again when
        // the container is restarted.
        if (!input.skipFully(12)) {
          return finish(input);
        }
        position_ = input.getPosition();
        return kResultContinue;
      }
      if (peeked >= 12 && chunkType == kFourCcList && listType == kFourCcMovi) {
        moviStart_ = input.getPosition();
        moviEnd_ = moviStart_ + chunkSize + 8;
        // The samples start after the header of the list, its type included.
        if (!input.skipFully(12)) {
          return finish(input);
        }
        state_ = State::kReadingSamples;
        position_ = input.getPosition();
        return kResultContinue;
      }
      // Every other chunk, the description of the streams and the index of the samples included, is
      // skipped entirely, its padding included.
      if (!input.skipFully(chunkSize + 8 + (chunkSize & 1))) {
        return finish(input);
      }
      position_ = input.getPosition();
      return kResultContinue;
    }
    case State::kReadingSamples: {
      return readMoviChunks(input);
    }
    case State::kEndOfInput:
    default: {
      return kResultEndOfInput;
    }
  }
}

bool AVAviExtractor::seek(const long position, const int64_t timeUs) {
  if (position > 0) {
    if (!indexAvailable_ || moviDataStart_ < 0 || moviEnd_ < 0 ||
        position < moviDataStart_ || position >= moviEnd_) {
      return false;
    }
    currentChunkReader_ = nullptr;
    skipBeforeTimeUs_ = -1;
    for (auto& reader : chunkReaders_) {
      reader->resetCursors();
      reader->setSkipBeforeTimeUs(-1);
    }
    // Start every stream at the sample that the index places at or after the target. The target is
    // the position of a key frame of the video stream, so the video reader starts exactly on it.
    for (auto& reader : chunkReaders_) {
      for (const auto& chunk : indexedChunks_) {
        if (chunk.reader == reader.get() && chunk.position >= position) {
          reader->setCurrentChunkIndex(chunk.sampleIndex);
          break;
        }
      }
    }
    state_ = State::kReadingSamples;
    position_ = position;
    return true;
  }
  currentChunkReader_ = nullptr;
  for (auto& reader : chunkReaders_) {
    reader->resetCursors();
    reader->setSkipBeforeTimeUs(timeUs);
  }
  skipBeforeTimeUs_ = timeUs;
  if (headersParsed_) {
    // The description of the streams is kept, the parse restarts at the list that holds the samples.
    state_ = State::kFindingMoviHeader;
    moviStart_ = -1;
    moviEnd_ = -1;
  } else {
    state_ = State::kSkippingToHdrl;
  }
  return true;
}

void AVAviExtractor::parseHdrlBody(const AVDataBuffer& data) {
  const size_t end = data.limit();
  size_t offset = 0;
  int streamId = 0;
  AviMainHeader mainHeader;
  bool hasMainHeader = false;

  while (offset + 8 <= end) {
    const AviChunkHeader chunk = readChunkHeader(data, offset, end);
    if (!chunk.valid) {
      break;
    }
    if (chunk.type == kFourCcList && chunk.bodySize >= 4 &&
        data.getLittleEndianUnsignedInt(chunk.bodyOffset) == kFourCcStrl) {
      // The identifier of a stream increments for every stream list, the ones that are skipped
      // included: it is what the identifiers of the chunks of the samples are derived from.
      parseStreamList(data, chunk.bodyOffset + 4, chunk.bodyOffset + chunk.bodySize, streamId);
      ++streamId;
    } else if (chunk.type == kFourCcAvih) {
      mainHeader = parseMainHeader(data, chunk);
      hasMainHeader = true;
    }
    if (chunk.nextOffset <= offset) {
      break;
    }
    offset = chunk.nextOffset;
  }

  if (hasMainHeader) {
    // The duration of the main header is usually wrong, the duration of the longest stream wins.
    durationUs_ = std::max<int64_t>(durationUs_,
                                    static_cast<int64_t>(mainHeader.totalFrames) *
                                      mainHeader.frameDurationUs);
  }
}

void AVAviExtractor::parseStreamList(const AVDataBuffer& data, const size_t start, const size_t end,
                                     const int streamId) {
  AviStreamHeader streamHeader;
  AviStreamFormat streamFormat;
  std::string name;
  bool hasStreamHeader = false;

  size_t offset = start;
  while (offset + 8 <= end) {
    const AviChunkHeader chunk = readChunkHeader(data, offset, end);
    if (!chunk.valid) {
      break;
    }
    if (chunk.type == kFourCcStrh) {
      streamHeader = parseStreamHeader(data, chunk);
      hasStreamHeader = true;
    } else if (chunk.type == kFourCcStrf) {
      // The format chunk describes the samples of the stream whose type the stream header declared,
      // therefore it is only read once that header was found.
      if (hasStreamHeader && streamHeader.isVideo()) {
        streamFormat = parseVideoFormat(data, chunk);
      } else if (hasStreamHeader && streamHeader.isAudio()) {
        streamFormat = parseAudioFormat(data, chunk);
      }
    } else if (chunk.type == kFourCcStrn && chunk.bodySize > 0) {
      name.assign(reinterpret_cast<const char*>(data.array() + chunk.bodyOffset), chunk.bodySize);
      // The name of a stream is a string that the container pads with a null byte.
      const size_t nullByte = name.find('\0');
      if (nullByte != std::string::npos) {
        name.resize(nullByte);
      }
    } else if (chunk.type == kFourCcIndx && chunk.bodySize >= 24) {
      const size_t body = chunk.bodyOffset;
      const auto read64 = [&data](const size_t at) {
        return static_cast<uint64_t>(data.getLittleEndianUnsignedInt(at)) |
               (static_cast<uint64_t>(data.getLittleEndianUnsignedInt(at + 4)) << 32);
      };
      OpenDmlIndex index;
      index.chunkId = data.getLittleEndianUnsignedInt(body + 8);
      index.indexType = data.array()[body + 3];
      index.baseOffset = read64(body + 12);
      const uint32_t entryCount = data.getLittleEndianUnsignedInt(body + 4);
      if (index.indexType == kAviIndexTypeChunks) {
        const size_t entryStart = body + 24;
        if (entryStart + static_cast<size_t>(entryCount) * 8 <= body + chunk.bodySize) {
          for (uint32_t i = 0; i < entryCount; ++i) {
            const size_t entry = entryStart + static_cast<size_t>(i) * 8;
            index.entryOffsets.push_back(data.getLittleEndianUnsignedInt(entry));
            index.entrySizes.push_back(data.getLittleEndianUnsignedInt(entry + 4));
          }
        }
      } else if (index.indexType == kAviIndexTypeIndexes) {
        const size_t entryStart = body + 24;
        if (entryStart + static_cast<size_t>(entryCount) * 16 <= body + chunk.bodySize) {
          for (uint32_t i = 0; i < entryCount; ++i) {
            const size_t entry = entryStart + static_cast<size_t>(i) * 16;
            index.subIndexOffsets.push_back(read64(entry));
            index.subIndexSizes.push_back(data.getLittleEndianUnsignedInt(entry + 8));
          }
        }
      }
      openDmlIndexes_.push_back(std::move(index));
    }
    if (chunk.nextOffset <= offset) {
      break;
    }
    offset = chunk.nextOffset;
  }

  if (!hasStreamHeader || !streamFormat.supported || streamFormat.format == nullptr ||
      output_ == nullptr) {
    // The stream carries a codec that this extractor does not describe, or no audio and no video.
    return;
  }

  const MediaType mediaType =
    streamFormat.video ? MediaType::kMediaTypeVideo : MediaType::kMediaTypeAudio;
  auto trackOutput = output_->track(streamId, mediaType, AVTrackOutput::emptyConsumer());
  const int64_t durationUs = streamHeader.durationUs();
  auto format = streamFormat.format;
  format->setId(streamId);
  format->setTrackId(streamId);
  format->setDurationUs(durationUs);
  if (streamHeader.suggestedBufferSize > 0) {
    format->setMaxInputSize(streamHeader.suggestedBufferSize);
  }
  if (!name.empty()) {
    format->setLabel(name);
  }
  trackOutput->format(format);
  durationUs_ = std::max(durationUs_, durationUs);
  chunkReaders_.push_back(std::make_unique<AVChunkReader>(
    streamId, streamHeader, streamFormat.video, std::move(trackOutput),
    allSamplesAreSyncSamples(format->getMimeType())));
}

AVChunkReader* AVAviExtractor::findChunkReader(const uint32_t chunkId) const {
  for (const auto& reader : chunkReaders_) {
    if (reader->handlesChunkId(chunkId)) {
      return reader.get();
    }
  }
  return nullptr;
}

AVExtractor::Result AVAviExtractor::readMoviChunks(AVExtractorInput& input) {
  if (moviEnd_ >= 0 && input.getPosition() >= moviEnd_) {
    // Every chunk of the list was read, which is where the container ends for this extractor: the
    // index of the samples follows the list.
    return finish(input);
  }
  if (currentChunkReader_ != nullptr) {
    if (currentChunkReader_->onChunkData(input)) {
      currentChunkReader_ = nullptr;
    }
    position_ = input.getPosition();
    return kResultContinue;
  }

  // The chunks of a RIFF file are aligned on even positions, and their padding is not counted by
  // their size.
  if ((input.getPosition() & 1) == 1 && !input.skipFully(1)) {
    return finish(input);
  }

  uint8_t scratch[12];
  input.resetPeekPosition();
  const long peeked = input.peek(scratch, 0, sizeof(scratch));
  if (peeked < 8) {
    input.resetPeekPosition();
    return finish(input);
  }
  const uint32_t chunkType = readLittleEndianUnsignedInt(scratch, 0);
  const uint32_t size = readLittleEndianUnsignedInt(scratch, 4);
  const uint32_t listType = peeked >= 12 ? readLittleEndianUnsignedInt(scratch, 8) : 0;
  input.resetPeekPosition();

  if (chunkType == kFourCcRiff) {
    // An OpenDML file continues in further RIFF chunks after the first movi list.
    if (!input.skipFully(12)) {
      return finish(input);
    }
    position_ = input.getPosition();
    return kResultContinue;
  }
  if (chunkType == kFourCcList) {
    // A nested list, e.g. the {@code rec} list of a file that groups its samples by stream, is
    // entered so that its chunks are read as well.
    if (peeked < 12 || !input.skipFully(listType == kFourCcMovi ? 12 : 8)) {
      return finish(input);
    }
    position_ = input.getPosition();
    return kResultContinue;
  }
  if (!input.skipFully(8)) {
    return finish(input);
  }
  if (chunkType == kFourCcJunk) {
    if (!input.skipFully(static_cast<long>(size) + static_cast<long>(size & 1))) {
      return finish(input);
    }
    position_ = input.getPosition();
    return kResultContinue;
  }

  AVChunkReader* const reader = findChunkReader(chunkType);
  if (reader == nullptr) {
    // The chunk belongs to a stream that this extractor does not expose, it is skipped.
    if (!input.skipFully(static_cast<long>(size) + static_cast<long>(size & 1))) {
      return finish(input);
    }
    position_ = input.getPosition();
    return kResultContinue;
  }
  // The size of a chunk is only trusted once it was validated against the container.
  const long position = input.getPosition();
  if (size > kMaxChunkSize || (moviEnd_ >= 0 && position + static_cast<long>(size) > moviEnd_)) {
    return finish(input);
  }

  // A video chunk whose identifier is the uncompressed one is a key frame, which is the only
  // description of a key frame that a container without an index carries. When the index is
  // available it also tells the key frames and the timestamps of the samples.
  bool indexedKeyFrame = false;
  int64_t indexedTimeUs = -1;
  const auto indexed = indexedChunkByPosition_.find(position - 8);
  if (indexed != indexedChunkByPosition_.end()) {
    indexedKeyFrame = indexed->second.keyFrame;
    indexedTimeUs = indexed->second.timeUs;
  }
  reader->onChunkStart(size, indexedKeyFrame || reader->isUncompressedChunkId(chunkType),
    indexedTimeUs);
  currentChunkReader_ = reader;
  if (currentChunkReader_->onChunkData(input)) {
    currentChunkReader_ = nullptr;
  }
  position_ = input.getPosition();
  return kResultContinue;
}

AVExtractor::Result AVAviExtractor::finish(AVExtractorInput& input) {
  state_ = State::kEndOfInput;
  currentChunkReader_ = nullptr;
  if (!trackEnded_) {
    for (auto& reader : chunkReaders_) {
      reader->endTrack();
    }
    trackEnded_ = true;
  }
  position_ = input.getPosition();
  return kResultEndOfInput;
}

bool AVAviExtractor::hasDeliveredSample() const {
  for (const auto& reader : chunkReaders_) {
    if (!reader->isSkipping()) {
      // The reader delivered the first chunk at or after the target of the seek.
      return true;
    }
  }
  return false;
}

} // namespace coral

