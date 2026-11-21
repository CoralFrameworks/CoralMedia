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

#include <win/WmfAudioDecoder.h>
#include <win/WmfAudioEncoder.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <vector>

namespace coral::codec::test {

namespace {

constexpr int SAMPLE_RATE = 48000;
constexpr int CHANNEL_COUNT = 2;
constexpr int FRAME_COUNT = SAMPLE_RATE;
constexpr int CHUNK_FRAMES = 1024;

std::shared_ptr<MediaFormat> createPcmFormat() {
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
 * Encodes the pcm signal and collects the encoded packets.
 */
std::vector<EncodedPacket> encodePcm(AudioEncoder* encoder, const std::vector<int16_t>& pcm,
  int frameCount, const int bytesPerFrame, std::shared_ptr<MediaFormat>* outFormat) {
  std::vector<EncodedPacket> packets;
  bool formatReported = false;
  bool endOfStream = false;

  const auto drainOutput = [&]() {
    while (true) {
      auto packet = encoder->dequeueOutputBuffer();
      if (packet == nullptr) {
        break;
      }
      if (packet->isFormatChanged()) {
        if (outFormat != nullptr) {
          *outFormat = packet->getFormat();
        }
        formatReported = true;
        continue;
      }
      if (packet->isEndOfStream()) {
        endOfStream = true;
        break;
      }
      if (packet->size() > 0) {
        EncodedPacket encoded;
        encoded.data.assign(packet->array(), packet->array() + packet->size());
        encoded.timeUs = packet->timeUs();
        encoded.keyFrame = packet->isKeyFrame();
        packets.push_back(std::move(encoded));
      }
    }
  };

  int frameOffset = 0;
  int guard = 0;
  while (frameOffset < frameCount && guard++ < 100000) {
    auto input = encoder->dequeueInputBuffer();
    if (input == nullptr) {
      drainOutput();
      continue;
    }
    const auto frames = std::min(CHUNK_FRAMES, frameCount - frameOffset);
    const auto size = static_cast<size_t>(frames) * bytesPerFrame;
    const auto timeUs = static_cast<int64_t>(frameOffset) * Definitions::kMicrosPerSecond / SAMPLE_RATE;
    input->init(timeUs, static_cast<int>(size));
    const auto data = input->data();
    data->position(0);
    data->put(reinterpret_cast<const uint8_t*>(pcm.data() + static_cast<size_t>(frameOffset) * CHANNEL_COUNT), size);
    data->limit(size);
    encoder->queueInputBuffer(input);
    frameOffset += frames;
    drainOutput();
  }

  encoder->signalEndOfInputStream();
  guard = 0;
  while (!endOfStream && guard++ < 1000) {
    const auto previousCount = packets.size();
    drainOutput();
    if (endOfStream || packets.size() == previousCount) {
      // the encoder has no more pending output.
      if (packets.size() == previousCount && guard > 4) {
        break;
      }
    }
  }

  EXPECT_TRUE(formatReported);
  return packets;
}

} // namespace anonymous

TEST(WmfAudioCodec, FactorySupportsAac) {
  auto factory = CreateBuiltinAudioCodecFactory();
  ASSERT_NE(factory, nullptr);
  EXPECT_TRUE(factory->isSupportedCodec(AudioFormatType::kAudioFormatTypeMpeg4AAC, true));
  EXPECT_TRUE(factory->isSupportedCodec(AudioFormatType::kAudioFormatTypeMpeg4AAC, false));
#if defined(WINDOWS) || defined(WIN32)
  EXPECT_NE(factory->createDecoder(MimeTypes::AUDIO_AAC, false), nullptr);
  EXPECT_NE(factory->createEncoder(AudioFormatType::kAudioFormatTypeMpeg4AAC,
    CodecManufacturer::kSoftware), nullptr);
#endif
}

TEST(WmfAudioCodec, EncodeDecodeRoundTrip) {
  const auto pcm = generatePcm(SAMPLE_RATE, CHANNEL_COUNT, FRAME_COUNT);
  const auto bytesPerFrame = 2 * CHANNEL_COUNT;

  auto encoder = std::make_unique<WmfAudioEncoder>(AudioFormatType::kAudioFormatTypeMpeg4AAC);
  ASSERT_TRUE(encoder->configure(createPcmFormat()));
  encoder->start();

  std::shared_ptr<MediaFormat> encodedFormat;
  const auto packets = encodePcm(encoder.get(), pcm, FRAME_COUNT, bytesPerFrame, &encodedFormat);
  encoder->stop();
  encoder->release();

  ASSERT_FALSE(packets.empty()) << "no aac packet was produced by the encoder";
  ASSERT_NE(encodedFormat, nullptr);
  EXPECT_EQ(encodedFormat->getMimeType(), MimeTypes::AUDIO_AAC);

  // the encoder delay of the aac encoder adds a small amount of data, the duration of the encoded
  // stream is close to the duration of the input signal.
  int64_t totalBytes = 0;
  for (const auto& packet: packets) {
    totalBytes += static_cast<int64_t>(packet.data.size());
  }
  EXPECT_GT(totalBytes, 0);

  auto decoder = std::make_unique<WmfAudioDecoder>(false);
  ASSERT_TRUE(decoder->configure(encodedFormat));
  decoder->start();
  const auto decodedFrames = decodePackets(decoder.get(), packets);
  decoder->stop();
  decoder->release();

  EXPECT_GT(decodedFrames, 0);
  EXPECT_NEAR(static_cast<double>(decodedFrames), FRAME_COUNT, FRAME_COUNT * 0.15);
  ASSERT_NE(decoder->getOutputFormat(), nullptr);
  EXPECT_EQ(decoder->getOutputFormat()->getSampleRate(), SAMPLE_RATE);
  EXPECT_EQ(decoder->getOutputFormat()->getChannelCount(), CHANNEL_COUNT);
}

TEST(WmfAudioCodec, DecodeMpeg4AacFile) {
  const auto path = tempFilePath("audio-codec.m4a");
  {
    auto writer = AudioFileWriter::CreateWithUrl(path, MediaFileType::kMediaFileTypeMpeg4AAC,
      createAacFormat());
    ASSERT_NE(writer, nullptr);
    Error* error = nullptr;
    ASSERT_TRUE(writer->open(&error)) << (error != nullptr ? error->toString() : "");
    const auto pcm = generatePcm(SAMPLE_RATE, CHANNEL_COUNT, FRAME_COUNT);
    int offset = 0;
    while (offset < FRAME_COUNT) {
      const auto frames = std::min(CHUNK_FRAMES, FRAME_COUNT - offset);
      ASSERT_EQ(writer->writeFromPCMBuffer(
        const_cast<int16_t*>(pcm.data() + static_cast<size_t>(offset) * CHANNEL_COUNT), frames,
        &error), frames);
      offset += frames;
    }
    writer->close();
  }

  auto extractorFactory = CreateBuiltinExtractorFactory();
  auto extractor = extractorFactory->createExtractor(path);
  ASSERT_NE(extractor, nullptr);

  auto output = std::make_shared<CollectingExtractorOutput>();
  extractor->init(*output);

  AVMediaExtractor::PositionHolder holder;
  int guard = 0;
  while (extractor->read(holder) == AVMediaExtractor::PlatformResult::RESULT_CONTINUE
      && ++guard < 100000) {
  }

  const auto* track = output->firstTrackOfType(MediaType::kMediaTypeAudio);
  ASSERT_NE(track, nullptr);
  ASSERT_FALSE(track->packets.empty());
  ASSERT_NE(track->format, nullptr);

  auto decoder = CreateBuiltinAudioCodecFactory()->createDecoder(track->format->getMimeType(), false);
  ASSERT_NE(decoder, nullptr);
  ASSERT_TRUE(decoder->configure(track->format));
  decoder->start();
  const auto decodedFrames = decodePackets(decoder.get(), track->packets);
  decoder->stop();
  decoder->release();

  EXPECT_NEAR(static_cast<double>(decodedFrames), FRAME_COUNT, FRAME_COUNT * 0.15);
  extractor->clear();
}

TEST(WmfAudioCodec, RejectsUnsupportedCodec) {
  auto factory = CreateBuiltinAudioCodecFactory();
  EXPECT_FALSE(factory->isSupportedCodec(AudioFormatType::kAudioFormatTypeVorbis, true));
  EXPECT_EQ(factory->createEncoder(AudioFormatType::kAudioFormatTypeVorbis,
    CodecManufacturer::kSoftware), nullptr);
}

} // namespace coral::codec::test

#endif // WINDOWS
