JSFX = 'tests/reference/twostorms_defitcher_ref.jsfx'
PLUGIN = 'com.twostorms.defitcher'
NC = 2
SIG = 'tests/sig/sib.f64'
CASES = {
    'default': [],
    'all_bands': ['55=1', '71=1', '58=-36'],
    'classic': ['50=1', '55=1', '71=1'],
    'classic_ms': ['50=1', '51=1', '55=1'],
    'targeted_ms': ['51=1', '55=1', '71=1'],
    'mid': ['51=2', '66=-40'],
    'side': ['51=3', '66=-45'],
    'lookahead_1': ['52=1'],
    'lookahead_5_classic': ['52=2', '50=1'],
    'sidechain': ['80=1', '55=1', '71=1'],
    'monitor_band': ['54=1', '55=1'],
    'monitor_removed': ['54=2', '55=1', '71=1'],
    'mix_50': ['79=50', '55=1'],
    'fast_heavy': ['55=1', '57=2.5', '58=-38', '59=20', '60=8', '61=0.2', '62=30'],
    'auto_learn': ['53=1', '55=1', '71=1'],
    'auto_reset': ['53=2', '55=1', '71=1', 't80000:53=1'],
    'auto_sidechain': ['53=1', '80=1', '55=1'],
    'automation': ['t30000:64=8000', 't60000:50=1', 't90000:52=2', 't110000:51=1'],
    'hits_signal': ['sig=tests/sig/hits.f64', '55=1', '71=1', '58=-45', '66=-45', '74=-45'],
}
