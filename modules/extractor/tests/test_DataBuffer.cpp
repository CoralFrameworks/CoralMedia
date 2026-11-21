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

#include <extractor/DataBuffer.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

using coral::AVDataBuffer;

TEST(AVDataBufferTest, StartsEmptyWithAFixedCapacity) {
  AVDataBuffer buffer;
  EXPECT_EQ(0u, buffer.position());
  EXPECT_GT(buffer.capacity(), 0u);
}

TEST(AVDataBufferTest, ReadsBigEndianValues) {
  const uint8_t data[] = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0, 0x11, 0x22};
  AVDataBuffer buffer(data, sizeof(data));

  EXPECT_EQ(0x12u, buffer.readUnsignedByte());
  EXPECT_EQ(0x3456u, buffer.readUnsignedShort());
  EXPECT_EQ(0x789ABCu, buffer.readUnsignedInt24());
  EXPECT_EQ(0xDEu, buffer.readUnsignedByte());
  EXPECT_EQ(0xF011u, buffer.readUnsignedShort());
  EXPECT_EQ(0x22u, buffer.readUnsignedByte());
}

TEST(AVDataBufferTest, ReadsLittleEndianValues) {
  const uint8_t data[] = {0x34, 0x12, 0x78, 0x56, 0x34, 0x12, 0x00, 0x00};
  AVDataBuffer buffer(data, sizeof(data));

  EXPECT_EQ(0x1234u, buffer.readLittleEndianUnsignedShort());
  EXPECT_EQ(0x12345678u, buffer.readLittleEndianUnsignedInt());
}

TEST(AVDataBufferTest, GrowsWhenResetToALargerCapacity) {
  AVDataBuffer buffer(4);
  buffer.reset(4096);
  EXPECT_GE(buffer.capacity(), 4096u);
  EXPECT_EQ(4096u, buffer.limit());
  EXPECT_EQ(0u, buffer.position());
}

TEST(AVDataBufferTest, TruncateSetsPositionAndLimit) {
  AVDataBuffer buffer(16);
  buffer.truncate(4, 12);
  EXPECT_EQ(4u, buffer.position());
  EXPECT_EQ(12u, buffer.limit());
  EXPECT_EQ(8u, buffer.remaining());
}

TEST(AVDataBufferTest, SkipBytesClampsToTheLimit) {
  const uint8_t data[] = {1, 2, 3, 4};
  AVDataBuffer buffer(data, sizeof(data));
  buffer.skipBytes(100);
  EXPECT_EQ(4u, buffer.position());
}

TEST(AVDataBufferTest, ReadBytesResizesAndAdvances) {
  const uint8_t data[] = {1, 2, 3, 4, 5, 6};
  AVDataBuffer buffer(data, sizeof(data));
  std::vector<uint8_t> out;
  buffer.readBytes(out, 4);
  ASSERT_EQ(4u, out.size());
  EXPECT_EQ(1, out[0]);
  EXPECT_EQ(4, out[3]);
  EXPECT_EQ(4u, buffer.position());
}

TEST(AVDataBufferTest, PeekBytesDoesNotAdvance) {
  const uint8_t data[] = {9, 8, 7};
  AVDataBuffer buffer(data, sizeof(data));
  const auto out = buffer.peekBytes(2);
  ASSERT_EQ(2u, out.size());
  EXPECT_EQ(9, out[0]);
  EXPECT_EQ(0u, buffer.position());
}

TEST(AVDataBufferTest, CopyToClampsToTheRemainingBytes) {
  const uint8_t data[] = {1, 2};
  AVDataBuffer buffer(data, sizeof(data));
  uint8_t destination[4] = {0, 0, 0, 0};
  EXPECT_EQ(2u, buffer.copyTo(destination, 4));
}

TEST(AVDataBufferTest, CopyOfABufferKeepsPositionAndLimit) {
  const uint8_t data[] = {1, 2, 3, 4, 5, 6, 7, 8};
  AVDataBuffer source(data, sizeof(data));
  source.position(3);
  source.limit(6);

  AVDataBuffer copy(source);
  EXPECT_EQ(3u, copy.position());
  EXPECT_EQ(6u, copy.limit());
  EXPECT_EQ(5, copy.array()[4]);
}
