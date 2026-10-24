#!/usr/bin/env python3
"""The Tuner's pitch detector (tests/tuner_test.cpp around src/core/pitch.cpp), on made-up notes:
- steady tones, sine and rich (plucked: harmonics 1/h, decaying), across guitar, bass and voice: the reading is within
  0.1 cent of the true pitch (median of the frames while the note sounds), at 44.1, 48 and 96 kHz
- a bass note with a weak fundamental (2nd and 3rd harmonics louder): no octave error
- stiff strings (inharmonic: the upper partials sharp, louder at the attack): read at the fundamental (itself
  f0 sqrt(1 + B)), within 0.25 cent, and steady as the partials fade (the spread under half a cent)
- a sung note with vibrato: the median within 3 cents, no octave jumps
- noise and silence: nothing"""
import os, subprocess, sys, tempfile
import numpy as np
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, 'build/linux/tuner_test')
TMP = tempfile.mkdtemp()
rng = np.random.default_rng(7)
bad = 0


def run(x, sr, rng_):
    p = os.path.join(TMP, 'x.f64')
    np.asarray(x, dtype=np.float64).tofile(p)
    r = subprocess.run([EXE, p, str(sr), str(rng_)], capture_output=True, text=True, check=True)
    a = np.array([[float(v) for v in l.split()] for l in r.stdout.strip().split('\n') if l.strip()])
    ms = float(r.stderr.split(',')[1].split('ms')[0])
    return a, ms


def cents(f, f0):
    return 1200 * np.log2(f / f0)


def pluck(f0, sr, dur=2.0, nh=30, B=0.0, weak=False, amp=0.3):
    t = np.arange(int(sr * dur)) / sr
    x = np.zeros_like(t)
    for h in range(1, nh + 1):
        fh = f0 * h * np.sqrt(1 + B * h * h)
        if fh > sr * 0.45: break
        a = 1.0 / h
        if weak and h == 1: a = 0.15
        if weak and h in (2, 3): a = 1.0
        x += a * np.exp(-t * (1.5 + 0.4 * h)) * np.sin(2 * np.pi * fh * t + rng.uniform(0, 6.28))
    return amp * x / np.max(np.abs(x))


def check(name, x, sr, rg, f0, tol, skip=0.15, until=None, spread=None):
    global bad
    a, ms = run(x, sr, rg)
    sel = (a[:, 0] > skip) & (a[:, 1] > 0)
    if until: sel &= a[:, 0] < until
    if not sel.any():
        print(f'BAD {name}: nothing found'); bad += 1; return
    c = cents(a[sel, 1], f0)
    med = np.median(c)
    octs = np.sum(np.abs(c) > 300)
    ok = abs(med) <= tol and octs == 0 and (spread is None or np.ptp(c) <= spread)
    print(f"{'OK ' if ok else 'BAD'} {name:44s} {med:+8.3f} cents (max {np.max(np.abs(c)):.2f}, {sel.sum()} frames, {octs} octave errors, {ms:.2f} ms/frame)")
    bad += not ok


NOTES = {'B0': 30.868, 'E1': 41.203, 'A1': 55.0, 'D2': 73.416, 'E2': 82.407, 'A2': 110.0, 'G3': 196.0, 'B3': 246.94,
         'E4': 329.63, 'A4': 440.0, 'C6': 1046.5}
for sr in (44100, 48000, 96000):
    for n, f in NOTES.items():
        rg = 1 if f < 60 else 3
        off = 3.7 * (1 if hash(n) % 2 else -1)
        f0 = f * 2 ** (off / 1200)
        t = np.arange(int(sr * 1.0)) / sr
        check(f'sine {n} {off:+.1f} c, {sr // 1000} kHz', 0.2 * np.sin(2 * np.pi * f0 * t), sr, rg, f0, 0.1)
for n in ('B0', 'E1', 'A1', 'D2', 'A2'):
    f0 = NOTES[n] * 2 ** (-2.3 / 1200)
    check(f'bass pluck {n} -2.3 c', pluck(f0, 48000), 48000, 1, f0, 0.1, until=1.6)
for n in ('E2', 'A2', 'D2', 'G3', 'B3', 'E4'):
    f0 = NOTES.get(n, 146.83) * 2 ** (1.6 / 1200)
    check(f'guitar pluck {n} +1.6 c', pluck(f0, 48000), 48000, 0, f0, 0.1, until=1.6)
check('bass pluck B0 at 192 kHz (a shorter run-in)', pluck(NOTES['B0'], 192000), 192000, 1, NOTES['B0'], 0.3, until=1.6)
check('bass, weak fundamental (E1)', pluck(41.203, 48000, weak=True), 48000, 1, 41.203, 0.2, until=1.6)
check('bass, weak fundamental, wide range (A1)', pluck(55.0, 48000, weak=True), 48000, 3, 55.0, 0.2, until=1.6)
for n, B, rg in (('E1', 0.0004, 1), ('B0', 0.0007, 1), ('E2', 0.0001, 0), ('A2', 0.00015, 0)):
    f0 = NOTES[n]
    check(f'stiff string {n} (B = {B})', pluck(f0, 48000, B=B), 48000, rg, f0 * np.sqrt(1 + B), 0.25, until=1.6, spread=0.5)
# a sung A3 with vibrato (+-25 cents, 5.5 Hz): a glottal-ish pulse through two formants
sr = 48000
t = np.arange(int(sr * 2.5)) / sr
f0 = 220.0
inst = f0 * 2 ** (25 / 1200 * np.sin(2 * np.pi * 5.5 * t))
ph = np.cumsum(inst) / sr
src = sum(np.sin(2 * np.pi * h * ph) / h ** 1.2 for h in range(1, 25))
from scipy.signal import lfilter
for F, bw in ((700, 90), (1200, 110)):
    r = np.exp(-np.pi * bw / sr); th = 2 * np.pi * F / sr
    src = lfilter([1 - r], [1, -2 * r * np.cos(th), r * r], src)
voice = 0.2 * src / np.max(np.abs(src))
check('voice A3 with vibrato', voice, sr, 2, f0, 3.0)
# noise and silence: nothing found
for name, x in (('white noise', 0.2 * rng.normal(size=sr * 2)), ('silence', np.zeros(sr * 2))):
    a, ms = run(x, sr, 3)
    found = np.mean(a[:, 1] > 0)
    ok = found < 0.05
    print(f"{'OK ' if ok else 'BAD'} {name:44s} pitch in {found * 100:.1f}% of frames")
    bad += not ok
print(f'{bad} bad')
sys.exit(1 if bad else 0)
