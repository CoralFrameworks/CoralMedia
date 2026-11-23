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

#include "DarwinVideoEncoder.h"

#include "DarwinHardwarePixelBuffer.h"
#include "DarwinMediaUtils.h"

#include <codec/HardwareVideo.h>

#include <media/Definitions.h>
#include <media/PixelBuffer.h>
#include <media/Utilities.h>
#include <minilog/log.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace coral {

namespace {

/**
 * Whether every NAL of the sample is described by a four byte length which ends exactly at its end.
 *
 * The conversion helper of the platform layer decides from the first bytes alone whether a sample is
 * Annex B, and the length prefix of a 256 to 511 byte NAL starts with the bytes of a three byte start
 * code as well (0x0000017C and 0x00 0x00 0x01 0x7C are the same bytes). A compressor emits such
 * packets for small frames, so the shape is checked against the whole buffer instead.
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

/** The attributes which make a pixel buffer the compressor reads without a conversion. */
CFMutableDictionaryRef createPixelBufferAttributes() {
  auto attributes = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  if (attributes == nullptr) {
    return nullptr;
  }

  const auto pixelFormat = static_cast<int32_t>(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
  auto formatNumber = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &pixelFormat);
  if (formatNumber != nullptr) {
    CFDictionarySetValue(attributes, kCVPixelBufferPixelFormatTypeKey, formatNumber);
    CFRelease(formatNumber);
  }

  // An empty IOSurface dictionary asks for a buffer the compressor can read from the GPU; without it
  // the session copies the frame before encoding it.
  auto surfaceProperties = CFDictionaryCreate(kCFAllocatorDefault, nullptr, nullptr, 0,
    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  if (surfaceProperties != nullptr) {
    CFDictionarySetValue(attributes, kCVPixelBufferIOSurfacePropertiesKey, surfaceProperties);
    CFRelease(surfaceProperties);
  }
  CFDictionarySetValue(attributes, kCVPixelBufferMetalCompatibilityKey, kCFBooleanTrue);
  return attributes;
}

void setSessionProperty(VTCompressionSessionRef session, CFStringRef key, CFTypeRef value,
  const char* name) {
  const auto status = VTSessionSetProperty(session, key, value);
  if (status != noErr) {
    MINILOG_WARN("the video encoder does not take the {} property: {}", name,
      darwin::osStatusToString(status));
  }
}

void setSessionIntProperty(VTCompressionSessionRef session, CFStringRef key, const int value,
  const char* name) {
  const auto number = static_cast<int32_t>(value);
  auto cfNumber = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &number);
  if (cfNumber == nullptr) {
    return;
  }
  setSessionProperty(session, key, cfNumber, name);
  CFRelease(cfNumber);
}

void setSessionFloatProperty(VTCompressionSessionRef session, CFStringRef key, const float value,
  const char* name) {
  auto cfNumber = CFNumberCreate(kCFAllocatorDefault, kCFNumberFloat32Type, &value);
  if (cfNumber == nullptr) {
    return;
  }
  setSessionProperty(session, key, cfNumber, name);
  CFRelease(cfNumber);
}

/**
 * Stores the parameter sets of the encoded stream in the coral format.
 *
 * The H.264 parameter sets are stored the way the built-in extractor stores them -- one parameter set
 * per entry with a one byte length -- so that a format from this encoder can be read back by every
 * decoder of the framework. HEVC is stored as the `hvcC` record the extractor produces.
 */
void applyCodecSpecificData(MediaFormat& format, const VideoCodecType codecType,
  CMFormatDescriptionRef description) {
  if (description == nullptr) {
    return;
  }
  if (codecType == VideoCodecType::kVideoCodecTypeH264) {
    size_t count = 0;
    if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(description, 0, nullptr, nullptr,
        &count, nullptr) != noErr) {
      MINILOG_WARN("the encoded frames do not describe their h264 parameter sets");
      return;
    }
    for (size_t index = 0; index < count; ++index) {
      const uint8_t* parameterSet = nullptr;
      size_t parameterSetSize = 0;
      if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(description, index, &parameterSet,
          &parameterSetSize, nullptr, nullptr) != noErr) {
        break;
      }
      if (parameterSet == nullptr || parameterSetSize == 0 || parameterSetSize > 255) {
        MINILOG_WARN("the h264 parameter set {} does not fit the codec specific data convention",
          index);
        continue;
      }
      std::vector<uint8_t> data;
      data.reserve(parameterSetSize + 1);
      data.push_back(static_cast<uint8_t>(parameterSetSize));
      data.insert(data.end(), parameterSet, parameterSet + parameterSetSize);
      format.setCodecSpecificData(static_cast<int>(index), data);
    }
    if (count > 3) {
      MINILOG_WARN("the encoded stream has {} h264 parameter sets, the format holds three", count);
    }
    return;
  }

  if (codecType == VideoCodecType::kVideoCodecTypeHevc) {
    // The session describes its stream as the sample description atom of the format description,
    // which is the hvcC record the extractor stores as well.
    const auto atoms = CMFormatDescriptionGetExtension(description,
      kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms);
    if (atoms != nullptr && CFGetTypeID(atoms) == CFDictionaryGetTypeID()) {
      const auto record = static_cast<CFDataRef>(CFDictionaryGetValue(
        static_cast<CFDictionaryRef>(atoms), CFSTR("hvcC")));
      if (record != nullptr && CFGetTypeID(record) == CFDataGetTypeID()) {
        const auto* bytes = CFDataGetBytePtr(record);
        const auto length = CFDataGetLength(record);
        if (bytes != nullptr && length > 0) {
          format.setCodecSpecificData(0, bytes, static_cast<size_t>(length));
          return;
        }
      }
    }

    // The record is missing on some sessions; the parameter sets alone describe the same stream.
    std::vector<std::vector<uint8_t>> parameterSets;
    size_t count = 0;
    if (CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(description, 0, nullptr, nullptr, &count,
        nullptr) == noErr) {
      for (size_t index = 0; index < count; ++index) {
        const uint8_t* parameterSet = nullptr;
        size_t parameterSetSize = 0;
        if (CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(description, index, &parameterSet,
            &parameterSetSize, nullptr, nullptr) != noErr) {
          break;
        }
        if (parameterSet != nullptr && parameterSetSize > 0) {
          parameterSets.emplace_back(parameterSet, parameterSet + parameterSetSize);
        }
      }
    }
    const auto record = darwin::hvcCFromParameterSets(parameterSets);
    if (record.empty()) {
      MINILOG_WARN("the encoded frames do not describe their hevc parameter sets");
      return;
    }
    format.setCodecSpecificData(0, record);
  }
}

} // namespace

DarwinVideoEncoder::DarwinVideoEncoder(const VideoCodecType codecType)
  : mCodecType(codecType) {
}

DarwinVideoEncoder::~DarwinVideoEncoder() {
  release();
}

bool DarwinVideoEncoder::configure(const std::shared_ptr<MediaFormat>& format,
  const std::shared_ptr<VideoSurface>& surface) {
  release();

  if (format == nullptr) {
    MINILOG_ERROR("failed to configure the video encoder: the format is null");
    return false;
  }
  if (!format->isVideo() || format->getMimeType().empty()) {
    MINILOG_ERROR("failed to configure the video encoder: invalid mime type: {}",
      format->getMimeType());
    return false;
  }
  if (surface != nullptr && surface->isValid()) {
    // The session takes its frames through queueInputBuffer, including the ones a GPU produced; a
    // surface would be a second source of frames which the encoder interface does not have.
    MINILOG_WARN("the video surface is not supported by the Darwin video encoder, "
                 "the frames are taken from the input buffer");
  }

  auto codecType = format->getVideoCodecType();
  if (codecType == VideoCodecType::kVideoCodecTypeInvalid) {
    codecType = mCodecType;
  }
  if (codecType == VideoCodecType::kVideoCodecTypeInvalid) {
    codecType = MediaUtils::convertToVideoCodecType(format->getMimeType());
  }
  if (!darwin::isVideoEncoderSupported(codecType)) {
    MINILOG_ERROR("failed to configure the video encoder: no encoder for the codec type {}",
      static_cast<uint32_t>(codecType));
    return false;
  }
  mCodecType = codecType;

  mWidth = format->getWidth() > 0 ? format->getWidth() : 0;
  mHeight = format->getHeight() > 0 ? format->getHeight() : 0;
  if (mWidth <= 0 || mHeight <= 0) {
    MINILOG_ERROR("failed to configure the video encoder: the frame size is unknown");
    return false;
  }
  // The 4:2:0 encoders require an even frame size.
  mWidth &= ~1;
  mHeight &= ~1;

  mFrameRate = format->getFrameRate() > 0 ? format->getFrameRate() : DEFAULT_FRAME_RATE;
  mKeyFrameInterval = format->getIFrameInterval() > 0 ? format->getIFrameInterval()
                                                      : 2 * mFrameRate;
  auto bitRate = format->getBitrate();
  if (bitRate <= 0) {
    bitRate = format->getAverageBitrate();
  }
  if (bitRate <= 0) {
    bitRate = DEFAULT_BIT_RATE;
  }
  mBitRate = bitRate;
  mOutputColorInfo = format->getColorInfo();

  if (!createSession()) {
    release();
    return false;
  }

  mInputBuffer = std::make_shared<NV12PixelBuffer>(mWidth, mHeight);
  mPacketBuffer = std::make_shared<AVPacketBuffer>(MediaType::kMediaTypeVideo);

  const auto mimeType = darwin::mimeTypeOfVideoCodec(mCodecType);
  if (mimeType.empty()) {
    MINILOG_ERROR("failed to configure the video encoder: the codec type has no mime type");
    release();
    return false;
  }
  // The parameter sets only become known when the first frame is encoded; the rest of the format
  // describes the stream already, so a consumer which asks before the first frame gets a usable one.
  mOutputFormat = MediaFormat::CreateVideoFormat(mimeType, mWidth, mHeight);
  mOutputFormat->setVideoCodecType(mCodecType);
  mOutputFormat->setFrameRate(mFrameRate);
  mOutputFormat->setAverageBitrate(mBitRate);
  mOutputFormat->setColorInfo(mOutputColorInfo);
  MINILOG_DEBUG("video encoder output format: {}", mOutputFormat->toString());

  mInputDequeued = false;
  mInputEndOfStream = false;
  mOutputEndOfStream = false;
  mFrameIndex = 0;
  mPendingFrameCount = 0;
  mFormatBuilt = false;
  mFormatChanged = false;
  mFormatReported = false;
  mConfigured = true;
  return true;
}

bool DarwinVideoEncoder::createSession() {
  const auto codecType = darwin::toCMVideoCodecType(mCodecType);
  if (codecType == 0) {
    MINILOG_ERROR("the video codec type has no platform identifier");
    return false;
  }
  const auto status = VTCompressionSessionCreate(kCFAllocatorDefault, mWidth, mHeight, codecType,
    nullptr, nullptr, nullptr, &DarwinVideoEncoder::compressionOutputCallback, this, &mSession);
  if (status != noErr || mSession == nullptr) {
    mSession = nullptr;
    return darwin::logStatus(status, "the video compression session could not be created",
      darwin::mimeTypeOfVideoCodec(mCodecType));
  }

  // The encoder is not driven by a clock: it takes the frames it is given, so it does not have to
  // trade quality for latency the way a real time encoder does.
  setSessionProperty(mSession, kVTCompressionPropertyKey_RealTime, kCFBooleanFalse, "RealTime");
  // The muxer records the packets in the order the encoder emits them, so the emit order has to be
  // the presentation order: a B-frame would make the two differ and the muxer would reorder a valid
  // stream into an undecodable one.
  setSessionProperty(mSession, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse,
    "AllowFrameReordering");
  setSessionIntProperty(mSession, kVTCompressionPropertyKey_AverageBitRate, mBitRate,
    "AverageBitRate");
  setSessionIntProperty(mSession, kVTCompressionPropertyKey_MaxKeyFrameInterval,
    mKeyFrameInterval, "MaxKeyFrameInterval");
  setSessionFloatProperty(mSession, kVTCompressionPropertyKey_ExpectedFrameRate,
    static_cast<float>(mFrameRate), "ExpectedFrameRate");

  const auto prepareStatus = VTCompressionSessionPrepareToEncodeFrames(mSession);
  if (prepareStatus != noErr) {
    // The session allocates what it needs on the first frame instead, which is not a failure.
    MINILOG_DEBUG("the video encoder prepares itself on the first frame: {}",
      darwin::osStatusToString(prepareStatus));
  }
  return true;
}

void DarwinVideoEncoder::start() {
  // The session is prepared at configure time and survives a flush, so a start only ends the state a
  // flush left behind.
  mInputEndOfStream = false;
  mOutputEndOfStream = false;
  mInputDequeued = false;
}

void DarwinVideoEncoder::stop() {
  flush();
}

void DarwinVideoEncoder::flush() {
  waitForPendingFrames();
  discardPendingPackets();
  mPendingFrameCount = 0;
  mInputDequeued = false;
  mInputEndOfStream = false;
  mOutputEndOfStream = false;
}

void DarwinVideoEncoder::release() {
  // The frames the session still holds are finished before the session goes away; the callbacks they
  // run read the state of this encoder.
  waitForPendingFrames();
  if (mSession != nullptr) {
    VTCompressionSessionInvalidate(mSession);
    CFRelease(mSession);
    mSession = nullptr;
  }
  discardPendingPackets();
  mInputBuffer.reset();
  mPacketBuffer.reset();
  {
    std::lock_guard<std::mutex> lock(mMutex);
    mOutputFormat.reset();
    mFormatBuilt = false;
    mFormatChanged = false;
    mFormatReported = false;
  }
  mInputDequeued = false;
  mInputEndOfStream = true;
  mOutputEndOfStream = true;
  mFrameIndex = 0;
  mPendingFrameCount = 0;
  mConfigured = false;
  mWidth = 0;
  mHeight = 0;
  mFrameRate = DEFAULT_FRAME_RATE;
  mBitRate = DEFAULT_BIT_RATE;
  mKeyFrameInterval = 0;
  mOutputColorInfo = ColorInfo();
}

std::shared_ptr<AVPixelBuffer> DarwinVideoEncoder::dequeueInputBuffer() {
  if (!mConfigured || mSession == nullptr || mInputBuffer == nullptr) {
    return nullptr;
  }
  if (mInputDequeued || mInputEndOfStream) {
    return nullptr;
  }
  mInputDequeued = true;
  // The buffer held the flags and the time of the frame it was given for before.
  mInputBuffer->clear();
  return mInputBuffer;
}

void DarwinVideoEncoder::queueInputBuffer(std::shared_ptr<AVPixelBuffer> inputBuffer) {
  if (!inputBuffer || !mConfigured || mSession == nullptr) {
    return;
  }
  mInputDequeued = false;

  if (inputBuffer->isEndOfStream()) {
    mInputEndOfStream = true;
    // The session emits the frames it still holds, so the caller can drain the rest of the stream.
    waitForPendingFrames();
    return;
  }

  CVPixelBufferRef pixelBuffer = nullptr;
  if (const auto hardware = std::dynamic_pointer_cast<HardwarePixelBuffer>(inputBuffer);
      hardware != nullptr) {
    // Zero-copy: the frame is the pixel buffer the producer wrote, which the session reads in place.
    pixelBuffer = retainHardwareFrame(hardware);
  } else {
    if (inputBuffer->width() != mWidth || inputBuffer->height() != mHeight) {
      MINILOG_ERROR("the video encoder takes {}x{} frames, the frame is {}x{}", mWidth, mHeight,
        inputBuffer->width(), inputBuffer->height());
      return;
    }
    pixelBuffer = copyFrameToPixelBuffer(inputBuffer);
  }
  if (pixelBuffer == nullptr) {
    return;
  }
  if (CVPixelBufferGetWidth(pixelBuffer) != static_cast<size_t>(mWidth) ||
      CVPixelBufferGetHeight(pixelBuffer) != static_cast<size_t>(mHeight)) {
    MINILOG_ERROR("the video encoder takes {}x{} frames, the frame holds {}x{}", mWidth, mHeight,
      CVPixelBufferGetWidth(pixelBuffer), CVPixelBufferGetHeight(pixelBuffer));
    CVPixelBufferRelease(pixelBuffer);
    return;
  }

  auto timeUs = inputBuffer->timeUs();
  if (timeUs == Definitions::kTimeUnset) {
    // A frame without a time still needs one; the frame counter spaces the frames with the frame
    // rate of the stream.
    timeUs = mFrameIndex * Definitions::kMicrosPerSecond / mFrameRate;
  }

  mPendingFrameCount.fetch_add(1, std::memory_order_relaxed);
  VTEncodeInfoFlags infoFlags = 0;
  const auto status = VTCompressionSessionEncodeFrame(mSession, pixelBuffer,
    darwin::toCMTime(timeUs), CMTimeMake(1, mFrameRate), nullptr, nullptr, &infoFlags);
  if (status != noErr) {
    // A frame which the call refused is never reported by the callback.
    releasePendingFrame();
    darwin::logStatus(status, "failed to encode a video frame");
    CVPixelBufferRelease(pixelBuffer);
    return;
  }
  ++mFrameIndex;
  if ((infoFlags & kVTEncodeInfo_FrameDropped) != 0) {
    MINILOG_WARN("the video encoder dropped a frame");
  }
  // The session retained the pixel buffer as long as it encodes it.
  CVPixelBufferRelease(pixelBuffer);
}

CVPixelBufferRef DarwinVideoEncoder::retainHardwareFrame(
  const std::shared_ptr<HardwarePixelBuffer>& hardware) {
  if (hardware->getTextureFormat() != HardwarePixelBuffer::TextureFormat::kNv12) {
    MINILOG_ERROR("the video encoder takes nv12 textures, the frame holds the texture format {}",
      static_cast<int>(hardware->getTextureFormat()));
    return nullptr;
  }
  CVPixelBufferRef pixelBuffer = nullptr;
  if (const auto darwinFrame = std::dynamic_pointer_cast<DarwinHardwarePixelBuffer>(hardware);
      darwinFrame != nullptr) {
    pixelBuffer = darwinFrame->getPixelBuffer();
  } else if (const auto native = hardware->getNativeTexture(); native != nullptr) {
    // A hardware buffer of another backend still names its texture through the interface; on Darwin
    // that texture is the pixel buffer.
    pixelBuffer = static_cast<CVPixelBufferRef>(native);
  }
  if (pixelBuffer == nullptr) {
    MINILOG_ERROR("the hardware frame carries no pixel buffer");
    return nullptr;
  }
  const auto pixelFormat = CVPixelBufferGetPixelFormatType(pixelBuffer);
  if (pixelFormat != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange &&
      pixelFormat != kCVPixelFormatType_420YpCbCr8BiPlanarFullRange) {
    // The texture is reported as NV12; a pixel buffer of another format would be encoded as if it
    // were, which the compressor accepts and turns into a broken picture.
    MINILOG_ERROR("the video encoder takes bi-planar 4:2:0 frames, the frame holds the pixel format {}",
      static_cast<unsigned>(pixelFormat));
    return nullptr;
  }
  // The session keeps its own reference while it encodes the frame; this one keeps the frame alive
  // until the submission returned, and is released right after it.
  CVPixelBufferRetain(pixelBuffer);
  return pixelBuffer;
}

CVPixelBufferRef DarwinVideoEncoder::createPixelBuffer() const {
  const auto attributes = createPixelBufferAttributes();
  if (attributes == nullptr) {
    MINILOG_ERROR("the attributes of the encoder pixel buffer could not be created");
    return nullptr;
  }
  CVPixelBufferRef pixelBuffer = nullptr;
  const auto status = CVPixelBufferCreate(kCFAllocatorDefault, mWidth, mHeight,
    kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, attributes, &pixelBuffer);
  CFRelease(attributes);
  if (status != kCVReturnSuccess || pixelBuffer == nullptr) {
    MINILOG_ERROR("failed to allocate a {}x{} nv12 pixel buffer: {}", mWidth, mHeight,
      static_cast<int>(status));
    return nullptr;
  }
  return pixelBuffer;
}

CVPixelBufferRef DarwinVideoEncoder::copyFrameToPixelBuffer(
  const std::shared_ptr<AVPixelBuffer>& frame) {
  const auto pixelBuffer = createPixelBuffer();
  if (pixelBuffer == nullptr) {
    return nullptr;
  }
  if (CVPixelBufferLockBaseAddress(pixelBuffer, 0) != kCVReturnSuccess) {
    MINILOG_ERROR("failed to lock the pixel buffer of an input frame");
    CVPixelBufferRelease(pixelBuffer);
    return nullptr;
  }
  const auto copied = copyFramePlanes(frame, pixelBuffer);
  CVPixelBufferUnlockBaseAddress(pixelBuffer, 0);
  if (!copied) {
    CVPixelBufferRelease(pixelBuffer);
    return nullptr;
  }
  return pixelBuffer;
}

bool DarwinVideoEncoder::copyFramePlanes(const std::shared_ptr<AVPixelBuffer>& frame,
  CVPixelBufferRef destination) {
  auto* luma = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(destination, 0));
  auto* chroma = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(destination, 1));
  const auto lumaStride = CVPixelBufferGetBytesPerRowOfPlane(destination, 0);
  const auto chromaStride = CVPixelBufferGetBytesPerRowOfPlane(destination, 1);
  if (luma == nullptr || chroma == nullptr) {
    MINILOG_ERROR("the encoder pixel buffer has no planes");
    return false;
  }
  if (const auto nv12 = frame->asNV12PixelBuffer(); nv12 != nullptr) {
    return copyNV12Planes(*nv12, luma, lumaStride, chroma, chromaStride);
  }
  if (const auto i420 = frame->asI420PixelBuffer(); i420 != nullptr) {
    return copyI420Planes(*i420, luma, lumaStride, chroma, chromaStride);
  }
  MINILOG_ERROR("the video encoder takes nv12 or i420 frames, the frame is the pixel format {}",
    static_cast<int>(frame->getFormat()));
  return false;
}

bool DarwinVideoEncoder::copyNV12Planes(const NV12PixelBuffer& source, uint8_t* luma,
  size_t lumaStride, uint8_t* chroma, size_t chromaStride) const {
  if (source.strideY() <= 0 || source.strideUV() <= 0) {
    return false;
  }
  const auto lumaBytes = std::min(static_cast<size_t>(mWidth),
    static_cast<size_t>(source.strideY()));
  // The interleaved chroma row of the picture is two bytes per sample, and the source may pad it.
  const auto chromaBytes = std::min(static_cast<size_t>(2 * ((mWidth + 1) / 2)),
    static_cast<size_t>(source.strideUV()));
  if (lumaBytes == 0 || chromaBytes == 0) {
    return false;
  }
  const auto chromaRows = (mHeight + 1) / 2;
  for (int row = 0; row < mHeight; ++row) {
    memcpy(luma + static_cast<ptrdiff_t>(row) * lumaStride,
      source.dataY() + static_cast<ptrdiff_t>(row) * source.strideY(), lumaBytes);
  }
  for (int row = 0; row < chromaRows; ++row) {
    memcpy(chroma + static_cast<ptrdiff_t>(row) * chromaStride,
      source.dataUV() + static_cast<ptrdiff_t>(row) * source.strideUV(), chromaBytes);
  }
  return true;
}

bool DarwinVideoEncoder::copyI420Planes(const I420PixelBuffer& source, uint8_t* luma,
  size_t lumaStride, uint8_t* chroma, size_t chromaStride) const {
  if (source.strideY() <= 0 || source.strideU() <= 0 || source.strideV() <= 0) {
    return false;
  }
  const auto chromaWidth = (mWidth + 1) / 2;
  const auto lumaBytes = std::min(static_cast<size_t>(mWidth),
    static_cast<size_t>(source.strideY()));
  const auto columns = std::min(
    std::min(static_cast<size_t>(chromaWidth), static_cast<size_t>(source.strideU())),
    static_cast<size_t>(source.strideV()));
  if (lumaBytes == 0 || columns == 0) {
    return false;
  }
  const auto chromaRows = (mHeight + 1) / 2;
  for (int row = 0; row < mHeight; ++row) {
    memcpy(luma + static_cast<ptrdiff_t>(row) * lumaStride,
      source.dataY() + static_cast<ptrdiff_t>(row) * source.strideY(), lumaBytes);
  }
  for (int row = 0; row < chromaRows; ++row) {
    const auto* u = source.dataU() + static_cast<ptrdiff_t>(row) * source.strideU();
    const auto* v = source.dataV() + static_cast<ptrdiff_t>(row) * source.strideV();
    auto* target = chroma + static_cast<ptrdiff_t>(row) * chromaStride;
    // The destination chroma plane is interleaved: the compressor reads the 420v layout.
    for (size_t column = 0; column < columns; ++column) {
      target[2 * column] = u[column];
      target[2 * column + 1] = v[column];
    }
  }
  return true;
}

void DarwinVideoEncoder::compressionOutputCallback(void* outputCallbackRefCon,
  void* sourceFrameRefCon, const OSStatus status, const VTEncodeInfoFlags infoFlags,
  CMSampleBufferRef sampleBuffer) {
  static_cast<void>(sourceFrameRefCon);
  auto* encoder = static_cast<DarwinVideoEncoder*>(outputCallbackRefCon);
  if (encoder == nullptr) {
    return;
  }
  try {
    encoder->onFrameEncoded(status, infoFlags, sampleBuffer);
  } catch (...) {
    // The callback runs on a VideoToolbox thread; an exception which escapes it would terminate the
    // process rather than fail a frame.
    MINILOG_ERROR("the video encoder failed to handle an encoded frame");
  }
}

void DarwinVideoEncoder::onFrameEncoded(const OSStatus status, const VTEncodeInfoFlags infoFlags,
  CMSampleBufferRef sampleBuffer) {
  releasePendingFrame();

  if (status != noErr) {
    darwin::logStatus(status, "failed to encode a video frame");
    return;
  }
  if (sampleBuffer == nullptr) {
    if ((infoFlags & kVTEncodeInfo_FrameDropped) != 0) {
      MINILOG_WARN("the video encoder dropped a frame");
    }
    return;
  }
  if (!CMSampleBufferDataIsReady(sampleBuffer)) {
    MINILOG_WARN("the video encoder completed a frame which holds no data yet");
    return;
  }

  const auto description = CMSampleBufferGetFormatDescription(sampleBuffer);
  {
    std::lock_guard<std::mutex> lock(mMutex);
    if (!mFormatBuilt && description != nullptr) {
      mFormatBuilt = true;
      buildOutputFormatLocked(description);
      // The caller has to see the format before the packet which belongs to it, and the format is
      // only known now.
      mFormatChanged = true;
    }
  }

  const auto blockBuffer = CMSampleBufferGetDataBuffer(sampleBuffer);
  if (blockBuffer == nullptr) {
    MINILOG_WARN("the encoded frame holds no block buffer");
    return;
  }
  const auto dataLength = CMBlockBufferGetDataLength(blockBuffer);
  if (dataLength == 0) {
    MINILOG_WARN("the encoded frame is empty");
    return;
  }
  std::vector<uint8_t> bytes(dataLength);
  const auto copyStatus = CMBlockBufferCopyDataBytes(blockBuffer, 0, dataLength, bytes.data());
  if (copyStatus != noErr) {
    darwin::logStatus(copyStatus, "failed to read an encoded video frame");
    return;
  }
  // The session emits the four byte length prefixed form the muxer writes, which is passed through
  // unchanged. The shape is checked for diagnostics only: the bitstream helpers validate the whole
  // buffer before they decide between the length prefixed and the Annex B form.
  if (!isFourByteLengthPrefixed(bytes.data(), bytes.size())) {
    MINILOG_DEBUG("the encoded frame is not four byte length prefixed");
  }
  auto data = std::move(bytes);

  auto keyFrame = true;
  const auto attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, false);
  if (attachments != nullptr && CFArrayGetCount(attachments) > 0) {
    const auto dictionary = static_cast<CFDictionaryRef>(
      const_cast<void*>(CFArrayGetValueAtIndex(attachments, 0)));
    if (dictionary != nullptr) {
      const auto notSync = static_cast<CFBooleanRef>(
        CFDictionaryGetValue(dictionary, kCMSampleAttachmentKey_NotSync));
      if (notSync != nullptr && CFGetTypeID(notSync) == CFBooleanGetTypeID() &&
          CFBooleanGetValue(notSync)) {
        keyFrame = false;
      }
    }
  }

  PendingPacket packet;
  packet.data = std::move(data);
  packet.timeUs = darwin::toTimeUs(CMSampleBufferGetPresentationTimeStamp(sampleBuffer));
  packet.keyFrame = keyFrame;
  {
    std::lock_guard<std::mutex> lock(mMutex);
    mPendingPackets.push_back(std::move(packet));
  }
}

void DarwinVideoEncoder::buildOutputFormatLocked(CMFormatDescriptionRef description) {
  const auto dimensions = CMVideoFormatDescriptionGetDimensions(description);
  const auto width = dimensions.width > 0 ? static_cast<int>(dimensions.width) : mWidth;
  const auto height = dimensions.height > 0 ? static_cast<int>(dimensions.height) : mHeight;
  const auto mimeType = darwin::mimeTypeOfVideoCodec(mCodecType);
  if (mimeType.empty()) {
    MINILOG_ERROR("the encoded stream has no mime type for the codec type");
    return;
  }
  auto format = MediaFormat::CreateVideoFormat(mimeType, width, height);
  format->setVideoCodecType(mCodecType);
  format->setFrameRate(mFrameRate);
  format->setAverageBitrate(mBitRate);
  // The session describes the color only when it was told to; the frames inherited the color space of
  // the source, which is what the consumers convert with.
  auto colorInfo = mOutputColorInfo;
  const auto descriptionColorInfo = darwin::colorInfoOf(description);
  if (descriptionColorInfo.colorSpace != ColorInfo::SpaceType::kSpaceUnknown) {
    colorInfo.colorSpace = descriptionColorInfo.colorSpace;
  }
  if (descriptionColorInfo.colorRange != ColorInfo::RangeType::kRangeUnknown) {
    colorInfo.colorRange = descriptionColorInfo.colorRange;
  }
  if (descriptionColorInfo.colorTransfer != ColorInfo::TransferType::kTransferUnknown) {
    colorInfo.colorTransfer = descriptionColorInfo.colorTransfer;
  }
  format->setColorInfo(colorInfo);
  applyCodecSpecificData(*format, mCodecType, description);
  mOutputFormat = std::move(format);
  MINILOG_DEBUG("video encoder output format: {}", mOutputFormat->toString());
}

std::shared_ptr<AVPacketBuffer> DarwinVideoEncoder::takePendingPacket() {
  PendingPacket packet;
  {
    std::lock_guard<std::mutex> lock(mMutex);
    if (mPendingPackets.empty()) {
      return nullptr;
    }
    packet = std::move(mPendingPackets.front());
    mPendingPackets.pop_front();
  }
  if (mPacketBuffer == nullptr) {
    return nullptr;
  }
  mPacketBuffer->clear();
  mPacketBuffer->ensureSpaceForWrite(packet.data.size());
  const auto data = mPacketBuffer->data();
  if (data != nullptr) {
    data->position(0);
    if (!packet.data.empty()) {
      data->put(packet.data.data(), packet.data.size());
    }
    data->flip();
  }
  mPacketBuffer->timeUs(packet.timeUs);
  if (packet.keyFrame) {
    mPacketBuffer->addFlag(AVBufferFlags::kKeyFrame);
  }
  return mPacketBuffer;
}

std::shared_ptr<AVPacketBuffer> DarwinVideoEncoder::createEndOfStreamBuffer() {
  mPacketBuffer->clear();
  mPacketBuffer->setFlags(AVBufferFlags::kEndOfStream);
  return mPacketBuffer;
}

std::shared_ptr<AVPacketBuffer> DarwinVideoEncoder::dequeueOutputBuffer() {
  if (!mConfigured || mSession == nullptr || mPacketBuffer == nullptr) {
    return nullptr;
  }

  std::shared_ptr<MediaFormat> outputFormat;
  {
    std::lock_guard<std::mutex> lock(mMutex);
    if (mFormatChanged && !mFormatReported) {
      mFormatReported = true;
      outputFormat = mOutputFormat;
    }
  }
  if (outputFormat != nullptr) {
    // A flags only packet which carries the format the session described, before the packet which
    // belongs to it; the muxer takes the track description from it.
    mPacketBuffer->clear();
    mPacketBuffer->setFormat(outputFormat);
    return mPacketBuffer;
  }

  if (auto packet = takePendingPacket(); packet != nullptr) {
    return packet;
  }
  if (!mInputEndOfStream) {
    return nullptr;
  }
  if (mPendingFrameCount.load() != 0) {
    // The callback of a frame the session finished may still be on its way, and the end of the
    // stream must not overtake the packet it carries.
    return nullptr;
  }
  if (mOutputEndOfStream) {
    return nullptr;
  }
  mOutputEndOfStream = true;
  return createEndOfStreamBuffer();
}

void DarwinVideoEncoder::releaseOutputBuffer(const std::shared_ptr<AVPacketBuffer>& outputBuffer,
  const bool render) {
  static_cast<void>(outputBuffer);
  static_cast<void>(render);
  // The packet buffer is owned by the encoder and reused for the next packet.
}

void DarwinVideoEncoder::signalEndOfInputStream() {
  if (mSession == nullptr) {
    return;
  }
  mInputEndOfStream = true;
  // The session emits the frames it still holds, so the packets are in the queue when they are asked
  // for.
  waitForPendingFrames();
}

void DarwinVideoEncoder::waitForPendingFrames() {
  if (mSession == nullptr) {
    return;
  }
  const auto status = VTCompressionSessionCompleteFrames(mSession, kCMTimePositiveInfinity);
  if (status != noErr) {
    darwin::logStatus(status, "failed to complete the pending video frames");
  }
}

void DarwinVideoEncoder::releasePendingFrame() {
  auto pending = mPendingFrameCount.load(std::memory_order_relaxed);
  while (pending > 0 && !mPendingFrameCount.compare_exchange_weak(pending, pending - 1,
           std::memory_order_relaxed)) {
  }
}

void DarwinVideoEncoder::discardPendingPackets() {
  std::deque<PendingPacket> packets;
  {
    std::lock_guard<std::mutex> lock(mMutex);
    packets.swap(mPendingPackets);
  }
  packets.clear();
}

} // namespace coral

#endif // defined(__APPLE__)
