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
 * @file WinHttpDataSource.cpp
 *
 * The Windows implementation of {@code AVHttpDataSource}, which reads the media of an http or an
 * https uri with WinHTTP.
 *
 * WinHTTP is used rather than WinINet, because it is the API of the platform that is meant for a
 * service: it neither reads nor writes the settings of the interactive user, and it negotiates the
 * transport security of an https uri on its own.
 *
 * The strategy of the reads, i.e. the window that the bytes are read ahead into and the range requests
 * that fill it, is the one of {@code HttpDataSourceBase}: this file only implements the operations
 * that talk to the server.
 */

#include "HttpDataSourceBase.h"

// NOMINMAX keeps the platform headers from defining the min and max macros, which would break the
// standard library the implementation uses.
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

#include <minilog/log.h>

namespace coral {

namespace {

/** The largest number of bytes a single reading of the platform API takes. */
constexpr size_t MAX_PLATFORM_READ_SIZE = 256 * 1024;

/** Converts a UTF-8 string to the UTF-16 string the platform API takes, or an empty string. */
std::wstring toWideString(const std::string& value) {
  if (value.empty()) {
    return std::wstring();
  }
  const int length = ::MultiByteToWideChar(CP_UTF8, 0, value.c_str(),
    static_cast<int>(value.size()), nullptr, 0);
  if (length <= 0) {
    return std::wstring();
  }
  std::wstring wide(static_cast<size_t>(length), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), wide.data(), length);
  return wide;
}

/** Converts a UTF-16 string to the UTF-8 string the module uses. */
std::string toUtf8String(const std::wstring& value) {
  if (value.empty()) {
    return std::string();
  }
  const int length = ::WideCharToMultiByte(CP_UTF8, 0, value.c_str(),
    static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
  if (length <= 0) {
    return std::string();
  }
  std::string text(static_cast<size_t>(length), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), text.data(), length,
    nullptr, nullptr);
  return text;
}

/** The parts of an http url a request is built from. */
struct Url {
  /** The host of the url, without its port. */
  std::wstring host;

  /** The path of the resource, with its query string. */
  std::wstring path;

  /** The port of the url, i.e. the default port of its scheme when it does not name one. */
  INTERNET_PORT port = 0;

  /** Whether the url is read over a secured connection, i.e. whether its scheme is https. */
  bool secure = false;
};

/** Parses the url of a media, or returns false when the uri is not a usable http url. */
bool parseUrl(const std::string& uri, Url& url) {
  const std::wstring wideUri = toWideString(uri);
  if (wideUri.empty()) {
    return false;
  }
  URL_COMPONENTS components = {};
  components.dwStructSize = sizeof(components);
  components.dwSchemeLength = static_cast<DWORD>(-1);
  components.dwHostNameLength = static_cast<DWORD>(-1);
  components.dwUrlPathLength = static_cast<DWORD>(-1);
  components.dwExtraInfoLength = static_cast<DWORD>(-1);
  if (!::WinHttpCrackUrl(wideUri.c_str(), static_cast<DWORD>(wideUri.size()), 0, &components)) {
    MINILOG_ERROR("the url of the media is not a usable http url: {}", uri);
    return false;
  }
  if (components.nScheme != INTERNET_SCHEME_HTTP && components.nScheme != INTERNET_SCHEME_HTTPS) {
    MINILOG_ERROR("the url of the media is not an http url: {}", uri);
    return false;
  }

  url.secure = components.nScheme == INTERNET_SCHEME_HTTPS;
  url.port = components.nPort;
  url.host.assign(components.lpszHostName, components.dwHostNameLength);
  url.path.assign(components.lpszUrlPath, components.dwUrlPathLength);
  if (components.lpszExtraInfo != nullptr && components.dwExtraInfoLength > 0) {
    // The query string is part of the resource, and the path of a request does not carry it.
    url.path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
  }
  return true;
}

/**
 * The connection of WinHTTP to the server of a media, and the request that reads the media from the
 * position it was opened at. The handles are released by the destructor, so that neither an early
 * return nor an exception can leak them.
 */
class WinHttpConnection final : public HttpDataSourceBase::Connection {
public:
  WinHttpConnection(std::string uri, Url url, const AVHttpDataSource::Parameters& parameters);

  ~WinHttpConnection() override;

  WinHttpConnection(const WinHttpConnection& other) = delete;

  WinHttpConnection& operator=(const WinHttpConnection& other) = delete;

  bool openRequest(long position, const std::string& headers) override;

  long read(uint8_t* buffer, size_t length) override;

  void closeRequest() override;

  [[nodiscard]] int getResponseCode() const override { return responseCode_; }

  [[nodiscard]] std::string getResponseHeader(const std::string& name) const override;

  /** Returns whether the session and the connection to the host were opened. */
  [[nodiscard]] bool isConnected() const { return connect_ != nullptr; }

private:
  /** Releases every handle of the connection. */
  void release();

  std::string uri_;
  Url url_;
  HINTERNET session_ = nullptr;
  HINTERNET connect_ = nullptr;
  HINTERNET request_ = nullptr;

  /** The status code of the response of the request that is open. */
  int responseCode_ = 0;
};

WinHttpConnection::WinHttpConnection(std::string uri, Url url,
                                     const AVHttpDataSource::Parameters& parameters)
  : uri_(std::move(uri)),
    url_(std::move(url)) {
  // The proxy of the configuration is used rather than the automatic detection of a proxy: the detection
  // of a proxy costs seconds when the network does not answer the configuration protocol, and it is not
  // covered by the timeouts of the session. No credentials are sent either: the media of an http url is
  // public, or it is the application that signs the requests through the properties of the parameters.
  session_ = ::WinHttpOpen(L"coral/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (session_ == nullptr) {
    MINILOG_ERROR("failed to open a connection to the media of {}", uri_);
    return;
  }
  const int timeoutMs = static_cast<int>(std::max<int64_t>(parameters.connectTimeoutMs, 0));
  ::WinHttpSetTimeouts(session_, timeoutMs, timeoutMs, timeoutMs,
    static_cast<int>(std::max<int64_t>(parameters.readTimeoutMs, 0)));

  connect_ = ::WinHttpConnect(session_, url_.host.c_str(),
    static_cast<INTERNET_PORT>(url_.port), 0);
  if (connect_ == nullptr) {
    MINILOG_ERROR("failed to connect to the host of {}", uri_);
    return;
  }
}

WinHttpConnection::~WinHttpConnection() { release(); }

void WinHttpConnection::release() {
  closeRequest();
  if (connect_ != nullptr) {
    ::WinHttpCloseHandle(connect_);
    connect_ = nullptr;
  }
  if (session_ != nullptr) {
    ::WinHttpCloseHandle(session_);
    session_ = nullptr;
  }
}

bool WinHttpConnection::openRequest(const long position, const std::string& headers) {
  closeRequest();
  if (connect_ == nullptr) {
    return false;
  }

  request_ = ::WinHttpOpenRequest(connect_, L"GET", url_.path.c_str(), nullptr,
    WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, url_.secure ? WINHTTP_FLAG_SECURE : 0);
  if (request_ == nullptr) {
    MINILOG_ERROR("failed to create the request of the media of {} at {}", uri_, position);
    return false;
  }

  const std::wstring wideHeaders = toWideString(headers);
  if (!::WinHttpAddRequestHeaders(request_, wideHeaders.c_str(),
        static_cast<DWORD>(wideHeaders.size()), WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE)) {
    MINILOG_ERROR("failed to set the headers of the request of the media of {}", uri_);
    closeRequest();
    return false;
  }

  if (!::WinHttpSendRequest(request_, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !::WinHttpReceiveResponse(request_, nullptr)) {
    MINILOG_ERROR("failed to read the media of {}: the request failed with {}", uri_,
      static_cast<int>(::GetLastError()));
    closeRequest();
    return false;
  }

  DWORD statusCode = 0;
  DWORD statusCodeSize = sizeof(statusCode);
  if (!::WinHttpQueryHeaders(request_, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeSize, WINHTTP_NO_HEADER_INDEX)) {
    closeRequest();
    return false;
  }
  responseCode_ = static_cast<int>(statusCode);
  return true;
}

long WinHttpConnection::read(uint8_t* buffer, const size_t length) {
  if (request_ == nullptr) {
    return -1;
  }
  // The strategy of the reads of the base asks for the bytes of the window it fills, which is smaller
  // than the largest reading of the platform API, therefore the length fits its 32 bit length.
  const auto requested = static_cast<DWORD>(std::min(length, MAX_PLATFORM_READ_SIZE));
  DWORD read = 0;
  if (!::WinHttpReadData(request_, buffer, requested, &read)) {
    MINILOG_ERROR("failed to read the media of {}: {}", uri_, static_cast<int>(::GetLastError()));
    return -1;
  }
  return static_cast<long>(read);
}

void WinHttpConnection::closeRequest() {
  if (request_ != nullptr) {
    ::WinHttpCloseHandle(request_);
    request_ = nullptr;
  }
  responseCode_ = 0;
}

std::string WinHttpConnection::getResponseHeader(const std::string& name) const {
  if (request_ == nullptr) {
    return {};
  }
  const std::wstring wideName = toWideString(name);
  if (wideName.empty()) {
    return {};
  }
  DWORD length = 0;
  ::WinHttpQueryHeaders(request_, WINHTTP_QUERY_CUSTOM, wideName.c_str(), WINHTTP_NO_OUTPUT_BUFFER,
    &length, WINHTTP_NO_HEADER_INDEX);
  if (length == 0 || ::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
    return {};
  }
  std::wstring value(length / sizeof(wchar_t), L'\0');
  if (!::WinHttpQueryHeaders(request_, WINHTTP_QUERY_CUSTOM, wideName.c_str(), value.data(), &length,
        WINHTTP_NO_HEADER_INDEX)) {
    return {};
  }
  // The length the query reports counts the final null character of the value, which is not part of it.
  value.resize(length / sizeof(wchar_t));
  while (!value.empty() && value.back() == L'\0') {
    value.pop_back();
  }
  return toUtf8String(value);
}

/**
 * The http data source of the platform: it creates the connection of WinHTTP to the server of the uri
 * that is opened, and leaves the reading of the media to {@code HttpDataSourceBase}.
 */
class WinHttpDataSource final : public HttpDataSourceBase {
public:
  explicit WinHttpDataSource(Parameters parameters)
    : HttpDataSourceBase(std::move(parameters)) {
  }

protected:
  [[nodiscard]] std::unique_ptr<Connection> createConnection(const std::string& uri) override {
    Url url;
    if (!parseUrl(uri, url)) {
      return nullptr;
    }
    auto connection = std::make_unique<WinHttpConnection>(uri, std::move(url), getParameters());
    if (!connection->isConnected()) {
      return nullptr;
    }
    return connection;
  }
};

/** The factory of the Windows data source, which the module exposes through its interface. */
class WinHttpDataSourceFactory final : public AVHttpDataSourceFactory {
public:
  [[nodiscard]] std::shared_ptr<AVHttpDataSource> create(
    AVHttpDataSource::Parameters parameters) override {
    return std::make_shared<WinHttpDataSource>(std::move(parameters));
  }
};

} // namespace anonymous

bool isHttpDataSourceSupported() { return true; }

std::unique_ptr<AVHttpDataSourceFactory> createHttpDataSourceFactory() {
  return std::make_unique<WinHttpDataSourceFactory>();
}

} // namespace coral
