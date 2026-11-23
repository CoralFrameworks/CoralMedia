# Coral datasource

The random access data sources of the Coral media framework. A data source serves the bytes of a
media at a position, and the extractor module reads the container through it; `AVDefaultDataSource`
is the one that picks the data source of a uri, so an application names where its media is and does
not have to know which platform object reads it.

| Interface | Android | Windows | macOS / iOS |
| --- | --- | --- | --- |
| `file` / plain path | `AVFileDataSource` | `AVFileDataSource` | `AVDarwinFileDataSource` |
| `asset` / `bundle` / `file:///android_asset/` | `AVAssetDataSource` | — | `AVBundleDataSource` |
| `content` | `AVContentDataSource` | — | — |
| `rawresource` / `android.resource` | `AVRawResourceDataSource` | — | — |
| `http` / `https` | — | `WinHttpDataSource` | `DarwinHttpDataSource` (`NSURLSession`) |
| in memory | `AVByteArrayDataSource` | same | same |

The table is the contract of `AVDefaultDataSource`: a uri that names a file of the local file system
is read with the data source of the platform, an `asset` uri is read from the bundle of the
application, and the http uris are read with the http data source of the platform. The module keeps
the interfaces and the parts that are the same on every platform; the platform files hold the calls
of the system API.

## Android

The Android data sources read the media the platform itself names. An `asset:///video/a.mp4` and a
`file:///android_asset/video/a.mp4` name a file of the APK, a `content://` uri names the media a
provider hands out, and an `android.resource://` uri names a raw resource. The three are read through
a descriptor, or through the stream of the asset when the APK stores it compressed, without copying
the media into the cache of the application. The application context that the data sources read
through is registered with `jni::ApplicationContext`; see
[AndroidDataSource.h](include/datasource/AndroidDataSource.h) and the Android documentation of the
application.

## macOS and iOS

`src/darwin/DarwinDataSource.mm` implements the local media of the two platforms:

* **`AVDarwinFileDataSource`** reads a plain path or a `file` uri. A path may be percent-encoded
  (`file:///Users/me/My%20Movie.mp4`), may name the default authority
  (`file://localhost/...`), and may start with `~`. A relative path is resolved against the current
  directory first and against the resource directory of the main bundle second, so an application
  that ships its media as a resource can use either the path or the `asset` uri.
* **`AVBundleDataSource`** reads a resource of the main bundle. The supported uris are
  `asset:///video/a.mp4`, `bundle:///video/a.mp4`, `asset://video/a.mp4` (the first path component is
  read as the authority of the uri, like the host of an `http` url), and the Android spelling
  `file:///android_asset/video/a.mp4`. The resource directory of the main bundle is the first place a
  resource is looked up in, and the bundle directory is the second, which is what a command line tool
  and a resource copied beside its executable use. A path that leaves the bundle is refused.

A url that the system file picker returned is **security scoped**: the sandbox grants the access to
the url object rather than to the path it names. `AVDarwinFileDataSource` starts the access of the
url before it opens the file and stops it when the data source is closed. A url that was converted
to a string loses the scope in some cases, so an application that keeps the `NSURL` should start the
access itself and keep it while the media is read; the data source then opens the path with the
access the application holds. The `UIDocumentPicker` and the `PHPicker` hand over the file url of
the media they were asked for, which is the recommended input for the player.

The Photos library is not a stream of bytes the system hands out. A `PHAsset` is read with
`AVAssetReader` (the codec module of this framework does that) or exported to a file, and the picker
already returns the file url of the selection; the datasource module deliberately has no `ph://`
scheme that would silently copy a media into the temporary directory. An application that stores a
`PHAsset.localIdentifier` exports the asset before it hands the url to the player.

`src/darwin/DarwinHttpDataSource.mm` implements the http data source with **`NSURLSession`**:

* the range requests, the read ahead window and the interpretation of the response are the ones of
  `HttpDataSourceBase`, as on Windows;
* the session negotiates HTTP/1.1 or HTTP/2, the transport security of an `https` uri, the
  redirects, the system proxy and the certificate validation;
* the request properties of `AVHttpDataSource::Parameters` are set on every request, and the
  connection and read timeouts are applied to the session;
* the body is buffered by the delegate, and the task is suspended when more than 4 MiB wait for the
  window and resumed when the consumer drained it, so a server that ignores the range request cannot
  grow the buffer without a bound.

App Transport Security applies to `NSURLSession` as it does to the rest of the application: an
`http` uri of a host other than the local network requires the `NSAppTransportSecurity` exception of
the application's `Info.plist`. The tests of the module use a media of a local socket, which the
system allows without an exception; a media of a public host should be served over `https`.

## Tests

`tests/test_DataSource.cpp` covers the file, the buffer and the default data source on the host
platform, and on macOS it also covers the Darwin file source (percent-encoded path, `localhost`
authority, missing file) and the bundle source (the `asset`, `bundle` and `file:///android_asset`
spellings, percent-encoded resource names, the path that leaves the bundle, a missing resource).
`tests/test_DarwinHttpDataSource.cpp` serves a media from a socket of the test and reads it through
`NSURLSession`: a media of one window, a media larger than the window (which opens a second range
request) and a server that does not answer. The Android sources are exercised on a device by the
demo application, as described in the module documentation of the application.

```shell
cmake -S . -B build -G "Unix Makefiles"
cmake --build build --target test_datasource
./build/modules/datasource/tests/test_datasource
```
