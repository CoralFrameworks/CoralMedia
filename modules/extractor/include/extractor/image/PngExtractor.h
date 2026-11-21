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

#ifndef CORAL_EXTRACTOR_IMAGE_PNG_EXTRACTOR_H
#define CORAL_EXTRACTOR_IMAGE_PNG_EXTRACTOR_H

#include <extractor/Extractor.h>
#include <extractor/image/SingleSampleExtractor.h>

#include <memory>
#include <string>

namespace coral {

/**
 * Extracts a PNG image as a single sample.
 *
 * <p>This is the extractor module counterpart of ExoPlayer's {@code PngExtractor}. The file starts
 * with the first two bytes of the PNG signature, {@code 0x89 0x50}.
 */
class AVPngExtractor final : public AVExtractor {
public:
  AVPngExtractor();

  ~AVPngExtractor() override = default;

  AVPngExtractor(const AVPngExtractor& other) = delete;

  AVPngExtractor& operator=(const AVPngExtractor& other) = delete;

  int sniff(AVExtractorInput& input) override;

  void init(AVExtractorOutput& output) override;

  Result read(AVExtractorInput& input) override;

  bool seek(long position, int64_t timeUs) override;

  [[nodiscard]] long getPosition() const override { return imageExtractor_.getPosition(); }

  [[nodiscard]] std::string getName() const override { return "PNG"; }

private:
  AVSingleSampleExtractor imageExtractor_;
};

} // namespace coral

#endif // CORAL_EXTRACTOR_IMAGE_PNG_EXTRACTOR_H
