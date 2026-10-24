JSFX = 'tests/reference/compressor_ref.jsfx'
PLUGIN = 'com.twostorms.compressor'
NC = 2
CASES = {
    'default': [],
    'opto': ['101=2'],
    'sidechain_trim': ['65=1', '98=-6', '95=120', '96=2'],
    'gain_auto': ['10=-6', 't40000:10=3'],
}
