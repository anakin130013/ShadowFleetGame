// Sound effects (v0.25.3). The 1.75" Command Console has an ES8311 codec and
// a speaker connector; the 1.43" Table Console has no audio hardware, so on
// that build these are no-ops. Every sound is original and synthesized in
// code from a few tone steps -- no sound files, nothing licensed.
#pragma once
#include <stdint.h>

enum SfxId : uint8_t {
  SFX_CHIME,    // a new phase / round begins
  SFX_HORN,     // a battle starts somewhere in the galaxy
  SFX_FANFARE,  // a public big moment: first blood, seat of power, the Core falls
  SFX_ALARM,    // a darker one: betrayal, a destroyed fleet, the final round
  SFX_CHIRP,    // an incoming transmission (static + a blip)
  SFX_COUNT
};

void sfAudioBegin();       // after the display/touch bring-up (needs the I2C bus)
void sfxPlay(SfxId id);    // queue a sound; returns at once
const char *sfAudioStatus();  // v0.25.6: what sfAudioBegin found ("ready ...", or why sound is off)
