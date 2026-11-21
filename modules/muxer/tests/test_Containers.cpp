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
#include <media/Format.h>
#include <muxer/Muxer.h>
#include <muxer/MuxerAdapter.h>
#include <muxer/MuxerFactory.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace coral;
using namespace coral::muxer;

struct ReadResult {
  /** The number of audio/video tracks, i.e. the tracks the muxer wrote. */
  int trackCount = 0;
  /** The number of tracks the extractor exposes, including its auxiliary metadata tracks. */
  int totalTrackCount = 0;
  std::shared_ptr<MediaFormat> format;
  int64_t sampleCount = 0;
  int64_t durationUs = 0;
};

bool readOutput(const std::string& path, ReadResult* result) {
  auto dataSource = std::make_shared<AVFileDataSource>();
  if (!dataSource->open(path)) {
    return false;
  }
  AVReader reader(dataSource, AVExtractorsFactory::createDefault(-1, dataSource->getLength()));
  if (!reader.prepare()) {
    return false;
  }
  const auto ids = reader.getTrackIds();
  if (!ids.empty()) {
    result->format = reader.getTrackFormat(ids.front());
    reader.setTrackConsumer(ids.front(), [result](const AVSample& sample) {
      if (!sample.isEndOfStream() && sample.size() > 0) {
        ++result->sampleCount;
      }
    });
  }
  while (true) {
    const int status = reader.read();
    if (status == AVReader::kResultError) {
      return false;
    }
    if (status == AVReader::kResultEndOfInput) {
      break;
    }
  }
  reader.endTracks();
  // The extractor may declare auxiliary tracks next to the ones the test muxed, e.g. the ID3
  // metadata track of an ADTS stream, so only the audio/video tracks are counted. Their formats are
  // published once the samples were read.
  result->totalTrackCount = static_cast<int>(ids.size());
  result->trackCount = 0;
  for (const int id: ids) {
    const auto format = reader.getTrackFormat(id);
    if (format != nullptr && (format->isAudio() || format->isVideo())) {
      ++result->trackCount;
    }
  }
  result->durationUs = reader.getDurationUs();
  if (result->format == nullptr && !ids.empty()) {
    result->format = reader.getTrackFormat(ids.front());
  }
  return true;
}

std::vector<uint8_t> readFileStart(const std::filesystem::path& path, const size_t count) {
  std::ifstream stream(path, std::ios::binary);
  std::vector<uint8_t> data(count);
  stream.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(count));
  data.resize(static_cast<size_t>(stream.gcount()));
  return data;
}

} // namespace

TEST(AdtsMuxerTest, WritesOneAdtsHeaderPerAacSample) {
  const auto output = std::filesystem::current_path() / "coral-adts.aac";
  std::filesystem::remove(output);
  auto factory = createMuxerFactory();
  ASSERT_NE(nullptr, factory);
  Error error;
  auto muxer = factory->create(output.string(), Container::kAdts, MuxerOptions(), &error);
  ASSERT_NE(nullptr, muxer) << error.message;

  auto format = MediaFormat::CreateAudioFormat("audio/mp4a-latm", 48000, 2);
  format->setCodecSpecificData(0, std::vector<uint8_t>{ 0x11, 0x90 });
  const int track = muxer->addTrack(format);
  ASSERT_GE(track, 0) << muxer->getError().message;
  const std::vector<uint8_t> payload(100, 0x5A);
  for (int i = 0; i < 10; ++i) {
    SampleInfo info;
    info.presentationTimeUs = static_cast<int64_t>(i) * 21333;
    ASSERT_TRUE(muxer->writeSample(track, payload.data(), payload.size(), info));
  }
  ASSERT_TRUE(muxer->finish()) << muxer->getError().message;
  EXPECT_EQ(Container::kAdts, muxer->getContainer());

  const auto start = readFileStart(output, 3);
  ASSERT_EQ(3u, start.size());
  EXPECT_EQ(0xFF, start[0]);
  EXPECT_EQ(0xF1, start[1]);
  EXPECT_EQ(static_cast<uint8_t>(1 << 6 | 3 << 2), start[2]);

  ReadResult result;
  ASSERT_TRUE(readOutput(output.string(), &result));
  ASSERT_EQ(1, result.trackCount);
  // The ADTS extractor declares the ID3 metadata track of the stream next to the audio track, even
  // when the stream carries no tag: only the audio track is part of the muxed file.
  EXPECT_EQ(2, result.totalTrackCount);
  ASSERT_NE(nullptr, result.format);
  EXPECT_EQ("audio/mp4a-latm", result.format->getMimeType());
  EXPECT_EQ(10, result.sampleCount);
  if (std::getenv("CORAL_KEEP_MUXER_OUTPUT") == nullptr) {
    std::filesystem::remove(output);
  }
}

TEST(WebmMuxerTest, WritesVp9AndOpusTracks) {
  const auto output = std::filesystem::current_path() / "coral-webm.webm";
  std::filesystem::remove(output);
  auto factory = createMuxerFactory();
  ASSERT_NE(nullptr, factory);
  Error error;
  auto muxer = factory->create(output.string(), Container::kWebM, MuxerOptions(), &error);
  ASSERT_NE(nullptr, muxer) << error.message;

  auto videoFormat = MediaFormat::CreateVideoFormat("video/x-vnd.on2.vp9", 320, 240);
  videoFormat->setFrameRate(30);
  auto audioFormat = MediaFormat::CreateAudioFormat("audio/opus", 48000, 2);
  const std::vector<uint8_t> opusHead = {
    'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 2, 0x01, 0x38,
    0x00, 0x00, 0xBB, 0x80, 0, 0, 0
  };
  audioFormat->setCodecSpecificData(0, opusHead);
  const int videoTrack = muxer->addTrack(videoFormat);
  const int audioTrack = muxer->addTrack(audioFormat);
  ASSERT_GE(videoTrack, 0) << muxer->getError().message;
  ASSERT_GE(audioTrack, 0) << muxer->getError().message;

  const std::vector<uint8_t> videoPayload(32, 0x11);
  const std::vector<uint8_t> audioPayload(24, 0x22);
  for (int i = 0; i < 6; ++i) {
    SampleInfo info;
    info.presentationTimeUs = static_cast<int64_t>(i) * 33333;
    info.durationUs = 33333;
    info.flags = i == 0 ? static_cast<uint32_t>(SampleFlag::kKeyFrame) : 0;
    ASSERT_TRUE(muxer->writeSample(videoTrack, videoPayload.data(), videoPayload.size(), info));
  }
  for (int i = 0; i < 14; ++i) {
    SampleInfo info;
    info.presentationTimeUs = static_cast<int64_t>(i) * 21333;
    info.durationUs = 21333;
    ASSERT_TRUE(muxer->writeSample(audioTrack, audioPayload.data(), audioPayload.size(), info));
  }
  ASSERT_TRUE(muxer->finish()) << muxer->getError().message;
  EXPECT_EQ(Container::kWebM, muxer->getContainer());

  ReadResult result;
  ASSERT_TRUE(readOutput(output.string(), &result));
  EXPECT_EQ(2, result.trackCount);
  ASSERT_NE(nullptr, result.format);
  EXPECT_GT(result.sampleCount, 0);
  EXPECT_GT(result.durationUs, 0);
  if (std::getenv("CORAL_KEEP_MUXER_OUTPUT") == nullptr) {
    std::filesystem::remove(output);
  }
}

TEST(OggMuxerTest, WritesOpusHeadersAndPages) {
  const auto output = std::filesystem::current_path() / "coral-opus.ogg";
  std::filesystem::remove(output);
  auto factory = createMuxerFactory();
  ASSERT_NE(nullptr, factory);
  Error error;
  auto muxer = factory->create(output.string(), Container::kOgg, MuxerOptions(), &error);
  ASSERT_NE(nullptr, muxer) << error.message;

  auto format = MediaFormat::CreateAudioFormat("audio/opus", 48000, 2);
  const std::vector<uint8_t> opusHead = {
    'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 2, 0x38, 0x01,
    0x00, 0x00, 0xBB, 0x80, 0, 0, 0
  };
  format->setCodecSpecificData(0, opusHead);
  const int track = muxer->addTrack(format);
  ASSERT_GE(track, 0) << muxer->getError().message;
  const std::vector<uint8_t> payload(40, 0x33);
  for (int i = 0; i < 10; ++i) {
    SampleInfo info;
    info.presentationTimeUs = static_cast<int64_t>(i) * 20000;
    info.durationUs = 20000;
    ASSERT_TRUE(muxer->writeSample(track, payload.data(), payload.size(), info));
  }
  ASSERT_TRUE(muxer->finish()) << muxer->getError().message;
  EXPECT_EQ(Container::kOgg, muxer->getContainer());

  const auto start = readFileStart(output, 4);
  ASSERT_EQ(4u, start.size());
  EXPECT_EQ('O', start[0]);
  EXPECT_EQ('g', start[1]);
  EXPECT_EQ('g', start[2]);
  EXPECT_EQ('S', start[3]);

  ReadResult result;
  ASSERT_TRUE(readOutput(output.string(), &result));
  ASSERT_EQ(1, result.trackCount);
  ASSERT_NE(nullptr, result.format);
  EXPECT_EQ("audio/opus", result.format->getMimeType());
  EXPECT_EQ(10, result.sampleCount);
  EXPECT_NEAR(200'000, static_cast<double>(result.durationUs), 30'000);
  if (std::getenv("CORAL_KEEP_MUXER_OUTPUT") == nullptr) {
    std::filesystem::remove(output);
  }
}

TEST(OggMuxerTest, RemuxesVorbisFromOgg) {
  const auto input = std::string(CORAL_ASSETS_DIR) + "/music/music.ogg";
  if (!std::filesystem::exists(input)) {
    GTEST_SKIP() << "fixture is not present";
  }
  const auto output = std::filesystem::current_path() / "coral-vorbis.ogg";
  std::filesystem::remove(output);
  auto factory = createMuxerFactory();
  ASSERT_NE(nullptr, factory);
  Error error;
  auto muxer = factory->create(output.string(), Container::kOgg, MuxerOptions(), &error);
  ASSERT_NE(nullptr, muxer) << error.message;

  auto dataSource = std::make_shared<AVFileDataSource>();
  ASSERT_TRUE(dataSource->open(input));
  AVReader reader(dataSource, AVExtractorsFactory::createDefault(-1, dataSource->getLength()));
  ASSERT_TRUE(reader.prepare()) << reader.getLastError();
  const auto ids = reader.getTrackIds();
  ASSERT_EQ(1u, ids.size());
  const auto format = reader.getTrackFormat(ids.front());
  ASSERT_NE(nullptr, format);
  const int track = muxer->addTrack(format);
  ASSERT_GE(track, 0) << muxer->getError().message;
  int sourceSamples = 0;
  reader.setTrackConsumer(ids.front(), [&](const AVSample& sample) {
    if (sample.isEndOfStream() || sample.isCodecConfig() || sample.size() == 0) {
      return;
    }
    SampleInfo info;
    info.presentationTimeUs = sample.timeUs;
    ASSERT_TRUE(muxer->writeSample(track, sample.array(), sample.size(), info));
    ++sourceSamples;
  });
  while (true) {
    const int status = reader.read();
    ASSERT_NE(AVReader::kResultError, status) << reader.getLastError();
    if (status == AVReader::kResultEndOfInput) {
      break;
    }
  }
  reader.endTracks();
  ASSERT_GT(sourceSamples, 0);
  ASSERT_TRUE(muxer->finish()) << muxer->getError().message;

  ReadResult result;
  ASSERT_TRUE(readOutput(output.string(), &result));
  ASSERT_EQ(1, result.trackCount);
  ASSERT_NE(nullptr, result.format);
  EXPECT_EQ("audio/vorbis", result.format->getMimeType());
  EXPECT_EQ(sourceSamples, result.sampleCount);
  EXPECT_NEAR(204'000'000, static_cast<double>(result.durationUs), 1'000'000);
  if (std::getenv("CORAL_KEEP_MUXER_OUTPUT") == nullptr) {
    std::filesystem::remove(output);
  }
}

TEST(OggMuxerTest, RemuxesFlacFromNativeStream) {
  const auto input = std::string(CORAL_ASSETS_DIR) + "/music/music.flac";
  if (!std::filesystem::exists(input)) {
    GTEST_SKIP() << "fixture is not present";
  }
  const auto output = std::filesystem::current_path() / "coral-flac.ogg";
  std::filesystem::remove(output);
  auto factory = createMuxerFactory();
  ASSERT_NE(nullptr, factory);
  Error error;
  auto muxer = factory->create(output.string(), Container::kOgg, MuxerOptions(), &error);
  ASSERT_NE(nullptr, muxer) << error.message;

  auto dataSource = std::make_shared<AVFileDataSource>();
  ASSERT_TRUE(dataSource->open(input));
  AVReader reader(dataSource, AVExtractorsFactory::createDefault(-1, dataSource->getLength()));
  ASSERT_TRUE(reader.prepare()) << reader.getLastError();
  const auto ids = reader.getTrackIds();
  ASSERT_EQ(1u, ids.size());
  int track = -1;
  int sourceSamples = 0;
  std::shared_ptr<MediaFormat> publishedFormat;
  reader.setTrackConsumer(ids.front(), [&](const AVSample& sample) {
    if (track < 0) {
      publishedFormat = reader.getTrackFormat(ids.front());
      if (publishedFormat == nullptr) {
        return;
      }
      track = muxer->addTrack(publishedFormat);
      ASSERT_GE(track, 0) << muxer->getError().message;
    }
    if (sample.isEndOfStream() || sample.isCodecConfig() || sample.size() == 0) {
      return;
    }
    SampleInfo info;
    info.presentationTimeUs = sample.timeUs;
    ASSERT_TRUE(muxer->writeSample(track, sample.array(), sample.size(), info));
    ++sourceSamples;
  });
  while (true) {
    const int status = reader.read();
    ASSERT_NE(AVReader::kResultError, status) << reader.getLastError();
    if (status == AVReader::kResultEndOfInput) {
      break;
    }
  }
  reader.endTracks();
  ASSERT_NE(nullptr, publishedFormat);
  EXPECT_EQ("audio/flac", publishedFormat->getMimeType());
  ASSERT_GT(sourceSamples, 0);
  ASSERT_TRUE(muxer->finish()) << muxer->getError().message;

  ReadResult result;
  ASSERT_TRUE(readOutput(output.string(), &result));
  ASSERT_EQ(1, result.trackCount);
  ASSERT_NE(nullptr, result.format);
  EXPECT_EQ("audio/flac", result.format->getMimeType());
  EXPECT_GT(result.sampleCount, 0);
  if (std::getenv("CORAL_KEEP_MUXER_OUTPUT") == nullptr) {
    std::filesystem::remove(output);
  }
}

TEST(MuxerAdapterTest, SupportsTheNativeContainerMatrix) {
  auto factory = createMuxerAdapterFactory();
  ASSERT_NE(nullptr, factory);
  EXPECT_TRUE(factory->isSupportMuxerFormat(MuxerFormat::kMuxerFormatMpeg4));
  EXPECT_TRUE(factory->isSupportMuxerFormat(MuxerFormat::kMuxerFormatWebm));
  EXPECT_TRUE(factory->isSupportMuxerFormat(MuxerFormat::kMuxerFormatOgg));
  EXPECT_TRUE(factory->isSupportMuxerFormat(MuxerFormat::kMuxerFormatOpus));
  EXPECT_FALSE(factory->isSupportMuxerFormat(MuxerFormat::kMuxerFormat3Gpp));
  EXPECT_FALSE(factory->isSupportMuxerFormat(MuxerFormat::kMuxerFormatFlac));
  EXPECT_FALSE(factory->isSupportMuxerFormat(MuxerFormat::kMuxerFormatH263));
  EXPECT_FALSE(factory->isSupportMuxerFormat(MuxerFormat::kMuxerFormatH264));
}

TEST(WavMuxerTest, WritesPcm16AndPatchesTheRiffSizes) {
  const auto output = std::filesystem::current_path() / "coral-pcm.wav";
  std::filesystem::remove(output);
  auto factory = createMuxerFactory();
  ASSERT_NE(nullptr, factory);
  Error error;
  auto muxer = factory->create(output.string(), Container::kWav, MuxerOptions(), &error);
  ASSERT_NE(nullptr, muxer) << error.message;

  auto format = MediaFormat::CreateAudioFormat("audio/raw", 48000, 2);
  format->setSampleFormat(AudioSampleFormat::kInt16);
  const int track = muxer->addTrack(format);
  ASSERT_GE(track, 0) << muxer->getError().message;
  const size_t frameBytes = 1024 * 2 * 2;
  const std::vector<uint8_t> payload(frameBytes, 0);
  for (int i = 0; i < 10; ++i) {
    SampleInfo info;
    info.presentationTimeUs = static_cast<int64_t>(i) * 1024 * 1'000'000 / 48000;
    ASSERT_TRUE(muxer->writeSample(track, payload.data(), payload.size(), info));
  }
  ASSERT_TRUE(muxer->finish()) << muxer->getError().message;
  EXPECT_EQ(Container::kWav, muxer->getContainer());

  const auto start = readFileStart(output, 44);
  ASSERT_GE(start.size(), 44u);
  EXPECT_EQ('R', start[0]);
  EXPECT_EQ('I', start[1]);
  EXPECT_EQ('F', start[2]);
  EXPECT_EQ('F', start[3]);
  EXPECT_EQ('W', start[8]);
  EXPECT_EQ('A', start[9]);
  EXPECT_EQ('V', start[10]);
  EXPECT_EQ('E', start[11]);
  const auto readLe32 = [&start](const size_t offset) {
    return static_cast<uint32_t>(start[offset]) |
           (static_cast<uint32_t>(start[offset + 1]) << 8) |
           (static_cast<uint32_t>(start[offset + 2]) << 16) |
           (static_cast<uint32_t>(start[offset + 3]) << 24);
  };
  EXPECT_EQ(36u + 10 * frameBytes, readLe32(4));
  EXPECT_EQ(10 * frameBytes, readLe32(40));
  EXPECT_EQ(44 + 10 * frameBytes, std::filesystem::file_size(output));

  ReadResult result;
  ASSERT_TRUE(readOutput(output.string(), &result));
  ASSERT_EQ(1, result.trackCount);
  ASSERT_NE(nullptr, result.format);
  EXPECT_EQ("audio/raw", result.format->getMimeType());
  EXPECT_EQ(AudioSampleFormat::kInt16, result.format->getSampleFormat());
  EXPECT_GT(result.sampleCount, 0);
  EXPECT_NEAR(213'333, static_cast<double>(result.durationUs), 2'000);
  if (std::getenv("CORAL_KEEP_MUXER_OUTPUT") == nullptr) {
    std::filesystem::remove(output);
  }
}
