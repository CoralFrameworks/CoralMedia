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

#include <extractor/text/DefaultSubtitleParserFactory.h>
#include <extractor/text/SubtitleParser.h>

#include <media/Format.h>
#include <media/Types.h>

#include <gtest/gtest.h>

#include <memory>
#include <string>

using coral::AVSubtitleParser;
using coral::AVSubtitleParserFactory;
using coral::cueReplacementBehaviorForMimeType;
using coral::CueReplacementBehavior;
using coral::MediaFormat;
using coral::MimeTypes;

namespace {

std::shared_ptr<MediaFormat> makeFormat(const std::string& mimeType) {
  return MediaFormat::CreateAudioFormat(mimeType, 0, 0);
}

} // namespace

TEST(AVSubtitleParserFactoryTest, SupportsTheParsersThatAreRegistered) {
  const auto factory = coral::createDefaultSubtitleParserFactory();
  ASSERT_NE(nullptr, factory);
  EXPECT_TRUE(factory->supportsFormat(*makeFormat(MimeTypes::TEXT_VTT)));
  EXPECT_TRUE(factory->supportsFormat(*makeFormat(MimeTypes::APPLICATION_MP4VTT)));
  EXPECT_TRUE(factory->supportsFormat(*makeFormat(MimeTypes::APPLICATION_SUBRIP)));
  EXPECT_TRUE(factory->supportsFormat(*makeFormat(MimeTypes::TEXT_SSA)));
  EXPECT_TRUE(factory->supportsFormat(*makeFormat(MimeTypes::APPLICATION_TX3G)));
  EXPECT_TRUE(factory->supportsFormat(*makeFormat(MimeTypes::APPLICATION_TTML)));
  EXPECT_TRUE(factory->supportsFormat(*makeFormat(MimeTypes::APPLICATION_PGS)));
  EXPECT_TRUE(factory->supportsFormat(*makeFormat(MimeTypes::APPLICATION_VOBSUB)));
  EXPECT_TRUE(factory->supportsFormat(*makeFormat(MimeTypes::APPLICATION_DVBSUBS)));
  EXPECT_TRUE(factory->supportsFormat(*makeFormat(MimeTypes::APPLICATION_CEA608)));
}

TEST(AVSubtitleParserFactoryTest, ReportsTheFormatsThatHaveNoParserYet) {
  const auto factory = coral::createDefaultSubtitleParserFactory();
  EXPECT_FALSE(factory->supportsFormat(*makeFormat(MimeTypes::VIDEO_H264)));
  EXPECT_FALSE(factory->supportsFormat(*makeFormat("")));
}

TEST(AVSubtitleParserFactoryTest, CreatesAParserForASupportedFormat) {
  const auto factory = coral::createDefaultSubtitleParserFactory();
  EXPECT_NE(nullptr, factory->create(*makeFormat(MimeTypes::TEXT_VTT)));
  EXPECT_NE(nullptr, factory->create(*makeFormat(MimeTypes::APPLICATION_MP4VTT)));
  EXPECT_NE(nullptr, factory->create(*makeFormat(MimeTypes::APPLICATION_SUBRIP)));
  EXPECT_NE(nullptr, factory->create(*makeFormat(MimeTypes::TEXT_SSA)));
  EXPECT_NE(nullptr, factory->create(*makeFormat(MimeTypes::APPLICATION_TX3G)));
  EXPECT_NE(nullptr, factory->create(*makeFormat(MimeTypes::APPLICATION_TTML)));
  EXPECT_NE(nullptr, factory->create(*makeFormat(MimeTypes::APPLICATION_PGS)));
  EXPECT_NE(nullptr, factory->create(*makeFormat(MimeTypes::APPLICATION_VOBSUB)));
  EXPECT_NE(nullptr, factory->create(*makeFormat(MimeTypes::APPLICATION_DVBSUBS)));
  EXPECT_NE(nullptr, factory->create(*makeFormat(MimeTypes::APPLICATION_CEA608)));
  EXPECT_NE(nullptr, factory->create(*makeFormat(MimeTypes::APPLICATION_CEA708)));
}

TEST(AVSubtitleParserFactoryTest, CreatesNoParserForAnUnsupportedFormat) {
  const auto factory = coral::createDefaultSubtitleParserFactory();
  EXPECT_EQ(nullptr, factory->create(*makeFormat(MimeTypes::VIDEO_H264)));
}

TEST(AVSubtitleParserFactoryTest, ReportsTheCueReplacementBehaviorOfTheFormat) {
  const auto factory = coral::createDefaultSubtitleParserFactory();
  EXPECT_EQ(CueReplacementBehavior::kMerge,
            factory->getCueReplacementBehavior(*makeFormat(MimeTypes::TEXT_VTT)));
  EXPECT_EQ(CueReplacementBehavior::kMerge,
            factory->getCueReplacementBehavior(*makeFormat(MimeTypes::APPLICATION_SUBRIP)));
  EXPECT_EQ(CueReplacementBehavior::kMerge,
            factory->getCueReplacementBehavior(*makeFormat(MimeTypes::TEXT_SSA)));
  EXPECT_EQ(CueReplacementBehavior::kReplace,
            factory->getCueReplacementBehavior(*makeFormat(MimeTypes::APPLICATION_MP4VTT)));
  EXPECT_EQ(CueReplacementBehavior::kReplace,
            factory->getCueReplacementBehavior(*makeFormat(MimeTypes::APPLICATION_CEA708)));
}

TEST(AVSubtitleParserFactoryTest, MapsTheMimeTypesToTheirReplacementBehavior) {
  // The formats whose samples carry a complete cue set replace the cues that are shown.
  EXPECT_EQ(CueReplacementBehavior::kReplace,
            cueReplacementBehaviorForMimeType(MimeTypes::APPLICATION_MP4VTT));
  EXPECT_EQ(CueReplacementBehavior::kReplace,
            cueReplacementBehaviorForMimeType(MimeTypes::APPLICATION_TX3G));
  EXPECT_EQ(CueReplacementBehavior::kReplace,
            cueReplacementBehaviorForMimeType(MimeTypes::APPLICATION_PGS));
  EXPECT_EQ(CueReplacementBehavior::kReplace,
            cueReplacementBehaviorForMimeType(MimeTypes::APPLICATION_VOBSUB));
  EXPECT_EQ(CueReplacementBehavior::kReplace,
            cueReplacementBehaviorForMimeType(MimeTypes::APPLICATION_DVBSUBS));
  EXPECT_EQ(CueReplacementBehavior::kReplace,
            cueReplacementBehaviorForMimeType(MimeTypes::APPLICATION_CEA708));

  // The formats whose cues state their own duration merge with the cues that are still shown.
  EXPECT_EQ(CueReplacementBehavior::kMerge,
            cueReplacementBehaviorForMimeType(MimeTypes::TEXT_VTT));
  EXPECT_EQ(CueReplacementBehavior::kMerge,
            cueReplacementBehaviorForMimeType(MimeTypes::APPLICATION_SUBRIP));
  EXPECT_EQ(CueReplacementBehavior::kMerge,
            cueReplacementBehaviorForMimeType(MimeTypes::TEXT_SSA));
  EXPECT_EQ(CueReplacementBehavior::kMerge,
            cueReplacementBehaviorForMimeType(MimeTypes::APPLICATION_TTML));
  // An unknown format gets the default of ExoPlayer rather than an error.
  EXPECT_EQ(CueReplacementBehavior::kMerge, cueReplacementBehaviorForMimeType("text/x-unknown"));
}
