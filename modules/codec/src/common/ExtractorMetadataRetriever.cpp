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

#include "ExtractorMetadataRetriever.h"

#include <datasource/DataSource.h>
#include <extractor/DefaultExtractorsFactory.h>
#include <extractor/Reader.h>
#include <media/Types.h>

#include <minilog/log.h>

#include <memory>
#include <utility>

namespace coral {

namespace {

/**
 * The number of chunks read while waiting for the formats of the tracks of a media.
 *
 * <p>A reader stops preparing as soon as the first track is declared, while the format of a track may
 * only be published once the container was parsed: a WAV file, for instance, declares its track before
 * it read the header that describes it. The retriever reads until every track it knows has a format,
 * and a container that never publishes one is given up on after this many reads rather than read in
 * full.
 */
constexpr int kMaxFormatReads = 32;

/** Returns whether a track of the media was declared without its format being published. */
bool hasMissingFormat(const AVReader& reader) {
  for (const int trackId : reader.getTrackIds()) {
    if (reader.getTrackFormat(trackId) == nullptr) {
      return true;
    }
  }
  return false;
}

} // namespace anonymous

ExtractorMetadataRetriever::ExtractorMetadataRetriever(std::string url) noexcept
  : mUrl(std::move(url)) {
}

ExtractorMetadataRetriever::~ExtractorMetadataRetriever() {
  std::lock_guard<std::mutex> lock(mMutex);
  releaseLocked();
}

void ExtractorMetadataRetriever::cancelLoading() {
  // The reads are not asynchronous: there is nothing to cancel. The method exists because the
  // interface mirrors the one of the platform, where a remote media is read in the background.
}

int ExtractorMetadataRetriever::getTrackCount() {
  std::lock_guard<std::mutex> lock(mMutex);
  prepareLocked();
  return static_cast<int>(mTrackIds.size());
}

std::shared_ptr<MediaFormat> ExtractorMetadataRetriever::getTrackFormat(const int trackIndex) {
  std::lock_guard<std::mutex> lock(mMutex);
  if (!prepareLocked()) {
    return nullptr;
  }
  if (trackIndex < 0 || trackIndex >= static_cast<int>(mTrackIds.size())) {
    return nullptr;
  }
  return mReader != nullptr ? mReader->getTrackFormat(mTrackIds[trackIndex]) : nullptr;
}

bool ExtractorMetadataRetriever::prepareLocked() {
  if (mPrepared) {
    return true;
  }
  if (mPrepareFailed) {
    // A media that was refused once is refused again: the retriever is asked about the same url.
    return false;
  }
  mPrepareFailed = true;
  if (mUrl.empty()) {
    MINILOG_WARN("the metadata of a media without a url cannot be read");
    return false;
  }

  // The media is read with the data source of its scheme, exactly as the player reads it: the asset
  // of the platform, the media of a content provider and a file of the file system are all uris.
  auto dataSource = std::make_shared<AVDefaultDataSource>();
  if (!dataSource->open(mUrl)) {
    MINILOG_WARN("the metadata of {} was not read: the data source could not open it", mUrl);
    return false;
  }
  const int fileType = FileTypes::inferFileTypeFromUrl(mUrl);
  auto reader = std::make_unique<AVReader>(
    dataSource, AVExtractorsFactory::createDefault(fileType), fileType);
  if (!reader->prepare()) {
    MINILOG_WARN("the metadata of {} was not read: {}", mUrl, reader->getLastError());
    // The data source of the reader is the one that was opened here, and the reader did not take it
    // over: closing it here is what releases the platform object of an asset or a provider.
    dataSource->close();
    return false;
  }

  // The formats of the tracks are what the caller asks for, and a container may publish them after
  // the reader prepared: read the media until every declared track has one.
  bool missingFormat = hasMissingFormat(*reader);
  for (int read = 0; read < kMaxFormatReads && missingFormat; ++read) {
    const int result = reader->read();
    missingFormat = hasMissingFormat(*reader);
    if (result == AVReader::kResultError ||
        (result == AVReader::kResultEndOfInput && missingFormat)) {
      break;
    }
  }

  mTrackIds = reader->getTrackIds();
  mReader = std::move(reader);
  mDataSource = std::move(dataSource);
  mPrepared = true;
  mPrepareFailed = false;
  return true;
}

void ExtractorMetadataRetriever::releaseLocked() {
  mTrackIds.clear();
  mReader.reset();
  if (mDataSource != nullptr) {
    mDataSource->close();
    mDataSource.reset();
  }
  mPrepared = false;
  mPrepareFailed = false;
}

} // namespace coral
