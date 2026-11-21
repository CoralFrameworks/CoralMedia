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

#include <extractor/text/webvtt/WebvttParser.h>

#include "text/SubtitleTextUtil.h"
#include "text/webvtt/WebvttCss.h"
#include "text/webvtt/WebvttCueParser.h"

#include <media/Definitions.h>

#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace coral {
namespace {

constexpr const char* kWebvttHeader = "WEBVTT";
constexpr const char* kCommentMarker = "NOTE";
constexpr const char* kStyleMarker = "STYLE";
constexpr const char* kRegionMarker = "REGION";

/** The layout that a WebVTT `REGION` block declares. */
struct WebvttRegion final {
  std::string id;
  bool widthSet = false;
  float width = 1.0f;
  int lines = 3;
  float regionAnchorX = 0.0f;
  float regionAnchorY = 1.0f;
  float viewportAnchorX = 0.0f;
  float viewportAnchorY = 1.0f;
  bool scrollUp = false;
};

void parseRegionSetting(const std::string_view setting, WebvttRegion& region) {
  const size_t colon = setting.find(':');
  if (colon == std::string_view::npos) {
    return;
  }
  const std::string_view name = setting.substr(0, colon);
  const std::string value = text::trim(setting.substr(colon + 1));
  if (name == "id") {
    region.id = std::string(value);
  } else if (name == "width") {
    float fraction = 0.0f;
    if (text::parsePercentage(value, fraction)) {
      region.width = fraction;
      region.widthSet = true;
    }
  } else if (name == "lines") {
    int64_t lines = 0;
    if (text::parseInteger(value, lines) && lines > 0) {
      region.lines = static_cast<int>(lines);
    }
  } else if (name == "regionanchor" || name == "viewportanchor") {
    const size_t comma = value.find(',');
    if (comma == std::string_view::npos) {
      return;
    }
    float x = 0.0f;
    float y = 0.0f;
    if (!text::parsePercentage(text::trim(value.substr(0, comma)), x) ||
        !text::parsePercentage(text::trim(value.substr(comma + 1)), y)) {
      return;
    }
    if (name == "regionanchor") {
      region.regionAnchorX = x;
      region.regionAnchorY = y;
    } else {
      region.viewportAnchorX = x;
      region.viewportAnchorY = y;
    }
  } else if (name == "scroll") {
    region.scrollUp = value == "up";
  }
}

void parseRegionBlock(text::Reader& reader, const std::string_view firstLine,
    std::map<std::string, WebvttRegion>& regions) {
  WebvttRegion region;
  parseRegionSetting(text::trim(firstLine), region);
  std::string line;
  while (reader.readLine(line)) {
    if (line.empty()) {
      break;
    }
    parseRegionSetting(text::trim(line), region);
  }
  if (!region.id.empty()) {
    regions[region.id] = region;
  }
}

std::string regionIdFromSettings(const std::string_view settings) {
  size_t position = 0;
  while (position < settings.size()) {
    while (position < settings.size() && (settings[position] == ' ' || settings[position] == '\t' ||
                                         settings[position] == '\f')) {
      ++position;
    }
    const size_t start = position;
    while (position < settings.size() && settings[position] != ' ' && settings[position] != '\t' &&
           settings[position] != '\f') {
      ++position;
    }
    const std::string_view setting = settings.substr(start, position - start);
    const size_t colon = setting.find(':');
    if (colon != std::string_view::npos && setting.substr(0, colon) == "region") {
      return std::string(setting.substr(colon + 1));
    }
  }
  return std::string();
}

AVCue::AnchorType anchorAt(const float fraction) {
  if (fraction <= 0.25f) {
    return AVCue::AnchorType::kStart;
  }
  if (fraction >= 0.75f) {
    return AVCue::AnchorType::kEnd;
  }
  return AVCue::AnchorType::kMiddle;
}

void applyRegion(const WebvttRegion& region, AVCue& cue) {
  cue.position = region.viewportAnchorX;
  cue.positionAnchor = anchorAt(region.regionAnchorX);
  cue.line = region.viewportAnchorY;
  cue.lineType = AVCue::LineType::kFraction;
  cue.lineAnchor = anchorAt(region.regionAnchorY);
  if (region.widthSet) {
    cue.size = region.width;
  }
  cue.regionLines = region.lines;
}

/** Reads lines until the next empty one, which ends a block. */
void skipBlock(text::Reader& reader) {
  std::string line;
  while (reader.readLine(line)) {
    if (line.empty()) {
      return;
    }
  }
}

} // namespace

bool AVWebvttParser::parse(const uint8_t* data, const size_t size, const OutputOptions& options,
                           const CueConsumer& output) {
  if (data == nullptr || size == 0 || output == nullptr) {
    return false;
  }
  text::Reader reader(data, size);
  std::string line;
  if (!reader.readLine(line) || line.rfind(kWebvttHeader, 0) != 0) {
    return false;
  }
  // The header line may carry a description; the rest of the header block ends at the first empty
  // line.
  while (reader.readLine(line)) {
    if (line.empty()) {
      break;
    }
  }

  std::vector<AVCuesWithTiming> results;
  std::vector<text::webvtt::CssStyle> cssStyles;
  std::map<std::string, WebvttRegion> regions;
  while (reader.readLine(line)) {
    if (line.empty()) {
      continue;
    }
    if (line.rfind(kCommentMarker, 0) == 0 &&
        (line.size() == 4 || line[4] == ' ' || line[4] == '\t')) {
      skipBlock(reader);
      continue;
    }
    if (line == kStyleMarker) {
      std::string block;
      std::string blockLine;
      while (reader.readLine(blockLine)) {
        if (blockLine.empty()) break;
        block += blockLine;
        block.push_back('\n');
      }
      for (auto& style: text::webvtt::parseCssStyles(block)) {
        cssStyles.push_back(std::move(style));
      }
      continue;
    }
    if (line == kRegionMarker || line.rfind("REGION ", 0) == 0 ||
        line.rfind("REGION\t", 0) == 0) {
      parseRegionBlock(reader, line.substr(6), regions);
      continue;
    }

    // A cue starts either with its timing line or with an identifier line followed by it.
    std::string headerLine = line;
    std::string cueId;
    std::string settings;
    int64_t startUs = 0;
    int64_t endUs = 0;
    if (!text::webvtt::parseCueHeaderLine(headerLine, startUs, endUs, settings)) {
      cueId = headerLine;
      if (!reader.readLine(headerLine) ||
          !text::webvtt::parseCueHeaderLine(headerLine, startUs, endUs, settings)) {
        // A block that is not a cue is skipped as a whole rather than failing the document.
        if (!headerLine.empty()) {
          skipBlock(reader);
        }
        continue;
      }
    }

    std::string textBuilder;
    std::string textLine;
    while (reader.readLine(textLine)) {
      if (textLine.empty()) {
        break;
      }
      if (!textBuilder.empty()) {
        textBuilder.push_back('\n');
      }
      textBuilder += text::trim(textLine);
    }

    AVCue cue;
    text::webvtt::applyCueDefaults(cue);
    const std::string regionId = regionIdFromSettings(settings);
    if (!regionId.empty()) {
      const auto region = regions.find(regionId);
      if (region != regions.end()) {
        applyRegion(region->second, cue);
      }
    }
    text::webvtt::applyCueSettings(settings, cue);
    text::webvtt::parseCueText(textBuilder, cue, cssStyles, cueId);
    text::webvtt::finalizeCue(cue);

    AVCuesWithTiming result;
    result.cues.push_back(std::move(cue));
    result.startTimeUs = startUs;
    result.durationUs = endUs - startUs;
    results.push_back(std::move(result));
  }

  text::emitInRequestedOrder(std::move(results), options.startTimeUs, options.outputAllCues, output);
  return true;
}

void AVWebvttParser::reset() {
  // The parser holds no state across samples: every call is a whole document.
}

} // namespace coral
