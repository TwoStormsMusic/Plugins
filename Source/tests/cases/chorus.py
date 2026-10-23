JSFX = 'tests/reference/twostorms_chorus_ref.jsfx'
PLUGIN = 'com.twostorms.chorus'
NC = 2
CASES = {
    'default': [],
    'voices8': ['55=8', '56=100'],
    'voices1': ['55=1', '47=70'],
    'ensemble': ['50=1', '54=14', '53=3'],
    'flanger': ['50=2', '54=2.5', '53=2', '51=0.2', '60=60', '62=20'],
    'flanger_negfb_xfb': ['50=2', '60=-80', '68=50', '57=120'],
    'vibrato': ['50=3', '51=5', '53=3', '54=6'],
    'through_zero': ['50=4', '54=5', '53=4.5', '51=0.15', '60=50', '62=20', '57=60'],
    'tz_switch': ['t40000:50=4', 't90000:50=0'],
    'triangle': ['58=1', '51=1.5'],
    'smooth_random': ['58=2', '61=80'],
    'tape': ['64=1', '65=60'],
    'tape_age0_gaps': ['64=1', '65=0', 'sig=tests/sig/hits.f64'],
    'bbd': ['64=2', '65=50', '54=10'],
    'bbd_old': ['64=2', '65=100', '54=25', '60=40'],
    'drive_hq': ['66=70'],
    'drive_std': ['66=70', '69=0'],
    'std_quality': ['69=0', '55=6'],
    'filters': ['62=500', '63=3000'],
    'width_wide': ['67=200'],
    'width_mono': ['67=0'],
    'input_gain': ['46=9', '47=80'],
    'all_wet': ['47=100'],
    'all_dry': ['47=0'],
    'sync_play': ['52=10', 'tempo=128', 'play=1'],
    'sync_stopped': ['52=7', 'tempo=90'],
    'sync_play_offset': ['52=13', 'tempo=100', 'play=1', 'beat0=2.25'],
    'silence_gaps': ['sig=tests/sig/hits.f64', '60=30'],
    'automation': ['t30000:55=8', 't60000:54=20', 't80000:64=2', 't100000:60=-40', 't120000:69=0'],
    'presetlike_jet': ['47=50', '50=2', '51=0.15', '53=2.5', '54=2', '55=1', '60=70'],
}
# The CLAP's High quality goes beyond the JSFX's (4x drive and BBD, per-sample LFOs, six-point reads): compare the
# Standard quality, which is the JSFX's sample for sample. (tests/chorus_hq.py checks High on its own.)
CASES = {k: ['69=0'] + [a for a in v if not a.split(':')[-1].startswith('69=')] for k, v in CASES.items()}
