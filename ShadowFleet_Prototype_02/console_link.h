#pragma once
// ===========================================================================
// Console link (v0.22.1) -- ESP-NOW between consoles.
//
// The TABLE console (1.43") hosts the game; a COMMAND console (1.75") that
// hears it at power-on joins as a player instead of hosting its own:
//   table -> broadcast BEACON every 500 ms ("a table is here", channel 1)
//   command -> table   HELLO, then CALLs (one per game action)
//   table -> command   STATE (the whole GameState, ~4 KB, in 230-byte chunks,
//                      unicast so the radio retries) whenever it changes or
//                      at least once a second; REPLY to each CALL
// The command console keeps an exact mirror of the table's GameState, so its
// screens read state exactly as before; only the gameState* functions that
// CHANGE the game are forwarded (see gameStateSetRemote in game_state.h).
// Both consoles must run the same firmware -- every packet carries
// sizeof(GameState) and a protocol version, and mismatches are ignored.
// ===========================================================================
#include <stdint.h>

#define LINK_CHANNEL 1   // the table's OWN network channel; on home Wi-Fi it is the router's (command consoles scan)

// Table console: start ESP-NOW alongside the Wi-Fi network (call after the AP is up).
void linkBeginTable(bool onStation);   // onStation: the table joined home Wi-Fi (v0.22.5)
// (v0.23.2: the table sends state / beacons and runs forwarded calls on its
// own task, started by linkBeginTable -- nothing to call from loop().)

// Command console: listen up to `ms` for a table. true = joined (this console
// is now a mirror; don't start a Wi-Fi network); false = no table found
// (ESP-NOW shut down again; host as usual).
bool linkTryJoinTable(uint32_t ms);
// Command console: health -- ms since the last state from the table (0 if never).
uint32_t linkMsSinceState();
bool linkIsJoined();
// Command console: re-introduce itself if the table goes quiet -- call from loop().
void linkCommandLoop(int mySlot = -1);  // 2.0-dev.13: mySlot = this console's seat (tells the table it's still here)
