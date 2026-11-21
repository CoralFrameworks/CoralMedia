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

#include <gtest/gtest.h>

#include <mp4/SampleTable.h>

#include <cstdint>
#include <vector>

namespace {

constexpr uint32_t kKeyFrame = 1u << 0;

uint32_t readU32(const std::vector<uint8_t>& data, const size_t offset) {
  return (static_cast<uint32_t>(data[offset]) << 24) | (static_cast<uint32_t>(data[offset + 1]) << 16) |
         (static_cast<uint32_t>(data[offset + 2]) << 8) | static_cast<uint32_t>(data[offset + 3]);
}

std::string boxType(const std::vector<uint8_t>& data) {
  return std::string(reinterpret_cast<const char*>(data.data() + 4), 4);
}

void addChunk(coral::muxer::mp4::SampleTable& table, const int samples) {
  table.addChunk(1000);
  for (int i = 1; i < samples; ++i) {
    table.incrementLastChunkSampleCount();
  }
}

} // namespace

TEST(SampleTableTest, DerivesDurationsAndCompositionOffsetsFromDecodeOrder) {
  coral::muxer::mp4::SampleTable table(90000);
  table.addSample(10, 0, 0, kKeyFrame);
  table.addSample(10, 120000, 0, 0);
  table.addSample(10, 40000, 0, 0);
  table.addSample(10, 80000, 0, 0);
  table.finalize(0);

  ASSERT_EQ(4u, table.size());
  for (size_t i = 0; i < table.size(); ++i) {
    EXPECT_EQ(40000, table.sampleDurationUs(i));
  }
  EXPECT_EQ(14400, table.durationTicks());

  const auto offsets = table.compositionOffsetsTicks();
  ASSERT_EQ(4u, offsets.size());
  EXPECT_EQ(0, offsets[0]);
  EXPECT_EQ(7200, offsets[1]);
  EXPECT_EQ(-3600, offsets[2]);
  EXPECT_EQ(-3600, offsets[3]);
  EXPECT_TRUE(table.hasCompositionOffsets());
}

TEST(SampleTableTest, HonoursExplicitDurationsAndDuplicatesThePreviousOne) {
  coral::muxer::mp4::SampleTable table(1000);
  table.addSample(1, 0, 100000, kKeyFrame);
  table.addSample(2, 100000, 0, 0);
  table.addSample(3, 250000, 0, 0);
  table.finalize(0);

  ASSERT_EQ(3u, table.size());
  EXPECT_EQ(100000, table.sampleDurationUs(0));
  EXPECT_EQ(150000, table.sampleDurationUs(1));
  EXPECT_EQ(150000, table.sampleDurationUs(2));
  EXPECT_EQ(400000, table.durationUs());
}

TEST(SampleTableTest, CompressesSttsAndStscEntries) {
  coral::muxer::mp4::SampleTable table(90000);
  table.addSample(1, 0, 40000, kKeyFrame);
  table.addSample(1, 40000, 40000, 0);
  table.addSample(1, 80000, 40000, 0);
  table.addSample(1, 120000, 80000, 0);
  table.finalize(0);
  addChunk(table, 3);
  addChunk(table, 3);
  addChunk(table, 2);

  coral::muxer::mp4::BoxWriter stts;
  table.writeStts(stts);
  ASSERT_GE(stts.size(), 16u);
  EXPECT_EQ("stts", boxType(stts.data()));
  EXPECT_EQ(2u, readU32(stts.data(), 12)); // entry_count

  coral::muxer::mp4::BoxWriter stsc;
  table.writeStsc(stsc);
  EXPECT_EQ("stsc", boxType(stsc.data()));
  EXPECT_EQ(2u, readU32(stsc.data(), 12)); // entry_count: 3,3 then 2
  EXPECT_EQ(1u, readU32(stsc.data(), 16)); // first_chunk
  EXPECT_EQ(3u, readU32(stsc.data(), 20)); // samples_per_chunk
  EXPECT_EQ(3u, readU32(stsc.data(), 28)); // first_chunk of the second entry
  EXPECT_EQ(2u, readU32(stsc.data(), 32)); // samples_per_chunk
}

TEST(SampleTableTest, SelectsCo64ForLargeChunkOffsets) {
  coral::muxer::mp4::SampleTable table(1000);
  table.addSample(1, 0, 40, kKeyFrame);
  table.addChunk(5'000'000'000LL);

  coral::muxer::mp4::BoxWriter chunkOffsets;
  table.writeChunkOffsets(chunkOffsets, false);
  ASSERT_GE(chunkOffsets.size(), 8u);
  EXPECT_EQ("co64", boxType(chunkOffsets.data()));
  EXPECT_EQ(1u, readU32(chunkOffsets.data(), 12));
}

TEST(SampleTableTest, ShiftsAllChunkOffsets) {
  coral::muxer::mp4::SampleTable table(1000);
  table.addSample(1, 0, 40, kKeyFrame);
  table.addChunk(100);
  table.addSample(1, 40, 40, 0);
  table.addChunk(200);
  table.shiftChunkOffsets(64);

  ASSERT_EQ(2u, table.chunks().size());
  EXPECT_EQ(164, table.chunks()[0].offset);
  EXPECT_EQ(264, table.chunks()[1].offset);
}
