#!/usr/bin/env python3
"""The Wah's Voice (formants in series, a cascade, or in parallel; the CLAP's own): a 110 Hz sawtooth through each vowel lands its first
three formants where the vowel table puts them, ah comes out near the input's level (Level 0 dB) and oo about 7 dB
under it; moving voices stay finite. And the bands as switches: bands 1 and 3 on (2 off) play side by side even with
"series" set between 2 and 3, the same as band 3 on its own added to band 1 on its own; no band on is the dry signal."""
import os, subprocess, sys, tempfile
import numpy as np
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST = os.path.join(ROOT, 'build/linux/clap_host_test')
BUNDLE = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'build/linux/TwoStorms.clap')
TMP = tempfile.mkdtemp()
env = dict(os.environ, TS_PLUGIN='com.twostorms.wah')
SR, n, f0 = 48000, 96000, 110.0
t = np.arange(n) / SR
x = sum(np.sin(2 * np.pi * f0 * h * t) / h for h in range(1, int(10000 / f0)))
x = 0.25 * x / np.max(np.abs(x))
sig = os.path.join(TMP, 'saw.f64')
np.stack([x, x, 0 * x, 0 * x], 1).astype(np.float64).tofile(sig)


def render(args, s=sig):
    o = os.path.join(TMP, 'o.f64')
    subprocess.run([HOST, 'render', BUNDLE, s, o, str(SR), '512'] + args, env=env, check=True, capture_output=True)
    return np.fromfile(o).reshape(-1, 2)[:, 0]


# vowel path 1 (oo-ah-oo): position 0 = oo, 50 = ah; path 2 (ee-eh-ae): 0 = ee
VOW = {'oo': (1, 0, (330, 900, 2300)), 'ah': (1, 50, (768, 1333, 2522)), 'ee': (2, 0, (342, 2322, 3000))}
bad = 0
lv = {}
for name, (path, pos, F) in VOW.items():
    y = render(['155=1', '156=1', '157=1', '53=1', '54=1', '55=1', f'56={path}', '63=0', f'59={pos}', f'90={pos}', f'121={pos}'])[n // 2:]
    xs = x[n // 2:]
    Y = np.abs(np.fft.rfft(y * np.hanning(len(y))))
    X = np.abs(np.fft.rfft(xs * np.hanning(len(xs))))
    hs = []
    for h in range(1, int(4000 / f0)):
        k = int(round(h * f0 * len(y) / SR))
        hs.append(20 * np.log10(Y[k - 2:k + 3].max() / X[k - 2:k + 3].max()))
    hs = np.array(hs)
    pk = [(i + 1) * f0 for i in range(1, len(hs) - 1) if hs[i] > hs[i - 1] and hs[i] >= hs[i + 1]]
    ok = all(min(abs(p - f) for p in pk) <= f0 * 0.75 for f in F if f < 3500)
    lv[name] = 20 * np.log10(np.sqrt(np.mean(y ** 2)) / np.sqrt(np.mean(xs ** 2)))
    bad += not ok
    print(f"{'OK ' if ok else 'BAD'} {name}: envelope peaks {' '.join('%.0f' % p for p in pk[:4])} (table {F}), level {lv[name]:+.1f} dB")
ok = abs(lv['ah']) < 1.5 and -9 < lv['oo'] - lv['ah'] < -5
bad += not ok
print(f"{'OK ' if ok else 'BAD'} levels: ah {lv['ah']:+.1f} dB, oo {lv['oo'] - lv['ah']:+.1f} dB re ah")
# the bands in parallel (formants side by side): ah's first two formants still there, about as loud
y = render(['155=1', '156=1', '157=1', '55=1', '56=1', '63=0', '59=50', '90=50', '121=50'])[n // 2:]
xs = x[n // 2:]
Y = np.abs(np.fft.rfft(y * np.hanning(len(y))))
X = np.abs(np.fft.rfft(xs * np.hanning(len(xs))))
hs = np.array([20 * np.log10(Y[int(round(h * f0 * len(y) / SR)) - 2:int(round(h * f0 * len(y) / SR)) + 3].max() /
                             X[int(round(h * f0 * len(y) / SR)) - 2:int(round(h * f0 * len(y) / SR)) + 3].max()) for h in range(1, int(4000 / f0))])
pk = [(i + 1) * f0 for i in range(1, len(hs) - 1) if hs[i] > hs[i - 1] and hs[i] >= hs[i + 1]]
lvp = 20 * np.log10(np.sqrt(np.mean(y ** 2)) / np.sqrt(np.mean(xs ** 2)))
ok = all(min(abs(p - f) for p in pk) <= f0 * 0.75 for f in (768, 1333)) and abs(lvp) < 4
bad += not ok
print(f"{'OK ' if ok else 'BAD'} ah, formants in parallel: envelope peaks {' '.join('%.0f' % p for p in pk[:4])}, level {lvp:+.1f} dB")
for name, a in [('envelope, hits', ['155=1', '156=1', '157=1', '55=1', '56=3', '58=1', '89=1', '120=1', 'sig=tests/sig/hits.f64']),
                ('LFO 1/8, female, stereo offset', ['155=1', '156=1', '157=1', '55=2', '56=1', '58=3', '89=3', '120=3', '78=9', '109=9', '140=9', '85=60', 'tempo=120', 'play=1']),
                ('2 bands, child, shift', ['155=1', '156=1', '55=3', '56=4', '57=-5', 't40000:59=90'])]:
    s = sig
    for q in a:
        if q.startswith('sig='): s = os.path.join(ROOT, q[4:])
    y = render([q for q in a if not q.startswith('sig=')], s)
    ok = np.all(np.isfinite(y)) and np.max(np.abs(y)) < 4
    bad += not ok
    print(f"{'OK ' if ok else 'BAD'} {name}: peak {np.max(np.abs(y)):.2f}")
# the bands as switches (Voice off)
B1 = ['58=0', '59=30', '60=400', '61=2000', '89=0', '90=70', '91=900', '92=4000', '120=0', '121=60', '122=1500', '123=6000']
y13 = render(B1 + ['155=1', '156=0', '157=1', '54=1'])
y1 = render(B1 + ['155=1', '156=0', '157=0'])
y3 = render(B1 + ['155=0', '156=0', '157=1'])
d = np.max(np.abs(y13 - (y1 + y3)))
ok = d < 1e-9
bad += not ok
print(f"{'OK ' if ok else 'BAD'} bands 1 and 3 (2 off, 'series' into 3 set): band 1 + band 3, max diff {d:.2g}")
y0 = render(B1 + ['155=0', '156=0', '157=0'])
d = np.max(np.abs(y0 - x))
ok = d < 1e-9
bad += not ok
print(f"{'OK ' if ok else 'BAD'} no band on: the dry signal, max diff {d:.2g}")
sys.exit(1 if bad else 0)
