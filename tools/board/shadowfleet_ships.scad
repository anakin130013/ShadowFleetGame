// Shadow Fleet -- SHIP fleet markers (2026-11-20, test print). One silhouette per faction, after the game's own original ship
// art (tools/art/ships): the Rebels' fighter with wingtip engines, the Empire's wedge, the Pykes' boxy freighter, the Crimson
// Dawn's swept blade. ~12.8 mm long, <= 8.6 mm wide so three fit round a world (holes 12.5 mm out, 120 deg apart) pointing
// outward; a 3 mm stem lifts each ship like the old markers (it floats over the world token), and it spins so a ship can point where it's
// headed. Flat, printed FACE DOWN (peg up, no supports), the fleet number as big as each hull allows, engraved 0.6 mm.
// Every outline corner is filleted (FILLET). User 2026-11-20: "keep the numbers as big as you can", fillets; test against
// the round markers (shadowfleet_pieces.scad).
//
//   PIECE = "ship" (FACTION, NUMBER, face down for printing)  |  "sheet" (all four x 1-3)  |  "world" (three round a world)
//   tools/board/make_ships.sh exports all twelve into tools/board/out/ships/.

PIECE = "sheet"; FACTION = 0; NUMBER = 1;
H = 2.4; ENGRAVE = 0.6; PEG_D = 2.9; PEG_H = 2.2;
STEM_D = 4.0; STEM_H = 3.0;   // user 2026-11-20: stand up like the old markers (3 mm stem); no board change -- same peg, the stem rests round the hole
FILLET = 0.4;      // corner radius on every outline (concave and convex); thin parts keep >= 0.5 mm
FCOL = ["#FFC933", "#E6EEF5", "#9B5FE0", "#D0343E"];
$fn = 48;

module raw2d(f) {   // nose toward +x
  if (f == 0) {        // REBELS: a wider fuselage (room for the number), a short wing, an engine nacelle on each wingtip
    union() {
      polygon([[6.6, 0], [4.6, 2.0], [-5.4, 2.0], [-5.4, -2.0], [4.6, -2.0]]);
      translate([-4.0, -4.3]) square([2.6, 8.6]);
      for (s = [-1, 1]) translate([-6.4, s > 0 ? 3.2 : -4.3]) square([6.4, 1.1]);
    }
  } else if (f == 1) { // EMPIRE: the wedge
    polygon([[6.6, 0], [-4.8, 4.3], [-6.2, 4.3], [-6.2, -4.3], [-4.8, -4.3]]);
  } else if (f == 2) { // PYKES: a boxy freighter with a blunt nose
    union() {
      offset(r = 1.4) offset(delta = -1.4) translate([-5.6, -3.7]) square([9.2, 7.4]);
      translate([3.2, -1.8]) square([2.8, 3.6]);
      translate([-6.4, -2.6]) square([1.2, 5.2]);
    }
  } else {             // CRIMSON DAWN: the swept blade (a wider body than the mockup, for the number)
    polygon([[6.6, 0], [1.6, 2.0], [-0.4, 4.3], [-2.8, 4.3], [-2.6, 2.1], [-6.2, 1.9], [-6.2, -1.9], [-2.6, -2.1], [-2.8, -4.3], [-0.4, -4.3], [1.6, -2.0]]);
  }
}
FIL = [0.3, FILLET, FILLET, FILLET];   // the Rebel's engine gaps are narrow: a smaller radius keeps them open
module hull2d(f) { offset(r = FIL[f]) offset(r = -2 * FIL[f]) offset(r = FIL[f]) raw2d(f); }   // fillets, both ways

// the number: [centre x, size] per hull -- as big as each one allows (reads nose-up)
NUM = [[2.0, 4.4], [-2.6, 4.8], [-1.2, 5.6], [-2.4, 4.4]];
PEG_X = -0.6;      // the middle of the ship (balanced); raised 3 mm, a tail over the world token clears it by ~1.6 mm
module num2d(f, n) { translate([NUM[f][0], 0]) rotate(-90) text(str(n), size = NUM[f][1], font = "Liberation Sans:style=Bold", halign = "center", valign = "center"); }

module ship(f, n) {   // modelled face UP (PIECE "ship" flips it face down for printing)
  difference() { linear_extrude(H) hull2d(f); translate([0, 0, H - ENGRAVE]) linear_extrude(ENGRAVE + 0.01) num2d(f, n); }
  translate([PEG_X, 0, -STEM_H + 0.01]) cylinder(d = STEM_D, h = STEM_H);                          // stem
  translate([PEG_X, 0, -STEM_H - PEG_H + 0.02]) cylinder(d = PEG_D, h = PEG_H);                   // peg (unchanged: same fleet holes)
}
module painted(f, n) {
  color(FCOL[f]) ship(f, n);
  color("#14161C") translate([0, 0, H - ENGRAVE + 0.05]) linear_extrude(ENGRAVE) num2d(f, n);
}

if (PIECE == "ship") rotate([180, 0, 0]) ship(FACTION, NUMBER);
else if (PIECE == "sheet") for (f = [0 : 3]) for (n = [1 : 3]) translate([(n - 1) * 18, -f * 16, 0]) painted(f, n);
else if (PIECE == "world") {   // the fit: a world's slot, its token, three ships pointing out
  color("#2A3242") difference() { translate([-30, -30, -3]) cube([60, 60, 3]); translate([0, 0, -1.6]) cylinder(d = 17, h = 2); }
  color("#2A3242") translate([0, 0, -1.6]) cylinder(d = 16.4, h = 3.0);
  color(FCOL[1]) translate([0, 0, 1.39]) linear_extrude(0.2) scale(12.2 / 28) { difference() { circle(r = 12, $fn = 6); circle(r = 9.8, $fn = 6); } circle(r = 4.5); }
  for (k = [0 : 2]) let (a = 90 + k * 120, f2 = [1, 0, 3][k]) translate([12.5 * cos(a), 12.5 * sin(a), 0]) rotate(a) translate([-PEG_X, 0, STEM_H]) painted(f2, k + 1);
}
