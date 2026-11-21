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

#ifndef CORAL_EXTRACTOR_TEXT_CUE_GROUP_H
#define CORAL_EXTRACTOR_TEXT_CUE_GROUP_H

#include <extractor/text/Cue.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace coral {

/**
 * The cues that are shown together at one time.
 *
 * <p>This is the counterpart of ExoPlayer's {@code androidx.media3.common.text.CueGroup}, and it is
 * what a player hands to an application when it delivers subtitles: one group per moment at which
 * the set of shown cues changes, including the empty group that tells the application to clear.
 *
 * <p>The cues are sorted by {@link AVCue::zIndex} in ascending order, and a cue that comes later in
 * the group is drawn above an earlier one when their boxes overlap.
 */
class AVCueGroup final {
public:
  /** An empty group, presented at time zero. */
  static const AVCueGroup kEmptyTimeZero;

  /** An empty group at time zero. */
  AVCueGroup() = default;

  /**
   * @param cues The cues of the group.
   * @param presentationTimeUs The time the group describes, in microseconds.
   */
  explicit AVCueGroup(std::vector<AVCue> cues, int64_t presentationTimeUs = 0);

  /** The cues of the group, ordered by z index. */
  [[nodiscard]] const std::vector<AVCue>& getCues() const { return mCues; }

  /** The time of the group, in microseconds. */
  [[nodiscard]] int64_t getPresentationTimeUs() const { return mPresentationTimeUs; }

  /** Whether the group holds no cue, which is the group that clears the screen. */
  [[nodiscard]] bool isEmpty() const { return mCues.empty(); }

  /** Returns a human readable description of the group, for diagnostics. */
  [[nodiscard]] std::string toString() const;

private:
  std::vector<AVCue> mCues;
  int64_t mPresentationTimeUs = 0;
};

/** Compares the two groups by value. */
bool operator==(const AVCueGroup& lhs, const AVCueGroup& rhs);

/** Compares the two groups by value. */
inline bool operator!=(const AVCueGroup& lhs, const AVCueGroup& rhs) {
  return !(lhs == rhs);
}

} // namespace coral

#endif //CORAL_EXTRACTOR_TEXT_CUE_GROUP_H
