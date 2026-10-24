#!/usr/bin/env python3
"""The Tremolo's grids beyond the JSFX (no reference): the gap, groups taking turns (with their own lengths), linked
grids, step levels, the pitch (and its latency), and the other modes with several grids. A 1 kHz sine, 120 bpm, 1/8
steps (12000 samples at 48 kHz), the song playing from beat 0."""
import os, subprocess, sys, tempfile
import numpy as np
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST = os.path.join(ROOT, 'build/linux/clap_host_test')
BUNDLE = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'build/linux/TwoStorms.clap')
TMP = tempfile.mkdtemp()
env = dict(os.environ, TS_PLUGIN='com.twostorms.tremolo')
SR, STEP = 48000, 12000
n = 8 * STEP + 8192
t = np.arange(n) / SR
x = 0.5 * np.sin(2 * np.pi * 1000 * t)
sig = os.path.join(TMP, 'sine.f64')
np.stack([x, x, 0 * x, 0 * x], 1).astype(np.float64).tofile(sig)


def pat(g, txt):
    s = txt.replace(' ', '')
    q = []
    for i in range(64):
        c = s[i % len(s)]
        q.append(255 if c in 'xX' else (int((ord(c) - 48) * 25.5 + 0.5) if c.isdigit() else 0))
    out = []
    for w in range(11):
        v = sum(q[w * 6 + i] * 256 ** i for i in range(6) if w * 6 + i < 64)
        out.append(f'{100 + g * 11 + w}={v}')
    return out


def render(args):
    o = os.path.join(TMP, 'o.f64')
    r = subprocess.run([HOST, 'render', BUNDLE, sig, o, str(SR), '512', 'tempo=120', 'play=1', '65=2', '59=0.5', '60=0.5'] + args,
                       env=env, capture_output=True, text=True, check=True)
    lat = 0
    for line in (r.stdout + r.stderr).splitlines():
        if line.startswith('latency '): lat = int(line.split()[1])
    return np.fromfile(o).reshape(-1, 2), lat


def gain(y, step, a, b, ch=0, lat=0):
    """the output's level re the input's over [a, b) of a step (fractions)"""
    i0, i1 = lat + int((step + a) * STEP), lat + int((step + b) * STEP)
    return np.sqrt(np.mean(y[i0:i1, ch] ** 2)) / (0.5 / np.sqrt(2))


bad = 0
def check(name, ok, info=''):
    global bad
    bad += not ok
    print(f"{'OK ' if ok else 'BAD'} {name:44s} {info}")


# the gap: every step on, 40% gap: sounds for the first 60%, silent at the end
y, _ = render(['64=1', '67=40'] + pat(0, 'x'))
on = [gain(y, s, 0.1, 0.5) for s in range(1, 6)]
off = [gain(y, s, 0.68, 0.97) for s in range(1, 6)]
check('gap: steps sound, then go quiet', min(on) > 0.98 and max(off) < 0.02, f'on {min(on):.3f} off {max(off):.4f}')
y, _ = render(['67=40'] + pat(0, 'x'))
check('no gap when it is off', gain(y, 2, 0.68, 0.97) > 0.98)
# an isolated step is never gapped
y, _ = render(['64=1', '67=40'] + pat(0, 'x.'))
check('gap: an isolated step plays to its end', gain(y, 2, 0.7, 0.97) > 0.98 and gain(y, 3, 0.1, 0.9) < 0.02,
      f'{gain(y, 2, 0.7, 0.97):.3f}')
# step levels
y, _ = render(pat(0, 'x5'))
g5 = gain(y, 3, 0.1, 0.9)
check('a 50% step plays at half level', abs(g5 - 128 / 255) < 0.01, f'{g5:.4f}')
# two grids taking turns (2 steps each), grid 1 left, grid 2 right
y, _ = render(['87=1', '72=0', '68=2', '69=2', '80=-100', '81=100'] + pat(0, 'x') + pat(1, 'x'))
L = [gain(y, s, 0.1, 0.9, 0) for s in range(8)]
Rr = [gain(y, s, 0.1, 0.9, 1) for s in range(8)]
exp = [1, 1, 0, 0, 1, 1, 0, 0]
okk = all(abs(L[s] - exp[s]) < 0.02 and abs(Rr[s] - (1 - exp[s])) < 0.02 for s in range(8))
check('unlinked: 1 then 2, in turns', okk, ' '.join(f'{a:.2f}/{b:.2f}' for a, b in zip(L, Rr)))
# their own lengths: 3 and 5 steps, the cycle is 8
y, _ = render(['87=1', '72=0', '68=3', '69=5', '80=-100', '81=100'] + pat(0, 'x') + pat(1, 'x'))
L = [gain(y, s, 0.1, 0.9, 0) for s in range(8)]
exp = [1, 1, 1, 0, 0, 0, 0, 0]
check('group lengths 3 + 5', all(abs(L[s] - exp[s]) < 0.02 for s in range(8)), ' '.join(f'{a:.2f}' for a in L))
# the gap at a group's end runs into the next group's first step
y, _ = render(['87=1', '72=0', '68=2', '69=2', '64=1', '67=40'] + pat(0, 'x') + pat(1, 'x'))
check('gap at the boundary between groups', gain(y, 1, 0.7, 0.97) < 0.02 and gain(y, 3, 0.7, 0.97) < 0.02 and gain(y, 2, 0.1, 0.5) > 0.98)
# linked grids play together: complementary patterns make a steady level
y, _ = render(['87=1'] + pat(0, 'x.') + pat(1, '.x'))
gl = [gain(y, s, 0.1, 0.9) for s in range(1, 7)]
check('linked: together', min(gl) > 0.98 and max(gl) < 1.02, f'{min(gl):.3f}..{max(gl):.3f}')
# the pitch: grid 1 an octave up; the latency is the shifter's frame
y, lat = render(['89=12', '87=1'] + pat(0, '.') + pat(1, 'x'))
seg = y[lat + 2 * STEP: lat + 6 * STEP, 0]
Y = np.abs(np.fft.rfft(seg * np.hanning(len(seg))))
f = np.fft.rfftfreq(len(seg), 1 / SR)
pk = f[np.argmax(Y)]
check('pitch: +12 st is 2 kHz, latency 4096', abs(pk - 2000) < 3 and lat == 4096, f'peak {pk:.1f} Hz, latency {lat}')
y0, lat0 = render(pat(0, 'x'))
check('no pitch: no latency', lat0 == 0)
# the other modes with several grids: finite, no jumps bigger than the sine's own
for name, a in [('harmonic, 2 grids in turns', ['51=2', '87=1', '72=0', '68=3', '69=5']),
                ('auto-pan, 4 grids linked', ['51=1', '87=3'] + pat(2, 'x.x.') + pat(3, '.xx.')),
                ('LFO x pattern, 3 grids, gap, swing', ['50=2', '54=9', '87=2', '73=0', '64=1', '66=60', '68=7', '69=9'])]:
    y, _ = render(a)
    d = np.max(np.abs(np.diff(y, axis=0)))
    check(name, np.all(np.isfinite(y)) and d < 0.2, f'largest step {d:.3f}')
sys.exit(1 if bad else 0)
