# CoralMedia examples

The framework's standalone validation project. `coralmedia_validate` exercises the four stages of the
media pipeline through the public API of the modules and reports each one:

| Stage | What it proves | Module |
| --- | --- | --- |
| encode | synthetic 1 s AAC audio and 30 H.264 frames are encoded | `codec` (platform codecs) |
| mux | the encoded tracks are written into an MP4 | `muxer` (`Mp4Muxer`), platform muxer as fallback |
| demux | the written file is read back, track by track | `extractor` |
| decode | the demuxed tracks are decoded again | `codec` (platform codecs) |

The program needs no fixture: it produces its own content. A checkout that carries codec fixtures
(a real H.264/AAC file) can run the demux and decode stages on that file as well:

```sh
coralmedia_validate --input path/to/video.mp4
```

## Building

As part of the framework, the examples are configured with the repository:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

On their own, which is what an IDE opens when working on the validation program:

```sh
cmake -S examples -B build/examples -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/examples
./build/examples/validate/coralmedia_validate
```

The standalone configure resolves CoralCore and the third-party targets through
`cmake/CoralMediaDependencies.cmake` and adds the framework the same way the application root does, so
the submodules have to be initialized first (see the dependency manifest's header).

## Exit codes

| Code | Meaning |
| --- | --- |
| 0 | at least one stage ran and none failed |
| 1 | a stage failed |
| 77 | nothing could be validated: the platform has no encoder/decoder and no `--input` was given |

`ctest` registers the program with `SKIP_RETURN_CODE 77`, so a machine without the platform codecs
reports the test as skipped rather than failed. `--workdir <dir>` places the produced files somewhere
inspectable, and `--keep` keeps them (they are removed on success otherwise).
