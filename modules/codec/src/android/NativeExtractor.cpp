/*
 * Copyright (C) 2023 The Coral Project
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

#include "NativeExtractor.h"

#include <media/Types.h>

#include <minilog/log.h>

#include <array>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

namespace coral {

std::shared_ptr<NativeExtractor>
NativeExtractor::CreateWithUrl(const std::string& url) {
  auto extractor = std::make_shared<NativeExtractor>();
  auto result = extractor->setDataSource(url);
  if (!result) {
    extractor.reset();
    return nullptr;
  }
  return extractor;
}

std::shared_ptr<NativeExtractor>
NativeExtractor::CreateForInput(std::string url) {
  static_cast<void>(url);
  if (__builtin_available(android 28, *)) {
    // Nothing is opened here: the media is the one of the reader, which is given to the extractor when
    // it is asked to recognize the container.
    return std::make_shared<NativeExtractor>();
  }
  // The data source of the NDK that reads the media of the reader does not exist on this platform, so
  // the caller has to fall back to an extractor that opens the media of the url.
  return nullptr;
}

int NativeExtractor::sniff(AVExtractorInput& input) {
  if (mExtractor != nullptr) {
    // The media was opened by the url this extractor was created with, so it reads itself and it
    // recognizes the container it was created for.
    return AVExtractorsFactory::kSniffResultPossible;
  }
  // The media is the one of the reader: the extractor of the platform reads it through the data source
  // of this extractor, which is what makes the bytes it consumed the bytes the reader measures.
  if (!openFromInput(input)) {
    return AVExtractorsFactory::kSniffResultNone;
  }
  return AVExtractorsFactory::kSniffResultCertain;
}

bool NativeExtractor::openFromInput(AVExtractorInput& input) {
  if (mExtractor != nullptr) {
    return true;
  }
  auto extractor = AMediaExtractor_new();
  if (extractor == nullptr) {
    MINILOG_ERROR("Failed to create the extractor of the media of the reader");
    return false;
  }
  mExtractor.reset(extractor);

  mInput = std::make_unique<AVMediaExtractorInput>(input);
  if (__builtin_available(android 28, *)) {
    mDataSource = AMediaDataSource_new();
    if (mDataSource == nullptr) {
      MINILOG_ERROR("Failed to create the data source of the media of the reader");
      mExtractor.reset();
      mInput.reset();
      return false;
    }
    AMediaDataSource_setUserdata(mDataSource, this);
    AMediaDataSource_setReadAt(mDataSource, &NativeExtractor::readAt);
    AMediaDataSource_setGetSize(mDataSource, &NativeExtractor::getSize);
    AMediaDataSource_setClose(mDataSource, &NativeExtractor::closeSource);

    const auto status = AMediaExtractor_setDataSourceCustom(mExtractor.get(), mDataSource);
    if (status != AMEDIA_OK) {
      MINILOG_ERROR("Failed to set the data source of the media of the reader: {}",
          static_cast<int>(status));
      AMediaDataSource_delete(mDataSource);
      mDataSource = nullptr;
      mExtractor.reset();
      mInput.reset();
      return false;
    }
    return true;
  }
  // The data source of the NDK that reads the media of the reader does not exist on this platform.
  mInput.reset();
  mExtractor.reset();
  return false;
}

ssize_t NativeExtractor::readAt(void* userdata, const off64_t position, void* buffer,
    const size_t size) {
  auto* extractor = static_cast<NativeExtractor*>(userdata);
  if (extractor == nullptr || extractor->mInput == nullptr || buffer == nullptr || size == 0) {
    return 0;
  }
  // The data source of the platform is read at the position it asks for, so the input of the reader is
  // moved there first: it is the media the reader measures, and the position it holds follows the reads
  // of the platform.
  if (!extractor->mInput->setPosition(static_cast<long>(position), true)) {
    return 0;
  }
  const long read = extractor->mInput->read(static_cast<uint8_t*>(buffer), 0, size);
  return read == AVMediaExtractorInput::kEndOfInput ? 0 : read;
}

ssize_t NativeExtractor::getSize(void* userdata) {
  auto* extractor = static_cast<NativeExtractor*>(userdata);
  if (extractor == nullptr || extractor->mInput == nullptr) {
    return -1;
  }
  return extractor->mInput->getLength();
}

void NativeExtractor::closeSource(void* userdata) {
  // The input of the reader is owned by the reader, which outlives the extractor: there is nothing to
  // release here.
  static_cast<void>(userdata);
}

NativeExtractor::NativeExtractor()
  : mOutput(nullptr),
    needPrepareFormat(true),
    mExtractor(nullptr),
    mCurrentPosition(Definitions::kPositionUnset) {
  mData = std::make_unique<uint8_t[]>(maxInputSize);
}

NativeExtractor::~NativeExtractor() {
  mExtractor.reset();
  mOutput = nullptr;
}

bool NativeExtractor::setDataSource(const std::string& url) {
  struct stat statBuffer{};
  if (stat(url.c_str(), &statBuffer) != 0) {
    // The full-width braces that used to be here are not format placeholders, so the error string
    // was never printed.
    MINILOG_ERROR("Failed to state file: {}, error: {}", url, strerror(errno));
    return false;
  }

  // open a local file url
  auto fd = ::open(url.c_str(), O_RDONLY);
  if (fd < 0) {
    return false;
  }

  // creates an AMediaExtractor
  auto extractor = AMediaExtractor_new();
  media_status_t err = AMediaExtractor_setDataSourceFd(extractor, fd, 0, statBuffer.st_size);
  ::close(fd);
  if (err != AMEDIA_OK) {
    AMediaExtractor_delete(extractor);
    return false;
  }
  mExtractor.reset(extractor);
  return true;
}

void NativeExtractor::init(AVExtractorOutput& output) {
  mOutput = &output;
  prepareFormat();
}

AVMediaExtractor::PlatformResult NativeExtractor::read(PositionHolder& seekPosition) {
  static_cast<void>(seekPosition);
  if (mOutput == nullptr || !mExtractor) {
    return PlatformResult::RESULT_END_OF_INPUT;
  }

  if (needPrepareFormat) {
    prepareFormat();
  }

  auto sampleSize = AMediaExtractor_readSampleData(mExtractor.get(),
    mData.get(), maxInputSize);
  if (sampleSize < 0) {
    return PlatformResult::RESULT_END_OF_INPUT;
  }

  if (sampleSize > 0) {
    int trackIndex = AMediaExtractor_getSampleTrackIndex(mExtractor.get());
    auto sampleTimeUs = AMediaExtractor_getSampleTime(mExtractor.get());
    auto sampleFlag = AMediaExtractor_getSampleFlags(mExtractor.get());
    auto flags = AVBufferFlags::kNone;
    if (sampleFlag == AMEDIAEXTRACTOR_SAMPLE_FLAG_SYNC) {
      flags = AVBufferFlags::kKeyFrame;
    }
    if (mCurrentPosition == Definitions::kPositionUnset) {
      mCurrentPosition = sampleSize;
    } else {
      mCurrentPosition += sampleSize;
    }
    // copy sample to output
    for (auto& track: tracks) {
      if (track.isValid() && track.getTrackIndex() == trackIndex) {
        track.sampleData(mData.get(), sampleSize, sampleTimeUs, flags);
        break;
      }
    }
  }

  AMediaExtractor_advance(mExtractor.get());
  return PlatformResult::RESULT_CONTINUE;
}

bool NativeExtractor::seekTo(int64_t timeUs, AVMediaExtractor::SeekOption option) {
  if (!mExtractor) {
    return false;
  }
  auto err = AMediaExtractor_seekTo(mExtractor.get(), timeUs, (SeekMode) option);
  return err == AMEDIA_OK;
}

void NativeExtractor::clear() {
  mExtractor.reset();

  // The data source of the media of the reader is created by this extractor, so it is the one that
  // deletes it: the extractor that reads through it is already released.
  if (mDataSource != nullptr) {
    if (__builtin_available(android 28, *)) {
      AMediaDataSource_delete(mDataSource);
    }
    mDataSource = nullptr;
  }
  mInput.reset();

  for (auto& track: tracks) {
    track.clear();
  }
  tracks.clear();
  needPrepareFormat = true;
}

int64_t NativeExtractor::getDurationUs() const { return durationUs; }

void NativeExtractor::prepareFormat() {
  if (needPrepareFormat && mOutput != nullptr) {
    if (!mExtractor) {
      return;
    }
    auto trackCount = getTrackCount();
    for (int i = 0; i < trackCount; ++i) {
      auto format = AMediaExtractor_getTrackFormat(mExtractor.get(), i);
      if (format == nullptr) {
        AMediaExtractor_unselectTrack(mExtractor.get(), i);
        continue;
      }
      const char* mime;
      int intValue;
      int64_t longValue;

      if (AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_MAX_INPUT_SIZE,
        &intValue)) {
        if (intValue > maxInputSize) {
          maxInputSize = intValue;
          mData.reset();
          mData = std::make_unique<uint8_t[]>(maxInputSize);
        }
      }
      if (AMediaFormat_getInt64(format, AMEDIAFORMAT_KEY_DURATION,
        &longValue)) {
        durationUs = std::max(durationUs, longValue);
      }

      auto type = MediaType::kMediaTypeInvalid;
      if (AMediaFormat_getString(format, AMEDIAFORMAT_KEY_MIME, &mime)) {
        if (MimeTypes::isAudio(mime)) {
          type = MediaType::kMediaTypeAudio;
        } else if (MimeTypes::isVideo(mime)) {
          type = MediaType::kMediaTypeVideo;
        }
      }

      if (type != MediaType::kMediaTypeInvalid) {
        int trackId = i;
        if (AMediaFormat_getInt32(format, /*AMEDIAFORMAT_KEY_TRACK_ID*/ "track-id", &intValue)) {
          trackId = intValue;
        }
        auto trackOutput =
            mOutput->track(trackId, type, AVTrackOutput::emptyConsumer());
        if (trackOutput != nullptr) {
          auto track = MediaTrack(i, trackId, std::move(trackOutput));
          track.changeFormat(getTrackFormat(i));
          tracks.emplace_back(track);
          AMediaExtractor_selectTrack(mExtractor.get(), i);
        } else {
          AMediaExtractor_unselectTrack(mExtractor.get(), i);
        }
      } else {
        tracks.emplace_back(i);
        AMediaExtractor_unselectTrack(mExtractor.get(), i);
      }

      AMediaFormat_delete(format);
    }
    needPrepareFormat = false;
    mOutput->endTracks();
    // The extractor is the seek map of the media it reads, and it is owned by a shared pointer, so the
    // output can keep it alive for as long as it refers to it.
    mOutput->seekMap(shared_from_this());
  }
}

int NativeExtractor::getTrackCount() {
  if (mExtractor) {
    return (int) AMediaExtractor_getTrackCount(mExtractor.get());
  }
  return 0;
}

std::shared_ptr<MediaFormat> NativeExtractor::getTrackFormat(int trackIndex) {
  if (!mExtractor) {
    return nullptr;
  }
  auto trackCount = getTrackCount();
  if (trackCount <= trackIndex) {
    return nullptr;
  }
  auto format = AMediaExtractor_getTrackFormat(mExtractor.get(), trackIndex);
  if (format == nullptr) {
    return nullptr;
  }

  std::shared_ptr<MediaFormat> outputFormat;
  const char* stringValue;
  int32_t intValue;
  int64_t longValue;
  if (!AMediaFormat_getString(format, AMEDIAFORMAT_KEY_MIME, &stringValue)) {
    AMediaFormat_delete(format);
    return nullptr;
  }
  auto mimeType = stringValue;
  if (MimeTypes::isAudio(mimeType)) {
    int sampleRate = 0;
    int channelCount = 0;
    if (AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_SAMPLE_RATE,
      &intValue)) {
      sampleRate = intValue;
    }
    if (AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_CHANNEL_COUNT,
      &intValue)) {
      channelCount = intValue;
    }

    outputFormat = MediaFormat::CreateAudioFormat(mimeType, sampleRate, channelCount);
    if (AMediaFormat_getInt32(format, "pcm-encoding", &intValue)) {
      AudioSampleFormat sampleFormat;
      switch (intValue) {
        case ENCODING_PCM_16BIT: {
          sampleFormat = AudioSampleFormat::kInt16;
          break;
        }
        case ENCODING_PCM_FLOAT: {
          sampleFormat = AudioSampleFormat::kFloat;
          break;
        }
        case ENCODING_PCM_24BIT_PACKED: {
          sampleFormat = AudioSampleFormat::kInt24;
          break;
        }
        case ENCODING_PCM_32BIT: {
          sampleFormat = AudioSampleFormat::kInt32;
          break;
        }
        default: {
          sampleFormat = AudioSampleFormat::kInt16;
          break;
        }
      }
      outputFormat->setSampleFormat(sampleFormat);
    } else if (AMediaFormat_getInt32(format, "bits-per-sample", &intValue)) {
      if (intValue == 16) {
        outputFormat->setSampleFormat(AudioSampleFormat::kInt16);
      } else if (intValue == 24) {
        outputFormat->setSampleFormat(AudioSampleFormat::kInt24);
      } else if (intValue == 32) {
        outputFormat->setSampleFormat(AudioSampleFormat::kInt32);
      }
    } else {
      // If "pcm-encoding" is missing, the raw audio sample format is signed 16-bit short.
      outputFormat->setSampleFormat(AudioSampleFormat::kInt16);
    }
  } else if (MimeTypes::isVideo(mimeType)) {
    int width = 0;
    int height = 0;
    if (AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_WIDTH, &intValue)) {
      width = intValue;
    }
    if (AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_HEIGHT, &intValue)) {
      height = intValue;
    }
    outputFormat = MediaFormat::CreateVideoFormat(mimeType, width, height);

    if (AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL,
      &intValue)) {
      outputFormat->setIFrameInterval(intValue);
    }
    if (AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_FRAME_RATE, &intValue)) {
      outputFormat->setFrameRate(intValue);
    }
    if (AMediaFormat_getInt32(format, /*AMEDIAFORMAT_KEY_ROTATION*/ "rotation-degrees", &intValue)) {
      outputFormat->setRotation(intValue);
    }
  } else {
    // other tracks unavailable.
    AMediaFormat_delete(format);
    return nullptr;
  }

  // container mime type
  if (AMediaFormat_getString(format, "file-format", &stringValue)) {
    outputFormat->setContainerMimeType(stringValue);
  }

  if (AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_MAX_INPUT_SIZE,
    &intValue)) {
    outputFormat->setMaxInputSize(intValue);
  }
  if (AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_BIT_RATE, &intValue)) {
    outputFormat->setAverageBitrate(intValue);
  }
  if (AMediaFormat_getInt32(format, /*AMEDIAFORMAT_KEY_TRACK_ID*/ "track-id",
    &intValue)) {
    outputFormat->setTrackId(intValue);
  }
  if (AMediaFormat_getInt32(format, /*AMEDIAFORMAT_KEY_PROFILE*/ "profile",
    &intValue)) {
    outputFormat->setProfile(intValue);
  }
  if (AMediaFormat_getInt32(format, /*AMEDIAFORMAT_KEY_LEVEL*/ "level",
    &intValue)) {
    outputFormat->setLevel(intValue);
  }
  if (AMediaFormat_getInt32(
    format, /*AMEDIAFORMAT_KEY_FRAME_COUNT*/ "frame-count", &intValue)) {
    outputFormat->setFrameCount(intValue);
  }
  if (AMediaFormat_getInt64(format, AMEDIAFORMAT_KEY_DURATION, &longValue)) {
    outputFormat->setDurationUs(longValue);
  }

  // handle codec-specific data, for more information refers to:
  // https://developer.android.com/reference/android/media/MediaCodec#CSD
  std::array<std::string, 3> codec_specific_keys = { "csd-0", "csd-1", "csd-2" };
  void* data = nullptr;
  size_t size = 0;

  for (int i = 0; i < codec_specific_keys.size(); ++i) {
    if (AMediaFormat_getBuffer(format, codec_specific_keys[i].c_str(), &data, &size)) {
      if (data && size > 0) {
        outputFormat->setCodecSpecificData(i, (const uint8_t*) data, size);
      }
    } else {
      break;
    }
  }

  AMediaFormat_delete(format);
  return outputFormat;
}

int64_t NativeExtractor::getReadPosition() const { return mCurrentPosition; }

coral::SeekPoints NativeExtractor::getPlatformSeekPoints(const int64_t timeUs) const {
  auto first = coral::SeekPoint(timeUs, INT64_MAX);
  return coral::SeekPoints(first);
}

void NativeExtractor::MediaTrack::changeFormat(const std::shared_ptr<MediaFormat>& format) const {
  MINILOG_DEBUG("format is changed: {}, format: {}", (trackOutput != nullptr), format->toString());
  if (trackOutput != nullptr) {
    trackOutput->format(format);
  }
}

void NativeExtractor::MediaTrack::sampleData(const uint8_t* data, int size, int64_t timeUs,
  AVBufferFlags flags) const {
  if (trackOutput != nullptr && size > 0 && data != nullptr) {
    // The sample owns a copy of the bytes of the platform, which are only valid until the next read:
    // the output is free to keep the sample for as long as it needs it.
    trackOutput->sample(AVSample(trackId, timeUs, toSampleFlags(flags),
        std::make_shared<AVBlockBuffer>(data, static_cast<uint32_t>(size))));
  }
}

} // namespace coral
