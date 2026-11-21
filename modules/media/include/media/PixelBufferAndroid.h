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

#ifndef CORAL_MEDIA_PIXEL_BUFFER_ANDROID_H
#define CORAL_MEDIA_PIXEL_BUFFER_ANDROID_H

#include <media/PixelBufferHandle.h>

#if defined(__ANDROID__)

struct AHardwareBuffer;

namespace coral {

/**
 * Wraps an AHardwareBuffer as a PixelBuffer without copying it. The handle takes its own
 * AHardwareBuffer reference; a GPU consumer can use nativeHandle(AndroidHardwareBuffer).
 */
std::shared_ptr<PixelBuffer> createAndroidHardwareBufferPixelBuffer(AHardwareBuffer* buffer,
  int width, int height, PixelFormat format, const ColorInfo& color = {}, int64_t timeUs = 0);

} // namespace coral

#endif // defined(__ANDROID__)

#endif // CORAL_MEDIA_PIXEL_BUFFER_ANDROID_H
