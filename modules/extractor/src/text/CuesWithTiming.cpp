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

#include <extractor/text/CuesWithTiming.h>

namespace coral {

bool operator==(const AVCuesWithTiming& lhs, const AVCuesWithTiming& rhs) {
  return lhs.cues == rhs.cues && lhs.cueTimesUs == rhs.cueTimesUs &&
         lhs.cueDurationsUs == rhs.cueDurationsUs && lhs.startTimeUs == rhs.startTimeUs &&
         lhs.durationUs == rhs.durationUs;
}

} // namespace coral
