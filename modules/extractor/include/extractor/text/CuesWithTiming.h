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

#ifndef CORAL_EXTRACTOR_TEXT_CUES_WITH_TIMING_H
#define CORAL_EXTRACTOR_TEXT_CUES_WITH_TIMING_H

#include <extractor/text/Cue.h>

#include <media/Definitions.h>

#include <cstdint>
#include <vector>

namespace coral {

/**
 * The cues that one sample of a subtitle track produces, together with their timing.
 *
 * <p>This is the counterpart of ExoPlayer's {@code androidx.media3.extractor.text.CuesWithTiming},
 * and it is the unit a {@link AVSubtitleParser} emits: a subtitle sample either carries its own
 * timing (a WebVTT file, for example) or is a single cue whose timing is the one of the sample in the
 * container (a MP4 timed text sample, for example).
 *
 * <p>The player maps the times to the time line of the item. {@link #startTimeUs} is
 * {@link Definitions::kTimeUnset} when the data carries no timing of its own, in which case the time
 * of the container sample is used.
 *
 * <p>The per cue times and durations are a superset of ExoPlayer's {@code CuesWithTiming}, which
 * describes one interval per instance: a parser may therefore emit one instance per cue interval
 * (leaving {@link #cueTimesUs} and {@link #cueDurationsUs} empty and setting
 * {@link #startTimeUs}/{@link #durationUs}), or one instance that describes several intervals.
 */
class AVCuesWithTiming final {
public:
  /** The cues that the sample produced. */
  std::vector<AVCue> cues;

  /** The start of each cue, relative to {@link #startTimeUs}; empty means "all at
   * {@link #startTimeUs}". */
  std::vector<int64_t> cueTimesUs;

  /** The duration of each cue; an empty vector or an entry of {@link Definitions::kTimeUnset} means
   * "until the next cue or the end of the sample". */
  std::vector<int64_t> cueDurationsUs;

  /** The time of the first cue, or {@link Definitions::kTimeUnset} when the sample defines it. */
  int64_t startTimeUs = Definitions::kTimeUnset;

  /** The duration that covers every cue of this sample, or {@link Definitions::kTimeUnset}. */
  int64_t durationUs = Definitions::kTimeUnset;

  /** Returns whether the sample produced no cue. */
  [[nodiscard]] bool isEmpty() const { return cues.empty(); }

  /**
   * Returns whether every cue of the sample has a known duration.
   *
   * <p>A player needs this to know whether it has to keep the cues of the sample until the next
   * sample replaces them, which is the {@link CueReplacementBehavior::kReplace} case.
   */
  [[nodiscard]] bool allCuesHaveDuration() const {
    if (cues.empty() || cueDurationsUs.size() < cues.size()) {
      return false;
    }
    for (size_t i = 0; i < cues.size(); ++i) {
      if (cueDurationsUs[i] == Definitions::kTimeUnset || cueDurationsUs[i] <= 0) {
        return false;
      }
    }
    return true;
  }
};

/** Compares the two results by value. */
bool operator==(const AVCuesWithTiming& lhs, const AVCuesWithTiming& rhs);

/** Compares the two results by value. */
inline bool operator!=(const AVCuesWithTiming& lhs, const AVCuesWithTiming& rhs) {
  return !(lhs == rhs);
}

} // namespace coral

#endif //CORAL_EXTRACTOR_TEXT_CUES_WITH_TIMING_H
