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

#ifndef CORAL_CODEC_VIDEO_FRAME_METADATA_LISTENER_H
#define CORAL_CODEC_VIDEO_FRAME_METADATA_LISTENER_H

#include <media/Format.h>

#include <memory>

namespace coral {

/** Receives metadata for a video frame about to be rendered. */
class VideoFrameMetadataListener {
public:
  virtual ~VideoFrameMetadataListener() noexcept = default;

  virtual void onVideoFrameAboutToBeRendered(int64_t presentationTimeUs, int64_t releaseTimeNs,
                                             const std::shared_ptr<MediaFormat>& format) = 0;
};

} // namespace coral

#endif // CORAL_CODEC_VIDEO_FRAME_METADATA_LISTENER_H
