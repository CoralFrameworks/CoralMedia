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

#ifndef CORAL_CODEC_FFMPEG_METADATA_RETRIEVER_H
#define CORAL_CODEC_FFMPEG_METADATA_RETRIEVER_H

#include <codec/MediaMetadataRetriever.h>

#include "private/ffcodec/FfmpegUtils.h"

namespace coral {

/**
 * The implementation class of MediaMetadataRetriever based on Ffmpeg
 */
class FfmpegMetadataRetriever final : public MediaMetadataRetriever {
public:
  static std::unique_ptr<FfmpegMetadataRetriever> sniff(const std::string& url);

  explicit FfmpegMetadataRetriever(AVFormatContext* formatContext) noexcept;

  void cancelLoading() override;

  int getTrackCount() override;

  std::shared_ptr<MediaFormat> getTrackFormat(int trackIndex) override;

  static const char* getName() { return "FfmpegMetadataRetriever"; }

private:
  static std::string convertToMime(AVCodecID codecId);

  AVFormatContext* pFormatContext;
  bool needsFindStreamInfo;
};

} // namespace coral

#endif // CORAL_CODEC_FFMPEG_METADATA_RETRIEVER_H
