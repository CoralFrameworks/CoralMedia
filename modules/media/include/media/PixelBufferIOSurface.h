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

#ifndef CORAL_MEDIA_PIXEL_BUFFER_IOSURFACE_H
#define CORAL_MEDIA_PIXEL_BUFFER_IOSURFACE_H

#include <media/PixelBufferHandle.h>

#include <functional>
#include <memory>

#if defined(__APPLE__)

#include <CoreVideo/CoreVideo.h>

namespace coral {

/**
 * Wraps a `CVPixelBuffer` as a PixelBuffer without copying it. The handle takes its own reference to
 * the pixel buffer, so the producer may release its reference as soon as the handle owns it.
 *
 * A `CVPixelBuffer` that a hardware decoder produced (or that was created with
 * `kCVPixelBufferIOSurfacePropertiesKey`) is backed by an `IOSurface`; the handle exposes it through
 * `nativeHandle(PixelBufferNativeHandleKind::IOSurface)`, which is what a Metal or OpenGL consumer
 * imports. The handle also locks the planes for a CPU consumer, because an IOSurface is mappable.
 *
 * @param pixelBuffer The pixel buffer to wrap. Its format must be one of the bi-planar 4:2:0 formats
 *                    (NV12) or a packed 32 bit format (BGRA), which `coral::PixelBuffer::format()`
 *                    reports as `kFormatNV12` / `kFormatBGRA`.
 * @param release     Called when the handle is released or destroyed, which is how a decoder buffer
 *                    that backs the pixel buffer returns to its producer.
 */
std::shared_ptr<PixelBuffer> createIOSurfacePixelBuffer(CVPixelBufferRef pixelBuffer,
  int width,
  int height,
  PixelFormat format,
  const ColorInfo& color = {},
  int64_t timeUs = 0,
  std::function<void()> release = nullptr);

} // namespace coral

#endif // defined(__APPLE__)

#endif // CORAL_MEDIA_PIXEL_BUFFER_IOSURFACE_H
