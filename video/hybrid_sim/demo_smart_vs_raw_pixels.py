# Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
# Use is governed by the KAVL Source Licence (LICENSE).
#
# The improved fallback against raw pixels sent analog-TV style, at the same share of the channel.
# A simulation for judging the idea, not part of the radio. Needs numpy and Pillow (pip install numpy pillow) and ffmpeg with libsvtav1.
# Usage: python3 <this file> <master clip> <digital AV1 .mkv (1080p60, ~251 kbit/s, key frame every 2 s)> <out .mp4> <regular .ttf> <bold .ttf>
import subprocess, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont

SRC, DIG, OUT, FONT, FONT_B = sys.argv[1:6]
FPS, SEG = 30, 3
STEPS = [5, 4, 3, 2, 1, 0, -1, -2, -3, -4, -5, -6]
N = len(STEPS) * SEG * FPS
PW, PH = 960, 540            # each panel
AW, AH, T = 192, 108, 5      # the analog-style layer: 192x108, 3-D DCT over 5 frames
SYM = 440e3 / 1.02           # the 1080p60 profile's symbol rate
SHARE = 0.15                 # share of the channel for the analog-style layer
K = int(SHARE * SYM * 2 * T / FPS)   # real values it can send per group of T frames
CH = 12                      # chunks of 12x12 DCT coefficients (per plane, per temporal index)
KEEP = K // (CH * CH)
FRAME_S = 33618 / SYM        # one KAVL frame on air
KEY_S = 2.0                  # key frame every 2 s (live)
# measured share of code words decoded (QPSK 6/15, KAVL receiver, noise channel)
def p_ok(snr):
    return 1.0 if snr >= 1 else 0.98 if snr >= 0.5 else 0.73 if snr >= 0 else 0.0

def dctm(n):
    k = np.arange(n)[:, None]; i = np.arange(n)[None, :]
    m = np.cos(np.pi * (2 * i + 1) * k / (2 * n)) * np.sqrt(2.0 / n)
    m[0] /= np.sqrt(2.0)
    return m
Dh, Dw, Dt = dctm(AH), dctm(AW), dctm(T)

def to_ycc(rgb):
    r, g, b = [rgb[..., i].astype(np.float64) for i in range(3)]
    y = 0.299 * r + 0.587 * g + 0.114 * b
    cb = 128 - 0.168736 * r - 0.331264 * g + 0.5 * b
    cr = 128 + 0.5 * r - 0.418688 * g - 0.081312 * b
    return np.stack([y, cb, cr]) - 128.0          # planes, zero-centred

def to_rgb(ycc):
    y, cb, cr = ycc + 128.0
    r = y + 1.402 * (cr - 128); g = y - 0.344136 * (cb - 128) - 0.714136 * (cr - 128); b = y + 1.772 * (cb - 128)
    return np.clip(np.stack([r, g, b], -1), 0, 255).astype(np.uint8)

rng = np.random.default_rng(2026)

def analog_group(frames, snr, chroma_w=1.0):
    """frames: T x AH x AW x 3 (uint8). Returns the received T frames (uint8). chroma_w < 1: perceptual allocation (brightness first)."""
    Tn = len(frames)
    Dt = dctm(Tn)
    KEEPn = int(SHARE * SYM * 2 * Tn / FPS) // (CH * CH)
    x = np.stack([to_ycc(f) for f in frames], 1)          # 3, T, AH, AW
    X = np.tensordot(Dt, Dh @ x @ Dw.T, axes=([1], [1])).transpose(1, 0, 2, 3)   # spatial DCT, then along time
    P, S = X.shape[0], X.shape[1]
    nb_h, nb_w = AH // CH, AW // CH
    C = X.reshape(P, S, nb_h, CH, nb_w, CH).transpose(0, 1, 2, 4, 3, 5).reshape(-1, CH * CH)
    lam = np.maximum((C ** 2).mean(1), 1e-9)
    w = np.ones_like(lam)
    w[lam.size // 3:] = chroma_w                           # chunks of Cb and Cr come after Y
    keep = np.argsort(w * lam)[::-1][:KEEPn]
    g = (w[keep] / lam[keep]) ** 0.25                      # weighted SoftCast: g^2 ~ sqrt(w / lambda)
    g *= np.sqrt(KEEPn / np.sum(g ** 2 * lam[keep]))       # power 1 per sent value on average
    sig2 = 10 ** (-snr / 10)                               # noise per real value (Es/N0 per real dimension)
    y = g[:, None] * C[keep] + rng.normal(0, np.sqrt(sig2), (KEEPn, CH * CH))
    R = np.zeros_like(C)
    R[keep] = (g * lam[keep] / (g ** 2 * lam[keep] + sig2))[:, None] * y    # linear least-squares estimate
    Xr = R.reshape(P, S, nb_h, nb_w, CH, CH).transpose(0, 1, 2, 4, 3, 5).reshape(P, S, AH, AW)
    xr = Dh.T @ np.tensordot(Dt.T, Xr, axes=([1], [1])).transpose(1, 0, 2, 3) @ Dw
    return [to_rgb(xr[:, t]) for t in range(Tn)]

def reader(args, w, h):
    p = subprocess.Popen(['ffmpeg', '-hide_banner', '-loglevel', 'error'] + args + ['-f', 'rawvideo', '-pix_fmt', 'rgb24', '-'], stdout=subprocess.PIPE)
    n = w * h * 3
    while True:
        b = p.stdout.read(n)
        if len(b) < n:
            return
        yield np.frombuffer(b, np.uint8).reshape(h, w, 3)

dig = reader(['-i', DIG, '-vf', f'scale={PW}:{PH}:flags=bicubic,fps={FPS}'], PW, PH)
src = reader(['-ss', '20', '-t', str(len(STEPS) * SEG + 1), '-i', SRC, '-vf', f'scale={AW}:{AH}:flags=area,fps={FPS}'], AW, AH)
srcbig = reader(['-ss', '20', '-t', str(len(STEPS) * SEG + 1), '-i', SRC, '-vf', f'scale={PW}:{PH}:flags=area,fps={FPS}'], PW, PH)

W, HDR, FTR = 2 * PW, 92, 64
out = subprocess.Popen(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-s', f'{W}x{HDR + PH + FTR}', '-r', str(FPS), '-i', '-',
                        '-c:v', 'libx264', '-crf', '17', '-preset', 'medium', '-pix_fmt', 'yuv420p', '-movflags', '+faststart', OUT], stdin=subprocess.PIPE)
f_big, f_mid, f_small = ImageFont.truetype(FONT_B, 40), ImageFont.truetype(FONT_B, 24), ImageFont.truetype(FONT, 19)
TEAL, RED, AMBER, GREY = (115, 184, 209), (224, 97, 87), (226, 170, 80), (170, 176, 182)

# the digital side: which KAVL frames arrive; a loss spoils the picture until the next key frame arrives intact
n_kavl = int(len(STEPS) * SEG / FRAME_S) + 2
ok_kavl = [rng.random() < p_ok(STEPS[min(len(STEPS) - 1, int(k * FRAME_S / SEG))]) for k in range(n_kavl)]
broken = False
last_good = None
T2 = 10
buf1, buf2, src_q = [], [], []
den = None                     # v2: the receiver's running average (192x108, float)
RW, RH = 72, 40                # raw pixels: 72x40, Y full + Cb/Cr at half resolution = 4320 values a frame (15 % of the channel at 30 fps)
def raw_frame(img, snr):
    """One frame sent pixel by pixel as signal levels (analog-TV style), with the channel's noise on every value."""
    small = np.asarray(Image.fromarray(img).resize((RW, RH), Image.BOX), np.float64)
    ycc = to_ycc(small)                                   # 3, RH, RW
    y = ycc[0]
    cb = ycc[1].reshape(RH // 2, 2, RW // 2, 2).mean((1, 3))
    cr = ycc[2].reshape(RH // 2, 2, RW // 2, 2).mean((1, 3))
    vals = [y, cb, cr]
    sig2 = 10 ** (-snr / 10)
    outp = []
    for v in vals:                                        # each plane scaled to power 1 (its mean and spread travel digitally, like a sync level)
        m, s = v.mean(), v.std() + 1e-9
        rx = (v - m) / s + rng.normal(0, np.sqrt(sig2), v.shape)
        outp.append(rx * s + m)
    cbu = np.repeat(np.repeat(outp[1], 2, 0), 2, 1)
    cru = np.repeat(np.repeat(outp[2], 2, 0), 2, 1)
    return to_rgb(np.stack([outp[0], cbu, cru]))
def lo(img):                   # 960x540 -> 192x108
    return np.asarray(Image.fromarray(img).resize((AW, AH), Image.BOX), np.float64)
def up(arr):                   # 192x108 float -> 960x540 float
    return np.asarray(Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8)).resize((PW, PH), Image.BICUBIC), np.float64)
for i in range(N):
    t = i / FPS
    snr = STEPS[min(len(STEPS) - 1, int(t / SEG))]
    d = next(dig)
    d_src = next(srcbig)
    if not src_q:
        src_q = [next(src) for _ in range(T2)]
        g2 = analog_group(src_q, snr, chroma_w=0.3)        # v2: groups of 10 frames, brightness first
        g1 = analog_group(src_q[:5], snr) + analog_group(src_q[5:], snr)   # v1: as in the first demo
        buf1, buf2 = g1, g2
        src_q = src_q[:0]
        src_q = ['x'] * T2                                  # placeholders: consumed one per frame below
    src_q.pop()
    a1, a2 = buf1.pop(0), buf2.pop(0)
    k = int(t / FRAME_S)
    if not ok_kavl[k]:
        broken = True
    elif broken and int(t / KEY_S) != int((t - 1 / FPS) / KEY_S):
        broken = False
    if not broken:
        last_good = d
    # v2 receiver: motion-adaptive average over frames (still areas: up to 6 frames; moving areas: at once)
    a2f = a2.astype(np.float64)
    if den is None:
        den = a2f
    else:
        diff = np.abs(a2f - den).mean(2, keepdims=True)
        alpha = np.clip(diff / 40.0, 1 / 6, 1.0)
        den = den + alpha * (a2f - den)
    v2 = up(den)
    # v2 receiver: the last sharp digital frame's detail where the fuzzy picture still matches it
    if last_good is not None:
        dl = lo(last_good)
        hp = last_good.astype(np.float64) - up(dl)
        sim_lo = np.exp(-(np.abs(den - dl).mean(2) / 18.0) ** 2)
        sim_up = np.asarray(Image.fromarray((sim_lo * 255).astype(np.uint8)).resize((PW, PH), Image.BICUBIC), np.float64)[..., None] / 255.0
        v2 = v2 + sim_up * hp
    v2 = np.clip(v2, 0, 255).astype(np.uint8)
    canvas = Image.new('RGB', (W, HDR + PH + FTR), (16, 19, 22))
    dr = ImageDraw.Draw(canvas)
    if not broken:
        canvas.paste(Image.fromarray(d), (0, HDR))
        canvas.paste(Image.fromarray(d), (PW, HDR))
    else:
        canvas.paste(Image.fromarray(v2), (0, HDR))
        canvas.paste(Image.fromarray(raw_frame(d_src, snr)).resize((PW, PH), Image.NEAREST), (PW, HDR))
        for x0, label in ((0, 'smart fallback (DCT)'), (PW, 'raw pixels, 72x40')):
            dr.rectangle([x0 + 12, HDR + 12, x0 + 330, HDR + 50], fill=(0, 0, 0))
            dr.text((x0 + 22, HDR + 17), label, font=f_mid, fill=TEAL)
    dr.line([PW, HDR, PW, HDR + PH], fill=(16, 19, 22), width=4)
    col = (120, 200, 130) if snr >= 1 else AMBER if snr >= 0 else RED
    dr.text((24, 22), f'Signal {snr:+d} dB', font=f_big, fill=col)
    dr.text((300, 34), 'KAVL 1080p60 profile, 440 kHz + 15 % analog-style layer  (SIMULATION)', font=f_mid, fill=GREY)
    dr.text((24, HDR - 4), 'Fallback A: smart (frequency coefficients, improved receiver)', font=f_small, fill=GREY, anchor='ld')
    dr.text((PW + 24, HDR - 4), 'Fallback B: raw pixels, analog-TV style, shown unsmoothed', font=f_small, fill=GREY, anchor='ld')
    dr.text((24, HDR + PH + 12), 'Both use the same 15 % of the channel (about 130,000 values a second). Raw pixels: each pixel value sent as a signal level, noise lands', font=f_small, fill=GREY)
    dr.text((24, HDR + PH + 36), 'on every pixel as grain; 72x40 is all that fits at 30 fps. Above the digital threshold both show the digital picture. Assumes sync holds to -6 dB.', font=f_small, fill=GREY)
    out.stdin.write(np.asarray(canvas).tobytes())
out.stdin.close()
out.wait()
print('frames', N)
