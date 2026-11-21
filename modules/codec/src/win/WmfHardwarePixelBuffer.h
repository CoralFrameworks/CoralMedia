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

#ifndef CORAL_CODEC_WMF_HARDWARE_PIXEL_BUFFER_H
#define CORAL_CODEC_WMF_HARDWARE_PIXEL_BUFFER_H

#include <codec/HardwareVideo.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>

namespace coral {

/**
 * The Windows half of {@link HardwarePixelBuffer}: a `ID3D11Texture2D` slice the decoder filled in.
 *
 * The buffer holds a reference to the texture rather than to the `IMFSample` it came from. That is the
 * point of a decoded texture: the sample is the decoder allocator's bookkeeping, while the texture is the
 * surface, and keeping the surface alive is what keeps the pool from handing the same slice to the next
 * frame while it is still being sampled.
 */
class WmfHardwarePixelBuffer final : public HardwarePixelBuffer,
                                     public std::enable_shared_from_this<WmfHardwarePixelBuffer> {
public:
  WmfHardwarePixelBuffer(Owner<AVPixelBuffer>* owner, Microsoft::WRL::ComPtr<ID3D11Texture2D> texture,
    uint32_t subresource, int width, int height, TextureFormat format);

  ~WmfHardwarePixelBuffer() override = default;

  [[nodiscard]] PixelFormat getFormat() const override;

  [[nodiscard]] int width() const override {
    return mWidth;
  }

  [[nodiscard]] int height() const override {
    return mHeight;
  }

  [[nodiscard]] void* getNativeTexture() const override;

  [[nodiscard]] uint32_t getSubresource() const override {
    return mSubresource;
  }

  [[nodiscard]] TextureFormat getTextureFormat() const override {
    return mFormat;
  }

  void release() override;

  std::shared_ptr<AVPixelBuffer> asPixelBuffer() override;

  /** Returns the same D3D11 texture as a backend-neutral PixelBuffer handle. */
  std::shared_ptr<PixelBuffer> asPixelBufferHandle() override;

private:
  Owner<AVPixelBuffer>* mOwner = nullptr;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> mTexture;
  uint32_t mSubresource = 0;
  int mWidth = 0;
  int mHeight = 0;
  TextureFormat mFormat = TextureFormat::kNv12;
};

} // namespace coral

#endif // CORAL_CODEC_WMF_HARDWARE_PIXEL_BUFFER_H
