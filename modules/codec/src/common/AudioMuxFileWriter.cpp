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

#include "AudioMuxFileWriter.h"

#include <codec/MediaCodecFactory.h>
#include <codec/MediaMuxer.h>
#include <codec/Utilities.h>
#include <media/SampleBuffer.h>
#include <media/Time.h>
#include <media/Utilities.h>
#include <minilog/log.h>

#include <sys/stat.h>

#include <sstream>
#include <utility>

namespace coral {

std::unique_ptr<AudioFileWriter>
AudioMuxFileWriter::sniff(const std::string& outputUrl,
  const MediaFileType& fileType,
  const std::shared_ptr<MediaFormat>& format) {
  auto muxerFormat = convertToMuxerFormat(fileType);
  if (!isMuxerFormatSupported(muxerFormat)) {
    return nullptr;
  }

  // Checks whether the platform is able to create a muxer of the format.
  auto muxerFactory = CreateBuiltinMuxerFactory();
  if (muxerFactory == nullptr || !muxerFactory->isSupportMuxerFormat(muxerFormat)) {
    MINILOG_ERROR("the platform doesn't support the muxer format: {}", convertToText(muxerFormat));
    return nullptr;
  }

  // Checks if audio format type is available
  auto AudioFormatType = format->getAudioFormatType();
  if (AudioFormatType == AudioFormatType::kAudioFormatTypeInvalid) {
    AudioFormatType = MediaUtils::getDefaultAudioFormatType(fileType);
  }
  if (!supportAudioEncoding(AudioFormatType)) {
    MINILOG_ERROR("unsupported audio format type: {}", convertToText(AudioFormatType));
    return nullptr;
  }

  // Checks if the format is available, when encoding, only pcm_16bit is available.
  if (format->getSampleFormat() != AudioSampleFormat::kInt16) {
    MINILOG_ERROR("unsupported pcm data format: ｛｝", convertToText(format->getSampleFormat()));
    return nullptr;
  }

  return std::make_unique<AudioMuxFileWriter>(outputUrl, fileType, format);
}

AudioMuxFileWriter::AudioMuxFileWriter(std::string outputUrl,
  const MediaFileType& fileType,
  const std::shared_ptr<MediaFormat>& format) noexcept
  : AudioFileWriter(format->getSampleFormat(), format->getSampleRate(), format->getChannelCount()),
    mOutputUrl(std::move(outputUrl)),
    mFileType(fileType),
    mInputFormat(format),
    mTrackIndex(AVMediaMuxer::kInvalidTrack),
    mMuxerStarted(false),
    mFramePosition(0) {
  mBytesPerFrame = MediaUtils::getBytesPerFrame(getSampleFormat(), getChannelCount());
}

bool AudioMuxFileWriter::open(Error** outError) {
  const auto muxerFormat = convertToMuxerFormat(mFileType);
  if (!isMuxerFormatSupported(muxerFormat)) {
    std::stringstream ss;
    ss << "Unsupported muxer format: " << convertToText(muxerFormat);
    handleError(outError, "Failed to prepare muxer", ss.str().c_str());
    return false;
  }
  mAudioMuxer = CreateBuiltinMuxerFactory()->createMuxer();
  if (mAudioMuxer == nullptr) {
    handleError(outError, "Failed to prepare muxer", "No muxer is available on the platform");
    return false;
  }
  if (const auto result = mAudioMuxer->prepare(mOutputUrl, muxerFormat); !result) {
    handleError(outError, "Failed to prepare muxer");
    return false;
  }

  auto formatType = mInputFormat->getAudioFormatType();
  if (formatType == AudioFormatType::kAudioFormatTypeInvalid ||
    formatType == AudioFormatType::kAudioFormatTypeLinearPCM) {
    formatType = MediaUtils::getDefaultAudioFormatType(mFileType);
  }
  const auto manufacturer = CodecUtils::getAudioCodecManufacturer(formatType, true);
  const auto mimeType = MediaUtils::convertToMimeType(formatType);
  auto mediaFormat =
    MediaFormat::CreateAudioFormat(mimeType, mInputFormat->getSampleRate(), mInputFormat->getChannelCount());

  mAudioEncoder = CreateBuiltinAudioCodecFactory()->createEncoder(formatType, manufacturer);
  if (mAudioEncoder) {
    mAudioEncoder->configure(mInputFormat);
    mAudioEncoder->start();
    return true;
  }
  return false;
}

void AudioMuxFileWriter::close() {
  if (mAudioEncoder) {
    mAudioEncoder->signalEndOfInputStream();
    mAudioEncoder->stop();
    mAudioEncoder->release();
    mAudioEncoder.reset();
  }
  if (mAudioMuxer) {
    mAudioMuxer->stop();
    mAudioMuxer->release();
    mAudioMuxer.reset();
  }
}

int64_t AudioMuxFileWriter::writeFromPCMBuffer(void* inputBuffer, const size_t numFrames, Error** outError) {
  std::shared_ptr<AVSampleBuffer> queuedInputBuffer = nullptr;

  while (true) {
    queuedInputBuffer = mAudioEncoder->dequeueInputBuffer();
    if (queuedInputBuffer != nullptr) {
      const auto bufferSize = numFrames * mBytesPerFrame;
      const auto pts = AVTime(mFramePosition, mInputFormat->getSampleRate());
      queuedInputBuffer->init(pts.timeUs(), bufferSize);
      const auto data = queuedInputBuffer->data();
      data->position(0);
      data->put(static_cast<const uint8_t*>(inputBuffer), bufferSize);
      data->limit(bufferSize);
      mAudioEncoder->queueInputBuffer(queuedInputBuffer);
      mFramePosition += numFrames;
      break;
    }
    tryDequeueOutputBuffer();
  }

  tryDequeueOutputBuffer();
  return numFrames;
}

void AudioMuxFileWriter::tryDequeueOutputBuffer() {
  const auto packetBuffer = mAudioEncoder->dequeueOutputBuffer();
  if (packetBuffer != nullptr) {
    if (packetBuffer->isFormatChanged()) {
      onFormatChanged(packetBuffer->getFormat());
    } else {
      onPacketReady(packetBuffer);
    }
  }
}

void AudioMuxFileWriter::onFormatChanged(const std::shared_ptr<MediaFormat>& format) {
  if (mAudioMuxer != nullptr) {
    mTrackIndex = mAudioMuxer->addTrack(format);
    if (mTrackIndex != AVMediaMuxer::kInvalidTrack) {
      mAudioMuxer->start();
      mMuxerStarted = true;
    }
  }
}

void AudioMuxFileWriter::onPacketReady(const std::shared_ptr<AVPacketBuffer>& packetBuffer) {
  if (!packetBuffer || packetBuffer->isDiscard()) {
    return;
  }
  if (mAudioMuxer != nullptr && mMuxerStarted) {
    const auto endOfStream = packetBuffer->isEndOfStream();
    packetBuffer->setStreamIndex(mTrackIndex);
    mAudioMuxer->writeFrame(packetBuffer);
    if (endOfStream) {
      mAudioMuxer->stop();
      mAudioMuxer->release();
      mAudioMuxer.reset();
    }
  }
}

bool AudioMuxFileWriter::supportAudioEncoding(const AudioFormatType& codecType) {
  if (codecType == AudioFormatType::kAudioFormatTypeMpeg4AAC) {
    return true;
  }
  return false;
}

bool AudioMuxFileWriter::isMuxerFormatSupported(const MuxerFormat& format) {
  if (format == MuxerFormat::kMuxerFormatMpeg4 ||
      format == MuxerFormat::kMuxerFormatWebm ||
      format == MuxerFormat::kMuxerFormat3Gpp) {
    return true;
  }
  return false;
}

MuxerFormat AudioMuxFileWriter::convertToMuxerFormat(const MediaFileType& fileType) {
  switch (fileType) {
    case MediaFileType::kMediaFileTypeMpeg4AAC:
    case MediaFileType::kMediaFileTypeMpeg4Video: {
      return MuxerFormat::kMuxerFormatMpeg4;
    }
    case MediaFileType::kMediaFileTypeH263: {
      return MuxerFormat::kMuxerFormatH263;
    }
    case MediaFileType::kMediaFileTypeH264: {
      return MuxerFormat::kMuxerFormatH264;
    }
    case MediaFileType::kMediaFileTypeFlac: {
      return MuxerFormat::kMuxerFormatFlac;
    }
    case MediaFileType::kMediaFileTypeOpus: {
      return MuxerFormat::kMuxerFormatOpus;
    }
    case MediaFileType::kMediaFileTypeWav:
    default: {
      return MuxerFormat::kMuxerFormatInvalid;
    }
  }
}

} // namespace coral
