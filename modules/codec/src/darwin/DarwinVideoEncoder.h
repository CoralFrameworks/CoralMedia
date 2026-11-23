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

#ifndef CORAL_CODEC_DARWIN_VIDEO_ENCODER_H
#define CORAL_CODEC_DARWIN_VIDEO_ENCODER_H

#if defined(__APPLE__)

#include <codec/MediaCodec.h>

#include <media/ColorInfo.h>
#include <media/Definitions.h>
#include <media/Format.h>
#include <media/PixelBuffer.h>

#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

namespace coral {

class HardwarePixelBuffer;

/**
 * The implementation of the VideoEncoder based on an Apple VideoToolbox compression session.
 *
 * The encoder takes its frames through {@link queueInputBuffer} in the NV12 pixel format. A
 * {@link HardwarePixelBuffer} whose texture is a `CVPixelBuffer` is encoded in place -- the session
 * reads the `IOSurface` the GPU wrote -- and every other frame is copied into a pixel buffer first.
 *
 * The compressed packets are length prefixed NAL units (AVCC) in decode order: the session is asked
 * for no frame reordering, so the order the encoder emits is the order the muxer writes.
 */
class DarwinVideoEncoder final : public VideoEncoder {
public:
  explicit DarwinVideoEncoder(VideoCodecType codecType);

  ~DarwinVideoEncoder() override;

  // Makes the single argument overload of the base class visible.
  using VideoEncoder::configure;

  bool configure(const std::shared_ptr<MediaFormat>& format,
    const std::shared_ptr<VideoSurface>& surface) override;

  void start() override;

  void stop() override;

  void flush() override;

  void release() override;

  std::shared_ptr<AVPixelBuffer> dequeueInputBuffer() override;

  void queueInputBuffer(std::shared_ptr<AVPixelBuffer> inputBuffer) override;

  std::shared_ptr<AVPacketBuffer> dequeueOutputBuffer() override;

  void releaseOutputBuffer(const std::shared_ptr<AVPacketBuffer>& outputBuffer, bool render) override;

  void signalEndOfInputStream() override;

  [[nodiscard]] const std::shared_ptr<MediaFormat>& getOutputFormat() const override {
    return mOutputFormat;
  }

  [[nodiscard]] const char* getName() const override {
    return "DarwinVideoEncoder";
  }

private:
  /**
   * A compressed packet the session produced which the caller has not dequeued yet.
   *
   * The output callback runs on a VideoToolbox thread and cannot fill the packet buffer the caller
   * owns, so the bytes are copied here and the buffer is filled on the caller's thread.
   */
  struct PendingPacket {
    std::vector<uint8_t> data;
    int64_t timeUs = Definitions::kTimeUnset;
    bool keyFrame = false;
  };

  /** The compression output callback; it forwards to {@link onFrameEncoded} and never throws. */
  static void compressionOutputCallback(void* outputCallbackRefCon, void* sourceFrameRefCon,
    OSStatus status, VTEncodeInfoFlags infoFlags, CMSampleBufferRef sampleBuffer);

  void onFrameEncoded(OSStatus status, VTEncodeInfoFlags infoFlags,
    CMSampleBufferRef sampleBuffer);

  bool createSession();

  /**
   * Fills the output format from the format description of the first encoded frame.
   *
   * The caller holds {@link mMutex}, which is what makes the format the callback describes and the
   * format {@link dequeueOutputBuffer} hands out the same object.
   */
  void buildOutputFormatLocked(CMFormatDescriptionRef description);

  /**
   * The pixel buffer of a hardware frame, retained for the submission.
   *
   * This is the zero-copy path: the decoder or camera which produced the frame wrote an `IOSurface`
   * the compressor reads directly, so neither the luma nor the chroma plane reaches the CPU.
   */
  CVPixelBufferRef retainHardwareFrame(const std::shared_ptr<HardwarePixelBuffer>& hardware);

  /** Allocates an NV12 pixel buffer of the configured size, which the session accepts. */
  CVPixelBufferRef createPixelBuffer() const;

  /** Copies an NV12 or I420 frame into a pixel buffer; null when the frame is neither. */
  CVPixelBufferRef copyFrameToPixelBuffer(const std::shared_ptr<AVPixelBuffer>& frame);

  bool copyFramePlanes(const std::shared_ptr<AVPixelBuffer>& frame, CVPixelBufferRef destination);

  bool copyNV12Planes(const NV12PixelBuffer& source, uint8_t* luma, size_t lumaStride,
    uint8_t* chroma, size_t chromaStride) const;

  bool copyI420Planes(const I420PixelBuffer& source, uint8_t* luma, size_t lumaStride,
    uint8_t* chroma, size_t chromaStride) const;

  /** Pops the oldest encoded packet into the reusable packet buffer, or returns null. */
  std::shared_ptr<AVPacketBuffer> takePendingPacket();

  std::shared_ptr<AVPacketBuffer> createEndOfStreamBuffer();

  /** Waits until the frames the session still holds are encoded and delivered. */
  void waitForPendingFrames();

  /** Releases one submission; the count guards the end of the stream against an in-flight frame. */
  void releasePendingFrame();

  void discardPendingPackets();

  static constexpr int DEFAULT_BIT_RATE = 4000000;
  static constexpr int DEFAULT_FRAME_RATE = 30;

  VideoCodecType mCodecType;

  bool mConfigured = false;
  bool mInputDequeued = false;
  bool mInputEndOfStream = false;
  bool mOutputEndOfStream = false;

  VTCompressionSessionRef mSession = nullptr;

  std::shared_ptr<MediaFormat> mOutputFormat;

  int mWidth = 0;
  int mHeight = 0;
  int mFrameRate = DEFAULT_FRAME_RATE;
  int mBitRate = DEFAULT_BIT_RATE;
  int mKeyFrameInterval = 0;

  /** The color space of the frames which are encoded, which the output format inherits. */
  ColorInfo mOutputColorInfo;

  /** The frame the caller fills, in the NV12 pixel format the session takes. */
  std::shared_ptr<AVPixelBuffer> mInputBuffer;

  std::shared_ptr<AVPacketBuffer> mPacketBuffer;

  /** The encoded packets waiting to be handed out; protected by {@link mMutex}. */
  std::deque<PendingPacket> mPendingPackets;

  /**
   * The number of frames which were submitted and whose output callback has not run yet.
   *
   * The end of the stream is only reported once it reaches zero: the callback of a frame may still be
   * on its way when the session finished the frame it was completing.
   */
  std::atomic_int mPendingFrameCount { 0 };

  /** The number of the frame which is queued next, which spaces the frames without a time. */
  int64_t mFrameIndex = 0;

  /** Whether the output format was built from the format description of an encoded frame. */
  bool mFormatBuilt = false;
  /** Whether {@link mOutputFormat} has to be handed out before the next packet. */
  bool mFormatChanged = false;
  /** Whether the format of this encoder was handed out already. */
  bool mFormatReported = false;

  /** Guards the packet queue and the output format, which the callback threads write. */
  mutable std::mutex mMutex;
};

} // namespace coral

#endif // defined(__APPLE__)

#endif // CORAL_CODEC_DARWIN_VIDEO_ENCODER_H
