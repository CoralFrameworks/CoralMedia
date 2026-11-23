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

// The POSIX file API is used rather than the MSVC specific "secure" replacements, because the
// implementation is shared by every platform the project supports.
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <datasource/DataSource.h>
#include <datasource/HttpDataSource.h>

#if defined(__ANDROID__)
#include <datasource/AndroidDataSource.h>
#endif

#if defined(__APPLE__)
#include <datasource/DarwinDataSource.h>
#endif

#include <cctype>
#include <cerrno>
#include <cstring>
#include <utility>

namespace coral {

namespace {

/**
 * A small RAII wrapper around {@code std::FILE}, so that the file handle can never be leaked, not
 * even when an early return happens while opening the data source.
 */
class FileHandle final {
public:
  FileHandle() noexcept = default;

  explicit FileHandle(std::FILE* file) noexcept : file_(file) {}

  FileHandle(const FileHandle& other) = delete;

  FileHandle& operator=(const FileHandle& other) = delete;

  FileHandle(FileHandle&& other) noexcept : file_(other.file_) { other.file_ = nullptr; }

  FileHandle& operator=(FileHandle&& other) noexcept {
    if (this != &other) {
      close();
      file_ = other.file_;
      other.file_ = nullptr;
    }
    return *this;
  }

  ~FileHandle() { close(); }

  [[nodiscard]] bool isValid() const { return file_ != nullptr; }

  [[nodiscard]] std::FILE* get() const { return file_; }

  void close() {
    if (file_ != nullptr) {
      std::fclose(file_);
      file_ = nullptr;
    }
  }

private:
  std::FILE* file_ = nullptr;
};

bool seekTo(std::FILE* file, const long position) {
#if defined(_WIN32)
  return _fseeki64(file, static_cast<long long>(position), SEEK_SET) == 0;
#else
  return std::fseek(file, position, SEEK_SET) == 0;
#endif
}

bool seekToEnd(std::FILE* file) {
#if defined(_WIN32)
  return _fseeki64(file, 0, SEEK_END) == 0;
#else
  return std::fseek(file, 0, SEEK_END) == 0;
#endif
}

long tell(std::FILE* file) {
#if defined(_WIN32)
  return static_cast<long>(_ftelli64(file));
#else
  return static_cast<long>(std::ftell(file));
#endif
}

} // namespace anonymous

// ==================================================================================================
// AVFileDataSource
// ==================================================================================================

struct AVFileDataSource::Handle final {
  FileHandle file;
};

AVFileDataSource::AVFileDataSource() noexcept = default;

AVFileDataSource::~AVFileDataSource() { close(); }

bool AVFileDataSource::open(const std::string& uri) {
  close();

  FileHandle file(std::fopen(uri.c_str(), "rb"));
  if (!file.isValid()) {
    return false;
  }

  // The length of the file is the position of its end, therefore the stream is moved there and
  // then back to its beginning.
  if (!seekToEnd(file.get())) {
    return false;
  }
  const long fileLength = tell(file.get());
  if (fileLength < 0 || !seekTo(file.get(), 0)) {
    return false;
  }

  handle_ = std::unique_ptr<Handle>(new Handle());
  handle_->file = std::move(file);
  uri_ = uri;
  length_ = fileLength;
  return true;
}

int AVFileDataSource::readAt(const long position, uint8_t* buffer, const size_t offset,
                             const size_t length) {
  if (!isOpen() || buffer == nullptr) {
    return -1;
  }
  if (position < 0 || length == 0) {
    return 0;
  }
  if (length_ >= 0 && position >= length_) {
    return 0;
  }
  if (!seekTo(handle_->file.get(), position)) {
    return -1;
  }
  const size_t read = std::fread(buffer + offset, 1, length, handle_->file.get());
  if (read == 0 && std::ferror(handle_->file.get()) != 0) {
    return -1;
  }
  return static_cast<int>(read);
}

void AVFileDataSource::close() {
  handle_.reset();
  uri_.clear();
  length_ = -1;
}

// ==================================================================================================
// AVByteArrayDataSource
// ==================================================================================================

bool AVByteArrayDataSource::open(const std::string& uri) {
  uri_ = uri;
  length_ = static_cast<long>(data_.size());
  open_ = true;
  return true;
}

int AVByteArrayDataSource::readAt(const long position, uint8_t* buffer, const size_t offset,
                                  const size_t length) {
  if (!open_ || buffer == nullptr) {
    return -1;
  }
  if (position < 0 || length == 0) {
    return 0;
  }
  const auto total = static_cast<long>(data_.size());
  if (position >= total) {
    return 0;
  }
  size_t available = static_cast<size_t>(total - position);
  if (available > length) {
    available = length;
  }
  std::memcpy(buffer + offset, data_.data() + position, available);
  return static_cast<int>(available);
}

// ==================================================================================================
// AVDefaultDataSource
// ==================================================================================================

namespace {

/** Returns the scheme of a uri in lower case, i.e. what precedes its first colon, or an empty string. */
std::string schemeOf(const std::string& uri) {
  const auto colon = uri.find(':');
  // A scheme is at least two characters long, which keeps the drive letter of a path of the platform,
  // e.g. "C:\media", from being read as the scheme of a uri.
  if (colon == std::string::npos || colon < 2) {
    return std::string();
  }
  std::string scheme = uri.substr(0, colon);
  for (char& character : scheme) {
    character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
  }
  return scheme;
}

/** Returns the uri of a file, i.e. the uri without the scheme of a file uri. */
std::string filePathOf(const std::string& uri) {
  const std::string scheme = schemeOf(uri);
  if (scheme != "file") {
    return uri;
  }
  std::string path = uri.substr(scheme.size() + 1);
  // A file uri names the authority of the file, which is empty for a file of the local file system, and
  // the path of the platform follows it: "file:///C:/media" is the path "C:/media".
  if (path.rfind("//", 0) == 0) {
    path.erase(0, 2);
  }
  if (path.size() > 2 && path[0] == '/' && path[2] == ':') {
    path.erase(0, 1);
  }
  return path;
}

} // namespace anonymous

AVDefaultDataSource::AVDefaultDataSource()
  : httpDataSourceFactory_(createHttpDataSourceFactory()) {
}

AVDefaultDataSource::AVDefaultDataSource(
  std::unique_ptr<AVHttpDataSourceFactory> httpDataSourceFactory)
  : httpDataSourceFactory_(std::move(httpDataSourceFactory)) {
}

AVDefaultDataSource::~AVDefaultDataSource() { close(); }

bool AVDefaultDataSource::open(const std::string& uri) {
  close();
  const std::string scheme = schemeOf(uri);
  std::shared_ptr<AVDataSource> delegate;
  if (scheme == "http" || scheme == "https") {
    // The data source of an http uri is the one of the platform, which the factory of the module
    // creates: the module holds the interface, and the platform holds the implementation.
    std::shared_ptr<AVHttpDataSource> httpDataSource =
      httpDataSourceFactory_ != nullptr ? httpDataSourceFactory_->create(AVHttpDataSource::Parameters())
                                        : nullptr;
    if (httpDataSource == nullptr || !httpDataSource->open(uri)) {
      return false;
    }
    delegate = std::move(httpDataSource);
  }
#if defined(__ANDROID__)
  else if (scheme == "asset") {
    // The asset of the APK the application was installed from, e.g. "asset:///video/a.mp4".
    delegate = std::make_shared<AVAssetDataSource>();
    if (!delegate->open(uri)) {
      return false;
    }
  } else if (scheme == "content") {
    // The media of a content provider, e.g. the one a picker returned: read through the descriptor of
    // the provider rather than copied into the cache of the application.
    delegate = std::make_shared<AVContentDataSource>();
    if (!delegate->open(uri)) {
      return false;
    }
  } else if (scheme == "rawresource" || scheme == "android.resource") {
    // The raw resource of the APK, e.g. "android.resource://com.example/raw/a".
    delegate = std::make_shared<AVRawResourceDataSource>();
    if (!delegate->open(uri)) {
      return false;
    }
  } else if ((scheme.empty() || scheme == "file") &&
             filePathOf(uri).rfind("/android_asset/", 0) == 0) {
    // An asset named through the uri of a file, which is what an application that stores its media
    // under "assets/" commonly writes.
    delegate = std::make_shared<AVAssetDataSource>();
    if (!delegate->open(uri)) {
      return false;
    }
  }
#endif
#if defined(__APPLE__)
  else if (scheme == "asset" || scheme == "bundle") {
    // A resource of the main bundle, e.g. "asset:///video/a.mp4" or "bundle:///video/a.mp4". The
    // scheme of Android is accepted by the same data source, so a media that is named once plays on
    // both platforms.
    delegate = std::make_shared<AVBundleDataSource>();
    if (!delegate->open(uri)) {
      return false;
    }
  } else if ((scheme.empty() || scheme == "file") &&
             filePathOf(uri).rfind("/android_asset/", 0) == 0) {
    // The Android spelling of a bundled media, e.g. "file:///android_asset/video/a.mp4".
    delegate = std::make_shared<AVBundleDataSource>();
    if (!delegate->open(uri)) {
      return false;
    }
  } else {
    // Every other uri names a file of the local file system, either by its path or with the scheme
    // of a file uri. The data source of the platform resolves a percent-encoded file uri, the
    // security scope of a url the system picker returned, and a relative path of the bundle.
    delegate = std::make_shared<AVDarwinFileDataSource>();
    if (!delegate->open(uri)) {
      return false;
    }
  }
#else
  else {
    // Every other uri names a file of the local file system, either by its path or with the scheme of a
    // file uri: an unknown scheme is not a reason to refuse to read the media of a path that happens to
    // contain a colon.
    delegate = std::make_shared<AVFileDataSource>();
    if (!delegate->open(filePathOf(uri))) {
      return false;
    }
  }
#endif
  delegate_ = std::move(delegate);
  uri_ = uri;
  return true;
}

int AVDefaultDataSource::readAt(const long position, uint8_t* buffer, const size_t offset,
                                const size_t length) {
  if (delegate_ == nullptr) {
    return -1;
  }
  return delegate_->readAt(position, buffer, offset, length);
}

long AVDefaultDataSource::getLength() const {
  return delegate_ != nullptr ? delegate_->getLength() : -1;
}

void AVDefaultDataSource::close() {
  if (delegate_ != nullptr) {
    delegate_->close();
    delegate_.reset();
  }
  uri_.clear();
}

bool AVDefaultDataSource::isOpen() const {
  return delegate_ != nullptr && delegate_->isOpen();
}

AVDataSourceFactory createDefaultDataSourceFactory() {
  return [](const std::string& uri) -> std::shared_ptr<AVDataSource> {
    static_cast<void>(uri);
    return std::make_shared<AVDefaultDataSource>();
  };
}

} // namespace coral
