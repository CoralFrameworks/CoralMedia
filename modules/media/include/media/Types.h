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

#ifndef CORAL_MEDIA_TYPES_H
#define CORAL_MEDIA_TYPES_H

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace coral {

/**
 * Represents a media type or an invalid value
 */
enum class MediaType : int8_t {
  //!< Invalid media type
  kMediaTypeInvalid = -1,
  //!< A type constant for some default type, where the type itself is unknown.
  kMediaTypeDefault = 0,
  //!< A type constant for audio
  kMediaTypeAudio = 1,
  //!< A type constant for video
  kMediaTypeVideo = 2,
  //!< A type constant for subtitle.
  kMediaTypeSubtitle = 3,
  //!< A type constant for image.
  kMediaTypeImage = 4,
  //!< A type constant for metadata.
  kMediaTypeMetadata = 5,
};

/**
 * Represents a media file type or an invalid value
 */
enum class MediaFileType : int32_t {
  //!< Invalid media file type
  kMediaFileTypeUnknown = -1,
  //!< A type constant for mpeg4 aac file
  kMediaFileTypeMpeg4AAC = 0,
  //!< A type constant for mpeg4 video file
  kMediaFileTypeMpeg4Video = 1,
  //!< A type constant for h263 video file
  kMediaFileTypeH263 = 2,
  //!< A type constant for h264 video file
  kMediaFileTypeH264 = 3,
  //!< A type constant for flac audio file
  kMediaFileTypeFlac = 4,
  //!< A type constant for opus audio file
  kMediaFileTypeOpus = 5,
  //!< A type constant for wav audio file
  kMediaFileTypeWav = 6,
};

enum class MediaCharacteristic : int8_t {
  /**
   * An unknown media characteristic
   */
  kMediaCharacteristicUnknown = -1,
  /**
   * A media characteristic that indicates that a track or media selection
   * option includes audible content.
   *
   * kMediaTypeAudio is a media type with the characteristic
   * kMediaCharacteristicAudible.
   */
  kMediaCharacteristicAudible = 0,
  /**
   * A media characteristic that indicates that a track or media selection
   * option includes visual content.
   *
   * kMediaTypeVideo is a media type with the characteristic
   * kMediaCharacteristicVisual.
   */
  kMediaCharacteristicVisual = 1,
  /**
   * A media characteristic that indicates that a track or media selection
   * option includes legible content.
   *
   * kMediaTypeSubtitle is a media type with the characteristic
   * kMediaCharacteristicLegible.
   */
  kMediaCharacteristicLegible = 2,
};

/**
 * Media selection flags.
 */
enum MediaSelectionFlags : uint8_t {
  /** Indicates that no selection flag is set. */
  kMediaSelectionFlagNone = 0, // 0
  /**
   * Indicates that the track should be selected if user preferences do not
   * state otherwise.
   */
  kMediaSelectionDefault = 1, // 1
  /**
   * Indicates that the track must be displayed. Only applies to text tracks.
   */
  kMediaSelectionForced = 1 << 1, // 2
  /**
   * Indicates that the player may choose to play the track in absence of an
   * explicit user preference.
   */
  kMediaSelectionAutoSelect = 1 << 2, // 4
};

/**
 * The replacement behavior of the cues of a text track.
 *
 * <p>Mirrors ExoPlayer's {@code Format.CUE_REPLACEMENT_BEHAVIOR_*} constants: it tells a player
 * whether the cues of a sample add to the ones that are still shown, or replace them.
 */
enum class CueReplacementBehavior : uint8_t {
  //!< The cues of a sample are merged with the cues that are still shown.
  kMerge = 1,
  //!< The cues of a sample replace every cue that is still shown.
  kReplace = 2,
};

/**
 * How a player renders subtitles when no subtitle/text output is attached.
 *
 * <p>The mode is a player policy rather than a property of the media: it is shared by the compose
 * player and the playback player so that both answer the same question the same way.
 */
enum class SubtitleRenderMode : uint8_t {
  //!< Deliver cues to the attached outputs, and burn them into the video frames only when there is none.
  kAuto = 0,
  //!< Never burn subtitles into the frames; an application that attaches no output shows no subtitle.
  kOutputOnly = 1,
  //!< Always burn subtitles into the frames, whether an output is attached or not.
  kBurnIn = 2,
};

/**
 * Role flags.
 *
 * <p>The values are the ones of ExoPlayer's {@code C.RoleFlags}, so that a format described by a
 * ported extractor keeps its meaning.
 */
enum RoleFlags : uint32_t {
  /** Indicates that no role flag is set. */
  kRoleFlagNone = 0,
  /** Indicates the track is the main one of its kind. */
  kRoleFlagMain = 1,
  /** Indicates the track is an alternate of the main track. */
  kRoleFlagAlternate = 1 << 1,
  /** Indicates the track is supplementary to the main track. */
  kRoleFlagSupplementary = 1 << 2,
  /** Indicates the track contains commentary. */
  kRoleFlagCommentary = 1 << 3,
  /** Indicates the track contains a dubbed version of the main track. */
  kRoleFlagDub = 1 << 4,
  /** Indicates the track contains emergency information. */
  kRoleFlagEmergency = 1 << 5,
  /** Indicates the track contains captions. */
  kRoleFlagCaption = 1 << 6,
  /** Indicates the track contains subtitles. */
  kRoleFlagSubtitle = 1 << 7,
  /** Indicates the track contains a sign language translation. */
  kRoleFlagSign = 1 << 8,
  /** Indicates the track describes the video of the main track. */
  kRoleFlagDescribesVideo = 1 << 9,
  /**
   * Indicates the track describes the music and the sound effects of the main track, e.g. an audio
   * description.
   */
  kRoleFlagDescribesMusicAndSound = 1 << 10,
  /** Indicates the track is an enhanced dialog intelligibility version. */
  kRoleFlagEnhancedDialogIntelligibility = 1 << 11,
  /** Indicates the track transcribes the dialog. */
  kRoleFlagTranscribesDialog = 1 << 12,
  /** Indicates the track is meant to be easy to read. */
  kRoleFlagEasyToRead = 1 << 13,
};

/**
 * The audio format type
 */
enum class AudioFormatType : uint32_t {
  // clang-format off
  //!< Invalid audio format type
  kAudioFormatTypeInvalid   = 0,
  //!< A type constant for encoding type linear PCM
  kAudioFormatTypeLinearPCM = 1,
  //!< A type constant for encoding type ac3
  kAudioFormatTypeAC3       = 2,
  //!< A type constant for encoding type mp3
  kAudioFormatTypeMpeg3     = 3,
  //!< A type constant for encoding type aac
  kAudioFormatTypeMpeg4AAC  = 4,
  //!< A type constant for encoding type alaw
  kAudioFormatTypeALaw      = 5,
  //!< A type constant for encoding type mlaw
  kAudioFormatTypeMLaw      = 6,
  //!< A type constant for encoding type amr
  kAudioFormatTypeAMR       = 7,
  //!< A type constant for encoding type amr_wb
  kAudioFormatTypeAMR_WB    = 8,
  //!< A type constant for encoding type flac
  kAudioFormatTypeFlac      = 9,
  //!< A type constant for encoding type opus
  kAudioFormatTypeOpus      = 10,
  //!< A type constant for encoding type vorbis
  kAudioFormatTypeVorbis    = 11,
  //!< A type constant for encoding type alac
  kAudioFormatTypeAlac      = 12,
  //!< A type constant for encoding type truehd
  kAudioFormatTypeTrueHD    = 13,
  //!< A type constant for encoding type dts
  kAudioFormatTypeDTS       = 14,
  // clang-format on
};

/**
 * Standard AudioFormatFlags Values for AudioBasicDescription
 * These are the standard AudioFormatFlags for use in the mFormatFlags field of the
 * AudioBasicDescription structure.
 */
enum AudioFormatFlags : uint32_t {
  // clang-format off
  kAudioFormatFlagIsFloat                     = (1U << 0), // 0x1
  kAudioFormatFlagIsBigEndian                 = (1U << 1), // 0x2
  kAudioFormatFlagIsSignedInteger             = (1U << 2), // 0x4
  kAudioFormatFlagIsPacked                    = (1U << 3), // 0x8
  kAudioFormatFlagIsAlignedHigh               = (1U << 4), // 0x10
  kAudioFormatFlagIsNonInterleaved            = (1U << 5), // 0x20
  kAudioFormatFlagIsNonMixable                = (1U << 6), // 0x40
  kAudioFormatFlagsAreAllClear                = 0x80000000,

  kLinearPCMFormatFlagIsFloat                 = kAudioFormatFlagIsFloat,
  kLinearPCMFormatFlagIsBigEndian             = kAudioFormatFlagIsBigEndian,
  kLinearPCMFormatFlagIsSignedInteger         = kAudioFormatFlagIsSignedInteger,
  kLinearPCMFormatFlagIsPacked                = kAudioFormatFlagIsPacked,
  kLinearPCMFormatFlagIsAlignedHigh           = kAudioFormatFlagIsAlignedHigh,
  kLinearPCMFormatFlagIsNonInterleaved        = kAudioFormatFlagIsNonInterleaved,
  kLinearPCMFormatFlagIsNonMixable            = kAudioFormatFlagIsNonMixable,
  kLinearPCMFormatFlagsSampleFractionShift    = 7,
  kLinearPCMFormatFlagsSampleFractionMask     = (0x3F << kLinearPCMFormatFlagsSampleFractionShift),
  kLinearPCMFormatFlagsAreAllClear            = kAudioFormatFlagsAreAllClear,

  kAppleLosslessFormatFlag_16BitSourceData    = 1,
  kAppleLosslessFormatFlag_20BitSourceData    = 2,
  kAppleLosslessFormatFlag_24BitSourceData    = 3,
  kAppleLosslessFormatFlag_32BitSourceData    = 4
  // clang-format on
};

/**
 * Represents audio sample format, or an invalid value.
 *     {@link kInvalid}
 *     {@link kDefault}
 *     {@link kInt16}
 *     {@link kFloat}
 *     {@link kInt24}
 *     {@link kInt32}
 */
enum class AudioSampleFormat : int32_t {
  //!< Invalid audio sample format
  kInvalid = -1,
  //!< Default audio sample format.
  kDefault = 0,
  //!< Audio sample format: PCM 16 bit per sample. Guaranteed to be
  //!< supported by devices.
  kInt16 = 1,
  //!< Audio sample format: single-precision floating-point per sample
  kFloat = 2,
  //!< Audio sample format: Signed 24-bit integers, packed into 3 bytes.
  kInt24 = 3,
  //!< Audio sample format: PCM encoding with 32 bits per sample.
  kInt32 = 4,
};

/**
 * The video codec type
 */
enum class VideoCodecType : uint32_t {
  //!< Invalid video codec type
  kVideoCodecTypeInvalid = 0,
  //!< A type constant for encoding type h263
  kVideoCodecTypeH263 = 1,
  //!< A type constant for encoding type h264
  kVideoCodecTypeH264 = 2,
  //!< A type constant for encoding type hevc
  kVideoCodecTypeHevc = 3,
  //!< A type constant for encoding type av01
  kVideoCodecTypeAV1 = 4,
  //!< A type constant for encoding type mpeg2-video
  kVideoCodecTypeMpeg2Video = 5,
  //!< A type constant for encoding type mpeg4-video
  kVideoCodecTypeMpeg4Video = 6,
  //!< A type constant for encoding type vp8
  kVideoCodecTypeVP8 = 7,
  //!< A type constant for encoding type vp9
  kVideoCodecTypeVP9 = 8,
  //!< A type constant for encoding type dolby vision
  kVideoCodecTypeDolbyVision = 9,
};

/**
 * Represents a video frame pixel format, or an invalid value.
 */
enum class PixelFormat : int8_t {
  //!< Invalid video pixel format
  kFormatInvalid = -1,
  //!< A type constant for pixel format surface. Usually for Android MediaCodec's decoded output surface.
  kFormatSurface = 0,
  //!< A type constant for pixel format I420.
  kFormatI420 = 1,
  //!< A type constant for pixel format NV12.
  kFormatNV12 = 2,
  //!< A type constant for pixel format RGBA_8888.
  kFormatRGBA = 3,
  //!< A type constant for pixel format BGRA_8888.
  kFormatBGRA = 4,
  //!< A type constant for a single channel plane, i.e. the Y, U or V plane of a YUV frame on the GPU.
  kFormatGray = 5,
};

/**
 * Defines common file type constants and helper methods.
 */
class FileTypes {
public:
  /** Unknown file type. */
  static constexpr int UNKNOWN = -1;

  /** File type for the AC-3 and E-AC-3 formats. */
  static constexpr int AC3 = 0;

  /** File type for the AC-4 format. */
  static constexpr int AC4 = 1;

  /** File type for the ADTS format. */
  static constexpr int ADTS = 2;

  /** File type for the AMR format. */
  static constexpr int AMR = 3;

  /** File type for the FLAC format. */
  static constexpr int FLAC = 4;

  /** File type for the FLV format. */
  static constexpr int FLV = 5;

  /** File type for the Matroska and WebM formats. */
  static constexpr int MATROSKA = 6;

  /** File type for the MP3 format. */
  static constexpr int MP3 = 7;

  /** File type for the MP4 format. */
  static constexpr int MP4 = 8;

  /** File type for the Ogg format. */
  static constexpr int OGG = 9;

  /** File type for the MPEG-PS format. */
  static constexpr int PS = 10;

  /** File type for the MPEG-TS format. */
  static constexpr int TS = 11;

  /** File type for the WAV format. */
  static constexpr int WAV = 12;

  /** File type for the WebVTT format. */
  static constexpr int WEBVTT = 13;

  /** File type for the JPEG format. */
  static constexpr int JPEG = 14;

  /** File type for the MIDI format. */
  static constexpr int MIDI = 15;

  /** File type for the AVI format. */
  static constexpr int AVI = 16;

  /** File type for the PNG format. */
  static constexpr int PNG = 17;

  /** File type for the WEBP format. */
  static constexpr int WEBP = 18;

  /** File type for the BMP format. */
  static constexpr int BMP = 19;

  /** File type for the HEIF format. */
  static constexpr int HEIF = 20;

  /** File type for the AVIF format. */
  static constexpr int AVIF = 21;

  /** File type for the SubRip format. */
  static constexpr int SRT = 22;

  /** File type for the SubStation Alpha and Advanced SubStation Alpha formats. */
  static constexpr int SSA = 23;

private:
  static constexpr auto HEADER_CONTENT_TYPE = "Content-Type";

  static constexpr auto EXTENSION_AC3 = ".ac3";
  static constexpr auto EXTENSION_EC3 = ".ec3";
  static constexpr auto EXTENSION_AC4 = ".ac4";
  static constexpr auto EXTENSION_ADTS = ".adts";
  static constexpr auto EXTENSION_AAC = ".aac";
  static constexpr auto EXTENSION_AMR = ".amr";
  static constexpr auto EXTENSION_FLAC = ".flac";
  static constexpr auto EXTENSION_FLV = ".flv";
  static constexpr auto EXTENSION_MID = ".mid";
  static constexpr auto EXTENSION_MIDI = ".midi";
  static constexpr auto EXTENSION_SMF = ".smf";
  static constexpr auto EXTENSION_PREFIX_MK = ".mk";
  static constexpr auto EXTENSION_WEBM = ".webm";
  static constexpr auto EXTENSION_PREFIX_OG = ".og";
  static constexpr auto EXTENSION_OPUS = ".opus";
  static constexpr auto EXTENSION_MP3 = ".mp3";
  static constexpr auto EXTENSION_MP4 = ".mp4";
  static constexpr auto EXTENSION_PREFIX_M4 = ".m4";
  static constexpr auto EXTENSION_PREFIX_MP4 = ".mp4";
  static constexpr auto EXTENSION_PREFIX_CMF = ".cmf";
  static constexpr auto EXTENSION_PS = ".ps";
  static constexpr auto EXTENSION_MPEG = ".mpeg";
  static constexpr auto EXTENSION_MPG = ".mpg";
  static constexpr auto EXTENSION_M2P = ".m2p";
  static constexpr auto EXTENSION_TS = ".ts";
  static constexpr auto EXTENSION_PREFIX_TS = ".ts";
  static constexpr auto EXTENSION_WAV = ".wav";
  static constexpr auto EXTENSION_WAVE = ".wave";
  static constexpr auto EXTENSION_VTT = ".vtt";
  static constexpr auto EXTENSION_WEBVTT = ".webvtt";
  static constexpr auto EXTENSION_JPG = ".jpg";
  static constexpr auto EXTENSION_JPEG = ".jpeg";
  static constexpr auto EXTENSION_AVI = ".avi";
  static constexpr auto EXTENSION_PNG = ".png";
  static constexpr auto EXTENSION_WEBP = ".webp";
  static constexpr auto EXTENSION_BMP = ".bmp";
  static constexpr auto EXTENSION_DIB = ".dib";
  static constexpr auto EXTENSION_HEIC = ".heic";
  static constexpr auto EXTENSION_HEIF = ".heif";
  static constexpr auto EXTENSION_AVIF = ".avif";
  static constexpr auto EXTENSION_SRT = ".srt";
  static constexpr auto EXTENSION_SSA = ".ssa";
  static constexpr auto EXTENSION_ASS = ".ass";

public:
  /**
   * Returns the {@link MediaFileType} corresponding to the MIME type provided.
   *
   * Returns {@link UNKNOWN} if the MIME type is empty.
   */
  static int inferFileTypeFromMimeType(const std::string& mimeType);

  /**
   * Returns the {@link MediaFileType} corresponding to the {@link url} provided.
   */
  static int inferFileTypeFromUrl(const std::string& url);

private:
  FileTypes() = default;
  ~FileTypes() = default;

  static bool startsWith(const std::string& str, const std::string& prefix, const int toOffset) {
    if (toOffset < 0 || toOffset > str.length() - prefix.length()) {
      return false;
    }
    return str.size() >= prefix.size() && str.compare(toOffset, prefix.size(), prefix) == toOffset;
  }

  static bool endsWith(const std::string& str, const std::string& subStr) {
    return (str.rfind(subStr) == (str.length() - subStr.length()));
  }
};

/**
 * Defines common MIME types and helper methods.
 */
class MimeTypes {
public:
  static constexpr auto TYPE_AUDIO = "audio";
  static constexpr auto TYPE_VIDEO = "video";
  static constexpr auto TYPE_IMAGE = "image";
  static constexpr auto TYPE_TEXT = "text";
  static constexpr auto TYPE_APPLICATION = "application";

  /// audio MIME types
  static constexpr auto AUDIO_MP4 = "audio/mp4";
  static constexpr auto AUDIO_AAC = "audio/mp4a-latm";
  static constexpr auto AUDIO_MATROSKA = "audio/x-matroska";
  static constexpr auto AUDIO_WEBM = "audio/webm";
  static constexpr auto AUDIO_MPEG = "audio/mpeg";
  static constexpr auto AUDIO_MPEG_L1 = "audio/mpeg-L1";
  static constexpr auto AUDIO_MPEG_L2 = "audio/mpeg-L2";
  static constexpr auto AUDIO_MPEGH_MHA1 = "audio/mha1";
  static constexpr auto AUDIO_MPEGH_MHM1 = "audio/mhm1";
  static constexpr auto AUDIO_RAW = "audio/raw";
  static constexpr auto AUDIO_G711_ALAW = "audio/g711-alaw";
  static constexpr auto AUDIO_G711_MLAW = "audio/g711-mlaw";
  static constexpr auto AUDIO_AC3 = "audio/ac3";
  static constexpr auto AUDIO_E_AC3 = "audio/eac3";
  static constexpr auto AUDIO_E_AC3_JOC = "audio/eac3-joc";
  static constexpr auto AUDIO_AC4 = "audio/ac4";
  static constexpr auto AUDIO_TRUEHD = "audio/true-hd";
  static constexpr auto AUDIO_DTS = "audio/vnd.dts";
  static constexpr auto AUDIO_DTS_HD = "audio/vnd.dts.hd";
  static constexpr auto AUDIO_DTS_EXPRESS = "audio/vnd.dts.hd;profile=lbr";
  static constexpr auto AUDIO_DTS_X = "audio/vnd.dts.uhd;profile=p2";
  static constexpr auto AUDIO_VORBIS = "audio/vorbis";
  static constexpr auto AUDIO_OPUS = "audio/opus";
  static constexpr auto AUDIO_AMR = "audio/amr";
  static constexpr auto AUDIO_AMR_NB = "audio/3gpp";
  static constexpr auto AUDIO_AMR_WB = "audio/amr-wb";
  static constexpr auto AUDIO_FLAC = "audio/flac";
  static constexpr auto AUDIO_ALAC = "audio/alac";
  static constexpr auto AUDIO_MSGSM = "audio/gsm";
  static constexpr auto AUDIO_OGG = "audio/ogg";
  static constexpr auto AUDIO_WAV = "audio/wav";
  static constexpr auto AUDIO_MIDI = "audio/midi";
  static constexpr auto AUDIO_IAMF = "audio/iamf";
  static constexpr auto AUDIO_UNKNOWN = "audio/x-unknown";

  /// video MIME types
  static constexpr auto VIDEO_MP4 = "video/mp4";
  static constexpr auto VIDEO_MATROSKA = "video/x-matroska";
  static constexpr auto VIDEO_WEBM = "video/webm";
  static constexpr auto VIDEO_H263 = "video/3gpp";
  static constexpr auto VIDEO_H264 = "video/avc";
  static constexpr auto VIDEO_H265 = "video/hevc";
  static constexpr auto VIDEO_APV = "video/apv";
  static constexpr auto VIDEO_VP8 = "video/x-vnd.on2.vp8";
  static constexpr auto VIDEO_VP9 = "video/x-vnd.on2.vp9";
  static constexpr auto VIDEO_AV1 = "video/av01";
  static constexpr auto VIDEO_MP2T = "video/mp2t";
  static constexpr auto VIDEO_MP4V = "video/mp4v-es";
  static constexpr auto VIDEO_MPEG = "video/mpeg";
  static constexpr auto VIDEO_PS = "video/mp2p";
  static constexpr auto VIDEO_MPEG2 = "video/mpeg2";
  static constexpr auto VIDEO_VC1 = "video/wvc1";
  static constexpr auto VIDEO_DIVX = "video/divx";
  static constexpr auto VIDEO_FLV = "video/x-flv";
  static constexpr auto VIDEO_DOLBY_VISION = "video/dolby-vision";
  static constexpr auto VIDEO_OGG = "video/ogg";
  static constexpr auto VIDEO_AVI = "video/x-msvideo";
  static constexpr auto VIDEO_MJPEG = "video/mjpeg";
  static constexpr auto VIDEO_MP42 = "video/mp42";
  static constexpr auto VIDEO_MP43 = "video/mp43";
  static constexpr auto VIDEO_MV_HEVC = "video/mv-hevc";
  static constexpr auto VIDEO_RAW = "video/raw";
  static constexpr auto VIDEO_UNKNOWN = "video/x-unknown";

  /// text/ MIME types
  static constexpr auto TEXT_VTT = "text/vtt";
  static constexpr auto TEXT_SSA = "text/x-ssa";
  static constexpr auto TEXT_UNKNOWN = "text/x-unknown";

  /// application/ MIME types
  static constexpr auto APPLICATION_MP4 = "application/mp4";
  static constexpr auto APPLICATION_WEBM = "application/webm";
  static constexpr auto APPLICATION_MATROSKA = "application/x-matroska";
  static constexpr auto APPLICATION_MPD = "application/dash+xml";
  static constexpr auto APPLICATION_M3U8 = "application/x-mpegURL";
  static constexpr auto APPLICATION_SS = "application/vnd.ms-sstr+xml";
  static constexpr auto APPLICATION_ID3 = "application/id3";
  static constexpr auto APPLICATION_CEA608 = "application/cea-608";
  static constexpr auto APPLICATION_CEA708 = "application/cea-708";
  static constexpr auto APPLICATION_SUBRIP = "application/x-subrip";
  static constexpr auto APPLICATION_TTML = "application/ttml+xml";
  static constexpr auto APPLICATION_TX3G = "application/x-quicktime-tx3g";
  static constexpr auto APPLICATION_MP4VTT = "application/x-mp4-vtt";
  static constexpr auto APPLICATION_MP4CEA608 = "application/x-mp4-cea-608";
  static constexpr auto APPLICATION_RAWCC = "application/x-rawcc";
  static constexpr auto APPLICATION_VOBSUB = "application/vobsub";
  static constexpr auto APPLICATION_PGS = "application/pgs";
  static constexpr auto APPLICATION_SCTE35 = "application/x-scte35";
  static constexpr auto APPLICATION_SDP = "application/sdp";
  static constexpr auto APPLICATION_DEPTH_METADATA = "application/x-depth-metadata";
  static constexpr auto APPLICATION_EMSG = "application/x-emsg";
  static constexpr auto APPLICATION_DVBSUBS = "application/dvbsubs";
  static constexpr auto APPLICATION_EXIF = "application/x-exif";
  static constexpr auto APPLICATION_ICY = "application/x-icy";
  static constexpr auto APPLICATION_AIT = "application/vnd.dvb.ait";
  static constexpr auto APPLICATION_RTSP = "application/x-rtsp";
  static constexpr auto APPLICATION_MEDIA3_CUES = "application/x-media3-cues";
  /**
   * MIME type for an image URI loaded from an external image management framework.
   */
  static constexpr auto APPLICATION_EXTERNALLY_LOADED_IMAGE = "application/x-image-uri";

  /// image/ MIME types
  static constexpr auto IMAGE_JPEG = "image/jpeg";
  static constexpr auto IMAGE_JPEG_R = "image/jpeg_r";
  static constexpr auto IMAGE_PNG = "image/png";
  static constexpr auto IMAGE_HEIF = "image/heif";
  static constexpr auto IMAGE_HEIC = "image/heic";
  static constexpr auto IMAGE_AVIF = "image/avif";
  static constexpr auto IMAGE_BMP = "image/bmp";
  static constexpr auto IMAGE_WEBP = "image/webp";
  static constexpr auto IMAGE_RAW = "image/raw";

  /**
   * A non-standard codec string for E-AC3-JOC. Use of this constant allows for
   * disambiguation between regular E-AC3 ("ec-3") and E-AC3-JOC ("ec+3")
   * streams from the codec string alone. The standard is to use "ec-3" for
   * both, as per the [MP4RA registered codec types](https://mp4ra.org/#/codecs).
   */
  static constexpr auto CODEC_E_AC3_JOC = "ec+3";

  /**
   * Returns whether the given string is an audio MIME type.
   */
  static bool isAudio(const std::string& mimeType) {
    return startsWith(mimeType, TYPE_AUDIO);
  }

  /**
   * Returns whether the given string is a video MIME type.
   */
  static bool isVideo(const std::string& mimeType) {
    return startsWith(mimeType, TYPE_VIDEO);
  }

  /**
   * Returns whether the given {@code mimeType} is a Matroska MIME type, including WebM.
   */
  static bool isMatroska(const std::string& mimeType);

  /**
   * Returns whether the given string is a text MIME type, including known text
   * types that use &quot;application&quot; as their base type.
   */
  static bool isText(const std::string& mimeType);

  /**
   * Returns whether the given string is an image MIME type.
   */
  static bool isImage(const std::string& mimeType) {
    return startsWith(mimeType, TYPE_IMAGE) || APPLICATION_EXTERNALLY_LOADED_IMAGE == mimeType;
  }

  /**
   * Returns true if it is known that all samples in a stream of the given MIME
   * type and codec are guaranteed to be sync samples (i.e., {@link
   * C#BUFFER_FLAG_KEY_FRAME} is guaranteed to be set on every sample) and the
   * inherent duration of each sample is negligible (i.e., we never expect to
   * require a sample because playback partially falls into its duration).
   *
   * @param mimeType The MIME type of the stream.
   * @param codec The RFC 6381 codec string of the stream, or empty if unknown.
   *
   * @return Whether it is known that all samples in the stream are guaranteed
   * to be sync samples.
   */
  static bool allSamplesAreSyncSamples(const std::string& mimeType, const std::string& codec);

  /**
   * Returns the first video MIME type derived from an RFC 6381 codecs string.
   *
   * @param codecs An RFC 6381 codecs string.
   * @return The first derived video MIME type, or empty.
   */
  static std::string getVideoMediaMimeType(const std::string& codecs);

  /**
   * Returns whether the given {@code codecs} string contains a codec which
   * corresponds to the given {@code mimeType}.
   *
   * @param codecs An RFC 6381 codecs string.
   * @param mimeType A MIME type to look for.
   *
   * @return Whether the given {@code codecs} string contains a codec which
   * corresponds to the given {@code mimeType}.
   */
  static bool containsCodecsCorrespondingToMimeType(const std::string& codecs, const std::string& mimeType) {
    return !getCodecsCorrespondingToMimeType(codecs, mimeType).empty();
  }

  /**
   * Returns a subsequence of {@code codecs} containing the codec strings that
   * correspond to the given {@code mimeType}. Returns null if {@code mimeType}
   * is null, {@code codecs} is null, or
   * {@code codecs} does not contain a codec that corresponds to {@code
   * mimeType}.
   *
   * @param codecs An RFC 6381 codecs string.
   * @param mimeType A MIME type to look for.
   *
   * @return A subsequence of {@code codecs} containing the codec strings that
   * correspond to the given {@code mimeType}. Returns empty if {@code mimeType}
   * is empty, {@code codecs} is empty, or {@code codecs} does not contain a codec
   * that corresponds to {@code mimeType}.
   */
  static std::string getCodecsCorrespondingToMimeType(const std::string& codecs, const std::string& mimeType);

  /**
   * Returns the first audio MIME type derived from an RFC 6381 codecs string.
   *
   * @param codecs An RFC 6381 codecs string.
   *
   * @return The first derived audio MIME type, or {@code null}.
   */
  static std::string getAudioMediaMimeType(const std::string& codecs);

  /**
   * Returns the first text MIME type derived from an RFC 6381 codecs string.
   *
   * @param codecs An RFC 6381 codecs string.
   *
   * @return The first derived text MIME type, or {@code null}.
   */
  static std::string getTextMediaMimeType(const std::string& codecs);

  /**
   * Returns the MIME type corresponding to an RFC 6381 codec string, or {@code
   * null} if it could not be determined.
   *
   * @param codec An RFC 6381 codec string.
   *
   * @return The corresponding MIME type, or {@code null} if it could not be determined.
   */
  static std::string getMediaMimeType(const std::string& codec);

  /**
   * Returns the MP4 object type identifier corresponding to a MIME type, as defined in RFC 6381
   * and <a href="https://mp4ra.org/registered-types/object-types">MPEG-4 Object Types</a>.
   *
   * @param sampleMimeType The MIME type of the track.
   *
   * @return The corresponding MP4 object type identifier, or {@code null} if it could not be determined.
   */
  static std::optional<uint8_t> getMp4ObjectTypeFromMimeType(const std::string& sampleMimeType);

  /**
   * Returns the MIME type corresponding to an MP4 object type identifier, as
   * defined in RFC 6381 and https://mp4ra.org/#/object_types.
   *
   * @param objectType An MP4 object type identifier.
   *
   * @return The corresponding MIME type, or {@code null} if it could not be determined.
   */
  static std::string getMimeTypeFromMp4ObjectType(int objectType);

  /**
   * Returns whether the given {@code codecs} and {@code supplementalCodecs}
   * correspond to a valid Dolby Vision codec.
   *
   * @param codecs An RFC 6381 codecs string for the base codec. may be null.
   * @param supplementalCodecs An optional RFC 6381 codecs string for supplemental codecs.
   *
   * @return Whether the given {@code codecs} and {@code supplementalCodecs}
   * correspond to a valid Dolby Vision codec.
   */
  static bool isDolbyVisionCodec(const std::string& codecs, const std::string& supplementalCodecs);

  /**
   * Returns the {@link MediaType type} constant corresponding to a specified
   * MIME type, which may be {@link MediaType::kMediaTypeInvalid} if it could
   * not be determined.
   *
   * @param mimeType A MIME type.
   *
   * @return The corresponding {@link C.TrackType track type}, which may be
   * {@link MediaType::kMediaTypeInvalid} if it could not be determined.
   */
  static MediaType getMediaType(const std::string& mimeType);

  /**
   * Normalizes the MIME type provided so that equivalent MIME types are
   * uniquely represented.
   *
   * @param mime A MIME type to normalize, or null.
   *
   * @return The normalized MIME type, or the argument MIME type if its normalized form is unknown.
   */
  static std::string normalizeMimeType(const std::string& mime);

  /**
   * Returns the available audio MIME types
   */
  static const std::vector<std::string>& audioAvailableMimeTypes();

private:
  MimeTypes() = default;
  ~MimeTypes() = default;

  static std::string getCustomMimeTypeForCodec(const std::string& codec);

  static MediaType getMediaTypeForCustomMimeType(const std::string& mimeType);

  /**
   * Represents an audio encoding, or an invalid or unset value.
   */
  enum class AudioEncoding : int32_t {
    ENCODING_INVALID = static_cast<int32_t>(AudioSampleFormat::kInvalid),
    ENCODING_PCM_16BIT = static_cast<int32_t>(AudioSampleFormat::kInt16),
    ENCODING_PCM_FLOAT = static_cast<int32_t>(AudioSampleFormat::kFloat),
    ENCODING_PCM_24BIT = static_cast<int32_t>(AudioSampleFormat::kInt24),
    ENCODING_PCM_32BIT = static_cast<int32_t>(AudioSampleFormat::kInt32),
    ENCODING_MP3 = 5,
    ENCODING_AAC_LC = 6,
    ENCODING_AAC_HE_V1 = 7,
    ENCODING_AAC_HE_V2 = 8,
    ENCODING_AAC_XHE = 9,
    ENCODING_AAC_ELD = 10,
    ENCODING_AAC_ER_BSAC = 11,
    ENCODING_AC3 = 12,
    ENCODING_E_AC3 = 13,
    ENCODING_E_AC3_JOC = 14,
    ENCODING_AC4 = 15,
    ENCODING_DTS = 16,
    ENCODING_DTS_HD = 17,
    ENCODING_DOLBY_TRUEHD = 18,
    ENCODING_OPUS = 19,
    ENCODING_DTS_UHD_P2 = 20,
  };

  /**
   * An MP4A Object Type Indication (OTI) and its optional audio OTI is defined by RFC 6381.
   */
  struct Mp4aObjectType {
    /**
     * The Object Type Indication of the MP4A codec.
     */
    int objectTypeIndication;

    /**
     * The Audio Object Type Indication of the MP4A codec, or 0 if it is absent.
     */
    int audioObjectTypeIndication;

    explicit Mp4aObjectType(const int objectTypeIndication, const int audioObjectTypeIndication) noexcept
      : objectTypeIndication(objectTypeIndication),
        audioObjectTypeIndication(audioObjectTypeIndication) {
    }

    [[nodiscard]] AudioEncoding getEncoding() const {
      switch (audioObjectTypeIndication) {
        case 2: {
          return AudioEncoding::ENCODING_AAC_LC;
        }
        case 5: {
          return AudioEncoding::ENCODING_AAC_HE_V1;
        }
        case 29: {
          return AudioEncoding::ENCODING_AAC_HE_V2;
        }
        case 42: {
          return AudioEncoding::ENCODING_AAC_XHE;
        }
        case 23: {
          return AudioEncoding::ENCODING_AAC_ELD;
        }
        case 22: {
          return AudioEncoding::ENCODING_AAC_ER_BSAC;
        }
        default: {
          return AudioEncoding::ENCODING_INVALID;
        }
      }
    }
  };

  /**
   * Returns the {@link Mp4aObjectType} of an RFC 6381 MP4 audio codec string.
   *
   * Per https://mp4ra.org/#/object_types and
   * https://tools.ietf.org/html/rfc6381#section-3.3, an MP4 codec string has
   * the form:
   *
   * <pre>
   *         ~~~~~~~~~~~~~~ Object Type Indication (OTI) byte in hex
   *    mp4a.[a-zA-Z0-9]{2}(.[0-9]{1,2})?
   *                         ~~~~~~~~~~ audio OTI, decimal. Only for certain
   * OTI.
   * </pre>
   *
   * For example, mp4a.40.2 has an OTI of 0x40 and an audio OTI of 2.
   *
   * @param codec An RFC 6381 MP4 audio codec string.
   *
   * @return The {@link Mp4aObjectType}, or empty if the input was invalid.
   */
  static std::optional<Mp4aObjectType>
  getObjectTypeFromMp4aRFC6381CodecString(const std::string& codec);

  struct CustomMimeType {
    const std::string mimeType;
    const std::string codecPrefix;
    const MediaType mediaType;

    explicit CustomMimeType(std::string mimeType, std::string codecPrefix, const MediaType mediaType) noexcept
      : mimeType(std::move(mimeType)),
        codecPrefix(std::move(codecPrefix)),
        mediaType(mediaType) {
    }
  };

  static constexpr auto MP4A_RFC_6381_CODEC = "^mp4a\\.([a-zA-Z0-9]{2})(?:\\.([0-9]{1,2}))?$";

  static bool startsWith(const std::string& str, const std::string& prefix) {
    return str.size() >= prefix.size() &&
           str.compare(0, prefix.size(), prefix) == 0;
  }

  static std::vector<std::string> splitCodecs(const std::string& codecs);

  static const std::vector<CustomMimeType>& getCustomMimeTypes();
};

} // namespace coral

#endif //CORAL_MEDIA_TYPES_H
