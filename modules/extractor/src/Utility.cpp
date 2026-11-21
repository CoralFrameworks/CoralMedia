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

#include <extractor/Utility.h>

#include <algorithm>

namespace coral {

int64_t AVUtil::gcd(int64_t a, int64_t b) {
  if (a <= 0 || b <= 0) {
    return 1;
  }
  while (b != 0) {
    const int64_t remainder = a % b;
    a = b;
    b = remainder;
  }
  return a == 0 ? 1 : a;
}

int64_t AVUtil::lcm(const int64_t a, const int64_t b, const int64_t max) {
  const int64_t gcdValue = gcd(a, b);
  const int64_t lcmValue = a / gcdValue * b;
  return lcmValue <= max ? lcmValue : max;
}

int AVUtil::indexOf(const uint8_t data[], const size_t size, const uint32_t fourCc) {
  const uint8_t c0 = static_cast<uint8_t>(fourCc >> 24);
  const uint8_t c1 = static_cast<uint8_t>(fourCc >> 16);
  const uint8_t c2 = static_cast<uint8_t>(fourCc >> 8);
  const uint8_t c3 = static_cast<uint8_t>(fourCc);
  if (size < 4) {
    return -1;
  }
  for (size_t i = 0; i + 4 <= size; ++i) {
    if (data[i] == c0 && data[i + 1] == c1 && data[i + 2] == c2 && data[i + 3] == c3) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

bool AVUtil::startsWith(const uint8_t data[], const size_t size, const uint32_t fourCc) {
  if (size < 4) {
    return false;
  }
  return readUnsignedInt(data, 0) == fourCc;
}

int AVUtil::peekFourCc(AVExtractorInput& input, const int offset) {
  uint8_t scratch[4];
  if (offset != 0) {
    input.resetPeekPosition();
    if (offset > 0) {
      input.skipPeeked(offset);
    }
  }
  const long peeked = input.peek(scratch, 0, sizeof(scratch));
  if (peeked != static_cast<long>(sizeof(scratch))) {
    return -1;
  }
  return static_cast<int>(readUnsignedInt(scratch, 0));
}

size_t AVUtil::annexBSize(const uint8_t data[], const size_t size, const int lengthSize) {
  if (data == nullptr || lengthSize < 1 || lengthSize > 4 ||
      size < static_cast<size_t>(lengthSize)) {
    return 0;
  }
  // A sample that starts with a start code is in the framing the decoders expect already, and its
  // lengths would not describe it: converting it again would corrupt it.
  if (size >= 3 && data[0] == 0 && data[1] == 0 &&
      (data[2] == 1 || (size >= 4 && data[2] == 0 && data[3] == 1))) {
    return 0;
  }
  size_t offset = 0;
  size_t convertedSize = 0;
  while (offset + static_cast<size_t>(lengthSize) <= size) {
    size_t unitSize = 0;
    for (int index = 0; index < lengthSize; ++index) {
      unitSize = (unitSize << 8) | data[offset + index];
    }
    offset += static_cast<size_t>(lengthSize);
    if (unitSize == 0 || unitSize > size - offset) {
      // The prefixes do not describe the sample, so it is not length prefixed after all.
      return 0;
    }
    convertedSize += kStartCodeSize + unitSize;
    offset += unitSize;
  }
  if (offset != size || convertedSize == 0) {
    return 0;
  }
  return convertedSize;
}

void AVUtil::toAnnexB(const uint8_t data[], const size_t size, const int lengthSize,
                      uint8_t target[]) {
  size_t offset = 0;
  size_t written = 0;
  while (offset + static_cast<size_t>(lengthSize) <= size) {
    size_t unitSize = 0;
    for (int index = 0; index < lengthSize; ++index) {
      unitSize = (unitSize << 8) | data[offset + index];
    }
    offset += static_cast<size_t>(lengthSize);
    target[written] = 0;
    target[written + 1] = 0;
    target[written + 2] = 0;
    target[written + 3] = 1;
    written += kStartCodeSize;
    std::copy(data + offset, data + offset + unitSize, target + written);
    written += unitSize;
    offset += unitSize;
  }
}

// ==================================================================================================
// AVBitReader
// ==================================================================================================

uint32_t AVBitReader::readBits(const int numBits) {
  if (numBits == 0) {
    return 0;
  }
  if (data_ == nullptr || !canReadBits(numBits)) {
    bitPosition_ += numBits;
    return 0;
  }

  uint32_t value = 0;
  const uint8_t* const bytes = data_->array();
  int remaining = numBits;
  while (remaining > 0) {
    const int byteIndex = bitPosition_ / 8;
    const int bitIndex = bitPosition_ % 8;
    const int bitsInByte = 8 - bitIndex;
    const int toRead = std::min(remaining, bitsInByte);

    // Extract the bits, most significant bit first.
    const uint32_t mask = static_cast<uint32_t>((1 << bitsInByte) - 1);
    uint32_t byte = (static_cast<uint32_t>(bytes[byteIndex]) & mask);
    const int drop = bitsInByte - toRead;
    byte >>= drop;
    value = (value << toRead) | byte;

    bitPosition_ += toRead;
    remaining -= toRead;
  }
  return value;
}

} // namespace coral
