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

#include "H264DxvaParser.h"

#if defined(WINDOWS) || defined(WIN32)

#include <minilog/log.h>

#include <algorithm>
#include <climits>
#include <cstring>

namespace coral::d3d11va {

namespace {

/** The maximum number of the parameter sets of one stream the parser keeps. */
constexpr size_t kMaxParameterSets = 32;
constexpr size_t kMaxReferenceFrames = 32;
constexpr size_t kMaxMmcoOperations = 66;

bool startsWithStartCode(const uint8_t* data, const size_t size, size_t& codeSize) {
  if (size >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) {
    codeSize = 4;
    return true;
  }
  if (size >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1) {
    codeSize = 3;
    return true;
  }
  return false;
}

/** The position of the next start code at or after `from`, or `npos`. */
size_t findStartCode(const uint8_t* data, const size_t size, const size_t from, size_t& codeSize) {
  for (size_t i = from; i + 2 < size; ++i) {
    if (data[i] == 0 && data[i + 1] == 0) {
      if (data[i + 2] == 1) {
        codeSize = 3;
        return i;
      }
      if (i + 3 < size && data[i + 2] == 0 && data[i + 3] == 1) {
        codeSize = 4;
        return i;
      }
    }
  }
  return SIZE_MAX;
}

/**
 * Whether the whole buffer splits into length prefixed NAL units.
 */
bool validateLengthPrefixed(const uint8_t* data, const size_t size, const int lengthSize) {
  if (data == nullptr || size == 0 || lengthSize < 1 || lengthSize > 4) {
    return false;
  }
  size_t offset = 0;
  size_t count = 0;
  while (offset < size) {
    if (offset + static_cast<size_t>(lengthSize) > size) {
      return false;
    }
    size_t length = 0;
    for (int i = 0; i < lengthSize; ++i) {
      length = (length << 8) | data[offset + static_cast<size_t>(i)];
    }
    offset += static_cast<size_t>(lengthSize);
    if (length == 0 || offset + length > size) {
      return false;
    }
    const int type = data[offset] & 0x1f;
    if (type == 0 || type > 23) {
      return false;
    }
    offset += length;
    ++count;
  }
  return count > 0 && offset == size;
}

bool skipScalingList(BitReader& reader, const int size) {
  int lastScale = 8;
  int nextScale = 8;
  for (int index = 0; index < size; ++index) {
    if (nextScale != 0) {
      int32_t delta = 0;
      if (!reader.readSe(delta)) {
        return false;
      }
      nextScale = (lastScale + delta + 256) % 256;
    }
    lastScale = nextScale == 0 ? lastScale : nextScale;
  }
  return true;
}

bool parseHrdParameters(BitReader& reader) {
  uint32_t count = 0;
  if (!reader.readUe(count) || count > 31) {
    return false;
  }
  uint32_t ignored = 0;
  if (!reader.readBits(4, ignored) || !reader.readBits(4, ignored)) {
    return false;
  }
  for (uint32_t index = 0; index <= count; ++index) {
    uint32_t value = 0;
    if (!reader.readUe(value) || !reader.readUe(value) || !reader.readBit(value)) {
      return false;
    }
  }
  if (!reader.readBits(5, ignored) || !reader.readBits(5, ignored) || !reader.readBits(5, ignored) ||
          !reader.readBits(5, ignored)) {
    return false;
  }
  return true;
}

/**
 * Reads the VUI of an SPS. The fields the decoder uses are the colour description and the
 * `max_num_reorder_frames`; the rest is consumed so that the following syntax is reached.
 */
bool parseVui(BitReader& reader, Sps& sps) {
  uint32_t flag = 0;
  if (!reader.readBit(flag)) {
    return false;
  }
  if (flag) {
    uint32_t aspectRatioIdc = 0;
    if (!reader.readBits(8, aspectRatioIdc)) {
      return false;
    }
    if (aspectRatioIdc == 255) {
      uint32_t ignored = 0;
      if (!reader.readBits(16, ignored) || !reader.readBits(16, ignored)) {
        return false;
      }
    }
  }
  if (!reader.readBit(flag)) {
    return false;
  }
  if (flag) {
    uint32_t overscan = 0;
    if (!reader.readBit(overscan)) {
      return false;
    }
  }
  if (!reader.readBit(flag)) {
    return false;
  }
  if (flag) {
    uint32_t videoFormat = 0;
    uint32_t fullRange = 0;
    uint32_t colourDescription = 0;
    if (!reader.readBits(3, videoFormat) || !reader.readBit(fullRange) || !reader.readBit(colourDescription)) {
      return false;
    }
    sps.color.colorRange = fullRange ? ColorInfo::RangeType::kRangeFull : ColorInfo::RangeType::kRangeLimited;
    if (colourDescription) {
      uint32_t primaries = 0;
      uint32_t transfer = 0;
      uint32_t matrix = 0;
      if (!reader.readBits(8, primaries) || !reader.readBits(8, transfer) || !reader.readBits(8, matrix)) {
        return false;
      }
      sps.color.colorSpace = ColorInfo::isoColorPrimariesToColorSpace(static_cast<int>(primaries));
      if (sps.color.colorSpace == ColorInfo::SpaceType::kSpaceUnknown) {
        if (matrix == 1) {
          sps.color.colorSpace = ColorInfo::SpaceType::kSpaceBT709;
        } else if (matrix == 5 || matrix == 6) {
          sps.color.colorSpace = ColorInfo::SpaceType::kSpaceBT601;
        } else if (matrix == 9 || matrix == 10) {
          sps.color.colorSpace = ColorInfo::SpaceType::kSpaceBT2020;
        }
      }
      sps.color.colorTransfer = ColorInfo::isoTransferCharacteristicsToColorTransfer(static_cast<int>(transfer));
    }
  }
  if (!reader.readBit(flag)) {
    return false;
  }
  if (flag) {
    uint32_t ignored = 0;
    if (!reader.readUe(ignored) || !reader.readUe(ignored)) {
      return false;
    }
  }
  if (!reader.readBit(flag)) {
    return false;
  }
  if (flag) {
    uint32_t ignored = 0;
    if (!reader.readBits(32, ignored) || !reader.readBits(32, ignored) || !reader.readBit(ignored)) {
      return false;
    }
  }
  uint32_t nalHrd = 0;
  uint32_t vclHrd = 0;
  if (!reader.readBit(nalHrd) || !reader.readBit(vclHrd)) {
    return false;
  }
  if (nalHrd && !parseHrdParameters(reader)) {
    return false;
  }
  if (vclHrd && !parseHrdParameters(reader)) {
    return false;
  }
  if (nalHrd || vclHrd) {
    uint32_t lowDelay = 0;
    if (!reader.readBit(lowDelay)) {
      return false;
    }
  }
  uint32_t picStruct = 0;
  if (!reader.readBit(picStruct)) {
    return false;
  }
  if (!reader.readBit(flag)) {
    return false;
  }
  if (flag) {
    uint32_t ignored = 0;
    if (!reader.readBit(ignored) || !reader.readUe(ignored) || !reader.readUe(ignored) || !reader.readUe(ignored) ||
            !reader.readUe(ignored)) {
      return false;
    }
    uint32_t reorderFrames = 0;
    uint32_t maxDecFrameBuffering = 0;
    if (!reader.readUe(reorderFrames) || !reader.readUe(maxDecFrameBuffering)) {
      return false;
    }
    if (reorderFrames <= 16) {
      sps.maxNumReorderFrames = static_cast<int>(reorderFrames);
    }
  }
  return true;
}

} // namespace

// --------------------------------------------------------------------------------------------------

int nalLengthSizeOf(const std::vector<uint8_t>& csd) {
  // An `AVCDecoderConfigurationRecord` starts with configurationVersion 1 and stores the length size
  // in the fifth byte. The framework's MP4 extractor keeps the parameter sets rather than the record,
  // in which case the default four byte length applies.
  if (csd.size() >= 7 && csd[0] == 1) {
    return (csd[4] & 0x03) + 1;
  }
  return 4;
}

bool splitNalUnits(const uint8_t* data, size_t size, int lengthSize, NalFormat format, std::vector<NalUnit>& units) {
  units.clear();
  if (data == nullptr || size == 0) {
    return false;
  }
  if (lengthSize < 1 || lengthSize > 4) {
    lengthSize = 4;
  }

  if (format == NalFormat::kLengthPrefixed) {
    size_t offset = 0;
    while (offset < size) {
      if (offset + static_cast<size_t>(lengthSize) > size) {
        return false;
      }
      size_t length = 0;
      for (int i = 0; i < lengthSize; ++i) {
        length = (length << 8) | data[offset + static_cast<size_t>(i)];
      }
      offset += static_cast<size_t>(lengthSize);
      if (length == 0 || offset + length > size || length > INT_MAX) {
        return false;
      }
      NalUnit unit;
      unit.data = data + offset;
      unit.size = length;
      unit.type = data[offset] & 0x1f;
      unit.refIdc = (data[offset] >> 5) & 0x03;
      unit.offset = offset;
      units.push_back(unit);
      offset += length;
    }
    return !units.empty();
  }

  // Annex B: a NAL unit runs from after its start code to the byte before the next start code. A four
  // byte start code stores a zero in front of the three byte one, which belongs to the start code and
  // not to the preceding NAL unit.
  size_t offset = 0;
  size_t codeSize = 0;
  while (offset < size) {
    size_t start = SIZE_MAX;
    // The first NAL may be preceded by zero bytes.
    while (offset < size) {
      start = findStartCode(data, size, offset, codeSize);
      break;
    }
    if (start == SIZE_MAX) {
      break;
    }
    const size_t nalStart = start + codeSize;
    size_t nextCodeSize = 0;
    size_t nextStart = findStartCode(data, size, nalStart, nextCodeSize);
    size_t nalEnd = nextStart == SIZE_MAX ? size : nextStart;
    if (nextStart != SIZE_MAX && nalEnd > nalStart && data[nalEnd - 1] == 0) {
      // The zero in front of a four byte start code, or the trailing zero byte of the NAL unit.
      --nalEnd;
    }
    if (nalEnd > nalStart) {
      NalUnit unit;
      unit.data = data + nalStart;
      unit.size = nalEnd - nalStart;
      unit.type = data[nalStart] & 0x1f;
      unit.refIdc = (data[nalStart] >> 5) & 0x03;
      unit.offset = nalStart;
      units.push_back(unit);
    }
    if (nextStart == SIZE_MAX) {
      break;
    }
    offset = nextStart;
  }
  return !units.empty();
}

NalFormat detectNalFormat(const uint8_t* data, size_t size, int lengthSize) {
  if (lengthSize < 1 || lengthSize > 4) {
    lengthSize = 4;
  }
  if (validateLengthPrefixed(data, size, lengthSize)) {
    return NalFormat::kLengthPrefixed;
  }
  return NalFormat::kAnnexB;
}

std::vector<uint8_t> extractRbsp(const uint8_t* data, size_t size) {
  std::vector<uint8_t> rbsp;
  if (data == nullptr || size == 0) {
    return rbsp;
  }
  rbsp.reserve(size);
  int zeroCount = 0;
  for (size_t index = 0; index < size; ++index) {
    const uint8_t byte = data[index];
    if (zeroCount >= 2 && byte == 0x03) {
      // The emulation prevention byte is removed, and the zero count restarts because the next byte
      // is the escaped 0..3.
      zeroCount = 0;
      continue;
    }
    zeroCount = byte == 0 ? zeroCount + 1 : 0;
    rbsp.push_back(byte);
  }
  return rbsp;
}

// --------------------------------------------------------------------------------------------------

bool BitReader::readBit(uint32_t& value) {
  if (mFailed || mBitPosition >= mSize * 8) {
    mFailed = true;
    return false;
  }
  const size_t byteIndex = mBitPosition >> 3;
  const int bitIndex = 7 - static_cast<int>(mBitPosition & 7);
  value = (mData[byteIndex] >> bitIndex) & 1U;
  ++mBitPosition;
  return true;
}

bool BitReader::readBits(const int count, uint32_t& value) {
  if (count < 0 || count > 32) {
    mFailed = true;
    return false;
  }
  uint32_t result = 0;
  for (int index = 0; index < count; ++index) {
    uint32_t bit = 0;
    if (!readBit(bit)) {
      return false;
    }
    result = (result << 1) | bit;
  }
  value = result;
  return true;
}

bool BitReader::readUe(uint32_t& value) {
  int leadingZeros = 0;
  uint32_t bit = 0;
  while (true) {
    if (!readBit(bit)) {
      return false;
    }
    if (bit != 0) {
      break;
    }
    ++leadingZeros;
    if (leadingZeros > 31) {
      mFailed = true;
      return false;
    }
  }
  if (leadingZeros == 0) {
    value = 0;
    return true;
  }
  uint32_t suffix = 0;
  if (!readBits(leadingZeros, suffix)) {
    return false;
  }
  value = ((1U << leadingZeros) - 1U) + suffix;
  return true;
}

bool BitReader::readSe(int32_t& value) {
  uint32_t code = 0;
  if (!readUe(code)) {
    return false;
  }
  if ((code & 1U) != 0) {
    value = static_cast<int32_t>((code + 1U) / 2U);
  } else {
    value = -static_cast<int32_t>(code / 2U);
  }
  return true;
}

bool BitReader::skipBits(const size_t count) {
  if (mFailed || mBitPosition + count > mSize * 8) {
    mFailed = true;
    return false;
  }
  mBitPosition += count;
  return true;
}

bool BitReader::moreRbspData() const noexcept {
  if (mSize == 0 || mBitPosition > mSize * 8) {
    return false;
  }
  size_t lastSetBit = 0;
  bool found = false;
  for (size_t index = mSize; index > 0; --index) {
    const uint8_t byte = mData[index - 1];
    if (byte != 0) {
      int bit = 7;
      while (bit >= 0 && ((byte >> bit) & 1U) == 0) {
        --bit;
      }
      // `bit` counts from the least significant bit; the reader counts the bits from the most
      // significant one, which is the position of the last set bit of this byte in the stream.
      lastSetBit = (index - 1) * 8 + static_cast<size_t>(7 - bit);
      found = true;
      break;
    }
  }
  return found && mBitPosition < lastSetBit;
}

// --------------------------------------------------------------------------------------------------

bool parseSps(const std::vector<uint8_t>& rbsp, Sps& sps) {
  sps = Sps{};
  if (rbsp.size() < 4) {
    return false;
  }
  BitReader reader(rbsp.data(), rbsp.size());
  uint32_t profileIdc = 0;
  uint32_t ignored = 0;
  uint32_t levelIdc = 0;
  uint32_t spsId = 0;
  if (!reader.readBits(8, profileIdc) || !reader.readBits(8, ignored) || !reader.readBits(8, levelIdc) ||
          !reader.readUe(spsId)) {
    return false;
  }
  sps.profileIdc = static_cast<int>(profileIdc);
  sps.levelIdc = static_cast<int>(levelIdc);
  sps.id = static_cast<int>(spsId);

  const bool highProfile = profileIdc == 100 || profileIdc == 110 || profileIdc == 122 || profileIdc == 244 ||
                           profileIdc == 44 || profileIdc == 83 || profileIdc == 86 || profileIdc == 118 ||
                           profileIdc == 128 || profileIdc == 138 || profileIdc == 139 || profileIdc == 134 ||
                           profileIdc == 135;
  if (highProfile) {
    uint32_t chromaFormatIdc = 1;
    if (!reader.readUe(chromaFormatIdc) || chromaFormatIdc > 3) {
      return false;
    }
    sps.chromaFormatIdc = static_cast<int>(chromaFormatIdc);
    if (chromaFormatIdc == 3) {
      if (!reader.readBit(ignored)) {
        return false;
      }
      sps.separateColourPlane = ignored != 0;
    }
    uint32_t bitDepthLumaMinus8 = 0;
    uint32_t bitDepthChromaMinus8 = 0;
    if (!reader.readUe(bitDepthLumaMinus8) || !reader.readUe(bitDepthChromaMinus8) || bitDepthLumaMinus8 > 8 ||
            bitDepthChromaMinus8 > 8) {
      return false;
    }
    sps.bitDepthLuma = 8 + static_cast<int>(bitDepthLumaMinus8);
    sps.bitDepthChroma = 8 + static_cast<int>(bitDepthChromaMinus8);
    if (!reader.readBit(ignored)) {
      return false;
    }
    uint32_t seqScalingMatrixPresent = 0;
    if (!reader.readBit(seqScalingMatrixPresent)) {
      return false;
    }
    if (seqScalingMatrixPresent) {
      sps.seqScalingMatrixPresent = true;
      const int count = chromaFormatIdc != 3 ? 8 : 12;
      for (int index = 0; index < count; ++index) {
        uint32_t present = 0;
        if (!reader.readBit(present)) {
          return false;
        }
        if (present && !skipScalingList(reader, index < 6 ? 16 : 64)) {
          return false;
        }
      }
    }
  }

  uint32_t log2MaxFrameNumMinus4 = 0;
  if (!reader.readUe(log2MaxFrameNumMinus4) || log2MaxFrameNumMinus4 > 12) {
    return false;
  }
  sps.log2MaxFrameNum = 4 + static_cast<int>(log2MaxFrameNumMinus4);

  uint32_t pocType = 0;
  if (!reader.readUe(pocType) || pocType > 2) {
    return false;
  }
  sps.picOrderCntType = static_cast<int>(pocType);
  if (pocType == 0) {
    uint32_t log2MaxPocLsbMinus4 = 0;
    if (!reader.readUe(log2MaxPocLsbMinus4) || log2MaxPocLsbMinus4 > 12) {
      return false;
    }
    sps.log2MaxPicOrderCntLsb = 4 + static_cast<int>(log2MaxPocLsbMinus4);
  } else if (pocType == 1) {
    uint32_t deltaAlwaysZero = 0;
    if (!reader.readBit(deltaAlwaysZero)) {
      return false;
    }
    sps.deltaPicOrderAlwaysZero = deltaAlwaysZero != 0;
    if (!reader.readSe(sps.offsetForNonRefPic) || !reader.readSe(sps.offsetForTopToBottomField)) {
      return false;
    }
    uint32_t cycleCount = 0;
    if (!reader.readUe(cycleCount) || cycleCount > 255) {
      return false;
    }
    sps.offsetForRefFrame.reserve(cycleCount);
    for (uint32_t index = 0; index < cycleCount; ++index) {
      int32_t offset = 0;
      if (!reader.readSe(offset)) {
        return false;
      }
      sps.offsetForRefFrame.push_back(offset);
    }
  }

  uint32_t maxNumRefFrames = 0;
  uint32_t gaps = 0;
  uint32_t widthMbsMinus1 = 0;
  uint32_t heightMapUnitsMinus1 = 0;
  if (!reader.readUe(maxNumRefFrames) || maxNumRefFrames > 16 || !reader.readBit(gaps) ||
          !reader.readUe(widthMbsMinus1) || !reader.readUe(heightMapUnitsMinus1) || widthMbsMinus1 > 4096 ||
          heightMapUnitsMinus1 > 4096) {
    return false;
  }
  sps.maxNumRefFrames = static_cast<int>(maxNumRefFrames);
  sps.gapsInFrameNumAllowed = gaps != 0;
  sps.picWidthInMbs = static_cast<int>(widthMbsMinus1) + 1;
  sps.picHeightInMapUnits = static_cast<int>(heightMapUnitsMinus1) + 1;

  uint32_t frameMbsOnly = 0;
  if (!reader.readBit(frameMbsOnly)) {
    return false;
  }
  sps.frameMbsOnly = frameMbsOnly != 0;
  if (!sps.frameMbsOnly) {
    uint32_t mbaff = 0;
    if (!reader.readBit(mbaff)) {
      return false;
    }
    sps.mbAdaptiveFrameField = mbaff != 0;
  }
  uint32_t direct8x8 = 0;
  if (!reader.readBit(direct8x8)) {
    return false;
  }
  sps.direct8x8Inference = direct8x8 != 0;

  uint32_t cropping = 0;
  if (!reader.readBit(cropping)) {
    return false;
  }
  sps.frameCropping = cropping != 0;
  if (sps.frameCropping) {
    uint32_t left = 0;
    uint32_t right = 0;
    uint32_t top = 0;
    uint32_t bottom = 0;
    if (!reader.readUe(left) || !reader.readUe(right) || !reader.readUe(top) || !reader.readUe(bottom)) {
      return false;
    }
    sps.cropLeft = static_cast<int>(left);
    sps.cropRight = static_cast<int>(right);
    sps.cropTop = static_cast<int>(top);
    sps.cropBottom = static_cast<int>(bottom);
  }

  const int chromaArrayType = sps.separateColourPlane ? 0 : sps.chromaFormatIdc;
  const int cropUnitX = chromaArrayType == 0 ? 1 : (chromaArrayType == 3 ? 1 : 2);
  const int cropUnitY = (chromaArrayType == 0 ? 1 : (chromaArrayType == 3 ? 1 : 2)) * (2 - (sps.frameMbsOnly ? 1 : 0));
  sps.codedWidth = sps.picWidthInMbs * 16;
  sps.codedHeight = (2 - (sps.frameMbsOnly ? 1 : 0)) * sps.picHeightInMapUnits * 16;
  sps.width = sps.codedWidth - cropUnitX * (sps.cropLeft + sps.cropRight);
  sps.height = sps.codedHeight - cropUnitY * (sps.cropTop + sps.cropBottom);
  if (sps.width <= 0 || sps.height <= 0) {
    return false;
  }

  uint32_t vuiPresent = 0;
  if (!reader.readBit(vuiPresent)) {
    return false;
  }
  if (vuiPresent && !parseVui(reader, sps)) {
    return false;
  }

  sps.valid = true;
  return true;
}

bool parsePps(const std::vector<uint8_t>& rbsp, Pps& pps) {
  pps = Pps{};
  if (rbsp.empty()) {
    return false;
  }
  BitReader reader(rbsp.data(), rbsp.size());
  uint32_t ppsId = 0;
  uint32_t spsId = 0;
  uint32_t entropy = 0;
  uint32_t bottomField = 0;
  uint32_t sliceGroups = 0;
  if (!reader.readUe(ppsId) || ppsId > 255 || !reader.readUe(spsId) || spsId > 31 || !reader.readBit(entropy) ||
          !reader.readBit(bottomField) || !reader.readUe(sliceGroups) || sliceGroups > 7) {
    return false;
  }
  pps.id = static_cast<int>(ppsId);
  pps.spsId = static_cast<int>(spsId);
  pps.entropyCoding = entropy != 0;
  pps.bottomFieldPicOrderInFramePresent = bottomField != 0;
  pps.numSliceGroupsMinus1 = static_cast<int>(sliceGroups);

  uint32_t value = 0;
  if (!reader.readUe(value) || value > 31) {
    return false;
  }
  pps.numRefIdxL0DefaultActiveMinus1 = static_cast<int>(value);
  if (!reader.readUe(value) || value > 31) {
    return false;
  }
  pps.numRefIdxL1DefaultActiveMinus1 = static_cast<int>(value);
  uint32_t weightedPred = 0;
  uint32_t weightedBipred = 0;
  if (!reader.readBit(weightedPred) || !reader.readBits(2, weightedBipred)) {
    return false;
  }
  pps.weightedPred = weightedPred != 0;
  pps.weightedBipredIdc = static_cast<int>(weightedBipred);

  int32_t signedValue = 0;
  if (!reader.readSe(signedValue)) {
    return false;
  }
  pps.picInitQp = 26 + static_cast<int>(signedValue);
  if (!reader.readSe(signedValue)) {
    return false;
  }
  pps.picInitQs = 26 + static_cast<int>(signedValue);
  if (!reader.readSe(signedValue)) {
    return false;
  }
  pps.chromaQpIndexOffset = static_cast<int>(signedValue);
  uint32_t deblocking = 0;
  uint32_t constrained = 0;
  uint32_t redundant = 0;
  if (!reader.readBit(deblocking) || !reader.readBit(constrained) || !reader.readBit(redundant)) {
    return false;
  }
  pps.deblockingFilterControlPresent = deblocking != 0;
  pps.constrainedIntraPred = constrained != 0;
  pps.redundantPicCntPresent = redundant != 0;

  if (reader.moreRbspData()) {
    uint32_t transform8x8 = 0;
    if (!reader.readBit(transform8x8)) {
      return false;
    }
    pps.transform8x8Mode = transform8x8 != 0;
    uint32_t scalingPresent = 0;
    if (!reader.readBit(scalingPresent)) {
      return false;
    }
    if (scalingPresent) {
      pps.scalingMatrixPresent = true;
      const int count = 6 + (pps.transform8x8Mode ? 2 : 0);
      for (int index = 0; index < count; ++index) {
        uint32_t present = 0;
        if (!reader.readBit(present)) {
          return false;
        }
        if (present && !skipScalingList(reader, index < 6 ? 16 : 64)) {
          return false;
        }
      }
    }
    if (reader.moreRbspData()) {
      if (!reader.readSe(signedValue)) {
        return false;
      }
      pps.secondChromaQpIndexOffset = static_cast<int>(signedValue);
    }
  }

  pps.valid = true;
  return true;
}

bool parseSliceHeader(const std::vector<uint8_t>& rbsp, const Sps& sps, const Pps& pps, const int nalType,
        const int nalRefIdc, SliceHeader& header) {
  header = SliceHeader{};
  if (rbsp.empty()) {
    return false;
  }
  header.nalType = nalType;
  header.nalRefIdc = nalRefIdc;
  BitReader reader(rbsp.data(), rbsp.size());
  uint32_t value = 0;
  if (!reader.readUe(value)) {
    return false;
  }
  header.firstMbInSlice = value;
  if (!reader.readUe(value) || value > 9) {
    return false;
  }
  header.sliceType = static_cast<int>(value);
  if (!reader.readUe(value) || value > 255) {
    return false;
  }
  header.ppsId = static_cast<int>(value);

  if (sps.separateColourPlane) {
    if (!reader.readBits(2, value)) {
      return false;
    }
    header.colourPlaneId = static_cast<int>(value);
  }

  if (!reader.readBits(sps.log2MaxFrameNum, value)) {
    return false;
  }
  header.frameNum = value;

  if (!sps.frameMbsOnly) {
    uint32_t fieldPic = 0;
    if (!reader.readBit(fieldPic)) {
      return false;
    }
    header.fieldPic = fieldPic != 0;
    if (header.fieldPic) {
      uint32_t bottom = 0;
      if (!reader.readBit(bottom)) {
        return false;
      }
      header.bottomField = bottom != 0;
    }
  }

  if (nalType == 5) {
    if (!reader.readUe(value)) {
      return false;
    }
    header.idrPicId = value;
  }

  if (sps.picOrderCntType == 0) {
    if (!reader.readBits(sps.log2MaxPicOrderCntLsb, value)) {
      return false;
    }
    header.picOrderCntLsb = value;
    if (pps.bottomFieldPicOrderInFramePresent && !header.fieldPic) {
      if (!reader.readSe(header.deltaPicOrderCntBottom)) {
        return false;
      }
    }
  } else if (sps.picOrderCntType == 1 && !sps.deltaPicOrderAlwaysZero) {
    if (!reader.readSe(header.deltaPicOrderCnt[0])) {
      return false;
    }
    if (pps.bottomFieldPicOrderInFramePresent && !header.fieldPic) {
      if (!reader.readSe(header.deltaPicOrderCnt[1])) {
        return false;
      }
    }
  }

  if (pps.redundantPicCntPresent) {
    if (!reader.readUe(value)) {
      return false;
    }
    header.redundantPicCnt = value;
  }

  const int sliceType = header.sliceType % 5;
  const bool isP = sliceType == 0 || sliceType == 3;
  const bool isB = sliceType == 1;
  const bool isI = sliceType == 2 || sliceType == 4;

  if (isB) {
    uint32_t directSpatial = 0;
    if (!reader.readBit(directSpatial)) {
      return false;
    }
    header.directSpatialMvPred = directSpatial != 0;
  }
  if (isP || isB) {
    uint32_t override = 0;
    if (!reader.readBit(override)) {
      return false;
    }
    header.overrideNumRefIdx = override != 0;
    if (header.overrideNumRefIdx) {
      if (!reader.readUe(value) || value > 31) {
        return false;
      }
      header.numRefIdxL0ActiveMinus1 = value;
      if (isB) {
        if (!reader.readUe(value) || value > 31) {
          return false;
        }
        header.numRefIdxL1ActiveMinus1 = value;
      }
    } else {
      header.numRefIdxL0ActiveMinus1 = static_cast<uint32_t>(pps.numRefIdxL0DefaultActiveMinus1);
      header.numRefIdxL1ActiveMinus1 = static_cast<uint32_t>(pps.numRefIdxL1DefaultActiveMinus1);
    }
  }

  // The reference picture list modification commands are parsed to advance the reader; the driver
  // parses the same commands itself because the decoder selects the short slice control form.
  const auto skipRefPicListModification = [&reader]() -> bool {
    uint32_t present = 0;
    if (!reader.readBit(present)) {
      return false;
    }
    if (!present) {
      return true;
    }
    for (size_t guard = 0; guard < 66; ++guard) {
      uint32_t operation = 0;
      if (!reader.readUe(operation)) {
        return false;
      }
      if (operation == 3) {
        return true;
      }
      if (operation > 2) {
        return false;
      }
      uint32_t argument = 0;
      if (!reader.readUe(argument)) {
        return false;
      }
    }
    return false;
  };
  if (!isI) {
    // The list 0 modification is present for every inter slice, including a B slice whose
    // `direct_spatial_mv_pred_flag` is set: the flag only selects the direct motion vector
    // derivation, not whether the reference picture list is signalled.
    if (!skipRefPicListModification()) {
      return false;
    }
    if (isB && !skipRefPicListModification()) {
      return false;
    }
  }

  const bool weighted = (pps.weightedPred && isP) || (pps.weightedBipredIdc == 1 && isB);
  if (weighted) {
    if (!reader.readUe(value) || value > 7) {
      return false;
    }
    if (sps.chromaFormatIdc != 0) {
      if (!reader.readUe(value) || value > 7) {
        return false;
      }
    }
    const uint32_t l0Count = header.overrideNumRefIdx ? header.numRefIdxL0ActiveMinus1 + 1
                                                      : static_cast<uint32_t>(pps.numRefIdxL0DefaultActiveMinus1 + 1);
    for (uint32_t index = 0; index < l0Count && index <= 31; ++index) {
      uint32_t lumaWeightFlag = 0;
      if (!reader.readBit(lumaWeightFlag)) {
        return false;
      }
      if (lumaWeightFlag) {
        int32_t ignored = 0;
        if (!reader.readSe(ignored) || !reader.readSe(ignored)) {
          return false;
        }
      }
      if (sps.chromaFormatIdc != 0) {
        uint32_t chromaWeightFlag = 0;
        if (!reader.readBit(chromaWeightFlag)) {
          return false;
        }
        if (chromaWeightFlag) {
          for (int plane = 0; plane < 4; ++plane) {
            int32_t ignored = 0;
            if (!reader.readSe(ignored)) {
              return false;
            }
          }
        }
      }
    }
    if (isB) {
      const uint32_t l1Count = header.overrideNumRefIdx ? header.numRefIdxL1ActiveMinus1 + 1
                                                        : static_cast<uint32_t>(pps.numRefIdxL1DefaultActiveMinus1 + 1);
      for (uint32_t index = 0; index < l1Count && index <= 31; ++index) {
        uint32_t lumaWeightFlag = 0;
        if (!reader.readBit(lumaWeightFlag)) {
          return false;
        }
        if (lumaWeightFlag) {
          int32_t ignored = 0;
          if (!reader.readSe(ignored) || !reader.readSe(ignored)) {
            return false;
          }
        }
        if (sps.chromaFormatIdc != 0) {
          uint32_t chromaWeightFlag = 0;
          if (!reader.readBit(chromaWeightFlag)) {
            return false;
          }
          if (chromaWeightFlag) {
            for (int plane = 0; plane < 4; ++plane) {
              int32_t ignored = 0;
              if (!reader.readSe(ignored)) {
                return false;
              }
            }
          }
        }
      }
    }
  }

  if (header.nalRefIdc != 0) {
    if (nalType == 5) {
      uint32_t noOutput = 0;
      uint32_t longTerm = 0;
      if (!reader.readBit(noOutput) || !reader.readBit(longTerm)) {
        return false;
      }
      header.noOutputOfPriorPics = noOutput != 0;
      header.longTermReference = longTerm != 0;
    } else {
      uint32_t adaptive = 0;
      if (!reader.readBit(adaptive)) {
        return false;
      }
      header.adaptiveRefPicMarking = adaptive != 0;
      if (header.adaptiveRefPicMarking) {
        for (size_t index = 0; index < kMaxMmcoOperations; ++index) {
          uint32_t operation = 0;
          if (!reader.readUe(operation)) {
            return false;
          }
          if (operation == 0) {
            break;
          }
          Mmco command;
          command.operation = operation;
          switch (operation) {
            case 1:
            case 3:
              if (!reader.readUe(command.argument1)) {
                return false;
              }
              if (operation == 3 && !reader.readUe(command.argument2)) {
                return false;
              }
              break;
            case 2:
              if (!reader.readUe(command.argument1)) {
                return false;
              }
              break;
            case 4:
              if (!reader.readUe(command.argument1)) {
                return false;
              }
              break;
            case 5:
              break;
            case 6:
              if (!reader.readUe(command.argument1)) {
                return false;
              }
              break;
            default:
              return false;
          }
          header.mmco.push_back(command);
        }
      }
    }
  }

  if (pps.entropyCoding && !isI) {
    if (!reader.readUe(value) || value > 2) {
      return false;
    }
    header.cabacInitIdc = static_cast<int>(value);
  }
  if (!reader.readSe(header.sliceQpDelta)) {
    return false;
  }

  if (sliceType == 3) {
    uint32_t switchFlag = 0;
    if (!reader.readBit(switchFlag)) {
      return false;
    }
    int32_t ignored = 0;
    if (!reader.readSe(ignored)) {
      return false;
    }
  } else if (sliceType == 4) {
    int32_t ignored = 0;
    if (!reader.readSe(ignored)) {
      return false;
    }
  }

  if (pps.deblockingFilterControlPresent) {
    if (!reader.readUe(value) || value > 2) {
      return false;
    }
    header.disableDeblockingFilterIdc = static_cast<int>(value);
    if (header.disableDeblockingFilterIdc != 1) {
      int32_t offset = 0;
      if (!reader.readSe(offset)) {
        return false;
      }
      header.sliceAlphaC0OffsetDiv2 = static_cast<int>(offset);
      if (!reader.readSe(offset)) {
        return false;
      }
      header.sliceBetaOffsetDiv2 = static_cast<int>(offset);
    }
  }

  header.valid = true;
  return true;
}

// --------------------------------------------------------------------------------------------------

void parseParameterSets(const std::vector<uint8_t>& blob, std::vector<Sps>& sps, std::vector<Pps>& pps) {
  if (blob.empty()) {
    return;
  }

  const auto parseUnits = [&sps, &pps](const std::vector<NalUnit>& units) {
    for (const auto& unit: units) {
      if (unit.size < 1) {
        continue;
      }
      if (unit.type == 7 && sps.size() < kMaxParameterSets) {
        Sps parsed;
        if (parseSps(extractRbsp(unit.data + 1, unit.size - 1), parsed)) {
          sps.push_back(parsed);
        } else {
          MINILOG_DEBUG("the H.264 SPS could not be parsed");
        }
      } else if (unit.type == 8 && pps.size() < kMaxParameterSets) {
        Pps parsed;
        if (parsePps(extractRbsp(unit.data + 1, unit.size - 1), parsed)) {
          pps.push_back(parsed);
        } else {
          MINILOG_DEBUG("the H.264 PPS could not be parsed");
        }
      }
    }
  };

  // An `AVCDecoderConfigurationRecord` carries the parameter sets in its own length prefixed form.
  if (blob.size() >= 7 && blob[0] == 1) {
    const int lengthSize = (blob[4] & 0x03) + 1;
    size_t offset = 6;
    const size_t spsCount = blob[5] & 0x1f;
    std::vector<NalUnit> units;
    const auto readRecordUnits = [&](const size_t count) {
      for (size_t index = 0; index < count; ++index) {
        if (offset + static_cast<size_t>(lengthSize) > blob.size()) {
          return;
        }
        size_t length = 0;
        for (int i = 0; i < lengthSize; ++i) {
          length = (length << 8) | blob[offset + static_cast<size_t>(i)];
        }
        offset += static_cast<size_t>(lengthSize);
        if (length == 0 || offset + length > blob.size()) {
          return;
        }
        NalUnit unit;
        unit.data = blob.data() + offset;
        unit.size = length;
        unit.type = unit.data[0] & 0x1f;
        unit.refIdc = (unit.data[0] >> 5) & 0x03;
        unit.offset = offset;
        units.push_back(unit);
        offset += length;
      }
    };
    readRecordUnits(spsCount);
    if (offset < blob.size()) {
      const size_t ppsCount = blob[offset];
      ++offset;
      readRecordUnits(ppsCount);
    }
    parseUnits(units);
    if (!units.empty()) {
      return;
    }
  }

  // The framework's MP4 extractor stores one parameter set per codec specific data entry, prefixed by
  // its one byte length.
  if (blob.size() >= 2 && blob[0] + 1 == blob.size() && (blob[1] & 0x1f) == 7) {
    NalUnit unit;
    unit.data = blob.data() + 1;
    unit.size = blob.size() - 1;
    unit.type = 7;
    unit.refIdc = (unit.data[0] >> 5) & 0x03;
    parseUnits({ unit });
    return;
  }
  if (blob.size() >= 2 && blob[0] + 1 == blob.size() && (blob[1] & 0x1f) == 8) {
    NalUnit unit;
    unit.data = blob.data() + 1;
    unit.size = blob.size() - 1;
    unit.type = 8;
    unit.refIdc = (unit.data[0] >> 5) & 0x03;
    parseUnits({ unit });
    return;
  }

  // A bare parameter set without any framing.
  if ((blob[0] & 0x1f) == 7 || (blob[0] & 0x1f) == 8) {
    NalUnit unit;
    unit.data = blob.data();
    unit.size = blob.size();
    unit.type = blob[0] & 0x1f;
    unit.refIdc = (blob[0] >> 5) & 0x03;
    parseUnits({ unit });
    if (!sps.empty() || !pps.empty()) {
      return;
    }
  }

  std::vector<NalUnit> units;
  if (splitNalUnits(blob.data(), blob.size(), 4, NalFormat::kAnnexB, units)) {
    parseUnits(units);
  }
}

} // namespace coral::d3d11va

#endif // defined(WINDOWS) || defined(WIN32)
