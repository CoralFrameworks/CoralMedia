# The datasource tests

The suite is a gtest executable, `test_datasource`, that covers the whole module:

| File | Coverage |
| ---- | -------- |
| `test_DataSource.cpp` | `AVFileDataSource`, `AVByteArrayDataSource` and `AVDefaultDataSource`, i.e. the data source of a file, the one of a buffer, and the one that delegates a uri to the data source of its scheme. |
| `test_HttpDataSource.cpp` | The http data source as the module exposes it: the interface, the factory of the platform, and the failures that need no network. |
| `test_HttpDataSourceBase.cpp` | The strategy of the http reads, i.e. the range requests and the window the bytes are read ahead into, bound to a server of the test rather than to the one of a platform. |
| `TestData.h` | The builders of the content the tests read. |
| `test_main.cpp` | The `main` of the executable. |

## Build and run

```
cmake -S . -B build -G Ninja
cmake --build build --target test_datasource
build/libs/datasource/tests/test_datasource
```

When the build is run with `cmake --build build`, the test is also registered with ctest:

```
ctest --test-dir build -R test_datasource --output-on-failure
```

## The platform and the network

No test opens the media of a server: a test that needs one belongs to the platform, and the suite
must pass on a machine that has no network. The strategy that a platform shares with the others is
therefore tested through a server of the test, which makes the requests it receives, and the bytes
they are asked for, observable; the implementation of a platform is only asserted to fail on the uris
that are not http urls.

## Android

The data sources of the Android platform (an asset, a raw resource, a content uri and the descriptor
they are read through) need a device: they are covered by `AndroidDataSourceTest` of the demo, see
`docs/ANDROID.md` of the module.
