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

#ifndef CORAL_CODEC_DARWIN_VIDEO_DECODER_H
#define CORAL_CODEC_DARWIN_VIDEO_DECODER_H

#if defined(__APPLE__)

#include <codec/HardwareVideo.h>
#include <codec/MediaCodec.h>

#include <media/ColorInfo.h>
#include <media/Definitions.h>
#include <media/Format.h>

#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>

namespace coral {

/**
 * The implementation of the VideoDecoder based on an Apple VideoToolbox decompression session.
 *
 * Every decoded frame is either the `CVPixelBuffer` the session produced -- an `IOSurface` backed
 * buffer which a GPU consumer samples without a copy -- or a copy of its planes in an
 * {@link NV12PixelBuffer}. A hardware frame is bi-planar 4:2:0
 * (`kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange`) by default, or one packed BGRA plane
 * (`kCVPixelFormatType_32BGRA`) when the input format's `processFormat` asks for it. That lets a
 * consumer that samples one RGBA texture stay zero-copy.
 *
 * The surface the decoder is configured with is what decides which of the two the caller gets. A
 * {@link HardwareVideoSurface} asks for the platform buffers; any other surface is a request this
 * decoder cannot honour and logs a warning, after which the frames are copied. A hardware session
 * which falls back to software still produces the same `CVPixelBuffer` shape, so the choice is made
 * once, at configure time, and a frame never changes type halfway through a stream.
 */
class DarwinVideoDecoder final : public VideoDecoder,
                                 public AVPixelBuffer::Owner<AVPixelBuffer> {
public:
  DarwinVideoDecoder();

  ~DarwinVideoDecoder() override;

  // Makes the single argument overload of the base class visible.
  using VideoDecoder::configure;

  bool configure(const std::shared_ptr<MediaFormat>& format,
    const std::shared_ptr<VideoSurface>& surface) override;

  void start() override;

  void stop() override;

  void flush() override;

  void release() override;

  std::shared_ptr<AVPacketBuffer> dequeueInputBuffer() override;

  void queueInputBuffer(std::shared_ptr<AVPacketBuffer> inputBuffer) override;

  std::shared_ptr<AVPixelBuffer> dequeueOutputBuffer() override;

  void releaseOutputBuffer(const std::shared_ptr<AVPixelBuffer>& outputBuffer, bool render) override;

  void signalEndOfInputStream() override;

  void setOutputStartTimeUs(int64_t outputStartTimeUs) override;

  void recycleBuffer(std::shared_ptr<AVPixelBuffer> frameBuffer) override;

  [[nodiscard]] const std::shared_ptr<MediaFormat>& getOutputFormat() const override {
    return mOutputFormat;
  }

  [[nodiscard]] const char* getName() const override {
    return "DarwinVideoDecoder";
  }

private:
  /**
   * The VideoToolbox output callback.
   *
   * The session invokes it on its own threads, so it only forwards to {@link onFrameDecoded}, which
   * takes the locks the decoder state needs.
   */
  static void decompressionOutputCallback(void* decompressionOutputRefCon,
    void* sourceFrameRefCon,
    OSStatus status,
    VTDecodeInfoFlags infoFlags,
    CVImageBufferRef imageBuffer,
    CMTime presentationTimeStamp,
    CMTime presentationDuration);

  /** Wraps or copies a decoded pixel buffer and queues it for {@link dequeueOutputBuffer}. */
  void onFrameDecoded(OSStatus status, CVImageBufferRef imageBuffer,
    CMTime presentationTimeStamp);

  /**
   * Takes the decoded frame as the platform buffer, without touching its pixels.
   *
   * The buffer keeps its own reference to the pixel buffer, so the decoder's pool may hand the same
   * surface to the next frame as soon as the consumer is done with this one.
   */
  std::shared_ptr<AVPixelBuffer> createHardwareFrameBuffer(CVPixelBufferRef pixelBuffer,
    int64_t timeUs);

  /** Copies the planes of a decoded pixel buffer into an NV12 buffer the CPU can read. */
  std::shared_ptr<AVPixelBuffer> createCpuFrameBuffer(CVPixelBufferRef pixelBuffer, int64_t timeUs);

  /** Returns the next decoded frame the callback queued, or null when there is none. */
  std::shared_ptr<AVPixelBuffer> takePendingFrame();

  void pushPendingFrame(std::shared_ptr<AVPixelBuffer> frame);

  /** Drops the frames which were decoded but not handed out; the caller keeps the ones it holds. */
  void discardPendingFrames();

  /** Waits until the frames the session decodes asynchronously have been delivered. */
  void drainAsynchronousFrames();

  /** Drops the frames of the stream which ended and resets the state that describes it. */
  void resetStreamState();

  /** Rebuilds the output format for the current frame size; the caller holds {@link mFormatMutex}. */
  void rebuildOutputFormatLocked();

  std::shared_ptr<AVPixelBuffer> createEndOfStreamBuffer();

  VTDecompressionSessionRef mSession = nullptr;
  /**
   * The format description the session was created from.
   *
   * The session retains what it needs, but a stream change would otherwise be the only way to get the
   * description back, and the input samples are stamped with it.
   */
  CMVideoFormatDescriptionRef mFormatDescription = nullptr;

  /**
   * The surface the caller offered when it wants decoded frames in GPU buffers, or null.
   *
   * It is kept for the life of the decoder and is never replaced or cleared while a session is
   * running; the callback reads it without a lock.
   */
  std::shared_ptr<VideoSurface> mHardwareSurface;

  /** The layout of a hardware frame: NV12 by default, BGRA when the format asked for it. */
  HardwarePixelBuffer::TextureFormat mHardwareTextureFormat =
      HardwarePixelBuffer::TextureFormat::kNv12;

  /** The format reported for hardware frames; mirrors {@link mHardwareTextureFormat}. */
  PixelFormat mHardwarePixelFormat = PixelFormat::kFormatNV12;

  std::shared_ptr<MediaFormat> mOutputFormat;

  /**
   * The color space of the decoded frames, which the renderers use to convert them to RGB.
   *
   * A software decode does not describe the frames again, so the color information of the track is
   * what the frames are given; it is read on the callback threads under {@link mFormatMutex}.
   */
  ColorInfo mOutputColorInfo;

  int mWidth = 0;
  int mHeight = 0;
  /** The frame rate of the track, or 0 when the format does not name one. */
  int mFrameRate = 0;

  std::shared_ptr<AVPacketBuffer> mInputBuffer;

  /**
   * The decoded frames waiting to be handed out.
   *
   * The session pushes frames from its own threads, so the queue is protected by {@link mFrameMutex}.
   * It is bounded: a caller which falls behind by more than the queue loses its oldest frame rather
   * than making the decoder grow without limit.
   */
  std::deque<std::shared_ptr<AVPixelBuffer>> mPendingFrames;

  /** The decoded frames which were handed to the caller and not released yet. */
  std::atomic_int mOutstandingOutputCount { 0 };

  /** The frames decoded before this time are marked as skipped; `kTimeUnset` disables the check. */
  std::atomic<int64_t> mOutputStartTimeUs { Definitions::kTimeUnset };

  /** The fallback presentation time of a frame which arrives without one. */
  int64_t mFrameIndex = 0;

  bool mConfigured = false;
  bool mInputDequeued = false;
  bool mInputEndOfStream = false;
  bool mOutputEndOfStream = false;
  bool mDrainRequested = false;
  /** Whether the decoder the session really uses was logged, which only the first frame can tell. */
  bool mHardwareAccelerationLogged = false;

  mutable std::mutex mFrameMutex;

  /**
   * Protects the frame size, the output format, and the color information from the callback threads.
   *
   * The input side never touches these: a decoder only changes its output when the stream does, and
   * that happens on the callback thread.
   */
  mutable std::mutex mFormatMutex;
};

} // namespace coral

#endif // defined(__APPLE__)

#endif // CORAL_CODEC_DARWIN_VIDEO_DECODER_H
