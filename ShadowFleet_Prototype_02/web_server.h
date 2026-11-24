#pragma once
// allowHome (v0.22.5): the Table Console tries the saved home Wi-Fi first
// ("home Wi-Fi mode": phones stay on their own network); a hosting Command
// Console always runs its own "ShadowFleet" network as before.
void networkInit(bool allowHome);
void networkHandle();
bool networkOnHomeWifi();         // joined the home network this boot
bool networkHomeSaved();          // a home network is saved
bool networkHomeFailed();         // saved + preferred, but unreachable this boot
const char *networkHomeSsid();    // saved network name ("" if none)
const char *networkGameUrl();     // what phones open: "http://<ip>" on home Wi-Fi, "http://192.168.4.1" otherwise
void networkSwitchMode(bool home);
const char *networkFailWhy();     // 3.1-dev.62: why the home join failed, in plain words ("" if it didn't)  // remember the preference and restart (table button)
