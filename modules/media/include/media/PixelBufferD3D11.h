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

#ifndef CORAL_MEDIA_PIXEL_BUFFER_D3D11_H
#define CORAL_MEDIA_PIXEL_BUFFER_D3D11_H

#include <media/PixelBufferHandle.h>

#include <functional>
#include <memory>

#if defined(_WIN32)

struct ID3D11Texture2D;

namespace coral {

/**
 * Wraps a D3D11 texture as a PixelBuffer without copying it. The handle takes its own COM reference.
 *
 * The texture is the producer's storage; the caller must keep its contents stable while the effect
 * graph samples it. GPU completion order is deliberately not encoded here yet: the later SyncToken
 * phase adds that explicitly.
 *
 * @param release Called when the handle is released or destroyed, which is how a decoder buffer
 *                that backs the texture returns to its producer.
 */
std::shared_ptr<PixelBuffer> createD3D11PixelBuffer(ID3D11Texture2D* texture, int width, int height,
  PixelFormat format, const ColorInfo& color = {}, int64_t timeUs = 0,
  std::function<void()> release = nullptr);

} // namespace coral

#endif // defined(_WIN32)

#endif // CORAL_MEDIA_PIXEL_BUFFER_D3D11_H
