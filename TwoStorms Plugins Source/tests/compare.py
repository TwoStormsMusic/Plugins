#!/usr/bin/env python3
"""Render the original JSFX (through ysfx) and the C++ port on the same signals and settings, and
report the largest sample difference for each case."""
import os, subprocess, sys, tempfile
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REF = os.path.join(ROOT, 'build/tools/jsfx_ref')
JSFX = os.path.join(ROOT, 'tests/reference/compressor_ref.jsfx')
PORT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'build/test/engine_run')
SIG = os.path.join(ROOT, 'tests/sig/mix.f64')
TMP = tempfile.mkdtemp()

CASES = {
    'default': [],
    'vca_hard': ['50=-30', '51=8', '52=0', '53=1', '55=50'],
    'ratio_inf': ['50=-24', '51=100', '52=3'],
    'opto': ['101=2'],
    'fet': ['101=3', '50=-28'],
    'varimu': ['101=4', '50=-28'],
    'custom_raw_classic': ['101=0', '88=0', '89=0', '100=0'],
    'custom_soft_2stage_fb': ['101=0', '89=2', '100=1', '90=70', '102=40', '103=1'],
    'custom_optolike_prog': ['101=0', '89=1', '100=2', '90=30', '102=20', '103=0'],
    'rms': ['57=1', '87=30'],
    'rms_long': ['57=1', '87=500', '53=200', '55=900'],
    'hybrid': ['57=2', '92=70'],
    'mono': ['58=0'],
    'mono_hybrid': ['58=0', '57=2'],
    'midside': ['58=2', '59=40'],
    'leftright': ['58=3'],
    'link50': ['59=50'],
    'link50_power': ['59=50', '93=1'],
    'link100_power': ['93=1'],
    'lookahead5': ['67=1', '68=5'],
    'lookahead_fastatt': ['67=1', '68=10', '53=2'],
    'lookahead_rms_hold': ['67=1', '68=3', '57=1', '54=40'],
    'hold': ['54=80', '55=300'],
    'range6': ['56=6', '50=-35'],
    'sidechain': ['65=1'],
    'sidechain_trim': ['65=1', '98=-6', '95=120', '96=2'],
    'hpf6': ['95=150', '96=0'],
    'hpf12_tilt': ['95=300', '96=1', '97=6'],
    'tilt_neg': ['97=-9'],
    'range1': ['74=1', '75=60', '76=250'],
    'ranges2_loud': ['74=1', '77=1', '78=2000', '79=5000'],
    'ranges2_comb': ['74=1', '77=1', '94=1'],
    'ranges_all_rms': ['74=1', '77=1', '80=1', '83=1', '57=1'],
    'automakeup_avg': ['62=1'],
    'automakeup_static': ['62=1', '99=1'],
    'drywet40_makeup': ['61=40', '62=1'],
    'input_plus6': ['60=6'],
    'delta': ['64=1'],
    'listen': ['66=1', '74=1'],
    'listen_mono': ['66=1', '58=0'],
    'output_gain': ['10=-3.5'],
    'output_ramp': ['t40000:10=-12', 't80000:10=4'],
    'automation_thr': ['t30000:50=-40', 't90000:50=-10'],
    'la_toggle': ['t50000:67=1', 't100000:67=0'],
    'la_time_change': ['67=1', 't60000:68=12'],
    'char_switch': ['t50000:101=3', 't100000:101=4'],
    'sat_warm_custom': ['101=0', '102=80', '103=1'],
    'preset_parallel_smash': '50=-30 51=20 52=2 53=1 54=0 55=60 56=60 57=0 61=40 62=1 69=12 70=0 89=1 90=60 95=20 100=0 101=3 102=30 103=0'.split(),
    'preset_mixbus': '50=-14 51=2 52=10 53=30 54=0 55=300 56=60 57=0 61=100 62=1 69=2 70=1 89=1 90=40 95=60 100=1 101=4 102=30 103=1'.split(),
}

def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print('FAILED', cmd, r.stderr)
        sys.exit(1)
    return r.stderr

def main():
    blocks = [int(b) for b in os.environ.get('BLOCKS', '512').split(',')]
    srs = [int(s) for s in os.environ.get('SRS', '48000').split(',')]
    only = os.environ.get('ONLY')
    worst = 0.0
    fails = 0
    for sr in srs:
        for blk in blocks:
            for name, args in CASES.items():
                if only and only not in name:
                    continue
                a = os.path.join(TMP, 'a.f64'); b = os.path.join(TMP, 'b.f64')
                run([REF, JSFX, SIG, a, str(sr), str(blk)] + args)
                run([PORT, SIG, b, str(sr), str(blk)] + args)
                x = np.fromfile(a); y = np.fromfile(b)
                d = np.abs(x - y).max() if len(x) == len(y) else float('inf')
                ref = np.abs(x).max()
                db = 20 * np.log10(d / ref) if d > 0 else -999
                flag = 'OK ' if d < 1e-9 else 'BAD'
                if d >= 1e-9: fails += 1
                worst = max(worst, d)
                print(f'{flag} sr={sr} blk={blk:5d} {name:28s} maxdiff={d:.3e} ({db:7.1f} dB re peak {ref:.3f})')
    print(f'worst {worst:.3e}, {fails} cases above 1e-9')
    return fails

if __name__ == '__main__':
    sys.exit(1 if main() else 0)
