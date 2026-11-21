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

#ifndef CORAL_MUXER_MUXER_OUTPUT_H
#define CORAL_MUXER_MUXER_OUTPUT_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace coral {
namespace muxer {

/** The byte sink of a container writer. */
class MuxerOutput {
public:
  virtual ~MuxerOutput() = default;

  virtual bool write(const uint8_t* data, size_t size) = 0;
  virtual bool seek(int64_t position) = 0;
  [[nodiscard]] virtual int64_t position() const = 0;
  [[nodiscard]] virtual bool isSeekable() const = 0;
  virtual bool truncate(int64_t size) = 0;

  /**
   * Moves @p size bytes from @p source to @p destination inside the output.
   *
   * The ranges may overlap; the implementation copies in the safe direction. Fast-start writing
   * uses it to move the `mdat` box after the `moov` box once the final movie header size is known.
   */
  virtual bool copyBytes(int64_t destination, int64_t source, int64_t size) = 0;

  virtual bool close() = 0;

  /** Deletes the file or other persistent storage. */
  virtual void remove() = 0;
};

std::unique_ptr<MuxerOutput> openFileOutput(const std::string& path, std::string* error);

} // namespace muxer
} // namespace coral

#endif // CORAL_MUXER_MUXER_OUTPUT_H
