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
#include <extractor/text/CueTimeline.h>
#include <extractor/text/CuesWithTiming.h>
#include <extractor/text/SubtitleParser.h>
#include <extractor/text/webvtt/Mp4WebvttParser.h>
#include <extractor/text/webvtt/WebvttParser.h>

#include <media/Format.h>
#include <media/Types.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

using coral::AVCue;
using coral::AVCueSpan;
using coral::AVCueTimeline;
using coral::AVCuesWithTiming;
using coral::AVMp4WebvttParser;
using coral::AVSubtitleParser;
using coral::AVWebvttParser;
using coral::CueReplacementBehavior;
using coral::MediaFormat;

namespace {

std::vector<AVCuesWithTiming>
parseWebvtt(const std::string& document,
            const AVSubtitleParser::OutputOptions& options = AVSubtitleParser::OutputOptions::allCues()) {
  AVWebvttParser parser;
  std::vector<AVCuesWithTiming> results;
  const bool parsed = parser.parse(reinterpret_cast<const uint8_t*>(document.data()), document.size(),
                                   options,
                                   [&results](const AVCuesWithTiming& cues) {
                                     results.push_back(cues);
                                   });
  EXPECT_TRUE(parsed);
  return results;
}

std::string textOf(const AVCuesWithTiming& cues) {
  std::string text;
  for (const auto& cue : cues.cues) {
    if (!text.empty()) {
      text += "|";
    }
    text += cue.text;
  }
  return text;
}

/** Builds an MP4 box. */
std::vector<uint8_t> box(const std::string& type, const std::vector<uint8_t>& payload) {
  std::vector<uint8_t> data;
  const uint32_t size = static_cast<uint32_t>(8 + payload.size());
  data.push_back(static_cast<uint8_t>(size >> 24));
  data.push_back(static_cast<uint8_t>(size >> 16));
  data.push_back(static_cast<uint8_t>(size >> 8));
  data.push_back(static_cast<uint8_t>(size));
  data.insert(data.end(), type.begin(), type.end());
  data.insert(data.end(), payload.begin(), payload.end());
  return data;
}

std::vector<uint8_t> bytes(const std::string& value) {
  return std::vector<uint8_t>(value.begin(), value.end());
}

} // namespace

TEST(AVWebvttParserTest, ParsesACueWithTheDefaultSettings) {
  const auto results = parseWebvtt("WEBVTT\n\n"
                                   "00:00:01.000 --> 00:00:03.500\n"
                                   "Hello world\n");
  ASSERT_EQ(1u, results.size());
  const auto& result = results[0];
  EXPECT_EQ(1000000, result.startTimeUs);
  EXPECT_EQ(2500000, result.durationUs);
  ASSERT_EQ(1u, result.cues.size());
  const AVCue& cue = result.cues[0];
  EXPECT_EQ("Hello world", cue.text);
  EXPECT_EQ(AVCue::Alignment::kCenter, cue.textAlignment);
  EXPECT_FLOAT_EQ(0.5f, cue.position);
  EXPECT_EQ(AVCue::AnchorType::kMiddle, cue.positionAnchor);
  EXPECT_FLOAT_EQ(1.0f, cue.size);
  EXPECT_EQ(AVCue::kDimenUnset, cue.line);
  EXPECT_EQ(AVCue::LineType::kNumber, cue.lineType);
}

TEST(AVWebvttParserTest, SkipsTheHeaderDescriptionAndTheBlocks) {
  const auto results = parseWebvtt("WEBVTT - a description\n"
                                   "Kind: captions\n"
                                   "\n"
                                   "NOTE this is a comment\n"
                                   "that spans two lines\n"
                                   "\n"
                                   "STYLE\n"
                                   "::cue { color: red }\n"
                                   "\n"
                                   "REGION\n"
                                   "id:fred width:40%\n"
                                   "\n"
                                   "00:00:00.000 --> 00:00:01.000\n"
                                   "First\n"
                                   "\n"
                                   "00:00:02.000 --> 00:00:03.000\n"
                                   "Second\n");
  ASSERT_EQ(2u, results.size());
  EXPECT_EQ("First", textOf(results[0]));
  EXPECT_EQ("Second", textOf(results[1]));
}

TEST(AVWebvttParserTest, ParsesACueIdentifier) {
  const auto results = parseWebvtt("WEBVTT\n\n"
                                   "cue-1\n"
                                   "00:00:01.000 --> 00:00:02.000\n"
                                   "Identified\n");
  ASSERT_EQ(1u, results.size());
  EXPECT_EQ("Identified", textOf(results[0]));
  EXPECT_EQ(1000000, results[0].startTimeUs);
}

TEST(AVWebvttParserTest, AppliesTheCueSettings) {
  const auto results = parseWebvtt("WEBVTT\n\n"
                                   "00:00:00.000 --> 00:00:01.000 align:start position:10% size:40% "
                                   "line:20%,center\n"
                                   "Left\n");
  ASSERT_EQ(1u, results.size());
  const AVCue& cue = results[0].cues[0];
  EXPECT_EQ(AVCue::Alignment::kStart, cue.textAlignment);
  EXPECT_FLOAT_EQ(0.1f, cue.position);
  EXPECT_EQ(AVCue::AnchorType::kStart, cue.positionAnchor);
  EXPECT_FLOAT_EQ(0.4f, cue.size);
  EXPECT_FLOAT_EQ(0.2f, cue.line);
  EXPECT_EQ(AVCue::LineType::kFraction, cue.lineType);
  EXPECT_EQ(AVCue::AnchorType::kMiddle, cue.lineAnchor);
}

TEST(AVWebvttParserTest, ClampsTheSizeToTheSpaceLeftOfThePosition) {
  const auto results = parseWebvtt("WEBVTT\n\n"
                                   "00:00:00.000 --> 00:00:01.000 align:end position:80% size:100%\n"
                                   "Right\n");
  const AVCue& cue = results[0].cues[0];
  EXPECT_FLOAT_EQ(0.8f, cue.position);
  EXPECT_EQ(AVCue::AnchorType::kEnd, cue.positionAnchor);
  // The size may not reach past the right edge: the anchor keeps 80% of the viewport.
  EXPECT_FLOAT_EQ(0.8f, cue.size);
}

TEST(AVWebvttParserTest, ParsesTheVerticalSetting) {
  const auto results = parseWebvtt("WEBVTT\n\n"
                                   "00:00:00.000 --> 00:00:01.000 vertical:rl\n"
                                   "Vertical\n");
  EXPECT_EQ(AVCue::VerticalType::kRightToLeft, results[0].cues[0].verticalType);
}

TEST(AVWebvttParserTest, ParsesTheInlineMarkup) {
  const auto results = parseWebvtt("WEBVTT\n\n"
                                   "00:00:00.000 --> 00:00:01.000\n"
                                   "<b>bold</b> and <i>italic</i> and <u>under</u>\n");
  const AVCue& cue = results[0].cues[0];
  EXPECT_EQ("bold and italic and under", cue.text);
  ASSERT_EQ(3u, cue.spans.size());
  EXPECT_EQ(AVCueSpan::Type::kTextStyle, cue.spans[0].type);
  EXPECT_TRUE(cue.spans[0].style.bold);
  EXPECT_EQ(0u, cue.spans[0].start);
  EXPECT_EQ(4u, cue.spans[0].end);
  EXPECT_TRUE(cue.spans[1].style.italic);
  EXPECT_EQ(9u, cue.spans[1].start);
  EXPECT_TRUE(cue.spans[2].style.underline);
}

TEST(AVWebvttParserTest, ParsesTheDefaultColourClasses) {
  const auto results = parseWebvtt("WEBVTT\n\n"
                                   "00:00:00.000 --> 00:00:01.000\n"
                                   "<c.yellow>yellow</c><c.bg_blue>blue</c>\n");
  const AVCue& cue = results[0].cues[0];
  EXPECT_EQ("yellowblue", cue.text);
  ASSERT_EQ(2u, cue.spans.size());
  EXPECT_EQ(AVCueSpan::Type::kForegroundColor, cue.spans[0].type);
  EXPECT_EQ(0xFFFFFF00u, cue.spans[0].color);
  EXPECT_EQ(AVCueSpan::Type::kBackgroundColor, cue.spans[1].type);
  EXPECT_EQ(0xFF0000FFu, cue.spans[1].color);
}

TEST(AVWebvttParserTest, ParsesTheVoiceAndLanguageTags) {
  const auto results = parseWebvtt("WEBVTT\n\n"
                                   "00:00:00.000 --> 00:00:01.000\n"
                                   "<v Anna>Hello</v> <lang fr>Bonjour</lang>\n");
  const AVCue& cue = results[0].cues[0];
  EXPECT_EQ("Hello Bonjour", cue.text);
  ASSERT_EQ(2u, cue.spans.size());
  EXPECT_EQ(AVCueSpan::Type::kVoice, cue.spans[0].type);
  EXPECT_EQ("Anna", cue.spans[0].text);
  EXPECT_EQ(AVCueSpan::Type::kLanguage, cue.spans[1].type);
  EXPECT_EQ("fr", cue.spans[1].text);
}

TEST(AVWebvttParserTest, ParsesTheEntities) {
  const auto results = parseWebvtt("WEBVTT\n\n"
                                   "00:00:00.000 --> 00:00:01.000\n"
                                   "&amp; &lt; &gt; &nbsp;end\n");
  EXPECT_EQ("& < >  end", results[0].cues[0].text);
}

TEST(AVWebvttParserTest, ExtractsTheRubyAnnotation) {
  // The test sources are ASCII on purpose: the compiler of a test target decodes a narrow literal
  // with the code page of the machine, so a multi byte character would make the test depend on it.
  const std::string base = "\xE6\xBC\xA2";       // U+6F22
  const std::string second = "\xE5\xAD\x97";     // U+5B57
  const std::string document = "WEBVTT\n\n"
                               "00:00:00.000 --> 00:00:01.000\n"
                               "<ruby>" +
                               base + "<rt>kan</rt>" + second + "</ruby>\n";
  const auto results = parseWebvtt(document);
  const AVCue& cue = results[0].cues[0];
  EXPECT_EQ(base + second, cue.text);
  ASSERT_EQ(1u, cue.spans.size());
  EXPECT_EQ(AVCueSpan::Type::kRuby, cue.spans[0].type);
  EXPECT_EQ("kan", cue.spans[0].ruby.text);
  EXPECT_EQ(0u, cue.spans[0].start);
  EXPECT_EQ(base.size(), cue.spans[0].end);
}

TEST(AVWebvttParserTest, KeepsTheCuesOfADocumentWithAMalformedOne) {
  const auto results = parseWebvtt("WEBVTT\n\n"
                                   "00:00:00.000 --> nonsense\n"
                                   "Broken\n"
                                   "\n"
                                   "00:00:01.000 --> 00:00:02.000\n"
                                   "Valid\n");
  ASSERT_EQ(1u, results.size());
  EXPECT_EQ("Valid", textOf(results[0]));
}

TEST(AVWebvttParserTest, RejectsADocumentWithoutTheHeader) {
  AVWebvttParser parser;
  const std::string document = "00:00:00.000 --> 00:00:01.000\nNo header\n";
  EXPECT_FALSE(parser.parse(reinterpret_cast<const uint8_t*>(document.data()), document.size(),
                            AVSubtitleParser::OutputOptions::allCues(),
                            [](const AVCuesWithTiming&) {}));
}

TEST(AVWebvttParserTest, EmitsTheCuesAfterTheRequestedStartFirst) {
  const std::string document = "WEBVTT\n\n"
                               "00:00:00.000 --> 00:00:01.000\n"
                               "First\n"
                               "\n"
                               "00:00:02.000 --> 00:00:03.000\n"
                               "Second\n";
  const auto afterOnly = parseWebvtt(document, AVSubtitleParser::OutputOptions::onlyCuesAfter(1500000));
  ASSERT_EQ(1u, afterOnly.size());
  EXPECT_EQ("Second", textOf(afterOnly[0]));

  const auto allCues =
    parseWebvtt(document, AVSubtitleParser::OutputOptions::cuesAfterThenRemainingCuesBefore(1500000));
  ASSERT_EQ(2u, allCues.size());
  EXPECT_EQ("Second", textOf(allCues[0]));
  EXPECT_EQ("First", textOf(allCues[1]));
}

TEST(AVMp4WebvttParserTest, ParsesTheCueBoxesOfASample) {
  std::vector<uint8_t> sample;
  const auto append = [&sample](const std::vector<uint8_t>& value) {
    sample.insert(sample.end(), value.begin(), value.end());
  };
  // One cue with settings and text, and one cue with text only.
  std::vector<uint8_t> vttc1;
  const auto sttg1 = box("sttg", bytes("align:start"));
  const auto payl1 = box("payl", bytes("First"));
  vttc1.insert(vttc1.end(), sttg1.begin(), sttg1.end());
  vttc1.insert(vttc1.end(), payl1.begin(), payl1.end());
  append(box("vttc", vttc1));

  std::vector<uint8_t> vttc2;
  const auto payl2 = box("payl", bytes("Second"));
  vttc2.insert(vttc2.end(), payl2.begin(), payl2.end());
  append(box("vttc", vttc2));

  AVMp4WebvttParser parser;
  std::vector<AVCuesWithTiming> results;
  ASSERT_TRUE(parser.parse(sample.data(), sample.size(),
                           AVSubtitleParser::OutputOptions::allCues(),
                           [&results](const AVCuesWithTiming& cues) {
                             results.push_back(cues);
                           }));
  ASSERT_EQ(1u, results.size());
  EXPECT_EQ(coral::Definitions::kTimeUnset, results[0].startTimeUs);
  ASSERT_EQ(2u, results[0].cues.size());
  EXPECT_EQ("First", results[0].cues[0].text);
  EXPECT_EQ(AVCue::Alignment::kStart, results[0].cues[0].textAlignment);
  EXPECT_EQ("Second", results[0].cues[1].text);
}

TEST(AVWebvttParserTest, FeedsTheCueTimelineOfAPlayer) {
  const auto results = parseWebvtt("WEBVTT\n\n"
                                   "00:00:01.000 --> 00:00:02.000\n"
                                   "Hello\n");
  ASSERT_EQ(1u, results.size());

  // A WebVTT document states its own times; the player maps them onto the item with a sample at
  // time zero and a sample relative offset.
  AVCueTimeline timeline;
  timeline.add(results[0], 0, coral::Definitions::kTimeUnset, CueReplacementBehavior::kMerge,
               MediaFormat::OFFSET_SAMPLE_RELATIVE);
  EXPECT_TRUE(timeline.cuesAt(999999).isEmpty());
  EXPECT_EQ("Hello", timeline.cuesAt(1000000).getCues()[0].text);
  EXPECT_TRUE(timeline.cuesAt(2000000).isEmpty());
}

TEST(AVMp4WebvttParserTest, UsesTheTimeOfTheSampleForItsCues) {
  const auto payl = box("payl", bytes("Sample cue"));
  const auto sample = box("vttc", payl);

  AVMp4WebvttParser parser;
  std::vector<AVCuesWithTiming> results;
  ASSERT_TRUE(parser.parse(sample.data(), sample.size(),
                           AVSubtitleParser::OutputOptions::allCues(),
                           [&results](const AVCuesWithTiming& cues) {
                             results.push_back(cues);
                           }));
  ASSERT_EQ(1u, results.size());

  AVCueTimeline timeline;
  timeline.add(results[0], 5000000, 6000000, CueReplacementBehavior::kReplace,
               MediaFormat::OFFSET_SAMPLE_RELATIVE);
  EXPECT_TRUE(timeline.cuesAt(4999999).isEmpty());
  EXPECT_EQ("Sample cue", timeline.cuesAt(5000000).getCues()[0].text);
  EXPECT_TRUE(timeline.cuesAt(6000000).isEmpty());
}

TEST(AVMp4WebvttParserTest, EmitsAnEmptyCueSetForASampleWithoutACue) {
  // `vtte` is the box of an empty cue: the sample still replaces the cues of the previous one.
  const auto sample = box("vtte", {});
  AVMp4WebvttParser parser;
  std::vector<AVCuesWithTiming> results;
  ASSERT_TRUE(parser.parse(sample.data(), sample.size(),
                           AVSubtitleParser::OutputOptions::allCues(),
                           [&results](const AVCuesWithTiming& cues) {
                             results.push_back(cues);
                           }));
  ASSERT_EQ(1u, results.size());
  EXPECT_TRUE(results[0].cues.empty());
}

TEST(AVMp4WebvttParserTest, RejectsATruncatedBox) {
  const uint8_t sample[] = {0x00, 0x00, 0x00, 0x10, 'v', 't', 't', 'c'};
  AVMp4WebvttParser parser;
  EXPECT_FALSE(parser.parse(sample, sizeof(sample), AVSubtitleParser::OutputOptions::allCues(),
                            [](const AVCuesWithTiming&) {}));
}
TEST(AVWebvttParserTest, AppliesTheCueStyleSheet) {
  const std::string path = std::string(CORAL_ASSETS_DIR) + "/subtitle/webvtt_css_styles";
  std::ifstream input(path, std::ios::binary);
  const std::string document(std::istreambuf_iterator<char>(input), {});
  ASSERT_FALSE(document.empty());
  const auto results = parseWebvtt(document);
  ASSERT_GE(results.size(), 2u);
  EXPECT_EQ("This is the first subtitle.", results[0].cues[0].text);
  bool hasForeground = false;
  bool hasBackground = false;
  for (const auto& span: results[0].cues[0].spans) {
    if (span.type == AVCueSpan::Type::kForegroundColor && span.color == 0xFFFFEFD5u) {
      hasForeground = true;
    }
    if (span.type == AVCueSpan::Type::kBackgroundColor && span.color == 0xFF008000u) {
      hasBackground = true;
    }
  }
  EXPECT_TRUE(hasForeground) << "the global ::cue colour was not applied";
  EXPECT_TRUE(hasBackground) << "the global ::cue background was not applied";

  bool hasPeach = false;
  for (const auto& span: results[1].cues[0].spans) {
    if (span.type == AVCueSpan::Type::kForegroundColor && span.color == 0xFFFFDAB9u) {
      hasPeach = true;
    }
  }
  EXPECT_TRUE(hasPeach) << "the id selector colour was not applied";
}
TEST(AVWebvttParserTest, AppliesTheCssSelectorsToTheMatchingElements) {
  const auto results = parseWebvtt("WEBVTT\n\n"
                                   "STYLE\n"
                                   "::cue(.hot) { color: red; font-size: 150% }\n"
                                   "::cue(b) { color: lime }\n"
                                   "::cue(#title) { background-color: blue }\n"
                                   "\n"
                                   "title\n"
                                   "00:00.000 --> 00:02.000\n"
                                   "<c.hot>Hot</c> <b>Bold</b>\n");
  ASSERT_EQ(1u, results.size());
  const AVCue& cue = results[0].cues[0];
  ASSERT_EQ("Hot Bold", cue.text);

  bool hotColor = false;
  bool hotSize = false;
  bool boldColor = false;
  bool background = false;
  for (const auto& span: cue.spans) {
    if (span.type == AVCueSpan::Type::kForegroundColor && span.color == 0xFFFF0000u &&
        span.start == 0 && span.end == 3) {
      hotColor = true;
    }
    if (span.type == AVCueSpan::Type::kTextStyle && span.start == 0 && span.end == 3 &&
        span.style.textSize == 1.5f && !span.style.textSizeAbsolute) {
      hotSize = true;
    }
    if (span.type == AVCueSpan::Type::kForegroundColor && span.color == 0xFF00FF00u &&
        span.start == 4 && span.end == 8) {
      boldColor = true;
    }
    if (span.type == AVCueSpan::Type::kBackgroundColor && span.color == 0xFF0000FFu &&
        span.start == 0 && span.end == 8) {
      background = true;
    }
  }
  EXPECT_TRUE(hotColor) << "the class selector did not colour its element";
  EXPECT_TRUE(hotSize) << "the class selector did not apply its font-size";
  EXPECT_TRUE(boldColor) << "the tag selector did not colour the <b> element";
  EXPECT_TRUE(background) << "the cue id selector did not style the whole cue";
}
TEST(AVWebvttParserTest, AppliesTheCssSelectorSpecificity) {
  const auto results = parseWebvtt("WEBVTT\n\n"
                                   "STYLE\n"
                                   "::cue { color: white }\n"
                                   "::cue(.hot) { color: red }\n"
                                   "::cue(#title) { color: lime }\n"
                                   "\n"
                                   "title\n"
                                   "00:00.000 --> 00:01.000\n"
                                   "<c.hot>Hot</c>\n");
  ASSERT_EQ(1u, results.size());
  const AVCue& cue = results[0].cues[0];
  ASSERT_EQ("Hot", cue.text);

  // The id rule is the most specific one, so the first covering colour is its lime.
  uint32_t firstColor = 0;
  bool found = false;
  for (const auto& span: cue.spans) {
    if (span.type == AVCueSpan::Type::kForegroundColor && span.start == 0 && span.end == 3) {
      firstColor = span.color;
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found);
  EXPECT_EQ(0xFF00FF00u, firstColor) << "the id selector must win over the class selector";
}
TEST(AVWebvttParserTest, AppliesTheCssRubyPositionAndCombineUpright) {
  const auto results = parseWebvtt("WEBVTT\n\n"
                                   "STYLE\n"
                                   "::cue(rt) { ruby-position: under }\n"
                                   "::cue(.upright) { text-combine-upright: all }\n"
                                   "\n"
                                   "00:00.000 --> 00:02.000\n"
                                   "<ruby>base<rt>anno</rt></ruby> <c.upright>12</c>\n");
  ASSERT_EQ(1u, results.size());
  const AVCue& cue = results[0].cues[0];
  ASSERT_EQ("base 12", cue.text);

  bool rubyUnder = false;
  bool combineUpright = false;
  for (const auto& span: cue.spans) {
    if (span.type == AVCueSpan::Type::kRuby && span.start == 0 && span.end == 4) {
      rubyUnder = span.ruby.position == AVCueSpan::RubyPosition::kAfter;
    }
    if (span.type == AVCueSpan::Type::kHorizontalTextInVerticalContext) {
      combineUpright = span.start == 5 && span.end == 7;
    }
  }
  EXPECT_TRUE(rubyUnder) << "the rt selector did not move the annotation under the base";
  EXPECT_TRUE(combineUpright) << "text-combine-upright did not mark its range";
}
TEST(AVWebvttParserTest, AppliesARegionToItsCue) {
  const std::string document =
    "WEBVTT\n\n"
    "REGION\n"
    "id:fred\n"
    "width:40%\n"
    "lines:3\n"
    "regionanchor:0%,100%\n"
    "viewportanchor:10%,90%\n"
    "scroll:up\n\n"
    "00:00:00.000 --> 00:00:01.000 region:fred\n"
    "Hello\n";
  const auto results = parseWebvtt(document);
  ASSERT_EQ(1u, results.size());
  ASSERT_EQ(1u, results[0].cues.size());
  const AVCue& cue = results[0].cues[0];
  EXPECT_FLOAT_EQ(0.1f, cue.position);
  EXPECT_EQ(AVCue::AnchorType::kStart, cue.positionAnchor);
  EXPECT_FLOAT_EQ(0.9f, cue.line);
  EXPECT_EQ(3, cue.regionLines);
  EXPECT_EQ(AVCue::LineType::kFraction, cue.lineType);
  EXPECT_EQ(AVCue::AnchorType::kEnd, cue.lineAnchor);
  EXPECT_FLOAT_EQ(0.4f, cue.size);
}

TEST(AVWebvttParserTest, LetsCueSettingsOverrideTheRegion) {
  const std::string document =
    "WEBVTT\n\n"
    "REGION id:fred width:40% viewportanchor:10%,90%\n\n"
    "00:00:00.000 --> 00:00:01.000 region:fred position:20%\n"
    "Hello\n";
  const auto results = parseWebvtt(document);
  ASSERT_EQ(1u, results.size());
  ASSERT_EQ(1u, results[0].cues.size());
  const AVCue& cue = results[0].cues[0];
  EXPECT_FLOAT_EQ(0.2f, cue.position);
}

TEST(AVWebvttParserTest, AppliesTheCssVoiceSelectorAndElementRule) {
  const auto results = parseWebvtt("WEBVTT\n\n"
                                   "STYLE\n"
                                   "::cue(v[voice=\"Frog\"]) { color: cyan; font-weight: bold }\n"
                                   "::cue(v) { text-decoration: underline }\n"
                                   "\n"
                                   "00:00.000 --> 00:02.000\n"
                                   "<v Frog>Hi</v>\n");
  ASSERT_EQ(1u, results.size());
  const AVCue& cue = results[0].cues[0];
  ASSERT_EQ("Hi", cue.text);

  bool voiceColor = false;
  bool bold = false;
  bool underline = false;
  for (const auto& span: cue.spans) {
    if (span.type == AVCueSpan::Type::kForegroundColor && span.color == 0xFF00FFFFu &&
        span.start == 0 && span.end == 2) {
      voiceColor = true;
    }
    if (span.type == AVCueSpan::Type::kTextStyle && span.start == 0 && span.end == 2 &&
        span.style.bold) {
      bold = true;
    }
    if (span.type == AVCueSpan::Type::kTextStyle && span.start == 0 && span.end == 2 &&
        span.style.underline) {
      underline = true;
    }
  }
  EXPECT_TRUE(voiceColor) << "the voice selector did not colour its element";
  EXPECT_TRUE(bold) << "the voice selector did not apply its weight";
  EXPECT_TRUE(underline) << "the element selector did not apply its decoration";
}
