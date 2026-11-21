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

#ifndef CORAL_DATASOURCE_ANDROID_DATA_SOURCE_SUPPORT_H
#define CORAL_DATASOURCE_ANDROID_DATA_SOURCE_SUPPORT_H

#include <datasource/AndroidDataSource.h>

#include <core/jni/android/AssetFileDescriptor.h>

#include <memory>
#include <string>

namespace coral::android {

/**
 * Returns the length of the region of the file a descriptor describes.
 *
 * <p>The platform leaves the length of a region that extends to the end of its file unset, which is
 * resolved against the size of the file: a data source that knows the length of its media is what
 * lets an extractor tell the end of it from a short read.
 *
 * @param fileDescriptor The descriptor of the file.
 * @param startOffset The offset of the region in the file.
 * @param declaredLength The length the platform declared, or
 * {@code jni::AssetFileDescriptor::kUnknownLength}.
 *
 * @return the length of the region, or -1 when it cannot be known.
 */
long resolveLength(int fileDescriptor, long startOffset, long declaredLength);

/**
 * Creates the data source of the region an {@code AssetFileDescriptor} describes.
 *
 * <p>The descriptor is not closed by the data source: it belongs to the
 * {@code AssetFileDescriptor}, which its holder closes.
 *
 * @return the data source, or null when the region cannot be read at a position.
 */
std::unique_ptr<AVFileDescriptorDataSource> createFileDescriptorSource(
  const jni::AssetFileDescriptor& assetFileDescriptor);

/**
 * Returns the path an asset uri names, relative to the assets directory.
 *
 * <p>{@code asset:///video/a.mp4}, {@code file:///android_asset/video/a.mp4} and
 * {@code /android_asset/video/a.mp4} all name {@code video/a.mp4}.
 *
 * @return the path, or an empty string when the uri names no asset.
 */
std::string assetPathOf(const std::string& uri);

/**
 * Returns whether a string is the decimal identifier of a resource.
 */
bool isResourceId(const std::string& text);

} // namespace coral::android

#endif //CORAL_DATASOURCE_ANDROID_DATA_SOURCE_SUPPORT_H
