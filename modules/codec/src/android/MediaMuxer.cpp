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

#include "MediaMuxer.h"

#include "MediaCodecUtils.h"

#include <core/jni/android/MediaCodec.h>

namespace coral {

MediaMuxer::MediaMuxer() {
}

bool MediaMuxer::prepare(std::string url, const MuxerFormat format) {
  if (!supportMuxerFormat(format)) {
    return false;
  }

  muxer = JniMediaMuxer::CreateWithUrl(url, convertMuxerFormat(format));

  mStatus = Status::kMuxerStatusPrepared;
  return true;
}

int MediaMuxer::addTrack(const std::shared_ptr<MediaFormat>& format) {
  if (!muxer || !format) {
    return kInvalidTrack;
  }

  // handle rotation for media muxer
  if (format->isVideo() && format->getRotation() != Definitions::kInvalidValue) {
    muxer->setOrientationHint(format->getRotation());
  }

  // handle max input size for allocation of ByteBuffer
  if (format->getMaxInputSize() != Definitions::kInvalidValue) {
    maxInputSize = std::max(maxInputSize, format->getMaxInputSize());
  }

  auto jniFormat = MediaCodecUtils::convertToJniMediaFormat(format);
  if (!jniFormat) {
    return kInvalidTrack;
  }
  return muxer->addTrack(jniFormat);
}

void MediaMuxer::writeFrame(const std::shared_ptr<AVPacketBuffer>& buffer) {
  if (!muxer || buffer->isCodecConfig() || buffer->size() <= 0 || buffer->streamIndex() < 0) {
    return;
  }

  auto inputBuffer = buffer->array();
  auto inputSize = buffer->size();
  maxInputSize = std::max(maxInputSize, (int) inputSize);

  int flags = 0;
  if (buffer->isEndOfStream()) {
    flags |= JniMediaCodec::BUFFER_FLAG_END_OF_STREAM;
  } else if (buffer->isCodecConfig()) {
    flags |= JniMediaCodec::BUFFER_FLAG_CODEC_CONFIG;
  } else if (buffer->isKeyFrame()) {
    flags |= JniMediaCodec::BUFFER_FLAG_KEY_FRAME;
  }

  auto byteBuffer = JniByteBuffer::allocate(inputSize);
  byteBuffer->writeByteArray(inputBuffer, inputSize);

  JniMediaCodec::BufferInfo bufferInfo;
  bufferInfo.flags = flags;
  bufferInfo.offset = 0;
  bufferInfo.presentationTimeUs = buffer->timeUs();
  bufferInfo.size = inputSize;

  muxer->writeSampleData(buffer->streamIndex(), byteBuffer, bufferInfo);
}

void MediaMuxer::start() {
  if (isStarted()) {
    return;
  }
  mStatus = Status::kMuxerStatusStarted;
  if (muxer) {
    muxer->start();
  }
}

void MediaMuxer::stop() {
  if (!isStarted()) {
    return;
  }
  mStatus = Status::kMuxerStatusStopped;
  if (muxer) {
    muxer->stop();
  }
}

void MediaMuxer::release() {
  mStatus = Status::kMuxerStatusIdle;
  if (muxer) {
    muxer->release();
  }
}

bool MediaMuxer::supportMuxerFormat(const MuxerFormat format) {
  if (format == MuxerFormat::kMuxerFormatMpeg4
      || format == MuxerFormat::kMuxerFormatWebm
      || format == MuxerFormat::kMuxerFormat3Gpp
      || format == MuxerFormat::kMuxerFormatOgg) {
    return true;
  }
  return false;
}

JniMediaMuxer::OutputFormat MediaMuxer::convertMuxerFormat(const MuxerFormat format) {
  switch (format) {
    case MuxerFormat::kMuxerFormatMpeg4: {
      return JniMediaMuxer::MUXER_OUTPUT_MPEG_4;
    }
    case MuxerFormat::kMuxerFormatWebm: {
      return JniMediaMuxer::MUXER_OUTPUT_WEBM;
    }
    case MuxerFormat::kMuxerFormat3Gpp: {
      return JniMediaMuxer::MUXER_OUTPUT_3GPP;
    }
    case MuxerFormat::kMuxerFormatOgg: {
      return JniMediaMuxer::MUXER_OUTPUT_OGG;
    }
    default: {
      return JniMediaMuxer::MUXER_OUTPUT_MPEG_4;
    }
  }
}

} // namespace coral
