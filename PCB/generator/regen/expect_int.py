"""expected netlist after hooking up the IMU interrupt: Q1201 level shifter, R1204 (3V3) / R1205 (5V) pull-ups, A1.5 on IMU_INT_5V"""
import json, sys
sys.path.insert(0, '..')
from netsig import sig, diff
cur = json.load(open('current_sig.json'))
exp = {'nets': {k: list(v) for k, v in cur['nets'].items()}, 'parts': {k: list(v) for k, v in cur['parts'].items()}}
exp['parts']['Q1201'] = ['2N7002', 'Package_TO_SOT_SMD:SOT-23', False]
exp['parts']['R1204'] = ['10k', 'Resistor_SMD:R_0402_1005Metric', False]
exp['parts']['R1205'] = ['10k', 'Resistor_SMD:R_0402_1005Metric', False]
exp['nets'].pop('unconnected-(A1-D2-Pad5)', None)
exp['nets']['IMU_INT_5V'] = ['A1.5', 'Q1201.3', 'R1205.2']
exp['nets']['/imu/IMU_INT'] = sorted(exp['nets']['/imu/IMU_INT'] + ['Q1201.2', 'R1204.2'])
exp['nets']['+3V3'] = sorted(exp['nets']['+3V3'] + ['Q1201.1', 'R1204.1'])
exp['nets']['+5V'] = sorted(exp['nets']['+5V'] + ['R1205.1'])
json.dump(exp, open('expected_int_sig.json', 'w'), indent=1)
new = sig(sys.argv[1]); m = diff(exp, new)
print(len(new['nets']), 'nets,', len(new['parts']), 'parts;', len(m), 'differences vs expected')
for x in m[:20]: print(' ', x)
