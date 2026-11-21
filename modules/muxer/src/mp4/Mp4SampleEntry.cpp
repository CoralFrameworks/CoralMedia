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

#include "Mp4SampleEntry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

namespace coral {
namespace muxer {
namespace mp4 {
namespace {

void writeU32(std::vector<uint8_t>& data, const uint32_t value) {
  data.push_back(static_cast<uint8_t>(value >> 24));
  data.push_back(static_cast<uint8_t>(value >> 16));
  data.push_back(static_cast<uint8_t>(value >> 8));
  data.push_back(static_cast<uint8_t>(value));
}

bool splitLengthPrefixed(const std::vector<uint8_t>& data, const size_t lengthSize,
  std::vector<std::vector<uint8_t>>* units) {
  if (data.size() <= lengthSize) {
    return false;
  }
  size_t position = 0;
  std::vector<std::vector<uint8_t>> parsed;
  while (position < data.size()) {
    if (position + lengthSize > data.size()) {
      return false;
    }
    size_t length = 0;
    for (size_t i = 0; i < lengthSize; ++i) {
      length = (length << 8) | data[position + i];
    }
    position += lengthSize;
    if (length == 0 || position + length > data.size()) {
      return false;
    }
    parsed.emplace_back(data.begin() + static_cast<std::ptrdiff_t>(position),
      data.begin() + static_cast<std::ptrdiff_t>(position + length));
    position += length;
  }
  if (parsed.empty()) {
    return false;
  }
  *units = std::move(parsed);
  return true;
}

std::vector<std::vector<uint8_t>> collectNalUnits(const std::vector<std::vector<uint8_t>>& csdList) {
  std::vector<std::vector<uint8_t>> units;
  for (const auto& csd: csdList) {
    if (csd.empty()) {
      continue;
    }
    if (startsWithStartCode(csd.data(), csd.size())) {
      auto annexB = splitNalUnits(csd);
      units.insert(units.end(), annexB.begin(), annexB.end());
      continue;
    }
    // Extracted MP4 CSD is commonly an AVC/HEVC length-prefixed record with a 1-byte length;
    // encoder CSD is commonly Annex B. Accept both, and 2/4-byte lengths as well.
    std::vector<std::vector<uint8_t>> parsed;
    if (splitLengthPrefixed(csd, 4, &parsed) || splitLengthPrefixed(csd, 2, &parsed) ||
        splitLengthPrefixed(csd, 1, &parsed)) {
      units.insert(units.end(), parsed.begin(), parsed.end());
      continue;
    }
    units.push_back(csd);
  }
  return units;
}

int findFrequencyIndex(const int sampleRate) {
  static constexpr std::array<int, 13> kRates = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000,
    11025, 8000, 7350 };
  for (size_t i = 0; i < kRates.size(); ++i) {
    if (kRates[i] == sampleRate) {
      return static_cast<int>(i);
    }
  }
  return 4; // 44100 Hz
}

std::vector<uint8_t> descriptorLength(size_t length) {
  // Four-byte expandable form. It is legal and avoids resizing an already written descriptor.
  std::array<uint8_t, 4> bytes = { static_cast<uint8_t>(0x80 | ((length >> 21) & 0x7F)),
    static_cast<uint8_t>(0x80 | ((length >> 14) & 0x7F)), static_cast<uint8_t>(0x80 | ((length >> 7) & 0x7F)),
    static_cast<uint8_t>(length & 0x7F) };
  return { bytes.begin(), bytes.end() };
}

void appendDescriptor(BoxWriter& writer, const uint8_t tag, const std::vector<uint8_t>& payload) {
  writer.u8(tag);
  const auto length = descriptorLength(payload.size());
  writer.bytes(length);
  writer.bytes(payload);
}

class BitReader {
public:
  explicit BitReader(const std::vector<uint8_t>& data)
          : mData(data) {}

  uint32_t read(const int count) {
    uint32_t value = 0;
    for (int i = 0; i < count; ++i) {
      value <<= 1;
      if (mByte < mData.size()) {
        value |= (mData[mByte] >> (7 - mBit)) & 1u;
      }
      if (++mBit == 8) {
        mBit = 0;
        ++mByte;
      }
    }
    return value;
  }

  void skip(const int count) {
    for (int i = 0; i < count; ++i) {
      if (++mBit == 8) {
        mBit = 0;
        ++mByte;
      }
    }
  }

private:
  const std::vector<uint8_t>& mData;
  size_t mByte = 0;
  int mBit = 0;
};

std::vector<uint8_t> makeAvcC(const MediaFormat& format) {
  const auto& csd = format.csdList();
  if (!csd.empty() && csd.front().size() >= 7 && csd.front()[0] == 1 && (csd.front()[4] & 0x03) == 3) {
    BoxWriter writer;
    const size_t box = writer.beginBox("avcC");
    writer.bytes(csd.front());
    writer.endBox(box);
    return writer.take();
  }

  const auto units = collectNalUnits(csd);
  std::vector<uint8_t> sps;
  std::vector<uint8_t> pps;
  for (const auto& unit: units) {
    if (unit.empty()) {
      continue;
    }
    const uint8_t type = unit[0] & 0x1F;
    if (type == 7 && sps.empty()) {
      sps = unit;
    } else if (type == 8 && pps.empty()) {
      pps = unit;
    }
  }
  if (sps.size() < 4 || pps.empty()) {
    return {};
  }

  BoxWriter writer;
  const size_t box = writer.beginBox("avcC");
  writer.u8(1);
  writer.u8(sps[1]);
  writer.u8(sps[2]);
  writer.u8(sps[3]);
  writer.u8(0xFF);
  writer.u8(0xE1);
  writer.u16(static_cast<uint16_t>(sps.size()));
  writer.bytes(sps);
  writer.u8(1);
  writer.u16(static_cast<uint16_t>(pps.size()));
  writer.bytes(pps);
  writer.endBox(box);
  return writer.take();
}

std::vector<uint8_t> makeHvcC(const MediaFormat& format) {
  const auto& csd = format.csdList();
  if (!csd.empty() && csd.front().size() >= 23 && csd.front()[0] == 1 && (csd.front()[21] & 0x03) == 3) {
    BoxWriter writer;
    const size_t box = writer.beginBox("hvcC");
    writer.bytes(csd.front());
    writer.endBox(box);
    return writer.take();
  }

  const auto units = collectNalUnits(csd);
  std::vector<uint8_t> vps;
  std::vector<uint8_t> sps;
  std::vector<uint8_t> pps;
  for (const auto& unit: units) {
    if (unit.size() < 3) {
      continue;
    }
    const uint8_t type = (unit[0] >> 1) & 0x3F;
    if (type == 32 && vps.empty()) {
      vps = unit;
    } else if (type == 33 && sps.empty()) {
      sps = unit;
    } else if (type == 34 && pps.empty()) {
      pps = unit;
    }
  }

  uint32_t profileSpace = 0;
  uint32_t tier = 0;
  uint32_t profileIdc = 1;
  uint32_t compatibility = 0;
  uint64_t constraints = 0;
  uint32_t levelIdc = 120;
  if (sps.size() > 5) {
    std::vector<uint8_t> rbsp(sps.begin() + 2, sps.end());
    BitReader reader(rbsp);
    reader.skip(4 + 3 + 1);
    profileSpace = reader.read(2);
    tier = reader.read(1);
    profileIdc = reader.read(5);
    compatibility = reader.read(32);
    constraints = (static_cast<uint64_t>(reader.read(16)) << 32) | (static_cast<uint64_t>(reader.read(16)) << 16) |
                  reader.read(16);
    levelIdc = reader.read(8);
  } else if (format.getProfile() != MediaFormat::NO_VALUE) {
    profileIdc = static_cast<uint32_t>(std::max(0, format.getProfile()));
  }
  if (format.getLevel() != MediaFormat::NO_VALUE) {
    levelIdc = static_cast<uint32_t>(std::max(0, format.getLevel()));
  }
  if (compatibility == 0) {
    compatibility = 0xFFFFFFFFu;
  }

  BoxWriter writer;
  const size_t box = writer.beginBox("hvcC");
  writer.u8(1);
  writer.u8(static_cast<uint8_t>((profileSpace << 6) | (tier << 5) | profileIdc));
  writer.u32(compatibility);
  writer.u8(static_cast<uint8_t>(constraints >> 40));
  writer.u8(static_cast<uint8_t>(constraints >> 32));
  writer.u8(static_cast<uint8_t>(constraints >> 24));
  writer.u8(static_cast<uint8_t>(constraints >> 16));
  writer.u8(static_cast<uint8_t>(constraints >> 8));
  writer.u8(static_cast<uint8_t>(constraints));
  writer.u8(static_cast<uint8_t>(levelIdc));
  writer.u16(0xF000);
  writer.u8(0xFC);
  writer.u8(0xFC | 1);
  writer.u8(0xF8);
  writer.u8(0xF8);
  writer.u16(0);
  writer.u8(0x03);
  writer.u8(static_cast<uint8_t>((vps.empty() ? 0 : 1) + (sps.empty() ? 0 : 1) +
                                (pps.empty() ? 0 : 1)));
  const std::array<std::pair<uint8_t, std::vector<uint8_t>*>, 3> arrays = { std::make_pair(32, &vps),
    std::make_pair(33, &sps), std::make_pair(34, &pps) };
  for (const auto& [type, unit]: arrays) {
    if (unit == nullptr || unit->empty()) {
      continue;
    }
    writer.u8(static_cast<uint8_t>(0x80 | type));
    writer.u16(1);
    writer.u16(static_cast<uint16_t>(unit->size()));
    writer.bytes(*unit);
  }
  writer.endBox(box);
  return writer.take();
}

std::vector<uint8_t> makeAv1C(const MediaFormat& format) {
  const auto& csd = format.csdList();
  if (csd.empty() || csd.front().empty()) {
    return {};
  }
  const auto& data = csd.front();

  BoxWriter writer;
  const size_t box = writer.beginBox("av1C");
  if (data.size() >= 4 && (data[0] & 0x80) != 0 && (data[0] & 0x7F) == 1) {
    writer.bytes(data);
    writer.endBox(box);
    return writer.take();
  }

  const uint32_t profile = format.getProfile() == MediaFormat::NO_VALUE
                                   ? 0
                                   : static_cast<uint32_t>(std::clamp(format.getProfile(), 0, 2));
  const uint32_t level =
          format.getLevel() == MediaFormat::NO_VALUE ? 0 : static_cast<uint32_t>(std::clamp(format.getLevel(), 0, 31));
  // 8-bit 4:2:0 by default; the sequence header remains in the config OBUs and can describe a
  // different format to a decoder that parses it.
  writer.u8(static_cast<uint8_t>(0x80 | 1)); // marker + version
  writer.u8(static_cast<uint8_t>((profile << 5) | level));
  writer.u8(0x0C); // 8-bit, 4:2:0
  writer.u8(0x00); // no initial presentation delay
  writer.bytes(data);
  writer.endBox(box);
  return writer.take();
}

int isoColorPrimaries(const ColorInfo& colorInfo) {
  switch (colorInfo.colorSpace) {
    case ColorInfo::SpaceType::kSpaceBT709:
      return 1;
    case ColorInfo::SpaceType::kSpaceBT601:
      return 5;
    case ColorInfo::SpaceType::kSpaceBT2020:
      return 9;
    default:
      return 2;
  }
}

int isoTransferCharacteristics(const ColorInfo& colorInfo) {
  switch (colorInfo.colorTransfer) {
    case ColorInfo::TransferType::kTransferSDR:
      return 1;
    case ColorInfo::TransferType::kTransferST2084:
      return 16;
    case ColorInfo::TransferType::kTransferHLG:
      return 18;
    case ColorInfo::TransferType::kTransferSRGB:
      return 13;
    case ColorInfo::TransferType::kTransferGamma22:
      return 4;
    case ColorInfo::TransferType::kTransferLinear:
      return 8;
    default:
      return 2;
  }
}

int isoMatrixCoefficients(const ColorInfo& colorInfo) {
  switch (colorInfo.colorSpace) {
    case ColorInfo::SpaceType::kSpaceBT709:
      return 1;
    case ColorInfo::SpaceType::kSpaceBT601:
      return 5;
    case ColorInfo::SpaceType::kSpaceBT2020:
      return 9;
    default:
      return 2;
  }
}

std::vector<uint8_t> makeVpcC(const MediaFormat& format) {
  BoxWriter writer;
  const size_t box = writer.beginFullBox("vpcC", 1, 0);
  const auto& csd = format.csdList();
  if (!csd.empty() && csd.front().size() >= 8 && csd.front()[0] == 1 && csd.front()[1] == 0 && csd.front()[2] == 0 &&
          csd.front()[3] == 0) {
    // The CSD already contains the version, flags and payload of a vpcC box.
    writer.bytes(csd.front().data() + 4, csd.front().size() - 4);
    writer.endBox(box);
    return writer.take();
  }

  uint8_t profile = 0;
  uint8_t level = 10;
  uint8_t bitDepth = 8;
  uint8_t chromaSubsampling = 0;
  uint8_t colorPrimaries = 1;
  uint8_t transferCharacteristics = 1;
  uint8_t matrixCoefficients = 1;

  if (!csd.empty()) {
    const auto& data = csd.front();
    size_t position = 0;
    while (position + 2 <= data.size()) {
      const uint8_t id = data[position++];
      const size_t length = data[position++];
      if (position + length > data.size()) {
        break;
      }
      const uint8_t* value = data.data() + position;
      switch (id) {
        case 1:
          if (length >= 1) {
            profile = value[0];
          }
          break;
        case 2:
          if (length >= 1) {
            level = value[0];
          }
          break;
        case 3:
          if (length >= 1) {
            bitDepth = value[0];
          }
          break;
        case 4:
          if (length >= 1) {
            chromaSubsampling = value[0];
          }
          break;
        case 5:
          if (length >= 1) {
            colorPrimaries = value[0];
          }
          break;
        case 6:
          if (length >= 1) {
            transferCharacteristics = value[0];
          }
          break;
        case 7:
          if (length >= 1) {
            matrixCoefficients = value[0];
          }
          break;
        default:
          break;
      }
      position += length;
    }
  }
  if (format.getProfile() != MediaFormat::NO_VALUE) {
    profile = static_cast<uint8_t>(std::clamp(format.getProfile(), 0, 3));
  }
  if (format.getLevel() != MediaFormat::NO_VALUE) {
    level = static_cast<uint8_t>(std::clamp(format.getLevel(), 0, 255));
  }
  const ColorInfo& colorInfo = format.getColorInfo();
  if (colorInfo.colorSpace != ColorInfo::SpaceType::kSpaceUnknown) {
    colorPrimaries = static_cast<uint8_t>(isoColorPrimaries(colorInfo));
    matrixCoefficients = static_cast<uint8_t>(isoMatrixCoefficients(colorInfo));
  }
  if (colorInfo.colorTransfer != ColorInfo::TransferType::kTransferUnknown) {
    transferCharacteristics = static_cast<uint8_t>(isoTransferCharacteristics(colorInfo));
  }
  const uint8_t videoFullRange =
    colorInfo.colorRange == ColorInfo::RangeType::kRangeFull ? 1 : 0;

  writer.u8(profile);
  writer.u8(level);
  writer.u8(static_cast<uint8_t>(((bitDepth & 0x0F) << 4) | ((chromaSubsampling & 0x07) << 1) |
                                 videoFullRange));
  writer.u8(colorPrimaries);
  writer.u8(transferCharacteristics);
  writer.u8(matrixCoefficients);
  writer.u16(0); // codec initialization data size; unused for VP9 in MP4
  writer.endBox(box);
  return writer.take();
}

uint16_t readLittleEndian16(const std::vector<uint8_t>& data, const size_t offset) {
  if (offset + 2 > data.size()) {
    return 0;
  }
  return static_cast<uint16_t>(data[offset] | (data[offset + 1] << 8));
}

std::vector<uint8_t> makeColr(const ColorInfo& colorInfo) {
  BoxWriter writer;
  const size_t box = writer.beginBox("colr");
  writer.fourcc("nclx");
  writer.u16(static_cast<uint16_t>(isoColorPrimaries(colorInfo)));
  writer.u16(static_cast<uint16_t>(isoTransferCharacteristics(colorInfo)));
  writer.u16(static_cast<uint16_t>(isoMatrixCoefficients(colorInfo)));
  writer.u8(colorInfo.colorRange == ColorInfo::RangeType::kRangeFull ? 0x80 : 0);
  writer.endBox(box);
  return writer.take();
}

std::vector<uint8_t> makeMdcv(const std::vector<uint8_t>& hdrStaticInfo) {
  if (hdrStaticInfo.size() < 25 ||
      std::all_of(hdrStaticInfo.begin() + 1, hdrStaticInfo.begin() + 21,
        [](const uint8_t value) { return value == 0; })) {
    return {};
  }
  BoxWriter writer;
  const size_t box = writer.beginBox("mdcv");
  for (size_t i = 0; i < 8; ++i) {
    writer.u16(readLittleEndian16(hdrStaticInfo, 1 + i * 2));
  }
  const auto maxLuminance = static_cast<uint32_t>(
    readLittleEndian16(hdrStaticInfo, 17)) * 10000u;
  const auto minLuminance = static_cast<uint32_t>(
    readLittleEndian16(hdrStaticInfo, 19)) * 10000u;
  writer.u32(maxLuminance);
  writer.u32(minLuminance);
  writer.endBox(box);
  return writer.take();
}

std::vector<uint8_t> makeClli(const std::vector<uint8_t>& hdrStaticInfo) {
  if (hdrStaticInfo.size() < 25 ||
      std::all_of(hdrStaticInfo.begin() + 21, hdrStaticInfo.end(),
        [](const uint8_t value) { return value == 0; })) {
    return {};
  }
  BoxWriter writer;
  const size_t box = writer.beginBox("clli");
  writer.u16(readLittleEndian16(hdrStaticInfo, 21));
  writer.u16(readLittleEndian16(hdrStaticInfo, 23));
  writer.endBox(box);
  return writer.take();
}

struct DolbyVisionConfig {
  int profile = 8;
  int level = 6;
};

DolbyVisionConfig dolbyVisionConfig(const MediaFormat& format) {
  DolbyVisionConfig config;
  const std::string& codecs = format.getCodecs();
  const size_t first = codecs.find('.');
  const size_t second = first == std::string::npos ? std::string::npos : codecs.find('.', first + 1);
  if (first != std::string::npos && second != std::string::npos) {
    config.profile = std::atoi(codecs.c_str() + first + 1);
    config.level = std::atoi(codecs.c_str() + second + 1);
  }
  if (config.profile <= 0 && format.getProfile() != MediaFormat::NO_VALUE) {
    config.profile = format.getProfile();
  }
  if (config.level <= 0 && format.getLevel() != MediaFormat::NO_VALUE) {
    config.level = format.getLevel();
  }
  return config;
}

std::vector<uint8_t> makeDolbyVisionConfig(const DolbyVisionConfig& config) {
  std::vector<uint8_t> data(24, 0);
  uint8_t blCompatibilityId = 0;
  uint8_t mdCompression = 0;
  if (config.profile == 8) {
    blCompatibilityId = 4;
  } else if (config.profile == 9) {
    blCompatibilityId = 2;
    mdCompression = 1;
  }
  data[0] = 1; // dv_version_major
  data[1] = 0; // dv_version_minor
  data[2] = static_cast<uint8_t>(((config.profile & 0x7F) << 1) |
                                 ((config.level >> 5) & 0x01));
  data[3] = static_cast<uint8_t>(((config.level & 0x1F) << 3) | (1 << 2) | 1);
  data[4] = static_cast<uint8_t>((blCompatibilityId << 4) | (mdCompression << 2));
  return data;
}

std::vector<uint8_t> makeDolbyVisionSpecificBox(const MediaFormat& format) {
  const DolbyVisionConfig config = dolbyVisionConfig(format);
  std::vector<uint8_t> base = config.profile <= 8 ? makeHvcC(format) : makeAvcC(format);
  if (base.empty()) {
    return {};
  }
  const char* boxType = config.profile == 5 ? "dvcC"
                        : config.profile == 8 || config.profile == 9 ? "dvvC"
                                                                     : nullptr;
  if (boxType == nullptr) {
    return {};
  }
  BoxWriter writer;
  const size_t box = writer.beginBox(boxType);
  writer.bytes(makeDolbyVisionConfig(config));
  writer.endBox(box);
  const auto dovi = writer.take();
  base.insert(base.end(), dovi.begin(), dovi.end());
  return base;
}

std::vector<uint8_t> makeD263(const MediaFormat& format) {
  int profile = 0;
  int level = 10;
  const std::string& codecs = format.getCodecs();
  const size_t first = codecs.find('.');
  const size_t second = first == std::string::npos ? std::string::npos : codecs.find('.', first + 1);
  if (first != std::string::npos && second != std::string::npos) {
    profile = std::atoi(codecs.c_str() + first + 1);
    level = std::atoi(codecs.c_str() + second + 1);
  }
  BoxWriter writer;
  const size_t box = writer.beginBox("d263");
  writer.fourcc("    "); // vendor
  writer.u8(0);          // decoder version
  writer.u8(static_cast<uint8_t>(std::clamp(level, 0, 255)));
  writer.u8(static_cast<uint8_t>(std::clamp(profile, 0, 255)));
  writer.endBox(box);
  return writer.take();
}

std::vector<uint8_t> makeAudioSpecificConfig(const MediaFormat& format) {
  const auto& csd = format.csdList();
  if (!csd.empty() && csd.front().size() >= 2) {
    return csd.front();
  }
  const int objectType = 2; // AAC-LC
  const int sampleRate = format.getSampleRate() > 0 ? format.getSampleRate() : 48000;
  const int frequencyIndex = findFrequencyIndex(sampleRate);
  const int channelConfig = std::clamp(format.getChannelCount(), 1, 7);
  const uint32_t value = (static_cast<uint32_t>(objectType) << 11) | (static_cast<uint32_t>(frequencyIndex) << 7) |
                         (static_cast<uint32_t>(channelConfig - 1) << 3);
  return { static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value) };
}

std::vector<uint8_t> makeEsds(const uint8_t objectType, const uint8_t streamType,
        const std::vector<uint8_t>& decoderSpecific, const int averageBitrate, const int maxBitrate) {
  BoxWriter decoderConfig;
  decoderConfig.u8(objectType);
  decoderConfig.u8(static_cast<uint8_t>((streamType << 2) | 1));
  decoderConfig.u24(0);
  decoderConfig.u32(static_cast<uint32_t>(std::max(0, maxBitrate)));
  decoderConfig.u32(static_cast<uint32_t>(std::max(0, averageBitrate)));
  BoxWriter specific;
  specific.bytes(decoderSpecific);
  appendDescriptor(decoderConfig, 0x05, specific.data());

  BoxWriter es;
  es.u16(0);
  es.u8(0);
  appendDescriptor(es, 0x04, decoderConfig.data());
  BoxWriter slConfig;
  slConfig.u8(0x02);
  appendDescriptor(es, 0x06, slConfig.data());

  BoxWriter writer;
  const size_t box = writer.beginFullBox("esds", 0, 0);
  appendDescriptor(writer, 0x03, es.data());
  writer.endBox(box);
  return writer.take();
}

std::vector<uint8_t> makeDOps(const MediaFormat& format) {
  std::vector<uint8_t> payload;
  const auto& csd = format.csdList();
  if (!csd.empty() && !csd.front().empty()) {
    const auto& data = csd.front();
    if (data.size() > 8 && std::memcmp(data.data(), "OpusHead", 8) == 0) {
      payload.assign(data.begin() + 8, data.end());
    } else if (data.size() >= 11) {
      payload = data;
    }
  }

  if (payload.size() < 11) {
    payload.clear();
    payload.push_back(0); // version
    payload.push_back(static_cast<uint8_t>(std::max(0, format.getChannelCount())));
    const int preSkip = format.getEncoderDelay() > 0 ? format.getEncoderDelay() : 3840;
    payload.push_back(static_cast<uint8_t>(preSkip >> 8));
    payload.push_back(static_cast<uint8_t>(preSkip));
    const int sampleRate = format.getSampleRate() > 0 ? format.getSampleRate() : 48000;
    payload.push_back(static_cast<uint8_t>(sampleRate >> 24));
    payload.push_back(static_cast<uint8_t>(sampleRate >> 16));
    payload.push_back(static_cast<uint8_t>(sampleRate >> 8));
    payload.push_back(static_cast<uint8_t>(sampleRate));
    payload.push_back(0);
    payload.push_back(0);
    payload.push_back(0); // channel mapping family 0
  }
  // For encapsulation in MP4, the OpusHead version byte must be 0.
  payload[0] = 0;

  BoxWriter writer;
  const size_t box = writer.beginBox("dOps");
  writer.bytes(payload);
  writer.endBox(box);
  return writer.take();
}

std::vector<uint8_t> makeDFla(const MediaFormat& format) {
  const auto& csd = format.csdList();
  if (csd.empty() || csd.front().empty()) {
    return {};
  }
  const auto& data = csd.front();
  size_t offset = 0;
  if (data.size() > 4 && std::memcmp(data.data(), "fLaC", 4) == 0) {
    offset = 4;
  } else if ((data[0] & 0x7F) != 0) {
    // Not a FLAC metadata block header; keep the whole payload.
    offset = 0;
  }

  BoxWriter writer;
  const size_t box = writer.beginFullBox("dfLa", 0, 0);
  writer.bytes(data.data() + offset, data.size() - offset);
  writer.endBox(box);
  return writer.take();
}

uint8_t channelConfiguration(const int channelCount) {
  switch (std::clamp(channelCount, 1, 6)) {
    case 1:
      return 1;
    case 2:
      return 2;
    case 3:
      return 3;
    case 4:
      return 4;
    case 5:
      return 5;
    default:
      return 7;
  }
}

uint8_t sampleRateCode(const int sampleRate) {
  switch (sampleRate) {
    case 48000:
      return 0;
    case 44100:
      return 1;
    case 32000:
      return 2;
    default:
      return 0;
  }
}

std::vector<uint8_t> makeDac3(const MediaFormat& format) {
  if (!format.csdList().empty() && format.csdList().front().size() == 3) {
    return format.csdList().front();
  }
  const uint32_t fscod = sampleRateCode(format.getSampleRate());
  const uint32_t acmod = channelConfiguration(format.getChannelCount());
  const uint32_t lfeon = format.getChannelCount() >= 6 ? 1 : 0;
  const uint32_t value = (fscod << 22) | (8u << 17) | (0u << 14) | (acmod << 11) | (lfeon << 10) | (10u << 5);
  return { static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value) };
}

std::vector<uint8_t> makeDec3(const MediaFormat& format) {
  if (!format.csdList().empty() && format.csdList().front().size() >= 5) {
    return format.csdList().front();
  }
  const int dataRate = std::clamp(format.getBitrate() > 0 ? format.getBitrate() / 1000 : 0, 0, 8191);
  const uint32_t fscod = sampleRateCode(format.getSampleRate());
  const uint32_t acmod = channelConfiguration(format.getChannelCount());
  const uint32_t lfeon = format.getChannelCount() >= 6 ? 1 : 0;
  const uint16_t header = static_cast<uint16_t>((dataRate << 3) | 0);
  const uint32_t substream = (fscod << 22) | (16u << 17) | (0u << 12) | (acmod << 9) | (lfeon << 8) | (0u << 1);
  return { static_cast<uint8_t>(header >> 8), static_cast<uint8_t>(header), static_cast<uint8_t>(substream >> 16),
    static_cast<uint8_t>(substream >> 8), static_cast<uint8_t>(substream) };
}

std::vector<uint8_t> makeCodecSpecificBox(const Codec codec, const MediaFormat& format) {
  switch (codec) {
    case Codec::kH264:
      return makeAvcC(format);
    case Codec::kHevc:
      return makeHvcC(format);
    case Codec::kAv1:
      return makeAv1C(format);
    case Codec::kVp9:
      return makeVpcC(format);
    case Codec::kMpeg4:
      return makeEsds(0x20, 4, format.csdList().empty() ? std::vector<uint8_t>() : format.csdList().front(),
              format.getAverageBitrate(), format.getBitrate());
    case Codec::kAac:
      return makeEsds(0x40, 5, makeAudioSpecificConfig(format), format.getAverageBitrate(), format.getBitrate());
    case Codec::kOpus:
      return makeDOps(format);
    case Codec::kFlac:
      return makeDFla(format);
    case Codec::kAc3:
      return [&format]() {
        BoxWriter writer;
        const size_t box = writer.beginBox("dac3");
        writer.bytes(makeDac3(format));
        writer.endBox(box);
        return writer.take();
      }();
    case Codec::kEac3:
      return [&format]() {
        BoxWriter writer;
        const size_t box = writer.beginBox("dec3");
        writer.bytes(makeDec3(format));
        writer.endBox(box);
        return writer.take();
      }();
    case Codec::kH263:
      return makeD263(format);
    case Codec::kDolbyVision:
      return makeDolbyVisionSpecificBox(format);
    case Codec::kRawAudio:
      return {};
    case Codec::kUnknown:
    default:
      return {};
  }
}

const char* sampleEntryFourcc(const Codec codec, const MediaFormat& format) {
  switch (codec) {
    case Codec::kH264:
      return "avc1";
    case Codec::kHevc:
      return "hvc1";
    case Codec::kAv1:
      return "av01";
    case Codec::kVp9:
      return "vp09";
    case Codec::kMpeg4:
      return "mp4v";
    case Codec::kAac:
      return "mp4a";
    case Codec::kOpus:
      return "Opus";
    case Codec::kFlac:
      return "fLaC";
    case Codec::kAc3:
      return "ac-3";
    case Codec::kEac3:
      return "ec-3";
    case Codec::kH263:
      return "s263";
    case Codec::kDolbyVision: {
      const int profile = dolbyVisionConfig(format).profile;
      return profile == 5 ? "dvh1" : profile == 9 ? "avc1" : "hvc1";
    }
    case Codec::kRawAudio:
      return "sowt";
    case Codec::kCea608:
      return "c608";
    case Codec::kCea708:
      return "c708";
    case Codec::kTx3g:
      return "tx3g";
    case Codec::kWvtt:
      return "wvtt";
    case Codec::kStpp:
      return "stpp";
    case Codec::kUnknown:
    default:
      return "";
  }
}

void writeVisualSampleEntry(BoxWriter& writer, const char* fourcc, const MediaFormat& format,
        const std::vector<uint8_t>& codecSpecificBox) {
  const size_t entry = writer.beginBox(fourcc);
  writer.zeros(6);
  writer.u16(1); // data reference index
  writer.u16(0);
  writer.u16(0);
  writer.zeros(12);
  writer.u16(static_cast<uint16_t>(std::max(0, format.getWidth())));
  writer.u16(static_cast<uint16_t>(std::max(0, format.getHeight())));
  writer.u32(0x00480000); // 72 dpi
  writer.u32(0x00480000);
  writer.u32(0);
  writer.u16(1);      // frame count
  writer.zeros(32);   // compressorname
  writer.u16(0x0018); // depth
  writer.i16(-1);     // pre-defined
  writer.bytes(codecSpecificBox);

  const size_t pasp = writer.beginBox("pasp");
  writer.u32(1);
  writer.u32(1);
  writer.endBox(pasp);

  const ColorInfo& colorInfo = format.getColorInfo();
  if (colorInfo.colorSpace != ColorInfo::SpaceType::kSpaceUnknown ||
      colorInfo.colorTransfer != ColorInfo::TransferType::kTransferUnknown ||
      colorInfo.colorRange != ColorInfo::RangeType::kRangeUnknown) {
    writer.bytes(makeColr(colorInfo));
  }
  writer.bytes(makeMdcv(colorInfo.hdrStaticInfo));
  writer.bytes(makeClli(colorInfo.hdrStaticInfo));
  writer.endBox(entry);
}

void writeCaptionSampleEntry(BoxWriter& writer, const char* fourcc, const MediaFormat& format) {
  const size_t entry = writer.beginBox(fourcc);
  writer.zeros(6);
  writer.u16(1); // data reference index
  // The optional clcp box states the caption service, the language and the accessibility flags.
  const size_t clcp = writer.beginBox("clcp");
  const int serviceNumber = format.getCaptionServiceNumber();
  writer.u8(static_cast<uint8_t>(serviceNumber > 0 ? serviceNumber : 1));
  std::string language = format.getLanguage();
  if (language.size() != 3) {
    language = "und";
  }
  writer.bytes(reinterpret_cast<const uint8_t*>(language.data()), 3);
  writer.u8(0); // easy reader
  writer.u8(0); // wide aspect
  writer.endBox(clcp);
  writer.endBox(entry);
}

void writeTextSampleEntry(BoxWriter& writer, const Codec codec, const char* fourcc,
        const MediaFormat& format) {
  const size_t entry = writer.beginBox(fourcc);
  writer.zeros(6);
  writer.u16(1); // data reference index
  if (codec == Codec::kTx3g) {
    if (!format.csdList().empty() && !format.csdList().front().empty()) {
      // The style and the font table that the extractor read from the source sample entry.
      writer.bytes(format.csdList().front());
    } else {
      // The default style of the 3GPP timed text sample entry: bottom centred white text.
      writer.u32(0);          // display flags
      writer.u8(1);           // horizontal justification
      writer.u8(1);           // vertical justification
      writer.u32(0);          // background colour
      writer.zeros(8);        // text box
      writer.u16(0);          // start char
      writer.u16(0);          // end char
      writer.u16(1);          // font id
      writer.u8(0);           // face style flags
      writer.u8(18);          // font size
      writer.u32(0xFFFFFFFF); // text colour
    }
  } else if (codec == Codec::kWvtt) {
    const size_t config = writer.beginBox("vttC");
    writer.string("WEBVTT");
    writer.endBox(config);
  } else {
    // The namespace, the schema location and the auxiliary MIME type of a TTML track.
    writer.string("http://www.w3.org/ns/ttml");
    writer.u8(0);
    writer.string("http://www.w3.org/ns/ttml#metadata");
    writer.u8(0);
    writer.u8(0);
  }
  writer.endBox(entry);
}

void writeAudioSampleEntry(BoxWriter& writer, const char* fourcc, const MediaFormat& format,
        const std::vector<uint8_t>& codecSpecificBox) {
  const size_t entry = writer.beginBox(fourcc);
  writer.zeros(6);
  writer.u16(1); // data reference index
  writer.u16(0);
  writer.u16(0);
  writer.zeros(4);
  writer.u16(static_cast<uint16_t>(std::max(0, format.getChannelCount())));
  writer.u16(16); // sample size
  writer.u16(0);
  writer.u16(0);
  writer.u32(static_cast<uint32_t>(std::max(0, format.getSampleRate())) << 16);
  writer.bytes(codecSpecificBox);
  writer.endBox(entry);
}

} // namespace

Codec codecFor(const MediaFormat& format) {
  const std::string& mime = format.getMimeType();
  if (mime == "video/avc" || mime == "video/h264") {
    return Codec::kH264;
  }
  if (mime == "video/hevc" || mime == "video/h265") {
    return Codec::kHevc;
  }
  if (mime == "video/av01") {
    return Codec::kAv1;
  }
  if (mime == "video/x-vnd.on2.vp9" || mime == "video/vp9") {
    return Codec::kVp9;
  }
  if (mime == "video/mp4v-es" || mime == "video/mp4v") {
    return Codec::kMpeg4;
  }
  if (mime == "video/3gpp" || mime == "video/h263") {
    return Codec::kH263;
  }
  if (mime == "video/dolby-vision") {
    return Codec::kDolbyVision;
  }
  if (mime == "audio/mp4a-latm" || mime == "audio/aac") {
    return Codec::kAac;
  }
  if (mime == "audio/opus") {
    return Codec::kOpus;
  }
  if (mime == "audio/flac") {
    return Codec::kFlac;
  }
  if (mime == "audio/ac3") {
    return Codec::kAc3;
  }
  if (mime == "audio/eac3" || mime == "audio/eac3-joc") {
    return Codec::kEac3;
  }
  if (mime == "audio/raw") {
    return Codec::kRawAudio;
  }
  if (mime == "application/cea-708") {
    return Codec::kCea708;
  }
  if (mime == "application/x-mp4-cea-608" || mime == "application/cea-608" || mime == "application/x-cea-608") {
    return Codec::kCea608;
  }
  if (mime == "application/x-quicktime-tx3g") {
    return Codec::kTx3g;
  }
  if (mime == "application/x-mp4-vtt") {
    return Codec::kWvtt;
  }
  if (mime == "application/ttml+xml") {
    return Codec::kStpp;
  }

  switch (format.getVideoCodecType()) {
    case VideoCodecType::kVideoCodecTypeH264:
      return Codec::kH264;
    case VideoCodecType::kVideoCodecTypeHevc:
      return Codec::kHevc;
    case VideoCodecType::kVideoCodecTypeAV1:
      return Codec::kAv1;
    case VideoCodecType::kVideoCodecTypeVP9:
      return Codec::kVp9;
    case VideoCodecType::kVideoCodecTypeMpeg4Video:
      return Codec::kMpeg4;
    case VideoCodecType::kVideoCodecTypeH263:
      return Codec::kH263;
    case VideoCodecType::kVideoCodecTypeDolbyVision:
      return Codec::kDolbyVision;
    default:
      break;
  }

  switch (format.getAudioFormatType()) {
    case AudioFormatType::kAudioFormatTypeMpeg4AAC:
      return Codec::kAac;
    case AudioFormatType::kAudioFormatTypeOpus:
      return Codec::kOpus;
    case AudioFormatType::kAudioFormatTypeFlac:
      return Codec::kFlac;
    case AudioFormatType::kAudioFormatTypeLinearPCM:
      return Codec::kRawAudio;
    case AudioFormatType::kAudioFormatTypeAC3:
      return mime == "audio/eac3" || mime == "audio/eac3-joc" ? Codec::kEac3 : Codec::kAc3;
    default:
      break;
  }
  return Codec::kUnknown;
}

bool isAudioCodec(const Codec codec) {
  return codec == Codec::kAac || codec == Codec::kOpus || codec == Codec::kFlac || codec == Codec::kAc3 ||
         codec == Codec::kEac3 || codec == Codec::kRawAudio;
}

bool isVideoCodec(const Codec codec) {
  return codec == Codec::kH264 || codec == Codec::kHevc || codec == Codec::kAv1 || codec == Codec::kVp9 ||
         codec == Codec::kMpeg4 || codec == Codec::kH263 || codec == Codec::kDolbyVision;
}

bool isCaptionCodec(const Codec codec) {
  return codec == Codec::kCea608 || codec == Codec::kCea708;
}

bool isTextCodec(const Codec codec) {
  return isCaptionCodec(codec) || codec == Codec::kTx3g || codec == Codec::kWvtt || codec == Codec::kStpp;
}

bool requiresCodecSpecificData(const Codec codec) {
  return codec == Codec::kH264 || codec == Codec::kHevc || codec == Codec::kAv1 || codec == Codec::kMpeg4 ||
         codec == Codec::kFlac || codec == Codec::kDolbyVision;
}

int32_t chooseTrackTimescale(const Codec codec, const MediaFormat& format) {
  if (isAudioCodec(codec)) {
    return format.getSampleRate() > 0 ? format.getSampleRate() : 48000;
  }
  return 90000;
}

int64_t defaultSampleDurationUs(const Codec codec, const MediaFormat& format) {
  if (isAudioCodec(codec)) {
    const int sampleRate = format.getSampleRate() > 0 ? format.getSampleRate() : 48000;
    switch (codec) {
      case Codec::kAac:
        return std::max<int64_t>(1, std::llround(1024.0 * 1'000'000.0 / sampleRate));
      case Codec::kOpus:
        return std::max<int64_t>(1, std::llround(960.0 * 1'000'000.0 / sampleRate));
      case Codec::kFlac:
        return std::max<int64_t>(1, std::llround(4096.0 * 1'000'000.0 / sampleRate));
      case Codec::kAc3:
      case Codec::kEac3:
        return std::max<int64_t>(1, std::llround(1536.0 * 1'000'000.0 / sampleRate));
      case Codec::kRawAudio:
        return std::max<int64_t>(1, std::llround(1024.0 * 1'000'000.0 / sampleRate));
      default:
        break;
    }
    return 0;
  }
  if (format.getFrameRate() > 0) {
    return std::max<int64_t>(1, 1'000'000 / format.getFrameRate());
  }
  return 33333;
}

bool startsWithStartCode(const uint8_t* data, const size_t size) {
  if (data == nullptr || size < 3) {
    return false;
  }
  if (data[0] == 0 && data[1] == 0 && data[2] == 1) {
    return true;
  }
  return size >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1;
}

std::vector<std::vector<uint8_t>> splitNalUnits(const std::vector<uint8_t>& data) {
  std::vector<std::vector<uint8_t>> units;
  size_t position = 0;
  while (position + 3 < data.size()) {
    size_t start = std::string::npos;
    size_t startCodeLength = 0;
    for (size_t i = position; i + 2 < data.size(); ++i) {
      if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
        start = i;
        startCodeLength = 3;
        break;
      }
      if (i + 3 < data.size() && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1) {
        start = i;
        startCodeLength = 4;
        break;
      }
    }
    if (start == std::string::npos) {
      break;
    }
    const size_t nalStart = start + startCodeLength;
    size_t next = data.size();
    for (size_t i = nalStart; i + 2 < data.size(); ++i) {
      if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
        next = i;
        break;
      }
      if (i + 3 < data.size() && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1) {
        next = i;
        break;
      }
    }
    if (nalStart < next) {
      units.emplace_back(data.begin() + static_cast<std::ptrdiff_t>(nalStart),
              data.begin() + static_cast<std::ptrdiff_t>(next));
    }
    position = next;
  }
  if (units.empty() && !data.empty()) {
    units.push_back(data);
  }
  return units;
}

std::vector<uint8_t> annexBToLengthPrefixed(const uint8_t* data, const size_t size) {
  std::vector<std::vector<uint8_t>> units = splitNalUnits(std::vector<uint8_t>(data, data + size));
  std::vector<uint8_t> output;
  output.reserve(size);
  for (const auto& unit: units) {
    writeU32(output, static_cast<uint32_t>(unit.size()));
    output.insert(output.end(), unit.begin(), unit.end());
  }
  return output;
}

std::vector<uint8_t> wrapCaptionSample(const uint8_t* data, const size_t size) {
  std::vector<uint8_t> box;
  box.reserve(size + 8);
  const auto boxSize = static_cast<uint32_t>(size + 8);
  for (int shift = 24; shift >= 0; shift -= 8) {
    box.push_back(static_cast<uint8_t>(boxSize >> shift));
  }
  box.push_back('c');
  box.push_back('d');
  box.push_back('a');
  box.push_back('t');
  if (data != nullptr && size > 0) {
    box.insert(box.end(), data, data + size);
  }
  return box;
}

void writeSampleEntry(BoxWriter& writer, const Codec codec, const MediaFormat& format) {
  const char* fourcc = sampleEntryFourcc(codec, format);
  if (fourcc[0] == '\0') {
    return;
  }
  const std::vector<uint8_t> codecSpecificBox = makeCodecSpecificBox(codec, format);
  if (isCaptionCodec(codec)) {
    writeCaptionSampleEntry(writer, fourcc, format);
  } else if (isTextCodec(codec)) {
    writeTextSampleEntry(writer, codec, fourcc, format);
  } else if (isVideoCodec(codec)) {
    writeVisualSampleEntry(writer, fourcc, format, codecSpecificBox);
  } else {
    writeAudioSampleEntry(writer, fourcc, format, codecSpecificBox);
  }
}

} // namespace mp4
} // namespace muxer
} // namespace coral
