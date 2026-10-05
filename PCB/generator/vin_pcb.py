#!/usr/bin/env python3
"""Board side of the Vin change (run with KiCad's python, KiCad closed): keep-alive circuit removed, OE tied to GND,
+10V net renamed VIN, R1003 value 18k.   usage: vin_pcb.py in.kicad_pcb out.kicad_pcb"""
import pcbnew, sys
src, dst = sys.argv[1:3]
B = pcbnew.LoadBoard(src)
fps = {f.GetReference(): f for f in B.GetFootprints()}
REMOVE = ['C1104', 'C1105', 'D1101', 'D1102', 'R1101', 'R1102', 'Q1101', 'JP1101']
DEAD_NETS = {'KA', '/logic/KX', '/logic/KY', '/logic/OE_N'}
def net(name):
    n = B.FindNet(name)
    if n is None:
        n = pcbnew.NETINFO_ITEM(B, name); B.Add(n); n = B.FindNet(name)
    return n
gnd = net('GND')
for r in ('U1101', 'U1102', 'U1103'):
    for p in fps[r].Pads():
        if p.GetNumber() == '13': p.SetNet(gnd)                      # OE tied low
for p in fps['A1'].Pads():
    if p.GetNumber() == '9': p.SetNet(net('unconnected-(A1-D6-Pad9)'))   # D6 (KA) no longer used
fps['R1003'].SetValue('18k')
B.FindNet('+10V').SetNetname('VIN')
dead = [t for t in B.GetTracks() if t.GetNetname() in DEAD_NETS]
print('removing', len(dead), 'track/via items on', sorted(DEAD_NETS), 'and', len(REMOVE), 'footprints')
victims = [fps[r] for r in REMOVE]
for t in dead: B.Remove(t)
for f in victims: B.Remove(f)
B.Save(dst)
