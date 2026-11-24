#!/bin/sh
# Exports every small piece into tools/board/out/pieces/: a world-claim token per faction and fleet markers 1-3, all with
# the symbol / number ENGRAVED (print face down; paint the engraving or colour-paint it in the slicer). See shadowfleet_pieces.scad.
cd "$(dirname "$0")"
O="/c/Program Files/OpenSCAD/openscad.exe"
mkdir -p out/pieces && rm -f out/pieces/*.stl
f=0
for n in rebels empire pykes crimson_dawn; do "$O" -D 'PIECE="token"' -D FACTION=$f -o "out/pieces/token_${n}.stl" shadowfleet_pieces.scad 2>/dev/null; f=$((f + 1)); done
for k in 1 2 3; do "$O" -D 'PIECE="fleet"' -D NUMBER=$k -o "out/pieces/fleet_${k}.stl" shadowfleet_pieces.scad 2>/dev/null; done
"$O" -D 'PIECE="sheet"' --imgsize=1400,900 --camera=33,10,-14,60,0,20,150 --colorscheme="Tomorrow Night" -o out/pieces/preview.png shadowfleet_pieces.scad 2>/dev/null
ls out/pieces
