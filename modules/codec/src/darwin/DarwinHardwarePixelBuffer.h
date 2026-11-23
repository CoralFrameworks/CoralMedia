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

#ifndef CORAL_CODEC_DARWIN_HARDWARE_PIXEL_BUFFER_H
#define CORAL_CODEC_DARWIN_HARDWARE_PIXEL_BUFFER_H

#if defined(__APPLE__)

#include <codec/HardwareVideo.h>

#include <CoreVideo/CoreVideo.h>

#include <cstdint>
#include <memory>

namespace coral {

/**
 * The Apple half of {@link HardwarePixelBuffer}: a `CVPixelBuffer` the decoder filled in.
 *
 * The buffer keeps its own reference to the pixel buffer, so the decoder's allocator may reuse its
 * slot as soon as the frame is returned, not when the decoder discards it. A hardware decoder emits
 * bi-planar 4:2:0 pixel buffers (`kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange`) which are backed
 * by an `IOSurface`, so `asPixelBufferHandle()` hands the consumer the surface without a copy.
 *
 * The pixel buffer is the platform texture: `getNativeTexture()` returns it as a `CVPixelBufferRef`,
 * which is what a Metal consumer passes to `CVMetalTextureCache` (and an OpenGL consumer to
 * `CVOpenGLTextureCache`), so the decoded frame never reaches the CPU.
 */
class DarwinHardwarePixelBuffer final : public HardwarePixelBuffer,
                                        public std::enable_shared_from_this<DarwinHardwarePixelBuffer> {
public:
  DarwinHardwarePixelBuffer(Owner<AVPixelBuffer>* owner, CVPixelBufferRef pixelBuffer, int width,
    int height, TextureFormat format);

  ~DarwinHardwarePixelBuffer() override;

  [[nodiscard]] PixelFormat getFormat() const override;

  [[nodiscard]] int width() const override {
    return mWidth;
  }

  [[nodiscard]] int height() const override {
    return mHeight;
  }

  [[nodiscard]] void* getNativeTexture() const override;

  [[nodiscard]] uint32_t getSubresource() const override {
    return 0;
  }

  [[nodiscard]] TextureFormat getTextureFormat() const override {
    return mFormat;
  }

  /** The pixel buffer the decoder wrote, borrowed for as long as this buffer is alive. */
  [[nodiscard]] CVPixelBufferRef getPixelBuffer() const {
    return mPixelBuffer;
  }

  void release() override;

  std::shared_ptr<AVPixelBuffer> asPixelBuffer() override;

  /** Returns the same pixel buffer as a backend-neutral IOSurface handle. */
  std::shared_ptr<PixelBuffer> asPixelBufferHandle() override;

private:
  Owner<AVPixelBuffer>* mOwner = nullptr;
  CVPixelBufferRef mPixelBuffer = nullptr;
  int mWidth = 0;
  int mHeight = 0;
  TextureFormat mFormat = TextureFormat::kNv12;
};

} // namespace coral

#endif // defined(__APPLE__)

#endif // CORAL_CODEC_DARWIN_HARDWARE_PIXEL_BUFFER_H
