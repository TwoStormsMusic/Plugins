#!/usr/bin/env python3
"""The Tuner as a plug-in (its pitch reading is tests/tuner_pitch.py): the audio passes untouched; Mute fades it out
in 10 ms and back in 10 ms (also automated); its settings survive a state save and load; and its
Mute silences the Gain Stages it listens to (two of them), which play again once the Tuner lets go, and only the
Tuner with "Mute all tracks heard" off (clap_host_test tunermute)."""
import os, subprocess, sys, tempfile
import numpy as np
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST = os.path.join(ROOT, 'build/linux/clap_host_test')
BUNDLE = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'build/linux/TwoStorms.clap')
TMP = tempfile.mkdtemp()
env = dict(os.environ, TS_PLUGIN='com.twostorms.tuner')
SR = 48000
bad = 0


def ok(c, msg):
    global bad
    print(('OK  ' if c else 'BAD ') + msg)
    bad += not c


def render(sig, args, extra=()):
    i = os.path.join(TMP, 'i.f64'); o = os.path.join(TMP, 'o.f64')
    z = np.zeros(len(sig))
    np.stack([sig, -sig, z, z], 1).astype(np.float64).tofile(i)
    subprocess.run([HOST, 'render', BUNDLE, i, o, str(SR), '512'] + list(args) + list(extra), env=env, check=True, capture_output=True)
    return np.fromfile(o).reshape(-1, 2)


n = SR * 2
x = 0.5 * np.sin(2 * np.pi * 110 * np.arange(n) / SR)
y = render(x, [])
ok(np.max(np.abs(y[:, 0] - x)) == 0 and np.max(np.abs(y[:, 1] + x)) == 0, 'Mute off: the audio passes untouched')
y = render(x, ['53=1'])
ok(np.max(np.abs(y[SR // 100 + 2:])) < 1e-12, f'Mute on: silent after its 10 ms fade (peak {np.max(np.abs(y[SR // 100 + 2:])):.2g})')
# automation: on at 0.5 s, off at 1.2 s (the plug-ins take parameter changes at the start of the block they fall in)
a, b = SR // 2, int(SR * 1.2)
y = render(x, [f't{a}:53=1', f't{b}:53=0'])
a, b = a // 512 * 512, b // 512 * 512
f = int(0.01 * SR)
ok(np.max(np.abs(y[:a, 0] - x[:a])) == 0, 'automated Mute: untouched before')
ok(np.max(np.abs(y[a + f + 1:b])) < 1e-12, 'automated Mute: silent from 10 ms after it goes on')
ok(np.max(np.abs(y[b + f + 1:, 0] - x[b + f + 1:])) < 1e-12, 'automated Mute: whole again 10 ms after it goes off')
d = np.diff(y[:, 0])
ok(np.max(np.abs(d)) < 0.5 * 2 * np.pi * 110 / SR * 1.5, f'no clicks at the fades (largest step {np.max(np.abs(d)):.4f})')
# state: the settings come back
r = subprocess.run([HOST, 'render', BUNDLE, os.path.join(TMP, 'i.f64'), os.path.join(TMP, 'o.f64'), str(SR), '512', '54=442', '55=1', '57=3',
                    '58=2', '56=1', '--state'], env=env, capture_output=True, text=True)
ok(r.returncode == 0, 'state saved and loaded with the settings changed' + ('' if r.returncode == 0 else ': ' + r.stdout[-300:] + r.stderr[-300:]))
# the Gain Stage it listens to
r = subprocess.run([HOST, 'tunermute', BUNDLE], capture_output=True, text=True)
ok(r.returncode == 0, 'Mute silences both Gain Stages listened to (or only the Tuner), and lets them go: ' + r.stdout.strip())
print(f'{bad} bad')
sys.exit(1 if bad else 0)
