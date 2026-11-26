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

#ifndef CORAL_CODEC_H264_DXVA_PARSER_H
#define CORAL_CODEC_H264_DXVA_PARSER_H

#if defined(WINDOWS) || defined(WIN32)

#include <media/ColorInfo.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace coral::d3d11va {

/**
 * How the NAL units of an H.264 sample are framed.
 *
 * The framework's extractors hand the samples over in the Annex B byte-stream form, but a caller can
 * feed the decoder the length prefixed (AVCC) form its container stored as well, so both are accepted.
 */
enum class NalFormat : uint8_t {
  kAnnexB,
  kLengthPrefixed,
};

/** One NAL unit of the sample. `data` points at the NAL header, without the start code. */
struct NalUnit {
  const uint8_t* data = nullptr;
  size_t size = 0;
  /** The `nal_unit_type` field, 1..23 for the types this decoder understands. */
  int type = 0;
  /** The `nal_ref_idc` field; 0 means the NAL does not contribute to the reference pictures. */
  int refIdc = 0;
  /** The offset of `data` in the buffer it was found in. */
  size_t offset = 0;
};

/**
 * Returns the length of the NAL unit length field of an AVCC configuration record.
 *
 * @param csd the `AVCDecoderConfigurationRecord` (the first codec specific data entry), or null.
 * @return 1, 2, 3 or 4, and 4 when the buffer is not an AVCC record.
 */
int nalLengthSizeOf(const std::vector<uint8_t>& csd);

/**
 * Splits a sample into its NAL units.
 *
 * @param data     the sample.
 * @param size     the size of the sample.
 * @param lengthSize the length field size of the length prefixed form.
 * @param format   how the sample is framed.
 * @param units    receives the NAL units, in the order of the sample.
 *
 * @return false when the sample is malformed, in which case the caller drops it.
 */
bool splitNalUnits(const uint8_t* data, size_t size, int lengthSize, NalFormat format, std::vector<NalUnit>& units);

/**
 * Detects how a sample is framed.
 *
 * A length prefixed sample that happens to start with the bytes of an Annex B start code is a real
 * case: the length of its first NAL unit can be 1, which makes the prefix `00 00 00 01`. The whole
 * buffer is therefore examined against both framings, and the length prefixed one wins when it is
 * consistent, which is the same rule the Apple implementation uses.
 */
NalFormat detectNalFormat(const uint8_t* data, size_t size, int lengthSize);

/**
 * Returns the RBSP of a NAL unit, with the emulation prevention bytes removed.
 */
std::vector<uint8_t> extractRbsp(const uint8_t* data, size_t size);

/**
 * A most-significant-bit first bit reader over an RBSP.
 *
 * Every read reports failure instead of asserting, because the decoder must hand a malformed stream
 * to the platform fallback rather than crash on it.
 */
class BitReader {
  public:
  BitReader(const uint8_t* data, size_t size) noexcept
          : mData(data),
            mSize(size) {}

  bool readBit(uint32_t& value);
  bool readBits(int count, uint32_t& value);
  bool readUe(uint32_t& value);
  bool readSe(int32_t& value);
  bool skipBits(size_t count);

  [[nodiscard]] size_t bitPosition() const noexcept { return mBitPosition; }
  [[nodiscard]] size_t bitsRemaining() const noexcept {
    return mSize * 8 > mBitPosition ? mSize * 8 - mBitPosition : 0;
  }
  [[nodiscard]] bool failed() const noexcept { return mFailed; }

  /**
   * Whether the reader is before the last significant bit, i.e. there are fields after the current
   * position rather than only the `rbsp_trailing_bits` of the syntax structure.
   */
  [[nodiscard]] bool moreRbspData() const noexcept;

  private:
  const uint8_t* mData;
  size_t mSize;
  size_t mBitPosition = 0;
  bool mFailed = false;
};

/** The subset of the sequence parameter set the D3D11VA front-end needs. */
struct Sps {
  bool valid = false;

  int id = -1;
  int profileIdc = 0;
  int levelIdc = 0;
  int chromaFormatIdc = 1;
  bool separateColourPlane = false;
  int bitDepthLuma = 8;
  int bitDepthChroma = 8;
  bool seqScalingMatrixPresent = false;

  int log2MaxFrameNum = 0;
  int picOrderCntType = 0;
  int log2MaxPicOrderCntLsb = 0;
  bool deltaPicOrderAlwaysZero = false;
  int32_t offsetForNonRefPic = 0;
  int32_t offsetForTopToBottomField = 0;
  std::vector<int32_t> offsetForRefFrame;

  int maxNumRefFrames = 0;
  bool gapsInFrameNumAllowed = false;
  int picWidthInMbs = 0;
  int picHeightInMapUnits = 0;
  bool frameMbsOnly = true;
  bool mbAdaptiveFrameField = false;
  bool direct8x8Inference = true;

  bool frameCropping = false;
  int cropLeft = 0;
  int cropRight = 0;
  int cropTop = 0;
  int cropBottom = 0;

  /** The cropped display size, which is what the decoder reports for a frame. */
  int width = 0;
  int height = 0;
  /** The macroblock aligned coded size, which is what the decode surface is allocated for. */
  int codedWidth = 0;
  int codedHeight = 0;

  /** `max_num_reorder_frames` of the VUI, or -1 when the stream does not declare it. */
  int maxNumReorderFrames = -1;

  ColorInfo color;
};

/** The subset of the picture parameter set the D3D11VA front-end needs. */
struct Pps {
  bool valid = false;

  int id = -1;
  int spsId = -1;
  bool entropyCoding = false;
  bool bottomFieldPicOrderInFramePresent = false;
  int numSliceGroupsMinus1 = 0;
  int numRefIdxL0DefaultActiveMinus1 = 0;
  int numRefIdxL1DefaultActiveMinus1 = 0;
  bool weightedPred = false;
  int weightedBipredIdc = 0;
  int picInitQp = 26;
  int picInitQs = 26;
  int chromaQpIndexOffset = 0;
  int secondChromaQpIndexOffset = 0;
  bool deblockingFilterControlPresent = false;
  bool constrainedIntraPred = false;
  bool redundantPicCntPresent = false;
  bool transform8x8Mode = false;
  /** Whether the PPS carries a scaling matrix, which this front-end does not provide to the driver. */
  bool scalingMatrixPresent = false;
};

/** One `memory_management_control_operation` of the adaptive reference picture marking. */
struct Mmco {
  uint32_t operation = 0;
  uint32_t argument1 = 0;
  uint32_t argument2 = 0;
};

/** The slice header fields the front-end parses; the slice data itself is decoded by the driver. */
struct SliceHeader {
  bool valid = false;

  int nalType = 0;
  int nalRefIdc = 0;
  uint32_t firstMbInSlice = 0;
  /** The `slice_type` value 0..9; `sliceType % 5` is the picture type. */
  int sliceType = 0;
  int ppsId = 0;
  int colourPlaneId = 0;
  uint32_t frameNum = 0;
  bool fieldPic = false;
  bool bottomField = false;
  uint32_t idrPicId = 0;

  uint32_t picOrderCntLsb = 0;
  int32_t deltaPicOrderCntBottom = 0;
  int32_t deltaPicOrderCnt[2] = { 0, 0 };

  uint32_t redundantPicCnt = 0;
  bool directSpatialMvPred = false;
  bool overrideNumRefIdx = false;
  uint32_t numRefIdxL0ActiveMinus1 = 0;
  uint32_t numRefIdxL1ActiveMinus1 = 0;

  bool adaptiveRefPicMarking = false;
  std::vector<Mmco> mmco;
  bool noOutputOfPriorPics = false;
  bool longTermReference = false;

  int cabacInitIdc = 0;
  int32_t sliceQpDelta = 0;
  int disableDeblockingFilterIdc = 0;
  int sliceAlphaC0OffsetDiv2 = 0;
  int sliceBetaOffsetDiv2 = 0;
};

/**
 * Parses a sequence parameter set from its RBSP.
 */
bool parseSps(const std::vector<uint8_t>& rbsp, Sps& sps);

/**
 * Parses a picture parameter set from its RBSP.
 */
bool parsePps(const std::vector<uint8_t>& rbsp, Pps& pps);

/**
 * Parses a slice header from the RBSP of a slice NAL unit.
 *
 * The parse covers every field which precedes the slice data, including the fields whose value the
 * decoder does not use itself: the driver parses the same header from the bitstream, so the host has
 * to skip them exactly to find the reference picture marking that drives the host side DPB.
 */
bool parseSliceHeader(const std::vector<uint8_t>& rbsp, const Sps& sps, const Pps& pps, int nalType, int nalRefIdc,
        SliceHeader& header);

/**
 * Parses the SPS and the PPS entries of an `AVCDecoderConfigurationRecord` or of an Annex B blob.
 *
 * @param blob  the codec specific data of the track.
 * @param sps   receives every parsed sequence parameter set.
 * @param pps   receives every parsed picture parameter set.
 */
void parseParameterSets(const std::vector<uint8_t>& blob, std::vector<Sps>& sps, std::vector<Pps>& pps);

} // namespace coral::d3d11va

#endif // defined(WINDOWS) || defined(WIN32)

#endif // CORAL_CODEC_H264_DXVA_PARSER_H
