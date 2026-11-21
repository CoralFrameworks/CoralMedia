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

#ifndef CORAL_EXTRACTOR_METADATA_METADATA_DECODER_H
#define CORAL_EXTRACTOR_METADATA_METADATA_DECODER_H

#include <media/Format.h>
#include <media/Metadata.h>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace coral {

/**
 * The decoder of the samples of a metadata track.
 *
 * <p>The counterpart of ExoPlayer's {@code MetadataDecoder} and of its
 * {@code MetadataDecoderFactory}. A player reads the samples of a track whose media type is
 * {@link MediaType::kMediaTypeMetadata} and gives each payload to the decoder of its format, which
 * appends the entries it describes to the metadata of the current position.
 *
 * <p>A decoder is stateful only for as long as a sample needs it: every sample is a complete
 * document, apart from an ID3 tag list that the parser reads in one call.
 */
class AVMetadataDecoder {
public:
  virtual ~AVMetadataDecoder() = default;

  /**
   * Decodes one metadata sample.
   *
   * @param data The bytes of the sample.
   * @param size The number of bytes available.
   * @param metadata The metadata to append the entries to.
   *
   * @return {@code true} when the sample was decoded, {@code false} when it is malformed.
   */
  virtual bool decode(const uint8_t* data, size_t size,
                      const std::shared_ptr<Metadata>& metadata) = 0;

  /** Forgets the state of the decoder, so that it can decode the samples of another stream. */
  virtual void reset() {}
};

/**
 * The factory of the decoders of the metadata formats that the module supports.
 *
 * <p>The counterpart of {@code MetadataDecoderFactory.DEFAULT}.
 */
class AVMetadataDecoderFactory {
public:
  virtual ~AVMetadataDecoderFactory() = default;

  /** Returns whether a decoder exists for the given format. */
  [[nodiscard]] virtual bool supportsFormat(const MediaFormat& format) const = 0;

  /** Returns the decoder of the format, or null when none exists. */
  [[nodiscard]] virtual std::unique_ptr<AVMetadataDecoder> create(const MediaFormat& format) const = 0;
};

/**
 * Creates the factory of the decoders of ID3, {@code emsg} and SCTE-35 metadata.
 *
 * <p>The counterpart of {@code MetadataDecoderFactory.DEFAULT}, which is what
 * {@code PlayerItemMetadataOutput} uses when the application does not install its own factory.
 */
[[nodiscard]] std::unique_ptr<AVMetadataDecoderFactory> createDefaultMetadataDecoderFactory();

} // namespace coral

#endif //CORAL_EXTRACTOR_METADATA_METADATA_DECODER_H
