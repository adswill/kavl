# Hybrid digital + analog-style fallback: simulations

Three demo renders used to judge a fallback that keeps showing a picture below the digital threshold (2026-11-24). They model KAVL's 1080p60 profile (440 kHz, QPSK 6/15) with KAVL's measured decode rates, and an analog-style layer on 15 % of the channel that sends the picture's strongest 3-D DCT coefficients as plain signal values (SoftCast-style, linear least-squares at the receiver). They assume frame sync holds down to -6 dB.

| Script | Shows |
|---|---|
| `demo_digital_vs_hybrid.py` | digital only against the hybrid, +5 to -6 dB |
| `demo_fallback_first_vs_improved.py` | the first fallback against the improved one: brightness before colour and groups of 10 frames at the transmitter; noise averaged over still areas and the last sharp digital frame's detail laid in where it still matches, at the receiver |
| `demo_smart_vs_raw_pixels.py` | the improved fallback against raw pixels (72x40, analog-TV style) at the same share |

Findings: the improved fallback keeps faces and shapes recognisable to about -3 dB; raw pixels drown in noise below 0 dB, because every pixel gets the same small share of the power, while the DCT layer puts it into what matters most.

```bash
pip install numpy pillow
ffmpeg -ss 20 -t 36 -i master.mp4 -an -vf scale=1920:1080,fps=60 -c:v libsvtav1 -preset 4 -b:v 251k -bufsize 376k -g 120 digital.mkv
python3 demo_digital_vs_hybrid.py master.mp4 digital.mkv out.mp4 ../../third_party/fonts/Inter-Regular.ttf ../../third_party/fonts/Inter-SemiBold.ttf
```
