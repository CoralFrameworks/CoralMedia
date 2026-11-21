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

/**
 * @file test_HttpDataSourceBase.cpp
 *
 * The tests of the strategy of the http reads, i.e. the part of the http data source that every
 * platform shares: the range requests, the window the bytes are read ahead into, and the
 * interpretation of the response.
 *
 * The suite does not need a server: it binds the strategy to a fake one, which makes the number of the
 * requests, and the bytes they are asked for, observable.
 */

#include "HttpDataSourceBase.h"
#include "TestData.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using coral::test::makePattern;

namespace {

/** The number of bytes the fake server serves for one request, i.e. the size of its range. */
constexpr size_t kRangeLength = 64 * 1024;

/**
 * A server that serves a buffer of bytes: it honors a range request for the number of bytes it decides
 * to serve, and it reports the length of its media in the way the http protocol does.
 */
class FakeServer {
public:
  explicit FakeServer(std::vector<uint8_t> media, const size_t rangeLength = kRangeLength)
    : media_(std::move(media)),
      rangeLength_(rangeLength) {
  }

  FakeServer(const FakeServer& other) = delete;

  FakeServer& operator=(const FakeServer& other) = delete;

  /** The status code every response reports. */
  int statusCode = 206;

  /** Whether sending a request fails, e.g. because the server is not reachable. */
  bool requestFails = false;

  /** Whether opening the connection of the platform fails, e.g. because the url is unusable. */
  bool connectFails = false;

  /** The number of requests that were sent. */
  int requestCount = 0;

  /** The headers of the last request that was sent. */
  std::string lastHeaders;

  /** The number of requests that are open. */
  int openRequestCount = 0;

  [[nodiscard]] long length() const { return static_cast<long>(media_.size()); }

  [[nodiscard]] const std::vector<uint8_t>& media() const { return media_; }

  [[nodiscard]] size_t rangeLength() const { return rangeLength_; }

private:
  std::vector<uint8_t> media_;
  size_t rangeLength_;
};

/** One request of the fake server, i.e. the connection the strategy of the reads makes. */
class FakeConnection final : public coral::HttpDataSourceBase::Connection {
public:
  explicit FakeConnection(FakeServer& server) : server_(server) {
  }

  ~FakeConnection() override {
    if (requestOpen_) {
      --server_.openRequestCount;
    }
  }

  FakeConnection(const FakeConnection& other) = delete;

  FakeConnection& operator=(const FakeConnection& other) = delete;

  bool openRequest(const long position, const std::string& headers) override {
    ++server_.requestCount;
    server_.lastHeaders = headers;
    if (server_.requestFails) {
      return false;
    }
    closeRequest();
    requestOpen_ = true;
    ++server_.openRequestCount;
    position_ = position;
    served_ = 0;
    return true;
  }

  long read(uint8_t* buffer, const size_t length) override {
    if (!requestOpen_) {
      return -1;
    }
    const long remaining = server_.length() - position_ - static_cast<long>(served_);
    if (remaining <= 0) {
      return 0;
    }
    // The server never serves more than the range it agreed to, and never more than the media holds.
    size_t toRead = std::min(static_cast<size_t>(remaining), length);
    toRead = std::min(toRead, server_.rangeLength() - served_);
    if (toRead == 0) {
      return 0;
    }
    std::memcpy(buffer, server_.media().data() + position_ + served_, toRead);
    served_ += toRead;
    return static_cast<long>(toRead);
  }

  void closeRequest() override {
    if (requestOpen_) {
      requestOpen_ = false;
      --server_.openRequestCount;
    }
  }

  [[nodiscard]] int getResponseCode() const override { return server_.statusCode; }

  [[nodiscard]] std::string getResponseHeader(const std::string& name) const override {
    if (name == "Content-Range") {
      return "bytes 0-" + std::to_string(server_.length() - 1) + "/" + std::to_string(server_.length());
    }
    if (name == "Content-Length") {
      return std::to_string(server_.length());
    }
    return std::string();
  }

private:
  FakeServer& server_;
  bool requestOpen_ = false;
  long position_ = 0;
  size_t served_ = 0;
};

/** The http data source of a fake server, i.e. the strategy of the reads bound to one. */
class FakeHttpDataSource final : public coral::HttpDataSourceBase {
public:
  FakeHttpDataSource(FakeServer& server, coral::AVHttpDataSource::Parameters parameters)
    : HttpDataSourceBase(std::move(parameters)),
      server_(server) {
  }

  [[nodiscard]] const coral::AVHttpDataSource::Parameters& parameters() const { return getParameters(); }

protected:
  [[nodiscard]] std::unique_ptr<Connection> createConnection(const std::string& uri) override {
    static_cast<void>(uri);
    if (server_.connectFails) {
      return nullptr;
    }
    return std::make_unique<FakeConnection>(server_);
  }

private:
  FakeServer& server_;
};

/** Reads the bytes of a media at a position, and returns them, for the assertions of the tests. */
std::vector<uint8_t> readAt(coral::AVHttpDataSource& source, const long position, const size_t length) {
  std::vector<uint8_t> buffer(length);
  const int read = source.readAt(position, buffer.data(), 0, length);
  if (read < 0) {
    return std::vector<uint8_t>();
  }
  buffer.resize(static_cast<size_t>(read));
  return buffer;
}

} // namespace anonymous

TEST(HttpDataSourceBaseTest, ReportsTheLengthAndTheContentOfAMedia) {
  const auto media = makePattern(256 * 1024);
  FakeServer server(media);
  FakeHttpDataSource source(server, {});

  ASSERT_TRUE(source.open("http://example.com/media.bin"));
  EXPECT_TRUE(source.isOpen());
  EXPECT_EQ("http://example.com/media.bin", source.getUri());
  EXPECT_EQ(server.length(), source.getLength());
  EXPECT_EQ(206, source.getResponseCode());
  EXPECT_EQ(1, server.requestCount);

  // The request that the open made reports the length, and it reads the media of the first read too.
  const auto first = readAt(source, 0, 16);
  ASSERT_EQ(16u, first.size());
  EXPECT_TRUE(std::equal(first.begin(), first.end(), media.begin()));
  EXPECT_EQ(1, server.requestCount);

  // The bytes of the window are served without asking the server again.
  const auto inside = readAt(source, 1000, 8);
  ASSERT_EQ(8u, inside.size());
  for (int i = 0; i < 8; ++i) {
    EXPECT_EQ(media[1000 + i], inside[static_cast<size_t>(i)]);
  }
  EXPECT_EQ(1, server.requestCount);
}

TEST(HttpDataSourceBaseTest, OpensARequestAtEveryPositionTheWindowDoesNotHold) {
  const auto media = makePattern(256 * 1024);
  FakeServer server(media);
  FakeHttpDataSource source(server, {});
  ASSERT_TRUE(source.open("http://example.com/media.bin"));

  // The request of the open serves the first range, i.e. 64 KiB of the media.
  ASSERT_EQ(16u, readAt(source, 0, 16).size());
  EXPECT_EQ(1, server.requestCount);

  // The position that follows the range of the request that is open is read by a new one.
  const auto follows = readAt(source, static_cast<long>(kRangeLength), 8);
  ASSERT_EQ(8u, follows.size());
  EXPECT_EQ(media[static_cast<size_t>(kRangeLength)], follows[0]);
  EXPECT_EQ(2, server.requestCount);

  // A seek that the window does not hold is read by a request that starts there.
  const auto seeked = readAt(source, 200 * 1024, 8);
  ASSERT_EQ(8u, seeked.size());
  EXPECT_EQ(media[200 * 1024], seeked[0]);
  EXPECT_EQ(3, server.requestCount);

  // A seek back into the range of a request that was given up is read by a new one.
  ASSERT_EQ(8u, readAt(source, 100 * 1024, 8).size());
  EXPECT_EQ(4, server.requestCount);
  EXPECT_EQ(1, server.openRequestCount);
}

TEST(HttpDataSourceBaseTest, ReadsTheEndOfAMediaAndRefusesThePositionsBeyondIt) {
  const auto media = makePattern(256 * 1024);
  FakeServer server(media);
  FakeHttpDataSource source(server, {});
  ASSERT_TRUE(source.open("http://example.com/media.bin"));

  // A read that overlaps the end reports the bytes that are left.
  const auto tail = readAt(source, server.length() - 4, 16);
  ASSERT_EQ(4u, tail.size());
  EXPECT_EQ(media[static_cast<size_t>(server.length()) - 4], tail[0]);

  uint8_t buffer[8] = {};
  EXPECT_EQ(0, source.readAt(server.length(), buffer, 0, sizeof(buffer)));
  EXPECT_EQ(0, source.readAt(server.length() + 1000, buffer, 0, sizeof(buffer)));
  EXPECT_EQ(0, source.readAt(0, buffer, 0, 0));
  EXPECT_EQ(-1, source.readAt(-1, buffer, 0, sizeof(buffer)));
  EXPECT_EQ(-1, source.readAt(0, nullptr, 0, sizeof(buffer)));
}

TEST(HttpDataSourceBaseTest, SendsTheRangeAndThePropertiesOfTheRequests) {
  FakeServer server(makePattern(256 * 1024));
  coral::AVHttpDataSource::Parameters parameters;
  parameters.connectTimeoutMs = 1000;
  parameters.readTimeoutMs = 2000;
  parameters.requestProperties.emplace_back("Authorization", "Bearer coral");
  FakeHttpDataSource source(server, parameters);

  ASSERT_TRUE(source.open("http://example.com/media.bin"));
  EXPECT_EQ(1000, source.parameters().connectTimeoutMs);
  EXPECT_EQ(2000, source.parameters().readTimeoutMs);
  EXPECT_EQ(1u, source.parameters().requestProperties.size());

  // Every request carries the range it reads, the encoding that keeps a server from compressing what
  // it sends, and the properties of the parameters.
  EXPECT_EQ(0u, server.lastHeaders.find("Range: bytes=0-"));
  EXPECT_NE(std::string::npos, server.lastHeaders.find("Accept-Encoding: identity"));
  EXPECT_NE(std::string::npos, server.lastHeaders.find("Authorization: Bearer coral"));

  // A request that starts at another position carries the range of that position.
  ASSERT_EQ(8u, readAt(source, 100 * 1024, 8).size());
  EXPECT_EQ(0u, server.lastHeaders.find("Range: bytes=102400-"));
}

TEST(HttpDataSourceBaseTest, ReadsAMediaThatAServerDoesNotRange) {
  const auto media = makePattern(256 * 1024);
  FakeServer server(media);
  // A server that ignores the range request answers with the whole media, which is only usable when
  // it was asked from its beginning.
  server.statusCode = 200;
  FakeHttpDataSource source(server, {});

  ASSERT_TRUE(source.open("http://example.com/media.bin"));
  EXPECT_EQ(200, source.getResponseCode());
  // The length of the media is the one of the whole response, i.e. of its Content-Length.
  EXPECT_EQ(server.length(), source.getLength());
  ASSERT_EQ(16u, readAt(source, 0, 16).size());

  // A position the open did not read cannot be served by such a server.
  uint8_t buffer[8] = {};
  EXPECT_EQ(-1, source.readAt(static_cast<long>(kRangeLength), buffer, 0, sizeof(buffer)));
}

TEST(HttpDataSourceBaseTest, FailsToOpenAMediaWhenTheServerAnswersAnError) {
  FakeServer server(makePattern(32 * 1024));
  server.statusCode = 404;
  FakeHttpDataSource source(server, {});

  EXPECT_FALSE(source.open("http://example.com/media.bin"));
  EXPECT_FALSE(source.isOpen());
  EXPECT_EQ(-1, source.getLength());
  EXPECT_EQ(0, source.getResponseCode());
  EXPECT_EQ(0, server.openRequestCount);
}

TEST(HttpDataSourceBaseTest, FailsToOpenAMediaWhenTheConnectionFails) {
  FakeServer server(makePattern(32 * 1024));
  server.connectFails = true;
  FakeHttpDataSource source(server, {});

  EXPECT_FALSE(source.open("not-a-url"));
  EXPECT_FALSE(source.isOpen());
  EXPECT_EQ(0, server.requestCount);
}

TEST(HttpDataSourceBaseTest, FailsToOpenAMediaWhenTheRequestFails) {
  FakeServer server(makePattern(32 * 1024));
  server.requestFails = true;
  FakeHttpDataSource source(server, {});

  EXPECT_FALSE(source.open("http://example.com/media.bin"));
  EXPECT_FALSE(source.isOpen());
  EXPECT_EQ(0, server.openRequestCount);
}

TEST(HttpDataSourceBaseTest, ClosesTheRequestOfAMediaThatIsClosed) {
  FakeServer server(makePattern(32 * 1024));
  FakeHttpDataSource source(server, {});
  ASSERT_TRUE(source.open("http://example.com/media.bin"));
  ASSERT_EQ(8u, readAt(source, 0, 8).size());
  EXPECT_EQ(1, server.openRequestCount);

  source.close();
  EXPECT_FALSE(source.isOpen());
  EXPECT_EQ(0, server.openRequestCount);

  uint8_t buffer[8] = {};
  EXPECT_EQ(-1, source.readAt(0, buffer, 0, sizeof(buffer)));

  // Closing is idempotent, because a destructor calls it after a caller already did.
  source.close();
  EXPECT_FALSE(source.isOpen());
}
