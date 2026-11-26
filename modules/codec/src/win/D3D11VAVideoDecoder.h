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

#ifndef CORAL_CODEC_D3D11VA_VIDEO_DECODER_H
#define CORAL_CODEC_D3D11VA_VIDEO_DECODER_H

#if defined(WINDOWS) || defined(WIN32)

#include <codec/HardwareVideo.h>
#include <codec/MediaCodec.h>

#include <media/ColorInfo.h>
#include <media/Configuration.h>
#include <media/Definitions.h>
#include <media/Format.h>
#include <media/Types.h>

#include "H264DxvaParser.h"

#include <d3d11.h>
#include <dxva.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace coral {

class D3D11VAHardwarePixelBuffer;

/**
 * The Windows video decoder built on Direct3D 11 Video Acceleration (D3D11VA).
 *
 * The platform Media Foundation decoder is a transform whose output surface it owns, and a transform
 * exposes no completion signal for the driver's write into that surface: a renderer which samples the
 * surface straight after `ProcessOutput` can read a picture the GPU is still writing, which is the
 * tearing the Media Foundation path shows. D3D11VA puts the decode pipeline in the caller's hands: the
 * decoder creates the output texture array, binds each picture view itself, and knows exactly when the
 * `DecoderEndFrame` submission has completed. It therefore hands out frames whose GPU write is
 * finished, which is what the zero-copy EffectEngine input needs.
 *
 * The decoder is a synchronous component. The H.264 access units are parsed by
 * {@link d3d11va::parseSliceHeader} into the DXVA picture parameters the driver consumes, the driver
 * decodes the slice data, and the host maintains the decoded picture buffer (DPB): reference marking,
 * the sliding window, output reordering and the texture slot the next picture is decoded into. The
 * decoded frames are cached internally like the Media Foundation decoder caches its pending frames, so
 * a caller that holds the frame it renders does not stall the decode of the following pictures.
 *
 * The decoder needs a {@link HardwareVideoSurface} with an `ID3D11Device`. A format without a surface,
 * a codec other than H.264, or a device without the decode profile falls back to the Media Foundation
 * decoder, so the factory's contract is unchanged: the caller always gets a working `VideoDecoder`.
 *
 * `MediaFormat::processFormat` is honoured: `kFormatBGRA` asks the decoder to convert the decoded NV12
 * picture to a packed BGRA texture with the device's video processor. That keeps a consumer which
 * samples one RGBA texture zero-copy; the default is the native NV12 layout.
 */
class D3D11VAVideoDecoder final : public VideoDecoder, public AVPixelBuffer::Owner<AVPixelBuffer> {
  public:
  D3D11VAVideoDecoder();

  ~D3D11VAVideoDecoder() override;

  // Makes the single argument overload of the base class visible.
  using VideoDecoder::configure;

  bool configure(const std::shared_ptr<MediaFormat>& format, const std::shared_ptr<VideoSurface>& surface) override;

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

  [[nodiscard]] const std::shared_ptr<MediaFormat>& getOutputFormat() const override { return mOutputFormat; }

  [[nodiscard]] const char* getName() const override { return "D3D11VAVideoDecoder"; }

  /** Whether this decoder can decode @p type through D3D11VA. */
  [[nodiscard]] static bool isSupported(VideoCodecType type);

  private:
  /** One output view of the decode texture array. */
  struct DecodeSlot {
    Microsoft::WRL::ComPtr<ID3D11VideoDecoderOutputView> view;
    /** Pictures of the DPB which name this slice. */
    int decoderHolds = 0;
    /** Frames handed to the caller which were not released yet. */
    int consumerHolds = 0;

    [[nodiscard]] bool isFree() const { return decoderHolds == 0 && consumerHolds == 0; }
  };

  /** One texture of the packed output ring used by `processFormat == kFormatBGRA`. */
  struct ConvertedSlot {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> view;
    int consumerHolds = 0;
  };

  /** One picture of the decoder's DPB. */
  struct Picture {
    int slot = -1;
    uint32_t frameNum = 0;
    uint32_t frameNumWrap = 0;
    int topFieldOrderCnt = 0;
    int bottomFieldOrderCnt = 0;
    bool shortTermReference = false;
    bool longTermReference = false;
    uint32_t longTermPicId = 0;
    bool outputPending = true;
    bool outputDone = false;
    int64_t timeUs = Definitions::kTimeUnset;
    bool keyFrame = false;
  };

  // ------------------------------------------------------------------------------------------------
  // Configuration
  // ------------------------------------------------------------------------------------------------
  bool configureD3D11VA(const std::shared_ptr<MediaFormat>& format, const std::shared_ptr<VideoSurface>& surface);

  /** Uses the Media Foundation decoder for a stream this front-end cannot decode. */
  bool configureFallback(const std::shared_ptr<MediaFormat>& format, const std::shared_ptr<VideoSurface>& surface);

  /** Parses the codec specific data of the track into {@link mSpsMap} and {@link mPpsMap}. */
  bool parseCodecSpecificData();

  /** Selects the decoder profile and its configuration on the caller's device. */
  bool selectDecoderProfile();

  /** Creates the decode texture array, its output views and the video decoder. */
  bool createDecodeResources();

  /** Creates the packed output ring and the video processor. */
  bool createConversionResources();

  /** Creates the codec from the parameter sets discovered so far, when they are complete. */
  bool ensureCodec();

  /** Releases every Direct3D 11 object and the picture buffer of a failed configuration. */
  void releaseD3D11VAState();

  // ------------------------------------------------------------------------------------------------
  // Decoding
  // ------------------------------------------------------------------------------------------------
  bool decodeAccessUnit(const uint8_t* data, size_t size, int64_t timeUs, bool keyFrame);

  bool decodePicture(const std::vector<d3d11va::NalUnit>& slices, int64_t timeUs, bool keyFrame);

  /** Creates the Media Foundation fallback and feeds it the access unit which decided the fallback. */
  bool switchToFallback(const uint8_t* data, size_t size, int64_t timeUs, bool keyFrame);

  bool submitFrame(DXVA_PicParams_H264& pictureParameters, const std::vector<d3d11va::NalUnit>& slices, int slot);

  /** Returns the display order count the decoder may have to reorder, from the SPS or the VUI. */
  [[nodiscard]] int reorderDepth() const;

  /** Hands the frames whose presentation order is settled to {@link mPendingFrames}. */
  void bumpOutput(bool force);

  /** Emits one picture: wraps the decoded slice, or converts it when `processFormat` asked for it. */
  bool emitPicture(const std::shared_ptr<Picture>& picture);

  bool convertPicture(const std::shared_ptr<Picture>& picture, std::shared_ptr<AVPixelBuffer>& output);

  /** Waits until the GPU finished the commands submitted so far on the device context. */
  bool waitForGpu();

  std::shared_ptr<AVPixelBuffer> createEndOfStreamBuffer();

  /** Releases the decode slot a picture held; the consumer may still hold a frame of it. */
  void releasePictureSlot(const std::shared_ptr<Picture>& picture);

  /** Releases the decoder side of every picture, e.g. on flush or release. */
  void resetPictureBuffer();

  // ------------------------------------------------------------------------------------------------
  // Reference picture management
  // ------------------------------------------------------------------------------------------------
  void computePictureOrderCount(const d3d11va::Sps& sps, const d3d11va::Pps& pps, const d3d11va::SliceHeader& header,
          bool idr, int& topFieldOrderCnt, int& bottomFieldOrderCnt, uint32_t& frameNumOffset);

  void fillReferenceLists(const d3d11va::Sps& sps, DXVA_PicParams_H264& parameters, int currentSlot,
          const d3d11va::SliceHeader& header);

  void applyReferenceMarking(const d3d11va::Sps& sps, const d3d11va::Pps& pps, const d3d11va::SliceHeader& header,
          bool idr, const std::shared_ptr<Picture>& picture);

  void dropShortTermReference(const std::shared_ptr<Picture>& picture);

  void dropLongTermReference(const std::shared_ptr<Picture>& picture);

  std::shared_ptr<Picture> findShortTermReference(uint32_t frameNumWrap);

  std::shared_ptr<Picture> findLongTermReference(uint32_t longTermPicNum);

  // ------------------------------------------------------------------------------------------------
  // Helpers
  // ------------------------------------------------------------------------------------------------
  int allocateDecodeSlot();

  int allocateConvertedSlot();

  [[nodiscard]] bool isAtLeastOutputStartTime(int64_t timeUs) const {
    return mOutputStartTimeUs == Definitions::kTimeUnset || timeUs == Definitions::kTimeUnset ||
           timeUs >= mOutputStartTimeUs;
  }

  /** Stores a `D3D11_VIDEO_DECODER_BUFFER_DESC` for one submitted buffer. */
  static void fillBufferDesc(D3D11_VIDEO_DECODER_BUFFER_DESC& description, UINT type, UINT size, UINT macroblocks);

  // ------------------------------------------------------------------------------------------------
  // State
  // ------------------------------------------------------------------------------------------------
  std::shared_ptr<MediaFormat> mInputFormat;
  std::shared_ptr<MediaFormat> mOutputFormat;
  std::shared_ptr<VideoSurface> mHardwareSurface;

  /** The fallback decoder, or null when this instance really decodes through D3D11VA. */
  std::unique_ptr<VideoDecoder> mFallback;

  Microsoft::WRL::ComPtr<ID3D11Device> mDevice;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> mContext;
  Microsoft::WRL::ComPtr<ID3D11VideoDevice> mVideoDevice;
  Microsoft::WRL::ComPtr<ID3D11VideoContext> mVideoContext;
  Microsoft::WRL::ComPtr<ID3D11VideoDecoder> mVideoDecoder;
  Microsoft::WRL::ComPtr<ID3D11Query> mCompletionQuery;

  GUID mDecoderGuid{};
  D3D11_VIDEO_DECODER_CONFIG mDecoderConfig{};

  /** The texture array the decode output views name; its slices are the DPB entries. */
  Microsoft::WRL::ComPtr<ID3D11Texture2D> mDecodeTexture;
  std::vector<DecodeSlot> mDecodeSlots;

  /** The packed output ring, only allocated when `processFormat` requests BGRA. */
  Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> mProcessorEnumerator;
  Microsoft::WRL::ComPtr<ID3D11VideoProcessor> mVideoProcessor;
  std::vector<ConvertedSlot> mConvertedSlots;
  bool mConvertOutput = false;

  std::map<int, d3d11va::Sps> mSpsMap;
  std::map<int, d3d11va::Pps> mPpsMap;
  d3d11va::Sps mSps;
  d3d11va::Pps mPps;
  bool mCodecReady = false;

  int mWidth = 0;
  int mHeight = 0;
  int mCodedWidth = 0;
  int mCodedHeight = 0;
  VideoConfiguration mOutputConfig;
  ColorInfo mOutputColorInfo;

  std::shared_ptr<AVPacketBuffer> mInputBuffer;
  bool mConfigured = false;
  bool mInputDequeued = false;
  bool mInputEndOfStream = false;
  bool mOutputEndOfStream = false;
  bool mDrainDone = false;
  bool mDiscardUntilIdr = false;

  std::deque<std::shared_ptr<AVPixelBuffer>> mPendingFrames;
  int64_t mOutputStartTimeUs = Definitions::kTimeUnset;

  /** The pictures the decoder still holds: references and frames waiting to be output. */
  std::vector<std::shared_ptr<Picture>> mPictures;
  /** The short-term reference pictures, newest first. */
  std::vector<std::shared_ptr<Picture>> mShortTermReferences;
  /** The long-term reference pictures by their long term picture id. */
  std::map<uint32_t, std::shared_ptr<Picture>> mLongTermReferences;

  /** The state the next picture order count is derived from. */
  uint32_t mPreviousFrameNum = 0;
  uint32_t mPreviousFrameNumOffset = 0;
  int mPreviousPicOrderCntMsb = 0;
  int mPreviousPicOrderCntLsb = 0;
  int mStatusReportFeedbackNumber = 1;
  int mReorderDepth = 1;

  /** The frames handed to the caller and not released yet. */
  std::atomic_int mOutstandingOutputCount{ 0 };
  /** Protects the slot counters, which the render thread also changes. */
  mutable std::mutex mSlotMutex;
};

} // namespace coral

#endif // defined(WINDOWS) || defined(WIN32)

#endif // CORAL_CODEC_D3D11VA_VIDEO_DECODER_H
