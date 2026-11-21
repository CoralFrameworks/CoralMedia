/*
 * Copyright (C) 2022 The Coral Project
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

#include "MediaExtractor.h"
#include "MediaCodecUtils.h"

#include <media/Types.h>

#include <vector>

namespace coral {

std::shared_ptr<MediaExtractor>
MediaExtractor::CreateWithUrl(const std::string& url) {
  auto extractor = std::make_shared<MediaExtractor>();
  extractor->setDataSource(url);
  return extractor;
}

MediaExtractor::MediaExtractor()
  : mOutput(nullptr),
    needPrepareFormat(true),
    mCurrentPosition(Definitions::kPositionUnset) {
  extractor = std::make_unique<JniMediaExtractor>();
}

MediaExtractor::~MediaExtractor() {
  extractor.reset();
  mOutput = nullptr;
}

void MediaExtractor::setDataSource(const std::string& url) {
  if (!extractor) {
    return;
  }
  extractor->setDataSource(url);
}

void MediaExtractor::init(AVExtractorOutput& output) {
  mOutput = &output;
  prepareFormat();
}

AVMediaExtractor::PlatformResult MediaExtractor::read(PositionHolder& seekPosition) {
  static_cast<void>(seekPosition);
  if (mOutput == nullptr || !extractor) {
    return PlatformResult::RESULT_END_OF_INPUT;
  }

  if (needPrepareFormat) {
    prepareFormat();
  }

  auto byteBuffer = JniByteBuffer::allocate(maxInputSize);
  int sampleSize = extractor->readSampleData(byteBuffer, 0);
  if (sampleSize < 0) {
    return PlatformResult::RESULT_END_OF_INPUT;
  } else {
    if (sampleSize > 0) {
      if (mCurrentPosition == Definitions::kPositionUnset) {
        mCurrentPosition = sampleSize;
      } else {
        mCurrentPosition += sampleSize;
      }

      int trackIndex = extractor->getSampleTrackIndex();
      auto sampleTimeUs = extractor->getSampleTime();
      auto sampleFlags = extractor->getSampleFlags();

      auto dataBuffer = std::vector<uint8_t>(std::min(sampleSize, maxInputSize));
      auto size = byteBuffer->getByteArray(dataBuffer.data(), dataBuffer.size());
      auto flags = AVBufferFlags::kNone;
      if (sampleFlags == JniMediaExtractor::SAMPLE_FLAG_SYNC) {
        flags = AVBufferFlags::kKeyFrame;
      }
      for (auto& track: tracks) {
        if (track.isValid() && track.getTrackIndex() == trackIndex) {
          track.sampleData(dataBuffer.data(), size, sampleTimeUs, flags);
          break;
        }
      }
    }

    extractor->advance();
    return PlatformResult::RESULT_CONTINUE;
  }
}

void MediaExtractor::prepareFormat() {
  if (needPrepareFormat && mOutput != nullptr) {
    if (!extractor) {
      return;
    }
    auto trackCount = extractor->getTrackCount();
    for (int i = 0; i < trackCount; ++i) {
      auto trackFormat = extractor->getTrackFormat(i);
      if (!trackFormat) {
        tracks.emplace_back(i);
        extractor->unselectTrack(i);
      } else {
        // parse the max input size for read sample data
        int intValue;
        if (trackFormat->getInteger(JniMediaFormat::KEY_MAX_INPUT_SIZE, &intValue)) {
          maxInputSize = std::max(maxInputSize, intValue);
        }

        // parse the durationUS for seeking
        int64_t longValue;
        if (trackFormat->getLong(JniMediaFormat::KEY_DURATION, &longValue)) {
          durationUs = std::max(durationUs, longValue);
        }

        std::string mime;
        auto result = trackFormat->getString(JniMediaFormat::KEY_MIME, &mime);
        auto type = MediaType::kMediaTypeInvalid;
        if (result) {
          if (MimeTypes::isAudio(mime)) {
            type = MediaType::kMediaTypeAudio;
          } else if (MimeTypes::isVideo(mime)) {
            type = MediaType::kMediaTypeVideo;
          }
        }
        if (type != MediaType::kMediaTypeInvalid) {
          int trackId = i;
          if (trackFormat->getInteger(JniMediaFormat::KEY_TRACK_ID, &intValue)) {
            trackId = intValue;
          }
          auto trackOutput =
              mOutput->track(trackId, type, AVTrackOutput::emptyConsumer());
          if (trackOutput != nullptr) {
            auto track = MediaTrack(i, trackId, std::move(trackOutput));
            track.changeFormat(MediaCodecUtils::convertToMediaFormat(trackFormat));
            tracks.emplace_back(track);
            extractor->selectTrack(i);
          } else {
            tracks.emplace_back(i);
            extractor->unselectTrack(i);
          }
        } else {
          tracks.emplace_back(i);
          extractor->unselectTrack(i);
        }
      }
    }
    needPrepareFormat = false;
    mOutput->endTracks();
    // The extractor is the seek map of the media it reads, and it is owned by a shared pointer, so the
    // output can keep it alive for as long as it refers to it.
    mOutput->seekMap(shared_from_this());
  }
}

std::shared_ptr<MediaFormat> MediaExtractor::getTrackFormat(int trackIndex) {
  if (!extractor) {
    return nullptr;
  }
  auto trackCount = extractor->getTrackCount();
  if (trackCount <= trackIndex) {
    return nullptr;
  }
  auto trackFormat = extractor->getTrackFormat(trackIndex);
  if (!trackFormat) {
    return nullptr;
  }

  // parse the max input size for read sample data
  int intValue;
  if (trackFormat->getInteger(JniMediaFormat::KEY_MAX_INPUT_SIZE, &intValue)) {
    maxInputSize = std::max(maxInputSize, intValue);
  }

  // parse the durationUS for seeking
  int64_t longValue;
  if (trackFormat->getLong(JniMediaFormat::KEY_DURATION, &longValue)) {
    durationUs = std::max(durationUs, longValue);
  }
  return MediaCodecUtils::convertToMediaFormat(trackFormat);
}

bool MediaExtractor::seekTo(int64_t timeUs, AVMediaExtractor::SeekOption option) {
  if (!extractor) {
    return false;
  }
  extractor->seekTo(timeUs, (JniMediaExtractor::SeekMode) option);
  return true;
}

void MediaExtractor::clear() {
  if (!extractor) {
    return;
  }
  extractor->release();

  for (auto& track: tracks) {
    track.clear();
  }
  tracks.clear();
  needPrepareFormat = true;
}

int64_t MediaExtractor::getDurationUs() const { return durationUs; }

int64_t MediaExtractor::getReadPosition() const { return mCurrentPosition; }

coral::SeekPoints MediaExtractor::getPlatformSeekPoints(const int64_t timeUs) const {
  auto first = coral::SeekPoint(timeUs, INT64_MAX);
  return coral::SeekPoints(first);
}

//--------------------------------------------------------------------------------------------------

void MediaExtractor::MediaTrack::changeFormat(const std::shared_ptr<MediaFormat>& format) {
  if (format == nullptr) {
    return;
  }
  if (trackOutput != nullptr) {
    trackOutput->format(format);
  }
}

void MediaExtractor::MediaTrack::sampleData(const uint8_t* data, int size, int64_t timeUs, AVBufferFlags flags) {
  if (trackOutput != nullptr && size > 0 && data != nullptr) {
    // The sample owns a copy of the bytes of the platform, which are only valid until the next read:
    // the output is free to keep the sample for as long as it needs it.
    trackOutput->sample(AVSample(trackId, timeUs, toSampleFlags(flags),
        std::make_shared<AVBlockBuffer>(data, static_cast<uint32_t>(size))));
  }
}

} // namespace coral
