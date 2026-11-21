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

#include <extractor/text/SubtitleExtractor.h>

#include <extractor/text/SubtitleParser.h>

#include <media/Definitions.h>
#include <media/Format.h>
#include <media/Types.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace coral {
namespace {

/** The number of bytes that the detection of the document looks at. */
constexpr size_t kDetectionLimit = AVExtractor::kSniffPeekLimit;

/** The largest document that is read as one sample. */
constexpr size_t kMaxDocumentSize = 64 * 1024 * 1024;

/** Returns the offset of the first line of @p data, i.e. after a UTF-8 byte order mark. */
size_t textOffset(const uint8_t* data, const size_t size) {
  if (size >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
    return 3;
  }
  return 0;
}

/** Returns whether @p data starts a WebVTT document. */
bool looksLikeWebvtt(const uint8_t* data, const size_t size) {
  const size_t offset = textOffset(data, size);
  if (size < offset + 6) {
    return false;
  }
  if (std::memcmp(data + offset, "WEBVTT", 6) != 0) {
    return false;
  }
  // The header is the word, followed by the end of the line or by its description.
  if (size == offset + 6) {
    return true;
  }
  const uint8_t next = data[offset + 6];
  return next == '\n' || next == '\r' || next == ' ' || next == '\t';
}

/** Returns whether @p data starts a SubRip document, i.e. holds a timing line. */
bool looksLikeSubrip(const uint8_t* data, const size_t size) {
  size_t position = textOffset(data, size);
  size_t lines = 0;
  while (position < size && lines < 256) {
    size_t end = position;
    while (end < size && data[end] != '\n') {
      ++end;
    }
    const std::string_view line(reinterpret_cast<const char*>(data) + position, end - position);
    if (line.find("-->") != std::string_view::npos) {
      // A SubRip timing line states its times with a comma, a WebVTT one with a dot.
      return line.find(':') != std::string_view::npos && line.find(',') != std::string_view::npos;
    }
    position = end + 1;
    ++lines;
  }
  return false;
}

/** Returns whether @p data looks like a TTML document. */
bool looksLikeTtml(const uint8_t* data, const size_t size) {
  const size_t offset = textOffset(data, size);
  std::string_view text(reinterpret_cast<const char*>(data) + offset, size - offset);
  const size_t limit = std::min<size_t>(text.size(), 1024);
  text = text.substr(0, limit);
  if (text.rfind("<?xml", 0) == 0 || text.rfind("<tt", 0) == 0) return true;
  const size_t tt = text.find("<tt");
  if (tt == std::string_view::npos) return false;
  const size_t xml = text.find("<?xml");
  return xml == std::string_view::npos || xml < tt;
}
/** Returns whether @p data starts an SSA/ASS document. */
bool looksLikeSsa(const uint8_t* data, const size_t size) {
  size_t position = textOffset(data, size);
  size_t lines = 0;
  while (position < size && lines < 256) {
    size_t end = position;
    while (end < size && data[end] != '\n') {
      ++end;
    }
    std::string_view line(reinterpret_cast<const char*>(data) + position, end - position);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    if (line == "[Script Info]" || line == "[V4+ Styles]" || line == "[V4 Styles]" ||
        line == "[Events]") {
      return true;
    }
    if (line.rfind("Dialogue:", 0) == 0) {
      return true;
    }
    position = end + 1;
    ++lines;
  }
  return false;
}

/** Reads the whole input into @p data, up to the size limit of a document. */
bool readDocument(AVExtractorInput& input, std::vector<uint8_t>& data) {
  const long length = input.getLength();
  if (length > 0) {
    if (static_cast<size_t>(length) > kMaxDocumentSize) {
      return false;
    }
    data.resize(static_cast<size_t>(length));
    long total = 0;
    while (total < length) {
      const long read = input.read(data.data(), static_cast<size_t>(total),
                                   static_cast<size_t>(length - total));
      if (read == kEndOfInput || read <= 0) {
        break;
      }
      total += read;
    }
    data.resize(static_cast<size_t>(total));
    return total > 0;
  }
  uint8_t chunk[16 * 1024];
  while (true) {
    const long read = input.read(chunk, 0, sizeof(chunk));
    if (read == kEndOfInput || read <= 0) {
      break;
    }
    data.insert(data.end(), chunk, chunk + read);
    if (data.size() > kMaxDocumentSize) {
      return false;
    }
  }
  return !data.empty();
}

} // namespace

std::string AVSubtitleExtractor::mimeTypeForDocument(const uint8_t* data, const size_t size) {
  if (data == nullptr || size == 0) {
    return {};
  }
  if (looksLikeWebvtt(data, size)) {
    return MimeTypes::TEXT_VTT;
  }
  if (looksLikeSsa(data, size)) {
    return MimeTypes::TEXT_SSA;
  }
  if (looksLikeTtml(data, size)) {
    return MimeTypes::APPLICATION_TTML;
  }
  if (looksLikeSubrip(data, size)) {
    return MimeTypes::APPLICATION_SUBRIP;
  }
  return {};
}

int AVSubtitleExtractor::sniff(AVExtractorInput& input) {
  input.resetPeekPosition();
  std::vector<uint8_t> buffer(kDetectionLimit);
  const long peeked = input.peek(buffer.data(), 0, buffer.size());
  input.resetPeekPosition();
  if (peeked <= 0) {
    sniffFailureReason_ = "the input is empty";
    return AVExtractorsFactory::kSniffResultNone;
  }
  buffer.resize(static_cast<size_t>(peeked));
  if (looksLikeWebvtt(buffer.data(), buffer.size())) {
    return AVExtractorsFactory::kSniffResultCertain;
  }
  if (looksLikeSsa(buffer.data(), buffer.size()) || looksLikeSubrip(buffer.data(), buffer.size()) ||
      looksLikeTtml(buffer.data(), buffer.size())) {
    // The signatures of SubRip and SSA are text patterns rather than magic bytes, so the extractor
    // only claims to be able to try them: a container that recognized the input wins.
    return AVExtractorsFactory::kSniffResultPossible;
  }
  sniffFailureReason_ = "the input is not a subtitle document";
  return AVExtractorsFactory::kSniffResultNone;
}

void AVSubtitleExtractor::init(AVExtractorOutput& output) {
  output_ = &output;
  trackOutput_ = output.track(0, MediaType::kMediaTypeSubtitle, AVTrackOutput::emptyConsumer());
  delivered_ = false;
}

AVSubtitleExtractor::Result AVSubtitleExtractor::read(AVExtractorInput& input) {
  if (trackOutput_ == nullptr || delivered_) {
    return kResultEndOfInput;
  }
  std::vector<uint8_t> data;
  if (!readDocument(input, data)) {
    sniffFailureReason_ = "the document could not be read";
    trackOutput_->endTrack();
    delivered_ = true;
    return kResultEndOfInput;
  }
  const std::string mimeType = mimeTypeForDocument(data.data(), data.size());
  if (mimeType.empty() || data.size() > std::numeric_limits<uint32_t>::max()) {
    sniffFailureReason_ = "the document is not a supported subtitle format";
    trackOutput_->endTrack();
    delivered_ = true;
    return kResultEndOfInput;
  }

  auto format = MediaFormat::CreateSubtitleFormat(mimeType);
  // The cues of the document state their own times, so they are placed at those times rather than
  // at the time of the sample, which is zero.
  format->setSubsampleOffsetUs(MediaFormat::OFFSET_SAMPLE_RELATIVE);
  format->setCueReplacementBehavior(cueReplacementBehaviorForMimeType(mimeType));
  format->setSelectionFlags(kMediaSelectionDefault);
  format->setRoleFlags(kRoleFlagSubtitle);
  trackOutput_->format(format);

  auto buffer = std::make_shared<AVBlockBuffer>(data.data(), static_cast<uint32_t>(data.size()));
  AVSample sample(0, 0,
                  static_cast<uint32_t>(AVSample::kFlagKeyFrame | AVSample::kFlagFirstSample |
                                        AVSample::kFlagLastSample),
                  std::move(buffer));
  trackOutput_->sample(sample);
  trackOutput_->endTrack();

  if (output_ != nullptr) {
    // A document has no index of its own: the player seeks within it, and the sample is re-read
    // from the start of the file.
    output_->seekMap(std::make_shared<AVUnseekableSeekMap>(0));
    output_->endTracks();
  }
  delivered_ = true;
  return kResultEndOfInput;
}

bool AVSubtitleExtractor::seek(long position, int64_t timeUs) {
  (void)position;
  // kTimeUnset is the "no position" marker the player passes before it knows the start position.
  // Resetting here would make the period read the one-document subtitle file a second time and
  // append a duplicate sample, which the subtitle renderer then parses twice.
  if (timeUs >= 0) {
    delivered_ = false;
  }
  return true;
}

} // namespace coral
