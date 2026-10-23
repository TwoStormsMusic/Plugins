# The CLAP Tremolo grew past the JSFX (three modes, one to four grids, step levels, the gap, the pitch); what is left of
# the JSFX matches it: the LFO, and one grid at full level (the JSFX's Depth is the grid's depth here: clap:92=70).
JSFX = 'tests/reference/twostorms_tremolo_ref.jsfx'
PLUGIN = 'com.twostorms.tremolo'
NC = 2
G = 'clap:92=70'
CASES = {
    'default': [G],
    'sine4': ['50=0'],
    'triangle_skew': ['50=0', '55=1', '56=30', '57=90', '58=45'],
    'square_fast': ['50=0', '55=2', '53=12', '59=1', '60=1'],
    'ramps': ['50=0', '55=3', 't70000:55=4'],
    'random_step': ['50=0', '55=5', '53=6', '58=60'],
    'random_smooth': ['50=0', '55=6', '53=5', '58=90'],
    'sync8_play': ['50=0', '54=9', 'tempo=128', 'play=1'],
    'sync_bar_34': ['50=0', '54=3', 'tempo=100', 'play=1', 'ts=3/4', 'beat0=1.5'],
    'pattern_play': [G, 'tempo=140', 'play=1', 'beat0=0.37'],
    'pattern_swing_triplet': [G, '65=3', '66=60', 'jsfx:64=12', 'clap:68=12', 'tempo=120', 'play=1'],
    'pattern_stopped': [G, 'tempo=90', 'jsfx:64=24', 'clap:68=24'],
    'pattern_stereo': [G, 'tempo=120', 'play=1', '58=90', 'jsfx:64=16'],
    'lfo_x_pattern': [G, '50=2', '54=9', 'tempo=110', 'play=1'],
    'pattern_autopan': [G, '51=1', 'tempo=120', 'play=1'],
    'pattern_harmonic': [G, '51=2', '62=900', 'tempo=120', 'play=1'],
    'autopan': ['51=1', '50=0', '53=2'],
    'harmonic': ['51=2', '50=0', '62=900', '52=90'],
    'clock_free': [G, '61=1', 'tempo=130'],
    'midi_restart': ['61=2', '50=0', '53=3', 'm20000:144,60,100', 'm50000:144,62,90', 'm50010:128,62,0', 'm90000:144,64,1'],
    'onset_restart': ['61=3', '50=0', '53=2', '75=-25'],
    'mix50': ['63=50', '50=0'],
    'automation': ['50=0', 't30000:52=100', 't60000:51=1', 't90000:51=2'],
}
