/*
 * Copyright (C) 2022 The Coral Project
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

#include <media/PixelBuffer.h>

namespace coral {

namespace {
class FlagsFrameBuffer final : public AVPixelBuffer {
public:
  FlagsFrameBuffer() = default;

  ~FlagsFrameBuffer() override = default;

  [[nodiscard]] PixelFormat getFormat() const override { return PixelFormat::kFormatInvalid; }

  [[nodiscard]] int width() const override { return 0; }

  [[nodiscard]] int height() const override { return 0; }

  [[nodiscard]] int64_t timeUs() const override { return Definitions::kTimeUnset; }

  void release() override {
    // Do nothing
  }
};
} // namespace anonymous

std::shared_ptr<AVPixelBuffer> AVPixelBuffer::newFlagsOnlyInstance() {
  return std::make_shared<FlagsFrameBuffer>();
}

AVPixelBuffer::~AVPixelBuffer() = default;

I420PixelBuffer::I420PixelBuffer(Owner <AVPixelBuffer>* owner,
    int width,
    int height,
    int strideY,
    int strideU,
    int strideV) noexcept
    : owner(owner),
      mWidth(width),
      mHeight(height),
      mStrideY(strideY),
      mStrideU(strideU),
      mStrideV(strideV),
      mSize(I420DataSize(height, strideY, strideU, mStrideV)) {
  mData = std::make_unique<uint8_t[]>(mSize);
}

I420PixelBuffer::~I420PixelBuffer() {
  owner = nullptr;
}

NV12PixelBuffer::NV12PixelBuffer(Owner <AVPixelBuffer>* owner,
    int width,
    int height,
    int strideY,
    int strideUV) noexcept
    : owner(owner),
      width_(width),
      height_(height),
      stride_y_(strideY),
      stride_uv_(strideUV),
      mSize(NV12DataSize(height, strideY, strideUV)) {
  mData = std::make_unique<uint8_t[]>(mSize);
}

NV12PixelBuffer::~NV12PixelBuffer() {
  owner = nullptr;
}

} // namespace coral
