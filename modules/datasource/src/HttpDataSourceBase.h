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

#ifndef CORAL_DATASOURCE_HTTP_DATA_SOURCE_BASE_H
#define CORAL_DATASOURCE_HTTP_DATA_SOURCE_BASE_H

#include <datasource/HttpDataSource.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace coral {

/**
 * The part of an http data source that does not depend on the platform: the parameters of the
 * requests, the window of bytes the reads are served from, and the interpretation of the response of
 * a request.
 *
 * <p>A platform implements {@link Connection}, i.e. the handful of operations that talk to a server,
 * and {@link createConnection}, and leaves the strategy of the reads to this class. The split keeps
 * the implementation of a platform down to the calls of its http API, and it keeps the strategy from
 * being copied, and diverging, from one platform to the next.
 */
class HttpDataSourceBase : public AVHttpDataSource {
public:
  ~HttpDataSourceBase() override;

  bool open(const std::string& uri) final;

  int readAt(long position, uint8_t* buffer, size_t offset, size_t length) final;

  [[nodiscard]] long getLength() const final { return length_; }

  [[nodiscard]] const std::string& getUri() const final { return uri_; }

  void close() final;

  [[nodiscard]] bool isOpen() const final { return connection_ != nullptr; }

  [[nodiscard]] int getResponseCode() const final;

  /**
   * One request of a server, i.e. the operations a platform implements to read the media of a uri.
   *
   * <p>A request reads the media from the position it was opened at and nothing more: its response is
   * read to its end, which is what lets the connection to the server be used by the request that
   * follows. A request that is given up while its response is unread costs a connection, and the
   * latency of a connection is what makes a media of several megabytes arrive slowly.
   */
  class Connection {
  public:
    virtual ~Connection() = default;

    Connection(const Connection& other) = delete;

    Connection& operator=(const Connection& other) = delete;

    /**
     * Sends a request that reads the media from a position, and receives its response.
     *
     * @param position The absolute position the response reads from. The range of the request is part
     * of the headers, the position is passed so that a failure can be logged with it.
     * @param headers The headers of the request, each of them followed by CRLF and without the final
     * empty line.
     *
     * @return {@code true} when the server answered.
     */
    virtual bool openRequest(long position, const std::string& headers) = 0;

    /**
     * Reads the bytes of the response of the request that is open.
     *
     * @return The number of bytes that were read, which is 0 when the response is exhausted, or a
     * negative value on error.
     */
    virtual long read(uint8_t* buffer, size_t length) = 0;

    /** Releases the request that is open, and keeps the connection to the server. Idempotent. */
    virtual void closeRequest() = 0;

    /** Returns the status code of the response of the request that is open, or 0 when none is. */
    [[nodiscard]] virtual int getResponseCode() const = 0;

    /** Returns the value of a header of the response of the request that is open, or an empty string. */
    [[nodiscard]] virtual std::string getResponseHeader(const std::string& name) const = 0;

  protected:
    Connection() = default;
  };

protected:
  explicit HttpDataSourceBase(Parameters parameters) noexcept;

  /**
   * Opens the connection of the platform to the server of a uri.
   *
   * @return The connection, or {@code nullptr} when the uri is not one that the platform can read.
   */
  [[nodiscard]] virtual std::unique_ptr<Connection> createConnection(const std::string& uri) = 0;

  /** Returns the parameters of the requests, which the connection of the platform is created with. */
  [[nodiscard]] const Parameters& getParameters() const { return parameters_; }

private:
  /** Opens the request that reads the media from the given position. */
  bool openRequest(long position);

  /** Reads ahead from the request that is open into the window the reads are served from. */
  bool fillWindow(long position);

  /** Releases the request that is open, and keeps the connection to the server. */
  void closeRequest();

  std::unique_ptr<Connection> connection_;
  Parameters parameters_;
  std::string uri_;
  long length_ = -1;

  /** The bytes that were read ahead of the position the caller asked for. */
  std::vector<uint8_t> window_;

  /** The position of the first byte of the window. */
  long windowStart_ = 0;

  /** The number of bytes the window holds. */
  size_t windowLength_ = 0;

  /** Whether a request that still has a response to read is open. */
  bool requestOpen_ = false;

  /** The position the request that is open reads the media from. */
  long requestPosition_ = 0;
};

} // namespace coral

#endif // CORAL_DATASOURCE_HTTP_DATA_SOURCE_BASE_H
