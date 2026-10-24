#!/usr/bin/env python3
"""What the CLAP Delay does beyond the JSFX (no reference): Line C; series through all three lines (A's repeats into
B, B's into C) and linked lines; dry and wet levels; the reverb ducking on its own; reverse (every repeat backwards,
on the way out, with no gaps)."""
import os, subprocess, sys, tempfile
import numpy as np
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST = os.path.join(ROOT, 'build/linux/clap_host_test')
BUNDLE = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'build/linux/TwoStorms.clap')
TMP = tempfile.mkdtemp()
env = dict(os.environ, TS_PLUGIN='com.twostorms.delay')
SR = 48000
A, B, C = 70, 110, 150          # the lines' first parameters
K_ON, K_TL, K_SL, K_FB, K_LEVEL, K_SH, K_SHSY, K_TAPS = 0, 1, 2, 6, 9, 26, 32, 29
P_C12, P_C23, P_LK12, P_LK23, P_DRY, P_WET, P_DUCK, P_RDUCK, P_RVB = 45, 44, 58, 48, 47, 69, 50, 254, 60
bad = 0


def ok(c, msg):
    global bad
    print(('OK  ' if c else 'BAD ') + msg)
    bad += not c


def render(sig, args):
    i = os.path.join(TMP, 'i.f64'); o = os.path.join(TMP, 'o.f64')
    z = np.zeros(len(sig))
    np.stack([sig, sig, z, z], 1).astype(np.float64).tofile(i)
    subprocess.run([HOST, 'render', BUNDLE, i, o, str(SR), '512'] + [f'{k}={v}' for k, v in args], env=env, check=True, capture_output=True)
    return np.fromfile(o).reshape(-1, 2)[:, 0]


def free(line, ms):   # a line at a free time, one tap
    return [(line + K_SL, 0), (line + K_TL, ms)]


WET = [(P_DRY, -60), (P_WET, 0)]
n = SR * 3
imp = np.zeros(n); imp[1000] = 1.0

# Line C alone, at 300 ms, no feedback
y = render(imp, WET + [(A + K_ON, 0), (C + K_ON, 1), (C + K_LEVEL, 0), (C + K_FB, 0)] + free(C, 300))
pk = int(np.argmax(np.abs(y)))
ok(abs(pk - (1000 + 0.3 * SR)) < 12 and abs(y[pk]) > 0.5, f'Line C alone: its repeat at {(pk - 1000) / SR * 1000:.1f} ms (want 300)')

# series through all three: 100 + 150 + 200 ms (each line's own repeat too, every line heard)
args = WET + [(P_C12, 1), (P_C23, 1)]
for L, t in ((A, 100), (B, 150), (C, 200)):
    args += [(L + K_ON, 1), (L + K_LEVEL, 0), (L + K_FB, 0)] + free(L, t)
y = render(imp, args)
want = {100: 'A', 250: 'A into B', 450: 'A into B into C'}
for t, name in want.items():
    j = 1000 + int(t * SR / 1000)
    ok(np.abs(y[j - 12:j + 12]).max() > 0.5, f'series A > B > C: a repeat at {t} ms ({name})')
j = 1000 + int(150 * SR / 1000)
ok(np.abs(y[j - 12:j + 12]).max() < 0.05, 'series: no repeat of the input itself in B (at 150 ms)')

# linked B and C at 100%: B's repeats come back through C (and C's through B), alternating times
args = WET + [(A + K_ON, 0), (P_C23, 2), (P_LK23, 100)]
for L, t in ((B, 100), (C, 170)):
    args += [(L + K_ON, 1), (L + K_LEVEL, 0), (L + K_FB, 80)] + free(L, t)
y = render(imp, args)
for t in (100, 270, 440):
    j = 1000 + int(t * SR / 1000)
    ok(np.abs(y[j - 12:j + 12]).max() > 0.2, f'linked B and C: a repeat at {t} ms (B, then C, then B ...)')
j = 1000 + int(200 * SR / 1000)
ok(np.abs(y[j - 12:j + 12]).max() < 0.05, "linked at 100%: B's own second repeat (200 ms) is gone")

# dry and wet levels
y = render(imp, [(P_DRY, -6), (P_WET, -60), (A + K_FB, 0)] + free(A, 200))
ok(abs(y[1000] - 10 ** (-6 / 20)) < 1e-3 and np.abs(y[2000:]).max() < 1e-6, f'dry -6 dB, wet off: {y[1000]:.4f} and no repeats')
y = render(imp, [(P_DRY, -60), (P_WET, -12), (A + K_FB, 0), (A + K_LEVEL, 0)] + free(A, 200))
j = 1000 + int(0.2 * SR)
ok(abs(np.abs(y[j - 12:j + 12]).max() - 10 ** (-12 / 20)) < 0.01 and abs(y[1000]) < 1e-9, 'wet -12 dB, dry off')

# the reverb ducking on its own: a steady tone, delays not ducked, the reverb ducked hard
t = np.arange(n) / SR
tone = 0.5 * np.sin(2 * np.pi * 220 * t)
base = WET + [(A + K_FB, 0), (P_RVB, 60), (51, -40)] + free(A, 200)
y0 = render(tone, base)
y1 = render(tone, base + [(P_RDUCK, 100)])
y2 = render(tone, base + [(P_DUCK, 100)])
r = lambda v: np.sqrt(np.mean(v[SR:] ** 2))
yd = render(tone, [(k, v) if k != P_RVB else (k, 0) for k, v in base])   # the delays alone
ok(abs(r(y1) - r(yd)) < 0.3 * r(yd) and r(y2) < r(y0) and abs(r(y2) - r(y1)) > 0.05,
   f'ducking: none {r(y0):.3f}; the reverb ducked {r(y1):.3f} (the delays alone: {r(yd):.3f}); the delays ducked {r(y2):.3f}')

# reverse: a rising chirp comes back falling, every repeat, with no gaps in a held sound
tt = np.arange(int(0.2 * SR)) / SR
chirp = np.sin(2 * np.pi * (300 * tt + 0.5 * (3000 - 300) / 0.2 * tt * tt)) * np.hanning(len(tt))
x = np.zeros(n); x[2000:2000 + len(tt)] = chirp
y = render(x, WET + [(A + K_SH, 2), (A + K_FB, 50), (A + K_LEVEL, 0)] + free(A, 500))


def slope(seg):
    # the frequency of the strongest bin, 5 ms frames: rising or falling
    f = []
    for i in range(0, len(seg) - 480, 240):
        w = seg[i:i + 480] * np.hanning(480)
        if np.sqrt(np.mean(w ** 2)) < 0.01:
            continue
        f.append(np.argmax(np.abs(np.fft.rfft(w, 4096))) * SR / 4096)
    return np.polyfit(np.arange(len(f)), f, 1)[0] if len(f) > 4 else 0, len(f)


env_ = np.convolve(np.abs(y), np.ones(480) / 480, 'same')
segs, on, st = [], False, 0
for i, v in enumerate(env_):
    if not on and v > 0.02: on, st = True, i
    elif on and v < 0.005: on = False; segs.append((st, i))
ok(len(segs) >= 3, f'reverse: {len(segs)} repeats heard')
for k, (a0, a1) in enumerate(segs[:3]):
    s, nf = slope(y[a0:a1])
    ok(s < -50, f'reverse: repeat {k + 1} at {a0 / SR * 1000:.0f} ms falls ({s:+.0f} Hz per frame, {nf} frames)')
s, _ = slope(x[1500:2000 + len(tt) + 500])
ok(s > 50, f'(the input rises: {s:+.0f} Hz per frame)')
# a held sound (noise) reversed: no gaps at the seams
noise = np.random.default_rng(3).normal(0, 0.2, n)
y = render(noise, WET + [(A + K_SH, 2), (A + K_FB, 0), (A + K_LEVEL, 0)] + free(A, 300))
rm = np.sqrt(np.convolve(y[SR:] ** 2, np.ones(240) / 240, 'valid'))
ok(rm.min() > 0.35 * np.median(rm), f'reverse, held noise: quietest 5 ms {20 * np.log10(rm.min() / np.median(rm)):+.1f} dB re the median (no gaps)')
# reverse on the beat and reverse + pitch stay finite and loud enough
for name, a in (('on the beat', [(A + K_SH, 2), (A + K_SHSY, 10)]), ('+ pitch', [(A + K_SH, 3), (A + 27, 12)])):
    y = render(noise, WET + a + [(A + K_FB, 40), (A + K_LEVEL, 0)] + free(A, 400))
    ok(np.isfinite(y).all() and np.sqrt(np.mean(y[SR:] ** 2)) > 0.05, f'reverse {name}: finite, rms {np.sqrt(np.mean(y[SR:] ** 2)):.3f}')
print(f'{bad} bad')
sys.exit(1 if bad else 0)
