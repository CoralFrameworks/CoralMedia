/*
 * Copyright (C) 2022 The Coral Project
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

#ifndef CORAL_MEDIA_PATH_UTILS_H
#define CORAL_MEDIA_PATH_UTILS_H

#include <string>
#include <vector>
#include <sys/stat.h>

#if defined(WIN32)

#include <windows.h>

// Copied from linux libc sys/stat.h:
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define PATH_MAX (MAX_PATH)

// For getcwd
#include <direct.h>
#define getcwd _getcwd

#define SEPARATOR '\\'
#define SEPARATOR_STR  "\\"
#else

#include <unistd.h>

#define SEPARATOR '/'
#define SEPARATOR_STR "/"
#endif

// ssize_t is a POSIX type.
#if defined(WIN32) || defined(_WIN32)
#include <basetsd.h>
typedef SSIZE_T ssize_t;
#endif

class PathUtils {
public:
  static bool exists(const char* filePath) {
#if defined(_WIN64) || defined(_M_X64)
    struct _stat64 file;
    return _stat64(filePath, &file) == 0;
#else
    struct stat file;
    return stat(filePath, &file) == 0;
#endif
  }

  static bool isFile(const char *filePath) {
    struct stat file;
    if (stat(filePath, &file) == 0) {
      return S_ISREG(file.st_mode);
    }
    return false;
  }

  static std::string getName(const std::string& filePath) {
    if (filePath.empty()) {
      return "";
    }

    std::vector<std::string> segments(split(filePath));
    return segments.back();
  }

  static std::vector<std::string> split(const std::string& filePath) {
    std::vector<std::string> segments;
    if (filePath.empty()) {
      return segments;
    }

    size_t current;
    ssize_t next = -1;

    // If there is a leading disk designator such as C:, this naturally becomes the first segment.
    // However if there there is leading slash or back slash, we need to explicitly preserve it.
    // Note that we are guaranteed to have at least one char in the path at this point.
    if (filePath[0] == '/' || filePath[0] == '\\') {
      segments.push_back(filePath.substr(0, 1));
      next = 0;
    }

    do {
      current = size_t(next + 1);
      next = filePath.find_first_of(SEPARATOR, current);

      std::string segment(filePath.substr(current, next - current));
      if (!segment.empty()) {
        segments.push_back(segment);
      }
    } while (next != std::string::npos);

    if (segments.empty()) {
      segments.push_back(filePath);
    }

    return segments;
  }

protected:
  PathUtils() = default;
  ~PathUtils() = default;
};

#endif //CORAL_MEDIA_PATH_UTILS_H