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

#include <extractor/text/CueGroup.h>

#include <algorithm>
#include <sstream>
#include <string>
#include <utility>

namespace coral {

const AVCueGroup AVCueGroup::kEmptyTimeZero{};

AVCueGroup::AVCueGroup(std::vector<AVCue> cues, const int64_t presentationTimeUs)
  : mCues(std::move(cues)),
    mPresentationTimeUs(presentationTimeUs) {
  // The order of the cues decides which one is drawn on top, so it is the z index and not the order
  // of the data. The sort is stable: two cues with the same z index keep the order the parser gave
  // them, which is the order of the document that produced them.
  std::stable_sort(mCues.begin(), mCues.end(), [](const AVCue& lhs, const AVCue& rhs) {
    return lhs.zIndex < rhs.zIndex;
  });
}

std::string AVCueGroup::toString() const {
  std::ostringstream stream;
  stream << "AVCueGroup{presentationTimeUs=" << mPresentationTimeUs << ",cues=[";
  for (size_t i = 0; i < mCues.size(); ++i) {
    if (i != 0) {
      stream << ",";
    }
    stream << mCues[i].toString();
  }
  stream << "]}";
  return stream.str();
}

bool operator==(const AVCueGroup& lhs, const AVCueGroup& rhs) {
  return lhs.getPresentationTimeUs() == rhs.getPresentationTimeUs() &&
         lhs.getCues() == rhs.getCues();
}

} // namespace coral
