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

#if defined(_WIN32)

#include <media/PixelBufferD3D11.h>

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <functional>
#include <utility>

namespace coral {

namespace {

class D3D11PixelBufferStorage final : public PixelBufferStorage {
public:
  D3D11PixelBufferStorage(ID3D11Texture2D* texture, const int width, const int height,
      const PixelFormat format, const ColorInfo& color, const int64_t timeUs,
      std::function<void()> release)
    : mTexture(texture), mRelease(std::move(release)) {
    mInfo.width = width;
    mInfo.height = height;
    mInfo.format = format;
    mInfo.storage = PixelBufferStorageType::D3D11Texture;
    mInfo.capabilities = PixelBufferStorageCapability::TextureSample |
                         PixelBufferStorageCapability::Transfer;
    if (mTexture != nullptr && SUCCEEDED(mTexture.As(&mKeyedMutex))) {
      mInfo.capabilities = mInfo.capabilities | PixelBufferStorageCapability::KeyedMutex;
    }
    mInfo.color = color;
    mInfo.timeUs = timeUs;
  }

  [[nodiscard]] const PixelBufferInfo& info() const override {
    return mInfo;
  }

  bool lock(PixelBufferLockMode, PixelBufferPlane*, uint32_t*) override {
    return false;
  }

  void unlock() override {
  }

  bool nativeHandle(const PixelBufferNativeHandleKind kind, void** handle,
      uint32_t* subresource) const override {
    if (kind != PixelBufferNativeHandleKind::D3D11Texture2D || handle == nullptr ||
        mTexture == nullptr) {
      return false;
    }
    *handle = mTexture.Get();
    if (subresource != nullptr) {
      *subresource = 0;
    }
    return true;
  }

  bool acquireKeyedMutex(const uint64_t key, const uint32_t timeoutMilliseconds) override {
    if (mKeyedMutex == nullptr) {
      return false;
    }
    const auto timeout = timeoutMilliseconds == UINT32_MAX ? INFINITE : timeoutMilliseconds;
    return SUCCEEDED(mKeyedMutex->AcquireSync(key, timeout));
  }

  void releaseKeyedMutex(const uint64_t key) override {
    if (mKeyedMutex != nullptr) {
      static_cast<void>(mKeyedMutex->ReleaseSync(key));
    }
  }

  void release() override {
    auto release = std::move(mRelease);
    if (release) {
      release();
    }
  }

  ~D3D11PixelBufferStorage() override {
    release();
  }

private:
  Microsoft::WRL::ComPtr<ID3D11Texture2D> mTexture;
  Microsoft::WRL::ComPtr<IDXGIKeyedMutex> mKeyedMutex;
  std::function<void()> mRelease;
  PixelBufferInfo mInfo;
};

} // namespace anonymous

std::shared_ptr<PixelBuffer> createD3D11PixelBuffer(ID3D11Texture2D* texture, const int width,
    const int height, const PixelFormat format, const ColorInfo& color, const int64_t timeUs,
    std::function<void()> release) {
  if (texture == nullptr || width <= 0 || height <= 0) {
    return nullptr;
  }
  return PixelBuffer::create(std::make_shared<D3D11PixelBufferStorage>(
    texture, width, height, format, color, timeUs, std::move(release)));
}

} // namespace coral

#endif // defined(_WIN32)
