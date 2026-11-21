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

#ifndef CORAL_EXTRACTOR_METADATA_ID3_PARSER_H
#define CORAL_EXTRACTOR_METADATA_ID3_PARSER_H

#include <media/Metadata.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace coral {

/**
 * The parser of an ID3v2 tag.
 *
 * <p>This is the counterpart of ExoPlayer's {@code Id3Decoder}, reduced to the frame types that the
 * extractors of the module expose: the text, URL, comment, private, picture and chapter frames. A
 * frame the class has no model for is kept as its payload, so a caller never loses a tag it does not
 * understand.
 *
 * <p>Versions 2.2, 2.3 and 2.4 are read, including the unsynchronisation of the tag and of the
 * frames that state it, and the extended header. A compressed or encrypted frame is skipped, because
 * its content cannot be decoded without the algorithm that produced it.
 */
class AVId3Parser final {
public:
  AVId3Parser() = delete;

  /**
   * Returns whether @p data starts an ID3v2 tag.
   */
  [[nodiscard]] static bool isId3(const uint8_t* data, size_t size);

  /**
   * Returns the number of bytes of the tag that starts at @p data, header and footer included, or
   * zero when the data is not a valid tag.
   *
   * <p>An extractor uses it to skip the tag without parsing it, and to find the samples that
   * follow it.
   */
  [[nodiscard]] static size_t getTagSize(const uint8_t* data, size_t size);

  /**
   * Parses the tag that starts at @p data, appending one entry per frame to @p metadata.
   *
   * @param data The bytes of the tag.
   * @param size The number of bytes available.
   * @param metadata The metadata to append the entries to.
   *
   * @return The number of bytes of the tag, header and footer included, or zero when the data is not
   *         a valid tag.
   */
  static size_t parse(const uint8_t* data, size_t size, const std::shared_ptr<Metadata>& metadata);

  /**
   * Parses every tag that starts at @p data.
   *
   * <p>A file may carry several tags in a row (an ID3v2 tag followed by another one), which is what
   * an MP3 file with a large picture looks like.
   *
   * @return The number of bytes of the tags.
   */
  static size_t parseAll(const uint8_t* data, size_t size, const std::shared_ptr<Metadata>& metadata);

  /**
   * Returns the value of the first text frame of @p id, or an empty string.
   *
   * <p>The convenience that turns the entries of a tag into the title, the artist or the album of a
   * format.
   */
  [[nodiscard]] static std::string findText(const std::shared_ptr<Metadata>& metadata,
                                            const std::string& frameId);
};

} // namespace coral

#endif //CORAL_EXTRACTOR_METADATA_ID3_PARSER_H
