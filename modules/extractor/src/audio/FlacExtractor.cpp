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

#include <extractor/audio/FlacExtractor.h>

#include <extractor/metadata/VorbisCommentParser.h>

#include <media/Utilities.h>

#include <algorithm>
#include <string>
#include <vector>

namespace coral {

namespace {

/** The identifier of a native FLAC stream. */
constexpr uint32_t kFlac = 0x664C6143;  // "fLaC"

/** The identifier of a FLAC frame that is embedded in an Ogg or a MP4 container. */
constexpr uint32_t kFlacSyncCode = 0x664C6143;

/** The size of the STREAMINFO block, in bytes. */
constexpr int kStreamInfoSize = 34;

/** The maximum size of a metadata block that this extractor buffers. */
constexpr int kMaxMetadataBlockSize = 1024 * 1024;

/** The maximum size of a FLAC frame, in bytes. */
constexpr int kMaxFrameSize = 16 * 1024 * 1024;

/** The number of bytes read from the stream when the frame size is not bounded. */
constexpr int kDefaultFrameReadSize = 64 * 1024;

/** The metadata block type of the STREAMINFO block. */
constexpr int kBlockTypeStreamInfo = 0;

/** The metadata block type of the Vorbis comment block. */
constexpr int kBlockTypeVorbisComment = 4;

/** The metadata block type of the cue sheet block. */
constexpr int kBlockTypeCuesheet = 5;

/** The metadata block type of the picture block. */
constexpr int kBlockTypePicture = 6;

/**
 * The sample rate table of the FLAC frame header, indexed by the sample rate code.
 * A zero means that the sample rate is described elsewhere.
 */
constexpr int kSampleRates[] = {0,     88200, 176400, 192000, 8000,  16000, 22050, 24000,
                                32000, 44100, 48000,  96000,  0,     0,     0,     0};

/**
 * The number of samples table of the FLAC frame header, indexed by the block size code.
 * A zero means that the block size is described elsewhere.
 */
constexpr int kBlockSizes[] = {0, 192, 576, 1152, 2304, 4608, 0, 0, 256, 512, 1024, 2048, 4096,
                               8192, 16384, 32768};

/**
 * The number of bits per sample table of the FLAC frame header, indexed by the size code.
 */
constexpr int kBitsPerSample[] = {0, 8, 12, 0, 16, 20, 24, 0};

} // namespace anonymous

// ==================================================================================================
// Sniffing
// ==================================================================================================

int AVFlacExtractor::sniff(AVExtractorInput& input) {
  sniffFailureReason_.clear();
  input.resetPeekPosition();

  uint8_t scratch[4];
  const long peeked = input.peek(scratch, 0, sizeof(scratch));
  input.resetPeekPosition();
  if (peeked < 4) {
    sniffFailureReason_ = "The input is too short.";
    return AVExtractorsFactory::kSniffResultNone;
  }
  if (AVUtil::readUnsignedInt(scratch, 0) == kFlac) {
    return AVExtractorsFactory::kSniffResultCertain;
  }
  sniffFailureReason_ = "The fLaC marker was not found.";
  return AVExtractorsFactory::kSniffResultNone;
}

// ==================================================================================================
// Metadata
// ==================================================================================================

void AVFlacExtractor::parseStreamInfo(const uint8_t data[], const size_t size) {
  if (size < kStreamInfoSize) {
    return;
  }
  streamInfo_.minBlockSize = AVUtil::readUnsignedShort(data, 0);
  streamInfo_.maxBlockSize = AVUtil::readUnsignedShort(data, 2);
  streamInfo_.minFrameSize = AVUtil::readUnsignedInt24(data, 4);
  streamInfo_.maxFrameSize = AVUtil::readUnsignedInt24(data, 7);

  // The next eight bytes hold the sample rate (20 bits), the number of channels (3 bits), the
  // number of bits per sample (5 bits) and the total number of samples (36 bits).
  const uint64_t packed = (static_cast<uint64_t>(AVUtil::readUnsignedInt(data, 10)) << 32) |
                          AVUtil::readUnsignedInt(data, 14);
  streamInfo_.sampleRate = static_cast<int>((packed >> 44) & 0xFFFFF);
  streamInfo_.channelCount = static_cast<int>(((packed >> 41) & 0x7) + 1);
  streamInfo_.bitsPerSample = static_cast<int>(((packed >> 36) & 0x1F) + 1);
  streamInfo_.totalSamples = static_cast<int64_t>(packed & 0xFFFFFFFFFLL);

  if (streamInfo_.sampleRate > 0 && streamInfo_.totalSamples > 0) {
    durationUs_ = streamInfo_.totalSamples * AVUtil::kMicrosPerSecond / streamInfo_.sampleRate;
  }
}

bool AVFlacExtractor::readMetadata(AVExtractorInput& input) {
  if (!input.readFully(scratch_, 4)) {
    return false;
  }
  if (AVUtil::readUnsignedInt(scratch_.array(), 0) != kFlac) {
    return false;
  }

  bool lastBlock = false;
  while (!lastBlock) {
    if (!input.readFully(scratch_, 4)) {
      return false;
    }
    const uint8_t* const header = scratch_.array();
    lastBlock = (header[0] & 0x80) != 0;
    const int blockType = header[0] & 0x7F;
    const int blockSize = (header[1] << 16) | (header[2] << 8) | header[3];
    if (blockSize < 0) {
      return false;
    }

    if (blockType == kBlockTypeStreamInfo) {
      if (blockSize != kStreamInfoSize || !input.readFully(scratch_, kStreamInfoSize)) {
        return false;
      }
      parseStreamInfo(scratch_.array(), kStreamInfoSize);
      hasStreamInfo_ = true;
      continue;
    }

    if (blockType == 127) {
      // An invalid block type, the stream is damaged.
      return false;
    }

    // The comment, picture and cue sheet blocks describe the stream; the other blocks are skipped.
    if ((blockType == kBlockTypeVorbisComment || blockType == kBlockTypeCuesheet ||
         blockType == kBlockTypePicture) &&
        blockSize > 0 && blockSize <= kMaxMetadataBlockSize) {
      std::vector<uint8_t> block(static_cast<size_t>(blockSize));
      if (!input.readFully(block.data(), 0, block.size())) {
        return false;
      }
      if (metadata_ == nullptr) {
        metadata_ = std::make_shared<Metadata>(std::vector<std::shared_ptr<Metadata::Entry>>{});
      }
      if (blockType == kBlockTypeVorbisComment) {
        AVVorbisCommentParser::parse(block.data(), block.size(), metadata_);
      } else if (blockType == kBlockTypeCuesheet) {
        AVVorbisCommentParser::parseFlacCuesheet(block.data(), block.size(), metadata_);
      } else {
        AVVorbisCommentParser::parseFlacPicture(block.data(), block.size(), metadata_);
      }
      continue;
    }

    // The remaining blocks are not needed, skip them without buffering.
    if (blockSize > kMaxMetadataBlockSize) {
      // A padding or a picture block may be large, skipping it is always possible.
      if (!input.skipFully(blockSize)) {
        return false;
      }
    } else if (!input.skipFully(blockSize)) {
      return false;
    }
  }

  firstFramePosition_ = input.getPosition();
  return hasStreamInfo_;
}

// ==================================================================================================
// Frames
// ==================================================================================================

bool AVFlacExtractor::peekFrameHeader(AVExtractorInput& input, int& samplesInFrame,
                                      int& frameSize) {
  input.resetPeekPosition();
  uint8_t header[6];
  const long peeked = input.peek(header, 0, sizeof(header));
  input.resetPeekPosition();
  if (peeked < 5) {
    return false;
  }
  // The sync code is 14 bits long: 0b11111111111110.
  if (header[0] != 0xFF || (header[1] & 0xFC) != 0xF8) {
    return false;
  }
  // The reserved bit must be zero and the blocking strategy must be either fixed or variable.
  if ((header[1] & 0x02) != 0) {
    return false;
  }

  const int blockSizeCode = (header[2] & 0xF0) >> 4;
  const int sampleRateCode = header[2] & 0x0F;
  const int channelAssignment = (header[3] & 0xF0) >> 4;
  const int bitsPerSampleCode = (header[3] & 0x0E) >> 1;
  if (blockSizeCode == 0 || sampleRateCode == 15 || channelAssignment > 10 ||
      bitsPerSampleCode == 3) {
    return false;
  }

  if (sampleRateCode == 0) {
    // The sample rate is the one of the STREAMINFO block.
    if (streamInfo_.sampleRate <= 0) {
      return false;
    }
  } else if (kSampleRates[sampleRateCode] <= 0) {
    return false;
  }
  const int bitsPerSample =
    bitsPerSampleCode == 0 ? streamInfo_.bitsPerSample : kBitsPerSample[bitsPerSampleCode];
  if (bitsPerSample <= 0) {
    return false;
  }

  int blockSize = kBlockSizes[blockSizeCode];
  int cursor = 4;
  if (blockSizeCode == 6) {
    // The block size is stored on the next byte, incremented by one.
    blockSize = header[cursor] + 1;
    ++cursor;
  } else if (blockSizeCode == 7) {
    // The block size is stored on the next two bytes, incremented by one.
    if (peeked < cursor + 2) {
      return false;
    }
    blockSize = ((header[cursor] << 8) | header[cursor + 1]) + 1;
    cursor += 2;
  }
  if (sampleRateCode == 12) {
    // The sample rate is stored on the next byte, in kHz.
    ++cursor;
  } else if (sampleRateCode == 13 || sampleRateCode == 14) {
    // The sample rate is stored on the next two bytes, in Hz or in tens of Hz.
    cursor += 2;
  }

  // The header ends with the CRC-8 of the previous bytes.
  frameSize = -1;
  samplesInFrame = blockSize;
  return true;
}

// ==================================================================================================
// Extractors
// ==================================================================================================

void AVFlacExtractor::init(AVExtractorOutput& output) {
  trackOutput_ = output.track(0, MediaType::kMediaTypeAudio, AVTrackOutput::emptyConsumer());
  seekMapOutput_ = &output;
  scratch_.reset(static_cast<uint32_t>(kDefaultFrameReadSize));
  output.seekMap(std::make_shared<AVUnseekableSeekMap>(Definitions::kTimeUnset));
  // A native FLAC stream holds a single track, therefore every track is known immediately.
  output.endTracks();
}

std::shared_ptr<MediaFormat> AVFlacExtractor::buildFormat() const {
  auto format =
    MediaFormat::CreateAudioFormat(MimeTypes::AUDIO_FLAC, streamInfo_.sampleRate,
                                   streamInfo_.channelCount);
  format->setContainerMimeType(MimeTypes::AUDIO_FLAC);
  format->setMaxInputSize(streamInfo_.maxFrameSize > 0 ? streamInfo_.maxFrameSize : kMaxFrameSize);
  format->setDurationUs(durationUs_);
  format->setSampleFormat(MediaUtils::getSampleFormat(streamInfo_.bitsPerSample));

  // The decoder needs the STREAMINFO block, exactly as it appears in the stream.
  std::vector<uint8_t> streamInfoBlock(4 + kStreamInfoSize);
  streamInfoBlock[0] = static_cast<uint8_t>(kBlockTypeStreamInfo);
  streamInfoBlock[1] = 0;
  streamInfoBlock[2] = 0;
  streamInfoBlock[3] = static_cast<uint8_t>(kStreamInfoSize);
  const uint16_t minBlockSize = static_cast<uint16_t>(streamInfo_.minBlockSize);
  const uint16_t maxBlockSize = static_cast<uint16_t>(streamInfo_.maxBlockSize);
  streamInfoBlock[4] = static_cast<uint8_t>(minBlockSize >> 8);
  streamInfoBlock[5] = static_cast<uint8_t>(minBlockSize & 0xFF);
  streamInfoBlock[6] = static_cast<uint8_t>(maxBlockSize >> 8);
  streamInfoBlock[7] = static_cast<uint8_t>(maxBlockSize & 0xFF);
  streamInfoBlock[8] = static_cast<uint8_t>((streamInfo_.minFrameSize >> 16) & 0xFF);
  streamInfoBlock[9] = static_cast<uint8_t>((streamInfo_.minFrameSize >> 8) & 0xFF);
  streamInfoBlock[10] = static_cast<uint8_t>(streamInfo_.minFrameSize & 0xFF);
  streamInfoBlock[11] = static_cast<uint8_t>((streamInfo_.maxFrameSize >> 16) & 0xFF);
  streamInfoBlock[12] = static_cast<uint8_t>((streamInfo_.maxFrameSize >> 8) & 0xFF);
  streamInfoBlock[13] = static_cast<uint8_t>(streamInfo_.maxFrameSize & 0xFF);

  const uint64_t packed = (static_cast<uint64_t>(streamInfo_.sampleRate & 0xFFFFF) << 44) |
                          (static_cast<uint64_t>((streamInfo_.channelCount - 1) & 0x7) << 41) |
                          (static_cast<uint64_t>((streamInfo_.bitsPerSample - 1) & 0x1F) << 36) |
                          (static_cast<uint64_t>(streamInfo_.totalSamples) & 0xFFFFFFFFFLL);
  for (int i = 0; i < 8; ++i) {
    streamInfoBlock[14 + i] = static_cast<uint8_t>((packed >> (56 - i * 8)) & 0xFF);
  }
  // The last 16 bytes are the MD5 of the unencoded audio, which this extractor does not keep.
  format->setCodecSpecificData(0, streamInfoBlock.data(), streamInfoBlock.size());
  if (metadata_ != nullptr) {
    format->setMetadata(metadata_);
  }
  return format;
}

AVExtractor::Result AVFlacExtractor::read(AVExtractorInput& input) {
  if (!metadataRead_) {
    if (!readMetadata(input)) {
      position_ = input.getPosition();
      return kResultEndOfInput;
    }
    metadataRead_ = true;
  }
  // The extractor reports the position of the input, so that the reader always observes progress,
  // even when the metadata was the only thing that was consumed.
  position_ = input.getPosition();
  if (!hasStreamInfo_) {
    return kResultEndOfInput;
  }

  if (!formatPublished_) {
    trackOutput_->format(buildFormat());
    formatPublished_ = true;
    if (seekMapOutput_ != nullptr) {
      if (durationUs_ != Definitions::kTimeUnset && firstFramePosition_ >= 0 && streamInfo_.sampleRate > 0) {
        // A native FLAC stream is seekable by interpolation only when the frame size is constant,
        // therefore the seek map describes a single point at the start of the audio.
        std::vector<AVSeekMap::SeekPoint> seekPoints;
        seekPoints.push_back(AVSeekMap::SeekPoint(0, firstFramePosition_));
        seekMapOutput_->seekMap(std::make_shared<AVIndexSeekMap>(seekPoints, durationUs_, true));
      } else {
        seekMapOutput_->seekMap(std::make_shared<AVUnseekableSeekMap>(durationUs_));
      }
    }
    seekMapPublished_ = true;
  }

  int samplesInFrame = 0;
  int frameSize = 0;
  if (!peekFrameHeader(input, samplesInFrame, frameSize)) {
    position_ = input.getPosition();
    if (!trackEnded_) {
      trackOutput_->endTrack();
      trackEnded_ = true;
    }
    return kResultEndOfInput;
  }

  // A FLAC frame is self delimiting, but its size is only known once the next frame header was
  // found. The STREAMINFO block gives an upper bound of the size of a frame, which is what the
  // reader uses; a frame that is larger than that bound is split over several reads, which the
  // decoder accepts because a frame is a self contained unit of compressed audio.
  int toRead = kDefaultFrameReadSize;
  if (streamInfo_.maxFrameSize > 0) {
    toRead = std::max(streamInfo_.maxFrameSize, kDefaultFrameReadSize);
  }
  const long remaining = input.getLength() >= 0 ? input.getLength() - input.getPosition() : -1;
  if (remaining > 0 && remaining < toRead) {
    toRead = static_cast<int>(remaining);
  }
  const long read = input.read(scratch_, static_cast<size_t>(toRead));
  if (read == kEndOfInput) {
    position_ = input.getPosition();
    if (!trackEnded_) {
      trackOutput_->endTrack();
      trackEnded_ = true;
    }
    return kResultEndOfInput;
  }

  const int64_t timeUs = streamInfo_.sampleRate > 0
                           ? samplesRead_ * AVUtil::kMicrosPerSecond / streamInfo_.sampleRate
                           : Definitions::kTimeUnset;
  samplesRead_ += samplesInFrame;
  ++frameIndex_;

  if (read > 0) {
    auto buffer = std::make_shared<AVBlockBuffer>(static_cast<uint32_t>(read));
    buffer->put(scratch_.array(), static_cast<size_t>(read));
    buffer->position(0);
    buffer->limit(static_cast<size_t>(read));
    uint32_t flags = AVSample::kFlagKeyFrame;
    if (frameIndex_ == 1) {
      flags |= AVSample::kFlagFirstSample;
    }
    AVSample sample(0, timeUs, flags, std::move(buffer));
    trackOutput_->sample(sample);
  }

  position_ = input.getPosition();
  return kResultContinue;
}

bool AVFlacExtractor::seek(long position, int64_t timeUs) {
  static_cast<void>(position);
  static_cast<void>(timeUs);
  // The extractor is stateless with respect to the position, the reader sets the input position.
  return true;
}

} // namespace coral
