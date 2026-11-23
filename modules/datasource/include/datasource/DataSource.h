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

#ifndef CORAL_DATASOURCE_DATA_SOURCE_H
#define CORAL_DATASOURCE_DATA_SOURCE_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace coral {

class AVHttpDataSourceFactory;

/**
 * The datasource module counterpart of ExoPlayer's {@code DataSource}.
 *
 * <p>A data source provides random access to a stream of bytes. It is deliberately minimal: the
 * buffering, the caching and the position tracking are the responsibility of the
 * {@link AVExtractorInput} that wraps it.
 *
 * <p>Implementations follow the RAII idiom: {@link open} acquires the underlying resource and
 * {@link close} releases it, and {@link close} is idempotent so that it can safely be called from a
 * destructor.
 */
class AVDataSource {
public:
  virtual ~AVDataSource() = default;

  AVDataSource(const AVDataSource& other) = delete;

  AVDataSource& operator=(const AVDataSource& other) = delete;

  /**
   * Opens the data source.
   *
   * @param uri The uri of the media. The interpretation is left to the implementation.
   *
   * @return {@code true} on success.
   */
  virtual bool open(const std::string& uri) = 0;

  /**
   * Reads data at an absolute position.
   *
   * <p>The data source may return fewer bytes than requested, including zero, even when the end of
   * the stream was not reached. Callers that need a precise number of bytes must loop.
   *
   * @param position The absolute position to read from.
   * @param buffer The destination buffer.
   * @param offset The offset in {@code buffer} at which to start writing.
   * @param length The maximum number of bytes to read.
   *
   * @return The number of bytes read, or a negative value on error.
   */
  virtual int readAt(long position, uint8_t* buffer, size_t offset, size_t length) = 0;

  /**
   * Returns the length of the stream in bytes, or {@code -1} when unknown.
   */
  [[nodiscard]] virtual long getLength() const = 0;

  /**
   * Returns the uri of the media.
   */
  [[nodiscard]] virtual const std::string& getUri() const = 0;

  /**
   * Closes the data source and releases the underlying resource. Idempotent.
   */
  virtual void close() = 0;

  /**
   * Returns whether the data source is currently open.
   */
  [[nodiscard]] virtual bool isOpen() const = 0;

protected:
  AVDataSource() = default;
};

/**
 * A data source that reads from a file on the local file system.
 *
 * <p>The file handle is owned by the instance and is closed by {@link close} or by the destructor,
 * whichever comes first.
 */
class AVFileDataSource final : public AVDataSource {
public:
  AVFileDataSource() noexcept;

  ~AVFileDataSource() override;

  bool open(const std::string& uri) override;

  int readAt(long position, uint8_t* buffer, size_t offset, size_t length) override;

  [[nodiscard]] long getLength() const override { return length_; }

  [[nodiscard]] const std::string& getUri() const override { return uri_; }

  void close() override;

  [[nodiscard]] bool isOpen() const override { return handle_ != nullptr; }

private:
  // The underlying file handle is only ever touched by this class, it is hidden behind an
  // incomplete type so that the platform headers do not leak into the public interface.
  struct Handle;

  std::unique_ptr<Handle> handle_;
  std::string uri_;
  long length_ = -1;
};

/**
 * A data source that serves an in memory buffer.
 *
 * <p>Mainly useful for tests and for media that was already loaded, e.g. from an Android asset or
 * from a network response.
 */
class AVByteArrayDataSource final : public AVDataSource {
public:
  AVByteArrayDataSource() = default;

  explicit AVByteArrayDataSource(std::vector<uint8_t> data) noexcept
    : data_(std::move(data)) {
  }

  ~AVByteArrayDataSource() override = default;

  bool open(const std::string& uri) override;

  int readAt(long position, uint8_t* buffer, size_t offset, size_t length) override;

  [[nodiscard]] long getLength() const override {
    return static_cast<long>(data_.size());
  }

  [[nodiscard]] const std::string& getUri() const override { return uri_; }

  void close() override { open_ = false; }

  [[nodiscard]] bool isOpen() const override { return open_; }

  /**
   * Replaces the content of the data source. The data source must not be open.
   */
  void setData(std::vector<uint8_t> data) {
    data_ = std::move(data);
    length_ = static_cast<long>(data_.size());
  }

  [[nodiscard]] const std::vector<uint8_t>& getData() const { return data_; }

private:
  std::vector<uint8_t> data_;
  std::string uri_;
  long length_ = 0;
  bool open_ = false;
};

/**
 * A data source that reads a uri with the data source of its scheme.
 *
 * <p>The counterpart of ExoPlayer's {@code DefaultDataSource}: a uri of the local file system is read
 * with {@link AVFileDataSource}, and the media of an {@code http} or an {@code https} uri with the
 * http data source of the platform, so that an application does not have to know where the media it
 * plays is.
 *
 * <p>On Android the schemes of the platform are read with the data sources of
 * {@code AndroidDataSource.h} as well: an {@code asset} uri (and a {@code file} uri whose path starts
 * with {@code /android_asset/}), a {@code content} uri, and a {@code rawresource} or
 * {@code android.resource} uri. Those data sources read through the application context that the
 * Android binding registers with {@code jni::ApplicationContext}, therefore an asset or a content uri
 * is only readable once that context is registered.
 *
 * <p>On macOS and iOS the local media is read with the data sources of {@code DarwinDataSource.h}:
 * a plain path or a {@code file} uri (including a percent-encoded path, the default authority and a
 * url the system picker returned, whose security scope the data source starts), and an
 * {@code asset} or {@code bundle} uri of a resource of the main bundle. The Android spelling of a
 * bundled media, {@code file:///android_asset/...}, names the same resource on Apple, so a media
 * that is named once is playable on every platform. The media of the Photos library is not a stream
 * of bytes the system hands out: the application exports the asset, or hands over the file url the
 * system picker returned.
 *
 * <p>The data source of the scheme is created by {@link open} and owned by this instance, which releases
 * it when it is closed: the two follow the RAII idiom together.
 */
class AVDefaultDataSource final : public AVDataSource {
public:
  AVDefaultDataSource();

  /**
   * @param httpDataSourceFactory The factory the data source of an {@code http} or an {@code https}
   * uri is created with. The factory of the platform is used when none is provided, so that an
   * application only names one to read its media with the parameters of its choosing, e.g. with a
   * token or with another timeout.
   */
  explicit AVDefaultDataSource(std::unique_ptr<AVHttpDataSourceFactory> httpDataSourceFactory);

  ~AVDefaultDataSource() override;

  bool open(const std::string& uri) override;

  int readAt(long position, uint8_t* buffer, size_t offset, size_t length) override;

  [[nodiscard]] long getLength() const override;

  [[nodiscard]] const std::string& getUri() const override { return uri_; }

  void close() override;

  [[nodiscard]] bool isOpen() const override;

private:
  /**
   * The factory of the http data source of the platform, i.e. of the data source an {@code http} or
   * an {@code https} uri is read with. It is owned by this instance, which is the data source that
   * delegates to it.
   */
  std::unique_ptr<AVHttpDataSourceFactory> httpDataSourceFactory_;

  std::shared_ptr<AVDataSource> delegate_;
  std::string uri_;
};

/**
 * The counterpart of ExoPlayer's {@code DataSource.Factory}: it creates the data source of a uri.
 *
 * <p>A factory is called by the media period when it starts to read a media, on the thread that loads
 * it, and the data source it returns is owned by the period until it is released.
 */
using AVDataSourceFactory = std::function<std::shared_ptr<AVDataSource>(const std::string& uri)>;

/**
 * Returns the factory of {@link AVDefaultDataSource}, i.e. the factory of the data source that a player
 * uses when the application does not provide one.
 */
[[nodiscard]] AVDataSourceFactory createDefaultDataSourceFactory();

} // namespace coral

#endif //CORAL_DATASOURCE_DATA_SOURCE_H
