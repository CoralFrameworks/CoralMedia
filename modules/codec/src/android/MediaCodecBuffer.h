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

#ifndef CORAL_CODEC_ANDROID_MEDIACODEC_BUFFER_H
#define CORAL_CODEC_ANDROID_MEDIACODEC_BUFFER_H

#include <media/PixelBuffer.h>
#include <media/SurfaceFrameSource.h>

namespace coral {

/**
 * The implementation of the PixelBuffer based on MediaCodec buffer index
 */
class MediaCodecBuffer final : public AVPixelBuffer,
                               public std::enable_shared_from_this<MediaCodecBuffer> {
public:
  explicit MediaCodecBuffer(const int width, const int height) noexcept
    : MediaCodecBuffer(nullptr, width, height) {
  }

  explicit MediaCodecBuffer(Owner<AVPixelBuffer>* owner, const int width, const int height) noexcept
    : MediaCodecBuffer(owner, width, height, Definitions::kInvalidValue) {
  }

  explicit MediaCodecBuffer(const int width, const int height, const int index) noexcept
    : MediaCodecBuffer(nullptr, width, height, index) {
  }

  explicit MediaCodecBuffer(Owner<AVPixelBuffer>* owner, const int width, const int height, const int index) noexcept
    : owner(owner),
      mWidth(width),
      mHeight(height),
      mIndex(index) {
  }

  ~MediaCodecBuffer() override {
    owner = nullptr;
  }

  [[nodiscard]] PixelFormat getFormat() const override {
    return PixelFormat::kFormatSurface;
  }

  [[nodiscard]] int width() const override {
    return mWidth;
  }

  [[nodiscard]] int height() const override {
    return mHeight;
  }

  [[nodiscard]] int index() const {
    return mIndex;
  }

  void release() override {
    if (owner) {
      owner->recycleBuffer(shared_from_this());
    }
  }

  void setIndex(const int index) {
    mIndex = index;
  }

  std::shared_ptr<MediaCodecBuffer> asMediaCodecBuffer() override {
    return shared_from_this();
  }

  /**
   * The composition reads every decoded buffer through this: a surface-backed frame is still a pixel buffer,
   * and it is the conversion stage that asks the surface for its pixels. Without the override the base's null
   * answer makes a hardware-decoded frame look like something that is not a video frame at all.
   */
  std::shared_ptr<AVPixelBuffer> asPixelBuffer() override {
    return shared_from_this();
  }

  /** The consumer of the surface this frame was rendered into; null in software mode. */
  [[nodiscard]] std::shared_ptr<SurfaceFrameSource> getSurfaceFrameSource() override {
    return mSurfaceFrames.lock();
  }

  /**
   * Names the surface whose queue holds this frame.
   *
   * Weak on purpose: the surface owns the consumer and the composition owns the surface, so a buffer that
   * outlives either of them reports no consumer rather than keeping a dead one alive.
   */
  void setSurfaceFrameSource(const std::shared_ptr<SurfaceFrameSource>& source) {
    mSurfaceFrames = source;
  }

private:
  Owner<AVPixelBuffer>* owner;
  int mWidth;
  int mHeight;
  int mIndex;
  std::weak_ptr<SurfaceFrameSource> mSurfaceFrames;
};

} // namespace coral

#endif // CORAL_CODEC_ANDROID_MEDIACODEC_BUFFER_H

