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

#include "FragmentedMp4Muxer.h"

#include "Mp4Boxes.h"
#include "SampleTable.h"

#include <minilog/log.h>

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace coral {
namespace muxer {
namespace mp4 {
namespace {

constexpr int64_t kDefaultMovieTimescale = 1000;
constexpr int64_t kDefaultFragmentDurationUs = 2'000'000;
constexpr int64_t kMaxPendingBytes = 64 * 1024 * 1024;
constexpr uint32_t kKeyFrameFlag = static_cast<uint32_t>(SampleFlag::kKeyFrame);
constexpr uint32_t kSyncSampleFlags = 0x02000000;
constexpr uint32_t kNonSyncSampleFlags = 0x01010000;

} // namespace

FragmentedMp4Muxer::FragmentedMp4Muxer(std::unique_ptr<MuxerOutput> output, MuxerOptions options) noexcept
        : mOutput(std::move(output)),
          mOptions(options) {}

FragmentedMp4Muxer::~FragmentedMp4Muxer() {
  if (mStatus == Status::kWriting) {
    cancel();
  }
}

std::unique_ptr<FragmentedMp4Muxer> FragmentedMp4Muxer::create(const std::string& url, const MuxerOptions& options,
        Error* error) {
  MuxerOptions normalized = options;
  if (normalized.fragmentDurationUs <= 0) {
    normalized.fragmentDurationUs = kDefaultFragmentDurationUs;
  }
  if (normalized.movieTimescale <= 0) {
    normalized.movieTimescale = kDefaultMovieTimescale;
  }

  std::string openError;
  auto output = openFileOutput(url, &openError);
  if (output == nullptr) {
    if (error != nullptr) {
      error->code = 3;
      error->message = openError;
    }
    return nullptr;
  }

  auto muxer = std::unique_ptr<FragmentedMp4Muxer>(new FragmentedMp4Muxer(std::move(output), normalized));
  muxer->mFragmentDurationUs = normalized.fragmentDurationUs;
  muxer->mStatus = Status::kWriting;
  return muxer;
}

int FragmentedMp4Muxer::addTrack(const std::shared_ptr<MediaFormat>& format, const TrackOptions& options) {
  if (mStatus != Status::kWriting) {
    fail("tracks can be added only while the muxer is writing");
    return -1;
  }
  if (mHeaderWritten) {
    fail("all tracks must be added before the first sample in a fragmented MP4");
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
  if (std::any_of(mTracks.begin(), mTracks.end(), [trackId](const Track& track) { return track.id == trackId; })) {
    fail("a track with this id already exists");
    return -1;
  }
  mNextTrackId = std::max(mNextTrackId, trackId + 1);

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

bool FragmentedMp4Muxer::writeSample(const int trackId, const uint8_t* data, const size_t size,
        const SampleInfo& info) {
  if (mStatus != Status::kWriting) {
    return fail("the muxer is not writing");
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

  const bool keyFrame = (info.flags & kKeyFrameFlag) != 0;
  if (isVideoCodec(track->codec)) {
    if (!track->hadKeyFrame) {
      if (!keyFrame) {
        return true;
      }
    }
  }

  if (!mHeaderWritten && !writeHeader()) {
    return false;
  }
  if (shouldFlushBefore(*track, info, keyFrame)) {
    if (!flushFragment()) {
      return false;
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
  if (isVideoCodec(track->codec) && keyFrame) {
    track->hadKeyFrame = true;
  }
  mHasSamples = true;
  return true;
}

bool FragmentedMp4Muxer::addMetadata(const std::shared_ptr<Metadata>& metadata) {
  if (mStatus != Status::kWriting) {
    return fail("metadata can be added only while the muxer is writing");
  }
  if (mHeaderWritten) {
    return fail("metadata must be added before the first sample in a fragmented MP4");
  }
  mMetadata.add(metadata);
  return true;
}

bool FragmentedMp4Muxer::shouldFlushBefore(const Track& track, const SampleInfo& info, const bool keyFrame) const {
  if (track.pending.empty()) {
    return false;
  }
  if (track.pendingBytes >= kMaxPendingBytes) {
    return true;
  }
  if (hasVideoTrack()) {
    if (!isVideoCodec(track.codec) || !keyFrame || !track.hadKeyFrame) {
      return false;
    }
    return info.presentationTimeUs - track.pendingMinPts >= mFragmentDurationUs;
  }
  return info.presentationTimeUs - track.pendingMinPts >= mFragmentDurationUs;
}

bool FragmentedMp4Muxer::hasVideoTrack() const {
  return std::any_of(mTracks.begin(), mTracks.end(), [](const Track& track) { return isVideoCodec(track.codec); });
}

bool FragmentedMp4Muxer::writeHeader() {
  if (mTracks.empty()) {
    return fail("the muxer has no tracks");
  }

  BoxWriter ftyp;
  writeFtyp(ftyp);
  if (!writeBytes(ftyp.data().data(), ftyp.size())) {
    return fail("cannot write the ftyp box");
  }

  BoxWriter moov;
  if (!writeMoov(moov)) {
    return false;
  }
  if (!writeBytes(moov.data().data(), moov.size())) {
    return fail("cannot write the moov box");
  }

  BoxWriter xmp;
  mMetadata.writeXmp(xmp);
  if (!writeBytes(xmp.data().data(), xmp.size())) {
    return fail("cannot write the XMP box");
  }

  mHeaderWritten = true;
  return true;
}

bool FragmentedMp4Muxer::writeMoov(BoxWriter& writer) {
  const size_t moov = writer.beginBox("moov");
  writeMvhd(writer, 0, mOptions.movieTimescale, mNextTrackId, mMetadata.creationTime(), mMetadata.modificationTime());
  mMetadata.writeUdta(writer);

  for (const auto& track: mTracks) {
    if (!writeEmptyTrack(writer, track)) {
      return false;
    }
  }

  const size_t mvex = writer.beginBox("mvex");
  for (const auto& track: mTracks) {
    const size_t trex = writer.beginFullBox("trex", 0, 0);
    writer.u32(static_cast<uint32_t>(track.id));
    writer.u32(1); // default sample description index
    writer.u32(0); // default sample duration
    writer.u32(0); // default sample size
    writer.u32(0); // default sample flags
    writer.endBox(trex);
  }
  writer.endBox(mvex);
  writer.endBox(moov);
  return true;
}

bool FragmentedMp4Muxer::writeEmptyTrack(BoxWriter& writer, const Track& track) {
  const bool audio = isAudioCodec(track.codec);
  const bool text = isTextCodec(track.codec);
  const int width = audio || text ? 0 : track.format->getWidth();
  const int height = audio || text ? 0 : track.format->getHeight();
  const int rotation = mMetadata.orientation(track.format->getRotation());

  const size_t trak = writer.beginBox("trak");
  writeTkhd(writer, track.id, 0, mOptions.movieTimescale, mMetadata.creationTime(), mMetadata.modificationTime(),
          rotation, track.options.alternateGroup, track.options.enabled, audio, width, height);

  if (track.options.auxiliary) {
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

  const size_t mdia = writer.beginBox("mdia");
  writeMdhd(writer, 0, track.timescale, mMetadata.creationTime(), mMetadata.modificationTime(), track.options.language);
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

  const size_t stts = writer.beginFullBox("stts", 0, 0);
  writer.u32(0);
  writer.endBox(stts);

  const size_t stsc = writer.beginFullBox("stsc", 0, 0);
  writer.u32(0);
  writer.endBox(stsc);

  const size_t stsz = writer.beginFullBox("stsz", 0, 0);
  writer.u32(0);
  writer.u32(0);
  writer.endBox(stsz);

  const size_t stco = writer.beginFullBox("stco", 0, 0);
  writer.u32(0);
  writer.endBox(stco);

  writer.endBox(stbl);
  writer.endBox(minf);
  writer.endBox(mdia);
  writer.endBox(trak);
  return true;
}

bool FragmentedMp4Muxer::flushFragment() {
  struct FragmentTrack {
    Track* track = nullptr;
    SampleTable table;
    bool hasCompositionOffsets = false;
    uint32_t totalBytes = 0;
  };

  std::vector<FragmentTrack> fragmentTracks;
  int64_t totalBytes = 0;
  for (auto& track: mTracks) {
    if (track.pending.empty()) {
      continue;
    }
    FragmentTrack fragment;
    fragment.track = &track;
    fragment.table = SampleTable(track.timescale);
    for (const auto& sample: track.pending) {
      fragment.table.addSample(static_cast<uint32_t>(sample.data.size()), sample.presentationTimeUs, sample.durationUs,
              sample.flags);
    }
    // The next sample of this track is not known yet (the sample that triggered the flush has not
    // been added), so derive the last duration from the previous one instead of stretching it to
    // another track's fragment boundary. Stretching would make the audio and video timelines drift
    // by up to one frame per fragment.
    fragment.table.finalize(defaultSampleDurationUs(track.codec, *track.format));
    fragment.hasCompositionOffsets = isVideoCodec(track.codec) && fragment.table.hasCompositionOffsets();
    for (const auto& sample: track.pending) {
      fragment.totalBytes += static_cast<uint32_t>(sample.data.size());
    }
    totalBytes += fragment.totalBytes;
    fragmentTracks.push_back(std::move(fragment));
  }
  if (fragmentTracks.empty()) {
    return true;
  }

  const bool largeMdat = totalBytes + 8 > std::numeric_limits<uint32_t>::max();
  const int mdatHeaderSize = largeMdat ? 16 : 8;
  const int64_t moofStart = mOutput->position();

  BoxWriter moof;
  const size_t moofBox = moof.beginBox("moof");
  const size_t mfhd = moof.beginFullBox("mfhd", 0, 0);
  moof.u32(static_cast<uint32_t>(mSequenceNumber));
  moof.endBox(mfhd);

  std::vector<size_t> dataOffsetPositions;
  dataOffsetPositions.reserve(fragmentTracks.size());
  for (const auto& fragment: fragmentTracks) {
    const Track& track = *fragment.track;
    const size_t traf = moof.beginBox("traf");

    const size_t tfhd = moof.beginFullBox("tfhd", 0, 0x000001);
    moof.u32(static_cast<uint32_t>(track.id));
    moof.u64(static_cast<uint64_t>(moofStart));
    moof.endBox(tfhd);

    const size_t tfdt = moof.beginFullBox("tfdt", 1, 0);
    moof.u64(static_cast<uint64_t>(std::max<int64_t>(0, track.nextDecodeTimeTicks)));
    moof.endBox(tfdt);

    uint32_t trunFlags = 0x000001 | 0x000100 | 0x000200 | 0x000400;
    if (fragment.hasCompositionOffsets) {
      trunFlags |= 0x000800;
    }
    const size_t trun = moof.beginFullBox("trun", fragment.hasCompositionOffsets ? 1 : 0, trunFlags);
    moof.u32(static_cast<uint32_t>(fragment.table.size()));
    dataOffsetPositions.push_back(moof.size());
    moof.i32(0);
    const auto compositionOffsets =
            fragment.hasCompositionOffsets ? fragment.table.compositionOffsetsTicks() : std::vector<int32_t>();
    for (size_t i = 0; i < fragment.table.size(); ++i) {
      const auto& sample = fragment.table.samples()[i];
      moof.u32(static_cast<uint32_t>(
              std::min<int64_t>(fragment.table.sampleDurationTicks(i), std::numeric_limits<uint32_t>::max())));
      moof.u32(sample.size);
      const bool sync = isAudioCodec(track.codec) || isTextCodec(track.codec) ||
                        (sample.flags & kKeyFrameFlag) != 0;
      moof.u32(sync ? kSyncSampleFlags : kNonSyncSampleFlags);
      if (fragment.hasCompositionOffsets) {
        moof.i32(compositionOffsets[i]);
      }
    }
    moof.endBox(trun);
    moof.endBox(traf);
  }
  moof.endBox(moofBox);

  int64_t dataOffset = static_cast<int64_t>(moof.size()) + mdatHeaderSize;
  for (size_t i = 0; i < fragmentTracks.size(); ++i) {
    if (dataOffset > std::numeric_limits<int32_t>::max()) {
      return fail("a fragment is too large");
    }
    moof.patchU32(dataOffsetPositions[i], static_cast<uint32_t>(dataOffset));
    dataOffset += fragmentTracks[i].totalBytes;
  }

  if (!writeBytes(moof.data().data(), moof.size())) {
    return fail("cannot write a movie fragment");
  }

  BoxWriter mdat;
  const auto mdatSize = static_cast<uint64_t>(mdatHeaderSize + totalBytes);
  if (largeMdat) {
    mdat.u32(1);
    mdat.fourcc("mdat");
    mdat.u64(mdatSize);
  } else {
    mdat.u32(static_cast<uint32_t>(mdatSize));
    mdat.fourcc("mdat");
  }
  if (!writeBytes(mdat.data().data(), mdat.size())) {
    return fail("cannot write the fragment mdat header");
  }

  for (auto& fragment: fragmentTracks) {
    Track& track = *fragment.track;
    for (const auto& sample: track.pending) {
      if (!writeBytes(sample.data.data(), sample.data.size())) {
        return fail("cannot write a fragment sample");
      }
    }
    track.nextDecodeTimeTicks += fragment.table.durationTicks();
    if (!track.pending.empty()) {
      track.lastSampleDurationUs = track.pending.back().durationUs > 0
                                           ? track.pending.back().durationUs
                                           : fragment.table.sampleDurationUs(fragment.table.size() - 1);
      track.pending.clear();
      track.pendingBytes = 0;
      track.pendingMinPts = 0;
      track.pendingMaxPts = 0;
    }
  }

  ++mSequenceNumber;
  return true;
}

bool FragmentedMp4Muxer::finish() {
  if (mStatus != Status::kWriting) {
    return fail("the muxer is not writing");
  }
  if (!mHeaderWritten) {
    return fail("the muxer has no samples");
  }
  if (!flushFragment()) {
    return false;
  }
  if (!mOutput->close()) {
    return fail("cannot close the output file");
  }
  mStatus = Status::kCompleted;
  return true;
}

void FragmentedMp4Muxer::cancel() {
  if (mStatus == Status::kCompleted || mStatus == Status::kCancelled) {
    return;
  }
  if (mOutput != nullptr) {
    mOutput->remove();
  }
  mStatus = Status::kCancelled;
}

bool FragmentedMp4Muxer::writeBytes(const uint8_t* data, const size_t size) {
  return size == 0 || (mOutput != nullptr && mOutput->write(data, size));
}

bool FragmentedMp4Muxer::fail(std::string message) {
  mError.code = 1;
  mError.message = std::move(message);
  mStatus = Status::kFailed;
  MINILOG_ERROR("fragmented mp4 muxer: {}", mError.message);
  return false;
}

bool FragmentedMp4Muxer::reject(std::string message) {
  // A track the container cannot represent is not an error of the muxer: the caller skips it and
  // keeps writing the other tracks, so the status stays kWriting.
  mError.code = 1;
  mError.message = std::move(message);
  MINILOG_WARN("fragmented mp4 muxer: {}", mError.message);
  return false;
}

FragmentedMp4Muxer::Track* FragmentedMp4Muxer::findTrack(const int trackId) {
  for (auto& track: mTracks) {
    if (track.id == trackId) {
      return &track;
    }
  }
  return nullptr;
}

const FragmentedMp4Muxer::Track* FragmentedMp4Muxer::findTrack(const int trackId) const {
  for (const auto& track: mTracks) {
    if (track.id == trackId) {
      return &track;
    }
  }
  return nullptr;
}

} // namespace mp4
} // namespace muxer
} // namespace coral
