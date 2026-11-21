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

#include <extractor/audio/WavExtractor.h>

#include <extractor/Utility.h>

#include <media/Utilities.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>

namespace coral {

namespace {

/** The identifiers of the RIFF chunks that this extractor understands. */
constexpr uint32_t kRiff = 0x52494646;       // "RIFF"
constexpr uint32_t kWave = 0x57415645;       // "WAVE"
constexpr uint32_t kFormat = 0x666D7420;     // "fmt "
constexpr uint32_t kData = 0x64617461;       // "data"
constexpr uint32_t kId3 = 0x49443320;        // "ID3 "
constexpr uint32_t kFact = 0x66616374;       // "fact"
constexpr uint32_t kRiff2 = 0x52494632;      // "RIF2"
constexpr uint32_t kRf64 = 0x52463634;       // "RF64"

/** The WAVE_FORMAT_* identifiers. */
constexpr int kWaveFormatPcm = 0x0001;
constexpr int kWaveFormatIeeeFloat = 0x0003;
constexpr int kWaveFormatALaw = 0x0006;
constexpr int kWaveFormatMuLaw = 0x0007;
constexpr int kWaveFormatExtensible = 0xFFFE;

/** The maximum size of a RIFF chunk that this extractor is willing to read into memory. */
constexpr int kMaxChunkHeaderSize = 1024 * 1024;

/** The number of bytes read at once from the data chunk. */
constexpr size_t kDataChunkReadSize = 16 * 1024;

} // namespace anonymous

int AVWavExtractor::sniff(AVExtractorInput& input) {
  sniffFailureReason_.clear();
  input.resetPeekPosition();

  uint8_t scratch[16];
  const long peeked = input.peek(scratch, 0, sizeof(scratch));
  input.resetPeekPosition();
  if (peeked < 12) {
    sniffFailureReason_ = "The input is shorter than a RIFF header.";
    return AVExtractorsFactory::kSniffResultNone;
  }

  const uint32_t riff = AVUtil::readUnsignedInt(scratch, 0);
  if (riff != kRiff && riff != kRiff2 && riff != kRf64) {
    sniffFailureReason_ = "The RIFF identifier was not found.";
    return AVExtractorsFactory::kSniffResultNone;
  }
  if (AVUtil::readUnsignedInt(scratch, 8) != kWave) {
    sniffFailureReason_ = "The WAVE identifier was not found.";
    return AVExtractorsFactory::kSniffResultNone;
  }
  if (peeked >= 16 && AVUtil::readUnsignedInt(scratch, 12) == kFormat) {
    // The format chunk immediately follows the header, this is definitely a WAV file.
    return AVExtractorsFactory::kSniffResultCertain;
  }
  return AVExtractorsFactory::kSniffResultPossible;
}

void AVWavExtractor::init(AVExtractorOutput& output) {
  output_ = &output;
  trackOutput_ = output.track(0, MediaType::kMediaTypeAudio, AVTrackOutput::emptyConsumer());
  // The duration is unknown until the data chunk is found. Declaring the seek map here rather than
  // after the header was parsed keeps a file whose header is broken reportable: the reader is told
  // that the container cannot be seeked even though it never learns its duration.
  output.seekMap(std::make_shared<AVUnseekableSeekMap>(Definitions::kTimeUnset));
  // A RIFF file holds a single stream, therefore every track is known immediately.
  output.endTracks();
  scratch_.reset(static_cast<uint32_t>(kDataChunkReadSize));
}

bool AVWavExtractor::readHeaders(AVExtractorInput& input) {
  // The RIFF header: "RIFF" + size + "WAVE", where every field is little endian except the two
  // four byte identifiers.
  if (!input.readFully(scratch_, 12)) {
    return false;
  }
  const uint32_t riff = AVUtil::readUnsignedInt(scratch_.array(), 0);
  const uint32_t wave = AVUtil::readUnsignedInt(scratch_.array(), 8);
  if ((riff != kRiff && riff != kRiff2 && riff != kRf64) || wave != kWave) {
    return false;
  }

  bool foundFormat = false;
  const long fileLength = input.getLength();
  while (true) {
    const long chunkStart = input.getPosition();
    if (fileLength >= 0 && chunkStart + 8 > fileLength) {
      return false;
    }
    if (!input.readFully(scratch_, 8)) {
      return false;
    }
    const uint32_t id = AVUtil::readUnsignedInt(scratch_.array(), 0);
    const uint32_t chunkSize = scratch_.getLittleEndianUnsignedInt(4);

    if (id == kData) {
      if (!foundFormat) {
        return false;
      }
      const long realChunkSize = getChunkSize(chunkSize);
      dataStartPosition_ = input.getPosition();
      if (realChunkSize < 0 || fileLength < 0) {
        dataEndPosition_ = -1;
      } else {
        dataEndPosition_ = dataStartPosition_ + realChunkSize;
      }
      if (dataEndPosition_ >= 0 && format_.byteRate > 0) {
        durationUs_ = static_cast<int64_t>(
          (dataEndPosition_ - dataStartPosition_) * AVUtil::kMicrosPerSecond / format_.byteRate);
      }
      return true;
    }

    if (id == kRf64) {
      // The ds64 chunk of an RF64 file holds the real sizes: the RIFF size, the data size, the
      // sample count and the table length, in that order.
      if (chunkSize < 28 || !input.readFully(scratch_, 28)) {
        return false;
      }
      const uint8_t* const ds64 = scratch_.array();
      const uint64_t dataSize = (static_cast<uint64_t>(AVUtil::readUnsignedInt(ds64, 8)) << 32) |
                                AVUtil::readUnsignedInt(ds64, 12);
      if (dataSize > 0 && dataSize <= static_cast<uint64_t>(std::numeric_limits<long>::max())) {
        rf64DataSize_ = static_cast<long>(dataSize);
      }
      const long padding = static_cast<long>(chunkSize % 2);
      if (padding != 0 && !input.skipFully(padding)) {
        return false;
      }
      continue;
    }

    if (id == kFormat) {
      if (chunkSize == 0 || chunkSize > static_cast<uint32_t>(kMaxChunkHeaderSize)) {
        return false;
      }
      if (!input.readFully(scratch_, chunkSize)) {
        return false;
      }
      if (chunkSize < 16) {
        return false;
      }
      // The whole fmt chunk is little endian.
      const uint8_t* const fmt = scratch_.array();
      static_cast<void>(fmt);
      format_.codec = scratch_.getLittleEndianUnsignedShort(0);
      format_.channelCount = scratch_.getLittleEndianUnsignedShort(2);
      format_.sampleRate = static_cast<int>(scratch_.getLittleEndianUnsignedInt(4));
      format_.byteRate = static_cast<int>(scratch_.getLittleEndianUnsignedInt(8));
      format_.blockAlign = scratch_.getLittleEndianUnsignedShort(12);
      format_.bitsPerSample = scratch_.getLittleEndianUnsignedShort(14);
      if (format_.codec == kWaveFormatExtensible) {
        if (chunkSize >= 26) {
          // The first two bytes of the extension are the size of the extension, the following two
          // bytes are the valid bits per sample, then the channel mask, then the real format.
          const int validBitsPerSample = scratch_.getLittleEndianUnsignedShort(18);
          const int subFormatOffset = 24;
          if (chunkSize >= static_cast<uint32_t>(subFormatOffset + 2)) {
            format_.bitsPerSample = validBitsPerSample;
            format_.codec = scratch_.getLittleEndianUnsignedShort(subFormatOffset);
          }
        }
        if (format_.codec == kWaveFormatExtensible) {
          return false;
        }
      }
      if (format_.channelCount <= 0 || format_.sampleRate <= 0) {
        return false;
      }
      if (format_.blockAlign <= 0) {
        const int bytesPerSample = std::max(1, format_.bitsPerSample / 8);
        format_.blockAlign = format_.channelCount * bytesPerSample;
      }
      if (format_.byteRate <= 0) {
        format_.byteRate = format_.sampleRate * format_.blockAlign;
      }
      hasFormat_ = true;
      foundFormat = true;
      const long padding = static_cast<long>(chunkSize % 2);
      if (padding != 0 && !input.skipFully(padding)) {
        return false;
      }
      continue;
    }

    if (id == kId3 || id == kFact) {
      // Metadata and the fact chunk are not needed.
      const long toSkip = getChunkSize(chunkSize);
      if (toSkip < 0 || !input.skipFully(toSkip + toSkip % 2)) {
        return false;
      }
      continue;
    }

    // An unknown chunk, skip it.
    const long unknownChunkSize = getChunkSize(chunkSize);
    if (unknownChunkSize < 0 || !input.skipFully(unknownChunkSize + unknownChunkSize % 2)) {
      return false;
    }
  }
}

long AVWavExtractor::getChunkSize(const uint32_t chunkSize) {
  if (chunkSize != 0xFFFFFFFF) {
    return static_cast<long>(chunkSize);
  }
  // An RF64 file stores a placeholder in the chunk header and the real size in its ds64 chunk.
  if (rf64DataSize_ >= 0) {
    const long size = rf64DataSize_;
    rf64DataSize_ = -1;
    return size;
  }
  return -1;
}

std::shared_ptr<MediaFormat> AVWavExtractor::buildFormat() const {
  std::string mimeType;
  switch (format_.codec) {
    case kWaveFormatIeeeFloat:
    case kWaveFormatPcm: {
      mimeType = MimeTypes::AUDIO_RAW;
      break;
    }
    case kWaveFormatALaw: {
      mimeType = MimeTypes::AUDIO_G711_ALAW;
      break;
    }
    case kWaveFormatMuLaw: {
      mimeType = MimeTypes::AUDIO_G711_MLAW;
      break;
    }
    default: {
      mimeType = MimeTypes::AUDIO_RAW;
      break;
    }
  }

  auto format = MediaFormat::CreateAudioFormat(mimeType, format_.sampleRate, format_.channelCount);
  format->setContainerMimeType(MimeTypes::AUDIO_WAV);
  format->setMaxInputSize(format_.blockAlign);
  format->setDurationUs(durationUs_);
  format->setAverageBitrate(format_.byteRate * 8);

  AudioSampleFormat sampleFormat = AudioSampleFormat::kDefault;
  switch (format_.codec) {
    case kWaveFormatIeeeFloat: {
      // A 64 bit float wave file is rare, and cannot be rendered without a conversion, therefore
      // it is reported as a float format and left to the decoder.
      sampleFormat = format_.bitsPerSample <= 32 ? AudioSampleFormat::kFloat
                                                 : AudioSampleFormat::kFloat;
      break;
    }
    case kWaveFormatALaw:
    case kWaveFormatMuLaw: {
      sampleFormat = AudioSampleFormat::kInt16;
      break;
    }
    case kWaveFormatPcm:
    default: {
      sampleFormat = MediaUtils::getSampleFormat(format_.bitsPerSample);
      break;
    }
  }
  format->setSampleFormat(sampleFormat);
  return format;
}

AVExtractor::Result AVWavExtractor::read(AVExtractorInput& input) {
  if (state_ == State::kReadingHeader) {
    if (!readHeaders(input)) {
      position_ = input.getPosition();
      return kResultEndOfInput;
    }
    state_ = State::kReadingData;
    if (output_ != nullptr && durationUs_ != Definitions::kTimeUnset) {
      // The data chunk gave the duration, so the seek map that was declared while the extractor was
      // initialized is replaced by one that reports it. The container itself is still not seekable,
      // because the extractor only supports reading forward.
      output_->seekMap(std::make_shared<AVUnseekableSeekMap>(durationUs_));
    }
  }
  // The extractor reports the position it reached, which is the position of the input. This keeps
  // the progress observable by the reader even when the data chunk is empty.
  position_ = input.getPosition();

  if (!hasFormat_) {
    return kResultEndOfInput;
  }
  if (trackOutput_ == nullptr) {
    return kResultEndOfInput;
  }
  if (!formatPublished_) {
    // The format is only complete once the data chunk was found, publish it exactly once.
    trackOutput_->format(buildFormat());
    formatPublished_ = true;
  }

  if (dataEndPosition_ >= 0 && position_ >= dataEndPosition_) {
    if (!trackEnded_) {
      trackOutput_->endTrack();
      trackEnded_ = true;
    }
    return kResultEndOfInput;
  }

  size_t toRead = kDataChunkReadSize;
  if (dataEndPosition_ >= 0) {
    const long remaining = dataEndPosition_ - position_;
    if (remaining < static_cast<long>(toRead)) {
      toRead = static_cast<size_t>(remaining);
    }
  }
  const long readStartPosition = position_;
  const long read = input.read(scratch_, toRead);
  if (read == kEndOfInput) {
    position_ = input.getPosition();
    if (!trackEnded_) {
      trackOutput_->endTrack();
      trackEnded_ = true;
    }
    return kResultEndOfInput;
  }
  totalBytesRead_ += read;
  position_ = input.getPosition();

  // Only deliver whole audio frames, a partial frame at the end of the chunk is dropped.
  size_t usable = static_cast<size_t>(read);
  if (format_.blockAlign > 0) {
    usable -= usable % static_cast<size_t>(format_.blockAlign);
  }
  if (usable > 0) {
    const int64_t timeUs = format_.byteRate > 0
                             ? static_cast<int64_t>((readStartPosition - dataStartPosition_) *
                                                    AVUtil::kMicrosPerSecond / format_.byteRate)
                             : Definitions::kTimeUnset;

    auto buffer = std::make_shared<AVBlockBuffer>(static_cast<uint32_t>(usable));
    buffer->put(scratch_.array(), usable);
    buffer->position(0);
    buffer->limit(usable);

    AVSample sample(0, timeUs, AVSample::kFlagKeyFrame, std::move(buffer));
    trackOutput_->sample(sample);
  }
  return kResultContinue;
}

bool AVWavExtractor::seek(const long position, const int64_t timeUs) {
  static_cast<void>(timeUs);
  if (dataStartPosition_ >= 0 && position < dataStartPosition_) {
    // The reader restarted the input before the samples, which is the only seek this extractor
    // supports because its seek map is not seekable. The header has to be parsed again, otherwise
    // the bytes of the header would be reported as audio.
    state_ = State::kReadingHeader;
    hasFormat_ = false;
    formatPublished_ = false;
    trackEnded_ = false;
    totalBytesRead_ = 0;
  }
  // Apart from the restart, the extractor is stateless with respect to the position: the reader
  // sets the position of the input, and the samples are read from there.
  return true;
}

} // namespace coral
