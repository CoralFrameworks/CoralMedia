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

#include <codec/MediaMuxer.h>

#if defined(ANDROID) || defined(__ANDROID__)

#include "android/MediaMuxer.h"
#include "android/NativeMuxer.h"

#endif

#if defined(WINDOWS) || defined(WIN32)

#include "win/WmfMediaMuxer.h"

#endif

#if defined(__APPLE__)

#include "darwin/DarwinMediaMuxer.h"

#endif

namespace coral {

namespace {

class BuiltinMuxerFactory final : public MediaMuxerFactory {
public:
  bool isSupportMuxerFormat(MuxerFormat muxerFormat) override;

  std::unique_ptr<AVMediaMuxer> createMuxer() override;
};

bool BuiltinMuxerFactory::isSupportMuxerFormat(MuxerFormat muxerFormat) {
#if defined(ANDROID) || defined(__ANDROID__)
  if (muxerFormat == MuxerFormat::kMuxerFormatMpeg4
      || muxerFormat == MuxerFormat::kMuxerFormatWebm
      || muxerFormat == MuxerFormat::kMuxerFormat3Gpp
      || muxerFormat == MuxerFormat::kMuxerFormatOgg) {
    return true;
  }
#endif
#if defined(WINDOWS) || defined(WIN32)
  // only the containers which are supported by the media foundation sink writer are available.
  if (WmfMediaMuxer::isSupportedFormat(muxerFormat)) {
    return true;
  }
#endif
#if defined(__APPLE__)
  // only the containers which are supported by the AVAssetWriter are available.
  if (DarwinMediaMuxer::isSupportedFormat(muxerFormat)) {
    return true;
  }
#endif
  return false;
}

std::unique_ptr<AVMediaMuxer> BuiltinMuxerFactory::createMuxer() {
#if defined(ANDROID) || defined(__ANDROID__)
#if defined(CORAL_SUPPORTS_NDK_MEDIACODEC)
  return std::make_unique<NativeMuxer>();
#endif
  return std::make_unique<MediaMuxer>();
#endif

#if defined(WINDOWS) || defined(WIN32)
  return std::make_unique<WmfMediaMuxer>();
#endif

#if defined(__APPLE__)
  return std::make_unique<DarwinMediaMuxer>();
#endif

  return {};
}

} // namespace anonymous

std::unique_ptr<MediaMuxerFactory> CreateBuiltinMuxerFactory() {
  return std::make_unique<BuiltinMuxerFactory>();
}

} // namespace coral
