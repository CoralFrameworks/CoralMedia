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

#include "WavMuxer.h"

#include <minilog/log.h>

#include <array>
#include <limits>
#include <utility>

namespace coral {
namespace muxer {
namespace {

void putU16(std::array<uint8_t, 44>& data, const size_t offset, const uint16_t value) {
  data[offset] = static_cast<uint8_t>(value);
  data[offset + 1] = static_cast<uint8_t>(value >> 8);
}

void putU32(std::array<uint8_t, 44>& data, const size_t offset, const uint32_t value) {
  data[offset] = static_cast<uint8_t>(value);
  data[offset + 1] = static_cast<uint8_t>(value >> 8);
  data[offset + 2] = static_cast<uint8_t>(value >> 16);
  data[offset + 3] = static_cast<uint8_t>(value >> 24);
}

bool isRawAudio(const std::shared_ptr<MediaFormat>& format) {
  return format != nullptr &&
         (format->getAudioFormatType() == AudioFormatType::kAudioFormatTypeLinearPCM ||
          format->getMimeType() == "audio/raw" || format->getMimeType() == "audio/wav");
}

} // namespace

WavMuxer::WavMuxer(std::unique_ptr<MuxerOutput> output, MuxerOptions options) noexcept
    : mOutput(std::move(output)),
      mOptions(options) {
}

WavMuxer::~WavMuxer() {
  if (mStatus == Status::kWriting) {
    cancel();
  }
}

std::unique_ptr<WavMuxer> WavMuxer::create(const std::string& url, const MuxerOptions& options,
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
  auto muxer = std::unique_ptr<WavMuxer>(new WavMuxer(std::move(output), options));
  muxer->mStatus = Status::kWriting;
  return muxer;
}

int WavMuxer::addTrack(const std::shared_ptr<MediaFormat>& format,
  const TrackOptions& options) {
  if (mStatus != Status::kWriting) {
    fail("tracks can be added only while the muxer is writing");
    return -1;
  }
  if (format == nullptr) {
    fail("the track format is null");
    return -1;
  }
  if (mTrackId >= 0) {
    reject("a WAV file holds only one track");
    return -1;
  }
  if (!isRawAudio(format)) {
    reject("the WAV muxer supports raw PCM audio only");
    return -1;
  }
  if (mHeaderWritten) {
    fail("the track must be added before the first sample");
    return -1;
  }
  mSampleRate = format->getSampleRate() > 0 ? format->getSampleRate() : 48000;
  mChannelCount = format->getChannelCount() > 0 ? format->getChannelCount() : 2;
  switch (format->getSampleFormat()) {
    case AudioSampleFormat::kInt24:
      mBitsPerSample = 24;
      mAudioFormat = 1;
      break;
    case AudioSampleFormat::kInt32:
      mBitsPerSample = 32;
      mAudioFormat = 1;
      break;
    case AudioSampleFormat::kFloat:
      mBitsPerSample = 32;
      mAudioFormat = 3;
      break;
    case AudioSampleFormat::kInt16:
    default:
      mBitsPerSample = 16;
      mAudioFormat = 1;
      break;
  }
  mBlockAlign = mChannelCount * (mBitsPerSample / 8);
  mTrackId = 0;
  (void) options;
  return mTrackId;
}

bool WavMuxer::writeSample(const int trackId, const uint8_t* data, const size_t size,
  const SampleInfo& info) {
  (void) info;
  if (mStatus != Status::kWriting) {
    return fail("the muxer is not writing");
  }
  if (trackId != mTrackId) {
    return fail("the sample references an unknown track");
  }
  if (data == nullptr && size > 0) {
    return fail("the sample data is null");
  }
  if (size > static_cast<size_t>(std::numeric_limits<uint32_t>::max() - kHeaderSize)) {
    return fail("the WAV payload is larger than 4 GB");
  }
  if (!mHeaderWritten) {
    if (!writeHeader()) {
      return false;
    }
  }
  if (!writeBytes(data, size)) {
    return fail("cannot write the WAV sample");
  }
  mDataBytes += static_cast<int64_t>(size);
  ++mSampleCount;
  return true;
}

bool WavMuxer::writeHeader() {
  std::array<uint8_t, 44> header{};
  header[0] = 'R';
  header[1] = 'I';
  header[2] = 'F';
  header[3] = 'F';
  putU32(header, 4, 36); // patched at finish
  header[8] = 'W';
  header[9] = 'A';
  header[10] = 'V';
  header[11] = 'E';
  header[12] = 'f';
  header[13] = 'm';
  header[14] = 't';
  header[15] = ' ';
  putU32(header, 16, 16);
  putU16(header, 20, static_cast<uint16_t>(mAudioFormat));
  putU16(header, 22, static_cast<uint16_t>(mChannelCount));
  putU32(header, 24, static_cast<uint32_t>(mSampleRate));
  putU32(header, 28, static_cast<uint32_t>(mSampleRate * mBlockAlign));
  putU16(header, 32, static_cast<uint16_t>(mBlockAlign));
  putU16(header, 34, static_cast<uint16_t>(mBitsPerSample));
  header[36] = 'd';
  header[37] = 'a';
  header[38] = 't';
  header[39] = 'a';
  putU32(header, 40, 0); // patched at finish
  if (!writeBytes(header.data(), header.size())) {
    return fail("cannot write the WAV header");
  }
  mHeaderWritten = true;
  return true;
}

bool WavMuxer::patchSizes() {
  if (mDataBytes > std::numeric_limits<uint32_t>::max() - 36) {
    return fail("the WAV payload is larger than 4 GB");
  }
  std::array<uint8_t, 4> value{};
  value[0] = static_cast<uint8_t>(mDataBytes + 36);
  value[1] = static_cast<uint8_t>((mDataBytes + 36) >> 8);
  value[2] = static_cast<uint8_t>((mDataBytes + 36) >> 16);
  value[3] = static_cast<uint8_t>((mDataBytes + 36) >> 24);
  if (!mOutput->seek(4) || !writeBytes(value.data(), value.size())) {
    return fail("cannot patch the RIFF size");
  }
  value[0] = static_cast<uint8_t>(mDataBytes);
  value[1] = static_cast<uint8_t>(mDataBytes >> 8);
  value[2] = static_cast<uint8_t>(mDataBytes >> 16);
  value[3] = static_cast<uint8_t>(mDataBytes >> 24);
  if (!mOutput->seek(40) || !writeBytes(value.data(), value.size())) {
    return fail("cannot patch the data size");
  }
  return true;
}

bool WavMuxer::addMetadata(const std::shared_ptr<Metadata>& metadata) {
  (void) metadata;
  return mStatus == Status::kWriting;
}

bool WavMuxer::finish() {
  if (mStatus != Status::kWriting) {
    return fail("the muxer is not writing");
  }
  if (!mHeaderWritten || mSampleCount == 0) {
    return fail("the muxer has no samples");
  }
  if (!patchSizes()) {
    return false;
  }
  if (!mOutput->close()) {
    return fail("cannot close the output file");
  }
  mStatus = Status::kCompleted;
  return true;
}

void WavMuxer::cancel() {
  if (mStatus == Status::kCompleted || mStatus == Status::kCancelled) {
    return;
  }
  if (mOutput != nullptr) {
    mOutput->remove();
  }
  mStatus = Status::kCancelled;
}

bool WavMuxer::fail(std::string message) {
  mError.code = 1;
  mError.message = std::move(message);
  mStatus = Status::kFailed;
  MINILOG_ERROR("wav muxer: {}", mError.message);
  return false;
}

bool WavMuxer::reject(std::string message) {
  mError.code = 1;
  mError.message = std::move(message);
  MINILOG_WARN("wav muxer: {}", mError.message);
  return false;
}

bool WavMuxer::writeBytes(const uint8_t* data, const size_t size) {
  return size == 0 || (mOutput != nullptr && mOutput->write(data, size));
}

} // namespace muxer
} // namespace coral
