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

#include "SampleTable.h"

#include "Mp4Boxes.h"

#include <algorithm>
#include <limits>

namespace coral {
namespace muxer {
namespace mp4 {
namespace {

constexpr uint32_t kSampleFlagKeyFrame = 1u << 0;

} // namespace

SampleTable::SampleTable(const int32_t timescale)
        : mTimescale(timescale > 0 ? timescale : 90000) {}

void SampleTable::setTimescale(const int32_t timescale) {
  if (timescale > 0) {
    mTimescale = timescale;
  }
}

void SampleTable::addSample(const uint32_t size, const int64_t presentationTimeUs, const int64_t durationUs,
        const uint32_t flags) {
  mSamples.push_back({ size, presentationTimeUs, durationUs, flags });
}

void SampleTable::addChunk(const int64_t offset) { mChunks.push_back({ offset, 1 }); }

void SampleTable::incrementLastChunkSampleCount() {
  if (!mChunks.empty()) {
    ++mChunks.back().sampleCount;
  }
}

void SampleTable::finalize(const int64_t defaultDurationUs, const int64_t endPresentationTimeUs) {
  if (mSamples.empty()) {
    return;
  }

  std::vector<int64_t> presentationTimes;
  presentationTimes.reserve(mSamples.size());
  for (const auto& sample: mSamples) {
    presentationTimes.push_back(sample.presentationTimeUs);
  }
  std::sort(presentationTimes.begin(), presentationTimes.end());
  presentationTimes.erase(std::unique(presentationTimes.begin(), presentationTimes.end()), presentationTimes.end());

  int64_t lastKnownDurationUs = 0;
  for (auto& sample: mSamples) {
    if (sample.durationUs > 0) {
      lastKnownDurationUs = sample.durationUs;
      continue;
    }

    const auto next = std::upper_bound(presentationTimes.begin(), presentationTimes.end(), sample.presentationTimeUs);
    if (next != presentationTimes.end()) {
      sample.durationUs = *next - sample.presentationTimeUs;
    } else if (endPresentationTimeUs != kUnsetPresentationTime && endPresentationTimeUs > sample.presentationTimeUs) {
      sample.durationUs = endPresentationTimeUs - sample.presentationTimeUs;
    } else if (lastKnownDurationUs > 0) {
      sample.durationUs = lastKnownDurationUs;
    } else if (defaultDurationUs > 0) {
      sample.durationUs = defaultDurationUs;
    } else {
      sample.durationUs = 0;
    }

    if (sample.durationUs > 0) {
      lastKnownDurationUs = sample.durationUs;
    }
  }
}

int64_t SampleTable::firstPresentationTimeUs() const {
  return mSamples.empty() ? 0 : mSamples.front().presentationTimeUs;
}

int64_t SampleTable::minimumPresentationTimeUs() const {
  if (mSamples.empty()) {
    return 0;
  }
  int64_t minimum = mSamples.front().presentationTimeUs;
  for (const auto& sample: mSamples) {
    minimum = std::min(minimum, sample.presentationTimeUs);
  }
  return minimum;
}

int64_t SampleTable::maximumPresentationTimeUs() const {
  if (mSamples.empty()) {
    return 0;
  }
  int64_t maximum = mSamples.front().presentationTimeUs;
  for (const auto& sample: mSamples) {
    maximum = std::max(maximum, sample.presentationTimeUs);
  }
  return maximum;
}

int64_t SampleTable::scaleUs(const int64_t timeUs) const { return scaleUsToTicks(timeUs, mTimescale); }

int64_t SampleTable::sampleDurationUs(const size_t index) const {
  return index < mSamples.size() ? mSamples[index].durationUs : 0;
}

int64_t SampleTable::sampleDurationTicks(const size_t index) const {
  const int64_t ticks = scaleUs(sampleDurationUs(index));
  return std::max<int64_t>(1, ticks);
}

int64_t SampleTable::durationTicks() const {
  int64_t duration = 0;
  for (size_t i = 0; i < mSamples.size(); ++i) {
    duration += sampleDurationTicks(i);
  }
  return duration;
}

int64_t SampleTable::durationUs() const {
  int64_t duration = 0;
  for (const auto& sample: mSamples) {
    duration += std::max<int64_t>(0, sample.durationUs);
  }
  return duration;
}

bool SampleTable::allKeyFrames() const {
  return std::all_of(mSamples.begin(), mSamples.end(),
          [](const SampleRecord& sample) { return (sample.flags & static_cast<uint32_t>(kSampleFlagKeyFrame)) != 0; });
}

std::vector<uint32_t> SampleTable::syncSampleNumbers() const {
  std::vector<uint32_t> keyFrames;
  for (size_t i = 0; i < mSamples.size(); ++i) {
    if ((mSamples[i].flags & static_cast<uint32_t>(kSampleFlagKeyFrame)) != 0) {
      keyFrames.push_back(static_cast<uint32_t>(i + 1));
    }
  }
  return keyFrames;
}

bool SampleTable::hasCompositionOffsets() const {
  if (mSamples.empty()) {
    return false;
  }
  const int64_t basePts = firstPresentationTimeUs();
  int64_t decodeTimeTicks = 0;
  for (size_t i = 0; i < mSamples.size(); ++i) {
    const int64_t compositionTicks = scaleUs(mSamples[i].presentationTimeUs - basePts);
    if (compositionTicks != decodeTimeTicks) {
      return true;
    }
    decodeTimeTicks += sampleDurationTicks(i);
  }
  return false;
}

std::vector<int32_t> SampleTable::compositionOffsetsTicks() const {
  std::vector<int32_t> offsets;
  if (mSamples.empty()) {
    return offsets;
  }

  const int64_t basePts = firstPresentationTimeUs();
  int64_t decodeTimeTicks = 0;
  offsets.reserve(mSamples.size());
  for (size_t i = 0; i < mSamples.size(); ++i) {
    const int64_t compositionTicks = scaleUs(mSamples[i].presentationTimeUs - basePts);
    const int64_t offset = compositionTicks - decodeTimeTicks;
    offsets.push_back(static_cast<int32_t>(
            std::clamp<int64_t>(offset, std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max())));
    decodeTimeTicks += sampleDurationTicks(i);
  }
  return offsets;
}

void SampleTable::shiftChunkOffsets(const int64_t delta) {
  for (auto& chunk: mChunks) {
    chunk.offset += delta;
  }
}

void SampleTable::shiftPresentationTimes(const int64_t delta) {
  for (auto& sample: mSamples) {
    sample.presentationTimeUs += delta;
  }
}

bool SampleTable::needs64BitChunkOffsets() const {
  return std::any_of(mChunks.begin(), mChunks.end(),
          [](const ChunkRecord& chunk) { return chunk.offset > std::numeric_limits<uint32_t>::max(); });
}

void SampleTable::writeStts(BoxWriter& writer) const {
  struct Entry {
    uint32_t count;
    uint32_t duration;
  };
  std::vector<Entry> entries;
  entries.reserve(mSamples.size());
  for (size_t i = 0; i < mSamples.size(); ++i) {
    const auto duration = static_cast<uint32_t>(std::max<int64_t>(1, sampleDurationTicks(i)));
    if (!entries.empty() && entries.back().duration == duration) {
      ++entries.back().count;
    } else {
      entries.push_back({ 1, duration });
    }
  }

  const size_t stts = writer.beginFullBox("stts", 0, 0);
  writer.u32(static_cast<uint32_t>(entries.size()));
  for (const auto& entry: entries) {
    writer.u32(entry.count);
    writer.u32(entry.duration);
  }
  writer.endBox(stts);
}

void SampleTable::writeCtts(BoxWriter& writer) const {
  const std::vector<int32_t> offsets = compositionOffsetsTicks();
  struct Entry {
    uint32_t count;
    int32_t offset;
  };
  std::vector<Entry> entries;
  entries.reserve(offsets.size());
  for (const auto offset: offsets) {
    if (!entries.empty() && entries.back().offset == offset) {
      ++entries.back().count;
    } else {
      entries.push_back({ 1, offset });
    }
  }

  const size_t ctts = writer.beginFullBox("ctts", 1, 0);
  writer.u32(static_cast<uint32_t>(entries.size()));
  for (const auto& entry: entries) {
    writer.u32(entry.count);
    writer.i32(entry.offset);
  }
  writer.endBox(ctts);
}

void SampleTable::writeStss(BoxWriter& writer) const {
  const std::vector<uint32_t> keyFrames = syncSampleNumbers();
  const size_t stss = writer.beginFullBox("stss", 0, 0);
  writer.u32(static_cast<uint32_t>(keyFrames.size()));
  for (const auto sample: keyFrames) {
    writer.u32(sample);
  }
  writer.endBox(stss);
}

void SampleTable::writeStsz(BoxWriter& writer) const {
  const size_t stsz = writer.beginFullBox("stsz", 0, 0);
  writer.u32(0);
  writer.u32(static_cast<uint32_t>(mSamples.size()));
  for (const auto& sample: mSamples) {
    writer.u32(sample.size);
  }
  writer.endBox(stsz);
}

void SampleTable::writeStsc(BoxWriter& writer) const {
  const size_t stsc = writer.beginFullBox("stsc", 0, 0);
  uint32_t entryCount = 0;
  uint32_t previousSampleCount = 0;
  for (size_t i = 0; i < mChunks.size(); ++i) {
    if (i == 0 || mChunks[i].sampleCount != previousSampleCount) {
      ++entryCount;
      previousSampleCount = mChunks[i].sampleCount;
    }
  }
  writer.u32(entryCount);
  previousSampleCount = 0;
  for (size_t i = 0; i < mChunks.size(); ++i) {
    if (i == 0 || mChunks[i].sampleCount != previousSampleCount) {
      writer.u32(static_cast<uint32_t>(i + 1));
      writer.u32(mChunks[i].sampleCount);
      writer.u32(1);
      previousSampleCount = mChunks[i].sampleCount;
    }
  }
  writer.endBox(stsc);
}

void SampleTable::writeChunkOffsets(BoxWriter& writer, const bool force64Bit) const {
  if (force64Bit || needs64BitChunkOffsets()) {
    const size_t co64 = writer.beginFullBox("co64", 0, 0);
    writer.u32(static_cast<uint32_t>(mChunks.size()));
    for (const auto& chunk: mChunks) {
      writer.u64(clampToU64(chunk.offset));
    }
    writer.endBox(co64);
    return;
  }

  const size_t stco = writer.beginFullBox("stco", 0, 0);
  writer.u32(static_cast<uint32_t>(mChunks.size()));
  for (const auto& chunk: mChunks) {
    writer.u32(clampToU32(chunk.offset));
  }
  writer.endBox(stco);
}

} // namespace mp4
} // namespace muxer
} // namespace coral
