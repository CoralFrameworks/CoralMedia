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

#include <media/Definitions.h>
#include <media/Format.h>
#include <media/Types.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

using coral::AVCue;
using coral::AVCueGroup;
using coral::AVCueTimeline;
using coral::AVCuesWithTiming;
using coral::CueReplacementBehavior;
using coral::Definitions;
using coral::MediaFormat;

namespace {

AVCue makeCue(const std::string& text) {
  AVCue cue;
  cue.text = text;
  return cue;
}

AVCuesWithTiming makeSample(const std::string& text, const int64_t startTimeUs = Definitions::kTimeUnset,
                            const int64_t durationUs = Definitions::kTimeUnset) {
  AVCuesWithTiming sample;
  sample.cues.push_back(makeCue(text));
  sample.startTimeUs = startTimeUs;
  sample.durationUs = durationUs;
  return sample;
}

/** Returns the text of the cues of a group, joined by `+`. */
std::string textsOf(const AVCueGroup& group) {
  std::string text;
  for (const auto& cue : group.getCues()) {
    if (!text.empty()) {
      text += "+";
    }
    text += cue.text;
  }
  return text;
}

} // namespace

TEST(AVCueTimelineTest, ShowsTheCueOfASampleOverTheTimeOfTheSample) {
  AVCueTimeline timeline;
  timeline.add(makeSample("hello"), 1000000, 2000000, CueReplacementBehavior::kMerge,
               MediaFormat::OFFSET_SAMPLE_RELATIVE);

  EXPECT_TRUE(timeline.cuesAt(999999).isEmpty());
  const auto shown = timeline.cuesAt(1000000);
  ASSERT_EQ(1u, shown.getCues().size());
  EXPECT_EQ("hello", shown.getCues()[0].text);
  EXPECT_EQ(1000000, shown.getPresentationTimeUs());
  EXPECT_FALSE(timeline.cuesAt(1999999).isEmpty());
  EXPECT_TRUE(timeline.cuesAt(2000000).isEmpty());}

TEST(AVCueTimelineTest, UsesTheTimingThatTheSampleStates) {
  AVCuesWithTiming sample;
  sample.cues.push_back(makeCue("first"));
  sample.cues.push_back(makeCue("second"));
  sample.startTimeUs = 200000;
  sample.cueTimesUs = {0, 1000000};
  sample.cueDurationsUs = {500000, 500000};

  AVCueTimeline timeline;
  timeline.add(sample, 5000000, 7000000, CueReplacementBehavior::kMerge,
               MediaFormat::OFFSET_SAMPLE_RELATIVE);

  // The time of the sample is the time base: 5000000 + 200000 + the offset of the cue.
  EXPECT_EQ("first", textsOf(timeline.cuesAt(5200000)));
  EXPECT_EQ("first", textsOf(timeline.cuesAt(5699999)));
  EXPECT_TRUE(timeline.cuesAt(5700000).isEmpty());
  EXPECT_EQ("second", textsOf(timeline.cuesAt(6200000)));
  EXPECT_TRUE(timeline.cuesAt(6700000).isEmpty());
}

TEST(AVCueTimelineTest, AddsTheOffsetOfTheFormatToATimeStatedInTheData) {
  AVCuesWithTiming sample;
  sample.cues.push_back(makeCue("cue"));
  sample.startTimeUs = 1000;
  sample.cueTimesUs = {0};
  sample.cueDurationsUs = {100};

  AVCueTimeline timeline;
  timeline.add(sample, 5000, 9000, CueReplacementBehavior::kMerge, 200);

  // A format offset means the timing of the data is in its own base rather than the one of the
  // sample: 1000 + 200 is the start of the cue, the time of the sample is not used.
  EXPECT_EQ("cue", textsOf(timeline.cuesAt(1200)));
  EXPECT_TRUE(timeline.cuesAt(1300).isEmpty());
}

TEST(AVCueTimelineTest, EndsACueWithoutDurationAtTheNextCueOfTheSample) {
  AVCuesWithTiming sample;
  sample.cues.push_back(makeCue("first"));
  sample.cues.push_back(makeCue("second"));
  sample.cueTimesUs = {0, 2000000};

  AVCueTimeline timeline;
  timeline.add(sample, 0, 4000000, CueReplacementBehavior::kMerge,
               MediaFormat::OFFSET_SAMPLE_RELATIVE);

  EXPECT_EQ("first", textsOf(timeline.cuesAt(0)));
  EXPECT_EQ("first", textsOf(timeline.cuesAt(1999999)));
  EXPECT_EQ("second", textsOf(timeline.cuesAt(2000000)));
  EXPECT_EQ("second", textsOf(timeline.cuesAt(3999999)));
  EXPECT_TRUE(timeline.cuesAt(4000000).isEmpty());
}

TEST(AVCueTimelineTest, MergesTheCuesOfConsecutiveSamples) {
  AVCueTimeline timeline;
  timeline.add(makeSample("long", Definitions::kTimeUnset, 3000000), 0, 3000000,
               CueReplacementBehavior::kMerge, MediaFormat::OFFSET_SAMPLE_RELATIVE);
  timeline.add(makeSample("short", Definitions::kTimeUnset, 1000000), 1000000, 2000000,
               CueReplacementBehavior::kMerge, MediaFormat::OFFSET_SAMPLE_RELATIVE);

  EXPECT_EQ("long+short", textsOf(timeline.cuesAt(1500000)));
  EXPECT_EQ("long", textsOf(timeline.cuesAt(2500000)));
}

TEST(AVCueTimelineTest, ReplacesTheCuesWhenTheBehaviorSaysSo) {
  AVCueTimeline timeline;
  timeline.add(makeSample("old", Definitions::kTimeUnset, 3000000), 0, 3000000,
               CueReplacementBehavior::kReplace, MediaFormat::OFFSET_SAMPLE_RELATIVE);
  timeline.add(makeSample("new", Definitions::kTimeUnset, 1000000), 1000000, 2000000,
               CueReplacementBehavior::kReplace, MediaFormat::OFFSET_SAMPLE_RELATIVE);

  EXPECT_EQ("new", textsOf(timeline.cuesAt(1500000)));
  EXPECT_TRUE(timeline.cuesAt(2500000).isEmpty());
}

TEST(AVCueTimelineTest, ClearsTheScreenForAReplaceSampleWithoutACue) {
  AVCueTimeline timeline;
  timeline.add(makeSample("old", Definitions::kTimeUnset, 3000000), 0, 3000000,
               CueReplacementBehavior::kReplace, MediaFormat::OFFSET_SAMPLE_RELATIVE);
  ASSERT_FALSE(timeline.cuesAt(1000000).isEmpty());

  // An empty sample of a track that replaces its cues is how the track clears the screen.
  AVCuesWithTiming empty;
  timeline.add(empty, 1500000, 2000000, CueReplacementBehavior::kReplace,
               MediaFormat::OFFSET_SAMPLE_RELATIVE);
  EXPECT_TRUE(timeline.cuesAt(1500000).isEmpty());
}

TEST(AVCueTimelineTest, KeepsTheCuesOnAMergeSampleWithoutACue) {
  AVCueTimeline timeline;
  timeline.add(makeSample("kept", Definitions::kTimeUnset, 3000000), 0, 3000000,
               CueReplacementBehavior::kMerge, MediaFormat::OFFSET_SAMPLE_RELATIVE);
  AVCuesWithTiming empty;
  timeline.add(empty, 1000000, 2000000, CueReplacementBehavior::kMerge,
               MediaFormat::OFFSET_SAMPLE_RELATIVE);
  EXPECT_EQ("kept", textsOf(timeline.cuesAt(1500000)));
}

TEST(AVCueTimelineTest, ClearDropsEveryCue) {
  AVCueTimeline timeline;
  timeline.add(makeSample("cue", Definitions::kTimeUnset, 3000000), 0, 3000000,
               CueReplacementBehavior::kMerge, MediaFormat::OFFSET_SAMPLE_RELATIVE);
  ASSERT_EQ(1u, timeline.size());
  timeline.clear();
  EXPECT_TRUE(timeline.isEmpty());
  EXPECT_TRUE(timeline.cuesAt(0).isEmpty());
}

TEST(AVCueTimelineTest, ReportsTheNextChangeOfTheShownCues) {
  AVCueTimeline timeline;
  timeline.add(makeSample("cue", Definitions::kTimeUnset, 2000000), 1000000, 3000000,
               CueReplacementBehavior::kMerge, MediaFormat::OFFSET_SAMPLE_RELATIVE);

  // The cue is shown over [1000000, 3000000): the next change is its start, then its end.
  EXPECT_EQ(1000000, timeline.nextChangeTimeUs(0));
  EXPECT_EQ(3000000, timeline.nextChangeTimeUs(1000000));
  EXPECT_EQ(Definitions::kTimeUnset, timeline.nextChangeTimeUs(3000000));
}

TEST(AVCueTimelineTest, DropsTheCuesBehindTheRetainWindow) {
  AVCueTimeline timeline(1000000);
  timeline.add(makeSample("old", Definitions::kTimeUnset, 100000), 0, 100000,
               CueReplacementBehavior::kMerge, MediaFormat::OFFSET_SAMPLE_RELATIVE);
  ASSERT_EQ(1u, timeline.size());

  timeline.add(makeSample("new", Definitions::kTimeUnset, 100000), 5000000, 5100000,
               CueReplacementBehavior::kMerge, MediaFormat::OFFSET_SAMPLE_RELATIVE);
  // The first cue ended long before the newest sample minus the window, so it is dropped.
  EXPECT_EQ(1u, timeline.size());
  EXPECT_EQ("new", textsOf(timeline.cuesAt(5000000)));
  EXPECT_TRUE(timeline.cuesAt(0).isEmpty());
}

TEST(AVCueTimelineTest, KeepsTheNumberOfCuesBounded) {
  AVCuesWithTiming sample;
  const auto count = AVCueTimeline::kMaxCues + 100;
  sample.cues.reserve(count);
  sample.cueTimesUs.reserve(count);
  sample.cueDurationsUs.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    sample.cues.push_back(makeCue("cue"));
    sample.cueTimesUs.push_back(static_cast<int64_t>(i) * 10000);
    sample.cueDurationsUs.push_back(10000);
  }

  AVCueTimeline timeline;
  timeline.add(sample, 0, static_cast<int64_t>(count) * 10000, CueReplacementBehavior::kMerge,
               MediaFormat::OFFSET_SAMPLE_RELATIVE);
  EXPECT_LE(timeline.size(), AVCueTimeline::kMaxCues);
}

TEST(AVCueTimelineTest, KeepsEveryCueWhenTheRetainWindowIsZero) {
  AVCueTimeline timeline(1000000);
  timeline.add(makeSample("first", Definitions::kTimeUnset, 1000000), 0, 1000000,
               CueReplacementBehavior::kMerge, MediaFormat::OFFSET_SAMPLE_RELATIVE);
  timeline.setRetainDurationUs(0);
  timeline.add(makeSample("second", Definitions::kTimeUnset, 1000000), 2000000, 3000000,
               CueReplacementBehavior::kMerge, MediaFormat::OFFSET_SAMPLE_RELATIVE);
  // A window of zero keeps every cue: it is the "no pruning" mode.
  EXPECT_EQ(2u, timeline.size());
}
