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

#ifndef CORAL_CODEC_FFMPEG_MUXER_H
#define CORAL_CODEC_FFMPEG_MUXER_H

#include <codec/MediaMuxer.h>

#include "private/ffcodec/FfmpegUtils.h"

#include <memory>

namespace coral {

/**
 * The muxer implementation based on Ffmpeg
 */
class FfmpegMuxer final : public AVMediaMuxer {
public:
  FfmpegMuxer();

  ~FfmpegMuxer() override;

  bool prepare(std::string url, MuxerFormat format) override;

  int addTrack(const std::shared_ptr<MediaFormat>& format) override;

  void writeFrame(const std::shared_ptr<AVPacketBuffer>& buffer) override;

  void start() override;

  void stop() override;

  void release() override;

  const char* getName() const override;

protected:
  int openMuxer();

  int addAudioTrack(AVCodecID codecId, int sampleRate, int channelCount, int bitRate);

  int addVideoTrack(AVCodecID codecId, int width, int height, int frameRate, int bitRate);

  AVStream* createStream(AVCodecID id);

  int writeHeader();

  bool writeExtraData(int trackIndex, uint8_t* extraData, size_t size);

  int writeTrailer();

  void closeMuxer();

  void setMetadata(const char* key, const char* val);

private:
  static const char* getMuxerFormatName(MuxerFormat format);

  std::string mOutputUrl;
  std::unique_ptr<AVFormatContext, AVFormatContextDeleter> pFormatContext;
  std::unique_ptr<AVPacket, AVPacketDeleter> mCachePacket;
};

class FfmpegMuxerFactory final : public MediaMuxerFactory {
public:
  FfmpegMuxerFactory();

  ~FfmpegMuxerFactory() override;

  bool isSupportMuxerFormat(MuxerFormat muxerFormat) override;

  std::unique_ptr<AVMediaMuxer> createMuxer() override;
};

} // namespace coral

#endif // CORAL_CODEC_FFMPEG_MUXER_H
