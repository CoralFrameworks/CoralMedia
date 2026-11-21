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

#include <media/Types.h>

#include "PathUtils.h"

#include <cstring>
#include <regex>
#include <string>

namespace coral {

int FileTypes::inferFileTypeFromMimeType(const std::string& mimeType) {
  if (mimeType.empty()) {
    return UNKNOWN;
  }
  auto const& mime = MimeTypes::normalizeMimeType(mimeType);
  if (mime == MimeTypes::AUDIO_AC3 || mime == MimeTypes::AUDIO_E_AC3 ||
      mime == MimeTypes::AUDIO_E_AC3_JOC) {
    return AC3;
  }
  if (mime == MimeTypes::AUDIO_AC4) {
    return AC4;
  }
  if (mime == MimeTypes::AUDIO_AMR || mime == MimeTypes::AUDIO_AMR_NB ||
      mime == MimeTypes::AUDIO_AMR_WB) {
    return AMR;
  }
  if (mime == MimeTypes::AUDIO_FLAC) {
    return FLAC;
  }
  if (mime == MimeTypes::VIDEO_FLV) {
    return FLV;
  }
  if (mime == MimeTypes::AUDIO_MIDI) {
    return MIDI;
  }
  if (mime == MimeTypes::VIDEO_MATROSKA || mime == MimeTypes::AUDIO_MATROSKA ||
      mime == MimeTypes::VIDEO_WEBM || mime == MimeTypes::AUDIO_WEBM ||
      mime == MimeTypes::APPLICATION_WEBM) {
    return MATROSKA;
  }
  if (mime == MimeTypes::AUDIO_MPEG) {
    return MP3;
  }
  if (mime == MimeTypes::VIDEO_MP4 || mime == MimeTypes::AUDIO_MP4 ||
      mime == MimeTypes::APPLICATION_MP4) {
    return MP4;
  }
  if (mime == MimeTypes::AUDIO_OGG) {
    return OGG;
  }
  if (mime == MimeTypes::VIDEO_PS) {
    return PS;
  }
  if (mime == MimeTypes::VIDEO_MP2T) {
    return TS;
  }
  if (mime == MimeTypes::AUDIO_WAV) {
    return WAV;
  }
  if (mime == MimeTypes::VIDEO_AVI) {
    return AVI;
  }
  if (mime == MimeTypes::TEXT_VTT) {
    return WEBVTT;
  }
  if (mime == MimeTypes::APPLICATION_SUBRIP) {
    return SRT;
  }
  if (mime == MimeTypes::TEXT_SSA) {
    return SSA;
  }
  if (mime == MimeTypes::IMAGE_JPEG) {
    return JPEG;
  }
  if (mime == MimeTypes::IMAGE_PNG) {
    return PNG;
  }
  if (mime == MimeTypes::IMAGE_WEBP) {
    return WEBP;
  }
  if (mime == MimeTypes::IMAGE_BMP) {
    return BMP;
  }
  if (mime == MimeTypes::IMAGE_HEIF || mime == MimeTypes::IMAGE_HEIC) {
    return HEIF;
  }
  if (mime == MimeTypes::IMAGE_AVIF) {
    return AVIF;
  }
  return UNKNOWN;
}

int FileTypes::inferFileTypeFromUrl(const std::string& url) {
  if (!PathUtils::exists(url.c_str()) || !PathUtils::isFile(url.c_str())) {
    return UNKNOWN;
  }
  const auto filename = PathUtils::getName(url);
  if (endsWith(filename, EXTENSION_AC3) || endsWith(filename, EXTENSION_EC3)) {
    return AC3;
  } else if (endsWith(filename, EXTENSION_AC4)) {
    return AC4;
  } else if (endsWith(filename, EXTENSION_ADTS) ||
             endsWith(filename, EXTENSION_AAC)) {
    return ADTS;
  } else if (endsWith(filename, EXTENSION_AMR)) {
    return AMR;
  } else if (endsWith(filename, EXTENSION_FLAC)) {
    return FLAC;
  } else if (endsWith(filename, EXTENSION_FLV)) {
    return FLV;
  } else if (endsWith(filename, EXTENSION_MID) ||
             endsWith(filename, EXTENSION_MIDI) ||
             endsWith(filename, EXTENSION_SMF)) {
    return MIDI;
  } else if (startsWith(filename, EXTENSION_PREFIX_MK,
               filename.length() -
               (strlen(EXTENSION_PREFIX_MK) + 1)) ||
             endsWith(filename, EXTENSION_WEBM)) {
    return MATROSKA;
  } else if (endsWith(filename, EXTENSION_MP3)) {
    return MP3;
  } else if (endsWith(filename, EXTENSION_MP4) ||
             startsWith(filename, EXTENSION_PREFIX_M4,
               filename.length() -
               (strlen(EXTENSION_PREFIX_M4) + 1)) ||
             startsWith(filename, EXTENSION_PREFIX_MP4,
               filename.length() -
               (strlen(EXTENSION_PREFIX_MP4) + 1)) ||
             startsWith(filename, EXTENSION_PREFIX_CMF,
               filename.length() -
               (strlen(EXTENSION_PREFIX_CMF) + 1))) {
    return MP4;
  } else if (startsWith(filename, EXTENSION_PREFIX_OG,
               filename.length() -
               (strlen(EXTENSION_PREFIX_OG) + 1)) ||
             endsWith(filename, EXTENSION_OPUS)) {
    return OGG;
  } else if (endsWith(filename, EXTENSION_PS) ||
             endsWith(filename, EXTENSION_MPEG) ||
             endsWith(filename, EXTENSION_MPG) ||
             endsWith(filename, EXTENSION_M2P)) {
    return PS;
  } else if (endsWith(filename, EXTENSION_TS) ||
             startsWith(filename, EXTENSION_PREFIX_TS,
               filename.length() -
               (strlen(EXTENSION_PREFIX_TS) + 1))) {
    return TS;
  } else if (endsWith(filename, EXTENSION_WAV) ||
             endsWith(filename, EXTENSION_WAVE)) {
    return WAV;
  } else if (endsWith(filename, EXTENSION_VTT) ||
             endsWith(filename, EXTENSION_WEBVTT)) {
    return WEBVTT;
  } else if (endsWith(filename, EXTENSION_SRT)) {
    return SRT;
  } else if (endsWith(filename, EXTENSION_SSA) || endsWith(filename, EXTENSION_ASS)) {
    return SSA;
  } else if (endsWith(filename, EXTENSION_JPG) ||
             endsWith(filename, EXTENSION_JPEG)) {
    return JPEG;
  } else if (endsWith(filename, EXTENSION_AVI)) {
    return AVI;
  } else if (endsWith(filename, EXTENSION_PNG)) {
    return PNG;
  } else if (endsWith(filename, EXTENSION_WEBP)) {
    return WEBP;
  } else if (endsWith(filename, EXTENSION_BMP) ||
             endsWith(filename, EXTENSION_DIB)) {
    return BMP;
  } else if (endsWith(filename, EXTENSION_HEIC) ||
             endsWith(filename, EXTENSION_HEIF)) {
    return HEIF;
  } else if (endsWith(filename, EXTENSION_AVIF)) {
    return AVIF;
  } else {
    return UNKNOWN;
  }
}

//----------------------------------------------------------------------------------------------------------------------

bool MimeTypes::isMatroska(const std::string& mimeType) {
  if (mimeType.empty()) {
    return false;
  }
  return startsWith(mimeType, VIDEO_WEBM) || startsWith(mimeType, AUDIO_WEBM) ||
         startsWith(mimeType, APPLICATION_WEBM) ||
         startsWith(mimeType, VIDEO_MATROSKA) ||
         startsWith(mimeType, AUDIO_MATROSKA) ||
         startsWith(mimeType, APPLICATION_MATROSKA);
}

bool MimeTypes::isText(const std::string& mimeType) {
  return startsWith(mimeType, TYPE_TEXT) ||
         APPLICATION_MEDIA3_CUES == mimeType ||
         APPLICATION_CEA608 == mimeType || APPLICATION_CEA708 == mimeType ||
         APPLICATION_MP4CEA608 == mimeType || APPLICATION_SUBRIP == mimeType ||
         APPLICATION_TTML == mimeType || APPLICATION_TX3G == mimeType ||
         APPLICATION_MP4VTT == mimeType || APPLICATION_RAWCC == mimeType ||
         APPLICATION_VOBSUB == mimeType || APPLICATION_PGS == mimeType ||
         APPLICATION_DVBSUBS == mimeType;
}

bool MimeTypes::allSamplesAreSyncSamples(const std::string& mimeType, const std::string& codec) {
  if (mimeType.empty()) {
    return false;
  }
  // TODO: Add additional audio MIME types. Also consider evaluating based on
  // Format rather than just MIME type, since in some cases the property is true
  // for a subset of the profiles belonging to a single MIME type. If we do
  // this, we should move the method to a different class.
  if (mimeType == AUDIO_MPEG
      || mimeType == AUDIO_MPEG_L1
      || mimeType == AUDIO_MPEG_L2
      || mimeType == AUDIO_RAW
      || mimeType == AUDIO_G711_ALAW
      || mimeType == AUDIO_G711_MLAW
      || mimeType == AUDIO_FLAC
      || mimeType == AUDIO_FLAC
      || mimeType == AUDIO_AC3
      || mimeType == AUDIO_E_AC3
      || mimeType == AUDIO_E_AC3_JOC) {
    return true;
  }
  if (mimeType == AUDIO_AAC) {
    if (codec.empty()) {
      return false;
    }
    auto objectType = getObjectTypeFromMp4aRFC6381CodecString(codec);
    if (objectType == std::nullopt) {
      return false;
    }
    const auto encoding = objectType->getEncoding();
    // xHE-AAC is an exception in which it's not true that all samples will be
    // sync samples. Also return false for ENCODING_INVALID, which indicates we
    // weren't able to parse the encoding from the codec string.
    return encoding != AudioEncoding::ENCODING_INVALID &&
           encoding != AudioEncoding::ENCODING_AAC_XHE;
  }
  return false;
}

std::string MimeTypes::getVideoMediaMimeType(const std::string& codecs) {
  if (codecs.empty()) {
    return "";
  }
  auto codecList = splitCodecs(codecs);
  for (auto const& codec: codecList) {
    auto mimeType = getMediaMimeType(codec);
    if (!mimeType.empty() && isVideo(mimeType)) {
      return mimeType;
    }
  }
  return "";
}

std::string
MimeTypes::getCodecsCorrespondingToMimeType(const std::string& codecs, const std::string& mimeType) {
  if (codecs.empty() || mimeType.empty()) {
    return "";
  }
  auto codecList = splitCodecs(codecs);
  auto codecStr = std::string();
  for (auto const& codec: codecList) {
    if (mimeType == getMediaMimeType(codec)) {
      if (codecStr.size() > 0) {
        codecStr.append(",");
      }
      codecStr.append(codec);
    }
  }
  return codecStr.size() > 0 ? codecStr : "";
}

std::string MimeTypes::getAudioMediaMimeType(const std::string& codecs) {
  if (codecs.empty()) {
    return "";
  }
  auto codecList = splitCodecs(codecs);
  for (auto const& codec: codecList) {
    auto mimeType = getMediaMimeType(codec);
    if (!mimeType.empty() && isAudio(mimeType)) {
      return mimeType;
    }
  }
  return "";
}

std::string MimeTypes::getTextMediaMimeType(const std::string& codecs) {
  if (codecs.empty()) {
    return "";
  }
  auto codecList = splitCodecs(codecs);
  for (auto const& codec: codecList) {
    auto mimeType = getMediaMimeType(codec);
    if (!mimeType.empty() && isText(mimeType)) {
      return mimeType;
    }
  }
  return "";
}

std::string MimeTypes::getMediaMimeType(const std::string& codec) {
  if (codec.empty()) {
    return "";
  }

  // convert to lower case
  std::string codecType = codec;
  std::transform(codecType.begin(), codecType.end(), codecType.begin(),
    [](unsigned char c) { return std::tolower(c); });
  if (startsWith(codecType, "avc1") || startsWith(codecType, "avc3")) {
    return VIDEO_H264;
  } else if (startsWith(codecType, "hev1") || startsWith(codecType, "hvc1")) {
    return VIDEO_H265;
  } else if (startsWith(codecType, "dvav") || startsWith(codecType, "dva1")
             || startsWith(codecType, "dvhe") || startsWith(codecType, "dvh1")) {
    return VIDEO_DOLBY_VISION;
  } else if (startsWith(codecType, "av01")) {
    return VIDEO_AV1;
  } else if (startsWith(codecType, "vp9") || startsWith(codecType, "vp09")) {
    return VIDEO_VP9;
  } else if (startsWith(codecType, "vp8") || startsWith(codecType, "vp08")) {
    return VIDEO_VP8;
  } else if (startsWith(codecType, "mp4a")) {
    std::string mimeType;
    if (startsWith(codecType, "mp4a.")) {
      auto objectType = getObjectTypeFromMp4aRFC6381CodecString(codec);
      if (objectType != std::nullopt) {
        mimeType =
          getMimeTypeFromMp4ObjectType(objectType->objectTypeIndication);
      }
    }
    return mimeType.empty() ? AUDIO_AAC : mimeType;
  } else if (startsWith(codecType, "mha1")) {
    return AUDIO_MPEGH_MHA1;
  } else if (startsWith(codecType, "mhm1")) {
    return AUDIO_MPEGH_MHM1;
  } else if (startsWith(codecType, "ac-3") || startsWith(codecType, "dac3")) {
    return AUDIO_AC3;
  } else if (startsWith(codecType, "ec-3") || startsWith(codecType, "dec3")) {
    return AUDIO_E_AC3;
  } else if (startsWith(codecType, CODEC_E_AC3_JOC)) {
    return AUDIO_E_AC3_JOC;
  } else if (startsWith(codecType, "ac-4") || startsWith(codecType, "dac4")) {
    return AUDIO_AC4;
  } else if (startsWith(codecType, "dtsc")) {
    return AUDIO_DTS;
  } else if (startsWith(codecType, "dtse")) {
    return AUDIO_DTS_EXPRESS;
  } else if (startsWith(codecType, "dtsh") || startsWith(codecType, "dtsl")) {
    return AUDIO_DTS_HD;
  } else if (startsWith(codecType, "dtsx")) {
    return AUDIO_DTS_X;
  } else if (startsWith(codecType, "opus")) {
    return AUDIO_OPUS;
  } else if (startsWith(codecType, "vorbis")) {
    return AUDIO_VORBIS;
  } else if (startsWith(codecType, "flac")) {
    return AUDIO_FLAC;
  } else if (startsWith(codecType, "stpp")) {
    return APPLICATION_TTML;
  } else if (startsWith(codecType, "wvtt")) {
    return TEXT_VTT;
  } else if (codecType.find("cea708") != std::string::npos) {
    return APPLICATION_CEA708;
  } else if (codecType.find("eia608") != std::string::npos ||
             codecType.find("cea608") != std::string::npos) {
    return APPLICATION_CEA608;
  } else {
    return getCustomMimeTypeForCodec(codec);
  }
}

std::optional<uint8_t>
MimeTypes::getMp4ObjectTypeFromMimeType(const std::string& sampleMimeType) {
  if (AUDIO_AAC == sampleMimeType) {
    return 0x40;
  }
  if (AUDIO_VORBIS == sampleMimeType) {
    return 0xDD;
  }
  if (VIDEO_MP4V == sampleMimeType) {
    return 0x20;
  }
  return std::nullopt;
}

std::string MimeTypes::getMimeTypeFromMp4ObjectType(const int objectType) {
  switch (objectType) {
    case 0x20: {
      return VIDEO_MP4V;
    }
    case 0x21: {
      return VIDEO_H264;
    }
    case 0x23: {
      return VIDEO_H265;
    }
    case 0x60:
    case 0x61:
    case 0x62:
    case 0x63:
    case 0x64:
    case 0x65: {
      return VIDEO_MPEG2;
    }
    case 0x6A: {
      return VIDEO_MPEG;
    }
    case 0x69:
    case 0x6B: {
      return AUDIO_MPEG;
    }
    case 0x6C: {
      return IMAGE_JPEG;
    }
    case 0xA3: {
      return VIDEO_VC1;
    }
    case 0xB1: {
      return VIDEO_VP9;
    }
    case 0x40:
    case 0x66:
    case 0x67:
    case 0x68: {
      return AUDIO_AAC;
    }
    case 0xA5: {
      return AUDIO_AC3;
    }
    case 0xA6: {
      return AUDIO_E_AC3;
    }
    case 0xA9:
    case 0xAC: {
      return AUDIO_DTS;
    }
    case 0xAA:
    case 0xAB: {
      return AUDIO_DTS_HD;
    }
    case 0xAD: {
      return AUDIO_OPUS;
    }
    case 0xAE: {
      return AUDIO_AC4;
    }
    case 0xDD: {
      return AUDIO_VORBIS;
    }
    default: {
      return "";
    }
  }
}

bool MimeTypes::isDolbyVisionCodec(const std::string& codecs, const std::string& supplementalCodecs) {
  if (codecs.empty()) {
    return false;
  }
  if (startsWith(codecs, "dvhe") || startsWith(codecs, "dvh1")) {
    // profile 5
    return true;
  }
  if (supplementalCodecs.empty()) {
    return false;
  }
  // profiles 8, 9 and 10
  return (startsWith(supplementalCodecs, "dvhe") && startsWith(codecs, "hev1"))
         || (startsWith(supplementalCodecs, "dvh1") && startsWith(codecs, "hvc1"))
         || (startsWith(supplementalCodecs, "dvav") && startsWith(codecs, "avc3"))
         || (startsWith(supplementalCodecs, "dva1") && startsWith(codecs, "avc1"))
         || (startsWith(supplementalCodecs, "dav1") && startsWith(codecs, "av01"));
}

MediaType MimeTypes::getMediaType(const std::string& mime) {
  auto mimeType = normalizeMimeType(mime);
  if (mimeType.empty()) {
    return MediaType::kMediaTypeInvalid;
  } else if (isAudio(mimeType)) {
    return MediaType::kMediaTypeAudio;
  } else if (isVideo(mimeType)) {
    return MediaType::kMediaTypeVideo;
  } else if (isText(mimeType)) {
    return MediaType::kMediaTypeSubtitle;
  } else if (isImage(mimeType)) {
    return MediaType::kMediaTypeImage;
  } else if (APPLICATION_ID3 == mimeType
             || APPLICATION_EMSG == mimeType
             || APPLICATION_SCTE35 == mimeType
             || APPLICATION_ICY == mimeType
             || APPLICATION_AIT == mimeType) {
    return MediaType::kMediaTypeMetadata;
  } else {
    return getMediaTypeForCustomMimeType(mimeType);
  }
}

std::string MimeTypes::normalizeMimeType(const std::string& mime) {
  if (mime.empty()) {
    return {};
  }
  // convert to lower case
  std::string mimeType = mime;
  std::transform(mimeType.begin(), mimeType.end(), mimeType.begin(),
    [](unsigned char c) { return std::tolower(c); });
  if (mimeType == "video/x-mvhevc") {
    // Normalize uncommon versions of some video MIME types to their standard
    // equivalent.
    return VIDEO_MV_HEVC;
  }
  if (mimeType == "audio/x-flac") {
    // Normalize uncommon versions of some audio MIME types to their standard
    // equivalent.
    return AUDIO_FLAC;
  }
  if (mimeType == "audio/mp3") {
    return AUDIO_MPEG;
  }
  if (mimeType == "audio/x-wav") {
    return AUDIO_WAV;
  }
  if (mimeType == "application/x-mpegurl") {
    // Normalize MIME types that are often written with upper-case letters to
    // their common form.
    return APPLICATION_M3U8;
  }
  if (mimeType == "audio/mpeg-l1") {
    return AUDIO_MPEG_L1;
  }
  if (mimeType == "audio/mpeg-l2") {
    return AUDIO_MPEG_L2;
  }
  return mimeType;
}

const std::vector<std::string>& MimeTypes::audioAvailableMimeTypes() {
  static const std::vector<std::string> sAudioMimeTypes = {
    AUDIO_MP4, AUDIO_AAC, AUDIO_MATROSKA, AUDIO_WEBM,
    AUDIO_MPEG, AUDIO_MPEG_L1, AUDIO_MPEG_L2, AUDIO_MPEGH_MHA1,
    AUDIO_MPEGH_MHM1, AUDIO_RAW, AUDIO_G711_ALAW, AUDIO_G711_MLAW,
    AUDIO_AC3, AUDIO_E_AC3, AUDIO_E_AC3_JOC, AUDIO_AC4,
    AUDIO_TRUEHD, AUDIO_DTS, AUDIO_DTS_HD, AUDIO_DTS_EXPRESS,
    AUDIO_DTS_X, AUDIO_VORBIS, AUDIO_OPUS, AUDIO_AMR,
    AUDIO_AMR_NB, AUDIO_AMR_WB, AUDIO_FLAC, AUDIO_ALAC,
    AUDIO_MSGSM, AUDIO_OGG, AUDIO_WAV, AUDIO_IAMF };
  return sAudioMimeTypes;
}

std::string MimeTypes::getCustomMimeTypeForCodec(const std::string& codec) {
  for (const auto& customMimeType: getCustomMimeTypes()) {
    if (startsWith(codec, customMimeType.codecPrefix)) {
      return customMimeType.mimeType;
    }
  }
  return "";
}

MediaType
MimeTypes::getMediaTypeForCustomMimeType(const std::string& mimeType) {
  for (const auto& customMimeType: getCustomMimeTypes()) {
    if (mimeType == customMimeType.mimeType) {
      return customMimeType.mediaType;
    }
  }
  return MediaType::kMediaTypeInvalid;
}

std::optional<MimeTypes::Mp4aObjectType>
MimeTypes::getObjectTypeFromMp4aRFC6381CodecString(const std::string& codec) {
  // match mp4a codec with regex
  std::regex pattern(MP4A_RFC_6381_CODEC);
  std::smatch matcher;
  auto match = std::regex_match(codec, matcher, pattern);
  if (!match) {
    return std::nullopt;
  }
  auto objectTypeIndicationHex = matcher.str(1);
  auto audioObjectTypeIndicationDec = matcher.str(2);
  int audioObjectTypeIndication = 0;
  auto objectTypeIndication =
    (int) std::stoul(objectTypeIndicationHex, nullptr, 16);
  if (!audioObjectTypeIndicationDec.empty()) {
    audioObjectTypeIndication =
      std::stoi(audioObjectTypeIndicationDec, nullptr);
    return Mp4aObjectType(objectTypeIndication, audioObjectTypeIndication);
  }
  return std::nullopt;
}

std::vector<std::string> MimeTypes::splitCodecs(const std::string& codecs) {
  if (codecs.empty()) {
    return {};
  }

  // split codecs with regex
  std::regex pattern("(\\s*,\\s*)");
  std::smatch matcher;
  auto match = std::regex_match(codecs, matcher, pattern);
  if (!match) {
    return {};
  }

  std::vector<std::string> codecLists;
  for (auto i = 0; i < matcher.size(); ++i) {
    auto const str = matcher.str(i);
    if (!str.empty()) {
      codecLists.push_back(matcher.str(i));
    }
  }

  return codecLists;
}

const std::vector<MimeTypes::CustomMimeType>& MimeTypes::getCustomMimeTypes() {
  static std::vector<CustomMimeType> sCustomMimeTypes;
  return sCustomMimeTypes;
}

} // namespace coral
