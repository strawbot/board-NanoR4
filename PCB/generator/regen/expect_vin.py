"""expected netlist after: keep-alive removed, OE tied to GND, +10V -> VIN, R1003 47k -> 18k"""
import json, sys
sys.path.insert(0, '..')
from netsig import sig, diff
base = json.load(open('baseline_sig.json'))
removed = {'C1104', 'C1105', 'D1101', 'D1102', 'R1101', 'R1102', 'Q1101', 'JP1101'}
exp = {'nets': {}, 'parts': {}}
for r, v in base['parts'].items():
    if r in removed: continue
    v = list(v)
    if r == 'R1003': v[0] = '18k'
    exp['parts'][r] = v
oe_pins = {'U1101.13', 'U1102.13', 'U1103.13'}
for n, pins in base['nets'].items():
    pins = [p for p in pins if p.split('.')[0] not in removed]
    if n == '/logic/OE_N': pins = [p for p in pins if p in oe_pins]; n = 'GND'; 
    if n == 'GND': pins = sorted(set(pins) | oe_pins)
    if n == '+10V': n = 'VIN'
    if n == 'KA': pins = ['A1.9']; n = 'unconnected-(A1-D6-Pad9)'
    if not pins: continue
    if n in exp['nets']: exp['nets'][n] = sorted(set(exp['nets'][n]) | set(pins))
    else: exp['nets'][n] = sorted(pins)
json.dump(exp, open('expected_vin_sig.json', 'w'), indent=1)
new = sig(sys.argv[1] if len(sys.argv) > 1 else 'out')
m = diff(exp, new)
print(len(new['nets']), 'nets,', len(new['parts']), 'parts;', len(m), 'differences vs expected')
for x in m[:30]: print(' ', x)
