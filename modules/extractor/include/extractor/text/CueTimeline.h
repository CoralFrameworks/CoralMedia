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

#ifndef CORAL_EXTRACTOR_TEXT_CUE_TIMELINE_H
#define CORAL_EXTRACTOR_TEXT_CUE_TIMELINE_H

#include <extractor/text/CueGroup.h>
#include <extractor/text/CuesWithTiming.h>

#include <media/Definitions.h>
#include <media/Types.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace coral {

/**
 * The cues of a subtitle track, indexed by the time of the item.
 *
 * <p>This is the player side counterpart of ExoPlayer's {@code TextRenderer} cue bookkeeping: a
 * parser emits {@link AVCuesWithTiming} per sample, the time of the sample maps the cues to the time
 * line of the item, and the players ask the timeline which cues are shown at a given time. It lives
 * in the extractor module so that the compose player and the playback player share one
 * implementation of the merge/replace rules, the seek reset and the memory window instead of growing
 * two that drift apart.
 *
 * <p>The timeline is thread safe: a subtitle track is read by a producer thread while the transport
 * thread asks for the cues of the current frame. A caller never holds the lock while it draws.
 *
 * <h3>Memory</h3>
 * Only the cues of a window behind the newest sample are kept, and no more than
 * {@link #kMaxCues} cues: a one hour subtitle track must not turn into one hour of cues in memory
 * because the reader happened to run ahead.
 */
class AVCueTimeline final {
public:
  /** How far behind the newest sample the cues are kept, in microseconds. */
  static constexpr int64_t kDefaultRetainDurationUs = 30000000;

  /** The hard limit of the cues that are kept, whatever the window is. */
  static constexpr size_t kMaxCues = 8192;

  explicit AVCueTimeline(int64_t retainDurationUs = kDefaultRetainDurationUs);

  AVCueTimeline(const AVCueTimeline& other) = delete;

  AVCueTimeline& operator=(const AVCueTimeline& other) = delete;

  /**
   * Drops every cue.
   *
   * <p>Called on a seek, when the item changes and when the selected subtitle track changes: the
   * samples that were read are no longer the ones of the time that is about to be played.
   */
  void clear();

  /**
   * Adds the cues that one sample produced.
   *
   * @param cues            The result of the parser for the sample.
   * @param sampleStartUs   The presentation time of the sample, in microseconds.
   * @param sampleEndUs     The end of the sample, or {@link Definitions::kTimeUnset} when unknown.
   * @param behavior        Whether the cues replace the ones that are still shown.
   * @param subsampleOffsetUs {@link MediaFormat::OFFSET_SAMPLE_RELATIVE} when the timing inside the
   *                        sample is relative to its own time stamp, or the offset to add to it.
   */
  void add(const AVCuesWithTiming& cues, int64_t sampleStartUs, int64_t sampleEndUs,
           CueReplacementBehavior behavior, int64_t subsampleOffsetUs);

  /**
   * Returns the cues that are shown at @p timeUs.
   *
   * @param timeUs The time of the item, in microseconds.
   */
  [[nodiscard]] AVCueGroup cuesAt(int64_t timeUs) const;

  /**
   * Returns the next time after @p timeUs at which the set of shown cues may change, or
   * {@link Definitions::kTimeUnset} when no cue is pending.
   *
   * <p>A player uses it to schedule the delivery of the next cue rather than polling, and a test uses
   * it to walk the cues of a track. The answer is strictly after @p timeUs, so a caller that loops on
   * it always makes progress.
   */
  [[nodiscard]] int64_t nextChangeTimeUs(int64_t timeUs) const;

  /** Returns how many cues are held. */
  [[nodiscard]] size_t size() const;

  /** Returns whether no cue is held. */
  [[nodiscard]] bool isEmpty() const;

  /** Sets how far behind the newest sample the cues are kept. 0 keeps everything. */
  void setRetainDurationUs(int64_t retainDurationUs);

  /** Returns how far behind the newest sample the cues are kept. */
  [[nodiscard]] int64_t getRetainDurationUs() const;

private:
  /**
   * Ends every cue that is still shown at @p timeUs, and drops the ones that start at or after it.
   *
   * <p>A replacing sample hides the cues that it finds on the screen when it starts; the cues that
   * ended before it are history and are kept, because the reader of a track runs ahead of the
   * playhead and the transport still asks the time line about earlier times.
   */
  void trimCuesAtLocked(int64_t timeUs);

  /** One cue with the times it is shown at. */
  struct TimedCue final {
    //!< The start of the cue, in microseconds of the item.
    int64_t startUs = 0;
    //!< The end of the cue, or 0 when {@link #hasEnd} is false.
    int64_t endUs = 0;
    //!< Whether the cue has a known end.
    bool hasEnd = false;
    //!< The cue itself.
    AVCue cue;
  };

  /** Drops the cues that fell out of the window, and the oldest ones over the hard limit. */
  void pruneLocked();

  /** Returns whether @p cue is shown at @p timeUs. */
  static bool isShownAt(const TimedCue& cue, int64_t timeUs);

  mutable std::mutex mMutex;
  std::vector<TimedCue> mCues;
  int64_t mRetainDurationUs;
  /** The newest end of a sample that was added, used to bound the window. */
  int64_t mNewestSampleEndUs = Definitions::kTimeUnset;
};

} // namespace coral

#endif //CORAL_EXTRACTOR_TEXT_CUE_TIMELINE_H
