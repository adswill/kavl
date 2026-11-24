# Third-party parts

KAVL's own code is under the KAVL Source Licence (`LICENSE`). These parts come from others and keep their own licences.

## In this repository

| Part | Licence | Where | Full text |
|---|---|---|---|
| Dear ImGui 1.92.2b | MIT, Copyright (c) 2014-2025 Omar Cornut | `third_party/imgui` | `third_party/imgui/LICENSE.txt` |
| miniaudio | Public domain (Unlicense) or MIT No Attribution, David Reid | `third_party/miniaudio` | end of `third_party/miniaudio/miniaudio.h` |
| Inter 4.1 (subset) | SIL Open Font License 1.1, Copyright (c) 2016 The Inter Project Authors | `third_party/fonts` | `third_party/fonts/OFL-1.1.txt` |
| JetBrains Mono 2.304 (subset) | SIL Open Font License 1.1, Copyright 2020 The JetBrains Mono Project Authors | `third_party/fonts` | `third_party/fonts/OFL-1.1.txt` |

## Libraries the app links (installed separately, not included)

| Library | Licence | Used for |
|---|---|---|
| GLFW | zlib | window, keyboard, mouse |
| dav1d | BSD 2-clause | AV1 video decoding |
| libopus | BSD 3-clause | Opus sound decoding |
| libhackrf | BSD 3-clause | HackRF receive and transmit |
| libusb | LGPL 2.1 or later, linked dynamically | listing RTL-SDR dongles |

## Programs the app runs (separate programs, not part of KAVL)

| Program | Licence | Used for |
|---|---|---|
| rtl_tcp (librtlsdr) | GPL 2 or later | RTL-SDR dongles: the app starts it and reads its IQ over a local network socket |
| ffmpeg | LGPL / GPL, depending on the build | encoding a video for transmission (Transmit tab, `kavl/video` scripts) |
