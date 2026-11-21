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

#include "OggMuxer.h"

#include <minilog/log.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <utility>

namespace coral {
namespace muxer {
namespace ogg {
namespace {

constexpr uint8_t kHeaderBos = 0x02;
constexpr uint8_t kHeaderEos = 0x04;
constexpr uint32_t kOggCrcPolynomial = 0x04C11DB7;
constexpr size_t kMaxSegments = 255;

bool isOpus(const MediaFormat& format) {
  return format.getMimeType() == "audio/opus" ||
         format.getAudioFormatType() == AudioFormatType::kAudioFormatTypeOpus;
}

bool isVorbis(const MediaFormat& format) {
  return format.getMimeType() == "audio/vorbis" ||
         format.getAudioFormatType() == AudioFormatType::kAudioFormatTypeVorbis;
}

bool isFlac(const MediaFormat& format) {
  return format.getMimeType() == "audio/flac" ||
         format.getAudioFormatType() == AudioFormatType::kAudioFormatTypeFlac;
}

void putLe16(std::vector<uint8_t>& data, const uint16_t value) {
  data.push_back(static_cast<uint8_t>(value));
  data.push_back(static_cast<uint8_t>(value >> 8));
}

void putLe32(std::vector<uint8_t>& data, const uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    data.push_back(static_cast<uint8_t>(value >> (i * 8)));
  }
}

void putLe64(std::vector<uint8_t>& data, const uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    data.push_back(static_cast<uint8_t>(value >> (i * 8)));
  }
}

uint32_t oggCrc(const std::vector<uint8_t>& data) {
  uint32_t crc = 0;
  for (const uint8_t value: data) {
    crc ^= static_cast<uint32_t>(value) << 24;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x80000000u) != 0 ? (crc << 1) ^ kOggCrcPolynomial : crc << 1;
    }
  }
  return crc;
}

std::vector<uint8_t> makeOpusHead(const MediaFormat& format) {
  const auto& csd = format.csdList();
  if (!csd.empty() && csd.front().size() >= 19 &&
      std::memcmp(csd.front().data(), "OpusHead", 8) == 0) {
    return csd.front();
  }
  std::vector<uint8_t> head = { 'O', 'p', 'u', 's', 'H', 'e', 'a', 'd' };
  if (!csd.empty() && csd.front().size() >= 11) {
    head.push_back(1);
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
  head.push_back(0);
  head.push_back(0);
  return head;
}

std::vector<uint8_t> makeOpusComment() {
  static constexpr char kVendor[] = "Coral";
  std::vector<uint8_t> data = { 'O', 'p', 'u', 's', 'T', 'a', 'g', 's' };
  putLe32(data, sizeof(kVendor) - 1);
  data.insert(data.end(), kVendor, kVendor + sizeof(kVendor) - 1);
  putLe32(data, 0);
  return data;
}

std::vector<uint8_t> makeVorbisComment() {
  static constexpr char kVendor[] = "Coral";
  std::vector<uint8_t> data = { 0x03, 'v', 'o', 'r', 'b', 'i', 's' };
  putLe32(data, sizeof(kVendor) - 1);
  data.insert(data.end(), kVendor, kVendor + sizeof(kVendor) - 1);
  putLe32(data, 0);
  data.push_back(1); // framing bit
  return data;
}

bool splitXiphLacing(const std::vector<uint8_t>& data,
  std::vector<std::vector<uint8_t>>* packets) {
  if (data.size() < 2 || data[0] != 2) {
    return false;
  }
  size_t position = 1;
  size_t lengths[2] = { 0, 0 };
  for (size_t i = 0; i < 2; ++i) {
    size_t length = 0;
    while (position < data.size() && data[position] == 255) {
      length += 255;
      ++position;
    }
    if (position >= data.size()) {
      return false;
    }
    length += data[position++];
    lengths[i] = length;
  }
  if (position + lengths[0] + lengths[1] > data.size()) {
    return false;
  }
  packets->clear();
  packets->emplace_back(data.begin() + static_cast<std::ptrdiff_t>(position),
    data.begin() + static_cast<std::ptrdiff_t>(position + lengths[0]));
  position += lengths[0];
  packets->emplace_back(data.begin() + static_cast<std::ptrdiff_t>(position),
    data.begin() + static_cast<std::ptrdiff_t>(position + lengths[1]));
  position += lengths[1];
  packets->emplace_back(data.begin() + static_cast<std::ptrdiff_t>(position), data.end());
  return true;
}

std::vector<uint8_t> makeFlacHeader(const MediaFormat& format) {
  const auto& csd = format.csdList();
  if (csd.empty() || csd.front().empty()) {
    return {};
  }
  std::vector<uint8_t> header = { 0x7F, 'F', 'L', 'A', 'C', 1, 0, 0, 1 };
  if (csd.front().size() >= 4 && std::memcmp(csd.front().data(), "fLaC", 4) == 0) {
    header.insert(header.end(), csd.front().begin(), csd.front().end());
  } else {
    header.insert(header.end(), { 'f', 'L', 'a', 'C' });
    header.insert(header.end(), csd.front().begin(), csd.front().end());
  }
  return header;
}

std::vector<std::vector<uint8_t>> vorbisHeaders(const MediaFormat& format) {  const auto& csd = format.csdList();
  if (csd.size() >= 3) {
    return { csd[0], csd[1], csd[2] };
  }
  if (csd.size() == 2) {
    // Some extractors expose only the identification and setup headers; the comment header is
    // generated for the Ogg stream.
    return { csd[0], makeVorbisComment(), csd[1] };
  }
  if (csd.size() == 1) {
    std::vector<std::vector<uint8_t>> packets;
    if (splitXiphLacing(csd.front(), &packets)) {
      return packets;
    }
  }
  return {};
}

int64_t packetDurationUs(const OggCodec codec, const int sampleRate) {
  if (codec == OggCodec::kOpus) {
    return std::max<int64_t>(1, std::llround(960.0 * 1'000'000.0 / 48000.0));
  }
  if (codec == OggCodec::kFlac) {
    return std::max<int64_t>(1, std::llround(4096.0 * 1'000'000.0 / std::max(1, sampleRate)));
  }
  return std::max<int64_t>(1, std::llround(1024.0 * 1'000'000.0 / std::max(1, sampleRate)));
}

} // namespace

OggMuxer::OggMuxer(std::unique_ptr<MuxerOutput> output, MuxerOptions options) noexcept
    : mOutput(std::move(output)),
      mOptions(options) {
}

OggMuxer::~OggMuxer() {
  if (mStatus == Status::kWriting) {
    cancel();
  }
}

std::unique_ptr<OggMuxer> OggMuxer::create(const std::string& url, const MuxerOptions& options,
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
  auto muxer = std::unique_ptr<OggMuxer>(new OggMuxer(std::move(output), options));
  muxer->mSerial = 0x436F7261u; // "Cora"
  muxer->mStatus = Status::kWriting;
  return muxer;
}

int OggMuxer::addTrack(const std::shared_ptr<MediaFormat>& format,
  const TrackOptions& options) {
  if (mStatus != Status::kWriting) {
    fail("tracks can be added only while the muxer is writing");
    return -1;
  }
  if (mTrackId >= 0) {
    reject("an Ogg stream holds only one track");
    return -1;
  }
  if (format == nullptr) {
    fail("the track format is null");
    return -1;
  }
  if (isOpus(*format)) {
    mCodec = OggCodec::kOpus;
  } else if (isVorbis(*format)) {
    mCodec = OggCodec::kVorbis;
  } else if (isFlac(*format)) {
    mCodec = OggCodec::kFlac;
  } else {
    reject("the Ogg muxer supports Opus, Vorbis and FLAC audio only");
    return -1;
  }
  mFormat = format;
  mSampleRate = format->getSampleRate() > 0 ? format->getSampleRate() : 48000;
  if (mCodec == OggCodec::kOpus) {
    const auto head = makeOpusHead(*format);
    mPreSkip = head.size() >= 12 ? static_cast<int64_t>(head[10] | (head[11] << 8)) : 0;
    mSampleRate = 48000;
  }
  mTrackId = 0;
  (void) options;
  return mTrackId;
}

bool OggMuxer::writeHeaders() {
  if (mCodec == OggCodec::kOpus) {
    if (!writePacket(makeOpusHead(*mFormat), 0, kHeaderBos)) {
      return false;
    }
    if (!writePacket(makeOpusComment(), 0, 0)) {
      return false;
    }
  } else if (mCodec == OggCodec::kFlac) {
    const auto header = makeFlacHeader(*mFormat);
    if (header.empty()) {
      return fail("the FLAC track has no STREAMINFO");
    }
    if (!writePacket(header, 0, kHeaderBos)) {
      return false;
    }
  } else {
    const auto packets = vorbisHeaders(*mFormat);
    if (packets.size() != 3) {
      return fail("the Vorbis track has no identification/comment/setup headers");
    }
    if (!writePacket(packets[0], 0, kHeaderBos) ||
        !writePacket(packets[1], 0, 0) ||
        !writePacket(packets[2], 0, 0)) {
      return false;
    }
  }
  mHeadersWritten = true;
  return true;
}

bool OggMuxer::writeSample(const int trackId, const uint8_t* data, const size_t size,
  const SampleInfo& info) {
  if (mStatus != Status::kWriting) {
    return fail("the muxer is not writing");
  }
  if (trackId != mTrackId) {
    return fail("the sample references an unknown track");
  }
  if (data == nullptr && size > 0) {
    return fail("the sample data is null");
  }
  if (!mHeadersWritten && !writeHeaders()) {
    return false;
  }
  const int64_t durationUs = info.durationUs > 0 ? info.durationUs
                                                 : packetDurationUs(mCodec, mSampleRate);
  int64_t granule = 0;
  if (mCodec == OggCodec::kOpus) {
    granule = mPreSkip + static_cast<int64_t>(
      std::llround(static_cast<double>(info.presentationTimeUs + durationUs) * 48000.0 / 1'000'000.0));
  } else {
    granule = static_cast<int64_t>(
      std::llround(static_cast<double>(info.presentationTimeUs + durationUs) *
                   static_cast<double>(mSampleRate) / 1'000'000.0));
  }
  mGranulePosition = std::max(mGranulePosition, granule);
  if (!writePacket(std::vector<uint8_t>(data, data + size), mGranulePosition, 0)) {
    return false;
  }
  mHasSamples = true;
  return true;
}

bool OggMuxer::addMetadata(const std::shared_ptr<Metadata>& metadata) {
  (void) metadata;
  return mStatus == Status::kWriting;
}

bool OggMuxer::finish() {
  if (mStatus != Status::kWriting) {
    return fail("the muxer is not writing");
  }
  if (!mHeadersWritten || !mHasSamples) {
    return fail("the muxer has no samples");
  }
  if (!writePage({}, mGranulePosition, kHeaderEos)) {
    return false;
  }
  if (!mOutput->close()) {
    return fail("cannot close the output file");
  }
  mStatus = Status::kCompleted;
  return true;
}

void OggMuxer::cancel() {
  if (mStatus == Status::kCompleted || mStatus == Status::kCancelled) {
    return;
  }
  if (mOutput != nullptr) {
    mOutput->remove();
  }
  mStatus = Status::kCancelled;
}

bool OggMuxer::writePacket(const std::vector<uint8_t>& packet,
  const int64_t granulePosition, const uint8_t flags) {
  return writePage({ packet }, granulePosition, flags);
}

bool OggMuxer::writePage(const std::vector<std::vector<uint8_t>>& packets,
  const int64_t granulePosition, const uint8_t flags) {
  std::vector<uint8_t> segments;
  std::vector<uint8_t> payload;
  for (const auto& packet: packets) {
    size_t remaining = packet.size();
    while (remaining >= 255) {
      segments.push_back(255);
      remaining -= 255;
    }
    segments.push_back(static_cast<uint8_t>(remaining));
    payload.insert(payload.end(), packet.begin(), packet.end());
  }

  if (segments.empty()) {
    return writePageChunk({}, {}, granulePosition, flags);
  }

  size_t segmentIndex = 0;
  size_t payloadPosition = 0;
  while (segmentIndex < segments.size()) {
    const size_t count = std::min<size_t>(kMaxSegments, segments.size() - segmentIndex);
    std::vector<uint8_t> pageSegments(segments.begin() + static_cast<std::ptrdiff_t>(segmentIndex),
      segments.begin() + static_cast<std::ptrdiff_t>(segmentIndex + count));
    size_t pageBytes = 0;
    for (const uint8_t segment: pageSegments) {
      pageBytes += segment;
    }
    if (payloadPosition + pageBytes > payload.size()) {
      return fail("an Ogg page exceeds its packet payload");
    }
    const std::vector<uint8_t> pagePayload(
      payload.begin() + static_cast<std::ptrdiff_t>(payloadPosition),
      payload.begin() + static_cast<std::ptrdiff_t>(payloadPosition + pageBytes));
    const bool lastPage = segmentIndex + count == segments.size();
    const uint8_t pageFlags = segmentIndex == 0 ? flags : 0x01;
    if (!writePageChunk(pageSegments, pagePayload,
          lastPage ? granulePosition : -1, pageFlags)) {
      return false;
    }
    segmentIndex += count;
    payloadPosition += pageBytes;
  }
  return true;
}

bool OggMuxer::writePageChunk(const std::vector<uint8_t>& segments,
  const std::vector<uint8_t>& payload, const int64_t granulePosition, const uint8_t flags) {
  std::vector<uint8_t> page;
  page.reserve(27 + segments.size() + payload.size());
  page.insert(page.end(), { 'O', 'g', 'g', 'S', 0, flags });
  putLe64(page, static_cast<uint64_t>(granulePosition));
  putLe32(page, mSerial);
  putLe32(page, mSequence);
  putLe32(page, 0); // checksum, patched below
  page.push_back(static_cast<uint8_t>(segments.size()));
  page.insert(page.end(), segments.begin(), segments.end());
  page.insert(page.end(), payload.begin(), payload.end());

  const uint32_t crc = oggCrc(page);
  page[22] = static_cast<uint8_t>(crc);
  page[23] = static_cast<uint8_t>(crc >> 8);
  page[24] = static_cast<uint8_t>(crc >> 16);
  page[25] = static_cast<uint8_t>(crc >> 24);
  if (!writeBytes(page.data(), page.size())) {
    return fail("cannot write an Ogg page");
  }
  ++mSequence;
  return true;
}

bool OggMuxer::fail(std::string message) {
  mError.code = 1;
  mError.message = std::move(message);
  mStatus = Status::kFailed;
  MINILOG_ERROR("ogg muxer: {}", mError.message);
  return false;
}

bool OggMuxer::reject(std::string message) {
  mError.code = 1;
  mError.message = std::move(message);
  MINILOG_WARN("ogg muxer: {}", mError.message);
  return false;
}

bool OggMuxer::writeBytes(const uint8_t* data, const size_t size) {
  return size == 0 || (mOutput != nullptr && mOutput->write(data, size));
}

} // namespace ogg
} // namespace muxer
} // namespace coral
