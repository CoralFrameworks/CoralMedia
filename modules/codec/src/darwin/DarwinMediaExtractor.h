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

#ifndef CORAL_CODEC_DARWIN_MEDIA_EXTRACTOR_H
#define CORAL_CODEC_DARWIN_MEDIA_EXTRACTOR_H

#if defined(__APPLE__)

#include <codec/MediaExtractor.h>

#include <memory>
#include <string>

namespace coral {

/**
 * The implementation of the AVMediaExtractor based on the AVFoundation asset reader.
 *
 * The extractor delivers the compressed (native) sample data of the audio and video streams, so the
 * demuxed packets can be decoded by an {@link AVMediaDecoder} implementation. The asset reader is
 * configured without output settings, which makes it hand out the samples of the container in their
 * decode order rather than the frames of a decoder:
 *
 * <ul>
 *   <li>a video sample is converted to the Annex B byte stream the decoders of the framework
 *       consume, since the asset reader hands it out in the length prefixed form;</li>
 *   <li>an audio sample buffer may carry many compressed packets, which are split back into one
 *       packet per {@link AVSample} with their own timestamps.</li>
 * </ul>
 *
 * <p>The media is opened by {@link #createWithUrl}, so the extractor reads itself: the byte stream of
 * the {@link AVExtractorInput} of the reader is not the media it demuxes, which is why
 * {@code ExtractorFactory::createInputExtractor} reports that no input extractor is available.
 *
 * <p>The platform objects are held behind a pimpl so that this header stays a plain C++ header: it
 * is included by C++ translation units, which must not see any Objective-C type.
 */
class DarwinMediaExtractor final : public AVMediaExtractor {
public:
  /**
   * Creates a new extractor of the url, returns nullptr when the url is not a supported media file.
   *
   * <p>The media is opened by this call, so the extractor reads it itself: this is what the legacy
   * reader, which drives an extractor without an input of its own, uses.
   *
   * <p>The extractor is shared rather than owned by the caller alone: it hands itself out as the seek
   * map of the media it reads, so it has to be owned by a shared pointer.
   *
   * @param url A url, either a path of the file system or one with a scheme.
   */
  static std::shared_ptr<DarwinMediaExtractor> createWithUrl(const std::string& url);

  ~DarwinMediaExtractor() override;

  /**
   * Always returns {@link AVExtractorsFactory::kSniffResultPossible}: the media was opened by the url
   * the extractor was created for, and the input of the reader is not the media it demuxes.
   */
  int sniff(AVExtractorInput& input) override;

  void init(AVExtractorOutput& output) override;

  PlatformResult read(PositionHolder& seekPosition) override;

  bool seekTo(int64_t timeUs, SeekOption option) override;

  void clear() override;

  [[nodiscard]] int64_t getDurationUs() const override;

  [[nodiscard]] int64_t getReadPosition() const override;

  [[nodiscard]] bool isSeekable() const override;

  [[nodiscard]] coral::SeekPoints getPlatformSeekPoints(int64_t timeUs) const override;

  [[nodiscard]] std::string getName() const override { return "DarwinMediaExtractor"; }

private:
  explicit DarwinMediaExtractor(std::string url);

  struct Impl;
  std::unique_ptr<Impl> mImpl;
};

} // namespace coral

#endif // defined(__APPLE__)

#endif // CORAL_CODEC_DARWIN_MEDIA_EXTRACTOR_H
