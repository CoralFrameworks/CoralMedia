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

#ifndef CORAL_EXTRACTOR_TEST_MEDIA_H
#define CORAL_EXTRACTOR_TEST_MEDIA_H

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace coral {
namespace test {

/**
 * Appends a big endian 16 bit value to a buffer.
 */
inline void putUnsignedShort(std::vector<uint8_t>& out, const uint16_t value) {
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>(value & 0xFF));
}

/**
 * Appends a little endian 16 bit value to a buffer.
 */
inline void putLittleEndianUnsignedShort(std::vector<uint8_t>& out, const uint16_t value) {
  out.push_back(static_cast<uint8_t>(value & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
}

/**
 * Appends a big endian 32 bit value to a buffer.
 */
inline void putUnsignedInt(std::vector<uint8_t>& out, const uint32_t value) {
  out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>(value & 0xFF));
}

/**
 * Appends a little endian 32 bit value to a buffer.
 */
inline void putLittleEndianUnsignedInt(std::vector<uint8_t>& out, const uint32_t value) {
  out.push_back(static_cast<uint8_t>(value & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

/**
 * Appends the four characters of a four byte identifier.
 */
inline void putFourCc(std::vector<uint8_t>& out, const char* code) {
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast<uint8_t>(code[i]));
  }
}

/**
 * Builds a PCM WAV file holding a ramp of 16 bit samples, mono or stereo.
 *
 * @param sampleRate The sample rate in Hz.
 * @param channelCount The number of channels.
 * @param sampleCount The number of frames, i.e. of samples per channel.
 * @param withListChunk Whether a LIST chunk is inserted before the data chunk.
 */
inline std::vector<uint8_t> buildWav(const int sampleRate, const int channelCount,
                                     const int sampleCount, const bool withListChunk = false) {
  const int bitsPerSample = 16;
  const int blockAlign = channelCount * bitsPerSample / 8;
  const int byteRate = sampleRate * blockAlign;
  const int dataSize = sampleCount * blockAlign;

  std::vector<uint8_t> payload(static_cast<size_t>(dataSize));
  for (int i = 0; i < sampleCount; ++i) {
    for (int c = 0; c < channelCount; ++c) {
      const auto sample = static_cast<int16_t>((i * 37 + c * 11) % 30000 - 15000);
      const size_t offset = static_cast<size_t>(i * blockAlign + c * 2);
      payload[offset] = static_cast<uint8_t>(sample & 0xFF);
      payload[offset + 1] = static_cast<uint8_t>((sample >> 8) & 0xFF);
    }
  }

  std::vector<uint8_t> fmt;
  putLittleEndianUnsignedShort(fmt, 1);
  putLittleEndianUnsignedShort(fmt, static_cast<uint16_t>(channelCount));
  putLittleEndianUnsignedInt(fmt, static_cast<uint32_t>(sampleRate));
  putLittleEndianUnsignedInt(fmt, static_cast<uint32_t>(byteRate));
  putLittleEndianUnsignedShort(fmt, static_cast<uint16_t>(blockAlign));
  putLittleEndianUnsignedShort(fmt, static_cast<uint16_t>(bitsPerSample));

  std::vector<uint8_t> body;
  putFourCc(body, "fmt ");
  putLittleEndianUnsignedInt(body, static_cast<uint32_t>(fmt.size()));
  body.insert(body.end(), fmt.begin(), fmt.end());

  if (withListChunk) {
    // A LIST chunk holding the INFO software tag.
    std::vector<uint8_t> info;
    putFourCc(info, "INFO");
    putFourCc(info, "ISFT");
    putLittleEndianUnsignedInt(info, 6);
    const char* const software = "coral";
    info.insert(info.end(), software, software + 6);
    putFourCc(body, "LIST");
    putLittleEndianUnsignedInt(body, static_cast<uint32_t>(info.size()));
    body.insert(body.end(), info.begin(), info.end());
    if (info.size() % 2 != 0) {
      body.push_back(0);
    }
  }

  putFourCc(body, "data");
  putLittleEndianUnsignedInt(body, static_cast<uint32_t>(payload.size()));
  body.insert(body.end(), payload.begin(), payload.end());

  std::vector<uint8_t> file;
  putFourCc(file, "RIFF");
  putLittleEndianUnsignedInt(file, static_cast<uint32_t>(body.size() + 4));
  putFourCc(file, "WAVE");
  file.insert(file.end(), body.begin(), body.end());
  return file;
}

/**
 * Builds a stream of MPEG-1 layer 3 frames.
 *
 * <p>Each frame carries a real MPEG-1 layer 3 header at 128 kbit/s and 44100 Hz, which is the
 * 417 byte frame size of the format, so that the frame chain of the extractor follows the headers
 * of the frames rather than the accidental byte patterns of the payload.
 *
 * @param frameCount The number of frames.
 * @param withId3 Whether an ID3v2.3 tag is prepended to the stream.
 */
/** Builds an ID3v2.3 tag holding one UTF-8 title frame. */
inline std::vector<uint8_t> buildId3TagWithTitle(const std::string& title) {
  std::vector<uint8_t> frame(10, 0);
  frame[0] = 'T';
  frame[1] = 'I';
  frame[2] = 'T';
  frame[3] = '2';
  const size_t payloadSize = 1 + title.size();
  frame[4] = static_cast<uint8_t>((payloadSize >> 24) & 0xFF);
  frame[5] = static_cast<uint8_t>((payloadSize >> 16) & 0xFF);
  frame[6] = static_cast<uint8_t>((payloadSize >> 8) & 0xFF);
  frame[7] = static_cast<uint8_t>(payloadSize & 0xFF);
  frame.push_back(3);  // the payload is UTF-8
  frame.insert(frame.end(), title.begin(), title.end());
  std::vector<uint8_t> tag = {'I', 'D', '3', 3, 0, 0};
  tag.push_back(static_cast<uint8_t>((frame.size() >> 21) & 0x7F));
  tag.push_back(static_cast<uint8_t>((frame.size() >> 14) & 0x7F));
  tag.push_back(static_cast<uint8_t>((frame.size() >> 7) & 0x7F));
  tag.push_back(static_cast<uint8_t>(frame.size() & 0x7F));
  tag.insert(tag.end(), frame.begin(), frame.end());
  return tag;
}
inline std::vector<uint8_t> buildMp3(const int frameCount, const bool withId3 = false) {
  // MPEG-1, layer 3, no CRC, 128 kbit/s, 44100 Hz, stereo: 144 * 128000 / 44100 + 0 = 417 bytes.
  constexpr uint32_t kHeader = 0xFFFB9000;
  constexpr int kFrameSize = 417;

  std::vector<uint8_t> out;
  if (withId3) {
    const auto tag = buildId3TagWithTitle("Coral");
    out.insert(out.end(), tag.begin(), tag.end());
  }

  for (int i = 0; i < frameCount; ++i) {
    std::vector<uint8_t> frame(kFrameSize, static_cast<uint8_t>(i));
    frame[0] = static_cast<uint8_t>((kHeader >> 24) & 0xFF);
    frame[1] = static_cast<uint8_t>((kHeader >> 16) & 0xFF);
    frame[2] = static_cast<uint8_t>((kHeader >> 8) & 0xFF);
    frame[3] = static_cast<uint8_t>(kHeader & 0xFF);
    out.insert(out.end(), frame.begin(), frame.end());
  }
  return out;
}

/**
 * Builds an ADTS stream of AAC frames, without any valid AAC payload.
 *
 * @param frameCount The number of frames.
 * @param sampleRateIndex The AAC sampling frequency index, 4 for 44100 Hz.
 * @param channelConfig The channel configuration, 2 for stereo.
 */
inline std::vector<uint8_t> buildAdts(const int frameCount, const int sampleRateIndex = 4,
                                      const int channelConfig = 2, const bool withId3 = false) {
  constexpr int kPayloadSize = 64;
  const int frameLength = kPayloadSize + 7;
  std::vector<uint8_t> out;
  if (withId3) {
    const auto tag = buildId3TagWithTitle("Coral");
    out.insert(out.end(), tag.begin(), tag.end());
  }
  for (int i = 0; i < frameCount; ++i) {
    std::vector<uint8_t> frame(static_cast<size_t>(frameLength), static_cast<uint8_t>(i));
    // Sync word, MPEG-4, layer 0, no CRC.
    frame[0] = 0xFF;
    frame[1] = 0xF1;
    // Profile 1 (AAC LC), the sampling frequency index, the channel configuration.
    frame[2] = static_cast<uint8_t>(((1 - 1) << 6) | (sampleRateIndex << 2) |
                                    ((channelConfig >> 2) & 0x01));
    frame[3] = static_cast<uint8_t>(((channelConfig & 0x03) << 6) |
                                    ((frameLength >> 11) & 0x03));
    frame[4] = static_cast<uint8_t>((frameLength >> 3) & 0xFF);
    frame[5] = static_cast<uint8_t>(((frameLength & 0x07) << 5) | 0x1F);
    // One raw data block, and the buffer fullness of a variable bitrate stream.
    frame[6] = 0xFC;
    out.insert(out.end(), frame.begin(), frame.end());
  }
  return out;
}

/**
 * Builds a native FLAC stream with a single STREAMINFO block.
 *
 * @param sampleRate The sample rate in Hz.
 * @param channelCount The number of channels.
 * @param bitsPerSample The number of bits per sample.
 * @param totalSamples The number of samples per channel.
 */
inline std::vector<uint8_t> buildFlac(const int sampleRate, const int channelCount,
                                      const int bitsPerSample, const int64_t totalSamples,
                                      const std::vector<std::string>& comments = {}) {
  std::vector<uint8_t> out;
  putFourCc(out, "fLaC");

  // The STREAMINFO block; it is the last one unless the file also states its comments.
  constexpr int kBlockSize = 34;
  out.push_back(comments.empty() ? 0x80 : 0x00);
  out.push_back(0);
  out.push_back(0);
  out.push_back(kBlockSize);
  putUnsignedShort(out, 4096);
  putUnsignedShort(out, 4096);
  out.push_back(0);
  out.push_back(0x10);
  out.push_back(0x00);
  out.push_back(0);
  out.push_back(0x10);
  out.push_back(0x00);

  const uint64_t packed = (static_cast<uint64_t>(sampleRate & 0xFFFFF) << 44) |
                          (static_cast<uint64_t>((channelCount - 1) & 0x7) << 41) |
                          (static_cast<uint64_t>((bitsPerSample - 1) & 0x1F) << 36) |
                          (static_cast<uint64_t>(totalSamples) & 0xFFFFFFFFFLL);
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<uint8_t>((packed >> (56 - i * 8)) & 0xFF));
  }
  for (int i = 0; i < 16; ++i) {
    out.push_back(0);
  }

  if (!comments.empty()) {
    // A VORBIS_COMMENT block: the vendor string and one comment per string, both with a little
    // endian length. The block is the last metadata block of the stream.
    std::vector<uint8_t> block;
    putLittleEndianUnsignedInt(block, 5);
    for (const char* c = "coral"; *c != '\0'; ++c) {
      block.push_back(static_cast<uint8_t>(*c));
    }
    putLittleEndianUnsignedInt(block, static_cast<uint32_t>(comments.size()));
    for (const auto& comment : comments) {
      putLittleEndianUnsignedInt(block, static_cast<uint32_t>(comment.size()));
      block.insert(block.end(), comment.begin(), comment.end());
    }
    out.push_back(0x84);
    out.push_back(static_cast<uint8_t>((block.size() >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((block.size() >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(block.size() & 0xFF));
    out.insert(out.end(), block.begin(), block.end());
  }

  // One syntactically valid frame header, followed by a payload of a fixed size.
  out.push_back(0xFF);
  out.push_back(0xF8);
  // A fixed block size of 4096 samples and the sample rate code 9, i.e. 44100 Hz.
  out.push_back(static_cast<uint8_t>((0x0C << 4) | 0x09));
  // Two independent channels, 16 bits per sample, reserved bit zero.
  out.push_back(static_cast<uint8_t>((0x01 << 4) | (0x04 << 1)));
  // The frame number, encoded on one byte, and the CRC of the header.
  out.push_back(0x00);
  out.push_back(0x00);
  for (int i = 0; i < 64; ++i) {
    out.push_back(static_cast<uint8_t>(i));
  }
  return out;
}

/**
 * Builds an ISO base media file holding a single PCM audio track.
 *
 * <p>The file is an unfragmented MP4: a {@code ftyp} box, a {@code moov} box holding the movie
 * header and one audio track whose sample table describes every sample, and a {@code mdat} box
 * holding the samples. The track uses the {@code sowt} sample entry, i.e. signed 16 bit little
 * endian PCM, so that no codec configuration record is needed.
 *
 * @param sampleCount The number of samples, each of 1024 bytes.
 */
inline std::vector<uint8_t> buildMp4(const int sampleCount) {
  constexpr uint32_t kTimescale = 1000;
  constexpr int kSampleSize = 1024;
  constexpr int kSamplesPerChunk = 1;
  constexpr int kSampleRate = 44100;
  constexpr int kChannelCount = 1;

  const auto makeBox = [](const char* type, const std::vector<uint8_t>& content) {
    std::vector<uint8_t> box;
    // The layout of a box is: the size, the type, then the content.
    putUnsignedInt(box, static_cast<uint32_t>(8 + content.size()));
    putFourCc(box, type);
    box.insert(box.end(), content.begin(), content.end());
    return box;
  };
  const auto append = [](std::vector<uint8_t>& out, const std::vector<uint8_t>& content) {
    out.insert(out.end(), content.begin(), content.end());
  };

  // The payload of the mdat box.
  std::vector<uint8_t> payload(static_cast<size_t>(sampleCount) * kSampleSize);
  for (size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<uint8_t>(i);
  }

  const uint32_t unityMatrix[9] = {0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000};

  // mvhd: version 0, the timescale, the duration, the rate, the volume and the matrix.
  std::vector<uint8_t> mvhd;
  putUnsignedInt(mvhd, 0);                                   // version and flags
  putUnsignedInt(mvhd, 0);                                   // creation time
  putUnsignedInt(mvhd, 0);                                   // modification time
  putUnsignedInt(mvhd, kTimescale);                          // timescale
  putUnsignedInt(mvhd, static_cast<uint32_t>(sampleCount));  // duration
  putUnsignedInt(mvhd, 0x00010000);                          // rate 1.0
  putUnsignedShort(mvhd, 0x0100);                            // volume 1.0
  putUnsignedShort(mvhd, 0);                                 // reserved
  putUnsignedInt(mvhd, 0);                                   // reserved
  putUnsignedInt(mvhd, 0);                                   // reserved
  for (const uint32_t value : unityMatrix) {
    putUnsignedInt(mvhd, value);
  }
  for (int i = 0; i < 6; ++i) {
    putUnsignedInt(mvhd, 0);  // pre_defined
  }
  putUnsignedInt(mvhd, 0);  // next track id

  // tkhd: version 0, flags 3 (enabled and used by the movie), track id 1.
  std::vector<uint8_t> tkhd;
  putUnsignedInt(tkhd, 3);                                   // version and flags
  putUnsignedInt(tkhd, 0);                                   // creation time
  putUnsignedInt(tkhd, 0);                                   // modification time
  putUnsignedInt(tkhd, 1);                                   // track id
  putUnsignedInt(tkhd, 0);                                   // reserved
  putUnsignedInt(tkhd, static_cast<uint32_t>(sampleCount));  // duration
  putUnsignedInt(tkhd, 0);                                   // reserved
  putUnsignedInt(tkhd, 0);                                   // reserved
  putUnsignedShort(tkhd, 0);                                 // layer
  putUnsignedShort(tkhd, 0);                                 // alternate group
  putUnsignedShort(tkhd, 0x0100);                            // volume
  putUnsignedShort(tkhd, 0);                                 // reserved
  for (const uint32_t value : unityMatrix) {
    putUnsignedInt(tkhd, value);
  }
  putUnsignedInt(tkhd, 0);  // width
  putUnsignedInt(tkhd, 0);  // height

  // mdhd: version 0, the timescale, the duration and the packed language "und".
  std::vector<uint8_t> mdhd;
  putUnsignedInt(mdhd, 0);                                   // version and flags
  putUnsignedInt(mdhd, 0);                                   // creation time
  putUnsignedInt(mdhd, 0);                                   // modification time
  putUnsignedInt(mdhd, kTimescale);                          // timescale
  putUnsignedInt(mdhd, static_cast<uint32_t>(sampleCount));  // duration
  putUnsignedShort(mdhd, 0x55C4);                            // language "und"
  putUnsignedShort(mdhd, 0);                                 // pre_defined

  std::vector<uint8_t> hdlr;
  putUnsignedInt(hdlr, 0);  // version and flags
  putUnsignedInt(hdlr, 0);  // pre_defined
  putFourCc(hdlr, "soun");
  for (int i = 0; i < 3; ++i) {
    putUnsignedInt(hdlr, 0);  // reserved
  }
  const char* const handlerName = "SoundHandler";
  hdlr.insert(hdlr.end(), handlerName, handlerName + 13);

  // smhd.
  std::vector<uint8_t> smhd;
  putUnsignedInt(smhd, 0);    // version and flags
  putUnsignedShort(smhd, 0);  // balance
  putUnsignedShort(smhd, 0);  // reserved

  // dinf/dref/url.
  std::vector<uint8_t> url;
  putUnsignedInt(url, 1);  // version and flags: the media data is in the same file
  std::vector<uint8_t> dref;
  putUnsignedInt(dref, 0);  // version and flags
  putUnsignedInt(dref, 1);  // entry count
  append(dref, makeBox("url ", url));

  // The sample entry of the audio track: an "mp4a" sound description holding an esds box.
  std::vector<uint8_t> audioEntry;
  for (int i = 0; i < 6; ++i) {
    audioEntry.push_back(0);  // reserved
  }
  putUnsignedShort(audioEntry, 1);                       // data reference index
  putUnsignedShort(audioEntry, 0);                       // version
  putUnsignedShort(audioEntry, 0);                       // revision
  putUnsignedInt(audioEntry, 0);                         // vendor
  putUnsignedShort(audioEntry, kChannelCount);           // channel count
  putUnsignedShort(audioEntry, 16);                      // bits per sample
  putUnsignedShort(audioEntry, 0);                       // compression id
  putUnsignedShort(audioEntry, 0);                       // packet size
  putUnsignedInt(audioEntry, kSampleRate << 16);         // sample rate as a 16.16 value

  // The esds box describes the decoder configuration: MPEG-4 audio, AAC LC.
  std::vector<uint8_t> esds;
  putUnsignedInt(esds, 0);     // version and flags
  esds.push_back(0x03);        // the elementary stream descriptor
  esds.push_back(0x19);        // the length of the descriptor
  putUnsignedShort(esds, 1);   // the identifier of the elementary stream
  esds.push_back(0);           // the stream priority and the flags
  esds.push_back(0x04);        // the decoder configuration descriptor
  esds.push_back(0x11);        // the length of the descriptor
  esds.push_back(0x40);        // the object type indication: MPEG-4 audio
  esds.push_back(0x15);        // the stream type (audio) and the upstream flag
  esds.push_back(0);           // the buffer size, part 1
  esds.push_back(0);           // the buffer size, part 2
  esds.push_back(0);           // the buffer size, part 3
  putUnsignedInt(esds, 128000);  // the maximum bitrate
  putUnsignedInt(esds, 128000);  // the average bitrate
  esds.push_back(0x05);        // the decoder specific info
  esds.push_back(0x02);        // the length of the record
  esds.push_back(0x12);        // the AudioSpecificConfig: AAC LC, 44100 Hz, mono
  esds.push_back(0x08);
  esds.push_back(0x06);        // the sync layer descriptor
  esds.push_back(0x01);
  esds.push_back(0x02);        // the predefined value of the descriptor
  append(audioEntry, makeBox("esds", esds));
  const auto sampleDescription = makeBox("mp4a", audioEntry);

  // stsd.
  std::vector<uint8_t> stsd;
  putUnsignedInt(stsd, 0);  // version and flags
  putUnsignedInt(stsd, 1);  // entry count
  append(stsd, sampleDescription);

  // stts: every sample lasts one unit of the media timescale.
  std::vector<uint8_t> stts;
  putUnsignedInt(stts, 0);                                   // version and flags
  putUnsignedInt(stts, 1);                                   // entry count
  putUnsignedInt(stts, static_cast<uint32_t>(sampleCount));  // sample count
  putUnsignedInt(stts, 1);                                   // sample delta

  // stsc: one sample per chunk.
  std::vector<uint8_t> stsc;
  putUnsignedInt(stsc, 0);                        // version and flags
  putUnsignedInt(stsc, 1);                        // entry count
  putUnsignedInt(stsc, 1);                        // first chunk
  putUnsignedInt(stsc, kSamplesPerChunk);         // samples per chunk
  putUnsignedInt(stsc, 1);                        // sample description index

  // stsz: a constant sample size.
  std::vector<uint8_t> stsz;
  putUnsignedInt(stsz, 0);                                   // version and flags
  putUnsignedInt(stsz, kSampleSize);                         // sample size
  putUnsignedInt(stsz, static_cast<uint32_t>(sampleCount));  // sample count

  // stss: every sample is a sync sample.
  std::vector<uint8_t> stss;
  putUnsignedInt(stss, 0);                                   // version and flags
  putUnsignedInt(stss, static_cast<uint32_t>(sampleCount));  // entry count
  for (int i = 0; i < sampleCount; ++i) {
    putUnsignedInt(stss, static_cast<uint32_t>(i + 1));
  }

  // stco: the offset of every chunk, patched once the layout of the file is known.
  std::vector<uint8_t> stco;
  putUnsignedInt(stco, 0);                                   // version and flags
  putUnsignedInt(stco, static_cast<uint32_t>(sampleCount));  // entry count
  for (int i = 0; i < sampleCount; ++i) {
    putUnsignedInt(stco, 0);  // the offset of the chunk
  }

  std::vector<uint8_t> stbl;
  append(stbl, makeBox("stsd", stsd));
  append(stbl, makeBox("stts", stts));
  append(stbl, makeBox("stsc", stsc));
  append(stbl, makeBox("stsz", stsz));
  append(stbl, makeBox("stco", stco));
  append(stbl, makeBox("stss", stss));

  std::vector<uint8_t> minf;
  append(minf, makeBox("smhd", smhd));
  append(minf, makeBox("dinf", makeBox("dref", dref)));
  append(minf, makeBox("stbl", stbl));

  std::vector<uint8_t> mdia;
  append(mdia, makeBox("mdhd", mdhd));
  append(mdia, makeBox("hdlr", hdlr));
  append(mdia, makeBox("minf", minf));

  std::vector<uint8_t> trak;
  append(trak, makeBox("tkhd", tkhd));
  append(trak, makeBox("mdia", mdia));

  std::vector<uint8_t> moov;
  append(moov, makeBox("mvhd", mvhd));
  append(moov, makeBox("trak", trak));

  std::vector<uint8_t> ftyp;
  putFourCc(ftyp, "isom");
  putUnsignedInt(ftyp, 0x200);
  putFourCc(ftyp, "isom");
  putFourCc(ftyp, "iso2");

  std::vector<uint8_t> file;
  append(file, makeBox("ftyp", ftyp));
  append(file, makeBox("moov", moov));
  const long mdatPayloadOffset = static_cast<long>(file.size()) + 8;
  append(file, makeBox("mdat", payload));

  // Patch the chunk offsets of the sample table: the media data follows the moov box.
  const std::vector<uint8_t> stcoType = {'s', 't', 'c', 'o'};
  for (size_t i = 0; i + 4 <= file.size(); ++i) {
    if (file[i] != stcoType[0] || file[i + 1] != stcoType[1] || file[i + 2] != stcoType[2] ||
        file[i + 3] != stcoType[3]) {
      continue;
    }
    // The layout of the box is: size, type, version and flags, entry count, then the offsets.
    const size_t offsets = i + 4 + 4 + 4;
    for (int entry = 0; entry < sampleCount; ++entry) {
      const long offset = mdatPayloadOffset + static_cast<long>(entry) * kSampleSize;
      const size_t at = offsets + static_cast<size_t>(entry) * 4;
      if (at + 4 > file.size()) {
        break;
      }
      file[at] = static_cast<uint8_t>((offset >> 24) & 0xFF);
      file[at + 1] = static_cast<uint8_t>((offset >> 16) & 0xFF);
      file[at + 2] = static_cast<uint8_t>((offset >> 8) & 0xFF);
      file[at + 3] = static_cast<uint8_t>(offset & 0xFF);
    }
    break;
  }

  return file;
}

/**
 * Builds a minimal ISO base media file that holds one subtitle track.
 *
 * <p>The file is a file type box, a movie box and the media data, and its sample table declares one
 * chunk per sample so that a sample of any size can be described. The handler of the track is the
 * `text` handler, which is the one a timed text track of an MP4 file declares.
 *
 * @param entryType The four character code of the sample entry: `tx3g` or `wvtt`.
 * @param samples The payload of every sample, in order.
 * @param sampleDurationTicks The duration of every sample, in the timescale of the track.
 */
inline std::vector<uint8_t> buildMp4WithSubtitleTrack(
    const std::string& entryType, const std::vector<std::vector<uint8_t>>& samples,
    const uint32_t sampleDurationTicks = 1000) {
  constexpr uint32_t kTimescale = 1000;
  const uint32_t sampleCount = static_cast<uint32_t>(samples.size());

  const auto makeBox = [](const char* type, const std::vector<uint8_t>& content) {
    std::vector<uint8_t> box;
    putUnsignedInt(box, static_cast<uint32_t>(8 + content.size()));
    putFourCc(box, type);
    box.insert(box.end(), content.begin(), content.end());
    return box;
  };
  const auto append = [](std::vector<uint8_t>& out, const std::vector<uint8_t>& content) {
    out.insert(out.end(), content.begin(), content.end());
  };

  const uint32_t unityMatrix[9] = {0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000};

  const uint32_t durationTicks = sampleDurationTicks * sampleCount;

  std::vector<uint8_t> mvhd;
  putUnsignedInt(mvhd, 0);             // version and flags
  putUnsignedInt(mvhd, 0);             // creation time
  putUnsignedInt(mvhd, 0);             // modification time
  putUnsignedInt(mvhd, kTimescale);    // timescale
  putUnsignedInt(mvhd, durationTicks); // duration
  putUnsignedInt(mvhd, 0x00010000);    // rate
  putUnsignedShort(mvhd, 0x0100);      // volume
  putUnsignedShort(mvhd, 0);           // reserved
  putUnsignedInt(mvhd, 0);             // reserved
  putUnsignedInt(mvhd, 0);             // reserved
  for (const uint32_t value : unityMatrix) {
    putUnsignedInt(mvhd, value);
  }
  for (int i = 0; i < 6; ++i) {
    putUnsignedInt(mvhd, 0);
  }
  putUnsignedInt(mvhd, 2);  // next track id

  std::vector<uint8_t> tkhd;
  putUnsignedInt(tkhd, 3);             // version and flags: enabled and used by the movie
  putUnsignedInt(tkhd, 0);             // creation time
  putUnsignedInt(tkhd, 0);             // modification time
  putUnsignedInt(tkhd, 1);             // track id
  putUnsignedInt(tkhd, 0);             // reserved
  putUnsignedInt(tkhd, durationTicks); // duration
  putUnsignedInt(tkhd, 0);             // reserved
  putUnsignedInt(tkhd, 0);             // reserved
  putUnsignedShort(tkhd, 0);           // layer
  putUnsignedShort(tkhd, 0);           // alternate group
  putUnsignedShort(tkhd, 0);           // volume
  putUnsignedShort(tkhd, 0);           // reserved
  for (const uint32_t value : unityMatrix) {
    putUnsignedInt(tkhd, value);
  }
  putUnsignedInt(tkhd, 0);  // width
  putUnsignedInt(tkhd, 0);  // height

  std::vector<uint8_t> mdhd;
  putUnsignedInt(mdhd, 0);             // version and flags
  putUnsignedInt(mdhd, 0);             // creation time
  putUnsignedInt(mdhd, 0);             // modification time
  putUnsignedInt(mdhd, kTimescale);    // timescale
  putUnsignedInt(mdhd, durationTicks); // duration
  putUnsignedShort(mdhd, 0x55C4);      // language "und"
  putUnsignedShort(mdhd, 0);           // pre_defined

  std::vector<uint8_t> hdlr;
  putUnsignedInt(hdlr, 0);  // version and flags
  putUnsignedInt(hdlr, 0);  // pre_defined
  putFourCc(hdlr, "text");
  for (int i = 0; i < 3; ++i) {
    putUnsignedInt(hdlr, 0);  // reserved
  }
  for (const char* c = "TextHandler"; *c != '\0'; ++c) {
    hdlr.push_back(static_cast<uint8_t>(*c));
  }

  // The sample entry: the eight bytes that every sample entry starts with, then the fields of the
  // timed text entry itself.
  std::vector<uint8_t> entry;
  for (int i = 0; i < 6; ++i) {
    entry.push_back(0);  // reserved
  }
  putUnsignedShort(entry, 1);  // data reference index
  if (entryType == "tx3g") {
    putUnsignedInt(entry, 0);  // display flags
    entry.push_back(1);        // horizontal justification: centred
    entry.push_back(1);        // vertical justification: centred
    entry.push_back(0);        // background colour: transparent
    entry.push_back(0);
    entry.push_back(0);
    entry.push_back(0);
    for (int i = 0; i < 4; ++i) {
      putUnsignedShort(entry, 0);  // default text box: top, left, bottom, right
    }
    putUnsignedShort(entry, 0);  // start char
    putUnsignedShort(entry, 0);  // end char
    putUnsignedShort(entry, 1);  // font id
    entry.push_back(0);          // face style flags
    entry.push_back(18);         // font size
    entry.push_back(255);        // text colour: opaque white
    entry.push_back(255);
    entry.push_back(255);
    entry.push_back(255);
    std::vector<uint8_t> ftab;
    putUnsignedShort(ftab, 1);  // entry count
    putUnsignedShort(ftab, 1);  // font id
    ftab.push_back(4);          // the length of the name
    for (const char* c = "Sans"; *c != '\0'; ++c) {
      ftab.push_back(static_cast<uint8_t>(*c));
    }
    append(entry, makeBox("ftab", ftab));
  }
  if (entryType == "c708") {
    const std::vector<uint8_t> clcp = {1, 'e', 'n', 'g', 1, 0};
    append(entry, makeBox("clcp", clcp));
  }
  const auto sampleEntry = makeBox(entryType.c_str(), entry);

  std::vector<uint8_t> stsd;
  putUnsignedInt(stsd, 0);  // version and flags
  putUnsignedInt(stsd, 1);  // entry count
  append(stsd, sampleEntry);

  std::vector<uint8_t> stts;
  putUnsignedInt(stts, 0);                 // version and flags
  putUnsignedInt(stts, 1);                 // entry count
  putUnsignedInt(stts, sampleCount);       // sample count
  putUnsignedInt(stts, sampleDurationTicks);

  std::vector<uint8_t> stsc;
  putUnsignedInt(stsc, 0);           // version and flags
  putUnsignedInt(stsc, 1);           // entry count
  putUnsignedInt(stsc, 1);           // first chunk
  putUnsignedInt(stsc, 1);           // samples per chunk
  putUnsignedInt(stsc, 1);           // sample description index

  std::vector<uint8_t> stsz;
  putUnsignedInt(stsz, 0);  // version and flags
  putUnsignedInt(stsz, 0);  // sample size: the sizes are stated per sample
  putUnsignedInt(stsz, sampleCount);
  for (const auto& sample : samples) {
    putUnsignedInt(stsz, static_cast<uint32_t>(sample.size()));
  }

  std::vector<uint8_t> stco;
  putUnsignedInt(stco, 0);  // version and flags
  putUnsignedInt(stco, sampleCount);
  for (uint32_t i = 0; i < sampleCount; ++i) {
    putUnsignedInt(stco, 0);  // patched once the layout of the file is known
  }

  std::vector<uint8_t> nmhd;
  putUnsignedInt(nmhd, 0);  // version and flags

  std::vector<uint8_t> stbl;
  append(stbl, makeBox("stsd", stsd));
  append(stbl, makeBox("stts", stts));
  append(stbl, makeBox("stsc", stsc));
  append(stbl, makeBox("stsz", stsz));
  append(stbl, makeBox("stco", stco));

  std::vector<uint8_t> url;
  putUnsignedInt(url, 1);  // version and flags: the media data is in the same file
  std::vector<uint8_t> dref;
  putUnsignedInt(dref, 0);  // version and flags
  putUnsignedInt(dref, 1);  // entry count
  append(dref, makeBox("url ", url));

  std::vector<uint8_t> minf;
  append(minf, makeBox("nmhd", nmhd));
  append(minf, makeBox("dinf", makeBox("dref", dref)));
  append(minf, makeBox("stbl", stbl));

  std::vector<uint8_t> mdia;
  append(mdia, makeBox("mdhd", mdhd));
  append(mdia, makeBox("hdlr", hdlr));
  append(mdia, makeBox("minf", minf));

  std::vector<uint8_t> trak;
  append(trak, makeBox("tkhd", tkhd));
  append(trak, makeBox("mdia", mdia));

  std::vector<uint8_t> moov;
  append(moov, makeBox("mvhd", mvhd));
  append(moov, makeBox("trak", trak));

  std::vector<uint8_t> ftyp;
  putFourCc(ftyp, "isom");
  putUnsignedInt(ftyp, 0x200);
  putFourCc(ftyp, "isom");
  putFourCc(ftyp, "iso2");

  std::vector<uint8_t> payload;
  for (const auto& sample : samples) {
    payload.insert(payload.end(), sample.begin(), sample.end());
  }

  std::vector<uint8_t> file;
  append(file, makeBox("ftyp", ftyp));
  append(file, makeBox("moov", moov));
  const long mdatPayloadOffset = static_cast<long>(file.size()) + 8;
  append(file, makeBox("mdat", payload));

  // Patch the chunk offsets of the sample table: the media data follows the movie box.
  const std::vector<uint8_t> stcoType = {'s', 't', 'c', 'o'};
  for (size_t i = 0; i + 4 <= file.size(); ++i) {
    if (file[i] != stcoType[0] || file[i + 1] != stcoType[1] || file[i + 2] != stcoType[2] ||
        file[i + 3] != stcoType[3]) {
      continue;
    }
    // The layout of the box is: size, type, version and flags, entry count, then the offsets.
    const size_t offsets = i + 4 + 4 + 4;
    long offset = mdatPayloadOffset;
    for (uint32_t entryIndex = 0; entryIndex < sampleCount; ++entryIndex) {
      const size_t at = offsets + static_cast<size_t>(entryIndex) * 4;
      if (at + 4 > file.size()) {
        break;
      }
      file[at] = static_cast<uint8_t>((offset >> 24) & 0xFF);
      file[at + 1] = static_cast<uint8_t>((offset >> 16) & 0xFF);
      file[at + 2] = static_cast<uint8_t>((offset >> 8) & 0xFF);
      file[at + 3] = static_cast<uint8_t>(offset & 0xFF);
      offset += static_cast<long>(samples[entryIndex].size());
    }
    break;
  }

  return file;
}

/**
 * Builds a minimal MP4 with a top-level `emsg` box before its `moov` box.
 *
 * <p>The emsg payload is a complete version 0 event message. The extractors expose it as a
 * `application/x-emsg` metadata track whose sample timestamp is the presentation time of the
 * message.
 */
inline std::vector<uint8_t> buildMp4WithEmsg(const std::vector<uint8_t>& emsgPayload) {
  const auto makeBox = [](const char* type, const std::vector<uint8_t>& content) {
    std::vector<uint8_t> box;
    putUnsignedInt(box, static_cast<uint32_t>(8 + content.size()));
    putFourCc(box, type);
    box.insert(box.end(), content.begin(), content.end());
    return box;
  };

  std::vector<uint8_t> mvhd;
  putUnsignedInt(mvhd, 0);          // version and flags
  putUnsignedInt(mvhd, 0);          // creation time
  putUnsignedInt(mvhd, 0);          // modification time
  putUnsignedInt(mvhd, 1000);       // timescale
  putUnsignedInt(mvhd, 2000);       // duration
  putUnsignedInt(mvhd, 0x00010000); // rate
  putUnsignedShort(mvhd, 0x0100);   // volume
  putUnsignedShort(mvhd, 0);        // reserved
  putUnsignedInt(mvhd, 0);
  putUnsignedInt(mvhd, 0);
  const uint32_t unityMatrix[9] = {0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000};
  for (const uint32_t value : unityMatrix) putUnsignedInt(mvhd, value);
  for (int i = 0; i < 6; ++i) putUnsignedInt(mvhd, 0);
  putUnsignedInt(mvhd, 2);  // next track id

  std::vector<uint8_t> ftyp;
  putFourCc(ftyp, "isom");
  putUnsignedInt(ftyp, 0x200);
  putFourCc(ftyp, "isom");
  putFourCc(ftyp, "iso2");

  std::vector<uint8_t> file;
  const auto ftypBox = makeBox("ftyp", ftyp);
  const auto emsgBox = makeBox("emsg", emsgPayload);
  const auto moovBox = makeBox("moov", makeBox("mvhd", mvhd));
  file.insert(file.end(), ftypBox.begin(), ftypBox.end());
  file.insert(file.end(), emsgBox.begin(), emsgBox.end());
  file.insert(file.end(), moovBox.begin(), moovBox.end());
  return file;
}

/**
 * Builds a single-program transport stream with a SCTE-35 splice information PES stream.
 */
inline std::vector<uint8_t> buildTsWithScte35(const std::vector<uint8_t>& spliceInfo) {
  const auto packet = [](const int pid, const std::vector<uint8_t>& payload, const int counter,
                         const bool unitStart) {
    std::vector<uint8_t> out(188, 0xFF);
    out[0] = 0x47;
    out[1] = static_cast<uint8_t>(((unitStart ? 0x40 : 0) | ((pid >> 8) & 0x1F)));
    out[2] = static_cast<uint8_t>(pid & 0xFF);
    out[3] = static_cast<uint8_t>(0x10 | (counter & 0x0F));
    std::copy(payload.begin(), payload.end(), out.begin() + 4);
    return out;
  };

  std::vector<uint8_t> pes;
  pes.insert(pes.end(), {0x00, 0x00, 0x01, 0xE0, 0x00, 0x00, 0x80, 0x00, 0x00});
  pes.insert(pes.end(), spliceInfo.begin(), spliceInfo.end());

  std::vector<uint8_t> out;
  const auto patPacket = packet(0, std::vector<uint8_t>{0x00, 0x00, 0xB0, 0x0D, 0x00, 0x01, 0xC1,
      0x00, 0x00, 0x00, 0x01, 0xE1, 0x00, 0x00, 0x00, 0x00, 0x00}, 0, true);
  const auto pmtPacket = packet(0x100, std::vector<uint8_t>{0x00, 0x02, 0xB0, 0x12, 0x00, 0x01,
      0xC1, 0x00, 0x00, 0xE1, 0x00, 0xF0, 0x00, 0x86, 0xE1, 0x01, 0xF0, 0x00, 0x00, 0x00, 0x00,
      0x00}, 0, true);
  const auto pesPacket = packet(0x101, pes, 0, true);
  out.insert(out.end(), patPacket.begin(), patPacket.end());
  out.insert(out.end(), pmtPacket.begin(), pmtPacket.end());
  out.insert(out.end(), pesPacket.begin(), pesPacket.end());
  // The TS sniffer checks for five consecutive sync bytes.
  for (int i = 0; i < 2; ++i) {
    const auto nullPacket = packet(0x1FFF, {}, i, false);
    out.insert(out.end(), nullPacket.begin(), nullPacket.end());
  }
  return out;
}

/**
 * Builds a fragmented MP4 whose `emsg` box follows its `moov` box.
 *
 * <p>The `mvex` box makes the movie fragmented, so the extractor reserves its metadata track before
 * reading the top-level event message.
 */
inline std::vector<uint8_t> buildFragmentedMp4WithEmsgAfterMoov(
    const std::vector<uint8_t>& emsgPayload) {
  const auto makeBox = [](const char* type, const std::vector<uint8_t>& content) {
    std::vector<uint8_t> box;
    putUnsignedInt(box, static_cast<uint32_t>(8 + content.size()));
    putFourCc(box, type);
    box.insert(box.end(), content.begin(), content.end());
    return box;
  };

  std::vector<uint8_t> mvhd;
  putUnsignedInt(mvhd, 0);
  putUnsignedInt(mvhd, 0);
  putUnsignedInt(mvhd, 0);
  putUnsignedInt(mvhd, 1000);
  putUnsignedInt(mvhd, 2000);
  putUnsignedInt(mvhd, 0x00010000);
  putUnsignedShort(mvhd, 0x0100);
  putUnsignedShort(mvhd, 0);
  putUnsignedInt(mvhd, 0);
  putUnsignedInt(mvhd, 0);
  for (const uint32_t value : {0x00010000u, 0u, 0u, 0u, 0x00010000u, 0u, 0u, 0u, 0x40000000u}) {
    putUnsignedInt(mvhd, value);
  }
  for (int i = 0; i < 6; ++i) putUnsignedInt(mvhd, 0);
  putUnsignedInt(mvhd, 2);

  std::vector<uint8_t> ftyp;
  putFourCc(ftyp, "isom");
  putUnsignedInt(ftyp, 0x200);
  putFourCc(ftyp, "isom");
  putFourCc(ftyp, "iso2");

  std::vector<uint8_t> moovPayload = makeBox("mvhd", mvhd);
  const auto mvex = makeBox("mvex", {});
  moovPayload.insert(moovPayload.end(), mvex.begin(), mvex.end());
  const auto ftypBox = makeBox("ftyp", ftyp);
  const auto moovBox = makeBox("moov", moovPayload);
  const auto emsgBox = makeBox("emsg", emsgPayload);
  std::vector<uint8_t> file;
  file.insert(file.end(), ftypBox.begin(), ftypBox.end());
  file.insert(file.end(), moovBox.begin(), moovBox.end());
  file.insert(file.end(), emsgBox.begin(), emsgBox.end());
  return file;
}

/**
 * Builds a single-track MP4 whose track is an H.264 video track. The samples are AVCC samples:
 * each NAL unit is prefixed by its four byte length, as the `avcC` record below declares.
 */
inline std::vector<uint8_t> buildMp4WithVideoTrack(
    const std::vector<std::vector<uint8_t>>& samples, const std::vector<uint8_t>& avcC,
    const uint32_t sampleDurationTicks = 1000) {
  constexpr uint32_t kTimescale = 1000;
  const uint32_t sampleCount = static_cast<uint32_t>(samples.size());
  const auto makeBox = [](const char* type, const std::vector<uint8_t>& content) {
    std::vector<uint8_t> box;
    putUnsignedInt(box, static_cast<uint32_t>(8 + content.size()));
    putFourCc(box, type);
    box.insert(box.end(), content.begin(), content.end());
    return box;
  };
  const auto append = [](std::vector<uint8_t>& out, const std::vector<uint8_t>& content) {
    out.insert(out.end(), content.begin(), content.end());
  };

  const uint32_t unityMatrix[9] = {0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000};
  const uint32_t durationTicks = sampleDurationTicks * sampleCount;

  std::vector<uint8_t> mvhd;
  putUnsignedInt(mvhd, 0);
  putUnsignedInt(mvhd, 0);
  putUnsignedInt(mvhd, 0);
  putUnsignedInt(mvhd, kTimescale);
  putUnsignedInt(mvhd, durationTicks);
  putUnsignedInt(mvhd, 0x00010000);
  putUnsignedShort(mvhd, 0x0100);
  putUnsignedShort(mvhd, 0);
  putUnsignedInt(mvhd, 0);
  putUnsignedInt(mvhd, 0);
  for (const uint32_t value : unityMatrix) putUnsignedInt(mvhd, value);
  for (int i = 0; i < 6; ++i) putUnsignedInt(mvhd, 0);
  putUnsignedInt(mvhd, 2);

  std::vector<uint8_t> tkhd;
  putUnsignedInt(tkhd, 3);
  putUnsignedInt(tkhd, 0);
  putUnsignedInt(tkhd, 0);
  putUnsignedInt(tkhd, 1);
  putUnsignedInt(tkhd, 0);
  putUnsignedInt(tkhd, durationTicks);
  putUnsignedInt(tkhd, 0);
  putUnsignedInt(tkhd, 0);
  putUnsignedShort(tkhd, 0);
  putUnsignedShort(tkhd, 0);
  putUnsignedShort(tkhd, 0);
  putUnsignedShort(tkhd, 0);
  for (const uint32_t value : unityMatrix) putUnsignedInt(tkhd, value);
  putUnsignedInt(tkhd, 16 << 16);
  putUnsignedInt(tkhd, 16 << 16);

  std::vector<uint8_t> mdhd;
  putUnsignedInt(mdhd, 0);
  putUnsignedInt(mdhd, 0);
  putUnsignedInt(mdhd, 0);
  putUnsignedInt(mdhd, kTimescale);
  putUnsignedInt(mdhd, durationTicks);
  putUnsignedShort(mdhd, 0x55C4);
  putUnsignedShort(mdhd, 0);

  std::vector<uint8_t> hdlr;
  putUnsignedInt(hdlr, 0);
  putUnsignedInt(hdlr, 0);
  putFourCc(hdlr, "vide");
  for (int i = 0; i < 3; ++i) putUnsignedInt(hdlr, 0);
  for (const char* c = "VideoHandler"; *c != '\0'; ++c) {
    hdlr.push_back(static_cast<uint8_t>(*c));
  }

  std::vector<uint8_t> entry;
  for (int i = 0; i < 6; ++i) entry.push_back(0);
  putUnsignedShort(entry, 1);
  putUnsignedShort(entry, 0);
  putUnsignedShort(entry, 0);
  putUnsignedInt(entry, 0);
  putUnsignedInt(entry, 0);
  putUnsignedInt(entry, 0);
  putUnsignedShort(entry, 16);
  putUnsignedShort(entry, 16);
  putUnsignedInt(entry, 0x00480000);
  putUnsignedInt(entry, 0x00480000);
  putUnsignedInt(entry, 0);
  putUnsignedShort(entry, 1);
  for (int i = 0; i < 32; ++i) entry.push_back(0);
  putUnsignedShort(entry, 0x0018);
  putUnsignedShort(entry, 0xFFFF);
  append(entry, makeBox("avcC", avcC));
  const auto sampleEntry = makeBox("avc1", entry);

  std::vector<uint8_t> stsd;
  putUnsignedInt(stsd, 0);
  putUnsignedInt(stsd, 1);
  append(stsd, sampleEntry);

  std::vector<uint8_t> stts;
  putUnsignedInt(stts, 0);
  putUnsignedInt(stts, 1);
  putUnsignedInt(stts, sampleCount);
  putUnsignedInt(stts, sampleDurationTicks);

  std::vector<uint8_t> stsc;
  putUnsignedInt(stsc, 0);
  putUnsignedInt(stsc, 1);
  putUnsignedInt(stsc, 1);
  putUnsignedInt(stsc, 1);
  putUnsignedInt(stsc, 1);

  std::vector<uint8_t> stsz;
  putUnsignedInt(stsz, 0);
  putUnsignedInt(stsz, 0);
  putUnsignedInt(stsz, sampleCount);
  for (const auto& sample : samples) putUnsignedInt(stsz, static_cast<uint32_t>(sample.size()));

  std::vector<uint8_t> stco;
  putUnsignedInt(stco, 0);
  putUnsignedInt(stco, sampleCount);
  for (uint32_t i = 0; i < sampleCount; ++i) putUnsignedInt(stco, 0);

  std::vector<uint8_t> nmhd;
  putUnsignedInt(nmhd, 0);

  std::vector<uint8_t> stbl;
  append(stbl, makeBox("stsd", stsd));
  append(stbl, makeBox("stts", stts));
  append(stbl, makeBox("stsc", stsc));
  append(stbl, makeBox("stsz", stsz));
  append(stbl, makeBox("stco", stco));

  std::vector<uint8_t> url;
  putUnsignedInt(url, 1);
  std::vector<uint8_t> dref;
  putUnsignedInt(dref, 0);
  putUnsignedInt(dref, 1);
  append(dref, makeBox("url ", url));

  std::vector<uint8_t> minf;
  append(minf, makeBox("nmhd", nmhd));
  append(minf, makeBox("dinf", makeBox("dref", dref)));
  append(minf, makeBox("stbl", stbl));

  std::vector<uint8_t> mdia;
  append(mdia, makeBox("mdhd", mdhd));
  append(mdia, makeBox("hdlr", hdlr));
  append(mdia, makeBox("minf", minf));

  std::vector<uint8_t> trak;
  append(trak, makeBox("tkhd", tkhd));
  append(trak, makeBox("mdia", mdia));

  std::vector<uint8_t> moov;
  append(moov, makeBox("mvhd", mvhd));
  append(moov, makeBox("trak", trak));

  std::vector<uint8_t> ftyp;
  putFourCc(ftyp, "isom");
  putUnsignedInt(ftyp, 0x200);
  putFourCc(ftyp, "isom");
  putFourCc(ftyp, "iso2");

  std::vector<uint8_t> payload;
  for (const auto& sample : samples) payload.insert(payload.end(), sample.begin(), sample.end());

  std::vector<uint8_t> file;
  append(file, makeBox("ftyp", ftyp));
  append(file, makeBox("moov", moov));
  const long mdatPayloadOffset = static_cast<long>(file.size()) + 8;
  append(file, makeBox("mdat", payload));

  const std::vector<uint8_t> stcoType = {'s', 't', 'c', 'o'};
  for (size_t i = 0; i + 4 <= file.size(); ++i) {
    if (file[i] != stcoType[0] || file[i + 1] != stcoType[1] || file[i + 2] != stcoType[2] ||
        file[i + 3] != stcoType[3]) {
      continue;
    }
    const size_t offsets = i + 4 + 4 + 4;
    long offset = mdatPayloadOffset;
    for (uint32_t entryIndex = 0; entryIndex < sampleCount; ++entryIndex) {
      const size_t at = offsets + static_cast<size_t>(entryIndex) * 4;
      if (at + 4 > file.size()) break;
      file[at] = static_cast<uint8_t>((offset >> 24) & 0xFF);
      file[at + 1] = static_cast<uint8_t>((offset >> 16) & 0xFF);
      file[at + 2] = static_cast<uint8_t>((offset >> 8) & 0xFF);
      file[at + 3] = static_cast<uint8_t>(offset & 0xFF);
      offset += static_cast<long>(samples[entryIndex].size());
    }
    break;
  }
  return file;
}

/**
 * Appends the characters of a string to a buffer, without a terminating null.
 */
inline void putAscii(std::vector<uint8_t>& out, const char* text) {
  for (const char* c = text; *c != '\0'; ++c) {
    out.push_back(static_cast<uint8_t>(*c));
  }
}

// ==================================================================================================
// Ogg
// ==================================================================================================

/**
 * Writes the bits of a Vorbis setup header in the order in which the encoder writes them, i.e. from
 * the least significant bit of every byte.
 */
class VorbisBitWriter final {
public:
  VorbisBitWriter() = default;

  ~VorbisBitWriter() = default;

  VorbisBitWriter(const VorbisBitWriter& other) = delete;

  VorbisBitWriter& operator=(const VorbisBitWriter& other) = delete;

  /** Writes the given number of bits of a value, least significant bit first. */
  void writeBits(const uint32_t value, const int numBits) {
    for (int i = 0; i < numBits; ++i) {
      if (bitPosition_ % 8 == 0) {
        bytes_.push_back(0);
      }
      const auto bit = static_cast<uint8_t>(((value >> i) & 1U) << (bitPosition_ % 8));
      bytes_[bytes_.size() - 1] = static_cast<uint8_t>(bytes_[bytes_.size() - 1] | bit);
      ++bitPosition_;
    }
  }

  [[nodiscard]] const std::vector<uint8_t>& bytes() const { return bytes_; }

private:
  std::vector<uint8_t> bytes_;
  int bitPosition_ = 0;
};

/**
 * Returns the index of the highest set bit of a value, as the Vorbis specification defines it, i.e.
 * the number of bits that a value needs.
 */
inline int vorbisILog(const int x) {
  int value = 0;
  int remaining = x;
  while (remaining > 0) {
    ++value;
    remaining >>= 1;
  }
  return value;
}

/**
 * Returns the exponent that the identification header of a Vorbis stream stores for a block size,
 * i.e. its base two logarithm.
 */
inline int vorbisBlockSizeExponent(const int blockSize) {
  return vorbisILog(blockSize) - 1;
}

/**
 * Appends one Ogg page holding the given packets to a buffer.
 *
 * <p>The checksum of the page is written as zero, which the extractor does not validate. The
 * granule position of a page is the number of samples up to the end of the last packet that
 * completes in it.
 */
inline void appendOggPage(std::vector<uint8_t>& out, const int type, const int64_t granulePosition,
                          const uint32_t serialNumber, const uint32_t sequenceNumber,
                          const std::vector<std::vector<uint8_t>>& packets) {
  std::vector<uint8_t> laces;
  for (const auto& packet : packets) {
    size_t remaining = packet.size();
    while (remaining >= 255) {
      laces.push_back(255);
      remaining -= 255;
    }
    laces.push_back(static_cast<uint8_t>(remaining));
  }

  putFourCc(out, "OggS");
  out.push_back(0);  // The version of the bitstream.
  out.push_back(static_cast<uint8_t>(type));
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<uint8_t>((granulePosition >> (i * 8)) & 0xFF));
  }
  putLittleEndianUnsignedInt(out, serialNumber);
  putLittleEndianUnsignedInt(out, sequenceNumber);
  putLittleEndianUnsignedInt(out, 0);  // The checksum.
  out.push_back(static_cast<uint8_t>(laces.size()));
  out.insert(out.end(), laces.begin(), laces.end());
  for (const auto& packet : packets) {
    out.insert(out.end(), packet.begin(), packet.end());
  }
}

/**
 * Builds the identification header of a Vorbis stream.
 */
inline std::vector<uint8_t> buildVorbisIdentificationHeader(const int sampleRate,
                                                           const int channels,
                                                           const int blockSize0,
                                                           const int blockSize1,
                                                           const int nominalBitrate) {
  std::vector<uint8_t> out;
  out.push_back(0x01);
  putAscii(out, "vorbis");
  putLittleEndianUnsignedInt(out, 0);  // The version of the bitstream.
  out.push_back(static_cast<uint8_t>(channels));
  putLittleEndianUnsignedInt(out, static_cast<uint32_t>(sampleRate));
  putLittleEndianUnsignedInt(out, 0);  // The maximum bitrate.
  putLittleEndianUnsignedInt(out, static_cast<uint32_t>(nominalBitrate));
  putLittleEndianUnsignedInt(out, 0);  // The minimum bitrate.
  out.push_back(static_cast<uint8_t>((vorbisBlockSizeExponent(blockSize1) << 4) |
                                     vorbisBlockSizeExponent(blockSize0)));
  out.push_back(0x01);  // The framing bit.
  return out;
}

/**
 * Builds the comment header of a Vorbis stream, which carries a vendor string and one comment.
 */
inline std::vector<uint8_t> buildVorbisCommentHeader() {
  const char* const vendor = "coral";
  const char* const comment = "TITLE=fixture";
  std::vector<uint8_t> out;
  out.push_back(0x03);
  putAscii(out, "vorbis");
  putLittleEndianUnsignedInt(out, 5);
  putAscii(out, vendor);
  putLittleEndianUnsignedInt(out, 1);
  putLittleEndianUnsignedInt(out, 13);
  putAscii(out, comment);
  out.push_back(0x01);  // The framing bit.
  return out;
}

/**
 * Builds the setup header of a Vorbis stream, which declares one codebook, one floor, one residue,
 * one mapping and the modes that tell the block size of every audio packet.
 *
 * @param twoModes Whether a second mode, which uses the large block size, is declared.
 */
inline std::vector<uint8_t> buildVorbisSetupHeader(const bool twoModes = false) {
  std::vector<uint8_t> out;
  out.push_back(0x05);
  putAscii(out, "vorbis");

  VorbisBitWriter bits;
  bits.writeBits(0, 8);  // The number of codebooks, minus one.
  // A single codebook, which is not ordered, is not sparse and has no lookup table.
  bits.writeBits(0x564342, 24);
  bits.writeBits(1, 16);  // The dimensions.
  bits.writeBits(1, 24);  // The number of entries.
  bits.writeBits(0, 1);   // The ordered flag.
  bits.writeBits(0, 1);   // The sparse flag.
  bits.writeBits(0, 5);   // The length of the single entry.
  bits.writeBits(0, 4);   // The type of the lookup table.
  // A single time domain transform, which must be zero.
  bits.writeBits(0, 6);
  bits.writeBits(0, 16);
  // A single floor of type 1, without a partition.
  bits.writeBits(0, 6);
  bits.writeBits(1, 16);
  bits.writeBits(0, 5);  // The number of partitions.
  bits.writeBits(0, 2);  // The multiplier.
  bits.writeBits(0, 4);  // The number of bits of a value.
  // A single residue of type 0 with a single classification.
  bits.writeBits(0, 6);
  bits.writeBits(0, 16);
  bits.writeBits(0, 24);
  bits.writeBits(0, 24);
  bits.writeBits(0, 24);
  bits.writeBits(0, 6);  // The number of classifications, minus one.
  bits.writeBits(0, 8);  // The class book.
  bits.writeBits(0, 3);  // The low bits of the cascade.
  bits.writeBits(0, 1);  // The high bits of the cascade, which are absent.
  // A single mapping, which has neither a submap nor a coupling step.
  bits.writeBits(0, 6);
  bits.writeBits(0, 16);  // The type of the mapping.
  bits.writeBits(0, 1);   // The submaps flag.
  bits.writeBits(0, 1);   // The coupling flag.
  bits.writeBits(0, 2);   // The reserved bits.
  bits.writeBits(0, 8);   // The unused submap value.
  bits.writeBits(0, 8);   // The floor of the submap.
  bits.writeBits(0, 8);   // The residue of the submap.
  // The modes, which end the header.
  bits.writeBits(twoModes ? 1 : 0, 6);
  bits.writeBits(0, 1);  // The first mode uses the small block size.
  bits.writeBits(0, 16);
  bits.writeBits(0, 16);
  bits.writeBits(0, 8);
  if (twoModes) {
    bits.writeBits(1, 1);  // The second mode uses the large block size.
    bits.writeBits(0, 16);
    bits.writeBits(0, 16);
    bits.writeBits(0, 8);
  }
  bits.writeBits(1, 1);  // The framing bit.

  const auto& setup = bits.bytes();
  out.insert(out.end(), setup.begin(), setup.end());
  return out;
}

/**
 * Builds an Ogg container that carries a Vorbis stream.
 *
 * <p>Every audio packet is stored in its own page, so that the granule position of a page is the
 * granule at the end of the packet it carries. The packets after the first one each contribute the
 * samples that their block shares with the previous block.
 *
 * @param sampleRate The sample rate of the stream.
 * @param channels The number of channels.
 * @param audioPacketCount The number of audio packets.
 * @param packetSize The size of the payload of an audio packet.
 * @param twoModes Whether the stream declares the second, larger block size, which the packets of
 *                 every second pair use.
 */
inline std::vector<uint8_t> buildOggVorbis(const int sampleRate, const int channels,
                                           const int audioPacketCount, const int packetSize = 32,
                                           const bool twoModes = false) {
  constexpr int kBlockSize0 = 256;
  constexpr int kBlockSize1 = 2048;
  constexpr uint32_t kSerialNumber = 0x4C4F5241;  // "CORAL"

  std::vector<uint8_t> out;
  appendOggPage(out, 0x02, 0, kSerialNumber, 0,
                {buildVorbisIdentificationHeader(sampleRate, channels, kBlockSize0, kBlockSize1,
                                                 112000)});
  appendOggPage(out, 0x00, 0, kSerialNumber, 1,
                {buildVorbisCommentHeader(), buildVorbisSetupHeader(twoModes)});

  int64_t granulePosition = 0;
  int previousBlockSize = 0;
  for (int i = 0; i < audioPacketCount; ++i) {
    const int mode = twoModes && (i / 2) % 2 == 1 ? 1 : 0;
    const int blockSize = mode == 1 ? kBlockSize1 : kBlockSize0;
    std::vector<uint8_t> packet(static_cast<size_t>(packetSize), static_cast<uint8_t>(i));
    // The first bit of an audio packet is the packet type, which is zero, and the bits that follow
    // it are the number of the mode that describes the block size of the packet.
    packet[0] = static_cast<uint8_t>(mode << 1);
    if (i > 0) {
      granulePosition += (blockSize + previousBlockSize) / 4;
    }
    previousBlockSize = blockSize;
    const bool lastPacket = i == audioPacketCount - 1;
    appendOggPage(out, lastPacket ? 0x04 : 0x00, granulePosition, kSerialNumber,
                  static_cast<uint32_t>(2 + i), {packet});
  }
  return out;
}

/**
 * Builds an Ogg container that carries an Opus stream.
 *
 * <p>Every audio packet declares one frame of 20 milliseconds, i.e. a granule of 960 samples at
 * 48000 Hz.
 */
inline std::vector<uint8_t> buildOggOpus(const int channels, const int audioPacketCount,
                                         const int packetSize = 32) {
  constexpr uint32_t kSerialNumber = 0x4F505553;  // "OPUS"
  constexpr int kOpusSampleRate = 48000;
  constexpr int kPacketDurationUs = 20000;
  constexpr int kPacketGranule = kOpusSampleRate * kPacketDurationUs / 1000000;  // 960 samples.
  // The table of contents byte declares the configuration 13, i.e. a frame of 20 milliseconds, two
  // channels and a single frame.
  constexpr uint8_t kTocByte = static_cast<uint8_t>((13 << 3) | (1 << 2));

  std::vector<uint8_t> identificationHeader;
  putAscii(identificationHeader, "OpusHead");
  identificationHeader.push_back(1);  // The version of the stream.
  identificationHeader.push_back(static_cast<uint8_t>(channels));
  putLittleEndianUnsignedShort(identificationHeader, 312);  // The pre-skip.
  putLittleEndianUnsignedInt(identificationHeader, kOpusSampleRate);
  putLittleEndianUnsignedShort(identificationHeader, 0);  // The output gain.
  identificationHeader.push_back(0);                      // The mapping family.

  std::vector<uint8_t> commentHeader;
  putAscii(commentHeader, "OpusTags");
  putLittleEndianUnsignedInt(commentHeader, 5);
  putAscii(commentHeader, "coral");
  putLittleEndianUnsignedInt(commentHeader, 0);  // The number of comments.

  std::vector<uint8_t> out;
  appendOggPage(out, 0x02, 0, kSerialNumber, 0, {identificationHeader});
  appendOggPage(out, 0x00, 0, kSerialNumber, 1, {commentHeader});
  for (int i = 0; i < audioPacketCount; ++i) {
    std::vector<uint8_t> packet(static_cast<size_t>(packetSize), 0);
    packet[0] = kTocByte;
    const bool lastPacket = i == audioPacketCount - 1;
    appendOggPage(out, lastPacket ? 0x04 : 0x00,
                  static_cast<int64_t>(i + 1) * kPacketGranule, kSerialNumber,
                  static_cast<uint32_t>(2 + i), {packet});
  }
  return out;
}

/**
 * Builds an Ogg container that carries a FLAC stream, i.e. the Ogg mapping of the codec.
 *
 * <p>Every audio packet is a FLAC frame whose block size key declares 4096 samples, which is what
 * the granularity of the timestamps of the stream is.
 */
inline std::vector<uint8_t> buildOggFlac(const int sampleRate, const int channels,
                                         const int bitsPerSample, const int audioPacketCount,
                                         const int frameSize = 64) {
  constexpr uint32_t kSerialNumber = 0x464C4143;  // "FLAC"
  constexpr int kBlockSize = 4096;

  std::vector<uint8_t> streamInfo(34, 0);
  streamInfo[0] = static_cast<uint8_t>((kBlockSize >> 8) & 0xFF);
  streamInfo[1] = static_cast<uint8_t>(kBlockSize & 0xFF);
  streamInfo[2] = streamInfo[0];
  streamInfo[3] = streamInfo[1];
  const uint64_t packed = (static_cast<uint64_t>(sampleRate & 0xFFFFF) << 44) |
                          (static_cast<uint64_t>((channels - 1) & 0x7) << 41) |
                          (static_cast<uint64_t>((bitsPerSample - 1) & 0x1F) << 36) |
                          static_cast<uint64_t>(audioPacketCount) * kBlockSize;
  for (int i = 0; i < 8; ++i) {
    streamInfo[10 + i] = static_cast<uint8_t>((packed >> (56 - i * 8)) & 0xFF);
  }

  std::vector<uint8_t> mapping;
  mapping.push_back(0x7F);
  putAscii(mapping, "FLAC");
  mapping.push_back(1);  // The major version of the mapping.
  mapping.push_back(0);  // The minor version of the mapping.
  putUnsignedShort(mapping, 1);  // The number of header packets.
  putAscii(mapping, "fLaC");
  mapping.push_back(0x80);  // The last metadata block, of the type STREAMINFO.
  mapping.push_back(0);
  mapping.push_back(0);
  mapping.push_back(34);
  mapping.insert(mapping.end(), streamInfo.begin(), streamInfo.end());

  std::vector<uint8_t> out;
  appendOggPage(out, 0x02, 0, kSerialNumber, 0, {mapping});
  for (int i = 0; i < audioPacketCount; ++i) {
    std::vector<uint8_t> packet(static_cast<size_t>(frameSize), 0);
    packet[0] = 0xFF;
    packet[1] = 0xF8;  // The sync code, without a reserved bit, with a fixed block size.
    packet[2] = static_cast<uint8_t>((12 << 4) | 9);  // 4096 samples, a sample rate of 44100 Hz.
    packet[3] = 0x00;
    const bool lastPacket = i == audioPacketCount - 1;
    appendOggPage(out, lastPacket ? 0x04 : 0x00,
                  static_cast<int64_t>(i + 1) * kBlockSize, kSerialNumber,
                  static_cast<uint32_t>(1 + i), {packet});
  }
  return out;
}

// ==================================================================================================
// AVI
// ==================================================================================================

/**
 * Builds one chunk of a RIFF file, its padding included.
 */
inline std::vector<uint8_t> buildAviChunk(const char* type, const std::vector<uint8_t>& body) {
  std::vector<uint8_t> out;
  putFourCc(out, type);
  putLittleEndianUnsignedInt(out, static_cast<uint32_t>(body.size()));
  out.insert(out.end(), body.begin(), body.end());
  if (body.size() % 2 != 0) {
    out.push_back(0);
  }
  return out;
}

/**
 * Builds one LIST chunk of a RIFF file, whose body is prefixed by its type.
 */
inline std::vector<uint8_t> buildAviList(const char* type, const std::vector<uint8_t>& body) {
  std::vector<uint8_t> content;
  putFourCc(content, type);
  content.insert(content.end(), body.begin(), body.end());
  return buildAviChunk("LIST", content);
}

/**
 * Builds an {@code AVIMAINHEADER} structure, i.e. the body of an {@code avih} chunk.
 */
inline std::vector<uint8_t> buildAviMainHeader(const int frameDurationUs, const int totalFrames,
                                               const int streamCount) {
  std::vector<uint8_t> out;
  putLittleEndianUnsignedInt(out, static_cast<uint32_t>(frameDurationUs));
  putLittleEndianUnsignedInt(out, 0);  // The maximum bytes per second.
  putLittleEndianUnsignedInt(out, 0);  // The padding granularity.
  putLittleEndianUnsignedInt(out, 0x10);  // The flags, i.e. the index flag.
  putLittleEndianUnsignedInt(out, static_cast<uint32_t>(totalFrames));
  putLittleEndianUnsignedInt(out, 0);  // The number of initial frames.
  putLittleEndianUnsignedInt(out, static_cast<uint32_t>(streamCount));
  putLittleEndianUnsignedInt(out, 0);  // The suggested buffer size.
  putLittleEndianUnsignedInt(out, 0);  // The width of the movie.
  putLittleEndianUnsignedInt(out, 0);  // The height of the movie.
  for (int i = 0; i < 4; ++i) {
    putLittleEndianUnsignedInt(out, 0);  // The reserved values.
  }
  return out;
}

/**
 * Builds an {@code AVISTREAMHEADER} structure, i.e. the body of a {@code strh} chunk.
 *
 * @param type The four character code of the type of the stream, e.g. {@code auds}.
 * @param handler The four character code of the codec of the stream.
 * @param scale The number of units of a chunk.
 * @param rate The number of units per second.
 * @param length The number of chunks of the stream.
 * @param sampleSize The size of a sample, or 0 when the chunks have no fixed size.
 */
inline std::vector<uint8_t> buildAviStreamHeader(const char* type, const char* handler,
                                                 const int scale, const int rate, const int length,
                                                 const int suggestedBufferSize,
                                                 const int sampleSize) {
  std::vector<uint8_t> out;
  putFourCc(out, type);
  putFourCc(out, handler);
  putLittleEndianUnsignedInt(out, 0);  // The flags of the stream.
  putLittleEndianUnsignedShort(out, 0);  // The priority.
  putLittleEndianUnsignedShort(out, 0);  // The language.
  putLittleEndianUnsignedInt(out, 0);    // The number of initial frames.
  putLittleEndianUnsignedInt(out, static_cast<uint32_t>(scale));
  putLittleEndianUnsignedInt(out, static_cast<uint32_t>(rate));
  putLittleEndianUnsignedInt(out, 0);  // The start time.
  putLittleEndianUnsignedInt(out, static_cast<uint32_t>(length));
  putLittleEndianUnsignedInt(out, static_cast<uint32_t>(suggestedBufferSize));
  putLittleEndianUnsignedInt(out, 0);  // The quality.
  putLittleEndianUnsignedInt(out, static_cast<uint32_t>(sampleSize));
  for (int i = 0; i < 4; ++i) {
    putLittleEndianUnsignedShort(out, 0);  // The frame rectangle.
  }
  return out;
}

/**
 * Builds a {@code WAVEFORMATEX} structure, i.e. the body of the {@code strf} chunk of an audio
 * stream.
 */
inline std::vector<uint8_t> buildAviWaveFormat(const int formatTag, const int sampleRate,
                                               const int channels, const int bitsPerSample) {
  const int blockAlign = channels * bitsPerSample / 8;
  std::vector<uint8_t> out;
  putLittleEndianUnsignedShort(out, static_cast<uint16_t>(formatTag));
  putLittleEndianUnsignedShort(out, static_cast<uint16_t>(channels));
  putLittleEndianUnsignedInt(out, static_cast<uint32_t>(sampleRate));
  putLittleEndianUnsignedInt(out, static_cast<uint32_t>(sampleRate * blockAlign));
  putLittleEndianUnsignedShort(out, static_cast<uint16_t>(blockAlign));
  putLittleEndianUnsignedShort(out, static_cast<uint16_t>(bitsPerSample));
  return out;
}

/**
 * Builds a {@code BITMAPINFOHEADER} structure, i.e. the body of the {@code strf} chunk of a video
 * stream.
 */
inline std::vector<uint8_t> buildAviBitmapInfoHeader(const char* compression, const int width,
                                                     const int height) {
  std::vector<uint8_t> out;
  putLittleEndianUnsignedInt(out, 40);  // The size of the structure.
  putLittleEndianUnsignedInt(out, static_cast<uint32_t>(width));
  putLittleEndianUnsignedInt(out, static_cast<uint32_t>(height));
  putLittleEndianUnsignedShort(out, 1);   // The number of planes.
  putLittleEndianUnsignedShort(out, 24);  // The number of bits per pixel.
  putFourCc(out, compression);
  putLittleEndianUnsignedInt(out, 0);  // The size of the image.
  putLittleEndianUnsignedInt(out, 0);  // The horizontal resolution.
  putLittleEndianUnsignedInt(out, 0);  // The vertical resolution.
  putLittleEndianUnsignedInt(out, 0);  // The number of colours.
  putLittleEndianUnsignedInt(out, 0);  // The number of important colours.
  return out;
}

/**
 * Assembles a RIFF file from the bodies of its {@code hdrl} and {@code movi} lists.
 */
inline std::vector<uint8_t> buildAviFile(const std::vector<uint8_t>& hdrlBody,
                                         const std::vector<uint8_t>& moviBody) {
  const std::vector<uint8_t> hdrl = buildAviList("hdrl", hdrlBody);
  const std::vector<uint8_t> movi = buildAviList("movi", moviBody);
  std::vector<uint8_t> file;
  putFourCc(file, "RIFF");
  putLittleEndianUnsignedInt(file, static_cast<uint32_t>(4 + hdrl.size() + movi.size()));
  putFourCc(file, "AVI ");
  file.insert(file.end(), hdrl.begin(), hdrl.end());
  file.insert(file.end(), movi.begin(), movi.end());
  return file;
}

/**
 * Builds an AVI file holding a single PCM audio stream.
 *
 * <p>Every chunk of the stream holds the same number of samples, therefore the timestamps of the
 * samples are evenly spaced.
 *
 * @param sampleRate The sample rate of the stream.
 * @param channels The number of channels.
 * @param chunkCount The number of chunks, i.e. of samples, of the stream.
 * @param samplesPerChunk The number of frames of a chunk.
 * @param formatTag The format tag of the stream, which a test uses to build an unsupported stream.
 */
inline std::vector<uint8_t> buildAviPcmAudio(const int sampleRate, const int channels,
                                             const int chunkCount, const int samplesPerChunk,
                                             const int formatTag = 1) {
  constexpr int kBitsPerSample = 16;
  const int blockAlign = channels * kBitsPerSample / 8;
  const int chunkSize = samplesPerChunk * blockAlign;

  std::vector<uint8_t> stream;
  const std::vector<uint8_t> header =
    buildAviChunk("strh", buildAviStreamHeader("auds", "\0\0\0\0", samplesPerChunk, sampleRate,
                                               chunkCount, chunkSize, blockAlign));
  stream.insert(stream.end(), header.begin(), header.end());
  const std::vector<uint8_t> format =
    buildAviChunk("strf", buildAviWaveFormat(formatTag, sampleRate, channels, kBitsPerSample));
  stream.insert(stream.end(), format.begin(), format.end());

  std::vector<uint8_t> hdrlBody =
    buildAviChunk("avih", buildAviMainHeader(samplesPerChunk * 1000000 / sampleRate, chunkCount, 1));
  const std::vector<uint8_t> streamList = buildAviList("strl", stream);
  hdrlBody.insert(hdrlBody.end(), streamList.begin(), streamList.end());

  std::vector<uint8_t> moviBody;
  for (int i = 0; i < chunkCount; ++i) {
    std::vector<uint8_t> payload(static_cast<size_t>(chunkSize));
    for (size_t j = 0; j < payload.size(); ++j) {
      payload[j] = static_cast<uint8_t>(i + j);
    }
    const std::vector<uint8_t> chunk = buildAviChunk("00wb", payload);
    moviBody.insert(moviBody.end(), chunk.begin(), chunk.end());
  }
  return buildAviFile(hdrlBody, moviBody);
}

/**
 * Builds an AVI file holding one video stream and one PCM audio stream, whose chunks are
 * interleaved.
 *
 * <p>The first frame of the video stream is framed as an uncompressed frame, i.e. as a key frame,
 * and the frames that follow are framed as compressed ones.
 *
 * @param compression The four character code of the codec of the video stream.
 * @param frameCount The number of frames of the video stream.
 * @param frameSize The size of the payload of a frame.
 */
inline std::vector<uint8_t> buildAviVideoAndAudio(const char* compression, const int frameCount,
                                                  const int frameSize) {
  constexpr int kSampleRate = 8000;
  constexpr int kChannels = 1;
  constexpr int kBitsPerSample = 16;
  constexpr int kSamplesPerChunk = 64;
  constexpr int kAudioChunkCount = 8;
  constexpr int kWidth = 320;
  constexpr int kHeight = 240;
  constexpr int kFrameRate = 25;
  const int blockAlign = kChannels * kBitsPerSample / 8;
  const int audioChunkSize = kSamplesPerChunk * blockAlign;

  std::vector<uint8_t> videoStream;
  const std::vector<uint8_t> videoHeader =
    buildAviChunk("strh", buildAviStreamHeader("vids", compression, 1, kFrameRate, frameCount,
                                               frameSize, 0));
  videoStream.insert(videoStream.end(), videoHeader.begin(), videoHeader.end());
  const std::vector<uint8_t> videoFormat =
    buildAviChunk("strf", buildAviBitmapInfoHeader(compression, kWidth, kHeight));
  videoStream.insert(videoStream.end(), videoFormat.begin(), videoFormat.end());

  std::vector<uint8_t> audioStream;
  const std::vector<uint8_t> audioHeader =
    buildAviChunk("strh", buildAviStreamHeader("auds", "\0\0\0\0", kSamplesPerChunk, kSampleRate,
                                               kAudioChunkCount, audioChunkSize, blockAlign));
  audioStream.insert(audioStream.end(), audioHeader.begin(), audioHeader.end());
  const std::vector<uint8_t> audioFormat =
    buildAviChunk("strf", buildAviWaveFormat(1, kSampleRate, kChannels, kBitsPerSample));
  audioStream.insert(audioStream.end(), audioFormat.begin(), audioFormat.end());

  std::vector<uint8_t> hdrlBody =
    buildAviChunk("avih", buildAviMainHeader(1000000 / kFrameRate, frameCount, 2));
  const std::vector<uint8_t> videoList = buildAviList("strl", videoStream);
  hdrlBody.insert(hdrlBody.end(), videoList.begin(), videoList.end());
  const std::vector<uint8_t> audioList = buildAviList("strl", audioStream);
  hdrlBody.insert(hdrlBody.end(), audioList.begin(), audioList.end());

  std::vector<uint8_t> moviBody;
  const int chunkCount = std::max(frameCount, kAudioChunkCount);
  for (int i = 0; i < chunkCount; ++i) {
    if (i < frameCount) {
      std::vector<uint8_t> frame(static_cast<size_t>(frameSize), static_cast<uint8_t>(i));
      // The identifier of an uncompressed frame is what marks a key frame in a file that carries no
      // index.
      const std::vector<uint8_t> chunk = buildAviChunk(i == 0 ? "00db" : "00dc", frame);
      moviBody.insert(moviBody.end(), chunk.begin(), chunk.end());
    }
    if (i < kAudioChunkCount) {
      std::vector<uint8_t> payload(static_cast<size_t>(audioChunkSize),
                                   static_cast<uint8_t>(i));
      // The identifier of a chunk starts with the two digits of the identifier of its stream, which
      // is 1 for the audio stream of this fixture.
      const std::vector<uint8_t> audioChunk = buildAviChunk("01wb", payload);
      moviBody.insert(moviBody.end(), audioChunk.begin(), audioChunk.end());
    }
  }
  return buildAviFile(hdrlBody, moviBody);
}

} // namespace test
} // namespace coral

#endif //CORAL_EXTRACTOR_TEST_MEDIA_H
