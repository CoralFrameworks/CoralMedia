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

#ifndef CORAL_CODEC_DARWIN_MEDIA_UTILS_H
#define CORAL_CODEC_DARWIN_MEDIA_UTILS_H

#if defined(__APPLE__)

#include <codec/CodecTypes.h>

#include <media/ColorInfo.h>
#include <media/Format.h>

#include <AudioToolbox/AudioToolbox.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace coral::darwin {

// ==================================================================================================
// Logging
// ==================================================================================================

/** Returns the four character code of an OSStatus, e.g. "'bada'", or its signed decimal value. */
[[nodiscard]] std::string osStatusToString(OSStatus status);

/** Logs an OSStatus and returns false, so a caller can write `return logStatus(...)`. */
bool logStatus(OSStatus status, const std::string& message, const std::string& detail = {});

/** Logs a failure that did not come from an OSStatus. */
void logFailure(const std::string& message, const std::string& detail = {});

// ==================================================================================================
// Codec support and codec identifiers
// ==================================================================================================

/** Whether the platform provides a decoder for the video codec. */
[[nodiscard]] bool isVideoDecoderSupported(VideoCodecType codecType);

/** Whether the platform provides an encoder for the video codec. */
[[nodiscard]] bool isVideoEncoderSupported(VideoCodecType codecType);

/** Whether the platform provides a decoder for the audio codec. */
[[nodiscard]] bool isAudioDecoderSupported(AudioFormatType formatType);

/** Whether the platform provides an encoder for the audio codec. */
[[nodiscard]] bool isAudioEncoderSupported(AudioFormatType formatType);

[[nodiscard]] CMVideoCodecType toCMVideoCodecType(VideoCodecType codecType);
[[nodiscard]] VideoCodecType toVideoCodecType(CMVideoCodecType codecType);
[[nodiscard]] AudioFormatID toAudioFormatID(AudioFormatType formatType);
[[nodiscard]] AudioFormatType toAudioFormatType(AudioFormatID formatID);

/** The coral mime type of the video codec, or an empty string when it is not a coral codec. */
[[nodiscard]] std::string mimeTypeOfVideoCodec(VideoCodecType codecType);

/** The coral mime type of the audio codec, or an empty string when it is not a coral codec. */
[[nodiscard]] std::string mimeTypeOfAudioFormat(AudioFormatType formatType);

/** The number of audio frames one compressed packet of the codec carries, or 0 when it varies. */
[[nodiscard]] int defaultFramesPerPacket(AudioFormatType formatType);

/** The coral sample format of a linear PCM AudioStreamBasicDescription. */
[[nodiscard]] AudioSampleFormat sampleFormatOf(const AudioStreamBasicDescription& format);

/** The number of bytes one interleaved audio frame occupies. */
[[nodiscard]] int bytesPerFrameOf(const AudioStreamBasicDescription& format);

// ==================================================================================================
// Audio stream descriptions
// ==================================================================================================

/**
 * Describes the interleaved PCM layout the codec interfaces use.
 *
 * The sample format is expected to be one of `kInt16`, `kInt24`, `kInt32` or `kFloat`; any other
 * value produces an invalid description.
 */
[[nodiscard]] AudioStreamBasicDescription makePcmFormat(int sampleRate, int channelCount,
  AudioSampleFormat sampleFormat);

/** Describes the compressed layout of a codec: no fixed bytes per frame or packet. */
[[nodiscard]] AudioStreamBasicDescription makeCompressedFormat(AudioFormatType formatType,
  int sampleRate, int channelCount);

// ==================================================================================================
// MediaFormat <-> CMFormatDescription
// ==================================================================================================

/**
 * Builds the coral format of a track from the format description of its first sample buffer.
 *
 * The audio codec specific data of the result is the AudioSpecificConfig of an AAC stream (as the
 * extractor module stores it), not the ESDS descriptor CoreMedia reports, so the format can be
 * consumed by the other platform independent decoders as well.
 *
 * @param description   the format description of the track.
 * @param mediaType     whether the track is audio or video.
 * @param containerMime the mime type of the container the track was read from.
 * @param trackId       the identifier the extractor reports for the track.
 *
 * @return the format, or null when the description is not one of the supported codecs.
 */
[[nodiscard]] std::shared_ptr<MediaFormat> createMediaFormat(CMFormatDescriptionRef description,
  MediaType mediaType, const std::string& containerMime, int trackId);

/** Builds a format description of the coral format, or null when the codec data is incomplete. */
[[nodiscard]] CMFormatDescriptionRef createFormatDescription(
  const std::shared_ptr<MediaFormat>& format);

/** Builds the audio format description of the coral format. */
[[nodiscard]] CMFormatDescriptionRef createAudioFormatDescription(const MediaFormat& format);

/** Builds the video format description of the coral format. */
[[nodiscard]] CMFormatDescriptionRef createVideoFormatDescription(const MediaFormat& format);

// ==================================================================================================
// Codec specific data
// ==================================================================================================

/**
 * Splits the H.264 codec specific data of the format into parameter sets without their length prefix.
 *
 * The accepted forms are the ones the extractors of the framework produce: one parameter set per
 * entry with a one byte length prefix, an Annex B byte stream, a four byte length prefixed NAL, an
 * `avcC` configuration record, and a bare parameter set.
 *
 * @return the NAL units, SPS first and PPS second; empty when no parameter set was found.
 */
[[nodiscard]] std::vector<std::vector<uint8_t>> h264ParameterSets(const MediaFormat& format);

/**
 * Splits the HEVC codec specific data of the format into parameter sets without their length prefix.
 *
 * The accepted forms are an `hvcC` configuration record (the form the extractor module stores), an
 * Annex B byte stream, and one parameter set per entry.
 *
 * @return the NAL units, VPS first, then SPS and PPS; empty when no parameter set was found.
 */
[[nodiscard]] std::vector<std::vector<uint8_t>> hevcParameterSets(const MediaFormat& format);

/** Builds an `hvcC` configuration record from HEVC parameter sets. */
[[nodiscard]] std::vector<uint8_t> hvcCFromParameterSets(
  const std::vector<std::vector<uint8_t>>& parameterSets);

/** Extracts the AudioSpecificConfig from an ESDS descriptor; empty when it is not an ESDS stream. */
[[nodiscard]] std::vector<uint8_t> ascFromEsds(const std::vector<uint8_t>& esds);

/**
 * Builds an ESDS descriptor around an AudioSpecificConfig, which is the magic cookie AudioConverter
 * requires for an AAC stream. `bitrate` and `channelCount` only seed the informational fields.
 */
[[nodiscard]] std::vector<uint8_t> esdsFromAsc(const std::vector<uint8_t>& asc, int bitrate,
  int channelCount);

/** The magic cookie AudioConverter needs to decompress the audio format. */
[[nodiscard]] std::vector<uint8_t> audioDecoderMagicCookie(const MediaFormat& format);

/** The magic cookie a `CMAudioFormatDescription` of the audio format stores. */
[[nodiscard]] std::vector<uint8_t> audioFormatDescriptionCookie(const MediaFormat& format);

// ==================================================================================================
// Bitstream helpers
// ==================================================================================================

/** Whether the buffer starts with an Annex B start code. */
[[nodiscard]] bool isAnnexB(const uint8_t* data, size_t size);

/**
 * Converts an H.264/HEVC sample to the four byte length prefixed (AVCC) form a CMSampleBuffer holds.
 *
 * An Annex B sample is converted; a length prefixed sample whose NAL length is four bytes is copied.
 * A sample with `nalLengthSize` of one or two bytes is rewritten with four byte lengths.
 *
 * @return the converted sample, or an empty vector when the sample is malformed.
 */
[[nodiscard]] std::vector<uint8_t> toLengthPrefixed(const uint8_t* data, size_t size,
  int nalLengthSize);

/** Converts a four byte length prefixed sample to an Annex B byte stream. */
[[nodiscard]] std::vector<uint8_t> toAnnexB(const uint8_t* data, size_t size, int nalLengthSize);

/** The length of a NAL unit prefix of the format description, or 4 when it does not say. */
[[nodiscard]] int nalLengthSizeOf(CMVideoFormatDescriptionRef description);

// ==================================================================================================
// Misc
// ==================================================================================================

/** Converts a presentation time to microseconds, or `Definitions::kTimeUnset` when it is invalid. */
[[nodiscard]] int64_t toTimeUs(CMTime time);

/** Converts microseconds to a presentation time. */
[[nodiscard]] CMTime toCMTime(int64_t timeUs);

/** A one line description of the format description, for diagnostics. */
[[nodiscard]] std::string formatDescriptionSummary(CMFormatDescriptionRef description);

/** The color information the format description declares, or an unknown `ColorInfo`. */
[[nodiscard]] ColorInfo colorInfoOf(CMFormatDescriptionRef description);

} // namespace coral::darwin

#endif // defined(__APPLE__)

#endif // CORAL_CODEC_DARWIN_MEDIA_UTILS_H
