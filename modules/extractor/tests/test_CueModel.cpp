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

#include <extractor/text/Cue.h>
#include <extractor/text/CueGroup.h>
#include <extractor/text/CuesWithTiming.h>

#include <media/Definitions.h>
#include <media/Types.h>

#include <gtest/gtest.h>

#include <string>
#include <vector>

using coral::AVCue;
using coral::AVCueBitmap;
using coral::AVCueGroup;
using coral::AVCueSpan;
using coral::AVCuesWithTiming;
using coral::Definitions;
using coral::PixelFormat;

namespace {

AVCue makeTextCue(const std::string& text, const int zIndex = 0) {
  AVCue cue;
  cue.text = text;
  cue.zIndex = zIndex;
  return cue;
}

} // namespace

TEST(AVCueTest, IsEmptyWithoutTextAndWithoutAPicture) {
  const AVCue cue;
  EXPECT_TRUE(cue.isEmpty());
  EXPECT_FALSE(cue.isBitmap());
  EXPECT_FALSE(cue.hasRenderingAttributes());
}

TEST(AVCueTest, ReportsWhetherItCarriesARenderingAttribute) {
  AVCue cue = makeTextCue("text");
  EXPECT_FALSE(cue.hasRenderingAttributes());

  cue.textAlignment = AVCue::Alignment::kCenter;
  EXPECT_TRUE(cue.hasRenderingAttributes());

  AVCue sized;
  sized.text = "text";
  sized.textSize = 0.05f;
  sized.textSizeType = AVCue::TextSizeType::kFractional;
  EXPECT_TRUE(sized.hasRenderingAttributes());

  AVCue indented;
  indented.text = "text";
  indented.paragraphIndent = 0.05f;
  EXPECT_TRUE(indented.hasRenderingAttributes());
}

TEST(AVCueTest, ComparesByValue) {
  EXPECT_EQ(makeTextCue("a"), makeTextCue("a"));
  EXPECT_NE(makeTextCue("a"), makeTextCue("b"));
  EXPECT_NE(makeTextCue("a", 0), makeTextCue("a", 1));

  AVCue styled = makeTextCue("a");
  AVCueSpan span;
  span.type = AVCueSpan::Type::kTextStyle;
  span.start = 0;
  span.end = 1;
  span.style.bold = true;
  styled.spans.push_back(span);
  EXPECT_NE(makeTextCue("a"), styled);
}

TEST(AVCueTest, ComparesBitmapsByValue) {
  AVCueBitmap bitmap;
  bitmap.width = 2;
  bitmap.height = 2;
  bitmap.format = PixelFormat::kFormatRGBA;
  bitmap.rowBytes = 8;
  bitmap.pixels = {1, 2, 3, 4, 5, 6, 7, 8};

  AVCue image;
  image.bitmap = bitmap;
  EXPECT_FALSE(image.isEmpty());
  EXPECT_TRUE(image.isBitmap());

  AVCue same;
  same.bitmap = bitmap;
  EXPECT_EQ(image, same);

  AVCue other;
  other.bitmap = bitmap;
  other.bitmap.pixels[0] = 9;
  EXPECT_NE(image, other);

  AVCueBitmap encoded;
  encoded.encoded = {0x89, 0x50, 0x4E, 0x47};
  EXPECT_FALSE(encoded.isEmpty());
  AVCue encodedCue;
  encodedCue.bitmap = encoded;
  EXPECT_TRUE(encodedCue.isBitmap());
  EXPECT_NE(encoded, bitmap);
}

TEST(AVCueSpanTest, KnowsWhenItCoversNoText) {
  AVCueSpan span;
  EXPECT_TRUE(span.isEmpty());
  span.start = 1;
  span.end = 2;
  EXPECT_FALSE(span.isEmpty());
}

TEST(AVCueGroupTest, StartsEmptyAtTimeZero) {
  EXPECT_TRUE(AVCueGroup::kEmptyTimeZero.isEmpty());
  EXPECT_EQ(0, AVCueGroup::kEmptyTimeZero.getPresentationTimeUs());
}

TEST(AVCueGroupTest, SortsTheCuesByZIndexAndKeepsTheOrderOfEqualOnes) {
  const AVCueGroup group({makeTextCue("top", 3), makeTextCue("first-low", -1),
                          makeTextCue("second-low", -1), makeTextCue("middle", 1)},
                         1200);

  const auto& cues = group.getCues();
  ASSERT_EQ(4u, cues.size());
  EXPECT_EQ("first-low", cues[0].text);
  EXPECT_EQ("second-low", cues[1].text);
  EXPECT_EQ("middle", cues[2].text);
  EXPECT_EQ("top", cues[3].text);
  EXPECT_EQ(1200, group.getPresentationTimeUs());
}

TEST(AVCueGroupTest, ComparesTheCuesAndTheTime) {
  const AVCueGroup group({makeTextCue("a")}, 10);
  EXPECT_EQ(group, AVCueGroup({makeTextCue("a")}, 10));
  EXPECT_NE(group, AVCueGroup({makeTextCue("a")}, 11));
  EXPECT_NE(group, AVCueGroup({makeTextCue("b")}, 10));
}

TEST(AVCuesWithTimingTest, ReportsWhetherEveryCueHasADuration) {
  AVCuesWithTiming cues;
  EXPECT_TRUE(cues.isEmpty());
  EXPECT_FALSE(cues.allCuesHaveDuration());

  cues.cues.push_back(makeTextCue("a"));
  EXPECT_FALSE(cues.isEmpty());
  EXPECT_FALSE(cues.allCuesHaveDuration());

  cues.cueDurationsUs.push_back(1000);
  EXPECT_TRUE(cues.allCuesHaveDuration());

  cues.cues.push_back(makeTextCue("b"));
  EXPECT_FALSE(cues.allCuesHaveDuration());
}

TEST(AVCuesWithTimingTest, DefaultsToNoTimingOfItsOwn) {
  const AVCuesWithTiming cues;
  EXPECT_EQ(Definitions::kTimeUnset, cues.startTimeUs);
  EXPECT_EQ(Definitions::kTimeUnset, cues.durationUs);
}
