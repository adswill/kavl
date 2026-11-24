# KAVL

Narrow-band digital video for radio: 720p60 to 4K120 with stereo sound, in 160 kHz to 1.25 MHz.

The KAVL app receives a KAVL signal from an RTL-SDR, a HackRF or an IQ recording and plays the picture and the sound. It also encodes a video file and transmits it with a HackRF, or writes the signal to an IQ file.

## Contents

- [Profiles](#profiles)
- [Requirements](#requirements)
- [Build](#build)
- [Use](#use)
- [Tools](#tools)
- [How it works](#how-it-works)
- [Limits](#limits)
- [Licence](#licence)
- [Testing](#testing)

## Profiles

Each profile has a normal and a robust form. The signal needed is Es/N0, the signal-to-noise ratio of the symbols, measured with 99 % of the code words intact.

| Profile | Width | Signal needed | Width (robust) | Signal needed (robust) |
|---|---|---|---|---|
| 720p60 | 160 kHz | 18 dB | 230 kHz | 13.5 dB |
| 1080p60 | 260 kHz | 18 dB | 400 kHz | 13.5 dB |
| 1440p60 | 400 kHz | 18 dB | 560 kHz | 13.5 dB |
| 4K60 | 650 kHz | 18 dB | 950 kHz | 13.5 dB |
| 4K120 | 870 kHz | 18 dB | 1.25 MHz | 13.5 dB |

Video is AV1, sound is Opus stereo at 48 kHz and 112 kbit/s. The video budget of each profile gives the same picture quality (VMAF) at every size: SVT-AV1 preset 2 with a key frame every 4 s.

## Requirements

macOS 12 or later (built and tested on Apple silicon).

```bash
brew install cmake ninja glfw dav1d opus hackrf libusb
```

- RTL-SDR dongles need `rtl_tcp`: `brew install librtlsdr`.
- Transmitting a video or the camera needs `ffmpeg` with SVT-AV1 and Opus: `brew install ffmpeg`.

## Build

```bash
cmake -S . -B build -G Ninja
cmake --build build
open build/KAVL.app
```

`-DKAVL_BUILD_APP=OFF` builds only the library, the tools and the tests.

## Use

**Receive**

1. Choose the source: IQ file, RTL-SDR or HackRF.
2. Leave the profile on Auto: the app finds the profile of the signal in about a second. Or choose it.
3. For a radio, set the frequency and the gain. For a file, open it or drop it on the window; a frequency in the file name (`..._433000000Hz.wav`) is read.
4. Press Start.

The picture plays after the first key frame and two seconds of buffer. Record IQ saves what the receiver gets as an IQ `.wav` in Documents/KAVL. The left panel shows the lock, the link margin above the profile's threshold, the constellation, the signal-to-noise ratio, the frequency and clock offsets and the code words received.

| Key | Action |
|---|---|
| F or double-click the picture | Picture only |
| Esc | Back |
| F11 | Full screen |
| Cmd+O | Open an IQ file |

**Transmit**

1. Choose the programme:
   - **Video file**: drop it on the window or choose one, then press Encode for the profile.
   - **Camera (live)**: a camera and a microphone, encoded while transmitting. macOS asks once for access.
   - **Test pattern (live)**: a moving pattern and a 1 kHz tone, no camera needed.
2. Choose the profile.
3. Choose the protection: none, 1 frame in 16 or 2 frames in 32 (6.25 % of the bits), or 1 frame in 8 (12.5 %). Any lost frame of a group is rebuilt, from a burst of interference or a fade.
4. Choose HackRF (frequency, TX gain, amplifier) or IQ file, then start.

A video file repeats until you stop it. A live programme uses SVT-AV1 in real-time mode at the profile's bit rate. On an M2 Pro every profile encodes in real time, 4K120 included. The picture is softer than a slow file encode at the same bit rate. Transmit only where your licence and the band plan allow it.

An RTL-SDR on the Receive tab can receive the HackRF's transmission at the same time.

## Tools

| Tool | Does |
|---|---|
| `kavl_avtx` | Video (AV1 `.ivf`) and sound (Opus `.ogg`) to a KAVL IQ `.wav`, for a profile (`--profile N`) |
| `kavl_decwav` | Decodes a KAVL IQ `.wav` (16-bit or float, any rate); writes the video and sound out with `--ivf` and `--ogg` |
| `kavl_iqwav` | A test signal with random payload |
| `video/make_demos.sh` | One demo per profile from a video clip, each checked by decoding it back |
| `video/codec_search.sh` | Encoder settings against picture quality (VMAF) |

`kavl_avtx` with no arguments lists the profiles and their video budgets.

## How it works

- Single carrier, root-raised-cosine roll-off 0.02.
- Frames of 32400 data symbols with a known pilot every 32 symbols, a 64-symbol preamble and a 140-symbol header.
- Error correction: ATSC 3.0 LDPC codes, 64800 bits, with non-uniform constellations (NUC-64 and NUC-256).
- The receiver finds frames by a differential preamble search that is blind to the frequency offset. It removes the offset before the matched filter, measures the clock drift from one preamble to the next and tracks the phase from the pilots.
- Audio and video packets run across the code words. Each code word points to the first packet that starts in it, so a lost code word costs only the packets in it.
- Optional protection across frames: Reed-Solomon over GF(256), groups of 8 to 32 frames with 1 or 2 parity frames. The frame header names the protection, so the receiver needs no setting. With 1 frame in 8, the 1440p60 profile still gives a perfect picture at 17.8 dB, where 2.5 % of the code words fail.

## Limits

- Tested with recordings and with simulated receivers: noise, frequency offset, clock offset, phase noise, IQ imbalance, DC offset and 8-bit samples. Not yet tested over the air.
- Without protection a lost code word loses its packets, and the picture repairs itself at the next key frame.
- The 4K120 demo is made from a 60 fps master by motion interpolation.
- macOS only.

## Licence

KAVL Source Licence 1.0: you may read, build and run it for your own use and send changes back. Copying, redistributing or publishing changed versions needs written permission. See `LICENSE` and `NOTICE`. Third-party parts: `THIRD_PARTY.md`.

## Testing

```bash
ctest --test-dir build --output-on-failure
```

`KAVL_MEASURE=1 build/test_kavl_thresholds` measures the threshold of every modcod (slow).
