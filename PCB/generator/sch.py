import uuid, math
from libs import get_symbol
PROJECT="hex_ctrl"
def U(): return str(uuid.uuid4())
def g(v): return round(round(v/1.27)*1.27,4)
def gp(p): return (g(p[0]),g(p[1]))
def f(v): 
    v=round(v,4)
    return ("%.4f"%v).rstrip('0').rstrip('.') if v!=int(v) else str(int(v))
FONT='(effects (font (size 1.27 1.27)))'
POWER={"GND":("power","GND"),"+10V":("power","+10V"),"+5V":("power","+5V"),"+3V3":("power","+3V3"),"PWR_FLAG":("power","PWR_FLAG")}

class Sheet:
    def __init__(self, fname, title, paper="A3"):
        self.fname=fname; self.title=title; self.paper=paper
        self.uuid=U(); self.items=[]; self.libs={}; self.pins={}
        self.instances=[]   # list of (path, base, page)
        self.pcount=0; self.fcount=0
    def lib(self,lib,name,as_name=None,newlib=None):
        key=f"{newlib or lib}:{as_name or name}"
        if key not in self.libs:
            txt,pins=get_symbol(lib,name,as_name,newlib)
            self.libs[key]=txt; self.pins[key]=pins
        return key
    def customlib(self,key,txt,pins):
        self.libs[key]=txt; self.pins[key]=pins; return key
    # ---- geometry
    @staticmethod
    def rot(px,py,r):
        c=round(math.cos(math.radians(r))); s=round(math.sin(math.radians(r)))
        return px*c-py*s, px*s+py*c
    def sym(self, libkey, ref, value, fp="", X=0, Y=0, r=0, dnp=False, fields=None, inbom=True):
        o=dict(kind="sym",lib=libkey,ref=ref,value=value,fp=fp,X=g(X),Y=g(Y),r=r,dnp=dnp,uuid=U(),fields=fields or {},inbom=inbom)
        self.items.append(o); return o
    def pin(self, s, num):
        p=self.pins[s["lib"]][num]
        rx,ry=self.rot(p["x"],p["y"],s["r"])
        return (s["X"]+rx, s["Y"]-ry)
    def out(self, s, num):
        p=self.pins[s["lib"]][num]
        bx,by=self.rot(math.cos(math.radians(p["a"])), math.sin(math.radians(p["a"])), s["r"])
        return (-round(bx), round(by))
    def wire(self,*pts):
        for a,b in zip(pts,pts[1:]):
            self.items.append(dict(kind="wire",a=gp(a),b=gp(b),uuid=U()))
    def junction(self,p): self.items.append(dict(kind="junc",p=gp(p),uuid=U()))
    def nc(self,p): self.items.append(dict(kind="nc",p=gp(p),uuid=U()))
    def text(self,p,t,size=1.5): self.items.append(dict(kind="text",p=p,t=t,size=size,uuid=U()))
    def label(self,kind,name,p,direction,shape="passive"):
        ang={(1,0):0,(-1,0):180,(0,-1):90,(0,1):270}[direction]
        self.items.append(dict(kind=kind,name=name,p=gp(p),ang=ang,shape=shape,uuid=U()))
    def power(self,net,p,direction):
        key=self.lib(*POWER[net])
        if net=="GND": r={(0,1):0,(1,0):90,(0,-1):180,(-1,0):270}[direction]
        else: r={(0,-1):0,(-1,0):90,(0,1):180,(1,0):270}[direction]
        if net=="PWR_FLAG":
            self.fcount+=1; n=self.fcount; ref=lambda b,n=n: f"#FLG{b+n:04d}"
        else:
            self.pcount+=1; n=self.pcount; ref=lambda b,n=n: f"#PWR{b+n:04d}"
        p=gp(p); s=self.sym(key,ref,net,"",p[0],p[1],r,inbom=False)
        return s
    def stub(self,s,num,kind,name=None,L=2.54,shape="passive"):
        p=self.pin(s,num); d=self.out(s,num)
        if kind=="nc": self.nc(p); return p
        e=(p[0]+d[0]*L,p[1]+d[1]*L)
        self.wire(p,e)
        if kind=="power": self.power(name,e,d)
        else: self.label(kind,name,e,d,shape)
        return e
    def sheet(self, name, file, X, Y, W, H, pins_left, pins_right, page_of):
        X,Y,W,H=g(X),g(Y),g(W),g(H)
        o=dict(kind="sheet",name=name,file=file,X=X,Y=Y,W=W,H=H,uuid=U(),pins=[],page_of=page_of)
        for i,pn in enumerate(pins_left): o["pins"].append((pn,"input",X,Y+2.54*(i+1),180))
        for i,pn in enumerate(pins_right): o["pins"].append((pn,"passive",X+W,Y+2.54*(i+1),0))
        self.items.append(o); return o
    def sheet_pin_stub(self, sh, pn, kind, name, L=5.08, shape="passive"):
        for (n,t,x,y,a) in sh["pins"]:
            if n==pn:
                d=(-1,0) if a==180 else (1,0)
                e=(x+d[0]*L,y); self.wire((x,y),e); self.label(kind,name,e,d,shape); return e
        raise KeyError(pn)
    # ---- output
    def render(self, is_root=False):
        o=[f'(kicad_sch (version 20230121) (generator eeschema)\n  (uuid {self.uuid})\n  (paper "{self.paper}")']
        o.append(f'  (title_block (title "{self.title}") (rev "1") (company "Hex muscle-wire controller") (comment 1 "Generated starter schematic - verify before fabrication"))')
        o.append('  (lib_symbols\n'+"\n".join(self.libs.values())+'\n  )')
        for it in self.items:
            k=it["kind"]
            if k=="wire":
                o.append(f'  (wire (pts (xy {f(it["a"][0])} {f(it["a"][1])}) (xy {f(it["b"][0])} {f(it["b"][1])})) (stroke (width 0) (type default)) (uuid {it["uuid"]}))')
            elif k=="junc":
                o.append(f'  (junction (at {f(it["p"][0])} {f(it["p"][1])}) (diameter 0) (color 0 0 0 0) (uuid {it["uuid"]}))')
            elif k=="nc":
                o.append(f'  (no_connect (at {f(it["p"][0])} {f(it["p"][1])}) (uuid {it["uuid"]}))')
            elif k=="text":
                t=it["t"].replace('"','\\"')
                o.append(f'  (text "{t}" (at {f(it["p"][0])} {f(it["p"][1])} 0) (effects (font (size {it["size"]} {it["size"]})) (justify left bottom)) (uuid {it["uuid"]}))')
            elif k in("label","global","hier"):
                a=it["ang"]; j="left" if a in(0,90) else "right"
                x,y=it["p"]
                if k=="label":
                    o.append(f'  (label "{it["name"]}" (at {f(x)} {f(y)} {a}) (fields_autoplaced) (effects (font (size 1.27 1.27)) (justify {j} bottom)) (uuid {it["uuid"]}))')
                elif k=="global":
                    o.append(f'  (global_label "{it["name"]}" (shape {it["shape"]}) (at {f(x)} {f(y)} {a}) (fields_autoplaced) (effects (font (size 1.27 1.27)) (justify {j})) (uuid {it["uuid"]})\n    (property "Intersheetrefs" "${{INTERSHEET_REFS}}" (at {f(x)} {f(y)} 0) (effects (font (size 1.27 1.27)) hide)))')
                else:
                    o.append(f'  (hierarchical_label "{it["name"]}" (shape {it["shape"]}) (at {f(x)} {f(y)} {a}) (fields_autoplaced) (effects (font (size 1.27 1.27)) (justify {j})) (uuid {it["uuid"]}))')
            elif k=="sym":
                insts=[(p,it["ref"](b) if callable(it["ref"]) else it["ref"]) for (p,b,pg) in self.instances]
                ref0=insts[0][1]
                X,Y,r=it["X"],it["Y"],it["r"]
                pl=self.pins[it["lib"]]; n=len(pl)
                ang=90 if r in (90,270) else 0
                vert_lib = n==2 and all(abs(p["x"])<0.01 for p in pl.values())
                horiz_lib = n==2 and not vert_lib
                if ref0.startswith("#"):
                    props=[("Reference",ref0,X,Y+4,True,0,"left"),("Value",it["value"],X,(Y+3.8 if it["value"]=="GND" else Y-3.5),False,0,"left")]
                elif n==0:
                    props=[("Reference",ref0,X+2.5,Y-1.3,False,0,"left"),("Value",it["value"],X+2.5,Y+1.3,False,0,"left")]
                elif (vert_lib and r in(0,180)) or (horiz_lib and r in(90,270)):
                    props=[("Reference",ref0,X+2.5,Y-1.3,False,ang,"left"),("Value",it["value"],X+2.5,Y+1.3,False,ang,"left")]
                elif n==2:
                    props=[("Reference",ref0,X-2,Y-2.8,False,ang,"left"),("Value",it["value"],X-2,Y+3.6,False,ang,"left")]
                elif n==3:
                    props=[("Reference",ref0,X+5,Y-1.3,False,0,"left"),("Value",it["value"],X+5,Y+1.3,False,0,"left")]
                else:
                    top=max(p["y"] for p in pl.values()); bot=min(p["y"] for p in pl.values())
                    props=[("Reference",ref0,X+1.5,Y-top-1.5,False,0,"left"),("Value",it["value"],X+1.5,Y-bot+2.5,False,0,"left")]
                props+= [("Footprint",it["fp"],X,Y+5,True,0,"left"),("Datasheet","~",X,Y,True,0,"left")]
                for kk,vv in it["fields"].items(): props.append((kk,vv,X,Y+7,True,0,"left"))
                ps="\n".join(f'    (property "{n}" "{v}" (at {f(x)} {f(y)} {a}) (effects (font (size 1.27 1.27)) (justify {j}){" hide" if h else ""}))' for n,v,x,y,h,a,j in props)
                pins="\n".join(f'    (pin "{n}" (uuid {U()}))' for n in self.pins[it["lib"]])
                inst="\n".join(f'        (path "{p}" (reference "{rf}") (unit 1))' for p,rf in insts)
                o.append(f'  (symbol (lib_id "{it["lib"]}") (at {f(X)} {f(Y)} {r}) (unit 1)\n    (in_bom {"yes" if it["inbom"] else "no"}) (on_board yes) (dnp {"yes" if it["dnp"] else "no"})\n    (uuid {it["uuid"]})\n{ps}\n{pins}\n    (instances\n      (project "{PROJECT}"\n{inst}\n      )\n    )\n  )')
            elif k=="sheet":
                X,Y,W,H=it["X"],it["Y"],it["W"],it["H"]
                pins="\n".join(f'    (pin "{n}" {t} (at {f(x)} {f(y)} {a}) (effects (font (size 1.27 1.27)) (justify {"left" if a==180 else "right"})) (uuid {U()}))' for n,t,x,y,a in it["pins"])
                inst="\n".join(f'        (path "{p}" (page "{it["page_of"](p)}"))' for (p,b,pg) in self.instances)
                o.append(f'  (sheet (at {f(X)} {f(Y)}) (size {f(W)} {f(H)}) (fields_autoplaced)\n    (stroke (width 0.1524) (type solid))\n    (fill (color 0 0 0 0.0000))\n    (uuid {it["uuid"]})\n    (property "Sheetname" "{it["name"]}" (at {f(X)} {f(Y-0.7)} 0) (effects (font (size 1.27 1.27)) (justify left bottom)))\n    (property "Sheetfile" "{it["file"]}" (at {f(X)} {f(Y+H+0.6)} 0) (effects (font (size 1.27 1.27)) (justify left top)))\n{pins}\n    (instances\n      (project "{PROJECT}"\n{inst}\n      )\n    )\n  )')
        if is_root: o.append('  (sheet_instances\n    (path "/" (page "1"))\n  )')
        o.append(')')
        return "\n".join(o)+"\n"
