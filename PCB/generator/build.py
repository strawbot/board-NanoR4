import os, json
from sch import *
OUT="/home/claude/kc/hex_ctrl"; os.makedirs(OUT,exist_ok=True)
FP=dict(R0402="Resistor_SMD:R_0402_1005Metric", C0402="Capacitor_SMD:C_0402_1005Metric", C0603="Capacitor_SMD:C_0603_1608Metric",
 C1206="Capacitor_SMD:C_1206_3216Metric", SOT23="Package_TO_SOT_SMD:SOT-23", SOD323="Diode_SMD:D_SOD-323", SOD523="Diode_SMD:D_SOD-523", SMA="Diode_SMD:D_SMA",
 R2512="Resistor_SMD:R_2512_6332Metric", TSSOP16="Package_SO:TSSOP-16_4.4x5mm_P0.65mm", H5="Connector_PinHeader_2.54mm:PinHeader_1x05_P2.54mm_Vertical",
 H4="Connector_PinHeader_2.54mm:PinHeader_1x04_P2.54mm_Vertical", H2="Connector_PinHeader_2.54mm:PinHeader_1x02_P2.54mm_Vertical",
 MH="MountingHole:MountingHole_2.2mm_M2", NANO="hex_ctrl:Arduino_Nano_R4_Castellated", POLY="Capacitor_Tantalum_SMD:CP_EIA-7343-31_Kemet-D",
 SJ="Jumper:SolderJumper-2_P1.3mm_Open_RoundedPad1.0x1.5mm", TP="TestPoint:TestPoint_Pad_D1.0mm", LGA="Package_LGA:LGA-14_3x2.5mm_P0.5mm_LayoutBorder3x4y")

# ---------- custom IMU symbol
def pin(t,x,y,a,name,num): return f'(pin {t} line (at {x} {y} {a}) (length 2.54) (name "{name}" {FONT}) (number "{num}" {FONT}))'
imu_pins=[("input",-12.7,5.08,0,"AP_SCL","13"),("bidirectional",-12.7,2.54,0,"AP_SDA","14"),("input",-12.7,0,0,"AP_AD0","1"),("input",-12.7,-2.54,0,"AP_CS","12"),
 ("output",12.7,5.08,180,"INT1","4"),("bidirectional",12.7,2.54,180,"INT2/FSYNC","9"),
 ("no_connect",12.7,-2.54,180,"RESV","2"),("no_connect",12.7,-5.08,180,"RESV","3"),("no_connect",12.7,-7.62,180,"RESV","7"),("no_connect",12.7,-10.16,180,"RESV","10"),("no_connect",-12.7,-10.16,0,"RESV","11"),
 ("power_in",-2.54,15.24,270,"VDD","8"),("power_in",2.54,15.24,270,"VDDIO","5"),("power_in",0,-15.24,90,"GND","6")]
IMU_TXT=f'''    (symbol "hex_ctrl:ICM-42688-P" (in_bom yes) (on_board yes)
      (property "Reference" "U" (at -10.16 13.97 0) (effects (font (size 1.27 1.27)) (justify left)))
      (property "Value" "ICM-42688-P" (at 3.81 13.97 0) (effects (font (size 1.27 1.27)) (justify left)))
      (property "Footprint" "{FP['LGA']}" (at 0 -20.32 0) (effects (font (size 1.27 1.27)) hide))
      (property "Datasheet" "" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
      (symbol "ICM-42688-P_0_1" (rectangle (start -10.16 12.7) (end 10.16 -12.7) (stroke (width 0.254) (type default)) (fill (type background))))
      (symbol "ICM-42688-P_1_1"
        {chr(10).join(pin(*p) for p in imu_pins)}
      )
    )'''
IMU_PINS={p[5]:dict(type=p[0],x=p[1],y=p[2],a=p[3],name=p[4]) for p in imu_pins}

# ---------- instance bookkeeping
ROOT=U()
root=Sheet("hex_ctrl.kicad_sch","Hex muscle-wire controller - top")
power=Sheet("power.kicad_sch","Power input"); logic=Sheet("logic.kicad_sch","Gate drive shift registers and keep-alive")
imu=Sheet("imu.kicad_sch","IMU"); group=Sheet("group.kicad_sch","Sense group (2 limbs)"); limb=Sheet("limb.kicad_sch","Limb: 4 channels + wire holes", paper="A4")
root.uuid=ROOT

# ---------- ROOT
nano=root.sym(root.lib("MCU_Module","Arduino_Nano_v3.x","Arduino_Nano_R4","hex_ctrl"),"A1","Arduino Nano R4",FP["NANO"],101.6,101.6)
conn={"9":"KA","12":"RCLK_MCU","14":"SER","16":"SCK_MCU","19":"ISNS_A","20":"ISNS_B","21":"ISNS_C","22":"VIN_S"}
for n in root.pins[nano["lib"]]:
    if n in conn: root.stub(nano,n,"global",conn[n],L=5.08)
    elif n in("4","29"): root.stub(nano,n,"power","GND")
    elif n=="27": root.stub(nano,n,"power","+5V")
    elif n=="30": root.stub(nano,n,"power","+10V")
    else: root.stub(nano,n,"nc")
root.text((60,50),"Nano R4 pin choices: D9 (RCLK) and D6 (KA) must be GPT timer outputs; D11/D13 hardware SPI. Verify against the Nano R4 pinout.")
root.text((60,54),"IMU uses the Nano's 3.3 V Qwiic bus (Wire1) through its through-holes, see IMU sheet. D2 left free for an IMU interrupt if level-shifted later.")
mh=root.lib("Mechanical","MountingHole")
for i in range(5): root.sym(mh,f"H{i+1}","M2",FP["MH"],240+i*15,40,inbom=False)
pages={}
def page_of(name): return lambda p: pages[(p,name)]
shp=root.sheet("power","power.kicad_sch",200,70,30,15,[],[],page_of("power"))
shl=root.sheet("logic","logic.kicad_sch",240,70,30,15,[],[],page_of("logic"))
shi=root.sheet("imu","imu.kicad_sch",280,70,30,15,[],[],page_of("imu"))
gs={}
for gi,g in enumerate("ABC"):
    s=root.sheet(f"group_{g}","group.kicad_sch",210+gi*65,130,30,25,[f"G{k}" for k in range(1,9)],["ISNS"],page_of(f"group_{g}"))
    gs[g]=s
    for k in range(1,9): root.sheet_pin_stub(s,f"G{k}","global",f"G_{g}{k}")
    root.sheet_pin_stub(s,"ISNS","global",f"ISNS_{g}")
root.text((200,190),"Group A = limbs L1+L2, B = L3+L4, C = L5+L6. Each limb holds two antagonist pairs (W1/W2, W3/W4).")

# ---------- POWER (base 1000)
j=power.sym(power.lib("Connector_Generic","Conn_01x02"),lambda b:f"J{b+1}","PWR_IN",FP["H2"],40,60)
power.stub(j,"1","label","VIN_RAW"); power.stub(j,"2","power","GND")
tvs=power.sym(power.lib("Device","D_Zener"),lambda b:f"D{b+1}","SMAJ12A",FP["SMA"],60,80,90)
power.stub(tvs,"1","label","VIN_RAW"); power.stub(tvs,"2","power","GND")
qp=power.sym(power.lib("Device","Q_PMOS_GSD",None,"hex_ctrl"),lambda b:f"Q{b+1}","AO3401A",FP["SOT23"],90,60,90)
power.stub(qp,"3","label","VIN_RAW"); power.stub(qp,"2","power","+10V")
power.stub(qp,"1","label","PG")
rg=power.sym(power.lib("Device","R"),lambda b:f"R{b+1}","10k",FP["R0402"],110,90)
power.stub(rg,"1","label","PG"); power.stub(rg,"2","power","GND")
for i,(lib,val,fp,dnp) in enumerate([("C_Polarized","100uF 16V polymer",FP["POLY"],False),("C","22uF 25V",FP["C1206"],False),("C","22uF 25V",FP["C1206"],False)]):
    c=power.sym(power.lib("Device",lib),lambda b,i=i:f"C{b+1+i}",val,fp,150+i*15,70,dnp=dnp)
    power.stub(c,"1","power","+10V"); power.stub(c,"2","power","GND")
r1=power.sym(power.lib("Device","R"),lambda b:f"R{b+2}","100k",FP["R0402"],210,65)
r2=power.sym(power.lib("Device","R"),lambda b:f"R{b+3}","47k",FP["R0402"],210,85)
cf=power.sym(power.lib("Device","C"),lambda b:f"C{b+4}","10nF",FP["C0402"],225,85)
power.stub(r1,"1","power","+10V"); power.stub(r1,"2","global","VIN_S"); power.stub(r2,"1","global","VIN_S"); power.stub(r2,"2","power","GND")
power.stub(cf,"1","global","VIN_S"); power.stub(cf,"2","power","GND")
for net,x in [("+10V",60),("GND",80)]:
    fl=power.power("PWR_FLAG",(x,120),(0,-1)); power.wire((x,120),(x,125)); power.power(net,(x,125),(0,1) if net=="GND" else (0,-1))
power.text((30,30),"Reverse protection: AO3401A P-FET (body diode anode at input). Vgs = -10 V at a 10 V supply, inside its +/-12 V rating; add a gate clamp zener if the supply can exceed 12 V.")
power.text((30,34),"Bulk capacitance: size from breadboard rail-sag measurements. VIN_S = 10 V x 47k/147k = 3.2 V to ADC A3.")

# ---------- LOGIC (base 1100)
sr={}
for i,g in enumerate("ABC"):
    u=logic.sym(logic.lib("74xx","74HC595"),lambda b,i=i:f"U{b+1+i}","74HC595",FP["TSSOP16"],60+i*80,90)
    sr[g]=u
    logic.stub(u,"16","power","+5V"); logic.stub(u,"8","power","GND"); logic.stub(u,"10","power","+5V")
    logic.stub(u,"13","label","OE_N"); logic.stub(u,"11","label","SCK"); logic.stub(u,"12","label","RCLK")
    logic.stub(u,"14","global" if g=="A" else "label",{"A":"SER","B":"SR1","C":"SR2"}[g])
    if g=="C": logic.stub(u,"9","nc")
    else: logic.stub(u,"9","label",{"A":"SR1","B":"SR2"}[g])
    for k,pn in enumerate(["15","1","2","3","4","5","6","7"]):
        logic.stub(u,pn,"global",f"G_{g}{k+1}",L=5.08)
    c=logic.sym(logic.lib("Device","C"),lambda b,i=i:f"C{b+1+i}","100nF",FP["C0402"],60+i*80+14,60)
    logic.stub(c,"1","power","+5V"); logic.stub(c,"2","power","GND")
rs=logic.sym(logic.lib("Device","R"),lambda b:f"R{b+3}","0R (33R if ringing)",FP["R0402"],60,150,90)
logic.stub(rs,"2","global","SCK_MCU"); logic.stub(rs,"1","label","SCK")
rr=logic.sym(logic.lib("Device","R"),lambda b:f"R{b+4}","0R (33R if ringing)",FP["R0402"],60,165,90)
logic.stub(rr,"2","global","RCLK_MCU"); logic.stub(rr,"1","label","RCLK")
rpu=logic.sym(logic.lib("Device","R"),lambda b:f"R{b+1}","10k",FP["R0402"],300,150)
logic.stub(rpu,"1","power","+5V"); logic.stub(rpu,"2","label","OE_N")
c1=logic.sym(logic.lib("Device","C"),lambda b:f"C{b+4}","100nF",FP["C0402"],150,190,90)
logic.stub(c1,"2","global","KA"); logic.stub(c1,"1","label","KX")
d1=logic.sym(logic.lib("Device","D"),lambda b:f"D{b+1}","BAT54",FP["SOD323"],175,200,270)
logic.stub(d1,"1","label","KX"); logic.stub(d1,"2","power","GND")
d2=logic.sym(logic.lib("Device","D"),lambda b:f"D{b+2}","BAT54",FP["SOD323"],195,185)
logic.stub(d2,"2","label","KX"); logic.stub(d2,"1","label","KY")
c2=logic.sym(logic.lib("Device","C"),lambda b:f"C{b+5}","100nF",FP["C0402"],215,200)
logic.stub(c2,"1","label","KY"); logic.stub(c2,"2","power","GND")
rb=logic.sym(logic.lib("Device","R"),lambda b:f"R{b+2}","10k",FP["R0402"],230,200)
logic.stub(rb,"1","label","KY"); logic.stub(rb,"2","power","GND")
qk=logic.sym(logic.lib("Device","Q_NMOS_GSD",None,"hex_ctrl"),lambda b:f"Q{b+1}","2N7002",FP["SOT23"],260,190)
logic.stub(qk,"1","label","KY"); logic.stub(qk,"2","power","GND"); logic.stub(qk,"3","label","OE_N")
jp=logic.sym(logic.lib("Jumper","SolderJumper_2_Open",None,"hex_ctrl"),lambda b:f"JP{b+1}","KA_BYPASS",FP["SJ"],300,185)
logic.stub(jp,"1","label","OE_N"); logic.stub(jp,"2","power","GND")
logic.text((140,172),"Keep-alive: KA square wave (about 10 kHz) charges KY; Q holds OE_N low (outputs on). KA stops -> OE_N pulled high -> all outputs off in about 1 ms.")
logic.text((140,180),"KA must be toggled by firmware (once per scheduler frame), NOT by a free-running timer: a timer keeps toggling through a hang or a debugger halt.")
logic.text((140,176),"JP bypasses the keep-alive for debugging only. Leave open in normal use.")
logic.text((40,30),"Chain: SER -> U_A -> U_B -> U_C. QA..QH of U_A drive G_A1..G_A8 (limbs L1, L2), U_B -> L3, L4, U_C -> L5, L6.")

# ---------- IMU (base 1200)
jq=imu.sym(imu.lib("Connector_Generic","Conn_01x04"),lambda b:f"J{b+1}","Nano Qwiic holes",FP["H4"],40,80)
imu.stub(jq,"1","power","GND"); imu.stub(jq,"2","power","+3V3"); imu.stub(jq,"3","label","SDA"); imu.stub(jq,"4","label","SCL")
icm=imu.customlib("hex_ctrl:ICM-42688-P",IMU_TXT,IMU_PINS)
u=imu.sym(icm,lambda b:f"U{b+1}","ICM-42688-P",FP["LGA"],150,90)
imu.stub(u,"13","label","SCL"); imu.stub(u,"14","label","SDA"); imu.stub(u,"1","power","GND"); imu.stub(u,"12","label","VDD_IMU")
imu.stub(u,"8","label","VDD_IMU"); imu.stub(u,"5","label","VDD_IMU"); imu.stub(u,"6","power","GND")
imu.stub(u,"4","label","IMU_INT"); imu.stub(u,"9","nc")
for n in ["2","3","7","10","11"]: imu.stub(u,n,"nc")
rf=imu.sym(imu.lib("Device","R"),lambda b:f"R{b+1}","0R (10R for RC filter)",FP["R0402"],80,50,90)
imu.stub(rf,"2","power","+3V3"); imu.stub(rf,"1","label","VDD_IMU")
for i,(val,fp) in enumerate([("1uF",FP["C0402"]),("100nF",FP["C0402"]),("100nF",FP["C0402"])]):
    c=imu.sym(imu.lib("Device","C"),lambda b,i=i:f"C{b+1+i}",val,fp,100+i*12,60)
    imu.stub(c,"1","label","VDD_IMU"); imu.stub(c,"2","power","GND")
for i,net in enumerate(["SDA","SCL"]):
    r=imu.sym(imu.lib("Device","R"),lambda b,i=i:f"R{b+2+i}","4k7",FP["R0402"],100+i*12,120,dnp=True)
    imu.stub(r,"1","power","+3V3"); imu.stub(r,"2","label",net)
tp=imu.sym(imu.lib("Connector","TestPoint"),lambda b:f"TP{b+1}","IMU_INT",FP["TP"],200,70)
imu.stub(tp,"1","label","IMU_INT")
imu.power("PWR_FLAG",(40,130),(0,-1)); imu.wire((40,130),(40,135)); imu.power("+3V3",(40,135),(0,1))
imu.power("PWR_FLAG",(60,130),(0,-1)); imu.wire((60,130),(60,135)); imu.label("label","VDD_IMU",(60,135),(0,1))
imu.text((30,25),"ICM-42688-P in I2C mode: AP_CS tied high, AP_AD0 low -> address 0x68. Powered from the Nano's Qwiic 3.3 V; pull-ups fitted only if the Nano's are insufficient.")
imu.text((30,29),"CHECK BEFORE LAYOUT: RESV pins (2, 3, 7, 10, 11) and INT2/FSYNC (9) are left unconnected here; confirm against the datasheet. Confirm the LGA footprint pad order.")
imu.text((30,33),"INT1 goes to a test point only: its 3.3 V level may not read reliably on the 5 V Nano. Poll the FIFO over I2C instead.")

# ---------- GROUP (bases 700/800/900)
LS=[]
for li,nm in enumerate(["limb_a","limb_b"]):
    s=group.sheet(nm,"limb.kicad_sch",80,50+li*50,35,20,["G1","G2","G3","G4"],["SRC"],None); LS.append(s)
    for k in range(1,5): group.sheet_pin_stub(s,f"G{k}","hier",f"G{k+4*li}",shape="input")
    group.sheet_pin_stub(s,"SRC","label","SRC")
rsn=group.sym(group.lib("Device","R"),lambda b:f"R{b+1}","1R 1% 1W",FP["R2512"],160,90)
group.stub(rsn,"1","label","SRC"); group.stub(rsn,"2","power","GND")
rfl=group.sym(group.lib("Device","R"),lambda b:f"R{b+2}","0R",FP["R0402"],185,80,90)
group.stub(rfl,"2","label","SRC"); group.stub(rfl,"1","hier","ISNS",shape="output")
cfl=group.sym(group.lib("Device","C"),lambda b:f"C{b+1}","1nF (DNP)",FP["C0402"],210,95,dnp=True)
group.stub(cfl,"1","hier","ISNS",shape="output"); group.stub(cfl,"2","power","GND")
tpg=group.sym(group.lib("Connector","TestPoint"),lambda b:f"TP{b+1}","ISNS",FP["TP"],230,80)
group.stub(tpg,"1","hier","ISNS",shape="output")
group.text((60,30),"Sense group: all 8 MOSFET sources (SRC) return through RSENSE. Route ISNS as a Kelvin trace from the top of RSENSE to the ADC.")
group.text((60,34),"RC filter option: fit C (1 nF) and change R to 100R only if the ADC readings are noisy.")

# ---------- LIMB (bases 100..600)
for k in range(4):
    x0=20+50*k; y0=95
    lh=(x0,y0)
    rgk=limb.sym(limb.lib("Device","R"),lambda b,k=k:f"R{b+1+k}","220R",FP["R0402"],x0+7.62,y0,90)
    limb.wire(lh,limb.pin(rgk,"2") if limb.pin(rgk,"2")[0]<limb.pin(rgk,"1")[0] else limb.pin(rgk,"1"))
    limb.label("hier",f"G{k+1}",lh,(-1,0),"input")
    cac=limb.sym(limb.lib("Device","C"),lambda b,k=k:f"C{b+1+k}","22nF (100nF w/ IRLZ34N)",FP["C0402"],x0+19.05,y0,90)
    cp=sorted([limb.pin(cac,"1"),limb.pin(cac,"2")])
    right=max(limb.pin(rgk,"1"),limb.pin(rgk,"2"))
    limb.wire(right,cp[0])
    q=limb.sym(limb.lib("Device","Q_NMOS_GSD",None,"hex_ctrl"),lambda b,k=k:f"Q{b+1+k}","AO3400A",FP["SOT23"],x0+27.94,y0)
    gpin=limb.pin(q,"1")
    limb.wire(cp[1],gpin)
    rpd=limb.sym(limb.lib("Device","R"),lambda b,k=k:f"R{b+11+k}","100k",FP["R0402"],gpin[0],gpin[1]+7.62)
    limb.wire(gpin,limb.pin(rpd,"1")); limb.junction(gpin)
    dc=limb.sym(limb.lib("Device","D"),lambda b,k=k:f"D{b+11+k}","BAT54 (SOD-523)",FP["SOD523"],gpin[0]-5.08,gpin[1]+7.62,270)
    kp=limb.pin(dc,"1"); limb.wire(kp,limb.pin(rpd,"1")); limb.junction(limb.pin(rpd,"1"))
    limb.stub(dc,"2","power","GND")
    limb.stub(rpd,"2","power","GND")
    limb.stub(q,"2","hier","SRC",L=5.08,shape="passive")
    dp=limb.pin(q,"3"); node=(dp[0],dp[1]-7.62)
    limb.wire(dp,node)
    d=limb.sym(limb.lib("Device","D"),lambda b,k=k:f"D{b+1+k}","BAT165",FP["SOD323"],dp[0]+7.62,node[1]-3.81-2.54,270)
    ap=limb.pin(d,"2"); limb.wire(ap,(ap[0],node[1]),node); limb.junction(node)
    limb.stub(d,"1","power","+10V")
    limb.label("label",f"W{k+1}",node,(-1,0))
jl=limb.sym(limb.lib("Connector_Generic","Conn_01x05"),lambda b:f"J{b+1}","WIRE_HOLES",FP["H5"],255,80)
for pn,net in [("1","W1"),("2","W2"),("4","W3"),("5","W4")]: limb.stub(jl,pn,"label",net,L=5.08)
limb.stub(jl,"3","power","+10V")
limb.text((20,30),"Limb: 4 low-side channels. Muscle wire k runs from the +10 V hole to hole Wk. W1/W2 and W3/W4 are the antagonist pairs.")
limb.text((20,34),"Flyback diodes may be fitted DNP if the wire leads are short. Gate pull-downs keep every MOSFET off when the 74HC595 outputs are disabled.")
limb.text((20,38),"AO3400A pins: 1 G, 2 S, 3 D (SOT-23).")
limb.text((20,42),"Gate guard (hardware, no software involved): CAC AC-couples the drive, RPD bleeds it, the BAT54 clamp restores the low level each cycle.")
limb.text((20,46),"Normal pulses (up to ~250 us) pass with a few percent droop; a gate stuck high decays below threshold in ~3 ms (22 nF x 100 k). Use 100 nF with IRLZ34N.")

# ---------- instance paths, pages
pages[(f"/{ROOT}","power")]=2; pages[(f"/{ROOT}","logic")]=3; pages[(f"/{ROOT}","imu")]=4
root.instances=[(f"/{ROOT}",0,1)]
power.instances=[(f"/{ROOT}/{shp['uuid']}",1000,2)]
logic.instances=[(f"/{ROOT}/{shl['uuid']}",1100,3)]
imu.instances=[(f"/{ROOT}/{shi['uuid']}",1200,4)]
group.instances=[]; limb.instances=[]
pg=5
for gi,g in enumerate("ABC"):
    gp=f"/{ROOT}/{gs[g]['uuid']}"
    pages[(f"/{ROOT}",f"group_{g}")]=pg; group.instances.append((gp,700+100*gi,pg)); pg+=1
    for li,s in enumerate(LS):
        limbno=gi*2+li+1
        pages[(gp,s["name"])]=pg
        limb.instances.append((f"{gp}/{s['uuid']}",100*limbno,pg)); pg+=1
for s in LS:
    s["page_of"]=(lambda nm: (lambda p: pages[(p,nm)]))(s["name"])
for sh in [root,power,logic,imu,group,limb]:
    open(f"{OUT}/{sh.fname}","w").write(sh.render(is_root=(sh is root)))
# project + symbol lib
open(f"{OUT}/hex_ctrl.kicad_pro","w").write(json.dumps({"meta":{"filename":"hex_ctrl.kicad_pro","version":1},"sheets":[],"boards":[]},indent=2))
import re as _re
_lib={}
for sh in [root,power,logic,imu,group,limb]:
    for k,v in sh.libs.items():
        if k.startswith("hex_ctrl:"): _lib[k]=v.replace('(symbol "'+k+'"','(symbol "'+k.split(":",1)[1]+'"',1)
open(f"{OUT}/hex_ctrl.kicad_sym","w").write("(kicad_symbol_lib (version 20220914) (generator hex_ctrl_gen)\n"+"\n".join(_lib.values())+"\n)\n")
open(f"{OUT}/sym-lib-table","w").write('(sym_lib_table\n  (lib (name "hex_ctrl")(type "KiCad")(uri "${KIPRJMOD}/hex_ctrl.kicad_sym")(options "")(descr "Project symbols"))\n)\n')
print("written")
