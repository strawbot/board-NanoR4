#!/usr/bin/env python3
"""Board side of the 21 V rating change (KiCad's python, KiCad closed): C1001 -> 100uF 35V (7343-43 case, same pads),
C1002/C1003 -> 10uF 50V X7R, D1001 -> SMAJ24A, new gate zener D1002 (SOD-123) next to Q1001.
usage: clamp_pcb.py in.kicad_pcb out.kicad_pcb"""
import pcbnew, sys, json, os
mm = pcbnew.ToMM; F = pcbnew.FromMM; V = pcbnew.VECTOR2I
src, dst = sys.argv[1:3]
LIB = '/Applications/KiCad/KiCad.app/Contents/SharedSupport/footprints'
reg = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'regen', 'uuid_registry.json')))
B = pcbnew.LoadBoard(src)
fps = {f.GetReference(): f for f in B.GetFootprints()}
def net(name): return B.FindNet(name)
fps['D1001'].SetValue('SMAJ24A'); fps['C1002'].SetValue('10uF 50V X7R'); fps['C1003'].SetValue('10uF 50V X7R')
# --- C1001: reload as the taller case (identical pads)
old = fps['C1001']; q = fps['Q1001']
new = pcbnew.FootprintLoad(LIB + '/Capacitor_Tantalum_SMD.pretty', 'CP_EIA-7343-43_Kemet-X')
new.SetFPID(pcbnew.LIB_ID('Capacitor_Tantalum_SMD', 'CP_EIA-7343-43_Kemet-X')); new.SetPosition(old.GetPosition()); new.SetOrientation(old.GetOrientation())
new.SetReference('C1001'); new.SetValue('100uF 35V polymer'); new.SetPath(old.GetPath()); new.SetAttributes(old.GetAttributes())
for a, b in ((old.Reference(), new.Reference()), (old.Value(), new.Value())):
    b.SetPosition(a.GetPosition()); b.SetAttributes(a.GetAttributes()); b.SetLayer(a.GetLayer()); b.SetVisible(a.IsVisible())
for p in old.Pads():
    t = new.FindPadByNumber(p.GetNumber())
    if t: t.SetNet(p.GetNet())
# --- D1002
d = pcbnew.FootprintLoad(LIB + '/Diode_SMD.pretty', 'D_SOD-123')
d.SetFPID(pcbnew.LIB_ID('Diode_SMD', 'D_SOD-123')); d.SetReference('D1002'); d.SetValue('BZT52C10 (10 V)')
d.SetPosition(q.GetPosition()); d.SetPath(pcbnew.KIID_PATH('/%s/%s' % (reg['sheets']['hex_ctrl:power'], reg['syms']['power:D1002'])))
d.Reference().SetLayer(q.Reference().GetLayer()); d.Reference().SetVisible(q.Reference().IsVisible()); d.Value().SetLayer(q.Value().GetLayer()); d.Value().SetVisible(q.Value().IsVisible())
for p in d.Pads(): p.SetNet(net('VIN') if p.GetNumber() == '1' else net('/power/PG'))
B.Remove(old); B.Add(new); B.Add(d)
B.Save(dst); print('saved')
