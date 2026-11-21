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

#ifndef CORAL_MEDIA_SYNC_TOKEN_H
#define CORAL_MEDIA_SYNC_TOKEN_H

#include <cstdint>
#include <memory>

namespace coral {

/** The platform signal a SyncToken can export for another GPU API. */
enum class SyncTokenKind : uint8_t {
  None,
  D3D11Fence,
};

struct SyncTokenNativeHandle {
  SyncTokenKind kind = SyncTokenKind::None;
  /** A borrowed platform handle valid until the token is destroyed. */
  void* handle = nullptr;
  /** The timeline value the handle reaches when the token is signalled. */
  uint64_t value = 0;
};

/**
 * A GPU/CPU completion signal attached to a frame.
 *
 * A producer attaches a token to a PixelBufferView when the texture is not guaranteed complete at
 * submit time. The consumer waits before sampling. The interface deliberately does not expose a
 * platform fence type; a backend-specific implementation owns it.
 */
class SyncToken {
public:
  virtual ~SyncToken() = default;

  /** Blocks until signalled or timeout. Returns false on timeout/error. */
  virtual bool wait(uint64_t timeoutNanoseconds) = 0;

  /** Polls without blocking. */
  [[nodiscard]] virtual bool isSignaled() const = 0;

  /** Exports a handle another GPU API can import, when the backend supports it. */
  virtual bool nativeHandle(SyncTokenNativeHandle* out) const {
    static_cast<void>(out);
    return false;
  }
};

} // namespace coral

#endif // CORAL_MEDIA_SYNC_TOKEN_H
