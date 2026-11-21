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

#include <extractor/Reader.h>

#include <algorithm>
#include <utility>

namespace coral {

namespace {

/** The default number of bytes that the probe may peek. */
constexpr size_t kProbeBufferSize = 64 * 1024;

/**
 * Reads exactly {@code length} bytes at an absolute position, looping until the data source has
 * nothing left to give.
 *
 * @return The number of bytes that were read, which is smaller than {@code length} only when the
 *         end of the stream was reached.
 */
size_t readAtFully(AVDataSource& dataSource, const long position, uint8_t* buffer,
                   const size_t length) {
  size_t total = 0;
  while (total < length) {
    const int read = dataSource.readAt(position + static_cast<long>(total), buffer, total,
                                       length - total);
    if (read <= 0) {
      break;
    }
    total += static_cast<size_t>(read);
  }
  return total;
}

} // namespace anonymous

// ==================================================================================================
// AVTrackOutput
// ==================================================================================================

const AVTrackOutput::SampleConsumer& AVTrackOutput::emptyConsumer() {
  static const SampleConsumer sEmpty;
  return sEmpty;
}

void AVTrackOutput::endTrack() {
  AVSample endOfStream;
  endOfStream.trackIndex = id_;
  endOfStream.flags = static_cast<uint32_t>(AVSample::kFlagEndOfStream);
  sample(endOfStream);
}

// ==================================================================================================
// AVExtractor
// ==================================================================================================

bool AVExtractor::seek(const long position, const int64_t timeUs) {
  static_cast<void>(position);
  static_cast<void>(timeUs);
  // The default implementation accepts every seek: the container is re-read from the new position,
  // which is what {@link AVReader} does with the input of the extractor.
  return true;
}

// ==================================================================================================
// AVSample
// ==================================================================================================

std::string AVSample::toString() const {
  std::string str = "AVSample{trackIndex=" + std::to_string(trackIndex) +
                    ", timeUs=" + std::to_string(timeUs) +
                    ", size=" + std::to_string(size()) + ", flags=[";
  bool first = true;
  const auto appendFlag = [&str, &first](const char* name) {
    if (!first) {
      str.append(", ");
    }
    str.append(name);
    first = false;
  };
  if (hasFlag(kFlagKeyFrame)) {
    appendFlag("keyFrame");
  }
  if (hasFlag(kFlagCodecConfig)) {
    appendFlag("codecConfig");
  }
  if (hasFlag(kFlagEndOfStream)) {
    appendFlag("endOfStream");
  }
  if (hasFlag(kFlagFirstSample)) {
    appendFlag("firstSample");
  }
  if (hasFlag(kFlagLastSample)) {
    appendFlag("lastSample");
  }
  if (hasFlag(kFlagHasSupplementalData)) {
    appendFlag("supplementalData");
  }
  if (hasFlag(kFlagDiscard)) {
    appendFlag("discard");
  }
  str.append("]}");
  return str;
}

// ==================================================================================================
// AVSeekMap
// ==================================================================================================

std::string AVSeekMap::SeekPoint::toString() const {
  return "{timeUs=" + std::to_string(timeUs) + ", position=" + std::to_string(position) + "}";
}

bool AVIndexSeekMap::getSeekPoints(const int64_t timeUs, SeekPoint& first, SeekPoint& second) const {
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
  second = (it == seekPoints_.end()) ? first : *it;
  return true;
}

// ==================================================================================================
// AVExtractorsFactory
// ==================================================================================================

std::vector<std::unique_ptr<AVExtractor>>
AVExtractorsFactory::createExtractors(const std::string& uri,
                                      const std::vector<std::string>& responseHeaders) {
  static_cast<void>(uri);
  static_cast<void>(responseHeaders);
  return createExtractors();
}

// ==================================================================================================
// AVReaderOutput
// ==================================================================================================

std::shared_ptr<AVTrackOutput> AVReaderOutput::track(const int id, const MediaType type,
                                                     AVTrackOutput::SampleConsumer consumer) {
  // The caller supplied consumer wins over the resolver, which lets an extractor route the samples
  // of a track without going through the reader.
  if (!consumer && resolver_) {
    consumer = resolver_(id);
  }
  auto info = std::make_shared<AVTrackInfo>(id, type, std::move(consumer));
  auto trackOutput = std::make_shared<AVTrackOutput>(id, info);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    tracks_[id] = info;
  }
  return trackOutput;
}

void AVReaderOutput::setFormat(const int id, std::shared_ptr<MediaFormat> format) {
  const auto track = getTrack(id);
  if (track != nullptr) {
    track->setFormat(format);
  }
}

std::shared_ptr<AVTrackOutput> AVReaderOutput::track(int id, AVTrackOutput::SampleConsumer consumer) {
  return track(id, MediaType::kMediaTypeDefault, std::move(consumer));
}

void AVReaderOutput::seekMap(std::shared_ptr<AVSeekMap> seekMap) {
  std::lock_guard<std::mutex> lock(mutex_);
  seekMap_ = std::move(seekMap);
}

std::shared_ptr<AVTrackInfo> AVReaderOutput::getTrack(const int id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = tracks_.find(id);
  return it == tracks_.end() ? nullptr : it->second;
}

std::vector<int> AVReaderOutput::getTrackIds() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<int> ids;
  ids.reserve(tracks_.size());
  for (const auto& entry : tracks_) {
    ids.push_back(entry.first);
  }
  return ids;
}

int64_t AVReaderOutput::getDurationUs() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return seekMap_ != nullptr ? seekMap_->getDurationUs() : Definitions::kTimeUnset;
}

// ==================================================================================================
// AVReader
// ==================================================================================================

AVReader::AVReader(std::shared_ptr<AVDataSource> dataSource,
                   std::unique_ptr<AVExtractorsFactory> extractorsFactory, const int fileType)
  : dataSource_(std::move(dataSource)),
    extractorsFactory_(std::move(extractorsFactory)),
    fileType_(fileType) {
  // The output resolves the consumer of a track when the track is created, so that a consumer
  // registered before prepare() already receives the first samples of the track.
  output_ = std::make_shared<AVReaderOutput>([this](const int id) {
    std::lock_guard<std::mutex> lock(consumersMutex_);
    const auto it = consumers_.find(id);
    return it == consumers_.end() ? AVTrackOutput::SampleConsumer() : it->second;
  });
}

AVReader::~AVReader() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  // Release the resources in reverse order of acquisition, so that the extractor never outlives
  // the input it reads from.
  extractor_.reset();
  input_.reset();
  if (dataSource_ != nullptr) {
    dataSource_->close();
  }
  output_.reset();
}

bool AVReader::prepare() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  return prepareInternalLocked();
}

bool AVReader::prepareInternalLocked() {
  if (prepared_) {
    return true;
  }
  if (dataSource_ == nullptr) {
    lastError_ = "The data source is null.";
    return false;
  }
  if (!dataSource_->isOpen()) {
    if (dataSource_->getUri().empty()) {
      // The caller must open the data source when it has no uri, e.g. an in memory data source.
      lastError_ = "The data source is closed and has no uri to open.";
      return false;
    }
    if (!dataSource_->open(dataSource_->getUri())) {
      lastError_ = "Unable to open the data source.";
      return false;
    }
  }

  input_ = std::unique_ptr<AVDefaultExtractorInput>(new AVDefaultExtractorInput(dataSource_, 0));
  if (!sniffExtractorsLocked()) {
    return false;
  }

  extractor_->init(*output_);

  // Some containers only declare their tracks once the extractor read their header, therefore the
  // reading loop is primed until the first track is known. A movie box that follows a large mdat
  // (which is where an MP4 written by a typical encoder keeps it) is reached one bounded skip at a
  // time, so the priming is not limited to the per-read iteration count: 16 skips would stop inside
  // the mdat of every file larger than about 16 MB and report a media with no tracks.
  constexpr int kMaxPrepareIterations = 100000;
  for (int iteration = 0; iteration < kMaxPrepareIterations; ++iteration) {
    if (!output_->getTrackIds().empty()) {
      break;
    }
    const long positionBefore = position_;
    const AVExtractor::Result result = extractor_->read(*input_);
    if (extractor_->getPosition() > position_) {
      position_ = extractor_->getPosition();
    }
    if (input_->getPosition() > position_) {
      position_ = input_->getPosition();
    }
    if (result == AVExtractor::kResultEndOfInput || position_ == positionBefore) {
      break;
    }
  }

  durationUs_ = output_->getDurationUs();
  prepared_ = true;
  return true;
}

bool AVReader::sniffExtractorsLocked() {
  if (extractorsFactory_ == nullptr || input_ == nullptr) {
    lastError_ = "The extractor factory is null.";
    return false;
  }

  auto extractors = extractorsFactory_->createExtractors();
  if (extractors.empty()) {
    lastError_ = "The extractors factory returned no extractor.";
    return false;
  }

  // The probe is served from a private snapshot of the first bytes of the media, so that a
  // rejected extractor can never leave the real input in a moved state.
  const long length = dataSource_->getLength();
  size_t probeLength = kProbeBufferSize;
  if (length >= 0 && static_cast<size_t>(length) < probeLength) {
    probeLength = static_cast<size_t>(length);
  }
  std::vector<uint8_t> probe(probeLength);
  if (probeLength > 0) {
    probe.resize(readAtFully(*dataSource_, 0, probe.data(), probeLength));
  }

  auto probeSource = std::make_shared<AVByteArrayDataSource>(probe);
  probeSource->open("probe");
  AVDefaultExtractorInput probeInput(probeSource, 0);

  std::unique_ptr<AVExtractor> bestExtractor;
  int bestResult = AVExtractorsFactory::kSniffResultNone;
  std::string failureReason;
  for (auto& extractor : extractors) {
    if (extractor == nullptr) {
      continue;
    }
    probeInput.resetPosition(0, true);
    const int result = extractor->sniff(probeInput);
    if (result > bestResult) {
      bestResult = result;
      bestExtractor = std::move(extractor);
      failureReason.clear();
    } else if (result <= AVExtractorsFactory::kSniffResultNone && failureReason.empty()) {
      const std::string reason = extractor->getSniffFailureReason();
      if (!reason.empty()) {
        failureReason = reason;
      }
    }
  }

  if (bestExtractor == nullptr || bestResult <= AVExtractorsFactory::kSniffResultNone) {
    lastError_ = failureReason.empty() ? "No extractor recognized the media." : failureReason;
    return false;
  }

  extractor_ = std::move(bestExtractor);
  return true;
}

int AVReader::read() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (!prepared_ && !prepareInternalLocked()) {
    return kResultError;
  }
  if (seekPending_ && !resolveSeekLocked()) {
    return kResultError;
  }

  // The reader keeps reading while the extractor makes progress, and stops at the position that was
  // requested by a seek. This is what allows an extractor to keep consuming peeked or reread data
  // without the reader mistaking it for the end of the input.
  const long target = seekTarget_;
  bool anyProgress = false;
  for (int iteration = 0; iteration < kMaxIterationsPerRead; ++iteration) {
    const long positionBefore = position_;
    const int result = readInternalLocked();
    if (result == kResultError) {
      return kResultError;
    }
    if (position_ != positionBefore) {
      anyProgress = true;
    }
    if (target >= 0 && position_ >= target) {
      return kResultEndOfInput;
    }
    if (result == kResultEndOfInput) {
      // The extractor has nothing left at the current position.
      return anyProgress ? kResultEndOfInput : kResultEndOfInput;
    }
    if (position_ == positionBefore) {
      // The extractor neither consumed data nor reached the end of the input: stop rather than
      // spinning forever on a malformed container.
      lastError_ = "The extractor stopped making progress.";
      return kResultError;
    }
    if (target >= 0) {
      // A seek is still pending, keep reading until the requested position is reached.
      continue;
    }
    return kResultContinue;
  }
  return kResultContinue;
}

int AVReader::readInternalLocked() {
  if (extractor_ == nullptr || input_ == nullptr) {
    return kResultError;
  }
  const AVExtractor::Result result = extractor_->read(*input_);
  // The position of the reader is the position of the input, which it follows even when it moves
  // backwards: an extractor that locates the samples of a file whose movie box follows the media
  // data reads them from the position it rewound to, and holding on to the furthest position seen
  // would report such a reader as making no progress, which fails the reading of the file.
  position_ = input_->getPosition();
  return result == AVExtractor::kResultEndOfInput ? kResultEndOfInput : kResultContinue;
}

bool AVReader::resolveSeekLocked() {
  if (!seekPending_) {
    return true;
  }
  if (input_ == nullptr || extractor_ == nullptr) {
    return false;
  }

  long target = pendingPosition_;
  bool resolvedByTime = false;
  if (target < 0) {
    const auto seekMap = output_->getSeekMap();
    AVSeekMap::SeekPoint seekPoint;
    if (seekMap != nullptr && seekMap->isSeekable() &&
        seekMap->getSeekPoint(pendingTimeUs_, seekPoint)) {
      target = seekPoint.position;
      resolvedByTime = true;
    } else {
      target = 0;
    }
  }
  if (target < 0) {
    target = 0;
  }

  if (!extractor_->seek(target, pendingTimeUs_)) {
    lastError_ = "The extractor refused the seek.";
    return false;
  }
  if (!input_->resetPosition(target, true)) {
    lastError_ = "Unable to seek in the input.";
    return false;
  }

  position_ = target;
  endOfInputReached_ = false;
  tracksEnded_ = false;
  seekPending_ = false;
  // A seek that the seek map could not resolve restarts the container at its start. The extractor
  // is then responsible for skipping to the requested time, and the reader must let it read past
  // the start rather than treat the restart position as the end of the requested range. A restart
  // at zero is handled the same way, because a container that maps every time to zero still has to
  // read its single sample, and a byte position that was resolved is still reached exactly.
  seekTarget_ = (pendingPosition_ > 0 || (resolvedByTime && target > 0)) ? target : -1;
  return true;
}

bool AVReader::seek(const long position, const int64_t timeUs) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (position < 0 && timeUs < 0) {
    return false;
  }
  if (position >= 0 && dataSource_ != nullptr) {
    const long length = dataSource_->getLength();
    if (length >= 0 && position > length) {
      return false;
    }
  }
  pendingPosition_ = position;
  pendingTimeUs_ = timeUs;
  seekPending_ = true;
  seekTarget_ = -1;
  endOfInputReached_ = false;
  return true;
}

void AVReader::endTracks() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (output_ == nullptr || tracksEnded_) {
    return;
  }
  for (const int trackId : output_->getTrackIds()) {
    const auto track = output_->getTrack(trackId);
    if (track == nullptr) {
      continue;
    }
    AVSample endOfStream;
    endOfStream.trackIndex = trackId;
    endOfStream.flags = static_cast<uint32_t>(AVSample::kFlagEndOfStream);
    track->consume(endOfStream);
  }
  tracksEnded_ = true;
}

long AVReader::getPosition() const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  return position_;
}

int64_t AVReader::getDurationUs() const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  // The duration is read from the seek map, which an extractor may publish once it read the header
  // of the container, i.e. after prepare() returned.
  const int64_t duration = output_ != nullptr ? output_->getDurationUs() : Definitions::kTimeUnset;
  if (duration != Definitions::kTimeUnset) {
    return duration;
  }
  return durationUs_;
}

std::shared_ptr<AVSeekMap> AVReader::getSeekMap() const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  return output_ != nullptr ? output_->getSeekMap() : nullptr;
}

std::shared_ptr<AVTrackInfo> AVReader::getTrack(const int id) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  return output_ != nullptr ? output_->getTrack(id) : nullptr;
}

std::vector<int> AVReader::getTrackIds() const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  return output_ != nullptr ? output_->getTrackIds() : std::vector<int>();
}

std::shared_ptr<MediaFormat> AVReader::getTrackFormat(const int id) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  const auto track = output_ != nullptr ? output_->getTrack(id) : nullptr;
  return track != nullptr ? track->getFormat() : std::shared_ptr<MediaFormat>();
}

void AVReader::setTrackConsumer(const int id, AVTrackOutput::SampleConsumer consumer) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  {
    std::lock_guard<std::mutex> consumersLock(consumersMutex_);
    consumers_[id] = consumer;
  }
  if (output_ != nullptr) {
    const auto track = output_->getTrack(id);
    if (track != nullptr) {
      track->setConsumer(std::move(consumer));
    }
  }
}

std::string AVReader::getLastError() const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  return lastError_;
}

int AVReader::getFileType() const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  return fileType_;
}

} // namespace coral
