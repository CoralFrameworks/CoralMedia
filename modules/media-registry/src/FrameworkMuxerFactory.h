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

#ifndef CORAL_FRAMEWORK_MUXER_FACTORY_H
#define CORAL_FRAMEWORK_MUXER_FACTORY_H

#include <codec/MediaMuxer.h>

#include <memory>

namespace coral {

class FrameworkMuxerFactory final : public MediaMuxerFactory {
public:
  FrameworkMuxerFactory();

  ~FrameworkMuxerFactory() override;

  void setCustomFactory(std::unique_ptr<MediaMuxerFactory> factory);

  bool isSupportMuxerFormat(MuxerFormat muxerFormat) override;

  std::unique_ptr<AVMediaMuxer> createMuxer() override;

private:
  std::unique_ptr<MediaMuxerFactory> builtinFactory;
  std::unique_ptr<MediaMuxerFactory> customFactory;
};

} // namespace coral

#endif //CORAL_FRAMEWORK_MUXER_FACTORY_H
