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

#ifndef CORAL_MUXER_MP4_METADATA_H
#define CORAL_MUXER_MP4_METADATA_H

#include <media/Metadata.h>
#include <media/MetadataEntries.h>

#include "BoxWriter.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace coral {
namespace muxer {
namespace mp4 {

/** Collects the file-level metadata the MP4 writer understands. */
class Mp4Metadata final {
public:
  void add(const std::shared_ptr<Metadata>& metadata);

  /** Adds or replaces an mdta entry, used for the MP4-AT auxiliary metadata. */
  void setEntry(const std::shared_ptr<MdtaMetadataEntry>& entry);

  [[nodiscard]] bool hasLocation() const { return mHasLocation; }
  [[nodiscard]] bool hasOrientation() const { return mHasOrientation; }
  [[nodiscard]] bool hasTimestamp() const { return mHasTimestamp; }
  [[nodiscard]] bool hasXmp() const { return !mXmp.empty(); }
  [[nodiscard]] bool hasChapters() const { return !mChapters.empty(); }

  [[nodiscard]] int orientation(int fallback) const;

  [[nodiscard]] int64_t creationTime() const { return mCreationTime; }
  [[nodiscard]] int64_t modificationTime() const { return mModificationTime; }

  /** Writes `udta` with the ISO 6709 location and the `mdta` entries, if present. */
  void writeUdta(BoxWriter& writer) const;

  /** Writes the top-level XMP `uuid` box, if present. */
  void writeXmp(BoxWriter& writer) const;

private:
  bool mHasLocation = false;
  double mLatitude = 0;
  double mLongitude = 0;

  bool mHasOrientation = false;
  int mOrientation = 0;

  bool mHasTimestamp = false;
  int64_t mCreationTime = 0;
  int64_t mModificationTime = 0;

  std::vector<uint8_t> mXmp;

  std::vector<Mp4Chapter> mChapters;

  std::vector<std::string> mKeys;
  std::map<std::string, std::shared_ptr<MdtaMetadataEntry>> mEntries;
};

} // namespace mp4
} // namespace muxer
} // namespace coral

#endif // CORAL_MUXER_MP4_METADATA_H
