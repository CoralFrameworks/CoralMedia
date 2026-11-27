# FFmpeg prebuilt package

This directory is a prebuilt FFmpeg package, not an FFmpeg source checkout.

Layout:

```text
android/<abi>/include   FFmpeg public headers for that ABI
android/<abi>/libffmpeg.so
windows/<arch>/include  FFmpeg public headers
windows/<arch>/         import libraries, DLLs and the MinGW runtime DLLs
linux/<arch>/include    FFmpeg public headers
linux/<arch>/           shared FFmpeg libraries
cmake/FFmpegConfig.cmake imported FFmpeg::ffmpeg target
BUILD_INFO.txt          version and build settings
```

`CoralMedia/modules/ffcodec` includes `cmake/FFmpegConfig.cmake` and links `FFmpeg::ffmpeg`
when `CORAL_SUPPORTS_FFMPEG` is enabled.

The Android package keeps the headers and the library of each ABI together. The Windows package
is checked in. The Linux package is built and published by `CoralFrameworks/FfmpegBuilder` CI and is
consumed from its artifact (or a release asset through `scripts/fetch-ffmpeg-linux.sh`), so
`linux/` is intentionally not tracked in this repository.

The shared libraries are replaceable: keeping the same FFmpeg major version is enough to swap
`libffmpeg.so`, the DLLs or the Linux `libav*.so` files without rebuilding the framework.
