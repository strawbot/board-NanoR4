import pcbnew, re
t=open('/tmp/hex.net').read()
b=pcbnew.BOARD()
b.GetDesignSettings().SetBoardThickness(pcbnew.FromMM(0.8))
# nets
nets={}
for m in re.finditer(r'\(net \(code "\d+"\) \(name "([^"]*)"\)(.*?)\)\s*(?=\(net |\)\s*\)\s*$)',t,re.S):
    name=m.group(1)
    ni=pcbnew.NETINFO_ITEM(b,name); b.Add(ni)
    for ref,pin in re.findall(r'\(node \(ref "([^"]+)"\) \(pin "([^"]+)"\)',m.group(2)):
        nets[(ref,pin)]=ni
# footprints
for m in re.finditer(r'\(comp \(ref "([^"]+)"\)\s*\(value "([^"]*)"\)\s*\(footprint "([^"]+)"\).*?\(sheetpath \(names "[^"]*"\) \(tstamps "([^"]*)"\)\)\s*\(tstamps "([^"]+)"\)\)',t,re.S):
    ref,val,fpn,sp,sym=m.groups()
    lib,name=fpn.split(':')
    path='/home/claude/kc/hex_ctrl/hex_ctrl.pretty' if lib=='hex_ctrl' else f'/usr/share/kicad/footprints/{lib}.pretty'
    fp=pcbnew.FootprintLoad(path,name)
    fp.SetFPID(pcbnew.LIB_ID(lib,name))
    fp.SetReference(ref); fp.SetValue(val)
    fp.SetPath(pcbnew.KIID_PATH(sp+sym))
    if 'DNP' in val.upper(): 
        try: fp.SetDNP(True)
        except Exception: pass
    b.Add(fp)
    for p in fp.Pads():
        ni=nets.get((ref,p.GetNumber()))
        if ni: p.SetNet(ni)
_HEX_TEST_BOARD=b
exec(open('/home/claude/kc/hex_ctrl/hex_place.py').read())
out='/home/claude/kc/hex_ctrl/hex_ctrl.kicad_pcb'
pcbnew.SaveBoard(out,b)
print("saved", len(list(b.GetFootprints())), "footprints,", b.GetNetCount(), "nets")

# ---- reference designators to the fabrication layers (keeps silk clear on a dense board)
for fp in b.GetFootprints():
    ref = fp.Reference()
    ref.SetLayer(pcbnew.B_Fab if fp.IsFlipped() else pcbnew.F_Fab)
    ref.SetTextSize(pcbnew.VECTOR2I(pcbnew.FromMM(0.5), pcbnew.FromMM(0.5)))
    ref.SetTextThickness(pcbnew.FromMM(0.08))

# ---- a few useful silk labels on the bottom (outside the Nano)
import math
def silk(txt, x, y, ang):
    t = pcbnew.PCB_TEXT(b)
    t.SetText(txt); t.SetLayer(pcbnew.B_SilkS)
    t.SetTextSize(pcbnew.VECTOR2I(pcbnew.FromMM(1.0), pcbnew.FromMM(1.0)))
    t.SetTextThickness(pcbnew.FromMM(0.15))
    t.SetPosition(pcbnew.VECTOR2I(pcbnew.FromMM(CX + x), pcbnew.FromMM(CY - y)))
    try: t.SetTextAngle(pcbnew.EDA_ANGLE(ang, pcbnew.DEGREES_T))
    except Exception: pass
    try: t.SetMirrored(True)
    except Exception: pass
    b.Add(t)
for L in range(1, 7):
    phi = 60 * (L - 1)
    r = A / 2 - 8.5
    silk(f"L{L}  W1 W2 + W3 W4", r * math.cos(math.radians(phi)), r * math.sin(math.radians(phi)), (phi + 90) % 180 if phi not in (0, 180) else 90)
pr = R - 8.3
silk("+10V  GND", pr * math.cos(math.radians(210)), pr * math.sin(math.radians(210)) - 2.2, 300)
pcbnew.SaveBoard(out, b)
print("labels done")
