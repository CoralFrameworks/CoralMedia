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

#include "NativeSurface.h"

namespace coral {

NativeSurface::NativeSurface(ANativeWindow* nativeWindow) noexcept {
  mNativeSurface = std::unique_ptr<ANativeWindow, NativeWindowDeleter>(nativeWindow);
}

NativeSurface::NativeSurface(std::unique_ptr<JniSurface> surface) noexcept {
  auto env = JvmEnv::get().getEnvironment();
  auto nativeWindow = ANativeWindow_fromSurface(env, (jobject) surface->get());
  mNativeSurface = std::unique_ptr<ANativeWindow, NativeWindowDeleter>(nativeWindow);
  this->surface = std::move(surface);
}

NativeSurface::~NativeSurface() {
  mNativeSurface.reset();
  surface.reset();
}

void* NativeSurface::getInstance() const { return mNativeSurface.get(); }

bool NativeSurface::isValid() const {
  if (surface) {
    return surface != nullptr && surface->isValid();
  }
  return mNativeSurface != nullptr;
}

void NativeSurface::release() {
  if (surface) {
    surface->release();
  }
  if (mNativeSurface != nullptr) {
    mNativeSurface.reset();
  }
}

} // namespace coral
