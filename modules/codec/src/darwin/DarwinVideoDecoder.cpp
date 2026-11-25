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

#if defined(__APPLE__)

#include "DarwinVideoDecoder.h"

#include "DarwinHardwarePixelBuffer.h"
#include "DarwinMediaUtils.h"

#include <codec/HardwareVideo.h>

#include <media/Definitions.h>
#include <media/PixelBuffer.h>
#include <minilog/log.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace coral {

namespace {

/** The capacity of the input buffer when the format does not name a maximum input size. */
constexpr int DEFAULT_MAX_INPUT_SIZE = 2 * 1024 * 1024;

/** The number of decoded frames that are taken out of the session and held at most. */
constexpr size_t MAX_PENDING_FRAMES = 12;

/** The number of pending frames above which the input is paused, so that the queue cannot grow. */
constexpr size_t MAX_PENDING_FRAMES_FOR_INPUT = 8;

/**
 * The number of decoded frames the caller may hold at once.
 *
 * The session reorders its output against the input it holds, and a caller which is given more frames
 * than it can display drops them; feeding the decoder further while four are still out keeps the
 * reordering bounded without starving the pipeline.
 */
constexpr int MAX_OUTSTANDING_FRAMES = 4;

/** The frame rate a sample without a presentation time is spaced with when the track has none. */
constexpr int DEFAULT_FRAME_RATE = 30;

/**
 * Whether every NAL of the sample is described by a four byte length which ends exactly at its end.
 *
 * The conversion helper of the platform layer decides from the first bytes alone whether a sample is
 * Annex B, and the length prefix of a 256 to 511 byte NAL starts with the bytes of a three byte start
 * code as well (0x0000017C and 0x00 0x00 0x01 0x7C are the same bytes). A compressed frame of that
 * size is common, so the shape is checked against the whole buffer before anything is converted.
 */
bool isFourByteLengthPrefixed(const uint8_t* data, const size_t size) {
  if (data == nullptr || size < 5) {
    return false;
  }
  size_t offset = 0;
  while (offset + 4 <= size) {
    const auto length = (static_cast<uint32_t>(data[offset]) << 24) |
                        (static_cast<uint32_t>(data[offset + 1]) << 16) |
                        (static_cast<uint32_t>(data[offset + 2]) << 8) |
                        static_cast<uint32_t>(data[offset + 3]);
    offset += 4;
    if (length == 0 || offset + length > size) {
      return false;
    }
    offset += length;
  }
  return offset == size;
}

/** The attributes which make the session produce a surface a GPU can sample. */
CFMutableDictionaryRef createDestinationImageBufferAttributes(
    const HardwarePixelBuffer::TextureFormat hardwareFormat) {
  auto attributes = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  if (attributes == nullptr) {
    return nullptr;
  }

  const auto pixelFormat = static_cast<int32_t>(
    hardwareFormat == HardwarePixelBuffer::TextureFormat::kRgba
      ? kCVPixelFormatType_32BGRA
      : hardwareFormat == HardwarePixelBuffer::TextureFormat::kP010
        ? kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange
        : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
  auto formatNumber = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &pixelFormat);
  if (formatNumber != nullptr) {
    CFDictionarySetValue(attributes, kCVPixelBufferPixelFormatTypeKey, formatNumber);
    CFRelease(formatNumber);
  }

  // An empty IOSurface dictionary is what asks for a buffer which the GPU can map; without it the
  // session may allocate a buffer the consumer would have to copy.
  auto surfaceProperties = CFDictionaryCreate(kCFAllocatorDefault, nullptr, nullptr, 0,
    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  if (surfaceProperties != nullptr) {
    CFDictionarySetValue(attributes, kCVPixelBufferIOSurfacePropertiesKey, surfaceProperties);
    CFRelease(surfaceProperties);
  }
  CFDictionarySetValue(attributes, kCVPixelBufferMetalCompatibilityKey, kCFBooleanTrue);
  return attributes;
}

/** The specification which asks for a hardware decoder where the platform has one. */
CFMutableDictionaryRef createDecoderSpecification() {
  auto specification = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  if (specification == nullptr) {
    return nullptr;
  }
#if __has_builtin(__builtin_available)
  // The hardware preference keys are older than the SDKs which declare them as available on this
  // platform, hence the runtime check rather than an unconditional reference.
  if (__builtin_available(macOS 10.9, iOS 17.0, tvOS 17.0, *)) {
    CFDictionarySetValue(specification,
      kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder, kCFBooleanTrue);
  }
#else
  CFDictionarySetValue(specification,
    kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder, kCFBooleanTrue);
#endif
  return specification;
}

/** Logs whether the session really decodes in hardware, which the request above does not guarantee. */
void logHardwareAcceleration(VTDecompressionSessionRef session) {
  if (session == nullptr) {
    return;
  }
#if __has_builtin(__builtin_available)
  if (__builtin_available(macOS 10.9, iOS 17.0, tvOS 17.0, *)) {
#endif
    CFTypeRef usingHardware = nullptr;
    const auto status = VTSessionCopyProperty(session,
      kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder, kCFAllocatorDefault,
      &usingHardware);
    if (status != noErr || usingHardware == nullptr) {
      MINILOG_DEBUG("the video decoder does not report how it decodes");
      return;
    }
    const auto accelerated = CFGetTypeID(usingHardware) == CFBooleanGetTypeID() &&
                             CFBooleanGetValue(static_cast<CFBooleanRef>(usingHardware));
    MINILOG_DEBUG("the video decoder decodes {} hardware acceleration",
      accelerated ? "with" : "without");
    CFRelease(usingHardware);
#if __has_builtin(__builtin_available)
  }
#endif
}

/** Whether the pixel buffer holds the bi-planar 4:2:0 the session was asked for. */
bool isBiPlanar420(CVPixelBufferRef pixelBuffer) {
  const auto format = CVPixelBufferGetPixelFormatType(pixelBuffer);
  return format == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ||
         format == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange ||
         format == kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange ||
         format == kCVPixelFormatType_420YpCbCr10BiPlanarFullRange;
}

/** Whether the pixel buffer holds the packed BGRA the session was asked for. */
bool isBgra8(CVPixelBufferRef pixelBuffer) {
  return CVPixelBufferGetPixelFormatType(pixelBuffer) == kCVPixelFormatType_32BGRA;
}

bool sameString(CFTypeRef value, CFStringRef expected) {
  return value != nullptr && CFGetTypeID(value) == CFStringGetTypeID() &&
    CFStringCompare(static_cast<CFStringRef>(value), expected, 0) == kCFCompareEqualTo;
}

/**
 * Reads the colour description the codec put in the format description.
 *
 * <p>A container may omit its colour box even when the bitstream carries the same information in the
 * VUI. VideoToolbox exposes those VUI values as format-description extensions, so a decoder can
 * report HDR even when the extractor's container-level ColorInfo is empty.
 */
ColorInfo colorInfoFromFormatDescription(CMFormatDescriptionRef description) {
  ColorInfo color;
  if (description == nullptr) {
    return color;
  }
  const auto extensions = CMFormatDescriptionGetExtensions(description);
  if (extensions == nullptr) {
    return color;
  }
  const auto primaries = CFDictionaryGetValue(extensions,
    kCMFormatDescriptionExtension_ColorPrimaries);
  if (sameString(primaries, kCMFormatDescriptionColorPrimaries_ITU_R_2020)) {
    color.colorSpace = ColorInfo::SpaceType::kSpaceBT2020;
  } else if (sameString(primaries, kCMFormatDescriptionColorPrimaries_ITU_R_709_2)) {
    color.colorSpace = ColorInfo::SpaceType::kSpaceBT709;
  } else if (sameString(primaries, kCMFormatDescriptionColorPrimaries_EBU_3213) ||
             sameString(primaries, kCMFormatDescriptionColorPrimaries_SMPTE_C)) {
    color.colorSpace = ColorInfo::SpaceType::kSpaceBT601;
  }

  const auto transfer = CFDictionaryGetValue(extensions,
    kCMFormatDescriptionExtension_TransferFunction);
  if (sameString(transfer, kCMFormatDescriptionTransferFunction_SMPTE_ST_2084_PQ)) {
    color.colorTransfer = ColorInfo::TransferType::kTransferST2084;
  } else if (sameString(transfer, kCMFormatDescriptionTransferFunction_ITU_R_2100_HLG)) {
    color.colorTransfer = ColorInfo::TransferType::kTransferHLG;
  } else if (sameString(transfer, kCMFormatDescriptionTransferFunction_Linear)) {
    color.colorTransfer = ColorInfo::TransferType::kTransferLinear;
  } else if (sameString(transfer, kCMFormatDescriptionTransferFunction_sRGB)) {
    color.colorTransfer = ColorInfo::TransferType::kTransferSRGB;
  } else if (transfer != nullptr) {
    color.colorTransfer = ColorInfo::TransferType::kTransferSDR;
  }

  const auto matrix = CFDictionaryGetValue(extensions,
    kCMFormatDescriptionExtension_YCbCrMatrix);
  if (color.colorSpace == ColorInfo::SpaceType::kSpaceUnknown) {
    if (sameString(matrix, kCMFormatDescriptionYCbCrMatrix_ITU_R_2020)) {
      color.colorSpace = ColorInfo::SpaceType::kSpaceBT2020;
    } else if (sameString(matrix, kCMFormatDescriptionYCbCrMatrix_ITU_R_709_2)) {
      color.colorSpace = ColorInfo::SpaceType::kSpaceBT709;
    } else if (sameString(matrix, kCMFormatDescriptionYCbCrMatrix_ITU_R_601_4)) {
      color.colorSpace = ColorInfo::SpaceType::kSpaceBT601;
    }
  }

  const auto fullRange = CFDictionaryGetValue(extensions,
    kCMFormatDescriptionExtension_FullRangeVideo);
  if (fullRange != nullptr && CFGetTypeID(fullRange) == CFBooleanGetTypeID()) {
    color.colorRange = CFBooleanGetValue(static_cast<CFBooleanRef>(fullRange))
      ? ColorInfo::RangeType::kRangeFull
      : ColorInfo::RangeType::kRangeLimited;
  }
  return color;
}

ColorInfo mergeColorInfo(const ColorInfo& container, const ColorInfo& codec) {
  ColorInfo merged = container;
  if (merged.colorSpace == ColorInfo::SpaceType::kSpaceUnknown) {
    merged.colorSpace = codec.colorSpace;
  }
  if (merged.colorTransfer == ColorInfo::TransferType::kTransferUnknown) {
    merged.colorTransfer = codec.colorTransfer;
  }
  if (merged.colorRange == ColorInfo::RangeType::kRangeUnknown) {
    merged.colorRange = codec.colorRange;
  }
  return merged;
}

} // namespace

DarwinVideoDecoder::DarwinVideoDecoder() = default;

DarwinVideoDecoder::~DarwinVideoDecoder() {
  release();
}

bool DarwinVideoDecoder::configure(const std::shared_ptr<MediaFormat>& format,
  const std::shared_ptr<VideoSurface>& surface) {
  release();

  if (format == nullptr) {
    MINILOG_ERROR("failed to configure the video decoder: the format is null");
    return false;
  }
  if (!format->isVideo() || format->getMimeType().empty()) {
    MINILOG_ERROR("failed to configure the video decoder: invalid mime type: {}",
      format->getMimeType());
    return false;
  }

  if (surface != nullptr && surface->isValid()) {
    if (std::dynamic_pointer_cast<HardwareVideoSurface>(surface) != nullptr) {
      mHardwareSurface = surface;
    } else {
      // A window surface is Android's way of asking the codec to present the frames itself; the
      // Darwin decoder has no such output, and a caller which offered one still gets the frames.
      MINILOG_WARN("the video surface is not supported by the Darwin video decoder, "
                   "the frames are copied to system memory");
    }
  }

  mFormatDescription = darwin::createFormatDescription(format);
  if (mFormatDescription == nullptr) {
    MINILOG_ERROR("failed to configure the video decoder: "
                  "the format description could not be created for {}", format->toString());
    release();
    return false;
  }

  // The container's colour box wins; the codec VUI fills whatever the container left unknown. The
  // latter is common for an HDR10 stream in an SDR container.
  const ColorInfo effectiveColor = mergeColorInfo(
    format->getColorInfo(), colorInfoFromFormatDescription(mFormatDescription));

  // The format description carries the coded size of the parameter sets, which is the size a stream
  // without a container header is described by.
  const auto dimensions = CMVideoFormatDescriptionGetDimensions(mFormatDescription);
  const auto width = format->getWidth() > 0 ? format->getWidth()
                                            : static_cast<int>(dimensions.width);
  const auto height = format->getHeight() > 0 ? format->getHeight()
                                              : static_cast<int>(dimensions.height);
  if (width <= 0 || height <= 0) {
    MINILOG_ERROR("failed to configure the video decoder: the frame size is unknown");
    release();
    return false;
  }

  // A hardware surface can honour the format's process format: BGRA lets a consumer that samples one
  // RGBA texture stay zero-copy, while the default NV12 is what a YUV consumer wants. A format
  // without a process format, or a CPU decode, keeps the platform default.
  mHardwareTextureFormat = HardwarePixelBuffer::TextureFormat::kNv12;
  mHardwarePixelFormat = PixelFormat::kFormatNV12;
  if (mHardwareSurface != nullptr) {
    const auto requested = format->getProcessFormat();
    const auto color = effectiveColor;
    const bool hdr = color.colorSpace == ColorInfo::SpaceType::kSpaceBT2020 &&
      (color.colorTransfer == ColorInfo::TransferType::kTransferST2084 ||
       color.colorTransfer == ColorInfo::TransferType::kTransferHLG);
    if (requested == PixelFormat::kFormatP010 || (hdr && requested != PixelFormat::kFormatBGRA)) {
      // 10-bit YUV is the decoder layout that preserves HDR10/HLG precision. A BGRA request still
      // wins because a consumer that explicitly asked for packed RGB wants one texture.
      mHardwareTextureFormat = HardwarePixelBuffer::TextureFormat::kP010;
      mHardwarePixelFormat = PixelFormat::kFormatP010;
    } else if (requested == PixelFormat::kFormatBGRA) {
      mHardwareTextureFormat = HardwarePixelBuffer::TextureFormat::kRgba;
      mHardwarePixelFormat = PixelFormat::kFormatBGRA;
    } else if (requested != PixelFormat::kFormatInvalid &&
               requested != PixelFormat::kFormatNV12) {
      MINILOG_DEBUG("the Darwin decoder ignores the requested process format {}; "
                    "hardware frames stay NV12", static_cast<int>(requested));
    }
  } else if (format->getProcessFormat() == PixelFormat::kFormatBGRA) {
    MINILOG_DEBUG("the Darwin decoder keeps CPU frames in NV12; the process format needs a "
                  "hardware surface");
  }

  {
    std::lock_guard<std::mutex> lock(mFormatMutex);
    mWidth = width;
    mHeight = height;
    mFrameRate = format->getFrameRate() > 0 ? format->getFrameRate() : 0;
    mOutputColorInfo = effectiveColor;
    rebuildOutputFormatLocked();
  }

  const auto decoderSpecification = createDecoderSpecification();
  VTDecompressionOutputCallbackRecord callbackRecord {};
  callbackRecord.decompressionOutputCallback = &DarwinVideoDecoder::decompressionOutputCallback;
  callbackRecord.decompressionOutputRefCon = this;

  // The attributes are copied by the session, the specification is only read while it is created.
  const auto createSession = [&](const HardwarePixelBuffer::TextureFormat hardwareFormat,
                                 OSStatus* outStatus) {
    const auto attributes = createDestinationImageBufferAttributes(hardwareFormat);
    if (attributes == nullptr) {
      *outStatus = paramErr;
      return;
    }
    mSession = nullptr;
    *outStatus = VTDecompressionSessionCreate(kCFAllocatorDefault, mFormatDescription,
      decoderSpecification, attributes, &callbackRecord, &mSession);
    CFRelease(attributes);
  };

  OSStatus status = noErr;
  createSession(mHardwareTextureFormat, &status);
  if ((status != noErr || mSession == nullptr) &&
      (mHardwareTextureFormat == HardwarePixelBuffer::TextureFormat::kRgba ||
       mHardwareTextureFormat == HardwarePixelBuffer::TextureFormat::kP010)) {
    // Some VideoToolbox decoders cannot deliver 32BGRA. Keep the session usable by falling back to
    // the platform layout; the consumer can then convert the frame itself.
    MINILOG_WARN("VideoToolbox could not create the requested session ({}); falling back to NV12",
                 static_cast<int>(status));
    mHardwareTextureFormat = HardwarePixelBuffer::TextureFormat::kNv12;
    mHardwarePixelFormat = PixelFormat::kFormatNV12;
    {
      std::lock_guard<std::mutex> lock(mFormatMutex);
      rebuildOutputFormatLocked();
    }
    createSession(mHardwareTextureFormat, &status);
  }
  if (decoderSpecification != nullptr) {
    CFRelease(decoderSpecification);
  }
  if (status != noErr || mSession == nullptr) {
    const auto failed = darwin::logStatus(status,
      "the video decompression session could not be created", format->toString());
    mSession = nullptr;
    release();
    return failed;
  }

  auto capacity = format->getMaxInputSize();
  if (capacity <= 0) {
    capacity = DEFAULT_MAX_INPUT_SIZE;
  }
  mInputBuffer = std::make_shared<AVPacketBuffer>(MediaType::kMediaTypeVideo);
  mInputBuffer->ensureSpaceForWrite(static_cast<size_t>(capacity));

  mInputEndOfStream = false;
  mOutputEndOfStream = false;
  mDrainRequested = false;
  mInputDequeued = false;
  mFrameIndex = 0;
  mOutstandingOutputCount = 0;
  mHardwareAccelerationLogged = false;
  // A consumer which seeks sets the start time again; a fresh stream starts at its beginning.
  mOutputStartTimeUs = Definitions::kTimeUnset;
  mConfigured = true;
  MINILOG_DEBUG("the video decoder is configured with {}", format->toString());
  return true;
}

void DarwinVideoDecoder::rebuildOutputFormatLocked() {
  auto format = MediaFormat::CreateVideoFormat(MimeTypes::VIDEO_RAW, mWidth, mHeight);
  format->setPixelFormat(mHardwarePixelFormat);
  format->setColorInfo(mOutputColorInfo);
  if (mFrameRate > 0) {
    format->setFrameRate(mFrameRate);
  }
  mOutputFormat = std::move(format);
  MINILOG_DEBUG("video decoder output format: {}", mOutputFormat->toString());
}

void DarwinVideoDecoder::start() {
  drainAsynchronousFrames();
  resetStreamState();
}

void DarwinVideoDecoder::stop() {
  drainAsynchronousFrames();
  resetStreamState();
}

void DarwinVideoDecoder::flush() {
  drainAsynchronousFrames();
  resetStreamState();
}

void DarwinVideoDecoder::release() {
  // The session must not deliver a frame while it is torn down, and the callback reads the state of
  // this decoder, so the outstanding frames are finished before anything is released.
  drainAsynchronousFrames();
  if (mSession != nullptr) {
    VTDecompressionSessionInvalidate(mSession);
    CFRelease(mSession);
    mSession = nullptr;
  }
  if (mFormatDescription != nullptr) {
    CFRelease(mFormatDescription);
    mFormatDescription = nullptr;
  }
  discardPendingFrames();
  mInputBuffer.reset();
  {
    std::lock_guard<std::mutex> lock(mFormatMutex);
    mOutputFormat.reset();
    mWidth = 0;
    mHeight = 0;
    mFrameRate = 0;
    mOutputColorInfo = ColorInfo();
  }
  mHardwareSurface.reset();
  mHardwareTextureFormat = HardwarePixelBuffer::TextureFormat::kNv12;
  mHardwarePixelFormat = PixelFormat::kFormatNV12;
  mInputDequeued = false;
  mInputEndOfStream = true;
  mOutputEndOfStream = true;
  mDrainRequested = false;
  mFrameIndex = 0;
  mOutstandingOutputCount = 0;
  mHardwareAccelerationLogged = false;
  mConfigured = false;
}

void DarwinVideoDecoder::resetStreamState() {
  discardPendingFrames();
  mInputDequeued = false;
  mInputEndOfStream = false;
  mOutputEndOfStream = false;
  mDrainRequested = false;
  mFrameIndex = 0;
  mOutstandingOutputCount = 0;
}

void DarwinVideoDecoder::drainAsynchronousFrames() {
  if (mSession == nullptr) {
    return;
  }
  // The wait also finishes the frames the session delays, so a decoder which holds frames for the
  // reordering emits them here instead of leaving them in the stream which is about to be dropped.
  const auto status = VTDecompressionSessionWaitForAsynchronousFrames(mSession);
  if (status != noErr) {
    darwin::logStatus(status, "failed to wait for the decoded video frames");
  }
  // Every callback has run, so no further frame can precede the held ones; hand them out in order.
  flushReorderFrames();
}

void DarwinVideoDecoder::flushReorderFrames() {
  std::vector<std::shared_ptr<AVPixelBuffer>> ordered;
  {
    std::lock_guard<std::mutex> lock(mFrameMutex);
    ordered.reserve(mReorderFrames.size());
    for (auto& entry : mReorderFrames) {
      ordered.push_back(std::move(entry.second));
    }
    mReorderFrames.clear();
  }
  for (auto& frame : ordered) {
    pushPendingFrame(std::move(frame));
  }
}

std::shared_ptr<AVPacketBuffer> DarwinVideoDecoder::dequeueInputBuffer() {
  if (!mConfigured || mSession == nullptr || mInputBuffer == nullptr) {
    return nullptr;
  }
  if (mInputDequeued || mInputEndOfStream) {
    return nullptr;
  }
  if (mOutstandingOutputCount.load() >= MAX_OUTSTANDING_FRAMES) {
    return nullptr;
  }
  {
    std::lock_guard<std::mutex> lock(mFrameMutex);
    if (mPendingFrames.size() + mReorderFrames.size() >= MAX_PENDING_FRAMES_FOR_INPUT) {
      return nullptr;
    }
  }
  mInputDequeued = true;
  // The buffer held the payload and the flags of the sample it was given for before, which belong to
  // that sample and not to the one which takes its place.
  mInputBuffer->clear();
  return mInputBuffer;
}

void DarwinVideoDecoder::queueInputBuffer(std::shared_ptr<AVPacketBuffer> inputBuffer) {
  if (!inputBuffer || !mConfigured || mSession == nullptr) {
    return;
  }
  mInputDequeued = false;

  if (inputBuffer->isEndOfStream()) {
    mInputEndOfStream = true;
    return;
  }

  const auto* sampleData = inputBuffer->array();
  auto sampleSize = inputBuffer->size();
  if (sampleData == nullptr || sampleSize == 0) {
    return;
  }

  // The samples of the framework's extractors are Annex B or AVCC; the session holds AVCC with four
  // byte lengths, which is also what the format description was built with. A sample which already
  // has that shape is passed through untouched, so that a small NAL is not mistaken for the start
  // code of an Annex B sample; every other form is converted.
  std::vector<uint8_t> converted;
  if (!isFourByteLengthPrefixed(sampleData, sampleSize)) {
    converted = darwin::toLengthPrefixed(sampleData, sampleSize, 4);
    if (converted.empty()) {
      MINILOG_ERROR("the video decoder rejected a malformed sample of {} bytes", sampleSize);
      return;
    }
    sampleData = converted.data();
    sampleSize = converted.size();
  }

  // A CMBlockBuffer frees the block it is given with the allocator of that block, so the sample is
  // copied into memory the block allocator may own.
  auto* bytes = static_cast<uint8_t*>(malloc(sampleSize));
  if (bytes == nullptr) {
    MINILOG_ERROR("failed to allocate the {} bytes of a video sample", sampleSize);
    return;
  }
  memcpy(bytes, sampleData, sampleSize);

  CMBlockBufferRef blockBuffer = nullptr;
  auto status = CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, bytes, sampleSize,
    kCFAllocatorMalloc, nullptr, 0, sampleSize, 0, &blockBuffer);
  if (status != noErr || blockBuffer == nullptr) {
    // The memory block is only taken over by a block buffer which was created.
    free(bytes);
    darwin::logStatus(status, "failed to create the block buffer of a video sample");
    return;
  }

  auto timeUs = inputBuffer->timeUs();
  if (timeUs == Definitions::kTimeUnset) {
    // A sample without a time still has to reach the session with one; the frame counter spaces them
    // with the frame rate of the track, which the caller can only tell apart from real times by its
    // own bookkeeping, and the pipeline uses the frame times when it has them.
    const auto rate = mFrameRate > 0 ? mFrameRate : DEFAULT_FRAME_RATE;
    timeUs = mFrameIndex * Definitions::kMicrosPerSecond / rate;
  }
  ++mFrameIndex;

  CMSampleTimingInfo timing {};
  timing.duration = mFrameRate > 0 ? CMTimeMake(1, mFrameRate) : kCMTimeInvalid;
  timing.presentationTimeStamp = darwin::toCMTime(timeUs);
  timing.decodeTimeStamp = kCMTimeInvalid;

  CMSampleBufferRef sampleBuffer = nullptr;
  status = CMSampleBufferCreateReady(kCFAllocatorDefault, blockBuffer, mFormatDescription, 1, 1,
    &timing, 1, &sampleSize, &sampleBuffer);
  CFRelease(blockBuffer);
  if (status != noErr || sampleBuffer == nullptr) {
    darwin::logStatus(status, "failed to create the sample buffer of a video sample");
    return;
  }

  if (!inputBuffer->isKeyFrame()) {
    // An attachment which is absent means sync in CoreMedia, so only a frame which is not a sync
    // frame needs the key.
    const auto attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, true);
    if (attachments != nullptr && CFArrayGetCount(attachments) > 0) {
      const auto dictionary = static_cast<CFMutableDictionaryRef>(
        const_cast<void*>(CFArrayGetValueAtIndex(attachments, 0)));
      if (dictionary != nullptr) {
        CFDictionarySetValue(dictionary, kCMSampleAttachmentKey_NotSync, kCFBooleanTrue);
      }
    }
  }

  VTDecodeInfoFlags infoFlags = 0;
  status = VTDecompressionSessionDecodeFrame(mSession, sampleBuffer,
    kVTDecodeFrame_EnableAsynchronousDecompression, nullptr, &infoFlags);
  if (status != noErr) {
    darwin::logStatus(status, "failed to decode a video sample");
  }
  // The session retains the sample buffer for as long as it decodes it.
  CFRelease(sampleBuffer);
}

void DarwinVideoDecoder::decompressionOutputCallback(void* decompressionOutputRefCon,
  void* sourceFrameRefCon, const OSStatus status, const VTDecodeInfoFlags infoFlags,
  CVImageBufferRef imageBuffer, const CMTime presentationTimeStamp,
  const CMTime presentationDuration) {
  static_cast<void>(sourceFrameRefCon);
  static_cast<void>(infoFlags);
  static_cast<void>(presentationDuration);
  auto* decoder = static_cast<DarwinVideoDecoder*>(decompressionOutputRefCon);
  if (decoder == nullptr) {
    return;
  }
  try {
    decoder->onFrameDecoded(status, imageBuffer, presentationTimeStamp);
  } catch (...) {
    // The callback runs on a VideoToolbox thread; an exception which escapes it would terminate the
    // process rather than fail a frame.
    MINILOG_ERROR("the video decoder failed to handle a decoded frame");
  }
}

void DarwinVideoDecoder::onFrameDecoded(const OSStatus status, CVImageBufferRef imageBuffer,
  const CMTime presentationTimeStamp) {
  if (status != noErr) {
    if (status == kVTVideoDecoderBadDataErr) {
      MINILOG_WARN("the video decoder skipped a frame of bad data");
    } else {
      darwin::logStatus(status, "failed to decode a video frame");
    }
    return;
  }
  if (imageBuffer == nullptr || CFGetTypeID(imageBuffer) != CVPixelBufferGetTypeID()) {
    MINILOG_WARN("the video decoder produced a frame which is not a pixel buffer");
    return;
  }
  const auto pixelBuffer = static_cast<CVPixelBufferRef>(imageBuffer);
  const auto timeUs = darwin::toTimeUs(presentationTimeStamp);

  auto frame = mHardwareSurface != nullptr ? createHardwareFrameBuffer(pixelBuffer, timeUs)
                                           : createCpuFrameBuffer(pixelBuffer, timeUs);
  if (frame == nullptr) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(mFormatMutex);
    const auto width = frame->width();
    const auto height = frame->height();
    if (width != mWidth || height != mHeight) {
      // A stream which changes its size mid-way is decoded at the new size; the consumers crop and
      // scale with the format they are given, so the format follows the frame.
      MINILOG_DEBUG("the video decoder output size changed from {}x{} to {}x{}",
        mWidth, mHeight, width, height);
      mWidth = width;
      mHeight = height;
      rebuildOutputFormatLocked();
    }
    frame->setColorInfo(mOutputColorInfo);
  }

  const auto outputStartTimeUs = mOutputStartTimeUs.load();
  if (outputStartTimeUs != Definitions::kTimeUnset && timeUs != Definitions::kTimeUnset &&
      timeUs < outputStartTimeUs) {
    frame->shouldBeSkipped = true;
  }
  // A frame without a presentation time cannot be placed relative to the others; hand it out in the
  // order the session produced it rather than leaving it in the reorder buffer forever.
  if (timeUs == Definitions::kTimeUnset) {
    pushPendingFrame(std::move(frame));
    return;
  }

  // VideoToolbox calls back in decode order. A frame is held until a later frame with a larger
  // presentation time arrives; then every held frame can be handed out sorted by presentation time.
  // For a stream without reordering this only holds the current frame until the next one arrives,
  // and for a B-frame run it emits the whole run in display order as soon as the next P frame is
  // decoded, so the caller never sees the decode order.
  {
    std::lock_guard<std::mutex> lock(mFrameMutex);
    if (!mReorderFrames.empty()) {
      const auto maxPresentationTimeUs = mReorderFrames.rbegin()->first;
      if (timeUs > maxPresentationTimeUs) {
        for (auto& entry : mReorderFrames) {
          if (mPendingFrames.size() >= MAX_PENDING_FRAMES) {
            mPendingFrames.pop_front();
          }
          mPendingFrames.push_back(std::move(entry.second));
        }
        mReorderFrames.clear();
      }
    }
    mReorderFrames.emplace(timeUs, std::move(frame));
    // A stream whose timestamps never advance cannot flush the buffer; bound it like the pending
    // queue and let the caller consume the oldest frames.
    while (mReorderFrames.size() > MAX_PENDING_FRAMES) {
      auto oldest = mReorderFrames.begin();
      if (mPendingFrames.size() >= MAX_PENDING_FRAMES) {
        mPendingFrames.pop_front();
      }
      mPendingFrames.push_back(std::move(oldest->second));
      mReorderFrames.erase(oldest);
    }
  }
}

std::shared_ptr<AVPixelBuffer> DarwinVideoDecoder::createHardwareFrameBuffer(
  CVPixelBufferRef pixelBuffer, const int64_t timeUs) {
  const auto width = static_cast<int>(CVPixelBufferGetWidth(pixelBuffer));
  const auto height = static_cast<int>(CVPixelBufferGetHeight(pixelBuffer));
  if (width <= 0 || height <= 0) {
    return nullptr;
  }
  const bool matchesRequestedLayout =
    mHardwareTextureFormat == HardwarePixelBuffer::TextureFormat::kRgba
      ? isBgra8(pixelBuffer)
      : isBiPlanar420(pixelBuffer);
  if (!matchesRequestedLayout) {
    // The buffer would be handed out with a format it does not hold, which a consumer would sample
    // as garbage rather than fail on.
    MINILOG_ERROR("the decoded frame does not have the {} layout the decoder asked for, "
                  "the pixel format is {}",
      mHardwareTextureFormat == HardwarePixelBuffer::TextureFormat::kRgba ? "BGRA" :
        mHardwareTextureFormat == HardwarePixelBuffer::TextureFormat::kP010 ? "P010" : "NV12",
      static_cast<unsigned>(CVPixelBufferGetPixelFormatType(pixelBuffer)));
    return nullptr;
  }
  auto frame = std::make_shared<DarwinHardwarePixelBuffer>(this, pixelBuffer, width, height,
    mHardwareTextureFormat);
  frame->timeUs(timeUs);
  return frame;
}

std::shared_ptr<AVPixelBuffer> DarwinVideoDecoder::createCpuFrameBuffer(
  CVPixelBufferRef pixelBuffer, const int64_t timeUs) {
  if (!isBiPlanar420(pixelBuffer) || CVPixelBufferGetPlaneCount(pixelBuffer) < 2) {
    MINILOG_ERROR("the decoded frame is not the bi-planar 4:2:0 the decoder asked for, "
                  "the pixel format is {}", static_cast<unsigned>(
                    CVPixelBufferGetPixelFormatType(pixelBuffer)));
    return nullptr;
  }
  const auto width = static_cast<int>(CVPixelBufferGetWidth(pixelBuffer));
  const auto height = static_cast<int>(CVPixelBufferGetHeight(pixelBuffer));
  if (width <= 0 || height <= 0) {
    return nullptr;
  }
  if (CVPixelBufferLockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) {
    darwin::logFailure("failed to lock a decoded frame");
    return nullptr;
  }

  auto frame = std::make_shared<NV12PixelBuffer>(this, width, height);
  const auto* luma = static_cast<const uint8_t*>(
    CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 0));
  const auto* chroma = static_cast<const uint8_t*>(
    CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 1));
  const auto lumaStride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 0);
  const auto chromaStride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 1);
  const auto chromaRows = (height + 1) / 2;
  // The plane may be wider than the picture -- the decoder lines its rows up -- and the frame's
  // extent is what the consumer uses.
  const auto lumaBytes = std::min(static_cast<size_t>(width), lumaStride);
  const auto chromaBytes = std::min(static_cast<size_t>(2 * ((width + 1) / 2)), chromaStride);
  if (luma != nullptr) {
    for (int row = 0; row < height; ++row) {
      memcpy(frame->mutableDataY() + static_cast<ptrdiff_t>(row) * frame->strideY(),
        luma + static_cast<ptrdiff_t>(row) * lumaStride, lumaBytes);
    }
  }
  if (chroma != nullptr) {
    for (int row = 0; row < chromaRows; ++row) {
      memcpy(frame->mutableDataUV() + static_cast<ptrdiff_t>(row) * frame->strideUV(),
        chroma + static_cast<ptrdiff_t>(row) * chromaStride, chromaBytes);
    }
  }
  CVPixelBufferUnlockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);
  frame->timeUs(timeUs);
  return frame;
}

std::shared_ptr<AVPixelBuffer> DarwinVideoDecoder::takePendingFrame() {
  std::lock_guard<std::mutex> lock(mFrameMutex);
  if (mPendingFrames.empty()) {
    return nullptr;
  }
  auto frame = std::move(mPendingFrames.front());
  mPendingFrames.pop_front();
  ++mOutstandingOutputCount;
  return frame;
}

void DarwinVideoDecoder::pushPendingFrame(std::shared_ptr<AVPixelBuffer> frame) {
  std::lock_guard<std::mutex> lock(mFrameMutex);
  if (mPendingFrames.size() >= MAX_PENDING_FRAMES) {
    // The caller is behind by more than the queue it consumes; the oldest frame is dropped so that
    // the decoder does not stall on a queue which only the caller could drain.
    mPendingFrames.pop_front();
  }
  mPendingFrames.push_back(std::move(frame));
}

void DarwinVideoDecoder::discardPendingFrames() {
  std::deque<std::shared_ptr<AVPixelBuffer>> frames;
  {
    std::lock_guard<std::mutex> lock(mFrameMutex);
    frames.swap(mPendingFrames);
    mReorderFrames.clear();
  }
  // The frames are destroyed outside the lock: a hardware frame releases its pixel buffer with
  // itself, which must not happen while the decoder thread is waiting for the queue.
  frames.clear();
}

std::shared_ptr<AVPixelBuffer> DarwinVideoDecoder::createEndOfStreamBuffer() {
  auto buffer = AVPixelBuffer::newFlagsOnlyInstance();
  buffer->setFlags(AVBufferFlags::kEndOfStream);
  return buffer;
}

std::shared_ptr<AVPixelBuffer> DarwinVideoDecoder::dequeueOutputBuffer() {
  if (!mConfigured || mSession == nullptr) {
    return nullptr;
  }
  if (auto frame = takePendingFrame(); frame != nullptr) {
    if (!mHardwareAccelerationLogged) {
      mHardwareAccelerationLogged = true;
      // The session reports the decoder it uses only once it decodes, which is why the value is read
      // with the first frame rather than when the session was created.
      logHardwareAcceleration(mSession);
    }
    return frame;
  }
  if (!mInputEndOfStream) {
    return nullptr;
  }
  if (!mDrainRequested) {
    // The wait is what makes the end of the stream final: every output callback has run by the time
    // it returns, so the frames of the stream are all in the queue or were dropped by it.
    mDrainRequested = true;
    drainAsynchronousFrames();
    return takePendingFrame();
  }
  if (mOutstandingOutputCount.load() != 0) {
    // The caller still renders a frame of the stream which ended, and the end of the stream must not
    // overtake it.
    return nullptr;
  }
  if (mOutputEndOfStream) {
    return nullptr;
  }
  mOutputEndOfStream = true;
  return createEndOfStreamBuffer();
}

void DarwinVideoDecoder::releaseOutputBuffer(const std::shared_ptr<AVPixelBuffer>& outputBuffer,
  const bool render) {
  static_cast<void>(render);
  if (outputBuffer) {
    // The buffer returns to this decoder through recycleBuffer, which releases its outstanding slot.
    outputBuffer->release();
  }
}

void DarwinVideoDecoder::recycleBuffer(std::shared_ptr<AVPixelBuffer> frameBuffer) {
  if (!frameBuffer) {
    return;
  }
  auto outstanding = mOutstandingOutputCount.load(std::memory_order_relaxed);
  while (outstanding > 0 &&
         !mOutstandingOutputCount.compare_exchange_weak(outstanding, outstanding - 1,
           std::memory_order_relaxed)) {
  }
  // Nothing else has to happen: a hardware frame owns its pixel buffer and hands it back by being
  // destroyed, and the CPU frames are not pooled. A pool would have to be cleared before a buffer was
  // used again, because the flags and the timestamp of the frame it held before would otherwise be
  // handed out with the frame which takes its place.
}

void DarwinVideoDecoder::signalEndOfInputStream() {
  mInputEndOfStream = true;
}

void DarwinVideoDecoder::setOutputStartTimeUs(const int64_t outputStartTimeUs) {
  mOutputStartTimeUs = outputStartTimeUs;
}

} // namespace coral

#endif // defined(__APPLE__)
