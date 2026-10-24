JSFX = 'tests/reference/twostorms_multiband_fixed_ref.jsfx'   # with the fix: a band that is off gets no saturation
PLUGIN = 'com.twostorms.multiband'
NC = 2
SIG = 'tests/sig/mix.f64'
# crossovers: 50/52/54 frequency, 51/53/55 used; 56 linear phase; 57 input, 58 mix, 59 detection, 60 RMS time,
# 61 channel mode, 62 link, 63 link law, 64 sidechain, 65 sidechain trim, 66 lookahead, 67 its time, 68 delta, 84 hybrid
# band b's parameter k: 90 + 20 b + k (on 0, solo 1, mode 2, threshold 3, ratio 4, knee 5, attack 6, release 7, range 8,
# makeup 9, auto makeup 10, hold 11, character 12, base 13, envelope 14, release mode 15, curve 16, feedback 17,
# saturation 18, saturation type 19)
def band(b, **kw):
    K = dict(on=0, solo=1, mode=2, thr=3, rat=4, knee=5, att=6, rel=7, rng=8, mk=9, amk=10, hld=11, ch=12, base=13,
             env=14, relm=15, curve=16, fb=17, sat=18, satt=19)
    return [f'{90 + 20 * b + K[k]}={v}' for k, v in kw.items()]

HOT = band(0, thr=-36, rng=-12) + band(1, thr=-36, rng=-12) + band(2, thr=-36, rng=-12) + band(3, thr=-36, rng=-12)
CASES = {
    'default': [],
    'two_hot': HOT,
    'one_band': ['51=0', '93=-36', '98=-12'],
    'three_bands': ['53=1'] + HOT,
    'four_bands': ['53=1', '55=1'] + HOT,
    'four_unsorted': ['50=5000', '52=200', '54=1200', '53=1', '55=1'] + HOT,
    'close_crossovers': ['50=1000', '52=1100', '54=1150', '53=1', '55=1'] + HOT,
    'xo_automation': ['53=1'] + HOT + ['t30000:50=400', 't60000:52=6000', 't90000:55=1', 't120000:53=0'],
    'expand': ['53=1'] + band(0, mode=1, thr=-30, rng=6) + band(1, mode=1, thr=-40, rng=12, rat=3) + band(2, mode=1, thr=-20, rng=24, rat=100),
    'expand_down': ['53=1'] + band(0, mode=1, thr=-30, rng=-12) + band(1, mode=1, thr=-40, rng=-20, rat=4),
    'ratio_inf_knee0': ['53=1'] + band(0, thr=-30, rat=100, knee=0, rng=-20) + band(1, thr=-30, rat=1.5, knee=24, rng=-20),
    'hold': ['53=1'] + band(0, thr=-36, rng=-12, hld=80) + band(1, thr=-36, rng=-12, hld=300, att=0.1, rel=5),
    'rms': ['59=1', '60=30', '53=1'] + HOT,
    'hybrid': ['59=2', '60=50', '84=70', '53=1'] + HOT,
    'rms_short': ['59=1', '60=0.5', '53=1'] + HOT,
    'mono': ['61=0', '53=1'] + HOT,
    'stereo_link50': ['61=1', '62=50', '53=1'] + HOT,
    'stereo_link0_power': ['61=1', '62=0', '63=1', '53=1'] + HOT,
    'mid_side': ['61=2', '62=30', '53=1'] + HOT,
    'left_right': ['61=3', '62=0', '53=1'] + HOT,
    'sidechain': ['64=1', '65=6', '53=1'] + HOT,
    'lookahead': ['66=1', '67=5', '53=1'] + HOT,
    'lookahead_long': ['66=1', '67=10', '55=1', '53=1'] + HOT,
    'delta': ['68=1', '53=1'] + HOT,
    'mix_input': ['57=6', '58=40', '53=1'] + HOT,
    'solo': ['53=1'] + HOT + band(1, solo=1),
    'solo_two': ['53=1', '55=1'] + HOT + band(0, solo=1) + band(3, solo=1),
    'band_off': ['53=1'] + HOT + band(1, on=0),
    'makeup': ['53=1'] + HOT + band(0, mk=4) + band(2, mk=-3),
    'auto_makeup': ['53=1'] + HOT + band(0, amk=1) + band(1, amk=1),
    'char_custom_raw': ['53=1'] + HOT + band(0, ch=0, env=0, relm=0) + band(1, ch=0, env=0, relm=2) + band(2, ch=0, env=1, relm=1, curve=1),
    'char_opto': ['53=1'] + HOT + band(0, ch=2) + band(1, ch=2) + band(2, ch=2),
    'char_fet': ['53=1'] + HOT + band(0, ch=3) + band(1, ch=3) + band(2, ch=3),
    'char_varimu': ['53=1'] + HOT + band(0, ch=4) + band(1, ch=4) + band(2, ch=4),
    'char_mixed': ['53=1', '55=1'] + HOT + band(0, ch=2) + band(1, ch=3) + band(2, ch=4) + band(3, ch=1),
    'custom_sat_warm': ['53=1'] + HOT + band(0, ch=0, sat=60, satt=1, fb=30) + band(1, ch=0, sat=80, satt=0, curve=2),
    'feedback_la': ['66=1', '53=1'] + HOT + band(0, ch=3) + band(1, ch=2),
    'off_band_fet': ['53=1'] + HOT + band(1, on=0, ch=3),
    'linear': ['56=1', '53=1'] + HOT,
    'linear_four': ['56=1', '53=1', '55=1'] + HOT,
    'linear_two_la': ['56=1', '66=1', '67=8'] + HOT,
    'linear_sidechain': ['56=1', '64=1', '53=1'] + HOT,
    'linear_xo_move': ['56=1', '53=1'] + HOT + ['t40000:50=300', 't40100:50=320', 't80000:52=5000'],
    'linear_mid_side': ['56=1', '61=2', '53=1'] + HOT,
    'mode_switch': ['53=1'] + HOT + ['t50000:56=1', 't100000:56=0'],
    'band_count_switch': HOT + ['t40000:53=1', 't80000:55=1', 't120000:51=0'],
    'hits': ['53=1', 'sig=tests/sig/hits.f64'] + HOT + band(0, mode=1, thr=-40, rng=-30, rat=10),
}
