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

#include "D3D11VAVideoDecoder.h"

#if defined(WINDOWS) || defined(WIN32)

#include "D3D11VAHardwarePixelBuffer.h"
#include "WmfMediaUtils.h"
#include "WmfVideoDecoder.h"

#include <media/Utilities.h>
#include <minilog/log.h>

#include <dxva.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

namespace coral {

namespace {

/** The number of output views of the decode texture array: 16 references plus the current picture. */
constexpr int kDecodeSlotCount = 18;
/** The number of packed textures the BGRA output ring keeps in rotation. */
constexpr int kConvertedSlotCount = 8;
constexpr int MAX_PENDING_FRAMES = 8;
constexpr int MAX_OUTSTANDING_FRAMES = 6;
constexpr int DEFAULT_MAX_INPUT_SIZE = 2048 * 2048;

/** `DXVA2_NoEncrypt`, the configuration GUID of an unencrypted decoder. */
constexpr GUID kNoEncryptGuid = { 0x1b81bed0, 0xa0c7, 0x11d3, { 0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5 } };

/**
 * The H.264 VLD decode profiles. They are declared here rather than taken from `d3d11.h` so that the
 * module does not have to link the GUID library of the SDK for two constants.
 */
constexpr GUID kH264VldNoFgt = { 0x1b81be68, 0xa0c7, 0x11d3, { 0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5 } };
constexpr GUID kH264VldFgt = { 0x1b81be69, 0xa0c7, 0x11d3, { 0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5 } };

/**
 * The colour space of the decoded stream for the video processor, which is what converts NV12 to the
 * packed output. The processor has one switch for the YUV matrix and one for the range.
 */
D3D11_VIDEO_PROCESSOR_COLOR_SPACE toInputColorSpace(const ColorInfo& color, const uint32_t height) {
  D3D11_VIDEO_PROCESSOR_COLOR_SPACE space{};
  space.Usage = 0;
  space.RGB_Range = 0;
  switch (color.colorSpace) {
    case ColorInfo::SpaceType::kSpaceBT601:
      space.YCbCr_Matrix = 0;
      break;
    case ColorInfo::SpaceType::kSpaceBT709:
      space.YCbCr_Matrix = 1;
      break;
    case ColorInfo::SpaceType::kSpaceUnknown:
    case ColorInfo::SpaceType::kSpaceBT2020:
    default:
      space.YCbCr_Matrix = height >= 720 ? 1 : 0;
      break;
  }
  space.YCbCr_xvYCC = 0;
  // `D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235` is 1 and `_0_255` is 2.
  space.Nominal_Range = color.colorRange == ColorInfo::RangeType::kRangeFull ? 2 : 1;
  return space;
}

D3D11_VIDEO_PROCESSOR_COLOR_SPACE outputColorSpace() {
  D3D11_VIDEO_PROCESSOR_COLOR_SPACE space{};
  space.Usage = 0;
  space.RGB_Range = 0;
  space.YCbCr_Matrix = 1;
  space.YCbCr_xvYCC = 0;
  space.Nominal_Range = 2;
  return space;
}

ColorInfo mergeColorInfo(const ColorInfo& container, const ColorInfo& codec) {
  ColorInfo result = container;
  if (result.colorSpace == ColorInfo::SpaceType::kSpaceUnknown) {
    result.colorSpace = codec.colorSpace;
  }
  if (result.colorRange == ColorInfo::RangeType::kRangeUnknown) {
    result.colorRange = codec.colorRange;
  }
  if (result.colorTransfer == ColorInfo::TransferType::kTransferUnknown) {
    result.colorTransfer = codec.colorTransfer;
  }
  if (result.hdrStaticInfo.empty()) {
    result.hdrStaticInfo = codec.hdrStaticInfo;
  }
  return result;
}

bool isSupportedSps(const d3d11va::Sps& sps, const d3d11va::Pps& pps) {
  if (!sps.valid || !pps.valid) {
    return false;
  }
  // The decoder provides packed NV12 output views, so the stream has to be progressive 4:2:0 8-bit
  // without the host side features (slice groups, scaling matrices) this front-end does not forward.
  if (!sps.frameMbsOnly || sps.separateColourPlane || sps.chromaFormatIdc != 1 || sps.bitDepthLuma != 8 ||
          sps.bitDepthChroma != 8 || sps.seqScalingMatrixPresent || sps.maxNumRefFrames > 16) {
    return false;
  }
  return pps.numSliceGroupsMinus1 == 0 && !pps.scalingMatrixPresent;
}

} // namespace

D3D11VAVideoDecoder::D3D11VAVideoDecoder() = default;

D3D11VAVideoDecoder::~D3D11VAVideoDecoder() { release(); }

bool D3D11VAVideoDecoder::isSupported(const VideoCodecType type) { return type == VideoCodecType::kVideoCodecTypeH264; }

bool D3D11VAVideoDecoder::configure(const std::shared_ptr<MediaFormat>& format,
        const std::shared_ptr<VideoSurface>& surface) {
  if (!format) {
    MINILOG_ERROR("failed to configure the D3D11VA decoder: the format is null");
    return false;
  }
  if (!MimeTypes::isVideo(format->getMimeType())) {
    MINILOG_ERROR("failed to configure the D3D11VA decoder: invalid mime type: {}", format->getMimeType());
    return false;
  }

  auto codecType = format->getVideoCodecType();
  if (codecType == VideoCodecType::kVideoCodecTypeInvalid) {
    codecType = MediaUtils::convertToVideoCodecType(format->getMimeType());
  }
  if (codecType != VideoCodecType::kVideoCodecTypeH264) {
    MINILOG_DEBUG("the D3D11VA decoder does not decode {}; the platform decoder is used", convertToText(codecType));
    return configureFallback(format, surface);
  }

  if (configureD3D11VA(format, surface)) {
    return true;
  }
  MINILOG_DEBUG("the D3D11VA decoder cannot be configured; the platform decoder is used");
  return configureFallback(format, surface);
}

bool D3D11VAVideoDecoder::configureD3D11VA(const std::shared_ptr<MediaFormat>& format,
        const std::shared_ptr<VideoSurface>& surface) {
  if (surface == nullptr || !surface->isValid()) {
    MINILOG_DEBUG("the D3D11VA decoder needs a valid hardware video surface");
    return false;
  }
  auto* hardwareSurface = dynamic_cast<HardwareVideoSurface*>(surface.get());
  if (hardwareSurface == nullptr || hardwareSurface->getNativeDevice() == nullptr) {
    MINILOG_DEBUG("the D3D11VA decoder needs a hardware surface with an ID3D11Device");
    return false;
  }

  auto* nativeDevice = static_cast<ID3D11Device*>(hardwareSurface->getNativeDevice());
  nativeDevice->AddRef();
  mDevice.Attach(nativeDevice);
  mDevice->GetImmediateContext(&mContext);
  if (mDevice == nullptr || mContext == nullptr) {
    MINILOG_ERROR("the D3D11VA decoder could not obtain the device context");
    return false;
  }
  if (FAILED(mDevice.As(&mVideoDevice)) || mVideoDevice == nullptr || FAILED(mContext.As(&mVideoContext)) ||
          mVideoContext == nullptr) {
    MINILOG_DEBUG("the Direct3D 11 device has no video decode interface");
    return false;
  }
  D3D11_QUERY_DESC queryDescription{};
  queryDescription.Query = D3D11_QUERY_EVENT;
  if (FAILED(mDevice->CreateQuery(&queryDescription, &mCompletionQuery)) || mCompletionQuery == nullptr) {
    MINILOG_DEBUG("the D3D11VA decoder could not create its completion query");
    return false;
  }

  mInputFormat = format;
  mHardwareSurface = surface;
  mConvertOutput = format->getProcessFormat() == PixelFormat::kFormatBGRA;
  mWidth = format->getWidth() != MediaFormat::NO_VALUE ? format->getWidth() : 0;
  mHeight = format->getHeight() != MediaFormat::NO_VALUE ? format->getHeight() : 0;
  mOutputColorInfo = format->getColorInfo();

  parseCodecSpecificData();
  if (!mSpsMap.empty() && !mPpsMap.empty()) {
    if (!ensureCodec()) {
      MINILOG_DEBUG("the D3D11VA decoder does not support this H.264 stream");
      return false;
    }
  } else {
    // The parameter sets can arrive with the first access unit; the codec is created then.
    if (mWidth > 0 && mHeight > 0) {
      mOutputFormat = MediaFormat::CreateVideoFormat(MimeTypes::VIDEO_RAW, mWidth, mHeight);
      mOutputFormat->setPixelFormat(PixelFormat::kFormatNV12);
      mOutputFormat->setColorInfo(mOutputColorInfo);
    }
    MINILOG_DEBUG("the D3D11VA decoder waits for the parameter sets of the stream");
  }

  auto bufferSize = format->getMaxInputSize();
  if (bufferSize <= 0) {
    bufferSize = DEFAULT_MAX_INPUT_SIZE;
  }
  mInputBuffer = std::make_shared<AVPacketBuffer>(MediaType::kMediaTypeVideo);
  mInputBuffer->ensureSpaceForWrite(static_cast<size_t>(bufferSize));

  mInputDequeued = false;
  mInputEndOfStream = false;
  mOutputEndOfStream = false;
  mDrainDone = false;
  mConfigured = true;
  return true;
}

bool D3D11VAVideoDecoder::configureFallback(const std::shared_ptr<MediaFormat>& format,
        const std::shared_ptr<VideoSurface>& surface) {
  releaseD3D11VAState();
  auto fallback = std::make_unique<WmfVideoDecoder>();
  if (!fallback->configure(format, surface)) {
    MINILOG_ERROR("the platform video decoder could not be configured either");
    return false;
  }
  fallback->start();
  mFallback = std::move(fallback);
  mInputFormat = format;
  mOutputFormat = mFallback->getOutputFormat();
  mConfigured = true;
  return true;
}

bool D3D11VAVideoDecoder::parseCodecSpecificData() {
  mSpsMap.clear();
  mPpsMap.clear();
  if (!mInputFormat) {
    return false;
  }
  for (const auto& blob: mInputFormat->csdList()) {
    std::vector<d3d11va::Sps> spsList;
    std::vector<d3d11va::Pps> ppsList;
    d3d11va::parseParameterSets(blob, spsList, ppsList);
    for (const auto& sps: spsList) {
      if (sps.valid) {
        mSpsMap[sps.id] = sps;
      }
    }
    for (const auto& pps: ppsList) {
      if (pps.valid) {
        mPpsMap[pps.id] = pps;
      }
    }
  }
  return !mSpsMap.empty();
}

bool D3D11VAVideoDecoder::selectDecoderProfile() {
  const GUID candidates[] = {
    kH264VldNoFgt,
    kH264VldFgt,
  };
  bool found = false;
  for (const auto& candidate: candidates) {
    BOOL supported = FALSE;
    if (SUCCEEDED(mVideoDevice->CheckVideoDecoderFormat(&candidate, DXGI_FORMAT_NV12, &supported)) && supported) {
      mDecoderGuid = candidate;
      found = true;
      break;
    }
  }
  if (!found) {
    MINILOG_DEBUG("the Direct3D 11 device has no H.264 NV12 decode profile");
    return false;
  }

  D3D11_VIDEO_DECODER_DESC description{};
  description.SampleWidth = static_cast<UINT>(mCodedWidth);
  description.SampleHeight = static_cast<UINT>(mCodedHeight);
  description.OutputFormat = DXGI_FORMAT_NV12;
  description.Guid = mDecoderGuid;

  UINT count = 0;
  if (FAILED(mVideoDevice->GetVideoDecoderConfigCount(&description, &count)) || count == 0) {
    MINILOG_DEBUG("the H.264 decode profile exposes no configuration");
    return false;
  }
  bool configured = false;
  for (UINT index = 0; index < count; ++index) {
    D3D11_VIDEO_DECODER_CONFIG candidate{};
    if (FAILED(mVideoDevice->GetVideoDecoderConfig(&description, index, &candidate))) {
      continue;
    }
    // The short slice control form is the one this front-end fills: the driver parses the slice
    // headers itself, and the host only sends the picture parameters, the slice bytes and their
    // positions. A configuration which requires the long form is left to the platform decoder.
    if (candidate.ConfigBitstreamRaw == 2 && IsEqualGUID(candidate.guidConfigBitstreamEncryption, kNoEncryptGuid)) {
      mDecoderConfig = candidate;
      configured = true;
      break;
    }
  }
  if (!configured) {
    MINILOG_DEBUG("the H.264 decode profile has no short slice control configuration");
    return false;
  }
  MINILOG_DEBUG("the D3D11VA decoder profile is selected: {}x{}, bitstream mode {}", mCodedWidth, mCodedHeight,
          mDecoderConfig.ConfigBitstreamRaw);
  return true;
}

bool D3D11VAVideoDecoder::createDecodeResources() {
  mVideoDecoder.Reset();
  mDecodeSlots.clear();
  mDecodeTexture.Reset();

  const auto textureWidth = static_cast<UINT>((mCodedWidth + 15) & ~15);
  const auto textureHeight = static_cast<UINT>((mCodedHeight + 15) & ~15);
  if (textureWidth == 0 || textureHeight == 0) {
    return false;
  }

  D3D11_TEXTURE2D_DESC description{};
  description.Width = textureWidth;
  description.Height = textureHeight;
  description.MipLevels = 1;
  description.ArraySize = kDecodeSlotCount;
  description.Format = DXGI_FORMAT_NV12;
  description.SampleDesc.Count = 1;
  description.Usage = D3D11_USAGE_DEFAULT;
  description.BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;
  HRESULT result = mDevice->CreateTexture2D(&description, nullptr, &mDecodeTexture);
  if (FAILED(result)) {
    description.BindFlags = D3D11_BIND_DECODER;
    result = mDevice->CreateTexture2D(&description, nullptr, &mDecodeTexture);
  }
  if (FAILED(result) || mDecodeTexture == nullptr) {
    MINILOG_ERROR("the D3D11VA decode texture could not be created: {}", WmfMediaUtils::hresultToString(result));
    return false;
  }

  mDecodeSlots.resize(kDecodeSlotCount);
  for (int index = 0; index < kDecodeSlotCount; ++index) {
    D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC viewDescription{};
    viewDescription.DecodeProfile = mDecoderGuid;
    viewDescription.ViewDimension = D3D11_VDOV_DIMENSION_TEXTURE2D;
    viewDescription.Texture2D.ArraySlice = static_cast<UINT>(index);
    result = mVideoDevice->CreateVideoDecoderOutputView(mDecodeTexture.Get(), &viewDescription,
            &mDecodeSlots[index].view);
    if (FAILED(result) || mDecodeSlots[index].view == nullptr) {
      MINILOG_ERROR("the D3D11VA output view {} could not be created: {}", index,
              WmfMediaUtils::hresultToString(result));
      mDecodeSlots.clear();
      mDecodeTexture.Reset();
      return false;
    }
  }

  D3D11_VIDEO_DECODER_DESC decoderDescription{};
  decoderDescription.SampleWidth = static_cast<UINT>(mCodedWidth);
  decoderDescription.SampleHeight = static_cast<UINT>(mCodedHeight);
  decoderDescription.OutputFormat = DXGI_FORMAT_NV12;
  decoderDescription.Guid = mDecoderGuid;
  result = mVideoDevice->CreateVideoDecoder(&decoderDescription, &mDecoderConfig, &mVideoDecoder);
  if (FAILED(result) || mVideoDecoder == nullptr) {
    MINILOG_ERROR("the D3D11VA video decoder could not be created: {}", WmfMediaUtils::hresultToString(result));
    mDecodeSlots.clear();
    mDecodeTexture.Reset();
    return false;
  }
  return true;
}

bool D3D11VAVideoDecoder::createConversionResources() {
  mConvertedSlots.clear();
  mVideoProcessor.Reset();
  mProcessorEnumerator.Reset();
  if (!mConvertOutput) {
    return true;
  }

  D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
  content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
  const auto frameRate =
          mInputFormat != nullptr && mInputFormat->getFrameRate() > 0 ? mInputFormat->getFrameRate() : 30;
  content.InputFrameRate.Numerator = static_cast<UINT>(frameRate);
  content.InputFrameRate.Denominator = 1;
  content.InputWidth = static_cast<UINT>((mCodedWidth + 15) & ~15);
  content.InputHeight = static_cast<UINT>((mCodedHeight + 15) & ~15);
  content.OutputWidth = static_cast<UINT>(mWidth);
  content.OutputHeight = static_cast<UINT>(mHeight);
  content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
  if (FAILED(mVideoDevice->CreateVideoProcessorEnumerator(&content, &mProcessorEnumerator)) ||
          mProcessorEnumerator == nullptr) {
    MINILOG_WARN("the D3D11VA decoder could not create a video processor");
    return false;
  }
  UINT inputSupport = 0;
  UINT outputSupport = 0;
  if (FAILED(mProcessorEnumerator->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &inputSupport)) ||
          (inputSupport & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) == 0 ||
          FAILED(mProcessorEnumerator->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &outputSupport)) ||
          (outputSupport & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) == 0) {
    MINILOG_WARN("the video processor does not support the NV12 to BGRA conversion");
    return false;
  }
  if (FAILED(mVideoDevice->CreateVideoProcessor(mProcessorEnumerator.Get(), 0, &mVideoProcessor)) ||
          mVideoProcessor == nullptr) {
    MINILOG_WARN("the D3D11VA decoder could not create the video processor");
    return false;
  }
  const auto outputSpace = outputColorSpace();
  mVideoContext->VideoProcessorSetOutputColorSpace(mVideoProcessor.Get(), &outputSpace);

  mConvertedSlots.resize(kConvertedSlotCount);
  for (int index = 0; index < kConvertedSlotCount; ++index) {
    D3D11_TEXTURE2D_DESC description{};
    description.Width = static_cast<UINT>(mWidth);
    description.Height = static_cast<UINT>(mHeight);
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    // The effect graph imports the texture through a shared NT handle; the decoder waits for its own
    // write, so no keyed mutex is needed on top of that.
    description.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    HRESULT result = mDevice->CreateTexture2D(&description, nullptr, &mConvertedSlots[index].texture);
    if (FAILED(result)) {
      description.MiscFlags = 0;
      result = mDevice->CreateTexture2D(&description, nullptr, &mConvertedSlots[index].texture);
    }
    if (FAILED(result) || mConvertedSlots[index].texture == nullptr) {
      MINILOG_WARN("the D3D11VA conversion texture could not be created: {}", WmfMediaUtils::hresultToString(result));
      mConvertedSlots.clear();
      return false;
    }
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC viewDescription{};
    viewDescription.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    viewDescription.Texture2D.MipSlice = 0;
    result = mVideoDevice->CreateVideoProcessorOutputView(mConvertedSlots[index].texture.Get(),
            mProcessorEnumerator.Get(), &viewDescription, &mConvertedSlots[index].view);
    if (FAILED(result) || mConvertedSlots[index].view == nullptr) {
      MINILOG_WARN("the D3D11VA conversion view could not be created: {}", WmfMediaUtils::hresultToString(result));
      mConvertedSlots.clear();
      return false;
    }
  }
  return true;
}

bool D3D11VAVideoDecoder::ensureCodec() {
  if (mCodecReady) {
    return true;
  }
  if (mSpsMap.empty() || mPpsMap.empty()) {
    return false;
  }

  d3d11va::Pps pps = mPpsMap.begin()->second;
  auto spsIt = mSpsMap.find(pps.spsId);
  if (spsIt == mSpsMap.end()) {
    spsIt = mSpsMap.begin();
  }
  const d3d11va::Sps sps = spsIt->second;
  if (!isSupportedSps(sps, pps)) {
    MINILOG_DEBUG("the D3D11VA decoder cannot decode this H.264 profile/level combination");
    return false;
  }

  mSps = sps;
  mPps = pps;
  const auto requestedProcessFormat =
          mInputFormat != nullptr ? mInputFormat->getProcessFormat() : PixelFormat::kFormatInvalid;
  if (requestedProcessFormat != PixelFormat::kFormatInvalid && requestedProcessFormat != PixelFormat::kFormatNV12 &&
          requestedProcessFormat != PixelFormat::kFormatBGRA) {
    MINILOG_DEBUG("the D3D11VA decoder ignores the requested process format {}; the frames stay NV12",
            static_cast<int>(requestedProcessFormat));
  }
  MINILOG_DEBUG("D3D11VA SPS: {}x{} coded {}x{} poc={} log2fn={} log2poc={} maxref={} reorder={}", sps.width,
          sps.height, sps.codedWidth, sps.codedHeight, sps.picOrderCntType, sps.log2MaxFrameNum,
          sps.log2MaxPicOrderCntLsb, sps.maxNumRefFrames, sps.maxNumReorderFrames);
  MINILOG_DEBUG("D3D11VA PPS: id={} entropy={} weighted={} bipred={} deblock={} refs={}/{} 8x8={}", pps.id,
          pps.entropyCoding, pps.weightedPred, pps.weightedBipredIdc, pps.deblockingFilterControlPresent,
          pps.numRefIdxL0DefaultActiveMinus1, pps.numRefIdxL1DefaultActiveMinus1, pps.transform8x8Mode);
  mCodedWidth = sps.codedWidth;
  mCodedHeight = sps.codedHeight;
  mWidth = sps.width;
  mHeight = sps.height;
  mReorderDepth = std::clamp(sps.maxNumReorderFrames >= 0 ? sps.maxNumReorderFrames : sps.maxNumRefFrames, 0, 16);

  if (!selectDecoderProfile() || !createDecodeResources()) {
    return false;
  }
  if (mConvertOutput && !createConversionResources()) {
    // A device whose processor cannot write BGRA keeps the native layout; the caller can still
    // convert the frame, it only loses the direct texture path.
    MINILOG_WARN("the requested BGRA output is not available; the frames stay NV12");
    mConvertOutput = false;
  }

  mOutputColorInfo = mergeColorInfo(mInputFormat != nullptr ? mInputFormat->getColorInfo() : ColorInfo{}, sps.color);
  mOutputConfig = VideoConfiguration(static_cast<uint32_t>(mWidth), static_cast<uint32_t>(mHeight));
  mOutputConfig.pixelFormat = mConvertOutput ? PixelFormat::kFormatBGRA : PixelFormat::kFormatNV12;
  mOutputFormat = MediaFormat::CreateVideoFormat(MimeTypes::VIDEO_RAW, mWidth, mHeight);
  mOutputFormat->setPixelFormat(mOutputConfig.pixelFormat);
  mOutputFormat->setColorInfo(mOutputColorInfo);
  if (mInputFormat != nullptr) {
    mOutputFormat->setFrameRate(mInputFormat->getFrameRate());
  }
  mCodecReady = true;
  MINILOG_DEBUG("the D3D11VA decoder is configured: {}", mOutputFormat->toString());
  return true;
}

// --------------------------------------------------------------------------------------------------

void D3D11VAVideoDecoder::start() {
  if (mFallback) {
    mFallback->start();
    return;
  }
  mInputDequeued = false;
  mInputEndOfStream = false;
  mOutputEndOfStream = false;
  mDrainDone = false;
}

void D3D11VAVideoDecoder::stop() {
  if (mFallback) {
    mFallback->stop();
    return;
  }
  flush();
}

void D3D11VAVideoDecoder::flush() {
  if (mFallback) {
    mFallback->flush();
    return;
  }
  resetPictureBuffer();
  for (const auto& frame: mPendingFrames) {
    if (frame != nullptr) {
      frame->release();
    }
  }
  mPendingFrames.clear();
  if (mInputBuffer != nullptr) {
    mInputBuffer->clear();
  }
  mInputDequeued = false;
  mInputEndOfStream = false;
  mOutputEndOfStream = false;
  mDrainDone = false;
  mDiscardUntilIdr = false;
  mPreviousFrameNum = 0;
  mPreviousFrameNumOffset = 0;
  mPreviousPicOrderCntMsb = 0;
  mPreviousPicOrderCntLsb = 0;
  mOutstandingOutputCount = 0;
}

void D3D11VAVideoDecoder::release() {
  if (mFallback) {
    mFallback->release();
    mFallback.reset();
  }
  releaseD3D11VAState();
  mInputBuffer.reset();
  mOutputFormat.reset();
  mInputFormat.reset();
  mConfigured = false;
}

void D3D11VAVideoDecoder::releaseD3D11VAState() {
  resetPictureBuffer();
  for (const auto& frame: mPendingFrames) {
    if (frame != nullptr) {
      frame->release();
    }
  }
  mPendingFrames.clear();
  mInputBuffer.reset();
  mConvertedSlots.clear();
  mVideoProcessor.Reset();
  mProcessorEnumerator.Reset();
  mDecodeSlots.clear();
  mDecodeTexture.Reset();
  mVideoDecoder.Reset();
  mCompletionQuery.Reset();
  mVideoContext.Reset();
  mVideoDevice.Reset();
  mContext.Reset();
  mDevice.Reset();
  mHardwareSurface.reset();
  mSpsMap.clear();
  mPpsMap.clear();
  mCodecReady = false;
  mConvertOutput = false;
  mInputDequeued = false;
  mInputEndOfStream = false;
  mOutputEndOfStream = false;
  mDrainDone = false;
  mDiscardUntilIdr = false;
  mConfigured = false;
  mOutstandingOutputCount = 0;
}

// --------------------------------------------------------------------------------------------------

std::shared_ptr<AVPacketBuffer> D3D11VAVideoDecoder::dequeueInputBuffer() {
  if (mFallback) {
    return mFallback->dequeueInputBuffer();
  }
  if (!mConfigured || mInputEndOfStream || mInputDequeued || mInputBuffer == nullptr) {
    return nullptr;
  }
  if (mPendingFrames.size() >= MAX_PENDING_FRAMES || mOutstandingOutputCount >= MAX_OUTSTANDING_FRAMES) {
    return nullptr;
  }
  mInputDequeued = true;
  mInputBuffer->clear();
  return mInputBuffer;
}

void D3D11VAVideoDecoder::queueInputBuffer(std::shared_ptr<AVPacketBuffer> inputBuffer) {
  if (mFallback) {
    mFallback->queueInputBuffer(std::move(inputBuffer));
    return;
  }
  if (!inputBuffer) {
    return;
  }
  mInputDequeued = false;
  if (inputBuffer->isEndOfStream()) {
    mInputEndOfStream = true;
    return;
  }
  if (inputBuffer->isCodecConfig()) {
    return;
  }
  const auto size = inputBuffer->size();
  if (size == 0 || inputBuffer->array() == nullptr) {
    return;
  }
  if (!decodeAccessUnit(inputBuffer->array(), size, inputBuffer->timeUs(), inputBuffer->isKeyFrame())) {
    MINILOG_WARN("the D3D11VA decoder dropped an H.264 access unit");
  }
}

std::shared_ptr<AVPixelBuffer> D3D11VAVideoDecoder::dequeueOutputBuffer() {
  if (mFallback) {
    return mFallback->dequeueOutputBuffer();
  }
  if (!mPendingFrames.empty()) {
    auto frame = std::move(mPendingFrames.front());
    mPendingFrames.pop_front();
    if (frame != nullptr && !frame->isEndOfStream()) {
      ++mOutstandingOutputCount;
    }
    return frame;
  }
  if (mInputEndOfStream && !mOutputEndOfStream) {
    if (!mDrainDone) {
      bumpOutput(true);
      mDrainDone = true;
      if (!mPendingFrames.empty()) {
        auto frame = std::move(mPendingFrames.front());
        mPendingFrames.pop_front();
        if (frame != nullptr && !frame->isEndOfStream()) {
          ++mOutstandingOutputCount;
        }
        return frame;
      }
    }
    mOutputEndOfStream = true;
    return createEndOfStreamBuffer();
  }
  if (mOutputEndOfStream) {
    return createEndOfStreamBuffer();
  }
  return nullptr;
}

void D3D11VAVideoDecoder::releaseOutputBuffer(const std::shared_ptr<AVPixelBuffer>& outputBuffer, const bool render) {
  static_cast<void>(render);
  if (mFallback) {
    mFallback->releaseOutputBuffer(outputBuffer, render);
    return;
  }
  if (outputBuffer) {
    outputBuffer->release();
  }
}

void D3D11VAVideoDecoder::signalEndOfInputStream() {
  if (mFallback) {
    mFallback->signalEndOfInputStream();
    return;
  }
  mInputEndOfStream = true;
}

void D3D11VAVideoDecoder::setOutputStartTimeUs(const int64_t outputStartTimeUs) {
  if (mFallback) {
    mFallback->setOutputStartTimeUs(outputStartTimeUs);
    return;
  }
  mOutputStartTimeUs = outputStartTimeUs;
}

void D3D11VAVideoDecoder::recycleBuffer(std::shared_ptr<AVPixelBuffer> frameBuffer) {
  if (!frameBuffer) {
    return;
  }
  auto hardware = std::dynamic_pointer_cast<D3D11VAHardwarePixelBuffer>(frameBuffer);
  if (hardware == nullptr) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mSlotMutex);
    if (hardware->isConverted()) {
      const auto index = hardware->slotIndex();
      if (index >= 0 && index < static_cast<int>(mConvertedSlots.size()) && mConvertedSlots[index].consumerHolds > 0) {
        --mConvertedSlots[index].consumerHolds;
      }
    } else {
      const auto index = hardware->slotIndex();
      if (index >= 0 && index < static_cast<int>(mDecodeSlots.size()) && mDecodeSlots[index].consumerHolds > 0) {
        --mDecodeSlots[index].consumerHolds;
      }
    }
  }
  if (mOutstandingOutputCount > 0) {
    --mOutstandingOutputCount;
  }
}

std::shared_ptr<AVPixelBuffer> D3D11VAVideoDecoder::createEndOfStreamBuffer() {
  auto buffer = AVPixelBuffer::newFlagsOnlyInstance();
  buffer->setFlags(AVBufferFlags::kEndOfStream);
  return buffer;
}

// --------------------------------------------------------------------------------------------------
// Decoding
// --------------------------------------------------------------------------------------------------

bool D3D11VAVideoDecoder::decodeAccessUnit(const uint8_t* data, const size_t size, const int64_t timeUs,
        const bool keyFrame) {
  if (mDiscardUntilIdr && !keyFrame) {
    return true;
  }
  int lengthSize = 4;
  if (mInputFormat != nullptr && !mInputFormat->csdList().empty()) {
    lengthSize = d3d11va::nalLengthSizeOf(mInputFormat->csdList()[0]);
  }
  const auto format = d3d11va::detectNalFormat(data, size, lengthSize);
  std::vector<d3d11va::NalUnit> units;
  if (!d3d11va::splitNalUnits(data, size, lengthSize, format, units)) {
    return false;
  }

  // Parameter sets can be carried by the sample itself, which is how a stream without a container
  // header describes its codec. They are collected before the slices of the same sample are decoded.
  bool parameterSetsChanged = false;
  for (const auto& unit: units) {
    if (unit.size < 1 || (unit.type != 7 && unit.type != 8)) {
      continue;
    }
    const auto rbsp = d3d11va::extractRbsp(unit.data + 1, unit.size - 1);
    if (unit.type == 7) {
      d3d11va::Sps sps;
      if (d3d11va::parseSps(rbsp, sps)) {
        if (!mSpsMap.contains(sps.id) || mSpsMap[sps.id].codedWidth != sps.codedWidth ||
                mSpsMap[sps.id].codedHeight != sps.codedHeight) {
          parameterSetsChanged = true;
        }
        mSpsMap[sps.id] = sps;
      }
    } else {
      d3d11va::Pps pps;
      if (d3d11va::parsePps(rbsp, pps)) {
        mPpsMap[pps.id] = pps;
      }
    }
  }

  if (!mCodecReady) {
    if (!ensureCodec()) {
      // The stream uses a feature this front-end does not forward, or the device cannot run the
      // profile. The platform decoder takes over while nothing has been decoded yet.
      return switchToFallback(data, size, timeUs, keyFrame);
    }
  } else if (parameterSetsChanged && keyFrame) {
    // A new sequence starts with its parameter sets. The DPB and the decode surfaces belong to the
    // sequence which just ended, so they are dropped and rebuilt for the new coded size.
    resetPictureBuffer();
    mCodecReady = false;
    if (!ensureCodec()) {
      mDiscardUntilIdr = true;
      return false;
    }
  }

  std::vector<d3d11va::NalUnit> slices;
  slices.reserve(units.size());
  for (const auto& unit: units) {
    if (unit.type == 1 || unit.type == 5) {
      slices.push_back(unit);
    }
  }
  if (slices.empty()) {
    return true;
  }
  if (!decodePicture(slices, timeUs, keyFrame)) {
    return false;
  }
  mDiscardUntilIdr = false;
  return true;
}

bool D3D11VAVideoDecoder::decodePicture(const std::vector<d3d11va::NalUnit>& slices, const int64_t timeUs,
        const bool keyFrame) {
  if (slices.empty()) {
    return false;
  }

  d3d11va::SliceHeader firstHeader;
  bool firstParsed = false;
  for (const auto& unit: slices) {
    if (unit.size < 1) {
      continue;
    }
    const auto rbsp = d3d11va::extractRbsp(unit.data + 1, unit.size - 1);
    // The picture parameters are read first, then the header selects the parameter set of the slice.
    uint32_t ppsId = 0;
    {
      d3d11va::BitReader reader(rbsp.data(), rbsp.size());
      uint32_t value = 0;
      if (!reader.readUe(value) || !reader.readUe(value) || !reader.readUe(ppsId)) {
        continue;
      }
    }
    auto ppsIt = mPpsMap.find(static_cast<int>(ppsId));
    if (ppsIt == mPpsMap.end()) {
      continue;
    }
    auto spsIt = mSpsMap.find(ppsIt->second.spsId);
    if (spsIt == mSpsMap.end()) {
      continue;
    }
    d3d11va::SliceHeader header;
    if (d3d11va::parseSliceHeader(rbsp, spsIt->second, ppsIt->second, unit.type, unit.refIdc, header)) {
      if (!firstParsed) {
        firstHeader = header;
        firstParsed = true;
      }
    }
  }
  if (!firstParsed) {
    MINILOG_DEBUG("the D3D11VA decoder could not parse a slice header");
    return false;
  }
  if (firstHeader.fieldPic || !mSps.frameMbsOnly) {
    MINILOG_DEBUG("the D3D11VA decoder does not decode interlaced pictures");
    return false;
  }
  const int sliceType = firstHeader.sliceType % 5;
  if (sliceType == 3 || sliceType == 4) {
    MINILOG_DEBUG("the D3D11VA decoder does not decode SP/SI pictures");
    return false;
  }
  if (firstHeader.ppsId != mPps.id) {
    MINILOG_DEBUG("the D3D11VA decoder dropped a slice of another parameter set");
    return false;
  }

  const bool idr = firstHeader.nalType == 5;
  if (idr) {
    // The picture order count restarts at an IDR, so the pictures of the sequence which just ended
    // have to be output before the new one enters the buffer: sorting them together would put the
    // IDR of count zero in front of frames that are still waiting for their presentation time.
    bumpOutput(true);
  }
  int topFieldOrderCnt = 0;
  int bottomFieldOrderCnt = 0;
  uint32_t frameNumOffset = 0;
  computePictureOrderCount(mSps, mPps, firstHeader, idr, topFieldOrderCnt, bottomFieldOrderCnt, frameNumOffset);

  int slot = allocateDecodeSlot();
  if (slot < 0) {
    // The DPB holds every surface. Outputting the pictures whose display order settled is what
    // releases the surfaces; when they are all references of a live stream the frame is dropped.
    bumpOutput(true);
    slot = allocateDecodeSlot();
  }
  if (slot < 0) {
    MINILOG_WARN("the D3D11VA decoder has no free decode surface (pictures={}, short={}, "
                 "long={}, pending={}, outstanding={})",
            mPictures.size(), mShortTermReferences.size(), mLongTermReferences.size(), mPendingFrames.size(),
            mOutstandingOutputCount.load());
    return false;
  }

  DXVA_PicParams_H264 pictureParameters{};
  std::memset(&pictureParameters, 0, sizeof(pictureParameters));
  pictureParameters.wFrameWidthInMbsMinus1 = static_cast<USHORT>(mSps.picWidthInMbs - 1);
  pictureParameters.wFrameHeightInMbsMinus1 =
          static_cast<USHORT>((mSps.frameMbsOnly ? mSps.picHeightInMapUnits : mSps.picHeightInMapUnits * 2) - 1);
  pictureParameters.CurrPic.Index7Bits = static_cast<UCHAR>(slot & 0x7f);
  pictureParameters.CurrPic.AssociatedFlag = 0;
  pictureParameters.num_ref_frames = static_cast<UCHAR>(mSps.maxNumRefFrames);
  pictureParameters.field_pic_flag = 0;
  pictureParameters.MbaffFrameFlag = 0;
  pictureParameters.residual_colour_transform_flag = 0;
  pictureParameters.sp_for_switch_flag = 0;
  pictureParameters.chroma_format_idc = static_cast<USHORT>(mSps.chromaFormatIdc);
  pictureParameters.RefPicFlag = firstHeader.nalRefIdc != 0;
  pictureParameters.constrained_intra_pred_flag = mPps.constrainedIntraPred ? 1 : 0;
  pictureParameters.weighted_pred_flag = mPps.weightedPred ? 1 : 0;
  pictureParameters.weighted_bipred_idc = static_cast<USHORT>(mPps.weightedBipredIdc);
  pictureParameters.MbsConsecutiveFlag = 1;
  pictureParameters.frame_mbs_only_flag = mSps.frameMbsOnly ? 1 : 0;
  pictureParameters.transform_8x8_mode_flag = mPps.transform8x8Mode ? 1 : 0;
  pictureParameters.MinLumaBipredSize8x8Flag = mSps.levelIdc >= 31 ? 1 : 0;
  pictureParameters.IntraPicFlag = (firstHeader.sliceType % 5) == 2 || (firstHeader.sliceType % 5) == 4 ? 1 : 0;
  pictureParameters.bit_depth_luma_minus8 = static_cast<UCHAR>(mSps.bitDepthLuma - 8);
  pictureParameters.bit_depth_chroma_minus8 = static_cast<UCHAR>(mSps.bitDepthChroma - 8);
  pictureParameters.Reserved16Bits = 3;
  pictureParameters.StatusReportFeedbackNumber = static_cast<UINT>(mStatusReportFeedbackNumber++);
  pictureParameters.CurrFieldOrderCnt[0] = topFieldOrderCnt;
  pictureParameters.CurrFieldOrderCnt[1] = bottomFieldOrderCnt;
  pictureParameters.pic_init_qs_minus26 = static_cast<CHAR>(mPps.picInitQs - 26);
  pictureParameters.chroma_qp_index_offset = static_cast<CHAR>(mPps.chromaQpIndexOffset);
  pictureParameters.second_chroma_qp_index_offset = static_cast<CHAR>(mPps.secondChromaQpIndexOffset);
  pictureParameters.ContinuationFlag = 1;
  pictureParameters.pic_init_qp_minus26 = static_cast<CHAR>(mPps.picInitQp - 26);
  pictureParameters.num_ref_idx_l0_active_minus1 = static_cast<UCHAR>(
          firstHeader.overrideNumRefIdx ? firstHeader.numRefIdxL0ActiveMinus1 : mPps.numRefIdxL0DefaultActiveMinus1);
  pictureParameters.num_ref_idx_l1_active_minus1 = static_cast<UCHAR>(
          firstHeader.overrideNumRefIdx ? firstHeader.numRefIdxL1ActiveMinus1 : mPps.numRefIdxL1DefaultActiveMinus1);
  pictureParameters.frame_num = static_cast<USHORT>(firstHeader.frameNum);
  pictureParameters.log2_max_frame_num_minus4 = static_cast<UCHAR>(mSps.log2MaxFrameNum - 4);
  pictureParameters.pic_order_cnt_type = static_cast<UCHAR>(mSps.picOrderCntType);
  pictureParameters.log2_max_pic_order_cnt_lsb_minus4 = static_cast<UCHAR>(std::max(0, mSps.log2MaxPicOrderCntLsb - 4));
  pictureParameters.delta_pic_order_always_zero_flag = mSps.deltaPicOrderAlwaysZero ? 1 : 0;
  pictureParameters.direct_8x8_inference_flag = mSps.direct8x8Inference ? 1 : 0;
  pictureParameters.entropy_coding_mode_flag = mPps.entropyCoding ? 1 : 0;
  pictureParameters.pic_order_present_flag = mPps.bottomFieldPicOrderInFramePresent ? 1 : 0;
  pictureParameters.num_slice_groups_minus1 = 0;
  pictureParameters.slice_group_map_type = 0;
  pictureParameters.deblocking_filter_control_present_flag = mPps.deblockingFilterControlPresent ? 1 : 0;
  pictureParameters.redundant_pic_cnt_present_flag = mPps.redundantPicCntPresent ? 1 : 0;
  pictureParameters.slice_group_change_rate_minus1 = 0;
  fillReferenceLists(mSps, pictureParameters, slot, firstHeader);

  if (!submitFrame(pictureParameters, slices, slot)) {
    return false;
  }

  auto picture = std::make_shared<Picture>();
  picture->slot = slot;
  picture->frameNum = firstHeader.frameNum;
  picture->frameNumWrap = firstHeader.frameNum;
  picture->topFieldOrderCnt = topFieldOrderCnt;
  picture->bottomFieldOrderCnt = bottomFieldOrderCnt;
  picture->timeUs = timeUs;
  picture->keyFrame = keyFrame;
  {
    std::lock_guard<std::mutex> lock(mSlotMutex);
    ++mDecodeSlots[slot].decoderHolds;
  }
  mPictures.push_back(picture);

  applyReferenceMarking(mSps, mPps, firstHeader, idr, picture);

  mPreviousFrameNum = firstHeader.frameNum;
  mPreviousFrameNumOffset = frameNumOffset;
  if (firstHeader.nalRefIdc != 0 && mSps.picOrderCntType == 0) {
    mPreviousPicOrderCntLsb = static_cast<int>(firstHeader.picOrderCntLsb);
    mPreviousPicOrderCntMsb = topFieldOrderCnt - mPreviousPicOrderCntLsb;
  }

  bumpOutput(false);
  return true;
}

bool D3D11VAVideoDecoder::submitFrame(DXVA_PicParams_H264& pictureParameters,
        const std::vector<d3d11va::NalUnit>& slices, const int slot) {
  if (!mVideoDecoder || slot < 0 || slot >= static_cast<int>(mDecodeSlots.size())) {
    return false;
  }

  const int macroblocks =
          mSps.picWidthInMbs * (mSps.frameMbsOnly ? mSps.picHeightInMapUnits : mSps.picHeightInMapUnits * 2);

  HRESULT result = mVideoContext->DecoderBeginFrame(mVideoDecoder.Get(), mDecodeSlots[slot].view.Get(), 0, nullptr);
  if (result == E_PENDING) {
    for (int attempt = 0; attempt < 50 && result == E_PENDING; ++attempt) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      result = mVideoContext->DecoderBeginFrame(mVideoDecoder.Get(), mDecodeSlots[slot].view.Get(), 0, nullptr);
    }
  }
  if (FAILED(result)) {
    MINILOG_WARN("the D3D11VA decoder could not begin a frame: {}", WmfMediaUtils::hresultToString(result));
    return false;
  }

  bool success = true;

  // Picture parameters.
  void* bufferData = nullptr;
  UINT bufferSize = 0;
  result = mVideoContext->GetDecoderBuffer(mVideoDecoder.Get(), D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS,
          &bufferSize, &bufferData);
  if (FAILED(result) || bufferData == nullptr || bufferSize < sizeof(pictureParameters)) {
    MINILOG_WARN("the D3D11VA picture parameter buffer is not available");
    success = false;
  } else {
    std::memcpy(bufferData, &pictureParameters, sizeof(pictureParameters));
    mVideoContext->ReleaseDecoderBuffer(mVideoDecoder.Get(), D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS);
  }

  // Slice data. The driver parses the slice headers, so every slice NAL is copied with the start code
  // which the DXVA stream syntax uses.
  std::vector<DXVA_Slice_H264_Short> sliceControl;
  sliceControl.resize(slices.size());
  UINT bitstreamWritten = 0;
  if (success) {
    result = mVideoContext->GetDecoderBuffer(mVideoDecoder.Get(), D3D11_VIDEO_DECODER_BUFFER_BITSTREAM, &bufferSize,
            &bufferData);
    if (FAILED(result) || bufferData == nullptr) {
      MINILOG_WARN("the D3D11VA bitstream buffer is not available");
      success = false;
    } else {
      auto* const bitstream = static_cast<uint8_t*>(bufferData);
      for (size_t index = 0; index < slices.size(); ++index) {
        const auto& unit = slices[index];
        const size_t required = 3 + unit.size;
        if (bitstreamWritten + required > bufferSize) {
          MINILOG_WARN("the D3D11VA bitstream buffer is too small");
          success = false;
          break;
        }
        bitstream[bitstreamWritten] = 0;
        bitstream[bitstreamWritten + 1] = 0;
        bitstream[bitstreamWritten + 2] = 1;
        std::memcpy(bitstream + bitstreamWritten + 3, unit.data, unit.size);
        sliceControl[index].BSNALunitDataLocation = bitstreamWritten;
        sliceControl[index].SliceBytesInBuffer = static_cast<UINT>(required);
        sliceControl[index].wBadSliceChopping = 0;
        bitstreamWritten += static_cast<UINT>(required);
      }
      if (success) {
        // The driver wants the bitstream padded to a 128 byte boundary; the padding belongs to the
        // last slice so that the driver reads it as part of that slice.
        const UINT padding = (128 - (bitstreamWritten % 128)) % 128;
        if (bitstreamWritten + padding <= bufferSize) {
          std::memset(bitstream + bitstreamWritten, 0, padding);
          bitstreamWritten += padding;
          if (!sliceControl.empty()) {
            sliceControl.back().SliceBytesInBuffer += padding;
          }
        }
      }
      mVideoContext->ReleaseDecoderBuffer(mVideoDecoder.Get(), D3D11_VIDEO_DECODER_BUFFER_BITSTREAM);
    }
  }

  // The slice control table.
  if (success) {
    result = mVideoContext->GetDecoderBuffer(mVideoDecoder.Get(), D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL, &bufferSize,
            &bufferData);
    if (FAILED(result) || bufferData == nullptr || bufferSize < sliceControl.size() * sizeof(DXVA_Slice_H264_Short)) {
      MINILOG_WARN("the D3D11VA slice control buffer is not available");
      success = false;
    } else {
      std::memcpy(bufferData, sliceControl.data(), sliceControl.size() * sizeof(DXVA_Slice_H264_Short));
      mVideoContext->ReleaseDecoderBuffer(mVideoDecoder.Get(), D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL);
    }
  }

  if (success) {
    D3D11_VIDEO_DECODER_BUFFER_DESC descriptions[3];
    fillBufferDesc(descriptions[0], D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS,
            static_cast<UINT>(sizeof(pictureParameters)), 0);
    fillBufferDesc(descriptions[1], D3D11_VIDEO_DECODER_BUFFER_BITSTREAM, bitstreamWritten,
            static_cast<UINT>(macroblocks));
    fillBufferDesc(descriptions[2], D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL,
            static_cast<UINT>(sliceControl.size() * sizeof(DXVA_Slice_H264_Short)), static_cast<UINT>(macroblocks));
    result = mVideoContext->SubmitDecoderBuffers(mVideoDecoder.Get(), 3, descriptions);
    if (FAILED(result)) {
      MINILOG_WARN("the D3D11VA decoder rejected the frame: {}", WmfMediaUtils::hresultToString(result));
      success = false;
    }
  }

  result = mVideoContext->DecoderEndFrame(mVideoDecoder.Get());
  if (FAILED(result)) {
    MINILOG_WARN("the D3D11VA decoder could not end the frame: {}", WmfMediaUtils::hresultToString(result));
    success = false;
  }
  if (!success) {
    return false;
  }
  return waitForGpu();
}

void D3D11VAVideoDecoder::fillBufferDesc(D3D11_VIDEO_DECODER_BUFFER_DESC& description, const UINT type, const UINT size,
        const UINT macroblocks) {
  description = {};
  description.BufferType = static_cast<D3D11_VIDEO_DECODER_BUFFER_TYPE>(type);
  description.DataSize = size;
  description.NumMBsInBuffer = macroblocks;
}

bool D3D11VAVideoDecoder::waitForGpu() {
  if (mContext == nullptr || mCompletionQuery == nullptr) {
    return true;
  }
  mContext->End(mCompletionQuery.Get());
  mContext->Flush();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  for (;;) {
    const HRESULT status = mContext->GetData(mCompletionQuery.Get(), nullptr, 0, 0);
    if (status == S_OK) {
      return true;
    }
    if (status != S_FALSE) {
      MINILOG_WARN("the D3D11VA completion query failed: {}", WmfMediaUtils::hresultToString(status));
      return false;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      MINILOG_WARN("the D3D11VA decode did not complete in time");
      return false;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
}

bool D3D11VAVideoDecoder::switchToFallback(const uint8_t* data, const size_t size, const int64_t timeUs,
        const bool keyFrame) {
  if (!mPictures.empty()) {
    // Frames of this stream were already decoded; switching would break the reference chain.
    MINILOG_WARN("the D3D11VA decoder cannot fall back after decoding started");
    return false;
  }
  auto format = mInputFormat;
  auto surface = mHardwareSurface;
  if (!configureFallback(format, surface)) {
    return false;
  }
  if (mFallback == nullptr) {
    return false;
  }
  // Feed the access unit that decided the fallback, so that the decoder does not miss a frame.
  auto input = mFallback->dequeueInputBuffer();
  if (input == nullptr) {
    return true;
  }
  input->ensureSpaceForWrite(size);
  const auto destination = input->data();
  destination->position(0);
  if (size > 0) {
    destination->put(data, size);
  }
  destination->flip();
  input->timeUs(timeUs);
  if (keyFrame) {
    input->addFlag(AVBufferFlags::kKeyFrame);
  }
  mFallback->queueInputBuffer(input);
  return true;
}

// --------------------------------------------------------------------------------------------------
// Reference picture management
// --------------------------------------------------------------------------------------------------

void D3D11VAVideoDecoder::computePictureOrderCount(const d3d11va::Sps& sps, const d3d11va::Pps& pps,
        const d3d11va::SliceHeader& header, const bool idr, int& topFieldOrderCnt, int& bottomFieldOrderCnt,
        uint32_t& frameNumOffset) {
  static_cast<void>(pps);
  const uint32_t maxFrameNum = 1U << sps.log2MaxFrameNum;
  if (idr) {
    frameNumOffset = 0;
  } else if (mPreviousFrameNum > header.frameNum) {
    frameNumOffset = mPreviousFrameNumOffset + maxFrameNum;
  } else {
    frameNumOffset = mPreviousFrameNumOffset;
  }

  if (sps.picOrderCntType == 0) {
    const uint32_t maxPicOrderCntLsb = 1U << sps.log2MaxPicOrderCntLsb;
    int previousMsb = idr ? 0 : mPreviousPicOrderCntMsb;
    const int previousLsb = idr ? 0 : mPreviousPicOrderCntLsb;
    int msb = previousMsb;
    if (header.picOrderCntLsb < static_cast<uint32_t>(previousLsb) &&
            static_cast<uint32_t>(previousLsb) - header.picOrderCntLsb >= maxPicOrderCntLsb / 2) {
      msb = previousMsb + static_cast<int>(maxPicOrderCntLsb);
    } else if (header.picOrderCntLsb > static_cast<uint32_t>(previousLsb) &&
               header.picOrderCntLsb - static_cast<uint32_t>(previousLsb) > maxPicOrderCntLsb / 2) {
      msb = previousMsb - static_cast<int>(maxPicOrderCntLsb);
    }
    topFieldOrderCnt = msb + static_cast<int>(header.picOrderCntLsb);
    bottomFieldOrderCnt = topFieldOrderCnt;
    if (pps.bottomFieldPicOrderInFramePresent && !header.fieldPic) {
      bottomFieldOrderCnt += header.deltaPicOrderCntBottom;
    }
  } else if (sps.picOrderCntType == 1) {
    int expectedDeltaPerCycle = 0;
    for (const auto offset: sps.offsetForRefFrame) {
      expectedDeltaPerCycle += offset;
    }
    uint32_t absFrameNum = 0;
    if (!sps.offsetForRefFrame.empty()) {
      absFrameNum = frameNumOffset + header.frameNum;
    }
    if (header.nalRefIdc == 0 && absFrameNum > 0) {
      --absFrameNum;
    }
    int expectedPicOrderCnt = 0;
    if (absFrameNum > 0) {
      const auto cycleCount = static_cast<uint32_t>(sps.offsetForRefFrame.size());
      const uint32_t cycleNumber = (absFrameNum - 1) / cycleCount;
      const uint32_t frameNumInCycle = (absFrameNum - 1) % cycleCount;
      expectedPicOrderCnt = static_cast<int>(cycleNumber) * expectedDeltaPerCycle;
      for (uint32_t index = 0; index <= frameNumInCycle && index < cycleCount; ++index) {
        expectedPicOrderCnt += sps.offsetForRefFrame[index];
      }
      if (header.nalRefIdc == 0) {
        expectedPicOrderCnt += sps.offsetForNonRefPic;
      }
    }
    topFieldOrderCnt = expectedPicOrderCnt + header.deltaPicOrderCnt[0];
    bottomFieldOrderCnt = topFieldOrderCnt + sps.offsetForTopToBottomField + header.deltaPicOrderCnt[1];
  } else {
    int tempPicOrderCnt = 0;
    if (idr) {
      tempPicOrderCnt = 0;
    } else if (header.nalRefIdc == 0) {
      tempPicOrderCnt = static_cast<int>(2 * (frameNumOffset + header.frameNum)) - 1;
    } else {
      tempPicOrderCnt = static_cast<int>(2 * (frameNumOffset + header.frameNum));
    }
    topFieldOrderCnt = tempPicOrderCnt;
    bottomFieldOrderCnt = tempPicOrderCnt;
  }
}

void D3D11VAVideoDecoder::fillReferenceLists(const d3d11va::Sps& sps, DXVA_PicParams_H264& parameters,
        const int currentSlot, const d3d11va::SliceHeader& header) {
  static_cast<void>(sps);
  static_cast<void>(currentSlot);
  static_cast<void>(header);
  int index = 0;
  for (const auto& picture: mShortTermReferences) {
    if (index >= 16 || picture == nullptr) {
      break;
    }
    parameters.RefFrameList[index].Index7Bits = static_cast<UCHAR>(picture->slot & 0x7f);
    parameters.RefFrameList[index].AssociatedFlag = 0;
    parameters.FieldOrderCntList[index][0] = picture->topFieldOrderCnt;
    parameters.FieldOrderCntList[index][1] = picture->bottomFieldOrderCnt;
    parameters.FrameNumList[index] = static_cast<USHORT>(picture->frameNum);
    parameters.UsedForReferenceFlags |= 1U << (2 * index);
    parameters.UsedForReferenceFlags |= 1U << (2 * index + 1);
    ++index;
  }
  for (const auto& entry: mLongTermReferences) {
    if (index >= 16 || entry.second == nullptr) {
      break;
    }
    const auto& picture = entry.second;
    parameters.RefFrameList[index].Index7Bits = static_cast<UCHAR>(picture->slot & 0x7f);
    parameters.RefFrameList[index].AssociatedFlag = 1;
    parameters.FieldOrderCntList[index][0] = picture->topFieldOrderCnt;
    parameters.FieldOrderCntList[index][1] = picture->bottomFieldOrderCnt;
    parameters.FrameNumList[index] = static_cast<USHORT>(picture->longTermPicId);
    parameters.UsedForReferenceFlags |= 1U << (2 * index);
    parameters.UsedForReferenceFlags |= 1U << (2 * index + 1);
    ++index;
  }
  for (; index < 16; ++index) {
    parameters.RefFrameList[index].bPicEntry = 0xff;
    parameters.FieldOrderCntList[index][0] = 0;
    parameters.FieldOrderCntList[index][1] = 0;
    parameters.FrameNumList[index] = 0;
  }
}

void D3D11VAVideoDecoder::applyReferenceMarking(const d3d11va::Sps& sps, const d3d11va::Pps& pps,
        const d3d11va::SliceHeader& header, const bool idr, const std::shared_ptr<Picture>& picture) {
  static_cast<void>(pps);
  const auto clearReferenceFlags = [](const std::shared_ptr<Picture>& target) {
    target->shortTermReference = false;
    target->longTermReference = false;
  };
  // A picture which stops being a reference and was already handed to the caller must give its
  // decode surface back: the surface is what the next picture is decoded into.
  const auto releaseIfFinished = [this](const std::shared_ptr<Picture>& target) {
    if (target != nullptr && !target->shortTermReference && !target->longTermReference && target->outputDone) {
      releasePictureSlot(target);
    }
  };

  if (idr) {
    for (const auto& reference: mShortTermReferences) {
      clearReferenceFlags(reference);
      releaseIfFinished(reference);
    }
    for (const auto& entry: mLongTermReferences) {
      clearReferenceFlags(entry.second);
      releaseIfFinished(entry.second);
    }
    mShortTermReferences.clear();
    mLongTermReferences.clear();
    if (header.longTermReference) {
      picture->longTermReference = true;
      picture->longTermPicId = 0;
      mLongTermReferences[0] = picture;
    } else {
      picture->shortTermReference = true;
      mShortTermReferences.insert(mShortTermReferences.begin(), picture);
    }
    return;
  }

  if (header.nalRefIdc == 0) {
    return;
  }

  if (!header.adaptiveRefPicMarking) {
    // Sliding window: the oldest short term reference leaves the DPB to make room for this picture.
    const int capacity = std::max(1, sps.maxNumRefFrames);
    while (static_cast<int>(mShortTermReferences.size() + mLongTermReferences.size()) >= capacity) {
      if (!mShortTermReferences.empty()) {
        dropShortTermReference(mShortTermReferences.back());
      } else if (!mLongTermReferences.empty()) {
        dropLongTermReference(mLongTermReferences.begin()->second);
      } else {
        break;
      }
    }
    picture->shortTermReference = true;
    mShortTermReferences.insert(mShortTermReferences.begin(), picture);
    return;
  }

  picture->shortTermReference = true;
  mShortTermReferences.insert(mShortTermReferences.begin(), picture);

  // The picture numbers of the memory management operations are relative to the current frame, and
  // wrap at `MaxFrameNum`. The reference pictures get their wrapped number for the comparison.
  const auto maxFrameNum = static_cast<uint32_t>(1U << sps.log2MaxFrameNum);
  for (const auto& reference: mShortTermReferences) {
    if (reference != nullptr) {
      reference->frameNumWrap =
              reference->frameNum > header.frameNum ? reference->frameNum - maxFrameNum : reference->frameNum;
    }
  }

  for (const auto& command: header.mmco) {
    switch (command.operation) {
      case 1: {
        const auto target = (header.frameNum + maxFrameNum - (command.argument1 + 1)) % maxFrameNum;
        auto reference = findShortTermReference(target);
        if (reference != nullptr) {
          dropShortTermReference(reference);
        }
        break;
      }
      case 2: {
        auto reference = findLongTermReference(command.argument1);
        if (reference != nullptr) {
          dropLongTermReference(reference);
        }
        break;
      }
      case 3: {
        const auto target = (header.frameNum + maxFrameNum - (command.argument1 + 1)) % maxFrameNum;
        auto reference = findShortTermReference(target);
        if (reference != nullptr) {
          reference->shortTermReference = false;
          mShortTermReferences.erase(std::remove(mShortTermReferences.begin(), mShortTermReferences.end(), reference),
                  mShortTermReferences.end());
          reference->longTermReference = true;
          reference->longTermPicId = command.argument2;
          mLongTermReferences[command.argument2] = reference;
        }
        break;
      }
      case 4:
      case 5: {
        if (command.operation == 5) {
          for (const auto& reference: mShortTermReferences) {
            clearReferenceFlags(reference);
            releaseIfFinished(reference);
          }
          for (const auto& entry: mLongTermReferences) {
            clearReferenceFlags(entry.second);
            releaseIfFinished(entry.second);
          }
          mShortTermReferences.clear();
          mLongTermReferences.clear();
        }
        break;
      }
      case 6: {
        picture->shortTermReference = false;
        mShortTermReferences.erase(std::remove(mShortTermReferences.begin(), mShortTermReferences.end(), picture),
                mShortTermReferences.end());
        picture->longTermReference = true;
        picture->longTermPicId = command.argument1;
        mLongTermReferences[command.argument1] = picture;
        break;
      }
      default:
        break;
    }
  }
}

void D3D11VAVideoDecoder::dropShortTermReference(const std::shared_ptr<Picture>& picture) {
  if (picture == nullptr) {
    return;
  }
  picture->shortTermReference = false;
  mShortTermReferences.erase(std::remove(mShortTermReferences.begin(), mShortTermReferences.end(), picture),
          mShortTermReferences.end());
  if (!picture->longTermReference && picture->outputDone) {
    releasePictureSlot(picture);
  }
}

void D3D11VAVideoDecoder::dropLongTermReference(const std::shared_ptr<Picture>& picture) {
  if (picture == nullptr) {
    return;
  }
  picture->longTermReference = false;
  for (auto it = mLongTermReferences.begin(); it != mLongTermReferences.end();) {
    if (it->second == picture) {
      it = mLongTermReferences.erase(it);
    } else {
      ++it;
    }
  }
  if (!picture->shortTermReference && picture->outputDone) {
    releasePictureSlot(picture);
  }
}

std::shared_ptr<D3D11VAVideoDecoder::Picture> D3D11VAVideoDecoder::findShortTermReference(const uint32_t frameNumWrap) {
  for (const auto& picture: mShortTermReferences) {
    if (picture != nullptr && picture->frameNumWrap == frameNumWrap) {
      return picture;
    }
  }
  return nullptr;
}

std::shared_ptr<D3D11VAVideoDecoder::Picture> D3D11VAVideoDecoder::findLongTermReference(
        const uint32_t longTermPicNum) {
  const auto it = mLongTermReferences.find(longTermPicNum);
  return it == mLongTermReferences.end() ? nullptr : it->second;
}

void D3D11VAVideoDecoder::releasePictureSlot(const std::shared_ptr<Picture>& picture) {
  if (picture == nullptr || picture->slot < 0) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mSlotMutex);
    if (picture->slot < static_cast<int>(mDecodeSlots.size()) && mDecodeSlots[picture->slot].decoderHolds > 0) {
      --mDecodeSlots[picture->slot].decoderHolds;
    }
  }
  const auto it = std::find(mPictures.begin(), mPictures.end(), picture);
  if (it != mPictures.end()) {
    mPictures.erase(it);
  }
}

void D3D11VAVideoDecoder::resetPictureBuffer() {
  std::lock_guard<std::mutex> lock(mSlotMutex);
  for (const auto& picture: mPictures) {
    if (picture == nullptr) {
      continue;
    }
    if (picture->slot >= 0 && picture->slot < static_cast<int>(mDecodeSlots.size()) &&
            mDecodeSlots[picture->slot].decoderHolds > 0) {
      --mDecodeSlots[picture->slot].decoderHolds;
    }
    picture->shortTermReference = false;
    picture->longTermReference = false;
  }
  mPictures.clear();
  mShortTermReferences.clear();
  mLongTermReferences.clear();
}

// --------------------------------------------------------------------------------------------------
// Output
// --------------------------------------------------------------------------------------------------

int D3D11VAVideoDecoder::reorderDepth() const { return std::clamp(mReorderDepth, 0, 16); }

void D3D11VAVideoDecoder::bumpOutput(const bool force) {
  std::vector<std::shared_ptr<Picture>> pending;
  pending.reserve(mPictures.size());
  for (const auto& picture: mPictures) {
    if (picture != nullptr && picture->outputPending && !picture->outputDone) {
      pending.push_back(picture);
    }
  }
  if (pending.empty()) {
    return;
  }
  std::sort(pending.begin(), pending.end(),
          [](const std::shared_ptr<Picture>& left, const std::shared_ptr<Picture>& right) {
            if (left->topFieldOrderCnt == right->topFieldOrderCnt) {
              return left->frameNum < right->frameNum;
            }
            return left->topFieldOrderCnt < right->topFieldOrderCnt;
          });

  if (force) {
    for (const auto& picture: pending) {
      emitPicture(picture);
    }
    return;
  }
  const auto depth = static_cast<size_t>(reorderDepth());
  size_t index = 0;
  while (pending.size() - index > depth) {
    if (!emitPicture(pending[index])) {
      break;
    }
    ++index;
  }
}

bool D3D11VAVideoDecoder::emitPicture(const std::shared_ptr<Picture>& picture) {
  if (picture == nullptr || picture->outputDone) {
    return true;
  }
  std::shared_ptr<AVPixelBuffer> output;
  if (mConvertOutput) {
    if (!convertPicture(picture, output)) {
      MINILOG_WARN("the D3D11VA decoder could not convert a frame to BGRA");
      return false;
    }
  } else {
    std::lock_guard<std::mutex> lock(mSlotMutex);
    if (picture->slot < 0 || picture->slot >= static_cast<int>(mDecodeSlots.size())) {
      return false;
    }
    auto buffer = std::make_shared<D3D11VAHardwarePixelBuffer>(this, mDecodeTexture,
            static_cast<uint32_t>(picture->slot), mWidth, mHeight, HardwarePixelBuffer::TextureFormat::kNv12,
            PixelFormat::kFormatNV12, picture->slot, false);
    buffer->timeUs(picture->timeUs);
    buffer->setColorInfo(mOutputColorInfo);
    if (!isAtLeastOutputStartTime(picture->timeUs)) {
      buffer->shouldBeSkipped = true;
    }
    ++mDecodeSlots[picture->slot].consumerHolds;
    output = std::move(buffer);
  }
  if (output == nullptr) {
    MINILOG_WARN("the D3D11VA decoder could not emit a decoded picture");
    return false;
  }

  picture->outputPending = false;
  picture->outputDone = true;
  mPendingFrames.push_back(output);

  if (!picture->shortTermReference && !picture->longTermReference) {
    releasePictureSlot(picture);
  }
  return true;
}

bool D3D11VAVideoDecoder::convertPicture(const std::shared_ptr<Picture>& picture,
        std::shared_ptr<AVPixelBuffer>& output) {
  if (!mVideoProcessor || mProcessorEnumerator == nullptr || picture->slot < 0 ||
          picture->slot >= static_cast<int>(mDecodeSlots.size())) {
    return false;
  }
  int slot = -1;
  {
    std::lock_guard<std::mutex> lock(mSlotMutex);
    for (int index = 0; index < static_cast<int>(mConvertedSlots.size()); ++index) {
      if (mConvertedSlots[index].consumerHolds == 0) {
        slot = index;
        ++mConvertedSlots[index].consumerHolds;
        break;
      }
    }
  }
  if (slot < 0) {
    MINILOG_WARN("the D3D11VA decoder has no free BGRA conversion surface");
    return false;
  }

  Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> inputView;
  D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDescription{};
  inputDescription.FourCC = 0;
  inputDescription.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
  inputDescription.Texture2D.MipSlice = 0;
  inputDescription.Texture2D.ArraySlice = static_cast<UINT>(picture->slot);
  HRESULT result = mVideoDevice->CreateVideoProcessorInputView(mDecodeTexture.Get(), mProcessorEnumerator.Get(),
          &inputDescription, &inputView);
  if (FAILED(result) || inputView == nullptr) {
    MINILOG_WARN("the D3D11VA decoder could not read the decoded texture");
    std::lock_guard<std::mutex> lock(mSlotMutex);
    --mConvertedSlots[slot].consumerHolds;
    return false;
  }

  const auto inputSpace = toInputColorSpace(mOutputColorInfo, static_cast<uint32_t>(mHeight));
  mVideoContext->VideoProcessorSetStreamColorSpace(mVideoProcessor.Get(), 0, &inputSpace);
  RECT source{ 0, 0, static_cast<LONG>(mWidth), static_cast<LONG>(mHeight) };
  mVideoContext->VideoProcessorSetStreamSourceRect(mVideoProcessor.Get(), 0, TRUE, &source);
  RECT destination{ 0, 0, static_cast<LONG>(mWidth), static_cast<LONG>(mHeight) };
  mVideoContext->VideoProcessorSetStreamDestRect(mVideoProcessor.Get(), 0, TRUE, &destination);
  mVideoContext->VideoProcessorSetStreamFrameFormat(mVideoProcessor.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);

  D3D11_VIDEO_PROCESSOR_STREAM stream{};
  stream.Enable = TRUE;
  stream.OutputIndex = 0;
  stream.InputFrameOrField = 0;
  stream.pInputSurface = inputView.Get();
  result = mVideoContext->VideoProcessorBlt(mVideoProcessor.Get(), mConvertedSlots[slot].view.Get(), 0, 1, &stream);
  if (FAILED(result) || !waitForGpu()) {
    MINILOG_WARN("the D3D11VA BGRA conversion failed: {}", WmfMediaUtils::hresultToString(result));
    std::lock_guard<std::mutex> lock(mSlotMutex);
    --mConvertedSlots[slot].consumerHolds;
    return false;
  }

  auto buffer = std::make_shared<D3D11VAHardwarePixelBuffer>(this, mConvertedSlots[slot].texture, 0, mWidth, mHeight,
          HardwarePixelBuffer::TextureFormat::kRgba, PixelFormat::kFormatBGRA, slot, true);
  buffer->timeUs(picture->timeUs);
  buffer->setColorInfo(mOutputColorInfo);
  if (!isAtLeastOutputStartTime(picture->timeUs)) {
    buffer->shouldBeSkipped = true;
  }
  output = std::move(buffer);
  return true;
}

int D3D11VAVideoDecoder::allocateDecodeSlot() {
  std::lock_guard<std::mutex> lock(mSlotMutex);
  for (int index = 0; index < static_cast<int>(mDecodeSlots.size()); ++index) {
    if (mDecodeSlots[index].isFree()) {
      return index;
    }
  }
  return -1;
}

int D3D11VAVideoDecoder::allocateConvertedSlot() {
  std::lock_guard<std::mutex> lock(mSlotMutex);
  for (int index = 0; index < static_cast<int>(mConvertedSlots.size()); ++index) {
    if (mConvertedSlots[index].consumerHolds == 0) {
      ++mConvertedSlots[index].consumerHolds;
      return index;
    }
  }
  return -1;
}

} // namespace coral

#endif // defined(WINDOWS) || defined(WIN32)
