"""libs2.py : symbol definitions taken from the project's current (known-good) schematics, not from the stock library.
get(key) -> (embedded symbol text, {pin number: dict(x, y, a, name, type)})"""
import os, re, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from nano_iface_sch import spans
BASE = os.path.join(HERE, 'regen', 'baseline')
_texts = {}
def _load():
    if _texts: return
    for fn in ('hex_ctrl', 'power', 'logic', 'imu', 'group', 'limb'):
        s = open(os.path.join(BASE, fn + '.kicad_sch')).read()
        i = s.index('(lib_symbols')
        for a, b in spans(s):
            if a >= i and s[a:b].startswith('(lib_symbols'):
                sub = s[a:b]
                for x, y in spans(sub):
                    t = sub[x:y]
                    m = re.match(r'\(symbol "([^"]+)"', t)
                    if m and m.group(1) not in _texts: _texts[m.group(1)] = t
                break
def _pins(t):
    out = {}
    for m in re.finditer(r'\(pin\s+(\w+)\s+\w+\s*\(at\s+([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)\)(.*?)\(number\s+"([^"]*)"', t, re.S):
        body = m.group(5); nm = re.search(r'\(name\s+"([^"]*)"', body)
        out[m.group(6)] = dict(type=m.group(1), x=float(m.group(2)), y=float(m.group(3)), a=float(m.group(4)), name=nm.group(1) if nm else '')
    return out
def get(key):
    _load()
    if key == 'hex_ctrl:VIN' and key not in _texts:        # the supply rail after reverse protection (was +10V); lives in hex_ctrl.kicad_sym
        _texts[key] = _texts['power:+10V'].replace('power:+10V', 'hex_ctrl:VIN').replace('+10V', 'VIN')
    t = _texts[key]
    return t, _pins(t)
def keys():
    _load(); return sorted(_texts)
if __name__ == '__main__':
    for k in keys():
        t, p = get(k)
        print(f'{k:<36}', {n: (v['x'], v['y'], int(v['a'])) for n, v in p.items()} if len(p) <= 6 else f'{len(p)} pins')
