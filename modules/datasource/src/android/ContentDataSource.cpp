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
#include <core/jni/android/ContentResolver.h>
#include <core/jni/android/Context.h>
#include <core/jni/android/Uri.h>

#include <minilog/log.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace coral {

struct AVContentDataSource::Impl {
  /// The descriptor of the provider, which owns the file descriptor the reads are served from.
  std::unique_ptr<jni::AssetFileDescriptor> descriptor;
  std::unique_ptr<AVFileDescriptorDataSource> fileSource;
  std::string uri;
  bool open = false;

  void release();
};

AVContentDataSource::AVContentDataSource()
  : impl_(std::make_unique<Impl>()) {
}

AVContentDataSource::~AVContentDataSource() {
  close();
}

bool AVContentDataSource::open(const std::string& uri) {
  close();

  auto context = jni::ApplicationContext::get();
  if (context == nullptr) {
    MINILOG_WARN("cannot open the content media of {}: no application context was registered", uri);
    return false;
  }
  auto resolver = context->getContentResolver();
  if (resolver == nullptr) {
    MINILOG_WARN("cannot open the content media of {}: the context has no content resolver", uri);
    return false;
  }

  std::string scheme;
  if (auto parsed = jni::Uri::parse(uri)) {
    scheme = parsed->getScheme();
  }
  std::unique_ptr<jni::AssetFileDescriptor> descriptor;
  if (scheme == "content") {
    // The typed call asks a media provider for the original media rather than the copy it would
    // otherwise be allowed to transcode.
    descriptor = resolver->openTypedAssetFileDescriptor(uri, "*/*");
  }
  if (descriptor == nullptr) {
    // A provider that does not implement the typed call, and a uri of another scheme that a content
    // resolver opens, are both served by the plain call.
    descriptor = resolver->openAssetFileDescriptor(uri, "r");
  }
  if (descriptor == nullptr) {
    MINILOG_WARN("cannot open the content media of {}: {}", uri, JniHelper::takeLastError());
    return false;
  }
  auto source = android::createFileDescriptorSource(*descriptor);
  if (source == nullptr || !source->open(uri)) {
    MINILOG_WARN("the provider of {} does not serve a seekable media: {}", uri,
      JniHelper::takeLastError());
    return false;
  }
  impl_->descriptor = std::move(descriptor);
  impl_->fileSource = std::move(source);
  impl_->uri = uri;
  impl_->open = true;
  return true;
}

int AVContentDataSource::readAt(const long position, uint8_t* buffer, const size_t offset,
                                const size_t length) {
  if (!impl_->open || impl_->fileSource == nullptr || buffer == nullptr) {
    return -1;
  }
  return impl_->fileSource->readAt(position, buffer, offset, length);
}

long AVContentDataSource::getLength() const {
  if (impl_ == nullptr || impl_->fileSource == nullptr) {
    return -1;
  }
  return impl_->fileSource->getLength();
}

const std::string& AVContentDataSource::getUri() const {
  return impl_->uri;
}

void AVContentDataSource::close() {
  if (impl_ != nullptr) {
    impl_->release();
  }
}

bool AVContentDataSource::isOpen() const {
  return impl_ != nullptr && impl_->open;
}

void AVContentDataSource::Impl::release() {
  fileSource.reset();
  if (descriptor != nullptr) {
    descriptor->close();
    descriptor.reset();
  }
  uri.clear();
  open = false;
}

} // namespace coral
