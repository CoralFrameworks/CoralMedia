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

#include <extractor/metadata/MetadataDecoder.h>

#include <media/Format.h>
#include <media/Metadata.h>
#include <media/MetadataEntries.h>
#include <media/Types.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using coral::AVMetadataDecoder;
using coral::AVMetadataDecoderFactory;
using coral::createDefaultMetadataDecoderFactory;
using coral::EmsgMetadataEntry;
using coral::Id3FrameMetadataEntry;
using coral::MediaFormat;
using coral::Metadata;
using coral::MimeTypes;
using coral::Scte35MetadataEntry;

namespace {

void appendUInt32(std::vector<uint8_t>& out, const uint32_t value) {
  out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>(value & 0xFF));
}

void appendUInt64(std::vector<uint8_t>& out, const uint64_t value) {
  appendUInt32(out, static_cast<uint32_t>(value >> 32));
  appendUInt32(out, static_cast<uint32_t>(value & 0xFFFFFFFF));
}

void appendString(std::vector<uint8_t>& out, const std::string& value) {
  out.insert(out.end(), value.begin(), value.end());
  out.push_back(0);
}

std::shared_ptr<Metadata> makeMetadata() {
  return std::make_shared<Metadata>(std::vector<std::shared_ptr<Metadata::Entry>>{});
}

/** Builds an ID3v2.3 tag holding one UTF-8 text frame. */
std::vector<uint8_t> makeId3Tag(const std::string& frameId, const std::string& text) {
  std::vector<uint8_t> payload = {3};
  payload.insert(payload.end(), text.begin(), text.end());
  std::vector<uint8_t> frame;
  frame.insert(frame.end(), frameId.begin(), frameId.end());
  appendUInt32(frame, static_cast<uint32_t>(payload.size()));
  frame.push_back(0);
  frame.push_back(0);
  frame.insert(frame.end(), payload.begin(), payload.end());
  std::vector<uint8_t> tag = {'I', 'D', '3', 3, 0, 0};
  tag.push_back(static_cast<uint8_t>((frame.size() >> 21) & 0x7F));
  tag.push_back(static_cast<uint8_t>((frame.size() >> 14) & 0x7F));
  tag.push_back(static_cast<uint8_t>((frame.size() >> 7) & 0x7F));
  tag.push_back(static_cast<uint8_t>(frame.size() & 0x7F));
  tag.insert(tag.end(), frame.begin(), frame.end());
  return tag;
}

std::shared_ptr<MediaFormat> makeFormat(const std::string& mime) {
  auto format = MediaFormat::CreateSubtitleFormat(mime);
  return format;
}

} // namespace

TEST(AVMetadataDecoderTest, DecodesAnId3Sample) {
  const auto factory = createDefaultMetadataDecoderFactory();
  ASSERT_TRUE(factory->supportsFormat(*makeFormat(MimeTypes::APPLICATION_ID3)));
  const auto decoder = factory->create(*makeFormat(MimeTypes::APPLICATION_ID3));
  ASSERT_NE(nullptr, decoder);

  const auto tag = makeId3Tag("TIT2", "Title");
  const auto metadata = makeMetadata();
  ASSERT_TRUE(decoder->decode(tag.data(), tag.size(), metadata));
  ASSERT_EQ(1u, metadata->getEntries().size());
  const auto* frame = dynamic_cast<Id3FrameMetadataEntry*>(metadata->getEntries()[0].get());
  ASSERT_NE(nullptr, frame);
  EXPECT_EQ("TIT2", frame->getFrameId());
  EXPECT_EQ("Title", frame->getText());
}

TEST(AVMetadataDecoderTest, DecodesAnEmsgEventOfVersionZero) {
  const auto factory = createDefaultMetadataDecoderFactory();
  const auto decoder = factory->create(*makeFormat(MimeTypes::APPLICATION_EMSG));
  ASSERT_NE(nullptr, decoder);

  std::vector<uint8_t> payload = {0, 0, 0, 0};  // version 0, flags 0
  appendString(payload, "urn:example:event");
  appendString(payload, "value");
  appendUInt32(payload, 1000);  // the timescale
  appendUInt32(payload, 0);     // the presentation time delta
  appendUInt32(payload, 1500);  // the event duration
  appendUInt32(payload, 42);    // the identifier
  payload.push_back(1);
  payload.push_back(2);
  payload.push_back(3);

  const auto metadata = makeMetadata();
  ASSERT_TRUE(decoder->decode(payload.data(), payload.size(), metadata));
  ASSERT_EQ(1u, metadata->getEntries().size());
  const auto* event = dynamic_cast<EmsgMetadataEntry*>(metadata->getEntries()[0].get());
  ASSERT_NE(nullptr, event);
  EXPECT_EQ("urn:example:event", event->getSchemeIdUri());
  EXPECT_EQ("value", event->getValue());
  EXPECT_EQ(1'500'000, event->getEventDurationUs());
  EXPECT_EQ(42u, event->getId());
  ASSERT_EQ(3u, event->getMessageData().size());
  EXPECT_EQ(2, event->getMessageData()[1]);
}

TEST(AVMetadataDecoderTest, DecodesAnEmsgEventOfVersionOne) {
  const auto factory = createDefaultMetadataDecoderFactory();
  const auto decoder = factory->create(*makeFormat(MimeTypes::APPLICATION_EMSG));
  std::vector<uint8_t> payload = {1, 0, 0, 0};  // version 1, flags 0
  appendUInt32(payload, 1000);
  appendUInt64(payload, 5'000'000);
  appendUInt32(payload, 500);
  appendUInt32(payload, 7);
  appendString(payload, "urn:example:event");
  appendString(payload, "v1");
  const auto metadata = makeMetadata();
  ASSERT_TRUE(decoder->decode(payload.data(), payload.size(), metadata));
  ASSERT_EQ(1u, metadata->getEntries().size());
  const auto* event = dynamic_cast<EmsgMetadataEntry*>(metadata->getEntries()[0].get());
  ASSERT_NE(nullptr, event);
  EXPECT_EQ("urn:example:event", event->getSchemeIdUri());
  EXPECT_EQ("v1", event->getValue());
  EXPECT_EQ(500'000, event->getEventDurationUs());
  EXPECT_EQ(7u, event->getId());
}

TEST(AVMetadataDecoderTest, DecodesAScte35Section) {
  const auto factory = createDefaultMetadataDecoderFactory();
  const auto decoder = factory->create(*makeFormat(MimeTypes::APPLICATION_SCTE35));
  ASSERT_NE(nullptr, decoder);
  const std::vector<uint8_t> section = {0xFC, 0x30, 0x11, 0x00, 0x00};
  const auto metadata = makeMetadata();
  ASSERT_TRUE(decoder->decode(section.data(), section.size(), metadata));
  ASSERT_EQ(1u, metadata->getEntries().size());
  const auto* spliceInfo = dynamic_cast<Scte35MetadataEntry*>(metadata->getEntries()[0].get());
  ASSERT_NE(nullptr, spliceInfo);
  EXPECT_EQ(section, spliceInfo->getSpliceInfo());
}

TEST(AVMetadataDecoderTest, RejectsMalformedSamples) {
  const auto factory = createDefaultMetadataDecoderFactory();
  const auto emsg = factory->create(*makeFormat(MimeTypes::APPLICATION_EMSG));
  auto metadata = makeMetadata();
  const std::vector<uint8_t> truncated = {0, 0, 0, 0, 'a', 0};
  EXPECT_FALSE(emsg->decode(truncated.data(), truncated.size(), metadata));
  const auto scte35 = factory->create(*makeFormat(MimeTypes::APPLICATION_SCTE35));
  const std::vector<uint8_t> notASection = {0x00, 1, 2, 3};
  EXPECT_FALSE(scte35->decode(notASection.data(), notASection.size(), metadata));
  EXPECT_TRUE(metadata->getEntries().empty());
}

TEST(AVMetadataDecoderTest, ReportsTheFormatsItSupports) {
  const auto factory = createDefaultMetadataDecoderFactory();
  EXPECT_TRUE(factory->supportsFormat(*makeFormat(MimeTypes::APPLICATION_ID3)));
  EXPECT_TRUE(factory->supportsFormat(*makeFormat(MimeTypes::APPLICATION_EMSG)));
  EXPECT_TRUE(factory->supportsFormat(*makeFormat(MimeTypes::APPLICATION_SCTE35)));
  EXPECT_FALSE(factory->supportsFormat(*makeFormat(MimeTypes::TEXT_VTT)));
  EXPECT_EQ(nullptr, factory->create(*makeFormat(MimeTypes::TEXT_VTT)));
}
