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

#include "WebmMuxer.h"

#include "Ebml.h"

#include <minilog/log.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace coral {
namespace muxer {
namespace webm {
namespace {

constexpr uint32_t kEbml = 0x1A45DFA3;
constexpr uint32_t kEbmlVersion = 0x4286;
constexpr uint32_t kEbmlReadVersion = 0x42F7;
constexpr uint32_t kEbmlMaxIdLength = 0x42F2;
constexpr uint32_t kEbmlMaxSizeLength = 0x42F3;
constexpr uint32_t kDocType = 0x4282;
constexpr uint32_t kDocTypeVersion = 0x4287;
constexpr uint32_t kDocTypeReadVersion = 0x4285;
constexpr uint32_t kSegment = 0x18538067;
constexpr uint32_t kInfo = 0x1549A966;
constexpr uint32_t kTimecodeScale = 0x2AD7B1;
constexpr uint32_t kSegmentDuration = 0x4489;
constexpr uint32_t kMuxingApp = 0x4D80;
constexpr uint32_t kWritingApp = 0x5741;
constexpr uint32_t kCluster = 0x1F43B675;
constexpr uint32_t kTimecode = 0xE7;
constexpr uint32_t kSimpleBlock = 0xA3;
constexpr uint32_t kTracks = 0x1654AE6B;
constexpr uint32_t kTrackEntry = 0xAE;
constexpr uint32_t kTrackNumber = 0xD7;
constexpr uint32_t kTrackUid = 0x73C5;
constexpr uint32_t kTrackType = 0x83;
constexpr uint32_t kFlagLacing = 0x9C;
constexpr uint32_t kDefaultDuration = 0x23E383;
constexpr uint32_t kLanguage = 0x22B59C;
constexpr uint32_t kCodecId = 0x86;
constexpr uint32_t kCodecPrivate = 0x63A2;
constexpr uint32_t kVideo = 0xE0;
constexpr uint32_t kPixelWidth = 0xB0;
constexpr uint32_t kPixelHeight = 0xBA;
constexpr uint32_t kAudio = 0xE1;
constexpr uint32_t kSamplingFrequency = 0xB5;
constexpr uint32_t kChannels = 0x9F;
constexpr uint32_t kBitDepth = 0x6264;
constexpr uint32_t kCodecDelay = 0x56AA;
constexpr uint32_t kSeekPreRoll = 0x56BB;

constexpr uint32_t kKeyFrameFlag = static_cast<uint32_t>(SampleFlag::kKeyFrame);
constexpr int64_t kMaxClusterDurationUs = 2'000'000;
constexpr int64_t kMaxPendingBytes = 32 * 1024 * 1024;
constexpr uint64_t kTimecodeScaleNs = 1'000'000;

bool isVideoCodec(const WebmCodec codec) {
  return codec == WebmCodec::kVp8 || codec == WebmCodec::kVp9 || codec == WebmCodec::kAv1;
}

bool isAudioCodec(const WebmCodec codec) {
  return codec == WebmCodec::kOpus || codec == WebmCodec::kVorbis;
}

WebmCodec codecFor(const MediaFormat& format) {
  const std::string& mime = format.getMimeType();
  if (mime == "video/x-vnd.on2.vp8" || mime == "video/vp8") {
    return WebmCodec::kVp8;
  }
  if (mime == "video/x-vnd.on2.vp9" || mime == "video/vp9") {
    return WebmCodec::kVp9;
  }
  if (mime == "video/av01") {
    return WebmCodec::kAv1;
  }
  if (mime == "audio/opus") {
    return WebmCodec::kOpus;
  }
  if (mime == "audio/vorbis") {
    return WebmCodec::kVorbis;
  }
  switch (format.getVideoCodecType()) {
    case VideoCodecType::kVideoCodecTypeVP8:
      return WebmCodec::kVp8;
    case VideoCodecType::kVideoCodecTypeVP9:
      return WebmCodec::kVp9;
    case VideoCodecType::kVideoCodecTypeAV1:
      return WebmCodec::kAv1;
    default:
      break;
  }
  switch (format.getAudioFormatType()) {
    case AudioFormatType::kAudioFormatTypeOpus:
      return WebmCodec::kOpus;
    case AudioFormatType::kAudioFormatTypeVorbis:
      return WebmCodec::kVorbis;
    default:
      break;
  }
  return WebmCodec::kUnknown;
}

const char* codecId(const WebmCodec codec) {
  switch (codec) {
    case WebmCodec::kVp8:
      return "V_VP8";
    case WebmCodec::kVp9:
      return "V_VP9";
    case WebmCodec::kAv1:
      return "V_AV1";
    case WebmCodec::kOpus:
      return "A_OPUS";
    case WebmCodec::kVorbis:
      return "A_VORBIS";
    case WebmCodec::kUnknown:
    default:
      return "";
  }
}

int bitsPerSample(const MediaFormat& format) {
  switch (format.getSampleFormat()) {
    case AudioSampleFormat::kInt24:
      return 24;
    case AudioSampleFormat::kInt32:
    case AudioSampleFormat::kFloat:
      return 32;
    case AudioSampleFormat::kInt16:
    default:
      return 16;
  }
}

std::vector<uint8_t> makeOpusHead(const MediaFormat& format) {
  const auto& csd = format.csdList();
  if (!csd.empty() && csd.front().size() >= 19 &&
      std::memcmp(csd.front().data(), "OpusHead", 8) == 0) {
    return csd.front();
  }
  std::vector<uint8_t> head = { 'O', 'p', 'u', 's', 'H', 'e', 'a', 'd' };
  if (!csd.empty() && csd.front().size() >= 11) {
    // A dOps payload has the same fields as OpusHead after the signature, with version 0.
    head.push_back(1); // OpusHead version
    head.insert(head.end(), csd.front().begin() + 1, csd.front().end());
    return head;
  }
  const int channels = std::max(1, format.getChannelCount());
  const int sampleRate = format.getSampleRate() > 0 ? format.getSampleRate() : 48000;
  const int preSkip = format.getEncoderDelay() > 0 ? format.getEncoderDelay() : 3840;
  head.push_back(1);
  head.push_back(static_cast<uint8_t>(channels));
  head.push_back(static_cast<uint8_t>(preSkip));
  head.push_back(static_cast<uint8_t>(preSkip >> 8));
  for (int i = 0; i < 4; ++i) {
    head.push_back(static_cast<uint8_t>(sampleRate >> (i * 8)));
  }
  head.push_back(0);
  head.push_back(0); // output gain
  head.push_back(0); // channel mapping family
  return head;
}

std::vector<uint8_t> makeVorbisPrivate(const MediaFormat& format) {
  const auto& csd = format.csdList();
  if (csd.size() >= 2) {
    std::vector<uint8_t> data;
    data.push_back(2); // three packets
    const auto lace = [&data](const size_t length) {
      size_t remaining = length;
      while (remaining >= 255) {
        data.push_back(255);
        remaining -= 255;
      }
      data.push_back(static_cast<uint8_t>(remaining));
    };
    std::vector<uint8_t> comment = { 0x03, 'v', 'o', 'r', 'b', 'i', 's' };
    const char vendor[] = "Coral";
    const uint32_t vendorLength = sizeof(vendor) - 1;
    for (int i = 0; i < 4; ++i) {
      comment.push_back(static_cast<uint8_t>(vendorLength >> (i * 8)));
    }
    comment.insert(comment.end(), vendor, vendor + vendorLength);
    comment.insert(comment.end(), { 0, 0, 0, 0, 1 });
    std::vector<std::vector<uint8_t>> packets = { csd[0], comment, csd[1] };
    if (csd.size() >= 3) {
      packets[1] = csd[1];
      packets[2] = csd[2];
    }
    lace(packets[0].size());
    lace(packets[1].size());
    for (const auto& packet: packets) {
      data.insert(data.end(), packet.begin(), packet.end());
    }
    return data;
  }
  if (csd.size() == 1 && !csd.front().empty() && csd.front()[0] == 2) {
    return csd.front();
  }
  return {};
}

std::vector<uint8_t> makeCodecPrivate(const WebmCodec codec, const MediaFormat& format) {
  switch (codec) {
    case WebmCodec::kVp8:
      return {};
    case WebmCodec::kVp9:
    case WebmCodec::kAv1:
      return format.csdList().empty() ? std::vector<uint8_t>() : format.csdList().front();
    case WebmCodec::kOpus:
      return makeOpusHead(format);
    case WebmCodec::kVorbis:
      return makeVorbisPrivate(format);
    case WebmCodec::kUnknown:
    default:
      return {};
  }
}

void putInt16(std::vector<uint8_t>& data, const int16_t value) {
  data.push_back(static_cast<uint8_t>(static_cast<uint16_t>(value) >> 8));
  data.push_back(static_cast<uint8_t>(value));
}

std::vector<uint8_t> makeTrackEntry(const int number, const int trackId, const WebmCodec codec,
  const MediaFormat& format, const TrackOptions& options) {
  std::vector<uint8_t> payload;
  append(payload, uintElement(kTrackNumber, static_cast<uint64_t>(number)));
  append(payload, uintElement(kTrackUid, static_cast<uint64_t>(number)));
  append(payload, uintElement(kTrackType, isVideoCodec(codec) ? 1 : 2));
  append(payload, uintElement(kFlagLacing, 0));
  append(payload, stringElement(kCodecId, codecId(codec)));
  const auto codecPrivate = makeCodecPrivate(codec, format);
  if (!codecPrivate.empty()) {
    append(payload, binaryElement(kCodecPrivate, codecPrivate));
  }
  if (isVideoCodec(codec)) {
    if (format.getFrameRate() > 0) {
      append(payload, uintElement(kDefaultDuration,
        static_cast<uint64_t>(1'000'000'000.0 / format.getFrameRate())));
    }
    std::vector<uint8_t> video;
    append(video, uintElement(kPixelWidth, static_cast<uint64_t>(std::max(0, format.getWidth()))));
    append(video, uintElement(kPixelHeight, static_cast<uint64_t>(std::max(0, format.getHeight()))));
    append(payload, element(kVideo, video));
  } else {
    std::vector<uint8_t> audio;
    append(audio, floatElement(kSamplingFrequency,
      format.getSampleRate() > 0 ? format.getSampleRate() : 48000.0, true));
    append(audio, uintElement(kChannels,
      static_cast<uint64_t>(std::max(1, format.getChannelCount()))));
    append(audio, uintElement(kBitDepth, static_cast<uint64_t>(bitsPerSample(format))));
    append(payload, element(kAudio, audio));
    if (codec == WebmCodec::kOpus) {
      const auto opusHead = makeOpusHead(format);
      const uint64_t preSkip = opusHead.size() >= 12
                                 ? static_cast<uint64_t>(opusHead[10] | (opusHead[11] << 8))
                                 : 3840;
      append(payload, uintElement(kCodecDelay, preSkip * 1'000'000'000ull / 48000ull));
      append(payload, uintElement(kSeekPreRoll, 80'000'000ull));
    }
  }
  if (!options.language.empty()) {
    append(payload, stringElement(kLanguage, options.language));
  }
  (void) trackId;
  return element(kTrackEntry, payload);
}

} // namespace

WebmMuxer::WebmMuxer(std::unique_ptr<MuxerOutput> output, MuxerOptions options) noexcept
    : mOutput(std::move(output)),
      mOptions(options) {
}

WebmMuxer::~WebmMuxer() {
  if (mStatus == Status::kWriting) {
    cancel();
  }
}

std::unique_ptr<WebmMuxer> WebmMuxer::create(const std::string& url, const MuxerOptions& options,
  Error* error) {
  std::string openError;
  auto output = openFileOutput(url, &openError);
  if (output == nullptr) {
    if (error != nullptr) {
      error->code = 3;
      error->message = openError;
    }
    return nullptr;
  }
  auto muxer = std::unique_ptr<WebmMuxer>(new WebmMuxer(std::move(output), options));
  muxer->mStatus = Status::kWriting;
  return muxer;
}

int WebmMuxer::addTrack(const std::shared_ptr<MediaFormat>& format,
  const TrackOptions& options) {
  if (mStatus != Status::kWriting) {
    fail("tracks can be added only while the muxer is writing");
    return -1;
  }
  if (mHeaderWritten) {
    fail("all tracks must be added before the first sample");
    return -1;
  }
  if (format == nullptr) {
    fail("the track format is null");
    return -1;
  }
  const WebmCodec codec = codecFor(*format);
  if (codec == WebmCodec::kUnknown) {
    reject("the WebM muxer does not support this codec: " + format->getMimeType());
    return -1;
  }
  int id = options.trackId;
  if (id < 0) {
    id = mNextTrackId;
  }
  if (id <= 0 ||
      std::any_of(mTracks.begin(), mTracks.end(),
        [id](const Track& track) { return track.id == id; })) {
    fail("the track id is invalid or already used");
    return -1;
  }
  mNextTrackId = std::max(mNextTrackId, id + 1);
  Track track;
  track.format = format;
  track.options = options;
  track.id = id;
  track.number = static_cast<int>(mTracks.size()) + 1;
  track.codec = codec;
  mTracks.push_back(std::move(track));
  return id;
}

bool WebmMuxer::writeSample(const int trackId, const uint8_t* data, const size_t size,
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
  if (size == 0) {
    return true;
  }
  const bool keyFrame = (info.flags & kKeyFrameFlag) != 0;
  if (isVideoCodec(track->codec) && !track->hadKeyFrame) {
    if (!keyFrame) {
      return true;
    }
  }
  if (!mHeaderWritten && !writeHeader()) {
    return false;
  }
  if (shouldFlushBefore(*track, info, keyFrame) && !flushCluster()) {
    return false;
  }
  if (!hasPendingSamples()) {
    mClusterStartUs = info.presentationTimeUs;
  }

  PendingSample sample;
  sample.data.assign(data, data + size);
  sample.presentationTimeUs = info.presentationTimeUs;
  sample.durationUs = info.durationUs;
  sample.flags = info.flags;
  track->pending.push_back(std::move(sample));
  track->lastPresentationTimeUs = info.presentationTimeUs;
  track->lastDurationUs = info.durationUs;
  if (isVideoCodec(track->codec) && keyFrame) {
    track->hadKeyFrame = true;
  }
  const int64_t sampleDuration = info.durationUs > 0 ? info.durationUs : defaultDurationUs(*track);
  mLastEndUs = std::max(mLastEndUs, info.presentationTimeUs + sampleDuration);
  mHasSamples = true;
  return true;
}

bool WebmMuxer::hasPendingSamples() const {
  return std::any_of(mTracks.begin(), mTracks.end(),
    [](const Track& track) { return !track.pending.empty(); });
}

bool WebmMuxer::hasVideoTrack() const {
  return std::any_of(mTracks.begin(), mTracks.end(),
    [](const Track& track) { return isVideoCodec(track.codec); });
}

bool WebmMuxer::shouldFlushBefore(const Track& track, const SampleInfo& info,
  const bool keyFrame) const {
  if (!hasPendingSamples()) {
    return false;
  }
  int64_t pendingBytes = 0;
  for (const auto& other: mTracks) {
    for (const auto& sample: other.pending) {
      pendingBytes += static_cast<int64_t>(sample.data.size());
    }
  }
  if (pendingBytes >= kMaxPendingBytes) {
    return true;
  }
  if (hasVideoTrack()) {
    if (!isVideoCodec(track.codec) || !keyFrame || !track.hadKeyFrame) {
      return false;
    }
  }
  return info.presentationTimeUs - mClusterStartUs >= kMaxClusterDurationUs;
}

int64_t WebmMuxer::defaultDurationUs(const Track& track) const {
  if (isVideoCodec(track.codec)) {
    return track.format->getFrameRate() > 0
             ? std::max<int64_t>(1, 1'000'000 / track.format->getFrameRate())
             : 33333;
  }
  const int sampleRate = track.format->getSampleRate() > 0 ? track.format->getSampleRate() : 48000;
  if (track.codec == WebmCodec::kOpus) {
    return std::max<int64_t>(1, std::llround(960.0 * 1'000'000.0 / sampleRate));
  }
  if (track.codec == WebmCodec::kVorbis) {
    return std::max<int64_t>(1, std::llround(1024.0 * 1'000'000.0 / sampleRate));
  }
  return 0;
}

bool WebmMuxer::writeHeader() {
  if (mTracks.empty()) {
    return fail("the muxer has no tracks");
  }

  std::vector<uint8_t> ebml;
  append(ebml, uintElement(kEbmlVersion, 1));
  append(ebml, uintElement(kEbmlReadVersion, 1));
  append(ebml, uintElement(kEbmlMaxIdLength, 4));
  append(ebml, uintElement(kEbmlMaxSizeLength, 8));
  append(ebml, stringElement(kDocType, "webm"));
  append(ebml, uintElement(kDocTypeVersion, 4));
  append(ebml, uintElement(kDocTypeReadVersion, 2));
  const auto ebmlElement = element(kEbml, ebml);

  std::vector<uint8_t> infoPayload;
  append(infoPayload, uintElement(kTimecodeScale, kTimecodeScaleNs));
  append(infoPayload, stringElement(kMuxingApp, "Coral"));
  append(infoPayload, stringElement(kWritingApp, "Coral"));
  const size_t durationOffsetInPayload = infoPayload.size();
  const auto durationElement = floatElement(kSegmentDuration, 0.0, true);
  append(infoPayload, durationElement);
  const auto infoElement = element(kInfo, infoPayload);
  const size_t durationInInfo = infoElement.size() - infoPayload.size() +
                                durationOffsetInPayload + durationElement.size() - 8;

  std::vector<uint8_t> tracksPayload;
  for (const auto& track: mTracks) {
    append(tracksPayload,
      makeTrackEntry(track.number, track.id, track.codec, *track.format, track.options));
  }
  const auto tracksElement = element(kTracks, tracksPayload);

  std::vector<uint8_t> segmentHeader;
  putId(segmentHeader, kSegment);
  putUnknownSize(segmentHeader);

  if (!writeBytes(ebmlElement.data(), ebmlElement.size())) {
    return fail("cannot write the EBML header");
  }
  if (!writeBytes(segmentHeader.data(), segmentHeader.size())) {
    return fail("cannot write the Segment header");
  }
  const int64_t infoStart = mOutput->position();
  if (!writeBytes(infoElement.data(), infoElement.size())) {
    return fail("cannot write the Info element");
  }
  mDurationPosition = infoStart + static_cast<int64_t>(durationInInfo);
  if (!writeBytes(tracksElement.data(), tracksElement.size())) {
    return fail("cannot write the Tracks element");
  }
  mHeaderWritten = true;
  return true;
}

bool WebmMuxer::flushCluster() {
  struct Reference {
    Track* track = nullptr;
    const PendingSample* sample = nullptr;
  };
  std::vector<Reference> references;
  int64_t minimumPts = std::numeric_limits<int64_t>::max();
  for (auto& track: mTracks) {
    for (const auto& sample: track.pending) {
      references.push_back({ &track, &sample });
      minimumPts = std::min(minimumPts, sample.presentationTimeUs);
    }
  }
  if (references.empty()) {
    return true;
  }
  std::sort(references.begin(), references.end(),
    [](const Reference& left, const Reference& right) {
      if (left.sample->presentationTimeUs != right.sample->presentationTimeUs) {
        return left.sample->presentationTimeUs < right.sample->presentationTimeUs;
      }
      return left.track->number < right.track->number;
    });

  const int64_t clusterTimeMs = minimumPts / 1000;
  std::vector<uint8_t> clusterPayload;
  append(clusterPayload, uintElement(kTimecode, static_cast<uint64_t>(clusterTimeMs)));
  for (const auto& reference: references) {
    const PendingSample& sample = *reference.sample;
    const int64_t relativeUs = sample.presentationTimeUs - clusterTimeMs * 1000;
    const auto relativeMs = static_cast<int16_t>(
      std::clamp<int64_t>(std::llround(relativeUs / 1000.0), -32768, 32767));
    std::vector<uint8_t> block;
    putSize(block, static_cast<uint64_t>(reference.track->number));
    putInt16(block, relativeMs);
    block.push_back(static_cast<uint8_t>(
      isVideoCodec(reference.track->codec)
        ? ((sample.flags & kKeyFrameFlag) != 0 ? 0x80 : 0)
        : 0x80));
    block.insert(block.end(), sample.data.begin(), sample.data.end());
    append(clusterPayload, element(kSimpleBlock, block));
  }
  const auto clusterElement = element(kCluster, clusterPayload);
  if (!writeBytes(clusterElement.data(), clusterElement.size())) {
    return fail("cannot write a Cluster element");
  }
  for (auto& track: mTracks) {
    track.pending.clear();
  }
  mClusterStartUs = mLastEndUs;
  return true;
}

bool WebmMuxer::addMetadata(const std::shared_ptr<Metadata>& metadata) {
  (void) metadata;
  return mStatus == Status::kWriting;
}

bool WebmMuxer::finish() {  if (mStatus != Status::kWriting) {
    return fail("the muxer is not writing");
  }
  if (!mHeaderWritten || !mHasSamples) {
    return fail("the muxer has no samples");
  }
  if (!flushCluster()) {
    return false;
  }
  if (mOutput->isSeekable() && mDurationPosition >= 0) {
    uint64_t bits = 0;
    const double durationMs = static_cast<double>(mLastEndUs) / 1000.0;
    std::memcpy(&bits, &durationMs, sizeof(bits));
    std::array<uint8_t, 8> value{};
    for (int i = 0; i < 8; ++i) {
      value[i] = static_cast<uint8_t>(bits >> (56 - i * 8));
    }
    if (!mOutput->seek(mDurationPosition) ||
        !writeBytes(value.data(), value.size())) {
      return fail("cannot patch the WebM duration");
    }
  }
  if (!mOutput->close()) {
    return fail("cannot close the output file");
  }
  mStatus = Status::kCompleted;
  return true;
}

void WebmMuxer::cancel() {
  if (mStatus == Status::kCompleted || mStatus == Status::kCancelled) {
    return;
  }
  if (mOutput != nullptr) {
    mOutput->remove();
  }
  mStatus = Status::kCancelled;
}

bool WebmMuxer::fail(std::string message) {
  mError.code = 1;
  mError.message = std::move(message);
  mStatus = Status::kFailed;
  MINILOG_ERROR("webm muxer: {}", mError.message);
  return false;
}

bool WebmMuxer::reject(std::string message) {
  mError.code = 1;
  mError.message = std::move(message);
  MINILOG_WARN("webm muxer: {}", mError.message);
  return false;
}

bool WebmMuxer::writeBytes(const uint8_t* data, const size_t size) {
  return size == 0 || (mOutput != nullptr && mOutput->write(data, size));
}

WebmMuxer::Track* WebmMuxer::findTrack(const int trackId) {
  for (auto& track: mTracks) {
    if (track.id == trackId) {
      return &track;
    }
  }
  return nullptr;
}

const WebmMuxer::Track* WebmMuxer::findTrack(const int trackId) const {
  for (const auto& track: mTracks) {
    if (track.id == trackId) {
      return &track;
    }
  }
  return nullptr;
}

} // namespace webm
} // namespace muxer
} // namespace coral
