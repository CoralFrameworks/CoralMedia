# The extractor demo

`extractor_demo` is a small command line tool that opens a media file with `coral::AVReader`, walks
the tracks and prints the samples that the extractors deliver. It doubles as an integration test of
the module: every container that the module supports can be exercised with it, and the output shows
the format, the timestamps, the sample sizes and the flags that were extracted.

```
extractor_demo <media file> [options]

  --max-samples <count>  Stop after the given number of samples. Defaults to 16.
  --time-us <time>       Start at the given time, in microseconds, and therefore exercise a seek.
  --file-type <type>     A hint of the container type, see coral::FileTypes. It is inferred from the
                         extension when it is not given.
  --quiet                Only print the summary, no sample detail.
```

## Build

The demo is built with the rest of the project, and it is only configured on a host platform:

```
cmake -S . -B build -G Ninja
cmake --build build --target extractor_demo
```

The executable is placed in `libs/extractor/demo/extractor_demo` inside the build directory.

## Try it on a file of the repository

```
extractor_demo path/to/a/file.mp4
extractor_demo path/to/a/file.mkv --max-samples 8
extractor_demo path/to/a/file.mp3 --time-us 5000000
```

The repository ships one fixture of every newly supported container:

```
extractor_demo assets/music/music.amr --max-samples 4
extractor_demo assets/image/image.bmp --quiet
extractor_demo assets/image/image.png --quiet
extractor_demo assets/image/bridge.jpg --quiet
extractor_demo assets/image/image.webp --quiet
extractor_demo assets/image/image.avif --quiet
extractor_demo assets/image/mandrill_128.heic --quiet
extractor_demo assets/video/video.flv --max-samples 4
extractor_demo assets/video/video.ts --max-samples 4 --time-us 15000000
```

The FLV and TS fixtures carry H.264 video and AAC audio, the AMR fixture is a narrow band stream,
and the image fixtures are single sample containers. The timestamps and the sample sizes of the FLV
and TS fixtures match the packet list of `ffprobe`; the seek of the TS fixture resumes at the key
frame that precedes the requested time, exactly as the ExoPlayer extractor does.

## Generate a file to try it on

The test suite builds its media in memory, see `libs/extractor/tests/TestMedia.h`. The helpers can be
reused from a small program to write a fixture on disk:

```cpp
#include "TestMedia.h"

#include <cstdio>

int main() {
  const auto media = coral::test::buildWav(44100, 2, 44100);
  std::FILE* file = std::fopen("sample.wav", "wb");
  std::fwrite(media.data(), 1, media.size(), file);
  std::fclose(file);
  return 0;
}
```

The helpers are `buildWav`, `buildMp3`, `buildAdts`, `buildFlac`, `buildMp4`, `buildOggVorbis`,
`buildOggOpus`, `buildOggFlac`, `buildAviPcmAudio` and `buildAviVideoAndAudio`. Compile the program
with the include directories of the module and of `libs/extractor/tests`.

## What the output means

| Line | Meaning |
| ---- | ------- |
| `File type` | The file type that was inferred from the uri, or `-1` when it is unknown. |
| `Duration`, `Seekable` | The duration and the seekability declared by the `AVSeekMap` of the container. |
| `mime`, `container` | The sample MIME type and the MIME type of the container of a track. |
| `codecs` | The RFC 6381 codec string, when the container describes it. |
| `csd[n]` | The codec specific data of index `n`, which the decoder needs to be initialized. |
| `timeUs`, `size` | The presentation timestamp and the payload size of a sample. |
| `flags` | `key`, `config`, `first`, `last`, `discard` or `eos`, see `coral::AVSample::Flags`. |
| `~N samples/s` | An estimate derived from the shortest distance between two timestamps. |
