// Console link (v0.22.1) -- see console_link.h for the protocol overview.
#include "console_link.h"
#include "game_state.h"
#include "sf_health.h"
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <Preferences.h>
#include <string.h>

enum : uint8_t { LT_BEACON = 1, LT_HELLO, LT_STATE, LT_CALL, LT_REPLY, LT_HERE };  // LT_HERE: 2.0-dev.13
static const uint32_t LINK_MAGIC = 0x4B4C4653;  // "SFLK"
static const uint8_t LINK_VER = 1;
static const int CHUNK = 1400;   // v0.22.3: ESP-NOW v2 frames carry up to 1470 bytes -> the state is 3 packets, not 19
static const int STATE_CHUNKS = (sizeof(GameState) + CHUNK - 1) / CHUNK;
static_assert(STATE_CHUNKS <= 32, "GameState too big for the 32-chunk mask");

struct __attribute__((packed)) LinkHdr { uint32_t magic; uint8_t type; uint8_t ver; uint16_t stateSize; };
struct __attribute__((packed)) StateMsg { LinkHdr h; uint16_t snap; uint8_t idx; uint8_t count; uint8_t data[CHUNK]; };
struct __attribute__((packed)) CallMsg { LinkHdr h; uint16_t req; uint8_t fn; uint8_t nargs; int32_t args[6]; char str[32]; };
struct __attribute__((packed)) ReplyMsg { LinkHdr h; uint16_t req; uint16_t snap; int32_t value; char text[64]; };
struct __attribute__((packed)) HereMsg { LinkHdr h; int8_t slot; };   // "still here" from a command console's player

static LinkHdr hdr(uint8_t type) { LinkHdr h = { LINK_MAGIC, type, LINK_VER, (uint16_t)sizeof(GameState) }; return h; }
static bool hdrOk(const uint8_t *d, int len) {
  if (len < (int)sizeof(LinkHdr)) return false;
  const LinkHdr *h = (const LinkHdr *)d;
  return h->magic == LINK_MAGIC && h->ver == LINK_VER && h->stateSize == sizeof(GameState);
}
static const uint8_t BCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static wifi_interface_t table_if = WIFI_IF_AP;   // v0.22.5: WIFI_IF_STA when the table is on home Wi-Fi

static bool addPeer(const uint8_t *mac, wifi_interface_t ifidx) {
  if (esp_now_is_peer_exist(mac)) return true;
  esp_now_peer_info_t p;
  memset(&p, 0, sizeof(p));
  memcpy(p.peer_addr, mac, 6);
  p.channel = 0;  // = the current channel (LINK_CHANNEL)
  p.ifidx = ifidx;
  p.encrypt = false;
  if (esp_now_add_peer(&p) != ESP_OK) return false;
  if (memcmp(mac, BCAST, 6) != 0) {
    // v0.22.3: consoles sit inches apart -- send at 24 Mbps instead of the
    // 1 Mbps default (each state packet was taking milliseconds of airtime).
    esp_now_rate_config_t rc;
    memset(&rc, 0, sizeof(rc));
    rc.phymode = WIFI_PHY_MODE_11G;
    rc.rate = WIFI_PHY_RATE_24M;
    esp_now_set_peer_rate_config(mac, &rc);
  }
  return true;
}
static void sendRaw(const uint8_t *mac, const void *data, size_t len) {
  for (int tries = 0; tries < 20; tries++) {          // the send queue can be briefly full
    if (esp_now_send(mac, (const uint8_t *)data, len) != ESP_ERR_ESPNOW_NO_MEM) return;
    delay(2);
  }
}

// ---------------------------------------------------------------------------
// TABLE CONSOLE
// ---------------------------------------------------------------------------
struct Inbound { uint8_t mac[6]; uint8_t type; CallMsg call; };
static QueueHandle_t table_inbox = nullptr;
static const int MAX_CONSOLES = 4;
struct ConsoleRec { bool used; uint8_t mac[6]; uint16_t lastReq; ReplyMsg lastReply; bool hasReply; };
static ConsoleRec consoles[MAX_CONSOLES];
static uint16_t table_snap = 0;
static uint32_t st_tSent = 0, st_tChanged = 0, st_tCalls = 0, st_tReport = 0;   // 5-second stats (v0.22.2)
static uint32_t table_lastHash = 0, table_lastSend = 0, table_lastBeacon = 0, table_lastCheck = 0;

static void tableRecv(const esp_now_recv_info_t *info, const uint8_t *d, int len) {
  if (!hdrOk(d, len) || !table_inbox) return;
  const uint8_t type = ((const LinkHdr *)d)->type;
  if (type != LT_HELLO && !(type == LT_CALL && len >= (int)sizeof(CallMsg)) && !(type == LT_HERE && len >= (int)sizeof(HereMsg))) return;
  Inbound in;
  memset(&in, 0, sizeof(in));
  memcpy(in.mac, info->src_addr, 6);
  in.type = type;
  if (type == LT_CALL) memcpy(&in.call, d, sizeof(CallMsg));
  if (type == LT_HERE) in.call.args[0] = ((const HereMsg *)d)->slot;
  xQueueSend(table_inbox, &in, 0);  // never block the Wi-Fi task
}

static ConsoleRec *consoleFor(const uint8_t *mac, bool create) {
  for (auto &c : consoles) if (c.used && memcmp(c.mac, mac, 6) == 0) return &c;
  if (!create) return nullptr;
  for (auto &c : consoles) if (!c.used) {
    memset(&c, 0, sizeof(c));
    c.used = true;
    memcpy(c.mac, mac, 6);
    addPeer(mac, table_if);
    Serial.printf("[LINK] command console joined: %02X:%02X:%02X:%02X:%02X:%02X\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return &c;
  }
  return nullptr;
}

static uint32_t hashState(const GameState &g) {  // FNV-1a over the whole struct
  const uint8_t *p = (const uint8_t *)&g;
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < sizeof(GameState); i++) { h ^= p[i]; h *= 16777619u; }
  return h;
}

static void sendStateTo(const GameState &g, uint16_t snap) {
  StateMsg m;
  m.h = hdr(LT_STATE);
  m.snap = snap;
  m.count = STATE_CHUNKS;
  for (auto &c : consoles) {
    if (!c.used) continue;
    for (int i = 0; i < STATE_CHUNKS; i++) {
      const size_t off = (size_t)i * CHUNK, n = min((size_t)CHUNK, sizeof(GameState) - off);
      m.idx = i;
      memcpy(m.data, (const uint8_t *)&g + off, n);
      sendRaw(c.mac, &m, sizeof(LinkHdr) + 4 + n);
    }
  }
}

static void linkTask(void *);  // v0.23.2, below

void linkBeginTable(bool onStation) {
  table_if = onStation ? WIFI_IF_STA : WIFI_IF_AP;
  if (esp_now_init() != ESP_OK) { Serial.println("[LINK] esp_now_init failed"); return; }
  table_inbox = xQueueCreate(8, sizeof(Inbound));
  addPeer(BCAST, table_if);
  esp_now_register_recv_cb(tableRecv);
  // Stack: tableLinkStep can hold two GameState copies (~3.7 KB each) plus a send buffer.
  xTaskCreate(linkTask, "link", 16384, nullptr, 3, nullptr);
  uint8_t ch = 0; wifi_second_chan_t sc;
  esp_wifi_get_channel(&ch, &sc);  // v0.22.6: the real one (the router's on home Wi-Fi), not LINK_CHANNEL
  Serial.printf("[LINK] table console: ESP-NOW up on channel %d (%s), state %u bytes in %d chunks\n",
                ch, onStation ? "home Wi-Fi" : "own network", (unsigned)sizeof(GameState), STATE_CHUNKS);
}

static void tableLinkStep() {
  if (!table_inbox) return;
  const uint32_t now = millis();
  bool forceState = false;
  Inbound in;
  while (xQueueReceive(table_inbox, &in, 0) == pdTRUE) {
    ConsoleRec *c = consoleFor(in.mac, true);
    if (!c) continue;
    if (in.type == LT_HELLO) { forceState = true; continue; }
    if (in.type == LT_HERE) { gameStatePresent(in.call.args[0]); continue; }  // 2.0-dev.13
    if (c->hasReply && in.call.req == c->lastReq) {   // a retry of a call we already ran
      sendRaw(c->mac, &c->lastReply, sizeof(ReplyMsg));
      continue;
    }
    in.call.str[sizeof(in.call.str) - 1] = '\0';
    RemoteResult r = gameStateDispatch(in.call.fn, in.call.args, in.call.str);
    st_tCalls++;
    // Push the new state BEFORE the reply, so the console already mirrors it
    // when its call returns.
    GameState g = gameStateRead();
    table_snap++;
    table_lastHash = hashState(g);
    table_lastSend = now;
    sendStateTo(g, table_snap);
    st_tSent++;
    ReplyMsg rep;
    memset(&rep, 0, sizeof(rep));
    rep.h = hdr(LT_REPLY);
    rep.req = in.call.req;
    rep.snap = table_snap;
    rep.value = r.value;
    memcpy(rep.text, r.text, sizeof(rep.text));
    c->lastReq = rep.req;
    c->lastReply = rep;
    c->hasReply = true;
    sendRaw(c->mac, &rep, sizeof(rep));
  }
  if (now - st_tReport >= 5000) {
    st_tReport = now;
    bool any = false;
    for (auto &c : consoles) any |= c.used;
    if (any) Serial.printf("[LINK] 5s: sent %u states (%u changed), ran %u calls\n", (unsigned)st_tSent, (unsigned)st_tChanged, (unsigned)st_tCalls);
    st_tSent = st_tChanged = st_tCalls = 0;
  }
  if (now - table_lastBeacon >= 200) {   // v0.22.5: often enough for a console scanning channels
    table_lastBeacon = now;
    LinkHdr b = hdr(LT_BEACON);
    sendRaw(BCAST, &b, sizeof(b));
  }
  if (forceState || now - table_lastCheck >= 100) {   // phones change the state too -- watch for it
    table_lastCheck = now;
    bool any = false;
    for (auto &c : consoles) any |= c.used;
    if (!any) return;
    GameState g = gameStateRead();
    const uint32_t h = hashState(g);
    if (forceState || h != table_lastHash || now - table_lastSend >= 1000) {
      if (h != table_lastHash) { table_snap++; st_tChanged++; }
      st_tSent++;
      table_lastHash = h;
      table_lastSend = now;
      sendStateTo(g, table_snap);
    }
  }
}

// v0.23.2: the table's side of the link runs on its OWN task. It used to run
// from loop() between phone requests -- on home Wi-Fi each phone's /state
// answer goes through the router and takes longer, so a command console's tap
// waited its turn (100-200 ms, user: "clicks have definitely slowed down").
// This task wakes the moment a tap arrives (or every 20 ms for beacons and
// state pushes), at a higher priority than loop().
static void linkTask(void *) {
  healthWatchThisTask();  // 2.0-dev.6: stuck HEALTH_WDT_S seconds -> restart (sf_health.h)
  for (;;) {
    healthFeed();
    Inbound peek;
    xQueuePeek(table_inbox, &peek, pdMS_TO_TICKS(20));  // sleep until a tap or hello arrives
    tableLinkStep();
  }
}

// ---------------------------------------------------------------------------
// COMMAND CONSOLE
// ---------------------------------------------------------------------------
static uint8_t table_mac[6];
static volatile bool table_found = false, joined = false;
static uint8_t beacon_mac[6];                // v0.22.6: last beacon heard (the table's MAC changes with its Wi-Fi mode)
static volatile bool beacon_seen = false;
static const uint8_t SWEEP_ORDER[] = { 1, 6, 11, 2, 3, 4, 5, 7, 8, 9, 10, 12, 13 };
static GameState rx_state;                 // reassembly buffer
static uint32_t rx_mask = 0;
static uint16_t rx_snap = 0;
static volatile uint16_t applied_snap = 0;
static volatile bool have_state = false;
static volatile uint32_t last_state_ms = 0;
static volatile uint16_t wait_req = 0;
static volatile bool reply_ready = false;
static ReplyMsg reply_buf;
static uint16_t next_req = 1;
static volatile uint32_t st_cApplied = 0, st_cChunks = 0;          // 5-second stats (v0.22.2)
static uint32_t st_cCalls = 0, st_cCallMsSum = 0, st_cCallMsMax = 0, st_cWaitMsMax = 0, st_cReport = 0;

static void commandRecv(const esp_now_recv_info_t *info, const uint8_t *d, int len) {
  if (!hdrOk(d, len)) return;
  const uint8_t type = ((const LinkHdr *)d)->type;
  if (type == LT_BEACON) {
    if (!table_found) { memcpy(table_mac, info->src_addr, 6); table_found = true; }
    memcpy(beacon_mac, info->src_addr, 6);
    beacon_seen = true;
    return;
  }
  if (!table_found || memcmp(info->src_addr, table_mac, 6) != 0) return;
  if (type == LT_STATE && len > (int)(sizeof(LinkHdr) + 4)) {
    const StateMsg *m = (const StateMsg *)d;
    if (m->count != STATE_CHUNKS || m->idx >= STATE_CHUNKS) return;
    if (m->snap != rx_snap) { rx_snap = m->snap; rx_mask = 0; }
    const size_t off = (size_t)m->idx * CHUNK, n = min((size_t)CHUNK, sizeof(GameState) - off);
    if ((size_t)(len - sizeof(LinkHdr) - 4) < n) return;
    memcpy((uint8_t *)&rx_state + off, m->data, n);
    st_cChunks++;
    rx_mask |= 1u << m->idx;
    const uint32_t full = (STATE_CHUNKS == 32) ? 0xFFFFFFFFu : ((1u << STATE_CHUNKS) - 1);
    if (rx_mask == full) {
      gameStateApplySnapshot(rx_state);
      st_cApplied++;
      applied_snap = rx_snap;
      have_state = true;
      last_state_ms = millis();
      rx_mask = 0;
    }
  } else if (type == LT_REPLY && len >= (int)sizeof(ReplyMsg)) {
    const ReplyMsg *r = (const ReplyMsg *)d;
    if (r->req == wait_req && !reply_ready) { memcpy(&reply_buf, r, sizeof(ReplyMsg)); reply_ready = true; }
  }
}

// Forwards one game action to the table and waits for its result -- and for
// the state that includes it -- so the caller's next gameStateRead() already
// shows the change. Runs on the UI task; worst case ~1 s if the radio drops.
static bool remoteCall(uint8_t fn, const int32_t *args, int nargs, const char *str, RemoteResult *out) {
  CallMsg m;
  memset(&m, 0, sizeof(m));
  m.h = hdr(LT_CALL);
  m.req = next_req++;
  if (next_req == 0) next_req = 1;
  m.fn = fn;
  m.nargs = (uint8_t)min(nargs, 6);
  for (int i = 0; i < m.nargs; i++) m.args[i] = args[i];
  if (str) strncpy(m.str, str, sizeof(m.str) - 1);
  reply_ready = false;
  wait_req = m.req;
  const uint32_t tCall = millis();
  for (int attempt = 0; attempt < 3; attempt++) {
    sendRaw(table_mac, &m, sizeof(m));
    const uint32_t t0 = millis();
    while (!reply_ready && millis() - t0 < 120) delay(2);   // v0.22.3: retry sooner (was 300 ms)
    if (reply_ready) break;
  }
  if (!reply_ready) { Serial.printf("[LINK] call %u timed out\n", fn); return false; }
  const uint32_t t0 = millis();
  while ((int16_t)(applied_snap - reply_buf.snap) < 0 && millis() - t0 < 400) delay(2);
  st_cCalls++;
  st_cWaitMsMax = max(st_cWaitMsMax, millis() - t0);
  st_cCallMsSum += millis() - tCall;
  st_cCallMsMax = max(st_cCallMsMax, millis() - tCall);
  out->value = reply_buf.value;
  memcpy(out->text, reply_buf.text, sizeof(out->text));
  out->text[sizeof(out->text) - 1] = '\0';
  return true;
}

bool linkTryJoinTable(uint32_t ms) {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(LINK_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_ps(WIFI_PS_NONE);  // v0.22.2: radio never naps -- it was missing state packets (ESP-NOW receive)
  if (esp_now_init() != ESP_OK) { Serial.println("[LINK] esp_now_init failed"); return false; }
  esp_now_register_recv_cb(commandRecv);
  Serial.printf("[LINK] listening %u ms for a table console...\n", (unsigned)ms);
  uint32_t t0 = millis();
  // v0.22.5: on home Wi-Fi the table sits on the router's channel -- try the
  // channel it was on last time first, then sweep 1-13 (beacons every 200 ms).
  Preferences prefs;
  prefs.begin("sflink", false);
  const int lastCh = prefs.getInt("ch", LINK_CHANNEL);
  int k = -1, ch = lastCh;
  while (!table_found && millis() - t0 < ms) {
    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    const uint32_t dwell = millis();
    while (!table_found && millis() - dwell < (k < 0 ? 600u : 260u)) delay(10);
    if (table_found) break;
    k = (k + 1) % (int)sizeof(SWEEP_ORDER);
    ch = SWEEP_ORDER[k];
  }
  if (table_found && ch != lastCh) prefs.putInt("ch", ch);
  prefs.end();
  if (table_found) Serial.printf("[LINK] table heard on channel %d\n", ch);
  if (table_found) {
    addPeer(table_mac, WIFI_IF_STA);
    LinkHdr hello = hdr(LT_HELLO);
    t0 = millis();
    while (!have_state && millis() - t0 < 2500) {
      sendRaw(table_mac, &hello, sizeof(hello));
      for (int i = 0; i < 25 && !have_state; i++) delay(20);
    }
  }
  if (!have_state) {
    Serial.println(table_found ? "[LINK] a table answered but sent no state -- hosting instead"
                               : "[LINK] no table console -- hosting");
    esp_now_unregister_recv_cb();
    esp_now_deinit();
    table_found = false;
    return false;
  }
  joined = true;
  gameStateSetRemote(remoteCall);
  Serial.printf("[LINK] joined the table console %02X:%02X:%02X:%02X:%02X:%02X\n",
                table_mac[0], table_mac[1], table_mac[2], table_mac[3], table_mac[4], table_mac[5]);
  return true;
}

uint32_t linkMsSinceState() { return have_state ? millis() - last_state_ms : 0; }
bool linkIsJoined() { return joined; }

// v0.22.6: the table may come back on another channel (it joined / left home
// Wi-Fi) and with another MAC (station vs AP). Follow its beacon: adopt the
// MAC it beacons from, and if none is heard here, hop to the next channel.
static void followTableBeacon(uint32_t now) {
  static uint32_t lastHop = 0;
  static int k = -1;
  if (now - lastHop < 300) return;
  lastHop = now;
  if (beacon_seen) {
    if (memcmp(beacon_mac, table_mac, 6) != 0) {
      esp_now_del_peer(table_mac);
      memcpy(table_mac, beacon_mac, 6);
      addPeer(table_mac, WIFI_IF_STA);
      Serial.printf("[LINK] table is back as %02X:%02X:%02X:%02X:%02X:%02X\n",
                    table_mac[0], table_mac[1], table_mac[2], table_mac[3], table_mac[4], table_mac[5]);
    }
    uint8_t ch = 0; wifi_second_chan_t sc;
    esp_wifi_get_channel(&ch, &sc);
    static uint8_t savedCh = 0;
    if (ch != savedCh) {  // remember it for the next boot
      savedCh = ch;
      Preferences prefs;
      prefs.begin("sflink", false);
      if (prefs.getInt("ch", LINK_CHANNEL) != ch) prefs.putInt("ch", ch);
      prefs.end();
    }
    beacon_seen = false;  // stay on this channel while beacons keep coming
    return;
  }
  k = (k + 1) % (int)sizeof(SWEEP_ORDER);
  esp_wifi_set_channel(SWEEP_ORDER[k], WIFI_SECOND_CHAN_NONE);
  if (k == 0) Serial.println("[LINK] table quiet -- searching channels 1-13");
}

// Command console, from loop(): if the table went quiet (rebooted), say hello
// again so it adds us back -- and follow it to a new channel (v0.22.6).
void linkCommandLoop(int mySlot) {
  static uint32_t lastHello = 0, lastHere = 0;
  if (!joined) return;
  const uint32_t now = millis();
  if (mySlot >= 0 && now - lastHere >= HERE_EVERY_MS) {  // 2.0-dev.13: so the table doesn't mark us AWAY
    lastHere = now;
    HereMsg m;
    m.h = hdr(LT_HERE);
    m.slot = (int8_t)mySlot;
    sendRaw(table_mac, &m, sizeof(m));
  }
  if (now - st_cReport >= 5000) {
    st_cReport = now;
    Serial.printf("[LINK] 5s: applied %u states from %u chunks; %u calls, avg %u ms, max %u ms (waiting for state max %u ms)\n",
                  (unsigned)st_cApplied, (unsigned)st_cChunks, (unsigned)st_cCalls,
                  (unsigned)(st_cCalls ? st_cCallMsSum / st_cCalls : 0), (unsigned)st_cCallMsMax, (unsigned)st_cWaitMsMax);
    st_cApplied = st_cChunks = 0; st_cCalls = st_cCallMsSum = st_cCallMsMax = st_cWaitMsMax = 0;
  }
  if (now - last_state_ms > 4000) followTableBeacon(now);
  else beacon_seen = false;
  if (now - last_state_ms > 3000 && now - lastHello > 1000) {
    lastHello = now;
    LinkHdr hello = hdr(LT_HELLO);
    sendRaw(table_mac, &hello, sizeof(hello));
  }
}
