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
 * @file test_DarwinHttpDataSource.cpp
 *
 * The tests of the http data source of macOS and iOS, i.e. of `NSURLSession` bound to the strategy of
 * `HttpDataSourceBase`. The suite serves the media from a socket of the test rather than opening a
 * media of a public server: the platform API is exercised end to end, and no test needs a network.
 */

#if defined(__APPLE__)

#include "TestData.h"

#include <datasource/HttpDataSource.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using coral::AVHttpDataSource;
using coral::createHttpDataSourceFactory;
using coral::test::makePattern;

namespace {

/**
 * A server of one media, i.e. a socket of the test that answers a range request with the bytes of the
 * media the request names. It is the counterpart of the fake server of the strategy test: here the
 * http API of the platform is the client, so the socket is the server.
 */
class LocalHttpServer final {
public:
  explicit LocalHttpServer(std::vector<uint8_t> media)
    : media_(std::move(media)) {
    listenSocket_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenSocket_ < 0) {
      return;
    }
    int reuse = 1;
    ::setsockopt(listenSocket_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(listenSocket_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(listenSocket_, 4) != 0) {
      ::close(listenSocket_);
      listenSocket_ = -1;
      return;
    }
    socklen_t length = sizeof(address);
    if (::getsockname(listenSocket_, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
      ::close(listenSocket_);
      listenSocket_ = -1;
      return;
    }
    port_ = ntohs(address.sin_port);
    serving_ = std::thread([this] { serve(); });
  }

  ~LocalHttpServer() {
    stopping_ = true;
    if (serving_.joinable()) {
      serving_.join();
    }
    if (listenSocket_ >= 0) {
      ::close(listenSocket_);
      listenSocket_ = -1;
    }
  }

  LocalHttpServer(const LocalHttpServer& other) = delete;

  LocalHttpServer& operator=(const LocalHttpServer& other) = delete;

  [[nodiscard]] bool isValid() const { return listenSocket_ >= 0; }

  [[nodiscard]] std::string url(const std::string& path = "media.bin") const {
    return "http://127.0.0.1:" + std::to_string(port_) + "/" + path;
  }

  [[nodiscard]] int requestCount() const { return requestCount_.load(); }

private:
  void serve() {
    while (!stopping_) {
      pollfd descriptor{};
      descriptor.fd = listenSocket_;
      descriptor.events = POLLIN;
      const int ready = ::poll(&descriptor, 1, 100);
      if (ready <= 0) {
        continue;
      }
      sockaddr_in client{};
      socklen_t length = sizeof(client);
      const int clientSocket = ::accept(listenSocket_, reinterpret_cast<sockaddr*>(&client), &length);
      if (clientSocket < 0) {
        if (stopping_) {
          return;
        }
        continue;
      }
      // A client that closes its side while the response is written must not signal the process, and
      // a connection that never sends its request must not block the server thread for ever.
      int noSignal = 1;
      ::setsockopt(clientSocket, SOL_SOCKET, SO_NOSIGPIPE, &noSignal, sizeof(noSignal));
      timeval timeout{};
      timeout.tv_sec = 2;
      ::setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
      handle(clientSocket);
      ::close(clientSocket);
    }
  }

  void handle(const int clientSocket) {
    std::string request;
    char buffer[4096];
    while (request.find("\r\n\r\n") == std::string::npos && request.size() < 64 * 1024) {
      const ssize_t received = ::recv(clientSocket, buffer, sizeof(buffer), 0);
      if (received <= 0) {
        return;
      }
      request.append(buffer, static_cast<size_t>(received));
    }
    requestCount_.fetch_add(1);

    size_t start = 0;
    size_t end = media_.empty() ? 0 : media_.size() - 1;
    const std::string rangeHeader = "Range: bytes=";
    const auto range = request.find(rangeHeader);
    if (range != std::string::npos) {
      const auto valueStart = range + rangeHeader.size();
      const auto dash = request.find('-', valueStart);
      const auto lineEnd = request.find("\r\n", valueStart);
      if (dash != std::string::npos && lineEnd != std::string::npos && dash > valueStart) {
        start = static_cast<size_t>(std::strtoull(request.substr(valueStart, dash - valueStart).c_str(),
          nullptr, 10));
        if (dash + 1 < lineEnd) {
          end = static_cast<size_t>(std::strtoull(
            request.substr(dash + 1, lineEnd - dash - 1).c_str(), nullptr, 10));
        }
      }
    }

    if (media_.empty() || start >= media_.size()) {
      const std::string response =
        "HTTP/1.1 416 Range Not Satisfiable\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
      sendAll(clientSocket, response.data(), response.size());
      return;
    }
    end = std::min(end, media_.size() - 1);
    const size_t count = end - start + 1;
    const std::string header =
      "HTTP/1.1 206 Partial Content\r\n"
      "Content-Type: application/octet-stream\r\n"
      "Accept-Ranges: bytes\r\n"
      "Content-Range: bytes " + std::to_string(start) + "-" + std::to_string(end) + "/" +
        std::to_string(media_.size()) + "\r\n"
      "Content-Length: " + std::to_string(count) + "\r\n"
      "Connection: close\r\n\r\n";
    if (sendAll(clientSocket, header.data(), header.size())) {
      sendAll(clientSocket, media_.data() + start, count);
    }
  }

  static bool sendAll(const int socket, const void* data, const size_t length) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    size_t sent = 0;
    while (sent < length) {
      const ssize_t written = ::send(socket, bytes + sent, length - sent, 0);
      if (written <= 0) {
        return false;
      }
      sent += static_cast<size_t>(written);
    }
    return true;
  }

  std::vector<uint8_t> media_;
  int listenSocket_ = -1;
  uint16_t port_ = 0;
  std::atomic<bool> stopping_{ false };
  std::atomic<int> requestCount_{ 0 };
  std::thread serving_;
};

} // namespace anonymous

TEST(DarwinHttpDataSourceTest, ReadsAMediaOfTheServer) {
  const auto media = makePattern(64 * 1024);
  LocalHttpServer server(media);
  ASSERT_TRUE(server.isValid());

  auto factory = createHttpDataSourceFactory();
  ASSERT_NE(nullptr, factory);
  auto source = factory->create(AVHttpDataSource::Parameters());
  ASSERT_NE(nullptr, source);

  const auto uri = server.url();
  ASSERT_TRUE(source->open(uri)) << uri;
  EXPECT_TRUE(source->isOpen());
  EXPECT_EQ(uri, source->getUri());
  EXPECT_EQ(static_cast<long>(media.size()), source->getLength());
  EXPECT_EQ(206, source->getResponseCode());

  std::vector<uint8_t> buffer(256);
  ASSERT_EQ(256, source->readAt(0, buffer.data(), 0, buffer.size()));
  EXPECT_TRUE(std::equal(buffer.begin(), buffer.end(), media.begin()));

  // The position inside the window of the request is served from the bytes that were read ahead.
  ASSERT_EQ(128, source->readAt(1000, buffer.data(), 0, 128));
  for (int i = 0; i < 128; ++i) {
    EXPECT_EQ(media[1000 + static_cast<size_t>(i)], buffer[static_cast<size_t>(i)]);
  }
  EXPECT_EQ(1, server.requestCount());

  source->close();
  EXPECT_FALSE(source->isOpen());
}

TEST(DarwinHttpDataSourceTest, OpensARequestForAPositionBeyondTheWindow) {
  // The media is larger than the one megabyte window the reads are served from, so a read beyond the
  // window makes the strategy open a request of its own.
  const auto media = makePattern(2 * 1024 * 1024 + 123);
  LocalHttpServer server(media);
  ASSERT_TRUE(server.isValid());

  auto factory = createHttpDataSourceFactory();
  ASSERT_NE(nullptr, factory);
  auto source = factory->create(AVHttpDataSource::Parameters());
  ASSERT_NE(nullptr, source);
  ASSERT_TRUE(source->open(server.url()));
  EXPECT_EQ(static_cast<long>(media.size()), source->getLength());

  std::vector<uint8_t> buffer(64);
  ASSERT_EQ(64, source->readAt(0, buffer.data(), 0, buffer.size()));
  EXPECT_TRUE(std::equal(buffer.begin(), buffer.end(), media.begin()));

  const long position = 1536 * 1024;
  ASSERT_EQ(64, source->readAt(position, buffer.data(), 0, buffer.size()));
  for (int i = 0; i < 64; ++i) {
    EXPECT_EQ(media[static_cast<size_t>(position) + static_cast<size_t>(i)],
      buffer[static_cast<size_t>(i)]);
  }
  EXPECT_GE(server.requestCount(), 2);
  source->close();
}

TEST(DarwinHttpDataSourceTest, FailsToOpenAMediaTheServerDoesNotServe) {
  auto factory = createHttpDataSourceFactory();
  ASSERT_NE(nullptr, factory);
  auto source = factory->create(AVHttpDataSource::Parameters());
  ASSERT_NE(nullptr, source);

  // A request to a port that no server listens on fails before a response is available.
  EXPECT_FALSE(source->open("http://127.0.0.1:1/media.bin"));
  EXPECT_FALSE(source->isOpen());
  EXPECT_EQ(-1, source->getLength());
  EXPECT_EQ(0, source->getResponseCode());
}

#endif // defined(__APPLE__)
