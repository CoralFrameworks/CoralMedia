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

#include "Mp4Muxer.h"

#include "Mp4Boxes.h"

#include <minilog/log.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <utility>

namespace coral {
namespace muxer {
namespace mp4 {
namespace {

constexpr int64_t kDefaultMovieTimescale = 1000;
constexpr int64_t kMaxPendingBytes = 32 * 1024 * 1024;
constexpr size_t kMaxPendingSamples = 8192;
constexpr uint32_t kKeyFrameFlag = static_cast<uint32_t>(SampleFlag::kKeyFrame);

std::string makeTempPath(const std::string& suffix) {
  std::error_code error;
  const auto directory = std::filesystem::current_path(error);
  if (error) {
    return {};
  }
  static std::atomic<uint64_t> counter{0};
  const auto stamp = static_cast<uint64_t>(
    std::chrono::steady_clock::now().time_since_epoch().count());
  return (directory / ("coral-muxer-" + std::to_string(stamp) + "-" +
                       std::to_string(counter.fetch_add(1)) + suffix))
    .string();
}

std::string makeSpoolPath() {
  return makeTempPath(".mdat");
}

std::vector<uint8_t> u64Bytes(const uint64_t value) {
  std::vector<uint8_t> data(8);
  for (int i = 0; i < 8; ++i) {
    data[i] = static_cast<uint8_t>(value >> (56 - i * 8));
  }
  return data;
}

} // namespace

Mp4Muxer::Mp4Muxer(std::unique_ptr<MuxerOutput> output, MuxerOptions options) noexcept
        : mOutput(std::move(output)),
          mOptions(options) {}

Mp4Muxer::~Mp4Muxer() {
  if (mStatus == Status::kWriting) {
    cancel();
  }
}

std::unique_ptr<Mp4Muxer> Mp4Muxer::create(const std::string& url, const MuxerOptions& options, Error* error) {
  std::string openError;
  auto output = openFileOutput(url, &openError);
  if (output == nullptr) {
    if (error != nullptr) {
      error->code = 3;
      error->message = openError;
    }
    return nullptr;
  }
  auto muxer = create(std::move(output), options, error);
  return muxer;
}

std::unique_ptr<Mp4Muxer> Mp4Muxer::create(std::unique_ptr<MuxerOutput> output,
  const MuxerOptions& options, Error* error) {
  if (output == nullptr) {
    if (error != nullptr) {
      error->code = 3;
      error->message = "the output is null";
    }
    return nullptr;
  }
  if (options.fragmentDurationUs > 0) {
    if (error != nullptr) {
      error->code = 2;
      error->message = "fragmented MP4 requires Container::kFragmentedMp4";
    }
    return nullptr;
  }

  MuxerOptions normalized = options;
  if (normalized.movieTimescale <= 0) {
    normalized.movieTimescale = kDefaultMovieTimescale;
  }
  if (normalized.interleaveDurationUs < 0) {
    normalized.interleaveDurationUs = 0;
  }
  if (!output->isSeekable() && !normalized.fastStart) {
    if (error != nullptr) {
      error->code = 3;
      error->message = "a non-seekable output requires fast-start";
    }
    return nullptr;
  }

  auto muxer = std::unique_ptr<Mp4Muxer>(new Mp4Muxer(std::move(output), normalized));
  const bool needsSpool =
    normalized.fastStart && (!muxer->mOutput->isSeekable() || normalized.auxiliaryTracksInAxte);
  if (needsSpool) {
    muxer->mSpoolMode = true;
    muxer->mSpoolPath = makeSpoolPath();
    if (muxer->mSpoolPath.empty()) {
      if (error != nullptr) {
        error->code = 4;
        error->message = "cannot create a fast-start spool path";
      }
      muxer->mOutput->remove();
      return nullptr;
    }
    std::string spoolError;
    muxer->mSpool = openFileOutput(muxer->mSpoolPath, &spoolError);
    if (muxer->mSpool == nullptr) {
      if (error != nullptr) {
        error->code = 4;
        error->message = spoolError;
      }
      muxer->mOutput->remove();
      return nullptr;
    }
  }

  std::string headerError;
  if (!muxer->writeFileHeader(&headerError)) {
    if (error != nullptr) {
      error->code = 4;
      error->message = headerError;
    }
    muxer->mOutput->remove();
    if (muxer->mSpool != nullptr) {
      muxer->mSpool->remove();
    }
    return nullptr;
  }
  muxer->mStatus = Status::kWriting;
  return muxer;
}

bool Mp4Muxer::writeFileHeader(std::string* error) {
  BoxWriter ftyp;
  writeFtyp(ftyp);
  if (!writeBytes(ftyp.data().data(), ftyp.size())) {
    *error = "cannot write the ftyp box";
    return false;
  }
  if (mSpoolMode) {
    mFtypSize = static_cast<int64_t>(ftyp.size());
    BoxWriter mdat;
    mdat.u32(1);
    mdat.fourcc("mdat");
    mdat.u64(0);
    mMdatStart = 0;
    if (!mSpool->write(mdat.data().data(), mdat.size())) {
      *error = "cannot write the spooled mdat header";
      return false;
    }
    mMdatEnd = mSpool->position();
    return true;
  }

  // A 64-bit mdat header avoids patching the file size twice. The size is filled in at finish.
  BoxWriter mdat;
  mdat.u32(1);
  mdat.fourcc("mdat");
  mdat.u64(0);
  mMdatStart = mOutput->position();
  if (!writeBytes(mdat.data().data(), mdat.size())) {
    *error = "cannot write the mdat header";
    return false;
  }
  mMdatEnd = mOutput->position();
  return true;
}

int Mp4Muxer::addTrack(const std::shared_ptr<MediaFormat>& format, const TrackOptions& options) {
  if (mStatus != Status::kWriting) {
    fail("tracks can be added only while the muxer is writing");
    return -1;
  }
  if (format == nullptr) {
    fail("the track format is null");
    return -1;
  }
  if (format->isDerivedTrack()) {
    // The samples of a derived track (e.g. captions extracted from the SEI of a video track) are
    // already written with its source track; writing them again would duplicate the track.
    reject("the track is derived from another track and is not muxed: " + format->getMimeType());
    return -1;
  }

  const Codec codec = codecFor(*format);
  if (codec == Codec::kUnknown) {
    reject("the MP4 muxer does not support this codec: " + format->getMimeType());
    return -1;
  }
  if (requiresCodecSpecificData(codec) && (format->csdList().empty() || format->csdList().front().empty())) {
    reject("the track has no codec-specific data");
    return -1;
  }

  int trackId = options.trackId;
  if (trackId < 0) {
    trackId = mNextTrackId;
  } else if (trackId == 0) {
    fail("the track id must be positive");
    return -1;
  }
  if (std::any_of(mTracks.begin(), mTracks.end(), [trackId](const Track& track) { return track.id == trackId; }) ||
      std::any_of(mExternalTracks.begin(), mExternalTracks.end(),
        [trackId](const ExternalTrack& track) { return track.id == trackId; })) {
    fail("a track with this id already exists");
    return -1;
  }
  mNextTrackId = std::max(mNextTrackId, trackId + 1);

  if (mOptions.auxiliaryTracksInAxte && options.auxiliary) {
    if (mAxteMuxer == nullptr) {
      mAxtePath = makeTempPath(".axte.tmp");
      MuxerOptions cacheOptions = mOptions;
      cacheOptions.auxiliaryTracksInAxte = false;
      cacheOptions.fastStart = false;
      Error error;
      mAxteMuxer = Mp4Muxer::create(mAxtePath, cacheOptions, &error);
      if (mAxteMuxer == nullptr) {
        fail("cannot create the MP4-AT auxiliary file: " + error.message);
        return -1;
      }
    }
    TrackOptions cacheTrackOptions = options;
    cacheTrackOptions.auxiliary = false;
    cacheTrackOptions.trackId = -1;
    const int cacheTrackId = mAxteMuxer->addTrack(format, cacheTrackOptions);
    if (cacheTrackId < 0) {
      fail("cannot add the auxiliary track to the MP4-AT file");
      return -1;
    }
    mExternalTracks.push_back({ trackId, cacheTrackId, options.auxiliaryTrackType });
    return trackId;
  }

  Track track(chooseTrackTimescale(codec, *format));
  track.format = format;
  track.options = options;
  track.id = trackId;
  track.codec = codec;
  if (track.options.language.empty()) {
    track.options.language = format->getLanguage();
  }
  mTracks.push_back(std::move(track));
  return mTracks.back().id;
}

bool Mp4Muxer::writeSample(const int trackId, const uint8_t* data, const size_t size, const SampleInfo& info) {
  if (mStatus != Status::kWriting) {
    return fail("the muxer is not writing");
  }
  if (ExternalTrack* external = findExternalTrack(trackId); external != nullptr) {
    return mAxteMuxer->writeSample(external->cacheTrackId, data, size, info);
  }
  Track* track = findTrack(trackId);
  if (track == nullptr) {
    return fail("the sample references an unknown track");
  }
  if (data == nullptr && size > 0) {
    return fail("the sample data is null");
  }
  if (size > std::numeric_limits<uint32_t>::max()) {
    return fail("the sample is larger than 4 GB");
  }
  if (size == 0) {
    return true;
  }

  if (isVideoCodec(track->codec)) {
    const bool keyFrame = (info.flags & kKeyFrameFlag) != 0;
    if (!track->hadKeyFrame) {
      if (!keyFrame) {
        // A video track must start with a key frame; samples before it cannot be decoded.
        return true;
      }
      track->hadKeyFrame = true;
    }
  }

  std::vector<uint8_t> converted;
  const uint8_t* payload = data;
  size_t payloadSize = size;
  if ((track->codec == Codec::kH264 || track->codec == Codec::kHevc ||
       track->codec == Codec::kDolbyVision) && startsWithStartCode(data, size)) {
    converted = annexBToLengthPrefixed(data, size);
    payload = converted.data();
    payloadSize = converted.size();
  } else if (isCaptionCodec(track->codec)) {
    converted = wrapCaptionSample(data, size);
    payload = converted.data();
    payloadSize = converted.size();
  }
  if (payloadSize > std::numeric_limits<uint32_t>::max()) {
    return fail("the sample is larger than 4 GB after Annex B conversion");
  }

  PendingSample sample;
  sample.data.assign(payload, payload + payloadSize);
  sample.presentationTimeUs = info.presentationTimeUs;
  sample.durationUs = info.durationUs;
  sample.flags = info.flags;

  if (track->pending.empty()) {
    track->pendingMinPts = info.presentationTimeUs;
    track->pendingMaxPts = info.presentationTimeUs;
  } else {
    track->pendingMinPts = std::min(track->pendingMinPts, info.presentationTimeUs);
    track->pendingMaxPts = std::max(track->pendingMaxPts, info.presentationTimeUs);
  }
  track->pendingBytes += static_cast<int64_t>(payloadSize);
  track->pending.push_back(std::move(sample));

  return maybeFlushTrack(*track);
}

bool Mp4Muxer::addMetadata(const std::shared_ptr<Metadata>& metadata) {
  if (mStatus != Status::kWriting) {
    return fail("metadata can be added only while the muxer is writing");
  }
  mMetadata.add(metadata);
  return true;
}

bool Mp4Muxer::addMetadataEntry(const std::string& key, std::vector<uint8_t> value,
  const MdtaMetadataEntry::ValueType type) {
  mMetadata.setEntry(std::make_shared<MdtaMetadataEntry>(key, std::move(value), type));
  return true;
}

bool Mp4Muxer::maybeFlushTrack(Track& track) {
  if (track.pending.empty()) {
    return true;
  }
  const int64_t span = track.pendingMaxPts - track.pendingMinPts;
  const bool overWindow = mOptions.interleaveDurationUs <= 0 || span >= mOptions.interleaveDurationUs;
  const bool overBytes = track.pendingBytes >= kMaxPendingBytes;
  const bool overSamples = track.pending.size() >= kMaxPendingSamples;
  if (overWindow || overBytes || overSamples) {
    return flushTrack(track);
  }
  return true;
}

bool Mp4Muxer::flushTrack(Track& track) {
  if (track.pending.empty()) {
    return true;
  }
  MuxerOutput* output = mSpoolMode ? mSpool.get() : mOutput.get();
  if (output == nullptr) {
    return fail("the sample output is not available");
  }

  int64_t chunkDurationUs = 0;
  for (size_t i = 0; i < track.pending.size(); ++i) {
    const bool startChunk =
            i == 0 || (mOptions.interleaveDurationUs > 0 && chunkDurationUs >= mOptions.interleaveDurationUs);
    if (startChunk) {
      track.table.addChunk(output->position());
      chunkDurationUs = 0;
    } else {
      track.table.incrementLastChunkSampleCount();
    }

    const PendingSample& sample = track.pending[i];
    if (!output->write(sample.data.data(), sample.data.size())) {
      return fail("cannot write a sample to the output");
    }
    track.table.addSample(static_cast<uint32_t>(sample.data.size()), sample.presentationTimeUs, sample.durationUs,
            sample.flags);

    int64_t durationUs = sample.durationUs;
    if (durationUs <= 0 && i + 1 < track.pending.size()) {
      durationUs = std::max<int64_t>(0, track.pending[i + 1].presentationTimeUs - sample.presentationTimeUs);
    }
    chunkDurationUs += durationUs;
  }

  mMdatEnd = output->position();
  track.pending.clear();
  track.pendingBytes = 0;
  track.pendingMinPts = 0;
  track.pendingMaxPts = 0;
  return true;
}

bool Mp4Muxer::flushPendingTracks() {
  std::vector<Track*> tracks;
  for (auto& track: mTracks) {
    if (!track.pending.empty()) {
      tracks.push_back(&track);
    }
  }
  std::sort(tracks.begin(), tracks.end(),
          [](const Track* left, const Track* right) { return left->pendingMinPts < right->pendingMinPts; });
  for (Track* track: tracks) {
    if (!flushTrack(*track)) {
      return false;
    }
  }
  return true;
}

bool Mp4Muxer::finalizePrimarySamples() {
  if (!flushPendingTracks()) {
    return false;
  }
  bool hasSamples = false;
  for (const auto& track: mTracks) {
    hasSamples = hasSamples || !track.table.empty();
  }
  if (!hasSamples) {
    return fail("the primary movie has no samples");
  }

  mGlobalFirstPts = std::numeric_limits<int64_t>::max();
  for (const auto& track: mTracks) {
    if (!track.table.empty()) {
      mGlobalFirstPts = std::min(mGlobalFirstPts, track.table.firstPresentationTimeUs());
    }
  }
  if (!mOptions.writeNegativeTimestampsInEditList && mGlobalFirstPts != 0) {
    for (auto& track: mTracks) {
      if (!track.table.empty()) {
        track.table.shiftPresentationTimes(-mGlobalFirstPts);
      }
    }
    mGlobalFirstPts = 0;
  }
  for (auto& track: mTracks) {
    if (!track.table.empty()) {
      track.table.finalize(defaultSampleDurationUs(track.codec, *track.format));
    }
  }

  const auto mdatSize = static_cast<uint64_t>(mMdatEnd - mMdatStart);
  std::array<uint8_t, 8> sizeBytes{};
  for (size_t i = 0; i < sizeBytes.size(); ++i) {
    sizeBytes[i] = static_cast<uint8_t>(mdatSize >> (56 - i * 8));
  }
  MuxerOutput* output = mSpoolMode ? mSpool.get() : mOutput.get();
  if (output == nullptr) {
    return fail("the sample output is not available");
  }
  if (!output->seek(mMdatStart + 8) || !output->write(sizeBytes.data(), sizeBytes.size())) {
    return fail(mSpoolMode ? "cannot patch the spooled mdat size" : "cannot patch the mdat size");
  }
  return true;
}

bool Mp4Muxer::finish() {
  if (mStatus != Status::kWriting) {
    return fail("the muxer is not writing");
  }
  if (mSpoolMode) {
    if (!mExternalTracks.empty()) {
      return finishSpooledFastStartWithAuxiliaryTracks();
    }
    return finishSpooledFastStart();
  }
  if (!mExternalTracks.empty()) {
    return finishWithAuxiliaryTracks();
  }
  if (!finalizePrimarySamples()) {
    return false;
  }

  std::vector<uint8_t> moovBytes;
  if (!writeMoovBytes(&moovBytes)) {
    return false;
  }

  if (mOptions.fastStart) {
    const int64_t shift = static_cast<int64_t>(moovBytes.size());
    if (!mOutput->copyBytes(mMdatStart + shift, mMdatStart, mMdatEnd - mMdatStart)) {
      return fail("cannot move the mdat box for fast-start output");
    }
    for (auto& track: mTracks) {
      track.table.shiftChunkOffsets(shift);
    }
    if (!writeMoovBytes(&moovBytes)) {
      return false;
    }
    if (static_cast<int64_t>(moovBytes.size()) != shift) {
      return fail("the moov box changed size while moving the media data");
    }
    if (!mOutput->seek(mMdatStart) || !writeBytes(moovBytes.data(), moovBytes.size())) {
      return fail("cannot write the moov box at the start");
    }
  } else {
    if (!mOutput->seek(mMdatEnd) || !writeBytes(moovBytes.data(), moovBytes.size())) {
      return fail("cannot write the moov box");
    }
  }

  if (!mOutput->close()) {
    return fail("cannot close the output file");
  }
  mStatus = Status::kCompleted;
  return true;
}

bool Mp4Muxer::finishSpooledFastStart() {
  if (!finalizePrimarySamples()) {
    return false;
  }

  std::vector<uint8_t> moovBytes;
  if (!writeMoovBytes(&moovBytes)) {
    return false;
  }
  const int64_t shift = mFtypSize + static_cast<int64_t>(moovBytes.size());
  for (auto& track: mTracks) {
    track.table.shiftChunkOffsets(shift);
  }
  const auto moovSize = moovBytes.size();
  if (!writeMoovBytes(&moovBytes) || moovBytes.size() != moovSize) {
    return fail("the moov box changed size while spooling the mdat");
  }
  if (!writeBytes(moovBytes.data(), moovBytes.size())) {
    return fail("cannot write the moov box");
  }

  if (!copySpoolToOutput()) {
    return false;
  }
  if (!mOutput->close()) {
    return fail("cannot close the output file");
  }
  std::error_code error;
  std::filesystem::remove(mSpoolPath, error);
  mSpool.reset();
  mStatus = Status::kCompleted;
  return true;
}

bool Mp4Muxer::copySpoolToOutput() {
  if (!mSpool->close()) {
    return fail("cannot close the spooled mdat");
  }
  std::ifstream spool(mSpoolPath, std::ios::binary);
  if (!spool.is_open()) {
    return fail("cannot open the spooled mdat");
  }
  std::vector<uint8_t> buffer(1 << 20);
  while (spool.read(reinterpret_cast<char*>(buffer.data()),
           static_cast<std::streamsize>(buffer.size())) ||
         spool.gcount() > 0) {
    const auto read = static_cast<size_t>(spool.gcount());
    if (!writeBytes(buffer.data(), read)) {
      return fail("cannot copy the spooled mdat");
    }
    if (read < buffer.size()) {
      break;
    }
  }
  spool.close();
  return true;
}

bool Mp4Muxer::finishSpooledFastStartWithAuxiliaryTracks() {
  if (!finalizePrimarySamples()) {
    return false;
  }
  uint64_t axteSize = 0;
  if (!finishAuxiliaryMovie(&axteSize)) {
    return false;
  }
  addMetadataEntry("auxiliary.tracks.length", u64Bytes(axteSize),
    MdtaMetadataEntry::ValueType::kUnsignedInt64);
  addMetadataEntry("auxiliary.tracks.offset", u64Bytes(0),
    MdtaMetadataEntry::ValueType::kUnsignedInt64);

  std::vector<uint8_t> moovBytes;
  if (!writeMoovBytes(&moovBytes)) {
    return false;
  }
  const auto moovSize = moovBytes.size();
  const uint64_t mdatSize = static_cast<uint64_t>(mMdatEnd - mMdatStart);
  const uint64_t axteOffset = static_cast<uint64_t>(mFtypSize) + moovSize + mdatSize;
  mMetadata.setEntry(std::make_shared<MdtaMetadataEntry>("auxiliary.tracks.offset",
    u64Bytes(axteOffset), MdtaMetadataEntry::ValueType::kUnsignedInt64));
  const int64_t shift = mFtypSize + static_cast<int64_t>(moovSize);
  for (auto& track: mTracks) {
    track.table.shiftChunkOffsets(shift);
  }
  if (!writeMoovBytes(&moovBytes) || moovBytes.size() != moovSize) {
    return fail("the primary moov size changed while spooling the MP4-AT output");
  }
  if (!writeBytes(moovBytes.data(), moovBytes.size())) {
    return fail("cannot write the primary moov box");
  }
  if (!copySpoolToOutput()) {
    return false;
  }
  if (!appendAuxiliaryMovie(axteSize)) {
    return false;
  }
  if (!mOutput->close()) {
    return fail("cannot close the MP4-AT output");
  }
  std::error_code error;
  std::filesystem::remove(mSpoolPath, error);
  mSpool.reset();
  mStatus = Status::kCompleted;
  return true;
}

bool Mp4Muxer::finishWithAuxiliaryTracks() {
  if (!finalizePrimarySamples()) {
    return false;
  }
  uint64_t axteSize = 0;
  if (!finishAuxiliaryMovie(&axteSize)) {
    return false;
  }
  addMetadataEntry("auxiliary.tracks.length", u64Bytes(axteSize),
    MdtaMetadataEntry::ValueType::kUnsignedInt64);
  addMetadataEntry("auxiliary.tracks.offset", u64Bytes(0),
    MdtaMetadataEntry::ValueType::kUnsignedInt64);

  std::vector<uint8_t> moovBytes;
  if (!writeMoovBytes(&moovBytes)) {
    return false;
  }
  const uint64_t axteOffset = static_cast<uint64_t>(mMdatEnd) + moovBytes.size();
  mMetadata.setEntry(std::make_shared<MdtaMetadataEntry>("auxiliary.tracks.offset",
    u64Bytes(axteOffset), MdtaMetadataEntry::ValueType::kUnsignedInt64));
  const auto moovSize = moovBytes.size();
  if (!writeMoovBytes(&moovBytes) || moovBytes.size() != moovSize) {
    return fail("the primary moov size changed while adding MP4-AT metadata");
  }
  if (!mOutput->seek(mMdatEnd) || !writeBytes(moovBytes.data(), moovBytes.size())) {
    return fail("cannot write the primary moov box");
  }
  if (!appendAuxiliaryMovie(axteSize)) {
    return false;
  }
  if (!mOutput->close()) {
    return fail("cannot close the MP4-AT output");
  }
  mStatus = Status::kCompleted;
  return true;
}

bool Mp4Muxer::finishAuxiliaryMovie(uint64_t* axteSize) {
  if (mAxteMuxer == nullptr) {
    return fail("the MP4-AT auxiliary writer was not created");
  }
  std::vector<std::shared_ptr<Metadata::Entry>> auxiliaryMetadata;
  auxiliaryMetadata.push_back(std::make_shared<Mp4TimestampData>(mMetadata.creationTime(),
    mMetadata.modificationTime()));
  auxiliaryMetadata.push_back(std::make_shared<MdtaMetadataEntry>("auxiliary.tracks.interleaved",
    std::vector<uint8_t>{ 0 }, MdtaMetadataEntry::ValueType::kUnsignedInt8));
  std::vector<uint8_t> map;
  map.push_back(1);
  map.push_back(static_cast<uint8_t>(std::min<size_t>(mExternalTracks.size(), 255)));
  for (const auto& track: mExternalTracks) {
    map.push_back(static_cast<uint8_t>(std::clamp(track.type, 0, 3)));
  }
  auxiliaryMetadata.push_back(std::make_shared<MdtaMetadataEntry>("auxiliary.tracks.map", map,
    MdtaMetadataEntry::ValueType::kBinary));
  if (!mAxteMuxer->addMetadata(std::make_shared<Metadata>(auxiliaryMetadata)) ||
      !mAxteMuxer->finish()) {
    return fail("cannot finish the MP4-AT auxiliary movie: " + mAxteMuxer->getError().message);
  }

  std::error_code fileError;
  const auto cacheSize = std::filesystem::file_size(mAxtePath, fileError);
  if (fileError) {
    return fail("cannot measure the MP4-AT auxiliary movie");
  }
  *axteSize = 16 + static_cast<uint64_t>(cacheSize);
  return true;
}

bool Mp4Muxer::appendAuxiliaryMovie(const uint64_t axteSize) {
  BoxWriter axte;
  axte.u32(1);
  axte.fourcc("axte");
  axte.u64(axteSize);
  if (!writeBytes(axte.data().data(), axte.size())) {
    return fail("cannot write the MP4-AT axte header");
  }
  std::ifstream cache(mAxtePath, std::ios::binary);
  if (!cache.is_open()) {
    return fail("cannot open the MP4-AT auxiliary movie");
  }
  std::vector<uint8_t> buffer(1 << 20);
  while (cache.read(reinterpret_cast<char*>(buffer.data()),
           static_cast<std::streamsize>(buffer.size())) ||
         cache.gcount() > 0) {
    const auto read = static_cast<size_t>(cache.gcount());
    if (!writeBytes(buffer.data(), read)) {
      return fail("cannot append the MP4-AT auxiliary movie");
    }
    if (read < buffer.size()) {
      break;
    }
  }
  cache.close();
  std::error_code error;
  std::filesystem::remove(mAxtePath, error);
  mAxteMuxer.reset();
  return true;
}

bool Mp4Muxer::writeMoovBytes(std::vector<uint8_t>* bytes) {
  BoxWriter moov;
  writeMoov(moov);
  BoxWriter xmp;
  mMetadata.writeXmp(xmp);

  bytes->clear();
  bytes->reserve(moov.size() + xmp.size());
  bytes->insert(bytes->end(), moov.data().begin(), moov.data().end());
  bytes->insert(bytes->end(), xmp.data().begin(), xmp.data().end());
  return true;
}

void Mp4Muxer::writeMoov(BoxWriter& writer) {
  const size_t moov = writer.beginBox("moov");

  int64_t movieDurationUs = 0;
  for (const auto& track: mTracks) {
    if (track.table.empty()) {
      continue;
    }
    int64_t firstPts = track.table.firstPresentationTimeUs();
    if (mGlobalFirstPts > 0) {
      firstPts -= mGlobalFirstPts;
    }
    const int64_t presentationDurationUs =
            firstPts < 0 ? std::max<int64_t>(0, track.table.durationUs() + firstPts) : track.table.durationUs();
    const int64_t delayUs = firstPts > 0 ? firstPts : 0;
    movieDurationUs = std::max(movieDurationUs, delayUs + presentationDurationUs);
  }

  writeMvhd(writer, movieDurationUs, mOptions.movieTimescale, mNextTrackId, mMetadata.creationTime(),
          mMetadata.modificationTime());
  mMetadata.writeUdta(writer);
  for (const auto& track: mTracks) {
    if (!track.table.empty()) {
      writeTrack(writer, track, mGlobalFirstPts, mOptions.movieTimescale);
    }
  }
  writer.endBox(moov);
}

void Mp4Muxer::writeTrack(BoxWriter& writer, const Track& track, const int64_t globalFirstPts,
        const int64_t movieTimescale) {
  const int64_t durationUs = track.table.durationUs();
  int64_t firstPts = track.table.firstPresentationTimeUs();
  if (globalFirstPts > 0) {
    firstPts -= globalFirstPts;
  }
  const int64_t presentationDurationUs = firstPts < 0 ? std::max<int64_t>(0, durationUs + firstPts) : durationUs;
  const int64_t delayUs = firstPts > 0 ? firstPts : 0;
  const int64_t trackDurationUs = delayUs + presentationDurationUs;
  const int rotation = mMetadata.orientation(track.format->getRotation());
  const bool audio = isAudioCodec(track.codec);
  const bool text = isTextCodec(track.codec);
  const int width = audio || text ? 0 : track.format->getWidth();
  const int height = audio || text ? 0 : track.format->getHeight();

  const size_t trak = writer.beginBox("trak");
  writeTkhd(writer, track.id, trackDurationUs, movieTimescale, mMetadata.creationTime(), mMetadata.modificationTime(),
          rotation, track.options.alternateGroup, track.options.enabled, audio, width, height);

  if (track.options.auxiliary) {
    // An auxiliary track describes a primary track. Prefer the first primary video track, and fall
    // back to the first primary track of any kind.
    int referencedTrackId = -1;
    for (const auto& other: mTracks) {
      if (other.id != track.id && !other.options.auxiliary && isVideoCodec(other.codec)) {
        referencedTrackId = other.id;
        break;
      }
    }
    if (referencedTrackId < 0) {
      for (const auto& other: mTracks) {
        if (other.id != track.id && !other.options.auxiliary) {
          referencedTrackId = other.id;
          break;
        }
      }
    }
    if (referencedTrackId >= 0) {
      const size_t tref = writer.beginBox("tref");
      const size_t cdsc = writer.beginBox("cdsc");
      writer.u32(static_cast<uint32_t>(referencedTrackId));
      writer.endBox(cdsc);
      writer.endBox(tref);
    }
  }

  if (firstPts != 0) {
    const size_t edts = writer.beginBox("edts");
    const size_t elst = writer.beginFullBox("elst", 1, 0);
    if (firstPts > 0) {
      writer.u32(2);
      // An empty edit delays the start of the track.
      writer.u64(static_cast<uint64_t>(scaleUsToTicks(delayUs, movieTimescale)));
      writer.u64(std::numeric_limits<uint64_t>::max());
      writer.u16(1);
      writer.u16(0);
      writer.u64(static_cast<uint64_t>(scaleUsToTicks(presentationDurationUs, movieTimescale)));
      writer.u64(0);
      writer.u16(1);
      writer.u16(0);
    } else {
      // One edit skips the part of the media that precedes the presentation timeline.
      writer.u32(1);
      writer.u64(static_cast<uint64_t>(scaleUsToTicks(presentationDurationUs, movieTimescale)));
      writer.u64(static_cast<uint64_t>(scaleUsToTicks(-firstPts, track.timescale)));
      writer.u16(1);
      writer.u16(0);
    }
    writer.endBox(elst);
    writer.endBox(edts);
  }

  const size_t mdia = writer.beginBox("mdia");
  writeMdhd(writer, track.table.durationTicks(), track.timescale, mMetadata.creationTime(),
          mMetadata.modificationTime(), track.options.language);
  writeHdlr(writer, audio ? "soun" : text ? "text" : "vide",
          audio ? "SoundHandler" : text ? "TextHandler" : "VideoHandler");

  const size_t minf = writer.beginBox("minf");
  if (audio) {
    const size_t smhd = writer.beginFullBox("smhd", 0, 0);
    writer.u16(0);
    writer.u16(0);
    writer.endBox(smhd);
  } else if (text) {
    const size_t nmhd = writer.beginFullBox("nmhd", 0, 0);
    writer.endBox(nmhd);
  } else {
    const size_t vmhd = writer.beginFullBox("vmhd", 0, 1);
    writer.u16(0);
    writer.u16(0);
    writer.u16(0);
    writer.u16(0);
    writer.endBox(vmhd);
  }
  writeDinf(writer);

  const size_t stbl = writer.beginBox("stbl");
  const size_t stsd = writer.beginFullBox("stsd", 0, 0);
  writer.u32(1);
  writeSampleEntry(writer, track.codec, *track.format);
  writer.endBox(stsd);

  track.table.writeStts(writer);
  if (!audio && !text && track.table.hasCompositionOffsets()) {
    track.table.writeCtts(writer);
  }
  if (!audio && !text && !track.table.allKeyFrames()) {
    track.table.writeStss(writer);
  }
  track.table.writeStsz(writer);
  track.table.writeStsc(writer);
  track.table.writeChunkOffsets(writer, mOptions.fastStart);
  writer.endBox(stbl);
  writer.endBox(minf);
  writer.endBox(mdia);
  writer.endBox(trak);
}

void Mp4Muxer::cancel() {
  if (mStatus == Status::kCompleted || mStatus == Status::kCancelled) {
    return;
  }
  if (mAxteMuxer != nullptr) {
    mAxteMuxer->cancel();
    mAxteMuxer.reset();
  }
  if (!mAxtePath.empty()) {
    std::error_code error;
    std::filesystem::remove(mAxtePath, error);
  }
  if (mSpool != nullptr) {
    mSpool->remove();
    mSpool.reset();
  }
  if (!mSpoolPath.empty()) {
    std::error_code error;
    std::filesystem::remove(mSpoolPath, error);
  }
  if (mOutput != nullptr) {
    mOutput->remove();
  }
  mStatus = Status::kCancelled;
}

bool Mp4Muxer::writeBytes(const uint8_t* data, const size_t size) {
  return size == 0 || (mOutput != nullptr && mOutput->write(data, size));
}

bool Mp4Muxer::fail(std::string message) {
  mError.code = 1;
  mError.message = std::move(message);
  mStatus = Status::kFailed;
  MINILOG_ERROR("mp4 muxer: {}", mError.message);
  return false;
}

bool Mp4Muxer::reject(std::string message) {
  // A track the container cannot represent is not an error of the muxer: the caller skips it and
  // keeps writing the other tracks, so the status stays kWriting.
  mError.code = 1;
  mError.message = std::move(message);
  MINILOG_WARN("mp4 muxer: {}", mError.message);
  return false;
}

Mp4Muxer::Track* Mp4Muxer::findTrack(const int trackId) {
  for (auto& track: mTracks) {
    if (track.id == trackId) {
      return &track;
    }
  }
  return nullptr;
}

const Mp4Muxer::Track* Mp4Muxer::findTrack(const int trackId) const {
  for (const auto& track: mTracks) {
    if (track.id == trackId) {
      return &track;
    }
  }
  return nullptr;
}

Mp4Muxer::ExternalTrack* Mp4Muxer::findExternalTrack(const int trackId) {
  for (auto& track: mExternalTracks) {
    if (track.id == trackId) {
      return &track;
    }
  }
  return nullptr;
}

std::vector<std::vector<uint8_t>> Mp4Muxer::splitNalUnits(const std::vector<uint8_t>& data) {
  return mp4::splitNalUnits(data);
}

} // namespace mp4
} // namespace muxer
} // namespace coral
