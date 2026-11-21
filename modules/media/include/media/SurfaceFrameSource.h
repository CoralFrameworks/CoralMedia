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

#ifndef CORAL_MEDIA_SURFACE_FRAME_SOURCE_H
#define CORAL_MEDIA_SURFACE_FRAME_SOURCE_H

#include <cstdint>
#include <memory>
#include <string>

namespace coral {

class AVPixelBuffer;

/**
 * The consumer of the surface a decoder renders into.
 *
 * A decoder that draws into a surface hands out frames that name a frame rather than holding its pixels:
 * the pixels live in the surface's own queue until someone takes them. Android's `SurfaceTexture` is exactly
 * that, and it is a one-producer, one-consumer object -- the queue belongs to one decoder and one consumer --
 * which is why the consumer is attached to the surface rather than looked up per device.
 *
 * The buffer a decoder returns for such a frame carries this interface, so the stage that converts the frame
 * asks the right consumer instead of guessing.
 */
class SurfaceFrameSource {
public:
  virtual ~SurfaceFrameSource() = default;

  /**
   * Takes the next queued frame.
   *
   * @param width          the frame's width.
   * @param height         the frame's height.
   * @param expectedTimeUs the presentation time the caller wants, or 0 for whatever is next.
   * @param error          set when a frame was expected and could not be produced.
   * @return the frame, or null when none was queued, which is the normal answer when the decoder is behind.
   */
  [[nodiscard]] virtual std::shared_ptr<AVPixelBuffer> acquireSurfaceFrame(int width, int height,
    int64_t expectedTimeUs, std::string* error) = 0;
};

} // namespace coral

#endif // CORAL_MEDIA_SURFACE_FRAME_SOURCE_H
