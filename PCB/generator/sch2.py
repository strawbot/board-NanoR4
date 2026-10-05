"""sch2.py : second-generation schematic writer (KiCad 10 compatible text, stable UUIDs, wired layouts, no-collision fields).
Symbols come from libs2 (the definitions already embedded in the project, so they are known to load).
Every symbol / sheet UUID is taken from regen/uuid_registry.json when present so footprint links in the PCB keep working."""
import json, math, os, re, uuid
from libs2 import get as libget
HERE = os.path.dirname(os.path.abspath(__file__))
REG = json.load(open(os.path.join(HERE, 'regen', 'uuid_registry.json')))
PROJECT = 'hex_ctrl'
def U(): return str(uuid.uuid4())
G = 1.27
def g(v): return round(round(v / G) * G, 4)
def gp(p): return (g(p[0]), g(p[1]))
def f(v):
    v = round(v, 4)
    return ('%.4f' % v).rstrip('0').rstrip('.') if v != int(v) else str(int(v))
CW = 1.05      # approx character width of 1.27 mm text
TH = 1.5       # text height box

def sym_bbox(key):
    """local (y-up) bounding box of a lib symbol: graphics + pin ends"""
    t, pins = libget(key)
    xs = []; ys = []
    for m in re.finditer(r'\((?:xy|start|end|mid|center)\s+([-\d.]+)\s+([-\d.]+)\)', t):
        xs.append(float(m.group(1))); ys.append(float(m.group(2)))
    for m in re.finditer(r'\(center\s+([-\d.]+)\s+([-\d.]+)\)\s*\(radius\s+([-\d.]+)\)', t):
        cx, cy, r = float(m.group(1)), float(m.group(2)), float(m.group(3)); xs += [cx - r, cx + r]; ys += [cy - r, cy + r]
    for p in pins.values(): xs.append(p['x']); ys.append(p['y'])
    return (min(xs), min(ys), max(xs), max(ys)) if xs else (0, 0, 0, 0)

class Box:
    __slots__ = ('x0', 'y0', 'x1', 'y1')
    def __init__(s, x0, y0, x1, y1): s.x0, s.y0, s.x1, s.y1 = min(x0, x1), min(y0, y1), max(x0, x1), max(y0, y1)
    def hit(s, o): return s.x0 < o.x1 and s.x1 > o.x0 and s.y0 < o.y1 and s.y1 > o.y0
    def grow(s, d): return Box(s.x0 - d, s.y0 - d, s.x1 + d, s.y1 + d)

class Sheet:
    def __init__(self, fname, title, paper='A3', base0=0):
        self.fname = fname; self.stem = fname.replace('.kicad_sch', ''); self.title = title; self.paper = paper; self.base0 = base0
        self.uuid = REG['files'].get(self.stem) if 'files' in REG else None
        self.uuid = self.uuid or (REG['root'] if self.stem == 'hex_ctrl' else U())
        self.items = []; self.libs = {}; self.pins = {}; self.instances = []; self.pcount = 0; self.fcount = 0
        self.occ = []       # occupied boxes (for field placement)
        self.notes = []
    # ---- library
    def lib(self, key):
        if key not in self.libs:
            t, p = libget(key); self.libs[key] = t; self.pins[key] = p
        return key
    # ---- geometry
    @staticmethod
    def rot(px, py, r):
        c = round(math.cos(math.radians(r))); s = round(math.sin(math.radians(r)))
        return px * c - py * s, px * s + py * c
    def tx(self, s, px, py):
        if s.get('mx'): px = -px
        if s.get('my'): py = -py
        rx, ry = self.rot(px, py, s['r']); return (s['X'] + rx, s['Y'] - ry)
    def pin(self, s, num):
        p = self.pins[s['lib']][num]; return self.tx(s, p['x'], p['y'])
    def out(self, s, num):
        p = self.pins[s['lib']][num]
        bx = math.cos(math.radians(p['a'])); by = math.sin(math.radians(p['a']))
        if s.get('mx'): bx = -bx
        if s.get('my'): by = -by
        rx, ry = self.rot(bx, by, s['r']); return (-round(rx), round(ry))
    def sym_box(self, s):
        x0, y0, x1, y1 = sym_bbox(s['lib']); pts = [self.tx(s, x, y) for x, y in ((x0, y0), (x0, y1), (x1, y0), (x1, y1))]
        return Box(min(p[0] for p in pts), min(p[1] for p in pts), max(p[0] for p in pts), max(p[1] for p in pts))
    # ---- items
    def sym(self, libkey, ref, value, fp='', X=0, Y=0, r=0, dnp=False, fields=None, inbom=True, mx=False, my=False, place=None, fpos=None):
        ref0 = ref(self.base0) if callable(ref) else ref
        key = f'{self.stem}:{ref0}'
        uid = REG['syms'].get(key) or U()
        o = dict(kind='sym', lib=libkey, ref=ref, value=value, fp=fp, X=g(X), Y=g(Y), r=r, dnp=dnp, uuid=uid, fields=fields or {}, inbom=inbom, mx=mx, my=my, place=place, fpos=fpos)
        self.items.append(o); return o
    def wire(self, *pts):
        for a, b in zip(pts, pts[1:]):
            a, b = gp(a), gp(b)
            if a != b: self.items.append(dict(kind='wire', a=a, b=b, uuid=U()))
    def junction(self, p): self.items.append(dict(kind='junc', p=gp(p), uuid=U()))
    def nc(self, p): self.items.append(dict(kind='nc', p=gp(p), uuid=U()))
    def text(self, p, t, size=1.27): self.items.append(dict(kind='text', p=p, t=t, size=size, uuid=U()))
    def bus(self, *pts):
        for a, b in zip(pts, pts[1:]): self.items.append(dict(kind='bus', a=gp(a), b=gp(b), uuid=U()))
    def bus_entry(self, p, dx=2.54, dy=-2.54): self.items.append(dict(kind='busent', p=gp(p), dx=dx, dy=dy, uuid=U()))
    def poly(self, pts, dash=True): self.items.append(dict(kind='poly', pts=[gp(p) for p in pts], dash=dash, uuid=U()))
    def label(self, kind, name, p, direction, shape='passive'):
        ang = {(1, 0): 0, (-1, 0): 180, (0, -1): 90, (0, 1): 270}[direction]
        self.items.append(dict(kind=kind, name=name, p=gp(p), ang=ang, shape=shape, uuid=U()))
    def power(self, net, p, direction):
        key = self.lib(('hex_ctrl:' if net == 'VIN' else 'power:') + net)
        if net == 'GND': r = {(0, 1): 0, (1, 0): 90, (0, -1): 180, (-1, 0): 270}[direction]
        else: r = {(0, -1): 0, (-1, 0): 90, (0, 1): 180, (1, 0): 270}[direction]
        if net == 'PWR_FLAG': self.fcount += 1; n = self.fcount; ref = lambda b, n=n: f'#FLG{b + n:04d}'
        else: self.pcount += 1; n = self.pcount; ref = lambda b, n=n: f'#PWR{b + n:04d}'
        p = gp(p); return self.sym(key, ref, net, '', p[0], p[1], r, inbom=False)
    def stub(self, s, num, kind, name=None, L=2.54, shape='passive'):
        p = self.pin(s, num); d = self.out(s, num)
        if kind == 'nc': self.nc(p); return p
        e = (p[0] + d[0] * L, p[1] + d[1] * L); self.wire(p, e)
        if kind == 'power': self.power(name, e, d)
        else: self.label(kind, name, e, d, shape)
        return e
    def sheet(self, name, file, X, Y, W, H, pins, key=None, page_of=None):
        """pins: list of (name, type, side 'l'|'r'|'t'|'b', offset_mm from top-left along the side)"""
        X, Y, W, H = g(X), g(Y), g(W), g(H)
        uid = REG['sheets'].get(key or f'{self.stem}:{name}') or U()
        o = dict(kind='sheet', name=name, file=file, X=X, Y=Y, W=W, H=H, uuid=uid, pins=[], page_of=page_of)
        for (pn, t, side, off) in pins:
            if side == 'l': o['pins'].append((pn, t, X, Y + g(off), 180))
            elif side == 'r': o['pins'].append((pn, t, X + W, Y + g(off), 0))
            elif side == 't': o['pins'].append((pn, t, X + g(off), Y, 90))
            else: o['pins'].append((pn, t, X + g(off), Y + H, 270))
        self.items.append(o); return o
    def spin(self, sh, pn):
        for (n, t, x, y, a) in sh['pins']:
            if n == pn: return (x, y), {180: (-1, 0), 0: (1, 0), 90: (0, -1), 270: (0, 1)}[a]
        raise KeyError(pn)
    def sheet_pin_stub(self, sh, pn, kind, name, L=5.08, shape='passive'):
        (x, y), d = self.spin(sh, pn); e = (x + d[0] * L, y + d[1] * L); self.wire((x, y), e)
        self.label(kind, name, e, d, shape); return e
    # ---- field placement (no collisions)
    def _textbox(self, x, y, s, just='left', size=1.27, ang=0):
        w = len(s) * CW * size / 1.27; h = TH * size / 1.27
        if ang == 90: x0 = x - h / 2; return Box(x0, y - w, x0 + h, y) if just == 'left' else Box(x0, y, x0 + h, y + w)
        if just == 'left': return Box(x, y - h / 2, x + w, y + h / 2)
        if just == 'right': return Box(x - w, y - h / 2, x, y + h / 2)
        return Box(x - w / 2, y - h / 2, x + w / 2, y + h / 2)
    def _collect(self):
        occ = []
        for it in self.items:
            k = it['kind']
            if k == 'wire' or k == 'bus':
                occ.append(Box(it['a'][0], it['a'][1], it['b'][0], it['b'][1]).grow(0.35))
            elif k == 'junc': occ.append(Box(it['p'][0], it['p'][1], it['p'][0], it['p'][1]).grow(0.6))
            elif k == 'nc': occ.append(Box(it['p'][0], it['p'][1], it['p'][0], it['p'][1]).grow(0.9))
            elif k == 'sheet':
                occ.append(Box(it['X'], it['Y'], it['X'] + it['W'], it['Y'] + it['H']).grow(0.3))
                occ.append(Box(it['X'], it['Y'] - 2.2, it['X'] + len(it['name']) * CW, it['Y']))
                occ.append(Box(it['X'], it['Y'] + it['H'], it['X'] + (len(it['file']) + 6) * CW, it['Y'] + it['H'] + 2.4))
                for (n, t, x, y, a) in it['pins']:
                    w = len(n) * CW + 1
                    occ.append(Box(x, y - 0.9, x + w, y + 0.9) if a == 180 else Box(x - w, y - 0.9, x, y + 0.9) if a == 0 else Box(x - 0.9, y, x + 0.9, y + w) if a == 90 else Box(x - 0.9, y - w, x + 0.9, y))
            elif k == 'text':
                for i, line in enumerate(it['t'].split('\n')): pass
                occ.append(self._textbox(it['p'][0], it['p'][1] - 0.7, it['t'], 'left', it['size']))
            elif k in ('label', 'global', 'hier'):
                x, y = it['p']; w = len(it['name']) * CW + (3 if k != 'label' else 0.5); a = it['ang']
                occ.append(Box(x, y - 1.1, x + w, y + 0.3) if a == 0 else Box(x - w, y - 1.1, x, y + 0.3) if a == 180 else Box(x - 1.1, y - w, x + 0.3, y) if a == 90 else Box(x - 1.1, y, x + 0.3, y + w))
            elif k == 'sym':
                sb = self.sym_box(it); occ.append(sb.grow(0.25)); it['_box'] = sb
        return occ
    def layout_fields(self):
        occ = self._collect()
        for it in self.items:
            if it['kind'] != 'sym': continue
            r0 = it['ref'](self.base0) if callable(it['ref']) else it['ref']
            if r0.startswith('#'):                       # power symbol: value text at fixed offset
                X, Y = it['X'], it['Y']; val = it['value']
                if val == 'GND' and it['r'] == 90: vpos = (X + 3.6, Y + 0.3, 'left')
                elif val == 'GND' and it['r'] == 270: vpos = (X - 3.6, Y + 0.3, 'right')
                else: vpos = (X, (Y + 3.8 if val == 'GND' else Y - 3.5), 'left')
                it['_f'] = [('Reference', r0, X, Y + 4, True, 0, 'left'), ('Value', val, vpos[0], vpos[1], False, 0, vpos[2])]
                continue
        for it in self.items:
            if it['kind'] != 'sym' or '_f' in it: continue
            r0 = it['ref'](self.base0) if callable(it['ref']) else it['ref']
            sb = it['_box']
            if it['dnp']: sb = sb.grow(1.8)
            cx = (sb.x0 + sb.x1) / 2; cy = (sb.y0 + sb.y1) / 2
            ref, val = r0, it['value']
            if it.get('fpos'):
                (xr, yr, jr), (xv, yv, jv) = it['fpos']['ref'], it['fpos']['val']
                it['_f'] = [('Reference', ref, xr, yr, False, 0, jr), ('Value', val, xv, yv, False, 0, jv)]
                occ += [self._textbox(xr, yr, ref, jr), self._textbox(xv, yv, val, jv)]
                continue
            plan = it.get('place')
            cands = []
            if plan: cands.append(plan)
            cands += ['r', 'l', 'a', 'b', 'r2', 'l2']
            chosen = None
            for c in cands:
                if c == 'r':   rb = self._textbox(sb.x1 + 0.8, cy - 0.9, ref, 'left'); vb = self._textbox(sb.x1 + 0.8, cy + 0.9, val, 'left'); pos = (('left', sb.x1 + 0.8, cy - 0.9), ('left', sb.x1 + 0.8, cy + 0.9))
                elif c == 'l': rb = self._textbox(sb.x0 - 0.8, cy - 0.9, ref, 'right'); vb = self._textbox(sb.x0 - 0.8, cy + 0.9, val, 'right'); pos = (('right', sb.x0 - 0.8, cy - 0.9), ('right', sb.x0 - 0.8, cy + 0.9))
                elif c == 'a': rb = self._textbox(cx, sb.y0 - 3.4, ref, 'center'); vb = self._textbox(cx, sb.y0 - 1.5, val, 'center'); pos = (('center', cx, sb.y0 - 3.4), ('center', cx, sb.y0 - 1.5))
                elif c == 'b': rb = self._textbox(cx, sb.y1 + 1.5, ref, 'center'); vb = self._textbox(cx, sb.y1 + 3.4, val, 'center'); pos = (('center', cx, sb.y1 + 1.5), ('center', cx, sb.y1 + 3.4))
                elif c == 'r2': rb = self._textbox(sb.x1 + 0.8, cy - 2.8, ref, 'left'); vb = self._textbox(sb.x1 + 0.8, cy + 2.8, val, 'left'); pos = (('left', sb.x1 + 0.8, cy - 2.8), ('left', sb.x1 + 0.8, cy + 2.8))
                else:          rb = self._textbox(sb.x0 - 0.8, cy - 2.8, ref, 'right'); vb = self._textbox(sb.x0 - 0.8, cy + 2.8, val, 'right'); pos = (('right', sb.x0 - 0.8, cy - 2.8), ('right', sb.x0 - 0.8, cy + 2.8))
                if not any(rb.hit(o) or vb.hit(o) for o in occ):
                    chosen = (c, pos, rb, vb); break
            if not chosen:
                c = 'r'; rb = self._textbox(sb.x1 + 0.8, cy - 0.9, ref, 'left'); vb = self._textbox(sb.x1 + 0.8, cy + 0.9, val, 'left')
                chosen = (c, (('left', sb.x1 + 0.8, cy - 0.9), ('left', sb.x1 + 0.8, cy + 0.9)), rb, vb)
                self.notes.append(f'{self.stem}: fields of {r0} could not be placed clear')
            (jr, xr, yr), (jv, xv, yv) = chosen[1]
            occ += [chosen[2], chosen[3]]
            hide_val = False
            it['_f'] = [('Reference', ref, xr, yr, False, 0, jr), ('Value', val, xv, yv, False, 0, jv)]
    # ---- output
    def render(self, is_root=False):
        self.layout_fields()
        o = [f'(kicad_sch (version 20230121) (generator eeschema)\n  (uuid {self.uuid})\n  (paper "{self.paper}")']
        o.append(f'  (title_block (title "{self.title}") (rev "2") (company "Hex muscle-wire controller") (comment 1 "Layout regenerated 2026-10-04; rev 2: Vin rail, keep-alive removed"))')
        o.append('  (lib_symbols\n' + '\n'.join('    ' + t.strip() for t in self.libs.values()) + '\n  )')
        for it in self.items:
            k = it['kind']
            if k == 'wire': o.append(f'  (wire (pts (xy {f(it["a"][0])} {f(it["a"][1])}) (xy {f(it["b"][0])} {f(it["b"][1])})) (stroke (width 0) (type default)) (uuid {it["uuid"]}))')
            elif k == 'bus': o.append(f'  (bus (pts (xy {f(it["a"][0])} {f(it["a"][1])}) (xy {f(it["b"][0])} {f(it["b"][1])})) (stroke (width 0) (type default)) (uuid {it["uuid"]}))')
            elif k == 'busent': o.append(f'  (bus_entry (at {f(it["p"][0])} {f(it["p"][1])}) (size {f(it["dx"])} {f(it["dy"])}) (stroke (width 0) (type default)) (uuid {it["uuid"]}))')
            elif k == 'poly':
                pts = ' '.join(f'(xy {f(x)} {f(y)})' for x, y in it['pts'])
                o.append(f'  (polyline (pts {pts}) (stroke (width 0.2) (type {"dash" if it["dash"] else "solid"})) (uuid {it["uuid"]}))')
            elif k == 'junc': o.append(f'  (junction (at {f(it["p"][0])} {f(it["p"][1])}) (diameter 0) (color 0 0 0 0) (uuid {it["uuid"]}))')
            elif k == 'nc': o.append(f'  (no_connect (at {f(it["p"][0])} {f(it["p"][1])}) (uuid {it["uuid"]}))')
            elif k == 'text':
                t = it['t'].replace('"', '\\"').replace('\n', '\\n')
                o.append(f'  (text "{t}" (at {f(it["p"][0])} {f(it["p"][1])} 0) (effects (font (size {it["size"]} {it["size"]})) (justify left bottom)) (uuid {it["uuid"]}))')
            elif k in ('label', 'global', 'hier'):
                a = it['ang']; j = 'left' if a in (0, 90) else 'right'; x, y = it['p']
                if k == 'label': o.append(f'  (label "{it["name"]}" (at {f(x)} {f(y)} {a}) (effects (font (size 1.27 1.27)) (justify {j} bottom)) (uuid {it["uuid"]}))')
                elif k == 'global': o.append(f'  (global_label "{it["name"]}" (shape {it["shape"]}) (at {f(x)} {f(y)} {a}) (effects (font (size 1.27 1.27)) (justify {j})) (uuid {it["uuid"]})\n    (property "Intersheetrefs" "${{INTERSHEET_REFS}}" (at {f(x)} {f(y)} 0) (effects (font (size 1.27 1.27)) hide)))')
                else: o.append(f'  (hierarchical_label "{it["name"]}" (shape {it["shape"]}) (at {f(x)} {f(y)} {a}) (effects (font (size 1.27 1.27)) (justify {j})) (uuid {it["uuid"]}))')
            elif k == 'sym':
                insts = [(p, it['ref'](b) if callable(it['ref']) else it['ref']) for (p, b, pg) in self.instances]
                ref0 = insts[0][1]; X, Y, r = it['X'], it['Y'], it['r']
                fa = 90 if it['r'] in (90, 270) else 0
                props = [(n_, v_, x_, y_, h_, fa, j_) for (n_, v_, x_, y_, h_, a_, j_) in it['_f']] + [('Footprint', it['fp'], X, Y + 5, True, 0, 'left'), ('Datasheet', '~', X, Y, True, 0, 'left')]
                for kk, vv in it['fields'].items(): props.append((kk, vv, X, Y + 7, True, 0, 'left'))
                ps = '\n'.join(f'    (property "{n}" "{v}" (at {f(x)} {f(y)} {a}) (effects (font (size 1.27 1.27)){"" if j == "center" else f" (justify {j})"}{" hide" if h else ""}))' for n, v, x, y, h, a, j in props)
                pins = '\n'.join(f'    (pin "{n}" (uuid {U()}))' for n in self.pins[it['lib']])
                inst = '\n'.join(f'        (path "{p}" (reference "{rf}") (unit 1))' for p, rf in insts)
                mir = (' (mirror y)' if it.get('mx') else '') + (' (mirror x)' if it.get('my') else '')
                o.append(f'  (symbol (lib_id "{it["lib"]}") (at {f(X)} {f(Y)} {r}){mir} (unit 1)\n    (in_bom {"yes" if it["inbom"] else "no"}) (on_board yes) (dnp {"yes" if it["dnp"] else "no"})\n    (uuid {it["uuid"]})\n{ps}\n{pins}\n    (instances\n      (project "{PROJECT}"\n{inst}\n      )\n    )\n  )')
            elif k == 'sheet':
                X, Y, W, H = it['X'], it['Y'], it['W'], it['H']
                pins = '\n'.join(f'    (pin "{n}" {t} (at {f(x)} {f(y)} {a}) (effects (font (size 1.27 1.27)) (justify {"left" if a == 180 else "right" if a == 0 else "right"})) (uuid {U()}))' for n, t, x, y, a in it['pins'])
                inst = '\n'.join(f'        (path "{p}" (page "{it["page_of"](p)}"))' for (p, b, pg) in self.instances)
                o.append(f'  (sheet (at {f(X)} {f(Y)}) (size {f(W)} {f(H)}) (fields_autoplaced)\n    (stroke (width 0.1524) (type solid))\n    (fill (color 0 0 0 0.0000))\n    (uuid {it["uuid"]})\n    (property "Sheetname" "{it["name"]}" (at {f(X)} {f(Y - 0.7)} 0) (effects (font (size 1.27 1.27)) (justify left bottom)))\n    (property "Sheetfile" "{it["file"]}" (at {f(X)} {f(Y + H + 0.6)} 0) (effects (font (size 1.27 1.27)) (justify left top)))\n{pins}\n    (instances\n      (project "{PROJECT}"\n{inst}\n      )\n    )\n  )')
        if is_root: o.append('  (sheet_instances\n    (path "/" (page "1"))\n  )')
        o.append(')')
        return '\n'.join(o) + '\n'
