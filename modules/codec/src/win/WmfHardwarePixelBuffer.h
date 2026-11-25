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
#include <mfobjects.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>

namespace coral {

/**
 * The Windows half of {@link HardwarePixelBuffer}: a `ID3D11Texture2D` slice the decoder filled in.
 *
 * The buffer keeps the `IMFSample` it came from alive, not only the texture. A decoder that draws into a
 * DXGI texture array hands out one *slice* per sample, and the sample is the pool's ticket for that
 * slice: dropping it lets Media Foundation refill the same slice with a later picture while the consumer
 * still holds this frame. Holding the texture alone is not enough for the same reason - the texture is
 * the whole array and stays valid. The buffer drops the sample when the consumer releases it, which is
 * what returns the slice to the decoder pool.
 */
class WmfHardwarePixelBuffer final : public HardwarePixelBuffer,
                                     public std::enable_shared_from_this<WmfHardwarePixelBuffer> {
public:
  WmfHardwarePixelBuffer(Owner<AVPixelBuffer>* owner, Microsoft::WRL::ComPtr<IMFSample> sample,
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture, uint32_t subresource, int width, int height,
    TextureFormat format);

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
  /** Pins the decoder pool's slice: the sample stays alive until the consumer releases this buffer. */
  Microsoft::WRL::ComPtr<IMFSample> mSample;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> mTexture;
  uint32_t mSubresource = 0;
  int mWidth = 0;
  int mHeight = 0;
  TextureFormat mFormat = TextureFormat::kNv12;
};

} // namespace coral

#endif // CORAL_CODEC_WMF_HARDWARE_PIXEL_BUFFER_H
