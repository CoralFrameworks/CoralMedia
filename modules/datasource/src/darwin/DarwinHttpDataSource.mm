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
 * @file DarwinHttpDataSource.mm
 *
 * The macOS and iOS implementation of {@code AVHttpDataSource}, which reads the media of an http or
 * an https uri with `NSURLSession`.
 *
 * `NSURLSession` is the http API of the platform: it talks HTTP/1.1 and HTTP/2, negotiates the
 * transport security of an https uri with the same rules as the rest of the system, follows the
 * redirects and honors the proxy and the App Transport Security configuration of the application.
 * The strategy of the reads, i.e. the range requests and the window that the bytes are read ahead
 * into, is the one of {@code HttpDataSourceBase}: this file only implements the operations that talk
 * to the server.
 */

#if defined(__APPLE__)

#include "HttpDataSourceBase.h"

#import <Foundation/Foundation.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <minilog/log.h>

namespace coral {

namespace {

/**
 * The number of bytes the session may buffer before the task is suspended. The base reads the media
 * through a window of one megabyte, and the range request of a compliant server is that wide; a
 * server that ignores the range answers with the whole media, and suspending the task keeps the
 * bytes that wait for the window from growing without a bound.
 */
constexpr size_t kHighWaterBytes = 4 * 1024 * 1024;

/** The number of bytes that are left before the suspended task is resumed. */
constexpr size_t kLowWaterBytes = 1 * 1024 * 1024;

/** The longest time a request of a media is given before the session gives it up, in seconds. */
constexpr int kResourceTimeoutSeconds = 60;

/**
 * The response of a request: the headers once they arrived, and the bytes the delegate received.
 *
 * The delegate and the connection share the state rather than the connection owning the delegate:
 * the session retains the delegate until it is invalidated, and the state outlives both so that a
 * callback that arrives after the request was closed cannot reach a destroyed object.
 */
struct RequestState {
  std::mutex mutex;
  std::condition_variable changed;

  __strong NSHTTPURLResponse* response = nil;
  __strong NSError* error = nil;
  __strong NSURLSessionDataTask* task = nil;

  std::vector<uint8_t> data;
  size_t consumed = 0;
  bool finished = false;
  bool suspended = false;

  [[nodiscard]] size_t availableLocked() const {
    return data.size() - consumed;
  }

  void compactLocked() {
    if (consumed == data.size()) {
      data.clear();
      consumed = 0;
    } else if (consumed > kLowWaterBytes) {
      data.erase(data.begin(), data.begin() + static_cast<ptrdiff_t>(consumed));
      consumed = 0;
    }
  }
};

/** Converts an Objective-C string to the utf-8 string of the module, or an empty string. */
std::string utf8String(NSString* text) {
  if (text == nil || text.UTF8String == nullptr) {
    return {};
  }
  return text.UTF8String;
}

/** Describes the error of a request for the log. */
std::string describeError(NSError* error) {
  return error != nil ? utf8String(error.localizedDescription) : std::string("no error");
}

/** Parses the uri of a media, or returns nil when it is not a usable http url. */
NSURL* httpUrlFromString(const std::string& uri) {
  NSString* text = [NSString stringWithUTF8String:uri.c_str()];
  if (text == nil) {
    return nil;
  }
  NSURL* url = [NSURL URLWithString:text];
  if (url == nil || url.host.length == 0) {
    return nil;
  }
  NSString* scheme = url.scheme.lowercaseString;
  if (![scheme isEqualToString:@"http"] && ![scheme isEqualToString:@"https"]) {
    return nil;
  }
  return url;
}

/**
 * Sets the headers of the request. The base expresses them as the lines of an http request, i.e. each
 * of them followed by CRLF, which is the form the platform request is filled from.
 */
void applyHeaders(NSMutableURLRequest* request, const std::string& headers) {
  NSString* text = [NSString stringWithUTF8String:headers.c_str()];
  if (text == nil) {
    return;
  }
  NSCharacterSet* whitespace = [NSCharacterSet whitespaceAndNewlineCharacterSet];
  for (NSString* line in [text componentsSeparatedByString:@"\r\n"]) {
    if (line.length == 0) {
      continue;
    }
    const NSRange colon = [line rangeOfString:@":"];
    if (colon.location == NSNotFound) {
      continue;
    }
    NSString* name = [[line substringToIndex:colon.location]
      stringByTrimmingCharactersInSet:whitespace];
    NSString* value = [[line substringFromIndex:colon.location + 1]
      stringByTrimmingCharactersInSet:whitespace];
    if (name.length > 0) {
      [request setValue:value forHTTPHeaderField:name];
    }
  }
}

} // namespace anonymous

} // namespace coral

/** The delegate of one request, which collects the response and the body into the shared state. */
@interface CoralDarwinHttpSessionDelegate : NSObject <NSURLSessionDataDelegate>
- (instancetype)initWithState:(std::shared_ptr<coral::RequestState>)state;
@end

@implementation CoralDarwinHttpSessionDelegate {
  std::shared_ptr<coral::RequestState> _state;
}

- (instancetype)initWithState:(std::shared_ptr<coral::RequestState>)state {
  self = [super init];
  if (self != nil) {
    _state = std::move(state);
  }
  return self;
}

- (void)URLSession:(NSURLSession*)session
             dataTask:(NSURLSessionDataTask*)dataTask
   didReceiveResponse:(NSURLResponse*)response
    completionHandler:(void (^)(NSURLSessionResponseDisposition disposition))completionHandler {
  static_cast<void>(session);
  auto state = _state;
  if (state != nullptr) {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->response = [response isKindOfClass:[NSHTTPURLResponse class]]
                        ? static_cast<NSHTTPURLResponse*>(response)
                        : nil;
    state->task = dataTask;
    state->changed.notify_all();
  }
  completionHandler(NSURLSessionResponseAllow);
}

- (void)URLSession:(NSURLSession*)session
             dataTask:(NSURLSessionDataTask*)dataTask
       didReceiveData:(NSData*)data {
  static_cast<void>(session);
  auto state = _state;
  if (state == nullptr) {
    return;
  }
  bool suspend = false;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    const auto* bytes = static_cast<const uint8_t*>(data.bytes);
    state->data.insert(state->data.end(), bytes, bytes + data.length);
    state->changed.notify_all();
    if (!state->suspended && state->availableLocked() >= coral::kHighWaterBytes) {
      state->suspended = true;
      suspend = true;
    }
  }
  if (suspend) {
    [dataTask suspend];
  }
}

- (void)URLSession:(NSURLSession*)session
                 task:(NSURLSessionTask*)task
    didCompleteWithError:(NSError*)error {
  static_cast<void>(session);
  static_cast<void>(task);
  auto state = _state;
  if (state == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(state->mutex);
  state->finished = true;
  if (error != nil && state->error == nil) {
    state->error = error;
  }
  state->changed.notify_all();
}

@end

namespace coral {

namespace {

/**
 * The connection of `NSURLSession` to the server of a media, and the request that reads the media
 * from the position it was opened at.
 *
 * A session is created for one request and invalidated when the request is closed: the base opens a
 * request per range window, and a session that outlived a request would keep the delegate and the
 * connection of the previous request alive while the next one runs.
 */
class DarwinHttpConnection final : public HttpDataSourceBase::Connection {
public:
  DarwinHttpConnection(std::string uri, const AVHttpDataSource::Parameters& parameters)
    : uri_(std::move(uri)),
      parameters_(parameters) {
  }

  ~DarwinHttpConnection() override {
    closeRequest();
  }

  DarwinHttpConnection(const DarwinHttpConnection& other) = delete;

  DarwinHttpConnection& operator=(const DarwinHttpConnection& other) = delete;

  bool openRequest(long position, const std::string& headers) override;

  long read(uint8_t* buffer, size_t length) override;

  void closeRequest() override;

  [[nodiscard]] int getResponseCode() const override;

  [[nodiscard]] std::string getResponseHeader(const std::string& name) const override;

private:
  std::string uri_;
  AVHttpDataSource::Parameters parameters_;

  __strong NSURLSession* session_ = nil;
  __strong NSURLSessionDataTask* task_ = nil;
  std::shared_ptr<RequestState> state_;
};

bool DarwinHttpConnection::openRequest(const long position, const std::string& headers) {
  static_cast<void>(position);
  closeRequest();

  NSURL* url = httpUrlFromString(uri_);
  if (url == nil) {
    MINILOG_ERROR("the url of the media is not a usable http url: {}", uri_);
    return false;
  }
  auto state = std::make_shared<RequestState>();

  NSMutableURLRequest* request = [NSMutableURLRequest requestWithURL:url];
  request.HTTPMethod = @"GET";
  request.cachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
  const auto readTimeoutMs = std::max<int64_t>(parameters_.readTimeoutMs, 1);
  const auto connectTimeoutMs = std::max<int64_t>(parameters_.connectTimeoutMs, 1);
  request.timeoutInterval = static_cast<NSTimeInterval>(readTimeoutMs) / 1000.0;
  applyHeaders(request, headers);

  NSURLSessionConfiguration* configuration =
    [NSURLSessionConfiguration ephemeralSessionConfiguration];
  configuration.timeoutIntervalForRequest =
    static_cast<NSTimeInterval>(readTimeoutMs) / 1000.0;
  configuration.timeoutIntervalForResource = static_cast<NSTimeInterval>(
    std::max<int64_t>(kResourceTimeoutSeconds * 1000, readTimeoutMs + connectTimeoutMs) / 1000);
  configuration.HTTPMaximumConnectionsPerHost = 1;
  configuration.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
  configuration.URLCache = nil;
  configuration.HTTPCookieStorage = nil;
  configuration.URLCredentialStorage = nil;
  configuration.HTTPShouldSetCookies = NO;
  // A request that waits for the network to become reachable would wait far beyond the timeout of
  // the media, which a load reports as a failure rather than as a stall.
  configuration.waitsForConnectivity = NO;

  CoralDarwinHttpSessionDelegate* delegate =
    [[CoralDarwinHttpSessionDelegate alloc] initWithState:state];
  NSURLSession* session = [NSURLSession sessionWithConfiguration:configuration
                                                       delegate:delegate
                                                  delegateQueue:nil];
  if (session == nil) {
    MINILOG_ERROR("failed to create the session of the media of {}", uri_);
    return false;
  }
  NSURLSessionDataTask* task = [session dataTaskWithRequest:request];
  if (task == nil) {
    MINILOG_ERROR("failed to create the request of the media of {}", uri_);
    [session invalidateAndCancel];
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->task = task;
  }
  session_ = session;
  task_ = task;
  state_ = state;
  [task resume];

  // The connection is reported with the response headers: the base reads the status and the length
  // of the media before it reads a byte of the body.
  const auto waitMs = std::max<int64_t>(connectTimeoutMs, readTimeoutMs);
  std::unique_lock<std::mutex> lock(state->mutex);
  const bool ready = state->changed.wait_for(lock, std::chrono::milliseconds(waitMs), [&state] {
    return state->response != nil || state->finished;
  });
  if (!ready || state->response == nil) {
    const auto error = state->error;
    lock.unlock();
    if (!ready) {
      MINILOG_ERROR("the server of {} did not answer within {} ms", uri_, waitMs);
    } else {
      MINILOG_ERROR("the request of the media of {} failed: {}", uri_, describeError(error));
    }
    closeRequest();
    return false;
  }
  return true;
}

long DarwinHttpConnection::read(uint8_t* buffer, size_t length) {
  if (state_ == nullptr || buffer == nullptr) {
    return -1;
  }
  if (length == 0) {
    return 0;
  }
  auto state = state_;
  std::unique_lock<std::mutex> lock(state->mutex);
  const auto timeout = std::chrono::milliseconds(std::max<int64_t>(parameters_.readTimeoutMs, 1));
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (true) {
    const size_t available = state->availableLocked();
    if (available > 0) {
      const size_t toCopy = std::min(available, length);
      std::memcpy(buffer, state->data.data() + state->consumed, toCopy);
      state->consumed += toCopy;
      state->compactLocked();
      bool resume = false;
      if (state->suspended && state->availableLocked() <= kLowWaterBytes) {
        state->suspended = false;
        resume = true;
      }
      NSURLSessionDataTask* task = state->task;
      lock.unlock();
      if (resume && task != nil) {
        [task resume];
      }
      return static_cast<long>(toCopy);
    }
    if (state->finished) {
      if (state->error != nil) {
        const auto error = state->error;
        lock.unlock();
        MINILOG_ERROR("failed to read the media of {}: {}", uri_, describeError(error));
        return -1;
      }
      return 0;
    }
    if (state->changed.wait_until(lock, deadline) == std::cv_status::timeout) {
      if (state->availableLocked() == 0 && !state->finished) {
        lock.unlock();
        MINILOG_ERROR("the read of the media of {} timed out after {} ms", uri_,
          parameters_.readTimeoutMs);
        closeRequest();
        return -1;
      }
    }
  }
}

void DarwinHttpConnection::closeRequest() {
  auto state = state_;
  state_.reset();
  if (state != nullptr) {
    // The delegate may be waiting for a read that will never complete; the finished flag wakes it.
    std::lock_guard<std::mutex> lock(state->mutex);
    state->finished = true;
    state->changed.notify_all();
  }
  if (task_ != nil) {
    [task_ cancel];
    task_ = nil;
  }
  if (session_ != nil) {
    [session_ invalidateAndCancel];
    session_ = nil;
  }
}

int DarwinHttpConnection::getResponseCode() const {
  auto state = state_;
  if (state == nullptr) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(state->mutex);
  return state->response != nil ? static_cast<int>(state->response.statusCode) : 0;
}

std::string DarwinHttpConnection::getResponseHeader(const std::string& name) const {
  auto state = state_;
  if (state == nullptr) {
    return {};
  }
  NSString* wanted = [NSString stringWithUTF8String:name.c_str()];
  if (wanted == nil) {
    return {};
  }
  std::lock_guard<std::mutex> lock(state->mutex);
  NSHTTPURLResponse* response = state->response;
  if (response == nil) {
    return {};
  }
  for (id key in response.allHeaderFields) {
    NSString* keyText = [key isKindOfClass:[NSString class]] ? static_cast<NSString*>(key)
                                                            : [key description];
    if ([keyText caseInsensitiveCompare:wanted] != NSOrderedSame) {
      continue;
    }
    id value = response.allHeaderFields[key];
    NSString* valueText = [value isKindOfClass:[NSString class]]
                            ? static_cast<NSString*>(value)
                            : [value description];
    return utf8String(valueText);
  }
  return {};
}

/** The http data source of the platform, i.e. the strategy of the reads bound to `NSURLSession`. */
class DarwinHttpDataSource final : public HttpDataSourceBase {
public:
  explicit DarwinHttpDataSource(Parameters parameters)
    : HttpDataSourceBase(std::move(parameters)) {
  }

protected:
  [[nodiscard]] std::unique_ptr<Connection> createConnection(const std::string& uri) override {
    if (httpUrlFromString(uri) == nil) {
      return nullptr;
    }
    return std::make_unique<DarwinHttpConnection>(uri, getParameters());
  }
};

/** The factory of the Darwin data source, which the module exposes through its interface. */
class DarwinHttpDataSourceFactory final : public AVHttpDataSourceFactory {
public:
  [[nodiscard]] std::shared_ptr<AVHttpDataSource> create(
    AVHttpDataSource::Parameters parameters) override {
    return std::make_shared<DarwinHttpDataSource>(std::move(parameters));
  }
};

} // namespace anonymous

bool isHttpDataSourceSupported() { return true; }

std::unique_ptr<AVHttpDataSourceFactory> createHttpDataSourceFactory() {
  return std::make_unique<DarwinHttpDataSourceFactory>();
}

} // namespace coral

#endif // defined(__APPLE__)
