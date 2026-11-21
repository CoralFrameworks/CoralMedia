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

#include <ffcodec/FfmpegAudioEncoder.h>

#include <media/Format.h>
#include <media/Utilities.h>

#include <minilog/log.h>

#include <utility>

namespace coral {

FfmpegAudioEncoder::FfmpegAudioEncoder()
    : flushed(false),
      released(true),
      requestEof(false),
      framePosition(0),
      pCodecContext(nullptr),
      pAudioFifo(nullptr),
      availableOutputBufferCount(DEFAULT_MAX_BUFFER_COUNT) {
  av_register_all();
  mCacheFrame = std::unique_ptr<AVFrame, AVFrameDeleter>(av_frame_alloc());
  mResampleFrame = std::unique_ptr<AVFrame, AVFrameDeleter>(av_frame_alloc());
  mCachePacket = std::unique_ptr<AVPacket, AVPacketDeleter>(av_packet_alloc());
  availableOutputBuffers.resize(availableOutputBufferCount);
  for (auto i = 0; i < availableOutputBufferCount; ++i) {
    auto packetBuffer = std::make_shared<AVPacketBuffer>(this, MediaType::kMediaTypeAudio);
    availableOutputBuffers[i] = std::move(packetBuffer);
  }
}

FfmpegAudioEncoder::~FfmpegAudioEncoder() {
  mCacheFrame.reset();
  mResampleFrame.reset();
  mCachePacket.reset();
  mediaFormat.reset();
}

bool FfmpegAudioEncoder::configure(const std::shared_ptr<MediaFormat> &format) {
  mediaFormat = format;

  auto result = initEncoder();

  if (result) {
    released = false;
    requestEof = false;
    framePosition = 0;
  }
  return result;
}

void FfmpegAudioEncoder::start() {
  if (!pCodecContext) {
    MINILOG_ERROR("Failed to configure before start encoding.");
    return;
  }
  if (!thread) {
    thread = std::make_unique<std::thread>(&FfmpegAudioEncoder::run, this);
  }
}

void FfmpegAudioEncoder::stop() {
  {
    std::unique_lock<std::mutex> lock(pending_lock_);
    if (!released) {
      released = true;
      lock.unlock();
    }
    notify_cv_.notify_all();
  }
  if (thread->joinable()) {
    thread->join();
  } else {
    thread->detach();
  }
  thread.reset();
}

void FfmpegAudioEncoder::flush() {
  std::unique_lock<std::mutex> lock(pending_lock_);
  if (!released) {
    flushed = true;
    lock.unlock();
    notify_cv_.notify_all();
  } else {
    flushed = false;
    clearBufferQueuesLocked();
  }
}

void FfmpegAudioEncoder::release() {
  stop();
  pCodecContext.reset();
  clearBufferQueuesLocked();
}

std::shared_ptr<AVSampleBuffer> FfmpegAudioEncoder::dequeueInputBuffer() {
  std::unique_lock<std::mutex> lock(pending_lock_);
  if (availableInputBuffers.empty()) {
    auto config = AudioConfiguration(mediaFormat->getSampleRate(),
        mediaFormat->getChannelCount(),
        mediaFormat->getSampleFormat());
    return std::make_shared<AVSampleBuffer>(config);
  }
  auto buffer = std::move(availableInputBuffers.front());
  availableInputBuffers.pop_front();
  return buffer;
}

void FfmpegAudioEncoder::queueInputBuffer(std::shared_ptr<AVSampleBuffer> inputBuffer) {
  std::unique_lock<std::mutex> lock(pending_lock_);
  maybeNotifyError();
  if (inputBuffer != nullptr) {
    queuedInputBuffers.push_back(std::move(inputBuffer));
    maybeNotifyProcessLoop();
  }
}

std::shared_ptr<AVPacketBuffer> FfmpegAudioEncoder::dequeueOutputBuffer() {
  std::shared_ptr<AVPacketBuffer> outputBuffer = nullptr;
  std::unique_lock<std::mutex> lock(pending_lock_);
  maybeNotifyError();
  if (!queuedOutputBuffers.empty()) {
    outputBuffer = std::move(queuedOutputBuffers.front());
    queuedOutputBuffers.pop_front();
  }
  return outputBuffer;
}

void FfmpegAudioEncoder::releaseOutputBuffer(const std::shared_ptr<AVPacketBuffer> &outputBuffer, bool render) {
  if (outputBuffer) {
    outputBuffer->release();
  }
}

void FfmpegAudioEncoder::signalEndOfInputStream() {
  auto inputBuffer = dequeueInputBuffer();
  if (inputBuffer != nullptr) {
    inputBuffer->clear();
    inputBuffer->setFlags(AVBufferFlags::kEndOfStream);
    queueInputBuffer(inputBuffer);
  } else {
    std::unique_lock<std::mutex> lock(pending_lock_);
    requestEof = true;
    maybeNotifyProcessLoop();
  }
}

void FfmpegAudioEncoder::recycleBuffer(std::shared_ptr<AVSampleBuffer> inputBuffer) {
  std::unique_lock<std::mutex> lock(pending_lock_);
  if (inputBuffer != nullptr) {
    inputBuffer->clear();
    availableInputBuffers.push_back(std::move(inputBuffer));
    maybeNotifyProcessLoop();
  }
}

void FfmpegAudioEncoder::recycleBuffer(std::shared_ptr<AVPacketBuffer> packetBuffer) {
  std::unique_lock<std::mutex> lock(pending_lock_);
  packetBuffer->clear();
  availableOutputBuffers[availableOutputBufferCount++] = std::move(packetBuffer);
  maybeNotifyProcessLoop();
}

bool FfmpegAudioEncoder::encode() {
  std::shared_ptr<AVSampleBuffer> inputBuffer;
  bool flushEncoder;
  {
    std::unique_lock<std::mutex> lock(pending_lock_);
    while (!released && !canProcessBuffer()) {
      notify_cv_.wait(lock);
    }
    if (released) {
      return false;
    }
    inputBuffer = queuedInputBuffers.front();
    queuedInputBuffers.pop_front();
    flushEncoder = flushed;
    flushed = false;
  }

  if (flushEncoder) {
    std::unique_lock<std::mutex> lock(pending_lock_);
    clearBufferQueuesLocked();
    if (pCodecContext) {
      avcodec_flush_buffers(pCodecContext.get());
    }
    framePosition = 0;
  }

  // discard frame
  if (inputBuffer->isDiscard()) {
    inputBuffer->release();
    return true;
  }

  // queue end of stream frame
  if (inputBuffer->isEndOfStream()) {
    inputBuffer->release();
    auto outputBuffer = dequeueAvailableOutputBuffer();
    outputBuffer->clear();
    outputBuffer->setFlags(AVBufferFlags::kEndOfStream);
    queueOutputBuffer(std::move(outputBuffer));
    return true;
  }

  if (inputBuffer->size() > 0) {
    auto sampleFmt = FfmpegUtils::convertToSampleFormat(mediaFormat->getSampleFormat());
    auto nb_samples =
        (int)(
            inputBuffer->size() /
                (av_get_bytes_per_sample(sampleFmt) * mediaFormat->getChannelCount()));
    auto ret = av_samples_fill_arrays(
        mCacheFrame->data, mCacheFrame->linesize, inputBuffer->array(),
        mediaFormat->getChannelCount(), nb_samples, sampleFmt, 0);
    mCacheFrame->sample_rate = mediaFormat->getSampleRate();
    mCacheFrame->channels = mediaFormat->getChannelCount();
    mCacheFrame->format = sampleFmt;
    mCacheFrame->nb_samples = nb_samples;
    if (sampleFmt != pCodecContext->sample_fmt) {
      convertAndEncodeFrame(mCacheFrame.get());
    } else {
      mCacheFrame->pts = framePosition;
      framePosition += mCacheFrame->nb_samples;
      encodeFrame(mCacheFrame.get());
      av_frame_unref(mCacheFrame.get());
    }
  }
  inputBuffer->release();
  return true;
}

bool FfmpegAudioEncoder::initEncoder() {
  int ret;
  auto mimeType = mediaFormat->getMimeType();
  if (mimeType.empty()) {
    MINILOG_ERROR("Failed to find audio mime to configure audio encoder.");
    return false;
  }

  auto codec_id = FfmpegUtils::convertToCodecID(mimeType);
  if (codec_id == AV_CODEC_ID_NONE) {
    MINILOG_ERROR("Failed to init: codec id is {}, mimeType: {}", avcodec_get_name(AV_CODEC_ID_NONE), mimeType);
    return false;
  }

  auto codec = avcodec_find_encoder(codec_id);
  if (!codec) {
    MINILOG_ERROR("Failed to find encoder: {}", avcodec_get_name(codec_id));
    return false;
  }

  pCodecContext.reset(avcodec_alloc_context3(codec));
  if (!pCodecContext) {
    MINILOG_ERROR("Failed to allocate context.");
    return false;
  }

  pCodecContext->sample_rate = mediaFormat->getSampleRate();
  pCodecContext->channels = mediaFormat->getChannelCount();
  pCodecContext->bit_rate = Definitions::DEFAULT_AUDIO_BITRATE;
  pCodecContext->channel_layout =
      av_get_default_channel_layout(pCodecContext->channels);
  pCodecContext->sample_fmt = codec->sample_fmts[0];
  pCodecContext->time_base = AVRational{1, pCodecContext->sample_rate};
  pCodecContext->err_recognition = AV_EF_IGNORE_ERR;

  // open audio encoder
  if ((ret = avcodec_open2(pCodecContext.get(), codec, nullptr)) < 0) {
    pCodecContext.reset();
    MINILOG_ERROR("Failed to open audio encoder, result: {}", av_err2str(ret));
    return false;
  }

  initResample();

  return true;
}

void FfmpegAudioEncoder::prepareFormat() {
  if (pCodecContext->bit_rate > 0) {
    mediaFormat->setAverageBitrate(pCodecContext->bit_rate);
  }
  auto outputBuffer = dequeueAvailableOutputBuffer();
  outputBuffer->clear();
  outputBuffer->setFormat(getOutputFormat());
  queueOutputBuffer(std::move(outputBuffer));
}

void FfmpegAudioEncoder::prepareCodecConfig() {
  if (pCodecContext != nullptr
      && pCodecContext->extradata != nullptr
      && pCodecContext->extradata_size > 0) {
    auto outputBuffer = dequeueAvailableOutputBuffer();
    outputBuffer->clear();
    outputBuffer->setFlags(AVBufferFlags::kCodecConfig);
    outputBuffer->timeUs(0);
    auto outputData = outputBuffer->data();
    outputData->position(0);
    outputData->put(pCodecContext->extradata, pCodecContext->extradata_size);
    outputData->flip();
    queueOutputBuffer(std::move(outputBuffer));
  }
}

void FfmpegAudioEncoder::clearBufferQueuesLocked() {
  while (!queuedInputBuffers.empty()) {
    queuedInputBuffers.front()->release();
    queuedInputBuffers.pop_front();
  }
  while (!queuedOutputBuffers.empty()) {
    queuedOutputBuffers.front()->release();
    queuedOutputBuffers.pop_front();
  }
  if (pAudioFifo && av_audio_fifo_size(pAudioFifo) > 0) {
    av_audio_fifo_drain(pAudioFifo, av_audio_fifo_size(pAudioFifo));
  }
}

bool FfmpegAudioEncoder::readFifoAndEncode() {
  const int frame_size = FFMIN(av_audio_fifo_size(pAudioFifo), pCodecContext->frame_size);

  /* Initialize temporary storage for one output frame. */
  auto outputFrame = mResampleFrame.get();
  if (!initOutputFrame(&outputFrame, pCodecContext.get(), frame_size)) {
    MINILOG_ERROR("Failed to init output frame");
    return false;
  }

  /* Read as many samples from the FIFO buffer as required to fill the frame.
   * The samples are stored in the frame temporarily. */
  if (av_audio_fifo_read(pAudioFifo, (void **)outputFrame->data, frame_size) < frame_size) {
    MINILOG_ERROR("Could not read data from FIFO");
    av_frame_unref(outputFrame);
    return false;
  }

  outputFrame->pts = framePosition;
  framePosition += outputFrame->nb_samples;

  /* Encode one frame worth of audio samples. */
  return encodeFrame(outputFrame);
}

bool FfmpegAudioEncoder::encodeFrame(AVFrame *frame) {
  int error;
  /* Send the audio frame stored in the temporary packet to the encoder.
   * The output audio stream encoder is used to do this. */
  error = avcodec_send_frame(pCodecContext.get(), frame);
  if (error < 0 && error != AVERROR_EOF) {
    MINILOG_ERROR("Failed to call avcodec_send_frame: {}", av_err2str(error));
    return false;
  }

  /* Receive one encoded frame from the encoder. */
  auto packet = mCachePacket.get();
  av_packet_unref(packet);
  error = avcodec_receive_packet(pCodecContext.get(), packet);
  if (error == AVERROR(EAGAIN)) {
    return true;
  } else if (error == AVERROR_EOF) {
    auto outputBuffer = dequeueAvailableOutputBuffer();
    outputBuffer->clear();
    outputBuffer->setFlags(AVBufferFlags::kEndOfStream);
    queueOutputBuffer(std::move(outputBuffer));
    return true;
  } else if (error < 0) {
    MINILOG_ERROR("Failed to call avcodec_receive_packet: ｛｝", av_err2str(error));
    return false;
  } else {
    auto buffer = dequeueAvailableOutputBuffer();
    buffer->clear();
    if (packet->flags & AV_PKT_FLAG_KEY) {
      buffer->setFlags(AVBufferFlags::kKeyFrame);
    }
    auto timeUs = av_rescale_q(packet->pts, pCodecContext->time_base, AV_TIME_BASE_Q);
    buffer->timeUs(timeUs);
    auto outputBuffer = buffer->data();
    outputBuffer->position(0);
    outputBuffer->put(packet->data, packet->size);
    outputBuffer->flip();
    queueOutputBuffer(std::move(buffer));
    return true;
  }
}

bool FfmpegAudioEncoder::convertAndEncodeFrame(AVFrame *frame) {
  /* Temporary storage for the converted input samples. */
  uint8_t **convertedSamples = nullptr;

  /* Initialize the temporary storage for the converted input samples. */
  auto result = initConvertedSamples(&convertedSamples, pCodecContext.get(), frame->nb_samples);
  if (!result) {
    releaseBuffer(convertedSamples);
    MINILOG_ERROR("Failed to init converted samples.");
    return false;
  }

  /**
   * Convert the input samples to the desired output sample format.
   * This requires a temporary storage provided by converted_input_samples.
   */
  if (!convertSamples((SwrContext *)pCodecContext->opaque,
      (const uint8_t **)frame->extended_data,
      convertedSamples, frame->nb_samples)) {
    releaseBuffer(convertedSamples);
    MINILOG_ERROR("Failed to convert samples.");
    return false;
  }

  /* Add the converted input samples to the FIFO buffer for later processing. */
  result = addSampleToFifo(pAudioFifo, convertedSamples, frame->nb_samples);
  releaseBuffer(convertedSamples);
  if (!result) {
    MINILOG_ERROR("Failed to add samples to fifo.");
    return false;
  }

  /**
   * If we have enough samples for the encoder, we encode them.
   * At the end of the file, we pass the remaining samples to the encoder.
   */
  while (av_audio_fifo_size(pAudioFifo) >= pCodecContext->frame_size
      || (requestEof && av_audio_fifo_size(pAudioFifo) > 0)) {
    if (!readFifoAndEncode()) {
      break;
    }
  }

  return result;
}

bool FfmpegAudioEncoder::initResample() {
  auto sampleFmt = FfmpegUtils::convertToSampleFormat(mediaFormat->getSampleFormat());
  if (sampleFmt != pCodecContext->sample_fmt) {
    auto resampleContext = swr_alloc();
    av_opt_set_int(resampleContext, "in_channel_layout",
        av_get_default_channel_layout(mediaFormat->getChannelCount()), 0);
    av_opt_set_int(resampleContext, "out_channel_layout",
        (int64_t)pCodecContext->channel_layout, 0);
    av_opt_set_int(resampleContext, "in_sample_rate", mediaFormat->getSampleRate(),
        0);
    av_opt_set_int(resampleContext, "out_sample_rate",
        (int64_t)pCodecContext->sample_rate, 0);
    av_opt_set_int(resampleContext, "in_sample_fmt", sampleFmt, 0);
    av_opt_set_int(resampleContext, "out_sample_fmt", pCodecContext->sample_fmt, 0);
    auto result = swr_init(resampleContext);
    if (result < 0) {
      swr_free(&resampleContext);
      MINILOG_ERROR("Failed to call swr_init: {}", av_err2str(result));
      return false;
    }
    pCodecContext->opaque = resampleContext;
    pAudioFifo = av_audio_fifo_alloc(pCodecContext->sample_fmt, pCodecContext->channels, 1);
  } else {
    pCodecContext->opaque = nullptr;
  }
  return true;
}

bool FfmpegAudioEncoder::initConvertedSamples(uint8_t ***converted_input_samples,
    AVCodecContext *codecContext,
    int frame_size) const {
  int error;

  /**
   * Allocate as many pointers as there are audio channels.
   * Each pointer will later point to the audio samples of the corresponding
   * channels (although it may be NULL for interleaved formats).
   */
  if (!(
      *converted_input_samples = (uint8_t**)calloc(
          codecContext->channels, sizeof(**converted_input_samples)))) {
    MINILOG_ERROR("Could not allocate converted input sample pointers.");
    return false;
  }

  /**
   * Allocate memory for the samples of all channels in one consecutive
   * block for convenience.
   */
  if ((
      error = av_samples_alloc(*converted_input_samples, nullptr,
          codecContext->channels, frame_size,
          codecContext->sample_fmt, 0)) < 0) {
    av_freep(&(*converted_input_samples)[0]);
    free(*converted_input_samples);
    MINILOG_ERROR("Could not allocate converted input samples (error {}).", av_err2str(error));
    return false;
  }
  return true;
}

bool FfmpegAudioEncoder::convertSamples(SwrContext *resampleContext,
    const uint8_t **inputData,
    uint8_t **convertedData,
    int frameSize) const {
  int error;

  /* Convert the samples using the resampler. */
  if ((error = swr_convert(resampleContext, convertedData, frameSize, inputData, frameSize)) < 0) {
    MINILOG_ERROR("Could not convert input samples (error {})", av_err2str(error));
    return false;
  }
  return true;
}

bool FfmpegAudioEncoder::addSampleToFifo(AVAudioFifo *fifo, uint8_t **convertedInputSamples, int frame_size) const {
  int error;

  /* Make the FIFO as large as it needs to be to hold both, the old and the new samples. */
  if ((error = av_audio_fifo_realloc(fifo, av_audio_fifo_size(fifo) + frame_size)) < 0) {
    MINILOG_ERROR("Could not reallocate FIFO");
    return false;
  }

  /* Store the new samples in the FIFO buffer. */
  if (av_audio_fifo_write(fifo, (void **)convertedInputSamples, frame_size) <
      frame_size) {
    MINILOG_ERROR("Could not write data to FIFO");
    return false;
  }
  return true;
}

bool FfmpegAudioEncoder::initOutputFrame(AVFrame **frame, AVCodecContext *codecContext, int frame_size) const {
  int error;

  av_frame_unref(*frame);

  /**
   * Set the frame's parameters, especially its size and format.
   * av_frame_get_buffer needs this to allocate memory for the
   * audio samples of the frame.
   * Default channel layouts based on the number of channels
   * are assumed for simplicity.
   */
  (*frame)->nb_samples = frame_size;
  (*frame)->channel_layout = codecContext->channel_layout;
  (*frame)->format = codecContext->sample_fmt;
  (*frame)->sample_rate = codecContext->sample_rate;

  /**
   * Allocate the samples of the created frame. This call will make
   * sure that the audio frame can hold as many samples as specified.
   */
  if ((error = av_frame_get_buffer(*frame, 0)) < 0) {
    MINILOG_ERROR("Could not allocate output frame samples (error {})", av_err2str(error));
    return false;
  }
  return true;
}

void FfmpegAudioEncoder::releaseCodecContext(AVCodecContext *codecContext) {
  if (codecContext) {
    SwrContext *swrContext;
    if ((swrContext = (SwrContext *)codecContext->opaque)) {
      swr_free(&swrContext);
      codecContext->opaque = nullptr;
    }
    avcodec_free_context(&codecContext);
  }
}

void FfmpegAudioEncoder::releaseBuffer(uint8_t **buffer) {
  if (buffer) {
    av_freep(&buffer[0]);
    free(buffer);
  }
}

} // namespace coral
