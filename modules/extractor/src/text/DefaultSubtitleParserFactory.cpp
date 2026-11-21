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

#include <extractor/text/DefaultSubtitleParserFactory.h>

#include <extractor/text/ssa/SsaParser.h>
#include <extractor/text/subrip/SubripParser.h>
#include <extractor/text/tx3g/Tx3gParser.h>
#include <extractor/text/ttml/TtmlParser.h>
#include <extractor/text/pgs/PgsParser.h>
#include <extractor/text/vobsub/VobSubParser.h>
#include <extractor/text/dvbsub/DvbSubParser.h>
#include <extractor/text/cea608/Cea608Parser.h>
#include <extractor/text/cea708/Cea708Parser.h>
#include <extractor/text/webvtt/Mp4WebvttParser.h>
#include <extractor/text/webvtt/WebvttParser.h>

#include <media/Format.h>
#include <media/Types.h>

#include <cstdint>
#include <memory>
#include <string>

namespace coral {
namespace {

/**
 * The formats that the factory has a parser for.
 *
 * <p>One value per ported format: the factory maps the MIME type of a format to it, and every value
 * has exactly one parser, so that a format cannot be answered for by a parser that does not exist.
 * The list grows as the formats are ported (see `SUBTITLES_AND_METADATA.md`, S1/S2/S8).
 */
enum class ParserKind : uint8_t {
  //!< No parser: the format is not supported.
  kNone = 0,
  //!< A WebVTT document, including the one a container synthesizes per sample.
  kWebvtt = 1,
  //!< The boxes of a WebVTT sample of an MP4 container.
  kMp4Webvtt = 2,
  //!< A SubRip document.
  kSubrip = 3,
  //!< An SSA/ASS document.
  kSsa = 4,
  //!< A 3GPP timed text sample of an MP4 container.
  kTx3g = 5,
  //!< A TTML/IMSC document.
  kTtml = 6,
  //!< A PGS bitmap display set.
  kPgs = 7,
  //!< A VobSub SPU packet.
  kVobSub = 8,
  //!< A DVB subtitle segment stream.
  kDvbSub = 9,
  //!< A CEA-608 caption byte stream.
  kCea608 = 10,
  //!< A CEA-708 (DTVCC) cc_data stream.
  kCea708 = 11,
};

ParserKind parserKindForFormat(const MediaFormat& format) {
  const std::string& mimeType = format.getMimeType();
  if (mimeType == MimeTypes::TEXT_VTT) {
    return ParserKind::kWebvtt;
  }
  if (mimeType == MimeTypes::APPLICATION_MP4VTT) {
    return ParserKind::kMp4Webvtt;
  }
  if (mimeType == MimeTypes::APPLICATION_SUBRIP) {
    return ParserKind::kSubrip;
  }
  if (mimeType == MimeTypes::TEXT_SSA) {
    return ParserKind::kSsa;
  }
  if (mimeType == MimeTypes::APPLICATION_TX3G) {
    return ParserKind::kTx3g;
  }
  if (mimeType == MimeTypes::APPLICATION_TTML) {
    return ParserKind::kTtml;
  }
  if (mimeType == MimeTypes::APPLICATION_PGS) {
    return ParserKind::kPgs;
  }
  if (mimeType == MimeTypes::APPLICATION_VOBSUB) {
    return ParserKind::kVobSub;
  }
  if (mimeType == MimeTypes::APPLICATION_DVBSUBS) {
    return ParserKind::kDvbSub;
  }
  if (mimeType == MimeTypes::APPLICATION_CEA608 ||
      mimeType == MimeTypes::APPLICATION_MP4CEA608) {
    return ParserKind::kCea608;
  }
  if (mimeType == MimeTypes::APPLICATION_CEA708) {
    return ParserKind::kCea708;
  }
  return ParserKind::kNone;
}

} // namespace

CueReplacementBehavior cueReplacementBehaviorForMimeType(const std::string& mimeType) {
  // The formats whose samples carry one complete cue set each replace the cues that are shown; the
  // ones whose samples carry a cue (or a piece of one) merge. The values are the ones of ExoPlayer's
  // parser implementations.
  if (mimeType == MimeTypes::APPLICATION_MP4VTT || mimeType == MimeTypes::APPLICATION_TX3G ||
      mimeType == MimeTypes::APPLICATION_PGS || mimeType == MimeTypes::APPLICATION_VOBSUB ||
      mimeType == MimeTypes::APPLICATION_DVBSUBS || mimeType == MimeTypes::APPLICATION_RAWCC ||
      mimeType == MimeTypes::APPLICATION_CEA708) {
    return CueReplacementBehavior::kReplace;
  }
  // WebVTT, SubRip, SSA and TTML: a cue states the time it ends at, so the cues of a sample merge
  // with the ones that are still shown. The same answer is the default of ExoPlayer for a format
  // without a parser of its own.
  return CueReplacementBehavior::kMerge;
}

bool AVDefaultSubtitleParserFactory::supportsFormat(const MediaFormat& format) const {
  return parserKindForFormat(format) != ParserKind::kNone;
}

CueReplacementBehavior
AVDefaultSubtitleParserFactory::getCueReplacementBehavior(const MediaFormat& format) const {
  return cueReplacementBehaviorForMimeType(format.getMimeType());
}

std::unique_ptr<AVSubtitleParser>
AVDefaultSubtitleParserFactory::create(const MediaFormat& format) const {
  switch (parserKindForFormat(format)) {
    case ParserKind::kWebvtt:
      return std::unique_ptr<AVSubtitleParser>(new AVWebvttParser());
    case ParserKind::kMp4Webvtt:
      return std::unique_ptr<AVSubtitleParser>(new AVMp4WebvttParser());
    case ParserKind::kSubrip:
      return std::unique_ptr<AVSubtitleParser>(new AVSubripParser());
    case ParserKind::kSsa:
      // A track of a container states its header once, in the codec specific data; a standalone
      // document states it in every document.
      return std::unique_ptr<AVSubtitleParser>(new AVSsaParser(format.csdList()));
    case ParserKind::kTx3g:
      // The default style of the track belongs to its sample description, which the extractor
      // hands over as the codec specific data of the format.
      return std::unique_ptr<AVSubtitleParser>(new AVTx3gParser(format.csdList()));
    case ParserKind::kTtml:
      return std::unique_ptr<AVSubtitleParser>(new AVTtmlParser());
    case ParserKind::kPgs:
      return std::unique_ptr<AVSubtitleParser>(new AVPgsParser());
    case ParserKind::kVobSub:
      return std::unique_ptr<AVSubtitleParser>(new AVVobSubParser(format.csdList()));
    case ParserKind::kDvbSub:
      return std::unique_ptr<AVSubtitleParser>(new AVDvbSubParser());
    case ParserKind::kCea608:
      return std::unique_ptr<AVSubtitleParser>(new AVCea608Parser());
    case ParserKind::kCea708: {
      const int serviceNumber = format.getCaptionServiceNumber();
      return std::unique_ptr<AVSubtitleParser>(
        new AVCea708Parser(serviceNumber > 0 ? serviceNumber : 1));
    }
    case ParserKind::kNone:
    default:
      return nullptr;
  }
}

std::unique_ptr<AVSubtitleParserFactory> createDefaultSubtitleParserFactory() {
  return std::unique_ptr<AVSubtitleParserFactory>(new AVDefaultSubtitleParserFactory());
}

} // namespace coral
