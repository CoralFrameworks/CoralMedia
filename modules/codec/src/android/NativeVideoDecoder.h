/*
 * Copyright (C) 2023 The Coral Project
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

#ifndef CORAL_CODEC_ANDROID_NATIVE_VIDEO_DECODER_H
#define CORAL_CODEC_ANDROID_NATIVE_VIDEO_DECODER_H

#include <codec/MediaCodec.h>
#include <media/ColorInfo.h>

#if defined(__ANDROID__)
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>
#endif

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>

namespace coral {

/**
 * The VideoDecoder implementation based on AMediaCodec
 */
class NativeVideoDecoder final : public VideoDecoder,
                                 public AVPixelBuffer::Owner<AVPixelBuffer> {
private:
  static constexpr int INDEX_UNSET = -1;
  static constexpr int DEFAULT_MAX_BUFFER_COUNT = 8;

  struct AMediaCodecDeleter {
    void operator()(AMediaCodec *codec) const {
      if (codec != nullptr) {
        AMediaCodec_stop(codec);
        AMediaCodec_delete(codec);
      }
    }
  };

  struct AMediaFormatDeleter {
    void operator()(AMediaFormat *mediaFormat) const {
      if (mediaFormat != nullptr) {
        AMediaFormat_delete(mediaFormat);
      }
    }
  };

  struct NativeWindowDeleter {
    void operator()(ANativeWindow *nativeWindow) const {
      if (nativeWindow != nullptr) {
        ANativeWindow_release(nativeWindow);
        nativeWindow = nullptr;
      }
    }
  };

  mutable std::mutex pending_lock_;

  std::unique_ptr<AMediaFormat, AMediaFormatDeleter> codecFormat;
  std::unique_ptr<AMediaCodec, AMediaCodecDeleter> decoder;
  std::unique_ptr<ANativeWindow, NativeWindowDeleter> mNativeWindow;
  std::shared_ptr<MediaFormat> outputFormat;
  int mWidth;
  int mHeight;
  std::atomic_int mInputIndex;
  bool mInputEof;
  bool mOutputEof;
  /**
   * Whether the consumer of the output surface samples its frames itself.
   *
   * A `SurfaceTexture` consumer can only sample a frame that is already queued, so a decoder configured with
   * such a surface renders each frame as it is decoded; a surface the platform presents holds the frame until
   * the composition asks for it. See `VideoSurface::isSampledByConsumer`.
   */
  bool mSurfaceIsSampled = false;
  /**
   * The consumer that takes the frames of the output surface, when there is one.
   *
   * Carried on every surface buffer this decoder returns, so the conversion stage asks the surface that
   * produced the frame for it: a `SurfaceTexture` queue belongs to one decoder and one consumer.
   */
  std::shared_ptr<SurfaceFrameSource> mSurfaceFrames;
  int64_t outputStartTimeUs{};

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
  DecodeInfo decodeInfo{};

public:
  NativeVideoDecoder();

  ~NativeVideoDecoder() override;

  bool configure(const std::shared_ptr<MediaFormat> &format, const std::shared_ptr<VideoSurface> &surface) override;

  void setOutputStartTimeUs(int64_t outputStartTimeUs) override;

  void start() override;

  void stop() override;

  void flush() override;

  void release() override;

  std::shared_ptr<AVPacketBuffer> dequeueInputBuffer() override;

  void queueInputBuffer(std::shared_ptr<AVPacketBuffer> buffer) override;

  std::shared_ptr<AVPixelBuffer> dequeueOutputBuffer() override;

  void signalEndOfInputStream() override;

  void releaseOutputBuffer(const std::shared_ptr<AVPixelBuffer>& frameBuffer, bool render) override;

  void recycleBuffer(std::shared_ptr<AVPixelBuffer> outputBuffer) override;

  const std::shared_ptr<MediaFormat> &getOutputFormat() const override {
    return outputFormat;
  }

  const char *getName() const override {
    return "NdkVideoDecoder";
  }

private:
  bool hasOutputSurface() const {
    return mNativeWindow != nullptr;
  }

  void parseOutputFormat(AMediaFormat *mediaFormat);

  std::shared_ptr<AVPixelBuffer>
  createSoftwareFrameBuffer(uint8_t *data,
      int size,
      ssize_t index,
      const AMediaCodecBufferInfo &info,
      int colorFormat);

  std::shared_ptr<AVPixelBuffer>
  createSoftwareBufferCopyYuv420Planar(uint8_t *data,
      int size,
      ssize_t index,
      const AMediaCodecBufferInfo &info);

  std::shared_ptr<AVPixelBuffer>
  createSoftwareBufferCopyYuv420SemiPlanar(uint8_t *data,
      int size,
      ssize_t index,
      const AMediaCodecBufferInfo &info);

  std::shared_ptr<AVPixelBuffer>
  createSoftwareBufferCopyYuv420PackedSemiPlanar(uint8_t *data,
      int size,
      ssize_t index,
      const AMediaCodecBufferInfo &info);

  std::shared_ptr<AVPixelBuffer>
  dequeueAvailableOutputBuffer(const PixelFormat &formatType);
};

} // namespace coral

#endif // CORAL_CODEC_ANDROID_NATIVE_VIDEO_DECODER_H

