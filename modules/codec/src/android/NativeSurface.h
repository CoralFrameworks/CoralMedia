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

#ifndef CORAL_CODEC_ANDROID_NATIVE_SURFACE_H
#define CORAL_CODEC_ANDROID_NATIVE_SURFACE_H

#include <codec/MediaCodec.h>

#include <android/native_window.h>
#include <android/native_window_jni.h>

#include <core/jni/android/Surface.h>

#include <memory>

namespace coral {

class NativeSurface final : public VideoSurface {
public:
  explicit NativeSurface(ANativeWindow* nativeWindow) noexcept;

  explicit NativeSurface(std::unique_ptr<JniSurface> surface) noexcept;

  ~NativeSurface() override;

  [[nodiscard]] void* getInstance() const override;

  [[nodiscard]] bool isValid() const override;

  void release() override;

private:
  struct NativeWindowDeleter {
    void operator()(ANativeWindow* nativeWindow) const {
      if (nativeWindow != nullptr) {
        ANativeWindow_release(nativeWindow);
        nativeWindow = nullptr;
      }
    }
  };

  std::unique_ptr<JniSurface> surface;
  std::unique_ptr<ANativeWindow, NativeWindowDeleter> mNativeSurface;
};

} // namespace coral

#endif // CORAL_CODEC_ANDROID_NATIVE_SURFACE_H
