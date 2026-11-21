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

#include <core/jni/android/Uri.h>

#include <sys/stat.h>
#include <unistd.h>

#include <cstddef>
#include <memory>
#include <string>

namespace coral::android {

long resolveLength(const int fileDescriptor, const long startOffset,
                   const long declaredLength) {
  if (declaredLength != jni::AssetFileDescriptor::kUnknownLength) {
    return declaredLength;
  }
  // The 64 bit form is used on every ABI: a media is not required to be smaller than the range of
  // the 32 bit file size of a 32 bit ABI.
  struct stat64 information {};
  if (fileDescriptor < 0 || fstat64(fileDescriptor, &information) != 0) {
    return -1;
  }
  // A file of no size is a file the platform cannot describe rather than a media of no bytes.
  if (information.st_size == 0) {
    return -1;
  }
  const auto length = static_cast<long>(information.st_size - startOffset);
  return length >= 0 ? length : -1;
}

std::unique_ptr<AVFileDescriptorDataSource> createFileDescriptorSource(
  const jni::AssetFileDescriptor& assetFileDescriptor) {
  auto fileDescriptor = assetFileDescriptor.getFileDescriptor();
  if (fileDescriptor == nullptr || !fileDescriptor->isValid()) {
    return nullptr;
  }
  const int descriptor = fileDescriptor->getInt();
  if (descriptor < 0) {
    return nullptr;
  }
  // The position of a descriptor is asked for rather than assumed: a provider that serves the media
  // from a pipe hands out a descriptor that exists, and that no read at a position can serve.
  if (lseek(descriptor, 0, SEEK_CUR) < 0) {
    return nullptr;
  }
  const long startOffset = assetFileDescriptor.getStartOffset();
  const long length = resolveLength(descriptor, startOffset, assetFileDescriptor.getLength());
  return std::make_unique<AVFileDescriptorDataSource>(descriptor, startOffset, length);
}

std::string assetPathOf(const std::string& uri) {
  auto parsed = jni::Uri::parse(uri);
  if (parsed == nullptr) {
    return {};
  }
  std::string path = parsed->getPath();
  // The path of an asset names the directory the assets are packaged under, which the platform does
  // not: "file:///android_asset/video/a.mp4" opens the asset "video/a.mp4".
  static const std::string kAndroidAssetPrefix = "/android_asset/";
  if (path.rfind(kAndroidAssetPrefix, 0) == 0) {
    return path.substr(kAndroidAssetPrefix.size());
  }
  if (!path.empty() && path[0] == '/') {
    return path.substr(1);
  }
  return path;
}

bool isResourceId(const std::string& text) {
  if (text.empty()) {
    return false;
  }
  for (const char character : text) {
    if (character < '0' || character > '9') {
      return false;
    }
  }
  return true;
}

} // namespace coral::android
