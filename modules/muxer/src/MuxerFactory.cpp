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

#include "muxer/MuxerFactory.h"

#include "AacMuxer.h"
#include "WavMuxer.h"
#include "mp4/FragmentedMp4Muxer.h"
#include "mp4/Mp4Muxer.h"
#include "ogg/OggMuxer.h"
#include "webm/WebmMuxer.h"

namespace coral {
namespace muxer {
namespace {

class BuiltinMuxerFactory final : public MuxerFactory {
public:
  [[nodiscard]] bool supports(const Container container) const noexcept override {
    return container == Container::kMp4 || container == Container::kFragmentedMp4 ||
           container == Container::kWav || container == Container::kAdts ||
           container == Container::kWebM || container == Container::kOgg;
  }

  std::unique_ptr<Muxer> create(const std::string& url, const Container container,
    const MuxerOptions& options, Error* error) override {
    switch (container) {
      case Container::kMp4:
        if (options.fragmentDurationUs > 0) {
          // The option is the original way to ask for a fragmented file; honor it even when the
          // caller did not use the dedicated container enum.
          return mp4::FragmentedMp4Muxer::create(url, options, error);
        }
        return mp4::Mp4Muxer::create(url, options, error);
      case Container::kFragmentedMp4:
        return mp4::FragmentedMp4Muxer::create(url, options, error);
      case Container::kWav:
        return WavMuxer::create(url, options, error);
      case Container::kAdts:
        return AacMuxer::create(url, options, error);
      case Container::kWebM:
        return webm::WebmMuxer::create(url, options, error);
      case Container::kOgg:
        return ogg::OggMuxer::create(url, options, error);
      default:
        if (error != nullptr) {
          error->code = 1;
          error->message = "the container is not implemented yet";
        }
        return nullptr;
    }
  }
};

} // namespace

std::unique_ptr<MuxerFactory> createMuxerFactory() {
  return std::make_unique<BuiltinMuxerFactory>();
}

} // namespace muxer
} // namespace coral
