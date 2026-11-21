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

/**
 * A demo of the muxer and of the metadata retriever: a sine signal and synthetic video frames are
 * encoded with the platform codecs and muxed into an mpeg4 file by the platform muxer afterwards.
 * The tracks of the written file are reported by the metadata retriever and the file is demuxed and
 * decoded again.
 *
 * usage: codec_muxer [output file]
 */

#include <codec/MediaCodecFactory.h>
#include <codec/MediaExtractor.h>
#include <codec/MediaMetadataRetriever.h>
#include <codec/MediaMuxer.h>

#include <media/PixelBuffer.h>
#include <media/Utilities.h>
#include <minilog/log.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace coral;

namespace {

constexpr int AUDIO_SAMPLE_RATE = 48000;
constexpr int AUDIO_CHANNEL_COUNT = 2;
constexpr int AUDIO_FRAME_COUNT = AUDIO_SAMPLE_RATE;
constexpr int AUDIO_CHUNK_FRAMES = 1024;

constexpr int VIDEO_WIDTH = 640;
constexpr int VIDEO_HEIGHT = 480;
constexpr int VIDEO_FRAME_COUNT = 30;
constexpr int VIDEO_FRAME_RATE = 30;

constexpr double PI = 3.14159265358979323846;

struct Packet {
  std::vector<uint8_t> data;
  int64_t timeUs = 0;
  bool keyFrame = false;
};

/**
 * Encodes the audio signal, the encoded packets are reported by the out parameter.
 */
bool encodeAudio(std::vector<Packet>& packets, std::shared_ptr<MediaFormat>& outFormat) {
  auto inputFormat = MediaFormat::CreateAudioFormat(MimeTypes::AUDIO_RAW, AUDIO_SAMPLE_RATE,
    AUDIO_CHANNEL_COUNT);
  inputFormat->setAudioFormatType(AudioFormatType::kAudioFormatTypeLinearPCM);
  inputFormat->setSampleFormat(AudioSampleFormat::kInt16);

  auto encoder = CreateBuiltinAudioCodecFactory()->createEncoder(
    AudioFormatType::kAudioFormatTypeMpeg4AAC, CodecManufacturer::kHardware);
  if (encoder == nullptr || !encoder->configure(inputFormat)) {
    printf("the platform does not provide an aac encoder\n");
    return false;
  }
  encoder->start();

  const auto bytesPerFrame = AUDIO_CHANNEL_COUNT * static_cast<int>(sizeof(int16_t));
  std::vector<int16_t> chunk(static_cast<size_t>(AUDIO_CHUNK_FRAMES) * AUDIO_CHANNEL_COUNT);
  bool endOfStream = false;

  const auto drain = [&]() {
    while (true) {
      auto packet = encoder->dequeueOutputBuffer();
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
      packets.push_back({ std::vector<uint8_t>(packet->array(), packet->array() + packet->size()),
        packet->timeUs(), packet->isKeyFrame() });
    }
  };

  int offset = 0;
  while (offset < AUDIO_FRAME_COUNT) {
    auto input = encoder->dequeueInputBuffer();
    if (input == nullptr) {
      drain();
      continue;
    }
    const auto frames = std::min(AUDIO_CHUNK_FRAMES, AUDIO_FRAME_COUNT - offset);
    for (int frame = 0; frame < frames; ++frame) {
      const auto value = static_cast<int16_t>(
        std::sin(2.0 * PI * 440.0 * (offset + frame) / AUDIO_SAMPLE_RATE) * 12000.0);
      for (int channel = 0; channel < AUDIO_CHANNEL_COUNT; ++channel) {
        chunk[static_cast<size_t>(frame) * AUDIO_CHANNEL_COUNT + channel] = value;
      }
    }
    const auto size = static_cast<size_t>(frames) * bytesPerFrame;
    input->init(static_cast<int64_t>(offset) * Definitions::kMicrosPerSecond / AUDIO_SAMPLE_RATE,
      static_cast<int>(size));
    const auto data = input->data();
    data->position(0);
    data->put(reinterpret_cast<const uint8_t*>(chunk.data()), size);
    data->limit(size);
    encoder->queueInputBuffer(input);
    offset += frames;
    drain();
  }

  encoder->signalEndOfInputStream();
  for (int guard = 0; guard < 1000 && !endOfStream; ++guard) {
    const auto count = packets.size();
    drain();
    if (packets.size() == count && guard > 4) {
      break;
    }
  }
  encoder->stop();
  encoder->release();
  return !packets.empty() && outFormat != nullptr;
}

/**
 * Encodes the synthetic video frames, the encoded packets are reported by the out parameter.
 */
bool encodeVideo(std::vector<Packet>& packets, std::shared_ptr<MediaFormat>& outFormat) {
  auto inputFormat = MediaFormat::CreateVideoFormat(MimeTypes::VIDEO_H264, VIDEO_WIDTH,
    VIDEO_HEIGHT);
  inputFormat->setVideoCodecType(VideoCodecType::kVideoCodecTypeH264);
  inputFormat->setFrameRate(VIDEO_FRAME_RATE);
  inputFormat->setAverageBitrate(2000000);

  auto encoder = CreateBuiltinVideoCodecFactory()->createVideoEncoder(
    VideoCodecType::kVideoCodecTypeH264, CodecManufacturer::kHardware, nullptr);
  if (encoder == nullptr || !encoder->configure(inputFormat, nullptr)) {
    printf("the platform does not provide an h264 encoder\n");
    return false;
  }
  encoder->start();

  bool endOfStream = false;
  const auto drain = [&]() {
    while (true) {
      auto packet = encoder->dequeueOutputBuffer();
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
      packets.push_back({ std::vector<uint8_t>(packet->array(), packet->array() + packet->size()),
        packet->timeUs(), packet->isKeyFrame() });
    }
  };

  for (int index = 0; index < VIDEO_FRAME_COUNT; ++index) {
    std::shared_ptr<AVPixelBuffer> input;
    int inputGuard = 0;
    while (input == nullptr && inputGuard++ < 1000) {
      input = encoder->dequeueInputBuffer();
      if (input == nullptr) {
        drain();
      }
    }
    if (input == nullptr) {
      return false;
    }

    // The frame of the encoder input is filled with a moving gradient.
    if (input->getFormat() == PixelFormat::kFormatI420) {
      auto i420 = input->asI420PixelBuffer();
      for (int row = 0; row < VIDEO_HEIGHT; ++row) {
        auto* rowData = i420->mutableDataY() + static_cast<ptrdiff_t>(row) * i420->strideY();
        for (int column = 0; column < VIDEO_WIDTH; ++column) {
          rowData[column] = static_cast<uint8_t>((column / 2 + row / 2 + index * 4) & 0xFF);
        }
      }
      for (int row = 0; row < i420->chromaHeight(); ++row) {
        auto* u = i420->mutableDataU() + static_cast<ptrdiff_t>(row) * i420->strideU();
        auto* v = i420->mutableDataV() + static_cast<ptrdiff_t>(row) * i420->strideV();
        for (int column = 0; column < i420->chromaWidth(); ++column) {
          u[column] = static_cast<uint8_t>(128 + ((column + index) & 0x1F));
          v[column] = static_cast<uint8_t>(128 - ((row + index) & 0x1F));
        }
      }
    } else {
      auto nv12 = input->asNV12PixelBuffer();
      for (int row = 0; row < VIDEO_HEIGHT; ++row) {
        auto* rowData = nv12->mutableDataY() + static_cast<ptrdiff_t>(row) * nv12->strideY();
        for (int column = 0; column < VIDEO_WIDTH; ++column) {
          rowData[column] = static_cast<uint8_t>((column / 2 + row / 2 + index * 4) & 0xFF);
        }
      }
      for (int row = 0; row < nv12->chromaHeight(); ++row) {
        auto* uv = nv12->mutableDataUV() + static_cast<ptrdiff_t>(row) * nv12->strideUV();
        for (int column = 0; column < nv12->chromaWidth(); ++column) {
          uv[column * 2] = static_cast<uint8_t>(128 + ((column + index) & 0x1F));
          uv[column * 2 + 1] = static_cast<uint8_t>(128 - ((row + index) & 0x1F));
        }
      }
    }

    input->timeUs(static_cast<int64_t>(index) * Definitions::kMicrosPerSecond / VIDEO_FRAME_RATE);
    encoder->queueInputBuffer(input);
    drain();
  }

  encoder->signalEndOfInputStream();
  for (int guard = 0; guard < 1000 && !endOfStream; ++guard) {
    const auto count = packets.size();
    drain();
    if (packets.size() == count && guard > 4) {
      break;
    }
  }
  encoder->stop();
  encoder->release();
  return !packets.empty() && outFormat != nullptr;
}

/**
 * Writes the packets of a track into the muxer.
 */
void muxTrack(AVMediaMuxer* muxer, const std::vector<Packet>& packets, const int streamIndex,
  const MediaType mediaType) {
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

} // namespace anonymous

int main(int argc, char** argv) {
  std::string path;
  if (argc > 1) {
    path = argv[1];
  } else {
    path = (std::filesystem::temp_directory_path() / "coral-muxer.m4v").string();
  }
  std::error_code error;
  std::filesystem::remove(path, error);

  std::vector<Packet> audioPackets;
  std::vector<Packet> videoPackets;
  std::shared_ptr<MediaFormat> audioFormat;
  std::shared_ptr<MediaFormat> videoFormat;
  if (!encodeAudio(audioPackets, audioFormat) || !encodeVideo(videoPackets, videoFormat)) {
    return 1;
  }
  printf("encoded %zu audio packets and %zu video packets\n", audioPackets.size(),
    videoPackets.size());

  // muxes the encoded tracks into the container.
  auto muxer = CreateBuiltinMuxerFactory()->createMuxer();
  if (muxer == nullptr || !muxer->prepare(path, MuxerFormat::kMuxerFormatMpeg4)) {
    printf("the platform does not provide a muxer for the mpeg4 format\n");
    return 1;
  }
  const auto audioTrack = muxer->addTrack(audioFormat);
  const auto videoTrack = muxer->addTrack(videoFormat);
  if (audioTrack == AVMediaMuxer::kInvalidTrack || videoTrack == AVMediaMuxer::kInvalidTrack) {
    printf("failed to add the tracks to the muxer\n");
    return 1;
  }
  muxer->start();
  muxTrack(muxer.get(), audioPackets, audioTrack, MediaType::kMediaTypeAudio);
  muxTrack(muxer.get(), videoPackets, videoTrack, MediaType::kMediaTypeVideo);
  muxer->stop();
  muxer->release();
  printf("muxed the tracks into %s (%.1f kb)\n", path.c_str(),
    std::filesystem::file_size(path) / 1024.0);

  // reports the tracks of the written file.
  auto retriever = MediaMetadataRetriever::create(path);
  if (retriever == nullptr) {
    printf("the metadata retriever is not available\n");
    return 1;
  }
  const auto trackCount = retriever->getTrackCount();
  printf("the metadata retriever reports %d tracks\n", trackCount);
  for (int index = 0; index < trackCount; ++index) {
    const auto format = retriever->getTrackFormat(index);
    printf("  track %d: %s\n", index, format != nullptr ? format->toString().c_str() : "unsupported");
  }

  // demuxes and decodes the file again.
  auto extractor = CreateBuiltinExtractorFactory()->createExtractor(path);
  if (extractor == nullptr) {
    printf("the platform extractor cannot read the written file\n");
    return 1;
  }
  struct DemuxOutput final : AVMediaExtractor::Output {
    class TrackCollector final : public AVTrackOutput::Consumer {
    public:
      void setFormat(const std::shared_ptr<MediaFormat>& newFormat) override {
        format = newFormat;
      }

      void consume(const AVSample& sample) override {
        packets.push_back({ std::vector<uint8_t>(sample.array(), sample.array() + sample.size()),
          sample.timeUs, sample.isKeyFrame() });
      }

      std::shared_ptr<MediaFormat> format;
      std::vector<Packet> packets;
    };

    std::shared_ptr<AVTrackOutput> track(const int id, const MediaType type,
      AVTrackOutput::SampleConsumer) override {
      // The consumer the extractor offers is not used: the platform extractor delivers the samples
      // through the track output, which is bound to the collector of this output.
      auto output = std::make_shared<TrackCollector>();
      if (type == MediaType::kMediaTypeAudio) {
        audio = output;
      } else {
        video = output;
      }
      return std::make_shared<AVTrackOutput>(id, output);
    }

    std::shared_ptr<AVTrackOutput> track(const int id, AVTrackOutput::SampleConsumer consumer) override {
      // The media type of the track is not known, the samples are collected like the ones of the
      // tracks whose type was reported.
      return track(id, MediaType::kMediaTypeDefault, std::move(consumer));
    }

    void endTracks() override {
    }

    void seekMap(std::shared_ptr<AVSeekMap>) override {
    }

    std::shared_ptr<TrackCollector> audio;
    std::shared_ptr<TrackCollector> video;
  };

  auto output = std::make_shared<DemuxOutput>();
  extractor->init(*output);
  AVMediaExtractor::PositionHolder holder;
  int guard = 0;
  while (extractor->read(holder) == AVMediaExtractor::PlatformResult::RESULT_CONTINUE
      && ++guard < 100000) {
  }

  if (output->video != nullptr && output->video->format != nullptr) {
    auto decoder = CreateBuiltinVideoCodecFactory()->createVideoDecoder(
      VideoCodecType::kVideoCodecTypeH264, CodecManufacturer::kHardware);
    if (decoder != nullptr && decoder->configure(output->video->format, nullptr)) {
      decoder->start();
      int64_t frames = 0;
      const auto drainVideo = [&]() {
        while (true) {
          auto frame = decoder->dequeueOutputBuffer();
          if (frame == nullptr) {
            return false;
          }
          if (frame->isEndOfStream()) {
            frame->release();
            return true;
          }
          frames++;
          frame->release();
        }
      };
      for (const auto& packet: output->video->packets) {
        int inputGuard = 0;
        while (inputGuard++ < 1000) {
          auto input = decoder->dequeueInputBuffer();
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
            decoder->queueInputBuffer(input);
            break;
          }
          if (drainVideo()) {
            break;
          }
        }
        drainVideo();
      }
      decoder->signalEndOfInputStream();
      for (int pull = 0; pull < 1000 && !drainVideo(); ++pull) {
      }
      decoder->stop();
      decoder->release();
      printf("decoded %lld video frames of %d muxed frames\n", static_cast<long long>(frames),
        VIDEO_FRAME_COUNT);
    }
  }
  if (output->audio != nullptr && output->audio->format != nullptr) {
    printf("demuxed %zu audio packets of %s\n", output->audio->packets.size(),
      output->audio->format->getMimeType().c_str());
  }
  extractor->clear();
  return 0;
}
