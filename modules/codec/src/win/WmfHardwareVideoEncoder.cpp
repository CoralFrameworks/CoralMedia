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

#include "WmfHardwareVideoEncoder.h"

#include "WmfMediaUtils.h"

#include <codec/HardwareVideo.h>
#include <codec/MediaCodecFactory.h>
#include <media/Definitions.h>
#include <minilog/log.h>

#include <codecapi.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <strmif.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace coral {

namespace {

using Microsoft::WRL::ComPtr;

constexpr int64_t kInputWaitMs = 2000;
constexpr int64_t kDrainWaitMs = 5000;
/** Matches `WindowsD3D11EncoderOutputNode::kTextureCount`: at most this many textures may be held. */
constexpr int64_t kTextureRingDepth = 3;
std::atomic<uint64_t> gQueuedTextureFrames { 0 };
constexpr LONGLONG kHundredNanosecondsPerSecond = 10'000'000;

bool subtypeFor(const VideoCodecType type, GUID& subtype) {
  return WmfMediaUtils::videoCodecTypeToSubtype(type, subtype);
}

/** One texture in the encoder input pool, exposed through the same abstraction a decoder uses. */
class EncoderInputBuffer final : public HardwarePixelBuffer,
                                 public std::enable_shared_from_this<EncoderInputBuffer> {
public:
  EncoderInputBuffer(Microsoft::WRL::ComPtr<ID3D11Texture2D> texture, const int width, const int height)
    : mTexture(std::move(texture)), mWidth(width), mHeight(height) {
  }

  [[nodiscard]] PixelFormat getFormat() const override {
    return PixelFormat::kFormatBGRA;
  }

  [[nodiscard]] int width() const override { return mWidth; }
  [[nodiscard]] int height() const override { return mHeight; }
  [[nodiscard]] void* getNativeTexture() const override {
    return const_cast<ID3D11Texture2D*>(mTexture.Get());
  }
  [[nodiscard]] uint32_t getSubresource() const override { return 0; }
  [[nodiscard]] TextureFormat getTextureFormat() const override { return TextureFormat::kRgba; }

  void release() override {
    // The encoder owns the texture; the pool slot is recycled by dequeueInputBuffer().
  }

  std::shared_ptr<AVPixelBuffer> asPixelBuffer() override { return shared_from_this(); }

  std::shared_ptr<PixelBuffer> asPixelBufferHandle() override { return nullptr; }

private:
  Microsoft::WRL::ComPtr<ID3D11Texture2D> mTexture;
  int mWidth = 0;
  int mHeight = 0;
};

Microsoft::WRL::ComPtr<IMFTransform> activateHardwareEncoder(const GUID& subtype) {
  MFT_REGISTER_TYPE_INFO output{ MFMediaType_Video, subtype };
  IMFActivate** activates = nullptr;
  UINT32 count = 0;
  const HRESULT enumerated = ::MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
    MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, nullptr, &output, &activates, &count);
  if (FAILED(enumerated) || count == 0 || activates == nullptr) {
    if (activates != nullptr) ::CoTaskMemFree(activates);
    return nullptr;
  }
  ComPtr<IMFTransform> transform;
  for (UINT32 index = 0; index < count && transform == nullptr; ++index) {
    ComPtr<IMFTransform> candidate;
    if (SUCCEEDED(activates[index]->ActivateObject(IID_PPV_ARGS(&candidate))) && candidate != nullptr) {
      transform = candidate;
    }
  }
  for (UINT32 index = 0; index < count; ++index) activates[index]->Release();
  ::CoTaskMemFree(activates);
  return transform;
}

} // namespace anonymous

WmfHardwareVideoEncoder::WmfHardwareVideoEncoder(const VideoCodecType codecType)
  : mCodecType(codecType) {
  mWmfStarted = WmfMediaUtils::startup();
  mPacketBuffer = std::make_shared<AVPacketBuffer>(MediaType::kMediaTypeVideo);
}

WmfHardwareVideoEncoder::~WmfHardwareVideoEncoder() {
  release();
  if (mWmfStarted) {
    WmfMediaUtils::shutdown();
  }
}

bool WmfHardwareVideoEncoder::configure(const std::shared_ptr<MediaFormat>& format,
  const std::shared_ptr<VideoSurface>& surface) {
  if (format == nullptr) {
    MINILOG_ERROR("the hardware encoder has no output format");
    return false;
  }
  auto* const hardwareSurface = dynamic_cast<HardwareVideoSurface*>(surface.get());
  auto* const nativeDevice = hardwareSurface != nullptr ? hardwareSurface->getNativeDevice() : nullptr;
  if (nativeDevice == nullptr) {
    MINILOG_ERROR("the hardware encoder needs a HardwareVideoSurface with a D3D11 device");
    return false;
  }
  auto* const device = static_cast<ID3D11Device*>(nativeDevice);
  device->AddRef();
  mDevice.Attach(device);

  auto codecType = format->getVideoCodecType();
  if (codecType == VideoCodecType::kVideoCodecTypeInvalid) {
    codecType = mCodecType;
  }
  if (codecType == VideoCodecType::kVideoCodecTypeInvalid) {
    MINILOG_ERROR("the hardware encoder has no codec type");
    return false;
  }
  mCodecType = codecType;

  mWidth = format->getWidth() != MediaFormat::NO_VALUE ? format->getWidth() : 0;
  mHeight = format->getHeight() != MediaFormat::NO_VALUE ? format->getHeight() : 0;
  if (mWidth <= 0 || mHeight <= 0) {
    MINILOG_ERROR("the hardware encoder has no frame size");
    return false;
  }
  mWidth &= ~1;
  mHeight &= ~1;
  mFrameRate = format->getFrameRate() != MediaFormat::NO_VALUE && format->getFrameRate() > 0
                 ? format->getFrameRate()
                 : 30;
  mInputFormat = format;

  GUID outputSubtype{};
  if (!subtypeFor(mCodecType, outputSubtype)) {
    MINILOG_ERROR("the hardware encoder does not support {}", convertToText(mCodecType));
    return false;
  }
  mTransform = activateHardwareEncoder(outputSubtype);
  if (mTransform == nullptr) {
    MINILOG_WARN("no hardware encoder MFT is available for {}", convertToText(mCodecType));
    return false;
  }

  ComPtr<IMFAttributes> attributes;
  if (SUCCEEDED(mTransform->GetAttributes(&attributes)) && attributes != nullptr) {
    attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
  }

  if (FAILED(::MFCreateDXGIDeviceManager(&mResetToken, &mDeviceManager)) ||
      mDeviceManager == nullptr) {
    MINILOG_ERROR("the hardware encoder could not create a D3D11 device manager");
    release();
    return false;
  }
  if (FAILED(mDeviceManager->ResetDevice(mDevice.Get(), mResetToken))) {
    MINILOG_ERROR("the hardware encoder could not attach its D3D11 device");
    release();
    return false;
  }
  if (FAILED(mTransform->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
        reinterpret_cast<ULONG_PTR>(mDeviceManager.Get())))) {
    MINILOG_ERROR("the hardware encoder refused the D3D11 device manager");
    release();
    return false;
  }

  ComPtr<ICodecAPI> codecApi;
  if (SUCCEEDED(mTransform->QueryInterface(IID_PPV_ARGS(&codecApi))) && codecApi != nullptr) {
    VARIANT value;
    VariantInit(&value);
    value.vt = VT_UI4;
    value.ulVal = 0;
    codecApi->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &value);
    VariantClear(&value);
  }

  if (!configureTypes()) {
    release();
    return false;
  }

  D3D11_TEXTURE2D_DESC poolDescription{};
  poolDescription.Width = static_cast<UINT>(mWidth);
  poolDescription.Height = static_cast<UINT>(mHeight);
  poolDescription.MipLevels = 1;
  poolDescription.ArraySize = 1;
  poolDescription.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  poolDescription.SampleDesc.Count = 1;
  poolDescription.Usage = D3D11_USAGE_DEFAULT;
  poolDescription.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  mInputSlots.resize(static_cast<size_t>(kTextureRingDepth));
  for (InputSlot& slot: mInputSlots) {
    if (FAILED(mDevice->CreateTexture2D(&poolDescription, nullptr, &slot.texture)) ||
        slot.texture == nullptr) {
      MINILOG_ERROR("the hardware encoder could not create its input texture pool");
      release();
      return false;
    }
    slot.buffer = std::make_shared<EncoderInputBuffer>(slot.texture, mWidth, mHeight);
    if (slot.buffer == nullptr) {
      release();
      return false;
    }
  }
  MINILOG_INFO("the Windows hardware encoder is {} for {}x{} at {} fps", convertToText(mCodecType),
    mWidth, mHeight, mFrameRate);

  if (FAILED(mTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0))) {
    MINILOG_ERROR("the hardware encoder could not begin streaming");
    release();
    return false;
  }
  mTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

  if (FAILED(mTransform.As(&mEvents)) || mEvents == nullptr) {
    MINILOG_ERROR("the hardware encoder has no event generator");
    release();
    return false;
  }
  mEvents->BeginGetEvent(nullptr, nullptr);
  mConfigured = true;
  return true;
}

bool WmfHardwareVideoEncoder::configureTypes() {
  GUID outputSubtype{};
  if (!subtypeFor(mCodecType, outputSubtype)) {
    return false;
  }
  auto bitrate = mInputFormat->getBitrate();
  if (bitrate == MediaFormat::NO_VALUE || bitrate <= 0) {
    bitrate = 4'000'000;
  }

  ComPtr<IMFMediaType> outputType;
  if (FAILED(::MFCreateMediaType(&outputType)) || outputType == nullptr) {
    return false;
  }
  outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  outputType->SetGUID(MF_MT_SUBTYPE, outputSubtype);
  outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  outputType->SetUINT64(MF_MT_FRAME_SIZE, ::Pack2UINT32AsUINT64(static_cast<UINT32>(mWidth),
    static_cast<UINT32>(mHeight)));
  outputType->SetUINT64(MF_MT_FRAME_RATE, ::Pack2UINT32AsUINT64(static_cast<UINT32>(mFrameRate), 1));
  outputType->SetUINT64(MF_MT_PIXEL_ASPECT_RATIO, ::Pack2UINT32AsUINT64(1, 1));
  outputType->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(bitrate));
  if (FAILED(mTransform->SetOutputType(0, outputType.Get(), 0))) {
    MINILOG_ERROR("the hardware encoder refused the H.264/HEVC output type");
    return false;
  }

  // The engine produces BGRA8. `MFVideoFormat_ARGB32` names the same byte order to WMF, and the
  // hardware MFTs accept it as a texture input; the bind flag lets the MFT read a render target.
  ComPtr<IMFMediaType> inputType;
  if (FAILED(::MFCreateMediaType(&inputType)) || inputType == nullptr) {
    return false;
  }
  inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32);
  inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  inputType->SetUINT64(MF_MT_FRAME_SIZE, ::Pack2UINT32AsUINT64(static_cast<UINT32>(mWidth),
    static_cast<UINT32>(mHeight)));
  inputType->SetUINT64(MF_MT_FRAME_RATE, ::Pack2UINT32AsUINT64(static_cast<UINT32>(mFrameRate), 1));
  inputType->SetUINT64(MF_MT_PIXEL_ASPECT_RATIO, ::Pack2UINT32AsUINT64(1, 1));
  inputType->SetUINT32(MF_SA_D3D11_BINDFLAGS, D3D11_BIND_RENDER_TARGET);
  if (FAILED(mTransform->SetInputType(0, inputType.Get(), 0))) {
    MINILOG_ERROR("the hardware encoder refused the BGRA8 texture input type");
    return false;
  }

  ComPtr<IMFMediaType> actual;
  if (SUCCEEDED(mTransform->GetOutputCurrentType(0, &actual)) && actual != nullptr) {
    const auto format = WmfMediaUtils::convertToMediaFormat(actual.Get(), true);
    if (format != nullptr) {
      format->setVideoCodecType(mCodecType);
      format->setWidth(mWidth);
      format->setHeight(mHeight);
      format->setFrameRate(mFrameRate);
      mOutputFormat = format;
    }
  }
  return mOutputFormat != nullptr;
}

bool WmfHardwareVideoEncoder::refreshOutputFormat() {
  ComPtr<IMFMediaType> outputType;
  if (FAILED(mTransform->GetOutputCurrentType(0, &outputType)) || outputType == nullptr) {
    return false;
  }
  const auto format = WmfMediaUtils::convertToMediaFormat(outputType.Get(), true);
  if (format == nullptr) {
    return false;
  }
  format->setVideoCodecType(mCodecType);
  format->setWidth(mWidth);
  format->setHeight(mHeight);
  format->setFrameRate(mFrameRate);
  mOutputFormat = format;
  MINILOG_DEBUG("hardware encoder output format: {}", mOutputFormat->toString());
  return true;
}

bool WmfHardwareVideoEncoder::pumpEvents(const bool wait, const uint32_t timeoutMilliseconds) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeoutMilliseconds);
  for (;;) {
    ComPtr<IMFMediaEvent> event;
    const HRESULT result = mEvents->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
    if (result == MF_E_NO_EVENTS_AVAILABLE) {
      if (!wait) {
        return true;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }
    if (FAILED(result) || event == nullptr) {
      return false;
    }

    MediaEventType type = MEUnknown;
    event->GetType(&type);
    switch (type) {
      case METransformNeedInput:
        // The encoder has consumed the previous input; its texture and sample can return to the
        // pool, which is the backpressure the writer waits on in dequeueInputBuffer().
        if (!mInFlightSlots.empty()) {
          mInputSlots[mInFlightSlots.front()].inFlight = false;
          mInFlightSlots.pop_front();
        }
        if (!mPendingInputSamples.empty()) {
          mPendingInputSamples.pop_front();
        }
        mNeedInput = true;
        break;
      case METransformHaveOutput:
        if (!drainOutputs()) {
          return false;
        }
        break;
      case METransformDrainComplete:
        mDrainComplete = true;
        break;
      case MEError:
        MINILOG_ERROR("the hardware encoder reported an error");
        return false;
      default:
        break;
    }
    mEvents->BeginGetEvent(nullptr, nullptr);
  }
}

bool WmfHardwareVideoEncoder::drainOutputs() {
  for (;;) {
    MFT_OUTPUT_DATA_BUFFER output{};
    output.dwStreamID = 0;
    DWORD status = 0;
    const HRESULT result = mTransform->ProcessOutput(0, 1, &output, &status);
    if (result == MF_E_TRANSFORM_NEED_MORE_INPUT) {
      return true;
    }
    if (result == MF_E_TRANSFORM_STREAM_CHANGE) {
      refreshOutputFormat();
      continue;
    }
    if (result == E_UNEXPECTED) {
      // The MFT reports no more output for the event we just handled; it is not a failure.
      return true;
    }
    if (FAILED(result)) {
      MINILOG_ERROR("the hardware encoder failed to produce a packet: 0x{:08x}",
        static_cast<unsigned>(result));
      return false;
    }
    if (output.pSample != nullptr) {
      ComPtr<IMFSample> sample;
      sample.Attach(output.pSample);
      mEncodedSamples.push_back(std::move(sample));
    }
    if (output.pEvents != nullptr) {
      output.pEvents->Release();
    }
  }
}

bool WmfHardwareVideoEncoder::waitForInput(const uint32_t timeoutMilliseconds) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeoutMilliseconds);
  while (!mNeedInput) {
    if (mEndOfStream || !pumpEvents(true, 5)) {
      return false;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
  }
  return true;
}

bool WmfHardwareVideoEncoder::queueHardwareTexture(void* const texture, const int64_t timeUs) {
  const std::lock_guard<std::mutex> lock(mMutex);
  if (!mConfigured || mEndOfStream || mTransform == nullptr || texture == nullptr) {
    return false;
  }

  auto* const d3dTexture = static_cast<ID3D11Texture2D*>(texture);
  ComPtr<IMFMediaBuffer> buffer;
  HRESULT result = ::MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), d3dTexture, 0, FALSE, &buffer);
  if (FAILED(result) || buffer == nullptr) {
    MINILOG_ERROR("the hardware encoder could not wrap the D3D11 texture");
    return false;
  }
  ComPtr<IMFSample> sample;
  result = ::MFCreateSample(&sample);
  if (FAILED(result) || sample == nullptr) {
    return false;
  }
  sample->AddBuffer(buffer.Get());
  sample->SetSampleTime(timeUs * 10);
  sample->SetSampleDuration(kHundredNanosecondsPerSecond / mFrameRate);

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kInputWaitMs);
  for (;;) {
    result = mTransform->ProcessInput(0, sample.Get(), 0);
    if (result != MF_E_NOTACCEPTING) {
      break;
    }
    mNeedInput = false;
    if (std::chrono::steady_clock::now() >= deadline) {
      MINILOG_ERROR("the hardware encoder was still not accepting input");
      return false;
    }
    pumpEvents(true, 5);
  }
  if (FAILED(result)) {
    MINILOG_ERROR("the hardware encoder refused a texture: 0x{:08x}", static_cast<unsigned>(result));
    return false;
  }
  mNeedInput = false;
  mNextInputTimeUs = timeUs;
  mPendingInputSamples.push_back(sample);
  while (mPendingInputSamples.size() > static_cast<size_t>(kTextureRingDepth) + 1) {
    mPendingInputSamples.pop_front();
  }
  for (size_t index = 0; index < mInputSlots.size(); ++index) {
    if (mInputSlots[index].texture.Get() == texture) {
      mInFlightSlots.push_back(index);
      break;
    }
  }
  return true;
}

std::shared_ptr<AVPixelBuffer> WmfHardwareVideoEncoder::dequeueInputBuffer() {
  const std::lock_guard<std::mutex> lock(mMutex);
  if (!mConfigured || mEndOfStream) {
    return nullptr;
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kInputWaitMs);
  for (;;) {
    pumpEvents(false, 0);
    for (InputSlot& slot: mInputSlots) {
      if (!slot.inFlight) {
        slot.inFlight = true;
        slot.buffer->timeUs(Definitions::kTimeUnset);
        return slot.buffer;
      }
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return nullptr;
    }
    if (!pumpEvents(true, 5)) {
      return nullptr;
    }
  }
}

void WmfHardwareVideoEncoder::queueInputBuffer(std::shared_ptr<AVPixelBuffer> inputBuffer) {
  if (inputBuffer == nullptr) {
    return;
  }
  if (inputBuffer->isEndOfStream()) {
    signalEndOfInputStream();
    return;
  }
  auto hardware = std::dynamic_pointer_cast<HardwarePixelBuffer>(inputBuffer);
  if (hardware == nullptr) {
    MINILOG_WARN("the hardware encoder received a CPU buffer; it needs a HardwarePixelBuffer");
    return;
  }
  const int64_t timeUs = inputBuffer->timeUs() != Definitions::kTimeUnset
                           ? inputBuffer->timeUs()
                           : mNextInputTimeUs;
  queueHardwareTexture(hardware->getNativeTexture(), timeUs);
}

std::shared_ptr<AVPacketBuffer> WmfHardwareVideoEncoder::dequeueOutputBuffer() {
  const std::lock_guard<std::mutex> lock(mMutex);
  if (!mConfigured) {
    return nullptr;
  }
  pumpEvents(false, 0);

  if (mEncodedSamples.empty()) {
    if (mEndOfStream && mDrainComplete) {
      return createEndOfStreamBuffer();
    }
    return nullptr;
  }

  ComPtr<IMFSample> sample = mEncodedSamples.front();
  mEncodedSamples.pop_front();

  if (!mFormatReported && mOutputFormat != nullptr) {
    mFormatReported = true;
    mPacketBuffer->clear();
    mPacketBuffer->setFormat(mOutputFormat);
    mEncodedSamples.push_front(sample);
    return mPacketBuffer;
  }

  ComPtr<IMFMediaBuffer> buffer;
  if (FAILED(sample->ConvertToContiguousBuffer(&buffer)) || buffer == nullptr) {
    return nullptr;
  }
  BYTE* data = nullptr;
  DWORD maxLength = 0;
  DWORD currentLength = 0;
  if (FAILED(buffer->Lock(&data, &maxLength, &currentLength))) {
    return nullptr;
  }

  mPacketBuffer->clear();
  LONGLONG time = 0;
  mPacketBuffer->timeUs(SUCCEEDED(sample->GetSampleTime(&time)) ? time / 10 : Definitions::kTimeUnset);
  mPacketBuffer->ensureSpaceForWrite(currentLength);
  const auto outputData = mPacketBuffer->data();
  outputData->position(0);
  if (currentLength > 0) {
    outputData->put(data, currentLength);
  }
  outputData->flip();
  UINT32 cleanPoint = 0;
  if (SUCCEEDED(sample->GetUINT32(MFSampleExtension_CleanPoint, &cleanPoint)) && cleanPoint) {
    mPacketBuffer->addFlag(AVBufferFlags::kKeyFrame);
  }
  buffer->Unlock();
  return mPacketBuffer;
}

void WmfHardwareVideoEncoder::releaseOutputBuffer(
  const std::shared_ptr<AVPacketBuffer>& outputBuffer, const bool render) {
  static_cast<void>(outputBuffer);
  static_cast<void>(render);
}

void WmfHardwareVideoEncoder::signalEndOfInputStream() {
  const std::lock_guard<std::mutex> lock(mMutex);
  if (mEndOfStream || mTransform == nullptr) {
    return;
  }
  mEndOfStream = true;
  mTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
  mTransform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(kDrainWaitMs);
  while (!mDrainComplete && std::chrono::steady_clock::now() < deadline) {
    pumpEvents(true, 5);
  }
}

std::shared_ptr<AVPacketBuffer> WmfHardwareVideoEncoder::createEndOfStreamBuffer() {
  mPacketBuffer->clear();
  mPacketBuffer->setFlags(AVBufferFlags::kEndOfStream);
  return mPacketBuffer;
}

void WmfHardwareVideoEncoder::start() {
  mStarted = true;
}

void WmfHardwareVideoEncoder::stop() {
  // The writer stops the encoder after its feeding thread has joined, so no frame is in flight and
  // no lock is needed around the drain. Taking one here would deadlock because the drain pumps
  // output events through the same mutex.
  if (!mEndOfStream) {
    signalEndOfInputStream();
  }
}

void WmfHardwareVideoEncoder::flush() {
  const std::lock_guard<std::mutex> lock(mMutex);
  if (mTransform != nullptr) {
    mTransform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
  }
  mEncodedSamples.clear();
  mPendingInputSamples.clear();
  mEndOfStream = false;
  mDrainComplete = false;
  mNeedInput = false;
  mFormatReported = false;
}

void WmfHardwareVideoEncoder::release() {
  const std::lock_guard<std::mutex> lock(mMutex);
  mEvents.Reset();
  if (mTransform != nullptr) {
    mTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    mTransform.Reset();
  }
  mEncodedSamples.clear();
  mPendingInputSamples.clear();
  mInFlightSlots.clear();
  mInputSlots.clear();
  mDeviceManager.Reset();
  mDevice.Reset();
  mPacketBuffer.reset();
  mOutputFormat.reset();
  mConfigured = false;
  mStarted = false;
  mEndOfStream = true;
  mDrainComplete = true;
}

bool WmfHardwareVideoEncoder::isSupported(const VideoCodecType formatType) {
  GUID subtype{};
  if (!subtypeFor(formatType, subtype)) {
    return false;
  }
  const bool started = WmfMediaUtils::startup();
  MFT_REGISTER_TYPE_INFO output{ MFMediaType_Video, subtype };
  IMFActivate** activates = nullptr;
  UINT32 count = 0;
  const HRESULT result = ::MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
    MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, nullptr, &output, &activates, &count);
  if (activates != nullptr) {
    for (UINT32 index = 0; index < count; ++index) activates[index]->Release();
    ::CoTaskMemFree(activates);
  }
  if (started) {
    WmfMediaUtils::shutdown();
  }
  return SUCCEEDED(result) && count > 0;
}

} // namespace coral






