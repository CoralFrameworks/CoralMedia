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
 * A demo of the video pipeline: synthetic frames are encoded into an h264 elementary stream by the
 * platform video encoder and the encoded packets are decoded by the platform video decoder
 * afterwards.
 *
 * usage: codec_video_pipeline [output h264 file]
 */

#include <codec/AudioFileWriter.h>
#include <codec/MediaCodecFactory.h>
#include <codec/MediaExtractor.h>

#include <media/PixelBuffer.h>
#include <minilog/log.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace coral;

namespace {

constexpr int WIDTH = 640;
constexpr int HEIGHT = 480;
constexpr int FRAME_COUNT = 30;
constexpr int FRAME_RATE = 30;

struct Packet {
  std::vector<uint8_t> data;
  int64_t timeUs = 0;
  bool keyFrame = false;
};

std::shared_ptr<I420PixelBuffer> generateFrame(const int index) {
  auto frame = std::make_shared<I420PixelBuffer>(WIDTH, HEIGHT);
  for (int row = 0; row < HEIGHT; ++row) {
    auto* rowData = frame->mutableDataY() + static_cast<ptrdiff_t>(row) * frame->strideY();
    for (int column = 0; column < WIDTH; ++column) {
      rowData[column] = static_cast<uint8_t>((column / 2 + row / 2 + index * 4) & 0xFF);
    }
  }
  for (int row = 0; row < frame->chromaHeight(); ++row) {
    auto* u = frame->mutableDataU() + static_cast<ptrdiff_t>(row) * frame->strideU();
    auto* v = frame->mutableDataV() + static_cast<ptrdiff_t>(row) * frame->strideV();
    for (int column = 0; column < frame->chromaWidth(); ++column) {
      u[column] = static_cast<uint8_t>(128 + ((column + index) & 0x1F));
      v[column] = static_cast<uint8_t>(128 - ((row + index) & 0x1F));
    }
  }
  return frame;
}

void fillFrame(const std::shared_ptr<AVPixelBuffer>& target, const std::shared_ptr<I420PixelBuffer>& source) {
  if (target->getFormat() == PixelFormat::kFormatI420) {
    auto i420 = target->asI420PixelBuffer();
    for (int row = 0; row < HEIGHT; ++row) {
      memcpy(i420->mutableDataY() + static_cast<ptrdiff_t>(row) * i420->strideY(),
        source->dataY() + static_cast<ptrdiff_t>(row) * source->strideY(), WIDTH);
    }
    for (int row = 0; row < source->chromaHeight(); ++row) {
      memcpy(i420->mutableDataU() + static_cast<ptrdiff_t>(row) * i420->strideU(),
        source->dataU() + static_cast<ptrdiff_t>(row) * source->strideU(), source->chromaWidth());
      memcpy(i420->mutableDataV() + static_cast<ptrdiff_t>(row) * i420->strideV(),
        source->dataV() + static_cast<ptrdiff_t>(row) * source->strideV(), source->chromaWidth());
    }
    return;
  }
  auto nv12 = target->asNV12PixelBuffer();
  for (int row = 0; row < HEIGHT; ++row) {
    memcpy(nv12->mutableDataY() + static_cast<ptrdiff_t>(row) * nv12->strideY(),
      source->dataY() + static_cast<ptrdiff_t>(row) * source->strideY(), WIDTH);
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

} // namespace anonymous

int main(int argc, char** argv) {
  std::string outputPath;
  if (argc > 1) {
    outputPath = argv[1];
  } else {
    outputPath = (std::filesystem::temp_directory_path() / "coral-video-pipeline.h264").string();
  }

  auto format = MediaFormat::CreateVideoFormat(MimeTypes::VIDEO_H264, WIDTH, HEIGHT);
  format->setVideoCodecType(VideoCodecType::kVideoCodecTypeH264);
  format->setFrameRate(FRAME_RATE);
  format->setAverageBitrate(2000000);

  auto encoder = CreateBuiltinVideoCodecFactory()->createVideoEncoder(
    VideoCodecType::kVideoCodecTypeH264, CodecManufacturer::kHardware, nullptr);
  if (encoder == nullptr) {
    printf("the platform does not provide an h264 encoder\n");
    return 1;
  }
  if (!encoder->configure(format, nullptr)) {
    printf("failed to configure the h264 encoder\n");
    return 1;
  }
  encoder->start();

  std::vector<Packet> packets;
  std::shared_ptr<MediaFormat> encodedFormat;

  const auto drain = [&]() {
    while (true) {
      auto packet = encoder->dequeueOutputBuffer();
      if (packet == nullptr) {
        return false;
      }
      if (packet->isFormatChanged()) {
        encodedFormat = packet->getFormat();
        continue;
      }
      if (packet->isEndOfStream()) {
        return true;
      }
      packets.push_back({ std::vector<uint8_t>(packet->array(), packet->array() + packet->size()),
        packet->timeUs(), packet->isKeyFrame() });
    }
  };

  for (int index = 0; index < FRAME_COUNT; ++index) {
    std::shared_ptr<AVPixelBuffer> input;
    int guard = 0;
    while (input == nullptr && guard++ < 1000) {
      input = encoder->dequeueInputBuffer();
      if (input == nullptr && drain()) {
        break;
      }
    }
    if (input == nullptr) {
      printf("the encoder did not accept the frame %d\n", index);
      break;
    }
    fillFrame(input, generateFrame(index));
    input->timeUs(static_cast<int64_t>(index) * Definitions::kMicrosPerSecond / FRAME_RATE);
    encoder->queueInputBuffer(input);
    drain();
  }
  encoder->signalEndOfInputStream();
  for (int guard = 0; guard < 1000 && !drain(); ++guard) {
  }
  encoder->stop();
  encoder->release();

  printf("encoded %d frames into %zu packets, %s\n", FRAME_COUNT, packets.size(),
    encodedFormat != nullptr ? encodedFormat->toString().c_str() : "");

  // writes the elementary stream for further inspection.
  {
    std::ofstream stream(outputPath, std::ios::binary);
    for (const auto& packet: packets) {
      stream.write(reinterpret_cast<const char*>(packet.data.data()),
        static_cast<std::streamsize>(packet.data.size()));
    }
    printf("wrote the h264 elementary stream into %s\n", outputPath.c_str());
  }

  auto decoder = CreateBuiltinVideoCodecFactory()->createVideoDecoder(
    VideoCodecType::kVideoCodecTypeH264, CodecManufacturer::kHardware);
  if (decoder == nullptr || encodedFormat == nullptr) {
    printf("the platform does not provide an h264 decoder\n");
    return 1;
  }
  if (!decoder->configure(encodedFormat, nullptr)) {
    printf("failed to configure the h264 decoder\n");
    return 1;
  }
  decoder->start();

  int64_t frames = 0;
  const auto drainDecoder = [&]() {
    while (true) {
      auto output = decoder->dequeueOutputBuffer();
      if (output == nullptr) {
        return false;
      }
      if (output->isEndOfStream()) {
        output->release();
        return true;
      }
      if (frames == 0) {
        printf("the decoded frame format is %s %dx%d\n",
          convertToText(output->getFormat()), output->width(), output->height());
      }
      frames++;
      output->release();
    }
  };

  for (const auto& packet: packets) {
    int guard = 0;
    while (guard++ < 1000) {
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
      if (drainDecoder()) {
        break;
      }
    }
    drainDecoder();
  }
  decoder->signalEndOfInputStream();
  for (int guard = 0; guard < 1000 && !drainDecoder(); ++guard) {
  }
  decoder->stop();
  decoder->release();

  printf("decoded %lld frames of %d encoded frames\n", static_cast<long long>(frames), FRAME_COUNT);
  return 0;
}
