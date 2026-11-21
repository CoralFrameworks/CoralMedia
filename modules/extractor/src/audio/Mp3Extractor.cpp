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

#include <extractor/audio/Mp3Extractor.h>

#include <extractor/metadata/Id3Parser.h>

#include <algorithm>
#include <string>

namespace coral {

namespace {

/** The number of bytes of an ID3v2 header. */
constexpr int kId3HeaderSize = 10;

/** The identifier of the ID3v2 tags. */
constexpr uint32_t kId3 = 0x494433;  // "ID3"

/** The identifier of the Xing header. */
constexpr uint32_t kXing = 0x58696E67;  // "Xing"

/** The identifier of the Info header, which is a Xing header of a constant bitrate stream. */
constexpr uint32_t kInfo = 0x496E666F;  // "Info"

/** The identifier of the VBRI header. */
constexpr uint32_t kVbri = 0x56425249;  // "VBRI"

/** The offset of the VBRI header in the frame, in bytes. */
constexpr size_t kVbriOffset = 32;

/**
 * Returns whether the given byte pair may start an MPEG audio frame.
 */
bool isFrameSync(const uint8_t first, const uint8_t second) {
  return first == 0xFF && (second & 0xE0) == 0xE0;
}

} // namespace anonymous

// ==================================================================================================
// Sniffing
// ==================================================================================================

int AVMp3Extractor::sniff(AVExtractorInput& input) {
  sniffFailureReason_.clear();
  input.resetPeekPosition();

  uint8_t scratch[4];
  const long peeked = input.peek(scratch, 0, sizeof(scratch));
  input.resetPeekPosition();
  if (peeked < 2) {
    sniffFailureReason_ = "The input is too short.";
    return AVExtractorsFactory::kSniffResultNone;
  }

  if (peeked >= 3 && AVUtil::readUnsignedInt24(scratch, 0) == kId3) {
    // An ID3v2 tag, the payload decides between MP3 and ADTS.
    return AVExtractorsFactory::kSniffResultPossible;
  }
  if (isFrameSync(scratch[0], scratch[1])) {
    AVMpegAudioHeader header;
    if (peeked >= 4 && header.setForHeaderData(AVUtil::readUnsignedInt(scratch, 0))) {
      return AVExtractorsFactory::kSniffResultCertain;
    }
    return AVExtractorsFactory::kSniffResultPossible;
  }

  // A stream may start with junk, search for a frame within the first kilobytes.
  AVDataBuffer probe(4096);
  const long probeLength = input.peek(probe.array(), 0, 4096);
  input.resetPeekPosition();
  if (probeLength <= 0) {
    sniffFailureReason_ = "The input is empty.";
    return AVExtractorsFactory::kSniffResultNone;
  }
  const auto* const data = probe.array();
  for (long i = 0; i + 4 <= probeLength; ++i) {
    if (isFrameSync(data[i], data[i + 1])) {
      AVMpegAudioHeader header;
      if (header.setForHeaderData(AVUtil::readUnsignedInt(data, static_cast<int>(i)))) {
        sniffFailureReason_ = "An MPEG audio frame was found after the start of the input.";
        return AVExtractorsFactory::kSniffResultPossible;
      }
    }
  }
  sniffFailureReason_ = "No MPEG audio frame header was found.";
  return AVExtractorsFactory::kSniffResultNone;
}

// ==================================================================================================
// Setup
// ==================================================================================================

void AVMp3Extractor::init(AVExtractorOutput& output) {
  trackOutput_ = output.track(0, MediaType::kMediaTypeAudio, AVTrackOutput::emptyConsumer());
  metadataOutput_ = output.track(1, MediaType::kMediaTypeMetadata, AVTrackOutput::emptyConsumer());
  auto metadataFormat = MediaFormat::CreateSubtitleFormat(MimeTypes::APPLICATION_ID3);
  metadataFormat->setContainerMimeType(MimeTypes::AUDIO_MPEG);
  metadataFormat->setTrackId(1);
  metadataFormat->setSubsampleOffsetUs(0);
  metadataOutput_->format(metadataFormat);
  scratch_.reset(static_cast<uint32_t>(AVMpegAudioHeader::kMaxFrameSize));
  seekMapOutput_ = &output;
  output.seekMap(std::make_shared<AVUnseekableSeekMap>(Definitions::kTimeUnset));
  output.endTracks();
  tracksEnded_ = true;
}

// ==================================================================================================
// ID3
// ==================================================================================================

bool AVMp3Extractor::readId3Headers(AVExtractorInput& input) {
  const long readStartPosition = input.getPosition();
  input.resetPeekPosition();
  long position = readStartPosition;
  const long length = input.getLength();

  while (true) {
    if (length >= 0 && position + kId3HeaderSize > length) {
      break;
    }
    uint8_t scratch[kId3HeaderSize];
    input.resetPeekPosition();
    input.setPeekPosition(position - readStartPosition);
    if (input.peek(scratch, 0, sizeof(scratch)) < static_cast<long>(sizeof(scratch))) {
      break;
    }
    if (AVUtil::readUnsignedInt24(scratch, 0) != kId3) {
      // There is no tag at this position, this is where the audio starts.
      break;
    }
    const int majorVersion = scratch[3];
    if (majorVersion < 2 || majorVersion > 4) {
      break;
    }
    const int flags = scratch[5];
    // The tag size is a 28 bit synchsafe integer.
    const int tagSize = ((scratch[6] & 0x7F) << 21) | ((scratch[7] & 0x7F) << 14) |
                        ((scratch[8] & 0x7F) << 7) | (scratch[9] & 0x7F);
    long totalSize = kId3HeaderSize + tagSize;
    if (majorVersion >= 4 && (flags & 0x10) != 0) {
      // The tag is followed by a footer of the same size as the header.
      totalSize += kId3HeaderSize;
    }
    if (majorVersion == 3 && (flags & 0x20) != 0) {
      // The tag is compressed, which this extractor does not support.
      break;
    }
    if (tagSize <= 0 || totalSize > 256L * 1024 * 1024) {
      break;
    }
    position += totalSize;
  }

  streamStartPosition_ = position;
  input.resetPeekPosition();
  if (position == readStartPosition) {
    return true;
  }
  const long tagBytes = position - readStartPosition;
  if (tagBytes > 0 && tagBytes <= 4L * 1024 * 1024) {
    // The tags are kept so that the format of the track states the title and the artist that they
    // describe, and so that the samples of an ID3 metadata track can be built from them.
    std::vector<uint8_t> tags(static_cast<size_t>(tagBytes));
    if (!input.readFully(tags.data(), 0, tags.size())) {
      return false;
    }
    metadata_ = std::make_shared<Metadata>(std::vector<std::shared_ptr<Metadata::Entry>>{});
    if (AVId3Parser::parseAll(tags.data(), tags.size(), metadata_) == 0) {
      metadata_.reset();
    } else {
      id3Tags_ = std::move(tags);
      auto buffer = std::make_shared<AVBlockBuffer>(static_cast<uint32_t>(id3Tags_.size()));
      buffer->put(id3Tags_.data(), id3Tags_.size());
      buffer->position(0);
      buffer->limit(id3Tags_.size());
      AVSample sample(1, 0, static_cast<uint32_t>(AVSample::kFlagKeyFrame), std::move(buffer));
      metadataOutput_->sample(sample);
    }
  }
  return input.getPosition() == position || input.setPosition(position, true);
}

// ==================================================================================================
// Frame search
// ==================================================================================================

bool AVMp3Extractor::readMidStreamId3(AVExtractorInput& input, const int64_t timeUs) {
  uint8_t header[kId3HeaderSize];
  if (input.peek(header, 0, sizeof(header)) != static_cast<long>(sizeof(header)) ||
      AVUtil::readUnsignedInt24(header, 0) != kId3) {
    return false;
  }
  const int flags = header[5];
  const int tagSize = ((header[6] & 0x7F) << 21) | ((header[7] & 0x7F) << 14) |
                      ((header[8] & 0x7F) << 7) | (header[9] & 0x7F);
  const long totalSize = kId3HeaderSize + tagSize +
                         (header[3] >= 4 && (flags & 0x10) != 0 ? kId3HeaderSize : 0);
  if (tagSize <= 0 || totalSize > 4L * 1024 * 1024) {
    return false;
  }
  std::vector<uint8_t> tags(static_cast<size_t>(totalSize));
  if (!input.readFully(tags.data(), 0, tags.size())) {
    return false;
  }
  auto buffer = std::make_shared<AVBlockBuffer>(static_cast<uint32_t>(tags.size()));
  buffer->put(tags.data(), tags.size());
  buffer->position(0);
  buffer->limit(tags.size());
  AVSample sample(1, timeUs, static_cast<uint32_t>(AVSample::kFlagKeyFrame), std::move(buffer));
  metadataOutput_->sample(sample);
  return true;
}

bool AVMp3Extractor::findNextFrame(AVExtractorInput& input, Frame& frame) {
  input.resetPeekPosition();
  while (true) {
    const long candidateOffset = input.getPeekPositionOffset();
    const int first = input.peekByte();
    if (first == kEndOfInput) {
      return false;
    }
    const int second = input.peekByte();
    if (second == kEndOfInput) {
      return false;
    }
    if (isFrameSync(static_cast<uint8_t>(first), static_cast<uint8_t>(second))) {
      // Rewind the peek position to the beginning of the candidate frame.
      input.setPeekPosition(candidateOffset);
      uint8_t scratch[4];
      if (input.peek(scratch, 0, sizeof(scratch)) == static_cast<long>(sizeof(scratch))) {
        AVMpegAudioHeader candidate;
        if (candidate.setForHeaderData(AVUtil::readUnsignedInt(scratch, 0)) &&
            candidate.getFrameSize() > 4) {
          frame.header = candidate;
          frame.frameSize = candidate.getFrameSize();
          frame.startPosition = input.getPosition() + candidateOffset;
          // Leave the read and the peek positions at the start of the frame.
          input.resetPeekPosition();
          return true;
        }
      }
      // The sync word was not a valid header, continue after it.
      input.setPeekPosition(candidateOffset + 1);
      continue;
    }
    // The sync word was not found, continue with the next byte.
    input.setPeekPosition(candidateOffset + 1);
  }
}

// ==================================================================================================
// VBR headers
// ==================================================================================================

bool AVMp3Extractor::readXingHeader(const AVDataBuffer& frameData, const Frame& frame,
                                    const size_t offset) {
  const size_t available = frameData.limit();
  if (offset + 8 > available) {
    return false;
  }
  const uint8_t* const data = frameData.array();
  const uint32_t identifier = AVUtil::readUnsignedInt(data, static_cast<int>(offset));
  if (identifier != kXing && identifier != kInfo) {
    return false;
  }

  const uint32_t flags = AVUtil::readUnsignedInt(data, static_cast<int>(offset) + 4);
  size_t cursor = offset + 8;
  if ((flags & 0x1) != 0) {
    if (cursor + 4 > available) {
      return false;
    }
    seeker_.frameCount = static_cast<int>(AVUtil::readUnsignedInt(data, static_cast<int>(cursor)));
    cursor += 4;
  }
  if ((flags & 0x2) != 0) {
    if (cursor + 4 > available) {
      return false;
    }
    seeker_.byteCount = static_cast<long>(AVUtil::readUnsignedInt(data, static_cast<int>(cursor)));
    cursor += 4;
  }
  seeker_.present = true;
  if (seeker_.frameCount > 0) {
    seeker_.durationUs = frameTimeUs(seeker_.frameCount);
    if (seeker_.durationUs > 0 && seeker_.byteCount > 0) {
      seeker_.averageBitrate =
        static_cast<int>(seeker_.byteCount * 8 * AVUtil::kMicrosPerSecond / seeker_.durationUs);
    }
  }
  return true;
}

bool AVMp3Extractor::readVbriHeader(const AVDataBuffer& frameData, const Frame& frame) {
  const size_t available = frameData.limit();
  if (kVbriOffset + 26 > available) {
    return false;
  }
  const uint8_t* const data = frameData.array();
  if (AVUtil::readUnsignedInt(data, static_cast<int>(kVbriOffset)) != kVbri) {
    return false;
  }
  seeker_.byteCount =
    static_cast<long>(AVUtil::readUnsignedInt(data, static_cast<int>(kVbriOffset) + 10));
  seeker_.frameCount =
    static_cast<int>(AVUtil::readUnsignedInt(data, static_cast<int>(kVbriOffset) + 14));
  seeker_.present = true;
  if (seeker_.frameCount > 0) {
    seeker_.durationUs = frameTimeUs(seeker_.frameCount);
    if (seeker_.durationUs > 0 && seeker_.byteCount > 0) {
      seeker_.averageBitrate =
        static_cast<int>(seeker_.byteCount * 8 * AVUtil::kMicrosPerSecond / seeker_.durationUs);
    }
  }
  static_cast<void>(frame);
  return true;
}

bool AVMp3Extractor::readVbrSeeker(const AVDataBuffer& frameData, const Frame& frame) {
  // The side information of the frame precedes the Xing header.
  const int sideDataSize = frame.header.version == AVMpegAudioHeader::Version::kVersion1
                             ? (frame.header.channels == 1 ? 17 : 32)
                             : (frame.header.channels == 1 ? 9 : 17);
  const size_t xingOffset = 4 + static_cast<size_t>(sideDataSize);
  if (xingOffset < frameData.limit() && readXingHeader(frameData, frame, xingOffset)) {
    return true;
  }
  if (readVbriHeader(frameData, frame)) {
    return true;
  }
  // Some encoders place the header right after the frame header, without side information.
  return readXingHeader(frameData, frame, 4);
}

// ==================================================================================================
// Timestamps and seek map
// ==================================================================================================

int64_t AVMp3Extractor::frameTimeUs(const int64_t frameIndex) const {
  if (header_.sampleRate <= 0) {
    return Definitions::kTimeUnset;
  }
  return frameIndex * header_.getSamplesPerFrame() * AVUtil::kMicrosPerSecond / header_.sampleRate;
}

int64_t AVMp3Extractor::constantBitrateTimeUs(const long framePosition) const {
  if (averageBitrate_ <= 0) {
    return Definitions::kTimeUnset;
  }
  return (framePosition - firstFramePosition_) * 8 * AVUtil::kMicrosPerSecond / averageBitrate_;
}

void AVMp3Extractor::buildSeekMap() {
  seekPoints_.clear();
  if (durationUs_ == Definitions::kTimeUnset && averageBitrate_ > 0 && firstFramePosition_ >= 0 &&
      dataEndPosition_ > firstFramePosition_) {
    durationUs_ = (dataEndPosition_ - firstFramePosition_) * 8 * AVUtil::kMicrosPerSecond /
                  averageBitrate_;
  }
  if (seeker_.present && seeker_.durationUs > 0) {
    // A variable bitrate stream is seekable by interpolation on its average bitrate.
    const auto seekPointCount = static_cast<int>(seeker_.durationUs / (10 * AVUtil::kMicrosPerSecond));
    for (int i = 0; i <= seekPointCount; ++i) {
      AVSeekMap::SeekPoint point;
      point.timeUs = static_cast<int64_t>(i) * 10 * AVUtil::kMicrosPerSecond;
      point.position =
        firstFramePosition_ + point.timeUs * averageBitrate_ / 8 / AVUtil::kMicrosPerSecond;
      seekPoints_.push_back(point);
    }
  } else if (durationUs_ != Definitions::kTimeUnset && averageBitrate_ > 0 &&
             firstFramePosition_ >= 0) {
    // A constant bitrate stream is seekable by interpolation.
    const auto seekPointCount = static_cast<int>(durationUs_ / (10 * AVUtil::kMicrosPerSecond));
    for (int i = 0; i <= seekPointCount; ++i) {
      AVSeekMap::SeekPoint point;
      point.timeUs = static_cast<int64_t>(i) * 10 * AVUtil::kMicrosPerSecond;
      point.position =
        firstFramePosition_ + point.timeUs * averageBitrate_ / 8 / AVUtil::kMicrosPerSecond;
      seekPoints_.push_back(point);
    }
  }
  if (seekPoints_.size() < 2) {
    seekPoints_.clear();
  }
  if (seekMapOutput_ == nullptr) {
    return;
  }
  if (seekPoints_.empty()) {
    seekMapOutput_->seekMap(std::make_shared<AVUnseekableSeekMap>(durationUs_));
  } else {
    seekMapOutput_->seekMap(std::make_shared<AVIndexSeekMap>(seekPoints_, durationUs_, true));
  }
}

// ==================================================================================================
// Reading
// ==================================================================================================

std::shared_ptr<MediaFormat> AVMp3Extractor::buildFormat() const {
  auto format =
    MediaFormat::CreateAudioFormat(header_.getMimeType(), header_.sampleRate, header_.channels);
  format->setContainerMimeType(MimeTypes::AUDIO_MPEG);
  format->setMaxInputSize(header_.getFrameSize());
  format->setDurationUs(durationUs_);
  if (averageBitrate_ > 0) {
    format->setAverageBitrate(averageBitrate_);
  }
  format->setSampleFormat(AudioSampleFormat::kInt16);
  if (metadata_ != nullptr) {
    format->setMetadata(metadata_);
  }
  return format;
}

AVExtractor::Result AVMp3Extractor::read(AVExtractorInput& input) {
  if (!id3Read_) {
    if (!readId3Headers(input)) {
      position_ = input.getPosition();
      return kResultEndOfInput;
    }
    id3Read_ = true;
  }
  if (!tracksEnded_) {
    seekMapOutput_->endTracks();
    tracksEnded_ = true;
  }
  while (readMidStreamId3(input, frameTimeUs(frameIndex_))) {
  }

  Frame frame;
  if (!findNextFrame(input, frame)) {
    position_ = input.getPosition();
    if (!trackEnded_ && formatPublished_) {
      trackOutput_->endTrack();
      if (metadataOutput_ != nullptr) {
        metadataOutput_->endTrack();
      }
      trackEnded_ = true;
    }
    return kResultEndOfInput;
  }

  // Move the read position to the beginning of the frame that was found.
  if (!input.setPosition(frame.startPosition, true)) {
    position_ = input.getPosition();
    return kResultEndOfInput;
  }

  const long read = input.read(scratch_, static_cast<size_t>(frame.frameSize));
  position_ = input.getPosition();
  if (read == kEndOfInput || read < 4) {
    return kResultEndOfInput;
  }
  if (!header_.setForHeaderData(AVUtil::readUnsignedInt(scratch_.array(), 0))) {
    // The frame is damaged, resynchronize from the next byte.
    if (!input.setPosition(frame.startPosition + 1, true)) {
      return kResultEndOfInput;
    }
    position_ = input.getPosition();
    return kResultContinue;
  }

  const bool firstFrame = firstFramePosition_ < 0;
  if (firstFrame) {
    firstFramePosition_ = frame.startPosition;
    if (readVbrSeeker(scratch_, frame)) {
      // A variable bitrate stream is described by its header, and is seeked by interpolation.
      constBitrate_ = false;
      if (seeker_.averageBitrate > 0) {
        averageBitrate_ = seeker_.averageBitrate;
      }
    } else {
      // Without a VBR header the stream is assumed to be of a constant bitrate.
      constBitrate_ = true;
      averageBitrate_ = header_.bitrate;
    }
    const long length = input.getLength();
    if (length >= 0) {
      dataEndPosition_ = length;
      if (durationUs_ == Definitions::kTimeUnset && averageBitrate_ > 0) {
        durationUs_ =
          (length - firstFramePosition_) * 8 * AVUtil::kMicrosPerSecond / averageBitrate_;
      }
    }
    buildSeekMap();
  }

  if (!formatPublished_) {
    trackOutput_->format(buildFormat());
    formatPublished_ = true;
  }

  const int64_t timeUs = constBitrate_ ? constantBitrateTimeUs(frame.startPosition)
                                       : frameTimeUs(frameIndex_);

  auto buffer = std::make_shared<AVBlockBuffer>(static_cast<uint32_t>(read));
  buffer->put(scratch_.array(), static_cast<size_t>(read));
  buffer->position(0);
  buffer->limit(static_cast<size_t>(read));

  uint32_t flags = AVSample::kFlagKeyFrame;
  if (firstFrame) {
    flags |= AVSample::kFlagFirstSample;
  }
  const AVSample sample(0, timeUs, flags, std::move(buffer));
  trackOutput_->sample(sample);

  ++frameIndex_;
  return kResultContinue;
}

bool AVMp3Extractor::seek(long position, int64_t timeUs) {
  static_cast<void>(position);
  static_cast<void>(timeUs);
  // The extractor is stateless with respect to the position, the reader sets the input position.
  return true;
}

} // namespace coral
