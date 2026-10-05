# oct_place.py - draws the octagonal outline and places every part in its zone.
# Run in the PCB editor: Tools > Scripting Console, then
#     exec(open(r"<path to this file>").read())
# after Tools > Update PCB from Schematic (F8). Safe to re-run: the outline is
# redrawn and every part is moved again. It gives a starting placement only;
# expect to nudge parts and fix courtyard overlaps before routing.
import math, pcbnew

A   = 72.0                  # across flats, mm
CX, CY = 150.0, 100.0       # board centre on the sheet, mm
R   = (A / 2) / math.cos(math.radians(22.5))   # vertex radius
# Eight sides so every limb edge lies on a 45 degree multiple (the autorouter only
# routes 0/45/90 degrees). Edges face 0,45,...,315 deg. The front (90, USB/Nano) and
# rear (270) edges carry no limb; the six limbs sit on the other six edges.
LIMB_ANGLES = [0, 45, 135, 180, 225, 315]       # limb 1..6
FRONT_CUT = 7.0   # mm sliced off the front (USB) flat: nothing but the Nano's USB end lives there, so the
                  # module overhangs the board. 7 mm is the limit: at 8 mm the limb 3 wire tracks come within
                  # 0.39 mm of the new edge (rule: 0.5 mm). Set 0 for the full octagon.

b = globals().get("_HEX_TEST_BOARD") or pcbnew.GetBoard()

def P(x, y):                 # maths coords (mm, y up) -> KiCad internal units
    return pcbnew.VECTOR2I(pcbnew.FromMM(CX + x), pcbnew.FromMM(CY - y))

def pol(r, deg):
    return (r * math.cos(math.radians(deg)), r * math.sin(math.radians(deg)))

missing = []
# Build the reference map from GetFootprints(): in some KiCad versions
# FindFootprintByReference returns an unwrapped SWIG pointer.
FPS = {}
for _fp in b.GetFootprints():
    FPS[_fp.GetReference()] = _fp

def fp_of(ref):
    fp = FPS.get(ref)
    if fp is None:
        missing.append(ref)
    return fp

def set_rot(fp, deg):
    deg = deg % 360
    try:
        fp.SetOrientationDegrees(deg)
    except AttributeError:
        fp.SetOrientation(pcbnew.EDA_ANGLE(deg, pcbnew.DEGREES_T))

def get_rot(fp):
    try:
        return fp.GetOrientationDegrees()
    except AttributeError:
        return fp.GetOrientation().AsDegrees()

def put(ref, xy, rot=0.0):
    fp = fp_of(ref)
    if fp is None:
        return
    set_rot(fp, rot)
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
pts = [pol(R, 22.5 + 45 * k) for k in range(8)]
if FRONT_CUT > 0:            # replace the two front vertices (67.5 and 112.5 deg) by the ends of a chord
    yc = A / 2 - FRONT_CUT
    xr = pts[0][0] + pts[0][1] - yc          # the 45 deg edge from vertex 22.5 deg satisfies x + y = const
    pts[1], pts[2] = (xr, yc), (-xr, yc)
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
    phi = LIMB_ANGLES[L - 1]
    to = frame(pol(A / 2, phi), phi)
    base = L * 100
    fp = fp_of(f"J{base + 1}")            # 1x05 header footprint, origin at pin 1
    if fp:
        set_rot(fp, phi)
        fp.SetPosition(P(*to(-2 * 2.54, -3.0)))
    for i in range(4):
        xl = (i - 1.5) * PITCH
        put(f"D{base + 1 + i}",  to(xl, -6.8),  phi)          # flyback, long axis radial
        put(f"Q{base + 1 + i}",  to(xl, -10.5), phi)          # AO3400A
        put(f"R{base + 1 + i}",  to(xl - 0.75, -16.2), phi)   # RG: innermost, so G_x from the register reaches pad 1
        put(f"R{base + 11 + i}", to(xl + 0.75, -14.0), phi)   # RPD
        put(f"C{base + 1 + i}",  to(xl - 0.75, -14.0), phi)   # CAC gate coupling: R.2 and C.1 are neighbours
        put(f"D{base + 11 + i}", to(xl + 0.8, -16.4), phi)    # BAT54 gate clamp

def at(r, ang, rot, ref):                   # polar placement, rot relative to radial
    put(ref, pol(r, ang), ang + rot)

# ---------------------------------------------------------------- groups
# Group A/B/C at the vertices 30/150/270 deg, between their two limbs.
for gi, v in enumerate([22.5, 157.5, 270]):
    RG = R if v != 270 else A / 2                     # group C sits mid-edge on the rear flat
    g = 700 + 100 * gi
    at(RG - 10.14, v, 90, f"R{g + 1}")                            # RSENSE 2512, tangential
    to = frame(pol(RG - 14.14, v), v)
    put(f"TP{g + 1}", to(-2.4, 0), v)
    put(f"R{g + 2}",  to(0.0, 0), v)                        # ISNS series R
    put(f"C{g + 1}",  to(1.6, 0), v)                        # ISNS filter C (DNP)

# ---------------------------------------------------------------- shift registers (74HC595)
# Round 2 placement (the one that routed completely): U1101/U1102 sit left and right of
# centre, turned so their outputs QB-QH face the gate resistors of their two limbs;
# U1103 sits below the IMU. Round 2 coordinates are (+-12.0, 2.7), U1103 (0.25, -8.0).
# The gate coupling caps/clamp diodes added since then fill the strip just outside the
# registers, so U1101/U1102 are pulled 1.0 mm toward the centre (SR_DX) and lifted
# 1.5 mm (SR_DY): the smallest change that clears every courtyard (checked 2026-10-03)
# while keeping the gate nets and SCK/RCLK/OE_N about as short as round 2. Do not
# put them back on the 10.8 mm ring: that cost ~30% more gate/clock wire and failed to route.
# U1103 sits 10.5 mm below round 2's spot (SR3_DY): its top-row pin pocket (OE_N/RCLK/SER escapes)
# was jammed against the IMU decoupling caps, and the Nano's SWD/I2C holes now occupy the area under
# the IMU (see "Nano interface" below). The rear band below it is free.
SR3_DY = 10.5
SR_DX, SR_DY = -1.0 + (A - 63.0) / 2, 1.5      # follows the limb strips outward
for ref, xy, rot in [("U1101", (12.0 + SR_DX, 2.7 + SR_DY), 270), ("C1101", (14.0 + SR_DX, -1.9 + SR_DY), 0),
                     ("U1102", (-12.0 - SR_DX, 2.7 + SR_DY), 270), ("C1102", (-8.3 - SR_DX, 3.0 + SR_DY), 0),
                     ("U1103", (0.25, -8.0 - SR3_DY), 90), ("C1103", (-5.0, -8.0 - SR3_DY), 90)]:
    put(ref, xy, rot)

# ---------------------------------------------------------------- power (247.5 deg vertex)
to = frame(pol(R - 8.34, 247.5), 247.5)
fp = fp_of("J1001")                # 1x02 holes, origin at pin 1, laid along the edge
if fp:
    set_rot(fp, 247.5 + 180)      # 1x04 (power + GND/TX/RX): pins 3-4 extend toward the free rear band, away from limb 5
    fp.SetPosition(P(*to(-1.27, 0)))
at(R - 12.14, 247.5, 90, "D1001")          # TVS
at(R - 17.44, 247.5, 0,  "Q1001")          # reverse-protection P-FET
at(11.3, 244.5, 90, "C1001")          # 100 uF polymer
at(7.3, 247.5, 90, "C1002")
at(7.3, 292.5, 90, "C1003")          # second ceramic, distributed
at(15.3, 247.5, 0, "R1001")
at(11.0, 274.5, 0, "R1002")           # VIN_S divider
at(11.0, 281.5, 0, "R1003")
at(13.4, 276.5, 0, "C1004")

# ---------------------------------------------------------------- SCK / RCLK series resistors
# (the keep-alive output-enable circuit that used to sit at the 292.5 deg vertex was removed: the gate guard on every channel
# makes it unnecessary, and OE is tied to GND)
for ref, ang in [("R1103", 296.5), ("R1104", 304.5)]:
    at(13.0, ang, 0, ref)

# ---------------------------------------------------------------- IMU at the centre
put("U1201", (0, 0), 0)
for ref, xy in [("C1201", (-3.0, -3.2)), ("C1202", (0.0, -3.2)), ("C1203", (3.0, -3.2)),
                ("R1201", (-4.2, 0.0)), ("R1202", (4.2, 1.0)), ("R1203", (4.2, -1.0)),
                ("TP1201", (0.0, 3.5))]:
    put(ref, xy, 0)
put("J1201", (-1.27 * 3, 10.4), 90)                  # Qwiic holes: PLACEHOLDER position

# ---------------------------------------------------------------- mounting holes
for n, ang in enumerate([22.5, 157.5, 202.5, 337.5], start=1):   # H1..H4; H5 on the rear flat
    put(f"H{n}", pol(R - 3.6, ang))
put("H5", pol(A / 2 - 3.6, 270))                 # rear flat, centred


# ---------------------------------------------------------------- Nano interface (positions from Arduino's ABX00142 CAD)
# The Nano lies on the bottom with its castellated edge on A1. Its Qwiic-side through-holes (SCL/SDA) and the
# SWD test pads on its underside therefore land at fixed offsets from A1; the board gets a 2.0 mm pad/1.0 mm hole
# at each. Nano CAD coords (nx, ny) -> board: x = 54.29 - ny, y = TY - 17.78 - (nx - 78.755)  (maths coords, y up),
# where TY is the A1 pad-field centre and 17.78 = 7 x 2.54 is half the pad run. SILK LABELS for these live in
# generator/nano_iface_pcb.py (absolute board text). Re-check against the physical Nano before ordering.
TY = A / 2 - 3.0 - 0.5 - 22.5 - 1.27
def nano_xy(nx, ny):
    return (54.29 - ny, TY - 17.78 - (nx - 78.755))
for ref, (nx, ny) in {"TP1202": (81.295, 59.370), "TP1203": (81.295, 49.210),        # SCL, SDA holes
                      "TP1204": (81.203, 51.741), "TP1205": (78.663, 54.281),        # SWDIO, SWCLK
                      "TP1206": (81.203, 56.821), "TP1207": (78.663, 56.821),        # NRST, GND
                      "TP1208": (78.663, 51.741)}.items():                            # 3V3
    put(ref, nano_xy(nx, ny), 0)

# ---------------------------------------------------------------- courtyard clean-up
# The polar/vertex placements above leave 17 small courtyard overlaps at A = 72 mm (sense
# resistors and testpoints against limb parts, the power and keep-alive blocks against the
# shift register U1103). These are the smallest moves that clear them, found with a
# nearest-free-spot search on 2026-10-03 (limb parts and the 74HC595s stay put). Absolute
# board coordinates (mm, y up) and rotation; re-derive if A or the offsets above change.
NUDGE = {
    "C1001": (-9.74, -8.13, 334),
    "C1002": (-2.86, -6.15, 338),
    "C1003": (2.73, -6.15, 22),
    "C1004": (1.38, -13.46, 276),
    "C901": (-3.99, -22.11, 270),
    "D1001": (-9.61, -25.04, 338),
    "D1002": (-14.92, -27.69, 90),
    "J1001": (-11.31, -28.53, 68),
    "Q1001": (-7.74, -19.89, 248),
    "R1002": (-1.05, -13.28, 274),
    "R1003": (-0.1, -11.77, 282),
    "R1103": (7.21, -12.59, 296),
    "R701": (26.93, 11.03, 112),
    "R801": (-26.93, 11.03, 248),
    "R902": (-3.0, -21.99, 270),
    "TP701": (22.54, 11.25, 22),
    "TP801": (-23.79, 7.98, 158),
    "TP901": (4.1, -21.86, 270),
}
for ref, (x, y, rot) in NUDGE.items():
    put(ref, (x, y), rot)

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
        set_rot(fp, get_rot(fp) + 90)
    pads = [p.GetPosition() for p in fp.Pads()]
    cx = sum(pcbnew.ToMM(q.x) for q in pads) / len(pads) - CX
    cy = CY - sum(pcbnew.ToMM(q.y) for q in pads) / len(pads)
    target = (0.0, TY)   # pad-field centre; USB end 3 mm inside the front flat, as in the hexagon; board centre is 1.27 mm toward USB
    pos = fp.GetPosition()
    fp.SetPosition(pcbnew.VECTOR2I(pos.x + pcbnew.FromMM(target[0] - cx),
                                   pos.y - pcbnew.FromMM(target[1] - cy)))

pcbnew.Refresh()
print("placed; missing references:", missing if missing else "none")
