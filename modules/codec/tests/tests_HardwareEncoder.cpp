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

#if defined(_WIN32)

#include <codec/HardwareVideo.h>
#include <codec/MediaCodec.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <memory>

using namespace coral;

namespace {

/** A minimal HardwarePixelBuffer backed by a D3D11 NV12 texture. */
class TestHardwarePixelBuffer final : public HardwarePixelBuffer,
                                      public std::enable_shared_from_this<TestHardwarePixelBuffer> {
public:
  TestHardwarePixelBuffer(Microsoft::WRL::ComPtr<ID3D11Texture2D> texture, const int width,
      const int height)
    : mTexture(std::move(texture)),
      mWidth(width),
      mHeight(height) {
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
    return const_cast<ID3D11Texture2D*>(mTexture.Get());
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
  Microsoft::WRL::ComPtr<ID3D11Texture2D> mTexture;
  int mWidth = 0;
  int mHeight = 0;
};

/** An encoder whose input is a GPU texture, used to exercise the hardware input contract. */
class TestHardwareVideoEncoder final : public VideoEncoder {
public:
  TestHardwareVideoEncoder(std::shared_ptr<TestHardwarePixelBuffer> input, const int width,
      const int height)
    : mInput(std::move(input)),
      mWidth(width),
      mHeight(height) {
  }

  bool configure(const std::shared_ptr<MediaFormat>& format,
      const std::shared_ptr<VideoSurface>&) override {
    mFormat = format;
    return format != nullptr;
  }

  void start() override {
    mStarted = true;
  }

  void stop() override {
    mStarted = false;
  }

  void flush() override {
  }

  void release() override {
    mInput.reset();
  }

  std::shared_ptr<AVPixelBuffer> dequeueInputBuffer() override {
    return mStarted ? mInput : nullptr;
  }

  void queueInputBuffer(std::shared_ptr<AVPixelBuffer> inputBuffer) override {
    mQueued = std::move(inputBuffer);
    mQueuedTimeUs = mQueued != nullptr ? mQueued->timeUs() : 0;
  }

  std::shared_ptr<AVPacketBuffer> dequeueOutputBuffer() override {
    return nullptr;
  }

  void releaseOutputBuffer(const std::shared_ptr<AVPacketBuffer>&, bool) override {
  }

  void signalEndOfInputStream() override {
  }

  [[nodiscard]] const std::shared_ptr<MediaFormat>& getOutputFormat() const override {
    return mFormat;
  }

  [[nodiscard]] const char* getName() const override {
    return "TestHardwareVideoEncoder";
  }

  void setOutputStartTimeUs(int64_t) {
  }

  [[nodiscard]] bool queued() const {
    return mQueued != nullptr;
  }

  [[nodiscard]] int64_t queuedTimeUs() const {
    return mQueuedTimeUs;
  }

  [[nodiscard]] int width() const {
    return mWidth;
  }

  [[nodiscard]] int height() const {
    return mHeight;
  }

private:
  std::shared_ptr<TestHardwarePixelBuffer> mInput;
  std::shared_ptr<AVPixelBuffer> mQueued;
  std::shared_ptr<MediaFormat> mFormat;
  int mWidth = 0;
  int mHeight = 0;
  bool mStarted = false;
  int64_t mQueuedTimeUs = 0;
};

} // namespace anonymous

TEST(CodecHardwareEncoderTest, DequeuesAHardwarePixelBufferInput) {
  Microsoft::WRL::ComPtr<ID3D11Device> device;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
  D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &device, &level, &context))) {
    GTEST_SKIP() << "no D3D11 device";
  }

  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = 64;
  desc.Height = 64;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_NV12;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
  ASSERT_TRUE(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture)));

  auto input = std::make_shared<TestHardwarePixelBuffer>(texture, 64, 64);
  auto encoder = std::make_shared<TestHardwareVideoEncoder>(input, 64, 64);
  auto format = MediaFormat::CreateVideoFormat("video/avc", 64, 64);
  ASSERT_TRUE(encoder->configure(format, nullptr));
  encoder->start();

  auto buffer = encoder->dequeueInputBuffer();
  ASSERT_NE(buffer, nullptr);
  auto hardware = std::dynamic_pointer_cast<HardwarePixelBuffer>(buffer);
  ASSERT_NE(hardware, nullptr);
  EXPECT_EQ(hardware->getNativeTexture(), texture.Get());
  EXPECT_EQ(hardware->getTextureFormat(), HardwarePixelBuffer::TextureFormat::kNv12);
  EXPECT_EQ(hardware->width(), 64);
  EXPECT_EQ(hardware->height(), 64);

  buffer->timeUs(1234);
  encoder->queueInputBuffer(buffer);
  EXPECT_TRUE(encoder->queued());
  EXPECT_EQ(encoder->queuedTimeUs(), 1234);
  encoder->stop();
}

#endif // defined(_WIN32)
