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

#ifndef CORAL_DATASOURCE_DARWIN_DATA_SOURCE_H
#define CORAL_DATASOURCE_DARWIN_DATA_SOURCE_H

#include <datasource/DataSource.h>

#if defined(__APPLE__)

#include <memory>
#include <string>

namespace coral {

/**
 * A data source that reads a file of the local file system of macOS or iOS.
 *
 * <p>It is the counterpart of {@link AVFileDataSource} for the uris the platform produces, and the
 * one {@link AVDefaultDataSource} creates for a local media:
 *
 * <ul>
 *   <li>a plain path, e.g. {@code /Users/me/Movies/a.mp4} or {@code video/a.mp4};</li>
 *   <li>a {@code file} uri, e.g. {@code file:///Users/me/Movies/a.mp4}, whose path may be
 *       percent-encoded ({@code file:///Users/me/My%20Movie.mp4}) and may name the default authority
 *       ({@code file://localhost/...});</li>
 *   <li>a path that starts with {@code ~}, which is resolved against the home directory.</li>
 * </ul>
 *
 * <p>A url the system picker returned is security scoped: the sandbox grants the access of the url
 * itself, not of the path it names. The data source starts the access of the url before it opens the
 * file and stops it when the source is closed. A url that was converted to a string by its owner has
 * lost the scope in some cases, which is why the owner that keeps the {@code NSURL} should start the
 * access itself and keep it for as long as the media is read; the data source then opens the path
 * with the access the owner holds.
 *
 * <p>A relative path is resolved against the current directory first, which is what the process of a
 * command line tool and a test expect, and against the resource directory of the main bundle second,
 * which is what an application that ships its media as a resource expects. A media of the app bundle
 * can also be named by {@link AVBundleDataSource}.
 */
class AVDarwinFileDataSource final : public AVDataSource {
public:
  AVDarwinFileDataSource();

  ~AVDarwinFileDataSource() override;

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
 * A data source that reads a resource of the main bundle, the counterpart of the Android
 * {@code AVAssetDataSource}.
 *
 * <p>The supported uris are:
 *
 * <ul>
 *   <li>{@code asset:///video/sample.mp4} and {@code bundle:///video/sample.mp4}, i.e. the path of
 *       the resource relative to the resources of the bundle;</li>
 *   <li>{@code asset://video/sample.mp4}, where the first path component is read as the authority of
 *       the uri rather than as a directory, i.e. the same resource;</li>
 *   <li>{@code file:///android_asset/video/sample.mp4}, which is the uri Android uses for an asset,
 *       so that a media named once plays on both platforms.</li>
 * </ul>
 *
 * <p>The resource is looked up in the resource directory of the main bundle, i.e.
 * {@code MyApp.app/Contents/Resources} on macOS and the directory of {@code MyApp.app} on iOS, and
 * then in the bundle directory, which is what a command line tool and a resource that is copied next
 * to the executable use. A path that leaves the bundle ({@code ..}) is refused rather than read.
 *
 * <p>The data source is a thin lookup over {@link AVDarwinFileDataSource}, which reads the resource:
 * the bundle names where the media is, and the file data source reads it with the security scope and
 * the positional reads of the platform.
 */
class AVBundleDataSource final : public AVDataSource {
public:
  AVBundleDataSource();

  ~AVBundleDataSource() override;

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

} // namespace coral

#endif // defined(__APPLE__)

#endif // CORAL_DATASOURCE_DARWIN_DATA_SOURCE_H
