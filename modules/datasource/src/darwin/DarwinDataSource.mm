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

#if defined(__APPLE__)

#include <datasource/DarwinDataSource.h>

#import <Foundation/Foundation.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include <minilog/log.h>

namespace coral {

namespace {

/** Converts the utf-8 url of the caller to a string, or returns nil for an invalid one. */
NSString* stringFromUtf8(const std::string& text) {
  if (text.empty()) {
    return nil;
  }
  return [NSString stringWithUTF8String:text.c_str()];
}

/**
 * Returns the file url a uri names, or nil when the uri is not a local file.
 *
 * A plain path is a file of the file system, and a `file` uri is parsed by the platform. A `file`
 * uri whose path is not percent-encoded is accepted as well: a path of a media commonly contains a
 * space, and the caller may have built the uri by prefixing `file://` to the path it already had.
 */
NSURL* fileURLFromUri(NSString* text) {
  if (text.length == 0) {
    return nil;
  }
  if (![text.lowercaseString hasPrefix:@"file:"]) {
    // A plain path, which may name the home directory of the user.
    return [NSURL fileURLWithPath:[text stringByExpandingTildeInPath]];
  }

  NSURL* url = [NSURL URLWithString:text];
  if (url != nil && url.isFileURL) {
    if (url.host.length > 0 && ![url.host isEqualToString:@"localhost"]) {
      // A file url of a remote authority names a share rather than a file of this device.
      return nil;
    }
    return url;
  }

  // The platform refuses a url whose path is not percent-encoded, e.g. a path with a space. The
  // bytes after the scheme are the path in that case, and the authority is the empty one.
  NSString* rest = [text substringFromIndex:5];
  if ([rest hasPrefix:@"//"]) {
    rest = [rest substringFromIndex:2];
    NSRange slash = [rest rangeOfString:@"/"];
    if (slash.location == NSNotFound) {
      return nil;
    }
    NSString* host = [rest substringToIndex:slash.location];
    if (host.length > 0 && ![host isEqualToString:@"localhost"]) {
      return nil;
    }
    rest = [rest substringFromIndex:slash.location];
  }
  NSString* path = rest.stringByRemovingPercentEncoding ?: rest;
  return path.length > 0 ? [NSURL fileURLWithPath:path] : nil;
}

/**
 * Returns the path of a bundle resource relative to the bundle, or nil when the uri does not name a
 * bundle resource.
 *
 * The `asset` and `bundle` schemes name the resource directly. The `file:///android_asset/` uri of
 * Android names the same resource, so that a media named once is playable on both platforms.
 */
NSString* bundlePathFromUri(NSString* text) {
  const NSRange colon = [text rangeOfString:@":"];
  if (colon.location == NSNotFound) {
    return nil;
  }
  NSString* scheme = [[text substringToIndex:colon.location] lowercaseString];
  NSString* rest = [text substringFromIndex:colon.location + 1];
  // A query or a fragment is not part of the path of a resource. The percent-encoded forms of the
  // two characters are part of the name, and are decoded below.
  const NSRange query = [rest rangeOfString:@"?"];
  if (query.location != NSNotFound) {
    rest = [rest substringToIndex:query.location];
  }
  const NSRange fragment = [rest rangeOfString:@"#"];
  if (fragment.location != NSNotFound) {
    rest = [rest substringToIndex:fragment.location];
  }

  if ([scheme isEqualToString:@"file"]) {
    NSURL* url = [NSURL URLWithString:text];
    NSString* path = url.isFileURL ? url.path : nil;
    if (path == nil) {
      path = rest.stringByRemovingPercentEncoding ?: rest;
      if ([path hasPrefix:@"//"]) {
        path = [path substringFromIndex:2];
        const NSRange slash = [path rangeOfString:@"/"];
        path = slash.location == NSNotFound ? @"" : [path substringFromIndex:slash.location];
      }
    }
    static NSString* const kAndroidAssetPrefix = @"/android_asset/";
    if (![path hasPrefix:kAndroidAssetPrefix]) {
      return nil;
    }
    return [path substringFromIndex:kAndroidAssetPrefix.length];
  }

  if (![scheme isEqualToString:@"asset"] && ![scheme isEqualToString:@"bundle"]) {
    return nil;
  }

  if ([rest hasPrefix:@"//"]) {
    rest = [rest substringFromIndex:2];
    const NSRange slash = [rest rangeOfString:@"/"];
    if (slash.location == NSNotFound) {
      return nil;
    }
    NSString* host = [rest substringToIndex:slash.location];
    NSString* path = [rest substringFromIndex:slash.location];
    rest = host.length > 0 && ![host isEqualToString:@"localhost"]
             ? [host stringByAppendingString:path]
             : path;
  }
  rest = rest.stringByRemovingPercentEncoding ?: rest;
  while ([rest hasPrefix:@"/"]) {
    rest = [rest substringFromIndex:1];
  }
  return rest.length > 0 ? rest : nil;
}

/**
 * Resolves a bundle relative path against the main bundle, or returns nil when no resource of the
 * bundle has that path.
 *
 * The resource directory is the first root, i.e. `Contents/Resources` of a macOS application and the
 * directory of the `.app` of an iOS application. The bundle directory is the second root, which is
 * what a command line tool, whose resource directory is the directory of its executable, and a
 * resource copied beside the executable use.
 */
NSURL* resolveBundleResource(NSString* relative) {
  if (relative.length == 0) {
    return nil;
  }
  NSBundle* bundle = [NSBundle mainBundle];
  NSMutableArray<NSURL*>* roots = [NSMutableArray arrayWithCapacity:2];
  if (bundle.resourceURL != nil) {
    [roots addObject:bundle.resourceURL];
  }
  if (bundle.bundleURL != nil && ![bundle.bundleURL isEqual:bundle.resourceURL]) {
    [roots addObject:bundle.bundleURL];
  }

  NSFileManager* manager = [NSFileManager defaultManager];
  for (NSURL* root in roots) {
    NSURL* candidate = [[root URLByAppendingPathComponent:relative] URLByStandardizingPath];
    NSString* base = root.URLByStandardizingPath.path;
    NSString* path = candidate.path;
    if (base.length == 0 || path.length == 0) {
      continue;
    }
    if (![base hasSuffix:@"/"]) {
      base = [base stringByAppendingString:@"/"];
    }
    // The standardised path keeps a request for "../" out of the bundle: a resource names a file of
    // the bundle rather than a file the link of the bundle happens to reach.
    if (![path hasPrefix:base]) {
      continue;
    }
    BOOL directory = NO;
    if ([manager fileExistsAtPath:path isDirectory:&directory] && !directory) {
      return candidate;
    }
  }
  return nil;
}

} // namespace anonymous

// ==================================================================================================
// AVDarwinFileDataSource
// ==================================================================================================

struct AVDarwinFileDataSource::Impl {
  ~Impl() { release(); }

  /**
   * Opens one candidate url, taking the security scope of the url on success.
   *
   * The scope is started before the file is opened and stopped when the data source is closed; a url
   * that was not security scoped reports no scope, and nothing is stopped then.
   */
  bool openUrl(NSURL* url) {
    if (url == nil || !url.isFileURL) {
      return false;
    }
    const BOOL scoped = [url startAccessingSecurityScopedResource];
    const int descriptor = ::open(url.fileSystemRepresentation, O_RDONLY);
    struct stat information {};
    if (descriptor < 0 || fstat(descriptor, &information) != 0 ||
        !S_ISREG(information.st_mode)) {
      if (descriptor >= 0) {
        ::close(descriptor);
      }
      if (scoped) {
        [url stopAccessingSecurityScopedResource];
      }
      return false;
    }
    release();
    fileDescriptor = descriptor;
    length = static_cast<long>(information.st_size);
    isScoped = scoped != NO;
    scopedUrl = url;
    return true;
  }

  void release() {
    if (fileDescriptor >= 0) {
      ::close(fileDescriptor);
      fileDescriptor = -1;
    }
    if (isScoped && scopedUrl != nil) {
      [scopedUrl stopAccessingSecurityScopedResource];
    }
    isScoped = false;
    scopedUrl = nil;
    length = -1;
  }

  int fileDescriptor = -1;
  long length = -1;
  bool isScoped = false;
  //!< The url whose security scope is held, which has to stay alive until the scope is stopped.
  __strong NSURL* scopedUrl = nil;
  std::string uri;
};

AVDarwinFileDataSource::AVDarwinFileDataSource()
  : impl_(std::make_unique<Impl>()) {
}

AVDarwinFileDataSource::~AVDarwinFileDataSource() {
  close();
}

bool AVDarwinFileDataSource::open(const std::string& uri) {
  close();
  NSString* text = stringFromUtf8(uri);
  if (text == nil) {
    MINILOG_WARN("the media uri is not a valid utf-8 string");
    return false;
  }
  NSURL* url = fileURLFromUri(text);
  if (url == nil) {
    MINILOG_WARN("the uri {} does not name a file of the local file system", uri);
    return false;
  }
  if (!impl_->openUrl(url)) {
    // A relative path is resolved against the resource directory of the main bundle when it is not
    // a file of the current directory, which is what an application that ships its media expects.
    NSString* path = url.path;
    if (url.host.length == 0 && path.length > 0 && ![path hasPrefix:@"/"]) {
      // The same lookup the bundle data source uses, which keeps a "../" out of the bundle.
      NSURL* resource = resolveBundleResource(path);
      if (resource != nil && impl_->openUrl(resource)) {
        impl_->uri = uri;
        return true;
      }
    }
    MINILOG_WARN("failed to open the media file: {}", uri);
    return false;
  }
  impl_->uri = uri;
  return true;
}

int AVDarwinFileDataSource::readAt(const long position, uint8_t* buffer, const size_t offset,
                                   const size_t length) {
  if (impl_ == nullptr || impl_->fileDescriptor < 0 || buffer == nullptr) {
    return -1;
  }
  if (position < 0 || length == 0) {
    return 0;
  }
  if (impl_->length >= 0 && position >= impl_->length) {
    return 0;
  }
  size_t toRead = length;
  if (impl_->length >= 0 && static_cast<long>(toRead) > impl_->length - position) {
    toRead = static_cast<size_t>(impl_->length - position);
  }
  const auto read = pread(impl_->fileDescriptor, buffer + offset, toRead,
    static_cast<off_t>(position));
  if (read < 0) {
    if (errno == EINTR) {
      // A read that the signal of the process interrupted is a short read, not a broken media.
      return 0;
    }
    MINILOG_ERROR("failed to read {} bytes at {} of {}", toRead, position, impl_->uri);
    return -1;
  }
  return static_cast<int>(read);
}

long AVDarwinFileDataSource::getLength() const {
  return impl_ != nullptr ? impl_->length : -1;
}

const std::string& AVDarwinFileDataSource::getUri() const {
  return impl_->uri;
}

void AVDarwinFileDataSource::close() {
  if (impl_ != nullptr) {
    impl_->release();
    impl_->uri.clear();
  }
}

bool AVDarwinFileDataSource::isOpen() const {
  return impl_ != nullptr && impl_->fileDescriptor >= 0;
}

// ==================================================================================================
// AVBundleDataSource
// ==================================================================================================

struct AVBundleDataSource::Impl {
  std::unique_ptr<AVDarwinFileDataSource> source;
  std::string uri;
  bool open = false;
};

AVBundleDataSource::AVBundleDataSource()
  : impl_(std::make_unique<Impl>()) {
}

AVBundleDataSource::~AVBundleDataSource() {
  close();
}

bool AVBundleDataSource::open(const std::string& uri) {
  close();
  NSString* text = stringFromUtf8(uri);
  if (text == nil) {
    MINILOG_WARN("the bundle uri is not a valid utf-8 string");
    return false;
  }
  NSString* relative = bundlePathFromUri(text);
  if (relative.length == 0) {
    MINILOG_WARN("the uri {} names no resource of the bundle", uri);
    return false;
  }
  NSURL* resource = resolveBundleResource(relative);
  if (resource == nil) {
    MINILOG_WARN("the resource {} was not found in the main bundle", relative.UTF8String);
    return false;
  }
  auto source = std::make_unique<AVDarwinFileDataSource>();
  NSString* absolute = resource.absoluteString;
  if (absolute == nil || !source->open(absolute.UTF8String)) {
    MINILOG_WARN("failed to open the bundle resource {} of {}", relative.UTF8String, uri);
    return false;
  }
  impl_->source = std::move(source);
  impl_->uri = uri;
  impl_->open = true;
  return true;
}

int AVBundleDataSource::readAt(const long position, uint8_t* buffer, const size_t offset,
                               const size_t length) {
  if (impl_ == nullptr || !impl_->open || impl_->source == nullptr) {
    return -1;
  }
  return impl_->source->readAt(position, buffer, offset, length);
}

long AVBundleDataSource::getLength() const {
  return impl_ != nullptr && impl_->source != nullptr ? impl_->source->getLength() : -1;
}

const std::string& AVBundleDataSource::getUri() const {
  return impl_->uri;
}

void AVBundleDataSource::close() {
  if (impl_ != nullptr) {
    if (impl_->source != nullptr) {
      impl_->source->close();
      impl_->source.reset();
    }
    impl_->open = false;
    impl_->uri.clear();
  }
}

bool AVBundleDataSource::isOpen() const {
  return impl_ != nullptr && impl_->open && impl_->source != nullptr && impl_->source->isOpen();
}

} // namespace coral

#endif // defined(__APPLE__)
