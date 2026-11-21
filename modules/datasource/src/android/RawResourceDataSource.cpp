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
#include <core/jni/android/Context.h>
#include <core/jni/android/Resources.h>
#include <core/jni/android/Uri.h>

#include <minilog/log.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>

namespace coral {

namespace {

/**
 * Returns the identifier a decimal resource id names.
 *
 * <p>Only called with a string {@link android::isResourceId} accepted, therefore the conversion
 * cannot fail.
 */
int resourceIdOf(const std::string& text) {
  return static_cast<int>(std::strtol(text.c_str(), nullptr, 10));
}

} // namespace anonymous

struct AVRawResourceDataSource::Impl {
  /// The descriptor of the resource, which owns the file descriptor the reads are served from.
  std::unique_ptr<jni::AssetFileDescriptor> descriptor;
  std::unique_ptr<AVFileDescriptorDataSource> fileSource;
  std::string uri;
  bool open = false;

  void release();
};

AVRawResourceDataSource::AVRawResourceDataSource()
  : impl_(std::make_unique<Impl>()) {
}

AVRawResourceDataSource::~AVRawResourceDataSource() {
  close();
}

bool AVRawResourceDataSource::open(const std::string& uri) {
  close();

  auto context = jni::ApplicationContext::get();
  if (context == nullptr) {
    MINILOG_WARN("cannot open the raw resource of {}: no application context was registered", uri);
    return false;
  }
  auto parsed = jni::Uri::parse(uri);
  if (parsed == nullptr) {
    MINILOG_WARN("the uri {} cannot be parsed", uri);
    return false;
  }
  const std::string scheme = parsed->getScheme();
  std::unique_ptr<jni::Resources> resources;
  int resourceId = 0;

  if (scheme == "rawresource") {
    resources = context->getResources();
    const auto segments = parsed->getPathSegments();
    if (segments.size() != 1 || !android::isResourceId(segments[0])) {
      MINILOG_WARN("the raw resource uri {} must name exactly one resource id", uri);
      return false;
    }
    resourceId = resourceIdOf(segments[0]);
  } else if (scheme == "android.resource") {
    std::string path = parsed->getPath();
    if (!path.empty() && path[0] == '/') {
      path.erase(0, 1);
    }
    std::string packageName = parsed->getHost();
    const std::string ownPackage = context->getPackageName();
    if (packageName.empty()) {
      packageName = ownPackage;
    }
    resources = packageName == ownPackage
      ? context->getResources()
      : context->getResourcesForApplication(packageName);
    if (resources == nullptr) {
      MINILOG_WARN("the package of {} was not found: {}", uri, JniHelper::takeLastError());
      return false;
    }
    if (android::isResourceId(path)) {
      resourceId = resourceIdOf(path);
    } else {
      // The name form is the one that costs a lookup: the platform is asked for the identifier of
      // "package:path" as a resource of the raw type.
      resourceId = resources->getIdentifier(packageName + ":" + path, "raw", std::string());
    }
  } else {
    MINILOG_WARN("the scheme of {} is not the one of a raw resource", uri);
    return false;
  }

  if (resourceId <= 0) {
    MINILOG_WARN("the raw resource of {} was not found", uri);
    return false;
  }
  auto descriptor = resources->openRawResourceFd(resourceId);
  if (descriptor == nullptr) {
    // A resource the APK stores compressed has no region of a file to describe, and the media of a
    // resource is packaged uncompressed for exactly this reason.
    MINILOG_WARN("the raw resource of {} does not exist, or is stored compressed: {}", uri,
      JniHelper::takeLastError());
    return false;
  }
  auto source = android::createFileDescriptorSource(*descriptor);
  if (source == nullptr || !source->open(uri)) {
    MINILOG_WARN("the raw resource of {} cannot be read at a position: {}", uri,
      JniHelper::takeLastError());
    return false;
  }
  impl_->descriptor = std::move(descriptor);
  impl_->fileSource = std::move(source);
  impl_->uri = uri;
  impl_->open = true;
  return true;
}

int AVRawResourceDataSource::readAt(const long position, uint8_t* buffer, const size_t offset,
                                    const size_t length) {
  if (!impl_->open || impl_->fileSource == nullptr || buffer == nullptr) {
    return -1;
  }
  return impl_->fileSource->readAt(position, buffer, offset, length);
}

long AVRawResourceDataSource::getLength() const {
  if (impl_ == nullptr || impl_->fileSource == nullptr) {
    return -1;
  }
  return impl_->fileSource->getLength();
}

const std::string& AVRawResourceDataSource::getUri() const {
  return impl_->uri;
}

void AVRawResourceDataSource::close() {
  if (impl_ != nullptr) {
    impl_->release();
  }
}

bool AVRawResourceDataSource::isOpen() const {
  return impl_ != nullptr && impl_->open;
}

void AVRawResourceDataSource::Impl::release() {
  fileSource.reset();
  if (descriptor != nullptr) {
    descriptor->close();
    descriptor.reset();
  }
  uri.clear();
  open = false;
}

} // namespace coral
