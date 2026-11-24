#!/usr/bin/env bash
# Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
# Use is governed by the KAVL Source Licence (LICENSE).

# One demo IQ .wav per KAVL video profile (kavl_avtx --profile N) from a master clip, each checked by decoding it back (kavl_decwav: the received AV1
# and Opus must equal what was sent).
#   kavl/video/make_demos.sh <master clip> <start s> <out dir> <kavl build dir>
# One encode per profile at its video budget (kavl_avtx lists them): SVT-AV1 preset 2 (4K: preset 5), key frame every 4 s,
# VBR; Opus 32 kbit/s stereo.
set -euo pipefail
SRC=$1; SS=$2; OUT=$3; B=$4
mkdir -p "$OUT"
enc() {   # enc <name> <seconds> <vf> <fps> <preset> <kbit/s>
    local name=$1 dur=$2 vf=$3 fps=$4 preset=$5 kbps=$6
    [ -s "$OUT/$name.mkv" ] && return 0
    nice -n 19 ffmpeg -hide_banner -loglevel error -y -ss "$SS" -t "$dur" -i "$SRC" -vf "$vf" -c:v libsvtav1 -preset "$preset" -b:v "${kbps}k" \
        -bufsize "$((kbps * 3 / 2))k" -g "$((fps * 4))" -svtav1-params "lp=4:rc=1:tune=0" -c:a libopus -b:a 32k -ac 2 -ar 48000 "$OUT/$name.mkv"
    ffmpeg -hide_banner -loglevel error -y -i "$OUT/$name.mkv" -map 0:v -c copy -f ivf "$OUT/$name.ivf"
    ffmpeg -hide_banner -loglevel error -y -i "$OUT/$name.mkv" -map 0:a -c copy -f ogg "$OUT/$name.ogg"
}
air() {   # air <profile> <encode name> <label>
    local p=$1 name=$2 label=$3
    local wav="$OUT/kavl_${label}_433000000Hz.wav"
    nice -n 19 "$B/kavl_avtx" --profile "$p" --video "$OUT/$name.ivf" --audio "$OUT/$name.ogg" --snr 30 --freq 433000000 --out "$wav" | tail -1
    local sr; sr=$("$B/kavl_avtx" | awk -v p="$p" '$1 == p {for (i = 1; i <= NF; i++) if ($i == "ksym/s") print $(i - 1) * 1000}')
    nice -n 19 "$B/kavl_decwav" "$wav" --symrate "$sr" --ivf "$OUT/rx.ivf" --ogg "$OUT/rx.ogg" | tail -2
    python3 - "$OUT/$name.ivf" "$OUT/rx.ivf" <<'EOF'
import struct, sys
def frames(p):
    d = open(p, 'rb').read(); q = struct.unpack('<H', d[6:8])[0]; o = []
    while q + 12 <= len(d):
        s = struct.unpack('<I', d[q:q + 4])[0]; o.append(d[q + 12:q + 12 + s]); q += 12 + s
    return o
a, b = frames(sys.argv[1]), frames(sys.argv[2])
print(f"  video {len(b)}/{len(a)} frames, {'bit-identical' if a == b else 'DIFFERENT'}")
EOF
}
L="scale=%s:flags=lanczos,fps=60,format=yuv420p"
enc v360p15 30 "scale=640:360:flags=lanczos,fps=15,format=yuv420p" 15 2 45
enc v360p30 30 "scale=640:360:flags=lanczos,fps=30,format=yuv420p" 30 2 108
enc v720 30 "$(printf "$L" 1280:720)" 60 2 281
enc v1080 30 "$(printf "$L" 1920:1080)" 60 2 480
enc v1440 30 "$(printf "$L" 2560:1440)" 60 2 772
enc v2160 15 "$(printf "$L" 3840:2160)" 60 5 1166
air 0 v360p15 360p15_135kHz
air 1 v360p30 360p30_230kHz
air 2 v720 720p60_500kHz
air 3 v1080 1080p60_800kHz
air 4 v1440 1440p60_930kHz
air 5 v2160 4K60_1MHz
rm -f "$OUT/rx.ivf" "$OUT/rx.ogg"
