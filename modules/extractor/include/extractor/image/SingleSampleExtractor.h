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

#ifndef CORAL_EXTRACTOR_IMAGE_SINGLE_SAMPLE_EXTRACTOR_H
#define CORAL_EXTRACTOR_IMAGE_SINGLE_SAMPLE_EXTRACTOR_H

#include <extractor/Extractor.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace coral {

/**
 * Extracts a container whose whole payload is a single sample, e.g. a still image.
 *
 * <p>The extractor declares an image track when it is initialized, reads the input in chunks and
 * delivers the bytes that it accumulated as one sample when the end of the input is reached. The
 * sample timestamp is zero and the sample is a key frame, because a single sample container has no
 * timing information.
 *
 * <p>The memory that the sample needs is bounded by {@link kMaxSampleSize}; a file that is larger is
 * not buffered and the track ends without a sample. The size comes from the input, so a malformed
 * length in a header can never make the extractor allocate more than that bound.
 *
 * <p>This is the extractor module counterpart of ExoPlayer's {@code SingleSampleExtractor}.
 */
class AVSingleSampleExtractor final : public AVExtractor {
public:
  /**
   * The identifier of the image track, which is the one ExoPlayer reserves so that it does not
   * collide with the tracks of a motion photo container.
   */
  static constexpr int kImageTrackId = 1024;

  /** The maximum size of a single sample that this extractor buffers, in bytes. */
  static constexpr size_t kMaxSampleSize = 512U * 1024U * 1024U;

  /**
   * @param fileSignature The big endian signature of the container, or a negative value when the
   *                      caller implements {@link #sniff} itself.
   * @param fileSignatureLength The number of bytes of the signature, or a negative value when the
   *                            caller implements {@link #sniff} itself.
   * @param sampleMimeType The MIME type of the sample and of the container.
   */
  AVSingleSampleExtractor(int fileSignature, int fileSignatureLength, std::string sampleMimeType);

  ~AVSingleSampleExtractor() override = default;

  AVSingleSampleExtractor(const AVSingleSampleExtractor& other) = delete;

  AVSingleSampleExtractor& operator=(const AVSingleSampleExtractor& other) = delete;

  int sniff(AVExtractorInput& input) override;

  void init(AVExtractorOutput& output) override;

  Result read(AVExtractorInput& input) override;

  bool seek(long position, int64_t timeUs) override;

  [[nodiscard]] long getPosition() const override { return position_; }

  [[nodiscard]] std::string getName() const override { return "single sample"; }

private:
  /**
   * Builds the format of the image track.
   */
  std::shared_ptr<MediaFormat> buildFormat() const;

  /**
   * Delivers the bytes that were accumulated as one sample and ends the track.
   */
  void outputSample();

  int fileSignature_;
  int fileSignatureLength_;
  std::string sampleMimeType_;
  bool initialized_ = false;
  bool ended_ = false;
  bool trackEnded_ = false;
  long position_ = 0;
  std::vector<uint8_t> data_;
  std::shared_ptr<AVTrackOutput> trackOutput_;
};

} // namespace coral

#endif // CORAL_EXTRACTOR_IMAGE_SINGLE_SAMPLE_EXTRACTOR_H
