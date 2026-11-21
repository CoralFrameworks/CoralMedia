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

#ifndef CORAL_DATASOURCE_HTTP_DATA_SOURCE_H
#define CORAL_DATASOURCE_HTTP_DATA_SOURCE_H

#include <datasource/DataSource.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace coral {

/**
 * The interface of a data source that reads the media of an {@code http} or an {@code https} uri.
 *
 * <p>The counterpart of ExoPlayer's {@code HttpDataSource}. Only the interface is exposed by this
 * module: the implementation talks to the http API of the platform, which is not, therefore it lives
 * in the source tree of the module, one file per platform, and an application creates it with the
 * factory of {@link AVHttpDataSourceFactory}.
 *
 * <p>The bytes the extractor asks for are read with a range request, so that seeking into the media
 * reads the media that follows the target rather than the media that precedes it, and a read that
 * follows the previous one continues the request that is open instead of making a new one.
 *
 * <p>Implementations follow the RAII idiom: {@link open} acquires the connection and {@link close}
 * releases it, the destructor releases it too, and {@link close} is idempotent so that it can be called
 * from either.
 */
class AVHttpDataSource : public AVDataSource {
public:
  /** The time a connection, a request or a read waits before it is given up, in milliseconds. */
  static constexpr int64_t DEFAULT_TIMEOUT_MS = 8000;

  /**
   * The parameters of the requests the data source makes. They are what the factory of ExoPlayer's
   * {@code DefaultHttpDataSource} exposes through its setters.
   */
  struct Parameters {
    /** The timeout of the connection to the server, in milliseconds. */
    int64_t connectTimeoutMs = DEFAULT_TIMEOUT_MS;

    /** The timeout of a read from the server, in milliseconds. */
    int64_t readTimeoutMs = DEFAULT_TIMEOUT_MS;

    /** The headers that are sent with every request, e.g. {@code User-Agent} or a token. */
    std::vector<std::pair<std::string, std::string>> requestProperties;
  };

  ~AVHttpDataSource() override = default;

  /**
   * Returns the status code of the response of the request that is open, or 0 when none is.
   */
  [[nodiscard]] virtual int getResponseCode() const = 0;

protected:
  AVHttpDataSource() = default;
};

/**
 * The factory of the http data source, i.e. the counterpart of ExoPlayer's
 * {@code HttpDataSource.Factory}.
 *
 * <p>It creates the implementation of the platform, so that a caller neither names it nor knows which
 * http API it is built on. The platform of a build that does not implement one is created by a
 * factory that returns no data source, and every uri therefore fails to open, which a caller reports
 * as a load error.
 */
class AVHttpDataSourceFactory {
public:
  virtual ~AVHttpDataSourceFactory() = default;

  AVHttpDataSourceFactory(const AVHttpDataSourceFactory& other) = delete;

  AVHttpDataSourceFactory& operator=(const AVHttpDataSourceFactory& other) = delete;

  /**
   * Creates the http data source of the platform.
   *
   * @param parameters The parameters of the requests the data source makes.
   *
   * @return The data source, or {@code nullptr} when the platform of the build does not implement one.
   */
  [[nodiscard]] virtual std::shared_ptr<AVHttpDataSource> create(AVHttpDataSource::Parameters parameters) = 0;

protected:
  AVHttpDataSourceFactory() = default;
};

/**
 * Returns whether the platform of the build implements the http data source. The platforms that do
 * not implement it fail to open every uri, which a caller reports as a load error.
 */
[[nodiscard]] bool isHttpDataSourceSupported();

/**
 * Creates the factory of the http data source of the platform, i.e. the factory of the data source
 * that {@link AVDefaultDataSource} uses for an http or an https uri.
 */
[[nodiscard]] std::unique_ptr<AVHttpDataSourceFactory> createHttpDataSourceFactory();

} // namespace coral

#endif //CORAL_DATASOURCE_HTTP_DATA_SOURCE_H
