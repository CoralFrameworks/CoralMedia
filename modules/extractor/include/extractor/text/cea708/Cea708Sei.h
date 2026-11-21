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

#ifndef CORAL_EXTRACTOR_TEXT_CEA708_CEA708_SEI_H
#define CORAL_EXTRACTOR_TEXT_CEA708_CEA708_SEI_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace coral {
namespace text {

/**
 * Extracts the CEA-708 cc_data that an H.264 or H.265 SEI NAL unit carries.
 *
 * <p>The NAL unit is the one of the access unit, including its header and without a start code or
 * length prefix. Only the `user_data_registered_itu_t_t35` payload of ATSC/DirectTV is considered;
 * the returned bytes are the raw `cc_data` triplets that {@link AVCea708Parser} consumes. An empty
 * result means the unit has no caption data.
 *
 * @param nal The bytes of the NAL unit.
 * @param size The number of bytes in @p nal.
 * @param hevc Whether the NAL unit is H.265 (two header bytes) rather than H.264 (one).
 */
std::vector<uint8_t> extractCea708CcDataFromSei(const uint8_t* nal, size_t size, bool hevc);

/**
 * Extracts the CEA-708 cc_data from every SEI NAL unit of an Annex B stream.
 *
 * <p>This is the MP4 form of a video sample after it was converted from length-prefixed NAL units,
 * and the form a transport stream delivers. An empty result means no access unit unit carried
 * caption data.
 */
std::vector<uint8_t> extractCea708CcDataFromAnnexB(const uint8_t* data, size_t size, bool hevc);

/** The caption byte streams of one cc_data sequence. */
struct CeaCcData final {
  /** CEA-608 byte pairs, ready for {@link AVCea608Parser}. */
  std::vector<uint8_t> cea608;
  /** CEA-708 cc_data triplets, ready for {@link AVCea708Parser}. */
  std::vector<uint8_t> cea708;
};

/**
 * Splits a cc_data sequence into its CEA-608 byte pairs and its CEA-708 triplets.
 *
 * <p>Each 3-byte entry starts with the `cc_valid`/`cc_type` byte: types 0 and 1 carry a 608 pair in
 * the two following bytes, types 2 and 3 carry a 708 packet and keep the header.
 */
CeaCcData splitCeaCcData(const uint8_t* data, size_t size);

} // namespace text
} // namespace coral

#endif //CORAL_EXTRACTOR_TEXT_CEA708_CEA708_SEI_H
