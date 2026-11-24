#pragma once
#include <Arduino.h>

// ===========================================================================
// Shadow Fleet - shared game state (multiplayer)
//
// v0.7.0 restructure: the game is no longer hardcoded as attacker vs defender.
// It now supports up to 4 players in numbered slots. Slot 0 is always the HOST
// (the AMOLED board, for now -- becomes a dedicated ESP later). Slots 1-3 are
// phones that join from the lobby. Every player has the same capabilities;
// there is no privileged "attacker" role anymore.
//
// Lifecycle: STAGE_LOBBY (players join / name themselves) -> STAGE_PLAYING.
// ===========================================================================

#define MAX_PLAYERS 4
#define HOST_SLOT 0

#define NUM_PLANETS 17  // 2.0 (2026-11-02): 16-world board + a 3rd Core world (the design notes "DECISIONS 2026-11-01")
#define NUM_CHARACTERISTICS 4  // 0=affiliation, 1=resources, 2=unique, 3=defense
#define MAX_PLANET_DEFENSE 12  // the absolute most (3.1-dev.76: each world's own limit is snapshotDefenseLimit -- Outer 6 / Mid 8 / Core 10)

// 2.0-dev.13 drop out / rejoin
#define AWAY_NONE 0
#define AWAY_QUIET 1             // no word from their phone / console for AWAY_AFTER_MS
#define AWAY_STEPPED 2           // they tapped STEP AWAY (only COME BACK ends it)
#define AWAY_AFTER_MS 30000
#define HERE_EVERY_MS 10000      // a command console tells the table it's still there

#define STAGE_LOBBY 0
#define STAGE_PLAYING 1
#define STAGE_GAME_OVER 2

// Victory Points. Game length is selectable by any player in the lobby
// (6/8/10 rounds); the round-holding VP source is what makes that choice
// meaningful (a longer game gives more rounds for holding bonuses to add
// up). All values here are deliberately easy to retune after a playtest.
#define DEFAULT_ROUND_LIMIT 8
// v0.30.1 (user 2026-10-19: "we shouldn't get VP each round anymore... only upon
// capture. The money is the roundly spoils"): a planet is WORTH VP while you hold
// it -- you gain them when you take it, and whoever takes it from you gets them.
// A planet won with a favor is worth nothing until the favor is kept.
#define VP_PLANET_OUTER 2
#define VP_PLANET_MID 3
#define VP_PLANET_CORE 5
// HOME WORLDS (user 2026-11-01: "if you steal a planet it is like stealing the
// VP"): each faction starts holding its home world. What it's worth depends on
// who holds it -- your own 2, someone else's 6 (they lose their 2), and nothing
// once it's taken back. A home world whose faction nobody plays is neutral.
// 2.0-dev.23 VP SIMPLIFIED (user 2026-11-04, "it feels like there's a million"): VP comes
// from worlds held (2 / 3 / 5, any home world 4), your quest (+5), destroying a
// player's fleet (+2) and the drawn event's own reward -- nothing else. Cut:
// reputation tier VP, +1 per kept promise, Best customer / Merchant prince, and
// the 2 / 6 / 3 home-world split.
#define VP_HOME 4
#define VP_PVP_DECISIVE_WIN 2        // per decisive PvP victory

// Income (v0.23.1; agreed 2026-09-27, user: "it needs to feel strained if you
// don't have any planets"). Paid to every player at the start of each new
// round (not round 1 -- everyone starts with 12). Retune after playtests.
#define INCOME_BASE_EVERY 2      // 3.1-dev.101 (user: "base pay should go up every round"): +1 base income every this many rounds
#define INCOME_BASE 3            // everyone, planets or not (2.0: was 1 -- "1 credit without a planet doesn't seem like enough")
#define INCOME_RESOURCES_HIGH 3  // per planet you hold whose Resources are High
#define INCOME_RESOURCES_LOW 1   // per planet you hold whose Resources are Low

// ===========================================================================
// SHADOW FLEET 2.0 (design agreed with the user 2026-10-28/29; the design notes
// "2.0 PICKS" / "USER ROUND n"; mockups on the "2.0 screens" canvas page).
// Every number here is meant to be retuned after a playtest.
// ===========================================================================
// TRAITS: 12 effects dealt to the 12 worlds each game (planetTrait); each world
// has its own name for each effect (sf_content.cpp). Holding a world gives you
// its trait. Revealing a trait costs REVEAL_COST[2] (1 CR).
enum TraitId : uint8_t {
  TR_REFINERY = 0,  // +2 CR every Prepare
  TR_SHIPYARDS,     // adding or repairing ships costs 1 CR
  TR_SPYNET,        // your first reveal each round is free
  TR_ANCIENT,       // reroll your first lost exchange in every battle
  TR_BLACKMARKET,   // +1 CR whenever anyone buys at any market
  TR_COURT,         // +2 standing with this world's faction every Prepare
  TR_TRADEHUB,      // +1 CR every Prepare
  TR_HIDDENBASE,    // rebuilding a destroyed fleet costs 1 CR a ship
  TR_MINES,         // +3 CR every Prepare
  TR_MEDICAL,       // repair one ship free every Prepare
  TR_TRAINING,      // +1 to your roll in the first exchange of every battle
  TR_COUNT
};
#define TRAIT_INCOME_REFINERY 2
#define TRAIT_INCOME_TRADEHUB 1
#define TRAIT_INCOME_MINES 3
#define TRAIT_COURT_STANDING 2

// MARKETS: every world sells one good (planetGood, dealt per game). Each Prepare
// MARKETS_OPEN of them open, drawn at random, the same for everyone, with
// MARKET_STOCK each. Buy from anywhere during Prepare. Price follows your
// standing with the world's faction; the holder gets a cut.
enum GoodId : uint8_t {
  GOOD_FUEL = 0,     // one fleet may move 2 steps this round (on your honor)
  GOOD_REPAIRS,      // repair 2 ships
  GOOD_SURVEY,       // reveal all four secrets of this world
  GOOD_BLESSING,     // reroll once in your next battle
  GOOD_MERCENARIES,  // 2.0-dev.26: MERC_SHIPS hired ships fight in your next battle (was +1 to your rolls)
  GOOD_SMUGGLED,     // +1 ship to a fleet
  GOOD_AUDIENCE,     // better standing with this world's faction
  GOOD_CARGO,        // get 4 CR back next Prepare
  GOOD_HARBOR,       // players can't attack your fleets this round
  GOOD_BACTA,        // repair 1 ship
  GOOD_PERMITS,      // your next purchase of a world costs 3 CR less
  GOOD_TROOPS,       // +1 defender on the world you hold with the fewest
  GOOD_COUNT
};
#define MARKETS_OPEN 4
#define MARKET_STOCK 2
// 3.1-dev.9 FACTION STALLS (user 2026-11-07: "the factions are selling, not the planets"): each faction runs one
// stall with three goods of its own (FACTION_GOODS), two of them out each round, MARKET_STOCK of each. Shut to you
// at tier 2 and below; tier 4 pays 1 less, tier 5 2 less. No cut for holding a world any more.
#define STALL_ITEMS 3   // 2 out each round; the 3rd only on MARKET DAY (Galaxy News)
#define STALL_TIER_ADJ { 0, 0, 0, -1, -2 }
#define MARKET_CUT 1            // to the world's holder per sale (MARKET_CUT_CORE on a Core world)
#define MARKET_CUT_CORE 2
#define GOOD_AUDIENCE_STANDING 4
#define GOOD_CARGO_RETURN 4
#define GOOD_PERMITS_DISCOUNT 3

// DEALS (mockup "C . Deals"; user: quiet offers across the table, enforced by
// the game): during Prepare a player offers another Credits for one promise.
// The other side can ACCEPT (the Credits move at once and the promise starts),
// DECLINE, or COUNTER (the same form, pre-filled; DEAL_MAX_COUNTERS per deal).
// Open offers lapse when Prepare ends. The payer is always whoever made the
// first offer; the promiser is the player it went to.
enum DealAsk : uint8_t {
  DEAL_NO_ATTACK = 0,  // the promiser can't FIGHT the payer's worlds (DEAL_ROUNDS rounds)
  DEAL_STAY_AWAY,      // the promiser can't fight, negotiate or pay for one world (DEAL_ROUNDS)
  DEAL_PAY_LATER,      // the promiser sends askCr Credits at the next round start
  DEAL_MARKET_CUT,     // the promiser's market cut from one of their worlds goes to the payer (DEAL_ROUNDS)
  DEAL_ASK_COUNT
};
enum DealState : uint8_t { DEAL_FREE = 0, DEAL_OFFERED, DEAL_ACTIVE };
#define DEAL_SLOTS 6           // offers + promises in play at once, game-wide
#define DEAL_ROUNDS 2          // a promise covers the round it's agreed in and the next one
#define DEAL_MAX_COUNTERS 2
#define DEAL_MAX_GIVE 30
#define DEAL_MAX_ASK_CR 20
#define DEAL_MSG_LEN 60        // a typed message (phones; the 1.75" picks a canned line)
struct Deal {
  uint8_t state = DEAL_FREE;
  int8_t payer = -1, promiser = -1;
  int8_t turn = -1;            // DEAL_OFFERED: the slot that has to answer
  uint8_t ask = DEAL_NO_ATTACK;
  int8_t world = -1;           // DEAL_STAY_AWAY / DEAL_MARKET_CUT
  uint8_t give = 0;            // Credits from the payer
  uint8_t askCr = 0;           // DEAL_PAY_LATER
  uint8_t counters = 0;
  int8_t untilRound = -1;      // DEAL_ACTIVE: over at the start of this round
  int8_t line = -1;            // dealLine() index, -1 = msg
  char msg[DEAL_MSG_LEN + 1] = "";
};

// THE DIPLOMAT: one per player; DIPLOMAT_COST to send, back DIPLOMAT_ROUNDS
// rounds later (at that Prepare) with all four secrets of the world.
#define DIPLOMAT_ON 0           // 3.1-dev.17: the diplomat is a 2.0 leftover -- off
#define DIPLOMAT_COST 3
#define DIPLOMAT_ROUNDS 2

// QUESTS (asymmetric, +QUEST_VP at the end) and FACTION STRENGTHS.
#define QUEST_VP 5
#define QUEST_REBEL_WINS 3          // Rebels: win 3 battles vs the Empire / Imperial-sympathetic worlds
#define PYKE_INCOME_BONUS 0         // Pykes: spice money (3.1-dev.15: the old faction strengths are gone -- classes + quirks replace them)
#define CRIMSON_SILENT_CAP 2        // Crimson Dawn: +1 CR per other player's market purchase, max this a round
#define CRIMSON_NOMARKET_INCOME 1   // 2.0-dev.32: with markets off, Silent Partners pays this every Prepare instead

// GALAXY EVENTS: one drawn at the halfway point.
enum EventId : int8_t {
  EV_NONE = -1,
  EV_UNBOUND = 0,   // up to 3 unclaimed worlds: +2 defenders, can't be bought, first to take one +1 VP
  EV_CRACKDOWN,     // unclaimed Empire-sympathetic worlds +2 defenders
  EV_SPICEBOOM,     // High-resource worlds pay double for two rounds
  EV_BOUNTY,        // destroying a fleet of the VP leader: +2 VP
  EV_STORM,         // this round fleets can't move between rings (on your honor)
  EV_ASHEN,         // THE ASHEN FLEET: a joint strike target in the middle of the map; markets closed while it lives
  EV_COUNT
};
// THE ASHEN FLEET (user 2026-10-29): a mutinied battle group with HP. In
// Resolution any player can STRIKE it from ATTACK or the middle of the Galaxy
// map: every living ship in the fleets sent rolls a die, the total is the
// damage, and those fleets are spent for the round. It answers each strike
// with one die: 1-2 misses, 3-5 one hit, 6 two hits (ships lost from the
// strike). While it lives every market is closed. When it sinks, everyone who
// damaged it gets ASHEN_VP and the top damage dealer ASHEN_TOP_VP more.
// Planned later (user: "D could come later"): it rolls +1 below half HP.
#define ASHEN_HP_PER_PLAYER 10
#define ASHEN_VP 2
#define ASHEN_TOP_VP 1
#define PUBLIC_ASHEN_SUNK 40        // publicMoment when it goes down (the table + the relay)
#define EVENT_UNBOUND_WORLDS 3
#define EVENT_GARRISON_BONUS 2
#define EVENT_UNBOUND_VP 1
#define EVENT_BOUNTY_VP 2
#define PUBLIC_EVENT_BASE 20        // publicMoment = PUBLIC_EVENT_BASE + EventId (the table + the relay)

#define PHASE_INVEST 0
#define PHASE_MOVEMENT 1
#define PHASE_RESOLUTION 2

#define PLANET_UNCONTROLLED -1

// Takeover approaches (chosen + committed on the planet takeover screen).
#define TAKEOVER_NONE 0
#define TAKEOVER_FIGHT 1
#define TAKEOVER_NEGOTIATE 2
#define TAKEOVER_PAY 3

// Battle kinds.
#define BATTLE_NONE 0
#define BATTLE_PLANET 1   // attacker fleet vs a planet garrison
#define BATTLE_PVP 2      // attacker fleet vs another player's fleet

// Planet-battle roll modifiers from faction relationships (v0.20.1; replaced
// the old flat +1 attacker bonus). Only apply once the attacker has revealed
// the planet's Affiliation. PvP battles have no modifiers -- reputations are
// per player, so no faction gets an inherent edge against another player.
// 2.0-dev.24 (user 2026-11-04: "one ship is a great idea... you get an escort"):
// the faction help is a SHIP, not a +1 on every roll (a +1 swung a 6-vs-4/5
// fight from 62-75% to 93-96%; a ship is ~+10%). The escort / militia fights
// only this battle, takes its side's FIRST loss, and never joins your fleet.
#define BATTLE_ESCORT_SHIPS 1   // attacker Favorable+ with the planet faction's rival -> its ship escorts you
#define BATTLE_MILITIA_SHIPS 1  // attacker Hostile with the planet's own faction -> local militia joins the garrison

#define NUM_FLEETS 3        // fleets per player
// 2.0-dev.20 EMPIRE STRENGTH (agreed 2026-10-29: "all fleets hold 7"): FLEET_SHIPS is
// the ROOM in every fleet array (the most any fleet can hold); what a player's
// fleets can actually hold is snapshotFleetCapacity() -- 7 for the Empire, 6 else.
#define FLEET_SHIPS 7
#define FLEET_SHIPS_NORMAL 6
#define FLEET_SHIPS_EMPIRE 7
// v0.28.0 two-fleet battles: a battle holds every ship of every fleet it
// involves -- ship (fleet f, slot s) is index f * FLEET_SHIPS + s.
#define BATTLE_SHIPS (NUM_FLEETS * FLEET_SHIPS)
#define ESCORT_LOSS_IDX BATTLE_SHIPS  // lastLossIdx when the escort ship was the one lost
// 2.0-dev.26 MERCENARIES (user 2026-11-04: "switch to 2 hired ships"): a bought
// good = MERC_SHIPS ships that fight your next battle (planet or PvP, attacking or
// defending), take your side's hits FIRST (before an escort), then leave.
#define MERC_SHIPS 2
// 2.0-dev.28 (user: "there needs to be a 'use mercenaries' before a battle"): an attacker
// who leaves them home adds this bit to the fleet mask (gameStateCommitFight /
// gameStateStartPvpBattle); a PvP defender calls them in with gameStateCallMercs().
#define FIGHT_LEAVE_MERCS 0x40
#define MERC_SHIPS_MAX 4
#define MERC_LOSS_IDX (BATTLE_SHIPS + 1)  // lastLossIdx = MERC_LOSS_IDX + k: hired ship k was lost (k = how many are left)
#define FLEET_START_SHIPS 3 // ships a fleet starts with, before any investment
#define FLEET_INVEST_COST 2 // Credits per additional ship

// Factions, for reputation tracking. Index order matches the visual/quote
// tables in the .ino and web_server.cpp -- keep them in sync if this changes.
#define FACTION_REBEL 0
#define FACTION_EMPIRE 1
#define FACTION_PYKE 2
#define FACTION_CRIMSON_DAWN 3
#define NUM_FACTIONS 4

// Reputation tiers (hidden score -10..+10 maps to one of these 5 bands).
// Hostile <=-7, Wary -6..-2, Neutral -1..+1 (start), Favorable +2..+6,
// Allied >=+7.
#define REP_HOSTILE 0
#define REP_WARY 1
#define REP_NEUTRAL 2
#define REP_FAVORABLE 3
#define REP_ALLIED 4

extern const char *REP_TIER_NAMES[5];  // "HOSTILE".."ALLIED" -- tier only, never the raw score

// Garrison strength tiers -- the live per-planet garrison ship count is
// hidden by default (shown only as a tier) unless the player has LOST a
// Fight there before, per gameStateHasLostAt().
#define GARRISON_LOW 0
#define GARRISON_MEDIUM 1
#define GARRISON_HIGH 2
extern const char *GARRISON_TIER_NAMES[3];  // "LOW", "MEDIUM", "HIGH"

// Honor-system obligation follow-up (accepted a non-Credit negotiation
// offer -> some rounds later, a check-in transmission asks if you kept your
// word). Self-reported by design -- the software can't verify what happens
// on the physical board, same reasoning as the offers themselves.
#define MAX_PENDING_OBLIGATIONS 3
#define OBLIGATION_ROUNDS_DUE 3    // how many rounds later the check-in fires
#define OBLIGATION_FULFILL_REP 6   // reputation reward for keeping your word (v0.29.0 scale)
#define OBLIGATION_BREAK_REP -10   // reputation penalty for breaking it

// v0.29.0 REPUTATION SCALE (user 2026-10-19: "the scale may need to be a bit bigger
// so actions can get more granular"). A hidden score per faction, -50..+50; the
// player sees the tier and, on Factions, a bar with a line at their exact spot.
#define REP_MIN -50
#define REP_MAX 50
#define REP_HOSTILE_MAX -30        // -50..-30 Hostile
#define REP_WARY_MAX -10           // -29..-10 Wary
#define REP_NEUTRAL_MAX 9          //  -9..+9  Neutral
#define REP_FAVORABLE_MAX 16       // 3.1-dev.50 #15 (user: champions by round 5-6): +10..+16 Friend, +17.. Family (was 29 / 30)
#define REP_CHAMPION_MIN 23        // 3.1-dev.50 TIER 6 = CHAMPION: Family at +23 or more AND the highest at the table
#define REP_START_OWN 9            // 3.1-dev.8: your home faction -- the TOP of tier 3 (one good move from tier 4's gift)
#define REP_START_RIVAL -20        // 3.1-dev.8: its rival -- tier 2 (they're watching)
#define REP_START_DEFECTOR_OLD REP_START_RIVAL  // 3.1-dev.49 (user: "defecting drops favor low"): the faction a defector LEFT knows them -- tier 2
#define REP_START_DEFECTOR_THIRD 5               // 3.1-dev.49: ... and a small head start with a related third faction (DEFECTOR_THIRD)
#define REP_COURTED (-3)           // 3.1-dev.49 #16: winning / buying a world by talks or Credits annoys its faction's RIVAL a little
#define ERA_RESIST_FAVOR (-10)     // 3.1-dev.49 #17 THE PEOPLE RESIST: buying a world in the Rebellion costs Rebel favor (Pay stays open)
// 3.1-dev.52 THE GALLERY (#20, user 2026-11-14): anyone can WATCH another player's talk live; before its last question each other
// player may act once -- WHISPER AGAINST (GAL_WHISPER_CR, one per talk: the talker's NEXT question gets harder) or VOUCH (free,
// Friend+ with that faction: a wrong reply is removed from the next question). Named, never secret. STAKES when the talk ends:
// won = vouchers +GAL_STAKE favor with that faction, the whisperer -GAL_STAKE; lost = the other way round.
#define GAL_WHISPER_CR 2
#define GAL_STAKE 2
#define WORLD_TURN_ROUNDS 3        // 3.1-dev.49 #18: a world held this many rounds takes its holder's faction as its Sympathy
#define WORLD_TURN_ON 0            // 3.1-dev.74 (user: "I don't know if switching sides is worth it"; it broke a kept promise): OFF
// 3.1-dev.7 CLASS RULES (tunable). Strength / weakness per class (commander_art.h):
#define CLASS_WARLORD_SHIPS 1       // WARLORD: extra ship in a battle you start
// 3.1-dev.47 WARLORD REWORK (playtest 4: "makes everyone mad, can't buy anything, can't progress in favor"): the extra -10
// ("Feared") is gone; instead a world taken by force pleases its faction's RIVAL more (THE MUSCLE), and talks ask one more question.
// 3.1-dev.48 THE SCOUNDREL REWORK (playtest 3: "barely noticeable", broke after one lost fight; user 2026-11-13: "the Han Solo
// scene ... you've abused the system so much you get in trouble"): SELL IT TWICE -- whenever a Scoundrel pleases a faction (wins
// a world by talks / buys one / takes one by force / finishes a job / makes amends) its RIVAL offers to pay for the same work
// (a hologram, SELL IT TWICE or KEEP IT CLEAN): Credits by size + favor with the buyer + 1 HEAT. Heat cools only in a round
// with no sale; at SELL_HEAT_CAUGHT you're CAUGHT: both factions of that pair drop you SELL_CAUGHT_FAVOR and both strike next
// round (pay one back to stop its strike). Plus THE STASH (income floor) and MARK-UP (Pay costs more). Shown as moods.
#define SELL_CR_OUTER 5            // a world sold twice, by ring
#define SELL_CR_MID 6
#define SELL_CR_CORE 8
#define SELL_CR_JOB 5
#define SELL_CR_DEED 4
#define SELL_FAVOR 3               // favor with the faction that bought it
#ifndef SELL_HEAT_CAUGHT  // (a test build may pass -DSELL_HEAT_CAUGHT=2)
#define SELL_HEAT_CAUGHT 3         // heat that gets you caught
#endif
#define SELL_CAUGHT_FAVOR (-20)    // ... a tier down with both
#define SELL_PAYBACK_EXTRA 3       // pay back what they paid you + this to call off their strike
#define SCOUNDREL_INCOME_FLOOR 6   // THE STASH (3.1-dev.101: the floor is base income + SCOUNDREL_STASH_OVER, at least this)
#define SCOUNDREL_STASH_OVER 3
// 3.1-dev.101 THE CONSUL REWORK (playtest 2026-11-20: "infuriating", "can't make money", "couldn't hold a world"):
#define CONSUL_TREATY_DEF 2        // TREATY WORLDS: a world a Consul wins by negotiation gets this many defenders ...
#define CONSUL_TREATY_ROUNDS 2  // the round it's won + the NEXT one. 3.1-dev.114 (user: "felt a little strong ... one round"): its +1 CR now ends with it too     // ... can't be bribed / bought out for this many rounds (this one + the next) ...
#define CONSUL_TREATY_CR 1         // ... and pays this much a round while the Consul holds it
#define CONSUL_FORCE_FAVOR 3       // NO STOMACH FOR WAR: a world a Consul takes by force costs this much more favor with its faction
#define DUG_IN_DEF 2               // 3.1-dev.102 a world taken BY FORCE this round: +2 defenders against a second attack (was: closed -- dust settling)
#define STANDBY_SHIPS 2            // 3.1-dev.102 STAND BY: beaten to a world, the fleet's first attack next round gets +2 ships
#define STEADY_AFTER 2             // 3.1-dev.102 the bigger side (FH_HELP2+ more ships) wins ties; after this many lost exchanges in a row it rolls best of 2 once
#define GFH_MAX_SHIPS 3            // 3.1-dev.102 GUNS FOR HIRE (the Warlord's interactive move): up to 3 ships lent, once a round
#define GFH_MAX_CR 15
#define GFH_FAVOR 2                // ... the hirer's faction likes the Warlord more
#define GFH_RETURN_MIN 3           // ... after the battle each lent ship rolls: this or more comes home
#define ALERT_DEFENDERS 1          // negotiate, then fight: a failed talk lets you attack that world the same round, +1 defender
#define SCOUNDREL_MARKUP 2         // (MARK-UP, gone since 3.1-dev.78)
#define SCOUNDREL_BOUNTY 3         // 3.1-dev.78 PRICE ON YOUR HEAD (user: "everyone gets mad at you"): whoever takes a Scoundrel's world gets this
// 3.1-dev.79 TAKING A HELD WORLD WITHOUT A FIGHT (user: "it needs to be steep ... it should steer you toward fighting, but be an option
// if you just can't fight"): BUYOUT = Pay on a commander's world, UPRISING = Talk there.
#define BUYOUT_MIN_CR 20          // a buyout costs twice the Pay price, never under this
#define BUYOUT_MULT 2
#define UPRISING_DEFENDERS 2      // the world goes to you at once, but shaky: this many defenders
#define BLACKMAIL_CR 8             // 3.1-dev.78 the Crimson Dawn's Family gift: once, a rival pays you this -- or the table learns their debts
enum SellKind : int8_t { SELL_TALK = 0, SELL_PAY, SELL_JOB, SELL_DEED, SELL_FIGHT };
enum BcKind : int8_t { BC_MY_WORLDS = 0, BC_WORLD, BC_TALK_WORLD, BC_PLAYER };   // 3.1-dev.98 the Back Channel's conditions
enum BcHow : int8_t { BCH_ATTACK = 0, BCH_PVP, BCH_TAKE_TALK, BCH_UPRISE };
#define BC_MAX_CR 10                 // 3.1-dev.98 THE BACK CHANNEL (Consul): a private offer of 1..BC_MAX_CR, once a round
#define FILE_COOLDOWN 2              // 3.1-dev.97 PULL A FILE (user: "maybe a 2 round cooldown, not every round"): a new pull every 2 rounds
#define CLASS_WARLORD_SPOILS 3      // 3.1-dev.93 WAR SPOILS: every world a Warlord takes by force (from anyone) = +3 with their own faction
#define CLASS_WARLORD_RIVAL 6       // WARLORD: the rival's favor gain when you take a world by force (everyone else: REP_RIVAL_GAIN)
#define CLASS_CONSUL_SHORT 1        // CONSUL: the defender's extra ship in a battle you start
#define CLASS_SCOUNDREL_DISCOUNT 1  // SCOUNDREL: off every market good
#define CLASS_QUEST_FORCE 4         // WARLORD quest Conqueror: worlds taken by force
#define CLASS_QUEST_PEACE 4         // CONSUL quest Kingmaker: worlds won by talk or pay
#define CLASS_QUEST_SPY 12  // 3.1-dev.115: no more buying Sympathy -- it grows outward from your worlds (was 15)          // INFILTRATOR quest Spymaster: worlds whose Sympathy you know
#define CLASS_QUEST_FORTUNE 30      // SCOUNDREL quest Make a fortune: Credits held at once  // 3.1-dev.6: a defector's standing with the faction they left (top of Neutral)
// 3.1-dev.8 THE FAVOR SPINE (vision doc tab "Favor ladders"). Tier 1..5 = REP_HOSTILE..REP_ALLIED.
#define PAY_TIER_ADJ { 2, 1, 0, -1, -2 }  // Pay on a world whose Sympathy you know, by your tier with it
#define STALL_SHUT_MAX_TIER REP_WARY      // a faction's market stall is shut to you at tier 2 and below
#define GIFT_SURVEY_WORLDS 2              // Empire tier 4: Imperial survey -- the Defense of 2 worlds
#define GIFT_SECRET_WORLDS 2              // Crimson Dawn tier 4: a secret -- the Sympathy of 2 worlds
#define GIFT_LOAN_CR 6                    // Pykes tier 4: a loan you never repay
#define GIFT_SPICE_INCOME 1               // Pykes tier 5: a spice route, +1 income a round
#define GIFT_LINE_SHIPS 1                 // Empire tier 5: a ship of the line rides with you in every battle
#define GIFT_SAFE_SHIPS 2                 // Rebels tier 5: your home world (else your best) fights with 2 loyal Rebel ships
// 3.1-dev.10 GALAXY NEWS (vision doc tab "Galaxy News"): at every round start one TABLE-WIDE item
// (plus any grudge strike landing) and one PERSONAL item per player.
#define GRUDGE_MAX_PER_GAME 4      // grudge strikes a game, all players together
#define GRUDGE_COOLDOWN 3          // rounds before the same faction strikes the same player again
#define ERA_ROUNDS 2               // 3.1-dev.30 (user, playtest 2026-11-08: the Spice Age "lasted foreverrrr"): an Era lasts 3 rounds (was half the game)
#define GRUDGE_EMPIRE_HIT 3        // 3.1-dev.11: FIXED damage, named a round ahead -- the Imperial strike costs the named world 3 garrison; at 0 it falls
#define GRUDGE_UPRISING 2          // garrison a Rebel uprising costs
#define GRUDGE_SHAKEDOWN_CR 5      // Credits a Pyke shakedown takes
#define GRUDGE_ASSASSIN_HIT 2      // Crimson Dawn assassins: your biggest fleet loses 2 ships (never its last)
#define OFFENSIVE_HIT 3            // late game: a faction comes for one of its own worlds (warned a round ahead): -3, at 0 it falls
#define FALLEN_GARRISON 2          // what a world that fell back to its faction starts with
#define NEWS_COMMOTION_HIT 2       // garrison a commotion costs an unclaimed world
#define NEWS_REINFORCE_SHIPS 2           // garrison a faction adds to a world it nearly lost
#define NEWS_LASTSTAND_SHIPS 2           // every home world fortifies
#define RACE_FAVOR 6               // first to take the race world: favor with the faction
#define RACE_CR 3
#define DESPERATION_FAVOR 8        // a losing faction's huge reward
#define DESPERATION_CR 8
#define JOB_FAVOR 6                // a job done: favor + Credits
#define JOB_CR 4
#define JOB_FAIL_FAVOR -2          // a job taken and not done
#define JOB_ROUNDS 3  // 3.1-dev.114 (user: "jobs should always allow 3 rounds")
enum NewsId : int8_t {             // LOG_NEWS num (public)
  NEWS_COMMOTION = 0, NEWS_REINFORCE, NEWS_RACE, NEWS_MARKETDAY, NEWS_HIRING, NEWS_EMBARGO,
  NEWS_OFFENSIVE, NEWS_DESPERATION, NEWS_LASTSTAND, NEWS_STRIKE, NEWS_RACE_WON, NEWS_OFFENSIVE_LANDS,
  NEWS_ERA, NEWS_ERA_END, NEWS_CHAMPION,
  NEWS_WAR, NEWS_WAR_WON, NEWS_WAR_LOST, NEWS_SUMMIT, NEWS_SUMMIT_BROKEN, NEWS_SUMMIT_HELD, NEWS_COVER_BLOWN, NEWS_HEIST,  // 3.1-dev.13
  NEWS_SAFEHOUSE,  // 3.1-dev.42: a Crimson Dawn deed -- the whole table learns a world's garrison (planet, other = garrison)
  NEWS_FH_DEALT, NEWS_FH_TAKEN, NEWS_FH_HOLDS, NEWS_FH_TURN, NEWS_FH_BEGIN,
  NEWS_CAUGHT,
  NEWS_RESIST,
  NEWS_TALK, NEWS_WHISPER, NEWS_VOUCH,
  NEWS_BIGBUY,     // 3.1-dev.53 who = the buyer's faction, planet = the shielded world or -1, other = slot * 8 + item (table: victim = item)  // 3.1-dev.52 THE GALLERY (table only): who = the actor's faction, planet, victim = the talker's faction     // 3.1-dev.49 THE PEOPLE RESIST: someone bought a world in the Rebellion (who = the buyer's faction, planet)     // 3.1-dev.48: a Scoundrel caught playing both sides (who = one faction, other = the player)  // 3.1-dev.43 THE FINAL HOUR (TAKEN/HOLDS: faction = the winner's / holder's, planet)
  NEWS_FUEL,       // 3.1-dev.74 (user: "the board should announce to everyone when fuel is bought so it isn't easy to lie about it"): who = the buyer's faction, other = slot
  NEWS_EXPOSED,    // 3.1-dev.78 a blackmailed player refused: who = their first debt's faction, planet = its world, other = slot * 20 + (second debt world + 1)
  NEWS_UPRISING,   // 3.1-dev.79 who = the new holder's faction, planet, other = the old holder's slot
  NEWS_BUYOUT,     // 3.1-dev.79 an offer: who = the buyer's faction, planet, other = slot * 4 + holder (price in the planet's "bo")
  NEWS_BUYOUT_SOLD, NEWS_BUYOUT_KEPT,  // 3.1-dev.79 who = the buyer's faction, planet, other = buyer slot * 4 + holder slot
  NEWS_BC_BROKEN,  // 3.1-dev.98 a back-channel deal was broken (no names)
  NEWS_MARK,       // 3.1-dev.88 the Dawn's champion named the Mark: who = the Dawn, other = the Mark's slot
  NEWS_FH_STRIKE,  // 3.1-dev.123 the finale's strike now playing: who = the (first) attacker's faction, planet, other = how many strikes
  NEWS_COUNT  // 3.1-dev.12: who = faction; ERA other = declarer; CHAMPION other = new slot, planet = old slot
};
enum PNewsId : int8_t {            // LOG_PNEWS num (private)
  PN_STRIKE_WARN = 0, PN_WATCHING, PN_JOB, PN_TIPOFF, PN_DEBT, PN_STRIKE_HIT, PN_STRIKE_OFF,
  PN_COMMOTION_WHO, PN_JOB_DONE, PN_JOB_FAILED, PN_RACE_WON, PN_OFFENSIVE_HIT, PN_JOB_REMIND, PN_OFFENSIVE_WARN,
  PN_TAX_PAID, PN_TAX_COLLECTED, PN_RECORDS,  // 3.1-dev.12 Era notices (notifications, not full screen)
  PN_WAR_WARN, PN_WAR_LAPSED, PN_SUMMIT_ON_WAR, PN_SUMMIT_DIVIDEND, PN_COVER_ON, PN_COVER_END, PN_HEIST,  // 3.1-dev.13 (full screen)
  PN_JOB_TAKEN, PN_JOB_PASSED,  // 3.1-dev.16: your answer to a job (your History only); PASSED other = 1 when you let it lapse
  PN_GRUDGE_PAID,  // 3.1-dev.20: you paid them off (other = Credits)
  PN_DEED_DONE,    // 3.1-dev.42: you made amends (planet = the world or -1, other = deed kind 0 ship / 1 round / 2 world)
  PN_LOAN_BACK,    // 3.1-dev.42: a lent ship (Join the cell / Lend a blade): planet = fleet, other = 2 if it rode on a strike + 1 if it came home
  PN_BLADE_SEEN,   // 3.1-dev.42: to the VICTIM of a Dawn strike: other = the player whose ship rode with the assassins
  PN_SOLD,         // 3.1-dev.48: you sold it twice (who = the buyer, planet, other = Credits) -- your History
  PN_PAIDBACK,     // 3.1-dev.48: caught -- you paid a faction back (who, other = Credits)
  PN_TURNED,       // 3.1-dev.49 (see below)
  PN_GALLERY,      // 3.1-dev.52: your whisper / vouch was settled (who = the faction, planet, other = won*2 + whisper)
  // PN_TURNED: 3.1-dev.49: a world you held WORLD_TURN_ROUNDS rounds took your faction's side (who = your faction, planet)
  PN_BLACKMAILED, PN_BLACKMAIL_PAID,  // 3.1-dev.78 (other = the blackmailer / the payer)
  PN_BUYOUT_OFFER,  // 3.1-dev.79 someone offers to buy your world (planet, other = the buyer)
  PN_FILE_BOUGHT,   // 3.1-dev.88 someone bought your file (no who)
  PN_INSIDE,        // 3.1-dev.88 Inside man: planet, other = its defenders right now
  PN_BC_OFFER,      // 3.1-dev.98 the Back Channel: a private offer for you (other = the Consul)
  PN_BC_ANSWER,     // ... to the Consul: accepted (planet = 1) or declined (planet = 0); other = who
  PN_BC_BROKEN,     // ... to the Consul: they broke it (other = who, planet = Credits you got)
  PN_BC_BROKE,      // ... to the breaker: you broke it (other = the Consul, planet = Credits you paid back)
  PN_DEAL_FELL,     // 3.1-dev.102 the ink was still wet: they took your world by force (planet, who = its faction, other = Credits back * 2 + head start)
  PN_BEATEN,        // 3.1-dev.102 someone got to the world you had open first (planet, other = who)
  PN_GFH_OFFER,     // 3.1-dev.102 GUNS FOR HIRE: a Warlord offers you ships (other = the Warlord)
  PN_GFH_ANSWER,    // ... to the Warlord: planet = 1 accepted / 0 declined, other = who
  PN_GFH_BACK,      // ... to the Warlord: after the battle, other = came home * 8 + lent
};
// 3.1-dev.43 THE FINAL HOUR (user 2026-11-11/12; the design notes "THE FINAL HOUR"). In the SECOND-TO-LAST round everyone
// DRAWS a gambit from their class's four (comeback: the VP leader draws 1, last place 3, the rest 2 -- keep one) and a
// private BLACK MARKET offers 2 more. The LAST round: Prepare keeps Talk / Pay + CALLING IN DEBTS; Movement = LAST ORDERS
// (each fleet strikes one world anywhere -- a JUMP PASS for worlds out of reach -- or stays home to guard one, gambits
// pinned secretly to strikes); Resolution = THE REVEAL (every strike shown, gambits played at the reveal) then THE
// FINALE: world by world, Outer Rim to the Core. Strikes fight SIDE BY SIDE against the garrison; once it falls, the
// strikes still standing TURN ON EACH OTHER until one is left.
#define FH_GAMBITS 16
#define FH_HAND 3                  // gambits you can hold (the draw + the black market)
#define FH_BM_OFFERS 2
#define FH_BM_CR 15                // a black-market gambit
#define FH_JUMP_CR 3               // a jump pass (a strike on a world out of reach)
#define FH_HIRE_CR 3               // 3.1-dev.116 CASH IN (family: "a way to cash in unused credits"): a hired ship for one strike in Last Orders
#define FH_HIRE_MAX 5              // hired ships per strike (ordAlly's high nibble; the low nibble = allied ships)
#define FH_STRIKES 5               // your 3 fleets + Smuggler's Run + (3.1-dev.53) a Mercenary Armada
#define FH_HIDDEN_FLEET 4
#define FH_BOMBARD 3
#define FH_NO_RETREAT 2
#define FH_RALLY 2
#define FH_TURNCOAT 4
#define FH_HIGH_ROLLER_CR 2        // Credits per extra ship
#define FH_HIGH_ROLLER_MAX 6
#define FH_SMUGGLER_SHIPS 3
// 3.1-dev.53 #23 LATE-GAME BIG BUYS (user: Marcus has "so much money and nothing to do"; "the final hour might be part of the
// answer"): the black market also offers each player 2 random BIG-TICKET items (of these 5); you may buy ONE a game; every
// purchase is announced to the table. All of them act in the Final Hour.
enum BigItem : int8_t { BIG_CAPITAL = 0, BIG_SHIELD, BIG_ARMADA, BIG_CHARTS, BIG_SPY, BIG_COUNT };
#define BIG_PRICES { 30, 25, 20, 15, 45 }
// 3.1-dev.77 BLACK-MARKET LOTS (user: "we need more things ... useful in the late game ... cool, but not OP"): two a player, dealt
// with the gambits; each can be bought once. Smuggled troops / Fortifications pick one of your worlds; the rest wait for your next fight.
enum LotItem : int8_t { LOT_TROOPS = 0, LOT_FORTIFY, LOT_SABOTEUR, LOT_TURNCOAT, LOT_SECOND, LOT_SLICER,  // 3.1-dev.78 SLICER: one rival's file, this round
                       LOT_FORGED, LOT_BLADE, LOT_GETAWAY, LOT_INSIDE, LOT_DECOY, LOT_REFIT,  // 3.1-dev.88 cheap late-game lots
                       LOT_COUNT };
#define LOT_PRICES { 9, 10, 10, 5, 9, 8, 5, 4, 3, 4, 4, 5 }  // 3.1-dev.114 TURNCOAT 12 -> 5 (user: "steep for one ship")
#define LOT_OFFERS 4               // 3.1-dev.88: four lots a round, at least one LOT_CHEAP_MAX or less
#define LOT_CHEAP_MAX 6
#define LOT_HOLD 3                 // 3.1-dev.88 THE CARGO HOLD (user: "3 kind of makes sense"): kept until used, lost at game end
// lots that wait in the hold (the others work at once, this round)
#define LOT_KEEPS ((1u << LOT_SABOTEUR) | (1u << LOT_TURNCOAT) | (1u << LOT_SECOND) | (1u << LOT_FORGED) | (1u << LOT_BLADE) | \
                   (1u << LOT_GETAWAY) | (1u << LOT_INSIDE) | (1u << LOT_DECOY) | (1u << LOT_REFIT))
#define LOT_TROOPS_SHIPS 3         // Smuggled troops: +3 defenders, even past the limit (never past MAX_PLANET_DEFENSE)
#define LOT_SABOTEUR_HIT 2         // Saboteur: the next world you attack loses 2 defenders before the first roll
#define LOT_SECOND_SHIPS 2         // Second chance: when a fleet drops to 2 in a fight, 2 ships jump in (once)
#define BIG_CAPITAL_SHIPS 4        // join your biggest strike, past a fleet's size
#define BIG_SHIELD_HITS 3          // the shielded world's first hits in the finale don't count
#define BIG_ARMADA_SHIPS 4         // an extra strike of its own (strike slot 4)
#define FH_LONG_CON 3
#define FH_DEBT_CHAMP 3            // ships a faction sends when you call in its debt (free)
#define FH_DEBT_FAMILY 2
#define FH_DEBT_FRIEND 1
#define FH_DEBT_NEUTRAL_CR 0       // 3.1-dev.74 (user: "calling in your debts shouldn't cost money - its debts!"): Neutral and Wary owe you nothing (was 6 CR a ship)
#define FH_DEBT_NEUTRAL_MAX 2
#define FH_DEBT_WARY_CR 0          // (was 10 CR for one ship)
#define FH_HELP2 3                 // invisible help (#9) vs an unclaimed garrison: this many MORE ships = best of 2 ...
#define FH_HELP3 7                 // ... = best of 3
#define FH_LOG_LEN 900
enum Gambit : int8_t {             // index = class * 4 + k
  G_HIDDEN_FLEET = 0, G_BOMBARD, G_NO_RETREAT, G_RALLY,      // WARLORD
  G_TREATY, G_PEACE, G_TURNCOAT, G_OLD_FRIENDS,              // CONSUL
  G_INTERCEPT, G_SABOTAGE, G_FALSE_FLAG, G_GHOST,            // INFILTRATOR
  G_LOADED, G_HIGH_ROLLER, G_SMUGGLER, G_LONG_CON            // SCOUNDREL
};
#define FH_PINNED_MASK ((1u << G_HIDDEN_FLEET) | (1u << G_BOMBARD) | (1u << G_NO_RETREAT) | (1u << G_TURNCOAT) | (1u << G_GHOST) | (1u << G_LOADED) | (1u << G_HIGH_ROLLER))
#define FH_REVEAL_MASK ((1u << G_RALLY) | (1u << G_TREATY) | (1u << G_PEACE) | (1u << G_OLD_FRIENDS) | (1u << G_SABOTAGE) | (1u << G_FALSE_FLAG) | (1u << G_LONG_CON))
#define PUBLIC_NEWS_BASE 60        // publicMoment = PUBLIC_NEWS_BASE + NewsId (the table)
// 3.1-dev.12 CHAMPIONS + ERAS. A faction's CHAMPION = the player at tier 5 with it holding the highest favor
// (ties keep the holder). Gaining the title gives ONE DECREE (lose the title first = lose the decree). A decree
// starts that faction's ERA (eraFaction = the faction): one rule that's good for everyone, one STING the
// faction's tier-5 players are spared from, and a perk for the faction's current champion. One Era at a time
// (a new decree replaces it); it lasts half the game.
// 3.1-dev.88 THE NEW FOUR (user 2026-11-18): 2 rounds, LOCKED once declared, every sting hits EVERYONE (the champion too -- "easier
// that way"), the champion's perk is the reward. Each number an Era touches is tagged on the phone (never a silent change).
#define ERA_SPICE_MULT 2            // Spice Age, everyone: income doubled ("2 credits isn't a real boost"), every shop puts out two goods
#define ERA_SPICE_CHAMP_MULT 3      // ... champion: income tripled
#define ERA_SPICE_PAY_PCT 150       // ... sting: buying a world costs x1.5 (ships / shops / lots unchanged)
#define ERA_TAX_PER_WORLD 1         // Imperial Order sting: 1 CR a round per world held past ERA_TAX_FREE (champion included)
#define ERA_TAX_FREE 2
#define ERA_PATROL_PCT 50           // Imperial Order, everyone: patrols half price
#define ERA_FILE_CR 6               // The Dawn grips the galaxy, sting: anyone may buy another player's file, once a round
#define ERA_MARK_CR 5               // ... champion NAMES THE MARK: taking a world from the Mark pays this, from the Dawn
#define WARPATH_SHIPS 3             // 3.1-dev.88 WARPATH (replaces Declare war): declared in Prepare, struck the same Resolution
#define ERA_HOPE_TALK_GARRISON 2    // Rebellion Rises champion: a world you win by talks holds +2
#define ERA_LIE_LEN 60              // Shadow Age champion: one Holonet lie a round
// 3.1-dev.13 ONCE-A-GAME MOVES (vision doc tab "Characters and classes"; once-a-game v2-v6 in the design notes).
#define MOVE_FROM_ROUND 3           // no move before round 3
#define WAR_CAP 12                  // DECLARE WAR: ships pulled from your worlds' garrisons (each keeps 1) ...
#define WAR_BONUS 2                 // ... + 2 in the fight
#define SUMMIT_WORLDS 3             // SUMMIT: up to 3 worlds ...
#define SUMMIT_ROUNDS 2             // ... for 2 rounds (this one and the next): no fights there
#define SUMMIT_KEEPERS 6            // break it: 2 peacekeeper ships from each faction but the world's rival
#define SUMMIT_BREAK_FAVOR -10      // ... and the breaker drops with those three
#define SUMMIT_DIVIDEND 10          // held both rounds: the Consul gains half a step with every faction
#define SUMMIT_PAY_OFF 2            // the Consul during the Summit: Pay -2 (and talks -1 question)
#define COVER_ROUNDS 3              // DEEP COVER: 3 rounds as Family of another faction
#define COVER_SCORE 35              // (the favor you seem to have)
#define COVER_BLOWN -20             // attacking their world: cover blown, a tier down
#define STORY_LEN 60                // one planted Holonet story while under cover
#define HEIST_SMOOTH_CR 5           // THE HEIST, smooth: +5 CR and a free good, no roll
#define HEIST_BOLD_CR 10            // bold, a die 3-6: +10 CR, a good and a stolen ship ...
#define HEIST_BOLD_MIN 3
#define HEIST_FAIL_CR 3             // ... 1-2: you escape with 3 CR and drop a tier with them
#define HEIST_FAIL_FAVOR -20
#define HEIST_PUSH_CR 10            // 3.1-dev.20 push your luck after a bold success: 3-6 = +10 more; 1-2 = lose the 10 and a tier
#define GRUDGE_PAYOFF_CR 12         // 3.1-dev.20 pay a grudge off before it lands (user: "it should be expensive")
#define GRUDGE_PAYOFF_PYKE_CR 8     // 3.1-dev.40 (playtest 3: paying off the Pykes cost ~3x the 5 CR they could take) -- ~1.5x
// 3.1-dev.42 MAKE AMENDS (user 2026-11-11): every faction has 3 deeds of the same three KINDS -- a SHIP, a ROUND, a WORLD.
// A deed calls the strike off and lifts your favor a FULL STEP (out of tier 1: "they respect what you did"); paying
// Credits calls it off but only lifts HALF A STEP (you stay in tier 1: "they don't trust you yet").
#define DEED_SHIP 0
#define DEED_ROUND 1
#define DEED_WORLD 2
#define GRUDGE_PAY_FAVOR 10        // paying off: +10 favor, never above tier 1
#define DEED_TROOPS 4              // Empire, Lend troops: garrison ships from a world (it keeps >= 1) -- 3.1-dev.72 (user: deeds "don't hit as hard" as the 12 CR payoff): was 2
#define DEED_LOAN_ROUNDS 2         // Rebels Join the cell / Dawn Lend a blade: the ships come home 2 rounds later
#define DEED_CELL_SHIPS 3          // 3.1-dev.72: Join the cell lends 3 ships (was 1)
#define DEED_HAULER_SHIPS 2        // 3.1-dev.72: Hand over a hauler gives 2 ships for good (was 1)
#define DEED_BLADE_SHIPS 2         // 3.1-dev.72: Lend a blade lends 2 ships, each rolls its own 1-2 (was 1)
#define DEED_SHELTER 3             // 3.1-dev.72: Shelter refugees costs the world 3 defenders for good (was 1)
#define DEED_BLADE_LOST_MAX 2      // Lend a blade: if it rides on a strike, a die roll of 1-2 = it doesn't come back
#define DEED_WORLD_ROUNDS 3        // Lockdown / Collateral last this round + the next two (3.1-dev.72: was 2)
static inline int grudgePayoffCr(int f) { return f == 2 ? GRUDGE_PAYOFF_PYKE_CR : GRUDGE_PAYOFF_CR; }  // f 2 = the Pykes
#define WAR_LOG_LEN 40
#define TABLE_MOMENT_MS 8000        // 3.1-dev.15: the table shows each queued moment this long (= TABLE_ALERT_MS in the .ino)
#define REP_PAY 3                  // you bought one of their planets
#define REP_WALKOUT -2             // you left their negotiation mid-conversation
#define REP_NEG_FLAWLESS 5         // v0.30.0: every answer landed -- the planet, no strings
#define REP_NEG_FAVOR 2            // won it by promising a favor
#define REP_NEG_REFUSE -2          // they asked a favor and you said no
#define REP_NEG_CUTOFF -6          // two offending replies: they cut you off
#define REP_CONQUEST -12           // you took one of their planets by force
#define REP_RIVAL_GAIN 4           // ...and their rival is glad
// Arrows shown for a change: |d| < 5 one, < 10 two, more three (both platforms).
#define REP_ARROWS_TWO 5
#define REP_ARROWS_THREE 10

// v0.25.0 balance (agreed 2026-10-12, user: "the more the cpu reacts the
// funner"; 2026-10-16: "levels between planets in the same rings"). Every
// planet sits on a ring of map A2/A3; its starting garrison is the ring's base,
// +1 if its Defense trait is High -- so revealing Defense tells you exactly
// where it sits in its ring. Pay prices follow the garrison (snapshotPayPrice).
// 3.1-dev.76 DEFENSE (user 2026-11-15: "defending your planet against 2 ships is impossible ... there should be a way to raise
// defenses"): limits once a world is held, a PATROL you hire and must keep paying for, worlds that rebuild on their own.
#define DEF_LIMIT_OUTER 6
#define DEF_LIMIT_MID 8
#define DEF_LIMIT_CORE 10
#define DEF_FORT_BONUS 2          // Fortifications (a black-market lot, dev.77): +2 on one world, for good
#define PATROL_CR 6               // hired guards: 2 ships on one of your worlds for 2 rounds (this one + the next)
#define PATROL_SHIPS 2
#define RALLY_AFTER 3             // 3.1-dev.101 RALLY: after this many exchanges lost in a row, a side's next roll gets +1
#define PATROL_ROUNDS 2
#define SQUEEZE_FLEET_SHIPS 2     // 3 players: the seat between the other two starts with Fleets 2 and 3 at 2 ships each
#define RING_OUTER 0
#define RING_MID 1
#define RING_CORE 2
#define GARRISON_OUTER 2          // Outer Rim: 2 (Low) or 3 (High)
#define GARRISON_MID 4            // Mid Rim: 4 or 5
#define GARRISON_CORE 7           // Core Worlds: 7 or 8 -- they want a combined assault
#define GARRISON_HIGH_DEFENSE 1   // extra for a High Defense trait
#define FACTION_ALERT_GARRISON 1  // each planet taken BY FORCE from a faction: its other unclaimed planets +1
#define REGROWTH_EVERY_ROUNDS 2   // unclaimed planets regain ...
#define REGROWTH_AMOUNT 1         // ... this much, up to their starting garrison
// v0.25.4: capital guard -- a seat of power off the Core starts at this (+1 for
// High Defense), so no capital is a free round-1 grab; Coruscant keeps the
// Core's 7-8. Prices come only from public things + a per-game market value
// (never the garrison or Resources -- the price used to leak both).
// HOME GUARD (2026-11-01): a home world held by its own faction keeps this
// guard; a conqueror holds it with an ordinary Outer Rim garrison. Only a fight
// takes a home world (no Negotiate, no Pay). A neutral home (nobody plays that
// faction) has the smaller guard.
#define HOME_GUARD 6
#define HOME_GUARD_NEUTRAL 4
#define PRICE_OUTER 5
#define PRICE_MID 8
#define PRICE_CORE 13
#define PRICE_MARKET_MAX 2       // each planet +0..2, rolled per game
// v0.30.0 ESCALATING PAY (user 2026-10-19: cash deals left negotiation; "maybe
// buying your first planet is cheaper... and it goes up everytime"): your first
// purchase costs PAY_FIRST_DISCOUNT less, each one after it PAY_STEP more.
#define PAY_FIRST_DISCOUNT 2
#define PAY_STEP 3

// v0.30.0 NEGOTIATION = A CONVERSATION (user 2026-10-19, Fire Emblem recruiting;
// mockup https://claude.ai/artifact/4KsxPW9rF5B8BHdSZiyLz1). The planet's
// faction asks questions from the shared bank (negotiation_bank.h): easy, then
// medium, then hard. Graded by MISSES (a reply they didn't like): none = the
// planet with no strings; one = pick one of three favors; two or more = they
// name one hard favor (their first). Two OFFENDING replies = cut off.
#define NEG_Q_OUTER 3
#define NEG_Q_MID 4
#define NEG_Q_CORE 5
#define NEG_MAX_Q 5
#define NEG_CUTOFF_OFFENDS 2    // offending replies that end the call
#define NEG_LOSSES_CLOSED 2     // conversations lost at a planet before it closes to you for good
#define NEG_FAVORS 3            // favors drawn for a favor stage (v0.30.1: from NEG_FAVOR_BANK)
// v0.30.1 the LAST rounds (user: "Expensive planets! Favors tough to accomplish in
// 1-2 rounds, but could be a hail mary"): the favor stage draws from the late
// bank -- Credits now at your PAY price x 3/2 (at least +4), or a deed due when
// the war ends; undone at the end = they take the planet back before the count.
#define NEG_LATE_ROUNDS 2
#define NEG_LATE_PRICE_NUM 3
#define NEG_LATE_PRICE_DEN 2
#define NEG_LATE_PRICE_MIN_EXTRA 4
// GameState.negResult
#define NEG_TALKING 0
#define NEG_WON 1               // no strings
#define NEG_WON_FAVOR 2         // you owe a favor (or paid the late price)
#define NEG_CUT_OFF 3
#define NEG_ENDED 4             // you refused their favor
#define NEG_HOSTILE 5           // v0.30.1: they're hostile -- FIGHT or WALK AWAY
// GameState.negMarks
#define NEG_MARK_NONE 0
#define NEG_MARK_LIKED 1
#define NEG_MARK_NEUTRAL 2
#define NEG_MARK_OFFENDED 3
#define NEG_REFUSE 3            // gameStateNegotiationAnswer: "nothing more" in the favor stage

struct PlanetInfo {
  const char *name;
  const char *characteristic[NUM_CHARACTERISTICS];
  int ring;          // RING_OUTER / RING_MID / RING_CORE (v0.25.0)
  int basePayPrice;  // starting "asking price" before resource/defense scaling
};

// Starting garrison of an unclaimed planet: its ring's base, +1 for High Defense.
int planetStartGarrison(int planetIdx);
struct GameState;
// v0.25.4 the shuffled galaxy (live state; see GameState.planetFactionOf).
int planetFaction(int planetIdx);        // FACTION_*
bool planetResHigh(int planetIdx);
bool planetDefHigh(int planetIdx);
int planetHomeOf(int planetIdx);          // FACTION_* whose home world it is (Yavin 4, Mustafar, Oba Diah, Dathomir), else -1
int homePlanetOf(int faction);            // the reverse, -1 if none
int homeSlotOf(const GameState &gs, int faction);  // the seated player of that faction, else -1
const char *planetCharValue(const GameState &gs, int planetIdx, int charIdx);  // text the intel tiles show
const char *planetCharValueNow(int planetIdx, int charIdx);  // same, from the live state

extern const PlanetInfo PLANETS[NUM_PLANETS];
extern const int REVEAL_COST[NUM_CHARACTERISTICS];
extern const char *ATK_SHIP_NAMES[FLEET_SHIPS];
extern const char *PHASE_NAMES[3];

// A negotiation offer. creditCost is mechanically enforced (deducted on
// accept). If creditCost is 0 the offer's "cost" is an honor-system
// obligation described in text -- players self-enforce (per design 6B).
// v0.28.1: promises the GAME can judge (playtest 2026-10-16: "some favors the
// computer can know. If I overtook an empire planet, it should know that").
// Do-promises are kept the moment you do it and broken if the deadline
// passes; don't-promises are broken the moment you do it and kept at the
// deadline. PROMISE_HONOR stays a self-reported check-in.
enum PromiseCheck : uint8_t {
  PROMISE_HONOR = 0,       // on your honor (the map can't see it yet)
  PROMISE_TAKE_FACTION,    // do: gain a planet of faction `target` by the deadline
  PROMISE_WIN_PVP,         // do: destroy another commander's fleet in battle by the deadline
  PROMISE_NO_INVEST,       // don't: invest in this planet's defense until the deadline
  PROMISE_NO_ATTACK,       // don't: attack a planet of faction `target` until the deadline
  PROMISE_HOLD,            // keep this planet until the deadline (lost at a round start = broken)
  PROMISE_TRIBUTE,         // pay `target` Credits at each round start until the deadline (can't = broken)
  PROMISE_WIN_BATTLE,      // v0.30.1 do: capture a planet in a fight, or destroy another commander's fleet
  PROMISE_TAKE_CORE,       // v0.30.1 do: gain a Core world
  PROMISE_HOLD_TWO,        // v0.30.1 do: hold this planet and a planet of faction `target` at the same time
};
// (The favors themselves live in negotiation_bank.h -- NegFavor, v0.30.1.)

// A promised honor-system obligation from an accepted negotiation offer,
// awaiting a self-reported check-in some rounds later.
struct PendingObligation {
  bool active = false;
  int factionIdx = -1;
  int planetIdx = -1;
  int dueRound = -1;
  char text[120] = "";   // the original offer's obligation text
  uint8_t check = 0;     // v0.28.1: PromiseCheck (0 = honor system, a check-in)
  int8_t target = -1;    // the faction (TAKE / NO_ATTACK / HOLD_TWO) or Credits per round (TRIBUTE)
  int8_t offer = -1;     // which favor (NEG_FAVOR_BANK index since v0.30.1) -- for the history
};

// v0.28.3: a promise that was kept or broken (playtest: "A history of your
// promises might be nice"). The text comes back from the offer table.
struct PromiseRecord {
  int8_t faction = -1, planet = -1, offer = -1, round = -1;
  uint8_t kept = 0;
};
#define PROMISE_HISTORY 4  // per player, newest first

// Faction roll modifiers for one planet battle, with who grants them (for
// the briefing: "d6 +1 (REBEL ALLIANCE support)").
struct BattleMods {
  int atkMod = 0;             // added to the attacker's d6
  int atkSourceFaction = -1;  // faction whose support grants it, -1 = none
  int defMod = 0;             // added to the garrison's d6
  int defSourceFaction = -1;  // faction whose resentment grants it, -1 = none
  // 2.0-dev.24: atkMod / defMod are only bought ones now (GOOD_MERCENARIES);
  // the faction help is ships. Source factions above = whose escort / militia.
  int atkEscort = 0;          // escort ships (BATTLE_ESCORT_SHIPS) from atkSourceFaction
  int defMilitia = 0;         // militia ships (BATTLE_MILITIA_SHIPS) from defSourceFaction
  int atkMercs = 0;           // 2.0-dev.26: your hired ships (GOOD_MERCENARIES)
};

struct Player {
  bool active = false;          // slot in use
  bool connected = false;       // currently has a live client (phones)
  char id[24] = "";             // stable client id (phones store this to rejoin)
  char name[20] = "";           // display name
  int chosenFaction = -1;       // FACTION_REBEL..FACTION_CRIMSON_DAWN, -1 = not yet chosen
  int commander = -1;           // commander id (faction*3 + i, see commander_art.h), -1 = not yet chosen (v0.21.20)
  int credits = 12;
  int lastIncome = 0;  // v0.23.1: what the latest round start paid (shown on the Invest banner)
  bool lockedIn = false;        // ready for the current phase
  // 2.0-dev.13 DROP OUT / REJOIN (board O; user: "keep working if someone drops
  // out ... rejoin anytime and jump back in"): AWAY counts as locked in.
  uint8_t away = 0;             // AWAY_NONE / AWAY_QUIET / AWAY_STEPPED
  uint8_t welcome = 0;          // 1 = back: show "while you were away" once
  int16_t awayRound = -1, awayVp = 0, awayCr = 0;  // where they stood when they left
  uint32_t awayWorlds = 0;      // the worlds they held then (bit per planet)
  bool revealed[NUM_PLANETS][NUM_CHARACTERISTICS] = {};
  uint32_t intelEyed = 0;       // 2.0-dev.41: worlds this player BOUGHT intel on since the last Movement (bit per planet; the rumor's source)
  // Each player has NUM_FLEETS fleets, each holding up to FLEET_SHIPS ships.
  // fleetShipCount = how many ship slots have been commissioned (built) in
  // that fleet so far (starts at FLEET_START_SHIPS, investable up to
  // FLEET_SHIPS). fleetShipAlive tracks which commissioned slots are still
  // alive; a destroyed ship stays false until repaired (v0.27.3: Invest, 2 CR).
  // Slots at/beyond fleetShipCount are always false (not yet built).
  int fleetShipCount[NUM_FLEETS] = { FLEET_START_SHIPS, FLEET_START_SHIPS, FLEET_START_SHIPS };
  bool fleetShipAlive[NUM_FLEETS][FLEET_SHIPS] = {};
  // Hidden reputation score per faction, -10..+10, starts at 0 (Neutral).
  // Never shown to the player as a raw number -- only the tier it maps to,
  // surfaced via the Faction Transmission screen on tier changes.
  int reputation[NUM_FACTIONS] = { 0, 0, 0, 0 };
  // Honor-system obligations awaiting a future check-in.
  PendingObligation obligations[MAX_PENDING_OBLIGATIONS];
  PromiseRecord promiseLog[PROMISE_HISTORY];  // v0.28.3: newest first
  // Whether this player has ever LOST a Fight at each planet -- the real
  // garrison ship count for a planet only unlocks once you've fought there
  // and lost. Otherwise the UI only shows a qualitative tier (Low/Medium/
  // High), never the live number.
  bool hasLostAt[NUM_PLANETS] = {};
  // Victory Points -- fully public (shown to every player, unlike reputation).
  // Per-round commitment (v0.20.2; both cleared when a new round starts):
  // fleetSpent -- this fleet already attacked this round (win, lose, or
  //   withdraw), so one fleet can't chip at a garrison, retreat, and repeat.
  //   A different fleet can still follow up the same round.
  // takeoverLocked -- you committed to an approach on this planet this
  //   round (CONFIRM FIGHT, or negotiation offers revealed), so you can't
  //   peek and then switch approach ("saw the defense, I'll negotiate").
  bool fleetSpent[NUM_FLEETS] = {};
  bool takeoverLocked[NUM_PLANETS] = {};
  int vp = 0;
  uint8_t purchases = 0;                // v0.30.0: planets bought this game (PAY escalates)
  uint8_t negLosses[NUM_PLANETS] = {};  // v0.30.0: conversations lost per planet (NEG_LOSSES_CLOSED = closed for good)
  // --- 2.0 ---
  int8_t diplomatPlanet = -1;   // where your diplomat is (-1 = home)
  int8_t diplomatBack = -1;     // the round it comes back
  int8_t freeRevealRound = -1;  // TR_SPYNET: the round your free reveal was used
  int8_t silentRound = -1;      // Crimson Dawn: the round silentEarned counts
  uint8_t silentEarned = 0;
  uint8_t nextBattleMercs = 0;  // GOOD_MERCENARIES: hired ships for your next battle (2.0-dev.26; was a +1 roll)
  uint8_t nextBattleReroll = 0; // GOOD_BLESSING: rerolls in your next battle
  uint8_t cargoDue = 0;         // GOOD_CARGO: Credits paid at the next Prepare
  int8_t harborRound = -1;      // GOOD_HARBOR: the round your fleets can't be attacked by players
  uint8_t permitDiscount = 0;   // GOOD_PERMITS: off your next Pay
  uint8_t rebelWins = 0;        // Rebel quest progress
  uint8_t questDone = 0;        // set at the end: 1 = quest completed (+QUEST_VP)
  uint8_t forceTakes = 0;       // 3.1-dev.7: worlds taken by force (Conqueror)
  uint8_t peaceTakes = 0;       // 3.1-dev.7: worlds won by talk or pay (Kingmaker)
  uint8_t peakCredits = 0;      // 3.1-dev.7: most Credits held at once (Make a fortune)
  uint8_t giftsGot = 0;         // 3.1-dev.8: bit f = faction f's tier-4 gift was given (never twice)
  int8_t grudgeFaction = -1;    // 3.1-dev.10: a faction coming for you (warned) ...
  int8_t grudgeRound = -1;      // ... lands at the start of this round (unless you climb out of tier 1 first)
  int8_t grudgeTarget = -1;     // 3.1-dev.11: the world named in the warning (Empire / Rebels), -1 otherwise
  int8_t lastStrike[4] = { -10, -10, -10, -10 };  // round each faction last struck you
  int8_t noAtkRound = -1, noAtkF = -1;  // 3.1-dev.42 a ROUND deed: no fleet attacks through this round (faction you promised)
  uint8_t shakenMask = 0; int8_t shakenRound = -1;  // 3.1-dev.76 SHAKEN: fleets that ran from a player battle can't attack in shakenRound
  int8_t lockWorld = -1, lockUntil = -1;   // 3.1-dev.42 Empire Lockdown: no adding garrison there through lockUntil
  int8_t collatWorld = -1, collatUntil = -1;  // 3.1-dev.42 Pyke Collateral: its income goes to the Pykes through collatUntil
  int8_t loanFleet = -1, loanBack = -1, loanKind = 0, loanN = 0;  // 3.1-dev.72 loanN = how many ships are away;  // 3.1-dev.42 a lent ship: from this fleet, home at the start of loanBack (1 cell, 2 blade)
  int8_t grudgeDeed = -1;       // 3.1-dev.42 the ONE deed this grudge asks for (user: "offered one at a time") ...
  int8_t deedNext[4] = { 0, 1, 2, 0 };  // ... each faction asks for its deeds in turn (ship / round / world), skipping what you can't do
  int8_t lastWatch = -10;       // round you last got "they're watching"
  int8_t jobFaction = -1, jobWorld = -1, jobDue = -1;  // 3.1-dev.10 a job: take jobWorld by the end of round jobDue
  uint8_t jobState = 0;         // 0 none, 1 offered (answer it), 2 taken
  uint8_t jobAnnounced = 0;     // 1 = "the <faction> are hiring" was on the table
  uint8_t jobsOffered = 0;      // jobs are shared fairly: the next one goes to whoever has had fewest
  uint8_t decrees = 0;          // 3.1-dev.12: bit f = you hold faction f's decree (from gaining its title)
  uint8_t moveUsed = 0;         // 3.1-dev.13: your once-a-game move is spent
  int8_t heistPushF = -1, heistPushRound = -1;  // 3.1-dev.20: a bold heist that worked -- you may push your luck this round
  int8_t coverFaction = -1, coverUntil = -1;  // DEEP COVER: living as Family of this faction until the end of coverUntil
  int16_t coverSaved = 0;       // your real favor with them
  uint8_t storyUsed = 0; int8_t storyRound = -1;  // the planted story airs in this round's Movement
  char story[STORY_LEN + 4] = "";
  int8_t brkPlanet = -1, brkRound = -1;  // you confirmed ATTACK ANYWAY here this round (ceasefire / cover)
  int8_t spyOn = -1;            // (3.1-dev.8 spy at court -- replaced by Blackmail in 3.1-dev.78)
  int8_t fileOn = -1, fileRound = -1;    // 3.1-dev.78 THEIR FILE (Infiltrator): whose file you hold, and the round you picked it
  int8_t sliceOn = -1, sliceRound = -1;  // 3.1-dev.78 the Slicer lot: one rival's file (+ their defenders), this round
  uint32_t upriseTried = 0;     // 3.1-dev.79 one uprising a world, a game
  uint8_t blackmailUsed = 0; int8_t bmBy = -1, bmRound = -1;  // 3.1-dev.78 Blackmail: used yours; someone blackmailing YOU (answer it)
  uint16_t noteSeq = 0;         // a new personal notice (diplomat back, a sighting, an event) bumps this
  char note[72] = "";
  // 3.1-dev.43 THE FINAL HOUR (see FH_* above)
  int8_t fhDeal[3] = { -1, -1, -1 }; uint8_t fhDealN = 0;  // the gambits drawn for you -- keep one (fhDealN 0 = nothing to pick)
  uint16_t fhHand = 0;           // bit g = you hold gambit g (unplayed)
  int8_t fhBm[FH_BM_OFFERS] = { -1, -1 };  // your private black market (-1 = bought / none)
  uint8_t fhCalled = 0;          // bit f = you called in that faction's debt
  uint8_t fhAllyFrom[4] = {};    // ships each faction sent you
  int8_t ordT[FH_STRIKES] = { -1, -1, -1, -1, -1 };  // LAST ORDERS: the world each fleet strikes (3 = Smuggler's Run), -1 = stays home
  int8_t ordGuard[3] = { -1, -1, -1 };           // a fleet that stays home guards this world of yours
  uint8_t ordAlly[FH_STRIKES] = {};              // allied ships riding with each fleet
  int8_t ordG[FH_STRIKES] = { -1, -1, -1, -1, -1 };  // the gambit pinned to each strike
  int8_t rvG[FH_HAND] = { -1, -1, -1 }, rvA[FH_HAND] = { -1, -1, -1 }, rvB[FH_HAND] = { -1, -1, -1 };  // gambits played AT THE REVEAL
  // 3.1-dev.48 SELL IT TWICE (Scoundrels): the offer on the table, heat, and getting caught
  int8_t bigOff[2] = { -1, -1 }, bigOwned = -1, bigArg = -1;  // 3.1-dev.53 the big-ticket offers, the one you bought (+ its world)
  int8_t lotOff[LOT_OFFERS] = { -1, -1, -1, -1 }; uint16_t lotsGot = 0;  // 3.1-dev.77 black-market lots: on offer, bought this round (mask)
  int8_t hold[LOT_HOLD] = { -1, -1, -1 };  // 3.1-dev.88 THE CARGO HOLD: lots kept until used
  int8_t fileBuyRound = -1;               // 3.1-dev.88 the Dawn's Era: bought someone's file this round
  // 3.1-dev.98 THE BACK CHANNEL (the Consul's offer this round): to whom, which condition (BcKind), its world / player, the price,
  // the round, and its state (0 none, 1 offered, 2 accepted, 3 declined, 4 broken)
  int8_t bcTo = -1, bcKind = -1, bcArg = -1, bcCr = 0, bcRound = -1, bcSt = 0;
  uint8_t defSeen[NUM_PLANETS] = {};  // 3.1-dev.101: defenders you SAW there when you revealed Defense, + 1 (0 = never looked)
  int8_t alertWorld = -1, alertRound = -1;  // 3.1-dev.101: a talk failed here this round -- you may fight it, they're on alert
  int8_t shopRound = -1; uint8_t shopMask = 0;  // 3.1-dev.101: one item per faction shop per round (bit per faction)
  uint8_t headStart = 0;        // 3.1-dev.102: bit per faction -- your next talk with them starts one question in (a deal that fell through)
  int8_t troubleRound = -1;     // 3.1-dev.102: the round you were last paid "for your trouble" (once a round)
  uint8_t standByMask = 0; int8_t standByRound = -1;  // 3.1-dev.102 STAND BY: fleets with +STANDBY_SHIPS on their first attack in standByRound
  int8_t gfhTo = -1, gfhRound = -1, gfhSt = 0;  // 3.1-dev.102 GUNS FOR HIRE (the Warlord's side): 1 offered, 2 accepted, 3 declined
  uint8_t gfhCr = 0, gfhN = 0; uint32_t gfhMask = 0;  // ... price, ships, which ones (bit = fleet * 8 + slot)
  uint8_t hiredN = 0; int8_t hiredFrom = -1, hiredUntil = -1;
  int8_t beatenWorld = -1, beatenRound = -1;  // 3.1-dev.102: the world someone took while you had it open (the three choices)  // ... (the hirer's side): ships for your next battle, from whom, last round
  int8_t sellTo = -1, sellKind = -1, sellPlanet = -1, sellRound = -1; uint8_t sellCr = 0;
  uint8_t heat = 0; int8_t heatSold = -1;          // heat + the last round you sold
  uint8_t soldCr[4] = {};                          // Credits each faction paid you (the payback)
  int8_t caughtF[2] = { -1, -1 }, caughtRound = -1;  // CAUGHT: these two strike at the start of caughtRound
  int8_t champOffer = -1, titleBlock = -1;  // 3.1-dev.49 #13 ONE TITLE: a second faction you top (swap for it?); the title you gave up
};

// One queued Faction Transmission (v0.23.0).
#define TX_QUEUE_LEN 4
struct TxMsg {
  int16_t line = -1;     // TX_LINES index (transmission_bank.h); -1 = obligation check-in
  int8_t faction = -1;   // who speaks
  int8_t planet = -1;    // fills {planet}; -1 = none
  int8_t fromTier = -1;  // tier chip "FROM -> TO"; -1 = no change to show
  int8_t toTier = -1;
  uint8_t big = 0;       // 1 = big moment: gold PRIORITY chip, longer static
  int8_t obIdx = -1;     // check-in: which obligations[] slot
  int8_t cause = -1;     // v0.28.3: TxCause of a normal line (KEPT / BROKE get a PROMISE title)
  int8_t delta = 0;      // v0.29.0: how far this faction's standing moved (arrows), 0 = none
};

// 3.1-dev.80 BATTLE REPLAY (user: "I would like to be able to tap into battles in the history and see the battle's course"; "I don't want
// to use up all the memory"): the last BATTLE_RECS battles, each exchange as one byte (attacker roll << 4 | defender roll).
#define BATTLE_RECS 8
#define BATTLE_REC_LOG 24
struct BattleRec {
  int8_t round = -1, kind = 0, atk = -1, def = -1, planet = -1;
  uint8_t a0 = 0, d0 = 0, n = 0, result = 0, retreat = 0;  // start counts, exchanges, 1 attacker won / 2 defender held / 3 withdrew, who withdrew
  uint8_t log[BATTLE_REC_LOG] = {};
};
struct GameState {
  // --- Lifecycle ---
  int stage = STAGE_LOBBY;
  int roundLimit = DEFAULT_ROUND_LIMIT;  // selectable in the lobby: 6, 8, or 10
  bool eventsOn = false;  // 3.1-dev.18: OFF by default (user: revisit as game modes later). 2.0-dev.33 Lobby switch (user: "not sure if we are ready for mid game events on this playtest")
  bool marketsOn = true;  // 2.0-dev.32 Lobby switch (user: markets may be "a little too much for some people's first game")

  // --- Players ---
  Player players[MAX_PLAYERS];

  // --- Round / Phase ---
  int phase = PHASE_INVEST;
  int round = 1;

  // --- Planet control & defense ---
  // Which of the 12 planets are actually in play this game (selectable by
  // any player while still in the Lobby). Defaults to all included --
  // explicitly set in gameStateInit(), NOT via this initializer list (an
  // under-sized initializer here caused a real bug once already with
  // planetOwner, below -- see gameStateInit()).
  bool planetIncluded[NUM_PLANETS] = {};
  // Real initial values (PLANET_UNCONTROLLED for every planet, all 12) are
  // set in gameStateInit()'s loop, not here -- an under-sized initializer
  // list here is exactly what caused a real bug once already (see the fix
  // comment in gameStateInit()), so this stays a plain zero-init and the
  // loop is the single source of truth.
  int planetOwner[NUM_PLANETS] = {};
  // Garrison ships currently defending each planet. Initialized to each
  // planet's baseDefense; an owner can invest during Invest phase to add more
  // (up to MAX_PLANET_DEFENSE).
  int planetDefense[NUM_PLANETS] = { 0, 0 };

  // --- Active battle (one at a time, globally) ---
  int battleKind = BATTLE_NONE;   // BATTLE_NONE / BATTLE_PLANET / BATTLE_PVP
  bool battleActive = false;
  int battleAttackerSlot = -1;
  int battleAttackerFleet = -1;   // the attacker's first fleet in the battle (see battleAttackerFleets)
  uint8_t battleAttackerFleets = 0;  // v0.28.0: bit per attacking fleet (combined assault)
  uint8_t battleDefenderFleets = 0;  // v0.28.0: bit per defending fleet (PvP; the defender may reinforce)
  // PLANET battle target:
  int battleTargetPlanet = -1;
  // PVP battle target:
  int battleDefenderSlot = -1;    // the player being attacked
  int battleDefenderFleet = -1;   // which of their fleets is defending
  // Working copies of the engaged fleets' ship-alive arrays during a battle.
  bool atkAlive[BATTLE_SHIPS] = {};  // working copy, populated when a battle starts (index f * FLEET_SHIPS + s)
  bool defAlive[BATTLE_SHIPS] = {};
  int defRemaining = 0;           // garrison ships still defending (PLANET only; includes the militia)
  // 2.0-dev.24 escort / militia (planet battles): ships that fight only this
  // battle and take their side's first loss. A lost escort is lastLossIdx
  // ESCORT_LOSS_IDX.
  int8_t battleEscortFaction = -1;  // whose ship escorts the attacker (-1 = none)
  uint8_t battleEscortAlive = 0;
  int8_t battleMilitiaFaction = -1;
  uint8_t battleMilitiaAlive = 0;   // still inside defRemaining
  uint8_t battleQuirk = 0;      // 3.1-dev.15 ship quirks left this battle: 1 atk shrug, 2 def shrug, 4 atk escape, 8 def escape, 16 atk first shot, 32 def first shot
  uint8_t tqN = 0; int8_t tq[6][4] = {};  // 3.1-dev.15 the table moment queue (moment, actor, planet, victim)
  uint32_t tqShownMs = 0;
  uint8_t battleAtkExtra = 0, battleDefExtra = 0;  // 3.1-dev.7: class extra ships (Warlord / Consul / Infiltrator's world); hit first
  uint8_t battleAtkMercs = 0, battleDefMercs = 0;  // 2.0-dev.26 hired ships still fighting (defender: PvP only)
  // Per-exchange actions (PVP uses both; PLANET uses only attacker).
  int atkAction = 0;              // 0 none, 1 fight, 2 withdraw
  int defAction = 0;
  int lastAtkRoll = 0;
  int lastDefRoll = 0;
  // v0.20.0 (battle animations): bumped once per resolved exchange, reset
  // to 0 when a battle starts. Every device animates when it sees this
  // change -- roll values alone can't signal "new exchange", since two
  // exchanges in a row can roll the same numbers.
  int battleExchange = 0;
  // Ship index (into atkAlive/defAlive) destroyed by the last exchange, or
  // -1 (tie, or a garrison ship lost -- those have no index). Which side
  // lost it follows from lastAtkRoll vs lastDefRoll.
  int lastLossIdx = -1;
  // Faction modifiers fixed at battle start (planet battles; 0 in PvP).
  // lastAtkRoll/lastDefRoll INCLUDE these; die face = roll - mod.
  int battleAtkMod = 0;
  int battleDefMod = 0;
  // v0.20.2: the last exchange was a retreat under fire -- 0 = no,
  // 1 = the attacker withdrew, 2 = the defender withdrew. Only the other
  // (pursuing) side could deal damage in it; the scene uses this so a
  // retreating side that "wins" the roll just escapes.
  int lastRetreatSide = 0;
  bool battleOver = false;
  char resultText[96] = "";

  // --- Takeover sessions, one per player slot ---
  // v0.19.1: was a single global session, so a second player beginning a
  // takeover silently replaced the first player's. Each slot now owns its
  // own; a slot has an open session iff takeoverPlanet[slot] >= 0.
  int takeoverPlanet[MAX_PLAYERS] = { -1, -1, -1, -1 };
  int takeoverMethod[MAX_PLAYERS] = {};  // committed method (TAKEOVER_NONE = 0)
  // v0.30.0: the negotiation conversation, once NEGOTIATE is committed. Read it
  // through snapshotNegotiation() rather than these fields.
  uint8_t negQ[MAX_PLAYERS][NEG_MAX_Q] = {};      // NEG_BANK index per question
  int8_t negDrop[MAX_PLAYERS][NEG_MAX_Q] = {};    // reply your contact took away (-1 = none)
  uint8_t negPerm[MAX_PLAYERS][NEG_MAX_Q] = {};   // the order the replies are shown in (0..5)
  uint8_t negMarks[MAX_PLAYERS][NEG_MAX_Q] = {};  // NEG_MARK_*
  uint8_t negCount[MAX_PLAYERS] = {};             // questions in this conversation
  uint8_t negStep[MAX_PLAYERS] = {};              // current question; == negCount: the favor stage
  uint8_t negResult[MAX_PLAYERS] = {};            // NEG_TALKING .. NEG_ENDED
  int8_t negFavor[MAX_PLAYERS] = { -1, -1, -1, -1 };  // the favor you took (NEG_FAVOR_BANK index; result card)
  uint8_t negOffer[MAX_PLAYERS][NEG_FAVORS] = {};     // v0.30.1: the favors drawn for the favor stage
  uint8_t negOfferN[MAX_PLAYERS] = {};
  int8_t negLine[MAX_PLAYERS] = { -1, -1, -1, -1 };   // v0.30.1: NEG_HOSTILE_LINES index (a hostile result)
  // v0.30.1: a planet won with a favor is worth no VP until the favor is kept.
  uint8_t planetVpPending[NUM_PLANETS] = {};
  // Pay/Negotiate outcome message for this slot's session. Kept separate
  // from the battle's resultText so one player's takeover message never
  // lands on another player's screen.
  char takeoverResult[MAX_PLAYERS][96] = {};

  // --- Notifications (e.g. "you are being attacked") ---
  // A simple per-slot one-line alert the UI can surface.
  char alert[MAX_PLAYERS][80] = {};

  // --- Faction Transmission queue ---
  // Set when a player's reputation with a faction crosses a tier boundary,
  // OR when a promised honor-system obligation comes due for a check-in.
  // Each player's own UI (AMOLED for the host, phone JS for phones) polls
  // its own slot's entry and shows the transmission screen/overlay, then
  // calls gameStateClearTransmission() (normal) or gameStateResolveObligation()
  // (check-in) to resolve it.
  // The four transmission* fields mirror the HEAD of txQueue (what the UI
  // shows now); the text comes from snapshotTransmissionText().
  bool transmissionPending[MAX_PLAYERS] = {};
  int transmissionFaction[MAX_PLAYERS] = { -1, -1, -1, -1 };
  // If true, this is an obligation check-in: the UI should show YES/NO
  // buttons instead of a simple dismiss, and transmissionObligationIdx says
  // which of that player's obligations[] slots it refers to.
  bool transmissionIsObligation[MAX_PLAYERS] = {};
  int transmissionObligationIdx[MAX_PLAYERS] = { -1, -1, -1, -1 };

  // v0.23.0: a real queue (a capture moves two factions at once -- the
  // second used to overwrite the first) holding line ids into the generated
  // transmission bank instead of 160-char text (smaller over the console link).
  TxMsg txQueue[MAX_PLAYERS][TX_QUEUE_LEN] = {};
  uint8_t txCount[MAX_PLAYERS] = {};
  uint8_t bigMomentsDone[MAX_PLAYERS] = {};  // bit per TxBig: each fires once per player per game
  bool firstBloodDone = false;               // the first battle capture of the game happened
  // v0.24.4: the latest PUBLIC big moment, for the table's alert (a new
  // publicSeq = a new one). Allied and oathbreaker stay private (user 2026-10-15).
  uint16_t publicSeq = 0;
  int8_t publicMoment = -1;  // TxBig
  int8_t publicActor = -1;   // the faction that did it (-1 = nobody: final round)
  int8_t publicVictim = -1;  // the faction it was done to
  int8_t publicPlanet = -1;
  // v0.25.2: who goes first on the map this game (picked at START; the table
  // shows a light spinning round the badges and landing on them).
  int8_t firstPlayer = -1;
  // v0.25.4: THE SHUFFLED GALAXY (user 2026-10-16: "eliminate advantages that
  // come from having memorized the board"). Rolled at every START: each
  // planet's faction (the 4 seats of power keep theirs; the other 8 go 2 per
  // faction), its Defense level (same High/Low mix per ring), its Resources
  // (6 High / 6 Low) and a market offset on its price. Before a game they
  // hold the classic table (PLANETS). Read them through planetFaction(),
  // planetResHigh(), planetDefHigh() and planetCharValue().
  int8_t planetFactionOf[NUM_PLANETS] = {};
  uint8_t planetResIsHigh[NUM_PLANETS] = {};
  uint8_t planetDefIsHigh[NUM_PLANETS] = {};
  uint8_t planetMarket[NUM_PLANETS] = {};
  // --- 2.0 ---
  uint8_t planetTrait[NUM_PLANETS] = {};   // TraitId, dealt at START
  uint8_t planetGood[NUM_PLANETS] = {};    // GoodId, dealt at START
  uint8_t marketStock[NUM_PLANETS] = {};   // left to sell this round (0 = closed or sold out)
  int8_t marketOpen[MARKETS_OPEN] = { -1, -1, -1, -1 };  // 2.0 world markets -- always closed since 3.1-dev.9 (kept for the 1.75")
  int8_t stallGood[4][STALL_ITEMS] = { { -1, -1, -1 }, { -1, -1, -1 }, { -1, -1, -1 }, { -1, -1, -1 } };  // 3.1-dev.9: per faction, -1 = closed
  int8_t embargoFaction = -1, embargoRound = -1;  // 3.1-dev.10: a faction shut its stall to everyone this round
  int8_t raceWorld = -1, raceFaction = -1, raceUntil = -1;  // a race: first to take raceWorld (-2 = any world of raceFaction's rival = DESPERATION)
  int8_t lastNews = -1;       // the last table-wide item (no repeats back to back)
  uint8_t strikesThisGame = 0, lastStandDone = 0;
  int8_t offWorld = -1, offFaction = -1, offRound = -1;  // 3.1-dev.11: a late offensive, warned a round ahead
  int8_t champion[4] = { -1, -1, -1, -1 };  // 3.1-dev.12: each faction's champion (slot)
  int8_t eraFaction = -1, eraBy = -1, eraFrom = -1, eraUntil = -1;  // the Era: whose, declared by, rounds
  int8_t lieRound = -1;          // (the Shadow Age lie -- gone in 3.1-dev.88; Deep Cover keeps its own story)
  int8_t markSlot = -1;          // 3.1-dev.88 the Dawn's Era: THE MARK, named by its champion
  uint8_t battleGetaway = 0;     // 3.1-dev.88 Clean getaway used this battle (bit 1 attacker, bit 2 defender)
  char lie[ERA_LIE_LEN + 4] = "";
  int8_t warBy = -1, warWorld = -1, warRound = -1;  // 3.1-dev.13 a declared war: the War Room opens in warRound's Resolution
  int8_t summitBy = -1, summitUntil = -1; int8_t summitWorld[SUMMIT_WORLDS] = { -1, -1, -1 }; uint8_t summitBroken = 0;
  uint8_t battleKeepers = 0;      // the next planet battle's defender gets peacekeepers
  // the last War Room fight, for the phones' playback: dice per exchange (atk << 4 | def)
  int8_t warLastBy = -1, warLastWorld = -1, warLastWon = -1; uint8_t warLastA = 0, warLastD = 0, warLastN = 0; uint8_t warLog[WAR_LOG_LEN] = {};
  uint8_t stallStock[4][STALL_ITEMS] = {};
  uint8_t planetUnbound[NUM_PLANETS] = {}; // EV_UNBOUND: droid-held, can't be bought
  int8_t eventId = EV_NONE;                // the galaxy event drawn this game
  int8_t eventRound = -1;                  // the round it struck
  // per battle (set at battle start, consumed during it)
  uint8_t atkRerolls = 0, defRerolls = 0;  // TR_ANCIENT / GOOD_BLESSING
  uint8_t atkFirstBonus = 0, defFirstBonus = 0;  // TR_TRAINING: +1 on the first exchange
  Deal deals[DEAL_SLOTS] = {};
  uint16_t dealSeq = 0;                    // bumps whenever any deal changes
  // EV_ASHEN
  int16_t ashenHp = 0, ashenMaxHp = 0;     // alive while ashenHp > 0
  uint8_t ashenDmg[MAX_PLAYERS] = {};      // damage dealt this game, per player
  uint16_t ashenSeq = 0;                   // bumps on every strike
  int8_t ashenLastSlot = -1;               // the latest strike: who, ships sent, damage, its answer
  uint8_t ashenLastShips = 0, ashenLastDmg = 0, ashenLastRoll = 0, ashenLastHits = 0;
  // 3.1-dev.43 THE FINAL HOUR: 0 = not yet, 1 = the gambits are dealt, 2 = the finale is playing, 3 = played
  int8_t galWhisper[MAX_PLAYERS] = { -1, -1, -1, -1 };  // 3.1-dev.52 THE GALLERY: who whispered against this talker's talk
  uint8_t galVouch[MAX_PLAYERS] = {}, galActed[MAX_PLAYERS] = {};  // who vouched / who has acted (bits), per talker
  int8_t chanceRecent[4] = { -1, -1, -1, -1 }; uint8_t chanceNext = 0;  // 3.1-dev.51: the last worlds a race / job / commotion named (the next is picked far from them)
  int8_t raceDoneRound = -1; uint8_t raceWinners = 0;  // 3.1-dev.49 #12: a race finished this round -- others finishing it the same round win too
  int8_t planetHeldSince[NUM_PLANETS] = {};             // 3.1-dev.49 #18: the round the current holder took it
  int8_t planetTakenRound[NUM_PLANETS] = {};            // 3.1-dev.76 DUST SETTLING: the round it changed hands in a Resolution (no second attack that round)
  int8_t patrolUntil[NUM_PLANETS] = {};                 // 3.1-dev.76 PATROL: hired guards stand here through this round
  uint8_t patrolShips[NUM_PLANETS] = {};
  uint32_t fortMask = 0;                                // 3.1-dev.76: fortified worlds (+DEF_FORT_BONUS to their limit)
  uint32_t treatyMask = 0;                              // 3.1-dev.101 TREATY WORLDS: won by a Consul's talk, still theirs (+1 CR a round)
  int8_t treatyUntil[NUM_PLANETS] = {};                 // 3.1-dev.101: no bribe / buyout of it through this round
  int8_t inkRound[NUM_PLANETS] = {};                    // 3.1-dev.102 THE INK IS STILL WET: won by talk / bribe in this round ...
  int8_t inkBy[NUM_PLANETS] = {};                       // ... by this slot + 1 ...
  uint8_t inkHow[NUM_PLANETS] = {};                     // ... 1 talk, 2 bribe ...
  uint8_t inkCr[NUM_PLANETS] = {};                      // ... the bribe (back if it falls through)
  uint32_t inkHit = 0;                                  // ... its one attack has been tried this round
  uint8_t negHead[MAX_PLAYERS] = {};                    // 3.1-dev.102: this talk began with the head start
  int8_t buyBy[NUM_PLANETS] = {};                       // 3.1-dev.79 BUYOUT: the buyer + 1 (0 = no offer), their Credits held until the holder answers
  uint8_t buyCr[NUM_PLANETS] = {};
  int8_t buyRound[NUM_PLANETS] = {};
  int8_t upriseFrom[NUM_PLANETS] = {};                  // 3.1-dev.79 UPRISING: the old holder + 1, who may strike back this round
  uint8_t negUprising[MAX_PLAYERS] = {};                // this talk is an uprising (hard questions only, no misses)
  BattleRec battleRecs[BATTLE_RECS];                    // 3.1-dev.80 newest at battleRecHead - 1
  BattleRec battleCur;                                  // the battle being fought
  uint8_t battleRecHead = 0, battleCurPushed = 0;
  uint8_t battlePatrolAlive = 0;                        // 3.1-dev.76: the patrol fighting in this battle (falls after the militia, before the garrison)
  uint8_t fhStage = 0;
  uint16_t fhLogN = 0;
  uint8_t fhLog[FH_LOG_LEN] = {};  // the reveal + every world's fight, for the phones' playback (format: game_state.cpp fhFinaleLocked)
};

// ---- 2.0 rules (game_state.cpp) and content (sf_content.cpp) ----
int snapshotRevealCost(const GameState &gs, int slot, int planetIdx, int charIdx);  // 0 = your free reveal (TR_SPYNET)
// 2.0-dev.27 THE INFORMANT (user 2026-11-04): a reveal during Resolution -- from the
// battle forecast -- costs INFORMANT_COST_MULT times the Prepare price (no free one).
#define INFORMANT_COST_MULT 3
// 2.0-dev.27 forecast v2: the garrison sizes `slot` can't rule out at planet p, from
// what they know (ring, home world, Defense if revealed, a possible militia). Never
// reads the hidden count -- the forecast turns it into whole-battle odds.
void snapshotGarrisonRange(const GameState &gs, int slot, int planetIdx, int *lo, int *hi);
int snapshotDefendersNow(const GameState &gs, int p);                 // 3.1-dev.101: garrison + a live patrol
int snapshotDefSeenState(const GameState &gs, int slot, int p);       // 3.1-dev.101: -1 never looked, 0 same, 1 more, 2 fewer
bool snapshotIsLastPlace(const GameState &gs, int slot);              // 3.1-dev.101: never the Mark
uint32_t snapshotNeighbors(int p);                                    // 3.1-dev.102: worlds one step away on the board (REDIRECT)
bool snapshotInkWet(const GameState &gs, int p);                      // 3.1-dev.102
const char *gameStateGfhOffer(int slot, int to, uint32_t mask, int cr);  // 3.1-dev.102 GUNS FOR HIRE
bool gameStateGfhAnswer(int slot, bool accept);
const char *gameStateStandBy(int slot, int fleet);                    // 3.1-dev.102
// UI 3.0 step 4 (2.0-dev.39) THE HOLONET RUMOR: one TRUE thing some player is up to, never who --
// built from the state itself (no saved log), the same for everyone for the whole round.
// 2.0-dev.41: the Holonet has a VOICE (sly gossip) and points at worlds: *planet = the world it names, or -1
// (the owner's phone says "that's YOUR world"; the table doesn't).
void snapshotRumor(const GameState &gs, char *out, size_t n, int *planet = nullptr);
int snapshotMarketPrice(const GameState &gs, int slot, int planetIdx);
extern const uint8_t FACTION_GOODS[4][3];                                // 3.1-dev.9: what each faction's stall sells
bool snapshotStallShut(const GameState &gs, int slot, int faction);     // 3.1-dev.9: tier 2 or below
int snapshotStallPrice(const GameState &gs, int slot, int faction, int k);
bool snapshotStallCanBuy(const GameState &gs, int slot, int faction, int k);  // any target
bool gameStateStallBuy(int slot, int faction, int k, int target);
bool gameStateAnswerJob(int slot, bool take);                          // 3.1-dev.10: a job offer -- take it or pass
bool gameStateDeclareEra(int slot, int faction);                       // 3.1-dev.12: spend your decree
bool gameStateSetLie(int slot, const char *text);                      // 3.1-dev.12: the Shadow Age champion's lie
bool gameStateDeclareWar(int slot, int planet);                        // 3.1-dev.13 WARLORD (3.1-dev.88: WARPATH, declared in Prepare)
const char *gameStateBcOffer(int slot, int to, int kind, int arg, int cr);  // 3.1-dev.98 THE BACK CHANNEL
bool gameStateBcAnswer(int slot, bool accept);
const char *gameStateBuyFile(int slot, int target);                    // 3.1-dev.88 the Dawn's Era: read a rival's file (6 CR)
bool gameStateNameMark(int slot, int target);                          // 3.1-dev.88 the Dawn's champion names the Mark
const char *gameStateUseCargo(int slot, int k, int arg);               // 3.1-dev.88 use a lot from your cargo hold
bool snapshotBmOpen(const GameState &gs);                              // 3.1-dev.88 the black market is open (Final Hour or the Dawn's Era)
int snapshotPatrolCost(const GameState &gs);                           // 3.1-dev.88 (Imperial Order: half)
bool gameStateWarRoom(int slot, const int8_t *pull, bool march);       // pull[NUM_PLANETS] ships from your worlds; march = through a Summit
bool gameStateWarPullBack(int slot);                                   // a Summit on your war world: take the declaration back
bool gameStateCallSummit(int slot, const int *worlds, int n);          // CONSUL
bool gameStateBreak(int slot, int planet);                             // confirmed ATTACK ANYWAY (ceasefire / cover) for this round
bool gameStateDeepCover(int slot, int faction);                        // INFILTRATOR
bool gameStateSetStory(int slot, const char *text);                    // ... the planted story
bool gameStateHeist(int slot, int faction, bool bold);                 // SCOUNDREL
bool gameStateHeistPush(int slot, bool push);                          // 3.1-dev.20: roll again, or take the money
bool gameStatePayOffGrudge(int slot);                                  // 3.1-dev.20: GRUDGE_PAYOFF_CR stops the strike
const char *gameStateGrudgeDeed(int slot, int kind, int world);       // 3.1-dev.42: make amends (nullptr = done, else why not)
const char *snapshotDeedRefusal(const GameState &gs, int slot, int kind, int world);  // world < 0: "is any world OK"
int snapshotOfferedDeed(const GameState &gs, int slot);              // the deed the grudge asks for now (-1 none doable)
bool snapshotSummitCovers(const GameState &gs, int planet);
int snapshotStallBought(const GameState &gs, int slot, int faction);  // 3.1-dev.114 buys at that stall this round
int snapshotStallLimit(const GameState &gs);                          // 1 (Spice Age: 2)
int snapshotEra(const GameState &gs);                                  // the Era in force (a faction) or -1
bool snapshotEraSpared(const GameState &gs, int slot);                 // tier 5 with the Era's faction
int snapshotShipCost(const GameState &gs, int slot, bool offBoard);    // 2 CR (traits, the Spice Age)
bool snapshotPayShut(const GameState &gs, int slot);                   // the Rebellion Rises: nobody buys worlds
bool snapshotVpHidden(const GameState &gs, int slot);                  // the Shadow Age: others' VP hidden from you
void snapshotNewsText(const GameState &gs, char *rim, size_t rimSize, char *big, size_t bigSize, char *small, size_t smallSize);  // 3.1-dev.23: the table's three lines
const char *snapshotCallName(const GameState &gs, int faction);  // 3.1-dev.23: the player in that seat by character (KESS), else the faction
//  // the table's Galaxy News line      // target: a world (SURVEY) or a faction (AUDIENCE), else -1
int snapshotSafeWorld(const GameState &gs, int slot);                  // 3.1-dev.8: Rebels tier 5 -- your safe world, or -1
bool snapshotMarketIsOpen(const GameState &gs, int planetIdx);
bool snapshotCanBuy(const GameState &gs, int slot, int planetIdx);  // open, in stock, Prepare, affordable, usable
bool snapshotCanSendDiplomat(const GameState &gs, int slot, int planetIdx);
int snapshotQuestProgress(const GameState &gs, int slot, int *goal);
void gameStateCreditPvpWin(int slot);
int gameStateFhShowStep();
int gameStateFhSteps();             // 3.1-dev.123 how many worlds the finale fights over          // 3.1-dev.117 the finale step every screen shows (-1 = none; 0 = the reveal; n = world n; > worlds = the summary)
uint8_t gameStateFhSkipMask();      // who asked to skip this step
uint32_t gameStateFhStepLeftMs();   // ms left in this step
bool gameStateFhSkip(int slot);     // ask (or stop asking) to skip this step  // 3.1-dev.113: keeps that player's open "win a battle / beat a fleet" promises  // progress toward your quest (goal filled in)
bool snapshotQuestDone(const GameState &gs, int slot);                // judged as if the game ended now
int snapshotHolderTrait(const GameState &gs, int slot, int trait);    // how many worlds with this trait you hold
bool gameStateMarketBuy(int slot, int planetIdx);
bool gameStateSendDiplomat(int slot, int planetIdx);
// Deals: a new offer (dealIdx -1, `to` = who it goes to) or a counter to an
// open one (dealIdx = its slot; `to` ignored). line = dealLine() index, or -1
// with msg (phones). Returns the deal slot, or -1 (see snapshotDealSendRefusal).
int gameStateDealSend(int slot, int dealIdx, int to, int give, int ask, int world, int askCr, int line, const char *msg);
bool gameStateDealAnswer(int slot, int dealIdx, bool accept);  // DECLINE by the sender withdraws it
// Why that send would be refused right now (nullptr = it would go through).
const char *snapshotDealSendRefusal(const GameState &gs, int slot, int dealIdx, int to, int give, int ask, int world, int askCr);
// The deal stopping `slot` from taking `planetIdx` by `method` (TAKEOVER_*):
// -1 = none; why (optional) gets "You gave your word to ...".
int snapshotDealBlock(const GameState &gs, int slot, int planetIdx, int method, char *why, size_t whySize);
int snapshotDealsWaiting(const GameState &gs, int slot);  // offers waiting on `slot`'s answer
// The Ashen Fleet: alive? / why `slot` can't strike it with these fleets right
// now (fleetMask bit per fleet; nullptr = it would go through) / strike it
// (returns the damage done, -1 = refused; the result is in gs.ashenLast*).
bool snapshotAshenAlive(const GameState &gs);
const char *snapshotAshenStrikeRefusal(const GameState &gs, int slot, int fleetMask);
int gameStateAshenStrike(int slot, int fleetMask);
const char *ashenTaunt(int i);  // its admiral, for the table (cycles)
int ashenTauntCount();
// One line for what the promise is, worded for `viewer` ("They don't attack your worlds until round 5").
void snapshotDealAskText(const GameState &gs, int dealIdx, int viewer, char *out, size_t outSize);
const char *dealAskLabel(int ask);         // the compose list ("Don't attack my worlds")
int dealLineCount();
const char *dealLine(int line);            // canned lines: 3 per faction, then the counter lines
int dealLineFor(int faction, int k);       // the k-th line a faction can pick (k < DEAL_PICK_LINES)
#define DEAL_PICK_LINES 6                  // your 3 + "Too low." / "Add a promise." / "Final offer."
const char *traitName(int traitId, int planetIdx);  // this world's name for the effect
const char *traitEffect(int traitId);               // one line: what holding it gives you
const char *goodName(int goodId);
const char *goodEffect(int goodId);
int goodBasePrice(int goodId);
const char *eventName(int eventId);
const char *eventText(int eventId);
const char *questName(int faction);
const char *questText(int faction);
const char *strengthName(int faction);
const char *strengthText(int faction);
void snapshotPublicText(const GameState &gs, char *head, size_t headSize, char *sub, size_t subSize);  // the relay line

// ---- Lifecycle / lobby ----
// Writes the HEAD transmission's text for `slot` (bank line with {planet}
// filled in, or the obligation's promise for a check-in); "" if none.
void snapshotTransmissionText(const GameState &gs, int slot, char *out, size_t outSize);
void gameStateInit();
GameState gameStateRead();
// 2.0-dev.15: copy the state into a buffer you own -- no temporary on the
// stack (GameState is ~5 KB; two of them overflowed the web task, dev.13-14).
void gameStateReadInto(GameState &out);

// ---- Event log (3.1-dev.3) ----
// Everything worth telling someone is written ONCE as a small event; the
// phone's Feed (Inbox + History), its notifications and the end-of-game story
// all read it. Public events are for everyone; each player also has a private
// ring. The log lives beside GameState in PSRAM (not inside it, so the many
// GameState copies don't grow) and is saved with the game.
enum EventKind : uint8_t {
  LOG_NONE = 0,
  LOG_ROUND,     // pub: a round began (round field)
  LOG_TAKE,      // pub: who (slot) took planet; other = old owner slot or -1; num = TakeHow
  LOG_RETURNED,  // pub: planet went back to its faction; who = the slot that lost it
  LOG_MOMENT,    // pub: a big public moment; who = actor faction, other = victim faction, num = publicMoment
  LOG_LOST,      // mine: you lost planet; who = the slot that took it (-1 = its faction); num = TakeHow
  LOG_NOTE,      // mine: a personal notice; num = index into EventLog.notes[slot]
  LOG_PROMISE,   // mine: a promise resolved; who = faction, num = 1 kept / 0 broken
  LOG_STAMP,     // mine: a class strength / weakness fired; num = StampId, planet = where
  LOG_QUEST,     // pub: who (slot) finished their class quest (+QUEST_VP)
  LOG_GIFT,      // mine (3.1-dev.8): who = faction; num = 0 tier-4 gift, 1 tier-5 gift on, 2 tier-5 gift lost, 3 the spy reports; planet / other = the worlds (or the spied slot)
  LOG_NEWS,      // pub (3.1-dev.10): Galaxy News; num = NewsId, who = faction, planet = world, other = a slot / faction / result
  LOG_PNEWS,     // mine (3.1-dev.10): your personal news; num = PNewsId, who = faction, planet = world, other = detail
};
enum StampId : int8_t { STAMP_WARLORD = 0, STAMP_FEARED, STAMP_SILVER, STAMP_SOFT, STAMP_FREELOOK, STAMP_THIN, STAMP_BLACKMARKET, STAMP_DISTRUST, STAMP_LINE, STAMP_SAFE,
                       STAMP_RANKS, STAMP_ESCAPED, STAMP_SHRUGGED, STAMP_FIRSTSHOT,  // 3.1-dev.15 ship quirks
                       STAMP_REROLL, STAMP_MEDICAL, STAMP_COURT, STAMP_TRAINING,  // 3.1-dev.19 traits you can see
                       STAMP_SHAKEN,  // 3.1-dev.76
                       STAMP_SABOTEUR, STAMP_TURNCOAT, STAMP_SECOND,  // 3.1-dev.77 black-market lots firing
                       STAMP_BOUNTY,  // 3.1-dev.78 you took a Scoundrel's world
                       STAMP_MARK, STAMP_BLADE, STAMP_DECOY, STAMP_GETAWAY, STAMP_WARPATH,
                       STAMP_SPOILS, STAMP_RALLY, STAMP_ALERT, STAMP_TREATY,
                       STAMP_DUGIN, STAMP_STANDBY, STAMP_HIRED };  // 3.1-dev.102  // 3.1-dev.101 (STAMP_SOFT = the Consul's NO STOMACH FOR WAR now)  // 3.1-dev.93  // 3.1-dev.88
enum TakeHow : int8_t { TAKE_OTHER = 0, TAKE_FIGHT = 1, TAKE_TALK = 2, TAKE_PAY = 3 };
struct GameEvent {
  uint16_t seq = 0;   // 0 = empty slot
  uint8_t kind = LOG_NONE;
  uint8_t round = 0;
  int8_t who = -1, planet = -1, other = -1, num = 0;
};
#define EV_PUB_N 64
#define EV_MINE_N 16
#define EV_NOTE_N 4
#define EV_NOTE_LEN 72
struct EventLog {
  uint16_t epoch = 0;  // bumps every new game, so a phone knows its cache is stale
  uint16_t pubSeq = 0;
  GameEvent pub[EV_PUB_N];
  uint16_t mineSeq[MAX_PLAYERS] = {};
  GameEvent mine[MAX_PLAYERS][EV_MINE_N];
  uint8_t noteNext[MAX_PLAYERS] = {};
  char notes[MAX_PLAYERS][EV_NOTE_N][EV_NOTE_LEN] = {};
};
// Copies the whole log (about 2.2 KB) into a buffer you own (keep it off the stack).
void gameStateEventsInto(EventLog &out);

// A player joins into a SPECIFIC requested slot, picking their own spot in
// the lobby rather than being auto-assigned the first free one. If this
// clientId already has an active slot (reconnecting), that original slot is
// returned unconditionally regardless of requestedSlot -- reconnects always
// resume where they were rather than re-picking. Returns the assigned slot,
// or -1 if requestedSlot is invalid, already taken by someone else, or the
// game has already left the Lobby.
int gameStateJoinSlot(const char *clientId, const char *name, int requestedSlot);

// True if `slot` is a valid, currently-unclaimed slot a NEW player could
// join into (for rendering the slot-picker UI).
bool gameStateSlotOpen(int slot);

// Mark a slot's live-connection status (phones; the AMOLED player is always
// connected once joined).
void gameStateSetConnected(int slot, bool connected);
// 2.0-dev.13: a request or link message came from this player's device (the
// table only). Ends a "went quiet" AWAY.
void gameStatePresent(int slot);
// STEP AWAY (away = true) / COME BACK (false).
void gameStateStepAway(int slot, bool away);
// Closes the "while you were away" card.
void gameStateWelcomeAck(int slot);

// ---- Faction selection (Lobby only) ----
// Claims a faction for this player. Fails (returns false) if another ACTIVE
// player already holds it. Re-picking your own currently-held faction is
// always allowed. Sets real starting reputation the moment a faction is
// claimed: Allied with your own faction, Hostile with its rival, Neutral
// with the other two -- overwritten cleanly if you change your pick before
// the game actually starts.
bool gameStateChooseFaction(int slot, int factionIdx);

// True if `factionIdx` is already claimed by a DIFFERENT active player (for
// rendering faction availability in the picker UI).
bool gameStateFactionTaken(int factionIdx, int excludeSlot);

// v0.21.20: picks your commander (id = faction*3 + i, 0..2 within your
// faction; see commander_art.h). Lobby only, after choosing a faction; fails
// for a commander of another faction. Changing faction clears it.
bool gameStateChooseCommander(int slot, int commanderId);

// Host starts the game (lobby -> playing). Allowed with any number of
// players, but only once every active player has chosen a faction and (v0.21.20)
// a commander.
void gameStateStartGame();
// v0.27.3: after GAME OVER, back to a FRESH lobby -- everyone leaves their seat
// (user 2026-10-16: "there needs to be a way to restart the game at the end").
// Keeps the game length. Does nothing unless the game is over.
void gameStateNewGame();
// v0.30.3: END GAME mid-game (any seated player, after a confirm) -- a fresh
// Lobby for everyone, like NEW GAME, and the table's save is wiped. Needed
// because the table now always resumes a saved game when it boots.
void gameStateEndGame();
// v0.30.3 SAVE STATES (user 2026-10-28: "crashing is happening a bunch"): the
// TABLE writes the game to flash (LittleFS on the free "spiffs" partition)
// whenever it changed, at most every SAVE_EVERY_MS, and only while no battle
// is running -- a crash mid-battle resumes from just before it. Call
// gameStateSaveTick() from loop(); gameStateRestoreSaved() once at boot,
// before the network/link start. Restored players show as disconnected until
// their phone / console rejoins (same id, same seat).
#define SAVE_EVERY_MS 3000
void gameStateSaveTick();
// 2.0-dev.17: the table runs gameStateSaveTick() on its own low-priority task
// (a save takes 300-700 ms; inside loop() it held up every phone's /state).
// Call from loop(); starts the task once.
void gameStateSaveTaskStart();
#define SAVE_TASK_STACK 4096
bool gameStateRestoreSaved();   // true = a saved game is running again
// 2.0-dev.20: how many ships each of this player's fleets can hold (Empire 7, else 6).
int snapshotFleetCapacity(const GameState &gs, int slot);
// v0.28.3: housekeeping on the HOST (table, or a 1.75" hosting alone) -- call often
// from loop(). Closes a finished battle nobody closed (playtest 2026-10-19: the
// attacker's phone never tapped CONTINUE, and "a battle is already in progress"
// blocked every fight after it).
void gameStateTick();
// v0.29.0: just `slot`'s four reputation scores (cheap -- no whole-state copy).
void gameStateReputationScores(int slot, int out[NUM_FACTIONS]);
#define BATTLE_AUTO_CLOSE_MS 20000  // a finished battle closes itself this long after its last exchange

// Selectable game length, set by any player while still in the lobby (has
// no effect once the game has started). Accepts 6, 8, or 10; anything else
// is ignored.
void gameStateSetRoundLimit(int rounds);
void gameStateSetMarkets(bool on);  // 2.0-dev.32: Lobby only
void gameStateSetEvents(bool on);   // 2.0-dev.33: Lobby only

// ---- Victory Points ----
// Fully public -- unlike reputation, any caller can query any player's VP.
int gameStatePlayerVP(int slot);
// Returns the winning slot once STAGE_GAME_OVER (highest VP; ties broken by
// planets controlled, then Credits). Returns -1 if the game isn't over yet,
// or -2 for a genuine draw (every tiebreaker also equal).
int gameStateWinnerSlot();

// ---- Intel / economy ----
bool gameStateRevealCharacteristic(int slot, int planetIdx, int charIdx);

// Owner invests 1 Credit-priced garrison ship into a planet they control.
// Returns false if not owner, not enough credits, or at MAX_PLANET_DEFENSE.
bool gameStateInvestDefense(int slot, int planetIdx);
int snapshotDefenseLimit(const GameState &gs, int p);   // 3.1-dev.76: the most defenders world p can hold
bool gameStatePatrol(int slot, int planetIdx);         // 3.1-dev.76: hire a patrol for one of your worlds (Prepare)

// Commission one more ship into one of your own fleets (FLEET_INVEST_COST
// Credits each). Returns false if not enough credits or already at
// FLEET_SHIPS capacity. Persists in game state -- ship counts and losses
// carry across battles and rounds. A fleet with no living ships (never
// founded, or fully destroyed -- it has left the board) can only be
// (re)built by a player who owns a planet, where it respawns; rebuilding a
// destroyed fleet starts it fresh (v0.20.2).
bool gameStateInvestFleet(int slot, int fleetIdx);

// ---- Phase ----
// Marks a slot ready. Once ALL active players are locked in, the phase
// advances (Invest -> Movement -> Resolution -> Invest); looping to Invest
// starts a new round. Lock-in flags reset on advance.
void gameStateLockIn(int slot);

// ---- Takeover ----
// Begin a takeover session for a planet. Only during Resolution.
void gameStateBeginTakeover(int slot, int planetIdx);
void gameStateCancelTakeover(int slot);
// Commit to an approach. For FIGHT this starts the battle; for PAY it resolves
// immediately if affordable; for NEGOTIATE it rolls two offers to choose from.
void gameStateCommitTakeover(int slot, int method);
// FIGHT with a specific fleet (the battle briefing's picker). Same as
// gameStateCommitTakeover(slot, TAKEOVER_FIGHT), which uses the first fleet
// with ships. Refused (reason in takeoverResult) if that fleet is empty or
// a battle is already running.
// v0.28.0: fleetMask = bit per fleet sent in (two or more = a combined
// assault: ships pooled, every one spent, no bonus); -1 = the first ready fleet.
void gameStateCommitFight(int slot, int fleetMask);
// v0.30.0 the negotiation conversation. Answer the current question with a
// reply index (0..2, the bank's own order -- NegView.replies says which to
// show); in the favor stage, 0..2 = that favor (only 0 when they name one) or
// NEG_REFUSE. Close dismisses the result card (ends the session).
void gameStateNegotiationAnswer(int slot, int choice);
void gameStateNegotiationClose(int slot);
// v0.30.1: a HOSTILE result's FIGHT -- closes the conversation and reopens the
// planet to a fight this round (the one allowed switch of approach).
void gameStateNegotiationFight(int slot);
// Everything a screen needs to draw `slot`'s conversation (pure).
struct NegView {
  bool open = false;          // slot has a conversation (talking or its result)
  int planet = -1;
  int faction = -1;           // who's really talking -- show it only when `known`
  bool known = false;         // Affiliation revealed; else an UNKNOWN AMBASSADOR
  int count = 0, step = 0, result = NEG_TALKING;
  uint8_t marks[NEG_MAX_Q] = {};
  int question = -1;          // NEG_BANK index while a question is up, else -1
  bool tipped = false;        // your contact took one reply away
  int replies[3] = { -1, -1, -1 }, nReplies = 0;  // bank reply indices, in display order
  bool favorStage = false;    // all questions asked, not convinced: what will you do?
  bool hard = false;          // two or more misses: one favor, take it or leave it
  bool late = false;          // v0.30.1: the last rounds -- the late favor bank
  int nFavors = 0;            // favors offered: choice i = favors[i]
  int favors[NEG_FAVORS] = { -1, -1, -1 };  // NEG_FAVOR_BANK indices
  int lateCredits = 0;        // what a Credits favor costs you here
  int favorTaken = -1;        // NEG_FAVOR_BANK index, on a NEG_WON_FAVOR result
  int losses = 0;             // conversations lost at this planet (incl. this one)
  int hostileLine = -1;       // NEG_HOSTILE_LINES index on a NEG_HOSTILE result
  int roundsLeft = 0;         // rounds after this one before the war ends
};
NegView snapshotNegotiation(const GameState &gs, int slot);
// v0.30.1: what a planet is worth while `slot` holds it (2.0: a home world's
// worth depends on who holds it), and what `slot` would pay for a late Credits
// favor on it.
int planetVP(const GameState &gs, int planetIdx, int slot);
int planetVPNow(int planetIdx, int slot);  // same, from the live state
int snapshotLateFavorPrice(const GameState &gs, int slot, int planetIdx);
// A favor's text with {planet} and {cr} filled in for `slot`.
void snapshotFavorText(const GameState &gs, int slot, int favor, int planetIdx, char *out, size_t outSize);
// Why `slot` can't negotiate for `planetIdx` right now, or nullptr.
const char *snapshotNegotiationRefusal(const GameState &gs, int slot, int planetIdx);
// "Hold Kessel for 3 rounds..." -- a favor's text with the planet filled in.
void negFillPlanet(const char *src, int planetIdx, char *out, size_t outSize);

// Computed asking price for PAY, from resources + defense + this player's
// reputation tier with the planet's faction (pricier when Hostile/Wary,
// cheaper when Favorable/Allied).
int gameStatePayPrice(int slot, int planetIdx);

// ---- Battle ----
// Per-exchange action: 1 = fight, 2 = withdraw.
// PLANET battles: only the attacker acts; the garrison always fights.
// PVP battles: BOTH the attacker and defender submit an action; the exchange
//   resolves once both are in. Either withdrawing ends the battle.
void gameStateBattleAction(int slot, int action);
void gameStateEndTakeoverAfterBattle(int slot);  // cleanup once battle is over

// ---- PvP ----
// Number of ships still alive in a given player's fleet.
int gameStateFleetStrength(int slot, int fleetIdx);
// Launch a PvP attack: attackerSlot's attacker fleet vs defenderSlot's fleet.
// Only during Resolution, only if no battle is currently active, and only
// against a different, active player. Alerts the defender. Returns nullptr
// on success, else a player-facing reason (e.g. the attacking fleet already
// fought this round) -- a static string, safe to keep.
const char *gameStateStartPvpBattle(int attackerSlot, int attackerFleetMask, int defenderSlot, int defenderFleet);  // v0.28.0: a mask
// v0.28.0: the PvP defender brings another fleet (same space -- honor system)
// before the first exchange, when the attacker came with two or more; the
// defender may match the attacker's fleet count. Defending never spends a fleet.
const char *gameStateReinforce(int slot, int fleetIdx);
// 2.0-dev.28: the PvP defender brings their hired ships, before the first exchange.
const char *gameStateCallMercs(int slot);
bool snapshotCanCallMercs(const GameState &gs, int slot);
// Pure: fleets `slot` could still add as a PvP defender (bit per fleet).
int snapshotReinforceMask(const GameState &gs, int slot);
// Clean up a finished PvP battle (writes survivors back to both players).
void gameStateEndPvpBattle();

// ---- Notifications ----
void gameStateSetAlert(int slot, const char *msg);
void gameStateClearAlert(int slot);

// ---- Reputation / Faction Transmission ----
// Tier a player currently holds with a faction (REP_HOSTILE..REP_ALLIED).
// Never expose the raw score to the UI -- tier only.
int gameStateReputationTier(int slot, int factionIdx);
// Garrison tier (GARRISON_LOW/MEDIUM/HIGH) for a planet's CURRENT live
// garrison ship count. Always safe to call/display -- the tier itself is
// never hidden, only the exact number.
int gameStateGarrisonTier(int planetIdx);
// True once this player has lost a Fight at this planet -- unlocks showing
// the real garrison ship count instead of just the tier.
bool gameStateHasLostAt(int slot, int planetIdx);
// Clears a pending transmission after the UI has shown it. For an
// obligation check-in, use gameStateResolveObligation() instead -- this
// alone won't apply the reputation consequence.
void gameStateClearTransmission(int slot);
// Answers a pending obligation check-in: true = kept your word (+reputation),
// false = broke it (-reputation, a real trust penalty). Also clears the
// pending transmission.
void gameStateResolveObligation(int slot, bool fulfilled);

// ---- Snapshot queries (pure: never take the mutex) ----
// Same rules as the gameState* getters above, but computed from a GameState
// COPY you already hold (from gameStateRead()). Use these when building
// anything from one snapshot -- e.g. /state JSON -- so every value in it
// describes the same instant, instead of re-locking and reading live state
// ~30 times per response. game_state.cpp's locked code also calls them on
// `state` directly, so each rule has exactly one implementation.
int snapshotGarrisonTier(const GameState &gs, int planetIdx);
int snapshotReputationTier(const GameState &gs, int slot, int factionIdx);
int snapshotPayPrice(const GameState &gs, int slot, int planetIdx);
// Credits this player earns at the next round start (INCOME_* above).
int snapshotIncome(const GameState &gs, int slot);
int snapshotWinnerSlot(const GameState &gs);  // -1 not over, -2 draw
// Faction roll modifiers `slot` would get attacking `planetIdx` right now
// (what the battle briefing shows, and what the battle then locks in).
BattleMods snapshotPlanetBattleMods(const GameState &gs, int slot, int planetIdx);


// Faction index for a planet's Affiliation characteristic (0=Rebel Alliance,
// 1=Galactic Empire, 2=Pyke Syndicate, 3=Crimson Dawn). Useful for
// reputation lookups once a planet's Affiliation has been revealed.
int gameStateFactionForPlanet(int planetIdx);

// ---- Console link (v0.22.1; see console_link.h) ----------------------------
// A command console that joined a table console keeps an exact MIRROR of the
// table's GameState: reads work as always, and the functions that CHANGE the
// game are forwarded to the table instead of running locally. Each forwarded
// function has an id here; gameStateDispatch() runs it on the table.
enum RemoteFn : uint8_t {
  RF_JOIN_SLOT = 1, RF_CHOOSE_FACTION, RF_CHOOSE_COMMANDER, RF_SET_ROUND_LIMIT, RF_START_GAME,
  RF_LOCK_IN, RF_REVEAL, RF_INVEST_DEFENSE, RF_INVEST_FLEET, RF_BEGIN_TAKEOVER, RF_CANCEL_TAKEOVER,
  RF_COMMIT_TAKEOVER, RF_COMMIT_FIGHT, RF_CHOOSE_OFFER, RF_PAY_PRICE, RF_BATTLE_ACTION,
  RF_END_TAKEOVER_AFTER_BATTLE, RF_START_PVP, RF_END_PVP, RF_CLEAR_TRANSMISSION, RF_RESOLVE_OBLIGATION,
  RF_NEW_GAME,  // v0.27.3
  RF_REINFORCE,  // v0.28.0
  RF_NEG_CLOSE,  // v0.30.0 (RF_CHOOSE_OFFER now answers the conversation)
  RF_NEG_FIGHT,  // v0.30.1
  RF_END_GAME,   // v0.30.3
  RF_MARKET_BUY,     // 2.0
  RF_SEND_DIPLOMAT,  // 2.0
  RF_DEAL_SEND,      // 2.0: args slot, dealIdx, to, give, ask | world << 8 | askCr << 16, line
  RF_DEAL_ANSWER,    // 2.0
  RF_ASHEN_STRIKE,   // 2.0
  RF_STEP_AWAY,      // 2.0-dev.13
  RF_WELCOME_ACK,    // 2.0-dev.13
  RF_CALL_MERCS,     // 2.0-dev.28
  RF_SET_MARKETS,    // 2.0-dev.32
  RF_SET_EVENTS,     // 2.0-dev.33
  RF_STALL_BUY,      // 3.1-dev.9
  RF_ANSWER_JOB,     // 3.1-dev.10
  RF_DECLARE_ERA,    // 3.1-dev.12
  RF_SET_LIE,        // 3.1-dev.12
  RF_MOVE,           // 3.1-dev.13 (the 1.75" doesn't use it yet)
};
struct RemoteResult { int32_t value; char text[64]; };
// Returns false if the table never answered.
typedef bool (*GameRemoteFn)(uint8_t fn, const int32_t *args, int nargs, const char *str, RemoteResult *out);
void gameStateSetRemote(GameRemoteFn fn);                  // non-null: this console mirrors a table
RemoteResult gameStateDispatch(uint8_t fn, const int32_t *args, const char *str);  // table: run a forwarded call
void gameStateApplySnapshot(const GameState &gs);           // command console: replace the mirror
bool gameStateSellTwice(int slot, bool sell);               // 3.1-dev.48 answer the offer
int snapshotPayBackCost(const GameState &gs, int slot, int f);  // 3.1-dev.48
bool gameStatePayBack(int slot, int f);
bool gameStateSwapTitle(int slot);
const char *gameStateGallery(int slot, int talker, bool whisper);  // 3.1-dev.52 nullptr = done, else why not                          // 3.1-dev.49 #13 take the second title (give up the one you hold)                     // 3.1-dev.48 caught: pay one side back
// 3.1-dev.43 THE FINAL HOUR (phones only; the 1.75" has no screens for it -- AMOLED_TODO.md)
bool snapshotFhFinal(const GameState &gs);                  // the last round
bool snapshotFhReach(const GameState &gs, int slot, int p); // a strike on p needs no jump pass
int snapshotFhDebt(const GameState &gs, int slot, int f, int *cost);  // ships that faction would send (cost = CR per ship, 0 free); 0 = none
int snapshotFhShips(const GameState &gs, int slot, int k);  // live ships in fleet k
bool gameStateFhPick(int slot, int k);                      // keep drawn gambit k
bool gameStateFhBuy(int slot, int k);                       // buy black-market offer k
const char *gameStateBuyLot(int slot, int k, int world);   // 3.1-dev.77: buy black-market lot k (world for troops / fortifications; the rival's slot for a Slicer)
bool gameStatePickFile(int slot, int target);              // 3.1-dev.78 the Infiltrator opens one rival's file, once a round
const char *gameStateBlackmail(int slot, int target);      // 3.1-dev.78 the Crimson Dawn's Family gift, once a game
bool gameStateBlackmailAnswer(int slot, bool pay);
bool gameStateBuyoutAnswer(int slot, int planetIdx, bool match);  // 3.1-dev.79 the holder sells (half the price) or matches it (keeps the world)         // 3.1-dev.78 pay up, or let the table learn your debts
const char *gameStateFhBuyBig(int slot, int k, int world);  // 3.1-dev.53 buy big-ticket offer k (world: the Planetary Shield's)
bool gameStateFhDebt(int slot, int f, int n);               // call in faction f's debt (n ships when they cost Credits)
const char *gameStateFhOrders(int slot, const int8_t *t, const int8_t *guard, const uint8_t *ally, const int8_t *g);  // nullptr = ok
const char *gameStateFhReveal(int slot, const int8_t *g, const int8_t *a, const int8_t *b);
