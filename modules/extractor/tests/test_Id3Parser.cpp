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

#include <extractor/metadata/Id3Parser.h>

#include <media/Metadata.h>
#include <media/MetadataEntries.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using coral::AVId3Parser;
using coral::Id3FrameMetadataEntry;
using coral::Metadata;

namespace {

void appendSynchsafe(std::vector<uint8_t>& out, const uint32_t value) {
  out.push_back(static_cast<uint8_t>((value >> 21) & 0x7F));
  out.push_back(static_cast<uint8_t>((value >> 14) & 0x7F));
  out.push_back(static_cast<uint8_t>((value >> 7) & 0x7F));
  out.push_back(static_cast<uint8_t>(value & 0x7F));
}

void appendUInt32(std::vector<uint8_t>& out, const uint32_t value) {
  out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>(value & 0xFF));
}

void appendUInt24(std::vector<uint8_t>& out, const uint32_t value) {
  out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>(value & 0xFF));
}

/** Builds a version 2.3 or 2.4 frame. */
std::vector<uint8_t> makeFrame(const std::string& id, const std::vector<uint8_t>& payload,
                               const uint8_t formatFlags = 0) {
  std::vector<uint8_t> frame;
  frame.insert(frame.end(), id.begin(), id.end());
  if (id.size() == 3) {
    appendUInt24(frame, static_cast<uint32_t>(payload.size()));
  } else {
    appendUInt32(frame, static_cast<uint32_t>(payload.size()));
  }
  if (id.size() == 4) {
    frame.push_back(0);  // status flags
    frame.push_back(formatFlags);
  }
  frame.insert(frame.end(), payload.begin(), payload.end());
  return frame;
}

/** Builds a version 2.4 frame, whose size is synchsafe. */
std::vector<uint8_t> makeFrame24(const std::string& id, const std::vector<uint8_t>& payload,
                                 const uint8_t formatFlags = 0) {
  std::vector<uint8_t> frame;
  frame.insert(frame.end(), id.begin(), id.end());
  appendSynchsafe(frame, static_cast<uint32_t>(payload.size()));
  frame.push_back(0);  // status flags
  frame.push_back(formatFlags);
  frame.insert(frame.end(), payload.begin(), payload.end());
  return frame;
}

std::vector<uint8_t> makeTag(const uint8_t major, const std::vector<uint8_t>& frames,
                             const uint8_t flags = 0) {
  std::vector<uint8_t> tag = {'I', 'D', '3', major, 0, flags};
  appendSynchsafe(tag, static_cast<uint32_t>(frames.size()));
  tag.insert(tag.end(), frames.begin(), frames.end());
  return tag;
}

std::vector<uint8_t> concat(const std::vector<std::vector<uint8_t>>& parts) {
  std::vector<uint8_t> out;
  for (const auto& part : parts) {
    out.insert(out.end(), part.begin(), part.end());
  }
  return out;
}

/** Returns the first entry whose frame id is @p id. */
const Id3FrameMetadataEntry* findFrame(const std::shared_ptr<Metadata>& metadata,
                                       const std::string& id) {
  for (const auto& entry : metadata->getEntries()) {
    const auto* frame = dynamic_cast<Id3FrameMetadataEntry*>(entry.get());
    if (frame != nullptr && frame->getFrameId() == id) {
      return frame;
    }
  }
  return nullptr;
}

std::shared_ptr<Metadata> parseTag(const std::vector<uint8_t>& tag) {
  auto metadata = std::make_shared<Metadata>(std::vector<std::shared_ptr<Metadata::Entry>>{});
  const size_t consumed = AVId3Parser::parse(tag.data(), tag.size(), metadata);
  EXPECT_EQ(tag.size(), consumed);
  return metadata;
}

/** Builds the payload of a text frame with a UTF-8 value. */
std::vector<uint8_t> utf8Text(const std::string& text) {
  std::vector<uint8_t> payload = {3};
  payload.insert(payload.end(), text.begin(), text.end());
  return payload;
}

} // namespace

TEST(AVId3ParserTest, RecognisesTheTagAndItsSize) {
  const auto tag = makeTag(3, makeFrame("TIT2", utf8Text("Title")));
  EXPECT_TRUE(AVId3Parser::isId3(tag.data(), tag.size()));
  EXPECT_EQ(tag.size(), AVId3Parser::getTagSize(tag.data(), tag.size()));
  EXPECT_EQ(0u, AVId3Parser::getTagSize(tag.data(), 4));
  const uint8_t notATag[] = {'I', 'D', '4', 3, 0, 0, 0, 0, 0, 0};
  EXPECT_FALSE(AVId3Parser::isId3(notATag, sizeof(notATag)));
}

TEST(AVId3ParserTest, ParsesTheTextFramesOfAVersion23Tag) {
  const auto tag = makeTag(3, concat({makeFrame("TIT2", utf8Text("Title")),
                                      makeFrame("TPE1", utf8Text("Artist")),
                                      makeFrame("TALB", utf8Text("Album"))}));
  const auto metadata = parseTag(tag);
  EXPECT_EQ("Title", AVId3Parser::findText(metadata, "TIT2"));
  EXPECT_EQ("Artist", AVId3Parser::findText(metadata, "TPE1"));
  EXPECT_EQ("Album", AVId3Parser::findText(metadata, "TALB"));
}

TEST(AVId3ParserTest, DecodesALatin1TextFrame) {
  std::vector<uint8_t> payload = {0, 'C', 'a', 'f', 0xE9};
  const auto metadata = parseTag(makeTag(3, makeFrame("TIT2", payload)));
  EXPECT_EQ("Caf\xC3\xA9", AVId3Parser::findText(metadata, "TIT2"));
}

TEST(AVId3ParserTest, ParsesACommentFrame) {
  std::vector<uint8_t> payload = {3, 'e', 'n', 'g', 0, 'a', ' ', 'c', 'o', 'm', 'm', 'e', 'n', 't'};
  const auto metadata = parseTag(makeTag(3, makeFrame("COMM", payload)));
  const auto* frame = findFrame(metadata, "COMM");
  ASSERT_NE(nullptr, frame);
  EXPECT_EQ("eng", frame->getLanguage());
  EXPECT_EQ("", frame->getDescription());
  EXPECT_EQ("a comment", frame->getText());
}

TEST(AVId3ParserTest, ParsesAPrivateFrame) {
  std::vector<uint8_t> payload = {'c', 'o', 'm', '.', 'e', 'x', 'a', 'm', 'p', 'l', 'e', 0, 1, 2, 3};
  const auto metadata = parseTag(makeTag(3, makeFrame("PRIV", payload)));
  const auto* frame = findFrame(metadata, "PRIV");
  ASSERT_NE(nullptr, frame);
  EXPECT_EQ("com.example", frame->getDescription());
  ASSERT_EQ(3u, frame->getPayload().size());
  EXPECT_EQ(1, frame->getPayload()[0]);
  EXPECT_EQ(3, frame->getPayload()[2]);
}

TEST(AVId3ParserTest, ParsesAPictureFrame) {
  std::vector<uint8_t> payload = {3};
  const std::string mime = "image/png";
  payload.insert(payload.end(), mime.begin(), mime.end());
  payload.push_back(0);   // end of the MIME type
  payload.push_back(3);   // the picture is the front cover
  payload.push_back(0);   // empty description
  const std::vector<uint8_t> picture = {0x89, 'P', 'N', 'G', 1, 2, 3};
  payload.insert(payload.end(), picture.begin(), picture.end());
  const auto metadata = parseTag(makeTag(3, makeFrame("APIC", payload)));
  const auto* frame = findFrame(metadata, "APIC");
  ASSERT_NE(nullptr, frame);
  EXPECT_EQ("image/png", frame->getDescription());
  ASSERT_EQ(picture.size(), frame->getPayload().size());
  EXPECT_EQ(0x89, frame->getPayload()[0]);
}

TEST(AVId3ParserTest, KeepsAFrameItHasNoModelFor) {
  const std::vector<uint8_t> payload = {9, 8, 7};
  const auto metadata = parseTag(makeTag(3, makeFrame("XABC", payload)));
  const auto* frame = findFrame(metadata, "XABC");
  ASSERT_NE(nullptr, frame);
  EXPECT_EQ(payload, frame->getPayload());
  EXPECT_TRUE(frame->getText().empty());
}

TEST(AVId3ParserTest, ParsesAVersion24TagWithSynchsafeFrameSizes) {
  std::vector<uint8_t> txxxPayload = {3};
  for (const char* c = "desc"; *c != '\0'; ++c) {
    txxxPayload.push_back(static_cast<uint8_t>(*c));
  }
  txxxPayload.push_back(0);
  for (const char* c = "value"; *c != '\0'; ++c) {
    txxxPayload.push_back(static_cast<uint8_t>(*c));
  }
  const auto tag = makeTag(4, concat({makeFrame24("TIT2", utf8Text("Title")),
                                      makeFrame24("TXXX", txxxPayload)}));
  const auto metadata = parseTag(tag);
  EXPECT_EQ("Title", AVId3Parser::findText(metadata, "TIT2"));
  const auto* frame = findFrame(metadata, "TXXX");
  ASSERT_NE(nullptr, frame);
  EXPECT_EQ("desc", frame->getDescription());
  EXPECT_EQ("value", frame->getText());
}

TEST(AVId3ParserTest, RemovesTheUnsynchronisationOfATag) {
  // A version 2.3 tag escapes every 0xFF of its body with a 0x00, after its frame sizes were
  // computed; the parser unsynchronises the whole body before it reads the frames.
  const std::vector<uint8_t> payload = {0, 'A', 0xFF, 'B'};
  const auto body = makeFrame("TIT2", payload);
  std::vector<uint8_t> storedBody;
  for (const uint8_t byte : body) {
    storedBody.push_back(byte);
    if (byte == 0xFF) {
      storedBody.push_back(0);
    }
  }
  std::vector<uint8_t> tag = {'I', 'D', '3', 3, 0, 0x80};
  appendSynchsafe(tag, static_cast<uint32_t>(storedBody.size()));
  tag.insert(tag.end(), storedBody.begin(), storedBody.end());
  const auto metadata = parseTag(tag);
  EXPECT_EQ("A\xC3\xBF" "B", AVId3Parser::findText(metadata, "TIT2"));
}

TEST(AVId3ParserTest, RemovesTheUnsynchronisationOfAVersion24Frame) {
  // A version 2.4 frame states its own unsynchronisation in its format flags.
  std::vector<uint8_t> payload = {0, 'A', 0xFF, 0x00, 'B'};
  const auto tag = makeTag(4, makeFrame24("TIT2", payload, 0x02));
  const auto metadata = parseTag(tag);
  EXPECT_EQ("A\xC3\xBF" "B", AVId3Parser::findText(metadata, "TIT2"));
}

TEST(AVId3ParserTest, MapsAVersion22FrameIdentifier) {
  // A version 2.2 frame has a three character identifier.
  std::vector<uint8_t> payload = {0, 'O', 'l', 'd'};
  const auto tag = makeTag(2, makeFrame("TT2", payload));
  const auto metadata = parseTag(tag);
  const auto* frame = findFrame(metadata, "TT2");
  ASSERT_NE(nullptr, frame);
  EXPECT_EQ("Old", frame->getText());
  // The lookup accepts either identifier.
  EXPECT_EQ("Old", AVId3Parser::findText(metadata, "TIT2"));
  EXPECT_EQ("Old", AVId3Parser::findText(metadata, "TT2"));
}

TEST(AVId3ParserTest, ParsesTwoTagsInARow) {
  const auto first = makeTag(3, makeFrame("TIT2", utf8Text("First")));
  const auto second = makeTag(3, makeFrame("TPE1", utf8Text("Second")));
  const auto media = concat({first, second});
  auto metadata = std::make_shared<Metadata>(std::vector<std::shared_ptr<Metadata::Entry>>{});
  const size_t consumed = AVId3Parser::parseAll(media.data(), media.size(), metadata);
  EXPECT_EQ(media.size(), consumed);
  EXPECT_EQ("First", AVId3Parser::findText(metadata, "TIT2"));
  EXPECT_EQ("Second", AVId3Parser::findText(metadata, "TPE1"));
}

TEST(AVId3ParserTest, SkipsACompressedFrame) {
  // The compression flag of a version 2.3 frame is the high bit of its format flags.
  const auto tag = makeTag(3, makeFrame("TIT2", utf8Text("Hidden"), 0x80));
  const auto metadata = parseTag(tag);
  EXPECT_TRUE(metadata->getEntries().empty());
}

TEST(AVId3ParserTest, RejectsATagThatStatesMoreThanItHolds) {
  std::vector<uint8_t> tag = {'I', 'D', '3', 3, 0, 0};
  appendSynchsafe(tag, 100);
  EXPECT_EQ(0u, AVId3Parser::getTagSize(tag.data(), tag.size()));
}
