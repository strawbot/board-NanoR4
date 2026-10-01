# hex_place.py - draws the hexagonal outline and places every part in its zone.
# Run in the PCB editor: Tools > Scripting Console, then
#     exec(open(r"<path to this file>").read())
# after Tools > Update PCB from Schematic (F8). Safe to re-run: the outline is
# redrawn and every part is moved again. It gives a starting placement only;
# expect to nudge parts and fix courtyard overlaps before routing.
import math, pcbnew

A   = 60.0                  # across flats, mm
CUT = 6.0                   # truncation at the USB vertex (90 deg), mm
CX, CY = 150.0, 100.0       # board centre on the sheet, mm
R   = A / math.sqrt(3)      # vertex radius

try:
    b = BOARD                # set by an external test harness
except NameError:
    b = pcbnew.GetBoard()

def P(x, y):                 # maths coords (mm, y up) -> KiCad internal units
    return pcbnew.VECTOR2I(pcbnew.FromMM(CX + x), pcbnew.FromMM(CY - y))

def pol(r, deg):
    return (r * math.cos(math.radians(deg)), r * math.sin(math.radians(deg)))

missing = []
def fp_of(ref):
    fp = b.FindFootprintByReference(ref)
    if fp is None:
        missing.append(ref)
    return fp

def put(ref, xy, rot=0.0):
    fp = fp_of(ref)
    if fp is None:
        return
    fp.SetOrientationDegrees(rot % 360)
    fp.SetPosition(P(*xy))

def frame(origin, ang):
    """Local frame: x along the edge (tangent), y outward (radial)."""
    ux, uy = math.sin(math.radians(ang)), -math.cos(math.radians(ang))
    ox, oy = math.cos(math.radians(ang)),  math.sin(math.radians(ang))
    def to(xl, yl):
        return (origin[0] + xl * ux + yl * ox, origin[1] + xl * uy + yl * oy)
    return to

# ---------------------------------------------------------------- outline
for d in list(b.GetDrawings()):
    if d.GetLayer() == pcbnew.Edge_Cuts:
        b.Remove(d)
V = [pol(R, 30 + 60 * k) for k in range(6)]
def along(a, c, dist):
    L = math.dist(a, c)
    return (a[0] + (c[0] - a[0]) * dist / L, a[1] + (c[1] - a[1]) * dist / L)
pts = []
for k in range(6):
    if k == 1:
        pts += [along(V[1], V[0], CUT), along(V[1], V[2], CUT)]
    else:
        pts.append(V[k])
for i in range(len(pts)):
    s = pcbnew.PCB_SHAPE(b)
    s.SetShape(pcbnew.SHAPE_T_SEGMENT)
    s.SetStart(P(*pts[i])); s.SetEnd(P(*pts[(i + 1) % len(pts)]))
    s.SetLayer(pcbnew.Edge_Cuts); s.SetWidth(pcbnew.FromMM(0.1))
    b.Add(s)

# ---------------------------------------------------------------- limbs
# Limb L (1..6) sits on the edge at 60*(L-1) deg. Parts are numbered L*100+n.
# Local frame: x along the edge, y outward; the edge is at y = 0.
PITCH = 3.6
for L in range(1, 7):
    phi = 60 * (L - 1)
    to = frame(pol(A / 2, phi), phi)
    base = L * 100
    fp = fp_of(f"J{base + 1}")            # 1x05 header footprint, origin at pin 1
    if fp:
        fp.SetOrientationDegrees(phi % 360)
        fp.SetPosition(P(*to(-2 * 2.54, -3.0)))
    for i in range(4):
        xl = (i - 1.5) * PITCH
        put(f"D{base + 1 + i}",  to(xl, -6.8),  phi)          # flyback, long axis radial
        put(f"Q{base + 1 + i}",  to(xl, -10.5), phi)          # AO3400A
        put(f"R{base + 1 + i}",  to(xl - 0.75, -14.0), phi)   # RG
        put(f"R{base + 11 + i}", to(xl + 0.75, -14.0), phi)   # RPD

def at(r, ang, rot, ref):                   # polar placement, rot relative to radial
    put(ref, pol(r, ang), ang + rot)

# ---------------------------------------------------------------- groups
# Group A/B/C at the vertices 30/150/270 deg, between their two limbs.
for gi, v in enumerate([30, 150, 270]):
    g = 700 + 100 * gi
    at(24.5, v, 90, f"R{g + 1}")                            # RSENSE 2512, tangential
    to = frame(pol(20.5, v), v)
    put(f"TP{g + 1}", to(-2.4, 0), v)
    put(f"R{g + 2}",  to(0.0, 0), v)                        # ISNS series R
    put(f"C{g + 1}",  to(1.6, 0), v)                        # ISNS filter C (DNP)
    at(10.8, v, 90, f"U{1101 + gi}")                        # 74HC595, inner ring
    at(15.0, v, 0, f"C{1101 + gi}")                         # its decoupling

# ---------------------------------------------------------------- 2-layer routing tweaks (Sep 2026)
# U1101/U1102 moved off the Nano's pad rows (their via space was blocked), turned so
# outputs QB-QH face the gate resistors; decoupling caps moved to match.
put("U1101", (12.0, 2.7), 270)
put("U1102", (-12.0, 2.7), 270)
put("C1101", (14.0, -1.9), 0)
put("C1102", (-8.3, 3.0), 0)
# courtyard clean-up nudges (power corner and keep-alive)
put("C1001", (-10.8033, -6.1773), -60)
put("C1003", (-5.0844, -5.9044), -40)
put("R1002", (-6.445, -9.5239), -124)
put("C1004", (-4.6654, -10.5109), -114)
put("C1104", (19.6865, -7.9519), -30)
# Sep 2026 round 2: limb 2/3 holes slid clear of the Nano; U1103 moved for the SWD/Qwiic holes;
# UART pads J1 and SWD header J2 over the Nano (top side).
put("J201", (11.5255, 24.5227), 60)
put("D201", (7.7564, 21.1399), 60)
put("J301", (-20.3243, 19.4427), 120)
put("D304", (-7.6855, 21.106), 120)
put("U1103", (0.25, -8.0), 90)
put("C1103", (-1.5, -3.0), 90)
put("R502", (-6.2743, -14.8678), -120)
put("R511", (-3.4975, -16.3359), -120)
put("R604", (3.4322, -14.5171), -60)
put("R613", (6.2543, -14.834), -60)
put("R614", (2.3654, -17.8299), -60)
put("J1", (5.0, -8.0), 0)
put("J2", (4.5, 5.0), 0)

# ---------------------------------------------------------------- power (210 deg vertex)
to = frame(pol(26.3, 210), 210)
fp = fp_of("J1001")                # 1x02 holes, origin at pin 1, laid along the edge
if fp:
    fp.SetOrientationDegrees(210 % 360)
    fp.SetPosition(P(*to(-1.27, 0)))
at(22.5, 210, 90, "D1001")          # TVS
at(16.8, 210, 0,  "Q1001")          # reverse-protection P-FET
at(12.2, 210, 90, "C1001")          # 100 uF polymer
at(7.8, 190, 90, "C1002")
at(7.8, 230, 90, "C1003")
to = frame(pol(19.7, 210), 210)
put("R1001", to(2.0, 0), 210)
put("D1002", to(-0.9, 0), 210 + 90)
at(11.0, 236, 0, "R1002")           # VIN_S divider
at(11.0, 241, 0, "R1003")
at(11.0, 246, 0, "C1004")

# ---------------------------------------------------------------- keep-alive / logic misc (330 deg vertex)
at(17.3, 330, 0, "Q1101")
at(24.5, 330, 90, "JP1101")
to = frame(pol(21.0, 330), 330)
for ref, xl in [("C1104", -3.0), ("D1101", -1.0), ("D1102", 1.2), ("C1105", 2.9)]:
    put(ref, to(xl, 0), 330)
for ref, ang in [("R1102", 318), ("R1101", 326), ("R1103", 334), ("R1104", 342)]:
    at(13.0, ang, 0, ref)

# ---------------------------------------------------------------- IMU at the centre
put("U1201", (0, 0), 0)
for ref, xy in [("C1201", (-3.0, -3.2)), ("C1202", (0.0, -3.2)), ("C1203", (3.0, -3.2)),
                ("R1201", (-4.2, 0.0)), ("R1202", (4.2, 1.0)), ("R1203", (4.2, -1.0)),
                ("TP1201", (0.0, 3.5))]:
    put(ref, xy, 0)

# ---------------------------------------------------------------- mounting holes
for n, k in enumerate([0, 2, 3, 4, 5], start=1):
    put(f"H{n}", pol(R - 3.6, 30 + 60 * k))

# ---------------------------------------------------------------- Nano on the bottom
fp = fp_of("A1")
if fp:
    if not fp.IsFlipped():
        try:
            fp.Flip(fp.GetPosition(), pcbnew.FLIP_DIRECTION_LEFT_RIGHT)   # KiCad 9+
        except Exception:
            fp.Flip(fp.GetPosition(), True)                             # KiCad 7/8
    def pad_xy(num):
        for p in fp.Pads():
            if p.GetNumber() == num:
                q = p.GetPosition()
                return (pcbnew.ToMM(q.x) - CX, CY - pcbnew.ToMM(q.y))
    # USB end (D12/D13, pads 15/16) must point to the truncated 90 deg vertex
    for _ in range(4):
        a, z = pad_xy("1"), pad_xy("15")
        if z[1] - a[1] > 10:
            break
        fp.SetOrientationDegrees((fp.GetOrientationDegrees() + 90) % 360)
    pads = [p.GetPosition() for p in fp.Pads()]
    cx = sum(pcbnew.ToMM(q.x) for q in pads) / len(pads) - CX
    cy = CY - sum(pcbnew.ToMM(q.y) for q in pads) / len(pads)
    target = (0.0, R - CUT - 0.5 - 22.5 - 1.27)   # pad-field centre; board centre is 1.27 mm toward USB
    pos = fp.GetPosition()
    fp.SetPosition(pcbnew.VECTOR2I(pos.x + pcbnew.FromMM(target[0] - cx),
                                   pos.y - pcbnew.FromMM(target[1] - cy)))

# SWD test-point holes (J3) and Qwiic holes (J1201) share the Nano footprint origin
if fp:
    for ref in ("J3", "J1201"):
        h = fp_of(ref)
        if h:
            h.SetOrientationDegrees(0)
            h.SetPosition(fp.GetPosition())
    # H6/H7: holes under the Nano's USB-end corner holes (M1.6 screws or alignment pins)
    for ref, dx in (("H6", 7.62), ("H7", -7.62)):
        h = fp_of(ref)
        if h:
            q = fp.GetPosition()
            h.SetPosition(pcbnew.VECTOR2I(q.x + pcbnew.FromMM(dx), q.y - pcbnew.FromMM(20.32)))

pcbnew.Refresh()
print("placed; missing references:", missing if missing else "none")
