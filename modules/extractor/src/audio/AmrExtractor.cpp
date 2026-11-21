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

#include <extractor/audio/AmrExtractor.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>

namespace coral {

namespace {

/**
 * The frame sizes, including the table of contents byte, indexed by frame type, for AMR narrow band.
 * The table is the one of RFC 4867, section 5.3.
 */
constexpr int kFrameSizeBytesByTypeNb[] = { 13, 14, 16, 18, 20, 21, 27, 32, 6, 7, 6, 6, 1, 1, 1, 1 };

/**
 * The frame sizes, including the table of contents byte, indexed by frame type, for AMR wide band.
 */
constexpr int kFrameSizeBytesByTypeWb[] = { 18, 24, 33, 37, 41, 47, 51, 59, 61, 6, 1, 1, 1, 1, 1, 1 };

/** The signature of a narrow band stream. */
constexpr char kSignatureNb[] = "#!AMR\n";

/** The signature of a wide band stream. */
constexpr char kSignatureWb[] = "#!AMR-WB\n";

/** The number of bits of a table of contents byte that may not be set. */
constexpr uint8_t kTocPaddingMask = 0x83;

/** The sample rate of a narrow band stream, in Hz. */
constexpr int kSampleRateNb = 8000;

/** The sample rate of a wide band stream, in Hz. */
constexpr int kSampleRateWb = 16000;

/** The duration of every frame, in microseconds. */
constexpr int64_t kSampleTimePerFrameUs = 20000;

/** The maximum size of a frame of either mode, in bytes. */
constexpr int kMaxFrameSize = 61;

} // namespace anonymous

int AVAmrExtractor::sniff(AVExtractorInput& input) {
  sniffFailureReason_.clear();
  input.resetPeekPosition();

  uint8_t signature[sizeof(kSignatureWb)];
  const long peeked = input.peek(signature, 0, sizeof(signature));
  input.resetPeekPosition();
  if (peeked >= static_cast<long>(sizeof(kSignatureNb) - 1) &&
      std::memcmp(signature, kSignatureNb, sizeof(kSignatureNb) - 1) == 0) {
    return AVExtractorsFactory::kSniffResultCertain;
  }
  if (peeked >= static_cast<long>(sizeof(kSignatureWb) - 1) &&
      std::memcmp(signature, kSignatureWb, sizeof(kSignatureWb) - 1) == 0) {
    return AVExtractorsFactory::kSniffResultCertain;
  }
  sniffFailureReason_ = "The AMR signature was not found.";
  return AVExtractorsFactory::kSniffResultNone;
}

void AVAmrExtractor::init(AVExtractorOutput& output) {
  trackOutput_ = output.track(0, MediaType::kMediaTypeAudio, AVTrackOutput::emptyConsumer());
  output.seekMap(std::make_shared<AVUnseekableSeekMap>(Definitions::kTimeUnset));
  // An AMR stream holds a single track, therefore every track is known immediately.
  output.endTracks();
}

bool AVAmrExtractor::readHeader(AVExtractorInput& input) {
  input.resetPeekPosition();
  uint8_t signature[sizeof(kSignatureWb)];
  const long peeked = input.peek(signature, 0, sizeof(signature));
  if (peeked < static_cast<long>(sizeof(kSignatureNb) - 1)) {
    return false;
  }
  long signatureSize = 0;
  if (peeked >= static_cast<long>(sizeof(kSignatureWb) - 1) &&
      std::memcmp(signature, kSignatureWb, sizeof(kSignatureWb) - 1) == 0) {
    isWideBand_ = true;
    signatureSize = sizeof(kSignatureWb) - 1;
  } else if (std::memcmp(signature, kSignatureNb, sizeof(kSignatureNb) - 1) == 0) {
    isWideBand_ = false;
    signatureSize = sizeof(kSignatureNb) - 1;
  } else {
    return false;
  }
  return input.setPosition(signatureSize, true);
}

std::shared_ptr<MediaFormat> AVAmrExtractor::buildFormat() const {
  const std::string containerMimeType = isWideBand_ ? MimeTypes::AUDIO_AMR_WB : MimeTypes::AUDIO_AMR;
  const std::string sampleMimeType = isWideBand_ ? MimeTypes::AUDIO_AMR_WB : MimeTypes::AUDIO_AMR_NB;
  const int sampleRate = isWideBand_ ? kSampleRateWb : kSampleRateNb;
  const int maxInputSize = isWideBand_ ? kFrameSizeBytesByTypeWb[8] : kFrameSizeBytesByTypeNb[7];

  auto format = MediaFormat::CreateAudioFormat(sampleMimeType, sampleRate, 1);
  format->setContainerMimeType(containerMimeType);
  format->setMaxInputSize(maxInputSize);
  format->setSampleFormat(AudioSampleFormat::kDefault);
  return format;
}

int AVAmrExtractor::getFrameSize(const int frameType) const {
  if (frameType < 0 || frameType > 15) {
    return -1;
  }
  if (isWideBand_) {
    // For wide band, the frame types 10 to 13 are reserved for future use.
    if (frameType >= 10 && frameType <= 13) {
      return -1;
    }
    return kFrameSizeBytesByTypeWb[frameType];
  }
  // For narrow band, the frame types 12 to 14 are reserved for future use.
  if (frameType >= 12 && frameType <= 14) {
    return -1;
  }
  return kFrameSizeBytesByTypeNb[frameType];
}

AVExtractor::Result AVAmrExtractor::read(AVExtractorInput& input) {
  position_ = input.getPosition();
  if (!headerRead_) {
    if (!readHeader(input)) {
      finish(input);
      return kResultEndOfInput;
    }
    headerRead_ = true;
    position_ = input.getPosition();
  }
  if (trackOutput_ == nullptr) {
    finish(input);
    return kResultEndOfInput;
  }
  if (!formatPublished_) {
    trackOutput_->format(buildFormat());
    formatPublished_ = true;
  }

  input.resetPeekPosition();
  const int tableOfContents = input.peekByte();
  if (tableOfContents == static_cast<int>(kEndOfInput)) {
    finish(input);
    return kResultEndOfInput;
  }
  const auto toc = static_cast<uint8_t>(tableOfContents);
  if ((toc & kTocPaddingMask) != 0) {
    // The padding bits of the table of contents byte must be zero, a frame that sets one of them is
    // not an AMR frame at all.
    finish(input);
    return kResultEndOfInput;
  }
  const int frameType = (toc >> 3) & 0x0F;
  const int frameSize = getFrameSize(frameType);
  if (frameSize <= 0) {
    finish(input);
    return kResultEndOfInput;
  }

  uint8_t frame[kMaxFrameSize];
  const long read = input.read(frame, 0, static_cast<size_t>(frameSize));
  position_ = input.getPosition();
  if (read != frameSize) {
    // A truncated frame is dropped, there is nothing left to read after it.
    finish(input);
    return kResultEndOfInput;
  }

  auto buffer = std::make_shared<AVBlockBuffer>(static_cast<uint32_t>(frameSize));
  buffer->put(frame, static_cast<size_t>(frameSize));
  buffer->position(0);
  buffer->limit(static_cast<size_t>(frameSize));

  if (skipBeforeTimeUs_ >= 0 && currentSampleTimeUs_ < skipBeforeTimeUs_) {
    // The seek restarted the stream and the requested time was not reached yet. The frame is
    // consumed without being delivered, and the timestamps keep following the stream.
    currentSampleTimeUs_ += kSampleTimePerFrameUs;
    return kResultContinue;
  }

  uint32_t flags = AVSample::kFlagKeyFrame;
  if (sampleCount_ == 0) {
    flags |= AVSample::kFlagFirstSample;
  }
  AVSample sample(0, currentSampleTimeUs_, flags, std::move(buffer));
  trackOutput_->sample(sample);

  ++sampleCount_;
  currentSampleTimeUs_ += kSampleTimePerFrameUs;
  return kResultContinue;
}

bool AVAmrExtractor::seek(const long position, const int64_t timeUs) {
  if (position == 0) {
    // The reader restarted the input, the signature has to be consumed again.
    headerRead_ = false;
    isWideBand_ = false;
    formatPublished_ = false;
    trackEnded_ = false;
    currentSampleTimeUs_ = 0;
    sampleCount_ = 0;
  }
  // An AMR stream has no index, therefore a time seek restarts it and the frames before the
  // requested time are skipped.
  skipBeforeTimeUs_ = position == 0 && timeUs > 0 ? timeUs : -1;
  return true;
}

void AVAmrExtractor::finish(AVExtractorInput& input) {
  position_ = input.getPosition();
  if (trackOutput_ != nullptr && !trackEnded_) {
    trackOutput_->endTrack();
    trackEnded_ = true;
  }
}

} // namespace coral
