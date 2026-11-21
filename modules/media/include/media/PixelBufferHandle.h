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

#ifndef CORAL_MEDIA_PIXEL_BUFFER_HANDLE_H
#define CORAL_MEDIA_PIXEL_BUFFER_HANDLE_H

#include <media/ColorInfo.h>
#include <media/Types.h>

#include <cstdint>
#include <memory>

namespace coral {

class AVPixelBuffer;
class PixelBuffer;

/** Where the pixels of a PixelBuffer are stored. */
enum class PixelBufferStorageType : uint8_t {
  Cpu,
  D3D11Texture,
  MetalTexture,
  OpenGLTexture,
  VulkanImage,
  AndroidHardwareBuffer,
  IOSurface,
  OHNativeBuffer,
  LinuxDmaBuf,
  NativeSurface,
};

/** The native handle exposed by a PixelBufferStorage. Values match Effect's NativeHandleKind. */
enum class PixelBufferNativeHandleKind : uint8_t {
  None,
  D3D11Texture2D,
  MetalTexture,
  OpenGLTexture,
  VulkanImage,
  AndroidHardwareBuffer,
  IOSurface,
  OHNativeBuffer,
  LinuxDmaBuf,
  NativeSurface,
};

/** What a consumer may do with a PixelBuffer without a conversion. */
enum class PixelBufferStorageCapability : uint32_t {
  CpuRead = 1u << 0,
  CpuWrite = 1u << 1,
  TextureSample = 1u << 2,
  TextureRender = 1u << 3,
  Transfer = 1u << 4,
  KeyedMutex = 1u << 5,
};

constexpr PixelBufferStorageCapability operator|(
    const PixelBufferStorageCapability a, const PixelBufferStorageCapability b) {
  return static_cast<PixelBufferStorageCapability>(
    static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

constexpr bool hasPixelBufferCapability(
    const PixelBufferStorageCapability value, const PixelBufferStorageCapability wanted) {
  return (static_cast<uint32_t>(value) & static_cast<uint32_t>(wanted)) ==
         static_cast<uint32_t>(wanted);
}

/** Backend-neutral metadata of one PixelBuffer. */
struct PixelBufferInfo {
  int width = 0;
  int height = 0;
  PixelFormat format = PixelFormat::kFormatInvalid;
  PixelBufferStorageType storage = PixelBufferStorageType::Cpu;
  PixelBufferStorageCapability capabilities = PixelBufferStorageCapability::CpuRead;
  ColorInfo color;
  int rotationDegrees = 0;
  int64_t timeUs = 0;
  int64_t durationUs = 0;
};

/** A CPU mapping of one plane while a PixelBuffer is locked. */
struct PixelBufferPlane {
  uint8_t* data = nullptr;
  int rowBytes = 0;
  int width = 0;
  int height = 0;
};

enum class PixelBufferLockMode : uint8_t {
  Read,
  Write,
  ReadWrite,
};

/**
 * The storage of a PixelBuffer.
 *
 * A CPU storage locks its planes; a platform storage imports a native texture/buffer into the effect
 * backend and reports the operation through a SyncToken in a later phase. The handle never exposes a
 * platform type in this interface.
 */
class PixelBufferStorage {
public:
  virtual ~PixelBufferStorage() = default;

  [[nodiscard]] virtual const PixelBufferInfo& info() const = 0;

  /** Locks CPU-readable/writeable planes. A texture storage returns false. */
  virtual bool lock(PixelBufferLockMode mode, PixelBufferPlane* planes, uint32_t* planeCount) = 0;
  virtual void unlock() = 0;

  /**
   * Returns an opaque native handle. `kind` is a platform-defined value owned by the platform layer;
   * the generic media layer only forwards it.
   */
  virtual bool nativeHandle(PixelBufferNativeHandleKind kind, void** handle,
      uint32_t* subresource) const {
    static_cast<void>(kind);
    static_cast<void>(handle);
    static_cast<void>(subresource);
    return false;
  }

  /** Gives the storage back to its producer, if the producer has a recycle callback. */
  virtual void release() {
  }

  /** Acquires the backend's cross-API keyed mutex, if the storage has one. */
  virtual bool acquireKeyedMutex(uint64_t key, uint32_t timeoutMilliseconds) {
    static_cast<void>(key);
    static_cast<void>(timeoutMilliseconds);
    return false;
  }

  virtual void releaseKeyedMutex(uint64_t key) {
    static_cast<void>(key);
  }
};

/**
 * A reference-counted pixel buffer handle.
 *
 * This is the CVPixelBuffer-shaped object: callers pass the handle, not a concrete I420/NV12/texture
 * class. The storage behind it may be CPU memory, a D3D11/Metal/GL texture or a native surface buffer.
 */
class PixelBuffer final {
public:
  static std::shared_ptr<PixelBuffer> create(std::shared_ptr<PixelBufferStorage> storage);
  /** Wraps an existing AVPixelBuffer without copying its pixels. */
  static std::shared_ptr<PixelBuffer> fromAVPixelBuffer(const std::shared_ptr<AVPixelBuffer>& buffer);

  ~PixelBuffer();

  PixelBuffer(const PixelBuffer&) = delete;
  PixelBuffer& operator=(const PixelBuffer&) = delete;

  [[nodiscard]] const PixelBufferInfo& info() const;
  [[nodiscard]] const std::shared_ptr<PixelBufferStorage>& storage() const;

  [[nodiscard]] int width() const;
  [[nodiscard]] int height() const;
  [[nodiscard]] PixelFormat format() const;
  [[nodiscard]] int64_t timeUs() const;

  bool lock(PixelBufferLockMode mode, PixelBufferPlane* planes, uint32_t* planeCount);
  void unlock();
  bool nativeHandle(PixelBufferNativeHandleKind kind, void** handle,
      uint32_t* subresource) const;
  void release();
  bool acquireKeyedMutex(uint64_t key, uint32_t timeoutMilliseconds);
  void releaseKeyedMutex(uint64_t key);

private:
  explicit PixelBuffer(std::shared_ptr<PixelBufferStorage> storage);

  std::shared_ptr<PixelBufferStorage> mStorage;
};

} // namespace coral

#endif // CORAL_MEDIA_PIXEL_BUFFER_HANDLE_H
