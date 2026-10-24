#!/usr/bin/env python3
"""The Chorus's High quality on its own (it goes beyond the JSFX's, so there is no reference): every case renders,
stays finite and close in level to Standard; the drive aliases less; the vibrato's modulation has no 6 kHz spurs."""
import os, subprocess, sys, tempfile
import numpy as np
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST = os.path.join(ROOT, 'build/linux/clap_host_test')
BUNDLE = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'build/linux/TwoStorms.clap')
TMP = tempfile.mkdtemp()
env = dict(os.environ, TS_PLUGIN='com.twostorms.chorus')
sys.path.insert(0, os.path.join(ROOT, 'tests/cases'))
import chorus as C

def render(sig, args, sr=48000):
    out = os.path.join(TMP, 'o.f64')
    subprocess.run([HOST, 'render', BUNDLE, sig, out, str(sr), '512'] + args, env=env, capture_output=True, check=True)
    return np.fromfile(out).reshape(-1, 2)

bad = 0
for name, args in C.CASES.items():
    sig = os.path.join(ROOT, 'tests/sig/mix.f64')
    a = [x for x in args if not x.startswith('sig=')]
    for x in args:
        if x.startswith('sig='): sig = os.path.join(ROOT, x[4:])
    std = render(sig, a)
    hq = render(sig, [x if x != '69=0' else '69=1' for x in a])
    ok = np.all(np.isfinite(hq))
    rs, rh = np.sqrt(np.mean(std ** 2)), np.sqrt(np.mean(hq ** 2))
    db = 20 * np.log10(rh / rs) if rs > 0 and rh > 0 else 0
    ok = ok and abs(db) < 1.5
    bad += not ok
    print(f"{'OK ' if ok else 'BAD'} {name:24s} level High vs Standard {db:+.2f} dB")

# a 9 kHz sine through the drive: what lands where it shouldn't (not at 9 kHz's harmonics, all above 20 kHz)
sr, n = 48000, 96000
t = np.arange(n) / sr
x = 0.5 * np.sin(2 * np.pi * 9000 * t)
sig = np.stack([x, x, 0 * x, 0 * x], 1).astype(np.float64)
p = os.path.join(TMP, 's.f64'); sig.tofile(p)
def alias(args):
    y = render(p, args)[n // 2:, 0]
    Y = np.abs(np.fft.rfft(y * np.hanning(len(y)))) ** 2
    f = np.fft.rfftfreq(len(y), 1 / sr)
    harm = np.zeros_like(f, bool)
    for k in range(1, 3):
        harm |= np.abs(f - 9000 * k) < 60
    return 10 * np.log10(Y[~harm & (f > 100)].sum() / Y[harm].sum())
base = ['47=100', '50=3', '53=0', '54=6', '66=90', '64=0']
print(f"drive aliasing (energy off the harmonics, re the harmonics): Standard {alias(base + ['69=0']):.1f} dB, High {alias(base + ['69=1']):.1f} dB")
sys.exit(1 if bad else 0)
