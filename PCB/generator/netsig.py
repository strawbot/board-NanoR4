#!/usr/bin/env python3
"""netsig.py DIR [out.json] : export DIR/hex_ctrl.kicad_sch's netlist with kicad-cli and print/save a signature
   {"nets": {name: [REF.pin,...]}, "parts": {ref: [value, footprint, dnp]}}  (used to prove a layout change is electrically identical)"""
import subprocess, sys, json, os, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
K = '/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli'
def parse(s):
    i = 0; n = len(s); st = [[]]
    while i < n:
        c = s[i]
        if c == '(': st.append([]); i += 1
        elif c == ')': t = st.pop(); st[-1].append(t); i += 1
        elif c in ' \t\r\n': i += 1
        elif c == '"':
            j = i + 1; b = []
            while s[j] != '"':
                if s[j] == '\\': j += 1
                b.append(s[j]); j += 1
            st[-1].append(''.join(b)); i = j + 1
        else:
            j = i
            while j < n and s[j] not in ' \t\r\n()': j += 1
            st[-1].append(s[i:j]); i = j
    return st[0][0]
def sig(d):
    out = os.path.join('/tmp', 'netsig_%d.net' % os.getpid())
    subprocess.run([K, 'sch', 'export', 'netlist', '--format', 'kicadsexpr', '-o', out, os.path.join(d, 'hex_ctrl.kicad_sch')], capture_output=True, check=True)
    t = parse(open(out).read()); os.remove(out)
    nets = {}; parts = {}
    for sec in t:
        if not isinstance(sec, list) or not sec: continue
        if sec[0] == 'nets':
            for n in sec[1:]:
                name = [q for q in n if isinstance(q, list) and q[0] == 'name'][0][1]
                pins = sorted('%s.%s' % (q[1][1], q[2][1]) for q in n if isinstance(q, list) and q[0] == 'node')
                nets[name] = pins
        if sec[0] == 'components':
            for c in sec[1:]:
                d_ = {q[0]: q for q in c if isinstance(q, list)}
                ref = d_['ref'][1]; val = d_.get('value', ['', ''])[1]; fp = d_.get('footprint', ['', ''])[1]
                props = {q[1][1]: q[2][1] for q in c if isinstance(q, list) and q[0] == 'property' and len(q) > 2 and isinstance(q[1], list)}
                parts[ref] = [val, fp, 'dnp' in ' '.join(str(x) for x in c)]
    return {'nets': nets, 'parts': parts}
def diff(a, b):
    na, nb = a['nets'], b['nets']; msgs = []
    for k in sorted(set(na) | set(nb)):
        if k not in nb: msgs.append('net missing: %s %s' % (k, na[k][:6]))
        elif k not in na: msgs.append('net new: %s %s' % (k, nb[k][:6]))
        elif na[k] != nb[k]: msgs.append('net differs: %s old=%s new=%s' % (k, sorted(set(na[k]) - set(nb[k]))[:5], sorted(set(nb[k]) - set(na[k]))[:5]))
    pa, pb = a['parts'], b['parts']
    for r in sorted(set(pa) | set(pb)):
        if r not in pb: msgs.append('part missing: ' + r)
        elif r not in pa: msgs.append('part new: ' + r)
        elif pa[r][:2] != pb[r][:2]: msgs.append('part differs: %s %s -> %s' % (r, pa[r][:2], pb[r][:2]))
    return msgs
if __name__ == '__main__':
    s = sig(sys.argv[1])
    if len(sys.argv) > 2: json.dump(s, open(sys.argv[2], 'w'), indent=1)
    print(len(s['nets']), 'nets,', len(s['parts']), 'parts')
