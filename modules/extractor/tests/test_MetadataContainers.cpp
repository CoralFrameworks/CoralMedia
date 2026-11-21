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

#include "TestMedia.h"

#include <datasource/DataSource.h>
#include <extractor/DefaultExtractorsFactory.h>
#include <extractor/metadata/Id3Parser.h>
#include <extractor/DefaultExtractorsFactory.h>
#include <extractor/metadata/Id3Parser.h>
#include <extractor/Reader.h>

#include <media/Format.h>
#include <media/Metadata.h>
#include <media/MetadataEntries.h>
#include <media/Types.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using coral::AVByteArrayDataSource;
using coral::AVDefaultExtractorsFactory;
using coral::AVExtractorsFactory;
using coral::AVReader;
using coral::AVSample;
using coral::Definitions;
using coral::FileTypes;
using coral::MediaFormat;
using coral::MediaType;
using coral::Metadata;
using coral::ChapterMetadataEntry;
using coral::TextMetadataEntry;

namespace {

/** A reader over an in memory media whose every track is consumed. */
class MetadataHarness final {
public:
  explicit MetadataHarness(std::vector<uint8_t> media, const int fileType = -1) {
    auto source = std::make_shared<AVByteArrayDataSource>(std::move(media));
    source->open("memory");
    reader_ = std::unique_ptr<AVReader>(
      new AVReader(std::move(source),
                   std::unique_ptr<AVExtractorsFactory>(new AVDefaultExtractorsFactory(fileType))));
  }

  MetadataHarness(const MetadataHarness& other) = delete;

  MetadataHarness& operator=(const MetadataHarness& other) = delete;

  bool read() {
    for (int id = 0; id < 8; ++id) {
      reader_->setTrackConsumer(id, [](const AVSample&) {});
    }
    if (!reader_->prepare()) {
      return false;
    }
    while (reader_->read() == AVReader::kResultContinue) {
    }
    return true;
  }

  [[nodiscard]] AVReader& reader() const { return *reader_; }

  [[nodiscard]] int firstTrackId() const {
    const auto ids = reader_->getTrackIds();
    return ids.empty() ? Definitions::kIndexUnset : ids.front();
  }

  [[nodiscard]] std::shared_ptr<MediaFormat> format() const {
    return reader_->getTrackFormat(firstTrackId());
  }

private:
  std::unique_ptr<AVReader> reader_;
};

const std::string* findText(const std::shared_ptr<Metadata>& metadata, const std::string& key) {
  if (metadata == nullptr) {
    return nullptr;
  }
  for (const auto& entry : metadata->getEntries()) {
    const auto* text = dynamic_cast<TextMetadataEntry*>(entry.get());
    if (text != nullptr && text->getKey() == key) {
      return &text->getValue();
    }
  }
  return nullptr;
}

} // namespace

TEST(MetadataContainerTest, ReadsTheCommentsOfAFlacStream) {
  const auto media =
    coral::test::buildFlac(44100, 2, 16, 4096, {"TITLE=Fixture", "ARTIST=Someone"});
  MetadataHarness harness(std::vector<uint8_t>(media.begin(), media.end()));
  ASSERT_TRUE(harness.read());

  const auto& format = harness.format();
  ASSERT_NE(nullptr, format);
  const auto& metadata = format->getMetadata();
  ASSERT_NE(nullptr, metadata);
  ASSERT_NE(nullptr, findText(metadata, "title"));
  EXPECT_EQ("Fixture", *findText(metadata, "title"));
  ASSERT_NE(nullptr, findText(metadata, "artist"));
  EXPECT_EQ("Someone", *findText(metadata, "artist"));
  ASSERT_NE(nullptr, findText(metadata, "vendor"));
  EXPECT_EQ("coral", *findText(metadata, "vendor"));
}

TEST(MetadataContainerTest, ReadsTheCommentsOfAVorbisOggStream) {
  const auto media = coral::test::buildOggVorbis(44100, 2, 4);
  MetadataHarness harness(std::vector<uint8_t>(media.begin(), media.end()));
  ASSERT_TRUE(harness.read());

  const auto& format = harness.format();
  ASSERT_NE(nullptr, format);
  const auto& metadata = format->getMetadata();
  ASSERT_NE(nullptr, metadata);
  // The fixture states one comment, TITLE=fixture.
  ASSERT_NE(nullptr, findText(metadata, "title"));
  EXPECT_EQ("fixture", *findText(metadata, "title"));
  ASSERT_NE(nullptr, findText(metadata, "vendor"));
  EXPECT_EQ("coral", *findText(metadata, "vendor"));
}

TEST(MetadataContainerTest, ReadsTheTitleOfAnMp3Stream) {
  const auto media = coral::test::buildMp3(5, true);
  MetadataHarness harness(std::vector<uint8_t>(media.begin(), media.end()));
  ASSERT_TRUE(harness.read());

  const auto& format = harness.format();
  ASSERT_NE(nullptr, format);
  const auto& metadata = format->getMetadata();
  ASSERT_NE(nullptr, metadata);
  EXPECT_EQ("Coral", coral::AVId3Parser::findText(metadata, "TIT2"));
}

TEST(MetadataContainerTest, ReadsTheTitleOfAnAdtsStream) {
  const auto media = coral::test::buildAdts(5, 4, 2, true);
  MetadataHarness harness(std::vector<uint8_t>(media.begin(), media.end()));
  ASSERT_TRUE(harness.read());

  const auto& format = harness.format();
  ASSERT_NE(nullptr, format);
  const auto& metadata = format->getMetadata();
  ASSERT_NE(nullptr, metadata);
  EXPECT_EQ("Coral", coral::AVId3Parser::findText(metadata, "TIT2"));
}

TEST(MetadataContainerTest, ReadsTheTagsAndChaptersOfAMatroskaStream) {
  const std::string path = std::string(CORAL_ASSETS_DIR) + "/subtitle/tags.mkv";
  auto dataSource = std::make_shared<coral::AVFileDataSource>();
  ASSERT_TRUE(dataSource->open(path));
  coral::AVReader reader(
    dataSource, std::unique_ptr<coral::AVExtractorsFactory>(new AVDefaultExtractorsFactory()));
  for (int id = 0; id < 8; ++id) {
    reader.setTrackConsumer(id, [](const AVSample&) {});
  }
  ASSERT_TRUE(reader.prepare());
  while (reader.read() == coral::AVReader::kResultContinue) {
  }
  const auto ids = reader.getTrackIds();
  ASSERT_FALSE(ids.empty());
  const auto format = reader.getTrackFormat(ids.front());

  ASSERT_NE(nullptr, format);
  const auto& metadata = format->getMetadata();
  ASSERT_NE(nullptr, metadata);


  const std::string* title = findText(metadata, "title");
  ASSERT_NE(nullptr, title);
  EXPECT_EQ("Fixture", *title);
  const std::string* artist = findText(metadata, "artist");
  ASSERT_NE(nullptr, artist);
  EXPECT_EQ("Someone", *artist);

  std::vector<const ChapterMetadataEntry*> chapters;
  for (const auto& entry : metadata->getEntries()) {
    if (const auto* chapter = dynamic_cast<ChapterMetadataEntry*>(entry.get())) {
      chapters.push_back(chapter);
    }
  }
  ASSERT_EQ(2u, chapters.size());
  EXPECT_EQ("Intro", chapters[0]->getTitle());
  EXPECT_EQ(0, chapters[0]->getTimeUs());
  EXPECT_EQ("Part two", chapters[1]->getTitle());
  EXPECT_EQ(1'000'000, chapters[1]->getTimeUs());
}

TEST(MetadataContainerTest, ReadsTheVendorOfAnOpusOggStream) {
  const auto media = coral::test::buildOggOpus(2, 4);
  MetadataHarness harness(std::vector<uint8_t>(media.begin(), media.end()));
  ASSERT_TRUE(harness.read());

  const auto& format = harness.format();
  ASSERT_NE(nullptr, format);
  const auto& metadata = format->getMetadata();
  ASSERT_NE(nullptr, metadata);
  ASSERT_NE(nullptr, findText(metadata, "vendor"));
  EXPECT_EQ("coral", *findText(metadata, "vendor"));
}
