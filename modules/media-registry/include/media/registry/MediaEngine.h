/*
 * Copyright (C) 2022 The Coral Project
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
#ifndef CORAL_MEDIA_ENGINE_H
#define CORAL_MEDIA_ENGINE_H

#include <atomic>
#include <memory>

namespace coral {

class AudioCodecFactory;
class ExtractorFactory;
class MediaMuxerFactory;
class VideoCodecFactory;

class MediaEngine final {
public:
  MediaEngine(const MediaEngine& other) = delete;
  MediaEngine(MediaEngine&& other) = delete;
  MediaEngine& operator=(const MediaEngine& other) = delete;
  MediaEngine& operator=(MediaEngine&& other) = delete;

  static MediaEngine& getInstance();

  void prepareDefaultFactory();

  void registerAudioCodecFactory(std::unique_ptr<AudioCodecFactory> factory);

  void registerExtractorFactory(std::unique_ptr<ExtractorFactory> factory);

  void registerMuxerFactory(std::unique_ptr<MediaMuxerFactory> factory);

  void registerVideoCodecFactory(std::unique_ptr<VideoCodecFactory> factory);

  const std::unique_ptr<AudioCodecFactory>& getAudioCodecFactory() const {
    return audioCodecFactory;
  }

  const std::unique_ptr<VideoCodecFactory>& getVideoCodecFactory() const {
    return videoCodecFactory;
  }

  const std::unique_ptr<ExtractorFactory>& getExtractorFactory() const {
    return extractorFactory;
  }

  const std::unique_ptr<MediaMuxerFactory>& getMuxerFactory() const {
    return muxerFactory;
  }

private:
  MediaEngine() = default;
  ~MediaEngine() = default;

  std::atomic_bool prepared = false;

  std::unique_ptr<AudioCodecFactory> audioCodecFactory;
  std::unique_ptr<ExtractorFactory> extractorFactory;
  std::unique_ptr<MediaMuxerFactory> muxerFactory;
  std::unique_ptr<VideoCodecFactory> videoCodecFactory;
};

} // namespace coral

#endif // CORAL_MEDIA_ENGINE_H
