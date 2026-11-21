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

#include "muxer/Muxer.h"

namespace coral {
namespace muxer {

bool Muxer::writeSample(const int trackId, const std::shared_ptr<AVPacketBuffer>& packet) {
  if (packet == nullptr) {
    return false;
  }
  SampleInfo info;
  info.presentationTimeUs = packet->timeUs();
  if (packet->isKeyFrame()) {
    info.flags |= static_cast<uint32_t>(SampleFlag::kKeyFrame);
  }
  if (packet->isCodecConfig()) {
    info.flags |= static_cast<uint32_t>(SampleFlag::kCodecConfig);
  }
  if (packet->isDiscard()) {
    info.flags |= static_cast<uint32_t>(SampleFlag::kDiscard);
  }
  return writeSample(trackId, packet->array(), packet->size(), info);
}

} // namespace muxer
} // namespace coral
