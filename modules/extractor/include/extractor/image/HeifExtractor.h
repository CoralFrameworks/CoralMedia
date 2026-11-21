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

#ifndef CORAL_EXTRACTOR_IMAGE_HEIF_EXTRACTOR_H
#define CORAL_EXTRACTOR_IMAGE_HEIF_EXTRACTOR_H

#include <extractor/Extractor.h>
#include <extractor/image/SingleSampleExtractor.h>

#include <memory>
#include <string>

namespace coral {

/**
 * Extracts a HEIF/HEIC image as a single sample.
 *
 * <p>This is the extractor module counterpart of ExoPlayer's {@code HeifExtractor}, in the mode that
 * exposes the still image track. The file is recognized when its first box is an {@code ftyp} box
 * whose major brand is {@code heic}. A motion photo, i.e. a HEIC file that also carries an
 * {@code mpvd} atom, is exposed as the image alone.
 */
class AVHeifExtractor final : public AVExtractor {
public:
  AVHeifExtractor();

  ~AVHeifExtractor() override = default;

  AVHeifExtractor(const AVHeifExtractor& other) = delete;

  AVHeifExtractor& operator=(const AVHeifExtractor& other) = delete;

  int sniff(AVExtractorInput& input) override;

  void init(AVExtractorOutput& output) override;

  Result read(AVExtractorInput& input) override;

  bool seek(long position, int64_t timeUs) override;

  [[nodiscard]] long getPosition() const override { return imageExtractor_.getPosition(); }

  [[nodiscard]] std::string getName() const override { return "HEIF"; }

private:
  AVSingleSampleExtractor imageExtractor_;
};

} // namespace coral

#endif // CORAL_EXTRACTOR_IMAGE_HEIF_EXTRACTOR_H
