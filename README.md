# KAVL

Narrow-band digital video for radio: 360p15 in 12 kHz up to 4K120 with stereo sound in 1.25 MHz.

The KAVL app receives a KAVL signal from an RTL-SDR, a HackRF or an IQ recording and plays the picture and the sound. It also encodes a video file and transmits it with a HackRF, or writes the signal to an IQ file.

## Contents

- [Profiles](#profiles)
- [Install](#install) (Mac: from download to running, updating, problems)
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

Two narrow profiles go much further on little power, with NUC-64 10/15 (13.5 dB) and speech-grade mono sound:

| Profile | Width | Signal needed | Video | Sound | Delay | Picture after tuning |
|---|---|---|---|---|---|---|
| 360p30 | 24 kHz | 13.5 dB | 60 kbit/s | Opus mono 16 kbit/s | about 3.3 s | about 5 s |
| 360p15 | 12 kHz | 13.5 dB | 25 kbit/s | Opus mono 12 kbit/s | about 6.5 s | about 10 s |

Their frames last 1.4 s and 2.9 s on air, which sets the delay. A key frame comes every 4 s (more often would eat too much of so few bits). The receiver finds a narrow signal up to 20 kHz off its frequency, using the spectrum; picture quality at these rates is soft, best for faces and slow scenes.

## Install

KAVL has no ready-made download: you build it from this source code on your own computer. It takes one command and about 10 minutes the first time.

| Platform | Works? |
|---|---|
| Mac with Apple silicon (M1 to M4), macOS 12 or later | Yes, tested |
| Mac with an Intel processor, macOS 12 or later | Should work, not tested |
| Windows | Not yet |
| Linux | Not yet |

### Mac: from download to running

You need an internet connection and about 3 GB of free disk space. You never need to type your password into anything except macOS's own windows and Homebrew's installer.

**1. Open Terminal.** Press Cmd+Space, type `Terminal`, press Return. A window with a text prompt opens. Every command below is typed (or pasted) there and run with Return.

**2. Install Homebrew** (skip if you have it; if unsure, type `brew --version`: a version number means you have it).
Go to https://brew.sh, copy the one line under "Install Homebrew", paste it into Terminal and press Return. It asks for your Mac password (the letters do not show while you type; that is normal) and may install Apple's developer tools. At the end it prints "Next steps" with two or three commands: copy and run those too, then close Terminal and open it again.

**3. Download KAVL.** Paste this and press Return:

```bash
cd ~ && git clone https://github.com/adswill/kavl.git
```

This makes a folder `kavl` in your home folder. (If it says `git` needs the developer tools, click Install in the window that appears, wait until it finishes, then run the line again. Or download the ZIP from the green Code button on GitHub, double-click it, and rename the folder to `kavl` in your home folder.)

**4. Build and open KAVL.** Paste this and press Return:

```bash
cd ~/kavl && tools/install_mac.sh
```

It checks Apple's developer tools and Homebrew, installs the libraries KAVL needs (only missing ones; it upgrades nothing you have), builds the app and opens it. It prints four steps, `1/4` to `4/4`, and then `Done.` in green. If it stops, it says why in red and what to do; do that and run the same line again.

**5. Next time**, open KAVL by double-clicking `KAVL.app` in the `kavl/build` folder of your home folder (Finder: Go > Home), or with `open ~/kavl/build/KAVL.app`. You can drag it to the Dock to keep it there.

**Check that it works** without any radio: in KAVL, set Receive > Source to **Loopback**, Transmit > Programme to **Test pattern**, Transmit > Send to **Loopback**, and press Start transmitting. A moving pattern with a tone should appear in the middle within about 2 seconds.

**Radios.** Plug in an RTL-SDR or a HackRF over USB; no drivers to install on a Mac. The camera and microphone: macOS asks once whether KAVL may use them; click Allow. (Changed your mind: System Settings > Privacy & Security > Camera / Microphone.)

### Mac: updating

```bash
cd ~/kavl && git pull && tools/install_mac.sh
```

Quit KAVL first. This takes seconds when little changed. (Downloaded the ZIP instead: download the new ZIP, replace the `kavl` folder, run `cd ~/kavl && tools/install_mac.sh`.)

### Mac: removing

Drag the `kavl` folder from your home folder to the Bin. Settings are in `~/Library/Application Support/KAVL`, recordings in `Documents/KAVL`. The Homebrew libraries can stay (or `brew uninstall` them).

### Mac: when something goes wrong

| What you see | What to do |
|---|---|
| `zsh: command not found: brew` | Homebrew's "Next steps" were skipped. Run `eval "$(/opt/homebrew/bin/brew shellenv)"` (Intel Mac: `/usr/local/bin/brew`), then the command again. |
| `Permission denied` running `tools/install_mac.sh` | Run `chmod +x tools/install_mac.sh`, then the command again. |
| `No such file or directory` | You are not in the kavl folder: start the line with `cd ~/kavl &&` as shown. |
| The build failed | It printed the lines that say why, and the whole log is in `~/kavl/build/install.log`. Run `brew update && brew upgrade` and try again; if it still fails, open an issue on GitHub with that log. |
| KAVL opens but the camera picture stays black | System Settings > Privacy & Security > Camera: turn KAVL on, then quit and open KAVL again. |
| The RTL-SDR is not found | Unplug it and plug it in again, then press Refresh next to the source. |

### Windows and Linux

Not yet: the app uses macOS's camera, screen and audio frameworks, and has only been built on a Mac. Porting is planned.

### Building by hand (developers)

```bash
brew install cmake ninja pkgconf glfw dav1d opus hackrf libusb svt-av1 librtlsdr ffmpeg
cmake -S . -B build -G Ninja
cmake --build build
open build/KAVL.app
```

A built app uses Homebrew's `ffmpeg` (it runs it as a separate program) and `rtl_tcp` for RTL-SDR dongles. To make an app that carries its own LGPL FFmpeg and runs on a Mac without Homebrew, build FFmpeg once from the FFmpeg 8.0 source (https://ffmpeg.org/releases/ffmpeg-8.0.tar.xz) before building the app:

```bash
tools/build_ffmpeg.sh /path/to/ffmpeg-8.0
```

Each app build then copies `ffmpeg`, `ffprobe` and every library into KAVL.app and signs it ad hoc (`tools/bundle_app.sh`).

`-DKAVL_BUILD_APP=OFF` builds only the library, the tools and the tests. `-DKAVL_BUNDLE=OFF` skips the bundling.

## Use

One screen holds the whole station: **Receive** on the left, the received picture with the spectrum and waterfall in the middle, **Transmit** on the right, and a status line at the bottom (receiver, transmitter, delay, decoder speed). Each section folds away; View > Receive column / Transmit column (Cmd+1, Cmd+2) hides a whole column.

**Receive**

1. Choose the source: IQ file, RTL-SDR, HackRF, or Loopback (this Mac's own transmitter, no radio needed).
2. Leave the profile on Auto: the app reads the signal's width off the spectrum and finds the profile in about a second. Or choose it.
3. For a radio, set the frequency and the gain. For a file, open it or drop it on the window; a frequency in the file name (`..._433000000Hz.wav`) is read.
4. Press Start receiving.

The frequency, the gains, the amplifier, the bias tee and the ppm correction change at once while receiving. When the transmission stops and comes back (even with another profile, on Auto), the receiver picks it up by itself, with no need to stop and start. Record IQ saves what the receiver gets as an IQ `.wav` in Documents/KAVL. The Receive column shows the lock, the link margin above the profile's threshold, the constellation, the signal-to-noise ratio, the frequency and clock offsets, the code words received, and the delay.

**Low delay.** The picture starts 0.4 s after the first key frame and is then kept as close to the arrivals as they allow. Live from the camera's encoder to the screen measures about 0.75 s at 1440p60 (0.9 s at 720p, whose frames last longer). Protection adds one group of frames, because a group must arrive whole before it can be rebuilt: about 1.4 s for 1 in 16 at 1440p, 3.4 s at 720p. The transmitter sends its clock once a second, so a receiver on the same Mac (or on Macs with synced clocks) shows the delay.

**Spectrum and waterfall**

- Click: tune there. Drag: pan. Mouse wheel: zoom. Double-click: the whole span again.
- Drag the shaded band (the profile's width) to retune. A red dashed line marks this Mac's transmitter.
- Options above the plot: averaging, peak hold, automatic or set dB range, waterfall on or off. Drag the line between spectrum and waterfall to share the height.
- Hover for the frequency and level under the mouse.

| Key | Action |
|---|---|
| F or double-click the picture | Picture only |
| P, or Own window on the picture | The picture in a window of its own (there: F or double-click for full screen, Esc to close) |
| Esc | Back |
| F11 | Full screen |
| Cmd+1, Cmd+2 | Hide or show the Receive, the Transmit column |
| Cmd+O | Open an IQ file |
| [ and ] | Receive frequency 5 kHz down, up (Shift: 50 kHz) |
| Cmd+Z | Back to the frequency before the last retune |
| Tab, arrows, Space or Return | Move between the controls and press them (keyboard only) |
| Cmd+=, Cmd+-, Cmd+0 | Larger, smaller, normal text (also View > Text size) |

**Transmit**

1. Choose the programme:
   - **Video file**: drop it on the window or choose one, then press Encode for the profile.
   - **Camera (live)**: a camera and a microphone, encoded while transmitting. They are kept by name; the Mac's own camera and microphone are chosen unless you pick others. macOS asks once for access.
   - **Test pattern (live)**: a moving pattern and a 1 kHz tone, no camera needed.
2. Choose the profile.
3. Choose the protection: none, 1 frame in 16 or 2 frames in 32 (6.25 % of the bits), or 1 frame in 8 (12.5 %). Any lost frame of a group is rebuilt, from a burst of interference or a fade.
4. Choose where to send: HackRF (frequency, TX gain, amplifier), IQ file, or Loopback (into this Mac's receiver). Then start.

**Callsign.** Transmit > Callsign puts your callsign into the picture you send, so every receiver and every recording shows it (station identification). Drag it on the preview to place it, drag its corner or use the mouse wheel to size it, and choose the font (the app's own, or any TTF/OTF file), the colour and a dark edge. A video file carries it from Encode for this profile; change it and encode again.

The frequency, the TX gain and the amplifier change at once while transmitting. A video file repeats until you stop it. A live programme uses SVT-AV1 in real-time mode at the profile's bit rate. On an M2 Pro every profile encodes in real time, 4K120 included. The picture is softer than a slow file encode at the same bit rate. Transmit only where your licence and the band plan allow it.

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
- macOS only (Apple silicon tested); Windows and Linux are planned.
- Screen readers: the app draws its own controls (Dear ImGui), so VoiceOver cannot read them. Everything can be reached from the keyboard (see the keys above), and every status colour has a word next to it.

## Licence

KAVL Source Licence 1.0: you may read, build and run it for your own use and send changes back. Copying, redistributing or publishing changed versions needs written permission. See `LICENSE` and `NOTICE`. Third-party parts: `THIRD_PARTY.md`.

## Testing

```bash
tools/check.sh
```

One command, about 15 s: build, the fast tests (in parallel), and a 10 s run of the app itself, hidden, sending its test pattern to its own receiver. It fails when the receiver does not lock, no picture comes, or the delay is above 1.5 s (`MAX_DELAY`, `SECS`, `PROFILE` change that; `SKIP_APP=1` for build and tests only).

`ctest --test-dir build -j4 --output-on-failure` runs the tests alone. `KAVL_MEASURE=1 build/test_kavl_thresholds` measures the threshold of every modcod (slow).

Builds use ccache when it is installed, and the app bundle (ffmpeg, libraries, signing) is only redone when something in it changed.
