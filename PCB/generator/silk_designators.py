#!/usr/bin/env python3
"""Move connector (J*) and test point (TP*) reference designators from F.Fab to F.SilkS (run with KiCad's python,
KiCad closed). Each goes to the nearest spot that clears pads, vias, other silkscreen and the board edge.
usage: silk_designators.py in.kicad_pcb out.kicad_pcb"""
import pcbnew, sys, math, re
mm = pcbnew.ToMM; F = pcbnew.FromMM; V = pcbnew.VECTOR2I
src, dst = sys.argv[1:3]
B = pcbnew.LoadBoard(src); FPS = list(B.GetFootprints())
SIZE, THICK = (1.0, 1.0), 0.15
# TP1202-TP1208 (Nano I2C/SWD holes) keep their designators on Fab: they already carry function labels
# (SCL, SDA, CLK, DIO, RST, GND, 3V3) beside each pad, and the area is too crowded for a second set of labels.
SKIP = {'TP1202', 'TP1203', 'TP1204', 'TP1205', 'TP1206', 'TP1207', 'TP1208'}
want = [f for f in FPS if re.fullmatch(r'(J\d+|TP\d+)', f.GetReference()) and not f.IsFlipped() and f.GetReference() not in SKIP]
CLR = 0.25          # clearance from text to exposed copper / other silk
# ---- obstacles (mm): rects (x0,y0,x1,y1) and segments (x0,y0,x1,y1,half-width)
rects = []; segs = []
def rect_of(bb, m): return (mm(bb.GetLeft()) - m, mm(bb.GetTop()) - m, mm(bb.GetRight()) + m, mm(bb.GetBottom()) + m)
for f in FPS:
    for p in f.Pads(): rects.append(rect_of(p.GetBoundingBox(), CLR))
    for g in f.GraphicalItems():
        if g.GetLayer() != pcbnew.F_SilkS: continue
        c = g.GetClass()
        if c == 'PCB_SHAPE' and g.GetShape() == pcbnew.SHAPE_T_SEGMENT:
            segs.append((mm(g.GetStart().x), mm(g.GetStart().y), mm(g.GetEnd().x), mm(g.GetEnd().y), mm(g.GetWidth()) / 2 + CLR))
        else: rects.append(rect_of(g.GetBoundingBox(), CLR))
    for t in (f.Reference(), f.Value()):
        if t.IsVisible() and t.GetLayer() == pcbnew.F_SilkS and f not in want: rects.append(rect_of(t.GetBoundingBox(), CLR))
for t in B.GetTracks():
    if t.Type() == pcbnew.PCB_VIA_T: rects.append(rect_of(t.GetBoundingBox(), CLR))
for d in B.GetDrawings():
    if d.GetLayer() == pcbnew.F_SilkS: rects.append(rect_of(d.GetBoundingBox(), CLR))
outline = pcbnew.SHAPE_POLY_SET(); B.GetBoardPolygonOutlines(outline, False)
inner = pcbnew.SHAPE_POLY_SET(outline); inner.Deflate(F(0.6), pcbnew.CORNER_STRATEGY_ROUND_ALL_CORNERS, F(0.01))
def seg_hits(r, s):
    x0, y0, x1, y1 = r; ax, ay, bx, by, w = s
    # distance from segment to rect <= w  (sample along the segment)
    n = max(2, int(math.hypot(bx - ax, by - ay) / 0.2))
    for i in range(n + 1):
        px = ax + (bx - ax) * i / n; py = ay + (by - ay) * i / n
        dx = max(x0 - px, 0, px - x1); dy = max(y0 - py, 0, py - y1)
        if math.hypot(dx, dy) <= w: return True
    return False
def free(r):
    for o in rects:
        if r[0] < o[2] and r[2] > o[0] and r[1] < o[3] and r[3] > o[1]: return False
    for s in segs:
        if seg_hits(r, s): return False
    # inside the board edge margin
    box = pcbnew.SHAPE_POLY_SET(); box.NewOutline()
    for x, y in ((r[0], r[1]), (r[2], r[1]), (r[2], r[3]), (r[0], r[3])): box.Append(F(x), F(y))
    box.BooleanSubtract(inner)
    return box.OutlineCount() == 0
placed = []; failed = []
for f in sorted(want, key=lambda f: f.GetReference()):
    t = f.Reference(); old_layer = t.GetLayerName()
    t.SetLayer(pcbnew.F_SilkS); t.SetVisible(True); t.SetTextSize(V(F(SIZE[0]), F(SIZE[1]))); t.SetTextThickness(F(THICK))
    c = f.GetBoundingBox(False); cx, cy = mm(c.GetCenter().x), mm(c.GetCenter().y)
    best = None
    for r10 in range(0, 241):
        r = r10 * 0.1
        n = 1 if r10 == 0 else max(12, int(2 * math.pi * r / 0.8))
        for k in range(n):
            th = 2 * math.pi * k / n
            for ang in (0, 90):
                t.SetTextAngleDegrees(ang); t.SetPosition(V(F(cx + r * math.cos(th)), F(cy + r * math.sin(th))))
                bb = t.GetBoundingBox(); rr = (mm(bb.GetLeft()), mm(bb.GetTop()), mm(bb.GetRight()), mm(bb.GetBottom()))
                if free(rr): best = (r, rr); break
            if best: break
        if best: break
    if best:
        placed.append((f.GetReference(), round(best[0], 1))); rects.append((best[1][0] - CLR, best[1][1] - CLR, best[1][2] + CLR, best[1][3] + CLR))
    else:
        failed.append(f.GetReference()); t.SetLayer(pcbnew.F_Fab)
print('placed', len(placed), 'failed', failed); print(placed)
B.Save(dst)
