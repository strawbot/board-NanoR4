#!/usr/bin/env python3
"""build2.py : wired, flow-ordered rewrite of the schematic layout (2026-10-04).
Run:  python3 build2.py [sheet ...]    (default: every sheet written so far); output goes to regen/out/
Electrical content is unchanged: regen/out is compared with regen/baseline by netsig.py before anything is installed."""
import os, sys, shutil, json
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sch2 import *
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, 'regen', 'out'); BASE = os.path.join(HERE, 'regen', 'baseline')
os.makedirs(OUT, exist_ok=True)
FP = dict(R0402="Resistor_SMD:R_0402_1005Metric", C0402="Capacitor_SMD:C_0402_1005Metric", C1206="Capacitor_SMD:C_1206_3216Metric",
 SOT23="Package_TO_SOT_SMD:SOT-23", SOD323="Diode_SMD:D_SOD-323", SOD523="Diode_SMD:D_SOD-523", SMA="Diode_SMD:D_SMA",
 R2512="Resistor_SMD:R_2512_6332Metric", TSSOP16="Package_SO:TSSOP-16_4.4x5mm_P0.65mm", H5="Connector_PinHeader_2.54mm:PinHeader_1x05_P2.54mm_Vertical",
 H4="Connector_PinHeader_2.54mm:PinHeader_1x04_P2.54mm_Vertical", MH="MountingHole:MountingHole_2.2mm_M2", NANO="hex_ctrl:Arduino_Nano_R4_Castellated",
 POLY="Capacitor_Tantalum_SMD:CP_EIA-7343-43_Kemet-X", SJ="Jumper:SolderJumper-2_P1.3mm_Open_RoundedPad1.0x1.5mm", SOD123="Diode_SMD:D_SOD-123", TP="TestPoint:TestPoint_Pad_D1.0mm",
 LGA="Package_LGA:LGA-14_3x2.5mm_P0.5mm_LayoutBorder3x4y", HOLE="hex_ctrl:Nano_Hole_Pad")
ROOT = REG['root']
def mm_(n): return n * 1.27

def build_limb():
    s = Sheet('limb.kicad_sch', 'Limb: 4 channels + wire holes', 'A3', 100)
    R_, C_, D_, Q_ = s.lib('Device:R'), s.lib('Device:C'), s.lib('Device:D'), s.lib('hex_ctrl:Q_NMOS_GSD')
    J_ = s.lib('Connector_Generic:Conn_01x05')
    ROWS = [70, 122, 174, 226]; JX, JY = 330, 135
    jl = s.sym(J_, lambda b: f'J{b + 1}', 'WIRE_HOLES', FP['H5'], JX, JY, place='r')
    jpin = {1: s.pin(jl, '1'), 2: s.pin(jl, '2'), 3: s.pin(jl, '3'), 4: s.pin(jl, '4'), 5: s.pin(jl, '5')}
    # distinct vertical lanes for the four W wires: rows 1,2 descend (outer lane first), rows 3,4 ascend (inner lane first)
    lane = {0: 312, 1: 304, 2: 304, 3: 312}
    pinfor = {0: 1, 1: 2, 2: 4, 3: 5}
    for k, y0 in enumerate(ROWS):
        x0 = 30
        # --- gate chain: G in -> RG -> CAC -> gate node -> Q
        s.label('hier', f'G{k + 1}', (x0, y0), (-1, 0), 'input')
        rg = s.sym(R_, lambda b, k=k: f'R{b + 1 + k}', '220R', FP['R0402'], 50.8, y0, 90)
        cac = s.sym(C_, lambda b, k=k: f'C{b + 1 + k}', '22nF (100nF w/ IRLZ34N)', FP['C0402'], 76.2, y0, 90, place='b')
        s.wire((x0, y0), s.pin(rg, '2')) if s.pin(rg, '2')[0] < s.pin(rg, '1')[0] else s.wire((x0, y0), s.pin(rg, '1'))
        rgr = max(s.pin(rg, '1'), s.pin(rg, '2')); cl = min(s.pin(cac, '1'), s.pin(cac, '2')); cr = max(s.pin(cac, '1'), s.pin(cac, '2'))
        s.wire(rgr, cl)
        q = s.sym(Q_, lambda b, k=k: f'Q{b + 1 + k}', 'AO3400A', FP['SOT23'], 106.68, y0, place='r')
        gate = s.pin(q, '1'); gn = (96.52, y0)
        s.wire(cr, gn, gate); s.junction(gn)
        # --- gate guard: RPD to GND, BAT54 clamp (K at gate node, A to GND)
        rpd = s.sym(R_, lambda b, k=k: f'R{b + 11 + k}', '100k', FP['R0402'], gn[0], y0 + 11.43, place='r')
        dc = s.sym(D_, lambda b, k=k: f'D{b + 11 + k}', 'BAT54 (SOD-523)', FP['SOD523'], gn[0] - 7.62, y0 + 11.43, 270, place='l')
        top = s.pin(rpd, '1'); s.wire(gn, top); s.wire(top, s.pin(dc, '1')); s.junction(top)
        bot = s.pin(rpd, '2'); s.wire(s.pin(dc, '2'), bot); s.junction(bot)
        s.wire(bot, (bot[0], bot[1] + 2.54)); s.power('GND', (bot[0], bot[1] + 2.54), (0, 1))
        # --- source -> SRC (sheet pin), drain -> W node, flyback diode to Vin
        s.stub(q, '2', 'hier', 'SRC', L=5.08, shape='passive')
        dp = s.pin(q, '3'); yw = y0 - 12.7; xw = dp[0]
        xd = xw + 8.89
        s.wire(dp, (xw, yw), (xd, yw))
        fd = s.sym(D_, lambda b, k=k: f'D{b + 1 + k}', 'BAT165', FP['SOD323'], xd, yw - 3.81, 270, place='r')
        s.junction((xd, yw)); s.stub(fd, '1', 'power', 'VIN')
        # --- W wire to the connector
        pj = jpin[pinfor[k]]
        xl = lane[k]
        s.wire((xd, yw), (xl, yw), (xl, pj[1]), pj)
        if k in (0, 1): pass
        s.label('label', f'W{k + 1}', (xd + 25.4, yw), (1, 0))
    s.stub(jl, '3', 'power', 'VIN', L=2.54)
    s.text((20, 18), 'Limb: 4 low-side channels. Muscle wire k runs from the Vin hole to hole Wk. W1/W2 and W3/W4 are the antagonist pairs.')
    s.text((20, 22), 'Flyback diodes may be fitted DNP if the wire leads are short. Gate pull-downs keep every MOSFET off when the 74HC595 outputs are disabled.')
    s.text((20, 26), 'AO3400A pins: 1 G, 2 S, 3 D (SOT-23).')
    s.text((20, 30), 'Gate guard (hardware, no software involved): CAC AC-couples the drive, RPD bleeds it, the BAT54 clamp restores the low level each cycle.')
    s.text((20, 34), 'Normal pulses (up to ~250 us) pass with a few percent droop; a gate stuck high decays below threshold in ~3 ms (22 nF x 100 k). Use 100 nF with IRLZ34N.')
    return s

def build_group():
    s = Sheet('group.kicad_sch', 'Sense group (2 limbs)', 'A3', 700)
    R_, C_, TP_ = s.lib('Device:R'), s.lib('Device:C'), s.lib('Connector:TestPoint')
    ls = []
    for li, nm in enumerate(['limb_a', 'limb_b']):
        sh = s.sheet(nm, 'limb.kicad_sch', 80, 60 + li * 50, 35, 22, [(f'G{k + 1}', 'input', 'l', 2.54 * (k + 1)) for k in range(4)] + [('SRC', 'passive', 'r', 2.54)])
        ls.append(sh)
        for k in range(4):
            (px, py), d = s.spin(sh, f'G{k + 1}')
            s.wire((px, py), (px - 15.24, py)); s.label('hier', f'G{k + 1 + 4 * li}', (px - 15.24, py), (-1, 0), 'input')
    (ax, ay), _ = s.spin(ls[0], 'SRC'); (bx, by), _ = s.spin(ls[1], 'SRC')
    xs = 135.89
    s.wire((ax, ay), (xs, ay), (xs, by), (bx, by)); s.junction((xs, ay)) if False else None
    jy = (ay + by) / 2
    # SRC node: RSENSE to GND below, RC branch to the right
    node = (xs, g(jy)); s.junction(node)
    s.label('label', 'SRC', (xs - 8.89, ay), (1, 0))
    rsn = s.sym(R_, lambda b: f'R{b + 1}', '1R 1% 1W', FP['R2512'], xs + 12.7, node[1] + 15.24, place='r')
    s.wire(node, (xs + 12.7, node[1]), s.pin(rsn, '1')); 
    s.junction((xs + 12.7, node[1])) if False else None
    s.wire(s.pin(rsn, '2'), (xs + 12.7, s.pin(rsn, '2')[1] + 2.54)); s.power('GND', (xs + 12.7, s.pin(rsn, '2')[1] + 2.54), (0, 1))
    rfl = s.sym(R_, lambda b: f'R{b + 2}', '0R', FP['R0402'], xs + 38.1, node[1], 270, place='a')
    s.wire((xs + 12.7, node[1]), s.pin(rfl, '2')); s.junction((xs + 12.7, node[1]))
    ir = s.pin(rfl, '1'); xi = ir[0] + 10.16
    cfl = s.sym(C_, lambda b: f'C{b + 1}', '1nF (DNP)', FP['C0402'], xi, node[1] + 11.43, dnp=True, place='r')
    tpg = s.sym(TP_, lambda b: f'TP{b + 1}', 'ISNS', FP['TP'], xi, node[1] - 12.7, place='r', inbom=False)
    s.wire(ir, (xi, ir[1])); s.junction((xi, ir[1]))
    s.wire((xi, ir[1]), s.pin(cfl, '1')); s.wire((xi, ir[1]), s.pin(tpg, '1'))
    s.wire(s.pin(cfl, '2'), (xi, s.pin(cfl, '2')[1] + 2.54)); s.power('GND', (xi, s.pin(cfl, '2')[1] + 2.54), (0, 1))
    s.wire((xi, ir[1]), (xi + 15.24, ir[1])); s.label('hier', 'ISNS', (xi + 15.24, ir[1]), (1, 0), 'output')
    s.text((60, 28), 'Sense group: all 8 MOSFET sources (SRC) return through RSENSE. Route ISNS as a Kelvin trace from the top of RSENSE to the ADC.')
    s.text((60, 32), 'RC filter option: fit C (1 nF) and change R to 100R only if the ADC readings are noisy.')
    return s, ls


def build_power():
    s = Sheet('power.kicad_sch', 'Power input', 'A3', 1000)
    J_, DZ, QP, R_, C_, CP = s.lib('Connector_Generic:Conn_01x04'), s.lib('Device:D_Zener'), s.lib('hex_ctrl:Q_PMOS_GSD'), s.lib('Device:R'), s.lib('Device:C'), s.lib('Device:C_Polarized')
    RY = 99.06                                    # main rail y
    j = s.sym(J_, lambda b: f'J{b + 1}', 'PWR_IN_UART', FP['H4'], 50.8, 101.6, mx=True, place='a')
    p1, p2, p3, p4 = (s.pin(j, n) for n in '1234')
    # VIN_RAW -> node -> P-FET drain ; TVS to GND at the node
    q = s.sym(QP, lambda b: f'Q{b + 1}', 'AO3401A', FP['SOT23'], 127.0, 101.6, 90, place='a')
    qd, qs, qg = s.pin(q, '3'), s.pin(q, '2'), s.pin(q, '1')
    s.wire(p1, (qd[0], p1[1])); s.label('label', 'VIN_RAW', (73.66, p1[1]), (1, 0))
    tvs = s.sym(DZ, lambda b: f'D{b + 1}', 'SMAJ24A', FP['SMA'], 88.9, RY + 3.81, 270, place='r')
    s.wire((88.9, RY), s.pin(tvs, '1')); s.junction((88.9, RY)); s.stub(tvs, '2', 'power', 'GND')
    # gate: PG net, pull-down to GND
    rg = s.sym(R_, lambda b: f'R{b + 1}', '10k', FP['R0402'], 127.0, 114.3, place='r')
    dz = s.sym(DZ, lambda b: f'D{b + 2}', 'BZT52C10 (10 V)', FP['SOD123'], 132.08, 105.41, 270, place='r')       # gate clamp: limits Vgs to -10 V
    s.wire((132.08, RY), s.pin(dz, '1')); s.junction((132.08, RY))
    s.wire(s.pin(dz, '2'), (127.0, s.pin(dz, '2')[1])); s.junction((127.0, s.pin(dz, '2')[1]))
    s.wire(qg, s.pin(rg, '1')); s.label('label', 'PG', (127.0, 109.22), (-1, 0)); s.stub(rg, '2', 'power', 'GND')
    # Vin rail (after reverse protection): bulk caps, flag, divider
    RE = 228.6
    s.wire(qs, (RE, RY)); s.wire((RE, RY), (RE, RY - 2.54)); s.power('VIN', (RE, RY - 2.54), (0, -1))
    caps = [(CP, 'C_Polarized', '100uF 35V polymer', FP['POLY'], 152.4), (C_, 'C', '10uF 50V X7R', FP['C1206'], 170.18), (C_, 'C', '10uF 50V X7R', FP['C1206'], 187.96)]
    for i, (lib, _, val, fp, x) in enumerate(caps):
        c = s.sym(lib, lambda b, i=i: f'C{b + 1 + i}', val, fp, x, RY + 3.81, place='r' if i == 2 else 'l')
        s.junction((x, RY))
    gy = RY + 11.43
    s.wire((152.4, gy), (187.96, gy)); 
    for x in (152.4, 170.18, 187.96): s.wire((x, RY + 7.62), (x, gy))
    s.junction((170.18, gy)); s.wire((170.18, gy), (170.18, gy + 2.54)); s.power('GND', (170.18, gy + 2.54), (0, 1))
    s.wire((187.96, gy), (200.66, gy)); s.power('PWR_FLAG', (200.66, gy), (0, 1)); 
    s.power('PWR_FLAG', (200.66, RY), (0, -1)); s.junction((200.66, RY))
    r1 = s.sym(R_, lambda b: f'R{b + 2}', '100k', FP['R0402'], 215.9, 106.68, place='l')
    r2 = s.sym(R_, lambda b: f'R{b + 3}', '18k', FP['R0402'], 215.9, 118.11, place='l')
    cf = s.sym(C_, lambda b: f'C{b + 4}', '10nF', FP['C0402'], 228.6, 115.57, place='r')
    s.wire((215.9, RY), s.pin(r1, '1')); s.junction((215.9, RY))
    s.wire(s.pin(r1, '2'), s.pin(r2, '1')); s.junction((215.9, 111.76))
    s.wire((215.9, 111.76), (254.0, 111.76)); s.junction((228.6, 111.76))
    s.label('hier', 'VIN_S', (254.0, 111.76), (1, 0), 'output')
    s.stub(r2, '2', 'power', 'GND'); s.stub(cf, '2', 'power', 'GND')
    # UART / ground pins of J1001
    s.wire(p2, (60.96, p2[1])); s.power('GND', (60.96, p2[1]), (1, 0))
    s.wire(p3, (66.04, p3[1])); s.label('hier', 'NANO_TX', (66.04, p3[1]), (1, 0), 'bidirectional')
    s.wire(p4, (66.04, p4[1])); s.label('hier', 'NANO_RX', (66.04, p4[1]), (1, 0), 'bidirectional')
    s.text((30, 30), 'Reverse protection: AO3401A P-FET (body diode anode at input). The gate is pulled to GND, so Vgs would be -Vin; D1002 (10 V zener, gate to source) holds it at -10 V for any Vin (rating +/-12 V).')
    s.text((30, 34), 'Bulk capacitance: size from breadboard rail-sag measurements. VIN_S = Vin x 18k/118k = 3.2 V at Vin = 21 V, to ADC A3 (scale 0.1525).')
    s.text((30, 42), 'Supply ratings (Vin = 21 V now): D1001 SMAJ24A (24 V standoff, clamps near 39 V - above the 30 V Vds of AO3401A / AO3400A, so it only absorbs moderate surges); C1001 35 V, C1002/C1003 50 V X7R. Check the Nano VIN limit against Vin.')
    s.text((30, 38), 'J1001: pin 1 VIN_RAW, pin 2 GND, pin 3 TX, pin 4 RX (Nano D1/TX, D0/RX). GND is shared with the power input; pins 2-4 form a GND / TX / RX UART header. Do not plug a UART cable onto pins 1-2.')
    return s

def build_logic():
    s = Sheet('logic.kicad_sch', 'Gate drive shift registers', 'A3', 1100)
    R_, C_, D_, QN, JPs, IC = s.lib('Device:R'), s.lib('Device:C'), s.lib('Device:D'), s.lib('hex_ctrl:Q_NMOS_GSD'), s.lib('hex_ctrl:SolderJumper_2_Open'), s.lib('74xx:74HC595')
    XS, XR = 135.89, 128.27                        # trunks: SCK, RCLK
    IX = 170.18; IYS = [80.01, 144.78, 209.55]
    chain_nc = None
    pinsL = {}
    for i, g_ in enumerate('ABC'):
        Y = IYS[i]
        u = s.sym(IC, lambda b, i=i: f'U{b + 1 + i}', '74HC595', FP['TSSOP16'], IX, Y, fpos=dict(ref=(IX - 2.54, Y - 22.0, 'right'), val=(IX - 2.54, Y - 19.5, 'right')))
        # supply: VCC -> +5V, decoupling cap beside it; GND
        vcc = s.pin(u, '16'); nodey = Y - 27.94
        s.wire(vcc, (IX, nodey)); s.junction((IX, nodey)); s.wire((IX, nodey), (IX, nodey - 2.54)); s.power('+5V', (IX, nodey - 2.54), (0, -1))
        cx = IX + 12.7
        c = s.sym(C_, lambda b, i=i: f'C{b + 1 + i}', '100nF', FP['C0402'], cx, nodey + 3.81, place='r')
        s.wire((IX, nodey), (cx, nodey)); s.stub(c, '2', 'power', 'GND')
        s.stub(u, '8', 'power', 'GND')
        # SRCLR tied high (stub left)
        s.stub(u, '10', 'power', '+5V', L=5.08)
        # outputs -> hier labels
        for k, pn in enumerate(['15', '1', '2', '3', '4', '5', '6', '7']):
            p = s.pin(u, pn); e = (IX + 17.78, p[1]); s.wire(p, e); s.label('hier', f'G_{g_}{k + 1}', e, (1, 0), 'output')
        pinsL[g_] = dict(ser=s.pin(u, '14'), sck=s.pin(u, '11'), rclk=s.pin(u, '12'), oe=s.pin(u, '13'), qh=s.pin(u, '9'))
    # --- trunks and stubs
    sck_y, rclk_y = zip(*[(pinsL[g]['sck'][1], pinsL[g]['rclk'][1]) for g in 'ABC'])
    for g_ in 'ABC':
        p = pinsL[g_]
        s.wire(p['sck'], (XS, p['sck'][1])); s.junction((XS, p['sck'][1]))
        s.wire(p['rclk'], (XR, p['rclk'][1])); s.junction((XR, p['rclk'][1]))
        s.wire(p['oe'], (p['oe'][0] - 5.08, p['oe'][1])); s.power('GND', (p['oe'][0] - 5.08, p['oe'][1]), (-1, 0))   # outputs always enabled
    # SCK / RCLK inputs through the series resistors
    YSK, YRK = 48.26, 55.88
    s.label('hier', 'SCK_MCU', (30.48, YSK), (-1, 0), 'input')
    rs = s.sym(R_, lambda b: f'R{b + 3}', '0R (33R if ringing)', FP['R0402'], 63.5, YSK, 270, place='a')
    s.wire((30.48, YSK), s.pin(rs, '2')); s.wire(s.pin(rs, '1'), (XS, YSK))
    s.wire((XS, YSK), (XS, sck_y[2])); s.label('label', 'SCK', (XS, 65.0), (1, 0))
    s.label('hier', 'RCLK_MCU', (30.48, YRK), (-1, 0), 'input')
    rr = s.sym(R_, lambda b: f'R{b + 4}', '0R (33R if ringing)', FP['R0402'], 63.5, YRK, 270, place='b')
    s.wire((30.48, YRK), s.pin(rr, '2')); s.wire(s.pin(rr, '1'), (XR, YRK))
    s.wire((XR, YRK), (XR, rclk_y[2])); s.label('label', 'RCLK', (XR, 67.0), (-1, 0))
    # SER chain
    s.label('hier', 'SER', (30.48, pinsL['A']['ser'][1]), (-1, 0), 'input')
    s.wire((30.48, pinsL['A']['ser'][1]), pinsL['A']['ser'])
    for a_, b_, nm in (('A', 'B', 'SR1'), ('B', 'C', 'SR2')):
        q = pinsL[a_]['qh']; d = pinsL[b_]['ser']; ymid = (q[1] + d[1]) / 2
        xv = IX + 20.32; xl = 147.32
        s.wire(q, (xv, q[1]), (xv, ymid), (xl, ymid), (xl, d[1]), d)
        s.label('label', nm, (xv, ymid - 8.0), (1, 0)) if False else None
        s.label('label', nm, (xl + 10.0, ymid), (1, 0))
    s.nc(pinsL['C']['qh'])
    s.text((230, 30), 'Chain: SER -> U_A -> U_B -> U_C. QA..QH of U_A drive G_A1..G_A8 (limbs L1, L2), U_B -> L3, L4, U_C -> L5, L6.')
    s.text((230, 36), 'OE is tied to GND: outputs are always enabled. The gate guard on every channel (CAC / RPD / BAT54) replaces the old keep-alive output-enable circuit.')
    return s

def build_imu():
    s = Sheet('imu.kicad_sch', 'IMU', 'A3', 1200)
    R_, C_, TP, IC, J_ = s.lib('Device:R'), s.lib('Device:C'), s.lib('Connector:TestPoint'), s.lib('hex_ctrl:ICM-42688-P'), None
    u = s.sym(IC, lambda b: f'U{b + 1}', 'ICM-42688-P', FP['LGA'], 190.5, 114.3, place='b')
    pin = lambda n: s.pin(u, n)
    # supply: +3V3 -> RC filter -> VDD_IMU rail -> decoupling and the two supply pins
    RYV = 76.2
    s.power('+3V3', (50.8, RYV), (0, -1))
    rf = s.sym(R_, lambda b: f'R{b + 1}', '0R (10R for RC filter)', FP['R0402'], 76.2, RYV, 270, place='a')
    s.wire((50.8, RYV), s.pin(rf, '2')); s.wire(s.pin(rf, '1'), (193.04, RYV))
    s.label('label', 'VDD_IMU', (93.98, RYV), (1, 0))
    for i, (val, x) in enumerate([('1uF', 106.68), ('100nF', 119.38), ('100nF', 132.08)]):
        c = s.sym(C_, lambda b, i=i: f'C{b + 1 + i}', val, FP['C0402'], x, RYV + 3.81, place='r')
        s.junction((x, RYV)); s.stub(c, '2', 'power', 'GND')
    v8, v5 = pin('8'), pin('5')
    s.wire((v8[0], RYV), v8); s.wire((v5[0], RYV), v5); s.junction((v8[0], RYV))
    s.power('PWR_FLAG', (152.4, RYV), (0, -1)); s.junction((152.4, RYV))
    # I2C: pads -> bus lines -> IMU ; pull-ups (DNP) from +3V3
    tps = {}
    scl, sda = pin('13'), pin('14')
    for nm, y, name, x, ref in (('SCL', scl[1], 'NANO_SCL_HOLE', 63.5, 'TP1202'), ('SDA', sda[1], 'NANO_SDA_HOLE', 63.5, 'TP1203')):
        pass
    t1 = s.sym(TP, lambda b: 'TP1202', 'NANO_SCL_HOLE', FP['HOLE'], 63.5, scl[1], 270, place='b', inbom=False)
    t2 = s.sym(TP, lambda b: 'TP1203', 'NANO_SDA_HOLE', FP['HOLE'], 63.5, sda[1] + 15.24, 270, place='b', inbom=False)
    s.wire(s.pin(t1, '1'), scl); s.label('label', 'SCL', (101.6, scl[1]), (1, 0))
    s.wire(s.pin(t2, '1'), (s.pin(t2, '1')[0] + 12.7, s.pin(t2, '1')[1]), (s.pin(t2, '1')[0] + 12.7, sda[1]), sda) if False else None
    p2 = s.pin(t2, '1'); xb = 76.2
    s.wire(p2, (xb, p2[1]), (xb, sda[1]), sda); s.label('label', 'SDA', (xb + 25.4, sda[1]), (1, 0))
    rsc = s.sym(R_, lambda b: f'R{b + 3}', '4k7', FP['R0402'], 152.4, scl[1] - 12.7, dnp=True, place='r')
    s.wire(s.pin(rsc, '2'), (152.4, scl[1])); s.junction((152.4, scl[1])); s.stub(rsc, '1', 'power', '+3V3')
    rsd = s.sym(R_, lambda b: f'R{b + 2}', '4k7', FP['R0402'], 139.7, scl[1] - 12.7 + 0.0, dnp=True, place='l')
    s.wire(s.pin(rsd, '2'), (139.7, scl[1]), (139.7, sda[1])) if False else None
    s.wire(s.pin(rsd, '2'), (139.7, sda[1])); s.junction((139.7, sda[1])); s.stub(rsd, '1', 'power', '+3V3')
    # fixed straps: AP_AD0 low, AP_CS high
    s.wire(pin('1'), (pin('1')[0] - 5.08, pin('1')[1])); s.power('GND', (pin('1')[0] - 5.08, pin('1')[1]), (-1, 0))
    s.stub(u, '12', 'label', 'VDD_IMU', L=15.24)
    s.stub(u, '6', 'power', 'GND')
    # INT1 (3.3 V) -> test point and a one-MOSFET level shifter to the Nano's 5 V logic (D2).
    # Q1201 gate at 3V3, source on the IMU side, drain on the Nano side, 10k pull-up on each side (works for push-pull or open-drain).
    i1 = pin('4'); yl = i1[1]
    s.label('label', 'IMU_INT', (205.74, yl), (1, 0))
    xtp, xra, xq, xrb = 220.98, 228.6, 243.84, 256.54
    s.wire(i1, (xq - 5.08, yl))
    tpi = s.sym(TP, lambda b: f'TP{b + 1}', 'IMU_INT', FP['TP'], xtp, yl - 2.54, place='r', inbom=False)
    s.wire(s.pin(tpi, '1'), (xtp, yl)); s.junction((xtp, yl))
    ra = s.sym(R_, lambda b: f'R{b + 4}', '10k', FP['R0402'], xra, yl - 7.62, place='r')
    s.wire(s.pin(ra, '2'), (xra, yl)); s.junction((xra, yl)); s.stub(ra, '1', 'power', '+3V3')
    qi = s.sym(s.lib('hex_ctrl:Q_NMOS_GSD'), lambda b: f'Q{b + 1}', '2N7002', FP['SOT23'], xq, yl - 2.54, 270, place='b')
    s.stub(qi, '1', 'power', '+3V3')
    sd = s.pin(qi, '3')
    rb = s.sym(R_, lambda b: f'R{b + 5}', '10k', FP['R0402'], xrb, yl - 7.62, place='r')
    s.wire(sd, (xrb + 12.7, yl)); s.wire(s.pin(rb, '2'), (xrb, yl)); s.junction((xrb, yl)); s.stub(rb, '1', 'power', '+5V')
    s.label('hier', 'IMU_INT_5V', (xrb + 12.7, yl), (1, 0), 'output')
    s.nc(pin('9'))
    for n in ('2', '3', '7', '10', '11'): s.nc(pin(n))
    # debug / Nano access holes block
    s.poly([(30.48, 168.91), (152.4, 168.91), (152.4, 218.44), (30.48, 218.44), (30.48, 168.91)], dash=True)
    s.text((33, 173), 'Nano SWD test-pad access holes (pads are on the Nano underside)', 1.27)
    dbg = [('TP1204', 'DBG_SWDIO', None, 38.1), ('TP1205', 'DBG_SWCLK', None, 55.88), ('TP1206', 'DBG_NRST', None, 73.66), ('TP1207', 'DBG_GND', 'GND', 91.44), ('TP1208', 'DBG_3V3', '+3V3', 109.22)]
    for ref, val, net, x in dbg:
        t = s.sym(TP, lambda b, r=ref: r, val, FP['HOLE'], x, 190.5, place='b', inbom=False)
        p = s.pin(t, '1'); e = (p[0], p[1] + 2.54); s.wire(p, e)
        if net == 'GND': s.power('GND', e, (0, 1))
        elif net == '+3V3': s.power('+3V3', e, (0, 1))
        else: s.nc(e)
    s.text((20, 22), 'ICM-42688-P in I2C mode: AP_CS tied high, AP_AD0 low -> address 0x68. Powered from the Nano 3.3 V pin; pull-ups fitted only if the Nano is insufficient.')
    s.text((20, 26), 'CHECK BEFORE LAYOUT: RESV pins (2, 3, 7, 10, 11) and INT2/FSYNC (9) are left unconnected here; confirm against the datasheet. Confirm the LGA footprint pad order.')
    s.text((20, 30), 'INT1 is 3.3 V; Q1201 (gate at 3V3, 10k pull-up each side) shifts it to the Nano 5 V logic on D2 (net IMU_INT_5V). Polarity and push-pull / open-drain are set in the IMU INT_CONFIG register; TP1201 is the 3.3 V side.')
    s.text((20, 245), 'NANO INTERFACE (replaces the J1201 Qwiic header). 3V3 and GND come from Nano pins 17 and 4 on A1.')
    s.text((20, 249), 'TP1202 / TP1203 (SCL / SDA): pads with 1.0 mm holes under the two plated holes either side of the Nano Qwiic connector. Solder a short pin or wire through each.')
    s.text((20, 253), 'TP1204-TP1208 (SWDIO, SWCLK, NRST, GND, 3V3): 1.0 mm holes over the Nano SWD test pads, which are on the Nano underside facing this board, under the Qwiic connector.')
    s.text((20, 257), 'Hole positions come from Arduino ABX00142 CAD files, which are older than the Nano in hand: verify against the real board before ordering. Measured: the pad marked 3V3 is 3V3.')
    return s


def build_root(pages):
    s = Sheet('hex_ctrl.kicad_sch', 'Hex muscle-wire controller - top', 'A3', 0)
    NANO = s.lib('hex_ctrl:Arduino_Nano_R4')
    a1 = s.sym(NANO, 'A1', 'Arduino Nano R4', FP['NANO'], 170.18, 101.6, fpos=dict(ref=(185.42, 78.74, 'left'), val=(185.42, 81.28, 'left')))
    P = lambda n: s.pin(a1, n)
    for n in ('6', '7', '8', '9', '10', '11', '13', '15', '28', '3', '18', '23', '24', '25', '26'): s.nc(P(n))
    s.stub(a1, '30', 'power', 'VIN', L=2.54); s.stub(a1, '17', 'power', '+3V3', L=7.62); s.stub(a1, '27', 'power', '+5V', L=12.7)
    g4, g29 = P('4'), P('29'); gy = g4[1] + 2.54
    s.wire(g4, (g4[0], gy)); s.wire(g29, (g29[0], gy), (g4[0], gy)); s.junction((g4[0], gy)); s.power('GND', (g4[0], gy), (0, 1))
    mh = s.lib('Mechanical:MountingHole')
    for i in range(5): s.sym(mh, f'H{i + 1}', 'M2', FP['MH'], 120 + i * 16, 284, inbom=False)
    # ---- blocks
    pw = s.sheet('power', 'power.kicad_sch', 300, 40, 45, 17.78, [('NANO_TX', 'bidirectional', 'l', 5.08), ('NANO_RX', 'bidirectional', 'l', 7.62), ('VIN_S', 'output', 'l', 12.7)], page_of=lambda p: pages[(p, 'power')])
    im = s.sheet('imu', 'imu.kicad_sch', 45, 40, 50, 20.32, [('IMU_INT_5V', 'output', 'r', 5.08)], page_of=lambda p: pages[(p, 'imu')])
    LX, LY, LW = 38.1, 147.32, 60.96
    lpins = [('RCLK_MCU', 'input', 't', 10.16), ('SER', 'input', 't', 20.32), ('SCK_MCU', 'input', 't', 30.48)]
    for gi, g_ in enumerate('ABC'):
        for k in range(8): lpins.append((f'G_{g_}{k + 1}', 'output', 'r', 35.56 * gi + 2.54 * (k + 1)))
    lg = s.sheet('logic', 'logic.kicad_sch', LX, LY, LW, 99.06, lpins, page_of=lambda p: pages[(p, 'logic')])
    grp = {}
    for gi, g_ in enumerate('ABC'):
        grp[g_] = s.sheet(f'group_{g_}', 'group.kicad_sch', 185.42, LY + 35.56 * gi, 38.1, 25.4, [(f'G{k + 1}', 'input', 'l', 2.54 * (k + 1)) for k in range(8)] + [('ISNS', 'output', 'r', 2.54)], page_of=(lambda nm: (lambda p: pages[(p, nm)]))(f'group_{g_}'))
    # ---- gate lines: logic -> groups (straight, global label keeps the net name)
    for gi, g_ in enumerate('ABC'):
        for k in range(8):
            (lx, ly), _ = s.spin(lg, f'G_{g_}{k + 1}'); (gx, gy_), _ = s.spin(grp[g_], f'G{k + 1}')
            s.wire((lx, ly), (gx, gy_)); s.label('global', f'G_{g_}{k + 1}', (lx + 8.89 + (k % 2) * 22.86, ly), (1, 0), 'passive')
    # ---- control lines: logic top pins -> Nano left pins
    for nm, pn in (('RCLK_MCU', '12'), ('SER', '14'), ('SCK_MCU', '16')):
        (px, py), _ = s.spin(lg, nm); t = P(pn)
        s.wire((px, py), (px, t[1]), t); s.label('global', nm, (px + 18.0 + 12.0 * ('KA RCLK_MCU SER SCK_MCU'.split().index(nm)) * 0, t[1]), (1, 0), 'passive')
    # ---- IMU interrupt (level shifted on the imu sheet) -> Nano D2
    (ix, iy), _ = s.spin(im, 'IMU_INT_5V'); t = P('5')
    s.wire((ix, iy), (129.54, iy), (129.54, t[1]), t); s.label('global', 'IMU_INT_5V', (105.0, iy), (1, 0), 'passive')
    # ---- ISNS: groups -> Nano right pins (A0..A2), each on its own riser
    for g_, pn, xr in (('A', '19', 246.38), ('B', '20', 251.46), ('C', '21', 256.54)):
        (gx, gy_), _ = s.spin(grp[g_], 'ISNS'); t = P(pn)
        s.wire((gx, gy_), (xr, gy_), (xr, t[1]), t); s.label('global', f'ISNS_{g_}', (195.58 + 'ABC'.index(g_) * 15.24, t[1]), (1, 0), 'passive')
    # ---- power block: VIN_S -> A3 ; UART -> D1/D0
    (vx, vy), _ = s.spin(pw, 'VIN_S'); t = P('22')
    s.wire((vx, vy), (205.74, vy), (205.74, t[1]), t); s.label('global', 'VIN_S', (265.0, vy), (1, 0), 'passive')
    (tx_, ty), _ = s.spin(pw, 'NANO_TX'); (rx_, ry), _ = s.spin(pw, 'NANO_RX')
    s.wire((tx_, ty), (146.05, ty), (146.05, P('1')[1]), P('1')); s.label('global', 'NANO_TX', (280.0, ty), (1, 0), 'passive')
    s.wire((rx_, ry), (139.7, ry), (139.7, P('2')[1]), P('2')); s.label('global', 'NANO_RX', (280.0, ry), (1, 0), 'passive')
    # ---- block descriptions
    s.text((300, 64), 'Reverse-protected Vin input (J1001), TVS, bulk caps,', 1.0); s.text((300, 67), 'VIN_S divider; pins 2-4 of J1001 = GND / TX / RX', 1.0)
    s.text((45, 70), 'ICM-42688-P on the Nano 3.3 V, SCL/SDA solder pads', 1.0)
    s.text((42, 190), '3 x 74HC595 gate-drive chain', 1.0); s.text((42, 193), '(OE tied low)', 1.0)
    for gi, g_ in enumerate('ABC'): s.text((185.42, 147.32 + 35.56 * gi + 30), 'RSENSE + limb_a + limb_b (8 channels)', 1.0)
    # ---- notes and the rail legend
    s.text((100, 40), 'Nano R4 pin choices: D9 (RCLK) must be a GPT timer output; D11/D13 hardware SPI. D6 (formerly KA) is now unused; D2 reads the IMU interrupt. Verify against the Nano R4 pinout.', 1.27)
    s.text((100, 44), 'IMU: 3.3 V and I2C come from the Nano through solder pads under its Qwiic holes (see imu sheet). The IMU interrupt is level-shifted to 5 V and wired to D2.', 1.27)
    s.text((45, 74), 'I2C / power reach the Nano through TP1202/TP1203 (solder to its Qwiic holes); SWD holes TP1204-1208.', 1.0)
    s.text((185, 142), 'Group A = limbs L1+L2, B = L3+L4, C = L5+L6. Each limb holds two antagonist pairs (W1/W2, W3/W4).', 1.27)
    s.text((38, 262), 'POWER RAILS (global power symbols, not drawn as wires):', 1.27)
    s.text((38, 266), 'VIN   power sheet (reverse-protected input, J1001; 21 V in test) -> limb flyback diodes and wire holes, Nano VIN pin', 1.27)
    s.text((38, 270), '+5V   Nano 5 V pin -> 74HC595s (logic sheet)        +3V3  Nano 3V3 pin (A1 pad 17) -> IMU sheet', 1.27)
    s.text((38, 274), 'GND   common to every sheet; Nano pins 4 and 29', 1.27)
    return s

def write_all(sel):
    pages = {}
    pages[(f'/{ROOT}', 'power')] = 2; pages[(f'/{ROOT}', 'logic')] = 3; pages[(f'/{ROOT}', 'imu')] = 4
    for gi, g_ in enumerate('ABC'): pages[(f'/{ROOT}', f'group_{g_}')] = 5 + 3 * gi
    root = build_root(pages); root.instances = [(f'/{ROOT}', 0, 1)]
    limb = build_limb(); group, LS = build_group(); power = build_power(); logic = build_logic(); imu = build_imu()
    group.instances = []; limb.instances = []
    pg = 5
    gs = {g_: REG['sheets'][f'hex_ctrl:group_{g_}'] for g_ in 'ABC'}
    for gi, g_ in enumerate('ABC'):
        gpth = f'/{ROOT}/{gs[g_]}'
        group.instances.append((gpth, 700 + 100 * gi, pg)); pg += 1
        for li, sh in enumerate(LS):
            pages[(gpth, sh['name'])] = pg
            limb.instances.append((f'{gpth}/{sh["uuid"]}', 100 * (gi * 2 + li + 1), pg)); pg += 1
    for sh in LS: sh['page_of'] = (lambda nm: (lambda p: pages[(p, nm)]))(sh['name'])
    power.instances = [(f'/{ROOT}/{REG["sheets"]["hex_ctrl:power"]}', 1000, 2)]
    logic.instances = [(f'/{ROOT}/{REG["sheets"]["hex_ctrl:logic"]}', 1100, 3)]
    imu.instances = [(f'/{ROOT}/{REG["sheets"]["hex_ctrl:imu"]}', 1200, 4)]
    res = {'hex_ctrl': root, 'limb': limb, 'group': group, 'power': power, 'logic': logic, 'imu': imu}
    for name in ('hex_ctrl', 'power', 'logic', 'imu', 'group', 'limb'):
        fn = name + '.kicad_sch'
        if name in res and name in sel: open(os.path.join(OUT, fn), 'w').write(res[name].render(is_root=(name == 'hex_ctrl')))
        else: shutil.copy(os.path.join(BASE, fn), os.path.join(OUT, fn))
    for fn in ('hex_ctrl.kicad_pro', 'sym-lib-table', 'hex_ctrl.kicad_sym', 'fp-lib-table'):
        if os.path.exists(os.path.join(BASE, fn)): shutil.copy(os.path.join(BASE, fn), os.path.join(OUT, fn))
    # the VIN power symbol lives in the project symbol library
    from libs2 import get as _get
    kp = os.path.join(OUT, 'hex_ctrl.kicad_sym'); ks = open(kp).read()
    if '(symbol "VIN"' not in ks:
        t = _get('hex_ctrl:VIN')[0].replace('(symbol "hex_ctrl:VIN"', '(symbol "VIN"', 1)
        k = ks.rstrip().rfind(')'); open(kp, 'w').write(ks[:k].rstrip() + '\n  ' + t.strip().replace('\n', '\n  ') + '\n)\n')
    for sh in (root, limb, group, power, logic, imu):
        for n in sh.notes: print('NOTE', n)

if __name__ == '__main__':
    sel = sys.argv[1:] or ['hex_ctrl', 'limb', 'group', 'power', 'logic', 'imu']
    write_all(sel); print('written:', sel)
