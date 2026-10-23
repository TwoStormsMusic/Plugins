# The JSFX's band count (50) is the CLAP's three band switches (155-157).
# Voice (a cascade of formants in the CLAP, the JSFX's are bandpasses side by side) has no reference: tests/wah_voice.py
JSFX = 'tests/reference/twostorms_wah_ref.jsfx'
PLUGIN = 'com.twostorms.wah'
NC = 2
SIG = 'tests/sig/hits.f64'
CASES = {
    'default': [],
    'pedal_auto': ['t30000:59=0', 't60000:59=100', 't90000:59=30'],
    'pedal_midi_cc': ['m10000:176,11,0', 'm40000:176,11,127', 'm70000:176,11,60', 'm71000:177,11,10'],
    'pedal_bend_ch': ['74=128', '75=2', 'm20000:225,0,100', 'm60000:224,0,0', 'm80000:225,127,127'],
    'learn': ['76=1', 'm20000:176,7,100', 'm50000:176,7,10'],
    'pedal_range_curve': ['86=20', '87=80', '88=-60', 'm30000:176,11,127', '66=50'],
    'envelope': ['58=1', '67=-45', '68=24', '69=4', '70=180'],
    'envelope_hpf_sc': ['58=1', '72=1', '73=400', '67=-50'],
    'transient': ['58=2', '67=-50', '71=10', '69=1.5', '70=220'],
    'lfo_free': ['58=3', '77=2', '79=1', '81=90'],
    'lfo_random_stereo': ['58=3', '77=3', '79=5', '85=60'],
    'lfo_sync_play': ['58=3', '78=9', '79=0', 'tempo=120', 'play=1', 'beat0=0.3'],
    'lfo_square_down': ['58=3', '79=4', '65=1', '80=70'],
    'lowpass_24_drive': ['64=1', '84=1', '83=12'],
    'highpass': ['64=2', '62=3', '82=8'],
    'two_bands_parallel': ['jsfx:50=1', 'clap:155=1', 'clap:156=1', 'clap:157=0', '89=1', '91=900'],
    'two_bands_series': ['jsfx:50=1', 'clap:155=1', 'clap:156=1', 'clap:157=0', '53=1', '89=3', '108=1.5'],
    'three_bands_mixed': ['jsfx:50=2', 'clap:155=1', 'clap:156=1', 'clap:157=1', '53=0', '54=1', '89=1', '120=3', '139=0.7'],
    'mix_70': ['51=70', '58=1'],
    'qtoe_dir': ['82=12', '65=1', '58=1'],
    'automation': ['t20000:58=1', 'jsfx:t50000:50=1', 'clap:t50000:155=1', 'clap:t50000:156=1', 'clap:t50000:157=0', 't80000:58=3'],
    'mix_signal': ['sig=tests/sig/mix.f64', '58=2', 'jsfx:50=1', 'clap:155=1', 'clap:156=1', 'clap:157=0', '89=1'],
}
