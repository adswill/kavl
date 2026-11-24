#!/usr/bin/env bash
# Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
# Use is governed by the KAVL Source Licence (LICENSE).

# KAVL video budget measurement (runs locally; owner 2026-11-23): how good does 1440p60 look at the bit rates the 500 kHz link carries?
# Makes a 1440p60 reference from a test clip, encodes it with SVT-AV1 at several bit rates and presets, and scores each with VMAF (and PSNR).
#   kavl/video/measure.sh <ffmpeg> <clip url> <start s> <seconds> <out dir> [rates in kbit/s ...]
# Prints a markdown table; the encodes stay in <out dir> for a look.
set -euo pipefail
FF=$1; URL=$2; SS=$3; DUR=$4; OUT=$5; shift 5
RATES=${*:-"1500 2000 2400 3000 4000"}
mkdir -p "$OUT"
REF="$OUT/ref_1440p60.y4m"
"$FF" -hide_banner -loglevel error -y -ss "$SS" -t "$DUR" -i "$URL" -an -vf "scale=2560:1440:flags=lanczos,fps=60,format=yuv420p" -strict -1 "$REF"
echo "reference: $(du -h "$REF" | cut -f1), $DUR s"
echo
echo "| Encoder settings | Target (kbit/s) | Actual (kbit/s) | VMAF mean | VMAF 1% low | PSNR-Y (dB) | Encode speed (fps) |"
echo "|---|---|---|---|---|---|---|"
score() {   # score <encoded file> <label> <target>
    local f=$1 label=$2 tgt=$3
    local bytes; bytes=$(wc -c < "$f" | tr -d " ")
    local kbps; kbps=$(awk -v b="$bytes" -v d="$DUR" 'BEGIN{printf "%.0f", b*8/d/1000}')
    "$FF" -hide_banner -loglevel error -i "$f" -i "$REF" -lavfi "[0:v]setpts=PTS-STARTPTS[d];[1:v]setpts=PTS-STARTPTS[r];[d][r]libvmaf=log_fmt=json:log_path=$f.vmaf.json:n_threads=3:feature=name=psnr" -f null - 2>/dev/null
    python3 - "$f.vmaf.json" "$label" "$tgt" "$kbps" "$4" <<'EOF'
import json,sys
j=json.load(open(sys.argv[1]))
v=sorted(fr["metrics"]["vmaf"] for fr in j["frames"])
p=[fr["metrics"].get("psnr_y",0) for fr in j["frames"]]
low=v[max(0,len(v)//100-1)] if v else 0
print(f"| {sys.argv[2]} | {sys.argv[3]} | {sys.argv[4]} | {sum(v)/len(v):.1f} | {low:.1f} | {sum(p)/len(p):.2f} | {sys.argv[5]} |")
EOF
}
for preset in 4 2; do
  for r in $RATES; do
    f="$OUT/svt_p${preset}_${r}k.ivf"
    t0=$(python3 -c "import time;print(time.time())")
    "$FF" -hide_banner -loglevel error -y -i "$REF" -c:v libsvtav1 -preset "$preset" -b:v "${r}k" -g 240 \
      -svtav1-params "lp=3:rc=1:tune=0:enable-overlays=1:scd=1" "$f"
    t1=$(python3 -c "import time;print(time.time())")
    fps=$(awk -v a="$t0" -v b="$t1" -v d="$DUR" 'BEGIN{printf "%.1f", d*60/(b-a)}')
    score "$f" "SVT-AV1 preset $preset, VBR, tune VQ" "$r" "$fps"
  done
done
# film grain synthesis on the slowest preset at the link rate
for r in $RATES; do
  f="$OUT/svt_p2_fg_${r}k.ivf"
  t0=$(python3 -c "import time;print(time.time())")
  "$FF" -hide_banner -loglevel error -y -i "$REF" -c:v libsvtav1 -preset 2 -b:v "${r}k" -g 240 \
    -svtav1-params "lp=3:rc=1:tune=0:enable-overlays=1:scd=1:film-grain=8:film-grain-denoise=1" "$f"
  t1=$(python3 -c "import time;print(time.time())")
  fps=$(awk -v a="$t0" -v b="$t1" -v d="$DUR" 'BEGIN{printf "%.1f", d*60/(b-a)}')
  score "$f" "SVT-AV1 preset 2, VBR, film grain 8" "$r" "$fps"
done
