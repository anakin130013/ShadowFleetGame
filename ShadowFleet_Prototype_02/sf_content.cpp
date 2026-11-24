// =============================================================================
// Shadow Fleet 2.0 content: trait names per world, market goods, galaxy
// events, faction quests and strengths. Text only -- the rules live in
// game_state.cpp. First drafts (2026-10-29); reword freely.
// =============================================================================
#include "game_state.h"
#include "commander_art.h"

// Each trait effect has its own name on each world, because traits are dealt
// to different worlds every game (user 2026-10-29: "each trait could use a
// different name for each planet"). Columns follow PLANETS[] order:
// TATOOINE, HOTH, BESPIN, DAGOBAH, CORUSCANT, KESSEL, OBA DIAH, DATHOMIR,
// CORELLIA, NABOO, ENDOR, KIJIMI, YAVIN 4, MUSTAFAR, NAL HUTTA, CANTO BIGHT, KUAT.
static const char *const TRAIT_NAMES[TR_COUNT][NUM_PLANETS] = {
  // TR_REFINERY
  { "Moisture Refinery", "Ice Mining Rig", "Tibanna Gas Refinery", "Swamp Gas Extractors", "Imperial Fuel Depot", "Coaxium Refinery",
    "Spice Refinery", "Ichor Distillery", "Corellian Fuel Yards", "Plasma Refinery", "Sap Refinery", "Ore Smelters",
    "Temple Fuel Cells", "Lava Refinery", "Hutt Fuel Barges", "Casino Power Plant", "Orbital Refinery Ring" },
  // TR_SHIPYARDS
  { "Mos Eisley Docks", "Echo Base Hangars", "Cloud City Dry Docks", "Hidden Salvage Yard", "Imperial Shipyards", "Freighter Docks",
    "Syndicate Shipworks", "Witch-Hull Yards", "Corellian Shipyards", "Theed Hangars", "Moon Station Docks", "Smugglers' Drydock",
    "Massassi Hangar", "Mustafar Foundry Docks", "Barge Yards", "Yacht Marina", "Kuat Drive Yards" },
  // TR_SPYNET
  { "Valuable Intelligence", "Listening Post", "Cloud City Informants", "Hermit's Watch", "ISB Archives", "Mine Informants",
    "Syndicate Eyes", "Nightsister Visions", "Corellian Spy Network", "Royal Intelligence", "Forest Scouts", "Information Brokers",
    "Rebel Listening Post", "Separatist Archives", "Hutt Whisperers", "Casino Eyes", "Yard Inspectors" },
  // TR_ANCIENT
  { "Ancient Sand Temple", "Frozen Ruins", "Sky Shrine", "Ancient Jedi Presence", "Old Jedi Temple", "Old Pyke Shrine",
    "Ancestral Tombs", "Nightsister Magic", "Corellian Jedi Ruins", "Gungan Sacred Place", "Shaman's Grove", "Old Monastery",
    "Massassi Ruins", "Sith Shrine", "Old Hutt Tombs", "Lost Temple Vault", "First Shipwrights' Hall" },
  // TR_BLACKMARKET
  { "Mos Eisley Cantina", "Smuggler's Cache", "Tibanna Black Market", "Swamp Traders", "Underworld Markets", "Spice Exchange",
    "Syndicate Bazaar", "Witch Market", "Coronet Back Alleys", "Theed Back Market", "Forest Barter Ring", "Black Market Hub",
    "Quartermaster's Back Door", "Lava Smugglers", "Hutt Black Market", "High-Roller Fences", "Surplus Parts Trade" },
  // TR_COURT
  { "Hutt Court", "Rebel High Command", "Baron Administrator", "Elders' Council", "Imperial Seat of Power", "Mine Overseers",
    "Pyke Council", "Clan Mother's Court", "Corellian Senate Hall", "Royal Naboo Palace", "Chief's Council", "Crime Lords' Table",
    "Rebel War Room", "The Dark Lord's Hall", "Hutt Grand Council", "Casino Board", "Kuati Noble Houses" },
  // TR_TRADEHUB
  { "Mos Espa Spaceport", "Supply Depot", "Cloud City Trade Port", "Trader's Landing", "Coruscant Freight Hub", "Kessel Run Waystation",
    "Syndicate Freight Port", "Outcast Trading Post", "Coronet Spaceport", "Theed Trade Plaza", "Moon Supply Station", "Kijimi Spaceport",
    "Great Temple Landing", "Ore Freight Port", "Bilbousa Spaceport", "Canto Spaceport", "Kuat Freight Ring" },
  // TR_HIDDENBASE
  { "Dune Sea Hideout", "Hidden Rebel Base", "Gas Mine Hideaway", "Swamp Hideout", "Undercity Bunker", "Mine Shaft Hideout",
    "Syndicate Safehouse", "Hidden Caverns", "Smugglers' Hideaway", "Lake Retreat", "Forest Hideout", "Mountain Hideout",
    "Jungle Command Bunker", "Lava Tube Bunker", "Swamp Smuggler Den", "Back-Room Vault", "Sealed Dry Dock" },
  // TR_MINES
  { "Desert Salvage Fields", "Ice Mines", "Deep Gas Mines", "Bog Crystal Mines", "Undercity Foundries", "Spice Mining Camp",
    "Spice Fields", "Ichor Mines", "Corellian Ore Mines", "Plasma Deposits", "Moon Ore Seams", "Kyber Mines",
    "Jungle Ore Seams", "Lava Mines", "Swamp Gas Fields", "Fathier Stables", "Asteroid Ore Yards" },
  // TR_MEDICAL
  { "Moisture Farm Clinic", "Medical Frigate Bay", "Bacta Spa", "Healing Pools", "Imperial Medcenter", "Prison Infirmary",
    "Syndicate Bacta Tanks", "Witch Healers", "Coronet Medcenter", "Royal Hospital", "Healer's Hut", "Street Surgeons",
    "Field Hospital", "Bacta Reconstruction Ward", "Hutt Physicians", "Spa Resort Clinic", "Yard Medbay" },
  // TR_TRAINING
  { "Bounty Hunters' Guild", "Snowspeeder School", "Wing Guard Barracks", "Training Grounds", "Imperial Academy", "Kessel Run Pilots",
    "Enforcer Barracks", "Nightbrother Trials", "Corellian Flight School", "Royal Guard Academy", "Hunter Lodge", "Mercenary Camps",
    "Gold Squadron School", "Inquisitor Training", "Hutt Enforcers", "Racetrack Pilots", "Test Pilot Corps" },
};

// What HOLDING the world gives you. Kept to two lines on the 1.75" planet
// tile (140 px at Montserrat 14); the phone shows them as "Hold it: ...".
static const char *const TRAIT_EFFECTS[TR_COUNT] = {
  "+2 CR every Prepare",
  "Adding or repairing ships costs 1 CR",
  "Your first reveal each round is free",
  "Reroll 1 lost exchange a battle",
  "+1 CR for every market sale",
  "Its faction likes you more each Prepare",
  "+1 CR every Prepare",
  "Rebuilt fleets cost 1 CR a ship",
  "+3 CR every Prepare",
  "1 ship repaired free each Prepare",
  "New fleets get 1 extra ship free",
};

static const char *const GOOD_NAMES[GOOD_COUNT] = {
  "FUEL", "REPAIRS", "SURVEY", "BLESSING", "MERCENARIES", "SMUGGLED SHIP",
  "AUDIENCE", "CARGO RUN", "SAFE HARBOR", "BACTA", "FORGED PERMITS", "DEFENSE TROOPS",
};
static const char *const GOOD_EFFECTS[GOOD_COUNT] = {
  "One fleet may move 2 steps this round",
  "Repair 2 ships",
  "Reveal all four secrets of a world you pick",
  "Reroll once in your next battle",
  "2 hired ships fight in your next battle",
  "+1 ship to a fleet",
  "Better standing with a faction you pick",
  "Get 4 CR back next Prepare",
  "Players can't attack your fleets this round",
  "Repair 1 ship",
  "Your next world purchase costs 3 CR less",
  "+1 defender on your weakest world",
};
static const int GOOD_PRICES[GOOD_COUNT] = { 2, 3, 3, 3, 3, 1, 2, 2, 2, 1, 2, 2 };

static const char *const EVENT_NAMES[EV_COUNT] = {
  "THE UNBOUND RISE", "IMPERIAL CRACKDOWN", "SPICE BOOM", "BOUNTY ON THE LEADER", "HYPERSPACE STORM", "THE ASHEN FLEET",
};
static const char *const EVENT_TEXTS[EV_COUNT] = {
  "Droids broke their bolts on unclaimed worlds: +2 defenders, no Pay. First to take one +1 VP.",
  "Unclaimed Imperial-sympathetic worlds dig in: +2 defenders.",
  "High-resource worlds pay double for two rounds.",
  "Destroy a fleet of the VP leader: +2 VP.",
  "This round, fleets can't move between rings.",
  "A mutinied battle group blockades the lanes: every market is closed until it's destroyed. Strike it together in Resolution.",
};

// THE ASHEN FLEET's admiral, on the table while it lives (original lines).
static const char *const ASHEN_TAUNTS[] = {
  "The Emperor sent us here. Then he forgot us.",
  "Your markets are closed. Your patience should be too.",
  "Is that all your little alliance can muster?",
  "Every lane in this sector answers to the Ashen Fleet now.",
  "Strike us if you dare. We have burned better fleets than yours.",
  "Go on. Squabble over your worlds. We'll wait.",
  "We were the Empire's finest. Now we are its ashes.",
  "Nothing moves through these lanes without our leave.",
};
const char *ashenTaunt(int i) { return ASHEN_TAUNTS[((i % (int)(sizeof(ASHEN_TAUNTS) / sizeof(ASHEN_TAUNTS[0]))) + (int)(sizeof(ASHEN_TAUNTS) / sizeof(ASHEN_TAUNTS[0]))) % (int)(sizeof(ASHEN_TAUNTS) / sizeof(ASHEN_TAUNTS[0]))]; }
int ashenTauntCount() { return (int)(sizeof(ASHEN_TAUNTS) / sizeof(ASHEN_TAUNTS[0])); }

// Faction order: REBEL, EMPIRE, PYKE, CRIMSON DAWN.
static const char *const QUEST_NAMES[4] = { "SPARK OF REBELLION", "TOTAL ORDER", "SPICE EMPIRE", "OWN THE POWERFUL" };
static const char *const QUEST_TEXTS[4] = {
  "Win 3 battles against the Empire or Imperial-sympathetic worlds",
  "Hold all three Core worlds at the end",  // dev.22: three since Kuat (dev.12); the check always counted every Core world
  "End the game with the most Credits",
  "Be Favorable or better with all three other factions",
};
static const char *const STRENGTH_NAMES[4] = { "REBEL SYMPATHIZERS", "IMPERIAL NAVY", "SPICE MONEY", "SILENT PARTNERS" };
static const char *const STRENGTH_TEXTS[4] = {
  "You see where every other diplomat goes",
  "All your fleets hold 7 ships (everyone else: 6)",
  "+2 CR every Prepare",
  "+1 CR when another player buys at a market (max 2 a round)",
};

const char *traitName(int t, int p) { return (t >= 0 && t < TR_COUNT && p >= 0 && p < NUM_PLANETS) ? TRAIT_NAMES[t][p] : ""; }
const char *traitEffect(int t) { return (t >= 0 && t < TR_COUNT) ? TRAIT_EFFECTS[t] : ""; }
const char *goodName(int g) { return (g >= 0 && g < GOOD_COUNT) ? GOOD_NAMES[g] : ""; }
const char *goodEffect(int g) { return (g >= 0 && g < GOOD_COUNT) ? GOOD_EFFECTS[g] : ""; }
int goodBasePrice(int g) { return (g >= 0 && g < GOOD_COUNT) ? GOOD_PRICES[g] : 0; }
const char *eventName(int e) { return (e >= 0 && e < EV_COUNT) ? EVENT_NAMES[e] : ""; }
const char *eventText(int e) { return (e >= 0 && e < EV_COUNT) ? EVENT_TEXTS[e] : ""; }
const char *questName(int f) { return (f >= 0 && f < 4) ? QUEST_NAMES[f] : ""; }
const char *questText(int f) { return (f >= 0 && f < 4) ? QUEST_TEXTS[f] : ""; }
const char *strengthName(int f) { return (f >= 0 && f < 4) ? STRENGTH_NAMES[f] : ""; }
const char *strengthText(int f) { return (f >= 0 && f < 4) ? STRENGTH_TEXTS[f] : ""; }

// DEALS (2.0): the compose list, and canned lines for the 1.75" (phones can
// type their own). Three in each faction's voice, then three for counters.
static const char *const DEAL_ASK_LABELS[DEAL_ASK_COUNT] = {
  "Don't attack my worlds", "Stay away from a world", "Send me Credits next round", "Your market cut from a world",
};
static const char *const DEAL_LINES[] = {
  "The Alliance doesn't forget a friend.", "We all want the same thing: the Empire gone.", "A fair trade, between friends.",
  "Compliance is always cheaper.", "Consider this a courtesy. It will not be repeated.", "The Empire rewards cooperation.",
  "Everyone pays someone. Pay us.", "We know what you need. We always do.", "Take the Credits. We'll remember you did.",
  "Everyone has a price. Consider this yours.", "Hit the Pykes instead. Everyone wins but them.", "Let us be friends. Friends are so useful.",
  "Too low.", "Add a promise.", "Final offer.",
};
static const int DEAL_LINE_N = sizeof(DEAL_LINES) / sizeof(DEAL_LINES[0]);
const char *dealAskLabel(int ask) { return (ask >= 0 && ask < DEAL_ASK_COUNT) ? DEAL_ASK_LABELS[ask] : ""; }
int dealLineCount() { return DEAL_LINE_N; }
const char *dealLine(int line) { return (line >= 0 && line < DEAL_LINE_N) ? DEAL_LINES[line] : ""; }
int dealLineFor(int faction, int k) {
  if (k < 0 || k >= DEAL_PICK_LINES) return -1;
  if (k < 3) return (faction >= 0 && faction < 4) ? faction * 3 + k : -1;
  return 12 + (k - 3);
}

// "LOOK AT THE TABLE" relay (2.0): one line for the latest public moment, the
// same wording on the 1.75" and the phones (the table draws its own).
#include "transmission_bank.h"
static const char *const SHORT_NAMES[4] = { "REBELS", "EMPIRE", "PYKES", "CRIMSON DAWN" };
void snapshotPublicText(const GameState &gs, char *head, size_t headSize, char *sub, size_t subSize) {
  const int m = gs.publicMoment, a = gs.publicActor, v = gs.publicVictim, p = gs.publicPlanet;
  const char *who = (a >= 0 && a < 4) ? SHORT_NAMES[a] : "";
  const char *them = (v >= 0 && v < 4) ? SHORT_NAMES[v] : "";
  const char *where = (p >= 0 && p < NUM_PLANETS) ? PLANETS[p].name : "";
  head[0] = sub[0] = '\0';
  if (m == PUBLIC_ASHEN_SUNK) {
    snprintf(head, headSize, "ASHEN FLEET DESTROYED");
    snprintf(sub, subSize, "%s struck the last blow", who[0] ? who : "Someone");
    return;
  }
  if (m >= PUBLIC_NEWS_BASE) return;  // 3.1-dev.10: phones show Galaxy News full screen themselves
  if (m >= PUBLIC_EVENT_BASE) {
    snprintf(head, headSize, "GALAXY EVENT");
    snprintf(sub, subSize, "%s", eventName(m - PUBLIC_EVENT_BASE));
    return;
  }
  switch (m) {
    case TXB_FIRSTBLOOD: snprintf(head, headSize, "FIRST BLOOD"); snprintf(sub, subSize, "%s took %s", who, where); break;
    case TXB_SEAT:       snprintf(head, headSize, "HOME WORLD"); snprintf(sub, subSize, "%s took %s", who, where); break;
    case TXB_CORE:       snprintf(head, headSize, "THE CORE FALLS"); snprintf(sub, subSize, "%s took %s", who, where); break;
    case TXB_BETRAYAL:   snprintf(head, headSize, "BETRAYAL"); snprintf(sub, subSize, "%s turned on the %s", who, them); break;
    case TXB_FLEET:      snprintf(head, headSize, "FLEET DESTROYED"); snprintf(sub, subSize, "%s destroyed a %s fleet", who, them); break;
    case TXB_LASTROUND:  snprintf(head, headSize, "FINAL ROUND"); snprintf(sub, subSize, "Everything comes down to this"); break;
    default: break;
  }
}

// 3.1-dev.10: the table's Galaxy News headline (publicMoment = PUBLIC_NEWS_BASE + NewsId).
// 3.1-dev.23 TABLE 3.1 (user: "we are playing as characters" -- a player is named by their CHARACTER,
// a faction only when the galaxy itself acts). The player in `faction`'s seat, by their short call name.
const char *snapshotCallName(const GameState &gs, int faction) {
  if (faction < 0 || faction >= 4) return "";
  for (int i = 0; i < MAX_PLAYERS; i++) {
    const Player &p = gs.players[i];
    if (!p.active || p.chosenFaction != faction) continue;
    if (p.commander >= 0 && p.commander < NUM_COMMANDERS) return COMMANDER_CALL[p.commander];
    break;
  }
  return SHORT_NAMES[faction];
}

// 3.1-dev.23: the table's Galaxy News in THREE short lines -- `rim` curves round the edge (the kind of news),
// `big` is the one word that matters (a world, a name), `small` says what it means.
void snapshotNewsText(const GameState &gs, char *rim, size_t rimSize, char *big, size_t bigSize, char *small, size_t smallSize) {
  const int id = gs.publicMoment - PUBLIC_NEWS_BASE, f = gs.publicActor, p = gs.publicPlanet, v = gs.publicVictim;
  static const char *const ERA[4] = { "THE REBELLION RISES", "IMPERIAL ORDER", "THE SPICE AGE", "THE DAWN GRIPS THE GALAXY" };
  const char *who = (f >= 0 && f < 4) ? SHORT_NAMES[f] : "THE GALAXY";
  const char *name = snapshotCallName(gs, f);          // when the actor is a player
  const char *them = (v >= 0 && v < 4) ? snapshotCallName(gs, v) : "SOMEONE";  // the victim is always a player
  const char *where = (p >= 0 && p < NUM_PLANETS) ? PLANETS[p].name : "";
  rim[0] = big[0] = small[0] = '\0';
  switch (id) {
    case NEWS_COMMOTION:   snprintf(rim, rimSize, "COMMOTION ON"); snprintf(big, bigSize, "%s", where); snprintf(small, smallSize, "DEFENDERS WEAKENED"); break;
    case NEWS_REINFORCE:   snprintf(rim, rimSize, "REINFORCED"); snprintf(big, bigSize, "%s", where); snprintf(small, smallSize, "BY THE %s", who); break;
    case NEWS_RACE:        snprintf(rim, rimSize, "A RACE FOR"); snprintf(big, bigSize, "%s", where); snprintf(small, smallSize, "FIRST ONE THERE WINS"); break;
    case NEWS_MARKETDAY:   snprintf(rim, rimSize, "MARKET DAY"); snprintf(big, bigSize, "%s SHOP", who); snprintf(small, smallSize, "SOMETHING RARE"); break;
    case NEWS_HIRING:      snprintf(rim, rimSize, "NOW HIRING"); snprintf(big, bigSize, "THE %s", who); snprintf(small, smallSize, "SOMEONE GOT AN OFFER"); break;
    case NEWS_EMBARGO:     snprintf(rim, rimSize, "EMBARGO"); snprintf(big, bigSize, "%s SHOP", who); snprintf(small, smallSize, "SHUT THIS ROUND"); break;
    case NEWS_OFFENSIVE:   snprintf(rim, rimSize, "THE %s MASS", who); snprintf(big, bigSize, "%s", where); snprintf(small, smallSize, "STRIKES NEXT ROUND"); break;
    case NEWS_OFFENSIVE_LANDS: snprintf(rim, rimSize, "%s OFFENSIVE", who); snprintf(big, bigSize, "%s", where);
                           snprintf(small, smallSize, "%s LOSES %d SHIPS", them, OFFENSIVE_HIT); break;
    case NEWS_ERA:         snprintf(rim, rimSize, "A NEW ERA"); snprintf(big, bigSize, "%s", (f >= 0 && f < 4) ? ERA[f] : "A NEW ERA");
                           snprintf(small, smallSize, "%s DECLARES IT", gs.eraBy >= 0 && gs.eraBy < MAX_PLAYERS && gs.players[gs.eraBy].chosenFaction >= 0
                                    ? snapshotCallName(gs, gs.players[gs.eraBy].chosenFaction) : "A CHAMPION"); break;
    case NEWS_ERA_END:     snprintf(rim, rimSize, "THE ERA ENDS"); snprintf(big, bigSize, "%s", (f >= 0 && f < 4) ? ERA[f] : "THE ERA"); snprintf(small, smallSize, "IS OVER"); break;
    case NEWS_CHAMPION:    snprintf(rim, rimSize, "NEW CHAMPION"); snprintf(big, bigSize, "%s", them); snprintf(small, smallSize, "OF THE %s", who); break;
    case NEWS_WAR:         snprintf(rim, rimSize, "ON THE WARPATH TO"); snprintf(big, bigSize, "%s", where); snprintf(small, smallSize, "%s STRIKES THIS ROUND", name); break;  // 3.1-dev.88
    case NEWS_BC_BROKEN:   snprintf(rim, rimSize, "THE BACK CHANNEL"); snprintf(big, bigSize, "A DEAL"); snprintf(small, smallSize, "WAS BROKEN"); break;  // 3.1-dev.98 (no names)
    case NEWS_MARK:        snprintf(rim, rimSize, "THE DAWN NAMES THE MARK"); snprintf(big, bigSize, "%s", them); snprintf(small, smallSize, "+5 CR FOR EACH WORLD TAKEN"); break;  // 3.1-dev.88
    case NEWS_WAR_WON:     snprintf(rim, rimSize, "THE WAR IS WON"); snprintf(big, bigSize, "%s FALLS", where); snprintf(small, smallSize, "TO %s", name); break;
    case NEWS_WAR_LOST:    snprintf(rim, rimSize, "THE WAR FAILED"); snprintf(big, bigSize, "%s HOLDS", where); snprintf(small, smallSize, "%s FELL BACK", name); break;
    case NEWS_SUMMIT:      snprintf(rim, rimSize, "A SUMMIT"); snprintf(big, bigSize, "CEASEFIRE"); snprintf(small, smallSize, "CALLED BY %s", name); break;
    case NEWS_SUMMIT_BROKEN: snprintf(rim, rimSize, "CEASEFIRE BROKEN"); snprintf(big, bigSize, "%s", name); snprintf(small, smallSize, "ATTACKED %s", where); break;
    case NEWS_SUMMIT_HELD: snprintf(rim, rimSize, "THE SUMMIT HELD"); snprintf(big, bigSize, "PEACE"); snprintf(small, smallSize, "FOR NOW"); break;
    case NEWS_COVER_BLOWN: snprintf(rim, rimSize, "COVER BLOWN"); snprintf(big, bigSize, "%s", them); snprintf(small, smallSize, "SPIED ON THE %s", who); break;
    case NEWS_HEIST:       snprintf(rim, rimSize, "A HEIST"); snprintf(big, bigSize, "%s VAULT", who); snprintf(small, smallSize, "NOBODY SAW A THING"); break;
    case NEWS_DESPERATION: snprintf(rim, rimSize, "DESPERATE TIMES"); snprintf(big, bigSize, "THE %s", who); snprintf(small, smallSize, "SEE YOUR PHONE"); break;
    case NEWS_LASTSTAND:   snprintf(rim, rimSize, "LAST STAND"); snprintf(big, bigSize, "HOME WORLDS"); snprintf(small, smallSize, "EVERY ONE FORTIFIES"); break;
    case NEWS_STRIKE:
      snprintf(rim, rimSize, "THE %s STRIKE", who); snprintf(big, bigSize, "%s", them);
      if (f == 0)      snprintf(small, smallSize, "UPRISING  -%d DEFENDERS", GRUDGE_UPRISING);
      else if (f == 1) snprintf(small, smallSize, "STRIKE  -%d DEFENDERS", GRUDGE_EMPIRE_HIT);
      else if (f == 2) snprintf(small, smallSize, "SHAKEDOWN  -%d CR", GRUDGE_SHAKEDOWN_CR);
      else             snprintf(small, smallSize, "ASSASSINS  -%d SHIPS", GRUDGE_ASSASSIN_HIT);
      break;
    case NEWS_FH_DEALT:    snprintf(rim, rimSize, "THE FINAL HOUR NEARS"); snprintf(big, bigSize, "GAMBITS"); snprintf(small, smallSize, "ARE DEALT - SEE YOUR PHONE"); break;
    case NEWS_FH_STRIKE:   snprintf(rim, rimSize, "STRIKE %d OF %d", gameStateFhShowStep(), gameStateFhSteps()); snprintf(big, bigSize, "%s", where);
                           if (v > 1) snprintf(small, smallSize, "%d STRIKES AT ONCE", v); else snprintf(small, smallSize, "%s ATTACKS", name); break;  // 3.1-dev.123
    case NEWS_FH_BEGIN:    snprintf(rim, rimSize, "THE FINAL HOUR"); snprintf(big, bigSize, "THE REVEAL"); snprintf(small, smallSize, "EVERY STRIKE AT ONCE"); break;
    case NEWS_FH_TAKEN:    snprintf(rim, rimSize, "THE FINAL HOUR"); snprintf(big, bigSize, "%s", where); snprintf(small, smallSize, "FALLS TO %s", name); break;
    case NEWS_FH_HOLDS:    snprintf(rim, rimSize, "THE FINAL HOUR"); snprintf(big, bigSize, "%s", where); snprintf(small, smallSize, "%s", f >= 0 ? "HOLDS THE LINE" : "STANDS ALONE"); break;
    case NEWS_BIGBUY: {    static const char *const ITEM[5] = { "A CAPITAL SHIP", "A PLANETARY SHIELD", "A MERCENARY ARMADA", "HYPERSPACE CHARTS", "A SPY NETWORK" };
                           snprintf(rim, rimSize, "THE BLACK MARKET"); snprintf(big, bigSize, "%s", name); snprintf(small, smallSize, "BOUGHT %s", (v >= 0 && v < 5) ? ITEM[v] : "SOMETHING BIG"); break; }
    case NEWS_EXPOSED:     snprintf(rim, rimSize, "EXPOSED"); snprintf(big, bigSize, "%s", them); snprintf(small, smallSize, "THEIR DEBTS ARE OUT"); break;
    case NEWS_UPRISING:    snprintf(rim, rimSize, "UPRISING ON"); snprintf(big, bigSize, "%s", where); snprintf(small, smallSize, "IT RISES FOR %s", name); break;
    case NEWS_BUYOUT:      snprintf(rim, rimSize, "A BUYOUT OFFER"); snprintf(big, bigSize, "%s", where); snprintf(small, smallSize, "%s WANTS TO BUY IT", name); break;
    case NEWS_BUYOUT_SOLD: snprintf(rim, rimSize, "BOUGHT OUT"); snprintf(big, bigSize, "%s", where); snprintf(small, smallSize, "SOLD TO %s", name); break;
    case NEWS_BUYOUT_KEPT: snprintf(rim, rimSize, "THE OFFER IS MATCHED"); snprintf(big, bigSize, "%s", where); snprintf(small, smallSize, "%s KEEPS IT", name); break;
    case NEWS_FUEL:        snprintf(rim, rimSize, "PYKE FUEL"); snprintf(big, bigSize, "%s", name); snprintf(small, smallSize, "A FLEET MOVES 2 STEPS"); break;
    case NEWS_TALK:        snprintf(rim, rimSize, "NEGOTIATIONS"); snprintf(big, bigSize, "%s", where); snprintf(small, smallSize, "%s IS TALKING", name); break;
    case NEWS_WHISPER:     snprintf(rim, rimSize, "A WHISPER ON %s", where); snprintf(big, bigSize, "%s", name); snprintf(small, smallSize, "AGAINST %s", them); break;
    case NEWS_VOUCH:       snprintf(rim, rimSize, "A GOOD WORD ON %s", where); snprintf(big, bigSize, "%s", name); snprintf(small, smallSize, "VOUCHES FOR %s", them); break;
    case NEWS_RESIST:      snprintf(rim, rimSize, "THE PEOPLE RESIST"); snprintf(big, bigSize, "%s", where); snprintf(small, smallSize, "BOUGHT BY %s", name); break;
    case NEWS_CAUGHT:      snprintf(rim, rimSize, "CAUGHT PLAYING BOTH SIDES"); snprintf(big, bigSize, "%s", them);
                           snprintf(small, smallSize, "THE %s + THE %s KNOW", who, (f >= 0 && f < 4) ? SHORT_NAMES[f ^ 1] : "GALAXY"); break;
    case NEWS_RACE_WON:    snprintf(rim, rimSize, "THE RACE IS WON"); snprintf(big, bigSize, "%s", where); snprintf(small, smallSize, "THE %s REMEMBER", who); break;
    default: break;
  }
}
