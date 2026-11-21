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

#ifndef CORAL_DATASOURCE_ANDROID_DATA_SOURCE_H
#define CORAL_DATASOURCE_ANDROID_DATA_SOURCE_H

#include <datasource/DataSource.h>

#if defined(__ANDROID__)

#include <memory>
#include <string>

namespace coral {

/**
 * A data source that reads an asset of the APK, the counterpart of ExoPlayer's
 * {@code AssetDataSource}.
 *
 * <p>The supported uris are:
 * <ul>
 *   <li>{@code asset:///path/in/the/assets} (e.g. {@code asset:///video/sample.mp4});</li>
 *   <li>{@code file:///android_asset/path/in/the/assets} and {@code /android_asset/path/...}, which
 *   is the same thing under the uri of a file.</li>
 * </ul>
 *
 * <p>An asset that the APK stores uncompressed is read through its file descriptor, i.e. without
 * copying it and without a read position: a seek costs the platform nothing. An asset that is stored
 * compressed has no descriptor, and is read through the stream the platform opens for it, which a
 * backwards seek reopens.
 *
 * <p>The data source reads the assets of the application context that was registered with
 * {@code jni::ApplicationContext}; see {@code CoralContext} of the Android binding.
 */
class AVAssetDataSource final : public AVDataSource {
public:
  AVAssetDataSource();

  ~AVAssetDataSource() override;

  bool open(const std::string& uri) override;

  int readAt(long position, uint8_t* buffer, size_t offset, size_t length) override;

  [[nodiscard]] long getLength() const override;

  [[nodiscard]] const std::string& getUri() const override;

  void close() override;

  [[nodiscard]] bool isOpen() const override;

private:
  struct Impl;

  std::unique_ptr<Impl> impl_;
};

/**
 * A data source that reads a content uri, the counterpart of ExoPlayer's {@code ContentDataSource}.
 *
 * <p>The media is read through the descriptor the provider hands out, i.e. without copying it into
 * the cache of the application, which is what makes a media the picker returned playable as it is.
 * The descriptor must be seekable: a provider that serves the media from a pipe cannot be read at a
 * position, and opening it fails.
 *
 * <p>A media provider is asked for the media in its original format, so that the data source reads
 * what the content uri names rather than a transcoded copy of it.
 */
class AVContentDataSource final : public AVDataSource {
public:
  AVContentDataSource();

  ~AVContentDataSource() override;

  bool open(const std::string& uri) override;

  int readAt(long position, uint8_t* buffer, size_t offset, size_t length) override;

  [[nodiscard]] long getLength() const override;

  [[nodiscard]] const std::string& getUri() const override;

  void close() override;

  [[nodiscard]] bool isOpen() const override;

private:
  struct Impl;

  std::unique_ptr<Impl> impl_;
};

/**
 * A data source that reads a raw resource of the APK, the counterpart of ExoPlayer's
 * {@code RawResourceDataSource}.
 *
 * <p>The supported uris are:
 * <ul>
 *   <li>{@code android.resource://[package]/id}, where the package is optional and defaults to the
 *   package of the application;</li>
 *   <li>{@code android.resource://[package]/[type/]name}, where the type defaults to {@code raw};</li>
 *   <li>{@code rawresource:///id}, which is the deprecated form of the first uri.</li>
 * </ul>
 *
 * <p>A resource the APK stores compressed has no descriptor, and the data source refuses it rather
 * than inflating it into memory.
 */
class AVRawResourceDataSource final : public AVDataSource {
public:
  AVRawResourceDataSource();

  ~AVRawResourceDataSource() override;

  bool open(const std::string& uri) override;

  int readAt(long position, uint8_t* buffer, size_t offset, size_t length) override;

  [[nodiscard]] long getLength() const override;

  [[nodiscard]] const std::string& getUri() const override;

  void close() override;

  [[nodiscard]] bool isOpen() const override;

private:
  struct Impl;

  std::unique_ptr<Impl> impl_;
};

/**
 * A data source that reads a region of a file descriptor, the counterpart of ExoPlayer's
 * {@code FileDescriptorDataSource}.
 *
 * <p>The platform hands out the media of an asset, of a raw resource and of a content uri as the
 * descriptor of a file and the offset and length of a region of it; this data source reads that
 * region, which is what the three data sources above are built on.
 *
 * <p>The descriptor is read with {@code pread}, which leaves the file position of the descriptor to
 * its owner, and it is neither seeked nor closed by the data source: a descriptor is shared with the
 * object that handed it out.
 */
class AVFileDescriptorDataSource final : public AVDataSource {
public:
  /**
   * @param fileDescriptor The descriptor to read, which the caller keeps open.
   * @param offset The offset of the region of the file the data source serves.
   * @param length The length of the region, or -1 when it extends to the end of the file.
   */
  AVFileDescriptorDataSource(int fileDescriptor, long offset, long length) noexcept;

  ~AVFileDescriptorDataSource() override = default;

  bool open(const std::string& uri) override;

  int readAt(long position, uint8_t* buffer, size_t offset, size_t length) override;

  [[nodiscard]] long getLength() const override { return length_; }

  [[nodiscard]] const std::string& getUri() const override { return uri_; }

  void close() override;

  [[nodiscard]] bool isOpen() const override { return isOpen_; }

  /**
   * Returns the descriptor this data source reads, or -1 when there is none.
   */
  [[nodiscard]] int getFileDescriptor() const { return fileDescriptor_; }

  /**
   * Returns the offset of the region this data source serves.
   */
  [[nodiscard]] long getStartOffset() const { return offset_; }

private:
  int fileDescriptor_ = -1;
  long offset_ = 0;
  long length_ = -1;
  std::string uri_;
  bool isOpen_ = false;
};

} // namespace coral

#endif // defined(__ANDROID__)

#endif //CORAL_DATASOURCE_ANDROID_DATA_SOURCE_H
