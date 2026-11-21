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

#ifndef CORAL_MEDIA_METADATA_ENTRIES_H
#define CORAL_MEDIA_METADATA_ENTRIES_H

#include <media/Metadata.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace coral {

/** One chapter of a container, e.g. a Matroska {@code ChapterAtom}. */
class ChapterMetadataEntry final : public Metadata::Entry {
public:
  ChapterMetadataEntry(const int64_t timeUs, std::string title)
      : mTimeUs(timeUs),
        mTitle(std::move(title)) {
  }

  [[nodiscard]] int64_t getTimeUs() const { return mTimeUs; }
  [[nodiscard]] const std::string& getTitle() const { return mTitle; }

  std::string toString() override {
    return "ChapterMetadataEntry timeUs=" + std::to_string(mTimeUs) + " title=" + mTitle;
  }

private:
  int64_t mTimeUs;
  std::string mTitle;
};

/** A picture that a container carries as metadata, e.g. the cover of an album. */
class PictureMetadataEntry final : public Metadata::Entry {
public:
  PictureMetadataEntry(std::string mimeType, std::string description, int width, int height,
                       int depth, std::vector<uint8_t> data)
      : mMimeType(std::move(mimeType)),
        mDescription(std::move(description)),
        mWidth(width),
        mHeight(height),
        mDepth(depth),
        mData(std::move(data)) {
  }

  [[nodiscard]] const std::string& getMimeType() const { return mMimeType; }
  [[nodiscard]] const std::string& getDescription() const { return mDescription; }
  [[nodiscard]] int getWidth() const { return mWidth; }
  [[nodiscard]] int getHeight() const { return mHeight; }
  [[nodiscard]] int getDepth() const { return mDepth; }
  [[nodiscard]] const std::vector<uint8_t>& getData() const { return mData; }

  std::string toString() override {
    return "PictureMetadataEntry mime=" + mMimeType + " size=" + std::to_string(mData.size());
  }

private:
  std::string mMimeType;
  std::string mDescription;
  int mWidth;
  int mHeight;
  int mDepth;
  std::vector<uint8_t> mData;
};

/** A text metadata entry stored under a key, e.g. a Vorbis comment or a Matroska tag. */
class TextMetadataEntry final : public Metadata::Entry {
public:
  TextMetadataEntry(std::string key, std::string value, std::string language = {})
      : mKey(std::move(key)),
        mValue(std::move(value)),
        mLanguage(std::move(language)) {
  }

  [[nodiscard]] const std::string& getKey() const { return mKey; }
  [[nodiscard]] const std::string& getValue() const { return mValue; }
  [[nodiscard]] const std::string& getLanguage() const { return mLanguage; }

  std::string toString() override {
    return "TextMetadataEntry key=" + mKey + " value=" + mValue;
  }

private:
  std::string mKey;
  std::string mValue;
  std::string mLanguage;
};

/** A binary metadata entry stored under a key. */
class BinaryMetadataEntry final : public Metadata::Entry {
public:
  BinaryMetadataEntry(std::string key, std::vector<uint8_t> value)
      : mKey(std::move(key)),
        mValue(std::move(value)) {
  }

  [[nodiscard]] const std::string& getKey() const { return mKey; }
  [[nodiscard]] const std::vector<uint8_t>& getValue() const { return mValue; }

  std::string toString() override {
    return "BinaryMetadataEntry key=" + mKey + " size=" + std::to_string(mValue.size());
  }

private:
  std::string mKey;
  std::vector<uint8_t> mValue;
};

/**
 * One ID3 frame, with the fields that the parser decoded.
 *
 * <p>The decoded text is kept apart from the payload rather than replacing it: a player that shows
 * the title of an MP3 reads {@link getText()}, and a caller that wants a frame the parser has no
 * model for reads {@link getPayload()} and interprets it itself.
 */
class Id3FrameMetadataEntry final : public Metadata::Entry {
public:
  Id3FrameMetadataEntry(std::string frameId, std::string description, std::string language,
                        std::string text, std::vector<uint8_t> payload)
      : mFrameId(std::move(frameId)),
        mDescription(std::move(description)),
        mLanguage(std::move(language)),
        mText(std::move(text)),
        mPayload(std::move(payload)) {
  }

  [[nodiscard]] const std::string& getFrameId() const { return mFrameId; }
  [[nodiscard]] const std::string& getDescription() const { return mDescription; }
  [[nodiscard]] const std::string& getLanguage() const { return mLanguage; }
  [[nodiscard]] const std::string& getText() const { return mText; }
  [[nodiscard]] const std::vector<uint8_t>& getPayload() const { return mPayload; }

  std::string toString() override {
    return "Id3FrameMetadataEntry id=" + mFrameId + " text=" + mText;
  }

private:
  std::string mFrameId;
  std::string mDescription;
  std::string mLanguage;
  std::string mText;
  std::vector<uint8_t> mPayload;
};

/** An MP4 `emsg` event, i.e. a metadata sample of a fragmented or unfragmented movie. */
class EmsgMetadataEntry final : public Metadata::Entry {
public:
  EmsgMetadataEntry(std::string schemeIdUri, std::string value, int64_t eventDurationUs,
                    uint64_t id, std::vector<uint8_t> messageData)
      : mSchemeIdUri(std::move(schemeIdUri)),
        mValue(std::move(value)),
        mEventDurationUs(eventDurationUs),
        mId(id),
        mMessageData(std::move(messageData)) {
  }

  [[nodiscard]] const std::string& getSchemeIdUri() const { return mSchemeIdUri; }
  [[nodiscard]] const std::string& getValue() const { return mValue; }
  [[nodiscard]] int64_t getEventDurationUs() const { return mEventDurationUs; }
  [[nodiscard]] uint64_t getId() const { return mId; }
  [[nodiscard]] const std::vector<uint8_t>& getMessageData() const { return mMessageData; }

  std::string toString() override {
    return "EmsgMetadataEntry scheme=" + mSchemeIdUri + " value=" + mValue;
  }

private:
  std::string mSchemeIdUri;
  std::string mValue;
  int64_t mEventDurationUs;
  uint64_t mId;
  std::vector<uint8_t> mMessageData;
};

/** A SCTE-35 splice information section, as carried by an MPEG-TS or an MP4 track. */
class Scte35MetadataEntry final : public Metadata::Entry {
public:
  explicit Scte35MetadataEntry(std::vector<uint8_t> spliceInfo)
      : mSpliceInfo(std::move(spliceInfo)) {
  }

  [[nodiscard]] const std::vector<uint8_t>& getSpliceInfo() const { return mSpliceInfo; }

  std::string toString() override {
    return "Scte35MetadataEntry size=" + std::to_string(mSpliceInfo.size());
  }

private:
  std::vector<uint8_t> mSpliceInfo;
};

/** A typed metadata entry stored under a key in an MP4 `mdta` box. */
class MdtaMetadataEntry final : public Metadata::Entry {
public:
  enum class ValueType : uint8_t {
    kUtf8 = 1,
    kInteger = 21,
    kFloat = 23,
    kUnsignedInt8 = 75,
    kUnsignedInt64 = 78,
    kBinary = 0,
  };

  MdtaMetadataEntry(std::string key, std::string value)
      : mKey(std::move(key)),
        mValue(value.begin(), value.end()),
        mType(ValueType::kUtf8) {
  }

  MdtaMetadataEntry(std::string key, std::vector<uint8_t> value, const ValueType type)
      : mKey(std::move(key)),
        mValue(std::move(value)),
        mType(type) {
  }

  MdtaMetadataEntry(std::string key, const int64_t value)
      : mKey(std::move(key)),
        mValue(sizeof(int64_t)),
        mType(ValueType::kInteger) {
    const auto raw = static_cast<uint64_t>(value);
    for (size_t i = 0; i < sizeof(uint64_t); ++i) {
      mValue[i] = static_cast<uint8_t>(raw >> (56 - i * 8));
    }
  }

  [[nodiscard]] const std::string& getKey() const { return mKey; }
  [[nodiscard]] const std::vector<uint8_t>& getValue() const { return mValue; }
  [[nodiscard]] ValueType getValueType() const { return mType; }

  std::string toString() override {
    return "MdtaMetadataEntry key=" + mKey + " size=" + std::to_string(mValue.size());
  }

private:
  std::string mKey;
  std::vector<uint8_t> mValue;
  ValueType mType;
};

/** ISO 6709 location metadata. */
class Mp4LocationData final : public Metadata::Entry {
public:
  Mp4LocationData(const double latitude, const double longitude)
      : mLatitude(latitude),
        mLongitude(longitude) {
  }

  [[nodiscard]] double getLatitude() const { return mLatitude; }
  [[nodiscard]] double getLongitude() const { return mLongitude; }

  std::string toString() override {
    return "Mp4LocationData " + std::to_string(mLatitude) + "," + std::to_string(mLongitude);
  }

private:
  double mLatitude;
  double mLongitude;
};

/** Clockwise rotation of the video, in degrees. */
class Mp4OrientationData final : public Metadata::Entry {
public:
  explicit Mp4OrientationData(const int rotation) : mRotation(rotation) {}

  [[nodiscard]] int getRotation() const { return mRotation; }

  std::string toString() override {
    return "Mp4OrientationData " + std::to_string(mRotation);
  }

private:
  int mRotation;
};

/** MP4 creation/modification times, in seconds since 1904-01-01 UTC. */
class Mp4TimestampData final : public Metadata::Entry {
public:
  Mp4TimestampData(const int64_t creationTime, const int64_t modificationTime)
      : mCreationTime(creationTime),
        mModificationTime(modificationTime) {
  }

  [[nodiscard]] int64_t getCreationTime() const { return mCreationTime; }
  [[nodiscard]] int64_t getModificationTime() const { return mModificationTime; }

  std::string toString() override {
    return "Mp4TimestampData " + std::to_string(mCreationTime) + "/" +
           std::to_string(mModificationTime);
  }

private:
  int64_t mCreationTime;
  int64_t mModificationTime;
};

/** An XMP packet, stored in an MP4 `uuid` box. */
class XmpData final : public Metadata::Entry {
public:
  explicit XmpData(std::vector<uint8_t> data) : mData(std::move(data)) {}

  [[nodiscard]] const std::vector<uint8_t>& getData() const { return mData; }

  std::string toString() override {
    return "XmpData size=" + std::to_string(mData.size());
  }

private:
  std::vector<uint8_t> mData;
};

/** One entry of an MP4 chapter list. */
class Mp4Chapter final {
public:
  Mp4Chapter(const int64_t timeUs, std::string title)
      : mTimeUs(timeUs),
        mTitle(std::move(title)) {
  }

  [[nodiscard]] int64_t getTimeUs() const { return mTimeUs; }
  [[nodiscard]] const std::string& getTitle() const { return mTitle; }

private:
  int64_t mTimeUs;
  std::string mTitle;
};

/** A chapter list, written as a Nero/QuickTime `chpl` box. */
class Mp4ChaptersData final : public Metadata::Entry {
public:
  explicit Mp4ChaptersData(std::vector<Mp4Chapter> chapters)
      : mChapters(std::move(chapters)) {
  }

  [[nodiscard]] const std::vector<Mp4Chapter>& getChapters() const { return mChapters; }

  std::string toString() override {
    return "Mp4ChaptersData count=" + std::to_string(mChapters.size());
  }

private:
  std::vector<Mp4Chapter> mChapters;
};

} // namespace coral

#endif // CORAL_MEDIA_METADATA_ENTRIES_H
