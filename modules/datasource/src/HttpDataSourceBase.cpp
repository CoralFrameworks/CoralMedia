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

#include "HttpDataSourceBase.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <utility>

#include <minilog/log.h>

namespace coral {

namespace {

/** The largest number of bytes a single read asks the server for. */
constexpr size_t MAX_READ_SIZE = 256 * 1024;

/**
 * The number of bytes a request reads ahead. The extractor reads the media in small buffers that overlap
 * rather than follow each other, because it reads ahead of the position it parses: a source that opened a
 * request per read would ask the server for the same bytes again and again, and pay the latency of a
 * request every time. The window is wide enough that the requests of a media of the usual size are few,
 * because every request costs a connection to the server and its latency.
 */
constexpr size_t READ_AHEAD_SIZE = 1024 * 1024;

/**
 * Parses the total length of the media from a {@code Content-Range} header, i.e. the part that follows
 * its slash, or -1 when the header does not describe it.
 */
long parseContentRangeLength(const std::string& value) {
  const auto slash = value.find('/');
  if (slash == std::string::npos) {
    return -1;
  }
  const std::string total = value.substr(slash + 1);
  if (total.empty() || total == "*") {
    return -1;
  }
  try {
    const long length = std::stol(total);
    return length > 0 ? length : -1;
  } catch (const std::exception&) {
    return -1;
  }
}

/** Parses the length of a {@code Content-Length} header, or -1 when it does not describe one. */
long parseContentLength(const std::string& value) {
  if (value.empty()) {
    return -1;
  }
  try {
    const long length = std::stol(value);
    return length > 0 ? length : -1;
  } catch (const std::exception&) {
    return -1;
  }
}

} // namespace anonymous

HttpDataSourceBase::HttpDataSourceBase(Parameters parameters) noexcept
  : parameters_(std::move(parameters)) {
}

HttpDataSourceBase::~HttpDataSourceBase() { close(); }

int HttpDataSourceBase::getResponseCode() const {
  return connection_ != nullptr ? connection_->getResponseCode() : 0;
}

bool HttpDataSourceBase::open(const std::string& uri) {
  close();

  auto connection = createConnection(uri);
  if (connection == nullptr) {
    return false;
  }
  connection_ = std::move(connection);
  uri_ = uri;
  // The first request reads the media from its beginning and reports its length.
  if (!openRequest(0)) {
    close();
    return false;
  }
  return true;
}

bool HttpDataSourceBase::openRequest(const long position) {
  if (connection_ == nullptr) {
    return false;
  }
  closeRequest();

  // The media is read from the requested position on: the length of the media that is left, and the
  // length of the whole media when the response reports it, are read from the response.
  // The request reads the window that starts at the requested position, and nothing more, so that the
  // response can be read to its end.
  std::string headers = "Range: bytes=" + std::to_string(position) + "-" +
                        std::to_string(position + static_cast<long>(READ_AHEAD_SIZE) - 1);
  headers.append("\r\nAccept-Encoding: identity");
  for (const auto& property : parameters_.requestProperties) {
    headers.append("\r\n");
    headers.append(property.first);
    headers.append(": ");
    headers.append(property.second);
  }

  if (!connection_->openRequest(position, headers)) {
    return false;
  }

  const int statusCode = connection_->getResponseCode();
  // A server that ignores the range request answers with the whole media, which is only usable when it
  // was asked from its beginning.
  if (statusCode != 200 && statusCode != 206) {
    MINILOG_ERROR("the server answered {} for {}", statusCode, uri_);
    closeRequest();
    return false;
  }
  if (statusCode == 200 && position != 0) {
    MINILOG_ERROR("the server of {} does not read the media from a position", uri_);
    closeRequest();
    return false;
  }

  const long total = statusCode == 206
                       ? parseContentRangeLength(connection_->getResponseHeader("Content-Range"))
                       : parseContentLength(connection_->getResponseHeader("Content-Length"));
  if (total > 0) {
    length_ = total;
  }
  requestOpen_ = true;
  requestPosition_ = position;
  return true;
}

int HttpDataSourceBase::readAt(const long position, uint8_t* buffer, const size_t offset,
                               const size_t length) {
  if (!isOpen() || buffer == nullptr) {
    return -1;
  }
  if (position < 0) {
    return -1;
  }
  if (length == 0 || (length_ >= 0 && position >= length_)) {
    return 0;
  }
  // The bytes the caller asks for are served from the window the request read ahead, and a position the
  // window does not hold, e.g. the target of a seek, is read by a request that starts there and fills the
  // window again. The window is what keeps a read that overlaps the previous one, which is how the
  // extractor reads, from asking the server for the same bytes again.
  const long windowEnd = windowStart_ + static_cast<long>(windowLength_);
  const bool windowHoldsPosition = windowLength_ > 0 && position >= windowStart_ && position < windowEnd;
  if (!windowHoldsPosition) {
    // A request that is still open serves the position when it starts there and none of it was read
    // yet: that is the request {@link open} made to learn the length of the media, which a read that
    // starts where the open did would otherwise give up, and ask the server again, for nothing.
    const bool requestServesPosition =
      requestOpen_ && windowLength_ == 0 && position == requestPosition_;
    if (!requestServesPosition && !openRequest(position)) {
      return -1;
    }
    if (!fillWindow(position)) {
      return -1;
    }
  }
  const auto windowOffset = static_cast<size_t>(position - windowStart_);
  const size_t available = windowLength_ - windowOffset;
  const size_t toCopy = std::min(available, length);
  std::memcpy(buffer + offset, window_.data() + windowOffset, toCopy);
  return static_cast<int>(toCopy);
}

bool HttpDataSourceBase::fillWindow(const long position) {
  windowStart_ = position;
  windowLength_ = 0;
  window_.resize(READ_AHEAD_SIZE);

  long readPosition = position;
  while (windowLength_ < window_.size()) {
    const size_t remaining = window_.size() - windowLength_;
    const long read = connection_->read(window_.data() + windowLength_,
      std::min(remaining, MAX_READ_SIZE));
    if (read < 0) {
      if (windowLength_ == 0) {
        MINILOG_ERROR("failed to read the media of {} at {}", uri_, readPosition);
        closeRequest();
        return false;
      }
      // The bytes that were read are served, and the failure is reported by the read that reaches it.
      break;
    }
    if (read == 0) {
      // The response is exhausted, i.e. the request read the media to its end.
      break;
    }
    windowLength_ += static_cast<size_t>(read);
    readPosition += read;
  }
  return windowLength_ > 0;
}

void HttpDataSourceBase::closeRequest() {
  if (connection_ != nullptr) {
    connection_->closeRequest();
  }
  requestOpen_ = false;
}

void HttpDataSourceBase::close() {
  connection_.reset();
  uri_.clear();
  length_ = -1;
  window_.clear();
  windowStart_ = 0;
  windowLength_ = 0;
  requestOpen_ = false;
  requestPosition_ = 0;
}

} // namespace coral
