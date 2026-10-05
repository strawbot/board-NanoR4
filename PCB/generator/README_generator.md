# Generator scripts (how the schematic and board were produced)

## Current schematic generator (rev 2, 2026-10-04) -- use this one
- build2.py   : the design, laid out as wired, left-to-right flow (one function per sheet; the top sheet is a block
                diagram with real wires). `python3 build2.py` writes regen/out/*.kicad_sch.
- sch2.py     : sheet writer: stable UUIDs, bus/poly/sheet-pin support, collision-free reference/value text
- libs2.py    : symbol definitions, read from regen/baseline (the schematics as they were before the re-layout)
- netsig.py   : netlist signature + diff. `python3 netsig.py DIR out.json`. Proves a layout change is electrically
                identical: every net and every pin, plus part values and footprints.
- regen/uuid_registry.json : UUIDs of the sheets and parts of the original schematics. build2.py reuses them, so
                the PCB's footprint links (footprint path = sheet UUID / symbol UUID) keep working.
- regen/baseline, regen/baseline_sig.json : the rev 1 schematics and their netlist signature. libs2.py reads its symbol
                definitions from regen/baseline, so keep it.
- regen/current_sig.json : netlist signature of the installed rev 2 schematics (148 nets, 201 parts): the reference now.
- regen/expect_vin.py : example of a deliberate change: the netlist expected after the rev 2 edit (keep-alive removed, +10V -> VIN).
- vin_pcb.py : the matching board edit (parts removed, OE tied to GND, net renamed). VIN lives in hex_ctrl.kicad_sym as hex_ctrl:VIN.

Procedure after editing build2.py:
1. `python3 build2.py`
2. `python3 -c "import json,netsig; a=json.load(open('regen/current_sig.json')); print(netsig.diff(a, netsig.sig('regen/out')))"`
   must print `[]` (or only differences you meant to make; then refresh regen/current_sig.json). For a deliberate change,
   write the expected result first (see regen/expect_vin.py) and compare against that.
3. Close KiCad, copy regen/out/*.kicad_sch over the project's schematics, run ERC, and run PCB DRC with
   `--schematic-parity` (it must report no parity issues: that proves the footprint links survived).

Net names: cross-sheet signals use a *global* label on the top-sheet wire, so net names stay "KA", "G_A1" ... (a local
label on the top sheet would rename them "/KA"). Inside a sheet, signals arrive through hierarchical labels.

## One-off edit scripts (kept for the record)
- nano_iface_sch.py / nano_iface_pcb.py : J1201 -> SDA/SCL pads, SWD access holes, 4-pin J1001 (schematic and board)
- silk_designators.py : move J* / TP* designators to the front silkscreen
- netsig.py, silk_designators.py need KiCad's own python (pcbnew) where they touch the board.

## Rev 1 generator (legacy, do not run)
build.py, sch.py, libs.py, gen_board.py, testplace.py, check.py wrote the original schematics and board with KiCad 7
on Linux. Re-running build.py regenerates every UUID and breaks the PCB's footprint links.
