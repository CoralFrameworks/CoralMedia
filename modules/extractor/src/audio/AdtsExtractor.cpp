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

#include <extractor/audio/AdtsExtractor.h>

#include <extractor/metadata/Id3Parser.h>

#include <algorithm>
#include <string>
#include <vector>

namespace coral {

namespace {

/** The identifier of the ID3v2 tags. */
constexpr uint32_t kId3 = 0x494433;  // "ID3"

/** The number of bytes of an ID3v2 header. */
constexpr int kId3HeaderSize = 10;

/** The maximum size of an ADTS frame, in bytes. */
constexpr int kMaxFrameSize = 8191;

/**
 * Builds the two bytes of the AudioSpecificConfig of an AAC stream.
 *
 * <p>@param profile The profile of the ADTS header, which the specification defines as the audio
 * object type minus one.
 */
std::vector<uint8_t> buildAudioSpecificConfig(const int profile, const int samplingFrequencyIndex,
                                              const int channelConfig) {
  // The record is five bits of audio object type, four bits of sampling frequency index, four bits
  // of channel configuration, then three bits that are zero.
  const auto audioObjectType = static_cast<uint32_t>(profile + 1);
  const uint32_t value = (audioObjectType << 11) |
                         (static_cast<uint32_t>(samplingFrequencyIndex) << 7) |
                         (static_cast<uint32_t>(channelConfig) << 3);
  std::vector<uint8_t> config(2);
  config[0] = static_cast<uint8_t>((value >> 8) & 0xFF);
  config[1] = static_cast<uint8_t>(value & 0xFF);
  return config;
}
} // namespace anonymous

bool AVAdtsExtractor::skipId3Headers(AVExtractorInput& input) {
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
    if (position > readStartPosition) {
      input.skipPeeked(position - readStartPosition);
    }
    if (input.peek(scratch, 0, sizeof(scratch)) < static_cast<long>(sizeof(scratch))) {
      break;
    }
    if (AVUtil::readUnsignedInt24(scratch, 0) != kId3) {
      break;
    }
    const int flags = scratch[5];
    const int tagSize = ((scratch[6] & 0x7F) << 21) | ((scratch[7] & 0x7F) << 14) |
                        ((scratch[8] & 0x7F) << 7) | (scratch[9] & 0x7F);
    if (tagSize <= 0) {
      break;
    }
    long totalSize = kId3HeaderSize + tagSize;
    if (scratch[3] >= 4 && (flags & 0x10) != 0) {
      totalSize += kId3HeaderSize;
    }
    if (totalSize > 256L * 1024 * 1024) {
      break;
    }
    position += totalSize;
  }

  input.resetPeekPosition();
  const long tagBytes = position - readStartPosition;
  if (tagBytes > 0 && tagBytes <= 4L * 1024 * 1024) {
    // The tags are kept so that the format of the track states the title and the artist that they
    // describe. The tag may precede an MP3 stream: the MP3 extractor wins the sniff, and this code is
    // only reached when the stream is really ADTS.
    if (!input.setPosition(readStartPosition, true)) {
      return false;
    }
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
  if (position != readStartPosition && input.getPosition() != position &&
      !input.setPosition(position, true)) {
    return false;
  }
  return true;
}

bool AVAdtsExtractor::readMidStreamId3(AVExtractorInput& input, const int64_t timeUs) {
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

bool AVAdtsExtractor::findNextFrame(AVExtractorInput& input, AVAdtsHeader& header,
                                    long& framePosition) {
  input.resetPeekPosition();
  while (true) {
    const int first = input.peekByte();
    if (first == kEndOfInput) {
      return false;
    }
    const int second = input.peekByte();
    if (second == kEndOfInput) {
      return false;
    }
    // The peek position is now two bytes after the start of the candidate frame.
    const long candidateOffset = input.getPeekPositionOffset() - 2;
    if (first == 0xFF && (second & 0xF6) == 0xF0) {
      input.setPeekPosition(candidateOffset);
      uint8_t scratch[AVAdtsHeader::kHeaderSize];
      if (input.peek(scratch, 0, sizeof(scratch)) == static_cast<long>(sizeof(scratch))) {
        AVAdtsHeader candidate;
        if (candidate.setForHeaderData(scratch, 0)) {
          header = candidate;
          framePosition = input.getPosition() + candidateOffset;
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

int AVAdtsExtractor::sniff(AVExtractorInput& input) {
  sniffFailureReason_.clear();
  input.resetPeekPosition();

  uint8_t scratch[AVAdtsHeader::kHeaderSize];
  const long peeked = input.peek(scratch, 0, sizeof(scratch));
  input.resetPeekPosition();
  if (peeked < 2) {
    sniffFailureReason_ = "The input is too short.";
    return AVExtractorsFactory::kSniffResultNone;
  }

  if (peeked >= 3 && AVUtil::readUnsignedInt24(scratch, 0) == kId3) {
    // An ID3 tag may precede an ADTS stream, but it may also precede an MP3 stream. The MP3
    // extractor reports the same confidence and is tried first when no frame follows.
    return AVExtractorsFactory::kSniffResultPossible;
  }
  if (peeked >= static_cast<long>(sizeof(scratch))) {
    AVAdtsHeader header;
    if (header.setForHeaderData(scratch, 0)) {
      // Two valid and consistent frames in a row make the detection certain.
      return AVExtractorsFactory::kSniffResultCertain;
    }
  }
  if (peeked >= 2 && scratch[0] == 0xFF && (scratch[1] & 0xF0) == 0xF0) {
    return AVExtractorsFactory::kSniffResultPossible;
  }
  sniffFailureReason_ = "No ADTS frame header was found.";
  return AVExtractorsFactory::kSniffResultNone;
}

void AVAdtsExtractor::init(AVExtractorOutput& output) {
  trackOutput_ = output.track(0, MediaType::kMediaTypeAudio, AVTrackOutput::emptyConsumer());
  metadataOutput_ = output.track(1, MediaType::kMediaTypeMetadata, AVTrackOutput::emptyConsumer());
  auto metadataFormat = MediaFormat::CreateSubtitleFormat(MimeTypes::APPLICATION_ID3);
  metadataFormat->setContainerMimeType(MimeTypes::AUDIO_AAC);
  metadataFormat->setTrackId(1);
  metadataFormat->setSubsampleOffsetUs(0);
  metadataOutput_->format(metadataFormat);
  scratch_.reset(static_cast<uint32_t>(kMaxFrameSize));
  output_ = &output;
  output.seekMap(std::make_shared<AVUnseekableSeekMap>(Definitions::kTimeUnset));
  output.endTracks();
  tracksEnded_ = true;
}

std::shared_ptr<MediaFormat> AVAdtsExtractor::buildFormat(const AVAdtsHeader& header) const {
  const int channelCount = header.channelConfig;
  auto format = MediaFormat::CreateAudioFormat(MimeTypes::AUDIO_AAC, header.getSampleRate(),
                                               channelCount);
  format->setContainerMimeType(MimeTypes::AUDIO_AAC);
  format->setMaxInputSize(header.getFrameLength());
  format->setDurationUs(durationUs_);
  format->setSampleFormat(AudioSampleFormat::kInt16);
  const auto config =
    buildAudioSpecificConfig(header.profile, header.getSamplingFrequencyIndex(), channelCount);
  if (!config.empty()) {
    format->setCodecSpecificData(0, config.data(), config.size());
  }
  if (metadata_ != nullptr) {
    format->setMetadata(metadata_);
  }
  return format;
}

AVExtractor::Result AVAdtsExtractor::read(AVExtractorInput& input) {
  if (!id3Skipped_) {
    if (!skipId3Headers(input)) {
      position_ = input.getPosition();
      return kResultEndOfInput;
    }
    id3Skipped_ = true;
  }
  if (!tracksEnded_) {
    output_->endTracks();
    tracksEnded_ = true;
  }
  while (readMidStreamId3(input,
           sampleRate_ > 0 ? sampleCount_ * 1024 * AVUtil::kMicrosPerSecond / sampleRate_ : 0)) {
  }
  // The extractor reports the position of the input, so that the reader always observes progress,
  // even when the current read only skipped bytes.
  position_ = input.getPosition();

  AVAdtsHeader header;
  long framePosition = -1;
  if (!findNextFrame(input, header, framePosition)) {
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

  if (framePosition != input.getPosition() && !input.setPosition(framePosition, true)) {
    position_ = input.getPosition();
    return kResultEndOfInput;
  }

  const long read = input.read(scratch_, static_cast<size_t>(header.getFrameLength()));
  if (read == kEndOfInput) {
    position_ = input.getPosition();
    return kResultEndOfInput;
  }
  position_ = input.getPosition();
  if (read < AVAdtsHeader::kHeaderSize) {
    // The frame was truncated, there is nothing left to read.
    return kResultEndOfInput;
  }

  AVAdtsHeader actual;
  if (!actual.setForHeaderData(scratch_.array(), 0)) {
    // The frame is damaged, resynchronize from the next byte.
    if (!input.setPosition(framePosition + 1, true)) {
      position_ = input.getPosition();
      return kResultEndOfInput;
    }
    position_ = input.getPosition();
    return kResultContinue;
  }

  if (!formatPublished_) {
    sampleRate_ = actual.getSampleRate();
    channelCount_ = actual.channelConfig;
    trackOutput_->format(buildFormat(actual));
    formatPublished_ = true;
  }

  const int64_t timeUs = sampleRate_ > 0
                           ? sampleCount_ * AVUtil::kMicrosPerSecond / sampleRate_
                           : Definitions::kTimeUnset;
  sampleCount_ += actual.getSamplesPerFrame();

  if (read > 0) {
    auto buffer = std::make_shared<AVBlockBuffer>(static_cast<uint32_t>(read));
    buffer->put(scratch_.array(), static_cast<size_t>(read));
    buffer->position(0);
    buffer->limit(static_cast<size_t>(read));
    AVSample sample(0, timeUs,
                    AVSample::kFlagKeyFrame | (timeUs == 0 ? AVSample::kFlagFirstSample
                                                          : AVSample::kFlagNone),
                    std::move(buffer));
    trackOutput_->sample(sample);
  }

  return kResultContinue;
}

bool AVAdtsExtractor::seek(long position, int64_t timeUs) {
  static_cast<void>(position);
  static_cast<void>(timeUs);
  // The extractor is stateless with respect to the position, the reader sets the input position.
  return true;
}

} // namespace coral
