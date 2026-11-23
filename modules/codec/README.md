# Coral codec

The cross platform media codec library of the Coral project. The library exposes the platform
independent interfaces of the media framework, and every platform provides its own implementation
of them:

| Interface | Android | Windows | macOS / iOS |
| --- | --- | --- | --- |
| `AVMediaExtractor` (demuxer) | `MediaExtractor` / `NativeExtractor` | `WmfMediaExtractor` | `DarwinMediaExtractor` (AVAssetReader) |
| `AVMediaMuxer` (muxer) | `MediaMuxer` / `NativeMuxer` | `WmfMediaMuxer` | `DarwinMediaMuxer` (AVAssetWriter) |
| `AudioDecoder` / `AudioEncoder` | `MediaCodecAudioDecoder` / `MediaCodecAudioEncoder` | `WmfAudioDecoder` / `WmfAudioEncoder` | `DarwinAudioDecoder` / `DarwinAudioEncoder` (AudioConverter) |
| `VideoDecoder` / `VideoEncoder` | `MediaCodecVideoDecoder` / `MediaCodecVideoEncoder` | `WmfVideoDecoder` / `WmfVideoEncoder` | `DarwinVideoDecoder` / `DarwinVideoEncoder` (VideoToolbox) |
| `MediaMetadataRetriever` | `ExtractorMetadataRetriever` (data source + extractor) | same | same |
| `AudioFileReader` | `NdkAudioFileReader` | `WmfAudioFileReader` | `DarwinAudioFileReader` (ExtAudioFile) |
| `AudioFileWriter` | `AudioMuxFileWriter` (muxer) | `WmfAudioFileWriter` (sink writer) | `DarwinAudioFileWriter` (ExtAudioFile) |

## The metadata retriever

`MediaMetadataRetriever` is the one interface of the library that has no platform implementation:
`src/common/ExtractorMetadataRetriever.*` reads the media with the data sources and the extractors of
`libs/datasource` and `libs/extractor`, like the player does, which is the counterpart of
`androidx.media3.inspector.MetadataRetriever`. A media is therefore inspected by the uri it is played
with - a path, an http uri, and on Android an asset, a raw resource or the media of a content provider
- and the formats it reports are the ones of the reader, down to the identifier of a track. That is
what makes the track of an `AssetItem` the same track the reader serves.

## Implementation of the Windows platform

The Windows implementation is built on the Media Foundation framework (`mfplat`, `mfreadwrite`,
`mf`, `mfuuid`):

* `src/win/WmfMediaUtils.*` — the shared helpers: the Media Foundation lifetime, the conversion
  between the coral media types and the Media Foundation media types, and the creation of the
  synchronous Media Foundation transforms (MFT).
* `src/win/WmfTransform.*` — a thin wrapper of `IMFTransform`. Several decoders of the platform
  must not be polled with `ProcessOutput` while `GetOutputStatus` reports
  `MF_E_TRANSFORM_NEED_MORE_INPUT`, otherwise the decoded frames are kept inside the decoder.
* `src/win/WmfMediaExtractor.*` — the `AVMediaExtractor` based on `IMFSourceReader`. The reader is
  configured for pass through, so the compressed samples of the streams are delivered. The samples
  of the tracks are delivered interleaved by their presentation time.
* `src/win/WmfMediaMuxer.*` — the `AVMediaMuxer` based on `IMFSinkWriter`. The media types of the
  tracks are declared by `addTrack`, so the sink writer takes the already encoded packets of the
  tracks without inserting a further encoder. The samples are held back by a reorder window before
  they are written, because the sink writer drops the samples which are older than the samples it
  has already received. The mpeg4 and the 3gpp containers are supported.
* `src/win/WmfAudioDecoder.*`, `src/win/WmfAudioEncoder.*`, `src/win/WmfVideoDecoder.*`,
  `src/win/WmfVideoEncoder.*` — the codecs based on the Media Foundation transforms. The available
  codecs are the ones the platform provides, for example AAC, MP3, AC3 and FLAC audio and H.264
  video. The audio/video samples are handed over in the Annex B format, which is the format the
  Media Foundation video decoders expect.
* `src/win/WmfAudioFileReader.*` — the `AudioFileReader` based on `IMFSourceReader`, the audio of
  the file is decoded to 16 bit PCM.
* `src/win/WmfAudioFileWriter.*` — the `AudioFileWriter` based on `IMFSinkWriter`. The sink writer
  muxes the samples into the container and inserts the audio encoder which is required by the file
  type, so the m4a/aac and the wav files are written without any further platform support.
  The `AudioMuxFileWriter` (the encoder of the platform combined with `WmfMediaMuxer`) is available
  as well.

## Implementation of the Apple platforms

macOS and iOS share one implementation, `src/darwin`, built on the system media frameworks:

* `src/darwin/DarwinMediaUtils.*` — the shared helpers: the conversion between the coral codec
  types and the CoreMedia/AudioToolbox identifiers, the creation of a `CMFormatDescription` from a
  `MediaFormat` (and back), the codec specific data handling, and the NAL unit helpers.
  The coral convention is the one of the built-in extractor: H.264 parameter sets are stored one per
  entry as `[one byte length][NAL]`, HEVC stores its `hvcC` record (including the prefix and suffix
  SEI units it may carry, so the HDR metadata of the stream survives), and AAC stores the
  `AudioSpecificConfig` rather than the ESDS descriptor CoreMedia reports. H.264 has no extensions
  parameter in `CMVideoFormatDescriptionCreateFromH264ParameterSets`, so the description is built by
  `CMVideoFormatDescriptionCreate` with the `avcC` atom in the sample description extension atoms
  and the color metadata beside it; HEVC uses the creation function that takes the extensions
  directly.
* `src/darwin/DarwinMediaExtractor.*` — the `AVMediaExtractor` based on `AVAssetReader`. The track
  output is configured with `outputSettings = nil`, so the reader passes the compressed samples
  through instead of decoding them. The audio packets of the sample buffers are split back into the
  packets the container stored (the reader coalesces them), the samples of the tracks are delivered
  interleaved by presentation time, and each H.264/HEVC packet is converted to Annex B for the
  decoders that consume the framework's samples. Seeking recreates the reader with a time range,
  because `AVAssetReader` cannot be repositioned after it started.
* `src/darwin/DarwinMediaMuxer.*` — the `AVMediaMuxer` based on `AVAssetWriter`. The writer input
  carries the `CMFormatDescription` of the track and no output settings, so the writer takes the
  already encoded packets and inserts no further encoder. The mpeg4 and the 3gpp containers are
  supported. The encoded packets are written in the four byte length prefixed (AVCC) form, whether
  the caller produces Annex B or AVCC, and an existing output file is removed first because
  `AVAssetWriter` refuses to replace it.
* `src/darwin/DarwinAudioDecoder.*`, `src/darwin/DarwinAudioEncoder.*` — the codecs based on
  `AudioConverter`. The decoders cover the compressed audio codecs the platform provides (AAC, MP3,
  AC3, ALAC, FLAC, Opus, AMR, ...), and the encoders cover AAC and ALAC. An AAC stream is decoded
  with the ESDS magic cookie `AudioConverter` requires; the coral format keeps the
  `AudioSpecificConfig`, and the ESDS is rebuilt from it at the platform boundary. The input
  procedure hands over every queued packet in one call, which is the shape `AudioConverter` expects
  for externally framed compressed input.
* `src/darwin/DarwinVideoDecoder.*`, `src/darwin/DarwinVideoEncoder.*` — the codecs based on
  VideoToolbox. The decoder creates a `VTDecompressionSession` with
  `kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder`, so H.264/HEVC are decoded by
  the hardware decoder whenever the platform has one, and the session falls back to the software
  decoder of the same codec otherwise. The encoder is a `VTCompressionSession` with frame
  reordering disabled, which keeps decode order and presentation order the same and lets the muxer
  write the packets in the order it receives them.
* `src/darwin/DarwinHardwarePixelBuffer.*` — the zero-copy frame. When the decoder is configured
  with a `HardwareVideoSurface`, the decompression session emits `CVPixelBuffer`s backed by an
  `IOSurface`, and the decoder hands them out as a `HardwarePixelBuffer` instead of copying them to
  a CPU buffer. `getNativeTexture()` returns the `CVPixelBufferRef` (what
  `CVMetalTextureCacheCreateTextureFromImage` takes), and `asPixelBufferHandle()` wraps the same
  buffer as a `PixelBuffer` whose storage is `IOSurface`, so a Metal/OpenGL consumer imports the
  surface without a copy. A decoder that cannot decode in hardware, or that was configured without a
  hardware surface, still produces the same frames as CPU `NV12PixelBuffer`s.
* `src/darwin/DarwinAudioFileReader.*` — the `AudioFileReader` based on `ExtAudioFile`, decoding the
  audio of the file to 16 bit PCM.
* `src/darwin/DarwinAudioFileWriter.*` — the `AudioFileWriter` based on `ExtAudioFile`, writing the
  m4a/aac, flac and wav files without any further platform support.

The two `.mm` translation units (the AVFoundation extractor and muxer) are Objective-C++ and are
compiled with ARC; CMake enables the `OBJCXX` language on Apple platforms for them. Everything else
is plain C++ against the C interfaces of the system frameworks, so the same sources build for macOS
and iOS.

### The zero-copy frame path

A decoder that is configured with a `HardwareVideoSurface` produces `HardwarePixelBuffer` frames
instead of `NV12PixelBuffer` frames. The surface only has to say that it is valid; VideoToolbox does
not need a Metal device to allocate the `CVPixelBuffer`s, so `getNativeDevice()` may return null:

```cpp
class MetalHardwareSurface final : public coral::HardwareVideoSurface {
public:
  [[nodiscard]] void* getInstance() const override { return nullptr; }
  [[nodiscard]] void* getNativeDevice() const override { return device; } // or null
  [[nodiscard]] bool isValid() const override { return true; }
  void release() override {}
};

auto decoder = coral::CreateBuiltinVideoCodecFactory()->createVideoDecoder(
  coral::VideoCodecType::kVideoCodecTypeH264, coral::CodecManufacturer::kHardware);
decoder->configure(trackFormat, std::make_shared<MetalHardwareSurface>());

auto frame = decoder->dequeueOutputBuffer();
if (auto hardware = std::dynamic_pointer_cast<coral::HardwarePixelBuffer>(frame)) {
  // The decoded frame is a CVPixelBuffer; no pixels were copied.
  auto* pixelBuffer = static_cast<CVPixelBufferRef>(hardware->getNativeTexture());
  // The same buffer as a backend-neutral handle whose storage is an IOSurface.
  auto handle = hardware->asPixelBufferHandle();
}
```

A frame that a hardware decoder cannot produce -- a stream the platform decodes in software, or a
decoder configured without a surface -- arrives as a CPU `NV12PixelBuffer` instead, so a caller can
always consume the frame and only loses the zero-copy property.

### Building on Apple platforms

The codec is built as part of the repository; the Apple specifics are the `OBJCXX` language and the
system frameworks, both of which the CMake files add themselves:

```shell
# macOS
cmake -S . -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8

# iOS simulator (full Xcode, not just the command line tools)
cmake -S . -B build-ios -G "Unix Makefiles" \
      -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphonesimulator \
      -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 \
      -DCMAKE_BUILD_TYPE=Release -DIS_HOST_PLATFORM=OFF -DCORAL_BUILD_TESTS=OFF
cmake --build build-ios -j8
```

## Samples

The demos in the `samples` directory are built with the library and can be run directly:

```shell
codec_media_info       [media file]   # creates a demo m4a file when no file is given and prints its tracks
codec_audio_transcode  [file.m4a]     # encodes a sine signal into an m4a file and decodes it again
codec_video_pipeline   [file.h264]    # encodes synthetic frames into h264 and decodes them again
codec_muxer            [file.m4v]     # encodes, muxes and demuxes audio and video, reports the tracks
```

## Tests

The gtest based tests of the `tests` directory are registered with CTest:

```shell
cmake --build <build dir> --config Debug --target tests_codec
ctest --test-dir <build dir>/libs/codec -C Debug --output-on-failure
```
