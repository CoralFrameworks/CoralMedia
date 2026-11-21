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
#include <extractor/text/pgs/PgsParser.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include <zlib.h>

using coral::AVCue;
using coral::AVCuesWithTiming;
using coral::AVPgsParser;
using coral::AVSubtitleParser;

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

void appendSegment(std::vector<uint8_t>& stream, const uint8_t type,
    const std::vector<uint8_t>& payload) {
  stream.push_back(0x50);
  stream.push_back(0x47);
  for (int i = 0; i < 4; ++i) stream.push_back(0);
  for (int i = 0; i < 4; ++i) stream.push_back(0);
  stream.push_back(type);
  appendU16(stream, static_cast<uint16_t>(payload.size()));
  stream.insert(stream.end(), payload.begin(), payload.end());
}

std::vector<uint8_t> makeDisplaySet() {
  std::vector<uint8_t> stream;
  std::vector<uint8_t> pcs;
  appendU16(pcs, 2);
  appendU16(pcs, 2);
  pcs.push_back(0x10);
  appendU16(pcs, 0);
  pcs.push_back(0);
  pcs.push_back(0);
  pcs.push_back(0);
  pcs.push_back(1);
  appendU16(pcs, 1);
  pcs.push_back(0);
  pcs.push_back(0);
  appendU16(pcs, 0);
  appendU16(pcs, 0);
  appendSegment(stream, 0x16, pcs);

  std::vector<uint8_t> pds{0, 0};
  pds.insert(pds.end(), {0, 16, 128, 128, 0});
  pds.insert(pds.end(), {1, 76, 255, 85, 255});
  appendSegment(stream, 0x14, pds);

  std::vector<uint8_t> ods;
  appendU16(ods, 1);
  ods.push_back(0);
  ods.push_back(0);
  ods.push_back(0);
  ods.push_back(0);
  ods.push_back(2);
  appendU16(ods, 2);
  appendU16(ods, 2);
  ods.push_back(4);
  ods.push_back(1);
  appendSegment(stream, 0x15, ods);
  appendSegment(stream, 0x80, {});
  return stream;
}

/** A 10x10 plane with one 3x2 window at (4,6) holding a 2x2 object at (1,0). */
std::vector<uint8_t> makeWindowedDisplaySet() {
  std::vector<uint8_t> stream;
  std::vector<uint8_t> pcs;
  appendU16(pcs, 10);
  appendU16(pcs, 10);
  pcs.push_back(0x10);
  appendU16(pcs, 0);
  pcs.push_back(0);
  pcs.push_back(0);
  pcs.push_back(0);
  pcs.push_back(1);
  appendU16(pcs, 1);  // object id
  pcs.push_back(1);   // window id
  pcs.push_back(0);   // flags
  appendU16(pcs, 1);  // object x within the window
  appendU16(pcs, 0);  // object y within the window
  appendSegment(stream, 0x16, pcs);

  std::vector<uint8_t> wds;
  wds.push_back(1);  // window count
  wds.push_back(1);  // window id
  appendU16(wds, 4);
  appendU16(wds, 6);
  appendU16(wds, 3);
  appendU16(wds, 2);
  appendSegment(stream, 0x17, wds);

  std::vector<uint8_t> pds{0, 0};
  pds.insert(pds.end(), {0, 16, 128, 128, 0});
  pds.insert(pds.end(), {1, 76, 255, 85, 255});
  appendSegment(stream, 0x14, pds);

  std::vector<uint8_t> ods;
  appendU16(ods, 1);
  ods.push_back(0);
  ods.push_back(0);
  ods.push_back(0);
  ods.push_back(0);
  ods.push_back(2);
  appendU16(ods, 2);
  appendU16(ods, 2);
  ods.push_back(4);
  ods.push_back(1);
  appendSegment(stream, 0x15, ods);
  appendSegment(stream, 0x80, {});
  return stream;
}

} // namespace

TEST(AVPgsParserTest, ParsesASmallDisplaySetIntoABitmap) {
  const auto stream = makeDisplaySet();
  AVPgsParser parser;
  std::vector<AVCuesWithTiming> results;
  ASSERT_TRUE(parser.parse(stream.data(), stream.size(),
    AVSubtitleParser::OutputOptions::allCues(),
    [&results](const AVCuesWithTiming& cues) { results.push_back(cues); }));
  ASSERT_EQ(1u, results.size());
  ASSERT_EQ(1u, results[0].cues.size());
  const auto& cue = results[0].cues[0];
  EXPECT_TRUE(cue.isBitmap());
  EXPECT_EQ(2, cue.bitmap.width);
  EXPECT_EQ(2, cue.bitmap.height);
  ASSERT_EQ(16u, cue.bitmap.pixels.size());
  EXPECT_EQ(255, cue.bitmap.pixels[3]);
  EXPECT_GT(cue.bitmap.pixels[0], 100);
  EXPECT_FLOAT_EQ(0.0f, cue.position);
  EXPECT_FLOAT_EQ(0.0f, cue.line);
  EXPECT_FLOAT_EQ(1.0f, cue.size);
  EXPECT_FLOAT_EQ(1.0f, cue.bitmapHeight);
  EXPECT_EQ(AVCue::AnchorType::kStart, cue.positionAnchor);
}

TEST(AVPgsParserTest, ParsesAZlibCompressedDisplaySet) {
  const auto stream = zlibCompress(makeDisplaySet());
  AVPgsParser parser;
  std::vector<AVCuesWithTiming> results;
  ASSERT_TRUE(parser.parse(stream.data(), stream.size(),
    AVSubtitleParser::OutputOptions::allCues(),
    [&results](const AVCuesWithTiming& cues) { results.push_back(cues); }));
  ASSERT_EQ(1u, results.size());
  ASSERT_EQ(1u, results[0].cues.size());
  EXPECT_EQ("", results[0].cues[0].text);
  EXPECT_TRUE(results[0].cues[0].isBitmap());
}

TEST(AVPgsParserTest, AppliesTheWindowDefinition) {
  const auto stream = makeWindowedDisplaySet();
  AVPgsParser parser;
  std::vector<AVCuesWithTiming> results;
  ASSERT_TRUE(parser.parse(stream.data(), stream.size(),
    AVSubtitleParser::OutputOptions::allCues(),
    [&results](const AVCuesWithTiming& cues) { results.push_back(cues); }));
  ASSERT_EQ(1u, results.size());
  ASSERT_EQ(1u, results[0].cues.size());
  const auto& cue = results[0].cues[0];
  EXPECT_EQ(3, cue.bitmap.width);
  EXPECT_EQ(2, cue.bitmap.height);
  EXPECT_FLOAT_EQ(0.4f, cue.position);
  EXPECT_FLOAT_EQ(0.6f, cue.line);
  EXPECT_FLOAT_EQ(0.3f, cue.size);
  EXPECT_FLOAT_EQ(0.2f, cue.bitmapHeight);
  ASSERT_EQ(3u * 2u * 4u, cue.bitmap.pixels.size());
  // The object sits at (1,0) inside the window, so the first pixel of row 0 is transparent and
  // the pixel at x=1 is the object's first pixel.
  EXPECT_EQ(0, cue.bitmap.pixels[3]);
  EXPECT_EQ(255, cue.bitmap.pixels[(1 * 4) + 3]);
}
