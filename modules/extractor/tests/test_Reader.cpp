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

#include "TestMedia.h"

#include <extractor/DefaultExtractorsFactory.h>
#include <extractor/Reader.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using coral::AVByteArrayDataSource;
using coral::AVDefaultExtractorsFactory;
using coral::AVExtractorsFactory;
using coral::AVReader;
using coral::AVSample;
using coral::AVSeekMap;
using coral::AVTrackInfo;
using coral::Definitions;
using coral::FileTypes;
using coral::MediaType;
using coral::MimeTypes;

namespace {

/**
 * A description of one delivered sample, kept by the test consumers so that the assertions do not
 * depend on the lifetime of the buffer of the sample.
 */
struct DeliveredSample {
  int trackIndex = -1;
  int64_t timeUs = 0;
  uint32_t flags = 0;
  size_t size = 0;
  std::vector<uint8_t> payload;
};

/**
 * The harness of the reader tests: it creates a reader on an in memory media and collects every
 * sample that is delivered.
 */
class ReaderHarness final {
public:
  explicit ReaderHarness(std::vector<uint8_t> media, const int fileType = -1) {
    auto dataSource = std::make_shared<AVByteArrayDataSource>(std::move(media));
    dataSource->open("memory");
    reader_ = std::unique_ptr<AVReader>(
      new AVReader(dataSource, std::unique_ptr<AVExtractorsFactory>(
                                 new AVDefaultExtractorsFactory(fileType))));
  }

  ReaderHarness(const ReaderHarness& other) = delete;

  ReaderHarness& operator=(const ReaderHarness& other) = delete;

  ~ReaderHarness() = default;

  /**
   * Registers the consumer of a track, before {@link prepare} is called.
   */
  void registerConsumer(const int trackId) {
    reader_->setTrackConsumer(trackId, [this, trackId](const AVSample& sample) {
      std::cout << "[consumer] track=" << trackId << " eos=" << (sample.isEndOfStream() ? 1 : 0)
                << " size=" << sample.size() << std::endl;
      if (sample.isEndOfStream()) {
        endOfStream_[trackId] = true;
        return;
      }
      DeliveredSample delivered;
      delivered.trackIndex = sample.trackIndex;
      delivered.timeUs = sample.timeUs;
      delivered.flags = sample.flags;
      delivered.size = sample.size();
      if (sample.array() != nullptr && sample.size() > 0) {
        delivered.payload.assign(sample.array(), sample.array() + sample.size());
      }
      samples_[trackId].push_back(std::move(delivered));
    });
  }

  bool prepare() {
    const bool prepared = reader_->prepare();
    std::cout << "[prepare] prepared=" << prepared
              << " extractor=" << reader_->getSelectedExtractorName()
              << " tracks=" << reader_->getTrackIds().size()
              << " error=" << reader_->getLastError() << std::endl;
    return prepared;
  }

  int readToEnd() {
    int result = AVReader::kResultContinue;
    for (int i = 0; i < 1000000; ++i) {
      result = reader_->read();
      if (result != AVReader::kResultContinue) {
        break;
      }
    }
    // The extractors only signal the end of a track when they observe it, close them now.
    reader_->endTracks();
    return result;
  }

  [[nodiscard]] AVReader& reader() const { return *reader_; }

  [[nodiscard]] const std::vector<DeliveredSample>& samples(const int trackId) const {
    static const std::vector<DeliveredSample> sEmpty;
    const auto it = samples_.find(trackId);
    return it == samples_.end() ? sEmpty : it->second;
  }

  [[nodiscard]] bool isEndOfStream(const int trackId) const {
    const auto it = endOfStream_.find(trackId);
    return it != endOfStream_.end() && it->second;
  }

private:
  std::unique_ptr<AVReader> reader_;
  std::map<int, std::vector<DeliveredSample>> samples_;
  std::map<int, bool> endOfStream_;
};

/**
 * Returns a consumer that counts the samples of a track without keeping them.
 */
std::shared_ptr<int> makeCounter() { return std::make_shared<int>(0); }

} // namespace anonymous
// ==================================================================================================
// WAV
// ==================================================================================================

TEST(AVReaderTest, DiagnoseSelectedExtractor) {
  const int frameCount = 20;
  auto media = coral::test::buildMp3(frameCount);
  std::cout << "[diagnose] media size=" << media.size() << std::endl;
  ReaderHarness harness(std::move(media), FileTypes::MP3);
  harness.registerConsumer(0);
  const bool prepared = harness.prepare();
  std::cout << "[diagnose] prepared=" << prepared << " extractor="
            << harness.reader().getSelectedExtractorName() << std::endl;
  const int result = harness.readToEnd();
  std::cout << "[diagnose] result=" << result << " samples=" << harness.samples(0).size()
            << " error=" << harness.reader().getLastError() << std::endl;
  SUCCEED();
}

TEST(AVReaderTest, DetectsAndExtractsAPcmWavFile) {
  // 1 second of 16 bit stereo audio at 44100 Hz.
  auto media = coral::test::buildWav(44100, 2, 44100);
  const auto expectedSize = media.size();

  ReaderHarness harness(std::move(media), FileTypes::WAV);
  harness.registerConsumer(0);

  ASSERT_TRUE(harness.prepare());
  const auto& samples = harness.samples(0);
  EXPECT_TRUE(samples.empty());

  EXPECT_EQ(AVReader::kResultEndOfInput, harness.readToEnd());

  const auto& delivered = harness.samples(0);
  ASSERT_FALSE(delivered.empty());
  EXPECT_TRUE(harness.isEndOfStream(0));

  size_t totalBytes = 0;
  for (const auto& sample : delivered) {
    EXPECT_EQ(0, sample.trackIndex);
    EXPECT_TRUE(sample.flags & AVSample::kFlagKeyFrame);
    EXPECT_GT(sample.size, 0u);
    totalBytes += sample.size;
  }
  // The payload of the data chunk is 44100 frames of 4 bytes.
  EXPECT_EQ(44100u * 4u, totalBytes);
  EXPECT_GT(expectedSize, totalBytes);

  const auto format = harness.reader().getTrackFormat(0);
  ASSERT_NE(nullptr, format);
  EXPECT_EQ(std::string(MimeTypes::AUDIO_RAW), format->getMimeType());
  EXPECT_EQ(44100, format->getSampleRate());
  EXPECT_EQ(2, format->getChannelCount());
  EXPECT_EQ(coral::AudioSampleFormat::kInt16, format->getSampleFormat());
  EXPECT_NEAR(1000000, format->getDurationUs(), 1000);

  const auto seekMap = harness.reader().getSeekMap();
  ASSERT_NE(nullptr, seekMap);
  EXPECT_FALSE(seekMap->isSeekable());
}

TEST(AVReaderTest, SkipsTheUnknownChunksOfAWavFile) {
  auto media = coral::test::buildWav(8000, 1, 8000, true);
  ReaderHarness harness(std::move(media));
  harness.registerConsumer(0);

  ASSERT_TRUE(harness.prepare());
  EXPECT_EQ(AVReader::kResultEndOfInput, harness.readToEnd());

  const auto& delivered = harness.samples(0);
  ASSERT_FALSE(delivered.empty());
  size_t totalBytes = 0;
  for (const auto& sample : delivered) {
    totalBytes += sample.size;
  }
  // The LIST chunk is skipped, the whole data chunk is delivered.
  EXPECT_EQ(8000u * 2u, totalBytes);
}

TEST(AVReaderTest, ReportsTheTimestampOfEveryWavSample) {
  auto media = coral::test::buildWav(1000, 1, 1000);
  ReaderHarness harness(std::move(media));
  harness.registerConsumer(0);
  ASSERT_TRUE(harness.prepare());
  ASSERT_EQ(AVReader::kResultEndOfInput, harness.readToEnd());

  const auto& delivered = harness.samples(0);
  ASSERT_FALSE(delivered.empty());
  // The first sample starts at zero.
  EXPECT_EQ(0, delivered.front().timeUs);
  // Every sample is contiguous.
  for (size_t i = 1; i < delivered.size(); ++i) {
    EXPECT_GE(delivered[i].timeUs, delivered[i - 1].timeUs);
  }
}

// ==================================================================================================
// MP3
// ==================================================================================================

TEST(AVReaderTest, DetectsAndExtractsAnMp3Stream) {
  const int frameCount = 20;
  auto media = coral::test::buildMp3(frameCount);
  std::cout << "[mp3size] " << media.size() << std::endl;
  ReaderHarness harness(std::move(media), FileTypes::MP3);
  harness.registerConsumer(0);

  ASSERT_TRUE(harness.prepare());
  const int result = harness.readToEnd();
  std::cout << "[mp3count] result=" << result << " samples=" << harness.samples(0).size()
            << " position=" << harness.reader().getPosition()
            << " error=" << harness.reader().getLastError() << std::endl;
  EXPECT_EQ(AVReader::kResultEndOfInput, result);

  const auto& delivered = harness.samples(0);
  ASSERT_EQ(static_cast<size_t>(frameCount), delivered.size());
  // MPEG-1 layer 3 at 128 kbit/s and 44100 Hz: 144 * 128000 / 44100 = 417 bytes per frame.
  EXPECT_EQ(417u, delivered.front().size);
  EXPECT_EQ(0, delivered.front().timeUs);
  EXPECT_TRUE(delivered.front().flags & AVSample::kFlagFirstSample);
  // 1152 samples at 44100 Hz is about 26122 microseconds per frame, and the constant bitrate
  // timestamps are computed from the position of the frame, so a small difference is expected.
  EXPECT_NEAR(26122, delivered[1].timeUs, 100);

  const auto format = harness.reader().getTrackFormat(0);
  ASSERT_NE(nullptr, format);
  EXPECT_EQ(std::string(MimeTypes::AUDIO_MPEG), format->getMimeType());
  EXPECT_EQ(44100, format->getSampleRate());
  EXPECT_EQ(2, format->getChannelCount());
  EXPECT_EQ(128000, format->getAverageBitrate());
  EXPECT_GT(format->getDurationUs(), 0);
}

TEST(AVReaderTest, SkipsTheId3TagOfAnMp3Stream) {
  auto media = coral::test::buildMp3(5, true);
  ReaderHarness harness(std::move(media));
  harness.registerConsumer(0);

  ASSERT_TRUE(harness.prepare());
  EXPECT_EQ(AVReader::kResultEndOfInput, harness.readToEnd());

  const auto& delivered = harness.samples(0);
  // The tag is 64 bytes long and holds five bytes that are not a frame, the five frames that
  // follow it are the only samples of the stream.
  ASSERT_EQ(5u, delivered.size());
  EXPECT_EQ(417u, delivered.front().size);
  for (const auto& sample : delivered) {
    EXPECT_EQ(417u, sample.size);
  }
}

TEST(AVReaderTest, ExposesTheMp3Id3TagAsATimedMetadataTrack) {
  auto media = coral::test::buildMp3(5, true);
  ReaderHarness harness(std::move(media));
  harness.registerConsumer(0);
  harness.registerConsumer(1);

  ASSERT_TRUE(harness.prepare());
  EXPECT_EQ(AVReader::kResultEndOfInput, harness.readToEnd());

  const auto& metadata = harness.samples(1);
  ASSERT_EQ(1u, metadata.size());
  EXPECT_EQ(0, metadata.front().timeUs);
  ASSERT_GE(metadata.front().size, 3u);
  EXPECT_EQ('I', metadata.front().payload[0]);
  EXPECT_EQ('D', metadata.front().payload[1]);
  EXPECT_EQ('3', metadata.front().payload[2]);
  const auto format = harness.reader().getTrackFormat(1);
  ASSERT_NE(nullptr, format);
  EXPECT_EQ(MimeTypes::APPLICATION_ID3, format->getMimeType());
}

TEST(AVReaderTest, ExposesAnId3TagInTheMiddleOfAnMp3Stream) {
  auto media = coral::test::buildMp3(4, false);
  const auto tag = coral::test::buildId3TagWithTitle("Middle");
  media.insert(media.begin() + 417, tag.begin(), tag.end());
  ReaderHarness harness(std::move(media));
  harness.registerConsumer(0);
  harness.registerConsumer(1);

  ASSERT_TRUE(harness.prepare());
  EXPECT_EQ(AVReader::kResultEndOfInput, harness.readToEnd());
  const auto& metadata = harness.samples(1);
  ASSERT_EQ(1u, metadata.size());
  EXPECT_GT(metadata.front().timeUs, 0);
  EXPECT_EQ("ID3", std::string(metadata.front().payload.begin(),
                              metadata.front().payload.begin() + 3));
}

// ==================================================================================================
// ADTS
// ==================================================================================================

TEST(AVReaderTest, DetectsAndExtractsAnAdtsStream) {
  const int frameCount = 10;
  auto media = coral::test::buildAdts(frameCount);
  ReaderHarness harness(std::move(media), FileTypes::ADTS);
  harness.registerConsumer(0);

  ASSERT_TRUE(harness.prepare());
  EXPECT_EQ(AVReader::kResultEndOfInput, harness.readToEnd());

  const auto& delivered = harness.samples(0);
  ASSERT_EQ(static_cast<size_t>(frameCount), delivered.size());
  EXPECT_EQ(71u, delivered.front().size);
  EXPECT_EQ(0, delivered.front().timeUs);
  // 1024 samples at 44100 Hz is about 23219 microseconds per frame.
  EXPECT_NEAR(23219, delivered[1].timeUs, 1);

  const auto format = harness.reader().getTrackFormat(0);
  ASSERT_NE(nullptr, format);
  EXPECT_EQ(std::string(MimeTypes::AUDIO_AAC), format->getMimeType());
  EXPECT_EQ(44100, format->getSampleRate());
  EXPECT_EQ(2, format->getChannelCount());
  ASSERT_EQ(1u, format->csdList().size());
  // The AudioSpecificConfig of AAC LC at 44100 Hz with two channels.
  ASSERT_EQ(2u, format->csdList()[0].size());
  EXPECT_EQ(0x12, format->csdList()[0][0]);
  EXPECT_EQ(0x10, format->csdList()[0][1]);
}

TEST(AVReaderTest, ExposesTheAdtsId3TagAsATimedMetadataTrack) {
  auto media = coral::test::buildAdts(5, 4, 2, true);
  ReaderHarness harness(std::move(media), FileTypes::ADTS);
  harness.registerConsumer(0);
  harness.registerConsumer(1);

  ASSERT_TRUE(harness.prepare());
  EXPECT_EQ(AVReader::kResultEndOfInput, harness.readToEnd());

  const auto& metadata = harness.samples(1);
  ASSERT_EQ(1u, metadata.size());
  EXPECT_EQ(0, metadata.front().timeUs);
  ASSERT_GE(metadata.front().size, 3u);
  EXPECT_EQ('I', metadata.front().payload[0]);
  EXPECT_EQ('D', metadata.front().payload[1]);
  EXPECT_EQ('3', metadata.front().payload[2]);
  const auto format = harness.reader().getTrackFormat(1);
  ASSERT_NE(nullptr, format);
  EXPECT_EQ(MimeTypes::APPLICATION_ID3, format->getMimeType());
}

TEST(AVReaderTest, ExtractsAScte35TrackFromTransportStream) {
  const auto media = coral::test::buildTsWithScte35({0xFC, 0x30, 0x11, 0x00, 0x00, 0x00});
  ReaderHarness harness(std::move(media), FileTypes::TS);
  harness.registerConsumer(0x101);

  ASSERT_TRUE(harness.prepare());
  EXPECT_EQ(AVReader::kResultEndOfInput, harness.readToEnd());
  const auto& samples = harness.samples(0x101);
  ASSERT_EQ(1u, samples.size());
  EXPECT_EQ(0, samples.front().timeUs);
  EXPECT_EQ(0xFC, samples.front().payload[0]);
  const auto format = harness.reader().getTrackFormat(0x101);
  ASSERT_NE(nullptr, format);
  EXPECT_EQ(MimeTypes::APPLICATION_SCTE35, format->getMimeType());
}

// ==================================================================================================
// FLAC
// ==================================================================================================

TEST(AVReaderTest, ExposesAnId3TagInTheMiddleOfAnAdtsStream) {
  auto media = coral::test::buildAdts(4, 4, 2, false);
  const auto tag = coral::test::buildId3TagWithTitle("Middle");
  media.insert(media.begin() + 71, tag.begin(), tag.end());
  ReaderHarness harness(std::move(media), FileTypes::ADTS);
  harness.registerConsumer(0);
  harness.registerConsumer(1);

  ASSERT_TRUE(harness.prepare());
  EXPECT_EQ(AVReader::kResultEndOfInput, harness.readToEnd());
  const auto& metadata = harness.samples(1);
  ASSERT_EQ(1u, metadata.size());
  EXPECT_GT(metadata.front().timeUs, 0);
  EXPECT_EQ("ID3", std::string(metadata.front().payload.begin(),
                              metadata.front().payload.begin() + 3));
}

TEST(AVReaderTest, DetectsAndExtractsANativeFlacStream) {
  auto media = coral::test::buildFlac(44100, 2, 16, 44100);
  ReaderHarness harness(std::move(media), FileTypes::FLAC);
  harness.registerConsumer(0);

  ASSERT_TRUE(harness.prepare());
  // The format of a native FLAC stream is only known once the metadata blocks were read.
  EXPECT_EQ(nullptr, harness.reader().getTrackFormat(0));
  EXPECT_EQ(AVReader::kResultEndOfInput, harness.readToEnd());
  EXPECT_FALSE(harness.samples(0).empty());

  const auto format = harness.reader().getTrackFormat(0);
  ASSERT_NE(nullptr, format);
  EXPECT_EQ(std::string(MimeTypes::AUDIO_FLAC), format->getMimeType());
  EXPECT_EQ(44100, format->getSampleRate());
  EXPECT_EQ(2, format->getChannelCount());
  ASSERT_EQ(1u, format->csdList().size());
  EXPECT_GE(format->csdList()[0].size(), 34u);

  const auto seekMap = harness.reader().getSeekMap();
  ASSERT_NE(nullptr, seekMap);
  EXPECT_NEAR(1000000, seekMap->getDurationUs(), 1000);
}

// ==================================================================================================
// MP4
// ==================================================================================================

TEST(AVReaderTest, DetectsAndExtractsAnMp4File) {
  const int sampleCount = 12;
  auto media = coral::test::buildMp4(sampleCount);
  ReaderHarness harness(std::move(media), FileTypes::MP4);
  harness.registerConsumer(0);

  ASSERT_TRUE(harness.prepare());
  const int result = harness.readToEnd();
  std::cout << "[mp4] result=" << result << " samples=" << harness.samples(0).size()
            << " error=" << harness.reader().getLastError() << std::endl;
  EXPECT_EQ(AVReader::kResultEndOfInput, result);

  const auto& delivered = harness.samples(0);
  ASSERT_EQ(static_cast<size_t>(sampleCount), delivered.size());
  for (const auto& sample : delivered) {
    EXPECT_EQ(1024u, sample.size);
    EXPECT_TRUE(sample.flags & AVSample::kFlagKeyFrame);
  }
  EXPECT_EQ(0, delivered.front().timeUs);
  // The media timescale is 1000, so every sample lasts one millisecond.
  EXPECT_EQ(1000, delivered[1].timeUs);

  const auto format = harness.reader().getTrackFormat(0);
  ASSERT_NE(nullptr, format);
  EXPECT_TRUE(format->isAudio());
  EXPECT_EQ(std::string(MimeTypes::AUDIO_AAC), format->getMimeType());
  EXPECT_EQ(1, format->getChannelCount());
  EXPECT_EQ(1, format->getTrackId());
  EXPECT_EQ("mp4a.40.2", format->getCodecs());
  ASSERT_FALSE(format->csdList().empty());
  ASSERT_EQ(2u, format->csdList()[0].size());
  EXPECT_EQ(0x12, format->csdList()[0][0]);
  EXPECT_EQ(0x08, format->csdList()[0][1]);

  const auto seekMap = harness.reader().getSeekMap();
  ASSERT_NE(nullptr, seekMap);
  EXPECT_NEAR(static_cast<double>(sampleCount) * 1000.0, static_cast<double>(seekMap->getDurationUs()),
              2000.0);
}

// ==================================================================================================
// OGG
// ==================================================================================================

TEST(AVReaderTest, DetectsAndExtractsAnOggVorbisStream) {
  const int packetCount = 10;
  auto media = coral::test::buildOggVorbis(44100, 2, packetCount);
  ReaderHarness harness(std::move(media));
  harness.registerConsumer(0);

  ASSERT_TRUE(harness.prepare());
  const auto format = harness.reader().getTrackFormat(0);
  ASSERT_NE(nullptr, format);
  EXPECT_TRUE(format->isAudio());
  EXPECT_EQ(std::string(MimeTypes::AUDIO_VORBIS), format->getMimeType());
  EXPECT_EQ(std::string(MimeTypes::AUDIO_OGG), format->getContainerMimeType());
  EXPECT_EQ(44100, format->getSampleRate());
  EXPECT_EQ(2, format->getChannelCount());
  EXPECT_EQ(112000, format->getAverageBitrate());
  // The decoder reads the identification header and the setup header of the stream.
  ASSERT_EQ(2u, format->csdList().size());
  EXPECT_EQ(30u, format->csdList()[0].size());
  EXPECT_GT(format->csdList()[1].size(), 0u);

  EXPECT_EQ(AVReader::kResultEndOfInput, harness.readToEnd());
  const auto& delivered = harness.samples(0);
  ASSERT_EQ(static_cast<size_t>(packetCount), delivered.size());
  // The number of samples of a packet is appended to the payload of a Vorbis sample.
  EXPECT_EQ(36u, delivered.front().size);
  EXPECT_TRUE(delivered.front().flags & AVSample::kFlagFirstSample);
  EXPECT_TRUE(harness.isEndOfStream(0));

  // The first packet of a Vorbis stream contributes no sample, every packet that follows contributes
  // the samples that its block shares with the previous one, i.e. (256 + 256) / 4 = 128.
  constexpr int64_t kSamplesPerPacket = 128;
  EXPECT_EQ(0, delivered[0].timeUs);
  EXPECT_EQ(0, delivered[1].timeUs);
  for (size_t i = 2; i < delivered.size(); ++i) {
    const int64_t expected =
      static_cast<int64_t>(i - 1) * kSamplesPerPacket * 1000000 / 44100;
    EXPECT_NEAR(static_cast<double>(expected), static_cast<double>(delivered[i].timeUs), 1.0);
  }

  const auto seekMap = harness.reader().getSeekMap();
  ASSERT_NE(nullptr, seekMap);
  EXPECT_TRUE(seekMap->isSeekable());
  // The duration of the stream is the granule position of the page that ends it.
  const int64_t expectedDuration =
    static_cast<int64_t>(packetCount - 1) * kSamplesPerPacket * 1000000 / 44100;
  EXPECT_NEAR(static_cast<double>(expectedDuration),
              static_cast<double>(seekMap->getDurationUs()), 2.0);
}

TEST(AVReaderTest, UsesTheLargeBlockSizeOfTheSecondVorbisMode) {
  const int packetCount = 6;
  auto media = coral::test::buildOggVorbis(44100, 2, packetCount, 32, true);
  ReaderHarness harness(std::move(media));
  harness.registerConsumer(0);

  ASSERT_TRUE(harness.prepare());
  ASSERT_EQ(AVReader::kResultEndOfInput, harness.readToEnd());
  const auto& delivered = harness.samples(0);
  ASSERT_EQ(static_cast<size_t>(packetCount), delivered.size());

  // The fixture declares a small block size of 256 samples and a large one of 2048, and every
  // second pair of packets uses the large one: the granules of the stream are 0, 0, 128, 704, 1728
  // and 2304 samples.
  const int64_t expectedSamples[] = {0, 0, 128, 704, 1728, 2304};
  for (size_t i = 0; i < delivered.size(); ++i) {
    const int64_t expected = expectedSamples[i] * 1000000 / 44100;
    EXPECT_NEAR(static_cast<double>(expected), static_cast<double>(delivered[i].timeUs), 1.0);
  }
}

TEST(AVReaderTest, DetectsAndExtractsAnOggOpusStream) {
  const int packetCount = 8;
  auto media = coral::test::buildOggOpus(2, packetCount);
  ReaderHarness harness(std::move(media));
  harness.registerConsumer(0);

  ASSERT_TRUE(harness.prepare());
  const auto format = harness.reader().getTrackFormat(0);
  ASSERT_NE(nullptr, format);
  EXPECT_EQ(std::string(MimeTypes::AUDIO_OPUS), format->getMimeType());
  EXPECT_EQ(std::string(MimeTypes::AUDIO_OGG), format->getContainerMimeType());
  EXPECT_EQ(48000, format->getSampleRate());
  EXPECT_EQ(2, format->getChannelCount());
  // The decoder reads the identification header, the pre-skip and the seek pre-roll of the stream.
  ASSERT_EQ(3u, format->csdList().size());
  EXPECT_EQ(19u, format->csdList()[0].size());
  EXPECT_EQ(8u, format->csdList()[1].size());
  EXPECT_EQ(8u, format->csdList()[2].size());

  EXPECT_EQ(AVReader::kResultEndOfInput, harness.readToEnd());
  const auto& delivered = harness.samples(0);
  ASSERT_EQ(static_cast<size_t>(packetCount), delivered.size());
  for (size_t i = 0; i < delivered.size(); ++i) {
    EXPECT_EQ(32u, delivered[i].size);
    EXPECT_EQ(static_cast<int64_t>(i) * 20000, delivered[i].timeUs);
    EXPECT_TRUE(delivered[i].flags & AVSample::kFlagKeyFrame);
  }

  const auto seekMap = harness.reader().getSeekMap();
  ASSERT_NE(nullptr, seekMap);
  EXPECT_EQ(static_cast<int64_t>(packetCount) * 20000, seekMap->getDurationUs());
}

TEST(AVReaderTest, DetectsAndExtractsAnOggFlacStream) {
  const int packetCount = 4;
  constexpr int64_t kSamplesPerFrame = 4096;
  auto media = coral::test::buildOggFlac(44100, 2, 16, packetCount);
  ReaderHarness harness(std::move(media));
  harness.registerConsumer(0);

  ASSERT_TRUE(harness.prepare());
  const auto format = harness.reader().getTrackFormat(0);
  ASSERT_NE(nullptr, format);
  EXPECT_EQ(std::string(MimeTypes::AUDIO_FLAC), format->getMimeType());
  EXPECT_EQ(std::string(MimeTypes::AUDIO_OGG), format->getContainerMimeType());
  EXPECT_EQ(44100, format->getSampleRate());
  EXPECT_EQ(2, format->getChannelCount());
  // The decoder reads the "fLaC" marker and the STREAMINFO block of the stream.
  ASSERT_EQ(1u, format->csdList().size());
  EXPECT_EQ(42u, format->csdList()[0].size());

  EXPECT_EQ(AVReader::kResultEndOfInput, harness.readToEnd());
  const auto& delivered = harness.samples(0);
  ASSERT_EQ(static_cast<size_t>(packetCount), delivered.size());
  for (size_t i = 0; i < delivered.size(); ++i) {
    EXPECT_EQ(static_cast<int64_t>(i) * kSamplesPerFrame * 1000000 / 44100, delivered[i].timeUs);
  }

  const auto seekMap = harness.reader().getSeekMap();
  ASSERT_NE(nullptr, seekMap);
  EXPECT_EQ(static_cast<int64_t>(packetCount) * kSamplesPerFrame * 1000000 / 44100,
            seekMap->getDurationUs());
}

TEST(AVReaderTest, SeeksInAnOggStream) {
  const int packetCount = 40;
  auto media = coral::test::buildOggVorbis(44100, 2, packetCount);
  ReaderHarness harness(std::move(media));
  harness.registerConsumer(0);

  ASSERT_TRUE(harness.prepare());
  ASSERT_EQ(AVReader::kResultEndOfInput, harness.readToEnd());
  const size_t firstRunSize = harness.samples(0).size();
  ASSERT_EQ(static_cast<size_t>(packetCount), firstRunSize);

  // The granule of the fifth audio packet, i.e. 4 * 128 samples, which the index of the seek map
  // resolves to the page that carries the packet.
  const int64_t targetUs = 4 * 128 * 1000000 / 44100;
  ASSERT_TRUE(harness.reader().seek(-1, targetUs));
  for (int i = 0; i < 100 && harness.samples(0).size() == firstRunSize; ++i) {
    if (harness.reader().read() == AVReader::kResultError) {
      break;
    }
  }

  const auto& delivered = harness.samples(0);
  ASSERT_GT(delivered.size(), firstRunSize);
  // The sample that contains the requested time is delivered, therefore its timestamp is at most
  // one packet before the target.
  const int64_t timeUs = delivered[firstRunSize].timeUs;
  EXPECT_LE(timeUs, targetUs);
  EXPECT_GE(timeUs, targetUs - 128 * 1000000 / 44100 - 1);
}

TEST(AVReaderTest, IndexesALongOggStream) {
  // More pages than the seek map indexes, i.e. more than 4096, so that the sampling that keeps the
  // index bounded is exercised as well.
  const int packetCount = 4200;
  auto media = coral::test::buildOggVorbis(44100, 2, packetCount);
  ReaderHarness harness(std::move(media));
  harness.registerConsumer(0);

  ASSERT_TRUE(harness.prepare());
  ASSERT_EQ(AVReader::kResultEndOfInput, harness.readToEnd());
  const auto& delivered = harness.samples(0);
  ASSERT_EQ(static_cast<size_t>(packetCount), delivered.size());

  const auto seekMap = harness.reader().getSeekMap();
  ASSERT_NE(nullptr, seekMap);
  EXPECT_TRUE(seekMap->isSeekable());
  EXPECT_LE(seekMap->getSeekPoints().size(), 4096u);
  EXPECT_EQ(static_cast<int64_t>(packetCount - 1) * 128 * 1000000 / 44100,
            seekMap->getDurationUs());

  // The seek resolves to a page boundary that the index kept, which precedes the requested time by
  // the stride of the sampled index, and the stream reader skips the packets in between.
  const int64_t targetUs = static_cast<int64_t>(2000) * 128 * 1000000 / 44100;
  ASSERT_TRUE(harness.reader().seek(-1, targetUs));
  for (int i = 0; i < 1000 && harness.samples(0).size() == static_cast<size_t>(packetCount); ++i) {
    if (harness.reader().read() == AVReader::kResultError) {
      break;
    }
  }
  const auto& afterSeek = harness.samples(0);
  ASSERT_GT(afterSeek.size(), static_cast<size_t>(packetCount));
  const int64_t timeUs = afterSeek[static_cast<size_t>(packetCount)].timeUs;
  EXPECT_LE(timeUs, targetUs);
  EXPECT_GE(timeUs, targetUs - static_cast<int64_t>(200) * 128 * 1000000 / 44100);
}

TEST(AVReaderTest, TheOggExtractorWinsOverTheAudioOnlyExtractors) {
  // An Ogg stream whose first bytes are also a plausible MPEG frame header: the signature of the
  // container is what decides, whatever the file type the caller claims.
  auto media = coral::test::buildOggVorbis(44100, 2, 4);
  ReaderHarness harness(std::move(media), FileTypes::MP3);
  harness.registerConsumer(0);

  ASSERT_TRUE(harness.prepare());
  EXPECT_EQ("Ogg", harness.reader().getSelectedExtractorName());
  EXPECT_EQ(AVReader::kResultEndOfInput, harness.readToEnd());
  EXPECT_EQ(4u, harness.samples(0).size());
}

TEST(AVReaderTest, FailsOnAnOggStreamOfAnUnsupportedCodec) {
  // An Ogg page that carries a Theora identification header, which is not a codec of the extractor.
  std::vector<uint8_t> packet = {0x80, 't', 'h', 'e', 'o', 'r', 'a'};
  std::vector<uint8_t> media;
  coral::test::appendOggPage(media, 0x02, 0, 7, 0, {packet});

  ReaderHarness harness(std::move(media));
  EXPECT_FALSE(harness.prepare());
  EXPECT_FALSE(harness.reader().getLastError().empty());
}

// ==================================================================================================
// AVI
// ==================================================================================================

TEST(AVReaderTest, DetectsAndExtractsAPcmAviFile) {
  const int chunkCount = 10;
  auto media = coral::test::buildAviPcmAudio(8000, 1, chunkCount, 64);
  ReaderHarness harness(std::move(media), FileTypes::AVI);
  harness.registerConsumer(0);

  ASSERT_TRUE(harness.prepare());
  const auto format = harness.reader().getTrackFormat(0);
  ASSERT_NE(nullptr, format);
  EXPECT_TRUE(format->isAudio());
  EXPECT_EQ(std::string(MimeTypes::AUDIO_RAW), format->getMimeType());
  EXPECT_EQ(8000, format->getSampleRate());
  EXPECT_EQ(1, format->getChannelCount());
  EXPECT_EQ(coral::AudioSampleFormat::kInt16, format->getSampleFormat());

  const auto track = harness.reader().getTrack(0);
  ASSERT_NE(nullptr, track);
  EXPECT_EQ(MediaType::kMediaTypeAudio, track->getType());

  EXPECT_EQ(AVReader::kResultEndOfInput, harness.readToEnd());
  const auto& delivered = harness.samples(0);
  ASSERT_EQ(static_cast<size_t>(chunkCount), delivered.size());
  for (size_t i = 0; i < delivered.size(); ++i) {
    EXPECT_EQ(128u, delivered[i].size);
    // Every chunk holds 64 frames at 8000 Hz, i.e. eight milliseconds.
    EXPECT_EQ(static_cast<int64_t>(i) * 8000, delivered[i].timeUs);
    EXPECT_TRUE(delivered[i].flags & AVSample::kFlagKeyFrame);
  }
  EXPECT_TRUE(delivered.front().flags & AVSample::kFlagFirstSample);
  EXPECT_TRUE(harness.isEndOfStream(0));

  const auto seekMap = harness.reader().getSeekMap();
  ASSERT_NE(nullptr, seekMap);
  // The index of an AVI file describes the samples that precede it, therefore the container reports
  // its duration without being seekable.
  EXPECT_FALSE(seekMap->isSeekable());
  EXPECT_EQ(static_cast<int64_t>(chunkCount) * 8000, seekMap->getDurationUs());
}

TEST(AVReaderTest, ExtractsTheVideoAndTheAudioOfAnAviFile) {
  const int frameCount = 6;
  auto media = coral::test::buildAviVideoAndAudio("FMP4", frameCount, 100);
  ReaderHarness harness(std::move(media), FileTypes::AVI);
  harness.registerConsumer(0);
  harness.registerConsumer(1);

  ASSERT_TRUE(harness.prepare());
  const auto ids = harness.reader().getTrackIds();
  ASSERT_EQ(2u, ids.size());
  EXPECT_EQ(0, ids[0]);
  EXPECT_EQ(1, ids[1]);

  const auto videoFormat = harness.reader().getTrackFormat(0);
  ASSERT_NE(nullptr, videoFormat);
  EXPECT_TRUE(videoFormat->isVideo());
  EXPECT_EQ(std::string(MimeTypes::VIDEO_MP4V), videoFormat->getMimeType());
  EXPECT_EQ(320, videoFormat->getWidth());
  EXPECT_EQ(240, videoFormat->getHeight());

  const auto audioFormat = harness.reader().getTrackFormat(1);
  ASSERT_NE(nullptr, audioFormat);
  EXPECT_TRUE(audioFormat->isAudio());
  EXPECT_EQ(std::string(MimeTypes::AUDIO_RAW), audioFormat->getMimeType());

  EXPECT_EQ(AVReader::kResultEndOfInput, harness.readToEnd());
  const auto& video = harness.samples(0);
  ASSERT_EQ(static_cast<size_t>(frameCount), video.size());
  for (size_t i = 0; i < video.size(); ++i) {
    EXPECT_EQ(100u, video[i].size);
    // The movie lasts 6 frames at 25 frames per second, i.e. 40 milliseconds per frame.
    EXPECT_EQ(static_cast<int64_t>(i) * 40000, video[i].timeUs);
  }
  // The fixture frames its first video chunk as an uncompressed frame, which is what marks a key
  // frame in a container whose index is stored after the samples it describes.
  EXPECT_TRUE(video[0].flags & AVSample::kFlagKeyFrame);
  EXPECT_FALSE(video[1].flags & AVSample::kFlagKeyFrame);

  const auto& audio = harness.samples(1);
  ASSERT_EQ(8u, audio.size());
  for (size_t i = 0; i < audio.size(); ++i) {
    EXPECT_EQ(128u, audio[i].size);
    EXPECT_EQ(static_cast<int64_t>(i) * 8000, audio[i].timeUs);
    // Every audio sample of a RIFF file is a sync sample.
    EXPECT_TRUE(audio[i].flags & AVSample::kFlagKeyFrame);
  }
}

TEST(AVReaderTest, TreatsEveryFrameOfAMotionJpegAviFileAsAKeyFrame) {
  auto media = coral::test::buildAviVideoAndAudio("MJPG", 3, 64);
  ReaderHarness harness(std::move(media), FileTypes::AVI);
  harness.registerConsumer(0);

  ASSERT_TRUE(harness.prepare());
  const auto format = harness.reader().getTrackFormat(0);
  ASSERT_NE(nullptr, format);
  EXPECT_EQ(std::string(MimeTypes::VIDEO_MJPEG), format->getMimeType());

  EXPECT_EQ(AVReader::kResultEndOfInput, harness.readToEnd());
  const auto& video = harness.samples(0);
  ASSERT_EQ(3u, video.size());
  for (const auto& sample : video) {
    EXPECT_TRUE(sample.flags & AVSample::kFlagKeyFrame);
  }
}

TEST(AVReaderTest, SkipsTheStreamOfAnAviFileWhoseCodecIsUnknown) {
  // A stream whose format tag describes a codec that this extractor does not expose, e.g. a Speex
  // stream: the container is recognized, but it exposes no track at all.
  auto media = coral::test::buildAviPcmAudio(8000, 1, 4, 64, 0xA109);
  ReaderHarness harness(std::move(media), FileTypes::AVI);
  harness.registerConsumer(0);

  ASSERT_TRUE(harness.prepare());
  EXPECT_TRUE(harness.reader().getTrackIds().empty());
  EXPECT_EQ(AVReader::kResultEndOfInput, harness.readToEnd());
}

TEST(AVReaderTest, SeeksInAnAviFile) {
  const int chunkCount = 20;
  auto media = coral::test::buildAviPcmAudio(8000, 1, chunkCount, 64);
  ReaderHarness harness(std::move(media), FileTypes::AVI);
  harness.registerConsumer(0);

  ASSERT_TRUE(harness.prepare());
  ASSERT_EQ(AVReader::kResultEndOfInput, harness.readToEnd());
  const size_t firstRunSize = harness.samples(0).size();
  ASSERT_EQ(static_cast<size_t>(chunkCount), firstRunSize);

  // The container is not seekable, therefore the reader restarts it and the extractor skips the
  // samples that precede the requested time.
  const int64_t targetUs = 80000;
  ASSERT_TRUE(harness.reader().seek(-1, targetUs));
  for (int i = 0; i < 100 && harness.samples(0).size() == firstRunSize; ++i) {
    if (harness.reader().read() == AVReader::kResultError) {
      break;
    }
  }

  const auto& delivered = harness.samples(0);
  ASSERT_GT(delivered.size(), firstRunSize);
  EXPECT_EQ(targetUs, delivered[firstRunSize].timeUs);
}

TEST(AVReaderTest, RejectsAPositionSeekInAnAviFile) {
  auto media = coral::test::buildAviPcmAudio(8000, 1, 10, 64);
  ReaderHarness harness(std::move(media), FileTypes::AVI);
  harness.registerConsumer(0);

  ASSERT_TRUE(harness.prepare());
  ASSERT_EQ(AVReader::kResultEndOfInput, harness.readToEnd());
  // The extractor cannot resume at a byte position, because the index of an AVI file is stored
  // after the samples it describes.
  EXPECT_TRUE(harness.reader().seek(1000, -1));
  EXPECT_EQ(AVReader::kResultError, harness.reader().read());
  EXPECT_FALSE(harness.reader().getLastError().empty());
}

// ==================================================================================================
// Failure cases
// ==================================================================================================

TEST(AVReaderTest, FailsWhenNoExtractorRecognizesTheMedia) {
  std::vector<uint8_t> media(1024, 0x5A);
  ReaderHarness harness(std::move(media));
  EXPECT_FALSE(harness.prepare());
  EXPECT_FALSE(harness.reader().getLastError().empty());
}

TEST(AVReaderTest, FailsOnAnEmptyMedia) {
  std::vector<uint8_t> media;
  ReaderHarness harness(std::move(media));
  EXPECT_FALSE(harness.prepare());
}

TEST(AVReaderTest, RejectsASeekBeyondTheEndOfTheMedia) {
  auto media = coral::test::buildWav(8000, 1, 800);
  ReaderHarness harness(std::move(media));
  harness.registerConsumer(0);
  ASSERT_TRUE(harness.prepare());
  EXPECT_FALSE(harness.reader().seek(1000000, -1));
  EXPECT_TRUE(harness.reader().seek(0, -1));
}

TEST(AVReaderTest, SeekRestartsTheExtractionAtTheRequestedPosition) {
  auto media = coral::test::buildWav(8000, 1, 8000);
  ReaderHarness harness(std::move(media), FileTypes::WAV);
  harness.registerConsumer(0);
  ASSERT_TRUE(harness.prepare());
  ASSERT_EQ(AVReader::kResultEndOfInput, harness.readToEnd());
  const auto firstRunSize = harness.samples(0).size();
  ASSERT_GT(firstRunSize, 0u);

  // A second pass from the beginning produces the same samples.
  ASSERT_TRUE(harness.reader().seek(0, -1));
  for (int i = 0; i < 1000000; ++i) {
    if (harness.reader().read() != AVReader::kResultContinue) {
      break;
    }
  }
  EXPECT_EQ(firstRunSize * 2, harness.samples(0).size());
}

TEST(AVReaderTest, TheReaderIsUsableWithoutAnyRegisteredConsumer) {
  auto media = coral::test::buildWav(8000, 1, 800);
  ReaderHarness harness(std::move(media));
  ASSERT_TRUE(harness.prepare());
  EXPECT_EQ(AVReader::kResultEndOfInput, harness.readToEnd());
}

TEST(AVReaderTest, ExposesTheTrackIdsAndTypes) {
  auto media = coral::test::buildWav(8000, 1, 800);
  ReaderHarness harness(std::move(media));
  harness.registerConsumer(0);
  ASSERT_TRUE(harness.prepare());

  const auto ids = harness.reader().getTrackIds();
  ASSERT_EQ(1u, ids.size());
  EXPECT_EQ(0, ids[0]);

  const auto track = harness.reader().getTrack(0);
  ASSERT_NE(nullptr, track);
  EXPECT_EQ(0, track->id());
  EXPECT_EQ(MediaType::kMediaTypeAudio, track->getType());
  EXPECT_EQ(nullptr, harness.reader().getTrack(42));
}

TEST(AVReaderTest, TheFactoryPrefersTheExtractorOfTheKnownFileType) {
  // A WAV file whose extension claims otherwise: the sniffing must still find the WAV extractor.
  auto media = coral::test::buildWav(8000, 1, 800);
  ReaderHarness harness(std::move(media), FileTypes::MP3);
  harness.registerConsumer(0);
  ASSERT_TRUE(harness.prepare());
  EXPECT_EQ(AVReader::kResultEndOfInput, harness.readToEnd());
  EXPECT_FALSE(harness.samples(0).empty());
}
