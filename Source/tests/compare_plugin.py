#!/usr/bin/env python3
"""Render a TwoStorms JSFX (through ysfx) and its CLAP port (through clap_host_test) on the same
signals and settings, and report the largest sample difference for each case.

    python3 tests/compare_plugin.py <name> [bundle.clap]

<name> picks tests/cases/<name>.py, which defines:
    JSFX   reference file (relative to the repo), with get_host_placement stubbed
    PLUGIN the CLAP plug-in ID
    NC     main channels (2, or 8 for the Act); the input file holds NC + 2 (sidechain) channels
    CASES  {case name: [ "slider=value", "t<frame>:slider=value", ... ]}
    TOL    (optional) the largest difference counted as a match (default 1e-9)
    SIG    (optional) the test signal for stereo plug-ins (default tests/sig/mix.f64; tests/sig/hits.f64 has gaps);
           a case can pick its own with a "sig=<file>" argument
    SLIDER (optional) function: CLAP parameter ID -> the JSFX's slider number (the Gain Stage numbers them differently)
Environment: SRS=44100,48000  BLOCKS=64,512  ONLY=<substring of case names>  F32=1 (32-bit I/O)
"""
import importlib.util, os, re, subprocess, sys, tempfile
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REF = os.path.join(ROOT, 'build/tools/jsfx_ref')
HOST = os.path.join(ROOT, 'build/linux/clap_host_test')
TMP = tempfile.mkdtemp()


def load_cases(name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(ROOT, 'tests/cases', name + '.py'))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def signal(nc, name='tests/sig/mix.f64'):
    """tests/sig/mix.f64 holds 4 channels (in L, in R, sidechain L, sidechain R); for more main
    channels, the extra ones are the main pair, delayed and scaled differently per channel."""
    mix = np.fromfile(os.path.join(ROOT, name)).reshape(-1, 4)
    if nc == 2:
        path = os.path.join(ROOT, name)
        return path, len(mix)
    n = len(mix)
    out = np.zeros((n, nc + 2))
    for c in range(nc):
        src = mix[:, c % 2]
        d = 37 * (c // 2)
        g = [1.0, 0.8, 0.5, 1.3, 0.25, 0.9, 0.7, 1.1][c]
        out[d:, c] = src[:n - d] * g if d else src * g
    out[:, nc] = mix[:, 2]
    out[:, nc + 1] = mix[:, 3]
    path = os.path.join(TMP, f'sig{nc}.f64')
    out.astype(np.float64).tofile(path)
    return path, n


def jsfx_args(m, args):
    """the case's "id=value" arguments with the JSFX's slider numbers"""
    f = getattr(m, 'SLIDER', None)
    if not f:
        return args
    out = []
    for a in args:
        mm = re.match(r'^(t\d+:)?(\d+)=(.*)$', a)
        if not mm:   # tempo=, play=, MIDI events: as they are
            out.append(a)
            continue
        out.append(f'{mm.group(1) or ""}{f(int(mm.group(2)))}={mm.group(3)}')
    return out


def run(cmd, env=None):
    r = subprocess.run(cmd, capture_output=True, text=True, env=env)
    if r.returncode != 0:
        print('FAILED', ' '.join(cmd), r.stdout[-2000:], r.stderr[-2000:])
        sys.exit(1)
    return r.stdout + r.stderr


def main():
    name = sys.argv[1]
    bundle = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, 'build/linux/TwoStorms.clap')
    m = load_cases(name)
    nc = getattr(m, 'NC', 2)
    tol = getattr(m, 'TOL', 1e-9)
    jsfx = os.path.join(ROOT, m.JSFX)
    sig, _ = signal(nc, getattr(m, 'SIG', 'tests/sig/mix.f64'))
    blocks = [int(b) for b in os.environ.get('BLOCKS', '512').split(',')]
    srs = [int(s) for s in os.environ.get('SRS', '48000').split(',')]
    only = os.environ.get('ONLY')
    env = dict(os.environ, TS_PLUGIN=m.PLUGIN)
    extra = ['--f32'] if os.environ.get('F32') else []
    worst, fails = 0.0, 0
    for sr in srs:
        for blk in blocks:
            for cname, args in m.CASES.items():
                if only and only not in cname:
                    continue
                a = os.path.join(TMP, 'a.f64'); b = os.path.join(TMP, 'b.f64')
                csig = sig
                for x in args:
                    if x.startswith('sig='):
                        csig, _ = signal(nc, x[4:])
                args = [x for x in args if not x.startswith('sig=')]
                # "env:NAME=value": the CLAP render's environment only; "jsfx:p=v": the JSFX's sliders only; "clap:p=v": the CLAP's only
                cenv = dict(env)
                for x in args:
                    if x.startswith('env:'):
                        k, v = x[4:].split('=', 1)
                        cenv[k] = v.replace('$ROOT', ROOT)
                jargs = [x[5:] if x.startswith('jsfx:') else x for x in args if not x.startswith('env:') and not x.startswith('clap:')]
                cargs = [x[5:] if x.startswith('clap:') else x for x in args if not x.startswith('env:') and not x.startswith('jsfx:')]
                run([REF, jsfx, csig, a, str(sr), str(blk), f'inch={nc + 2}', f'outch={nc}'] + jsfx_args(m, jargs))
                run([HOST, 'render', bundle, csig, b, str(sr), str(blk)] + cargs + extra, cenv)
                x = np.fromfile(a); y = np.fromfile(b)
                d = np.abs(x - y).max() if len(x) == len(y) else float('inf')
                ref = np.abs(x).max()
                db = 20 * np.log10(d / ref) if d > 0 and ref > 0 else -999
                ok = d <= tol
                if not ok: fails += 1
                worst = max(worst, d)
                print(f"{'OK ' if ok else 'BAD'} sr={sr} blk={blk:5d} {cname:30s} maxdiff={d:.3e} ({db:7.1f} dB re peak {ref:.3f})", flush=True)
    print(f'worst {worst:.3e}, {fails} cases above {tol:g}')
    return fails


if __name__ == '__main__':
    sys.exit(1 if main() else 0)
