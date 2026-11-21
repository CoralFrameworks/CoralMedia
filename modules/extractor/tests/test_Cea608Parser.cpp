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

#include <extractor/text/cea608/Cea608Parser.h>
#include <extractor/text/Cue.h>
#include <extractor/text/CuesWithTiming.h>
#include <extractor/text/SubtitleParser.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

using coral::AVCea608Parser;
using coral::AVCue;
using coral::AVCueSpan;
using coral::AVCuesWithTiming;
using coral::AVSubtitleParser;

TEST(AVCea608ParserTest, DecodesAPopOnCaption) {
  const std::vector<uint8_t> data{
    0x14, 0x20,
    0x48, 0x45,
    0x4C, 0x4C,
    0x4F, 0x20,
    0x14, 0x2F,
  };
  AVCea608Parser parser;
  std::vector<AVCuesWithTiming> results;
  ASSERT_TRUE(parser.parse(data.data(), data.size(),
    AVSubtitleParser::OutputOptions::allCues(),
    [&results](const AVCuesWithTiming& cues) { results.push_back(cues); }));
  ASSERT_EQ(1u, results.size());
  ASSERT_EQ(1u, results[0].cues.size());
  EXPECT_EQ("HELLO", results[0].cues[0].text);
}

TEST(AVCea608ParserTest, AppliesMidrowColoursAndItalics) {
  const std::vector<uint8_t> data{
    0x14, 0x20,
    0x11, 0x28,  // mid-row: red
    0x52, 0x45,
    0x44, 0x20,
    0x11, 0x2E,  // mid-row: italics
    0x49, 0x54,
    0x14, 0x2F,
  };
  AVCea608Parser parser;
  std::vector<AVCuesWithTiming> results;
  ASSERT_TRUE(parser.parse(data.data(), data.size(),
    AVSubtitleParser::OutputOptions::allCues(),
    [&results](const AVCuesWithTiming& cues) { results.push_back(cues); }));
  ASSERT_EQ(1u, results.size());
  ASSERT_EQ(1u, results[0].cues.size());
  const AVCue& cue = results[0].cues[0];
  ASSERT_EQ(" RED  IT", cue.text);
  bool red = false;
  bool italic = false;
  for (const auto& span: cue.spans) {
    if (span.type == AVCueSpan::Type::kForegroundColor && span.start <= 1 && span.end >= 4) {
      red = span.color == 0xFFFF0000u;
    }
    if (span.type == AVCueSpan::Type::kTextStyle && span.start == 6 && span.end == 8) {
      italic = span.style.italic;
    }
  }
  EXPECT_TRUE(red) << "a mid-row colour must colour the following text";
  EXPECT_TRUE(italic) << "a mid-row italics code must italicise the following text";
}

TEST(AVCea608ParserTest, SelectsOneOfTheTwoTextChannels) {
  const std::vector<uint8_t> data{
    0x14, 0x20,  // channel 1 pop-on
    0x41, 0x42,  // "AB"
    0x14, 0x2F,  // channel 1 EDM
    0x1C, 0x20,  // channel 2 pop-on
    0x43, 0x44,  // "CD"
    0x1C, 0x2F,  // channel 2 EDM
  };
  for (const int channel: {1, 2}) {
    AVCea608Parser parser(channel);
    std::vector<AVCuesWithTiming> results;
    ASSERT_TRUE(parser.parse(data.data(), data.size(),
      AVSubtitleParser::OutputOptions::allCues(),
      [&results](const AVCuesWithTiming& cues) { results.push_back(cues); }));
    ASSERT_EQ(1u, results.size()) << "channel " << channel;
    ASSERT_EQ(1u, results[0].cues.size()) << "channel " << channel;
    EXPECT_EQ(channel == 1 ? "AB" : "CD", results[0].cues[0].text);
  }
}

TEST(AVCea608ParserTest, DecodesSpecialAndExtendedCharacters) {
  const std::vector<uint8_t> special{
    0x14, 0x20,
    0x11, 0x37,  // music note
    0x14, 0x2F,
  };
  AVCea608Parser parser;
  std::vector<AVCuesWithTiming> specialResults;
  ASSERT_TRUE(parser.parse(special.data(), special.size(),
    AVSubtitleParser::OutputOptions::allCues(),
    [&specialResults](const AVCuesWithTiming& cues) { specialResults.push_back(cues); }));
  ASSERT_EQ(1u, specialResults.size());
  ASSERT_EQ(1u, specialResults[0].cues.size());
  EXPECT_EQ("\xE2\x99\xAA", specialResults[0].cues[0].text);

  const std::vector<uint8_t> extended{
    0x14, 0x20,
    0x12, 0x20,  // Á
    0x14, 0x2F,
  };
  std::vector<AVCuesWithTiming> extendedResults;
  ASSERT_TRUE(parser.parse(extended.data(), extended.size(),
    AVSubtitleParser::OutputOptions::allCues(),
    [&extendedResults](const AVCuesWithTiming& cues) { extendedResults.push_back(cues); }));
  ASSERT_EQ(1u, extendedResults.size());
  ASSERT_EQ(1u, extendedResults[0].cues.size());
  EXPECT_EQ("\xC3\x81", extendedResults[0].cues[0].text);
}
