#!/usr/bin/env python3
"""Board side of the IMU interrupt (KiCad's python, KiCad closed): level shifter Q1201 + R1204 (3V3 pull-up) + R1205 (5V pull-up),
A1 pad 5 (D2) on the new net IMU_INT_5V.  usage: int_pcb.py in.kicad_pcb out.kicad_pcb"""
import pcbnew, sys, json, os
mm = pcbnew.ToMM; F = pcbnew.FromMM; V = pcbnew.VECTOR2I
src, dst = sys.argv[1:3]
LIB = '/Applications/KiCad/KiCad.app/Contents/SharedSupport/footprints'
reg = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'regen', 'uuid_registry.json')))
B = pcbnew.LoadBoard(src)
fps = {f.GetReference(): f for f in B.GetFootprints()}
tp = fps['TP1201']; seed = tp.GetPosition()
def net(name):
    n = B.FindNet(name)
    if n is None:
        n = pcbnew.NETINFO_ITEM(B, name); B.Add(n); n = B.FindNet(name)
    return n
n5 = net('IMU_INT_5V')
for p in fps['A1'].Pads():
    if p.GetNumber() == '5': p.SetNet(n5)
path = lambda ref: pcbnew.KIID_PATH('/%s/%s' % (reg['sheets']['hex_ctrl:imu'], reg['syms']['imu:' + ref]))
def add(ref, val, lib, name, nets):
    f = pcbnew.FootprintLoad('%s/%s.pretty' % (LIB, lib), name)
    f.SetFPID(pcbnew.LIB_ID(lib, name)); f.SetReference(ref); f.SetValue(val); f.SetPosition(seed); f.SetPath(path(ref))
    f.Reference().SetLayer(tp.Reference().GetLayer()); f.Reference().SetVisible(tp.Reference().IsVisible())
    f.Value().SetLayer(tp.Value().GetLayer()); f.Value().SetVisible(tp.Value().IsVisible())
    for p in f.Pads(): p.SetNet(net(nets[p.GetNumber()]))
    B.Add(f)
add('Q1201', '2N7002', 'Package_TO_SOT_SMD', 'SOT-23', {'1': '+3V3', '2': '/imu/IMU_INT', '3': 'IMU_INT_5V'})
add('R1204', '10k', 'Resistor_SMD', 'R_0402_1005Metric', {'1': '+3V3', '2': '/imu/IMU_INT'})
add('R1205', '10k', 'Resistor_SMD', 'R_0402_1005Metric', {'1': '+5V', '2': 'IMU_INT_5V'})
B.Save(dst); print('saved')
