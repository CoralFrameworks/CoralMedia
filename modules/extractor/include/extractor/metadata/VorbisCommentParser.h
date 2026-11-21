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

#ifndef CORAL_EXTRACTOR_METADATA_VORBIS_COMMENT_PARSER_H
#define CORAL_EXTRACTOR_METADATA_VORBIS_COMMENT_PARSER_H

#include <media/Metadata.h>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace coral {

/**
 * The parser of a Vorbis comment block and of the picture block of FLAC.
 *
 * <p>The counterpart of ExoPlayer's {@code VorbisUtil.readVorbisCommentHeader} and of its FLAC
 * metadata readers. A comment block is a vendor string and a list of `NAME=value` pairs, both with
 * a little endian length; FLAC stores the same block in its `VORBIS_COMMENT` metadata block and a
 * cover picture in its `PICTURE` block.
 *
 * <p>A comment name is case insensitive per the specification, so the entries are stored with a
 * lower case key. A comment that has no `=` is kept as a key with an empty value rather than
 * dropped.
 */
class AVVorbisCommentParser final {
public:
  AVVorbisCommentParser() = delete;

  /**
   * Parses a Vorbis comment block, appending the vendor and one entry per comment to @p metadata.
   *
   * @param data The block, starting at the length of the vendor string.
   * @param size The number of bytes available.
   * @param metadata The metadata to append the entries to.
   *
   * @return The number of bytes the block took, or zero when it is malformed.
   */
  static size_t parse(const uint8_t* data, size_t size, const std::shared_ptr<Metadata>& metadata);

  /**
   * Parses a FLAC `PICTURE` block, appending one picture entry to @p metadata.
   *
   * @return The number of bytes the block took, or zero when it is malformed.
   */
  static size_t parseFlacPicture(const uint8_t* data, size_t size,
                                 const std::shared_ptr<Metadata>& metadata);

  /**
   * Parses a FLAC `CUESHEET` block, appending its raw bytes under the `flac.cuesheet` key.
   *
   * <p>The block is not interpreted: a cue sheet describes the layout of a whole album, and none of
   * the players of the module uses it yet. Keeping the bytes lets a caller that does read them.
   *
   * @return The number of bytes the block took.
   */
  static size_t parseFlacCuesheet(const uint8_t* data, size_t size,
                                  const std::shared_ptr<Metadata>& metadata);
};

} // namespace coral

#endif //CORAL_EXTRACTOR_METADATA_VORBIS_COMMENT_PARSER_H
