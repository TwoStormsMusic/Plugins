#!/usr/bin/env python3
"""Pull a TwoStorms JSFX's own sliders (50 and up) and its factory presets into C++ starting points.

    python3 tools/jsfx_extract.py <file.jsfx> sliders   -> ParamTable lines (ranges, defaults, enum labels)
    python3 tools/jsfx_extract.py <file.jsfx> presets   -> FactoryPreset table (menu path, "p=v ..." string)

The output is a starting point: formats, units and which parameters the host sees are decided by hand.
"""
import re
import sys


def sliders(src):
    out = []
    for m in re.finditer(r'^slider(\d+):([^<]*)<([^>]*)>(.*)$', src, re.M):
        n = int(m.group(1))
        if n < 43:
            continue
        default = m.group(2).strip()
        spec = m.group(3)
        name = m.group(4).strip().lstrip('-').strip()
        parts = spec.split(',', 3)
        lo, hi = parts[0].strip(), parts[1].strip()
        step = parts[2].strip() if len(parts) > 2 else ''
        labels = None
        if '{' in spec:
            labels = spec[spec.index('{') + 1:spec.rindex('}')].split(',')
            step = step.split('{')[0]
        cname = name.replace('"', '\\"')
        if labels:
            ls = ', '.join('"%s"' % l.strip().replace('"', '\\"') for l in labels)
            out.append(f'    t.addEnum({n}, "{cname}", {{{ls}}}, {default}, F, M);')
        else:
            isint = step.strip() in ('1',) and '.' not in lo + hi + default
            flags = 'F | PF_INT' if isint else 'F'
            out.append(f'    t.add({n}, "{cname}", {lo}, {hi}, {default}, {flags}, nullptr, M);   // step {step}')
    return '\n'.join(out)


def presets(src):
    menu = ''
    for m in re.finditer(r'(?:strcpy|strcat)\(#pr_fmenu,\s*"([^"]*)"\)', src):
        menu += m.group(1)
    items = [x for x in menu.split('|') if x != '']
    paths = []
    group = ''
    for it in items:
        close = False
        if it.startswith('>'):
            group = it[1:]
            continue
        if it.startswith('<'):
            it = it[1:]
            close = True
        paths.append((group + '/' + it) if group else it)
        if close:
            group = ''
    vals = {}
    for m in re.finditer(r'n == (\d+) \? pr_str\("([^"]*)"\)', src):
        vals[int(m.group(1))] = m.group(2)
    out = []
    for i, p in enumerate(paths):
        v = vals.get(i, '')
        out.append('        {"%s", "%s"},' % (p.replace('"', '\\"'), v))
    return '\n'.join(out)


if __name__ == '__main__':
    src = open(sys.argv[1], encoding='utf-8', errors='replace').read()
    print(sliders(src) if sys.argv[2] == 'sliders' else presets(src))
