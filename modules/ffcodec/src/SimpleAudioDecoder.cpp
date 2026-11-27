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

#include "private/ffcodec/SimpleAudioDecoder.h"

#include <media/BlockBuffer.h>
#include <media/Types.h>

#include <minilog/log.h>

namespace coral {

SimpleAudioDecoder::SimpleAudioDecoder(
    int numInputBuffers,
    int numOutputBuffers,
    bool outputFloat) noexcept
    : SimpleDecoder(numInputBuffers, numOutputBuffers),
      outputFloat(outputFloat) {
  cachePacket = std::unique_ptr<AVPacket, AVPacketDeleter>(av_packet_alloc());
  cacheFrame = std::unique_ptr<AVFrame, AVFrameDeleter>(av_frame_alloc());
}

void SimpleAudioDecoder::configure(const std::shared_ptr<MediaFormat>& format) {
  codecName = FfmpegUtils::getCodecName(format->getMimeType());
  extraData = getExtraData(format->getMimeType(), format->csdList());
  auto sampleFormat = outputFloat ? AudioSampleFormat::kFloat : AudioSampleFormat::kInt16;
  outputConfig = AudioConfiguration(format->getSampleRate(), format->getChannelCount(), sampleFormat);
  outputBufferSize =
      outputFloat ? INITIAL_OUTPUT_BUFFER_SIZE_32BIT : INITIAL_OUTPUT_BUFFER_SIZE_16BIT;
  ffmpegInitialize(codecName,
      extraData,
      outputFloat,
      format->getSampleRate(),
      format->getChannelCount());
  SimpleDecoder::configure(format);
  mOutputFormat = format->buildUpon();
  mOutputFormat->setSampleFormat(sampleFormat);
}

void SimpleAudioDecoder::recycleBuffer(std::shared_ptr<AVSampleBuffer> outputBuffer) {
  SimpleDecoder::releaseOutputBuffer(outputBuffer);
}

std::shared_ptr<AVPacketBuffer> SimpleAudioDecoder::createInputBuffer() {
  return std::make_shared<AVPacketBuffer>(MediaType::kMediaTypeAudio);
}

std::shared_ptr<AVSampleBuffer> SimpleAudioDecoder::createOutputBuffer() {
  return std::make_shared<AVSampleBuffer>(this, outputConfig);
}

std::optional<Error>
SimpleAudioDecoder::decodeBuffer(const std::shared_ptr<AVPacketBuffer>& inputBuffer,
    const std::shared_ptr<AVSampleBuffer>& outputBuffer, bool reset) {

  if (reset) {
    resetDecoder();
    if (!mCodecContext) {
      MINILOG_ERROR("Failed to reset decoder");
      return Error("Failed to reset decoder");
    }
  }

  if (mCodecContext == nullptr || inputBuffer == nullptr || outputBuffer == nullptr) {
    return Error("Failed to find decoder or no sample to decode");
  }

  // copy data to packet
  auto packet = cachePacket.get();
  av_packet_unref(packet);
  packet->data = inputBuffer->array();
  packet->size = inputBuffer->size();
  // convert pts to decoder context's time base
  packet->dts = packet->pts =
      av_rescale_q(inputBuffer->timeUs(), AVRational{1, AV_TIME_BASE}, mCodecContext->time_base);
  packet->stream_index = inputBuffer->streamIndex();

  if (resampledBuffer == nullptr) {
    resampledBuffer = std::make_unique<uint8_t[]>(outputBufferSize + AV_INPUT_BUFFER_PADDING_SIZE);
  }
  int result = decodePacket(packet, resampledBuffer.get(), outputBufferSize);
  av_packet_unref(packet);
  if (result == CODEC_ERROR_OTHER) {
    return std::nullopt;
  } else if (result == CODEC_ERROR_INVALID_DATA) {
    outputBuffer->setFlags(AVBufferFlags::kDiscard);
  } else if (result == CODEC_ERROR_EOF) {
    outputBuffer->setFlags(AVBufferFlags::kEndOfStream);
  } else {
    outputBuffer->init(inputBuffer->timeUs(), result);
    auto outputData = outputBuffer->data();
    outputData->position(0);
    outputData->put(resampledBuffer.get(), result);
    outputData->limit(result);
  }
  return std::nullopt;
}

void SimpleAudioDecoder::onRelease() {
  mCodecContext.reset();
}

std::vector<uint8_t>
SimpleAudioDecoder::getExtraData(
    const std::string& mimeType, const std::vector<std::vector<uint8_t>>& initializationData) {
  if (mimeType == MimeTypes::AUDIO_AAC || mimeType == MimeTypes::AUDIO_OPUS) {
    return initializationData[0];
  }
  if (mimeType == MimeTypes::AUDIO_ALAC) {
    return getAlacExtraData(initializationData);
  }
  if (mimeType == MimeTypes::AUDIO_VORBIS) {
    return getVorbisExtraData(initializationData);
  }
  return {};
}

std::vector<uint8_t>
SimpleAudioDecoder::getAlacExtraData(const std::vector<std::vector<uint8_t>>& initializationData) {
  // FFmpeg's ALAC decoder expects an ALAC atom, which contains the ALAC "magic cookie", as extra
  // data. initializationData[0] contains only the magic cookie, and so we need to package it into
  // an ALAC atom. See:
  // https://ffmpeg.org/doxygen/0.6/alac_8c.html
  // https://github.com/macosforge/alac/blob/master/ALACMagicCookieDescription.txt
  auto const& magicCookie = initializationData[0];
  auto alacAtomLength = 12 + magicCookie.size();
  auto alacAtom = AVBlockBuffer(alacAtomLength);
  alacAtom.putInt(alacAtomLength);
  alacAtom.putInt(0x616c6163); // type=alac
  alacAtom.putInt(0); // version=0, flags=0
  auto pos = alacAtom.position();
  alacAtom.put(magicCookie.data(), pos, magicCookie.size());
  alacAtom.flip();

  auto result = std::vector<uint8_t>(alacAtom.limit());
  for (unsigned char& i: result) {
    i = alacAtom.get();
  }
  return result;
}

std::vector<uint8_t>
SimpleAudioDecoder::getVorbisExtraData(const std::vector<std::vector<uint8_t>>& initializationData) {
  auto const& header0 = initializationData[0];
  auto const& header1 = initializationData[1];
  auto extraData = std::vector<uint8_t>(header0.size() + header1.size() + 6);
  extraData[0] = (uint8_t)(header0.size() >> 8);
  extraData[1] = (uint8_t)(header0.size() & 0xFF);
  for (int i = 0; i < header0.size(); ++i) {
    extraData[i + 2] = header0[i];
  }
  extraData[header0.size() + 2] = 0;
  extraData[header0.size() + 3] = 0;
  extraData[header0.size() + 4] = (uint8_t)(header1.size() >> 8);
  extraData[header0.size() + 5] = (uint8_t)(header1.size() & 0xFF);
  for (int i = 0; i < header1.size(); ++i) {
    extraData[header0.size() + 6 + i] = header1[i];
  }
  return extraData;
}

void SimpleAudioDecoder::ffmpegInitialize(const std::string& codecName,
    const std::vector<uint8_t>& extraData,
    bool outputFloat,
    int rawSampleRate,
    int rawChannelCount) {

  auto codec = avcodec_find_decoder_by_name(codecName.data());
  auto context = createContext(codec,
      extraData.data(),
      extraData.size(),
      outputFloat,
      rawSampleRate,
      rawChannelCount);
  mCodecContext.reset();
  if (context) {
    mCodecContext.reset(context);
    if (mCodecContext->sample_rate == 0) {
      mCodecContext->sample_rate = rawSampleRate;
    }
    if (ffChannels(mCodecContext.get()) == 0) {
      ffSetChannels(mCodecContext.get(), rawChannelCount);
    }
  }
}

AVCodecContext* SimpleAudioDecoder::createContext(
    const AVCodec* codec,
    const uint8_t* initialData,
    int initialSize,
    bool outputFloat,
    int rawSampleRate,
    int rawChannelCount) const {
  auto context = avcodec_alloc_context3(codec);
  if (!context) {
    MINILOG_ERROR("Failed to allocate context.");
    return nullptr;
  }
  context->request_sample_fmt = outputFloat ? AV_SAMPLE_FMT_FLT : AV_SAMPLE_FMT_S16;
  if (initialData) {
    context->extradata_size = initialSize;
    context->extradata = (uint8_t*)av_malloc(initialSize + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!context->extradata) {
      releaseCodecContext(context);
      MINILOG_ERROR("Failed to allocate extradata.");
      return nullptr;
    }
    memcpy(context->extradata, initialData, initialSize);
  }
  if (context->codec_id == AV_CODEC_ID_PCM_MULAW
      || context->codec_id == AV_CODEC_ID_PCM_ALAW) {
    context->sample_rate = rawSampleRate;
    ffSetChannels(context, rawChannelCount);
  }
  context->err_recognition = AV_EF_IGNORE_ERR;
  int result = avcodec_open2(context, codec, nullptr);
  if (result < 0) {
    releaseCodecContext(context);
    return nullptr;
  }
  return context;
}

void SimpleAudioDecoder::resetDecoder() {
  if (!mCodecContext) {
    MINILOG_ERROR("Tried to reset without a context.");
    return;
  }

  auto requestOutputFloat = (mCodecContext->request_sample_fmt == AV_SAMPLE_FMT_FLT);
  auto codecId = mCodecContext->codec_id;
  if (codecId == AV_CODEC_ID_TRUEHD) {
    // Release and recreate the context if the codec is TrueHD.
    // TODO: Figure out why flushing doesn't work for this codec.
    mCodecContext.reset();
    auto const codec = avcodec_find_decoder(codecId);
    if (!codec) {
      MINILOG_ERROR("Unexpected error finding codec {}", avcodec_get_name(codecId));
      return;
    }
    mCodecContext.reset(createContext(codec, extraData.data(), extraData.size(), requestOutputFloat, -1, -1));
  }
  avcodec_flush_buffers(mCodecContext.get());
  if (mCodecContext->sample_rate == 0) {
    mCodecContext->sample_rate = outputConfig.sampleRate;
  }
  if (ffChannels(mCodecContext.get()) == 0) {
    ffSetChannels(mCodecContext.get(), outputConfig.channelCount);
  }
}

int SimpleAudioDecoder::decodePacket(AVPacket* inputPacket,
    uint8_t* outputBuffer,
    int outputSize) {
  int result;

  auto context = mCodecContext.get();

  // Sends audio packet to decode
  result = avcodec_send_packet(context, inputPacket);
  if (result) {
    MINILOG_ERROR("Failed to call avcodec_send_packet: {}", ffErrorString(result));
    return result == AVERROR_INVALIDDATA ? CODEC_ERROR_INVALID_DATA
                                         : CODEC_ERROR_OTHER;
  }

  // receives audio data and resample
  int outSize = 0;
  while (true) {
    result = avcodec_receive_frame(context, cacheFrame.get());
    if (result) {
      av_frame_unref(cacheFrame.get());
      if (result == AVERROR(EAGAIN)) {
        break;
      } else if (result == AVERROR_EOF) {
        return CODEC_ERROR_EOF;
      }
      MINILOG_ERROR("Failed to call avcodec_receive_frame: {}", ffErrorString(result));
      return CODEC_ERROR_INVALID_DATA;
    }

    // prepares the resample context
    SwrContext* resampleContext;
    if (context->opaque != nullptr) {
      resampleContext = (SwrContext*)context->opaque;
    } else {
      resampleContext = swr_alloc();
      ffSetSwrChannelLayout(resampleContext, "in_chlayout", ffChannels(context));
      ffSetSwrChannelLayout(resampleContext, "out_chlayout", ffChannels(context));
      av_opt_set_int(resampleContext, "in_sample_rate", context->sample_rate, 0);
      av_opt_set_int(resampleContext, "out_sample_rate", (int64_t)context->sample_rate, 0);
      av_opt_set_int(resampleContext, "in_sample_fmt", context->sample_fmt, 0);
      // The output format is always the requested format.
      av_opt_set_int(resampleContext, "out_sample_fmt", context->request_sample_fmt, 0);
      result = swr_init(resampleContext);
      if (result < 0) {
        MINILOG_ERROR("Failed to call swr_init: {}", ffErrorString(result));
        av_frame_unref(cacheFrame.get());
        swr_free(&resampleContext);
        return CODEC_ERROR_INVALID_DATA;
      }
      context->opaque = resampleContext;
    }

    int resample_nb_samples = (int)av_rescale_rnd(
        swr_get_delay(resampleContext, cacheFrame->sample_rate) +
        cacheFrame->nb_samples,
        context->sample_rate, cacheFrame->sample_rate, AV_ROUND_INF);
    int resampledBufferSize = av_samples_get_buffer_size(
        nullptr, ffChannels(context), resample_nb_samples, context->request_sample_fmt, 0);
    if (outSize + resampledBufferSize > outputSize) {
      av_frame_unref(cacheFrame.get());
      MINILOG_ERROR("Output buffer size {} too small for output data {}", outputSize, (outSize + resampledBufferSize));
      return CODEC_ERROR_INVALID_DATA;
    }

    // resamples audio data
    result = swr_convert(resampleContext, &outputBuffer, resampledBufferSize,
        (const uint8_t**)cacheFrame->data,
        cacheFrame->nb_samples);
    av_frame_unref(cacheFrame.get());
    if (result < 0) {
      MINILOG_ERROR("swr_convert error: {}", ffErrorString(result));
      return CODEC_ERROR_INVALID_DATA;
    }

    // checks if resample context has any available data
    int available = swr_get_out_samples(resampleContext, 0);
    if (available != 0) {
      MINILOG_ERROR("Expected no samples remaining after resampling, but found {}", available);
      return CODEC_ERROR_INVALID_DATA;
    }

    outputBuffer += resampledBufferSize;
    outSize += resampledBufferSize;
  }

  return outSize;
}

void SimpleAudioDecoder::releaseCodecContext(AVCodecContext* context) {
  if (!context) {
    return;
  }
  SwrContext* swrContext;
  if ((swrContext = (SwrContext*)context->opaque)) {
    swr_free(&swrContext);
    context->opaque = nullptr;
  }
  avcodec_free_context(&context);
}

} // namespace coral
