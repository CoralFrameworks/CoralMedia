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

#include "Mp4Metadata.h"

#include "Mp4Boxes.h"

#include <algorithm>
#include <cstdio>

namespace coral {
namespace muxer {
namespace mp4 {
namespace {

constexpr uint8_t kXmpUuid[16] = { 0xBE, 0x7A, 0xCF, 0xCB, 0x97, 0xA9, 0x42, 0xE8, 0x9C, 0x71, 0x99, 0x94, 0x91, 0xE3,
  0xAF, 0xAC };

} // namespace

void Mp4Metadata::add(const std::shared_ptr<Metadata>& metadata) {
  if (metadata == nullptr) {
    return;
  }
  for (const auto& entry: metadata->getEntries()) {
    if (entry == nullptr) {
      continue;
    }
    if (const auto* location = dynamic_cast<Mp4LocationData*>(entry.get()); location != nullptr && !mHasLocation) {
      mHasLocation = true;
      mLatitude = location->getLatitude();
      mLongitude = location->getLongitude();
    } else if (const auto* orientation = dynamic_cast<Mp4OrientationData*>(entry.get());
            orientation != nullptr && !mHasOrientation) {
      mHasOrientation = true;
      mOrientation = orientation->getRotation();
    } else if (const auto* timestamp = dynamic_cast<Mp4TimestampData*>(entry.get());
            timestamp != nullptr && !mHasTimestamp) {
      mHasTimestamp = true;
      mCreationTime = timestamp->getCreationTime();
      mModificationTime = timestamp->getModificationTime();
    } else if (const auto* xmp = dynamic_cast<XmpData*>(entry.get()); xmp != nullptr && mXmp.empty()) {
      mXmp = xmp->getData();
    } else if (const auto* chapters = dynamic_cast<Mp4ChaptersData*>(entry.get());
            chapters != nullptr && mChapters.empty()) {
      mChapters = chapters->getChapters();
    } else if (auto* mdta = dynamic_cast<MdtaMetadataEntry*>(entry.get()); mdta != nullptr) {
      if (mEntries.find(mdta->getKey()) == mEntries.end()) {
        mKeys.push_back(mdta->getKey());
        mEntries[mdta->getKey()] = std::shared_ptr<MdtaMetadataEntry>(entry, mdta);
      }
    }
  }
}

int Mp4Metadata::orientation(const int fallback) const { return mHasOrientation ? mOrientation : fallback; }

void Mp4Metadata::setEntry(const std::shared_ptr<MdtaMetadataEntry>& entry) {
  if (entry == nullptr) {
    return;
  }
  if (mEntries.find(entry->getKey()) == mEntries.end()) {
    mKeys.push_back(entry->getKey());
  }
  mEntries[entry->getKey()] = entry;
}

void Mp4Metadata::writeUdta(BoxWriter& writer) const {
  if (!mHasLocation && mKeys.empty() && mChapters.empty()) {
    return;
  }

  const size_t udta = writer.beginBox("udta");
  if (mHasLocation) {
    char location[64] = {};
    std::snprintf(location, sizeof(location), "%+.4f%+.4f/", mLatitude, mLongitude);
    const auto length = static_cast<uint16_t>(std::char_traits<char>::length(location));

    const char xyz[4] = { '\xA9', 'x', 'y', 'z' };
    const size_t box = writer.beginBox(xyz);
    writer.u16(length);
    writer.u16(0x15C7); // ISO-639-2/T language code "und"
    writer.string(location);
    writer.endBox(box);
  }

  if (!mKeys.empty()) {
    const size_t meta = writer.beginFullBox("meta", 0, 0);
    writeHdlr(writer, "mdir", "");

    const size_t keys = writer.beginFullBox("keys", 0, 0);
    writer.u32(static_cast<uint32_t>(mKeys.size()));
    for (const auto& key: mKeys) {
      writer.u32(static_cast<uint32_t>(8 + key.size()));
      writer.fourcc("mdta");
      writer.string(key);
    }
    writer.endBox(keys);

    const size_t ilst = writer.beginBox("ilst");
    for (size_t i = 0; i < mKeys.size(); ++i) {
      char index[4] = { static_cast<char>((i + 1) >> 24), static_cast<char>((i + 1) >> 16),
        static_cast<char>((i + 1) >> 8), static_cast<char>(i + 1) };
      const auto& entry = mEntries.at(mKeys[i]);
      const size_t item = writer.beginBox(index);
      uint32_t type = 0;
      switch (entry->getValueType()) {
        case MdtaMetadataEntry::ValueType::kUtf8:
          type = 1;
          break;
        case MdtaMetadataEntry::ValueType::kInteger:
          type = 21;
          break;
        case MdtaMetadataEntry::ValueType::kFloat:
          type = 23;
          break;
        case MdtaMetadataEntry::ValueType::kUnsignedInt8:
          type = 75;
          break;
        case MdtaMetadataEntry::ValueType::kUnsignedInt64:
          type = 78;
          break;
        case MdtaMetadataEntry::ValueType::kBinary:
        default:
          type = 0;
          break;
      }
      const size_t data = writer.beginFullBox("data", 0, type);
      writer.u32(0);
      writer.bytes(entry->getValue());
      writer.endBox(data);
      writer.endBox(item);
    }
    writer.endBox(ilst);
    writer.endBox(meta);
  }

  if (!mChapters.empty()) {
    const size_t chpl = writer.beginFullBox("chpl", 1, 0);
    writer.u32(0); // reserved
    const size_t count = std::min<size_t>(mChapters.size(), 255);
    writer.u8(static_cast<uint8_t>(count));
    for (size_t i = 0; i < count; ++i) {
      const Mp4Chapter& chapter = mChapters[i];
      writer.u64(static_cast<uint64_t>(std::max<int64_t>(0, chapter.getTimeUs()) * 10));
      const std::string& title = chapter.getTitle();
      const size_t titleLength = std::min<size_t>(title.size(), 255);
      writer.u8(static_cast<uint8_t>(titleLength));
      writer.bytes(reinterpret_cast<const uint8_t*>(title.data()), titleLength);
    }
    writer.endBox(chpl);
  }
  writer.endBox(udta);
}

void Mp4Metadata::writeXmp(BoxWriter& writer) const {
  if (mXmp.empty()) {
    return;
  }
  const size_t box = writer.beginBox("uuid");
  writer.bytes(kXmpUuid, sizeof(kXmpUuid));
  writer.bytes(mXmp);
  writer.endBox(box);
}

} // namespace mp4
} // namespace muxer
} // namespace coral
