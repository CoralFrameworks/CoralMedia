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

#include "AacMuxer.h"

#include <minilog/log.h>

#include <algorithm>
#include <array>
#include <utility>

namespace coral {
namespace muxer {
namespace {

bool isAac(const std::shared_ptr<MediaFormat>& format) {
  return format != nullptr &&
         (format->getAudioFormatType() == AudioFormatType::kAudioFormatTypeMpeg4AAC ||
          format->getMimeType() == "audio/mp4a-latm" || format->getMimeType() == "audio/aac");
}

int frequencyIndex(const int sampleRate) {
  static constexpr std::array<int, 13> kRates = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350
  };
  for (size_t i = 0; i < kRates.size(); ++i) {
    if (kRates[i] == sampleRate) {
      return static_cast<int>(i);
    }
  }
  return 4;
}

bool parseAudioSpecificConfig(const std::vector<uint8_t>& config, int& objectType,
  int& frequency, int& channels) {
  if (config.size() < 2) {
    return false;
  }
  objectType = (config[0] >> 3) & 0x1F;
  frequency = ((config[0] & 0x07) << 1) | (config[1] >> 7);
  channels = (config[1] >> 3) & 0x0F;
  return objectType > 0 && objectType < 32;
}

} // namespace

AacMuxer::AacMuxer(std::unique_ptr<MuxerOutput> output, MuxerOptions options) noexcept
    : mOutput(std::move(output)),
      mOptions(options) {
}

AacMuxer::~AacMuxer() {
  if (mStatus == Status::kWriting) {
    cancel();
  }
}

std::unique_ptr<AacMuxer> AacMuxer::create(const std::string& url, const MuxerOptions& options,
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
  auto muxer = std::unique_ptr<AacMuxer>(new AacMuxer(std::move(output), options));
  muxer->mStatus = Status::kWriting;
  return muxer;
}

int AacMuxer::addTrack(const std::shared_ptr<MediaFormat>& format,
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
    reject("an ADTS stream holds only one track");
    return -1;
  }
  if (!isAac(format)) {
    reject("the ADTS muxer supports AAC audio only");
    return -1;
  }

  int objectType = 2;
  const auto& csd = format->csdList();
  if (!csd.empty() && parseAudioSpecificConfig(csd.front(), objectType, mFrequencyIndex,
        mChannelConfiguration)) {
    // The AudioSpecificConfig carries the rate and layout even when the format fields are unset.
    if (mFrequencyIndex < 0 || mFrequencyIndex > 12) {
      mFrequencyIndex = frequencyIndex(format->getSampleRate());
    }
    mChannelConfiguration = std::clamp(mChannelConfiguration, 0, 7);
  } else {
    mFrequencyIndex = frequencyIndex(format->getSampleRate());
    mChannelConfiguration = std::clamp(format->getChannelCount(), 1, 7);
  }
  mProfile = std::clamp(objectType - 1, 0, 3);
  mTrackId = 0;
  (void) options;
  return mTrackId;
}

bool AacMuxer::writeSample(const int trackId, const uint8_t* data, const size_t size,
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
  if (size > 0x1FFF - 7) {
    return fail("the AAC frame is too large for an ADTS header");
  }
  (void) info;

  const uint32_t frameLength = static_cast<uint32_t>(size + 7);
  std::array<uint8_t, 7> header{};
  header[0] = 0xFF;
  header[1] = 0xF1; // MPEG-4, layer 0, no CRC.
  header[2] = static_cast<uint8_t>(((mProfile & 0x03) << 6) |
                                   ((mFrequencyIndex & 0x0F) << 2) |
                                   ((mChannelConfiguration >> 2) & 0x01));
  header[3] = static_cast<uint8_t>(((mChannelConfiguration & 0x03) << 6) |
                                   ((frameLength >> 11) & 0x03));
  header[4] = static_cast<uint8_t>((frameLength >> 3) & 0xFF);
  header[5] = static_cast<uint8_t>(((frameLength & 0x07) << 5) | 0x1F);
  header[6] = 0xFC; // Buffer fullness 0x7FF and one raw data block.
  if (!writeBytes(header.data(), header.size()) ||
      !writeBytes(data, size)) {
    return fail("cannot write the AAC sample");
  }
  ++mSampleCount;
  return true;
}

bool AacMuxer::addMetadata(const std::shared_ptr<Metadata>& metadata) {
  (void) metadata;
  return mStatus == Status::kWriting;
}

bool AacMuxer::finish() {
  if (mStatus != Status::kWriting) {
    return fail("the muxer is not writing");
  }
  if (mSampleCount == 0) {
    return fail("the muxer has no samples");
  }
  if (!mOutput->close()) {
    return fail("cannot close the output file");
  }
  mStatus = Status::kCompleted;
  return true;
}

void AacMuxer::cancel() {
  if (mStatus == Status::kCompleted || mStatus == Status::kCancelled) {
    return;
  }
  if (mOutput != nullptr) {
    mOutput->remove();
  }
  mStatus = Status::kCancelled;
}

bool AacMuxer::fail(std::string message) {
  mError.code = 1;
  mError.message = std::move(message);
  mStatus = Status::kFailed;
  MINILOG_ERROR("adts muxer: {}", mError.message);
  return false;
}

bool AacMuxer::reject(std::string message) {
  mError.code = 1;
  mError.message = std::move(message);
  MINILOG_WARN("adts muxer: {}", mError.message);
  return false;
}

bool AacMuxer::writeBytes(const uint8_t* data, const size_t size) {
  return size == 0 || (mOutput != nullptr && mOutput->write(data, size));
}

} // namespace muxer
} // namespace coral
