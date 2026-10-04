import pcbnew,re,sys
from shapely.geometry import Polygon
t=open('/tmp/hex.net').read()
b=pcbnew.BOARD()
for m in re.finditer(r'\(comp \(ref "([^"]+)"\).*?\(footprint "([^"]+)"\)',t,re.S):
    ref,fpn=m.groups(); lib,name=fpn.split(':')
    path='/home/claude/kc/hex_ctrl/hex_ctrl.pretty' if lib=='hex_ctrl' else f'/usr/share/kicad/footprints/{lib}.pretty'
    fp=pcbnew.FootprintLoad(path,name); fp.SetReference(ref); b.Add(fp)
_HEX_TEST_BOARD=b
exec(open('/home/claude/kc/hex_ctrl/hex_place.py').read())
pcbnew.SaveBoard('/tmp/test.kicad_pcb',b)
polys={}
for fp in b.GetFootprints():
    fp.BuildCourtyardCaches()
    side='B' if fp.IsFlipped() else 'F'
    cy=fp.GetCourtyard(pcbnew.B_CrtYd if side=='B' else pcbnew.F_CrtYd)
    if cy.OutlineCount()==0: continue
    o=cy.Outline(0); pts=[(pcbnew.ToMM(o.CPoint(i).x),pcbnew.ToMM(o.CPoint(i).y)) for i in range(o.PointCount())]
    if len(pts)>2: polys[fp.GetReference()]=(side,Polygon(pts).buffer(0))
refs=list(polys); bad=[]
for i,a in enumerate(refs):
    for c in refs[i+1:]:
        if polys[a][0]!=polys[c][0]: continue
        if polys[a][1].buffer(0.03).intersects(polys[c][1].buffer(0.03)): bad.append((a,c,round(polys[a][1].intersection(polys[c][1]).area,2)))
# outline containment
edge=Polygon([(pcbnew.ToMM(d.GetStart().x),pcbnew.ToMM(d.GetStart().y)) for d in b.GetDrawings() if d.GetLayer()==pcbnew.Edge_Cuts])
out=[r for r,(s,p) in polys.items() if not edge.buffer(0.01).contains(p)]
print("overlaps:",len(bad)); print(bad[:60]); print("outside board:",out)
