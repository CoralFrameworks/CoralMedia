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

#include <extractor/DefaultExtractorsFactory.h>

#include <extractor/audio/AdtsExtractor.h>
#include <extractor/audio/AmrExtractor.h>
#include <extractor/audio/FlacExtractor.h>
#include <extractor/audio/Mp3Extractor.h>
#include <extractor/audio/WavExtractor.h>
#include <extractor/container/AviExtractor.h>
#include <extractor/container/FlvExtractor.h>
#include <extractor/container/MatroskaExtractor.h>
#include <extractor/container/Mp4Extractor.h>
#include <extractor/container/OggExtractor.h>
#include <extractor/container/TsExtractor.h>
#include <extractor/image/AvifExtractor.h>
#include <extractor/image/BmpExtractor.h>
#include <extractor/image/HeifExtractor.h>
#include <extractor/image/JpegExtractor.h>
#include <extractor/image/PngExtractor.h>
#include <extractor/image/WebpExtractor.h>
#include <extractor/text/SubtitleExtractor.h>

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

namespace coral {

namespace {

/**
 * The factory of one extractor, so that the creation can be ordered.
 */
using ExtractorCreator = std::unique_ptr<AVExtractor> (*)();

std::unique_ptr<AVExtractor> createMp4() {
  return std::make_unique<AVMp4Extractor>();
}

std::unique_ptr<AVExtractor> createMatroska() {
  return std::make_unique<AVMatroskaExtractor>();
}

std::unique_ptr<AVExtractor> createOgg() {
  return std::make_unique<AVOggExtractor>();
}

std::unique_ptr<AVExtractor> createAvi() {
  return std::make_unique<AVAviExtractor>();
}

std::unique_ptr<AVExtractor> createMp3() {
  return std::make_unique<AVMp3Extractor>();
}

std::unique_ptr<AVExtractor> createAdts() {
  return std::make_unique<AVAdtsExtractor>();
}

std::unique_ptr<AVExtractor> createWav() {
  return std::make_unique<AVWavExtractor>();
}

std::unique_ptr<AVExtractor> createFlac() {
  return std::make_unique<AVFlacExtractor>();
}

std::unique_ptr<AVExtractor> createAmr() {
  return std::make_unique<AVAmrExtractor>();
}

std::unique_ptr<AVExtractor> createFlv() {
  return std::make_unique<AVFlvExtractor>();
}

std::unique_ptr<AVExtractor> createTs() {
  return std::make_unique<AVTsExtractor>();
}

std::unique_ptr<AVExtractor> createAvif() {
  return std::make_unique<AVAvifExtractor>();
}

std::unique_ptr<AVExtractor> createHeif() {
  return std::make_unique<AVHeifExtractor>();
}

std::unique_ptr<AVExtractor> createBmp() {
  return std::make_unique<AVBmpExtractor>();
}

std::unique_ptr<AVExtractor> createPng() {
  return std::make_unique<AVPngExtractor>();
}

std::unique_ptr<AVExtractor> createWebp() {
  return std::make_unique<AVWebpExtractor>();
}

std::unique_ptr<AVExtractor> createJpeg() {
  return std::make_unique<AVJpegExtractor>();
}

std::unique_ptr<AVExtractor> createSubtitle() {
  return std::make_unique<AVSubtitleExtractor>();
}

/**
 * Returns the creator of the extractor that handles the given file type, or null.
 */
ExtractorCreator creatorForFileType(const int fileType) {
  switch (fileType) {
    case FileTypes::MP4: {
      return &createMp4;
    }
    case FileTypes::MATROSKA: {
      return &createMatroska;
    }
    case FileTypes::OGG: {
      return &createOgg;
    }
    case FileTypes::AVI: {
      return &createAvi;
    }
    case FileTypes::MP3: {
      return &createMp3;
    }
    case FileTypes::ADTS: {
      return &createAdts;
    }
    case FileTypes::WAV: {
      return &createWav;
    }
    case FileTypes::FLAC: {
      return &createFlac;
    }
    case FileTypes::AMR: {
      return &createAmr;
    }
    case FileTypes::FLV: {
      return &createFlv;
    }
    case FileTypes::TS: {
      return &createTs;
    }
    case FileTypes::AVIF: {
      return &createAvif;
    }
    case FileTypes::HEIF: {
      return &createHeif;
    }
    case FileTypes::BMP: {
      return &createBmp;
    }
    case FileTypes::PNG: {
      return &createPng;
    }
    case FileTypes::WEBP: {
      return &createWebp;
    }
    case FileTypes::JPEG: {
      return &createJpeg;
    }
    case FileTypes::WEBVTT:
    case FileTypes::SRT:
    case FileTypes::SSA: {
      return &createSubtitle;
    }
    default: {
      return nullptr;
    }
  }
}

} // namespace anonymous

AVDefaultExtractorsFactory::AVDefaultExtractorsFactory(const int fileType, const long length) noexcept
  : fileType_(fileType),
    length_(length) {
}

std::vector<std::unique_ptr<AVExtractor>> AVDefaultExtractorsFactory::createExtractors() {
  // The length is kept so that an extractor may specialize its parse when it is known, and so that
  // the signature matches the ExoPlayer factory.
  static_cast<void>(length_);

  // The order matters: an MP4 file starts with a box that may be preceded by an ID3 tag, a
  // Matroska file starts with the EBML header, an Ogg file starts with the capture pattern of its
  // first page, and both an MP3 stream and an ADTS stream start with an ID3 tag, in which case the
  // first valid frame header decides. The audio only containers are tried last, because a stream
  // that one of them recognizes by accident is a container that a signature based sniffing
  // recognizes with certainty.
  constexpr ExtractorCreator defaultOrder[] = {
    // The image containers whose files are ISO base media boxes are tried before MP4, because the
    // signature of their file type box is also one that MP4 accepts.
    &createAvif,
    &createHeif,
    &createPng,
    &createBmp,
    &createWebp,
    &createJpeg,
    &createFlv,
    &createTs,
    &createMp4,
    &createMatroska,
    &createOgg,
    &createAvi,
    &createAmr,
    &createMp3,
    &createAdts,
    &createWav,
    &createFlac,
    // A standalone subtitle document is recognized by its text header, so it is tried last: it can
    // only win when no container recognized the input at all.
    &createSubtitle
  };

  std::vector<ExtractorCreator> creators;
  const ExtractorCreator preferred = creatorForFileType(fileType_);
  // The file type hint of an ISO base media file does not tell an AVIF or HEIF still image apart
  // from a movie: both start with a file type box, and the MP4 extractor accepts either. The image
  // extractors are tried before it so that the more specific signature wins even when the caller
  // claimed that the media is an MP4.
  if (preferred != nullptr && preferred != &createMp4) {
    creators.push_back(preferred);
  }
  for (const auto creator: defaultOrder) {
    if (creator != preferred || preferred == &createMp4) {
      creators.push_back(creator);
    }
  }

  std::vector<std::unique_ptr<AVExtractor>> extractors;
  extractors.reserve(creators.size());
  for (const auto creator: creators) {
    if (auto extractor = creator(); extractor != nullptr) {
      extractors.push_back(std::move(extractor));
    }
  }
  return extractors;
}

} // namespace coral
