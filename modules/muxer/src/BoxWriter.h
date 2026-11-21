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

#ifndef CORAL_MUXER_BOX_WRITER_H
#define CORAL_MUXER_BOX_WRITER_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace coral {
namespace muxer {
namespace mp4 {

/** Big-endian MP4 box buffer with size patching. */
class BoxWriter final {
public:
  void u8(uint8_t value);
  void u16(uint16_t value);
  void u24(uint32_t value);
  void u32(uint32_t value);
  void u64(uint64_t value);
  void i16(int16_t value);
  void i32(int32_t value);
  void bytes(const uint8_t* data, size_t size);
  void bytes(const std::vector<uint8_t>& data);
  void fourcc(const char type[4]);
  void zeros(size_t count);
  void string(const std::string& value);

  /** Writes a box header and returns the position of its 32-bit size field. */
  size_t beginBox(const char type[4]);
  void endBox(size_t sizePosition);

  /** Writes a large box header (32-bit size 1, 64-bit size field). */
  size_t beginBox64(const char type[4]);
  void endBox64(size_t sizePosition);

  /** Writes a full box header. */
  size_t beginFullBox(const char type[4], uint8_t version, uint32_t flags);

  /** Patches the 32-bit size of a box started at @p sizePosition. */
  void patchU32(size_t position, uint32_t value);

  /** Patches a 64-bit value, e.g. the size of a large box. */
  void patchU64(size_t position, uint64_t value);

  [[nodiscard]] size_t size() const { return mData.size(); }
  [[nodiscard]] const std::vector<uint8_t>& data() const { return mData; }
  std::vector<uint8_t> take();

private:
  std::vector<uint8_t> mData;
};

} // namespace mp4
} // namespace muxer
} // namespace coral

#endif // CORAL_MUXER_BOX_WRITER_H
