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

#include <extractor/text/SubtitleParser.h>

namespace coral {

AVSubtitleParser::OutputOptions AVSubtitleParser::OutputOptions::allCues() {
  return OutputOptions{};
}

AVSubtitleParser::OutputOptions
AVSubtitleParser::OutputOptions::onlyCuesAfter(const int64_t startTimeUs) {
  OutputOptions options;
  options.startTimeUs = startTimeUs;
  options.outputAllCues = false;
  return options;
}

AVSubtitleParser::OutputOptions
AVSubtitleParser::OutputOptions::cuesAfterThenRemainingCuesBefore(const int64_t startTimeUs) {
  OutputOptions options;
  options.startTimeUs = startTimeUs;
  options.outputAllCues = true;
  return options;
}

} // namespace coral
