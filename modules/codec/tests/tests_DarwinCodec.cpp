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

#if defined(__APPLE__)

#include <codec/AudioFileReader.h>
#include <codec/AudioFileWriter.h>
#include <codec/HardwareVideo.h>
#include <codec/MediaCodecFactory.h>
#include <codec/MediaExtractor.h>
#include <codec/MediaMetadataRetriever.h>
#include <codec/MediaMuxer.h>

#include <media/PixelBufferHandle.h>
#include <media/PixelBufferIOSurface.h>

#include <darwin/DarwinMediaUtils.h>

#include <CoreFoundation/CoreFoundation.h>
#include <CoreVideo/CoreVideo.h>

#include <climits>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace coral::codec::test {

namespace {

constexpr int AUDIO_SAMPLE_RATE = 48000;
constexpr int AUDIO_CHANNEL_COUNT = 2;
constexpr int AUDIO_FRAME_COUNT = AUDIO_SAMPLE_RATE;
constexpr int AUDIO_CHUNK_FRAMES = 1024;
constexpr int VIDEO_WIDTH = 640;
constexpr int VIDEO_HEIGHT = 480;
constexpr int VIDEO_FRAME_COUNT = 30;
constexpr int VIDEO_FRAME_RATE = 30;

/**
 * A decode target that requests the zero-copy output of the platform. VideoToolbox does not need a
 * device to allocate its IOSurface backed pixel buffers, so the surface names none.
 */
class TestHardwareVideoSurface final : public HardwareVideoSurface {
public:
  [[nodiscard]] void* getInstance() const override {
    return nullptr;
  }

  [[nodiscard]] void* getNativeDevice() const override {
    return nullptr;
  }

  [[nodiscard]] bool isValid() const override {
    return true;
  }

  void release() override {
  }
};

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

std::shared_ptr<MediaFormat> createVideoFormat(const VideoCodecType codecType =
    VideoCodecType::kVideoCodecTypeH264) {
  const auto mimeType = codecType == VideoCodecType::kVideoCodecTypeHevc
                          ? MimeTypes::VIDEO_H265
                          : MimeTypes::VIDEO_H264;
  auto format = MediaFormat::CreateVideoFormat(mimeType, VIDEO_WIDTH, VIDEO_HEIGHT);
  format->setVideoCodecType(codecType);
  format->setFrameRate(VIDEO_FRAME_RATE);
  format->setAverageBitrate(2000000);
  return format;
}

/**
 * A hardware frame the encoder consumes without a copy: the platform pixel buffer of a producer.
 */
class TestCVPixelBufferFrame final : public HardwarePixelBuffer,
                                     public std::enable_shared_from_this<TestCVPixelBufferFrame> {
public:
  TestCVPixelBufferFrame(CVPixelBufferRef pixelBuffer, const int width, const int height)
    : mPixelBuffer(pixelBuffer), mWidth(width), mHeight(height) {
    if (mPixelBuffer != nullptr) {
      CVPixelBufferRetain(mPixelBuffer);
    }
  }

  ~TestCVPixelBufferFrame() override {
    if (mPixelBuffer != nullptr) {
      CVPixelBufferRelease(mPixelBuffer);
      mPixelBuffer = nullptr;
    }
  }

  [[nodiscard]] PixelFormat getFormat() const override {
    return PixelFormat::kFormatNV12;
  }

  [[nodiscard]] int width() const override {
    return mWidth;
  }

  [[nodiscard]] int height() const override {
    return mHeight;
  }

  [[nodiscard]] void* getNativeTexture() const override {
    return mPixelBuffer;
  }

  [[nodiscard]] uint32_t getSubresource() const override {
    return 0;
  }

  [[nodiscard]] TextureFormat getTextureFormat() const override {
    return TextureFormat::kNv12;
  }

  void release() override {
  }

  std::shared_ptr<AVPixelBuffer> asPixelBuffer() override {
    return shared_from_this();
  }

private:
  CVPixelBufferRef mPixelBuffer = nullptr;
  int mWidth = 0;
  int mHeight = 0;
};

/** Fills one synthetic frame into whichever layout the encoder's input buffer uses. */
void fillTestFrame(const std::shared_ptr<AVPixelBuffer>& frame, const int index) {
  const auto width = frame->width();
  const auto height = frame->height();
  if (const auto nv12 = frame->asNV12PixelBuffer(); nv12 != nullptr) {
    for (int row = 0; row < height; ++row) {
      auto* data = nv12->mutableDataY() + static_cast<ptrdiff_t>(row) * nv12->strideY();
      for (int column = 0; column < width; ++column) {
        data[column] = static_cast<uint8_t>((column / 2 + row / 2 + index * 4) & 0xFF);
      }
    }
    for (int row = 0; row < nv12->chromaHeight(); ++row) {
      auto* uv = nv12->mutableDataUV() + static_cast<ptrdiff_t>(row) * nv12->strideUV();
      for (int column = 0; column < nv12->chromaWidth(); ++column) {
        uv[column * 2] = static_cast<uint8_t>(128 + ((column + index) & 0x1F));
        uv[column * 2 + 1] = static_cast<uint8_t>(128 - ((row + index) & 0x1F));
      }
    }
    return;
  }
  if (const auto i420 = frame->asI420PixelBuffer(); i420 != nullptr) {
    for (int row = 0; row < height; ++row) {
      auto* data = i420->mutableDataY() + static_cast<ptrdiff_t>(row) * i420->strideY();
      for (int column = 0; column < width; ++column) {
        data[column] = static_cast<uint8_t>((column / 2 + row / 2 + index * 4) & 0xFF);
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
  }
}

struct EncodedAudio {
  std::vector<EncodedPacket> packets;
  std::shared_ptr<MediaFormat> format;
};

EncodedAudio encodeAudio(const int frameCount = AUDIO_FRAME_COUNT) {
  EncodedAudio result;
  auto encoder = CreateBuiltinAudioCodecFactory()->createEncoder(
    AudioFormatType::kAudioFormatTypeMpeg4AAC, CodecManufacturer::kHardware);
  if (encoder == nullptr || !encoder->configure(createPcmFormat())) {
    return result;
  }
  encoder->start();

  const auto pcm = generatePcm(AUDIO_SAMPLE_RATE, AUDIO_CHANNEL_COUNT, frameCount);
  const auto bytesPerFrame = static_cast<size_t>(AUDIO_CHANNEL_COUNT) * sizeof(int16_t);

  bool endOfStream = false;
  const auto drain = [&]() {
    while (true) {
      auto packet = encoder->dequeueOutputBuffer();
      if (packet == nullptr) {
        return;
      }
      if (packet->isFormatChanged()) {
        result.format = packet->getFormat();
        continue;
      }
      if (packet->isEndOfStream()) {
        endOfStream = true;
        return;
      }
      if (packet->size() > 0) {
        result.packets.push_back({ std::vector<uint8_t>(packet->array(),
                                   packet->array() + packet->size()),
          packet->timeUs(), packet->isKeyFrame() });
      }
    }
  };

  int offset = 0;
  int guard = 0;
  while (offset < frameCount && guard++ < 100000) {
    auto input = encoder->dequeueInputBuffer();
    if (input == nullptr) {
      drain();
      continue;
    }
    const auto frames = std::min(AUDIO_CHUNK_FRAMES, frameCount - offset);
    const auto size = static_cast<size_t>(frames) * bytesPerFrame;
    const auto timeUs = static_cast<int64_t>(offset) * Definitions::kMicrosPerSecond /
                        AUDIO_SAMPLE_RATE;
    input->init(timeUs, static_cast<int>(size));
    const auto data = input->data();
    data->position(0);
    data->put(reinterpret_cast<const uint8_t*>(pcm.data() +
      static_cast<size_t>(offset) * AUDIO_CHANNEL_COUNT), size);
    data->limit(size);
    encoder->queueInputBuffer(input);
    offset += frames;
    drain();
  }
  encoder->signalEndOfInputStream();
  guard = 0;
  while (!endOfStream && guard++ < 1000) {
    drain();
  }
  encoder->stop();
  encoder->release();
  return result;
}

struct EncodedVideo {
  std::vector<EncodedPacket> packets;
  std::shared_ptr<MediaFormat> format;
};

EncodedVideo encodeVideo(const VideoCodecType codecType = VideoCodecType::kVideoCodecTypeH264,
    const int frameCount = VIDEO_FRAME_COUNT) {
  EncodedVideo result;
  auto encoder = CreateBuiltinVideoCodecFactory()->createVideoEncoder(codecType,
    CodecManufacturer::kHardware, nullptr);
  if (encoder == nullptr || !encoder->configure(createVideoFormat(codecType), nullptr)) {
    return result;
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
        result.format = packet->getFormat();
        continue;
      }
      if (packet->isEndOfStream()) {
        endOfStream = true;
        return;
      }
      if (packet->size() > 0) {
        result.packets.push_back({ std::vector<uint8_t>(packet->array(),
                                   packet->array() + packet->size()),
          packet->timeUs(), packet->isKeyFrame() });
      }
    }
  };

  for (int index = 0; index < frameCount; ++index) {
    std::shared_ptr<AVPixelBuffer> frame;
    int guard = 0;
    while (frame == nullptr && guard++ < 1000) {
      frame = encoder->dequeueInputBuffer();
      if (frame == nullptr) {
        drain();
      }
    }
    if (frame == nullptr) {
      break;
    }
    fillTestFrame(frame, index);
    frame->timeUs(static_cast<int64_t>(index) * Definitions::kMicrosPerSecond / VIDEO_FRAME_RATE);
    encoder->queueInputBuffer(frame);
    drain();
  }
  encoder->signalEndOfInputStream();
  int guard = 0;
  while (!endOfStream && guard++ < 1000) {
    drain();
  }
  encoder->stop();
  encoder->release();
  return result;
}

struct DecodedVideo {
  int64_t frames = 0;
  int width = 0;
  int height = 0;
  int hardwareFrames = 0;
  int ioSurfaceFrames = 0;
  PixelFormat format = PixelFormat::kFormatInvalid;
  ColorInfo colorInfo;
};

DecodedVideo decodeVideo(const std::shared_ptr<MediaFormat>& format,
    const std::vector<EncodedPacket>& packets, const std::shared_ptr<VideoSurface>& surface = nullptr,
    const VideoCodecType codecType = VideoCodecType::kVideoCodecTypeH264) {
  DecodedVideo result;
  if (format == nullptr) {
    return result;
  }
  auto decoder = CreateBuiltinVideoCodecFactory()->createVideoDecoder(codecType,
    CodecManufacturer::kHardware);
  if (decoder == nullptr || !decoder->configure(format, surface)) {
    return result;
  }
  decoder->start();

  bool endOfStream = false;
  const auto drain = [&]() {
    while (true) {
      auto frame = decoder->dequeueOutputBuffer();
      if (frame == nullptr) {
        return;
      }
      if (frame->isEndOfStream()) {
        endOfStream = true;
        frame->release();
        return;
      }
      ++result.frames;
      result.width = frame->width();
      result.height = frame->height();
      result.format = frame->getFormat();
      result.colorInfo = frame->getColorInfo();
      if (const auto hardware = std::dynamic_pointer_cast<HardwarePixelBuffer>(frame);
          hardware != nullptr) {
        ++result.hardwareFrames;
        auto* pixelBuffer = static_cast<CVPixelBufferRef>(hardware->getNativeTexture());
        if (pixelBuffer != nullptr && CVPixelBufferGetIOSurface(pixelBuffer) != nullptr) {
          ++result.ioSurfaceFrames;
        }
        if (auto handle = hardware->asPixelBufferHandle(); handle != nullptr) {
          EXPECT_EQ(handle->storage()->info().storage, PixelBufferStorageType::IOSurface);
          void* nativeHandle = nullptr;
          uint32_t subresource = 0;
          EXPECT_TRUE(handle->nativeHandle(PixelBufferNativeHandleKind::IOSurface, &nativeHandle,
            &subresource));
          EXPECT_NE(nativeHandle, nullptr);
          PixelBufferPlane planes[8]{};
          uint32_t planeCount = 0;
          EXPECT_TRUE(handle->lock(PixelBufferLockMode::Read, planes, &planeCount));
          EXPECT_EQ(planeCount, 2u);
          EXPECT_NE(planes[0].data, nullptr);
          EXPECT_NE(planes[1].data, nullptr);
          handle->unlock();
          handle->release();
        }
      }
      frame->release();
    }
  };

  for (const auto& packet : packets) {
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
      drain();
    }
    drain();
  }
  decoder->signalEndOfInputStream();
  int guard = 0;
  while (!endOfStream && guard++ < 1000) {
    drain();
  }
  decoder->stop();
  decoder->release();
  return result;
}

/** The resources of the main bundle, i.e. the directory of the executable of a command line test. */
std::string bundleResourceDirectory() {
  CFBundleRef bundle = CFBundleGetMainBundle();
  CFURLRef url = CFBundleCopyResourcesDirectoryURL(bundle);
  char buffer[PATH_MAX] = {};
  const bool ok = url != nullptr &&
                  CFURLGetFileSystemRepresentation(url, true,
                    reinterpret_cast<UInt8*>(buffer), sizeof(buffer)) != 0;
  if (url != nullptr) {
    CFRelease(url);
  }
  return ok ? std::string(buffer) : std::string();
}

/** Removes a fixture written to the resources of the test when the test ends. */
struct BundleFixtureCleanup {
  std::filesystem::path path;

  ~BundleFixtureCleanup() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
};

/** Reads every track of the platform extractor into the collector. */
std::shared_ptr<CollectingExtractorOutput> extractAll(const std::string& path) {
  auto extractor = CreateBuiltinExtractorFactory()->createExtractor(path);
  if (extractor == nullptr) {
    return nullptr;
  }
  auto output = std::make_shared<CollectingExtractorOutput>();
  extractor->init(*output);
  AVMediaExtractor::PositionHolder holder;
  int guard = 0;
  while (extractor->read(holder) == AVMediaExtractor::PlatformResult::RESULT_CONTINUE &&
         ++guard < 200000) {
  }
  return output;
}

} // namespace anonymous

// ==================================================================================================
// Bitstream helpers
// ==================================================================================================

TEST(DarwinMediaUtils, DoesNotMistakeASmallLengthPrefixedFrameForAnnexB) {
  // A NAL unit of 300 bytes is prefixed by 00 00 01 2C, whose first three bytes are the ones of a
  // three byte Annex B start code. The whole buffer is a valid sequence of length prefixed units, so
  // it must stay length prefixed, and only a buffer that is not must be read as Annex B.
  std::vector<uint8_t> payload(300);
  for (size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<uint8_t>(0x41 + (i % 16));
  }
  ASSERT_EQ(payload.size(), 300u);
  std::vector<uint8_t> avcc = { 0x00, 0x00, 0x01, 0x2C };
  avcc.insert(avcc.end(), payload.begin(), payload.end());
  EXPECT_EQ(avcc.size(), 304u);

  const auto lengthPrefixed = darwin::toLengthPrefixed(avcc.data(), avcc.size(), 4);
  EXPECT_EQ(lengthPrefixed, avcc);

  const auto annexB = darwin::toAnnexB(avcc.data(), avcc.size(), 4);
  ASSERT_EQ(annexB.size(), avcc.size());
  EXPECT_EQ(std::vector<uint8_t>(annexB.begin(), annexB.begin() + 4),
    (std::vector<uint8_t>{ 0x00, 0x00, 0x00, 0x01 }));
  EXPECT_EQ(std::vector<uint8_t>(annexB.begin() + 4, annexB.end()), payload);

  // The genuine Annex B form converts back to the length prefixed form.
  std::vector<uint8_t> genuineAnnexB = { 0x00, 0x00, 0x00, 0x01 };
  genuineAnnexB.insert(genuineAnnexB.end(), payload.begin(), payload.end());
  const auto converted = darwin::toLengthPrefixed(genuineAnnexB.data(), genuineAnnexB.size(), 4);
  EXPECT_EQ(converted, avcc);
}

TEST(DarwinMediaUtils, ParsesLengthPrefixedParameterSets) {
  const std::vector<uint8_t> sps = { 0x67, 0x64, 0x00, 0x1E, 0xAC, 0xD9, 0x40 };
  const std::vector<uint8_t> pps = { 0x68, 0xEB, 0xE3, 0xCB };
  const auto buildBlob = [&](const int lengthSize) {
    std::vector<uint8_t> blob;
    const auto append = [&blob, lengthSize](const std::vector<uint8_t>& nal) {
      for (int i = lengthSize - 1; i >= 0; --i) {
        blob.push_back(static_cast<uint8_t>((nal.size() >> (i * 8)) & 0xFF));
      }
      blob.insert(blob.end(), nal.begin(), nal.end());
    };
    append(sps);
    append(pps);
    return blob;
  };

  for (const int lengthSize : { 4, 2 }) {
    auto format = MediaFormat::CreateVideoFormat(MimeTypes::VIDEO_H264, 640, 480);
    format->setVideoCodecType(VideoCodecType::kVideoCodecTypeH264);
    format->setCodecSpecificData(0, buildBlob(lengthSize));
    const auto parsed = darwin::h264ParameterSets(*format);
    ASSERT_EQ(parsed.size(), 2u) << "length size " << lengthSize;
    EXPECT_EQ(parsed[0], sps);
    EXPECT_EQ(parsed[1], pps);
  }
}

TEST(DarwinMediaUtils, ParsesTheHvcCRecordOfAnHevcTrack) {
  // The NAL unit headers name the types: 32 is VPS, 33 SPS, 34 PPS and 39 a prefix SEI.
  const std::vector<std::vector<uint8_t>> parameterSets = {
    { 0x40, 0x01, 0x0C, 0x01, 0xFF, 0xFF },
    { 0x42, 0x01, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0xB0 },
    { 0x44, 0x01, 0xC0, 0x72, 0x4C, 0x14, 0xC9 },
    { 0x4E, 0x01, 0x05, 0x20, 0x00, 0x01 },
  };
  const auto record = darwin::hvcCFromParameterSets(parameterSets);
  ASSERT_FALSE(record.empty());
  EXPECT_EQ(record[0], 1);

  auto format = MediaFormat::CreateVideoFormat(MimeTypes::VIDEO_H265, 1920, 1080);
  format->setVideoCodecType(VideoCodecType::kVideoCodecTypeHevc);
  format->setCodecSpecificData(0, record);
  const auto parsed = darwin::hevcParameterSets(*format);
  ASSERT_EQ(parsed.size(), parameterSets.size());
  for (size_t i = 0; i < parsed.size(); ++i) {
    EXPECT_EQ(parsed[i], parameterSets[i]);
  }

  // A description created from real parameter sets carries the same arrays beside the color
  // extensions; the encoder and the muxer tests exercise that path with a decodable stream.
}

// ==================================================================================================
// Audio codec
// ==================================================================================================

TEST(DarwinAudioCodec, FactorySupportsAac) {
  auto factory = CreateBuiltinAudioCodecFactory();
  ASSERT_NE(factory, nullptr);
  EXPECT_TRUE(factory->isSupportedCodec(AudioFormatType::kAudioFormatTypeMpeg4AAC, true));
  EXPECT_TRUE(factory->isSupportedCodec(AudioFormatType::kAudioFormatTypeMpeg4AAC, false));
  EXPECT_NE(factory->createDecoder(MimeTypes::AUDIO_AAC, false), nullptr);
  EXPECT_NE(factory->createEncoder(AudioFormatType::kAudioFormatTypeMpeg4AAC,
    CodecManufacturer::kHardware), nullptr);
  EXPECT_FALSE(factory->isSupportedCodec(AudioFormatType::kAudioFormatTypeVorbis, true));
  EXPECT_EQ(factory->createEncoder(AudioFormatType::kAudioFormatTypeVorbis,
    CodecManufacturer::kSoftware), nullptr);
}

TEST(DarwinAudioCodec, EncodeDecodeRoundTrip) {
  const auto encoded = encodeAudio();
  ASSERT_FALSE(encoded.packets.empty()) << "the platform AAC encoder produced no packet";
  ASSERT_NE(encoded.format, nullptr);
  EXPECT_EQ(encoded.format->getMimeType(), MimeTypes::AUDIO_AAC);
  EXPECT_FALSE(encoded.format->csdList().empty());

  auto decoder = CreateBuiltinAudioCodecFactory()->createDecoder(MimeTypes::AUDIO_AAC, false);
  ASSERT_NE(decoder, nullptr);
  ASSERT_TRUE(decoder->configure(encoded.format));
  decoder->start();
  const auto frames = decodePackets(decoder.get(), encoded.packets);
  decoder->stop();
  decoder->release();

  EXPECT_NEAR(static_cast<double>(frames), AUDIO_FRAME_COUNT, AUDIO_FRAME_COUNT * 0.15);
}

// ==================================================================================================
// Video codec
// ==================================================================================================

TEST(DarwinVideoCodec, FactorySupportsH264) {
  auto factory = CreateBuiltinVideoCodecFactory();
  ASSERT_NE(factory, nullptr);
  EXPECT_TRUE(factory->isSupportedCodec(VideoCodecType::kVideoCodecTypeH264, true));
  EXPECT_TRUE(factory->isSupportedCodec(VideoCodecType::kVideoCodecTypeH264, false));
  EXPECT_NE(factory->createVideoDecoder(VideoCodecType::kVideoCodecTypeH264,
    CodecManufacturer::kHardware), nullptr);
  EXPECT_NE(factory->createVideoEncoder(VideoCodecType::kVideoCodecTypeH264,
    CodecManufacturer::kHardware, nullptr), nullptr);
  EXPECT_FALSE(factory->isSupportedCodec(VideoCodecType::kVideoCodecTypeVP8, true));
  EXPECT_EQ(factory->createVideoDecoder(VideoCodecType::kVideoCodecTypeVP8,
    CodecManufacturer::kHardware), nullptr);
}

TEST(DarwinVideoCodec, EncodeDecodeRoundTrip) {
  const auto encoded = encodeVideo();
  ASSERT_FALSE(encoded.packets.empty()) << "the platform H.264 encoder produced no packet";
  ASSERT_NE(encoded.format, nullptr);
  EXPECT_EQ(encoded.format->getMimeType(), MimeTypes::VIDEO_H264);
  EXPECT_EQ(encoded.format->getWidth(), VIDEO_WIDTH);
  EXPECT_EQ(encoded.format->getHeight(), VIDEO_HEIGHT);
  EXPECT_FALSE(encoded.format->csdList().empty());
  EXPECT_GT(encoded.packets.front().data.size(), 4u);

  const auto decoded = decodeVideo(encoded.format, encoded.packets);
  EXPECT_GE(decoded.frames, VIDEO_FRAME_COUNT - 2);
  EXPECT_EQ(decoded.width, VIDEO_WIDTH);
  EXPECT_EQ(decoded.height, VIDEO_HEIGHT);
  EXPECT_EQ(decoded.format, PixelFormat::kFormatNV12);
  EXPECT_EQ(decoded.hardwareFrames, 0) << "a decoder without a hardware surface must copy to CPU";
}

TEST(DarwinVideoCodec, EncodeDecodeHevcRoundTrip) {
  auto factory = CreateBuiltinVideoCodecFactory();
  ASSERT_NE(factory, nullptr);
  if (!factory->isSupportedCodec(VideoCodecType::kVideoCodecTypeHevc, true) ||
      !factory->isSupportedCodec(VideoCodecType::kVideoCodecTypeHevc, false)) {
    GTEST_SKIP() << "the platform has no HEVC codec";
  }
  const auto encoded = encodeVideo(VideoCodecType::kVideoCodecTypeHevc, 10);
  if (encoded.packets.empty()) {
    GTEST_SKIP() << "the platform HEVC encoder is not available in this session";
  }
  ASSERT_NE(encoded.format, nullptr);
  EXPECT_EQ(encoded.format->getMimeType(), MimeTypes::VIDEO_H265);
  EXPECT_FALSE(encoded.format->csdList().empty());
  EXPECT_EQ(encoded.format->csdList()[0].front(), 1) << "the HEVC codec data is an hvcC record";

  const auto decoded = decodeVideo(encoded.format, encoded.packets, nullptr,
    VideoCodecType::kVideoCodecTypeHevc);
  EXPECT_GE(decoded.frames, 8);
  EXPECT_EQ(decoded.width, VIDEO_WIDTH);
  EXPECT_EQ(decoded.height, VIDEO_HEIGHT);
}

TEST(DarwinVideoCodec, AcceptsAZeroCopyPixelBufferFrame) {
  auto factory = CreateBuiltinVideoCodecFactory();
  ASSERT_NE(factory, nullptr);
  auto encoder = factory->createVideoEncoder(VideoCodecType::kVideoCodecTypeH264,
    CodecManufacturer::kHardware, nullptr);
  ASSERT_NE(encoder, nullptr);
  ASSERT_TRUE(encoder->configure(createVideoFormat(), nullptr));
  encoder->start();

  std::vector<EncodedPacket> packets;
  std::shared_ptr<MediaFormat> format;
  bool endOfStream = false;
  const auto drain = [&]() {
    while (true) {
      auto packet = encoder->dequeueOutputBuffer();
      if (packet == nullptr) {
        return;
      }
      if (packet->isFormatChanged()) {
        format = packet->getFormat();
        continue;
      }
      if (packet->isEndOfStream()) {
        endOfStream = true;
        return;
      }
      if (packet->size() > 0) {
        packets.push_back({ std::vector<uint8_t>(packet->array(),
                             packet->array() + packet->size()),
          packet->timeUs(), packet->isKeyFrame() });
      }
    }
  };

  for (int index = 0; index < 10; ++index) {
    // The encoder is asked for its input buffer first, as every caller of the interface does, but
    // the frame that is queued is the producer's pixel buffer rather than that buffer.
    std::shared_ptr<AVPixelBuffer> ignored;
    int guard = 0;
    while (ignored == nullptr && guard++ < 100) {
      ignored = encoder->dequeueInputBuffer();
      if (ignored == nullptr) {
        drain();
      }
    }
    ASSERT_NE(ignored, nullptr);

    // The attributes are the ones a producer of a decodable frame uses; the pixel buffer only has to
    // hold the 4:2:0 planes for the encoder, which reads it in place.
    CVPixelBufferRef pixelBuffer = nullptr;
    ASSERT_EQ(CVPixelBufferCreate(kCFAllocatorDefault, VIDEO_WIDTH, VIDEO_HEIGHT,
      kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, nullptr, &pixelBuffer), kCVReturnSuccess);
    ASSERT_NE(pixelBuffer, nullptr);
    CVPixelBufferLockBaseAddress(pixelBuffer, 0);
    auto* luma = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 0));
    const auto lumaStride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 0);
    auto* chroma = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 1));
    const auto chromaStride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 1);
    for (int row = 0; row < VIDEO_HEIGHT; ++row) {
      for (int column = 0; column < VIDEO_WIDTH; ++column) {
        luma[row * lumaStride + column] =
          static_cast<uint8_t>((column / 2 + row / 2 + index * 4) & 0xFF);
      }
    }
    for (int row = 0; row < VIDEO_HEIGHT / 2; ++row) {
      for (int column = 0; column < VIDEO_WIDTH / 2; ++column) {
        chroma[row * chromaStride + column * 2] = 128;
        chroma[row * chromaStride + column * 2 + 1] = 128;
      }
    }
    CVPixelBufferUnlockBaseAddress(pixelBuffer, 0);

    auto frame = std::make_shared<TestCVPixelBufferFrame>(pixelBuffer, VIDEO_WIDTH, VIDEO_HEIGHT);
    CVPixelBufferRelease(pixelBuffer);
    frame->timeUs(static_cast<int64_t>(index) * Definitions::kMicrosPerSecond / VIDEO_FRAME_RATE);
    encoder->queueInputBuffer(frame);
    drain();
  }
  encoder->signalEndOfInputStream();
  int guard = 0;
  while (!endOfStream && guard++ < 1000) {
    drain();
  }
  encoder->stop();
  encoder->release();

  ASSERT_FALSE(packets.empty()) << "the encoder produced no packet for the zero-copy frames";
  ASSERT_NE(format, nullptr);
  const auto decoded = decodeVideo(format, packets);
  EXPECT_GE(decoded.frames, 8);
}

TEST(DarwinVideoCodec, AttachesTheColorExtensionsToAnH264Description) {
  const auto encoded = encodeVideo(VideoCodecType::kVideoCodecTypeH264, 1);
  ASSERT_NE(encoded.format, nullptr) << "the platform H.264 encoder is not available";
  ColorInfo color;
  color.colorSpace = ColorInfo::SpaceType::kSpaceBT709;
  color.colorRange = ColorInfo::RangeType::kRangeFull;
  color.colorTransfer = ColorInfo::TransferType::kTransferSDR;
  encoded.format->setColorInfo(color);

  CMFormatDescriptionRef description = darwin::createFormatDescription(encoded.format);
  ASSERT_NE(description, nullptr);
  const auto extensionEquals = [description](CFStringRef key, CFStringRef expected) {
    CFTypeRef value = CMFormatDescriptionGetExtension(description, key);
    return value != nullptr && CFEqual(value, expected);
  };
  EXPECT_TRUE(extensionEquals(kCMFormatDescriptionExtension_ColorPrimaries,
    kCMFormatDescriptionColorPrimaries_ITU_R_709_2));
  EXPECT_TRUE(extensionEquals(kCMFormatDescriptionExtension_TransferFunction,
    kCMFormatDescriptionTransferFunction_ITU_R_709_2));
  EXPECT_TRUE(extensionEquals(kCMFormatDescriptionExtension_YCbCrMatrix,
    kCMFormatDescriptionYCbCrMatrix_ITU_R_709_2));
  const auto fullRange = static_cast<CFBooleanRef>(CMFormatDescriptionGetExtension(description,
    kCMFormatDescriptionExtension_FullRangeVideo));
  ASSERT_NE(nullptr, fullRange);
  EXPECT_TRUE(CFBooleanGetValue(fullRange));

  // The color information is carried beside the parameter sets, which the decoders still read from
  // the avcC atom of the description.
  size_t parameterSetCount = 0;
  int nalLengthSize = 0;
  EXPECT_EQ(noErr, CMVideoFormatDescriptionGetH264ParameterSetAtIndex(description, 0, nullptr,
    nullptr, &parameterSetCount, &nalLengthSize));
  EXPECT_GE(parameterSetCount, 2u);
  EXPECT_EQ(4, nalLengthSize);
  CFRelease(description);
}

TEST(DarwinVideoCodec, DecodesIntoZeroCopyIOSurfaceFrames) {
  const auto encoded = encodeVideo();
  ASSERT_NE(encoded.format, nullptr);

  auto surface = std::make_shared<TestHardwareVideoSurface>();
  const auto decoded = decodeVideo(encoded.format, encoded.packets, surface);
  EXPECT_GE(decoded.frames, VIDEO_FRAME_COUNT - 2);
  EXPECT_EQ(decoded.hardwareFrames, decoded.frames);
  EXPECT_EQ(decoded.ioSurfaceFrames, decoded.frames)
      << "every hardware frame must be backed by an IOSurface";
  EXPECT_EQ(decoded.format, PixelFormat::kFormatNV12);
}

// ==================================================================================================
// Muxer and extractor
// ==================================================================================================

TEST(DarwinMediaMuxer, FactorySupportsMpeg4) {
  auto factory = CreateBuiltinMuxerFactory();
  ASSERT_NE(factory, nullptr);
  EXPECT_TRUE(factory->isSupportMuxerFormat(MuxerFormat::kMuxerFormatMpeg4));
  EXPECT_TRUE(factory->isSupportMuxerFormat(MuxerFormat::kMuxerFormat3Gpp));
  EXPECT_FALSE(factory->isSupportMuxerFormat(MuxerFormat::kMuxerFormatWebm));
  EXPECT_NE(factory->createMuxer(), nullptr);
}

TEST(DarwinMediaMuxer, MuxesAudioAndVideoTracks) {
  const auto path = tempFilePath("darwin-muxer.m4v");
  const auto audio = encodeAudio();
  const auto video = encodeVideo();
  ASSERT_FALSE(audio.packets.empty());
  ASSERT_FALSE(video.packets.empty());
  ASSERT_NE(audio.format, nullptr);
  ASSERT_NE(video.format, nullptr);

  auto factory = CreateBuiltinMuxerFactory();
  ASSERT_NE(factory, nullptr);
  auto muxer = factory->createMuxer();
  ASSERT_NE(muxer, nullptr);
  ASSERT_TRUE(muxer->prepare(path, MuxerFormat::kMuxerFormatMpeg4));
  EXPECT_EQ(muxer->getStatus(), AVMediaMuxer::Status::kMuxerStatusPrepared);

  const auto audioTrack = muxer->addTrack(audio.format);
  const auto videoTrack = muxer->addTrack(video.format);
  ASSERT_GE(audioTrack, 0);
  ASSERT_GE(videoTrack, 0);
  EXPECT_NE(audioTrack, videoTrack);

  muxer->start();
  ASSERT_TRUE(muxer->isStarted());

  const auto writePackets = [&muxer](const std::vector<EncodedPacket>& packets, const int index,
                                    const MediaType mediaType) {
    for (const auto& packet : packets) {
      auto buffer = std::make_shared<AVPacketBuffer>(mediaType);
      buffer->ensureSpaceForWrite(packet.data.size());
      const auto data = buffer->data();
      data->position(0);
      data->put(packet.data.data(), packet.data.size());
      data->flip();
      buffer->timeUs(packet.timeUs);
      buffer->setStreamIndex(index);
      if (packet.keyFrame) {
        buffer->addFlag(AVBufferFlags::kKeyFrame);
      }
      muxer->writeFrame(buffer);
    }
  };
  writePackets(audio.packets, audioTrack, MediaType::kMediaTypeAudio);
  writePackets(video.packets, videoTrack, MediaType::kMediaTypeVideo);
  muxer->stop();
  EXPECT_EQ(muxer->getStatus(), AVMediaMuxer::Status::kMuxerStatusStopped);
  muxer->release();
  EXPECT_EQ(muxer->getStatus(), AVMediaMuxer::Status::kMuxerStatusIdle);

  ASSERT_TRUE(std::filesystem::exists(path));
  EXPECT_GT(std::filesystem::file_size(path), 4096u);

  const auto extracted = extractAll(path);
  ASSERT_NE(extracted, nullptr);
  EXPECT_EQ(extracted->trackCount(), 2u);
  const auto* audioTrackData = extracted->firstTrackOfType(MediaType::kMediaTypeAudio);
  const auto* videoTrackData = extracted->firstTrackOfType(MediaType::kMediaTypeVideo);
  ASSERT_NE(audioTrackData, nullptr);
  ASSERT_NE(videoTrackData, nullptr);
  ASSERT_NE(audioTrackData->format, nullptr);
  ASSERT_NE(videoTrackData->format, nullptr);
  EXPECT_EQ(audioTrackData->format->getMimeType(), MimeTypes::AUDIO_AAC);
  EXPECT_EQ(audioTrackData->format->getSampleRate(), AUDIO_SAMPLE_RATE);
  EXPECT_EQ(audioTrackData->format->getChannelCount(), AUDIO_CHANNEL_COUNT);
  EXPECT_EQ(videoTrackData->format->getMimeType(), MimeTypes::VIDEO_H264);
  EXPECT_EQ(videoTrackData->format->getWidth(), VIDEO_WIDTH);
  EXPECT_EQ(videoTrackData->format->getHeight(), VIDEO_HEIGHT);
  EXPECT_GE(videoTrackData->packets.size(), VIDEO_FRAME_COUNT - 2);
  EXPECT_GT(videoTrackData->keyFrameCount, 0);

  auto audioDecoder = CreateBuiltinAudioCodecFactory()->createDecoder(
    audioTrackData->format->getMimeType(), false);
  ASSERT_NE(audioDecoder, nullptr);
  ASSERT_TRUE(audioDecoder->configure(audioTrackData->format));
  audioDecoder->start();
  const auto decodedAudioFrames = decodePackets(audioDecoder.get(), audioTrackData->packets);
  audioDecoder->stop();
  audioDecoder->release();
  EXPECT_NEAR(static_cast<double>(decodedAudioFrames), AUDIO_FRAME_COUNT,
    AUDIO_FRAME_COUNT * 0.20);

  const auto decoded = decodeVideo(videoTrackData->format, videoTrackData->packets);
  EXPECT_GE(decoded.frames, VIDEO_FRAME_COUNT - 2);
  EXPECT_EQ(decoded.width, VIDEO_WIDTH);
  EXPECT_EQ(decoded.height, VIDEO_HEIGHT);
}

TEST(DarwinMediaMuxer, PreservesTheH264ColorMetadataInTheFile) {
  const auto path = tempFilePath("darwin-color-metadata.mp4");
  auto video = encodeVideo(VideoCodecType::kVideoCodecTypeH264, 5);
  ASSERT_FALSE(video.packets.empty());
  ASSERT_NE(video.format, nullptr);
  ColorInfo color;
  color.colorSpace = ColorInfo::SpaceType::kSpaceBT709;
  color.colorRange = ColorInfo::RangeType::kRangeFull;
  color.colorTransfer = ColorInfo::TransferType::kTransferSDR;
  video.format->setColorInfo(color);

  auto muxer = CreateBuiltinMuxerFactory()->createMuxer();
  ASSERT_NE(muxer, nullptr);
  ASSERT_TRUE(muxer->prepare(path, MuxerFormat::kMuxerFormatMpeg4));
  const auto track = muxer->addTrack(video.format);
  ASSERT_GE(track, 0);
  muxer->start();
  for (const auto& packet : video.packets) {
    auto buffer = std::make_shared<AVPacketBuffer>(MediaType::kMediaTypeVideo);
    buffer->ensureSpaceForWrite(packet.data.size());
    const auto data = buffer->data();
    data->position(0);
    data->put(packet.data.data(), packet.data.size());
    data->flip();
    buffer->timeUs(packet.timeUs);
    buffer->setStreamIndex(track);
    if (packet.keyFrame) {
      buffer->addFlag(AVBufferFlags::kKeyFrame);
    }
    muxer->writeFrame(buffer);
  }
  muxer->stop();
  ASSERT_EQ(muxer->getStatus(), AVMediaMuxer::Status::kMuxerStatusStopped);
  muxer->release();

  const auto extracted = extractAll(path);
  ASSERT_NE(extracted, nullptr);
  const auto* videoTrack = extracted->firstTrackOfType(MediaType::kMediaTypeVideo);
  ASSERT_NE(videoTrack, nullptr);
  ASSERT_NE(videoTrack->format, nullptr);
  EXPECT_EQ(videoTrack->format->getColorInfo().colorSpace,
    ColorInfo::SpaceType::kSpaceBT709);
  EXPECT_EQ(videoTrack->format->getColorInfo().colorRange, ColorInfo::RangeType::kRangeFull);
  EXPECT_EQ(videoTrack->format->getColorInfo().colorTransfer,
    ColorInfo::TransferType::kTransferSDR);
  const auto decoded = decodeVideo(videoTrack->format, videoTrack->packets);
  EXPECT_GE(decoded.frames, 3);
}

TEST(DarwinMediaMuxer, PreservesTheHevcColorMetadataInTheFile) {
  auto video = encodeVideo(VideoCodecType::kVideoCodecTypeHevc, 5);
  if (video.packets.empty()) {
    GTEST_SKIP() << "the platform HEVC encoder is not available in this session";
  }
  ASSERT_NE(video.format, nullptr);
  ColorInfo color;
  color.colorSpace = ColorInfo::SpaceType::kSpaceBT2020;
  color.colorRange = ColorInfo::RangeType::kRangeLimited;
  color.colorTransfer = ColorInfo::TransferType::kTransferHLG;
  video.format->setColorInfo(color);

  const auto path = tempFilePath("darwin-hevc-color-metadata.mp4");
  auto muxer = CreateBuiltinMuxerFactory()->createMuxer();
  ASSERT_NE(muxer, nullptr);
  ASSERT_TRUE(muxer->prepare(path, MuxerFormat::kMuxerFormatMpeg4));
  const auto track = muxer->addTrack(video.format);
  ASSERT_GE(track, 0);
  muxer->start();
  for (const auto& packet : video.packets) {
    auto buffer = std::make_shared<AVPacketBuffer>(MediaType::kMediaTypeVideo);
    buffer->ensureSpaceForWrite(packet.data.size());
    const auto data = buffer->data();
    data->position(0);
    data->put(packet.data.data(), packet.data.size());
    data->flip();
    buffer->timeUs(packet.timeUs);
    buffer->setStreamIndex(track);
    if (packet.keyFrame) {
      buffer->addFlag(AVBufferFlags::kKeyFrame);
    }
    muxer->writeFrame(buffer);
  }
  muxer->stop();
  ASSERT_EQ(muxer->getStatus(), AVMediaMuxer::Status::kMuxerStatusStopped);
  muxer->release();

  const auto extracted = extractAll(path);
  ASSERT_NE(extracted, nullptr);
  const auto* videoTrack = extracted->firstTrackOfType(MediaType::kMediaTypeVideo);
  ASSERT_NE(videoTrack, nullptr);
  ASSERT_NE(videoTrack->format, nullptr);
  EXPECT_EQ(videoTrack->format->getMimeType(), MimeTypes::VIDEO_H265);
  EXPECT_EQ(videoTrack->format->getColorInfo().colorSpace,
    ColorInfo::SpaceType::kSpaceBT2020);
  EXPECT_EQ(videoTrack->format->getColorInfo().colorTransfer,
    ColorInfo::TransferType::kTransferHLG);
  EXPECT_EQ(videoTrack->format->getColorInfo().colorRange,
    ColorInfo::RangeType::kRangeLimited);
  const auto decoded = decodeVideo(videoTrack->format, videoTrack->packets, nullptr,
    VideoCodecType::kVideoCodecTypeHevc);
  EXPECT_GE(decoded.frames, 3);
}

TEST(DarwinMediaMuxer, MuxesVideoInto3gpp) {
  const auto path = tempFilePath("darwin-muxer.3gp");
  const auto video = encodeVideo(VideoCodecType::kVideoCodecTypeH264, 10);
  ASSERT_FALSE(video.packets.empty());
  ASSERT_NE(video.format, nullptr);

  auto muxer = CreateBuiltinMuxerFactory()->createMuxer();
  ASSERT_NE(muxer, nullptr);
  ASSERT_TRUE(muxer->prepare(path, MuxerFormat::kMuxerFormat3Gpp));
  const auto track = muxer->addTrack(video.format);
  ASSERT_GE(track, 0);
  muxer->start();
  for (const auto& packet : video.packets) {
    auto buffer = std::make_shared<AVPacketBuffer>(MediaType::kMediaTypeVideo);
    buffer->ensureSpaceForWrite(packet.data.size());
    const auto data = buffer->data();
    data->position(0);
    data->put(packet.data.data(), packet.data.size());
    data->flip();
    buffer->timeUs(packet.timeUs);
    buffer->setStreamIndex(track);
    if (packet.keyFrame) {
      buffer->addFlag(AVBufferFlags::kKeyFrame);
    }
    muxer->writeFrame(buffer);
  }
  muxer->stop();
  EXPECT_EQ(muxer->getStatus(), AVMediaMuxer::Status::kMuxerStatusStopped);
  muxer->release();

  const auto extracted = extractAll(path);
  ASSERT_NE(extracted, nullptr);
  const auto* videoTrack = extracted->firstTrackOfType(MediaType::kMediaTypeVideo);
  ASSERT_NE(videoTrack, nullptr);
  ASSERT_NE(videoTrack->format, nullptr);
  EXPECT_EQ(videoTrack->format->getMimeType(), MimeTypes::VIDEO_H264);
  const auto decoded = decodeVideo(videoTrack->format, videoTrack->packets);
  EXPECT_GE(decoded.frames, 8);
}

TEST(DarwinMediaMuxer, MuxesAudioOnly) {
  const auto path = tempFilePath("darwin-muxer.m4a");
  const auto audio = encodeAudio();
  ASSERT_FALSE(audio.packets.empty());
  ASSERT_NE(audio.format, nullptr);

  auto muxer = CreateBuiltinMuxerFactory()->createMuxer();
  ASSERT_NE(muxer, nullptr);
  ASSERT_TRUE(muxer->prepare(path, MuxerFormat::kMuxerFormatMpeg4));
  const auto track = muxer->addTrack(audio.format);
  ASSERT_GE(track, 0);
  muxer->start();
  for (const auto& packet : audio.packets) {
    auto buffer = std::make_shared<AVPacketBuffer>(MediaType::kMediaTypeAudio);
    buffer->ensureSpaceForWrite(packet.data.size());
    const auto data = buffer->data();
    data->position(0);
    data->put(packet.data.data(), packet.data.size());
    data->flip();
    buffer->timeUs(packet.timeUs);
    buffer->setStreamIndex(track);
    muxer->writeFrame(buffer);
  }
  muxer->stop();
  muxer->release();

  const auto extracted = extractAll(path);
  ASSERT_NE(extracted, nullptr);
  const auto* audioTrack = extracted->firstTrackOfType(MediaType::kMediaTypeAudio);
  ASSERT_NE(audioTrack, nullptr);
  ASSERT_NE(audioTrack->format, nullptr);
  EXPECT_EQ(audioTrack->format->getMimeType(), MimeTypes::AUDIO_AAC);
  EXPECT_EQ(audioTrack->format->getSampleRate(), AUDIO_SAMPLE_RATE);
  EXPECT_GT(audioTrack->sampleCount, 0);

  auto decoder = CreateBuiltinAudioCodecFactory()->createDecoder(MimeTypes::AUDIO_AAC, false);
  ASSERT_NE(decoder, nullptr);
  ASSERT_TRUE(decoder->configure(audioTrack->format));
  decoder->start();
  const auto frames = decodePackets(decoder.get(), audioTrack->packets);
  decoder->stop();
  decoder->release();
  EXPECT_NEAR(static_cast<double>(frames), AUDIO_FRAME_COUNT, AUDIO_FRAME_COUNT * 0.20);
}

// ==================================================================================================
// The data source of a local media
// ==================================================================================================

TEST(DarwinDataSource, ReadsABundledMediaThroughTheMetadataRetriever) {
  const auto directory = bundleResourceDirectory();
  if (directory.empty()) {
    GTEST_SKIP() << "the resources of the main bundle are not available";
  }
  const std::string name = "coral-darwin-bundled.m4a";
  const auto path = std::filesystem::path(directory) / name;
  BundleFixtureCleanup cleanup{ path };

  {
    auto writer = AudioFileWriter::CreateWithUrl(path.string(),
      MediaFileType::kMediaFileTypeMpeg4AAC, createAacFormat());
    ASSERT_NE(writer, nullptr);
    Error* error = nullptr;
    ASSERT_TRUE(writer->open(&error)) << (error != nullptr ? error->toString() : "");
    const auto pcm = generatePcm(AUDIO_SAMPLE_RATE, AUDIO_CHANNEL_COUNT, AUDIO_SAMPLE_RATE / 4);
    writer->writeFromPCMBuffer(const_cast<int16_t*>(pcm.data()), pcm.size() / AUDIO_CHANNEL_COUNT,
      &error);
    writer->close();
  }
  ASSERT_TRUE(std::filesystem::exists(path));

  auto retriever = MediaMetadataRetriever::create("asset:///" + name);
  ASSERT_NE(retriever, nullptr) << "the metadata retriever cannot read the bundled media";
  ASSERT_GE(retriever->getTrackCount(), 1);
  const auto format = retriever->getTrackFormat(0);
  ASSERT_NE(format, nullptr);
  EXPECT_EQ(format->getMimeType(), MimeTypes::AUDIO_AAC);

  // The same file by its file uri, which the platform file data source reads.
  auto fileRetriever = MediaMetadataRetriever::create("file://" + path.string());
  ASSERT_NE(fileRetriever, nullptr);
  EXPECT_GE(fileRetriever->getTrackCount(), 1);
}

// ==================================================================================================
// Audio files
// ==================================================================================================

TEST(DarwinAudioFile, WritesAndReadsMpeg4Aac) {
  const auto path = tempFilePath("darwin-audio-file.m4a");
  auto writer = AudioFileWriter::CreateWithUrl(path, MediaFileType::kMediaFileTypeMpeg4AAC,
    createAacFormat());
  ASSERT_NE(writer, nullptr);
  Error* error = nullptr;
  ASSERT_TRUE(writer->open(&error)) << (error != nullptr ? error->toString() : "");
  const auto pcm = generatePcm(AUDIO_SAMPLE_RATE, AUDIO_CHANNEL_COUNT, AUDIO_FRAME_COUNT);
  int offset = 0;
  while (offset < AUDIO_FRAME_COUNT) {
    const auto frames = std::min(AUDIO_CHUNK_FRAMES, AUDIO_FRAME_COUNT - offset);
    ASSERT_EQ(writer->writeFromPCMBuffer(
      const_cast<int16_t*>(pcm.data() + static_cast<size_t>(offset) * AUDIO_CHANNEL_COUNT),
      frames, &error), frames) << (error != nullptr ? error->toString() : "");
    offset += frames;
  }
  EXPECT_EQ(writer->getFramePosition(), AUDIO_FRAME_COUNT);
  writer->close();
  writer.reset();

  ASSERT_TRUE(std::filesystem::exists(path));
  EXPECT_GT(std::filesystem::file_size(path), 1024u);

  auto reader = AudioFileReader::CreateWithUrl(path);
  ASSERT_NE(reader, nullptr);
  ASSERT_TRUE(reader->open(&error)) << (error != nullptr ? error->toString() : "");
  EXPECT_EQ(reader->getSampleRate(), AUDIO_SAMPLE_RATE);
  EXPECT_EQ(reader->getChannelCount(), AUDIO_CHANNEL_COUNT);
  EXPECT_EQ(reader->getSampleFormat(), AudioSampleFormat::kInt16);
  EXPECT_NEAR(static_cast<double>(reader->getSampleLength()), AUDIO_FRAME_COUNT,
    AUDIO_FRAME_COUNT * 0.05);
  const auto frames = readAllFrames(reader.get(), &error);
  ASSERT_GT(frames, 0) << (error != nullptr ? error->toString() : "");
  EXPECT_NEAR(static_cast<double>(frames), AUDIO_FRAME_COUNT, AUDIO_FRAME_COUNT * 0.10);
  reader->close();
}

// ==================================================================================================
// A real media file, when the checkout carries one
// ==================================================================================================

#ifdef CORAL_TEST_DATA_DIR
TEST(DarwinMediaExtractor, SeeksToPreviousSync) {
  const std::filesystem::path fixture = std::filesystem::path(CORAL_TEST_DATA_DIR) /
                                        "video" / "video720p_5s.mp4";
  if (!std::filesystem::exists(fixture)) {
    GTEST_SKIP() << "the fixture is not available: " << fixture;
  }
  auto extractor = CreateBuiltinExtractorFactory()->createExtractor(fixture.string());
  ASSERT_NE(extractor, nullptr);
  EXPECT_TRUE(extractor->isSeekable());
  EXPECT_NEAR(static_cast<double>(extractor->getDurationUs()), 5'000'000.0, 100'000.0);
  auto output = std::make_shared<CollectingExtractorOutput>();
  extractor->init(*output);
  AVMediaExtractor::PositionHolder holder;
  int guard = 0;
  while (guard++ < 200000) {
    if (extractor->read(holder) != AVMediaExtractor::PlatformResult::RESULT_CONTINUE) {
      break;
    }
  }
  const auto* videoBefore = output->firstTrackOfType(MediaType::kMediaTypeVideo);
  ASSERT_NE(videoBefore, nullptr);
  ASSERT_NE(videoBefore->format, nullptr);
  const auto baseline = videoBefore->packets.size();
  ASSERT_GT(baseline, 0u);

  ASSERT_TRUE(extractor->seekTo(2'000'000, AVMediaExtractor::SeekOption::SEEK_TO_PREVIOUS_SYNC));
  guard = 0;
  while (guard++ < 20000) {
    const auto result = extractor->read(holder);
    if (result != AVMediaExtractor::PlatformResult::RESULT_CONTINUE) {
      break;
    }
    if (videoBefore->packets.size() > baseline) {
      break;
    }
  }
  ASSERT_GT(videoBefore->packets.size(), baseline);
  const auto& first = videoBefore->packets[baseline];
  EXPECT_TRUE(first.keyFrame) << "a seek to a sync sample must begin with a key frame";
  EXPECT_LE(first.timeUs, 2'100'000) << "the seek must start at the sample at or before the target";
  extractor->clear();
}

TEST(DarwinMediaExtractor, SeeksToNextSync) {
  const std::filesystem::path fixture = std::filesystem::path(CORAL_TEST_DATA_DIR) /
                                        "video" / "video1080p_20s.mp4";
  if (!std::filesystem::exists(fixture)) {
    GTEST_SKIP() << "the fixture is not available: " << fixture;
  }
  auto extractor = CreateBuiltinExtractorFactory()->createExtractor(fixture.string());
  ASSERT_NE(extractor, nullptr);
  auto output = std::make_shared<CollectingExtractorOutput>();
  extractor->init(*output);
  AVMediaExtractor::PositionHolder holder;
  int guard = 0;
  while (guard++ < 400000) {
    if (extractor->read(holder) != AVMediaExtractor::PlatformResult::RESULT_CONTINUE) {
      break;
    }
  }
  const auto* videoBefore = output->firstTrackOfType(MediaType::kMediaTypeVideo);
  ASSERT_NE(videoBefore, nullptr);
  ASSERT_NE(videoBefore->format, nullptr);
  std::vector<int64_t> keyFrameTimes;
  for (const auto& packet : videoBefore->packets) {
    if (packet.keyFrame) {
      keyFrameTimes.push_back(packet.timeUs);
    }
  }
  if (keyFrameTimes.size() < 2u) {
    GTEST_SKIP() << "the asset has no second key frame to seek to";
  }
  const auto baseline = videoBefore->packets.size();
  // A position just before the second key frame: the previous sync sample is the first key frame,
  // so seeking to the next sync sample has to land on the second one.
  const auto target = std::max<int64_t>(keyFrameTimes[1] - 200'000, 0);
  ASSERT_TRUE(extractor->seekTo(target, AVMediaExtractor::SeekOption::SEEK_TO_NEXT_SYNC));
  guard = 0;
  while (guard++ < 40000) {
    const auto result = extractor->read(holder);
    if (result != AVMediaExtractor::PlatformResult::RESULT_CONTINUE) {
      break;
    }
    if (videoBefore->packets.size() > baseline) {
      break;
    }
  }
  ASSERT_GT(videoBefore->packets.size(), baseline);
  const auto& next = videoBefore->packets[baseline];
  EXPECT_TRUE(next.keyFrame) << "a seek to the next sync sample must land on a key frame";
  EXPECT_GE(next.timeUs, target) << "the seek must not deliver a sample before the target";
  EXPECT_EQ(next.timeUs, keyFrameTimes[1]) << "the seek must land on the next sync sample";
  extractor->clear();
}

TEST(DarwinMediaExtractor, ReadsARealFlacFile) {
  const std::filesystem::path fixture = std::filesystem::path(CORAL_TEST_DATA_DIR) /
                                        "music" / "music.flac";
  if (!std::filesystem::exists(fixture)) {
    GTEST_SKIP() << "the fixture is not available: " << fixture;
  }
  auto extractor = CreateBuiltinExtractorFactory()->createExtractor(fixture.string());
  ASSERT_NE(extractor, nullptr);
  auto output = std::make_shared<CollectingExtractorOutput>();
  extractor->init(*output);
  AVMediaExtractor::PositionHolder holder;
  int guard = 0;
  while (guard++ < 20000) {
    const auto result = extractor->read(holder);
    if (result != AVMediaExtractor::PlatformResult::RESULT_CONTINUE) {
      break;
    }
    const auto* audio = output->firstTrackOfType(MediaType::kMediaTypeAudio);
    if (audio != nullptr && audio->packets.size() >= 20) {
      break;
    }
  }
  const auto* audioTrack = output->firstTrackOfType(MediaType::kMediaTypeAudio);
  ASSERT_NE(audioTrack, nullptr);
  ASSERT_NE(audioTrack->format, nullptr);
  EXPECT_EQ(audioTrack->format->getMimeType(), MimeTypes::AUDIO_FLAC);
  EXPECT_GT(audioTrack->format->getSampleRate(), 0);
  EXPECT_GT(audioTrack->format->getChannelCount(), 0);
  ASSERT_GE(audioTrack->packets.size(), 20u);
  std::vector<EncodedPacket> packets(audioTrack->packets.begin(),
    audioTrack->packets.begin() + 20);

  auto decoder = CreateBuiltinAudioCodecFactory()->createDecoder(MimeTypes::AUDIO_FLAC, false);
  ASSERT_NE(decoder, nullptr);
  ASSERT_TRUE(decoder->configure(audioTrack->format));
  decoder->start();
  const auto frames = decodePackets(decoder.get(), packets);
  decoder->stop();
  decoder->release();
  // The FLAC blocks of the asset are 1152 frames, which the decoder has to produce in full.
  EXPECT_GE(frames, 20 * 1152 - 1152);
  EXPECT_LE(frames, 20 * 1152 + 1152);
  extractor->clear();
}

TEST(DarwinMediaExtractor, ReadsARealWaveFile) {
  const std::filesystem::path fixture = std::filesystem::path(CORAL_TEST_DATA_DIR) /
                                        "music" / "music.wav";
  if (!std::filesystem::exists(fixture)) {
    GTEST_SKIP() << "the fixture is not available: " << fixture;
  }
  auto extractor = CreateBuiltinExtractorFactory()->createExtractor(fixture.string());
  ASSERT_NE(extractor, nullptr);
  auto output = std::make_shared<CollectingExtractorOutput>();
  extractor->init(*output);
  AVMediaExtractor::PositionHolder holder;
  int guard = 0;
  while (guard++ < 20000) {
    if (extractor->read(holder) != AVMediaExtractor::PlatformResult::RESULT_CONTINUE) {
      break;
    }
    const auto* audio = output->firstTrackOfType(MediaType::kMediaTypeAudio);
    if (audio != nullptr && audio->packets.size() >= 10) {
      break;
    }
  }
  const auto* audioTrack = output->firstTrackOfType(MediaType::kMediaTypeAudio);
  ASSERT_NE(audioTrack, nullptr);
  ASSERT_NE(audioTrack->format, nullptr);
  EXPECT_EQ(audioTrack->format->getMimeType(), MimeTypes::AUDIO_RAW);
  EXPECT_EQ(audioTrack->format->getAudioFormatType(),
    AudioFormatType::kAudioFormatTypeLinearPCM);
  EXPECT_EQ(audioTrack->format->getSampleFormat(), AudioSampleFormat::kInt16);
  ASSERT_GE(audioTrack->packets.size(), 5u);
  std::vector<EncodedPacket> packets(audioTrack->packets.begin(),
    audioTrack->packets.begin() + 5);

  auto decoder = CreateBuiltinAudioCodecFactory()->createDecoder(MimeTypes::AUDIO_RAW, false);
  ASSERT_NE(decoder, nullptr);
  ASSERT_TRUE(decoder->configure(audioTrack->format));
  decoder->start();
  const auto frames = decodePackets(decoder.get(), packets);
  decoder->stop();
  decoder->release();
  EXPECT_GT(frames, 0);
  extractor->clear();
}

TEST(DarwinMediaExtractor, ReportsTheRotationOfTheTrack) {
  const std::filesystem::path fixture = std::filesystem::path(CORAL_TEST_DATA_DIR) /
                                        "video" / "portrait_rotated_avc_aac.mp4";
  if (!std::filesystem::exists(fixture)) {
    GTEST_SKIP() << "the fixture is not available: " << fixture;
  }
  const auto extracted = extractAll(fixture.string());
  ASSERT_NE(extracted, nullptr);
  const auto* videoTrack = extracted->firstTrackOfType(MediaType::kMediaTypeVideo);
  ASSERT_NE(videoTrack, nullptr);
  ASSERT_NE(videoTrack->format, nullptr);
  EXPECT_TRUE(videoTrack->format->getRotation() == 90 ||
              videoTrack->format->getRotation() == 270)
      << "the rotation of the track is " << videoTrack->format->getRotation();
}

TEST(DarwinVideoCodec, DecodesARealHevcAsset) {
  const std::filesystem::path fixture = std::filesystem::path(CORAL_TEST_DATA_DIR) /
                                        "video" / "IPhone12_4k_HLG.mp4";
  if (!std::filesystem::exists(fixture)) {
    GTEST_SKIP() << "the fixture is not available: " << fixture;
  }
  const auto extracted = extractAll(fixture.string());
  ASSERT_NE(extracted, nullptr);
  const auto* videoTrack = extracted->firstTrackOfType(MediaType::kMediaTypeVideo);
  ASSERT_NE(videoTrack, nullptr) << "the platform extractor found no video track";
  ASSERT_NE(videoTrack->format, nullptr);
  EXPECT_EQ(videoTrack->format->getMimeType(), MimeTypes::VIDEO_H265);
  EXPECT_GT(videoTrack->sampleCount, 0);
  EXPECT_FALSE(videoTrack->format->csdList().empty());
  EXPECT_GT(videoTrack->format->getProfile(), 0);
  EXPECT_GT(videoTrack->format->getLevel(), 0);
  // The asset is an HLG HDR clip: the track has to carry the color metadata so that a renderer can
  // convert it, and the decoded frames have to keep it.
  EXPECT_EQ(videoTrack->format->getColorInfo().colorSpace, ColorInfo::SpaceType::kSpaceBT2020);
  EXPECT_EQ(videoTrack->format->getColorInfo().colorTransfer,
    ColorInfo::TransferType::kTransferHLG);

  const auto decoded = decodeVideo(videoTrack->format, videoTrack->packets, nullptr,
    VideoCodecType::kVideoCodecTypeHevc);
  EXPECT_GE(decoded.frames, 1);
  EXPECT_GT(decoded.width, 0);
  EXPECT_EQ(decoded.colorInfo.colorSpace, ColorInfo::SpaceType::kSpaceBT2020);
  EXPECT_EQ(decoded.colorInfo.colorTransfer, ColorInfo::TransferType::kTransferHLG);
}

TEST(DarwinMediaExtractor, ReadsARealMpeg4File) {
  const std::filesystem::path fixture = std::filesystem::path(CORAL_TEST_DATA_DIR) /
                                        "video" / "video720p_5s.mp4";
  if (!std::filesystem::exists(fixture)) {
    GTEST_SKIP() << "the fixture is not available: " << fixture;
  }
  const auto extracted = extractAll(fixture.string());
  ASSERT_NE(extracted, nullptr);
  const auto* videoTrack = extracted->firstTrackOfType(MediaType::kMediaTypeVideo);
  ASSERT_NE(videoTrack, nullptr) << "the platform extractor found no video track";
  ASSERT_NE(videoTrack->format, nullptr);
  EXPECT_GT(videoTrack->sampleCount, 0);
  EXPECT_GT(videoTrack->keyFrameCount, 0);
  EXPECT_GT(videoTrack->format->getProfile(), 0);
  EXPECT_GT(videoTrack->format->getLevel(), 0);

  const auto* audioTrack = extracted->firstTrackOfType(MediaType::kMediaTypeAudio);
  if (audioTrack != nullptr && audioTrack->format != nullptr &&
      audioTrack->format->getMimeType() == MimeTypes::AUDIO_AAC) {
    auto audioDecoder = CreateBuiltinAudioCodecFactory()->createDecoder(MimeTypes::AUDIO_AAC,
      false);
    ASSERT_NE(audioDecoder, nullptr);
    ASSERT_TRUE(audioDecoder->configure(audioTrack->format));
    audioDecoder->start();
    const auto frames = decodePackets(audioDecoder.get(), audioTrack->packets);
    audioDecoder->stop();
    audioDecoder->release();
    EXPECT_GT(frames, 0) << "the AAC track of the asset decoded to no sample";
  }

  auto decoder = CreateBuiltinVideoCodecFactory()->createVideoDecoder(
    VideoCodecType::kVideoCodecTypeH264, CodecManufacturer::kHardware);
  ASSERT_NE(decoder, nullptr);
  ASSERT_TRUE(decoder->configure(videoTrack->format, nullptr));
  decoder->start();
  int frames = 0;
  for (const auto& packet : videoTrack->packets) {
    if (frames >= 10) {
      break;
    }
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
      auto frame = decoder->dequeueOutputBuffer();
      if (frame != nullptr) {
        frames++;
        frame->release();
      }
    }
    while (auto frame = decoder->dequeueOutputBuffer()) {
      if (!frame->isEndOfStream()) {
        frames++;
      }
      frame->release();
    }
  }
  decoder->signalEndOfInputStream();
  int guard = 0;
  while (guard++ < 100) {
    auto frame = decoder->dequeueOutputBuffer();
    if (frame == nullptr) {
      break;
    }
    if (!frame->isEndOfStream()) {
      frames++;
    }
    frame->release();
  }
  decoder->stop();
  decoder->release();
  EXPECT_GE(frames, 5);
}
#endif // CORAL_TEST_DATA_DIR

} // namespace coral::codec::test

#endif // defined(__APPLE__)
