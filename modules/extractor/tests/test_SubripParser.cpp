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
#include <extractor/text/subrip/SubripParser.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

using coral::AVCue;
using coral::AVCueSpan;
using coral::AVCuesWithTiming;
using coral::AVSubripParser;
using coral::AVSubtitleParser;

namespace {

std::vector<AVCuesWithTiming>
parseSubrip(const std::string& document,
            const AVSubtitleParser::OutputOptions& options = AVSubtitleParser::OutputOptions::allCues()) {
  AVSubripParser parser;
  std::vector<AVCuesWithTiming> results;
  const bool parsed = parser.parse(reinterpret_cast<const uint8_t*>(document.data()), document.size(),
                                   options,
                                   [&results](const AVCuesWithTiming& cues) {
                                     results.push_back(cues);
                                   });
  EXPECT_TRUE(parsed);
  return results;
}

} // namespace

TEST(AVSubripParserTest, ParsesACueWithItsIndexAndTiming) {
  const auto results = parseSubrip("1\n"
                                   "00:00:01,000 --> 00:00:02,500\n"
                                   "Hello\n");
  ASSERT_EQ(1u, results.size());
  EXPECT_EQ(1000000, results[0].startTimeUs);
  EXPECT_EQ(1500000, results[0].durationUs);
  ASSERT_EQ(1u, results[0].cues.size());
  EXPECT_EQ("Hello", results[0].cues[0].text);
}

TEST(AVSubripParserTest, AcceptsCRLFAndAMissingIndex) {
  const auto results = parseSubrip("00:00:01,500 --> 00:00:03,000\r\n"
                                   "No index\r\n");
  ASSERT_EQ(1u, results.size());
  EXPECT_EQ(1500000, results[0].startTimeUs);
  EXPECT_EQ("No index", results[0].cues[0].text);
}

TEST(AVSubripParserTest, AcceptsTimingsWithoutHours) {
  const auto results = parseSubrip("1\n"
                                   "00:01,250 --> 00:02,750\n"
                                   "Short\n");
  ASSERT_EQ(1u, results.size());
  EXPECT_EQ(1250000, results[0].startTimeUs);
  EXPECT_EQ(1500000, results[0].durationUs);
}

TEST(AVSubripParserTest, JoinsTheLinesOfACue) {
  const auto results = parseSubrip("1\n"
                                   "00:00:00,000 --> 00:00:02,000\n"
                                   "First line\n"
                                   "Second line\n"
                                   "\n"
                                   "2\n"
                                   "00:00:03,000 --> 00:00:04,000\n"
                                   "Next\n");
  ASSERT_EQ(2u, results.size());
  EXPECT_EQ("First line\nSecond line", results[0].cues[0].text);
  EXPECT_EQ("Next", results[1].cues[0].text);
}

TEST(AVSubripParserTest, ParsesTheInlineMarkup) {
  const auto results = parseSubrip("1\n"
                                   "00:00:00,000 --> 00:00:02,000\n"
                                   "<b>bold</b> <i>italic</i> <u>under</u> <s>strike</s>\n");
  const AVCue& cue = results[0].cues[0];
  EXPECT_EQ("bold italic under strike", cue.text);
  ASSERT_EQ(4u, cue.spans.size());
  EXPECT_TRUE(cue.spans[0].style.bold);
  EXPECT_TRUE(cue.spans[1].style.italic);
  EXPECT_TRUE(cue.spans[2].style.underline);
  EXPECT_TRUE(cue.spans[3].style.strikeout);
}

TEST(AVSubripParserTest, ParsesTheFontColour) {
  const auto results = parseSubrip("1\n"
                                   "00:00:00,000 --> 00:00:02,000\n"
                                   "<font color=\"#FF0000\">red</font>\n");
  const AVCue& cue = results[0].cues[0];
  EXPECT_EQ("red", cue.text);
  ASSERT_EQ(1u, cue.spans.size());
  EXPECT_EQ(AVCueSpan::Type::kForegroundColor, cue.spans[0].type);
  EXPECT_EQ(0xFFFF0000u, cue.spans[0].color);
}

TEST(AVSubripParserTest, ParsesTheNamedFontColour) {
  const auto results = parseSubrip("1\n"
                                   "00:00:00,000 --> 00:00:02,000\n"
                                   "<font color=blue>blue</font>\n");
  ASSERT_EQ(1u, results[0].cues[0].spans.size());
  EXPECT_EQ(0xFF0000FFu, results[0].cues[0].spans[0].color);
}

TEST(AVSubripParserTest, ParsesTheFontFace) {
  const auto results = parseSubrip("1\n"
                                   "00:00:00,000 --> 00:00:02,000\n"
                                   "<font face=\"Arial\">text</font>\n");
  ASSERT_EQ(1u, results.size());
  ASSERT_EQ("text", results[0].cues[0].text);
  ASSERT_EQ(1u, results[0].cues[0].spans.size());
  const AVCueSpan& span = results[0].cues[0].spans[0];
  EXPECT_EQ(AVCueSpan::Type::kTextStyle, span.type);
  EXPECT_EQ("Arial", span.style.fontFamily);
  EXPECT_EQ(0u, span.start);
  EXPECT_EQ(4u, span.end);
}

TEST(AVSubripParserTest, ParsesSubscriptAndSuperscript) {
  const auto results = parseSubrip("1\n"
                                   "00:00:00,000 --> 00:00:02,000\n"
                                   "<sub>sub</sub> and <sup>sup</sup>\n");
  ASSERT_EQ(1u, results.size());
  const AVCue& cue = results[0].cues[0];
  ASSERT_EQ("sub and sup", cue.text);
  ASSERT_EQ(2u, cue.spans.size());
  EXPECT_EQ(AVCueSpan::Type::kSubscript, cue.spans[0].type);
  EXPECT_EQ(0u, cue.spans[0].start);
  EXPECT_EQ(3u, cue.spans[0].end);
  EXPECT_EQ(AVCueSpan::Type::kSuperscript, cue.spans[1].type);
  EXPECT_EQ(8u, cue.spans[1].start);
  EXPECT_EQ(11u, cue.spans[1].end);
}

TEST(AVSubripParserTest, ParsesBigSmallAndMonospace) {
  const auto results = parseSubrip("1\n"
                                   "00:00:00,000 --> 00:00:02,000\n"
                                   "<big>B</big><small>s</small><tt>t</tt>\n");
  ASSERT_EQ(1u, results.size());
  const AVCue& cue = results[0].cues[0];
  ASSERT_EQ("Bst", cue.text);
  ASSERT_EQ(3u, cue.spans.size());
  EXPECT_FLOAT_EQ(1.25f, cue.spans[0].style.textSize);
  EXPECT_FALSE(cue.spans[0].style.textSizeAbsolute);
  EXPECT_FLOAT_EQ(0.8f, cue.spans[1].style.textSize);
  EXPECT_EQ("monospace", cue.spans[2].style.fontFamily);
}

TEST(AVSubripParserTest, ParsesHeadingsAndBlocks) {
  const auto results = parseSubrip("1\n"
                                   "00:00:00,000 --> 00:00:02,000\n"
                                   "<h1>Title</h1><p>Body</p>\n");
  ASSERT_EQ(1u, results.size());
  const AVCue& cue = results[0].cues[0];
  // A paragraph break is at most one blank line, as in the Android HTML parser.
  EXPECT_EQ("Title\n\nBody\n\n", cue.text);
  ASSERT_EQ(1u, cue.spans.size());
  EXPECT_EQ(0u, cue.spans[0].start);
  EXPECT_EQ(5u, cue.spans[0].end);
  EXPECT_TRUE(cue.spans[0].style.bold);
  EXPECT_FLOAT_EQ(1.5f, cue.spans[0].style.textSize);
}

TEST(AVSubripParserTest, AppliesABlockquoteIndent) {
  const auto results = parseSubrip("1\n"
                                   "00:00:00,000 --> 00:00:02,000\n"
                                   "<blockquote>Quote</blockquote>\n"
                                   "\n"
                                   "2\n"
                                   "00:00:02,000 --> 00:00:04,000\n"
                                   "<blockquote><blockquote>Deep</blockquote></blockquote>\n");
  ASSERT_EQ(2u, results.size());
  EXPECT_FLOAT_EQ(0.05f, results[0].cues[0].paragraphIndent);
  EXPECT_FLOAT_EQ(0.10f, results[1].cues[0].paragraphIndent);
}

TEST(AVSubripParserTest, ParsesTheLegacyAlignmentTag) {
  const auto results = parseSubrip("1\n"
                                   "00:00:00,000 --> 00:00:02,000\n"
                                   "{\\an8}Top middle\n");
  const AVCue& cue = results[0].cues[0];
  EXPECT_EQ("Top middle", cue.text);
  EXPECT_EQ(AVCue::AnchorType::kMiddle, cue.positionAnchor);
  EXPECT_EQ(AVCue::AnchorType::kStart, cue.lineAnchor);
  EXPECT_FLOAT_EQ(0.5f, cue.position);
  EXPECT_FLOAT_EQ(0.08f, cue.line);
  EXPECT_EQ(AVCue::LineType::kFraction, cue.lineType);
}

TEST(AVSubripParserTest, ParsesTheEntities) {
  const auto results = parseSubrip("1\n"
                                   "00:00:00,000 --> 00:00:02,000\n"
                                   "&amp; &lt; &gt; &quot; &#65;\n");
  EXPECT_EQ("& < > \" A", results[0].cues[0].text);
}

TEST(AVSubripParserTest, DecodesAUtf16Document) {
  const std::string text = "1\n00:00:00,000 --> 00:00:01,000\nHello\n";
  std::vector<uint8_t> document = {0xFF, 0xFE};
  for (const char character : text) {
    document.push_back(static_cast<uint8_t>(character));
    document.push_back(0);
  }
  AVSubripParser parser;
  std::vector<AVCuesWithTiming> results;
  ASSERT_TRUE(parser.parse(document.data(), document.size(),
                           AVSubtitleParser::OutputOptions::allCues(),
                           [&results](const AVCuesWithTiming& cues) {
                             results.push_back(cues);
                           }));
  ASSERT_EQ(1u, results.size());
  EXPECT_EQ("Hello", results[0].cues[0].text);
}

TEST(AVSubripParserTest, SkipsACueWithAnInvalidTiming) {
  const auto results = parseSubrip("1\n"
                                   "nonsense\n"
                                   "Broken\n"
                                   "\n"
                                   "2\n"
                                   "00:00:01,000 --> 00:00:02,000\n"
                                   "Valid\n");
  ASSERT_EQ(1u, results.size());
  EXPECT_EQ("Valid", results[0].cues[0].text);
}

TEST(AVSubripParserTest, EmitsTheCuesAfterTheRequestedStartFirst) {
  const std::string document = "1\n"
                               "00:00:00,000 --> 00:00:01,000\n"
                               "First\n"
                               "\n"
                               "2\n"
                               "00:00:02,000 --> 00:00:03,000\n"
                               "Second\n";
  const auto afterOnly = parseSubrip(document, AVSubtitleParser::OutputOptions::onlyCuesAfter(1500000));
  ASSERT_EQ(1u, afterOnly.size());
  EXPECT_EQ("Second", afterOnly[0].cues[0].text);

  const auto allCues =
    parseSubrip(document, AVSubtitleParser::OutputOptions::cuesAfterThenRemainingCuesBefore(1500000));
  ASSERT_EQ(2u, allCues.size());
  EXPECT_EQ("Second", allCues[0].cues[0].text);
  EXPECT_EQ("First", allCues[1].cues[0].text);
}
