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

#include <extractor/text/CueTimeline.h>

#include <media/Format.h>

#include <algorithm>
#include <iterator>
#include <limits>
#include <utility>

namespace coral {
namespace {

/** Adds two times, saturating rather than wrapping: a subtitle file may state any number. */
int64_t addSaturated(const int64_t lhs, const int64_t rhs) {
  if (rhs > 0 && lhs > std::numeric_limits<int64_t>::max() - rhs) {
    return std::numeric_limits<int64_t>::max();
  }
  if (rhs < 0 && lhs < std::numeric_limits<int64_t>::min() - rhs) {
    return std::numeric_limits<int64_t>::min();
  }
  return lhs + rhs;
}

} // namespace

AVCueTimeline::AVCueTimeline(const int64_t retainDurationUs)
  : mRetainDurationUs(retainDurationUs) {
}

void AVCueTimeline::clear() {
  std::lock_guard<std::mutex> lock(mMutex);
  mCues.clear();
  mNewestSampleEndUs = Definitions::kTimeUnset;
}

void AVCueTimeline::trimCuesAtLocked(const int64_t timeUs) {
  mCues.erase(std::remove_if(mCues.begin(), mCues.end(),
                [timeUs](const TimedCue& cue) { return cue.startUs >= timeUs; }),
              mCues.end());
  for (auto& cue : mCues) {
    if (!cue.hasEnd || cue.endUs > timeUs) {
      cue.endUs = timeUs;
      cue.hasEnd = true;
    }
  }
}

void AVCueTimeline::add(const AVCuesWithTiming& cues, const int64_t sampleStartUs,
                        const int64_t sampleEndUs, const CueReplacementBehavior behavior,
                        const int64_t subsampleOffsetUs) {
  if (cues.cues.empty()) {
    // A sample without a cue still clears the screen on a track whose samples replace the cues that
    // are shown: an empty MP4 WebVTT sample (`vtte`) is exactly that. The cues that precede the
    // sample are kept, so that a reader that ran ahead of the playhead does not erase what is still
    // to be shown at an earlier time.
    if (behavior == CueReplacementBehavior::kReplace) {
      std::lock_guard<std::mutex> lock(mMutex);
      trimCuesAtLocked(sampleStartUs);
    }
    return;
  }

  // The time of the first cue, mapped to the time line of the item. This is the rule of ExoPlayer's
  // SubtitleTranscodingTrackOutput: a sample that carries no timing of its own is placed at the time
  // of the container sample, and a sample that carries one is either relative to the sample
  // (OFFSET_SAMPLE_RELATIVE) or stated in the time base the format offset describes.
  int64_t baseUs = sampleStartUs;
  if (cues.startTimeUs != Definitions::kTimeUnset) {
    baseUs = subsampleOffsetUs == MediaFormat::OFFSET_SAMPLE_RELATIVE
               ? addSaturated(sampleStartUs, cues.startTimeUs)
               : addSaturated(cues.startTimeUs, subsampleOffsetUs);
  }

  // Where the last cue of a sample that states no duration ends.
  int64_t sampleLimitUs = Definitions::kTimeUnset;
  if (sampleEndUs != Definitions::kTimeUnset && sampleEndUs > sampleStartUs) {
    sampleLimitUs = sampleEndUs;
  } else if (cues.durationUs != Definitions::kTimeUnset && cues.durationUs > 0) {
    sampleLimitUs = addSaturated(baseUs, cues.durationUs);
  }

  std::vector<TimedCue> timed;
  timed.reserve(cues.cues.size());
  for (size_t i = 0; i < cues.cues.size(); ++i) {
    TimedCue entry;
    entry.cue = cues.cues[i];
    const int64_t offsetUs = i < cues.cueTimesUs.size() ? cues.cueTimesUs[i] : 0;
    entry.startUs = addSaturated(baseUs, offsetUs);
    const int64_t durationUs =
      i < cues.cueDurationsUs.size() ? cues.cueDurationsUs[i] : Definitions::kTimeUnset;
    if (durationUs != Definitions::kTimeUnset && durationUs > 0) {
      entry.endUs = addSaturated(entry.startUs, durationUs);
      entry.hasEnd = true;
    }
    timed.push_back(std::move(entry));
  }

  // A cue without a duration is shown until the next cue of the sample starts, or until the sample
  // ends. The cues are already in the order of the data, so a forward scan is enough.
  for (size_t i = 0; i < timed.size(); ++i) {
    if (timed[i].hasEnd) {
      continue;
    }
    int64_t endUs = sampleLimitUs;
    for (size_t j = i + 1; j < timed.size(); ++j) {
      if (timed[j].startUs > timed[i].startUs) {
        endUs = timed[j].startUs;
        break;
      }
    }
    if (endUs != Definitions::kTimeUnset && endUs > timed[i].startUs) {
      timed[i].endUs = endUs;
      timed[i].hasEnd = true;
    }
  }

  std::lock_guard<std::mutex> lock(mMutex);
  if (behavior == CueReplacementBehavior::kReplace) {
    // The cues of the sample replace the ones that are shown when it starts. The cues that ended
    // before it starts are kept: a decoder reads ahead of the playhead, and dropping them would erase
    // the history of the time line that the transport still asks about.
    trimCuesAtLocked(baseUs);
  }
  mCues.insert(mCues.end(), std::make_move_iterator(timed.begin()),
               std::make_move_iterator(timed.end()));
  // The samples arrive in order, but a seek or a format that states its own times may deliver a
  // cue that starts before one that was already added.
  std::stable_sort(mCues.begin(), mCues.end(), [](const TimedCue& lhs, const TimedCue& rhs) {
    return lhs.startUs < rhs.startUs;
  });
  if (sampleEndUs != Definitions::kTimeUnset &&
      (mNewestSampleEndUs == Definitions::kTimeUnset || sampleEndUs > mNewestSampleEndUs)) {
    mNewestSampleEndUs = sampleEndUs;
  }
  pruneLocked();
}

AVCueGroup AVCueTimeline::cuesAt(const int64_t timeUs) const {
  std::vector<AVCue> active;
  {
    std::lock_guard<std::mutex> lock(mMutex);
    for (const auto& cue : mCues) {
      if (isShownAt(cue, timeUs)) {
        active.push_back(cue.cue);
      }
    }
  }
  return AVCueGroup(std::move(active), timeUs);
}

int64_t AVCueTimeline::nextChangeTimeUs(const int64_t timeUs) const {
  std::lock_guard<std::mutex> lock(mMutex);
  int64_t nextUs = Definitions::kTimeUnset;
  for (const auto& cue : mCues) {
    if (cue.startUs > timeUs && (nextUs == Definitions::kTimeUnset || cue.startUs < nextUs)) {
      nextUs = cue.startUs;
    }
    if (cue.hasEnd && cue.endUs > timeUs &&
        (nextUs == Definitions::kTimeUnset || cue.endUs < nextUs)) {
      nextUs = cue.endUs;
    }
  }
  return nextUs;
}

size_t AVCueTimeline::size() const {
  std::lock_guard<std::mutex> lock(mMutex);
  return mCues.size();
}

bool AVCueTimeline::isEmpty() const {
  std::lock_guard<std::mutex> lock(mMutex);
  return mCues.empty();
}

void AVCueTimeline::setRetainDurationUs(const int64_t retainDurationUs) {
  std::lock_guard<std::mutex> lock(mMutex);
  mRetainDurationUs = retainDurationUs;
  pruneLocked();
}

int64_t AVCueTimeline::getRetainDurationUs() const {
  std::lock_guard<std::mutex> lock(mMutex);
  return mRetainDurationUs;
}

bool AVCueTimeline::isShownAt(const TimedCue& cue, const int64_t timeUs) {
  return cue.startUs <= timeUs && (!cue.hasEnd || timeUs < cue.endUs);
}

void AVCueTimeline::pruneLocked() {
  if (mRetainDurationUs > 0 && mNewestSampleEndUs != Definitions::kTimeUnset) {
    const int64_t horizonUs = mNewestSampleEndUs - mRetainDurationUs;
    mCues.erase(std::remove_if(mCues.begin(), mCues.end(),
                               [horizonUs](const TimedCue& cue) {
                                 return cue.hasEnd && cue.endUs < horizonUs;
                               }),
                mCues.end());
  }
  if (mCues.size() <= kMaxCues) {
    return;
  }
  // The hard limit is a bound that has to hold even when the window does not: a malformed track can
  // state thousands of overlapping cues. The cues that already ended go first, then the oldest ones.
  std::stable_sort(mCues.begin(), mCues.end(), [](const TimedCue& lhs, const TimedCue& rhs) {
    if (lhs.hasEnd != rhs.hasEnd) {
      return lhs.hasEnd;
    }
    return lhs.startUs < rhs.startUs;
  });
  mCues.erase(mCues.begin(), mCues.begin() + static_cast<std::ptrdiff_t>(mCues.size() - kMaxCues));
  std::stable_sort(mCues.begin(), mCues.end(), [](const TimedCue& lhs, const TimedCue& rhs) {
    return lhs.startUs < rhs.startUs;
  });
}

} // namespace coral
