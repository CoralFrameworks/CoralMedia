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

#include "NativeMuxer.h"

#include "MediaCodecUtils.h"

#include <minilog/log.h>

#include <cstdio>

namespace coral {

NativeMuxer::NativeMuxer() = default;

NativeMuxer::~NativeMuxer() = default;

bool NativeMuxer::prepare(std::string url, const MuxerFormat format) {
  OutputFormat outputFormat;
  switch (format) {
    case MuxerFormat::kMuxerFormatMpeg4: {
      outputFormat = AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4;
      break;
    }
    case MuxerFormat::kMuxerFormatWebm: {
      outputFormat = AMEDIAMUXER_OUTPUT_FORMAT_WEBM;
      break;
    }
    case MuxerFormat::kMuxerFormat3Gpp: {
      outputFormat = AMEDIAMUXER_OUTPUT_FORMAT_THREE_GPP;
      break;
    }
    case MuxerFormat::kMuxerFormatOgg: {
      outputFormat = AMEDIAMUXER_OUTPUT_FORMAT_OGG;
      break;
    }
    case MuxerFormat::kMuxerFormatOpus:
    case MuxerFormat::kMuxerFormatFlac:
    case MuxerFormat::kMuxerFormatH263:
    case MuxerFormat::kMuxerFormatH264:
    default: {
      MINILOG_ERROR("Unsupported MuxerFormat: {}", convertToText(format));
      return false;
    }
  }

  FILE* fp = fopen(url.c_str(), "wb");
  if (fp != nullptr) {
    mFd = fileno(fp);
  } else {
    mFd = -1;
    MINILOG_ERROR("Failed to create file: {}", url);
    return false;
  }

  mediaMuxer.reset(AMediaMuxer_new(mFd, outputFormat));
  mStatus = Status::kMuxerStatusPrepared;

  fclose(fp);

  return true;
}

int NativeMuxer::addTrack(const std::shared_ptr<MediaFormat>& format) {
  if (mediaMuxer) {
    auto codecFormat = MediaCodecUtils::convertToNdkMediaFormat(format);
    auto trackIndex = AMediaMuxer_addTrack(mediaMuxer.get(), codecFormat);
    AMediaFormat_delete(codecFormat);
    return trackIndex;
  }
  return kInvalidTrack;
}

void NativeMuxer::writeFrame(const std::shared_ptr<AVPacketBuffer>& buffer) {
  if (!mediaMuxer || buffer->isCodecConfig() || buffer->size() <= 0 || buffer->streamIndex() < 0) {
    return;
  }

  bufferInfo.flags = 0;
  if (buffer->isEndOfStream()) {
    bufferInfo.flags |= AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM;
  } else if (buffer->isCodecConfig()) {
    bufferInfo.flags |= AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG;
  } else if (buffer->isKeyFrame()) {
    bufferInfo.flags |= AMEDIACODEC_BUFFER_FLAG_KEY_FRAME;
  }
  bufferInfo.offset = 0;
  bufferInfo.presentationTimeUs = buffer->timeUs();
  bufferInfo.size = (int32_t) buffer->size();
  auto result = AMediaMuxer_writeSampleData(mediaMuxer.get(),
    buffer->streamIndex(),
    buffer->array(),
    &bufferInfo);
  if (result) {
    MINILOG_ERROR("write sample data: {}", std::to_string(result));
  }
}

void NativeMuxer::start() {
  if (isStarted()) {
    return;
  }
  if (mediaMuxer) {
    AMediaMuxer_start(mediaMuxer.get());
  }
  mStatus = Status::kMuxerStatusStarted;
}

void NativeMuxer::stop() {
  if (!isStarted()) {
    return;
  }
  if (mediaMuxer) {
    AMediaMuxer_stop(mediaMuxer.get());
  }
  mStatus = Status::kMuxerStatusStopped;
}

void NativeMuxer::release() {
  if (isStarted()) {
    stop();
  }
  mediaMuxer.reset();
  mStatus = Status::kMuxerStatusIdle;
}

} // namespace coral
