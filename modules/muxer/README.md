# muxer

`libs/muxer` writes encoded audio/video samples into media containers. It is the native replacement
for the platform muxers used by `AssetWriter`.

## MP4 support

The MP4 writer implements the milestones of [docs/DESIGN.md](docs/DESIGN.md):

- M0: seekable `ftyp` + 64-bit `mdat` + `moov`, H.264/H.265/AAC, B-frame `ctts`, `stss`/`stsz`/
  `stsc`/`stco`/`co64`, `avcC`/`hvcC`/`esds`, `mdta` metadata;
- M1: edit lists for negative or delayed starts, fast-start (`moov` before `mdat`, including
  non-seekable outputs via a temporary spool), location/orientation/timestamp/XMP metadata, bounded
  interleaving and real chunk tables;
- M2: fragmented MP4 (`moof`/`mfhd`/`tfhd`/`tfdt`/`trun`/`mvex`/`trex`, key-frame fragment
  boundaries, 64-bit `mdat`);
- M3 (MP4 subset): AV1 (`av01`/`av1C`), VP9 (`vp09`/`vpcC`), MPEG-4 Visual (`mp4v`/`esds`), H.263
  (`s263`/`d263`), Opus (`Opus`/`dOps`), FLAC (`fLaC`/`dfLa`), AC-3 (`ac-3`/`dac3`), E-AC-3
  (`ec-3`/`dec3`) and raw PCM (`sowt`); Dolby Vision profiles 5/8/9 (`dvcC`/`dvvC`); `colr`,
  `mdcv` and `clli` from `MediaFormat::getColorInfo()`; language, rotation, `tref/cdsc`
  auxiliary-track references, CEA-608/708 caption tracks (`c608`/`c708` sample entries, `clcp`,
  `cdat` samples), the MP4 text tracks (`tx3g`, `wvtt`, `stpp`), `chpl` chapters and MP4-AT `axte`
  auxiliary tracks (`MuxerOptions::auxiliaryTracksInAxte`). A track derived from another one (e.g.
  captions that the extractor derives from the SEI of a video track) is marked on its `MediaFormat`
  and rejected by `addTrack`, so a remux does not write the same captions twice. A subtitle or
  metadata track whose format the container cannot represent (e.g. Matroska PGS or VobSub) is
  skipped by a remux rather than failing the file.
- M3 (containers): ADTS (`Container::kAdts`), RIFF/WAVE PCM/float (`Container::kWav`), WebM
  (`Container::kWebM`, VP8/VP9/AV1 video + Opus/Vorbis audio) and Ogg (`Container::kOgg`,
  Opus/Vorbis/FLAC audio).

The native writer is the default for `AssetWriter` on Windows and Android. The platform muxers
remain available for comparison and rollback with `CORAL_MUXER=platform`.

## Integration

`AssetWriter` uses the native writer by default. To route it through the platform muxer instead:

```powershell
$env:CORAL_MUXER = "platform"
```

`CORAL_MUXER=mp4` and `CORAL_MUXER=native` are accepted as explicit native aliases. See
[docs/DESIGN.md](docs/DESIGN.md) for the feature scope, the Media3 comparison, the API contract, the
integration plan and the milestones,
[docs/ANDROID_COMPARISON.md](docs/ANDROID_COMPARISON.md) for the device comparison results, and
[docs/EXPORT_MATRIX.md](docs/EXPORT_MATRIX.md) for the switch validation matrix.

## Build and test

```powershell
cmake --build cmake-build-debug --target test_muxer
cmake-build-debug/coral-libs/libs/muxer/tests/test_muxer.exe
```

Set `CORAL_KEEP_MUXER_OUTPUT=1` to keep the files the end-to-end tests write, then inspect them with
`ffprobe`. The tests cover H.264/H.265 remuxing, B-frame timestamps, fast-start (including a
non-seekable memory sink), edit lists, fragmented MP4, interleaved chunks, typed metadata,
HDR/color and Dolby Vision remuxing, the MP4 codec sample-entry matrix, CEA-608/708 caption tracks,
auxiliary-track references, MP4-AT `axte` (also combined with non-seekable fast-start), chapters,
ADTS, WAV, WebM and Ogg.

The export matrix is validated with the `test_compose` suite in both modes:

```powershell
# native default
cmake-build-debug/coral-libs/coral/compose/tests/test_compose.exe

# platform opt-in
$env:CORAL_MUXER = "platform"
cmake-build-debug/coral-libs/coral/compose/tests/test_compose.exe
```

Both runs execute the same export, composition, range, audio-only and muxer-comparison tests.
