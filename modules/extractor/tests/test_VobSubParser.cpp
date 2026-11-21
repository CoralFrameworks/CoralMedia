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

#include <extractor/text/CuesWithTiming.h>
#include <extractor/text/SubtitleParser.h>
#include <extractor/text/vobsub/VobSubParser.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include <zlib.h>

using coral::AVCue;
using coral::AVCuesWithTiming;
using coral::AVSubtitleParser;
using coral::AVVobSubParser;

namespace {

void appendU16(std::vector<uint8_t>& data, const uint16_t value) {
  data.push_back(static_cast<uint8_t>(value >> 8));
  data.push_back(static_cast<uint8_t>(value & 0xFF));
}

std::vector<uint8_t> zlibCompress(const std::vector<uint8_t>& input) {
  uLongf size = compressBound(static_cast<uLong>(input.size()));
  std::vector<uint8_t> output(size);
  EXPECT_EQ(Z_OK, compress2(output.data(), &size, input.data(),
    static_cast<uLong>(input.size()), Z_BEST_COMPRESSION));
  output.resize(size);
  return output;
}

std::vector<uint8_t> makeSpu() {
  std::vector<uint8_t> control;
  // A sequence states its time in 100ths of a second and the offset of the next sequence; zero
  // means that this is the last one.
  control.insert(control.end(), {0x00, 0x00, 0x00, 0x00});
  control.insert(control.end(), {0x03, 0x11, 0x11});
  control.insert(control.end(), {0x04, 0xFF, 0xFF});
  control.insert(control.end(), {0x05, 0x00, 0x00, 0x03, 0x00, 0x00, 0x01});
  control.insert(control.end(), {0x06, 0x00, 0x1B, 0x00, 0x1C});
  control.push_back(0xFF);

  std::vector<uint8_t> packet;
  appendU16(packet, static_cast<uint16_t>(4 + control.size() + 2));
  appendU16(packet, 4);
  packet.insert(packet.end(), control.begin(), control.end());
  packet.push_back(0x41);
  packet.push_back(0x41);
  return packet;
}

std::vector<std::vector<uint8_t>> codecPrivate() {
  const std::string index =
    "size: 4x2\n"
    "palette: 000000, ff0000, 00ff00, 0000ff, ffffff, 000000, 000000, 000000,"
    " 000000, 000000, 000000, 000000, 000000, 000000, 000000, 000000\n";
  return {std::vector<uint8_t>(index.begin(), index.end())};
}

} // namespace

TEST(AVVobSubParserTest, ParsesAnSpuPacketIntoABitmap) {
  const auto packet = makeSpu();
  AVVobSubParser parser(codecPrivate());
  std::vector<AVCuesWithTiming> results;
  ASSERT_TRUE(parser.parse(packet.data(), packet.size(),
    AVSubtitleParser::OutputOptions::allCues(),
    [&results](const AVCuesWithTiming& cues) { results.push_back(cues); }));
  ASSERT_EQ(1u, results.size());
  ASSERT_EQ(1u, results[0].cues.size());
  const auto& cue = results[0].cues[0];
  EXPECT_TRUE(cue.isBitmap());
  EXPECT_EQ(4, cue.bitmap.width);
  EXPECT_EQ(2, cue.bitmap.height);
  ASSERT_EQ(32u, cue.bitmap.pixels.size());
  EXPECT_EQ(255, cue.bitmap.pixels[0]);
  EXPECT_EQ(0, cue.bitmap.pixels[1]);
  EXPECT_EQ(0, cue.bitmap.pixels[2]);
  EXPECT_EQ(255, cue.bitmap.pixels[3]);
  EXPECT_FLOAT_EQ(0.0f, cue.position);
  EXPECT_FLOAT_EQ(0.0f, cue.line);
  EXPECT_FLOAT_EQ(1.0f, cue.size);
  EXPECT_FLOAT_EQ(1.0f, cue.bitmapHeight);
  EXPECT_EQ(AVCue::AnchorType::kStart, cue.positionAnchor);
}

TEST(AVVobSubParserTest, ParsesAZlibCompressedSpu) {
  const auto packet = zlibCompress(makeSpu());
  AVVobSubParser parser(codecPrivate());
  std::vector<AVCuesWithTiming> results;
  ASSERT_TRUE(parser.parse(packet.data(), packet.size(),
    AVSubtitleParser::OutputOptions::allCues(),
    [&results](const AVCuesWithTiming& cues) { results.push_back(cues); }));
  ASSERT_EQ(1u, results.size());
  ASSERT_EQ(1u, results[0].cues.size());
  EXPECT_TRUE(results[0].cues[0].isBitmap());
}
