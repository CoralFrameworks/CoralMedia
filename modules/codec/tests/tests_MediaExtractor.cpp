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

#include "CodecTestUtils.h"

#if defined(WINDOWS) || defined(WIN32)

#include <win/WmfMediaExtractor.h>

#include <gtest/gtest.h>

#include <memory>
#include <vector>

namespace coral::codec::test {

namespace {

constexpr int SAMPLE_RATE = 48000;
constexpr int CHANNEL_COUNT = 2;
constexpr int FRAME_COUNT = SAMPLE_RATE;

std::shared_ptr<MediaFormat> createLinearPcmFormat() {
  auto format = MediaFormat::CreateAudioFormat(MimeTypes::AUDIO_RAW, SAMPLE_RATE, CHANNEL_COUNT);
  format->setAudioFormatType(AudioFormatType::kAudioFormatTypeLinearPCM);
  format->setSampleFormat(AudioSampleFormat::kInt16);
  return format;
}

std::shared_ptr<MediaFormat> createAacFormat() {
  auto format = MediaFormat::CreateAudioFormat(MimeTypes::AUDIO_AAC, SAMPLE_RATE, CHANNEL_COUNT);
  format->setAudioFormatType(AudioFormatType::kAudioFormatTypeMpeg4AAC);
  format->setSampleFormat(AudioSampleFormat::kInt16);
  format->setAverageBitrate(128000);
  return format;
}

/**
 * Writes a wav file with the given number of frames.
 */
bool writeWaveFile(const std::string& path, const int frameCount) {
  auto writer = AudioFileWriter::CreateWithUrl(path, MediaFileType::kMediaFileTypeWav,
    createLinearPcmFormat());
  if (writer == nullptr) {
    return false;
  }
  Error* error = nullptr;
  if (!writer->open(&error)) {
    return false;
  }
  const auto pcm = generatePcm(SAMPLE_RATE, CHANNEL_COUNT, frameCount);
  const auto written = writer->writeFromPCMBuffer(const_cast<int16_t*>(pcm.data()), frameCount,
    &error);
  writer->close();
  return written == frameCount;
}

/**
 * Writes an aac file with the given number of frames.
 */
bool writeAacFile(const std::string& path, const int frameCount) {
  auto writer = AudioFileWriter::CreateWithUrl(path, MediaFileType::kMediaFileTypeMpeg4AAC,
    createAacFormat());
  if (writer == nullptr) {
    return false;
  }
  Error* error = nullptr;
  if (!writer->open(&error)) {
    return false;
  }
  const auto pcm = generatePcm(SAMPLE_RATE, CHANNEL_COUNT, frameCount);
  constexpr int CHUNK_FRAMES = 1024;
  int written = 0;
  while (written < frameCount) {
    const auto frames = std::min(CHUNK_FRAMES, frameCount - written);
    if (writer->writeFromPCMBuffer(const_cast<int16_t*>(pcm.data() + written * CHANNEL_COUNT),
      frames, &error) != frames) {
      return false;
    }
    written += frames;
  }
  writer->close();
  return true;
}

/**
 * Reads the whole stream of the extractor.
 */
void readToEnd(AVMediaExtractor* extractor, const std::shared_ptr<CollectingExtractorOutput>& output) {
  AVMediaExtractor::PositionHolder holder;
  int guard = 0;
  while (extractor->read(holder) == AVMediaExtractor::PlatformResult::RESULT_CONTINUE) {
    if (++guard > 100000) {
      break;
    }
  }
}

} // namespace anonymous

TEST(WmfMediaExtractor, ExtractsWaveFile) {
  const auto path = tempFilePath("extractor.wav");
  ASSERT_TRUE(writeWaveFile(path, FRAME_COUNT));

  // The extractor hands itself out as the seek map of the media it reads, so it is owned by a shared
  // pointer: the unique pointer of the concrete factory is moved into one.
  std::shared_ptr<AVMediaExtractor> extractor = WmfMediaExtractor::createWithUrl(path);
  ASSERT_NE(extractor, nullptr);

  auto output = std::make_shared<CollectingExtractorOutput>();
  extractor->init(*output);

  EXPECT_TRUE(output->endTracksCalled);
  EXPECT_EQ(output->trackCount(), 1u);
  EXPECT_NE(output->mSeekMap, nullptr);

  const auto* track = output->firstTrackOfType(MediaType::kMediaTypeAudio);
  ASSERT_NE(track, nullptr);
  ASSERT_NE(track->format, nullptr);
  EXPECT_EQ(track->format->getSampleRate(), SAMPLE_RATE);
  EXPECT_EQ(track->format->getChannelCount(), CHANNEL_COUNT);
  EXPECT_EQ(track->format->getAudioFormatType(), AudioFormatType::kAudioFormatTypeLinearPCM);

  readToEnd(extractor.get(), output);

  EXPECT_GT(track->sampleCount, 0);
  EXPECT_EQ(track->byteCount, static_cast<int64_t>(FRAME_COUNT) * CHANNEL_COUNT * 2);
  EXPECT_NEAR(static_cast<double>(extractor->getDurationUs()), 1000000.0, 100000.0);
  EXPECT_GT(extractor->getReadPosition(), 0);

  // seeks back to the half of the file and reads a sample again.
  ASSERT_TRUE(extractor->seekTo(500000, AVMediaExtractor::SeekOption::SEEK_TO_PREVIOUS_SYNC));
  {
    AVMediaExtractor::PositionHolder holder;
    ASSERT_EQ(extractor->read(holder), AVMediaExtractor::PlatformResult::RESULT_CONTINUE);
    EXPECT_GE(output->tracks.begin()->second.lastTimeUs, 400000);
  }

  extractor->clear();
}

TEST(WmfMediaExtractor, ExtractsMpeg4AacFile) {
  const auto path = tempFilePath("extractor.m4a");
  ASSERT_TRUE(writeAacFile(path, FRAME_COUNT));

  // The extractor hands itself out as the seek map of the media it reads, so it is owned by a shared
  // pointer: the unique pointer of the concrete factory is moved into one.
  std::shared_ptr<AVMediaExtractor> extractor = WmfMediaExtractor::createWithUrl(path);
  ASSERT_NE(extractor, nullptr);

  auto output = std::make_shared<CollectingExtractorOutput>();
  extractor->init(*output);

  const auto* track = output->firstTrackOfType(MediaType::kMediaTypeAudio);
  ASSERT_NE(track, nullptr);
  ASSERT_NE(track->format, nullptr);
  EXPECT_EQ(track->format->getSampleRate(), SAMPLE_RATE);
  EXPECT_EQ(track->format->getChannelCount(), CHANNEL_COUNT);
  EXPECT_EQ(track->format->getAudioFormatType(), AudioFormatType::kAudioFormatTypeMpeg4AAC);
  EXPECT_FALSE(track->format->getMimeType().empty());

  readToEnd(extractor.get(), output);

  EXPECT_GT(track->sampleCount, 0);
  EXPECT_GT(track->byteCount, 0);
  EXPECT_NEAR(static_cast<double>(extractor->getDurationUs()), 1000000.0, 150000.0);
  extractor->clear();
}

TEST(WmfMediaExtractor, RejectsUnknownFile) {
  EXPECT_EQ(WmfMediaExtractor::createWithUrl(tempFilePath("does-not-exist.mp4")), nullptr);
}

} // namespace coral::codec::test

#endif // WINDOWS
