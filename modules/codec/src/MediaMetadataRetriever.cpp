/*
 * Copyright (C) 2023 The Coral Project
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

#include <codec/MediaMetadataRetriever.h>

#include "common/ExtractorMetadataRetriever.h"

#include <memory>

namespace coral {

MediaMetadataRetriever::~MediaMetadataRetriever() = default;

std::unique_ptr<MediaMetadataRetriever>
MediaMetadataRetriever::create(const std::string& url) {
  // The retriever always exists, a media that cannot be read reports no track instead: the answer of
  // an unreadable media is what the caller asks for, not a null retriever.
  //
  // The media is read with the data sources and the extractors of the module on every platform, like
  // the player reads it. That is what makes the retriever read the uris of the platform (an asset, a
  // raw resource or the media of a content provider on Android) and what makes its formats the ones
  // of the reader, down to the identifiers of the tracks.
  return std::make_unique<ExtractorMetadataRetriever>(url);
}

} // namespace coral
