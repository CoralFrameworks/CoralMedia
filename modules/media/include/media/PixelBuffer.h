/*
 * Copyright (C) 2022 The Coral Project
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

#ifndef CORAL_MEDIA_PIXEL_BUFFER_H
#define CORAL_MEDIA_PIXEL_BUFFER_H

#include <media/Buffer.h>
#include <media/ColorInfo.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace coral {

class BgraPixelBuffer;
class I420PixelBuffer;
class NV12PixelBuffer;
class PixelBuffer;
class SurfaceFrameSource;

#if defined(ANDROID) || defined(__ANDROID__)
class MediaCodecBuffer;
#endif

/**
 * Base class for decoded frame buffers of different types of pixel format and storage.
 */
class AVPixelBuffer : public AVFrameBuffer {
public:
  static std::shared_ptr<AVPixelBuffer> newFlagsOnlyInstance();

  ~AVPixelBuffer() override;

  /**
   * This function specifies in what pixel format the data is stored in.
   */
  [[nodiscard]] virtual PixelFormat getFormat() const = 0;

  /**
   * Returns the width of the frame in pixels.
   */
  [[nodiscard]] virtual int width() const = 0;

  /**
   * Returns the height of the frame in pixels.
   */
  [[nodiscard]] virtual int height() const = 0;

  /**
   * The consumer of the surface this frame was rendered into, or null for a frame that holds its pixels.
   *
   * A decoder that draws into a surface returns buffers that name a queued frame rather than carrying pixels:
   * whoever converts the frame has to ask the surface for it, and this is how it knows which surface.
   */
  [[nodiscard]] virtual std::shared_ptr<SurfaceFrameSource> getSurfaceFrameSource() {
    return nullptr;
  }

  /**
   * The enum for clockwise rotation
   */
  enum class Rotation : uint8_t {
    kRotation_0,
    kRotation_90,
    kRotation_180,
    kRotation_270,
  };

  /**
   * Returns the rotation of the PixelBuffer
   */
  [[nodiscard]] Rotation getRotation() const {
    return mRotation;
  }

  /**
   * Sets a new rotation of the PixelBuffer
   */
  void setRotation(const Rotation& newRotation) {
    mRotation = newRotation;
  }

  [[nodiscard]] const ColorInfo& getColorInfo() const {
    return mColorInfo;
  }

  void setColorInfo(const ColorInfo& colorInfo) {
    mColorInfo = colorInfo;
  }

  [[nodiscard]] MediaType getMediaType() const override {
    return MediaType::kMediaTypeVideo;
  }

  /**
   * Ensures that data is large enough to accommodate a write of a
   * given length at its current position.
   *
   * @param length    The length of the write that must be accommodated, in bytes.
   */
  virtual void ensureSpaceForWrite(size_t length) {
  }

  virtual std::shared_ptr<I420PixelBuffer> asI420PixelBuffer() { return nullptr; }

  virtual std::shared_ptr<BgraPixelBuffer> asBgraPixelBuffer() { return nullptr; }

  /**
   * Returns the backend-neutral PixelBuffer handle for this frame, or null when the frame has no such
   * handle yet. A decoder implements this without changing its inherited AVPixelBuffer API.
   */
  virtual std::shared_ptr<PixelBuffer> asPixelBufferHandle() { return nullptr; }

  virtual std::shared_ptr<NV12PixelBuffer> asNV12PixelBuffer() { return nullptr; }

#if defined(ANDROID) || defined(__ANDROID__)
  virtual std::shared_ptr<MediaCodecBuffer> asMediaCodecBuffer() { return nullptr; }
#endif

protected:
  Rotation mRotation = Rotation::kRotation_0;
  ColorInfo mColorInfo;
};

/**
 * The video frame buffer with packed BGRA_8888 pixels in CPU memory.
 *
 * <p>This is the layout thumbnail providers and image pipelines produce. It is a plain
 * AVPixelBuffer, so `PixelBuffer::fromAVPixelBuffer` can expose it through the backend-neutral
 * handle without a copy.
 */
class BgraPixelBuffer final : public AVPixelBuffer,
                              public std::enable_shared_from_this<BgraPixelBuffer> {
public:
  BgraPixelBuffer(const int width, const int height)
      : mWidth(width), mHeight(height), mData(static_cast<size_t>(width) * height * 4) {
    mColorInfo.colorSpace = ColorInfo::SpaceType::kSpaceBT709;
    mColorInfo.colorRange = ColorInfo::RangeType::kRangeLimited;
    mColorInfo.colorTransfer = ColorInfo::TransferType::kTransferSDR;
  }

  [[nodiscard]] PixelFormat getFormat() const override {
    return PixelFormat::kFormatBGRA;
  }

  [[nodiscard]] int width() const override {
    return mWidth;
  }

  [[nodiscard]] int height() const override {
    return mHeight;
  }

  [[nodiscard]] uint8_t* data() noexcept {
    return mData.data();
  }

  [[nodiscard]] const uint8_t* data() const noexcept {
    return mData.data();
  }

  [[nodiscard]] size_t size() const noexcept {
    return mData.size();
  }

  void release() override {
  }

  std::shared_ptr<BgraPixelBuffer> asBgraPixelBuffer() override {
    return shared_from_this();
  }

private:
  int mWidth;
  int mHeight;
  std::vector<uint8_t> mData;
};

/**
 * The video frame buffer with pixel format i420.
 */
class I420PixelBuffer final : public AVPixelBuffer,
                              public std::enable_shared_from_this<I420PixelBuffer> {
private:
  Owner<AVPixelBuffer>* owner;
  const int mWidth;
  const int mHeight;
  const int mStrideY;
  const int mStrideU;
  const int mStrideV;
  const int mSize;
  std::unique_ptr<uint8_t[]> mData;

public:
  explicit I420PixelBuffer(const int width, const int height) noexcept
    : I420PixelBuffer(width, height, width, (width + 1) / 2, (width + 1) / 2) {
  }

  explicit I420PixelBuffer(Owner<AVPixelBuffer>* owner, const int width, const int height) noexcept
    : I420PixelBuffer(owner, width, height, width, (width + 1) / 2, (width + 1) / 2) {
  }

  explicit I420PixelBuffer(const int width,
    const int height,
    const int strideY,
    const int strideU,
    const int strideV) noexcept
    : I420PixelBuffer(nullptr, width, height, strideY, strideU, strideV) {
  }

  explicit I420PixelBuffer(Owner<AVPixelBuffer>* owner,
    int width,
    int height,
    int strideY,
    int strideU,
    int strideV) noexcept;

  ~I420PixelBuffer() override;

  PixelFormat getFormat() const override {
    return PixelFormat::kFormatI420;
  }

  int chromaWidth() const { return (width() + 1) / 2; }

  int chromaHeight() const { return (height() + 1) / 2; }

  [[nodiscard]] int width() const override { return mWidth; }

  [[nodiscard]] int height() const override { return mHeight; }

  [[nodiscard]] size_t size() const { return mSize; }

  [[nodiscard]] const uint8_t* getData() const { return mData.get(); }

  [[nodiscard]] int strideY() const { return mStrideY; }

  [[nodiscard]] int strideU() const { return mStrideU; }

  [[nodiscard]] int strideV() const { return mStrideV; }

  [[nodiscard]] const uint8_t* dataY() const { return mData.get(); }

  [[nodiscard]] const uint8_t* dataU() const {
    return mData.get() + strideY() * height();
  }

  [[nodiscard]] const uint8_t* dataV() const {
    return mData.get() + strideY() * height() + strideU() * (int) ((height() + 1) / 2);
  }

  uint8_t* mutableDataY() const noexcept { return const_cast<uint8_t*>(dataY()); }

  uint8_t* mutableDataU() const noexcept { return const_cast<uint8_t*>(dataU()); }

  uint8_t* mutableDataV() const noexcept { return const_cast<uint8_t*>(dataV()); }

  void release() override {
    if (owner) {
      owner->recycleBuffer(shared_from_this());
    }
  }

  std::shared_ptr<AVPixelBuffer> asPixelBuffer() override {
    return shared_from_this();
  }

  std::shared_ptr<I420PixelBuffer> asI420PixelBuffer() override {
    return shared_from_this();
  }

private:
  static int I420DataSize(int height, int stride_y, int stride_u, int stride_v) {
    return stride_y * height + (stride_u + stride_v) * (int) ((height + 1) / 2);
  }
};

/**
 * The video frame buffer with pixel format nv12.
 */
class NV12PixelBuffer final : public AVPixelBuffer,
                              public std::enable_shared_from_this<NV12PixelBuffer> {
private:
  Owner<AVPixelBuffer>* owner;
  const int width_;
  const int height_;
  const int stride_y_;
  const int stride_uv_;
  const int mSize;
  std::unique_ptr<uint8_t[]> mData;

public:
  explicit NV12PixelBuffer(int width, int height) noexcept
    : NV12PixelBuffer(width, height, width, width + width % 2) {
  }

  explicit NV12PixelBuffer(Owner<AVPixelBuffer>* owner, int width, int height) noexcept
    : NV12PixelBuffer(owner, width, height, width, width + width % 2) {
  }

  explicit NV12PixelBuffer(int width, int height, int strideY, int strideUV) noexcept
    : NV12PixelBuffer(nullptr, width, height, strideY, strideUV) {
  }

  explicit NV12PixelBuffer(Owner<AVPixelBuffer>* owner,
    int width,
    int height,
    int strideY,
    int strideUV) noexcept;

  ~NV12PixelBuffer() override;

  [[nodiscard]] PixelFormat getFormat() const override {
    return PixelFormat::kFormatNV12;
  }

  int chromaWidth() const { return (width() + 1) / 2; }

  int chromaHeight() const { return (height() + 1) / 2; }

  [[nodiscard]] int width() const override { return width_; }

  [[nodiscard]] int height() const override { return height_; }

  [[nodiscard]] size_t size() const { return mSize; }

  [[nodiscard]] const uint8_t* getData() const { return mData.get(); }

  [[nodiscard]] int strideY() const { return stride_y_; }

  [[nodiscard]] int strideUV() const { return stride_uv_; }

  [[nodiscard]] const uint8_t* dataY() const { return mData.get(); }

  [[nodiscard]] const uint8_t* dataUV() const { return mData.get() + UVOffset(); }

  uint8_t* mutableDataY() const {
    return const_cast<uint8_t*>(dataY());
  }

  uint8_t* mutableDataUV() const {
    return const_cast<uint8_t*>(dataUV());
  }

  void release() override {
    if (owner) {
      owner->recycleBuffer(shared_from_this());
    }
  }

  std::shared_ptr<AVPixelBuffer> asPixelBuffer() override {
    return shared_from_this();
  }

  std::shared_ptr<NV12PixelBuffer> asNV12PixelBuffer() override {
    return shared_from_this();
  }

private:
  [[nodiscard]] size_t UVOffset() const { return stride_y_ * height_; }

  static int NV12DataSize(const int height, const int stride_y, const int stride_uv) {
    return stride_y * height + stride_uv * ((height + 1) / 2);
  }
};

} // namespace coral

#endif // CORAL_MEDIA_PIXEL_BUFFER_H

