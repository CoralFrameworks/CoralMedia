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

#include <extractor/image/AvifExtractor.h>
#include <extractor/image/BmpExtractor.h>
#include <extractor/image/HeifExtractor.h>
#include <extractor/image/JpegExtractor.h>
#include <extractor/image/PngExtractor.h>
#include <extractor/image/WebpExtractor.h>

#include <extractor/Utility.h>

namespace coral {

namespace {

/** The identifier of a RIFF file, i.e. the first field of a WebP file. */
constexpr uint32_t kRiff = 0x52494646; // "RIFF"

/** The form type of a WebP file, i.e. the field that follows the size of a RIFF file. */
constexpr uint32_t kWebp = 0x57454250; // "WEBP"

/** The identifier of the ISO base media file type box. */
constexpr uint32_t kFtyp = 0x66747970; // "ftyp"

/** The major brand of an AVIF file. */
constexpr uint32_t kAvif = 0x61766966; // "avif"

/** The major brand of a HEIF/HEIC file. */
constexpr uint32_t kHeic = 0x68656963; // "heic"

/** The number of bytes of the header of an ISO base media box. */
constexpr size_t kBoxHeaderSize = 8;

} // namespace anonymous

// ==================================================================================================
// BMP
// ==================================================================================================

AVBmpExtractor::AVBmpExtractor()
  : imageExtractor_(0x424D, 2, MimeTypes::IMAGE_BMP) {
}

int AVBmpExtractor::sniff(AVExtractorInput& input) { return imageExtractor_.sniff(input); }

void AVBmpExtractor::init(AVExtractorOutput& output) { imageExtractor_.init(output); }

AVExtractor::Result AVBmpExtractor::read(AVExtractorInput& input) { return imageExtractor_.read(input); }

bool AVBmpExtractor::seek(const long position, const int64_t timeUs) { return imageExtractor_.seek(position, timeUs); }

// ==================================================================================================
// PNG
// ==================================================================================================

AVPngExtractor::AVPngExtractor()
  : imageExtractor_(0x8950, 2, MimeTypes::IMAGE_PNG) {
}

int AVPngExtractor::sniff(AVExtractorInput& input) { return imageExtractor_.sniff(input); }

void AVPngExtractor::init(AVExtractorOutput& output) { imageExtractor_.init(output); }

AVExtractor::Result AVPngExtractor::read(AVExtractorInput& input) { return imageExtractor_.read(input); }

bool AVPngExtractor::seek(const long position, const int64_t timeUs) { return imageExtractor_.seek(position, timeUs); }

// ==================================================================================================
// JPEG
// ==================================================================================================

AVJpegExtractor::AVJpegExtractor()
  : imageExtractor_(0xFFD8, 2, MimeTypes::IMAGE_JPEG) {
}

int AVJpegExtractor::sniff(AVExtractorInput& input) { return imageExtractor_.sniff(input); }

void AVJpegExtractor::init(AVExtractorOutput& output) { imageExtractor_.init(output); }

AVExtractor::Result AVJpegExtractor::read(AVExtractorInput& input) { return imageExtractor_.read(input); }

bool AVJpegExtractor::seek(const long position, const int64_t timeUs) { return imageExtractor_.seek(position, timeUs); }

// ==================================================================================================
// WebP
// ==================================================================================================

AVWebpExtractor::AVWebpExtractor()
  : imageExtractor_(-1, -1, MimeTypes::IMAGE_WEBP) {
}

int AVWebpExtractor::sniff(AVExtractorInput& input) {
  input.resetPeekPosition();
  uint8_t header[12];
  const long peeked = input.peek(header, 0, sizeof(header));
  input.resetPeekPosition();
  if (peeked < static_cast<long>(sizeof(header))) {
    return AVExtractorsFactory::kSniffResultNone;
  }
  if (AVUtil::readUnsignedInt(header, 0) != kRiff || AVUtil::readUnsignedInt(header, 8) != kWebp) {
    return AVExtractorsFactory::kSniffResultNone;
  }
  return AVExtractorsFactory::kSniffResultCertain;
}

void AVWebpExtractor::init(AVExtractorOutput& output) { imageExtractor_.init(output); }

AVExtractor::Result AVWebpExtractor::read(AVExtractorInput& input) { return imageExtractor_.read(input); }

bool AVWebpExtractor::seek(const long position, const int64_t timeUs) { return imageExtractor_.seek(position, timeUs); }

// ==================================================================================================
// AVIF
// ==================================================================================================

AVAvifExtractor::AVAvifExtractor()
  : imageExtractor_(-1, -1, MimeTypes::IMAGE_AVIF) {
}

int AVAvifExtractor::sniff(AVExtractorInput& input) {
  input.resetPeekPosition();
  uint8_t header[12];
  const long peeked = input.peek(header, 0, sizeof(header));
  input.resetPeekPosition();
  if (peeked < static_cast<long>(sizeof(header))) {
    return AVExtractorsFactory::kSniffResultNone;
  }
  // The file starts with an ftyp box whose major brand is "avif".
  if (AVUtil::readUnsignedInt(header, 4) != kFtyp || AVUtil::readUnsignedInt(header, 8) != kAvif) {
    return AVExtractorsFactory::kSniffResultNone;
  }
  return AVExtractorsFactory::kSniffResultCertain;
}

void AVAvifExtractor::init(AVExtractorOutput& output) { imageExtractor_.init(output); }

AVExtractor::Result AVAvifExtractor::read(AVExtractorInput& input) { return imageExtractor_.read(input); }

bool AVAvifExtractor::seek(const long position, const int64_t timeUs) { return imageExtractor_.seek(position, timeUs); }

// ==================================================================================================
// HEIF
// ==================================================================================================

AVHeifExtractor::AVHeifExtractor()
  : imageExtractor_(-1, -1, MimeTypes::IMAGE_HEIF) {
}

int AVHeifExtractor::sniff(AVExtractorInput& input) {
  input.resetPeekPosition();
  uint8_t header[24];
  const long peeked = input.peek(header, 0, sizeof(header));
  input.resetPeekPosition();
  if (peeked < 12) {
    return AVExtractorsFactory::kSniffResultNone;
  }
  // The first box must be the file type box, and it must carry a major brand and a minor version.
  if (AVUtil::readUnsignedInt(header, 4) != kFtyp) {
    return AVExtractorsFactory::kSniffResultNone;
  }
  const uint32_t boxSize = AVUtil::readUnsignedInt(header, 0);
  size_t headerSize = kBoxHeaderSize;
  uint64_t atomSize = boxSize;
  if (boxSize == 1) {
    // A size of one means that the real size follows the type as a 64 bit value.
    if (peeked < 24) {
      return AVExtractorsFactory::kSniffResultNone;
    }
    atomSize = (static_cast<uint64_t>(AVUtil::readUnsignedInt(header, 8)) << 32) | AVUtil::readUnsignedInt(header, 12);
    headerSize = 16;
  }
  if (atomSize < headerSize + 8) {
    return AVExtractorsFactory::kSniffResultNone;
  }
  if (peeked < static_cast<long>(headerSize) + 4) {
    return AVExtractorsFactory::kSniffResultNone;
  }
  if (AVUtil::readUnsignedInt(header, static_cast<int>(headerSize)) != kHeic) {
    return AVExtractorsFactory::kSniffResultNone;
  }
  return AVExtractorsFactory::kSniffResultCertain;
}

void AVHeifExtractor::init(AVExtractorOutput& output) { imageExtractor_.init(output); }

AVExtractor::Result AVHeifExtractor::read(AVExtractorInput& input) { return imageExtractor_.read(input); }

bool AVHeifExtractor::seek(const long position, const int64_t timeUs) { return imageExtractor_.seek(position, timeUs); }

} // namespace coral
