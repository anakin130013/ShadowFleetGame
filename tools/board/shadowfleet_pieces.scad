// Shadow Fleet -- the small printed pieces (2026-11-20, for the MakerWorld launch).
//
// WORLD-CLAIM TOKEN: sits in a world's 17 mm slot (peg into the 4.2 mm hole) to show who holds it. Its face carries the
// faction's symbol -- the game's own ORIGINAL insignia (sun / hex / diamond / crescent, same as the phone's insigniaSvg),
// never a real Star Wars emblem. FLEET MARKER: the A3 marker (7.5 mm cap, peg into a 3.2 mm fleet hole), bolder number.
//
// ENGRAVED, like the fleet markers already printed (user 2026-11-20: no inlays -- "too hard to print without 0.2mm nozzle").
// Both print FACE DOWN (crisp face on the bed, no supports; the peg points up); the face is mirrored so it reads right once
// flipped. The symbol / number is sunk 0.6 mm (3 layers at 0.2) and every line is at least ~1 mm wide for a 0.4 nozzle.
// Second colour: paint it by hand, or in Bambu Studio use colour painting on the engraved faces (AMS).
//
//   openscad -D 'PIECE="token"' -D FACTION=0 -o token_rebels.stl shadowfleet_pieces.scad
//   tools/board/make_pieces.sh exports the full set into tools/board/out/pieces/.

PIECE   = "token";    // "token" | "fleet" | "sheet" (a preview of everything)
FACTION = 0;          // 0 Rebels (sun), 1 Empire (hex), 2 Pykes (diamond), 3 Crimson Dawn (crescent)
NUMBER  = 1;          // fleet marker number

// fits (match tools/board/shadowfleet_board_b17.scad)
SLOT_D = 17; SLOT_DEPTH = 1.6; PEG_HOLE_D = 4.2; PEG_HOLE_DEPTH = 2.6; FLEET_HOLE_D = 3.2;
TOKEN_D = SLOT_D - 0.6;        // 16.4
TOKEN_H = SLOT_DEPTH + 1.4;    // 3.0: stands 1.4 mm proud of the map
ENGRAVE = 0.6;                 // engraving depth
SYM = 12.2;                    // symbol size on the token face
FLEET_CAP_D = 7.5; FLEET_CAP_H = 2.4;
$fn = 72;

// ---- the four insignia, drawn on the phone's 28-unit grid, centred (lines >= 2.2 units = ~0.96 mm) ----
module ray(a, b, w) { hull() { translate(a) circle(d = w, $fn = 16); translate(b) circle(d = w, $fn = 16); } }
module insignia(f) {
  translate([-14, -14]) {
    if (f == 0) {         // the sun: a disc and eight rays
      translate([14, 14]) circle(r = 5);
      for (k = [0 : 7]) let (a = k * 45) ray([14 + 8.4 * cos(a), 14 + 8.4 * sin(a)], [14 + 12 * cos(a), 14 + 12 * sin(a)], 2.2);
    } else if (f == 1) {  // the hex: a ring and a core
      translate([14, 14]) { difference() { circle(r = 12, $fn = 6); circle(r = 9.8, $fn = 6); } circle(r = 4.5); }
    } else if (f == 2) {  // the diamond: a ring and a core
      translate([14, 14]) { difference() { rotate(45) square(17, center = true); rotate(45) square(12.6, center = true); } circle(r = 4); }
    } else {              // the crescent (the phone's path: a r 11 disc minus a r 10 disc to its right)
      difference() { translate([14.42, 14]) circle(r = 11); translate([19, 14]) circle(r = 10); }
    }
  }
}
module face2d(f) { mirror([1, 0]) scale(SYM / 28) insignia(f); }   // mirrored: printed face down

module token(f) {
  difference() {
    union() {
      cylinder(d = TOKEN_D, h = TOKEN_H);
      translate([0, 0, TOKEN_H - 0.01]) cylinder(d = PEG_HOLE_D - 0.4, h = PEG_HOLE_DEPTH - 0.3, $fn = 24);
    }
    translate([0, 0, -0.01]) linear_extrude(ENGRAVE + 0.01) face2d(f);
  }
}

module num2d(n) { mirror([1, 0]) text(str(n), size = 5.4, font = "Liberation Sans:style=Bold", halign = "center", valign = "center"); }
module fleet(n) {
  difference() {
    union() {
      cylinder(d = FLEET_CAP_D, h = FLEET_CAP_H, $fn = 40);
      translate([0, 0, FLEET_CAP_H - 0.01]) cylinder(d = 3.6, h = 3, $fn = 24);                     // stem
      translate([0, 0, FLEET_CAP_H + 3 - 0.02]) cylinder(d = FLEET_HOLE_D - 0.3, h = 2.2, $fn = 16);  // peg
    }
    translate([0, 0, -0.01]) linear_extrude(ENGRAVE + 0.01) num2d(n);
  }
}

FCOL = ["#FFC933", "#E6EEF5", "#9B5FE0", "#D0343E"];
if (PIECE == "token") token(FACTION);
else if (PIECE == "fleet") fleet(NUMBER);
else if (PIECE == "sheet") {   // preview: face up, the engraving filled with paint
  for (f = [0 : 3]) translate([f * 22, 0, 0]) rotate([0, 180, 0]) {
    color("#1B2230") token(f);
    color(FCOL[f]) translate([0, 0, 0.05]) linear_extrude(ENGRAVE - 0.1) face2d(f);
  }
  for (n = [1 : 3]) translate([(n - 1) * 14 + 12, -22, 0]) rotate([0, 180, 0]) {
    color("#C8364A") fleet(n);
    color("#F2F2F2") translate([0, 0, 0.05]) linear_extrude(ENGRAVE - 0.1) num2d(n);
  }
}
