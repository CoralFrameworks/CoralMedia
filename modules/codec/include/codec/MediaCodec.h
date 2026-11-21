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

#ifndef CORAL_CODEC_MEDIA_CODEC_H
#define CORAL_CODEC_MEDIA_CODEC_H

#include <media/Format.h>
#include <media/PacketBuffer.h>
#include <media/SampleBuffer.h>
#include <media/PixelBuffer.h>
#include <media/SurfaceFrameSource.h>

#include <memory>

namespace coral {

/**
 * An AVMediaCodec interface for decoding or encoding.
 *
 * @param <InputBuffer> The type of buffer input to the AVMediaCodec.
 * @param <OutputBuffer> The type of buffer output from the AVMediaCodec.
 */
template<typename InputBuffer, typename OutputBuffer>
class AVMediaCodec {
public:
  virtual ~AVMediaCodec() = default;

  /**
   * Configures a component.
   *
   * @param format The format of the input data (decoder) or the desired format of the output data (encoder).
   *               Passing null as {@param format} is equivalent to passing an empty {@param format}.
   */
  virtual bool configure(const std::shared_ptr<MediaFormat> &format) = 0;

  /**
   * After successfully configuring the component, call {@link start}.
   *
   * Call method {@link start} also if the codec is configured in asynchronous mode, and it has just been
   * flushed, to resume requesting input buffers.
   */
  virtual void start() = 0;

  /**
   * Finish the decode/encode session, note that the codec instance remains active and ready to
   * be {@link start}ed again.
   *
   * To ensure that it is available to other client call {@link release} and don't just rely on
   * garbage collection to eventually do this for you.
   */
  virtual void stop() = 0;

  /**
   * Flushes the AVMediaCodec Ownership of dequeued input buffers is returned to the AVMediaCodec
   *
   * The caller is still responsible for releasing any dequeued output buffers.
   */
  virtual void flush() = 0;

  /**
   * Releases the AVMediaCodec Must be called when the Codec is no longer needed.
   */
  virtual void release() = 0;

  /**
   * Dequeues the next input buffer to be filled and queued to the AVMediaCodec
   *
   * @return The input buffer, which will have been cleared, or null if a buffer isn't available.
   */
  virtual std::shared_ptr<InputBuffer> dequeueInputBuffer() = 0;

  /**
   * Queues an input buffer to the AVMediaCodec
   *
   * @param inputBuffer The input buffer.
   */
  virtual void queueInputBuffer(std::shared_ptr<InputBuffer> inputBuffer) = 0;

  /**
   * Dequeues the next output buffer from the AVMediaCodec
   *
   * @return The output buffer, or null if an output buffer isn't available.
   */
  virtual std::shared_ptr<OutputBuffer> dequeueOutputBuffer() = 0;

  /**
   * If you are done with a buffer, use this call to return the buffer to the codec
   * or to render it on the output surface or frame renderer. If you configured the
   * codec with an output surface, setting {@param render} to true will first
   * send the buffer to that output surface. The surface will release the buffer back
   * to the codec once it is no longer used/displayed.
   *
   * @param outputBuffer The client-owned output buffer previously returned from a call to
   *                     {@link dequeueOutputBuffer}.
   *
   * @param render       If a valid surface was specified when configuring the codec, passing
   *                     true renders this output buffer to the surface or frame renderer.
   */
  virtual void releaseOutputBuffer(const std::shared_ptr<OutputBuffer>& outputBuffer, bool render) = 0;

  /**
   * Signals end-of-stream on input.
   */
  virtual void signalEndOfInputStream() = 0;

  /**
   * Returns the output format
   */
  [[nodiscard]] virtual const std::shared_ptr<MediaFormat> &getOutputFormat() const = 0;

  /**
   * Returns the name of the AVMediaCodec
   */
  [[nodiscard]] virtual const char *getName() const = 0;
};

/**
 * An AVMediaDecoder interface for decoding audio or video.
 *
 * @tparam DecodedBuffer The decoded output buffer.
 */
template<typename DecodedBuffer>
class AVMediaDecoder : public AVMediaCodec<AVPacketBuffer, DecodedBuffer> {
public:
  /**
   * Sets the timestamp from which output buffers should be produced, in microseconds.
   *
   * Any decoded buffer with a timestamp less than {@code outputStartTimeUs} should be skipped by
   * the implementation and not made available via {@link dequeueOutputBuffer}.
   *
   * This method must only be called before {@link queueInputBuffer} queuing the first input
   * buffer initially or after {@link flush}.
   *
   * @param outputStartTimeUs The time from which output buffer should be produced, in microseconds.
   */
  virtual void setOutputStartTimeUs(int64_t outputStartTimeUs) = 0;

  [[nodiscard]] const char* getName() const override {
    return "AVMediaDecoder";
  }
};

/**
 * An AVMediaEncoder interface for encoding audio or video.
 *
 * @tparam DecodedBuffer the decoded input buffer.
 */
template<typename DecodedBuffer>
class AVMediaEncoder : public AVMediaCodec<DecodedBuffer, AVPacketBuffer> {
public:
  [[nodiscard]] const char* getName() const override {
    return "AVMediaEncoder";
  }
};

/**
 * Base class for decoding audio.
 */
using AudioDecoder = AVMediaDecoder<AVSampleBuffer>;

/**
 * Base class for encoding audio sample buffer.
 */
using AudioEncoder = AVMediaEncoder<AVSampleBuffer>;


/**
 * The interface of the VideoSurface
 */
class VideoSurface {
public:
  virtual ~VideoSurface() = default;

  /**
   * Gets the instance pointer of the surface
   */
  [[nodiscard]] virtual void *getInstance() const = 0;

  /**
   * Indicates whether the Surface is valid
   */
  [[nodiscard]] virtual bool isValid() const = 0;

  /**
   * Tells a consumer-backed surface how large the frames are going to be.
   *
   * A `SurfaceTexture` starts with no useful default size, and some `MediaCodec` implementations refuse to
   * configure their output on it (`err -22`) unless it has one. The decoder knows the track size, so it hands
   * it over here before the codec is configured; surfaces that are a real window ignore it.
   */
  virtual void setDefaultBufferSize(int width, int height) {
    (void) width;
    (void) height;
  }

  /**
   * Whether the consumer of this surface samples its frames itself.
   *
   * The two consumers want opposite things from the decoder. A surface the platform presents (a
   * `SurfaceView`) shows a frame when the frame is due, so the decoder holds it until then. A surface a GPU
   * consumer samples (Android's `SurfaceTexture`) is the other way round: the frame has to be in the queue
   * before it can be sampled, so the decoder renders it as soon as it is decoded and the consumer picks the
   * frames up in order.
   *
   * The default answers for the platform's own presentation, which is what a surface with no such requirement
   * wants.
   */
  [[nodiscard]] virtual bool isSampledByConsumer() const {
    return false;
  }

  /**
   * The consumer that takes the frames this surface receives, or null when the platform presents them.
   *
   * A surface the platform presents has no such consumer: the frames go to a window. One whose frames are
   * sampled hands out a consumer, and the buffers the decoder returns carry it so that the conversion stage
   * takes the frames from the right surface -- a `SurfaceTexture` is a one-producer, one-consumer queue, so
   * "the device's surface" is not a thing that exists.
   */
  [[nodiscard]] virtual std::shared_ptr<SurfaceFrameSource> getFrameSource() const {
    return nullptr;
  }

  /**
   * Releases the surface
   */
  virtual void release() = 0;
};

/**
 * Base class for decoding video.
 */
class VideoDecoder : public AVMediaDecoder<AVPixelBuffer> {
public:
  ~VideoDecoder() override;

  /**
   * Configures a component.
   *
   * @param format The format of the input data (decoder) or the desired format of the output data (encoder).
   *               Passing {@code null} as {@code format} is equivalent to passing an empty {@link MediaFormat}.
   */
  bool configure(const std::shared_ptr<MediaFormat> &format) override {
    return configure(format, nullptr);
  }

  /**
   * Configures a component.
   *
   * @param format The format of the input data (decoder) or the desired format of the output data (encoder).
   *               Passing {@code null} as {@code format} is equivalent to passing an empty {@link MediaFormat}.
   * @param surface Specify a surface on which to render the output of this decoder.
   *                Pass {@code null} as {@code surface} if the codec does not generate raw video
   *                output (e.g. not a video decoder) and/or if you want to configure the codec for
   *                {@link ByteBuffer} output. Only supported on Android Platform.
   */
  virtual bool configure(const std::shared_ptr<MediaFormat> &format, const std::shared_ptr<VideoSurface> &surface) = 0;
};

/**
 * Base class for encoding video frame or surface.
 */
class VideoEncoder : public AVMediaEncoder<AVPixelBuffer> {
public:
  ~VideoEncoder() override;

  /**
   * Configures a component.
   *
   * @param format The format of the input data (decoder) or the desired format of the output data (encoder).
   *               Passing {@code null} as {@code format} is equivalent to passing an empty {@link MediaFormat}.
   */
  bool configure(const std::shared_ptr<MediaFormat> &format) override {
    return configure(format, nullptr);
  }

  /**
   * Configures a component.
   *
   * @param format The format of the input data (decoder) or the desired format of the output data (encoder).
   *               Passing null as {@param format} is equivalent to passing an empty {@param format}.
   *
   * @param surface Specify a surface on which to render the output of this decoder.
   *                Pass null as {@param surface} if the codec does not generate raw video
   *                output (e.g. not a video decoder) and/or if you want to configure the codec for
   *                {@link ByteBuffer} output. Only supported on Android Platform.
   */
  virtual bool configure(const std::shared_ptr<MediaFormat> &format, const std::shared_ptr<VideoSurface> &surface) = 0;
};

/**
 * The holder of the Surface
 */
class SurfaceHolder {
public:
  enum SurfaceType : uint8_t {
    /* Creates a java surface object */
    kSurfaceObj = 0,
    /* Creates an ANativeWindow*/
    kNativeWindow = 1,
  };

  explicit SurfaceHolder(const SurfaceType type) noexcept
      : mType(type) {
  }

  ~SurfaceHolder() = default;

  // The surface to holder
  std::shared_ptr<VideoSurface> surface;

  [[nodiscard]] SurfaceType surfaceType() const {
    return mType;
  }

private:
  SurfaceType mType;
};

} // namespace coral

#endif //CORAL_CODEC_MEDIA_CODEC_H

