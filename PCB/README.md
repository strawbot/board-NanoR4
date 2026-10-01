# hex_ctrl: KiCad starter project

Generated schematic for the hexagonal 24-channel muscle-wire controller (Nano R4 carrier).
Written in KiCad 7 file format so any newer KiCad (including 10) opens it; KiCad will
offer to save in its own format on first save.

## Sheets
- hex_ctrl.kicad_sch  top: Nano R4 (A1), mounting holes H1-H5, sheet links
- power.kicad_sch     supply holes, TVS, reverse-protection P-FET, bulk C, VIN_S divider (refs 1000+)
- logic.kicad_sch     3 x 74HC595, keep-alive, /OE pull-up, bypass jumper (refs 1100+)
- imu.kicad_sch       ICM-42688-P on the Nano's 3.3 V Qwiic bus (refs 1200+)
- group.kicad_sch     used 3 times (A, B, C): RSENSE + ISNS option, two limb sheets (refs 700/800/900+)
- limb.kicad_sch      used 6 times (L1..L6): 4 channels + 5 wire holes (refs 100..600+)

Group A = limbs L1 + L2, B = L3 + L4, C = L5 + L6.

## Getting to a board
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
The placement script uses A = 63 mm across flats (was 60). The per-gate guard
(CAC + BAT54 on every channel) made each limb block deeper; 63 mm is the smallest
size that placed with zero courtyard overlaps. Change A at the top of hex_place.py.

## Verify before layout
- Nano R4: D9 (RCLK) and D6 (KA) must be GPT timer outputs.
- Footprint A1 is hex_ctrl:Arduino_Nano_R4_Castellated (project library): 30 SMD pads
  for soldering the Nano flat on the bottom by its castellated edge. Pad size and offset
  are estimates; check against Arduino's Nano R4 CAD files. J1201 (Qwiic holes) is a
  1x04 2.54 mm placeholder at a placeholder position.
- ICM-42688-P: RESV pins and INT2/FSYNC left unconnected; confirm with the datasheet.
  Confirm the LGA-14 footprint pad order against the datasheet land pattern.
- DNP parts: C701/C801/C901 (ISNS filter), R1202/R1203 (I2C pull-ups), D1002 (gate zener).
