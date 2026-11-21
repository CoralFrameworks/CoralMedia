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

#ifndef CORAL_CODEC_EXTRACTOR_MEDIA_METADATA_RETRIEVER_H
#define CORAL_CODEC_EXTRACTOR_MEDIA_METADATA_RETRIEVER_H

#include <media/Format.h>

#include <memory>
#include <string>

namespace coral {

/**
 * MediaMetadataRetriever class provides a unified interface for retrieving
 * frame and metadata from an input media file.
 *
 * <p>Mirrors {@code android.media.MediaMetadataRetriever}, except that the media is named by the uri
 * it is played with: the implementation of the module reads it with the data sources and the
 * extractors of {@code libs/datasource} and {@code libs/extractor}, like the player does. That is a
 * path, an http uri, and on Android the uri of an asset, of a raw resource or of a content provider.
 * The counterpart of the platform class is {@code androidx.media3.inspector.MetadataRetriever}.
 */
class MediaMetadataRetriever {
public:
  virtual ~MediaMetadataRetriever();

  static std::unique_ptr<MediaMetadataRetriever> create(const std::string& url);

  /**
   * Cancels loading remote media metadata if needed
   */
  virtual void cancelLoading() = 0;

  /**
   * Returns the track count of the media
   */
  virtual int getTrackCount() = 0;

  /**
   * Returns the media format for the specified track index of the media
   *
   * @param trackIndex the specified track of the media
   * @return the media format or nullptr.
   */
  virtual std::shared_ptr<MediaFormat> getTrackFormat(int trackIndex) = 0;
};

} // namespace coral

#endif // CORAL_CODEC_EXTRACTOR_MEDIA_METADATA_RETRIEVER_H
