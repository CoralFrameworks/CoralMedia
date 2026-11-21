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

#ifndef CORAL_DATASOURCE_TEST_DATA_H
#define CORAL_DATASOURCE_TEST_DATA_H

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace coral {
namespace test {

/**
 * The content the data source tests read: 0x00 to 0xFF, then 0x00 to 0xFF again for the default size.
 */
inline std::vector<uint8_t> makePattern(const size_t size = 512) {
  std::vector<uint8_t> data(size);
  for (size_t i = 0; i < data.size(); ++i) {
    data[i] = static_cast<uint8_t>(i % 256);
  }
  return data;
}

/** Writes a file in the temporary directory of the platform and returns its path. */
inline std::string writeTemporaryFile(const std::string& name, const std::vector<uint8_t>& data) {
  const auto path = std::filesystem::temp_directory_path() / name;
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
  file.close();
  return path.string();
}

} // namespace test
} // namespace coral

#endif //CORAL_DATASOURCE_TEST_DATA_H
