/*
 * Copyright (C) 2022 The Coral Project
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

#ifndef CORAL_MEDIA_COLOR_INFO_H
#define CORAL_MEDIA_COLOR_INFO_H

#include <cstdint>
#include <vector>

namespace coral {
/**
 * Describes pixel and encoding. ImageInfo can be created from ColorInfo by
 * providing dimensions.
 * It encodes how pixel bits describe alpha, transparency; color components red, blue,
 * and green; and ColorSpace, the range and linearity of colors.
 */
struct ColorInfo {
  /**
   * The type of color space for image or video.
   */
  enum class SpaceType : int8_t {
    kSpaceUnknown = 0,
    kSpaceBT709 = 1,
    kSpaceBT601 = 2,
    kSpaceBT2020 = 4,
  };

  /**
   * The type of color range for image or video.
   */
  enum class RangeType : int8_t {
    kRangeUnknown = 0,
    kRangeFull = 1,
    kRangeLimited = 2,
  };

  /**
   * The color transfer characteristics for image or video.
   */
  enum class TransferType : int8_t {
    kTransferUnknown = 0,
    kTransferLinear = 1,
    kTransferSDR = 2,
    kTransferST2084 = 3,
    kTransferHLG = 4,
    kTransferSRGB = 5,
    kTransferGamma22 = 10,
  };

  /**
   * The color space
   */
  SpaceType colorSpace;

  /**
   * The color range
   */
  RangeType colorRange;

  /**
   * The color transfer characteristics
   */
  TransferType colorTransfer;

  /**
   * HdrStaticInfo as defined in CTA-861.3, or empty if none specified.
   */
  std::vector<uint8_t> hdrStaticInfo;

  ColorInfo()
    : colorSpace(SpaceType::kSpaceUnknown),
      colorRange(RangeType::kRangeUnknown),
      colorTransfer(TransferType::kTransferUnknown) {
  }

  explicit ColorInfo(const SpaceType space, const RangeType range, const TransferType transfer) noexcept
    : colorSpace(space),
      colorRange(range),
      colorTransfer(transfer) {
  }

  explicit ColorInfo(const SpaceType space, const RangeType range, const TransferType transfer,
    const std::vector<uint8_t>& hdrStaticInfo) noexcept
    : colorSpace(space),
      colorRange(range),
      colorTransfer(transfer),
      hdrStaticInfo(hdrStaticInfo) {
  }

  ColorInfo(const ColorInfo& other) = default;
  ColorInfo& operator=(const ColorInfo& other) = default;

  static SpaceType isoColorPrimariesToColorSpace(const int isoColorPrimaries) {
    switch (isoColorPrimaries) {
      case 1:
        return SpaceType::kSpaceBT709;
      case 4: // BT.470M.
      case 5: // BT.470BG.
      case 6: // SMPTE 170M.
      case 7: // SMPTE 240M.
        return SpaceType::kSpaceBT601;
      case 9: // BT.2020
        return SpaceType::kSpaceBT2020;
      default:
        // Remaining color primaries are either reserved or unspecified.
        return SpaceType::kSpaceUnknown;
    }
  }

  static TransferType isoTransferCharacteristicsToColorTransfer(const int isoTransferCharacteristics) {
    switch (isoTransferCharacteristics) {
      case 1: // BT.709.
      case 6: // SMPTE 170M.
      case 7: // SMPTE 240M.
        return TransferType::kTransferSDR;
      case 4: {
        return TransferType::kTransferGamma22;
      }
      case 13: {
        return TransferType::kTransferSRGB;
      }
      case 16: {
        return TransferType::kTransferST2084;
      }
      case 18: {
        return TransferType::kTransferHLG;
      }
      default: {
        return TransferType::kTransferUnknown;
      }
    }
  }
};

typedef struct ColorInfo ColorInfo;

} // namespace coral

#endif // CORAL_MEDIA_COLOR_INFO_H
