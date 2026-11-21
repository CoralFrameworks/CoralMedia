# Coral codec

The cross platform media codec library of the Coral project. The library exposes the platform
independent interfaces of the media framework, and every platform provides its own implementation
of them:

| Interface | Android | Windows |
| --- | --- | --- |
| `AVMediaExtractor` (demuxer) | `MediaExtractor` / `NativeExtractor` | `WmfMediaExtractor` |
| `AVMediaMuxer` (muxer) | `MediaMuxer` / `NativeMuxer` | `WmfMediaMuxer` |
| `AudioDecoder` / `AudioEncoder` | `MediaCodecAudioDecoder` / `MediaCodecAudioEncoder` | `WmfAudioDecoder` / `WmfAudioEncoder` |
| `VideoDecoder` / `VideoEncoder` | `MediaCodecVideoDecoder` / `MediaCodecVideoEncoder` | `WmfVideoDecoder` / `WmfVideoEncoder` |
| `MediaMetadataRetriever` | `ExtractorMetadataRetriever` (data source + extractor) | same |
| `AudioFileReader` | `NdkAudioFileReader` | `WmfAudioFileReader` |
| `AudioFileWriter` | `AudioMuxFileWriter` (muxer) | `WmfAudioFileWriter` (sink writer) |

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
