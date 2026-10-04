# Generator scripts (how the schematic and board were produced)

- libs.py, sch.py : write KiCad 7-format schematic files; pull symbols from KiCad's symbol library folder
- build.py        : the design itself (all sheets, nets, values, footprints)
- gen_board.py    : builds hex_ctrl.kicad_pcb from the exported netlist, runs ../hex_place.py, moves refs to Fab
- testplace.py    : courtyard-overlap and board-outline check
- check.py        : netlist connectivity summary

They were run with KiCad 7's kicad-cli and pcbnew Python module on Linux. Paths at the top of
libs.py and gen_board.py point at /usr/share/kicad; change them for another install.
Note: KiCad 10's pcbnew Python bindings misbehaved in the scripting console here, so the board
generator may need adapting (or editing the .kicad_* text files directly) on that version.

IMPORTANT: re-running build.py regenerates all schematic UUIDs; after that, use
"Re-link footprints by reference" when updating the PCB, or regenerate the board too.
