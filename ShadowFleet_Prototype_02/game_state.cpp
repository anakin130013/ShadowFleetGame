#include "game_state.h"
#include "commander_art.h"
#include "transmission_bank.h"
#include "negotiation_bank.h"
#include <Preferences.h>
#include <LittleFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string.h>
#include <stdarg.h>
#include <initializer_list>
#include <new>
#ifndef SF_TWIN
#include "esp_heap_caps.h"
#endif

// The single shared game state instance and its access mutex. Declared here,
// at the top, so every function below (including the reputation helpers,
// which are defined before the older functions that historically declared
// these) can reference them. C++ has no hoisting for namespace-scope
// variables -- this must come before first use, not just first "main" use.
// 3.1-dev.3: the state lives in PSRAM (8 MB), not the table's scarce internal
// RAM (~5 KB freed there). PSRAM is up before C++ globals are built
// (CONFIG_SPIRAM_BOOT_INIT), and game logic isn't hot enough to feel its speed.
template <class T> static T *psNew() {
#ifndef SF_TWIN
  void *p = heap_caps_malloc(sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = malloc(sizeof(T));
#else
  void *p = malloc(sizeof(T));
#endif
  return new (p) T();
}
static GameState &state = *psNew<GameState>();
static EventLog &evlog = *psNew<EventLog>();
static SemaphoreHandle_t stateMutex = nullptr;
// Event log writers (defined beside setOwnerLocked; used throughout).
static void evPubLocked(uint8_t kind, int who, int planet, int other, int num);
static void evMineLocked(int slot, uint8_t kind, int who, int planet, int other, int num);
static void sellOfferLocked(int slot, int buyer, int kind, int planet);  // 3.1-dev.48 (below)
static void gallerySettleLocked(int talker, int p, bool won);  // 3.1-dev.52 (below)
static void evMomentLocked();

// ---- Console link (v0.22.1) -------------------------------------------------
// Set on a command console that mirrors a table console: every function that
// changes the game forwards itself to the table (first line of each) instead
// of touching the local mirror, which the link overwrites with the table's
// state. Never set on the table / a hosting console.
static GameRemoteFn g_remote = nullptr;
void gameStateSetRemote(GameRemoteFn fn) { g_remote = fn; }

static RemoteResult remoteCall(uint8_t fn, std::initializer_list<int32_t> a, const char *str = nullptr) {
  RemoteResult r;
  memset(&r, 0, sizeof(r));
  int32_t args[6] = { 0 };
  int n = 0;
  for (int32_t v : a) if (n < 6) args[n++] = v;
  if (!g_remote(fn, args, n, str, &r)) {
    r.value = -1;  // treated as failure by every caller (bool funcs check == 1, PvP != 0)
    snprintf(r.text, sizeof(r.text), "Lost contact with the table.");
  }
  return r;
}

// ---------------------------------------------------------------------------
// Static galaxy data
// ring = which ring of the map (v0.25.0; the starting garrison comes from it
// and the Defense trait, see planetStartGarrison); basePayPrice = starting
// asking price before resource/defense scaling. v0.25.0 also set BESPIN and
// DATHOMIR to Defense High and CORELLIA to Low, so each ring has both levels.
// ---------------------------------------------------------------------------
const PlanetInfo PLANETS[NUM_PLANETS] = {
  // 2.0 (2026-11-01/02, the 17-world board): Dathomir moved out to the Outer
  // Rim (Crimson Dawn's home world), Endor in to the Mid Rim; five new worlds
  // at the end so the old indices stay put.
  { "TATOOINE", { "Crimson Dawn", "High", "Valuable Intelligence", "Low" }, RING_OUTER, 3 },
  { "HOTH", { "Rebel Alliance", "Low", "Hidden Rebel Base", "High" }, RING_OUTER, 4 },
  { "BESPIN", { "Crimson Dawn", "High", "Tibanna Gas Refinery", "High" }, RING_MID, 4 },
  { "DAGOBAH", { "Rebel Alliance", "Low", "Ancient Jedi Presence", "Low" }, RING_OUTER, 2 },
  { "CORUSCANT", { "Galactic Empire", "High", "Imperial Seat of Power", "High" }, RING_CORE, 8 },
  { "KESSEL", { "Pyke Syndicate", "High", "Spice Mining Operation", "High" }, RING_OUTER, 5 },
  { "OBA DIAH", { "Pyke Syndicate", "High", "Pyke Syndicate Stronghold", "High" }, RING_OUTER, 6 },
  { "DATHOMIR", { "Crimson Dawn", "Low", "Nightsister Magic", "High" }, RING_OUTER, 3 },
  { "CORELLIA", { "Galactic Empire", "High", "Corellian Shipyards", "Low" }, RING_CORE, 6 },
  { "NABOO", { "Rebel Alliance", "Low", "Royal Naboo Palace", "Low" }, RING_MID, 3 },
  { "ENDOR", { "Rebel Alliance", "Low", "Ewok Village", "Low" }, RING_MID, 2 },
  { "KIJIMI", { "Pyke Syndicate", "Low", "Black Market Hub", "Low" }, RING_MID, 3 },
  { "YAVIN 4", { "Rebel Alliance", "High", "Massassi Temple Base", "High" }, RING_OUTER, 4 },
  { "MUSTAFAR", { "Galactic Empire", "High", "Fortress Vader", "High" }, RING_OUTER, 4 },
  { "NAL HUTTA", { "Pyke Syndicate", "High", "Hutt Palaces", "Low" }, RING_MID, 3 },
  { "CANTO BIGHT", { "Crimson Dawn", "High", "Casino City", "High" }, RING_MID, 3 },
  { "KUAT", { "Galactic Empire", "High", "Kuat Drive Yards", "High" }, RING_CORE, 6 },
};

// ---- v0.25.4: the shuffled galaxy ----
static const char *AFFILIATION_NAMES[NUM_FACTIONS] = { "Rebel Alliance", "Galactic Empire", "Pyke Syndicate", "Crimson Dawn" };

static int classicFaction(int p) {  // the PLANETS table's affiliation (before a game / fallback)
  const char *aff = PLANETS[p].characteristic[0];
  for (int f = 0; f < NUM_FACTIONS; f++) if (strcmp(aff, AFFILIATION_NAMES[f]) == 0) return f;
  return FACTION_CRIMSON_DAWN;
}

// 2.0 HOME WORLDS (2026-11-01), indexed by FACTION_*: Yavin 4, Mustafar, Oba Diah, Dathomir.
static const int8_t HOME_PLANET[NUM_FACTIONS] = { 12, 13, 6, 7 };

int homePlanetOf(int f) { return (f >= 0 && f < NUM_FACTIONS) ? HOME_PLANET[f] : -1; }
int planetHomeOf(int p) {
  for (int f = 0; f < NUM_FACTIONS; f++) if (HOME_PLANET[f] == p) return f;
  return -1;
}
int homeSlotOf(const GameState &gs, int f) {
  if (f < 0 || f >= NUM_FACTIONS) return -1;
  for (int i = 0; i < MAX_PLAYERS; i++) if (gs.players[i].active && gs.players[i].chosenFaction == f) return i;
  return -1;
}
// v1.0.1 (user 2026-11-23: unused home worlds "would be good to be like a normal outer rim world"): a home world whose
// faction nobody plays follows the Outer Rim rules -- the ring's guard, 2 VP, Negotiate / Bribe / Buyout / Uprising.
int liveHomeOf(const GameState &gs, int p) {
  const int home = planetHomeOf(p);
  return home >= 0 && homeSlotOf(gs, home) >= 0 ? home : -1;
}

// Starting (and rebuilt-after-capture) garrison. A home world held by its own
// faction keeps the home guard -- also while it waits for them at the start;
// a conqueror holds it with an ordinary Outer Rim garrison; nobody playing
// that faction = the neutral guard.
static int startGarrisonOf(const GameState &gs, int p) {
  if (p < 0 || p >= NUM_PLANETS) return GARRISON_OUTER;
  const int home = liveHomeOf(gs, p), owner = gs.planetOwner[p];
  if (home >= 0) {
    if (owner >= 0 && owner < MAX_PLAYERS) return gs.players[owner].chosenFaction == home ? HOME_GUARD : GARRISON_OUTER;
    return HOME_GUARD;
  }
  static const int RING_BASE[3] = { GARRISON_OUTER, GARRISON_MID, GARRISON_CORE };
  const int ring = PLANETS[p].ring;
  int g = RING_BASE[(ring >= 0 && ring < 3) ? ring : 0];
  if (gs.planetDefIsHigh[p]) g += GARRISON_HIGH_DEFENSE;
  return g > MAX_PLANET_DEFENSE ? MAX_PLANET_DEFENSE : g;
}

int planetFaction(int p) { return (p >= 0 && p < NUM_PLANETS) ? state.planetFactionOf[p] : FACTION_CRIMSON_DAWN; }
bool planetResHigh(int p) { return p >= 0 && p < NUM_PLANETS && state.planetResIsHigh[p]; }
bool planetDefHigh(int p) { return p >= 0 && p < NUM_PLANETS && state.planetDefIsHigh[p]; }

const char *planetCharValue(const GameState &gs, int p, int c) {
  if (p < 0 || p >= NUM_PLANETS) return "";
  switch (c) {
    case 0: return AFFILIATION_NAMES[(gs.planetFactionOf[p] >= 0 && gs.planetFactionOf[p] < NUM_FACTIONS) ? gs.planetFactionOf[p] : 3];
    case 1: return gs.planetResIsHigh[p] ? "High" : "Low";
    case 3: {  // v0.28.3: describes the planet's real starting strength (playtest 2026-10-19:
      // a seat of power read "Low" yet had its 6-7 capital guard -- "I went into hoth thinking
      // it was gonna be easy and it was stacked"). Low 2, Medium 3-4, High 5-6, Fortress 7+.
      const int g = startGarrisonOf(gs, p);
      return g >= 7 ? "Fortress" : g >= 5 ? "High" : g >= 3 ? "Medium" : "Low";
    }
    case 2: return traitName(gs.planetTrait[p], p);  // 2.0: traits are dealt each game
    default: return PLANETS[p].characteristic[c];
  }
}

const char *planetCharValueNow(int p, int c) { return planetCharValue(state, p, c); }
int planetVPNow(int p, int slot) { return planetVP(state, p, slot); }

// The classic galaxy (the PLANETS table) -- what the Lobby and a fresh boot hold.
static void loadClassicGalaxyLocked() {
  for (int p = 0; p < NUM_PLANETS; p++) {
    state.planetFactionOf[p] = classicFaction(p);
    state.planetResIsHigh[p] = strcmp(PLANETS[p].characteristic[1], "High") == 0;
    state.planetDefIsHigh[p] = strcmp(PLANETS[p].characteristic[3], "High") == 0;
    state.planetMarket[p] = 0;
    // 2.0: the classic trait and good for each world (what 1.0's traits were).
    static const uint8_t CLASSIC_TRAIT[NUM_PLANETS] = {
      TR_SPYNET, TR_HIDDENBASE, TR_REFINERY, TR_ANCIENT, TR_COURT, TR_MINES,
      TR_COURT, TR_TRAINING, TR_SHIPYARDS, TR_MEDICAL, TR_TRADEHUB, TR_BLACKMARKET,
      TR_HIDDENBASE, TR_TRAINING, TR_TRADEHUB, TR_BLACKMARKET, TR_SHIPYARDS };
    state.planetTrait[p] = CLASSIC_TRAIT[p];
    state.planetGood[p] = (uint8_t)(p % GOOD_COUNT);
    state.marketStock[p] = 0;
    state.planetUnbound[p] = 0;
  }
  for (int i = 0; i < MARKETS_OPEN; i++) state.marketOpen[i] = -1;
}

static void shuffleInts(int *a, int n) {
  for (int i = n - 1; i > 0; i--) { const int j = esp_random() % (i + 1); const int t = a[i]; a[i] = a[j]; a[j] = t; }
}

// Rolled at START: home worlds keep their faction, the other 13 are dealt
// round the factions (3 each, one random faction gets a 4th); Defense High/Low
// shuffled within each ring, home worlds aside (they have the home guard);
// Resources 8 High / 9 Low; a market offset of 0..PRICE_MARKET_MAX per planet.
static void shuffleGalaxyLocked() {
  int rest[NUM_PLANETS], nRest = 0;
  for (int p = 0; p < NUM_PLANETS; p++) {
    if (planetHomeOf(p) >= 0) state.planetFactionOf[p] = planetHomeOf(p);
    else rest[nRest++] = p;
  }
  int deal[NUM_PLANETS];
  const int first = esp_random() % NUM_FACTIONS;  // who gets the 13th world
  for (int i = 0; i < nRest; i++) deal[i] = (first + i) % NUM_FACTIONS;
  shuffleInts(deal, nRest);
  for (int i = 0; i < nRest; i++) state.planetFactionOf[rest[i]] = deal[i];

  for (int ring = RING_OUTER; ring <= RING_CORE; ring++) {
    int in[NUM_PLANETS], n = 0;
    for (int p = 0; p < NUM_PLANETS; p++) {
      if (PLANETS[p].ring != ring) continue;
      if (planetHomeOf(p) >= 0) state.planetDefIsHigh[p] = 0;
      else in[n++] = p;
    }
    shuffleInts(in, n);
    for (int i = 0; i < n; i++) state.planetDefIsHigh[in[i]] = i < n / 2;
  }

  int all[NUM_PLANETS];
  for (int p = 0; p < NUM_PLANETS; p++) all[p] = p;
  shuffleInts(all, NUM_PLANETS);
  for (int i = 0; i < NUM_PLANETS; i++) state.planetResIsHigh[all[i]] = i < NUM_PLANETS / 2;
  for (int p = 0; p < NUM_PLANETS; p++) state.planetMarket[p] = esp_random() % (PRICE_MARKET_MAX + 1);

  // 2.0: deal the traits and the market goods to the worlds (17 worlds, 11
  // traits and 12 goods -- some come up twice in a game).
  int deck[NUM_PLANETS];
  {  // 2.0-dev.32: with markets off, the Black Market trait (a cut of every sale) isn't dealt
    // 3.1-dev.19: SIX traits, each one you can see when it works (income line, hangar price, a stamp).
    // Cut: Spy network (= the Infiltrator), Black market (= the old market cut), Hidden base, and the
    // +1 / +3 income ones (folded into the Refinery's +2).
    static const uint8_t KEEP[] = { TR_REFINERY, TR_SHIPYARDS, TR_ANCIENT, TR_COURT, TR_MEDICAL, TR_TRAINING };
    int traits[TR_COUNT], nt = 0;
    for (uint8_t t : KEEP) traits[nt++] = t;
    for (int i = 0; i < NUM_PLANETS; i++) deck[i] = traits[i % nt];
  }
  shuffleInts(deck, NUM_PLANETS);
  for (int p = 0; p < NUM_PLANETS; p++) state.planetTrait[p] = (uint8_t)deck[p];
  for (int i = 0; i < NUM_PLANETS; i++) deck[i] = i % GOOD_COUNT;
  shuffleInts(deck, NUM_PLANETS);
  for (int p = 0; p < NUM_PLANETS; p++) state.planetGood[p] = (uint8_t)deck[p];

  Serial.print("[GALAXY] shuffled:");
  for (int p = 0; p < NUM_PLANETS; p++)
    Serial.printf(" %s=%c%c%c", PLANETS[p].name, "REPC"[state.planetFactionOf[p]],
                  state.planetDefIsHigh[p] ? 'D' : 'd', state.planetResIsHigh[p] ? 'R' : 'r');
  Serial.println();
}

int planetStartGarrison(int planetIdx) { return startGarrisonOf(state, planetIdx); }

static int reputationTierLocked(int score);  // defined below
static int rivalFactionIndex(int factionIdx);  // defined below
void snapshotRumor(const GameState &gs, char *out, size_t n, int *planet) {
  // 2.0-dev.41 (user: "someone is pretty interested in kijimi, huh?" / "maybe the table needs a personality"):
  // every line TRUE, in one sly gossip voice, never says who. Three tiers:
  //  1 TARGETED -- someone bought intel on a world another player HOLDS (always wins when there is one);
  //  2 WORLD LEAKS -- an unclaimed world's thin garrison, a world's hidden leaning (works with 2 players);
  //  3 PLAYER GOSSIP -- hired guns, a pile of Credits, cozy / angry factions, favors owed, a limping fleet, a quest.
  // One pick per round (a fixed seed, so every phone and the table agree).
  struct Cand { char t[84]; int8_t p; };  // ~2.3 KB static (the table's internal RAM is tight)
  static Cand top[6], rest[22];
  int nt = 0, nr = 0;
  const char *const LEAN[NUM_FACTIONS] = { "the Rebellion", "the Empire", "the Pykes", "Crimson Dawn" };
  static char nm[NUM_PLANETS][20];  // "KIJIMI" -> "Kijimi" (gossip doesn't shout)
  for (int p = 0; p < NUM_PLANETS; p++) {
    const char *src = PLANETS[p].name; size_t k = 0;
    for (; src[k] && k < sizeof(nm[0]) - 1; k++) nm[p][k] = (k == 0 || src[k - 1] == ' ') ? src[k] : (char)tolower((unsigned char)src[k]);
    nm[p][k] = 0;
  }
  for (int sl = 0; sl < MAX_PLAYERS; sl++) {
    const Player &pl = gs.players[sl];
    if (!pl.active || pl.chosenFaction < 0) continue;
    for (int p = 0; p < NUM_PLANETS; p++) {
      if (!(pl.intelEyed & (1u << p))) continue;
      const int own = gs.planetOwner[p];
      if (own >= 0 && own != sl && nt < 6) {
        snprintf(top[nt].t, sizeof(top[0].t), liveHomeOf(gs, p) >= 0 && liveHomeOf(gs, p) == gs.players[own].chosenFaction
                 ? "Someone has been casing %s. Bold." : "Someone is pretty interested in %s, huh?", nm[p]);
        top[nt++].p = (int8_t)p;
      } else if (own < 0 && nr < 16) {
        snprintf(rest[nr].t, sizeof(rest[0].t), "Someone has been asking around about %s.", nm[p]);
        rest[nr++].p = (int8_t)p;
      }
    }
    if (nr >= 16) continue;
    auto add = [&](int p) { rest[nr++].p = (int8_t)p; };
    if (pl.nextBattleMercs > 0) { snprintf(rest[nr].t, sizeof(rest[0].t), "Someone hired a couple of extra guns. Wonder why."); add(-1); }
    if (pl.credits >= 20) { snprintf(rest[nr].t, sizeof(rest[0].t), "Someone is sitting on %d Credits. Must be nice.", pl.credits); add(-1); }
    for (int f = 0; f < NUM_FACTIONS && nr < 20; f++) {
      if (f == pl.chosenFaction) continue;
      const int t = reputationTierLocked(pl.reputation[f]);
      if (t == REP_ALLIED) { snprintf(rest[nr].t, sizeof(rest[0].t), "Someone is awfully cozy with %s lately.", LEAN[f]); add(-1); }
      else if (t == REP_HOSTILE && f != rivalFactionIndex(pl.chosenFaction)) { snprintf(rest[nr].t, sizeof(rest[0].t), "Someone has made %s very, very angry.", LEAN[f]); add(-1); }
    }
    for (int k = 0; k < MAX_PENDING_OBLIGATIONS && nr < 20; k++)
      if (pl.obligations[k].active && pl.obligations[k].factionIdx >= 0 && pl.obligations[k].factionIdx < NUM_FACTIONS)
        { snprintf(rest[nr].t, sizeof(rest[0].t), "Someone owes %s a favor. They will collect.", LEAN[pl.obligations[k].factionIdx]); add(-1); break; }
    for (int f = 0; f < NUM_FLEETS && nr < 20; f++) {
      int alive = 0; for (int sh = 0; sh < FLEET_SHIPS; sh++) if (pl.fleetShipAlive[f][sh]) alive++;
      const int lost = pl.fleetShipCount[f] - alive;
      if (alive > 0 && lost > 0) { snprintf(rest[nr].t, sizeof(rest[0].t), "Someone's fleet is limping home, %d ship%s short.", lost, lost == 1 ? "" : "s"); add(-1); break; }
    }
    int goal = 1; const int prog = snapshotQuestProgress(gs, sl, &goal);
    if (!pl.questDone && goal > 1 && prog >= goal - 1 && nr < 20) { snprintf(rest[nr].t, sizeof(rest[0].t), "Someone is one step from finishing their quest. Tick tock."); add(-1); }
  }
  // World leaks: what the galaxy itself lets slip.
  uint32_t seed = (uint32_t)gs.round * 2654435761u;
  for (int p = 0; p < NUM_PLANETS; p++) seed = seed * 31 + (uint32_t)(gs.planetFactionOf[p] + 1);
  for (int p = 0; p < NUM_PLANETS && nr < 22; p++) {
    if (gs.planetOwner[p] >= 0 || liveHomeOf(gs, p) >= 0) continue;
    if (gs.planetDefense[p] < startGarrisonOf(gs, p)) {
      snprintf(rest[nr].t, sizeof(rest[0].t), "%s is down to %d defenders. You didn't hear it from me.", nm[p], gs.planetDefense[p]);
      rest[nr++].p = (int8_t)p;
    }
  }
  {  // one world's hidden leaning (a world not everyone knows yet)
    int pick = -1, seen = 0;
    for (int p = 0; p < NUM_PLANETS; p++) {
      if (liveHomeOf(gs, p) >= 0) continue;
      bool someoneBlind = false;
      for (int sl = 0; sl < MAX_PLAYERS; sl++) if (gs.players[sl].active && !gs.players[sl].revealed[p][0]) someoneBlind = true;
      if (someoneBlind && ((seed >> 3) + (uint32_t)p * 7u) % (uint32_t)(++seen) == 0) pick = p;
    }
    const int f = pick >= 0 ? gs.planetFactionOf[pick] : -1;
    if (f >= 0 && f < NUM_FACTIONS && nr < 22) {
      snprintf(rest[nr].t, sizeof(rest[0].t), "A little bird says %s leans toward %s.", nm[pick], LEAN[f]);
      rest[nr++].p = (int8_t)pick;
    }
  }
  // 3.1-dev.28: the same few things stay true round after round (a knocked-down garrison stays down), so
  // last round's world sits out while anything else can be said.
  // Same for the KIND of gossip ("Someone is sitting on N Credits" stays true for a rich player): the
  // first 12 letters of last round's line sit out too.
  static int8_t lastRound = -1, pickNow = -2, pickBefore = -2;
  static char kindNow[13] = "", kindBefore[13] = "";
  if (gs.round != lastRound) {
    pickBefore = pickNow; pickNow = -2; lastRound = (int8_t)gs.round;
    memcpy(kindBefore, kindNow, sizeof(kindBefore)); kindNow[0] = 0;
  }
  if (nr > 1) {
    int keep = 0;
    for (int i = 0; i < nr; i++) {
      const bool sameWorld = pickBefore >= 0 && rest[i].p == pickBefore;
      const bool sameKind = kindBefore[0] && strncmp(rest[i].t, kindBefore, 12) == 0;
      if (!sameWorld && !sameKind) { if (keep != i) rest[keep] = rest[i]; keep++; }
    }
    if (keep > 0) nr = keep;
  }
  const Cand *c = nt ? &top[seed % (uint32_t)nt] : nr ? &rest[seed % (uint32_t)nr] : nullptr;
  if (c) { pickNow = c->p; snprintf(kindNow, sizeof(kindNow), "%.12s", c->t); }
  if (planet) *planet = c ? c->p : -1;
  snprintf(out, n, "%s", c ? c->t : "Quiet night on the Holonet. Too quiet.");
}
// 3.1-dev.101: what defends a world right now (its garrison + a live patrol) -- what revealing Defense shows.
int snapshotDefendersNow(const GameState &gs, int p) {
  if (p < 0 || p >= NUM_PLANETS) return 0;
  return gs.planetDefense[p] + ((gs.planetOwner[p] >= 0 && gs.patrolUntil[p] >= gs.round) ? gs.patrolShips[p] : 0);
}
// 3.1-dev.101: -1 never looked, 0 still what you saw, 1 more now, 2 fewer now
int snapshotDefSeenState(const GameState &gs, int slot, int p) {
  if (slot < 0 || slot >= MAX_PLAYERS || p < 0 || p >= NUM_PLANETS || !gs.players[slot].defSeen[p]) return -1;
  const int seen = gs.players[slot].defSeen[p] - 1, now = snapshotDefendersNow(gs, p);
  return now == seen ? 0 : now > seen ? 1 : 2;
}
// 3.1-dev.101: the active player with strictly the fewest VP, when someone else could be named instead (the Mark)
bool snapshotIsLastPlace(const GameState &gs, int slot) {
  if (slot < 0 || slot >= MAX_PLAYERS || !gs.players[slot].active) return false;
  int others = 0;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (i == slot || !gs.players[i].active) continue;
    if (gs.players[i].vp <= gs.players[slot].vp) return false;
    if (gs.eraFaction < 0 || gs.champion[gs.eraFaction] != i) others++;
  }
  return others > 0;
}
void snapshotGarrisonRange(const GameState &gs, int slot, int p, int *lo, int *hi) {
  *lo = *hi = GARRISON_OUTER;
  if (p < 0 || p >= NUM_PLANETS || slot < 0 || slot >= MAX_PLAYERS) return;
  const Player &me = gs.players[slot];
  const int owner = gs.planetOwner[p];
  // 3.1-dev.101 (playtest 2026-11-20: "the percentages need to accurately show if ships will be joining"): a player's world can
  // bring ships the attacker can't see -- a decoy, the Empire's ship of the line, the Rebels' safe world. They go on the TOP.
  int hidden = 0;
  if (owner >= 0 && owner < MAX_PLAYERS && owner != slot) {
    hidden = 1;  // a decoy beacon
    if (gs.players[owner].chosenFaction == FACTION_EMPIRE) hidden += GIFT_LINE_SHIPS;
    if (gs.players[owner].chosenFaction == FACTION_REBEL && snapshotSafeWorld(gs, owner) == p) hidden += GIFT_SAFE_SHIPS;
  }
  const bool fresh = me.revealed[p][3] && snapshotDefSeenState(gs, slot, p) == 0;
  if (fresh) {
    *lo = *hi = me.defSeen[p] - 1;  // you looked and nothing changed since
    *hi += hidden;
  } else if (liveHomeOf(gs, p) >= 0 && owner < 0) {
    *lo = *hi = startGarrisonOf(gs, p);  // an unheld home guard's size is public
  } else if (owner >= 0 && owner < MAX_PLAYERS) {
    *lo = startGarrisonOf(gs, p);        // a player's world: anything up to a full garrison
    // 3.1-dev.101 (playtest 2026-11-20: "a 90% chance ... and then found 5 extra ships"): up to its limit PLUS smuggled
    // troops (hidden, past the limit) + the hidden helpers, and the patrol (public) on both ends.
    *hi = min(MAX_PLANET_DEFENSE, max(snapshotDefenseLimit(gs, p), startGarrisonOf(gs, p)) + LOT_TROOPS_SHIPS) + hidden;
    if (gs.patrolUntil[p] >= gs.round && gs.patrolShips[p] > 0) { *lo += gs.patrolShips[p]; *hi += gs.patrolShips[p]; }
  } else {
    static const int RING_BASE[3] = { GARRISON_OUTER, GARRISON_MID, GARRISON_CORE };
    const int ring = PLANETS[p].ring;
    *lo = RING_BASE[(ring >= 0 && ring < 3) ? ring : 0];
    *hi = min(MAX_PLANET_DEFENSE, *lo + GARRISON_HIGH_DEFENSE);
  }
  // A militia: certain once you know the world's Sympathy, otherwise possible
  // whenever you are Hostile with anyone (that's all you can tell).
  const int pf = gs.planetFactionOf[p];
  if (me.revealed[p][0]) {
    if (reputationTierLocked(me.reputation[pf]) == REP_HOSTILE) { *lo += BATTLE_MILITIA_SHIPS; *hi += BATTLE_MILITIA_SHIPS; }
  } else {
    for (int f = 0; f < NUM_FACTIONS; f++)
      if (reputationTierLocked(me.reputation[f]) == REP_HOSTILE) { *hi += BATTLE_MILITIA_SHIPS; break; }
  }
  if (me.alertRound == gs.round && me.alertWorld == p) { *lo += ALERT_DEFENDERS; *hi += ALERT_DEFENDERS; }  // 3.1-dev.101 they saw you coming
  if (gs.phase == PHASE_RESOLUTION && gs.planetTakenRound[p] == gs.round && owner != slot) { *lo += DUG_IN_DEF; *hi += DUG_IN_DEF; }  // 3.1-dev.102 dug in
}

// affiliation, resources, unique, defense
const int REVEAL_COST[NUM_CHARACTERISTICS] = { 2, 2, 1, 3 };  // 2.0: the trait is 1 CR (user 2026-10-29)

const char *ATK_SHIP_NAMES[FLEET_SHIPS] = { "X-WING", "Y-WING", "CORVETTE", "ESCORT A", "ESCORT B", "ESCORT C", "ESCORT D" };
// 2.0-dev.34 CRASH FIX (playtest 2026-11-04: the table restarted in a battle and crash-
// looped on resume): battle ship indexes run f * FLEET_SHIPS + slot (0..BATTLE_SHIPS-1)
// since v0.28.0, but ATK_SHIP_NAMES has one fleet's worth -- losing a Fleet 2 / 3 ship
// read past the array and snprintf'd a garbage pointer (LoadProhibited, addr 0x1).
static const char *shipNameOf(int idx) { return (idx >= 0) ? ATK_SHIP_NAMES[idx % FLEET_SHIPS] : "ship"; }

// Display-style faction names (all caps, matching how both UIs already show
// them) -- used so a player's "name" can just BE their faction once chosen,
// no separate typed name needed.
static const char *FACTION_DISPLAY_NAMES[NUM_FACTIONS] = {
  "REBEL ALLIANCE", "GALACTIC EMPIRE", "PYKE SYNDICATE", "CRIMSON DAWN"
};
const char *PHASE_NAMES[3] = { "PREPARE", "MOVEMENT", "RESOLUTION" };  // 2.0: Invest is now Prepare


// Planet names are stored in capitals for the map; speak them as names.
static size_t speakPlanetName(int planetIdx, char *out, size_t room) {
  const char *name = (planetIdx >= 0 && planetIdx < NUM_PLANETS) ? PLANETS[planetIdx].name : "this world";
  size_t o = 0;
  for (int i = 0; name[i] && o + 1 < room; i++)
    out[o++] = (i == 0 || name[i - 1] == ' ') ? name[i] : (char)tolower((unsigned char)name[i]);
  return o;
}

void negFillPlanet(const char *src, int planetIdx, char *out, size_t outSize) {
  if (!outSize) return;
  size_t o = 0;
  while (src && *src && o + 1 < outSize) {
    if (strncmp(src, "{planet}", 8) == 0) { o += speakPlanetName(planetIdx, out + o, outSize - o); src += 8; }
    else out[o++] = *src++;
  }
  out[o] = '\0';
}

// v0.30.1: what a planet is worth while you hold it (VP on capture). 2.0: a
// home world is worth 2 to its own faction and 6 to anyone who takes it.
int planetVP(const GameState &gs, int planetIdx, int slot) {
  if (planetIdx < 0 || planetIdx >= NUM_PLANETS) return 0;
  const int home = liveHomeOf(gs, planetIdx);
  if (home >= 0) return VP_HOME;  // 2.0-dev.23: any home world, whoever holds it
  const int ring = PLANETS[planetIdx].ring;
  return ring == RING_CORE ? VP_PLANET_CORE : ring == RING_MID ? VP_PLANET_MID : VP_PLANET_OUTER;
}

// v0.30.1: a late Credits favor costs your PAY price x 3/2, at least +4 -- always
// more than buying it outright, so a missed late negotiation is a gamble.
int snapshotLateFavorPrice(const GameState &gs, int slot, int planetIdx) {
  const int pay = snapshotPayPrice(gs, slot, planetIdx);
  const int scaled = (pay * NEG_LATE_PRICE_NUM + NEG_LATE_PRICE_DEN - 1) / NEG_LATE_PRICE_DEN;
  return max(scaled, pay + NEG_LATE_PRICE_MIN_EXTRA);
}

// 3.1-dev.114 (playtest: a new player read "Strike the Empire: take an Imperial world" as FIGHT): take-a-world favors say any way counts.
#define FAVOR_ANY_WAY " Any way counts: fight, negotiate or bribe."
static bool favorTakesWorld(uint8_t check) { return check == PROMISE_TAKE_FACTION || check == PROMISE_TAKE_CORE; }
void snapshotFavorText(const GameState &gs, int slot, int favor, int planetIdx, char *out, size_t outSize) {
  if (!outSize) return;
  out[0] = '\0';
  if (favor < 0 || favor >= NEG_FAVOR_COUNT) return;
  char tmp[200];
  negFillPlanet(NEG_FAVOR_BANK[favor].text, planetIdx, tmp, sizeof(tmp));
  const char *any = favorTakesWorld(NEG_FAVOR_BANK[favor].check) ? FAVOR_ANY_WAY : "";
  const char *cr = strstr(tmp, "{cr}");
  if (!cr) { snprintf(out, outSize, "%s%s", tmp, any); return; }
  snprintf(out, outSize, "%.*s%d%s%s", (int)(cr - tmp), tmp, snapshotLateFavorPrice(gs, slot, planetIdx), cr + 4, any);
}

// ---------------------------------------------------------------------------
// Reputation
// ---------------------------------------------------------------------------
const char *REP_TIER_NAMES[5] = { "HOSTILE", "WARY", "NEUTRAL", "FAVORABLE", "ALLIED" };
const char *GARRISON_TIER_NAMES[3] = { "LOW", "MEDIUM", "HIGH" };

int snapshotGarrisonTier(const GameState &gs, int planetIdx) {
  if (planetIdx < 0 || planetIdx >= NUM_PLANETS) return GARRISON_LOW;
  int count = gs.planetDefense[planetIdx];
  if (count <= 2) return GARRISON_LOW;
  if (count <= 5) return GARRISON_MEDIUM;
  return GARRISON_HIGH;
}

int gameStateGarrisonTier(int planetIdx) {
  int tier = GARRISON_LOW;
  if (stateMutex && xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
    tier = snapshotGarrisonTier(state, planetIdx);
    xSemaphoreGive(stateMutex);
  }
  return tier;
}

bool gameStateHasLostAt(int slot, int planetIdx) {
  if (slot < 0 || slot >= MAX_PLAYERS || planetIdx < 0 || planetIdx >= NUM_PLANETS) return false;
  bool lost = false;
  if (stateMutex && xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
    lost = state.players[slot].hasLostAt[planetIdx];
    xSemaphoreGive(stateMutex);
  }
  return lost;
}

static int factionIndexForPlanet(int planetIdx) { return planetFaction(planetIdx); }  // v0.25.4: shuffled per game

int gameStateFactionForPlanet(int planetIdx) {
  return factionIndexForPlanet(planetIdx);
}

static int rivalFactionIndex(int factionIdx) {
  switch (factionIdx) {
    case FACTION_REBEL: return FACTION_EMPIRE;
    case FACTION_EMPIRE: return FACTION_REBEL;
    case FACTION_PYKE: return FACTION_CRIMSON_DAWN;
    case FACTION_CRIMSON_DAWN: return FACTION_PYKE;
    default: return factionIdx;
  }
}

static int reputationTierLocked(int score) {
  if (score <= REP_HOSTILE_MAX) return REP_HOSTILE;  // v0.29.0 scale
  if (score <= REP_WARY_MAX) return REP_WARY;
  if (score <= REP_NEUTRAL_MAX) return REP_NEUTRAL;
  if (score <= REP_FAVORABLE_MAX) return REP_FAVORABLE;
  return REP_ALLIED;
}

// ---------------------------------------------------------------------------
// Transmission engine (v0.23.0)
// ---------------------------------------------------------------------------
// Every bucket of the bank (faction x cause, faction x planet, moment x
// faction) is a shuffled DECK: lines come out in a random order and none
// repeats until the deck is used up -- across games too, because each deck's
// shuffle seed and position live in flash (NVS "sftx"). Only the console that
// runs the rules (the table host, or a 1.75" hosting alone) draws.
static const int DECK_CAUSE0 = 0;                                       // + f*TXC_COUNT + cause
static const int DECK_PLANET0 = DECK_CAUSE0 + NUM_FACTIONS * TXC_COUNT;  // + f*NUM_PLANETS + planet
static const int DECK_BIG0 = DECK_PLANET0 + NUM_FACTIONS * NUM_PLANETS;  // + moment*NUM_FACTIONS + f
static const int NUM_DECKS = DECK_BIG0 + TXB_COUNT * NUM_FACTIONS;
struct TxDecks { uint32_t seed[NUM_DECKS]; uint8_t pos[NUM_DECKS]; };
static TxDecks decks;

static void loadDecks() {
  Preferences p;
  p.begin("sftx", true);
  if (p.getBytesLength("decks") != sizeof(decks) || p.getBytes("decks", &decks, sizeof(decks)) != sizeof(decks))
    memset(&decks, 0, sizeof(decks));  // first boot (or the bank's shape changed): fresh decks
  p.end();
}

static void saveDecks() {
  Preferences p;
  p.begin("sftx", false);
  p.putBytes("decks", &decks, sizeof(decks));
  p.end();
}

// The deck's order: a Fisher-Yates shuffle of 0..count-1 driven by its seed.
static int deckCard(uint32_t seed, int count, int pos) {
  uint8_t order[16];
  if (count > 16) count = 16;
  for (int i = 0; i < count; i++) order[i] = i;
  uint32_t x = seed ? seed : 0x9E3779B9u;
  for (int i = count - 1; i > 0; i--) {
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;  // xorshift32
    const int j = x % (i + 1);
    const uint8_t t = order[i]; order[i] = order[j]; order[j] = t;
  }
  return order[pos % count];
}

// Next line of a deck, or -1 if the bucket is empty. `wrap` = reshuffle when
// used up (cause/moment decks); planet decks don't wrap on their own -- see
// pickLineLocked.
static int drawLocked(int deck, const TxBucket &b, bool wrap) {
  if (b.count == 0) return -1;
  if (decks.pos[deck] >= b.count) {
    if (!wrap) return -1;
    decks.pos[deck] = 0;
    decks.seed[deck] = 0;
  }
  if (decks.pos[deck] == 0 && decks.seed[deck] == 0) decks.seed[deck] = esp_random() | 1;
  const int line = b.start + deckCard(decks.seed[deck], b.count, decks.pos[deck]);
  decks.pos[deck]++;
  saveDecks();
  return line;
}

// Line for a normal transmission. When you GAIN a planet and the speaker has
// lines about that planet, those come first; once they're used up the
// cause deck takes over, and the planet lines return when the speaker's cause
// deck has gone all the way round.
static int pickLineLocked(int faction, int cause, int planet) {
  const bool gain = cause == TXC_TOOK || cause == TXC_BETRAY || cause == TXC_RIVAL || cause == TXC_PAID || cause == TXC_DEAL;
  if (gain && planet >= 0 && planet < NUM_PLANETS) {
    const int line = drawLocked(DECK_PLANET0 + faction * NUM_PLANETS + planet, TX_PLANET_BUCKETS[faction][planet], false);
    if (line >= 0) return line;
  }
  const int deck = DECK_CAUSE0 + faction * TXC_COUNT + cause;
  const TxBucket &b = TX_CAUSE_BUCKETS[faction][cause];
  if (decks.pos[deck] >= b.count)  // about to reshuffle: this speaker's planet lines come back too
    for (int p = 0; p < NUM_PLANETS; p++) decks.pos[DECK_PLANET0 + faction * NUM_PLANETS + p] = 0, decks.seed[DECK_PLANET0 + faction * NUM_PLANETS + p] = 0;
  return drawLocked(deck, b, true);
}

// Mirrors the queue's head into the transmission* fields the UIs read.
static void syncTxHeadLocked(int slot) {
  const bool any = state.txCount[slot] > 0;
  const TxMsg &h = state.txQueue[slot][0];
  state.transmissionPending[slot] = any;
  state.transmissionFaction[slot] = any ? h.faction : -1;
  state.transmissionIsObligation[slot] = any && h.line < 0;
  state.transmissionObligationIdx[slot] = (any && h.line < 0) ? h.obIdx : -1;
}

static void pushTxLocked(int slot, const TxMsg &m) {
  if (state.txCount[slot] >= TX_QUEUE_LEN) {
    Serial.printf("[TX] queue full for P%d -- dropped a transmission\n", slot + 1);
    return;
  }
  state.txQueue[slot][state.txCount[slot]++] = m;
  syncTxHeadLocked(slot);
}

static void popTxLocked(int slot) {
  if (state.txCount[slot] == 0) return;
  for (int i = 1; i < state.txCount[slot]; i++) state.txQueue[slot][i - 1] = state.txQueue[slot][i];
  state.txCount[slot]--;
  state.txQueue[slot][state.txCount[slot]] = TxMsg();
  syncTxHeadLocked(slot);
}

// Queues big moment `moment` spoken by `faction` if this player hasn't had it
// yet this game. Returns true if it was queued.
static bool queueBigLocked(int slot, int moment, int faction, int planet, int fromTier, int toTier) {
  if (state.players[slot].active == false) return false;
  if (state.bigMomentsDone[slot] & (1u << moment)) return false;
  const int line = drawLocked(DECK_BIG0 + moment * NUM_FACTIONS + faction, TX_BIG_BUCKETS[moment][faction], true);
  if (line < 0) return false;
  state.bigMomentsDone[slot] |= 1u << moment;
  if (moment == TXB_FIRSTBLOOD || moment == TXB_SEAT || moment == TXB_CORE || moment == TXB_BETRAYAL || moment == TXB_FLEET) {
    state.publicMoment = moment;  // v0.24.4: the table shows these to everyone
    state.publicActor = state.players[slot].chosenFaction;
    state.publicVictim = faction;
    state.publicPlanet = planet;
    state.publicSeq++; evMomentLocked();
  }
  TxMsg m;
  m.line = line; m.faction = faction; m.planet = planet; m.big = 1;
  if (fromTier != toTier) { m.fromTier = fromTier; m.toTier = toTier; }
  pushTxLocked(slot, m);
  Serial.printf("[TX] P%d big moment %d from faction %d (line %d)\n", slot + 1, moment, faction, line);
  return true;
}

// You destroyed another player's whole fleet: the faction they play answers
// you (flavor only -- no reputation change).
static void promiseOnPvpWinLocked(int slot);  // v0.28.1 (with the obligations)
static void fleetDestroyedBigLocked(int winner, int loser) {
  promiseOnPvpWinLocked(winner);
  if (winner < 0 || winner >= MAX_PLAYERS || loser < 0 || loser >= MAX_PLAYERS) return;
  const int f = state.players[loser].chosenFaction;
  if (f >= 0 && f < NUM_FACTIONS) queueBigLocked(winner, TXB_FLEET, f, -1, -1, -1);
}

void snapshotTransmissionText(const GameState &gs, int slot, char *out, size_t outSize) {
  if (!outSize) return;
  out[0] = '\0';
  if (slot < 0 || slot >= MAX_PLAYERS || gs.txCount[slot] == 0) return;
  const TxMsg &m = gs.txQueue[slot][0];
  if (m.line < 0) {  // obligation check-in: the promise itself
    if (m.obIdx >= 0 && m.obIdx < MAX_PENDING_OBLIGATIONS)
      snprintf(out, outSize, "%s", gs.players[slot].obligations[m.obIdx].text);
    return;
  }
  if (m.line >= TX_LINE_COUNT) return;
  negFillPlanet(TX_LINES[m.line], m.planet, out, outSize);  // v0.30.0: shared with the conversation
}

// Big moments that `slot` gaining `planet` (any way: fight, pay or deal) could
// set off: someone else's home world (2.0; was the seats of power), or a Core
// world (once per player per game).
static uint8_t gainBigsLocked(int planet, int slot) {
  if (planet < 0 || planet >= NUM_PLANETS) return 0;
  uint8_t b = 0;
  const int home = liveHomeOf(state, planet);
  if (home >= 0 && slot >= 0 && slot < MAX_PLAYERS && state.players[slot].chosenFaction != home) b |= 1u << TXB_SEAT;
  if (PLANETS[planet].ring == RING_CORE) b |= 1u << TXB_CORE;
  return b;
}

// Assumes mutex already held. Applies a reputation delta and queues a
// transmission: a big moment if one applies (`bigs` = candidate TxBig bits,
// checked in priority order; reaching Allied adds TXB_ALLIED), else a normal
// line for `cause` if the tier crossed a boundary. `isConquest` triggers the
// betrayal rule: conquering a planet belonging to a faction you're
// currently Favorable+ with drops you straight to Neutral instead of just
// applying the normal delta (and the cause becomes TXC_BETRAY).
// 3.1-dev.8: worlds `slot` doesn't hold and hasn't learned characteristic `c` of, shuffled; up to n of them revealed.
static void revealSomeLocked(int slot, int c, int n, int *out) {
  int all[NUM_PLANETS], k = 0;
  for (int p = 0; p < NUM_PLANETS; p++)
    if (state.planetOwner[p] != slot && !state.players[slot].revealed[p][c]) all[k++] = p;
  shuffleInts(all, k);
  for (int i = 0; i < n; i++) { out[i] = i < k ? all[i] : -1; if (out[i] >= 0) state.players[slot].revealed[out[i]][c] = true; }
}
static bool addShipLocked(int slot);  // below
// 3.1-dev.8 GIFTS: a tier's gift comes the first time you cross into it, never twice;
// tier 5's is ongoing (computed from your tier wherever it applies) and goes when you drop.
static void favorGiftsLocked(int slot, int f, int oldTier, int newTier) {
  if (state.stage != STAGE_PLAYING || f < 0 || f >= NUM_FACTIONS) return;
  Player &pl = state.players[slot];
  if (pl.coverFaction == f) return;  // 3.1-dev.13 deep cover: fake favor -- no gifts
  if (newTier >= REP_FAVORABLE && !(pl.giftsGot & (1u << f))) {
    pl.giftsGot |= (uint8_t)(1u << f);
    int w[2] = { -1, -1 };
    switch (f) {
      case FACTION_EMPIRE: revealSomeLocked(slot, 3, GIFT_SURVEY_WORLDS, w); break;
      case FACTION_REBEL: w[0] = addShipLocked(slot) ? 1 : 0; break;
      case FACTION_PYKE: pl.credits += GIFT_LOAN_CR; break;
      case FACTION_CRIMSON_DAWN: revealSomeLocked(slot, 0, GIFT_SECRET_WORLDS, w); break;
    }
    if (f == FACTION_REBEL) evMineLocked(slot, LOG_GIFT, f, -1, -1, 0);  // other unused; the phone says where the ship went
    else evMineLocked(slot, LOG_GIFT, f, w[0], w[1], 0);
    Serial.printf("[FAVOR] P%d crossed into tier 4 with faction %d -- gift given\n", slot + 1, f);
  }
  if (newTier == REP_ALLIED && oldTier < REP_ALLIED) evMineLocked(slot, LOG_GIFT, f, -1, -1, 1);
  if (oldTier == REP_ALLIED && newTier < REP_ALLIED) evMineLocked(slot, LOG_GIFT, f, -1, -1, 2);
}
int snapshotEra(const GameState &gs) {
  return (gs.stage == STAGE_PLAYING && gs.eraFaction >= 0 && gs.round <= gs.eraUntil) ? gs.eraFaction : -1;
}
bool snapshotEraSpared(const GameState &gs, int slot) { (void)gs; (void)slot; return false; }  // 3.1-dev.88: every sting hits everyone
static bool eraChampLocked(const GameState &gs, int slot) { const int e = snapshotEra(gs); return e >= 0 && slot >= 0 && gs.champion[e] == slot; }
bool snapshotPayShut(const GameState &gs, int slot) { (void)gs; (void)slot; return false; }  // 3.1-dev.49 #17: no Era closes a verb (THE PEOPLE RESIST instead)
bool snapshotVpHidden(const GameState &gs, int slot) { (void)gs; (void)slot; return false; }  // 3.1-dev.88: the Shadow Age is gone
static void championUpdateLocked();  // in the news code below
static int tierOfLocked(int slot, int f) {
  return (slot >= 0 && slot < MAX_PLAYERS && f >= 0 && f < NUM_FACTIONS) ? reputationTierLocked(state.players[slot].reputation[f]) : REP_NEUTRAL;
}

static void applyReputationLocked(int slot, int factionIdx, int delta, bool isConquest,
                                  int cause, int planet, uint8_t bigs = 0) {
  // 3.1-dev.92 (user 2026-11-20: "reputation should not be affected in the last round. betrayal should not be possible in the
  // final hour"): favor is FROZEN in the last round -- no changes, no betrayal, no tier transmissions, no titles moving.
  if (snapshotFhFinal(state)) return;
  Player &pl = state.players[slot];
  int oldScore = pl.reputation[factionIdx];
  int oldTier = reputationTierLocked(oldScore);
  int newScore;
  if (isConquest && oldTier >= REP_FAVORABLE) {
    newScore = 0;  // betrayal: straight to Neutral, ignoring the normal delta
  } else {
    newScore = oldScore + delta;
    if (newScore > REP_MAX) newScore = REP_MAX;
    if (newScore < REP_MIN) newScore = REP_MIN;
  }
  pl.reputation[factionIdx] = newScore;
  int newTier = reputationTierLocked(newScore);
  favorGiftsLocked(slot, factionIdx, oldTier, newTier);  // 3.1-dev.8
  if (newTier != oldTier || newTier == REP_ALLIED) championUpdateLocked();  // 3.1-dev.12 (3.1-dev.50: Family scores move the champion rung too)
  if (isConquest && oldTier >= REP_FAVORABLE) cause = TXC_BETRAY;
  if (newTier == REP_ALLIED && oldTier < REP_ALLIED) bigs |= 1u << TXB_ALLIED;
  const int queuedBefore = state.txCount[slot];  // v0.29.0: stamp this change on what it queues
  static const uint8_t BIG_PRIORITY[] = { TXB_SEAT, TXB_BETRAYAL, TXB_CORE, TXB_OATHBREAKER, TXB_ALLIED, TXB_FIRSTBLOOD };
  bool sent = false;
  for (uint8_t b : BIG_PRIORITY)
    if (!sent && (bigs & (1u << b))) sent = queueBigLocked(slot, b, factionIdx, planet, oldTier, newTier);
  // A normal transmission only when the tier moves -- or for a promise kept / broken,
  // which always answers (v0.28.3: otherwise a judged promise could pass silently).
  if (!sent && (newTier != oldTier || cause == TXC_KEPT || cause == TXC_BROKE)) {
    TxMsg m;
    m.line = pickLineLocked(factionIdx, cause, planet);
    m.faction = factionIdx; m.planet = planet; m.fromTier = oldTier; m.toTier = newTier;
    m.cause = (int8_t)cause;
    if (m.line >= 0) pushTxLocked(slot, m);
  }
  const int moved = newScore - oldScore;
  for (int q = queuedBefore; q < state.txCount[slot] && q < TX_QUEUE_LEN; q++)
    if (state.txQueue[slot][q].faction == factionIdx) state.txQueue[slot][q].delta = (int8_t)constrain(moved, -100, 100);
  // 2.0-dev.23: no VP for reaching a tier any more -- standing pays through
  // prices, battle modifiers and talks.
}

int snapshotReputationTier(const GameState &gs, int slot, int factionIdx) {
  if (slot < 0 || slot >= MAX_PLAYERS || factionIdx < 0 || factionIdx >= NUM_FACTIONS) return REP_NEUTRAL;
  return reputationTierLocked(gs.players[slot].reputation[factionIdx]);
}

int gameStateReputationTier(int slot, int factionIdx) {
  int tier = REP_NEUTRAL;
  if (stateMutex && xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
    tier = snapshotReputationTier(state, slot, factionIdx);
    xSemaphoreGive(stateMutex);
  }
  return tier;
}

static void checkObligationsDueLocked(int slot);  // defined below

void gameStateClearTransmission(int slot) {
  if (g_remote) { remoteCall(RF_CLEAR_TRANSMISSION, {slot}); return; }
  if (slot < 0 || slot >= MAX_PLAYERS) return;
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return;
  popTxLocked(slot);                // v0.23.0: the next queued one (if any) becomes the head
  checkObligationsDueLocked(slot);  // a check-in that didn't fit the queue earlier
  xSemaphoreGive(stateMutex);
}

// ---------------------------------------------------------------------------
// Honor-system obligation follow-up
// ---------------------------------------------------------------------------
// Assumes mutex already held. Records a new obligation in the first free
// slot for this player (silently drops it if all MAX_PENDING_OBLIGATIONS
// slots are full -- a reasonable prototype limit rather than something a
// player is likely to hit).
static void queueObligationLocked(int slot, int factionIdx, int planetIdx, const char *text,
                                  uint8_t check = PROMISE_HONOR, int8_t target = -1, int8_t offer = -1) {
  Player &pl = state.players[slot];
  for (int i = 0; i < MAX_PENDING_OBLIGATIONS; i++) {
    if (!pl.obligations[i].active) {
      pl.obligations[i].active = true;
      pl.obligations[i].check = check;  // v0.28.1
      pl.obligations[i].target = target;
      pl.obligations[i].offer = offer;
      pl.obligations[i].factionIdx = factionIdx;
      pl.obligations[i].planetIdx = planetIdx;
      pl.obligations[i].dueRound = min(state.round + OBLIGATION_ROUNDS_DUE, state.roundLimit + 1);  // v0.30.1: never past the end -- the end judges it
      negFillPlanet(text, planetIdx, pl.obligations[i].text, sizeof(pl.obligations[i].text));  // v0.30.0: favors name {planet}
      if (favorTakesWorld(check) && !strstr(pl.obligations[i].text, "Any way counts"))  // 3.1-dev.114
        strlcat(pl.obligations[i].text, FAVOR_ANY_WAY, sizeof(pl.obligations[i].text));
      return;
    }
  }
}

// Assumes mutex already held. Queues a check-in transmission for every due
// obligation not already waiting in this player's queue. Runs at each new
// round AND whenever a transmission is cleared or answered, so a check-in
// that didn't fit the queue follows as soon as there's room. (v0.23.0: the
// queue replaced the single slot, so check-ins no longer wait for -- or get
// overwritten by -- a tier-change transmission.)
static void resolveObligationLocked(int slot, int idx, bool fulfilled);  // below
static void checkObligationsDueLocked(int slot) {
  if (state.stage != STAGE_PLAYING) return;
  // 3.1-dev.20 (user): no more I DID IT / I DIDN'T -- an honor-system promise that somehow exists counts as kept.
  for (int i = 0; i < MAX_PENDING_OBLIGATIONS; i++)
    if (state.players[slot].obligations[i].active && state.players[slot].obligations[i].check == PROMISE_HONOR && state.round >= state.players[slot].obligations[i].dueRound)
      resolveObligationLocked(slot, i, true);
  return;
  Player &pl = state.players[slot];
  for (int i = 0; i < MAX_PENDING_OBLIGATIONS; i++) {
    if (!pl.obligations[i].active || state.round < pl.obligations[i].dueRound) continue;
    if (pl.obligations[i].check != PROMISE_HONOR) continue;  // v0.28.1: the game judges those itself
    bool queued = false;
    for (int q = 0; q < state.txCount[slot]; q++)
      if (state.txQueue[slot][q].line < 0 && state.txQueue[slot][q].obIdx == i) queued = true;
    if (queued || state.txCount[slot] >= TX_QUEUE_LEN) continue;
    TxMsg m;
    m.line = -1; m.faction = pl.obligations[i].factionIdx; m.planet = pl.obligations[i].planetIdx; m.obIdx = i;
    pushTxLocked(slot, m);
  }
}

// v0.30.1 VP ON CAPTURE: every change of owner goes through here. A planet's VP
// leaves with it (the old owner loses them, the new one gains them); a planet
// won with a favor (vpPending) is worth nothing until the favor is kept.
// ---- Event log (3.1-dev.3). All assume the mutex is held. ----
static int8_t g_takeHow = TAKE_OTHER;  // the caller of setOwnerLocked says how the world was taken
static void evAddLocked(GameEvent *ring, int n, uint16_t &seq, uint8_t kind, int who, int planet, int other, int num) {
  if (++seq == 0) seq = 1;  // 0 marks an empty slot
  GameEvent &e = ring[seq % n];
  e.seq = seq; e.kind = kind; e.round = (uint8_t)state.round;
  e.who = (int8_t)who; e.planet = (int8_t)planet; e.other = (int8_t)other; e.num = (int8_t)num;
}
static void evPubLocked(uint8_t kind, int who, int planet, int other, int num) {
  evAddLocked(evlog.pub, EV_PUB_N, evlog.pubSeq, kind, who, planet, other, num);
}
static void evMineLocked(int slot, uint8_t kind, int who, int planet, int other, int num) {
  if (slot < 0 || slot >= MAX_PLAYERS) return;
  evAddLocked(evlog.mine[slot], EV_MINE_N, evlog.mineSeq[slot], kind, who, planet, other, num);
}
static int classOfLocked(int slot) {
  return (slot >= 0 && slot < MAX_PLAYERS) ? characterClass(state.players[slot].commander) : -1;
}
static void stampLocked(int slot, int stamp, int planet = -1) { evMineLocked(slot, LOG_STAMP, -1, planet, -1, stamp); }
// 3.1-dev.101 NEGOTIATE, THEN FIGHT (playtest 2026-11-20: a failed talk locked the world for the round): the talk failed, so the
// world opens to a FIGHT this round -- on alert, +ALERT_DEFENDERS. No second talk or bribe there this round.
static void alertAfterTalkLocked(int slot, int p) {
  if (slot < 0 || slot >= MAX_PLAYERS || p < 0 || p >= NUM_PLANETS) return;
  state.players[slot].takeoverLocked[p] = false;
  state.players[slot].alertWorld = (int8_t)p; state.players[slot].alertRound = (int8_t)state.round;
}
static void evResetLocked() {  // a new game: empty log, new epoch
  const uint16_t ep = evlog.epoch + 1;
  memset((void *)&evlog, 0, sizeof(EventLog));
  evlog.epoch = ep ? ep : 1;
}
static void evMomentLocked() {
  evPubLocked(LOG_MOMENT, state.publicActor, state.publicPlanet, state.publicVictim, state.publicMoment);
}

static void bcCheckLocked(int breaker, int how, int planet, int victim);  // 3.1-dev.98 (with the Back Channel, below)
static void newsOnGainLocked(int slot, int p);  // below (3.1-dev.10)
static void pnewsLocked(int slot, int id, int faction, int planet, int other);
// 3.1-dev.102 THE DEAL FELL THROUGH (user: "nobody wastes a turn"): a world won by talk / bribe this round was taken by force.
// A bribe comes back; a talk pays double the world's VP in Credits (once a round) and a head start with its faction (never a Consul's).
static void inkFellThroughLocked(int p, int old) {
  Player &o = state.players[old];
  const int f = planetFaction(p);
  int cr = 0; bool head = false;
  if (state.inkHow[p] == 2) cr = state.inkCr[p];
  else {
    if (o.troubleRound != state.round) { cr = 2 * planetVP(state, p, old); o.troubleRound = (int8_t)state.round; }
    if (classOfLocked(old) != CLASS_CONSUL && f >= 0 && f < NUM_FACTIONS) { o.headStart |= (uint8_t)(1u << f); head = true; }
  }
  o.credits += cr;
  pnewsLocked(old, PN_DEAL_FELL, f, p, cr * 2 + (head ? 1 : 0));
  Serial.printf("[INK] %s fell through for P%d: +%d CR%s\n", PLANETS[p].name, old + 1, cr, head ? " + head start" : "");
}
static void setOwnerLocked(int p, int slot, bool vpPending = false) {
  if (p < 0 || p >= NUM_PLANETS) return;
  if (state.planetOwner[p] != slot) { state.treatyMask &= ~(1u << p); state.treatyUntil[p] = 0; }  // 3.1-dev.101
  const int old = state.planetOwner[p];
  if (old != slot && old >= 0 && old < MAX_PLAYERS && slot >= 0 && slot < MAX_PLAYERS && state.stage == STAGE_PLAYING && classOfLocked(old) == CLASS_SCOUNDREL) {
    state.players[slot].credits += SCOUNDREL_BOUNTY;  // 3.1-dev.78 PRICE ON YOUR HEAD
    stampLocked(slot, STAMP_BOUNTY, p);
  }
  if (old != slot && old >= 0 && old == state.markSlot && slot >= 0 && slot < MAX_PLAYERS && snapshotEra(state) == FACTION_CRIMSON_DAWN) {
    state.players[slot].credits += ERA_MARK_CR;  // 3.1-dev.88 THE MARK: the Dawn pays for a world taken from them
    stampLocked(slot, STAMP_MARK, p);
  }
  if (old != slot && slot >= 0 && slot < MAX_PLAYERS && state.stage == STAGE_PLAYING && (g_takeHow == TAKE_TALK || g_takeHow == TAKE_PAY))
    bcCheckLocked(slot, (old >= 0 && old < MAX_PLAYERS && g_takeHow == TAKE_TALK) ? BCH_UPRISE : BCH_TAKE_TALK, p, old);  // 3.1-dev.98
  if (old != slot && state.buyBy[p] && state.buyBy[p] - 1 != slot) {  // 3.1-dev.79: it changed hands first -- the buyer's money comes back
    const int b = state.buyBy[p] - 1; if (b >= 0 && b < MAX_PLAYERS) state.players[b].credits += state.buyCr[p];
    state.buyBy[p] = 0; state.buyCr[p] = 0;
  }
  if (old != slot) state.upriseFrom[p] = 0;
  if (old != slot) {  // a patrol works for whoever hired it, not the new holder
    // 3.1-dev.102 (replaces DUST SETTLING): talk / bribe = THE INK IS STILL WET (one attack allowed, against its old defenders);
    // by force = DUG IN (+DUG_IN_DEF against a second attack this round).
    if (old >= 0 && old < MAX_PLAYERS && state.inkRound[p] == state.round && state.inkBy[p] - 1 == old && g_takeHow == TAKE_FIGHT) inkFellThroughLocked(p, old);
    state.inkRound[p] = 0; state.inkBy[p] = 0; state.inkHow[p] = 0; state.inkCr[p] = 0; state.inkHit &= ~(1u << p);
    state.planetTakenRound[p] = 0;
    if (state.phase == PHASE_RESOLUTION && slot >= 0 && slot < MAX_PLAYERS) {
      if ((g_takeHow == TAKE_TALK || g_takeHow == TAKE_PAY) && !state.negUprising[slot]) {
        state.inkRound[p] = (int8_t)state.round; state.inkBy[p] = (int8_t)(slot + 1); state.inkHow[p] = g_takeHow == TAKE_PAY ? 2 : 1;
      } else state.planetTakenRound[p] = (int8_t)state.round;  // dug in
      for (int o = 0; o < MAX_PLAYERS; o++)  // anyone who had it open: beaten to it -- their turn isn't wasted
        if (o != slot && state.players[o].active && state.takeoverPlanet[o] == p && state.takeoverMethod[o] == TAKEOVER_NONE) {
          pnewsLocked(o, PN_BEATEN, planetFaction(p), p, slot);
          state.players[o].beatenWorld = (int8_t)p; state.players[o].beatenRound = (int8_t)state.round;
        }
    }
    state.patrolShips[p] = 0; state.patrolUntil[p] = 0;
  }
  // 3.1-dev.8 DEBTS: a world won on a favor counts at once; planetVpPending only marks it as held on a debt.
  if (old >= 0 && old < MAX_PLAYERS) state.players[old].vp -= planetVP(state, p, old);
  state.planetOwner[p] = slot;
  if (old != slot) state.planetHeldSince[p] = (int8_t)state.round;  // 3.1-dev.49 #18
  state.planetVpPending[p] = (slot >= 0 && vpPending) ? 1 : 0;
  if (slot >= 0 && slot < MAX_PLAYERS) state.players[slot].vp += planetVP(state, p, slot);
  if (slot >= 0 && slot < MAX_PLAYERS) {
    // 2.0: a world you hold shows you everything about it.
    for (int c = 0; c < NUM_CHARACTERISTICS; c++) state.players[slot].revealed[p][c] = true;
    // 3.1-dev.115 KNOWLEDGE GROWS OUTWARD (user 2026-11-22: scouting "shouldn't be an added step in prepare"): holding a world
    // tells you whose side its neighbours are on (one jump; the Infiltrator hears two jumps out). Home worlds count at the start.
    {
      uint32_t ring1 = snapshotNeighbors(p), known = ring1;
      if (characterClass(state.players[slot].commander) == CLASS_INFILTRATOR)
        for (int n = 0; n < NUM_PLANETS; n++) if ((ring1 >> n) & 1) known |= snapshotNeighbors(n);
      for (int n = 0; n < NUM_PLANETS; n++) if (((known >> n) & 1) && n != p) state.players[slot].revealed[n][0] = true;
    }
    // EV_UNBOUND: the first to take a droid world gets a bonus; it's an ordinary world after.
    if (state.planetUnbound[p]) { state.planetUnbound[p] = 0; state.players[slot].vp += EVENT_UNBOUND_VP; }
  }
  // 3.1-dev.3: the log. Setup (home worlds at START) passes TAKE_OTHER with no
  // previous owner and isn't news.
  if (state.stage == STAGE_PLAYING) {
    if (slot >= 0 && slot < MAX_PLAYERS) {
      if (g_takeHow != TAKE_OTHER || old >= 0) evPubLocked(LOG_TAKE, slot, p, old, g_takeHow);
      Player &tk = state.players[slot];  // 3.1-dev.7: class quests
      if (g_takeHow == TAKE_FIGHT && tk.forceTakes < 255) tk.forceTakes++;
      if ((g_takeHow == TAKE_TALK || g_takeHow == TAKE_PAY) && tk.peaceTakes < 255) tk.peaceTakes++;
      if (old >= 0 && old < MAX_PLAYERS && old != slot) evMineLocked(old, LOG_LOST, slot, p, -1, g_takeHow);
      const int8_t sellBefore = tk.sellRound == state.round ? tk.sellTo : -1;
      newsOnGainLocked(slot, p);  // 3.1-dev.10: races and jobs
      if (old < 0 && (g_takeHow == TAKE_TALK || g_takeHow == TAKE_PAY) && planetFaction(p) >= 0)  // 3.1-dev.49 #16: its rival grumbles
        applyReputationLocked(slot, rivalFactionIndex(planetFaction(p)), REP_COURTED, false, TXC_COURTED, p);
      if (old < 0 && (tk.sellRound != state.round || tk.sellTo == sellBefore) && planetFaction(p) >= 0) {  // 3.1-dev.48 SELL IT TWICE
        const int pf = planetFaction(p);
        if (g_takeHow == TAKE_TALK) sellOfferLocked(slot, rivalFactionIndex(pf), SELL_TALK, p);
        else if (g_takeHow == TAKE_PAY) sellOfferLocked(slot, rivalFactionIndex(pf), SELL_PAY, p);
        else if (g_takeHow == TAKE_FIGHT) sellOfferLocked(slot, pf, SELL_FIGHT, p);  // the victim pays you to find out who did it
      }
    } else if (old >= 0 && old < MAX_PLAYERS) {
      evPubLocked(LOG_RETURNED, old, p, -1, 0);
      evMineLocked(old, LOG_LOST, -1, p, -1, TAKE_OTHER);
    }
  }
  g_takeHow = TAKE_OTHER;
}

// Assumes mutex held. Kept or broken: reputation, VP, the faction's answer.
static void resolveObligationLocked(int slot, int idx, bool fulfilled) {
  Player &pl = state.players[slot];
  if (!pl.obligations[idx].active) return;
  const int factionIdx = pl.obligations[idx].factionIdx;
  pl.obligations[idx].active = false;
  if (factionIdx < 0 || factionIdx >= NUM_FACTIONS) return;
  evMineLocked(slot, LOG_PROMISE, factionIdx, pl.obligations[idx].planetIdx, -1, fulfilled ? 1 : 0);
  const bool wasAllied = reputationTierLocked(pl.reputation[factionIdx]) == REP_ALLIED;
  applyReputationLocked(slot, factionIdx, fulfilled ? OBLIGATION_FULFILL_REP : OBLIGATION_BREAK_REP, false,
                        fulfilled ? TXC_KEPT : TXC_BROKE, pl.obligations[idx].planetIdx,
                        (!fulfilled && wasAllied) ? (1u << TXB_OATHBREAKER) : 0);
  for (int k = PROMISE_HISTORY - 1; k > 0; k--) pl.promiseLog[k] = pl.promiseLog[k - 1];  // v0.28.3 history
  pl.promiseLog[0].faction = (int8_t)factionIdx;
  pl.promiseLog[0].planet = (int8_t)pl.obligations[idx].planetIdx;
  pl.promiseLog[0].offer = pl.obligations[idx].offer;
  pl.promiseLog[0].round = (int8_t)state.round;
  pl.promiseLog[0].kept = fulfilled ? 1 : 0;
  // v0.30.1: the planet this favor won -- kept, it starts counting; broken, they take it back.
  const int p = pl.obligations[idx].planetIdx;
  if (p >= 0 && p < NUM_PLANETS && state.planetOwner[p] == slot && state.planetVpPending[p]) {
    if (fulfilled) {
      state.planetVpPending[p] = 0;  // 3.1-dev.8: the debt is paid (its VP already counted)
    } else {
      setOwnerLocked(p, PLANET_UNCONTROLLED);
      state.planetDefense[p] = planetStartGarrison(p);
      Serial.printf("[PROMISE] P%d broke a favor -- %s goes back to its faction\n", slot + 1, PLANETS[p].name);
    }
  }
  Serial.printf("[PROMISE] P%d %s promise %d (%s)\n", slot + 1, fulfilled ? "KEPT" : "BROKE", (int)pl.obligations[idx].check,
                pl.obligations[idx].text);
}

static bool isDoPromise(uint8_t c) {
  return c == PROMISE_TAKE_FACTION || c == PROMISE_WIN_PVP || c == PROMISE_WIN_BATTLE || c == PROMISE_TAKE_CORE || c == PROMISE_HOLD_TWO;
}

// v0.28.1 game-checked promises -- the hooks (all assume the mutex is held).
// `slot` gained `planet` (fight, pay or deal).
static void promiseOnGainLocked(int slot, int planet) {
  Player &pl = state.players[slot];
  for (int i = 0; i < MAX_PENDING_OBLIGATIONS; i++) {
    const PendingObligation &ob = pl.obligations[i];
    if (!ob.active) continue;
    if (ob.check == PROMISE_TAKE_FACTION && planetFaction(planet) == ob.target) resolveObligationLocked(slot, i, true);
    else if (ob.check == PROMISE_TAKE_CORE && PLANETS[planet].ring == RING_CORE) resolveObligationLocked(slot, i, true);  // v0.30.1
    else if (ob.check == PROMISE_HOLD_TWO && ob.planetIdx >= 0 && state.planetOwner[ob.planetIdx] == slot) {  // v0.30.1
      for (int q = 0; q < NUM_PLANETS; q++)
        if (q != ob.planetIdx && state.planetOwner[q] == slot && planetFaction(q) == ob.target) { resolveObligationLocked(slot, i, true); break; }
    }
  }
}
// v0.30.1: `slot` won a battle (captured a planet in a fight, or destroyed a fleet).
static void promiseOnBattleWinLocked(int slot) {
  if (slot < 0 || slot >= MAX_PLAYERS) return;
  Player &pl = state.players[slot];
  for (int i = 0; i < MAX_PENDING_OBLIGATIONS; i++)
    if (pl.obligations[i].active && pl.obligations[i].check == PROMISE_WIN_BATTLE) resolveObligationLocked(slot, i, true);
}
// `slot` destroyed another commander's fleet.
static void promiseOnPvpWinLocked(int slot) {
  if (slot < 0 || slot >= MAX_PLAYERS) return;
  Player &pl = state.players[slot];
  for (int i = 0; i < MAX_PENDING_OBLIGATIONS; i++)
    if (pl.obligations[i].active && pl.obligations[i].check == PROMISE_WIN_PVP) resolveObligationLocked(slot, i, true);
  promiseOnBattleWinLocked(slot);
}
// 3.1-dev.113: credit a PvP win the game missed (serial "PVPWIN <slot>", for a game already running when the fix landed).
void gameStateCreditPvpWin(int slot) {
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return;
  promiseOnPvpWinLocked(slot);
  xSemaphoreGive(stateMutex);
}
// v0.30.1: the war is over -- judge every promise still open before the final
// count. Do-promises not done are broken (a favor's planet goes back); the rest
// were kept (a HOLD only if you still hold it).
static void promiseGameEndLocked(int slot) {
  Player &pl = state.players[slot];
  for (int i = 0; i < MAX_PENDING_OBLIGATIONS; i++) {
    const PendingObligation &ob = pl.obligations[i];
    if (!ob.active) continue;
    if (ob.check == PROMISE_HOLD) resolveObligationLocked(slot, i, ob.planetIdx >= 0 && state.planetOwner[ob.planetIdx] == slot);
    else resolveObligationLocked(slot, i, !isDoPromise(ob.check));
  }
}
// `slot` attacked `planet`.
static void promiseOnAttackLocked(int slot, int planet) {
  Player &pl = state.players[slot];
  for (int i = 0; i < MAX_PENDING_OBLIGATIONS; i++) {
    const PendingObligation &ob = pl.obligations[i];
    if (ob.active && ob.check == PROMISE_NO_ATTACK && planetFaction(planet) == ob.target) resolveObligationLocked(slot, i, false);
  }
}
// `slot` invested in `planet`'s defense.
static void promiseOnInvestLocked(int slot, int planet) {
  Player &pl = state.players[slot];
  for (int i = 0; i < MAX_PENDING_OBLIGATIONS; i++) {
    const PendingObligation &ob = pl.obligations[i];
    if (ob.active && ob.check == PROMISE_NO_INVEST && ob.planetIdx == planet) resolveObligationLocked(slot, i, false);
  }
}
// A new round started (after income): pay tributes, check held planets, judge deadlines.
static void promiseRoundStartLocked(int slot) {
  Player &pl = state.players[slot];
  for (int i = 0; i < MAX_PENDING_OBLIGATIONS; i++) {
    PendingObligation &ob = pl.obligations[i];
    if (!ob.active || ob.check == PROMISE_HONOR) continue;
    if (ob.check == PROMISE_TRIBUTE) {
      const int amt = ob.target > 0 ? ob.target : 1;
      if (pl.credits < amt) { resolveObligationLocked(slot, i, false); continue; }
      pl.credits -= amt;
    }
    if (ob.check == PROMISE_HOLD && ob.planetIdx >= 0 && state.planetOwner[ob.planetIdx] != slot) {
      resolveObligationLocked(slot, i, false);
      continue;
    }
    if (state.round >= ob.dueRound)  // deadline: do-promises not done are broken, the rest kept
      resolveObligationLocked(slot, i, !isDoPromise(ob.check));
  }
}

void gameStateResolveObligation(int slot, bool fulfilled) {
  if (g_remote) { remoteCall(RF_RESOLVE_OBLIGATION, {slot, fulfilled ? 1 : 0}); return; }
  if (slot < 0 || slot >= MAX_PLAYERS) return;
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return;
  const bool headIsCheckIn = state.txCount[slot] > 0 && state.txQueue[slot][0].line < 0;
  const int idx = headIsCheckIn ? state.txQueue[slot][0].obIdx : -1;
  if (headIsCheckIn) popTxLocked(slot);  // answered -- the reaction below queues behind anything already waiting
  if (idx >= 0 && idx < MAX_PENDING_OBLIGATIONS) resolveObligationLocked(slot, idx, fulfilled);
  checkObligationsDueLocked(slot);  // next due check-in, if any
  xSemaphoreGive(stateMutex);
}

// ---------------------------------------------------------------------------
static int rollDie() { return 1 + (esp_random() % 6); }

static int firstAliveIndex(bool *alive) {  // battle arrays (BATTLE_SHIPS, v0.28.0)
  for (int i = 0; i < BATTLE_SHIPS; i++) if (alive[i]) return i;
  return -1;
}
static bool anyAlive(bool *alive) { return firstAliveIndex(alive) != -1; }
// 3.1-dev.45 #10 LAST SHIP KEEPS A FLEET (user 2026-11-11): every loss comes off the fleet with the MOST ships left (ties:
// the lower fleet), so a fleet only drops to 0 once every fleet in the fight is down to its last ship. Both sides.
static int lossIndex(bool *alive) {
  int best = -1, bestN = 0;
  for (int f = 0; f < NUM_FLEETS; f++) {
    int n = 0;
    for (int s = 0; s < FLEET_SHIPS; s++) if (alive[f * FLEET_SHIPS + s]) n++;
    if (n > bestN) { bestN = n; best = f; }
  }
  if (best < 0) return -1;
  for (int s = 0; s < FLEET_SHIPS; s++) if (alive[best * FLEET_SHIPS + s]) return best * FLEET_SHIPS + s;
  return -1;
}
static int aliveCount(const bool *alive) { int n = 0; for (int i = 0; i < BATTLE_SHIPS; i++) if (alive[i]) n++; return n; }

// Reset all of a player's fleets to their starting size (FLEET_START_SHIPS
// commissioned and alive; the rest not-yet-built).
static void initPlayerFleets(Player &pl) {
  // Only Fleet 1 starts populated, per the fleet rework -- Fleets 2 and 3
  // start completely empty (0 ships commissioned) and get "founded" the
  // first time a player invests in them, which requires owning a planet
  // (see gameStateInvestFleet()) since a new fleet gets placed there.
  pl.fleetShipCount[0] = FLEET_START_SHIPS;
  for (int s = 0; s < FLEET_SHIPS; s++) pl.fleetShipAlive[0][s] = (s < FLEET_START_SHIPS);
  for (int f = 1; f < NUM_FLEETS; f++) {
    pl.fleetShipCount[f] = 0;
    for (int s = 0; s < FLEET_SHIPS; s++) pl.fleetShipAlive[f][s] = false;
  }
}

static int fleetStrengthLocked(int slot, int fleetIdx) {
  if (slot < 0 || slot >= MAX_PLAYERS) return 0;
  if (fleetIdx < 0 || fleetIdx >= NUM_FLEETS) return 0;
  int n = 0;
  for (int s = 0; s < FLEET_SHIPS; s++)
    if (state.players[slot].fleetShipAlive[fleetIdx][s]) n++;
  return n;
}

// First fleet (by index) that currently has at least one living ship.
// Falls back to fleet 0 if none do (caller should still check strength > 0).
// Only used to pick an attacker, so it also skips fleets already spent this
// round (v0.20.2).
static int firstAvailableFleetLocked(int slot) {
  for (int f = 0; f < NUM_FLEETS; f++)
    if (fleetStrengthLocked(slot, f) > 0 && !state.players[slot].fleetSpent[f]) return f;
  return 0;
}

// Clears the per-round commitment flags (fleetSpent, takeoverLocked) for
// every player. Called when a new round starts and when a game starts.
static void resetRoundCommitmentsLocked() {
  for (int i = 0; i < MAX_PLAYERS; i++) {
    for (int f = 0; f < NUM_FLEETS; f++) state.players[i].fleetSpent[f] = false;
    for (int p = 0; p < NUM_PLANETS; p++) state.players[i].takeoverLocked[p] = false;
  }
}

static int activePlayerCount() {
  int n = 0;
  for (int i = 0; i < MAX_PLAYERS; i++) if (state.players[i].active) n++;
  return n;
}

static void resetGalaxyLocked() {
  loadClassicGalaxyLocked();  // v0.25.4: the classic layout until a game starts (then it's shuffled)
  for (int p = 0; p < NUM_PLANETS; p++) {
    state.planetDefense[p] = planetStartGarrison(p);
    // Bug fix: the struct's default initializer only explicitly set the
    // first 2 elements of planetOwner[NUM_PLANETS] (a leftover from when
    // NUM_PLANETS was 2, before the galaxy expanded to 12). C++ zero-
    // initializes the rest of an under-sized initializer list, and 0 is
    // ROLE/SLOT 0 -- not PLANET_UNCONTROLLED (-1) -- so planets 2-11 were
    // defaulting to "owned by whoever is in slot 0" from the moment any
    // game started. Explicit reset here fixes it for all 12.
    state.planetOwner[p] = PLANET_UNCONTROLLED;
    // Every planet is in play: the printed board has a slot for each, so the
    // Lobby planet picker was removed in v0.21.19. planetIncluded stays (all
    // true) for a possible player-count rule later.
    state.planetIncluded[p] = true;
  }
}

// ===========================================================================
// SHADOW FLEET 2.0 RULES (constants and the why: game_state.h, "SHADOW FLEET 2.0")
// ===========================================================================
static void noteLocked(int slot, const char *fmt, ...) {
  if (slot < 0 || slot >= MAX_PLAYERS) return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(state.players[slot].note, sizeof(state.players[slot].note), fmt, ap);
  va_end(ap);
  state.players[slot].noteSeq++;
  const uint8_t k = evlog.noteNext[slot]++ % EV_NOTE_N;  // 3.1-dev.3: into the Inbox too
  strncpy(evlog.notes[slot][k], state.players[slot].note, EV_NOTE_LEN - 1);
  evlog.notes[slot][k][EV_NOTE_LEN - 1] = '\0';
  evMineLocked(slot, LOG_NOTE, -1, -1, -1, k);
}

static const char *factionName(int f) { return (f >= 0 && f < NUM_FACTIONS) ? AFFILIATION_NAMES[f] : "a rival"; }

int snapshotHolderTrait(const GameState &gs, int slot, int trait) {
  int n = 0;
  for (int p = 0; p < NUM_PLANETS; p++) if (gs.planetOwner[p] == slot && gs.planetTrait[p] == trait) n++;
  return n;
}

int snapshotRevealCost(const GameState &gs, int slot, int p, int c) {
  if (c < 0 || c >= NUM_CHARACTERISTICS) return 0;
  if (gs.phase == PHASE_RESOLUTION) return REVEAL_COST[c] * INFORMANT_COST_MULT;  // 2.0-dev.27: the informant
  if (slot >= 0 && slot < MAX_PLAYERS && (snapshotHolderTrait(gs, slot, TR_SPYNET) > 0 || characterClass(gs.players[slot].commander) == CLASS_INFILTRATOR)
      && gs.players[slot].freeRevealRound != gs.round) return 0;  // TR_SPYNET / 3.1 INFILTRATOR: the first reveal each round
  return REVEAL_COST[c];
}

bool snapshotMarketIsOpen(const GameState &gs, int p) {
  for (int i = 0; i < MARKETS_OPEN; i++) if (gs.marketOpen[i] == p) return true;
  return false;
}

int snapshotMarketPrice(const GameState &gs, int slot, int p) {
  if (p < 0 || p >= NUM_PLANETS) return 0;
  int price = goodBasePrice(gs.planetGood[p]);
  // 3.1-dev.8 (user 2026-11-06: "the doors open and close for certain factions"): market prices never
  // move with favor -- favor only opens and shuts the stall (snapshotStallShut).
  return price < 1 ? 1 : price;
}

static bool fleetCanRepair(const Player &pl, int f) {
  bool alive = false, dead = false;
  for (int s = 0; s < pl.fleetShipCount[f] && s < FLEET_SHIPS; s++) { if (pl.fleetShipAlive[f][s]) alive = true; else dead = true; }
  return alive && dead;  // on the board, with a lost ship
}
static int fleetCapacityOf(const Player &pl) {  // 2.0-dev.20: the Empire's fleets hold 7
  return pl.chosenFaction == FACTION_EMPIRE ? FLEET_SHIPS_EMPIRE : FLEET_SHIPS_NORMAL;
}
int snapshotFleetCapacity(const GameState &gs, int slot) {
  return slot >= 0 && slot < MAX_PLAYERS ? fleetCapacityOf(gs.players[slot]) : FLEET_SHIPS_NORMAL;
}
static bool fleetCanGrow(const Player &pl, int f) {
  bool alive = false;
  for (int s = 0; s < FLEET_SHIPS; s++) if (pl.fleetShipAlive[f][s]) alive = true;
  return alive && pl.fleetShipCount[f] < fleetCapacityOf(pl);
}
static int weakestHeldWorld(const GameState &gs, int slot) {
  int best = -1;
  for (int p = 0; p < NUM_PLANETS; p++)
    if (gs.planetOwner[p] == slot && gs.planetDefense[p] < snapshotDefenseLimit(gs, p) && (best < 0 || gs.planetDefense[p] < gs.planetDefense[best])) best = p;
  return best;
}

// 3.1-dev.9: can `slot` use `good` right now? target = a world (SURVEY) or a faction (AUDIENCE); -1 = any.
static bool goodUsableFor(const GameState &gs, int slot, int good, int target) {
  const Player &pl = gs.players[slot];
  if (good == GOOD_SURVEY) {
    for (int p = 0; p < NUM_PLANETS; p++) {
      if (target >= 0 && p != target) continue;
      if (gs.planetOwner[p] == slot) continue;
      for (int c = 0; c < NUM_CHARACTERISTICS; c++) if (!pl.revealed[p][c]) return true;
    }
    return false;
  }
  if (good == GOOD_AUDIENCE) return target < NUM_FACTIONS && (target < 0 || reputationTierLocked(pl.reputation[target]) < REP_ALLIED || pl.reputation[target] < REP_MAX);
  switch (good) {
    case GOOD_REPAIRS: case GOOD_BACTA:
      for (int f = 0; f < NUM_FLEETS; f++) if (fleetCanRepair(pl, f)) return true;
      return false;
    case GOOD_SMUGGLED:
      for (int f = 0; f < NUM_FLEETS; f++) if (fleetCanGrow(pl, f)) return true;
      return false;
    case GOOD_TROOPS: return weakestHeldWorld(gs, slot) >= 0;
    case GOOD_HARBOR: return pl.harborRound != gs.round;
    default: return true;
  }
}
static bool goodUsable(const GameState &gs, int slot, int p) {  // the 2.0 world markets (closed since 3.1-dev.9)
  return goodUsableFor(gs, slot, gs.planetGood[p], gs.planetGood[p] == GOOD_AUDIENCE ? gs.planetFactionOf[p] : p);
}

const uint8_t FACTION_GOODS[4][3] = {
  { GOOD_BACTA, GOOD_SMUGGLED, GOOD_HARBOR },          // Rebels: people
  { GOOD_REPAIRS, GOOD_TROOPS, GOOD_SURVEY },          // Empire: order and information
  { GOOD_FUEL, GOOD_CARGO, GOOD_PERMITS },             // Pykes: money
  { GOOD_MERCENARIES, GOOD_BLESSING, GOOD_AUDIENCE },  // Crimson Dawn: blades and secrets
};
bool snapshotStallShut(const GameState &gs, int slot, int f) {
  if (f < 0 || f >= NUM_FACTIONS) return false;
  if (gs.embargoFaction == f && gs.embargoRound == gs.round) return true;  // 3.1-dev.10 EMBARGO: shut to everyone
  if (slot < 0 || slot >= MAX_PLAYERS) return false;
  return reputationTierLocked(gs.players[slot].reputation[f]) <= STALL_SHUT_MAX_TIER;
}
int snapshotStallPrice(const GameState &gs, int slot, int f, int k) {
  if (f < 0 || f >= NUM_FACTIONS || k < 0 || k >= STALL_ITEMS || gs.stallGood[f][k] < 0) return 0;
  int price = goodBasePrice(gs.stallGood[f][k]);
  if (slot >= 0 && slot < MAX_PLAYERS) {
    static const int8_t ADJ[5] = STALL_TIER_ADJ;  // user 2026-11-07: "the price can fluctuate with your standing"
    price += ADJ[reputationTierLocked(gs.players[slot].reputation[f])];
  }
  return price < 1 ? 1 : price;
}
// 3.1-dev.114 (playtest: "I can still buy out a shop"): buys at faction f's stall this round. shopMask: bit f = one, bit f+4 = two.
int snapshotStallBought(const GameState &gs, int slot, int f) {
  if (slot < 0 || slot >= MAX_PLAYERS || f < 0 || f >= NUM_FACTIONS) return 0;
  const Player &pl = gs.players[slot];
  if (pl.shopRound != gs.round) return 0;
  return ((pl.shopMask >> f) & 1) + ((pl.shopMask >> (f + 4)) & 1);
}
int snapshotStallLimit(const GameState &gs) { return (gs.eraFaction == FACTION_PYKE && gs.round <= gs.eraUntil) ? 2 : 1; }
bool snapshotStallCanBuy(const GameState &gs, int slot, int f, int k) {
  if (slot < 0 || slot >= MAX_PLAYERS || f < 0 || f >= NUM_FACTIONS || k < 0 || k >= STALL_ITEMS) return false;
  if (gs.stage != STAGE_PLAYING || gs.phase != PHASE_INVEST || !gs.players[slot].active) return false;
  if (gs.stallGood[f][k] < 0 || gs.stallStock[f][k] == 0 || snapshotStallShut(gs, slot, f)) return false;
  if (gs.players[slot].credits < snapshotStallPrice(gs, slot, f, k)) return false;
  if (snapshotStallBought(gs, slot, f) >= snapshotStallLimit(gs)) return false;  // 3.1-dev.114 one buy per shop a round (Spice Age: two)
  return goodUsableFor(gs, slot, gs.stallGood[f][k], -1);
}


bool snapshotCanBuy(const GameState &gs, int slot, int p) {
  if (slot < 0 || slot >= MAX_PLAYERS || p < 0 || p >= NUM_PLANETS) return false;
  if (gs.stage != STAGE_PLAYING || gs.phase != PHASE_INVEST || !gs.players[slot].active) return false;
  if (!snapshotMarketIsOpen(gs, p) || gs.marketStock[p] == 0) return false;
  if (gs.players[slot].credits < snapshotMarketPrice(gs, slot, p)) return false;
  { const int f = gs.planetFactionOf[p];  // 3.1-dev.101 (user: "one item per shop per turn"): one buy at each faction's shop a round
    if (f >= 0 && gs.players[slot].shopRound == gs.round && ((gs.players[slot].shopMask >> f) & 1)) return false; }
  return goodUsable(gs, slot, p);
}

bool snapshotCanSendDiplomat(const GameState &gs, int slot, int p) {
  if (!DIPLOMAT_ON) return false;  // 3.1-dev.17: gone -- the informant replaced it
  if (slot < 0 || slot >= MAX_PLAYERS || p < 0 || p >= NUM_PLANETS) return false;
  const Player &pl = gs.players[slot];
  if (gs.stage != STAGE_PLAYING || gs.phase != PHASE_INVEST || !pl.active) return false;
  if (pl.diplomatPlanet >= 0 || pl.credits < DIPLOMAT_COST) return false;
  for (int c = 0; c < NUM_CHARACTERISTICS; c++) if (!pl.revealed[p][c]) return true;
  return false;  // nothing left to learn there
}

// 3.1-dev.7: the quest is your CLASS's (was your faction's).
int snapshotQuestProgress(const GameState &gs, int slot, int *goal) {
  int g = 1, n = 0;
  if (slot >= 0 && slot < MAX_PLAYERS) {
    const Player &pl = gs.players[slot];
    switch (characterClass(pl.commander)) {
      case CLASS_WARLORD: g = CLASS_QUEST_FORCE; n = pl.forceTakes; break;
      case CLASS_CONSUL: g = CLASS_QUEST_PEACE; n = pl.peaceTakes; break;
      case CLASS_INFILTRATOR:
        g = CLASS_QUEST_SPY;
        for (int p = 0; p < NUM_PLANETS; p++) if (pl.revealed[p][0]) n++;
        break;
      case CLASS_SCOUNDREL: g = CLASS_QUEST_FORTUNE; n = max((int)pl.peakCredits, pl.credits); break;
      default: break;
    }
  }
  if (goal) *goal = g;
  return n > g ? g : n;
}

bool snapshotQuestDone(const GameState &gs, int slot) {
  int goal = 1;
  const int n = snapshotQuestProgress(gs, slot, &goal);
  return n >= goal;
}

static int repairShipsLocked(int slot, int n) {
  Player &pl = state.players[slot];
  int done = 0;
  for (int f = 0; f < NUM_FLEETS && done < n; f++) {
    if (!fleetCanRepair(pl, f)) continue;
    for (int s = 0; s < pl.fleetShipCount[f] && s < FLEET_SHIPS && done < n; s++)
      if (!pl.fleetShipAlive[f][s]) { pl.fleetShipAlive[f][s] = true; done++; }
  }
  return done;
}
static bool addShipLocked(int slot) {
  Player &pl = state.players[slot];
  for (int f = 0; f < NUM_FLEETS; f++) {
    if (!fleetCanGrow(pl, f)) continue;
    pl.fleetShipAlive[f][pl.fleetShipCount[f]] = true;
    pl.fleetShipCount[f]++;
    return true;
  }
  return false;
}

// Each Prepare: MARKETS_OPEN worlds open their markets, drawn at random, with MARKET_STOCK each.
static void openMarketsLocked() {
  for (int p = 0; p < NUM_PLANETS; p++) state.marketStock[p] = 0;
  // 3.1-dev.9: the world markets stay closed; each faction's stall puts out 2 of its 3 goods.
  for (int i = 0; i < MARKETS_OPEN; i++) state.marketOpen[i] = -1;
  int8_t prev[NUM_FACTIONS];
  for (int f = 0; f < NUM_FACTIONS; f++) prev[f] = state.stallGood[f][0];  // last round's good: not again this round
  for (int f = 0; f < NUM_FACTIONS; f++)
    for (int k = 0; k < STALL_ITEMS; k++) { state.stallGood[f][k] = -1; state.stallStock[f][k] = 0; }
  if (!state.marketsOn || (state.eventId == EV_ASHEN && state.ashenHp > 0)) return;  // switched off / the blockade
  for (int f = 0; f < NUM_FACTIONS; f++) {
    int g[3] = { FACTION_GOODS[f][0], FACTION_GOODS[f][1], FACTION_GOODS[f][2] };
    shuffleInts(g, 3);
    if (g[0] == prev[f]) { const int t = g[0]; g[0] = g[2]; g[2] = t; }  // it cycles: never the same good two rounds running
    // 3.1-dev.17: ONE good per faction a round (SPICE FLOWS: two)
    const int out = (state.eraFaction == FACTION_PYKE && state.round <= state.eraUntil) ? 2 : 1;
    for (int k = 0; k < out; k++) { state.stallGood[f][k] = (int8_t)g[k]; state.stallStock[f][k] = MARKET_STOCK; }
  }
  return;
  if (!state.marketsOn || (state.eventId == EV_ASHEN && state.ashenHp > 0)) {  // dev.32 switched off / the blockade: every market closed
    for (int i = 0; i < MARKETS_OPEN; i++) state.marketOpen[i] = -1;
    return;
  }
  int all[NUM_PLANETS];
  for (int p = 0; p < NUM_PLANETS; p++) all[p] = p;
  shuffleInts(all, NUM_PLANETS);
  for (int i = 0; i < MARKETS_OPEN; i++) {
    state.marketOpen[i] = (int8_t)all[i];
    state.marketStock[all[i]] = MARKET_STOCK;
  }
}

static int vpLeaderLocked() {  // the one player with the most VP (a tie = nobody)
  int best = -1, bestVp = -1000;
  bool tie = false;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (!state.players[i].active) continue;
    if (state.players[i].vp > bestVp) { best = i; bestVp = state.players[i].vp; tie = false; }
    else if (state.players[i].vp == bestVp) tie = true;
  }
  return tie ? -1 : best;
}

// Galaxy event at the halfway point (one per game, drawn at random).
static void maybeTriggerEventLocked() {
  if (!state.eventsOn) return;  // 2.0-dev.33: switched off in the Lobby
  if (state.eventId != EV_NONE || state.round != state.roundLimit / 2 + 1) return;
  const int e = esp_random() % EV_COUNT;
  state.eventId = (int8_t)e;
  state.eventRound = (int8_t)state.round;
  if (e == EV_UNBOUND) {
    int free[NUM_PLANETS], n = 0;
    for (int p = 0; p < NUM_PLANETS; p++) if (state.planetOwner[p] == PLANET_UNCONTROLLED) free[n++] = p;
    shuffleInts(free, n);
    for (int i = 0; i < n && i < EVENT_UNBOUND_WORLDS; i++) {
      state.planetUnbound[free[i]] = 1;
      state.planetDefense[free[i]] = min(snapshotDefenseLimit(state, free[i]), state.planetDefense[free[i]] + EVENT_GARRISON_BONUS);
    }
  } else if (e == EV_ASHEN) {
    int players = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) if (state.players[i].active) players++;
    state.ashenHp = state.ashenMaxHp = (int16_t)(ASHEN_HP_PER_PLAYER * max(1, players));
    for (int i = 0; i < MAX_PLAYERS; i++) state.ashenDmg[i] = 0;
    openMarketsLocked();  // this round's markets close at once
  } else if (e == EV_CRACKDOWN) {
    for (int p = 0; p < NUM_PLANETS; p++)
      if (state.planetOwner[p] == PLANET_UNCONTROLLED && factionIndexForPlanet(p) == FACTION_EMPIRE)
        state.planetDefense[p] = min(snapshotDefenseLimit(state, p), state.planetDefense[p] + EVENT_GARRISON_BONUS);
  }
  state.publicMoment = (int8_t)(PUBLIC_EVENT_BASE + e);  // the table + every player's "look at the table"
  state.publicActor = state.publicVictim = state.publicPlanet = -1;
  state.publicSeq++; evMomentLocked();
  for (int i = 0; i < MAX_PLAYERS; i++)
    if (state.players[i].active) noteLocked(i, "GALAXY EVENT: %s. %s", eventName(e), eventText(e));
  Serial.printf("[EVENT] round %d: %s\n", state.round, eventName(e));
}

// The start of every round's Prepare (after income): traits, goods, the diplomat, markets, events.
static void dealsRoundStartLocked();                    // 2.0 deals (defined with the deal rules below)
static int dealCutReceiverLocked(int planetIdx, int holder);

static void newsRoundStartLocked(bool tableBusy);  // below
static void roundStartPerksLocked() {
  for (int i = 0; i < MAX_PLAYERS; i++) {
    Player &pl = state.players[i];
    if (!pl.active) continue;
    if (pl.cargoDue) { pl.credits += pl.cargoDue; pl.lastIncome += pl.cargoDue; pl.cargoDue = 0; }
    for (int p = 0; p < NUM_PLANETS; p++) {
      if (state.planetOwner[p] != i) continue;
      if (state.planetTrait[p] == TR_COURT) {
        applyReputationLocked(i, factionIndexForPlanet(p), TRAIT_COURT_STANDING, false, TXC_DEAL, p);
        stampLocked(i, STAMP_COURT, p);  // 3.1-dev.19
      }
      if (state.planetTrait[p] == TR_MEDICAL && repairShipsLocked(i, 1) > 0) stampLocked(i, STAMP_MEDICAL, p);
    }
    if (pl.diplomatPlanet >= 0 && state.round >= pl.diplomatBack) {
      const int p = pl.diplomatPlanet;
      for (int c = 0; c < NUM_CHARACTERISTICS; c++) pl.revealed[p][c] = true;
      pl.diplomatPlanet = -1;
      pl.diplomatBack = -1;
      noteLocked(i, "Your diplomat returns from %s: all four secrets revealed.", PLANETS[p].name);
    }
  }
  dealsRoundStartLocked();
  openMarketsLocked();
  const uint16_t pubBefore = state.publicSeq;
  maybeTriggerEventLocked();
  newsRoundStartLocked(state.publicSeq != pubBefore);  // 3.1-dev.10 Galaxy News (the table keeps the event if one fired)
}

// The end of the game: quests and the two market awards, before the final count.
static void questsAndAwardsLocked() {
  for (int i = 0; i < MAX_PLAYERS; i++) {
    Player &pl = state.players[i];
    if (!pl.active) continue;
    if (!pl.questDone && snapshotQuestDone(state, i)) { pl.questDone = 1; pl.vp += QUEST_VP; evPubLocked(LOG_QUEST, i, -1, -1, 0); }
  }
}

// Battle hooks: who gets what for a battle starting now.
static int playerBattleRerolls(int slot) {
  if (slot < 0 || slot >= MAX_PLAYERS) return 0;
  Player &pl = state.players[slot];
  int n = (snapshotHolderTrait(state, slot, TR_ANCIENT) > 0 ? 1 : 0) + pl.nextBattleReroll;
  pl.nextBattleReroll = 0;
  return n;
}
static uint8_t playerBattleMercs(int slot) {  // GOOD_MERCENARIES, used up by this battle (2.0-dev.26: ships)
  if (slot < 0 || slot >= MAX_PLAYERS) return 0;
  const uint8_t m = state.players[slot].nextBattleMercs;
  state.players[slot].nextBattleMercs = 0;
  return m;
}
// 2.0-dev.24/26: a side about to lose a ship loses a hired ship first, then
// (attacker only) the escort. True when one of them took it (lastLossIdx set).
// 3.1-dev.7 class extra ships for a battle starting now (stamped for whoever they touch).
int snapshotSafeWorld(const GameState &gs, int slot) {
  if (slot < 0 || slot >= MAX_PLAYERS || reputationTierLocked(gs.players[slot].reputation[FACTION_REBEL]) != REP_ALLIED) return -1;
  // User 2026-11-07: the safe world is your HOME world; lose it and your most valuable world takes its place.
  for (int p = 0; p < NUM_PLANETS; p++) if (gs.planetOwner[p] == slot && planetHomeOf(p) == gs.players[slot].chosenFaction) return p;
  int best = -1, bestVp = -1;
  for (int p = 0; p < NUM_PLANETS; p++)
    if (gs.planetOwner[p] == slot && planetVP(gs, p, slot) > bestVp) { best = p; bestVp = planetVP(gs, p, slot); }
  return best;
}
static void classBattleExtrasLocked(int attacker, int worldOwner, int pvpDefender = -1) {
  state.battleAtkExtra = state.battleDefExtra = 0;
  // 3.1-dev.15 SHIP QUIRKS (by your faction's ship style; agreed 2026-11-06): Pyke ARMORED CARGO (the first hit
  // on your fleet each battle is shrugged off), Rebel SLIPPERY (lose the battle and one ship escapes), Crimson
  // Dawn AMBUSHERS (they fire first: an opening-exchange tie is their hit). Imperial RANKS = fleets of 7.
  auto quirks = [](int slot, uint8_t shrug, uint8_t esc, uint8_t first) -> uint8_t {
    if (slot < 0 || slot >= MAX_PLAYERS) return 0;
    const int f = state.players[slot].chosenFaction;
    return f == FACTION_PYKE ? shrug : f == FACTION_REBEL ? esc : f == FACTION_CRIMSON_DAWN ? first : 0;
  };
  state.battleQuirk = quirks(attacker, 1, 4, 16) | quirks(pvpDefender, 2, 8, 32);
  const int cls = classOfLocked(attacker);
  if (cls == CLASS_WARLORD) { state.battleAtkExtra += CLASS_WARLORD_SHIPS; stampLocked(attacker, STAMP_WARLORD); }
  // 3.1-dev.101: the Consul's SOFT HANDS is gone (user: "the attack weakness is terrible"). A failed talk = they're on alert.
  if (pvpDefender < 0 && state.players[attacker].alertRound == state.round && state.players[attacker].alertWorld == state.battleTargetPlanet) {
    state.battleDefExtra += ALERT_DEFENDERS; stampLocked(attacker, STAMP_ALERT, state.battleTargetPlanet);
  }
  if (worldOwner >= 0 && worldOwner < MAX_PLAYERS && worldOwner != attacker && classOfLocked(worldOwner) == CLASS_INFILTRATOR) {
    state.battleAtkExtra += 1; stampLocked(worldOwner, STAMP_THIN, state.battleTargetPlanet);
  }
  // 3.1-dev.8 tier-5 gifts that fight: the Empire's ship of the line, the Rebels' safe world.
  if (tierOfLocked(attacker, FACTION_EMPIRE) == REP_ALLIED) { state.battleAtkExtra += GIFT_LINE_SHIPS; stampLocked(attacker, STAMP_LINE); }
  const int def = pvpDefender >= 0 ? pvpDefender : worldOwner;
  if (def >= 0 && def < MAX_PLAYERS && def != attacker) {
    if (tierOfLocked(def, FACTION_EMPIRE) == REP_ALLIED) { state.battleDefExtra += GIFT_LINE_SHIPS; stampLocked(def, STAMP_LINE); }
    if (pvpDefender < 0 && snapshotSafeWorld(state, def) == state.battleTargetPlanet) {
      state.battleDefExtra += GIFT_SAFE_SHIPS; stampLocked(def, STAMP_SAFE, state.battleTargetPlanet);
    }
  }
}
static bool coverLossLocked(bool attackerSide) {
  uint8_t &mercs = attackerSide ? state.battleAtkMercs : state.battleDefMercs;
  uint8_t &extra = attackerSide ? state.battleAtkExtra : state.battleDefExtra;  // 3.1-dev.7: drawn with the hired ships, on top
  if (extra) { extra--; state.lastLossIdx = MERC_LOSS_IDX + mercs + extra; return true; }
  if (mercs) { mercs--; state.lastLossIdx = MERC_LOSS_IDX + mercs; return true; }
  if (attackerSide && state.battleEscortAlive) { state.battleEscortAlive--; state.lastLossIdx = ESCORT_LOSS_IDX; return true; }
  return false;
}
// Rerolls (TR_ANCIENT / GOOD_BLESSING): a side that lost the exchange rolls again once.
static void applyRerollsLocked(int &atkDie, int &defDie) {
  if (atkDie + state.battleAtkMod < defDie + state.battleDefMod && state.atkRerolls) { const int was = atkDie; state.atkRerolls--; atkDie = 1 + (esp_random() % 6); evMineLocked(state.battleAttackerSlot, LOG_STAMP, -1, -1, was * 10 + atkDie, STAMP_REROLL); }  // 3.1-dev.39: the stamp carries both rolls
  if (defDie + state.battleDefMod < atkDie + state.battleAtkMod && state.defRerolls) { const int was = defDie; state.defRerolls--; defDie = 1 + (esp_random() % 6); evMineLocked(state.battleDefenderSlot, LOG_STAMP, -1, -1, was * 10 + defDie, STAMP_REROLL); }
}
// A battle win that counts for the Rebels' quest (vs the Empire player or an Imperial-sympathetic world).
static void rebelWinLocked(int winner, int loserSlot, int planet) {
  if (winner < 0 || winner >= MAX_PLAYERS || state.players[winner].chosenFaction != FACTION_REBEL) return;
  const bool vsEmpire = (loserSlot >= 0 && loserSlot < MAX_PLAYERS && state.players[loserSlot].chosenFaction == FACTION_EMPIRE)
                     || (planet >= 0 && factionIndexForPlanet(planet) == FACTION_EMPIRE);
  if (vsEmpire && state.players[winner].rebelWins < 255) state.players[winner].rebelWins++;
}
// EV_BOUNTY: destroying a fleet of the VP leader.
static void bountyLocked(int winner, int loser) {
  if (state.eventId != EV_BOUNTY || winner < 0 || winner >= MAX_PLAYERS) return;
  if (loser >= 0 && loser == vpLeaderLocked()) {
    state.players[winner].vp += EVENT_BOUNTY_VP;
    noteLocked(winner, "Bounty collected: +%d VP for a fleet of the leader.", EVENT_BOUNTY_VP);
  }
}

bool gameStateMarketBuy(int slot, int p) {
  if (g_remote) return remoteCall(RF_MARKET_BUY, {slot, p}).value == 1;
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return false;
  bool ok = false;
  if (snapshotCanBuy(state, slot, p)) {
    Player &pl = state.players[slot];
    const int price = snapshotMarketPrice(state, slot, p);
    pl.credits -= price;
    state.marketStock[p]--;
    if (pl.shopRound != state.round) { pl.shopRound = (int8_t)state.round; pl.shopMask = 0; }  // 3.1-dev.101
    { const int f = factionIndexForPlanet(p); if (f >= 0) pl.shopMask |= (uint8_t)(1u << f); }
    // 3.1-dev.9: no cut for the world's holder any more (user 2026-11-07).
    for (int q = 0; q < NUM_PLANETS; q++)  // TR_BLACKMARKET
      if (state.planetTrait[q] == TR_BLACKMARKET && state.planetOwner[q] >= 0 && state.planetOwner[q] < MAX_PLAYERS)
        state.players[state.planetOwner[q]].credits += 1;
    switch (state.planetGood[p]) {
      case GOOD_FUEL: noteLocked(slot, "Fuel from %s: one of your fleets may move 2 steps this round.", PLANETS[p].name); break;
      case GOOD_REPAIRS: repairShipsLocked(slot, 2); break;
      case GOOD_SURVEY: for (int c = 0; c < NUM_CHARACTERISTICS; c++) pl.revealed[p][c] = true; break;
      case GOOD_BLESSING: pl.nextBattleReroll++; break;
      case GOOD_MERCENARIES: pl.nextBattleMercs = (uint8_t)min(MERC_SHIPS_MAX, pl.nextBattleMercs + MERC_SHIPS); break;
      case GOOD_SMUGGLED: addShipLocked(slot); break;
      case GOOD_AUDIENCE: applyReputationLocked(slot, factionIndexForPlanet(p), GOOD_AUDIENCE_STANDING, false, TXC_DEAL, p); break;
      case GOOD_CARGO: pl.cargoDue += GOOD_CARGO_RETURN; break;
      case GOOD_HARBOR: pl.harborRound = (int8_t)state.round; break;
      case GOOD_BACTA: repairShipsLocked(slot, 1); break;
      case GOOD_PERMITS: pl.permitDiscount += GOOD_PERMITS_DISCOUNT; break;
      case GOOD_TROOPS: { const int w = weakestHeldWorld(state, slot); if (w >= 0) state.planetDefense[w]++; break; }
      default: break;
    }
    ok = true;
  }
  xSemaphoreGive(stateMutex);
  return ok;
}

static void newsPubLocked(int id, int faction, int planet, int other);  // (below)
static void newsTableLocked(int id, int faction, int planet, int victimFaction);
bool gameStateStallBuy(int slot, int f, int k, int target) {
  if (g_remote) return remoteCall(RF_STALL_BUY, {slot, f, k, target}).value == 1;
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return false;
  bool ok = false;
  if (snapshotStallCanBuy(state, slot, f, k)) {
    const int good = state.stallGood[f][k];
    const bool needsWorld = good == GOOD_SURVEY, needsFaction = good == GOOD_AUDIENCE;
    const bool targetOk = needsWorld ? (target >= 0 && target < NUM_PLANETS) : needsFaction ? (target >= 0 && target < NUM_FACTIONS) : true;
    if (targetOk && goodUsableFor(state, slot, good, (needsWorld || needsFaction) ? target : -1)) {
      Player &pl = state.players[slot];
      pl.credits -= snapshotStallPrice(state, slot, f, k);
      state.stallStock[f][k]--;
      { const int had = snapshotStallBought(state, slot, f);  // 3.1-dev.114
        if (pl.shopRound != state.round) { pl.shopRound = (int8_t)state.round; pl.shopMask = 0; }
        pl.shopMask |= (uint8_t)(1u << (had ? f + 4 : f)); }
      for (int q = 0; q < NUM_PLANETS; q++)  // TR_BLACKMARKET (a world trait, not the old market cut)
        if (state.planetTrait[q] == TR_BLACKMARKET && state.planetOwner[q] >= 0 && state.planetOwner[q] < MAX_PLAYERS)
          state.players[state.planetOwner[q]].credits += 1;
      switch (good) {
        case GOOD_FUEL: noteLocked(slot, "Pyke fuel: one of your fleets may move 2 steps this round.");
          newsPubLocked(NEWS_FUEL, pl.chosenFaction, -1, slot); newsTableLocked(NEWS_FUEL, pl.chosenFaction, -1, -1); break;  // 3.1-dev.74: everyone knows
        case GOOD_REPAIRS: repairShipsLocked(slot, 2); break;
        case GOOD_SURVEY: for (int c = 0; c < NUM_CHARACTERISTICS; c++) pl.revealed[target][c] = true; break;
        case GOOD_BLESSING: pl.nextBattleReroll++; break;
        case GOOD_MERCENARIES: pl.nextBattleMercs = (uint8_t)min(MERC_SHIPS_MAX, pl.nextBattleMercs + MERC_SHIPS); break;
        case GOOD_SMUGGLED: addShipLocked(slot); break;
        case GOOD_AUDIENCE: applyReputationLocked(slot, target, GOOD_AUDIENCE_STANDING, false, TXC_DEAL, -1); break;
        case GOOD_CARGO: pl.cargoDue += GOOD_CARGO_RETURN; break;
        case GOOD_HARBOR: pl.harborRound = (int8_t)state.round; break;
        case GOOD_BACTA: repairShipsLocked(slot, 1); break;
        case GOOD_PERMITS: pl.permitDiscount += GOOD_PERMITS_DISCOUNT; break;
        case GOOD_TROOPS: { const int w = weakestHeldWorld(state, slot); if (w >= 0) state.planetDefense[w]++; break; }
        default: break;
      }
      Serial.printf("[MARKET] P%d bought good %d at faction %d's stall\n", slot + 1, good, f);
      ok = true;
    }
  }
  xSemaphoreGive(stateMutex);
  return ok;
}


// ===========================================================================
// 3.1-dev.10 GALAXY NEWS, GRUDGES, JOBS (vision doc tab "Galaxy News"). All run
// at a round start, mutex held. Every move is logged (LOG_NEWS public /
// LOG_PNEWS private); phones show each one as a full-screen announcement, the
// table shows the round's headline.
static void newsPubLocked(int id, int faction, int planet, int other) { evPubLocked(LOG_NEWS, faction, planet, other, id); }
static void exposeLocked(int target);  // 3.1-dev.78 (with Blackmail)
static void buyoutSettleLocked(int planetIdx, bool match);  // 3.1-dev.79
static void pnewsLocked(int slot, int id, int faction, int planet, int other) { evMineLocked(slot, LOG_PNEWS, faction, planet, other, id); }
// 3.1-dev.15 THE TABLE'S MOMENT QUEUE: one at a time, TABLE_MOMENT_MS each, never over a battle; the rest wait.
static void tableShowLocked(int moment, int a, int p, int v) {
  state.publicMoment = (int8_t)moment; state.publicActor = (int8_t)a; state.publicPlanet = (int8_t)p; state.publicVictim = (int8_t)v;
  state.publicSeq++; state.tqShownMs = millis();
}
static void newsTableLocked(int id, int faction, int planet, int victimFaction) {
  const bool busy = state.battleActive || (state.tqShownMs && millis() - state.tqShownMs < TABLE_MOMENT_MS);
  if (!busy && !state.tqN) { tableShowLocked(PUBLIC_NEWS_BASE + id, faction, planet, victimFaction); return; }
  if (state.tqN < 6) { int8_t *q = state.tq[state.tqN++]; q[0] = (int8_t)(PUBLIC_NEWS_BASE + id); q[1] = (int8_t)faction; q[2] = (int8_t)planet; q[3] = (int8_t)victimFaction; }
}
static void tableQueueTickLocked() {
  if (!state.tqN || state.battleActive || state.stage != STAGE_PLAYING) return;
  if (state.tqShownMs && millis() - state.tqShownMs < TABLE_MOMENT_MS) return;
  tableShowLocked(state.tq[0][0], state.tq[0][1], state.tq[0][2], state.tq[0][3]);
  for (int k = 1; k < state.tqN; k++) memcpy(state.tq[k - 1], state.tq[k], 4);
  state.tqN--;
}
static bool homeWorld(int p) { return liveHomeOf(state, p) >= 0; }
// 3.1-dev.51 SPREAD THE CHANCES (playtest: races / jobs / commotions "group in one place most games"): of the worlds that
// qualify, prefer one far from the last few the galaxy named (still random -- a jitter on the distance). Map positions as
// the phone's GALAXY_POS (and FH_POS below).
static const int16_t SPREAD_POS[NUM_PLANETS][2] = { {-59,181},{59,-181},{0,124},{59,181},{0,-56},{-59,-181},{-154,112},{-154,-112},{-48,28},
                                                    {88,88},{88,-88},{0,-124},{154,112},{154,-112},{-88,88},{-88,-88},{48,28} };
static int spreadPickLocked(const int *cand, int n) {
  if (n <= 0) return -1;
  int best = -1; float bestScore = -1;
  for (int i = 0; i < n; i++) {
    const int p = cand[i];
    float near = 400;
    for (int k = 0; k < 4; k++) {
      const int q = state.chanceRecent[k];
      if (q < 0 || q >= NUM_PLANETS) continue;
      const float dx = SPREAD_POS[p][0] - SPREAD_POS[q][0], dy = SPREAD_POS[p][1] - SPREAD_POS[q][1];
      near = min(near, sqrtf(dx * dx + dy * dy));
    }
    const float score = near + (float)(esp_random() % 120);  // far is better, but never fixed
    if (score > bestScore) { bestScore = score; best = p; }
  }
  state.chanceRecent[state.chanceNext++ % 4] = (int8_t)best;
  return best;
}
static int pickWorldLocked(bool (*ok)(int)) {  // a world that passes ok (spread away from recent chances), or -1
  int all[NUM_PLANETS], n = 0;
  for (int p = 0; p < NUM_PLANETS; p++) if (ok(p)) all[n++] = p;
  return spreadPickLocked(all, n);
}
static bool unclaimedOuter(int p) { return state.planetOwner[p] == PLANET_UNCONTROLLED && !homeWorld(p); }
static bool unclaimedHurt(int p) { return unclaimedOuter(p) && state.planetDefense[p] < planetStartGarrison(p); }
static void loseShipsLocked(int slot, int f, int n) {
  Player &pl = state.players[slot];
  for (int s = pl.fleetShipCount[f] - 1; s >= 0 && n > 0; s--) if (pl.fleetShipAlive[f][s]) { pl.fleetShipAlive[f][s] = false; n--; }
}
static int fleetAliveLocked(int slot, int f) {
  int n = 0;
  for (int s = 0; s < state.players[slot].fleetShipCount[f] && s < FLEET_SHIPS; s++) if (state.players[slot].fleetShipAlive[f][s]) n++;
  return n;
}
static int heldWorldLocked(int slot, bool weakest, bool allowHome) {
  int best = -1;
  for (int p = 0; p < NUM_PLANETS; p++) {
    if (state.planetOwner[p] != slot || (!allowHome && homeWorld(p))) continue;
    if (best < 0 || (weakest ? state.planetDefense[p] < state.planetDefense[best] : state.planetDefense[p] > state.planetDefense[best])) best = p;
  }
  return best;
}

// A grudge strike lands (the faction's own way of coming for you).
// 3.1-dev.11 FIXED damage (user: "fast, but some kind of animation"): the warning names it, the phone animates it.
// Logged as other = before * 10 + after (garrison or ships) so the phone can count it down.
static int strikeWorldLocked(int p, int hit, int f) {  // returns before*10+after; a world at 0 falls back to faction f
  const int before = state.planetDefense[p];
  if (homeWorld(p)) { state.planetDefense[p] = max(1, before - hit); return before * 10 + state.planetDefense[p]; }  // a home world is never taken this way
  const int after = max(0, before - hit);
  if (after == 0) {
    g_takeHow = TAKE_OTHER;
    setOwnerLocked(p, PLANET_UNCONTROLLED);
    state.planetFactionOf[p] = (int8_t)f;
    state.planetDefense[p] = FALLEN_GARRISON;
  } else state.planetDefense[p] = after;
  return before * 10 + after;
}
// 3.1-dev.42 a ROUND deed: why your fleets can't attack
static const char *const NO_ATTACK_WHY[4] = { "You promised the Rebels you'd stand down this round.", "Your fleets are on Imperial escort duty this round.",
                                              "Your holds are full of Pyke cargo this round.", "The Crimson Dawn told you to lie low this round." };
// 3.1-dev.42 a lent ship comes home (or doesn't): revive one ship in the fleet it left
static void loanReturnLocked(int slot, bool home, bool rode) {
  Player &pl = state.players[slot];
  const int f = pl.loanFleet;
  if (f < 0) return;
  int back = 0;  // 3.1-dev.72: several ships are away; a blade rolls for each of them
  for (int i = 0; i < max(1, (int)pl.loanN); i++) {
    const bool h = rode ? (int)(1 + esp_random() % 6) > DEED_BLADE_LOST_MAX : home;
    if (!h) continue;
    for (int s = 0; s < pl.fleetShipCount[f] && s < FLEET_SHIPS; s++) if (!pl.fleetShipAlive[f][s]) { pl.fleetShipAlive[f][s] = true; back++; break; }
  }
  pnewsLocked(slot, PN_LOAN_BACK, pl.loanKind == 2 ? FACTION_CRIMSON_DAWN : FACTION_REBEL, f, (rode ? 2 : 0) + (back > 0 ? 1 : 0));
  pl.loanFleet = pl.loanBack = -1; pl.loanKind = 0; pl.loanN = 0;
}
static int biggestFleetLocked(int slot, int minShips) {
  int best = -1;
  for (int k = 0; k < NUM_FLEETS; k++) if (fleetAliveLocked(slot, k) >= minShips && (best < 0 || fleetAliveLocked(slot, k) > fleetAliveLocked(slot, best))) best = k;
  return best;
}
static int grudgeTargetFor(int slot, int f) {  // the world a strike will hit (Empire: your weakest non-home world; Rebels: your strongest)
  if (f == FACTION_EMPIRE) { const int p = heldWorldLocked(slot, true, false); return p >= 0 ? p : heldWorldLocked(slot, true, true); }
  if (f == FACTION_REBEL) return heldWorldLocked(slot, false, true);
  return -1;
}
static void grudgeStrikeLocked(int slot, int f) {
  Player &pl = state.players[slot];
  int planet = -1, detail = 0;
  switch (f) {
    case FACTION_EMPIRE: case FACTION_REBEL: {
      planet = pl.grudgeTarget;
      if (planet < 0 || state.planetOwner[planet] != slot) planet = grudgeTargetFor(slot, f);  // the named world changed hands: the next one
      if (planet < 0) break;
      if (f == FACTION_EMPIRE) detail = strikeWorldLocked(planet, GRUDGE_EMPIRE_HIT, FACTION_EMPIRE);
      else { const int before = state.planetDefense[planet]; state.planetDefense[planet] = max(1, before - GRUDGE_UPRISING); detail = before * 10 + state.planetDefense[planet]; }
      break;
    }
    case FACTION_PYKE: detail = min((int)pl.credits, GRUDGE_SHAKEDOWN_CR); pl.credits -= detail; break;
    default: {  // Crimson Dawn assassins: your biggest fleet, never its last ship
      int best = -1;
      for (int k = 0; k < NUM_FLEETS; k++) if (fleetAliveLocked(slot, k) > 0 && (best < 0 || fleetAliveLocked(slot, k) > fleetAliveLocked(slot, best))) best = k;
      if (best < 0) break;
      const int before = fleetAliveLocked(slot, best), lost = min(GRUDGE_ASSASSIN_HIT, before - 1);
      loseShipsLocked(slot, best, lost);
      planet = best;  // (the fleet, for the phone)
      detail = before * 10 + (before - lost);
      for (int i = 0; i < MAX_PLAYERS; i++) {  // 3.1-dev.42 LEND A BLADE: a lent ship rode with them -- the victim learns whose
        Player &lp = state.players[i];
        if (i == slot || !lp.active || lp.loanKind != 2 || lp.loanFleet < 0) continue;
        pnewsLocked(slot, PN_BLADE_SEEN, FACTION_CRIMSON_DAWN, -1, i);
        loanReturnLocked(i, true, true);  // 3.1-dev.72: each lent blade rolls its own 1-2
      }
      break;
    }
  }
  pl.grudgeTarget = -1;
  newsPubLocked(NEWS_STRIKE, f, f == FACTION_CRIMSON_DAWN ? -1 : planet, slot);
  pnewsLocked(slot, PN_STRIKE_HIT, f, planet, detail);
  pl.lastStrike[f] = (int8_t)state.round;
  if (state.strikesThisGame < 255) state.strikesThisGame++;
  Serial.printf("[NEWS] grudge strike: faction %d on P%d (planet %d, detail %d)\n", f, slot + 1, planet, detail);
}


// ---- 3.1-dev.48 THE SCOUNDREL: SELL IT TWICE (see SELL_* in game_state.h) ----
// You pleased faction `pleased` with this work: its rival offers to pay for the same thing (a fight: the VICTIM pays you to
// "find out who did it"). One offer on the table at a time; it lapses at the next round start.
static void sellOfferLocked(int slot, int buyer, int kind, int planet) {
  if (slot < 0 || slot >= MAX_PLAYERS || classOfLocked(slot) != CLASS_SCOUNDREL || buyer < 0 || buyer >= NUM_FACTIONS) return;
  if (state.stage != STAGE_PLAYING || snapshotFhFinal(state)) return;  // the war's over in the Final Hour
  Player &pl = state.players[slot];
  if (pl.caughtRound >= 0) return;  // already in the freighter
  int cr = kind == SELL_JOB ? SELL_CR_JOB : kind == SELL_DEED ? SELL_CR_DEED : SELL_CR_OUTER;
  if ((kind == SELL_TALK || kind == SELL_PAY || kind == SELL_FIGHT) && planet >= 0 && planet < NUM_PLANETS)
    cr = PLANETS[planet].ring == RING_CORE ? SELL_CR_CORE : PLANETS[planet].ring == RING_MID ? SELL_CR_MID : SELL_CR_OUTER;
  pl.sellTo = (int8_t)buyer; pl.sellKind = (int8_t)kind; pl.sellPlanet = (int8_t)planet; pl.sellRound = (int8_t)state.round; pl.sellCr = (uint8_t)cr;
  Serial.printf("[SELL] P%d may sell it twice to faction %d (kind %d, world %d, %d CR)\n", slot + 1, buyer, kind, planet, cr);
}
bool gameStateSellTwice(int slot, bool sell) {
  if (g_remote || slot < 0 || slot >= MAX_PLAYERS) return false;
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return false;
  Player &pl = state.players[slot];
  const bool ok = pl.sellTo >= 0 && pl.sellRound == state.round && state.stage == STAGE_PLAYING;
  if (ok && sell) {
    const int f = pl.sellTo, rival = rivalFactionIndex(f);
    pl.credits += pl.sellCr;
    pl.soldCr[f] = (uint8_t)min(255, pl.soldCr[f] + pl.sellCr);
    pnewsLocked(slot, PN_SOLD, f, pl.sellPlanet, pl.sellCr);
    applyReputationLocked(slot, f, SELL_FAVOR, false, TXC_DEAL, pl.sellPlanet);
    pl.heat++; pl.heatSold = (int8_t)state.round;
    Serial.printf("[SELL] P%d sold it twice to faction %d: +%d CR, heat %d\n", slot + 1, f, pl.sellCr, pl.heat);
    if (pl.heat >= SELL_HEAT_CAUGHT) {  // CAUGHT: the two sides compared notes
      pl.heat = 0;
      pl.caughtF[0] = (int8_t)f; pl.caughtF[1] = (int8_t)rival; pl.caughtRound = (int8_t)(state.round + 1);
      applyReputationLocked(slot, f, SELL_CAUGHT_FAVOR, false, TXC_BROKE, pl.sellPlanet);
      applyReputationLocked(slot, rival, SELL_CAUGHT_FAVOR, false, TXC_BROKE, pl.sellPlanet);
      newsPubLocked(NEWS_CAUGHT, f, pl.sellPlanet, slot);
      newsTableLocked(NEWS_CAUGHT, f, pl.sellPlanet, pl.chosenFaction);
      Serial.printf("[SELL] P%d CAUGHT by factions %d + %d\n", slot + 1, f, rival);
    }
  }
  if (ok) { pl.sellTo = pl.sellKind = pl.sellPlanet = pl.sellRound = -1; pl.sellCr = 0; }
  xSemaphoreGive(stateMutex);
  return ok;
}
int snapshotPayBackCost(const GameState &gs, int slot, int f) {
  if (slot < 0 || slot >= MAX_PLAYERS || f < 0 || f >= NUM_FACTIONS) return 0;
  return gs.players[slot].soldCr[f] + SELL_PAYBACK_EXTRA;
}
bool gameStatePayBack(int slot, int f) {
  if (g_remote || slot < 0 || slot >= MAX_PLAYERS || f < 0 || f >= NUM_FACTIONS) return false;
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return false;
  Player &pl = state.players[slot];
  const int k = pl.caughtF[0] == f ? 0 : pl.caughtF[1] == f ? 1 : -1;
  const int cost = snapshotPayBackCost(state, slot, f);
  const bool ok = k >= 0 && pl.caughtRound > state.round - 1 && pl.credits >= cost;
  if (ok) {
    pl.credits -= cost; pl.soldCr[f] = 0; pl.caughtF[k] = -1;
    pnewsLocked(slot, PN_PAIDBACK, f, -1, cost);
    if (pl.caughtF[0] < 0 && pl.caughtF[1] < 0) pl.caughtRound = -1;
    Serial.printf("[SELL] P%d paid faction %d back (%d CR)\n", slot + 1, f, cost);
  }
  xSemaphoreGive(stateMutex);
  return ok;
}
// a round starts: offers lapse, heat cools after a quiet round, a caught Scoundrel's two strikes land
static void sellRoundStartLocked(int r) {
  for (int i = 0; i < MAX_PLAYERS; i++) {
    Player &pl = state.players[i];
    if (!pl.active) continue;
    pl.sellTo = pl.sellKind = pl.sellPlanet = pl.sellRound = -1; pl.sellCr = 0;
    if (pl.heat > 0 && pl.heatSold < r - 1) pl.heat--;
    if (pl.caughtRound == r) {
      for (int k = 0; k < 2; k++) {
        const int f = pl.caughtF[k];
        if (f < 0) continue;
        pl.grudgeTarget = (int8_t)grudgeTargetFor(i, f);
        grudgeStrikeLocked(i, f);
        pl.soldCr[f] = 0;
      }
      pl.caughtF[0] = pl.caughtF[1] = pl.caughtRound = -1;
    }
  }
}

// A world changed hands: does it finish a race or a job?
static void newsOnGainLocked(int slot, int p) {
  if (state.stage != STAGE_PLAYING || slot < 0 || slot >= MAX_PLAYERS) return;
  Player &pl = state.players[slot];
  if (state.raceFaction >= 0 && state.raceUntil >= state.round && (state.raceDoneRound < 0 || state.raceDoneRound == state.round) && !((state.raceWinners >> slot) & 1)) {
    const bool desperate = state.raceWorld == -2;
    if ((desperate && planetFaction(p) == rivalFactionIndex(state.raceFaction)) || (!desperate && state.raceWorld == p)) {
      const int f = state.raceFaction;
      pl.credits += desperate ? DESPERATION_CR : RACE_CR;
      applyReputationLocked(slot, f, desperate ? DESPERATION_FAVOR : RACE_FAVOR, false, TXC_DEAL, p);
      newsPubLocked(NEWS_RACE_WON, f, p, slot);
      pnewsLocked(slot, PN_RACE_WON, f, p, desperate ? 1 : 0);
      state.raceDoneRound = (int8_t)state.round; state.raceWinners |= (uint8_t)(1u << slot);  // 3.1-dev.49 #12: others can still finish it THIS round
    }
  }
  if (pl.jobState == 2 && pl.jobWorld == p) {
    pl.credits += JOB_CR;
    applyReputationLocked(slot, pl.jobFaction, JOB_FAVOR, false, TXC_DEAL, p);
    pnewsLocked(slot, PN_JOB_DONE, pl.jobFaction, p, 0);
    sellOfferLocked(slot, rivalFactionIndex(pl.jobFaction), SELL_JOB, p);  // 3.1-dev.48
    pl.jobState = 0; pl.jobFaction = pl.jobWorld = pl.jobDue = -1;
  }
}

bool gameStateAnswerJob(int slot, bool take) {
  if (g_remote) return remoteCall(RF_ANSWER_JOB, {slot, take ? 1 : 0}).value == 1;
  if (slot < 0 || slot >= MAX_PLAYERS || !stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return false;
  Player &pl = state.players[slot];
  const bool ok = pl.jobState == 1;
  if (ok) {
    pnewsLocked(slot, take ? PN_JOB_TAKEN : PN_JOB_PASSED, pl.jobFaction, pl.jobWorld, 0);  // 3.1-dev.16: your History
    if (take) pl.jobState = 2;
    else { pl.jobState = 0; pl.jobFaction = pl.jobWorld = pl.jobDue = -1; }
  }
  xSemaphoreGive(stateMutex);
  return ok;
}

// Offer `slot` a job from faction f: a world that isn't f's own (unclaimed; in the second half also another player's). 3.1-dev.28 (user: jobs and
// rumors "seem to be about the same two planets"): the pool was only f's RIVAL's unclaimed worlds (often
// one or two, the same ones every time) -- now any unclaimed world not of f, the rival's counted twice,
// and the last few job worlds skipped while there is anything else.
static int8_t recentJobWorld[4] = { -1, -1, -1, -1 };  // not saved: only for variety
static bool offerJobLocked(int slot, int f, bool announced) {
  Player &pl = state.players[slot];
  // 3.1-dev.29 (user: "jobs should go to claimed worlds too as the game develops ... maybe not in the
  // first half to push people to find more planets"): from the second half on, a world ANOTHER PLAYER
  // holds can be the job too (never a home world, never your own).
  const bool lateGame = state.round > state.roundLimit / 2;
  // 3.1-dev.122 (user: "if the planets you had taken ... used that as a general idea of where your ships were to help triangulate
  // doable jobs"): the game can't see the board, but your worlds say roughly where your fleets are -- the first pass only offers
  // worlds within two jumps of one you hold (your home counts), one jump away twice as often.
  uint32_t near1 = 0, near2 = 0;
  for (int q = 0; q < NUM_PLANETS; q++) if (state.planetOwner[q] == slot) near1 |= snapshotNeighbors(q);
  for (int q = 0; q < NUM_PLANETS; q++) if ((near1 >> q) & 1) near2 |= snapshotNeighbors(q);
  near2 |= near1;
  int all[4 * NUM_PLANETS], n = 0;
  for (int pass = 0; pass < 3 && n == 0; pass++)
    for (int p = 0; p < NUM_PLANETS; p++) {
      const int own = state.planetOwner[p];
      const bool open = unclaimedOuter(p) || (lateGame && own >= 0 && own != slot && !homeWorld(p));
      if (!open || planetFaction(p) == f || p == state.raceWorld) continue;
      if (pass == 0 && near2 && !((near2 >> p) & 1)) continue;  // too far from anything you hold
      bool recent = false;
      for (int k = 0; k < 4; k++) if (recentJobWorld[k] == p) recent = true;
      if (recent && pass < 2) continue;
      all[n++] = p;
      if ((near1 >> p) & 1) all[n++] = p;  // one jump from your worlds: twice as likely
      if (planetFaction(p) == rivalFactionIndex(f)) all[n++] = p;  // the rival's worlds come up more often
    }
  if (!n) return false;
  pl.jobFaction = (int8_t)f; pl.jobWorld = (int8_t)spreadPickLocked(all, n);  // 3.1-dev.51 spread out
  for (int k = 3; k > 0; k--) recentJobWorld[k] = recentJobWorld[k - 1];
  recentJobWorld[0] = pl.jobWorld;
  pl.jobDue = (int8_t)(state.round + JOB_ROUNDS - 1);
  pl.jobState = 1; pl.jobAnnounced = announced ? 1 : 0;
  if (pl.jobsOffered < 255) pl.jobsOffered++;
  pnewsLocked(slot, PN_JOB, f, pl.jobWorld, announced ? 1 : 0);
  return true;
}
// The faction (tier 3+) a player would work for: their best standing.
static int jobFactionFor(int slot) {
  int best = -1;
  for (int f = 0; f < NUM_FACTIONS; f++)
    if (tierOfLocked(slot, f) >= REP_NEUTRAL && (best < 0 || state.players[slot].reputation[f] > state.players[slot].reputation[best])) best = f;
  return best;
}
// The fairest player to hire: eligible, no job going, fewest offers so far.
static int fairestHireLocked() {
  int best = -1;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    const Player &pl = state.players[i];
    if (!pl.active || pl.jobState != 0 || jobFactionFor(i) < 0) continue;
    if (best < 0 || pl.jobsOffered < state.players[best].jobsOffered || (pl.jobsOffered == state.players[best].jobsOffered && (esp_random() & 1))) best = i;
  }
  return best;
}

static void eraRoundStartLocked();  // below
static void lotsRoundStartLocked();
static void dealLotsLocked(int i);
static void movesRoundStartLocked(bool *personal);
static void newsRoundStartLocked(bool tableBusy) {
  if (state.stage != STAGE_PLAYING) return;
  if (state.round <= 1) return;  // 3.1-dev.16 (user 2026-11-07): no Galaxy News in round 1
  const int r = state.round, late = state.roundLimit - 2;  // late = the last 3 rounds
  bool personal[MAX_PLAYERS] = {};
  bool tableUsed = tableBusy;
  // 1. Old business: a race that ran out, jobs offered and never answered, jobs out of time.
  if (state.raceFaction >= 0 && (state.raceUntil < r || (state.raceDoneRound >= 0 && state.raceDoneRound < r))) state.raceFaction = state.raceWorld = state.raceUntil = -1;
  if (state.raceFaction < 0) { state.raceDoneRound = -1; state.raceWinners = 0; }  // 3.1-dev.49
  eraRoundStartLocked();  // 3.1-dev.12
  lotsRoundStartLocked();  // 3.1-dev.88
  for (int i = 0; i < MAX_PLAYERS; i++) {  // 3.1-dev.102 hired guns never used by the end of the next round: all go home
    Player &h = state.players[i];
    if (!h.hiredN || h.hiredUntil >= r) continue;
    const int w = h.hiredFrom;
    if (w >= 0 && w < MAX_PLAYERS) { Player &wl = state.players[w];
      for (int b = 0; b < 32; b++) if ((wl.gfhMask >> b) & 1) { const int f = b / 8, s = b % 8; if (f < NUM_FLEETS && s < FLEET_SHIPS) wl.fleetShipAlive[f][s] = true; }
      wl.gfhMask = 0; pnewsLocked(w, PN_GFH_BACK, -1, -1, h.hiredN * 8 + h.hiredN); }
    h.hiredN = 0; h.hiredFrom = -1; h.hiredUntil = -1;
  }
  movesRoundStartLocked(personal);  // 3.1-dev.13
  for (int i = 0; i < MAX_PLAYERS; i++) {
    Player &pl = state.players[i];
    if (!pl.active) continue;
    if (pl.jobState == 1) { pl.jobState = 0; pl.jobFaction = pl.jobWorld = pl.jobDue = -1; }
    else if (pl.jobState == 2 && pl.jobDue < r) {
      pnewsLocked(i, PN_JOB_FAILED, pl.jobFaction, pl.jobWorld, 0);
      applyReputationLocked(i, pl.jobFaction, JOB_FAIL_FAVOR, false, TXC_DEAL, pl.jobWorld);
      pl.jobState = 0; pl.jobFaction = pl.jobWorld = pl.jobDue = -1;
      personal[i] = true;
    }
  }
  sellRoundStartLocked(r);  // 3.1-dev.48 offers lapse, heat cools, a caught Scoundrel's two strikes land
  for (int p = 0; WORLD_TURN_ON && p < NUM_PLANETS; p++) {  // 3.1-dev.49 #18 a world held WORLD_TURN_ROUNDS rounds takes its holder's side (OFF since dev.74)
    const int o = state.planetOwner[p];
    if (o < 0 || o >= MAX_PLAYERS || planetHomeOf(p) >= 0) continue;
    const int hf = state.players[o].chosenFaction;
    if (hf < 0 || planetFaction(p) == hf || r - state.planetHeldSince[p] < WORLD_TURN_ROUNDS) continue;
    state.planetFactionOf[p] = (int8_t)hf;
    state.players[o].revealed[p][0] = true;
    pnewsLocked(o, PN_TURNED, hf, p, -1);
    personal[o] = true;
    Serial.printf("[NEWS] %s now sides with faction %d (held by P%d)\n", PLANETS[p].name, hf, o + 1);
  }
  // 2. Grudge strikes that come due (warned last round). Climbed out of tier 1? They stand down.
  for (int i = 0; i < MAX_PLAYERS; i++) {
    Player &pl = state.players[i];
    if (!pl.active || pl.grudgeFaction < 0 || pl.grudgeRound != r) continue;
    const int f = pl.grudgeFaction;
    pl.grudgeFaction = pl.grudgeRound = -1;
    if (tierOfLocked(i, f) == REP_HOSTILE) {  // (grudgeStrikeLocked clears grudgeTarget)
      grudgeStrikeLocked(i, f);
      if (!tableUsed) { newsTableLocked(NEWS_STRIKE, f, -1, pl.chosenFaction); tableUsed = true; }
    } else { pnewsLocked(i, PN_STRIKE_OFF, f, -1, 0); pl.grudgeTarget = -1; }
    personal[i] = true;
  }
  // 2a. 3.1-dev.42: ships lent on a deed come home (a blade that never rode comes back safe).
  for (int i = 0; i < MAX_PLAYERS; i++) {
    Player &pl = state.players[i];
    if (pl.active && pl.loanFleet >= 0 && r >= pl.loanBack) { loanReturnLocked(i, true, false); personal[i] = true; }
  }
  // 2b. A late offensive that was announced last round lands (on whoever holds the world now).
  if (state.offFaction >= 0 && state.offRound == r) {
    const int p = state.offWorld, f = state.offFaction, o = p >= 0 ? state.planetOwner[p] : -1;
    if (o >= 0 && o < MAX_PLAYERS) {
      const int d = strikeWorldLocked(p, OFFENSIVE_HIT, f);
      pnewsLocked(o, PN_OFFENSIVE_HIT, f, p, d);
      newsPubLocked(NEWS_OFFENSIVE_LANDS, f, p, o);
      if (!tableUsed) { newsTableLocked(NEWS_OFFENSIVE_LANDS, f, p, state.players[o].chosenFaction); tableUsed = true; }
      personal[o] = true;
    }
    state.offWorld = state.offFaction = state.offRound = -1;
  }
  // 3. New grudge: one warning a round at most, never in round 1, never in the last round, a few a game.
  if (r >= 2 && r < state.roundLimit && state.strikesThisGame < GRUDGE_MAX_PER_GAME) {
    int who = -1, fw = -1, worst = 1000;
    for (int i = 0; i < MAX_PLAYERS; i++) {
      const Player &pl = state.players[i];
      if (!pl.active || pl.grudgeFaction >= 0) continue;
      for (int f = 0; f < NUM_FACTIONS; f++)
        if (tierOfLocked(i, f) == REP_HOSTILE && r - pl.lastStrike[f] >= GRUDGE_COOLDOWN && pl.reputation[f] < worst) { worst = pl.reputation[f]; who = i; fw = f; }
    }
    if (who >= 0) {
      state.players[who].grudgeFaction = (int8_t)fw;
      state.players[who].grudgeRound = (int8_t)(r + 1);
      state.players[who].grudgeTarget = (int8_t)grudgeTargetFor(who, fw);
      {  // 3.1-dev.42: the ONE deed they'll ask for -- their next in turn that you can do
        Player &wp = state.players[who];
        wp.grudgeDeed = wp.deedNext[fw];
        const int k = snapshotOfferedDeed(state, who);
        if (k >= 0) { wp.grudgeDeed = (int8_t)k; wp.deedNext[fw] = (int8_t)((k + 1) % 3); }
      }
      pnewsLocked(who, PN_STRIKE_WARN, fw, state.players[who].grudgeTarget, 0);
      personal[who] = true;
    }
  }
  // 4. One table-wide item, by the stage of the game; never the same one twice in a row.
  int cand[12], nc = 0;
  auto add = [&](int id, int weight) { for (int k = 0; k < weight && nc < 12; k++) if (id != state.lastNews) cand[nc++] = id; };
  const bool early = r <= 3, isLate = r >= late && r > 3;
  bool anyHurt = false, anyOpen = false, anyHeldOwn = false;
  for (int p = 0; p < NUM_PLANETS; p++) {
    if (unclaimedHurt(p)) anyHurt = true;
    if (unclaimedOuter(p)) anyOpen = true;
    const int o = state.planetOwner[p];
    if (o >= 0 && o < MAX_PLAYERS && !homeWorld(p) && state.players[o].chosenFaction != planetFaction(p)) anyHeldOwn = true;
  }
  if (state.offFaction >= 0) anyHeldOwn = false;  // one offensive at a time
  if (early) { if (anyOpen) { add(NEWS_COMMOTION, 2); add(NEWS_RACE, 2); } if (anyHurt) add(NEWS_REINFORCE, 1); if (state.marketsOn) add(NEWS_MARKETDAY, 1); }
  else if (!isLate) { if (anyOpen) { add(NEWS_HIRING, 2); add(NEWS_RACE, 1); } if (state.marketsOn) add(NEWS_EMBARGO, 1); if (anyHurt) add(NEWS_REINFORCE, 1); }
  else { if (anyHeldOwn) add(NEWS_OFFENSIVE, 2); add(NEWS_DESPERATION, 1); if (!state.lastStandDone) add(NEWS_LASTSTAND, 1); if (anyOpen) add(NEWS_RACE, 1); }
  if (!nc && anyOpen) cand[nc++] = NEWS_RACE;
  if (nc) {
    const int id = cand[esp_random() % nc];
    int f = -1, p = -1, other = -1;
    switch (id) {
      case NEWS_COMMOTION: {
        p = pickWorldLocked(unclaimedOuter);
        const int was = planetFaction(p);
        int nf = (was + 1 + (int)(esp_random() % 3)) % NUM_FACTIONS;
        state.planetFactionOf[p] = (int8_t)nf;
        state.planetDefense[p] = max(1, state.planetDefense[p] - NEWS_COMMOTION_HIT);
        for (int i = 0; i < MAX_PLAYERS; i++)  // only those who knew its Sympathy learn who's behind it
          if (state.players[i].active && state.players[i].revealed[p][0]) { pnewsLocked(i, PN_COMMOTION_WHO, nf, p, was); }
        break;
      }
      case NEWS_REINFORCE:
        p = pickWorldLocked(unclaimedHurt); f = planetFaction(p);
        state.planetDefense[p] = min(planetStartGarrison(p), state.planetDefense[p] + NEWS_REINFORCE_SHIPS);
        break;
      case NEWS_RACE:
        p = pickWorldLocked(unclaimedOuter); f = rivalFactionIndex(planetFaction(p));
        state.raceWorld = (int8_t)p; state.raceFaction = (int8_t)f; state.raceUntil = (int8_t)(r + 1);
        break;
      case NEWS_MARKETDAY: {
        f = (int)(esp_random() % NUM_FACTIONS);
        for (int k = 0; k < 3; k++) {
          const int g = FACTION_GOODS[f][k];
          if (g != state.stallGood[f][0] && g != state.stallGood[f][1]) { state.stallGood[f][2] = (int8_t)g; state.stallStock[f][2] = 1; other = g; }
        }
        break;
      }
      case NEWS_HIRING: {
        const int who = fairestHireLocked();
        f = who >= 0 ? jobFactionFor(who) : (int)(esp_random() % NUM_FACTIONS);
        if (who >= 0 && offerJobLocked(who, f, true)) personal[who] = true;
        break;
      }
      case NEWS_EMBARGO:
        f = (int)(esp_random() % NUM_FACTIONS); state.embargoFaction = (int8_t)f; state.embargoRound = (int8_t)r;
        break;
      case NEWS_OFFENSIVE: {
        int all[NUM_PLANETS], n = 0;
        for (int q = 0; q < NUM_PLANETS; q++) {
          const int o = state.planetOwner[q];
          if (o >= 0 && o < MAX_PLAYERS && !homeWorld(q) && state.players[o].chosenFaction != planetFaction(q)) all[n++] = q;
        }
        if (!n) break;
        p = all[esp_random() % n]; f = planetFaction(p);
        const int o = state.planetOwner[p];
        other = o;
        state.offWorld = (int8_t)p; state.offFaction = (int8_t)f; state.offRound = (int8_t)(r + 1);  // 3.1-dev.11: lands next round
        pnewsLocked(o, PN_OFFENSIVE_WARN, f, p, OFFENSIVE_HIT);
        personal[o] = true;
        break;
      }
      case NEWS_DESPERATION: {  // the faction that has lost the most worlds to players
        int lost[NUM_FACTIONS] = {};
        for (int q = 0; q < NUM_PLANETS; q++) if (state.planetOwner[q] >= 0 && !homeWorld(q)) lost[planetFaction(q)]++;
        f = 0;
        for (int k = 1; k < NUM_FACTIONS; k++) if (lost[k] > lost[f]) f = k;
        state.raceWorld = -2; state.raceFaction = (int8_t)f; state.raceUntil = (int8_t)(r + 1);
        other = rivalFactionIndex(f);
        break;
      }
      case NEWS_LASTSTAND:
        for (int q = 0; q < NUM_PLANETS; q++) if (homeWorld(q)) state.planetDefense[q] = min(snapshotDefenseLimit(state, q), state.planetDefense[q] + NEWS_LASTSTAND_SHIPS);
        state.lastStandDone = 1;
        break;
    }
    state.lastNews = (int8_t)id;
    if (id != NEWS_OFFENSIVE || p >= 0) {
      newsPubLocked(id, f, p, other);
      if (!tableUsed) { newsTableLocked(id, f, p, -1); tableUsed = true; }
      Serial.printf("[NEWS] round %d: %d (faction %d, world %d)\n", r, id, f, p);
    }
  }
  // 5. One personal item for everyone who has none yet: a debt due, a warning, a job, a tip-off.
  for (int i = 0; i < MAX_PLAYERS; i++) {
    Player &pl = state.players[i];
    if (!pl.active || personal[i]) continue;
    int debt = -1;
    for (int k = 0; k < MAX_PENDING_OBLIGATIONS; k++)
      if (pl.obligations[k].active && pl.obligations[k].dueRound >= 0 && pl.obligations[k].dueRound <= r + 1) { debt = k; break; }
    if (debt >= 0) { pnewsLocked(i, PN_DEBT, pl.obligations[debt].factionIdx, pl.obligations[debt].planetIdx, pl.obligations[debt].dueRound); continue; }
    int worst = -1;
    for (int f = 0; f < NUM_FACTIONS; f++)  // only when you FELL there (your home's rival starts in tier 2 -- that's not news)
      if (tierOfLocked(i, f) == REP_WARY && !(f == rivalFactionIndex(pl.chosenFaction) && pl.reputation[f] >= REP_START_RIVAL)
          && (worst < 0 || pl.reputation[f] < pl.reputation[worst])) worst = f;
    if (worst >= 0 && r - pl.lastWatch >= 2) { pnewsLocked(i, PN_WATCHING, worst, -1, 0); pl.lastWatch = (int8_t)r; continue; }
    int best = -1;
    for (int f = 0; f < NUM_FACTIONS; f++) if (tierOfLocked(i, f) >= REP_FAVORABLE && (best < 0 || pl.reputation[f] > pl.reputation[best])) best = f;
    const bool jobTurn = pl.jobState == 0 && jobFactionFor(i) >= 0 && (best < 0 || (r + i) % 2 == 0);
    if (jobTurn && offerJobLocked(i, jobFactionFor(i), false)) continue;
    if (pl.jobState == 2 && best < 0) { pnewsLocked(i, PN_JOB_REMIND, pl.jobFaction, pl.jobWorld, pl.jobDue); continue; }  // nothing else to say: your job
    if (best >= 0) {  // a tip-off: a friendly contact reveals one tile (Sympathy or Defense) on an unclaimed world
      int all[NUM_PLANETS * 2], n = 0;
      for (int p = 0; p < NUM_PLANETS; p++) {
        if (state.planetOwner[p] == i) continue;
        if (!pl.revealed[p][0]) all[n++] = p * 2;
        if (!pl.revealed[p][3]) all[n++] = p * 2 + 1;
      }
      if (n) {
        const int pick = all[esp_random() % n], p = pick / 2, ch = (pick & 1) ? 3 : 0;
        pl.revealed[p][ch] = true;
        pnewsLocked(i, PN_TIPOFF, best, p, ch);
      }
    }
  }
}

// ===========================================================================
// 3.1-dev.12 CHAMPIONS + ERAS
static void championUpdateLocked() {
  if (state.stage != STAGE_PLAYING) return;
  for (int i = 0; i < MAX_PLAYERS; i++) state.players[i].champOffer = -1;  // 3.1-dev.49: recomputed below
  for (int f = 0; f < NUM_FACTIONS; f++) {
    const int cur = state.champion[f];
    const bool curOk = cur >= 0 && cur < MAX_PLAYERS && state.players[cur].active && state.players[cur].reputation[f] >= REP_CHAMPION_MIN && state.players[cur].coverFaction != f;  // 3.1-dev.50 tier 6
    int best = curOk ? cur : -1;
    for (int i = 0; i < MAX_PLAYERS; i++) {
      if (!state.players[i].active || state.players[i].reputation[f] < REP_CHAMPION_MIN || i == best || state.players[i].coverFaction == f) continue;  // (no titles under cover; 3.1-dev.50 tier 6)
      if (state.players[i].titleBlock == f) continue;  // 3.1-dev.49: you gave this one up for another
      bool other = false;  // 3.1-dev.49 #13 ONE TITLE: someone who holds another title is offered a swap instead
      for (int g = 0; g < NUM_FACTIONS; g++) if (g != f && state.champion[g] == i) other = true;
      if (other) { if (best < 0 || state.players[i].reputation[f] > state.players[best].reputation[f]) state.players[i].champOffer = (int8_t)f; continue; }
      if (best < 0 || state.players[i].reputation[f] > state.players[best].reputation[f]) best = i;  // ties keep the holder
    }
    if (best == cur) continue;
    state.champion[f] = (int8_t)best;
    if (cur >= 0 && cur < MAX_PLAYERS) state.players[cur].decrees &= (uint8_t)~(1u << f);  // the title goes, the decree with it
    if (best >= 0) state.players[best].decrees |= (uint8_t)(1u << f);                     // one decree each time you gain it
    newsPubLocked(NEWS_CHAMPION, f, cur, best);
    newsTableLocked(NEWS_CHAMPION, f, -1, best >= 0 ? state.players[best].chosenFaction : -1);
    Serial.printf("[ERA] faction %d champion: P%d -> P%d\n", f, cur + 1, best + 1);
  }
}

bool gameStateSwapTitle(int slot) {  // 3.1-dev.49 #13: give up the title you hold for the one you're offered
  if (g_remote || slot < 0 || slot >= MAX_PLAYERS) return false;
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return false;
  Player &pl = state.players[slot];
  const int f = pl.champOffer;
  int held = -1;
  for (int g = 0; g < NUM_FACTIONS; g++) if (state.champion[g] == slot) held = g;
  const bool ok = f >= 0 && held >= 0 && held != f;
  if (ok) {
    state.champion[held] = -1;
    pl.decrees &= (uint8_t)~(1u << held);
    pl.titleBlock = (int8_t)held;
    championUpdateLocked();  // the new title is yours; the old one goes to the next best
    Serial.printf("[ERA] P%d swapped the faction %d title for faction %d\n", slot + 1, held, f);
  }
  xSemaphoreGive(stateMutex);
  return ok;
}
bool gameStateDeclareEra(int slot, int f) {
  if (g_remote) return remoteCall(RF_DECLARE_ERA, {slot, f}).value == 1;
  if (slot < 0 || slot >= MAX_PLAYERS || f < 0 || f >= NUM_FACTIONS) return false;
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return false;
  Player &pl = state.players[slot];
  const bool ok = state.stage == STAGE_PLAYING && (pl.decrees & (1u << f)) && state.champion[f] == slot
                  && snapshotEra(state) < 0;  // 3.1-dev.88: an Era is LOCKED once declared -- nobody can replace it
  if (ok) {
    pl.decrees &= (uint8_t)~(1u << f);
    state.markSlot = -1;
    if (state.eraFaction >= 0 && state.round <= state.eraUntil && state.eraFaction != f) newsPubLocked(NEWS_ERA_END, state.eraFaction, -1, -1);
    state.eraFaction = (int8_t)f; state.eraBy = (int8_t)slot; state.eraFrom = (int8_t)state.round;
    state.eraUntil = (int8_t)(state.round + ERA_ROUNDS - 1);  // 3.1-dev.30: ERA_ROUNDS, this round included (was half the game -- "lasted foreverrrr")
    state.lieRound = -1; state.lie[0] = '\0';
    if (f == FACTION_PYKE && state.phase == PHASE_INVEST)  // SPICE FLOWS at once: the third good comes out
      for (int k = 0; k < NUM_FACTIONS; k++)
        if (state.stallGood[k][0] >= 0 && state.stallGood[k][2] < 0)
          for (int g = 0; g < 3; g++) {
            const int gd = FACTION_GOODS[k][g];
            if (gd != state.stallGood[k][0] && gd != state.stallGood[k][1]) { state.stallGood[k][2] = (int8_t)gd; state.stallStock[k][2] = MARKET_STOCK; }
          }
    if (f == FACTION_CRIMSON_DAWN) for (int i = 0; i < MAX_PLAYERS; i++) if (state.players[i].active) dealLotsLocked(i);  // 3.1-dev.88 the market never sleeps
    newsPubLocked(NEWS_ERA, f, -1, slot);
    newsTableLocked(NEWS_ERA, f, -1, -1);
    Serial.printf("[ERA] P%d declares faction %d's Era, rounds %d-%d\n", slot + 1, f, state.eraFrom, state.eraUntil);
  }
  xSemaphoreGive(stateMutex);
  return ok;
}

bool gameStateSetLie(int slot, const char *text) {
  if (g_remote) return remoteCall(RF_SET_LIE, {slot}, text).value == 1;
  if (slot < 0 || slot >= MAX_PLAYERS || !text) return false;
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return false;
  const bool ok = false && text[0];  // 3.1-dev.88: the Shadow Age's lie is gone (Deep Cover keeps its story)
  if (ok) {
    size_t n = 0;
    for (size_t k = 0; text[k] && n < ERA_LIE_LEN; k++) { const char ch = text[k]; if (ch >= 32 && ch < 127) state.lie[n++] = ch; }
    state.lie[n] = '\0';
    state.lieRound = (int8_t)state.round;
  }
  const bool res = ok && state.lie[0];
  xSemaphoreGive(stateMutex);
  return res;
}

// At a round start: an Era that ran out ends; one in force does its round's work.
static void eraRoundStartLocked() {
  if (state.eraFaction >= 0 && state.round > state.eraUntil) {
    newsPubLocked(NEWS_ERA_END, state.eraFaction, -1, -1);
    state.eraFaction = state.eraBy = state.eraFrom = state.eraUntil = -1;
    state.markSlot = -1;
  }
  const int e = snapshotEra(state);
  if (e == FACTION_EMPIRE) {
    int collected = 0;
    const int champ = state.champion[FACTION_EMPIRE];
    for (int i = 0; i < MAX_PLAYERS; i++) {
      Player &pl = state.players[i];
      if (!pl.active) continue;
      // 3.1-dev.88 THE TAX ROLLS: 1 CR per world held past the 2nd -- everyone, the champion too (Imperial records are gone)
      int held = 0;
      for (int p = 0; p < NUM_PLANETS; p++) if (state.planetOwner[p] == i) held++;
      const int tax = min((int)pl.credits, max(0, held - ERA_TAX_FREE) * ERA_TAX_PER_WORLD);
      if (tax > 0) { pl.credits -= tax; collected += tax; pnewsLocked(i, PN_TAX_PAID, FACTION_EMPIRE, -1, tax); }
    }
    if (champ >= 0 && collected / 2 > 0) { state.players[champ].credits += collected / 2; pnewsLocked(champ, PN_TAX_COLLECTED, FACTION_EMPIRE, -1, collected / 2); }
  }
  if (e == FACTION_REBEL)  // HOPE SPREADS: every player repairs a ship free
    for (int i = 0; i < MAX_PLAYERS; i++) if (state.players[i].active) repairShipsLocked(i, 1);
  if (e == FACTION_CRIMSON_DAWN) { state.lieRound = -1; state.lie[0] = '\0'; }  // a new round, a new lie
}

// ===========================================================================
// 3.1-dev.13 ONCE-A-GAME MOVES. Each class has one, usable once, from round 3.
static void summitBreakLocked(int slot, int planet);
static void coverBlownLocked(int slot);
static bool moveReadyLocked(int slot, int cls) {
  if (slot < 0 || slot >= MAX_PLAYERS || state.stage != STAGE_PLAYING) return false;
  const Player &pl = state.players[slot];
  return pl.active && !pl.moveUsed && state.round >= MOVE_FROM_ROUND && classOfLocked(slot) == cls;
}
bool snapshotSummitCovers(const GameState &gs, int p) {
  if (gs.summitBy < 0 || gs.summitBroken || gs.round > gs.summitUntil) return false;
  for (int k = 0; k < SUMMIT_WORLDS; k++) if (gs.summitWorld[k] == p) return true;
  return false;
}
bool gameStateBreak(int slot, int planet) {
  if (slot < 0 || slot >= MAX_PLAYERS || planet < 0 || planet >= NUM_PLANETS) return false;
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return false;
  state.players[slot].brkPlanet = (int8_t)planet; state.players[slot].brkRound = (int8_t)state.round;
  xSemaphoreGive(stateMutex);
  return true;
}
// 3.1-dev.93 WAR SPOILS (user 2026-11-20: the Warlord "could almost never be a champion" -- fights after mid-game are against
// players, which move no favor): every world a Warlord takes BY FORCE, from anyone, earns +CLASS_WARLORD_SPOILS with their OWN
// faction -- unless the world leans to that faction (that's hitting your own side). Never in the last round (favor is frozen).
static void warSpoilsLocked(int atk, int fIdx, int p) {
  // 3.1-dev.101 NO STOMACH FOR WAR (the Consul's weakness now): a world taken by force costs more favor with its faction.
  if (atk >= 0 && atk < MAX_PLAYERS && classOfLocked(atk) == CLASS_CONSUL && !snapshotFhFinal(state) && fIdx >= 0 && fIdx < NUM_FACTIONS) {
    applyReputationLocked(atk, fIdx, -CONSUL_FORCE_FAVOR, false, TXC_RIVAL, p);
    stampLocked(atk, STAMP_SOFT, p);
  }
  if (atk < 0 || atk >= MAX_PLAYERS || classOfLocked(atk) != CLASS_WARLORD || snapshotFhFinal(state)) return;
  const int home = state.players[atk].chosenFaction;
  if (home < 0 || home >= NUM_FACTIONS || home == fIdx) return;
  applyReputationLocked(atk, home, CLASS_WARLORD_SPOILS, false, TXC_RIVAL, p);
  stampLocked(atk, STAMP_SPOILS, p);
}
// A world taken in a fight that isn't a battle screen (the War Room): the same consequences as a normal capture.
static void conquerLocked(int atk, int p) {
  g_takeHow = TAKE_FIGHT;
  const bool fromPlayer = state.planetOwner[p] >= 0 && state.planetOwner[p] != atk;  // 3.1-dev.36 (see the battle capture)
  setOwnerLocked(p, atk);
  promiseOnBattleWinLocked(atk);
  promiseOnGainLocked(atk, p);
  const int fIdx = factionIndexForPlanet(p);
  uint8_t bigs = gainBigsLocked(p, atk);
  if (reputationTierLocked(state.players[atk].reputation[fIdx]) >= REP_FAVORABLE) bigs |= 1u << TXB_BETRAYAL;
  if (!state.firstBloodDone) bigs |= 1u << TXB_FIRSTBLOOD;
  state.firstBloodDone = true;
  warSpoilsLocked(atk, fIdx, p);  // 3.1-dev.93
  if (fromPlayer) { applyReputationLocked(atk, fIdx, 0, false, TXC_TOOK, p, bigs); return; }  // 3.1-dev.36: from a PLAYER -- no favor moves (big moments still play)
  applyReputationLocked(atk, fIdx, REP_CONQUEST, true, TXC_TOOK, p, bigs);
  const bool muscle = classOfLocked(atk) == CLASS_WARLORD;  // 3.1-dev.47 THE MUSCLE
  applyReputationLocked(atk, rivalFactionIndex(fIdx), muscle ? CLASS_WARLORD_RIVAL : REP_RIVAL_GAIN, false, TXC_RIVAL, p);
  if (muscle) stampLocked(atk, STAMP_FEARED, p);
}

// ---- WARLORD: DECLARE WAR -> a round's warning -> the WAR ROOM ----
bool gameStateDeclareWar(int slot, int p) {
  if (g_remote || p < 0 || p >= NUM_PLANETS) return false;
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return false;
  // 3.1-dev.88 WARPATH (user: the 2-round war = "too much complexity"; "+3 is great"): name a world in PREPARE -- the table
  // announces it, everyone reacts in Movement -- and your attack on it this Resolution gets WARPATH_SHIPS more; every
  // ready fleet can join (the usual combined assault). No War Room, no pulling defenders.
  // 3.1-dev.89 (user: "warpath should work whenever. everyone elses works whenever"): any phase; the strike is THIS round's Resolution
  bool ok = moveReadyLocked(slot, CLASS_WARLORD) && state.warBy < 0
            && state.planetOwner[p] != slot && state.round < state.roundLimit - 1;  // 3.1-dev.43: a war never runs into the Final Hour
  if (ok) {
    Player &pl = state.players[slot];
    pl.moveUsed = 1;
    state.warBy = (int8_t)slot; state.warWorld = (int8_t)p; state.warRound = (int8_t)state.round;
    newsPubLocked(NEWS_WAR, pl.chosenFaction, p, slot);
    newsTableLocked(NEWS_WAR, pl.chosenFaction, p, -1);
    const int o = state.planetOwner[p];
    if (o >= 0 && o < MAX_PLAYERS) pnewsLocked(o, PN_WAR_WARN, pl.chosenFaction, p, slot);
    Serial.printf("[MOVE] P%d is on the warpath to %s (round %d)\n", slot + 1, PLANETS[p].name, state.warRound);
  }
  xSemaphoreGive(stateMutex);
  return ok;
}
bool gameStateWarPullBack(int slot) {
  if (g_remote || !stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return false;
  const bool ok = state.warBy == slot && slot >= 0;
  if (ok) { state.players[slot].moveUsed = 0; state.warBy = state.warWorld = state.warRound = -1; }  // the declaration comes back
  xSemaphoreGive(stateMutex);
  return ok;
}
bool gameStateWarRoom(int slot, const int8_t *pull, bool march) {
  if (g_remote || !pull) return false;
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return false;
  bool ok = false;  // 3.1-dev.90: the War Room is gone (WARPATH attacks through a normal battle)
  (void)march;
  int total = 0;
  const int w = state.warWorld;
  for (int p = 0; ok && p < NUM_PLANETS; p++) {
    if (pull[p] <= 0) continue;
    if (state.planetOwner[p] != slot || p == w || pull[p] > state.planetDefense[p] - 1) ok = false;
    total += pull[p];
  }
  if (ok && (total < 1 || total > WAR_CAP)) ok = false;
  if (ok && snapshotSummitCovers(state, w) && !march) ok = false;
  if (ok) {
    const bool keepers = snapshotSummitCovers(state, w);
    if (keepers) summitBreakLocked(slot, w);
    for (int p = 0; p < NUM_PLANETS; p++) if (pull[p] > 0) state.planetDefense[p] -= pull[p];
    int atk = total + WAR_BONUS, def = state.planetDefense[w] + (keepers ? SUMMIT_KEEPERS : 0);
    const int o = state.planetOwner[w];
    if (o >= 0 && o < MAX_PLAYERS && o != slot && classOfLocked(o) == CLASS_INFILTRATOR) atk++;  // Spread thin
    state.warLastA = (uint8_t)atk; state.warLastD = (uint8_t)def; state.warLastN = 0;
    while (atk > 0 && def > 0 && state.warLastN < WAR_LOG_LEN) {
      const int a = 1 + (int)(esp_random() % 6), d = 1 + (int)(esp_random() % 6);
      state.warLog[state.warLastN++] = (uint8_t)((a << 4) | d);
      if (a > d) def--; else atk--;
    }
    if (atk > 0 && def > 0) def = 0;  // (never happens: 40 exchanges is more than any war)
    const bool won = def <= 0;
    // survivors go home (each world back up to what it gave); the world taken keeps what's left over, at least 1
    int back = min(atk, total);
    for (int p = 0; p < NUM_PLANETS && back > 0; p++) if (pull[p] > 0) { const int r = min((int)pull[p], back); state.planetDefense[p] += r; back -= r; }
    if (won) {
      conquerLocked(slot, w);
      state.planetDefense[w] = max(1, atk - min(atk, total));
    } else state.planetDefense[w] = max(1, def - (keepers ? min(def, SUMMIT_KEEPERS) : 0));
    state.warLastBy = (int8_t)slot; state.warLastWorld = (int8_t)w; state.warLastWon = won ? 1 : 0;
    const int fac = state.players[slot].chosenFaction;
    newsPubLocked(won ? NEWS_WAR_WON : NEWS_WAR_LOST, fac, w, slot);
    newsTableLocked(won ? NEWS_WAR_WON : NEWS_WAR_LOST, fac, w, -1);
    state.warBy = state.warWorld = state.warRound = -1;
    Serial.printf("[MOVE] War Room: P%d %s %s (%d exchanges)\n", slot + 1, won ? "took" : "failed at", PLANETS[w].name, state.warLastN);
  }
  xSemaphoreGive(stateMutex);
  return ok;
}

// ---- CONSUL: THE SUMMIT ----
bool gameStateCallSummit(int slot, const int *worlds, int n) {
  if (g_remote || !worlds || n < 1 || n > SUMMIT_WORLDS) return false;
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return false;
  bool ok = moveReadyLocked(slot, CLASS_CONSUL) && state.summitBy < 0;
  for (int k = 0; ok && k < n; k++) {
    if (worlds[k] < 0 || worlds[k] >= NUM_PLANETS) ok = false;
    for (int j = 0; j < k; j++) if (worlds[j] == worlds[k]) ok = false;
  }
  if (ok) {
    state.players[slot].moveUsed = 1;
    state.summitBy = (int8_t)slot; state.summitUntil = (int8_t)(state.round + SUMMIT_ROUNDS - 1); state.summitBroken = 0;
    for (int k = 0; k < SUMMIT_WORLDS; k++) state.summitWorld[k] = (int8_t)(k < n ? worlds[k] : -1);
    const int fac = state.players[slot].chosenFaction;
    newsPubLocked(NEWS_SUMMIT, fac, worlds[0], slot);
    newsTableLocked(NEWS_SUMMIT, fac, worlds[0], -1);
    if (state.warBy >= 0 && snapshotSummitCovers(state, state.warWorld)) pnewsLocked(state.warBy, PN_SUMMIT_ON_WAR, fac, state.warWorld, slot);
  }
  xSemaphoreGive(stateMutex);
  return ok;
}
static void summitBreakLocked(int slot, int planet) {  // mutex held
  if (state.summitBy < 0 || state.summitBroken) return;
  state.summitBroken = 1;
  const int rival = rivalFactionIndex(planetFaction(planet));
  for (int f = 0; f < NUM_FACTIONS; f++) if (f != rival) applyReputationLocked(slot, f, SUMMIT_BREAK_FAVOR, false, TXC_DEAL, planet);
  newsPubLocked(NEWS_SUMMIT_BROKEN, state.players[slot].chosenFaction, planet, slot);
  newsTableLocked(NEWS_SUMMIT_BROKEN, state.players[slot].chosenFaction, planet, -1);
}

// ---- INFILTRATOR: DEEP COVER ----
bool gameStateDeepCover(int slot, int f) {
  if (g_remote || f < 0 || f >= NUM_FACTIONS) return false;
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return false;
  bool ok = moveReadyLocked(slot, CLASS_INFILTRATOR) && f != state.players[slot].chosenFaction;
  if (ok) {
    Player &pl = state.players[slot];
    pl.moveUsed = 1;
    pl.coverFaction = (int8_t)f; pl.coverUntil = (int8_t)(state.round + COVER_ROUNDS - 1);
    pl.coverSaved = (int16_t)pl.reputation[f];
    pl.reputation[f] = max(pl.reputation[f], COVER_SCORE);
    pnewsLocked(slot, PN_COVER_ON, f, -1, pl.coverUntil);
  }
  xSemaphoreGive(stateMutex);
  return ok;
}
static void coverEndLocked(int slot, int extra) {  // the real favor comes back (plus whatever moved while under, plus extra)
  Player &pl = state.players[slot];
  const int f = pl.coverFaction;
  if (f < 0) return;
  const int moved = pl.reputation[f] - COVER_SCORE;
  pl.coverFaction = pl.coverUntil = -1;
  pl.reputation[f] = constrain(pl.coverSaved + max(0, moved) + extra, REP_MIN, REP_MAX);
  championUpdateLocked();
}
static void coverBlownLocked(int slot) {
  const int f = state.players[slot].coverFaction;
  if (f < 0) return;
  coverEndLocked(slot, COVER_BLOWN);
  newsPubLocked(NEWS_COVER_BLOWN, f, -1, slot);
  newsTableLocked(NEWS_COVER_BLOWN, f, -1, state.players[slot].chosenFaction);
}
bool gameStateSetStory(int slot, const char *text) {
  if (g_remote || slot < 0 || slot >= MAX_PLAYERS || !text) return false;
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return false;
  Player &pl = state.players[slot];
  bool ok = pl.coverFaction >= 0 && !pl.storyUsed && state.phase != PHASE_RESOLUTION;
  if (ok) {
    size_t n = 0;
    for (size_t k = 0; text[k] && n < STORY_LEN; k++) { const char ch = text[k]; if (ch >= 32 && ch < 127) pl.story[n++] = ch; }
    pl.story[n] = '\0';
    ok = n > 0;
    if (ok) { pl.storyUsed = 1; pl.storyRound = (int8_t)state.round; }  // airs in this round's Movement
  }
  xSemaphoreGive(stateMutex);
  return ok;
}

// ---- SCOUNDREL: THE HEIST ----
static int freeGoodLocked(int slot, int f) {  // a good from faction f's stall that needs no target; applied; -1 if none
  int cand[3], n = 0;
  for (int k = 0; k < 3; k++) {
    const int g = FACTION_GOODS[f][k];
    if (g != GOOD_SURVEY && g != GOOD_AUDIENCE && goodUsableFor(state, slot, g, -1)) cand[n++] = g;
  }
  if (!n) return -1;
  const int g = cand[esp_random() % n];
  Player &pl = state.players[slot];
  switch (g) {
    case GOOD_FUEL: break;
    case GOOD_REPAIRS: repairShipsLocked(slot, 2); break;
    case GOOD_BLESSING: pl.nextBattleReroll++; break;
    case GOOD_MERCENARIES: pl.nextBattleMercs = (uint8_t)min(MERC_SHIPS_MAX, pl.nextBattleMercs + MERC_SHIPS); break;
    case GOOD_SMUGGLED: addShipLocked(slot); break;
    case GOOD_CARGO: pl.cargoDue += GOOD_CARGO_RETURN; break;
    case GOOD_HARBOR: pl.harborRound = (int8_t)state.round; break;
    case GOOD_BACTA: repairShipsLocked(slot, 1); break;
    case GOOD_PERMITS: pl.permitDiscount += GOOD_PERMITS_DISCOUNT; break;
    case GOOD_TROOPS: { const int w = weakestHeldWorld(state, slot); if (w >= 0) state.planetDefense[w]++; break; }
    default: break;
  }
  return g;
}
bool gameStateHeist(int slot, int f, bool bold) {
  if (g_remote || f < 0 || f >= NUM_FACTIONS) return false;
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return false;
  bool ok = moveReadyLocked(slot, CLASS_SCOUNDREL) && f != state.players[slot].chosenFaction;
  if (ok) {
    Player &pl = state.players[slot];
    pl.moveUsed = 1;
    int die = 0, good = -1, outcome;  // outcome: 0 smooth, 1 bold success, 2 bold caught
    if (!bold) { pl.credits += HEIST_SMOOTH_CR; good = freeGoodLocked(slot, f); outcome = 0; }
    else {
      die = 1 + (int)(esp_random() % 6);
      if (die >= HEIST_BOLD_MIN) { pl.credits += HEIST_BOLD_CR; good = freeGoodLocked(slot, f); addShipLocked(slot); outcome = 1;
                                   pl.heistPushF = (int8_t)f; pl.heistPushRound = (int8_t)state.round; }  // 3.1-dev.20: you may push your luck
      else { pl.credits += HEIST_FAIL_CR; applyReputationLocked(slot, f, HEIST_FAIL_FAVOR, false, TXC_DEAL, -1); outcome = 2; }
    }
    pnewsLocked(slot, PN_HEIST, f, die, (int8_t)(outcome * 16 + (good + 1)));
    newsPubLocked(NEWS_HEIST, f, -1, bold ? die : 0);  // never says who
    newsTableLocked(NEWS_HEIST, f, -1, -1);
    Serial.printf("[MOVE] P%d heist on faction %d: %s die %d outcome %d\n", slot + 1, f, bold ? "bold" : "smooth", die, outcome);
  }
  xSemaphoreGive(stateMutex);
  return ok;
}

// 3.1-dev.20 PUSH YOUR LUCK: after a bold heist that worked, roll once more -- 3-6 doubles the haul, 1-2 loses it.
bool gameStateHeistPush(int slot, bool push) {
  if (g_remote || slot < 0 || slot >= MAX_PLAYERS) return false;
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return false;
  Player &pl = state.players[slot];
  const int f = pl.heistPushF;
  const bool ok = f >= 0 && pl.heistPushRound == state.round;
  if (ok && push) {
    const int die = 1 + (int)(esp_random() % 6);
    if (die >= HEIST_BOLD_MIN) { pl.credits += HEIST_PUSH_CR; pnewsLocked(slot, PN_HEIST, f, die, 3 * 16); }
    else {
      pl.credits = max(0, (int)pl.credits - HEIST_BOLD_CR);
      applyReputationLocked(slot, f, HEIST_FAIL_FAVOR, false, TXC_DEAL, -1);
      pnewsLocked(slot, PN_HEIST, f, die, 4 * 16);
    }
  }
  pl.heistPushF = pl.heistPushRound = -1;
  xSemaphoreGive(stateMutex);
  return ok;
}
// 3.1-dev.20: pay a grudge off before it lands -- expensive, and they'll still be angry (the cooldown starts now).
bool gameStatePayOffGrudge(int slot) {
  if (g_remote || slot < 0 || slot >= MAX_PLAYERS) return false;
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return false;
  Player &pl = state.players[slot];
  const int f = pl.grudgeFaction;
  const bool ok = f >= 0 && pl.credits >= grudgePayoffCr(f);
  if (ok) {
    pl.credits -= grudgePayoffCr(f);
    pl.grudgeFaction = pl.grudgeRound = pl.grudgeTarget = -1;
    pl.lastStrike[f] = (int8_t)state.round;
    pnewsLocked(slot, PN_GRUDGE_PAID, f, -1, grudgePayoffCr(f));
    const int d = min(GRUDGE_PAY_FAVOR, REP_HOSTILE_MAX - (int)pl.reputation[f]);  // 3.1-dev.42 HALF A STEP: still tier 1
    if (d > 0) applyReputationLocked(slot, f, d, false, TXC_DEAL, -1);
  }
  xSemaphoreGive(stateMutex);
  return ok;
}

// 3.1-dev.42 MAKE AMENDS: what a deed needs. world < 0 = "is there any world that works" (for the phone's list).
static bool deedWorldOk(const GameState &gs, int slot, int f, int kind, int w) {
  if (w < 0 || w >= NUM_PLANETS || gs.planetOwner[w] != slot) return false;
  if (kind == DEED_SHIP) return f == FACTION_EMPIRE && gs.planetDefense[w] >= DEED_TROOPS + 1;   // Lend troops (home allowed)
  if (kind != DEED_WORLD || homeWorld(w)) return false;
  if (f == FACTION_REBEL) return gs.planetDefense[w] >= DEED_SHELTER + 1;                        // Shelter refugees
  return true;                                                                                   // Lockdown / Collateral / Safehouse talk
}
static const char *deedPossible(const GameState &gs, int slot, int kind, int world) {
  if (slot < 0 || slot >= MAX_PLAYERS || !gs.players[slot].active) return "You're not in this game.";
  const Player &pl = gs.players[slot];
  const int f = pl.grudgeFaction;
  if (f < 0) return "Nobody is coming for you.";
  if (kind < DEED_SHIP || kind > DEED_WORLD) return "Pick a deed.";
  if (kind == DEED_ROUND) return nullptr;
  const bool needWorld = kind == DEED_WORLD || f == FACTION_EMPIRE;
  if (needWorld) {
    if (world >= 0) return deedWorldOk(gs, slot, f, kind, world) ? nullptr : "That world can't do it.";
    for (int p = 0; p < NUM_PLANETS; p++) if (deedWorldOk(gs, slot, f, kind, p)) return nullptr;
    return kind == DEED_SHIP ? "No world of yours has 5 or more ships." : f == FACTION_REBEL ? "No world of yours (not your home) has 4 or more ships." : "You hold no world besides your home.";
  }
  if ((f == FACTION_REBEL || f == FACTION_CRIMSON_DAWN) && pl.loanFleet >= 0) return "Some of your ships are already away.";
  const int need = 1 + (f == FACTION_REBEL ? DEED_CELL_SHIPS : f == FACTION_PYKE ? DEED_HAULER_SHIPS : DEED_BLADE_SHIPS);  // the fleet keeps one
  for (int k = 0; k < NUM_FLEETS; k++) {
    int n = 0;
    for (int s = 0; s < pl.fleetShipCount[k] && s < FLEET_SHIPS; s++) if (pl.fleetShipAlive[k][s]) n++;
    if (n >= need) return nullptr;
  }
  return f == FACTION_REBEL ? "No fleet of yours has 4 or more ships." : "No fleet of yours has 3 or more ships.";
}
// the one deed on offer: the one picked with the warning, or (if you can't do it any more) the next one you can
int snapshotOfferedDeed(const GameState &gs, int slot) {
  if (slot < 0 || slot >= MAX_PLAYERS || gs.players[slot].grudgeFaction < 0) return -1;
  const int start = gs.players[slot].grudgeDeed < 0 ? 0 : gs.players[slot].grudgeDeed;
  for (int i = 0; i < 3; i++) if (!deedPossible(gs, slot, (start + i) % 3, -1)) return (start + i) % 3;
  return -1;
}
const char *snapshotDeedRefusal(const GameState &gs, int slot, int kind, int world) {
  if (kind != snapshotOfferedDeed(gs, slot)) return "That's not what they asked for.";
  return deedPossible(gs, slot, kind, world);
}
const char *gameStateGrudgeDeed(int slot, int kind, int world) {
  if (g_remote) return "Make amends from your phone.";
  if (slot < 0 || slot >= MAX_PLAYERS) return "You're not in this game.";
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return "Busy, try again.";
  const char *why = snapshotDeedRefusal(state, slot, kind, world);
  if (!why && (kind == DEED_WORLD || (kind == DEED_SHIP && state.players[slot].grudgeFaction == FACTION_EMPIRE)) && world < 0) why = "Pick a world.";
  if (!why) {
    Player &pl = state.players[slot];
    const int f = pl.grudgeFaction, r = state.round;
    int w = -1;
    if (kind == DEED_ROUND) { pl.noAtkRound = (int8_t)(state.phase == PHASE_RESOLUTION ? r + 1 : r); pl.noAtkF = (int8_t)f; }
    else if (kind == DEED_SHIP) {
      if (f == FACTION_EMPIRE) { w = world; state.planetDefense[w] -= DEED_TROOPS; }                       // Lend troops
      else {
        const int n = f == FACTION_REBEL ? DEED_CELL_SHIPS : f == FACTION_PYKE ? DEED_HAULER_SHIPS : DEED_BLADE_SHIPS;
        const int k = biggestFleetLocked(slot, n + 1);
        loseShipsLocked(slot, k, n);                                                                       // Hand over a hauler (for good) ...
        if (f != FACTION_PYKE) { pl.loanFleet = (int8_t)k; pl.loanN = (int8_t)n; pl.loanBack = (int8_t)(r + DEED_LOAN_ROUNDS); pl.loanKind = f == FACTION_CRIMSON_DAWN ? 2 : 1; }  // ... or lent
        w = -1;
      }
    } else {
      w = world;
      if (f == FACTION_EMPIRE) { pl.lockWorld = (int8_t)w; pl.lockUntil = (int8_t)(r + DEED_WORLD_ROUNDS - 1); }     // Lockdown
      else if (f == FACTION_REBEL) state.planetDefense[w] -= DEED_SHELTER;                                             // Shelter refugees
      else if (f == FACTION_PYKE) { pl.collatWorld = (int8_t)w; pl.collatUntil = (int8_t)(r + DEED_WORLD_ROUNDS); }   // Collateral: the next 2 incomes
      else newsPubLocked(NEWS_SAFEHOUSE, FACTION_CRIMSON_DAWN, w, state.planetDefense[w]);                             // Safehouse talk
    }
    pl.grudgeFaction = pl.grudgeRound = pl.grudgeTarget = -1;
    pl.lastStrike[f] = (int8_t)r;
    pnewsLocked(slot, PN_DEED_DONE, f, w, kind);
    sellOfferLocked(slot, rivalFactionIndex(f), SELL_DEED, w);  // 3.1-dev.48
    const int d = (REP_HOSTILE_MAX + 1) - (int)pl.reputation[f];  // a FULL STEP: out of tier 1
    if (d > 0) applyReputationLocked(slot, f, d, false, TXC_DEAL, -1);
    Serial.printf("[NEWS] P%d made amends with faction %d: deed kind %d (world %d)\n", slot + 1, f, kind, w);
  }
  xSemaphoreGive(stateMutex);
  return why;
}

// Round start: a war nobody fought lapses, a Summit ends (and pays if it held), cover runs out.
static void movesRoundStartLocked(bool *personal) {
  const int r = state.round;
  if (state.warBy >= 0 && r > state.warRound) { pnewsLocked(state.warBy, PN_WAR_LAPSED, state.players[state.warBy].chosenFaction, state.warWorld, 0); state.warBy = state.warWorld = state.warRound = -1; }
  if (state.summitBy >= 0 && r > state.summitUntil) {
    const int c = state.summitBy;
    if (!state.summitBroken) {
      for (int f = 0; f < NUM_FACTIONS; f++) applyReputationLocked(c, f, SUMMIT_DIVIDEND, false, TXC_DEAL, -1);
      pnewsLocked(c, PN_SUMMIT_DIVIDEND, state.players[c].chosenFaction, -1, 0);
      newsPubLocked(NEWS_SUMMIT_HELD, state.players[c].chosenFaction, state.summitWorld[0], c);
      if (personal) personal[c] = true;
    }
    state.summitBy = state.summitUntil = -1; state.summitBroken = 0;
    for (int k = 0; k < SUMMIT_WORLDS; k++) state.summitWorld[k] = -1;
  }
  for (int i = 0; i < MAX_PLAYERS; i++) {
    Player &pl = state.players[i];
    if (pl.coverFaction >= 0 && r > pl.coverUntil) { const int f = pl.coverFaction; coverEndLocked(i, 0); pnewsLocked(i, PN_COVER_END, f, -1, 0); if (personal) personal[i] = true; }
    pl.brkPlanet = pl.brkRound = -1;
  }
}

bool gameStateSendDiplomat(int slot, int p) {
  if (g_remote) return remoteCall(RF_SEND_DIPLOMAT, {slot, p}).value == 1;
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return false;
  bool ok = false;
  if (snapshotCanSendDiplomat(state, slot, p)) {
    Player &pl = state.players[slot];
    pl.credits -= DIPLOMAT_COST;
    pl.diplomatPlanet = (int8_t)p;
    pl.diplomatBack = (int8_t)(state.round + DIPLOMAT_ROUNDS);
    const int holder = state.planetOwner[p];
    if (holder >= 0 && holder < MAX_PLAYERS && holder != slot && state.players[holder].active)  // the holder always hears
      noteLocked(holder, "The %s sent a diplomat to your world %s.", factionName(pl.chosenFaction), PLANETS[p].name);
    ok = true;
  }
  xSemaphoreGive(stateMutex);
  return ok;
}

// ---------------------------------------------------------------------------
// DEALS (2.0, mockup "C . Deals"): Credits for one promise between two players,
// offered during Prepare. See the DealAsk notes in game_state.h.
// ---------------------------------------------------------------------------
static bool dealAskNeedsWorld(int ask) { return ask == DEAL_STAY_AWAY || ask == DEAL_MARKET_CUT; }

const char *snapshotDealSendRefusal(const GameState &gs, int slot, int dealIdx, int to, int give, int ask, int world, int askCr) {
  if (slot < 0 || slot >= MAX_PLAYERS || !gs.players[slot].active) return "You're not in this game.";
  if (gs.stage != STAGE_PLAYING || gs.phase != PHASE_INVEST) return "Deals are made during Prepare.";
  int payer = slot, promiser = to;
  if (dealIdx >= 0) {  // a counter
    if (dealIdx >= DEAL_SLOTS) return "That offer is gone.";
    const Deal &d = gs.deals[dealIdx];
    if (d.state != DEAL_OFFERED) return "That offer is gone.";
    if (d.turn != slot) return "It's their turn to answer.";
    if (d.counters >= DEAL_MAX_COUNTERS) return "No counters left -- accept or decline.";
    payer = d.payer;
    promiser = d.promiser;
  } else {
    if (to < 0 || to >= MAX_PLAYERS || to == slot || !gs.players[to].active) return "Pick who the offer is for.";
    bool room = false;
    for (int i = 0; i < DEAL_SLOTS; i++) {
      const Deal &d = gs.deals[i];
      if (d.state == DEAL_FREE) room = true;
      else if (d.state == DEAL_OFFERED && ((d.payer == slot && d.promiser == to) || (d.payer == to && d.promiser == slot)))
        return "You already have an offer open with them.";
    }
    if (!room) return "Too many deals in play -- wait for one to end.";
  }
  return "Deals are gone in 3.1 -- try a job, a Summit or a promise instead.";  // 3.1-dev.15 (vision doc: Deals = Cut)
  if (give < 0 || give > DEAL_MAX_GIVE) return "That's not an amount.";
  if (payer == slot && gs.players[slot].credits < give) return "You don't have that many Credits.";
  if (ask < 0 || ask >= DEAL_ASK_COUNT) return "Pick what you ask for.";
  if (dealAskNeedsWorld(ask) && (world < 0 || world >= NUM_PLANETS)) return "Pick a world.";
  if (ask == DEAL_STAY_AWAY && gs.planetOwner[world] == promiser) return "They already hold that world.";
  if (ask == DEAL_MARKET_CUT) return "Market cuts are gone.";  // 3.1-dev.9
  if (ask == DEAL_MARKET_CUT && gs.planetOwner[world] != promiser) return "They don't hold that world.";
  if (ask == DEAL_PAY_LATER && (askCr < 1 || askCr > DEAL_MAX_ASK_CR)) return "Ask for 1 to 20 Credits.";
  return nullptr;
}

int snapshotDealBlock(const GameState &gs, int slot, int planetIdx, int method, char *why, size_t whySize) {
  if (slot < 0 || slot >= MAX_PLAYERS || planetIdx < 0 || planetIdx >= NUM_PLANETS) return -1;
  for (int i = 0; i < DEAL_SLOTS; i++) {
    const Deal &d = gs.deals[i];
    if (d.state != DEAL_ACTIVE || d.promiser != slot) continue;
    const bool hit = (d.ask == DEAL_NO_ATTACK && method == TAKEOVER_FIGHT && gs.planetOwner[planetIdx] == d.payer)
                  || (d.ask == DEAL_STAY_AWAY && d.world == planetIdx);
    if (!hit) continue;
    if (why) {
      const int f = gs.players[d.payer].chosenFaction;
      if (d.ask == DEAL_NO_ATTACK) snprintf(why, whySize, "You gave your word to the %s: no attacks on their worlds until round %d.", factionName(f), d.untilRound);
      else snprintf(why, whySize, "You gave your word to the %s: you stay away from %s until round %d.", factionName(f), PLANETS[planetIdx].name, d.untilRound);
    }
    return i;
  }
  return -1;
}

int snapshotDealsWaiting(const GameState &gs, int slot) {
  int n = 0;
  for (int i = 0; i < DEAL_SLOTS; i++) if (gs.deals[i].state == DEAL_OFFERED && gs.deals[i].turn == slot) n++;
  return n;
}

int gameStateDealSend(int slot, int dealIdx, int to, int give, int ask, int world, int askCr, int line, const char *msg) {
  if (g_remote) return remoteCall(RF_DEAL_SEND, {slot, dealIdx, to, give, (ask & 0xFF) | ((world & 0xFF) << 8) | ((askCr & 0xFF) << 16), line}).value;
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return -1;
  int k = -1;
  if (!snapshotDealSendRefusal(state, slot, dealIdx, to, give, ask, world, askCr)) {
    k = dealIdx;
    if (k < 0) {
      for (int i = 0; i < DEAL_SLOTS && k < 0; i++) if (state.deals[i].state == DEAL_FREE) k = i;
      Deal &d = state.deals[k];
      d = Deal();
      d.state = DEAL_OFFERED;
      d.payer = (int8_t)slot;
      d.promiser = (int8_t)to;
    } else {
      state.deals[k].counters++;
    }
    Deal &d = state.deals[k];
    d.ask = (uint8_t)ask;
    d.world = dealAskNeedsWorld(ask) ? (int8_t)world : -1;
    d.give = (uint8_t)give;
    d.askCr = ask == DEAL_PAY_LATER ? (uint8_t)askCr : 0;
    d.line = (line >= 0 && line < dealLineCount()) ? (int8_t)line : -1;
    d.msg[0] = '\0';
    if (d.line < 0 && msg) {  // typed on a phone: printable ASCII only (the consoles' fonts), trimmed
      int n = 0;
      for (const char *c = msg; *c && n < DEAL_MSG_LEN; c++)
        if (*c >= 0x20 && *c <= 0x7E && !(n == 0 && *c == ' ')) d.msg[n++] = *c;
      while (n > 0 && d.msg[n - 1] == ' ') n--;
      d.msg[n] = '\0';
    }
    const int other = slot == d.payer ? d.promiser : d.payer;
    d.turn = (int8_t)other;
    state.dealSeq++;
    noteLocked(other, dealIdx < 0 ? "The %s sent you an offer. Open DEALS to answer." : "The %s countered your offer. Open DEALS to answer.",
               factionName(state.players[slot].chosenFaction));
  }
  xSemaphoreGive(stateMutex);
  return k;
}

bool gameStateDealAnswer(int slot, int dealIdx, bool accept) {
  if (g_remote) return remoteCall(RF_DEAL_ANSWER, {slot, dealIdx, accept ? 1 : 0}).value == 1;
  if (dealIdx < 0 || dealIdx >= DEAL_SLOTS || slot < 0 || slot >= MAX_PLAYERS) return false;
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return false;
  bool ok = false;
  Deal &d = state.deals[dealIdx];
  if (d.state == DEAL_OFFERED && (slot == d.payer || slot == d.promiser)) {
    const int other = slot == d.payer ? d.promiser : d.payer;
    const char *me = factionName(state.players[slot].chosenFaction);
    if (!accept) {
      noteLocked(other, d.turn == slot ? "The %s declined your offer." : "The %s withdrew their offer.", me);
      d = Deal();
      ok = true;
    } else if (d.turn == slot && state.phase == PHASE_INVEST && state.players[d.payer].credits >= d.give) {
      state.players[d.payer].credits -= d.give;
      state.players[d.promiser].credits += d.give;
      d.state = DEAL_ACTIVE;
      d.turn = -1;
      d.untilRound = (int8_t)(state.round + (d.ask == DEAL_PAY_LATER ? 1 : DEAL_ROUNDS));
      noteLocked(other, "The %s accepted your deal.", me);
      ok = true;
    }
    if (ok) state.dealSeq++;
  }
  xSemaphoreGive(stateMutex);
  return ok;
}

// ---------------------------------------------------------------------------
// THE ASHEN FLEET (EV_ASHEN; see game_state.h).
// ---------------------------------------------------------------------------
bool snapshotAshenAlive(const GameState &gs) { return gs.eventId == EV_ASHEN && gs.ashenHp > 0; }

static int fleetShipsLocked(const Player &pl, int f) {
  int n = 0;
  for (int s = 0; s < FLEET_SHIPS; s++) if (pl.fleetShipAlive[f][s]) n++;
  return n;
}

const char *snapshotAshenStrikeRefusal(const GameState &gs, int slot, int fleetMask) {
  if (!snapshotAshenAlive(gs)) return "The Ashen Fleet isn't here.";
  if (slot < 0 || slot >= MAX_PLAYERS || !gs.players[slot].active) return "You're not in this game.";
  if (gs.stage != STAGE_PLAYING || gs.phase != PHASE_RESOLUTION) return "Strikes happen in Resolution.";
  const Player &pl = gs.players[slot];
  if (gs.round <= pl.noAtkRound && pl.noAtkF >= 0 && pl.noAtkF < 4) return NO_ATTACK_WHY[(int)pl.noAtkF];  // 3.1-dev.42
  if (fleetMask <= 0) return "Pick at least one ready fleet.";
  for (int f = 0; f < NUM_FLEETS; f++) {
    if (!(fleetMask & (1 << f))) continue;
    if (pl.fleetSpent[f]) return "That fleet already fought this round.";
    if (fleetShipsLocked(pl, f) == 0) return "That fleet has no ships.";
  }
  return nullptr;
}

int gameStateAshenStrike(int slot, int fleetMask) {
  if (g_remote) return remoteCall(RF_ASHEN_STRIKE, {slot, fleetMask}).value;
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return -1;
  int dmg = -1;
  if (!snapshotAshenStrikeRefusal(state, slot, fleetMask)) {
    Player &pl = state.players[slot];
    int ships = 0, total = 0;
    for (int f = 0; f < NUM_FLEETS; f++) {
      if (!(fleetMask & (1 << f))) continue;
      const int n = fleetShipsLocked(pl, f);
      ships += n;
      for (int k = 0; k < n; k++) total += 1 + (int)(esp_random() % 6);  // every ship rolls one die
      pl.fleetSpent[f] = true;
    }
    dmg = min(total, (int)state.ashenHp);
    state.ashenHp -= dmg;
    state.ashenDmg[slot] = (uint8_t)min(255, state.ashenDmg[slot] + dmg);
    // It answers once per strike: 1-2 miss, 3-5 one hit, 6 two hits -- the same
    // whatever you send, so big joint strikes pay. Hits come off the struck
    // fleet with the most ships left.
    const int roll = 1 + (int)(esp_random() % 6);
    const int hits = roll <= 2 ? 0 : roll <= 5 ? 1 : 2;
    int lost = 0;
    for (int h = 0; h < hits; h++) {
      int best = -1, bestN = 0;
      for (int f = 0; f < NUM_FLEETS; f++)
        if ((fleetMask & (1 << f)) && fleetShipsLocked(pl, f) > bestN) { best = f; bestN = fleetShipsLocked(pl, f); }
      if (best < 0) break;
      for (int s = FLEET_SHIPS - 1; s >= 0; s--)
        if (pl.fleetShipAlive[best][s]) { pl.fleetShipAlive[best][s] = false; lost++; break; }
    }
    state.ashenLastSlot = (int8_t)slot;
    state.ashenLastShips = (uint8_t)ships;
    state.ashenLastDmg = (uint8_t)min(255, dmg);
    state.ashenLastRoll = (uint8_t)roll;
    state.ashenLastHits = (uint8_t)lost;
    state.ashenSeq++;
    Serial.printf("[ASHEN] P%d: %d ships, %d damage (%d HP left); it rolled %d, %d lost\n", slot + 1, ships, dmg, state.ashenHp, roll, lost);
    if (state.ashenHp <= 0) {  // sunk: VP for everyone who hurt it, a bit more for the most
      int topDmg = 0;
      bool tie = false;
      for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!state.ashenDmg[i]) continue;
        state.players[i].vp += ASHEN_VP;
        if (state.ashenDmg[i] > topDmg) { topDmg = state.ashenDmg[i]; tie = false; }
        else if (state.ashenDmg[i] == topDmg) tie = true;
      }
      for (int i = 0; i < MAX_PLAYERS; i++) {  // a tie for the most: each of them gets the extra
        if (!state.ashenDmg[i] || state.ashenDmg[i] != topDmg) continue;
        state.players[i].vp += ASHEN_TOP_VP;
        noteLocked(i, "The Ashen Fleet is gone. +%d VP -- you hit it hardest%s.", ASHEN_VP + ASHEN_TOP_VP, tie ? " (shared)" : "");
      }
      for (int i = 0; i < MAX_PLAYERS; i++)
        if (state.ashenDmg[i] && state.ashenDmg[i] != topDmg) noteLocked(i, "The Ashen Fleet is gone. +%d VP for your part in it.", ASHEN_VP);
      state.publicMoment = PUBLIC_ASHEN_SUNK;
      state.publicActor = (int8_t)state.players[slot].chosenFaction;  // the last blow
      state.publicVictim = state.publicPlanet = -1;
      state.publicSeq++; evMomentLocked();
    }
  }
  xSemaphoreGive(stateMutex);
  return dmg;
}

// Prepare is over: offers nobody answered lapse.
static void dealsPrepareEndLocked() {
  for (int i = 0; i < DEAL_SLOTS; i++)
    if (state.deals[i].state == DEAL_OFFERED) { state.deals[i] = Deal(); state.dealSeq++; }
}

// A new round: Credits owed are paid, promises that ran their course end.
static void dealsRoundStartLocked() {
  for (int i = 0; i < DEAL_SLOTS; i++) {
    Deal &d = state.deals[i];
    if (d.state != DEAL_ACTIVE || state.round < d.untilRound) continue;
    if (d.ask == DEAL_PAY_LATER) {  // paid automatically -- as much as they have
      Player &from = state.players[d.promiser];
      const int amt = min((int)d.askCr, from.credits);
      from.credits -= amt;
      state.players[d.payer].credits += amt;
      noteLocked(d.payer, "The %s paid you %d CR, as agreed.", factionName(from.chosenFaction), amt);
    }
    d = Deal();
    state.dealSeq++;
  }
}

// DEAL_MARKET_CUT: who a world's market cut goes to right now.
static int dealCutReceiverLocked(int planetIdx, int holder) {
  for (int i = 0; i < DEAL_SLOTS; i++) {
    const Deal &d = state.deals[i];
    if (d.state == DEAL_ACTIVE && d.ask == DEAL_MARKET_CUT && d.world == planetIdx && d.promiser == holder) return d.payer;
  }
  return holder;
}

void snapshotDealAskText(const GameState &gs, int dealIdx, int viewer, char *out, size_t outSize) {
  out[0] = '\0';
  if (dealIdx < 0 || dealIdx >= DEAL_SLOTS) return;
  const Deal &d = gs.deals[dealIdx];
  if (d.state == DEAL_FREE) return;
  const bool promising = viewer == d.promiser;
  const char *payerName = factionName(gs.players[d.payer].chosenFaction);
  const char *w = (d.world >= 0 && d.world < NUM_PLANETS) ? PLANETS[d.world].name : "?";
  char until[24];
  if (d.state == DEAL_ACTIVE) snprintf(until, sizeof(until), " until round %d", d.untilRound);
  else snprintf(until, sizeof(until), " for %d rounds", DEAL_ROUNDS);
  switch (d.ask) {
    case DEAL_NO_ATTACK:
      if (promising) snprintf(out, outSize, "You don't attack the %s's worlds%s", payerName, until);
      else snprintf(out, outSize, "They don't attack your worlds%s", until);
      break;
    case DEAL_STAY_AWAY:
      snprintf(out, outSize, "%s stay away from %s%s", promising ? "You" : "They", w, until);
      break;
    case DEAL_PAY_LATER:
      if (promising) snprintf(out, outSize, "You send them %d CR next round", d.askCr);
      else snprintf(out, outSize, "They send you %d CR next round", d.askCr);
      break;
    case DEAL_MARKET_CUT:
      if (promising) snprintf(out, outSize, "Your market cut from %s goes to them%s", w, until);
      else snprintf(out, outSize, "Their market cut from %s goes to you%s", w, until);
      break;
    default: break;
  }
}

void gameStateInit() {
  stateMutex = xSemaphoreCreateMutex();
  loadDecks();  // v0.23.0: transmission decks continue where the last game left off
  resetGalaxyLocked();
}

void gameStateReputationScores(int slot, int out[NUM_FACTIONS]) {
  for (int f = 0; f < NUM_FACTIONS; f++) out[f] = 0;
  if (slot < 0 || slot >= MAX_PLAYERS || !stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return;
  for (int f = 0; f < NUM_FACTIONS; f++) out[f] = state.players[slot].reputation[f];
  xSemaphoreGive(stateMutex);
}

// ---- 2.0-dev.13 DROP OUT / REJOIN (board O) ----
// A player whose phone or console goes quiet for AWAY_AFTER_MS (or who taps
// STEP AWAY) is AWAY: held locked in, so no phase waits for them; their
// Credits, worlds and fleets hold. Any word from their device brings them
// back (STEP AWAY needs COME BACK); a new phone or a rebooted console can take
// an AWAY seat from the seat picker. The table's own clock -- not in GameState.
static uint32_t lastContactMs[MAX_PLAYERS];
static void advancePhaseLocked();

static void markAwayLocked(int slot, uint8_t how) {
  Player &pl = state.players[slot];
  if (pl.away) { if (how == AWAY_STEPPED) pl.away = how; return; }
  pl.away = how;
  pl.welcome = 0;
  pl.awayRound = (int16_t)state.round;
  pl.awayVp = (int16_t)pl.vp;
  pl.awayCr = (int16_t)pl.credits;
  pl.awayWorlds = 0;
  for (int p = 0; p < NUM_PLANETS; p++) if (state.planetOwner[p] == slot) pl.awayWorlds |= 1u << p;
  pl.lockedIn = true;  // AWAY counts as locked in
  Serial.printf("[AWAY] P%d %s\n", slot + 1, how == AWAY_STEPPED ? "stepped away" : "went quiet");
  if (state.stage == STAGE_PLAYING) advancePhaseLocked();
}

static void markBackLocked(int slot) {
  Player &pl = state.players[slot];
  lastContactMs[slot] = millis() ? millis() : 1;
  if (!pl.away) return;
  pl.away = AWAY_NONE;
  pl.lockedIn = false;  // back in the current phase
  pl.welcome = state.stage == STAGE_PLAYING ? 1 : 0;
  Serial.printf("[AWAY] P%d is back (left in round %d, now round %d)\n", slot + 1, pl.awayRound, state.round);
}

void gameStatePresent(int slot) {
  if (g_remote || slot < 0 || slot >= MAX_PLAYERS) return;
  lastContactMs[slot] = millis() ? millis() : 1;
  if (state.players[slot].away != AWAY_QUIET) return;  // the usual case: nothing to change, no lock
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return;
  if (state.players[slot].active && state.players[slot].away == AWAY_QUIET) markBackLocked(slot);
  xSemaphoreGive(stateMutex);
}

void gameStateStepAway(int slot, bool away) {
  if (g_remote) { remoteCall(RF_STEP_AWAY, {slot, away ? 1 : 0}); return; }
  if (slot < 0 || slot >= MAX_PLAYERS) return;
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return;
  if (state.players[slot].active && state.stage == STAGE_PLAYING) {
    if (away) markAwayLocked(slot, AWAY_STEPPED);
    else markBackLocked(slot);
  }
  xSemaphoreGive(stateMutex);
}

void gameStateWelcomeAck(int slot) {
  if (g_remote) { remoteCall(RF_WELCOME_ACK, {slot}); return; }
  if (slot < 0 || slot >= MAX_PLAYERS) return;
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return;
  state.players[slot].welcome = 0;
  xSemaphoreGive(stateMutex);
}

static void tableQueueTickLocked();
static void fhTickLocked();  // 3.1-dev.43 (the Final Hour, below)
void gameStateTick() {
  if (g_remote) return;  // the table does this
  static uint32_t overSince = 0;
  static int overExchange = -1;
  if (!stateMutex || xSemaphoreTake(stateMutex, 0) != pdTRUE) return;  // busy: try next time
  tableQueueTickLocked();  // 3.1-dev.15 the table's moment queue
  fhTickLocked();          // 3.1-dev.43 the finale plays out, then the game ends
  // 2.0-dev.13: who has gone quiet; an AWAY player in a battle -- a defender
  // fights on, an attacker withdraws.
  int autoSlot = -1, autoAction = 0;
  if (state.stage == STAGE_PLAYING) {
    const uint32_t now = millis();
    for (int i = 0; i < MAX_PLAYERS; i++) {
      if (!state.players[i].active) continue;
      if (!lastContactMs[i]) lastContactMs[i] = now ? now : 1;  // after a boot: give everyone the full time
      if (!state.players[i].away && now - lastContactMs[i] > AWAY_AFTER_MS) markAwayLocked(i, AWAY_QUIET);
    }
    if (state.battleActive && !state.battleOver) {
      const int a = state.battleAttackerSlot, d = state.battleDefenderSlot;
      if (a >= 0 && a < MAX_PLAYERS && state.players[a].away && (state.battleKind == BATTLE_PLANET || state.atkAction == 0)) { autoSlot = a; autoAction = 2; }
      else if (state.battleKind == BATTLE_PVP && d >= 0 && d < MAX_PLAYERS && state.players[d].away && state.defAction == 0) { autoSlot = d; autoAction = 1; }
    }
  }
  const bool over = state.battleActive && state.battleOver;
  const int kind = state.battleKind, attacker = state.battleAttackerSlot, ex = state.battleExchange;
  xSemaphoreGive(stateMutex);
  if (autoSlot >= 0) { gameStateBattleAction(autoSlot, autoAction); return; }
  if (!over) { overSince = 0; return; }
  if (!overSince || ex != overExchange) { overSince = millis() ? millis() : 1; overExchange = ex; return; }
  if (millis() - overSince < BATTLE_AUTO_CLOSE_MS) return;
  overSince = 0;
  Serial.println("[GAME] closing a finished battle nobody closed");
  if (kind == BATTLE_PVP) gameStateEndPvpBattle();
  else gameStateEndTakeoverAfterBattle(attacker);
}

// 3.1-dev.83: rebuild the state IN PLACE (PSRAM) -- never a GameState temporary on a task stack (the design notes, Known issues).
static void resetStateLocked() { state.~GameState(); new (&state) GameState(); }
void gameStateNewGame() {
  if (g_remote) { remoteCall(RF_NEW_GAME, {}); return; }
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return;
  if (state.stage == STAGE_GAME_OVER) {
    const int limit = state.roundLimit;
    const uint16_t seq = state.publicSeq;  // keeps counting, so no screen mistakes it for a new moment
    resetStateLocked();  // 3.1-dev.83 (was state = GameState(): a ~8 KB temporary on the 16 KB loopTask stack -- it overflowed at game end once GameState grew)
    state.roundLimit = limit;
    state.publicSeq = seq;
    resetGalaxyLocked();
    Serial.println("[GAME] new game: back to the Lobby");
  }
  xSemaphoreGive(stateMutex);
}

void gameStateEndGame() {
  if (g_remote) { remoteCall(RF_END_GAME, {}); return; }
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return;
  if (state.stage == STAGE_PLAYING) {
    const int limit = state.roundLimit;
    const uint16_t seq = state.publicSeq;
    resetStateLocked();  // 3.1-dev.83 (was state = GameState(): a ~8 KB temporary on the 16 KB loopTask stack -- it overflowed at game end once GameState grew)
    state.roundLimit = limit;
    state.publicSeq = seq;
    resetGalaxyLocked();
    Serial.println("[GAME] game ended early: back to the Lobby");
  }
  xSemaphoreGive(stateMutex);
  // gameStateSaveTick() sees the Lobby and removes the save.
}

// ---- Save states (v0.30.3) ----
// File = header + the raw GameState (plain data, no pointers). The header
// rejects a save from a build with a different GameState layout.
struct SaveHeader { uint32_t magic, format, size, hash; };
static const uint32_t SAVE_MAGIC = 0x56534653;  // "SFSV"
static const uint32_t SAVE_FORMAT = 17;  // 17 = 3.1-dev.20;  // 16 = 3.1-dev.17;  // 15 = 3.1-dev.16;  // 14 = 3.1-dev.13 once-a-game moves;  // 13 = 3.1-dev.12 champions + Eras;  // 12 = 3.1-dev.11 fixed strikes;  // 11 = 3.1-dev.10 Galaxy News, grudges, jobs;  // 10 = 3.1-dev.9 faction stalls;  // 9 = 3.1-dev.8 debts count at once + gifts;  // 8 = 3.1-dev.7 class counters + extras;  // 7 = 3.1-dev.3: the event log follows the state;  // 2.0: new GameState; 3 = 17 worlds (dev.12); 4 = 7-ship fleet arrays (dev.20); 5 = simpler VP (dev.23)
static const char *SAVE_PATH = "/game.sav";
static const char *SAVE_TMP = "/game.tmp";
static bool save_fs_ok = false, save_fs_tried = false;
static uint32_t save_last_hash = 0, save_last_ms = 0;
static bool save_exists = false;

static uint32_t fnv1a(const void *p, size_t n) {
  const uint8_t *b = (const uint8_t *)p;
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
  return h;
}
static bool saveFsBegin() {
  if (!save_fs_tried) {
    save_fs_tried = true;
    save_fs_ok = LittleFS.begin(true, "/save", 2, "spiffs");  // formats on first use
    Serial.printf("[SAVE] flash storage %s\n", save_fs_ok ? "ready" : "FAILED (saves off)");
  }
  return save_fs_ok;
}

void gameStateSaveTick() {
  if (g_remote || !stateMutex) return;
  if (millis() - save_last_ms < SAVE_EVERY_MS) return;
  save_last_ms = millis();
  if (!saveFsBegin()) return;
  static GameState &snap = *psNew<GameState>();  // 3.1-dev.3: PSRAM (was internal .bss)
  static EventLog &evsnap = *psNew<EventLog>();
  if (xSemaphoreTake(stateMutex, 0) != pdTRUE) return;  // busy: next time
  snap = state;
  memcpy((void *)&evsnap, (const void *)&evlog, sizeof(EventLog));
  xSemaphoreGive(stateMutex);
  if (snap.stage != STAGE_PLAYING) {
    if (save_exists || LittleFS.exists(SAVE_PATH)) {
      LittleFS.remove(SAVE_PATH);
      save_exists = false;
      save_last_hash = 0;
      Serial.println("[SAVE] no game running -- save removed");
    }
    return;
  }
  if (snap.battleActive) return;  // only quiet moments are saved
  for (int i = 0; i < MAX_PLAYERS; i++) snap.players[i].connected = false;  // not part of the game
  const uint32_t h = fnv1a(&snap, sizeof(snap)) ^ (fnv1a(&evsnap, sizeof(EventLog)) * 31u);
  if (h == save_last_hash) return;
  const uint32_t t0 = millis();
  File f = LittleFS.open(SAVE_TMP, "w");
  if (!f) { Serial.println("[SAVE] open failed"); return; }
  SaveHeader hd = { SAVE_MAGIC, SAVE_FORMAT, (uint32_t)(sizeof(GameState) + sizeof(EventLog)), h };
  // 3.1-dev.63: written 1 KB at a time with a short pause between -- a flash write pauses the whole chip, and one
  // ~400 ms burst held up every phone's poll; small bursts let the web server answer in between.
  auto put = [&f](const uint8_t *p, size_t n) {
    while (n) {
      const size_t k = n > 1024 ? 1024 : n;
      if (f.write(p, k) != k) return false;
      p += k; n -= k;
      if (n) vTaskDelay(pdMS_TO_TICKS(12));
    }
    return true;
  };
  const bool ok = put((const uint8_t *)&hd, sizeof(hd)) && put((const uint8_t *)&snap, sizeof(snap)) && put((const uint8_t *)&evsnap, sizeof(EventLog));
  f.close();
  if (!ok) { Serial.println("[SAVE] write failed"); LittleFS.remove(SAVE_TMP); return; }
  LittleFS.remove(SAVE_PATH);
  if (!LittleFS.rename(SAVE_TMP, SAVE_PATH)) { Serial.println("[SAVE] rename failed"); return; }
  save_last_hash = h;
  save_exists = true;
  Serial.printf("[SAVE] round %d phase %d saved (%u ms)\n", snap.round, snap.phase, (unsigned)(millis() - t0));
}

#ifndef SF_TWIN
static void saveTask(void *) {
  bool told = false;
  for (;;) {
    gameStateSaveTick();
    if (!told && save_exists) {  // once: how much of its stack the first save used
      told = true;
      Serial.printf("[SAVE] save task stack spare %u bytes\n", (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    }
    vTaskDelay(pdMS_TO_TICKS(250));
  }
}
#endif
void gameStateSaveTaskStart() {
#ifndef SF_TWIN
  static int mode = 0;  // 0 = not started, 1 = its own task, 2 = couldn't start it: save from loop() as before
  if (mode == 0) {
    // Its stack must be internal RAM (flash writes switch the PSRAM cache off).
    mode = xTaskCreate(saveTask, "save", SAVE_TASK_STACK, nullptr, 1, nullptr) == pdPASS ? 1 : 2;
    if (mode == 2) Serial.println("[SAVE] couldn't start the save task -- saving from loop()");
  }
  if (mode == 2) gameStateSaveTick();
#else
  gameStateSaveTick();
#endif
}

bool gameStateRestoreSaved() {
  if (g_remote || !stateMutex || !saveFsBegin()) return false;
  const char *path = LittleFS.exists(SAVE_PATH) ? SAVE_PATH : (LittleFS.exists(SAVE_TMP) ? SAVE_TMP : nullptr);
  if (!path) return false;
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  static GameState &loaded = *psNew<GameState>();  // 3.1-dev.3: PSRAM (was internal .bss)
  static EventLog &evloaded = *psNew<EventLog>();
  SaveHeader hd;
  const bool ok = f.read((uint8_t *)&hd, sizeof(hd)) == sizeof(hd)
               && hd.magic == SAVE_MAGIC && hd.format == SAVE_FORMAT && hd.size == sizeof(GameState) + sizeof(EventLog)
               && f.read((uint8_t *)&loaded, sizeof(loaded)) == sizeof(loaded)
               && f.read((uint8_t *)&evloaded, sizeof(EventLog)) == sizeof(EventLog)
               && (fnv1a(&loaded, sizeof(loaded)) ^ (fnv1a(&evloaded, sizeof(EventLog)) * 31u)) == hd.hash
               && loaded.stage == STAGE_PLAYING;
  f.close();
  if (!ok) {
    Serial.printf("[SAVE] %s unusable (other firmware or damaged) -- ignored\n", path);
    LittleFS.remove(path);
    return false;
  }
  if (xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return false;
  state = loaded;
  memcpy((void *)&evlog, (const void *)&evloaded, sizeof(EventLog));
  xSemaphoreGive(stateMutex);
  save_last_hash = hd.hash;
  save_exists = true;
  Serial.printf("[SAVE] resumed the saved game: round %d of %d, phase %d\n", loaded.round, loaded.roundLimit, loaded.phase);
  return true;
}

GameState gameStateRead() {
  GameState copy;
  if (stateMutex && xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
    copy = state;
    xSemaphoreGive(stateMutex);
  }
  return copy;
}

void gameStateEventsInto(EventLog &out) {
  if (stateMutex && xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
    memcpy((void *)&out, (const void *)&evlog, sizeof(EventLog));
    xSemaphoreGive(stateMutex);
  }
}

void gameStateReadInto(GameState &out) {
  if (stateMutex && xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
    out = state;
    xSemaphoreGive(stateMutex);
  }
}

// ---- Lobby ----
bool gameStateSlotOpen(int slot) {
  if (slot < 0 || slot >= MAX_PLAYERS) return false;
  bool open = false;
  if (stateMutex && xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
    open = !state.players[slot].active;
    xSemaphoreGive(stateMutex);
  }
  return open;
}

int gameStateJoinSlot(const char *clientId, const char *name, int requestedSlot) {
  if (g_remote) return remoteCall(RF_JOIN_SLOT, {requestedSlot}, clientId).value;  // console link: forward
  int assigned = -1;
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return -1;

  // Reconnect: an existing active slot with this same client id always
  // resumes its OWN slot, regardless of what requestedSlot was sent this
  // time -- the picker only matters for a genuinely new join.
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (state.players[i].active && strcmp(state.players[i].id, clientId) == 0) {
      state.players[i].connected = true;
      if (state.players[i].away == AWAY_QUIET) markBackLocked(i);  // 2.0-dev.13
      else lastContactMs[i] = millis() ? millis() : 1;
      if (name && name[0]) strncpy(state.players[i].name, name, sizeof(state.players[i].name) - 1);
      assigned = i;
      break;
    }
  }
  // 2.0-dev.13: mid-game, a new phone or a rebooted console takes an AWAY seat back.
  if (assigned == -1 && state.stage == STAGE_PLAYING && requestedSlot >= 0 && requestedSlot < MAX_PLAYERS
      && state.players[requestedSlot].active && state.players[requestedSlot].away) {
    Player &pl = state.players[requestedSlot];
    strncpy(pl.id, clientId, sizeof(pl.id) - 1);
    pl.id[sizeof(pl.id) - 1] = '\0';
    pl.connected = true;
    markBackLocked(requestedSlot);
    assigned = requestedSlot;
  }
  // New join: claim the specifically requested slot, if it's valid and free.
  if (assigned == -1 && state.stage == STAGE_LOBBY
      && requestedSlot >= 0 && requestedSlot < MAX_PLAYERS
      && !state.players[requestedSlot].active) {
    Player &pl = state.players[requestedSlot];
    pl.active = true;
    pl.connected = true;
    strncpy(pl.id, clientId, sizeof(pl.id) - 1);
    snprintf(pl.name, sizeof(pl.name), "%s", (name && name[0]) ? name : "PLAYER");
    pl.chosenFaction = -1;
    pl.commander = -1;
    pl.credits = 12;
    pl.lockedIn = false;
    initPlayerFleets(pl);
    assigned = requestedSlot;
  }
  xSemaphoreGive(stateMutex);
  return assigned;
}

void gameStateSetConnected(int slot, bool connected) {
  if (slot < 0 || slot >= MAX_PLAYERS) return;
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return;
  if (state.players[slot].active) state.players[slot].connected = connected;
  xSemaphoreGive(stateMutex);
}

bool gameStateFactionTaken(int factionIdx, int excludeSlot) {
  if (factionIdx < 0 || factionIdx >= NUM_FACTIONS) return false;
  bool taken = false;
  if (stateMutex && xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
      if (i == excludeSlot) continue;
      if (state.players[i].active && state.players[i].chosenFaction == factionIdx) {
        taken = true;
        break;
      }
    }
    xSemaphoreGive(stateMutex);
  }
  return taken;
}

bool gameStateChooseFaction(int slot, int factionIdx) {
  if (g_remote) return remoteCall(RF_CHOOSE_FACTION, {slot, factionIdx}).value == 1;
  if (slot < 0 || slot >= MAX_PLAYERS || factionIdx < 0 || factionIdx >= NUM_FACTIONS) return false;
  bool ok = false;
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return false;
  Player &pl = state.players[slot];
  if (pl.active && state.stage == STAGE_LOBBY) {
    // Already claimed by someone else? Refuse. Re-picking your own current
    // choice (or picking for the first time) is always fine.
    bool takenByOther = false;
    for (int i = 0; i < MAX_PLAYERS; i++) {
      if (i == slot) continue;
      if (state.players[i].active && state.players[i].chosenFaction == factionIdx) {
        takenByOther = true;
        break;
      }
    }
    if (!takenByOther) {
      if (pl.chosenFaction != factionIdx) pl.commander = -1;  // v0.21.20: commanders belong to a faction
      pl.chosenFaction = factionIdx;
      // Your "name" is just your faction now -- no separate typed name.
      snprintf(pl.name, sizeof(pl.name), "%s", FACTION_DISPLAY_NAMES[factionIdx]);
      // Real starting reputation, set the moment a faction is claimed:
      // Allied with your own faction, Hostile with its rival, Neutral (the
      // default) with the other two. Overwritten cleanly if the player
      // changes their pick again before the game actually starts.
      for (int f = 0; f < NUM_FACTIONS; f++) pl.reputation[f] = 0;
      pl.reputation[factionIdx] = REP_START_OWN;                    // deep in Allied
      pl.reputation[rivalFactionIndex(factionIdx)] = REP_START_RIVAL; // deep in Hostile
      ok = true;
    }
  }
  xSemaphoreGive(stateMutex);
  return ok;
}

static void gameMoneyStartLocked();  // 3.1-dev.122 (the game record's money, below)
void gameStateStartGame() {
  if (g_remote) { remoteCall(RF_START_GAME, {}); return; }
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return;
  if (state.stage == STAGE_LOBBY) {
    // Every active player must have picked a faction first. The UI should
    // already be hiding/disabling START until this is true, so this is a
    // silent safety net, matching the pattern used elsewhere in this file
    // (e.g. gameStateSetRoundLimit ignoring an invalid value) rather than a
    // signaled error.
    bool everyoneReady = true;
    for (int i = 0; i < MAX_PLAYERS; i++) {
      if (state.players[i].active && (state.players[i].chosenFaction < 0 || state.players[i].commander < 0)) {
        everyoneReady = false;
        break;
      }
    }
    if (!everyoneReady) {
      xSemaphoreGive(stateMutex);
      return;
    }

    state.stage = STAGE_PLAYING;
    evResetLocked();
    state.phase = PHASE_INVEST;
    state.round = 1;
    {  // v0.25.2: who goes first -- a random seated player
      int seated[MAX_PLAYERS], n = 0;
      for (int i = 0; i < MAX_PLAYERS; i++) if (state.players[i].active) seated[n++] = i;
      state.firstPlayer = n ? seated[esp_random() % n] : -1;
    }
    // v0.23.0: no transmissions or big moments carry over from a last game.
    state.firstBloodDone = false;
    for (int i = 0; i < MAX_PLAYERS; i++) {
      state.txCount[i] = 0;
      state.bigMomentsDone[i] = 0;
      for (int q = 0; q < TX_QUEUE_LEN; q++) state.txQueue[i][q] = TxMsg();
      syncTxHeadLocked(i);
    }

    // Fresh-game resets for every planet (replay safety -- a device can
    // stay powered across multiple games without a reboot). Ownership and
    // defense reset for ALL 12 regardless of planetIncluded, since that
    // selection itself is a Lobby-time choice that persists into the game
    // and isn't touched here.
    shuffleGalaxyLocked();  // v0.25.4: a new galaxy every game -- BEFORE the garrisons, which depend on it
    for (int p = 0; p < NUM_PLANETS; p++) {
      state.planetOwner[p] = PLANET_UNCONTROLLED;
      state.planetVpPending[p] = 0;  // v0.30.1
      state.planetDefense[p] = planetStartGarrison(p);
    }

    for (int i = 0; i < MAX_PLAYERS; i++) {
      Player &pl = state.players[i];
      pl.lockedIn = false;
      // Defensive reset in case this ever runs more than once without a
      // full reboot -- everything about a fresh game should start clean,
      // faction/reputation aside (those were deliberately set during the
      // Lobby and are the whole point of picking a faction pre-game).
      pl.vp = 0;
      pl.credits = 12;
      pl.lastIncome = 0;
      pl.away = AWAY_NONE;  // 2.0-dev.13
      pl.welcome = 0;
      lastContactMs[i] = millis() ? millis() : 1;
      for (int p = 0; p < NUM_PLANETS; p++) {
        for (int c = 0; c < NUM_CHARACTERISTICS; c++)
          pl.revealed[p][c] = false;
        pl.hasLostAt[p] = false;
        pl.negLosses[p] = 0;  // v0.30.0
      }
      pl.purchases = 0;
      for (int o = 0; o < MAX_PENDING_OBLIGATIONS; o++) pl.obligations[o].active = false;
      for (int o = 0; o < PROMISE_HISTORY; o++) pl.promiseLog[o] = PromiseRecord();  // v0.28.3
      for (int f = 0; f < NUM_FLEETS; f++) pl.fleetSpent[f] = false;
      for (int p = 0; p < NUM_PLANETS; p++) pl.takeoverLocked[p] = false;
      if (pl.active) initPlayerFleets(pl);
      // 2.0: fresh per-game state, and you know which worlds are sympathetic to your own faction.
      pl.diplomatPlanet = pl.diplomatBack = pl.freeRevealRound = pl.silentRound = pl.harborRound = -1;
      pl.silentEarned = pl.nextBattleMercs = pl.nextBattleReroll = 0;
      pl.cargoDue = pl.permitDiscount = pl.rebelWins = pl.questDone = 0;
      pl.forceTakes = pl.peaceTakes = pl.peakCredits = 0;  // 3.1-dev.7
      pl.giftsGot = 0; pl.spyOn = -1;  // 3.1-dev.8
      pl.grudgeFaction = pl.grudgeRound = pl.grudgeTarget = -1; pl.lastWatch = -10;  // 3.1-dev.10
      pl.noAtkRound = pl.noAtkF = pl.lockWorld = pl.lockUntil = pl.collatWorld = pl.collatUntil = pl.loanFleet = pl.loanBack = pl.grudgeDeed = -1; pl.loanKind = 0;  // 3.1-dev.42 deeds
      pl.shakenMask = 0; pl.shakenRound = -1;  // 3.1-dev.76
      for (int f = 0; f < NUM_FACTIONS; f++) pl.deedNext[f] = (int8_t)(esp_random() % 3);  // where each faction starts its turn of deeds
      for (int f = 0; f < NUM_FACTIONS; f++) pl.lastStrike[f] = -10;
      pl.jobFaction = pl.jobWorld = pl.jobDue = -1; pl.jobState = pl.jobAnnounced = pl.jobsOffered = 0;
      pl.decrees = 0;  // 3.1-dev.12
      pl.heistPushF = pl.heistPushRound = -1;  // 3.1-dev.20
      pl.moveUsed = pl.storyUsed = 0; pl.coverFaction = pl.coverUntil = pl.storyRound = pl.brkPlanet = pl.brkRound = -1; pl.story[0] = '\0';  // 3.1-dev.13
      pl.note[0] = '\0';
      for (int p = 0; p < NUM_PLANETS; p++) {
        if (pl.chosenFaction >= 0 && state.planetFactionOf[p] == pl.chosenFaction) pl.revealed[p][0] = true;
        // 3.1-dev.6 OLD CONTACTS: a defector also knows their old faction's worlds.
        { const int from = pl.commander >= 0 && pl.commander < NUM_COMMANDERS ? CHARACTER_DEFECTOR_FROM[pl.commander] : -1;
          if (from >= 0 && state.planetFactionOf[p] == from) pl.revealed[p][0] = true; }
        if (characterClass(pl.commander) == CLASS_CONSUL) pl.revealed[p][0] = true;  // 3.1-dev.101 OLD FRIENDS: every world's Sympathy
        // 2.0-dev.31 (agreed 2026-11-04: "everyone knows the planet is swampy"): Resources
        // and Trait are public -- only Sympathy and Defense are bought.
        pl.revealed[p][1] = pl.revealed[p][2] = true;
      }
    }
    state.eventId = EV_NONE;
    state.eventRound = -1;
    for (int i = 0; i < DEAL_SLOTS; i++) state.deals[i] = Deal();
    state.ashenHp = state.ashenMaxHp = 0;
    state.ashenLastSlot = -1;
    for (int i = 0; i < MAX_PLAYERS; i++) state.ashenDmg[i] = 0;
    for (int p = 0; p < NUM_PLANETS; p++) state.planetUnbound[p] = 0;
    // 2.0 HOME WORLDS: every seated faction starts holding its home (+2 VP),
    // with the home guard; the others stay neutral.
    for (int f = 0; f < NUM_FACTIONS; f++) {
      const int hp = homePlanetOf(f), hs = homeSlotOf(state, f);
      if (hs >= 0) setOwnerLocked(hp, hs);
      state.planetDefense[hp] = planetStartGarrison(hp);
    }
    for (int p = 0; p < NUM_PLANETS; p++) { state.planetTakenRound[p] = 0; state.patrolUntil[p] = 0; state.patrolShips[p] = 0;  // 3.1-dev.76
                                             state.buyBy[p] = 0; state.buyCr[p] = 0; state.upriseFrom[p] = 0; }  // 3.1-dev.79
    state.fortMask = 0;
    gameMoneyStartLocked();  // 3.1-dev.122
    { // 3.1-dev.76 THE SQUEEZE (user: "When you have 3 players and you start in between two players you have a huge disadvantage"):
      // with one home empty, the home opposite it sits between the other two -- it starts with Fleets 2 and 3 at 2 ships each.
      static const int8_t OPPOSITE[NUM_FACTIONS] = { FACTION_CRIMSON_DAWN, FACTION_PYKE, FACTION_EMPIRE, FACTION_REBEL };
      int seated = 0, empty = -1, emptyN = 0;
      for (int f = 0; f < NUM_FACTIONS; f++) { if (homeSlotOf(state, f) >= 0) seated++; else { empty = f; emptyN++; } }
      const int sq = (seated == 3 && emptyN == 1) ? homeSlotOf(state, OPPOSITE[empty]) : -1;
      if (sq >= 0) {
        Player &pl = state.players[sq];
        for (int f = 1; f < NUM_FLEETS; f++) { pl.fleetShipCount[f] = SQUEEZE_FLEET_SHIPS; for (int s2 = 0; s2 < FLEET_SHIPS; s2++) pl.fleetShipAlive[f][s2] = s2 < SQUEEZE_FLEET_SHIPS; }
        noteLocked(sq, "You start between two rivals: Fleets 2 and 3 are already crewed, 2 ships each.");
        Serial.printf("[START] 3 players: P%d sits between the other two -- two extra fleets\n", sq + 1);
      }
    }
    for (int f = 0; f < NUM_FACTIONS; f++) state.champion[f] = -1;  // 3.1-dev.12
    state.warBy = state.warWorld = state.warRound = state.summitBy = state.summitUntil = state.warLastBy = state.warLastWorld = state.warLastWon = -1;  // 3.1-dev.13
    state.summitBroken = state.battleKeepers = state.warLastN = 0;
    state.eraFaction = state.eraBy = state.eraFrom = state.eraUntil = state.lieRound = -1; state.lie[0] = '\0';
    openMarketsLocked();
    newsRoundStartLocked(false);  // 3.1-dev.10: round 1 news
  }
  xSemaphoreGive(stateMutex);
}

void gameStateSetEvents(bool on) {  // 2.0-dev.33
  if (g_remote) { remoteCall(RF_SET_EVENTS, {on ? 1 : 0}); return; }
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return;
  if (state.stage == STAGE_LOBBY) state.eventsOn = on;
  xSemaphoreGive(stateMutex);
}

void gameStateSetMarkets(bool on) {  // 2.0-dev.32
  if (g_remote) { remoteCall(RF_SET_MARKETS, {on ? 1 : 0}); return; }
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return;
  if (state.stage == STAGE_LOBBY) state.marketsOn = on;
  xSemaphoreGive(stateMutex);
}

void gameStateSetRoundLimit(int rounds) {
  if (g_remote) { remoteCall(RF_SET_ROUND_LIMIT, {rounds}); return; }
  if (rounds != 6 && rounds != 8 && rounds != 10) return;
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return;
  if (state.stage == STAGE_LOBBY) state.roundLimit = rounds;
  xSemaphoreGive(stateMutex);
}

// ---- Intel / economy ----
int snapshotShipCost(const GameState &gs, int slot, bool offBoard) {
  int cost = FLEET_INVEST_COST;
  if (snapshotHolderTrait(gs, slot, TR_SHIPYARDS) > 0) cost = 1;
  if (offBoard && snapshotHolderTrait(gs, slot, TR_HIDDENBASE) > 0) cost = 1;
  return cost;
}

bool gameStateRevealCharacteristic(int slot, int planetIdx, int charIdx) {
  if (g_remote) return remoteCall(RF_REVEAL, {slot, planetIdx, charIdx}).value == 1;
  if (slot < 0 || slot >= MAX_PLAYERS) return false;
  if (planetIdx < 0 || planetIdx >= NUM_PLANETS) return false;
  if (charIdx < 0 || charIdx >= NUM_CHARACTERISTICS) return false;
  bool ok = false;
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return false;
  Player &pl = state.players[slot];
  if (pl.active && (!pl.revealed[planetIdx][charIdx] || (charIdx == 3 && snapshotDefSeenState(state, slot, planetIdx) > 0))) {  // 3.1-dev.101 LOOK AGAIN
    int cost = snapshotRevealCost(state, slot, planetIdx, charIdx);  // 2.0: TR_SPYNET makes one free a round
    if (pl.credits >= cost) {
      if (cost == 0) { pl.freeRevealRound = (int8_t)state.round; if (classOfLocked(slot) == CLASS_INFILTRATOR) stampLocked(slot, STAMP_FREELOOK, planetIdx); }
      pl.credits -= cost;
      pl.revealed[planetIdx][charIdx] = true;
      if (charIdx == 3) pl.defSeen[planetIdx] = (uint8_t)min(255, snapshotDefendersNow(state, planetIdx) + 1);  // 3.1-dev.101
      pl.intelEyed |= 1u << planetIdx;  // 2.0-dev.41: the Holonet hears about it
      ok = true;
    }
  }
  xSemaphoreGive(stateMutex);
  return ok;
}

bool gameStateInvestDefense(int slot, int planetIdx) {
  if (g_remote) return remoteCall(RF_INVEST_DEFENSE, {slot, planetIdx}).value == 1;
  if (slot < 0 || slot >= MAX_PLAYERS) return false;
  if (planetIdx < 0 || planetIdx >= NUM_PLANETS) return false;
  bool ok = false;
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return false;
  Player &pl = state.players[slot];
  if (pl.active && state.planetOwner[planetIdx] == slot
      && state.planetDefense[planetIdx] < snapshotDefenseLimit(state, planetIdx)  // 3.1-dev.76: Outer 6 / Mid 8 / Core 10
      && !(pl.lockWorld == planetIdx && state.round <= pl.lockUntil)  // 3.1-dev.42 Imperial lockdown
      && pl.credits >= 2) {
    pl.credits -= 2;                       // 2 Credits per extra garrison ship
    state.planetDefense[planetIdx] += 1;
    promiseOnInvestLocked(slot, planetIdx);  // v0.28.1
    ok = true;
  }
  xSemaphoreGive(stateMutex);
  return ok;
}

// 3.1-dev.76: each world's limit once held -- Outer 6 / Mid 8 / Core 10 (never below where it started: home guards are 6), +2 fortified.
int snapshotDefenseLimit(const GameState &gs, int p) {
  if (p < 0 || p >= NUM_PLANETS) return MAX_PLANET_DEFENSE;
  const int ring = PLANETS[p].ring == RING_CORE ? DEF_LIMIT_CORE : PLANETS[p].ring == RING_MID ? DEF_LIMIT_MID : DEF_LIMIT_OUTER;
  return min(MAX_PLANET_DEFENSE, max(ring, planetStartGarrison(p)) + (((gs.fortMask >> p) & 1) ? DEF_FORT_BONUS : 0));
}
int snapshotPatrolCost(const GameState &gs) { return snapshotEra(gs) == FACTION_EMPIRE ? (PATROL_CR * ERA_PATROL_PCT + 50) / 100 : PATROL_CR; }
// 3.1-dev.76 PATROL (user: "I like that it eventually goes away, so you have to maintain it"): two hired ships guard one of your
// worlds this round and the next. They fight first when it's attacked (after a militia) and the ones that survive stay on.
bool gameStatePatrol(int slot, int planetIdx) {
  if (g_remote || slot < 0 || slot >= MAX_PLAYERS || planetIdx < 0 || planetIdx >= NUM_PLANETS) return false;
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return false;
  Player &pl = state.players[slot];
  const int cost = snapshotPatrolCost(state);
  const bool ok = pl.active && state.phase == PHASE_INVEST && state.planetOwner[planetIdx] == slot && pl.credits >= cost
                  && !(state.patrolUntil[planetIdx] >= state.round && state.patrolShips[planetIdx] > 0);
  if (ok) {
    pl.credits -= cost;
    state.patrolShips[planetIdx] = PATROL_SHIPS;
    state.patrolUntil[planetIdx] = 120;  // 3.1-dev.101 (user: "a patrol should stay forever"): until it's beaten (ships at 0) or the world changes hands
    Serial.printf("[DEF] P%d hired a patrol for %s through round %d\n", slot + 1, PLANETS[planetIdx].name, state.patrolUntil[planetIdx]);
  }
  xSemaphoreGive(stateMutex);
  return ok;
}

bool gameStateInvestFleet(int slot, int fleetIdx) {
  if (g_remote) return remoteCall(RF_INVEST_FLEET, {slot, fleetIdx}).value == 1;
  if (slot < 0 || slot >= MAX_PLAYERS) return false;
  if (fleetIdx < 0 || fleetIdx >= NUM_FLEETS) return false;
  bool ok = false;
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return false;
  Player &pl = state.players[slot];
  // No living ships = the fleet isn't on the board: never founded, or
  // fully destroyed (v0.20.2 -- a destroyed fleet leaves the board until
  // it respawns). v0.28.2: a destroyed fleet rebuilds at your home hangar;
  // founding a never-built fleet still needs a planet you own.
  bool offBoard = (fleetStrengthLocked(slot, fleetIdx) == 0);
  // Respawning a destroyed fleet starts it fresh: its dead hulls are
  // cleared so it has full capacity again (otherwise a fleet that lost its
  // first 3 ships could only ever rebuild 3).
  bool rebuildDestroyed = offBoard && pl.fleetShipCount[fleetIdx] > 0;
  int capacityUsed = rebuildDestroyed ? 0 : pl.fleetShipCount[fleetIdx];
  // v0.27.3 REPAIR (playtest 2026-10-16: "I should be able to repair ships"):
  // a fleet still on the board buys back a lost ship first (same 2 CR,
  // anywhere -- user's pick), and only then commissions a new one.
  int deadSlot = -1;
  if (!offBoard)
    for (int s = 0; s < pl.fleetShipCount[fleetIdx]; s++) if (!pl.fleetShipAlive[fleetIdx][s]) { deadSlot = s; break; }
  // 2.0 traits: TR_SHIPYARDS makes every ship 1 CR; TR_HIDDENBASE makes (re)building an off-board fleet 1 CR.
  const int cost = snapshotShipCost(state, slot, offBoard);  // 3.1-dev.12: traits + the Spice Age
  if (pl.active && (deadSlot >= 0 || capacityUsed < fleetCapacityOf(pl)) && pl.credits >= cost
      ) {  // v0.28.4: every fleet is founded / rebuilt at your home hangar -- no planet needed (user 2026-10-19)
    if (rebuildDestroyed) {
      pl.fleetShipCount[fleetIdx] = 0;
      for (int s = 0; s < FLEET_SHIPS; s++) pl.fleetShipAlive[fleetIdx][s] = false;
    }
    pl.credits -= cost;
    if (deadSlot >= 0) {
      pl.fleetShipAlive[fleetIdx][deadSlot] = true;
    } else {
      int newSlot = pl.fleetShipCount[fleetIdx];
      pl.fleetShipAlive[fleetIdx][newSlot] = true;
      pl.fleetShipCount[fleetIdx] += 1;
      if (pl.fleetShipCount[fleetIdx] == FLEET_SHIPS && fleetCapacityOf(pl) == FLEET_SHIPS) stampLocked(slot, STAMP_RANKS);  // 3.1-dev.15 IMPERIAL RANKS
      // TR_TRAINING: a fleet you found or rebuild gets one extra ship free.
      if (offBoard && snapshotHolderTrait(state, slot, TR_TRAINING) > 0 && pl.fleetShipCount[fleetIdx] < fleetCapacityOf(pl)) {
        stampLocked(slot, STAMP_TRAINING);  // 3.1-dev.19
        pl.fleetShipAlive[fleetIdx][pl.fleetShipCount[fleetIdx]] = true;
        pl.fleetShipCount[fleetIdx] += 1;
      }
    }
    ok = true;
  }
  xSemaphoreGive(stateMutex);
  return ok;
}


// =====================================================================================================
// 3.1-dev.43 THE FINAL HOUR (user 2026-11-11/12). See the FH_* block in game_state.h.
// =====================================================================================================
// The map, for REACH (a strike needs no jump pass on a world you hold or one lane / ring step from it).
// Same positions + lanes as the phone's GALAXY_POS / GALAXY_LANES (web_server.cpp) -- keep them in step.
static const int16_t FH_POS[NUM_PLANETS][2] = { {-59,181},{59,-181},{0,124},{59,181},{0,-56},{-59,-181},{-154,112},{-154,-112},{-48,28},
                                                {88,88},{88,-88},{0,-124},{154,112},{154,-112},{-88,88},{-88,-88},{48,28} };
static const int8_t FH_LANES[][2] = { {3,2},{0,2},{5,11},{1,11},{12,9},{6,14},{7,15},{13,10},{11,4},{2,8},{2,16} };
static uint32_t fhAdj[NUM_PLANETS];
static uint32_t fhStationReach = 0;  // worlds next to a space station (east 0 deg, west 180 deg, on the Mid Rim)
// 3.1-dev.57 (user 2026-11-14: "the nebulae are stops"): The Maw (0 deg) and the Unknown Regions (180 deg) sit ON the Outer
// Rim as stops, so the two worlds either side of one are two steps apart, not neighbours.
static bool fhArcHitsNebula(float from, float d) {
  for (float at : {0.f, 180.f}) { float x = at - from; if (x <= 0) x += 360; if (x < d) return true; }
  return false;
}
static void fhAdjInit() {
  static bool done = false;
  if (done) return;
  done = true;
  for (int p = 0; p < NUM_PLANETS; p++) fhAdj[p] = 0;
  for (auto &l : FH_LANES) { fhAdj[l[0]] |= 1u << l[1]; fhAdj[l[1]] |= 1u << l[0]; }
  float ang[NUM_PLANETS];
  for (int p = 0; p < NUM_PLANETS; p++) { ang[p] = atan2f(FH_POS[p][1], FH_POS[p][0]) * 57.29578f; if (ang[p] < 0) ang[p] += 360; }
  for (int ring = 0; ring < 3; ring++) {  // ring neighbours: the next world round the ring each way
    for (int p = 0; p < NUM_PLANETS; p++) {
      if (PLANETS[p].ring != ring) continue;
      int nx = -1, pv = -1; float dn = 999, dp = 999;
      for (int q = 0; q < NUM_PLANETS; q++) {
        if (q == p || PLANETS[q].ring != ring) continue;
        float d = ang[q] - ang[p]; if (d < 0) d += 360;
        if (d < dn) { dn = d; nx = q; }
        if (360 - d < dp) { dp = 360 - d; pv = q; }
      }
      const bool outer = ring == RING_OUTER;
      if (nx >= 0 && !(outer && fhArcHitsNebula(ang[p], dn))) fhAdj[p] |= 1u << nx;
      if (pv >= 0 && !(outer && fhArcHitsNebula(ang[pv], dp))) fhAdj[p] |= 1u << pv;
    }
  }
  for (int s = 0; s < 2; s++) {  // a station sits on the Mid Rim between two worlds
    const float a = s ? 180.f : 0.f;
    int nx = -1, pv = -1; float dn = 999, dp = 999;
    for (int q = 0; q < NUM_PLANETS; q++) {
      if (PLANETS[q].ring != RING_MID) continue;
      float d = ang[q] - a; if (d < 0) d += 360;
      if (d < dn) { dn = d; nx = q; }
      if (360 - d < dp) { dp = 360 - d; pv = q; }
    }
    if (nx >= 0) fhStationReach |= 1u << nx;
    if (pv >= 0) fhStationReach |= 1u << pv;
  }
}
bool snapshotFhFinal(const GameState &gs) { return gs.stage == STAGE_PLAYING && gs.round >= gs.roundLimit; }
bool snapshotFhReach(const GameState &gs, int slot, int p) {
  fhAdjInit();
  if (p < 0 || p >= NUM_PLANETS || slot < 0 || slot >= MAX_PLAYERS) return false;
  if (gs.players[slot].bigOwned == BIG_CHARTS) return true;  // 3.1-dev.53 HYPERSPACE CHARTS
  uint32_t held = 0;
  for (int q = 0; q < NUM_PLANETS; q++) if (gs.planetOwner[q] == slot) held |= 1u << q;
  if (!held) return (fhStationReach >> p) & 1;   // no worlds: you launch from a space station
  if ((held >> p) & 1) return true;
  for (int q = 0; q < NUM_PLANETS; q++) if (((held >> q) & 1) && ((fhAdj[q] >> p) & 1)) return true;
  return false;
}
int snapshotFhShips(const GameState &gs, int slot, int k) {
  if (slot < 0 || slot >= MAX_PLAYERS || k < 0 || k >= NUM_FLEETS) return 0;
  const Player &pl = gs.players[slot];
  int n = 0;
  for (int s = 0; s < pl.fleetShipCount[k] && s < FLEET_SHIPS; s++) if (pl.fleetShipAlive[k][s]) n++;
  (void)0;  // (lent ships are already not alive -- 3.1-dev.72 dropped the old -1 here)
  return n;
}
int snapshotFhDebt(const GameState &gs, int slot, int f, int *cost) {
  if (cost) *cost = 0;
  if (slot < 0 || slot >= MAX_PLAYERS || f < 0 || f >= NUM_FACTIONS) return 0;
  if (gs.champion[f] == slot) return FH_DEBT_CHAMP;
  switch (reputationTierLocked(gs.players[slot].reputation[f])) {
    case REP_ALLIED: return FH_DEBT_FAMILY;
    case REP_FAVORABLE: return FH_DEBT_FRIEND;
    // 3.1-dev.74: a debt is owed, never sold -- only factions that like you answer, and they come free
    default: return 0;
  }
}
static int fhHandCount(const Player &pl) { int n = 0; for (int g = 0; g < FH_GAMBITS; g++) if ((pl.fhHand >> g) & 1) n++; return n; }

// The second-to-last round begins: DEAL. Comeback: the leader draws 1, last place 3, everyone else 2 (keep one).
static void fhDealLocked() {
  int hi = -9999, lo = 9999, n = 0;
  for (int i = 0; i < MAX_PLAYERS; i++) if (state.players[i].active) { hi = max(hi, state.players[i].vp); lo = min(lo, state.players[i].vp); n++; }
  for (int i = 0; i < MAX_PLAYERS; i++) {
    Player &pl = state.players[i];
    if (!pl.active) continue;
    const int cls = classOfLocked(i);
    if (cls < 0) continue;
    int draw = 2;
    if (hi != lo && n > 1) draw = pl.vp == hi ? 1 : pl.vp == lo ? 3 : 2;
    int pool[4] = { cls * 4, cls * 4 + 1, cls * 4 + 2, cls * 4 + 3 };
    for (int k = 3; k > 0; k--) { const int j = esp_random() % (k + 1); const int x = pool[k]; pool[k] = pool[j]; pool[j] = x; }
    for (int k = 0; k < 3; k++) pl.fhDeal[k] = (int8_t)(k < draw ? pool[k] : -1);
    pl.fhDealN = (uint8_t)draw;
    // the black market: two more, from anyone's class, never one you were just dealt
    int all[FH_GAMBITS], m = 0;
    for (int g = 0; g < FH_GAMBITS; g++) if (g / 4 != cls) all[m++] = g;
    for (int k = m - 1; k > 0; k--) { const int j = esp_random() % (k + 1); const int x = all[k]; all[k] = all[j]; all[j] = x; }
    pl.fhBm[0] = (int8_t)all[0]; pl.fhBm[1] = (int8_t)all[1];
    { const int a = esp_random() % BIG_COUNT; int b = esp_random() % (BIG_COUNT - 1); if (b >= a) b++;  // 3.1-dev.53 two big-ticket items
      pl.bigOff[0] = (int8_t)a; pl.bigOff[1] = (int8_t)b; }
    dealLotsLocked(i);  // 3.1-dev.88 four lots, one of them cheap
  }
  state.fhStage = 1;
  newsPubLocked(NEWS_FH_DEALT, -1, -1, -1);
  newsTableLocked(NEWS_FH_DEALT, -1, -1, -1);
  Serial.println("[FH] the gambits are dealt");
}
bool gameStateFhPick(int slot, int k) {
  if (g_remote || slot < 0 || slot >= MAX_PLAYERS) return false;
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return false;
  Player &pl = state.players[slot];
  const bool ok = pl.fhDealN > 0 && k >= 0 && k < pl.fhDealN && pl.fhDeal[k] >= 0;
  if (ok) { pl.fhHand |= 1u << pl.fhDeal[k]; pl.fhDealN = 0; Serial.printf("[FH] P%d keeps gambit %d\n", slot + 1, pl.fhDeal[k]); }
  xSemaphoreGive(stateMutex);
  return ok;
}
bool gameStateFhBuy(int slot, int k) {
  if (g_remote || slot < 0 || slot >= MAX_PLAYERS || k < 0 || k >= FH_BM_OFFERS) return false;
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return false;
  Player &pl = state.players[slot];
  const bool open = state.fhStage == 1 && (state.round < state.roundLimit || state.phase == PHASE_INVEST);
  const int g = pl.fhBm[k];
  const bool ok = open && g >= 0 && pl.credits >= FH_BM_CR && fhHandCount(pl) < FH_HAND && !((pl.fhHand >> g) & 1);
  if (ok) { pl.credits -= FH_BM_CR; pl.fhHand |= 1u << g; pl.fhBm[k] = -1; Serial.printf("[FH] P%d buys gambit %d\n", slot + 1, g); }
  xSemaphoreGive(stateMutex);
  return ok;
}
// 3.1-dev.88 THE CARGO HOLD
static int holdFreeLocked(int slot) { for (int k = 0; k < LOT_HOLD; k++) if (state.players[slot].hold[k] < 0) return k; return -1; }
static bool holdHasLocked(int slot, int item) {
  if (slot < 0 || slot >= MAX_PLAYERS) return false;
  for (int k = 0; k < LOT_HOLD; k++) if (state.players[slot].hold[k] == item) return true;
  return false;
}
static bool holdTakeLocked(int slot, int item) {
  if (slot < 0 || slot >= MAX_PLAYERS) return false;
  for (int k = 0; k < LOT_HOLD; k++) if (state.players[slot].hold[k] == item) { state.players[slot].hold[k] = -1; return true; }
  return false;
}
// Clean getaway: the side that withdraws takes no parting shot and isn't shaken (side 1 = attacker, 2 = defender)
static bool getawayLocked(int side) {
  if (state.battleGetaway & side) return true;
  const int s = side == 1 ? state.battleAttackerSlot : state.battleDefenderSlot;
  if (!holdTakeLocked(s, LOT_GETAWAY)) return false;
  state.battleGetaway |= (uint8_t)side; stampLocked(s, STAMP_GETAWAY, -1);
  return true;
}
bool snapshotBmOpen(const GameState &gs) {
  return gs.stage == STAGE_PLAYING && ((gs.fhStage == 1 && (gs.round < gs.roundLimit || gs.phase == PHASE_INVEST)) || snapshotEra(gs) == FACTION_CRIMSON_DAWN);
}
// Four lots a round, at least one cheap (user: "more items on the lower end of price -- stuff that benefits late game play").
static void dealLotsLocked(int i) {
  static const int PRICE[LOT_COUNT] = LOT_PRICES;
  Player &pl = state.players[i];
  int pool[LOT_COUNT], n = 0;
  for (int t = 0; t < LOT_COUNT; t++) pool[n++] = t;
  for (int k = n - 1; k > 0; k--) { const int j = esp_random() % (k + 1); const int x = pool[k]; pool[k] = pool[j]; pool[j] = x; }
  bool cheap = false;
  for (int k = 0; k < LOT_OFFERS; k++) { pl.lotOff[k] = (int8_t)pool[k]; if (PRICE[pool[k]] <= LOT_CHEAP_MAX) cheap = true; }
  if (!cheap) for (int k = LOT_OFFERS; k < n; k++) if (PRICE[pool[k]] <= LOT_CHEAP_MAX) { pl.lotOff[LOT_OFFERS - 1] = (int8_t)pool[k]; break; }
  pl.lotsGot = 0;
}
static void lotsRoundStartLocked() {  // a fresh offer every round the market is open
  if (!snapshotBmOpen(state)) return;
  for (int i = 0; i < MAX_PLAYERS; i++) if (state.players[i].active) dealLotsLocked(i);
}
const char *gameStateUseCargo(int slot, int k, int arg) {
  if (g_remote || slot < 0 || slot >= MAX_PLAYERS || k < 0 || k >= LOT_HOLD) return "Bad request.";
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return "Busy.";
  Player &pl = state.players[slot];
  const int item = pl.hold[k];
  const char *why = nullptr;
  if (state.stage != STAGE_PLAYING || !pl.active) why = "Not now.";
  else if (item == LOT_REFIT) {
    // 3.1-dev.101 (playtest: "it wouldn't let me use it on my destroyed fleet"): a fleet that was wiped out counts too -- it
    // comes back whole at your home hangar.
    bool wiped = arg >= 0 && arg < NUM_FLEETS && pl.fleetShipCount[arg] > 0;
    if (wiped) for (int s = 0; s < pl.fleetShipCount[arg] && s < FLEET_SHIPS; s++) if (pl.fleetShipAlive[arg][s]) wiped = false;
    if (arg < 0 || arg >= NUM_FLEETS || (!fleetCanRepair(pl, arg) && !wiped)) why = "Pick a fleet that has lost ships.";
    else { int n = 0; for (int s = 0; s < pl.fleetShipCount[arg] && s < FLEET_SHIPS; s++) if (!pl.fleetShipAlive[arg][s]) { pl.fleetShipAlive[arg][s] = true; n++; }
           if (!n) why = "That fleet has no ships to repair.";
           else if (wiped) noteLocked(slot, "Field refit: Fleet %d is back with %d ships. Place it at your home hangar.", arg + 1, n); }
  } else if (item == LOT_FORGED) {
    if (arg < 0 || arg >= NUM_FLEETS || !pl.fleetSpent[arg]) why = "Pick a fleet that already fought this round.";
    else if (state.phase != PHASE_RESOLUTION) why = "Forged orders work in Resolution.";
    else pl.fleetSpent[arg] = false;
  } else if (item == LOT_INSIDE) {
    if (arg < 0 || arg >= NUM_PLANETS) why = "Pick a world.";
    else { pnewsLocked(slot, PN_INSIDE, planetFaction(arg), arg, snapshotDefendersNow(state, arg));
           pl.revealed[arg][3] = true; pl.defSeen[arg] = (uint8_t)min(255, snapshotDefendersNow(state, arg) + 1); }  // 3.1-dev.101
  } else why = "That one works on its own when its moment comes.";
  if (!why) { pl.hold[k] = -1; Serial.printf("[BM] P%d used cargo lot %d (%d)\n", slot + 1, item, arg); }
  xSemaphoreGive(stateMutex);
  return why;
}
// 3.1-dev.98 THE BACK CHANNEL (the Consul's second strength; user 2026-11-20: "I love the idea of a back deal"): once a round, a
// private offer to one player -- "I pay you N CR if you [don't attack any of my worlds | don't attack <world> | don't negotiate for
// or bribe <world> | don't attack <player>] this round". Paid on accept. Broken: the breaker pays back what they got and the
// Consul receives DOUBLE (the bank adds the rest, unseen by the breaker -- user: "-5cr for the attacker, 5 come from the bank");
// the table hears that a deal was broken, never who. Checked where things really happen (a battle starts, a world changes hands).
const char *gameStateBcOffer(int slot, int to, int kind, int arg, int cr) {
  if (g_remote || slot < 0 || slot >= MAX_PLAYERS) return "Bad request.";
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return "Busy.";
  Player &pl = state.players[slot];
  const char *why = nullptr;
  if (state.stage != STAGE_PLAYING || classOfLocked(slot) != CLASS_CONSUL) why = "Only a Consul has a back channel.";
  else if (pl.bcRound == state.round && pl.bcSt != 0) why = "One offer a round.";  // 3.1-dev.101: open in the Final Hour too
  else if (to < 0 || to >= MAX_PLAYERS || to == slot || !state.players[to].active) why = "Pick a player.";
  else if (kind < BC_MY_WORLDS || kind > BC_PLAYER) why = "Pick a condition.";
  else if ((kind == BC_WORLD || kind == BC_TALK_WORLD) && (arg < 0 || arg >= NUM_PLANETS)) why = "Pick a world.";
  else if (kind == BC_PLAYER && (arg < 0 || arg >= MAX_PLAYERS || arg == to || !state.players[arg].active)) why = "Pick who to protect.";
  else if (cr < 1 || cr > BC_MAX_CR) why = "Offer 1 to 10 Credits.";
  else if (pl.credits < cr) why = "You don't have that much.";
  if (!why) {
    pl.bcTo = (int8_t)to; pl.bcKind = (int8_t)kind; pl.bcArg = (int8_t)(kind == BC_MY_WORLDS ? -1 : arg); pl.bcCr = (int8_t)cr;
    pl.bcRound = (int8_t)state.round; pl.bcSt = 1;
    pnewsLocked(to, PN_BC_OFFER, pl.chosenFaction, -1, slot);
    Serial.printf("[BC] P%d offers P%d %d CR (kind %d, arg %d)\n", slot + 1, to + 1, cr, kind, arg);
  }
  xSemaphoreGive(stateMutex);
  return why;
}
bool gameStateBcAnswer(int slot, bool accept) {
  if (g_remote || slot < 0 || slot >= MAX_PLAYERS) return false;
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return false;
  bool ok = false;
  for (int c = 0; c < MAX_PLAYERS && !ok; c++) {
    Player &k = state.players[c];
    if (k.bcTo != slot || k.bcRound != state.round || k.bcSt != 1) continue;
    if (accept && k.credits < k.bcCr) { k.bcSt = 3; break; }   // the Consul can't pay any more: it lapses
    ok = true;
    if (accept) { k.credits -= k.bcCr; state.players[slot].credits += k.bcCr; k.bcSt = 2; }
    else k.bcSt = 3;
    pnewsLocked(c, PN_BC_ANSWER, state.players[slot].chosenFaction, accept ? 1 : 0, slot);
  }
  xSemaphoreGive(stateMutex);
  return ok;
}
// Did this just break a deal the breaker accepted this round? (mutex held)
static void bcCheckLocked(int breaker, int how, int planet, int victim) {
  if (breaker < 0 || breaker >= MAX_PLAYERS) return;
  for (int c = 0; c < MAX_PLAYERS; c++) {
    Player &k = state.players[c];
    if (k.bcTo != breaker || k.bcRound != state.round || k.bcSt != 2) continue;
    const bool hostile = how == BCH_ATTACK || how == BCH_UPRISE;
    bool hit = false;
    if (k.bcKind == BC_MY_WORLDS) hit = hostile && planet >= 0 && victim == c;
    else if (k.bcKind == BC_WORLD) hit = hostile && planet == k.bcArg;
    else if (k.bcKind == BC_TALK_WORLD) hit = (how == BCH_TAKE_TALK || how == BCH_UPRISE) && planet == k.bcArg;
    else if (k.bcKind == BC_PLAYER) hit = (hostile || how == BCH_PVP) && victim == k.bcArg;
    if (!hit) continue;
    Player &b = state.players[breaker];
    const int back = min((int)b.credits, (int)k.bcCr);
    b.credits -= back;
    k.credits += back + k.bcCr;                 // the bank adds the other half
    k.bcSt = 4;
    pnewsLocked(c, PN_BC_BROKEN, b.chosenFaction, back + k.bcCr, breaker);
    pnewsLocked(breaker, PN_BC_BROKE, k.chosenFaction, back, c);
    newsPubLocked(NEWS_BC_BROKEN, -1, -1, -1);
    newsTableLocked(NEWS_BC_BROKEN, -1, -1, -1);
    Serial.printf("[BC] P%d broke P%d's deal: -%d / +%d\n", breaker + 1, c + 1, back, back + k.bcCr);
  }
}
// 3.1-dev.88 THE DAWN GRIPS THE GALAXY: anyone may buy another player's file, once a round; the champion names the Mark.
const char *gameStateBuyFile(int slot, int target) {
  if (g_remote || slot < 0 || slot >= MAX_PLAYERS) return "Bad request.";
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return "Busy.";
  Player &pl = state.players[slot];
  const char *why = nullptr;
  if (snapshotEra(state) != FACTION_CRIMSON_DAWN) why = "Files are only for sale while the Dawn grips the galaxy.";
  else if (target < 0 || target >= MAX_PLAYERS || target == slot || !state.players[target].active) why = "Pick a rival.";
  else if (pl.fileBuyRound == state.round) why = "One file a round.";
  else if (pl.credits < ERA_FILE_CR) why = "Not enough Credits.";
  if (!why) {
    pl.credits -= ERA_FILE_CR; pl.fileBuyRound = (int8_t)state.round;
    pl.sliceOn = (int8_t)target; pl.sliceRound = (int8_t)state.round;  // the same view as the Slicer lot
    pnewsLocked(target, PN_FILE_BOUGHT, FACTION_CRIMSON_DAWN, -1, 0);
    Serial.printf("[ERA] P%d bought P%d's file\n", slot + 1, target + 1);
  }
  xSemaphoreGive(stateMutex);
  return why;
}
bool gameStateNameMark(int slot, int target) {
  if (g_remote || slot < 0 || slot >= MAX_PLAYERS) return false;
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return false;
  const bool ok = snapshotEra(state) == FACTION_CRIMSON_DAWN && state.champion[FACTION_CRIMSON_DAWN] == slot && target != slot
                  && target >= 0 && target < MAX_PLAYERS && state.players[target].active && state.markSlot != target
                  && !snapshotIsLastPlace(state, target);  // 3.1-dev.101 (playtest: the Dawn piled everything on the trailing player)
  if (ok) {
    state.markSlot = (int8_t)target;
    newsPubLocked(NEWS_MARK, FACTION_CRIMSON_DAWN, -1, target);
    newsTableLocked(NEWS_MARK, FACTION_CRIMSON_DAWN, -1, state.players[target].chosenFaction);
  }
  xSemaphoreGive(stateMutex);
  return ok;
}
// 3.1-dev.77 BLACK-MARKET LOTS
const char *gameStateBuyLot(int slot, int k, int world) {
  if (g_remote || slot < 0 || slot >= MAX_PLAYERS || k < 0 || k >= LOT_OFFERS) return "Bad request.";
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return "Busy.";
  static const int PRICE[LOT_COUNT] = LOT_PRICES;
  Player &pl = state.players[slot];
  const int item = pl.lotOff[k];
  const bool keeps = item >= 0 && item < LOT_COUNT && ((LOT_KEEPS >> item) & 1);
  const bool needWorld = item == LOT_TROOPS || item == LOT_FORTIFY;
  const char *why = nullptr;
  if (!snapshotBmOpen(state)) why = "The black market is closed.";
  else if (item < 0 || item >= LOT_COUNT) why = "Not on offer.";
  else if ((pl.lotsGot >> item) & 1) why = "You already bought that.";
  else if (pl.lotsGot) why = "One deal a round with the broker.";  // 3.1-dev.101 (user: one item per shop per turn)
  else if (keeps && holdFreeLocked(slot) < 0) why = "Your cargo hold is full: use something first.";
  else if (pl.credits < PRICE[item]) why = "Not enough Credits.";
  else if (needWorld && (world < 0 || world >= NUM_PLANETS || state.planetOwner[world] != slot)) why = "Pick a world you hold.";
  else if (item == LOT_FORTIFY && ((state.fortMask >> world) & 1)) why = "That world is already fortified.";
  else if (item == LOT_SLICER && (world < 0 || world >= MAX_PLAYERS || world == slot || !state.players[world].active)) why = "Pick a rival.";
  if (!why) {
    pl.credits -= PRICE[item];
    pl.lotsGot |= (uint16_t)(1u << item);
    if (item == LOT_TROOPS) state.planetDefense[world] = (int8_t)min(MAX_PLANET_DEFENSE, state.planetDefense[world] + LOT_TROOPS_SHIPS);
    else if (item == LOT_FORTIFY) state.fortMask |= 1u << world;
    else if (item == LOT_SLICER) { pl.sliceOn = (int8_t)world; pl.sliceRound = (int8_t)state.round; }
    else if (keeps) pl.hold[holdFreeLocked(slot)] = (int8_t)item;
    Serial.printf("[BM] P%d buys lot %d (%d CR)\n", slot + 1, item, PRICE[item]);
  }
  xSemaphoreGive(stateMutex);
  return why;
}
// 3.1-dev.78 THEIR FILE: the Infiltrator opens one rival's file a round (Credits, debts, grudges, job, patrols).
bool gameStatePickFile(int slot, int target) {
  if (g_remote || slot < 0 || slot >= MAX_PLAYERS || target < 0 || target >= MAX_PLAYERS || target == slot) return false;
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return false;
  Player &pl = state.players[slot];
  const bool ok = state.stage == STAGE_PLAYING && classOfLocked(slot) == CLASS_INFILTRATOR && state.players[target].active
                  && (pl.fileRound < 0 || state.round - pl.fileRound >= FILE_COOLDOWN);  // 3.1-dev.97 PULL A FILE: every 2 rounds (the pulled one stays readable)
  if (ok) { pl.fileOn = (int8_t)target; pl.fileRound = (int8_t)state.round; }
  xSemaphoreGive(stateMutex);
  return ok;
}
// 3.1-dev.78 BLACKMAIL (user: the Crimson Dawn's Family gift, once -- "blackmail is too powerful to just get at friend").
static void exposeLocked(int target) {
  Player &t = state.players[target];
  int d[2] = { -1, -1 }, df = -1, n = 0;
  for (int o = 0; o < MAX_PENDING_OBLIGATIONS && n < 2; o++)
    if (t.obligations[o].active) { if (!n) df = t.obligations[o].factionIdx; d[n++] = t.obligations[o].planetIdx; }
  newsPubLocked(NEWS_EXPOSED, df, d[0], target * 20 + (d[1] + 1));
  newsTableLocked(NEWS_EXPOSED, df, d[0], t.chosenFaction);
  Serial.printf("[BLACKMAIL] P%d refused -- %d debt(s) made public\n", target + 1, n);
  t.bmBy = -1; t.bmRound = -1;
}
const char *gameStateBlackmail(int slot, int target) {
  if (g_remote || slot < 0 || slot >= MAX_PLAYERS || target < 0 || target >= MAX_PLAYERS || target == slot) return "Pick a rival.";
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return "Busy.";
  Player &pl = state.players[slot], &t = state.players[target];
  const char *why = nullptr;
  if (state.stage != STAGE_PLAYING || !t.active) why = "Pick a rival.";
  else if (pl.blackmailUsed) why = "You've used it.";
  else if (tierOfLocked(slot, FACTION_CRIMSON_DAWN) != REP_ALLIED) why = "Only the Crimson Dawn's Family can do that.";
  else if (t.bmBy >= 0) why = "Someone already has them over a barrel.";
  if (!why) {
    pl.blackmailUsed = 1; t.bmBy = (int8_t)slot; t.bmRound = (int8_t)state.round;
    evMineLocked(target, LOG_PNEWS, FACTION_CRIMSON_DAWN, -1, slot, PN_BLACKMAILED);
    Serial.printf("[BLACKMAIL] P%d blackmails P%d\n", slot + 1, target + 1);
  }
  xSemaphoreGive(stateMutex);
  return why;
}
bool gameStateBlackmailAnswer(int slot, bool pay) {
  if (g_remote || slot < 0 || slot >= MAX_PLAYERS) return false;
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return false;
  Player &t = state.players[slot];
  const int by = t.bmBy;
  bool ok = by >= 0 && by < MAX_PLAYERS;
  if (ok && pay) {
    if (t.credits < BLACKMAIL_CR) ok = false;
    else { t.credits -= BLACKMAIL_CR; state.players[by].credits += BLACKMAIL_CR; t.bmBy = -1; t.bmRound = -1;
           evMineLocked(by, LOG_PNEWS, FACTION_CRIMSON_DAWN, -1, slot, PN_BLACKMAIL_PAID); }
  } else if (ok) exposeLocked(slot);
  xSemaphoreGive(stateMutex);
  return ok;
}
// Saboteur / Turncoat / Hired blade fire when you start a fight on a world, a Decoy beacon when one of YOUR worlds is attacked;
// Second chance when one of your fleets drops to 2. (3.1-dev.88: they all wait in the CARGO HOLD until then.)
static void lotsAtBattleStartLocked(int attackerSlot, int planetIdx) {
  if (state.planetOwner[planetIdx] == attackerSlot) return;
  if (holdHasLocked(attackerSlot, LOT_SABOTEUR)) {
    const int k = min(LOT_SABOTEUR_HIT, min((int)state.planetDefense[planetIdx], state.defRemaining - 1));
    if (k > 0) { state.defRemaining -= k; holdTakeLocked(attackerSlot, LOT_SABOTEUR); stampLocked(attackerSlot, STAMP_SABOTEUR, planetIdx); }
  }
  if (holdHasLocked(attackerSlot, LOT_TURNCOAT)) {
    const int k = min(1, min((int)state.planetDefense[planetIdx], state.defRemaining - 1));
    if (k > 0) { state.defRemaining -= k; state.battleAtkMercs += k; holdTakeLocked(attackerSlot, LOT_TURNCOAT); stampLocked(attackerSlot, STAMP_TURNCOAT, planetIdx); }
  }
  if (holdTakeLocked(attackerSlot, LOT_BLADE)) { state.battleAtkMercs++; stampLocked(attackerSlot, STAMP_BLADE, planetIdx); }
  const int o = state.planetOwner[planetIdx];
  if (o >= 0 && o < MAX_PLAYERS && holdTakeLocked(o, LOT_DECOY)) { state.defRemaining++; stampLocked(o, STAMP_DECOY, planetIdx); }
  if (o >= 0 && o < MAX_PLAYERS && holdTakeLocked(o, LOT_BLADE)) { state.defRemaining++; stampLocked(o, STAMP_BLADE, planetIdx); }  // defending your world
}
static int aliveCount(const bool *alive);  // (above)
static void secondChanceLocked() {
  if (state.battleOver) return;
  const int a = state.battleAttackerSlot, d = state.battleDefenderSlot;
  if (a >= 0 && a < MAX_PLAYERS && holdHasLocked(a, LOT_SECOND) && aliveCount(state.atkAlive) == 2) {
    state.battleAtkMercs += LOT_SECOND_SHIPS; holdTakeLocked(a, LOT_SECOND); stampLocked(a, STAMP_SECOND, -1);
  }
  if (state.battleKind == BATTLE_PVP && d >= 0 && d < MAX_PLAYERS && holdHasLocked(d, LOT_SECOND) && aliveCount(state.defAlive) == 2) {
    state.battleDefMercs += LOT_SECOND_SHIPS; holdTakeLocked(d, LOT_SECOND); stampLocked(d, STAMP_SECOND, -1);
  }
}
const char *gameStateFhBuyBig(int slot, int k, int world) {
  if (g_remote || slot < 0 || slot >= MAX_PLAYERS || k < 0 || k > 1) return "Bad request.";
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return "Busy.";
  static const int PRICE[BIG_COUNT] = BIG_PRICES;
  Player &pl = state.players[slot];
  const int item = pl.bigOff[k];
  const char *why = nullptr;
  if (!(state.fhStage == 1 && (state.round < state.roundLimit || state.phase == PHASE_INVEST))) why = "The black market is closed.";
  else if (pl.bigOwned >= 0) why = "One big-ticket item a game.";
  else if (item < 0 || item >= BIG_COUNT) why = "Not on offer.";
  else if (pl.credits < PRICE[item]) why = "Not enough Credits.";
  else if (item == BIG_SHIELD && (world < 0 || world >= NUM_PLANETS || state.planetOwner[world] != slot)) why = "Pick a world you hold.";
  if (!why) {
    pl.credits -= PRICE[item];
    pl.bigOwned = (int8_t)item; pl.bigArg = (int8_t)(item == BIG_SHIELD ? world : -1);
    newsPubLocked(NEWS_BIGBUY, pl.chosenFaction, item == BIG_SHIELD ? world : -1, slot * 8 + item);
    newsTableLocked(NEWS_BIGBUY, pl.chosenFaction, item == BIG_SHIELD ? world : -1, item);
    Serial.printf("[FH] P%d buys big-ticket item %d (%d CR)\n", slot + 1, item, PRICE[item]);
  }
  xSemaphoreGive(stateMutex);
  return why;
}
bool gameStateFhDebt(int slot, int f, int n) {
  if (g_remote || slot < 0 || slot >= MAX_PLAYERS || f < 0 || f >= NUM_FACTIONS) return false;
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return false;
  Player &pl = state.players[slot];
  int cost = 0;
  const int most = snapshotFhDebt(state, slot, f, &cost);
  if (!cost) n = most;
  bool ok = snapshotFhFinal(state) && state.phase == PHASE_INVEST && !((pl.fhCalled >> f) & 1) && most > 0 && n >= 1 && n <= most && pl.credits >= n * cost;
  if (ok) {
    pl.credits -= n * cost;
    pl.fhCalled |= 1u << f;
    pl.fhAllyFrom[f] = (uint8_t)n;
    Serial.printf("[FH] P%d calls in the %s: %d ships (%d CR)\n", slot + 1, factionName(f), n, n * cost);
  }
  xSemaphoreGive(stateMutex);
  return ok;
}
static int fhAllies(const Player &pl) { int n = 0; for (int f = 0; f < NUM_FACTIONS; f++) n += pl.fhAllyFrom[f]; return n; }
// 3.1-dev.116: ordAlly[k] = allied ships (low 4 bits) | HIRED ships (high 4 bits) -- no new GameState field, saves still resume.
static inline int ordAllies(uint8_t v) { return v & 15; }
static inline int ordHired(uint8_t v) { return v >> 4; }
static int fhHireCost(const int8_t *t, const uint8_t *al) { int n = 0; for (int k = 0; k < FH_STRIKES; k++) if (t[k] >= 0) n += ordHired(al[k]); return n * FH_HIRE_CR; }
static int fhJumpCost(const GameState &gs, int slot, const int8_t *t) {
  int n = 0;
  for (int k = 0; k < FH_STRIKES; k++) if (t[k] >= 0 && !snapshotFhReach(gs, slot, t[k])) n++;
  return n * FH_JUMP_CR;
}
const char *gameStateFhOrders(int slot, const int8_t *t, const int8_t *guard, const uint8_t *ally, const int8_t *g) {
  if (g_remote || slot < 0 || slot >= MAX_PLAYERS || !t || !guard || !ally || !g) return "Bad orders.";
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return "Busy.";
  Player &pl = state.players[slot];
  const char *why = nullptr;
  if (!snapshotFhFinal(state) || state.phase != PHASE_MOVEMENT) why = "Last orders are given in the final Movement.";
  else if (pl.lockedIn) why = "Your orders are locked in.";
  int allies = 0; uint16_t used = 0;
  for (int k = 0; !why && k < FH_STRIKES; k++) {
    if (t[k] >= NUM_PLANETS || t[k] < -1) why = "No such world.";
    else if (t[k] >= 0 && state.planetOwner[t[k]] == slot) why = "That world is already yours.";
    else if (k < NUM_FLEETS && t[k] >= 0 && snapshotFhShips(state, slot, k) == 0) why = "That fleet has no ships.";
    else if (k == 3 && t[k] >= 0 && !((pl.fhHand >> G_SMUGGLER) & 1)) why = "You need Smuggler's Run for that.";
    else if (k == 4 && t[k] >= 0 && pl.bigOwned != BIG_ARMADA) why = "You need a Mercenary Armada for that.";
    if (k < NUM_FLEETS && guard[k] >= 0 && (guard[k] >= NUM_PLANETS || state.planetOwner[guard[k]] != slot)) why = "A fleet can only guard your own world.";
    allies += ordAllies(ally[k]);
    if (!why && ordHired(ally[k]) > FH_HIRE_MAX) why = "Too many hired ships on one strike.";
    if (!why && g[k] >= 0) {
      if (g[k] >= FH_GAMBITS || !((pl.fhHand >> g[k]) & 1) || !((FH_PINNED_MASK >> g[k]) & 1)) why = "You don't hold that gambit.";
      else if ((used >> g[k]) & 1) why = "One strike per gambit.";
      else if (t[k] < 0) why = "Pin a gambit to a strike.";
      else if (g[k] == G_TURNCOAT && state.planetOwner[t[k]] >= 0) why = "Turncoat Garrison works only on an unclaimed world.";
      used |= 1u << g[k];
    }
  }
  if (!why && allies > fhAllies(pl)) why = "You don't have that many allied ships.";
  if (!why && fhJumpCost(state, slot, t) + fhHireCost(t, ally) > pl.credits) why = "Not enough Credits for the jump passes and hired ships.";
  if (!why) for (int k = 0; k < FH_STRIKES; k++) { pl.ordT[k] = t[k]; pl.ordAlly[k] = ally[k]; pl.ordG[k] = g[k]; if (k < NUM_FLEETS) pl.ordGuard[k] = guard[k]; }
  xSemaphoreGive(stateMutex);
  return why;
}
// Movement -> Resolution in the last round: pay the jump passes (a strike you can't pay for stays home) and commit
// the pinned gambits + Smuggler's Run (they leave your hand -- they're on their way).
static void fhChargeLocked() {
  for (int i = 0; i < MAX_PLAYERS; i++) {
    Player &pl = state.players[i];
    if (!pl.active) continue;
    for (int k = FH_STRIKES - 1; k >= 0; k--) if (pl.ordT[k] >= 0 && fhJumpCost(state, i, pl.ordT) > pl.credits) { pl.ordT[k] = -1; pl.ordG[k] = -1; }
    pl.credits -= fhJumpCost(state, i, pl.ordT);
    for (int k = FH_STRIKES - 1; k >= 0 && fhHireCost(pl.ordT, pl.ordAlly) > pl.credits; k--) pl.ordAlly[k] &= 15;  // 3.1-dev.116 can't pay: they don't sign on
    pl.credits -= fhHireCost(pl.ordT, pl.ordAlly);
    for (int k = 0; k < FH_STRIKES; k++) if (pl.ordT[k] >= 0 && pl.ordG[k] >= 0) pl.fhHand &= ~(1u << pl.ordG[k]);
    if (pl.ordT[3] >= 0) pl.fhHand &= ~(1u << G_SMUGGLER);
    for (int k = 0; k < FH_HAND; k++) pl.rvG[k] = pl.rvA[k] = pl.rvB[k] = -1;
  }
}
const char *gameStateFhReveal(int slot, const int8_t *g, const int8_t *a, const int8_t *b) {
  if (g_remote || slot < 0 || slot >= MAX_PLAYERS || !g || !a || !b) return "Bad play.";
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return "Busy.";
  Player &pl = state.players[slot];
  const char *why = nullptr;
  if (!snapshotFhFinal(state) || state.phase != PHASE_RESOLUTION || state.fhStage != 1) why = "The reveal is over.";
  else if (pl.lockedIn) why = "You're locked in.";
  uint16_t used = 0;
  for (int k = 0; !why && k < FH_HAND; k++) {
    if (g[k] < 0) continue;
    if (g[k] >= FH_GAMBITS || !((pl.fhHand >> g[k]) & 1) || !((FH_REVEAL_MASK >> g[k]) & 1) || ((used >> g[k]) & 1)) why = "You don't hold that gambit.";
    used |= 1u << g[k];
  }
  if (!why) for (int k = 0; k < FH_HAND; k++) { pl.rvG[k] = g[k]; pl.rvA[k] = a[k]; pl.rvB[k] = b[k]; }
  xSemaphoreGive(stateMutex);
  return why;
}

// ---- THE FINALE ----
struct FhStrike { int8_t by, id, world; int16_t ships; uint16_t gm; uint8_t out, absorb, best; };
static FhStrike fhSt[MAX_PLAYERS * FH_STRIKES];
static int fhN = 0;
static void fhLog(int v) { if (state.fhLogN < FH_LOG_LEN) state.fhLog[state.fhLogN++] = (uint8_t)constrain(v, 0, 255); }
static int fhFind(int id) { for (int s = 0; s < fhN; s++) if (fhSt[s].id == id && !fhSt[s].out) return s; return -1; }
static int fhBiggest(int by) { int best = -1; for (int s = 0; s < fhN; s++) if (fhSt[s].by == by && !fhSt[s].out && (best < 0 || fhSt[s].ships > fhSt[best].ships)) best = s; return best; }
static int fhBestWorld(int slot) {
  int best = -1;
  for (int p = 0; p < NUM_PLANETS; p++) if (state.planetOwner[p] == slot && (best < 0 || planetVP(state, p, slot) > planetVP(state, best, slot))) best = p;
  return best;
}
static int fhRoll(int n) { int r = 0; for (int k = 0; k < max(1, min(3, n)); k++) r = max(r, rollDie()); return r; }

// Log format (bytes; the phones read it, web_server.cpp fhParse):
//   REVEAL: 0xE0, count, then per play: player, gambit, a, b   (a/b: a strike id / world / faction / ships, per gambit)
//   per world fought: 0xE1, world, k (strikes), holder+1 (0 = unclaimed), garrison before, garrison after (+ guards),
//     per strike: player, ships, gambit mask lo, gambit mask hi
//     exchanges: 0xE2, garrison die, die per strike (0 = out)   ...   0xE3 = the garrison fell, they turn on each other
//                0xE4, die per strike                            (the free-for-all)
//     0xE5, winner (0 = the holder keeps it, 1..k = that strike), ships left
static void fhFinaleLocked() {
  state.fhLogN = 0;
  fhN = 0;
  int guard[NUM_PLANETS] = {};
  for (int i = 0; i < MAX_PLAYERS; i++) {
    Player &pl = state.players[i];
    if (!pl.active) continue;
    int placed = 0;
    for (int k = 0; k < FH_STRIKES; k++) {
      const int ships = k < NUM_FLEETS ? snapshotFhShips(state, i, k) : k == 3 ? FH_SMUGGLER_SHIPS : BIG_ARMADA_SHIPS;
      if (pl.ordT[k] >= 0 && ships > 0 && fhN < MAX_PLAYERS * FH_STRIKES) {
        FhStrike &s = fhSt[fhN++];
        s = FhStrike{ (int8_t)i, (int8_t)(i * FH_STRIKES + k), pl.ordT[k], (int16_t)(ships + ordAllies(pl.ordAlly[k]) + ordHired(pl.ordAlly[k])), (uint16_t)(pl.ordG[k] >= 0 ? 1u << pl.ordG[k] : 0), 0, 0, 1 };
        placed += ordAllies(pl.ordAlly[k]);
      } else if (k < NUM_FLEETS && ships > 0) {  // stays home: guards a world (its own column, outside the garrison cap)
        int w = pl.ordGuard[k];
        if (w < 0 || state.planetOwner[w] != i) w = fhBestWorld(i);
        if (w >= 0) { guard[w] += ships + ordAllies(pl.ordAlly[k]); placed += ordAllies(pl.ordAlly[k]); }
      }
    }
    const int spare = fhAllies(pl) - placed, w = fhBestWorld(i);  // allies nobody placed guard your best world
    if (spare > 0 && w >= 0) guard[w] += spare;
  }
  for (int i = 0; i < MAX_PLAYERS; i++) if (state.players[i].active && state.players[i].bigOwned == BIG_CAPITAL) {  // 3.1-dev.53 CAPITAL SHIP
    const int b = fhBiggest(i);
    if (b >= 0) fhSt[b].ships += BIG_CAPITAL_SHIPS;
  }
  // AT THE REVEAL, in a fixed order: the long con, sabotage, false flags, calls for peace, treaties, rallies, old friends.
  static const int8_t ORDER[] = { G_LONG_CON, G_SABOTAGE, G_FALSE_FLAG, G_PEACE, G_TREATY, G_RALLY, G_OLD_FRIENDS };
  fhLog(0xE0);
  const int countAt = state.fhLogN; fhLog(0);
  int plays = 0;
  for (int8_t gg : ORDER) for (int i = 0; i < MAX_PLAYERS; i++) {
    Player &pl = state.players[i];
    if (!pl.active) continue;
    for (int k = 0; k < FH_HAND; k++) {
      if (pl.rvG[k] != gg || !((pl.fhHand >> gg) & 1)) continue;
      pl.fhHand &= ~(1u << gg);
      int a = pl.rvA[k], b = pl.rvB[k], s = -1;
      const bool ghost = (s = fhFind(a)) >= 0 && (fhSt[s].gm & (1u << G_GHOST));
      switch (gg) {
        case G_LONG_CON: {  // 3 ships off the VP leader's biggest strike (not you) onto yours
          int lead = -1;
          for (int o = 0; o < MAX_PLAYERS; o++) if (o != i && state.players[o].active && (lead < 0 || state.players[o].vp > state.players[lead].vp)) lead = o;
          const int from = lead >= 0 ? fhBiggest(lead) : -1, to = fhBiggest(i);
          int take = from >= 0 ? min(FH_LONG_CON, fhSt[from].ships - 1) : 0;
          if (take > 0) { fhSt[from].ships -= take; if (to >= 0) fhSt[to].ships += take; else { const int w = fhBestWorld(i); if (w >= 0) guard[w] += take; } }
          a = from >= 0 ? fhSt[from].id : -1; b = max(0, take);
        } break;
        case G_SABOTAGE: if (s >= 0 && !ghost && fhSt[s].by != i) { b = fhSt[s].ships - (fhSt[s].ships + 1) / 2; fhSt[s].ships = (fhSt[s].ships + 1) / 2; } else a = -1; break;
        case G_FALSE_FLAG: if (s >= 0 && !ghost && fhSt[s].by != i && b >= 0 && b < NUM_PLANETS && state.planetOwner[b] != fhSt[s].by) fhSt[s].world = (int8_t)b; else a = -1; break;
        case G_PEACE: if (s >= 0 && !ghost) fhSt[s].out = 1; else a = -1; break;
        case G_TREATY: if (a >= 0 && a < NUM_PLANETS && state.planetOwner[a] == i) { for (int q = 0; q < fhN; q++) if (fhSt[q].world == a) fhSt[q].out = 1; } else a = -1; break;
        case G_RALLY: b = 0; for (int p = 0; p < NUM_PLANETS; p++) if (state.planetOwner[p] == i) { state.planetDefense[p] += FH_RALLY; b++; } break;
        case G_OLD_FRIENDS: {
          const int f = (a >= 0 && a < NUM_FACTIONS) ? a : 0, extra = pl.fhAllyFrom[f], to = fhBiggest(i);
          if (to >= 0) fhSt[to].ships += extra; else { const int w = fhBestWorld(i); if (w >= 0) guard[w] += extra; }
          b = extra;
        } break;
      }
      fhLog(i); fhLog(gg); fhLog(a < 0 ? 255 : a); fhLog(b < 0 ? 255 : b);
      plays++;
    }
  }
  state.fhLog[countAt] = (uint8_t)plays;
  // THE FINALE: world by world, Outer Rim first, the Core last.
  int fights = 0, taken = 0;
  for (int ring = RING_OUTER; ring <= RING_CORE; ring++) for (int w = 0; w < NUM_PLANETS; w++) {
    if (PLANETS[w].ring != ring) continue;
    int idx[MAX_PLAYERS * FH_STRIKES], k = 0;
    for (int s = 0; s < fhN; s++) if (!fhSt[s].out && fhSt[s].world == w && fhSt[s].by != state.planetOwner[w]) {
      int same = -1;  // two of your fleets on one world fight as one strike
      for (int j = 0; j < k; j++) if (fhSt[idx[j]].by == fhSt[s].by) same = idx[j];
      if (same >= 0) { fhSt[same].ships += fhSt[s].ships; fhSt[same].gm |= fhSt[s].gm; fhSt[s].out = 1; }
      else idx[k++] = s;
    }
    if (!k) continue;
    fights++;
    const int holder = state.planetOwner[w];
    const int before = state.planetDefense[w] + guard[w];
    int D = before;
    for (int j = 0; j < k; j++) {  // gambits pinned to a strike go off as its fight starts
      FhStrike &s = fhSt[idx[j]];
      Player &pl = state.players[s.by];
      if (s.gm & (1u << G_HIDDEN_FLEET)) s.ships += FH_HIDDEN_FLEET;
      if (s.gm & (1u << G_BOMBARD)) D = max(0, D - FH_BOMBARD);
      if (s.gm & (1u << G_NO_RETREAT)) s.absorb = FH_NO_RETREAT;
      if (s.gm & (1u << G_TURNCOAT) && holder < 0) { const int n = min(FH_TURNCOAT, D); D -= n; s.ships += n; }
      if (s.gm & (1u << G_HIGH_ROLLER)) { const int n = min(FH_HIGH_ROLLER_MAX, pl.credits / FH_HIGH_ROLLER_CR); pl.credits -= n * FH_HIGH_ROLLER_CR; s.ships += n; }
    }
    int shield = (holder >= 0 && state.players[holder].bigOwned == BIG_SHIELD && state.players[holder].bigArg == w) ? BIG_SHIELD_HITS : 0;  // 3.1-dev.53
    fhLog(0xE1); fhLog(w); fhLog(k); fhLog(holder + 1); fhLog(before); fhLog(D); fhLog(shield);
    for (int j = 0; j < k; j++) { const FhStrike &s = fhSt[idx[j]]; fhLog(s.by); fhLog(s.ships); fhLog(s.gm & 255); fhLog(s.gm >> 8); }
    int living = 0;
    for (int j = 0; j < k; j++) if (fhSt[idx[j]].ships > 0) living++;
    // side by side: the garrison rolls once, every strike rolls against it. Beat it = a garrison ship falls; lose or tie = you lose one.
    for (int ex = 0; D > 0 && living > 0 && ex < 60 && state.fhLogN < FH_LOG_LEN - 40; ex++) {
      int most = 0;
      for (int j = 0; j < k; j++) most = max(most, (int)fhSt[idx[j]].ships);
      const int dHelp = holder < 0 ? (D - most >= FH_HELP3 ? 3 : D - most >= FH_HELP2 ? 2 : 1) : 1;  // #9 invisible help, both ways vs a world
      const int dd = fhRoll(dHelp);
      int roll[MAX_PLAYERS * FH_STRIKES] = {};
      for (int j = 0; j < k; j++) {
        FhStrike &s = fhSt[idx[j]];
        if (s.ships <= 0) continue;
        int help = holder < 0 ? (s.ships - D >= FH_HELP3 ? 3 : s.ships - D >= FH_HELP2 ? 2 : 1) : 1;
        if (s.gm & (1u << G_LOADED)) help++;
        roll[j] = fhRoll(help);
      }
      fhLog(0xE2); fhLog(dd);
      for (int j = 0; j < k; j++) fhLog(roll[j]);
      for (int hi = 6; hi >= 1; hi--) for (int j = 0; j < k; j++) {  // the highest rolls land first
        if (roll[j] != hi) continue;
        FhStrike &s = fhSt[idx[j]];
        if (hi > dd) { if (shield > 0) shield--; else if (D > 0) D--; }  // 3.1-dev.53 the shield takes the first hits
        else if (s.absorb) s.absorb--;
        else s.ships--;
      }
      living = 0;
      for (int j = 0; j < k; j++) if (fhSt[idx[j]].ships > 0) living++;
    }
    int win = 0;
    if (D <= 0 && living > 0) {
      if (living > 1) {  // THE GARRISON FALLS -- now they turn on each other: everyone rolls, the lowest roll loses a ship (a tie for lowest: roll again)
        fhLog(0xE3);
        for (int ex = 0; living > 1 && ex < 80 && state.fhLogN < FH_LOG_LEN - 20; ex++) {
          int roll[MAX_PLAYERS * FH_STRIKES] = {}, low = 7, lowN = 0, lowJ = -1;
          for (int j = 0; j < k; j++) if (fhSt[idx[j]].ships > 0) {
            roll[j] = fhRoll((fhSt[idx[j]].gm & (1u << G_LOADED)) ? 2 : 1);
            if (roll[j] < low) { low = roll[j]; lowN = 1; lowJ = j; } else if (roll[j] == low) lowN++;
          }
          fhLog(0xE4);
          for (int j = 0; j < k; j++) fhLog(roll[j]);
          if (lowN == 1) { FhStrike &s = fhSt[idx[lowJ]]; if (s.absorb) s.absorb--; else s.ships--; }
          living = 0;
          for (int j = 0; j < k; j++) if (fhSt[idx[j]].ships > 0) living++;
        }
        if (living > 1) {  // (never in practice) the biggest takes it
          int best = -1;
          for (int j = 0; j < k; j++) if (fhSt[idx[j]].ships > 0 && (best < 0 || fhSt[idx[j]].ships > fhSt[idx[best]].ships)) best = j;
          for (int j = 0; j < k; j++) if (j != best) fhSt[idx[j]].ships = 0;
        }
      }
      for (int j = 0; j < k; j++) if (fhSt[idx[j]].ships > 0) win = j + 1;
    }
    if (win) {
      const FhStrike &s = fhSt[idx[win - 1]];
      conquerLocked(s.by, w);
      state.planetDefense[w] = constrain((int)s.ships, 1, MAX_PLANET_DEFENSE);
      fhLog(0xE5); fhLog(win); fhLog(s.ships);
      // (3.1-dev.117: the table's headline comes when this world's turn in the playback reaches its result -- fhTickLocked)
      taken++;
    } else {
      state.planetDefense[w] = constrain(min(D, state.planetDefense[w]), 1, MAX_PLANET_DEFENSE + FH_RALLY);  // losses come off the guards first
      fhLog(0xE5); fhLog(0); fhLog(max(0, D));
      // (3.1-dev.117: headline at its turn -- fhTickLocked)
    }
  }
  state.fhStage = 2;
  Serial.printf("[FH] THE FINALE: %d strikes, %d worlds fought, %d taken, log %d bytes\n", fhN, fights, taken, state.fhLogN);
}
static void gameOverLocked();
// 3.1-dev.117 THE FINAL HOUR IN STEP (canvas board 27; user: "it needs to be unified so everyone is watching at the same time.
// all players have to press skip if we want skip to happen"): the TABLE keeps the clock. Step 0 = the reveal, then one step per
// world fought; every phone shows the step the table is on; a step is skipped only when every player still here asks; the
// table's headline for a world lands when its result does. Not in GameState (a restart mid-finale just plays it again).
struct FhStepInfo { int8_t w, holder, winner, first; uint8_t nEx, sides; bool turn; };
static FhStepInfo g_fhStep[NUM_PLANETS];
static int g_fhSteps = 0, g_fhShow = -1;
static uint32_t g_fhAt = 0, g_fhDur = 0;
static uint8_t g_fhSkip = 0, g_fhResShown = 0;
static void fhParseStepsLocked() {  // the same walk as the phones' fhParse
  const uint8_t *b = state.fhLog; const int N = state.fhLogN; int k = 0; g_fhSteps = 0;
  if (N > 1 && b[0] == 0xE0) k = 2 + b[1] * 4;
  while (k < N && g_fhSteps < NUM_PLANETS) {
    if (b[k] != 0xE1) { k++; continue; }
    if (k + 7 > N) break;
    FhStepInfo &s = g_fhStep[g_fhSteps];
    s.w = (int8_t)b[k + 1]; const int n = b[k + 2]; s.holder = (int8_t)(b[k + 3] - 1); k += 7;
    int8_t by[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
    for (int j = 0; j < n && k + 4 <= N; j++) { if (j < 8) by[j] = (int8_t)b[k]; k += 4; }
    s.nEx = 0; s.turn = false;
    while (k < N && b[k] != 0xE5) {
      if (b[k] == 0xE2) { s.nEx++; k += 2 + n; } else if (b[k] == 0xE3) { s.turn = true; k++; } else if (b[k] == 0xE4) { s.nEx++; k += 1 + n; } else k++;
    }
    const int win = k + 1 < N ? b[k + 1] : 0; k += 3;
    s.winner = (win > 0 && win <= 8) ? by[win - 1] : -1;
    s.first = by[0]; s.sides = (uint8_t)n;
    g_fhSteps++;
  }
}
static uint32_t fhStepMs(int step) {  // as long as the phones' animation of it
  if (step <= 0) return 5000;
  const FhStepInfo &s = g_fhStep[step - 1];
  return 1800 + 1800 + (uint32_t)s.nEx * (s.nEx > 18 ? 520 : 820) + (s.turn ? 3000 : 0) + 2600 + 600;  // 3.1-dev.119 + the map's ring flash
}
static void fhResultHeadlineLocked(int step) {
  if (step <= 0 || step > g_fhSteps) return;
  const FhStepInfo &s = g_fhStep[step - 1];
  const int hf = s.holder >= 0 ? state.players[s.holder].chosenFaction : -1;
  state.tqN = 0; state.tqShownMs = 0;  // the finale owns the table: show it now
  if (s.winner >= 0) newsTableLocked(NEWS_FH_TAKEN, state.players[s.winner].chosenFaction, s.w, hf);
  else newsTableLocked(NEWS_FH_HOLDS, hf, s.w, -1);
}
// 3.1-dev.123 THE TABLE'S STRIKE (canvas board 27): as a strike begins, the round screen says "STRIKE n OF m" round the rim, the
// world big, the attacker under it, in the attacker's colour; the result headline replaces it when the phones reach the result.
static void fhStrikeHeadlineLocked(int step) {
  if (step <= 0 || step > g_fhSteps) return;
  const FhStepInfo &s = g_fhStep[step - 1];
  const int af = s.first >= 0 && s.first < MAX_PLAYERS ? state.players[s.first].chosenFaction : -1;
  state.tqN = 0; state.tqShownMs = 0;
  newsTableLocked(NEWS_FH_STRIKE, af, s.w, s.sides);
}
static uint8_t fhHereMaskLocked() { uint8_t m = 0; for (int i = 0; i < MAX_PLAYERS; i++) if (state.players[i].active && !state.players[i].away) m |= 1u << i; return m; }
static void fhTickLocked() {
  if (state.stage != STAGE_PLAYING || state.fhStage != 2) { g_fhShow = -1; return; }
  const uint32_t now = millis();
  // 3.1-dev.121 (user: "the first screen is a totally blank screen with a skip button ... get rid of it"): no reveal step -- the
  // finale opens on the first strike (the gambits played at the reveal are listed in the summary).
  if (g_fhShow < 0) { fhParseStepsLocked(); g_fhShow = 1; g_fhAt = now; g_fhDur = g_fhSteps ? fhStepMs(1) : 8000; g_fhSkip = 0; g_fhResShown = 0; fhStrikeHeadlineLocked(1); }
  if (g_fhShow > g_fhSteps) {  // the summary: a moment to read it, then the final standings
    if (now - g_fhAt < 8000) return;
    g_fhShow = -1; state.fhStage = 3; gameOverLocked(); return;
  }
  const uint8_t here = fhHereMaskLocked();
  // 3.1-dev.119 (user: "you should be able to push it at anytime in the final hour, and it should skip to the end"): once
  // everyone still here has asked, the playback jumps to the summary. Votes last the whole finale.
  if (here && (g_fhSkip & here) == here) {
    g_fhShow = g_fhSteps + 1; g_fhAt = now; g_fhDur = 8000; g_fhSkip = 0;
    Serial.printf("[FH] everyone skipped: straight to the summary\n");
    return;
  }
  if (g_fhShow > 0 && !g_fhResShown && now - g_fhAt + 2600 + 600 >= g_fhDur) { fhResultHeadlineLocked(g_fhShow); g_fhResShown = 1; }
  if (now - g_fhAt >= g_fhDur) {
    g_fhShow++; g_fhAt = now; g_fhResShown = 0;
    g_fhDur = g_fhShow <= g_fhSteps ? fhStepMs(g_fhShow) : 8000;
    fhStrikeHeadlineLocked(g_fhShow);
    Serial.printf("[FH] playback step %d of %d\n", g_fhShow, g_fhSteps);
  }
}
int gameStateFhShowStep() { return g_fhShow; }
int gameStateFhSteps() { return g_fhSteps; }
uint8_t gameStateFhSkipMask() { return g_fhSkip; }
uint32_t gameStateFhStepLeftMs() { const uint32_t e = millis() - g_fhAt; return g_fhShow < 0 || e >= g_fhDur ? 0 : g_fhDur - e; }
bool gameStateFhSkip(int slot) {
  if (g_remote || slot < 0 || slot >= MAX_PLAYERS) return false;
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return false;
  const bool ok = state.fhStage == 2 && g_fhShow >= 0;
  if (ok) g_fhSkip ^= (uint8_t)(1u << slot);  // tap again to take it back
  xSemaphoreGive(stateMutex);
  return ok;
}

// 3.1-dev.101 THE GAME RECORD (user: "does crimson dawn win more?"): one line per finished game in /games.txt --
// rounds, then per player: faction, character, class, VP, worlds; then the Eras declared (faction:by) and who won.
#define GAMES_FILE "/games.txt"
// 3.1-dev.122 MONEY IN THE GAME RECORD (user: Rhel had TRIPLE income and "felt like she didn't have enough money"): each player's
// total round income and roughly what they spent (Credits gone between paydays + at the end). Not saved: a restart mid-game
// undercounts, the record says so with a '~'.
static int g_moneyIn[MAX_PLAYERS], g_moneyOut[MAX_PLAYERS], g_moneyMark[MAX_PLAYERS]; static bool g_moneyExact = false;
static void gameMoneyRoundLocked(int i, bool) { const int d = g_moneyMark[i] - state.players[i].credits; if (d > 0) g_moneyOut[i] += d; g_moneyMark[i] = state.players[i].credits; }
static void gameMoneyStartLocked() { for (int i = 0; i < MAX_PLAYERS; i++) { g_moneyIn[i] = g_moneyOut[i] = 0; g_moneyMark[i] = state.players[i].credits; } g_moneyExact = true; }
static void gameRecordLocked() {
  if (LittleFS.exists(GAMES_FILE)) { File c = LittleFS.open(GAMES_FILE, "r"); const size_t sz = c ? c.size() : 0; if (c) c.close(); if (sz > 24000) return; }
  char line[260]; int n = snprintf(line, sizeof(line), "r%d", state.roundLimit);
  int best = -1;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    const Player &pl = state.players[i];
    if (!pl.active) continue;
    int worlds = 0; for (int p = 0; p < NUM_PLANETS; p++) if (state.planetOwner[p] == i) worlds++;
    gameMoneyRoundLocked(i, false);
    n += snprintf(line + n, sizeof(line) - n, " | f%d c%d k%d vp%d w%d in%s%d out%d", pl.chosenFaction, pl.commander, characterClass(pl.commander), pl.vp, worlds,
                  g_moneyExact ? "" : "~", g_moneyIn[i], g_moneyOut[i]);
    if (best < 0 || pl.vp > state.players[best].vp) best = i;
    if (n >= (int)sizeof(line) - 30) break;
  }
  n += snprintf(line + n, sizeof(line) - n, " | era f%d by%d | top f%d k%d\n", state.eraFaction, state.eraBy,
                best >= 0 ? state.players[best].chosenFaction : -1, best >= 0 ? characterClass(state.players[best].commander) : -1);
  File f = LittleFS.open(GAMES_FILE, "a");
  if (f) { f.print(line); f.close(); }
  Serial.printf("[GAMES] %s", line);
}
// ---- Phase ----
static void gameOverLocked() {
  // v0.30.1: open promises are judged first, so an undone favor's planet goes back before the final count.
  for (int i = 0; i < MAX_PLAYERS; i++)
    if (state.players[i].active) promiseGameEndLocked(i);
  questsAndAwardsLocked();  // 2.0: quests, Merchant prince, Best customer
  state.stage = STAGE_GAME_OVER;
  gameRecordLocked();  // 3.1-dev.101
}
static void advancePhaseLocked() {
  // Assumes mutex held. Advances only if all active players are locked in
  // (2.0-dev.13: AWAY players are held locked in) -- and someone is here: with
  // everyone away, the table simply waits.
  int active = 0, locked = 0, present = 0;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (state.players[i].active) {
      active++;
      if (state.players[i].lockedIn) locked++;
      if (!state.players[i].away) present++;
    }
  }
  if (active == 0 || present == 0 || locked < active) return;

  // 3.1-dev.43 THE FINAL HOUR: everyone's in at the reveal -> the finale plays (the game ends when it has, fhTickLocked)
  if (snapshotFhFinal(state) && state.phase == PHASE_RESOLUTION) {
    if (state.fhStage != 1) return;
    newsTableLocked(NEWS_FH_BEGIN, -1, -1, -1);
    fhFinaleLocked();
    return;
  }
  if (snapshotFhFinal(state) && state.phase == PHASE_MOVEMENT) fhChargeLocked();

  state.phase = (state.phase + 1) % 3;
  // 3.1-dev.7: peak Credits for Make a fortune, and quests pay the moment they're done.
  for (int i = 0; i < MAX_PLAYERS; i++) {
    Player &qp = state.players[i];
    if (!qp.active) continue;
    if (qp.credits > qp.peakCredits) qp.peakCredits = (uint8_t)min(255, qp.credits);
    if (!qp.questDone && snapshotQuestDone(state, i)) { qp.questDone = 1; qp.vp += QUEST_VP; evPubLocked(LOG_QUEST, i, -1, -1, 0); }
  }
  if (state.phase == PHASE_RESOLUTION) for (int i = 0; i < MAX_PLAYERS; i++) state.players[i].intelEyed = 0;  // dev.41: the rumor has had its round
  for (int i = 0; i < MAX_PLAYERS; i++) state.players[i].lockedIn = state.players[i].away != AWAY_NONE;
  if (state.phase == PHASE_MOVEMENT) dealsPrepareEndLocked();  // 2.0: unanswered offers lapse
  if (state.phase == PHASE_MOVEMENT)  // 3.1-dev.16 (user): a job offer waits through Prepare, then lapses
    for (int i = 0; i < MAX_PLAYERS; i++) {
      Player &jp = state.players[i];
      if (jp.jobState != 1) continue;
      pnewsLocked(i, PN_JOB_PASSED, jp.jobFaction, jp.jobWorld, 1);
      jp.jobState = 0; jp.jobFaction = jp.jobWorld = jp.jobDue = -1;
    }

  if (state.phase == PHASE_INVEST) {
    // (v0.30.1: no VP per round any more -- planets are worth VP while held, see setOwnerLocked.)
    if (state.round >= state.roundLimit) {
      // This was the final round's Resolution -- freeze here rather than
      // looping back into a new Invest phase. v0.30.1: open promises are judged
      // first, so an undone favor's planet goes back before the final count.
      gameOverLocked();
      return;
    }

    state.round += 1;  // looped back around
    evPubLocked(LOG_ROUND, -1, -1, -1, 0);
    // v0.25.0 slow regrowth: every REGROWTH_EVERY_ROUNDS rounds unclaimed
    // planets rebuild their garrison a little, never past where they started.
    if ((state.round - 1) % REGROWTH_EVERY_ROUNDS == 0)
      for (int q = 0; q < NUM_PLANETS; q++)
        if (state.planetDefense[q] < planetStartGarrison(q))  // 3.1-dev.76: held worlds rebuild too (user: investing in defense is "the most tedious part")
          state.planetDefense[q] = min(planetStartGarrison(q), state.planetDefense[q] + REGROWTH_AMOUNT);
    // v0.23.1: income -- every player is paid for the round that starts now.
    for (int i = 0; i < MAX_PLAYERS; i++) {
      if (!state.players[i].active) continue;
      gameMoneyRoundLocked(i, true);  // 3.1-dev.122: what went out since the last payday
      state.players[i].lastIncome = snapshotIncome(state, i);
      state.players[i].credits += state.players[i].lastIncome;
      g_moneyIn[i] += state.players[i].lastIncome; g_moneyMark[i] = state.players[i].credits;
    }
    for (int i = 0; i < MAX_PLAYERS; i++)  // v0.28.1: tributes, held planets, deadlines
      if (state.players[i].active) promiseRoundStartLocked(i);
    for (int p = 0; p < NUM_PLANETS; p++)  // 3.1-dev.79: a buyout left unanswered for a round is a sale
      if (state.buyBy[p] && state.buyRound[p] < state.round) buyoutSettleLocked(p, false);
    for (int i = 0; i < MAX_PLAYERS; i++)  // 3.1-dev.78: a blackmail left unanswered for a round is a refusal
      if (state.players[i].active && state.players[i].bmBy >= 0 && state.players[i].bmRound < state.round) exposeLocked(i);
    resetRoundCommitmentsLocked();  // every fleet may fight, every planet reopens
    if (state.round == state.roundLimit - 1 && state.fhStage == 0) fhDealLocked();  // 3.1-dev.43: THE GAMBITS ARE DEALT
    roundStartPerksLocked();        // 2.0: traits, cargo, the diplomat, markets, the galaxy event
    if (state.round == state.roundLimit) {  // v0.24.4: the table announces it too
      state.publicMoment = TXB_LASTROUND;
      state.publicActor = state.publicVictim = state.publicPlanet = -1;
      state.publicSeq++; evMomentLocked();
    }
    if (state.round == state.roundLimit)  // v0.23.0: big moment -- your own faction calls as the last round begins
      for (int i = 0; i < MAX_PLAYERS; i++)
        if (state.players[i].active && state.players[i].chosenFaction >= 0)
          queueBigLocked(i, TXB_LASTROUND, state.players[i].chosenFaction, -1, -1, -1);
    for (int i = 0; i < MAX_PLAYERS; i++) {
      if (state.players[i].active) checkObligationsDueLocked(i);
    }
  }
}

void gameStateLockIn(int slot) {
  if (g_remote) { remoteCall(RF_LOCK_IN, {slot}); return; }
  if (slot < 0 || slot >= MAX_PLAYERS) return;
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return;
  if (state.players[slot].active) {
    state.players[slot].lockedIn = true;
    advancePhaseLocked();
  }
  xSemaphoreGive(stateMutex);
}

// ---- Pay price ----
// Pure (no lock) core price calculation, used by the public
// gameStatePayPrice() (which takes the lock itself), by /state JSON on a
// snapshot, and directly on `state` by gameStateCommitTakeover() (which
// already holds the lock when resolving TAKEOVER_PAY -- calling the public
// version there used to deadlock, since it tried to take the same
// non-recursive mutex a second time from within the same call chain,
// permanently blocking it and every other player's read/write for the rest
// of the session).
int snapshotIncome(const GameState &gs, int slot) {
  if (slot < 0 || slot >= MAX_PLAYERS || !gs.players[slot].active) return 0;
  const int base = INCOME_BASE + max(0, gs.round - 1) / INCOME_BASE_EVERY;  // 3.1-dev.101: base pay grows through the game
  int income = base;
  // EV_SPICEBOOM: High-resource worlds pay double at the next two Prepares.
  const bool boom = gs.eventId == EV_SPICEBOOM && gs.round > gs.eventRound && gs.round <= gs.eventRound + 2;  // the event strikes after that round's income
  for (int p = 0; p < NUM_PLANETS; p++)
    if (gs.planetOwner[p] == slot) {
      if (p == gs.players[slot].collatWorld && gs.round <= gs.players[slot].collatUntil) continue;  // 3.1-dev.42 Pyke collateral
      income += gs.planetResIsHigh[p] ? INCOME_RESOURCES_HIGH * (boom ? 2 : 1) : INCOME_RESOURCES_LOW;
      switch (gs.planetTrait[p]) {  // 2.0 traits
        case TR_REFINERY: income += TRAIT_INCOME_REFINERY; break;
        case TR_TRADEHUB: income += TRAIT_INCOME_TRADEHUB; break;
        case TR_MINES: income += TRAIT_INCOME_MINES; break;
        default: break;
      }
      if (((gs.treatyMask >> p) & 1) && gs.treatyUntil[p] >= gs.round && characterClass(gs.players[slot].commander) == CLASS_CONSUL) income += CONSUL_TREATY_CR;  // 3.1-dev.101 (dev.114: only while it runs)
    }
  if (gs.players[slot].chosenFaction == FACTION_PYKE) income += PYKE_INCOME_BONUS;  // spice money
  if (reputationTierLocked(gs.players[slot].reputation[FACTION_PYKE]) == REP_ALLIED) income += GIFT_SPICE_INCOME;  // 3.1-dev.8 a spice route
  if (characterClass(gs.players[slot].commander) == CLASS_SCOUNDREL) income = max(income, max(SCOUNDREL_INCOME_FLOOR, base + SCOUNDREL_STASH_OVER));  // 3.1-dev.48 THE STASH
  if (snapshotEra(gs) == FACTION_PYKE) income *= eraChampLocked(gs, slot) ? ERA_SPICE_CHAMP_MULT : ERA_SPICE_MULT;  // 3.1-dev.88 SPICE FLOWS
  return income;
}

int snapshotPayPrice(const GameState &gs, int slot, int planetIdx) {
  if (planetIdx < 0 || planetIdx >= NUM_PLANETS) return 0;
  // v0.25.4 (user 2026-10-16): the price used to add the current garrison and
  // the Resources level, so it leaked both. Now only public things count --
  // the ring (it's on the map) -- plus this game's
  // market value; reputation adjusts it only once you've revealed the
  // planet's Affiliation ("if you know the faction of the planet, it can then
  // adjust the price"), so a discount never gives the faction away.
  static const int RING_PRICE[3] = { PRICE_OUTER, PRICE_MID, PRICE_CORE };
  const int ring = PLANETS[planetIdx].ring;
  int price = RING_PRICE[(ring >= 0 && ring < 3) ? ring : 0] + gs.planetMarket[planetIdx];

  if (slot >= 0 && slot < MAX_PLAYERS && gs.players[slot].revealed[planetIdx][0]) {
    int factionIdx = factionIndexForPlanet(planetIdx);
    int tier = reputationTierLocked(gs.players[slot].reputation[factionIdx]);
    static const int8_t ADJ[5] = PAY_TIER_ADJ;  // 3.1-dev.8: the favor ladder
    if (tier >= 0 && tier < 5) price += ADJ[tier];
  }
  // v0.30.0 escalating PAY: the first purchase is cheap, every one after costs more.
  if (slot >= 0 && slot < MAX_PLAYERS)
    price += gs.players[slot].purchases == 0 ? -PAY_FIRST_DISCOUNT : PAY_STEP * (gs.players[slot].purchases - 1);
  if (slot >= 0 && slot < MAX_PLAYERS) price -= gs.players[slot].permitDiscount;  // 2.0: GOOD_PERMITS
  // (3.1-dev.78: no MARK-UP any more -- the Scoundrel's weakness is PRICE ON YOUR HEAD)
  if (slot >= 0 && gs.summitBy == slot && !gs.summitBroken && gs.round <= gs.summitUntil) price -= SUMMIT_PAY_OFF;  // 3.1-dev.13
  if (price < 1) price = 1;
  if (snapshotEra(gs) == FACTION_PYKE) price = (price * ERA_SPICE_PAY_PCT + 50) / 100;  // 3.1-dev.88 WORLDS COST A FORTUNE
  const int o = gs.planetOwner[planetIdx];  // 3.1-dev.79 BUYOUT: a commander's world costs twice as much, never under BUYOUT_MIN_CR
  if (o >= 0 && o != slot && liveHomeOf(gs, planetIdx) < 0) price = max(BUYOUT_MIN_CR, price * BUYOUT_MULT);
  return price;
}

int gameStatePayPrice(int slot, int planetIdx) {
  if (g_remote) return remoteCall(RF_PAY_PRICE, {slot, planetIdx}).value;
  int price = 0;
  if (stateMutex && xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
    price = snapshotPayPrice(state, slot, planetIdx);
    xSemaphoreGive(stateMutex);
  }
  return price;
}

// ---- Takeover ----
// Assumes mutex already held. Closes this slot's takeover session.
static void clearTakeoverLocked(int slot) {
  state.takeoverPlanet[slot] = -1;
  state.takeoverMethod[slot] = TAKEOVER_NONE;
  state.negCount[slot] = 0;  // v0.30.0: the conversation
  state.negStep[slot] = 0;
  state.negResult[slot] = NEG_TALKING;
  state.negFavor[slot] = -1;
  for (int k = 0; k < NEG_MAX_Q; k++) state.negMarks[slot][k] = NEG_MARK_NONE;
  state.negOfferN[slot] = 0;  // v0.30.1
  state.negLine[slot] = -1;
  state.takeoverResult[slot][0] = '\0';
}

void gameStateBeginTakeover(int slot, int planetIdx) {
  if (g_remote) { remoteCall(RF_BEGIN_TAKEOVER, {slot, planetIdx}); return; }
  if (slot < 0 || slot >= MAX_PLAYERS) return;
  if (planetIdx < 0 || planetIdx >= NUM_PLANETS) return;
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return;
  // v0.20.2: no second try on a planet you already committed to this round.
  const bool fhTalk = snapshotFhFinal(state) && state.phase == PHASE_INVEST;  // 3.1-dev.43: the last Prepare keeps Talk + Pay
  if (state.stage == STAGE_PLAYING && (fhTalk || (state.phase == PHASE_RESOLUTION && !snapshotFhFinal(state)))
      && !state.players[slot].takeoverLocked[planetIdx]) {
    clearTakeoverLocked(slot);
    state.takeoverPlanet[slot] = planetIdx;
  }
  xSemaphoreGive(stateMutex);
}

void gameStateCancelTakeover(int slot) {
  if (g_remote) { remoteCall(RF_CANCEL_TAKEOVER, {slot}); return; }
  if (slot < 0 || slot >= MAX_PLAYERS) return;
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return;
  if (state.takeoverPlanet[slot] >= 0) {
    // Only a real snub if you walk out of a conversation still going (v0.30.0:
    // it also counts as a lost one there); closing its result card is free.
    if (state.takeoverMethod[slot] == TAKEOVER_NEGOTIATE && state.negResult[slot] == NEG_TALKING && state.negCount[slot] > 0) {
      const int p = state.takeoverPlanet[slot];
      if (state.players[slot].negLosses[p] < 255) state.players[slot].negLosses[p]++;
      applyReputationLocked(slot, factionIndexForPlanet(p), REP_WALKOUT, false, TXC_WALKOUT, p);
      gallerySettleLocked(slot, p, false);  // 3.1-dev.52 walking out = the talk failed
      clearTakeoverLocked(slot); alertAfterTalkLocked(slot, p);  // 3.1-dev.101
      xSemaphoreGive(stateMutex);
      return;
    }
    clearTakeoverLocked(slot);
  }
  xSemaphoreGive(stateMutex);
}

// Assumes mutex already held. Starts a planet battle and returns nullptr,
// or refuses (changing nothing) and returns the player-facing reason.
// Same two guards gameStateStartPvpBattle() has:
//  - A battle is already running: there is one global battle slot, and
//    overwriting it orphaned the running battle's attacker (their losses
//    were never written back) and let a double-tapped FIGHT restart your
//    own battle, undoing ship losses.
//  - No living ships: firstAvailableFleetLocked() falls back to fleet 0
//    even when it's empty, and a zero-ship "fleet" still rolled -- it could
//    destroy garrison ships (even capture a 1-garrison planet), then
//    "lost" instantly, unlocking garrison intel for free.
//
// fleetIdx picks the attacking fleet (from the battle briefing); -1 = the
// first fleet with ships (old behavior, kept for callers without a picker).
// v0.28.0: the fleets in `mask` (bit per fleet) and why they can't attack, or nullptr.
static const char *attackFleetsRefusalLocked(int slot, int mask) {
  if (mask <= 0 || mask >= (1 << NUM_FLEETS)) return "Pick a fleet to send in.";
  { const Player &pl = state.players[slot]; if (state.round <= pl.noAtkRound && pl.noAtkF >= 0 && pl.noAtkF < 4) return NO_ATTACK_WHY[(int)pl.noAtkF]; }  // 3.1-dev.42
  { const Player &pl = state.players[slot]; if (state.round == pl.shakenRound && (mask & pl.shakenMask)) return "That fleet is shaken from its retreat: it can't attack this round."; }  // 3.1-dev.76
  for (int f = 0; f < NUM_FLEETS; f++) {
    if (!(mask & (1 << f))) continue;
    if (fleetStrengthLocked(slot, f) <= 0) return "That fleet has no ships left.";
    if (state.players[slot].fleetSpent[f]) return "That fleet already fought this round.";
  }
  return nullptr;
}
static int lowestFleet(int mask) {
  for (int f = 0; f < NUM_FLEETS; f++) if (mask & (1 << f)) return f;
  return -1;
}

bool snapshotInkWet(const GameState &gs, int p) {
  return p >= 0 && p < NUM_PLANETS && gs.inkRound[p] == gs.round && gs.inkBy[p] > 0 && gs.planetOwner[p] == gs.inkBy[p] - 1;
}
// 3.1-dev.102: ships that join a battle -- STAND BY (+2 on the fleet's first attack the round after) and GUNS FOR HIRE (the hirer's
// next battle, either side; they take the first hits; each rolls to go home to its Warlord).
static void gfhHomeLocked(int hirer) {
  Player &h = state.players[hirer];
  const int w = h.hiredFrom, n = h.hiredN;
  h.hiredN = 0; h.hiredFrom = -1; h.hiredUntil = -1;
  if (w < 0 || w >= MAX_PLAYERS || n <= 0) return;
  Player &wl = state.players[w];
  int back = 0;
  for (int k = 0; k < n; k++) if (rollDie() >= GFH_RETURN_MIN) back++;
  int left = back;
  for (int b = 0; b < 32 && left > 0; b++)
    if ((wl.gfhMask >> b) & 1) { const int f = b / 8, s = b % 8; if (f < NUM_FLEETS && s < FLEET_SHIPS && !wl.fleetShipAlive[f][s]) { wl.fleetShipAlive[f][s] = true; left--; } }
  wl.gfhMask = 0;
  pnewsLocked(w, PN_GFH_BACK, -1, -1, back * 8 + n);
  Serial.printf("[GFH] P%d's %d lent ships: %d came home\n", w + 1, n, back);
}
static void joinersLocked(int attacker, int defender, int fleetMask) {
  if (attacker >= 0 && attacker < MAX_PLAYERS) {
    Player &a = state.players[attacker];
    if (fleetMask >= 0 && a.standByRound == state.round && (a.standByMask & fleetMask)) {
      a.standByMask &= (uint8_t)~fleetMask; state.battleAtkExtra += STANDBY_SHIPS; stampLocked(attacker, STAMP_STANDBY);
    }
    if (a.hiredN && a.hiredFrom != defender) { state.battleAtkExtra += a.hiredN; stampLocked(attacker, STAMP_HIRED); gfhHomeLocked(attacker); }
  }
  if (defender >= 0 && defender < MAX_PLAYERS) {
    Player &d = state.players[defender];
    if (d.hiredN && d.hiredFrom != attacker) { state.battleDefExtra += d.hiredN; stampLocked(defender, STAMP_HIRED); gfhHomeLocked(defender); }
  }
}
uint32_t snapshotNeighbors(int p) { fhAdjInit(); return (p >= 0 && p < NUM_PLANETS) ? fhAdj[p] : 0; }
const char *gameStateGfhOffer(int slot, int to, uint32_t mask, int cr) {
  if (g_remote || slot < 0 || slot >= MAX_PLAYERS) return "Bad request.";
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return "Busy.";
  Player &pl = state.players[slot];
  const char *why = nullptr;
  int n = 0; for (int b = 0; b < 32; b++) if ((mask >> b) & 1) { const int f = b / 8, s = b % 8; if (f >= NUM_FLEETS || s >= FLEET_SHIPS || !pl.fleetShipAlive[f][s]) n = 99; else n++; }
  if (state.stage != STAGE_PLAYING || classOfLocked(slot) != CLASS_WARLORD) why = "Only a Warlord hires out guns.";
  else if (pl.gfhRound == state.round && pl.gfhSt != 0) why = "One offer a round.";
  else if (to < 0 || to >= MAX_PLAYERS || to == slot || !state.players[to].active) why = "Pick a player.";
  else if (n < 1 || n > GFH_MAX_SHIPS) why = "Pick 1 to 3 of your ships.";
  else if (cr < 1 || cr > GFH_MAX_CR) why = "Pick a price.";
  else if (state.players[to].hiredN) why = "They already have hired guns.";
  if (!why) {
    pl.gfhTo = (int8_t)to; pl.gfhRound = (int8_t)state.round; pl.gfhSt = 1; pl.gfhCr = (uint8_t)cr; pl.gfhN = (uint8_t)n; pl.gfhMask = mask;
    pnewsLocked(to, PN_GFH_OFFER, -1, -1, slot);
    Serial.printf("[GFH] P%d offers %d ships to P%d for %d CR\n", slot + 1, n, to + 1, cr);
  }
  xSemaphoreGive(stateMutex);
  return why;
}
bool gameStateGfhAnswer(int slot, bool accept) {
  if (g_remote || slot < 0 || slot >= MAX_PLAYERS) return false;
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return false;
  bool ok = false;
  for (int w = 0; w < MAX_PLAYERS && !ok; w++) {
    Player &wl = state.players[w];
    if (wl.gfhTo != slot || wl.gfhSt != 1 || wl.gfhRound != state.round) continue;
    Player &h = state.players[slot];
    if (accept && h.credits < wl.gfhCr) break;
    ok = true;
    if (!accept) { wl.gfhSt = 3; pnewsLocked(w, PN_GFH_ANSWER, -1, 0, slot); break; }
    int n = 0;  // the ships leave the Warlord's fleets now
    for (int b = 0; b < 32; b++) if ((wl.gfhMask >> b) & 1) { const int f = b / 8, s = b % 8;
      if (f < NUM_FLEETS && s < FLEET_SHIPS && wl.fleetShipAlive[f][s]) { wl.fleetShipAlive[f][s] = false; n++; } else wl.gfhMask &= ~(1u << b); }
    h.credits -= wl.gfhCr; wl.credits += wl.gfhCr;
    h.hiredN = (uint8_t)n; h.hiredFrom = (int8_t)w; h.hiredUntil = (int8_t)(state.round + 1);
    wl.gfhSt = 2;
    const int hf = h.chosenFaction;
    if (hf >= 0 && hf < NUM_FACTIONS && !snapshotFhFinal(state)) applyReputationLocked(w, hf, GFH_FAVOR, false, TXC_DEAL, -1, 0);
    pnewsLocked(w, PN_GFH_ANSWER, -1, 1, slot);
    Serial.printf("[GFH] P%d hired %d ships from P%d\n", slot + 1, n, w + 1);
  }
  xSemaphoreGive(stateMutex);
  return ok;
}
// 3.1-dev.102 STAND BY (beaten to a world): the fleet holds -- spent this round, +STANDBY_SHIPS on its first attack next round.
const char *gameStateStandBy(int slot, int fleet) {
  if (g_remote || slot < 0 || slot >= MAX_PLAYERS || fleet < 0 || fleet >= NUM_FLEETS) return "Bad request.";
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return "Busy.";
  Player &pl = state.players[slot];
  const char *why = nullptr;
  if (state.stage != STAGE_PLAYING || state.phase != PHASE_RESOLUTION) why = "Stand by is a Resolution order.";
  else if (pl.fleetSpent[fleet]) why = "That fleet already moved this round.";
  else if (fleetStrengthLocked(slot, fleet) <= 0) why = "That fleet has no ships.";
  if (!why) {
    pl.fleetSpent[fleet] = true;
    if (pl.standByRound != state.round + 1) pl.standByMask = 0;
    pl.standByMask |= (uint8_t)(1u << fleet); pl.standByRound = (int8_t)(state.round + 1);
  }
  xSemaphoreGive(stateMutex);
  return why;
}
static const char *startBattleLocked(int attackerSlot, int planetIdx, int fleetMask) {
  if (state.battleActive) return "A battle is already in progress.";
  // 3.1-dev.102 (replaces DUST SETTLING): taken by force this round = DUG IN (+2, below); won by talk / bribe = THE INK IS
  // STILL WET -- one attack this round, against its old defenders.
  bool dugIn = false;
  if (planetIdx >= 0 && planetIdx < NUM_PLANETS && state.phase == PHASE_RESOLUTION && state.planetOwner[planetIdx] != attackerSlot) {
    if (state.planetTakenRound[planetIdx] == state.round && state.upriseFrom[planetIdx] - 1 != attackerSlot) dugIn = true;
    if (snapshotInkWet(state, planetIdx) && ((state.inkHit >> planetIdx) & 1)) return "Someone already tried: the ink is still wet there, but it allows one attack a round.";
  }
  const bool leaveMercs = fleetMask >= 0 && (fleetMask & FIGHT_LEAVE_MERCS);  // 2.0-dev.28
  if (fleetMask >= 0) fleetMask &= (1 << NUM_FLEETS) - 1;
  if (fleetMask < 0) {  // no picker: the first fleet with ships
    const int f = firstAvailableFleetLocked(attackerSlot);
    if (fleetStrengthLocked(attackerSlot, f) <= 0) return "You have no fleet ready to fight.";
    fleetMask = 1 << f;
  }
  if (const char *why = attackFleetsRefusalLocked(attackerSlot, fleetMask)) return why;
  promiseOnAttackLocked(attackerSlot, planetIdx);  // v0.28.1: a "don't attack" promise breaks here
  // Faction modifiers are fixed for the whole battle -- exactly what the
  // briefing showed when the player confirmed.
  BattleMods mods = snapshotPlanetBattleMods(state, attackerSlot, planetIdx);
  state.battleAtkMod = mods.atkMod;
  state.battleDefMod = mods.defMod;
  state.battleEscortFaction = mods.atkEscort > 0 ? (int8_t)mods.atkSourceFaction : -1;
  state.battleEscortAlive = (uint8_t)mods.atkEscort;
  state.battleMilitiaFaction = mods.defMilitia > 0 ? (int8_t)mods.defSourceFaction : -1;
  state.battleMilitiaAlive = (uint8_t)mods.defMilitia;
  state.battleAtkMercs = leaveMercs ? 0 : (uint8_t)mods.atkMercs;  // 2.0-dev.26: hired ships (dev.28: unless left home)
  state.battleDefMercs = 0;
  if (!leaveMercs) state.players[attackerSlot].nextBattleMercs = 0;
  classBattleExtrasLocked(attackerSlot, state.planetOwner[planetIdx]);  // 3.1-dev.7
  if (snapshotInkWet(state, planetIdx)) state.inkHit |= 1u << planetIdx;  // 3.1-dev.102 its one attack
  if (dugIn) { state.battleDefExtra += DUG_IN_DEF; stampLocked(attackerSlot, STAMP_DUGIN, planetIdx); }
  joinersLocked(attackerSlot, state.planetOwner[planetIdx], fleetMask);  // 3.1-dev.102 stand by + hired guns
  state.atkRerolls = (uint8_t)playerBattleRerolls(attackerSlot);  // 2.0: TR_ANCIENT / GOOD_BLESSING
  state.defRerolls = 0;
  // Committed (v0.20.2): every fleet sent in is spent for the round, and this
  // planet is closed to any other approach until next round.
  for (int f = 0; f < NUM_FLEETS; f++) if (fleetMask & (1 << f)) state.players[attackerSlot].fleetSpent[f] = true;
  state.players[attackerSlot].takeoverLocked[planetIdx] = true;
  state.lastRetreatSide = 0;
  state.battleKind = BATTLE_PLANET;
  state.battleActive = true;
  state.battleAttackerSlot = attackerSlot;
  state.battleAttackerFleet = lowestFleet(fleetMask);
  state.battleAttackerFleets = (uint8_t)fleetMask;
  state.battleDefenderFleets = 0;
  state.battleTargetPlanet = planetIdx;
  state.battleDefenderSlot = -1;
  state.battleDefenderFleet = -1;
  // Copy the real fleets' current ship-alive state (losses persist across
  // battles/rounds) -- every fleet sent in, pooled (v0.28.0).
  for (int i = 0; i < BATTLE_SHIPS; i++) {
    const int f = i / FLEET_SHIPS;
    state.atkAlive[i] = (fleetMask & (1 << f)) && state.players[attackerSlot].fleetShipAlive[f][i % FLEET_SHIPS];
    state.defAlive[i] = false;
  }
  state.battlePatrolAlive = (state.planetOwner[planetIdx] >= 0 && state.patrolUntil[planetIdx] >= state.round) ? state.patrolShips[planetIdx] : 0;  // 3.1-dev.76
  state.defRemaining = state.planetDefense[planetIdx] + state.battleMilitiaAlive + state.battlePatrolAlive;
  state.battleGetaway = 0;
  bcCheckLocked(attackerSlot, BCH_ATTACK, planetIdx, state.planetOwner[planetIdx]);  // 3.1-dev.98 the Back Channel
  lotsAtBattleStartLocked(attackerSlot, planetIdx);  // 3.1-dev.77 Saboteur / Turncoat (3.1-dev.88 + Hired blade, Decoy beacon)
  if (state.warBy == attackerSlot && state.warWorld == planetIdx && state.warRound == state.round && state.phase == PHASE_RESOLUTION) {
    state.battleAtkMercs += WARPATH_SHIPS;  // 3.1-dev.88 WARPATH
    stampLocked(attackerSlot, STAMP_WARPATH, planetIdx);
    state.warBy = state.warWorld = state.warRound = -1;
  }
  state.atkAction = 0;
  state.defAction = 0;
  state.lastAtkRoll = 0;
  state.lastDefRoll = 0;
  state.battleExchange = 0;
  state.lastLossIdx = -1;
  state.battleOver = false;
  state.resultText[0] = '\0';
  return nullptr;
}

// Pure: the faction roll modifiers `slot` would get attacking `planetIdx`.
// Only applies once the planet's Affiliation is revealed -- you can't call
// in help against a faction you haven't identified (and the briefing
// never leaks hidden intel). PvP battles never use these.
BattleMods snapshotPlanetBattleMods(const GameState &gs, int slot, int planetIdx) {
  BattleMods m;
  if (slot < 0 || slot >= MAX_PLAYERS || planetIdx < 0 || planetIdx >= NUM_PLANETS) return m;
  // 2.0: GOOD_MERCENARIES counts whether or not you know the Affiliation (2.0-dev.26: as ships).
  m.atkMercs = gs.players[slot].nextBattleMercs;
  int pf = factionIndexForPlanet(planetIdx);
  int rival = rivalFactionIndex(pf);
  // 2.0-dev.25 (user: "militia should apply if you haven't revealed"): the
  // locals rise up whether or not you know who they lean toward. The phone's
  // forecast hides it until Sympathy is revealed (web_server.cpp fightMods).
  if (pf >= 0 && reputationTierLocked(gs.players[slot].reputation[pf]) == REP_HOSTILE) {
    m.defMilitia = BATTLE_MILITIA_SHIPS;
    m.defSourceFaction = pf;
  }
  // The escort still needs the Sympathy: you can't call in help against a
  // faction you haven't identified.
  if (!gs.players[slot].revealed[planetIdx][0]) return m;
  // Rival support: the planet faction's enemies back your attack.
  if (reputationTierLocked(gs.players[slot].reputation[rival]) >= REP_FAVORABLE) {
    m.atkEscort = BATTLE_ESCORT_SHIPS;  // 2.0-dev.24: a ship, not a +1
    m.atkSourceFaction = rival;
  }
  // Local resentment: a garrison that hates you fights harder.
  return m;
}

// ---------------------------------------------------------------------------
// v0.30.0 THE NEGOTIATION CONVERSATION (design: the design notes, decision log
// 2026-10-19). Questions come from per-faction DECKS whose order and position
// live in flash (NVS "sfneg"), like the transmission decks -- so a question
// doesn't come back until that faction has gone through the deck. Decks per
// faction: easy, medium, hard (shared bank) and, v0.30.1, its OWN questions.
// ---------------------------------------------------------------------------
#define NEG_DECKS_PER_FACTION 4  // 3 tiers + the faction's own questions
struct NegDecks { uint32_t seed[NUM_FACTIONS * NEG_DECKS_PER_FACTION]; uint8_t pos[NUM_FACTIONS * NEG_DECKS_PER_FACTION]; };
static NegDecks negDecks;
static bool negDecksLoaded = false;

static void negDecksLoadLocked() {
  if (negDecksLoaded) return;
  negDecksLoaded = true;
  Preferences p;
  p.begin("sfneg", true);
  if (p.getBytesLength("decks") != sizeof(negDecks) || p.getBytes("decks", &negDecks, sizeof(negDecks)) != sizeof(negDecks))
    memset(&negDecks, 0, sizeof(negDecks));  // first boot, or the decks' shape changed
  p.end();
}
static void negDecksSave() {
  Preferences p;
  p.begin("sfneg", false);
  p.putBytes("decks", &negDecks, sizeof(negDecks));
  p.end();
}
// Card `pos` of a seeded shuffle of 0..count-1 (count up to 128 -- 3.1-dev.82: the hard tier has 66 now).
static int negDeckCard(uint32_t seed, int count, int pos) {
  uint8_t order[128];
  if (count > 128) count = 128;
  for (int i = 0; i < count; i++) order[i] = i;
  uint32_t x = seed ? seed : 0x9E3779B9u;
  for (int i = count - 1; i > 0; i--) {
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    const int j = x % (i + 1);
    const uint8_t t = order[i]; order[i] = order[j]; order[j] = t;
  }
  return order[pos % count];
}
static bool negAskable(int q, int faction) {
  for (int r = 0; r < 3; r++) if (NEG_BANK[q].react[r][faction] > 0) return true;
  return false;
}
// The next card of deck `d` (bank range start..start+count) that `faction` can
// ask and that isn't already in this conversation.
static int negDrawLocked(int d, int start, int count, int faction, const uint8_t *taken, int nTaken) {
  if (count <= 0) return -1;
  for (int tries = 0; tries < 2 * count + 2; tries++) {
    if (negDecks.pos[d] >= count || negDecks.seed[d] == 0) { negDecks.pos[d] = 0; negDecks.seed[d] = esp_random() | 1; }
    const int q = start + negDeckCard(negDecks.seed[d], count, negDecks.pos[d]);
    negDecks.pos[d]++;
    bool dup = false;
    for (int k = 0; k < nTaken; k++) if (taken[k] == q) dup = true;
    if (!dup && negAskable(q, faction)) return q;
  }
  return -1;
}

const char *snapshotNegotiationRefusal(const GameState &gs, int slot, int planetIdx) {
  if (slot < 0 || slot >= MAX_PLAYERS || planetIdx < 0 || planetIdx >= NUM_PLANETS) return "No such planet.";
  if (gs.planetOwner[planetIdx] >= 0 && gs.planetOwner[planetIdx] != slot) return "A commander holds it -- you'll have to fight for it.";
  if (liveHomeOf(gs, planetIdx) >= 0) return "A home world -- only a fight takes it.";
  if (gs.players[slot].negLosses[planetIdx] >= NEG_LOSSES_CLOSED) return "They won't negotiate with you here again.";
  return nullptr;  // v0.30.1: a hostile faction is a screen of its own (NEG_HOSTILE), not a refusal
}

static bool negLateLocked() { return state.round > state.roundLimit - NEG_LATE_ROUNDS; }

static void negHostileLocked(int slot, int f) {
  state.negResult[slot] = NEG_HOSTILE;
  state.negLine[slot] = (int8_t)(NEG_HOSTILE_START[f] + (NEG_HOSTILE_COUNT[f] ? esp_random() % NEG_HOSTILE_COUNT[f] : 0));
}

// Assumes mutex held. Picks the questions, what your contact tips you off on,
// and the order replies are shown in.
static void startConversationLocked(int slot, int planetIdx) {
  negDecksLoadLocked();
  const int f = planetFaction(planetIdx), ring = PLANETS[planetIdx].ring;
  const int tier = reputationTierLocked(state.players[slot].reputation[f]);
  const bool known = state.players[slot].revealed[planetIdx][0];
  const bool consul = classOfLocked(slot) == CLASS_CONSUL;  // 3.1-dev.101: "if it's going to be good at talking, it should always be good"
  state.negStep[slot] = 0;
  state.negResult[slot] = NEG_TALKING;
  state.negFavor[slot] = -1;
  state.negOfferN[slot] = 0;
  state.negLine[slot] = -1;
  state.galWhisper[slot] = -1; state.galVouch[slot] = state.galActed[slot] = 0;  // 3.1-dev.52 THE GALLERY
  if (known && tier == REP_HOSTILE && !consul) {  // v0.30.1: you knew who they were -- they won't even start (3.1-dev.101: a Consul they still hear out)
    state.negCount[slot] = 0;
    negHostileLocked(slot, f);
    Serial.printf("[NEG] P%d at %s: hostile, they won't talk\n", slot + 1, PLANETS[planetIdx].name);
    return;
  }
  int n = ring == RING_CORE ? NEG_Q_CORE : ring == RING_MID ? NEG_Q_MID : NEG_Q_OUTER;
  // 3.1-dev.7: a CONSUL is asked one fewer (never below 2), a SCOUNDREL one more (at most NEG_MAX_Q).
  if (state.negUprising[slot] == 2) n = NEG_Q_CORE;  // 3.1-dev.101 a home world: five hard questions, no misses
  else if (consul && n > 2) { n--; stampLocked(slot, STAMP_SILVER, planetIdx); }
  else if (classOfLocked(slot) == CLASS_WARLORD && n < NEG_MAX_Q) { n++; stampLocked(slot, STAMP_DISTRUST, planetIdx); }  // 3.1-dev.48: BLUNT (the Scoundrel's is MARK-UP now)
  if (snapshotEra(state) == FACTION_REBEL && n > 2) n--;  // 3.1-dev.12 HOPE SPREADS
  if (state.summitBy == slot && !state.summitBroken && state.round <= state.summitUntil && n > 2) n--;  // 3.1-dev.13 the Consul at the Summit
  for (int k = 0; k < n; k++) {
    const int qt = state.negUprising[slot] ? NEG_HARD : k == 0 ? NEG_EASY : k == 1 ? NEG_MEDIUM : NEG_HARD;  // easy, medium, then hard (3.1-dev.79: an uprising is all hard)
    int q = -1;
    if (k == 1 && known && !state.negUprising[slot])  // v0.30.1: Affiliation revealed -- the second question is one of their own
      q = negDrawLocked(f * NEG_DECKS_PER_FACTION + 3, NEG_OWN_START[f], NEG_OWN_COUNT[f], f, state.negQ[slot], k);
    if (q < 0) q = negDrawLocked(f * NEG_DECKS_PER_FACTION + qt, NEG_TIER_START[qt], NEG_TIER_COUNT[qt], f, state.negQ[slot], k);
    if (q < 0) q = NEG_TIER_START[qt];
    state.negQ[slot][k] = (uint8_t)q;
    state.negMarks[slot][k] = NEG_MARK_NONE;
    state.negPerm[slot][k] = (uint8_t)(esp_random() % 6);
    // Your contact steers you away from one reply -- an offending one, else a
    // neutral one: Favorable on the hardest (last) question, Allied on every one.
    int8_t drop = -1;
    if ((consul && k == n - 1) || tier == REP_ALLIED || (tier == REP_FAVORABLE && k == n - 1)) {  // 3.1-dev.102: a Consul's contact, on the hardest one (every one was too strong)
      for (int r = 0; r < 3 && drop < 0; r++) if (NEG_BANK[q].react[r][f] < 0) drop = r;
      for (int r = 0; r < 3 && drop < 0; r++) if (NEG_BANK[q].react[r][f] == 0) drop = r;
    }
    state.negDrop[slot][k] = drop;
  }
  negDecksSave();
  state.negCount[slot] = (uint8_t)n;
  state.negHead[slot] = 0;  // 3.1-dev.102 THE HEAD START: the first question counts as answered
  if (!consul && !state.negUprising[slot] && n > 1 && f >= 0 && f < NUM_FACTIONS && ((state.players[slot].headStart >> f) & 1)) {
    state.negMarks[slot][0] = NEG_MARK_LIKED; state.negStep[slot] = 1; state.negHead[slot] = 1;
    state.players[slot].headStart &= (uint8_t)~(1u << f);
  }
  newsTableLocked(NEWS_TALK, state.players[slot].chosenFaction, planetIdx, -1);  // 3.1-dev.52 the table: "KESS IS TALKING ON KIJIMI"
  Serial.printf("[NEG] P%d at %s (faction %d): %d questions\n", slot + 1, PLANETS[planetIdx].name, f, n);
}

static int negMissesLocked(int slot) {
  int m = 0;
  for (int k = 0; k < state.negCount[slot]; k++) if (state.negMarks[slot][k] != NEG_MARK_LIKED) m++;
  return m;
}

// v0.30.1: the favor stage draws from the bank -- 3 at random; two or more
// misses = one hard favor only; the last rounds = 1 Credits + 2 late deeds.
// v0.30.4: the hard one is drawn from the faction's several hard favors
// (playtest 2026-10-28: "it was the same every time").
// 3.1-dev.51 #19 ONLY DOABLE FAVORS (playtest 3: "destroy a fleet in 3 rounds" with no fleet anywhere): a favor is offered only
// if it can be done on paper right now (the game can't see the board, so "in reach" isn't checked).
static bool favorDoableLocked(int slot, const NegFavor &x) {
  const int arg = x.arg;
  auto worldOf = [&](int fac, bool core) {
    for (int p = 0; p < NUM_PLANETS; p++)
      if (state.planetOwner[p] != slot && (core ? PLANETS[p].ring == RING_CORE : planetFaction(p) == fac)) return true;
    return false;
  };
  switch (x.check) {
    case PROMISE_TAKE_FACTION: return arg >= 0 && worldOf(arg, false);
    case PROMISE_TAKE_CORE: return worldOf(-1, true);
    case PROMISE_HOLD_TWO: {
      if (arg < 0) return true;
      for (int p = 0; p < NUM_PLANETS; p++) if (planetFaction(p) == arg) return true;
      return false;
    }
    case PROMISE_WIN_PVP:
      for (int o = 0; o < MAX_PLAYERS; o++) {
        if (o == slot || !state.players[o].active) continue;
        for (int k = 0; k < NUM_FLEETS; k++) if (fleetAliveLocked(o, k) > 0) return true;
      }
      return false;
    case PROMISE_WIN_BATTLE: {
      for (int k = 0; k < NUM_FLEETS; k++) if (fleetAliveLocked(slot, k) > 0) return true;
      return state.players[slot].credits >= 2;  // (can still build a fleet)
    }
    case PROMISE_TRIBUTE: return state.players[slot].credits >= max(1, arg);
    default: return true;  // honor, don't-invest, don't-attack, hold, Credits
  }
}
static void negDrawFavorsLocked(int slot, int f, bool hard, bool late) {
  int pool[24], n = 0, credits[8], nc = 0, deeds[16], nd = 0, hards[8], nh = 0;
  int anyCredits = -1;  // 3.1-dev.51: the fallback when nothing else can be done
  for (int i = 0; i < NEG_FAVOR_COUNT; i++) {
    const NegFavor &x = NEG_FAVOR_BANK[i];
    if (x.faction == f && x.check == NEG_FAVOR_CREDITS && anyCredits < 0) anyCredits = i;
    if (x.faction != f || x.late != (late ? 1 : 0)) continue;
    if (x.check != NEG_FAVOR_CREDITS && !favorDoableLocked(slot, x)) continue;  // 3.1-dev.51 #19
    if (x.hard && nh < 8) hards[nh++] = i;
    if (n < 24) pool[n++] = i;
    if (x.check == NEG_FAVOR_CREDITS) { if (nc < 8) credits[nc++] = i; }
    else if (nd < 16) deeds[nd++] = i;
  }
  if (hard && nh) { state.negOffer[slot][0] = (uint8_t)hards[esp_random() % nh]; state.negOfferN[slot] = 1; return; }
  if (hard && anyCredits >= 0) { state.negOffer[slot][0] = (uint8_t)anyCredits; state.negOfferN[slot] = 1; return; }  // 3.1-dev.51: nothing hard was doable
  auto shuffleInts = [](int *a, int len) { for (int i = len - 1; i > 0; i--) { const int j = esp_random() % (i + 1); const int t = a[i]; a[i] = a[j]; a[j] = t; } };
  int m = 0;
  if (late) {
    shuffleInts(credits, nc);
    shuffleInts(deeds, nd);
    if (nc) state.negOffer[slot][m++] = (uint8_t)credits[0];
    for (int i = 0; i < nd && m < NEG_FAVORS; i++) state.negOffer[slot][m++] = (uint8_t)deeds[i];
  } else {
    shuffleInts(pool, n);
    for (int i = 0; i < n && m < NEG_FAVORS; i++) state.negOffer[slot][m++] = (uint8_t)pool[i];
  }
  if (m == 0 && anyCredits >= 0) state.negOffer[slot][m++] = (uint8_t)anyCredits;  // 3.1-dev.51: always something you can do
  state.negOfferN[slot] = (uint8_t)m;
}


// ---- 3.1-dev.52 THE GALLERY (see GAL_* in game_state.h) ----
static int negTierOf(int q) {
  for (int t = 0; t < 3; t++) if (q >= NEG_TIER_START[t] && q < NEG_TIER_START[t] + NEG_TIER_COUNT[t]) return t;
  return NEG_HARD;
}
const char *gameStateGallery(int slot, int talker, bool whisper) {
  if (g_remote || slot < 0 || slot >= MAX_PLAYERS || talker < 0 || talker >= MAX_PLAYERS) return "Bad request.";
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return "Busy.";
  const char *why = nullptr;
  const int p = state.takeoverPlanet[talker];
  const int k = state.negStep[talker] + 1;  // the NEXT question
  Player &me = state.players[slot];
  if (slot == talker || !me.active) why = "Not your talk to touch.";
  else if (p < 0 || state.takeoverMethod[talker] != TAKEOVER_NEGOTIATE || state.negResult[talker] != NEG_TALKING) why = "That talk is over.";
  else if (k >= state.negCount[talker]) why = "Too late: they're on the last question.";
  else if ((state.galActed[talker] >> slot) & 1) why = "You've already had your say in this talk.";
  else if (whisper && state.galWhisper[talker] >= 0) why = "Someone already whispered against them.";
  else if (whisper && me.credits < GAL_WHISPER_CR) why = "A whisper costs 2 CR.";
  else if (!whisper && tierOfLocked(slot, planetFaction(p)) < REP_FAVORABLE) why = "Only their friends can vouch: you're not on good enough terms.";
  else if (!whisper && state.negDrop[talker][k] >= 0) why = "They already have help on the next question.";
  if (!why) {
    const int f = planetFaction(p);
    negDecksLoadLocked();
    if (whisper) {
      me.credits -= GAL_WHISPER_CR;
      const int q = state.negQ[talker][k], t = negTierOf(q);
      if (t >= NEG_HARD && state.negDrop[talker][k] >= 0) state.negDrop[talker][k] = -1;  // already hard: their contact goes quiet
      else {
        const int nt = min(NEG_HARD, t + 1);
        const int nq = negDrawLocked(f * NEG_DECKS_PER_FACTION + nt, NEG_TIER_START[nt], NEG_TIER_COUNT[nt], f, state.negQ[talker], state.negCount[talker]);
        if (nq >= 0) { state.negQ[talker][k] = (uint8_t)nq; state.negPerm[talker][k] = (uint8_t)(esp_random() % 6); state.negDrop[talker][k] = -1; }
      }
      state.galWhisper[talker] = (int8_t)slot;
    } else {
      const int q = state.negQ[talker][k];
      int8_t drop = -1;
      for (int r = 0; r < 3 && drop < 0; r++) if (NEG_BANK[q].react[r][f] < 0) drop = r;
      for (int r = 0; r < 3 && drop < 0; r++) if (NEG_BANK[q].react[r][f] == 0) drop = r;
      state.negDrop[talker][k] = drop;
      state.galVouch[talker] |= (uint8_t)(1u << slot);
    }
    negDecksSave();
    state.galActed[talker] |= (uint8_t)(1u << slot);
    newsTableLocked(whisper ? NEWS_WHISPER : NEWS_VOUCH, me.chosenFaction, p, state.players[talker].chosenFaction);
    Serial.printf("[NEG] gallery: P%d %s P%d at %s\n", slot + 1, whisper ? "whispers against" : "vouches for", talker + 1, PLANETS[p].name);
  }
  xSemaphoreGive(stateMutex);
  return why;
}
// the talk ended: the stakes (won = vouchers up, the whisperer down; lost = the other way round)
static void gallerySettleLocked(int talker, int p, bool won) {
  const int f = planetFaction(p);
  for (int o = 0; o < MAX_PLAYERS; o++) {
    const bool v = (state.galVouch[talker] >> o) & 1, w = state.galWhisper[talker] == o;
    if (!v && !w) continue;
    const bool good = v ? won : !won;
    applyReputationLocked(o, f, good ? GAL_STAKE : -GAL_STAKE, false, TXC_DEAL, p);
    pnewsLocked(o, PN_GALLERY, f, p, (won ? 2 : 0) + (w ? 1 : 0));
  }
  state.galWhisper[talker] = -1; state.galVouch[talker] = state.galActed[talker] = 0;
}

// Assumes mutex held. The planet is yours: reputation, VP, a favor owed.
// favor = NEG_FAVOR_BANK index, or -1 (no strings). A deed's planet is worth no
// VP until it's done (v0.30.1); a paid Credits favor counts at once.
static void winConversationLocked(int slot, int planetIdx, int favor) {
  const int f = planetFaction(planetIdx);
  const bool deed = favor >= 0 && NEG_FAVOR_BANK[favor].check != NEG_FAVOR_CREDITS;
  g_takeHow = TAKE_TALK;
  setOwnerLocked(planetIdx, slot, deed);
  if (snapshotEra(state) == FACTION_REBEL && eraChampLocked(state, slot))  // the Rebellion's champion: hope holds the world
    state.planetDefense[planetIdx] = min(snapshotDefenseLimit(state, planetIdx), state.planetDefense[planetIdx] + ERA_HOPE_TALK_GARRISON);
  applyReputationLocked(slot, f, favor >= 0 ? REP_NEG_FAVOR : REP_NEG_FLAWLESS, false, TXC_DEAL, planetIdx, gainBigsLocked(planetIdx, slot));
  promiseOnGainLocked(slot, planetIdx);  // before this deal's own promise is recorded
  if (deed) {
    const NegFavor &x = NEG_FAVOR_BANK[favor];
    queueObligationLocked(slot, f, planetIdx, x.text, x.check, x.arg, (int8_t)favor);
  }
  if (classOfLocked(slot) == CLASS_CONSUL) {  // 3.1-dev.101 TREATY WORLDS
    state.treatyMask |= 1u << planetIdx; state.treatyUntil[planetIdx] = (int8_t)(state.round + CONSUL_TREATY_ROUNDS - 1);
    state.planetDefense[planetIdx] = (int8_t)min(MAX_PLANET_DEFENSE, state.planetDefense[planetIdx] + CONSUL_TREATY_DEF);
    stampLocked(slot, STAMP_TREATY, planetIdx);
  }
  state.negResult[slot] = favor >= 0 ? NEG_WON_FAVOR : NEG_WON;
  state.negFavor[slot] = (int8_t)favor;
  gallerySettleLocked(slot, planetIdx, true);  // 3.1-dev.52
  Serial.printf("[NEG] P%d won %s%s\n", slot + 1, PLANETS[planetIdx].name, deed ? " (owes a favor)" : favor >= 0 ? " (paid)" : " (no strings)");
}

// 3.1-dev.79 UPRISING won: the world is yours at once but shaky; its old holder may strike back this round (dust or not).
static void uprisingWinLocked(int slot, int planetIdx) {
  const int old = state.planetOwner[planetIdx];
  g_takeHow = TAKE_TALK;
  setOwnerLocked(planetIdx, slot);
  state.planetDefense[planetIdx] = UPRISING_DEFENDERS;
  state.upriseFrom[planetIdx] = (int8_t)(old + 1);
  state.negResult[slot] = NEG_WON;
  state.negFavor[slot] = -1;
  newsPubLocked(NEWS_UPRISING, state.players[slot].chosenFaction, planetIdx, old);
  newsTableLocked(NEWS_UPRISING, state.players[slot].chosenFaction, planetIdx, old >= 0 && old < MAX_PLAYERS ? state.players[old].chosenFaction : -1);
  gallerySettleLocked(slot, planetIdx, true);
  Serial.printf("[UPRISING] P%d took %s from P%d\n", slot + 1, PLANETS[planetIdx].name, old + 1);
}
// 3.1-dev.79 BUYOUT: the buyer's Credits are held; the holder sells (keeps half) or matches (pays the same, keeps the world).
static void buyoutOfferLocked(int slot, int planetIdx) {
  char *res = state.takeoverResult[slot]; const size_t rs = sizeof(state.takeoverResult[slot]);
  const int holder = state.planetOwner[planetIdx], price = snapshotPayPrice(state, slot, planetIdx);
  if (state.buyBy[planetIdx]) { snprintf(res, rs, "There's already an offer on %s.", PLANETS[planetIdx].name); return; }
  if (state.players[slot].credits < price) { snprintf(res, rs, "Not enough Credits: a buyout of %s costs %d.", PLANETS[planetIdx].name, price); return; }
  state.players[slot].credits -= price;
  state.buyBy[planetIdx] = (int8_t)(slot + 1); state.buyCr[planetIdx] = (uint8_t)min(255, price); state.buyRound[planetIdx] = (int8_t)state.round;
  state.players[slot].takeoverLocked[planetIdx] = true;
  newsPubLocked(NEWS_BUYOUT, state.players[slot].chosenFaction, planetIdx, slot * 4 + holder);
  newsTableLocked(NEWS_BUYOUT, state.players[slot].chosenFaction, planetIdx, state.players[holder].chosenFaction);
  pnewsLocked(holder, PN_BUYOUT_OFFER, state.players[slot].chosenFaction, planetIdx, slot);
  clearTakeoverLocked(slot);
  snprintf(res, rs, "Offer made: %d CR for %s. They answer by the next round.", price, PLANETS[planetIdx].name);
  Serial.printf("[BUYOUT] P%d offers %d CR for %s (P%d)\n", slot + 1, price, PLANETS[planetIdx].name, holder + 1);
}
static void buyoutSettleLocked(int planetIdx, bool match) {
  const int buyer = state.buyBy[planetIdx] - 1, holder = state.planetOwner[planetIdx], price = state.buyCr[planetIdx];
  state.buyBy[planetIdx] = 0; state.buyCr[planetIdx] = 0;
  if (buyer < 0 || buyer >= MAX_PLAYERS) return;
  if (match || holder < 0 || holder >= MAX_PLAYERS) {  // kept (or the world already changed hands): the buyer gets their money back
    state.players[buyer].credits += price;
    if (match && holder >= 0) { state.players[holder].credits -= price;
      newsPubLocked(NEWS_BUYOUT_KEPT, state.players[buyer].chosenFaction, planetIdx, buyer * 4 + holder);
      newsTableLocked(NEWS_BUYOUT_KEPT, state.players[holder].chosenFaction, planetIdx, state.players[buyer].chosenFaction); }
    return;
  }
  state.players[holder].credits += price / 2;
  g_takeHow = TAKE_PAY;
  setOwnerLocked(planetIdx, buyer);
  newsPubLocked(NEWS_BUYOUT_SOLD, state.players[buyer].chosenFaction, planetIdx, buyer * 4 + holder);
  newsTableLocked(NEWS_BUYOUT_SOLD, state.players[buyer].chosenFaction, planetIdx, state.players[holder].chosenFaction);
  Serial.printf("[BUYOUT] %s sold to P%d for %d (P%d got %d)\n", PLANETS[planetIdx].name, buyer + 1, price, holder + 1, price / 2);
}
bool gameStateBuyoutAnswer(int slot, int planetIdx, bool match) {
  if (g_remote || slot < 0 || slot >= MAX_PLAYERS || planetIdx < 0 || planetIdx >= NUM_PLANETS) return false;
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return false;
  bool ok = state.buyBy[planetIdx] && state.planetOwner[planetIdx] == slot && (!match || state.players[slot].credits >= state.buyCr[planetIdx]);
  if (ok) buyoutSettleLocked(planetIdx, match);
  xSemaphoreGive(stateMutex);
  return ok;
}
static void loseConversationLocked(int slot, int planetIdx, int result) {
  alertAfterTalkLocked(slot, planetIdx);  // 3.1-dev.101 negotiate, then fight
  if (state.players[slot].negLosses[planetIdx] < 255) state.players[slot].negLosses[planetIdx]++;
  applyReputationLocked(slot, planetFaction(planetIdx), result == NEG_CUT_OFF ? REP_NEG_CUTOFF : REP_NEG_REFUSE, false,
                        TXC_WALKOUT, planetIdx);
  state.negResult[slot] = (uint8_t)result;
  gallerySettleLocked(slot, planetIdx, false);  // 3.1-dev.52
  Serial.printf("[NEG] P%d lost at %s (%s)\n", slot + 1, PLANETS[planetIdx].name, result == NEG_CUT_OFF ? "cut off" : "refused the favor");
}

static void uprisingWinLocked(int slot, int planetIdx);
void gameStateNegotiationAnswer(int slot, int choice) {
  if (g_remote) { remoteCall(RF_CHOOSE_OFFER, {slot, choice}); return; }
  if (slot < 0 || slot >= MAX_PLAYERS) return;
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return;
  const int p = state.takeoverPlanet[slot];
  if (p >= 0 && state.takeoverMethod[slot] == TAKEOVER_NEGOTIATE && state.negResult[slot] == NEG_TALKING && state.negCount[slot] > 0) {
    const int f = planetFaction(p), k = state.negStep[slot];
    if (k < state.negCount[slot]) {  // a question
      if (choice >= 0 && choice < 3 && choice != state.negDrop[slot][k]) {
        const int re = NEG_BANK[state.negQ[slot][k]].react[choice][f];
        state.negMarks[slot][k] = re > 0 ? NEG_MARK_LIKED : re < 0 ? NEG_MARK_OFFENDED : NEG_MARK_NEUTRAL;
        state.negStep[slot]++;
        int offends = 0;
        for (int j = 0; j <= k; j++) if (state.negMarks[slot][j] == NEG_MARK_OFFENDED) offends++;
        const bool done = state.negStep[slot] >= state.negCount[slot];
        if (state.negUprising[slot] && state.negMarks[slot][k] != NEG_MARK_LIKED) {  // 3.1-dev.79: one wrong word and the people stay home
          state.negResult[slot] = NEG_ENDED; gallerySettleLocked(slot, p, false); alertAfterTalkLocked(slot, p);  // 3.1-dev.101
          Serial.printf("[UPRISING] P%d failed at %s\n", slot + 1, PLANETS[p].name);
        }
        else if (state.negUprising[slot] && done) uprisingWinLocked(slot, p);
        else if (offends >= NEG_CUTOFF_OFFENDS) loseConversationLocked(slot, p, NEG_CUT_OFF);
        else if (done && reputationTierLocked(state.players[slot].reputation[f]) == REP_HOSTILE && classOfLocked(slot) != CLASS_CONSUL) {
          // v0.30.1: the unknown ambassador was hostile all along -- however well
          // you did, the mask comes off. Affiliation revealed free, nothing lost.
          state.players[slot].revealed[p][0] = true;
          negHostileLocked(slot, f);
          gallerySettleLocked(slot, p, false);  // 3.1-dev.52
          Serial.printf("[NEG] P%d at %s: the ambassador was hostile\n", slot + 1, PLANETS[p].name);
        }
        else if (done && negMissesLocked(slot) == 0) winConversationLocked(slot, p, -1);
        else if (done) negDrawFavorsLocked(slot, f, negMissesLocked(slot) >= 2, negLateLocked());
        // else: the next question
      }
    } else {  // the favor stage
      state.takeoverResult[slot][0] = '\0';
      if (choice == NEG_REFUSE) loseConversationLocked(slot, p, NEG_ENDED);
      else if (choice >= 0 && choice < state.negOfferN[slot]) {
        const int fav = state.negOffer[slot][choice];
        if (NEG_FAVOR_BANK[fav].check == NEG_FAVOR_CREDITS) {
          const int price = snapshotLateFavorPrice(state, slot, p);
          if (state.players[slot].credits < price) {
            snprintf(state.takeoverResult[slot], sizeof(state.takeoverResult[slot]), "You have %d CR -- that costs %d.", state.players[slot].credits, price);
          } else {
            state.players[slot].credits -= price;
            winConversationLocked(slot, p, fav);
          }
        } else {
          winConversationLocked(slot, p, fav);
        }
      }
    }
  }
  xSemaphoreGive(stateMutex);
}

void gameStateNegotiationClose(int slot) {
  if (g_remote) { remoteCall(RF_NEG_CLOSE, {slot}); return; }
  if (slot < 0 || slot >= MAX_PLAYERS) return;
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return;
  if (state.takeoverMethod[slot] == TAKEOVER_NEGOTIATE && state.negResult[slot] != NEG_TALKING) clearTakeoverLocked(slot);
  xSemaphoreGive(stateMutex);
}

// v0.30.1: they're hostile -- FIGHT instead. The one time you may switch from
// talking to fighting at a planet in the same round. Leaves an open takeover
// session on the planet so the screens can go straight to the battle forecast.
void gameStateNegotiationFight(int slot) {
  if (g_remote) { remoteCall(RF_NEG_FIGHT, {slot}); return; }
  if (slot < 0 || slot >= MAX_PLAYERS) return;
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return;
  const int p = state.takeoverPlanet[slot];
  if (p >= 0 && state.takeoverMethod[slot] == TAKEOVER_NEGOTIATE && state.negResult[slot] == NEG_HOSTILE) {
    clearTakeoverLocked(slot);
    state.players[slot].takeoverLocked[p] = false;
    state.takeoverPlanet[slot] = p;
  }
  xSemaphoreGive(stateMutex);
}

// The order replies are shown in: one of the six permutations of 0..2.
static const uint8_t NEG_PERMS[6][3] = { { 0, 1, 2 }, { 0, 2, 1 }, { 1, 0, 2 }, { 1, 2, 0 }, { 2, 0, 1 }, { 2, 1, 0 } };

NegView snapshotNegotiation(const GameState &gs, int slot) {
  NegView v;
  if (slot < 0 || slot >= MAX_PLAYERS) return v;
  const int p = gs.takeoverPlanet[slot];
  if (p < 0 || p >= NUM_PLANETS || gs.takeoverMethod[slot] != TAKEOVER_NEGOTIATE) return v;
  if (gs.negCount[slot] == 0 && gs.negResult[slot] != NEG_HOSTILE) return v;
  v.open = true;
  v.planet = p;
  v.faction = gs.planetFactionOf[p];
  v.known = gs.players[slot].revealed[p][0];
  v.count = gs.negCount[slot];
  v.step = gs.negStep[slot];
  v.result = gs.negResult[slot];
  v.favorTaken = gs.negFavor[slot];
  v.losses = gs.players[slot].negLosses[p];
  v.hostileLine = gs.negLine[slot];
  v.roundsLeft = max(0, gs.roundLimit - gs.round);
  v.late = gs.round > gs.roundLimit - NEG_LATE_ROUNDS;
  v.lateCredits = snapshotLateFavorPrice(gs, slot, p);
  int misses = 0;
  for (int k = 0; k < NEG_MAX_Q; k++) {
    v.marks[k] = k < v.count ? gs.negMarks[slot][k] : NEG_MARK_NONE;
    if (k < v.count && k < v.step && v.marks[k] != NEG_MARK_LIKED) misses++;
  }
  if (v.result != NEG_TALKING) return v;
  if (v.step < v.count) {
    const int k = v.step;
    v.question = gs.negQ[slot][k];
    v.tipped = gs.negDrop[slot][k] >= 0;
    for (int i = 0; i < 3; i++) {
      const int r = NEG_PERMS[gs.negPerm[slot][k] % 6][i];
      if (r != gs.negDrop[slot][k]) v.replies[v.nReplies++] = r;
    }
  } else {
    v.favorStage = true;
    v.hard = misses >= 2;
    v.nFavors = gs.negOfferN[slot];
    for (int i = 0; i < v.nFavors && i < NEG_FAVORS; i++) v.favors[i] = gs.negOffer[slot][i];
  }
  return v;
}

// Assumes mutex already held. fleetIdx only matters for TAKEOVER_FIGHT.
static void summitBreakLocked(int slot, int planet);  // 3.1-dev.13, below
static void buyoutOfferLocked(int slot, int planetIdx);  // 3.1-dev.79 (with the conversation code)
static void coverBlownLocked(int slot);
static void commitTakeoverLocked(int slot, int method, int fleetIdx) {
  if (state.takeoverPlanet[slot] < 0) return;
  int planetIdx = state.takeoverPlanet[slot];
  // v0.28.3 (playtest 2026-10-19: "you shouldn't be able to negotiate or pay for a
  // planet owned by another player. you have to face their defenses").
  const bool held = state.planetOwner[planetIdx] >= 0 && state.planetOwner[planetIdx] != slot && liveHomeOf(state, planetIdx) < 0;  // 3.1-dev.79
  state.negUprising[slot] = 0;
  if (method != TAKEOVER_FIGHT && state.players[slot].alertRound == state.round && state.players[slot].alertWorld == planetIdx) {  // 3.1-dev.101
    snprintf(state.takeoverResult[slot], sizeof(state.takeoverResult[slot]), "The talks failed. %s is on alert: only a fight now.", PLANETS[planetIdx].name); return;
  }
  if (method == TAKEOVER_PAY && ((state.treatyMask >> planetIdx) & 1) && state.treatyUntil[planetIdx] >= state.round) {  // 3.1-dev.101 TREATY WORLDS
    snprintf(state.takeoverResult[slot], sizeof(state.takeoverResult[slot]), "%s is under a Consul's treaty: no bribes, no buyouts until round %d.", PLANETS[planetIdx].name, state.treatyUntil[planetIdx] + 1); return;
  }
  if (held && method != TAKEOVER_FIGHT && snapshotSummitCovers(state, planetIdx)) {  // 3.1-dev.114 (user: a buyout being closed makes sense, uprising too)
    snprintf(state.takeoverResult[slot], sizeof(state.takeoverResult[slot]), "%s is under a ceasefire: no buyouts, no uprisings.", PLANETS[planetIdx].name); return;
  }
  if (held && method == TAKEOVER_PAY) { buyoutOfferLocked(slot, planetIdx); return; }
  if (held && method == TAKEOVER_NEGOTIATE) {
    // 3.1-dev.114 (playtest: "uprising being friend doesn't make a ton of sense"): anyone may try; the hard questions stay.
    if (((state.players[slot].upriseTried >> planetIdx) & 1) && snapshotEra(state) != FACTION_REBEL) { snprintf(state.takeoverResult[slot], sizeof(state.takeoverResult[slot]), "You've tried an uprising here already."); return; }  // 3.1-dev.114 THE PEOPLE RISE: try again in the Rebellion
    state.players[slot].upriseTried |= 1u << planetIdx;
    state.negUprising[slot] = 1;
  }
  // 3.1-dev.101 (user: "can you talk to home planets as consul or no? that ruined my game too"): a CONSUL may -- five hard
  // questions, no misses, one try per home world; won = theirs at once but shaky (UPRISING_DEFENDERS), like an uprising.
  if (method == TAKEOVER_NEGOTIATE && liveHomeOf(state, planetIdx) >= 0 && classOfLocked(slot) == CLASS_CONSUL && state.planetOwner[planetIdx] != slot) {
    if ((state.players[slot].upriseTried >> planetIdx) & 1) { snprintf(state.takeoverResult[slot], sizeof(state.takeoverResult[slot]), "You've tried to talk %s over already.", PLANETS[planetIdx].name); return; }
    state.players[slot].upriseTried |= 1u << planetIdx;
    state.negUprising[slot] = 2;
  }
  if ((method == TAKEOVER_PAY || method == TAKEOVER_NEGOTIATE) && state.planetOwner[planetIdx] >= 0
      && state.planetOwner[planetIdx] != slot && !state.negUprising[slot]) {
    snprintf(state.takeoverResult[slot], sizeof(state.takeoverResult[slot]), "A commander holds it -- you'll have to fight for it.");
    return;
  }
  if ((method == TAKEOVER_PAY || method == TAKEOVER_NEGOTIATE) && liveHomeOf(state, planetIdx) >= 0 && !state.negUprising[slot]) {  // 2.0 home worlds
    snprintf(state.takeoverResult[slot], sizeof(state.takeoverResult[slot]), "A home world -- only a fight takes it.");
    return;
  }
  if (snapshotDealBlock(state, slot, planetIdx, method, state.takeoverResult[slot], sizeof(state.takeoverResult[slot])) >= 0) return;  // 2.0 deals
  // 3.1-dev.36 (playtest 2026-11-08: two players started TALK on the same world at once -- one won it, then the
  // other "won" it too): the first to commit to a world has it while their talk / purchase / fight runs.
  for (int o = 0; o < MAX_PLAYERS; o++)
    if (o != slot && state.players[o].active && state.takeoverPlanet[o] == planetIdx && state.takeoverMethod[o] != TAKEOVER_NONE &&
        !(state.takeoverMethod[o] == TAKEOVER_NEGOTIATE && state.negResult[o] != NEG_TALKING)) {
      snprintf(state.takeoverResult[slot], sizeof(state.takeoverResult[slot]), "Someone got there first -- they're dealing with %s right now.", PLANETS[planetIdx].name);
      return;
    }
  state.takeoverMethod[slot] = method;
  char *result = state.takeoverResult[slot];
  size_t resultSize = sizeof(state.takeoverResult[slot]);

  if (method == TAKEOVER_FIGHT && snapshotFhFinal(state)) {  // 3.1-dev.43
    state.takeoverMethod[slot] = TAKEOVER_NONE;
    snprintf(state.takeoverResult[slot], sizeof(state.takeoverResult[slot]), "The Final Hour decides every fight -- give your Last Orders.");
    return;
  }
  bool keepers = false, blow = false;
  if (method == TAKEOVER_FIGHT) {  // 3.1-dev.13: a Summit's ceasefire, your deep cover -- both need ATTACK ANYWAY first
    const bool confirmed = state.players[slot].brkPlanet == planetIdx && state.players[slot].brkRound == state.round;
    if (snapshotSummitCovers(state, planetIdx) && !confirmed) {
      state.takeoverMethod[slot] = TAKEOVER_NONE;
      snprintf(state.takeoverResult[slot], sizeof(state.takeoverResult[slot]), "Ceasefire: %s is under the Summit.", PLANETS[planetIdx].name);
      return;
    }
    const int cf = state.players[slot].coverFaction;
    if (cf >= 0 && planetFaction(planetIdx) == cf && !confirmed) {
      state.takeoverMethod[slot] = TAKEOVER_NONE;
      snprintf(state.takeoverResult[slot], sizeof(state.takeoverResult[slot]), "Your cover: attacking a %s world blows it.", factionName(cf));
      return;
    }
    keepers = snapshotSummitCovers(state, planetIdx);
    blow = cf >= 0 && planetFaction(planetIdx) == cf;
  }
  if (method == TAKEOVER_FIGHT) {
    const char *refusal = startBattleLocked(slot, planetIdx, fleetIdx);
    if (!refusal && keepers) { summitBreakLocked(slot, planetIdx); state.battleDefExtra += SUMMIT_KEEPERS; }  // peacekeepers join the defense
    if (!refusal && blow) coverBlownLocked(slot);
    if (!refusal) {
      result[0] = '\0';
    } else {
      // Refused: stay on the approach menu with the reason.
      state.takeoverMethod[slot] = TAKEOVER_NONE;
      snprintf(result, resultSize, "%s", refusal);
    }
  } else if (method == TAKEOVER_PAY) {
    int price = snapshotPayPrice(state, slot, planetIdx);
    if (snapshotPayShut(state, slot)) {  // 3.1-dev.12 The Rebellion Rises: THE PEOPLE WON'T BE BOUGHT
      state.takeoverMethod[slot] = TAKEOVER_NONE;
      snprintf(result, resultSize, "The Rebellion Rises: the people won't be bought.");
    } else if (state.planetUnbound[planetIdx]) {  // EV_UNBOUND: droids take no bribes
      state.takeoverMethod[slot] = TAKEOVER_NONE;
      snprintf(result, resultSize, "The Unbound take no Credits. Fight or negotiate.");
    } else if (state.players[slot].credits >= price) {
      state.players[slot].credits -= price;
      state.players[slot].permitDiscount = 0;  // GOOD_PERMITS used up
      g_takeHow = TAKE_PAY;
      setOwnerLocked(planetIdx, slot);  // v0.30.1: its VP comes with it
      if (state.inkRound[planetIdx] == state.round && state.inkBy[planetIdx] - 1 == slot) state.inkCr[planetIdx] = (uint8_t)min(255, price);  // 3.1-dev.102
      if (state.players[slot].purchases < 255) state.players[slot].purchases++;  // v0.30.0: the next one costs more
      applyReputationLocked(slot, factionIndexForPlanet(planetIdx), REP_PAY, false, TXC_PAID, planetIdx, gainBigsLocked(planetIdx, slot));
      promiseOnGainLocked(slot, planetIdx);  // v0.28.1
      // Purchase done -- close the session, same as a successful
      // negotiation. (Previously left open, which only went unnoticed
      // because the next player's takeover overwrote the global session.)
      clearTakeoverLocked(slot);
    } else {
      snprintf(result, resultSize, "Not enough Credits to purchase %s.", PLANETS[planetIdx].name);
    }
  } else if (method == TAKEOVER_NEGOTIATE) {
    if (const char *why = state.negUprising[slot] ? nullptr : snapshotNegotiationRefusal(state, slot, planetIdx)) {
      state.takeoverMethod[slot] = TAKEOVER_NONE;
      snprintf(result, resultSize, "%s", why);
      return;
    }
    startConversationLocked(slot, planetIdx);
    // Hearing the first question commits you (v0.20.2): backing out now closes
    // this planet to you for the round instead of letting you switch to Pay/Fight.
    state.players[slot].takeoverLocked[planetIdx] = true;
  }
}

void gameStateCommitTakeover(int slot, int method) {
  if (g_remote) { remoteCall(RF_COMMIT_TAKEOVER, {slot, method}); return; }
  if (slot < 0 || slot >= MAX_PLAYERS) return;
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return;
  commitTakeoverLocked(slot, method, -1);
  xSemaphoreGive(stateMutex);
}

void gameStateCommitFight(int slot, int fleetIdx) {  // fleetIdx: a fleet MASK since v0.28.0
  if (g_remote) { remoteCall(RF_COMMIT_FIGHT, {slot, fleetIdx}); return; }
  if (slot < 0 || slot >= MAX_PLAYERS) return;
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return;
  commitTakeoverLocked(slot, TAKEOVER_FIGHT, fleetIdx);
  xSemaphoreGive(stateMutex);
}

// ---- Battle ----
// Resolves one exchange of the active battle when the required action(s) are in.
// "4" or "3+1" -- a roll shown as die + modifier, so the text always
// matches the die face the battle scene shows.
static void fmtRoll(char *buf, size_t n, int die, int mod) {
  if (mod > 0) snprintf(buf, n, "%d+%d", die, mod);
  else snprintf(buf, n, "%d", die);
}

static void secondChanceLocked();  // 3.1-dev.77 (below)
// 3.1-dev.101 RALLY (playtest 2026-11-20: "maybe you shouldn't be able to lose x in a row"): a side that lost RALLY_AFTER
// exchanges in a row gets +1 on its next roll (stamped), then the count starts over. Ties don't break a run. Battle-local:
// saves never happen mid-battle, so this needs no room in GameState.
// 3.1-dev.102 (user: "a little too much one sided now"; A + B): THE BIGGER SIDE -- FH_HELP2+ more ships at the first exchange --
// WINS TIES, and its luck can't run as long: after STEADY_AFTER lost exchanges in a row it rolls best of 2 once. Planet battles
// only (PvP stays plain d6), invisible, counted in the forecast. Rally (dev.101) folded in here.
static int8_t g_big = 0;      // 0 none, 1 attacker, 2 defender
static uint8_t g_bigRun = 0;  // the bigger side's lost exchanges in a row
static void bigTieLocked(int &atkRoll, int &defRoll) {
  if (atkRoll == defRoll) { if (g_big == 1) atkRoll++; else if (g_big == 2) defRoll++; }
  const bool bigLost = (g_big == 1 && defRoll > atkRoll) || (g_big == 2 && atkRoll > defRoll);
  if (bigLost) { if (g_bigRun < 255) g_bigRun++; } else if (atkRoll != defRoll) g_bigRun = 0;
}
// 3.1-dev.101 OUTNUMBERED (was the invisible help, unclaimed worlds only; playtest 2026-11-20: "if you come into the battle with
// imaginary dice, they should be a little more durable"): the side with FH_HELP2+ more ships at the FIRST exchange rolls 2 dice and
// keeps the best (FH_HELP3+: 3), for the WHOLE battle -- every PLANET battle (held worlds too). Never PvP (user: "the only purely
// even ones should be ship vs ship pvp battles"). Never shown, but counted in the forecast.
static void helpDiceLocked(int &atkDie, int &defDie, int mine, int theirs, int defSlot) {
  (void)defSlot;
  if (state.battleExchange == 0) { const int lead = mine - theirs; g_big = lead >= FH_HELP2 ? 1 : -lead >= FH_HELP2 ? 2 : 0; g_bigRun = 0; }
  if (g_big && g_bigRun >= STEADY_AFTER) {  // the steady hand
    if (g_big == 1) atkDie = max(atkDie, rollDie()); else defDie = max(defDie, rollDie());
    g_bigRun = 0;
  }
}
static void resolvePlanetExchangeLocked() {
  // Attacker fights (garrison always fights). Ties = no damage. v0.20.1:
  // the old flat +1 attacker bonus is gone -- the only modifiers are the
  // faction ones fixed at battle start (snapshotPlanetBattleMods).
  // 3.1-dev.45 #9 INVISIBLE HELP (user 2026-11-11: "make someone think the dice are in their favor"): against an UNCLAIMED
  // world the bigger side quietly rolls best of 2 (FH_HELP2+ more ships) or best of 3 (FH_HELP3+), recounted every exchange;
  // the player's escort + hired ships count for them, the militia + a Consul's extra for the garrison. Only the die that
  // counts is shown. Never for a player-held world, PvP, the War Room or a withdrawal's parting shot. Never mentioned.
  int atkDie = rollDie();
  int defDie = rollDie();
  helpDiceLocked(atkDie, defDie, aliveCount(state.atkAlive) + (state.battleEscortAlive ? 1 : 0) + state.battleAtkMercs + state.battleAtkExtra,
                 state.defRemaining + state.battleDefExtra, state.planetOwner[state.battleTargetPlanet]);  // 3.1-dev.101 OUTNUMBERED
  applyRerollsLocked(atkDie, defDie);  // 2.0
  int atkRoll = atkDie + state.battleAtkMod;
  int defRoll = defDie + state.battleDefMod;
  state.lastAtkRoll = atkRoll;
  state.lastDefRoll = defRoll;
  state.battleExchange++;
  state.lastLossIdx = -1;
  state.lastRetreatSide = 0;
  if (state.battleExchange == 1 && atkRoll == defRoll) {  // 3.1-dev.15 AMBUSHERS: the Dawn fires first
    if (state.battleQuirk & 16) { atkRoll++; stampLocked(state.battleAttackerSlot, STAMP_FIRSTSHOT); }
    else if (state.battleQuirk & 32) { defRoll++; stampLocked(state.battleDefenderSlot, STAMP_FIRSTSHOT); }
  }
  bigTieLocked(atkRoll, defRoll);  // 3.1-dev.102
  char a[8], d[8];
  fmtRoll(a, sizeof(a), atkDie, state.battleAtkMod);
  fmtRoll(d, sizeof(d), defDie, state.battleDefMod);

  if (atkRoll > defRoll && state.battleDefExtra) {  // 3.1-dev.7: a Consul's soft hands -- their extra ship goes first
    state.battleDefExtra--;
    snprintf(state.resultText, sizeof(state.resultText), "You %s vs Def %s -- a defending ship goes down!", a, d);
  } else if (atkRoll > defRoll) {
    state.defRemaining -= 1;
    if (state.battleMilitiaAlive) {  // 2.0-dev.24: the militia falls first
      state.battleMilitiaAlive--;
      snprintf(state.resultText, sizeof(state.resultText), "You %s vs Def %s -- the local militia is down!", a, d);
    } else if (state.battlePatrolAlive) {  // 3.1-dev.76: then the hired patrol
      state.battlePatrolAlive--;
      snprintf(state.resultText, sizeof(state.resultText), "You %s vs Def %s -- a patrol ship is down!", a, d);
    } else
    snprintf(state.resultText, sizeof(state.resultText),
             "You %s vs Def %s -- a defender destroyed! (%d left)", a, d, state.defRemaining);
  } else if (defRoll > atkRoll) {
    int idx = lossIndex(state.atkAlive);
    if (coverLossLocked(true)) {  // 2.0-dev.24/26: hired ships, then the escort, take the first hits
      snprintf(state.resultText, sizeof(state.resultText), "You %s vs Def %s -- your %s was destroyed!", a, d,
               state.lastLossIdx == ESCORT_LOSS_IDX ? "escort" : "hired gunship");
    } else if (idx >= 0 && (state.battleQuirk & 1)) {  // 3.1-dev.15 ARMORED CARGO
      state.battleQuirk &= (uint8_t)~1;
      stampLocked(state.battleAttackerSlot, STAMP_SHRUGGED);
      snprintf(state.resultText, sizeof(state.resultText), "You %s vs Def %s -- armored cargo: the hit is shrugged off!", a, d);
    } else if (idx >= 0) {
      state.atkAlive[idx] = false;
      state.lastLossIdx = idx;
      snprintf(state.resultText, sizeof(state.resultText),
               "You %s vs Def %s -- your %s was destroyed!", a, d, shipNameOf(idx));
    }
  } else {
    snprintf(state.resultText, sizeof(state.resultText),
             "You %s vs Def %s -- tie, no damage.", a, d);
  }

  if (state.defRemaining <= 0) {
    state.battleOver = true;
    rebelWinLocked(state.battleAttackerSlot, state.planetOwner[state.battleTargetPlanet], state.battleTargetPlanet);  // 2.0 (before the owner changes)
    // 3.1-dev.36 (playtest 2026-11-08: last round, every Rebel world was held by a player; taking one back cost ALL
    // Rebel favor): a world taken from another PLAYER isn't an attack on its faction -- no conquest drop, no
    // betrayal, no rival cheer. Only a faction's own (unclaimed) worlds count against you.
    const bool fromPlayer = state.planetOwner[state.battleTargetPlanet] >= 0 && state.planetOwner[state.battleTargetPlanet] != state.battleAttackerSlot;
    g_takeHow = TAKE_FIGHT;
    setOwnerLocked(state.battleTargetPlanet, state.battleAttackerSlot);  // v0.30.1: its VP changes hands
    promiseOnBattleWinLocked(state.battleAttackerSlot);
    promiseOnGainLocked(state.battleAttackerSlot, state.battleTargetPlanet);  // v0.28.1
    snprintf(state.resultText, sizeof(state.resultText),
             "DEFENSE DESTROYED! %s captured!", PLANETS[state.battleTargetPlanet].name);
    int fIdx = factionIndexForPlanet(state.battleTargetPlanet);
    // v0.23.0: the planet's faction speaks first (a big moment if this is
    // the war's first capture, their seat of power, your first Core world,
    // or a betrayal), then their rival -- both queue, neither overwrites.
    const int pIdx = state.battleTargetPlanet, atk = state.battleAttackerSlot;
    uint8_t bigs = gainBigsLocked(pIdx, atk);
    if (!fromPlayer && reputationTierLocked(state.players[atk].reputation[fIdx]) >= REP_FAVORABLE) bigs |= 1u << TXB_BETRAYAL;
    if (!state.firstBloodDone) bigs |= 1u << TXB_FIRSTBLOOD;
    state.firstBloodDone = true;  // only the war's very first capture counts, even if a bigger moment wins
    if (fromPlayer) applyReputationLocked(atk, fIdx, 0, false, TXC_TOOK, pIdx, bigs);  // no favor moves; big moments still play
    else {
      applyReputationLocked(atk, fIdx, REP_CONQUEST, true, TXC_TOOK, pIdx, bigs);
      const bool muscle = classOfLocked(atk) == CLASS_WARLORD;  // 3.1-dev.47 THE MUSCLE (was: Feared, -10 with the world's faction)
      applyReputationLocked(atk, rivalFactionIndex(fIdx), muscle ? CLASS_WARLORD_RIVAL : REP_RIVAL_GAIN, false, TXC_RIVAL, pIdx);
      if (muscle) stampLocked(atk, STAMP_FEARED, pIdx);
    }
    warSpoilsLocked(atk, fIdx, pIdx);  // 3.1-dev.93 (after the world's own transmission)
    // v0.25.0 faction alert: the faction you took it from braces its other
    // unclaimed worlds -- the more you hit one faction, the harder it gets.
    for (int q = 0; q < NUM_PLANETS; q++)
      if (q != pIdx && state.planetOwner[q] == PLANET_UNCONTROLLED && factionIndexForPlanet(q) == fIdx)
        state.planetDefense[q] = min(snapshotDefenseLimit(state, q), state.planetDefense[q] + FACTION_ALERT_GARRISON);
  } else if (!anyAlive(state.atkAlive) && !state.battleEscortAlive && !state.battleAtkMercs) {
    state.battleOver = true;
    snprintf(state.resultText, sizeof(state.resultText),
             "YOUR FLEET DESTROYED! %s holds.", PLANETS[state.battleTargetPlanet].name);
    if (state.battleQuirk & 4) {  // 3.1-dev.15 SLIPPERY: one ship escapes
      state.battleQuirk &= (uint8_t)~4;
      const int k = (state.lastLossIdx >= 0 && state.lastLossIdx < BATTLE_SHIPS) ? state.lastLossIdx : 0;
      state.atkAlive[k] = true;
      stampLocked(state.battleAttackerSlot, STAMP_ESCAPED);
      snprintf(state.resultText, sizeof(state.resultText), "Beaten -- but one ship slipped away. %s holds.", PLANETS[state.battleTargetPlanet].name);
    }
    // A real defeat here -- unlock the true garrison number for this
    // player at this planet going forward, instead of just the tier.
    state.players[state.battleAttackerSlot].hasLostAt[state.battleTargetPlanet] = true;
  }
  secondChanceLocked();  // 3.1-dev.77
}

static void resolvePvpExchangeLocked() {
  // Both sides already chose FIGHT (withdraw handled by caller). Roll; loser of
  // the roll loses their first surviving ship. Tie = no damage.
  int atkDie = rollDie();
  int defDie = rollDie();
  applyRerollsLocked(atkDie, defDie);  // 2.0
  int atkRoll = atkDie + state.battleAtkMod;  // 2.0: GOOD_MERCENARIES (0 otherwise -- PvP is plain d6)
  int defRoll = defDie + state.battleDefMod;
  state.lastAtkRoll = atkRoll;
  state.lastDefRoll = defRoll;
  state.battleExchange++;
  state.lastLossIdx = -1;
  state.lastRetreatSide = 0;
  if (state.battleExchange == 1 && atkRoll == defRoll) {  // 3.1-dev.15 AMBUSHERS: the Dawn fires first
    if (state.battleQuirk & 16) { atkRoll++; stampLocked(state.battleAttackerSlot, STAMP_FIRSTSHOT); }
    else if (state.battleQuirk & 32) { defRoll++; stampLocked(state.battleDefenderSlot, STAMP_FIRSTSHOT); }
  }

  if (atkRoll > defRoll) {
    if (coverLossLocked(false)) {  // 2.0-dev.26: their hired ships go first
      snprintf(state.resultText, sizeof(state.resultText), "You %d vs %d -- their hired gunship destroyed!", atkRoll, defRoll);
    } else if (state.battleQuirk & 2) {  // 3.1-dev.15 ARMORED CARGO (defender)
      state.battleQuirk &= (uint8_t)~2; stampLocked(state.battleDefenderSlot, STAMP_SHRUGGED);
      snprintf(state.resultText, sizeof(state.resultText), "You %d vs %d -- their armored cargo shrugs it off!", atkRoll, defRoll);
    } else {
      int idx = lossIndex(state.defAlive);
      if (idx >= 0) { state.defAlive[idx] = false; state.lastLossIdx = idx; }
      snprintf(state.resultText, sizeof(state.resultText),
               "You %d vs %d -- enemy ship destroyed!", atkRoll, defRoll);
    }
  } else if (defRoll > atkRoll) {
    if (coverLossLocked(true)) {
      snprintf(state.resultText, sizeof(state.resultText), "You %d vs %d -- your hired gunship destroyed!", atkRoll, defRoll);
    } else if (state.battleQuirk & 1) {  // 3.1-dev.15 ARMORED CARGO (attacker)
      state.battleQuirk &= (uint8_t)~1; stampLocked(state.battleAttackerSlot, STAMP_SHRUGGED);
      snprintf(state.resultText, sizeof(state.resultText), "You %d vs %d -- armored cargo: shrugged off!", atkRoll, defRoll);
    } else {
      int idx = lossIndex(state.atkAlive);
      if (idx >= 0) { state.atkAlive[idx] = false; state.lastLossIdx = idx; }
      snprintf(state.resultText, sizeof(state.resultText),
               "You %d vs %d -- your ship destroyed!", atkRoll, defRoll);
    }
  } else {
    snprintf(state.resultText, sizeof(state.resultText),
             "You %d vs %d -- tie, no damage.", atkRoll, defRoll);
  }

  if (!anyAlive(state.defAlive) && (state.battleQuirk & 8)) {  // 3.1-dev.15 SLIPPERY: beaten, but one ship escapes (no kill)
    state.battleQuirk &= (uint8_t)~8; state.battleOver = true;
    state.defAlive[(state.lastLossIdx >= 0 && state.lastLossIdx < BATTLE_SHIPS) ? state.lastLossIdx : 0] = true;
    stampLocked(state.battleDefenderSlot, STAMP_ESCAPED);
    snprintf(state.resultText, sizeof(state.resultText), "Beaten -- but one of their ships slipped away!");
  } else if (!anyAlive(state.atkAlive) && (state.battleQuirk & 4)) {
    state.battleQuirk &= (uint8_t)~4; state.battleOver = true;
    state.atkAlive[(state.lastLossIdx >= 0 && state.lastLossIdx < BATTLE_SHIPS) ? state.lastLossIdx : 0] = true;
    stampLocked(state.battleAttackerSlot, STAMP_ESCAPED);
    snprintf(state.resultText, sizeof(state.resultText), "Beaten -- but one ship slipped away!");
  } else if (!anyAlive(state.defAlive)) {
    state.battleOver = true;
    snprintf(state.resultText, sizeof(state.resultText), "ENEMY FLEET DESTROYED!");
    if (state.battleAttackerSlot >= 0) state.players[state.battleAttackerSlot].vp += VP_PVP_DECISIVE_WIN;
    bountyLocked(state.battleAttackerSlot, state.battleDefenderSlot);       // 2.0
    rebelWinLocked(state.battleAttackerSlot, state.battleDefenderSlot, -1); // 2.0
    fleetDestroyedBigLocked(state.battleAttackerSlot, state.battleDefenderSlot);
  } else if (!anyAlive(state.atkAlive)) {
    state.battleOver = true;
    snprintf(state.resultText, sizeof(state.resultText), "YOUR FLEET DESTROYED!");
    if (state.battleDefenderSlot >= 0) state.players[state.battleDefenderSlot].vp += VP_PVP_DECISIVE_WIN;
    bountyLocked(state.battleDefenderSlot, state.battleAttackerSlot);       // 2.0
    rebelWinLocked(state.battleDefenderSlot, state.battleAttackerSlot, -1); // 2.0
    fleetDestroyedBigLocked(state.battleDefenderSlot, state.battleAttackerSlot);
  }

  // Reset actions for the next exchange.
  state.atkAction = 0;
  state.defAction = 0;
  secondChanceLocked();  // 3.1-dev.77
}

// Assumes mutex held. Retreat under fire (v0.20.2): withdrawing isn't free.
// One last exchange where only the side that STAYS (the pursuer) can deal
// damage -- if the pursuer out-rolls the retreating side, the retreating
// fleet loses a ship on the way out. Ends the battle either way. Planet
// battles keep their faction modifiers; PvP is plain d6.
static void resolveRetreatLocked(bool attackerRetreats) {
  int atkDie = rollDie();
  int defDie = rollDie();
  int atkRoll = atkDie + state.battleAtkMod;
  int defRoll = defDie + state.battleDefMod;
  state.lastAtkRoll = atkRoll;
  state.lastDefRoll = defRoll;
  state.battleExchange++;
  state.lastLossIdx = -1;
  state.lastRetreatSide = attackerRetreats ? 1 : 2;
  state.battleOver = true;
  char a[8], d[8];
  fmtRoll(a, sizeof(a), atkDie, state.battleAtkMod);
  fmtRoll(d, sizeof(d), defDie, state.battleDefMod);

  bool planet = (state.battleKind == BATTLE_PLANET);
  bool pursuerHits = attackerRetreats ? (defRoll > atkRoll) : (atkRoll > defRoll);
  if (pursuerHits && getawayLocked(attackerRetreats ? 1 : 2)) pursuerHits = false;  // 3.1-dev.88 Clean getaway
  bool *fleeing = attackerRetreats ? state.atkAlive : state.defAlive;
  int idx = pursuerHits ? lossIndex(fleeing) : -1;
  if (pursuerHits && coverLossLocked(attackerRetreats)) {
    // 2.0-dev.24/26: a hired ship or the escort covers the retreat and is lost instead.
    const char *who = state.lastLossIdx == ESCORT_LOSS_IDX ? "escort" : "hired gunship";
    if (planet) snprintf(state.resultText, sizeof(state.resultText), "Withdrew under fire (%s vs %s) -- your %s covered you and was lost. %s holds.",
                         a, d, who, PLANETS[state.battleTargetPlanet].name);
    else snprintf(state.resultText, sizeof(state.resultText), "%s withdrew under fire (%s vs %s) -- a %s covered the escape and was lost.",
                  attackerRetreats ? "Attacker" : "Defender", a, d, who);
    return;
  }
  if (idx >= 0) {
    fleeing[idx] = false;
    state.lastLossIdx = idx;
  }

  if (planet) {
    const char *pname = PLANETS[state.battleTargetPlanet].name;
    if (idx < 0) {
      snprintf(state.resultText, sizeof(state.resultText),
               "Withdrew under fire (%s vs %s) -- clean escape. %s holds.", a, d, pname);
    } else if (anyAlive(state.atkAlive)) {
      snprintf(state.resultText, sizeof(state.resultText),
               "Withdrew under fire (%s vs %s) -- your %s was lost.", a, d, shipNameOf(idx));
    } else {
      snprintf(state.resultText, sizeof(state.resultText),
               "FLEET DESTROYED while withdrawing! %s holds.", pname);
      // Same as losing the fight outright: unlocks the real garrison count.
      state.players[state.battleAttackerSlot].hasLostAt[state.battleTargetPlanet] = true;
    }
  } else {
    const char *who = attackerRetreats ? "Attacker" : "Defender";
    if (idx < 0 || anyAlive(fleeing)) {
      // 3.1-dev.99: they ran, you hold the field -- a victory (+VP_PVP_DECISIVE_WIN), the same as destroying the fleet.
      const int winner = attackerRetreats ? state.battleDefenderSlot : state.battleAttackerSlot;
      if (winner >= 0) state.players[winner].vp += VP_PVP_DECISIVE_WIN;
      promiseOnPvpWinLocked(winner);  // 3.1-dev.113 (playtest: a Pyke "win a battle" debt stayed open after the enemy ran)
      snprintf(state.resultText, sizeof(state.resultText), "%s withdrew under fire (%s vs %s) -- %s. Victory: +%d VP.", who, a, d,
               idx < 0 ? "clean escape" : "lost a ship", VP_PVP_DECISIVE_WIN);
    } else {
      snprintf(state.resultText, sizeof(state.resultText), "%s's fleet destroyed while withdrawing!", who);
      // Destroying a whole fleet is a decisive win, however it happens.
      int winner = attackerRetreats ? state.battleDefenderSlot : state.battleAttackerSlot;
      if (winner >= 0) state.players[winner].vp += VP_PVP_DECISIVE_WIN;
      bountyLocked(winner, attackerRetreats ? state.battleAttackerSlot : state.battleDefenderSlot);  // 2.0
      rebelWinLocked(winner, attackerRetreats ? state.battleAttackerSlot : state.battleDefenderSlot, -1);
      fleetDestroyedBigLocked(winner, attackerRetreats ? state.battleAttackerSlot : state.battleDefenderSlot);
    }
  }
}

static void shakeLocked(int slot, int mask) {
  if (slot < 0 || slot >= MAX_PLAYERS || !mask) return;
  if (state.battleActive && getawayLocked(slot == state.battleAttackerSlot ? 1 : 2)) return;  // 3.1-dev.88 Clean getaway
  Player &pl = state.players[slot];
  pl.shakenMask = (uint8_t)(mask & ((1 << NUM_FLEETS) - 1)); pl.shakenRound = (int8_t)(state.round + 1);
  stampLocked(slot, STAMP_SHAKEN, -1);
}
// 3.1-dev.80 BATTLE REPLAY: the counts when the first roll comes, each exchange's rolls, how it ended.
static void battleRecBeginLocked() {
  BattleRec &r = state.battleCur;
  r = BattleRec();
  r.round = (int8_t)state.round; r.kind = (int8_t)state.battleKind; r.atk = (int8_t)state.battleAttackerSlot; r.def = (int8_t)state.battleDefenderSlot;
  r.planet = (int8_t)(state.battleKind == BATTLE_PLANET ? state.battleTargetPlanet : -1);
  r.a0 = (uint8_t)(aliveCount(state.atkAlive) + (state.battleEscortAlive ? 1 : 0) + state.battleAtkMercs + state.battleAtkExtra);
  r.d0 = (uint8_t)(state.battleKind == BATTLE_PLANET ? state.defRemaining + state.battleDefExtra : aliveCount(state.defAlive) + state.battleDefMercs);
  state.battleCurPushed = 0;
}
static void battleRecStepLocked(int exBefore) {
  BattleRec &r = state.battleCur;
  if (state.battleExchange != exBefore && r.n < BATTLE_REC_LOG)
    r.log[r.n++] = (uint8_t)((min(15, max(0, (int)state.lastAtkRoll)) << 4) | min(15, max(0, (int)state.lastDefRoll)));
  if (state.battleOver && !state.battleCurPushed && r.n) {
    r.retreat = state.lastRetreatSide;
    r.result = state.lastRetreatSide ? 3 : (state.battleKind == BATTLE_PLANET ? (state.defRemaining <= 0 ? 1 : 2) : (anyAlive(state.atkAlive) ? 1 : 2));
    state.battleRecs[state.battleRecHead % BATTLE_RECS] = r;
    state.battleRecHead = (uint8_t)((state.battleRecHead + 1) % BATTLE_RECS);
    state.battleCurPushed = 1;
  }
}
void gameStateBattleAction(int slot, int action) {
  if (g_remote) { remoteCall(RF_BATTLE_ACTION, {slot, action}); return; }
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return;
  if (!state.battleActive || state.battleOver) {
    xSemaphoreGive(stateMutex);
    return;
  }
  if (state.battleExchange == 0) battleRecBeginLocked();  // 3.1-dev.80 (again on each call until the first roll: a defender may bring a fleet)
  const int exBefore = state.battleExchange;

  if (state.battleKind == BATTLE_PLANET) {
    if (state.battleAttackerSlot != slot) { xSemaphoreGive(stateMutex); return; }
    if (action == 2) {
      resolveRetreatLocked(true);  // the garrison gets a parting shot
    } else {
      resolvePlanetExchangeLocked();
    }
  } else if (state.battleKind == BATTLE_PVP) {
    // Record this player's action; resolve once BOTH are in.
    if (slot == state.battleAttackerSlot) state.atkAction = action;
    else if (slot == state.battleDefenderSlot) state.defAction = action;
    else { xSemaphoreGive(stateMutex); return; }

    if (state.atkAction != 0 && state.defAction != 0) {
      // 3.1-dev.76 SHAKEN (user: "if you run away from a battle with someone, your ship needs to move"; "shaken is a great idea"):
      // a fleet that withdraws pulls back to your nearest world or a space station and can't attack next round.
      if (state.atkAction == 2) shakeLocked(state.battleAttackerSlot, state.battleAttackerFleets ? state.battleAttackerFleets : (1 << state.battleAttackerFleet));
      if (state.defAction == 2 && state.battleDefenderSlot >= 0) shakeLocked(state.battleDefenderSlot, state.battleDefenderFleets ? state.battleDefenderFleets : (state.battleDefenderFleet >= 0 ? 1 << state.battleDefenderFleet : 0));
      if (state.atkAction == 2 && state.defAction == 2) {
        // Mutual disengagement -- nobody is pursuing, so no parting shot.
        state.battleOver = true;
        snprintf(state.resultText, sizeof(state.resultText), "Both fleets withdrew.");
      } else if (state.atkAction == 2 || state.defAction == 2) {
        resolveRetreatLocked(state.atkAction == 2);  // the side that fought gets a parting shot
      } else {
        resolvePvpExchangeLocked();
      }
    }
  }
  battleRecStepLocked(exBefore);  // 3.1-dev.80

  xSemaphoreGive(stateMutex);
}

void gameStateEndTakeoverAfterBattle(int slot) {
  if (g_remote) { remoteCall(RF_END_TAKEOVER_AFTER_BATTLE, {slot}); return; }
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return;
  if (state.battleKind == BATTLE_PLANET && state.battleAttackerSlot == slot) {
    // If the attacker captured the planet, its garrison resets to base for the
    // new owner (they can re-invest during Invest). If defense held, restore
    // the garrison to what it was (survivors already decremented; simplest to
    // leave planetDefense as the authoritative "current garrison").
    if (state.battleOver && state.planetOwner[state.battleTargetPlanet] == slot) {
      state.planetDefense[state.battleTargetPlanet] = planetStartGarrison(state.battleTargetPlanet);
    } else {
      const int real = state.defRemaining - state.battleMilitiaAlive - state.battlePatrolAlive;  // 2.0-dev.24: the militia goes home (3.1-dev.76: the patrol stays on)
      state.planetDefense[state.battleTargetPlanet] = real > 0 ? real : planetStartGarrison(state.battleTargetPlanet);
      state.patrolShips[state.battleTargetPlanet] = state.battlePatrolAlive;
    }
    // Persist ship losses back to the attacker's real fleets (v0.28.0: all of them).
    for (int i = 0; i < BATTLE_SHIPS; i++) {
      const int f = i / FLEET_SHIPS;
      if (state.battleAttackerFleets & (1 << f)) state.players[slot].fleetShipAlive[f][i % FLEET_SHIPS] = state.atkAlive[i];
    }
    state.battleAttackerFleets = 0;
    state.battleKind = BATTLE_NONE;
    state.battleActive = false;
    state.battleAttackerSlot = -1;
    state.battleAttackerFleet = -1;
    state.battleTargetPlanet = -1;
    // Clear the takeover session too.
    clearTakeoverLocked(slot);
  }
  xSemaphoreGive(stateMutex);
}

// ---- Notifications ----
void gameStateSetAlert(int slot, const char *msg) {
  if (slot < 0 || slot >= MAX_PLAYERS) return;
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return;
  snprintf(state.alert[slot], sizeof(state.alert[slot]), "%s", msg ? msg : "");
  xSemaphoreGive(stateMutex);
}

void gameStateClearAlert(int slot) {
  if (slot < 0 || slot >= MAX_PLAYERS) return;
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return;
  state.alert[slot][0] = '\0';
  xSemaphoreGive(stateMutex);
}

// ---- PvP ----
int gameStateFleetStrength(int slot, int fleetIdx) {
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return 0;
  int n = fleetStrengthLocked(slot, fleetIdx);
  xSemaphoreGive(stateMutex);
  return n;
}

const char *gameStateStartPvpBattle(int attackerSlot, int attackerFleetMask, int defenderSlot, int defenderFleet) {
  if (g_remote) {
    static char refusal[64];
    RemoteResult r = remoteCall(RF_START_PVP, {attackerSlot, attackerFleetMask, defenderSlot, defenderFleet});
    if (r.value == 0) return nullptr;
    snprintf(refusal, sizeof(refusal), "%s", r.text);
    return refusal;
  }
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return "Game state unavailable.";
  const bool leaveMercs = (attackerFleetMask & FIGHT_LEAVE_MERCS) != 0;  // 2.0-dev.28
  attackerFleetMask &= (1 << NUM_FLEETS) - 1;

  // Guards, each with a player-facing reason (v0.20.2 -- this used to fail
  // silently, and the AMOLED then showed an empty battle screen).
  const char *refusal = nullptr;
  if (state.stage != STAGE_PLAYING || state.phase != PHASE_RESOLUTION)
    refusal = "Battles only happen in Resolution.";
  else if (snapshotFhFinal(state))
    refusal = "The Final Hour decides every fight.";
  else if (state.battleActive)
    refusal = "A battle is already in progress.";
  else if (attackerSlot < 0 || attackerSlot >= MAX_PLAYERS || defenderSlot < 0 || defenderSlot >= MAX_PLAYERS
           || attackerSlot == defenderSlot || !state.players[attackerSlot].active
           || !state.players[defenderSlot].active)
    refusal = "That player can't be attacked.";
  else if (defenderFleet < 0 || defenderFleet >= NUM_FLEETS)
    refusal = "No such fleet.";
  else if (const char *why = attackFleetsRefusalLocked(attackerSlot, attackerFleetMask))
    refusal = why;
  else if (fleetStrengthLocked(defenderSlot, defenderFleet) <= 0)
    refusal = "That fleet has no ships to attack.";
  else if (state.players[defenderSlot].harborRound == state.round)  // 2.0: GOOD_HARBOR
    refusal = "Their fleets are in safe harbor this round.";

  if (!refusal) {
    bcCheckLocked(attackerSlot, BCH_PVP, -1, defenderSlot);  // 3.1-dev.98 the Back Channel
    for (int f = 0; f < NUM_FLEETS; f++)  // attacking spends every fleet sent in (defending doesn't)
      if (attackerFleetMask & (1 << f)) state.players[attackerSlot].fleetSpent[f] = true;
    state.lastRetreatSide = 0;
    state.battleEscortFaction = -1; state.battleEscortAlive = 0;    // 2.0-dev.24: planet battles only
    state.battleMilitiaFaction = -1; state.battleMilitiaAlive = 0;
    state.battleKind = BATTLE_PVP;
    state.battleActive = true;
    state.battleAttackerSlot = attackerSlot;
    state.battleAttackerFleet = lowestFleet(attackerFleetMask);
    state.battleAttackerFleets = (uint8_t)attackerFleetMask;
    state.battleDefenderSlot = defenderSlot;
    state.battleDefenderFleet = defenderFleet;
    state.battleDefenderFleets = (uint8_t)(1 << defenderFleet);
    // Copy each fleet's current ship-alive state into the working arrays.
    for (int i = 0; i < BATTLE_SHIPS; i++) {
      const int f = i / FLEET_SHIPS, s = i % FLEET_SHIPS;
      state.atkAlive[i] = (attackerFleetMask & (1 << f)) && state.players[attackerSlot].fleetShipAlive[f][s];
      state.defAlive[i] = f == defenderFleet && state.players[defenderSlot].fleetShipAlive[f][s];
    }
    state.atkAction = 0;
    state.defAction = 0;
    state.lastAtkRoll = 0;
    state.lastDefRoll = 0;
    state.battleExchange = 0;
    state.lastLossIdx = -1;
    state.battleAtkMod = 0;  // PvP: plain d6 both sides (reputations are per player)
    state.battleDefMod = 0;
    state.battleAtkMercs = leaveMercs ? 0 : playerBattleMercs(attackerSlot);  // 2.0-dev.26: hired ships
    state.battleDefMercs = 0;  // dev.28: the defender calls theirs in (gameStateCallMercs)
    state.battleGetaway = 0;
    if (holdTakeLocked(attackerSlot, LOT_BLADE)) { state.battleAtkMercs++; stampLocked(attackerSlot, STAMP_BLADE, -1); }  // 3.1-dev.88 Hired blade
    if (holdTakeLocked(defenderSlot, LOT_BLADE)) { state.battleDefMercs++; stampLocked(defenderSlot, STAMP_BLADE, -1); }
    classBattleExtrasLocked(attackerSlot, -1, defenderSlot);  // 3.1-dev.7 (PvP: no world, so no Spread thin)
    joinersLocked(attackerSlot, defenderSlot, -1);  // 3.1-dev.102
    state.atkRerolls = (uint8_t)playerBattleRerolls(attackerSlot);  // 2.0: TR_ANCIENT / GOOD_BLESSING
    state.defRerolls = (uint8_t)playerBattleRerolls(defenderSlot);
    state.battleOver = false;
    state.resultText[0] = '\0';
    // Alert the defender.
    snprintf(state.alert[defenderSlot], sizeof(state.alert[defenderSlot]),
             "UNDER ATTACK by %s!", state.players[attackerSlot].name);
  }

  xSemaphoreGive(stateMutex);
  return refusal;
}

static int popcount3(int m) { return (m & 1) + ((m >> 1) & 1) + ((m >> 2) & 1); }

int snapshotReinforceMask(const GameState &gs, int slot) {
  if (!gs.battleActive || gs.battleKind != BATTLE_PVP || gs.battleOver || gs.battleExchange != 0) return 0;
  if (slot < 0 || slot != gs.battleDefenderSlot) return 0;
  if (popcount3(gs.battleAttackerFleets) < 2 || popcount3(gs.battleDefenderFleets) >= popcount3(gs.battleAttackerFleets)) return 0;
  int mask = 0;
  for (int f = 0; f < NUM_FLEETS; f++) {
    if (gs.battleDefenderFleets & (1 << f)) continue;
    for (int s = 0; s < FLEET_SHIPS; s++) if (gs.players[slot].fleetShipAlive[f][s]) { mask |= 1 << f; break; }
  }
  return mask;
}

const char *gameStateReinforce(int slot, int fleetIdx) {
  if (g_remote) {
    static char refusal[64];
    RemoteResult r = remoteCall(RF_REINFORCE, {slot, fleetIdx});
    if (r.value == 0) return nullptr;
    snprintf(refusal, sizeof(refusal), "%s", r.text);
    return refusal;
  }
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return "Game state unavailable.";
  const char *refusal = nullptr;
  if (fleetIdx < 0 || fleetIdx >= NUM_FLEETS || !(snapshotReinforceMask(state, slot) & (1 << fleetIdx))) {
    refusal = "That fleet can't join this fight.";
  } else {
    state.battleDefenderFleets |= (uint8_t)(1 << fleetIdx);
    for (int s = 0; s < FLEET_SHIPS; s++) state.defAlive[fleetIdx * FLEET_SHIPS + s] = state.players[slot].fleetShipAlive[fleetIdx][s];
  }
  xSemaphoreGive(stateMutex);
  return refusal;
}

bool snapshotCanCallMercs(const GameState &gs, int slot) {
  return gs.battleActive && gs.battleKind == BATTLE_PVP && !gs.battleOver && gs.battleExchange == 0
      && slot >= 0 && slot == gs.battleDefenderSlot && gs.battleDefMercs == 0 && gs.players[slot].nextBattleMercs > 0;
}

const char *gameStateCallMercs(int slot) {
  if (g_remote) {
    static char refusal[64];
    RemoteResult r = remoteCall(RF_CALL_MERCS, {slot});
    if (r.value == 0) return nullptr;
    snprintf(refusal, sizeof(refusal), "%s", r.text);
    return refusal;
  }
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return "Game state unavailable.";
  const char *refusal = nullptr;
  if (!snapshotCanCallMercs(state, slot)) refusal = "Your hired ships can't join now.";
  else state.battleDefMercs = playerBattleMercs(slot);
  xSemaphoreGive(stateMutex);
  return refusal;
}

void gameStateEndPvpBattle() {
  if (g_remote) { remoteCall(RF_END_PVP, {}); return; }
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return;
  if (state.battleKind == BATTLE_PVP) {
    // Write survivors back to both players' fleets.
    const int a = state.battleAttackerSlot, d = state.battleDefenderSlot;
    for (int i = 0; i < BATTLE_SHIPS; i++) {  // every fleet on each side (v0.28.0)
      const int f = i / FLEET_SHIPS, s = i % FLEET_SHIPS;
      if (a >= 0 && (state.battleAttackerFleets & (1 << f))) state.players[a].fleetShipAlive[f][s] = state.atkAlive[i];
      if (d >= 0 && (state.battleDefenderFleets & (1 << f))) state.players[d].fleetShipAlive[f][s] = state.defAlive[i];
    }
    state.battleAttackerFleets = state.battleDefenderFleets = 0;
    // Clear the defender's alert.
    if (d >= 0) state.alert[d][0] = '\0';

    state.battleKind = BATTLE_NONE;
    state.battleActive = false;
    state.battleAttackerSlot = -1;
    state.battleAttackerFleet = -1;
    state.battleDefenderSlot = -1;
    state.battleDefenderFleet = -1;
  }
  xSemaphoreGive(stateMutex);
}

// ---------------------------------------------------------------------------
// Victory Points
// ---------------------------------------------------------------------------
int gameStatePlayerVP(int slot) {
  if (slot < 0 || slot >= MAX_PLAYERS) return 0;
  int vp = 0;
  if (stateMutex && xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
    vp = state.players[slot].vp;
    xSemaphoreGive(stateMutex);
  }
  return vp;
}

int gameStateWinnerSlot() {
  int winner = -1;
  if (stateMutex && xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
    winner = snapshotWinnerSlot(state);
    xSemaphoreGive(stateMutex);
  }
  return winner;
}

int snapshotWinnerSlot(const GameState &gs) {
  if (gs.stage != STAGE_GAME_OVER) return -1;
  int winner = -1;

  // Tiebreak chain: highest VP -> most planets controlled -> most Credits.
  // If everything is still tied, it's a genuine draw (-2).
  int bestVP = -1;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (gs.players[i].active && gs.players[i].vp > bestVP) bestVP = gs.players[i].vp;
  }
  int roundA[MAX_PLAYERS];
  int countA = 0;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (gs.players[i].active && gs.players[i].vp == bestVP) roundA[countA++] = i;
  }

  if (countA == 1) {
    winner = roundA[0];
  } else {
    int bestPlanets = -1;
    for (int c = 0; c < countA; c++) {
      int count = 0;
      for (int p = 0; p < NUM_PLANETS; p++) if (gs.planetOwner[p] == roundA[c]) count++;
      if (count > bestPlanets) bestPlanets = count;
    }
    int roundB[MAX_PLAYERS];
    int countB = 0;
    for (int c = 0; c < countA; c++) {
      int count = 0;
      for (int p = 0; p < NUM_PLANETS; p++) if (gs.planetOwner[p] == roundA[c]) count++;
      if (count == bestPlanets) roundB[countB++] = roundA[c];
    }

    if (countB == 1) {
      winner = roundB[0];
    } else {
      int bestCredits = -1;
      for (int c = 0; c < countB; c++) {
        if (gs.players[roundB[c]].credits > bestCredits) bestCredits = gs.players[roundB[c]].credits;
      }
      int roundC[MAX_PLAYERS];
      int countC = 0;
      for (int c = 0; c < countB; c++) {
        if (gs.players[roundB[c]].credits == bestCredits) roundC[countC++] = roundB[c];
      }
      winner = (countC == 1) ? roundC[0] : -2;  // -2 = genuine draw
    }
  }

  return winner;
}

bool gameStateChooseCommander(int slot, int commanderId) {
  if (g_remote) return remoteCall(RF_CHOOSE_COMMANDER, {slot, commanderId}).value == 1;
  if (slot < 0 || slot >= MAX_PLAYERS || commanderId < 0 || commanderId >= NUM_COMMANDERS) return false;
  bool ok = false;
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return false;
  Player &pl = state.players[slot];
  if (pl.active && state.stage == STAGE_LOBBY && pl.chosenFaction >= 0
      && commanderId / COMMANDERS_PER_FACTION == pl.chosenFaction) {
    pl.commander = commanderId;
    // 3.1-dev.6: starting standing again (a re-pick may undo a defector's), then a
    // defector keeps a little extra with the faction they left.
    const int f = pl.chosenFaction;
    for (int k = 0; k < NUM_FACTIONS; k++) pl.reputation[k] = 0;
    pl.reputation[f] = REP_START_OWN;
    pl.reputation[rivalFactionIndex(f)] = REP_START_RIVAL;
    const int from = CHARACTER_DEFECTOR_FROM[commanderId];
    if (from >= 0 && from < NUM_FACTIONS) {  // 3.1-dev.49 (user): the faction they LEFT knows them; a related third gives a small head start
      static const int8_t DEFECTOR_THIRD[4] = { 3, 2, 0, 1 };  // left the Rebels -> the Dawn, the Empire -> the Pykes, the Pykes -> the Rebels, the Dawn -> the Empire
      pl.reputation[from] = REP_START_DEFECTOR_OLD;
      pl.reputation[DEFECTOR_THIRD[from]] = REP_START_DEFECTOR_THIRD;
    }
    ok = true;
  }
  xSemaphoreGive(stateMutex);
  return ok;
}

// Table console: run a call forwarded by a command console (console_link.cpp).
// g_remote is never set here, so these run for real.
RemoteResult gameStateDispatch(uint8_t fn, const int32_t *a, const char *str) {
  RemoteResult r;
  memset(&r, 0, sizeof(r));
  switch (fn) {
    case RF_JOIN_SLOT:          r.value = gameStateJoinSlot(str ? str : "", "", a[0]); break;
    case RF_CHOOSE_FACTION:     r.value = gameStateChooseFaction(a[0], a[1]) ? 1 : 0; break;
    case RF_CHOOSE_COMMANDER:   r.value = gameStateChooseCommander(a[0], a[1]) ? 1 : 0; break;
    case RF_SET_ROUND_LIMIT:    gameStateSetRoundLimit(a[0]); break;
    case RF_SET_MARKETS:        gameStateSetMarkets(a[0] != 0); break;  // 2.0-dev.32
    case RF_SET_EVENTS:         gameStateSetEvents(a[0] != 0); break;   // 2.0-dev.33
    case RF_START_GAME:         gameStateStartGame(); break;
    case RF_LOCK_IN:            gameStateLockIn(a[0]); break;
    case RF_REVEAL:             r.value = gameStateRevealCharacteristic(a[0], a[1], a[2]) ? 1 : 0; break;
    case RF_INVEST_DEFENSE:     r.value = gameStateInvestDefense(a[0], a[1]) ? 1 : 0; break;
    case RF_INVEST_FLEET:       r.value = gameStateInvestFleet(a[0], a[1]) ? 1 : 0; break;
    case RF_BEGIN_TAKEOVER:     gameStateBeginTakeover(a[0], a[1]); break;
    case RF_CANCEL_TAKEOVER:    gameStateCancelTakeover(a[0]); break;
    case RF_COMMIT_TAKEOVER:    gameStateCommitTakeover(a[0], a[1]); break;
    case RF_COMMIT_FIGHT:       gameStateCommitFight(a[0], a[1]); break;
    case RF_CHOOSE_OFFER:       gameStateNegotiationAnswer(a[0], a[1]); break;
    case RF_NEG_CLOSE:          gameStateNegotiationClose(a[0]); break;
    case RF_NEG_FIGHT:          gameStateNegotiationFight(a[0]); break;
    case RF_PAY_PRICE:          r.value = gameStatePayPrice(a[0], a[1]); break;
    case RF_BATTLE_ACTION:      gameStateBattleAction(a[0], a[1]); break;
    case RF_END_TAKEOVER_AFTER_BATTLE: gameStateEndTakeoverAfterBattle(a[0]); break;
    case RF_START_PVP: {
      const char *e = gameStateStartPvpBattle(a[0], a[1], a[2], a[3]);
      r.value = e ? 1 : 0;
      if (e) snprintf(r.text, sizeof(r.text), "%s", e);
      break;
    }
    case RF_END_PVP:            gameStateEndPvpBattle(); break;
    case RF_CLEAR_TRANSMISSION: gameStateClearTransmission(a[0]); break;
    case RF_RESOLVE_OBLIGATION: gameStateResolveObligation(a[0], a[1] != 0); break;
    case RF_NEW_GAME:           gameStateNewGame(); break;
    case RF_END_GAME:           gameStateEndGame(); break;
    case RF_MOVE:               r.value = 0; break;  // 3.1-dev.13: the moves are phone-only for now
    case RF_DECLARE_ERA:        r.value = gameStateDeclareEra(a[0], a[1]) ? 1 : 0; break;  // 3.1-dev.12
    case RF_SET_LIE:            r.value = gameStateSetLie(a[0], str) ? 1 : 0; break;
    case RF_ANSWER_JOB:         r.value = gameStateAnswerJob(a[0], a[1] != 0) ? 1 : 0; break;  // 3.1-dev.10
    case RF_STALL_BUY:          r.value = gameStateStallBuy(a[0], a[1], a[2], a[3]) ? 1 : 0; break;  // 3.1-dev.9
    case RF_MARKET_BUY:         r.value = gameStateMarketBuy(a[0], a[1]) ? 1 : 0; break;     // 2.0
    case RF_SEND_DIPLOMAT:      r.value = gameStateSendDiplomat(a[0], a[1]) ? 1 : 0; break;  // 2.0
    case RF_DEAL_SEND:          r.value = gameStateDealSend(a[0], a[1], a[2], a[3], a[4] & 0xFF, (int8_t)((a[4] >> 8) & 0xFF), (a[4] >> 16) & 0xFF, a[5], nullptr); break;
    case RF_DEAL_ANSWER:        r.value = gameStateDealAnswer(a[0], a[1], a[2] != 0) ? 1 : 0; break;
    case RF_ASHEN_STRIKE:       r.value = gameStateAshenStrike(a[0], a[1]); break;
    case RF_STEP_AWAY:          gameStateStepAway(a[0], a[1] != 0); break;  // 2.0-dev.13
    case RF_WELCOME_ACK:        gameStateWelcomeAck(a[0]); break;
    case RF_CALL_MERCS: {  // 2.0-dev.28
      const char *e = gameStateCallMercs(a[0]);
      r.value = e ? 1 : 0;
      if (e) snprintf(r.text, sizeof(r.text), "%s", e);
      break;
    }
    case RF_REINFORCE: {
      const char *e = gameStateReinforce(a[0], a[1]);
      r.value = e ? 1 : 0;
      if (e) snprintf(r.text, sizeof(r.text), "%s", e);
      break;
    }
    default: r.value = -1; break;
  }
  return r;
}

// Command console: replace the local mirror with the table's state.
void gameStateApplySnapshot(const GameState &gs) {
  if (!stateMutex || xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return;
  state = gs;
  xSemaphoreGive(stateMutex);
}
