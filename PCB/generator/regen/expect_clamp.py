"""expected netlist after the 21 V rating change: SMAJ24A, C1001 35 V (7343-43), C1002/C1003 10uF 50V X7R, gate zener D1002"""
import json, sys
sys.path.insert(0, '..')
from netsig import sig, diff
cur = json.load(open('current_sig.json'))
exp = {'nets': {k: list(v) for k, v in cur['nets'].items()}, 'parts': {k: list(v) for k, v in cur['parts'].items()}}
exp['parts']['D1001'][0] = 'SMAJ24A'
exp['parts']['C1001'][0] = '100uF 35V polymer'; exp['parts']['C1001'][1] = 'Capacitor_Tantalum_SMD:CP_EIA-7343-43_Kemet-X'
exp['parts']['C1002'][0] = exp['parts']['C1003'][0] = '10uF 50V X7R'
exp['parts']['D1002'] = ['BZT52C10 (10 V)', 'Diode_SMD:D_SOD-123', False]
exp['nets']['VIN'] = sorted(exp['nets']['VIN'] + ['D1002.1']); exp['nets']['/power/PG'] = sorted(exp['nets']['/power/PG'] + ['D1002.2'])
json.dump(exp, open('expected_clamp_sig.json', 'w'), indent=1)
new = sig(sys.argv[1])
m = diff(exp, new); print(len(new['nets']), 'nets,', len(new['parts']), 'parts;', len(m), 'differences vs expected')
for x in m[:20]: print(' ', x)
