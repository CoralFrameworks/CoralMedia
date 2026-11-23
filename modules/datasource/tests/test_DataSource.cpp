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

#if defined(__APPLE__)
#include <datasource/DarwinDataSource.h>

#include <CoreFoundation/CoreFoundation.h>

#include <climits>
#include <cstdio>
#include <fstream>
#endif

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
#if defined(__APPLE__)
using coral::AVBundleDataSource;
using coral::AVDarwinFileDataSource;
#endif
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

#if defined(__APPLE__)

namespace {

/** The directory the resources of the test are looked up in, i.e. the resources of the main bundle. */
std::string bundleResourceDirectory() {
  CFBundleRef bundle = CFBundleGetMainBundle();
  CFURLRef url = CFBundleCopyResourcesDirectoryURL(bundle);
  char buffer[PATH_MAX] = {};
  const bool ok = url != nullptr &&
                  CFURLGetFileSystemRepresentation(url, true,
                    reinterpret_cast<UInt8*>(buffer), sizeof(buffer)) != 0;
  if (url != nullptr) {
    CFRelease(url);
  }
  return ok ? std::string(buffer) : std::string();
}

void writeFileAt(const std::filesystem::path& path, const std::vector<uint8_t>& data) {
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(reinterpret_cast<const char*>(data.data()),
    static_cast<std::streamsize>(data.size()));
}

/** The percent-encoded file url of a path, as the platform writes one. */
std::string fileUrlOf(const std::filesystem::path& path) {
  const std::string text = path.string();
  CFURLRef url = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault,
    reinterpret_cast<const UInt8*>(text.c_str()), text.size(), false);
  CFStringRef string = url != nullptr ? CFURLGetString(url) : nullptr;
  char buffer[PATH_MAX * 4] = {};
  // CFURLGetString returns a borrowed string; only the url is owned and released here.
  const bool ok = string != nullptr &&
                  CFStringGetCString(string, buffer, sizeof(buffer), kCFStringEncodingUTF8) != 0;
  if (url != nullptr) {
    CFRelease(url);
  }
  return ok ? std::string(buffer) : std::string();
}

/** Removes a path of the bundle when the test ends, so that the next run starts from no resource. */
struct BundleFixtureCleanup {
  std::filesystem::path path;

  ~BundleFixtureCleanup() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
};

} // namespace anonymous

TEST(AVDarwinFileDataSourceTest, ReadsAFileUriWithAPercentEncodedPath) {
  const auto data = makePattern();
  const auto path = std::filesystem::temp_directory_path() / "coral darwin 中文.bin";
  writeFileAt(path, data);
  const auto uri = fileUrlOf(path);
  ASSERT_NE(std::string(), uri);
  EXPECT_NE(std::string::npos, uri.find('%')) << "the url of a path with a space is percent-encoded";

  AVDarwinFileDataSource source;
  ASSERT_TRUE(source.open(uri)) << uri;
  EXPECT_TRUE(source.isOpen());
  EXPECT_EQ(uri, source.getUri());
  EXPECT_EQ(static_cast<long>(data.size()), source.getLength());
  std::vector<uint8_t> buffer(64);
  ASSERT_EQ(64, source.readAt(0, buffer.data(), 0, buffer.size()));
  EXPECT_TRUE(std::equal(buffer.begin(), buffer.end(), data.begin()));
  source.close();
  EXPECT_FALSE(source.isOpen());
  EXPECT_EQ(-1, source.getLength());

  // The default data source reads the same url through the data source of the platform.
  AVDefaultDataSource defaultSource;
  ASSERT_TRUE(defaultSource.open(uri)) << uri;
  ASSERT_EQ(64, defaultSource.readAt(0, buffer.data(), 0, buffer.size()));
  EXPECT_TRUE(std::equal(buffer.begin(), buffer.end(), data.begin()));
  defaultSource.close();

  std::error_code error;
  std::filesystem::remove(path, error);
}

TEST(AVDarwinFileDataSourceTest, ReadsAFileUriWithTheLocalhostAuthority) {
  const auto data = makePattern();
  const auto path = std::filesystem::temp_directory_path() / "coral-darwin-localhost.bin";
  writeFileAt(path, data);
  const std::string uri = "file://localhost" + path.string();

  AVDefaultDataSource source;
  ASSERT_TRUE(source.open(uri)) << uri;
  EXPECT_EQ(static_cast<long>(data.size()), source.getLength());
  std::vector<uint8_t> buffer(16);
  ASSERT_EQ(16, source.readAt(16, buffer.data(), 0, buffer.size()));
  for (int i = 0; i < 16; ++i) {
    EXPECT_EQ(static_cast<uint8_t>((16 + i) % 256), buffer[static_cast<size_t>(i)]);
  }
  source.close();

  std::error_code error;
  std::filesystem::remove(path, error);
}

TEST(AVDarwinFileDataSourceTest, ReportsAnUnreadableUri) {
  AVDarwinFileDataSource source;
  EXPECT_FALSE(source.open("file:///coral/does-not-exist.bin"));
  EXPECT_FALSE(source.isOpen());
  EXPECT_EQ(-1, source.getLength());

  AVDefaultDataSource defaultSource;
  EXPECT_FALSE(defaultSource.open("file:///coral/does-not-exist.bin"));
  EXPECT_FALSE(defaultSource.isOpen());
}

TEST(AVBundleDataSourceTest, ReadsAnAssetOfTheMainBundle) {
  const auto directory = bundleResourceDirectory();
  ASSERT_FALSE(directory.empty());
  const auto data = makePattern();
  const auto path = std::filesystem::path(directory) / "coral-darwin-asset.bin";
  BundleFixtureCleanup cleanup{ path };
  writeFileAt(path, data);

  {
    AVBundleDataSource source;
    ASSERT_TRUE(source.open("asset:///coral-darwin-asset.bin")) << directory;
    EXPECT_TRUE(source.isOpen());
    EXPECT_EQ("asset:///coral-darwin-asset.bin", source.getUri());
    EXPECT_EQ(static_cast<long>(data.size()), source.getLength());
    std::vector<uint8_t> buffer(32);
    ASSERT_EQ(32, source.readAt(0, buffer.data(), 0, buffer.size()));
    EXPECT_TRUE(std::equal(buffer.begin(), buffer.end(), data.begin()));
    source.close();
    EXPECT_FALSE(source.isOpen());
  }

  // The default data source dispatches the scheme, and the same resource is readable through the
  // explicit alias of the scheme and through the Android spelling of an asset.
  for (const std::string uri : { std::string("asset:///coral-darwin-asset.bin"),
                                 std::string("bundle:///coral-darwin-asset.bin"),
                                 std::string("file:///android_asset/coral-darwin-asset.bin") }) {
    AVDefaultDataSource source;
    ASSERT_TRUE(source.open(uri)) << uri;
    EXPECT_EQ(static_cast<long>(data.size()), source.getLength());
    std::vector<uint8_t> buffer(32);
    ASSERT_EQ(32, source.readAt(0, buffer.data(), 0, buffer.size()));
    EXPECT_TRUE(std::equal(buffer.begin(), buffer.end(), data.begin()));
    source.close();
  }
}

TEST(AVBundleDataSourceTest, ReadsAnAssetNamedWithAnAuthorityAndAPercentEncodedPath) {
  const auto directory = bundleResourceDirectory();
  ASSERT_FALSE(directory.empty());
  const auto data = makePattern();
  const auto path = std::filesystem::path(directory) / "coral-darwin-assets" / "coral asset 中.bin";
  BundleFixtureCleanup cleanup{ std::filesystem::path(directory) / "coral-darwin-assets" };
  writeFileAt(path, data);

  AVBundleDataSource source;
  ASSERT_TRUE(source.open("asset://coral-darwin-assets/coral%20asset%20%E4%B8%AD.bin"));
  EXPECT_EQ(static_cast<long>(data.size()), source.getLength());
  std::vector<uint8_t> buffer(16);
  ASSERT_EQ(16, source.readAt(8, buffer.data(), 0, buffer.size()));
  for (int i = 0; i < 16; ++i) {
    EXPECT_EQ(static_cast<uint8_t>((8 + i) % 256), buffer[static_cast<size_t>(i)]);
  }
  source.close();
}

TEST(AVBundleDataSourceTest, RefusesAPathThatLeavesTheBundle) {
  AVBundleDataSource source;
  EXPECT_FALSE(source.open("asset:///../coral-darwin-outside.bin"));
  EXPECT_FALSE(source.isOpen());

  AVDefaultDataSource defaultSource;
  EXPECT_FALSE(defaultSource.open("asset:///../coral-darwin-outside.bin"));
}

TEST(AVBundleDataSourceTest, FailsForAMissingAsset) {
  AVBundleDataSource source;
  EXPECT_FALSE(source.open("asset:///coral-darwin-does-not-exist.bin"));
  EXPECT_FALSE(source.isOpen());
  EXPECT_EQ(-1, source.getLength());
}

#endif // defined(__APPLE__)

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
