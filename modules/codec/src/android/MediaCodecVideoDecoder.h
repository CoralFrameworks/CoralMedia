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

#ifndef CORAL_CODEC_ANDROID_MEDIACODEC_VIDEO_DECODER_H
#define CORAL_CODEC_ANDROID_MEDIACODEC_VIDEO_DECODER_H

#include <codec/MediaCodec.h>

#include <media/ColorInfo.h>
#include <media/PacketBuffer.h>

#include <core/jni/android/MediaCodec.h>
#include <core/jni/android/MediaFormat.h>

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>

namespace coral {

/**
 * The VideoDecoder implementation based on android.media.MediaCodec
 */
class MediaCodecVideoDecoder final : public VideoDecoder,
                                     public AVPixelBuffer::Owner<AVPixelBuffer> {
private:
  static constexpr int INDEX_UNSET = -1;

  mutable std::mutex pending_lock_;
  bool mHasOutputSurface;
  /**
   * Whether the consumer of the output surface samples its frames itself.
   *
   * A `SurfaceTexture` consumer can only sample a frame that is already queued, so a decoder configured with
   * such a surface renders each frame as it is decoded; a surface the platform presents holds the frame until
   * the composition asks for it.
   */
  bool mSurfaceIsSampled;
  /**
   * The consumer that takes the frames of the surface this decoder renders into.
   *
   * The decoder's output buffer names it so that the conversion stage asks the right queue for the frame: a
   * `SurfaceTexture` belongs to one decoder, so the device cannot answer that question.
   */
  std::shared_ptr<SurfaceFrameSource> mSurfaceFrames;

  std::unique_ptr<JniMediaCodec> mMediaCodec;
  std::unique_ptr<JniMediaFormat> mMediaFormat;
  std::shared_ptr<MediaFormat> mOutputFormat;
  int mWidth;
  int mHeight;
  std::atomic_int mInputIndex;
  bool mInputEof;
  bool mOutputEof;
  int64_t outputStartTimeUs;

  std::shared_ptr<AVPacketBuffer> packetBuffer;
  std::shared_ptr<AVPacketBuffer> dequeuedInputBuffer;

  std::deque<std::shared_ptr<AVPixelBuffer>> availableOutputBuffers;

  struct DecodeInfo {
    int width = 0;
    int height = 0;
    int stride = 0;
    int sliceHeight = 0;
    int cropTop = 0;
    int cropBottom = 0;
    int cropLeft = 0;
    int cropRight = 0;
    int displayWidth = 0;
    int displayHeight = 0;
    ColorInfo colorInfo{};
  };
  DecodeInfo decodeInfo;

public:
  MediaCodecVideoDecoder();

  ~MediaCodecVideoDecoder() override;

  void setOutputStartTimeUs(int64_t outputStartTimeUs) override;

  bool configure(const std::shared_ptr<MediaFormat> &mediaFormat,
      const std::shared_ptr<VideoSurface> &surface) override;

  void start() override;

  void stop() override;

  void flush() override;

  void release() override;

  std::shared_ptr<AVPacketBuffer> dequeueInputBuffer() override;

  void queueInputBuffer(std::shared_ptr<AVPacketBuffer> buffer) override;

  std::shared_ptr<AVPixelBuffer> dequeueOutputBuffer() override;

  void signalEndOfInputStream() override;

  void releaseOutputBuffer(const std::shared_ptr<AVPixelBuffer>& frameBuffer, bool render) override;

  void recycleBuffer(std::shared_ptr<AVPixelBuffer> decodedBuffer) override;

  const std::shared_ptr<MediaFormat> &getOutputFormat() const override {
    return mOutputFormat;
  }

  const char *getName() const override {
    return "MediaCodecVideoDecoder";
  }

private:
  bool hasOutputSurface() const {
    return mHasOutputSurface;
  }

  void parseOutputFormat(const std::unique_ptr<JniMediaFormat> &mediaFormat);

  [[nodiscard]] bool isAtLeastOutputStartTimeUs(int64_t timeUs) const {
    return outputStartTimeUs == Definitions::kTimeUnset || timeUs >= outputStartTimeUs;
  }

  std::shared_ptr<AVPixelBuffer>
  createSoftwareFrameBuffer(uint8_t *data,
      int size,
      int index,
      const JniMediaCodec::BufferInfo &info,
      int colorFormat);

  std::shared_ptr<AVPixelBuffer>
  createSoftwareBufferCopyYuv420Planar(uint8_t *data,
      int size,
      int index,
      const JniMediaCodec::BufferInfo &info);

  std::shared_ptr<AVPixelBuffer>
  createSoftwareBufferCopyYuv420SemiPlanar(uint8_t *data,
      int size,
      int index,
      const JniMediaCodec::BufferInfo &info);

  std::shared_ptr<AVPixelBuffer>
  createSoftwareBufferCopyYuv420PackedSemiPlanar(uint8_t *data,
      int size,
      int index,
      const JniMediaCodec::BufferInfo &info);

  std::shared_ptr<AVPixelBuffer>
  dequeueAvailableOutputBuffer(const PixelFormat &formatType);
};

} // namespace coral

#endif // CORAL_CODEC_ANDROID_MEDIACODEC_VIDEO_DECODER_H
