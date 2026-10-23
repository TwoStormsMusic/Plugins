JSFX = 'tests/reference/twostorms_gain_stage-fixed_ref.jsfx'
PLUGIN = 'com.twostorms.gainstage'
NC = 8

def SLIDER(p):
    # the JSFX's si(): ON/OFF is slider 1, the Act's 1..39 move up one, 40..42 -> 45..47, 50..53 -> 41..44
    if p == 54:
        return 1
    return p + 1 if p < 40 else (p + 5 if p < 43 else p - 9)

CASES = {
    'default': [],
    'gain': ['10=-6'],
    'off': ['54=0'],
    'off_glide': ['t30000:54=0', 't90000:54=1'],
    'off_glide_long': ['50=500', 't30000:54=0', 't70000:54=1'],
    'off_glide_zero': ['50=0', 't30000:54=0', 't60000:54=1'],
    'glide_with_gain': ['10=-3', '50=200', 't20000:54=0', 't40000:10=2', 't60000:54=1'],
    'ch4_unlinked': ['1=1', '6=0', '10=-3', '11=2', '12=4', 't30000:54=0'],
    'unused_off': ['19=0', 't20000:54=0'],
    'gain_auto': ['10=-6', 't30000:10=4'],
}
