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

#include "FrameworkExtractorFactory.h"

#if defined(CORAL_SUPPORTS_FFMPEG)
#include <ffcodec/FfmpegExtractor.h>
#endif

using namespace coral;

FrameworkExtractorFactory::FrameworkExtractorFactory() {
  builtinFactory = CreateBuiltinExtractorFactory();
#if defined(CORAL_SUPPORTS_FFMPEG)
  customFactory = std::make_unique<FfmpegExtractorFactory>();
#endif
}

FrameworkExtractorFactory::~FrameworkExtractorFactory() = default;

void FrameworkExtractorFactory::setCustomFactory(std::unique_ptr<ExtractorFactory> factory) {
  customFactory = std::move(factory);
}

std::shared_ptr<AVMediaExtractor>
FrameworkExtractorFactory::createExtractor(const std::string &url, bool platform) {
  std::shared_ptr<AVMediaExtractor> extractor = nullptr;
  if (platform) {
    extractor = builtinFactory->createExtractor(url);
  } else if (customFactory != nullptr) {
    extractor = customFactory->createExtractor(url);
  }
  return extractor;
}

