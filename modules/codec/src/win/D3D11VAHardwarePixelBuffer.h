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

#ifndef CORAL_CODEC_D3D11VA_HARDWARE_PIXEL_BUFFER_H
#define CORAL_CODEC_D3D11VA_HARDWARE_PIXEL_BUFFER_H

#if defined(WINDOWS) || defined(WIN32)

#include <codec/HardwareVideo.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <memory>

namespace coral {

/**
 * A decoded frame of the D3D11VA decoder: one subresource of the decode texture array.
 *
 * The buffer owns a reference to the texture, so a frame a renderer keeps stays valid even after the
 * decoder is released. It does not own the *slice*: the slice returns to the decoder's pool when the
 * consumer releases the buffer, and the decoder never writes a later picture into a slice which a
 * consumer still holds. That is the whole of the zero-copy contract of this decoder.
 *
 * A frame converted to a packed layout by {@link processFormat} is the same class over a texture of
 * its own; `isConverted()` tells the owner which pool the buffer belongs to.
 */
class D3D11VAHardwarePixelBuffer final : public HardwarePixelBuffer,
                                         public std::enable_shared_from_this<D3D11VAHardwarePixelBuffer> {
  public:
  D3D11VAHardwarePixelBuffer(Owner<AVPixelBuffer>* owner, Microsoft::WRL::ComPtr<ID3D11Texture2D> texture,
          uint32_t subresource, int width, int height, TextureFormat textureFormat, PixelFormat pixelFormat,
          int slotIndex, bool converted);

  ~D3D11VAHardwarePixelBuffer() override = default;

  [[nodiscard]] PixelFormat getFormat() const override { return mPixelFormat; }

  [[nodiscard]] int width() const override { return mWidth; }

  [[nodiscard]] int height() const override { return mHeight; }

  [[nodiscard]] void* getNativeTexture() const override;

  [[nodiscard]] uint32_t getSubresource() const override { return mSubresource; }

  [[nodiscard]] TextureFormat getTextureFormat() const override { return mTextureFormat; }

  void release() override;

  std::shared_ptr<AVPixelBuffer> asPixelBuffer() override;

  std::shared_ptr<PixelBuffer> asPixelBufferHandle() override;

  /** The pool slot the frame was decoded into, or the conversion ring slot it was written into. */
  [[nodiscard]] int slotIndex() const { return mSlotIndex; }

  /** Whether this frame is a packed conversion of a decoded picture rather than a decoded slice. */
  [[nodiscard]] bool isConverted() const { return mConverted; }

  /** The texture as the D3D11 texture array slice owner sees it. */
  [[nodiscard]] ID3D11Texture2D* texture() const { return mTexture.Get(); }

  /** Marks the buffer as returned; only the first call reaches the owner. */
  [[nodiscard]] bool markReleased() { return !mReleased.exchange(true); }

  private:
  Owner<AVPixelBuffer>* mOwner = nullptr;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> mTexture;
  uint32_t mSubresource = 0;
  int mWidth = 0;
  int mHeight = 0;
  TextureFormat mTextureFormat = TextureFormat::kNv12;
  PixelFormat mPixelFormat = PixelFormat::kFormatNV12;
  int mSlotIndex = 0;
  bool mConverted = false;
  std::atomic_bool mReleased{ false };
};

} // namespace coral

#endif // defined(WINDOWS) || defined(WIN32)

#endif // CORAL_CODEC_D3D11VA_HARDWARE_PIXEL_BUFFER_H
