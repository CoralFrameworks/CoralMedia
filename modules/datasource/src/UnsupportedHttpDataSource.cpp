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

/**
 * @file UnsupportedHttpDataSource.cpp
 *
 * The http data source of the platforms that do not implement one, i.e. every platform but Windows
 * and Apple.
 *
 * <p>It exists so that the interface of the module, and the factory an application creates the data
 * source with, are the same on every platform: the factory of a platform that does not implement one
 * creates no data source, and every http uri therefore fails to open, which a caller reports as a
 * load error rather than reading a path of the local file system.
 */

#include <datasource/HttpDataSource.h>

#include <memory>
#include <utility>

#include <minilog/log.h>

namespace coral {

namespace {

/** The factory of a platform that does not implement an http data source. */
class UnsupportedHttpDataSourceFactory final : public AVHttpDataSourceFactory {
public:
  [[nodiscard]] std::shared_ptr<AVHttpDataSource>
  create(const AVHttpDataSource::Parameters parameters) override {
    static_cast<void>(parameters);
    MINILOG_ERROR("the http data source is not implemented on this platform");
    return nullptr;
  }
};

} // namespace anonymous

bool isHttpDataSourceSupported() { return false; }

std::unique_ptr<AVHttpDataSourceFactory> createHttpDataSourceFactory() {
  return std::make_unique<UnsupportedHttpDataSourceFactory>();
}

} // namespace coral
