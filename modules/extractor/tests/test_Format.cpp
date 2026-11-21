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

#include <media/Format.h>
#include <media/Types.h>

#include <gtest/gtest.h>

#include <memory>
#include <string>

using coral::CueReplacementBehavior;
using coral::MediaFormat;
using coral::MimeTypes;
using coral::SubtitleRenderMode;

namespace {

std::shared_ptr<MediaFormat> makeFormat(const std::string& mimeType) {
  return MediaFormat::CreateAudioFormat(mimeType, 0, 0);
}

} // namespace

TEST(MediaFormatTest, RecognizesTheMimeTypesOfSubtitles) {
  // Every MIME type that `MimeTypes::isText` accepts must be a subtitle, including the ones that do
  // not start with `text/`: a player that only recognized the prefix used to treat an MP4 timed text
  // track as an unknown track and drop it.
  EXPECT_TRUE(makeFormat(MimeTypes::TEXT_VTT)->isSubtitle());
  EXPECT_TRUE(makeFormat(MimeTypes::TEXT_SSA)->isSubtitle());
  EXPECT_TRUE(makeFormat(MimeTypes::TEXT_UNKNOWN)->isSubtitle());
  EXPECT_TRUE(makeFormat(MimeTypes::APPLICATION_SUBRIP)->isSubtitle());
  EXPECT_TRUE(makeFormat(MimeTypes::APPLICATION_TTML)->isSubtitle());
  EXPECT_TRUE(makeFormat(MimeTypes::APPLICATION_TX3G)->isSubtitle());
  EXPECT_TRUE(makeFormat(MimeTypes::APPLICATION_MP4VTT)->isSubtitle());
  EXPECT_TRUE(makeFormat(MimeTypes::APPLICATION_CEA608)->isSubtitle());
  EXPECT_TRUE(makeFormat(MimeTypes::APPLICATION_PGS)->isSubtitle());
  EXPECT_TRUE(makeFormat(MimeTypes::APPLICATION_VOBSUB)->isSubtitle());
}

TEST(MediaFormatTest, DoesNotMistakeOtherMediaForSubtitles) {
  EXPECT_FALSE(makeFormat(MimeTypes::VIDEO_H264)->isSubtitle());
  EXPECT_FALSE(makeFormat(MimeTypes::AUDIO_AAC)->isSubtitle());
  EXPECT_FALSE(makeFormat(MimeTypes::APPLICATION_ID3)->isSubtitle());
  EXPECT_FALSE(makeFormat(MimeTypes::APPLICATION_EMSG)->isSubtitle());
  EXPECT_FALSE(makeFormat("")->isSubtitle());
}

TEST(MediaFormatTest, DefaultsTheCueReplacementBehaviorToMerge) {
  const auto format = makeFormat(MimeTypes::TEXT_VTT);
  EXPECT_EQ(CueReplacementBehavior::kMerge, format->getCueReplacementBehavior());
}

TEST(MediaFormatTest, KeepsAnExplicitCueReplacementBehavior) {
  auto format = makeFormat(MimeTypes::APPLICATION_TX3G);
  format->setCueReplacementBehavior(CueReplacementBehavior::kReplace);
  EXPECT_EQ(CueReplacementBehavior::kReplace, format->getCueReplacementBehavior());
  // The value is part of the format, so a derivation of the format keeps it.
  const auto derived = format->buildUpon();
  EXPECT_EQ(CueReplacementBehavior::kReplace, derived->getCueReplacementBehavior());
}

TEST(MediaFormatTest, DescribesTheCueOptionsOfASubtitle) {
  auto format = makeFormat(MimeTypes::APPLICATION_MP4VTT);
  format->setCueReplacementBehavior(CueReplacementBehavior::kReplace);
  const auto description = format->toString();
  EXPECT_NE(std::string::npos, description.find("cue-replacement-behavior=replace"));
}

TEST(MediaFormatTest, InfersTheFileTypeOfASubtitleDocument) {
  EXPECT_EQ(coral::FileTypes::SRT,
            coral::FileTypes::inferFileTypeFromMimeType(MimeTypes::APPLICATION_SUBRIP));
  EXPECT_EQ(coral::FileTypes::SSA,
            coral::FileTypes::inferFileTypeFromMimeType(MimeTypes::TEXT_SSA));
  EXPECT_EQ(coral::FileTypes::WEBVTT,
            coral::FileTypes::inferFileTypeFromMimeType(MimeTypes::TEXT_VTT));
#ifdef CORAL_ASSETS_DIR
  const std::string directory = std::string(CORAL_ASSETS_DIR) + "/subtitle/";
  EXPECT_EQ(coral::FileTypes::SRT, coral::FileTypes::inferFileTypeFromUrl(directory + "sample.srt"));
  EXPECT_EQ(coral::FileTypes::SSA, coral::FileTypes::inferFileTypeFromUrl(directory + "sample.ass"));
  EXPECT_EQ(coral::FileTypes::WEBVTT,
            coral::FileTypes::inferFileTypeFromUrl(directory + "sample.vtt"));
#endif
}

TEST(MediaFormatTest, NamesTheSubtitleRenderModes) {
  // The enum is shared by the two players, so its values are pinned rather than implicit.
  EXPECT_EQ(0, static_cast<int>(SubtitleRenderMode::kAuto));
  EXPECT_EQ(1, static_cast<int>(SubtitleRenderMode::kOutputOnly));
  EXPECT_EQ(2, static_cast<int>(SubtitleRenderMode::kBurnIn));
}
