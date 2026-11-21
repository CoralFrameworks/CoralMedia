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

#ifndef CORAL_CODEC_VIDEO_FRAME_RENDERER_H
#define CORAL_CODEC_VIDEO_FRAME_RENDERER_H

#include <memory>

namespace coral {

class AVPixelBuffer;

/**
 * A sink for decoded video frames.
 *
 * <p>This interface lives in `codec` so the playback library can accept a custom renderer without
 * linking the legacy `video` target. The old built-in renderers remain in `video` for the
 * DirectComposition demo until that path is removed.
 */
class VideoFrameRenderer {
public:
  virtual ~VideoFrameRenderer() noexcept = default;

  /**
   * Sets the output buffer to be rendered. The renderer owns the buffer and must release it.
   */
  virtual void setOutputBuffer(std::shared_ptr<AVPixelBuffer> frameBuffer) = 0;
};

} // namespace coral

#endif // CORAL_CODEC_VIDEO_FRAME_RENDERER_H
