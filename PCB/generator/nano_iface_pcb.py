#!/usr/bin/env python3
"""Board side of the Nano interface change (run with KiCad's python, KiCad closed).
Positions of the Nano features come from Arduino's ABX00142 Altium CAD (older than the Nano in hand;
verify by measurement). Nano coords (nx, ny) are mapped to the board from A1's castellated pads:
    pcb_x = A1.x + (54.29 - ny)        pcb_y = pad1.y + (nx - 78.755)
"""
import pcbnew, json, sys
mm = pcbnew.ToMM; F = pcbnew.FromMM; V = pcbnew.VECTOR2I
LIB = '/Applications/KiCad/KiCad.app/Contents/SharedSupport/footprints'
src, dst = sys.argv[1:3]
uu = json.load(open('generator/nano_iface_uuids.json'))
SH_IMU = 'c97bdb55-5522-40d8-85df-2152a7e2a132'
B = pcbnew.LoadBoard(src)
fps = {f.GetReference(): f for f in B.GetFootprints()}
a1 = fps['A1']; a1x = mm(a1.GetPosition().x)
pad1y = [mm(p.GetPosition().y) for p in a1.Pads() if p.GetNumber() == '1'][0]
def nano(nx, ny): return (a1x + (54.29 - ny), pad1y + (nx - 78.755))
def net(name):
    n = B.FindNet(name)
    if n is None:
        n = pcbnew.NETINFO_ITEM(B, name); B.Add(n); n = B.FindNet(name)
    return n
# --- capture everything we need before modifying the board (SWIG wrappers go stale after Remove)
j1 = fps['J1001']
j1_pos, j1_rot, j1_path, j1_val = j1.GetPosition(), j1.GetOrientationDegrees(), j1.GetPath(), 'PWR_IN_UART'
j1_ref = (j1.Reference().GetPosition(), j1.Reference().GetLayer(), j1.Reference().IsVisible(), j1.Reference().GetTextAngleDegrees())
tp1 = fps['TP1201']; tp_reflayer, tp_refvis = tp1.Reference().GetLayer(), tp1.Reference().IsVisible()
tp_ref_ang = tp1.Reference().GetTextAngleDegrees(); tp_val_vis = tp1.Value().IsVisible(); tp_val_layer = tp1.Value().GetLayer()
# A1 pad nets
for p in a1.Pads():
    if p.GetNumber() == '1': p.SetNet(net('NANO_TX'))
    if p.GetNumber() == '2': p.SetNet(net('NANO_RX'))
    if p.GetNumber() == '17': p.SetNet(net('+3V3'))
# J1201 out
B.Remove(fps['J1201'])
# J1001 -> 1x04
B.Remove(j1)
new = pcbnew.FootprintLoad(LIB + '/Connector_PinHeader_2.54mm.pretty', 'PinHeader_1x04_P2.54mm_Vertical')
new.SetFPID(pcbnew.LIB_ID('Connector_PinHeader_2.54mm', 'PinHeader_1x04_P2.54mm_Vertical'))
new.SetReference('J1001'); new.SetValue(j1_val); new.SetPosition(j1_pos); new.SetOrientationDegrees(j1_rot); new.SetPath(j1_path)
new.Reference().SetLayer(j1_ref[1]); new.Reference().SetVisible(j1_ref[2])
for p in new.Pads():
    p.SetNet({'1': net('/power/VIN_RAW'), '2': net('GND'), '3': net('NANO_TX'), '4': net('NANO_RX')}[p.GetNumber()])
B.Add(new)
# test-point style holes
FP = 'Nano_Hole_Pad'
spec = [('TP1202', 'NANO_SCL_HOLE', nano(81.295, 59.370), '/imu/SCL'),
        ('TP1203', 'NANO_SDA_HOLE', nano(81.295, 49.210), '/imu/SDA'),
        ('TP1204', 'DBG_SWDIO',     nano(81.203, 51.741), None),
        ('TP1205', 'DBG_SWCLK',     nano(78.663, 54.281), None),
        ('TP1206', 'DBG_NRST',      nano(81.203, 56.821), None),
        ('TP1207', 'DBG_GND',       nano(78.663, 56.821), 'GND'),
        ('TP1208', 'DBG_3V3',       nano(78.663, 51.741), '+3V3')]
for ref, val, (x, y), nn in spec:
    f = pcbnew.FootprintLoad('hex_ctrl.pretty', FP)
    f.SetFPID(pcbnew.LIB_ID('hex_ctrl', FP)); f.SetReference(ref); f.SetValue(val)
    f.SetPosition(V(F(x), F(y))); f.SetPath(pcbnew.KIID_PATH('/%s/%s' % (SH_IMU, uu[ref])))
    f.Reference().SetLayer(tp_reflayer); f.Reference().SetVisible(tp_refvis); f.Value().SetLayer(tp_val_layer); f.Value().SetVisible(tp_val_vis)
    for p in f.Pads():
        p.SetNet(net(nn if nn else 'unconnected-(%s-Pad1)' % ref))
    B.Add(f)
# silkscreen labels
def silk(txt, x, y, size=0.8, ang=0):
    t = pcbnew.PCB_TEXT(B); t.SetText(txt); t.SetPosition(V(F(x), F(y))); t.SetLayer(pcbnew.F_SilkS)
    t.SetTextSize(V(F(size), F(size))); t.SetTextThickness(F(0.15)); t.SetTextAngleDegrees(ang); B.Add(t)
posn = {r: p for r, _, p, _ in spec}
rowA = posn['TP1207'][1]; rowB = posn['TP1204'][1]
for lab, ref, up in (('3V3', 'TP1208', True), ('CLK', 'TP1205', True), ('GND', 'TP1207', True), ('DIO', 'TP1204', False), ('RST', 'TP1206', False)):
    x, y = posn[ref]; silk(lab, x, y - 1.9 if up else y + 1.9)
silk('SCL', posn['TP1202'][0], posn['TP1202'][1] + 1.9); silk('SDA', posn['TP1203'][0], posn['TP1203'][1] + 1.9)
B.Save(dst); print('saved', dst)
