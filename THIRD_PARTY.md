# Third-party parts

KAVL's own code is under the KAVL Source Licence (`LICENSE`). These parts come from others and keep their own licences.

## In this repository

| Part | Licence | Where | Full text |
|---|---|---|---|
| Dear ImGui 1.92.2b | MIT, Copyright (c) 2014-2025 Omar Cornut | `third_party/imgui` | `third_party/imgui/LICENSE.txt` |
| miniaudio | Public domain (Unlicense) or MIT No Attribution, David Reid | `third_party/miniaudio` | end of `third_party/miniaudio/miniaudio.h` |
| Inter 4.1 (subset) | SIL Open Font License 1.1, Copyright (c) 2016 The Inter Project Authors | `third_party/fonts` | `third_party/fonts/OFL-1.1.txt` |
| JetBrains Mono 2.304 (subset) | SIL Open Font License 1.1, Copyright 2020 The JetBrains Mono Project Authors | `third_party/fonts` | `third_party/fonts/OFL-1.1.txt` |

## Libraries the app links (copied unchanged into KAVL.app/Contents/Frameworks by `tools/bundle_app.sh`)

| Library | Licence | Used for |
|---|---|---|
| GLFW | zlib | window, keyboard, mouse |
| dav1d | BSD 2-clause | AV1 video decoding |
| libopus | BSD 3-clause | Opus sound decoding |
| libhackrf | BSD 3-clause | HackRF receive and transmit |
| libusb | LGPL 2.1 or later, linked dynamically (replaceable in the bundle) | listing RTL-SDR dongles |
| SVT-AV1 (libSvtAv1Enc) | BSD 3-clause Clear, plus the Alliance for Open Media patent licence 1.0 | AV1 encoding (used by the bundled ffmpeg) |

## Programs the app runs (separate programs, not part of KAVL)

| Program | Licence | Used for |
|---|---|---|
| rtl_tcp (librtlsdr) | GPL 2 or later | RTL-SDR dongles: the app starts it and reads its IQ over a local network socket |
| FFmpeg 8.0 (`ffmpeg`, `ffprobe`) | LGPL 2.1 or later | encoding a video file or the camera for transmission. Bundled in KAVL.app/Contents/MacOS as separate programs. |

The bundled FFmpeg is built by `tools/build_ffmpeg.sh` from the unchanged FFmpeg 8.0 source (https://ffmpeg.org/releases/ffmpeg-8.0.tar.xz) with one line changed for SVT-AV1 4 (`enable_adaptive_quantization` is now `aq_mode`; the script makes the change). It is built without `--enable-gpl` and without `--enable-nonfree`, so it is LGPL only. The script shows exactly how it was configured; with it and the source anyone can rebuild it and swap it in. The LGPL text is in KAVL.app/Contents/Resources/licences.
