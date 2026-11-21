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

#include <extractor/text/cea708/Cea708Sei.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace coral {
namespace text {
namespace {

constexpr int kPayloadTypeCaption = 4;
constexpr int kCountryCode = 0xB5;
constexpr int kProviderCodeAtsc = 0x31;
constexpr int kProviderCodeDirectTv = 0x2F;
constexpr uint32_t kUserIdentifierGa94 = 0x47413934u;
constexpr int kUserDataTypeMpegCc = 0x03;

/** Reads a value that may be spread over 0xFF continuation bytes. */
bool readNon255(const std::vector<uint8_t>& bytes, size_t& position, int& value) {
  value = 0;
  while (position < bytes.size()) {
    const uint8_t byte = bytes[position++];
    value += byte;
    if (byte != 0xFF) {
      return true;
    }
  }
  return false;
}

/** Removes the emulation-prevention bytes of an RBSP. */
std::vector<uint8_t> unescapeRbsp(const uint8_t* data, const size_t size) {
  std::vector<uint8_t> rbsp;
  rbsp.reserve(size);
  for (size_t i = 0; i < size; ++i) {
    if (i + 2 < size && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 3) {
      rbsp.push_back(0);
      rbsp.push_back(0);
      i += 2;
    } else {
      rbsp.push_back(data[i]);
    }
  }
  return rbsp;
}

} // namespace

std::vector<uint8_t> extractCea708CcDataFromSei(const uint8_t* nal, const size_t size,
    const bool hevc) {
  std::vector<uint8_t> result;
  const size_t headerSize = hevc ? 2 : 1;
  if (nal == nullptr || size <= headerSize) {
    return result;
  }
  const std::vector<uint8_t> rbsp = unescapeRbsp(nal + headerSize, size - headerSize);
  size_t position = 0;
  while (position + 1 < rbsp.size()) {
    int payloadType = 0;
    int payloadSize = 0;
    if (!readNon255(rbsp, position, payloadType) || !readNon255(rbsp, position, payloadSize)) {
      break;
    }
    if (payloadSize < 0 || static_cast<size_t>(payloadSize) > rbsp.size() - position) {
      break;
    }
    const size_t payloadStart = position;
    const size_t payloadEnd = payloadStart + static_cast<size_t>(payloadSize);
    position = payloadEnd;
    if (payloadType != kPayloadTypeCaption || payloadSize < 8) {
      continue;
    }
    const uint8_t* payload = rbsp.data() + payloadStart;
    const int countryCode = payload[0];
    const int providerCode = (payload[1] << 8) | payload[2];
    size_t cursor = 3;
    uint32_t userIdentifier = 0;
    if (providerCode == kProviderCodeAtsc) {
      if (payloadSize < 7) {
        continue;
      }
      userIdentifier = (static_cast<uint32_t>(payload[3]) << 24) |
                       (static_cast<uint32_t>(payload[4]) << 16) |
                       (static_cast<uint32_t>(payload[5]) << 8) | payload[6];
      cursor = 7;
    }
    if (cursor >= payloadEnd - payloadStart) {
      continue;
    }
    const int userDataTypeCode = payload[cursor++];
    if (providerCode == kProviderCodeDirectTv) {
      if (cursor >= payloadEnd - payloadStart) {
        continue;
      }
      ++cursor;  // user_data_length
    }
    const bool supported = countryCode == kCountryCode &&
                           (providerCode == kProviderCodeAtsc ||
                             providerCode == kProviderCodeDirectTv) &&
                           userDataTypeCode == kUserDataTypeMpegCc &&
                           (providerCode != kProviderCodeAtsc ||
                             userIdentifier == kUserIdentifierGa94);
    if (!supported || cursor >= payloadEnd - payloadStart) {
      continue;
    }
    // cc_data: reserved (1), process_cc_data_flag (1), zero_bit (1), cc_count (5), em_data (8).
    const uint8_t firstByte = payload[cursor++];
    const bool processCcData = (firstByte & 0x40) != 0;
    const int ccCount = firstByte & 0x1F;
    if (cursor >= payloadEnd - payloadStart) {
      continue;
    }
    ++cursor;  // em_data
    const size_t available = payloadEnd - payloadStart - cursor;
    const size_t ccSize = static_cast<size_t>(ccCount) * 3;
    if (processCcData && ccSize <= available) {
      result.insert(result.end(), payload + cursor, payload + cursor + ccSize);
    }
  }
  return result;
}

std::vector<uint8_t> extractCea708CcDataFromAnnexB(const uint8_t* data, const size_t size,
    const bool hevc) {
  std::vector<uint8_t> result;
  if (data == nullptr || size < 4) {
    return result;
  }
  size_t position = 0;
  while (position + 3 <= size) {
    size_t start = 0;
    if (position + 4 <= size && data[position] == 0 && data[position + 1] == 0 &&
        data[position + 2] == 0 && data[position + 3] == 1) {
      start = position + 4;
    } else if (data[position] == 0 && data[position + 1] == 0 && data[position + 2] == 1) {
      start = position + 3;
    } else {
      ++position;
      continue;
    }
    size_t end = start;
    while (end + 2 < size) {
      if (data[end] == 0 && data[end + 1] == 0 &&
          (data[end + 2] == 1 ||
            (end + 3 < size && data[end + 2] == 0 && data[end + 3] == 1))) {
        break;
      }
      ++end;
    }
    if (end <= start) {
      position = end;
      continue;
    }
    const int type = hevc ? ((data[start] >> 1) & 0x3F) : (data[start] & 0x1F);
    const bool isSei = hevc ? (type == 39 || type == 40) : type == 6;
    if (isSei) {
      const auto ccData = extractCea708CcDataFromSei(data + start, end - start, hevc);
      result.insert(result.end(), ccData.begin(), ccData.end());
    }
    position = end;
  }
  return result;
}

CeaCcData splitCeaCcData(const uint8_t* data, const size_t size) {
  CeaCcData result;
  if (data == nullptr) {
    return result;
  }
  for (size_t position = 0; position + 3 <= size; position += 3) {
    const uint8_t header = data[position];
    if ((header & 0x04) == 0) {
      continue;
    }
    const int type = header & 0x03;
    if (type <= 1) {
      result.cea608.push_back(data[position + 1]);
      result.cea608.push_back(data[position + 2]);
    } else {
      result.cea708.push_back(header);
      result.cea708.push_back(data[position + 1]);
      result.cea708.push_back(data[position + 2]);
    }
  }
  return result;
}

} // namespace text
} // namespace coral
