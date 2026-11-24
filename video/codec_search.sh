#!/usr/bin/env bash
# Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
# Use is governed by the KAVL Source Licence (LICENSE).

# KAVL codec search: which SVT-AV1 settings give the reference picture quality (VMAF, PSNR-Y) at the fewest bits?
#   kavl/video/codec_search.sh <master clip> <start s> <seconds> <out dir> "<label>|<ffmpeg video options>" ...
# Every encode starts from the same master frames (scaled to 1440p60 with lanczos) and is scored against them. Prints a markdown table.
set -uo pipefail
SRC=$1; SS=$2; DUR=$3; OUT=$4; shift 4
mkdir -p "$OUT"
VF="scale=2560:1440:flags=lanczos,fps=60,format=yuv420p"
echo "| Settings | Video kbit/s | VMAF mean | VMAF 1% low | PSNR-Y dB | Encode fps |"
echo "|---|---|---|---|---|---|"
i=0
for spec in "$@"; do
    label=${spec%%|*}; opts=${spec#*|}; i=$((i + 1))
    f="$OUT/enc_$i.ivf"
    t0=$(python3 -c "import time;print(time.time())")
    # shellcheck disable=SC2086
    nice -n 19 ffmpeg -hide_banner -loglevel error -y -ss "$SS" -t "$DUR" -i "$SRC" -an -vf "$VF" -c:v libsvtav1 $opts -f ivf "$f" || { echo "| $label | encode failed |"; continue; }
    t1=$(python3 -c "import time;print(time.time())")
    bytes=$(wc -c < "$f" | tr -d " ")
    kbps=$(awk -v b="$bytes" -v d="$DUR" 'BEGIN{printf "%.0f", b*8/d/1000}')
    fps=$(awk -v a="$t0" -v b="$t1" -v d="$DUR" 'BEGIN{printf "%.1f", d*60/(b-a)}')
    nice -n 19 ffmpeg -hide_banner -loglevel error -i "$f" -ss "$SS" -t "$DUR" -i "$SRC" -lavfi \
        "[0:v]setpts=PTS-STARTPTS[d];[1:v]$VF,setpts=PTS-STARTPTS[r];[d][r]libvmaf=log_fmt=json:log_path=$f.json:n_threads=4:feature=name=psnr" -f null - 2>/dev/null
    python3 - "$f.json" "$label" "$kbps" "$fps" <<'EOF'
import json, sys
j = json.load(open(sys.argv[1]))
v = sorted(fr["metrics"]["vmaf"] for fr in j["frames"])
p = [fr["metrics"].get("psnr_y", 0) for fr in j["frames"]]
low = v[max(0, len(v) // 100 - 1)]
print(f"| {sys.argv[2]} | {sys.argv[3]} | {sum(v)/len(v):.2f} | {low:.2f} | {sum(p)/len(p):.2f} | {sys.argv[4]} |", flush=True)
EOF
done
