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
#include <extractor/text/cea708/Cea708Parser.h>
#include <extractor/text/cea708/Cea708Sei.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

using coral::AVCue;
using coral::AVCueSpan;
using coral::AVCuesWithTiming;
using coral::AVCea708Parser;
using coral::AVSubtitleParser;

namespace {

/** The six data bytes of a visible DefineWindow command. */
std::vector<uint8_t> defineWindow() {
  // visible, priority 0, relative off, vertical anchor 74, horizontal anchor 104,
  // anchor id 4 (middle/middle), one row.
  return {0x98, 0x20, 0x4A, 0x68, 0x40, 0x00, 0x00};
}

/** Wraps DTVCC commands into the 3-byte cc_data triplets a CEA-708 sample carries. */
std::string ccData(const std::vector<uint8_t>& commands, const int serviceNumber = 1) {
  std::vector<uint8_t> packet;
  packet.push_back(static_cast<uint8_t>((serviceNumber << 5) | (commands.size() & 0x1F)));
  packet.insert(packet.end(), commands.begin(), commands.end());
  // The DTVCC packet length is odd: the start triplet carries one byte and every data triplet two.
  if (packet.size() % 2 == 0) {
    packet.push_back(0);
  }
  const int packetSize = static_cast<int>((packet.size() + 1) / 2);
  std::string result;
  result.push_back(static_cast<char>(0x07));  // start + valid
  result.push_back(static_cast<char>(packetSize));
  result.push_back(static_cast<char>(packet[0]));
  for (size_t i = 1; i + 1 < packet.size(); i += 2) {
    result.push_back(static_cast<char>(0x06));  // data + valid
    result.push_back(static_cast<char>(packet[i]));
    result.push_back(static_cast<char>(packet[i + 1]));
  }
  return result;
}

std::vector<AVCuesWithTiming> parseCea708(const std::string& data) {
  AVCea708Parser parser;
  std::vector<AVCuesWithTiming> results;
  EXPECT_TRUE(parser.parse(reinterpret_cast<const uint8_t*>(data.data()), data.size(),
    AVSubtitleParser::OutputOptions::allCues(),
    [&results](const AVCuesWithTiming& cues) { results.push_back(cues); }));
  return results;
}

std::vector<uint8_t> textCommands(const std::string& text) {
  std::vector<uint8_t> commands = defineWindow();
  commands.insert(commands.end(), text.begin(), text.end());
  commands.push_back(0x03);  // ETX
  return commands;
}

/** Wraps cc_data in the ATSC user_data_registered_itu_t_t35 SEI payload. */
std::vector<uint8_t> seiNal(const std::vector<uint8_t>& cc, const bool hevc = false) {
  std::vector<uint8_t> payload = {0xB5, 0x00, 0x31, 'G', 'A', '9', '4', 0x03,
    static_cast<uint8_t>(0x40 | (cc.size() / 3)), 0x00};
  payload.insert(payload.end(), cc.begin(), cc.end());
  std::vector<uint8_t> nal;
  if (hevc) {
    nal.push_back(0x00);  // H.265 NAL header (two bytes)
    nal.push_back(0x01);
  } else {
    nal.push_back(0x06);  // H.264 SEI
  }
  nal.push_back(0x04);  // payload type: user data
  nal.push_back(static_cast<uint8_t>(payload.size()));
  nal.insert(nal.end(), payload.begin(), payload.end());
  nal.push_back(0x80);  // rbsp trailing bits
  return nal;
}

} // namespace

TEST(AVCea708ParserTest, ParsesADefineWindowAndText) {
  const auto results = parseCea708(ccData(textCommands("Hello")));
  ASSERT_EQ(1u, results.size());
  ASSERT_EQ(1u, results[0].cues.size());
  const AVCue& cue = results[0].cues[0];
  EXPECT_EQ("Hello", cue.text);
  EXPECT_EQ(AVCue::Alignment::kLeft, cue.textAlignment);
  EXPECT_FLOAT_EQ(0.95f, cue.line);
  EXPECT_GT(cue.position, 0.4f);
  EXPECT_LT(cue.position, 0.6f);
  EXPECT_EQ(AVCue::AnchorType::kMiddle, cue.positionAnchor);
  EXPECT_EQ(AVCue::AnchorType::kMiddle, cue.lineAnchor);
}

TEST(AVCea708ParserTest, AppliesPenAttributes) {
  std::vector<uint8_t> commands = defineWindow();
  commands.push_back(0x90);  // SetPenAttributes
  commands.push_back(0x00);
  commands.push_back(0xC0);  // italic + underline
  commands.insert(commands.end(), {'H', 'i'});
  commands.push_back(0x03);
  const auto results = parseCea708(ccData(commands));
  ASSERT_EQ(1u, results.size());
  ASSERT_EQ(1u, results[0].cues.size());
  const AVCue& cue = results[0].cues[0];
  ASSERT_EQ("Hi", cue.text);
  bool styled = false;
  for (const auto& span: cue.spans) {
    if (span.type == AVCueSpan::Type::kTextStyle && span.start == 0 && span.end == 2) {
      styled = span.style.italic && span.style.underline;
    }
  }
  EXPECT_TRUE(styled) << "the pen italics/underline were not applied to the text";
}

TEST(AVCea708ParserTest, AppliesPenColour) {
  std::vector<uint8_t> commands = defineWindow();
  commands.push_back(0x91);  // SetPenColor
  commands.push_back(0x30);  // red, opaque
  commands.push_back(0x00);  // black background
  commands.push_back(0x00);  // edge
  commands.insert(commands.end(), {'R', 'e', 'd'});
  commands.push_back(0x03);
  const auto results = parseCea708(ccData(commands));
  ASSERT_EQ(1u, results.size());
  ASSERT_EQ(1u, results[0].cues.size());
  const AVCue& cue = results[0].cues[0];
  ASSERT_EQ("Red", cue.text);
  bool coloured = false;
  for (const auto& span: cue.spans) {
    if (span.type == AVCueSpan::Type::kForegroundColor && span.start == 0 && span.end == 3) {
      coloured = span.color == 0xFFFF0000u;
    }
  }
  EXPECT_TRUE(coloured) << "the pen foreground colour was not applied";
}

TEST(AVCea708ParserTest, DecodesAnExtendedCharacter) {
  std::vector<uint8_t> commands = defineWindow();
  commands.push_back('A');
  commands.push_back(0x10);  // EXT1
  commands.push_back(0x25);  // ellipsis in the G2 table
  commands.push_back('B');
  commands.push_back(0x03);
  const auto results = parseCea708(ccData(commands));
  ASSERT_EQ(1u, results.size());
  ASSERT_EQ(1u, results[0].cues.size());
  EXPECT_EQ("A\xE2\x80\xA6" "B", results[0].cues[0].text);
}

TEST(AVCea708ParserTest, ExtractsCcDataFromASeiNal) {
  const std::string cc = ccData(textCommands("Hi"));
  const std::vector<uint8_t> expected(cc.begin(), cc.end());
  const auto h264 = seiNal(expected);
  EXPECT_EQ(expected, coral::text::extractCea708CcDataFromSei(h264.data(), h264.size(), false));
  const auto h265 = seiNal(expected, true);
  EXPECT_EQ(expected, coral::text::extractCea708CcDataFromSei(h265.data(), h265.size(), true));
}

TEST(AVCea708ParserTest, ExtractsCcDataFromAnAnnexBStream) {
  const std::string cc = ccData(textCommands("Hi"));
  const std::vector<uint8_t> expected(cc.begin(), cc.end());
  std::vector<uint8_t> annexB = {0x00, 0x00, 0x00, 0x01};
  const auto sei = seiNal(expected);
  annexB.insert(annexB.end(), sei.begin(), sei.end());
  annexB.insert(annexB.end(), {0x00, 0x00, 0x01, 0x65, 0x88});  // IDR slice
  EXPECT_EQ(expected,
    coral::text::extractCea708CcDataFromAnnexB(annexB.data(), annexB.size(), false));
}

TEST(AVCea708ParserTest, AppliesTheDelayCommand) {
  std::vector<uint8_t> commands = defineWindow();
  commands.insert(commands.end(), {0x8D, 0x05});  // delay five tenths of a second
  commands.insert(commands.end(), {'H', 'i'});
  commands.push_back(0x03);
  const std::string data = ccData(commands);
  AVCea708Parser parser;
  std::vector<AVCuesWithTiming> results;
  ASSERT_TRUE(parser.parse(reinterpret_cast<const uint8_t*>(data.data()), data.size(),
    AVSubtitleParser::OutputOptions::allCues(),
    [&results](const AVCuesWithTiming& cues) { results.push_back(cues); }));
  ASSERT_EQ(1u, results.size());
  ASSERT_EQ(1u, results[0].cueTimesUs.size());
  EXPECT_EQ(500000, results[0].cueTimesUs[0]);
}

TEST(AVCea708ParserTest, CancelsTheDelayCommand) {
  std::vector<uint8_t> commands = defineWindow();
  commands.insert(commands.end(), {0x8D, 0x05, 0x8E});  // DLY five tenths, then DLC
  commands.insert(commands.end(), {'H', 'i'});
  commands.push_back(0x03);
  const std::string data = ccData(commands);
  AVCea708Parser parser;
  std::vector<AVCuesWithTiming> results;
  ASSERT_TRUE(parser.parse(reinterpret_cast<const uint8_t*>(data.data()), data.size(),
    AVSubtitleParser::OutputOptions::allCues(),
    [&results](const AVCuesWithTiming& cues) { results.push_back(cues); }));
  ASSERT_EQ(1u, results.size());
  EXPECT_TRUE(results[0].cueTimesUs.empty());
}

TEST(AVCea708ParserTest, DecodesTheSelectedService) {
  const std::string data = ccData(textCommands("Two"), 2);
  AVCea708Parser parser(2);
  std::vector<AVCuesWithTiming> results;
  ASSERT_TRUE(parser.parse(reinterpret_cast<const uint8_t*>(data.data()), data.size(),
    AVSubtitleParser::OutputOptions::allCues(),
    [&results](const AVCuesWithTiming& cues) { results.push_back(cues); }));
  ASSERT_EQ(1u, results.size());
  ASSERT_EQ(1u, results[0].cues.size());
  EXPECT_EQ("Two", results[0].cues[0].text);

  AVCea708Parser serviceOne;
  std::vector<AVCuesWithTiming> ignored;
  ASSERT_TRUE(serviceOne.parse(reinterpret_cast<const uint8_t*>(data.data()), data.size(),
    AVSubtitleParser::OutputOptions::allCues(),
    [&ignored](const AVCuesWithTiming& cues) { ignored.push_back(cues); }));
  EXPECT_TRUE(ignored.empty());
}

TEST(AVCea708ParserTest, SplitsCcDataInto608And708) {
  const std::vector<uint8_t> cc = {
    0x04, 0x14, 0x20,  // CEA-608 type 0
    0x05, 0x2E, 0x00,  // CEA-608 type 1
    0x06, 0x03, 0x10,  // DTVCC packet start
    0x00, 0x11, 0x22,  // invalid
  };
  const auto split = coral::text::splitCeaCcData(cc.data(), cc.size());
  EXPECT_EQ((std::vector<uint8_t>{0x14, 0x20, 0x2E, 0x00}), split.cea608);
  EXPECT_EQ((std::vector<uint8_t>{0x06, 0x03, 0x10}), split.cea708);
}
