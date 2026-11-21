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
#include <extractor/text/ttml/TtmlParser.h>

#include <gtest/gtest.h>

#include <fstream>
#include <iterator>

#include <string>
#include <vector>

using coral::AVCue;
using coral::AVCueSpan;
using coral::AVCuesWithTiming;
using coral::AVSubtitleParser;
using coral::AVTtmlParser;

namespace {

std::string readFixture(const std::string& name) {
  const std::string path = std::string(CORAL_ASSETS_DIR) + "/subtitle/ttml/" + name;
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input), {});
}

std::vector<AVCuesWithTiming> parseTtml(const std::string& document) {
  AVTtmlParser parser;
  std::vector<AVCuesWithTiming> results;
  EXPECT_TRUE(parser.parse(reinterpret_cast<const uint8_t*>(document.data()), document.size(),
    AVSubtitleParser::OutputOptions::allCues(),
    [&results](const AVCuesWithTiming& cues) { results.push_back(cues); }));
  return results;
}

std::string wrap(const std::string& body) {
  return "<tt xmlns=\"http://www.w3.org/ns/ttml\"><body><div>" + body + "</div></body></tt>";
}

} // namespace

TEST(AVTtmlParserTest, ParsesAParagraphWithItsTiming) {
  const auto results = parseTtml(wrap(
    "<p begin=\"00:00:01.000\" end=\"00:00:03.000\">Hello Coral</p>"));
  ASSERT_EQ(1u, results.size());
  EXPECT_EQ(1000000, results[0].startTimeUs);
  EXPECT_EQ(2000000, results[0].durationUs);
  ASSERT_EQ(1u, results[0].cues.size());
  EXPECT_EQ("Hello Coral", results[0].cues[0].text);
}

TEST(AVTtmlParserTest, ParsesOffsetTimesAndLineBreaks) {
  const auto results = parseTtml(wrap(
    "<p begin=\"1s\" end=\"2.5s\">First<br/>Second</p>"));
  ASSERT_EQ(1u, results.size());
  EXPECT_EQ(1000000, results[0].startTimeUs);
  EXPECT_EQ(1500000, results[0].durationUs);
  EXPECT_EQ("First\nSecond", results[0].cues[0].text);
}

TEST(AVTtmlParserTest, ParsesInlineColorAndWeight) {
  const auto results = parseTtml(wrap(
    "<p begin=\"0s\" end=\"2s\">Hello "
    "<span tts:color=\"#FF0000\" tts:fontWeight=\"bold\">Coral</span></p>"));
  ASSERT_EQ(1u, results.size());
  const AVCue& cue = results[0].cues[0];
  EXPECT_EQ("Hello Coral", cue.text);
  bool sawColor = false;
  bool sawBold = false;
  for (const AVCueSpan& span: cue.spans) {
    if (span.type == AVCueSpan::Type::kForegroundColor) {
      sawColor = true;
      EXPECT_EQ(0xFFFF0000u, span.color);
      EXPECT_EQ(6u, span.start);
      EXPECT_EQ(11u, span.end);
    } else if (span.type == AVCueSpan::Type::kTextStyle) {
      sawBold = span.style.bold;
    }
  }
  EXPECT_TRUE(sawColor);
  EXPECT_TRUE(sawBold);
}

TEST(AVTtmlParserTest, DecodesEntities) {
  const auto results = parseTtml(wrap(
    "<p begin=\"0s\" end=\"2s\">A &amp; B &lt; C</p>"));
  ASSERT_EQ(1u, results.size());
  EXPECT_EQ("A & B < C", results[0].cues[0].text);
}
TEST(AVTtmlParserTest, AppliesRegionOriginAndExtent) {
  const auto results = parseTtml(readFixture("multiple_regions.xml"));
  ASSERT_GE(results.size(), 3u);
  EXPECT_EQ("lorem", results[0].cues[0].text);
  EXPECT_FLOAT_EQ(0.1f, results[0].cues[0].position);
  EXPECT_FLOAT_EQ(0.1f, results[0].cues[0].line);
  EXPECT_FLOAT_EQ(0.2f, results[0].cues[0].size);

  bool foundUltimate = false;
  for (const auto& result: results) {
    if (result.cues[0].text == "They first said this") {
      foundUltimate = true;
      EXPECT_FLOAT_EQ(0.45f, result.cues[0].position);
      EXPECT_FLOAT_EQ(0.45f, result.cues[0].line);
      EXPECT_FLOAT_EQ(0.35f, result.cues[0].size);
    }
  }
  EXPECT_TRUE(foundUltimate);
}

TEST(AVTtmlParserTest, AppliesTextAlignment) {
  const auto results = parseTtml(readFixture("text_align.xml"));
  ASSERT_GE(results.size(), 3u);
  EXPECT_EQ(AVCue::Alignment::kLeft, results[0].cues[0].textAlignment);
  EXPECT_EQ(AVCue::Alignment::kCenter, results[2].cues[0].textAlignment);
}
TEST(AVTtmlParserTest, ParsesASimpleRubyContainer) {
  const auto simple = parseTtml(wrap("<p begin=\"0s\" end=\"2s\">Cue <span tts:ruby=\"container\"><span tts:ruby=\"base\">annotated</span><span tts:ruby=\"text\">ruby</span></span></p>"));
  ASSERT_FALSE(simple.empty());
}

TEST(AVTtmlParserTest, ParsesARubyContainer) {
  const auto results = parseTtml(readFixture("rubies.xml"));
  ASSERT_FALSE(results.empty());
  const auto& cue = results[0].cues[0];
  EXPECT_EQ("Cue with annotated text.", cue.text);
  bool found = false;
  for (const auto& span: cue.spans) {
    if (span.type == AVCueSpan::Type::kRuby && span.ruby.text == "1st rubies") {
      found = true;
      EXPECT_EQ(AVCueSpan::RubyType::kBase, span.ruby.type);
      EXPECT_EQ(AVCueSpan::RubyPosition::kBefore, span.ruby.position);
    }
  }
  EXPECT_TRUE(found) << "the first ruby annotation was not attached to the cue";
}

TEST(AVTtmlParserTest, AppliesFontSizeTextCombineAndEmphasis) {
  const auto results = parseTtml(wrap(
    "<p begin=\"0s\" end=\"2s\">"
    "<span tts:fontSize=\"150%\">big</span> "
    "<span tts:textCombine=\"all\">12</span> "
    "<span tts:textEmphasis=\"filled circle before\">em</span></p>"));
  ASSERT_EQ(1u, results.size());
  const AVCue& cue = results[0].cues[0];
  ASSERT_EQ("big 12 em", cue.text);

  bool hasFontSize = false;
  bool hasCombine = false;
  bool hasEmphasis = false;
  for (const auto& span: cue.spans) {
    if (span.type == AVCueSpan::Type::kTextStyle && span.start == 0 && span.end == 3) {
      hasFontSize = !span.style.textSizeAbsolute && span.style.textSize == 1.5f;
    }
    if (span.type == AVCueSpan::Type::kHorizontalTextInVerticalContext &&
        span.start == 4 && span.end == 6) {
      hasCombine = true;
    }
    if (span.type == AVCueSpan::Type::kTextEmphasis && span.start == 7 && span.end == 9) {
      hasEmphasis = span.emphasis.mark == AVCueSpan::TextEmphasisMark::kCircle &&
                    span.emphasis.mode == AVCueSpan::TextEmphasisMode::kBefore;
    }
  }
  EXPECT_TRUE(hasFontSize) << "tts:fontSize was not applied to the span";
  EXPECT_TRUE(hasCombine) << "tts:textCombine did not mark its range";
  EXPECT_TRUE(hasEmphasis) << "tts:textEmphasis was not applied to the span";
}

TEST(AVTtmlParserTest, AppliesShearAndMultiRowAlign) {
  const auto results = parseTtml(wrap(
    "<p begin=\"0s\" end=\"2s\" tts:shear=\"20%\" ebutts:multiRowAlign=\"center\" "
    "tts:fontSize=\"20px\">Hi</p>"));
  ASSERT_EQ(1u, results.size());
  const AVCue& cue = results[0].cues[0];
  EXPECT_FLOAT_EQ(-18.0f, cue.shearDegrees);
  EXPECT_EQ(AVCue::Alignment::kCenter, cue.multiRowAlignment);
  bool hasAbsoluteSize = false;
  for (const auto& span: cue.spans) {
    if (span.type == AVCueSpan::Type::kTextStyle && span.style.textSizeAbsolute &&
        span.style.textSize == 20.0f) {
      hasAbsoluteSize = true;
    }
  }
  EXPECT_TRUE(hasAbsoluteSize) << "a pixel font size must be absolute";
}

TEST(AVTtmlParserTest, AppliesTtsMultiRowAlign) {
  const auto results = parseTtml(wrap(
    "<p begin=\"0s\" end=\"2s\" tts:multiRowAlign=\"right\">Hi</p>"));
  ASSERT_EQ(1u, results.size());
  EXPECT_EQ(AVCue::Alignment::kRight, results[0].cues[0].multiRowAlignment);
}

TEST(AVTtmlParserTest, InheritsTimingFromTheDiv) {
  const auto results = parseTtml(wrap(
    "<div begin=\"1s\" end=\"3s\"><p>Inside</p></div>"
    "<div begin=\"10s\"><p begin=\"0.5s\" dur=\"2s\">Nested</p></div>"));
  ASSERT_EQ(2u, results.size());
  EXPECT_EQ(1000000, results[0].startTimeUs);
  EXPECT_EQ(2000000, results[0].durationUs);
  EXPECT_EQ(10500000, results[1].startTimeUs);
  EXPECT_EQ(2000000, results[1].durationUs);
}

TEST(AVTtmlParserTest, ParsesAnEmbeddedImage) {
  const std::string image =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=";
  const std::string document =
    "<tt xmlns=\"http://www.w3.org/ns/ttml\" xmlns:tts=\"http://www.w3.org/ns/ttml#styling\">"
    "<head><layout><region xml:id=\"r\" tts:origin=\"10% 20%\" tts:extent=\"30% 40%\"/></layout>"
    "<metadata><image xml:id=\"img\">" + image + "</image></metadata></head>"
    "<body><div begin=\"1s\" end=\"2s\" backgroundImage=\"#img\" region=\"r\"/></body></tt>";
  const auto results = parseTtml(document);
  ASSERT_EQ(1u, results.size());
  const AVCue& cue = results[0].cues[0];
  EXPECT_TRUE(cue.isBitmap());
  EXPECT_FALSE(cue.bitmap.encoded.empty());
  EXPECT_FLOAT_EQ(0.1f, cue.position);
  EXPECT_FLOAT_EQ(0.2f, cue.line);
  EXPECT_FLOAT_EQ(0.3f, cue.size);
  EXPECT_FLOAT_EQ(0.4f, cue.bitmapHeight);
  EXPECT_EQ(1000000, results[0].startTimeUs);
  EXPECT_EQ(1000000, results[0].durationUs);
}
