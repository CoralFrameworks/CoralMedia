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

#include <media/PixelBufferIOSurface.h>

#include <CoreFoundation/CoreFoundation.h>

#include <utility>

namespace coral {

namespace {

/**
 * The storage of a decoded pixel buffer.
 *
 * The pixel buffer is the producer's storage: the handle retains it, and the release callback tells
 * the producer that this handle is done with it. A decoder that pools its pixel buffers therefore
 * only has to hand the callback to this wrapper.
 */
class IOSurfacePixelBufferStorage final : public PixelBufferStorage {
public:
  IOSurfacePixelBufferStorage(CVPixelBufferRef pixelBuffer, const int width, const int height,
      const PixelFormat format, const ColorInfo& color, const int64_t timeUs,
      std::function<void()> release)
    : mPixelBuffer(pixelBuffer),
      mRelease(std::move(release)) {
    if (mPixelBuffer != nullptr) {
      CFRetain(mPixelBuffer);
    }
    mInfo.width = width;
    mInfo.height = height;
    mInfo.format = format;
    mInfo.storage = PixelBufferStorageType::IOSurface;
    // The IOSurface is created by the producer with Metal compatibility, so the texture can be
    // sampled without a transfer. Its memory is also mapped, so a CPU consumer can read it.
    mInfo.capabilities = PixelBufferStorageCapability::TextureSample |
                         PixelBufferStorageCapability::CpuRead;
    if (!isPlanar()) {
      mInfo.capabilities = mInfo.capabilities | PixelBufferStorageCapability::CpuWrite;
    }
    mInfo.color = color;
    mInfo.timeUs = timeUs;
  }

  ~IOSurfacePixelBufferStorage() override {
    releaseOwned();
    if (mPixelBuffer != nullptr) {
      CFRelease(mPixelBuffer);
      mPixelBuffer = nullptr;
    }
  }

  [[nodiscard]] const PixelBufferInfo& info() const override {
    return mInfo;
  }

  bool lock(const PixelBufferLockMode mode, PixelBufferPlane* planes,
      uint32_t* planeCount) override {
    if (mPixelBuffer == nullptr || planes == nullptr || planeCount == nullptr || mLocked) {
      return false;
    }
    const CVReturn result = CVPixelBufferLockBaseAddress(mPixelBuffer,
      mode == PixelBufferLockMode::Read ? kCVPixelBufferLock_ReadOnly : 0);
    if (result != kCVReturnSuccess) {
      return false;
    }
    mLocked = true;

    const auto planeExtent = [this](const size_t plane, const int fallbackWidth,
                                    const int fallbackHeight) {
      int width = fallbackWidth;
      int height = fallbackHeight;
      if (CVPixelBufferIsPlanar(mPixelBuffer)) {
        width = static_cast<int>(CVPixelBufferGetWidthOfPlane(mPixelBuffer, plane));
        height = static_cast<int>(CVPixelBufferGetHeightOfPlane(mPixelBuffer, plane));
      }
      return std::pair<uint8_t*, std::pair<int, int>>(
        static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(mPixelBuffer, plane)),
        { width, height });
    };

    if (isPlanar()) {
      const auto luma = planeExtent(0, mInfo.width, mInfo.height);
      const auto chroma = planeExtent(1, (mInfo.width + 1) / 2, (mInfo.height + 1) / 2);
      planes[0] = { luma.first, static_cast<int>(CVPixelBufferGetBytesPerRowOfPlane(mPixelBuffer, 0)),
        luma.second.first, luma.second.second };
      planes[1] = { chroma.first,
        static_cast<int>(CVPixelBufferGetBytesPerRowOfPlane(mPixelBuffer, 1)),
        chroma.second.first, chroma.second.second };
      *planeCount = 2;
      return true;
    }

    planes[0] = { static_cast<uint8_t*>(CVPixelBufferGetBaseAddress(mPixelBuffer)),
      static_cast<int>(CVPixelBufferGetBytesPerRow(mPixelBuffer)), mInfo.width, mInfo.height };
    *planeCount = 1;
    return true;
  }

  void unlock() override {
    if (mPixelBuffer != nullptr && mLocked) {
      CVPixelBufferUnlockBaseAddress(mPixelBuffer, 0);
      mLocked = false;
    }
  }

  bool nativeHandle(const PixelBufferNativeHandleKind kind, void** handle,
      uint32_t* subresource) const override {
    if (kind != PixelBufferNativeHandleKind::IOSurface || handle == nullptr ||
        mPixelBuffer == nullptr) {
      return false;
    }
    *handle = CVPixelBufferGetIOSurface(mPixelBuffer);
    if (subresource != nullptr) {
      *subresource = 0;
    }
    return *handle != nullptr;
  }

  void release() override {
    unlock();
    releaseOwned();
  }

private:
  [[nodiscard]] bool isPlanar() const {
    return mPixelBuffer != nullptr && CVPixelBufferIsPlanar(mPixelBuffer) &&
           CVPixelBufferGetPlaneCount(mPixelBuffer) >= 2;
  }

  void releaseOwned() {
    auto release = std::move(mRelease);
    mRelease = nullptr;
    if (release) {
      release();
    }
  }

  CVPixelBufferRef mPixelBuffer = nullptr;
  std::function<void()> mRelease;
  PixelBufferInfo mInfo;
  bool mLocked = false;
};

} // namespace anonymous

std::shared_ptr<PixelBuffer> createIOSurfacePixelBuffer(CVPixelBufferRef pixelBuffer,
    const int width, const int height, const PixelFormat format, const ColorInfo& color,
    const int64_t timeUs, std::function<void()> release) {
  if (pixelBuffer == nullptr || width <= 0 || height <= 0) {
    return nullptr;
  }
  if (format != PixelFormat::kFormatNV12 && format != PixelFormat::kFormatBGRA &&
      format != PixelFormat::kFormatRGBA) {
    return nullptr;
  }
  return PixelBuffer::create(std::make_shared<IOSurfacePixelBufferStorage>(pixelBuffer, width,
    height, format, color, timeUs, std::move(release)));
}

} // namespace coral

#endif // defined(__APPLE__)
