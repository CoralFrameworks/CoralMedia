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

#include <win/D3D11VAVideoDecoder.h>
#include <win/WmfMediaUtils.h>
#include <win/WmfVideoDecoder.h>
#include <win/WmfVideoEncoder.h>

#include <codec/HardwareVideo.h>

#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <gtest/gtest.h>

#include <minilog/log.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace coral::codec::test {

namespace {

using Microsoft::WRL::ComPtr;

/**
 * A decode target that is the test's own Direct3D 11 device, which is the shape a renderer's device takes.
 */
class TestHardwareVideoSurface final : public HardwareVideoSurface {
public:
  explicit TestHardwareVideoSurface(ID3D11Device* device) noexcept : mDevice(device) {}

  [[nodiscard]] void* getInstance() const override {
    return const_cast<ID3D11Device*>(mDevice);
  }

  [[nodiscard]] bool isValid() const override {
    return mDevice != nullptr;
  }

  void release() override {}

  [[nodiscard]] void* getNativeDevice() const override {
    return const_cast<ID3D11Device*>(mDevice);
  }

private:
  ID3D11Device* mDevice = nullptr;
};

constexpr int WIDTH = 320;
constexpr int HEIGHT = 240;
constexpr int FRAME_COUNT = 30;
constexpr int FRAME_RATE = 30;
constexpr int AUDIO_SAMPLE_RATE = 48000;
constexpr int AUDIO_CHANNEL_COUNT = 2;

std::shared_ptr<MediaFormat> createH264Format() {
  auto format = MediaFormat::CreateVideoFormat(MimeTypes::VIDEO_H264, WIDTH, HEIGHT);
  format->setVideoCodecType(VideoCodecType::kVideoCodecTypeH264);
  format->setFrameRate(FRAME_RATE);
  format->setAverageBitrate(1000000);
  return format;
}

bool fillFrame(const std::shared_ptr<AVPixelBuffer>& target, const int index) {
  auto source = generateI420Frame(WIDTH, HEIGHT, index);
  if (target->getFormat() == PixelFormat::kFormatI420) {
    auto i420 = target->asI420PixelBuffer();
    if (i420 == nullptr) {
      return false;
    }
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
    return true;
  }
  auto nv12 = target->asNV12PixelBuffer();
  if (nv12 == nullptr) {
    return false;
  }
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
  return true;
}

/**
 * Encodes the generated frames and collects the encoded packets.
 */
bool encodeFrames(std::vector<EncodedPacket>& packets, std::shared_ptr<MediaFormat>& encodedFormat) {
  auto encoder = std::make_unique<WmfVideoEncoder>(VideoCodecType::kVideoCodecTypeH264);
  if (!encoder->configure(createH264Format())) {
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
        encodedFormat = packet->getFormat();
        continue;
      }
      if (packet->isEndOfStream()) {
        endOfStream = true;
        return;
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

  for (int index = 0; index < FRAME_COUNT; ++index) {
    std::shared_ptr<AVPixelBuffer> input;
    int guard = 0;
    while (input == nullptr && guard++ < 1000) {
      input = encoder->dequeueInputBuffer();
      if (input == nullptr) {
        drain();
      }
    }
    if (input == nullptr || !fillFrame(input, index)) {
      return false;
    }
    input->timeUs(static_cast<int64_t>(index) * Definitions::kMicrosPerSecond / FRAME_RATE);
    encoder->queueInputBuffer(input);
    drain();
  }

  encoder->signalEndOfInputStream();
  int guard = 0;
  while (!endOfStream && guard++ < 1000) {
    const auto count = packets.size();
    drain();
    if (endOfStream || (packets.size() == count && guard > 4)) {
      break;
    }
  }
  encoder->stop();
  encoder->release();
  return !packets.empty() && encodedFormat != nullptr;
}

/**
 * Muxes the generated frames into an mpeg4 container by using the platform sink writer, the h264
 * encoder of the sink writer is used.
 *
 * @param withAudio whether an aac audio stream is written into the container too.
 */
bool writeMpeg4File(const std::string& path, const bool withAudio = false) {
  ComPtr<IMFAttributes> attributes;
  if (FAILED(::MFCreateAttributes(&attributes, 2))) {
    return false;
  }
  attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
  attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);

  ComPtr<IMFSinkWriter> writer;
  const auto widePath = WmfMediaUtils::utf8ToWide(path);
  if (FAILED(::MFCreateSinkWriterFromURL(widePath.c_str(), nullptr, attributes.Get(), &writer))) {
    MINILOG_ERROR("failed to create the sink writer");
    return false;
  }

  ComPtr<IMFMediaType> outputType;
  if (FAILED(::MFCreateMediaType(&outputType))) {
    return false;
  }
  outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
  outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  outputType->SetUINT32(MF_MT_AVG_BITRATE, 1000000);
  outputType->SetUINT64(MF_MT_FRAME_SIZE,
    ::Pack2UINT32AsUINT64(static_cast<UINT32>(WIDTH), static_cast<UINT32>(HEIGHT)));
  outputType->SetUINT64(MF_MT_FRAME_RATE,
    ::Pack2UINT32AsUINT64(static_cast<UINT32>(FRAME_RATE), 1));

  DWORD streamIndex = 0;
  auto result = writer->AddStream(outputType.Get(), &streamIndex);
  if (FAILED(result)) {
    MINILOG_ERROR("failed to add the video stream: {}", WmfMediaUtils::hresultToString(result));
    return false;
  }

  ComPtr<IMFMediaType> inputType;
  if (FAILED(::MFCreateMediaType(&inputType))) {
    return false;
  }
  inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
  inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  inputType->SetUINT64(MF_MT_FRAME_SIZE,
    ::Pack2UINT32AsUINT64(static_cast<UINT32>(WIDTH), static_cast<UINT32>(HEIGHT)));
  inputType->SetUINT64(MF_MT_FRAME_RATE,
    ::Pack2UINT32AsUINT64(static_cast<UINT32>(FRAME_RATE), 1));
  inputType->SetUINT64(MF_MT_PIXEL_ASPECT_RATIO, ::Pack2UINT32AsUINT64(1, 1));

  result = writer->SetInputMediaType(streamIndex, inputType.Get(), nullptr);
  if (FAILED(result)) {
    MINILOG_ERROR("failed to set the input media type: {}", WmfMediaUtils::hresultToString(result));
    return false;
  }

  DWORD audioStreamIndex = 0;
  ComPtr<IMFMediaType> audioInputType;
  if (withAudio) {
    ComPtr<IMFMediaType> audioOutputType;
    if (FAILED(::MFCreateMediaType(&audioOutputType))) {
      return false;
    }
    audioOutputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    audioOutputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    audioOutputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, AUDIO_SAMPLE_RATE);
    audioOutputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, AUDIO_CHANNEL_COUNT);
    audioOutputType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    audioOutputType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 128000 / 8);
    audioOutputType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 0);
    audioOutputType->SetUINT32(MF_MT_AVG_BITRATE, 128000);
    audioOutputType->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
    audioOutputType->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
    if (FAILED(writer->AddStream(audioOutputType.Get(), &audioStreamIndex))) {
      MINILOG_ERROR("failed to add the audio stream");
      return false;
    }
    audioInputType = WmfMediaUtils::createPcmMediaType(MFAudioFormat_PCM, AUDIO_SAMPLE_RATE,
      AUDIO_CHANNEL_COUNT);
    if (audioInputType == nullptr ||
        FAILED(writer->SetInputMediaType(audioStreamIndex, audioInputType.Get(), nullptr))) {
      MINILOG_ERROR("failed to set the audio input media type");
      return false;
    }
  }

  result = writer->BeginWriting();
  if (FAILED(result)) {
    return false;
  }

  const auto pcm = generatePcm(AUDIO_SAMPLE_RATE, AUDIO_CHANNEL_COUNT,
    AUDIO_SAMPLE_RATE * FRAME_COUNT / FRAME_RATE);
  int writtenAudioFrames = 0;
  const auto totalAudioFrames = AUDIO_SAMPLE_RATE * FRAME_COUNT / FRAME_RATE;

  for (int index = 0; index < FRAME_COUNT; ++index) {
    const auto frame = generateI420Frame(WIDTH, HEIGHT, index);
    std::vector<uint8_t> nv12(static_cast<size_t>(WIDTH) * HEIGHT +
                              static_cast<size_t>(WIDTH) * ((HEIGHT + 1) / 2));
    for (int row = 0; row < HEIGHT; ++row) {
      memcpy(nv12.data() + static_cast<ptrdiff_t>(row) * WIDTH,
        frame->dataY() + static_cast<ptrdiff_t>(row) * frame->strideY(), WIDTH);
    }
    auto* uv = nv12.data() + static_cast<ptrdiff_t>(WIDTH) * HEIGHT;
    for (int row = 0; row < frame->chromaHeight(); ++row) {
      const auto* u = frame->dataU() + static_cast<ptrdiff_t>(row) * frame->strideU();
      const auto* v = frame->dataV() + static_cast<ptrdiff_t>(row) * frame->strideV();
      auto* rowData = uv + static_cast<ptrdiff_t>(row) * WIDTH;
      for (int column = 0; column < frame->chromaWidth(); ++column) {
        rowData[column * 2] = u[column];
        rowData[column * 2 + 1] = v[column];
      }
    }

    const auto timeUs = static_cast<int64_t>(index) * Definitions::kMicrosPerSecond / FRAME_RATE;
    const auto sample = WmfMediaUtils::createSample(nv12.data(), nv12.size(), timeUs);
    if (sample == nullptr) {
      return false;
    }
    sample->SetSampleDuration(
      static_cast<LONGLONG>(Definitions::kMicrosPerSecond / FRAME_RATE) * 10);
    if (FAILED(writer->WriteSample(streamIndex, sample.Get()))) {
      return false;
    }

    if (withAudio) {
      // the audio is written in one frame burst per video frame.
      const auto frames = std::min(AUDIO_SAMPLE_RATE / FRAME_RATE, totalAudioFrames - writtenAudioFrames);
      if (frames > 0) {
        const auto audioTimeUs = static_cast<int64_t>(writtenAudioFrames) *
          Definitions::kMicrosPerSecond / AUDIO_SAMPLE_RATE;
        const auto audioSample = WmfMediaUtils::createSample(
          reinterpret_cast<const uint8_t*>(pcm.data()) +
            static_cast<size_t>(writtenAudioFrames) * AUDIO_CHANNEL_COUNT,
          static_cast<size_t>(frames) * AUDIO_CHANNEL_COUNT * sizeof(int16_t), audioTimeUs);
        if (audioSample == nullptr) {
          return false;
        }
        audioSample->SetSampleDuration(
          static_cast<LONGLONG>(frames) * Definitions::kMicrosPerSecond / AUDIO_SAMPLE_RATE * 10);
        if (FAILED(writer->WriteSample(audioStreamIndex, audioSample.Get()))) {
          return false;
        }
        writtenAudioFrames += frames;
      }
    }
  }
  return SUCCEEDED(writer->Finalize());
}

/**
 * Decodes the packets and returns the number of the decoded frames.
 */
int64_t decodeFrames(VideoDecoder* decoder, const std::vector<EncodedPacket>& packets,
  int* outWidth, int* outHeight) {
  int64_t frames = 0;
  const auto drainOutput = [&]() {
    while (true) {
      auto output = decoder->dequeueOutputBuffer();
      if (output == nullptr) {
        break;
      }
      if (output->isEndOfStream()) {
        output->release();
        return true;
      }
      if (outWidth != nullptr) {
        *outWidth = output->width();
      }
      if (outHeight != nullptr) {
        *outHeight = output->height();
      }
      frames++;
      output->release();
    }
    return false;
  };

  for (const auto& packet: packets) {
    int guard = 0;
    while (true) {
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
      if (drainOutput() || ++guard > 1000) {
        break;
      }
    }
    drainOutput();
  }

  decoder->signalEndOfInputStream();
  int guard = 0;
  while (!drainOutput() && ++guard < 1000) {
  }
  return frames;
}

/**
 * Decodes the packets while it plays the part of the video renderer: the frames it drops are marked as
 * discarded before they are given back to the decoder, which is what the renderer does with a frame it
 * is too late to show.
 *
 * <p>A frame which comes out of the decoder already marked as discarded is dropped by the renderer
 * without being rendered, so no frame may carry the mark of the frame which used its buffer before.
 *
 * @param outTaintedFrames The number of the frames which came out of the decoder already marked.
 * @param outDecodedFrames The number of the frames which were decoded.
 */
void decodeFramesWithDroppedFrames(VideoDecoder* decoder, const std::vector<EncodedPacket>& packets,
  int64_t* outTaintedFrames, int64_t* outDecodedFrames) {
  int64_t tainted = 0;
  int64_t decoded = 0;

  const auto drainOutput = [&]() {
    while (true) {
      auto output = decoder->dequeueOutputBuffer();
      if (output == nullptr) {
        return false;
      }
      if (output->isEndOfStream()) {
        output->release();
        return true;
      }
      if (output->isDiscard()) {
        ++tainted;
      }
      // Every second frame is dropped, which is what a renderer that cannot keep up does.
      const auto drop = (decoded % 2) != 0;
      ++decoded;
      if (drop) {
        output->addFlag(AVBufferFlags::kDiscard);
        decoder->releaseOutputBuffer(output, false);
      } else {
        decoder->releaseOutputBuffer(output, true);
      }
    }
  };

  for (const auto& packet: packets) {
    int guard = 0;
    while (true) {
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
      if (drainOutput() || ++guard > 1000) {
        break;
      }
    }
    drainOutput();
  }

  decoder->signalEndOfInputStream();
  int guard = 0;
  while (!drainOutput() && ++guard < 1000) {
  }

  *outTaintedFrames = tainted;
  *outDecodedFrames = decoded;
}

/**
 * Reads the luma plane of a decoded NV12 frame back from the texture and returns its statistics. A
 * decoder that returns a texture without a picture leaves the plane at the driver's fill value, which
 * makes the variance zero; the hardware-frame tests are the place that catches it.
 */
bool readLumaStats(HardwarePixelBuffer* hardware, double* variance, double* mean) {
  if (hardware == nullptr || variance == nullptr || mean == nullptr) {
    return false;
  }
  auto* texture = static_cast<ID3D11Texture2D*>(hardware->getNativeTexture());
  if (texture == nullptr) {
    return false;
  }
  ComPtr<ID3D11Device> device;
  texture->GetDevice(&device);
  if (device == nullptr) {
    return false;
  }
  ComPtr<ID3D11DeviceContext> context;
  device->GetImmediateContext(&context);
  D3D11_TEXTURE2D_DESC description{};
  texture->GetDesc(&description);
  description.ArraySize = 1;
  description.MipLevels = 1;
  description.Usage = D3D11_USAGE_STAGING;
  description.BindFlags = 0;
  description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  description.MiscFlags = 0;
  ComPtr<ID3D11Texture2D> staging;
  if (FAILED(device->CreateTexture2D(&description, nullptr, &staging)) || staging == nullptr) {
    return false;
  }
  context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, texture,
    hardware->getSubresource(), nullptr);
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
    return false;
  }
  const auto* luma = static_cast<const uint8_t*>(mapped.pData);
  double sum = 0;
  double sumSquares = 0;
  uint64_t count = 0;
  for (UINT row = 0; row < description.Height; ++row) {
    const auto* data = luma + static_cast<size_t>(row) * mapped.RowPitch;
    for (UINT column = 0; column < description.Width; ++column) {
      const double value = data[column];
      sum += value;
      sumSquares += value * value;
      ++count;
    }
  }
  context->Unmap(staging.Get(), 0);
  *mean = sum / static_cast<double>(count);
  *variance = sumSquares / static_cast<double>(count) - (*mean) * (*mean);
  return true;
}
} // namespace anonymous

TEST(WmfVideoCodec, DecodesIntoDirect3D11Textures) {
  std::vector<EncodedPacket> packets;
  std::shared_ptr<MediaFormat> encodedFormat;
  ASSERT_TRUE(encodeFrames(packets, encodedFormat));

  Microsoft::WRL::ComPtr<ID3D11Device> device;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
  D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  if (FAILED(::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
        &device, &level, &context))) {
    GTEST_SKIP() << "no Direct3D 11 device is available";
  }

  auto decoder = std::make_unique<WmfVideoDecoder>();
  ASSERT_TRUE(decoder->configure(encodedFormat, std::make_shared<TestHardwareVideoSurface>(device.Get())));
  decoder->start();

  int decodedFrames = 0;
  int hardwareFrames = 0;
  int textureFrames = 0;
  const auto drainOutput = [&]() {
    while (true) {
      auto output = decoder->dequeueOutputBuffer();
      if (output == nullptr) {
        return false;
      }
      if (output->isEndOfStream()) {
        output->release();
        return true;
      }
      ++decodedFrames;
      if (const auto hardware = std::dynamic_pointer_cast<HardwarePixelBuffer>(output);
          hardware != nullptr) {
        ++hardwareFrames;
        // The frame has to carry a texture of the device the caller handed in, in the format the decoder
        // negotiated. The pixels are checked further up the stack: `GpuVideoHardwareFrameSourceTest`
        // converts a decoded frame and reads it back, which proves the texture is readable and correctly
        // described, whereas a CPU-side copy here would only repeat the decoder's own work.
        auto* texture = static_cast<ID3D11Texture2D*>(hardware->getNativeTexture());
        EXPECT_NE(nullptr, texture);
        if (texture != nullptr) {
          Microsoft::WRL::ComPtr<ID3D11Device> textureDevice;
          texture->GetDevice(&textureDevice);
          EXPECT_EQ(device.Get(), textureDevice.Get());
          D3D11_TEXTURE2D_DESC description{};
          texture->GetDesc(&description);
          EXPECT_EQ(DXGI_FORMAT_NV12, description.Format);
          EXPECT_EQ(HardwarePixelBuffer::TextureFormat::kNv12, hardware->getTextureFormat());
          EXPECT_EQ(PixelFormat::kFormatNV12, hardware->getFormat());
          EXPECT_LT(hardware->getSubresource(), description.ArraySize);
          if (description.Format == DXGI_FORMAT_NV12) {
            ++textureFrames;
          }
        }
      }
      output->release();
    }
    return false;
  };

  for (const auto& packet: packets) {
    int guard = 0;
    while (true) {
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
      if (drainOutput() || ++guard > 1000) {
        break;
      }
    }
    drainOutput();
  }

  decoder->signalEndOfInputStream();
  int guard = 0;
  while (!drainOutput() && ++guard < 1000) {
  }
  decoder->stop();
  decoder->release();

  EXPECT_GE(decodedFrames, FRAME_COUNT - 2);
  if (hardwareFrames == 0) {
    // A machine whose decoder has no DXVA path (or a profile it cannot run there) hands out CPU frames,
    // which is the documented degradation; the test then has nothing to say about the texture path.
    GTEST_SKIP() << "this decoder produced CPU frames only, the hardware path is not available";
  }
  EXPECT_GE(hardwareFrames, decodedFrames - 1);
  // Every frame that arrived as a `HardwarePixelBuffer` has to hold an NV12 texture; a frame that claimed
  // to be hardware and carried something else would reach the converter with a format it cannot read.
  EXPECT_EQ(hardwareFrames, textureFrames);
}

TEST(D3D11VAVideoCodec, DecodesIntoDirect3D11Textures) {
  std::vector<EncodedPacket> packets;
  std::shared_ptr<MediaFormat> encodedFormat;
  ASSERT_TRUE(encodeFrames(packets, encodedFormat));

  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  if (FAILED(::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
        D3D11_SDK_VERSION, &device, &level, &context))) {
    GTEST_SKIP() << "no Direct3D 11 device with video support is available";
  }
  ComPtr<ID3D11VideoDevice> videoDevice;
  if (FAILED(device.As(&videoDevice)) || videoDevice == nullptr) {
    GTEST_SKIP() << "the device has no video decode interface";
  }
  constexpr GUID h264VldNoFgt = {
    0x1b81be68, 0xa0c7, 0x11d3, { 0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5 }
  };
  BOOL supportsNv12 = FALSE;
  if (FAILED(videoDevice->CheckVideoDecoderFormat(&h264VldNoFgt, DXGI_FORMAT_NV12, &supportsNv12)) ||
      !supportsNv12) {
    GTEST_SKIP() << "the device has no H.264 NV12 decode profile";
  }

  auto decoder = std::make_unique<D3D11VAVideoDecoder>();
  ASSERT_TRUE(decoder->configure(encodedFormat,
    std::make_shared<TestHardwareVideoSurface>(device.Get())));
  ASSERT_NE(decoder->getOutputFormat(), nullptr);
  EXPECT_EQ(decoder->getOutputFormat()->getPixelFormat(), PixelFormat::kFormatNV12);
  decoder->start();

  int decodedFrames = 0;
  int hardwareFrames = 0;
  double firstVariance = -1.0;
  double firstMean = 0.0;
  bool pixelsChecked = false;
  const auto drainOutput = [&]() {
    while (true) {
      auto output = decoder->dequeueOutputBuffer();
      if (output == nullptr) {
        return false;
      }
      if (output->isEndOfStream()) {
        output->release();
        return true;
      }
      ++decodedFrames;
      if (const auto hardware = std::dynamic_pointer_cast<HardwarePixelBuffer>(output);
          hardware != nullptr) {
        ++hardwareFrames;
        EXPECT_EQ(HardwarePixelBuffer::TextureFormat::kNv12, hardware->getTextureFormat());
        EXPECT_EQ(PixelFormat::kFormatNV12, hardware->getFormat());
        auto* texture = static_cast<ID3D11Texture2D*>(hardware->getNativeTexture());
        EXPECT_NE(nullptr, texture);
        if (texture != nullptr) {
          ComPtr<ID3D11Device> textureDevice;
          texture->GetDevice(&textureDevice);
          EXPECT_EQ(device.Get(), textureDevice.Get());
          D3D11_TEXTURE2D_DESC description{};
          texture->GetDesc(&description);
          EXPECT_EQ(DXGI_FORMAT_NV12, description.Format);
          EXPECT_LT(hardware->getSubresource(), description.ArraySize);
          if (!pixelsChecked) {
            pixelsChecked = true;
            if (!readLumaStats(hardware.get(), &firstVariance, &firstMean)) {
              firstVariance = -1.0;
            }
          }
        }
      }
      output->release();
    }
  };

  for (const auto& packet : packets) {
    int guard = 0;
    while (true) {
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
      if (drainOutput() || ++guard > 1000) {
        break;
      }
    }
    drainOutput();
  }
  decoder->signalEndOfInputStream();
  int guard = 0;
  while (!drainOutput() && ++guard < 1000) {
  }
  decoder->stop();
  decoder->release();

  EXPECT_GE(decodedFrames, FRAME_COUNT - 2);
  if (hardwareFrames == 0) {
    GTEST_SKIP() << "the D3D11VA decoder produced CPU frames only";
  }
  ASSERT_TRUE(pixelsChecked) << "no hardware frame carried a texture";
  // A texture whose luma plane has no variance is the driver's blank fill: the frame was handed out
  // but the decode never wrote a picture, which is exactly the failure the zero-copy chain shows.
  EXPECT_GT(firstVariance, 10.0) << "the decoded picture is blank (mean=" << firstMean << ")";
  EXPECT_EQ(hardwareFrames, decodedFrames);
}

TEST(D3D11VAVideoCodec, ProcessFormatConvertsToBgra) {
  std::vector<EncodedPacket> packets;
  std::shared_ptr<MediaFormat> encodedFormat;
  ASSERT_TRUE(encodeFrames(packets, encodedFormat));

  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  if (FAILED(::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
        D3D11_SDK_VERSION, &device, &level, &context))) {
    GTEST_SKIP() << "no Direct3D 11 device with video support is available";
  }

  auto format = encodedFormat->buildUpon();
  format->setProcessFormat(PixelFormat::kFormatBGRA);
  auto decoder = std::make_unique<D3D11VAVideoDecoder>();
  ASSERT_TRUE(decoder->configure(format, std::make_shared<TestHardwareVideoSurface>(device.Get())));
  ASSERT_NE(decoder->getOutputFormat(), nullptr);
  EXPECT_EQ(decoder->getOutputFormat()->getPixelFormat(), PixelFormat::kFormatBGRA);
  decoder->start();

  int hardwareFrames = 0;
  int bgraFrames = 0;
  const auto drainOutput = [&]() {
    while (true) {
      auto output = decoder->dequeueOutputBuffer();
      if (output == nullptr) {
        return false;
      }
      if (output->isEndOfStream()) {
        output->release();
        return true;
      }
      if (const auto hardware = std::dynamic_pointer_cast<HardwarePixelBuffer>(output);
          hardware != nullptr) {
        ++hardwareFrames;
        if (hardware->getTextureFormat() == HardwarePixelBuffer::TextureFormat::kRgba &&
            hardware->getFormat() == PixelFormat::kFormatBGRA) {
          auto* texture = static_cast<ID3D11Texture2D*>(hardware->getNativeTexture());
          if (texture != nullptr) {
            D3D11_TEXTURE2D_DESC description{};
            texture->GetDesc(&description);
            if (description.Format == DXGI_FORMAT_B8G8R8A8_UNORM) {
              ++bgraFrames;
            }
          }
        }
      }
      output->release();
    }
  };

  for (const auto& packet : packets) {
    int guard = 0;
    while (true) {
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
      if (drainOutput() || ++guard > 1000) {
        break;
      }
    }
    drainOutput();
  }
  decoder->signalEndOfInputStream();
  int guard = 0;
  while (!drainOutput() && ++guard < 1000) {
  }
  decoder->stop();
  decoder->release();

  if (hardwareFrames == 0) {
    GTEST_SKIP() << "the D3D11VA decoder produced CPU frames only";
  }
  // A device whose video processor cannot write BGRA degrades to NV12, which is the documented
  // behaviour; when the conversion is available every frame is one packed BGRA texture.
  if (bgraFrames == 0) {
    GTEST_SKIP() << "this device converts no NV12 to BGRA through its video processor";
  }
  EXPECT_EQ(hardwareFrames, bgraFrames);
}

TEST(D3D11VAVideoCodec, FallsBackWithoutAHardwareSurface) {
  std::vector<EncodedPacket> packets;
  std::shared_ptr<MediaFormat> encodedFormat;
  ASSERT_TRUE(encodeFrames(packets, encodedFormat));

  auto decoder = std::make_unique<D3D11VAVideoDecoder>();
  ASSERT_TRUE(decoder->configure(encodedFormat, nullptr));
  decoder->start();
  int width = 0;
  int height = 0;
  const auto frames = decodeFrames(decoder.get(), packets, &width, &height);
  decoder->stop();
  decoder->release();
  EXPECT_EQ(width, WIDTH);
  EXPECT_EQ(height, HEIGHT);
  EXPECT_GE(frames, FRAME_COUNT - 2);
}

TEST(WmfVideoCodec, EncodeDecodeRoundTrip) {
  std::vector<EncodedPacket> packets;
  std::shared_ptr<MediaFormat> encodedFormat;
  ASSERT_TRUE(encodeFrames(packets, encodedFormat));
  EXPECT_EQ(encodedFormat->getMimeType(), MimeTypes::VIDEO_H264);
  EXPECT_EQ(encodedFormat->getWidth(), WIDTH);
  EXPECT_EQ(encodedFormat->getHeight(), HEIGHT);

  auto decoder = std::make_unique<WmfVideoDecoder>();
  ASSERT_TRUE(decoder->configure(encodedFormat, nullptr));
  decoder->start();

  int decodedWidth = 0;
  int decodedHeight = 0;
  const auto decodedFrames = decodeFrames(decoder.get(), packets, &decodedWidth, &decodedHeight);
  decoder->stop();
  decoder->release();

  EXPECT_EQ(decodedWidth, WIDTH);
  EXPECT_EQ(decodedHeight, HEIGHT);
  // the h264 encoder delays the output of the frames, all of them are decoded.
  EXPECT_GE(decodedFrames, FRAME_COUNT - 2);
  EXPECT_LE(decodedFrames, FRAME_COUNT + 2);
}

TEST(WmfVideoCodec, TheFrameARecycledBufferHoldsDoesNotMarkTheFrameWhichReplacesIt) {
  std::vector<EncodedPacket> packets;
  std::shared_ptr<MediaFormat> encodedFormat;
  ASSERT_TRUE(encodeFrames(packets, encodedFormat));

  auto decoder = std::make_unique<WmfVideoDecoder>();
  ASSERT_TRUE(decoder->configure(encodedFormat, nullptr));
  decoder->start();

  int64_t taintedFrames = 0;
  int64_t decodedFrames = 0;
  decodeFramesWithDroppedFrames(decoder.get(), packets, &taintedFrames, &decodedFrames);
  decoder->stop();
  decoder->release();

  EXPECT_GE(decodedFrames, FRAME_COUNT - 2);
  // The decoder hands its frame buffers out again, and a frame which took the buffer of a dropped one
  // must not come out marked as discarded: the renderer drops it without rendering it otherwise.
  EXPECT_EQ(taintedFrames, 0)
      << taintedFrames << " of the " << decodedFrames
      << " decoded frames carried the state of the frame which used their buffer before";
}

TEST(WmfVideoCodec, ExtractAndDecodeMpeg4File) {
  const auto path = tempFilePath("video.m4v");
  {
    const WmfMediaScope mediaScope;
    ASSERT_TRUE(mediaScope.isStarted());
    ASSERT_TRUE(writeMpeg4File(path)) << "failed to write the mpeg4 file";
  }

  // the platform extractor demuxes the file, the video track contains the compressed packets.
  auto extractor = CreateBuiltinExtractorFactory()->createExtractor(path);
  ASSERT_NE(extractor, nullptr);
  auto output = std::make_shared<CollectingExtractorOutput>();
  extractor->init(*output);

  AVMediaExtractor::PositionHolder holder;
  int guard = 0;
  while (extractor->read(holder) == AVMediaExtractor::PlatformResult::RESULT_CONTINUE
      && ++guard < 100000) {
  }

  const auto* track = output->firstTrackOfType(MediaType::kMediaTypeVideo);
  ASSERT_NE(track, nullptr);
  ASSERT_NE(track->format, nullptr);
  EXPECT_EQ(track->format->getMimeType(), MimeTypes::VIDEO_H264);
  EXPECT_EQ(track->format->getWidth(), WIDTH);
  EXPECT_EQ(track->format->getHeight(), HEIGHT);
  ASSERT_FALSE(track->packets.empty());
  EXPECT_GE(track->packets.size(), FRAME_COUNT - 2);
  EXPECT_GT(track->keyFrameCount, 0);

  auto decoder = CreateBuiltinVideoCodecFactory()->createVideoDecoder(
    VideoCodecType::kVideoCodecTypeH264, CodecManufacturer::kHardware);
  ASSERT_NE(decoder, nullptr);
  ASSERT_TRUE(decoder->configure(track->format, nullptr));
  decoder->start();

  int decodedWidth = 0;
  int decodedHeight = 0;
  const auto decodedFrames = decodeFrames(decoder.get(), track->packets, &decodedWidth,
    &decodedHeight);
  decoder->stop();
  decoder->release();

  EXPECT_EQ(decodedWidth, WIDTH);
  EXPECT_EQ(decodedHeight, HEIGHT);
  EXPECT_GE(decodedFrames, FRAME_COUNT - 2);
  extractor->clear();
}

TEST(WmfVideoCodec, ExtractAudioAndVideoMpeg4File) {
  const auto path = tempFilePath("audio-video.m4v");
  {
    const WmfMediaScope mediaScope;
    ASSERT_TRUE(mediaScope.isStarted());
    ASSERT_TRUE(writeMpeg4File(path, true)) << "failed to write the mpeg4 file";
  }

  auto extractor = CreateBuiltinExtractorFactory()->createExtractor(path);
  ASSERT_NE(extractor, nullptr);
  auto output = std::make_shared<CollectingExtractorOutput>();
  extractor->init(*output);

  AVMediaExtractor::PositionHolder holder;
  int guard = 0;
  while (extractor->read(holder) == AVMediaExtractor::PlatformResult::RESULT_CONTINUE
      && ++guard < 100000) {
  }

  // both the audio and the video track are extracted.
  EXPECT_EQ(output->trackCount(), 2u);
  const auto* videoTrack = output->firstTrackOfType(MediaType::kMediaTypeVideo);
  const auto* audioTrack = output->firstTrackOfType(MediaType::kMediaTypeAudio);
  ASSERT_NE(videoTrack, nullptr);
  ASSERT_NE(audioTrack, nullptr);
  ASSERT_NE(videoTrack->format, nullptr);
  ASSERT_NE(audioTrack->format, nullptr);

  EXPECT_EQ(videoTrack->format->getMimeType(), MimeTypes::VIDEO_H264);
  EXPECT_EQ(videoTrack->format->getWidth(), WIDTH);
  EXPECT_EQ(videoTrack->format->getHeight(), HEIGHT);
  EXPECT_EQ(audioTrack->format->getMimeType(), MimeTypes::AUDIO_AAC);
  EXPECT_EQ(audioTrack->format->getSampleRate(), AUDIO_SAMPLE_RATE);
  EXPECT_EQ(audioTrack->format->getChannelCount(), AUDIO_CHANNEL_COUNT);

  EXPECT_GE(videoTrack->sampleCount, FRAME_COUNT - 2);
  EXPECT_GT(audioTrack->sampleCount, 0);
  EXPECT_NEAR(static_cast<double>(extractor->getDurationUs()), 1000000.0, 150000.0);

  // the samples of the streams are delivered interleaved by the presentation time.
  EXPECT_GE(videoTrack->firstTimeUs, 0);
  EXPECT_GE(audioTrack->firstTimeUs, 0);
  EXPECT_NEAR(static_cast<double>(videoTrack->lastTimeUs), 1000000.0, 150000.0);
  EXPECT_NEAR(static_cast<double>(audioTrack->lastTimeUs), 1000000.0, 150000.0);

  // both streams are decodable by the platform decoders.
  auto videoDecoder = CreateBuiltinVideoCodecFactory()->createVideoDecoder(
    VideoCodecType::kVideoCodecTypeH264, CodecManufacturer::kHardware);
  ASSERT_NE(videoDecoder, nullptr);
  ASSERT_TRUE(videoDecoder->configure(videoTrack->format, nullptr));
  videoDecoder->start();
  int width = 0;
  int height = 0;
  const auto decodedFrames = decodeFrames(videoDecoder.get(), videoTrack->packets, &width, &height);
  videoDecoder->stop();
  videoDecoder->release();
  EXPECT_EQ(width, WIDTH);
  EXPECT_EQ(height, HEIGHT);
  EXPECT_GE(decodedFrames, FRAME_COUNT - 2);

  auto audioDecoder = CreateBuiltinAudioCodecFactory()->createDecoder(
    audioTrack->format->getMimeType(), false);
  ASSERT_NE(audioDecoder, nullptr);
  ASSERT_TRUE(audioDecoder->configure(audioTrack->format));
  audioDecoder->start();
  const auto decodedAudioFrames = decodePackets(audioDecoder.get(), audioTrack->packets);
  audioDecoder->stop();
  audioDecoder->release();
  EXPECT_NEAR(static_cast<double>(decodedAudioFrames), AUDIO_SAMPLE_RATE, AUDIO_SAMPLE_RATE * 0.15);

  extractor->clear();
}

TEST(WmfVideoCodec, FactorySupportsH264) {
  auto factory = CreateBuiltinVideoCodecFactory();
  ASSERT_NE(factory, nullptr);
  EXPECT_TRUE(factory->isSupportedCodec(VideoCodecType::kVideoCodecTypeH264, true));
  EXPECT_TRUE(factory->isSupportedCodec(VideoCodecType::kVideoCodecTypeH264, false));
  EXPECT_NE(factory->createVideoDecoder(VideoCodecType::kVideoCodecTypeH264,
    CodecManufacturer::kHardware), nullptr);
  EXPECT_NE(factory->createVideoEncoder(VideoCodecType::kVideoCodecTypeH264,
    CodecManufacturer::kHardware, nullptr), nullptr);
}

TEST(WmfVideoCodec, RejectUnsupportedCodec) {
  auto factory = CreateBuiltinVideoCodecFactory();
  EXPECT_FALSE(factory->isSupportedCodec(VideoCodecType::kVideoCodecTypeVP8, true));
  EXPECT_EQ(factory->createVideoDecoder(VideoCodecType::kVideoCodecTypeVP8,
    CodecManufacturer::kHardware), nullptr);
}

} // namespace coral::codec::test

#endif // WINDOWS


