JSFX = 'tests/reference/twostorms_delay_ref.jsfx'
PLUGIN = 'com.twostorms.delay'
NC = 2
SIG = 'tests/sig/hits.f64'
# The CLAP: three lines (Line C, 150-189, off), the modulation slots moved up by 20 (190.. here, 170.. in the JSFX),
# dry and wet levels instead of the JSFX's dry/wet balance (equal power: its default 30% is these two levels), the
# routing per pair (45: A and B, 0 parallel, 1 series, 2 linked; the JSFX's 45 was 0 parallel, 1 A to B, 2 B to A, 3
# linked). Reverse is the CLAP's own (on the way out): tests/delay_new.py.
SLIDER = lambda p: p - 20 if p >= 190 else p
M30 = ['clap:47=-1.0023823186197764', 'clap:69=-6.8590647029398975']
CASES0 = {
    'default': [],
    'free_time': ['72=0', '71=180'],
    'stereo_times_cross': ['75=0', '72=0', '71=120', '74=0', '73=200', '77=30'],
    'pingpong_l': ['78=1'],
    'pingpong_r': ['78=2', '76=70'],
    'parallel_b': ['110=1'],
    'series_ab': ['45=1', '110=1'],
    'linked': ['jsfx:45=3', 'clap:45=2', '110=1', '58=70'],
    'taps_accel': ['99=3', '100=1', '101=70'],
    'taps_gallop': ['99=2', '100=3'],
    'tape': ['82=1', '83=50', '84=40', '85=30'],
    'bbd': ['82=2', '83=60'],
    'lofi': ['82=3', '83=70'],
    'drive_hq': ['87=60', '88=12'],
    'drive_std': ['67=0', '87=50', '88=19'],
    'drive_fold': ['87=80', '88=20'],
    'filters_bell': ['89=300', '90=3000', '92=6', '91=800', '93=2'],
    'diffusion': ['94=60', '95=40'],
    'pitch_up_hq': ['96=1', '97=7'],
    'pitch_down_std': ['67=0', '96=1', '97=-12'],
    'glide_auto': ['72=0', 't50000:71=500', 't90000:71=60'],
    'feedback_110': ['76=110'],
    'freeze': ['t60000:55=1', 't100000:55=0'],
    'duck_input': ['50=80', '51=-40'],
    'duck_sidechain': ['50=100', '54=1', '52=5', '53=200'],
    'reverb': ['60=50', '61=3'],
    'shimmer_fifth': ['60=40', '64=60', '65=1'],
    'shimmer_octdown': ['60=60', '64=100', '65=3', '62=60'],
    'sync_play': ['72=10', 'tempo=100', 'play=1'],
    'mod_lfo_time': ['190=1', '191=1', '192=0', '193=30', '194=2'],
    'mod_lfo_random': ['190=1', 'jsfx:191=3', 'clap:191=4', '192=1', '196=5', '194=3'],
    'mod_lfo_synced': ['190=1', '191=1', '192=3', '196=1', '197=4', 'tempo=120', 'play=1'],
    'mod_follower_in': ['190=2', '191=1', '192=2', '196=0', '194=5', '195=100'],
    'mod_follower_sc': ['190=2', '191=1', '192=5', '196=2', '193=-60'],
    'mod_follower_wet': ['190=2', '191=1', '192=6', '196=9'],
    'mod_midi_cc': ['190=3', '191=1', '192=7', '196=1', 'm10000:176,1,100', 'm50000:176,1,20'],
    'mod_midi_vel': ['190=3', '191=1', '192=1', '196=128', 'm20000:144,60,90', 'm70000:128,60,0'],
    'mod_pitchbend': ['190=3', '191=1', '192=8', '196=129', '96=1', 'm30000:224,0,100'],
    'mod_global_freeze': ['190=1', '191=0', '192=3', '193=100', '194=0.5', '196=4'],
    'mod_global_reverb': ['190=1', '191=0', '192=4', '193=60', '194=0.7'],
    'mod_two_slots': ['190=1', 'jsfx:191=3', 'clap:191=4', '192=10', '198=2', '199=2', '200=11', '110=1', '193=-40'],
    'input_mix': ['46=6', 'jsfx:47=100', 'clap:47=-60', 'clap:69=0'],
    'level_pan_width': ['79=-3', '80=-50', '81=150'],
    'mix_signal': ['sig=tests/sig/mix.f64', '76=60', '87=30'],
}

# every case at the JSFX's default balance (a case's own levels come after)
CASES = {k: M30 + v for k, v in CASES0.items()}
