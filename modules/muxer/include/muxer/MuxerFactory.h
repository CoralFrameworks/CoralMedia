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

#ifndef CORAL_MUXER_MUXER_FACTORY_H
#define CORAL_MUXER_MUXER_FACTORY_H

#include "muxer/Muxer.h"

#include <memory>
#include <string>

namespace coral {
namespace muxer {

/** The entry point of the container implementations. */
class MuxerFactory {
public:
  virtual ~MuxerFactory() = default;

  [[nodiscard]] virtual bool supports(Container container) const noexcept = 0;

  /**
   * Opens @p url for writing.
   *
   * @param options Container options.
   * @param error   Set when the output cannot be opened or the container is not supported.
   */
  virtual std::unique_ptr<Muxer> create(const std::string& url, Container container,
    const MuxerOptions& options, Error* error) = 0;
};

/** Creates the built-in factory. New containers are added to this factory, not to callers. */
std::unique_ptr<MuxerFactory> createMuxerFactory();

} // namespace muxer
} // namespace coral

#endif // CORAL_MUXER_MUXER_FACTORY_H
