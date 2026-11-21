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

#ifndef CORAL_MUXER_MUXER_ADAPTER_H
#define CORAL_MUXER_MUXER_ADAPTER_H

#include <codec/MediaMuxer.h>

#include <memory>

namespace coral {
namespace muxer {

/** An AVMediaMuxer over the native MP4/WebM/Ogg writers, for the existing AssetWriter path. */
std::unique_ptr<AVMediaMuxer> createMuxerAdapter();

/** A MediaMuxerFactory that creates the native writer adapter. */
std::unique_ptr<MediaMuxerFactory> createMuxerAdapterFactory();

} // namespace muxer
} // namespace coral

#endif // CORAL_MUXER_MUXER_ADAPTER_H
