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

#include <datasource/DataSource.h>
#include <extractor/DefaultExtractorInput.h>
#include <extractor/MediaExtractorInput.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using coral::AVByteArrayDataSource;
using coral::AVMediaExtractorInput;
using coral::AVDataBuffer;
using coral::AVDefaultExtractorInput;
using coral::kEndOfInput;

namespace {

/**
 * The content used by the tests: 0x00 to 0xFF, then 0x00 to 0xFF again.
 */
std::vector<uint8_t> makePattern() {
  std::vector<uint8_t> data(512);
  for (size_t i = 0; i < data.size(); ++i) {
    data[i] = static_cast<uint8_t>(i % 256);
  }
  return data;
}

std::shared_ptr<AVByteArrayDataSource> openSource(const std::vector<uint8_t>& data) {
  auto source = std::make_shared<AVByteArrayDataSource>(data);
  EXPECT_TRUE(source->open("memory"));
  return source;
}

} // namespace anonymous

TEST(AVMediaExtractorInputTest, AdaptsTheInputOfTheReader) {
  const auto data = makePattern();
  const auto source = openSource(data);
  AVDefaultExtractorInput input(source, 0);
  AVMediaExtractorInput adapted(input);
  EXPECT_EQ(data.size(), static_cast<size_t>(adapted.getLength()));
  EXPECT_FALSE(adapted.isAtEnd());

  // Peeking does not move the read position, and it leaves that position to the extractor.
  std::vector<uint8_t> peeked(4);
  ASSERT_EQ(4, adapted.peek(peeked.data(), 0, peeked.size()));
  EXPECT_TRUE(std::equal(peeked.begin(), peeked.end(), data.begin()));
  EXPECT_EQ(0, adapted.getPosition());

  std::vector<uint8_t> read(8);
  ASSERT_EQ(8, adapted.read(read.data(), 0, read.size()));
  EXPECT_TRUE(std::equal(read.begin(), read.end(), data.begin()));
  EXPECT_EQ(8, adapted.getPosition());

  // The position the adapter sets is the one the input reports, and its peek position follows it.
  ASSERT_TRUE(adapted.setPosition(256, false));
  EXPECT_EQ(256, adapted.getPosition());
  EXPECT_EQ(256, adapted.getPeekPosition());

  // The position holder of the input is the one the reader updates.
  ASSERT_NE(nullptr, adapted.getPositionHolder());
  EXPECT_EQ(256, adapted.getPositionHolder()->getPosition());

  ASSERT_TRUE(adapted.skipFully(256));
  EXPECT_TRUE(adapted.isAtEnd());
  EXPECT_EQ(kEndOfInput, adapted.read(read.data(), 0, read.size()));
}

TEST(AVDefaultExtractorInputTest, PeekDoesNotAdvanceTheReadPosition) {
  const auto source = openSource(makePattern());
  AVDefaultExtractorInput input(source);

  EXPECT_EQ(0, input.getPosition());
  EXPECT_EQ(0, input.getPeekPosition());

  uint8_t buffer[4] = {};
  ASSERT_EQ(4, input.peek(buffer, 0, sizeof(buffer)));
  EXPECT_EQ(0, buffer[0]);
  EXPECT_EQ(3, buffer[3]);
  EXPECT_EQ(0, input.getPosition());
  EXPECT_EQ(4, input.getPeekPosition());
  EXPECT_EQ(4, input.getPeekPositionOffset());

  input.resetPeekPosition();
  EXPECT_EQ(0, input.getPeekPositionOffset());
}

TEST(AVDefaultExtractorInputTest, ReadAdvancesTheReadPosition) {
  const auto source = openSource(makePattern());
  AVDefaultExtractorInput input(source);

  uint8_t buffer[4] = {};
  ASSERT_EQ(4, input.read(buffer, 0, sizeof(buffer)));
  EXPECT_EQ(0, buffer[0]);
  EXPECT_EQ(4, input.getPosition());
  ASSERT_EQ(4, input.read(buffer, 0, sizeof(buffer)));
  EXPECT_EQ(4, buffer[0]);
}

TEST(AVDefaultExtractorInputTest, ReadingPeekedDataServesTheSameBytes) {
  const auto source = openSource(makePattern());
  AVDefaultExtractorInput input(source);

  uint8_t peeked[8] = {};
  ASSERT_EQ(8, input.peek(peeked, 0, sizeof(peeked)));
  EXPECT_EQ(0, input.getPosition());

  // A read always starts at the read position, whether the data was peeked or not.
  input.resetPeekPosition();
  uint8_t read[8] = {};
  ASSERT_EQ(8, input.read(read, 0, sizeof(read)));
  EXPECT_EQ(8, input.getPosition());
  for (int i = 0; i < 8; ++i) {
    EXPECT_EQ(peeked[i], read[i]);
  }
}

TEST(AVDefaultExtractorInputTest, ReadReturnsEndOfInputWhenNothingIsLeft) {
  const auto source = openSource(makePattern());
  AVDefaultExtractorInput input(source);

  ASSERT_TRUE(input.setPosition(512, true));
  uint8_t buffer[4] = {};
  EXPECT_EQ(kEndOfInput, input.read(buffer, 0, sizeof(buffer)));
}

TEST(AVDefaultExtractorInputTest, ReadReturnsLessThanRequestedAtTheEnd) {
  const auto source = openSource(makePattern());
  AVDefaultExtractorInput input(source);

  ASSERT_TRUE(input.setPosition(510, true));
  uint8_t buffer[8] = {};
  EXPECT_EQ(2, input.read(buffer, 0, sizeof(buffer)));
}

TEST(AVDefaultExtractorInputTest, SkipMovesForward) {
  const auto source = openSource(makePattern());
  AVDefaultExtractorInput input(source);

  EXPECT_EQ(100, input.skip(100));
  EXPECT_EQ(100, input.getPosition());

  uint8_t buffer[1] = {};
  ASSERT_EQ(1, input.read(buffer, 0, 1));
  EXPECT_EQ(100, buffer[0]);
}

TEST(AVDefaultExtractorInputTest, SkipClampsAtTheEndOfTheInput) {
  const auto source = openSource(makePattern());
  AVDefaultExtractorInput input(source);

  EXPECT_EQ(512, input.skip(1000));
  EXPECT_EQ(512, input.getPosition());
  EXPECT_EQ(kEndOfInput, input.skip(10));
}

TEST(AVDefaultExtractorInputTest, SetPositionBackwardsReloadsTheBuffer) {
  const auto source = openSource(makePattern());
  AVDefaultExtractorInput input(source);

  uint8_t buffer[4] = {};
  ASSERT_TRUE(input.setPosition(300, true));
  ASSERT_EQ(4, input.read(buffer, 0, 4));
  EXPECT_EQ(static_cast<uint8_t>(300 % 256), buffer[0]);

  ASSERT_TRUE(input.setPosition(10, true));
  ASSERT_EQ(4, input.read(buffer, 0, 4));
  EXPECT_EQ(static_cast<uint8_t>(10), buffer[0]);
}

TEST(AVDefaultExtractorInputTest, SetPositionForwardSeekable) {
  const auto source = openSource(makePattern());
  AVDefaultExtractorInput input(source);

  uint8_t buffer[1] = {};
  ASSERT_TRUE(input.setPosition(400, true));
  ASSERT_EQ(1, input.read(buffer, 0, 1));
  EXPECT_EQ(static_cast<uint8_t>(400 % 256), buffer[0]);
}

TEST(AVDefaultExtractorInputTest, SetPositionBeyondTheEndFails) {
  const auto source = openSource(makePattern());
  AVDefaultExtractorInput input(source);
  EXPECT_FALSE(input.setPosition(1000, false));
}

/**
 * Builds a source that is larger than the read ahead buffer of the input, so that a skip or a move has
 * to be served by more than one read.
 */
std::vector<uint8_t> makeLargePattern() {
  std::vector<uint8_t> data(300 * 1024);
  for (size_t i = 0; i < data.size(); ++i) {
    data[i] = static_cast<uint8_t>(i % 256);
  }
  return data;
}

TEST(AVDefaultExtractorInputTest, SkipsFullyMoreThanTheReadAheadBuffer) {
  const auto data = makeLargePattern();
  const auto source = openSource(data);
  AVDefaultExtractorInput input(source);

  // The payload of an `mdat` box, an unknown chunk of a WAV file or the padding of a FLAC block is
  // larger than the buffer the input reads ahead: the skip has to be served by several reads, and a
  // single one reported the end of the input instead of reaching the position that follows the data.
  constexpr long kSkipLength = 200 * 1024;
  ASSERT_TRUE(input.skipFully(kSkipLength));
  EXPECT_EQ(kSkipLength, input.getPosition());

  uint8_t buffer[4] = {};
  ASSERT_EQ(4, input.read(buffer, 0, sizeof(buffer)));
  for (int i = 0; i < 4; ++i) {
    EXPECT_EQ(static_cast<uint8_t>((kSkipLength + i) % 256), buffer[i]) << "at offset " << i;
  }
}

TEST(AVDefaultExtractorInputTest, SkipFullyFailsWhenTheInputEndsFirst) {
  const auto source = openSource(makePattern());
  AVDefaultExtractorInput input(source);
  EXPECT_FALSE(input.skipFully(1000));
  EXPECT_EQ(512, input.getPosition());
}

TEST(AVDefaultExtractorInputTest, MovesBeyondTheReadAheadBufferToTheRequestedPosition) {
  const auto data = makeLargePattern();
  const auto source = openSource(data);
  AVDefaultExtractorInput input(source);

  // A container moves to the offset of a sample, which can be megabytes away from the position it
  // parsed the header at. A move that only advanced by the size of the buffer reported success while
  // the input was left short of the position, and the samples were read from the wrong place.
  constexpr long kPosition = 250 * 1024;
  ASSERT_TRUE(input.setPosition(kPosition, false));
  EXPECT_EQ(kPosition, input.getPosition());

  uint8_t buffer[1] = {};
  ASSERT_EQ(1, input.read(buffer, 0, 1));
  EXPECT_EQ(static_cast<uint8_t>(kPosition % 256), buffer[0]);
}

TEST(AVDefaultExtractorInputTest, MovesToTheEndOfTheInput) {
  const auto source = openSource(makePattern());
  AVDefaultExtractorInput input(source);

  // The end of the input is a position like any other: reaching it is a success, and it is the only
  // position beyond the buffer that a move to the end has to walk through.
  ASSERT_TRUE(input.setPosition(512, false));
  EXPECT_EQ(512, input.getPosition());
}

TEST(AVDefaultExtractorInputTest, ReadFullyFailsOnATruncatedInput) {
  const auto source = openSource(makePattern());
  AVDefaultExtractorInput input(source);

  uint8_t buffer[8] = {};
  EXPECT_TRUE(input.readFully(buffer, 0, 8));
  input.resetPosition(0, true);
  EXPECT_TRUE(input.skipFully(500));
  EXPECT_FALSE(input.skipFully(100));

  // Reading more than what is left fails, after having consumed the remaining bytes. The buffer is
  // left untouched because the failure happened before the buffer was filled.
  AVDataBuffer beyond;
  EXPECT_FALSE(input.readFully(beyond, 100));
  EXPECT_TRUE(beyond.limit() <= 100u);
}

TEST(AVDefaultExtractorInputTest, ReadFullyIntoABufferFillsItCompletely) {
  const auto source = openSource(makePattern());
  AVDefaultExtractorInput input(source);

  AVDataBuffer buffer;
  ASSERT_TRUE(input.readFully(buffer, 512));
  EXPECT_EQ(512u, buffer.limit());
  EXPECT_EQ(0u, buffer.position());
  EXPECT_EQ(0, buffer.array()[0]);
  EXPECT_EQ(static_cast<uint8_t>(511 % 256), buffer.array()[511]);

  // The whole input was consumed, a further read fails.
  ASSERT_TRUE(input.readFully(buffer, 0));
  AVDataBuffer beyond;
  EXPECT_FALSE(input.readFully(beyond, 8));
}

TEST(AVDefaultExtractorInputTest, PeekFullyFailsOnATruncatedInput) {
  const auto source = openSource(makePattern());
  AVDefaultExtractorInput input(source);

  // The buffer must be large enough for the requested length, because a partial peek fills all the
  // bytes that are available.
  uint8_t buffer[1024] = {};
  EXPECT_TRUE(input.peekFully(buffer, 0, 8));
  EXPECT_EQ(0, input.getPosition());
  EXPECT_FALSE(input.peekFully(buffer, 0, sizeof(buffer)));
  EXPECT_EQ(0, input.getPosition());
  // The failed peek stopped at the end of the input, and the read position did not move.
  EXPECT_EQ(512, input.getPeekPositionOffset());
  input.resetPeekPosition();
  EXPECT_EQ(0, input.getPeekPositionOffset());
}

TEST(AVDefaultExtractorInputTest, ThePositionHolderFollowsTheReadPosition) {
  const auto source = openSource(makePattern());
  AVDefaultExtractorInput input(source);

  EXPECT_EQ(0, input.getPositionHolder()->getPosition());
  uint8_t buffer[16] = {};
  ASSERT_EQ(16, input.read(buffer, 0, sizeof(buffer)));
  EXPECT_EQ(16, input.getPositionHolder()->getPosition());

  input.getPositionHolder()->setPosition(42);
  EXPECT_EQ(42, input.getPositionHolder()->getPosition());
}

TEST(AVDefaultExtractorInputTest, SkipPeekedMovesOnlyThePeekPosition) {
  const auto source = openSource(makePattern());
  AVDefaultExtractorInput input(source);

  EXPECT_EQ(0, input.peekByte());
  input.skipPeeked(3);
  EXPECT_EQ(4, input.getPeekPosition());
  EXPECT_EQ(0, input.getPosition());
  EXPECT_EQ(4, input.peekByte());
}
