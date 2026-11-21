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

#include <datasource/AndroidDataSource.h>

#include <minilog/log.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <utility>

#include <unistd.h>

namespace coral {

AVFileDescriptorDataSource::AVFileDescriptorDataSource(const int fileDescriptor, const long offset,
                                                       const long length) noexcept
  : fileDescriptor_(fileDescriptor),
    offset_(offset),
    length_(length) {
}

bool AVFileDescriptorDataSource::open(const std::string& uri) {
  close();
  // A descriptor that is not a region of a file, e.g. the one of a pipe, cannot be read at a
  // position: the platform reports it as an invalid offset rather than as a missing file.
  if (fileDescriptor_ < 0 || offset_ < 0) {
    return false;
  }
  uri_ = uri;
  isOpen_ = true;
  return true;
}

int AVFileDescriptorDataSource::readAt(const long position, uint8_t* buffer, const size_t offset,
                                       const size_t length) {
  if (!isOpen_ || buffer == nullptr) {
    return -1;
  }
  if (position < 0 || length == 0) {
    return 0;
  }
  if (length_ >= 0 && position >= length_) {
    return 0;
  }
  size_t toRead = length;
  if (length_ >= 0 && static_cast<long>(toRead) > length_ - position) {
    toRead = static_cast<size_t>(length_ - position);
  }
  // The descriptor is read with pread rather than with a seek and a read, so that a data source
  // cannot move the position of a descriptor that is shared with its owner.
  const auto read = pread64(fileDescriptor_, buffer + offset, toRead,
                            static_cast<off64_t>(offset_) + position);
  if (read < 0) {
    if (errno == EINTR) {
      // The caller loops over the reads of the media, therefore an interrupted read is a short read
      // rather than a failure of the media.
      return 0;
    }
    MINILOG_ERROR("failed to read {} bytes at {} of {}", toRead, position, uri_);
    return -1;
  }
  return static_cast<int>(read);
}

void AVFileDescriptorDataSource::close() {
  // The descriptor belongs to the object that handed it out, which closes it; a data source that
  // closed it here would close the media of its owner.
  isOpen_ = false;
  uri_.clear();
}

} // namespace coral
