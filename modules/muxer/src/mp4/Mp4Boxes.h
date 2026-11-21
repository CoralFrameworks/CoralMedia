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

#ifndef CORAL_MUXER_MP4_BOXES_H
#define CORAL_MUXER_MP4_BOXES_H

#include "BoxWriter.h"

#include <cstdint>
#include <string>

namespace coral {
namespace muxer {
namespace mp4 {

constexpr int64_t kMicrosPerSecond = 1'000'000;

/** Converts microseconds to a track/movie timescale, rounding half up. */
int64_t scaleUsToTicks(int64_t timeUs, int64_t timescale);

/** Converts timescale ticks to microseconds, rounding half up. */
int64_t scaleTicksToUs(int64_t ticks, int64_t timescale);

uint32_t clampToU32(int64_t value);

uint64_t clampToU64(int64_t value);

/** Packs an ISO-639-2/T code into the 15-bit mdhd language field. */
uint16_t packLanguage(const std::string& language);

/** Writes the 3x3 transformation matrix for a clockwise rotation in degrees. */
void writeMatrix(BoxWriter& writer, int rotation);

void writeFtyp(BoxWriter& writer);

void writeMvhd(BoxWriter& writer, int64_t durationUs, int64_t movieTimescale, int nextTrackId, int64_t creationTime,
        int64_t modificationTime);

void writeTkhd(BoxWriter& writer, int trackId, int64_t presentationDurationUs, int64_t movieTimescale,
        int64_t creationTime, int64_t modificationTime, int rotation, int alternateGroup, bool enabled, bool audio,
        int width, int height);

void writeMdhd(BoxWriter& writer, int64_t durationTicks, int64_t timescale, int64_t creationTime,
        int64_t modificationTime, const std::string& language);

void writeHdlr(BoxWriter& writer, const char* handlerType, const char* name);

void writeDinf(BoxWriter& writer);

} // namespace mp4
} // namespace muxer
} // namespace coral

#endif // CORAL_MUXER_MP4_BOXES_H
