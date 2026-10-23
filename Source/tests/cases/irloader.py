JSFX = 'tests/irtest/Effects/twostorms_ir_loader_ref.jsfx'   # in a folder with Data/TwoStorms IRs (its file sliders)
PLUGIN = 'com.twostorms.irloader'
NC = 2
SIG = 'tests/sig/mix.f64'
# IR files: the JSFX picks them by number in Data/TwoStorms IRs (1 a, 2 b, 3 c), the CLAP reads the same files
D = '$ROOT/tests/irtest/Data/TwoStorms IRs/'
A = ['jsfx:100=1', 'env:TS_TEST_IR1=' + D + 'a_cab_48k_mono.wav']        # 48 kHz mono, 24-bit
B = ['jsfx:100=2', 'env:TS_TEST_IR1=' + D + 'b_cab_44k_stereo.wav']      # 44.1 kHz stereo (resampled)
C = ['jsfx:100=3', 'env:TS_TEST_IR1=' + D + 'c_long_96k_mono.wav']       # 96 kHz mono, 0.5 s, float
B2 = ['jsfx:101=2', 'env:TS_TEST_IR2=' + D + 'b_cab_44k_stereo.wav', '70=1']
C2 = ['jsfx:101=3', 'env:TS_TEST_IR2=' + D + 'c_long_96k_mono.wav', '70=1']
SLOW = ['env:TS_TEST_IRBUDGET=1', 'clap:100=0']   # the JSFX's build pace; the JSFX doesn't normalize
CASES = {
    'no_ir': SLOW,
    'a_mono48': SLOW + A,
    'b_stereo44': SLOW + B,
    'c_long96': SLOW + C,
    'c_trim_off': SLOW + C + ['97=0'],
    'c_trim_60': SLOW + C + ['98=-60'],
    'two_ab': SLOW + A + B2,
    'two_ac_plain': SLOW + A + C2 + ['95=0', '96=0', '93=0'],
    'two_bc': SLOW + B + C2,
    'lat64': SLOW + A + ['90=1'],
    'lat128_stereo': SLOW + B + ['90=2'],
    'lat256_two': SLOW + A + B2 + ['90=3'],
    'mono_in_mono_ir': SLOW + A + ['91=1'],
    'mono_in_stereo_ir': SLOW + B + ['91=1'],
    'stereo_irs_off': SLOW + B + ['92=0'],
    'level_pan_delay_phase': SLOW + A + ['51=-6', '52=40', '53=1.37', '54=1'],
    'filters': SLOW + B + ['55=1', '56=120', '59=2', '57=1', '58=5000', '60=1'],
    'width': SLOW + B + ['61=160'],
    'mix': SLOW + A + ['94=40'],
    'solo2': SLOW + A + B2 + ['82=1'],
    'ir_off': SLOW + A + ['50=0'],
    'pan_two': SLOW + A + B2 + ['52=-60', '72=70', '73=2.5'],
    'automation': SLOW + B + ['t40000:51=-6', 't80000:55=1', 't80000:56=200', 't120000:61=50'],
    'auto_two': SLOW + A + B2 + ['t50000:70=0', 't100000:70=1', 't130000:81=40'],
    'hits': SLOW + A + ['sig=tests/sig/hits.f64'],
}
