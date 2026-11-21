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
 * @file test_HttpDataSource.cpp
 *
 * The tests of the http data source as the module exposes it, i.e. of the interface and of the factory
 * of the platform. The strategy of the reads is covered by test_HttpDataSourceBase.cpp, which binds it
 * to a server that is not the one of the platform.
 *
 * <p>The suite never opens a media that a server holds: a test that needs one belongs to the platform,
 * and the assertions here are the ones that hold without a network.
 */

#include <datasource/HttpDataSource.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>

using coral::AVHttpDataSource;
using coral::createHttpDataSourceFactory;
using coral::isHttpDataSourceSupported;

namespace {

/** Creates the data source of the platform, or returns nullptr where the platform has none. */
std::shared_ptr<AVHttpDataSource> createPlatformDataSource() {
  const auto factory = createHttpDataSourceFactory();
  if (factory == nullptr) {
    return nullptr;
  }
  return factory->create(AVHttpDataSource::Parameters());
}

} // namespace anonymous

TEST(AVHttpDataSourceTest, ReportsWhetherThePlatformImplementsIt) {
#if defined(_WIN32)
  EXPECT_TRUE(isHttpDataSourceSupported());
#else
  EXPECT_FALSE(isHttpDataSourceSupported());
#endif
}

TEST(AVHttpDataSourceTest, CreatesTheDataSourceOfThePlatformOrNone) {
  const auto factory = createHttpDataSourceFactory();
  ASSERT_NE(nullptr, factory);

  // A platform that does not implement the data source creates none, and it says so.
  const auto source = factory->create(AVHttpDataSource::Parameters());
  EXPECT_EQ(isHttpDataSourceSupported(), source != nullptr);
}

TEST(AVHttpDataSourceTest, ReceivesTheParametersOfTheRequests) {
  const auto factory = createHttpDataSourceFactory();
  ASSERT_NE(nullptr, factory);

  AVHttpDataSource::Parameters parameters;
  parameters.connectTimeoutMs = 1000;
  parameters.readTimeoutMs = 2000;
  parameters.requestProperties.emplace_back("User-Agent", "coral");
  // The factory of a platform that does not implement the data source accepts them all the same, so
  // that a caller only has to check the result and not the platform of its build.
  const auto source = factory->create(std::move(parameters));
  EXPECT_EQ(isHttpDataSourceSupported(), source != nullptr);
}

TEST(AVHttpDataSourceTest, FailsToOpenAUriThatIsNotAnHttpUrl) {
  const auto source = createPlatformDataSource();
  if (source == nullptr) {
    GTEST_SKIP() << "the platform does not implement the http data source";
  }

  EXPECT_FALSE(source->open("ftp://example.com/media.mkv"));
  EXPECT_FALSE(source->isOpen());
  EXPECT_EQ(-1, source->getLength());
  EXPECT_EQ(0, source->getResponseCode());
  EXPECT_TRUE(source->getUri().empty());

  // Neither does a path of the local file system: the scheme of a file is the one of AVFileDataSource.
  EXPECT_FALSE(source->open("media.mkv"));
  EXPECT_FALSE(source->isOpen());

  uint8_t buffer[8] = {};
  EXPECT_EQ(-1, source->readAt(0, buffer, 0, sizeof(buffer)));
  source->close();
  EXPECT_FALSE(source->isOpen());
}
