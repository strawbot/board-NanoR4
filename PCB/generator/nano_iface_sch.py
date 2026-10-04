#!/usr/bin/env python3
"""Schematic edit (2026-10-04): replace the J1201 Qwiic placeholder with the Nano interface.

 - root  : A1 pin 17 (3V3) -> +3V3 rail; A1 pins 1/2 (D1/TX, D0/RX) -> global labels NANO_TX / NANO_RX
 - power : J1001 (PWR_IN) becomes 1x04: VIN_RAW, GND, NANO_TX, NANO_RX (shares the power ground pin)
 - imu   : J1201 removed; adds SDA/SCL solder pads and the five SWD-test-pad access holes, plus notes
Edits the .kicad_sch text in place so existing UUIDs (and the PCB's footprint links) are untouched.
"""
import re, sys, uuid
sys.path.insert(0, '/tmp')   # spans/sx helpers live in the session scratch; see helper copies below
U = lambda: str(uuid.uuid4())
ROOT = '5e2476d1-9393-45c1-9d68-57b7e780fc63'
SH_POWER = '8877e868-a16b-436f-8e27-312b3d53da25'
SH_IMU = 'c97bdb55-5522-40d8-85df-2152a7e2a132'

def spans(s):
    out = []; d = 0; i = 0; n = len(s); start = None
    while i < n:
        c = s[i]
        if c == '"':
            i += 1
            while s[i] != '"':
                if s[i] == '\\': i += 1
                i += 1
        elif c == '(':
            d += 1
            if d == 2: start = i
        elif c == ')':
            if d == 2: out.append((start, i + 1))
            d -= 1
        i += 1
    return out

def items(s): return [(a, b, s[a:b]) for a, b in spans(s)]

def find_item(s, pred):
    for a, b, t in items(s):
        if pred(t): return a, b, t
    raise KeyError

def remove(s, pred, expect=None):
    rem = [(a, b) for a, b, t in items(s) if pred(t)]
    if expect is not None: assert len(rem) == expect, (len(rem), expect)
    for a, b in reversed(rem):
        # swallow the preceding indentation/newline
        a2 = a
        while a2 > 0 and s[a2 - 1] in ' \t': a2 -= 1
        if a2 > 0 and s[a2 - 1] == '\n': a2 -= 1
        s = s[:a2] + s[b:]
    return s

def append_items(s, texts):
    k = s.rstrip().rfind(')')
    return s[:k] + ''.join('\n  ' + t.strip() for t in texts) + '\n' + s[k:]

def libsym(src, name):
    a, b, t = find_item(src, lambda t: t.startswith('(lib_symbols'))
    sub = src[a:b]
    for x, y in spans(sub.replace('(lib_symbols', '(lib_symbols', 1)):
        pass
    # spans of children inside lib_symbols
    for x, y in spans(sub):
        if sub[x:y].startswith('(symbol "%s"' % name): return sub[x:y]
    raise KeyError(name)

def ensure_libsym(s, name, src):
    a, b, t = find_item(s, lambda t: t.startswith('(lib_symbols'))
    if ('(symbol "%s"' % name) in t: return s
    txt = libsym(src, name)
    k = b - 1
    return s[:k] + '\n    ' + txt.strip() + '\n  ' + s[k:]

def prop_at_shift(block, dy):
    return re.sub(r'(\(property "[^"]+" "[^"]*" \(at )([\d.\-]+) ([\d.\-]+)', lambda m: '%s%s %s' % (m.group(1), m.group(2), ('%g' % round(float(m.group(3)) + dy, 3))), block)

def inst(path, ref): return ('(instances\n      (project "hex_ctrl"\n        (path "/%s" (reference "%s") (unit 1))\n      )\n    )' % (path, ref))

def wire(a, b): return '(wire (pts (xy %g %g) (xy %g %g)) (stroke (width 0) (type default)) (uuid %s))' % (a[0], a[1], b[0], b[1], U())
def glabel(name, x, y, ang, shape='bidirectional', just='right'):
    return ('(global_label "%s" (shape %s) (at %g %g %g) (fields_autoplaced) (effects (font (size 1.27 1.27)) (justify %s)) (uuid %s)\n'
            '    (property "Intersheetrefs" "${INTERSHEET_REFS}" (at %g %g 0) (effects (font (size 1.27 1.27)) hide)))' % (name, shape, x, y, ang, just, U(), x, y))
def label(name, x, y, ang): return '(label "%s" (at %g %g %g) (effects (font (size 1.27 1.27)) (justify left bottom)) (uuid %s))' % (name, x, y, ang, U())
def text(t, x, y): return '(text "%s" (exclude_from_sim no) (at %g %g 0) (effects (font (size 1.27 1.27)) (justify left bottom)) (uuid %s))' % (t.replace('"', "'"), x, y, U())

def power_sym(lib, ref, val, x, y, ang, path):
    return ('(symbol (lib_id "%s") (at %g %g %g) (unit 1)\n    (in_bom no) (on_board yes) (dnp no)\n    (uuid %s)\n'
            '    (property "Reference" "%s" (at %g %g 0) (effects (font (size 1.27 1.27)) (justify left) hide))\n'
            '    (property "Value" "%s" (at %g %g 0) (effects (font (size 1.27 1.27)) (justify left)))\n'
            '    (property "Footprint" "" (at %g %g 0) (effects (font (size 1.27 1.27)) (justify left) hide))\n'
            '    (property "Datasheet" "~" (at %g %g 0) (effects (font (size 1.27 1.27)) (justify left) hide))\n'
            '    (pin "1" (uuid %s))\n    %s\n  )') % (lib, x, y, ang, U(), ref, x, y + 4, val, x, y + (3.5 if ang == 180 else 4) * (-1 if ang == 180 else 1), x, y + 5, x, y, U(), inst(path, ref))

def tp_sym(ref, val, x, y, fp, path, sym_uuid):
    return ('(symbol (lib_id "Connector:TestPoint") (at %g %g 0) (unit 1)\n    (in_bom yes) (on_board yes) (dnp no)\n    (uuid %s)\n'
            '    (property "Reference" "%s" (at %g %g 0) (effects (font (size 1.27 1.27)) (justify left)))\n'
            '    (property "Value" "%s" (at %g %g 0) (effects (font (size 1.27 1.27)) (justify left)))\n'
            '    (property "Footprint" "%s" (at %g %g 0) (effects (font (size 1.27 1.27)) (justify left) hide))\n'
            '    (property "Datasheet" "~" (at %g %g 0) (effects (font (size 1.27 1.27)) (justify left) hide))\n'
            '    (pin "1" (uuid %s))\n    %s\n  )') % (x, y, sym_uuid, ref, x + 1.5, y - 1.5, val, x + 1.5, y + 2.5, fp, x, y + 5, x, y, U(), inst(path, ref))

if __name__ == '__main__':
    FP_TH = 'hex_ctrl:Nano_Hole_Pad'
    imu = open('imu.kicad_sch').read()
    # ---------------- imu: remove J1201 block
    imu = remove(imu, lambda t: t.startswith('(symbol') and '"J1201"' in t, 1)
    imu = remove(imu, lambda t: t.startswith('(symbol') and ('"#PWR1201"' in t or '"#PWR1202"' in t), 2)
    for y in (77.47, 80.01, 82.55, 85.09):
        imu = remove(imu, lambda t, y=y: t.startswith('(wire') and ('(xy 34.29 %g) (xy 31.75 %g)' % (y, y)) in t, 1)
    imu = remove(imu, lambda t: t.startswith('(label "SDA"') and '(at 31.75 82.55' in t, 1)
    imu = remove(imu, lambda t: t.startswith('(label "SCL"') and '(at 31.75 85.09' in t, 1)
    new = []; sym_uuids = {}
    # SDA / SCL solder pads (Nano Qwiic-side through-holes), net labels SDA / SCL on this sheet
    for ref, val, name, x in (('TP1202', 'NANO_SCL_HOLE', 'SCL', 36.83), ('TP1203', 'NANO_SDA_HOLE', 'SDA', 49.53)):
        su = U(); sym_uuids[ref] = su
        new += [tp_sym(ref, val, x, 80.01, FP_TH, ROOT + '/' + SH_IMU, su), wire((x, 80.01), (x, 82.55)), label(name, x, 82.55, 270)]
    # debug access holes
    dbg = [('TP1204', 'DBG_SWDIO', 'DBG_SWDIO', 36.83), ('TP1205', 'DBG_SWCLK', 'DBG_SWCLK', 49.53), ('TP1206', 'DBG_NRST', 'DBG_NRST', 62.23),
           ('TP1207', 'DBG_GND', 'GND', 74.93), ('TP1208', 'DBG_3V3', '+3V3', 87.63)]
    pn = 1212
    for ref, val, net, x in dbg:
        su = U(); sym_uuids[ref] = su
        new += [tp_sym(ref, val, x, 100.33, FP_TH, ROOT + '/' + SH_IMU, su), wire((x, 100.33), (x, 102.87))]
        if net == 'GND': new.append(power_sym('power:GND', '#PWR%d' % pn, 'GND', x, 102.87, 0, ROOT + '/' + SH_IMU)); pn += 1
        elif net == '+3V3': new.append(power_sym('power:+3V3', '#PWR%d' % pn, '+3V3', x, 102.87, 180, ROOT + '/' + SH_IMU)); pn += 1
        else: new.append(label(net, x, 102.87, 270))
    new += [text('NANO INTERFACE (replaces the J1201 Qwiic header). 3V3 and GND come from Nano pins 17 and 4 on A1.', 30, 70),
            text('TP1202 / TP1203 (SCL / SDA): 2.0 mm pads with 1.0 mm holes under the two plated holes either side of the Nano Qwiic connector. Solder a short pin or wire through each.', 30, 73.5),
            text('TP1204-TP1208 (SWDIO, SWCLK, NRST, GND, 3V3): 1.0 mm holes over the Nano SWD test pads, which are on the Nano underside facing this board, under the Qwiic connector.', 30, 94),
            text('Hole positions come from Arduino ABX00142 CAD files, which are older than the Nano in hand: verify against the real board before ordering. The Nano pad marked 3V3 is on +5V in that CAD; measured as 3V3 on the real board.', 30, 97)]
    imu = append_items(imu, new)
    open('imu.kicad_sch', 'w').write(imu)
    srcimu = imu
    # ---------------- power: J1001 -> 1x04
    pw = open('power.kicad_sch').read()
    pw = ensure_libsym(pw, 'Connector_Generic:Conn_01x04', srcimu)
    a, b, t = find_item(pw, lambda t: t.startswith('(symbol (lib_id "Connector_Generic:Conn_01x02")') and '"J1001"' in t)
    t2 = t.replace('Connector_Generic:Conn_01x02', 'Connector_Generic:Conn_01x04', 1)
    t2 = t2.replace('(at 39.37 59.69 0) (unit 1)', '(at 39.37 62.23 0) (unit 1)', 1)
    t2 = prop_at_shift(t2, 2.54)
    t2 = t2.replace('PinHeader_1x02_P2.54mm_Vertical', 'PinHeader_1x04_P2.54mm_Vertical')
    t2 = t2.replace('"PWR_IN"', '"PWR_IN_UART"', 1)
    t2 = re.sub(r'(\(pin "2" \(uuid [0-9a-f\-]+\)\))', r'\1\n    (pin "3" (uuid %s))\n    (pin "4" (uuid %s))' % (U(), U()), t2, 1)
    pw = pw[:a] + t2 + pw[b:]
    pw = append_items(pw, [wire((34.29, 64.77), (31.75, 64.77)), glabel('NANO_TX', 31.75, 64.77, 180),
                            wire((34.29, 67.31), (31.75, 67.31)), glabel('NANO_RX', 31.75, 67.31, 180),
                            text('J1001: pin 1 VIN_RAW, pin 2 GND, pin 3 TX, pin 4 RX (Nano D1/TX, D0/RX). GND is shared with the power input; pins 2-4 form a GND / TX / RX UART header. Do not plug a UART cable onto pins 1-2.', 30, 52)])
    open('power.kicad_sch', 'w').write(pw)
    # ---------------- root: A1 pins
    rt = open('hex_ctrl.kicad_sch').read()
    rt = ensure_libsym(rt, 'power:+3V3', srcimu)
    for pos in ('88.9 88.9', '88.9 86.36', '104.14 76.2'):
        rt = remove(rt, lambda t, pos=pos: t.startswith('(no_connect') and ('(at %s)' % pos) in t, 1)
    rt = append_items(rt, [wire((88.9, 88.9), (83.82, 88.9)), glabel('NANO_TX', 83.82, 88.9, 180),
                            wire((88.9, 86.36), (83.82, 86.36)), glabel('NANO_RX', 83.82, 86.36, 180),
                            wire((104.14, 76.2), (104.14, 71.12)), power_sym('power:+3V3', '#PWR1301', '+3V3', 104.14, 71.12, 0, ROOT)])
    open('hex_ctrl.kicad_sch', 'w').write(rt)
    import json
    json.dump(sym_uuids, open('generator/nano_iface_uuids.json', 'w'), indent=1)
    print('done', sym_uuids)
