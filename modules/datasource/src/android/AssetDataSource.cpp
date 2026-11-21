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

#include "AndroidDataSourceSupport.h"

#include <datasource/AndroidDataSource.h>

#include <core/jni/JniHelper.h>
#include <core/jni/android/AssetFileDescriptor.h>
#include <core/jni/android/AssetManager.h>
#include <core/jni/android/Context.h>
#include <core/jni/android/InputStream.h>

#include <minilog/log.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>

namespace coral {

namespace {

/** The length the platform reports for a stream it can serve in full, i.e. no length at all. */
constexpr int kUnknownStreamLength = std::numeric_limits<int>::max();

} // namespace anonymous

struct AVAssetDataSource::Impl {
  /// The manager of the assets of the application, which a backwards seek reopens through.
  std::unique_ptr<jni::AssetManager> assets;

  /// The descriptor of an asset the APK stores uncompressed, and the region of it that is read.
  std::unique_ptr<jni::AssetFileDescriptor> descriptor;
  std::unique_ptr<AVFileDescriptorDataSource> fileSource;

  /// The stream of an asset the APK stores compressed, which is only read forwards.
  std::unique_ptr<jni::InputStream> stream;

  std::string assetPath;
  std::string uri;
  long length = -1;
  long position = 0;
  bool open = false;

  /**
   * Reopens the stream of the asset at its beginning, which is what serves a read before the
   * position the stream reached.
   */
  bool reopenStream();

  int readStream(long readPosition, uint8_t* buffer, size_t offset, size_t length);

  void release();
};

// ==================================================================================================
// AVAssetDataSource
// ==================================================================================================

AVAssetDataSource::AVAssetDataSource()
  : impl_(std::make_unique<Impl>()) {
}

AVAssetDataSource::~AVAssetDataSource() {
  close();
}

bool AVAssetDataSource::open(const std::string& uri) {
  close();

  auto context = jni::ApplicationContext::get();
  if (context == nullptr) {
    // The platform is what holds the assets, and it is only reachable through the context the
    // application registers: an asset uri cannot be served without one.
    MINILOG_WARN("cannot open the asset of {}: no application context was registered", uri);
    return false;
  }
  auto assets = context->getAssets();
  if (assets == nullptr) {
    MINILOG_WARN("cannot open the asset of {}: the context has no asset manager", uri);
    return false;
  }
  const std::string path = android::assetPathOf(uri);
  if (path.empty()) {
    MINILOG_WARN("the uri {} names no asset", uri);
    return false;
  }

  // An asset the APK stores uncompressed is a region of the APK file, which its descriptor serves
  // without copying it and without a position to seek to.
  if (auto descriptor = assets->openFd(path)) {
    auto source = android::createFileDescriptorSource(*descriptor);
    if (source != nullptr && source->open(uri)) {
      impl_->length = source->getLength();
      impl_->descriptor = std::move(descriptor);
      impl_->fileSource = std::move(source);
      impl_->assets = std::move(assets);
      impl_->assetPath = path;
      impl_->uri = uri;
      impl_->open = true;
      return true;
    }
    MINILOG_WARN("the descriptor of the asset {} cannot be read: {}", path,
      JniHelper::takeLastError());
    // The stream below is the fallback of a descriptor that exists but does not describe a region of
    // a file, which is rare, and of an asset that is stored compressed, which is not.
  }

  auto stream = assets->open(path, jni::AssetManager::kAccessRandom);
  if (stream == nullptr) {
    MINILOG_WARN("the asset {} was not found: {}", path, JniHelper::takeLastError());
    return false;
  }
  const int available = stream->available();
  impl_->length = available >= 0 && available != kUnknownStreamLength ? available : -1;
  impl_->assets = std::move(assets);
  impl_->stream = std::move(stream);
  impl_->assetPath = path;
  impl_->position = 0;
  impl_->uri = uri;
  impl_->open = true;
  return true;
}

int AVAssetDataSource::readAt(const long position, uint8_t* buffer, const size_t offset,
                              const size_t length) {
  if (!impl_->open || buffer == nullptr) {
    return -1;
  }
  if (position < 0 || length == 0) {
    return 0;
  }
  if (impl_->fileSource != nullptr) {
    return impl_->fileSource->readAt(position, buffer, offset, length);
  }
  return impl_->readStream(position, buffer, offset, length);
}

long AVAssetDataSource::getLength() const {
  if (impl_ == nullptr) {
    return -1;
  }
  return impl_->fileSource != nullptr ? impl_->fileSource->getLength() : impl_->length;
}

const std::string& AVAssetDataSource::getUri() const {
  return impl_->uri;
}

void AVAssetDataSource::close() {
  if (impl_ != nullptr) {
    impl_->release();
  }
}

bool AVAssetDataSource::isOpen() const {
  return impl_ != nullptr && impl_->open;
}

// ==================================================================================================
// AVAssetDataSource::Impl
// ==================================================================================================

bool AVAssetDataSource::Impl::reopenStream() {
  if (assets == nullptr) {
    return false;
  }
  stream = assets->open(assetPath, jni::AssetManager::kAccessRandom);
  position = 0;
  return stream != nullptr;
}

int AVAssetDataSource::Impl::readStream(const long readPosition, uint8_t* buffer,
                                        const size_t offset, const size_t length) {
  if (stream == nullptr) {
    return -1;
  }
  if (this->length >= 0 && readPosition >= this->length) {
    return 0;
  }
  if (readPosition < position) {
    // A stream of the platform is only read forwards: a read before the position it reached is
    // served by a stream that is opened again.
    if (!reopenStream()) {
      return -1;
    }
  }
  while (position < readPosition) {
    const long skipped = stream->skip(readPosition - position);
    if (skipped <= 0) {
      break;
    }
    position += skipped;
  }
  if (position != readPosition) {
    // The stream ended before the position was reached.
    return 0;
  }
  size_t toRead = length;
  if (this->length >= 0 && static_cast<long>(toRead) > this->length - readPosition) {
    toRead = static_cast<size_t>(this->length - readPosition);
  }
  const int read = stream->read(buffer + offset, toRead);
  if (read <= 0) {
    return 0;
  }
  position += read;
  return read;
}

void AVAssetDataSource::Impl::release() {
  if (stream != nullptr) {
    stream->close();
    stream.reset();
  }
  // The descriptor of the file is closed after the data source that reads it, and before the assets
  // manager that handed it out.
  fileSource.reset();
  if (descriptor != nullptr) {
    descriptor->close();
    descriptor.reset();
  }
  assets.reset();
  assetPath.clear();
  uri.clear();
  length = -1;
  position = 0;
  open = false;
}

} // namespace coral
