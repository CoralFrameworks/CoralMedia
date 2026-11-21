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

#include <gtest/gtest.h>

#include <datasource/DataSource.h>
#include <extractor/DefaultExtractorsFactory.h>
#include <extractor/Reader.h>
#include <media/MetadataEntries.h>
#include <mp4/Mp4Muxer.h>
#include <muxer/Muxer.h>
#include <muxer/MuxerFactory.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace coral;
using namespace coral::muxer;
using namespace coral::muxer::mp4;

/** A non-seekable sink used to exercise the fast-start spool. */
class MemoryMuxerOutput final : public MuxerOutput {
public:
  bool write(const uint8_t* data, const size_t size) override {
    if (data == nullptr && size > 0) {
      return false;
    }
    if (size > 0) {
      bytes.insert(bytes.end(), data, data + size);
    }
    return true;
  }

  bool seek(int64_t /*position*/) override { return false; }

  [[nodiscard]] int64_t position() const override {
    return static_cast<int64_t>(bytes.size());
  }

  [[nodiscard]] bool isSeekable() const override { return false; }

  bool truncate(int64_t /*size*/) override { return false; }

  bool copyBytes(int64_t /*destination*/, int64_t /*source*/, int64_t /*size*/) override {
    return false;
  }

  bool close() override { return true; }

  void remove() override { bytes.clear(); }

  std::vector<uint8_t> bytes;
};

std::string fixturePath(const std::string& name) { return std::string(CORAL_ASSETS_DIR) + "/video/" + name; }

std::string assetPath(const std::string& name) { return std::string(CORAL_ASSETS_DIR) + "/" + name; }

uint32_t muxerFlags(const uint32_t sampleFlags) {
  uint32_t flags = 0;
  if ((sampleFlags & AVSample::kFlagKeyFrame) != 0) {
    flags |= static_cast<uint32_t>(SampleFlag::kKeyFrame);
  }
  if ((sampleFlags & AVSample::kFlagCodecConfig) != 0) {
    flags |= static_cast<uint32_t>(SampleFlag::kCodecConfig);
  }
  return flags;
}

uint32_t typeFor(const char type[4]) {
  return (static_cast<uint32_t>(static_cast<uint8_t>(type[0])) << 24) |
         (static_cast<uint32_t>(static_cast<uint8_t>(type[1])) << 16) |
         (static_cast<uint32_t>(static_cast<uint8_t>(type[2])) << 8) |
         static_cast<uint32_t>(static_cast<uint8_t>(type[3]));
}

std::string typeName(const uint32_t type) {
  std::string name(4, '\0');
  name[0] = static_cast<char>((type >> 24) & 0xFF);
  name[1] = static_cast<char>((type >> 16) & 0xFF);
  name[2] = static_cast<char>((type >> 8) & 0xFF);
  name[3] = static_cast<char>(type & 0xFF);
  return name;
}

struct Box {
  uint32_t type = 0;
  int64_t offset = 0;
  int64_t size = 0;
  int64_t payloadOffset = 0;
  int64_t payloadSize = 0;
};

uint32_t readU32(const std::vector<uint8_t>& data, const size_t offset) {
  return (static_cast<uint32_t>(data[offset]) << 24) | (static_cast<uint32_t>(data[offset + 1]) << 16) |
         (static_cast<uint32_t>(data[offset + 2]) << 8) | static_cast<uint32_t>(data[offset + 3]);
}

uint64_t readU64(const std::vector<uint8_t>& data, const size_t offset) {
  return (static_cast<uint64_t>(readU32(data, offset)) << 32) | readU32(data, offset + 4);
}

std::vector<Box> parseBoxes(const std::vector<uint8_t>& data, size_t start, size_t end) {
  std::vector<Box> boxes;
  if (end > data.size()) {
    end = data.size();
  }
  size_t position = start;
  while (position + 8 <= end) {
    const uint32_t size32 = readU32(data, position);
    const uint32_t type = readU32(data, position + 4);
    int64_t size = size32;
    size_t headerSize = 8;
    if (size32 == 1) {
      if (position + 16 > end) {
        break;
      }
      size = static_cast<int64_t>(readU64(data, position + 8));
      headerSize = 16;
    } else if (size32 == 0) {
      size = static_cast<int64_t>(end - position);
    }
    if (size < static_cast<int64_t>(headerSize) || position + static_cast<size_t>(size) > end) {
      break;
    }
    Box box;
    box.type = type;
    box.offset = static_cast<int64_t>(position);
    box.size = size;
    box.payloadOffset = static_cast<int64_t>(position + headerSize);
    box.payloadSize = size - static_cast<int64_t>(headerSize);
    boxes.push_back(box);
    position += static_cast<size_t>(size);
  }
  return boxes;
}

const Box* findBox(const std::vector<Box>& boxes, const char type[4]) {
  const uint32_t wanted = typeFor(type);
  for (const auto& box: boxes) {
    if (box.type == wanted) {
      return &box;
    }
  }
  return nullptr;
}

std::vector<Box> childBoxes(const std::vector<uint8_t>& data, const Box& parent) {
  return parseBoxes(data, static_cast<size_t>(parent.payloadOffset),
          static_cast<size_t>(parent.payloadOffset + parent.payloadSize));
}

/** Returns the child boxes of the first sample entry of an `stsd` box. */
std::vector<Box> sampleEntryChildren(const std::vector<uint8_t>& data, const Box& stsd) {
  const auto entries = parseBoxes(data, static_cast<size_t>(stsd.payloadOffset) + 8,
    static_cast<size_t>(stsd.payloadOffset + stsd.payloadSize));
  if (entries.empty()) {
    return {};
  }
  const size_t childrenOffset = static_cast<size_t>(entries.front().payloadOffset) + 78;
  return parseBoxes(data, childrenOffset,
    static_cast<size_t>(entries.front().offset + entries.front().size));
}

struct MdtaValue {
  uint32_t type = 0;
  std::vector<uint8_t> value;
};

std::map<std::string, MdtaValue> readMdtaValues(const std::vector<uint8_t>& data,
  const Box& udta) {
  std::map<std::string, MdtaValue> values;
  const auto udtaChildren = childBoxes(data, udta);
  const Box* meta = findBox(udtaChildren, "meta");
  if (meta == nullptr) {
    return values;
  }
  const auto metaChildren = parseBoxes(data, static_cast<size_t>(meta->payloadOffset) + 4,
    static_cast<size_t>(meta->payloadOffset + meta->payloadSize));
  const Box* keys = findBox(metaChildren, "keys");
  const Box* ilst = findBox(metaChildren, "ilst");
  if (keys == nullptr || ilst == nullptr) {
    return values;
  }
  std::vector<std::string> keyNames;
  const size_t keyPayload = static_cast<size_t>(keys->payloadOffset);
  const size_t keyCount = readU32(data, keyPayload + 4);
  size_t keyCursor = keyPayload + 8;
  for (size_t i = 0; i < keyCount && keyCursor + 8 <= data.size(); ++i) {
    const uint32_t entrySize = readU32(data, keyCursor);
    if (entrySize < 8 || keyCursor + entrySize > data.size()) {
      break;
    }
    keyNames.emplace_back(reinterpret_cast<const char*>(data.data() + keyCursor + 8),
      entrySize - 8);
    keyCursor += entrySize;
  }
  const auto items = childBoxes(data, *ilst);
  for (const auto& item: items) {
    const size_t index = item.type;
    if (index == 0 || index > keyNames.size()) {
      continue;
    }
    const auto dataBoxes = childBoxes(data, item);
    const Box* dataBox = findBox(dataBoxes, "data");
    if (dataBox == nullptr || dataBox->payloadSize < 8) {
      continue;
    }
    MdtaValue entry;
    entry.type = readU32(data, static_cast<size_t>(dataBox->payloadOffset)) & 0xFFFFFFu;
    const auto* begin = data.data() + dataBox->payloadOffset + 8;
    entry.value.assign(begin, begin + (dataBox->payloadSize - 8));
    values[keyNames[index - 1]] = entry;
  }
  return values;
}

std::vector<uint8_t> readFile(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

bool readTrackInfo(const std::string& path, std::vector<int>* trackIds,
        std::vector<std::shared_ptr<MediaFormat>>* formats, int64_t* durationUs = nullptr) {
  auto dataSource = std::make_shared<AVFileDataSource>();
  if (!dataSource->open(path)) {
    return false;
  }
  AVReader reader(dataSource, AVExtractorsFactory::createDefault(-1, dataSource->getLength()));
  if (!reader.prepare()) {
    return false;
  }
  *trackIds = reader.getTrackIds();
  for (const int id: *trackIds) {
    formats->push_back(reader.getTrackFormat(id));
  }
  if (durationUs != nullptr) {
    *durationUs = reader.getDurationUs();
  }
  return true;
}

/**
 * Counts the audio/video tracks of a read-back file: the extractor also declares auxiliary tracks
 * next to them, e.g. the CEA-708 caption track that the MP4 extractor derives from the SEI of an
 * H.264/H.265 video track.
 */
size_t countPrimaryTracks(const std::vector<std::shared_ptr<MediaFormat>>& formats) {
  return static_cast<size_t>(std::count_if(formats.begin(), formats.end(),
    [](const std::shared_ptr<MediaFormat>& format) {
      return format != nullptr && (format->isAudio() || format->isVideo());
    }));
}

bool readSampleTimes(const std::string& path, const size_t trackIndex, std::vector<int64_t>* times) {
  auto dataSource = std::make_shared<AVFileDataSource>();
  if (!dataSource->open(path)) {
    return false;
  }
  AVReader reader(dataSource, AVExtractorsFactory::createDefault(-1, dataSource->getLength()));
  if (!reader.prepare()) {
    return false;
  }
  const auto ids = reader.getTrackIds();
  if (trackIndex >= ids.size()) {
    return false;
  }
  reader.setTrackConsumer(ids[trackIndex], [times](const AVSample& sample) {
    if (!sample.isEndOfStream() && sample.size() > 0) {
      times->push_back(sample.timeUs);
    }
  });
  while (true) {
    const int result = reader.read();
    if (result == AVReader::kResultError) {
      return false;
    }
    if (result == AVReader::kResultEndOfInput) {
      break;
    }
  }
  reader.endTracks();
  return true;
}

struct RemuxSettings {
  MuxerOptions options;
  Container container = Container::kMp4;
  std::shared_ptr<Metadata> metadata;
  int64_t timeOffsetUs = 0;
  int64_t videoTimeOffsetUs = 0;
  int64_t maxDurationUs = 0;
};

/** Remuxes the encoded packets of an input file without decoding or re-encoding. */
/** Reads the format of the first track of @p path, or null when the file cannot be read. */
std::shared_ptr<MediaFormat> readFirstFormat(const std::filesystem::path& path) {
  auto dataSource = std::make_shared<AVFileDataSource>();
  if (!dataSource->open(path.string())) {
    return nullptr;
  }
  AVReader reader(dataSource, AVExtractorsFactory::createDefault(-1, dataSource->getLength()));
  for (int id = 0; id < 8; ++id) {
    reader.setTrackConsumer(id, [](const AVSample&) {});
  }
  if (!reader.prepare()) {
    return nullptr;
  }
  while (reader.read() == AVReader::kResultContinue) {
  }
  const auto ids = reader.getTrackIds();
  return ids.empty() ? nullptr : reader.getTrackFormat(ids.front());
}
void remuxFile(const std::string& input, const std::filesystem::path& output, const RemuxSettings& settings = {}) {
  auto dataSource = std::make_shared<AVFileDataSource>();
  ASSERT_TRUE(dataSource->open(input));
  AVReader reader(dataSource, AVExtractorsFactory::createDefault(-1, dataSource->getLength()));
  ASSERT_TRUE(reader.prepare()) << reader.getLastError();

  auto factory = createMuxerFactory();
  ASSERT_NE(nullptr, factory);
  Error error;
  std::filesystem::remove(output);
  auto muxer = factory->create(output.string(), settings.container, settings.options, &error);
  ASSERT_NE(nullptr, muxer) << error.message;
  if (settings.metadata != nullptr) {
    ASSERT_TRUE(muxer->addMetadata(settings.metadata));
  }

  std::map<int, int> trackIds;
  std::map<int, bool> trackIsVideo;
  std::set<int> skippedTracks;
  for (const int id: reader.getTrackIds()) {
    const auto format = reader.getTrackFormat(id);
    if (format == nullptr || format->isDerivedTrack()) {
      // The captions that a track derives from another one are already written with their source
      // track, so the muxer would duplicate them.
      skippedTracks.insert(id);
      continue;
    }
    const int muxerTrack = muxer->addTrack(format);
    if (muxerTrack < 0) {
      // A remux keeps what the container can represent: a subtitle or metadata track of a format
      // the muxer does not write (e.g. Matroska PGS or VobSub, or an `emsg` track) is skipped.
      const std::string mime = format->getMimeType();
      ASSERT_TRUE(format->isSubtitle() || mime == "application/id3" || mime == "application/x-emsg" ||
                  mime == "application/x-scte35")
              << muxer->getError().message;
      skippedTracks.insert(id);
      continue;
    }
    trackIds[id] = muxerTrack;
    trackIsVideo[id] = format->isVideo();
  }

  for (const auto& [readerTrack, muxerTrack]: trackIds) {
    reader.setTrackConsumer(readerTrack, [&, muxerTrack, readerTrack](const AVSample& sample) {
      if (sample.isEndOfStream() || sample.isCodecConfig() || sample.size() == 0) {
        return;
      }
      if (settings.maxDurationUs > 0 && sample.timeUs > settings.maxDurationUs) {
        return;
      }
      const int64_t offset = settings.timeOffsetUs + (trackIsVideo[readerTrack] ? settings.videoTimeOffsetUs : 0);
      SampleInfo info;
      info.presentationTimeUs = sample.timeUs + offset;
      info.flags = muxerFlags(sample.flags);
      ASSERT_TRUE(muxer->writeSample(muxerTrack, sample.array(), sample.size(), info)) << muxer->getError().message;
    });
  }

  // Some extractors (e.g. native FLAC) report the track ids during preparation but only publish
  // the format once the stream metadata was parsed. Register those consumers too and add the track
  // when its format becomes available.
  for (const int readerTrack: reader.getTrackIds()) {
    if (trackIds.find(readerTrack) != trackIds.end() || skippedTracks.count(readerTrack) != 0) {
      continue;
    }
    reader.setTrackConsumer(readerTrack, [&, readerTrack](const AVSample& sample) {
      if (trackIds.find(readerTrack) == trackIds.end()) {
        if (skippedTracks.count(readerTrack) != 0) {
          return;
        }
        const auto format = reader.getTrackFormat(readerTrack);
        if (format == nullptr || format->isDerivedTrack()) {
          skippedTracks.insert(readerTrack);
          return;
        }
        const int muxerTrack = muxer->addTrack(format);
        if (muxerTrack < 0) {
          // A track the container cannot represent is skipped once and never retried.
          skippedTracks.insert(readerTrack);
          return;
        }
        trackIds[readerTrack] = muxerTrack;
        trackIsVideo[readerTrack] = format->isVideo();
      }
      if (sample.isEndOfStream() || sample.isCodecConfig() || sample.size() == 0) {
        return;
      }
      if (settings.maxDurationUs > 0 && sample.timeUs > settings.maxDurationUs) {
        return;
      }
      const int64_t offset = settings.timeOffsetUs + (trackIsVideo[readerTrack] ? settings.videoTimeOffsetUs : 0);
      SampleInfo info;
      info.presentationTimeUs = sample.timeUs + offset;
      info.flags = muxerFlags(sample.flags);
      ASSERT_TRUE(muxer->writeSample(trackIds[readerTrack], sample.array(), sample.size(), info))
              << muxer->getError().message;
    });
  }

  while (true) {
    const int result = reader.read();
    ASSERT_NE(AVReader::kResultError, result) << reader.getLastError();
    if (result == AVReader::kResultEndOfInput) {
      break;
    }
  }
  reader.endTracks();
  ASSERT_TRUE(muxer->finish()) << muxer->getError().message;
  EXPECT_EQ(Status::kCompleted, muxer->getStatus());
  ASSERT_TRUE(std::filesystem::exists(output));
}

bool readFirstFormat(const std::string& path, std::shared_ptr<MediaFormat>* format) {
  auto dataSource = std::make_shared<AVFileDataSource>();
  if (!dataSource->open(path)) {
    return false;
  }
  AVReader reader(dataSource, AVExtractorsFactory::createDefault(-1, dataSource->getLength()));
  if (!reader.prepare()) {
    return false;
  }
  const auto ids = reader.getTrackIds();
  if (ids.empty()) {
    return false;
  }
  *format = reader.getTrackFormat(ids.front());
  if (*format != nullptr) {
    return true;
  }
  reader.setTrackConsumer(ids.front(), [](const AVSample&) {});
  while (reader.read() == AVReader::kResultContinue) {
    *format = reader.getTrackFormat(ids.front());
    if (*format != nullptr) {
      return true;
    }
  }
  *format = reader.getTrackFormat(ids.front());
  return *format != nullptr;
}

bool readVideoFormat(const std::string& path, std::shared_ptr<MediaFormat>* format) {
  auto dataSource = std::make_shared<AVFileDataSource>();
  if (!dataSource->open(path)) {
    return false;
  }
  AVReader reader(dataSource, AVExtractorsFactory::createDefault(-1, dataSource->getLength()));
  if (!reader.prepare()) {
    return false;
  }
  const auto ids = reader.getTrackIds();
  for (const int id: ids) {
    const auto trackFormat = reader.getTrackFormat(id);
    if (trackFormat != nullptr && trackFormat->isVideo()) {
      *format = trackFormat;
      return true;
    }
  }
  for (const int id: ids) {
    reader.setTrackConsumer(id, [](const AVSample&) {});
  }
  while (reader.read() == AVReader::kResultContinue) {
    for (const int id: ids) {
      const auto trackFormat = reader.getTrackFormat(id);
      if (trackFormat != nullptr && trackFormat->isVideo()) {
        *format = trackFormat;
        return true;
      }
    }
  }
  return false;
}

void checkRemuxedFile(const std::filesystem::path& output, const int64_t expectedDurationUs,
        const size_t expectedTracks = 2) {
  std::vector<int> trackIds;
  std::vector<std::shared_ptr<MediaFormat>> formats;
  int64_t durationUs = 0;
  ASSERT_TRUE(readTrackInfo(output.string(), &trackIds, &formats, &durationUs));
  ASSERT_GE(trackIds.size(), expectedTracks);
  EXPECT_EQ(expectedTracks, countPrimaryTracks(formats));
  EXPECT_NEAR(static_cast<double>(expectedDurationUs), static_cast<double>(durationUs), expectedDurationUs * 0.02);
  bool hasVideo = false;
  bool hasAudio = false;
  for (const auto& format: formats) {
    if (format == nullptr) {
      continue;
    }
    hasVideo = hasVideo || format->isVideo();
    hasAudio = hasAudio || format->isAudio();
  }
  if (expectedTracks == 2) {
    EXPECT_TRUE(hasVideo);
    EXPECT_TRUE(hasAudio);
  }
}

void writeSyntheticTrack(const std::filesystem::path& output, const std::shared_ptr<MediaFormat>& format,
        const int sampleCount, const int64_t durationUs, const bool video) {
  auto factory = createMuxerFactory();
  ASSERT_NE(nullptr, factory);
  Error error;
  std::filesystem::remove(output);
  auto muxer = factory->create(output.string(), Container::kMp4, MuxerOptions(), &error);
  ASSERT_NE(nullptr, muxer) << error.message;
  const int track = muxer->addTrack(format);
  ASSERT_GE(track, 0) << muxer->getError().message;

  const std::vector<uint8_t> payload(32, 0x5A);
  for (int i = 0; i < sampleCount; ++i) {
    SampleInfo info;
    info.presentationTimeUs = static_cast<int64_t>(i) * durationUs;
    if (video && i == 0) {
      info.flags = static_cast<uint32_t>(SampleFlag::kKeyFrame);
    }
    ASSERT_TRUE(muxer->writeSample(track, payload.data(), payload.size(), info)) << muxer->getError().message;
  }
  ASSERT_TRUE(muxer->finish()) << muxer->getError().message;
  ASSERT_TRUE(std::filesystem::exists(output));
}

std::string mimeOfFirstTrack(const std::filesystem::path& output) {
  std::vector<int> trackIds;
  std::vector<std::shared_ptr<MediaFormat>> formats;
  if (!readTrackInfo(output.string(), &trackIds, &formats) || formats.empty() || formats.front() == nullptr) {
    return {};
  }
  return formats.front()->getMimeType();
}

} // namespace

TEST(Mp4MuxerTest, RemuxesH264AndAac) {
  const auto input = fixturePath("video720p_5s.mp4");
  if (!std::filesystem::exists(input)) {
    GTEST_SKIP() << "fixture is not present";
  }
  const auto output = std::filesystem::current_path() / "coral-muxer-h264-aac.mp4";
  remuxFile(input, output);
  if (!testing::Test::HasFatalFailure()) {
    checkRemuxedFile(output, 5'040'000);
  }
  if (std::getenv("CORAL_KEEP_MUXER_OUTPUT") == nullptr) {
    std::filesystem::remove(output);
  }
}

TEST(Mp4MuxerTest, WritesTypedMdtaMetadata) {
  const auto input = fixturePath("video720p_5s.mp4");
  if (!std::filesystem::exists(input)) {
    GTEST_SKIP() << "fixture is not present";
  }
  const auto output = std::filesystem::current_path() / "coral-muxer-metadata.mp4";
  RemuxSettings settings;
  settings.metadata = std::make_shared<Metadata>(std::vector<std::shared_ptr<Metadata::Entry>>{
      std::make_shared<MdtaMetadataEntry>("com.coral.test", "metadata-value") });
  remuxFile(input, output, settings);
  if (!testing::Test::HasFatalFailure()) {
    const auto bytes = readFile(output);
    const std::string text(bytes.begin(), bytes.end());
    EXPECT_NE(std::string::npos, text.find("mdta"));
    EXPECT_NE(std::string::npos, text.find("com.coral.test"));
    EXPECT_NE(std::string::npos, text.find("metadata-value"));
  }
  if (std::getenv("CORAL_KEEP_MUXER_OUTPUT") == nullptr) {
    std::filesystem::remove(output);
  }
}

TEST(Mp4MuxerTest, RemuxesHevcAndAac) {
  const auto input = fixturePath("hdr10-video-with-sdr-container.mp4");
  if (!std::filesystem::exists(input)) {
    GTEST_SKIP() << "fixture is not present";
  }
  const auto output = std::filesystem::current_path() / "coral-muxer-hevc-aac.mp4";
  remuxFile(input, output);
  if (!testing::Test::HasFatalFailure()) {
    checkRemuxedFile(output, 1'017'400);
  }
  if (std::getenv("CORAL_KEEP_MUXER_OUTPUT") == nullptr) {
    std::filesystem::remove(output);
  }
}

TEST(Mp4MuxerTest, WritesLocationOrientationTimestampAndXmp) {
  const auto input = fixturePath("video720p_5s.mp4");
  if (!std::filesystem::exists(input)) {
    GTEST_SKIP() << "fixture is not present";
  }
  const auto output = std::filesystem::current_path() / "coral-muxer-typed-metadata.mp4";
  const std::string xmp = "<xmp>coral</xmp>";
  RemuxSettings settings;
  settings.metadata = std::make_shared<Metadata>(
          std::vector<std::shared_ptr<Metadata::Entry>>{ std::make_shared<Mp4LocationData>(37.4219, -122.0840),
              std::make_shared<Mp4OrientationData>(90), std::make_shared<Mp4TimestampData>(123456, 654321),
              std::make_shared<XmpData>(std::vector<uint8_t>(xmp.begin(), xmp.end())),
              std::make_shared<MdtaMetadataEntry>("com.coral.typed", "value") });
  remuxFile(input, output, settings);
  if (!testing::Test::HasFatalFailure()) {
    const auto data = readFile(output);
    const auto topLevel = parseBoxes(data, 0, data.size());
    const Box* moov = findBox(topLevel, "moov");
    ASSERT_NE(nullptr, moov);
    const auto moovChildren = childBoxes(data, *moov);

    const Box* mvhd = findBox(moovChildren, "mvhd");
    ASSERT_NE(nullptr, mvhd);
    EXPECT_EQ(0u, readU32(data, static_cast<size_t>(mvhd->payloadOffset)) >> 24);
    EXPECT_EQ(123456u, readU32(data, static_cast<size_t>(mvhd->payloadOffset) + 4));

    const Box* udta = findBox(moovChildren, "udta");
    ASSERT_NE(nullptr, udta);
    const auto udtaChildren = childBoxes(data, *udta);
    const char xyz[4] = { '\xA9', 'x', 'y', 'z' };
    const Box* location = findBox(udtaChildren, xyz);
    ASSERT_NE(nullptr, location);
    const std::string locationText(reinterpret_cast<const char*>(data.data() + location->payloadOffset),
            static_cast<size_t>(location->payloadSize));
    EXPECT_NE(std::string::npos, locationText.find("+37.4219-122.0840"));

    const Box* meta = findBox(udtaChildren, "meta");
    ASSERT_NE(nullptr, meta);
    const auto metaChildren = parseBoxes(data, static_cast<size_t>(meta->payloadOffset) + 4,
            static_cast<size_t>(meta->payloadOffset + meta->payloadSize));
    EXPECT_NE(nullptr, findBox(metaChildren, "keys"));
    EXPECT_NE(nullptr, findBox(metaChildren, "ilst"));

    const Box* trak = findBox(moovChildren, "trak");
    ASSERT_NE(nullptr, trak);
    const auto trakChildren = childBoxes(data, *trak);
    const Box* tkhd = findBox(trakChildren, "tkhd");
    ASSERT_NE(nullptr, tkhd);
    const size_t matrix = static_cast<size_t>(tkhd->payloadOffset) + 40;
    EXPECT_EQ(0u, readU32(data, matrix));
    EXPECT_EQ(0x00010000u, readU32(data, matrix + 4));
    EXPECT_EQ(static_cast<uint32_t>(-0x00010000), readU32(data, matrix + 12));

    const Box* uuid = findBox(topLevel, "uuid");
    ASSERT_NE(nullptr, uuid);
    EXPECT_EQ(16, uuid->payloadSize - static_cast<int64_t>(xmp.size()));
    const std::string xmpText(reinterpret_cast<const char*>(data.data() + uuid->payloadOffset + 16),
            static_cast<size_t>(uuid->payloadSize) - 16);
    EXPECT_EQ(xmp, xmpText);

    // The extractor reads the metadata that the writer just wrote.
    const auto format = readFirstFormat(output);
    ASSERT_NE(nullptr, format) << "the written file could not be read";
    ASSERT_NE(nullptr, format->getMetadata());
    bool hasLocation = false;
    bool hasMdta = false;
    bool hasXmp = false;
    for (const auto& entry : format->getMetadata()->getEntries()) {
      if (const auto* location = dynamic_cast<Mp4LocationData*>(entry.get())) {
        hasLocation = true;
        EXPECT_NEAR(37.4219, location->getLatitude(), 1e-6);
        EXPECT_NEAR(-122.0840, location->getLongitude(), 1e-6);
      } else if (const auto* mdta = dynamic_cast<MdtaMetadataEntry*>(entry.get())) {
        if (mdta->getKey() == "com.coral.typed") {
          hasMdta = true;
          EXPECT_EQ("value", std::string(mdta->getValue().begin(), mdta->getValue().end()));
        }
      } else if (const auto* xmpEntry = dynamic_cast<XmpData*>(entry.get())) {
        hasXmp = true;
        EXPECT_EQ(xmp, std::string(xmpEntry->getData().begin(), xmpEntry->getData().end()));
      }
    }
    EXPECT_TRUE(hasLocation);
    EXPECT_TRUE(hasMdta);
    EXPECT_TRUE(hasXmp);
    EXPECT_EQ(90, format->getRotation());
  }
  if (std::getenv("CORAL_KEEP_MUXER_OUTPUT") == nullptr) {
    std::filesystem::remove(output);
  }
}

TEST(Mp4MuxerTest, WritesChapterList) {
  const auto input = fixturePath("video720p_5s.mp4");
  if (!std::filesystem::exists(input)) {
    GTEST_SKIP() << "fixture is not present";
  }
  const auto output = std::filesystem::current_path() / "coral-muxer-chapters.mp4";
  RemuxSettings settings;
  settings.metadata = std::make_shared<Metadata>(
    std::vector<std::shared_ptr<Metadata::Entry>>{
      std::make_shared<Mp4ChaptersData>(std::vector<Mp4Chapter>{
        Mp4Chapter(0, "Intro"),
        Mp4Chapter(1'000'000, "Part two")
      })
    });
  remuxFile(input, output, settings);
  if (!testing::Test::HasFatalFailure()) {
    const auto data = readFile(output);
    const auto topLevel = parseBoxes(data, 0, data.size());
    const Box* moov = findBox(topLevel, "moov");
    ASSERT_NE(nullptr, moov);
    const auto moovChildren = childBoxes(data, *moov);
    const Box* udta = findBox(moovChildren, "udta");
    ASSERT_NE(nullptr, udta);
    const auto udtaChildren = childBoxes(data, *udta);
    const Box* chpl = findBox(udtaChildren, "chpl");
    ASSERT_NE(nullptr, chpl);
    const size_t payload = static_cast<size_t>(chpl->payloadOffset);
    EXPECT_EQ(1u, readU32(data, payload) >> 24);
    EXPECT_EQ(2u, data[payload + 8]);
    EXPECT_EQ(0u, readU64(data, payload + 9));
    EXPECT_EQ(5u, data[payload + 17]);
    EXPECT_EQ("Intro", std::string(reinterpret_cast<const char*>(data.data() + payload + 18), 5));

    // The extractor reads the chapter list that the writer just wrote.
    const auto format = readFirstFormat(output);
    ASSERT_NE(nullptr, format) << "the written file could not be read";
    const auto& metadata = format->getMetadata();
    ASSERT_NE(nullptr, metadata);
    const Mp4ChaptersData* chapters = nullptr;
    for (const auto& entry : metadata->getEntries()) {
      if (const auto* chaptersEntry = dynamic_cast<Mp4ChaptersData*>(entry.get())) {
        chapters = chaptersEntry;
      }
    }
    ASSERT_NE(nullptr, chapters);
    ASSERT_EQ(2u, chapters->getChapters().size());
    EXPECT_EQ("Intro", chapters->getChapters()[0].getTitle());
    EXPECT_EQ(0, chapters->getChapters()[0].getTimeUs());
    EXPECT_EQ("Part two", chapters->getChapters()[1].getTitle());
    EXPECT_EQ(1'000'000, chapters->getChapters()[1].getTimeUs());
  }
  if (std::getenv("CORAL_KEEP_MUXER_OUTPUT") == nullptr) {
    std::filesystem::remove(output);
  }
}

TEST(Mp4MuxerTest, FastStartPlacesMoovBeforeMdat) {
  const auto input = fixturePath("video720p_5s.mp4");
  if (!std::filesystem::exists(input)) {
    GTEST_SKIP() << "fixture is not present";
  }
  const auto output = std::filesystem::current_path() / "coral-muxer-faststart.mp4";
  RemuxSettings settings;
  settings.options.fastStart = true;
  remuxFile(input, output, settings);
  if (!testing::Test::HasFatalFailure()) {
    const auto data = readFile(output);
    const auto boxes = parseBoxes(data, 0, data.size());
    ASSERT_GE(boxes.size(), 3u);
    EXPECT_EQ("ftyp", typeName(boxes[0].type));
    EXPECT_EQ("moov", typeName(boxes[1].type));
    EXPECT_EQ("mdat", typeName(boxes[2].type));
    EXPECT_GT(boxes[2].size, 0);

    const auto moovChildren = childBoxes(data, boxes[1]);
    const Box* trak = findBox(moovChildren, "trak");
    ASSERT_NE(nullptr, trak);
    const auto trakChildren = childBoxes(data, *trak);
    const Box* mdia = findBox(trakChildren, "mdia");
    ASSERT_NE(nullptr, mdia);
    const auto mdiaChildren = childBoxes(data, *mdia);
    const Box* minf = findBox(mdiaChildren, "minf");
    ASSERT_NE(nullptr, minf);
    const auto minfChildren = childBoxes(data, *minf);
    const Box* stbl = findBox(minfChildren, "stbl");
    ASSERT_NE(nullptr, stbl);
    const auto stblChildren = childBoxes(data, *stbl);
    EXPECT_NE(nullptr, findBox(stblChildren, "co64"));

    checkRemuxedFile(output, 5'040'000);
  }
  if (std::getenv("CORAL_KEEP_MUXER_OUTPUT") == nullptr) {
    std::filesystem::remove(output);
  }
}

TEST(Mp4MuxerTest, FastStartWritesAuxiliaryTracksOnSeekableOutput) {
  const auto input = fixturePath("video720p_5s.mp4");
  if (!std::filesystem::exists(input)) {
    GTEST_SKIP() << "fixture is not present";
  }
  std::shared_ptr<MediaFormat> format;
  ASSERT_TRUE(readVideoFormat(input, &format));
  ASSERT_NE(nullptr, format);
  const auto output = std::filesystem::current_path() / "coral-muxer-axte-faststart.mp4";
  std::filesystem::remove(output);
  auto factory = createMuxerFactory();
  ASSERT_NE(nullptr, factory);
  MuxerOptions options;
  options.fastStart = true;
  options.auxiliaryTracksInAxte = true;
  Error error;
  auto muxer = factory->create(output.string(), Container::kMp4, options, &error);
  ASSERT_NE(nullptr, muxer) << error.message;
  const int primary = muxer->addTrack(format);
  TrackOptions auxiliaryOptions;
  auxiliaryOptions.auxiliary = true;
  const int auxiliary = muxer->addTrack(format, auxiliaryOptions);
  ASSERT_GE(primary, 0);
  ASSERT_GE(auxiliary, 0) << muxer->getError().message;
  const std::vector<uint8_t> payload(32, 0x5A);
  for (int i = 0; i < 2; ++i) {
    SampleInfo info;
    info.presentationTimeUs = static_cast<int64_t>(i) * 40000;
    info.flags = static_cast<uint32_t>(SampleFlag::kKeyFrame);
    ASSERT_TRUE(muxer->writeSample(primary, payload.data(), payload.size(), info));
    ASSERT_TRUE(muxer->writeSample(auxiliary, payload.data(), payload.size(), info));
  }
  ASSERT_TRUE(muxer->finish()) << muxer->getError().message;

  const auto data = readFile(output);
  const auto topLevel = parseBoxes(data, 0, data.size());
  ASSERT_EQ(4u, topLevel.size());
  EXPECT_EQ("ftyp", typeName(topLevel[0].type));
  EXPECT_EQ("moov", typeName(topLevel[1].type));
  EXPECT_EQ("mdat", typeName(topLevel[2].type));
  EXPECT_EQ("axte", typeName(topLevel[3].type));
  if (std::getenv("CORAL_KEEP_MUXER_OUTPUT") == nullptr) {
    std::filesystem::remove(output);
  }
}

TEST(Mp4MuxerTest, FastStartWritesAuxiliaryTracksToNonSeekableOutput) {
  const auto input = fixturePath("video720p_5s.mp4");
  if (!std::filesystem::exists(input)) {
    GTEST_SKIP() << "fixture is not present";
  }
  std::shared_ptr<MediaFormat> format;
  ASSERT_TRUE(readVideoFormat(input, &format));
  ASSERT_NE(nullptr, format);

  auto memory = std::make_unique<MemoryMuxerOutput>();
  auto* memoryRaw = memory.get();
  MuxerOptions options;
  options.fastStart = true;
  options.auxiliaryTracksInAxte = true;
  Error error;
  auto muxer = Mp4Muxer::create(std::move(memory), options, &error);
  ASSERT_NE(nullptr, muxer) << error.message;
  const int primary = muxer->addTrack(format, TrackOptions{});
  TrackOptions auxiliaryOptions;
  auxiliaryOptions.auxiliary = true;
  auxiliaryOptions.auxiliaryTrackType = 1;
  const int auxiliary = muxer->addTrack(format, auxiliaryOptions);
  ASSERT_GE(primary, 0) << muxer->getError().message;
  ASSERT_GE(auxiliary, 0) << muxer->getError().message;

  auto dataSource = std::make_shared<AVFileDataSource>();
  ASSERT_TRUE(dataSource->open(input));
  AVReader reader(dataSource, AVExtractorsFactory::createDefault(-1, dataSource->getLength()));
  ASSERT_TRUE(reader.prepare()) << reader.getLastError();
  const auto ids = reader.getTrackIds();
  ASSERT_FALSE(ids.empty());
  int videoReaderTrack = -1;
  for (const int id: ids) {
    const auto readerFormat = reader.getTrackFormat(id);
    if (readerFormat != nullptr && readerFormat->isVideo()) {
      videoReaderTrack = id;
      break;
    }
  }
  ASSERT_GE(videoReaderTrack, 0);
  reader.setTrackConsumer(videoReaderTrack, [&](const AVSample& sample) {
    if (sample.isEndOfStream() || sample.isCodecConfig() || sample.size() == 0) {
      return;
    }
    SampleInfo info;
    info.presentationTimeUs = sample.timeUs;
    info.flags = muxerFlags(sample.flags);
    ASSERT_TRUE(muxer->writeSample(primary, sample.array(), sample.size(), info));
    ASSERT_TRUE(muxer->writeSample(auxiliary, sample.array(), sample.size(), info));
  });
  while (true) {
    const int status = reader.read();
    ASSERT_NE(AVReader::kResultError, status) << reader.getLastError();
    if (status == AVReader::kResultEndOfInput) {
      break;
    }
  }
  reader.endTracks();
  ASSERT_TRUE(muxer->finish()) << muxer->getError().message;

  const auto data = memoryRaw->bytes;
  const auto topLevel = parseBoxes(data, 0, data.size());
  ASSERT_EQ(4u, topLevel.size());
  EXPECT_EQ("ftyp", typeName(topLevel[0].type));
  EXPECT_EQ("moov", typeName(topLevel[1].type));
  EXPECT_EQ("mdat", typeName(topLevel[2].type));
  EXPECT_EQ("axte", typeName(topLevel[3].type));
  const Box& moov = topLevel[1];
  const Box& axte = topLevel[3];
  const auto moovChildren = childBoxes(data, moov);
  const Box* udta = findBox(moovChildren, "udta");
  ASSERT_NE(nullptr, udta);
  const auto values = readMdtaValues(data, *udta);
  const auto offsetIt = values.find("auxiliary.tracks.offset");
  const auto lengthIt = values.find("auxiliary.tracks.length");
  ASSERT_NE(values.end(), offsetIt);
  ASSERT_NE(values.end(), lengthIt);
  ASSERT_EQ(8u, offsetIt->second.value.size());
  ASSERT_EQ(8u, lengthIt->second.value.size());
  uint64_t offset = 0;
  uint64_t length = 0;
  for (size_t i = 0; i < 8; ++i) {
    offset = (offset << 8) | offsetIt->second.value[i];
    length = (length << 8) | lengthIt->second.value[i];
  }
  EXPECT_EQ(static_cast<uint64_t>(axte.offset), offset);
  EXPECT_EQ(static_cast<uint64_t>(axte.size), length);
  EXPECT_EQ(data.size(), static_cast<size_t>(axte.offset + axte.size));

  const auto output = std::filesystem::current_path() / "coral-muxer-axte-nonseekable.mp4";
  {
    std::ofstream stream(output, std::ios::binary);
    ASSERT_TRUE(stream.is_open());
    stream.write(reinterpret_cast<const char*>(data.data()),
      static_cast<std::streamsize>(data.size()));
  }
  std::vector<int> trackIds;
  std::vector<std::shared_ptr<MediaFormat>> formats;
  int64_t durationUs = 0;
  ASSERT_TRUE(readTrackInfo(output.string(), &trackIds, &formats, &durationUs));
  EXPECT_EQ(1u, countPrimaryTracks(formats));
  EXPECT_NEAR(5'040'000, static_cast<double>(durationUs), 50'000);

  const auto payloadPath = std::filesystem::current_path() / "coral-muxer-axte-payload.mp4";
  {
    std::ofstream payload(payloadPath, std::ios::binary);
    ASSERT_TRUE(payload.is_open());
    payload.write(reinterpret_cast<const char*>(data.data() + axte.payloadOffset),
      static_cast<std::streamsize>(axte.payloadSize));
  }
  trackIds.clear();
  formats.clear();
  durationUs = 0;
  ASSERT_TRUE(readTrackInfo(payloadPath.string(), &trackIds, &formats, &durationUs));
  EXPECT_EQ(1u, countPrimaryTracks(formats));
  EXPECT_NEAR(5'040'000, static_cast<double>(durationUs), 50'000);

  if (std::getenv("CORAL_KEEP_MUXER_OUTPUT") == nullptr) {
    std::filesystem::remove(output);
    std::filesystem::remove(payloadPath);
  }
}

TEST(Mp4MuxerTest, FastStartWritesToNonSeekableOutput) {
  const auto input = fixturePath("video720p_5s.mp4");
  if (!std::filesystem::exists(input)) {
    GTEST_SKIP() << "fixture is not present";
  }
  std::shared_ptr<MediaFormat> format;
  ASSERT_TRUE(readVideoFormat(input, &format));
  ASSERT_NE(nullptr, format);

  auto memory = std::make_unique<MemoryMuxerOutput>();
  auto* memoryRaw = memory.get();
  MuxerOptions options;
  options.fastStart = true;
  Error error;
  auto muxer = Mp4Muxer::create(std::move(memory), options, &error);
  ASSERT_NE(nullptr, muxer) << error.message;
  const int track = muxer->addTrack(format, TrackOptions{});
  ASSERT_GE(track, 0) << muxer->getError().message;

  auto dataSource = std::make_shared<AVFileDataSource>();
  ASSERT_TRUE(dataSource->open(input));
  AVReader reader(dataSource, AVExtractorsFactory::createDefault(-1, dataSource->getLength()));
  ASSERT_TRUE(reader.prepare()) << reader.getLastError();
  const auto ids = reader.getTrackIds();
  ASSERT_FALSE(ids.empty());
  int videoReaderTrack = -1;
  for (const int id: ids) {
    const auto readerFormat = reader.getTrackFormat(id);
    if (readerFormat != nullptr && readerFormat->isVideo()) {
      videoReaderTrack = id;
      break;
    }
  }
  ASSERT_GE(videoReaderTrack, 0);
  reader.setTrackConsumer(videoReaderTrack, [&](const AVSample& sample) {
    if (sample.isEndOfStream() || sample.isCodecConfig() || sample.size() == 0) {
      return;
    }
    SampleInfo info;
    info.presentationTimeUs = sample.timeUs;
    info.flags = muxerFlags(sample.flags);
    ASSERT_TRUE(muxer->writeSample(track, sample.array(), sample.size(), info));
  });
  while (true) {
    const int status = reader.read();
    ASSERT_NE(AVReader::kResultError, status) << reader.getLastError();
    if (status == AVReader::kResultEndOfInput) {
      break;
    }
  }
  reader.endTracks();
  ASSERT_TRUE(muxer->finish()) << muxer->getError().message;
  ASSERT_FALSE(memoryRaw->bytes.empty());

  const auto data = memoryRaw->bytes;
  const auto topLevel = parseBoxes(data, 0, data.size());
  ASSERT_GE(topLevel.size(), 3u);
  EXPECT_EQ("ftyp", typeName(topLevel[0].type));
  EXPECT_EQ("moov", typeName(topLevel[1].type));
  EXPECT_EQ("mdat", typeName(topLevel[2].type));
  EXPECT_EQ(data.size(), static_cast<size_t>(topLevel[2].offset + topLevel[2].size));

  const auto output = std::filesystem::current_path() / "coral-muxer-nonseekable.mp4";
  std::ofstream stream(output, std::ios::binary);
  ASSERT_TRUE(stream.is_open());
  stream.write(reinterpret_cast<const char*>(data.data()),
    static_cast<std::streamsize>(data.size()));
  stream.close();
  std::vector<int> trackIds;
  std::vector<std::shared_ptr<MediaFormat>> formats;
  int64_t durationUs = 0;
  ASSERT_TRUE(readTrackInfo(output.string(), &trackIds, &formats, &durationUs));
  EXPECT_EQ(1u, countPrimaryTracks(formats));
  EXPECT_NEAR(5'040'000, static_cast<double>(durationUs), 50'000);
  if (std::getenv("CORAL_KEEP_MUXER_OUTPUT") == nullptr) {
    std::filesystem::remove(output);
  }
}

TEST(Mp4MuxerTest, SplitsInterleavedSamplesIntoChunks) {
  const auto input = fixturePath("video720p_5s.mp4");
  if (!std::filesystem::exists(input)) {
    GTEST_SKIP() << "fixture is not present";
  }
  const auto output = std::filesystem::current_path() / "coral-muxer-chunks.mp4";
  remuxFile(input, output);
  if (!testing::Test::HasFatalFailure()) {
    const auto data = readFile(output);
    const auto topLevel = parseBoxes(data, 0, data.size());
    const Box* moov = findBox(topLevel, "moov");
    ASSERT_NE(nullptr, moov);
    const auto moovChildren = childBoxes(data, *moov);
    const auto trak = findBox(moovChildren, "trak");
    ASSERT_NE(nullptr, trak);
    const auto trakChildren = childBoxes(data, *trak);
    const Box* mdia = findBox(trakChildren, "mdia");
    ASSERT_NE(nullptr, mdia);
    const auto mdiaChildren = childBoxes(data, *mdia);
    const Box* minf = findBox(mdiaChildren, "minf");
    ASSERT_NE(nullptr, minf);
    const auto minfChildren = childBoxes(data, *minf);
    const Box* stbl = findBox(minfChildren, "stbl");
    ASSERT_NE(nullptr, stbl);
    const auto stblChildren = childBoxes(data, *stbl);
    const Box* stco = findBox(stblChildren, "stco");
    ASSERT_NE(nullptr, stco);
    EXPECT_GT(readU32(data, static_cast<size_t>(stco->payloadOffset) + 4), 1u);
    checkRemuxedFile(output, 5'040'000);
  }
  if (std::getenv("CORAL_KEEP_MUXER_OUTPUT") == nullptr) {
    std::filesystem::remove(output);
  }
}

TEST(Mp4MuxerTest, NegativeTimestampsWriteAnEditList) {
  const auto input = fixturePath("video720p_5s.mp4");
  if (!std::filesystem::exists(input)) {
    GTEST_SKIP() << "fixture is not present";
  }
  const auto output = std::filesystem::current_path() / "coral-muxer-editlist.mp4";
  RemuxSettings settings;
  settings.timeOffsetUs = -1'000'000;
  remuxFile(input, output, settings);
  if (!testing::Test::HasFatalFailure()) {
    const auto data = readFile(output);
    const auto topLevel = parseBoxes(data, 0, data.size());
    const Box* moov = findBox(topLevel, "moov");
    ASSERT_NE(nullptr, moov);
    const auto moovChildren = childBoxes(data, *moov);
    const Box* trak = findBox(moovChildren, "trak");
    ASSERT_NE(nullptr, trak);
    const auto trakChildren = childBoxes(data, *trak);
    const Box* edts = findBox(trakChildren, "edts");
    ASSERT_NE(nullptr, edts);
    const auto edtsChildren = childBoxes(data, *edts);
    const Box* elst = findBox(edtsChildren, "elst");
    ASSERT_NE(nullptr, elst);
    EXPECT_EQ(1u, readU32(data, static_cast<size_t>(elst->payloadOffset)) >> 24);
    EXPECT_EQ(1u, readU32(data, static_cast<size_t>(elst->payloadOffset) + 4));
    const uint64_t mediaTime = readU64(data, static_cast<size_t>(elst->payloadOffset) + 16);
    EXPECT_GT(mediaTime, 0u);

    std::vector<int64_t> videoTimes;
    ASSERT_TRUE(readSampleTimes(output.string(), 0, &videoTimes));
    ASSERT_FALSE(videoTimes.empty());
    EXPECT_GE(videoTimes.front(), 0);
  }
  if (std::getenv("CORAL_KEEP_MUXER_OUTPUT") == nullptr) {
    std::filesystem::remove(output);
  }
}

TEST(Mp4MuxerTest, NegativeTimestampsCanBeShiftedInsteadOfEdited) {
  const auto input = fixturePath("video720p_5s.mp4");
  if (!std::filesystem::exists(input)) {
    GTEST_SKIP() << "fixture is not present";
  }
  const auto output = std::filesystem::current_path() / "coral-muxer-shifted.mp4";
  RemuxSettings settings;
  settings.timeOffsetUs = -1'000'000;
  settings.options.writeNegativeTimestampsInEditList = false;
  remuxFile(input, output, settings);
  if (!testing::Test::HasFatalFailure()) {
    const auto data = readFile(output);
    const auto topLevel = parseBoxes(data, 0, data.size());
    const Box* moov = findBox(topLevel, "moov");
    ASSERT_NE(nullptr, moov);
    const auto moovChildren = childBoxes(data, *moov);
    const auto trak = findBox(moovChildren, "trak");
    ASSERT_NE(nullptr, trak);
    const auto trakChildren = childBoxes(data, *trak);
    EXPECT_EQ(nullptr, findBox(trakChildren, "edts"));
    checkRemuxedFile(output, 5'040'000);
    std::vector<int64_t> videoTimes;
    ASSERT_TRUE(readSampleTimes(output.string(), 0, &videoTimes));
    ASSERT_FALSE(videoTimes.empty());
    EXPECT_EQ(0, videoTimes.front());
  }
  if (std::getenv("CORAL_KEEP_MUXER_OUTPUT") == nullptr) {
    std::filesystem::remove(output);
  }
}

TEST(Mp4MuxerTest, DelayedTrackUsesAnEmptyEditAndExtendsTheMovieDuration) {
  const auto input = fixturePath("video720p_5s.mp4");
  if (!std::filesystem::exists(input)) {
    GTEST_SKIP() << "fixture is not present";
  }
  const auto output = std::filesystem::current_path() / "coral-muxer-delayed.mp4";
  RemuxSettings settings;
  settings.videoTimeOffsetUs = 1'000'000;
  remuxFile(input, output, settings);
  if (!testing::Test::HasFatalFailure()) {
    const auto data = readFile(output);
    const auto topLevel = parseBoxes(data, 0, data.size());
    const Box* moov = findBox(topLevel, "moov");
    ASSERT_NE(nullptr, moov);
    const auto moovChildren = childBoxes(data, *moov);
    const Box* mvhd = findBox(moovChildren, "mvhd");
    ASSERT_NE(nullptr, mvhd);
    const uint32_t movieDuration = readU32(data, static_cast<size_t>(mvhd->payloadOffset) + 16);
    EXPECT_NEAR(6'040u, movieDuration, 100u);

    const Box* trak = findBox(moovChildren, "trak");
    ASSERT_NE(nullptr, trak);
    const auto trakChildren = childBoxes(data, *trak);
    const Box* edts = findBox(trakChildren, "edts");
    ASSERT_NE(nullptr, edts);
    const auto edtsChildren = childBoxes(data, *edts);
    const Box* elst = findBox(edtsChildren, "elst");
    ASSERT_NE(nullptr, elst);
    EXPECT_EQ(2u, readU32(data, static_cast<size_t>(elst->payloadOffset) + 4));
    const uint64_t emptyEditDuration = readU64(data, static_cast<size_t>(elst->payloadOffset) + 8);
    EXPECT_NEAR(1000u, emptyEditDuration, 2u);

    checkRemuxedFile(output, 6'040'000);
  }
  if (std::getenv("CORAL_KEEP_MUXER_OUTPUT") == nullptr) {
    std::filesystem::remove(output);
  }
}

TEST(Mp4MuxerTest, RejectsDerivedCaptionTracks) {
  const std::vector<std::pair<Container, const char*>> containers = {
    { Container::kMp4, "coral-muxer-derived.mp4" },
    { Container::kFragmentedMp4, "coral-muxer-derived-fragmented.mp4" },
  };
  for (const auto& [container, name]: containers) {
    SCOPED_TRACE(name);
    const auto output = std::filesystem::current_path() / name;
    std::filesystem::remove(output);
    auto factory = createMuxerFactory();
    ASSERT_NE(nullptr, factory);
    Error error;
    auto muxer = factory->create(output.string(), container, MuxerOptions(), &error);
    ASSERT_NE(nullptr, muxer) << error.message;

    // The captions of a track derived from the SEI of a video track are already carried by the
    // video samples, so the muxer refuses it instead of writing the captions twice.
    auto derived = MediaFormat::CreateSubtitleFormat("application/cea-708");
    derived->setDerivedTrack(true);
    EXPECT_LT(muxer->addTrack(derived), 0);
    EXPECT_NE(std::string::npos, muxer->getError().message.find("derived"));
    muxer->cancel();
    std::filesystem::remove(output);
  }
}

TEST(Mp4MuxerTest, WritesCea708CaptionTrack) {
  const std::vector<std::vector<uint8_t>> samples = {
    { 0xFC, 0x94, 0x2E },
    { 0xFC, 0x94, 0x2F },
    { 0xFC, 0x94, 0x20 },
  };
  const std::vector<std::pair<Container, const char*>> containers = {
    { Container::kMp4, "coral-muxer-caption.mp4" },
    { Container::kFragmentedMp4, "coral-muxer-caption-fragmented.mp4" },
  };
  for (const auto& [container, name]: containers) {
    SCOPED_TRACE(name);
    const auto output = std::filesystem::current_path() / name;
    std::filesystem::remove(output);
    auto factory = createMuxerFactory();
    ASSERT_NE(nullptr, factory);
    Error error;
    auto muxer = factory->create(output.string(), container, MuxerOptions(), &error);
    ASSERT_NE(nullptr, muxer) << error.message;

    auto format = MediaFormat::CreateSubtitleFormat("application/cea-708");
    format->setLanguage("eng");
    format->setCaptionServiceNumber(2);
    const int track = muxer->addTrack(format);
    ASSERT_GE(track, 0) << muxer->getError().message;

    int64_t timeUs = 0;
    for (const auto& sample: samples) {
      SampleInfo info;
      info.presentationTimeUs = timeUs;
      info.durationUs = 33'333;
      info.flags = static_cast<uint32_t>(SampleFlag::kKeyFrame);
      ASSERT_TRUE(muxer->writeSample(track, sample.data(), sample.size(), info)) << muxer->getError().message;
      timeUs += 33'333;
    }
    ASSERT_TRUE(muxer->finish()) << muxer->getError().message;

    // The caption samples are wrapped in a `cdat` box, and the `c708` sample entry states the
    // service and the language of the track.
    const auto data = readFile(output);
    const std::string text(data.begin(), data.end());
    EXPECT_NE(std::string::npos, text.find("c708"));
    EXPECT_NE(std::string::npos, text.find("clcp"));
    EXPECT_NE(std::string::npos, text.find("cdat"));

    std::vector<int> trackIds;
    std::vector<std::shared_ptr<MediaFormat>> formats;
    ASSERT_TRUE(readTrackInfo(output.string(), &trackIds, &formats));
    ASSERT_FALSE(trackIds.empty());
    std::shared_ptr<MediaFormat> captionFormat;
    for (const auto& format: formats) {
      if (format != nullptr && format->isSubtitle()) {
        captionFormat = format;
        break;
      }
    }
    ASSERT_NE(nullptr, captionFormat);
    EXPECT_EQ("application/cea-708", captionFormat->getMimeType());

    // The extractor strips the envelope, so the payload of the last sample is read back unchanged.
    {
      auto dataSource = std::make_shared<AVFileDataSource>();
      ASSERT_TRUE(dataSource->open(output.string()));
      AVReader reader(dataSource, AVExtractorsFactory::createDefault(-1, dataSource->getLength()));
      ASSERT_TRUE(reader.prepare()) << reader.getLastError();
      int captionTrackId = -1;
      for (const int id: reader.getTrackIds()) {
        const auto format = reader.getTrackFormat(id);
        if (format != nullptr && format->isSubtitle()) {
          captionTrackId = id;
          break;
        }
      }
      ASSERT_GE(captionTrackId, 0);
      std::vector<uint8_t> readBack;
      reader.setTrackConsumer(captionTrackId, [&readBack](const AVSample& sample) {
        if (!sample.isEndOfStream() && !sample.isCodecConfig() && sample.size() > 0) {
          readBack.assign(sample.array(), sample.array() + sample.size());
        }
      });
      while (reader.read() == AVReader::kResultContinue) {
      }
      reader.endTracks();
      EXPECT_EQ(samples.back(), readBack);
    }
    if (std::getenv("CORAL_KEEP_MUXER_OUTPUT") == nullptr) {
      std::filesystem::remove(output);
    }
  }
}

TEST(Mp4MuxerTest, WritesTextSubtitleTracks) {
  struct TextCase {
    const char* mime;
    const char* expectedMime;
    const char* fourcc;
    std::vector<uint8_t> sample;
  };
  const std::string document = "<?xml version=\"1.0\"?><tt><body><div><p begin=\"0s\">Hi</p></div></body></tt>";
  const std::vector<TextCase> cases = {
    { "application/x-quicktime-tx3g", "application/x-quicktime-tx3g", "tx3g",
      { 0x00, 0x05, 'H', 'e', 'l', 'l', 'o' } },
    { "application/x-mp4-vtt", "application/x-mp4-vtt", "wvtt",
      { 0x00, 0x00, 0x00, 0x10, 'v', 't', 't', 'c', 0x00, 0x00, 0x00, 0x08, 'p', 'a', 'y', 'l' } },
    { "application/ttml+xml", "application/ttml+xml", "stpp",
      std::vector<uint8_t>(document.begin(), document.end()) },
  };
  for (const auto& textCase: cases) {
    SCOPED_TRACE(textCase.mime);
    const auto output = std::filesystem::current_path() /
      (std::string("coral-muxer-text-") + textCase.fourcc + ".mp4");
    std::filesystem::remove(output);
    auto factory = createMuxerFactory();
    ASSERT_NE(nullptr, factory);
    Error error;
    auto muxer = factory->create(output.string(), Container::kMp4, MuxerOptions(), &error);
    ASSERT_NE(nullptr, muxer) << error.message;

    auto format = MediaFormat::CreateSubtitleFormat(textCase.mime);
    const int track = muxer->addTrack(format);
    ASSERT_GE(track, 0) << muxer->getError().message;
    SampleInfo info;
    info.presentationTimeUs = 0;
    info.durationUs = 1'000'000;
    info.flags = static_cast<uint32_t>(SampleFlag::kKeyFrame);
    ASSERT_TRUE(muxer->writeSample(track, textCase.sample.data(), textCase.sample.size(), info))
            << muxer->getError().message;
    ASSERT_TRUE(muxer->finish()) << muxer->getError().message;

    const auto data = readFile(output);
    EXPECT_NE(std::string::npos, std::string(data.begin(), data.end()).find(textCase.fourcc));

    std::vector<int> trackIds;
    std::vector<std::shared_ptr<MediaFormat>> formats;
    ASSERT_TRUE(readTrackInfo(output.string(), &trackIds, &formats));
    std::shared_ptr<MediaFormat> subtitleFormat;
    for (const auto& readFormat: formats) {
      if (readFormat != nullptr && readFormat->isSubtitle()) {
        subtitleFormat = readFormat;
        break;
      }
    }
    ASSERT_NE(nullptr, subtitleFormat);
    EXPECT_EQ(textCase.expectedMime, subtitleFormat->getMimeType());
    if (std::getenv("CORAL_KEEP_MUXER_OUTPUT") == nullptr) {
      std::filesystem::remove(output);
    }
  }
}

TEST(Mp4MuxerTest, RemuxesTheTx3gTrackOfARealMp4) {
  const auto input = assetPath("subtitle/mov_text.mp4");
  if (!std::filesystem::exists(input)) {
    GTEST_SKIP() << "fixture is not present";
  }
  const auto output = std::filesystem::current_path() / "coral-muxer-tx3g.mp4";
  remuxFile(input, output);
  if (!testing::Test::HasFatalFailure()) {
    std::vector<int> trackIds;
    std::vector<std::shared_ptr<MediaFormat>> formats;
    ASSERT_TRUE(readTrackInfo(output.string(), &trackIds, &formats));
    bool hasTx3g = false;
    for (const auto& format: formats) {
      // The MP4 extractor also derives a caption track from the H.264 SEI of the video; the remux
      // skips it, so only the native tx3g track is written.
      if (format != nullptr && format->isSubtitle() && !format->isDerivedTrack()) {
        hasTx3g = hasTx3g || format->getMimeType() == "application/x-quicktime-tx3g";
      }
    }
    EXPECT_TRUE(hasTx3g);
  }
  if (std::getenv("CORAL_KEEP_MUXER_OUTPUT") == nullptr) {
    std::filesystem::remove(output);
  }
}

TEST(Mp4MuxerTest, SkipsASubtitleTheMp4CannotHold) {
  const auto input = assetPath("subtitle/ass.mkv");
  if (!std::filesystem::exists(input)) {
    GTEST_SKIP() << "fixture is not present";
  }
  const auto output = std::filesystem::current_path() / "coral-muxer-ssa-skipped.mp4";
  remuxFile(input, output);
  if (!testing::Test::HasFatalFailure()) {
    // The MP4 muxer cannot represent an SSA track: the remux keeps the video and audio tracks and
    // skips the subtitle instead of failing the file.
    checkRemuxedFile(output, 5'040'000);
    std::vector<int> trackIds;
    std::vector<std::shared_ptr<MediaFormat>> formats;
    ASSERT_TRUE(readTrackInfo(output.string(), &trackIds, &formats));
    for (const auto& format: formats) {
      ASSERT_NE(nullptr, format);
      EXPECT_NE("text/x-ssa", format->getMimeType());
    }
  }
  if (std::getenv("CORAL_KEEP_MUXER_OUTPUT") == nullptr) {
    std::filesystem::remove(output);
  }
}

TEST(Mp4MuxerTest, WritesFlacSampleEntryAndSampleTable) {
  const auto input = assetPath("music/music.flac");
  if (!std::filesystem::exists(input)) {
    GTEST_SKIP() << "fixture is not present";
  }
  // The native FLAC extractor does not bound a frame that is larger than its 64 KiB read window, so
  // remuxing the whole fixture stops after one packet. Use its STREAMINFO format as the input of a
  // synthetic FLAC track instead; this exercises the `fLaC` sample entry and `dfLa` box.
  std::shared_ptr<MediaFormat> format;
  ASSERT_TRUE(readFirstFormat(input, &format));
  ASSERT_NE(nullptr, format);
  ASSERT_EQ("audio/flac", format->getMimeType());
  ASSERT_FALSE(format->csdList().empty());

  const auto output = std::filesystem::current_path() / "coral-muxer-flac.mp4";
  writeSyntheticTrack(output, format, 180, 23220, false);
  if (!testing::Test::HasFatalFailure()) {
    std::vector<int> trackIds;
    std::vector<std::shared_ptr<MediaFormat>> formats;
    int64_t durationUs = 0;
    ASSERT_TRUE(readTrackInfo(output.string(), &trackIds, &formats, &durationUs));
    ASSERT_EQ(1u, trackIds.size());
    ASSERT_EQ(1u, formats.size());
    ASSERT_NE(nullptr, formats.front());
    EXPECT_EQ("audio/flac", formats.front()->getMimeType());
    EXPECT_NEAR(4'179'600, static_cast<double>(durationUs), 100'000);
  }
  if (std::getenv("CORAL_KEEP_MUXER_OUTPUT") == nullptr) {
    std::filesystem::remove(output);
  }
}

TEST(Mp4MuxerTest, WritesOpusAc3Eac3Av1Vp9AndMpeg4SampleEntries) {
  struct CodecCase {
    const char* name;
    std::shared_ptr<MediaFormat> format;
    bool video;
    const char* expectedMime;
  };

  std::vector<CodecCase> cases;
  {
    auto format = MediaFormat::CreateAudioFormat("audio/opus", 48000, 2);
    const std::vector<uint8_t> opusHead = { 'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 2, 0x01, 0x38, 0x00, 0x00, 0xBB,
      0x80, 0, 0, 0 };
    format->setCodecSpecificData(0, opusHead);
    cases.push_back({ "opus", format, false, "audio/opus" });
  }
  cases.push_back({ "ac3", MediaFormat::CreateAudioFormat("audio/ac3", 48000, 6), false, "audio/ac3" });
  cases.push_back({ "eac3", MediaFormat::CreateAudioFormat("audio/eac3", 48000, 6), false, "audio/eac3" });
  {
    auto format = MediaFormat::CreateVideoFormat("video/av01", 320, 240);
    format->setCodecSpecificData(0, std::vector<uint8_t>{ 0x81, 0x00, 0x0C, 0x00 });
    cases.push_back({ "av1", format, true, "video/av01" });
  }
  {
    auto format = MediaFormat::CreateVideoFormat("video/x-vnd.on2.vp9", 320, 240);
    format->setCodecSpecificData(0, std::vector<uint8_t>{ 1, 0, 0, 0 });
    cases.push_back({ "vp9", format, true, "video/x-vnd.on2.vp9" });
  }
  {
    auto format = MediaFormat::CreateVideoFormat("video/mp4v-es", 320, 240);
    format->setCodecSpecificData(0, std::vector<uint8_t>{ 0x00, 0x00, 0x01, 0xB0 });
    cases.push_back({ "mpeg4", format, true, "video/mp4v-es" });
  }
  {
    auto format = MediaFormat::CreateVideoFormat("video/3gpp", 320, 240);
    format->setCodecs("s263.0.10");
    cases.push_back({ "h263", format, true, "video/3gpp" });
  }
  {
    auto format = MediaFormat::CreateAudioFormat("audio/raw", 48000, 2);
    cases.push_back({ "raw", format, false, "audio/raw" });
  }

  for (const auto& test: cases) {
    ASSERT_NE(nullptr, test.format) << test.name;
    const auto output = std::filesystem::current_path() / (std::string("coral-muxer-codec-") + test.name + ".mp4");
    writeSyntheticTrack(output, test.format, 4, test.video ? 40000 : 21333, test.video);
    if (!testing::Test::HasFatalFailure()) {
      EXPECT_EQ(test.expectedMime, mimeOfFirstTrack(output)) << test.name;
    }
    if (std::getenv("CORAL_KEEP_MUXER_OUTPUT") == nullptr) {
      std::filesystem::remove(output);
    }
  }
}

TEST(Mp4MuxerTest, PreservesColorInfoAndWritesColr) {
  const auto input = assetPath("video/IPhone12_4k_HLG.mp4");
  if (!std::filesystem::exists(input)) {
    GTEST_SKIP() << "fixture is not present";
  }
  std::shared_ptr<MediaFormat> sourceFormat;
  ASSERT_TRUE(readVideoFormat(input, &sourceFormat));
  ASSERT_NE(nullptr, sourceFormat);
  EXPECT_EQ(ColorInfo::TransferType::kTransferHLG, sourceFormat->getColorInfo().colorTransfer);
  EXPECT_EQ(ColorInfo::SpaceType::kSpaceBT2020, sourceFormat->getColorInfo().colorSpace);

  const auto output = std::filesystem::current_path() / "coral-muxer-colr.mp4";
  RemuxSettings settings;
  settings.maxDurationUs = 1'000'000;
  remuxFile(input, output, settings);
  if (!testing::Test::HasFatalFailure()) {
    const auto data = readFile(output);
    const auto topLevel = parseBoxes(data, 0, data.size());
    const Box* moov = findBox(topLevel, "moov");
    ASSERT_NE(nullptr, moov);
    const auto moovChildren = childBoxes(data, *moov);
    const auto trak = findBox(moovChildren, "trak");
    ASSERT_NE(nullptr, trak);
    const auto trakChildren = childBoxes(data, *trak);
    const auto mdia = findBox(trakChildren, "mdia");
    ASSERT_NE(nullptr, mdia);
    const auto mdiaChildren = childBoxes(data, *mdia);
    const auto minf = findBox(mdiaChildren, "minf");
    ASSERT_NE(nullptr, minf);
    const auto minfChildren = childBoxes(data, *minf);
    const auto stbl = findBox(minfChildren, "stbl");
    ASSERT_NE(nullptr, stbl);
    const auto stblChildren = childBoxes(data, *stbl);
    const auto stsd = findBox(stblChildren, "stsd");
    ASSERT_NE(nullptr, stsd);
    const auto entryChildren = sampleEntryChildren(data, *stsd);
    const Box* colr = findBox(entryChildren, "colr");
    ASSERT_NE(nullptr, colr);
    ASSERT_GE(colr->payloadSize, 11);
    EXPECT_EQ(typeFor("nclx"), readU32(data, static_cast<size_t>(colr->payloadOffset)));
    EXPECT_EQ(9u, readU32(data, static_cast<size_t>(colr->payloadOffset) + 4) >> 16);
    EXPECT_EQ(18u, readU32(data, static_cast<size_t>(colr->payloadOffset) + 6) >> 16);

    std::vector<int> trackIds;
    std::vector<std::shared_ptr<MediaFormat>> formats;
    ASSERT_TRUE(readTrackInfo(output.string(), &trackIds, &formats));
    for (const auto& format: formats) {
      if (format != nullptr && format->isVideo()) {
        EXPECT_EQ(ColorInfo::TransferType::kTransferHLG, format->getColorInfo().colorTransfer);
        EXPECT_EQ(ColorInfo::SpaceType::kSpaceBT2020, format->getColorInfo().colorSpace);
      }
    }
  }
  if (std::getenv("CORAL_KEEP_MUXER_OUTPUT") == nullptr) {
    std::filesystem::remove(output);
  }
}

namespace {

std::vector<uint8_t> makeHdrStaticInfo() {
  std::vector<uint8_t> info(25, 0);
  const auto put = [&info](const size_t offset, const uint16_t value) {
    info[offset] = static_cast<uint8_t>(value);
    info[offset + 1] = static_cast<uint8_t>(value >> 8);
  };
  const uint16_t primaries[8] = { 34000, 16000, 13250, 34500, 7500, 3000, 15635, 16450 };
  for (size_t i = 0; i < 8; ++i) {
    put(1 + i * 2, primaries[i]);
  }
  put(17, 1000); // max display luminance, in units of 10000 nits
  put(19, 1);    // min display luminance
  put(21, 1000); // max content light level
  put(23, 400);  // max frame-average light level
  return info;
}

} // namespace

TEST(Mp4MuxerTest, WritesMdcvAndClliFromHdrStaticInfo) {
  const auto input = assetPath("video/hdr10-video-with-sdr-container.mp4");
  if (!std::filesystem::exists(input)) {
    GTEST_SKIP() << "fixture is not present";
  }
  std::shared_ptr<MediaFormat> format;
  ASSERT_TRUE(readVideoFormat(input, &format));
  ASSERT_NE(nullptr, format);
  const auto hdrStaticInfo = makeHdrStaticInfo();
  format->setColorInfo(ColorInfo(ColorInfo::SpaceType::kSpaceBT2020,
    ColorInfo::RangeType::kRangeLimited, ColorInfo::TransferType::kTransferST2084,
    hdrStaticInfo));

  const auto output = std::filesystem::current_path() / "coral-muxer-mdcv.mp4";
  writeSyntheticTrack(output, format, 4, 40000, true);
  if (!testing::Test::HasFatalFailure()) {
    const auto data = readFile(output);
    const auto topLevel = parseBoxes(data, 0, data.size());
    const Box* moov = findBox(topLevel, "moov");
    ASSERT_NE(nullptr, moov);
    const auto moovChildren = childBoxes(data, *moov);
    const auto trak = findBox(moovChildren, "trak");
    ASSERT_NE(nullptr, trak);
    const auto trakChildren = childBoxes(data, *trak);
    const auto mdia = findBox(trakChildren, "mdia");
    ASSERT_NE(nullptr, mdia);
    const auto mdiaChildren = childBoxes(data, *mdia);
    const auto minf = findBox(mdiaChildren, "minf");
    ASSERT_NE(nullptr, minf);
    const auto minfChildren = childBoxes(data, *minf);
    const auto stbl = findBox(minfChildren, "stbl");
    ASSERT_NE(nullptr, stbl);
    const auto stblChildren = childBoxes(data, *stbl);
    const auto stsd = findBox(stblChildren, "stsd");
    ASSERT_NE(nullptr, stsd);
    const auto entryChildren = sampleEntryChildren(data, *stsd);
    const Box* mdcv = findBox(entryChildren, "mdcv");
    const Box* clli = findBox(entryChildren, "clli");
    ASSERT_NE(nullptr, mdcv);
    ASSERT_NE(nullptr, clli);
    EXPECT_EQ(24, mdcv->payloadSize);
    EXPECT_EQ(4, clli->payloadSize);
    EXPECT_EQ(34000u, readU32(data, static_cast<size_t>(mdcv->payloadOffset)) >> 16);
    EXPECT_EQ(1000u, readU32(data, static_cast<size_t>(clli->payloadOffset)) >> 16);
    EXPECT_EQ(400u, readU32(data, static_cast<size_t>(clli->payloadOffset) + 2) >> 16);

    std::vector<int> trackIds;
    std::vector<std::shared_ptr<MediaFormat>> formats;
    ASSERT_TRUE(readTrackInfo(output.string(), &trackIds, &formats));
    ASSERT_FALSE(formats.empty());
    ASSERT_NE(nullptr, formats.front());
    const auto& readColor = formats.front()->getColorInfo();
    ASSERT_EQ(25u, readColor.hdrStaticInfo.size());
    EXPECT_EQ(hdrStaticInfo, readColor.hdrStaticInfo);
  }
  if (std::getenv("CORAL_KEEP_MUXER_OUTPUT") == nullptr) {
    std::filesystem::remove(output);
  }
}

TEST(Mp4MuxerTest, PreservesDolbyVisionConfig) {
  const auto input = assetPath("video/DolbyVision.mp4");
  if (!std::filesystem::exists(input)) {
    GTEST_SKIP() << "fixture is not present";
  }
  std::shared_ptr<MediaFormat> format;
  ASSERT_TRUE(readVideoFormat(input, &format));
  ASSERT_NE(nullptr, format);
  EXPECT_EQ("video/dolby-vision", format->getMimeType());
  EXPECT_EQ(0u, format->getCodecs().find("dvhe.8"));

  const auto output = std::filesystem::current_path() / "coral-muxer-dovi.mp4";
  writeSyntheticTrack(output, format, 4, 40000, true);
  if (!testing::Test::HasFatalFailure()) {
    const auto data = readFile(output);
    const auto topLevel = parseBoxes(data, 0, data.size());
    const Box* moov = findBox(topLevel, "moov");
    ASSERT_NE(nullptr, moov);
    const auto moovChildren = childBoxes(data, *moov);
    const auto trak = findBox(moovChildren, "trak");
    ASSERT_NE(nullptr, trak);
    const auto trakChildren = childBoxes(data, *trak);
    const auto mdia = findBox(trakChildren, "mdia");
    ASSERT_NE(nullptr, mdia);
    const auto mdiaChildren = childBoxes(data, *mdia);
    const auto minf = findBox(mdiaChildren, "minf");
    ASSERT_NE(nullptr, minf);
    const auto minfChildren = childBoxes(data, *minf);
    const auto stbl = findBox(minfChildren, "stbl");
    ASSERT_NE(nullptr, stbl);
    const auto stblChildren = childBoxes(data, *stbl);
    const auto stsd = findBox(stblChildren, "stsd");
    ASSERT_NE(nullptr, stsd);
    const auto entryChildren = sampleEntryChildren(data, *stsd);
    const Box* dvvC = findBox(entryChildren, "dvvC");
    ASSERT_NE(nullptr, dvvC);
    ASSERT_GE(dvvC->payloadSize, 5);
    EXPECT_EQ(8u, (readU32(data, static_cast<size_t>(dvvC->payloadOffset) + 2) >> 24) >> 1);

    EXPECT_EQ("video/dolby-vision", mimeOfFirstTrack(output));
  }
  if (std::getenv("CORAL_KEEP_MUXER_OUTPUT") == nullptr) {
    std::filesystem::remove(output);
  }
}

TEST(Mp4MuxerTest, AuxiliaryTrackReferencesThePrimaryVideoTrack) {
  const auto input = fixturePath("video720p_5s.mp4");
  if (!std::filesystem::exists(input)) {
    GTEST_SKIP() << "fixture is not present";
  }
  std::shared_ptr<MediaFormat> format;
  ASSERT_TRUE(readVideoFormat(input, &format));
  ASSERT_NE(nullptr, format);

  const auto output = std::filesystem::current_path() / "coral-muxer-auxiliary.mp4";
  std::filesystem::remove(output);
  auto factory = createMuxerFactory();
  ASSERT_NE(nullptr, factory);
  Error error;
  auto muxer = factory->create(output.string(), Container::kMp4, MuxerOptions(), &error);
  ASSERT_NE(nullptr, muxer) << error.message;
  const int primary = muxer->addTrack(format);
  TrackOptions auxiliaryOptions;
  auxiliaryOptions.auxiliary = true;
  const int auxiliary = muxer->addTrack(format, auxiliaryOptions);
  ASSERT_GE(primary, 0);
  ASSERT_GE(auxiliary, 0) << muxer->getError().message;
  const std::vector<uint8_t> payload(32, 0x5A);
  for (int i = 0; i < 2; ++i) {
    SampleInfo info;
    info.presentationTimeUs = static_cast<int64_t>(i) * 40000;
    info.flags = static_cast<uint32_t>(SampleFlag::kKeyFrame);
    ASSERT_TRUE(muxer->writeSample(primary, payload.data(), payload.size(), info));
    ASSERT_TRUE(muxer->writeSample(auxiliary, payload.data(), payload.size(), info));
  }
  ASSERT_TRUE(muxer->finish()) << muxer->getError().message;

  const auto data = readFile(output);
  const auto topLevel = parseBoxes(data, 0, data.size());
  const Box* moov = findBox(topLevel, "moov");
  ASSERT_NE(nullptr, moov);
  const auto moovChildren = childBoxes(data, *moov);
  std::vector<Box> traks;
  for (const auto& box: moovChildren) {
    if (box.type == typeFor("trak")) {
      traks.push_back(box);
    }
  }
  ASSERT_EQ(2u, traks.size());
  const auto auxChildren = childBoxes(data, traks[1]);
  const Box* tref = findBox(auxChildren, "tref");
  ASSERT_NE(nullptr, tref);
  const auto trefChildren = childBoxes(data, *tref);
  const Box* cdsc = findBox(trefChildren, "cdsc");
  ASSERT_NE(nullptr, cdsc);
  ASSERT_GE(cdsc->payloadSize, 4);
  EXPECT_EQ(static_cast<uint32_t>(primary),
    readU32(data, static_cast<size_t>(cdsc->payloadOffset)));
  if (std::getenv("CORAL_KEEP_MUXER_OUTPUT") == nullptr) {
    std::filesystem::remove(output);
  }
}

TEST(Mp4MuxerTest, WritesAuxiliaryTracksInAxte) {
  const auto input = fixturePath("video720p_5s.mp4");
  if (!std::filesystem::exists(input)) {
    GTEST_SKIP() << "fixture is not present";
  }
  std::shared_ptr<MediaFormat> format;
  ASSERT_TRUE(readVideoFormat(input, &format));
  ASSERT_NE(nullptr, format);

  const auto output = std::filesystem::current_path() / "coral-muxer-axte.mp4";
  std::filesystem::remove(output);
  auto factory = createMuxerFactory();
  ASSERT_NE(nullptr, factory);
  MuxerOptions options;
  options.auxiliaryTracksInAxte = true;
  Error error;
  auto muxer = factory->create(output.string(), Container::kMp4, options, &error);
  ASSERT_NE(nullptr, muxer) << error.message;
  const int primary = muxer->addTrack(format);
  TrackOptions auxiliaryOptions;
  auxiliaryOptions.auxiliary = true;
  auxiliaryOptions.auxiliaryTrackType = 1;
  const int auxiliary = muxer->addTrack(format, auxiliaryOptions);
  ASSERT_GE(primary, 0);
  ASSERT_GE(auxiliary, 0) << muxer->getError().message;
  const std::vector<uint8_t> payload(32, 0x5A);
  for (int i = 0; i < 2; ++i) {
    SampleInfo info;
    info.presentationTimeUs = static_cast<int64_t>(i) * 40000;
    info.flags = static_cast<uint32_t>(SampleFlag::kKeyFrame);
    ASSERT_TRUE(muxer->writeSample(primary, payload.data(), payload.size(), info));
    ASSERT_TRUE(muxer->writeSample(auxiliary, payload.data(), payload.size(), info));
  }
  ASSERT_TRUE(muxer->finish()) << muxer->getError().message;

  const auto data = readFile(output);
  const auto topLevel = parseBoxes(data, 0, data.size());
  const Box* moov = findBox(topLevel, "moov");
  const Box* axte = findBox(topLevel, "axte");
  ASSERT_NE(nullptr, moov);
  ASSERT_NE(nullptr, axte);
  const auto moovChildren = childBoxes(data, *moov);
  const Box* udta = findBox(moovChildren, "udta");
  ASSERT_NE(nullptr, udta);
  const auto values = readMdtaValues(data, *udta);
  const auto offsetIt = values.find("auxiliary.tracks.offset");
  const auto lengthIt = values.find("auxiliary.tracks.length");
  ASSERT_NE(values.end(), offsetIt);
  ASSERT_NE(values.end(), lengthIt);
  EXPECT_EQ(78u, offsetIt->second.type);
  EXPECT_EQ(78u, lengthIt->second.type);
  ASSERT_EQ(8u, offsetIt->second.value.size());
  ASSERT_EQ(8u, lengthIt->second.value.size());
  uint64_t offset = 0;
  uint64_t length = 0;
  uint64_t auxSize = 0;
  for (size_t i = 0; i < 8; ++i) {
    offset = (offset << 8) | offsetIt->second.value[i];
    length = (length << 8) | lengthIt->second.value[i];
    auxSize = (auxSize << 8) | data[static_cast<size_t>(axte->payloadOffset) - 8 + i];
  }
  EXPECT_EQ(static_cast<uint64_t>(axte->offset), offset);
  EXPECT_EQ(static_cast<uint64_t>(axte->size), length);
  EXPECT_EQ(static_cast<uint64_t>(axte->size), auxSize);

  const auto axteChildren = parseBoxes(data, static_cast<size_t>(axte->payloadOffset),
    static_cast<size_t>(axte->payloadOffset + axte->payloadSize));
  ASSERT_GE(axteChildren.size(), 3u);
  EXPECT_EQ("ftyp", typeName(axteChildren[0].type));
  EXPECT_EQ("mdat", typeName(axteChildren[1].type));
  EXPECT_EQ("moov", typeName(axteChildren[2].type));
  const auto axteMoov = parseBoxes(data, static_cast<size_t>(axteChildren[2].payloadOffset),
    static_cast<size_t>(axteChildren[2].payloadOffset + axteChildren[2].payloadSize));
  EXPECT_NE(nullptr, findBox(axteMoov, "trak"));
  if (std::getenv("CORAL_KEEP_MUXER_OUTPUT") == nullptr) {
    std::filesystem::remove(output);
  }
}

TEST(Mp4MuxerTest, FragmentedMp4WritesContinuousFragments) {
  const auto input = fixturePath("video720p_5s.mp4");
  if (!std::filesystem::exists(input)) {
    GTEST_SKIP() << "fixture is not present";
  }
  const auto output = std::filesystem::current_path() / "coral-muxer-fragmented.mp4";
  RemuxSettings settings;
  settings.container = Container::kFragmentedMp4;
  settings.options.fragmentDurationUs = 1'000'000;
  remuxFile(input, output, settings);
  if (!testing::Test::HasFatalFailure()) {
    const auto data = readFile(output);
    const auto topLevel = parseBoxes(data, 0, data.size());
    ASSERT_GE(topLevel.size(), 4u);
    EXPECT_EQ("ftyp", typeName(topLevel[0].type));
    EXPECT_EQ("moov", typeName(topLevel[1].type));

    const auto moovChildren = childBoxes(data, topLevel[1]);
    const Box* mvex = findBox(moovChildren, "mvex");
    ASSERT_NE(nullptr, mvex);
    EXPECT_NE(nullptr, findBox(childBoxes(data, *mvex), "trex"));

    int fragmentCount = 0;
    uint32_t lastSequence = 0;
    int firstVideoTfdt = -1;
    int secondVideoTfdt = -1;
    for (size_t i = 2; i < topLevel.size(); ++i) {
      if (topLevel[i].type == typeFor("moof")) {
        ++fragmentCount;
        const auto moofChildren = childBoxes(data, topLevel[i]);
        const Box* mfhd = findBox(moofChildren, "mfhd");
        ASSERT_NE(nullptr, mfhd);
        const uint32_t sequence = readU32(data, static_cast<size_t>(mfhd->payloadOffset) + 4);
        EXPECT_GT(sequence, lastSequence);
        lastSequence = sequence;
        for (const auto& traf: moofChildren) {
          if (traf.type != typeFor("traf")) {
            continue;
          }
          const auto trafChildren = childBoxes(data, traf);
          const Box* tfdt = findBox(trafChildren, "tfdt");
          ASSERT_NE(nullptr, tfdt);
          const int64_t baseTime = static_cast<int64_t>(readU64(data, static_cast<size_t>(tfdt->payloadOffset) + 4));
          if (firstVideoTfdt < 0) {
            firstVideoTfdt = static_cast<int>(baseTime);
          } else if (secondVideoTfdt < 0 && fragmentCount > 1) {
            secondVideoTfdt = static_cast<int>(baseTime);
          }
          // Only the first traf of every fragment tracks the video timeline of this test.
          break;
        }
      }
    }
    EXPECT_GE(fragmentCount, 2);
    EXPECT_EQ(0, firstVideoTfdt);
    EXPECT_GT(secondVideoTfdt, 0);
    EXPECT_NE(std::string::npos, std::string(data.begin(), data.end()).find("tfdt"));

    // Coral's MP4 extractor reports no movie duration for a fragmented file without a `mehd`
    // box, so validate the tracks and the last sample time instead.
    std::vector<int> trackIds;
    std::vector<std::shared_ptr<MediaFormat>> formats;
    ASSERT_TRUE(readTrackInfo(output.string(), &trackIds, &formats));
    EXPECT_EQ(2u, countPrimaryTracks(formats));
    std::vector<int64_t> videoTimes;
    ASSERT_TRUE(readSampleTimes(output.string(), 0, &videoTimes));
    ASSERT_FALSE(videoTimes.empty());
    EXPECT_NEAR(4'960'000, static_cast<double>(videoTimes.back()), 80'000);
  }
  if (std::getenv("CORAL_KEEP_MUXER_OUTPUT") == nullptr) {
    std::filesystem::remove(output);
  }
}

TEST(Mp4MuxerTest, CancelLeavesNoOutputFile) {
  const auto output = std::filesystem::current_path() / "coral-muxer-cancel.mp4";
  std::filesystem::remove(output);
  auto factory = createMuxerFactory();
  ASSERT_NE(nullptr, factory);
  Error error;
  auto muxer = factory->create(output.string(), Container::kMp4, MuxerOptions(), &error);
  ASSERT_NE(nullptr, muxer) << error.message;
  auto format = MediaFormat::CreateAudioFormat("audio/mp4a-latm", 48000, 2);
  const int track = muxer->addTrack(format);
  ASSERT_GE(track, 0) << muxer->getError().message;
  const std::vector<uint8_t> payload(16, 0);
  SampleInfo info;
  ASSERT_TRUE(muxer->writeSample(track, payload.data(), payload.size(), info));
  EXPECT_TRUE(std::filesystem::exists(output));
  muxer->cancel();
  EXPECT_EQ(Status::kCancelled, muxer->getStatus());
  EXPECT_FALSE(std::filesystem::exists(output));
}

TEST(Mp4MuxerTest, FragmentDurationOnPlainMp4SelectsFragmentedWriter) {
  auto factory = createMuxerFactory();
  ASSERT_NE(nullptr, factory);
  const auto output = std::filesystem::current_path() / "coral-muxer-fragment-option.mp4";
  std::filesystem::remove(output);
  MuxerOptions options;
  options.fragmentDurationUs = 1'000'000;
  Error error;
  auto muxer = factory->create(output.string(), Container::kMp4, options, &error);
  ASSERT_NE(nullptr, muxer) << error.message;
  EXPECT_EQ(Container::kFragmentedMp4, muxer->getContainer());

  auto format = MediaFormat::CreateAudioFormat("audio/mp4a-latm", 48000, 2);
  const int track = muxer->addTrack(format);
  ASSERT_GE(track, 0) << muxer->getError().message;
  const std::vector<uint8_t> payload(16, 0);
  SampleInfo info;
  ASSERT_TRUE(muxer->writeSample(track, payload.data(), payload.size(), info));
  ASSERT_TRUE(muxer->finish()) << muxer->getError().message;
  const auto data = readFile(output);
  const std::string text(data.begin(), data.end());
  EXPECT_NE(std::string::npos, text.find("moof"));
  if (std::getenv("CORAL_KEEP_MUXER_OUTPUT") == nullptr) {
    std::filesystem::remove(output);
  }
}
