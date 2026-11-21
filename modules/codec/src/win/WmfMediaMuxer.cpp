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

#include "WmfMediaMuxer.h"

#include "WmfMediaUtils.h"

#include <media/Definitions.h>
#include <minilog/log.h>

#include <algorithm>
#include <limits>
#include <utility>

namespace coral {

namespace {

// The number of the audio frames which are contained by one aac packet.
constexpr int AAC_FRAMES_PER_PACKET = 1024;

constexpr int64_t HNS_PER_SECOND = 10000000;

constexpr int DEFAULT_AUDIO_BIT_RATE = 128000;

//!< The window in which the samples of the tracks are reordered, in microseconds.
constexpr int64_t REORDER_WINDOW_US = 1000000;

} // namespace anonymous

WmfMediaMuxer::WmfMediaMuxer() {
  mWmfStarted = WmfMediaUtils::startup();
}

WmfMediaMuxer::~WmfMediaMuxer() {
  release();
  if (mWmfStarted) {
    WmfMediaUtils::shutdown();
  }
}

bool WmfMediaMuxer::isSupportedFormat(const MuxerFormat format) {
  GUID containerType{};
  return convertToContainerType(format, containerType);
}

bool WmfMediaMuxer::convertToContainerType(const MuxerFormat format, GUID& containerType) {
  switch (format) {
    case MuxerFormat::kMuxerFormatMpeg4: {
      containerType = MFTranscodeContainerType_MPEG4;
      return true;
    }
    case MuxerFormat::kMuxerFormat3Gpp: {
      containerType = MFTranscodeContainerType_3GP;
      return true;
    }
    default: {
      return false;
    }
  }
}

bool WmfMediaMuxer::prepare(std::string url, const MuxerFormat format) {
  if (!mSinkWriter && !mWmfStarted) {
    MINILOG_ERROR("the media foundation platform is not available");
    return false;
  }
  if (url.empty()) {
    MINILOG_ERROR("the output url is empty");
    return false;
  }

  GUID containerType{};
  if (!convertToContainerType(format, containerType)) {
    MINILOG_ERROR("the platform does not support the muxer format: {}", convertToText(format));
    return false;
  }

  Microsoft::WRL::ComPtr<IMFAttributes> attributes;
  auto result = ::MFCreateAttributes(&attributes, 3);
  if (FAILED(result)) {
    WmfMediaUtils::handleHresult(nullptr, result, "MFCreateAttributes failed");
    return false;
  }
  attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, containerType);
  attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
  attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);

  const auto wideUrl = WmfMediaUtils::toSinkWriterUrl(url);
  result = ::MFCreateSinkWriterFromURL(wideUrl.c_str(), nullptr, attributes.Get(), &mSinkWriter);
  if (FAILED(result) || mSinkWriter == nullptr) {
    mSinkWriter.Reset();
    WmfMediaUtils::handleHresult(nullptr, result, "MFCreateSinkWriterFromURL failed", url);
    return false;
  }

  mUrl = std::move(url);
  mFormat = format;
  mTracks.clear();
  mFinalized = false;
  mStatus = Status::kMuxerStatusPrepared;
  return true;
}

Microsoft::WRL::ComPtr<IMFMediaType>
WmfMediaMuxer::createTrackMediaType(const std::shared_ptr<MediaFormat>& format) {
  if (format == nullptr) {
    return nullptr;
  }
  GUID subType{};
  if (!WmfMediaUtils::mimeTypeToSubtype(format->getMimeType(), subType)) {
    MINILOG_ERROR("the track mime type is not supported: {}", format->getMimeType());
    return nullptr;
  }

  Microsoft::WRL::ComPtr<IMFMediaType> mediaType;
  if (FAILED(::MFCreateMediaType(&mediaType)) || mediaType == nullptr) {
    return nullptr;
  }

  const auto& csdList = format->csdList();
  const auto hasCodecSpecificData = !csdList.empty() && !csdList[0].empty();

  if (format->isAudio()) {
    WmfMediaUtils::fillAudioMediaType(mediaType.Get(), format, subType);
    mediaType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    mediaType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 0);

    if (subType == MFAudioFormat_AAC) {
      if (hasCodecSpecificData) {
        const auto blob = WmfMediaUtils::createAacUserData(csdList[0]);
        mediaType->SetBlob(MF_MT_USER_DATA, blob.data(), static_cast<UINT32>(blob.size()));
      }
      mediaType->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
      mediaType->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
      auto bitrate = format->getBitrate();
      if (bitrate == MediaFormat::NO_VALUE || bitrate <= 0) {
        bitrate = DEFAULT_AUDIO_BIT_RATE;
      }
      mediaType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
        static_cast<UINT32>(bitrate / 8));
    }
  } else if (format->isVideo()) {
    WmfMediaUtils::fillVideoMediaType(mediaType.Get(), format, subType);
    if (hasCodecSpecificData) {
      // The platform expects the parameter sets of the annex b byte stream.
      mediaType->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, csdList[0].data(),
        static_cast<UINT32>(csdList[0].size()));
    }
  } else {
    MINILOG_ERROR("the track media type is not supported");
    return nullptr;
  }

  return mediaType;
}

int WmfMediaMuxer::addTrack(const std::shared_ptr<MediaFormat>& format) {
  if (mSinkWriter == nullptr || format == nullptr) {
    return kInvalidTrack;
  }
  if (mStatus != Status::kMuxerStatusPrepared) {
    MINILOG_ERROR("the track must be added before the muxer is started");
    return kInvalidTrack;
  }

  const auto mediaType = createTrackMediaType(format);
  if (mediaType == nullptr) {
    return kInvalidTrack;
  }

  DWORD streamIndex = 0;
  auto result = mSinkWriter->AddStream(mediaType.Get(), &streamIndex);
  if (FAILED(result)) {
    WmfMediaUtils::handleHresult(nullptr, result, "IMFSinkWriter::AddStream failed",
      format->toString());
    return kInvalidTrack;
  }

  // The packets of the track are already encoded, so the media type of the samples which are
  // written is the media type of the stream itself.
  result = mSinkWriter->SetInputMediaType(streamIndex, mediaType.Get(), nullptr);
  if (FAILED(result)) {
    WmfMediaUtils::handleHresult(nullptr, result, "IMFSinkWriter::SetInputMediaType failed",
      format->toString());
    return kInvalidTrack;
  }

  TrackInfo info;
  info.video = format->isVideo();
  if (info.video) {
    const auto frameRate = format->getFrameRate();
    if (frameRate != MediaFormat::NO_VALUE && frameRate > 0) {
      info.packetDuration100ns = HNS_PER_SECOND / frameRate;
    }
  } else {
    GUID subType{};
    const auto isAac = WmfMediaUtils::mimeTypeToSubtype(format->getMimeType(), subType) &&
                       subType == MFAudioFormat_AAC;
    const auto sampleRate = format->getSampleRate();
    if (isAac && sampleRate > 0) {
      info.packetDuration100ns =
        static_cast<int64_t>(AAC_FRAMES_PER_PACKET) * HNS_PER_SECOND / sampleRate;
    }
  }

  if (mTracks.size() <= streamIndex) {
    mTracks.resize(streamIndex + 1);
  }
  mTracks[streamIndex] = info;

  return static_cast<int>(streamIndex);
}

void WmfMediaMuxer::writeFrame(const std::shared_ptr<AVPacketBuffer>& buffer) {
  if (mSinkWriter == nullptr || buffer == nullptr || !isStarted()) {
    return;
  }
  if (buffer->isCodecConfig() || buffer->isEndOfStream() || buffer->size() <= 0 ||
      buffer->streamIndex() < 0) {
    return;
  }

  const auto streamIndex = static_cast<DWORD>(buffer->streamIndex());
  if (streamIndex >= mTracks.size()) {
    MINILOG_ERROR("the stream index is invalid: {}", buffer->streamIndex());
    return;
  }

  auto sample = WmfMediaUtils::createSample(buffer->array(), buffer->size(), buffer->timeUs(),
    static_cast<uint32_t>(buffer->isKeyFrame()
                            ? AVBuffer::kKeyFrame
                            : AVBuffer::kNone));
  if (sample == nullptr) {
    return;
  }
  const auto& info = mTracks[streamIndex];
  if (info.packetDuration100ns > 0) {
    sample->SetSampleDuration(info.packetDuration100ns);
  }

  // The samples are held back by the reorder window first, the sink writer drops the samples
  // which are older than the samples it has already received.
  PendingSample pending;
  pending.streamIndex = streamIndex;
  pending.timeUs = buffer->timeUs();
  pending.sample = sample;

  const auto position = std::upper_bound(mPendingSamples.begin(), mPendingSamples.end(),
    pending.timeUs,
    [](const int64_t timeUs, const PendingSample& item) {
      return timeUs < item.timeUs;
    });
  mPendingSamples.insert(position, std::move(pending));
  if (buffer->timeUs() > mMaxPendingTimeUs) {
    mMaxPendingTimeUs = buffer->timeUs();
  }

  flushPendingSamples(false);
}

void WmfMediaMuxer::flushPendingSamples(const bool flushAll) {
  if (mSinkWriter == nullptr) {
    mPendingSamples.clear();
    return;
  }
  // A sample is written once the reorder window has passed, so the samples of the other tracks
  // which belong before it can still be received.
  const auto threshold = flushAll || mMaxPendingTimeUs == Definitions::kTimeUnset
                           ? std::numeric_limits<int64_t>::max()
                           : mMaxPendingTimeUs - REORDER_WINDOW_US;

  auto written = mPendingSamples.begin();
  while (written != mPendingSamples.end() && written->timeUs <= threshold) {
    const auto result = mSinkWriter->WriteSample(written->streamIndex, written->sample.Get());
    if (FAILED(result)) {
      WmfMediaUtils::handleHresult(nullptr, result, "IMFSinkWriter::WriteSample failed");
    }
    ++written;
  }
  mPendingSamples.erase(mPendingSamples.begin(), written);
}

void WmfMediaMuxer::start() {
  if (isStarted()) {
    return;
  }
  if (mSinkWriter != nullptr) {
    const auto result = mSinkWriter->BeginWriting();
    if (FAILED(result)) {
      WmfMediaUtils::handleHresult(nullptr, result, "IMFSinkWriter::BeginWriting failed");
      mStatus = Status::kMuxerStatusError;
      return;
    }
  }
  mPendingSamples.clear();
  mMaxPendingTimeUs = Definitions::kTimeUnset;
  mStatus = Status::kMuxerStatusStarted;
}

void WmfMediaMuxer::stop() {
  if (!isStarted()) {
    return;
  }
  if (mSinkWriter != nullptr && !mFinalized) {
    // every sample which is still held back is written before the file is finalized.
    flushPendingSamples(true);
    const auto result = mSinkWriter->Finalize();
    if (FAILED(result)) {
      WmfMediaUtils::handleHresult(nullptr, result, "IMFSinkWriter::Finalize failed");
      mStatus = Status::kMuxerStatusError;
      return;
    }
    mFinalized = true;
  }
  mStatus = Status::kMuxerStatusStopped;
}

void WmfMediaMuxer::release() {
  if (isStarted()) {
    stop();
  }
  mPendingSamples.clear();
  mMaxPendingTimeUs = Definitions::kTimeUnset;
  mSinkWriter.Reset();
  mTracks.clear();
  mFinalized = false;
  mStatus = Status::kMuxerStatusIdle;
}

} // namespace coral
