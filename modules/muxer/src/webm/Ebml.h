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

#ifndef CORAL_MUXER_WEBM_EBML_H
#define CORAL_MUXER_WEBM_EBML_H

#include <cstdint>
#include <string>
#include <vector>

namespace coral {
namespace muxer {
namespace webm {

/** Writes an EBML element ID, including its marker bits. */
void putId(std::vector<uint8_t>& data, uint32_t id);

/** Writes a variable-length element size. */
void putSize(std::vector<uint8_t>& data, uint64_t size);

/** Writes the eight-byte unknown-size value used by the Segment element. */
void putUnknownSize(std::vector<uint8_t>& data);

std::vector<uint8_t> element(uint32_t id, const std::vector<uint8_t>& payload);

std::vector<uint8_t> uintElement(uint32_t id, uint64_t value);

std::vector<uint8_t> stringElement(uint32_t id, const std::string& value);

std::vector<uint8_t> binaryElement(uint32_t id, const std::vector<uint8_t>& value);

std::vector<uint8_t> floatElement(uint32_t id, double value, bool eightBytes = true);

void append(std::vector<uint8_t>& destination, const std::vector<uint8_t>& source);

} // namespace webm
} // namespace muxer
} // namespace coral

#endif // CORAL_MUXER_WEBM_EBML_H
