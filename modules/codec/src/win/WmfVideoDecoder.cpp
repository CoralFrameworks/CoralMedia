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

#include "WmfVideoDecoder.h"

#include "WmfHardwarePixelBuffer.h"
#include "WmfMediaUtils.h"

#include <codec/HardwareVideo.h>

#include <media/Definitions.h>
#include <minilog/log.h>

#include <mfapi.h>
#include <mfobjects.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace coral {

namespace {

constexpr int DEFAULT_MAX_INPUT_SIZE = 2048 * 2048;
constexpr size_t MAX_POOLED_BUFFERS = 4;

/** The number of decoded frames that are taken out of the decoder and held at most. */
constexpr size_t MAX_PENDING_FRAMES = 8;

/** The number of input samples the transform may accept before it has to produce a frame. */
constexpr int MAX_INPUTS_AHEAD = 32;
constexpr int MAX_OUTSTANDING_FRAMES = 4;

/**
 * Whether the codec private data is an Annex B byte stream rather than a length prefixed one.
 */
bool isAnnexB(const std::vector<uint8_t>& data) {
  return data.size() >= 4 && data[0] == 0 && data[1] == 0
         && (data[2] == 1 || (data[2] == 0 && data[3] == 1));
}

/**
 * Converts the H.264 parameter sets from the Annex B byte stream form of Matroska to the
 * length-prefixed form of {@code MF_MT_MPEG_SEQUENCE_HEADER}.
 *
 * <p>The Media Foundation decoder accepts the sequence header of the samples it is given, which
 * are length-prefixed, but a Matroska track stores its codec private data with start codes. The
 * decoder otherwise accepts the input type and produces no frame at all.
 */
std::vector<uint8_t> normalizeH264SequenceHeader(const std::vector<uint8_t>& data) {
  const auto findStartCode = [&data](const size_t from) -> size_t {
    for (size_t i = from; i + 2 < data.size(); ++i) {
      if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
        return i;
      }
    }
    return std::string::npos;
  };

  const size_t firstStartCode = findStartCode(0);
  if (firstStartCode == std::string::npos) {
    // Already length-prefixed, or a form the decoder understands unchanged.
    return data;
  }

  std::vector<uint8_t> output;
  output.reserve(data.size());
  size_t startCode = firstStartCode;
  while (startCode != std::string::npos) {
    const size_t nalStart = startCode + 3;
    const size_t nextStartCode = findStartCode(nalStart);
    size_t nalEnd = nextStartCode == std::string::npos ? data.size() : nextStartCode;
    // A four byte start code has a zero in front of the three byte one, which is not part of the
    // preceding NAL unit.
    if (nalEnd > nalStart && nalEnd == nextStartCode && data[nalEnd - 1] == 0) {
      --nalEnd;
    }
    if (nalEnd > nalStart) {
      const uint32_t length = static_cast<uint32_t>(nalEnd - nalStart);
      output.push_back(static_cast<uint8_t>(length >> 24));
      output.push_back(static_cast<uint8_t>(length >> 16));
      output.push_back(static_cast<uint8_t>(length >> 8));
      output.push_back(static_cast<uint8_t>(length));
      output.insert(output.end(), data.begin() + static_cast<ptrdiff_t>(nalStart),
        data.begin() + static_cast<ptrdiff_t>(nalEnd));
    }
    startCode = nextStartCode;
  }
  return output;
}

/**
 * Returns the color space of the video output from the attributes of the media type, which is what
 * the renderers need to convert the decoded frames to RGB with the right matrix.
 *
 * The decoders of the platform do not always describe the output, the color space stays unknown then
 * and the renderer picks the matrix of the definition of the video.
 */
ColorInfo colorInfoOf(IMFMediaType* mediaType) {
  ColorInfo colorInfo;

  UINT32 transferMatrix = MFVideoTransferMatrix_Unknown;
  if (SUCCEEDED(mediaType->GetUINT32(MF_MT_YUV_MATRIX, &transferMatrix))) {
    if (transferMatrix == MFVideoTransferMatrix_BT709) {
      colorInfo.colorSpace = ColorInfo::SpaceType::kSpaceBT709;
    } else if (transferMatrix == MFVideoTransferMatrix_BT601) {
      colorInfo.colorSpace = ColorInfo::SpaceType::kSpaceBT601;
    } else if (transferMatrix == MFVideoTransferMatrix_BT2020_10 ||
               transferMatrix == MFVideoTransferMatrix_BT2020_12) {
      colorInfo.colorSpace = ColorInfo::SpaceType::kSpaceBT2020;
    }
  }

  if (colorInfo.colorSpace == ColorInfo::SpaceType::kSpaceUnknown) {
    UINT32 primaries = MFVideoPrimaries_Unknown;
    if (SUCCEEDED(mediaType->GetUINT32(MF_MT_VIDEO_PRIMARIES, &primaries))) {
      if (primaries == MFVideoPrimaries_BT709) {
        colorInfo.colorSpace = ColorInfo::SpaceType::kSpaceBT709;
      } else if (primaries == MFVideoPrimaries_BT470_2_SysM ||
                 primaries == MFVideoPrimaries_BT470_2_SysBG ||
                 primaries == MFVideoPrimaries_SMPTE170M) {
        colorInfo.colorSpace = ColorInfo::SpaceType::kSpaceBT601;
      } else if (primaries == MFVideoPrimaries_BT2020) {
        colorInfo.colorSpace = ColorInfo::SpaceType::kSpaceBT2020;
      }
    }
  }

  UINT32 nominalRange = MFNominalRange_Unknown;
  if (SUCCEEDED(mediaType->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &nominalRange))) {
    if (nominalRange == MFNominalRange_0_255) {
      colorInfo.colorRange = ColorInfo::RangeType::kRangeFull;
    } else if (nominalRange == MFNominalRange_16_235 || nominalRange == MFNominalRange_48_208) {
      colorInfo.colorRange = ColorInfo::RangeType::kRangeLimited;
    }
  }

  UINT32 transferFunction = MFVideoTransFunc_Unknown;
  if (SUCCEEDED(mediaType->GetUINT32(MF_MT_TRANSFER_FUNCTION, &transferFunction))) {
    if (transferFunction == MFVideoTransFunc_2084) {
      colorInfo.colorTransfer = ColorInfo::TransferType::kTransferST2084;
    } else if (transferFunction == MFVideoTransFunc_HLG) {
      colorInfo.colorTransfer = ColorInfo::TransferType::kTransferHLG;
    } else if (transferFunction == MFVideoTransFunc_sRGB) {
      colorInfo.colorTransfer = ColorInfo::TransferType::kTransferSRGB;
    } else if (transferFunction == MFVideoTransFunc_22) {
      colorInfo.colorTransfer = ColorInfo::TransferType::kTransferGamma22;
    } else if (transferFunction == MFVideoTransFunc_10) {
      colorInfo.colorTransfer = ColorInfo::TransferType::kTransferLinear;
    } else if (transferFunction == MFVideoTransFunc_709 ||
               transferFunction == MFVideoTransFunc_2020 ||
               transferFunction == MFVideoTransFunc_240M) {
      colorInfo.colorTransfer = ColorInfo::TransferType::kTransferSDR;
    }
  }
  return colorInfo;
}

} // namespace anonymous

WmfVideoDecoder::WmfVideoDecoder() {
  mWmfStarted = WmfMediaUtils::startup();
}

WmfVideoDecoder::~WmfVideoDecoder() {
  release();
  if (mWmfStarted) {
    WmfMediaUtils::shutdown();
  }
}

bool WmfVideoDecoder::configure(const std::shared_ptr<MediaFormat>& format,
  const std::shared_ptr<VideoSurface>& surface) {
  if (!format) {
    MINILOG_ERROR("failed to configure the video decoder: the format is null");
    return false;
  }
  if (surface != nullptr && surface->isValid()) {
    // A window surface is Android's way of asking the decoder to present its output itself, which the
    // Windows path does not have; a *GPU* surface is a different request -- allocate the textures here --
    // and that one this decoder does implement.
    if (std::dynamic_pointer_cast<HardwareVideoSurface>(surface) != nullptr) {
      mHardwareSurface = surface;
    } else {
      MINILOG_WARN("the audio/video surface is not supported by the Windows video decoder");
    }
  }

  const auto mimeType = format->getMimeType();
  if (mimeType.empty() || !MimeTypes::isVideo(mimeType)) {
    MINILOG_ERROR("failed to configure the video decoder: invalid mime type: {}", mimeType);
    return false;
  }
  if (!WmfMediaUtils::mimeTypeToSubtype(mimeType, mInputSubType)) {
    MINILOG_ERROR("failed to configure the video decoder: unsupported mime type: {}", mimeType);
    return false;
  }
  if (!WmfMediaUtils::isTransformAvailable(true, mInputSubType, true)) {
    MINILOG_ERROR("no video decoder is available for the mime type: {}", mimeType);
    return false;
  }

  mInputFormat = format;
  mWidth = format->getWidth() != MediaFormat::NO_VALUE ? format->getWidth() : 0;
  mHeight = format->getHeight() != MediaFormat::NO_VALUE ? format->getHeight() : 0;

  if (!createCodec() || !createHardwareOutput() || !createInputMediaType() || !negotiateOutputType(false)) {
    release();
    return false;
  }

  auto bufferSize = format->getMaxInputSize();
  if (bufferSize <= 0) {
    bufferSize = DEFAULT_MAX_INPUT_SIZE;
  }
  mInputBuffer = std::make_shared<AVPacketBuffer>(MediaType::kMediaTypeVideo);
  mInputBuffer->ensureSpaceForWrite(static_cast<size_t>(bufferSize));

  mInputEndOfStream = false;
  mOutputEndOfStream = false;
  mDrainRequested = false;
  mInputDequeued = false;
  mConfigured = true;
  return true;
}

bool WmfVideoDecoder::createCodec() {
  auto transform = WmfMediaUtils::createTransform(true, mInputSubType, true);
  if (transform == nullptr) {
    MINILOG_ERROR("failed to create the video decoder transform");
    return false;
  }
  mTransform.attach(std::move(transform));
  return true;
}

bool WmfVideoDecoder::createHardwareOutput() {
  mHardwareOutput = false;
  if (mHardwareSurface == nullptr) {
    return true;
  }
  auto* hardwareSurface = dynamic_cast<HardwareVideoSurface*>(mHardwareSurface.get());
  void* nativeDevice = hardwareSurface != nullptr ? hardwareSurface->getNativeDevice() : nullptr;
  if (nativeDevice == nullptr) {
    MINILOG_DEBUG("the hardware surface names no device, the decoder keeps CPU output");
    return true;
  }

  auto result = ::MFCreateDXGIDeviceManager(&mDeviceManagerToken, &mDeviceManager);
  if (FAILED(result) || mDeviceManager == nullptr) {
    WmfMediaUtils::handleHresult(nullptr, result, "MFCreateDXGIDeviceManager failed");
    mDeviceManager.Reset();
    return true;
  }
  result = mDeviceManager->ResetDevice(static_cast<ID3D11Device*>(nativeDevice), mDeviceManagerToken);
  if (FAILED(result)) {
    WmfMediaUtils::handleHresult(nullptr, result, "the decoder device manager refused the device");
    mDeviceManager.Reset();
    return true;
  }
  // The message has to arrive before the decoder starts producing: it is what makes the MFT take its
  // output surfaces from the manager's pool instead of allocating system memory.
  result = mTransform.get() != nullptr
             ? mTransform.get()->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                 reinterpret_cast<ULONG_PTR>(mDeviceManager.Get()))
             : E_POINTER;
  if (FAILED(result)) {
    WmfMediaUtils::handleHresult(nullptr, result, "the video decoder refused the Direct3D 11 device manager");
    mDeviceManager.Reset();
    return true;
  }
  mHardwareOutput = true;
  MINILOG_DEBUG("the video decoder writes its output into Direct3D 11 textures");
  return true;
}

bool WmfVideoDecoder::createInputMediaType() {
  Microsoft::WRL::ComPtr<IMFMediaType> mediaType;
  auto result = ::MFCreateMediaType(&mediaType);
  if (FAILED(result) || mediaType == nullptr) {
    MINILOG_ERROR("MFCreateMediaType failed");
    return false;
  }

  WmfMediaUtils::fillVideoMediaType(mediaType.Get(), mInputFormat, mInputSubType);

  // The H.264/HEVC decoders need the sequence header when the stream does not contain the
  // parameter sets in band.
  const auto& csdList = mInputFormat->csdList();
  if (!csdList.empty() && !csdList[0].empty()) {
    const auto sequenceHeader = mInputSubType == MFVideoFormat_H264
                                ? normalizeH264SequenceHeader(csdList[0])
                                : csdList[0];
    mPrependSequenceHeader = mInputSubType == MFVideoFormat_H264 && isAnnexB(csdList[0]);
    mSequenceHeaderPrepended = false;
    mediaType->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, sequenceHeader.data(),
      static_cast<UINT32>(sequenceHeader.size()));
  }

  result = mTransform.setInputType(mediaType.Get());
  if (SUCCEEDED(result)) {
    return true;
  }
  MINILOG_DEBUG("the requested video decoder input type is rejected: {}, trying the available types",
    WmfMediaUtils::hresultToString(result));

  for (DWORD index = 0;; ++index) {
    Microsoft::WRL::ComPtr<IMFMediaType> candidate;
    if (FAILED(mTransform.get()->GetInputAvailableType(0, index, &candidate)) ||
        candidate == nullptr) {
      break;
    }
    GUID subType{};
    if (FAILED(candidate->GetGUID(MF_MT_SUBTYPE, &subType)) || subType != mInputSubType) {
      continue;
    }
    if (SUCCEEDED(mTransform.setInputType(candidate.Get()))) {
      return true;
    }
  }

  WmfMediaUtils::handleHresult(nullptr, result, "failed to set the video decoder input type",
    mInputFormat->toString());
  return false;
}

bool WmfVideoDecoder::negotiateOutputType(const bool allowAvailableTypes) {
  const GUID preferredSubTypes[] = { MFVideoFormat_NV12, MFVideoFormat_IYUV };

  bool accepted = false;
  if (!allowAvailableTypes && mWidth > 0 && mHeight > 0) {
    for (const auto& subType: preferredSubTypes) {
      Microsoft::WRL::ComPtr<IMFMediaType> mediaType;
      if (FAILED(::MFCreateMediaType(&mediaType)) || mediaType == nullptr) {
        continue;
      }
      mediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
      mediaType->SetGUID(MF_MT_SUBTYPE, subType);
      mediaType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
      mediaType->SetUINT64(MF_MT_FRAME_SIZE,
        ::Pack2UINT32AsUINT64(static_cast<UINT32>(mWidth),
          static_cast<UINT32>(mHeight)));
      if (SUCCEEDED(mTransform.setOutputType(mediaType.Get()))) {
        accepted = true;
        break;
      }
    }
  }

  if (!accepted) {
    for (const auto& subType: preferredSubTypes) {
      auto candidate = mTransform.findOutputType(MFMediaType_Video, subType);
      if (candidate != nullptr && SUCCEEDED(mTransform.setOutputType(candidate.Get()))) {
        accepted = true;
        break;
      }
    }
  }
  if (!accepted) {
    MINILOG_ERROR("failed to negotiate the video decoder output type");
    return false;
  }

  Microsoft::WRL::ComPtr<IMFMediaType> outputType;
  if (FAILED(mTransform.getOutputType(&outputType)) || outputType == nullptr) {
    MINILOG_ERROR("the video decoder has no output type");
    return false;
  }

  UINT64 frameSize = 0;
  GUID subType{};
  outputType->GetUINT64(MF_MT_FRAME_SIZE, &frameSize);
  outputType->GetGUID(MF_MT_SUBTYPE, &subType);
  const auto width = static_cast<int>(frameSize >> 32);
  const auto height = static_cast<int>(frameSize & 0xffffffff);
  if (width > 0 && height > 0) {
    mWidth = width;
    mHeight = height;
  }
  mOutputSubType = subType;

  const auto pixelFormat = WmfMediaUtils::subtypeToPixelFormat(subType);
  if (pixelFormat == PixelFormat::kFormatInvalid) {
    MINILOG_ERROR("the video decoder output pixel format is not supported");
    return false;
  }
  mOutputConfig = VideoConfiguration(static_cast<uint32_t>(mWidth), static_cast<uint32_t>(mHeight));
  mOutputConfig.pixelFormat = pixelFormat;
  mOutputColorInfo = colorInfoOf(outputType.Get());
  mOutputFormat = MediaFormat::CreateVideoFormat(MimeTypes::VIDEO_RAW, mWidth, mHeight);
  mOutputFormat->setPixelFormat(pixelFormat);
  MINILOG_DEBUG("video decoder output format: {}", mOutputFormat->toString());
  return true;
}

void WmfVideoDecoder::start() {
  if (!mTransform.isValid()) {
    return;
  }
  mInputsSinceOutput = 0;
  mOutstandingOutputCount = 0;
  mSequenceHeaderPrepended = false;
  mTransform.notifyStartOfStream();
}

void WmfVideoDecoder::stop() {
  if (mTransform.isValid()) {
    mTransform.flush();
  }
}

void WmfVideoDecoder::flush() {
  if (mTransform.isValid()) {
    mTransform.flush();
  }
  mPendingInputSamples.clear();
  // The frames that were decoded are dropped with the stream they belong to.
  for (const auto& frame : mPendingFrames) {
    frame->release();
  }
  mPendingFrames.clear();
  mInputDequeued = false;
  mInputEndOfStream = false;
  mOutputEndOfStream = false;
  mDrainRequested = false;
  mInputsSinceOutput = 0;
  mOutstandingOutputCount = 0;
  mSequenceHeaderPrepended = false;
}

void WmfVideoDecoder::release() {
  mTransform.release();
  mTransform.reset();
  mPendingInputSamples.clear();
  mInputBuffer.reset();
  for (const auto& frame : mPendingFrames) {
    frame->release();
  }
  mPendingFrames.clear();
  discardBuffers();
  mInputDequeued = false;
  mInputEndOfStream = true;
  mOutputEndOfStream = true;
  mConfigured = false;
  mInputsSinceOutput = 0;
  mOutstandingOutputCount = 0;
  mSequenceHeaderPrepended = false;
  // The device manager is released after the transform, because the transform's pool holds the textures
  // the manager owns; the frames already handed out keep their own reference to the texture, not to the
  // manager, so they stay valid.
  mDeviceManager.Reset();
  mDeviceManagerToken = 0;
  mHardwareOutput = false;
  mHardwareSurface.reset();
}

void WmfVideoDecoder::discardBuffers() {
  std::lock_guard<std::mutex> lock(mPendingLock);
  mAvailableBuffers.clear();
}

std::shared_ptr<AVPixelBuffer> WmfVideoDecoder::obtainPooledBuffer() {
  std::lock_guard<std::mutex> lock(mPendingLock);
  while (!mAvailableBuffers.empty()) {
    auto buffer = mAvailableBuffers.front();
    mAvailableBuffers.pop_front();
    if (buffer->getFormat() != mOutputConfig.pixelFormat ||
        buffer->width() != static_cast<int>(mOutputConfig.width) ||
        buffer->height() != static_cast<int>(mOutputConfig.height)) {
      // The decoder changed its output while the frame the buffer holds was out, and the frame of the
      // new output does not fit in it. The buffer is dropped rather than reused.
      continue;
    }
    // The buffer holds the state of the frame it was given for before, and a frame which the renderer
    // dropped is marked as discarded: handing that state out with the frame which takes the buffer
    // would make the renderer drop it as well, so the buffer is cleared before it is used again.
    buffer->clear();
    return buffer;
  }
  return nullptr;
}

std::shared_ptr<AVPacketBuffer> WmfVideoDecoder::dequeueInputBuffer() {
  drainReturnedFrames();
  if (!mConfigured || mInputEndOfStream || mInputDequeued || mInputBuffer == nullptr) {
    return nullptr;
  }
  // The input of the decoder is bounded internally instead of in the renderer. The MediaCodec
  // decoders get the same effect from their fixed input buffer pool; the Media Foundation decoder
  // reuses one buffer, so without this it would accept the whole stream in a single call.
  fillPendingFrames();
  if (mOutstandingOutputCount >= MAX_OUTSTANDING_FRAMES || mPendingFrames.size() >= MAX_PENDING_FRAMES) {
    return nullptr;
  }
  if (mInputsSinceOutput >= MAX_INPUTS_AHEAD) {
    pushInputSamples();
    if (mInputsSinceOutput >= MAX_INPUTS_AHEAD) {
      return nullptr;
    }
  }
  mInputDequeued = true;
  mInputBuffer->clear();
  return mInputBuffer;
}

void WmfVideoDecoder::queueInputBuffer(std::shared_ptr<AVPacketBuffer> inputBuffer) {
  if (!inputBuffer || !mTransform.isValid()) {
    return;
  }
  mInputDequeued = false;

  if (inputBuffer->isEndOfStream()) {
    mInputEndOfStream = true;
    return;
  }
  const auto size = inputBuffer->size();
  if (size == 0) {
    return;
  }

  const uint8_t* data = inputBuffer->array();
  auto sampleSize = size;
  if (sampleSize == 0) {
    return;
  }

  std::vector<uint8_t> prefixedSample;
  if (mPrependSequenceHeader && !mSequenceHeaderPrepended && inputBuffer->isKeyFrame()
      && mInputFormat != nullptr) {
    const auto& csdList = mInputFormat->csdList();
    if (!csdList.empty() && !csdList[0].empty()) {
      const auto& sequenceHeader = csdList[0];
      prefixedSample.reserve(sequenceHeader.size() + sampleSize);
      prefixedSample.insert(prefixedSample.end(), sequenceHeader.begin(), sequenceHeader.end());
      prefixedSample.insert(prefixedSample.end(), data, data + sampleSize);
      data = prefixedSample.data();
      sampleSize = static_cast<int>(prefixedSample.size());
      mSequenceHeaderPrepended = true;
      MINILOG_DEBUG("prepended {} bytes of the sequence header to the first video frame",
        sequenceHeader.size());
    }
  }

  const auto sample =
    WmfMediaUtils::createSample(data, sampleSize,
      inputBuffer->timeUs(), inputBuffer->isKeyFrame()
                               ? AVBuffer::kKeyFrame
                               : AVBuffer::kNone);
  if (sample == nullptr) {
    return;
  }

  const auto result = mTransform.processInput(sample.Get());
  if (result == MF_E_NOTACCEPTING) {
    mPendingInputSamples.emplace_back(sample);
    ++mInputsSinceOutput;
    return;
  }
  if (FAILED(result)) {
    WmfMediaUtils::handleHresult(nullptr, result, "the video decoder rejected the input sample");
    return;
  }
  ++mInputsSinceOutput;
}

bool WmfVideoDecoder::pushInputSamples() {
  while (!mPendingInputSamples.empty()) {
    const auto result = mTransform.processInput(mPendingInputSamples.front().Get());
    if (result == MF_E_NOTACCEPTING) {
      return false;
    }
    mPendingInputSamples.pop_front();
    if (FAILED(result)) {
      WmfMediaUtils::handleHresult(nullptr, result,
        "the video decoder rejected the input sample");
    }
  }
  return true;
}

std::shared_ptr<AVPixelBuffer> WmfVideoDecoder::createEndOfStreamBuffer() {
  auto buffer = AVPixelBuffer::newFlagsOnlyInstance();
  buffer->setFlags(AVBufferFlags::kEndOfStream);
  return buffer;
}

std::shared_ptr<AVPixelBuffer> WmfVideoDecoder::dequeueOutputBuffer() {
  drainReturnedFrames();
  if (!mPendingFrames.empty()) {
    auto frame = std::move(mPendingFrames.front());
    mPendingFrames.pop_front();
    if (!frame->isEndOfStream()) {
      ++mOutstandingOutputCount;
    }
    return frame;
  }
  fillPendingFrames();
  if (mPendingFrames.empty()) {
    return nullptr;
  }
  auto frame = std::move(mPendingFrames.front());
  mPendingFrames.pop_front();
  if (!frame->isEndOfStream()) {
    ++mOutstandingOutputCount;
  }
  return frame;
}

void WmfVideoDecoder::fillPendingFrames() {
  drainReturnedFrames();
  // The frames the decoder holds are taken out of it even while the caller waits for the time of the
  // frame it was given: a decoder whose outputs stay in place buffers the frames of the stream for the
  // reordering, and hands them out in an order that is no longer the order of the stream.
  while (mPendingFrames.size() < MAX_PENDING_FRAMES) {
    auto frame = dequeueDecodedFrame();
    if (frame == nullptr) {
      return;
    }
    const bool endOfStream = frame->isEndOfStream();
    mPendingFrames.push_back(std::move(frame));
    if (endOfStream) {
      return;
    }
  }
}

std::shared_ptr<AVPixelBuffer> WmfVideoDecoder::dequeueDecodedFrame() {
  drainReturnedFrames();
  if (!mConfigured || !mTransform.isValid()) {
    return nullptr;
  }
  if (mOutputEndOfStream) {
    return createEndOfStreamBuffer();
  }

  const auto inputsPushed = pushInputSamples();

  while (true) {
    // The video decoders of the platform must not be polled while the output is not ready, the
    // frames are buffered for the reordering otherwise.
    if (mTransform.queryOutputState() == WmfTransformContext::OutputState::kNeedMoreInput) {
      if (mInputEndOfStream && inputsPushed) {
        if (!mDrainRequested) {
          mTransform.drain();
          mDrainRequested = true;
          continue;
        }
        mOutputEndOfStream = true;
        return createEndOfStreamBuffer();
      }
      return nullptr;
    }

    Microsoft::WRL::ComPtr<IMFSample> sample;
    const auto result = mTransform.processOutput(sample);

    if (result == MF_E_TRANSFORM_STREAM_CHANGE) {
      MINILOG_DEBUG("the video decoder changed its output type");
      if (!negotiateOutputType(true)) {
        return nullptr;
      }
      continue;
    }
    if (result == MF_E_TRANSFORM_NEED_MORE_INPUT) {
      if (mInputEndOfStream && inputsPushed) {
        if (!mDrainRequested) {
          mTransform.drain();
          mDrainRequested = true;
          continue;
        }
        mOutputEndOfStream = true;
        return createEndOfStreamBuffer();
      }
      return nullptr;
    }
    if (FAILED(result) || sample == nullptr) {
      if (FAILED(result)) {
        WmfMediaUtils::handleHresult(nullptr, result, "failed to decode the video frame");
      }
      return nullptr;
    }

    const auto frameBuffer = createFrameBuffer(sample);
    if (frameBuffer == nullptr) {
      continue;
    }
    if (!isAtLeastOutputStartTime(frameBuffer->timeUs())) {
      frameBuffer->shouldBeSkipped = true;
    }
    return frameBuffer;
  }
}

std::shared_ptr<AVPixelBuffer>
WmfVideoDecoder::createFrameBuffer(const Microsoft::WRL::ComPtr<IMFSample>& sample) {
  if (mWidth <= 0 || mHeight <= 0) {
    return nullptr;
  }

  if (mHardwareOutput) {
    if (auto hardware = createHardwareFrameBuffer(sample); hardware != nullptr) {
      return hardware;
    }
    // The sample held no texture: a stream change, or a profile the driver decodes in software. The copy
    // path below handles it, which is why hardware output degrades per frame instead of all at once.
  }

  Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
  auto result = sample->ConvertToContiguousBuffer(&buffer);
  if (FAILED(result) || buffer == nullptr) {
    WmfMediaUtils::handleHresult(nullptr, result, "the decoded frame has no contiguous buffer");
    return nullptr;
  }

  BYTE* scanline0 = nullptr;
  LONG pitch = 0;
  bool locked2D = false;
  Microsoft::WRL::ComPtr<IMF2DBuffer> buffer2D;
  if (SUCCEEDED(buffer.As(&buffer2D)) && buffer2D != nullptr) {
    if (FAILED(buffer2D->Lock2D(&scanline0, &pitch))) {
      locked2D = false;
    } else {
      locked2D = true;
    }
  }

  if (!locked2D) {
    BYTE* data = nullptr;
    DWORD maxLength = 0;
    if (FAILED(buffer->Lock(&data, &maxLength, nullptr))) {
      WmfMediaUtils::handleHresult(nullptr, result, "failed to lock the decoded video buffer");
      return nullptr;
    }
    scanline0 = data;

    UINT32 defaultStride = 0;
    Microsoft::WRL::ComPtr<IMFMediaType> outputType;
    if (SUCCEEDED(mTransform.getOutputType(&outputType)) && outputType != nullptr &&
        SUCCEEDED(outputType->GetUINT32(MF_MT_DEFAULT_STRIDE, &defaultStride)) &&
        defaultStride > 0) {
      pitch = static_cast<LONG>(defaultStride);
    } else {
      pitch = mWidth;
    }
  }

  const auto stride = std::abs(pitch);
  const auto bottomUp = pitch < 0;
  const auto chromaHeight = (mHeight + 1) / 2;

  // Returns the source pointer of the row of the plane.
  const auto rowOf = [&](const BYTE* plane, const int row, const int rows) -> const BYTE* {
    const auto sourceRow = bottomUp ? (rows - 1 - row) : row;
    return plane + static_cast<ptrdiff_t>(sourceRow) * stride;
  };

  std::shared_ptr<AVPixelBuffer> frameBuffer;
  if (mOutputSubType == MFVideoFormat_NV12) {
    // The pool can be empty -- on the first frame it always is -- and asking a null buffer what it is would
    // be a crash rather than the miss that the cast reports.
    const auto pooled = obtainPooledBuffer();
    auto nv12 = pooled != nullptr ? pooled->asNV12PixelBuffer() : nullptr;
    if (nv12 == nullptr || nv12->strideY() != stride) {
      nv12 = std::make_shared<NV12PixelBuffer>(this, mWidth, mHeight, stride, stride);
    }
    for (int row = 0; row < mHeight; ++row) {
      memcpy(nv12->mutableDataY() + static_cast<ptrdiff_t>(row) * nv12->strideY(),
        rowOf(scanline0, row, mHeight), static_cast<size_t>(stride));
    }
    const auto* uvPlane = scanline0 + static_cast<ptrdiff_t>(stride) * mHeight;
    for (int row = 0; row < chromaHeight; ++row) {
      memcpy(nv12->mutableDataUV() + static_cast<ptrdiff_t>(row) * nv12->strideUV(),
        rowOf(uvPlane, row, chromaHeight), static_cast<size_t>(stride));
    }
    frameBuffer = std::move(nv12);
  } else {
    const auto chromaStride = std::max(1L, stride / 2);
    const auto pooled = obtainPooledBuffer();
    auto i420 = pooled != nullptr ? pooled->asI420PixelBuffer() : nullptr;
    if (i420 == nullptr || i420->strideY() != stride || i420->strideU() != chromaStride) {
      i420 = std::make_shared<I420PixelBuffer>(this, mWidth, mHeight, stride, chromaStride,
        chromaStride);
    }
    for (int row = 0; row < mHeight; ++row) {
      memcpy(i420->mutableDataY() + static_cast<ptrdiff_t>(row) * i420->strideY(),
        rowOf(scanline0, row, mHeight), static_cast<size_t>(stride));
    }
    const auto* uPlane = scanline0 + static_cast<ptrdiff_t>(stride) * mHeight;
    const auto* vPlane = uPlane + static_cast<ptrdiff_t>(chromaStride) * chromaHeight;
    for (int row = 0; row < chromaHeight; ++row) {
      memcpy(i420->mutableDataU() + static_cast<ptrdiff_t>(row) * i420->strideU(),
        rowOf(uPlane, row, chromaHeight), static_cast<size_t>(chromaStride));
      memcpy(i420->mutableDataV() + static_cast<ptrdiff_t>(row) * i420->strideV(),
        rowOf(vPlane, row, chromaHeight), static_cast<size_t>(chromaStride));
    }
    frameBuffer = std::move(i420);
  }

  if (locked2D) {
    buffer2D->Unlock2D();
  } else {
    buffer->Unlock();
  }

  if (frameBuffer != nullptr) {
    frameBuffer->timeUs(sampleTimeUs(sample));
    // The samples the transform could not take yet are still ahead of its output, keep them in
    // the count so that the input of the stream stays bounded.
    mInputsSinceOutput = static_cast<int>(mPendingInputSamples.size());
    // The renderers convert the frames to RGB with the color space the decoder negotiated.
    frameBuffer->setColorInfo(mOutputColorInfo);
  }
  return frameBuffer;
}

std::shared_ptr<AVPixelBuffer>
WmfVideoDecoder::createHardwareFrameBuffer(const Microsoft::WRL::ComPtr<IMFSample>& sample) {
  if (sample == nullptr) {
    return nullptr;
  }
  Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
  if (FAILED(sample->GetBufferByIndex(0, &buffer)) || buffer == nullptr) {
    return nullptr;
  }
  Microsoft::WRL::ComPtr<IMFDXGIBuffer> dxgiBuffer;
  if (FAILED(buffer.As(&dxgiBuffer)) || dxgiBuffer == nullptr) {
    return nullptr;
  }
  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
  if (FAILED(dxgiBuffer->GetResource(IID_PPV_ARGS(&texture))) || texture == nullptr) {
    return nullptr;
  }
  UINT subresource = 0;
  if (FAILED(dxgiBuffer->GetSubresourceIndex(&subresource))) {
    subresource = 0;
  }
  // The decoder may pad the coded size (1088 for a 1080 line picture); the frame's extent is the one the
  // output type negotiated, which the consumer uses to crop and to convert the colour.
  auto frame = std::make_shared<WmfHardwarePixelBuffer>(this, std::move(texture), subresource, mWidth,
    mHeight, WmfHardwarePixelBuffer::TextureFormat::kNv12);
  frame->timeUs(sampleTimeUs(sample));
  mInputsSinceOutput = static_cast<int>(mPendingInputSamples.size());
  frame->setColorInfo(mOutputColorInfo);
  return frame;
}

int64_t WmfVideoDecoder::sampleTimeUs(const Microsoft::WRL::ComPtr<IMFSample>& sample) {
  LONGLONG time = 0;
  if (FAILED(sample->GetSampleTime(&time))) {
    return Definitions::kTimeUnset;
  }
  return time / 10; // 100ns to us
}

void WmfVideoDecoder::releaseOutputBuffer(const std::shared_ptr<AVPixelBuffer>& outputBuffer,
  const bool render) {
  static_cast<void>(render);
  if (outputBuffer) {
    // The buffer returns to this decoder through recycleBuffer, which releases its outstanding
    // output slot.
    outputBuffer->release();
  }
}

void WmfVideoDecoder::recycleBuffer(std::shared_ptr<AVPixelBuffer> frameBuffer) {
  if (!frameBuffer) {
    return;
  }
  if (std::dynamic_pointer_cast<HardwarePixelBuffer>(frameBuffer) != nullptr) {
    // The render thread returns a hardware surface here; the decoder thread may be blocked waiting
    // for that slot, so the outstanding count is released now rather than on the next input dequeue.
    if (mOutstandingOutputCount > 0) {
      --mOutstandingOutputCount;
    }
    {
      std::lock_guard<std::mutex> lock(mReturnedMutex);
      mReturnedFrames.push_back(std::move(frameBuffer));
    }
    return;
  }
  if (mOutstandingOutputCount > 0) {
    --mOutstandingOutputCount;
  }
  if (frameBuffer->getFormat() != mOutputConfig.pixelFormat ||
      frameBuffer->width() != static_cast<int>(mOutputConfig.width) ||
      frameBuffer->height() != static_cast<int>(mOutputConfig.height)) {
    return;
  }
  std::lock_guard<std::mutex> lock(mPendingLock);
  if (mAvailableBuffers.size() < MAX_POOLED_BUFFERS) {
    mAvailableBuffers.push_back(std::move(frameBuffer));
  }
}

void WmfVideoDecoder::drainReturnedFrames() {
  std::deque<std::shared_ptr<AVPixelBuffer>> returned;
  {
    std::lock_guard<std::mutex> lock(mReturnedMutex);
    returned.swap(mReturnedFrames);
  }
  // The outstanding count was released in recycleBuffer for a hardware surface, because the render
  // thread can return one while the decoder thread is blocked on the count.
}

void WmfVideoDecoder::signalEndOfInputStream() {
  mInputEndOfStream = true;
}

void WmfVideoDecoder::setOutputStartTimeUs(const int64_t outputStartTimeUs) {
  mOutputStartTimeUs = outputStartTimeUs;
}

} // namespace coral
