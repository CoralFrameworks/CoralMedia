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

#include "BoxWriter.h"

#include <cstring>
#include <utility>

namespace coral {
namespace muxer {
namespace mp4 {

void BoxWriter::u8(const uint8_t value) {
  mData.push_back(value);
}

void BoxWriter::u16(const uint16_t value) {
  mData.push_back(static_cast<uint8_t>(value >> 8));
  mData.push_back(static_cast<uint8_t>(value));
}

void BoxWriter::u24(const uint32_t value) {
  mData.push_back(static_cast<uint8_t>(value >> 16));
  mData.push_back(static_cast<uint8_t>(value >> 8));
  mData.push_back(static_cast<uint8_t>(value));
}

void BoxWriter::u32(const uint32_t value) {
  mData.push_back(static_cast<uint8_t>(value >> 24));
  mData.push_back(static_cast<uint8_t>(value >> 16));
  mData.push_back(static_cast<uint8_t>(value >> 8));
  mData.push_back(static_cast<uint8_t>(value));
}

void BoxWriter::u64(const uint64_t value) {
  u32(static_cast<uint32_t>(value >> 32));
  u32(static_cast<uint32_t>(value));
}

void BoxWriter::i16(const int16_t value) {
  u16(static_cast<uint16_t>(value));
}

void BoxWriter::i32(const int32_t value) {
  u32(static_cast<uint32_t>(value));
}

void BoxWriter::bytes(const uint8_t* data, const size_t size) {
  if (data != nullptr && size > 0) {
    mData.insert(mData.end(), data, data + size);
  }
}

void BoxWriter::bytes(const std::vector<uint8_t>& data) {
  bytes(data.data(), data.size());
}

void BoxWriter::fourcc(const char type[4]) {
  mData.insert(mData.end(), type, type + 4);
}

void BoxWriter::zeros(const size_t count) {
  mData.insert(mData.end(), count, 0);
}

void BoxWriter::string(const std::string& value) {
  mData.insert(mData.end(), value.begin(), value.end());
}

size_t BoxWriter::beginBox(const char type[4]) {
  const size_t position = mData.size();
  u32(0);
  fourcc(type);
  return position;
}

void BoxWriter::endBox(const size_t sizePosition) {
  const auto end = static_cast<uint32_t>(mData.size());
  patchU32(sizePosition, end - static_cast<uint32_t>(sizePosition));
}

size_t BoxWriter::beginBox64(const char type[4]) {
  const size_t position = mData.size();
  u32(1);
  fourcc(type);
  u64(0);
  return position;
}

void BoxWriter::endBox64(const size_t sizePosition) {
  const auto end = static_cast<uint64_t>(mData.size());
  patchU64(sizePosition + 8, end - static_cast<uint64_t>(sizePosition));
}

size_t BoxWriter::beginFullBox(const char type[4], const uint8_t version,
  const uint32_t flags) {
  const size_t position = beginBox(type);
  u8(version);
  u24(flags);
  return position;
}

void BoxWriter::patchU32(const size_t position, const uint32_t value) {
  if (position + 4 > mData.size()) {
    return;
  }
  mData[position] = static_cast<uint8_t>(value >> 24);
  mData[position + 1] = static_cast<uint8_t>(value >> 16);
  mData[position + 2] = static_cast<uint8_t>(value >> 8);
  mData[position + 3] = static_cast<uint8_t>(value);
}

void BoxWriter::patchU64(const size_t position, const uint64_t value) {
  patchU32(position, static_cast<uint32_t>(value >> 32));
  patchU32(position + 4, static_cast<uint32_t>(value));
}

std::vector<uint8_t> BoxWriter::take() {
  return std::move(mData);
}

} // namespace mp4
} // namespace muxer
} // namespace coral
