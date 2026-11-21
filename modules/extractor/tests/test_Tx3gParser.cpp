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
#include <extractor/text/tx3g/Tx3gParser.h>

#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using coral::AVCueSpan;
using coral::AVCuesWithTiming;
using coral::AVSubtitleParser;
using coral::AVTx3gParser;

namespace {

std::vector<uint8_t> readFile(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(input), {});
}

} // namespace

TEST(AVTx3gParserTest, ParsesTheInlineColourAndStyleOfASample) {
  const std::string base = std::string(CORAL_ASSETS_DIR) + "/subtitle/tx3g/";
  const auto initialization = readFile(base + "initialization");
  const auto sample = readFile(base + "sample_with_styl");
  ASSERT_FALSE(initialization.empty());
  ASSERT_FALSE(sample.empty());
  AVTx3gParser parser({initialization});
  std::vector<AVCuesWithTiming> results;
  ASSERT_TRUE(parser.parse(sample.data(), sample.size(),
    AVSubtitleParser::OutputOptions::allCues(),
    [&results](const AVCuesWithTiming& cues) { results.push_back(cues); }));
  ASSERT_EQ(1u, results.size());
  ASSERT_EQ(1u, results[0].cues.size());
  const auto& cue = results[0].cues[0];
  EXPECT_EQ("CC Test", cue.text);
  bool hasGreen = false;
  bool hasStyle = false;
  for (const auto& span: cue.spans) {
    if (span.type == AVCueSpan::Type::kForegroundColor) {
      hasGreen = span.color == 0xFF00FF00u;
    }
    if (span.type == AVCueSpan::Type::kTextStyle) {
      hasStyle = hasStyle || (span.style.bold && span.style.italic && span.style.underline);
    }
  }
  EXPECT_TRUE(hasGreen) << "the sample's inline green colour was not decoded";
  EXPECT_TRUE(hasStyle) << "the sample's inline font face was not decoded";
}