/*
 * Copyright (C) 2023 The Coral Project
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

#ifndef CORAL_CODEC_ANDROID_MEDIACODEC_SURFACE_H
#define CORAL_CODEC_ANDROID_MEDIACODEC_SURFACE_H

#include <codec/MediaCodec.h>

#include <core/jni/android/Surface.h>

namespace coral {

/**
 * The jni wrapper of the MediaCodec's Surface
 */
class MediaCodecSurface final : public VideoSurface {
public:
  explicit MediaCodecSurface(std::unique_ptr<JniSurface> surface) noexcept
    : surface(std::move(surface)) {
  }

  ~MediaCodecSurface() override {
    if (surface) {
      surface->release();
      surface.reset();
    }
  }

  [[nodiscard]] void* getInstance() const override {
    return surface != nullptr ? surface->get() : nullptr;
  }

  [[nodiscard]] bool isValid() const override {
    return surface != nullptr && surface->isValid();
  }

  void release() override {
    if (surface) {
      surface->release();
    }
  }

private:
  std::unique_ptr<JniSurface> surface;
};

} // namespace coral

#endif // CORAL_CODEC_ANDROID_MEDIACODEC_SURFACE_H
