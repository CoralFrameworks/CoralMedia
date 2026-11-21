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

#include <win/WmfAudioFileReader.h>

#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <vector>

namespace coral::codec::test {

namespace {

constexpr int SAMPLE_RATE = 48000;
constexpr int CHANNEL_COUNT = 2;
constexpr int FRAME_COUNT = SAMPLE_RATE; // one second

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

int64_t writePcm(AudioFileWriter* writer, const std::vector<int16_t>& pcm, Error** outError) {
  return writer->writeFromPCMBuffer(const_cast<int16_t*>(pcm.data()),
    pcm.size() / CHANNEL_COUNT, outError);
}

} // namespace anonymous

TEST(AudioFileWriter, WriteWaveFile) {
  const auto path = tempFilePath("writer.wav");
  const auto pcm = generatePcm(SAMPLE_RATE, CHANNEL_COUNT, FRAME_COUNT);

  auto writer = AudioFileWriter::CreateWithUrl(path, MediaFileType::kMediaFileTypeWav,
    createLinearPcmFormat());
  ASSERT_NE(writer, nullptr);
  EXPECT_EQ(writer->getSampleRate(), SAMPLE_RATE);
  EXPECT_EQ(writer->getChannelCount(), CHANNEL_COUNT);
  EXPECT_EQ(writer->getSampleFormat(), AudioSampleFormat::kInt16);

  Error* error = nullptr;
  ASSERT_TRUE(writer->open(&error)) << (error != nullptr ? error->toString() : "");
  EXPECT_EQ(writePcm(writer.get(), pcm, &error), FRAME_COUNT);
  EXPECT_EQ(writer->getFramePosition(), FRAME_COUNT);
  EXPECT_EQ(writer->getSampleLength(), FRAME_COUNT);
  writer->close();
  writer.reset();

  // the wav file is read by the wave file reader.
  auto reader = AudioFileReader::CreateWithUrl(path);
  ASSERT_NE(reader, nullptr);
  ASSERT_TRUE(reader->open(&error)) << (error != nullptr ? error->toString() : "");
  EXPECT_EQ(reader->getSampleRate(), SAMPLE_RATE);
  EXPECT_EQ(reader->getChannelCount(), CHANNEL_COUNT);
  EXPECT_EQ(reader->getSampleFormat(), AudioSampleFormat::kInt16);
  EXPECT_EQ(reader->getSampleLength(), FRAME_COUNT);
  reader->close();
  reader.reset();

  // the platform reader decodes the same wav file.
  auto wmfReader = WmfAudioFileReader::sniff(path);
  ASSERT_NE(wmfReader, nullptr);
  EXPECT_EQ(wmfReader->getSampleRate(), SAMPLE_RATE);
  EXPECT_EQ(wmfReader->getChannelCount(), CHANNEL_COUNT);
  EXPECT_EQ(wmfReader->getSampleFormat(), AudioSampleFormat::kInt16);
  EXPECT_NEAR(static_cast<double>(wmfReader->getSampleLength()), FRAME_COUNT, FRAME_COUNT * 0.05);

  const auto frames = readAllFrames(wmfReader.get(), &error);
  ASSERT_GT(frames, 0) << (error != nullptr ? error->toString() : "");
  EXPECT_NEAR(static_cast<double>(frames), FRAME_COUNT, FRAME_COUNT * 0.05);
  wmfReader->close();
}

TEST(AudioFileWriter, WriteMpeg4AacFile) {
  const auto path = tempFilePath("writer.m4a");
  const auto pcm = generatePcm(SAMPLE_RATE, CHANNEL_COUNT, FRAME_COUNT);

  auto writer = AudioFileWriter::CreateWithUrl(path, MediaFileType::kMediaFileTypeMpeg4AAC,
    createAacFormat());
  ASSERT_NE(writer, nullptr) << "the platform does not provide an aac encoder";

  Error* error = nullptr;
  ASSERT_TRUE(writer->open(&error)) << (error != nullptr ? error->toString() : "");

  // writes the pcm data in chunks, the encoder works on frame bursts.
  constexpr size_t CHUNK_FRAMES = 1024;
  size_t written = 0;
  while (written < FRAME_COUNT) {
    const auto frames = std::min<size_t>(CHUNK_FRAMES, FRAME_COUNT - written);
    const auto result = writer->writeFromPCMBuffer(
      const_cast<int16_t*>(pcm.data() + written * CHANNEL_COUNT), frames, &error);
    ASSERT_EQ(result, static_cast<int64_t>(frames)) << (error != nullptr ? error->toString() : "");
    written += frames;
  }
  EXPECT_EQ(writer->getFramePosition(), FRAME_COUNT);
  writer->close();
  writer.reset();

  ASSERT_TRUE(std::filesystem::exists(path));
  EXPECT_GT(std::filesystem::file_size(path), 1024u);

  // the written file is decoded by the platform reader.
  auto reader = WmfAudioFileReader::sniff(path);
  ASSERT_NE(reader, nullptr) << "the written file is not readable by the media foundation reader";
  EXPECT_EQ(reader->getSampleRate(), SAMPLE_RATE);
  EXPECT_EQ(reader->getChannelCount(), CHANNEL_COUNT);
  EXPECT_NEAR(static_cast<double>(reader->getSampleLength()), FRAME_COUNT, FRAME_COUNT * 0.05);

  const auto frames = readAllFrames(reader.get(), &error);
  ASSERT_GT(frames, 0) << (error != nullptr ? error->toString() : "");
  EXPECT_NEAR(static_cast<double>(frames), FRAME_COUNT, FRAME_COUNT * 0.10);
  reader->close();
}

TEST(AudioFileWriter, RejectUnsupportedFileType) {
  const auto path = tempFilePath("writer.opus");
  auto format = MediaFormat::CreateAudioFormat(MimeTypes::AUDIO_OPUS, SAMPLE_RATE, CHANNEL_COUNT);
  format->setAudioFormatType(AudioFormatType::kAudioFormatTypeOpus);
  format->setSampleFormat(AudioSampleFormat::kInt16);

  auto writer = AudioFileWriter::CreateWithUrl(path, MediaFileType::kMediaFileTypeOpus, format);
  EXPECT_EQ(writer, nullptr);
}

} // namespace coral::codec::test

#endif // WINDOWS
