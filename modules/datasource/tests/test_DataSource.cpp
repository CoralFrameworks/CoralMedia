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

#include "TestData.h"

#include <datasource/DataSource.h>
#include <datasource/HttpDataSource.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using coral::AVByteArrayDataSource;
using coral::AVDefaultDataSource;
using coral::AVFileDataSource;
using coral::AVHttpDataSource;
using coral::AVHttpDataSourceFactory;
using coral::test::makePattern;
using coral::test::writeTemporaryFile;

namespace {

std::shared_ptr<AVByteArrayDataSource> openByteArraySource(const std::vector<uint8_t>& data) {
  auto source = std::make_shared<AVByteArrayDataSource>(data);
  source->open("memory");
  return source;
}

/**
 * An http data source that serves a buffer rather than a server, so that the delegation of the
 * default data source to the http data source of its factory is tested without a network.
 */
class BufferHttpDataSource final : public AVHttpDataSource {
public:
  explicit BufferHttpDataSource(std::vector<uint8_t> data) : data_(std::move(data)) {
  }

  bool open(const std::string& uri) override {
    uri_ = uri;
    open_ = true;
    return true;
  }

  int readAt(const long position, uint8_t* buffer, const size_t offset, const size_t length) override {
    if (!open_ || buffer == nullptr || position < 0) {
      return -1;
    }
    const auto total = static_cast<long>(data_.size());
    if (length == 0 || position >= total) {
      return 0;
    }
    const size_t available = std::min(static_cast<size_t>(total - position), length);
    std::memcpy(buffer + offset, data_.data() + position, available);
    return static_cast<int>(available);
  }

  [[nodiscard]] long getLength() const override { return static_cast<long>(data_.size()); }

  [[nodiscard]] const std::string& getUri() const override { return uri_; }

  void close() override { open_ = false; }

  [[nodiscard]] bool isOpen() const override { return open_; }

  [[nodiscard]] int getResponseCode() const override { return 206; }

private:
  std::vector<uint8_t> data_;
  std::string uri_;
  bool open_ = false;
};

/** The factory of {@link BufferHttpDataSource}, which records what it was asked to create. */
class BufferHttpDataSourceFactory final : public AVHttpDataSourceFactory {
public:
  explicit BufferHttpDataSourceFactory(std::vector<uint8_t> data) : data_(std::move(data)) {
  }

  [[nodiscard]] std::shared_ptr<AVHttpDataSource> create(
    AVHttpDataSource::Parameters parameters) override {
    ++createCount;
    parameters_ = std::move(parameters);
    lastCreated = std::make_shared<BufferHttpDataSource>(data_);
    return lastCreated;
  }

  int createCount = 0;
  AVHttpDataSource::Parameters parameters_;
  std::shared_ptr<BufferHttpDataSource> lastCreated;

private:
  std::vector<uint8_t> data_;
};

/** The factory of a platform that does not implement an http data source. */
class UnimplementedHttpDataSourceFactory final : public AVHttpDataSourceFactory {
public:
  [[nodiscard]] std::shared_ptr<AVHttpDataSource> create(
    AVHttpDataSource::Parameters parameters) override {
    static_cast<void>(parameters);
    return nullptr;
  }
};

} // namespace anonymous

// ==================================================================================================
// AVFileDataSource
// ==================================================================================================

TEST(AVFileDataSourceTest, ReadsTheBytesOfAFile) {
  const auto data = makePattern();
  const auto path = writeTemporaryFile("coral-file-data-source.bin", data);

  AVFileDataSource source;
  ASSERT_TRUE(source.open(path)) << path;
  EXPECT_TRUE(source.isOpen());
  EXPECT_EQ(path, source.getUri());
  EXPECT_EQ(static_cast<long>(data.size()), source.getLength());

  std::vector<uint8_t> first(64);
  ASSERT_EQ(64, source.readAt(0, first.data(), 0, first.size()));
  EXPECT_TRUE(std::equal(first.begin(), first.end(), data.begin()));

  // The reads of a file are absolute, they do not follow each other.
  uint8_t byte = 0;
  ASSERT_EQ(1, source.readAt(300, &byte, 0, 1));
  EXPECT_EQ(static_cast<uint8_t>(300 % 256), byte);
  EXPECT_EQ(0, source.readAt(static_cast<long>(data.size()), &byte, 0, 1));

  source.close();
  EXPECT_FALSE(source.isOpen());
  EXPECT_EQ(-1, source.getLength());
  // Closing is idempotent, because a destructor calls it after a caller already did.
  source.close();
}

TEST(AVFileDataSourceTest, ReportsAnErrorWhenTheFileDoesNotExist) {
  AVFileDataSource source;
  EXPECT_FALSE(source.open("this-file-does-not-exist.bin"));
  EXPECT_FALSE(source.isOpen());
  EXPECT_EQ(-1, source.getLength());
}

TEST(AVFileDataSourceTest, ReportsAnErrorWhenItIsNotOpen) {
  AVFileDataSource source;
  uint8_t buffer = 0;
  EXPECT_EQ(-1, source.readAt(0, &buffer, 0, 1));
}

// ==================================================================================================
// AVByteArrayDataSource
// ==================================================================================================

TEST(AVByteArrayDataSourceTest, ReportsItsLengthAndContent) {
  const auto data = makePattern();
  const auto source = openByteArraySource(data);

  EXPECT_TRUE(source->isOpen());
  EXPECT_EQ(static_cast<long>(data.size()), source->getLength());
  EXPECT_EQ("memory", source->getUri());
  EXPECT_EQ(data, source->getData());
}

TEST(AVByteArrayDataSourceTest, ReadAtReturnsTheRequestedBytes) {
  const auto data = makePattern();
  const auto source = openByteArraySource(data);

  uint8_t buffer[8] = {};
  ASSERT_EQ(8, source->readAt(100, buffer, 0, sizeof(buffer)));
  for (int i = 0; i < 8; ++i) {
    EXPECT_EQ(static_cast<uint8_t>((100 + i) % 256), buffer[i]);
  }
}

TEST(AVByteArrayDataSourceTest, ReadAtTheEndReturnsNothing) {
  const auto data = makePattern();
  const auto source = openByteArraySource(data);

  uint8_t buffer[8] = {};
  EXPECT_EQ(0, source->readAt(static_cast<long>(data.size()), buffer, 0, sizeof(buffer)));
  EXPECT_EQ(0, source->readAt(1000, buffer, 0, sizeof(buffer)));
}

TEST(AVByteArrayDataSourceTest, ReadAtClampsTheLengthToTheAvailableBytes) {
  const auto data = makePattern();
  const auto source = openByteArraySource(data);

  uint8_t buffer[8] = {};
  EXPECT_EQ(4, source->readAt(508, buffer, 0, sizeof(buffer)));
}

TEST(AVByteArrayDataSourceTest, ReadAtWritesAtTheRequestedOffset) {
  const auto data = makePattern();
  const auto source = openByteArraySource(data);

  uint8_t buffer[4] = {};
  ASSERT_EQ(2, source->readAt(4, buffer, 2, 2));
  EXPECT_EQ(0, buffer[0]);
  EXPECT_EQ(0, buffer[1]);
  EXPECT_EQ(4, buffer[2]);
  EXPECT_EQ(5, buffer[3]);
}

// ==================================================================================================
// AVDefaultDataSource
// ==================================================================================================

TEST(AVDefaultDataSourceTest, ReadsAFileOfTheLocalFileSystem) {
  // A path of the platform holds a colon after its drive letter, which is not the scheme of a uri.
  const auto data = makePattern();
  const auto path = writeTemporaryFile("coral-default-data-source.bin", data);

  AVDefaultDataSource source;
  ASSERT_TRUE(source.open(path)) << path;
  EXPECT_TRUE(source.isOpen());
  EXPECT_EQ(static_cast<long>(data.size()), source.getLength());

  std::vector<uint8_t> first(64);
  ASSERT_EQ(64, source.readAt(0, first.data(), 0, first.size()));
  EXPECT_TRUE(std::equal(first.begin(), first.end(), data.begin()));
  source.close();
  EXPECT_FALSE(source.isOpen());
}

TEST(AVDefaultDataSourceTest, ReadsAFileUri) {
  const auto data = makePattern();
  const auto path = writeTemporaryFile("coral-default-data-source-uri.bin", data);
  // The file uri of the platform names the drive of the path, e.g. "file:///C:/media".
  std::string uri = "file:///" + path;
  for (char& character : uri) {
    if (character == '\\') {
      character = '/';
    }
  }

  AVDefaultDataSource source;
  ASSERT_TRUE(source.open(uri)) << uri;
  EXPECT_TRUE(source.isOpen());
  EXPECT_EQ(static_cast<long>(data.size()), source.getLength());
  source.close();
}

TEST(AVDefaultDataSourceTest, FailsToOpenAFileThatDoesNotExist) {
  AVDefaultDataSource source;
  const auto path = std::filesystem::temp_directory_path() / "coral-does-not-exist.bin";
  EXPECT_FALSE(source.open(path.string()));
  EXPECT_FALSE(source.isOpen());
  EXPECT_EQ(-1, source.getLength());
}

TEST(AVDefaultDataSourceTest, ReadsAnHttpUriWithTheHttpDataSourceOfItsFactory) {
  const auto data = makePattern();
  auto factory = std::make_unique<BufferHttpDataSourceFactory>(data);
  auto* const observer = factory.get();
  AVDefaultDataSource source(std::move(factory));

  ASSERT_TRUE(source.open("http://example.com/media.bin"));
  EXPECT_TRUE(source.isOpen());
  EXPECT_EQ(1, observer->createCount);
  EXPECT_EQ("http://example.com/media.bin", source.getUri());
  EXPECT_EQ(static_cast<long>(data.size()), source.getLength());
  ASSERT_NE(nullptr, observer->lastCreated);
  EXPECT_EQ("http://example.com/media.bin", observer->lastCreated->getUri());

  uint8_t buffer[8] = {};
  ASSERT_EQ(8, source.readAt(100, buffer, 0, sizeof(buffer)));
  for (int i = 0; i < 8; ++i) {
    EXPECT_EQ(static_cast<uint8_t>((100 + i) % 256), buffer[i]);
  }

  // Opening a uri releases the data source of the previous one, and creates a new one.
  ASSERT_TRUE(source.open("https://example.com/other.bin"));
  EXPECT_EQ(2, observer->createCount);
  ASSERT_NE(nullptr, observer->lastCreated);
  EXPECT_EQ("https://example.com/other.bin", observer->lastCreated->getUri());

  source.close();
  EXPECT_FALSE(source.isOpen());
}

TEST(AVDefaultDataSourceTest, FailsToOpenAnHttpUriWhenItsFactoryCreatesNoDataSource) {
  AVDefaultDataSource source(std::make_unique<UnimplementedHttpDataSourceFactory>());
  // An http uri is never read as a path of the local file system, not even by a platform that does
  // not implement the http data source: the failure is a load error.
  EXPECT_FALSE(source.open("http://example.com/media.bin"));
  EXPECT_FALSE(source.isOpen());
  EXPECT_EQ(-1, source.getLength());
}
