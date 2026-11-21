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

#include <codec/MediaMetadataRetriever.h>

#include <win/WmfAudioDecoder.h>
#include <win/WmfAudioEncoder.h>
#include <win/WmfMediaMuxer.h>
#include <win/WmfVideoEncoder.h>

// The production audio file writer which is based on the muxer and the encoder of the platform.
#include "common/AudioMuxFileWriter.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <memory>
#include <vector>

namespace coral::codec::test {

namespace {

constexpr int AUDIO_SAMPLE_RATE = 48000;
constexpr int AUDIO_CHANNEL_COUNT = 2;
constexpr int AUDIO_FRAME_COUNT = AUDIO_SAMPLE_RATE;
constexpr int AUDIO_CHUNK_FRAMES = 1024;

constexpr int VIDEO_WIDTH = 320;
constexpr int VIDEO_HEIGHT = 240;
constexpr int VIDEO_FRAME_COUNT = 30;
constexpr int VIDEO_FRAME_RATE = 30;

std::shared_ptr<MediaFormat> createPcmFormat() {
  auto format = MediaFormat::CreateAudioFormat(MimeTypes::AUDIO_RAW, AUDIO_SAMPLE_RATE,
    AUDIO_CHANNEL_COUNT);
  format->setAudioFormatType(AudioFormatType::kAudioFormatTypeLinearPCM);
  format->setSampleFormat(AudioSampleFormat::kInt16);
  return format;
}

std::shared_ptr<MediaFormat> createAacFormat() {
  auto format = MediaFormat::CreateAudioFormat(MimeTypes::AUDIO_AAC, AUDIO_SAMPLE_RATE,
    AUDIO_CHANNEL_COUNT);
  format->setAudioFormatType(AudioFormatType::kAudioFormatTypeMpeg4AAC);
  format->setSampleFormat(AudioSampleFormat::kInt16);
  format->setAverageBitrate(128000);
  return format;
}

std::shared_ptr<MediaFormat> createH264Format() {
  auto format = MediaFormat::CreateVideoFormat(MimeTypes::VIDEO_H264, VIDEO_WIDTH, VIDEO_HEIGHT);
  format->setVideoCodecType(VideoCodecType::kVideoCodecTypeH264);
  format->setFrameRate(VIDEO_FRAME_RATE);
  format->setAverageBitrate(1000000);
  return format;
}

/**
 * Encodes the generated pcm signal, the encoded packets are reported by the out parameter.
 */
bool encodeAudio(std::vector<EncodedPacket>& packets, std::shared_ptr<MediaFormat>& outFormat) {
  WmfAudioEncoder encoder(AudioFormatType::kAudioFormatTypeMpeg4AAC);
  if (!encoder.configure(createPcmFormat())) {
    return false;
  }
  encoder.start();

  const auto pcm = generatePcm(AUDIO_SAMPLE_RATE, AUDIO_CHANNEL_COUNT, AUDIO_FRAME_COUNT);
  const auto bytesPerFrame = AUDIO_CHANNEL_COUNT * static_cast<int>(sizeof(int16_t));
  bool endOfStream = false;

  const auto drain = [&]() {
    while (true) {
      auto packet = encoder.dequeueOutputBuffer();
      if (packet == nullptr) {
        return;
      }
      if (packet->isFormatChanged()) {
        outFormat = packet->getFormat();
        continue;
      }
      if (packet->isEndOfStream()) {
        endOfStream = true;
        return;
      }
      if (packet->size() > 0) {
        packets.push_back({ std::vector<uint8_t>(packet->array(), packet->array() + packet->size()),
          packet->timeUs(), packet->isKeyFrame() });
      }
    }
  };

  int offset = 0;
  int guard = 0;
  while (offset < AUDIO_FRAME_COUNT && guard++ < 100000) {
    auto input = encoder.dequeueInputBuffer();
    if (input == nullptr) {
      drain();
      continue;
    }
    const auto frames = std::min(AUDIO_CHUNK_FRAMES, AUDIO_FRAME_COUNT - offset);
    const auto size = static_cast<size_t>(frames) * bytesPerFrame;
    input->init(static_cast<int64_t>(offset) * Definitions::kMicrosPerSecond / AUDIO_SAMPLE_RATE,
      static_cast<int>(size));
    const auto data = input->data();
    data->position(0);
    data->put(reinterpret_cast<const uint8_t*>(pcm.data()) +
        static_cast<size_t>(offset) * AUDIO_CHANNEL_COUNT, size);
    data->limit(size);
    encoder.queueInputBuffer(input);
    offset += frames;
    drain();
  }

  encoder.signalEndOfInputStream();
  guard = 0;
  while (!endOfStream && guard++ < 1000) {
    const auto count = packets.size();
    drain();
    if (endOfStream || (packets.size() == count && guard > 4)) {
      break;
    }
  }
  encoder.stop();
  encoder.release();
  return !packets.empty() && outFormat != nullptr;
}

/**
 * Encodes the generated video frames, the encoded packets are reported by the out parameter.
 */
bool encodeVideo(std::vector<EncodedPacket>& packets, std::shared_ptr<MediaFormat>& outFormat) {
  WmfVideoEncoder encoder(VideoCodecType::kVideoCodecTypeH264);
  if (!encoder.configure(createH264Format())) {
    return false;
  }
  encoder.start();

  bool endOfStream = false;
  const auto drain = [&]() {
    while (true) {
      auto packet = encoder.dequeueOutputBuffer();
      if (packet == nullptr) {
        return;
      }
      if (packet->isFormatChanged()) {
        outFormat = packet->getFormat();
        continue;
      }
      if (packet->isEndOfStream()) {
        endOfStream = true;
        return;
      }
      if (packet->size() > 0) {
        packets.push_back({ std::vector<uint8_t>(packet->array(), packet->array() + packet->size()),
          packet->timeUs(), packet->isKeyFrame() });
      }
    }
  };

  for (int index = 0; index < VIDEO_FRAME_COUNT; ++index) {
    std::shared_ptr<AVPixelBuffer> input;
    int guard = 0;
    while (input == nullptr && guard++ < 1000) {
      input = encoder.dequeueInputBuffer();
      if (input == nullptr) {
        drain();
      }
    }
    if (input == nullptr) {
      return false;
    }
    // Fills the encoder input frame with the generated i420 frame.
    const auto source = generateI420Frame(VIDEO_WIDTH, VIDEO_HEIGHT, index);
    if (input->getFormat() == PixelFormat::kFormatI420) {
      auto i420 = input->asI420PixelBuffer();
      for (int row = 0; row < VIDEO_HEIGHT; ++row) {
        memcpy(i420->mutableDataY() + static_cast<ptrdiff_t>(row) * i420->strideY(),
          source->dataY() + static_cast<ptrdiff_t>(row) * source->strideY(), VIDEO_WIDTH);
      }
      for (int row = 0; row < source->chromaHeight(); ++row) {
        memcpy(i420->mutableDataU() + static_cast<ptrdiff_t>(row) * i420->strideU(),
          source->dataU() + static_cast<ptrdiff_t>(row) * source->strideU(), source->chromaWidth());
        memcpy(i420->mutableDataV() + static_cast<ptrdiff_t>(row) * i420->strideV(),
          source->dataV() + static_cast<ptrdiff_t>(row) * source->strideV(), source->chromaWidth());
      }
    } else {
      auto nv12 = input->asNV12PixelBuffer();
      for (int row = 0; row < VIDEO_HEIGHT; ++row) {
        memcpy(nv12->mutableDataY() + static_cast<ptrdiff_t>(row) * nv12->strideY(),
          source->dataY() + static_cast<ptrdiff_t>(row) * source->strideY(), VIDEO_WIDTH);
      }
      for (int row = 0; row < source->chromaHeight(); ++row) {
        const auto* u = source->dataU() + static_cast<ptrdiff_t>(row) * source->strideU();
        const auto* v = source->dataV() + static_cast<ptrdiff_t>(row) * source->strideV();
        auto* uv = nv12->mutableDataUV() + static_cast<ptrdiff_t>(row) * nv12->strideUV();
        for (int column = 0; column < source->chromaWidth(); ++column) {
          uv[column * 2] = u[column];
          uv[column * 2 + 1] = v[column];
        }
      }
    }

    input->timeUs(static_cast<int64_t>(index) * Definitions::kMicrosPerSecond / VIDEO_FRAME_RATE);
    encoder.queueInputBuffer(input);
    drain();
  }

  encoder.signalEndOfInputStream();
  int guard = 0;
  while (!endOfStream && guard++ < 1000) {
    const auto count = packets.size();
    drain();
    if (endOfStream || (packets.size() == count && guard > 4)) {
      break;
    }
  }
  encoder.stop();
  encoder.release();
  return !packets.empty() && outFormat != nullptr;
}

/**
 * Writes the encoded packets of the track into the muxer.
 */
void writePackets(AVMediaMuxer* muxer, const std::vector<EncodedPacket>& packets,
  const int streamIndex, const MediaType mediaType) {
  for (const auto& packet: packets) {
    auto buffer = std::make_shared<AVPacketBuffer>(mediaType);
    buffer->ensureSpaceForWrite(packet.data.size());
    const auto data = buffer->data();
    data->position(0);
    data->put(packet.data.data(), packet.data.size());
    data->flip();
    buffer->timeUs(packet.timeUs);
    buffer->setStreamIndex(streamIndex);
    if (packet.keyFrame) {
      buffer->addFlag(AVBufferFlags::kKeyFrame);
    }
    muxer->writeFrame(buffer);
  }
}

/**
 * Writes the packets of the tracks into the muxer, the samples are handed over in the order of
 * their presentation time like the renderer does.
 */
void writePacketsInterleaved(AVMediaMuxer* muxer,
  const std::vector<EncodedPacket>& audioPackets, const int audioStreamIndex,
  const std::vector<EncodedPacket>& videoPackets, const int videoStreamIndex) {
  struct Pending {
    const EncodedPacket* packet;
    int streamIndex;
    MediaType mediaType;
  };
  std::vector<Pending> pending;
  pending.reserve(audioPackets.size() + videoPackets.size());
  for (const auto& packet: audioPackets) {
    pending.push_back({ &packet, audioStreamIndex, MediaType::kMediaTypeAudio });
  }
  for (const auto& packet: videoPackets) {
    pending.push_back({ &packet, videoStreamIndex, MediaType::kMediaTypeVideo });
  }
  std::stable_sort(pending.begin(), pending.end(),
    [](const Pending& lhs, const Pending& rhs) {
      return lhs.packet->timeUs < rhs.packet->timeUs;
    });

  for (const auto& item: pending) {
    auto buffer = std::make_shared<AVPacketBuffer>(item.mediaType);
    buffer->ensureSpaceForWrite(item.packet->data.size());
    const auto data = buffer->data();
    data->position(0);
    data->put(item.packet->data.data(), item.packet->data.size());
    data->flip();
    buffer->timeUs(item.packet->timeUs);
    buffer->setStreamIndex(item.streamIndex);
    if (item.packet->keyFrame) {
      buffer->addFlag(AVBufferFlags::kKeyFrame);
    }
    muxer->writeFrame(buffer);
  }
}

} // namespace anonymous

TEST(WmfMediaMuxer, FactorySupportsMpeg4) {
  auto factory = CreateBuiltinMuxerFactory();
  ASSERT_NE(factory, nullptr);
  EXPECT_TRUE(factory->isSupportMuxerFormat(MuxerFormat::kMuxerFormatMpeg4));
  EXPECT_FALSE(factory->isSupportMuxerFormat(MuxerFormat::kMuxerFormatWebm));
  EXPECT_NE(factory->createMuxer(), nullptr);

  WmfMediaMuxer muxer;
  EXPECT_FALSE(muxer.prepare(tempFilePath("muxer.webm"), MuxerFormat::kMuxerFormatWebm));
  EXPECT_EQ(muxer.getStatus(), AVMediaMuxer::Status::kMuxerStatusIdle);
}

TEST(WmfMediaMuxer, MuxesAudioAndVideoTracks) {
  const auto path = tempFilePath("muxer.m4v");

  std::vector<EncodedPacket> audioPackets;
  std::vector<EncodedPacket> videoPackets;
  std::shared_ptr<MediaFormat> audioFormat;
  std::shared_ptr<MediaFormat> videoFormat;
  ASSERT_TRUE(encodeAudio(audioPackets, audioFormat));
  ASSERT_TRUE(encodeVideo(videoPackets, videoFormat));


  {
    WmfMediaMuxer muxer;
    ASSERT_TRUE(muxer.prepare(path, MuxerFormat::kMuxerFormatMpeg4));
    EXPECT_EQ(muxer.getStatus(), AVMediaMuxer::Status::kMuxerStatusPrepared);

    const auto audioTrack = muxer.addTrack(audioFormat);
    const auto videoTrack = muxer.addTrack(videoFormat);
    ASSERT_GE(audioTrack, 0);
    ASSERT_GE(videoTrack, 0);
    EXPECT_NE(audioTrack, videoTrack);

    muxer.start();
    ASSERT_TRUE(muxer.isStarted());

    writePackets(&muxer, audioPackets, audioTrack, MediaType::kMediaTypeAudio);
    writePackets(&muxer, videoPackets, videoTrack, MediaType::kMediaTypeVideo);

    muxer.stop();
    EXPECT_EQ(muxer.getStatus(), AVMediaMuxer::Status::kMuxerStatusStopped);
    muxer.release();
    EXPECT_EQ(muxer.getStatus(), AVMediaMuxer::Status::kMuxerStatusIdle);
  }

  ASSERT_TRUE(std::filesystem::exists(path));
  EXPECT_GT(std::filesystem::file_size(path), 4096u);

  // the file which has been muxed by the platform is demuxed and decoded by the platform again.
  auto extractor = CreateBuiltinExtractorFactory()->createExtractor(path);
  ASSERT_NE(extractor, nullptr);
  auto output = std::make_shared<CollectingExtractorOutput>();
  extractor->init(*output);

  AVMediaExtractor::PositionHolder holder;
  int guard = 0;
  while (extractor->read(holder) == AVMediaExtractor::PlatformResult::RESULT_CONTINUE
      && ++guard < 100000) {
  }

  EXPECT_EQ(output->trackCount(), 2u);
  const auto* audioTrack = output->firstTrackOfType(MediaType::kMediaTypeAudio);
  const auto* videoTrack = output->firstTrackOfType(MediaType::kMediaTypeVideo);
  ASSERT_NE(audioTrack, nullptr);
  ASSERT_NE(videoTrack, nullptr);
  ASSERT_NE(audioTrack->format, nullptr);
  ASSERT_NE(videoTrack->format, nullptr);

  EXPECT_EQ(audioTrack->format->getMimeType(), MimeTypes::AUDIO_AAC);
  EXPECT_EQ(audioTrack->format->getSampleRate(), AUDIO_SAMPLE_RATE);
  EXPECT_EQ(audioTrack->format->getChannelCount(), AUDIO_CHANNEL_COUNT);
  EXPECT_EQ(videoTrack->format->getMimeType(), MimeTypes::VIDEO_H264);
  EXPECT_EQ(videoTrack->format->getWidth(), VIDEO_WIDTH);
  EXPECT_EQ(videoTrack->format->getHeight(), VIDEO_HEIGHT);
  EXPECT_NEAR(static_cast<double>(extractor->getDurationUs()), 1000000.0, 150000.0);

  // the audio of the muxed file is decoded again.
  auto audioDecoder = CreateBuiltinAudioCodecFactory()->createDecoder(
    audioTrack->format->getMimeType(), false);
  ASSERT_NE(audioDecoder, nullptr);
  ASSERT_TRUE(audioDecoder->configure(audioTrack->format));
  audioDecoder->start();
  const auto decodedAudioFrames = decodePackets(audioDecoder.get(), audioTrack->packets);
  audioDecoder->stop();
  audioDecoder->release();
  EXPECT_NEAR(static_cast<double>(decodedAudioFrames), AUDIO_FRAME_COUNT, AUDIO_FRAME_COUNT * 0.15);

  // the video of the muxed file is decoded again.
  auto videoDecoder = CreateBuiltinVideoCodecFactory()->createVideoDecoder(
    VideoCodecType::kVideoCodecTypeH264, CodecManufacturer::kHardware);
  ASSERT_NE(videoDecoder, nullptr);
  ASSERT_TRUE(videoDecoder->configure(videoTrack->format, nullptr));
  videoDecoder->start();

  int64_t decodedFrames = 0;
  const auto drainVideo = [&]() {
    while (true) {
      auto frame = videoDecoder->dequeueOutputBuffer();
      if (frame == nullptr) {
        return false;
      }
      if (frame->isEndOfStream()) {
        frame->release();
        return true;
      }
      decodedFrames++;
      frame->release();
    }
  };
  for (const auto& packet: videoTrack->packets) {
    int inputGuard = 0;
    while (inputGuard++ < 1000) {
      auto input = videoDecoder->dequeueInputBuffer();
      if (input != nullptr) {
        input->ensureSpaceForWrite(packet.data.size());
        const auto data = input->data();
        data->position(0);
        data->put(packet.data.data(), packet.data.size());
        data->flip();
        input->timeUs(packet.timeUs);
        if (packet.keyFrame) {
          input->addFlag(AVBufferFlags::kKeyFrame);
        }
        videoDecoder->queueInputBuffer(input);
        break;
      }
      if (drainVideo()) {
        break;
      }
    }
    drainVideo();
  }
  videoDecoder->signalEndOfInputStream();
  guard = 0;
  while (!drainVideo() && ++guard < 1000) {
  }
  videoDecoder->stop();
  videoDecoder->release();

  EXPECT_GE(decodedFrames, VIDEO_FRAME_COUNT - 2);
  extractor->clear();
}

TEST(WmfMediaMuxer, AudioMuxFileWriterUsesThePlatformMuxer) {
  const auto path = tempFilePath("audio-mux-writer.m4a");
  const auto pcm = generatePcm(AUDIO_SAMPLE_RATE, AUDIO_CHANNEL_COUNT, AUDIO_FRAME_COUNT);

  // The audio mux file writer encodes the samples with the platform encoder and muxes them with
  // the platform muxer.
  auto writer = AudioMuxFileWriter::sniff(path, MediaFileType::kMediaFileTypeMpeg4AAC,
    createAacFormat());
  ASSERT_NE(writer, nullptr) << "the platform does not provide the muxer or the aac encoder";

  Error* error = nullptr;
  ASSERT_TRUE(writer->open(&error)) << (error != nullptr ? error->toString() : "");

  int offset = 0;
  while (offset < AUDIO_FRAME_COUNT) {
    const auto frames = std::min(AUDIO_CHUNK_FRAMES, AUDIO_FRAME_COUNT - offset);
    ASSERT_EQ(writer->writeFromPCMBuffer(
      const_cast<int16_t*>(pcm.data()) + static_cast<size_t>(offset) * AUDIO_CHANNEL_COUNT, frames,
      &error), frames) << (error != nullptr ? error->toString() : "");
    offset += frames;
  }
  writer->close();
  writer.reset();

  ASSERT_TRUE(std::filesystem::exists(path));
  EXPECT_GT(std::filesystem::file_size(path), 4096u);

  auto reader = AudioFileReader::CreateWithUrl(path);
  ASSERT_NE(reader, nullptr);
  ASSERT_TRUE(reader->open(&error)) << (error != nullptr ? error->toString() : "");
  EXPECT_EQ(reader->getSampleRate(), AUDIO_SAMPLE_RATE);
  EXPECT_EQ(reader->getChannelCount(), AUDIO_CHANNEL_COUNT);
  const auto frames = readAllFrames(reader.get(), &error);
  reader->close();
  EXPECT_NEAR(static_cast<double>(frames), AUDIO_FRAME_COUNT, AUDIO_FRAME_COUNT * 0.15);
}

TEST(MetadataRetriever, ReadsTheTracksOfTheMuxedFile) {
  const auto path = tempFilePath("metadata.m4v");

  std::vector<EncodedPacket> audioPackets;
  std::vector<EncodedPacket> videoPackets;
  std::shared_ptr<MediaFormat> audioFormat;
  std::shared_ptr<MediaFormat> videoFormat;
  ASSERT_TRUE(encodeAudio(audioPackets, audioFormat));
  ASSERT_TRUE(encodeVideo(videoPackets, videoFormat));

  {
    WmfMediaMuxer muxer;
    ASSERT_TRUE(muxer.prepare(path, MuxerFormat::kMuxerFormatMpeg4));
    const auto audioTrack = muxer.addTrack(audioFormat);
    const auto videoTrack = muxer.addTrack(videoFormat);
    ASSERT_GE(audioTrack, 0);
    ASSERT_GE(videoTrack, 0);
    muxer.start();
    writePacketsInterleaved(&muxer, audioPackets, audioTrack, videoPackets, videoTrack);
    muxer.stop();
    muxer.release();
  }

  auto retriever = MediaMetadataRetriever::create(path);
  ASSERT_NE(retriever, nullptr);
  // The media holds the audio track and the video track that were muxed, and a container may declare
  // a metadata track as well (the MPEG-4 muxer of the platform writes the `meta` box), which the
  // retriever reports too: the tracks are looked up by type rather than by index.
  EXPECT_GE(retriever->getTrackCount(), 2);
  std::shared_ptr<MediaFormat> audioTrackFormat;
  std::shared_ptr<MediaFormat> videoTrackFormat;
  for (int index = 0; index < retriever->getTrackCount(); ++index) {
    const auto format = retriever->getTrackFormat(index);
    if (format == nullptr) {
      continue;
    }
    if (format->isAudio() && audioTrackFormat == nullptr) {
      audioTrackFormat = format;
    } else if (format->isVideo() && videoTrackFormat == nullptr) {
      videoTrackFormat = format;
    }
  }
  ASSERT_NE(audioTrackFormat, nullptr);
  ASSERT_NE(videoTrackFormat, nullptr);

  EXPECT_EQ(audioTrackFormat->getMimeType(), MimeTypes::AUDIO_AAC);
  EXPECT_EQ(audioTrackFormat->getSampleRate(), AUDIO_SAMPLE_RATE);
  EXPECT_EQ(audioTrackFormat->getChannelCount(), AUDIO_CHANNEL_COUNT);
  EXPECT_NEAR(static_cast<double>(audioTrackFormat->getDurationUs()), 1000000.0, 150000.0);

  EXPECT_EQ(videoTrackFormat->getMimeType(), MimeTypes::VIDEO_H264);
  EXPECT_EQ(videoTrackFormat->getWidth(), VIDEO_WIDTH);
  EXPECT_EQ(videoTrackFormat->getHeight(), VIDEO_HEIGHT);
  // The rate of the track is the one its sample table declares, which is what the composition of an
  // asset schedules its frames with.
  EXPECT_EQ(videoTrackFormat->getFrameRate(), VIDEO_FRAME_RATE);
  EXPECT_EQ(videoTrackFormat->getRotation(), 0);
  EXPECT_NE(audioTrackFormat->getTrackId(), videoTrackFormat->getTrackId());
  ASSERT_FALSE(videoTrackFormat->csdList().empty());
  EXPECT_FALSE(videoTrackFormat->csdList()[0].empty());

  // the formats which are reported by the retriever are usable by the decoders.
  auto videoDecoder = CreateBuiltinVideoCodecFactory()->createVideoDecoder(
    VideoCodecType::kVideoCodecTypeH264, CodecManufacturer::kHardware);
  ASSERT_NE(videoDecoder, nullptr);
  EXPECT_TRUE(videoDecoder->configure(videoTrackFormat, nullptr));
  videoDecoder->release();

  // an index which is out of the range is reported without a format.
  EXPECT_EQ(retriever->getTrackFormat(retriever->getTrackCount()), nullptr);
  EXPECT_EQ(retriever->getTrackFormat(-1), nullptr);
  retriever->cancelLoading();
}

TEST(MetadataRetriever, ReportsNoTrackOfAnUnsupportedMedia) {
  auto retriever = MediaMetadataRetriever::create(tempFilePath("not-a-media.bin"));
  ASSERT_NE(retriever, nullptr) << "the retriever must be created even if the file is unreadable";
  EXPECT_EQ(retriever->getTrackCount(), 0);
  EXPECT_EQ(retriever->getTrackFormat(0), nullptr);
}

} // namespace coral::codec::test

#endif // WINDOWS
