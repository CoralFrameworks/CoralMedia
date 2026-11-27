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
#ifndef CORAL_CODEC_FFMPEG_UTILS_H
#define CORAL_CODEC_FFMPEG_UTILS_H

#include <media/Types.h>

#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/file.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace coral {

/**
 * FFmpeg moved the channel count into AVChannelLayout. The code keeps asking for a count, so these
 * helpers translate between the count and the layout (and set the default layout of a count).
 */
inline int ffChannels(const AVCodecContext* context) { return context->ch_layout.nb_channels; }
inline int ffChannels(const AVCodecParameters* parameters) { return parameters->ch_layout.nb_channels; }
inline int ffChannels(const AVFrame* frame) { return frame->ch_layout.nb_channels; }

inline void ffSetChannels(AVCodecContext* context, const int channels) {
  av_channel_layout_uninit(&context->ch_layout);
  av_channel_layout_default(&context->ch_layout, channels);
}
inline void ffSetChannels(AVCodecParameters* parameters, const int channels) {
  av_channel_layout_uninit(&parameters->ch_layout);
  av_channel_layout_default(&parameters->ch_layout, channels);
}
inline void ffSetChannels(AVFrame* frame, const int channels) {
  av_channel_layout_uninit(&frame->ch_layout);
  av_channel_layout_default(&frame->ch_layout, channels);
}

/** Formats an FFmpeg error code. av_err2str() is a C compound literal and does not compile as C++. */
inline std::string ffErrorString(const int error) {
  char buffer[AV_ERROR_MAX_STRING_SIZE]{};
  av_strerror(error, buffer, sizeof(buffer));
  return buffer;
}

/** The first sample format the codec supports, or AV_SAMPLE_FMT_NONE. */
inline AVSampleFormat ffFirstSampleFormat(const AVCodec* codec) {
  const AVSampleFormat* formats = nullptr;
  int count = 0;
  if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0,
          reinterpret_cast<const void**>(&formats), &count) < 0 ||
      formats == nullptr || count <= 0) {
    return AV_SAMPLE_FMT_NONE;
  }
  return formats[0];
}

inline void ffSetSwrChannelLayout(SwrContext* context, const char* name, const int channels) {
  AVChannelLayout layout{};
  av_channel_layout_default(&layout, channels);
  av_opt_set_chlayout(context, name, &layout, 0);
  av_channel_layout_uninit(&layout);
}

/**
 * A utility class for Ffmpeg.
 */
class FfmpegUtils {
public:
  /**
   * Returns the codec's name of the mime type.
   */
  static std::string getCodecName(const std::string& mimeType);

  /**
   * Converts the AVCodecID to mime type
   */
  static std::string convertToMime(AVCodecID codecId);

  /**
   * Converts the mime type to AVCodecID
   */
  static AVCodecID convertToCodecID(const std::string& mime);

  /**
   * Converts AudioSampleFormat to AVSampleFormat
   */
  static AVSampleFormat convertToSampleFormat(AudioSampleFormat sampleFormat);

  /**
   * Converts AVSampleFormat to AudioSampleFormat
   */
  static AudioSampleFormat convertToAudioSampleFormat(AVSampleFormat format);

  /**
   * Converts AVPixelFormat to PixelFormat
   */
  static PixelFormat convertToPixelFormat(AVPixelFormat format);

  /**
   * Converts PixelFormat to AVPixelFormat
   */
  static AVPixelFormat convertToAVPixelFormat(PixelFormat format);

  /**
   * Converts AVCodecID to AudioFormatType
   */
  static AudioFormatType convertToAudioFormatType(AVCodecID codecId);

  /**
   * Converts AVCodecID to VideoCodecType
   */
  static VideoCodecType convertToVideoCodecType(AVCodecID codecId);

  /**
   * Converts AVMediaType to MediaType
   */
  static MediaType getMediaType(AVMediaType mediaType);

private:
  FfmpegUtils() = default;

  ~FfmpegUtils() = default;
};

// The deleter for AVFormatContext
struct AVFormatContextDeleter {
  void operator()(AVFormatContext* context) const {
    if (context != nullptr && context->pb) {
      av_freep(&context->pb->buffer);
      avio_closep(&context->pb);
    }
    avformat_close_input(&context);
  }
};

// The deleter for AVPacket
struct AVPacketDeleter {
  void operator()(AVPacket* packet) const {
    if (packet != nullptr) {
      av_packet_unref(packet);
      av_packet_free(&packet);
    }
  }
};

// The deleter for AVFrame
struct AVFrameDeleter {
  void operator()(AVFrame* frame) const {
    if (frame != nullptr) {
      av_frame_unref(frame);
      av_frame_free(&frame);
    }
  }
};

// The deleter for SwsContext
struct SwsContextDeleter {
  void operator()(SwsContext* context) const {
    if (context != nullptr) {
      sws_freeContext(context);
    }
  }
};

// The deleter for AVAudioFifo
struct AVAudioFifoDeleter {
  void operator()(AVAudioFifo* fifo) const { av_audio_fifo_free(fifo); }
};

} // namespace coral

#endif // CORAL_CODEC_FFMPEG_UTILS_H
