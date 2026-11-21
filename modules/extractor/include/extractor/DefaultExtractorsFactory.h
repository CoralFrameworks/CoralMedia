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

#ifndef CORAL_EXTRACTOR_DEFAULT_EXTRACTORS_FACTORY_H
#define CORAL_EXTRACTOR_DEFAULT_EXTRACTORS_FACTORY_H

#include <extractor/Extractor.h>

#include <memory>
#include <string>
#include <vector>

namespace coral {

/**
 * Creates the extractors that are supported natively by this module.
 *
 * <p>This is the extractor module counterpart of ExoPlayer's {@code DefaultExtractorsFactory}. The
 * extractors are returned in the order in which they are asked to sniff the input, which matters
 * for the formats whose signature is ambiguous, e.g. an MP3 stream and an ADTS stream both start
 * with an ID3 tag, or an AVIF/HEIF still image and an MP4 movie both start with an ISO base media
 * file type box.
 */
class AVDefaultExtractorsFactory final : public AVExtractorsFactory {
public:
  /**
   * @param fileType The type of the container, as defined by {@link FileTypes}, or -1 when unknown.
   *                 When it is known, the matching extractor is tried first.
   * @param length The length of the media in bytes, or -1 when unknown.
   */
  explicit AVDefaultExtractorsFactory(int fileType = -1, long length = -1) noexcept;

  ~AVDefaultExtractorsFactory() override = default;

  AVDefaultExtractorsFactory(const AVDefaultExtractorsFactory& other) = delete;

  AVDefaultExtractorsFactory& operator=(const AVDefaultExtractorsFactory& other) = delete;

  std::vector<std::unique_ptr<AVExtractor>> createExtractors() override;

private:
  int fileType_;
  long length_;
};

inline std::unique_ptr<AVExtractorsFactory>
AVExtractorsFactory::createDefault(const int fileType, const long length) {
  return std::make_unique<AVDefaultExtractorsFactory>(fileType, length);
}

} // namespace coral

#endif //CORAL_EXTRACTOR_DEFAULT_EXTRACTORS_FACTORY_H
