#pragma once

// ---------------------------------------------------------------------------
// FIRMWARE VERSION
// Bump this whenever you flash a meaningfully different build. Check this
// exact string against Serial output on boot, the AMOLED home screen, and the
// phone page footer to confirm all three actually agree on what's running.
// ---------------------------------------------------------------------------
#define FIRMWARE_VERSION "v1.0.2"
#define FIRMWARE_DATE "2026-11-23"
// v0.1.0 - Original single-player Shadow Fleet UI (star field, static screens)
// v0.2.0 - Corrected BSP (pins/reset/enable/touch protocol) for 1.43 (non-C) board
// v0.3.0 - WiFi AP + phone defender terminal; multi-exchange battles (fight/withdraw);
//          Planet Intel with per-role Credits and reveal-to-discover characteristics
// v0.3.1 - Fixed boot loop (assert failed on buffer2): display DMA buffers were
//          competing with WiFi for the same limited internal RAM pool. Shrunk
//          buffer size (100->40 lines each) and init display before networkInit().
// v0.4.0 - Fixed green-flash-on-boot artifact (panel started at full brightness
//          before first frame rendered). Reworked battles: abstract fleet-health
//          numbers replaced with a real named 3-ship roster per side; exchanges
//          now say which ship was destroyed. Removed all emoji from phone UI
//          (rendering as boxes/tofu on some browsers) in favor of plain text.
// v0.4.1 - Added explicit UTF-8 charset declaration (meta tag + HTTP header) to
//          the phone page, since any leftover non-ASCII character without it
//          can render as mojibake on some phone browsers. Also zero-initialize
//          the two DMA display buffers right after allocation, since malloc
//          doesn't clear memory -- belt-and-suspenders fix alongside v0.4.0's
//          brightness-sequencing fix for the green boot flash.
// v0.4.2 - Touch/redraw latency tuning: explicitly set the LVGL touch-poll
//          timer to 15ms (was relying on lv_conf.h's default, likely 30ms+),
//          and raised the QSPI transaction queue depth from 2 to 4 so screen
//          flushes don't stall waiting for a free DMA transaction slot.
// v0.5.0 - Added a real game-phase system (Invest -> Movement -> Resolution),
//          synced between AMOLED and phone: both sides lock in when ready,
//          the phase advances once both have, looping back to Invest starts
//          a fresh round (ships restored; Credits/intel persist). Home screen
//          reworked around one big centered "lock in" action instead of a
//          crowded 2x2 button grid. Fixed BACK buttons being noticeably
//          smaller than every other button (44px vs 58-64px tall) -- likely
//          why they felt unresponsive -- and fixed a genuine overlap between
//          the last Planet Intel reveal button and BACK on the planet detail
//          screen. All primary buttons enlarged and repositioned closer to
//          screen center throughout.
// v0.6.0 - Planet takeover: the actual win condition. Planet Detail is now
//          phase-aware -- Invest/Movement still shows intel-reveal buttons,
//          but Resolution shows planet ownership plus two ways to take
//          control: CHALLENGE (launches a battle tied to that specific
//          planet; a decisive win or the enemy withdrawing hands you
//          control) and NEGOTIATE (a random, faction-flavored offer --
//          Crimson Dawn and Rebel Alliance pools are live now, Galactic
//          Empire and Pyke Syndicate pools are written and ready for when
//          the galaxy grows past 2 planets; accepting spends Credits and
//          transfers control with no fight). Planets list now shows a
//          (YOU)/(ENEMY) ownership badge per planet, and the Battle screen
//          header names the contested planet instead of a generic subtitle.
//          Negotiate is AMOLED/attacker-only this round; a phone-side
//          Negotiate UI is a clean next step.
// v0.7.0 - MULTIPLAYER RESTRUCTURE. Game state is no longer hardcoded
//          attacker-vs-defender: up to 4 players in numbered slots (slot 0 =
//          HOST = the AMOLED for now; slots 1-3 = phones). New lifecycle:
//          Start screen (HOST GAME) -> Lobby (phones join + name themselves,
//          rejoin-safe via a stored client id) -> host START GAME (allowed
//          solo for testing). Every player shares the same capabilities on
//          both AMOLED and phone (parity). Full takeover flow implemented per
//          spec: TAKE OVER PLANET -> COMMIT to Fight / Negotiate / Pay ->
//          resolve that path (Negotiate = 2 offers, no bailing to Pay). Fight
//          = your fleet vs the planet garrison; attacker may withdraw, the
//          garrison fights to the death. Pay price scales with resources +
//          current defense. Planet owners can INVEST during Invest phase to
//          add garrison ships (cap 8). Full 22-offer negotiation pool from
//          the design notes, split across Rebel/Empire/Pyke/Crimson Dawn +
//          neutral, all battles randomized. Non-credit offers (favors,
//          obligations, tasks) are honor-system flavor per decision 6B.
//          DEFERRED to next session: reputation-shaped offers/pricing,
//          player-vs-player battles (attacking a rival directly), the
//          "you are being attacked" cross-notification, and a formal
//          victory screen.
// v0.7.1 - Compile fix for v0.7.0: the addHeader() function and the
//          onBattleScreen flag were accidentally dropped at the file-
//          reassembly seam during the restructure, causing "not declared in
//          this scope" errors across every screen builder. Both restored.
//          (The "Multiple libraries found for WiFi.h" message is a harmless
//          warning -- Arduino picks the correct ESP32 built-in WiFi; the
//          stray WiFiEspAT library in your libraries folder can be deleted
//          if you want the warning gone, but it's not required.)
// v0.8.0 - Player-vs-player battles + bigger button hitboxes. Each player now
//          has 3 fleets of 3 ships (real tracked state). During Resolution:
//          BATTLE -> pick opponent -> pick their fleet -> ATTACK. PvP battles
//          are symmetric: BOTH sides tap FIGHT/WITHDRAW each exchange, either
//          may withdraw, stakes are ships only (no territory). The defender is
//          alerted ("UNDER ATTACK by X") and the AMOLED auto-jumps into the
//          battle when the host is targeted. One global battle at a time.
//          Full AMOLED + phone parity for the whole PvP flow. Hitbox fix: all
//          styled buttons now have a 12px invisible extended click area so
//          near-misses still register (helps if touch calibration drifts).
// v0.9.0 - Fleet management + two UI fixes.
//          (1) Bigger hitboxes: extended click area raised 12px -> 26px on
//          every styled button.
//          (2) Fixed a real bug: the Resolution-phase BATTLE button wasn't
//          appearing on Home until you navigated away and back, because the
//          nav row was only built once per screen-draw and a phase change
//          (often triggered by your own LOCK IN) didn't force a rebuild. Home
//          now detects a phase change and rebuilds itself automatically.
//          (3) Fleet management: each fleet now holds up to 6 ships (up from
//          a fixed 3), investable 1 at a time for 2 Credits during Invest
//          phase, persisted in game state -- ship losses from battle stay
//          lost across rounds; investment adds new ships on top of survivors.
//          Fleets screen reworked as a 3-fleet summary list -> tap into a
//          fleet to see each ship's individual status (ACTIVE/DESTROYED/not
//          built) and invest. Planet-takeover Fight battles now also draw
//          from and write losses back to a real named fleet (auto-picks your
//          first fleet with available ships), matching how PvP already
//          worked -- previously a takeover fight used a throwaway fleet that
//          had nothing to do with your actual ships. Full AMOLED + phone
//          parity for fleet viewing and investment.
// v0.10.0 - Faction Transmission: the reputation "face moment" prototype,
//          built standalone so it can be tuned for feel BEFORE real
//          reputation tier-crossing logic exists. Screen shakes (decaying,
//          like a connection punching through), a placeholder portrait
//          (colored circle + faction initial -- swap for real art later,
//          baked into flash, no SD card needed), and a classic bottom
//          dialogue box with typewriter-revealed text. Tap once to
//          fast-forward the typing, tap again to dismiss. Triggered for now
//          via a TEMPORARY "TEST COMMS" button on Home that cycles through
//          four demo lines across different factions -- remove/replace once
//          wired to real reputation events next session.
//          Reputation design locked for implementation: 5 tiers (Hostile/
//          Wary/Neutral/Favorable/Allied) on a -10..+10 scale (Hostile<=-7,
//          Wary -6..-2, Neutral -1..+1, Favorable +2..+6, Allied>=+7).
//          Conquering a planet: -3 with that faction, +1 with their rival
//          (Rebel<->Empire, Crimson Dawn<->Pyke), rival spillover ONLY on
//          military conquest. Withdrawing: 0. Successful Negotiate: +2.
//          Successful Pay: +1. Declining an offer: -1. Betrayal rule: if you
//          conquer a planet belonging to a faction you're currently
//          Favorable+ with, you drop straight to Neutral regardless of your
//          banked score. Tier affects Pay price and where Negotiate costs
//          land in their range (Hostile rolls near max, Allied near min).
//          Ambush mechanic explicitly deferred, not in this build.
// v0.10.1 - Fixed the transmission shake (real bug: transforming the LVGL
//          screen object directly isn't reliable given this project's
//          partial-buffer rendering; shake now animates a dedicated
//          full-screen container instead, which is the standard approach).
//          Also fixed an LVGL9 API-name error caught before it could become
//          a compile failure: LVGL 8's "transform_zoom" style setter was
//          renamed to "transform_scale" in LVGL 9 -- code now uses the
//          correct v9 name. Body text scaled up (~2.15x) and center-aligned;
//          faction name and portrait glyph also enlarged/centered for a
//          more cohesive look. Text is a visual scale-up of the small
//          default font (not a larger font file), so it may look a touch
//          soft rather than crisp -- ask if you want to switch to a real
//          larger built-in LVGL font instead (requires one lv_conf.h edit).
// v0.10.2 - Fixed a real clipping bug seen in testing: transform_scale
//          defaults to pivoting from a widget's TOP-LEFT corner, not its
//          center, so scaled-up text was expanding asymmetrically off to
//          the right and getting cut off by the dialogue box border.
//          Explicit center pivot (LV_PCT(50)/LV_PCT(50)) added to all three
//          scaled labels (glyph, faction name, body text) so they now
//          expand evenly outward. Body text scale reduced 2.15x -> 1.5x per
//          feedback; wrap width widened (180->230 pre-zoom) so it still
//          makes good use of the box at the smaller scale rather than
//          wrapping into an unnecessarily narrow column.
//          Shake motion not yet retuned -- waiting on a reference clip/
//          description of the desired motion before iterating further.
// v0.10.3 - Shake and visual style, driven by a real reference video
//          (pixel-art RPG dialogue system devlog). Shake rebuilt: was a
//          smooth decaying sine sway (450ms) that read as floaty; now a
//          sharper front-loaded X/Y jitter (squared decay, both axes) over
//          260ms for a punchier "impact" feel instead of a wave. Visual
//          style updated to match the reference's language: dialogue box
//          now has sharp corners (was rounded) for a more authentic retro
//          feel; the faction name moved out of the box into its own small
//          nameplate tag that straddles the box's top edge like a flag,
//          instead of floating text inside; portrait placeholder reshaped
//          from a plain circle into a tall rounded-rect "bust frame,"
//          closer to what real portrait art will eventually look like.
//          Note: portraits stayed centered near the top rather than
//          corner-anchored like the reference -- this is a ROUND display
//          with no true corners, and content pushed toward the circular
//          edge is exactly what caused the earlier touch-hitbox problems.
// v0.10.4 - Static burst on transmission entry, per feedback: "like it's
//          patching in through the current screen." A blocky static
//          overlay (same cheap live-redraw technique already used for the
//          star field -- no full-screen pixel buffer, which would be far
//          too much RAM alongside WiFi/display buffers) flickers over the
//          already-built transmission content for ~270ms right as the
//          screen opens, then clears straight into the shake + typewriter.
//          Tapping during the static skips it immediately, for fast
//          repeat-testing. Also logged for later, NOT started: "UI 2.0"
//          pass once the game itself is working -- new fonts, sizing that
//          properly fits the round screen, larger text across the board.
// v0.11.0 - REPUTATION SYSTEM, fully wired to real gameplay (not just the
//          test-button prototype). Per-player, per-faction hidden score
//          (-10..+10) with 5 tiers (Hostile/Wary/Neutral/Favorable/Allied),
//          exactly as designed: conquering a planet -3 with that faction +1
//          with their rival (Rebel<->Empire, Pyke<->Crimson Dawn), rival
//          spillover only on military conquest; withdrawing 0; successful
//          Negotiate +2; successful Pay +1; declining a rolled offer -1;
//          betrayal rule (conquering a Favorable+ faction's planet drops
//          you straight to Neutral). Tier consequences now live: Pay price
//          scales by tier (+4 Hostile down to -4 Allied), and each
//          Negotiate offer's real Credit cost is tier-adjusted too (shown
//          to the player before they choose, not just charged silently).
//          The Faction Transmission screen now fires for REAL tier
//          crossings -- checked immediately after every Fight/Pay/
//          Negotiate/Decline resolves, on both AMOLED and phone, with a
//          periodic safety-net check as backup. Full parity: the phone got
//          its own complete static/shake/typewriter implementation
//          (canvas-based static, JS-interval shake matching the AMOLED's
//          decay curve) plus its own TEST COMMS button.
//          Fixed a real compile-blocking bug while wiring this in: the
//          shared game state and its mutex were referenced by the new
//          reputation functions before their declaration later in the
//          file -- moved to the top of game_state.cpp where every function
//          can see them.
// v0.12.0 - REPUTATION VISIBILITY + HONOR-SYSTEM FOLLOW-UP.
//          Reputation display: Planet Detail now shows "REP: <TIER>" in a
//          corner, but ONLY once you've revealed that planet's Affiliation
//          -- tier only, never the raw score. New REPUTATION screen (Home ->
//          REPUTATION) lists your standing with all 4 factions, showing
//          UNKNOWN for any you haven't discovered yet anywhere. Full parity
//          on the phone: same corner display in planet detail, same
//          REPUTATION screen. The TEMPORARY TEST COMMS button moved off
//          Home onto the new Reputation screen on both platforms, since
//          that's a much more natural home for it and it was crowding Home.
//
//          Honor-system obligation follow-up: accepting a non-Credit
//          negotiation offer now creates a tracked promise, due for a
//          self-reported check-in 3 rounds later (OBLIGATION_ROUNDS_DUE,
//          tunable). The check-in reuses the exact same Faction Transmission
//          screen (static -> shake -> typewriter) but ends in real "I DID
//          IT" / "I DIDN'T" buttons instead of a simple dismiss. Keeping
//          your word: +2 reputation. Breaking it: -3, a real trust penalty
//          on par with a conquest. Up to 3 pending obligations tracked per
//          player.
//
//          Two real bugs caught and fixed while wiring this in:
//          (1) The exact same "used before declared" ordering bug hit AGAIN
//          -- this time FACTION_VISUALS (used by the new Reputation screen
//          before its old definition point further down the file).
//          Relocated to the early-globals block alongside MY_SLOT/
//          currentScreen/transmission_is_obligation from before. This is
//          now a confirmed recurring pattern worth watching for in any
//          future .ino restructuring: new top-of-file screens referencing
//          data tables defined later in the file.
//          (2) A premature-clear bug: the obligation check-in flow (both
//          AMOLED and phone) was initially wired to clear the pending
//          transmission the instant it was shown -- before the player
//          answered. Since gameStateResolveObligation() needs the stored
//          obligation index to still be set when the YES/NO answer comes
//          in, this would have silently broken every obligation follow-up.
//          Fixed on both platforms: the transmission only clears once
//          gameStateResolveObligation() itself runs, triggered by the
//          actual button press, not by simply displaying the screen.
// v0.12.1 - Compile fix: showReputation() was declared/defined with no
//          parameters, but makeButton() requires callbacks matching LVGL's
//          lv_event_cb_t signature (void(*)(lv_event_t*)), same pattern as
//          showPlanets/showFleets/backToHome. Fixed the signature in both
//          the forward declaration and definition. Also did a full sweep of
//          every function passed to makeButton() and lv_obj_add_event_cb()
//          across the whole file (including the ternary-selected offer
//          callbacks) to confirm no other instance of this same mismatch
//          exists -- all clean.
// v0.12.2 - CRASH FIX: "board resets when I hit START GAME."
//          Root cause: GameState grew to ~3.3KB per copy once reputation
//          and obligation tracking were added (v0.11.0-v0.12.0). Several
//          functions stack-allocate a full copy via `GameState gs =
//          gameStateRead();`. The exact reported crash's call path --
//          startGameCb() -> showHome() -> updateHomeUI() -- had TWO such
//          copies alive simultaneously (showHome's `gsNav` was still in
//          scope when it called updateHomeUI(), which fetched its own
//          separate copy), very likely overflowing the old 8KB LVGL task
//          stack. Fixed at the source: updateHomeUI() and
//          checkAndShowTransmission() (6 call sites) now take GameState by
//          const reference instead of self-fetching, eliminating the
//          double-copy in every path that already held a `gs`.
//          A third instance of the same pattern was found while auditing
//          (chooseOfferAWrapCb/BCb holding their own `gs` while calling
//          showNegotiate(), which self-fetches its own) -- this class of
//          bug is architectural (most screen-builder functions self-fetch
//          GameState) and a full sweep would be a much larger refactor than
//          this pass. Rather than rush that, LVGL_TASK_STACK_SIZE was
//          bumped from 8KB to 16KB (bsp_config.h) as the general safety
//          net: even two stacked ~3.3KB copies (6.6KB) now fits with real
//          headroom to spare, which the old 8KB budget did not have.
// v0.12.3 - CRASH FIX: "board resets when a phone player joins." Same root
//          cause class as v0.12.2, but a DIFFERENT FreeRTOS task: the
//          WebServer (handling /join, /state, every HTTP endpoint) runs on
//          Arduino's main "loopTask", which is entirely separate from the
//          LVGL display task bumped to 16KB last version -- that earlier
//          fix never touched this task's stack at all. GameState is ~3.3KB
//          per copy, and sendStateJson() in web_server.cpp copies a full
//          GameState plus builds a large JSON response via repeated String
//          concatenation, all on the loopTask's stack. The Arduino core's
//          default loopTask stack is only 8KB. Since the phone polls
//          /state every 700ms, this crash fires within about a second of
//          tapping JOIN GAME -- right when sendStateJson() first has to
//          serialize the newly-active player's data.
//          Fixed via the official Espressif mechanism: SET_LOOP_TASK_
//          STACK_SIZE(16 * 1024), placed at file scope near the top of the
//          .ino (it expands to a full function definition overriding a
//          weak default in the ESP32 core, so it can't live inside
//          setup()/loop() or any other function). Verified against a live
//          web search of the current arduino-esp32 core documentation/
//          examples rather than assumed, since this exact API has changed
//          across core versions historically.
//          Also directly reviewed gameStateJoin() and the /join handler
//          itself for a more specific bug (null pointer, bounds issue) --
//          both are clean; the stack-size diagnosis is the correct one.
// v0.13.0 - VICTORY POINTS: the game now has an actual ending.
//          Design locked before coding, per real discussion: reputation and
//          diplomacy needed to feed the win condition, not just planet
//          count, or all the reputation/obligation work this session would
//          have been irrelevant to actually winning.
//          Sources (all easy to retune -- see VP_* constants in
//          game_state.h): +1 VP per planet held, awarded each round it's
//          controlled (rewards sustained control, not grab-and-lose); +2 VP
//          one-time for reaching Favorable with a faction, +3 more for
//          reaching Allied; +1 VP for fulfilling an honor-system
//          obligation; +2 VP for a decisive PvP battle win (otherwise PvP
//          didn't feed the scoreboard at all -- no territory changes hands
//          in a fleet battle). Deliberately no VP penalties; reputation and
//          battle losses already have real in-game costs without stacking
//          a second punishment.
//          Game length: any player picks 6/8/10 rounds while still in the
//          Lobby (defaults to 8). The planet-holding VP source is what
//          makes that choice matter -- a longer game gives more rounds for
//          holding bonuses to accumulate. Not tied to NUM_PLANETS=2 in any
//          way, so this scales for free once more planets are added.
//          VP is fully PUBLIC (shown to every player), unlike reputation's
//          hidden score+tier-only model -- a real, visible leaderboard via
//          a new STANDINGS screen/view on both AMOLED and phone.
//          When the round limit is reached, the game freezes (STAGE_
//          GAME_OVER) and every device auto-navigates to a Game Over screen
//          announcing the winner (highest VP; ties broken by planets
//          controlled, then Credits; a genuine draw if every tiebreaker is
//          also tied) plus final standings. Full parity, AMOLED and phone.
//          Most of the game_state.h/.cpp plumbing for this (VP fields,
//          award hooks, round-limit setter, winner-determination logic) was
//          already in place when this version's work began; this pass
//          verified every hook line-by-line for correctness, then built the
//          two pieces that were still missing: the AMOLED's Game Over
//          screen (forward-declared but never actually implemented) plus
//          its auto-trigger, and the ENTIRE phone-side implementation
//          (round-length picker, VP in the state JSON, the Standings view,
//          the Game Over view, and the /set_round_limit endpoint), none of
//          which existed on that side at all.
// v0.13.1 - Four bug reports addressed:
//          (1) "Board doesn't turn on until I open Serial Monitor" -- a
//          well-documented ESP32-S3 native USB CDC issue (confirmed via
//          multiple arduino-esp32 GitHub issues, not guessed): once the CDC
//          TX buffer fills, Serial.print()/println() BLOCK INDEFINITELY if
//          no host is reading, hanging the whole board until something
//          opens the serial port. Fixed with the official mechanism added
//          specifically for this: Serial.setTxTimeoutMs(0), which makes
//          writes non-blocking (silently dropped) instead of hanging.
//          (2) "Typing my name in the phone app doesn't work" -- a real
//          bug: poll() runs every 700ms and calls render(), which was
//          rebuilding the ENTIRE join screen via innerHTML replacement
//          every single tick -- destroying and recreating the <input>
//          element, wiping both its typed value and its focus roughly
//          every 700ms. Fixed: the join screen now only builds once
//          (skipped on subsequent polls if the input already exists),
//          since nothing there needs live server data anyway.
//          (3) "Occasionally taps are super slow after startup" -- likely
//          the SAME root cause as (1): if the Serial TX buffer fills
//          mid-session (not just at cold boot), the same blocking stalls
//          the whole main loop, which also drives touch indirectly. The
//          fix in (1) may resolve this too; flag it if it persists.
//          (4) Touch offset ("have to tap a little higher than buttons are
//          placed") -- added a tunable TOUCH_Y_OFFSET (default 20px,
//          esp32_s3_touch_amoled_1.43c.cpp) that shifts the computed touch
//          Y up to compensate. Direction verified carefully from the
//          reported symptom (taps registering below the true finger
//          position) rather than guessed -- getting the sign wrong would
//          have made it worse. Documented clearly for fast on-device
//          tuning if 20px isn't quite right, since this needs a hardware
//          feel to dial in precisely, not more guessing from code alone.
// v0.14.0 - FULL GALAXY: expanded from 2 planets to all 12 from the
//          playtest board (Hoth, Bespin, Tatooine, Dagobah, Coruscant,
//          Kessel, Oba Diah, Dathomir, Corelia, Naboo, Endor, Kijimi).
//          NUM_PLANETS is a single constant everything else was already
//          parameterized on (checked -- no hardcoded "2" assumptions found
//          anywhere), so this was mostly data entry: invented Affiliation/
//          Resources/Unique Trait/Defense for the 10 new planets (only
//          Hoth/Tatooine had data before), assigning factions with a lore
//          basis rather than arbitrarily -- Oba Diah and Kessel to Pyke
//          Syndicate (their actual canon territory), Coruscant to the
//          Empire, etc. Coruscant is deliberately the most expensive/
//          defended world (basePayPrice 8, baseDefense 6) as the galaxy's
//          crown jewel; Dagobah and Endor are cheap early targets.
//          The one real bug this surfaced: the AMOLED Planets list placed
//          buttons at a fixed 84px vertical step with no scrolling -- fine
//          for 2 planets, but 12 would have put the last button at y=1052
//          on a 466px screen, completely unreachable. Fixed by moving the
//          list into a scrollable container (LVGL scrolls automatically
//          once a container's children exceed its own size). Phone side
//          needed no changes -- it already iterated the planet list
//          dynamically and browsers scroll long lists natively.
//          Every new planet's Affiliation string was cross-checked
//          character-for-character against factionIndexForPlanet()'s
//          string matching before packaging, since a typo there would have
//          silently misassigned a planet's faction/reputation/offer pool
//          without any compile or runtime error to catch it.
// v0.14.1 - CRITICAL DEADLOCK FIX: "when either player selects a takeover
//          method, it freezes both games." Confirmed root cause:
//          gameStateCommitTakeover() takes stateMutex (a standard
//          NON-recursive FreeRTOS mutex) at the top, then its TAKEOVER_PAY
//          branch called the PUBLIC gameStatePayPrice() -- which itself
//          tries to take the SAME mutex again. Since the mutex was already
//          held by the same call chain, that second take blocked forever
//          (portMAX_DELAY), and since gameStateCommitTakeover() could never
//          finish and reach its own xSemaphoreGive(), the mutex stayed
//          locked PERMANENTLY. Every subsequent gameStateRead() or other
//          locked call -- from BOTH the AMOLED's LVGL task and the phone's
//          WebServer/loopTask -- then blocked forever too, exactly matching
//          "freezes both games" (they share one mutex-protected state).
//          Fixed properly, not just patched: split gameStatePayPrice() into
//          an internal payPriceLocked() (assumes the mutex is already held,
//          used both by gameStateCommitTakeover() directly and by the
//          public gameStatePayPrice() wrapper for external callers) --
//          matching the "Locked" naming convention already used elsewhere
//          in this file for exactly this reason.
//          Given this was a systemic bug CLASS (not just one call site),
//          every one of the 28 public gameState* functions that takes the
//          mutex internally was programmatically checked against every
//          other one for this exact nested-lock pattern (comments stripped
//          first, after an initial pass produced a false positive from
//          descriptive comment text). This was the only instance found.
// v0.14.2 - Fix: "tap to continue isn't working" on the Faction
//          Transmission screen when reputation actually changes in real
//          play. Diagnosis took real digging -- checked for stale timers
//          from re-entrant transmissions (both static-burst and typewriter
//          timers were already correctly canceled/replaced on every call,
//          ruled out), checked whether the Battle screen's own periodic
//          refresh timer could corrupt things after being displaced
//          (already correctly guarded by a currentScreen check, ruled out)
//          -- before landing on the real cause: the full-screen invisible
//          tap target (tapCatcher) is a plain lv_obj_create() object, and
//          those are SCROLLABLE BY DEFAULT in LVGL9. As a pure click
//          target (not a content viewport), leaving it scrollable meant
//          any touch with even slight finger movement -- extremely common
//          in real, organic tapping, and more likely after the touch-
//          offset fix nudges the reported coordinate during a press --
//          could get classified by LVGL as a scroll/drag gesture instead
//          of a tap, which suppresses the CLICKED event entirely. This
//          explains the specific symptom pattern (intermittent in real
//          play, not reliably broken) far better than a one-off timing
//          bug would: clean, zero-movement taps still worked, which is
//          plausibly why earlier controlled TEST COMMS testing didn't
//          surface it. Fixed with lv_obj_clear_flag(tapCatcher,
//          LV_OBJ_FLAG_SCROLLABLE). Confirmed this is the ONLY hand-built
//          full-screen tap-catcher in the whole codebase (grepped for the
//          pattern) -- every other clickable element is a proper
//          lv_btn_create() button, which isn't scrollable by default, so
//          this was a fully isolated, precise fix, not a symptom of a
//          wider issue.
// v0.14.3 - Touch calibration: switching from guessing to measuring. Two
//          rounds of feedback on TOUCH_Y_OFFSET (needing to tap above
//          buttons) confirmed the SAME symptom persists even on a
//          full-screen tap target (the transmission's dismiss catcher) --
//          which is a meaningful clue, since a full-screen target should
//          register identically everywhere if this were just a small
//          constant offset. This might be a scale mismatch between the
//          touch sensor's native coordinate range and the display's
//          0..466 range, not purely an additive shift, and guessing a
//          third number blindly isn't the right move anymore.
//          Added real diagnostic logging instead: every touch now prints
//          BOTH the raw sensor reading (tp_x, tp_y, before any correction)
//          and the final computed screen coordinate to Serial (115200
//          baud), gated by TOUCH_DEBUG_LOG (set to 0 once calibration is
//          dialed in). Plan: tap a few known reference points (center, top
//          edge, bottom edge, left edge, right edge) one at a time, note
//          the raw values Serial Monitor shows for each, and report back
//          -- the actual relationship between physical tap location and
//          raw sensor value will show definitively whether this is an
//          offset, a scale factor, or an inverted/swapped axis, and the
//          real fix can be computed precisely instead of guessed a third
//          time.
// v0.14.4 - Touch calibration fixed with REAL data, not another guess.
//          Serial captures of raw touch readings at known reference points
//          (top edge, bottom edge, center) proved this was never a
//          constant-offset problem: tapping the top edge needed only a
//          ~5px correction, but tapping the bottom edge needed a ~38px
//          correction in the OPPOSITE direction. No single flat offset can
//          satisfy both -- which is exactly why two prior offset guesses
//          each only partially worked. The touch sensor's native Y range
//          doesn't map 1:1 onto the display's 0..466 pixel range; it needs
//          to be SCALED, not just shifted.
//          Solved the actual linear relationship from the top/bottom data
//          points: screen_Y = 485 - 1.05*raw_Y (implemented as integer
//          math, 21/20, to avoid floating point). Verified against the
//          measured data: top and bottom edges now land within 1px --
//          essentially exact.
//          Honest caveat: the CENTER tap from the same data still comes
//          out about 38px off using this 2-point linear fit. This could be
//          genuine nonlinearity in a cheap capacitive touch sensor (very
//          plausible) that a straight-line fit can't capture, or the
//          "center" tap itself wasn't pixel-precise (genuinely hard to
//          hit the exact geometric center of a 1.43" round display without
//          a visual crosshair to aim at). Top/bottom (and by extension,
//          buttons near those extremes) should now feel solid; if
//          mid-screen buttons still feel slightly off, the next step would
//          be a cleaner center data point (or a few more spread-out taps)
//          to fit a more accurate curve instead of a straight line.
//          X-axis was left unchanged -- the same data showed the existing
//          plain mapping already landing within 2-8px on the right edge
//          and center, no clear scale problem there like Y had.
// v0.15.0 - PHASE A: the foundational lobby/identity rework. Every player
//          (AMOLED included -- no longer hardcoded to slot 0) now joins via
//          name -> pick a seat (Player 1-4) -> pick a faction (Rebel/
//          Empire/Pyke/Crimson Dawn, one player per faction, max 4) ->
//          Lobby. Picking a faction sets REAL starting reputation: deep
//          Allied with your own, deep Hostile with its rival, Neutral with
//          the rest. Reputation is now always visible -- the old "reveal a
//          planet's affiliation to unlock reputation" gating is gone
//          entirely, on both platforms.
//
//          Planet selection: any player can toggle which of the 12 planets
//          are actually in play for this game, from a new Lobby screen on
//          both platforms. The in-game planet lists (both platforms) now
//          filter to only show included planets.
//
//          Fleet rework: only Fleet 1 starts populated (3 ships). Fleets 2
//          and 3 start completely empty -- investing in an empty fleet
//          "founds" it (using the existing 2cr-per-ship mechanic, no new
//          system needed) and requires owning at least one planet, since
//          the new fleet gets placed there.
//
//          A real, previously-unnoticed bug was fixed along the way:
//          planetOwner[] was never properly reset for all 12 planets after
//          the v0.14.0 galaxy expansion -- only the first 2 (a leftover
//          array initializer from when NUM_PLANETS was 2). Planets 3-12
//          were defaulting to "owned by slot 0" from the moment any game
//          started, on every single game since that expansion.
//
//          AMOLED gained real on-screen text input for the first time in
//          this codebase (LVGL's keyboard + textarea widgets) for name
//          entry -- previously only phones could type a name.
//          gameStateStartGame() now requires every active player has
//          chosen a faction before the game can actually begin, and does a
//          full replay-safe reset (planets, credits, fleets, intel,
//          obligations) so the same hardware can run multiple games
//          without a reboot.
//
//          KNOWN GAP: this is a large, multi-file architectural change
//          (game_state.h/.cpp, the .ino, web_server.cpp) that has NOT been
//          compiled or run on real hardware. Balance-checked and the full
//          gameState* call graph verified across both UI files, but given
//          the size of this change, budget real time for a testing/fixing
//          pass rather than expecting it to be flawless on first flash --
//          especially the new AMOLED keyboard UI and the slot/faction
//          picker screens, which are genuinely new territory for this
//          project and the parts I'm least able to verify without
//          hardware.
// v0.15.1 - Simplification: dropped the typed-name step entirely. Your
//          "name" is now just your faction, set automatically the moment
//          you claim one ("REBEL ALLIANCE", "GALACTIC EMPIRE", etc). Flow
//          is now: JOIN GAME -> pick a seat -> pick a faction -> Lobby --
//          one screen shorter on both platforms.
//          This also removes the AMOLED's on-screen keyboard/textarea
//          entirely, which was the newest and least-tested piece of the
//          v0.15.0 build (LVGL widgets never previously used in this
//          codebase). Removed cleanly -- no dangling references, checked.
//          Player identity now naturally doubles as faction identity
//          everywhere player.name is already displayed (Lobby, Standings,
//          battle screens, negotiation, etc.) with no changes needed at
//          any of those call sites.
//          NOTE FOR LATER (not started, explicitly deferred): a character-
//          select screen is planned on top of this -- roughly a dozen
//          named characters tied to the 4 factions, giving players both a
//          personal name AND a faction, rather than the faction alone
//          standing in for identity. Faction-as-name is intentionally the
//          right scope for now.
// v0.16.0 - PHASE B, part 1. Three quick fixes plus the two most involved
//          Phase B pieces on the AMOLED.
//          Fixes: Rebel Alliance's color changed from a red too close to
//          Crimson Dawn's (0xE8402A vs 0xB0202A) to gold (0xFFC933), on
//          both platforms.
//          Garrison hiding: the live per-planet garrison ship count is no
//          longer shown as a raw number by default -- only a tier (LOW/
//          MEDIUM/HIGH, thresholds 0-2/3-5/6-8). The real number only
//          unlocks for a specific planet once you've actually LOST a
//          Fight there. New per-player hasLostAt[] tracking, reset on a
//          fresh game. The planet OWNER still sees their own real count on
//          the Invest-phase management button (that's their own info, not
//          intel leaking to an attacker), and an ACTIVE battle still shows
//          live garrison depletion (that's combat feedback, not a lookup).
//          AMOLED planet pagination: the scrolling list is gone, replaced
//          with 4 planets per page drawn as filled pie-slice wedges (like
//          a pizza cut in quarters) -- genuinely new visual territory for
//          this project. Verified the exact LVGL9 lv_draw_arc() API
//          against LVGL's own source (github.com/lvgl/lvgl lv_arc.c)
//          before writing this, given the transform_zoom/transform_scale
//          mixup from earlier this session -- confirmed the 2-argument
//          signature (layer, &dsc) with center/radius embedded in the
//          descriptor, and that a large `width` value produces a filled
//          wedge rather than a thin ring.
//          Wedge tap targets are simple bounding-quadrant rectangles, not
//          precise wedge-shaped hit regions -- LVGL's hit-testing is
//          fundamentally rectangular, and the round glass bezel already
//          clips each quadrant's far corner (which sits outside the
//          visible circle anyway), so the effective tappable area should
//          read as wedge-shaped in practice without needing custom angle-
//          based hit-testing, which would be much riskier to get right
//          without hardware to test on. Small PREV/NEXT arrows sit right
//          at the outer rim per the requested design, intentionally at
//          the edge of the touch-safe radius established earlier this
//          session -- worth specific feedback on whether they're
//          reliably tappable in practice.
//          Planet-list scroll-reset complaint is resolved as a side
//          effect -- pagination replaced the scrolling list entirely, so
//          there's no scroll position to lose.
//
//          NOT YET DONE (remaining Phase B work): phone-side pagination
//          (a conventional 2x2 grid, "pizza slices" being a round-screen-
//          specific idea), phone-side garrison-hiding display, and the
//          reveal-button-becomes-text rework on both platforms.
// v0.16.1 - Planet wedges now have the same 0x38BDF8 blue border every
//          other button uses. Arcs don't have a native border concept
//          like rects do, so this draws each wedge twice: a full-size
//          wedge in the border color first, then a slightly smaller wedge
//          (radius inset ~3px, angle inset ~1 degree per side) in the
//          ownership fill color on top -- the border color shows through
//          as a ring on the outer curve and a thin line along both
//          straight edges. The angular inset (rather than a constant
//          linear width) means the border thins slightly toward the
//          center -- an approximation, not a pixel-perfect border, but
//          should read clearly as bordered at a glance and gives a nice
//          blue "spoke" separating all 4 wedges where they meet.
// v0.17.0 - PHASE B COMPLETE. Finished the three remaining pieces.
//          Phone-side pagination: the flat planet list is now a 2x2 grid,
//          4 planets per page with PREV/NEXT, mirroring the AMOLED's
//          pagination logic (same planetIncluded filtering, same 4-per-
//          page math) in a shape that suits a rectangular screen instead
//          of pie wedges.
//          Phone-side garrison hiding: the JSON no longer even SENDS the
//          raw garrison number unless it's actually safe to show (you've
//          lost a Fight there, or it's your own planet) -- not just hidden
//          client-side, genuinely absent from the payload.
//          Real bug caught and fixed on BOTH platforms while wiring this
//          up: the garrison display only unlocked the real number on a
//          loss, but never accounted for it being the player's OWN
//          planet -- an owner looking at their own world during
//          Resolution phase would have seen "MEDIUM" instead of their own
//          real count, even though they already know it (they're the ones
//          who built it up). Fixed on both the AMOLED and phone.
//          Reveal-button rework (both platforms): buttons are now bigger
//          and full-width, and once revealed, the value now occupies that
//          same freed space in a bordered card instead of squeezing into
//          a small corner label -- genuinely uses more of the screen, not
//          just a mechanical button-to-text swap in the same cramped
//          space. AMOLED row height adapts based on whether the owner's
//          invest-defense button also needs room below (54px with it,
//          66px without), keeping everything within the touch-safe
//          corner-distance bounds established earlier in this project.
//          Phone gained a matching progress indicator on the invest
//          button ([X/8]), previously AMOLED-only.
//          This closes out Phase B. Next up per the agreed plan: Phase C
//          (1.5x text scaling everywhere, Home screen rework).
// v0.17.1 - Fix + diagnostic for "can't select PLAYER 2 -- OPEN on the
//          phone, for any of them." Ruled out several server-side
//          hypotheses first (STAGE_LOBBY defaults correctly to 0 at
//          construction, no scoping issue hiding pickSlot from inline
//          onclick handlers, the query-string-on-POST pattern already
//          works fine for other endpoints) before finding a real,
//          confirmed structural bug: the seat-picker and faction-picker
//          screens were rebuilding their ENTIRE DOM on every 700ms poll
//          tick, unconditionally -- the exact same bug class that broke
//          the name-input field earlier in this project (a DOM element
//          torn down and recreated while the user is mid-interaction with
//          it can silently drop the gesture). Fixed with a snapshot-based
//          guard on both screens: only rebuild when the underlying player/
//          faction data actually changes, not on every tick regardless.
//          Given genuine uncertainty about whether this was the WHOLE
//          story (the reported symptom -- fails for every open slot,
//          every time -- reads more like a deterministic bug than a
//          timing race), also added a visible on-page diagnostic: both
//          pickSlot() and pickFaction() now show the raw server response
//          (or fetch error) directly on the phone's screen. This mirrors
//          the earlier Serial Monitor diagnostic approach for touch
//          calibration, adapted for a context where phone browser DevTools
//          access is a much higher barrier than reading Serial output.
//          If the rebuild-timing fix doesn't fully resolve this, the
//          debug text will show exactly what the server actually returned
//          (or didn't) on the next attempt, rather than guessing again.
// v0.17.2 - Pay now has a confirmation step, on BOTH platforms. Checked
//          the AMOLED first before assuming this was phone-specific --
//          it had the exact same gap (a single tap on PAY committed real
//          Credits instantly, no way back). Fight and Negotiate already
//          naturally lead into further screens (a battle, or offer
//          choices) before anything's final; Pay was the only one of the
//          three methods that went straight from tap to committed
//          transaction with zero intermediate step.
//          AMOLED: new SCR_CONFIRM_PAY screen between the method-choice
//          screen and the actual commit -- shows the price, the planet,
//          and current Credits, with CONFIRM PAYMENT and BACK. Since this
//          is a real distinct screen state (not a flag), there's no stale-
//          state risk -- re-entering a takeover always goes through the
//          method-choice screen first.
//          Phone: added a client-side payConfirmPending flag showing the
//          same confirm sub-view before calling the actual commit.
//          Explicitly reset on both starting a new takeover session and
//          cancelling one, since a boolean flag (unlike the AMOLED's
//          distinct screen) could otherwise persist stale across a later,
//          different takeover attempt and skip straight to confirm
//          without the player ever tapping PAY on that attempt.
// v0.17.3 - Faction-colored planets, made genuinely faction-based on both
//          platforms. Checked the AMOLED's existing wedge coloring before
//          replicating it to phone, since "highlighted by the color of
//          the syndicate that owns it" wasn't actually what was
//          implemented -- the wedges used a generic mine/rival/
//          uncontrolled scheme, not real per-faction colors. Upgraded
//          both to genuinely reflect the owning player's faction (their
//          faction doubles as their identity now, so this ties planet
//          control to "whose syndicate holds this").
//          Design note: the faction color went on the BORDER, not a full
//          background fill -- Rebel Alliance's bright gold as a full
//          wedge/button fill would have made light text unreadable
//          against it specifically. Border color carries the faction
//          signal (vivid, no text-contrast concern); fill/background
//          stays a safe dark tone with just a subtle warmer highlight for
//          the viewer's own planets. Same design language, same colors,
//          on both the AMOLED's pie wedges and the phone's planet grid.
// v0.18.0 - PHASE C, part 1: foundation + Home screen rework.
//          Verified a real technical concern before writing any of this:
//          does LVGL's transform_scale (the "zoom trick" already proven on
//          the Faction Transmission/Game Over screens) create a mismatch
//          between where a button visually appears and where it's actually
//          tappable, if applied broadly? Checked against LVGL's own docs --
//          transform properties are explicitly "visual-only... does not
//          affect layout... clickable area." A button's own tap target
//          stays based on its real size regardless of the label's scale
//          inside it. The actual constraint is just sizing buttons
//          generously enough to contain the bigger text without clipping,
//          not a touch-accuracy risk -- confirmed before treating this as
//          safe to apply broadly, not assumed.
//          Built ONE shared, reusable LVGL style object (style_big_text,
//          384 = 1.5x in LVGL's 1/256 transform_scale unit, matching the
//          value already validated on the transmission screen) rather
//          than repeating scale+pivot property calls at 100+ individual
//          label creation sites. Applied it inside makeButton() itself --
//          all 23 button call sites across the whole app get 1.5x text
//          automatically, no per-site changes needed.
//          Home screen fully reworked: bigger LOCK IN button, 1.5x phase
//          info text, and nav buttons sized and repositioned to actually
//          use the screen's available space (the original complaint --
//          buttons crammed into about a third of the screen with small
//          text everywhere else). Every new position re-verified against
//          the touch-safe corner-distance bound established earlier this
//          project; the 3-across Resolution-phase row sits close to the
//          practical limit of what 3 side-by-side targets can fit on a
//          round screen without the outer two creeping past it.
//
//          REMAINING PHASE C WORK: applyBigText() only reaches buttons
//          built via makeButton() -- the many screens with custom-built
//          labels (Lobby, seat/faction pickers, Planet Detail, Battle,
//          Standings, Reputation, etc.) still need it applied manually,
//          screen by screen. Phone-side text scaling (a separate, simpler
//          CSS font-size change, since the phone doesn't use LVGL at all)
//          hasn't been started.
// v0.18.1 - FREEZE FIX (hypothesis-based, needs confirmation): "clicking
//          START GAME freezes before Home can appear." Home is the most
//          heavily-transformed screen built so far -- roughly 7 widgets
//          using the 1.5x scale transform simultaneously (3 hand-built
//          labels + 4-5 nav buttons), far more than any other screen
//          tested (Select Planets, with just one transformed button,
//          confirmed working). Each transformed widget needs an extra
//          offscreen-buffer rendering pass in LVGL -- this many at once
//          may be a real resource/timing problem on this hardware,
//          matching the general class of issue this exact display has
//          hit before under load (the GameState-stack-overflow crashes
//          earlier this project).
//          Reverted Home specifically to a known-safe state rather than
//          guessing at a different fix blind: removed the scale from its
//          3 hand-built labels, and added makeButtonPlain() (same as
//          makeButton() but without the transform) for its 5 nav
//          buttons. The bigger button SIZES and REPOSITIONING from
//          v0.18.0 are kept -- those aren't suspected to be the issue,
//          only the simultaneous transform count. Every other screen's
//          use of the scaled makeButton() is untouched, since Select
//          Planets already confirmed that specific pattern works fine
//          with a single transformed button.
//          Honest confidence note: this is a reasoned hypothesis based on
//          the available evidence (Home vs. Select Planets contrast,
//          prior history of resource-related freezes on this hardware),
//          not a confirmed root cause -- I can't verify LVGL's internal
//          transform-rendering resource usage without a live device. If
//          the freeze persists after this, that would point away from
//          "too many simultaneous transforms" and toward something else
//          entirely, which would need fresh diagnosis rather than another
//          guess in the same direction.
// v0.19.0 - PHASE C, TECHNIQUE SWITCH: from the transform_scale "zoom
//          trick" to a genuinely bigger default font. Confirms the freeze
//          hypothesis from v0.18.1 was on the right track -- a real font
//          has no per-widget rendering cost, so there's no longer any
//          "how many transformed widgets is too many" question to manage
//          at all going forward.
//          REQUIRES a one-time local change, made to the user's own
//          lv_conf.h (not a project file): LV_FONT_MONTSERRAT_20 enabled,
//          LV_FONT_DEFAULT switched from Montserrat 14 to 20. Since
//          nothing in this codebase explicitly overrides its font, EVERY
//          label across the entire app -- every screen, not just the ones
//          already swept -- now renders at the new bigger size
//          automatically, genuinely achieving "1.5x everywhere" (20/14 =
//          1.43x) with zero remaining per-screen sweep needed.
//          Firmware-side reconciliation this required: retired the
//          style_big_text/applyBigText/ensureBigTextStyle machinery from
//          v0.18.0 entirely (redundant now, would have DOUBLED the
//          transmission body text's size on top of the new bigger
//          default -- 20pt * 1.5x transform = ~30pt-equivalent). Merged
//          makeButton()/makeButtonPlain() back into one plain function --
//          no freeze risk left to guard against. Recalibrated the 3
//          remaining PRE-EXISTING decorative transform_scale usages
//          (Game Over banner, faction portrait glyph, transmission name
//          tag) by dividing each by 20/14 to preserve their original
//          absolute visual size as intentional extra emphasis on top of
//          normal text, rather than let them compound into being even
//          bigger than before. transmission_body_label's transform was
//          removed entirely (it existed specifically to make dialogue
//          text "1.5x readable," which the new default now achieves on
//          its own) -- its wrap width also fixed from a narrow 230px
//          "pre-zoom" value to a real 380px, since the old value only
//          made sense as an input to a zoom that no longer exists.
//
//          NOT YET VERIFIED: text wrapping/overflow on screens with tight
//          width constraints that were originally tuned assuming 14pt
//          text -- a real possibility given the ~43% size increase, and
//          not something checkable without seeing it rendered. Worth a
//          broad look across screens, not just Home, when testing this.
// v0.19.1 - BUG-FIX PASS 1/5: takeover sessions are per-player. The
//          session (planet, committed method, two negotiation offers) was
//          a single global, so a second player tapping TAKE OVER PLANET
//          silently replaced the first player's session -- the first
//          player fell out of their flow, or saw someone else's offers.
//          Now one session per slot (GameState arrays indexed by slot;
//          open iff takeoverPlanet[slot] >= 0), closed via one
//          clearTakeoverLocked() helper. Pay/Negotiate messages moved off
//          the shared battle resultText into per-slot takeoverResult, so
//          one player's "Not enough Credits" can't show on another's
//          screen. Also: a SUCCESSFUL Pay now closes the session (it was
//          left open -- invisible under the global, but per-slot it would
//          have kept the phone on the approach menu for a planet you just
//          bought). A FAILED Pay now returns the AMOLED to the approach
//          screen showing why (was: silently back to planet detail),
//          matching the phone. Boot Serial now prints sizeof(GameState)
//          (grew ~420 bytes) to track stack-copy headroom.
// v0.19.2 - BUG-FIX PASS 2/5: a planet Fight can no longer overwrite a
//          battle in progress. There is one global battle slot;
//          gameStateStartPvpBattle() already refused to start while
//          battleActive, but startBattleLocked() (the planet-Fight path)
//          never checked, so a second player's FIGHT replaced the running
//          battle -- the original attacker's taps were then rejected and
//          their ship losses never written back. Same root cause let a
//          double-tapped FIGHT restart your OWN battle, re-copying your
//          fleet from its saved state and undoing mid-battle losses.
//          startBattleLocked() now returns false if a battle is active;
//          the refused Fight resets the method and sets takeoverResult
//          ("A battle is already in progress."). AMOLED commitFightCb only
//          enters the battle screen if the battle is really yours, else
//          re-shows the approach screen with the reason. Phone needed no
//          change (it renders from state, and shows takeoverResult).
// v0.19.3 - BUG-FIX PASS 3/5: no Fight without living ships. Nothing
//          checked fleet strength before a planet battle (the PvP path
//          did): firstAvailableFleetLocked() falls back to fleet 0 even
//          when every fleet is empty, and the zero-ship battle still
//          rolled. Worse than the logged symptom -- with the +1 attacker
//          bonus it won the first exchange ~58% of the time, so an empty
//          fleet could destroy garrison ships (permanently lowering the
//          planet's garrison) or capture a 1-garrison planet outright
//          (Dagobah); otherwise it "lost" instantly and got hasLostAt's
//          garrison intel for free. startBattleLocked() now returns a
//          refusal reason (nullptr = started) and refuses with "You have
//          no ships left to fight with."; shown via the v0.19.2 refusal
//          path on both platforms, no UI change needed.
// v0.19.4 - BUG-FIX PASS 4/5: /state JSON is built from one snapshot and
//          escaped properly. Three root causes:
//          (1) The only way to ask "what tier / what price / who won" was
//          the locking gameState* getters, so sendStateJson() -- despite
//          taking a gameStateRead() snapshot -- called ~30 of them, each
//          re-locking and reading LIVE state: one response could mix two
//          instants (e.g. a pay price computed after the snapshot's
//          credits), plus ~30 extra mutex round-trips per phone per 700ms
//          poll contending with the LVGL task. Added pure snapshot*
//          queries (snapshotGarrisonTier/ReputationTier/PayPrice/
//          AdjustedOfferCost/WinnerSlot) that compute from a GameState
//          copy; the public getters and the locked internals now call
//          them too (payPriceLocked/tierAdjustedCostLocked folded in), so
//          each rule has one implementation. sendStateJson() uses only
//          its snapshot, and resolves the caller's slot from it as well.
//          (2) Escaping was replace("\"","'") on some fields and nothing
//          on others (names, planet names, characteristics); a backslash
//          or control char would break JSON.parse on every phone. New
//          jsonStr() does RFC 8259 escaping for every string field.
//          (3) /join accepted ?name= into state unfiltered, which other
//          phones render via innerHTML. Neither client has sent a name
//          since v0.15.1 (name = faction), so the arg is simply no longer
//          read. Phone JS unchanged; JSON shape unchanged.
// v0.19.5 - BUG-FIX PASS 5/5: obligation check-ins no longer slip a round.
//          Each player has one transmission slot, and
//          checkObligationsDueLocked() ran ONLY at the start of a new
//          round, queuing at most one check-in and skipping entirely if
//          any transmission was already pending. Its own comment assumed
//          resolving one re-ran the scan "via the next tick" -- nothing
//          did. So a second check-in due the same round, or any check-in
//          blocked by a pending reputation transmission, waited a whole
//          extra round. Now gameStateClearTransmission() and
//          gameStateResolveObligation() re-run the scan, so the next due
//          check-in follows immediately (stage-guarded to PLAYING). Phone:
//          poll() no longer calls showTransmission() while the overlay is
//          open -- normal transmissions are acked the moment they're
//          shown, so the server now queues the next check-in while the
//          player may still be reading; it waits until they dismiss. (This
//          also stops any new transmission replacing one mid-read, which
//          could happen before.) AMOLED already only checks when not on
//          the transmission screen. Side effect: the "check-in overwritten
//          by a reputation transmission" edge case (MANIFEST Deferred) now
//          resurfaces right after that transmission instead of next round.
// v0.19.6 - Phone seat picker respects game stage. Found on hardware:
//          a phone opened after START GAME still listed empty seats as
//          OPEN, and tapping one showed a bare 'Result: {"slot":-1}'.
//          The server refusing mid-game joins is by design
//          (gameStateJoinSlot only accepts new players in the Lobby) --
//          the picker just never checked s.stage, and its rebuild
//          snapshot didn't include stage, so it wouldn't have updated
//          when the game started anyway. Now: stage is in the snapshot,
//          and outside the Lobby an unjoined phone sees "GAME IN
//          PROGRESS -- New players can only join from the Lobby."
//          Reconnecting phones are unaffected (they resume their slot via
//          yourSlot before this branch). Phone JS only.
// v0.20.0 - PHASE D, PART 1/3: animated battle scene, both platforms.
//          Each exchange now plays a ~1.5s scene (tap to skip): dice
//          tumble and land (winner's die gets a gold border; planet
//          attackers show the raw die + "+1"), laser bolts fly from the
//          winning side, the hit ship / garrison marker blinks white then
//          bursts into fragments with a screen shake. Planet battles are
//          fought in orbit above a rotating pixel-art planet with a
//          per-planet palette; PvP is open space. A result banner
//          (PLANET CAPTURED / FLEET DESTROYED / VICTORY / DEFEAT /
//          WITHDREW) closes the battle. Ships are original triangles in
//          faction colors; garrison markers are a neutral orange (the
//          planet's Affiliation is hidden intel, so no faction color).
//          State: GameState.battleExchange (bumped per exchange, reset at
//          battle start) + lastLossIdx (which ship died). Every device
//          animates when the counter changes -- roll values alone can't
//          show "new exchange". /state adds both plus per-ship
//          battleAtkAlive/battleDefAlive.
//          AMOLED: the whole scene is ONE object painted by a DRAW_MAIN
//          callback (the star-field/static/wedge technique) -- no
//          transform_scale, no pixel buffer; a 33ms timer invalidates it
//          while animating. The planet is a 28x28 block grid recomputed
//          once per 120ms rotation step, drawn as merged same-color runs.
//          Draws from a BattleView snapshot taken when the exchange is
//          detected, so a mid-animation update can't tear it. Result text
//          (now Montserrat 14, <=2 lines) is held back until the exchange
//          lands. Buttons moved to y326 (150x50), corners ~215px from
//          center (touch-safe). Header uses the battle's target planet,
//          not the last planet this device viewed.
//          Phone: same scene/timeline/palettes on a <canvas>; the battle
//          card is built once per battle and only its text/buttons update
//          on poll (Convention 8) -- it used to rebuild every 700ms.
//          Phone JS was syntax-checked and the scene rendered offline in
//          headless Edge with a fake /state (contact sheet of every beat).
//          DIAGNOSTICS: after each exchange the AMOLED prints
//          "[BATTLE] exchange N: F frames in T ms, draw cb avg/max us,
//          planet calc max us" -- ~45 frames per 1.5s = smooth. Much
//          lower = the scene is too heavy for the panel; send the line.
// v0.20.1 - PHASE D: combat modifiers + battle briefing (both platforms).
//          RULE CHANGE: the flat +1 attacker bonus on planet battles is
//          gone (user: an unexplained bonus felt wrong -- found when a 3
//          showed as 4 in the v0.20.0 test). Planet battles are now d6 vs
//          d6 plus faction modifiers, fixed at battle start:
//            Rival support  -- attacker Favorable+ with the planet
//                              faction's rival: +1 attacker.
//            Local resentment -- attacker Hostile with the planet's own
//                              faction: +1 garrison.
//          Both only apply once the attacker has revealed the planet's
//          Affiliation (you can't call in help against an unidentified
//          faction; also keeps hidden intel hidden). Values are named
//          constants (BATTLE_MOD_*) for playtest tuning. PvP stays plain
//          d6 both sides -- reputations are per player, so no faction
//          gets an inherent edge against another player.
//          Result text shows die+mod ("You 3+1 vs Def 4") so it always
//          matches the die face; the scene's die tag shows the real
//          modifier for either side (GameState.battleAtkMod/DefMod).
//          BATTLE BRIEFING: FIGHT now opens a briefing (CONFIRM FIGHT /
//          BACK) showing garrison tier (exact only if you've lost there),
//          your revealed intel, both roll formulas with their sources,
//          and a FLEET PICKER -- planet Fights used to auto-send the first
//          fleet with ships. New gameStateCommitFight(slot, fleet);
//          /takeover_commit takes &fleet=. Pure
//          snapshotPlanetBattleMods() drives briefing, battle, and JSON
//          (fightMods) alike. Also gives the AMOLED a place that shows
//          your revealed intel during Resolution (its planet detail
//          doesn't; the phone's always did). Phone briefing rebuilds only
//          when its content changes (it has a picker -- Convention 8).
// v0.20.2 - PHASE D: commitment & withdrawal rules (both platforms).
//          Agreed with the user to stop "peek and switch" and "chip,
//          retreat, reinforce, repeat":
//          COMMITMENT LOCK -- CONFIRM FIGHT, or negotiation offers being
//          revealed, sets Player.takeoverLocked[planet]: that planet is
//          closed to you (no new takeover of any kind) until next round.
//          The briefing's BACK stays free (it only shows known intel).
//          Leaving a negotiation now goes through a "LEAVE NEGOTIATION?"
//          confirm spelling out -1 rep + the lock. Planet detail shows
//          "You've made your move here this round" instead of TAKE OVER.
//          ONE ATTACK PER FLEET PER ROUND -- Player.fleetSpent[fleet] is
//          set when a fleet attacks (planet or PvP; defending doesn't
//          spend). A second fleet can still follow up the same round,
//          which keeps the multi-fleet-assault idea. Briefing greys out
//          spent fleets; PvP auto-picks the first ready fleet.
//          RETREAT UNDER FIRE -- withdrawing is one last exchange in which
//          only the side that stays can hit (lastRetreatSide tells the
//          scene; a retreating side that out-rolls just escapes). PvP:
//          both withdrawing = no shot. Losing the last ship this way
//          counts like losing the fight (hasLostAt / decisive-win VP).
//          WITHDRAW is a two-tap confirm (arms for 4s, explains the cost).
//          DESTROYED FLEETS LEAVE THE BOARD -- a fleet with no living
//          ships (never founded OR fully destroyed) can only be (re)built
//          by a player who owns a planet; rebuilding a destroyed fleet
//          starts it fresh (dead hulls cleared, full capacity). Fleet
//          screens show REBUILD/FOUND FLEET or explain why not.
//          Both round flags reset at each new round and at game start.
//          Fixed along the way: gameStateStartPvpBattle() failed silently
//          (the AMOLED then showed an empty battle screen). It now returns
//          a reason; /pvp_attack returns {ok:false, reason}; both UIs show
//          it. The invest button also used to vanish for a destroyed fleet
//          whose 6 slots were used up.
//          Phone screens verified offline (tools/phone_check.sh render:
//          new harness modes briefing/leave/armed/locked + retreat rows).
// v0.20.3 - 1.75" BOARD BRING-UP (Waveshare ESP32-S3-Touch-AMOLED-1.75-B),
//          first step of the 2026-09-27 rollout order. No gameplay change.
//          New board layer: board_select.h picks the board (SF_BOARD =
//          SF_BOARD_AMOLED_175 by default, or 143); bsp_config.h holds
//          per-board pins; the BSP file was renamed
//          esp32_s3_touch_amoled_1.43c.* -> board_bsp.* and keeps the shared
//          LVGL glue (flush, tick, task, lock, brightness) with #if SF_BOARD
//          sections only for what differs: panel init commands + column gap
//          (CO5300: gap 6 + its FE/19/1C page setup; SH8601: gap 8), the
//          1.43" panel-enable pin, touch (CST9217 @0x5A: write 0xD000, read
//          15 bytes, ack 0xD000 0xAB, byte6==0xAB valid, 12-bit x/y; vs the
//          FT-series @0x38), touch mapping (1.75: both axes mirrored, per
//          Waveshare's BSP), and the 1.43" battery ADC (1.75 battery is on
//          the AXP2101 -- stubbed, unused by the game). The CO5300 runs on
//          the existing SH8601 QSPI driver (same command framing) -- no new
//          display driver. All facts from Waveshare's repo (HARDWARE_
//          REFERENCE.md, pin_config.h, the LVGL-9 esp-brookesia BSP,
//          SensorLib TouchDrvCST92xx). Boot Serial now prints the board.
//          Verified on hardware: display correct first try (picture
//          identical to the 1.43), touch lands correctly (user-confirmed),
//          Wi-Fi AP + web server up. The 1.43 build is byte-for-byte the
//          same code path (size within a few bytes of v0.20.2).
// v0.21.0 - UI REFRESH step 1/7: foundation + Home hub + Standings, both
//          platforms (built overnight 2026-09-28 at the user's request;
//          compile-verified, phone rendered offline -- NOT yet flashed).
//          AMOLED UI kit (.ino, "UI KIT" section): theme colors (UI_*),
//          per-faction ring/text colors, uiCircleButton / uiPill /
//          uiBackButton / uiLabel, the round-progress rim (uiAddRim: one
//          arc per round, past dim, current bright), and original faction
//          insignia drawn live with LVGL lines/discs (uiDrawInsignia /
//          uiInsignia: Rebel sunburst, Imperial hexagon, Pyke diamond,
//          Crimson Dawn crescent). Home hub rebuilt per the mockup: rim
//          ring; top status cap (round / phase / CR / VP rank) is a hidden
//          tap zone into Standings; GALAXY center circle with your planet
//          turning inside (reuses the battle scene's planet renderer);
//          FLEETS + FACTIONS side circles; LOCK IN pill with "N of M ready".
//          During Resolution a red ATTACK circle (PvP) appears lower-right
//          -- placement not covered by the mockups, flagged for review.
//          Standings rebuilt: rank, insignia, faction, VP, planets, your
//          row in your color. Phone: theme via CSS variables (--accent =
//          your faction color) re-colors every screen; slim top status bar
//          on all in-game screens; Home hub (round bar, stats, 2x2 tiles
//          GALAXY/FLEETS/FACTIONS/STANDINGS, ATTACK in Resolution, LOCK IN
//          pill); planet list moved behind the GALAXY tile (galaxyMode);
//          Standings rebuilt with SVG insignia. Font stays Montserrat --
//          Oxanium needs LVGL's lv_font_conv (Node.js), a user decision.
// v0.21.1 - UI REFRESH step 2/7: round/phase banner + lock-in confirm, both
//          platforms (built overnight 2026-09-28; compile-verified, phone
//          rendered offline -- NOT yet flashed).
//          ROUND BANNER: whenever the round or phase changes (and at game
//          start) a full-screen banner shows "ROUND n / N" + "<PHASE>
//          PHASE" over animated hyperspace streaks (18 streaks, dashes
//          racing outward on a staggered ~1.1s loop), then Home after a tap
//          or ~3s. AMOLED draws the round number as big segmented digits
//          with thick lines (largest font is 20px; a transform_scale label
//          would cost an offscreen pass every animated frame). It never
//          interrupts battles/dialogs/transmissions -- it waits for a calm
//          screen (Home, lists, lobby) -- and it runs BEFORE any pending
//          transmission, so a check-in due this round follows the banner.
//          Phone: #banner overlay with CSS-animated streaks; never over a
//          battle you're in; transmissions wait while it's open.
//          LOCK-IN CONFIRM ("End turn?"): only when something is left on
//          the table -- 2+ Credits, a ready fleet in Resolution, or
//          negotiation offers open -- listing what's left, with STAY /
//          LOCK IN. Otherwise LOCK IN is still one tap.
//          Also: lockInCb keeps a single GameState copy on the LVGL stack.
//          NOT in this step (needs decisions): the round recap / Galactic
//          News (needs an event log in GameState) and income on the banner
//          (income isn't in the rules yet -- rollout step 3).
// v0.21.2 - UI REFRESH step 3/7: the Galaxy planet ring, both platforms.
//          AMOLED: the v0.20-era wedge pages (4 planets/page + tiny PREV/
//          NEXT arrows) are gone. Every planet in play now sits on one ring
//          around the bezel: 76px circles at radius 168 (farthest bounding
//          corner ~222px, Convention 5), each with its pixel-planet art and
//          a name tag. Border = the holder's faction color (hairline if
//          unclaimed); your own planets get a thicker border and tinted
//          fill. Center: GALAXY, a phase hint + your Credits, the back
//          button, and "ring color = who holds it". Colors refresh live if
//          a planet changes hands while you're looking. Tap -> the planet
//          page (still the old layout; phase-aware rebuild is next).
//          Why baked images: the star field invalidates the whole screen
//          every 180ms, so 12 planets drawn block-by-block would be ~1500
//          rect draws per frame. Each planet is rendered once into a 56x56
//          ARGB8888 image (12.5KB, PSRAM) with the shared renderer, frozen
//          at a per-planet rotation step.
//          Font: name tags use Montserrat 12 (widest, CORUSCANT, ~79px; 14
//          was too wide). It's a project-local copy of LVGL's own font file
//          (sf_font_montserrat_12.c, symbol renamed) so it needs no change
//          to the shared lv_conf.h.
//          Phone: the Galaxy grid becomes 3-column cards (pixel planet, name,
//          holder tag, same border rules, same frozen art), rebuilt only
//          when its HTML changes (Convention 8).
// v0.21.3 - Galaxy ring polish from the first hardware look at v0.21.2
//          (user: "very clickable and usable", no frame drops, but "a tiny
//          bit small"; CORUSCANT cut off; legend unnecessary).
//          Bigger circles, sized by planet count: 84px circles / 64px art
//          at radius 174 for 11-12 planets (~6px gaps), 92px / 70px at
//          radius 170 for 10 or fewer. Outer edge ~216px from center, inside
//          the rim ring. Why this is allowed now: Convention 5's bounding-box
//          corner rule over-constrains ROUND buttons -- their box corners are
//          empty; only the visible circle has to sit on the glass.
//          Name tags moved from the button to the screen: LVGL caps a
//          content-sized label at its parent's width, which clipped the
//          ~79px CORUSCANT inside the 76px circle. Tags aren't clickable, so
//          taps on them fall through to the circle.
//          "RING COLOR = WHO HOLDS IT" legend removed; GALAXY/hint/back
//          re-centered. Ring art now bakes per (planet, size), each its own
//          image descriptor, so LVGL's image cache never sees reused pixels.
// v0.21.4 - UI REFRESH step 4/7 (part 1): the phase-aware Planet page for
//          Invest and Movement, both platforms (canvas row "Planet page by
//          phase", approved 2026-09-28).
//          Layout: the planet turning up top (the Home planet renderer at
//          4px blocks = 112px; spinPlanetDrawCb now takes the planet index
//          as user_data and sizes its blocks from the object), the name,
//          and who holds it + garrison tier (exact if yours or you've lost
//          a Fight there), then by phase:
//            Invest, your planet: GARRISON n / 8 + a big INVEST DEFENSE pill
//              (greyed when full or short of Credits). The INTEL circle on
//              the right flips to the intel tiles and back (AMOLED; the
//              phone has room to show both).
//            Invest, other planets: DISCOVER INTEL -- 2x2 tiles; revealed
//              ones show the value (Affiliation also shows your reputation
//              tier with that faction), hidden ones are REVEAL + cost.
//            Movement: the same tiles, read-only (??? for hidden).
//          Behavior change: revealing intel is now an Invest-phase action
//          (it was also offered in Movement), as mocked -- Movement is for
//          moving pieces on the board. The rules code still allows it in
//          any phase; only the UI stopped offering it.
//          Resolution keeps the old takeover layout until part 2 (v0.21.5:
//          FIGHT / NEGOTIATE / PAY on the planet page itself).
//          Phone: same structure; planet page rebuilt only when its HTML
//          changes (shares the Galaxy's stableHtml guard). Harness modes
//          pinvest / pown / pmove; harness banner suppression now follows
//          the fake state's round/phase.
// v0.21.5 - Fix: AMOLED planet-page intel tiles all landed at the top-left
//          corner, off the glass (user, first look at v0.21.4: "I can barely
//          see the bottom left corner of the bottom left button").
//          Cause: in LVGL 9 lv_obj_set_pos/set_size are stored as LOCAL
//          STYLE properties, and planetIntelTile() called uiStyleButton()
//          -- which starts with lv_obj_remove_style_all() -- AFTER placing
//          the tile, wiping its position and size. Every other UI-kit
//          caller styles first (audited: no other object has the order
//          reversed). Rule: style an object before positioning/sizing it.
//          Serial log of the v0.21.4 session was clean (no resets).
// v0.21.6 - UI REFRESH step 4/7 (part 2): the Resolution planet page --
//          FIGHT / NEGOTIATE / PAY right on the planet (mockup v1 "Planet +
//          approach", approved 2026-09-27: "yes, 100%"), both platforms.
//          The separate CHOOSE APPROACH screen (showTakeover / the phone's
//          approach card) is gone: one tap fewer to any takeover, which was
//          the user's main complaint about fighting planets (~7 taps).
//          Each button opens the takeover session and jumps to that
//          approach's next step: FIGHT -> battle briefing, NEGOTIATE -> the
//          offers (seeing them still commits you, v0.20.2), PAY -> the price
//          confirm. BACK from the briefing / pay confirm now cancels the
//          still-uncommitted session (free -- only a negotiation you've seen
//          costs reputation) and returns to the planet page. A refusal (a
//          battle already running, not enough Credits, no ships) lands back
//          on the planet page with the reason in red. Leaving the planet page
//          closes any uncommitted session.
//          Layout: intel as four read-only chips (AFFIL. / RES. / TRAIT /
//          DEF.; the trait says KNOWN -- its full text is on the briefing),
//          a status line (the refusal, else "<faction> see you as <tier>"),
//          then three 96px circles. Your own planet: YOU HOLD THIS WORLD.
//          Committed this round: the "made your move" note.
//          Phone: the same on the planet page; /state now carries each
//          planet's asking price ("pay") so PAY n CR shows before a session
//          exists. Harness modes pres / prefused.
// v0.21.7 - UI REFRESH step 4/7 (part 3): the Battle Forecast, both
//          platforms (mockup "Battle forecast"; FE-style odds approved
//          2026-09-27). The text briefing becomes: BATTLE FORECAST + planet;
//          two columns -- your fleet vs the garrison -- each with its roll
//          formula (d6 +1) and the modifier's source (Rebel support /
//          Imperial resentment / no modifier); a VS medallion; the exact
//          WIN / TIE / LOSE odds for one exchange with a three-part bar;
//          the fleet picker (ready / spent / destroyed / not built); and a
//          red ENGAGE pill with a back circle. Odds are exact over the 36
//          d6 pairs; WIN and LOSE are rounded and TIE absorbs the rounding
//          (even fight = 42 / 16 / 42). Nothing new is revealed: garrison
//          count still only if you've lost a Fight there. The revealed-intel
//          list moved off this screen -- it's on the planet page one tap back.
//          Mechanics unchanged.
// v0.21.8 - Rule fix (user, testing v0.21.7): the garrison tier (LOW /
//          MEDIUM / HIGH) is now hidden until you reveal the planet's
//          Defense -- "Garrison ???" on the planet page and the forecast.
//          It was always shown, which gave away for free the thing the
//          Defense reveal is supposed to buy. Still known without a reveal:
//          your own planets, and planets where you've lost a Fight (you saw
//          the exact count). One rule, snapshotGarrisonTierKnown() in
//          game_state.cpp, used by every display; /state sends
//          garrisonTier -1 when hidden, so the phone never receives it.
//          (During a battle itself the scene still shows the garrison's
//          ships -- you're fighting them.)
// v0.21.9 - Battle Forecast redesign, both platforms (user on v0.21.7: "a
//          little small... lots of words"; redesign approved 2026-09-28).
//          Pictures instead of sentences: your fleet as a row of ship
//          triangles in your color; the garrison as squares (exact count
//          known), a 3-bar strength meter (tier known via Defense) or ???;
//          each side's roll as a big "d6" die with a +N badge on its corner;
//          the faction behind a bonus as its insignia under the die (instead
//          of "Rebel support" text); the odds as big WIN / LOSE numbers with
//          a small TIE between, over a thicker bar. "no modifier" and the
//          reveal-Affiliation hint are gone. Same data as v0.21.7.
//          AMOLED: forecastUnitsDrawCb (triangles / squares / meter packed
//          into user_data), forecastDie; back circle shrinks to 64px to fit
//          below the fleet row.
// v0.21.10 - UI REFRESH step 5: ATTACK moves into Fleets, both platforms
//          (user: the lower-right ATTACK circle was "a tiny bit awkward";
//          user's idea: the FLEETS button becomes the attack button).
//          During Resolution Home's FLEETS circle turns into a red ATTACK
//          circle (phone: the FLEETS tile becomes an ATTACK tile; the
//          separate ATTACK A PLAYER pill is gone). It opens Fleets in attack
//          mode: tap a ready fleet -> ATTACK WHO? (rivals with insignia and
//          ship counts) -> their fleets -> the Battle Forecast in PvP mode
//          (their ships face yours, plain d6 both sides = 42 / 16 / 42, your
//          fleet still switchable) -> ENGAGE. A refusal (that fleet already
//          fought, a battle is running) shows in red on the forecast.
//          Previously the attacking fleet was auto-picked (first ready one);
//          now you choose it. Fleets outside Resolution gets the same row
//          style (tap -> the fleet's ships / investing, unchanged).
//          AMOLED: uiRow / uiSmallBack helpers, forecast_pvp mode in
//          showBriefing, UNITS_SHIPS_LEFT. Phone: attackWith / pickDefFleet /
//          pickPvpAtk; after a successful ENGAGE the attack flow closes so
//          the player lands on Home after the battle.
// v0.21.11 - Rule change (user, 2026-09-29, superseding v0.21.8): the
//          garrison is never shown on a planet you don't own -- no tier, no
//          "???", no count (not even after losing a Fight there, which used
//          to unlock the exact number). Your own planets show the exact
//          count. The forecast's right column just says GARRISON over the
//          die. /state no longer carries garrisonTier or lostHere, and sends
//          "defense" only for your own planets. Removed with it:
//          snapshotGarrisonTierKnown(), the forecast's garrison squares and
//          strength meter (AMOLED + phone). In a battle the scene still
//          shows the garrison's ships -- you're fighting them.
// v0.21.12 - Fix (playtest 2026-10-12): on the phone, LOCK IN "doesn't
//          always work / show locked in", and after NEGOTIATE a player
//          "couldn't select any actions" (soft freeze). Two causes found in
//          the phone page, both about taps and repaints, not game rules:
//          (1) Convention 8 violation: Home, the lock-in confirm, the
//          negotiation offers and most other screens were thrown away and
//          rebuilt from scratch on EVERY 700ms poll, so a tap that landed
//          during a rebuild was dropped. Only Galaxy / planet pages had a
//          guard. Now every in-game screen goes through setApp(), which
//          skips the rebuild when the HTML is unchanged (the join pickers
//          and the battle card keep their own update logic).
//          (2) Overlapping polls (the timer plus one after every action)
//          could answer out of order, and a stale answer repainted old state
//          -- e.g. "LOCK IN" again right after locking. Polls are now
//          numbered and an answer older than one already shown is ignored.
//          If the negotiate freeze still happens after this, it needs a
//          closer look (which device, what was on screen) -- the server
//          side of negotiation was checked and has no lock/loop problem.
// v0.21.13 - UI REFRESH step 6: the Victory screen ("battle report"), both
//          platforms (agreed 2026-09-28; user 2026-10-12: "lets do a victory
//          screen"). When a battle ends the button says CONTINUE and leads
//          to a report: a VICTORY / DEFEAT / WITHDREW badge in your color /
//          red / dim, the planet (or what's left of both fleets in PvP), one
//          line on what it got you or cost you ("HOTH IS YOURS - +1 VP every
//          round you hold it", "ENEMY FLEET DESTROYED - +2 VP", "Fleet lost
//          - rebuild it at a planet you hold"...), and, when the battle moved
//          a faction's opinion of you, a pulsing INCOMING TRANSMISSION chip.
//          Held transmissions: the AMOLED's 180ms safety net no longer pops a
//          transmission over SCR_BATTLE / SCR_VICTORY, and the phone holds
//          its overlay while you're in a battle or its report -- the
//          transmission plays after CONTINUE instead of cutting into the
//          fight (the user's original complaint, 2026-09-27).
//          The report is built from each device's own snapshot of the final
//          exchange, so when the other commander closes a PvP battle first
//          this device goes straight to its report instead of being stuck on
//          a dead battle screen (new fallback: if it never saw the result,
//          Home). Outcome rules: BattleOutcome::of() / battleOutcome() mirror
//          game_state.cpp (decisive = a fleet destroyed, +2 VP in PvP).
// v0.21.14 - The Galaxy screen becomes a map of the physical board, both
//          platforms (user 2026-10-12: "I wonder if the galaxy page should
//          somehow line up with them"). Board layout A2 (tools/board/,
//          the design notes "Board design"): every planet sits at its board slot --
//          Outer Rim ring (Hoth, Endor, Tatooine, Dagobah, Oba Diah, Kessel),
//          Mid Rim ring (Kijimi, Naboo, Bespin, Dathomir), Core (Coruscant,
//          Corellia) -- over the ring routes, hyperspace lanes, the two
//          nebulae (The Maw E, Unknown Regions W) and the four faction
//          hangars (diamond + its two lanes, in faction colors). Planets left
//          out of the game show as empty slots, so the screen always matches
//          the table. Circles 68px (art baked at a new 52px size), neighbors
//          72px apart; GALAXY + Credits top-left, back bottom-left (the free
//          diagonals). Tap a planet -> its page, as before; owner colors and
//          live recolor unchanged. Phone: the same map in a square SVG with
//          positioned buttons. Layout tables: GALAXY_POS / GALAXY_LANES /
//          GALAXY_HANGARS (.ino) == the phone's (web_server.cpp) == the board
//          model (shadowfleet_board_a2.scad, scaled) -- change all three.
// v0.21.15 - Galaxy board map: nearest-target touch (user, reviewing the
//          v0.21.14 mockup: "worried about the pressability, especially with
//          the back button" -- the circles are ~6.5 mm on the 1.75" with 4 px
//          gaps, and the back button sat in a cramped pocket).
//          AMOLED: one invisible layer over the whole map takes every touch
//          and picks the CLOSEST target -- a planet in play (within 45 px of
//          its center) or the back button (within 56 px) -- so taps in gaps,
//          on lanes or just off a circle still land. The target under the
//          finger shows its pressed look (cyan border) while held; slide to
//          change it, lift over empty space to cancel; it acts on CLICKED
//          (LVGL's last event, like every other screen, since acting deletes
//          the layer). The circles themselves are no longer clickable.
//          Back button and GALAXY/Credits swapped: back now top-left (the
//          roomier pocket, no lanes cross it), title bottom-left.
//          Phone: a tap on the map that misses every circle opens the nearest
//          planet (galaxyTap). Visual layout otherwise unchanged.
// v0.21.16 - UI REFRESH: Negotiation restyle, both platforms (mockup
//          "Negotiation", approved 2026-09-27). Header: the planet, and who's
//          offering -- the faction's insignia + "The Pykes have two offers"
//          ONLY once you've revealed the planet's Affiliation ("The locals"
//          otherwise, so the screen never leaks hidden intel). The two offers
//          are big cyan cards: letter, the deal, then either its Credit price
//          (gold pill) or an orange "Honor promise - check-in in 3 rounds"
//          line for honor-system deals (the offer text's "(honor-system...)"
//          tag is moved into that line). A refused deal shows in red where the
//          "Leaving costs -1 reputation and locks X this round" hint sits;
//          LEAVE pill at the bottom. The LEAVE? confirm is restyled too (red
//          LEAVE / cyan STAY pills). /state offers gain "honor" (base cost <= 0
//          -- a reputation discount can bring a paid deal's cost to 0, so the
//          adjusted cost alone can't tell). Harness mode "neg". Also removed
//          the unused chooseOfferACb and the AMOLED's now-unused planetTapCb.
// v0.21.17 - UI REFRESH: Factions (reputation) restyle, both platforms (mockup
//          "Factions / reputation", approved 2026-09-27). Title FACTIONS, then
//          one row per faction: insignia, name in its color (your own marked
//          YOU, row outlined in your accent), five tier pips lit up to your
//          tier, and the tier word on the right, colored by how they feel
//          about you (Hostile red, Wary orange, Neutral grey, Favorable green,
//          Allied gold). Tier only, never the raw score. 72px back circle at
//          the bottom. Removed the TEMPORARY "TEST COMMS" demo button and its
//          canned lines on both platforms (testCommsCb / txTestComms) -- they
//          played fixed text unrelated to the game; context-aware
//          transmissions are on the roadmap. Harness mode "factions".
// v0.21.18 - UI REFRESH: join flow part 1 -- Start, Seat, Faction, both
//          platforms (mockups "Start", "Choose seat", "Choose faction",
//          approved 2026-09-27). AMOLED Start: hyperspace streaks behind a
//          big SHADOW / FLEET title (gold), version, a gold JOIN GAME pill
//          and how phones get in (Wi-Fi + password + address). The streaks
//          are the round banner's, factored into drawWarpStreaks() and
//          redrawn every 40ms only while Start is showing. Seat: four 110px
//          circles around a faint orbit (P1 top, clockwise) -- open seats
//          cyan with a big number, taken ones show the holder's insignia
//          and faction (or "?" / JOINING). Faction: 2x2 cards with insignia
//          and name in its color; taken ones dimmed with "TAKEN - Pn"; hint
//          "Allied with your own faction, Hostile with its rival". The two
//          pickers now rebuild live when someone else claims a seat or
//          faction (starTick compares joinScreenKey) -- before, they only
//          refreshed after a failed tap. Big text uses a project-local copy
//          of LVGL's Montserrat 36 (sf_font_montserrat_36.c, ~58KB flash),
//          same trick as the 12. Phone: same seat circles and faction cards
//          (its seat snapshot now includes factions so taken seats update).
//          Harness modes "seat" and "faction". Commander step + Lobby next.
// v0.21.19 - UI REFRESH: join flow part 2 -- Lobby, both platforms (mockup
//          "Lobby"). Four player circles (insignia in faction color, "?"
//          while choosing, empty when open) with "P2 - YOU" / "P1 EMPIRE"
//          labels, GAME LENGTH 6/8/10 pills, and START -- greyed until
//          everyone seated has a faction -- with a status line. NO PLANET
//          PICKER any more (user 2026-10-14: the printed board has a slot for
//          every planet, so all 12 are always in play): SELECT PLANETS, its
//          list screen, gameStateTogglePlanetIncluded and /toggle_planet are
//          gone; planetIncluded stays in GameState (all true) in case a
//          player-count rule wants it later. FIXES: (1) phones could not
//          start the game (no endpoint; the phone Lobby said "waiting for
//          host") -- new /start_game for any seated player, START pill on
//          the phone; (2) a game started elsewhere now moves the AMOLED from
//          the Lobby to Home (starTick), and the Lobby rebuilds live when
//          players, factions or game length change (joinScreenKey now covers
//          the length; the old per-loop() lobby label refresh is gone).
//          Harness mode "lobby".
// v0.21.20 - COMMANDERS, part 1: pick your commander, both platforms (user
//          2026-10-14: 3 per faction). 12 original pixel portraits (24x28)
//          drawn by tools/art/portraits_v2.html and EXPORTED to
//          commander_art.cpp (generated: palette + 1 byte/pixel, ~9KB);
//          names/bios + the transmission voice rule in commander_art.h.
//          Player.commander (id = faction*3 + i, -1 = none; picking a
//          different faction clears it); gameStateChooseCommander();
//          gameStateStartGame() now also waits for every commander. Join
//          flow: Seat -> Faction -> COMMANDER -> Lobby. AMOLED picker: the
//          chosen face 5x in a ring (clip_corner), the other two small at
//          the sides (tap to swap), dots, name + bio, TAKE COMMAND, back to
//          factions. AMOLED portraits are baked once per (commander,
//          variant) into PSRAM ARGB8888 at integer scales -- 2x Lobby, 5x
//          picker, 5x hologram (brightness -> 5 blues + scanlines) ready
//          for transmissions in v0.21.21; no transform_scale. Lobby circles
//          show the commander's face; "Waiting for Player N to pick a
//          commander". Phone: GET /art serves the same generated data
//          (palette + pixels as hex, names, bios) -- the page keeps no copy
//          and paints canvases; /choose_commander; /state players gain
//          "commander"; picker = big preview + 3 cards + TAKE COMMAND +
//          back; Lobby portraits. Harness: art_from_c.pl builds the fake
//          /art from the firmware files; modes "commander", "lobby".
// v0.21.21 - COMMANDERS, part 2: hologram transmissions, both platforms
//          (mockup "Hologram transmission"). The speaker is the faction's
//          first commander (Ilsa / Stahl / Vosk / Kaan) unless that's YOUR
//          commander, then the next one (user 2026-10-14: another player's
//          pick is fine, only yours is skipped) -- commanderVoiceFor() in
//          commander_art.h, twinned by commanderVoice() on the phone. Shown
//          as a 5x hologram portrait (CMD_IMG_HOLO: same art, brightness ->
//          5 blues + scanlines) in an emitter frame with a beam and ring and
//          an occasional flicker (140ms timer, opacity only); name chip
//          "GEN. TOBIN MARR - REBELS" in the faction color; the quote typed
//          in light cyan; TAP TO CLOSE. Check-ins use 14pt and green I DID IT
//          / red I DIDN'T pills (corners ~222px). Static burst, shake and
//          typewriter unchanged. Removes the placeholder box + letter and
//          its two transform_scale labels. Phone: same layout (emitter frame
//          with CSS flicker, beam, chip, italic quote, pill answers). Harness
//          modes "tx" and "txob".
// v0.22.0 - MULTI-BOARD step 1: the 1.43" is the TABLE HOST (user
//          2026-10-15: "1.43 isn't going to be a player anymore"; wants 4
//          phones + the 1.43" to be a full game). New build role SF_ROLE
//          (board_select.h): PLAYER (1.75" default -- a seat, hosting when
//          alone, as before) or TABLE (1.43" default; override -DSF_ROLE=1/2).
//          The table host runs the same game rules, Wi-Fi network and phone
//          server but never takes a seat (MY_SLOT stays -1) and shows only
//          the shared table screen, rebuilt when what it shows changes
//          (tableKey, from starTick, which returns early in this role):
//          Lobby = faces of who's joined, game length, how to join, "All N
//          ready - START from any phone"; Playing = the galaxy clock (round
//          rim, ROUND n / N, phase, "n of m locked in", each player's face at
//          a diagonal with a bright ring once locked in, VP beside it); Game
//          over = the winner's face, name, VP. Nothing on it is tappable.
//          The Wi-Fi network now allows 8 devices (was the default 4 -- 4
//          phones + a player board would have locked one out). Fix:
//          checkAndShowTransmission() read transmission slot -1 whenever the
//          AMOLED had no seat yet (Start/join screens) -- now returns early.
//          Boot Serial prints the role. Next: step 2, the 1.75" joining a
//          table host as a player board.
// v0.22.1 - MULTI-BOARD step 2: the CONSOLE LINK over ESP-NOW (user
//          2026-10-15: "lets set up with espnow channel and get them
//          talking"). Words: CONSOLE = an ESP32 screen (Table Console =
//          1.43", Command Console = 1.75"); MAP = the printed board.
//          console_link.cpp: whichever console hosts (always the table;
//          a command console when alone) broadcasts a BEACON every 500 ms on
//          Wi-Fi channel 1. A command console listens 3 s at power-on: heard
//          a table -> sends HELLO, receives the whole GameState (~4 KB in
//          230-byte chunks, unicast so the radio retries, pushed whenever its
//          hash changes and at least once a second) and becomes a MIRROR --
//          no Wi-Fi network or phone server of its own; phones use the
//          table's. Heard nothing -> hosts as before. On a mirror, the 21
//          gameState* functions that change the game forward themselves
//          (first line of each: if (g_remote) ...) as a CALL; the table runs
//          it (gameStateDispatch), pushes the new state, then REPLIES, and
//          the call returns only once the mirror holds that state -- so
//          screens that re-read right after an action see it. Retries are
//          de-duplicated per console. Packets carry a protocol version and
//          sizeof(GameState): mismatched firmware is ignored. Each console's
//          join id is now "CONSOLE-xxxxxx" from its MAC (was "AMOLED_HOST").
//          Power the table console first; a command console that boots
//          alone hosts its own game. If the table goes quiet the command
//          console keeps saying hello until it answers.
//          Map (tools/board, same day): A3 v3 -- cable channel underneath so
//          the map lies flat (map now 6 mm), peg-free plate, bezel fillets.
// v0.22.2 - Console link: chasing the command console's slowdowns (user
//          2026-10-15, first two-console test: "the 1.75 seems to be
//          experiencing some slowdowns"; the link itself worked first try --
//          the table saw the players, no call timeouts in the logs). The
//          command console's radio no longer power-saves
//          (esp_wifi_set_ps(WIFI_PS_NONE)) -- in station mode it naps between
//          beacons and can miss state packets, and every tap then waits up to
//          0.4 s for the next full copy. Both consoles now print link stats to
//          Serial every 5 s (table: states sent / changed, calls run; command:
//          states applied, chunks, call count + avg/max round trip + longest
//          wait for the new state) so the next step is measured, not guessed.
// v0.22.3 - Console link speed. v0.22.2's stats (user playing on the 1.75"):
//          idle was clean (1 keepalive state/s, 0 churn, every packet
//          arriving), but each tap's round trip took 106-566 ms -- the UI is
//          frozen meanwhile -- because the table pushed the whole state in 19
//          small packets at the 1 Mbps ESP-NOW default before replying, and
//          the occasional lost reply cost a 300 ms retry. Now: ESP-NOW v2
//          packets (1400-byte chunks -> 3 packets), console-to-console unicast
//          at 24 Mbps (esp_now_set_peer_rate_config), retry after 120 ms.
// v0.22.4 - Join by QR code on the Table Console (user 2026-10-15: "it would
//          be really really nice if I didn't have to send them to some silly
//          IP"). The table's Lobby shows two QR codes: 1 JOIN WI-FI (standard
//          "WIFI:T:WPA;S:ShadowFleet;P:starwars1;;" -- phone cameras offer to
//          join) and 2 OPEN GAME (http://192.168.4.1 -- opens the real
//          browser), with the name/password/address in small print as a
//          fallback. Codes are baked once into PSRAM images (4 px modules,
//          quiet zone) by a project-local copy of LVGL's bundled QR generator
//          (sf_qrcodegen.c/.h, Project Nayuki, MIT, renamed sfqr_) -- no
//          lv_conf.h change. Deliberately NO captive "sign-in" pop-up: faking
//          internet would cut phones' cellular data, and that pop-up browser
//          closes when you switch apps. Lobby re-laid out (title 36, seats
//          104, length 160, codes 182-314, status 376, fallback 402).
//          Planned next: "home Wi-Fi mode" (table joins your router; phones
//          stay on their own Wi-Fi) -- the router password entered once from
//          a PHONE setup page, never the console keyboard.
// v0.22.5 - HOME WI-FI MODE (user 2026-10-15: the favorite option -- phones keep
//          their own Wi-Fi and internet; "I don't like the two QR codes";
//          "keyboard is the worst"; wants a button to switch when there's no
//          Wi-Fi). The Table Console remembers ONE home network (NVS
//          "sfnet": ssid/pass/home) and joins it at power-on (12 s timeout),
//          announcing http://shadowfleet.local over mDNS. Its Lobby then
//          shows ONE QR code -- the game's address on that network -- and a
//          TABLE WI-FI button. No saved network / the user picked TABLE
//          WI-FI / the network can't be reached -> its own "ShadowFleet"
//          network as before, with ONE QR (join that Wi-Fi) + "then open
//          192.168.4.1", "Couldn't reach <name>" when it failed, and a HOME
//          WI-FI button when a network is saved. Buttons save the choice and
//          restart. SETUP happens on a phone, never the console: the phone
//          Lobby shows CONNECT THE TABLE TO HOME WI-FI while the table is on
//          its own network -> /setup lists the networks the table can see
//          (plus a name field), takes the password, saves, restarts. /state
//          gains "net" (home / canHome / ssid). The table's ESP-NOW runs on
//          whichever interface it uses (station on home Wi-Fi, AP otherwise),
//          beacons every 200 ms; a command console now sweeps channels 1-13
//          for up to 5 s, starting with the channel it found the table on
//          last time (NVS "sflink"). A hosting command console still runs its
//          own network only (networkInit(false)).
// v0.22.6 - Home Wi-Fi follow-ups from the first hardware test (2026-10-15):
//          (a) /setup opens at once -- the table scans for networks in the
//          background (at boot, refreshed each visit after 60 s) instead of
//          inside the request, which froze its loop for seconds and pulled
//          the radio off the AP's channel (the user "had to be patient").
//          (b) A command console now FOLLOWS the table: after 4 s without a
//          state it hops channels 1-13 (300 ms each) listening for the
//          table's beacon, adopts the MAC it beacons from (station and AP
//          MACs differ) and says hello -- no replug when the table moves
//          between its own network and home Wi-Fi. The channel is saved only
//          when found (no flash writes while searching). (c) The table's
//          "[LINK] ESP-NOW up" line prints the real channel and mode.
// v0.23.0 - TRANSMISSION ENGINE (agreed 2026-10-15; user: "lets do the fixes
//          and the transmissions"). The 512-line bank written with the user
//          (tools/transmissions/*.txt) is now in the firmware:
//          transmission_bank.cpp is GENERATED by tools/transmissions/
//          make_bank.pl (edit the .txt, rerun, rebuild). What a faction says
//          now depends on WHY its opinion of you moved -- took their planet /
//          betrayal / hit their rival / paid / deal / walked out / kept /
//          broke a promise -- with the real planet name, and planet-specific
//          lines first when you gain a planet they talk about. Still only
//          fires when a tier changes (keeps them special); the tier change
//          shows as its own line under the title ("NEUTRAL > FAVORABLE"),
//          never in the quote. NO REPEATS: every bucket is a shuffled deck
//          whose seed + position live in flash (NVS "sftx"), so lines don't
//          come back until the deck is used up -- across games too. QUEUE:
//          up to 4 transmissions per player (a capture moves two factions at
//          once; the second used to overwrite the first, and a check-in
//          could be overwritten too -- the "minor accepted" known issue is
//          gone). GameState stores line ids (TxMsg, 8 bytes) instead of 4 x
//          160-char quotes. BIG MOMENTS (all 8 agreed): first blood, seat of
//          power (Coruscant/Hoth/Oba Diah/Dathomir), your first Core world,
//          Allied, betrayal, oathbreaker, fleet destroyed, last round -- each
//          at most once per player per game, gold PRIORITY TRANSMISSION
//          title and a longer static burst, replacing that action's normal
//          message. Both platforms. Also: CORELIA -> CORELLIA (spelling).
//          Two Empire lines about Endor are PARKED in empire.txt: Endor is a
//          Rebel world, so the Empire only speaks of it as the Rebels' glad
//          rival and the lines read as a loss.
// v0.23.1 - INCOME (user 2026-10-15, after a game stalled with no money left:
//          "income doesn't work so I will have to restart lol. we need income
//          set up for sure"). There was none: credits started at 12 and only
//          ever went down. Now at the start of every new round each player
//          gets INCOME_BASE (1) + INCOME_RESOURCES_HIGH (3) per held planet
//          with High Resources + INCOME_RESOURCES_LOW (1) per Low one -- the
//          numbers agreed 2026-09-27 ("it needs to feel strained if you don't
//          have any planets"), named constants for retuning. The Invest
//          banner says "+N CR INCOME" (both platforms; Player.lastIncome,
//          "income" in /state).
// v0.23.2 - The table's console link runs on its OWN task (queue item (a),
//          parked earlier; user 2026-10-15 on home Wi-Fi: "clicks have
//          definitely slowed down now" -- logs showed 100-200 ms per tap,
//          was 11-175 ms on the table's own network). Taps from a command
//          console sat in a queue until loop() came round between phone
//          requests, and on home Wi-Fi every phone's /state reply goes
//          through the router and takes longer. linkTask (priority 3, above
//          loop()) now sleeps on that queue and runs the call the moment it
//          lands; beacons and state pushes every 20 ms. linkTableLoop() is gone.
// v0.23.3 - Diagnostics only (Convention 9: real data before guessing). User
//          2026-10-15 after v0.23.2: "still gets slow by the second round" --
//          but the link's taps measured 7-33 ms, so the slowness is on the
//          command console itself. The LVGL task now prints every 5 s:
//          "[UI] 5s: N passes, busy X ms (max pass Y ms); O objects, T
//          timers; heap ... PSRAM ...". Busy time or max pass growing round
//          over round = drawing got heavier; objects / timers growing = a
//          leak in screen building; heap shrinking = memory leak.
// v0.23.4 - More diagnostics. v0.23.3's numbers: Home in a game takes ~400 ms
//          per LVGL pass (2.5 frames/s, 100% busy) and creeps up each round
//          (393 -> 424 ms) with steady objects/timers/heap -- no leak; the
//          battle screen stays smooth, the Lobby is ~25 ms. Suspects: the rim
//          ring (8 wide anti-aliased arcs, past rounds semi-transparent --
//          more of them every round), the star field, the spinning planet,
//          the transmission static. Each now has a timed wrapper; a second
//          line "[UI] 5s draw ms: rim R, stars S, planet P, static X" says
//          where the time goes before anything is changed.
// v0.23.5 - FIX: the command console slowing down over a game (user
//          2026-10-15: "still gets slow by the second round", transmissions
//          "really slow"). v0.23.4's timings: the star field took ~2.9 s of
//          every 5 s on Home. Cause: clearScreen() added the star-drawing
//          callback to the screen on EVERY screen change; lv_obj_clean()
//          removes children, not the screen's own callbacks, so the copies
//          stacked up and each frame drew the stars dozens of times -- worse
//          the more screens you opened. It's registered once now. Also:
//          starTick no longer invalidates the whole screen every 180 ms (that
//          re-rendered every arc, circle and label in 12 slices just to move
//          a few stars). Each tick marks one small area per star for half the
//          stars (moving twice as far: same speed, and 17 areas stay under
//          LVGL's 32-area list) plus the screen's animated objects
//          (spinning planets, registered with trackAnimated() and removed on
//          delete). The battle scene, banner, warp, static and hologram
//          already invalidate themselves.
// v0.24.0 - THE ANY-ANGLE TABLE, part 1 (user 2026-10-15: "I would love for
//          most things to be understandable from any angle"; canvas rows
//          "Table Console v2" + "fun extras"; user picked clock / waiting /
//          battles / big moments / final round / who goes first / taunts /
//          victory / attract, cut the territory ring and Galactic News, and
//          asked for straight blaster bolts, not squiggles). The playing
//          screen of the 1.43" is now light and shape instead of words: the
//          round ring; a 3-part PHASE RING with symbols (coins = Invest,
//          chevrons = Move, crossed blades = Resolve), the current third lit
//          (red during a battle); the round number in segment digits; every
//          player's face badge placed toward their faction's HANGAR on the
//          map (TABLE_UP_MAP_DEG = 150: map A3 turns the screen 60 deg; change
//          it if a badge doesn't point at its hangar) with a VP chip turned
//          to face that player and a green check when locked in. "WHO ARE WE
//          WAITING FOR": once half the table has locked in, the others'
//          badges breathe in their color with a turning dashed ring.
//          BATTLES: the fighters' badges pulse red and blaster bolts fly
//          from the attacker -- badge to badge in PvP, to the planet in the
//          center (turning, with a red ripple) in a planet battle; everyone
//          else dims. Rotated / curved text = lv_draw_letter per letter
//          (tableTextRotated / tableTextArc). Animations are small objects
//          that redraw only themselves (Convention 14).
// v0.24.1 - Table fixes from the first look (user 2026-10-15): the badges sat
//          ~33 deg clockwise of their hangars ("the first player icon lines
//          up exactly with the extrusion to fit the usb cable") ->
//          TABLE_UP_MAP_DEG 150 -> 117 (calibrated on the real map). In-game
//          badges show the FACTION SYMBOL instead of the commander's face
//          ("Symbol instead of face"; tableSymbolBadge). The Lobby keeps faces.
// v0.24.2 - CRASH FIX attempt: the table rebooted with "Guru Meditation
//          LoadProhibited" in the display's SPI interrupt (spi_intr ->
//          spi_bus_lock_bg_exit, null pointer), twice on v0.24.0 and once on
//          v0.24.1 ~10 s after boot in the Lobby -- with lots of small,
//          frequent screen updates. The flush callback told LVGL "done" right
//          after QUEUEING each slice, before its DMA transfer finished. Now
//          the display driver signals it (esp_lcd on_color_trans_done ->
//          lv_display_flush_ready), the standard Espressif pattern, on both
//          boards. Also logs the LVGL task's spare stack every 5 s ("[UI] 5s
//          LVGL stack spare") in case it's a stack overflow instead.
// v0.24.3 - Table badge directions, second calibration (user 2026-10-16:
//          "crimson dawn points straight to hoth" at TABLE_UP_MAP_DEG 117;
//          at 150 the Empire badge sat on the USB fairing). Both fit the USB
//          port being on the picture's RIGHT edge, not the bottom: the top
//          faces USB dir (-30) + 90 = 60. TABLE_UP_MAP_DEG 117 -> 60.
// v0.24.4 - BIG MOMENTS ON THE TABLE (the "gold ring"; mockup "Table Console
//          v2 · big moment"). GameState gains the latest PUBLIC big moment
//          (publicSeq / publicMoment / publicActor / publicVictim /
//          publicPlanet), set when first blood, a seat of power, the Core
//          falling, a betrayal or a destroyed fleet fires for any player, and
//          at the start of the final round. Allied and oathbreaker stay
//          private (user 2026-10-15). When publicSeq changes the table takes
//          over for 8 s (TABLE_ALERT_MS): light chasing round the rim (gold;
//          red for betrayal, fleet destroyed and the final round), the
//          headline and who / what curved along the top AND the bottom edge,
//          the planet turning in the middle with a ripple (or the doer's
//          symbol, or the round number), then back to the clock. The clock's
//          rim turns red in the final round.
// v0.25.0 - CPU BALANCE PACKAGE (agreed 2026-10-12, user: "the more the cpu
//          reacts the funner"; 2026-10-16: "lets do it", plus "I would like
//          there to be levels between planets in the same rings"). Planets now
//          know their ring of the map (PlanetInfo.ring replaces baseDefense):
//          starting garrison = ring base (GARRISON_OUTER 2 / _MID 4 / _CORE 7)
//          + 1 for a High Defense trait (planetStartGarrison) -- so revealing
//          Defense tells you where a planet sits in its ring. Traits retuned
//          so every ring has both levels: BESPIN + DATHOMIR -> High (5),
//          CORELLIA -> Low (7). Outer: Dagobah/Endor/Tatooine 2, Hoth/Kessel/
//          Oba Diah 3; Mid: Naboo/Kijimi 4, Bespin/Dathomir 5; Core: Corellia
//          7, Coruscant 8. Pay prices follow (they already add the garrison).
//          FACTION ALERT: taking a planet BY FORCE gives that faction's other
//          unclaimed planets +1 garrison (FACTION_ALERT_GARRISON, capped at 8)
//          -- diplomacy becomes the cheaper path. SLOW REGROWTH: every 2
//          rounds (REGROWTH_EVERY_ROUNDS) unclaimed planets regain +1, never
//          past their starting garrison. All named constants for retuning.
// v0.25.1 - The last old-style screens, restyled on both platforms (queue
//          item (e); user found CONFIRM PAYMENT 2026-10-15). PURCHASE: the
//          planet turning, the price big in gold (red if you can't afford
//          it), what you have, "+1 WITH THE <FACTION>" once Affiliation is
//          revealed, a gold PAY pill (disabled when short) + the back circle.
//          FLEET DETAIL: six ship shapes in your color (destroyed = red cross,
//          not built = faint outline) with names, status + Credits on one
//          line, INVEST / FOUND / REBUILD as a pill. GAME OVER: the winner's
//          face in their color, VICTORY in gold if it's you (else "<FACTION>
//          WIN(S)"), then the standings rows -- now one shared helper with the
//          Standings screen (uiStandingsRows / standingsRowsHtml). Drops the
//          last transform_scale label (Convention 7).
// v0.25.2 - The rest of the agreed table screens (canvas "Table Console -- fun
//          extras"; user picks 2026-10-15). WHO GOES FIRST: GameState.
//          firstPlayer is picked at random at START; for 6.5 s the table shows
//          every faction badge at its hangar and a light spinning round them
//          three times, slowing and landing on that player, whose badge glows.
//          VICTORY CEREMONY (game over): the winner's symbol big in the middle,
//          fireworks in gold + their color, "<FACTION> WIN(S)" and VP /
//          planets curved along both edges. ATTRACT MODE (Lobby, nobody seated
//          yet): rings drawn in toward the join code ("pulling you in", not a
//          spinny galaxy -- user), "SHADOW FLEET" curved at the top and both
//          sides; seats + game length appear once someone joins. New table fx
//          FX_COMET / FX_FIREWORK / FX_INWARD (inward rings redraw at half
//          rate: they cover the whole screen); FX_PULSE takes its color.
// v0.25.3 - SOUND on the 1.75" Command Console (user 2026-10-15/16: "I mostly
//          want sounds for global events so the middle screen doesn't get
//          forgotten", "use the 1.75's speaker for it"). New sf_audio.h/.cpp:
//          the ES8311 codec (I2C 0x18 on the shared bus) + I2S (BCLK 9, WS 45,
//          DOUT 8, DIN 10, MCLK 42 -- Waveshare's BSP header and 08_ES8311
//          example agree; pin_config.h contradicts itself) + the speaker amp
//          (GPIO 46, switched on only while a sound plays). Espressif's
//          ES8311 driver is vendored as sf_es8311.* (Apache-2.0) with its
//          register access moved onto our i2c_master bus (Arduino's Wire would
//          clash with the touch panel's driver). Sounds are ORIGINAL, made in
//          code from tone steps on an audio task: CHIME (new phase), HORN
//          (a battle starts), FANFARE (first blood / seat of power / the Core
//          falls), ALARM (betrayal / fleet destroyed / final round), CHIRP (a
//          transmission opens). sfxWatch() in starTick follows the mirrored
//          state and is primed on its first look, so nothing plays at boot.
//          The 1.43" (no audio hardware) builds no-op stubs.
// v0.25.4 - THE SHUFFLED GALAXY (user 2026-10-16: "I do want to eliminate
//          advantages that come from having memorized the board. strengths and
//          alliances should be fluid and reassigned most games"). At every
//          START: the four SEATS OF POWER keep their faction (Coruscant Empire,
//          Hoth Rebels, Oba Diah Pykes, Dathomir Crimson Dawn -- "their
//          alliance stays the same"), the other 8 planets are dealt 2 per
//          faction (every faction holds 3); Defense High/Low is shuffled within
//          each ring (Outer 3/3, Mid 2/2, Core 1/1); Resources 6 High / 6 Low;
//          and each planet gets a market offset of 0-2 on its price. Lives in
//          GameState (planetFactionOf / planetResIsHigh / planetDefIsHigh /
//          planetMarket) so every console and phone sees the same galaxy;
//          the PLANETS table is now only the classic layout shown before a
//          game. CAPITAL GUARD: a seat off the Core starts at SEAT_GARRISON 6
//          (+1 for High Defense) -- "they should be strong, but maybe
//          fluctuate"; Coruscant keeps the Core's 7-8 ("it makes sense if the
//          empire is in the core story wise"). PRICES no longer leak the
//          garrison or Resources ("you can just tell if a planet is strong
//          based on the price"): ring band (PRICE_OUTER 5 / _MID 8 / _CORE 13)
//          + PRICE_SEAT_PREMIUM 3 for a seat + the market offset; reputation
//          adjusts it only after you've revealed the planet's Affiliation.
//          Boot Serial prints the shuffle ("[GALAXY] shuffled: ...").
//          PlanetInfo.basePayPrice is no longer used.
// v0.25.5 - Attract mode was too heavy on the table: its inward rings covered
//          the whole screen, so every frame re-rendered the curved title (36
//          rotated letters) and the QR code -- measured 97% busy, ~140 ms a
//          frame, which also slows the phones the table serves. Now four short
//          streaks are drawn in along the diagonals (each in its own small
//          box, stopping short of the code's corners), the same "pulling you
//          in" look at a fraction of the cost.
// v0.25.6 - Attract mode REMOVED (user 2026-10-16: "the attract mode doesn't
//          need to be there honestly. lets ditch it") -- the table's Lobby is
//          the same whether or not anyone has joined. Sound diagnostics (user:
//          "no sound on a new round"): sfAudioBegin() now records which step
//          worked or failed and reads the ES8311 chip ID (0xFD/0xFE, expected
//          83 11) to prove the codec answers on I2C; the status is repeated
//          every 10 s ("[AUDIO] ...") because boot lines are lost before the PC
//          listens; and a short chime plays at boot as a speaker test.
// v0.25.7 - Table clock uses the whole screen (user 2026-10-16: "the middle
//          table puck feels small. I can't see what phase we are in, the round
//          number needs to be centered. this one i don't think needs the stars,
//          I don't want it to be crowded but lets utilize all the space").
//          Center disc r84 with a big round number (72 px digits, dead
//          center) and the PHASE NAME curved above and below it; the phase
//          ring moved in to hug it (r96, 10 px) and every phase has its own
//          color (Invest gold, Movement cyan, Resolution orange, battle red)
//          that also colors the rim, so the phase reads from any seat. Badges
//          80 px (were 72), VP chips bigger (20 pt) out by the rim (r200); the
//          battle planet fills the center (140 px). No star field on the table.
// v0.25.8 - Table text fixed (user 2026-10-16, photo of v0.25.7: "everything
//          is a bit off and hard to read"). Root cause: lv_draw_letter's point
//          is the glyph's PIVOT (baseline center -- lv_draw_label.c), but
//          tableTextRotated / tableTextArc passed a top-left corner, an
//          offset applied UNROTATED -- so turned letters drifted apart
//          unevenly ("I N VE ST") and the VP numbers landed beside their
//          chips. Now every letter sits on its baseline. Also: the round
//          number centers what is drawn (a segment "1" is only its right
//          stroke, 20 px off center), thicker strokes; the phase name is 20 pt;
//          unlit phase-ring thirds are a dim neutral gray (the slate looked
//          bright blue); the lock check sits beside the badge, clear of the
//          VP chip. Same fix improves the big-moment headlines.
// v0.26.0 - The Table Console's game screens run on a GLOW RENDERER
//          (table_render.cpp; user 2026-10-16: "clean and elegant like an apple
//          watch", after their Clutch prop ran ~26 fps full-screen glow on the
//          same panel family; user picked option 3 and "single mood color").
//          Ported from clutch_core.ino: a per-pixel radius / angle table (one
//          quadrant, mirrored), per-frame 1-D tables (radial glow, band
//          profiles, angular masks), and a tone map that takes ONE mood color
//          and blooms to white above 1.0. Strips go out through the board
//          layer's DMA buffers (bsp_set_frame_hook: LVGL's flushes are dropped
//          while it runs; LVGL's own 2x40-row buffers are reused -- the table
//          has ~46 KB internal RAM free, too little for new ones). Badges,
//          numbers and words are drawn ONCE per change by LVGL into an 8-bit
//          canvas overlay (PSRAM), which the renderer lights white-hot. The
//          Lobby (QR code + button) stays LVGL. Mood: Invest gold, Movement
//          cyan, Resolution orange, battle red; alerts gold / red; victory
//          gold. Same screens as before (clock, who goes first, big moments,
//          victory) -- new look: anti-aliased rings drawing themselves on, a
//          hot glow, chasing rim light, pulsing badge rings, glowing bolts.
//          "[TR] 5s: N fps" on Serial. Capped at ~25 fps so the web server
//          and link keep their time.
// v0.27.0 - Table redesign: HANGAR GATES (user 2026-10-16 picked concept H of
//          https://claude.ai/artifact/QcRRMgmmA7R5MiA8Nc8gNm; the four badge
//          circles "don't feel well seated"). Each player's seat is a gate in
//          a ring (r 158) facing their hangar: its door arcs (r 134 / 182)
//          slide shut when they lock in, eased by the renderer; open gates
//          breathe once half the table has locked in. Standings = a bar by the
//          rim that grows (shape only, no VP numbers -- user's pick). 2 players
//          = two halves (gates pushed apart to sit across from each other, a
//          divider); 3 players = the unused faction's gate stays, dark.
//          Battles = LIVE SCORE: each side's die in the middle (turned to face
//          its player, tumbling ~0.7 s after every exchange, the winner's
//          white-hot) and the ships left as wedges (garrison as dots); bolts
//          gate to gate or into the planet. Phase change = a sweep of light
//          round the face (phase ring, gate ring, rim) as the new third fills.
//          Colors (user: "yellow isn't seen often in Star Wars control panels.
//          Lets do Red, blue, and a white (really light blue)"): Invest
//          white-blue, Movement blue, Resolution + battles red; big moments
//          white, dark moments red, victory white; who goes first blue (the
//          comet now runs the gate ring and the first player's gate slams shut).
//          Also fixes v0.26.0 booting as "v0.25.8" (compiled before the bump).
// v0.27.1 - Table speed: v0.27.0 ran at 11 fps (82 ms a frame, was 48) --
//          the gate masks looped all 4096 angle steps against every gate with
//          a float modulo each. Now only the steps near each gate are touched.
// v0.27.2 - Table speed, part 2 (v0.27.1 measured 13 fps / 70 ms): the angle
//          masks are read for every ring pixel and the new design is mostly
//          rings, so they move to internal RAM when there is room (ABINS 4096
//          -> 2048: still sub-pixel at the rim, 10 KB); the gate walls and the
//          2-player divider never move, so they are drawn once into the overlay
//          instead of as line segments every frame.
// v0.27.3 - Playtest fixes (first game on v0.27.2, 2026-10-16), both platforms:
//          * NEW GAME on the game-over screen (two taps) -> a FRESH Lobby,
//            everyone leaves their seat, game length kept (gameStateNewGame,
//            forwarded from command consoles as RF_NEW_GAME; phones drop back
//            to the seat picker, the 1.75" to Start). User: "there needs to be
//            a way to restart the game at the end".
//          * ATTACK in Resolution first asks A PLANET (the Galaxy map) or A
//            PLAYER (fleet, then rival). User: "attack is confusing ... it only
//            shows players, when people click that for attacking planets".
//          * PROMISES: the Factions screen lists your honor-system promises --
//            to whom, which planet, when due (phone: inline; AMOLED: a pill to a
//            PROMISES screen). /state carries the requester's own promises only.
//            User: "there is no place to see where your favors are and when
//            they are due".
//          * REPAIR: in Invest a fleet on the board buys back a lost ship (2 CR,
//            anywhere) before commissioning a new one; button "REPAIR A SHIP".
//          * Table memory: its free internal RAM dipped to 1 KB during the game
//            (renderer tables + phones polling) -- the table's LVGL / strip
//            buffers drop from 40 to 20 rows, ~37 KB back.
// v0.28.0 - TWO-FLEET BATTLES (combined assault; playtest 2026-10-16: "You need
//          to be able to bring two fleets to a fight, (both fleets add in for
//          the same battle, not just one after another)"). Agreed rules: no
//          bonus, ships pooled, every attacking fleet spent for the round. The
//          forecast's fleet buttons now toggle (both platforms); battles hold
//          BATTLE_SHIPS = NUM_FLEETS * FLEET_SHIPS ships a side (index fleet * 6
//          + slot) with battleAttackerFleets / battleDefenderFleets masks;
//          losses are written back to every fleet involved. PvP: when the
//          attacker came with 2+ fleets, the defender may bring more (up to the
//          attacker's count) before the first exchange -- "+ BRING FLEET n"
//          (gameStateReinforce, RF_REINFORCE, /reinforce); defending never
//          spends a fleet. Each fleet stands in its own column in the battle
//          scene; forecast ship icons wrap into rows; the table's live-score
//          wedges tighten. gameStateCommitFight / gameStateStartPvpBattle take
//          fleet MASKS now. sizeof(GameState) grows ~30 bytes -- flash BOTH.
// v0.28.1 - GAME-CHECKED PROMISES (playtest 2026-10-16: "some favors the computer
//          can know. If I overtook an empire planet, it should know that").
//          Offers now carry a PromiseCheck: TAKE_FACTION (take an <X> world in 3
//          rounds), WIN_PVP (destroy another commander's fleet), NO_INVEST (no
//          defense investment here), NO_ATTACK (attack no <X> world), HOLD (keep
//          this world), TRIBUTE (1 CR at every round start); the rest stay
//          HONOR (self-reported check-in, as before). Do-promises are KEPT the
//          moment they happen and BROKEN at the deadline; don't-promises are
//          BROKEN the moment they happen and KEPT at the deadline; tribute pays
//          itself (can't pay = broken); a held world lost by a round start =
//          broken. Same rewards as check-ins (rep +2 / -3, +1 VP, the
//          faction's kept/broke line). Offers that used to be vague tasks were
//          rewritten as checkable ones (Rebels: Strike the Empire, Local
//          Autonomy; Empire: Enforce Imperial Law, Imperial Garrison, Swear
//          Allegiance; Pykes: Spice Contract; Crimson Dawn: Remove a Rival,
//          Undercut the Syndicate; any world: Concession, Shared Revenue).
//          Offer cards and the Promises list say "the game checks it" or "on
//          your honor"; Serial logs "[PROMISE] ... KEPT/BROKE".
//          FIX: v0.27.3's promises in /state wrapped the text in an extra pair
//          of quotes (jsonStr already adds them) -- the JSON broke for any
//          player holding a promise, freezing their phone.
// v0.28.2 - A destroyed fleet REBUILDS AT YOUR HOME HANGAR (user 2026-10-19: "You
//          need to be able to rebuild at your primary spawn point in one of the
//          corners. if you lose your first fleet the game is essentially
//          over"). Same 2 CR a ship in Invest, no planet needed; founding a
//          never-built fleet (2 or 3) still needs a planet you control. Button:
//          REBUILD AT HANGAR; the defeat report says "rebuild it at your hangar".
// v0.28.3 - Second-playtest fixes (2026-10-19):
//          * STUCK BATTLE: a finished battle only closed when the attacker
//            tapped CONTINUE; a closed / locked phone left "a battle is already
//            in progress" forever. The host now closes it BATTLE_AUTO_CLOSE_MS
//            (20 s) after its last exchange (gameStateTick from loop()).
//          * No PAY or NEGOTIATE for another commander's planet -- FIGHT only
//            (rules + both planet pages). User: "you have to face their defenses".
//          * Defense trait = real starting strength: Low / Medium / High /
//            Fortress (seats of power and Core worlds never read Low -- the user
//            "went into hoth thinking it was gonna be easy").
//          * "EVERYONE IS WAITING ON YOU": once per phase when all others have
//            locked in (phone banner + buzz, 1.75" toast + chirp).
//          * Promise transmissions are titled PROMISE KEPT / PROMISE BROKEN and
//            always arrive (before, a kept/broken promise that didn't move a tier
//            sent nothing). PROMISE HISTORY (last 4, PromiseRecord) on both
//            platforms' Promises views.
//          * Owner colors: the Empire is bright steel white (#E6EEF5, user pick);
//            owned planets on both galaxy maps get thick rings + a tinted fill.
// v0.28.4 - Every fleet is FOUNDED or rebuilt at your home hangar -- no planet
//          needed any more (user 2026-10-19: "you should be able to create 2-3
//          at your home base"). Same FLEET_INVEST_COST (2 CR) a ship; retune if
//          extra fleets come too easily. Button: FOUND AT HANGAR.
// v0.29.0 - REPUTATION REVAMP (user 2026-10-19, mockup
//          https://claude.ai/artifact/HHeRWNatde6jL7AEEvhVcA): the hidden score is
//          -50..+50 (was -10..+10) with wider tiers -- Hostile -50..-30, Wary
//          -29..-10, Neutral -9..+9, Favorable +10..+29, Allied +30..+50 -- so
//          actions can be granular (user: "the scale may need to be a bit bigger").
//          Start: own +40, rival -40. Sizes (REP_* in game_state.h): pay +3, deal
//          +4, walk-out -2, conquest -12 (+4 to the rival), promise kept +6 /
//          broken -10, betrayal still snaps to 0. SHOWN, without numbers: the
//          Factions page has a five-zone bar with one bright line at your exact
//          standing (it glides from where that device last showed it, with a
//          streak), and changes appear as ARROWS (one / two / three by size) on
//          the battle report, a new PURCHASE card, and transmissions (beside the
//          tier chip; TxMsg.delta). /state now carries the requester's own scores.
//          Negotiation itself is unchanged until v0.30 (the conversation).
// v0.30.0 - NEGOTIATION IS A CONVERSATION (user 2026-10-19, Fire Emblem
//          recruiting; mockup https://claude.ai/artifact/4KsxPW9rF5B8BHdSZiyLz1).
//          NEGOTIATE now opens a conversation with the planet's faction: 3
//          questions on the Outer Rim, 4 Mid Rim, 5 in the Core -- easy, then
//          harder, then hard -- from a SHARED bank of 96 (negotiation_bank.cpp,
//          generated from tools/negotiation/questions.txt by make_bank.pl): any
//          faction may ask a question, and each reply carries every faction's
//          reaction, so knowing who you're talking to matters. Questions never
//          name a faction (user rule). Per-faction, per-tier decks in flash (NVS
//          "sfneg") so a question doesn't come back until the deck is used up.
//          Graded by MISSES (user: "2 greens and 1 grey should not be a no
//          strings attached win"): all liked = the planet, no strings (+5
//          standing); one miss = pick one of three FAVORS (+2, a game-checked
//          promise); two or more = they name ONE hard favor (each faction's
//          first: take a world of its rival); refusing = a loss (-2). Two
//          OFFENDING replies = cut off (-6). Walking out = a loss (-2). Two
//          losses at a planet = they never negotiate with you there again.
//          Reputation helps: Favorable = your contact removes the worst reply
//          on the last question, Allied = on every question; Hostile = they
//          won't talk. Unrevealed Affiliation = an UNKNOWN AMBASSADOR (torn gray
//          hologram, face picked by planet so it leaks nothing). A won
//          conversation is worth +1 VP (+2 in the Core) so late diplomacy still
//          counts (proposed 2026-10-19 -- VP_NEGOTIATION*, retune after play).
//          Cash deals are gone from negotiation; PAY ESCALATES instead (user:
//          "buying your first planet is cheaper... and it goes up everytime"):
//          the first purchase is 2 CR off, each later one 3 CR more
//          (PAY_FIRST_DISCOUNT / PAY_STEP). The old two-offer pools became each
//          faction's three favors. Transmissions wait while a conversation or
//          the purchase card is up (the 1.75" safety net used to cut in).
//          GameState changed (both consoles must run v0.30.0).
// v0.30.1 - VP ON CAPTURE + FAVORS THAT CAN'T BE DODGED (user 2026-10-19: "I can
//          take as many favors as I want and never have to do them"; "we
//          shouldn't get VP each round anymore... The money is the roundly
//          spoils"). Planets no longer pay VP every round: each is WORTH VP while
//          you hold it -- Outer 2 / Mid 3 / Core 5, +1 for a seat of power
//          (VP_PLANET_*) -- and the VP goes with the planet when someone takes it
//          (setOwnerLocked, the one place an owner changes). A planet won with a
//          favor is worth nothing until the favor is kept; broken = its faction
//          takes it back (unclaimed, garrison restored). Promise deadlines never
//          run past the end of the game, and at the end every open promise is
//          judged before the count (undone deeds = the planet goes back). The
//          v0.30.0 conversation VP is gone (capture VP replaces it).
//          FAVOR BANK (tools/negotiation/favors.txt -> negotiation_bank.cpp): 8
//          normal + 6 late favors per faction; each favor stage DRAWS 3 (two+
//          misses = the faction's hard favor only). THE LAST 2 ROUNDS
//          (NEG_LATE_ROUNDS) draw from the late bank: 1 Credits favor -- paid on
//          the spot at your PAY price x 3/2, at least +4, always dearer than
//          buying -- and 2 deeds due when the war ends, the rounds left spelled
//          out. New game checks: WIN_BATTLE (capture in a fight or destroy a
//          fleet), TAKE_CORE, HOLD_TWO (this planet + one of the faction's
//          RIVAL's -- user: holding their own "seems backwards").
//          FACTION QUESTIONS (faction_questions.txt, 6 each): once Affiliation is
//          revealed, the second question is one of the faction's own.
//          HOSTILE: Affiliation known -> "THEY WON'T TALK TO YOU"; unrevealed ->
//          the ambassador plays along, then unmasks at the end ("WE KNOW WHO YOU
//          ARE", a line from favors.txt H lines, Affiliation revealed free, no
//          standing lost, not a loss). Both offer FIGHT (the one allowed switch
//          of approach in a round: gameStateNegotiationFight) or WALK AWAY.
//          Both consoles must run v0.30.1 (GameState changed).
// v0.30.2 - PLAYTEST FIXES (2026-10-28). (1) The lock-in confirm named only the
//          first ready fleet ("only ever says fleet 1"); it now lists them all
//          ("Fleets 1 + 3 ready to fight"), both platforms. (2) REJOIN: the 1.75"
//          crashed mid-game and the game had to be restarted -- the table still
//          held its seat under the console's id, but JOIN GAME only offered the
//          seat picker, where that seat looked taken. JOIN GAME now finds the
//          seat with this console's id and goes straight back (faction /
//          commander pick if unfinished, else Lobby or Home). (3) The battle
//          report's CONTINUE froze the 1.75" when a transmission was waiting.
//          Likely cause (no serial log of it): two ~4 KB GameState copies on the
//          16 KB LVGL stack at once -- the CONTINUE handler's and the
//          transmission screen's own read (or the battle screen's and
//          showVictory's) -- with ~10 KB spare at idle. The transmission screen
//          now takes the commander from its caller; CONTINUE handlers call
//          showPendingTransmission(), whose copy is gone before any fallback
//          screen; battleFightCb / updateBattleUI scope theirs before
//          showVictory(). The pulsing INCOMING TRANSMISSION chip is tappable too
//          (same as CONTINUE). GameState unchanged; the console link still
//          needs both consoles on the same version.
// v0.30.3 - SAVE STATES on the table (user 2026-10-28: "there needs to be save
//          states, crashing is happening a bunch ... for the game computer, the
//          1.43"). The table writes the GameState (plain data) to LittleFS on
//          the unused "spiffs" partition -- wear-levelled, unlike NVS's 20 KB --
//          as /game.sav (header: magic, format, sizeof, FNV hash), via a temp
//          file + rename, at most every SAVE_EVERY_MS (3 s) and only when it
//          changed, never while a battle is running (a crash mid-battle resumes
//          from just before it -- a half-fought battle can't jam the one battle
//          slot). Boot: a valid save from this layout resumes automatically
//          (user picked "auto-resume + END GAME"); players come back
//          disconnected and rejoin by id (phones as always, the 1.75" via
//          v0.30.2's JOIN GAME). The save is removed whenever no game is
//          running. END GAME (new, gameStateEndGame / RF_END_GAME / POST
//          /end_game): Standings on both platforms, two taps -- a fresh Lobby
//          for everyone mid-game, like NEW GAME. The 1.75" now drops to Start
//          whenever its seat disappears (END GAME or NEW GAME). Includes
//          v0.30.2 (never flashed on its own). New link call -> both consoles
//          must run v0.30.3.
// v0.30.4 - (1) RESUME HANG (playtest 2026-10-28: the table froze, and after a
//          replug "everything is still frozen"): with a saved game the table's
//          glow renderer starts streaming frames from the LVGL task the moment
//          the first screen is up, and setup()'s brightness command (panel IO,
//          not thread-safe) collided with it -- setup() hung before Wi-Fi /
//          the link ever started (serial: no second "[BSP] brightness_set",
//          internal heap stayed at 126 KB = no Wi-Fi), so every reboot resumed
//          into the same hang. It now runs under the display lock. The
//          in-game freeze that started it (after a withdraw) is NOT explained
//          yet -- no serial log was running; the next one decides.
//          (2) HARD FAVORS VARY ("all times I was given the one option after
//          the negotiation, it was the same every time"): 2+ misses drew the
//          faction's single hard favor. favors.txt now marks 3-4 per faction
//          (rival world, destroy a fleet, a Core world, hold two) and one is
//          drawn at random.
// v0.30.5 - PHONE PAY FIXED (playtest 2026-10-28: "the pay button on the phone
//          during resolution still doesn't seem to work"). Broken since v0.30.0:
//          the negotiation rewrite removed `negF`, but the phone's PURCHASE
//          confirm still used it -- a ReferenceError, so tapping PAY rendered
//          nothing. Defined again (revealed Affiliation -> faction) and the line
//          now matches the AMOLED ("THE EMPIRE WILL APPROVE"). New phone-harness
//          scenario `payconfirm` so this screen gets rendered from now on. Phone
//          END GAME made a small outlined button (it was a full-width pill).
// ===========================================================================
// SHADOW FLEET 2.0 (1.0 = v0.30.5, frozen: tag v1.0, branch release/1.0)
// ===========================================================================
// v2.0-dev.1 (2026-10-29, built overnight for the user's morning test; design
//          in the design notes "2.0 PICKS" / "USER ROUND n", screens on the canvas
//          page "2.0 screens"). The four factions stay; the galaxy deepens:
//          - PREPARE replaces Invest; worlds are "Sympathetic to" a faction;
//            your own faction's worlds are known from the start; a world you
//            hold shows all its secrets; base income 3 (was 1); traits cost 1.
//          - TRAITS: 12 effects (TraitId) dealt to the 12 worlds each game,
//            each with its own name per world (sf_content.cpp); holding a
//            world gives its effect (income, cheap ships, free reveal,
//            reroll, defenders +1, market kickback, standing, rebuild,
//            free repair, extra ship on founding).
//          - MARKETS: every world sells one good (GoodId, dealt per game);
//            each Prepare 4 open at random, the same for everyone, 2 each;
//            buy from anywhere; price by your standing; the holder gets a
//            1 CR cut (2 on Core); Black market trait +1 per sale.
//          - THE DIPLOMAT: 3 CR, away 2 rounds, back with all four secrets.
//          - QUESTS (+5 VP at the end): Rebels 3 wins vs the Empire, Empire
//            both Core worlds, Pykes most Credits, Crimson Dawn Favorable+
//            with the other three. AWARDS: Merchant prince +3 (most cuts),
//            Best customer +2 (most purchases).
//          - STRENGTHS: Rebels see every diplomat, Pykes +2 CR a Prepare,
//            Crimson Dawn silent partners (+1 per other player's purchase,
//            max 2 a round). The Empire's 7-ship fleets are NOT in yet.
//          - GALAXY EVENT at the halfway point (Unbound / crackdown / spice
//            boom / bounty / hyperspace storm); the Ashen Fleet is NOT in yet.
//          - "LOOK AT THE TABLE": public moments + events pop up on every
//            1.75" and phone; personal notices (diplomat back, sightings).
//          - 1.75": no star field (frame rate), thinner round rim; in Prepare
//            the FACTIONS circle is MARKETS.
//          NOT YET: deals + inbox, the Ashen Fleet, the Empire's 7 ships.
//          New GameState (SAVE_FORMAT 2): a 1.0 save is ignored. Both
//          consoles must run 2.0 (the console link checks the state size).
// v2.0-dev.2 (2026-10-29, flashed to both consoles; user feedback from the
//          first 2.0 session):
//          - Markets: what the selected good does sits between the rows and
//            BUY in white 14 pt (the pill subline was dim and clipped).
//          - Planet page: a revealed trait shows its name AND what holding
//            the world gives you; tap it for a card. Trait texts trimmed to
//            two lines; Training's text fixed (it's the extra ship now).
//          - Diplomats: the world's holder gets a notice when one arrives.
//          - Standings: tap the quest line for your quest + strength card.
//          - Frame rate: LVGL sleeps on a semaphore while a strip goes to the
//            panel (was a busy spin), and logs [PERF] every 5 s. First
//            reading (Start screen, full-screen warp): ~70 ms to draw a full
//            frame, 0 ms waiting on the 40 MHz panel -- drawing is the cost.
// v2.0-dev.3 (2026-10-29, built with the consoles away -- NOT flashed):
//          - DEALS (mockup "C . Deals"): during Prepare, offer another player
//            Credits for one promise -- don't attack my worlds (2 rounds),
//            stay away from a world (no fight / talks / pay, 2 rounds), send
//            me Credits next round, or your market cut from a world (2
//            rounds). ACCEPT moves the Credits and starts the promise;
//            COUNTER (twice per deal) sends the form back pre-filled; DECLINE
//            / WITHDRAW. Open offers lapse when Prepare ends. The game holds
//            you to it: FIGHT greys out with "You gave your word to ...".
//            1.75": DEALS circle on the Prepare Home (red count), list, offer
//            view, a five-row form with canned lines. Phones: DEALS tile, the
//            same + a typed message (60 chars; the form never rebuilds while
//            you type). Notices when an offer arrives / is answered; the
//            lock-in confirm says "N offers unanswered". "Join the Ashen
//            Fleet strike" waits for the Ashen Fleet.
//          - Frame rate: disabled buttons are dimmed by color instead of a
//            50% object opacity (each one was drawn offscreen and blended on
//            every redraw). tools/perf_ab.ps1 + tools/lvconf/ = speed knobs
//            (-O2, both cores, LVGL in IRAM, 16 ms refresh, 80 MHz panel) to
//            A/B on the 1.75" when it's plugged in.
//          - The 1.75" no longer holds two GameState copies on the LVGL stack
//            in any deals path (GameState grew ~450 bytes for the deals).
// v2.0-dev.4 (2026-10-29, built with the consoles away -- NOT flashed):
//          THE ASHEN FLEET joins the halfway event deck (user's design):
//          - It has HP (10 per player). In Resolution any player STRIKES it
//            from ATTACK or from the middle of the Galaxy map: every living
//            ship in the fleets sent rolls a die, the total is the damage,
//            and those fleets are spent for the round.
//          - It answers each strike with ONE die (user picked option C):
//            1-2 miss, 3-5 one ship lost, 6 two -- the same however many
//            ships you send, so joint strikes pay. (D, "angrier below half
//            HP", is for later.)
//          - While it lives EVERY MARKET IS CLOSED: a pixel CLOSED sign hangs
//            over crossed planks on the Markets page (1.75" + phones); the
//            Prepare Home MARKETS circle / tile reads CLOSED.
//          - Sunk: +2 VP for everyone who damaged it, +1 more for the most
//            damage (shared on a tie); the table shows ASHEN FLEET DESTROYED.
//          - The Galaxy map: Coruscant + Corellia step out to +-46 (54 px)
//            and the flagship sits between them with its HP bar.
//          - THE TABLE: while it lives it takes the center screen in full
//            color (LVGL, not the one-color glow): an ember HP ring round the
//            rim, the flagship, HP, its admiral's taunts (a new one every
//            9 s), the round / lock-in line; a strike shows its damage big
//            for 8 s. Battles, big moments and who-goes-first still use the
//            glow screen.
//          - Original pixel sprites: tools/art/sprites_2_0.py.
// v2.0-dev.5 (2026-10-30, measured on the 1.75" with tools/perf_ab.ps1 +
//          tools/console_drive.ps1): FRAME RATE + TWO FREEZES.
//          - FREEZE FIX: the touch chip is polled over I2C from the screen task
//            with a 5000 ms timeout -- one stuck read froze the 1.75" for
//            exactly 5 s (caught idle on Home; likely the playtest "freeze").
//            I2C timeouts are now 30 ms; a slow touch read is logged.
//          - HANG FIX (dev.3 regression): the flush counted a transfer as in
//            flight AFTER queuing it; a tiny area could finish first and LVGL
//            waited forever. Counted before queuing (atomic), plus a 200 ms
//            give-up that logs "[BSP] flush wait gave up".
//          - Build with -O2 (the core's default is -Os): +18%.
//          - Start screen / round banner streaks: redraw only their own boxes,
//            angles worked out once (shakeNoise's sinf ran ~1700x a frame),
//            each streak a pre-drawn A8 image clipped to its segment.
//            Start: 13.6 -> 22 fps.
//          - The round rim, the spinning planet and the Galaxy map's board
//            (rings, lanes, nebulae, hangars) are drawn once into images and
//            copied; custom draws skip what the strip can't see (layerSees).
//            Opening Home ~200 -> ~72 ms, Galaxy ~184 -> ~135 ms.
//          - Found: the core's ESP32-S3 libs have a 16 KB instruction cache and
//            LVGL runs from flash, so EVERY small object costs ~1-2 ms; fewer,
//            simpler objects (and images for static art) are what pays.
//            Tried and dropped: both cores (slower), LVGL hot code in IRAM
//            (leaves 0-4 KB internal RAM), 80 MHz panel / 16 ms refresh (no
//            change while drawing is the cost), bigger circle-mask cache.
//          - Test tools: serial "T x y" tap, "D x0 y0 x1 y1 ms" drag, "P/R/Q"
//            per-object draw profile; [SCR] n on every screen change;
//            [PERF] flushes + slow refreshes.
// v2.0-dev.6 (2026-10-30): CRASHES -- CATCH THEM, RECOVER FROM THEM (user:
//          the table should be "more robust, crashes less, and recovers").
//          - sf_health.h/.cpp: every boot prints why the last run ended
//            (power / crash / watchdog / brownout) and, after a crash, the
//            core dump's task + address + backtrace (the core already writes
//            one to the coredump partition -- nothing read it). The last 4
//            are kept in NVS and shown at http://<table>/health and by the
//            serial command HEALTH. RTC breadcrumbs: the last run's uptime
//            and lowest free internal RAM.
//          - Task watchdog (8 s) on the LVGL, link and loop tasks: a stuck
//            console restarts; the table resumes its saved game.
//          - The 1.75" rejoins its seat by itself after a reboot (no JOIN
//            GAME tap) when the table still has that seat under its id.
//          - Serial "CRASH!" crashes on purpose (to test all of the above).
// v2.0-dev.7 (2026-10-30): SWIPE BACK + LOCK-IN POP.
//          - 1.75": a drag that starts at the round glass's left edge and
//            goes ~70 px right = the screen's back (user: "we could get rid of
//            every back button if we could swipe from the left side as back
//            reliably"). A "<" disc follows the finger, cyan once it counts.
//            Every back-button style registers itself (uiMarkBack); STAY /
//            KEEP PLAYING count as back on the confirm pages. The buttons stay
//            until the swipe has proven itself. Board layer: bsp_set_touch_hook.
//          - Table: a gate that locks in flashes a seal across itself and two
//            sparks race away along the gate ring; a locked gate keeps a
//            glowing seal (user: the lock-in was "a little too subtle ... just
//            give it a little more pop"; keep the glow look).
// v2.0-dev.8 (2026-10-30): BACK BUTTONS GONE on the 1.75" (user: "the swiping
//          is amazing. lets remove the back buttons"). They are still built
//          -- the swipe presses them -- but hidden (UI_BACK_BUTTONS); the
//          Galaxy's back is no longer a touch target. STAY / KEEP PLAYING stay.
//          Next: a layout pass to use the freed space.
// v2.0-dev.9 (2026-10-31): LAYOUT PASS after the back buttons went: the
//          buttons that sat beside one are centered -- TAKE COMMAND (wider),
//          the deal form's SEND, Standings' END GAME, the briefing's ENGAGE.
// v2.0-dev.10 (2026-10-31, found by the overnight soak test): the table fell
//          off the home Wi-Fi after ~70 min and never rejoined -- the game ran
//          on but no phone could reach it (nothing watched the connection).
//          homeWifiWatch(): logs each drop with the router's reason, reconnects
//          after 5 s (every 10 s), rejoins from scratch after a minute, and
//          announces shadowfleet.local again when it's back.
// v2.0-dev.11 (2026-10-31): TABLE LOCK-IN = "B + C" (user's pick from board P
//          take 2, rendered from this renderer's own math): same shapes; open
//          gates go quiet (doors 0.55x, symbol dimmer) and a locked gate's
//          doors bloom (1.8x) in its owner's faction color (per-seat tone maps
//          in PSRAM, picked per pixel inside the gate's span). Locking in
//          flashes them brighter for a moment. dev.7's seal + sparks removed.
// v2.0-dev.12 (2026-11-02): HOME WORLDS + 17 WORLDS (user 2026-10-31/11-01/11-02;
//          the design notes "DECISIONS 2026-11-01"; board take 4). PHONE-FIRST: the
//          1.75" compiles but keeps the 12-world map (AMOLED_TODO.md).
//          - NUM_PLANETS 12 -> 17: YAVIN 4, MUSTAFAR, NAL HUTTA, CANTO BIGHT and
//            KUAT appended (old indices unchanged); Dathomir moves to the Outer
//            Rim, Endor to the Mid Rim; the Core has 3 worlds.
//          - Home worlds (Yavin 4 R, Mustafar E, Oba Diah P, Dathomir C) replace
//            the seats of power: you start holding yours (+2 VP) with a home
//            guard of 6 that fights only for its own faction (a conqueror holds
//            it with an ordinary Outer Rim garrison); only a fight takes one;
//            worth 2 to its faction, 6 to anyone else (the VP leaves with it),
//            3 when nobody plays that faction (neutral, guard of 4).
//            planetVP() now takes the holder. TXB_SEAT = someone else's home
//            world ("HOME WORLD"); TXB_CORE = any Core world. Pay has no seat
//            premium. The 13 other worlds are dealt round the factions (one
//            random faction gets a 4th).
//          - The FORTRESS trait is gone (user: "unpredictable"); 11 traits are
//            dealt to 17 worlds, each new world has a name for each.
//          - Phone: the 17-world map (Core 56 / Mid 124 / Outer 190, hangars at
//            214 beside the nebulae, one lane each to the home world, a dashed
//            emergency lane to the nebula once your home is taken), Core circles
//            60, 5 new palettes (also in the .ino for the table's battle
//            planet), a HOME WORLD card on the planet page, FIGHT only.
//          - Transmissions: the bank knows 17 worlds; the Empire's and Rebels'
//            home-world lines now speak of Mustafar and Yavin.
//          - SAVE_FORMAT 3. Tested in the twin: tools/twin/homeworlds.js.
// v2.0-dev.13 (2026-11-02): DROP OUT / REJOIN (board O, approved) + the table's
//          gates for board B17.
//          - A player is AWAY after AWAY_AFTER_MS (30 s) with no word from their
//            phone or console, or when they tap STEP AWAY (Standings). AWAY =
//            held locked in (re-locked each phase), so nothing waits for them;
//            worlds, fleets, Credits hold. With everyone away the table waits.
//            In a battle an AWAY defender fights, an AWAY attacker withdraws.
//          - Back: their phone polls again (STEP AWAY needs COME BACK), or a new
//            phone / rebooted console takes the AWAY seat in the seat picker
//            (mid-game). Then a WELCOME BACK card: rounds missed, VP, Credits,
//            worlds lost / gained since they left.
//          - Presence: every request from a seated device (gameStatePresent);
//            the 1.75" sends a link LT_HERE with its seat every HERE_EVERY_MS;
//            the twin host's view polls use ?peek=1 and remote twin viewers
//            POST /here every 10 s.
//          - Table: an AWAY gate is dashed and dark, its bar and badge dimmed.
//            Gates for board B17: TABLE_UP_MAP_DEG 0 (the hub's plug points
//            south); the hangars sit in pairs 24 deg apart beside the nebulae,
//            narrower than two gates, so each gate keeps its side and order,
//            TABLE_GATE_SPREAD (35) off its nebula.
//          - Tested in the twin: tools/twin/dropout.js.
// v2.0-dev.14 (2026-11-03): HOW TO PLAY primer on phones (user: "a primer ... would
//          go along with a rulebook, but it would help get things associated with
//          the pictures"). Eleven swipeable picture cards built from the game's own
//          art (planet pixels, insignia, the board map, ships, dice, the reputation
//          bar): goal, factions, board, home world, phases, fleets, Credits + intel,
//          taking a world, battle, reputation, also out there. Opens from the seat
//          picker (NEW HERE?), the Lobby and Standings. Phone only, no game state
//          (nothing on the table or the 1.75"). Harness: phone_primera..k.
// v2.0-dev.15 (2026-11-03): TABLE CRASH FIX -- a player coming back from AWAY
//          crash-looped the table (dev.13-14). sendStateJson held TWO ~5 KB
//          GameState copies on the 16 KB web task stack (the second for the
//          re-read after gameStatePresent); with 17 worlds that overflowed it
//          (Stack canary, loopTask), and the saved game resumed into the same
//          crash. Found by a RAM test (fake phones polling) on the table.
//          - The web handlers now read the state into ONE copy kept in PSRAM
//            (webState + gameStateReadInto) -- no GameState on that stack.
//          - /state reserves STATE_JSON_RESERVE up front: one PSRAM block
//            instead of a realloc chain through internal RAM (allocations
//            <= 4 KB come from internal RAM); logs "[NET] /state answer N
//            bytes" each time a new largest answer goes out.
//          - /state carries a fingerprint ("h", FNV-1a of the answer); phones
//            send it back (?h=) and get {"same":1} when nothing changed, not the
//            whole ~11 KB answer. The Wi-Fi stack holds each answer in internal
//            RAM while it goes out: with 4 phones polling, the table's free
//            internal RAM hit 0 KB in the test.
// v2.0-dev.16 (2026-11-03): TABLE RAM HEADROOM. Busy play (4 phones, players
//          acting every 4 s) still ran the table to 0 KB free internal RAM:
//          every action changes /state, so every phone downloads it at once.
//          - The glow renderer's lookup tables (~18 KB: angle masks, radius,
//            profile and tone tables, row buffer) now live in PSRAM
//            (TR_TABLES_PSRAM 2; 1 = only the angle masks, 0 = all internal).
//            Measured on the table, busy play with 4 phones: lowest free 0 ->
//            24 KB, typical 21-28 -> 39-47 KB; 15.4 -> 15.1 fps.
//          - The tone map is rebuilt only when the mood color changes (was
//            1025 float entries every frame).
// v2.0-dev.17 (2026-11-04): SAVES OFF THE WEB SERVER'S LOOP. A save takes
//          300-700 ms (LittleFS write + rename) and ran inside loop(), between
//          networkHandle() calls, at most every SAVE_EVERY_MS (3 s) whenever
//          the game changed -- in busy play nearly every time -- so phones
//          waited for it (polls up to 2-4 s in the 2026-11-03 RAM tests; user:
//          "almost unacceptable"). gameStateSaveTick() now runs on its own
//          low-priority task (SAVE_TASK_STACK 4 KB, internal: flash writes turn
//          the PSRAM cache off); if the task can't start, loop() saves as before.
// v2.0-dev.18 (2026-11-04): RULES FIX -- the one-time Favorable / Allied VP
//          (+2 / +3) paid out the first time anything moved your standing with
//          your OWN faction (it starts Allied, but the "already paid" flags
//          started false) -- e.g. a Rebel capture of an Imperial world pleased
//          the Rebels: +5 VP for nothing. Tiers you start at now count as paid.
//          Twin test homeworlds.js checks the capture pays exactly its VP
//          (was 2 -> 13 VP for a 6 VP world).
// v2.0-dev.19 (2026-11-04): SMALLER /state (user: "makes sense to do someday").
//          Every action changes the state, so every phone downloads the whole
//          answer at once -- the Wi-Fi stack holds each in internal RAM while it
//          goes out, and the table answers one phone at a time. Planets were
//          ~6.6 of ~11 KB; now compact (11.0 -> 7.2 KB at the start of a game):
//          reveals as a bitmask + values only once revealed + costs, the market
//          good as an index into goodTab (name + effect once per good), flags
//          only when not their default. The phone's expandState() rebuilds the
//          old shape right after each poll, so no screen changed. Twin smoke.js
//          runs the page's own expandState() on a real answer.
// v2.0-dev.20 (2026-11-04): THE EMPIRE'S STRENGTH -- all its fleets hold 7 ships
//          (agreed 2026-10-29; the faction picker said "coming in the next
//          build"). FLEET_SHIPS is now the ROOM in the fleet arrays (7);
//          snapshotFleetCapacity() is what a player's fleets hold (Empire 7,
//          else 6) and every grow / found / Training check uses it. Battle
//          arrays: index = fleet * FLEET_SHIPS + slot, sent as shipStride; the
//          battle scene's ship spacing is 23 px (was 26) so a column of 7 fits
//          (phone + .ino twin). /state: players[].cap; the phone fleet screen
//          shows cap slots ("ESCORT D" is the 7th). SAVE_FORMAT 4 (old saves
//          are ignored). Twin test: tools/twin/sevenships.js.
// v2.0-dev.21 (2026-11-04): MARKET SHELVES on the phone (board M take 3, user:
//          "the shelves look awesome"). Markets = planks on a dark wall, each
//          good a long pixel object lying on its plank with its name stenciled
//          on it, stock lamps, BUY + price at the side (green = cheaper for you,
//          red = dearer, by your standing with the seller), sold out = a dusty
//          empty plank, "GONE". The four goods the mockup drew (missiles, fuel
//          tank, bacta tube, cargo crate) are ported from its generator; the other
//          eight (toolbox, probe, scroll, ship model, banner, shield emitter,
//          datapads, rifle rack) are new in the same style. One cached SVG per
//          shelf (shelfSvg). goodTab entries gain the GoodId + base price.
// v2.0-dev.22 (2026-11-04): TEXT FIX found writing the rulebook -- the Empire's
//          quest said "Hold both Core worlds"; since Kuat (dev.12) there are three
//          and the check (snapshotQuestProgress) always counted every Core world.
//          Now "Hold all three Core worlds". Balance question for the user: the
//          quest got harder when Kuat was added.
// v2.0-dev.23 (2026-11-04): VP SIMPLIFIED (user: "it feels like there's a
//          million, and they are complicated" -> agreed). VP now comes from:
//          worlds held (Outer 2 / Mid 3 / Core 5, ANY home world 4 to whoever
//          holds it), your quest (+5), destroying a player's fleet (+2), and the
//          drawn event's own reward (Unbound +1, Bounty +2, Ashen Fleet +2/+1).
//          CUT: Favorable +2 / Allied +3 (reputation pays through prices, battle
//          +1 and talks), +1 per kept promise, Best customer +2 / Merchant prince
//          +3 (and their counters), the 2 / 6 / 3 home-world split. Phone
//          wording (primer cards 1, 4, 10; home-world cards; markets; game over)
//          follows. SAVE_FORMAT 5. Twin homeworlds.js checks the 4s.
// v2.0-dev.24 (2026-11-04): THE ESCORT (user: "one ship is a great idea... you
//          get an escort"). Faction help in planet battles is a SHIP, not a
//          +1 on every roll: a +1 swung a 6-vs-4/5 fight from 62-75% to
//          93-96% and the players never saw where it came from. Favorable+
//          with the planet faction's rival -> that faction's ship ESCORTS you
//          (BATTLE_ESCORT_SHIPS 1); Hostile with the planet's own faction ->
//          a local MILITIA ship joins the garrison (BATTLE_MILITIA_SHIPS 1).
//          Both fight only this battle and take their side's FIRST loss (the
//          escort also covers a withdrawal), so a militia never stays in the
//          garrison and an escort never joins your fleet. Simulated: about
//          +10% instead of +30%. Still only once Affiliation is revealed.
//          GOOD_MERCENARIES stays a +1 roll for now (user to decide).
//          GameState: battleEscortFaction/Alive, battleMilitiaFaction/Alive
//          (old saves ignored -- size changed). /state: bEsc / bMil, fightMods
//          esc / mil. Phone: forecast shows "+1 ESCORT" (ship + insignia) and
//          "+1 MILITIA"; the battle scene draws the escort outlined one column
//          in, the militia as the top garrison square in its faction color.
//          Table: the escort counts on the ship wedges. Twin: escort.js.
// v2.0-dev.25 (2026-11-04): the MILITIA rises even when you haven't revealed
//          the world's Sympathy (user: "militia should apply if you haven't
//          revealed"). The escort still needs it. The forecast hides an
//          unrevealed militia (fightMods mil 0) so it can't leak the Sympathy;
//          you meet it in the battle. escort.js covers it.
// v2.0-dev.26 (2026-11-04): MERCENARIES = 2 HIRED SHIPS (user: "switch to 2
//          hired ships, thats great"), not +1 to your rolls. Bought from a
//          market (MERC_SHIPS 2, at most MERC_SHIPS_MAX 4 saved up), they fight
//          your next battle -- planet or PvP, attacking or defending -- take
//          your side's hits FIRST (before an escort; they also cover a
//          withdrawal) and leave after. Player.nextBattleMod -> nextBattleMercs
//          (now a ship count); GameState battleAtkMercs / battleDefMercs;
//          coverLossLocked() = hired ships, then the escort. PvP has no roll
//          modifiers left at all. /state bMerc, fightMods merc; phone forecast
//          "+2 HIRED" (planet + PvP), battle scene draws them tan one column in;
//          table counts them. Twin: mercs.js.
// v2.0-dev.27 (2026-11-04): FORECAST V2 + THE INFORMANT (mockup approved:
//          "battle forcast plus the world looks good"). The forecast shows the
//          chance to win the WHOLE battle (your ships + escort + hired vs the
//          garrison) instead of one exchange's 42%, as a range over the garrison
//          sizes you can't rule out (snapshotGarrisonRange: ring, home guard,
//          revealed Defense, a militia that is certain / possible) plus a word:
//          STRONG >=85 / FAVORED >=60 / RISKY >=35 / LONG SHOT, UNCLEAR when the
//          range is wider than 30 points. PvP odds are exact (their fleet is
//          visible). THE INFORMANT: reveals during Resolution cost
//          INFORMANT_COST_MULT (3) x the Prepare price; the forecast offers
//          Sympathy and Defense, each with its faction's own name (Rebels: a
//          sympathizer talks / send pathfinders; Empire: pull the ISB file /
//          launch a probe droid; Pykes: a spice runner owes you / bribe the
//          dockmaster; Crimson Dawn: call in a favor / slice their records).
//          Twin: forecast.js.
// v2.0-dev.28 (2026-11-04): AUTO + CHOOSING TO USE MERCENARIES (user: "auto is
//          great, it should only stop if YOU are at 2 ships, so you can choose
//          to escape"; "there needs to be a 'use mercenaries' before a battle").
//          AUTO (phone): after each exchange lands it fights again, until your
//          side (fleet + escort + hired) is down to 2 -- then it stops and asks
//          "fight on, or withdraw?"; STOP AUTO any time; PvP too. Mercenaries:
//          the forecast (planet and PvP) shows them as a toggle, on by default --
//          tap "HIRED SHIPS STAY HOME" to save them (fleet mask bit
//          FIGHT_LEAVE_MERCS 0x40 on /takeover_commit and /pvp_attack). A PvP
//          defender's hired ships no longer join by themselves: "+ CALL YOUR
//          HIRED SHIPS" until the first exchange (gameStateCallMercs,
//          RF_CALL_MERCS, /call_mercs, canCallMercs). Twin: mercs.js.
// v2.0-dev.29 (2026-11-04): "IF YOU ATTACK HERE" AS ONE LINE + FAVOR CARDS.
//          Every planet page you don't hold shows one line: your best fleet's
//          word (STRONG...LONG SHOT / UNCLEAR) + escort / hired / MILITIA(?)
//          icons; tap = the full forecast in Resolution, the per-fleet details
//          in Prepare / Movement (user: "tap to see the full broadcast makes
//          me happy"). /state planets carry "gr" [lo,hi] = snapshotGarrisonRange
//          for the viewer. Prepare Home shows each promise you owe as a card
//          (faction, world, text, 3-segment countdown, game-checked or honor)
//          -- user: "we should add that for all favors as well".
// v2.0-dev.30 (2026-11-04): SHIP ART in the phone battle (mockup "Ship art"
//          take 2, user: "I like it"): original pixel ships per faction --
//          worn hulls, faction color as markings (Rebel fighter, Imperial
//          wedge, Pyke hauler, Crimson Dawn yacht), a rusty mercenary gunship,
//          the escort outlined in white -- and a GARRISON KIND per world:
//          Outer Rim asteroid outpost / picket gunboat, Mid Rim turret platform
//          / ion battery, Core heavy station (neutral gray, never shows a
//          Sympathy), home guard = the home faction's ships, militia = a
//          platform in its faction's colors. Sprites are GENERATED into
//          web_server.cpp SPRITES from tools/art/ships/make_ships.py. The
//          Rebel ship stays an original design (user: "we can leave it for
//          now", re the X-wing). Phone only; the table + 1.75" keep shapes.
// v2.0-dev.31 (2026-11-04, after testing dev.30 on the table): MARKETS
//          (user: "needs the most help") -- the four stalls are a 2x2 grid on
//          one screen (no scrolling), stock = two lamps + "2 LEFT" / SOLD OUT,
//          the confusing "no holder, no cut" is gone; one line explains that
//          holding a market world earns 1 CR per sale (2 on a Core world), and
//          each stall / the planet page's market card says who earns it.
//          INTEL: Resources and Trait are PUBLIC from the start (agreed
//          2026-11-04, "everyone knows the planet is swampy"; it wasn't built
//          yet -- user asked) -- only Sympathy and Defense are bought; primer
//          card updated. ATTACK LINE: the label on its own row so it fits;
//          "MILITIA?" dropped (user: not needed).
// v2.0-dev.32 (2026-11-04): MARKETS ON / OFF in the Lobby (user: "can we add
//          a toggle to turn off the market for a game? ... a little too much
//          for some people's first game"). GameState.marketsOn (Lobby only,
//          kept between games like the game length), gameStateSetMarkets /
//          RF_SET_MARKETS / /set_markets, /state "mkt". Off = no market opens,
//          no MARKETS tile, the Black Market trait isn't dealt, no "market cut"
//          deals, and Crimson Dawn's Silent Partners (a cut of other players'
//          purchases) becomes +1 CR every Prepare (CRIMSON_NOMARKET_INCOME) so
//          the faction keeps a strength. Twin: nomarkets.js.
// v2.0-dev.33 (2026-11-04): GALAXY EVENT ON / OFF in the Lobby (user: "not
//          sure if we are ready for mid game events on this playtest").
//          GameState.eventsOn (Lobby only, kept between games); off = the
//          halfway event is never drawn (maybeTriggerEventLocked).
//          gameStateSetEvents / RF_SET_EVENTS / /set_events, /state "evt".
//          Twin: nomarkets.js checks it too.
// v2.0-dev.34 (2026-11-04): CRASH FIX from the playtest -- the table restarted
//          in a battle and crash-looped on resume (3 identical records: loopTask,
//          LoadProhibited at address 0x1 inside snprintf, from
//          gameStateBattleAction). Battle ship indexes run fleet * FLEET_SHIPS +
//          slot (0..20) since v0.28.0's combined fleets, but the battle text
//          looked the lost ship's name up in ATK_SHIP_NAMES (7 names) -- losing
//          a Fleet 2 / 3 ship read past the array. Not RAM (lowest free 16 KB).
//          shipNameOf() wraps the index (both places). Twin: fleet2.js.
//          Also from the playtest: the lock-in confirm only mentions unspent
//          Credits in Prepare (Movement / Resolution "should just show
//          fleets"), and the forecast warns of a BETRAYAL when the world's
//          faction is Favorable / Allied with you (they snap to Neutral).
// v2.0-dev.35 (2026-11-04): UI 3.0 STEP 1 -- THE FULL-SCREEN BATTLE (mockup
//          "UI 3.0" board 5, user: "big fan, no notes"; after the playtest:
//          "the whole screen during the actual fight feels really small"). The
//          phone battle covers the whole screen; the scene is PORTRAIT
//          (SCENE_W 200 x SCENE_H 300, was 380 x 200) and scaled to fit the
//          canvas, so ships, dice and the planet are about twice as big; big
//          title, a big result line, FIGHT / AUTO side by side and WITHDRAW /
//          CONTINUE full width. Phone only (the AMOLED keeps its own scene).
// v2.0-dev.36 (2026-11-04): UI 3.0 STEP 2 -- RESOLUTION = THE GALAXY (mockup
//          board 4; user: the galaxy is "my favorite screen", the target list
//          was "too much hand holding"). In Resolution the phone's Home IS the
//          map: worlds wear a halo in your odds' color (STRONG / FAVORED /
//          RISKY / LONG SHOT), rival fleets sit as chips at their hangars, and
//          a tap opens a sheet under the map -- a world: worth, earnings,
//          trait, market, your odds, SEND SOMEONE IN (the informants) and
//          FIGHT / TALK / PAY; a rival fleet: exact odds, +2 VP, ATTACK (into
//          the player-vs-player forecast). FACTIONS / STANDINGS and DONE THIS
//          ROUND under it. Phone only.
//          Follow-ups the same night (user on an iPhone): smaller round worlds
//          on a full-width map, ONE ring per world = who holds it (odds colors
//          "look too much like who owns what" -- tap a world to see them), and
//          the rival fleets folded behind one ATTACK A PLAYER button.
// v2.0-dev.37 (2026-11-04): UI 3.0 STEP 3 -- PREPARE = YOUR HANGAR (mockup
//          board 1; user: "a big fan of your hangar"). Prepare's Home: VP +
//          rank and Credits, then YOUR HANGAR -- each fleet a bay of real ship
//          sprites; tap an empty slot to build (+2), a lost ship to repair
//          (FIX 2), an empty / destroyed bay to found / rebuild (all
//          gameStateInvestFleet, 2 CR). Then one-line bars for the markets,
//          waiting deals and your nearest promise, and the round GALAXY button
//          with a TURNING planet (the 1.75"'s Home) between FACTIONS and
//          STANDINGS, LOCK IN under it. Phone only.
// v2.0-dev.38 (2026-11-04): UI 3.0 TAKE 4 -- RESOLUTION WORKS LIKE PREPARE
//          (user: the custom Resolution map "isn't panning out"; "pressing the
//          galaxy map in prepare and resolution need to be the same"; mockups A
//          + B: "lets build both"). The dev.36 Resolution map + sheets are gone.
//          Resolution's Home = the hangar layout: stat boxes, YOUR FLEETS (bays,
//          READY / SPENT, read-only), hired ships, the GALAXY button (orange),
//          a small ATTACK A PLAYER (its own screen, dev.37b) and DONE THIS
//          ROUND. The one glowing Galaxy map serves every phase; a world's page
//          in Resolution shows what's known as tiles, your odds, SEND SOMEONE IN
//          (the informants), its market and big FIGHT / TALK / PAY. Also: "tap
//          to open the market" (was "tap a stall to buy").
// v2.0-dev.39 (2026-11-05): UI 3.0 STEP 4 -- MOVEMENT + THE HOLONET RUMOR
//          (user: "yes sounds great", keep a galaxy button "so you can remind
//          yourself the affiliation of a planet"). Movement's Home = the same
//          layout: stat boxes, "MOVE YOUR SHIPS ON THE BOARD", YOUR FLEETS
//          (read-only), a HOLONET RUMOR card, the GALAXY button (cyan), DONE
//          MOVING. The rumor: snapshotRumor() picks one TRUE fact some player
//          is up to, never who (hired ships waiting, 20+ Credits, Allied with a
//          faction, made enemies of one, owes a favor, a fleet limping, looked
//          into a world's defenses, one step from a quest) -- built from the
//          state, no new saved data, the same pick for everyone all round;
//          /state "rumor" in Movement. Phone only (the table shows it later,
//          with the table announcements).
// v2.0-dev.40 (2026-11-05): MARKET PRICES NO LONGER LEAK SYMPATHY (user: "the
//          market gives away affiliations, i don't love that"). A stall's price
//          moved with your tier toward the world's hidden faction, so cheap /
//          pricey gave it away. Now standing changes the price only once you've
//          revealed that world's Sympathy (or hold it) -- the Pay rule.
// v2.0-dev.41 (2026-11-05): THE HOLONET GETS A VOICE (user: "someone is pretty
//          interested in kijimi, huh?", "maybe the table needs a personality";
//          a rumor about your world "should be clear to you that its a planet
//          you own, but not necessarily the whole table"). Rumors are sly gossip,
//          still always true and never who. TARGETED first: someone BOUGHT intel
//          on a world another player holds since the last Movement ->
//          "Someone is pretty interested in Kijimi, huh?" (home world: "casing
//          ... Bold."); the owner's phone adds "KIJIMI IS YOUR WORLD. Only you
//          see this line." WORLD LEAKS (work with 2 players): an unclaimed
//          world's thin garrison, one world's hidden leaning. Then player gossip
//          (exact Credits now). New Player.intelEyed (bit per world, cleared when
//          Resolution starts) -> SAVE_FORMAT 6, old saves don't load.
// v3.1-dev.1 (2026-11-06): UI 3.1 STAGE 0, part 1 -- SMOOTH SCREENS (user: "smooth
//          animations everywhere, nothing feels jittery or disjointed"). The
//          firmware line becomes 3.1-dev.N (2.0-dev.39-41 were never flashed,
//          by choice). setApp() no longer rebuilds the screen: it parses the
//          new HTML off-screen and PATCHES only what differs (attributes,
//          text, added / removed nodes; elements with the same id move instead
//          of being rebuilt). Canvases keep their pixels, a focused text box
//          keeps what's being typed, data-keep="1" protects a subtree. So
//          elements survive polls: transitions and animations finish instead
//          of restarting, taps never land on a vanishing button. Motion tokens
//          (--ease, 150/250/400 ms) + corner tokens from the 3.1 style guide;
//          buttons and cards ease their color / opacity changes and press in a
//          little; reduced-motion phones get near-instant changes. No rule
//          or game-state change (saves from dev.41 still load).
// v3.1-dev.2 (2026-11-06): UI 3.1 STAGE 0, part 2 -- LAYERS. Two new layers on
//          the phone, each its own root so neither ever rebuilds the screen:
//          the SHEET (every decision slides up from the bottom; the screen
//          behind settles back to 95% and dims; tapping it = the sheet's
//          dismiss) and NOTIFICATIONS (news drops in from the top, stacks up
//          to 3, newest on top, fades away on its own or on a tap). The LOCK
//          IN? confirm is the first sheet (was its own screen); the old NOTICE
//          / LOOK AT THE TABLE popup is now a notification. A sheet closes by
//          itself if its screen goes away (render() wraps renderScreen()).
//          Harness: new mode `notifs`; screenshots skip CSS transitions.
// v3.1-dev.3 (2026-11-07): UI 3.1 STAGE 0, part 3 -- THE EVENT LOG + THE FEED.
//          (1) GameState now lives in PSRAM (psNew<> in game_state.cpp; PSRAM
//          is up before C++ globals, CONFIG_SPIRAM_BOOT_INIT), and so do the
//          save task's snapshot and the restore buffer: the table's internal
//          RAM in globals dropped 149,828 -> 134,156 bytes (~15.7 KB freed).
//          (2) An EVENT LOG beside the state (EventLog, PSRAM, ~2.2 KB, NOT
//          inside GameState so its many copies don't grow): a public ring of
//          64 (LOG_ROUND, LOG_TAKE with how -- fight / talk / pay --,
//          LOG_RETURNED, LOG_MOMENT for every public big moment) and a private
//          ring of 16 per player (LOG_LOST, LOG_NOTE with the last 4 notice
//          texts, LOG_PROMISE kept / broken); an epoch per game. Saved with
//          the game (SAVE_FORMAT 7; older saves don't load). (3) /state adds
//          "ev": only events newer than the phone's es / em (eg = epoch; a
//          stale epoch gets everything). (4) Phone: the FEED -- a bell with an
//          unread count in Home's top bar (the first pixel icon), INBOX (your
//          events, newest first, unread glow) and HISTORY (public moves by
//          round, yours in gold); new private events arrive as top
//          notifications (the old note popup is retired). Twin test: feed.js.
// v3.1-dev.4 (2026-11-07): UI 3.1 STAGE 0, part 4 -- THE DISPLAY FONT. Phones on the
//          table's own Wi-Fi have no internet, so the table now serves Space
//          Grotesk (latin, variable 300-700, 22 KB, SIL OFL 1.1 -- tools/fonts/
//          OFL.txt) at /font.woff2, cached for a year. sf_web_font.h is
//          GENERATED by tools/fonts/make_font_h.py. Titles, buttons, labels and
//          the top bar use it (--display); reading text keeps the phone's own
//          font. The twin shim gained a binary send_P.
// v3.1-dev.5 (2026-11-07): UI 3.1 STAGE 0, part 5 -- RETRO CHARM IN THE PAGE. The
//          whole original 12x12 pixel ICON SET from the style guide (tiers 1-5,
//          crown, target, war, summit, job, commotion, race, heist, cover,
//          credits, victory, gift, holonet, you owe, fleet, era, bell; Partner
//          redrawn as a chain link) as PIX + pixIcon(). The pixel SCOREBOARD:
//          5x7 numerals (pnum()), used by Home's CREDITS / VICTORY PTS / PLANETS
//          boxes (now centered -- layout rule). A changed value COUNTS to its
//          new number (~420 ms ease-out) and thumps; the span is data-keep so a
//          poll's patch can't cut the count short; the clock starts at the first
//          frame and a safety timer lands the final value if frames stall (both
//          bugs found by the new harness mode `countup`). Patcher test skips the
//          inside of data-keep elements (it still compares their attributes).
// v3.1-dev.6 (2026-11-07): UI 3.1 STAGE 1, part 1 -- THE 16 CHARACTERS. Character
//          id = faction * 4 + class (WARLORD / CONSUL / INFILTRATOR / SCOUNDREL);
//          each faction's missing class is a DEFECTOR (Danto ex-Empire, Merrow
//          ex-Rebel, Nyra ex-Crimson Dawn, Korrik ex-Pyke). commander_art.h:
//          NUM_COMMANDERS 16, 4 per faction, CHARACTER_DEFECTOR_FROM, voices =
//          FACTION_VOICES (a born Consul/Warlord, never a defector or you);
//          commander_art.cpp re-exported (16 faces incl. the 4 defectors).
//          Defectors start at REP_START_DEFECTOR_OLD (+8, top of Neutral) with
//          their old faction and know its worlds' Sympathy from turn one (OLD
//          CONTACTS). /choose_character picks faction + character in one tap.
//          Phone: ONE character screen (4x4 grid, taken rows dimmed, defector
//          dots in their old color, a preview with story, facts and class kit)
//          replaces the faction + commander steps; Lobby CHANGE CHARACTER; your
//          face in Home's top bar opens YOUR CARD (quirk, homeworld / age /
//          ship, story, great at / hurts / once a game / quest). The class kit
//          is SHOWN here; its rules arrive in dev.7, the moves in stage 5.
// v3.1-dev.7 (2026-11-07): UI 3.1 STAGE 1, part 2 -- CLASS RULES + QUESTS.
//          WARLORD +1 extra ship in a battle you start / FEARED: taking a world
//          by force costs 10 more standing. CONSUL talks ask 1 fewer question
//          (min 2) / SOFT HANDS: the defender gets an extra ship in a battle
//          you start. INFILTRATOR: the first intel each round is free (Prepare
//          and Movement) / SPREAD THIN: whoever attacks your world gets an extra
//          ship. SCOUNDREL: market goods 1 less / talks ask 1 more question.
//          Class extra ships (battleAtkExtra / battleDefExtra) take the first
//          hits and are drawn with the hired ships; the phone's odds include
//          them (classAtk()). Every firing writes a LOG_STAMP event -> a stamp
//          notification. QUESTS are the class's (Conqueror 4 by force,
//          Kingmaker 4 by talk or pay, Spymaster 12 Sympathies known, Make a
//          fortune 30 Credits at once) and pay +5 VP the moment they're done
//          (checked at every phase change; LOG_QUEST tells everyone). Player
//          forceTakes / peaceTakes / peakCredits; SAVE_FORMAT 8. Standings and
//          your card show the quest with a progress bar. Faction strengths
//          (Rebel sympathizers etc.) still run until the ship-quirk stage.
// v3.1-dev.8 (2026-11-07): UI 3.1 STAGE 2 -- THE FAVOR SPINE.
//          Five tiers per faction with each faction's own words (Collaborator ..
//          Hero of the Rebellion, Enemy of the Empire .. Hero of the Empire,
//          Collateral .. Family, A loose end .. Inner circle). START: your home
//          faction at the TOP of tier 3 (score 9), its rival in tier 2 (-20), the
//          others 0; a defector also starts at 9 with the faction they left.
//          GIFTS (earned, never handed out): crossing into tier 4 the first time
//          gives that faction's one-time gift (Empire: Defense of 2 worlds,
//          Rebels: a free ship, Pykes: +6 CR, Dawn: Sympathy of 2 worlds;
//          Player.giftsGot); tier 5 holds an ongoing gift that goes when you drop
//          (Empire: a ship of the line in every battle you fight, Rebels: your
//          best world defends with +2 ships, Pykes: +1 income, Dawn: a spy at
//          court shows one player's tiers each round -- Player.spyOn). LOG_GIFT
//          events -> notifications; stamps SHIP OF THE LINE / SAFE WORLD.
//          PAY by tier (once Sympathy is known): +2 / +1 / 0 / -1 / -2. MARKET
//          prices no longer move with favor: a stall is SHUT to you at tier 2 or
//          below (mShut; your own world always sells). DEBTS: a world won on a
//          favor gives its VP at once; keep your word = favor up, break it =
//          they take it back (planetVpPending now only marks the debt).
//          Phone: FAVOR page (tier icon + word, bar, OUT FOR YOU / gift lines,
//          YOU OWE), the LADDER bottom sheet per faction, Home's THE GALAXY AND
//          YOU rows (out for you / you owe / your spy), shut stalls dashed,
//          transmissions use the faction's tier words. SAVE_FORMAT 9.
//          Not yet (later stages): grudge strikes at tier 1, champions + Eras,
//          outfit palette swaps on portraits.
// v3.1-dev.9 (2026-11-07): FACTION STALLS + the safe world (user 2026-11-07).
//          Markets: the FACTIONS sell, not the worlds -- one stall per faction
//          with three goods of its own (FACTION_GOODS: Rebels Bacta / Smuggled
//          ship / Safe harbor, Empire Repairs / Garrison troops / Survey, Pykes
//          Fuel / Cargo run / Forged permits, Crimson Dawn Mercenaries /
//          Blessing / Audience), two out each round, 2 of each. Shut to you at
//          tier 2 or below; tier 4 pays 1 CR less, tier 5 2 less (user: "the
//          price can fluctuate with your standing"). SURVEY asks which world,
//          AUDIENCE which faction (bottom sheets). The MARKET CUT is gone (and
//          the deal ask for it); the Black Market trait still pays its holder.
//          /stall_buy, RF_STALL_BUY; the 2.0 world markets stay closed
//          (marketOpen all -1, kept for the 1.75"). Safe world (Rebels tier 5)
//          = your HOME world, else your most valuable. The spy at court stays
//          (it was in the approved ladder). SAVE_FORMAT 10.
// v3.1-dev.10 (2026-11-07): UI 3.1 STAGE 3 -- GALAXY NEWS, GRUDGES, JOBS
//          (vision doc tab "Galaxy News"). Every round start: one TABLE-WIDE item
//          (early: commotion / reinforcements / a race / market day; middle:
//          hiring / embargo / a race / reinforcements; late: offensive /
//          desperation / last stand / a race; never the same twice in a row)
//          shown big on the table (publicMoment = PUBLIC_NEWS_BASE + NewsId,
//          snapshotNewsText), plus ONE PERSONAL item per player (a debt due, "they're
//          watching" at tier 2, a JOB offer, a TIP-OFF from a tier-4+ contact).
//          GRUDGES: tier 1 with a faction = a warning ("coming for you", why,
//          how to stop it), then the strike lands next round unless you climbed
//          out: Empire fleet retakes a world (NPC battle, 4 ships), Rebel
//          uprising (-2 garrison), Pyke shakedown (up to 5 CR), Crimson Dawn
//          assassins ambush your biggest fleet (3 ships, a free shot first). One
//          warning a round, never round 1 or the last round, 4 a game, 3-round
//          cooldown per faction. NPC battles resolve AT ONCE (autoBattle: a die
//          each, ties to the defender) -- a full battle screen is a later stage.
//          JOBS: take a world within 2 rounds for +6 favor / +4 CR (fail -2
//          favor), offered fairly (fewest offers first), announced ("the X are
//          hiring") or secret; TAKE / PASS sheet; /job. RACES: first to take a
//          world = +6 favor / +3 CR; DESPERATION: any world of the faction's
//          rival = +8 / +8. COMMOTION changes an unclaimed world's Sympathy and
//          weakens it; only players who knew its Sympathy learn who. EMBARGO
//          shuts a stall to all; MARKET DAY puts out a stall's 3rd good (1).
//          Phone: the ANNOUNCEMENT LAYER (#lyrAnn, full screen, one at a time,
//          this round's unseen items, never over a battle), Home rows STRIKE NEXT
//          ROUND / JOB / RACE, LOG_NEWS + LOG_PNEWS in the Feed. SAVE_FORMAT 11.
// v3.1-dev.11 (2026-11-07): STRIKES DO FIXED DAMAGE, NAMED A ROUND AHEAD, AND
//          ANIMATE (user: "fast, but there needs to be some kind of animation";
//          agreed: fixed damage). The warning names the target and the cost:
//          Empire -3 garrison on your weakest non-home world (0 = it falls back
//          to the Empire with FALLEN_GARRISON), Rebel uprising -2 on your
//          strongest (never below 1), Pyke shakedown up to 5 CR, Dawn assassins
//          -2 ships from your biggest fleet (never its last). Late OFFENSIVES are
//          now warned too (NEWS_OFFENSIVE = they mass, NEWS_OFFENSIVE_LANDS next
//          round: -3, 0 = it falls). A home world is never taken by a strike.
//          PN_STRIKE_HIT / PN_OFFENSIVE_HIT log other = before*10 + after; the
//          phone plays it: their ships sweep in, bolts, your garrison / ships /
//          Credits count down in pixel numerals, then the result (tap skips).
//          Home rows name the target. The dice NPC battle is gone. SAVE_FORMAT 12.
// v3.1-dev.12 (2026-11-07): UI 3.1 STAGE 4 -- CHAMPIONS + ERAS. A faction's
//          CHAMPION = whoever is at tier 5 with it with the most favor (ties keep
//          the holder; public, announced). Gaining the title = ONE DECREE (lose
//          the title, lose the decree). Declaring it starts that faction's ERA
//          for half the game (a new decree replaces the current Era): one rule
//          for everyone, one STING its tier-5 players are spared from, a perk for
//          its champion. REBELLION RISES: Hope spreads (talks -1 question, a free
//          repair each round) / the people won't be bought (no Pay) / champion's
//          talk-won worlds +2. IMPERIAL ORDER: Imperial records (one Defense a
//          round) / the tax rolls (1 CR per world held) / champion pays none and
//          collects half. SPICE AGE: spice flows (+2 income, all 3 goods at every
//          stall) / war is bad for business (ships +1) / champion Pay -3. SHADOW
//          AGE: whispers (intel reveals a 2nd secret) / nobody knows the score
//          (others' VP hidden) / champion sees it and writes one Holonet lie a
//          round (canned or typed, /lie) that airs beside the true rumor.
//          Phone: Era card (rules with a tick / cross for you), Home Era banner +
//          rules sheet, YOUR DECREE row + sheet, Favor page crown + DECLARE, the
//          lie sheet, hidden VP as '?', the Era's thin colored frame on every
//          screen, the galaxy map's lines in the Era color. Table: Era and
//          champion headlines. SAVE_FORMAT 13. /decree, /lie.
// v3.1-dev.13 (2026-11-07): UI 3.1 STAGE 5 -- THE ONCE-A-GAME MOVES (from round 3).
//          WARLORD DECLARE WAR: in a Resolution, name any world -> everyone hears
//          it (the holder: "reinforce it") -> next round's Resolution the WAR
//          ROOM: pull ships from your worlds' garrisons (each keeps 1, cap 12),
//          +2, one fight resolved at once and played back exchange by exchange
//          on every phone; survivors go home, a taken world keeps the leftovers
//          (min 1). Unopened = it lapses. CONSUL SUMMIT: up to 3 worlds, 2 rounds,
//          no fights there; the Consul talks -1 question and Pays 2 less; break
//          it (ATTACK ANYWAY) = 6 peacekeeper ships defend + -10 favor with the 3
//          factions that aren't the world's rival; held = +10 with all four. A
//          Summit on a war world: in the War Room the Warlord MARCHES ANYWAY or
//          PULLS BACK (declaration returned). INFILTRATOR DEEP COVER: 3 rounds as
//          Family of another faction (favor set to 35, no gifts, no titles; real
//          favor back after, keeping gains); plant ONE Holonet story (60 chars,
//          airs this Movement); attacking their world (warned, ATTACK ANYWAY) =
//          cover blown, a tier down, the table hears it. SCOUNDREL THE HEIST: a
//          vault (another faction): SMOOTH +5 CR + a free good, anonymous; BOLD a
//          die, 3-6 +10 CR + a good + a stolen ship, 1-2 +3 CR and a tier down;
//          the die rolls on your phone. Also fixed: Feed / news names were all
//          lowercase (a literal backspace in feedTitle's regex). SAVE_FORMAT 14.
// v3.1-dev.14 (2026-11-07): UI 3.1 STAGE 6 -- THE GALAXY MAP WINDOW. Today's map
//          (same SVG glow, rings, lanes, planet sprites -- identical by
//          construction, not redrawn) now sits in a square WINDOW across the
//          middle of the screen: pinch or drag to zoom (1-3.2x) and pan (one GPU
//          transform on the whole layer; a wheel zooms on desktop; + / - / fit
//          buttons), filters above it -- ALL / THREATS (your worlds under war, an
//          offensive, a strike, or a thin garrison) / CHANCES (races, your job,
//          the desperate faction's targets, unclaimed worlds of a faction that
//          likes you) / MINE -- that light the matching worlds and dim the rest,
//          with a list below. Tapping a world opens its SHEET (sprite, holder, VP,
//          ring, the four intel tiles, why it's marked) with OPEN <WORLD> to its
//          full page. A drag never counts as a tap. The Era still recolors the
//          map's lines. (A canvas rewrite was planned; keeping the HTML map
//          guarantees it matches today's.)
// v3.1-dev.15 (2026-11-07): UI 3.1 STAGE 7 -- FINISH. SHIP QUIRKS (by your
//          faction's ship style, agreed 2026-11-06), each stamped when it fires:
//          Imperial RANKS (stamp on a fleet's 7th ship), Rebel SLIPPERY (lose a
//          battle and one ship escapes -- in PvP that means no fleet kill), Pyke
//          ARMORED CARGO (the first hit on your fleet each battle is shrugged
//          off), Crimson Dawn AMBUSHERS (an opening-exchange tie is their hit).
//          TABLE MOMENT QUEUE: Galaxy News / Era / champion / move moments wait
//          their turn (TABLE_MOMENT_MS each, never over a battle) instead of
//          overwriting each other; drained in gameStateTick. CLEANUP: the old
//          2.0 faction strengths are gone (Pyke +2 income, Crimson silent
//          partners, Rebel sympathizers); DEALS are gone (the vision doc cut
//          them: no entry points, the server refuses offers). Phone: the HOW TO
//          PLAY primer is 3.1 (14 cards: + your character, Galaxy News, Eras;
//          favor tiers, faction stalls, the Feed); GAME OVER shows each class
//          quest, YOUR STORY (from the event log: worlds by force / talk / Pay,
//          losses, quest, titles, Eras, your move, strikes), HOW THE GAME WAS WON
//          and the Eras; WELCOME BACK shows your character and unread Feed.
//          Fixed for good: feedTitle no longer needs a backslash (shell-passed
//          scripts collapse '\' -- write edit scripts with the Write tool).
// v3.1-dev.16 (2026-11-07, from the user's own test): NO Galaxy News in round 1.
//          The round's news arrives on ONE screen like the mockup (GALAXY NEWS:
//          numbered items, THE WHOLE TABLE / JUST FOR YOU, the commotion's
//          "only you know" under it, the job with TAKE IT / PASS / SEE IT ON THE
//          MAP, then TO PREPARE); a strike landing still animates first and
//          mid-round moments keep their own cards. JOBS no longer pop up: an
//          offer waits on Home and in the Inbox (tappable) until PREPARE ENDS,
//          then lapses; taking, refusing or letting it lapse is logged
//          (PN_JOB_TAKEN / PN_JOB_PASSED) and shows in YOUR History only, in
//          gold, with your cover and heist. SAVE_FORMAT 15.
// v3.1-dev.17 (2026-11-07, user feedback): CLEANUP + POLISH. The DIPLOMAT is
//          gone (DIPLOMAT_ON 0; the informant replaced it). ONE WORLD VIEW: a
//          tap on the map / a filter list / a job opens the world's page, which
//          now slides up like a sheet (the little popup is gone), with a few
//          lines of STORY per world (WORLD_LORE), the VP as a gold badge on the
//          planet (no more "WORTH n VP" in the sub line or the tiles), and the
//          public tiles (Resources, Trait) on top, the secrets (Sympathy,
//          Defense) below; the trait's effect shows on the page. The STORE is
//          the mockup's look (a card per faction, faction-colored BUY, dashed
//          "opens at ..." when shut) and sells ONE good per faction a round,
//          never the same one two rounds running (Spice Age: two). The phase
//          change is the HYPERSPACE WIPE (pixel streaks, then the round in pixel
//          numerals and the phase in its color). The battle's result line has a
//          fixed height (no more resize on the last exchange). Feed / news use
//          CHARACTER names, not factions. RACE rows say they're Galaxy News.
//          SAVE_FORMAT 16.
// v3.1-dev.18 (2026-11-07, user feedback): FEELS LIKE AN APP -- "Add to Home
//          Screen" opens full screen with no address bar (apple-mobile-web-app
//          + /manifest.json standalone + /icon.png, an original pixel planet,
//          GENERATED by tools/icon/make_icon.py into sf_web_icon.h), safe-area
//          padding, and the phone's BACK button / edge swipe steps back inside
//          the game (history trap -> appBack) instead of leaving the page. The
//          ONCE-A-GAME move is explained on your card, the character select, the
//          Home row and its sheet (CLASSES moveText). The won-a-talk screen no
//          longer says "its VP comes when you do it" (the VP comes at once since
//          dev.8). "On your honor" labels are gone (every favor is game-checked).
//          The HYPERSPACE jump is radial now: stars at every angle stretch into
//          lines racing outward, a flash, then the phase. Galaxy events are OFF
//          by default (the Lobby switch still turns them on).
// v3.1-dev.19 (2026-11-07): TRAITS YOU CAN SEE (user: "if we are going to have
//          a mechanic, you need to feel/see it"). Only six are dealt now --
//          Refinery (+2 CR a round, named on the Prepare banner under the
//          income), Shipyards (ships 1 CR, named on the hangar price), Ancient
//          power (a reroll -- stamp REROLL), Court (favor each Prepare -- stamp
//          THE COURT), Medical (a free repair -- stamp MEDICAL BAY), Training (a
//          new fleet's free ship -- stamp TRAINING). Cut: Spy network (= the
//          Infiltrator), Black market (= the old market cut), Hidden base, Trade
//          hub / Mines (folded into the Refinery). Numbers wait for the balance
//          sweep. SWIPE DOWN closes any bottom sheet and a world's page.
// v3.1-dev.20 (2026-11-07): MOCKUP SWEEP + LEFTOVER SWEEP (user picks). Added:
//          WHAT EACH CHOICE DOES TO YOUR FAVOR under a world's FIGHT / TALK /
//          PAY (arrows, betrayal, questions, price; mockup World.dc.html); the
//          Era banner spells out its rules (and "you're spared"); DEEP COVER on
//          Home lists what you get and what blows it; a GRUDGE warning is a
//          hologram of the faction's voice typing its threat, with PAY THEM OFF
//          (GRUDGE_PAYOFF_CR 12, user: "expensive") -- stops this strike, starts
//          the cooldown, favor unchanged; THE HEIST'S PUSH YOUR LUCK after a bold
//          success (this round: 3-6 +10 more CR, 1-2 lose the 10 and a tier).
//          Cut: DEALS (phone screen + /state data + endpoints; the engine code
//          stays inert for the 1.75"), the 2.0 faction quests / strengths list,
//          the old pre-3.0 Home tile layout, the DIPLOMAT (UI, data, endpoint),
//          I DID IT / I DIDN'T (an honor promise now counts as kept). Fixed: the
//          character select's start line says the 3.1 favor words, not
//          Allied / Hostile. SAVE_FORMAT 17.
// v3.1-dev.21 (2026-11-07, user: "battles on the table are so hard to follow, they
//          need to be bigger"): the table's battle dice are about twice the size
//          (PvP 62 px, planet battles 52 px), and each side's ships are a BIG
//          number in the 36 pt font facing that side (the garrison side counts
//          militia + hired + extra ships too) instead of small wedges / squares.
//          Shake-down so far (tools/shakedown/bot.py): 2p and 4p bot games ran
//          8 rounds each with no problems, RAM lowest 34 KB.
// v3.1-dev.22 (2026-11-07): NUMBERS PASS #1 from the shake-down (6 bot games: 2 x 2p,
//          4 x 4p, all clean; a 19-minute soak with no reboot, RAM lowest 30 KB; 6
//          mid-game restarts, all resumed exactly). The one clear outlier: the
//          Infiltrator's Spymaster quest was done in EVERY game (12 Sympathies is
//          easy with a free reveal each round) -> 15. Everything else waits for
//          the playtest (bots play randomly).
// v3.1-dev.23 (2026-11-07): TABLE 3.1 -- every table screen redone from the canvas
//          https://claude.ai/artifact/7fLBrkn6uC5bPzA7ESFgWW (user: "align with the pillars, the
//          design rules we made ... sleek feel with retro charm"; then "worried with how small the
//          text is", "colors should always align with the faction taking the action", "we are
//          playing as characters"). One template for everything the table announces (tableMoment):
//          the kind of news round the rim (28 pt), the word that matters big and straight (36 pt,
//          shrinks to fit), what it means under it (20 pt) -- all twice, for both sides; mood =
//          the ACTING faction's color (Rebels gold, Empire steel, Pykes purple, Dawn crimson),
//          cyan for neutral news, coral for battles, gold for the end. Players are named by
//          CHARACTER (COMMANDER_CALL: KESS, THANE, ...; snapshotCallName), factions only when the
//          galaxy acts ("THE PYKES STRIKE" / big "KESS" / "SHAKEDOWN -5 CR"). snapshotNewsText now
//          gives three lines (rim / big / small). CLOCK: round as big PIXEL numerals (the phones'
//          5x7 set, table_pixfont.h) + "/8", the phase named twice in 20 pt where no seat is, each
//          player's VP as a pixel number by their gate facing them ("?" in the Shadow Age), no
//          phase ring and no standings bars; an ERA turns the whole face its faction's color with
//          "<ERA> - N ROUNDS LEFT" round the rim. BATTLE: only the fight -- other seats and gates
//          hidden; each side faces its own seat: die 56 px at r76 (winner bright, loser dim), ships
//          left as a pixel number at r128, its badge (or the world's name) at r186; "BATTLE FOR
//          KESSEL" / "KESS VS THANE" round the rim; bolts run off the axis, one each way. PHASE
//          CHANGE = HYPERSPACE: streaks race out from the middle, then a white flash (1.1 s, in
//          the renderer: 512 angle lanes, one compare per pixel side). New font
//          sf_font_montserrat_28.c (same trick as the 36). Also: home fleet bays say just
//          "Empty bay" (phone).
// v3.1-dev.24 (2026-11-07): TABLE 3.1 AS THE BOARDS DRAW IT (user: "I feel like you adjusted the
//          current pages to fit the mockup looks, I want the mockups almost exactly as you have them
//          made with bigger text all around"; photo of dev.23's battle: the old inner rings ran
//          through the ship count, bolts crossed the die). Renderer: no core ring, no ring between
//          the gates, ONE gate arc per seat at r 190 (+-14 deg; dashed + dim while open, filling in
//          solid and glowing on lock-in, then the flash), mood-colored like the boards; one solid rim.
//          Clock: symbols r 158, VP pixels r 116 + "VP", phase r 98 in the mood color (white in an
//          Era), round pixels in the middle + "/8". Battle: dashed axis, die r 86, pixel ships r 134 +
//          SHIPS / GARRISON, symbol (or the world's name) r 193, ring round the planet / VS, headline
//          round the rim where no seat is; no bolts. Moments: rim line, symbol in the middle, then
//          (both sides) a lighter line, the big word, a pixel number -- strike = "SHAKEDOWN ON" / KESS /
//          "-5 CR" with three rings; Era = "THE" / "SPICE AGE" / "3 ROUNDS", champion on the rim;
//          news rim = "GALAXY NEWS - ROUND N". Lobby (was untouched in dev.23): the code in the middle,
//          SCAN TO JOIN round the rim twice, faction symbols at their hangars lit once taken (dashed
//          rings while open), the character's name inside each facing that seat, OPEN while free; a
//          small Wi-Fi button under the code. Hyperspace kept (user: "looks perfect star wars").
// v3.1-dev.25 (2026-11-07): A TABLE PREVIEW HARNESS + CLEAN TEXT (user, photos of dev.24:
//          "overlapping numbers with others, janky text ... is there any way to make a harness so you
//          can see what it looks like??" -> "lets do them all!"). (1) Everything the table draws moved
//          from the .ino into table_scenes.cpp (+ ui_prims.cpp: lines, discs, faction symbols and
//          colors, shared with the .ino); tools/table_harness/run.py builds that REAL code + the glow
//          renderer + LVGL for a PC with Emscripten, renders 18 table screens from fake games and
//          writes build/table_harness/*.png + sheet.png. (2) WORDS are drawn upright once by LVGL into
//          a scratch buffer, then placed whole -- straight, turned to face a seat, or bent round the
//          rim -- with smooth sampling (letter-by-letter placement gave "TH ANE" / "SC AN"). (3) Layout
//          fixed against the harness: clock VP numbers alone by each seat (no "VP" label), phase name
//          inside them; an Era's name and rounds left on opposite sides; news lines short enough to
//          clear the rim; Lobby on its own Wi-Fi = "SCAN: TABLE WI-FI" / "THEN 192.168.4.1" round the
//          rim, smaller Wi-Fi button lower down. Also: a planet battle no longer counts the militia
//          twice on the table (it is already in defRemaining).
// v3.1-dev.26 (2026-11-07): the Lobby's FACES are back (user: "I loved seeing the pictures pop up of
//          your character"): a taken seat = the character's face (the phones' art at 2x) in a 64 px
//          circle ringed in the faction's color, at r 150 (moved in from 168 so it clears SCAN TO
//          JOIN); faction picked but no character yet = the faction's symbol in the circle + a dim
//          "CHOOSING..."; names inside at r 102 facing that seat; open seats a dashed ring + OPEN.
// v3.1-dev.27 (2026-11-07): ROOM TO BREATHE (user drew where the Lobby faces should go: "they are so
//          close to the text and squished ... lots of the padding around the text in some of these
//          screens is small"). Lobby: each seat turns 28 deg from its hangar toward the nearest side
//          (the free space beside the code, clear of SCAN TO JOIN), faces at r 140, names OUTSIDE the
//          faces (r 190) facing that seat; own-network rim lines a size down. Rim words everywhere
//          sit ~20 px clear of the glowing rim (28 pt at r 183, 20 pt at r 189; were 190 / 195).
//          Battles: ship count r 132, SHIPS / GARRISON r 163, symbol r 196 (more space between them).
// v3.1-dev.28 (2026-11-07): VARIETY in jobs and rumors (user: "they seem to be about the same two
//          planets"). JOBS were drawn only from the hiring faction's RIVAL's unclaimed worlds -- with
//          the hiring faction nearly always your home faction, that was one or two worlds every time;
//          now any unclaimed world that isn't the hiring faction's own (the rival's counted twice), and
//          the last four job worlds sit out while there is anything else. RUMORS: last round's world
//          sits out while anything else is true (a knocked-down garrison stays down and kept winning), and so does last round's KIND of line ("sitting on N Credits" ran 4 rounds in a row).
// v3.1-dev.29 (2026-11-07): LATE-GAME JOBS ON HELD WORLDS (user: "jobs should go to claimed worlds too
//          as the game develops. maybe not in the first half to push people to find more planets"). From
//          round > roundLimit / 2 a job can name a world ANOTHER PLAYER holds (never a home world, never
//          your own, never the hiring faction's own); the first half keeps unclaimed worlds only. Taking
//          it any way finishes the job (setOwnerLocked -> newsOnGainLocked already covered player-held
//          worlds). Phone: the offer says whose it is ("Take Kessel from Kess by the end of round 6").
// v3.1-dev.30 (2026-11-08, between playtest games): an ERA LASTS 3 ROUNDS (ERA_ROUNDS; was half the
//          game -- user: the Spice Age "seemed to last foreverrrr"). Primer card text updated.
// v3.1-dev.31 (2026-11-08, playtest 2 -- the table kept restarting on the venue's Wi-Fi and "pressing the
//          table wifi button isn't doing anything"): the Lobby's Wi-Fi button is the sketch's own uiPill
//          again (130 x 40 at (168, 350)); serial commands TABLEWIFI / HOMEWIFI switch networks from a PC.
// v3.1-dev.32 (2026-11-08, playtest 2 on a busy Wi-Fi 7 router, a busy router: the table restarted every
//          ~30-60 s -- "FROZE (task watchdog)", task LVGL, inside the glow renderer's frame; RAM fine). The
//          Wi-Fi driver shares the screen task's core and can hold it so long that ONE frame passed the 8 s
//          watchdog. The renderer now feeds the watchdog after every strip (a slow frame is just slow) and
//          leaves >= 15 ms between frames (~15 fps, was ~25 fps with 4 ms gaps) so Wi-Fi has room.
//          NOT done (blocked: it edits the shared esp32 core): -DHTTP_MAX_DATA_WAIT=500 for Android
//          Chrome's idle preconnect sockets (the WebServer waits 5 s on each).
// v3.1-dev.33 (2026-11-08, user: "we can edit the library ... make sure we have our backups"): built with
//          -DHTTP_MAX_DATA_WAIT=500 -- the WebServer drops a connection that hasn't sent its request within
//          0.5 s instead of 5 s (Android Chrome's idle preconnects stalled every phone). Needs the core patch
//          in tools/core_patches (README: backups + roll back). BUILD FLAGS NOW: -DSF_BOARD=143 -DHTTP_MAX_DATA_WAIT=500
// v3.1-dev.34 (2026-11-08): the watchdog waits 30 s (HEALTH_WDT_S, was 8). The restarts on a busy router were
//          "loopTask did not reset the watchdog" -- the web server stuck in one long send to a phone on a weak
//          link (it serves one phone at a time; the page is ~280 KB). A real hang still restarts, after 30 s.
// v3.1-dev.35 (2026-11-08, playtest 2: "the small page is going to be a must"): the phone page (391 KB) goes out
//          GZIPPED (113 KB, sf_page_gz.h GENERATED by tools/page/make_page_gz.py -- re-run after every phone-page
//          edit; a stale one is detected by fingerprint and the plain page is served instead), and "/" + "/art"
//          answer 304 Not Modified (ETag = build) to a phone that already has them -- reloads and "back to the
//          war" stop re-downloading everything.
// v3.1-dev.36 (2026-11-08 playtest fixes): (1) taking a world from another PLAYER costs no favor with its faction
//          (no conquest drop, no betrayal, no rival cheer; big moments still play) -- the last round forced an attack on
//          a player-held Rebel world and wiped all Rebel favor; the FIGHT favor preview says so. (2) The first to commit
//          to a world (talk / pay / fight) has it while that runs: a second player gets "Someone got there first"
//          (two players both "won" the same talk).
// v3.1-dev.37 (2026-11-08 playtest feedback): the Movement phase banner says MOVE YOUR SHIPS ON THE BOARD NOW (a
//          new player "felt like he should move on the phone"); tapping an Inbox item opens its full original popup
//          again (feedReplay -> annOpen).
// v3.1-dev.38 (2026-11-08): the page starts 14 px further below the status bar -- added to the Home Screen as an
//          app, iOS Safari blurs the strip just under the status bar and the top title sat in it ("blurry").
// v3.1-dev.39 (2026-11-10, playtest 3 quick fixes): (1) SERETH KAAN's portrait had its right half missing -- the art
//          exporter (tools/art/portraits_v2.html #export) gave pure-black pixels palette index 0 = TRANSPARENT; now
//          nudged to 0x010101. Re-exported: Vell, Nyra, Korrik, Brakk and Kaan change (their pure-black details show).
//          (2) ANCIENT reroll: the stamp carries both rolls ("rolled again: 2 -> 5" / "no luck this time", once per
//          battle) -- it worked but looked like it didn't. (3) The Defense tile on YOUR world shows GARRISON <n> ships;
//          on others it is BASE DEFENSE (the fixed rating, not what's there now). (4) "stall" -> "shop" in everything
//          players read (phone text + the table's MARKET DAY / EMBARGO lines).
// v3.1-dev.40 (2026-11-10): paying off a PYKE grudge costs 8 CR (~1.5x the 5 CR shakedown; was 12 like everyone) -- grudgePayoffCr().
// v3.1-dev.41 (2026-11-11): THE NEW BOARD on the phone (the b17 charm pass): the Galaxy map's Mid Rim worlds sit at 45 deg steps
//          (Naboo, Endor, Nal Hutta, Canto Bight moved), SPACE STATIONS east + west, the faction hangars + emergency lanes are
//          gone. Where fleets launch (words only -- the game never tracked board positions): your home world; if it fell, any
//          world you hold; hold nothing -> a space station. Primer, battle report, home-world page and the fleet buttons
//          (REBUILD FLEET / FOUND FLEET) say so. The Prepare screen keeps its name YOUR HANGAR (the repair bay, not a board spot).
// v3.1-dev.42 (2026-11-11): MAKE AMENDS (user: "two ways to calm them"; "feel good and feel star wars"). When a grudge is coming
//          for you the sheet offers ONE of the faction's three DEEDS (user: "offered one at a time"; each faction asks for them in
//          turn, skipping what you can't do -- Player.grudgeDeed / deedNext) -- the same three kinds for everyone, a SHIP / a ROUND / a WORLD:
//          Rebels: Join the cell (a ship away 2 rounds) / Stand down / Shelter refugees (-1 garrison on a world).
//          Empire: Lend troops (2 garrison from a world, keeps 1) / Escort duty / Lockdown (no adding garrison there, 2 rounds).
//          Pykes:  Hand over a hauler (a ship, for good) / Haul their cargo / Put up collateral (its income to them, next 2 rounds).
//          Dawn:   Lend a blade (a ship rides on their next strike: 1-2 lost, the VICTIM learns whose) / Lie low / Safehouse talk
//                  (the whole table learns that world's garrison). A ROUND deed = no fleet attacks this round (next, if in Resolution).
//          A deed calls the strike off + favor a FULL STEP (to the bottom of tier 2); PAY THEM OFF now also lifts favor, but only
//          half a step (+10, never out of tier 1). Only doable deeds are live (snapshotDeedRefusal); /state grudge.dk / grudge.dw.
//          New Player fields (noAtk / lock / collat / loan) -> old saves are ignored. Phone harness: grudgesheet, grudgeemp, amends.
// v3.1-dev.43 (2026-11-12): THE FINAL HOUR (user 2026-11-11/12; the design notes "THE FINAL HOUR"), built for a playtest -- numbers to tune.
//          Second-to-last round: THE GAMBITS ARE DEALT (table + phones) -- each player draws from their class's four (VP leader 1,
//          last place 3, the rest 2; keep one) and a private BLACK MARKET offers two more from other classes (15 CR, hold up to 3).
//          The last round: Prepare keeps TALK + PAY (fights are refused all round) + CALLING IN DEBTS (champion 3 ships, Family 2,
//          Friend 1 free; Neutral 6 CR a ship up to 2; Wary 10 CR for 1; Marked never) + a LAST CHANCE line. Movement = LAST ORDERS
//          (each fleet strikes one world anywhere or stays home to guard one of yours -- its own column, outside the garrison cap;
//          a JUMP PASS (3 CR) for a world not held / one lane or ring step from one you hold; no worlds = reach from a space
//          station; allied ships ride with a strike or guard your best world; WITH-YOUR-ORDERS gambits pinned to strikes).
//          Resolution = THE REVEAL (every strike + size; Ghost Strikes hidden; Intercepted Orders sees pins) with the
//          AT-THE-REVEAL gambits chosen in secret, then THE FINALE resolves at once (fhFinaleLocked) world by world, Outer Rim to
//          the Core: strikes fight the garrison SIDE BY SIDE (it rolls once, each strike against it; beat = a garrison ship
//          falls, lose / tie = you lose one; #9 invisible help vs unclaimed garrisons, both ways), and when the garrison falls the
//          strikes still standing TURN ON EACH OTHER (all roll, the lowest unique roll loses a ship, ties roll again). The table
//          plays each world as a moment; phones get the log (/state fh.log) and play it back (#fhPlay); the game ends ~20 s
//          later once the table's moments are done (fhTickLocked). Choices made while building (tell the user / retune): Intercepted
//          Orders = automatic at the reveal (see every pin + ghost); Smuggler's Run = an extra strike of 3; Turncoat = up to 4 of an
//          unclaimed garrison switch; Old Friends = that faction's ships again to your biggest strike; Long Con = 3 ships from the
//          VP leader's biggest strike to yours; High Roller = 1 ship per 2 CR left (max 6); unplayed gambits are wasted; a war can't
//          be declared into the last round. Phone-only (AMOLED_TODO). Bots (tools/shakedown/bot.py) play it. GameState grew -> old saves ignored.
// v3.1-dev.44 (2026-11-12): PLAYTEST 4 FIXES. (1) The phones kept the dev.42 page through the dev.43 flash, so the Final Hour never
//          showed (the table dealt the gambits in round 7; the last round had no screens) -> the page carries PAGE_FW (written by
//          tools/page/make_page_gz.py from this file) and reloads once when /state reports another firmware. (2) The home-screen
//          app's top strip still blurred under the iOS status bar -> +40 px there. (3) A PvP report names the fleet(s): "THANE'S
//          FLEET 1 DESTROYED" / "YOUR FLEET 2 WAS DESTROYED". (4) RAM LOW WATCH (the table hit 0 KB
//          free in the playtest, cause unknown): every new low of free internal RAM is logged "[RAM] new low N KB at T s: <uri> rR pP"
//          and listed on /health (sf_health healthLowDropped / healthLowNote).
// v3.1-dev.45 (2026-11-12): THE DECIDED-BUT-UNBUILT THREE. (1) #9 INVISIBLE HELP (user 2026-11-11): a player vs an UNCLAIMED
//          world -- the bigger side quietly rolls best of 2 at FH_HELP2 (3) more ships, best of 3 at FH_HELP3 (7) more,
//          recounted each exchange (escort, hired ships and a Warlord's extra count for the player; militia + a Consul's extra
//          for the garrison); only the counting die shows; never PvP, player-held worlds, the War Room or a parting shot; never
//          mentioned. (2) #10 LAST SHIP KEEPS A FLEET: every loss (both sides, parting shots too) comes off the fleet with the
//          most ships left (lossIndex). (3) THE JOB OFFER AS A HOLOGRAM: your contact in that faction makes the offer in its own
//          voice (JOB_LINES), then the terms + TAKE THE JOB / PASS. The battle forecast's odds don't include the help (on purpose).
// v3.1-dev.46 (2026-11-12): THE CLARITY PASS (playtest 4: a playtester "doesn't understand a lot of the news", transmissions
//          "mean nothing", nobody knows why escorts show up; mockup tools/ui31_mockups/clarity_droid.html). Every transmission,
//          news card, grudge, job offer and shut shop = the in-character message + a terminal READOUT (RE: what you did + round;
//          STANDING = a 5-segment bar + the NEW tier word, red with a down arrow / green with an up arrow; EFFECT rows: shop
//          closed / open again, escorts gained / lost, talks refused + militia, Family, the rival pleased) + RV-3 "RIVET", an
//          original droid who explains it in plain words on tap (corner icon, gold dot until read; rivetBtn / rivetSay /
//          readoutHtml / standingEffects / txReadout / newsReadout) with one action (SHOW ME ON THE MAP...). /state txPlanet.
//          A shop closed to you is shuttered ("CLOSED TO YOU"). The battle forecast: the old dice spots are gone (user), each
//          helper ship says whose (faction: tier word), an IF YOU WIN line (whose standing moves) + Rivet; an unknown world's
//          militia stays hidden as before. GARRISON -> DEFENDERS everywhere players read it (phone + table + rumors + battle
//          text; in-character lines keep "garrison"; the market good is now DEFENSE TROOPS).
//          Also: the RAM low watch keeps its list in RTC memory, so /health shows the run before's lows after a restart.
// v3.1-dev.47 (2026-11-13): THE WARLORD REWORK (playtest 4: "makes everyone mad, can't buy anything, can't progress in favor, it's
//          not fun that way"; user picks): the extra -10 per conquest ("Feared") is GONE; THE MUSCLE = a world a Warlord takes
//          by force pleases its faction's rival by CLASS_WARLORD_RIVAL (6) instead of REP_RIVAL_GAIN (4), stamped (STAMP_FEARED
//          now reads THE MUSCLE); the new weakness BLUNT = talks ask a Warlord one more question (like the Scoundrel; Core
//          talks are already at the 5-question cap). Class card text updated.
// v3.1-dev.48 (2026-11-13): THE SCOUNDREL REWORK (playtest 3: "barely noticeable", broke after one lost fight; user 2026-11-13:
//          the Han Solo freighter scene -- "you've abused the system so much you get in trouble", not a die roll). SELL IT TWICE:
//          when a Scoundrel wins a world by talks / buys one / takes one by force / finishes a job / makes amends, the RIVAL of
//          the faction it pleased (a fight: the VICTIM, who pays you to "find out who did it") offers to pay for the same work
//          -- a hologram with its own story (a back door, landing codes, a slow job, what they asked of you, find who hit it),
//          SELL IT TWICE or KEEP IT CLEAN: Credits by size (Outer 5 / Mid 6 / Core 8, job 5, deed 4) + 3 favor with the buyer +
//          1 HEAT. Heat cools only in a round without a sale; shown as a mood on Home ("someone's asking questions" -> "two
//          old enemies are comparing notes"). At 3: CAUGHT -- both factions of that pair -20 favor at once, Galaxy News + the
//          table, both strike at the start of next round (grudgeStrikeLocked), PAY ONE BACK (what they paid you + 3) to call
//          off its strike. THE STASH: income never below 6. MARK-UP: Pay costs a Scoundrel 2 more (stamped; replaces the
//          1 CR market discount). The extra talk question is the Warlord's alone now. Bots sell 70% of the time.
// v3.1-dev.49 (2026-11-13): THE LIST #12-18 (user picks). #12 RACE TIES: everyone who finishes a race in the same round wins it
//          (raceDoneRound / raceWinners; the race closes at the next round start). #13 ONE TITLE (user 2026-11-08: a defector
//          champion of two factions "felt really wrong"): someone who holds a title is OFFERED a second one instead
//          (Player.champOffer, Home row A SECOND TITLE -> a sheet: swap or keep; swapping gives up the old title + its decree and
//          blocks retaking it -- titleBlock). #14 DEFECTOR START: the faction a defector LEFT starts at tier 2 like any rival
//          (was top of tier 3), plus +5 with a related third (left the Rebels -> the Dawn, the Empire -> the Pykes, the Pykes ->
//          the Rebels, the Dawn -> the Empire). #16 (user: "make battling more appealing"): winning a world by TALK or PAY
//          costs a little favor with its faction's RIVAL (REP_COURTED -3) with a new transmission cause COURTED (4 lines per
//          faction, tools/transmissions/*.txt); the favor preview shows it. #17 THE PEOPLE RESIST: the Rebellion no longer shuts
//          Pay -- buying a world in it costs ERA_RESIST_FAVOR (-10) Rebel favor (Family + the champion spared) and the table
//          hears it (NEWS_RESIST). #18 A world held WORLD_TURN_ROUNDS (3) rounds takes its holder's faction as its Sympathy
//          (not home worlds; planetHeldSince; PN_TURNED). The favor preview also follows the Warlord / Scoundrel reworks.
//          #15 is dev.50.
// v3.1-dev.50 (2026-11-13): #15 CHAMPIONS TOO LATE -> TIER 6 (user: "maybe we just make a tier 6?"; "round 5-6 is perfect"): the
//          bands narrow -- Friend +10..+16, Family +17.. (were +10..+29 / +30..) -- and CHAMPION is the sixth rung: +23 or more
//          (REP_CHAMPION_MIN) AND the highest at the table. From the +9 start at ~3 favor a round (playtests): Friend after the
//          first good deal, Family ~round 3-4, Champion ~round 5-6. The phone's favor bar is redrawn to the new bands with a
//          gold tick at the champion rung. Family perks (talk help, spared from the Era sting, ongoing gifts) arrive sooner too.
// v3.1-dev.51 (2026-11-13): #19 ONLY DOABLE FAVORS (favorDoableLocked: a favor is offered only if it can be done on paper -- a
//          world of that faction / a Core world that isn't yours, another player with ships, your own fleet or 2 CR, the
//          tribute's Credits; else a Credits favor fills in). SPREAD THE CHANCES (user: they "group in one place most games"):
//          races, commotions, offensives and jobs pick a world far from the last four the galaxy named (spreadPickLocked,
//          distance + jitter). #21 (user: fine as is, "a couple hooded figures here and there fits the star wars vibe"): 2 in 5
//          unknown ambassadors are a hooded figure in the same torn grey (hoodedURL).
// v3.1-dev.52 (2026-11-14): THE GALLERY (#20; user: "being able to see is fun", whisper (a), named, stakes, WATCH) + PLAYER TAGS.
//          GALLERY: when a player starts a talk the table shows NEGOTIATIONS on the world and every other phone gets a
//          notification -- tap it to WATCH the talk live (/state "talks": hologram (the faction only if YOU know the world),
//          pips, the question + replies). Once per talk, before its last question: WHISPER AGAINST (2 CR, one per talk: the
//          next question moves up a tier -- a hard one loses its contact help) or VOUCH (free, Friend+ with that faction: a
//          wrong reply is gone from the next question). Named, shown to the talker + the table. STAKES at the end (won /
//          lost / walked out): vouchers +2 / -2, the whisperer -2 / +2 with that faction (PN_GALLERY notice).
//          PLAYER TAGS (user: a playtester "doesn't realize when a player is being mentioned"): every mention of a player on the
//          phone is a tag -- their face in a ring of their colour + the name (YOU for you); factions stay plain words
//          (pTag; feedWho returns it; whoName = the plain name; big moments in the Feed tag the players too).
// v3.1-dev.53 (2026-11-14): #23 LATE-GAME BIG BUYS (user: "the final hour might be part of the answer"; "offer 2 but you can only
//          buy one"): the black market (second-to-last round + the last Prepare) also offers each player 2 random BIG-TICKET
//          items of 5 -- CAPITAL SHIP 30 (+4 ships to your biggest strike, past a fleet's size), PLANETARY SHIELD 25 (a world
//          you hold: its first 3 hits in the finale don't count), MERCENARY ARMADA 20 (an extra 4-ship strike: strike slot 4,
//          FH_STRIKES 4 -> 5), HYPERSPACE CHARTS 15 (every world in reach, no jump passes), SPY NETWORK 45 (sees pins + ghosts
//          at the reveal, like Intercepted Orders). ONE a game; every purchase is announced (Galaxy News + the table,
//          NEWS_BIGBUY). The finale log carries a shield byte after the garrison (the phone's fhParse reads it).
// v3.1-dev.54 (2026-11-14): ATTACK ALERT + SPECTATOR MODE (P3; user 2026-11-12: "an alert/spectate button for when your planet is
//          getting attacked"; spectator mode for late joiners). Any battle you're not in pops a notification -- "[THANE]
//          ATTACKS NABOO · Tap to watch" (red + a buzz when it's YOUR world) -- and tapping it opens the same battle screen
//          watch-only (battleCard, shared with the fighters; no actions, STOP WATCHING / CLOSE, no battle report after).
//          SPECTATOR MODE: the mid-game seat picker offers WATCH THE GAME -- standings, HAPPENING NOW (the battle + any talks,
//          each with WATCH), the latest public history; spectators get the gallery's talks too (no actions; /state talks
//          now sent to unseated ids) but not the full-screen Galaxy News cards.
// v3.1-dev.55 (2026-11-14): the map's SIDE filter (playtest 3 'a faction filter'): a fifth chip SIDE opens a sheet -- REBELS / EMPIRE /
//          PYKES / CRIMSON DAWN / UNKNOWN with counts -- and lights the worlds whose Sympathy you know is theirs (galaxyMarks[4],
//          gFaction); the filter row is compact so five fit.
// v3.1-dev.56 (2026-11-14): THE PHASE TRACK (playtest: players unsure what to do when; user: "it needs to be on the phone"):
//          PREPARE > MOVEMENT > RESOLUTION across the top of the home screen with the current step lit, and a NOW line
//          saying what to do (Movement: move on the REAL BOARD, nothing to tap). Final round reads PREPARE > LAST ORDERS >
//          THE FINAL HOUR. The move card is now 'ON THE BOARD' and hides in the final round.
// v3.1-dev.57 (2026-11-14): THE NEBULAE ARE STOPS (user): The Maw and the Unknown Regions sit on the Outer Rim as stops, so the
//          worlds either side (e.g. Mustafar / Yavin 4) are two steps apart -- Final Hour reach no longer counts them as
//          neighbours (fhArcHitsNebula); the phone map draws each as a dashed stop with its name.
//          DECLARE WAR CLARITY (playtest 4): a three-step timeline (DECLARE / GET READY / THE WAR ROOM) with real round numbers
//          and the current step lit, on the move sheet, the confirm and a new war sheet for the Warlord and the world's holder
//          (holder: 'they can bring up to 14 ships, fortify it' + OPEN); the War Room shows WORLD / KEEPS / SENDS and the sum
//          vs the defenders; the move sheet says when it's too late (a war never runs into the Final Hour).
// v3.1-dev.58 (2026-11-14): THE NEW HOW TO PLAY (user: "a new how to play deck - from scratch"; "super awesome"). THE BASICS =
//          one 8-card deck (goal, your side, the board, three steps, Prepare, Movement, Resolution, the Final Hour). RIVET'S
//          GUIDES = 8 short topic decks Rivet narrates (favor, news, battles, once a game, sell it twice, the gallery, the Final
//          Hour, the galaxy moves), from the deck's last card and a GUIDES button on Standings (primerGuide / GUIDES).
// v3.1-dev.59 (2026-11-14): the NOW line can go away (user: "good for others but I don't want to see it the whole game"): an x on
//          the line hides it on that phone (localStorage sfNowOff); a NOW chip on the step bar brings it back. The step bar stays.
// v3.1-dev.60 (2026-11-14): Rivet stops covering text (user: "overlaps text in most places"): in a readout he sits inside the strip on
//          its right edge with room kept for him; news lines keep him inside their card; IF YOU WIN has him beside the line; a shut
//          shop's bubble opens BENEATH the card, full width (user: "should pop up beneath it"), the grid fills the gap beside it.
// v3.1-dev.61 (2026-11-14): SETTINGS (user: "rivet should be a toggle (experienced players can turn him off)"): a gear on Home's
//          top bar + a SETTINGS button in the Lobby open a sheet. THIS PHONE (localStorage): Rivet, the NOW line, buzz, motion.
//          THE TABLE: which Wi-Fi it is on, up to FOUR saved networks (newest first; at boot it scans and joins the first one in
//          range; slot 0 keeps the old keys so a saved network survives), FORGET, ADD A NETWORK (/setup, now with a show-password
//          box), switch home / own Wi-Fi (two taps). Endpoints /netinfo, /net_forget, /net_mode; passwords never leave the table.
// v3.1-dev.62 (2026-11-14): WI-FI FIXES for a weekend at a Synology Wi-Fi 7 router (user: "lets go for the wifi fixes"):
//          (1) sendBounded: the page, font, icon and /state go out through a non-blocking loop that cuts off a phone taking
//          nothing for 2.5 s or not done after 12 s (playtest 2's restarts = loopTask stuck >8 s in one send; the core waits
//          up to 10 s per stalled chunk) -- counted in /health; (2) the join scans first, tries each saved network in range
//          (max 2), 15 s each, and keeps WHY it failed in plain words (wrong password / not found or 2.4 GHz off / security
//          mode / router refused / no address) -> the table's Lobby under "Couldn't reach" + Settings; (3) the rejoin after a
//          drop uses the network actually joined; (4) own network: the quietest of channels 1/6/11 (was always 1) and power
//          save OFF (the station half napped); (5) /health ends with a Wi-Fi line (mode, drops + last reason, slow phones cut off).
// v3.1-dev.63 (2026-11-14): SPEED (user: "it felt so sluggish"). Measured on the table (dev.59, a 4-bot game + a probe
//          polling /state every 300 ms): 319 ms median, 478 ms p90, 1.3 s max -- vs 60-80 ms in the Lobby. Cause: the LVGL
//          task (priority 2, ~77% busy drawing the glow) floated onto core 1 and starved loopTask (priority 1 = the web
//          server). The table pins LVGL to core 0. Saves write 1 KB at a time with 12 ms pauses (a flash write pauses the
//          chip; one ~400 ms burst held every poll). TEXT SWEEP (user: typos + old text): "stalls" -> shops, "decisively"
//          -> "destroy a player's whole fleet", the halfway event "if switched on in the Lobby", "civilized", "Affiliation"
//          -> Sympathy in talks, the favor tag's stale "ITS VP COMES WHEN YOU DO IT", the decree + Era news said half the
//          game (an Era is 3 rounds since dev.30), the BETRAYAL warning used the old FAVORABLE / ALLIED words.
// v3.1-dev.64 (2026-11-14): THE GALAXY AND YOU, CLEANED UP (user: "really crowded by the end of the game"; mockup
//          https://claude.ai/artifact/UPgQvfbQ4VpshWeho6NUnH, "yes I agree"): look-alike rows merge (gambits -> one row and a
//          sheet, YOU OWE, OUT FOR YOU), rows sort NOW > YOUR MOVES > GOOD TO KNOW, the first 4 show + N MORE (stays open on the
//          phone until SHOW LESS), the header counts what needs you now, the action word on the right (CALM / CALL / DECIDE /
//          DUE / WRITE). A new Rivet guide THE GALAXY AND YOU (2 cards), opened from Rivet on the list's header. How to Play:
//          the gambits are dealt the round BEFORE the last (the cards said the last Prepare).
// v3.1-dev.65 (2026-11-14): RIVET'S GUIDES by plain questions (user: "it says 'your favor' when someone looking at it won't
//          know what favor is"): each guide's index row leads with what a new player would look for (WHAT NEEDS ME? / HOW THE
//          GALAXY REACTS TO YOU / WHAT'S GOING ON? / FIGHTING / YOUR BIG MOVE / PLAYING BOTH SIDES / WHEN SOMEONE ELSE
//          NEGOTIATES / HOW THE GAME ENDS / WHEN THE GALAXY CHANGES), then the game's own term ("Favor: ..."). Spell check of
//          every player-facing string (page, rules, transmissions, questions; pyspellchecker): no misspellings.
// v3.1-dev.66 (2026-11-14): the top bar no longer wraps -- since dev.61's Settings gear, "ROUND 3 / 8" and "9 CR" broke onto two
//          lines in Resolution (found in the keyword mockups, https://claude.ai/artifact/4xywKvzesxnSWRXKHeP5vv): no-wrap, tighter
//          gaps, 32 px gear + bell. Harness: tools/phone_harness/kw_proto.js (modes kw*) = the live-button / keyword / Rivet
//          first-time prototype, NOT in the firmware.
// v3.1-dev.67 (2026-11-14): TEACHING THE WORDS (mockup https://claude.ai/artifact/4xywKvzesxnSWRXKHeP5vv; user: captions "look very
//          AI", "I like idea 1 and 2"). (1) LIVE BUTTONS: FAVOR shows the four faction symbols in your mood color with each (red
//          + pulsing = out for you); STANDINGS shows a podium (one bar per player, yours outlined; Shadow Age = others flat).
//          (2) KEYWORDS: game terms get a faint gold dotted underline; a tap opens Rivet's one line in a pop-up under the word
//          (#kwPop, its own layer; closes on any tap or scroll). KW in the page: Escort, Militia, Hired, Defenders, Base defense,
//          Sympathetic to, Resources, Trait, Out for you, You owe, Gambit, Black market, Debts, Decree, Your spy, Heat -- on the
//          battle forecast, the planet tiles and the Galaxy and You headings. Rivet off in Settings = plain words.
// v3.1-dev.68 (2026-11-14): FREEZE FIX. dev.63's speed fix pinned the table's LVGL task to core 0; flashed with dev.67, a 4-bot
//          game froze it twice (task watchdog, IDLE0 starved). The LVGL task now runs on core 1 at priority 1 (= loopTask):
//          the screen and the web server share the core round-robin instead of the screen always winning.
//          MEASURED (4-bot game + a probe every 300 ms): /state median 319 -> 105 ms, p90 478 -> 177, p99 705 -> 277, no reboots.
// v3.1-dev.69 (2026-11-14): HOME WI-FI RETRY. Right after the dev.68 flash the table missed the home Wi-Fi once and sat on its own
//          network (nobody could reach it). Now, on its own network in an EMPTY Lobby after a failed join, it retries the home
//          network once a minute (10 tries; never after a wrong password / a security-mode refusal) and restarts onto it the
//          moment it gets in. A seated player or a running game stops the retries (a try moves the radio's channel).
// v3.1-dev.70 (2026-11-14): RIVET + KEYWORDS EVERYWHERE ELSE (P3; user: "adding it in more places is better" since he can be
//          switched off). rivetAsk() = a small Rivet button on the right, his line right under it. On: every negotiation
//          question (the rules + who you're talking to), the favor choice (a debt), PAY (why this price, personalized: ring,
//          escalation, standing, the Scoundrel's mark-up), the Era sheet, the four once-a-game moves (a tip each), the War Room,
//          the Final Hour's draw / black market / debts, each Favor ladder. New keywords: IF YOU FIGHT IT OUT (how a battle
//          works, on the odds card), HEAT, LAST ORDERS, JUMP PASS. The shut shop's Rivet uses the same helper.
// v3.1-dev.71 (2026-11-14, user's notes from dev.70 on a phone): the shut shop says "You're in bad standing with them" (not the
//          tier word); PAY's Rivet trimmed to what matters (Core dearer, liked = cheaper, Scoundrel extra); keywords INSIDE a
//          button no longer steal the press -- the word presses the button, only a 12 px strip under it asks Rivet; CALM THEM
//          in Galaxy News closes the news before the grudge sheet opens (it opened behind it); FAVOR BARS = five equal zones
//          with the marker placed inside its own zone (they were sized by score range and the marker ignored the gaps: Friend a
//          sliver, markers off at the edges), the champion mark a small notch above; faction rows use the short name (EMPIRE
//          wrapped onto three lines); the top bar tighter (13 px, 1 px spacing, wider gaps) -- on a phone ROUND 4 / 8 touched
//          RESOLUTION and the bell's count sat on the Credits.
// v3.1-dev.72 (2026-11-14): ONE SET OF TIER WORDS (user: "unify those so you don't have to know 16 of them"): Enemy / Distrusted /
//          Neutral / Friend / Family (+ Champion) for every faction; the flavored names stay only where a character speaks.
//          DEEDS COST MORE (user: "Join the cell costs 2 cr. easily the best move"; the 12 CR payoff "makes sense, but the other
//          things don't hit as hard"; round deeds stay 1 round -- 2 "seems harsh"): Join the cell 3 ships for 2 rounds (was 1),
//          Hand over two haulers (was 1), Lend two blades (each rolls its own 1-2), Lend troops 4 defenders (was 2), Shelter
//          refugees 3 defenders (was 1), Lockdown / Collateral 3 rounds (was 2). Player.loanN (GameState size changed: old saves
//          are dropped).
// v3.1-dev.73 (2026-11-14): SCREENOFF / SCREENON serial commands (saved per board) for the cracked table; table_check.py.
// v3.1-dev.74 (2026-11-16, playtest 5 at another house): THE CRASH: two restarts there, both "CRASH (panic) task tiT" = newlib
//          abort in lock_init_generic from uart_write -- the console's write lock is made lazily by the first print, and the
//          first print came from the Wi-Fi stack while internal RAM was at 0 KB (every phone reloading /state at once). The
//          .ino now prints once at boot so the lock exists; /state answers {"busy":1} (503) to a phone that already has the game
//          when free internal RAM < STATE_RAM_FLOOR (10 KB); phones back off 0.4-5 s + up to 1.5 s random after a failed or busy
//          poll, so a restarted table isn't hit by every phone at once. SETUP PAGE: the typed network name counts only when
//          ANOTHER NETWORK is picked (an iPhone's autofill put a username there and it won over the network tapped). DEBTS
//          ARE FREE (user: "it's debts! it should only give"): Friend 1 / Family 2 / Champion 3 ships, Neutral and Distrusted
//          owe you nothing (were 6 / 10 CR a ship). PYKE FUEL is announced on the table + in Galaxy News (NEWS_FUEL). WORLDS
//          NO LONGER SWITCH SIDES after 3 rounds (WORLD_TURN_ON 0; it broke a kept promise and "sympathy doesn't matter once
//          it's held"). BATTLES: other news waits while a battle is on screen (a badge counts it), stamps wait until the dice
//          land; a live battle sits at the top of the Inbox (tap = watch). The HEIST's public line no longer spoils your own
//          die. Era texts match the code (Spice Age: two goods a shop, not three). The HEAT row no longer runs off the edge;
//          the favor bar's "it moved" streak is a thin line, not a fill across the zone gaps.
// v3.1-dev.75 (2026-11-16): TRANSMISSIONS REWORKED (canvas board 5 + 6, user: "big, not too skippable, not too much to read unless you
//          want to dig into it"; "keep the feel of a transmission coming in"): TRANSMISSION INCOMING (rings pulsing out of the caller's
//          symbol, ~1.1 s, a tap answers) -> the hologram in a full-width frame pulsing in the caller's colour (relay label, LIVE, a
//          scanline), one typed line, then each change pops in as a chip (standing up green / down red with the new tier word, a NEW
//          GIFT gold with a shine, a lost gift red); WHY? opens the old readout + Rivet; CONTINUE fills over 1.6 s before it works.
//          ONE SCREEN FOR TWO FACTIONS (user: "I often raise with one faction and drop with another in the same action, and it gives
//          me two transmission screens"): /state sends txAlso (the next queued reaction from another faction to the same world), the
//          phone shows it as a chip and acks both (/transmission_ack?n=2). Gifts no longer pop up as a separate notification. THE
//          KIT (boards 7 + 8): YOUR KIT button on your card (the class rules moved off the busy card) and any face in Standings opens
//          that player's kit -- class strengths / weakness / once-a-game move + the faction's ship quirk, each with a small source
//          icon; others' gifts and favor stay secret. STAMPS show their source as a small icon (a star = your class, a faction
//          symbol = its ships or gift, a planet = a world's trait).
// v3.1-dev.76 (2026-11-16): DEFENSE (user: "defending your planet against 2 ships is impossible"; "investing in defense is maybe the most
//          tedious part of the game"): the Home PLANETS box is the door (shield + DEFEND, an orange dot when a world of yours has 1
//          defender) to ONE sheet -- every world you hold, its defenders as blocks up to its limit, - / + (2 CR), a PATROL chip, one
//          total, one CONFIRM (canvas boards 1 + 2). LIMITS once held: Outer 6 / Mid 8 / Core 10 (never below its start; home guards
//          6), +2 fortified (fortMask, filled by the black market in dev.77); starting garrisons unchanged; MAX_PLANET_DEFENSE 12 is
//          only the absolute cap. PATROL: 6 CR, 2 hired ships guard a world this round and next, fight after a militia and before the
//          garrison, survivors stay on; public ("PATROLLED") on the world. HELD WORLDS REBUILD +1 every 2 rounds back up to their start,
//          like unclaimed ones. DUST SETTLING: a world that changed hands in this Resolution can't be attacked again that round
//          (planetTakenRound). SHAKEN: a fleet that withdraws from a player battle can't attack next round, and the stamp says to move
//          it back to your nearest world or a space station. THE SQUEEZE: in a 3-player game the seat opposite the empty home (between
//          the other two) starts with Fleets 2 and 3 at 2 ships each. GameState + Player grew (old saves are dropped).
// v3.1-dev.77 (2026-11-16): THE BLACK MARKET (canvas boards 3 + 4, user: "looks amazing ... I love that it moves"; "it shouldn't feel
//          affiliated with crimson dawn"; "the black market should no longer be mentioned in the galaxy and you"): when it opens (the
//          round before last) the Home MARKETS strip turns into its door -- hazard stripes, a flickering broker hologram, a sweeping
//          scanline, a blinking amber light, its lots as tiles -- and tapping it opens the page: a smuggler's den (hologram blue + amber,
//          no faction colour), the lots as crates, the normal shops underneath. NEW LATE-GAME LOTS (user: "things useful in the late
//          game ... cool, but not OP"; Salvage crew rejected): two a player, each bought once -- SMUGGLED TROOPS 9 (+3 defenders on a
//          world, even past its limit), FORTIFICATIONS 10 (+2 limit on a world, for good), SABOTEUR 10 (the next world you attack
//          loses 2 defenders first), TURNCOAT 12 (one of its defenders fights for you), SECOND CHANCE 9 (a fleet down to 2 ships gets
//          2 more, once); each fires with a stamp. Player.lotOff / lotsGot / lotReady (old saves are dropped).
// v3.1-dev.78 (2026-11-16): SCOUNDREL = PRICE ON YOUR HEAD (user: "the bounty makes a lot of sense"; MARK-UP gone): whoever takes a
//          Scoundrel's world (any way) gets +3 CR, stamped BOUNTY; telegraphed on the world page, as a BOUNTY +3 chip in Standings and in
//          the map's CHANCES filter (not on the table -- user: it doesn't fit the table flow). INFILTRATOR = THEIR FILE (user: "the
//          infiltrator feels like a rip off consul"): once a round, open one rival's file from their face in Standings -- Credits,
//          debts (faction / world / due), a grudge coming, their job, patrols. SLICER (a 6th black-market lot, 8 CR): one rival's file
//          this round, plus every defender they hold. CRIMSON DAWN FAMILY GIFT = BLACKMAIL, once (was a spy at court; user: "blackmail is
//          too powerful to just get at friend"): pick a rival -- they pay you 8 CR, or the whole table hears their debts (NEWS_EXPOSED:
//          the worlds they owe on); unanswered by the next round = refused. Fixed a dev.77 slip: a dangling if hid the BIG TICKET row
//          once the black market closed. Player grew (old saves are dropped).
// v3.1-dev.79 (2026-11-16): TAKING A COMMANDER'S WORLD WITHOUT A FIGHT (user: "we got to round 5, and all planets are taken, so talking
//          and paying aren't effective"; "it needs to be steep ... it should steer you toward fighting"): a held (non-home) world shows
//          FIGHT / UPRISING / BUYOUT. BUYOUT = Pay at twice the price, never under 20 CR (snapshotPayPrice); the Credits are held, the
//          holder gets a BUYOUT OFFER on Home and in the Inbox: SELL (keeps half, the world and its VP go) or MATCH (pays the same, keeps
//          it; the buyer is refunded); no answer by the next round = sold; a world that changes hands first refunds the buyer; the table
//          and Galaxy News announce offer / sale / match. UPRISING = Talk there (user: "a revolt ... I am a fan of that kind of chaos";
//          "uprising ... one word buttons, it should be a keyword"): Friend+ with the world's faction, one try per world a game, every
//          question HARD and one wrong word ends it; win and the world is yours AT ONCE with 2 defenders, and its old holder may strike
//          back that same round (the dust-settling rule lets them). Both are keywords Rivet explains. GameState + Player grew.
// v3.1-dev.80 (2026-11-16): BATTLE REPLAY (user: "I would like to be able to tap into battles in the history and see the battle's course";
//          "I don't want to use up all the memory, so good call"): the table keeps the last 8 battles (BattleRec in GameState, ~34 bytes
//          each: round, who, the world, the counts at the first roll, each exchange as one byte, how it ended, who withdrew), recorded in
//          gameStateBattleAction; GET /battles sends them only when a phone opens History. History lists them (BATTLES · TAP TO REPLAY);
//          a tap plays the fight roll by roll -- both dice, both counts dropping, a line per roll, a strip of who won each roll.
// v3.1-dev.81 (2026-11-16): FIRST GAME (user: "the learning curve is intense ... a new player ... has a hard time figuring out where to click";
//          "a toggle for first game ... when you are choosing your character or seat"; "rivet should use proper nouns and not very many
//          pronouns"): a FIRST GAME? toggle under the seat picker (and in Settings), per phone. When it's on, Rivet introduces ONE thing
//          per phase, in order, the first time it matters: Home + the hangar, Movement on the board, Resolution (GALAXY -> FIGHT / TALK /
//          PAY), the PLANETS box, FAVOR, THE GALAXY AND YOU, your once-a-game move, the first Era, the black market -- each a small
//          sheet with a button that opens the thing. Lines name things instead of saying "it" / "they". Phone only.
// v3.1-dev.82 (2026-11-16): TALKS -- TRAP QUESTIONS (user: talks are "a bit too easy" because ORDER = Empire, MONEY = Pykes, CUTTHROAT =
//          Dawn, NICE = Rebels; more questions with the same keys "isn't the answer", and making the two right-sounding replies
//          impossible to tell apart "might not be either"; "that sounds okay to me"): 24 new HARD questions (tools/negotiation/
//          questions.txt; the hard tier is 66 now, 120 shared in all). In each, two replies speak in one faction's voice and one of them
//          GOES TOO FAR -- order vs cruelty (Empire), care vs naivety (Rebels), good business vs cheating your partners (Pykes), quiet vs
//          loud (Crimson Dawn) -- and the third speaks for another faction. Easy and medium questions unchanged; an UPRISING (all hard)
//          gets the traps too. negDeckCard shuffles up to 128 cards (was 64 -- the hard deck would have lost two).
// v3.1-dev.83 (2026-11-16): CRASH FIX found on the first bot game of dev.82: NEW GAME / END GAME did state = GameState(), which builds a
//          whole GameState temporary on the 16 KB loopTask stack; dev.76-80 grew GameState (patrols, buyouts, battle records) until it
//          overflowed (panic in gameStateNewGame, up 226 s, the end of an 8-round 4-bot game). Now resetStateLocked() rebuilds the
//          state in place in PSRAM (placement new). Also seen: after that panic the table rebooted but its web server didn't answer
//          until a USB reset -- not reproduced yet, watch for it.
// v3.1-dev.84 (2026-11-16, the user's first look at dev.83 on a phone): THE BLUR FOUND -- in the home-screen app iOS blurs the strip under
//          the status bar; the page leaves room for it (dev.44) but the full-screen layers didn't: the battle screen, the Final Hour /
//          reveal and transmissions now pad for the safe area (+40 px in standalone, like the body). THE COURT says so (a gift arrived
//          with "A DEAL ... MUSTAFAR"): planets send "tc"; a transmission caused by a Court you hold reads YOUR COURT / <WORLD>'S COURT.
//          BLACK MARKET: FOR NOW (lots: USE NOW / YOUR NEXT FIGHT / THIS ROUND) and FOR THE FINAL HOUR (big tickets, gambits) sections; a
//          more detailed broker (hood folds, rebreather, gloved hands on a lit datapad, a projector ring); a BLACK MARKET OPEN chip at the
//          bottom of Home while the door is off-screen (a tap scrolls to it; the door doesn't move). FIRST GAME: round 1 goes on -- after
//          your first ship Rivet points to GALAXY, and the first world page you open gets the FIGHT / TALK / PAY / reveal explanation;
//          a ROUND N CHECKLIST on Home in each Prepare of rounds 1-5 (fix and build your fleets, explore the galaxy, invest in defense),
//          ticked against how things stood when that Prepare began. Phone only.
// v3.1-dev.85 (2026-11-16): FIRST GAME fixes from a real game (user: "rivet is not popping up at all ... I turned it off and back on and
//          the little box ... went away"): switching FIRST GAME on starts the tour over (clears the introductions + checklists seen);
//          the ROUND N CHECKLIST gets an x like the NOW line and shrinks to a CHECKLIST n / 3 pill (per round). (The round-1 chain to
//          GALAXY was new in dev.84 -- the table was still on dev.83 during that game.)
// v3.1-dev.86 (2026-11-16): THE BROKER IS A PIXEL SPRITE (user: "make the black market guy a pixel sprite like the rest"; after trying a
//          head-tailed woman and a hooded woman: "dark and mysterious ... lets go back to the broker idea we had in the beginning"): drawn by
//          the character generator (tools/art/portraits_v2.html, preset brokerA: hood, glowing amber eyes, a respirator, amber mask lights,
//          a worn scarf -- no faction colours), packed as BROKER_ART (24x28 + 32x40) and drawn by brokerURL(): 32x40 in the black market
//          header, 24x28 through the portraits' hologram filter on the Home door, tiny on black-market stamps. Replaces the SVG line art.
// v3.1-dev.87 (2026-11-18): FLASH SPACE (user: "if you want to work on the flash space now ... lets go for it"; flash was at 91%): the
//          phone page was in flash TWICE -- plain (~600 KB, only a fallback) and gzipped (~178 KB, what every phone gets). PAGE_HTML now
//          stays in the source (make_page_gz.py + the phone harness read it there) but nothing uses it at run time, so it isn't linked;
//          / always sends the gzip. A static_assert (sizeof(PAGE_HTML) vs SF_PAGE_SRC_LEN) fails the BUILD if the page changed without
//          re-running tools/page/make_page_gz.py -- the compile-time replacement for the old "stale gzip -> serve plain" check.
// v3.1-dev.88 (2026-11-18) -- THE ERAS + POWERS TALK, BUILT (mockups: canvas boards 12-17).
//   ERAS: 2 rounds (ERA_ROUNDS), LOCKED once declared (no replacing), every sting hits EVERYONE (the champion too; Family
//   is no longer spared), the champion's perk is the reward. THE SPICE AGE = income x2 (champion x3), 2 goods a shop /
//   bribing a world x1.5 (no more ship +1). IMPERIAL ORDER = patrols half price (no more records) / tax 1 CR per world
//   past your 2nd, champion too / champion collects half. THE REBELLION RISES = hope (talks -1, free repair) / ANYONE can
//   start an Uprising (no Friend needed; THE PEOPLE RESIST gone) / talk-won worlds +2. THE DAWN GRIPS THE GALAXY replaces
//   the Shadow Age (no hidden score, no whispers, no lie) = the black market open every round / anyone may buy a rival's
//   file once a round for 6 CR (they hear "someone bought your file") / the champion NAMES THE MARK (5 CR from the Dawn
//   for a world taken from them). Phone: a personal FORECAST on the declare sheet, your faction PRESSES you each Prepare
//   you hold a decree (never declares for you), and an Era chip on every number it changes (income, bribe price,
//   patrols, negotiate questions, uprisings, the markets).
//   WARPATH replaces Declare war: declared in Prepare, struck the same Resolution with every ready fleet + 3 ships
//   (WARPATH_SHIPS; no War Room). Summit / Deep Cover one-liners; Deep Cover keeps its planted story.
//   BLACK MARKET: 6 cheap lots (Forged orders 5, Hired blade 4, Clean getaway 3, Inside man 4, Decoy beacon 4, Field
//   refit 5), four lots a round (one always <= 6 CR), dealt fresh each round it's open; THE CARGO HOLD keeps up to 3 lots
//   until used (tap: refit / forged / inside man; on their own: blade, decoy, getaway, saboteur, turncoat, second chance).
//   GALAXY AND YOU: one line a row, 3 on Home, red pulsing dot = a threat, amber blinking = needs you, the fix as a
//   button; the same rows close the round's news as YOUR SITUATION. RENAMES: TALK -> NEGOTIATE, PAY -> BRIBE.
// v3.1-dev.89 (2026-11-20) -- WARPATH works in any phase (user: "everyone elses works whenever"); the strike is still this
//   round's Resolution, so declaring early gives the table time to brace and declaring late doesn't.
// v3.1-dev.90 (2026-11-20) -- PLAYTEST SWEEP: the old War Room endpoint refuses (the bot still called it; WARPATH attacks
//   through a normal battle), a Summit on your Warpath gets PULL BACK on the Warpath sheet, Hired blade also fires when
//   YOUR world is attacked (its text said so), leftover 3-round Era text -> 2, Deep Cover no longer promises Era safety.
//   tools/shakedown/bot.py plays Warpath (declares any phase, attacks that world first).
// v3.1-dev.91 (2026-11-20) -- PHONE CLEANUP for launch (user: "fight screens shouldn't resize ever ever", "auto after 2 ships left
//   should still work", character select "a hair uncentered"): the battle text box is a fixed 50 px and the buttons a fixed
//   two-row block (bring-a-fleet / hired ships share the second row with WITHDRAW); AUTO pauses once at 2 ships, and tapping
//   AUTO again fights to the end; the character grid is balanced on the right so the portraits sit centred.
// v3.1-dev.92 (2026-11-20) -- FAVOR IS FROZEN IN THE LAST ROUND (user: "reputation should not be affected in the last round.
//   betrayal should not be possible in the final hour"): applyReputationLocked does nothing once snapshotFhFinal -- no favor
//   moves, no betrayal snap, no tier transmissions, no champion changes. The phone's favor preview says so instead of arrows.
// v3.1-dev.93 (2026-11-20) -- WAR SPOILS (user: the Warlord "could almost never be a champion" -- after mid-game the fights are
//   against players, which move no favor): every world a Warlord takes by force, from anyone, = +3 favor with their OWN faction
//   (CLASS_WARLORD_SPOILS), stamped WAR SPOILS; not for a world leaning to their own faction, not in the last round. On the kit.
// v3.1-dev.94 (2026-11-20) -- CHARACTER SELECT, LESS READING (user: "isn't quite so much reading"; "relaying the info twice"):
//   the card keeps the story untouched and adds one gold line on how the class plays (CLASS_PITCH, e.g. the Consul's "Smooth
//   talker, avoids fights.") + the once-a-game power's name; the rule boxes are gone -- YOUR KIT opens the full kit (same rows
//   as in a game, + the quest). Wording: "talks" -> negotiations (Blunt / Silver tongue), Deep Cover warns attacking their
//   worlds gets you found out, the Heist mentions push your luck.
// v3.1-dev.95 (2026-11-20) -- the character select's YOUR KIT is the same big amber button as the one on your card in a game
//   (user: the thin one "looks like the cheapest button we have made").
// v3.1-dev.96 (2026-11-20) -- LOBBY CLEANUP (user: the screen after character select "seems to be really busy"; "halfway events are
//   untested and unready, shops are an integral part of the game now, the option to remove shouldn't be there"): the MARKETS and
//   GALAXY EVENT switches are gone (markets always on, the halfway event always off; /set_markets and /set_events ignore
//   their argument). The seat picker gets CONNECT THE TABLE TO HOME WI-FI too (while the table is on its own network). Rivet's
//   galaxy guide: no halfway row, the Rebellion's sting is the uprising.
// v3.1-dev.97 (2026-11-20) -- PULL A FILE (user: the Infiltrator's "your file" trait "should be 'pull a file' and should maybe
//   have a 2 round cooldown, not every round"): a new pull every FILE_COOLDOWN (2) rounds; the pulled file stays readable until
//   the next pull; /state sends fileNext (the round of the next pull). Renamed on the card, the kit and Galaxy and You.
// v3.1-dev.98 (2026-11-20) -- THE BACK CHANNEL, the Consul's second strength (canvas boards 20-22; user: "I love the idea of a
//   back deal"): once a round a private offer to one player -- N CR (1-10) if they don't attack any of the Consul's worlds / a world /
//   don't negotiate for or bribe a world / don't attack a player, this round. Paid on accept. Broken (checked when a battle starts or
//   a world changes hands by talk, bribe or uprising): the breaker pays back N (all they see) and the Consul gets 2N -- the bank adds
//   the rest; Galaxy News + the table: a deal was broken, no names. The phone warns before an attack / negotiation / bribe that
//   would break it (GO AHEAD ANYWAY / KEEP MY WORD). NEWS_MARK moved to 42 (NEWS_BC_BROKEN = 41).
// v3.1-dev.99 (2026-11-20, during a playtest) -- (1) a PvP VICTORY counts when the loser withdraws: +VP_PVP_DECISIVE_WIN to whoever
//   holds the field (user: '"destroying" an enemy's fleet isn't my choice'); a mutual withdrawal still gives nothing. (2) YOUR KIT
//   lists FROM YOUR WORLDS AND FRIENDS: each held world's trait (e.g. Shipyards: ships cost 1) and every Family faction's gift
//   (user: Rhel 'wasn't sure why ships cost 1'); the hangar's Shipyards tag names the world. No GameState change (saves resume).
// v3.1-dev.100 (2026-11-20, during a playtest) -- the BLACK MARKET opens during the Dawn's Era (user: "we can't get into the black
//   market during the crimson dawn era"): the page only rendered with s.fh (the Final Hour); now also with s.bm.
// v3.1-dev.101 (2026-11-20) -- honest odds + RALLY (playtest: "I went in with a 90% chance ... found 5 extra ships and lost";
//   "the dice feel super stacked"). The dice were checked: 278 dice tonight, evenly spread; no hidden bonus for any player.
//   (1) The forecast's defender range for a player's world now runs up to its limit + smuggled troops (hidden), and adds a
//   public patrol to both ends -- it used to stop at the limit, so 9 defenders behind a "4-6" forecast read as 90%.
//   (2) RALLY: a side that loses 3 exchanges in a row gets +1 on its next roll (stamp RALLY +1), then the count starts over;
//   ties don't break a run. Planet + PvP battles. Battle-local.
//   PLUS the playtest batch (GameState changed -- old saves are dropped): (3) Field Refit works on a WIPED-OUT fleet (back at the
//   home hangar). (4) The forecast counts what joins: your hired blade / ship of the line / warpath; their hidden decoy, ship of
//   the line, safe world on the top end; the word (STRONG / FAVORED / RISKY / LONG SHOT) follows the WORST case. (5) DEFENSE INTEL
//   = the defenders you SAW (garrison + patrol), "MORE / FEWER SINCE YOU LOOKED" + LOOK AGAIN when it changes; the forecast uses it
//   while it's fresh. (6) OUTNUMBERED (the old invisible help): 3+ / 7+ more ships at the first exchange = best of 2 / 3 dice for
//   the whole battle, every PLANET battle (held worlds too), never PvP; still invisible, now in the forecast. (7) NEGOTIATE, THEN
//   FIGHT: a failed / walked-out talk opens that world to a fight the same round, +1 defender (ON ALERT); no second talk or bribe.
//   (8) Patrols stay until beaten. (9) CONSUL REWORK: soft hands gone; NO STOMACH FOR WAR (-3 favor with the world's faction on a
//   forced take); OLD FRIENDS (every Sympathy known); contacts on every question; hostile factions still talk; TREATY WORLDS
//   (+2 defenders, no bribe / buyout for 2 rounds, +1 CR a round); uprisings at any favor; home worlds by talk (5 hard, no
//   misses, once, shaky after); the back channel works in the Final Hour. (10) One buy per faction shop a round, one black-market
//   lot a round. (11) Base income +1 every 2 rounds (3 -> 7 by round 9); the Scoundrel's stash = base + 3. (12) The Mark is never
//   the player in last place. (13) /games: one line per finished game (factions, characters, classes, VP, Era, winner).
// v3.1-dev.102 (2026-11-20) -- STAGE A of the playtest redesign (canvas https://claude.ai/artifact/W93KkCWW9NKwG9Py7Z4Uof):
//   (1) DICE: the extra dice + Rally are gone. The BIGGER SIDE (FH_HELP2+ more ships at the first exchange) WINS TIES and, after
//   STEADY_AFTER lost exchanges in a row, rolls best of 2 once (the steady hand). Planet battles only, invisible, exact in the
//   forecast (battleWinPct walks every state). (2) NOBODY WASTES A TURN (replaces dust settling): a world won by talk / bribe is
//   INK STILL WET until the round ends -- one attack, against its old defenders; taken by force = DUG IN (+2). Deal fell through:
//   a bribe comes back; a talk pays 2x the world's VP (once a round) + a HEAD START with its faction (next talk starts a question
//   in; never a Consul's). Anyone who had the world open when it fell gets BEATEN TO IT: fight anyway / REDIRECT (worlds one step
//   away, snapshotNeighbors) / STAND BY (fleet spent now, +2 ships on its first attack next round). (3) GUNS FOR HIRE (the
//   Warlord's interactive move): once a round offer 1-3 ships for 1-15 CR; accepted = paid, the ships leave, the hirer's next
//   battle (either side, never against the Warlord) gets them in front; each rolls 3-6 to come home; unused by the end of the
//   next round = all home; +2 favor with the hirer's faction. (4) Consul contacts: the hardest question only. (5) Every Galaxy
//   and You row opens something (rows with nothing to do explain themselves).
// v3.1-dev.103 (2026-11-20) -- THE COCKPIT (canvas https://claude.ai/artifact/W93KkCWW9NKwG9Py7Z4Uof boards 19-23; user: "it needs
//   to feel like a cockpit"). Five decks on a fixed cockpit console: DOCKS (market / black market) . STAR MAP (the galaxy; the
//   spinning planet is its key) . BRIDGE (home, a raised hex key) . COMMS (the Galaxy and You, grouped NEEDS YOU / DEALS AND
//   PROMISES / THE GALAXY, with THIS PREPARE's list on top) . WAR TABLE (standings + YOUR FAVOR; its key = live VP bars per
//   player). A curved dash with the faction's trim (Rebels striped, Empire grey panels, Pykes riveted, Dawn red), angled keys,
//   console lights (COMMS red when something needs you, DOCKS amber when a market is open), dash LEDs (your move / your world is
//   under attack), canopy struts + glint, a holo-flicker + buzz on switching, swipe between neighbouring decks (never from the
//   map window or a sheet). LOCK IN = a launch control: HOLD half a second (a fill runs, it clicks with a buzz); a tap says
//   "hold". LIVE NOW bar on BRIDGE / COMMS / WAR TABLE: a battle or talk anywhere, one tap to watch / whisper. MERGES: one
//   character sheet (your face and any Standings row open the same card, with the class pitch + YOUR / THEIR KIT); one Era sheet
//   (the running Era + your decree); one gear menu (Rivet's guides + How to Play on top of Settings).
// v3.1-dev.104 (2026-11-21) -- cockpit fixes from the user's first look on his phone: LOCK IN back on the BRIDGE only, at the
//   end of the page (it scrolls with it; still held) -- it no longer floats over every deck and sheet; the console sits UNDER
//   the sheets (z 35); switching decks closes an open world / the attack picker; a real HOLOGRAM REFRESH on switching
//   (#holoFx: scanlines flicker across ~340 ms); swiping works on the STAR MAP unless it's zoomed in (or two fingers).
// v3.1-dev.105 (2026-11-21) -- the console on a real iPhone (user screenshot): the BRIDGE hex edge was drawn on the stretched dash
//   SVG and drifted off the key -> the edge is now two clipped layers ON the key; DOCKS / WAR TABLE ran off the screen and into
//   the home bar -> gentler tilt (18 / 9 deg), outer keys pulled in, the console pads env(safe-area-inset-bottom); the dash
//   LEDs were under the keys -> moved under the BRIDGE key.
// v3.1-dev.106 (2026-11-21) -- console lights MEAN something (user: the pulsing lights on COMMS and DOCKS "never go away"; urgent
//   things must still be communicated): COMMS pulses red only for an urgent row you haven't seen (commsSeen), then a steady dot until
//   it's dealt with, dark when nothing needs you; DOCKS lights only for a black market you haven't visited this round. Dash LEDs in
//   one centered holder. Stat boxes all centered vertically (CREDITS / VICTORY PTS looked odd next to the PLANETS button).
// v3.1-dev.107 (2026-11-21) -- LOCK IN hold 480 -> 900 ms (user: "a little too fast"); a phone that sees a new firmware reloads
//   STAGGERED (0.3-3.3 s) and RETRIES every 20 s until the new page arrives (dev.106 never reached the user's app: its one
//   reload was cut off during the post-flash memory crunch, and the once-per-version flag kept the old page).
// v3.1-dev.108 (2026-11-21) -- from the user's iPhone screenshot: the raised hex BRIDGE key "takes me out of the experience" ->
//   ONE LEVEL ROW of five keys (BRIDGE a little wider, glows when active), a flatter dash, gentle tilt on the outer pairs only,
//   all inside 3%..97% (WAR TABLE still ran off the right edge); the PLANETS stat button sized exactly like CREDITS / VICTORY PTS.
// v3.1-dev.109 (2026-11-21) -- user: dev.108 was wrong ("I WANT the hex button to raise"): the angled cockpit row and the RAISED HEX
//   BRIDGE are back, the hex pinned to the exact center (calc(50% - 42px), 84 px) so the dash LEDs sit right under it (they were
//   centered on the screen while the hex sat a few px right); NO BLACK STRIP at the bottom -- the dash plate itself runs down under
//   the iPhone home bar (.dash height + safe-area inset) instead of a padded strip. Kept from dev.108: the three matching stat boxes.
// v3.1-dev.110 (2026-11-21) -- CRASH / WEDGE FIXES + the star field. (1) BOOT WATCHDOG: three times after a USB flash the table drew
//   its screen but never finished starting Wi-Fi / the web server (before loop(), where the loop watchdog begins; RAM was fine) ->
//   if loop() hasn't started 60 s after power-up, restart (the save resumes); the next boot says so. (2) LOW-MEMORY GUARD: free
//   internal RAM hit 0-2 KB ~9 s after restarts as a reloading phone opened page + font + portraits + icon + a first full /state at
//   once -> below BIG_RAM_FLOOR (16 KB) the page answers a tiny 'Waking up the table' that retries in 2 s, the font / portraits /
//   a first /state answer busy (phones back off; the portraits are now re-asked until they arrive). Test: tools/shakedown/
//   reboot_storm.py (restart + N phones reloading at once, per cycle). (3) STAR FIELD behind the canopy (slow drift, shifts with
//   the deck); the dash LEDs ride ON the BRIDGE key under its point.
// v3.1-dev.111 (2026-11-21) -- THE REAL CAUSE OF THE 'FROZEN' TABLE (overnight reboot storm: 6 of 30 boots, caught by
//   tools/shakedown/catch_wedge.py): the router REJECTED the first login (reason 202) on its WPA2/WPA3 network; the table took it
//   for a wrong password, ran its own network ('ShadowFleet', 192.168.4.1) and -- a game being saved -- never tried home Wi-Fi
//   again, so phones on the home Wi-Fi found nothing. Now: a rejected login is retried up to 3 times at boot (rejections come back
//   in ~1 s); the background retry runs every 30 s (first at 20 s) whenever NOBODY is on the table's own network, mid-game too (30
//   tries); rejections no longer stop it. BOOT_WATCH_S 60 -> 120 (room for the retries). Twice the stars.
// v3.1-dev.112 (2026-11-21) -- MORE RAM HEADROOM (the reload burst still dipped to 2-5 KB free on ~1 boot in 10): (1) the TABLE
//   build puts LVGL's 64 KB working memory in PSRAM (tools/lvconf/lv_conf.h knob SF_LV_MEM_PSRAM; board_bsp.cpp refuses to
//   build the table without it); the draw strips stay in internal DMA RAM. (2) sendBounded queues nothing more while free
//   internal RAM is under SEND_RAM_FLOOR (20 KB) -- each phone's unacked page / font bytes (~5.7 KB) sit in internal RAM;
//   /health counts the waits.
//   The who-goes-first screen no longer plays at START (user: "100% useless"). The page's version is now checked against
//   FIRMWARE_VERSION at compile time (a stale page made every phone reload every 20 s, replaying the round banner).
// v3.1-dev.113 (2026-11-22, during a playtest) -- a PvP win by the enemy WITHDRAWING now also keeps your "win a battle" /
//   "beat a fleet" promises (dev.99 gave the +2 VP but skipped promiseOnPvpWinLocked; a Pyke debt stayed open). Serial
//   "PVPWIN <slot>" credits a missed win in a game already running.
// v3.1-dev.114 (2026-11-22) -- PLAYTEST FIX BATCH 1 (two family games on dev.112/113): RULES: one buy per faction SHOP a round
//   (dev.101 had limited the old world markets the phone no longer uses -- the stalls had no limit; Spice Age: two); UPRISING
//   needs no Friend standing (the hard questions + one try stay; the Rebellion's sting becomes "try again"); a CEASEFIRE
//   (Summit) world takes no buyouts or uprisings; the Consul TREATY's +1 CR ends with the treaty (through next round); JOBS
//   give 3 rounds (JOB_ROUNDS 2 -> 3) and say "within N rounds"; take-a-world favors add "Any way counts: fight, negotiate or
//   bribe."; TURNCOAT 12 -> 5 CR. PHONE: no double-tap zoom; the VP box opens the War Table; every sideways ">" chevron gone;
//   notifications flick up to dismiss; stamps from a finished battle wait until the battle screen closes; the defense sheet
//   shows your Credits; LOCK IN asks again only with a negotiation open (it's held now); NEGOTIATE centred; a BUYOUT OFFER
//   opens by itself for the holder and both sheets say where every Credit goes; CEASEFIRE chip on the world page; the
//   seat picker's debug line is gone; ink still wet + Forged Orders say what happens on the board.
// v3.1-dev.115 (2026-11-22) -- BATCH 2: WORLDS + MAP (canvas boards 24 / 25; user: "smoother, less to scroll around").
//   NO MORE BUYING INTEL: every world's DEFENSE shows as a public, live level (/state "dv": LOW 1-2 / MEDIUM 3-5 / HIGH 6-8 /
//   FORTRESS 9+ of its defenders right now, patrols included -- the same word means the same strength everywhere); SYMPATHY
//   grows outward: holding a world (home worlds from the start) shows its neighbours' (snapshotNeighbors; the INFILTRATOR hears
//   two jumps out -- LONG EARS replaces "first intel free"; Spymaster 15 -> 12). WORLD PAGE: the take-over buttons, then
//   DEFENSE + SYMPATHY tiles, RESOURCES + TRAIT lines, ONE JUMP AWAY buttons, a pager; swipe sideways = next world; it closes
//   only by dragging its top bar (the drag is kept from the browser: no pull-to-refresh) -- sheets too. STAR MAP: no zoom, the
//   map across the whole width, under it EVERY world in one list (holder, VP, defense level, side); ceasefire worlds ringed.
//   AUDIENCE shows your standing bar with each faction.
// v3.1-dev.116 (2026-11-22) -- BATCH 3: LAST ORDERS + THE GAMBIT MOMENT (canvas boards 29 / 30). LAST ORDERS: your Credits in
//   the corner (what's left after jump passes + hires); one card per fleet = where it strikes + its total ships; under a strike,
//   HIRED SHIPS (the CASH-IN -- family: "a way to cash in unused credits"; FH_HIRE_CR 3 each, up to FH_HIRE_MAX 5 a strike,
//   kept in ordAlly's high nibble so saves still resume; charged with the jump passes, dropped if you can't pay) and a GAMBIT
//   slot that opens a picker (pinning used to cycle on each tap); reveal gambits are listed as "saved for the reveal". THE
//   REVEAL: "every strike is on the map", then YOUR GAMBIT gets its own card -- PLAY <NAME> or KEEP IT -- and I'M READY waits
//   until you've chosen; who's ready shows as badges. No skip.
// v3.1-dev.117 (2026-11-22) -- BATCH 4: THE FINAL HOUR IN STEP (canvas board 27; user: "unified so everyone is watching at the
//   same time. all players have to press skip"). The TABLE keeps the clock (fhTickLocked, statics -- not saved): step 0 = the
//   reveal, then one step per world fought, each as long as its animation (fhStepMs); every phone shows the step the table is on
//   (/state fh.show / sk / left; the playback holds each result until the table moves on) and a step SKIPS ONLY WHEN EVERY PLAYER
//   STILL HERE ASKS (/fh_skip, tap again to take it back; the bar shows who has); SKIP TO THE END is gone from the live finale.
//   The table's headline for a world (FH_TAKEN / FH_HOLDS) now lands when that world's result does, not all at once; the
//   summary holds 8 s, then the final standings. REPLAY afterwards still plays on its own clock.
// v3.1-dev.118 (2026-11-22) -- BATCH 5: PHONE HEAT + BATTERY (playtest: "everyone's phones are really hot"). (1) The STAR MAP key's
//   turning planet asked for every screen frame (60/s, all game) to check a clock -- now one timer tick per spin step, none while
//   calm or hidden. (2) ADAPTIVE POLLING: 0.7 s while something is happening (a touch in the last 20 s, a change in the last
//   15 s, a battle, the finale, the lobby / game over), 1.5 s when the table is quiet, 5 s with the screen hidden; an instant
//   poll when it comes back. (3) CALM: after 25 s without a touch (and 8 s without a change) every looping CSS animation pauses
//   (24 of them: blinking lamps, glows, holo flicker -- lights stay lit) until the next touch.
// v3.1-dev.119 (2026-11-22) -- the user's look at dev.117: (1) the STAR MAP's detailed rows (holder + why, VP, defense, side) moved
//   from ALL (too long) to THREATS / CHANCES / MINE / SIDE (gallRows); ALL is the clean map. (2) The Credits box reads "44 - 18"
//   under what's left (defense sheet, Last Orders). (3) THE MAP IN THE FINALE (canvas board 7, never built until now): each world
//   opens on the galaxy map with a ring flashing on the target ("<WORLD> IS UNDER ATTACK", 1.8 s), the map shrinks while the
//   fight plays, and at the result the world flashes the winner's colour (fhMapHtml; the table's step allows the extra 1.8 s).
//   (4) SKIP = one small button in the corner, any time in the finale; once everyone still here has pressed it, the table jumps
//   straight to the summary (votes last the whole finale; "n/m" on the button).
// v3.1-dev.120 (2026-11-22) -- DRAG TO CLOSE FOLLOWS YOUR FINGER (user: "the swipe down animation doesn't seem to exist"): the world
//   page's slide-up animation (wsUp, fill-mode both) held its transform, so the drag's translateY never showed; the drag now
//   switches the animation off as it starts. The drag may start anywhere in the top 90 px (the bar + header; was 56).
// v3.1-dev.121 (2026-11-22) -- the user's first finale on dev.119/120 ("the flashing looks good!"): (1) NO REVEAL SCREEN -- it was
//   "a totally blank screen with a skip button" when nobody played a reveal gambit; the finale opens on the first strike (the
//   reveal's gambits are in the summary; a REPLAY skips it too). (2) The FIRST STRIKE's order glitch: for ~0.5 s before the
//   table's clock ticked (fh.show not sent yet) the phone ran the finale on its own timer, then got pulled back -- the phone now
//   follows the table from its first finale poll (fhLive = fh.st 2) and drops its own timer when it falls in step.
// v3.1-dev.122 (2026-11-22, overnight) -- THE LEFTOVERS: (1) HOW TO PLAY + Rivet's FIRST GAME tour speak the cockpit (BRIDGE / STAR MAP
//   / COMMS / DOCKS / WAR TABLE, hold LOCK IN), the rising base pay, one buy per shop, ties to the bigger side, defense on every
//   world + Sympathy from your neighbours; the round checklist's EXPLORE = open the STAR MAP (localStorage sfMap). (2) The game
//   record (/games) adds each player's income in / spent out (not saved; "~" after a restart). (3) JOBS aim near you: the first
//   pass only offers worlds within two jumps of one you hold, one jump away twice as likely. (4) WHISPER + VOUCH buttons say their
//   rule ("I don't understand the rules behind vouching"). (5) PULL A FILE moved from COMMS to the WAR TABLE, above the faces you
//   tap. (6) CALLING IN DEBTS opens by itself once in the last Prepare (the button stays). (7) SAVE A REPORT in the gear menu:
//   /report = the table's health + the game records + this game + its last battles, as a .txt download.
// v3.1-dev.123 (2026-11-22, overnight) -- THE TABLE'S STRIKE (canvas board 27): as each finale strike begins, the round screen shows
//   NEWS_FH_STRIKE -- "STRIKE n OF m" round the rim, the world big, "<CHARACTER> ATTACKS" (or "n STRIKES AT ONCE") under it, in the
//   attacker's colour -- then the FH_TAKEN / FH_HOLDS result headline when the phones reach the result (fhStrikeHeadlineLocked).
// v3.1-dev.124 (2026-11-23) -- HOW TO PLAY = A GUIDED WALKTHROUGH (canvas board 31; user: "completely redo how to play", "guided
//   walkthrough, a little flavor"): TOUR, 13 steps -- Rivet dims the screen and spotlights the REAL button (#tourSpot over the
//   element's rect, the bubble above / below it), one flavor line + one plain instruction each; "try it" steps move on when you
//   tap the real thing (NEXT always works, SKIP TOUR ends it). Starts by itself in a FIRST GAME (round 1 Prepare, once:
//   sfTourDone); HOW TO PLAY in the gear menu replays it (outside a game: the old cards). FIRST TIMES (user: "first time you
//   fight it tells you what is going to happen, the first negotiate tells you the goal ... explains a promise the first time you
//   see one"): Rivet sheets on your first battle (before the first roll), first negotiation, first favor offer -- once per phone
//   (sfFirst), off with Rivet's switch. Neither fires on the harness.
// v3.1-dev.125 (2026-11-23) -- THE FACTIONS FOR NEWCOMERS (a new player: "a page somewhere that gives the basic characteristics of
//   the factions for someone who doesn't know star wars ... maybe a drop down on the negotiate page"): FACTION_GUIDE -- who they
//   are, THEY LIKE / THEY HATE, how to answer them -- as WHO ARE THEY? under the faction's name in a negotiation, and THE FACTIONS
//   in the gear menu (all four). The unknown ambassador's hint no longer says to "scout".
// v3.1-dev.126 (2026-11-23) -- small fixes from the user's look at the previews: SMUGGLERS fits its 54 px column in Last Orders
//   (.fhfl.long, 8 px / .5 px spacing); a hidden Resources / Trait on the world page says "Unknown" instead of a bare "?" (only
//   the harness ever hid them -- they are public from round 1). Arduino IDE backup: tools/release/arduino_ide/ (build_opt.h with
//   the table's flags, a ready lv_conf.h with the PSRAM pool) staged into the public repo.
// v1.0 (2026-11-23) -- THE PUBLIC RELEASE (MakerWorld + github.com/anakin130013/ShadowFleetGame). Same game as v3.1-dev.126 (the
//   user flashed dev.126 from the live web installer as a customer: "worked great"); only the version name changes. The 3.1-dev
//   line ends here; later builds count on from v1.0.
// v1.0.1 (2026-11-23) -- UNUSED HOME WORLDS ARE ORDINARY OUTER RIM WORLDS (user: "unused homeworld would be good to be like a
//   normal outer rim world"; playtest: their special rules confused people). liveHomeOf(gs, p) = the home faction only while
//   someone plays it; every rule check uses it (guard, VP, the Negotiate / Bribe refusal, buyout, uprising, the Consul's home
//   talks, news, the forecast) and /state "home" sends it, so the phone shows such a world like any other. A home world nobody
//   plays: the Outer Rim guard, 2 VP, talks and bribes allowed; its Sympathy stays its own faction. HOME_GUARD_NEUTRAL unused.
//   Docs: the quick start + rulebook say to put a claim token AND fleet marker 1 on your home world.
// v1.0.2 (2026-11-23) -- THE EMPTY RIVET BOX (user screenshot, THE GAMBITS ARE DEALT): a Galaxy News item with no facts (no world,
//   no faction) drew an empty readout box holding only Rivet's button, its "for you" line hidden behind him. newsReadout now
//   shows that line as a FOR YOU row instead. The gambit line no longer names the old Galaxy and You screen. Harness: newsgambit.

