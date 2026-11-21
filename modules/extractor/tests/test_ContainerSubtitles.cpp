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

#include "TestMedia.h"

#include <datasource/DataSource.h>
#include <extractor/DefaultExtractorsFactory.h>
#include <extractor/Reader.h>
#include <extractor/text/CueTimeline.h>
#include <extractor/text/DefaultSubtitleParserFactory.h>
#include <extractor/text/SubtitleParser.h>

#include <media/Definitions.h>
#include <media/Format.h>
#include <media/Types.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using coral::AVByteArrayDataSource;
using coral::AVCueTimeline;
using coral::AVDefaultExtractorsFactory;
using coral::AVExtractorsFactory;
using coral::AVFileDataSource;
using coral::AVReader;
using coral::AVSample;
using coral::AVSubtitleParser;
using coral::CueReplacementBehavior;
using coral::Definitions;
using coral::FileTypes;
using coral::MediaFormat;
using coral::MediaType;
using coral::MimeTypes;

namespace {

/** One sample that a track delivered, kept after the buffer of the reader is released. */
struct DeliveredSample {
  int trackIndex = -1;
  int64_t timeUs = 0;
  uint32_t flags = 0;
  std::vector<uint8_t> payload;
};

/** A reader over an in memory or file based media, with every track consumed. */
class ReaderHarness final {
public:
  explicit ReaderHarness(std::vector<uint8_t> media, const int fileType = -1) {
    auto source = std::make_shared<AVByteArrayDataSource>(std::move(media));
    source->open("memory");
    create(std::move(source), fileType);
  }

  explicit ReaderHarness(const std::string& path, const int fileType = -1) {
    auto source = std::make_shared<AVFileDataSource>();
    EXPECT_TRUE(source->open(path));
    create(std::move(source), fileType);
  }

  ReaderHarness(const ReaderHarness& other) = delete;

  ReaderHarness& operator=(const ReaderHarness& other) = delete;

  bool prepare() { return reader_->prepare(); }

  /**
   * Registers the consumers of the tracks before the reader creates them: the identifiers of the
   * tracks of a container are only known once its header was read, and a consumer that is
   * registered for an identifier that does not exist yet is used by the track when it is created.
   */
  void consumeEveryTrack() {
    for (int id = 0; id < 8; ++id) {
      registerConsumer(id);
      // Caption tracks derived from a video track use 0x10000 | track index.
      registerConsumer(0x10000 | id);
    }
  }

  void readAll() {
    while (reader_->read() == AVReader::kResultContinue) {
    }
  }

  [[nodiscard]] AVReader& reader() const { return *reader_; }

  [[nodiscard]] const std::vector<DeliveredSample>& samples(const int trackId) const {
    static const std::vector<DeliveredSample> sEmpty;
    const auto found = samples_.find(trackId);
    return found == samples_.end() ? sEmpty : found->second;
  }

  [[nodiscard]] int trackIdOfType(const MediaType type) const {
    for (const int id : reader_->getTrackIds()) {
      const auto track = reader_->getTrack(id);
      if (track != nullptr && track->getType() == type) {
        return id;
      }
    }
    return Definitions::kIndexUnset;
  }

private:
  void registerConsumer(const int id) {
    reader_->setTrackConsumer(id, [this, id](const AVSample& sample) {
      if (sample.isEndOfStream() || sample.data == nullptr) {
        return;
      }
      DeliveredSample delivered;
      delivered.trackIndex = id;
      delivered.timeUs = sample.timeUs;
      delivered.flags = sample.flags;
      delivered.payload.assign(sample.array(), sample.array() + sample.size());
      samples_[id].push_back(std::move(delivered));
    });
  }

  void create(std::shared_ptr<coral::AVDataSource> source, const int fileType) {
    reader_ = std::unique_ptr<AVReader>(
      new AVReader(std::move(source),
                   std::unique_ptr<AVExtractorsFactory>(new AVDefaultExtractorsFactory(fileType))));
  }

  std::unique_ptr<AVReader> reader_;
  std::map<int, std::vector<DeliveredSample>> samples_;
};

std::string assetPath(const std::string& relative) {
#ifdef CORAL_ASSETS_DIR
  return std::string(CORAL_ASSETS_DIR) + "/" + relative;
#else
  (void)relative;
  return std::string();
#endif
}

/** Builds the payload of a `tx3g` sample: a length prefixed text. */
std::vector<uint8_t> tx3gSample(const std::string& text) {
  std::vector<uint8_t> sample;
  coral::test::putUnsignedShort(sample, static_cast<uint16_t>(text.size()));
  sample.insert(sample.end(), text.begin(), text.end());
  return sample;
}

/** Builds the payload of an MP4 WebVTT sample: a `vttc` box holding a `payl` box. */
std::vector<uint8_t> vttcSample(const std::string& text) {
  std::vector<uint8_t> payload;
  coral::test::putUnsignedInt(payload, static_cast<uint32_t>(8 + text.size()));
  coral::test::putFourCc(payload, "payl");
  payload.insert(payload.end(), text.begin(), text.end());
  std::vector<uint8_t> sample;
  coral::test::putUnsignedInt(sample, static_cast<uint32_t>(8 + payload.size()));
  coral::test::putFourCc(sample, "vttc");
  sample.insert(sample.end(), payload.begin(), payload.end());
  return sample;
}

/** Parses the samples of a subtitle track with the default factory. */
std::vector<coral::AVCuesWithTiming> parseTrack(const MediaFormat& format,
                                                const std::vector<DeliveredSample>& samples) {
  const auto factory = coral::createDefaultSubtitleParserFactory();
  std::vector<coral::AVCuesWithTiming> results;
  if (!factory->supportsFormat(format)) {
    return results;
  }
  const auto parser = factory->create(format);
  if (parser == nullptr) {
    return results;
  }
  for (const auto& sample : samples) {
    parser->parse(sample.payload.data(), sample.payload.size(),
                  AVSubtitleParser::OutputOptions::allCues(),
                  [&results](const coral::AVCuesWithTiming& cues) { results.push_back(cues); });
  }
  return results;
}

/** Returns the text of the cues of a group, joined by `|`. */
std::string textsOf(const coral::AVCuesWithTiming& cues) {
  std::string text;
  for (const auto& cue : cues.cues) {
    if (!text.empty()) {
      text += "|";
    }
    text += cue.text;
  }
  return text;
}

/** Ties a DTVCC payload to its caption box, as a QuickTime c708 sample does. */
std::vector<uint8_t> captionBox(const char type[5], const std::vector<uint8_t>& payload) {
  std::vector<uint8_t> box;
  const uint32_t size = static_cast<uint32_t>(8 + payload.size());
  for (int shift = 24; shift >= 0; shift -= 8) {
    box.push_back(static_cast<uint8_t>((size >> shift) & 0xFF));
  }
  for (int i = 0; i < 4; ++i) {
    box.push_back(static_cast<uint8_t>(type[i]));
  }
  box.insert(box.end(), payload.begin(), payload.end());
  return box;
}

/** A CEA-708 cc_data triplet stream that draws "Hello" in a visible window. */
std::vector<uint8_t> cea708CcData() {
  std::vector<uint8_t> commands = {0x98, 0x20, 0x4A, 0x68, 0x40, 0x00, 0x00};
  commands.insert(commands.end(), {'H', 'e', 'l', 'l', 'o'});
  commands.push_back(0x03);  // ETX
  commands.push_back(0x00);  // pad the DTVCC packet to an odd length
  std::vector<uint8_t> packet;
  packet.push_back(static_cast<uint8_t>((1 << 5) | commands.size()));
  packet.insert(packet.end(), commands.begin(), commands.end());
  const int packetSize = static_cast<int>((packet.size() + 1) / 2);
  std::vector<uint8_t> cc;
  cc.push_back(0x07);  // DTVCC start + valid
  cc.push_back(static_cast<uint8_t>(packetSize));
  cc.push_back(packet[0]);
  for (size_t i = 1; i + 1 < packet.size(); i += 2) {
    cc.push_back(0x06);  // DTVCC data + valid
    cc.push_back(packet[i]);
    cc.push_back(packet[i + 1]);
  }
  return cc;
}

std::vector<uint8_t> cea708Sample() {
  return captionBox("ccdp", cea708CcData());
}

/** The ATSC user_data_registered_itu_t_t35 SEI NAL that carries @p cc. */
std::vector<uint8_t> cea708SeiNal(const std::vector<uint8_t>& cc) {
  std::vector<uint8_t> payload = {0xB5, 0x00, 0x31, 'G', 'A', '9', '4', 0x03,
    static_cast<uint8_t>(0x40 | (cc.size() / 3)), 0x00};
  payload.insert(payload.end(), cc.begin(), cc.end());
  std::vector<uint8_t> nal = {0x06, 0x04, static_cast<uint8_t>(payload.size())};
  nal.insert(nal.end(), payload.begin(), payload.end());
  nal.push_back(0x80);
  return nal;
}

std::vector<uint8_t> avccSample(const std::vector<std::vector<uint8_t>>& nals) {
  std::vector<uint8_t> sample;
  for (const auto& nal: nals) {
    coral::test::putUnsignedInt(sample, static_cast<uint32_t>(nal.size()));
    sample.insert(sample.end(), nal.begin(), nal.end());
  }
  return sample;
}

std::vector<uint8_t> avcC() {
  return {1, 0x42, 0x00, 0x1E, 0xFF, 0xE1, 0x00, 0x04, 0x67, 0x42, 0x00, 0x1E, 0x01, 0x00, 0x02,
    0x68, 0xCE};
}

} // namespace

TEST(ContainerSubtitleTest, DeliversTheTx3gTrackOfAnMp4) {
  const std::vector<std::vector<uint8_t>> payloads = {tx3gSample("Hello"), tx3gSample("Second")};
  ReaderHarness harness(coral::test::buildMp4WithSubtitleTrack("tx3g", payloads), FileTypes::MP4);
  harness.consumeEveryTrack();
  ASSERT_TRUE(harness.prepare());
  harness.readAll();

  const int trackId = harness.trackIdOfType(MediaType::kMediaTypeSubtitle);
  ASSERT_NE(Definitions::kIndexUnset, trackId);
  EXPECT_EQ("MP4", harness.reader().getSelectedExtractorName());

  const auto format = harness.reader().getTrackFormat(trackId);
  ASSERT_NE(nullptr, format);
  EXPECT_TRUE(format->isSubtitle());
  EXPECT_EQ(MimeTypes::APPLICATION_TX3G, format->getMimeType());
  EXPECT_EQ(MediaFormat::OFFSET_SAMPLE_RELATIVE, format->getSubsampleOffsetUs());
  EXPECT_EQ(CueReplacementBehavior::kReplace, format->getCueReplacementBehavior());

  const auto& delivered = harness.samples(trackId);
  ASSERT_EQ(2u, delivered.size());
  EXPECT_EQ(0, delivered[0].timeUs);
  EXPECT_EQ(1000000, delivered[1].timeUs);
  // The sample of the format is a length prefixed text, which the parser decodes.
  ASSERT_GE(delivered[0].payload.size(), 2u);
  EXPECT_EQ(0u, delivered[0].payload[0]);
  EXPECT_EQ(5u, delivered[0].payload[1]);
}

TEST(ContainerSubtitleTest, ParsesTheCuesOfATx3gTrack) {
  const std::vector<std::vector<uint8_t>> payloads = {tx3gSample("Hello"), tx3gSample("Second")};
  ReaderHarness harness(coral::test::buildMp4WithSubtitleTrack("tx3g", payloads), FileTypes::MP4);
  harness.consumeEveryTrack();
  ASSERT_TRUE(harness.prepare());
  harness.readAll();

  const int trackId = harness.trackIdOfType(MediaType::kMediaTypeSubtitle);
  ASSERT_NE(Definitions::kIndexUnset, trackId);
  const auto format = harness.reader().getTrackFormat(trackId);
  const auto results = parseTrack(*format, harness.samples(trackId));
  ASSERT_EQ(2u, results.size());
  EXPECT_EQ("Hello", textsOf(results[0]));
  EXPECT_EQ("Second", textsOf(results[1]));

  // A timed text sample replaces the cues that are shown, so the timeline shows the cue of the
  // sample that was delivered last, over the interval of that sample.
  const auto& deliveredSamples = harness.samples(trackId);
  AVCueTimeline timeline;
  timeline.add(results[0], deliveredSamples[0].timeUs, deliveredSamples[0].timeUs + 1000000,
               format->getCueReplacementBehavior(), format->getSubsampleOffsetUs());
  ASSERT_EQ(1u, timeline.cuesAt(0).getCues().size());
  EXPECT_EQ("Hello", timeline.cuesAt(0).getCues()[0].text);
  EXPECT_TRUE(timeline.cuesAt(1000000).isEmpty());

  timeline.add(results[1], deliveredSamples[1].timeUs, deliveredSamples[1].timeUs + 1000000,
               format->getCueReplacementBehavior(), format->getSubsampleOffsetUs());
  ASSERT_EQ(1u, timeline.cuesAt(1000000).getCues().size());
  EXPECT_EQ("Second", timeline.cuesAt(1000000).getCues()[0].text);
  EXPECT_TRUE(timeline.cuesAt(2000000).isEmpty());
}

TEST(ContainerSubtitleTest, DeliversTheWvttTrackOfAnMp4) {
  const std::vector<std::vector<uint8_t>> payloads = {vttcSample("Hello"), vttcSample("Second")};
  ReaderHarness harness(coral::test::buildMp4WithSubtitleTrack("wvtt", payloads), FileTypes::MP4);
  harness.consumeEveryTrack();
  ASSERT_TRUE(harness.prepare());
  harness.readAll();

  const int trackId = harness.trackIdOfType(MediaType::kMediaTypeSubtitle);
  ASSERT_NE(Definitions::kIndexUnset, trackId);
  const auto format = harness.reader().getTrackFormat(trackId);
  ASSERT_NE(nullptr, format);
  EXPECT_EQ(MimeTypes::APPLICATION_MP4VTT, format->getMimeType());
  EXPECT_EQ(MediaFormat::OFFSET_SAMPLE_RELATIVE, format->getSubsampleOffsetUs());
  EXPECT_EQ(CueReplacementBehavior::kReplace, format->getCueReplacementBehavior());

  const auto results = parseTrack(*format, harness.samples(trackId));
  ASSERT_EQ(2u, results.size());
  EXPECT_EQ("Hello", textsOf(results[0]));
  EXPECT_EQ("Second", textsOf(results[1]));

  const auto& deliveredSamples = harness.samples(trackId);
  AVCueTimeline timeline;
  timeline.add(results[0], deliveredSamples[0].timeUs, deliveredSamples[0].timeUs + 1000000,
               format->getCueReplacementBehavior(), format->getSubsampleOffsetUs());
  EXPECT_EQ("Hello", timeline.cuesAt(0).getCues()[0].text);
  timeline.add(results[1], deliveredSamples[1].timeUs, deliveredSamples[1].timeUs + 1000000,
               format->getCueReplacementBehavior(), format->getSubsampleOffsetUs());
  EXPECT_EQ("Second", timeline.cuesAt(1000000).getCues()[0].text);
  EXPECT_TRUE(timeline.cuesAt(2000000).isEmpty());
}

TEST(ContainerSubtitleTest, DeliversTheStppTrackOfAnMp4) {
  const std::string document = "<?xml version=\"1.0\"?><tt><body><div><p begin=\"0.2s\" end=\"1.5s\">"
                               "Hello</p></div></body></tt>";
  const std::vector<std::vector<uint8_t>> payloads = {
    std::vector<uint8_t>(document.begin(), document.end())};
  ReaderHarness harness(coral::test::buildMp4WithSubtitleTrack("stpp", payloads), FileTypes::MP4);
  harness.consumeEveryTrack();
  ASSERT_TRUE(harness.prepare());
  harness.readAll();

  const int trackId = harness.trackIdOfType(MediaType::kMediaTypeSubtitle);
  ASSERT_NE(Definitions::kIndexUnset, trackId);
  const auto format = harness.reader().getTrackFormat(trackId);
  ASSERT_NE(nullptr, format);
  EXPECT_EQ(MimeTypes::APPLICATION_TTML, format->getMimeType());
  // The samples of a TTML track state absolute times, so they are not relative to the sample.
  EXPECT_EQ(0, format->getSubsampleOffsetUs());
  // The TTML parser is ported, so the track is both described and parsed.
  const auto factory = coral::createDefaultSubtitleParserFactory();
  EXPECT_TRUE(factory->supportsFormat(*format));
  ASSERT_EQ(1u, harness.samples(trackId).size());
  const auto results = parseTrack(*format, harness.samples(trackId));
  ASSERT_EQ(1u, results.size());
  EXPECT_EQ("Hello", textsOf(results[0]));
  EXPECT_EQ(200000, results[0].startTimeUs);
}

TEST(ContainerSubtitleTest, DeliversTheId3MetadataTrackOfAnMp4) {
  const std::vector<std::vector<uint8_t>> payloads = {
    {'I', 'D', '3', 4, 0, 0, 0, 0, 0, 0},
  };
  ReaderHarness harness(coral::test::buildMp4WithSubtitleTrack("id3 ", payloads), FileTypes::MP4);
  harness.consumeEveryTrack();
  ASSERT_TRUE(harness.prepare());
  harness.readAll();

  const int trackId = harness.trackIdOfType(MediaType::kMediaTypeMetadata);
  ASSERT_NE(Definitions::kIndexUnset, trackId);
  const auto format = harness.reader().getTrackFormat(trackId);
  ASSERT_NE(nullptr, format);
  EXPECT_EQ(MimeTypes::APPLICATION_ID3, format->getMimeType());
  ASSERT_EQ(1u, harness.samples(trackId).size());
  EXPECT_EQ(0, harness.samples(trackId)[0].timeUs);
  EXPECT_EQ(payloads[0], harness.samples(trackId)[0].payload);
}

TEST(ContainerSubtitleTest, DeliversATopLevelEmsgMetadataTrack) {
  std::vector<uint8_t> emsg;
  coral::test::putUnsignedInt(emsg, 0);  // version and flags
  for (const char c : std::string("urn:example:emsg")) emsg.push_back(static_cast<uint8_t>(c));
  emsg.push_back(0);
  for (const char c : std::string("value")) emsg.push_back(static_cast<uint8_t>(c));
  emsg.push_back(0);
  coral::test::putUnsignedInt(emsg, 1000);  // timescale
  coral::test::putUnsignedInt(emsg, 250);   // presentation time
  coral::test::putUnsignedInt(emsg, 500);   // event duration
  coral::test::putUnsignedInt(emsg, 7);     // id
  emsg.push_back(0x01);

  ReaderHarness harness(coral::test::buildMp4WithEmsg(emsg), FileTypes::MP4);
  harness.consumeEveryTrack();
  ASSERT_TRUE(harness.prepare());
  harness.readAll();

  const int trackId = harness.trackIdOfType(MediaType::kMediaTypeMetadata);
  ASSERT_NE(Definitions::kIndexUnset, trackId);
  const auto format = harness.reader().getTrackFormat(trackId);
  ASSERT_NE(nullptr, format);
  EXPECT_EQ(MimeTypes::APPLICATION_EMSG, format->getMimeType());
  ASSERT_EQ(1u, harness.samples(trackId).size());
  EXPECT_EQ(250000, harness.samples(trackId)[0].timeUs);
  EXPECT_EQ(emsg, harness.samples(trackId)[0].payload);
}

TEST(ContainerSubtitleTest, DeliversAnEmsgAfterTheMoovOfAFragmentedMp4) {
  std::vector<uint8_t> emsg;
  coral::test::putUnsignedInt(emsg, 0);
  for (const char c : std::string("urn:example:emsg-after")) emsg.push_back(static_cast<uint8_t>(c));
  emsg.push_back(0);
  for (const char c : std::string("after")) emsg.push_back(static_cast<uint8_t>(c));
  emsg.push_back(0);
  coral::test::putUnsignedInt(emsg, 1000);
  coral::test::putUnsignedInt(emsg, 500);
  coral::test::putUnsignedInt(emsg, 500);
  coral::test::putUnsignedInt(emsg, 9);
  emsg.push_back(0x02);

  ReaderHarness harness(coral::test::buildFragmentedMp4WithEmsgAfterMoov(emsg), FileTypes::MP4);
  harness.consumeEveryTrack();
  ASSERT_TRUE(harness.prepare());
  harness.readAll();

  const int trackId = harness.trackIdOfType(MediaType::kMediaTypeMetadata);
  ASSERT_NE(Definitions::kIndexUnset, trackId);
  const auto format = harness.reader().getTrackFormat(trackId);
  ASSERT_NE(nullptr, format);
  EXPECT_EQ(MimeTypes::APPLICATION_EMSG, format->getMimeType());
  ASSERT_EQ(1u, harness.samples(trackId).size());
  EXPECT_EQ(500000, harness.samples(trackId)[0].timeUs);
  EXPECT_EQ(emsg, harness.samples(trackId)[0].payload);
}

TEST(ContainerSubtitleTest, ReadsTheTx3gTrackOfARealMp4) {
  const std::string path = assetPath("subtitle/mov_text.mp4");
  ASSERT_FALSE(path.empty());
  ReaderHarness harness(path);
  harness.consumeEveryTrack();
  ASSERT_TRUE(harness.prepare());
  harness.readAll();

  EXPECT_EQ("MP4", harness.reader().getSelectedExtractorName());
  const int trackId = harness.trackIdOfType(MediaType::kMediaTypeSubtitle);
  ASSERT_NE(Definitions::kIndexUnset, trackId);
  const auto format = harness.reader().getTrackFormat(trackId);
  ASSERT_NE(nullptr, format);
  EXPECT_EQ(MimeTypes::APPLICATION_TX3G, format->getMimeType());

  const auto results = parseTrack(*format, harness.samples(trackId));
  // The track of the file starts with an empty sample, which clears the screen, and the cues follow.
  std::vector<std::string> texts;
  for (const auto& cues : results) {
    const std::string text = textsOf(cues);
    if (!text.empty()) {
      texts.push_back(text);
    }
  }
  ASSERT_GE(texts.size(), 2u);
  EXPECT_EQ("Hello Coral", texts[0]);
  EXPECT_EQ("Second cue", texts[1]);
}

TEST(ContainerSubtitleTest, DeliversTheSubtitleTrackOfAMatroska) {
  const std::string path = assetPath("subtitle/srt.mkv");
  ASSERT_FALSE(path.empty());
  ReaderHarness harness(path);
  harness.consumeEveryTrack();
  ASSERT_TRUE(harness.prepare());
  harness.readAll();

  EXPECT_EQ("Matroska", harness.reader().getSelectedExtractorName());
  const int trackId = harness.trackIdOfType(MediaType::kMediaTypeSubtitle);
  ASSERT_NE(Definitions::kIndexUnset, trackId);

  const auto format = harness.reader().getTrackFormat(trackId);
  ASSERT_NE(nullptr, format);
  EXPECT_EQ(MimeTypes::APPLICATION_SUBRIP, format->getMimeType());
  EXPECT_EQ(MediaFormat::OFFSET_SAMPLE_RELATIVE, format->getSubsampleOffsetUs());
  EXPECT_EQ(CueReplacementBehavior::kMerge, format->getCueReplacementBehavior());

  const auto& delivered = harness.samples(trackId);
  ASSERT_EQ(2u, delivered.size());
  EXPECT_EQ(200000, delivered[0].timeUs);
  EXPECT_EQ(1800000, delivered[1].timeUs);
  // The extractor synthesizes the prefix of the cue, because the block of the container holds only
  // the text of the subtitle.
  const std::string first(delivered[0].payload.begin(), delivered[0].payload.end());
  EXPECT_EQ(0u, first.rfind("1\n00:00:00,000 --> ", 0));
  EXPECT_NE(std::string::npos, first.find("Hello Coral"));

  const auto results = parseTrack(*format, delivered);
  ASSERT_EQ(2u, results.size());
  EXPECT_EQ("Hello Coral", textsOf(results[0]));
  EXPECT_EQ("Second cue", textsOf(results[1]));
  // The times of the parsed document are relative to the block, so the cue starts at the time of the
  // sample and lasts the duration that the block declared.
  EXPECT_EQ(0, results[0].startTimeUs);
  EXPECT_EQ(1300000, results[0].durationUs);
  AVCueTimeline timeline;
  timeline.add(results[0], delivered[0].timeUs, Definitions::kTimeUnset,
               format->getCueReplacementBehavior(), format->getSubsampleOffsetUs());
  EXPECT_TRUE(timeline.cuesAt(delivered[0].timeUs - 1).isEmpty());
  EXPECT_EQ("Hello Coral", timeline.cuesAt(delivered[0].timeUs).getCues()[0].text);
  EXPECT_TRUE(timeline.cuesAt(delivered[0].timeUs + 1300000).isEmpty());
}

TEST(ContainerSubtitleTest, ReadsAStandaloneSubripDocument) {
  const std::string document = "1\n00:00:00,200 --> 00:00:01,500\nHello Coral\n"
                               "\n2\n00:00:01,800 --> 00:00:03,000\nSecond cue\n";
  ReaderHarness harness(std::vector<uint8_t>(document.begin(), document.end()));
  harness.consumeEveryTrack();
  ASSERT_TRUE(harness.prepare());
  harness.readAll();

  EXPECT_EQ("Subtitle", harness.reader().getSelectedExtractorName());
  const int trackId = harness.trackIdOfType(MediaType::kMediaTypeSubtitle);
  ASSERT_NE(Definitions::kIndexUnset, trackId);
  const auto format = harness.reader().getTrackFormat(trackId);
  ASSERT_NE(nullptr, format);
  EXPECT_EQ(MimeTypes::APPLICATION_SUBRIP, format->getMimeType());
  EXPECT_EQ(CueReplacementBehavior::kMerge, format->getCueReplacementBehavior());

  const auto& delivered = harness.samples(trackId);
  ASSERT_EQ(1u, delivered.size());
  EXPECT_EQ(0, delivered[0].timeUs);
  EXPECT_EQ(document.size(), delivered[0].payload.size());

  const auto results = parseTrack(*format, delivered);
  ASSERT_EQ(2u, results.size());
  EXPECT_EQ(200000, results[0].startTimeUs);
  EXPECT_EQ("Hello Coral", textsOf(results[0]));
  EXPECT_EQ("Second cue", textsOf(results[1]));
}

TEST(ContainerSubtitleTest, ReadsAStandaloneWebvttDocument) {
  const std::string document = "WEBVTT\n\n00:00:00.200 --> 00:00:01.500\nHello Coral\n";
  ReaderHarness harness(std::vector<uint8_t>(document.begin(), document.end()));
  harness.consumeEveryTrack();
  ASSERT_TRUE(harness.prepare());
  harness.readAll();

  EXPECT_EQ("Subtitle", harness.reader().getSelectedExtractorName());
  const int trackId = harness.trackIdOfType(MediaType::kMediaTypeSubtitle);
  ASSERT_NE(Definitions::kIndexUnset, trackId);
  const auto format = harness.reader().getTrackFormat(trackId);
  ASSERT_NE(nullptr, format);
  EXPECT_EQ(MimeTypes::TEXT_VTT, format->getMimeType());

  const auto results = parseTrack(*format, harness.samples(trackId));
  ASSERT_EQ(1u, results.size());
  EXPECT_EQ("Hello Coral", textsOf(results[0]));
}

TEST(ContainerSubtitleTest, ReadsAStandaloneSsaDocument) {
  const std::string document =
    "[Script Info]\nPlayResX: 320\nPlayResY: 240\n\n"
    "[V4+ Styles]\n"
    "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, "
    "Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, "
    "Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n"
    "Style: Default,Arial,18,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,"
    "2,2,10,10,10,1\n\n"
    "[Events]\n"
    "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
    "Dialogue: 0,0:00:00.20,0:00:01.50,Default,,0,0,0,,Hello Coral\n";
  ReaderHarness harness(std::vector<uint8_t>(document.begin(), document.end()));
  harness.consumeEveryTrack();
  ASSERT_TRUE(harness.prepare());
  harness.readAll();

  EXPECT_EQ("Subtitle", harness.reader().getSelectedExtractorName());
  const int trackId = harness.trackIdOfType(MediaType::kMediaTypeSubtitle);
  ASSERT_NE(Definitions::kIndexUnset, trackId);
  const auto format = harness.reader().getTrackFormat(trackId);
  ASSERT_NE(nullptr, format);
  EXPECT_EQ(MimeTypes::TEXT_SSA, format->getMimeType());

  const auto results = parseTrack(*format, harness.samples(trackId));
  ASSERT_EQ(1u, results.size());
  EXPECT_EQ("Hello Coral", textsOf(results[0]));
  EXPECT_EQ(200000, results[0].startTimeUs);
}

TEST(ContainerSubtitleTest, DoesNotClaimAnMp4AsASubtitleDocument) {
  const auto media = coral::test::buildMp4(2);
  ReaderHarness harness(std::vector<uint8_t>(media.begin(), media.end()));
  ASSERT_TRUE(harness.prepare());
  EXPECT_EQ("MP4", harness.reader().getSelectedExtractorName());
}

TEST(ContainerSubtitleTest, DeliversTheC708TrackOfAnMp4) {
  const std::vector<std::vector<uint8_t>> payloads = {cea708Sample()};
  ReaderHarness harness(coral::test::buildMp4WithSubtitleTrack("c708", payloads), FileTypes::MP4);
  harness.consumeEveryTrack();
  ASSERT_TRUE(harness.prepare());
  harness.readAll();

  const int trackId = harness.trackIdOfType(MediaType::kMediaTypeSubtitle);
  ASSERT_NE(Definitions::kIndexUnset, trackId);
  const auto format = harness.reader().getTrackFormat(trackId);
  ASSERT_NE(nullptr, format);
  EXPECT_EQ(MimeTypes::APPLICATION_CEA708, format->getMimeType());
  EXPECT_EQ("eng", format->getLanguage());
  EXPECT_EQ(1, format->getCaptionServiceNumber());
  EXPECT_FALSE(format->isDerivedTrack());
  EXPECT_NE(0u, format->getRoleFlags() & coral::kRoleFlagCaption);
  EXPECT_NE(0u, format->getRoleFlags() & coral::kRoleFlagEasyToRead);
  const auto factory = coral::createDefaultSubtitleParserFactory();
  EXPECT_TRUE(factory->supportsFormat(*format));

  const auto results = parseTrack(*format, harness.samples(trackId));
  ASSERT_EQ(1u, results.size());
  EXPECT_EQ("Hello", textsOf(results[0]));
}

TEST(ContainerSubtitleTest, DerivesTheC708TrackFromMp4VideoSei) {
  const auto sample = avccSample({cea708SeiNal(cea708CcData()), {0x65, 0x88}});
  ReaderHarness harness(coral::test::buildMp4WithVideoTrack({sample}, avcC()), FileTypes::MP4);
  harness.consumeEveryTrack();
  ASSERT_TRUE(harness.prepare());
  harness.readAll();

  const int trackId = harness.trackIdOfType(MediaType::kMediaTypeSubtitle);
  ASSERT_NE(Definitions::kIndexUnset, trackId);
  const auto format = harness.reader().getTrackFormat(trackId);
  ASSERT_NE(nullptr, format);
  EXPECT_EQ(MimeTypes::APPLICATION_CEA708, format->getMimeType());
  EXPECT_TRUE(format->isDerivedTrack());

  const auto results = parseTrack(*format, harness.samples(trackId));
  ASSERT_EQ(1u, results.size());
  EXPECT_EQ("Hello", textsOf(results[0]));
}