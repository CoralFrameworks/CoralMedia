# FFmpeg prebuilt package

This directory is a prebuilt FFmpeg package, not an FFmpeg source checkout.

Layout:

```text
include/                 FFmpeg public headers and config.h
cmake/FFmpegConfig.cmake imported FFmpeg::ffmpeg target
android/<abi>/           libffmpeg.so
windows/<arch>/          import/static libraries and DLLs
linux/<arch>/            shared or static FFmpeg libraries
BUILD_INFO.txt           version and build settings
```

`CoralMedia/modules/ffcodec` includes `cmake/FFmpegConfig.cmake` and links `FFmpeg::ffmpeg`
when `CORAL_SUPPORTS_FFMPEG` is enabled.

The Android libraries are packaged from the FFmpeg build used by the application; the Windows and
Linux directories are populated by the corresponding prebuild pipeline. Missing platform libraries
produce a configure warning from `FFmpegConfig.cmake`, so a headers-only package cannot silently
link a consumer.
