#pragma once
// Commanders (v0.21.20; 3.1-dev.6: the 16 CHARACTERS, 4 per faction). The pixel art lives in commander_art.cpp, GENERATED from
// tools/art/portraits_v2.html#export (24x28, a small palette per portrait,
// one byte per pixel, 0 = transparent) -- re-export there, never hand-edit.
// Names/bios here are placeholders (original characters, no likenesses).
//
// Shared by the AMOLED (.ino) and the phone (/art JSON in web_server.cpp),
// so both platforms draw the exact same faces.
#include <stdint.h>

#define COMMANDERS_PER_FACTION 4
#define NUM_COMMANDERS 16
#define COMMANDER_ART_W 24
#define COMMANDER_ART_H 28

// 3.1-dev.6: the 16 CHARACTERS. id = faction * 4 + class; rows Rebels, Empire,
// Pykes, Crimson Dawn; columns WARLORD, CONSUL, INFILTRATOR, SCOUNDREL. Each
// faction's missing class is a DEFECTOR from another faction.
#define CLASS_WARLORD 0
#define CLASS_CONSUL 1
#define CLASS_INFILTRATOR 2
#define CLASS_SCOUNDREL 3
static inline int characterClass(int id) { return id >= 0 ? id % COMMANDERS_PER_FACTION : -1; }
// The faction a defector came from (-1 = born to their faction).
static const int8_t CHARACTER_DEFECTOR_FROM[NUM_COMMANDERS] = {
  -1, 1, -1, -1,   // Marr, Danto (ex-Empire), Voren, Kess
  -1, -1, -1, 0,   // Thane, Vell, Stahl, Merrow (ex-Rebel)
  -1, -1, 3, -1,   // Rhel, Vosk, Nyra (ex-Crimson Dawn), Oxa
  2, -1, -1, -1,   // Korrik (ex-Pyke), Ymra, Brakk, Kaan
};

struct CommanderArt {
  uint8_t nColors;
  const uint32_t *pal;  // 0xRRGGBB; entry 0 unused (index 0 = transparent)
  const uint8_t *px;    // COMMANDER_ART_W * COMMANDER_ART_H palette indices, row by row
};
extern const CommanderArt COMMANDER_ART[NUM_COMMANDERS];

struct CommanderInfo {
  const char *name;  // as shown on screen (uppercase)
  const char *bio;   // one line (the full story + facts live on the phone)
};
static const CommanderInfo COMMANDER_INFO[NUM_COMMANDERS] = {
  { "GEN. TOBIN MARR",    "Burned a Grand Moff's pardon. Kept the seal." },
  { "SEN. ARIS DANTO",    "Left the Imperial Senate. Still reads the birthday wishes." },
  { "CMDR. ILSA VOREN",   "Drew the Outer Rim's back routes. Crimson Dawn wants the maps back." },
  { "LT. KESS ARRANO",    "Best pilot in the squadron. Still owes the Pykes for the ship." },
  { "CAPT. VARO THANE",   "Never retreated. Once, he let one Rebel walk away." },
  { "MOFF SERAINE VELL",  "Governs by ledger. Keeps two of them." },
  { "ADM. KAEDRIC STAHL", "Thirty years of victories, each one with a bill attached." },
  { "DACE MERROW",        "Traded a few names for a pardon. Kept the old codes." },
  { "MATRIARCH RHEL",     "Old as the spice routes. Has never broken her word." },
  { "VOSK THE BROKER",    "Every deal has a price. He already knows yours." },
  { "NYRA SOL",           "Sold the Dawn's safehouses and vanished. They're still looking." },
  { "OXA TULL",           "Collects what's owed. With interest." },
  { "KORRIK DRAY",        "Broke legs for the Pykes. Now breaks them for the Dawn." },
  { "LADY YMRA VASH",     "No one has seen her face. Everyone has felt her reach." },
  { "BRAKK",              "Says little. Breaks things. Scratches a starbird on every charge." },
  { "SERETH KAAN",        "Charming, ruthless, always two moves ahead." },
};

// Who speaks for a faction in transmissions to you: its first born member in
// this order (a Consul first -- diplomats speak for factions), skipping
// defectors and YOUR character. 3.1-dev.6 (was: the first commander).
// 3.1-dev.23: the short name the TABLE calls each character by (big letters, few of them).
static const char *const COMMANDER_CALL[NUM_COMMANDERS] = {
  "MARR", "DANTO", "VOREN", "KESS", "THANE", "VELL", "STAHL", "MERROW",
  "RHEL", "VOSK", "NYRA", "OXA", "KORRIK", "YMRA", "BRAKK", "KAAN",
};

static const int8_t FACTION_VOICES[4][3] = { {0, 2, 3}, {5, 4, 6}, {9, 8, 11}, {13, 14, 15} };
static inline int commanderVoiceFor(int faction, int myCommanderId) {
  if (faction < 0 || faction > 3) return -1;
  for (int k = 0; k < 3; k++) if (FACTION_VOICES[faction][k] != myCommanderId) return FACTION_VOICES[faction][k];
  return FACTION_VOICES[faction][0];
}
