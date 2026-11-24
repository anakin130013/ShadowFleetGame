# Art sources

Original pixel art for Shadow Fleet, generated from code so it can be
re-rendered at any scale (and later converted into firmware data).

- `portraits.html` — the four faction commanders (24×28 grid, drawn with
  small shape helpers + auto outline). Open `portraits.html#rebel`
  (`#empire`, `#pyke`, `#crimson`) in a browser, or render headless:
  `msedge --headless=new --default-background-color=00000000 --window-size=144,168 --screenshot=face.png "file:///<path>/portraits.html#rebel"`
- `planet_render.html` — renders a planet with the exact phone battle-scene
  code (`bPlanetPixels`, `PLANET_PALETTES`, copied from `web_server.cpp`);
  `#0`..`#11` = planet index. Re-copy those two blocks if the palettes change.

All characters are original (Star Wars *feel*, no likenesses); names are
placeholders.

## Portraits v2 (2026-10-14) -- the art in the game

- `portraits_v2.html` -- the 12 commanders drawn from shape functions
  (tapered jaw, shadowed neck, sloped shoulders, hue-shifted 4-tone shading,
  selective outline). Preview: `#<name>:classicW` (names: ilsa marr kess stahl
  vell thane vosk oxa rhel kaan ymra brakk); hologram: `#<name>:classicW::holo`;
  hair override: `#<name>:classicW:<style>`.
- **Firmware export:** `#export` prints `commander_art.cpp`:
  `msedge --headless=new --dump-dom "file:///<path>/portraits_v2.html#export"`
  then save the `<pre>` text (HTML-unescaped) as
  `ShadowFleet_Prototype_02/commander_art.cpp`. Never hand-edit that file.
- Rule for new faces: hair/beard must differ from skin in BRIGHTNESS, not
  just hue, or they merge in the hologram.
- `portraits.html` is the old v1 set (kept for reference).
