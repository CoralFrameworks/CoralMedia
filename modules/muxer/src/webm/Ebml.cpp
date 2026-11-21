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

#include "Ebml.h"

#include <cstring>

namespace coral {
namespace muxer {
namespace webm {
namespace {

int idSize(const uint32_t id) {
  if ((id & 0xFF000000u) != 0) {
    return 4;
  }
  if ((id & 0x00FF0000u) != 0) {
    return 3;
  }
  if ((id & 0x0000FF00u) != 0) {
    return 2;
  }
  return 1;
}

int sizeLength(const uint64_t value) {
  for (int length = 1; length <= 8; ++length) {
    if (value < ((1ull << (7 * length)) - 1)) {
      return length;
    }
  }
  return 8;
}

} // namespace

void putId(std::vector<uint8_t>& data, const uint32_t id) {
  const int count = idSize(id);
  for (int i = count - 1; i >= 0; --i) {
    data.push_back(static_cast<uint8_t>(id >> (i * 8)));
  }
}

void putSize(std::vector<uint8_t>& data, const uint64_t size) {
  const int length = sizeLength(size);
  uint8_t first = static_cast<uint8_t>(1u << (8 - length));
  first |= static_cast<uint8_t>(size >> (8 * (length - 1)));
  data.push_back(first);
  for (int i = length - 2; i >= 0; --i) {
    data.push_back(static_cast<uint8_t>(size >> (i * 8)));
  }
}

void putUnknownSize(std::vector<uint8_t>& data) {
  data.insert(data.end(), { 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF });
}

std::vector<uint8_t> element(const uint32_t id, const std::vector<uint8_t>& payload) {
  std::vector<uint8_t> data;
  data.reserve(payload.size() + 12);
  putId(data, id);
  putSize(data, payload.size());
  data.insert(data.end(), payload.begin(), payload.end());
  return data;
}

std::vector<uint8_t> uintElement(const uint32_t id, const uint64_t value) {
  std::vector<uint8_t> payload;
  uint8_t bytes[8];
  int count = 1;
  for (int i = 0; i < 8; ++i) {
    bytes[i] = static_cast<uint8_t>(value >> ((7 - i) * 8));
  }
  for (int i = 0; i < 7; ++i) {
    if (bytes[i] != 0) {
      count = 8 - i;
      break;
    }
  }
  payload.insert(payload.end(), bytes + 8 - count, bytes + 8);
  return element(id, payload);
}

std::vector<uint8_t> stringElement(const uint32_t id, const std::string& value) {
  return element(id, std::vector<uint8_t>(value.begin(), value.end()));
}

std::vector<uint8_t> binaryElement(const uint32_t id, const std::vector<uint8_t>& value) {
  return element(id, value);
}

std::vector<uint8_t> floatElement(const uint32_t id, const double value, const bool eightBytes) {
  std::vector<uint8_t> payload;
  if (eightBytes) {
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    for (int i = 7; i >= 0; --i) {
      payload.push_back(static_cast<uint8_t>(bits >> (i * 8)));
    }
  } else {
    const float reduced = static_cast<float>(value);
    uint32_t bits = 0;
    std::memcpy(&bits, &reduced, sizeof(bits));
    for (int i = 3; i >= 0; --i) {
      payload.push_back(static_cast<uint8_t>(bits >> (i * 8)));
    }
  }
  return element(id, payload);
}

void append(std::vector<uint8_t>& destination, const std::vector<uint8_t>& source) {
  destination.insert(destination.end(), source.begin(), source.end());
}

} // namespace webm
} // namespace muxer
} // namespace coral
