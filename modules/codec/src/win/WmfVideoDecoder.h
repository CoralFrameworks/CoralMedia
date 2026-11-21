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

#ifndef CORAL_CODEC_WMF_VIDEO_DECODER_H
#define CORAL_CODEC_WMF_VIDEO_DECODER_H

#include <codec/MediaCodec.h>

#include "WmfTransform.h"

#include <media/ColorInfo.h>
#include <media/Configuration.h>
#include <media/Format.h>

#include <d3d11.h>
#include <mfobjects.h>
#include <wrl/client.h>

#include <deque>
#include <memory>
#include <mutex>

namespace coral {

/**
 * The implementation of the VideoDecoder based on the Windows Media Foundation transforms, the
 * decoded frames are delivered in the NV12 or I420 pixel format.
 */
class WmfVideoDecoder final : public VideoDecoder,
                              public AVPixelBuffer::Owner<AVPixelBuffer> {
public:
  WmfVideoDecoder();

  ~WmfVideoDecoder() override;

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

  /**
   * Takes the frames the decoder can give into {@code mPendingFrames}, so that nothing is left in the
   * decoder while the caller holds the frame it is about to render.
   */
  void fillPendingFrames();

  /** Returns the next frame the decoder produces, or null when it has none ready. */
  std::shared_ptr<AVPixelBuffer> dequeueDecodedFrame();

  void releaseOutputBuffer(const std::shared_ptr<AVPixelBuffer>& outputBuffer, bool render) override;

  void signalEndOfInputStream() override;

  void setOutputStartTimeUs(int64_t outputStartTimeUs) override;

  void recycleBuffer(std::shared_ptr<AVPixelBuffer> frameBuffer) override;

  [[nodiscard]] const std::shared_ptr<MediaFormat>& getOutputFormat() const override {
    return mOutputFormat;
  }

  [[nodiscard]] const char* getName() const override {
    return "WmfVideoDecoder";
  }

private:
  bool createCodec();

  bool createInputMediaType();

  bool negotiateOutputType(bool allowAvailableTypes);

  /**
   * Opens the Media Foundation device manager on the caller's GPU, when one was offered.
   *
   * @return false only for a failure that makes the decoder unusable; a device the MFT refuses is not one
   *         of them. The frames then arrive as CPU buffers, which every caller already handles, so a
   *         request for hardware output that the driver cannot honour degrades instead of failing.
   */
  bool createHardwareOutput();

  bool pushInputSamples();

  std::shared_ptr<AVPixelBuffer>
  createFrameBuffer(const Microsoft::WRL::ComPtr<IMFSample>& sample);

  /**
   * @return the frame as a `HardwarePixelBuffer`, or null when this sample is not a D3D11 texture.
   *
   * A decoder that accepted the device manager may still hand out a CPU sample for a frame (a stream
   * switch, a profile it cannot decode in hardware), and those fall back to the copy path.
   */
  std::shared_ptr<AVPixelBuffer>
  createHardwareFrameBuffer(const Microsoft::WRL::ComPtr<IMFSample>& sample);

  std::shared_ptr<AVPixelBuffer> createEndOfStreamBuffer();

  /**
   * Takes a recycled frame buffer out of the buffer pool, returns nullptr when the pool has none which
   * fits the current output configuration.
   *
   * <p>The buffer is cleared before it is handed out: the flags and the timestamp of the frame it held
   * before belong to that frame, not to the one which takes its place.
   */
  std::shared_ptr<AVPixelBuffer> obtainPooledBuffer();

  void discardBuffers();

  [[nodiscard]] bool isAtLeastOutputStartTime(const int64_t timeUs) const {
    return mOutputStartTimeUs == Definitions::kTimeUnset ||
           timeUs == Definitions::kTimeUnset ||
           timeUs >= mOutputStartTimeUs;
  }

  static int64_t sampleTimeUs(const Microsoft::WRL::ComPtr<IMFSample>& sample);

  bool mConfigured = false;
  bool mInputDequeued = false;
  bool mInputEndOfStream = false;
  bool mOutputEndOfStream = false;
  bool mDrainRequested = false;

  WmfTransformContext mTransform;
  GUID mInputSubType{};
  GUID mOutputSubType{};

  /**
   * The surface the caller offered when it wants decoded frames in GPU textures, or null.
   *
   * It is kept alive for the life of the decoder: the device it names owns the textures the decoder hands
   * out, and a frame the renderer still holds refers to one of them.
   */
  std::shared_ptr<VideoSurface> mHardwareSurface;
  /** The device manager the transform pulls textures through; null when the output is CPU frames. */
  Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> mDeviceManager;
  UINT mDeviceManagerToken = 0;
  /** True once the transform accepted the device manager, which is what makes a sample a texture. */
  bool mHardwareOutput = false;

  std::shared_ptr<MediaFormat> mInputFormat;
  std::shared_ptr<MediaFormat> mOutputFormat;
  VideoConfiguration mOutputConfig;
  /**
   * The color space the decoder describes the output with, which the renderers use to convert the
   * decoded frames to RGB.
   */
  ColorInfo mOutputColorInfo;

  int mWidth = 0;
  int mHeight = 0;

  std::shared_ptr<AVPacketBuffer> mInputBuffer;
  std::deque<Microsoft::WRL::ComPtr<IMFSample>> mPendingInputSamples;

  /**
   * The frames that were decoded and are waiting to be handed out.
   *
   * <p>A caller holds the frame it is about to render and does not ask for another one until it is time
   * to show it, so the frames the decoder can give in the meantime have to be taken out of it: a decoder
   * whose outputs are left in place buffers the frames of the stream for the reordering, and hands them
   * out in an order that is no longer the order of the stream, which a caller can only drop.
   */
  std::deque<std::shared_ptr<AVPixelBuffer>> mPendingFrames;

  int64_t mOutputStartTimeUs = Definitions::kTimeUnset;
  bool mWmfStarted = false;

  /**
   * The decoded frames which were handed to the caller and not released yet.
   *
   * <p>The transform reorders its output against the input it holds. Feeding it more input while
   * one of its output frames is still outstanding makes it produce frames with unrelated
   * timestamps, so the input is paused until that frame comes back to the decoder.
   */
  std::atomic_int mOutstandingOutputCount { 0 };

  /**
   * Hardware frames the renderer has finished with. `recycleBuffer` only appends to this queue; the
   * decoder thread drains it, so the render thread never mutates decoder state.
   */
  std::mutex mReturnedMutex;
  std::deque<std::shared_ptr<AVPixelBuffer>> mReturnedFrames;
  void drainReturnedFrames();

  /** The input samples submitted since the last frame the transform produced. */
  int mInputsSinceOutput = 0;

  /** Whether the codec private data is an Annex B byte stream which the first sample has to carry. */
  bool mPrependSequenceHeader = false;

  /** Whether the first key frame already received the codec private data. */
  bool mSequenceHeaderPrepended = false;

  mutable std::mutex mPendingLock;
  std::deque<std::shared_ptr<AVPixelBuffer>> mAvailableBuffers;
};

} // namespace coral

#endif //CORAL_CODEC_WMF_VIDEO_DECODER_H
