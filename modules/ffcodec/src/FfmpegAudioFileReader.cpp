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

#include <ffcodec/FfmpegAudioFileReader.h>

#include <media/Definitions.h>
#include <minilog/log.h>

namespace coral {

std::unique_ptr<FfmpegAudioFileReader>
FfmpegAudioFileReader::sniff(const std::string &inputUrl) {
  avformat_network_init();
  AVFormatContext *formatContext = nullptr;

  auto ret =
      avformat_open_input(&formatContext, inputUrl.c_str(), nullptr, nullptr);
  if (ret != 0) {
    MINILOG_ERROR("Failed to prepare AVFormatContext, use platform AudioFileDecoder: {}, error: {}",
        inputUrl,
        ffErrorString(ret));
    avformat_close_input(&formatContext);
    return nullptr;
  }

  if (formatContext == nullptr) {
    return nullptr;
  }

  ret = avformat_find_stream_info(formatContext, nullptr);
  if (ret < 0) {
    MINILOG_ERROR("Failed to call avformat_find_stream_info: {}", ffErrorString(ret));
    avformat_close_input(&formatContext);
    return nullptr;
  }
  return std::make_unique<FfmpegAudioFileReader>(formatContext);
}

FfmpegAudioFileReader::FfmpegAudioFileReader(AVFormatContext *formatContext) noexcept
    : mStreamIndex(Definitions::kInvalidValue),
      mOutputType(AudioSampleFormat::kDefault), mSampleRate(0),
      mChannelCount(0), mSampleLength(Definitions::kInvalidValue),
      mFramePosition(Definitions::kPositionUnset), pAudioFifo(nullptr) {
  avformat_network_init();
  pFormatContext.reset(formatContext);
  mCacheFrame = std::unique_ptr<AVFrame, AVFrameDeleter>(av_frame_alloc());
  mCachePacket = std::unique_ptr<AVPacket, AVPacketDeleter>(av_packet_alloc());
}

FfmpegAudioFileReader::~FfmpegAudioFileReader() {
  mCacheFrame.reset();
  mCachePacket.reset();
}

int64_t FfmpegAudioFileReader::getSampleLength() const {
  return mSampleLength;
}

int64_t FfmpegAudioFileReader::getSampleRate() const {
  return mSampleRate;
}

int32_t FfmpegAudioFileReader::getChannelCount() const {
  return mChannelCount;
}

int64_t FfmpegAudioFileReader::getFramePosition() const {
  return mFramePosition;
}

AudioSampleFormat FfmpegAudioFileReader::getSampleFormat() const {
  return mOutputType;
}

bool FfmpegAudioFileReader::open(Error **outError) {
  auto result = false;
  auto index = av_find_best_stream(pFormatContext.get(), AVMEDIA_TYPE_AUDIO, -1,
      -1, nullptr, 0);
  if (index < 0) {
    for (int i = 0; i < pFormatContext->nb_streams; ++i) {
      auto stream = pFormatContext->streams[i];
      auto codecpar = stream->codecpar;
      if (codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
        result = prepareDecoder(i, outError);
        break;
      }
    }
  } else {
    result = prepareDecoder(index, outError);
  }
  return result;
}

void FfmpegAudioFileReader::seekTo(int64_t framePosition) {
  if (!pFormatContext) {
    MINILOG_ERROR("Failed to seek position: not implementation of FormatContext!");
    return;
  }
  mFramePosition = std::min(framePosition, mSampleLength);
  auto stream = pFormatContext->streams[mStreamIndex];
  auto codecpar = stream->codecpar;
  auto position = av_rescale_q(framePosition, {1, codecpar->sample_rate},
      stream->time_base);
  if (pFormatContext->start_time != AV_NOPTS_VALUE) {
    position += av_rescale_q(pFormatContext->start_time, AVRational{1, AV_TIME_BASE},
        stream->time_base);
  }
  auto ret = av_seek_frame(pFormatContext.get(), mStreamIndex, position,
      AVSEEK_FLAG_BACKWARD);
  if (ret < 0) {
    MINILOG_ERROR("Failed to seek to frame position: {}", ffErrorString(ret));
  }
}

void FfmpegAudioFileReader::close() {
  pFormatContext.reset();
  pCodecContext.reset();
  pAudioFifo.reset();
  mStreamIndex = -1;
}

int64_t FfmpegAudioFileReader::readIntoBuffer(uint8_t *data, int64_t bufferSize, Error **outError) {
  int error;
  if (!pFormatContext || !pCodecContext) {
    return Definitions::kInvalidValue;
  }

  auto bytePerSample =
      av_get_bytes_per_sample(pCodecContext->request_sample_fmt);
  auto bytePerFrame = bytePerSample * ffChannels(pCodecContext.get());
  auto numFrames = bufferSize / bytePerFrame;

  auto packet = mCachePacket.get();
  av_packet_unref(packet);
  auto frame = mCacheFrame.get();
  av_frame_unref(frame);

  // read and decode audio data until the audio fifo is enough or end of stream
  while (!mReadEof && av_audio_fifo_size(pAudioFifo.get()) < numFrames) {
    // Read one audio frame from the input file into a temporary packet.
    error = av_read_frame(pFormatContext.get(), packet);
    if (error == AVERROR_EOF) {
      mReadEof = true;
      break;
    } else if (error < 0) {
      MINILOG_ERROR("Failed to read frame: {}", ffErrorString(error));
      break;
    }

    // un-reference the packet
    if (packet->stream_index != mStreamIndex) {
      av_packet_unref(packet);
      continue;
    }

    // Calculates the frame position
    mFramePosition = av_rescale_q(packet->pts, pCodecContext->time_base, {1, pCodecContext->sample_rate});
    mFramePosition = std::min(mSampleLength, mFramePosition);

    // Sends audio packet to decode
    error = avcodec_send_packet(pCodecContext.get(), packet);
    if (error) {
      handleError(outError, "Failed to decode audio compress buffer!");
      break;
    }

    // Receive one frame from the decoder.
    error = avcodec_receive_frame(pCodecContext.get(), frame);
    /* If the decoder asks for more data to be able to decode a frame,
     * return indicating that no data is present. */
    if (error == AVERROR(EAGAIN)) {
      av_packet_unref(packet);
      continue;
    } else if (error == AVERROR_EOF) {
      // If the end of the input file is reached, stop decoding.
      mReadEof = true;
      av_packet_unref(packet);
    } else if (error < 0) {
      MINILOG_ERROR("Failed to decode frame: {}", ffErrorString(error));
      av_packet_unref(packet);
    } else {
      if (!resampleAndStoreFrame(pCodecContext.get(), pAudioFifo.get(),
          frame)) {
        break;
      }
    }
  }

  // read interleaved audio to output buffer
  if (av_audio_fifo_size(pAudioFifo.get()) > 0) {
    auto frameSize = FFMIN(av_audio_fifo_size(pAudioFifo.get()), numFrames);
    auto framesRead =
        av_audio_fifo_read(pAudioFifo.get(), (void **)&data, frameSize);
    if (framesRead < 0) {
      MINILOG_ERROR("Failed to read from audio fifo: {}", ffErrorString(framesRead));
    }
    return framesRead;
  }
  return 0;
}

bool FfmpegAudioFileReader::prepareDecoder(int streamIndex, Error **outError) {
  int error;
  auto stream = pFormatContext->streams[streamIndex];

  // calculate the sample length based on sample rate.
  mSampleLength = av_rescale_q(stream->duration, stream->time_base, {1, stream->codecpar->sample_rate});

  // Find a decoder for the audio stream.
  auto codec = avcodec_find_decoder(stream->codecpar->codec_id);
  if (!codec) {
    handleError(outError, "Failed to find available audio codec!");
    return false;
  }

  // Allocate a new decoding context.
  auto codecContext = avcodec_alloc_context3(codec);;
  if (!codecContext) {
    handleError(outError, "Could not allocate a decoding context");
    return false;
  }
  pCodecContext.reset(codecContext);

  // Initialize the stream parameters with demuxer information
  error = avcodec_parameters_to_context(pCodecContext.get(), stream->codecpar);
  if (error < 0) {
    handleError(outError, "Failed to copy parameters to audio decoder!");
    pCodecContext.reset();
    return false;
  }

  // Open the decoder for the audio stream to use it later.
  if ((error = avcodec_open2(pCodecContext.get(), codec, nullptr)) < 0) {
    handleError(outError, "Failed to open audio decoder!", ffErrorString(error).c_str());
    pCodecContext.reset();
    return false;
  }

  // Set the packet timebase for the decoder.
  pCodecContext->pkt_timebase = stream->time_base;

  mStreamIndex = streamIndex;
  mSampleRate = pCodecContext->sample_rate;
  mChannelCount = ffChannels(pCodecContext.get());

  // find out the file format
  if (mOutputType == AudioSampleFormat::kDefault) {
    mOutputType = FfmpegUtils::convertToAudioSampleFormat(pCodecContext->sample_fmt);
  }
  // we only supports interleaved audio data to read
  switch (mOutputType) {
    case AudioSampleFormat::kFloat: {
      pCodecContext->request_sample_fmt = AV_SAMPLE_FMT_FLT;
      break;
    }
    case AudioSampleFormat::kInt32: {
      pCodecContext->request_sample_fmt = AV_SAMPLE_FMT_S32;
      break;
    }
    default: {
      pCodecContext->request_sample_fmt = AV_SAMPLE_FMT_S16;
      break;
    }
  }
  auto fifo = av_audio_fifo_alloc(pCodecContext->request_sample_fmt,
      ffChannels(pCodecContext.get()), 1);
  pAudioFifo.reset(fifo);

  return initResample();
}

bool FfmpegAudioFileReader::initResample() {
  auto resampleContext = swr_alloc();
  ffSetSwrChannelLayout(resampleContext, "in_chlayout", ffChannels(pCodecContext.get()));
  ffSetSwrChannelLayout(resampleContext, "out_chlayout", ffChannels(pCodecContext.get()));
  av_opt_set_int(resampleContext, "in_sample_rate", pCodecContext->sample_rate,
      0);
  av_opt_set_int(resampleContext, "out_sample_rate", pCodecContext->sample_rate,
      0);
  av_opt_set_int(resampleContext, "in_sample_fmt", pCodecContext->sample_fmt,
      0);
  // The output format is always the requested format.
  av_opt_set_int(resampleContext, "out_sample_fmt",
      pCodecContext->request_sample_fmt, 0);
  auto result = swr_init(resampleContext);
  if (result < 0) {
    MINILOG_ERROR("Failed to call swr_init: {}", ffErrorString(result));
    swr_free(&resampleContext);
    return false;
  }
  pCodecContext->opaque = resampleContext;
  return true;
}

bool FfmpegAudioFileReader::resampleAndStoreFrame(AVCodecContext *codecContext,
    AVAudioFifo *fifo,
    AVFrame *inputFrame) {
  /* Temporary storage for the converted input samples. */
  uint8_t **convertedSamples = nullptr;

  // Initialize the temporary storage for the converted input samples.
  auto result = initConvertedSamples(&convertedSamples, codecContext,
      inputFrame->nb_samples);
  if (!result) {
    releaseBuffer(convertedSamples);
    MINILOG_ERROR("Failed to init converted samples");
    return false;
  }

  // Convert the input samples to the desired output sample format.
  // This requires a temporary storage provided by converted_input_samples.
  if (!convertSamples((SwrContext *)codecContext->opaque,
      (const uint8_t **)inputFrame->extended_data,
      convertedSamples, inputFrame->nb_samples)) {
    releaseBuffer(convertedSamples);
    MINILOG_ERROR("Failed to convert samples");
    return false;
  }

  // Add the converted input samples to the FIFO buffer for later processing.
  result = addSamplesToFifo(fifo, convertedSamples, inputFrame->nb_samples);
  releaseBuffer(convertedSamples);
  if (!result) {
    MINILOG_ERROR("Failed to add samples to fifo");
    return false;
  }
  return true;
}

bool FfmpegAudioFileReader::initConvertedSamples(uint8_t ***convertedSamples,
    AVCodecContext *codecContext,
    int frameSize) {
  int error;

  /* Allocate as many pointers as there are audio channels.
   * Each pointer will later point to the audio samples of the corresponding
   * channels (although it may be NULL for interleaved formats).
   */
  if (!(
      *convertedSamples = (uint8_t **)calloc(ffChannels(codecContext), sizeof(**convertedSamples)))) {
    MINILOG_ERROR("Could not allocate converted input sample pointers");
    return false;
  }

  /* Allocate memory for the samples of all channels in one consecutive
   * block for convenience. */
  if ((
      error = av_samples_alloc(*convertedSamples, nullptr, ffChannels(codecContext),
          frameSize, codecContext->sample_fmt, 0)) < 0) {
    MINILOG_ERROR("Could not allocate converted input samples (error {})", ffErrorString(error));
    av_freep(&(*convertedSamples)[0]);
    free(*convertedSamples);
    return false;
  }
  return true;
}

bool FfmpegAudioFileReader::convertSamples(SwrContext *resampleContext,
    const uint8_t **inputData,
    uint8_t **convertedData,
    const int frameSize) {
  int error;

  /* Convert the samples using the resampler. */
  if ((error = swr_convert(resampleContext, convertedData, frameSize, inputData,frameSize)) < 0) {
    MINILOG_ERROR("Could not convert input samples (error {})", ffErrorString(error));
    return false;
  }
  return true;
}

bool FfmpegAudioFileReader::addSamplesToFifo(AVAudioFifo *fifo,
    uint8_t **convertedInputSamples,
    const int frame_size) {
  int error;

  /* Make the FIFO as large as it needs to be to hold both,
   * the old and the new samples. */
  if ((
      error = av_audio_fifo_realloc(fifo, av_audio_fifo_size(fifo) +
          frame_size)) < 0) {
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

void FfmpegAudioFileReader::releaseBuffer(uint8_t **buffer) {
  if (buffer) {
    av_freep(&buffer[0]);
    free(buffer);
  }
}

} // namespace coral
