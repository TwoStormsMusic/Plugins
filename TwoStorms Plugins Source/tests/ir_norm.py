#!/usr/bin/env python3
"""The IR Loader's Normalize (the CLAP's own): pink noise through cab-like IRs (peak-normalized files, +14 dB louder
than the input raw) comes out as loud as it went in (K-weighted, as LUFS: within 0.5 dB), one IR or two (two blend
instead of adding up, normalized as they play at 0 dB), IR levels act on top (no normalizing again), Off plays them raw."""
import os, subprocess, sys, tempfile, wave
import numpy as np
from scipy.signal import lfilter
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST = os.path.join(ROOT, 'build/linux/clap_host_test')
BUNDLE = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'build/linux/TwoStorms.clap')
TMP = tempfile.mkdtemp()
SR, N = 48000, 8192
rng = np.random.default_rng(5)


def cab(seed):
    """a cab-like IR: high pass ~80 Hz, a low bump, a presence peak, steep above 5 kHz, ripple; minimum phase"""
    r = np.random.default_rng(seed)
    f = np.fft.rfftfreq(N, 1 / SR); f[0] = 1
    d = -20 * np.log10(1 + (80 / f) ** 2) + 4 * np.exp(-0.5 * (np.log2(f / 110) / 0.25) ** 2)
    d += 6 * np.exp(-0.5 * (np.log2(f / 2500) / 0.4) ** 2) - 40 * np.log10(1 + (f / 5000) ** 4)
    for _ in range(12):
        d += r.normal(0, 1.5) * np.sin(np.log2(f) * r.uniform(3, 12) + r.uniform(0, 6))
    m = 10 ** (d / 20)
    full = np.concatenate([m, m[-2:0:-1]])
    c = np.fft.ifft(np.log(np.maximum(full, 1e-8))).real
    w = np.zeros(N); w[0] = 1; w[1:N // 2] = 2; w[N // 2] = 1
    h = np.fft.ifft(np.exp(np.fft.fft(c * w))).real[:2048] * np.hanning(4096)[2048:]
    return h


def write(path, chans):
    x = np.stack(chans, 1); x = x / np.max(np.abs(x)) * 0.999
    q = np.round(x * (2 ** 23 - 1)).astype(np.int64).reshape(-1)
    b = bytearray()
    for v in q: b += int(v & 0xffffff).to_bytes(3, 'little')
    w = wave.open(path, 'wb'); w.setnchannels(x.shape[1]); w.setsampwidth(3); w.setframerate(SR); w.writeframes(bytes(b)); w.close()


A = os.path.join(TMP, 'cab1.wav'); B = os.path.join(TMP, 'cab2st.wav')
write(A, [cab(1)]); write(B, [cab(2), cab(3)])
T = os.path.join(ROOT, 'tests/irtest/Data/TwoStorms IRs/')


def pink(n):
    X = np.fft.rfft(rng.normal(size=n)); f = np.fft.rfftfreq(n, 1 / SR); f[0] = 1; X /= np.sqrt(f); X[0] = 0
    x = np.fft.irfft(X, n); return x / np.sqrt(np.mean(x ** 2)) * 0.1


n = SR * 6
sig = os.path.join(TMP, 'pink.f64')
inp = np.stack([pink(n), pink(n)], 1)
np.concatenate([inp, np.zeros((n, 2))], 1).tofile(sig)


def kw(x):
    x = lfilter([1.53512485958697, -2.69169618940638, 1.19839281085285], [1, -1.69065929318241, 0.73248077421585], x)
    return lfilter([1, -2, 1], [1, -1.99004745483398, 0.99007225036621], x)


def lufs(x):
    x = x[-4 * SR:]
    return 10 * np.log10(np.mean(kw(x[:, 0]) ** 2) + np.mean(kw(x[:, 1]) ** 2))


def render(args, ir1=None, ir2=None):
    e = dict(os.environ, TS_PLUGIN='com.twostorms.irloader')
    if ir1: e['TS_TEST_IR1'] = ir1
    if ir2: e['TS_TEST_IR2'] = ir2
    o = os.path.join(TMP, 'o.f64')
    subprocess.run([HOST, 'render', BUNDLE, sig, o, str(SR), '512'] + args, env=e, check=True, capture_output=True)
    return np.fromfile(o).reshape(-1, 2)


L0 = lufs(inp)
bad = 0
CASES = [  # name, args, IR 1, IR 2, expected dB (re the input), tolerance
    ('one IR, off (raw)', ['100=0'], A, None, None, 0),
    ('one mono IR', [], A, None, 0, 0.5),
    ('one stereo IR', [], B, None, 0, 0.5),
    ('one IR at -6 dB', ['51=-6'], A, None, -6, 0.5),
    ('two IRs', ['70=1'], A, B, 0, 0.5),
    ('the same IR twice', ['70=1'], A, A, 0, 0.5),
    ('two, IR 2 turned down', ['70=1', '71=-24'], A, B, 'down', 0),
    ('two, both at -6 dB', ['70=1', '51=-6', '71=-6'], A, B, -6, 0.5),
    ('two, delayed 3 ms', ['70=1', '73=3'], A, B, 0, 0.5),
    ('bass-heavy test IR', [], T + 'a_cab_48k_mono.wav', None, 0, 0.5),
    ('96 kHz IR, resampled', [], T + 'c_long_96k_mono.wav', None, 0, 0.5),
]
for name, args, i1, i2, want, tol in CASES:
    d = lufs(render(args, i1, i2)) - L0
    if want is None:
        ok = d > 10
        print(f"{'OK ' if ok else 'BAD'} {name:28s} {d:+6.2f} dB (raw: louder than the input)")
    elif want == 'down':
        # not normalized again: IR 1 alone at its share of the pair, well under the input
        ok = d < -3
        print(f"{'OK ' if ok else 'BAD'} {name:28s} {d:+6.2f} dB (want under -3: the levels aren't normalized again)")
    else:
        ok = abs(d - want) <= tol
        print(f"{'OK ' if ok else 'BAD'} {name:28s} {d:+6.2f} dB (want {want:+.0f} +- {tol})")
    bad += not ok
print(f'{bad} bad')
sys.exit(1 if bad else 0)
