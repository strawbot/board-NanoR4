#!/usr/bin/env python3
"""Label every limb wire hole (J101..J601 pads) on the silkscreen, beside the hole, on both faces (run with KiCad's
python, KiCad closed). Replaces the old one-string-per-limb texts ('L1  W1 W2 + W3 W4') and the stale '+10V  GND'
text left from the hexagon generator. The label goes outboard of its hole (between it and the board edge) when it fits, else inboard.
usage: silk_limbs.py in.kicad_pcb out.kicad_pcb"""
import pcbnew, sys, math, re
mm = pcbnew.ToMM; F = pcbnew.FromMM; V = pcbnew.VECTOR2I
src, dst = sys.argv[1:3]
B = pcbnew.LoadBoard(src)
CX, CY = 150.0, 100.0
SIZE, THICK, CLR = 0.8, 0.12, 0.15
FPS = list(B.GetFootprints()); VIAS = [t for t in B.GetTracks() if t.Type() == pcbnew.PCB_VIA_T]
OLD = [d for d in B.GetDrawings() if d.GetClass() == 'PCB_TEXT' and (re.match(r'L\d  W1 W2', d.GetText()) or d.GetText().startswith('+10V'))]
pads = []
for f in FPS:
    for p in f.Pads():
        bb = p.GetBoundingBox(); pads.append((mm(bb.GetLeft()) - CLR, mm(bb.GetTop()) - CLR, mm(bb.GetRight()) + CLR, mm(bb.GetBottom()) + CLR))
for t in VIAS:
    if True:
        bb = t.GetBoundingBox(); pads.append((mm(bb.GetLeft()) - CLR, mm(bb.GetTop()) - CLR, mm(bb.GetRight()) + CLR, mm(bb.GetBottom()) + CLR))
placed = []
outline = pcbnew.SHAPE_POLY_SET(); B.GetBoardPolygonOutlines(outline, False)
inner = pcbnew.SHAPE_POLY_SET(outline); inner.Deflate(F(0.25), pcbnew.CORNER_STRATEGY_ROUND_ALL_CORNERS, F(0.01))
def hit(r, others):
    return any(not (r[2] < o[0] or r[0] > o[2] or r[3] < o[1] or r[1] > o[3]) for o in others)
n = 0
for ref in ('J101', 'J201', 'J301', 'J401', 'J501', 'J601'):
    f = next(x for x in FPS if x.GetReference() == ref)
    ps = sorted(f.Pads(), key=lambda q: int(q.GetNumber()))
    p0, p4 = (mm(ps[0].GetX()), mm(ps[0].GetY())), (mm(ps[4].GetX()), mm(ps[4].GetY()))
    mx, my = (p0[0] + p4[0]) / 2, (p0[1] + p4[1]) / 2
    ang = round(math.degrees(math.atan2(my - CY, mx - CX)) / 45) * 45       # outward direction of this limb
    ux, uy = math.cos(math.radians(ang)), math.sin(math.radians(ang))
    diag = ang % 90 != 0
    tdx, tdy = p4[0] - p0[0], p4[1] - p0[1]
    rot = (-45 if tdx * tdy > 0 else 45) if diag else 0            # text runs along the board edge on the 45-degree limbs
    ca, sa = math.cos(math.radians(rot)), -math.sin(math.radians(rot))  # screen y is down, KiCad angles run counter-clockwise
    for p in ps:
        name = p.GetNetname().split('/')[-1]
        txt = '+' if name == 'VIN' else name
        px, py = mm(p.GetX()), mm(p.GetY())
        w, h = len(txt) * SIZE * 0.85, SIZE * 1.1
        tl = math.hypot(tdx, tdy); tx, ty = tdx / tl, tdy / tl
        sign = -1 if p is ps[0] else (1 if p is ps[4] else 0)          # end holes may slide outward along the row
        for off, ts in [(o, t) for t in (0, 0.5, 1.0, 1.5, -0.5, -1.0) for o in (1.7, 1.9, 2.1, 2.3, 2.6)] + [(o, 0) for o in (-2.0, -2.4, -2.8, -3.2)]:
            x, y = px + ux * off + tx * sign * ts, py + uy * off + ty * sign * ts
            pts = [(x + (dx * ca - dy * sa), y + (dx * sa + dy * ca)) for dx in (-w / 2, w / 2) for dy in (-h / 2, h / 2)]
            r = (min(q[0] for q in pts), min(q[1] for q in pts), max(q[0] for q in pts), max(q[1] for q in pts))
            grid = [(x + (dx * ca - dy * sa), y + (dx * sa + dy * ca)) for dx in [-w / 2 + i * w / 8 for i in range(9)] for dy in (-h / 2, 0, h / 2)]
            ok = all(inner.Contains(V(F(q[0]), F(q[1]))) for q in pts) and not any(o[0] <= gx <= o[2] and o[1] <= gy <= o[3] for gx, gy in grid for o in pads)
            if ok: break
        else: print('no clear spot for', ref, txt)
        placed.append(r)
        for layer, mirror in ((pcbnew.F_SilkS, False), (pcbnew.B_SilkS, True)):
            t = pcbnew.PCB_TEXT(B); t.SetText(txt); t.SetLayer(layer); t.SetPosition(V(F(x), F(y)))
            t.SetTextSize(V(F(SIZE), F(SIZE))); t.SetTextThickness(F(THICK)); t.SetMirrored(mirror)
            t.SetTextAngle(pcbnew.EDA_ANGLE(rot, pcbnew.DEGREES_T))
            B.Add(t)
        n += 1
for d in OLD: B.Remove(d)
print('labelled', n, 'holes')
B.Save(dst)
