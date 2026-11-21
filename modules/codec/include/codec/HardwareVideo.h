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

#ifndef CORAL_CODEC_HARDWARE_VIDEO_H
#define CORAL_CODEC_HARDWARE_VIDEO_H

#include <codec/MediaCodec.h>
#include <media/PixelBuffer.h>

#include <cstdint>

namespace coral {

/**
 * A decode target that is a GPU instead of a window.
 *
 * The Windows decoder takes a `VideoSurface` to keep the API it shares with Android, where the surface is
 * the `SurfaceView` the decoder renders into. A GPU is the same idea one level lower: the decoder is asked
 * to allocate its output textures on {@link getNativeDevice} and hand them back, so the frames never pass
 * through a CPU buffer.
 *
 * A decoder that cannot honour it -- an old driver, a codec without a hardware path -- falls back to CPU
 * frames, which is why using this surface is a request rather than a promise.
 */
class HardwareVideoSurface : public VideoSurface {
public:
  /**
   * The device the decoder creates its output textures on, as an opaque platform handle.
   *
   * Windows: an `ID3D11Device*`. The decoder opens a Media Foundation device manager on it and asks the
   * MFT to draw into textures from that device's pool.
   */
  [[nodiscard]] virtual void* getNativeDevice() const = 0;
};

/**
 * A decoded frame that lives in a platform texture.
 *
 * The texture belongs to the decoder's pool. The buffer holds a reference to it, so the frame stays valid
 * until the last reference is released and the pool can only reuse the surface afterwards; a consumer that
 * wants to keep the pixels must copy them or keep the buffer, which is exactly the trade a zero-copy path
 * makes.
 *
 * `getFormat()` reports the format the *texture* holds (NV12 for every hardware decoder), not RGBA: the
 * conversion is the consumer's job, and `GpuVideoHardwareFrameSource` is the one that does it for the
 * video pipeline.
 */
class HardwarePixelBuffer : public AVPixelBuffer {
public:
  /** How the platform texture stores the picture. */
  enum class TextureFormat : uint8_t {
    /** A single NV12 texture: plane 0 is luma, plane 1 the interleaved chroma. */
    kNv12,
    /** `R8G8B8A8_UNORM`. */
    kRgba,
  };

  /**
   * The platform texture, valid for as long as this buffer is.
   *
   * Windows: an `ID3D11Texture2D*`. It is borrowed, not owned: the buffer keeps the reference the decoder
   * pool gave it.
   */
  [[nodiscard]] virtual void* getNativeTexture() const = 0;

  /**
   * Which subresource of that texture holds this frame.
   *
   * A decoder allocates a texture *array* and writes one slice per frame, so the pair
   * (texture, subresource) is what names a frame; 0 is the common case for a plain texture.
   */
  [[nodiscard]] virtual uint32_t getSubresource() const = 0;

  [[nodiscard]] virtual TextureFormat getTextureFormat() const = 0;
};

} // namespace coral

#endif // CORAL_CODEC_HARDWARE_VIDEO_H
