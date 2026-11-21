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

#ifndef CORAL_EXTRACTOR_CONTAINER_OGG_EXTRACTOR_H
#define CORAL_EXTRACTOR_CONTAINER_OGG_EXTRACTOR_H

#include <extractor/Extractor.h>
#include <extractor/ExtractorInput.h>

#include <memory>
#include <string>

namespace coral {

/**
 * The reader of one Ogg bitstream, i.e. of one codec carried by an Ogg container.
 *
 * <p>The class owns the parser and the seek map of the codec, and it is created by
 * {@link AVOggExtractor::sniff} once the identification header of the bitstream was recognized. It
 * is incomplete on purpose: the implementation lives in the source file of the extractor, so that
 * the codec specific parsers are not part of the public interface of the module.
 */
class AVOggStreamReader;

/**
 * Extracts the tracks, the samples and the seek map of an Ogg container.
 *
 * <p>The extractor reads the first page of the stream to identify the codec it carries, and then
 * delegates the parse to the {@link AVOggStreamReader} of that codec: the identification, the
 * comment and the setup headers are turned into the {@link MediaFormat} of the single audio track
 * of the container, and every packet that follows becomes one {@link AVSample}. The supported
 * codecs are:
 * <ul>
 *   <li>Vorbis, whose identification and setup headers are forwarded as the codec specific data of
 *       the track, as the decoder expects them;</li>
 *   <li>Opus, whose identification header, pre-skip and seek pre-roll are forwarded as the codec
 *       specific data of the track;</li>
 *   <li>FLAC, whose Ogg mapping header carries the STREAMINFO block that the track description
 *       forwards.</li>
 * </ul>
 * A container that carries any other codec, e.g. Theora video or a Speex stream, is not recognized
 * by {@link #sniff}, so the reader asks the other extractors of the factory.
 *
 * <p>An Ogg page carries the granule position of the last packet that completes in it, which is
 * what makes the timestamps of the packets exact: the extractor counts the samples of every packet
 * while it reads them, so that the granule position of a page only has to be read to check that
 * count. The page boundaries that the extraction visits are also collected into the seek map, so
 * that a seek resolves to the page that precedes the requested time; the first pages of a stream
 * are the only ones whose duration is unknown, because an Ogg container does not declare a
 * duration: the total number of granules is only known once the page that ends the stream was read,
 * or once the end of the input was reached.
 *
 * <p>An Ogg container holds a single logical bitstream per track. Chained streams, i.e. streams that
 * concatenate several logical bitstreams, and multiplexed ones, i.e. streams that interleave several
 * logical bitstreams in the same pages, are not supported: the extractor reads the pages of the
 * first stream it recognized and treats the others as if they were part of it.
 *
 * <p>This is the extractor module counterpart of ExoPlayer's {@code OggExtractor}, together with the
 * {@code OggPageHeader}, {@code OggPacket}, {@code StreamReader}, {@code VorbisReader},
 * {@code OpusReader}, {@code FlacReader} and {@code DefaultOggSeeker} helpers that the Java extractor
 * delegates to, and of the {@code VorbisUtil} and {@code OpusUtil} parsers of the container module.
 */
class AVOggExtractor final : public AVExtractor {
public:
  AVOggExtractor() noexcept;

  ~AVOggExtractor() override;

  AVOggExtractor(const AVOggExtractor& other) = delete;

  AVOggExtractor& operator=(const AVOggExtractor& other) = delete;

  /**
   * Returns {@code kSniffResultCertain} when the input starts with the page that begins an Ogg
   * bitstream of a codec that this extractor understands, and {@code kSniffResultNone} otherwise.
   * The peek position of the input is restored before returning.
   *
   * <p>The counterpart of {@code OggExtractor.sniff} and of its {@code sniffInternal} helper. The
   * stream reader of the recognized codec is created here, exactly like the Java implementation
   * creates it while sniffing, so that the pages that were peeked while sniffing are not parsed
   * again by {@link #read}.
   */
  int sniff(AVExtractorInput& input) override;

  /**
   * Stores the output that receives the track, the format, the samples and the seek map. The
   * counterpart of {@code OggExtractor.init}.
   */
  void init(AVExtractorOutput& output) override;

  /**
   * Reads the pages of the container and delivers the packets they carry. The counterpart of
   * {@code OggExtractor.read}.
   */
  Result read(AVExtractorInput& input) override;

  /**
   * Moves the parse to the position of the seek, which the reader resolved with the seek map of the
   * container. The counterpart of {@code OggExtractor.seek}.
   */
  bool seek(long position, int64_t timeUs) override;

  /**
   * Returns the position last observed on the input.
   */
  [[nodiscard]] long getPosition() const override { return position_; }

  /**
   * Returns the name of the container, for diagnostics.
   */
  [[nodiscard]] std::string getName() const override { return "Ogg"; }

  /**
   * Returns why {@link #sniff} rejected the input, for diagnostics.
   */
  [[nodiscard]] std::string getSniffFailureReason() const override {
    return sniffFailureReason_;
  }

private:
  /**
   * Recognizes the codec of the first page of the input and creates its stream reader.
   *
   * @return {@code true} when the bitstream is one of the codecs of the extractor, in which case
   *         {@link streamReader_} is set.
   */
  bool sniffInternal(AVExtractorInput& input);

  /**
   * The output that receives the track, the format, the samples and the seek map. It is owned by the
   * reader, which outlives the extractor, therefore the extractor only observes it.
   */
  AVExtractorOutput* output_ = nullptr;
  /**
   * The reader of the bitstream, created by the sniffing and owned by the extractor, or null when
   * the extractor was asked to read an input it does not recognize.
   */
  std::unique_ptr<AVOggStreamReader> streamReader_;
  /** Whether the track of the container was created, i.e. whether the headers were reached. */
  bool streamReaderInitialized_ = false;
  /** The position last observed on the input. */
  long position_ = 0;
  /** The reason why {@link #sniff} rejected the input. */
  std::string sniffFailureReason_;
};

} // namespace coral

#endif //CORAL_EXTRACTOR_CONTAINER_OGG_EXTRACTOR_H
