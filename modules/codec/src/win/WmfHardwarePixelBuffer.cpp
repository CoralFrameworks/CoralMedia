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

#include "WmfHardwarePixelBuffer.h"

#include <media/PixelBufferD3D11.h>

#include <utility>

namespace coral {

WmfHardwarePixelBuffer::WmfHardwarePixelBuffer(Owner<AVPixelBuffer>* owner,
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture, const uint32_t subresource, const int width,
    const int height, const TextureFormat format)
  : mOwner(owner),
    mTexture(std::move(texture)),
    mSubresource(subresource),
    mWidth(width),
    mHeight(height),
    mFormat(format) {
}

PixelFormat WmfHardwarePixelBuffer::getFormat() const {
  return mFormat == TextureFormat::kNv12 ? PixelFormat::kFormatNV12 : PixelFormat::kFormatRGBA;
}

void* WmfHardwarePixelBuffer::getNativeTexture() const {
  // const_cast would hide the intent: the consumer gets the pointer so it can view the texture, not write
  // through it, and the RHI takes it as `void*` for the same reason.
  return const_cast<ID3D11Texture2D*>(mTexture.Get());
}

void WmfHardwarePixelBuffer::release() {
  if (mOwner != nullptr) {
    mOwner->recycleBuffer(shared_from_this());
  }
}

std::shared_ptr<AVPixelBuffer> WmfHardwarePixelBuffer::asPixelBuffer() {
  return shared_from_this();
}

std::shared_ptr<PixelBuffer> WmfHardwarePixelBuffer::asPixelBufferHandle() {
  auto self = shared_from_this();
  return createD3D11PixelBuffer(mTexture.Get(), mWidth, mHeight, getFormat(), getColorInfo(),
    timeUs(), [self] { self->release(); });
}

} // namespace coral
