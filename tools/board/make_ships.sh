#!/bin/sh
# Exports the twelve ship fleet markers (4 factions x fleets 1-3), face down for printing, into tools/board/out/ships/,
# plus preview renders. See shadowfleet_ships.scad.
cd "$(dirname "$0")"
O="/c/Program Files/OpenSCAD/openscad.exe"
mkdir -p out/ships && rm -f out/ships/*.stl
f=0
for n in rebels empire pykes crimson_dawn; do
  for k in 1 2 3; do "$O" -D 'PIECE="ship"' -D FACTION=$f -D NUMBER=$k -o "out/ships/ship_${n}_${k}.stl" shadowfleet_ships.scad 2>/dev/null; done
  f=$((f + 1))
done
"$O" -D 'PIECE="sheet"' --imgsize=1000,1100 --viewall --autocenter --projection=o --colorscheme="Tomorrow Night" --camera=0,0,0,0,0,0,0 -o out/ships/sheet_top.png shadowfleet_ships.scad 2>/dev/null
"$O" -D 'PIECE="sheet"' --imgsize=1000,900 --viewall --autocenter --colorscheme="Tomorrow Night" --camera=0,0,0,50,0,20,0 -o out/ships/sheet_angle.png shadowfleet_ships.scad 2>/dev/null
"$O" -D 'PIECE="world"' --imgsize=1000,1000 --camera=0,0,0,40,0,20,110 --colorscheme="Tomorrow Night" -o out/ships/world.png shadowfleet_ships.scad 2>/dev/null
ls out/ships/*.stl | wc -l
