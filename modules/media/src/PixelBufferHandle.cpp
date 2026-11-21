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

#include <media/PixelBufferHandle.h>

#include <media/PixelBuffer.h>

#include <algorithm>
#include <utility>

namespace coral {

namespace {

int rotationDegrees(const AVPixelBuffer::Rotation rotation) {
  switch (rotation) {
    case AVPixelBuffer::Rotation::kRotation_90: return 90;
    case AVPixelBuffer::Rotation::kRotation_180: return 180;
    case AVPixelBuffer::Rotation::kRotation_270: return 270;
    case AVPixelBuffer::Rotation::kRotation_0:
    default: return 0;
  }
}

/** Adapts the existing CPU AVPixelBuffer subclasses to the new handle contract. */
class LegacyCpuPixelBufferStorage final : public PixelBufferStorage {
public:
  explicit LegacyCpuPixelBufferStorage(std::shared_ptr<AVPixelBuffer> buffer)
    : mBuffer(std::move(buffer)),
      mInfo(makeInfo(mBuffer)) {
  }

  [[nodiscard]] const PixelBufferInfo& info() const override {
    return mInfo;
  }

  bool lock(const PixelBufferLockMode mode, PixelBufferPlane* planes,
      uint32_t* planeCount) override {
    if (mBuffer == nullptr || planes == nullptr || planeCount == nullptr) {
      return false;
    }
    const auto writable = mode == PixelBufferLockMode::Write ||
                          mode == PixelBufferLockMode::ReadWrite;
    if (const auto i420 = mBuffer->asI420PixelBuffer(); i420 != nullptr) {
      planes[0] = { writable ? i420->mutableDataY() : const_cast<uint8_t*>(i420->dataY()),
        i420->strideY(), i420->width(), i420->height() };
      planes[1] = { writable ? i420->mutableDataU() : const_cast<uint8_t*>(i420->dataU()),
        i420->strideU(), (i420->width() + 1) / 2, (i420->height() + 1) / 2 };
      planes[2] = { writable ? i420->mutableDataV() : const_cast<uint8_t*>(i420->dataV()),
        i420->strideV(), (i420->width() + 1) / 2, (i420->height() + 1) / 2 };
      *planeCount = 3;
      return true;
    }
    if (const auto nv12 = mBuffer->asNV12PixelBuffer(); nv12 != nullptr) {
      planes[0] = { writable ? nv12->mutableDataY() : const_cast<uint8_t*>(nv12->dataY()),
        nv12->strideY(), nv12->width(), nv12->height() };
      planes[1] = { writable ? nv12->mutableDataUV() : const_cast<uint8_t*>(nv12->dataUV()),
        nv12->strideUV(), (nv12->width() + 1) / 2, (nv12->height() + 1) / 2 };
      *planeCount = 2;
      return true;
    }
    return false;
  }

  void unlock() override {
  }

  void release() override {
    if (mBuffer != nullptr) {
      mBuffer->release();
      mBuffer.reset();
    }
  }

private:
  static PixelBufferInfo makeInfo(const std::shared_ptr<AVPixelBuffer>& buffer) {
    PixelBufferInfo info;
    if (buffer == nullptr) {
      return info;
    }
    info.width = buffer->width();
    info.height = buffer->height();
    info.format = buffer->getFormat();
    info.storage = PixelBufferStorageType::Cpu;
    info.capabilities = PixelBufferStorageCapability::CpuRead |
                        PixelBufferStorageCapability::CpuWrite;
    info.color = buffer->getColorInfo();
    info.rotationDegrees = rotationDegrees(buffer->getRotation());
    info.timeUs = buffer->timeUs();
    return info;
  }

  std::shared_ptr<AVPixelBuffer> mBuffer;
  PixelBufferInfo mInfo;
};

} // namespace anonymous

std::shared_ptr<PixelBuffer> PixelBuffer::create(std::shared_ptr<PixelBufferStorage> storage) {
  if (storage == nullptr) {
    return nullptr;
  }
  return std::shared_ptr<PixelBuffer>(new PixelBuffer(std::move(storage)));
}

std::shared_ptr<PixelBuffer>
PixelBuffer::fromAVPixelBuffer(const std::shared_ptr<AVPixelBuffer>& buffer) {
  if (buffer == nullptr) {
    return nullptr;
  }
  return create(std::make_shared<LegacyCpuPixelBufferStorage>(buffer));
}

PixelBuffer::PixelBuffer(std::shared_ptr<PixelBufferStorage> storage)
  : mStorage(std::move(storage)) {
}

PixelBuffer::~PixelBuffer() = default;

const PixelBufferInfo& PixelBuffer::info() const {
  return mStorage->info();
}

const std::shared_ptr<PixelBufferStorage>& PixelBuffer::storage() const {
  return mStorage;
}

int PixelBuffer::width() const {
  return mStorage->info().width;
}

int PixelBuffer::height() const {
  return mStorage->info().height;
}

PixelFormat PixelBuffer::format() const {
  return mStorage->info().format;
}

int64_t PixelBuffer::timeUs() const {
  return mStorage->info().timeUs;
}

bool PixelBuffer::lock(const PixelBufferLockMode mode, PixelBufferPlane* planes,
    uint32_t* planeCount) {
  return mStorage->lock(mode, planes, planeCount);
}

void PixelBuffer::unlock() {
  mStorage->unlock();
}

bool PixelBuffer::nativeHandle(const PixelBufferNativeHandleKind kind, void** handle,
    uint32_t* subresource) const {
  return mStorage->nativeHandle(kind, handle, subresource);
}

void PixelBuffer::release() {
  mStorage->release();
}

bool PixelBuffer::acquireKeyedMutex(const uint64_t key, const uint32_t timeoutMilliseconds) {
  return mStorage->acquireKeyedMutex(key, timeoutMilliseconds);
}

void PixelBuffer::releaseKeyedMutex(const uint64_t key) {
  mStorage->releaseKeyedMutex(key);
}

} // namespace coral
