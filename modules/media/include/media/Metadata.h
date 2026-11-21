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

#ifndef CORAL_MEDIA_METADATA_H
#define CORAL_MEDIA_METADATA_H

#include <media/Definitions.h>

#include <memory>
#include <string>
#include <vector>

namespace coral {

class MediaFormat;

/** A collection of metadata entries. */
class Metadata final {
public:
  ~Metadata() = default;

  /** A metadata entry. */
  class Entry {
  public:
    virtual ~Entry() = default;

    /**
     * Returns the {@link MediaFormat} that can be used to decode the wrapped metadata in
     * {@link getWrappedMetadataBytes()}, or null if this Entry doesn't contain wrapped metadata.
     */
    virtual const std::shared_ptr<MediaFormat>& getWrappedMetadataFormat() {
      static std::shared_ptr<MediaFormat> sInvalidFormat = {};
      return sInvalidFormat;
    }

    /**
     * Returns the bytes of the wrapped metadata in this Entry, or null if it
     * doesn't contain wrapped metadata.
     */
    virtual std::vector<uint8_t> getWrappedMetadataBytes() {
      return {};
    }

    virtual std::string toString() = 0;
  };

  explicit Metadata(const std::vector<std::shared_ptr<Entry>>& entries) noexcept
      : Metadata(Definitions::kTimeUnset, entries) {}

  explicit Metadata(const int64_t presentationTimeUs, const std::vector<std::shared_ptr<Entry>>& entries) noexcept
      : entries(entries), presentationTimeUs(presentationTimeUs) {}

  /**
   * Returns the number of metadata entries.
   */
  [[nodiscard]] size_t length() const {
    return entries.size();
  }

  /**
   * Returns the entry at the specified index.
   *
   * @param index The index of the entry.
   * @return The entry at the specified index.
   */
  [[nodiscard]] const std::shared_ptr<Entry>& get(const int index) const {
    return entries[index];
  }

  [[nodiscard]] const std::vector<std::shared_ptr<Entry>>& getEntries() const {
    return entries;
  }

  [[nodiscard]] int64_t getPresentationTimeUs() const {
    return presentationTimeUs;
  }

  std::shared_ptr<Metadata> copyWithAppendedEntriesFrom(const std::shared_ptr<Metadata>& other) {
    if (other == nullptr) {
      return std::make_shared<Metadata>(presentationTimeUs, entries);
    }
    auto newEntries = std::vector<std::shared_ptr<Entry>>();
    for (auto const& entry: entries) {
      newEntries.push_back(entry);
    }
    for (auto const& entry: other->entries) {
      newEntries.push_back(entry);
    }
    return std::make_shared<Metadata>(presentationTimeUs, newEntries);
  }

  void appendedEntries(const std::vector<std::shared_ptr<Entry>>& otherEntries) {
    for (auto const& entry: otherEntries) {
      entries.push_back(entry);
    }
  }

private:
  std::vector<std::shared_ptr<Entry>> entries;

  /**
   * The presentation time of the metadata, in microseconds.
   *
   * This time is {@link Definitions::kTimeUnset} when not known or undefined.
   */
  int64_t presentationTimeUs;
};

} // namespace coral

#endif //CORAL_MEDIA_METADATA_H
