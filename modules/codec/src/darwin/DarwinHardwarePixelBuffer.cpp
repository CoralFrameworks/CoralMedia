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

#if defined(__APPLE__)

#include "DarwinHardwarePixelBuffer.h"

#include <media/PixelBufferIOSurface.h>

#include <CoreFoundation/CoreFoundation.h>

#include <utility>

namespace coral {

DarwinHardwarePixelBuffer::DarwinHardwarePixelBuffer(Owner<AVPixelBuffer>* owner,
    CVPixelBufferRef pixelBuffer, const int width, const int height, const TextureFormat format)
  : mOwner(owner),
    mPixelBuffer(pixelBuffer),
    mWidth(width),
    mHeight(height),
    mFormat(format) {
  if (mPixelBuffer != nullptr) {
    CFRetain(mPixelBuffer);
  }
}

DarwinHardwarePixelBuffer::~DarwinHardwarePixelBuffer() {
  if (mPixelBuffer != nullptr) {
    CFRelease(mPixelBuffer);
    mPixelBuffer = nullptr;
  }
}

PixelFormat DarwinHardwarePixelBuffer::getFormat() const {
  switch (mFormat) {
    case TextureFormat::kNv12:
      return PixelFormat::kFormatNV12;
    case TextureFormat::kRgba:
      return PixelFormat::kFormatBGRA;
    case TextureFormat::kP010:
      return PixelFormat::kFormatP010;
    default:
      return PixelFormat::kFormatBGRA;
  }
}

void* DarwinHardwarePixelBuffer::getNativeTexture() const {
  // The consumer views the pixel buffer, it does not own it; the interface hands platform handles
  // out as `void*` for the same reason.
  return mPixelBuffer;
}

void DarwinHardwarePixelBuffer::release() {
  if (mOwner != nullptr) {
    mOwner->recycleBuffer(shared_from_this());
  }
}

std::shared_ptr<AVPixelBuffer> DarwinHardwarePixelBuffer::asPixelBuffer() {
  return shared_from_this();
}

std::shared_ptr<PixelBuffer> DarwinHardwarePixelBuffer::asPixelBufferHandle() {
  auto self = shared_from_this();
  return createIOSurfacePixelBuffer(mPixelBuffer, mWidth, mHeight, getFormat(), getColorInfo(),
    timeUs(), [self] { self->release(); });
}

} // namespace coral

#endif // defined(__APPLE__)
