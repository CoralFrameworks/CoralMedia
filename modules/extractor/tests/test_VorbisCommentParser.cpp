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

#include <extractor/metadata/VorbisCommentParser.h>

#include <media/Metadata.h>
#include <media/MetadataEntries.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using coral::AVVorbisCommentParser;
using coral::BinaryMetadataEntry;
using coral::Metadata;
using coral::PictureMetadataEntry;
using coral::TextMetadataEntry;

namespace {

void putLittleEndianUInt32(std::vector<uint8_t>& out, const uint32_t value) {
  out.push_back(static_cast<uint8_t>(value & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

void putBigEndianUInt32(std::vector<uint8_t>& out, const uint32_t value) {
  out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>(value & 0xFF));
}

void appendString(std::vector<uint8_t>& out, const std::string& value) {
  out.insert(out.end(), value.begin(), value.end());
}

std::shared_ptr<Metadata> makeMetadata() {
  return std::make_shared<Metadata>(std::vector<std::shared_ptr<Metadata::Entry>>{});
}

const TextMetadataEntry* findText(const std::shared_ptr<Metadata>& metadata,
                                  const std::string& key) {
  for (const auto& entry : metadata->getEntries()) {
    const auto* text = dynamic_cast<TextMetadataEntry*>(entry.get());
    if (text != nullptr && text->getKey() == key) {
      return text;
    }
  }
  return nullptr;
}

} // namespace

TEST(AVVorbisCommentParserTest, ParsesTheVendorAndTheComments) {
  std::vector<uint8_t> block;
  putLittleEndianUInt32(block, 5);
  appendString(block, "coral");
  putLittleEndianUInt32(block, 3);
  for (const std::string comment : {"TITLE=Fixture", "ARTIST=Someone", "NOEQUALS"}) {
    putLittleEndianUInt32(block, static_cast<uint32_t>(comment.size()));
    appendString(block, comment);
  }

  const auto metadata = makeMetadata();
  EXPECT_EQ(block.size(), AVVorbisCommentParser::parse(block.data(), block.size(), metadata));

  ASSERT_NE(nullptr, findText(metadata, "vendor"));
  EXPECT_EQ("coral", findText(metadata, "vendor")->getValue());
  ASSERT_NE(nullptr, findText(metadata, "title"));
  EXPECT_EQ("Fixture", findText(metadata, "title")->getValue());
  ASSERT_NE(nullptr, findText(metadata, "artist"));
  EXPECT_EQ("Someone", findText(metadata, "artist")->getValue());
  ASSERT_NE(nullptr, findText(metadata, "noequals"));
  EXPECT_TRUE(findText(metadata, "noequals")->getValue().empty());
}

TEST(AVVorbisCommentParserTest, RejectsABlockWithAnInvalidLength) {
  std::vector<uint8_t> block;
  putLittleEndianUInt32(block, 100);
  appendString(block, "short");
  const auto metadata = makeMetadata();
  EXPECT_EQ(0u, AVVorbisCommentParser::parse(block.data(), block.size(), metadata));
  EXPECT_TRUE(metadata->getEntries().empty());
}

TEST(AVVorbisCommentParserTest, ParsesAFlacPicture) {
  std::vector<uint8_t> block;
  putBigEndianUInt32(block, 3);  // the picture is the front cover
  const std::string mimeType = "image/png";
  putBigEndianUInt32(block, static_cast<uint32_t>(mimeType.size()));
  appendString(block, mimeType);
  const std::string description = "cover";
  putBigEndianUInt32(block, static_cast<uint32_t>(description.size()));
  appendString(block, description);
  putBigEndianUInt32(block, 300);  // width
  putBigEndianUInt32(block, 200);  // height
  putBigEndianUInt32(block, 24);   // depth
  putBigEndianUInt32(block, 0);    // colours
  const std::vector<uint8_t> picture = {0x89, 'P', 'N', 'G', 1, 2, 3};
  putBigEndianUInt32(block, static_cast<uint32_t>(picture.size()));
  block.insert(block.end(), picture.begin(), picture.end());

  const auto metadata = makeMetadata();
  EXPECT_EQ(block.size(), AVVorbisCommentParser::parseFlacPicture(block.data(), block.size(), metadata));
  ASSERT_EQ(1u, metadata->getEntries().size());
  const auto* entry = dynamic_cast<const PictureMetadataEntry*>(metadata->getEntries()[0].get());
  ASSERT_NE(nullptr, entry);
  EXPECT_EQ("image/png", entry->getMimeType());
  EXPECT_EQ("cover", entry->getDescription());
  EXPECT_EQ(300, entry->getWidth());
  EXPECT_EQ(200, entry->getHeight());
  EXPECT_EQ(24, entry->getDepth());
  EXPECT_EQ(picture, entry->getData());
}

TEST(AVVorbisCommentParserTest, KeepsTheBytesOfACueSheet) {
  const std::vector<uint8_t> cueSheet = {1, 2, 3, 4};
  const auto metadata = makeMetadata();
  EXPECT_EQ(cueSheet.size(),
            AVVorbisCommentParser::parseFlacCuesheet(cueSheet.data(), cueSheet.size(), metadata));
  ASSERT_EQ(1u, metadata->getEntries().size());
  const auto* entry = dynamic_cast<const BinaryMetadataEntry*>(metadata->getEntries()[0].get());
  ASSERT_NE(nullptr, entry);
  EXPECT_EQ("flac.cuesheet", entry->getKey());
  EXPECT_EQ(cueSheet, entry->getValue());
}
