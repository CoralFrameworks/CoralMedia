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

#ifndef CORAL_CODEC_EXTRACTOR_METADATA_RETRIEVER_H
#define CORAL_CODEC_EXTRACTOR_METADATA_RETRIEVER_H

#include <codec/MediaMetadataRetriever.h>

#include <mutex>
#include <string>
#include <vector>

namespace coral {

class AVDataSource;
class AVReader;

/**
 * The metadata retriever of the module, built on the data sources and the extractors of the module.
 *
 * <p>The counterpart of {@code androidx.media3.inspector.MetadataRetriever}: the media is read the way
 * the player reads it, i.e. through a data source, so a media is inspected by the uri it is played
 * with. That is a file of the local file system, an http uri, and on Android an asset, a raw resource
 * or the media of a content provider.
 *
 * <p>The formats are the ones the extractor of the module reports, which is the extractor the reader
 * of the player uses as well: the track of an {@code AssetItem} and the track the reader serves are
 * therefore the same track, down to its identifier. A container that does not declare a property the
 * format has, e.g. the frame rate of a video track, is reported without it rather than with a value
 * the platform guessed.
 *
 * <p>The media is read when it is first asked about, and the reads are made on the calling thread.
 * The retriever is safe to use from multiple threads.
 */
class ExtractorMetadataRetriever final : public MediaMetadataRetriever {
public:
  explicit ExtractorMetadataRetriever(std::string url) noexcept;

  ~ExtractorMetadataRetriever() override;

  ExtractorMetadataRetriever(const ExtractorMetadataRetriever& other) = delete;

  ExtractorMetadataRetriever& operator=(const ExtractorMetadataRetriever& other) = delete;

  /**
   * Does nothing: the media of a data source is read synchronously, therefore a read that started
   * has already finished when the caller can cancel it.
   */
  void cancelLoading() override;

  int getTrackCount() override;

  std::shared_ptr<MediaFormat> getTrackFormat(int trackIndex) override;

  static const char* getName() { return "ExtractorMetadataRetriever"; }

private:
  /**
   * Opens the data source and reads the header of the media, once.
   *
   * @return {@code true} when the tracks of the media are known.
   */
  bool prepareLocked();

  void releaseLocked();

  std::string mUrl;
  std::shared_ptr<AVDataSource> mDataSource;
  std::unique_ptr<AVReader> mReader;
  std::vector<int> mTrackIds;
  std::mutex mMutex;
  bool mPrepared = false;
  bool mPrepareFailed = false;
};

} // namespace coral

#endif //CORAL_CODEC_EXTRACTOR_METADATA_RETRIEVER_H
