import math, pcbnew

A   = 60.0                 # across flats, mm  (parametric)
CUT = 6.0                  # truncation of the USB vertex, mm
CX, CY = 150.0, 100.0      # board centre on the KiCad sheet, mm
R   = A / math.sqrt(3)     # vertex radius
b   = pcbnew.GetBoard()

def P(x, y):               # maths coords (y up) -> KiCad (y down)
    return pcbnew.VECTOR2I(pcbnew.FromMM(CX + x), pcbnew.FromMM(CY - y))

def seg(p, q):
    s = pcbnew.PCB_SHAPE(b)
    s.SetShape(pcbnew.SHAPE_T_SEGMENT)
    s.SetStart(P(*p)); s.SetEnd(P(*q))
    s.SetLayer(pcbnew.Edge_Cuts); s.SetWidth(pcbnew.FromMM(0.1))
    b.Add(s)

def pol(r, deg):
    return (r*math.cos(math.radians(deg)), r*math.sin(math.radians(deg)))

def along(a, c, d):
    L = math.dist(a, c)
    return (a[0] + (c[0]-a[0])*d/L, a[1] + (c[1]-a[1])*d/L)

V = [pol(R, 30 + 60*k) for k in range(6)]
pts = []
for k in range(6):
    if k == 1:                              # vertex at 90 deg: USB end
        pts += [along(V[1], V[0], CUT), along(V[1], V[2], CUT)]
    else:
        pts.append(V[k])
for i in range(len(pts)):
    seg(pts[i], pts[(i+1) % len(pts)])

# Limb hole groups J1..J6 (PinHeader_1x05_P2.54mm footprint, no header fitted).
# Footprint origin is pin 1; pins run along the edge, pin 3 (+10 V) centred.
for k in range(6):
    fp = b.FindFootprintByReference(f"J{k+1}")
    if fp is None:
        continue
    phi = 60*k
    c = pol(A/2 - 3, phi)
    d = (math.sin(math.radians(phi)), -math.cos(math.radians(phi)))
    pin1 = (c[0] - 2*2.54*d[0], c[1] - 2*2.54*d[1])
    fp.SetOrientationDegrees(phi)
    fp.SetPosition(P(*pin1))

# M2 mounting holes H1..H5 at every vertex except the USB one (90 deg)
for n, k in enumerate([0, 2, 3, 4, 5], start=1):
    fp = b.FindFootprintByReference(f"H{n}")
    if fp: fp.SetPosition(P(*pol(R - 3.6, 30 + 60*k)))

for ref, xy in [("U_IMU", (0, 0))]:
    fp = b.FindFootprintByReference(ref)
    if fp: fp.SetPosition(P(*xy))

pcbnew.Refresh()