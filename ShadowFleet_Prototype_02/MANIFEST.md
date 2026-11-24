# Shadow Fleet Prototype 02 — Version Manifest

**Firmware version:** v1.0.2 (2026-11-23)

This is the exact, internally-consistent file set for the current build. If
your local folder doesn't match this list exactly, pull the fresh zip rather
than hunting through chat history for individual files.

## How to verify

The version shows in three places, all should say `v3.1-dev.72`:
1. **Serial Monitor** on boot: `Shadow Fleet Prototype 02  v1.0.2  (2026-11-23)`
2. **AMOLED start/home screen**: subtitle reads `v1.0.2`
3. **Phone page footer**: `Firmware v1.0.2`

## Required files

| File | Lines | MD5 |
|---|---:|---|
| `ShadowFleet_Prototype_02.ino` | 2343 | `794f08a0288acd2235f803c28a38eba3` |
| `version.h` | 829 | `a727464ebf770633aba55a9782fa1432` |
| `game_state.h` | 388 | `6e8da9c56edf2fd7a622b7054397efe7` |
| `game_state.cpp` | 1214 | `96d753fcf6800b43269f46efe6b2cf65` |
| `web_server.h` | 3 | `0db5f766483f596f2b0442d456ccd017` |
| `web_server.cpp` | 1012 | `9375bc1e8cc4f18ce22be65d0a9f87a3` |
| `bsp_config.h` | 72 | `b6e990bd376b63e0a22f84503dc77dfe` |
| `board_bsp.h` (was `esp32_s3_touch_amoled_1.43c.h`, renamed v0.20.3) | — | (MD5s frozen at v0.19.0; git is the source of truth) |
| `board_bsp.cpp` (was `esp32_s3_touch_amoled_1.43c.cpp`) + `board_select.h` (new) | — | |
| `i2c_bsp.h` | 20 | `907be9a7e4c81ac58f99ebb0283396ec` |
| `i2c_bsp.cpp` | 80 | `b4ecf5dfb9252793b8a68ae440000ffc` |
| `src/externLib/esp_lcd_sh8601.h` | 115 | `294acffa2244d0865c7e0188884d0f32` |
| `src/externLib/esp_lcd_sh8601.c` | 343 | `d3b44f5eeaf259baa2c508ab20e0637e` |

**Total: 13 files.** `esp_lcd_sh8601.c` must stay `.c`, not `.cpp`.

## What's new in v0.15.0: PHASE A -- the foundational rework

This is the first of a 4-phase "polish and go from mechanics-skeleton to a
real playable game" pass, worked out with the player before any code was
written. This phase is the foundation everything else depends on.

### Everyone joins the same way now -- including the AMOLED

Old flow: AMOLED was hardcoded as slot 0/"HOST"; phones auto-joined the
first free slot. New flow, identical on both platforms: **type your name ->
pick a seat (Player 1-4) -> pick a faction -> Lobby.** The AMOLED is no
longer special-cased -- it's just another player who happens to also run
the WiFi network.

Faction picking is real, not cosmetic: claiming Rebel Alliance, Galactic
Empire, Pyke Syndicate, or Crimson Dawn sets genuine starting reputation --
deep **Allied** with your own faction, deep **Hostile** with its rival
(Rebel<->Empire, Pyke<->Crimson Dawn), Neutral with the other two. Each
faction is claimable by only one player (max 4 players = all 4 used).

### Reputation is always visible now

The old "reveal a planet's Affiliation to unlock your reputation with that
faction" gating is gone entirely, on both platforms. Since faction choice
already gives you meaningful starting reputation with every faction from
turn one, hiding it no longer made sense.

### Planet selection during setup

Any player can toggle which of the 12 planets are actually in play, via a
new screen reachable from the Lobby on both platforms. Fully flexible --
any number, any combination. The in-game planet lists on both platforms now
filter to only the planets actually included.

### Fleet rework

Only Fleet 1 starts populated (3 ships). Fleets 2 and 3 start completely
empty. The existing "invest 2 Credits per ship" mechanic now does double
duty: the first time you invest in an empty fleet, that *is* founding it --
no separate system needed. Founding a new fleet requires owning at least
one planet, since the new fleet gets placed there.

## A real bug found and fixed along the way

`planetOwner[]` was never properly reset for all 12 planets after the
v0.14.0 galaxy expansion -- only the first 2 elements (a leftover array
initializer from when `NUM_PLANETS` was 2). Planets 3 through 12 were
defaulting to **"owned by slot 0"** from the moment any game started, on
every single game played since that expansion, undetected until this pass.
Fixed at the source (`gameStateInit()`), with the incomplete initializer
pattern cleaned up so it can't silently happen again.

## New territory for this codebase (v0.15.0, later simplified away)

v0.15.0 briefly gave the AMOLED real on-screen text input (LVGL's keyboard
+ textarea widgets) for name entry -- the first time this codebase had
used either widget. v0.15.1 removed the whole typed-name step (see above),
so this is no longer part of the build, but noted here since it was the
single highest-risk, least-testable piece of the original Phase A push.

## REQUIRED SETUP STEP FOR v0.19.0+ -- read before flashing

This version needs a matching `lv_conf.h` change, provided as a separate
file alongside this zip (not one of the 13 project files above, since it
lives in your local LVGL library installation, not this sketch folder).

1. Find your `lv_conf.h` (commonly under your Arduino libraries folder,
   e.g. `Documents\Arduino\libraries\lvgl\lv_conf.h`, though it can vary).
2. Replace it with the provided one (or manually set `LV_FONT_MONTSERRAT_20`
   to `1` and `LV_FONT_DEFAULT` to `&lv_font_montserrat_20`).
3. Recompile and flash.

Without this, text will look unchanged (still the old 14pt default) and
the recalibrated decorative scale effects (Game Over banner, faction
portraits, transmission name tag) will be sized wrong.

## How to play this build

1. Boot. AMOLED shows START -> **JOIN GAME** (not "HOST GAME" anymore).
2. Pick your seat (Player 1-4).
3. Pick your faction (see what's still available) -- this becomes your
   display name too, no typing needed.
4. You're in the Lobby. Any player can: toggle round length (6/8/10), open
   SELECT PLANETS to choose which worlds are in play, or START GAME once
   everyone's picked a faction (a status hint shows who's still deciding).
5. Phones: WiFi `ShadowFleet` / `starwars1`, then `http://192.168.4.1` --
   same seat -> faction flow.
7. Play as before -- reveal intel, fight/negotiate/pay for planets, battle
   other players, watch reputation and VP.

## Version history (recent)

- v0.14.1 - v0.14.4 - Two real production bugs fixed (a permanent mutex
  deadlock on Pay, and a scrollable-by-default LVGL tap target swallowing
  clicks), plus touch calibration properly re-derived from real measured
  Serial data after two earlier guesses each only partially worked.
- v0.15.0 - Phase A of the polish pass: player-selectable slots/factions on
  both platforms (AMOLED no longer hardcoded to slot 0), faction-driven
  starting reputation, reputation always visible, flexible planet selection
  during setup, and the fleet-founding rework. See above for full detail.
- v0.15.1 - Simplification: dropped the typed-name step entirely. Your
  "name" is now just your faction, set automatically the moment you claim
  one. Flow is now JOIN GAME -> pick a seat -> pick a faction -> Lobby --
  one screen shorter on both platforms. Also removes the AMOLED's
  on-screen keyboard/textarea entirely, which was the newest and least-
  tested piece of v0.15.0 (LVGL widgets never previously used in this
  codebase) -- removed cleanly, no dangling references. A character-select
  screen (a dozen named characters tied to the 4 factions, giving players
  both a personal name AND a faction) is logged for later; faction-as-name
  is the intentional scope for now.
- v0.16.0 - Phase B, part 1. Rebel Alliance's color changed to gold (was
  too close to Crimson Dawn's red). Garrison hiding: the live per-planet
  garrison count now only shows as a tier (LOW/MEDIUM/HIGH) by default,
  with the real number unlocking per-planet only after you've actually
  LOST a Fight there. AMOLED planet pagination: the scrolling list is
  replaced with 4 planets per page drawn as filled pie-slice wedges (a
  "pizza cut in quarters"), the first non-rectangular visual element in
  this project -- built after verifying the exact LVGL9 `lv_draw_arc()`
  signature against LVGL's own source rather than guessing. Wedge tap
  targets are simple bounding-quadrant rectangles rather than precise
  wedge-shaped hit regions, since the round glass bezel already clips each
  quadrant's unreachable far corner in practice. Small PREV/NEXT arrows
  sit right at the outer rim as requested -- intentionally at the edge of
  the touch-safe radius established earlier, worth specific feedback on.
  The scroll-reset complaint is resolved as a side effect of replacing the
  scrolling list entirely. Phone-side pagination, phone-side garrison
  hiding, and the reveal-button rework are the remaining Phase B pieces,
  not yet done.
- v0.16.1 - Planet wedges now have the same blue border every other button
  uses. Arcs have no native border concept, so each wedge draws twice: a
  full-size wedge in the border color, then a slightly smaller wedge
  (angle and radius both inset a little) in the ownership fill color on
  top -- reads as a clean border on the outer curve and a thin blue line
  along both straight edges, with a nice blue "spoke" where all 4 wedges
  meet at center.
- v0.17.0 - PHASE B COMPLETE. Phone-side pagination (2x2 grid, mirroring
  the AMOLED's per-page filtering/math). Phone-side garrison hiding --
  the JSON doesn't even send the raw number unless it's actually safe to
  show, not just hidden client-side. Real bug caught and fixed on BOTH
  platforms while wiring this up: the garrison display only unlocked on a
  loss, never accounting for it being the player's OWN planet -- an owner
  would have seen "MEDIUM" for their own world instead of their real
  count. Reveal-button rework on both platforms: bigger, full-width, and
  revealed values now occupy that same freed space in a bordered card
  instead of a small corner label. This closes Phase B; Phase C (1.5x
  text, Home rework) is next.
- v0.17.1 - Fix + diagnostic for "can't select PLAYER 2 -- OPEN on the
  phone, for any of them." Ruled out several server-side hypotheses first
  (STAGE_LOBBY correctly defaults to 0 at construction, no scoping issue
  hiding pickSlot from inline onclick handlers, query-string-on-POST
  already works fine for other endpoints) before finding a real structural
  bug: the seat/faction-picker screens rebuilt their entire DOM on every
  700ms poll tick unconditionally -- the same bug class that broke the
  name-input field earlier. Fixed with a snapshot-based guard that only
  rebuilds when the underlying data actually changes. Also added visible
  on-page diagnostic output (the raw server response or fetch error,
  shown directly on the phone screen) in case this wasn't the whole
  story -- mirrors the earlier Serial Monitor diagnostic approach, adapted
  since phone DevTools access is a much higher barrier.
- v0.17.2 - Pay now has a confirmation step, on BOTH platforms. Checked
  the AMOLED first before assuming this was phone-specific -- it had the
  exact same gap (a single tap on PAY committed real Credits instantly,
  no way back). Fight and Negotiate already naturally lead into further
  screens before anything's final; Pay was the only one of the three that
  went straight from tap to committed transaction. AMOLED gets a real new
  screen (SCR_CONFIRM_PAY) between method-choice and commit -- no stale-
  state risk, since re-entering a takeover always goes through the
  method-choice screen first. Phone gets a client-side confirm flag,
  explicitly reset on both starting and cancelling a takeover session, so
  it can't persist stale across a later, different attempt.
- v0.17.3 - Faction-colored planets, made genuinely faction-based on both
  platforms. Checked the AMOLED's existing wedge coloring before
  replicating it to phone -- "highlighted by the color of the syndicate
  that owns it" wasn't actually what was implemented, the wedges used a
  generic mine/rival/uncontrolled scheme, not real per-faction colors.
  Upgraded both to genuinely reflect the owning player's faction. Design
  note: the faction color went on the border, not a full background fill
  -- Rebel Alliance's bright gold as a full fill would have made light
  text unreadable against it specifically. Border carries the faction
  signal; fill/background stays a safe dark tone with a subtle warmer
  highlight for the viewer's own planets. Same colors, same design
  language, on both the AMOLED's pie wedges and the phone's planet grid.
- v0.18.0 - PHASE C, part 1: foundation + Home screen rework. Verified a
  real technical question before writing anything -- does LVGL's
  transform_scale (the zoom trick already proven on the Faction
  Transmission/Game Over screens) create a mismatch between where a
  button visually appears and where it's tappable, if applied broadly?
  Checked LVGL's own docs: transform properties are explicitly "visual-
  only... does not affect... clickable area." A button's tap target stays
  based on its real size regardless of the label's scale inside it -- the
  real constraint is just sizing buttons generously enough to contain the
  bigger text, not a touch-accuracy risk. Built one shared, reusable LVGL
  style (384 = 1.5x) applied inside makeButton() itself, so all 23 button
  call sites across the app get 1.5x text automatically. Home screen
  fully reworked: bigger LOCK IN button, 1.5x phase info, nav buttons
  resized/repositioned to actually use the screen instead of being
  crammed into a third of it -- every new position re-verified against
  the touch-safe corner bound. Remaining Phase C work: applying the scale
  to custom-built labels outside makeButton() (Lobby, pickers, Planet
  Detail, Battle, Standings, Reputation) and phone-side CSS font scaling,
  neither started yet.
- v0.18.1 - FREEZE FIX (hypothesis-based): "clicking START GAME freezes
  before Home can appear." Home turned out to be the most heavily-
  transformed screen built so far -- roughly 7 widgets using the scale
  transform at once (3 hand-built labels + 4-5 nav buttons), far more
  than any other screen tested (Select Planets, with just one
  transformed button, confirmed working fine). Each transformed widget
  needs an extra offscreen-buffer rendering pass in LVGL; this many at
  once may be a real resource/timing problem, matching the general class
  of issue this hardware has hit before under load. Reverted Home
  specifically to a known-safe state -- removed the scale from its 3
  labels, added makeButtonPlain() (same as makeButton() but without the
  transform) for its 5 nav buttons -- while keeping the bigger button
  sizes/positions from v0.18.0, which aren't suspected to be the issue.
  Every other screen's use of the scaled makeButton() is untouched.
  Honest confidence note: this is a reasoned hypothesis from the
  available evidence, not a confirmed root cause -- if the freeze
  persists after this, that points away from "too many simultaneous
  transforms" entirely and needs fresh diagnosis, not another guess in
  the same direction.
- v0.19.0 - PHASE C, TECHNIQUE SWITCH: confirmed the v0.18.1 freeze
  hypothesis was on the right track, then switched from the transform_
  scale "zoom trick" to a genuinely bigger default font -- a real font has
  no per-widget rendering cost, so there's no longer any "how many
  transformed widgets is too many" question to manage going forward.
  Requires a one-time local change to the user's own `lv_conf.h`
  (provided separately, not a project file): Montserrat 20 enabled and
  set as the default (was 14). Since nothing in this codebase overrides
  its font explicitly, EVERY label across the whole app now renders
  bigger automatically -- genuinely "everywhere," not just the screens
  swept so far. Firmware-side, retired the v0.18.0 transform machinery
  entirely (it would have doubled up with the new default -- 20pt base
  times a 1.5x transform on top), merged makeButton()/makeButtonPlain()
  back into one plain function, and recalibrated the 3 remaining
  decorative transform_scale usages (Game Over banner, faction portrait
  glyph, transmission name tag) to preserve their original visual size
  against the new bigger base rather than let them compound even larger.
  transmission_body_label's transform was removed entirely as fully
  redundant with the new default, with its wrap width fixed from a
  narrow pre-zoom value to a real one. Not yet verified: text wrapping/
  overflow on screens with width constraints tuned for the old smaller
  font -- a real possibility given the ~43% size increase, worth a
  broad look when testing.
- v0.19.1 - Bug-fix pass 1/5: takeover sessions are now per-player. A
  second player starting a takeover used to silently replace the
  first player's session (kicking them out of their flow, or swapping
  in someone else's negotiation offers). Pay/Negotiate messages are
  per-player too. A successful Pay now closes the session; a failed
  Pay returns the AMOLED to the approach screen with the reason, like
  the phone. Test: two players open takeovers at the same time; each
  should keep their own screen and offers.
- v0.19.2 - Bug-fix pass 2/5: a planet Fight can no longer overwrite a
  battle already in progress (the first attacker used to get stranded
  and their losses were lost). Also closes a double-tap exploit that
  restarted your own battle and undid your ship losses. A refused Fight
  shows "A battle is already in progress." on both platforms. Test:
  start a battle on one device, try FIGHT on another.
- v0.19.3 - Bug-fix pass 3/5: you can't Fight with no living ships.
  An empty fleet used to still roll dice -- it could knock out garrison
  ships, even capture a 1-garrison planet like Dagobah, or "lose"
  instantly and unlock the real garrison count for free. Now refused
  with "You have no ships left to fight with." on both platforms.
  Test: lose every ship, then try FIGHT on a planet.
- v0.19.4 - Bug-fix pass 4/5: the phone's `/state` data is now built
  from one consistent snapshot (was re-reading live game state ~30
  times per update, so values could disagree with each other), every
  text field is properly JSON-escaped, and `/join` no longer accepts a
  free-text name (unused since v0.15.1, and a way to inject text into
  other players' screens). No visible change expected -- test: phones
  play a normal game with no glitches; lobby, planets, offers, and
  standings all show correctly.
- v0.19.5 - Bug-fix pass 5/5: obligation check-ins no longer arrive a
  round late. When two came due together (or one was blocked by a
  reputation message), the second used to wait for the next round; now
  it follows as soon as the first is answered. On the phone, a new
  transmission also no longer replaces one you're still reading. Test:
  accept two honor-system offers in the same round; 3 rounds later both
  check-ins should appear back to back.
- v0.19.6 - Phone seat picker: a phone opened after the game has
  started now says "GAME IN PROGRESS -- New players can only join from
  the Lobby." instead of offering OPEN seats that the server refuses
  (it showed a raw `{"slot":-1}` error). Found in the first hardware
  test of the bug-fix pass. Joining mid-game is intentionally not
  allowed.
- v0.20.0 - PHASE D part 1/3: battles are animated on both platforms.
  Each exchange is a ~1.5s scene (tap to skip): dice tumble and land,
  laser bolts fly, the hit ship blinks and explodes with a screen
  shake. Planet battles happen in orbit above a rotating pixel-art
  planet (each planet has its own colors); PvP is open space. A banner
  announces the outcome. The AMOLED prints a `[BATTLE] ...` timing line
  to Serial after every exchange -- send it over if anything feels slow.
  Test: fight a planet, then a PvP battle; watch both devices.
- v0.20.1 - Combat rework part 1. The flat +1 attacker bonus is gone;
  planet battles are d6 vs d6 plus faction modifiers: +1 to you if
  you're Favorable+ with the planet faction's rival ("Rebel support"),
  +1 to the garrison if you're Hostile with the planet's own faction
  ("Imperial resentment") -- only once you've revealed its Affiliation.
  PvP stays a plain d6. FIGHT now opens a Battle Briefing: garrison
  tier, your intel, both roll formulas with sources, and a fleet
  picker, then CONFIRM FIGHT. Test: attack a rival faction's revealed
  planet (expect +1 both sides), then one you haven't revealed (none).
- v0.20.2 - Commitment & withdrawal rules. Once you CONFIRM FIGHT or see
  negotiation offers, that planet is closed to you for the rest of the
  round (no "peek, then switch"); leaving a negotiation asks first. Each
  fleet can attack once per round (a second fleet can still follow up).
  Withdrawing gives the other side a parting shot, and WITHDRAW takes two
  taps. A fully destroyed fleet leaves the board and can only be rebuilt
  at a planet you own (it comes back fresh). Refused PvP attacks now
  say why.
- v0.20.3 - The 1.75" board (ESP32-S3-Touch-AMOLED-1.75-B) is brought up
  and is now the default build. One codebase, two boards: `board_select.h`
  picks which; the 1.43" still builds with `-DSF_BOARD=143` (see
  the design notes). Display, touch and Wi-Fi verified on the 1.75". No gameplay
  change -- this is the foundation for the UI refresh.
- v0.21.0 - UI refresh step 1: the new look arrives. Home is now a hub:
  round-progress ring around the rim, GALAXY in the center (with one of
  your planets turning inside it), FLEETS and FACTIONS on the sides, LOCK
  IN at the bottom showing who's ready, and ATTACK during Resolution. Tap
  the round/phase/VP text at the top for Standings (rebuilt with faction
  insignia). The phone gets the same hub and colors, in your faction's
  accent. Other screens keep their old layout until their step.
- v0.21.1 - UI refresh step 2: a new round or phase now opens with a
  full-screen banner -- "ROUND 4 / 8, INVEST PHASE" over animated
  hyperspace streaks -- then Home (tap to skip). It never interrupts a
  battle or dialog. LOCK IN now asks "You still have 9 Credits unspent /
  Fleet 1 ready to fight -- lock in?" when something's left on the table;
  otherwise it's still one tap.
- v0.21.2 - UI refresh step 3: Galaxy is now a ring. Every planet in play
  sits around the edge of the screen as its own little pixel planet with
  its name, circled in the color of whoever holds it (thicker, with a
  tinted fill, if it's yours). No more pages or PREV/NEXT arrows. Tap a
  planet for its page. The phone's Galaxy matches: a 3-column grid of
  planet cards with the same colors.
- v0.21.3 - Galaxy ring polish: bigger planets (84px with all 12 in play,
  92px with 10 or fewer), CORUSCANT's name no longer cut off, and the
  "ring color = who holds it" line is gone.
- v0.21.4 - UI refresh step 4 (part 1): the planet page changes with the
  phase. In Invest, your own planets lead with GARRISON n / 8 and a big
  INVEST DEFENSE button (tap INTEL on the AMOLED to see its intel); other
  planets show DISCOVER INTEL tiles -- tap one to reveal it. In Movement
  the page is read-only: move your pieces on the board. Revealing intel is
  now done in Invest. Resolution's takeover page is next.
- v0.21.5 - Fix: the AMOLED planet page's intel tiles showed up off the
  top-left edge of the screen; they now sit in their 2x2 grid.
- v0.21.6 - UI refresh step 4 (part 2): in Resolution, FIGHT / NEGOTIATE /
  PAY are right on the planet page -- the separate "choose approach" screen
  is gone, so a takeover is one tap shorter. BACK from the battle briefing
  or the pay confirm returns to the planet (free, nothing was committed).
  If a takeover is refused (not enough Credits, a battle already running),
  the reason shows in red on the planet page.
- v0.21.7 - UI refresh step 4 (part 3): the Battle Forecast. FIGHT now opens
  a forecast screen: your fleet vs the garrison side by side with each
  roll's modifier and where it comes from, and your exact odds for one
  exchange -- WIN / TIE / LOSE with a bar. Pick a fleet, then ENGAGE.
- v0.21.8 - The garrison size (Low / Medium / High) stays hidden until you
  reveal that planet's Defense ("Garrison ???"). Your own planets, and ones
  where you've lost a fight, still show it.
- v0.21.9 - The Battle Forecast is now mostly pictures: your ships vs the
  garrison's strength, a big die per side with a +1 badge and the emblem of
  the faction behind it, and big WIN / LOSE odds.
- v0.21.10 - ATTACK moves into Fleets: in Resolution the FLEETS button
  becomes a red ATTACK button. Pick one of your ready fleets, then a rival,
  then which of their fleets to strike, and you get the Battle Forecast
  (even odds in PvP) with ENGAGE. The old lower-right ATTACK circle is gone.
- v0.21.11 - Garrison size is never shown on planets you don't own (no Low /
  Medium / High, no "???"); your own planets show the exact number of ships.
- v0.21.12 - Phone fix: taps on LOCK IN, the negotiation offers and other
  buttons no longer get lost while the page refreshes, and the page no longer
  flickers back to an older state (e.g. "LOCK IN" right after you locked in).
- v0.21.13 - Battle report: when a battle ends, CONTINUE shows VICTORY / DEFEAT /
  WITHDREW with what you won or lost. If the fight changed how a faction sees
  you, an INCOMING TRANSMISSION chip appears and the message plays after
  CONTINUE instead of interrupting the battle.
- v0.21.14 - The Galaxy screen is now a map of the 3D-printed board: every
  planet in its real slot with the rings, lanes, nebulae and faction hangars,
  and empty slots for planets left out -- set the table up to match it.
- v0.21.15 - Galaxy map is easier to tap: a touch anywhere near a planet (or the
  back button) picks the closest one and lights it up while you hold; lift
  over empty space to cancel. The back button moved to the top-left.
- v0.21.16 - Negotiation restyled: two big offer cards showing the price or
  "Honor promise", and the faction's emblem once you know who holds the planet.
- v0.21.17 - Factions (reputation) restyled: each faction's emblem, five pips
  showing your standing, and the tier word in a color for how they see you.
  The old TEST COMMS demo button is gone.
- v0.21.18 - New Start, Seat and Faction screens (join flow part 1): big title
  over hyperspace streaks, seats as four circles, factions as emblem cards.
  The pickers update live when someone else takes a seat or faction.
- v0.21.19 - New Lobby (player emblems, game length, START). Phones can now
  start the game. The planet picker is gone: all 12 planets are always in
  play, matching the printed board.
- v0.21.20 - Commanders: after your faction you pick one of its three
  commanders (12 original pixel portraits). Their faces show in the Lobby.
- v0.21.21 - Transmissions are holograms of a faction commander (never your
  own), with their name, instead of a colored box with a letter.
- v0.22.0 - The 1.43" becomes the TABLE HOST: it runs the game for up to 4
  phones and shows the table's galaxy clock (round, phase, who's locked in,
  everyone's face and VP). Up to 8 devices can join the Wi-Fi.
- v0.22.1 - Consoles talk over ESP-NOW: power the Table Console (1.43") first,
  and a Command Console (1.75") that boots near it joins the table's game as
  a player instead of starting its own. Phones join the table's Wi-Fi.
- v0.22.2 - Command Console responsiveness: its radio no longer power-saves;
  both consoles log link stats every 5 seconds.
- v0.22.3 - Faster console link: bigger, faster radio packets, so taps on the
  Command Console should feel instant again.
- v0.22.4 - Join by QR: the Table Console's Lobby shows a code to join the
  Wi-Fi and a code to open the game -- no typing.
- v0.22.5 - Home Wi-Fi mode: set the table up once from a phone, then it joins
  your Wi-Fi and shows ONE code -- phones play on their own Wi-Fi. A button on
  the table switches to its own Wi-Fi when you're away from home.
- v0.22.6 - Wi-Fi setup page opens instantly; a command console finds the
  table again by itself when the table switches Wi-Fi (no replug).
- v0.23.0 - Transmissions make sense now: factions react to what you actually
  did, on which planet, without repeating lines (even across games). Several
  can queue up. Eight rare PRIORITY transmissions for the big moments.
- v0.23.1 - Income: every new round pays +1 CR, plus 3 per planet you hold
  with High Resources and 1 per Low. The Invest banner shows how much.
- v0.23.2 - Faster taps on a command console: the table answers it right
  away instead of waiting behind phone traffic.
- v0.23.3 - Diagnostics: the consoles log their screen speed and memory every
  5 seconds (no gameplay change).
- v0.23.4 - More diagnostics: the time spent drawing the rim ring, stars,
  spinning planet and transmission static (no gameplay change).
- v0.23.5 - The command console no longer slows down as a game goes on, and
  every screen (transmissions too) should feel much quicker.
- v0.24.0 - The table screen reads from any side: phase ring with symbols,
  everyone's badge at their hangar with VP facing them, waiting players
  breathe, battles light up with blaster bolts.
- v0.24.1 - Table badges turned to sit at their hangars, and they show the
  faction symbol.
- v0.24.2 - Fix for the table restarting on its own (display crash).
- v0.24.3 - Table badges turned again so each faction points at its hangar.
- v0.24.4 - Big moments light up the table: a gold (or red) chase round the
  rim and the headline along both edges -- FIRST BLOOD, THE CORE FALLS,
  FINAL ROUND...
- v0.25.0 - The galaxy fights back: garrisons by ring (Outer 2-3, Mid 4-5, Core
  7-8; Defense High = the top of its ring), a faction you attack braces its
  other worlds (+1), and unclaimed worlds slowly rebuild.
- v0.25.1 - New look for the purchase, fleet and game-over screens.
- v0.25.2 - The table picks who goes first, celebrates the winner with
  fireworks, and pulses in attract mode while nobody has joined.
- v0.25.3 - Sound! The command console plays galaxy-wide moments, battle
  starts, new phases and incoming transmissions on its speaker.
- v0.25.4 - A new galaxy every game: planets' factions (except the four
  capitals), Defense and Resources are reshuffled at START. Capitals are
  always well defended, and prices no longer give a planet's strength away.
- v0.25.5 - Lighter attract-mode animation on the table.
- v0.25.6 - Attract mode removed; the command console chimes once at startup
  (speaker test) and reports its sound status.
- v0.25.7 - The table clock fills the screen: a big round number in the
  middle, the phase name above and below it, each phase in its own color
  (also on the rim), bigger badges and VP chips, no stars.
- v0.25.8 - Table text is even and readable (letters, VP numbers, headlines);
  the round number is truly centered.
- v0.26.0 - The table's game screens are rebuilt with a smooth glow renderer:
  one glowing mood color (gold Invest, cyan Movement, orange Resolution, red
  battles), rings that draw themselves on, white-hot numbers and symbols.
- v0.27.0 - New table design: each player has a hangar gate whose doors close
  when they lock in, a growing bar shows who is ahead, battles show live dice
  and ships, a sweep of light marks each phase change. White-blue / blue / red.
- v0.27.1 / v0.27.2 - The new table design runs smoothly again.
- v0.27.3 - NEW GAME at the end, ATTACK asks planet or player, a list of your
  promises (who, when due), repair lost ships in Invest (2 CR each).
- v0.28.0 - Bring two or three fleets into one battle (tap more fleets in the
  forecast). A player attacked by two fleets can bring another fleet too.
- v0.28.1 - Many promises are now judged by the game itself (take an Imperial
  world, don't invest here, pay 1 Credit a round...). Fixes phones freezing
  when you had a promise.
- v0.28.2 - A destroyed fleet can be rebuilt at your home hangar (no planet
  needed), so losing your only fleet no longer ends your game.
- v0.28.3 - Battles no longer get stuck; commanders' planets can only be
  fought for; Defense reads Low / Medium / High / Fortress; a "waiting on you"
  alert; PROMISE KEPT / BROKEN titles and a promise history; clearer owners
  on the map (the Empire is white now).
- v0.28.4 - Fleets 2 and 3 can be founded at your home hangar too.
- v0.29.0 - Reputation shows as a bar with a line at your exact standing
  (Factions), and every change shows as arrows on the screen that follows it.
- v0.30.0 - NEGOTIATE is a conversation: the planet's faction asks 3 / 4 / 5
  questions (Outer / Mid / Core) from a shared bank of 96; answer to their
  values. All liked = the planet with no strings; one miss = pick a favor you'll
  owe; two or more = their one hard favor; two insults = cut off; lose twice at
  a planet and it's closed to you for good. Reveal Affiliation first or you're
  talking to an unknown ambassador. A won conversation is +1 VP (+2 Core). PAY
  escalates: your first purchase is cheap, each one after costs more.
- v0.30.1 - Planets are worth VP while you hold them (Outer 2 / Mid 3 / Core 5,
  +1 seat of power) instead of VP every round; the VP goes with the planet. A
  planet won with a favor counts only once you keep it -- break it and they take
  the planet back, and every promise is judged before the final count. Favors are
  drawn from a bank of 56; in the last 2 rounds it's Credits now (dearer than
  buying) or a deed due when the war ends. Reveal Affiliation and they ask one of
  their own questions; a hostile faction won't talk (or unmasks at the end) --
  FIGHT or WALK AWAY.
- v0.30.2 - Playtest fixes: the lock-in confirm lists every ready fleet, not
  just Fleet 1; a 1.75" that reboots mid-game gets its seat back by tapping
  JOIN GAME; the battle report's CONTINUE no longer freezes when a
  transmission is waiting (and the INCOMING TRANSMISSION chip is tappable).
- v0.30.3 - Save states: the table saves the game every few seconds and picks it
  back up after a crash or power loss -- phones and the 1.75" rejoin their seats.
  END GAME (Standings, two taps) ends a game early for everyone.
- v0.30.4 - The table no longer hangs when it boots into a saved game, and the
  favor you're handed after a rough negotiation now varies (3-4 per faction).
- v0.30.5 - PAY works on phones again (broken since v0.30.0). **= Shadow Fleet 1.0** (tag v1.0).
- v2.0-dev.1 - First 2.0 build: Prepare phase, traits that do something (dealt
  each game), cycling markets with cuts, the diplomat, faction quests and
  strengths, a galaxy event at halfway, and "look at the table" on every
  screen. Deals, the Ashen Fleet and the Empire's 7-ship fleets come next.
- v2.0-dev.2 - Readable market text, trait effects on the planet page, the
  holder hears about diplomats, a quest card on Standings, a frame-rate meter.
- v2.0-dev.3 - DEALS: Credits for a promise between players, accept / counter /
  decline, enforced by the game; phones can type a message. Frame-rate test
  builds ready (tools/perf_ab.ps1).
- v2.0-dev.4 - THE ASHEN FLEET: an event with HP in the middle of the map; strike it
  together (every ship rolls a die, it answers with one die); markets CLOSED while it
  lives; +2 VP each when it sinks, +1 for the most damage; it takes over the table in color.
- v2.0-dev.5 - Frame rate + two freezes: a stuck touch read no longer freezes the
  1.75" for 5 s; a dev.3 flush hang fixed; -O2 build; streaks, rim, spinning planet
  and the Galaxy map drawn from pre-made images. Start 13.6 -> 22 fps, Home opens in
  ~72 ms (was ~200), Galaxy ~135 (was ~184).
- v2.0-dev.6 - Crash capture + recovery: why the last run ended and any crash's
  backtrace on every boot, the last 4 kept and shown at /health; an 8 s watchdog
  restarts a stuck console; the 1.75" rejoins its seat by itself after a reboot.
- v2.0-dev.7 - Swipe in from the left edge = back on the 1.75"; the table's lock-in
  flashes a seal across the gate with sparks along the ring, and stays sealed.
- v2.0-dev.8 - The 1.75"'s back buttons are gone: swipe in from the left edge instead.
- v2.0-dev.9 - Layout pass: buttons that sat beside a back button are centered.
- v2.0-dev.10 - The table rejoins the home Wi-Fi by itself when it drops (found by the overnight soak test).
- v2.0-dev.11 - Table lock-in: open gates quiet, a locked gate glows bright in its owner's faction color.
- v2.0-dev.12 - 17 worlds and HOME WORLDS (phones + table): you start holding your home world (guard of 6,
  only a fight takes it, worth 6 VP to whoever steals it); the Fortress trait is gone. The 1.75" waits for an AMOLED pass.
- v2.0-dev.13 - Drop out and rejoin: a quiet phone goes AWAY after 30 s (counts as locked in), STEP AWAY on Standings,
  take an AWAY seat back from any phone mid-game, WELCOME BACK recap. Table gates moved for the new board's hangars.
- v2.0-dev.14 - HOW TO PLAY on phones: eleven picture cards (seat picker, Lobby, Standings) to go with the rulebook.
- v2.0-dev.15 - Table crash fix: a player coming back from AWAY overflowed the web stack and crash-looped the table;
  phones only download /state when it changed (4 phones ran the table out of RAM).
- v2.0-dev.16 - Table RAM headroom: the glow's lookup tables moved to PSRAM (busy play with 4 phones: lowest free 0 -> 24 KB).
- v2.0-dev.17 - The table saves on its own task, so phones no longer wait 300-700 ms behind each save.
- v2.0-dev.18 - Rules fix: no more free +5 VP the first time your standing with your own faction moved.
- v2.0-dev.19 - /state is a third smaller (planets sent compact, expanded on the phone): 11.0 -> 7.2 KB.
- v2.0-dev.20 - The Empire's strength: all its fleets hold 7 ships (everyone else 6). Old saves are ignored.
- v2.0-dev.21 - Market shelves on the phone: each good a pixel object on its plank, BUY at the side, GONE when sold out.
- v2.0-dev.22 - The Empire's quest text says all three Core worlds (what the game always checked).
- v2.0-dev.23 - Simpler VP: worlds (home worlds 4), your quest, destroying fleets and the event's reward. Reputation, promise and market-award VP are gone.
- v2.0-dev.24 - The escort: faction help in a planet battle is a ship (escort for you, militia for the garrison) that takes the first hit, not a +1 on every roll.
- v2.0-dev.25 - The militia rises even on worlds you haven't read (the forecast doesn't show it there).
- v2.0-dev.26 - Mercenaries are 2 hired ships for your next battle (they take the first hits), not +1 to your rolls.
- v2.0-dev.27 - Forecast v2: your chance to win the whole battle, as a range and a word (STRONG / FAVORED / RISKY / LONG SHOT / UNCLEAR). The informant: learn a world's Sympathy or Defense from the forecast at 3x the price.
- v2.0-dev.28 - AUTO in battles (stops when you're down to 2 ships). Bring your hired ships or leave them home (forecast toggle); a defender calls theirs in.
- v2.0-dev.29 - Planet pages: one IF YOU ATTACK line (tap for the full forecast). Prepare Home: a card for every promise you owe, with its countdown.
- v2.0-dev.30 - Ship art in phone battles: a pixel ship per faction, mercenaries, the outlined escort, and a garrison kind per world.
- v2.0-dev.31 - Markets fit on one screen (2x2), clear stock, and say that holding a world earns from its sales. Resources and Trait are public. The attack line fits.
- v2.0-dev.32 - Lobby switch: MARKETS ON / OFF for a simpler first game (off: no markets, no Black Market trait, Crimson Dawn gets +1 CR a Prepare instead).
- v2.0-dev.33 - Lobby switch: GALAXY EVENT ON / OFF.
- v2.0-dev.34 - Crash fix: losing a Fleet 2 or 3 ship in a battle crashed the table (and it crash-looped on resume).
- v2.0-dev.35 - UI 3.0 step 1: the battle fills the whole phone screen, with ships and dice about twice as big.
- v2.0-dev.36 - UI 3.0 step 2: in Resolution the phone shows the galaxy -- tap a world or a rival fleet for its odds and FIGHT / TALK / PAY / ATTACK.
- v2.0-dev.37 - UI 3.0 step 3: Prepare is your hangar -- tap a bay to build or repair ships; the spinning GALAXY button.
- v2.0-dev.38 - UI 3.0: Resolution works like Prepare -- hangar-style Home with your fleets, the same Galaxy map, and a world's page with your odds, informants and FIGHT / TALK / PAY.
- v2.0-dev.39 - UI 3.0 step 4: Movement -- move on the board, your fleets, a true HOLONET RUMOR about some player (never who), the GALAXY button.
- v2.0-dev.40 - Market prices follow your standing only once you know the world's Sympathy (no more leaking affiliations).
- v2.0-dev.41 - The Holonet gets a voice: "Someone is pretty interested in Kijimi, huh?" when someone buys intel on a held world (the owner's phone alone says it's theirs), world leaks (thin garrisons, a world's leaning), sharper gossip. Old saves don't load.
- v3.1-dev.1 - UI 3.1 begins: phone screens are patched instead of rebuilt, so animations finish and taps never land on a vanishing button; shared motion timing; buttons ease and press. No rule changes.
- v3.1-dev.2 - Sheets and notifications: decisions slide up from the bottom (LOCK IN? first), news drops in from the top and stacks. No rule changes.
- v3.1-dev.3 - The Feed: a bell on Home opens your Inbox (what happened to you) and History (every public move by round); private news arrives as notifications. The game state moved to PSRAM (~15 KB of the table's internal RAM freed). Old saves don't load.
- v3.1-dev.4 - The table serves its own title font (Space Grotesk), so phones show the style guide's type even with no internet.
- v3.1-dev.5 - Pixel scoreboard numbers on Home that count up when they change; the full pixel icon set is in the page.
- v3.1-dev.6 - Pick one of 16 characters (faction x class) on one screen; defectors know their old faction's worlds; tap your face on Home for your card.
- v3.1-dev.7 - Classes play differently (Warlord, Consul, Infiltrator, Scoundrel) and stamp when they fire; class quests pay +5 VP the moment they're done. Old saves don't load.
- v3.1-dev.8 - THE FAVOR SPINE: each faction has its own five tier words and gifts (a one-time gift the first time you reach tier 4, an ongoing one while you stay at tier 5); you start at the top of tier 3 with your home faction; Pay moves with your tier, market stalls shut at tier 2 and below; a world won on a favor is yours with its VP at once (break your word and they take it back). Favor page + ladders, The Galaxy and You on Home. Old saves don't load.
- v3.1-dev.9 - Markets are FACTION STALLS: each faction sells its own goods, opens to you at tier 3, and is cheaper at tiers 4-5; no market cut any more. The Rebels' safe world is your home world. Old saves don't load.
- v3.1-dev.10 - GALAXY NEWS every round (a table-wide headline + one personal item each), GRUDGES (a faction you've made an enemy warns you, then strikes next round unless you climb out), JOBS (a faction offers favor + Credits to take a world), races, commotion, embargoes, offensives. News shows full screen on phones and big on the table. Old saves don't load.
- v3.1-dev.11 - Faction strikes do FIXED damage that the warning names a round ahead (so you can reinforce), late offensives are warned too, and a strike plays a short animation on your phone when it lands. Old saves don't load.
- v3.1-dev.12 - CHAMPIONS + ERAS: the top player at tier 5 with a faction becomes its Champion and holds a decree; declaring it starts that faction's Era for half the game (The Rebellion Rises, Imperial Order, The Spice Age, The Shadow Age), with a rule for everyone, a sting the faction's best friends are spared, and a champion perk. Old saves don't load.
- v3.1-dev.13 - ONCE-A-GAME MOVES from round 3: the Warlord declares war (War Room next round), the Consul calls a Summit (ceasefire, peacekeepers), the Infiltrator goes into Deep Cover (plant a story), the Scoundrel pulls The Heist. Old saves don't load.
- v3.1-dev.14 - The galaxy map sits in a window you can pinch, zoom and drag, with THREATS / CHANCES / MINE filters and a list below; tapping a world slides up its sheet.
- v3.1-dev.15 - Ship quirks (Imperial ranks, Rebel ships slip away, Pyke armored cargo, Dawn ambushers) stamp when they fire; the table queues its big moments; deals and the old faction strengths are gone; the 3.1 How to Play primer; the end of the game tells your story.
- v3.1-dev.16 - No Galaxy News in round 1; the round's news is one screen; job offers wait until the end of Prepare (look at the map first) and your answer shows in your own History.
- v3.1-dev.17 - Cleanup: no diplomat; one world view that slides up with a story per world; the store looks like the mockup and sells one good per faction a round; hyperspace wipe between phases; character names everywhere.
- v3.1-dev.18 - Add the game to your Home Screen and it opens like an app (no address bar; Back stays in the game); the once-a-game move is explained everywhere; a starburst hyperspace jump; galaxy events off by default.
- v3.1-dev.19 - Six world traits, each one you can see when it works; swipe down to close sheets and world pages.
- v3.1-dev.20 - See what each choice does to your favor before you pick; Era banners spell out their rules; pay off a grudge (expensive); push your luck after a bold heist; deals, the diplomat and other 2.0 leftovers are gone.
- v3.1-dev.21 - Bigger battles on the table: big dice and big ship counts.
- v3.1-dev.22 - Numbers pass #1: the Infiltrator's Spymaster quest needs 15 Sympathies (was 12).
- v3.1-dev.23 - Table 3.1: every table screen redone -- big text, pixel numbers, the acting faction's color, character names, battles that show only the fight, a hyperspace jump between phases. Home fleet bays say just "Empty bay".
- v3.1-dev.24 - The table drawn as the mockups: one gate arc per player, no inner rings, the battle laid out along each fighter's line, the mockups' news / strike / Era / victory layouts, and the Lobby redone.
- v3.1-dev.25 - Clean table text (words drawn whole, no wobble), layouts checked in a new table preview harness (tools/table_harness), overlaps fixed.
- v3.1-dev.26 - The table Lobby shows each character's face again as players join.
- v3.1-dev.27 - More room on the table: Lobby faces beside the code, names outside them, rim words further from the edge, battle numbers spaced out.
- v3.1-dev.28 - Jobs and Holonet rumors spread across many more worlds instead of repeating the same one or two.
- v3.1-dev.29 - In the second half of the game a job can name a world another player holds.
- v3.1-dev.30 - An Era lasts 3 rounds (was half the game).
- v3.1-dev.31 - The table's Wi-Fi button works again; Wi-Fi can be switched over the USB cable.
- v3.1-dev.32 - A busy Wi-Fi network can no longer restart the table (the screen checks in with the watchdog as it draws).
- v3.1-dev.33 - Android phones no longer stall everyone's taps (the table drops idle connections after half a second).
- v3.1-dev.34 - A slow phone connection no longer restarts the table (the watchdog waits 30 s).
- v3.1-dev.35 - The phone page loads about 3.5x faster (sent compressed) and isn't re-downloaded on reloads.
- v3.1-dev.36 - Taking a world from another player costs no favor; only one player can deal with a world at a time.
- v3.1-dev.37 - The Movement banner says to move your ships on the board; tapping an Inbox item opens its full popup.
- v3.1-dev.38 - The top title is no longer blurred when the game is added to an iPhone's Home Screen.
- v3.1-dev.39 - Sereth Kaan's portrait fixed; the Ancient reroll shows both rolls; your world's Defense shows its real garrison; "stall" is now "shop".
- v3.1-dev.40 - Paying off the Pykes costs 8 CR (was 12).
- v3.1-dev.41 - The phone map matches the new board: Mid Rim at 45 deg steps, space stations east and west, no hangars; fleets launch from your home world (fell: any world you hold; nothing: a space station).
- v3.1-dev.42 - MAKE AMENDS: three deeds per faction (a ship / a round / a world) call a grudge off and lift favor a full step; paying off lifts only half a step.
- v3.1-dev.43 - THE FINAL HOUR: gambits dealt in the second-to-last round (comeback draw 1/2/3, keep one) + a private black market; the last round = Talk/Pay + calling in debts, LAST ORDERS (one strike per fleet anywhere, jump passes, guards, pinned gambits), THE REVEAL (gambits played in secret), then the finale world by world (side by side; when the garrison falls the strikes turn on each other), played back on phones + table. How to play this build: play to the second-to-last round, draw on your phone, then give your orders and watch.
- v3.1-dev.44 - Playtest 4 fixes: phones reload by themselves after a flash (the old page hid the Final Hour), more room under the iOS status bar, PvP reports name the fleet.
- v3.1-dev.45 - Invisible help (the bigger side vs an unclaimed world quietly rolls best of 2 / 3), losses come off your biggest fleet first (a fleet keeps its last ship while it can), job offers arrive as a hologram from your contact.
- v3.1-dev.46 - The clarity pass: every transmission / news / grudge / job / closed shop gets a readout (why, your new standing, what changed) and Rivet, a droid who explains it on tap; the forecast says who sent each helper ship and whose standing moves if you win; "garrison" is now "defenders".
- v3.1-dev.47 - Warlord rework: no extra favor loss for conquests; taking a world by force pleases its rival more (+6, THE MUSCLE); talks ask a Warlord one more question.
- v3.1-dev.48 - Scoundrel rework: SELL IT TWICE (the rival pays for the same work; heat builds; caught at 3 = both sides strike, pay one back), income floor 6, MARK-UP +2 on Pay.
- v3.1-dev.49 - Race ties pay everyone that round; one champion title at a time (swap or keep); defectors start low with the faction they left; talking or buying a world annoys its rival a little; the Rebellion lets you buy worlds but THE PEOPLE RESIST; worlds held 3 rounds turn to your side.
- v3.1-dev.50 - Tier 6: Friend +10..16, Family +17.., Champion = +23 and the highest (champions around round 5-6 instead of 8).
- v3.1-dev.51 - Talks only offer favors you can do; races / jobs / commotions spread across the map; some unknown ambassadors are hooded figures.
- v3.1-dev.52 - The gallery: watch other players' talks live, whisper against them (2 CR, a harder next question) or vouch for them (a wrong answer gone), with favor stakes; every player is shown as a tag with their face and colour.
- v3.1-dev.53 - Big-ticket items in the black market (2 random offers, buy one a game, announced): Capital Ship, Planetary Shield, Mercenary Armada, Hyperspace Charts, Spy Network.
- v3.1-dev.54 - Attack alerts with WATCH (red and a buzz when it's your world), and spectator mode for anyone who joins mid-game.
- v3.1-dev.55 - The map's SIDE filter: light up the worlds you know side with a faction (or the ones you haven't read).
- v3.1-dev.56 - The phase track: the round's three steps on every phone with the current one lit, and a NOW line saying what to do.
- v3.1-dev.57 - The nebulae are stops (map + Final Hour reach); Declare War gets a timeline, a war sheet for both sides, and KEEPS / SENDS in the War Room.
- v3.1-dev.58 - A new HOW TO PLAY: an 8-card Basics deck, plus Rivet's Guides (8 topics) from the deck and Standings.
- v3.1-dev.59 - Hide the NOW line with its x (per phone); the NOW chip on the step bar brings it back.
- v3.1-dev.60 - Rivet no longer covers text; a shut shop's Rivet opens beneath the card.
- v3.1-dev.61 - SETTINGS: Rivet / NOW line / buzz / motion per phone; the table remembers up to 4 Wi-Fi networks.
- v3.1-dev.62 - Wi-Fi: slow phones can't stall the table, the join says why it failed, quietest channel on the table's own network.
- v3.1-dev.63 - Speed: the table's screen task moves off the web server's core; saves in small bursts; a text sweep.
- v3.1-dev.64 - The Galaxy and You: merged rows, sorted by urgency, 4 + MORE; a Rivet guide for it.
- v3.1-dev.65 - Rivet's Guides are listed by plain questions (WHAT NEEDS ME?, HOW THE GALAXY REACTS TO YOU, ...).
- v3.1-dev.66 - The phone's top bar stays on one line again (the Settings gear made it wrap).
- v3.1-dev.67 - FAVOR / STANDINGS buttons show live mood + a podium; game terms are tappable keywords Rivet explains.
- v3.1-dev.68 - Freeze fix: the table's screen task shares core 1 with the web server (dev.63's core-0 pin starved the watchdog).
- v3.1-dev.69 - A missed home Wi-Fi join retries every minute from an empty Lobby and restarts onto it.
- v3.1-dev.70 - Rivet explains negotiation, favors, Pay, Eras, the big moves, the War Room, the Final Hour and the ladder; 4 more keywords.
- v3.1-dev.71 - Fixes from a phone: favor bars, keyword hit areas, CALM THEM, the top bar, shop + Pay wording.
- v3.1-dev.72 - One set of tier words for every faction; the grudge deeds cost more (ships, defenders, rounds).
- v3.1-dev.73 - SCREENOFF / SCREENON per board; table_check.py.
- v3.1-dev.74 - Crash fix (console lock at boot, busy answers when RAM is low, phones back off); free debts; fuel announced; no side-switching; battle news waits; heist, HEAT row, favor bar, Era text, setup page fixes.
- v3.1-dev.75 - Transmissions reworked (incoming moment, one screen for two factions, chips, WHY?, hold-to-continue); YOUR KIT + kits from Standings; stamps show their source.
- v3.1-dev.76 - Defense: the PLANETS box opens a defense sheet; limits 6 / 8 / 10 once held; patrols; held worlds rebuild; dust settling; shaken retreats; the 3-player squeeze.
- v3.1-dev.77 - The black market: a door on Home (the markets strip goes dark), a smuggler's-den page, five new late-game lots.
- v3.1-dev.78 - Scoundrel: price on your head (bounty); Infiltrator: their file; the Slicer lot; the Crimson Dawn's Family gift is a one-time Blackmail.
- v3.1-dev.79 - BUYOUT (Pay on a held world: twice the price, sell or match) and UPRISING (Talk on a held world: Friend+, all hard, no misses, one try).
- v3.1-dev.80 - Battle replay: the last 8 battles in History, played back roll by roll.
- v3.1-dev.81 - FIRST GAME toggle (seat pick + Settings): Rivet introduces one thing per phase.
- v3.1-dev.82 - Talks: 24 trap questions on the hard tier (two replies in one faction's voice, one goes too far).
- v3.1-dev.83 - Crash fix: NEW GAME / END GAME rebuild the state in place (the temporary overflowed the web task's stack).
- v3.1-dev.84 - Blur fix for full-screen layers in the home-screen app; Court wording; black market NOW / FINAL HOUR + a better broker + an off-screen hint; first-game round-1 chain + checklist.
- v3.1-dev.85 - FIRST GAME switched on restarts the tour; the checklist shrinks to a pill.
- v3.1-dev.86 - The black-market broker is a pixel sprite like the characters (header, Home door hologram, stamps).
- v3.1-dev.87 - Flash: the plain copy of the phone page is gone (only the gzip is stored); a stale page now fails the build.

### v3.1-dev.88 (2026-11-18) -- Eras, powers, the black market's cheap end, Galaxy and You
- Eras last 2 rounds, are locked once declared, and every sting hits everyone (the champion too). Spice Age: income doubled (champion tripled), bribes x1.5. Imperial Order: patrols half price, tax on worlds past your 2nd, champion collects half. Rebellion: shorter negotiations, free repairs, anyone can start an Uprising. The Dawn grips the galaxy (replaces the Shadow Age): black market every round, files for sale (6 CR), the champion names the Mark (+5 CR a world).
- Declaring shows your personal forecast; your faction presses you every Prepare you hold a decree. Every number an Era changes wears its chip.
- WARPATH (Warlord): name a world in Prepare, strike it this Resolution with every ready fleet + 3 ships.
- Black market: six cheap lots (3-5 CR), four lots a round with one always cheap, and a CARGO HOLD (3) that keeps lots until used.
- Galaxy and You: one line a row, dots for what needs you, buttons on the row; the same rows end the round's news (YOUR SITUATION).
- TALK is now NEGOTIATE, PAY is now BRIBE.
- How to test: declare each Era (watch the chips + the table headline), the Dawn's Mark and file buying, a Warpath, buy and use cargo lots.

### v3.1-dev.89 (2026-11-20) -- Warpath any time
- WARPATH can be declared in any phase; you strike in this round's Resolution with every ready fleet + 3 ships.

### v3.1-dev.90 (2026-11-20) -- playtest sweep
- The War Room is fully gone; PULL BACK on your Warpath sheet when a Summit covers it; Hired Blade also defends your world; Era text says 2 rounds everywhere.

### v3.1-dev.91 (2026-11-20) -- phone cleanup
- The battle screen never resizes; AUTO pauses at 2 ships and AUTO again fights to the end; character select is centred.

### v3.1-dev.92 (2026-11-20) -- favor frozen in the Final Hour
- In the last round nothing changes your favor and there is no betrayal; the world page says FAVOR IS FROZEN.

### v3.1-dev.93 (2026-11-20) -- War Spoils
- Warlords: every world you take by force (from anyone) earns +3 favor with your own faction, with a WAR SPOILS stamp.

### v3.1-dev.94 (2026-11-20) -- character select, less reading
- The character card shows the story, one line on how the class plays and its power's name; YOUR KIT has every rule. Kit wording fixed (negotiations, Deep Cover's risk, push your luck).

### v3.1-dev.95 (2026-11-20) -- YOUR KIT button
- Character select uses the same YOUR KIT button as the in-game card.

### v3.1-dev.96 (2026-11-20) -- lobby cleanup
- No more Markets / Galaxy Event switches in the Lobby (markets always on, no halfway event). The seat picker offers CONNECT THE TABLE TO HOME WI-FI.

### v3.1-dev.97 (2026-11-20) -- Pull a File
- The Infiltrator's file is now PULL A FILE: a new pull every 2 rounds, and the pulled file stays open in between.

### v3.1-dev.98 (2026-11-20) -- the Back Channel
- Consuls: once a round, a private offer -- Credits now if a player stays off your worlds, a world, or another player this round. Break it and you pay back what you got; the Consul gets double. You're warned before you break one.

### v3.1-dev.99 (2026-11-20) -- fleet victories, kit sources
- Winning a fight against a commander's fleet earns +2 VP even when they withdraw. YOUR KIT shows what your worlds' traits and your Family factions give you.

### v3.1-dev.100 (2026-11-20) -- black market in the Dawn's Era
- Tapping BLACK MARKET while the Dawn grips the galaxy now opens it (it only worked in the Final Hour).

### v3.1-dev.101 (2026-11-20) -- honest odds + Rally
- The battle forecast counts the defenders a held world can really have: smuggled troops past the limit, and its patrol.
- RALLY: lose 3 exchanges in a row and your next roll gets +1 (stamped). Then it starts over.
- Field Refit rebuilds a wiped-out fleet. The forecast counts every ship that can join; the word follows the worst case.
- Defense intel shows the defenders you saw, and says when that changed (LOOK AGAIN).
- Outnumbering a world by 3+ / 7+ = best of 2 / 3 dice all battle, held worlds too (never PvP; invisible, in the forecast).
- A failed negotiation lets you fight that world the same round (+1 defender). Patrols stay until beaten.
- Consul rework: Old Friends, contacts every question, Treaty worlds, uprisings at any favor, home worlds by talk, No Stomach for War.
- One buy per shop a round; base income rises through the game; the Mark is never last place; /games keeps a game record.

### v3.1-dev.102 (2026-11-20) -- nobody wastes a turn
- The bigger fleet wins ties and its bad luck can't run as long (invisible, in the forecast). Extra dice and Rally are gone.
- Won by talk or bribe = INK STILL WET (one attack this round); won by force = DUG IN (+2). Lose a deal that way: Credits back / for your trouble + a head start.
- Beaten to a world: fight anyway, redirect one step, or stand by (+2 ships next round).
- GUNS FOR HIRE: the Warlord lends up to 3 ships for a price. Consul contacts: the hardest question only. Every Galaxy and You row opens.

### v3.1-dev.103 (2026-11-20) -- the cockpit
- A cockpit console with five decks: DOCKS, STAR MAP, BRIDGE, COMMS, WAR TABLE. Tap or swipe; your faction trims the dash.
- COMMS = the Galaxy and You, grouped, with this Prepare's list. LOCK IN is held, not tapped. LIVE NOW bar for battles and talks.
- One character sheet, one Era sheet, one gear menu (guides + How to Play live there now).

### v3.1-dev.104 (2026-11-21) -- cockpit fixes
- LOCK IN lives at the end of the Bridge (still held); the console stays under sheets; switching decks closes an open world.
- A visible hologram refresh when you switch; swipe works on the Star Map unless you're zoomed in.

### v3.1-dev.105 (2026-11-21) -- console fit
- The Bridge key's outline sits on the key, the outer keys fit the screen and clear the iPhone home bar, the dash lights are visible.

### v3.1-dev.106 (2026-11-21) -- lights that mean something
- COMMS pulses for something new and urgent (steady once you've seen it), DOCKS for a black market you haven't visited; LEDs centered; stat boxes even.

### v3.1-dev.107 (2026-11-21) -- a longer hold, reliable updates
- LOCK IN takes a slower hold. After a flash, phones fetch the new page a moment apart and keep trying until they have it.

### v3.1-dev.108 (2026-11-21) -- a level console
- Five keys in one level row (no raised hex), a flatter dash, everything on screen; the three stat boxes match.

### v3.1-dev.109 (2026-11-21) -- the cockpit, properly
- The raised hex Bridge key is back and centered with the dash lights under it; the dash runs to the bottom edge (no black bar).

### v3.1-dev.110 (2026-11-21) -- no more wedges
- A boot watchdog restarts a table that gets stuck starting Wi-Fi; low memory = phones are told to retry instead of the table falling over. Star field; LEDs under the Bridge key.

### v3.1-dev.111 (2026-11-21) -- the frozen table, found
- A router that turns the table away once no longer strands it on its own network: it tries again at boot and keeps trying in the background. More stars.

### v3.1-dev.112 (2026-11-21) -- more memory headroom
- The table's screen library keeps its workspace in the big memory chip, leaving far more room for phones; big page sends wait when memory runs low.
- No more who-goes-first screen at the start of a game.

### v3.1-dev.113 (2026-11-22) -- withdraw wins keep promises
- Beating a fleet that runs now counts for debts like "win a battle", not only for the VP.

### v3.1-dev.114 (2026-11-22) -- playtest fixes, batch 1
- One buy per shop a round (it really works now), uprisings for anyone, ceasefires block buyouts and uprisings, jobs give 3 rounds, clearer favors / buyouts / ink / Forged Orders, no double-tap zoom, no > arrows, swipe notifications away, Credits on the defense sheet.

### v3.1-dev.115 (2026-11-22) -- worlds and the star map
- Defense shows as LOW / MEDIUM / HIGH / FORTRESS on every world, live; Sympathy is learned from the worlds you hold (nothing to buy); a streamlined world page you can swipe through; a fixed star map with every world in one list.

### v3.1-dev.116 (2026-11-22) -- Last Orders and the gambit moment
- Last Orders shows your Credits, lets you hire extra ships for a strike (3 CR each) and pick a gambit from a list; at the reveal you must play your gambit or keep it before you can be ready.

### v3.1-dev.117 (2026-11-22) -- the Final Hour in step
- The table keeps the clock: every phone watches the same strike at the same time, and a strike is skipped only when everyone asks.

### v3.1-dev.118 (2026-11-22) -- cooler phones
- The phone page asks the table less often when nothing is happening, pauses its looping animations when nobody is touching it, and no longer redraws the star-map key 60 times a second.

### v3.1-dev.119 (2026-11-22) -- the map flash and a small skip button
- Each Final Hour strike opens on the map with a ring on the target and ends with the world flashing the winner's colour; one small SKIP TO THE END button (everyone has to press it); the star map lists worlds under each filter instead of under ALL; Credits read "44 - 18".

### v3.1-dev.120 (2026-11-22) -- drag to close
- Dragging a world page or a sheet down from its top now moves it with your finger.

### v3.1-dev.121 (2026-11-22) -- the finale starts on the first strike
- No more blank screen before the first strike, and the first strike plays in step on every phone.

### v3.1-dev.122 (2026-11-22) -- the leftovers
- How to Play and the first-game tour use the cockpit names; jobs aim near your worlds; whisper / vouch explain themselves; Pull a File is on the War Table; the debts sheet opens itself in the last Prepare; SAVE A REPORT in the gear menu; income and spending in the game record.

### v3.1-dev.123 (2026-11-22) -- the table shows each strike
- During the Final Hour the table shows which strike is playing (STRIKE 3 OF 7, the world, who attacks), then its result.

### v3.1-dev.124 (2026-11-23) -- How to Play, redone
- Rivet walks you through the real screens, spotlighting each button (13 steps, a little flavor). First fight, first negotiation and first promise each get a short explanation the first time.

### v3.1-dev.125 (2026-11-23) -- the factions, explained
- WHO ARE THEY? in a negotiation and THE FACTIONS in the gear menu: who each faction is, what they like and hate, and how to answer them.

### v3.1-dev.126 (2026-11-23) -- polish + an Arduino IDE backup
- Last Orders: SMUGGLERS fits beside its target. World page: hidden details say "Unknown", not "?".
- Building in the Arduino IDE: the sketch carries its own build_opt.h, and a ready lv_conf.h ships in arduino/.

### v1.0 (2026-11-23) -- the public release
- Shadow Fleet goes public: MakerWorld listing, public repo and web installer. Same game as v3.1-dev.126, renamed v1.0.

### v1.0.1 (2026-11-23) -- unused home worlds play like any Outer Rim world
- A home world whose faction nobody is playing is an ordinary Outer Rim world: its normal guard, 2 VP, and you can negotiate for it or bribe it.

### v1.0.2 (2026-11-23) -- no more empty Rivet box
- Galaxy News with nothing to point at (like THE GAMBITS ARE DEALT) shows its "for you" line plainly instead of an empty box.

## What's still ahead (the rest of the agreed plan)

The live roadmap, current focus, and design decisions now live in
`the design notes` at the repo root (the single source of truth); this is a
summary.

- **Phase B - DONE** (v0.16.0 - v0.17.0). Pagination, garrison hiding, and
  the reveal-button rework, all on both platforms.
- **Phase C - DONE** (v0.18.0 - v0.19.0). Bigger text everywhere (via a
  Montserrat 20 default font, not the zoom trick) and the Home rework.
- **Bug-fix pass - DONE** (v0.19.1 - v0.19.5). All five known issues
  found 2026-09-23, fixed before building on top of them.
- **Phase D (in progress)** - v0.20.0 animated battle scene (done),
  v0.20.1 combat modifiers + battle briefing (done), v0.20.2 commitment
  & withdrawal rules (done), v0.20.3 spectating, v0.20.4 post-battle taunts. Full agreed
  design in the design notes.

## Deferred (planned, not bugs, unrelated to the phase plan)

- No ship "repair" mechanic -- destroyed ships stay destroyed.
- Real portrait art for Faction Transmissions (currently a placeholder
  shape + faction initial) -- ready to swap in whenever art is supplied.
- Rare edge case: an obligation check-in and a fresh reputation-crossing
  transmission becoming due at nearly the same moment can have the newer
  one overwrite the pending notification before it displays. Low
  probability (reputation only moves from your own Resolution-phase
  actions; check-ins queue at round start and show immediately). The
  obligation isn't lost -- since v0.19.5 it resurfaces right after the
  reputation transmission is dismissed, not a round later.

## COMPILE STATUS -- READ THIS BEFORE FLASHING

v0.15.0 was a large, multi-file architectural change touching all four main
files. Brace/paren balance was verified in every file, including the
embedded phone JS (braces/parens/brackets all separately checked), and
every `gameState*` call across both UI files was confirmed to resolve
against a function actually declared in `game_state.h`.

v0.15.1 removed the keyboard/textarea entirely (the flagged highest-risk,
least-tested piece of v0.15.0) rather than fixing it -- the simplest
possible resolution to that specific risk. Confirmed via grep that no
lv_keyboard/lv_textarea reference remains anywhere in the file. Brace/
paren balance and the full gameState* call graph were re-verified after
the change.

v0.16.0 touched game_state.h/.cpp (garrison tiers + loss tracking), the
.ino (pie-wedge planet pagination, garrison display, color fix), and
web_server.cpp (color fix only so far). Brace/paren balance and the full
gameState* call graph were verified across all four files. The
`lv_draw_arc` call was checked against LVGL's own source (github.com/
lvgl/lvgl, lv_arc.c) for its exact LVGL9 signature before being written,
given the transform_zoom/transform_scale mixup from earlier in this
project's history -- not assumed or guessed.

v0.16.1 touched only the .ino (the wedge border fix, two draw calls per
wedge instead of one). Brace/paren balance was re-verified.

Both v0.16.0 and v0.16.1 are genuinely new visual territory (the first
non-rectangular UI element in this project) and have not been seen
rendered on real hardware -- the wedge layout, border appearance, tap-
target accuracy, and rim-arrow reachability are the parts most likely to
need a
tuning pass once actually tested, the same way touch calibration and
button hitboxes needed real iteration earlier in this project.

v0.17.0 touched the .ino and web_server.cpp (Phase B completion across
both platforms). Full balance check and gameState* call-graph resolution
were re-verified across all four files, including the embedded phone JS
(braces/parens/brackets separately checked). The garrison-visibility gap
for planet owners (fixed on both platforms that version) was found by
deliberately re-checking the display logic against every viewer context
-- owner, non-owner-never-lost, non-owner-has-lost -- rather than assuming
the original implementation covered all three correctly.

v0.17.1 touched only web_server.cpp (the seat/faction-picker rebuild fix
and diagnostics). Balance was re-verified, including the embedded JS. A
str_replace edit mid-fix briefly left mismatched braces in the faction-
picker block (an artifact of only replacing part of a nested structure)
-- caught by the routine balance check immediately after, not shipped.

v0.17.2 touched the .ino and web_server.cpp (the Pay confirmation step,
both platforms). Balance and the full gameState* call graph were re-
verified across all four files. Checked whether the reported gap was
phone-specific before assuming so -- it wasn't, the AMOLED had the
identical gap, which is why that version fixed both rather than just the
platform that was reported.

v0.17.3 touched the .ino and web_server.cpp (faction-colored planets,
both platforms). Balance and the full gameState* call graph were re-
verified. Checked the AMOLED's actual existing implementation before
replicating it to phone, rather than assuming the described behavior
matched what was really there -- it didn't (generic ownership colors,
not faction colors), so both platforms were upgraded to the same
genuinely faction-based design rather than just copying an
approximation.

v0.18.0 touched only the .ino (Phase C foundation + Home rework). Balance
and the full gameState* call graph were re-verified. The transform_scale
touch-safety question was checked against LVGL's own documentation before
committing to applying it broadly, not assumed safe by analogy to its
narrow prior use on non-interactive banners. Every repositioned Home
button was individually re-checked against the established touch-safe
corner-distance bound rather than just visually estimated.

v0.18.1 touched only the .ino (the Home freeze fix). Balance was re-
verified. That fix was explicitly hypothesis-based, flagged as such at
the time -- confirmed correct in practice, since removing the transform
was exactly the direction that led to v0.19.0's full resolution.

v0.19.0 touches the .ino, version.h, and -- critically -- a file OUTSIDE
this project entirely: the user's own `lv_conf.h`, provided as a separate
download alongside this zip. **This build will not look right without
that file also being applied** (copied over the existing `lv_conf.h` in
the local LVGL library installation, then recompiled) -- Montserrat 20
must be enabled and set as `LV_FONT_DEFAULT`, or all the recalibrated
transform_scale values in this version (tuned specifically for a 20pt
base) will be off. Balance and the full gameState* call graph were re-
verified after the reconciliation work. Not yet verified: text wrapping
against the new font size on screens with tight width constraints tuned
for the old one -- flagged in the changelog, worth checking broadly.

If a compile error surfaces, paste the first few and they'll be quick to
fix. If something behaves unexpectedly at runtime, describe exactly what
you tapped and what happened -- given how much is new here, specific
repro steps will matter more than usual for a fast diagnosis.
