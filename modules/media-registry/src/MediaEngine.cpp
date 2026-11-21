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

#include <media/registry/MediaEngine.h>

#include <codec/MediaExtractor.h>
#include <codec/MediaMuxer.h>

#include "FrameworkAudioCodecFactory.h"
#include "FrameworkExtractorFactory.h"
#include "FrameworkMuxerFactory.h"
#include "FrameworkVideoCodecFactory.h"

namespace coral {

MediaEngine &MediaEngine::getInstance() {
  static MediaEngine sInstance;
  sInstance.prepareDefaultFactory();
  return sInstance;
}

void MediaEngine::prepareDefaultFactory() {
  if (prepared) {
    return;
  }
  prepared = true;
  registerAudioCodecFactory(std::make_unique<FrameworkAudioCodecFactory>());
  registerExtractorFactory(std::make_unique<FrameworkExtractorFactory>());
  registerMuxerFactory(std::make_unique<FrameworkMuxerFactory>());
  registerVideoCodecFactory(std::make_unique<FrameworkVideoCodecFactory>());
}

void MediaEngine::registerAudioCodecFactory(std::unique_ptr<AudioCodecFactory> factory) {
  audioCodecFactory = std::move(factory);
}

void MediaEngine::registerExtractorFactory(std::unique_ptr<ExtractorFactory> factory) {
  extractorFactory = std::move(factory);
}

void MediaEngine::registerMuxerFactory(std::unique_ptr<MediaMuxerFactory> factory) {
  muxerFactory = std::move(factory);
}

void MediaEngine::registerVideoCodecFactory(std::unique_ptr<VideoCodecFactory> factory) {
  videoCodecFactory = std::move(factory);
}

} // namespace coral
