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

#ifndef CORAL_CODEC_ANDROID_MEDIA_CODEC_UTILS_H
#define CORAL_CODEC_ANDROID_MEDIA_CODEC_UTILS_H

#include <media/ColorInfo.h>
#include <media/Configuration.h>
#include <media/Format.h>

#include <core/jni/android/AudioFormat.h>
#include <core/jni/android/MediaFormat.h>

#if defined(__ANDROID__)
#include <media/NdkMediaFormat.h>
#endif

#include <memory>

namespace coral {

/**
 * A utility class for querying the available codecs.
 */
class MediaCodecUtils {
public:
  static constexpr int BUFFER_FLAG_KEY_FRAME = 1;

  enum {
    COLOR_RANGE_FULL = 0x1,
    COLOR_RANGE_LIMITED = 0x2,
  };

  static ColorInfo::RangeType getColorRange(int color_range);

  enum {
    COLOR_STANDARD_BT709 = 0x1,
    COLOR_STANDARD_BT601_PAL = 0x2,
    COLOR_STANDARD_BT601_NTSC = 0x4,
    COLOR_STANDARD_BT2020 = 0x6,
  };

  static ColorInfo::SpaceType getColorSpace(int color_space);

  enum {
    COLOR_TRANSFER_LINEAR = 0x1,
    COLOR_TRANSFER_SRGB = 0x2,
    COLOR_TRANSFER_SDR_VIDEO = 0x3,
    COLOR_TRANSFER_ST2084 = 0x6,
    COLOR_TRANSFER_HLG = 0x7,
  };

  static ColorInfo::TransferType getColorTransfer(int color_transfer);

  /** Constant quality mode */
  static constexpr int BITRATE_MODE_CQ = 0;

  /** Variable bitrate mode */
  static constexpr int BITRATE_MODE_VBR = 1;

  /** Constant bitrate mode */
  static constexpr int BITRATE_MODE_CBR = 2;

  /**
   * 24 bits per pixel RGB color format, with 8-bit red, green & blue
   * components. <p> Using 24-bit little-endian representation, colors stored as
   * Red 7:0, Green 15:8, Blue 23:16. <pre> byte              byte byte
   *  <------ i -----> | <---- i+1 ----> | <---- i+2 ----->
   * +-----------------+-----------------+-----------------+
   * |       RED       |      GREEN      |       BLUE      |
   * +-----------------+-----------------+-----------------+
   * </pre>
   *
   * This format corresponds to {@link android.graphics.PixelFormat#RGB_888},
   * and can also be represented as a flexible format by {@link
   * #COLOR_FormatRGBFlexible}.
   */
  static constexpr int COLOR_Format24bitBGR888 = 12;

  static constexpr int COLOR_FormatYUV411Planar = 17;

  static constexpr int COLOR_FormatYUV411PackedPlanar = 18;

  //!< YUV420P(I420) format
  static constexpr int COLOR_FormatYUV420Planar = 19;

  static constexpr int COLOR_FormatYUV420PackedPlanar = 20;

  //!< YUV420SP(NV12)
  static constexpr int COLOR_FormatYUV420SemiPlanar = 21;

  static constexpr int COLOR_FormatYUV422Planar = 22;

  static constexpr int COLOR_FormatYUV422PackedPlanar = 23;

  static constexpr int COLOR_FormatYUV422SemiPlanar = 24;

  static constexpr int COLOR_FormatYUV420PackedSemiPlanar = 39;

  static constexpr int COLOR_FormatYUV422PackedSemiPlanar = 40;

  static constexpr int COLOR_TI_FormatYUV420PackedSemiPlanar = 0x7f000100;

  static constexpr int COLOR_TI_FormatYUV420PackedSemiPlanarInterlaced = 0x7f000001;

  // COLOR_FormatSurface indicates that the data will be a GraphicBuffer
  // metadata reference. Note: in OMX this is called OMX_COLOR_FormatAndroidOpaque.
  static constexpr int COLOR_FormatSurface = 0x7F000789;

  /**
   * 32 bits per pixel RGBA color format, with 8-bit red, green, blue, and alpha
   * components. <p> Using 32-bit little-endian representation, colors stored as
   * Red 7:0, Green 15:8, Blue 23:16, and Alpha 31:24. <pre> byte byte byte byte
   *  <------ i -----> | <---- i+1 ----> | <---- i+2 ----> | <---- i+3 ----->
   * +-----------------+-----------------+-----------------+-----------------+
   * |       RED       |      GREEN      |       BLUE      |      ALPHA      |
   * +-----------------+-----------------+-----------------+-----------------+
   * </pre>
   *
   * This corresponds to {@link android.graphics.PixelFormat#RGBA_8888}.
   */
  static constexpr int COLOR_Format32bitABGR8888 = 0x7F00A000;

  /**
   * Flexible 12 bits per pixel, subsampled YUV color format with 8-bit chroma
   * and luma components. <p> Chroma planes are subsampled by 2 both
   * horizontally and vertically. Use this format with {@link Image}. This
   * format corresponds to {@link android.graphics.ImageFormat#YUV_420_888}, and
   * can represent the {@link #COLOR_FormatYUV411Planar},
   * {@link #COLOR_FormatYUV411PackedPlanar}, {@link #COLOR_FormatYUV420Planar},
   * {@link #COLOR_FormatYUV420PackedPlanar}, {@link
   * #COLOR_FormatYUV420SemiPlanar} and {@link
   * #COLOR_FormatYUV420PackedSemiPlanar} formats.
   *
   * @see android.media.Image.getFormat()
   */
  static constexpr int COLOR_FormatYUV420Flexible = 0x7F420888;

  /**
   * Flexible 16 bits per pixel, subsampled YUV color format with 8-bit chroma
   * and luma components. <p> Chroma planes are horizontally subsampled by 2.
   * Use this format with {@link Image}. This format corresponds to {@link
   * android.graphics.ImageFormat#YUV_422_888}, and can represent the {@link
   * #COLOR_FormatYCbYCr}, {@link #COLOR_FormatYCrYCb},
   * {@link #COLOR_FormatCbYCrY}, {@link #COLOR_FormatCrYCbY},
   * {@link #COLOR_FormatYUV422Planar}, {@link #COLOR_FormatYUV422PackedPlanar},
   * {@link #COLOR_FormatYUV422SemiPlanar} and {@link
   * #COLOR_FormatYUV422PackedSemiPlanar} formats.
   *
   * @see android.media.Image.getFormat()
   */
  static constexpr int COLOR_FormatYUV422Flexible = 0x7F422888;

  /**
   * Flexible 24 bits per pixel YUV color format with 8-bit chroma and luma
   * components. Chroma planes are not subsampled. Use this format with Image.
   * This format corresponds to android.graphics.ImageFormat.YUV_444_888, and
   * can represent the COLOR_FormatYUV444Interleaved format.
   *
   * See Also: android.media.Image.getFormat()
   */
  static constexpr int COLOR_FormatYUV444Flexible = 0x7F444888;

  /**
   * Flexible 32 bits per pixel RGBA color format with 8-bit red, green, blue,
   * and alpha components.
   * Use this format with Image. This format corresponds to
   * android.graphics.ImageFormat. FLEX_RGBA_8888, and can represent
   * COLOR_Format32bitBGRA8888, COLOR_Format32bitABGR8888 and
   * COLOR_Format32bitARGB8888 formats.
   *
   * See Also: android.media.Image.getFormat()
   */
  static constexpr int COLOR_FormatRGBAFlexible = 0x7F36A888;

  /**
   * Deprecated Use COLOR_FormatYUV420Flexible.
   */
  static constexpr int COLOR_QCOM_FormatYUV420SemiPlanar = 0x7fa30c00;

  static int getApiLevel();

  /**
   * Checks whether MediaCodec supports to decode the AudioFormatType data
   */
  static bool supportAudioDecoding(const AudioFormatType& formatType);

  /**
   * Checks whether MediaCodec supports to encode the AudioFormatType data
   */
  static bool supportAudioEncoding(const AudioFormatType& formatType);

  /**
   * Checks whether MediaCodec supports to decode the VideoCodecType data
   */
  static bool supportVideoDecoding(const VideoCodecType& formatType);

  /**
   * Checks whether MediaCodec supports to encode the AudioFormatType data
   */
  static bool supportVideoEncoding(const VideoCodecType& formatType);

  static std::shared_ptr<MediaFormat> convertToMediaFormat(const std::unique_ptr<JniMediaFormat>& format);

  static std::shared_ptr<MediaFormat> convertToMediaFormat(AMediaFormat* mediaFormat);

  static std::unique_ptr<JniMediaFormat> convertToJniMediaFormat(const std::shared_ptr<MediaFormat>& mediaFormat);

  static AMediaFormat* convertToNdkMediaFormat(const std::shared_ptr<MediaFormat>& mediaFormat);

  /**
   * NV12 to YUV420P
   */
  static void NV12toYUV420Planar(const uint8_t* input, int offset, uint8_t* output, int width, int height);

  /**
   * NV21 to YUV420P
   */
  static void NV21toYUV420Planar(const uint8_t* input, int offset, uint8_t* output, int width, int height);

  /**
   * YUV420P to YUV420SP
   */
  static void I420toYUV420SemiPlanar(const uint8_t* input, int offset, uint8_t* output, int width, int height);

  static void I420toNV21(const uint8_t* input, int offset, uint8_t* output, int width, int height);

  static void NV12toNV21(const uint8_t* input, int offset, uint8_t* output, int width, int height);

private:
  MediaCodecUtils() = default;
  ~MediaCodecUtils() = default;

  static constexpr int DEFAULT_I_FRAME_INTERVAL = 1;
  static constexpr int DEFAULT_FRAME_RATE = 30;

  static constexpr int DEFAULT_BUFFER_SIZE = 2048;
};

} // namespace coral

#endif // CORAL_CODEC_ANDROID_MEDIA_CODEC_UTILS_H
