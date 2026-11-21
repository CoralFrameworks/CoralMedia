# The extractor tests

The suite is a gtest executable, `test_extractor`, that covers the whole module:

| File | Coverage |
| ---- | -------- |
| `test_DataBuffer.cpp` | `AVDataBuffer`, the counterpart of `ParsableByteArray`. |
| `test_ExtractorInput.cpp` | `AVDefaultExtractorInput` and `AVMediaExtractorInput`, on an in memory data source of `libs/datasource`. |
| `test_Utility.cpp` | `AVUtil`, `AVBitReader` and the MPEG audio frame header. |
| `test_Reader.cpp` | `AVReader` end to end, i.e. the sniffing, the extraction, the seek map and the seek, for every container format. |
| `TestMedia.h` | The builders of the media that the tests use. |
| `test_main.cpp` | The `main` of the executable. |

## Build and run

```
cmake -S . -B build -G Ninja
cmake --build build --target test_extractor
build/libs/extractor/tests/test_extractor
```

When the build is run with `cmake --build build`, the test is also registered with ctest:

```
ctest --test-dir build -R test_extractor --output-on-failure
```

## The media builders

The tests never depend on a file on disk: every container is built in memory by `TestMedia.h`, which
makes the suite hermetic and lets a test assert on values that it chose itself, e.g. the exact
sample size of a frame.

| Builder | What it produces |
| ------- | ---------------- |
| `buildWav(sampleRate, channelCount, sampleCount, withListChunk)` | A PCM WAV file, optionally with a `LIST` chunk before the data chunk. |
| `buildMp3(frameCount, withId3)` | A chain of MPEG-1 layer 3 frames at 128 kbit/s and 44100 Hz, each of 417 bytes, optionally preceded by an ID3v2.3 tag. |
| `buildAdts(frameCount, sampleRateIndex, channelConfig)` | A chain of ADTS frames, each holding 64 bytes of payload. |
| `buildFlac(sampleRate, channelCount, bitsPerSample, totalSamples)` | A native FLAC stream: the `fLaC` marker, a STREAMINFO block and one frame. |
| `buildMp4(sampleCount)` | An unfragmented MP4 with one `mp4a` track of AAC LC, a complete sample table and an `mdat` box. |
| `buildOggVorbis(sampleRate, channels, audioPacketCount, packetSize, twoModes)` | An Ogg stream of Vorbis packets, one per page, whose headers declare a small block size of 256 samples and a large one of 2048. |
| `buildOggOpus(channels, audioPacketCount, packetSize)` | An Ogg stream of Opus packets of 20 milliseconds, one per page. |
| `buildOggFlac(sampleRate, channels, bitsPerSample, audioPacketCount, frameSize)` | An Ogg stream of FLAC frames of 4096 samples, with the STREAMINFO block of the codec in its mapping header. |
| `buildAviPcmAudio(sampleRate, channels, chunkCount, samplesPerChunk, formatTag)` | An AVI file with one audio stream of `chunkCount` chunks, one sample per chunk. The format tag selects an unsupported codec when a test asks for one. |
| `buildAviVideoAndAudio(compression, frameCount, frameSize)` | An AVI file with one video stream and one PCM audio stream, whose chunks are interleaved. The first frame is framed as a key frame. |

The fixtures are structurally valid but their payload is synthetic: the frames of the compressed
formats do not hold audio that a decoder could decode. The extractor module only parses the framing
of a container, therefore that is enough to exercise every code path of the parsers, and it keeps the
tests readable.
