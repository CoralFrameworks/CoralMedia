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
#include <extractor/text/CuesWithTiming.h>
#include <extractor/text/SubtitleParser.h>
#include <extractor/text/ssa/SsaParser.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using coral::AVCue;
using coral::AVCueSpan;
using coral::AVCuesWithTiming;
using coral::AVSsaParser;
using coral::AVSubtitleParser;

namespace {

const char* kHeader = "[Script Info]\n"
                      "ScriptType: v4.00+\n"
                      "PlayResX: 1000\n"
                      "PlayResY: 500\n"
                      "\n"
                      "[V4+ Styles]\n"
                      "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, "
                      "OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, "
                      "ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, "
                      "MarginR, MarginV, Encoding\n"
                      "Style: Default,Arial,20,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,"
                      "0,100,100,0,0,1,2,2,2,10,10,10,1\n"
                      "\n"
                      "[Events]\n"
                      "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, "
                      "Text\n";

std::vector<AVCuesWithTiming>
parseSsa(const std::string& document,
         const AVSubtitleParser::OutputOptions& options = AVSubtitleParser::OutputOptions::allCues()) {
  AVSsaParser parser;
  std::vector<AVCuesWithTiming> results;
  const bool parsed = parser.parse(reinterpret_cast<const uint8_t*>(document.data()), document.size(),
                                   options,
                                   [&results](const AVCuesWithTiming& cues) {
                                     results.push_back(cues);
                                   });
  EXPECT_TRUE(parsed);
  return results;
}

std::string documentWith(const std::string& dialogueLines) {
  return std::string(kHeader) + dialogueLines;
}

std::string textsOf(const AVCuesWithTiming& cues) {
  std::string text;
  for (const auto& cue : cues.cues) {
    if (!text.empty()) {
      text += "|";
    }
    text += cue.text;
  }
  return text;
}

bool hasStyleSpan(const AVCue& cue, bool bold, bool italic) {
  for (const auto& span : cue.spans) {
    if (span.type == AVCueSpan::Type::kTextStyle && span.style.bold == bold &&
        span.style.italic == italic) {
      return true;
    }
  }
  return false;
}

bool hasColor(const AVCue& cue, const uint32_t color) {
  for (const auto& span : cue.spans) {
    if (span.type == AVCueSpan::Type::kForegroundColor && span.color == color) {
      return true;
    }
  }
  return false;
}

} // namespace

TEST(AVSsaParserTest, ParsesTheHeaderAndTheDialogueLine) {
  const auto results =
    parseSsa(documentWith("Dialogue: 0,0:00:01.00,0:00:03.00,Default,,0,0,0,,Hello\n"));
  ASSERT_EQ(1u, results.size());
  EXPECT_EQ(1000000, results[0].startTimeUs);
  EXPECT_EQ(2000000, results[0].durationUs);
  ASSERT_EQ(1u, results[0].cues.size());
  const AVCue& cue = results[0].cues[0];
  EXPECT_EQ("Hello", cue.text);
  EXPECT_EQ(AVCue::Alignment::kCenter, cue.textAlignment);
  EXPECT_EQ(AVCue::AnchorType::kMiddle, cue.positionAnchor);
  EXPECT_EQ(AVCue::AnchorType::kEnd, cue.lineAnchor);
  EXPECT_FLOAT_EQ(0.5f, cue.position);
  EXPECT_FLOAT_EQ(0.95f, cue.line);
  // The font size of the style is relative to the script height.
  EXPECT_FLOAT_EQ(0.04f, cue.textSize);
  EXPECT_EQ(AVCue::TextSizeType::kFractionalIgnorePadding, cue.textSizeType);
  // The primary colour of the style is white.
  EXPECT_TRUE(hasColor(cue, 0xFFFFFFFFu));
}

TEST(AVSsaParserTest, AppliesThePlacementOverrides) {
  const auto results =
    parseSsa(documentWith("Dialogue: 0,0:00:00.00,0:00:01.00,Default,,0,0,0,,"
                          "{\\an7\\pos(100,50)}Top left\n"));
  const AVCue& cue = results[0].cues[0];
  EXPECT_EQ("Top left", cue.text);
  EXPECT_EQ(AVCue::Alignment::kLeft, cue.textAlignment);
  EXPECT_EQ(AVCue::AnchorType::kStart, cue.positionAnchor);
  EXPECT_EQ(AVCue::AnchorType::kStart, cue.lineAnchor);
  EXPECT_FLOAT_EQ(0.1f, cue.position);
  EXPECT_FLOAT_EQ(0.1f, cue.line);
  EXPECT_EQ(AVCue::LineType::kFraction, cue.lineType);
}

TEST(AVSsaParserTest, AppliesTheInlineStyleOverrides) {
  const auto results =
    parseSsa(documentWith("Dialogue: 0,0:00:00.00,0:00:01.00,Default,,0,0,0,,"
                          "{\\i1}italic{\\i0} plain\n"));
  const AVCue& cue = results[0].cues[0];
  EXPECT_EQ("italic plain", cue.text);
  EXPECT_TRUE(hasStyleSpan(cue, false, true));
}

TEST(AVSsaParserTest, AppliesTheInlineColourOverride) {
  const auto results = parseSsa(documentWith("Dialogue: 0,0:00:00.00,0:00:01.00,Default,,0,0,0,,"
                                             "{\\c&H0000FF&}red\n"));
  EXPECT_TRUE(hasColor(results[0].cues[0], 0xFFFF0000u));
}

TEST(AVSsaParserTest, AppliesTheInlineAlphaOverride) {
  const auto results = parseSsa(documentWith("Dialogue: 0,0:00:00.00,0:00:01.00,Default,,0,0,0,,"
                                             "{\\c&HFFFFFF&\\alpha&H80&}half\n"));
  // The inline alpha is a transparency: 0x80 makes the white colour half transparent.
  EXPECT_TRUE(hasColor(results[0].cues[0], 0x7FFFFFFFu));
}

TEST(AVSsaParserTest, ParsesTheLineBreakAndTheHardSpace) {
  const auto results = parseSsa(documentWith("Dialogue: 0,0:00:00.00,0:00:01.00,Default,,0,0,0,,"
                                             "first\\Nsecond\\hthird\n"));
  EXPECT_EQ("first\nsecond\xC2\xA0third", results[0].cues[0].text);
}

TEST(AVSsaParserTest, GroupsTheOverlappingDialoguesByTheirBoundaries) {
  const auto results = parseSsa(documentWith(
    "Dialogue: 0,0:00:00.00,0:00:02.00,Default,,0,0,0,,A\n"
    "Dialogue: 0,0:00:01.00,0:00:03.00,Default,,0,0,0,,B\n"));
  ASSERT_EQ(3u, results.size());
  EXPECT_EQ(0, results[0].startTimeUs);
  EXPECT_EQ(1000000, results[0].durationUs);
  EXPECT_EQ("A", textsOf(results[0]));
  EXPECT_EQ(1000000, results[1].startTimeUs);
  EXPECT_EQ(1000000, results[1].durationUs);
  EXPECT_EQ("A|B", textsOf(results[1]));
  EXPECT_EQ(2000000, results[2].startTimeUs);
  EXPECT_EQ("B", textsOf(results[2]));
}

TEST(AVSsaParserTest, SkipsADialogueLineWithFewerColumnsThanTheFormat) {
  const auto results = parseSsa(documentWith("Dialogue: 0,0:00:00.00,0:00:01.00,Default\n"));
  EXPECT_TRUE(results.empty());
}

TEST(AVSsaParserTest, SkipsADialogueLineWithAnInvalidTiming) {
  const auto results = parseSsa(documentWith("Dialogue: 0,nonsense,0:00:01.00,Default,,0,0,0,,A\n"
                                             "Dialogue: 0,0:00:01.00,0:00:02.00,Default,,0,0,0,,B\n"));
  ASSERT_EQ(1u, results.size());
  EXPECT_EQ("B", textsOf(results[0]));
}

TEST(AVSsaParserTest, UsesTheHeaderThatTheTrackSuppliesOnce) {
  const std::string formatLine =
    "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text";
  const std::string header = "[Script Info]\n"
                             "PlayResX: 400\n"
                             "PlayResY: 200\n"
                             "\n"
                             "[V4+ Styles]\n"
                             "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, "
                             "OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, "
                             "ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, "
                             "Alignment, MarginL, MarginR, MarginV, Encoding\n"
                             "Style: Default,Arial,20,&H00FFFFFF,&H000000FF,&H00000000,"
                             "&H00000000,0,0,0,0,100,100,0,0,1,2,2,2,10,10,10,1\n";
  const std::vector<std::vector<uint8_t>> initializationData = {
    std::vector<uint8_t>(formatLine.begin(), formatLine.end()),
    std::vector<uint8_t>(header.begin(), header.end()),
  };
  AVSsaParser parser(initializationData);
  const std::string sample = "Dialogue: 0,0:00:00.00,0:00:01.00,Default,,0,0,0,,Hi\n";
  std::vector<AVCuesWithTiming> results;
  ASSERT_TRUE(parser.parse(reinterpret_cast<const uint8_t*>(sample.data()), sample.size(),
                           AVSubtitleParser::OutputOptions::allCues(),
                           [&results](const AVCuesWithTiming& cues) {
                             results.push_back(cues);
                           }));
  ASSERT_EQ(1u, results.size());
  EXPECT_EQ(0, results[0].startTimeUs);
  EXPECT_EQ(1000000, results[0].durationUs);
  EXPECT_EQ("Hi", textsOf(results[0]));
  // The script height comes from the header, so the font size of the style is 20 / 200.
  EXPECT_FLOAT_EQ(0.1f, results[0].cues[0].textSize);
}

TEST(AVSsaParserTest, ReplacesTheHeaderOfTheNextDocument) {
  AVSsaParser parser;
  std::vector<AVCuesWithTiming> first;
  std::vector<AVCuesWithTiming> second;
  const auto parse = [&parser](const std::string& document, std::vector<AVCuesWithTiming>& out) {
    EXPECT_TRUE(parser.parse(reinterpret_cast<const uint8_t*>(document.data()), document.size(),
                             AVSubtitleParser::OutputOptions::allCues(),
                             [&out](const AVCuesWithTiming& cues) { out.push_back(cues); }));
  };
  parse(documentWith("Dialogue: 0,0:00:00.00,0:00:01.00,Default,,0,0,0,,Hi\n"), first);
  ASSERT_EQ(1u, first.size());
  EXPECT_FLOAT_EQ(0.04f, first[0].cues[0].textSize);

  // The next document states its own script height, which must replace the previous one.
  std::string document = documentWith("Dialogue: 0,0:00:00.00,0:00:01.00,Default,,0,0,0,,Hi\n");
  const std::string replaceFrom = "PlayResY: 500";
  const size_t position = document.find(replaceFrom);
  ASSERT_NE(std::string::npos, position);
  document.replace(position, replaceFrom.size(), "PlayResY: 1000");
  parse(document, second);
  ASSERT_EQ(1u, second.size());
  EXPECT_FLOAT_EQ(0.02f, second[0].cues[0].textSize);
}

TEST(AVSsaParserTest, EmitsTheCuesAfterTheRequestedStartFirst) {
  const std::string document = documentWith(
    "Dialogue: 0,0:00:00.00,0:00:01.00,Default,,0,0,0,,First\n"
    "Dialogue: 0,0:00:02.00,0:00:03.00,Default,,0,0,0,,Second\n");
  const auto afterOnly =
    parseSsa(document, AVSubtitleParser::OutputOptions::onlyCuesAfter(1500000));
  ASSERT_EQ(1u, afterOnly.size());
  EXPECT_EQ("Second", textsOf(afterOnly[0]));

  const auto allCues =
    parseSsa(document, AVSubtitleParser::OutputOptions::cuesAfterThenRemainingCuesBefore(1500000));
  ASSERT_EQ(2u, allCues.size());
  EXPECT_EQ("Second", textsOf(allCues[0]));
  EXPECT_EQ("First", textsOf(allCues[1]));
}
