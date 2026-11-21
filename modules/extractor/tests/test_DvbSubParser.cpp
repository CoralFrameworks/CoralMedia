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
#include <extractor/text/dvbsub/DvbSubParser.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

using coral::AVCuesWithTiming;
using coral::AVDvbSubParser;
using coral::AVSubtitleParser;

namespace {

void appendU16(std::vector<uint8_t>& data, const uint16_t value) {
  data.push_back(static_cast<uint8_t>(value >> 8));
  data.push_back(static_cast<uint8_t>(value & 0xFF));
}

void appendSegment(std::vector<uint8_t>& stream, const uint8_t type,
    const std::vector<uint8_t>& payload) {
  stream.push_back(0x0F);
  stream.push_back(type);
  appendU16(stream, 0);
  appendU16(stream, static_cast<uint16_t>(payload.size()));
  stream.insert(stream.end(), payload.begin(), payload.end());
}

std::vector<uint8_t> makeStream() {
  std::vector<uint8_t> stream;
  std::vector<uint8_t> display{0x10};
  appendU16(display, 4);
  appendU16(display, 2);
  appendSegment(stream, 0x14, display);

  std::vector<uint8_t> page{0, 0, 1, 1};
  page.push_back(1);
  page.push_back(0);
  appendU16(page, 0);
  appendU16(page, 0);
  appendSegment(stream, 0x10, page);

  std::vector<uint8_t> region{1, 0x08};
  appendU16(region, 4);
  appendU16(region, 2);
  region.insert(region.end(), {0, 3, 0, 1, 0, 0});
  appendU16(region, 1);
  region.push_back(0);
  region.insert(region.end(), {0, 0, 0, 0});
  appendSegment(stream, 0x11, region);

  std::vector<uint8_t> clut{0, 0};
  clut.insert(clut.end(), {1, 0x01, 76, 255, 85, 255});
  appendSegment(stream, 0x12, clut);

  std::vector<uint8_t> object;
  appendU16(object, 1);
  object.push_back(0);
  object.push_back(0);
  appendU16(object, 4);
  appendU16(object, 4);
  object.insert(object.end(), {1, 1, 1, 1, 1, 1, 1, 1});
  appendSegment(stream, 0x13, object);
  appendSegment(stream, 0x80, {});
  return stream;
}

} // namespace

TEST(AVDvbSubParserTest, PaintsTheBottomFieldBelowTheTopField) {
  std::vector<uint8_t> stream;
  std::vector<uint8_t> display{0x10};
  appendU16(display, 4);
  appendU16(display, 4);
  appendSegment(stream, 0x14, display);

  std::vector<uint8_t> page{0, 0, 1, 1, 1, 0};
  appendU16(page, 0);
  appendU16(page, 0);
  appendSegment(stream, 0x10, page);

  std::vector<uint8_t> region{1, 0x08};
  appendU16(region, 4);
  appendU16(region, 4);
  region.insert(region.end(), {0, 3, 0, 1, 0, 0});
  appendU16(region, 1);
  region.push_back(0);
  region.insert(region.end(), {0, 0, 0, 0});
  appendSegment(stream, 0x11, region);

  std::vector<uint8_t> clut{0, 0};
  clut.insert(clut.end(), {1, 0x01, 76, 255, 85, 255});
  clut.insert(clut.end(), {2, 0x01, 76, 255, 0, 255});
  appendSegment(stream, 0x12, clut);

  std::vector<uint8_t> object;
  appendU16(object, 1);
  object.push_back(0);
  object.push_back(0);
  appendU16(object, 4);
  appendU16(object, 4);
  object.insert(object.end(), {1, 1, 1, 1, 2, 2, 2, 2});
  appendSegment(stream, 0x13, object);
  appendSegment(stream, 0x80, {});

  AVDvbSubParser parser;
  std::vector<AVCuesWithTiming> results;
  ASSERT_TRUE(parser.parse(stream.data(), stream.size(),
    AVSubtitleParser::OutputOptions::allCues(),
    [&results](const AVCuesWithTiming& cues) { results.push_back(cues); }));
  ASSERT_EQ(1u, results.size());
  ASSERT_EQ(1u, results[0].cues.size());
  const auto& bitmap = results[0].cues[0].bitmap;
  ASSERT_EQ(4, bitmap.width);
  ASSERT_EQ(4, bitmap.height);
  // Row 0 is the top field and row 1 is the bottom field.
  EXPECT_NEAR(255, bitmap.pixels[0], 1);
  EXPECT_NEAR(0, bitmap.pixels[1], 4);
  EXPECT_NEAR(30, bitmap.pixels[4 * 4 + 1], 4);
  EXPECT_NEAR(255, bitmap.pixels[4 * 4 + 3], 1);
}

TEST(AVDvbSubParserTest, ParsesAPageRegionIntoABitmap) {
  const auto stream = makeStream();
  AVDvbSubParser parser;
  std::vector<AVCuesWithTiming> results;
  ASSERT_TRUE(parser.parse(stream.data(), stream.size(),
    AVSubtitleParser::OutputOptions::allCues(),
    [&results](const AVCuesWithTiming& cues) { results.push_back(cues); }));
  ASSERT_EQ(1u, results.size());
  ASSERT_EQ(1u, results[0].cues.size());
  const auto& cue = results[0].cues[0];
  EXPECT_TRUE(cue.isBitmap());
  EXPECT_EQ(4, cue.bitmap.width);
  EXPECT_EQ(2, cue.bitmap.height);
  ASSERT_EQ(32u, cue.bitmap.pixels.size());
  EXPECT_NEAR(255, cue.bitmap.pixels[0], 1);
  EXPECT_EQ(0, cue.bitmap.pixels[1]);
  EXPECT_EQ(0, cue.bitmap.pixels[2]);
  EXPECT_EQ(255, cue.bitmap.pixels[3]);
}
