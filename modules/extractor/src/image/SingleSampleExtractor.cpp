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

#include <extractor/image/SingleSampleExtractor.h>

#include <extractor/Utility.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace coral {

namespace {

/** The number of bytes that are read from the input at once. */
constexpr size_t kReadSize = 64 * 1024;

} // namespace anonymous

AVSingleSampleExtractor::AVSingleSampleExtractor(const int fileSignature, const int fileSignatureLength,
  std::string sampleMimeType)
  : fileSignature_(fileSignature),
    fileSignatureLength_(fileSignatureLength),
    sampleMimeType_(std::move(sampleMimeType)) {
  data_.reserve(kReadSize);
}

int AVSingleSampleExtractor::sniff(AVExtractorInput& input) {
  if (fileSignature_ < 0 || fileSignatureLength_ <= 0) {
    return AVExtractorsFactory::kSniffResultNone;
  }
  input.resetPeekPosition();
  uint8_t signature[4] = { 0, 0, 0, 0 };
  const size_t length = static_cast<size_t>(fileSignatureLength_);
  const long peeked = input.peek(signature, 0, std::min(length, sizeof(signature)));
  input.resetPeekPosition();
  if (peeked < static_cast<long>(length)) {
    return AVExtractorsFactory::kSniffResultNone;
  }
  uint32_t value = 0;
  if (length == 2) {
    value = AVUtil::readUnsignedShort(signature, 0);
  } else if (length == 3) {
    value = AVUtil::readUnsignedInt24(signature, 0);
  } else if (length >= 4) {
    value = AVUtil::readUnsignedInt(signature, 0);
  } else {
    value = signature[0];
  }
  return value == static_cast<uint32_t>(fileSignature_)
           ? AVExtractorsFactory::kSniffResultCertain
           : AVExtractorsFactory::kSniffResultNone;
}

void AVSingleSampleExtractor::init(AVExtractorOutput& output) {
  trackOutput_ = output.track(kImageTrackId, MediaType::kMediaTypeImage, AVTrackOutput::emptyConsumer());
  trackOutput_->format(buildFormat());
  // The whole input is one sample that starts at position zero, therefore every seek resolves to
  // the start of the file and the map is seekable even though the duration is unknown.
  std::vector<AVSeekMap::SeekPoint> seekPoints;
  seekPoints.emplace_back(0, 0);
  output.seekMap(std::make_shared<AVIndexSeekMap>(std::move(seekPoints), Definitions::kTimeUnset, true));
  output.endTracks();
  initialized_ = true;
}

std::shared_ptr<MediaFormat> AVSingleSampleExtractor::buildFormat() const {
  auto format = MediaFormat::CreateVideoFormat(sampleMimeType_, MediaFormat::NO_VALUE, MediaFormat::NO_VALUE);
  format->setContainerMimeType(sampleMimeType_);
  format->setMaxInputSize(static_cast<int>(kMaxSampleSize));
  return format;
}

AVExtractor::Result AVSingleSampleExtractor::read(AVExtractorInput& input) {
  position_ = input.getPosition();
  if (!initialized_) {
    return kResultEndOfInput;
  }
  if (ended_) {
    return kResultEndOfInput;
  }
  const long length = input.getLength();
  if (length > static_cast<long>(kMaxSampleSize)) {
    // The sample cannot be buffered within the bound, the track ends without data rather than
    // allocating an amount that the input declares.
    ended_ = true;
    outputSample();
    return kResultEndOfInput;
  }

  uint8_t chunk[kReadSize];
  const long read = input.read(chunk, 0, sizeof(chunk));
  position_ = input.getPosition();
  if (read == kEndOfInput) {
    ended_ = true;
    outputSample();
    return kResultEndOfInput;
  }
  if (read <= 0) {
    return kResultEndOfInput;
  }
  data_.insert(data_.end(), chunk, chunk + read);
  return kResultContinue;
}

void AVSingleSampleExtractor::outputSample() {
  if (trackOutput_ == nullptr) {
    return;
  }
  if (!data_.empty()) {
    auto buffer = std::make_shared<AVBlockBuffer>(static_cast<uint32_t>(data_.size()));
    buffer->put(data_.data(), data_.size());
    buffer->position(0);
    buffer->limit(data_.size());
    AVSample sample(kImageTrackId, 0, AVSample::kFlagKeyFrame | AVSample::kFlagFirstSample, std::move(buffer));
    trackOutput_->sample(sample);
  }
  if (!trackEnded_) {
    trackOutput_->endTrack();
    trackEnded_ = true;
  }
}

bool AVSingleSampleExtractor::seek(const long position, const int64_t timeUs) {
  static_cast<void>(timeUs);
  if (position == 0 || !ended_) {
    // A seek back to the start re-reads the sample; a seek that happens while the sample is still
    // being read restarts it, because the reader rewound the input.
    ended_ = false;
    trackEnded_ = false;
    data_.clear();
    data_.reserve(kReadSize);
  }
  position_ = position;
  return true;
}

} // namespace coral
