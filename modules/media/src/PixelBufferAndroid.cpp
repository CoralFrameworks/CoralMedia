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

#if defined(__ANDROID__)

#include <media/PixelBufferAndroid.h>

#include <android/hardware_buffer.h>
#include <dlfcn.h>

#include <utility>

namespace coral {

namespace {

using AcquireFn = void (*)(AHardwareBuffer*);
using ReleaseFn = void (*)(AHardwareBuffer*);
using DescribeFn = void (*)(const AHardwareBuffer*, AHardwareBuffer_Desc*);
using LockFn = int (*)(AHardwareBuffer*, uint64_t, int32_t, const ARect*, void**);
using LockPlanesFn = int (*)(AHardwareBuffer*, uint64_t, int32_t, const ARect*,
  AHardwareBuffer_Planes*);
using UnlockFn = int (*)(AHardwareBuffer*, int32_t*);

struct HardwareBufferApi {
  AcquireFn acquire = reinterpret_cast<AcquireFn>(dlsym(RTLD_DEFAULT, "AHardwareBuffer_acquire"));
  ReleaseFn release = reinterpret_cast<ReleaseFn>(dlsym(RTLD_DEFAULT, "AHardwareBuffer_release"));
  DescribeFn describe =
      reinterpret_cast<DescribeFn>(dlsym(RTLD_DEFAULT, "AHardwareBuffer_describe"));
  LockFn lock = reinterpret_cast<LockFn>(dlsym(RTLD_DEFAULT, "AHardwareBuffer_lock"));
  LockPlanesFn lockPlanes =
      reinterpret_cast<LockPlanesFn>(dlsym(RTLD_DEFAULT, "AHardwareBuffer_lockPlanes"));
  UnlockFn unlock = reinterpret_cast<UnlockFn>(dlsym(RTLD_DEFAULT, "AHardwareBuffer_unlock"));
};

const HardwareBufferApi& hardwareBufferApi() {
  static const HardwareBufferApi api;
  return api;
}

class AndroidHardwareBufferStorage final : public PixelBufferStorage {
public:
  AndroidHardwareBufferStorage(AHardwareBuffer* buffer, const int width, const int height,
      const PixelFormat format, const ColorInfo& color, const int64_t timeUs)
    : mBuffer(buffer) {
    const auto& api = hardwareBufferApi();
    if (mBuffer != nullptr && api.acquire != nullptr && api.describe != nullptr) {
      api.acquire(mBuffer);
      AHardwareBuffer_Desc desc{};
      api.describe(mBuffer, &desc);
      mUsage = desc.usage;
    } else {
      mBuffer = nullptr;
    }
    mInfo.width = width;
    mInfo.height = height;
    mInfo.format = format;
    mInfo.storage = PixelBufferStorageType::AndroidHardwareBuffer;
    mInfo.capabilities = PixelBufferStorageCapability::TextureSample |
                         PixelBufferStorageCapability::Transfer;
    if ((mUsage & AHARDWAREBUFFER_USAGE_CPU_READ_MASK) != 0) {
      mInfo.capabilities = mInfo.capabilities | PixelBufferStorageCapability::CpuRead;
    }
    if ((mUsage & AHARDWAREBUFFER_USAGE_CPU_WRITE_MASK) != 0) {
      mInfo.capabilities = mInfo.capabilities | PixelBufferStorageCapability::CpuWrite;
    }
    mInfo.color = color;
    mInfo.timeUs = timeUs;
  }

  ~AndroidHardwareBufferStorage() override {
    release();
  }

  [[nodiscard]] const PixelBufferInfo& info() const override {
    return mInfo;
  }

  bool lock(const PixelBufferLockMode mode, PixelBufferPlane* planes,
      uint32_t* planeCount) override {
    const auto& api = hardwareBufferApi();
    if (mBuffer == nullptr || api.lock == nullptr || planes == nullptr || planeCount == nullptr ||
        mLocked) {
      return false;
    }
    // Single-plane RGBA/BGRA maps through AHardwareBuffer_lock. YUV uses lockPlanes below.
    if (mInfo.format == PixelFormat::kFormatRGBA ||
        mInfo.format == PixelFormat::kFormatBGRA) {
      const uint64_t usage = mode == PixelBufferLockMode::Write
                               ? AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN
                               : AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN;
      void* data = nullptr;
      if (api.lock == nullptr || api.lock(mBuffer, usage, -1, nullptr, &data) != 0 ||
          data == nullptr) {
        return false;
      }
      planes[0] = { static_cast<uint8_t*>(data), mInfo.width * 4, mInfo.width, mInfo.height };
      *planeCount = 1;
      mLocked = true;
      return true;
    }

    if (api.lockPlanes == nullptr) {
      return false;
    }
    const uint64_t usage = mode == PixelBufferLockMode::Write
                             ? AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN
                             : AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN;
    AHardwareBuffer_Planes hardwarePlanes{};
    if (api.lockPlanes(mBuffer, usage, -1, nullptr, &hardwarePlanes) != 0) {
      return false;
    }
    const auto chromaWidth = (mInfo.width + 1) / 2;
    const auto chromaHeight = (mInfo.height + 1) / 2;
    if (mInfo.format == PixelFormat::kFormatI420 && hardwarePlanes.planeCount >= 3) {
      planes[0] = { static_cast<uint8_t*>(hardwarePlanes.planes[0].data),
        static_cast<int>(hardwarePlanes.planes[0].rowStride), mInfo.width, mInfo.height };
      planes[1] = { static_cast<uint8_t*>(hardwarePlanes.planes[1].data),
        static_cast<int>(hardwarePlanes.planes[1].rowStride), chromaWidth, chromaHeight };
      planes[2] = { static_cast<uint8_t*>(hardwarePlanes.planes[2].data),
        static_cast<int>(hardwarePlanes.planes[2].rowStride), chromaWidth, chromaHeight };
      *planeCount = 3;
      mLocked = true;
      return true;
    }
    if (mInfo.format == PixelFormat::kFormatNV12 && hardwarePlanes.planeCount == 2) {
      planes[0] = { static_cast<uint8_t*>(hardwarePlanes.planes[0].data),
        static_cast<int>(hardwarePlanes.planes[0].rowStride), mInfo.width, mInfo.height };
      planes[1] = { static_cast<uint8_t*>(hardwarePlanes.planes[1].data),
        static_cast<int>(hardwarePlanes.planes[1].rowStride), chromaWidth, chromaHeight };
      *planeCount = 2;
      mLocked = true;
      return true;
    }
    static_cast<void>(api.unlock(mBuffer, nullptr));
    return false;
  }

  void unlock() override {
    const auto& api = hardwareBufferApi();
    if (mBuffer != nullptr && mLocked && api.unlock != nullptr) {
      static_cast<void>(api.unlock(mBuffer, nullptr));
      mLocked = false;
    }
  }

  bool nativeHandle(const PixelBufferNativeHandleKind kind, void** handle,
      uint32_t* subresource) const override {
    if (kind != PixelBufferNativeHandleKind::AndroidHardwareBuffer || handle == nullptr ||
        mBuffer == nullptr) {
      return false;
    }
    *handle = mBuffer;
    if (subresource != nullptr) {
      *subresource = 0;
    }
    return true;
  }

  void release() override {
    const auto& api = hardwareBufferApi();
    if (mBuffer != nullptr && api.release != nullptr) {
      api.release(mBuffer);
      mBuffer = nullptr;
    }
  }

private:
  AHardwareBuffer* mBuffer = nullptr;
  uint64_t mUsage = 0;
  PixelBufferInfo mInfo;
  bool mLocked = false;
};

} // namespace anonymous

std::shared_ptr<PixelBuffer> createAndroidHardwareBufferPixelBuffer(AHardwareBuffer* buffer,
    const int width, const int height, const PixelFormat format, const ColorInfo& color,
    const int64_t timeUs) {
  if (buffer == nullptr || width <= 0 || height <= 0) {
    return nullptr;
  }
  return PixelBuffer::create(
    std::make_shared<AndroidHardwareBufferStorage>(buffer, width, height, format, color, timeUs));
}

} // namespace coral

#endif // defined(__ANDROID__)
