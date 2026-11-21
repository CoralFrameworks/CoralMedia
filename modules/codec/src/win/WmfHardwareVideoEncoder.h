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

#ifndef CORAL_WMF_HARDWARE_VIDEO_ENCODER_H
#define CORAL_WMF_HARDWARE_VIDEO_ENCODER_H

#include <codec/HardwareVideo.h>
#include <codec/MediaCodec.h>

#include <wrl/client.h>

#include <deque>
#include <memory>
#include <mutex>
#include <vector>

struct ID3D11Device;
struct ID3D11Texture2D;
struct IMFTransform;
struct IMFMediaEventGenerator;
struct IMFDXGIDeviceManager;
struct IMFSample;

namespace coral {

/**
 * A Windows hardware encoder (NVENC/AMF/Quick Sync through Media Foundation) fed by D3D11 textures.
 *
 * The encoder is an asynchronous MFT: it asks for input through `METransformNeedInput` events and
 * produces samples through `METransformHaveOutput`. Input arrives as a `HardwarePixelBuffer` - the
 * same abstraction a hardware decoder hands out - and is turned into an `IMFSample` with
 * `MFCreateDXGISurfaceBuffer`; the encoded packets are copied out by `dequeueOutputBuffer`.
 *
 * The caller must wait for the frame's effect fence before queueing the texture. No CPU pixel copy
 * happens in this class.
 */
class WmfHardwareVideoEncoder final : public VideoEncoder {
public:
  explicit WmfHardwareVideoEncoder(VideoCodecType codecType);

  ~WmfHardwareVideoEncoder() override;

  /** Whether a hardware MFT exists for @p codecType on this machine. */
  [[nodiscard]] static bool isSupported(VideoCodecType codecType);

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
    return "WmfHardwareVideoEncoder";
  }

private:
  bool configureTypes();
  bool refreshOutputFormat();
  bool queueHardwareTexture(void* texture, int64_t timeUs);
  bool pumpEvents(bool wait, uint32_t timeoutMilliseconds);
  bool drainOutputs();
  bool waitForInput(uint32_t timeoutMilliseconds);
  std::shared_ptr<AVPacketBuffer> createEndOfStreamBuffer();

  VideoCodecType mCodecType;
  Microsoft::WRL::ComPtr<ID3D11Device> mDevice;
  Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> mDeviceManager;
  Microsoft::WRL::ComPtr<IMFTransform> mTransform;
  Microsoft::WRL::ComPtr<IMFMediaEventGenerator> mEvents;

  std::shared_ptr<MediaFormat> mInputFormat;
  std::shared_ptr<MediaFormat> mOutputFormat;
  std::shared_ptr<AVPacketBuffer> mPacketBuffer;
  struct InputSlot {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    std::shared_ptr<HardwarePixelBuffer> buffer;
    bool inFlight = false;
  };
  std::vector<InputSlot> mInputSlots;
  std::deque<size_t> mInFlightSlots;
  std::deque<Microsoft::WRL::ComPtr<IMFSample>> mEncodedSamples;
  /**
   * Samples handed to the async MFT and not yet known to be consumed.
   *
   * The D3D11 surface buffer inside a sample references the slot's texture; the MFT may read it
   * after `ProcessInput` returns, so the sample has to stay alive until the next input request.
   */
  std::deque<Microsoft::WRL::ComPtr<IMFSample>> mPendingInputSamples;

  std::mutex mMutex;
  UINT32 mResetToken = 0;
  int mWidth = 0;
  int mHeight = 0;
  int mFrameRate = 30;
  int64_t mNextInputTimeUs = 0;
  bool mConfigured = false;
  bool mStarted = false;
  bool mNeedInput = false;
  bool mEndOfStream = false;
  bool mDrainComplete = false;
  bool mFormatReported = false;
  bool mWmfStarted = false;
};

} // namespace coral

#endif // CORAL_WMF_HARDWARE_VIDEO_ENCODER_H
