# hex_ctrl: KiCad starter project

Generated schematic for the octagonal 24-channel muscle-wire controller (Nano R4 carrier).
Written in KiCad 7 file format so any newer KiCad (including 10) opens it; KiCad will
offer to save in its own format on first save.

## Sheets
- hex_ctrl.kicad_sch  top: Nano R4 (A1), mounting holes H1-H5, sheet links
- power.kicad_sch     supply holes, TVS, reverse-protection P-FET, bulk C, VIN_S divider (refs 1000+)
- logic.kicad_sch     3 x 74HC595, keep-alive, /OE pull-up, bypass jumper (refs 1100+)
- imu.kicad_sch       ICM-42688-P on the Nano's 3.3 V I2C bus, SDA/SCL solder pads and SWD access holes (refs 1200+)
- group.kicad_sch     used 3 times (A, B, C): RSENSE + ISNS option, two limb sheets (refs 700/800/900+)
- limb.kicad_sch      used 6 times (L1..L6): 4 channels + 5 wire holes (refs 100..600+)

Group A = limbs L1 + L2, B = L3 + L4, C = L5 + L6.

## Schematic layout
Each sheet is drawn as a wired, left-to-right flow (protection chain on power, the SER chain and clock trunks on
logic, gate -> R -> C -> FET -> wire hole on each limb), and the top sheet is a block diagram: the Nano, the power,
logic, IMU and group blocks, with every signal (SER, SCK, RCLK, KA, the 24 gate lines, ISNS, VIN_S, UART) drawn as a
wire between blocks. The power rails are shown as a legend on that sheet. The layout comes from
generator/build2.py; generator/README_generator.md describes how it is regenerated and checked against the
original netlist (net for net, pin for pin).

## Design rules
hex_ctrl.kicad_dru (Board Setup > Custom Rules) adds: 0.125 mm pad clearance inside the
IMU footprint only; 0.4 mm min / 0.6 mm preferred width for +10V, GND, VIN_RAW, the wire
nets (W1..W4) and the SRC buses; 0.25 mm for the ISNS sense lines.

## Pre-placed board
hex_ctrl.kicad_pcb is placed and routed (72 mm octagon with the front 7 mm sliced off, 2 layers, 0.8 mm), with every
footprint linked to its schematic symbol and every pad on its net. Open it and press F8
to confirm it matches the schematic. oct_place.py regenerates the placement; hex_place.py
is the retired 63 mm hexagon version, kept for reference. The octagon exists because the
autorouter only routes at 0/45/90 degrees and the 60 degree limbs of the hexagon left 18
wire nets unrouted. The six limbs sit on the edges at 0/45/135/180/225/315 deg; the front
(USB/Nano) and rear edges carry no limb. The front strip holds nothing but the Nano's USB end, so
FRONT_CUT in oct_place.py (7 mm) trims it and the Nano overhangs the board by about 4 mm; its
silkscreen outline in the A1 footprint is left open at that end. 7 mm is the limit: at 8 mm the
limb 3 wire tracks come within 0.39 mm of the new edge (rule 0.5 mm). The cut saves roughly
260 mm2 of board, about 0.4 g.

## Getting to a board (manual route)
1. Open hex_ctrl.kicad_pro. Check the sheets (the text notes flag items to verify).
2. Run Inspect > Electrical Rules Checker. Expected: no errors; unconnected pins are
   marked with no-connect flags.
3. Open the PCB editor, set the board to 2 layers / 0.8 mm (Board Setup), then
   Tools > Update PCB from Schematic (F8).
4. Tools > Scripting Console:  exec(open(r"<path>/hex_place.py").read())
   Draws the hexagon and places every part in its zone (limbs on the edges,
   group parts at their vertices, power at 210 deg, keep-alive at 330 deg,
   IMU at the centre, Nano flipped to the bottom). Checked for courtyard overlaps.

## Board size
oct_place.py uses A = 72 mm across flats. The per-gate guard (CAC + BAT54 on every
channel) made each limb block deeper, and the shorter octagon edges leave less margin at
the vertices; 72 mm placed with the 15 listed courtyard nudges. Change A at the top of
oct_place.py and re-derive the NUDGE table if you resize.

## Designators on the board
Connector (J*) and test point (TP*) designators are on the front silkscreen so they can be read on the
assembled board (generator/silk_designators.py places each at the nearest free spot; rerun it after moving
parts). All other designators and all values stay on F.Fab, which is not part of the manufacturing outputs.
TP1202-TP1208 (the Nano holes) have function labels instead: SCL, SDA, CLK, DIO, RST, GND, 3V3.

## Nano interface (I2C pads, SWD access holes, UART)
J1201 (Qwiic placeholder) was removed. Instead:
- Power and ground come from the Nano castellation: A1 pad 17 (3V3) drives the +3V3 rail,
  pad 4 is GND.
- TP1202 (SCL) and TP1203 (SDA) are 1.6 mm pads with 1.0 mm plated holes
  (hex_ctrl:Nano_Hole_Pad) that sit under the two through-holes either side of the Nano's
  Qwiic connector. Solder a short pin or wire through each hole to the Nano.
- TP1204-TP1208 (SWDIO, SWCLK, NRST, GND, 3V3; silkscreen DIO, CLK, RST, GND, 3V3) are the same
  hole pad, placed over the Nano's SWD test pads. Those pads are on the Nano's underside, which
  faces this board, under the Qwiic connector. Use a pogo pin or solder a wire down the hole
  to reach a pad. SWDIO, SWCLK and NRST have no connection on this board (one-pad nets);
  GND goes to GND, 3V3 to +3V3.
- J1001 is now 1x04: pin 1 VIN_RAW, pin 2 GND, pin 3 TX (Nano D1), pin 4 RX (Nano D0). GND is
  shared with the power input, so pins 2-4 form a GND / TX / RX UART header. Do not plug a
  UART cable onto pins 1-2.

The hole positions come from Arduino's ABX00142 Altium CAD, which is older than the Nano in
hand (its test pad labelled 3V3 is on the +5V net there; measured as 3V3 on the real board).
Measure the real Nano and move TP1202-TP1208 if they differ: mapping and offsets are in
oct_place.py (nano_xy) and generator/nano_iface_pcb.py. The silkscreen labels are board text
from that script. Schematic edits are in generator/nano_iface_sch.py (run once; it edits the
.kicad_sch files in place so footprint links survive).

## Verify before layout
- Nano R4: D9 (RCLK) and D6 (KA) must be GPT timer outputs.
- Footprint A1 is hex_ctrl:Arduino_Nano_R4_Castellated (project library): 30 SMD pads
  for soldering the Nano flat on the bottom by its castellated edge. Pad size and offset
  are estimates; check against the real Nano. The Qwiic header placeholder J1201 is gone;
  see "Nano interface" above.
- ICM-42688-P: RESV pins and INT2/FSYNC left unconnected; confirm with the datasheet.
  Confirm the LGA-14 footprint pad order against the datasheet land pattern.
- DNP parts: C701/C801/C901 (ISNS filter), R1202/R1203 (I2C pull-ups), D1002 (gate zener).
