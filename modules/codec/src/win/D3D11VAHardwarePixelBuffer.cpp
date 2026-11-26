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

#include "D3D11VAHardwarePixelBuffer.h"

#if defined(WINDOWS) || defined(WIN32)

#include <media/PixelBufferD3D11.h>

#include <utility>

namespace coral {

D3D11VAHardwarePixelBuffer::D3D11VAHardwarePixelBuffer(Owner<AVPixelBuffer>* owner,
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture, const uint32_t subresource, const int width, const int height,
        const TextureFormat textureFormat, const PixelFormat pixelFormat, const int slotIndex, const bool converted)
        : mOwner(owner),
          mTexture(std::move(texture)),
          mSubresource(subresource),
          mWidth(width),
          mHeight(height),
          mTextureFormat(textureFormat),
          mPixelFormat(pixelFormat),
          mSlotIndex(slotIndex),
          mConverted(converted) {}

void* D3D11VAHardwarePixelBuffer::getNativeTexture() const {
  // The consumer views the texture, it does not own it; the interface hands platform handles out as
  // `void*` for the same reason.
  return const_cast<ID3D11Texture2D*>(mTexture.Get());
}

void D3D11VAHardwarePixelBuffer::release() {
  if (!markReleased()) {
    return;
  }
  if (mOwner != nullptr) {
    mOwner->recycleBuffer(shared_from_this());
  }
}

std::shared_ptr<AVPixelBuffer> D3D11VAHardwarePixelBuffer::asPixelBuffer() { return shared_from_this(); }

std::shared_ptr<PixelBuffer> D3D11VAHardwarePixelBuffer::asPixelBufferHandle() {
  auto self = shared_from_this();
  return createD3D11PixelBuffer(mTexture.Get(), mWidth, mHeight, mPixelFormat, getColorInfo(), timeUs(),
          [self] { self->release(); });
}

} // namespace coral

#endif // defined(WINDOWS) || defined(WIN32)
