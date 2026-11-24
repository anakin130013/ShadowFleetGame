#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <esp_wifi.h>
#include <lwip/sockets.h>  // 3.1-dev.62 sendBounded
#include <errno.h>
#include "web_server.h"
#include <LittleFS.h>
#include "game_state.h"
#include "version.h"
#include "sf_health.h"
#include "commander_art.h"
#include "sf_page_gz.h"   // 3.1-dev.35: the phone page, gzipped (tools/page/make_page_gz.py)
#include "sf_web_font.h"  // 3.1-dev.4: Space Grotesk, served at /font.woff2
#include "sf_web_icon.h"  // 3.1-dev.18: the home-screen icon, served at /icon.png
#include <new>
#include <esp_heap_caps.h>
#define STATE_RAM_FLOOR 10000  // 3.1-dev.74: below this much free internal RAM, /state polls are told to wait
#define BIG_RAM_FLOOR 16000    // 3.1-dev.110: below this, the page / font / portraits / a first full /state wait too (every phone retries)
static bool ramLow(uint32_t floor) { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < floor; }
#define SEND_RAM_FLOOR 20000   // 3.1-dev.112: big answers stop queueing below this much free internal RAM (sendBounded)
static uint32_t net_send_waits = 0;

// 2.0-dev.15: /state is built in one reserved block (PSRAM, being > 4 KB);
// sendStateJson logs a new largest answer so this can be checked.
#define STATE_JSON_RESERVE 24576
#define FH_STR2(x) #x
#define FH_STR(x) FH_STR2(x)  // 3.1-dev.43
static size_t stateJsonMax = 0;
#include "negotiation_bank.h"

static WebServer server(80);
// Home Wi-Fi mode state (v0.22.5; see networkInit).
static bool net_home = false, net_home_failed = false, net_home_saved = false, net_allow_home = false;
static char net_ssid[33] = "";
static char net_pass[65] = "";        // 3.1-dev.62: the password of the network actually joined (for the rejoin)
static char net_fail_why[72] = "";    // 3.1-dev.62: why the join failed, in plain words (table Lobby + Settings)
static volatile uint8_t net_join_reason = 0;
static uint32_t net_slow_drops = 0, net_drops_n = 0;   // 3.1-dev.62: phones cut off mid-send for stalling (sendBounded)
static uint8_t net_ap_channel = 1;
static bool net_fail_retry = false;   // 3.1-dev.69: the join failed for a reason worth retrying (not a wrong password)
static char net_url[40] = "http://192.168.4.1";
static uint32_t net_restart_at = 0;

// The phone UI is a single page that adapts to game stage/phase. It stores a
// random client id in localStorage so a refresh/reconnect rejoins the same
// slot (per design: disconnected slots stay reserved).
static const char PAGE_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1, user-scalable=no, viewport-fit=cover">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
<meta name="apple-mobile-web-app-title" content="Shadow Fleet">
<meta name="theme-color" content="#05070C">
<link rel="manifest" href="/manifest.json">
<link rel="apple-touch-icon" href="/icon.png">
<link rel="icon" href="/icon.png">
<title>Shadow Fleet</title>
<style>
  /* v0.21.0 UI refresh: "holo-console" theme from the approved mockups.
     --accent is the player's own faction color (set from JS once they've
     picked one). Screens not yet rebuilt still use the older classes below,
     re-colored through these variables. */
  :root { --bg:#05070C; --panel:#0B1420; --line:#2A3B50; --ring:#18263A; --text:#E6F1FA;
          --dim:#7C93A8; --faint:#56697C; --cyan:#5FD4FF; --red:#FF6B5A; --accent:#5FD4FF;
          /* 3.1 design language (style guide S1): motion, corners, spacing. Screens
             move to these as they're rebuilt; old values stay until then. */
          --ease:cubic-bezier(.2,.8,.2,1); --t-fast:150ms; --t-base:250ms; --t-slow:400ms;
          --r-sm:8px; --r-md:14px; --r-full:999px; }
  /* 3.1-dev.4: the display face (style guide S1) -- titles, labels, buttons, the top
     bar. Served by the table (/font.woff2, cached a year); reading text keeps the
     phone's own font. Until it loads, the old stack shows (font-display:swap). */
  @font-face { font-family:'SF Display'; src:url(font.woff2) format('woff2'); font-weight:300 700; font-display:swap; }
  :root { --display:'SF Display', 'Barlow Semi Condensed', 'Roboto Condensed', sans-serif; }
  h1, button, .topbar, .pill, .feedTitle, .frHead, .feedRoundHead, .feedTabs button, .ntHead, .feedHist b { font-family:var(--display); }
  /* Patched screens keep their elements, so state changes can glide. */
  button, .card { transition: background-color var(--t-fast) var(--ease), border-color var(--t-fast) var(--ease),
                  color var(--t-fast) var(--ease), opacity var(--t-base) var(--ease), transform var(--t-fast) var(--ease); }
  button:active { transform: scale(.97); }
  /* 3.1-dev.6 characters: select grid, preview, your card */
  .topL { display:flex; align-items:center; gap:10px; }
  .meAv { width:40px; height:40px; min-height:0; padding:0; margin:0; border-radius:50%; border:2px solid var(--accent); overflow:hidden; display:flex; align-items:flex-end; justify-content:center; background:#141A24; }
  .meAv img { width:34px; height:40px; }
  .cgrid { display:grid; grid-template-columns:28px repeat(4, minmax(0, 1fr)); gap:6px; align-items:center; margin:8px 0 12px; padding-right:34px; }  /* 3.1-dev.91: centred */
  .chead { font-family:var(--display); font-size:9px; letter-spacing:1px; color:var(--dim); text-align:center; }
  .crow { display:flex; justify-content:center; }
  .ccell { position:relative; margin:0; min-height:0; padding:6px 2px 4px; border-radius:var(--r-sm); border:1px solid var(--line); background:var(--panel); display:flex; flex-direction:column; align-items:center; gap:2px; }
  .ccell img { width:40px; height:47px; }
  .ccell small { font-size:11px; letter-spacing:0; color:var(--text); }
  .ccell.on { border:2px solid var(--fc); background:#141A24; box-shadow:0 0 12px color-mix(in srgb, var(--fc) 40%, transparent); }
  .ccell.taken { opacity:.35; }
  .cdef { position:absolute; top:4px; right:4px; width:8px; height:8px; border-radius:50%; background:var(--dc); box-shadow:0 0 0 2px #05070C; }
  .cprev { border-radius:var(--r-md); border:2px solid var(--fc); background:linear-gradient(color-mix(in srgb, var(--fc) 12%, #0C1118), #0C1118 45%); padding:14px; text-align:center; margin:0 0 10px; }
  .cprevTop { display:flex; align-items:center; justify-content:center; gap:14px; margin-bottom:10px; }
  .cprevTop > div { text-align:left; }
  .cface { width:68px; height:68px; border-radius:50%; border:2px solid var(--fc); background:#141A24; object-fit:cover; object-position:50% 100%; }
  .cface.big { width:88px; height:88px; }
  .cname { font-family:var(--display); font-weight:700; font-size:20px; letter-spacing:2px; }
  .cmeta { font-family:var(--display); font-size:11px; letter-spacing:2px; color:var(--fc); }
  .cfacts, .cquirk { font-size:12px; color:var(--dim); margin-top:3px; }
  .cquirk { font-style:italic; color:#C8D2DC; }
  .cfactRow { display:grid; grid-template-columns:repeat(3, minmax(0, 1fr)); gap:6px; margin:0 0 10px; }
  .cfactRow > div { border-radius:var(--r-sm); border:1px solid var(--line); background:#0C1118; padding:6px 4px; font-size:12px; }
  .cfactRow b { display:block; font-family:var(--display); font-size:9px; letter-spacing:1px; color:var(--fc); }
  .cstory { font-size:14px; line-height:1.5; font-style:italic; color:#D8E2EC; margin:0 0 10px; }
  .kit { border-radius:var(--r-sm); padding:8px 10px; margin:6px 0; font-size:14px; line-height:1.4; }
  .kit b { display:block; font-family:var(--display); font-size:11px; letter-spacing:2px; margin-bottom:2px; }
  .kit.good { background:#0E1A10; border:1px solid #2E5A36; } .kit.good b { color:#8CE7A0; }
  .kit.bad { background:#1A0E10; border:1px solid #5A2228; } .kit.bad b { color:#FF6B5A; }
  .kit.move { background:#0E1420; border:1px solid #2E4A6A; } .kit.move b { color:#5FD4FF; } .kit.move small { display:block; margin-top:4px; font-size:11px; color:var(--dim); }
  .kit.quest { background:#11161D; border:1px solid #3A4A5C; } .kit.quest b { color:#FFD166; }
  .fvsub { font-size:12px; color:var(--dim); margin-bottom:12px; }
  .fvrow { width:100%; text-align:left; font:inherit; color:inherit; background:var(--surface,#0C1118); cursor:pointer; }
  .fvrow .tier { display:inline-flex; align-items:center; gap:4px; white-space:nowrap; font-size:12px; }
  .fvrow .top { gap:8px; }
  .fvline { font-size:11px; letter-spacing:2px; color:var(--dim); margin-top:6px; }
  .ldhead { display:flex; align-items:center; justify-content:center; gap:10px; margin-bottom:12px; text-align:left; }
  .ldhead b { display:block; font-family:var(--display); letter-spacing:3px; font-size:16px; }
  .ldhead small { font-size:12px; color:var(--dim); }
  .ladder { display:flex; flex-direction:column; gap:6px; text-align:left; width:100%; min-width:0; }
  .rung, .ldgift { box-sizing:border-box; min-width:0; max-width:100%; }
  .ldgift span { flex:1; min-width:0; }
  .rung { border:1px solid var(--line); border-left:3px solid var(--rc); border-radius:8px; padding:8px 10px; opacity:.72; transition:opacity var(--t-base) var(--ease); }
  .rung.on { opacity:1; border-color:var(--rc); background:rgba(255,255,255,.04); }
  .rung .rtop { display:flex; align-items:center; gap:8px; font-family:var(--display); font-size:13px; letter-spacing:2px; color:var(--rc); }
  .rung .rtop em { margin-left:auto; font-style:normal; font-size:11px; color:#05070C; background:var(--rc); border-radius:99px; padding:1px 8px; }
  .rmean { font-size:13px; color:var(--text); margin-top:4px; }
  .ldgift { display:flex; align-items:center; gap:8px; margin-top:6px; padding:6px 8px; border:1px dashed #FFD16666; border-radius:8px; font-size:12px; color:var(--dim); }
  .ldgift b { color:#FFD166; }
  .ldgift.got { border-style:solid; border-color:#8CE7A066; }
  .ldgift.got b { color:#8CE7A0; }
  .ldgift em { margin-left:auto; font-style:normal; font-size:10px; letter-spacing:2px; color:var(--dim); white-space:nowrap; }
  /* 3.1-dev.43 THE FINAL HOUR */
  .gc { position:relative; border-radius:16px; padding:12px; margin:8px 0; text-align:left; background:linear-gradient(160deg,#1B1608,#0C1118 55%); border:2px solid #FFD166; box-shadow:0 0 0 4px rgba(255,209,102,.08); }
  .gc.on { box-shadow:0 0 0 3px #FFD166, 0 0 18px #FFD16655; }
  .gc .gcls { font-family:var(--display); font-size:10px; letter-spacing:3px; color:#FFD166; }
  .gc .gnm { font-family:var(--display); font-weight:700; letter-spacing:2px; font-size:18px; margin:4px 0 2px; }
  .gc .gwhen { display:inline-block; font-family:var(--display); font-size:10px; letter-spacing:2px; border:1px solid #5FD4FF; color:#5FD4FF; border-radius:999px; padding:2px 8px; margin:4px 0 6px; }
  .gc .gfx { font-size:13px; line-height:1.45; color:#E8D9A8; }
  .gc .gfl { font-size:11px; color:var(--faint); font-style:italic; margin-top:6px; }
  .gc .gpr { text-align:right; font-family:var(--display); color:#FFD166; font-weight:700; }
  .gc.mini { padding:8px 10px; border-width:1.5px; } .gc.mini .gnm { font-size:14px; } .gc.mini .gfx { font-size:12px; }
  .gfan { display:flex; gap:6px; } .gfan > div { flex:1; min-width:0; overflow-wrap:anywhere; }
  .gfan .gc .gwhen { font-size:8.5px; letter-spacing:1px; padding:2px 5px; }
  .gflip { animation:gflip 520ms var(--ease) both; transform-origin:50% 50%; }
  @keyframes gflip { from { transform:perspective(500px) rotateY(90deg); opacity:0; } to { transform:none; opacity:1; } }
  .gpop { animation:gpop 400ms var(--ease) both; }
  @keyframes gpop { from { transform:scale(.8) rotate(-4deg); opacity:0; } to { transform:rotate(-1.5deg); opacity:1; } }
  .fhlast { margin:6px 0 0; padding:8px 10px; border:1px solid #FFB347; background:#1E1608; border-radius:8px; font-size:12px; line-height:1.45; color:#F2D9A8; text-align:left; }
  .fhlast b { display:block; font-family:var(--display); letter-spacing:2px; font-size:11px; color:#FFB347; }
  .fhdebt { display:flex; align-items:center; gap:6px; flex-wrap:wrap; justify-content:space-between; border:1px solid var(--line); border-radius:12px; padding:8px 10px; margin:6px 0; text-align:left; font-size:13px; }
  .fhdebt b { font-family:var(--display); letter-spacing:2px; } .fhdebt small { display:block; color:var(--dim); font-size:11px; }
  .fhdebt button { font:inherit; font-size:12px; font-weight:700; letter-spacing:1px; color:#8CE7A0; background:none; border:1px solid #8CE7A0; border-radius:999px; padding:6px 10px; }
  .fhdebt button:disabled { opacity:.35; }
  .fhkick { font-family:var(--display); font-size:11px; letter-spacing:3px; color:var(--dim); text-align:center; margin-top:10px; }
  .fhbig { font-family:var(--display); font-weight:700; letter-spacing:3px; text-align:center; font-size:20px; margin:2px 0 4px; }
  .fhsub { font-size:12px; color:var(--dim); line-height:1.45; text-align:center; margin:4px 0; }
  .fhord { display:grid; grid-template-columns:62px 1fr auto; gap:8px; align-items:center; border:1px dashed var(--line); border-radius:14px; padding:8px 10px; margin:8px 0 0; background:var(--surface,#0C1118); }
  .fhord.on { border-style:solid; border-color:var(--accent); } .fhord.dead { opacity:.4; grid-template-columns:62px 1fr; }
  .fhfl { font-family:var(--display); font-size:10px; letter-spacing:2px; color:var(--dim); text-align:center; line-height:1.2; } .fhfl b { display:block; font-size:22px; color:var(--text); } .fhfl.long { font-size:8px; letter-spacing:.5px; }
  .fhtg { text-align:left; font:inherit; background:none; border:0; color:var(--text); padding:4px 0; }
  .fhtg b { display:block; font-family:var(--display); letter-spacing:2px; font-size:14px; } .fhtg small { display:block; font-size:11px; color:var(--dim); margin-top:2px; }
  .fhally { display:flex; align-items:center; gap:8px; justify-content:flex-end; font-size:12px; color:var(--dim); margin:4px 4px 0; }
  .fhally button { width:34px; height:34px; border-radius:50%; border:1px solid var(--line); background:none; color:var(--text); font-size:18px; } .fhally b { min-width:18px; text-align:center; color:var(--text); }
  .fhSkT { display:flex; justify-content:space-between; font-family:var(--display); font-size:11px; letter-spacing:1.5px; color:var(--dim); } .fhSkT b { color:#FFD166; font-weight:700; }
  .fhSkW { display:flex; flex-wrap:wrap; justify-content:center; gap:6px; margin:8px 0; } .fhSkW span { padding:4px 10px; border-radius:999px; border:1px solid #233142; color:#5C6F84; font-family:var(--display); font-size:10px; letter-spacing:1px; }
  .fhSkipPill { position:fixed; top:calc(env(safe-area-inset-top, 0px) + 10px); right:calc(env(safe-area-inset-right, 0px) + 12px); z-index:5; width:auto; min-height:0; margin:0; padding:7px 12px;
                border-radius:999px; border:1px solid var(--line); background:rgba(12,17,24,.9); color:var(--dim); font-family:var(--display); font-size:10px; letter-spacing:1.5px; }
  .fhSkipPill.on { border-color:#8CE7A0; color:#8CE7A0; }
  .fmm { position:relative; width:100%; max-width:300px; aspect-ratio:1 / 1; margin:8px auto; transition:max-width var(--t-slow, .4s) var(--ease, ease); }
  .fmm.small { max-width:120px; } .fmm i { position:absolute; width:6%; height:6%; transform:translate(-50%, -50%); border-radius:50%; background:var(--c); border:1px solid #05070C; }
  .fmm i.tgt { width:9%; height:9%; box-shadow:0 0 0 3px #FF6B5A; z-index:1; } .fmm.ring i.tgt { animation:fmmRing .7s ease-in-out infinite; }
  .fmm i.won { box-shadow:0 0 0 3px var(--c); animation:fmmWon .5s ease-in-out 4; }
  @keyframes fmmRing { 50% { box-shadow:0 0 0 10px #FF6B5A55, 0 0 22px #FF6B5A; } }
  @keyframes fmmWon { 50% { box-shadow:0 0 0 10px var(--c), 0 0 26px var(--c); transform:translate(-50%, -50%) scale(1.35); } }
  .fhpbox.ringOnly .fhcols, .fhpbox.ringOnly .fhmsg, .fhpbox.ringOnly .gc { display:none; }
  body.calm #fhPlay *, body.calm #fhPlay *::before { animation-play-state:running !important; }  /* the finale never goes calm */
  body.calm *, body.calm *::before, body.calm *::after { animation-play-state:paused !important; }  /* 3.1-dev.118 calm: nothing loops while nobody's looking */
  #tour { position:fixed; inset:0; z-index:60; pointer-events:none; }
  #tourSpot { position:fixed; pointer-events:none; box-shadow:0 0 0 3000px rgba(3,5,9,.78), 0 0 0 3px #5FD4FF, 0 0 24px #5FD4FF; transition:left .3s var(--ease), top .3s var(--ease), width .3s var(--ease), height .3s var(--ease); }
  #tourSpot.full { left:0; top:0; width:0; height:0; box-shadow:0 0 0 3000px rgba(3,5,9,.86); }
  #tourBub { position:fixed; left:max(12px, env(safe-area-inset-left)); right:max(12px, env(safe-area-inset-right)); pointer-events:auto; background:#0C1118; border:1px solid #5FD4FF; border-radius:14px; padding:12px; box-shadow:0 12px 40px rgba(0,0,0,.6); text-align:left; }
  .tbTop { display:flex; gap:10px; } .tbTop > div { display:flex; flex-direction:column; gap:4px; }
  .tbTop b { font-family:var(--display); font-size:11px; letter-spacing:2px; color:#5FD4FF; } .tbTop i { font-size:13px; color:var(--dim); line-height:1.4; } .tbTop span { font-size:15px; line-height:1.45; }
  .tbBot { display:flex; align-items:center; gap:8px; margin-top:10px; } .tbBot small { flex:1; font-family:var(--display); font-size:10px; letter-spacing:1.5px; color:#5C6F84; }
  .tbBot button { width:auto; min-height:38px; margin:0; padding:0 14px; border-radius:999px; font-family:var(--display); font-size:11px; letter-spacing:2px; border:1px solid #5FD4FF; background:#081A26; color:#5FD4FF; }
  .tbBot button.ghost { border-color:var(--line); background:none; color:var(--dim); }
  .fgCard { text-align:left; border:1px solid var(--fc); border-radius:14px; padding:12px; margin:10px 0; background:#0C1118; }
  .fgHead { display:flex; align-items:center; gap:10px; } .fgHead b { font-family:var(--display); letter-spacing:2px; color:var(--fc); }
  .fgCard p { margin:8px 0; font-size:14px; line-height:1.45; } .fgRow { display:flex; gap:10px; margin:4px 0; font-size:13px; }
  .fgRow span { flex:none; width:76px; font-family:var(--display); font-size:10px; letter-spacing:1.5px; color:var(--dim); padding-top:2px; } .fgRow b { font-weight:600; }
  .fgTalk { margin-top:8px; padding:8px 10px; border-radius:10px; background:#141A24; font-size:13px; font-style:italic; }
  .negWho { width:auto; min-height:0; margin:6px auto 0; padding:5px 12px; border-radius:999px; border:1px solid var(--line); background:none; color:var(--dim); font-family:var(--display); font-size:10px; letter-spacing:1.5px; }
  .rvGam { background:#120D18; border:2px solid #B48CFF; border-radius:14px; padding:12px; margin:14px 0 8px; box-shadow:0 0 24px #B48CFF33; }
  .rvWho { display:flex; flex-wrap:wrap; justify-content:center; gap:8px; margin:12px 0 4px; } .rvWho span { min-width:64px; padding:6px 10px; border-radius:999px; border:1px solid; font-family:var(--display); font-size:11px; letter-spacing:1px; }
  .loHead { display:flex; align-items:center; justify-content:space-between; gap:10px; margin-top:10px; }
  .loCard { display:grid; grid-template-columns:54px 1fr 54px; gap:8px; align-items:center; border:1px dashed var(--line); border-radius:14px; padding:10px; margin:10px 0 0; background:var(--surface,#0C1118); }
  .loCard.on { border-style:solid; border-color:#FF6B5A; } .loCard.dead { opacity:.45; grid-template-columns:54px 1fr; }
  .loTg { margin:0; min-height:52px; border-radius:12px; border:1px solid var(--line); background:#141A24; color:var(--text); text-align:left; padding:6px 12px; font:inherit; }
  .loTg b { display:block; font-family:var(--display); letter-spacing:1.5px; font-size:14px; } .loTg small { display:block; font-size:11px; margin-top:2px; }
  .loCard .fhally { grid-column:1 / -1; justify-content:center; margin:0; }
  .loCard .fhally button { width:30px; height:30px; min-height:0; margin:0; padding:0; font-size:16px; }
  .loRow { grid-column:1 / -1; display:grid; grid-template-columns:repeat(2, minmax(0, 1fr)); gap:8px; }
  .loHire { display:flex; align-items:center; gap:6px; background:#141A24; border-radius:10px; padding:6px 8px; } .loHire span { flex:1; text-align:left; }
  .loHire span b { display:block; font-family:var(--display); font-size:10px; letter-spacing:1px; } .loHire span small { font-size:11px; color:#FFD166; }
  .loHire button { width:30px; height:30px; min-height:0; margin:0; padding:0; border-radius:50%; border:1px solid var(--line); background:#0C1118; color:var(--text); font-size:16px; }
  .loHire button.plus { border-color:#FFD166; color:#FFD166; } .loHire > b { font-family:var(--display); min-width:14px; text-align:center; }
  .loGam { margin:0; min-height:0; text-align:left; padding:6px 8px; border-radius:10px; border:1px dashed var(--line); background:none; color:var(--text); font:inherit; }
  .loGam b { display:block; font-family:var(--display); font-size:10px; letter-spacing:1px; color:var(--dim); } .loGam small { font-size:11px; color:var(--dim); }
  .loGam.on { border:1px solid #B48CFF; background:#120D18; } .loGam.on b { color:#B48CFF; }
  .loSaved { display:flex; align-items:center; gap:10px; text-align:left; background:#120D18; border:1px solid #B48CFF66; border-radius:14px; padding:10px 12px; margin:10px 0 0; }
  .loSaved b { display:block; font-family:var(--display); font-size:11px; letter-spacing:1px; color:#B48CFF; } .loSaved small { font-size:12px; color:var(--dim); }
  .loCardIcon { flex:none; width:28px; height:38px; border-radius:5px; background:#2A2140; border:1px solid #B48CFF; }
  .fhpin { display:block; width:100%; margin:4px 0 0; padding:8px; font:inherit; font-size:12px; letter-spacing:1px; color:#FFD166; background:none; border:1px dashed #FFD16688; border-radius:10px; }
  .fhpin.on { border-style:solid; background:#1B1608; }
  .fhgrp { font-family:var(--display); font-size:11px; letter-spacing:2px; color:var(--dim); margin:10px 0 4px; text-align:left; }
  .fhworlds, .fhpick { display:flex; flex-wrap:wrap; gap:6px; justify-content:center; }
  .fhworlds button, .fhpick button { font:inherit; font-size:13px; color:var(--text); background:var(--surface,#0C1118); border:1px solid var(--line); border-radius:10px; padding:7px 10px; min-width:30%; }
  .fhworlds button.on, .fhpick button.on { border-color:var(--accent); box-shadow:0 0 0 1px var(--accent); }
  .fhworlds small { display:block; font-size:10px; } .fhpick small { color:var(--dim); font-size:12px; }
  .fhw { border:1px solid var(--line); border-radius:12px; padding:8px 10px; margin:6px 0; text-align:left; }
  .fhwh { display:flex; justify-content:space-between; align-items:baseline; margin-bottom:4px; } .fhwh b { font-family:var(--display); letter-spacing:2px; font-size:13px; } .fhwh span { font-size:11px; letter-spacing:1px; }
  .fhchip { display:inline-block; margin:2px 4px 2px 0; border:1px solid var(--c); color:var(--c); border-radius:999px; padding:3px 9px; font-size:11px; font-weight:700; letter-spacing:1px; }
  .fhnote { font-size:11px; color:#FFD166; line-height:1.4; margin-top:4px; }
  #fhPlay { position:fixed; inset:0; z-index:60; background:radial-gradient(circle at 50% 25%,#20090C 0,#05070C 62%); overflow-y:auto; display:flex; justify-content:center; }
  .fhpbox { width:100%; max-width:440px; padding:20px 16px 40px; box-sizing:border-box; }
  .fhcols { display:flex; gap:8px; justify-content:center; margin:14px 0 8px; }
  .fhcol { flex:1; max-width:120px; border:1px solid var(--c); color:var(--c); border-radius:12px; padding:8px 4px; text-align:center; background:var(--surface,#0C1118); transition:opacity var(--t-mid) var(--ease); }
  .fhcol svg { display:block; margin:0 auto; }
  .fhcol.def { border-width:2px; } .fhcol.out { opacity:.35; }
  .fhcol small { display:block; font-family:var(--display); font-size:10px; letter-spacing:2px; margin-bottom:4px; min-height:24px; }
  .fhcol .die { display:flex; align-items:center; justify-content:center; width:44px; height:44px; margin:6px auto 0; border-radius:10px; background:var(--c); }
  .fhcol .die:empty { background:none; border:1px dashed var(--line); }
  .fhmsg { text-align:center; font-size:14px; line-height:1.5; min-height:66px; margin:6px 0; }
  .fhskip { display:block; margin:18px auto 0; font:inherit; font-size:11px; letter-spacing:2px; color:var(--dim); background:none; border:1px solid var(--line); border-radius:999px; padding:8px 14px; }
  .fhres { display:flex; justify-content:space-between; border-bottom:1px solid var(--line); padding:8px 2px; font-size:14px; } .fhres b { font-family:var(--display); letter-spacing:2px; }
  .fhpl { font-size:13px; line-height:1.5; text-align:left; margin:4px 0; }
  /* 3.1-dev.46 THE CLARITY PASS: the readout strip + RV-3 "Rivet" */
  .rdo { position:relative; margin:10px 0 4px; padding:8px 12px 10px; min-height:48px; border:1px solid var(--line); border-radius:10px; background:#081018; text-align:left;
         font-family:ui-monospace, 'SFMono-Regular', Menlo, Consolas, monospace; font-size:11px; line-height:1.55; display:flex; flex-direction:column; gap:5px; }
  .rdL, .rdFx { display:flex; gap:8px; min-width:0; }
  .rdK { flex:none; width:74px; color:#5C6F84; letter-spacing:1px; }
  .rdV { min-width:0; color:var(--text); display:flex; flex-wrap:wrap; align-items:center; gap:6px; }
  .rdFx > div { flex:1; min-width:0; display:flex; flex-direction:column; gap:3px; }
  .rdR { display:grid; grid-template-columns:7px 1fr auto; gap:6px; align-items:center; }
  .rdR span { color:var(--text); } .rdR b { text-align:right; letter-spacing:.5px; }
  .rdR i { width:6px; height:6px; border-radius:50%; background:currentColor; }
  .rdR .good { color:#8CE7A0; } .rdR .bad { color:#FF6B5A; } .rdR .warn { color:#FFB347; }
  .rdBar { display:inline-grid; grid-template-columns:repeat(5, 16px); gap:2px; }
  .rdBar i { height:7px; border-radius:2px; background:#1A2532; } .rdBar i.on { background:var(--fc); opacity:.85; }
  .rdo:has(> .rvBtn) { padding-right:50px; }
  .rvBtn { position:absolute; right:7px; top:50%; margin-top:-17px; width:34px; height:34px; min-height:0; margin:0; padding:0; border-radius:50%; border:1.5px solid #5FD4FF; background:#141A24;
           display:flex; align-items:center; justify-content:center; }
  .rvBtn::after { content:""; position:absolute; top:2px; right:2px; width:7px; height:7px; border-radius:50%; background:#FFD166; box-shadow:0 0 6px #FFD166; }
  .rvBtn.read::after { display:none; }
  .rvSay { margin:14px 0 6px; display:flex; gap:10px; align-items:flex-start; text-align:left; background:#0B1620; border:1px solid #5FD4FF66; border-radius:14px; padding:10px 12px;
           font-size:13px; line-height:1.5; color:var(--text); animation:rvIn var(--t-base) var(--ease); }
  .rvSay[hidden] { display:none; }
  .rvSay svg { flex:none; }
  .rvSay > div { min-width:0; }
  .rvSay b { display:block; font-family:var(--display); font-size:10px; letter-spacing:2px; color:#5FD4FF; margin-bottom:2px; }
  .rvSay i { color:var(--dim); }
  .rvAct { margin:8px 0 0; min-height:0; padding:5px 12px; font-family:var(--display); font-size:11px; letter-spacing:2px; color:#FFD166; border:1px solid #FFD166; border-radius:999px; background:none; }
  @keyframes rvIn { from { opacity:0; transform:translateY(-6px); } to { opacity:1; transform:none; } }
  .rvAsk { display:flex; justify-content:flex-end; margin:6px 0 0; }
  .rvAsk .rvBtn { position:relative; right:auto; top:auto; margin:0; width:32px; height:32px; }
  .rvAsk + .rvSay { margin-top:6px; }
  .biLine .rvBtn { float:right; position:relative; right:auto; top:auto; width:32px; height:32px; margin:0 0 4px 8px; }
  .beatItem .rvSay { clear:both; }
  #txRead { margin:6px 0 0; max-width:100%; } #txRead .rdo { margin-right:6px; }
  .mkCard.shut { background:repeating-linear-gradient(135deg, #2A2210 0 8px, #15120A 8px 16px) !important; border:2px solid #5A4A1E !important; position:relative; }
  .mkStencil { align-self:stretch; text-align:center; font-family:ui-monospace, Menlo, Consolas, monospace; font-weight:700; font-size:11px; letter-spacing:2px; color:#FFB347;
               background:#0B0906; border:1px solid #FFB347; border-radius:4px; padding:3px; }
  .mkCard .rvSay { margin:6px 0 0; font-size:12px; }
  .escort small { display:block; font-size:10px; line-height:1.3; color:var(--hc, var(--dim)); max-width:120px; margin:2px auto 0; }
  .escort.why { border:1.5px dashed var(--hc); border-radius:10px; padding:5px 6px; }
  .fcwin { position:relative; margin:6px 0 0; font-family:ui-monospace, Menlo, Consolas, monospace; font-size:10px; letter-spacing:1px; color:var(--dim);
           display:flex; gap:10px; justify-content:center; flex-wrap:wrap; padding:2px 36px 6px; }
  .fcwin .rvBtn { right:0; width:30px; height:30px; margin-top:-15px; }
  .fcwin .up { color:#8CE7A0; font-weight:700; } .fcwin .dn { color:#FF6B5A; font-weight:700; }
  .caughtHolo { display:grid; grid-template-columns:1fr 1fr; gap:6px; } .caughtHolo .ghHolo { min-width:0; }
  .rbar .ctk { position:absolute; top:-6px; width:2px; height:4px; margin-left:-1px; background:#FFD166; opacity:.8; }  /* 3.1-dev.71: a notch above, not a second marker */
  .nt .ntHead { white-space:normal; line-height:1.5; }
  .firstTog { display:flex; align-items:center; gap:10px; width:100%; max-width:360px; margin:10px auto 0; padding:10px 12px; border-radius:14px; border:1px dashed #5FD4FF88; background:#0C1118; color:var(--text); font:inherit; text-align:left; }
  .firstTog.on { border:1px solid #5FD4FF; box-shadow:0 0 14px rgba(95,212,255,.2); }
  .firstTog span { flex:1; } .firstTog b { display:block; font-family:var(--display); font-size:12px; letter-spacing:2px; color:#5FD4FF; } .firstTog small { font-size:12px; color:var(--dim); }
  .firstTog i { font-style:normal; font-family:var(--display); font-size:11px; letter-spacing:2px; color:var(--dim); } .firstTog.on i { color:#5FD4FF; }
  .introRv { display:flex; gap:12px; align-items:flex-start; text-align:left; margin:4px 0 6px; } .introRv b { display:block; font-family:var(--display); font-size:11px; letter-spacing:3px; color:#5FD4FF; margin-bottom:4px; }
  .introRv span { font-size:16px; line-height:1.45; } .introRv > svg { flex:none; width:44px; height:44px; }
  .feedHist.bRep { display:flex; flex-direction:row; align-items:center; gap:8px; border:1px solid var(--line); background:#0C1118; width:100%; min-height:0; margin:4px 0 0; font:inherit; color:inherit; text-align:left; cursor:pointer; }
  .feedHist.bRep span { flex:1; text-align:left; } .feedHist.bRep b { white-space:nowrap; } .bRep em { margin-left:auto; font-style:normal; color:#FF6B5A; }
  .rpl { margin:10px 0 6px; } .rpLine { text-align:center; font-size:14px; min-height:20px; color:var(--text); }
  .rpSteps { display:flex; flex-wrap:wrap; gap:4px; justify-content:center; margin:8px 0; } .rpSteps i { width:10px; height:10px; border-radius:3px; background:#233142; }
  .rpSteps i.a { background:#8CE7A0; } .rpSteps i.d { background:#FF6B5A; }
  /* 3.1-dev.77 THE BLACK MARKET: a smuggler's den -- hologram blue + warm amber, no faction colour */
  .bmDoor { position:relative; overflow:hidden; display:block; width:100%; height:132px; margin:8px 0 0; padding:0; border:1px solid #E8A03C; border-radius:14px; text-align:left; color:#ECE6DA; font:inherit;
            background:radial-gradient(70% 120% at 18% 50%, rgba(79,227,208,.20), transparent 70%), linear-gradient(180deg, #0E1414, #080B0B); box-shadow:0 0 22px rgba(232,160,60,.25); }
  .bmDoor::after, .bm::before { content:''; position:absolute; inset:0; pointer-events:none; background:repeating-linear-gradient(0deg, rgba(255,255,255,.035) 0 1px, transparent 1px 3px); }
  .hz { position:absolute; left:0; right:0; height:7px; background:repeating-linear-gradient(135deg, #E8A03C 0 7px, #141410 7px 14px); } .hz.t { top:0; } .hz.b { bottom:0; }
  .bmDoor .sweep { position:absolute; left:0; right:0; top:0; height:22px; background:linear-gradient(180deg, transparent, rgba(79,227,208,.18), transparent); animation:bmSweep 2.6s linear infinite; }
  @keyframes bmSweep { 0% { transform:translateY(-30px); } 100% { transform:translateY(140px); } }
  .bmHolo { position:absolute; left:10px; top:12px; width:56px; height:60px; display:flex; align-items:flex-end; justify-content:center; }
  .bmHolo img { width:48px; height:56px; image-rendering:pixelated; filter:drop-shadow(0 0 7px rgba(79,227,208,.8)); animation:bmFlick 3.2s steps(1) infinite; }
  @keyframes bmFlick { 0%, 46%, 100% { opacity:.95; } 48% { opacity:.45; } 50% { opacity:.9; } 72% { opacity:.7; } 74% { opacity:.95; } }
  .bmDoorTx { position:absolute; left:74px; right:14px; top:16px; display:flex; flex-direction:column; gap:3px; }
  .bmDoorTx small { display:flex; align-items:center; gap:6px; font-family:ui-monospace, Menlo, Consolas, monospace; font-size:10px; letter-spacing:1px; color:#E8A03C; }
  .blink { display:inline-block; width:7px; height:7px; border-radius:50%; background:#E8A03C; box-shadow:0 0 8px #E8A03C; animation:bmBlink 1.2s steps(1) infinite; } @keyframes bmBlink { 0%, 60% { opacity:1; } 61%, 100% { opacity:.15; } }
  .bmDoorTx > span { display:flex; align-items:baseline; gap:8px; } .bmDoorTx b { font-family:var(--display); font-size:20px; letter-spacing:3px; color:#E8A03C; text-shadow:0 0 10px rgba(232,160,60,.5); }
  .bmDoorTx em { margin-left:auto; font-style:normal; font-family:var(--display); font-size:12px; letter-spacing:2px; color:#4FE3D0; }
  .bmTiles { position:absolute; left:10px; right:10px; bottom:14px; display:grid; grid-template-columns:repeat(4, minmax(0, 1fr)); gap:6px; }
  .bmTiles span { border:1px solid #3A3A2A; border-radius:6px; background:#121410; padding:5px 2px; text-align:center; font-family:var(--display); font-size:8px; letter-spacing:1px; line-height:1.3; color:#ECE6DA; }
  .bm { position:relative; text-align:left; color:#ECE6DA; margin:0 -4px; padding:4px; border-radius:14px; background:radial-gradient(90% 40% at 50% 8%, rgba(79,227,208,.16), transparent 70%), radial-gradient(120% 50% at 50% 100%, rgba(232,160,60,.12), transparent 70%); }
  .bmTop { display:flex; justify-content:space-between; gap:8px; font-family:ui-monospace, Menlo, Consolas, monospace; font-size:9px; letter-spacing:1px; color:#7E8A86; }
  .bmTop span { display:flex; align-items:center; gap:6px; }
  .bmHead { position:relative; display:flex; gap:12px; align-items:center; margin:8px 0; border:1px solid #1E2A2A; border-radius:14px; padding:10px; background:rgba(8,14,14,.7); }
  .bmHead .bmHolo { position:relative; left:auto; top:auto; flex:none; width:96px; height:120px; } .bmHead .bmHolo img { width:96px; height:120px; filter:drop-shadow(0 0 10px rgba(232,160,60,.25)); animation:none; }
  .bmHead b { display:block; font-family:var(--display); font-size:21px; letter-spacing:4px; color:#E8A03C; text-shadow:0 0 10px rgba(232,160,60,.55); }
  .bmHead q { display:block; font-family:ui-monospace, Menlo, Consolas, monospace; font-size:11px; line-height:1.5; color:#9FD9D0; margin:4px 0; } .bmHead small { font-family:ui-monospace, Menlo, Consolas, monospace; font-size:9px; color:#7E8A86; }
  .bmGrid { display:grid; grid-template-columns:repeat(2, minmax(0, 1fr)); gap:10px; }
  .bmCrate { position:relative; display:flex; flex-direction:column; gap:5px; border:1px solid #2E3A36; border-radius:8px; padding:12px 10px 10px; background:linear-gradient(180deg, rgba(20,24,22,.95), rgba(10,12,12,.95)); }
  .bmCrate .hz.t { border-radius:8px 8px 0 0; height:6px; opacity:.75; }
  .bmCode { display:flex; justify-content:space-between; gap:4px; font-family:ui-monospace, Menlo, Consolas, monospace; font-size:8px; letter-spacing:1px; color:#7E8A86; } .bmCode .tag { color:#E8A03C; }
  .bmCrate b { font-family:var(--display); font-size:13px; letter-spacing:1px; } .bmCrate small { flex:1; font-size:12px; line-height:1.35; color:#B5ADA0; }
  .bmCrate button { min-height:40px; margin:4px 0 0; padding:8px; border-radius:999px; border:1px solid #4FE3D0; background:rgba(79,227,208,.06); color:#4FE3D0; font-family:var(--display); font-size:12px; letter-spacing:2px; font-weight:700; }
  .bmCrate button:disabled { border-color:#2E3A36; color:#7E8A86; background:none; }
  .bmSays { text-align:center; font-family:ui-monospace, Menlo, Consolas, monospace; font-size:9px; color:#9A907F; margin:8px 0; }
  .bmPick { border:1px solid #E8A03C; border-radius:14px; padding:10px; margin:0 0 10px; display:flex; flex-wrap:wrap; gap:6px; } .bmPick b { width:100%; font-family:var(--display); font-size:12px; letter-spacing:2px; color:#E8A03C; }
  .bmPick button { min-height:40px; margin:0; padding:6px 12px; border-radius:999px; border:1px solid #4FE3D0; background:none; color:#4FE3D0; font-size:13px; }
  .bmShops { border-top:1px solid #1E2A2A; margin:10px 0 0; padding:8px 0 0; } .bmShops small { display:block; text-align:center; font-family:var(--display); font-size:10px; letter-spacing:3px; color:var(--dim); margin:0 0 6px; }
  .bmShops > div { display:grid; grid-template-columns:repeat(4, minmax(0, 1fr)); gap:6px; }
  .bmShops button { min-height:0; margin:0; padding:6px 3px; border-radius:8px; border:1px solid var(--sc); background:none; text-align:center; } .bmShops button.shut { border-style:dashed; opacity:.55; }
  .bmShops b { display:block; font-family:var(--display); font-size:9px; letter-spacing:1px; color:var(--sc); } .bmShops span { font-size:10px; color:#C9D2DC; }
  .stSrc.bmS img { width:14px; height:16px; border-radius:0; image-rendering:pixelated; }
  .chk { border:1px solid #5FD4FF66; border-radius:14px; padding:8px 10px 10px; margin:0 0 10px; background:#0A121A; text-align:left; }
  .chkH { display:flex; align-items:center; gap:8px; margin-bottom:4px; } .chkH b { font-family:var(--display); font-size:12px; letter-spacing:2px; color:#5FD4FF; } .chkH small { margin-left:auto; font-size:10px; color:var(--dim); letter-spacing:1px; }
  .chkH .ptx { margin-left:6px; } .chkH small { margin-left:auto; }
  .chkPill { display:flex; align-items:center; gap:8px; width:auto; min-height:0; margin:0 auto 10px; padding:6px 14px; border-radius:999px; border:1px solid #5FD4FF66; background:#0A121A; color:var(--text); font:inherit; }
  .chkPill b { font-family:var(--display); font-size:11px; letter-spacing:2px; color:#5FD4FF; } .chkPill span { font-size:12px; color:var(--dim); } .chkPill em { font-style:normal; color:#5FD4FF; }
  .chkR { display:flex; align-items:center; gap:10px; width:100%; min-height:0; margin:4px 0 0; padding:6px 4px; border:none; background:none; color:var(--text); font:inherit; text-align:left; }
  .chkR i { flex:none; width:20px; height:20px; border-radius:6px; border:1.5px solid #5FD4FF; display:flex; align-items:center; justify-content:center; font-style:normal; font-size:13px; color:#05070C; }
  .chkR.done i { background:#8CE7A0; border-color:#8CE7A0; } .chkR.done b { color:var(--dim); text-decoration:line-through; }
  .chkR b { display:block; font-family:var(--display); font-size:12px; letter-spacing:1px; } .chkR small { font-size:12px; color:var(--dim); }
  .bmSec { font-family:var(--display); font-size:12px; letter-spacing:3px; color:#4FE3D0; margin:12px 2px 6px; } .bmSec.fh { color:#E8A03C; }
  .bmSec small { display:block; font-family:inherit; font-size:10px; letter-spacing:1px; color:#7E8A86; margin-top:2px; }
  #bmHint { position:fixed; left:50%; bottom:calc(env(safe-area-inset-bottom, 0px) + 14px); transform:translateX(-50%); z-index:30; display:flex; align-items:center; gap:8px; min-height:0; margin:0; padding:8px 14px;
            border-radius:999px; border:1px solid #E8A03C; background:rgba(8,11,11,.94); color:#E8A03C; font-family:var(--display); font-size:11px; letter-spacing:2px; box-shadow:0 0 16px rgba(232,160,60,.35); transition:opacity var(--t-base) var(--ease), transform var(--t-base) var(--ease); }
  #bmHint.hide { opacity:0; pointer-events:none; transform:translate(-50%, 10px); }
  .fileBox { border:1px solid #C8364A66; border-radius:8px; padding:8px 10px; text-align:left; background:#120A0D; font-family:ui-monospace, Menlo, Consolas, monospace; font-size:11px; }
  .fileBox div { display:flex; gap:10px; padding:3px 0; } .fileBox span { width:78px; flex:none; color:#8A6A72; letter-spacing:1px; } .fileBox b { font-weight:400; color:#F0D8DE; }
  .srow .bty { font-style:normal; font-family:var(--display); font-size:9px; letter-spacing:1px; color:#FFD166; border:1px solid #FFD16666; border-radius:999px; padding:2px 6px; margin:0 6px; white-space:nowrap; }
  @media (prefers-reduced-motion: reduce) { .bmDoor .sweep, .bmHolo img, .blink { animation:none; } }
  .stats > .stat { margin:0; min-height:0; height:100%; width:100%; box-sizing:border-box; border-radius:12px; }  /* 3.1-dev.108: the PLANETS button sized like the other two */
  .wfGrid { display:grid; grid-template-columns:repeat(2, minmax(0, 1fr)); gap:10px; margin:10px 0 8px; }
  .wfT { background:#0C1118; border:1px solid var(--line); border-radius:14px; padding:10px 12px; text-align:left; display:flex; flex-direction:column; gap:6px; }
  .wfT > span { font-family:var(--display); font-size:11px; letter-spacing:2px; color:var(--dim); } .wfT > b { font-family:var(--display); font-size:19px; letter-spacing:1px; }
  .wfT > b small { font-size:12px; color:var(--dim); } .wfT > small { font-size:12px; color:var(--dim); line-height:1.35; }
  .wfBars { display:flex; gap:4px; } .wfBars i { flex:1; height:6px; border-radius:3px; }
  .wfLine { display:flex; align-items:baseline; gap:12px; background:#0C1118; border:1px solid var(--line); border-radius:12px; padding:9px 12px; margin:0 0 8px; text-align:left; }
  .wfLine span { flex:none; width:84px; font-family:var(--display); font-size:11px; letter-spacing:2px; color:var(--dim); } .wfLine b { font-weight:600; font-size:14px; } .wfLine small { color:var(--dim); font-weight:400; }
  .wfNbH { font-family:var(--display); font-size:11px; letter-spacing:2px; color:var(--dim); margin:12px 0 6px; }
  .wfNb { display:flex; flex-wrap:wrap; justify-content:center; gap:8px; } .wfNb button { width:auto; min-height:40px; margin:0; display:inline-flex; align-items:center; gap:8px; padding:0 14px; border-radius:999px; border:1px solid var(--line); background:#141A24; color:var(--text); font-family:var(--display); font-size:12px; letter-spacing:1px; }
  .wfNb i { width:12px; height:12px; border-radius:50%; flex:none; }
  .wfPager { display:flex; justify-content:center; gap:5px; margin:16px 0 4px; } .wfPager i { width:6px; height:6px; border-radius:3px; background:#233142; } .wfPager i.on { width:18px; background:var(--text); }
  .wfHint { font-size:12px; color:#5C6F84; text-align:center; margin-bottom:6px; }
  .audList { display:flex; flex-direction:column; gap:8px; margin:10px 0; } .audRow { margin:0; min-height:0; padding:10px 12px; border-radius:14px; border:1px solid var(--line); background:#141A24; color:var(--text); text-align:left; }
  .audTop { display:flex; align-items:center; gap:8px; margin-bottom:8px; } .audTop b { font-family:var(--display); letter-spacing:2px; font-size:13px; } .audTop span { margin-left:auto; font-family:var(--display); font-size:11px; letter-spacing:1.5px; }
  .dfHead { display:flex; align-items:center; justify-content:space-between; gap:10px; text-align:left; } .dfHead .sheetTitle, .dfHead .sheetSub { text-align:left; }
  .dfCr { flex:none; display:flex; flex-direction:column; align-items:flex-end; background:#141A24; border:1px solid #FFD16666; border-radius:12px; padding:6px 10px; }
  .dfCr small { font-family:var(--display); font-size:10px; letter-spacing:1.5px; color:var(--dim); } .dfCr b { font-family:var(--display); font-size:22px; color:#FFD166; }
  .stat.defbox { position:relative; font:inherit; color:inherit; cursor:pointer; padding:8px 10px; } .stat.defbox.on { border:2px solid #5FD4FF; box-shadow:0 0 18px rgba(95,212,255,.25); }
  .stat.defbox .defgo { display:flex; align-items:center; justify-content:center; gap:4px; font-family:var(--display); font-size:10px; letter-spacing:1.5px; color:#5FD4FF; }
  .stat.defbox .defdot { position:absolute; top:6px; right:6px; width:9px; height:9px; border-radius:50%; background:#FFB347; box-shadow:0 0 8px #FFB347; animation:defPulse 1.6s ease-in-out infinite; }
  @keyframes defPulse { 50% { opacity:.35; } }
  .dfRow { border:1px solid var(--line); border-radius:14px; padding:10px 12px; margin:8px 0 0; background:#141A24; text-align:left; }
  .dfRow.weak { border-color:#FFB347; }
  .dfTop { display:flex; align-items:baseline; gap:8px; } .dfTop b { font-family:var(--display); font-size:16px; letter-spacing:1px; }
  .dfTop small { font-family:var(--display); font-size:10px; letter-spacing:1.5px; color:var(--dim); } .dfTop em { margin-left:auto; font-style:normal; font-family:var(--display); font-size:10px; letter-spacing:1.5px; }
  .dfMid { display:flex; align-items:center; gap:10px; margin:8px 0; }
  .dfMid button { flex:none; width:44px; height:44px; min-height:0; margin:0; padding:0; border-radius:50%; border:1px solid var(--line); background:#0C1118; color:var(--dim); font-size:22px; }
  .dfMid button.plus { border-color:#5FD4FF; color:#5FD4FF; } .dfMid button:disabled { opacity:.35; }
  .dfSlots { flex:1; display:flex; flex-wrap:wrap; gap:4px; justify-content:center; }
  .dfSlots i { width:12px; height:16px; border-radius:3px; border:1px dashed #233142; } .dfSlots i.h { background:#8FA3B8; border:1px solid #8FA3B8; } .dfSlots i.n { background:#5FD4FF; border:1px solid #5FD4FF; }
  .dfBot { display:flex; align-items:center; gap:8px; font-size:13px; color:var(--dim); } .dfBot span { flex:1; }
  .dfPat { min-height:0; margin:0; padding:6px 10px; border-radius:999px; border:1px dashed var(--dim); background:none; color:var(--dim); font-family:var(--display); font-size:10px; letter-spacing:1.5px; }
  .dfPat.on { border:1px solid #8CE7A0; color:#8CE7A0; } .dfPat:disabled { opacity:.35; }
  .dfTotal { display:flex; justify-content:space-between; margin:12px 2px 0; font-family:var(--display); font-size:12px; letter-spacing:1.5px; color:var(--dim); } .dfTotal b { color:#FFD166; }
  .pill.dfGo { border:none; background:#5FD4FF; color:#05070C; } .pill.dfGo:disabled { background:none; border:1px solid var(--line); color:var(--dim); }
  .kitHead { display:flex; align-items:center; gap:14px; text-align:left; margin:2px 0 4px; }
  .kitHead img { width:64px; height:74px; border-radius:50%; border:3px solid var(--fc); object-fit:cover; object-position:50% 20%; background:#141A24; flex:none; }
  .kitHead b { display:block; font-family:var(--display); font-size:19px; letter-spacing:1px; } .kitHead span { font-family:var(--display); font-size:12px; letter-spacing:2px; } .kitHead i { font-style:normal; }
  .kitGrp { font-family:var(--display); font-size:11px; letter-spacing:3px; text-align:left; margin:12px 0 2px; }
  .kitRow { display:flex; align-items:center; gap:10px; text-align:left; border:1px solid var(--line); border-radius:8px; padding:8px 10px; margin:4px 0 0; background:#0A0F16; }
  .kitRow > span:not(.kitSrc) { flex:1; min-width:0; } .kitRow b { display:block; font-family:var(--display); font-size:13px; letter-spacing:1px; }
  .kitRow small { display:block; font-size:12px; color:var(--dim); line-height:1.35; } .kitRow em { font-style:normal; font-family:var(--display); font-size:9px; letter-spacing:1.5px; color:var(--faint); white-space:nowrap; }
  .kitSrc { width:26px; height:26px; flex:none; border-radius:50%; border:1px solid var(--kc); display:flex; align-items:center; justify-content:center; }
  .kitNote { font-size:12px; color:var(--faint); margin:12px 0 0; }
  .srow.tap { cursor:pointer; } .srow .srk { font-style:normal; color:var(--faint); margin-left:6px; }
  .stSrc { display:inline-flex; vertical-align:-3px; margin-right:6px; } .stSrc img { width:16px; height:16px; image-rendering:pixelated; border-radius:50%; }
  #ntHeld { position:fixed; top:calc(env(safe-area-inset-top, 0px) + 8px); left:50%; transform:translateX(-50%); z-index:60; pointer-events:none; padding:4px 12px; border-radius:999px;
            border:1px solid var(--line); background:rgba(12,17,24,.92); color:var(--dim); font-family:var(--display); font-size:10px; letter-spacing:2px; }
  .ptag { display:inline-flex; align-items:center; gap:4px; vertical-align:-3px; padding:0 7px 0 1px; border-radius:999px; border:1px solid var(--pc);
          background:color-mix(in srgb, var(--pc) 14%, transparent); line-height:1.3; white-space:nowrap; letter-spacing:normal; }
  .ptag img { width:15px; height:17px; border-radius:50%; object-fit:cover; object-position:50% 20%; background:#0C1118; }
  .ptag b { color:var(--pc); font-weight:700; font-size:.92em; }
  .watchQ { text-align:left; font-size:15px; line-height:1.4; margin:8px 0; }
  .watchR { text-align:left; font-size:13px; color:var(--dim); border:1px solid var(--line); border-radius:10px; padding:7px 10px; margin:4px 0; }
  .galNote { font-size:12px; line-height:1.4; border:1px dashed var(--line); border-radius:10px; padding:7px 10px; margin:6px 0; text-align:left; }
  .galNote.bad { border-color:#FF6B5A88; } .galNote.good { border-color:#8CE7A088; }
  .gy { margin:8px 0 0; }
  .gyhead { align-items:center; gap:8px; } .gyhead small { margin-left:auto; }
  .gyhelp { flex:none; width:28px; height:28px; min-height:0; margin:0; padding:0; border-radius:50%; border:1px solid #5FD4FF88; background:#141A24; display:flex; align-items:center; justify-content:center; }
  .gymore { display:block; width:100%; margin:6px 0 0; min-height:0; padding:8px; border-radius:10px; border:1px dashed var(--line); background:none; color:var(--dim); font-family:var(--display); font-size:11px; letter-spacing:2px; }
  .gyrow { box-sizing:border-box; display:flex; align-items:center; gap:8px; width:100%; margin:4px 0 0; padding:8px 10px; text-align:left; font:inherit; font-size:13px; color:var(--text);
           background:var(--surface,#0C1118); border:1px solid var(--line); border-radius:8px; cursor:pointer; }
  .gyrow span { flex:1; min-width:0; line-height:1.35; }
  .gyrow b { font-family:var(--display); letter-spacing:2px; font-size:11px; margin-right:4px; }
  .gyrow em { font-style:normal; font-size:11px; letter-spacing:2px; color:var(--dim); }
  .gyrow.spy span { white-space:normal; }
  .gyrow.bad { border-color:#FF6B5A88; } .gyrow.bad b { color:#FF6B5A; }
  .gyrow.due { border-color:#FFD166; } .gyrow.due em { color:#FFD166; }
  .mktile.shut, .stall.shut { border-style:dashed !important; opacity:.6; }
  .stbuy.shut { border-style:dashed; color:var(--dim); }
  .fstall { border:1px solid #2A1C10; border-left:3px solid var(--fc); border-radius:12px; padding:6px; margin:0 0 8px;
            background:repeating-linear-gradient(90deg, #0C0805 0 3px, #140D08 3px 42px); }
  .fstall.shut { border-style:dashed; opacity:.6; }
  .fsthead { display:flex; align-items:center; gap:6px; font-family:var(--display); font-size:12px; letter-spacing:2px; margin:0 2px 4px; }
  .fsthead b { color:var(--fc); }
  .fsthead span { margin-left:auto; font-size:10px; }
  .fstitems { display:grid; grid-template-columns:repeat(2, minmax(0, 1fr)); gap:6px; }
  .fstitems.three { grid-template-columns:repeat(3, minmax(0, 1fr)); }
  .fstall .stall { padding:2px; border:none; background:none; }
  .fstall .stall .stbuy { min-height:0; padding:7px 0; font-size:13px; margin:4px 0 0; }
  .fstall .stall .shelfsvg { height:52px; width:100%; }
  .fstall .stall .stfx { flex:none; font-size:11px; line-height:1.3; min-height:29px; margin:2px 2px 0; }
  .fstall .stall { align-self:start; }
  .pickgrid { display:grid; grid-template-columns:repeat(2, minmax(0, 1fr)); gap:6px; margin-top:10px; }
  .pickgrid button { margin:0; padding:10px 6px; font-size:13px; display:flex; flex-direction:column; align-items:center; gap:2px; }
  .pickgrid small { font-size:10px; letter-spacing:1px; }
  .sheetTitle { font-family:var(--display); font-size:18px; letter-spacing:3px; color:#FFD166; }
  .sheetSub { font-size:13px; color:var(--dim); margin-top:4px; }
  .debtnote { display:flex; gap:8px; align-items:flex-start; text-align:left; font-size:13px; color:var(--dim); margin:8px 0; padding:8px 10px; border:1px solid #FFD16655; border-radius:8px; }
  .debtnote b { color:#FFD166; }
  .qbar { height:8px; border-radius:4px; background:#1A2433; overflow:hidden; margin:6px 0 2px; }
  .qbar i { display:block; height:100%; background:#FFD166; transition:width var(--t-slow) var(--ease); }
  .kit small { font-size:11px; color:var(--dim); }
  .cstart { font-size:12px; color:var(--dim); margin-top:8px; line-height:1.4; }
  .cpitch { text-align:center; font-family:var(--display); font-size:15px; font-weight:700; letter-spacing:.5px; color:#FFD166; margin:4px 0 0; }
  .cpitch small { display:block; margin-top:3px; font-size:11px; letter-spacing:2px; color:var(--dim); font-weight:500; } .cpitch small b { color:#FFD166; }
  /* 3.1-dev.3 Feed + bell */
  .pix { display:block; image-rendering:pixelated; }
  .pn { display:inline-flex; align-items:flex-end; justify-content:center; vertical-align:middle; padding:4px 0 2px; }
  .stat .pn { display:inline-flex; font-size:inherit; letter-spacing:0; color:inherit; padding:6px 0 2px; }
  .stat b { display:inline-flex; align-items:flex-end; justify-content:center; }
  .pn.bump { animation: pnBump var(--t-base) var(--ease); }
  @keyframes pnBump { 0% { transform: scale(1.18); } 100% { transform: scale(1); } }
  .topR { display:flex; align-items:center; gap:10px; }
  .bell { position:relative; width:36px; height:36px; min-height:0; padding:0; margin:0; border-radius:50%; display:flex; align-items:center; justify-content:center; color:var(--text); }
  .bellN { position:absolute; top:-4px; right:-4px; min-width:18px; height:18px; padding:0 4px; box-sizing:border-box; border-radius:9px; background:var(--red); color:#fff; font-size:11px; font-weight:700; line-height:18px; letter-spacing:0; }
  .feedTop { display:flex; align-items:center; justify-content:space-between; margin:0 0 12px; }
  .feedBack { width:44px; height:44px; min-height:0; padding:0; margin:0; border-radius:50%; font-size:22px; }
  .feedTitle { font-size:20px; font-weight:700; letter-spacing:4px; }
  .feedTabs { display:grid; grid-template-columns:1fr 1fr; gap:4px; padding:4px; border-radius:var(--r-md); background:var(--panel); border:1px solid var(--line); margin-bottom:12px; }
  .feedTabs button { margin:0; min-height:40px; padding:8px; border:none; border-radius:10px; background:transparent; color:var(--dim); font-weight:700; letter-spacing:2px; font-size:14px; }
  .feedTabs button.on { background:#FFD166; color:#1A1206; }
  .feedRow { border-radius:var(--r-md); border:1px solid var(--line); background:var(--panel); padding:10px 14px; margin:8px 0; text-align:center; opacity:.72; }
  .feedRow.unread { border:2px solid var(--rc); opacity:1; }
  .frHead { font-size:12px; letter-spacing:3px; font-weight:700; color:var(--rc); }
  .frLine { font-size:15px; margin-top:3px; }
  .frRound { font-size:11px; color:var(--faint); letter-spacing:2px; margin-top:4px; }
  .feedRoundHead { font-size:11px; letter-spacing:3px; color:var(--dim); margin:14px 0 4px; }
  .feedHist { display:flex; flex-direction:column; align-items:center; gap:2px; padding:8px 10px; border-radius:var(--r-sm); font-size:14px; }
  .feedHist b { font-size:11px; letter-spacing:2px; }
  .feedHist.you { background:#1A1406; border:1px solid #5A4A1C; }
  .feedEmpty { color:var(--dim); font-size:14px; margin-top:30px; }
  /* 3.1 layers. SHEET = every decision: springs up from the bottom, the screen
     behind settles back and dims. NOTIFICATION = news mid-round: drops in from
     the top, stacks, fades away. Both animate transform + opacity only. */
  #app { transition: transform var(--t-slow) var(--ease), opacity var(--t-slow) var(--ease); transform-origin: 50% 30%; }
  body.sheetOn #app { transform: scale(.95); opacity: .55; }
  #lyrSheet { position:fixed; inset:0; z-index:40; pointer-events:none; }
  #lyrSheet .scrim { position:absolute; inset:0; background:rgba(0,0,0,.5); opacity:0; transition: opacity var(--t-slow) var(--ease); }
  #lyrSheet .sheetBox { position:absolute; left:0; right:0; bottom:0; max-height:86vh; overflow:auto; box-sizing:border-box;
    background:#0C1118; border-top:2px solid var(--sheetCol, var(--accent)); border-radius:22px 22px 0 0;
    padding:10px 16px calc(20px + env(safe-area-inset-bottom)); transform:translateY(105%);
    transition: transform var(--t-slow) var(--ease); box-shadow:0 -12px 40px rgba(0,0,0,.6); text-align:center; }
  #lyrSheet .grab { width:44px; height:5px; border-radius:3px; background:#2A3442; margin:0 auto 12px; }
  #lyrSheet.on { pointer-events:auto; }
  #lyrSheet.on .scrim { opacity:1; }
  #lyrSheet.on .sheetBox { transform:none; }
  #eraFrame { position:fixed; inset:0; z-index:30; pointer-events:none; opacity:0; transition:opacity var(--t-slow) var(--ease);
              box-shadow:inset 0 0 0 2px var(--ec), inset 0 0 36px -6px var(--ec); }
  #eraFrame.on { opacity:.85; }
  .eraBan { display:flex; justify-content:space-between; align-items:center; width:100%; margin:0 0 10px; padding:8px 12px; border:1px solid var(--ec); border-radius:12px;
            background:rgba(255,255,255,.03); color:var(--text); font:inherit; cursor:pointer; min-height:0; }
  .eraBan b { font-family:var(--display); letter-spacing:3px; font-size:13px; color:var(--ec); }
  .eraBan div { text-align:left; } .eraBan small { display:block; font-size:11px; color:var(--dim); margin-top:2px; }
  .eraBan span { font-size:11px; letter-spacing:2px; color:var(--dim); }
  .eraRule { text-align:left; border:1px solid var(--line); border-radius:10px; padding:8px 10px; margin:6px 0; font-size:13px; line-height:1.4; }
  .eraRule b { display:block; font-family:var(--display); font-size:11px; letter-spacing:2px; color:var(--text); margin-bottom:2px; }
  .eraRule.sting b { color:#FF6B5A; } .eraRule i { color:var(--dim); font-style:normal; }
  .eraYou { margin-top:8px; font-size:13px; text-align:left; }
  .decreeBtn { border-color:var(--ec) !important; color:var(--ec) !important; margin:-2px 0 10px; }
  .gyrow.decree { border-color:#FFD166; } .gyrow.decree b { color:#FFD166; }
  .gyrow.lie b { color:#E0475B; }
  .liePick { display:flex; flex-direction:column; gap:6px; margin:10px 0; }
  .liePick button { margin:0; min-height:0; padding:10px; font-size:13px; text-align:left; }
  #lieText { width:100%; box-sizing:border-box; font-size:16px; background:#05070C; color:var(--text); border:1px solid var(--line); border-radius:10px; padding:8px; }
  #lyrAnn { position:fixed; inset:0; z-index:70; display:flex; align-items:center; justify-content:center; padding:16px; box-sizing:border-box;
            background:rgba(5,7,12,.94); opacity:0; pointer-events:none; transition:opacity var(--t-base) var(--ease); }
  #lyrAnn.on { opacity:1; pointer-events:auto; }
  .annCard { width:100%; max-width:360px; text-align:center; transform:scale(.94) translateY(8px); transition:transform var(--t-slow) var(--ease); }
  #lyrAnn.on .annCard { transform:none; }
  .strk { position:relative; display:flex; align-items:center; justify-content:space-between; min-height:110px; margin:6px 0 10px; }
  .strkFoe { display:flex; flex-direction:column; gap:6px; transform:translateX(-160%); opacity:0; transition:transform 520ms var(--ease), opacity 300ms var(--ease); }
  .strk.go .strkFoe { transform:none; opacity:1; }
  .strkFoe img { width:46px; image-rendering:pixelated; }
  .strkBolt { position:absolute; left:56px; right:132px; top:50%; height:3px; border-radius:2px; background:var(--ac); box-shadow:0 0 12px var(--ac); transform-origin:left center; transform:scaleX(0); opacity:0; }
  .strkBolt.fire { animation:strkBolt 320ms var(--ease); }
  @keyframes strkBolt { 0% { transform:scaleX(0); opacity:1; } 55% { transform:scaleX(1); opacity:1; } 100% { transform:scaleX(1); opacity:0; } }
  .strkMe { width:132px; text-align:center; }
  .strkMe.hit { animation:strkShake 260ms linear; }
  @keyframes strkShake { 25% { transform:translateX(5px); } 75% { transform:translateX(-5px); } }
  .strkNum .pix { margin:0 auto; }
  .strkLbl { font-family:var(--display); font-size:10px; letter-spacing:2px; color:var(--dim); margin-bottom:4px; }
  .strkPips { display:flex; flex-wrap:wrap; justify-content:center; gap:4px; margin-top:8px; }
  .strkPips i { width:11px; height:11px; border-radius:2px; background:#8FA3B8; transition:opacity 260ms var(--ease), transform 260ms var(--ease); }
  .strkPips i.cr { background:#FFD166; border-radius:50%; }
  .strkPips img { width:28px; image-rendering:pixelated; transform:scaleX(-1); transition:opacity 260ms var(--ease), transform 260ms var(--ease); }
  .strkPips > .gone { opacity:0; transform:scale(.3); }
  .strkPips img.gone { transform:scaleX(-1) scale(.3); }
  .annCard.anim .annHead, .annCard.anim .annLine, .annCard.anim .annYou, .annCard.anim .annRd, .annCard.anim .annTap, .annCard.anim .annKick { opacity:0; transition:opacity var(--t-base) var(--ease); }
  .annCard.anim.done .annHead, .annCard.anim.done .annLine, .annCard.anim.done .annYou, .annCard.anim.done .annRd, .annCard.anim.done .annTap, .annCard.anim.done .annKick { opacity:1; }
  .warScene { display:flex; align-items:center; justify-content:space-between; margin:6px 0 12px; }
  .warScene > div:not(.wsDice) { width:110px; text-align:center; }
  .warScene small, .dieScene small { display:block; font-family:var(--display); font-size:10px; letter-spacing:2px; color:var(--dim); margin-bottom:6px; }
  .warScene .pix, .dieScene .pix { margin:0 auto; }
  .wsDice { display:flex; gap:10px; }
  .die { width:44px; height:44px; border-radius:10px; background:#E8EEF5; display:flex; align-items:center; justify-content:center; opacity:.75; transition:transform var(--t-fast) var(--ease), opacity var(--t-fast); }
  .die.win { opacity:1; transform:scale(1.12); box-shadow:0 0 14px var(--ac); }
  .die.big { width:84px; height:84px; border-radius:18px; margin:0 auto; }
  .dieScene { margin:6px 0 14px; text-align:center; }
  .warRow { display:flex; align-items:center; gap:8px; margin:6px 0; text-align:left; }
  .warRow span { flex:1; font-size:14px; } .warRow small { display:block; font-size:11px; color:var(--dim); }
  .warRow button { margin:0; min-height:0; width:40px; height:40px; padding:0; font-size:20px; }
  .warRow b { width:28px; text-align:center; font-family:var(--display); font-size:18px; }
  .warRow b.keep { color:var(--dim); margin-right:14px; } .warRow b.keep.thin { color:#FFB347; }
  .warHead { display:flex; font-family:var(--display); font-size:10px; letter-spacing:2px; color:var(--dim); margin:10px 0 0; }
  .warHead span:first-child { flex:1; text-align:left; } .warHead span:nth-child(2) { width:42px; text-align:center; } .warHead span:last-child { width:124px; text-align:center; }
  .wtl { margin:10px 0; text-align:left; }
  .wtl > div { display:flex; gap:10px; padding:7px 0; opacity:.6; }
  .wtl > div.on, .wtl > div.done { opacity:1; }
  .wtl i { flex:none; width:24px; height:24px; border-radius:50%; border:2px solid #FF6B5A; color:#FF6B5A; font-style:normal; font-family:var(--display); font-size:12px; display:flex; align-items:center; justify-content:center; }
  .wtl > div.on i { background:#FF6B5A; color:#05070C; } .wtl > div.done i { border-color:var(--dim); color:var(--dim); }
  .wtl span { font-size:13px; line-height:1.4; } .wtl b { font-family:var(--display); letter-spacing:2px; font-size:12px; margin-right:6px; }
  .wtl em { display:block; font-style:normal; font-size:10px; letter-spacing:1.5px; color:var(--dim); }
  .wtl > div.on b { color:#FF6B5A; }
  .warTot { display:flex; align-items:center; justify-content:center; gap:10px; margin:12px 0 6px; font-size:13px; color:var(--dim); }
  .pickgrid button.on { border-color:#5FD4FF; color:#5FD4FF; background:rgba(95,212,255,.08); }
  .pickgrid button small { color:var(--dim); }
  .gyrow.move { border-color:#FFD166; } .gyrow.move b { color:#FFD166; }
  .gyrow.cover b { color:#C8364A; }
  .cvList { display:block; font-size:11px; color:#8CE7A0; margin-top:3px; line-height:1.45; } .cvList i { font-style:normal; color:#FFB347; }
  .story { text-align:left; font-size:14px; line-height:1.45; }
  .story p { margin:4px 0; }
  .storyK { font-family:var(--display); font-size:11px; letter-spacing:3px; color:var(--dim); margin-bottom:4px; }
  .pclass { display:grid; grid-template-columns:repeat(2, minmax(0, 1fr)); gap:6px; }
  .pclass div { border:1px solid var(--line); border-radius:8px; padding:8px; text-align:center; }
  .pclass b { display:block; font-family:var(--display); letter-spacing:2px; font-size:12px; } .pclass small { font-size:11px; color:var(--dim); }
  .pnews { border:1px solid #5FD4FF; border-radius:12px; padding:8px 10px; margin:4px 0; text-align:left; }
  .pnews.you { border-color:#FFD166; } .pnews b { display:block; font-family:var(--display); letter-spacing:2px; font-size:12px; } .pnews span { font-size:13px; color:var(--dim); }
  .annCard.beat { max-height:calc(100vh - 32px); overflow-y:auto; text-align:center; }
  #lyrAnn:has(.annCard.beat) { align-items:flex-start; }
  .beatSub { font-size:13px; color:var(--dim); margin:-4px 0 12px; }
  .beatItem { text-align:left; border:1px solid var(--line); border-left:3px solid var(--ac); border-radius:14px; padding:10px 12px; margin:8px 0; background:#0C1118; }
  .biTop { display:flex; justify-content:space-between; gap:8px; align-items:baseline; }
  .biTop b { font-family:var(--display); font-size:13px; letter-spacing:2px; color:var(--ac); }
  .biTop span { font-size:9px; letter-spacing:2px; color:var(--dim); white-space:nowrap; }
  .biLine { font-size:14px; margin-top:4px; line-height:1.4; display:flow-root; }
  .biYou { font-size:12px; color:var(--dim); margin-top:4px; line-height:1.35; }
  .biOnly { margin-top:8px; padding:8px 10px; border-radius:10px; background:rgba(255,255,255,.04); font-size:13px; }
  .biOnly b { display:block; font-family:var(--display); font-size:10px; letter-spacing:2px; color:#FFD166; }
  .biAct { display:flex; flex-wrap:wrap; gap:6px; margin-top:8px; }
  .biAct button { margin:0; min-height:0; padding:8px 12px; font-size:12px; flex:1; }
  .biAct button:first-child { border-color:#FFD166; color:#FFD166; }
  .ghHolo { display:flex; align-items:center; gap:10px; margin:8px 0 2px; padding:8px; border-radius:12px; border:1px solid var(--hc); background:radial-gradient(circle at 20% 50%, rgba(95,212,255,.16), transparent 70%); position:relative; overflow:hidden; }
  .ghHolo::after { content:""; position:absolute; inset:0; background:repeating-linear-gradient(0deg, rgba(160,220,255,.08) 0 2px, transparent 2px 5px); pointer-events:none; }
  .ghHolo img { width:48px; height:56px; flex:none; animation:ghFlick 2.4s steps(1) infinite; }
  @keyframes ghFlick { 0%, 92% { opacity:1; } 94% { opacity:.4; } 96% { opacity:1; } }
  .ghLine { font-size:13px; font-style:italic; color:#CFE8FF; overflow:hidden; }
  .ghLine span { display:inline-block; animation:ghType 1.6s steps(40) both; clip-path:inset(0 0 0 0); }
  @keyframes ghType { from { clip-path:inset(0 100% 0 0); } to { clip-path:inset(0 0 0 0); } }
  .biNote { font-size:11px; letter-spacing:1px; color:var(--dim); margin-top:6px; }
  /* 3.1-dev.42 MAKE AMENDS: the deeds on the grudge sheet */
  .amendHead { font-family:var(--display); font-size:13px; letter-spacing:3px; color:#8CE7A0; margin:12px 0 6px; }
  .amendHead small { display:block; font-family:inherit; letter-spacing:0; font-size:12px; color:var(--dim); margin-top:2px; }
  .amend { border:1px solid var(--line); border-radius:12px; padding:10px; margin-bottom:8px; background:var(--surface, #0C1118); }
  .amend.off { opacity:.45; }
  .amTop { display:flex; justify-content:space-between; align-items:baseline; gap:8px; }
  .amTop b { font-family:var(--display); letter-spacing:2px; font-size:13px; }
  .amTop span { font-size:10px; letter-spacing:2px; color:var(--dim); }
  .amLine { font-size:13px; line-height:1.4; color:#B8C4D2; margin-top:4px; }
  .amend .biAct button:first-child { border-color:#8CE7A0; color:#8CE7A0; }
  .amWorlds button, .amend .amWorlds button:first-child { flex:1 1 40%; border-color:#8CE7A0; color:#8CE7A0; }
  .biAct button.ghost { border-color:var(--line); color:var(--dim); }
  .beatGo { margin-top:12px; border-color:#5FD4FF !important; color:#5FD4FF !important; }
  .feedRow.tap { cursor:pointer; }
  .fvPrev { text-align:left; margin:10px 0 0; padding:10px 12px; border:1px solid var(--line); border-radius:14px; background:#0C1118; }
  .fvPrev > b { display:block; font-family:var(--display); font-size:10px; letter-spacing:2px; color:var(--dim); margin-bottom:6px; }
  .fvPrev div { display:flex; gap:10px; font-size:13px; margin:4px 0; line-height:1.35; }
  .fvPrev i { flex:none; width:78px; font-style:normal; font-family:var(--display); font-size:11px; letter-spacing:2px; color:var(--text); padding-top:1px; }
  .fvPrev .up { color:#8CE7A0; font-weight:700; } .fvPrev .dn { color:#FF6B5A; font-weight:700; }
  .fvPrev small { font-size:11px; color:var(--dim); }
  .annKick { font-family:var(--display); font-size:11px; letter-spacing:4px; color:var(--dim); margin-top:10px; }
  .annHead { font-family:var(--display); font-size:26px; font-weight:700; letter-spacing:3px; color:var(--ac); margin:8px 0 10px; line-height:1.15; }
  .annLine { font-size:15px; color:var(--text); line-height:1.45; }
  .annYou { margin-top:16px; padding:10px 12px; border:1px solid var(--ac); border-radius:14px; font-size:13px; color:var(--text); text-align:left; line-height:1.4; }
  .annYou b { display:block; font-family:var(--display); font-size:10px; letter-spacing:3px; color:var(--ac); margin-bottom:4px; }
  .annTap { margin-top:24px; font-size:11px; letter-spacing:4px; color:var(--faint); }
  .gyrow.strike { border-color:#FF6B5A; background:rgba(255,107,90,.08); } .gyrow.strike b { color:#FF6B5A; }
  /* 3.1-dev.88 GALAXY AND YOU, one line a row (user: "don't want the bars to be too tall"): a dot for what needs you */
  .gy .gyrow { min-height:44px; padding:6px 10px; }
  .gy .gyrow span, .gy .gyrow.spy span { white-space:nowrap; overflow:hidden; text-overflow:ellipsis; }
  .gy .gyrow .cvList { display:none; }
  .gyrow.urg::before, .gyrow.act::before { content:''; flex:none; width:9px; height:9px; border-radius:50%; }
  .gyrow.urg::before { background:#FF6B5A; animation:gyPing 1.4s ease-out infinite; }
  .gyrow.act::before { background:#FFB347; animation:gyBlink 1.3s steps(1) infinite; }
  .gyrow.urg { border-color:#FF6B5A; animation:gyGlow 1.8s ease-in-out infinite; }
  .gyrow em.btn { flex:none; background:#FFD166; color:#05070C; border-radius:999px; padding:5px 10px; font-weight:700; letter-spacing:1px; }
  .gyrow.urg em.btn { background:#FF6B5A; }
  @keyframes gyPing { 0% { box-shadow:0 0 0 0 rgba(255,107,90,.7); } 100% { box-shadow:0 0 0 8px rgba(255,107,90,0); } }
  @keyframes gyBlink { 0%, 55% { opacity:1; } 56%, 100% { opacity:.2; } }
  @keyframes gyGlow { 0%, 100% { box-shadow:0 0 0 rgba(255,107,90,0); } 50% { box-shadow:0 0 14px rgba(255,107,90,.4); } }
  .gyhead .gyn { color:#FF6B5A; display:inline-flex; align-items:center; gap:6px; letter-spacing:1.5px; }
  .gyhead .gyn::before { content:''; width:7px; height:7px; border-radius:50%; background:#FF6B5A; animation:gyBlink 1.3s steps(1) infinite; }
  .sitHead { display:flex; align-items:center; gap:10px; margin:14px 0 4px; font-family:var(--display); font-size:13px; letter-spacing:4px; font-weight:700; color:#FFD166; }
  .sitHead::before, .sitHead::after { content:''; flex:1; height:1px; background:var(--line); }
  /* an Era never changes a number silently: its chip, in its colour */
  .etag { display:inline-block; vertical-align:middle; margin-left:5px; padding:1px 6px; border-radius:999px; border:1px solid var(--ec); color:var(--ec); background:rgba(255,255,255,.04);
          font-family:var(--display); font-size:9px; letter-spacing:1.5px; font-weight:700; white-space:nowrap; line-height:1.5; }
  .fc { display:flex; flex-direction:column; margin:4px 0 10px; }
  .fc small { font-family:var(--display); font-size:10px; letter-spacing:2.5px; color:var(--dim); padding:8px 0 2px; } .fc small.up { color:#8CE7A0; }
  .fcRow { display:flex; align-items:center; gap:10px; padding:6px 0; border-top:1px solid var(--line); font-size:13px; }
  .fcRow span { text-align:left; }
  .fcRow i { width:7px; height:7px; border-radius:50%; flex:none; } .fcRow span { flex:1; } .fcRow b { font-family:var(--display); white-space:nowrap; }
  .fcRow.up i { background:#8CE7A0; } .fcRow.up b { color:#8CE7A0; } .fcRow.dn i { background:#FF6B5A; } .fcRow.dn b { color:#FF6B5A; }
  .pressQ { display:flex; gap:12px; align-items:center; margin:6px 0 10px; } .pressQ q { font-size:17px; line-height:1.3; font-weight:500; quotes:'\201C' '\201D'; }
  .bcWorlds { grid-template-columns:repeat(3, minmax(0, 1fr)) !important; gap:6px !important; } .bcWorlds button { min-height:0 !important; padding:7px 4px !important; font-size:12px !important; }
  .bcWorlds button small { display:none; }
  .inkChip { --ic:#FFB347; border:1px dashed var(--ic); background:var(--raised, #141A24); border-radius:14px; padding:12px 14px; margin:10px 0; text-align:left; }
  .inkChip > b { display:inline-block; font-family:var(--display); letter-spacing:2px; font-size:12px; color:#05070C; background:var(--ic); border-radius:999px; padding:3px 10px; margin-bottom:6px; }

  /* 3.1-dev.103 THE COCKPIT */
  body.deckOn #app { padding-bottom: calc(150px + env(safe-area-inset-bottom, 0px)); position: relative; z-index: 1; }
  #deck { display: none; }
  body.deckOn #deck { display: block; position: fixed; left: 0; right: 0; bottom: 0; z-index: 35; max-width: 520px; margin: 0 auto; --dk: #FFC933; }  /* under the sheets (40) */
  .dash { height: calc(132px + env(safe-area-inset-bottom, 0px)) !important; }  /* 3.1-dev.109: the plate runs under the home bar -- no black strip */
  .dkLock { width: 100% !important; margin: 14px 0 4px !important; }
  #holoFx { position: fixed; inset: 0; z-index: 36; pointer-events: none; opacity: 0;
    background: repeating-linear-gradient(0deg, rgba(95,212,255,.16) 0 2px, transparent 2px 5px), linear-gradient(180deg, rgba(95,212,255,.10), rgba(95,212,255,0) 60%); }
  #holoFx.go { animation: holoFx 340ms ease-out; }
  @keyframes holoFx { 0% { opacity: 1; transform: translateY(-8px); } 25% { opacity: .35; } 45% { opacity: .9; transform: translateY(4px); } 70% { opacity: .25; } 100% { opacity: 0; transform: none; } }
  @media (prefers-reduced-motion: reduce) { #holoFx.go { animation: none; } }
  #deck.fac0 { --dk: #FFC933; } #deck.fac1 { --dk: #B9C3CF; } #deck.fac2 { --dk: #B48CFF; } #deck.fac3 { --dk: #E0475B; }
  .dkLock { position: relative; overflow: hidden; display: block; width: calc(100% - 32px); margin: 0 16px 6px; min-height: 52px; border-radius: 999px; border: 2px solid #5FD4FF; background: #0C1118; color: #5FD4FF; font: 700 15px var(--display); letter-spacing: 4px; touch-action: none; user-select: none; -webkit-user-select: none; }
  .dkLock small { display: block; font: 500 11px var(--body, sans-serif); letter-spacing: 0; color: var(--dim); }
  .dkLock span { position: relative; z-index: 1; }
  .dkLock .fill { position: absolute; left: 0; top: 0; bottom: 0; width: 0; background: #5FD4FF33; transition: width 0s; }
  .dkLock.holding .fill { width: 100%; transition: width 900ms linear; }
  .dkLock.fired { box-shadow: 0 0 18px #5FD4FF; }
  .dkLock.done { border-color: var(--line); color: var(--dim); }
  .dash { position: relative; height: 132px; }
  .dashBg { position: absolute; left: 0; top: 0; width: 100%; height: 100%; }
  .dashBg .plate { fill: #121925; stroke: #33445C; stroke-width: 1.5; }
  .dashBg .trim { fill: none; stroke: var(--dk); stroke-opacity: .65; stroke-width: 2; }
  .dashBg circle { fill: #5C6F84; }
  .dashBg .hexEdge { fill: none; stroke: #3A4A60; stroke-width: 4; stroke-linejoin: round; } .dashBg .hexEdge.on { stroke: var(--dk); }
  #deck.fac0 .dashBg .trim { stroke-dasharray: 10 6; }                       /* Rebels: worn hazard striping */
  #deck.fac1 .dashBg .plate { fill: #141B27; stroke: #8A96A6; }              /* Empire: polished grey panels */
  #deck.fac2 .dashBg .trim { stroke-dasharray: 2 5; stroke-width: 3; }       /* Pykes: riveted plating */
  #deck.fac3 .dashBg .plate { fill: #160C10; stroke: #6A2030; }             /* Crimson Dawn: dark red, a blade edge */
  .dk { position: absolute; display: flex; flex-direction: column; align-items: center; justify-content: center; gap: 2px; padding: 4px 0; border-radius: 10px; border: 1px solid #233142; background: #0C1118; color: #5C6F84; transition: transform 90ms ease; }
  .dk b { font: 700 9px var(--display); letter-spacing: 1px; } .dk small { font: 500 8px var(--display); letter-spacing: 1px; color: #5C6F84; }
  .dk svg, .dk canvas { width: 24px; height: 24px; image-rendering: pixelated; }
  /* 3.1-dev.109 (user: "I WANT the hex button to raise"): the angled cockpit row with the raised hex BRIDGE, the hex pinned
     to the exact center so the dash LEDs sit right under it */
  .dk-docks { left: 4%; top: 64px; width: 16%; height: 56px; transform: perspective(300px) rotateY(18deg); }
  .dk-map { left: 21.5%; top: 48px; width: 17%; height: 62px; transform: perspective(300px) rotateY(9deg); }
  .dk-bridge { left: calc(50% - 42px); top: 12px; width: 84px; height: 88px; border: none !important; background: transparent !important; box-shadow: none !important; isolation: isolate; }
  .dk-bridge::before, .dk-bridge::after { content: ''; position: absolute; clip-path: polygon(50% 0, 100% 22%, 100% 78%, 50% 100%, 0 78%, 0 22%); z-index: -1; }
  .dk-bridge::before { inset: 0; background: #3A4A60; }
  .dk-bridge::after { inset: 3px; background: #0C1118; }
  .dk-bridge.on::before { background: var(--dk); filter: drop-shadow(0 0 6px var(--dk)); }
  .dk-bridge.on::after { background: repeating-linear-gradient(0deg, color-mix(in srgb, var(--dk) 18%, #0C1118) 0 2px, color-mix(in srgb, var(--dk) 7%, #0C1118) 2px 4px); }
  .dk-bridge .lamp { top: 18px; right: 16px; }
  .dk-comms { left: 61.5%; top: 48px; width: 17%; height: 62px; transform: perspective(300px) rotateY(-9deg); }
  .dk-war { left: 80%; top: 64px; width: 16%; height: 56px; transform: perspective(300px) rotateY(-18deg); }
  .dk:active { filter: brightness(1.3); } .dk-docks:active { transform: perspective(300px) rotateY(18deg) translateY(2px) scale(.95); }
  .dk-map:active { transform: perspective(300px) rotateY(9deg) translateY(2px) scale(.95); } .dk-bridge:active { transform: translateY(2px) scale(.95); }
  .dk-comms:active { transform: perspective(300px) rotateY(-9deg) translateY(2px) scale(.95); } .dk-war:active { transform: perspective(300px) rotateY(-18deg) translateY(2px) scale(.95); }
  .dk.on { color: var(--dk); border-color: var(--dk); background: repeating-linear-gradient(0deg, color-mix(in srgb, var(--dk) 16%, transparent) 0 2px, color-mix(in srgb, var(--dk) 6%, transparent) 2px 4px); box-shadow: 0 0 14px color-mix(in srgb, var(--dk) 40%, transparent), inset 0 0 10px color-mix(in srgb, var(--dk) 20%, transparent); }
  .dk.on small { color: var(--dk); opacity: .7; }
  .dk .lamp { position: absolute; top: 5px; right: 6px; width: 7px; height: 7px; border-radius: 50%; background: var(--lc); box-shadow: 0 0 6px var(--lc); animation: lampBlink 1.4s ease-in-out infinite; }
  /* (dev.108: the BRIDGE key is a normal key now) */
  @keyframes lampBlink { 50% { opacity: .35; } }
  .dk .lamp.steady { animation: none; box-shadow: none; opacity: .8; }
  .led { position: absolute; bottom: 14px; width: 7px; height: 4px; border-radius: 1px; background: #2A3646; }
  .leds { position: absolute; left: 0; right: 0; top: calc(100% + 5px); display: flex; justify-content: center; gap: 6px; pointer-events: none; }  /* 3.1-dev.110: part of the BRIDGE key, under its point */
  .leds .led { position: static !important; }
  .led.l1.blink { background: #8CE7A0; box-shadow: 0 0 6px #8CE7A0; animation: lampBlink 1s infinite; } .led.l2.alarm { background: #FF6B5A; box-shadow: 0 0 8px #FF6B5A; animation: lampBlink .5s infinite; }
  #canopy { display: none; } body.deckOn #canopy { display: block; position: fixed; inset: 0; z-index: 0; pointer-events: none; overflow: hidden; max-width: 520px; margin: 0 auto; }
  #canopy::before { content: ''; position: absolute; inset: -60px; opacity: .55;
    background-image: radial-gradient(1px 1px at 20px 30px, #E8EEF5, transparent), radial-gradient(1px 1px at 120px 80px, #8FA3B8, transparent),
      radial-gradient(1.5px 1.5px at 210px 150px, #E8EEF5, transparent), radial-gradient(1px 1px at 300px 40px, #8FA3B8, transparent),
      radial-gradient(1px 1px at 60px 190px, #5C6F84, transparent), radial-gradient(1px 1px at 260px 230px, #8FA3B8, transparent),
      radial-gradient(1.5px 1.5px at 160px 260px, #5FD4FF, transparent), radial-gradient(1px 1px at 340px 300px, #E8EEF5, transparent),
      radial-gradient(1px 1px at 90px 110px, #E8EEF5, transparent), radial-gradient(1px 1px at 180px 20px, #8FA3B8, transparent),
      radial-gradient(1px 1px at 240px 90px, #E8EEF5, transparent), radial-gradient(1.5px 1.5px at 30px 250px, #FFD166, transparent),
      radial-gradient(1px 1px at 330px 170px, #8FA3B8, transparent), radial-gradient(1px 1px at 110px 300px, #E8EEF5, transparent),
      radial-gradient(1px 1px at 200px 200px, #5C6F84, transparent), radial-gradient(1px 1px at 280px 270px, #E8EEF5, transparent),
      radial-gradient(1px 1px at 50px 70px, #8FA3B8, transparent), radial-gradient(1px 1px at 150px 160px, #E8EEF5, transparent),
      radial-gradient(1px 1px at 350px 110px, #E8EEF5, transparent), radial-gradient(1px 1px at 230px 310px, #8FA3B8, transparent);
    background-size: 330px 300px;  /* 3.1-dev.111: twice the stars (user: "could do with a few more") */ transform: translateX(var(--sx, 0px)); transition: transform 600ms cubic-bezier(.2,.8,.2,1); animation: starDrift 90s linear infinite; }
  @keyframes starDrift { from { background-position: 0 0; } to { background-position: 0 320px; } }
  @media (prefers-reduced-motion: reduce) { #canopy::before { animation: none; transition: none; } }
  #canopy .arc { position: absolute; left: -15%; right: -15%; top: -440px; height: 520px; border-radius: 50%; border-bottom: 2px solid #2E3D5288; }
  #canopy .sl, #canopy .sr { position: absolute; bottom: 170px; width: 3px; height: 70vh; background: linear-gradient(#2E3D5200, #2E3D52CC); transform-origin: bottom center; }
  #canopy .sl { left: 22px; transform: rotate(16deg); } #canopy .sr { right: 22px; transform: rotate(-16deg); }
  #canopy .glint { position: absolute; top: 40px; left: -40%; width: 30%; height: 6px; border-radius: 3px; background: linear-gradient(90deg, transparent, #ffffff18, transparent); transform: rotate(-8deg); animation: glint 11s ease-in-out infinite; }
  @keyframes glint { 0%, 85% { left: -40%; } 100% { left: 120%; } }
  #app.holo { animation: holo 200ms steps(4); }
  @keyframes holo { 0% { opacity: .25; filter: hue-rotate(60deg) brightness(1.6); transform: translateY(2px); } 50% { opacity: .7; } 100% { opacity: 1; filter: none; transform: none; } }
  @media (prefers-reduced-motion: reduce) { #canopy .glint, .dk .lamp, .led { animation: none !important; } #app.holo { animation: none; } }
  .liveNow { display: flex; align-items: center; gap: 10px; width: 100%; min-height: 48px; margin: 4px 0 10px; border-radius: 999px; border: 1px solid #FF6B5A; background: #FF6B5A1A; color: var(--text); padding: 0 14px; text-align: left; font-size: 13px; }
  .liveNow i { width: 10px; height: 10px; border-radius: 50%; background: #FF6B5A; box-shadow: 0 0 0 4px #FF6B5A33; animation: lampBlink 1s infinite; flex-shrink: 0; }
  .liveNow b { font: 700 11px var(--display); letter-spacing: 2px; color: #FF6B5A; white-space: nowrap; } .liveNow span { flex: 1; }
  .liveNow em { font: 700 12px var(--display); color: #FF6B5A; font-style: normal; white-space: nowrap; }
  .liveNow.talk { border-color: #5FD4FF; background: #5FD4FF14; } .liveNow.talk i { background: #5FD4FF; box-shadow: 0 0 0 4px #5FD4FF33; } .liveNow.talk b, .liveNow.talk em { color: #5FD4FF; }
  .commsHead { display: flex; flex-direction: column; align-items: center; gap: 2px; margin: 6px 0 12px; }
  .commsHead b { font: 700 18px var(--display); letter-spacing: 4px; color: var(--accent); }
  .commsHead span { font: 500 11px ui-monospace, Menlo, Consolas, monospace; color: #5FD4FF; }
  .prepList { background: #0C1118; border: 1px solid #FFD16655; border-radius: 14px; padding: 10px 12px; margin-bottom: 14px; text-align: left; }
  .plHead { display: flex; justify-content: space-between; align-items: baseline; margin-bottom: 6px; } .plHead b { font: 700 11px var(--display); letter-spacing: 3px; color: #FFD166; } .plHead span { font-size: 12px; color: var(--dim); }
  .plRow { display: flex; align-items: center; gap: 10px; width: 100%; min-height: 44px; border-radius: 8px; border: 1px solid #233142; background: #141A24; color: var(--text); padding: 0 10px; margin: 4px 0; text-align: left; font-size: 13px; }
  .plRow i { width: 22px; height: 22px; border-radius: 50%; border: 2px solid var(--pc); color: var(--pc); font: 700 11px var(--display); font-style: normal; display: flex; align-items: center; justify-content: center; flex-shrink: 0; }
  .plRow span { flex: 1; } .plRow span b { font-family: var(--display); letter-spacing: 1px; color: var(--pc); } .plRow em { color: var(--dim); font-style: normal; }
  .plRow.done { opacity: .5; } .plRow.done i { background: #8CE7A022; border-color: #8CE7A0; color: #8CE7A0; }
  .cgHead { display: flex; align-items: center; gap: 8px; margin: 12px 0 6px; } .cgHead i { width: 7px; height: 7px; border-radius: 50%; background: var(--gc); box-shadow: 0 0 6px var(--gc); }
  .cgHead b { font: 700 11px var(--display); letter-spacing: 3px; color: var(--gc); } .cgHead span { flex: 1; height: 1px; background: #233142; }
  .inkChip > span { display:block; font-size:14px; line-height:1.45; }
  .inkChip.dug { border-style:solid; border-color:#B48CFF; } .inkChip.dug > b { background:#B48CFF; }
  .headStart { border:1px solid #5FD4FF; border-radius:8px; padding:8px 12px; margin:8px 0; display:flex; gap:10px; align-items:center; text-align:left; }
  .headStart > b { font-family:var(--display); font-size:11px; letter-spacing:2px; color:#05070C; background:#5FD4FF; border-radius:999px; padding:3px 9px; white-space:nowrap; }
  .headStart > span { font-size:13px; }
  .beatOpt { display:flex; align-items:center; gap:12px; width:100%; min-height:64px; border-radius:14px; border:1px solid var(--oc); background:var(--raised, #141A24); color:var(--text); padding:10px 14px; margin:6px 0; text-align:left; }
  .beatOpt i { width:38px; height:38px; flex-shrink:0; border-radius:50%; border:2px solid var(--oc); color:var(--oc); font:700 15px var(--display); font-style:normal; display:flex; align-items:center; justify-content:center; }
  .beatOpt span { flex:1; display:flex; flex-direction:column; gap:2px; } .beatOpt b { font-family:var(--display); letter-spacing:2px; color:var(--oc); }
  .beatOpt small { font-size:13px; color:var(--dim); } .beatOpt em { font:700 12px var(--display); color:var(--oc); font-style:normal; white-space:nowrap; }
  .gfhFleet { display:flex; align-items:center; gap:10px; background:var(--raised, #141A24); border-radius:8px; padding:8px 10px; margin:6px 0; }
  .gfhFleet > span { font:500 11px var(--display); color:var(--dim); width:64px; text-align:left; } .gfhFleet > div { display:flex; gap:6px; flex-wrap:wrap; }
  .gfhShip { width:32px; height:32px; border-radius:8px; border:1px solid var(--line); background:#0C1118; padding:0; }
  .gfhShip.on { border:2px solid #B48CFF; background:#B48CFF33; }
  .gfhPrice { display:flex; align-items:center; justify-content:space-between; background:var(--raised, #141A24); border-radius:8px; padding:6px 6px 6px 12px; margin:10px 0; }
  .gfhPrice span { font:600 11px var(--display); letter-spacing:2px; color:var(--dim); }
  .gfhPrice button { width:44px; height:44px; border-radius:50%; border:1px solid var(--line); background:#0C1118; color:var(--text); font-size:20px; }
  .gfhPrice b { font:700 24px var(--display); color:#FFD166; min-width:72px; text-align:center; }
  .bcOpt { display:flex; align-items:center; gap:10px; width:100%; margin:5px 0 0; min-height:0; padding:9px 12px; text-align:left; font:inherit; font-size:14px;
           color:var(--text); background:none; border:1.5px solid var(--line); border-radius:12px; }
  .bcOpt i { width:14px; height:14px; border-radius:50%; border:2px solid #5C6F84; flex:none; } .bcOpt.on { border-color:#5FD4FF; background:rgba(95,212,255,.07); }
  .bcOpt.on i { border-color:#5FD4FF; background:radial-gradient(#5FD4FF 45%, transparent 50%); }
  .bcLab { font-family:var(--display); font-size:11px; letter-spacing:2.5px; color:var(--dim); margin:12px 0 2px; text-align:left; }
  .bcPay { display:flex; align-items:center; justify-content:center; gap:16px; margin:12px 0 4px; } .bcPay .bcLab { margin:0; }
  .bcPay button { width:40px; height:40px; min-height:0; padding:0; border-radius:50%; border:1.5px solid var(--line); background:none; color:var(--dim); font-size:20px; }
  .bcPay b { font-family:var(--display); font-size:32px; color:#FFD166; } .bcPay small { font-size:13px; color:var(--dim); }
  .bcFace { display:block; width:84px; height:84px; margin:8px auto; border-radius:50%; border:2px solid #5FD4FF; }
  .bcQuote { font-size:18px; line-height:1.35; font-weight:500; margin:8px 0 12px; }
  .bcTerms { display:flex; flex-direction:column; gap:4px; text-align:left; border:1px solid var(--line); border-radius:12px; padding:10px 12px; font-size:13px; color:#B8C6D4; margin-bottom:10px; }
  .bmHold { display:flex; flex-direction:column; gap:6px; margin:4px 0 8px; }
  .bmHi { display:flex; align-items:center; gap:10px; border:1px dashed #C9A22788; border-radius:12px; padding:8px 10px; }
  .bmHi b { font-family:var(--display); letter-spacing:2px; font-size:12px; color:#FFD166; white-space:nowrap; } .bmHi small { flex:1; font-size:12px; color:var(--dim); }
  .bmHi button { min-height:0; padding:6px 12px; border-radius:999px; } .bmHi em { font-style:normal; font-size:10px; letter-spacing:1px; color:var(--dim); white-space:nowrap; }
  .gyrow.job b { color:#FFD166; } .gyrow.race b { color:#5FD4FF; }
  #lyrNotif { position:fixed; left:12px; right:12px; top:calc(10px + env(safe-area-inset-top)); z-index:60; pointer-events:none;
    display:flex; flex-direction:column; align-items:stretch; gap:8px; }
  .nt { pointer-events:auto; border-radius:var(--r-md); background:#0C1118; border:2px solid var(--ntCol, var(--accent));
    padding:10px 14px; text-align:center; box-shadow:0 12px 30px rgba(0,0,0,.55);
    transform:translateY(-130%); opacity:0; transition: transform var(--t-slow) var(--ease), opacity var(--t-slow) var(--ease); }
  .nt.in { transform:none; opacity:1; }
  .nt.out { transform:translateY(-40%) scale(.96); opacity:0; }
  .nt.stamp { border-style:dashed; transform:translateY(-130%) rotate(-2deg); }
  .nt.stamp.in { transform:rotate(-2deg); animation: stampIn var(--t-base) var(--ease); }
  .nt.stamp .ntHead { font-size:15px; letter-spacing:3px; }
  @keyframes stampIn { 0% { transform:scale(1.25) rotate(-6deg); } 100% { transform:scale(1) rotate(-2deg); } }
  .nt .ntHead { font-size:12px; letter-spacing:3px; font-weight:700; color:var(--ntCol, var(--accent)); }
  .nt .ntLine { font-size:15px; margin-top:3px; color:var(--text); }
  @media (prefers-reduced-motion: reduce) { *, *::before, *::after { animation-duration: 1ms !important; transition-duration: 1ms !important; } }
  html.rm *, html.rm *::before, html.rm *::after { animation-duration: 1ms !important; transition-duration: 1ms !important; }
  html.rm #banner .hsw { display:none; } html.rm #banner .hsBody, html.rm .wsheet { animation:none; }
  /* 3.1-dev.67 KEYWORDS + LIVE BUTTONS (mockup https://claude.ai/artifact/4xywKvzesxnSWRXKHeP5vv; user: "I like idea 1 and 2") */
  .kw { border-bottom:1.5px dotted #FFD166AA; padding-bottom:1px; cursor:help; }
  .kw.on { border-bottom-style:solid; border-bottom-color:#FFD166; }
  .kw { position:relative; }
  .kwh { display:none; }
  button .kw { pointer-events:none; }  /* 3.1-dev.71: inside a button the word presses the button; only the strip under it asks Rivet */
  button .kw .kwh { display:block; position:absolute; left:0; right:0; top:100%; height:12px; pointer-events:auto; }
  #kwPop { position:fixed; z-index:60; width:min(300px, calc(100vw - 24px)); display:flex; gap:10px; align-items:flex-start; text-align:left; background:#0B1620;
           border:1px solid #FFD16688; border-radius:12px; padding:10px 12px; font-size:13px; line-height:1.45; color:var(--text); box-shadow:0 10px 26px #000B;
           animation:rvIn var(--t-base) var(--ease); }
  #kwPop[hidden] { display:none; }
  #kwPop::before { content:""; position:absolute; left:var(--ax, 50%); width:12px; height:12px; margin-left:-6px; background:#0B1620; transform:rotate(45deg); }
  #kwPop.below::before { top:-7px; border-left:1px solid #FFD16688; border-top:1px solid #FFD16688; }
  #kwPop.above::before { bottom:-7px; border-right:1px solid #FFD16688; border-bottom:1px solid #FFD16688; }
  #kwPop svg { flex:none; } #kwPop > div { min-width:0; }
  #kwPop b { display:block; font-family:var(--display); font-size:10px; letter-spacing:2px; color:#FFD166; margin-bottom:2px; }
  #kwPop i { color:var(--dim); }
  .hgnav .cir.live { display:flex; flex-direction:column; align-items:center; justify-content:center; gap:3px; }
  .cir.live small { font-size:8px; letter-spacing:1.5px; color:var(--dim); }
  .fvq { display:grid; grid-template-columns:repeat(2, 15px); gap:3px 5px; }
  .fvq i { display:block; width:15px; height:15px; }
  .fvq i.hot { animation:fvHot 1.4s ease-in-out infinite; }
  @keyframes fvHot { 50% { filter:drop-shadow(0 0 4px #FF6B5A); } }
  .podium { display:flex; align-items:flex-end; gap:3px; height:26px; }
  .podium i { display:block; width:8px; border-radius:2px 2px 0 0; opacity:.55; }
  .podium i.me { opacity:1; box-shadow:0 0 0 1px #E8EEF5; }
  /* 3.1-dev.61 SETTINGS */
  .gear { width:36px; height:36px; min-height:0; padding:0; margin:0; border-radius:50%; display:flex; align-items:center; justify-content:center; color:var(--dim); background:none; border:none; }
  .setH { font-family:var(--display); font-size:11px; letter-spacing:3px; color:var(--dim); text-align:left; margin:18px 2px 6px; }
  .setRow { display:flex; align-items:center; justify-content:space-between; gap:12px; width:100%; min-height:0; margin:6px 0; padding:12px 14px; border-radius:14px;
            border:1px solid var(--line); background:#0C1118; color:var(--text); text-align:left; font:inherit; letter-spacing:0; }
  .setRow span { min-width:0; display:flex; flex-direction:column; gap:2px; }
  .setRow b { font-family:var(--display); font-size:14px; letter-spacing:2px; }
  .setRow small { font-size:12px; line-height:1.35; color:var(--dim); letter-spacing:0; }
  .tg { flex:none; position:relative; width:46px; height:26px; border-radius:13px; background:#1A2532; border:1px solid #2A3B50; transition:background var(--t-fast) var(--ease); }
  .tg::after { content:""; position:absolute; top:3px; left:3px; width:18px; height:18px; border-radius:50%; background:#5C6F84; transition:transform var(--t-fast) var(--ease), background var(--t-fast) var(--ease); }
  .tg.on { background:#123446; border-color:#5FD4FF; } .tg.on::after { transform:translateX(20px); background:#5FD4FF; }
  .setNet { text-align:left; font-size:14px; line-height:1.45; margin:4px 2px 8px; color:var(--text); }
  .setNet small { display:block; color:var(--dim); font-size:12px; }
  .netRow { display:flex; align-items:center; justify-content:space-between; gap:10px; padding:10px 14px; margin:6px 0; border-radius:14px; border:1px solid var(--line); background:#0C1118; text-align:left; }
  .netRow b { min-width:0; overflow:hidden; text-overflow:ellipsis; white-space:nowrap; font-size:14px; }
  .netRow em { font-style:normal; font-family:var(--display); font-size:10px; letter-spacing:2px; color:#8CE7A0; margin-left:8px; }
  .netRow button { flex:none; width:auto; min-height:0; margin:0; padding:6px 12px; border-radius:999px; font-family:var(--display); font-size:11px; letter-spacing:2px; background:none; border:1px solid #5C6F84; color:var(--dim); }
  .netRow button.armed { border-color:#FF6B5A; color:#FF6B5A; }
  .pill.setPill { font-size:15px; letter-spacing:2px; padding:12px 14px; min-height:0; margin:8px 0; }
  .setWarn { font-size:12px; line-height:1.4; color:#FFB347; text-align:left; margin:4px 2px; }
  body { background:var(--bg); color:var(--text); font-family: 'Barlow Semi Condensed', 'Roboto Condensed', sans-serif;
         text-align:center; padding: max(16px, calc(env(safe-area-inset-top) + 14px)) max(14px, env(safe-area-inset-right)) max(16px, env(safe-area-inset-bottom)) max(14px, env(safe-area-inset-left)); margin: 0;
         overscroll-behavior: none; -webkit-tap-highlight-color: transparent; touch-action: manipulation; }
  html { overscroll-behavior: none; touch-action: manipulation; }  /* 3.1-dev.114 (playtest: double-tap zoomed the page) */
  /* 3.1-dev.38: +14 px under the status bar -- as a home-screen app iOS blurs the strip just below it ("SHADOW FLEET" was blurry) */
  h1 { color:var(--dim); font-size: 13px; letter-spacing: 4px; font-weight: 600; margin: 0 0 8px; }
  @media (display-mode: standalone) { body { padding-top: calc(env(safe-area-inset-top) + 40px); } }
  /* 3.1-dev.84 (user: "the reveal and those last battles, the text at the top is blurry in the web app"; earlier: "the text at the very top
     of battles looks super blurry"): the battle screen, the Final Hour and transmissions cover the page, so they need the same room. */
  body .bfull { padding-top:max(14px, calc(env(safe-area-inset-top) + 14px)); }
  body .fhpbox { padding-top:max(20px, calc(env(safe-area-inset-top) + 20px)); }
  @media (display-mode: standalone) { body .bfull { padding-top: calc(env(safe-area-inset-top) + 40px); } body .fhpbox { padding-top: calc(env(safe-area-inset-top) + 40px); }
                                      body #txOverlay #txContent { padding-top: calc(env(safe-area-inset-top) + 40px); } }  /* 3.1-dev.44 (playtest 4 photo: "SHADOW FLEET" still blurred) */
  .card { border:1px solid var(--line); border-radius:16px; padding: 14px; margin: 12px 0; background:var(--panel); }
  input { font-size: 18px; padding: 12px; border-radius: 10px; border:2px solid var(--line);
          background:var(--bg); color:var(--text); width: 80%; text-align:center; }
  button { background:var(--panel); color:var(--text); border:1px solid var(--line); border-radius:14px;
           padding: 14px 22px; font-size: 16px; margin: 6px; min-height: 48px; letter-spacing: 1px; }
  button:disabled { opacity: 0.4; }
  .big { width: 90%; font-size: 18px; padding: 18px; border: 2px solid var(--accent); }
  /* Home hub + shared pieces */
  .topbar { display:flex; justify-content:space-between; align-items:center; font-size:14px;
            color:var(--dim); letter-spacing:2px; margin: 0 2px 10px; }
  .topbar b { color:var(--accent); letter-spacing:2px; }
  /* 3.1-dev.66: the Settings gear made it wrap ("ROUND 3 / 8" and "9 CR" on two lines in Resolution) -- never wrap, tighter */
  .topbar span, .topbar b { white-space:nowrap; }
  .topbar { gap:8px; letter-spacing:1px; font-size:13px; } .topbar b { letter-spacing:1.5px; }
  .topbar .topL { gap:8px; min-width:0; } .topbar .topR { gap:8px; }
  .topbar .gear, .topbar .bell { width:32px; height:32px; }
  .ptrack { margin:-4px 0 10px; }
  .pts { display:flex; justify-content:center; align-items:center; gap:6px; font-family:var(--display); font-size:11px; letter-spacing:2px; color:#5C6F84; }
  .pts span.on { color:#05070C; background:var(--accent); border-radius:99px; padding:3px 10px; font-weight:700; }
  .pts span.done { color:var(--dim); }
  .pts i { font-style:normal; color:#3A4656; }
  .ptline { margin:6px 2px 0; padding:7px 12px; border:1px solid var(--accent); border-radius:12px; font-size:13px; line-height:1.4; text-align:left;
            background:color-mix(in srgb, var(--accent) 8%, transparent); }
  .ptline b { font-family:var(--display); font-size:10px; letter-spacing:2px; color:var(--accent); margin-right:8px; }
  .ptline { display:flex; align-items:flex-start; } .ptline b { flex:none; padding-top:2px; } .ptline span { flex:1; min-width:0; }
  .ptx { flex:none; margin:-6px -8px -6px 6px; min-height:0; width:34px; height:34px; padding:0; border:0; background:none; color:var(--dim); font-size:20px; line-height:1; }
  .ptback { margin:0 0 0 4px; min-height:0; padding:2px 8px; font-family:var(--display); font-size:10px; letter-spacing:2px; color:var(--accent); border:1px solid var(--accent); border-radius:99px; background:none; }
  .segs { display:grid; gap:4px; margin: 6px 0 4px; }
  .segs div { height:6px; border-radius:3px; background:var(--ring); }
  .stats { display:grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap:10px; margin: 12px 0; }
  .stat { border:1px solid var(--line); border-radius:12px; padding:8px 10px; text-align:center; display:flex; flex-direction:column; align-items:center; justify-content:center; gap:2px; }  /* 3.1: centered (layout rule); 3.1-dev.106: vertically too, like the PLANETS button */
  .stat span { display:block; font-size:12px; letter-spacing:1px; color:var(--dim); }
  .stat b { font-size:26px; }
  .hubgrid { display:grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap:12px; }
  .tile { margin:0; min-height:150px; border-radius:18px; display:flex; flex-direction:column;
          align-items:center; justify-content:center; gap:6px; font-size:19px; font-weight:700; letter-spacing:2px; }
  .tile small { font-size:13px; font-weight:400; letter-spacing:0; color:var(--dim); }
  .tile.main { border:2px solid var(--accent); color:var(--accent); }
  .tile.atk { border:2px solid var(--red); color:var(--red); }
  .pill { width:100%; margin:12px 0 0; min-height:72px; border-radius:36px; border:2px solid var(--cyan);
          color:var(--cyan); font-size:22px; font-weight:700; letter-spacing:4px; display:flex;
          flex-direction:column; align-items:center; justify-content:center; }
  .pill small { font-size:13px; font-weight:400; letter-spacing:0; color:var(--dim); }
  .pill.attack { border-color:var(--red); color:var(--red); min-height:60px; font-size:18px; }
  .srow { display:flex; align-items:center; gap:12px; min-height:48px; padding:0 14px; margin:6px 0;
          border:1px solid var(--line); border-radius:12px; text-align:left; }
  .srow .rk { font-size:20px; font-weight:700; color:var(--faint); width:16px; }
  .srow .nm { flex-grow:1; font-size:15px; font-weight:600; letter-spacing:1px; }
  .srow .vp { font-size:20px; font-weight:700; }
  .srow .pl { font-size:13px; color:var(--dim); width:52px; text-align:right; }
  .srow.me { border:2px solid var(--accent); }
  .pixel { image-rendering: pixelated; }
  /* v0.21.1 round/phase banner: hyperspace streaks race outward */
  #banner { position:fixed; inset:0; background:var(--bg); z-index:900; display:none; overflow:hidden; }
  #banner.active { display:flex; flex-direction:column; align-items:center; justify-content:center; }
  #banner .streak { position:absolute; left:50%; top:45%; width:2px; height:34px; border-radius:1px;
                    background:#CFE9FF; transform-origin:1px 0; opacity:0; animation:warp 1.1s linear infinite; }
  @keyframes warp { 0% { transform:rotate(var(--a)) translateY(50px) scaleY(.6); opacity:.1; }
                    100% { transform:rotate(var(--a)) translateY(330px) scaleY(2); opacity:.9; } }
  #banner .num { font-size:110px; font-weight:700; line-height:1; position:relative; }
  #banner .num small { font-size:30px; color:var(--dim); }
  #banner .inc { color:#FFD166; font-size:15px; font-weight:700; letter-spacing:3px; margin-top:10px; position:relative; }
  #banner .lbl { font-size:14px; letter-spacing:6px; color:var(--dim); position:relative; }
  #banner .ph { font-size:24px; font-weight:700; letter-spacing:4px; color:var(--cyan); margin-top:6px; position:relative; }
  #banner .hsw { position:absolute; inset:0; pointer-events:none; }
  #banner .hsw i { position:absolute; left:50%; top:50%; height:2px; border-radius:1px; background:linear-gradient(90deg, transparent, #CFE8FF 40%, #FFFFFF);
                     transform-origin:0 50%; transform:rotate(var(--a)) translateX(var(--r)) scaleX(.03); animation:hsFly 700ms cubic-bezier(.55,0,.95,.35) forwards; }
  @keyframes hsFly { 0% { transform:rotate(var(--a)) translateX(var(--r)) scaleX(.03); } 70% { opacity:1; } 100% { transform:rotate(var(--a)) translateX(75vmax) scaleX(1.4); opacity:0; } }
  #banner .hsFlash { position:absolute; inset:0; background:radial-gradient(circle, #FFFFFF 0%, #CFE8FF55 35%, transparent 70%); opacity:0; animation:hsFlashA 520ms ease-out 380ms forwards; }
  @keyframes hsFlashA { 0% { opacity:0; } 30% { opacity:.85; } 100% { opacity:0; } }
  #banner .hsBody { position:relative; display:flex; flex-direction:column; align-items:center; animation:hsLand 420ms var(--ease) 480ms both; }
  @keyframes hsLand { 0% { opacity:0; transform:scale(1.15); } 100% { opacity:1; transform:none; } }
  #banner .hsBody .num { display:flex; align-items:flex-end; gap:8px; font-size:30px; margin:8px 0; }
  #banner .hsBody .num small { font-size:24px; color:var(--dim); }
  #banner .hsBody .ph { font-family:var(--display); font-size:28px; letter-spacing:6px; }
  @media (prefers-reduced-motion: reduce) { #banner .hsw { display:none; } #banner .hsBody { animation:none; } }
  #banner .incT { font-size:12px; letter-spacing:2px; color:#C9B27A; margin-top:2px; position:relative; }
  #banner .tap { font-size:12px; letter-spacing:2px; color:var(--faint); margin-top:22px; position:relative; }
  /* v0.21.2 Galaxy cards */
  .back { margin:4px 0 0; min-height:44px; padding:0 16px; border-radius:22px; border:1px solid var(--line); background:transparent; color:var(--dim); font-size:15px; }
  .gxhead { display:flex; flex-direction:column; align-items:center; margin:8px 0 12px; }
  .gxhead b { font-size:24px; letter-spacing:4px; color:var(--accent); }
  .gxhead span { font-size:13px; color:var(--dim); }
  /* v0.21.14 Galaxy = board map; positions in % of the square map */
  .gmap { position:relative; width:100%; padding-top:100%; }
  .gwin { position:relative; width:100%; max-width:100%; aspect-ratio:1 / 1; margin:0 auto; overflow:hidden; }
  .gall { display:flex; flex-direction:column; gap:6px; margin:10px 0; }
  .gall > div, .gall > button { display:grid; grid-template-columns:1fr 34px 78px 26px; gap:8px; align-items:center; text-align:left; }
  .gallH { padding:0 10px; font-family:var(--display); font-size:10px; letter-spacing:1.5px; color:#5C6F84; } .gallH span:not(:first-child) { text-align:center; }
  .gall > button { width:100%; margin:0; min-height:46px; padding:0 10px; border-radius:12px; border:1px solid var(--line); background:#0C1118; color:var(--text); }
  .gall .gw { display:flex; align-items:center; gap:8px; min-width:0; } .gall .gw > i { width:12px; height:12px; border-radius:50%; flex:none; }
  .gall .gw b { display:block; font-family:var(--display); font-size:13px; letter-spacing:1px; } .gall .gw small { display:block; font-size:11px; color:var(--dim); white-space:nowrap; overflow:hidden; text-overflow:ellipsis; }
  .gall .gv { text-align:center; font-family:var(--display); color:#FFD166; } .gall .gd { text-align:center; font-family:var(--display); font-size:10px; letter-spacing:1px; }
  .gall .gs { justify-self:center; width:16px; height:16px; border-radius:50%; box-sizing:border-box; }
  .gmap > button.cf { outline:2px dashed #5FD4FF; outline-offset:3px; }
  .gpan { position:absolute; inset:0; transform-origin:50% 50%; will-change:transform; }
  .gpan.anim { transition:transform var(--t-base) var(--ease); }
  .gpan .gmap { padding-top:100%; }
  .gmap.filt button.dim { opacity:.28; filter:grayscale(.6); }
  .gmap.filt button.lit { box-shadow:0 0 0 3px #FFD166, 0 0 18px #FFD166 !important; }
  .gmap button { transition:opacity var(--t-base) var(--ease); }
  .gzoom { position:absolute; right:4px; bottom:4px; display:flex; flex-direction:column; gap:4px; }
  .gzoom button { margin:0; width:32px; height:32px; min-height:0; padding:0; border-radius:50%; font-size:16px; background:#0C1118cc; }
  .gfilt { display:flex; justify-content:center; gap:4px; margin:4px 0 8px; flex-wrap:nowrap; }
  .gfilt button { margin:0; min-height:0; padding:6px 8px; font-size:10px; letter-spacing:1px; font-family:var(--display); border-radius:99px; white-space:nowrap; }  /* 3.1-dev.55: five fit */
  .gfilt button.on { border-color:#FFD166; color:#FFD166; }
  .gfilt i { font-style:normal; background:#FFD166; color:#05070C; border-radius:99px; padding:0 5px; margin-left:2px; }
  .glist { margin-top:8px; }
  .glist button { display:flex; align-items:center; gap:8px; width:100%; margin:4px 0 0; min-height:0; padding:8px 10px; text-align:left; font-size:13px; }
  .glist b { font-family:var(--display); letter-spacing:2px; font-size:12px; white-space:nowrap; }
  .glist span { flex:1; color:var(--dim); font-size:12px; } .glist em { font-style:normal; color:var(--dim); }
  .glist p, .ghint { font-size:12px; color:var(--dim); text-align:center; margin:8px 0 0; }
  .wsTop { display:flex; align-items:center; gap:12px; text-align:left; margin-bottom:10px; }
  .wsTop img { width:64px; height:64px; }
  .wsheet { margin:-6px -2px 0; padding:8px 12px 14px; border-radius:22px 22px 0 0; background:linear-gradient(#0E1520, #05070C 60%); border-top:1px solid var(--line);
            animation:wsUp var(--t-slow) var(--ease) both; }
  @keyframes wsUp { from { transform:translateY(40%); opacity:0; } to { transform:none; opacity:1; } }
  @media (prefers-reduced-motion: reduce) { .wsheet { animation:none; } }
  .trTag { font-size:10px; color:#C9B27A; letter-spacing:1px; }
  .wgrab { width:56px; height:5px; border-radius:3px; background:#3A4658; margin:2px auto 12px; cursor:pointer; }
  .wlore { font-size:14px; line-height:1.5; color:#C9D6E2; text-align:left; margin:2px 0 12px; padding:10px 12px; border-left:3px solid var(--accent); background:rgba(255,255,255,.03); border-radius:0 10px 10px 0; font-style:italic; }
  .pvpwrap { position:relative; display:inline-block; flex:none; }
  .pvp { position:absolute; right:-4px; bottom:2px; display:flex; align-items:center; gap:3px; padding:4px 7px; border-radius:99px; background:#FFD166; box-shadow:0 0 10px #FFD16688; font-style:normal; }
  .pvp small { font-family:var(--display); font-size:9px; font-weight:700; letter-spacing:1px; color:#1A1206; }
  .wtiles { display:grid; grid-template-columns:repeat(2, minmax(0, 1fr)); gap:6px; margin-bottom:8px; }
  .wtile { border:1px solid var(--line); border-radius:8px; padding:6px 8px; text-align:left; }
  .wtile small { display:block; font-size:10px; letter-spacing:2px; color:var(--dim); } .wtile b { font-size:13px; }
  .wtile.hid b { color:var(--faint); }
  .wtile.tap { cursor:pointer; } .wtile em { display:none; font-style:normal; font-size:12px; color:#8CE7A0; margin-top:4px; } .wtile.open em { display:block; }
  .gsvg { position:absolute; left:0; top:0; width:100%; height:100%; }
  .gmap button { position:absolute; width:14.6%; height:14.6%; transform:translate(-50%, -50%); margin:0; padding:0; border-radius:50%;
                 border:2px solid var(--line); background:#05070C; display:flex; align-items:center; justify-content:center; }
  .gmap button img { width:74%; height:74%; }
  .gmap button.core { width:12.9%; height:12.9%; }  /* 2.0 17 worlds: Core circles 60 of 466 */
  .gmap button.ashen { width:13%; height:6.4%; border:none; border-radius:4px; background:none; flex-direction:column; gap:2px; }
  .gmap button.ashen img { width:100%; height:auto; }
  .hpbar { height:8px; border-radius:4px; background:#3A0E0A; overflow:hidden; }
  .hpbar i { display:block; height:100%; background:#FF8C42; }
  .closedwrap { position:relative; height:300px; margin:10px 0; overflow:hidden; }
  .closedwrap .plank { position:absolute; left:-4%; top:46%; width:108%; height:26px; background:#8B5E34; border:3px solid #462C18; border-radius:4px; }
  .closedwrap img { position:absolute; left:50%; top:16%; transform:translateX(-50%); width:86%; image-rendering:pixelated; }
  .gmap .nm { position:absolute; left:50%; bottom:-2px; transform:translateX(-50%); font-size:9px; font-weight:700; letter-spacing:1px;
              color:var(--text); background:rgba(5,7,12,.8); border-radius:3px; padding:0 2px; white-space:nowrap; }
  .gslot { position:absolute; width:11%; height:11%; transform:translate(-50%, -50%); border-radius:50%; border:2px solid var(--ring); }
  /* v0.21.4 planet page */
  .phead { display:flex; align-items:center; gap:14px; margin:10px 0 14px; text-align:left; }
  .phead b { display:block; font-size:26px; letter-spacing:3px; }
  .phead span { font-size:14px; }
  .garr { display:flex; justify-content:center; align-items:baseline; gap:10px; margin:6px 0 0; }
  .garr span { font-size:14px; letter-spacing:2px; color:var(--dim); }
  .garr b { font-size:32px; }
  .sect { font-size:13px; letter-spacing:4px; color:var(--cyan); margin:18px 0 8px; text-align:left; }
  .intel { display:grid; grid-template-columns:repeat(2, minmax(0, 1fr)); gap:10px; }
  .intel .rv, .intel .done { margin:0; min-height:72px; box-sizing:border-box; border-radius:16px; display:flex; flex-direction:column; align-items:center; justify-content:center; gap:2px; padding:6px; }
  .intel .rv { border:2px solid var(--cyan); background:#081A26; }
  .intel .rv:disabled { border-color:var(--line); background:transparent; opacity:.6; }
  .intel .done { border:1px solid var(--line); }
  .intel .done.hid { border-style:dashed; }
  .intel .k { font-size:12px; letter-spacing:1px; color:var(--cyan); font-weight:700; }
  .intel .c { font-size:13px; color:#FFC933; }
  .intel .rv:disabled .c { color:var(--faint); }
  .intel .v { font-size:15px; font-weight:600; }
  .intel .hid .v { color:var(--faint); }
  .intel small { font-size:12px; color:var(--dim); }
  /* v0.21.6 takeover approach buttons on the planet page */
  /* v0.21.18 join flow */
  .joinhead { display:flex; flex-direction:column; align-items:center; margin:28px 0 6px; }
  .joinhead b { font-size:40px; letter-spacing:8px; line-height:1.05; }
  .joinsub { text-align:center; font-size:18px; font-weight:700; letter-spacing:3px; margin:10px 0 6px; }
  .joinsub small { font-size:12px; color:var(--dim); letter-spacing:3px; font-weight:400; }
  .joinhint { text-align:center; font-size:14px; color:var(--dim); }
  .seatgrid, .facgrid { display:grid; grid-template-columns:1fr 1fr; gap:12px; margin:8px 0 16px; }
  .seat { aspect-ratio:1; border-radius:50%; border:2px solid var(--cyan); background:#081A26; color:var(--cyan); display:flex; flex-direction:column; align-items:center; justify-content:center; gap:2px; padding:0; margin:0; width:100%; }
  .seat b { font-size:40px; line-height:1; }
  .seat small { font-size:12px; letter-spacing:2px; }
  .seat.taken { border:1px solid #22303F; background:var(--panel, #0B1420); color:var(--dim); opacity:1; }
  .seat .q { font-size:24px; color:var(--faint); }
  .faccard { min-height:130px; border-radius:18px; border:2px solid; background:#0B1018; display:flex; flex-direction:column; align-items:center; justify-content:center; gap:8px; padding:10px; margin:0; width:100%; }
  .faccard b { font-size:14px; letter-spacing:1px; line-height:1.2; }
  .faccard.taken { border:1px solid #22303F; background:transparent; opacity:.45; }
  .faccard small { font-size:11px; letter-spacing:1px; color:var(--dim); }
  /* v0.21.20 commanders */
  .cmdbig { width:200px; height:232px; margin:6px auto 10px; box-sizing:border-box; border:2px solid; border-radius:26px; background:#0B1018; display:flex; align-items:flex-end; justify-content:center; overflow:hidden; }
  .cmdbig img { width:192px; height:224px; }
  .cmdname { display:block; text-align:center; font-size:22px; letter-spacing:2px; }
  .cmdrow { display:grid; grid-template-columns:repeat(3, 1fr); gap:10px; margin:6px 0 4px; }
  .cmdcard { margin:0; padding:6px 0 8px; border-radius:16px; border:1px solid var(--line); background:#0B1420; display:flex; flex-direction:column; align-items:center; gap:4px; }
  .cmdcard img { width:72px; height:84px; }
  .cmdcard small { font-size:11px; letter-spacing:1px; }
  .lp { overflow:hidden; }
  .lp img { width:48px; height:56px; align-self:flex-end; }
  /* v0.21.19 lobby */
  .lobbyrow { display:grid; grid-template-columns:repeat(4, 1fr); gap:8px; justify-items:center; margin:14px 0 6px; }
  .lobbyrow > div { display:flex; flex-direction:column; align-items:center; gap:4px; }
  .lp { width:64px; height:64px; border-radius:50%; border:1px solid var(--line); background:#0B1420; display:flex; align-items:center; justify-content:center; }
  .lp .q { font-size:22px; color:var(--faint); }
  .lobbyrow small { font-size:11px; letter-spacing:1px; text-align:center; }
  .lenrow { display:grid; grid-template-columns:repeat(3, 1fr); gap:10px; }
  .lenrow .pill { margin:6px 0 0; font-size:20px; }
  .pill.quiet { border:1px solid var(--line); background:transparent; color:var(--dim); }
  /* v0.21.17 factions */
  .facrow { display:flex; align-items:center; gap:12px; border:1px solid var(--line); border-radius:16px; padding:12px 14px; margin:0 0 10px; }
  .facrow.mine { border-color:var(--accent); }
  .facrow .nm { flex:1; display:flex; flex-direction:column; align-items:flex-start; gap:8px; text-align:left; }
  .facrow .nm b { font-size:14px; letter-spacing:1px; }
  .facrow .nm .top { display:flex; justify-content:space-between; align-items:baseline; gap:8px; width:100%; }
  .replines { display:flex; flex-direction:column; gap:4px; margin:6px 0 2px; }
  .repline { display:flex; justify-content:center; gap:10px; font-size:13px; letter-spacing:2px; font-weight:700; }
  .rbar { position:relative; width:100%; height:6px; }
  .rbar .z { position:absolute; top:0; width:calc(20% - 2px); height:100%; border-radius:3px; opacity:.22; }
  .rbar .z.on { opacity:.75; }
  .rbar .trail { position:absolute; top:2px; height:2px; border-radius:1px; opacity:.95; box-shadow:0 0 6px currentColor; }  /* 3.1-dev.74: a streak, not a fill (it ran across the gaps into the next zone) */
  .rbar .mk { position:absolute; top:-5px; width:2px; height:16px; margin-left:-1px; border-radius:1px; background:#fff; box-shadow:0 0 6px #fff, 0 0 12px rgba(255,255,255,.5); transition:left .9s cubic-bezier(.2,.8,.2,1); }
  .facrow .tier { font-size:14px; font-weight:700; letter-spacing:1px; }
  /* v0.21.16 negotiation */
  .neghead { display:flex; flex-direction:column; align-items:center; gap:2px; margin:10px 0 14px; }
  .neghead b { font-size:24px; letter-spacing:3px; }
  .neghead span { font-size:14px; color:var(--dim); }
  /* v0.30.0 the conversation */
  .negface { border:1px solid rgba(127,227,255,.4); border-radius:14px; background:#061621; margin-bottom:6px; }
  .negpips { display:flex; gap:8px; margin-top:8px; }
  .negpips i { width:12px; height:12px; border-radius:50%; border:1px solid var(--line); }
  .negpips i.cur { border-color:var(--cyan); box-shadow:0 0 6px var(--cyan); }
  .negpips i.m1 { background:#8CE7A0; border-color:#8CE7A0; }
  .negpips i.m2 { background:#7C93A8; border-color:#7C93A8; }
  .negpips i.m3 { background:#FF6B5A; border-color:#FF6B5A; }
  .negtier { font-size:12px; letter-spacing:3px; color:var(--dim); margin:4px 0; }
  .negq { font-size:18px; line-height:1.4; color:var(--text); margin:6px 0 14px; }
  .negtip { font-size:13px; color:var(--cyan); margin:-6px 0 10px; }
  .negr { width:100%; margin:0 0 10px; min-height:58px; border-radius:16px; border:2px solid var(--cyan); background:#081A26; color:var(--text); font-size:15px; line-height:1.35; padding:10px 14px; text-align:left; }
  .negr.fav { border-color:#FF9B70; }
  .negr small { display:block; margin-bottom:4px; font-size:11px; letter-spacing:1px; color:#FF9B70; }
  .negowe { font-size:15px; color:#FFD166; max-width:320px; }
  .neghint, .negmsg { font-size:13px; color:var(--dim); margin:4px 0; }
  .negmsg { font-size:16px; color:var(--text); line-height:1.4; margin:10px 0 18px; }
  /* v0.21.13 battle report */
  .vic { display:flex; flex-direction:column; align-items:center; gap:6px; margin-top:10px; }
  .vic .k { font-size:13px; letter-spacing:4px; color:var(--dim); }
  .vic .badge { margin:6px 0 10px; padding:12px 34px; border:3px solid; border-radius:30px; font-size:30px; font-weight:700; letter-spacing:8px; }
  .vic .art { display:flex; flex-direction:column; align-items:center; gap:8px; min-height:140px; justify-content:center; }
  .vic b { font-size:20px; letter-spacing:2px; margin-top:8px; }
  .vic .dim { font-size:15px; color:var(--dim); }
  .vic .price { font-size:44px; font-weight:700; letter-spacing:2px; line-height:1.1; }
  .ships { display:grid; grid-template-columns:repeat(3, 1fr); gap:14px 8px; margin:14px 0 6px; width:100%; max-width:340px; }
  .ships div { display:flex; flex-direction:column; align-items:center; gap:4px; font-size:12px; letter-spacing:1px; color:var(--faint); }
  .ships div.on { color:var(--text); }
  .txchip { margin-top:14px; padding:8px 16px; border:1px solid var(--cyan); border-radius:16px; color:var(--cyan); font-size:13px; letter-spacing:2px; animation:txpulse 1.4s ease-in-out infinite; }
  @keyframes txpulse { 0%,100% { opacity:1; } 50% { opacity:.4; } }
  /* v0.21.10 list rows (Fleets / opponents / their fleets) */
  .rows button { width:100%; margin:0 0 12px; min-height:72px; border-radius:36px; border:2px solid var(--line); background:transparent; padding:0 22px;
                 display:grid; grid-template-columns:1fr auto; align-items:center; text-align:left; }
  .rows .t { grid-column:1; font-size:20px; font-weight:700; letter-spacing:2px; color:var(--text); }
  .rows small { grid-column:1; font-size:14px; color:var(--dim); }
  .rows svg { grid-column:2; grid-row:1 / span 2; }
  .rows button:disabled { opacity:.45; }
  /* v0.21.7 Battle Forecast */
  .fchead { display:flex; flex-direction:column; align-items:center; margin:8px 0 12px; }
  .fchead span { font-size:13px; letter-spacing:4px; color:var(--red); }
  .fchead b { font-size:24px; letter-spacing:3px; }
  .fcols { display:grid; grid-template-columns:1fr auto 1fr; gap:10px; align-items:center; padding:14px 10px; border:1px solid #3A1C1C; border-radius:16px; background:#0D0808; }
  .fcols div { display:flex; flex-direction:column; align-items:center; gap:2px; }
  .fcols .h { font-size:12px; letter-spacing:1px; }
  .fcols .units { height:18px; }
  .fcols .unk { font-size:18px; font-weight:700; color:#FF7A45; height:18px; line-height:18px; }
  .fcols .die { position:relative; width:64px; height:64px; box-sizing:border-box; border:3px solid; border-radius:14px; display:flex; align-items:center; justify-content:center; font-size:24px; font-weight:700; margin-top:10px; }
  .fcols .badge { position:absolute; top:-14px; right:-14px; width:30px; height:30px; border-radius:50%; color:var(--bg); font-size:15px; display:flex; align-items:center; justify-content:center; }
  .fcols .src { height:30px; margin-top:6px; }
  .betray { margin:12px 0 0; padding:10px 12px; border:1px solid var(--red); border-radius:12px; color:#FFB0A8; font-size:13px; line-height:1.35; text-align:left; }
  .betray b { color:var(--red); letter-spacing:2px; }
  /* UI 3.0 step 4: Movement */
  .movcard { margin:0 0 10px; padding:12px; border:1px solid #5FD4FF; border-radius:14px; text-align:center; }
  .movcard b { display:block; font-size:17px; letter-spacing:2px; }
  .movcard span { font-size:13px; color:var(--dim); }
  .hangar.mov .hgtitle span { color:#5FD4FF; }
  .gxbtn.mov { border-color:#5FD4FF; box-shadow:0 0 22px rgba(95,212,255,.25); }
  .gxbtn.mov span { color:#5FD4FF; }
  .rumor { margin:10px 0 0; padding:12px 14px; border:1px solid #B07CFF; border-radius:14px; background:#0E0A14; text-align:left; }
  .rumor span { display:block; font-size:11px; letter-spacing:3px; color:#B07CFF; }
  .rumor b { display:block; font-size:17px; font-weight:600; line-height:1.35; margin:4px 0; }
  .rumor small { font-size:12px; color:var(--dim); }
  .rumor.mine { border-color:#FFD166; box-shadow:0 0 18px rgba(255,209,102,.25); }
  .rumor em { display:block; font-style:normal; font-size:14px; font-weight:700; letter-spacing:2px; color:#FFD166; margin:2px 0 4px; }
  /* UI 3.0 take 4: Resolution home + a world's page in Resolution */
  .hangar.res .hgtitle span { color:#FF8C42; }
  .bay.ready { border-color:#FF8C42; }
  .baytag { font-size:11px; letter-spacing:1px; color:#FF8C42; flex:none; }
  .bay.spent .baytag { color:var(--dim); }
  .gxbtn.res { border-color:#FF8C42; box-shadow:0 0 22px rgba(255,140,66,.25); }
  .gxbtn.res span { color:#FF8C42; }
  .hgatk { width:100%; margin:6px 0 0; min-height:44px; border:1.5px solid var(--red); border-radius:22px; background:#1A0A0A; color:var(--red); font-size:14px; font-weight:700; letter-spacing:3px; }
  .rwtiles { display:grid; grid-template-columns:repeat(2, minmax(0, 1fr)); gap:8px; margin:10px 0 0; }
  .rwtiles div { border:1px solid var(--line); border-radius:12px; padding:6px 10px; text-align:center; }
  .rwtiles span { display:block; font-size:10px; letter-spacing:2px; color:#5FD4FF; }
  .rwtiles b { font-size:15px; }
  .rwodds { margin:10px 0 0; padding:10px 12px; background:#0B1220; border:1px solid var(--line); border-radius:16px; text-align:left; }
  .rwline { display:flex; justify-content:space-between; align-items:center; gap:8px; font-size:14px; color:#B7C7D6; }
  .rwline b { font-size:19px; white-space:nowrap; }
  .rwline small { font-size:13px; }
  .rwmkt { margin:10px 0 0; padding:8px 12px; border:1px solid #FFD166; border-radius:12px; font-size:13px; text-align:left; }
  .rwmkt b { font-size:11px; letter-spacing:2px; color:#FFD166; }
  .rwacts .sacts button { min-height:84px; font-size:18px; }
  .sacts button.stalk { font-size:13px; letter-spacing:1px; } .rwacts .sacts button.stalk { font-size:14px; }  /* 3.1-dev.88 NEGOTIATE fits */
  /* dev.37b: market stalls under the hangar, the ATTACK A PLAYER screen */
  .stat .rk { font-size:12px; color:var(--dim); margin-left:6px; }
  .mkstrip { margin:8px 0 0; padding:8px 10px 10px; border:1px solid #3A2A18; border-radius:16px; background:repeating-linear-gradient(90deg, #0C0805 0 3px, #140D08 3px 42px); }
  .mktiles { display:grid; grid-template-columns:repeat(4, minmax(0, 1fr)); gap:6px; margin-top:6px; }
  .mktile { margin:0; padding:6px 4px; min-height:52px; border:1px solid #5A4024; border-radius:10px; background:#1A120A; color:var(--text); font:inherit; display:flex; flex-direction:column; gap:2px; align-items:center; justify-content:center; }
  .mktile b { font-size:10px; letter-spacing:1px; color:#FFD166; text-align:center; line-height:1.1; }
  .mktile span { font-size:10px; color:#C9B89A; }
  .mktile.gone { opacity:.45; }
  .aphead { display:flex; flex-direction:column; gap:2px; margin:4px 0 10px; text-align:left; }
  .aphead b { font-size:22px; letter-spacing:3px; color:var(--red); }
  .aphead span { font-size:13px; color:var(--dim); }
  .apgroup { border:1px solid; border-radius:14px; padding:8px 10px; margin:0 0 8px; text-align:left; }
  .apname { display:flex; align-items:center; gap:6px; font-size:14px; font-weight:700; letter-spacing:2px; margin:0 0 4px; }
  .apname em { font-style:normal; font-size:11px; color:var(--red); border:1px solid var(--red); border-radius:8px; padding:1px 6px; margin-left:auto; }
  .aprow { display:flex; align-items:center; gap:8px; width:100%; box-sizing:border-box; margin:4px 0 0; padding:8px 10px; min-height:52px; border:1px solid var(--line); border-radius:12px; background:#0B1220; color:var(--text); font:inherit; text-align:left; }
  .apfl { font-size:13px; font-weight:700; letter-spacing:1px; width:84px; flex:none; }
  .apfl small { display:block; font-size:11px; font-weight:400; letter-spacing:0; color:var(--dim); }
  .apships { flex:1; display:flex; gap:2px; flex-wrap:wrap; }
  .apships img { width:22px; image-rendering:pixelated; }
  .aprow b { font-size:13px; white-space:nowrap; }
  .aprow .dim { font-size:12px; color:var(--dim); }
  /* UI 3.0 step 3: the hangar */
  .hgstats { display:flex; justify-content:space-between; font-size:14px; color:var(--dim); margin:2px 2px 8px; }
  .hgstats b { font-size:20px; color:var(--text); }
  .hangar { background:repeating-linear-gradient(0deg, #0A0E14 0 22px, #0C1118 22px 23px); border:1px solid var(--line); border-radius:16px; padding:10px; display:flex; flex-direction:column; gap:8px; }
  .hgtitle { display:flex; justify-content:space-between; align-items:baseline; }
  .hgtitle span { font-size:13px; font-weight:700; letter-spacing:3px; color:var(--accent); }
  .hgtitle small { font-size:12px; color:var(--dim); }
  .bay { box-sizing:border-box; display:flex; align-items:center; gap:8px; padding:6px 8px; border:1px solid var(--line); border-radius:12px; background:#0B1220; min-height:46px; }
  .bay.spent { opacity:.6; }
  .bayk { display:flex; flex-direction:column; align-items:center; font-size:9px; letter-spacing:1px; color:var(--dim); width:34px; flex:none; }
  .bayk b { font-size:18px; color:var(--text); line-height:1; }
  .bay .slots { flex:1; display:grid; gap:3px; align-items:center; }
  .bay .slots img { width:100%; max-width:40px; height:auto; image-rendering:pixelated; }
  .bay .slots i { height:24px; border:1px dashed #1E2A38; border-radius:6px; }
  .bay .slots button { margin:0; padding:0; min-height:30px; min-width:0; border-radius:8px; font-size:11px; font-weight:700; line-height:1; }
  .bay .slots button.plus { border:1.5px dashed var(--accent); color:var(--accent); background:none; }
  .bay .slots button.fix { border:1.5px solid var(--red); color:var(--red); background:#1A0A0A; font-size:9px; }
  .bay.empty { width:100%; margin:0; text-align:left; border-style:dashed; font:inherit; color:var(--dim); }
  .bay.empty.lost { border-color:var(--red); }
  .bay.empty .baywhat { flex:1; font-size:14px; }
  .bay.empty b { font-size:13px; color:#FFD166; }
  .hgbar { display:flex; align-items:center; gap:8px; width:100%; box-sizing:border-box; margin:8px 0 0; padding:10px 12px; min-height:46px; border:1px solid; border-radius:12px; background:none; font:inherit; font-size:13px; font-weight:700; letter-spacing:1px; text-align:left; }
  .hgbar span { flex:1; font-weight:400; letter-spacing:0; font-size:13px; }
  .hgbar span b { font-size:11px; letter-spacing:2px; margin-right:4px; }
  .hgbar em { margin-left:auto; font-style:normal; font-size:13px; }
  .hgnav { display:flex; justify-content:center; align-items:center; gap:16px; margin:8px 0 2px; }
  .hgnav .cir { width:66px; height:66px; min-height:0; margin:0; padding:0; border-radius:50%; font-size:10px; letter-spacing:1px; }
  .gxbtn { width:104px; height:104px; min-height:0; margin:0; padding:0; border-radius:50%; border:2px solid var(--accent); background:radial-gradient(circle, #0B1220 0, #05070C 70%);
           display:flex; flex-direction:column; align-items:center; justify-content:center; gap:6px; box-shadow:0 0 22px rgba(255,201,51,.22); }
  .gxbtn canvas { width:52px; height:52px; border-radius:50%; image-rendering:pixelated; }
  .gxbtn span { font-size:13px; font-weight:700; letter-spacing:3px; color:var(--accent); }
  /* the ATTACK A PLAYER toggle */
  .rgtop { display:flex; justify-content:space-between; align-items:center; gap:8px; }
  .rgtop > span { font-size:11px; letter-spacing:2px; color:var(--dim); }
  .rgatk { margin:0; padding:4px 12px; min-height:34px; border:1.5px solid var(--red); border-radius:17px; background:none; color:var(--red); font:inherit; font-size:12px; font-weight:700; letter-spacing:1px; }
  .rgatk.on { background:#2A0A0A; }
  /* UI 3.0 step 2: Resolution = the galaxy + its sheet */
  .rghead { display:flex; justify-content:space-between; align-items:baseline; margin:2px 0 4px; }
  .rghead b { font-size:20px; letter-spacing:3px; color:#FF8C42; }
  .rghead span { font-size:13px; color:var(--dim); }
  .gmap.rg { width:100%; max-width:380px; padding-top:0; aspect-ratio:1 / 1; margin:0 auto; }  /* a square: padding-top % follows the PARENT width */
  /* dev.36b (playtest on an iPhone: worlds overlapped, two rings each): smaller worlds, ONE ring = your odds,
     the owner shows as the name's color. */
  .gmap.rg.sm { max-width:300px; }
  .gmap.rg button, .gmap.rg button.core { width:11%; height:11%; min-height:0; min-width:0; border-width:3px; background:#05070C; }
  .gmap.rg button img { width:80%; height:80%; }
  .gmap.rg .nm { font-size:8px; bottom:-11px; background:#05070Ccc; padding:0 2px; border-radius:3px; }
  .rgstrip { display:flex; flex-direction:column; gap:4px; margin:0 0 4px; }
  .rgstrip > span { font-size:11px; letter-spacing:2px; color:var(--dim); text-align:left; }
  .rchips { display:flex; flex-wrap:wrap; gap:6px; }
  .rchip { margin:0; padding:4px 10px; min-height:34px; border:1.5px solid; border-radius:17px; background:none; font:inherit; font-size:12px; font-weight:700; letter-spacing:1px; }
  .rchip b { margin-left:6px; font-size:14px; }
  .gmap .fchip { position:absolute; width:9%; height:9%; transform:translate(-50%, -50%); margin:0; padding:0; border-radius:50%; border:2px solid; background:#05070C; font-size:12px; font-weight:700; }
  .rglegend { display:flex; justify-content:center; gap:10px; font-size:10px; letter-spacing:1px; margin:6px 0 0; }
  .rghint { font-size:13px; color:var(--dim); text-align:center; margin:6px 0; }
  .rgfoot { display:grid; grid-template-columns:repeat(2, minmax(0, 1fr)); gap:8px; margin:8px 0 0; }
  .rgfoot button { margin:0; min-height:44px; font-size:13px; letter-spacing:2px; }
  .sheet { margin:8px 0 0; padding:10px 12px 12px; background:#0B1220; border:1px solid var(--line); border-radius:16px; text-align:left; }
  .shead { display:flex; align-items:center; gap:10px; }
  .shead img { width:44px; height:44px; border-radius:50%; flex:none; }
  .shead > div { flex:1; min-width:0; }
  .shead b { font-size:19px; letter-spacing:2px; }
  .shead .vp { font-size:13px; color:#FFD166; font-weight:700; }
  .shead .sub { font-size:12px; color:var(--dim); line-height:1.3; }
  .shead .fdot { width:20px; height:20px; border-radius:50%; flex:none; }
  .sx { margin:0; min-width:40px; min-height:40px; padding:0; border-radius:20px; font-size:20px; color:var(--dim); }
  .sodds { display:flex; justify-content:space-between; align-items:center; gap:8px; margin:8px 0 0; padding:8px 10px; border:1px solid var(--line); border-radius:12px; font-size:13px; }
  .sodds b { font-size:16px; letter-spacing:1px; white-space:nowrap; }
  .sodds small { font-size:12px; }
  .slabel { font-size:11px; letter-spacing:3px; color:#5FD4FF; margin:10px 0 4px; }
  .ssends { display:grid; grid-template-columns:repeat(2, minmax(0, 1fr)); gap:8px; }
  .ssend { margin:0; padding:8px 10px; min-height:52px; border:1px solid #5FD4FF; border-radius:12px; background:#071420; color:var(--text); text-align:left; font:inherit; }
  .ssend b { display:block; font-size:12px; letter-spacing:1px; color:#5FD4FF; }
  .ssend span { font-size:12px; color:var(--dim); }
  .ssend:disabled { opacity:.45; }
  .sacts { display:grid; grid-template-columns:repeat(3, minmax(0, 1fr)); gap:8px; margin:10px 0 0; }
  .sacts.one { grid-template-columns:1fr; }
  .sacts button { margin:0; min-height:60px; border-radius:16px; border:2px solid; font-size:17px; font-weight:700; letter-spacing:2px; }
  .sacts small { display:block; font-size:12px; letter-spacing:0; text-indent:0; }
  .sacts button { padding:0 4px; text-indent:2px; white-space:nowrap; overflow:hidden; text-align:center; }  /* 3.1-dev.114 (playtest: NEGOTIATE wasn't centered): letter-spacing only pads the right side */
  .sacts button.stalk { text-indent:1px; }
  .sfight { border-color:#FF6B5A !important; color:#FF6B5A; background:#1A0A0A; }
  .stalk { border-color:#5FD4FF !important; color:#5FD4FF; background:#071420; }
  .spay { border-color:#FFD166 !important; color:#FFD166; background:#1A1608; }
  /* UI 3.0 step 1: the full-screen battle */
  .bfull { position:fixed; inset:0; z-index:40; display:flex; flex-direction:column; gap:8px; padding:14px 14px 18px; background:#05070C; box-sizing:border-box; width:100vw; height:100%; overflow:hidden; }
  .bfull #bTitle { margin:0; font-size:22px; font-weight:700; letter-spacing:3px; text-align:center; }
  .bfull > * { min-width:0; max-width:100%; }
  .bfull #bCanvas { flex:1 1 0; min-height:0; width:100%; height:0; display:block; border-radius:14px; }  /* height:0 + flex: its pixel size must never prop the layout open */
  .bfull #bText { margin:0; height:50px; flex:none; display:flex; align-items:center; justify-content:center; overflow:hidden; font-size:19px; font-weight:700; line-height:1.25; text-align:center; }
  .bfull #bBtns { display:grid; grid-template-columns:repeat(2, minmax(0, 1fr)); gap:10px; height:120px; flex:none; align-content:end; }
  .bfull #bBtns .brow2 { grid-column:1 / -1; display:flex; gap:8px; }
  .bfull #bBtns .brow2 button { flex:1; min-height:48px; font-size:13px; letter-spacing:1px; border-radius:24px; padding:0 6px; }
  .bfull #bBtns button { min-height:62px; margin:0; font-size:20px; font-weight:700; letter-spacing:3px; border-radius:31px; }
  .bfull #bBtns button.big { grid-column:1 / -1; }
  .verdict { margin:14px 0 4px; padding:12px 14px; border:1px solid var(--line); border-radius:12px; }
  .verdict small { font-size:11px; letter-spacing:3px; color:var(--dim); }
  .verdict div:first-of-type { display:flex; justify-content:space-between; align-items:baseline; margin:4px 0 8px; }
  .verdict b { font-size:28px; letter-spacing:2px; }
  .verdict span { font-size:17px; font-weight:700; }
  .verdict .vbar { position:relative; height:10px; border-radius:5px; background:#141C2A; overflow:hidden; }
  .verdict .vbar i { position:absolute; top:0; bottom:0; }
  .verdict .vbar i.maybe { background:repeating-linear-gradient(45deg,#3A4B5E 0 5px,#2A3B50 5px 10px); }
  .verdict p { margin:8px 0 0; font-size:13px; color:var(--dim); line-height:1.35; }
  .informant { display:flex; align-items:center; gap:10px; width:100%; box-sizing:border-box; margin-top:8px; padding:10px 12px; background:none; border:1px solid #5FD4FF; border-radius:12px; color:#5FD4FF; text-align:left; font:inherit; }
  .informant span { flex:1; font-size:13px; font-weight:700; letter-spacing:1px; }
  .informant small { display:block; font-size:12px; font-weight:400; letter-spacing:0; color:var(--dim); margin-top:2px; }
  .informant b { font-size:14px; }
  .informant:disabled { opacity:.45; }
  .hired { display:flex; align-items:center; justify-content:center; gap:6px; margin:6px auto 0; padding:4px 10px; background:none; border:1px dashed #5A5448; border-radius:10px; color:var(--dim); font:inherit; font-size:12px; font-weight:700; letter-spacing:1px; min-height:32px; }
  .hired.on { border:1px solid #B8A88A; color:#B8A88A; }
  .atkline { display:block; width:100%; box-sizing:border-box; margin:10px 0 0; padding:8px 12px 10px; background:none; border:1px solid #3A2226; border-radius:12px; color:var(--text); font:inherit; text-align:left; }
  .atkline span { display:block; font-size:11px; letter-spacing:2px; color:var(--red); margin-bottom:2px; }
  .atkline div { display:flex; align-items:center; gap:8px; }
  .atkline b { font-size:16px; letter-spacing:1px; }
  .atkline small { font-size:12px; color:var(--dim); }
  .atkline i { font-style:normal; font-size:11px; font-weight:700; display:inline-flex; align-items:center; }
  .atkline em { margin-left:auto; font-style:normal; color:var(--dim); font-size:18px; }
  .atkline.off { color:var(--faint); font-size:12px; letter-spacing:2px; }
  .atkmore { padding:6px 12px 2px; }
  .atkmore div { display:flex; justify-content:space-between; font-size:13px; padding:4px 0; border-bottom:1px solid var(--line); }
  .atkmore p { font-size:12px; color:var(--dim); margin:6px 0 0; }
  .favcard { margin-top:10px; padding:12px 14px; border:1px solid var(--line); border-radius:12px; text-align:left; }
  .favcard .top { display:flex; justify-content:space-between; font-size:11px; letter-spacing:2px; }
  .favcard .txt { font-size:14px; margin:6px 0 8px; }
  .favcard .segs { display:flex; gap:4px; align-items:center; }
  .favcard .segs i { flex:1; height:6px; border-radius:3px; background:#2A3240; }
  .favcard .segs small { margin-left:8px; font-size:12px; color:var(--dim); white-space:nowrap; }
  .fcols .escort { display:flex; align-items:center; justify-content:center; gap:6px; margin-top:6px; font-size:12px; letter-spacing:1px; font-weight:700; }
  .fcols em { font-style:normal; font-size:12px; font-weight:700; color:var(--dim); border:1px solid var(--line); border-radius:50%; width:34px; height:34px; display:flex; align-items:center; justify-content:center; }
  .fchint { font-size:13px; color:var(--faint); margin:8px 0 0; }
  .odds { margin:14px 0 4px; }
  .odds div:first-child { display:flex; justify-content:space-between; align-items:flex-end; font-size:28px; font-weight:700; }
  .odds div:first-child small { display:block; font-size:12px; letter-spacing:2px; }
  .odds .tie { font-size:13px; color:var(--dim); padding-bottom:4px; }
  .odds .bar { display:flex; gap:2px; height:12px; border-radius:4px; overflow:hidden; margin-top:4px; }
  .fleets3 { display:grid; grid-template-columns:repeat(3, minmax(0, 1fr)); gap:8px; margin-top:12px; }
  .fleets3 button { margin:0; min-height:60px; border-radius:14px; border:1px solid var(--line); background:transparent; font-size:15px; font-weight:700; display:flex; flex-direction:column; align-items:center; justify-content:center; }
  .fleets3 button small { font-size:12px; font-weight:400; color:var(--dim); }
  .fleets3 button.sel { border:2px solid var(--accent); color:var(--accent); }
  .fleets3 button:disabled { color:var(--faint); border-style:dashed; }
  .acts { display:grid; grid-template-columns:repeat(3, minmax(0, 1fr)); gap:8px; margin-top:14px; }
  .acts button { margin:0; min-height:64px; border-radius:16px; font-size:16px; font-weight:700; letter-spacing:1px; display:flex; flex-direction:column; align-items:center; justify-content:center; }
  .acts small { font-size:13px; font-weight:600; }
  .acts .fight { border:2px solid var(--red); background:#1A0B0B; color:var(--red); }
  .acts .neg { border:2px solid var(--cyan); background:#081A26; color:var(--cyan); font-size:14px; }
  .acts .pay { border:2px solid #FFC933; background:#1A1506; color:#FFC933; }
  .note { margin-top:14px; padding:12px 14px; border:1px dashed var(--line); border-radius:14px; font-size:14px; color:var(--dim); text-align:left; }
  /* 2.0-dev.21 market shelves: planks on a dark wall of vertical boards */
  .shelves { background:repeating-linear-gradient(90deg, #0C0805 0 3px, #140D08 3px 42px); border-radius:16px; padding:12px 8px 2px; margin:6px 0 10px; }
  .shelf { margin:0 0 14px; text-align:left; }
  .mkTop { display:flex; justify-content:space-between; align-items:center; margin-bottom:6px; }
  .mkTop span { font-family:var(--display); font-size:20px; font-weight:700; letter-spacing:3px; }
  .mkTop b { display:flex; align-items:flex-end; gap:4px; } .mkTop small { font-size:11px; color:#FFD166; letter-spacing:2px; }
  .mkGrid { display:grid; grid-template-columns:repeat(2, minmax(0, 1fr)); grid-auto-flow:row dense; gap:10px; margin:6px 0 12px; }
  .mkGrid > .rvSay { grid-column:1 / -1; margin:0; }
  .mkCard { display:flex; flex-direction:column; gap:8px; padding:10px; border-radius:14px; border:2px solid #6B4A28; background:linear-gradient(#2A1C10, #160E08); text-align:left; }
  .mkCard.wide { grid-column:1 / -1; }
  .mkCard.shut { border:2px dashed #3A4A5C; background:#0C1118; color:#8FA3B8; }
  .mkHead { display:flex; justify-content:space-between; align-items:center; gap:6px; }
  .mkHead b { font-family:var(--display); font-size:11px; letter-spacing:2px; color:var(--fc); }
  .mkHead span { display:flex; align-items:center; gap:3px; font-size:9px; letter-spacing:1px; color:var(--dim); white-space:nowrap; }
  .mkGood { display:flex; flex-direction:column; gap:2px; }
  .mkGood b { font-size:15px; } .mkGood span { font-size:12px; color:#D8C8B0; line-height:1.3; }
  .mkCard.shut .mkGood b { color:#C9D2DC; } .mkCard.shut .mkGood span { color:#8FA3B8; }
  .mkBuy { margin:6px 0 0; min-height:0; height:38px; padding:0; border-radius:19px; border:none; background:var(--fc); color:#0B0805; font-weight:700; font-size:13px; letter-spacing:1px; }
  .mkBuy:disabled { background:#2A2F38; color:#8FA3B8; opacity:1; }
  .mkShut { margin-top:auto; font-size:12px; font-weight:700; color:#FFB347; }
  .mkhelp { font-size:13px; color:var(--dim); margin:2px 0 8px; text-align:left; line-height:1.35; }
  .mkhelp b { color:#FFD166; font-weight:600; }
  .stalls { display:grid; grid-template-columns:repeat(2, minmax(0, 1fr)); gap:8px; margin:0 0 10px; }
  .stall { display:flex; flex-direction:column; background:repeating-linear-gradient(90deg, #0C0805 0 3px, #140D08 3px 42px); border:1px solid #2A1C10; border-radius:12px; padding:8px 6px; text-align:left; }
  .stall .shelfsvg { width:100%; height:auto; display:block; }
  .stall.gone { opacity:.55; }
  .sttop { font-size:11px; font-weight:700; letter-spacing:1px; margin:0 2px 2px; white-space:nowrap; overflow:hidden; text-overflow:ellipsis; }
  .ststock { display:flex; align-items:center; gap:4px; margin:2px 2px 4px; font-size:11px; font-weight:700; letter-spacing:1px; color:#8CE7A0; }
  .stall.gone .ststock { color:#FF6B5A; }
  .ststock i { width:10px; height:10px; border-radius:2px; background:#24160C; border:1px solid #3A2A18; }
  .ststock i.on { background:#8CE7A0; border-color:#8CE7A0; }
  .stfx { font-size:12px; color:#C9D6E2; line-height:1.3; margin:0 2px; flex:1; }
  .stcut { font-size:11px; color:var(--faint); margin:4px 2px 6px; line-height:1.25; }
  .stbuy { width:100%; min-height:40px; border-radius:20px; border:2px solid #FFD166; background:#1E1A08; color:#FFD166; font:inherit; font-size:14px; font-weight:700; letter-spacing:1px; }
  .stbuy:disabled { border-color:#3A2A18; color:#6A5A44; background:none; }
  .shtop { font-size:12px; font-weight:700; letter-spacing:2px; margin:0 4px; }
  .shrow { display:flex; align-items:center; gap:6px; }
  .shelfsvg { flex:1; min-width:0; width:100%; height:auto; display:block; }
  .buy { margin:0 0 30px; min-width:84px; min-height:52px; border-radius:26px; border:2px solid #FFD166; background:#1E1A08; color:#FFD166;
         font-weight:700; letter-spacing:2px; display:flex; flex-direction:column; align-items:center; justify-content:center; padding:4px 10px; }
  .buy small { font-size:13px; letter-spacing:0; }
  .buy:disabled { border-color:#5A4A20; color:#7A6A40; }
  .soldout { min-width:84px; margin-bottom:30px; text-align:center; color:#FF6B5A; font-size:12px; font-weight:700; letter-spacing:1px; }
  .shfx { background:rgba(5,7,12,.8); border-radius:8px; padding:6px 10px; font-size:14px; color:#9FB3C6; }
  .shfx span { color:var(--faint); font-size:12px; }
  /* 2.0-dev.14 HOW TO PLAY primer: picture cards that go with the rulebook */
  .prim { min-height:calc(100vh - 40px); display:flex; flex-direction:column; }
  .primtop { display:flex; justify-content:space-between; align-items:center; font-size:12px; letter-spacing:3px; color:var(--dim); }
  .primtop .back { margin:0; }
  .primk { font-size:12px; letter-spacing:4px; color:var(--dim); margin-top:14px; }
  .primt { font-size:26px; font-weight:700; letter-spacing:3px; margin:2px 0 10px; }
  .primpic { border:1px solid var(--ring); border-radius:18px; background:#070B12; padding:12px; margin:0 0 12px; }
  .primb { text-align:left; font-size:16px; line-height:1.4; flex-grow:1; }
  .primb p { margin:0 0 10px; }
  .primb b { color:var(--text); }
  .primdots { display:flex; justify-content:center; gap:6px; margin:6px 0 2px; }
  .prim .rvSay { margin:0 0 12px; font-size:14px; line-height:1.4; }
  .primpic .ptrack { margin:0 0 6px; } .primpic .wtl { margin:0; } .primpic .rdo { margin:0; }
  .gdlist { margin-top:10px; } .gdlist .gyrow { padding:12px; font-size:14px; } .gdlist .gyrow b { font-size:12px; display:block; margin-bottom:2px; }
  .prim > .back { display:inline-flex; align-items:center; gap:8px; justify-content:center; }
  .primdots i { width:7px; height:7px; border-radius:50%; background:var(--ring); }
  .primnav { display:grid; grid-template-columns:1fr 2fr; gap:10px; }
  .primnav .pill { margin-top:8px; min-height:60px; font-size:18px; }
  .prow { display:flex; align-items:center; gap:12px; text-align:left; margin:12px 0; }
  .prow .ic { flex:0 0 44px; display:flex; justify-content:center; }
  .prow b { display:block; font-size:14px; letter-spacing:2px; }
  .prow span { font-size:14px; color:var(--dim); }
  .pworlds { display:grid; grid-template-columns:repeat(3, 1fr); gap:6px; }
  .pworlds div { display:flex; flex-direction:column; align-items:center; gap:2px; font-size:11px; letter-spacing:2px; color:var(--dim); }
  .pworlds b { font-size:22px; }
  .pdie { width:54px; height:54px; box-sizing:border-box; border:3px solid; border-radius:12px; display:flex; align-items:center; justify-content:center; font-size:26px; font-weight:700; }
  .pfight { display:grid; grid-template-columns:1fr auto 1fr; align-items:center; gap:8px; }
  .pfight > div { display:flex; flex-direction:column; align-items:center; gap:8px; font-size:11px; letter-spacing:2px; color:var(--dim); }
  .ptiers { display:grid; grid-template-columns:repeat(5, 1fr); font-size:10px; letter-spacing:1px; margin-top:8px; }
  .gmap .pw { position:absolute; width:14.6%; height:14.6%; transform:translate(-50%, -50%); border-radius:50%; border:2px solid var(--line);
              background:#05070C; display:flex; align-items:center; justify-content:center; box-sizing:border-box; }
  .gmap .pw.core { width:12.9%; height:12.9%; }
  .gmap .pw img { width:74%; height:74%; }
  .credits { color:#FFD166; font-size: 15px; }
  .phase { color:#FFD166; font-size: 17px; margin: 6px 0; }
  .alert { color:#FF6B6B; font-weight:bold; margin: 8px 0; min-height: 20px; }
  .planet { border:1px solid #223; border-radius:10px; padding: 10px; margin: 8px 0; }
  .row { display:flex; justify-content:space-between; align-items:center; padding: 4px 0;
         border-top:1px solid #1a2230; font-size: 14px; }
  .row:first-child { border-top:none; }
  .val { color:#A9DDF1; } .unknown { color:#5a6c78; font-style:italic; }
  .foot { margin-top: 24px; font-size: 11px; color:#3a4a56; }
  .offer { border:2px solid #FFD166; border-radius:12px; padding: 14px; margin: 10px 0; text-align:left; }

  /* Faction Transmission overlay -- 3.1-dev.75 rework (canvas board 6, user: "big, not too skippable, not too much to read
     unless you want to dig into it"): TRANSMISSION INCOMING (rings + the caller's symbol), then the hologram in a full-width
     frame that pulses in the caller's colour, one spoken line, the changes popping in as chips, WHY? for the rest, and a
     CONTINUE that fills before it works. */
  #txOverlay { position:fixed; inset:0; z-index:1000; display:none; overflow-y:auto; --tc:#5FD4FF;
               background:radial-gradient(70% 38% at 50% 24%, color-mix(in srgb, var(--tc) 20%, transparent), transparent 70%), #03050A; }
  #txOverlay.active { display:block; }
  #txOverlay::before { content:''; position:fixed; inset:0; pointer-events:none; background:repeating-linear-gradient(0deg, color-mix(in srgb, var(--tc) 5%, transparent) 0 1px, transparent 1px 4px); }
  #txIncoming { position:fixed; inset:0; display:none; flex-direction:column; align-items:center; justify-content:center; gap:22px; }
  #txOverlay.incoming #txIncoming { display:flex; }
  #txOverlay.incoming #txContent { display:none; }
  .txRings { position:relative; width:170px; height:170px; display:flex; align-items:center; justify-content:center; }
  .txRings > i { position:absolute; inset:0; border-radius:50%; border:2px solid var(--tc); animation:txRing 1.4s ease-out infinite; }
  .txRings > i + i { animation-delay:.7s; }
  .txRings > span { width:112px; height:112px; border-radius:50%; border:1px solid color-mix(in srgb, var(--tc) 40%, #0A0814); background:#0A0814;
                    display:flex; align-items:center; justify-content:center; animation:txLock 1.2s steps(1) infinite; filter:drop-shadow(0 0 8px var(--tc)); }
  @keyframes txRing { 0% { transform:scale(.6); opacity:.9; } 100% { transform:scale(1.5); opacity:0; } }
  @keyframes txLock { 0%, 30% { opacity:.25; transform:translateX(-3px); } 34% { opacity:.8; transform:translateX(2px); } 40%, 100% { opacity:1; transform:none; } }
  .txInT { font-family:var(--display); font-size:13px; letter-spacing:6px; color:#8FA3B8; }
  .txInF { font-family:var(--display); font-size:24px; letter-spacing:4px; font-weight:700; color:var(--tc); text-shadow:0 0 14px var(--tc); text-align:center; padding:0 16px; }
  .txDots { display:flex; gap:6px; } .txDots i { width:6px; height:6px; border-radius:50%; background:var(--tc); animation:txDot 1s infinite; }
  .txDots i:nth-child(2) { animation-delay:.2s; } .txDots i:nth-child(3) { animation-delay:.4s; }
  @keyframes txDot { 0%, 100% { opacity:.2; } 50% { opacity:1; } }
  .txInTap { position:absolute; bottom:calc(env(safe-area-inset-bottom, 0px) + 32px); font-family:ui-monospace, Menlo, Consolas, monospace; font-size:11px; letter-spacing:2px; color:#5C6F84; }
  #txContent { position:relative; max-width:440px; margin:0 auto; padding:calc(env(safe-area-inset-top, 0px) + 16px) 16px calc(env(safe-area-inset-bottom, 0px) + 18px);
               min-height:100%; box-sizing:border-box; display:flex; flex-direction:column; gap:12px; }
  #txKick { display:flex; align-items:center; justify-content:center; gap:8px; font-family:var(--display); font-size:12px; letter-spacing:4px; color:#8FA3B8; }
  #txKick b { color:var(--tc); font-weight:500; } #txKick.prio, #txKick.prio b { color:#FFD166; }
  #txHolo { position:relative; height:170px; }
  #txHolo > .txP { position:absolute; inset:10px 0; border-radius:14px; border:2px solid var(--tc); animation:txPulseR 2.4s ease-out infinite; }
  #txHolo > .txP + .txP { animation-delay:1.2s; }
  @keyframes txPulseR { 0% { transform:scale(.98, .94); opacity:.85; } 100% { transform:scale(1.04, 1.22); opacity:0; } }
  #txFrame { position:absolute; inset:10px 0; border-radius:14px; overflow:hidden; border:2px solid color-mix(in srgb, var(--tc) 70%, transparent);
             background:repeating-linear-gradient(0deg, rgba(95,212,255,.10) 0 2px, transparent 2px 4px), #06121A; box-shadow:0 0 30px color-mix(in srgb, var(--tc) 35%, transparent);
             display:flex; align-items:flex-end; justify-content:center; }
  #txFrame .txGlow { position:absolute; left:0; right:0; bottom:0; height:40px; background:radial-gradient(60% 100% at 50% 100%, rgba(95,212,255,.35), transparent); }
  #txFrame .txScan { position:absolute; left:0; right:0; top:0; height:16px; background:linear-gradient(180deg, transparent, rgba(95,212,255,.25), transparent); animation:txScan 2.8s linear infinite; }
  @keyframes txScan { 0% { transform:translateY(-20px); } 100% { transform:translateY(170px); } }
  #txFrame .txRelay, #txFrame .txLive { position:absolute; top:9px; font-family:ui-monospace, Menlo, Consolas, monospace; font-size:9px; letter-spacing:1px; opacity:.8; }
  #txFrame .txRelay { left:12px; color:#5FD4FF; } #txFrame .txLive { right:12px; color:var(--tc); }
  #txPortrait { position:relative; height:100%; display:flex; align-items:flex-end; }
  #txPortrait img { height:128px; width:auto; animation:txflicker 3.2s steps(1) infinite; }
  @keyframes txflicker { 0%{opacity:.92} 46%{opacity:.92} 47%{opacity:.55} 49%{opacity:.92} 83%{opacity:.8} 84%{opacity:.92} }
  #txNameTag { text-align:center; font-family:var(--display); font-size:14px; font-weight:700; letter-spacing:3px; color:var(--tc); }
  #txBody { text-align:center; font-size:19px; line-height:1.35; font-weight:500; color:#E8EEF5; min-height:52px; }
  #txChips { display:flex; flex-direction:column; gap:8px; }
  .txChip { position:relative; overflow:hidden; display:flex; align-items:center; gap:10px; border-radius:14px; padding:10px 14px; border:1px solid var(--line);
            background:#0A0F16; opacity:0; transform:translateY(10px) scale(.96); transition:opacity .4s var(--ease), transform .4s var(--ease); }
  #txOverlay.shown .txChip { opacity:1; transform:none; }  /* each chip's transition-delay staggers the pop-in */
  .txChip > b { flex:1; min-width:0; text-align:left; font-family:var(--display); font-size:15px; font-weight:700; letter-spacing:2px; color:var(--text); }
  .txChip > em { font-style:normal; font-family:var(--display); font-size:16px; font-weight:700; letter-spacing:2px; white-space:nowrap; }
  .txChip.up { border-color:#2E4A38; background:rgba(140,231,160,.07); } .txChip.up > em { color:#8CE7A0; }
  .txChip.dn { border-color:#4A2A2E; background:rgba(255,107,90,.07); } .txChip.dn > em { color:#FF6B5A; }
  .txChip.gift { border-color:#FFD166; background:rgba(255,209,102,.08); } .txChip.gift > b, .txChip.gift > em { color:#FFD166; font-size:14px; }
  .txChip.gift::after { content:''; position:absolute; top:0; bottom:0; left:0; width:40%; background:linear-gradient(90deg, transparent, rgba(255,209,102,.25), transparent);
                        transform:translateX(-120%); animation:txShine 2.6s ease-in-out 1.1s infinite; }
  .txChip.lost { border-color:#4A2A2E; } .txChip.lost > b, .txChip.lost > em { color:#FF6B5A; font-size:14px; }
  @keyframes txShine { 0% { transform:translateX(-120%); } 60%, 100% { transform:translateX(260%); } }
  #txWhy { align-self:center; min-height:0; margin:0; padding:8px 14px; border:none; background:none; color:#8FA3B8; font-family:var(--display); font-size:12px; letter-spacing:2px; }
  #txRead { display:none; } #txOverlay.why #txRead { display:block; } #txRead .rdo { margin-right:6px; }
  #txCont { position:relative; overflow:hidden; margin:auto 0 0; min-height:56px; border-radius:999px; border:2px solid var(--tc); background:transparent; color:#E8EEF5;
            font-family:var(--display); font-size:18px; letter-spacing:4px; font-weight:700; }
  #txCont > i { position:absolute; left:0; top:0; bottom:0; width:0; background:color-mix(in srgb, var(--tc) 35%, transparent); }
  #txOverlay.shown #txCont > i { animation:txHold 1.6s linear forwards; }
  @keyframes txHold { to { width:100%; } }
  #txCont > span { position:relative; } #txCont:not(.ready) { opacity:.75; }
  @media (prefers-reduced-motion: reduce) { .txRings > i, .txRings > span, #txHolo > .txP, #txFrame .txScan, .txDots i, .txChip.gift::after { animation:none !important; }
    .txChip { transition:none; } }
</style>
</head>
<body>
<h1>SHADOW FLEET</h1>
<div id="app">Connecting...</div>
<div id="banner" onclick="hideBanner()"></div>
<div id="lyrSheet"><div class="scrim" onclick="sheetScrimTap()"></div><div class="sheetBox"><div class="grab"></div><div id="sheetBody"></div></div></div>
<div id="lyrNotif"></div>
<div id="eraFrame"></div>
<div id="lyrAnn" onclick="annTap(event)"><div class="annCard" id="annCard"></div></div>
<div class="foot">Firmware <span id="fw">--</span></div>

<div id="txOverlay" onclick="txTap(event)">
  <div id="txIncoming"><div class="txRings"><i></i><i></i><span id="txInSym"></span></div>
    <div class="txInT" id="txTitle">TRANSMISSION INCOMING</div><div class="txInF" id="txInF"></div><div class="txDots"><i></i><i></i><i></i></div><div class="txInTap">TAP TO ANSWER</div></div>
  <div id="txContent">
    <div id="txKick"></div>
    <div id="txHolo"><i class="txP"></i><i class="txP"></i><div id="txFrame"><span class="txRelay" id="txRelay"></span><span class="txLive">&#9679; LIVE</span><div id="txPortrait"></div><i class="txScan"></i><i class="txGlow"></i></div></div>
    <div id="txNameTag"></div>
    <div id="txBody"></div>
    <div id="txChips"></div>
    <button id="txWhy" onclick="txWhyTap(event)">WHY? &#9662;</button>
    <div id="txRead"></div>
    <button id="txCont" onclick="txContTap(event)"><i></i><span>CONTINUE</span></button>
  </div>
</div>

<script>
const PAGE_FW = 'v3.1-dev.126';  // 3.1-dev.44: written by tools/page/make_page_gz.py from version.h -- the phone reloads when the table runs something else
let myId = localStorage.getItem('sf_id');
if (!myId) { myId = 'p' + Math.floor(Math.random()*1e9); localStorage.setItem('sf_id', myId); }
let mySlot = -1;
let joined = false;
let currentPlanet = -1;

async function api(path, opts) {
  try { const r = await fetch(path, opts); return await r.json(); } catch(e) { return null; }
}
async function post(path) { return api(path, { method:'POST' }); }

async function pickSlot(slotIdx) {
  const dbg = document.getElementById('joinDebug');
  if (dbg) dbg.textContent = '';
  let r = null, errMsg = '';
  try {
    const resp = await fetch('/join?id=' + encodeURIComponent(myId) + '&slot=' + slotIdx, { method:'POST' });
    errMsg = 'HTTP ' + resp.status;
    r = await resp.json();
  } catch (e) { errMsg = 'FETCH ERROR: ' + e; }
  if (dbg) dbg.textContent = r && !(r.slot < 0) && r.ok !== false ? '' : 'That didn’t go through. Tap again.';  // 3.1-dev.114 (playtest: the debug line looked like an error)
  if (r && r.slot >= 0) { mySlot = r.slot; joined = true; }
  poll();
}
async function pickFaction(f) {
  cmdBack = false; cmdPick = 0; cmdEdit = true;  // v0.21.20: next, the commander step
  const dbg = document.getElementById('joinDebug');
  if (dbg) dbg.textContent = '';
  let r = null, errMsg = '';
  try {
    const resp = await fetch('/choose_faction?id='+encodeURIComponent(myId)+'&faction='+f, { method:'POST' });
    errMsg = 'HTTP ' + resp.status;
    r = await resp.json();
  } catch (e) { errMsg = 'FETCH ERROR: ' + e; }
  if (dbg) dbg.textContent = r && !(r.slot < 0) && r.ok !== false ? '' : 'That didn’t go through. Tap again.';  // 3.1-dev.114 (playtest: the debug line looked like an error)
  poll();
}
// v0.21.1: confirm first if something is left on the table (FE's "End turn?"):
// 2+ Credits unspent, a fleet ready to fight in Resolution, or offers open.
function lockLeftovers(s) {
  const me = s.players[mySlot];
  // v0.30.2: every ready fleet (1-based), not just the first.
  const readyFleets = s.phase === 2 ? me.fleets.map((f, i) => (f.alive > 0 && !f.spent) ? i + 1 : 0).filter(n => n) : [];
  // 2.0-dev.34 (playtest): unspent Credits only matter in Prepare -- Movement / Resolution don't nag about them.
  return { credits: s.phase === 0 && me.credits >= 2 ? me.credits : 0, readyFleets: readyFleets,
           offers: s.takeoverPlayer === mySlot && s.takeoverMethod === 2 && !!s.neg && s.neg.result === 0,
           deals: 0 };  // (3.1-dev.20: deals are cut)
}
async function lockIn() {
  if (lastS && !lockConfirmPending) {
    const l = lockLeftovers(lastS);
    // 3.1-dev.114 (user: "if you have to hold the lock in button, I don't think we need the second lock in page"): only an open negotiation still asks
    if (l.offers) { lockConfirmPending = true; poll(); return; }
  }
  lockConfirmPending = false; if (sheetKey === 'lock') sheetHide();
  await post('/lockin?id=' + encodeURIComponent(myId)); poll();
}
function cancelLockIn() { lockConfirmPending = false; if (sheetKey === 'lock') sheetHide(); poll(); }

// v0.21.1 round/phase banner (phone twin of the AMOLED's showRoundBanner()).
let bannerKey = -1, bannerTimer = null;
function bannerOpen() { return document.getElementById('banner').classList.contains('active'); }
function hideBanner() { document.getElementById('banner').classList.remove('active'); if (bannerTimer) { clearTimeout(bannerTimer); bannerTimer = null; } }
function showBanner(s) {
  const b = document.getElementById('banner');
  // 3.1-dev.17: the HYPERSPACE WIPE -- pixel streaks race out from the middle, then the phase lands.
  const PC = ['#FFD166', '#5FD4FF', '#FF6B5A'][s.phase] || '#5FD4FF';
  let h = '<div class="hsw">';
  for (let k = 0; k < 56; k++) {  // stars at every angle, each stretching into a line as it races outward
    const a = (k * 137.5) % 360, r0 = 6 + (k * 29) % 60, len = 40 + (k * 53) % 120, dl = ((k * 31) % 7) * 25;
    h += '<i style="--a:' + a.toFixed(1) + 'deg;--r:' + r0 + 'px;width:' + len + 'px;animation-delay:' + dl + 'ms;opacity:' + (0.45 + ((k * 13) % 6) / 10) + '"></i>';
  }
  h += '<b class="hsFlash"></b></div><div class="hsBody" style="--pc:' + PC + '"><div class="lbl">ROUND</div><div class="num">' + pnumSvg(s.round, '#E8EEF5', 12) + '<small>/ ' + s.roundLimit + '</small></div>';
  h += '<div class="ph" style="color:' + PC + '">' + s.phaseName + '</div>';
  if (s.phase === 1) h += '<div class="inc" style="color:#E8EEF5">MOVE YOUR SHIPS ON THE BOARD NOW</div>';  // 3.1-dev.37 (playtest: felt like he should move on the phone)
  // v0.23.1: a new round pays income -- say how much, on its Invest banner.
  const me = s.players[mySlot];
  if (s.phase === 0 && s.round > 1 && me && me.income > 0) {
    h += '<div class="inc">+'+me.income+' CR INCOME' + eraTag(s, 2, (s.v2 && s.v2.champ && s.v2.champ[2] === mySlot) ? '\u00d73 SPICE AGE' : '\u00d72 SPICE AGE') + '</div>';
    s.planets.forEach(p => { if (p.owner === mySlot && p.traitFx && /CR every Prepare/.test(p.traitFx)) h += '<div class="incT">'+p.traitFx.split(' ')[0]+' \u00b7 '+(p.chars && p.chars[2] ? p.chars[2].value : 'a trait')+'</div>'; });  // 3.1-dev.19
  }
  h += '<div class="tap">TAP TO CONTINUE</div></div>';
  b.innerHTML = h;
  b.classList.add('active');
  if (bannerTimer) clearTimeout(bannerTimer);
  bannerTimer = setTimeout(hideBanner, 3000);
}
// Banner a round/phase change once, but never over a battle you're in.
function maybeBanner(s) {
  if (s.stage !== 1 || mySlot < 0) return;
  const key = s.round * 4 + s.phase;
  if (key === bannerKey) return;
  const inMyBattle = s.battleActive && (s.battleAttackerSlot === mySlot || s.battleDefenderSlot === mySlot);
  if (inMyBattle) return;
  bannerKey = key;
  showBanner(s);
}
// v0.30.3: END GAME mid-game (Standings). 2.0-dev.6 (user: "too easy to click,
// and there should be an 'are you sure?' page"): it opens a confirm card --
// KEEP PLAYING big, END GAME small and apart.
let endConfirm = false;
// 2.0-dev.13 drop out / rejoin
async function stepAway() { standingsMode = false; await post('/away?id=' + encodeURIComponent(myId)); poll(); }
async function comeBack() { await post('/back?id=' + encodeURIComponent(myId)); poll(); }
async function welcomeAck() { await post('/welcome_ack?id=' + encodeURIComponent(myId)); poll(); }
function endGameTap() { endConfirm = true; if (lastS) render(lastS); }
function endGameKeep() { endConfirm = false; if (lastS) render(lastS); }
async function endGameGo() { endConfirm = false; standingsMode = false; await post('/end_game?id=' + encodeURIComponent(myId)); poll(); }
async function setRoundLimit(rounds) { await post('/set_round_limit?rounds=' + rounds); poll(); }
async function setMarkets(on) { await post('/set_markets?on=' + (on ? 1 : 0)); poll(); }  // 2.0-dev.32
async function setEvents(on) { await post('/set_events?on=' + (on ? 1 : 0)); poll(); }    // 2.0-dev.33
async function startGame() { await post('/start_game?id=' + encodeURIComponent(myId)); poll(); }
// v0.27.3: NEW GAME at game over -- two taps (the first arms it for 4 s), like the AMOLED.
let newGameArmed = 0;
async function newGameTap() {
  if (Date.now() - newGameArmed < 4000) { newGameArmed = 0; await post('/new_game?id=' + encodeURIComponent(myId)); poll(); }
  else { newGameArmed = Date.now(); if (lastS) render(lastS); }
}
async function reveal(p,c) { await post('/reveal?id='+encodeURIComponent(myId)+'&planet='+p+'&char='+c); poll(); }
async function invest(p) { await post('/invest?id='+encodeURIComponent(myId)+'&planet='+p); poll(); }
async function beginTakeover(p) { payConfirmPending = false; briefingPending = false; leaveNegPending = false; await post('/takeover_begin?id='+encodeURIComponent(myId)+'&planet='+p); poll(); }
async function commit(m) { await post('/takeover_commit?id='+encodeURIComponent(myId)+'&method='+m); poll(); }
// v0.21.6 planet-page approach buttons: open the session, then go straight
// to that approach's next step (twins of the AMOLED planet*Cb callbacks).
async function planetFight(p) { await beginTakeover(p); showBriefing(); }
async function planetNegotiate(p, sure) {
  if (!sure) { const w = bcWarn(lastS, p, 'talk'); if (w) return bcConfirm(w, 'planetNegotiate(' + p + ', true)'); }  // 3.1-dev.98
  negBase = lastS ? lastS.reputation.map(r => r.score || 0) : null; await beginTakeover(p); await commit(2); }
// v0.30.0 the conversation: a reply (bank index), a favor (0..2) or 3 = refuse; CONTINUE closes the result.
let negBase = null;  // your scores when the conversation started (the result card's arrows)
async function negAnswer(i) { await post('/negotiate_answer?id='+encodeURIComponent(myId)+'&choice='+i); poll(); }
async function negClose() { leaveNegPending = false; await post('/negotiate_close?id='+encodeURIComponent(myId)); poll(); }
// v0.30.1: they're hostile -- FIGHT goes straight to the battle forecast on that planet.
async function negFight() { await post('/negotiate_fight?id='+encodeURIComponent(myId)); showBriefing(); }
async function planetPay(p, sure) {
  if (!sure) { const w = bcWarn(lastS, p, 'talk'); if (w) return bcConfirm(w, 'planetPay(' + p + ', true)'); }  // 3.1-dev.98
  await beginTakeover(p); showPayConfirm();
}
async function leavePlanet() {
  const s = lastS;
  if (s && s.takeoverPlayer === mySlot && s.takeoverMethod !== 2) await post('/takeover_cancel?id='+encodeURIComponent(myId));
  currentPlanet = -1; poll();
}
function showPayConfirm() { payConfirmPending = true; poll(); }
// Battle briefing (v0.20.1): FIGHT opens it; CONFIRM FIGHT commits with the chosen fleet.
function showBriefing() { briefingPending = true; briefFleet = -1; briefSnap = ''; poll(); }
// v0.28.0: briefFleet is a MASK of the fleets sent in (-1 = the first ready one); tapping toggles.
function pickBriefFleet(f) { briefFleet = (briefFleet < 0 ? 0 : briefFleet) ^ (1 << f); poll(); }
// Exact odds for one exchange, d6+atk vs d6+def (tie = both miss); same
// rounding as exchangeOdds() in the .ino.
function exchangeOdds(atk, def) {
  let w = 0, l = 0;
  for (let a = 1; a <= 6; a++) for (let d = 1; d <= 6; d++) { if (a + atk > d + def) w++; else if (a + atk < d + def) l++; }
  const win = Math.floor((w * 100 + 18) / 36), lose = Math.floor((l * 100 + 18) / 36);
  return { win: win, tie: 100 - win - lose, lose: lose };
}
// 2.0-dev.27 forecast v2: the chance A ships beat D in a fight to the end
// (d6+am vs d6+dm per exchange, ties replayed, rerolls ignored).
// 3.1-dev.7: ships the class rules add to MY side of a fight I start (owner = the target's holder, -1 = none / PvP).
function classAtk(s, owner, pl) {
  const me = s.players[mySlot]; if (!me || me.commander < 0) return 0;
  let n = [1, 0, 0, 0][me.commander % 4];  // 3.1-dev.101: the Consul's soft hands is gone
  // 3.1-dev.101 (playtest: "the percentages need to accurately show if ships will be joining"): what you bring along
  if (((s.bm || {}).hold || []).includes(7)) n += 1;  // a hired blade in your hold
  if (tierIdx(s, 1) === 4) n += 1;                    // the Empire's ship of the line
  const w = s.v2 && s.v2.war;
  if (pl && w && w.by === mySlot && s.planets[w.w] === pl && s.phase === 2) n += 3;  // your warpath
  const o = owner >= 0 && owner !== mySlot ? s.players[owner] : null;
  if (o && o.commander >= 0 && o.commander % 4 === 2) n += 1;  // their Infiltrator spreads thin
  return n;
}
function battleWinPct(A, D, am, dm, pvp) {
  if (A <= 0) return 0; if (D <= 0) return 100;
  // 3.1-dev.102 THE BIGGER SIDE (planet battles; PvP stays plain): 3+ more ships at the start = it wins ties, and after 2 lost
  // exchanges in a row it rolls best of 2 once (the steady hand). Exact, by walking every state of the fight.
  const lead = pvp ? 0 : A - D, big = lead >= 3 ? 1 : -lead >= 3 ? 2 : 0;
  const pm = (k, v) => (Math.pow(v, k) - Math.pow(v - 1, k)) / Math.pow(6, k);
  const ex = (ka, kd) => { let w = 0, l = 0, t = 0;
    for (let a = 1; a <= 6; a++) for (let d = 1; d <= 6; d++) { const pr = pm(ka, a) * pm(kd, d), x = a + (am || 0), y = d + (dm || 0); if (x > y) w += pr; else if (x < y) l += pr; else t += pr; }
    if (big === 1) w += t; else if (big === 2) l += t; return [w, l]; };
  if (big) {
    const N = ex(1, 1), S = big === 1 ? ex(2, 1) : ex(1, 2), memo = {};
    const win = (a, d, run) => { if (d <= 0) return 1; if (a <= 0) return 0; const k = a + ',' + d + ',' + run; if (k in memo) return memo[k];
      const [w, l] = run >= 2 ? S : N, sum = w + l, pw = w / sum, pl = l / sum;
      const bigLost = big === 1 ? pl : pw;  // chance the bigger side loses this exchange
      const r = big === 1 ? pw * win(a, d - 1, 0) + pl * win(a - 1, d, run >= 2 ? 0 + 1 : run + 1)
                          : pw * win(a, d - 1, run >= 2 ? 1 : run + 1) + pl * win(a - 1, d, 0);
      void bigLost; return memo[k] = r; };
    return Math.round(win(A, D, 0) * 100);
  }
  const [w0, l0] = ex(1, 1), p = w0 / (w0 + l0), q = 1 - p;
  let sum = 0, c = 1;  // c = C(D-1+k, k)
  for (let k = 0; k < A; k++) { sum += c * Math.pow(p, D) * Math.pow(q, k); c = c * (D + k) / (k + 1); }
  return Math.round(sum * 100);
}
// A range of chances -> the word (STRONG / FAVORED / RISKY / LONG SHOT, or UNCLEAR when it's too wide to call).
function verdictOf(lo, hi) {
  const m = lo;  // 3.1-dev.101 (playtest: "I went in with a 90% chance"): the word follows the WORST case
  if (m >= 85) return { word: 'STRONG', col: '#7FE0A0' };
  if (m >= 60) return { word: 'FAVORED', col: 'var(--accent)' };
  if (m >= 35) return { word: 'RISKY', col: '#FFB13D' };
  return { word: 'LONG SHOT', col: 'var(--red)' };
}
function verdictHtml(lo, hi, note) {
  const v = verdictOf(lo, hi), rng = lo === hi ? lo + '%' : lo + '\u2013' + hi + '%';
  return '<div class="verdict"><small>' + kw('IF YOU FIGHT IT OUT') + '</small><div><b style="color:'+v.col+'">'+v.word+'</b><span style="color:'+v.col+'">'+rng+'</span></div>' +
         '<div class="vbar"><i style="left:0;width:'+lo+'%;background:'+v.col+'"></i><i class="maybe" style="left:'+lo+'%;width:'+(hi - lo)+'%"></i></div>' +
         (note ? '<p>'+note+'</p>' : '') + '</div>';
}
// 2.0-dev.29 "IF YOU ATTACK HERE", one line (user: "could be simpler"): your best fleet's
// word, the escort / militia / hired icons. Tap: the full forecast in Resolution,
// the details here in Prepare / Movement.
let attackOpen = -1;
function attackLine(s, i) {
  const p = s.planets[i], me = s.players[mySlot];
  if (!p || !me || p.owner === mySlot || !p.gr) return '';
  const res = s.phase === 2;
  const fl = me.fleets.map((f, k) => ({ k: k, n: f.alive, ok: f.alive > 0 && (!res || !f.spent) })).filter(f => f.ok).sort((a, b) => b.n - a.n);
  const sym = p.chars[0].revealed ? AFFILIATION_TO_FACTION.indexOf(p.chars[0].value) : -1;
  const tierOf = f => (s.reputation[f] ? s.reputation[f].tier : '').toUpperCase();
  const esc = sym >= 0 && ['FAVORABLE', 'ALLIED'].includes(tierOf(sym ^ 1)) ? 1 : 0;  // rivals: Rebels/Empire, Pykes/Crimson Dawn
  const milSure = sym >= 0 && tierOf(sym) === 'HOSTILE';  // dev.31: no "MILITIA?" (user: not needed) -- the range already allows it
  const merc = (s.v2 && s.v2.perks && s.v2.perks.merc) || 0;
  if (!fl.length) return '<div class="atkline off">\u2694 IF YOU ATTACK \u00B7 <span>no fleet '+(res ? 'ready' : 'built')+'</span></div>';
  const odds = f => { const a = Math.max(1, f.n + esc + merc + classAtk(s, p.owner, p)); return [battleWinPct(a, p.gr[1], 0, 0), battleWinPct(a, p.gr[0], 0, 0)]; };
  const best = fl[0], [lo, hi] = odds(best), v = verdictOf(lo, hi);
  const icons = (esc ? '<i title="escort">'+insigniaSvg(sym ^ 1, 14)+'</i>' : '') + (merc ? '<i style="color:#B8A88A">+'+merc+'</i>' : '') +
                (milSure ? '<i style="color:var(--red)">MILITIA</i>' : '');
  // dev.31 (user: "doesn't fit very well"): the label on its own row, the verdict + fleet below it.
  let h = '<button class="atkline" onclick="'+(res && !p.locked ? 'planetFight('+i+')' : 'attackOpen = attackOpen === '+i+' ? -1 : '+i+'; poll()')+'">' +
          '<span>\u2694 IF YOU ATTACK</span><div><b style="color:'+v.col+'">'+v.word+'</b><small>Fleet '+(best.k + 1)+' \u00B7 '+best.n+' ships</small>'+icons+'<em></em></div></button>';
  if (attackOpen === i && !res) {  // the details, outside Resolution
    h += '<div class="atkmore">' + fl.map(f => { const [a, b] = odds(f), w = verdictOf(a, b); return '<div><span>Fleet '+(f.k + 1)+' \u00B7 '+f.n+' ships</span><b style="color:'+w.col+'">'+w.word+' '+(a === b ? a : a+'\u2013'+b)+'%</b></div>'; }).join('') +
         '<p>Defenders: '+(p.gr[0] === p.gr[1] ? 'about '+p.gr[0] : 'usually '+p.gr[0]+'\u2013'+p.gr[1])+(esc ? ' \u00B7 an escort flies with you' : '')+(merc ? ' \u00B7 '+merc+' hired ships' : '')+
         (sym < 0 ? ' \u00B7 reveal its Sympathy to firm this up' : '')+'</p></div>';
  }
  return h;
}
// 2.0-dev.31: what a market world's holder earns per sale (MARKET_CUT 1, MARKET_CUT_CORE 2 in game_state.h).
function marketCutLine(s, i, short) {
  const p = s.planets[i]; if (!p) return '';
  const cut = (p.ring === 2 ? 2 : 1) + ' CR';
  if (p.owner === mySlot) return 'You hold it: you earn ' + cut + ' from every sale here.';
  if (p.owner >= 0) { const f = s.players[p.owner].chosenFaction; return (f >= 0 ? FACTION_SHORT_NAMES[f] : 'Its holder') + ' earns ' + cut + ' from every sale' + (short ? '.' : ' here. Take the world and it\x27s yours.'); }
  return short ? 'Take this world to earn ' + cut + ' a sale.' : 'Nobody holds it. Take this world and you earn ' + cut + ' from every sale here.';
}

// ---------------------------------------------------------------------------
// UI 3.0 STEP 2 (2.0-dev.36): RESOLUTION = THE GALAXY (mockup board 4, user: the
// galaxy is "my favorite screen"; the target list was "too much hand holding").
// Worlds wear a halo in your odds' color, rival fleets sit at their hangars, and
// a tap opens a SHEET under the map: the world (worth, earnings, trait, odds,
// SEND SOMEONE IN, FIGHT / TALK / PAY) or a rival fleet (exact odds, ATTACK).
// ---------------------------------------------------------------------------
let sheetPlanet = -1, sheetFleet = null;  // sheetFleet = {slot, f}
let rivalsOpen = false;  // dev.36d: the ATTACK A PLAYER chips
// dev.37b (user: "attacking a player should maybe take you to a different screen since there can be up to 9
// other fleets"): every other player's fleets, grouped by player, each with your odds -- tap one to attack.
let attackPlayersMode = false;
function openAttackPlayers() { attackPlayersMode = true; sheetPlanet = -1; sheetFleet = null; shownHtml = null; poll(); }
function closeAttackPlayers() { attackPlayersMode = false; shownHtml = null; poll(); }
function attackPlayersHtml(s, me, html) {
  const mine = me.fleets.map((fl, k) => ({ k: k, n: fl.alive, ok: fl.alive > 0 && !fl.spent })).filter(x => x.ok).sort((a, b) => b.n - a.n);
  const merc = (s.v2 && s.v2.perks && s.v2.perks.merc) || 0;
  html += '<button class="back" onclick="closeAttackPlayers()">&lsaquo; Home</button>';
  html += '<div class="aphead"><b>ATTACK A PLAYER</b><span>'+(mine.length ? 'Your best: Fleet '+(mine[0].k + 1)+' \u00B7 '+mine[0].n+' ships'+(merc ? ' + '+merc+' hired' : '') : 'No fleet of yours is ready')+'</span></div>';
  s.players.forEach((pl, slot) => {
    if (slot === mySlot || !pl.active || pl.chosenFaction < 0) return;
    const col = FACTION_UI_COLORS[pl.chosenFaction], url = spriteCanvas(FACTION_SHIP[pl.chosenFaction], '', true);
    const fleets = pl.fleets.map((fl, k) => ({ k: k, n: fl.alive })).filter(x => x.n > 0);
    html += '<div class="apgroup" style="border-color:'+col+'"><div class="apname" style="color:'+col+'">'+insigniaSvg(pl.chosenFaction, 18)+FACTION_SHORT_NAMES[pl.chosenFaction] +
            (s.v2 && s.v2.bounty === slot ? '<em>BOUNTY</em>' : '')+'</div>';
    if (!fleets.length) html += '<div class="note" style="margin:4px 0">No fleets in space.</div>';
    fleets.forEach(fl => {
      let odds = '<span class="dim">no fleet ready</span>';
      if (mine.length) { const pv = battleWinPct(Math.max(1, mine[0].n + merc + classAtk(s, -1)), fl.n, 0, 0, true), v = verdictOf(pv, pv); odds = '<b style="color:'+v.col+'">'+v.word+' <small>'+pv+'%</small></b>'; }
      const ships = Array.from({ length: Math.min(fl.n, 7) }, () => '<img class="pixel" src="'+(url ? url.toDataURL() : '')+'" alt="">').join('');
      html += '<button class="aprow"'+(mine.length ? ' onclick="attackPlayersMode = false; sheetAttack('+slot+','+fl.k+','+mine[0].k+')"' : ' disabled')+'>' +
              '<span class="apfl">FLEET '+(fl.k + 1)+'<small>'+fl.n+' ship'+(fl.n === 1 ? '' : 's')+' \u00B7 +2 VP</small></span><span class="apships">'+ships+'</span>'+odds+'</button>';
    });
    html += '</div>';
  });
  html += '<p class="note">Only a fleet next to theirs on the board can attack (on your honor).</p>';
  setApp(app, html);
}
function sheetPick(i) { sheetPlanet = sheetPlanet === i ? -1 : i; sheetFleet = null; shownHtml = null; poll(); }
function sheetPickFleet(slot, f) { sheetFleet = { slot: slot, f: f }; sheetPlanet = -1; shownHtml = null; poll(); }
function sheetClose() { sheetPlanet = -1; sheetFleet = null; shownHtml = null; poll(); }
function sheetAttack(slot, f, mine) {  // into the existing player-vs-player forecast
  pvpAtkFleet = mine; pvpAtkMask = 1 << mine; pvpTarget = slot; pvpDefFleet = f; pvpMsg = ''; battleMode = true;
  sheetFleet = null; shownHtml = null; poll();
}
// Your best fleet's odds against world i: the same sums as the attack line (dev.29).
function worldOdds(s, i) {
  const p = s.planets[i], me = s.players[mySlot];
  if (!p || !me || p.owner === mySlot || !p.gr) return null;
  const res = s.phase === 2;
  const fl = me.fleets.map((f, k) => ({ k: k, n: f.alive, ok: f.alive > 0 && (!res || !f.spent) })).filter(f => f.ok).sort((a, b) => b.n - a.n);
  const sym = p.chars[0].revealed ? AFFILIATION_TO_FACTION.indexOf(p.chars[0].value) : -1;
  const tierOf = f => (s.reputation[f] ? s.reputation[f].tier : '').toUpperCase();
  const esc = sym >= 0 && ['FAVORABLE', 'ALLIED'].includes(tierOf(sym ^ 1)) ? 1 : 0;
  const milSure = sym >= 0 && tierOf(sym) === 'HOSTILE';
  const merc = (s.v2 && s.v2.perks && s.v2.perks.merc) || 0;
  if (!fl.length) return { fl: fl, sym: sym, esc: esc, merc: merc, milSure: milSure, none: true };
  const odds = f => { const a = Math.max(1, f.n + esc + merc + classAtk(s, p.owner, p)); return [battleWinPct(a, p.gr[1], 0, 0), battleWinPct(a, p.gr[0], 0, 0)]; };
  const best = fl[0], [lo, hi] = odds(best);
  return { fl: fl, best: best, lo: lo, hi: hi, v: verdictOf(lo, hi), odds: odds, sym: sym, esc: esc, merc: merc, milSure: milSure };
}
// FIGHT / NEGOTIATE / BRIBE for world i (3.1-dev.88: TALK / PAY renamed -- user: "too ambiguous" at the table) -- the same rules the planet page used (v0.21.6 .. 2.0).
// 3.1-dev.20 (mockup World.dc.html): under FIGHT / TALK / PAY, what each would do to your favor.
// 3.1-dev.39 (playtest: "defense shouldn't say low if I have loaded up garrison"): the Defense tile is the
// world's BASE rating -- on your own world show the real garrison, elsewhere call it BASE DEFENSE.
function defKey(p) { return p.owner === mySlot || p.ds !== undefined ? 'DEFENDERS' : 'BASE DEFENSE'; }
// 3.1-dev.101 (user: the Defense intel "should" change when ships are added): what you SAW, and whether it changed since.
function defVal(p, v) {
  if (p.owner === mySlot) return (p.defense || 0) + ' ship' + ((p.defense || 0) === 1 ? '' : 's');
  if (p.ds === undefined) return v;
  return p.ds + ' ship' + (p.ds === 1 ? '' : 's') + (p.dd === 1 ? '<small style="color:#FFB347">MORE SINCE YOU LOOKED</small>' : p.dd === 2 ? '<small style="color:#FFB347">FEWER SINCE YOU LOOKED</small>' : '');
}
function favorPreview(s, i) {
  const p = s.planets[i], me = s.players[mySlot];
  if (!p || !me || p.owner === mySlot || p.locked) return '';
  if (fhFinal(s)) return '<div class="fvPrev"><b>THE FINAL HOUR · FAVOR IS FROZEN</b><small>Nothing you do this round changes your favor, and there’s no betrayal. Take what you can.</small></div>';  // 3.1-dev.92
  const sym = p.chars && p.chars[0] && p.chars[0].revealed ? AFFILIATION_TO_FACTION.indexOf(p.chars[0].value) : -1;
  const F = sym >= 0 ? FNAMES[sym] : 'its faction', R = sym >= 0 ? FNAMES[sym ^ 1] : 'its rival', t = sym >= 0 ? tierIdx(s, sym) : -1;
  const cls = me.commander >= 0 ? me.commander % 4 : -1;
  const rows = [];
  if (p.owner >= 0) rows.push(['FIGHT', 'Held by ' + feedWho(s, p.owner) + ': taking it from a player costs no favor.']);  // 3.1-dev.36
  else rows.push(['FIGHT', t >= 3 ? '<span class="dn">BETRAYAL</span>: the ' + F + ' drop you to \u201C' + favorWord(sym, 2) + '\u201D \u00b7 ' + R + ' <span class="up">\u2191</span>'
                             : F + ' <span class="dn">\u2193\u2193\u2193</span> \u00b7 ' + R + ' <span class="up">\u2191' + (cls === 0 ? '\u2191</span> (the muscle)' : '</span>') + (t === 0 ? ' \u00b7 they\u2019re already out for you' : '')]);
  if (p.owner < 0 && p.home < 0) {
    let q = [3, 4, 5][p.ring] || 3;
    if (cls === 1) q--; if (cls === 0) q++;  // 3.1-dev.48/49: the Warlord is BLUNT now (the Scoundrel's weakness is MARK-UP)
    if (eraOf(s) && eraOf(s).f === 0) q--;
    if (s.v2 && s.v2.summit && s.v2.summit.by === mySlot) q--;
    q = Math.max(2, q);
    rows.push(['NEGOTIATE', t === 0 ? '<span class="dn">They won\u2019t talk to you.</span>' : F + ' <span class="up">\u2191</span> \u00b7 ' + R + ' <span class="dn">\u2193</span> \u00b7 ' + q + ' questions' + eraTag(s, 0, '\u22121 HOPE')]);
    rows.push(['BRIBE', F + ' <span class="up">\u2191</span> \u00b7 ' + R + ' <span class="dn">\u2193</span> \u00b7 ' + p.pay + ' CR' + eraTag(s, 2, '\u00d71.5 SPICE')]);
  }
  return '<div class="fvPrev"><b>WHAT EACH CHOICE DOES TO YOUR FAVOR</b>' + rows.map(r => '<div><i>' + r[0] + '</i><span>' + r[1] + '</span></div>').join('') +
         (sym < 0 ? '<small>Reveal its Sympathy to know whose favor is at stake.</small>' : '') + '</div>';
}
function isConsul(s) { const me = s.players[mySlot]; return !!me && me.commander >= 0 && me.commander % 4 === 1; }
function takeoverActsHtml(s, i) {
  const p = s.planets[i], mine = p.owner >= 0 && p.owner === mySlot;
  let h = '';
  if (s.v2 && s.v2.summit && s.v2.summit.w.includes(i))  // 3.1-dev.114 (playtest: ceasefire worlds weren't shown until you attacked)
    h += '<div class="inkChip" style="--ic:#5FD4FF"><b>CEASEFIRE</b><span>' + feedWho(s, s.v2.summit.by) + '’s Summit protects ' + p.name + ' through round ' + s.v2.summit.u + '. No buyouts or uprisings. An attack breaks it: peacekeepers defend.</span></div>';
  if (p.dug && !mine) h += '<div class="inkChip dug"><b>DUG IN +2</b><span>Taken by force this round: its new holder has 2 more defenders until the round ends.</span></div>';  // 3.1-dev.102
  const res = (s.takeoverPlayer === mySlot && s.takeoverPlanet === i) ? s.takeoverResult : '';
  if (res) h += '<p class="alert">'+res+'</p>';
  if (mine) h += '<div class="note">You hold this world. Fortify it in Prepare.</div>';
  else if (p.locked) h += '<div class="note">You\x27ve made your move here this round.</div>';
  else if (p.ink && p.owner >= 0 && p.owner !== mySlot)  // 3.1-dev.102 THE INK IS STILL WET: one attack, nothing else
    h += (p.ink[1] ? '<div class="note">The ink is still wet on ' + p.name + ', but someone already tried this round.</div>'
                   : '<div class="inkChip"><b>INK STILL WET</b><span>' + feedWho(lastS, p.owner) + ' just ' + (p.ink[0] === 2 ? 'bribed' : 'talked') + ' their way in. A fleet of yours that\u2019s AT ' + p.name + ' on the board and hasn\u2019t attacked this round may fight for it, once, against the defenders it had before the deal. Win and it\u2019s yours.</span></div>' +
                     '<div class="sacts one"><button class="sfight" onclick="planetFight('+i+')">FIGHT FOR ' + p.name + '</button></div>');
  else if (p.al)  // 3.1-dev.101 negotiate, then fight
    h += '<div class="sacts one"><button class="sfight" onclick="planetFight('+i+')">FIGHT</button></div><div class="note">The talks failed. ' + p.name + ' is on alert: +1 defender if you fight now.</div>';
  else if (p.home >= 0 && isConsul(s))  // 3.1-dev.101: a Consul can talk a home world over (5 hard questions, no misses, once)
    h += '<div class="sacts"><button class="sfight" onclick="planetFight('+i+')">FIGHT</button><button class="stalk" onclick="planetNegotiate('+i+')">NEGOTIATE<small>5 HARD \u00b7 ONCE</small></button></div>';
  else if (p.home >= 0)  // a home world: only a fight takes it
    h += '<div class="sacts one"><button class="sfight" onclick="planetFight('+i+')">FIGHT</button></div>';
  else if (p.owner >= 0)  // 3.1-dev.79: a commander's world -- fight, stir an UPRISING (talk), or a BUYOUT (pay)
    h += '<div class="sacts"><button class="sfight" onclick="planetFight('+i+')">FIGHT</button>' +
         '<button class="stalk" onclick="planetNegotiate('+i+')">' + kw('UPRISING', 'UPRISING') + eraTag(s, 0, 'AGAIN') + '</button>' +
         (p.ty > 0 ? '<button class="spay" disabled>' + kw('BUYOUT', 'BUYOUT') + '<small>TREATY</small></button>' : p.bo ? '<button class="spay" disabled>' + kw('BUYOUT', 'BUYOUT') + '<small>OFFER IN</small></button>' : '<button class="spay" onclick="planetPay('+i+')">' + kw('BUYOUT', 'BUYOUT') + '<small>'+p.pay+' CR</small></button>') + '</div>';
  else h += '<div class="sacts"><button class="sfight" onclick="planetFight('+i+')">FIGHT</button>' +
            '<button class="stalk" onclick="planetNegotiate('+i+')">NEGOTIATE</button>' +
            '<button class="spay" onclick="planetPay('+i+')">BRIBE<small>'+p.pay+' CR' + eraTag(s, 2, '\u00d71.5') + '</small></button></div>';
  return h;
}

// ---------------------------------------------------------------------------
// UI 3.0 STEP 3 (2.0-dev.37): PREPARE = YOUR HANGAR (mockup board 1, user: "a big
// fan of your hangar"; "it feels like you are losing the feeling of going to the
// hangar to fix the ships"). Each fleet is a bay of real ship sprites: tap an empty
// slot to build (+2), a lost ship to repair (FIX 2), an empty bay to found / rebuild
// the fleet -- all gameStateInvestFleet, 2 CR. Under it: markets, your promises,
// and the round GALAXY button with a turning planet (the 1.75"'s Home).
// ---------------------------------------------------------------------------
let hgShipUrl = '', hgShipFac = -1, spinRAF = 0, spinPlanet = 0;
function hangarShipUrl(f) {
  if (hgShipFac !== f) { const c = spriteCanvas(FACTION_SHIP[f] || 'rebel', '', false); hgShipUrl = c ? c.toDataURL() : ''; hgShipFac = f; }
  return hgShipUrl;
}
// 3.1-dev.118 (playtest: "everyone's phones are really hot"): the planet on the STAR MAP key turns one step every PLANET_STEP_MS --
// it used to ask for every screen frame (60 a second, all game long) just to check the clock. Now a timer, and none while calm.
function galaxySpinLoop() {
  const c = document.getElementById('gxSpin');
  if (!c) { spinRAF = 0; return; }
  if (!document.body.classList.contains('calm') && !document.hidden) {
    const step = Math.floor(performance.now() / PLANET_STEP_MS);
    if (step !== c._st) {
      c._st = step;
      const pc = bPlanetPixels(spinPlanet, step), x = c.getContext('2d');
      x.imageSmoothingEnabled = false; x.clearRect(0, 0, c.width, c.height); x.drawImage(pc, 0, 0, c.width, c.height);
    }
  }
  spinRAF = setTimeout(galaxySpinLoop, PLANET_STEP_MS);
}
function shipyardTag(s) {  // 3.1-dev.19: the Shipyards trait, seen where it works
  const p = s.planets.find(p => p.owner === mySlot && p.traitFx && /ships costs 1/.test(p.traitFx));
  return p ? ' <small class="trTag">\u00b7 ' + (p.chars && p.chars[2] ? p.chars[2].value : 'Shipyards') + ' on ' + p.name + '</small>' : '';
}
function hangarBayHtml(s, me, k) {
  const sc = (s.v2 && s.v2.shipCost) || 2;  // 3.1-dev.12: the Spice Age makes ships dearer
  const fl = me.fleets[k], cap = me.cap || fl.ships.length, afford = me.credits >= sc, can = s.phase === 0;
  const lbl = '<span class="bayk">FLEET<b>'+(k + 1)+'</b></span>';
  if (s.phase !== 0) {  // take 4: Resolution (and step 4: Movement) show your fleets read-only
    if (fl.alive === 0) return '<div class="bay empty">'+lbl+'<span class="baywhat">'+(fl.total === 0 ? 'Empty bay' : 'Destroyed \u00B7 rebuild it in Prepare')+'</span></div>';
    const ships = Array.from({ length: fl.alive }, () => '<img class="pixel" src="'+hangarShipUrl(me.chosenFaction)+'" alt="ship">').join('');
    if (s.phase === 1) return '<div class="bay">'+lbl+'<div class="slots" style="grid-template-columns:repeat('+cap+',minmax(0,1fr))">'+ships+'</div></div>';
    return '<div class="bay'+(fl.spent ? ' spent' : ' ready')+'">'+lbl+'<div class="slots" style="grid-template-columns:repeat('+cap+',minmax(0,1fr))">'+ships+'</div>' +
           '<b class="baytag">'+(fl.spent ? 'SPENT' : 'READY')+'</b></div>';
  }
  if (fl.total === 0 || fl.alive === 0) {  // never founded / destroyed: one big bay button
    const what = fl.total === 0 ? 'Empty bay' : 'Destroyed \u00B7 rebuild it here';
    return '<button class="bay empty'+(fl.total > 0 ? ' lost' : '')+'"'+(can && afford ? ' onclick="investFleet('+k+')"' : ' disabled')+'>'+lbl+'<span class="baywhat">'+what+'</span><b>'+sc+' CR / ship'+shipyardTag(s)+'</b></button>';
  }
  let slots = '', plusShown = false;
  for (let sh = 0; sh < cap; sh++) {
    if (sh < fl.total && fl.ships[sh]) slots += '<img class="pixel" src="'+hangarShipUrl(me.chosenFaction)+'" alt="ship">';
    else if (sh < fl.total) slots += '<button class="fix"'+(can && afford ? ' onclick="investFleet('+k+')"' : ' disabled')+' aria-label="Repair a ship">FIX<br>'+sc+'</button>';
    else if (!plusShown) { plusShown = true; slots += '<button class="plus"'+(can && afford ? ' onclick="investFleet('+k+')"' : ' disabled')+' aria-label="Build a ship">+'+sc+'</button>'; }
    else slots += '<i></i>';
  }
  return '<div class="bay'+(fl.spent ? ' spent' : '')+'">'+lbl+'<div class="slots" style="grid-template-columns:repeat('+cap+',minmax(0,1fr))">'+slots+'</div></div>';
}
// 3.1-dev.19 SWIPE DOWN to close: any bottom sheet, and a world's page (when you're at its top).
(function () {
  let y0 = null, el = null, dy = 0;
  // 3.1-dev.115 (user: "swiping down refreshes the page on most browsers"): only a drag that STARTS on the top bar (the first
  // 56 px of a sheet or a world page) closes it, and that drag is kept from the browser (no pull-to-refresh).
  const onTop = (box, y) => y - box.getBoundingClientRect().top < 90;  // the top bar + header (3.1-dev.120: was 56 px)
  const target = (t, y) => { const sb = t.closest && t.closest('#lyrSheet .sheetBox'); if (sb) return sb.scrollTop <= 0 && onTop(sb, y) ? sb : null;
                             const ws = t.closest && t.closest('.wsheet'); return ws && window.scrollY <= 0 && onTop(ws, y) ? ws : null; };
  document.addEventListener('touchstart', e => { el = target(e.target, e.touches[0].clientY); y0 = el ? e.touches[0].clientY : null; dy = 0; }, { passive: true });
  document.addEventListener('touchmove', e => {
    if (y0 === null || !el) return;
    dy = e.touches[0].clientY - y0;
    if (e.cancelable) e.preventDefault();
    // 3.1-dev.120 (user: "the swipe down animation doesn't seem to exist"): the world page's slide-up animation (fill-mode both)
    // held its transform, so the drag never showed -- it gives way the moment you drag.
    if (dy > 0) { el.style.animation = 'none'; el.style.transition = 'none'; el.style.transform = 'translateY(' + dy + 'px)'; }
  }, { passive: false });
  document.addEventListener('touchend', () => {
    if (!el) return;
    const done = dy > 90, box = el;
    box.style.transition = ''; box.style.transform = '';
    if (done) { if (box.classList.contains('wsheet')) leavePlanet(); else sheetScrimTap(); }
    el = null; y0 = null; dy = 0;
  }, { passive: true });
})();
// 3.1-dev.56 THE PHASE TRACK (user: "it needs to be on the phone"; a new player thought he should move ships on the phone): the
// round's three steps with the current one lit, and one plain line saying what to do right now.
function phaseTrackHtml(s) {
  const fin = s.round >= s.roundLimit, ph = s.phase;
  const names = ['PREPARE', fin ? 'LAST ORDERS' : 'MOVEMENT', fin ? 'THE FINAL HOUR' : 'RESOLUTION'];
  const now = [fin ? 'Last chance to spend: repair and build ships, buy, call in your debts. Talk or buy worlds now; there are no fights this round.'
                   : 'Spend your Credits: build or repair ships, scout worlds, shop. Nothing moves yet.',
               fin ? 'Give your Last Orders here on the phone: where each fleet strikes. Nobody sees them until the reveal.'
                   : 'Move your ships on the REAL BOARD now, one step each. There\u2019s nothing to tap here until you\u2019re done.',
               fin ? 'The reveal: play your gambits, then begin the finale.' : 'Act: attack, talk or buy worlds. Each fleet can attack once.'][ph] || '';
  const off = !!s.planets && nowOff();  // 3.1-dev.59: only on the real Home screen (the How to Play deck always shows it)
  return '<div class="ptrack"><div class="pts">' + names.map((n, k) => '<span class="' + (k === ph ? 'on' : k < ph ? 'done' : '') + '">' + (k < ph ? '\u2713 ' : '') + n + '</span>').join('<i>\u00b7</i>') +
         (off ? '<button class="ptback" onclick="setNowOff(false)" aria-label="Show the NOW line">NOW</button>' : '') + '</div>' +
         (off ? '' : '<div class="ptline"><b>NOW</b><span>' + now + '</span>' + (s.planets ? '<button class="ptx" onclick="setNowOff(true)" aria-label="Hide the NOW line">\u00D7</button>' : '') + '</div>') + '</div>';
}
// 3.1-dev.59 (user: "make it so the NOW thing can go away if I want it to"): an x hides the NOW line on this phone; the NOW chip on
// the step bar brings it back. Remembered on this phone only.
let NOW_OFF = null;
function nowOff() { if (NOW_OFF === null) { try { NOW_OFF = localStorage.getItem('sfNowOff') === '1'; } catch (e) { NOW_OFF = false; } } return NOW_OFF; }
function setNowOff(v) { NOW_OFF = v; try { localStorage.setItem('sfNowOff', v ? '1' : '0'); } catch (e) {} shownHtml = null; if (lastS) render(lastS); if (sheetKey === 'settings') settingsShow(); }
// ---- 3.1-dev.67 KEYWORDS (user: captions under every word "makes it look very AI ... needs to be more clever"; mockup idea 2) ----
// Game terms get a faint gold dotted underline; a tap opens Rivet's one line in a pop-up under the word (its own layer, so polls
// never touch it). Rivet off in Settings = plain words, no underline.
const KW = {
  'ESCORT': 'A ship from a faction that likes you. It takes the first hit for you, and it covers your retreat.',
  'MILITIA': 'Locals who hate you grab a blaster and join the defenders. Get on better terms and they stay home.',
  'HIRED': 'Mercenaries you paid for. They ride with your next battle and take hits before your own ships.',
  'DEFENDERS': 'The ships guarding a world. Sink them all and the world is yours.',
  'BASE DEFENSE': 'How strongly the world is guarded before anyone touches it. Scout it before you pick a fight.',
  'SYMPATHETIC TO': 'The faction this world leans to. Your standing with them changes its price, its talks and who helps in a fight.',
  'RESOURCES': 'How rich it is. Hold it and it pays you every Prepare: High pays 3 Credits, Low pays 1.',
  'TRAIT': 'The world\u2019s special rule. Hold the world and the rule works for you.',
  'OUT FOR YOU': 'They\u2019ve had enough of you. Expect a strike soon, unless you calm them down.',
  'YOU OWE': 'A promise from a negotiation. Keep it and they warm to you. Break it, or run out of time, and they take the world back.',
  'GAMBIT': 'A secret card for the Final Hour. Some ride with a strike, some you play when every strike is revealed.',
  'BLACK MARKET': 'Opens near the end, where the markets used to be: late-game lots, one big-ticket item a game, and other classes\u2019 gambits.',
  'UPRISING': 'Talk the people of a commander\u2019s world into rising up. Anyone can try, once per world; every question is hard and one wrong word ends it. Win and the world is yours at once, with 2 defenders, and its old holder can strike back this round. One try per world.',
  'BUYOUT': 'Buy a world another commander holds, for twice the price (20 CR at least). They can sell (and keep half) or match your offer to keep it. No answer by next round means it\u2019s sold.',
  'DEBTS': 'In the last Prepare, the factions that like you send ships for your final strikes. The ones that don\u2019t want Credits.',
  'DECREE': 'Your champion\u2019s power. Declare it and that faction\u2019s Era runs for 2 rounds: a rule for everyone, a sting for everyone, and a perk for you.',
  'YOUR SPY': 'A gift from the Crimson Dawn. Each round you see one player\u2019s standing with every faction.',
  'HEAT': 'How suspicious people are of your double-dealing. It cools in a round you don\u2019t sell. Too hot and you\u2019re caught.',
  'IF YOU FIGHT IT OUT': 'Each exchange both sides roll a die; the lower roll loses a ship, a tie rolls again. More ships don\u2019t roll better, they last longer. You can withdraw between exchanges, but they get a free parting shot.',
  'LAST ORDERS': 'The last Movement. Every fleet strikes one world anywhere, or stays home to guard one of yours. Nobody sees your orders until the reveal.',
  'JUMP PASS': 'A world next to one you hold is in reach for free. Anything farther needs a jump pass: 3 Credits a strike.'
};
const KW_ORDER = Object.keys(KW).sort((a, b) => b.length - a.length);
function kw(term, label) {
  const l = label || term;
  if (!KW[term] || !setOn('rivet')) return l;
  return '<span class="kw" onclick="kwTap(event,\'' + term + '\')">' + l + '<i class="kwh" onclick="kwTap(event,\'' + term + '\')"></i></span>';
}
function kwify(h) {  // underline the first known term in a plain-text heading
  if (!setOn('rivet')) return h;
  for (const k of KW_ORDER) { const i = h.indexOf(k); if (i >= 0) return h.slice(0, i) + kw(k) + h.slice(i + k.length); }
  return h;
}
let kwOpen = null;
function kwClose() { const p = document.getElementById('kwPop'); if (p) p.hidden = true; if (kwOpen) kwOpen.classList.remove('on'); kwOpen = null; }
function kwTap(ev, term) {
  ev.stopPropagation(); ev.preventDefault();
  const el = ev.currentTarget.closest('.kw') || ev.currentTarget;
  if (kwOpen === el) return kwClose();
  kwClose();
  let p = document.getElementById('kwPop');
  if (!p) { p = document.createElement('div'); p.id = 'kwPop'; p.onclick = e => { e.stopPropagation(); kwClose(); }; document.body.appendChild(p); }
  let h = 0; for (const ch of term) h = (h * 31 + ch.charCodeAt(0)) >>> 0;
  p.innerHTML = rivetSvg(28) + '<div><b>' + term + '</b><i>' + RV_BEEPS[h % RV_BEEPS.length] + '</i> ' + KW[term] + '</div>';
  p.hidden = false;
  const r = el.getBoundingClientRect(), W = p.offsetWidth, H = p.offsetHeight, vw = window.innerWidth, vh = window.innerHeight;
  const left = Math.max(12, Math.min(vw - W - 12, r.left + r.width / 2 - W / 2)), below = r.bottom + 12 + H < vh - 8;
  p.className = below ? 'below' : 'above';
  p.style.left = left + 'px'; p.style.top = (below ? r.bottom + 10 : r.top - 10 - H) + 'px';
  p.style.setProperty('--ax', Math.max(14, Math.min(W - 14, r.left + r.width / 2 - left)) + 'px');
  kwOpen = el; el.classList.add('on');
}
document.addEventListener('click', () => { if (kwOpen) kwClose(); });
document.addEventListener('scroll', () => { if (kwOpen) kwClose(); }, { passive: true, capture: true });  // the page or any sheet
// ---- 3.1-dev.61 SETTINGS (user 2026-11-14: "rivet should be a toggle (experienced players can turn him off)") ----
// THIS PHONE = switches kept on the phone (localStorage); THE TABLE = its Wi-Fi, shared by everyone (/netinfo).
const SET = {};
function setOn(k) { if (!(k in SET)) { try { SET[k] = localStorage.getItem('sfSet_' + k) !== '0'; } catch (e) { SET[k] = true; } } return SET[k]; }
function setFlip(k) {
  SET[k] = !setOn(k); try { localStorage.setItem('sfSet_' + k, SET[k] ? '1' : '0'); } catch (e) {}
  applyMotion(); shownHtml = null; if (lastS) render(lastS); settingsShow();
}
function buzz(p) { if (!setOn('buzz')) return; try { if (navigator.vibrate) navigator.vibrate(p); } catch (e) {} }
function reducedMotion() { return !setOn('motion') || !!(window.matchMedia && matchMedia('(prefers-reduced-motion: reduce)').matches); }
function applyMotion() { document.documentElement.classList.toggle('rm', !setOn('motion')); }
applyMotion();
function gearSvg() {
  return '<svg viewBox="0 0 20 20" width="20" height="20" aria-hidden="true"><circle cx="10" cy="10" r="6.6" fill="none" stroke="currentColor" stroke-width="3.2" stroke-dasharray="2.6 2.58"/>' +
         '<circle cx="10" cy="10" r="5.2" fill="none" stroke="currentColor" stroke-width="1.8"/><circle cx="10" cy="10" r="1.8" fill="currentColor"/></svg>';
}
function gearBtn() { return '<button class="gear" onclick="openSettings()" aria-label="Settings">' + gearSvg() + '</button>'; }
let NETI = null, netArm = '';
async function openSettings() {
  netArm = ''; settingsShow();
  const r = await api('/netinfo');
  if (r && r.saved) { NETI = r; if (sheetKey === 'settings') settingsShow(); }
}
function setTog(label, sub, on, js) {
  return '<button class="setRow" onclick="' + js + '"><span><b>' + label + '</b><small>' + sub + '</small></span><i class="tg' + (on ? ' on' : '') + '"></i></button>';
}
// 3.1-dev.81 FIRST GAME (user: "the learning curve is intense ... a new player ... has a hard time figuring out where to click"; "I like the rivet
// introduction, having a toggle for first game would be good when you are choosing your character or seat"; "rivet should use proper
// nouns and not very many pronouns"): Rivet introduces ONE thing per phase, in order, the first time it matters. Per phone.
// 3.1-dev.84 FIRST-GAME CHECKLIST (user: "the first 5 rounds or so should have a checklist with fix and repair fleets, explore the galaxy,
// and invest in defense"): Prepare of rounds 1-5, ticks itself by comparing with how things stood when this round's Prepare began.
function chkSnap(s) {
  const me = s.players[mySlot];
  return { ships: myShips(s), rev: s.planets.reduce((a, p) => a + p.chars.filter(c => c.revealed).length, 0),
           def: s.planets.reduce((a, p) => a + (p.owner === mySlot ? (p.defense || 0) + (p.pt > 0 ? 2 : 0) : 0), 0), held: s.planets.filter(p => p.owner === mySlot).length };
}
function firstChecklistHtml(s) {
  if (!firstGame() || s.stage !== 1 || s.phase !== 0 || s.round > 5 || mySlot < 0) return '';
  const now = chkSnap(s), key = 'sfChk_' + s.round;
  let start = null; try { start = JSON.parse(localStorage.getItem(key) || 'null'); } catch (e) {}
  if (!start) { start = now; try { localStorage.setItem(key, JSON.stringify(now)); } catch (e) {} }
  const me = s.players[mySlot], lost = me.fleets ? me.fleets.some(f => (f.alive || 0) < (f.built || f.total || 0)) : false;
  const items = [['FIX AND BUILD YOUR FLEETS', now.ships > start.ships || (!lost && s.round > 1 && now.ships >= 6), 'tap a bay in YOUR HANGAR: 2 CR a ship', "document.querySelector('.hangar,.hgbay,.bay') && document.querySelector('.hangar,.hgbay,.bay').scrollIntoView({behavior:'smooth',block:'center'})"],
                 ['LOOK AT THE STAR MAP', mapSeen(s.round), 'open the STAR MAP and tap a world near yours', 'openGalaxy()'],
                 ['INVEST IN DEFENSE', now.held ? now.def > start.def : false, now.held ? 'tap PLANETS: add defenders to your weakest world' : 'take a world first', now.held ? 'openDefense()' : '']];
  const done = items.filter(x => x[1]).length;
  let min = false; try { min = localStorage.getItem('sfChkMin') === String(s.round); } catch (e) {}
  if (min) return '<button class="chkPill" onclick="chkMin(0)">' + rivetSvg(18) + '<b>CHECKLIST</b><span>' + done + ' / 3</span><em></em></button>';  // 3.1-dev.85
  return '<div class="chk"><div class="chkH">' + rivetSvg(22) + '<b>ROUND ' + s.round + ' CHECKLIST</b><small>' + done + ' / 3</small><button class="ptx" onclick="chkMin(1)" aria-label="Shrink the checklist">\u00D7</button></div>' +
    items.map(x => '<button class="chkR' + (x[1] ? ' done' : '') + '"' + (x[3] ? ' onclick="' + x[3] + '"' : ' disabled') + '><i>' + (x[1] ? '\u2713' : '') + '</i><span><b>' + x[0] + '</b><small>' + x[2] + '</small></span></button>').join('') + '</div>';
}
function mapSeen(r) { try { return localStorage.getItem('sfMap') === String(r); } catch (e) { return false; } }  // 3.1-dev.122
function chkMin(on) { try { if (on) localStorage.setItem('sfChkMin', String(lastS.round)); else localStorage.removeItem('sfChkMin'); } catch (e) {} shownHtml = null; render(lastS); }
function firstGame() { try { return localStorage.getItem('sfSet_first') === '1'; } catch (e) { return false; } }
function toggleFirst() { try { const on = !firstGame(); localStorage.setItem('sfSet_first', on ? '1' : '0');  // 3.1-dev.85: on = the tour starts over
    if (on) { localStorage.removeItem('sfIntro'); localStorage.removeItem('sfChkMin'); for (let r = 1; r <= 5; r++) localStorage.removeItem('sfChk_' + r); } } catch (e) {} lastSeatSnapshot = ''; shownHtml = null; if (lastS) render(lastS); if (sheetKey === 'settings') settingsShow(); }
function firstTogHtml() {
  const on = firstGame();
  return '<button class="firstTog' + (on ? ' on' : '') + '" onclick="toggleFirst()">' + rivetSvg(30) + '<span><b>FIRST GAME?</b><small>' + (on ? 'Rivet will show you around, one thing at a time.' : 'Tap if this is your first game: Rivet will show you around.') + '</small></span><i>' + (on ? 'ON' : 'OFF') + '</i></button>';
}
const INTRO = [  // [key, when, text, [button, js] | null] -- names, not pronouns
  ['home', s => s.round === 1 && s.phase === 0, 'Beep! This is the BRIDGE, your home screen. Round 1 starts with Prepare: Prepare is for spending Credits. Tap an empty slot in YOUR HANGAR to build a ship for 2 CR.', null],
  // 3.1-dev.84 (user: "after you buy your ship it should prompt you to look at the galaxy page and pick a planet near you and explain those
  // buttons and why they benefit you") -- these two follow straight on, not one-a-phase
  ['gal', s => s.round === 1 && s.phase === 0 && myShips(s) > 3, 'Nice ship. Next, open the STAR MAP and tap a world next to your home world. Every world shows how well defended it is, and the worlds next to yours tell you whose side they are on.', ['OPEN THE STAR MAP', 'openGalaxy()'], 1],
  ['world', s => currentPlanet >= 0 && s.planets[currentPlanet] && s.planets[currentPlanet].owner !== mySlot, s => worldIntro(s, currentPlanet), null, 1],
  ['move', s => s.phase === 1, 'Boop. Movement happens on the board, not on the phone. Move your fleets along the lanes, then hold LOCK IN on the BRIDGE.', null],
  ['res', s => s.phase === 2, 'Resolution is for taking worlds. Open the STAR MAP, tap a world one of your fleets sits next to, then pick FIGHT, NEGOTIATE or BRIBE.', ['OPEN THE STAR MAP', 'openGalaxy()']],
  ['def', s => s.round >= 2 && s.phase === 0 && s.planets.some(p => p.owner === mySlot), 'The PLANETS box opens your defenses. A world with 1 defender is easy to take: add defenders there in Prepare.', ['OPEN DEFENSES', 'openDefense()']],
  ['favor', s => s.round >= 2 && s.phase === 1, 'Every faction likes you a little more or a little less. The WAR TABLE shows where you stand with the Rebels, the Empire, the Pykes and the Crimson Dawn.', ['OPEN FAVOR', 'openReputation()']],
  ['gy', s => s.round >= 2 && s.phase === 2, 'COMMS lists what needs you right now: jobs, debts, threats. A red light on COMMS means something is waiting. Check COMMS every round.', null],
  ['move3', s => s.round >= 3 && s.phase === 0 && s.v2 && !s.v2.moveUsed && myClass(s) >= 0, s => 'Your once-a-game move is ready: ' + CLASSES[myClass(s)].move + '. Tap your face, then YOUR KIT, to read what ' + CLASSES[myClass(s)].move + ' does.', null],
  ['era', s => !!eraOf(s), s => 'An Era is running: ' + ERAS[eraOf(s).f].name + '. ' + ERAS[eraOf(s).f].name + ' changes one rule for everyone at the table. Tap the Era banner to read the rule.', null],
  ['bm', s => bmOpenNow(s), 'The markets went dark: the BLACK MARKET is open. The BLACK MARKET sells late-game lots. Open DOCKS to shop there.', ['OPEN THE BLACK MARKET', 'openBlackMarket()']],
];
function myShips(s) { const me = s.players[mySlot]; return me && me.fleets ? me.fleets.reduce((a, f) => a + (f.alive || 0), 0) : 0; }
function worldIntro(s, i) {
  const N = s.planets[i].name, learn = 'its DEFENSE says how strong ' + N + ' is; its SYMPATHY (whose side it is on) shows once you hold a world next to it';
  return 'This is ' + N + '. Read it first: ' + learn + '. Then, in Resolution, take ' + N + ': FIGHT with your ships, NEGOTIATE to win it without a shot (its faction likes you more), or BRIBE it with Credits. Every world you hold is worth Victory Points.';
}
function introSeen() { try { return JSON.parse(localStorage.getItem('sfIntro') || '{}'); } catch (e) { return {}; } }
function rivetIntro(s) {
  if (!firstGame() || !s || s.stage !== 1 || !joined || mySlot < 0) return;
  if (sheetKey || (typeof ANN !== 'undefined' && ANN.open) || battleOnScreen || feedMode || cardMode || bannerOpen() || document.getElementById('txOverlay').classList.contains('active')) return;
  const seen = introSeen(), at = s.round * 3 + s.phase;
  for (const [k, when, text, act, chain] of INTRO) {
    if (seen[k] || (seen._at === at && !chain) || !when(s)) continue;  // one a phase, except the round-1 chain
    seen[k] = 1; seen._at = at;
    try { localStorage.setItem('sfIntro', JSON.stringify(seen)); } catch (e) {}
    const tx = typeof text === 'function' ? text(s) : text;
    sheetShow('intro', '<div class="introRv">' + rivetSvg(44) + '<div><b>RIVET</b><span>' + tx + '</span></div></div>' +
      '<div class="biAct">' + (act ? '<button style="border-color:#5FD4FF;color:#5FD4FF" onclick="sheetHide();' + act[1] + '">' + act[0] + '</button>' : '') + '<button class="ghost" onclick="sheetHide()">GOT IT</button></div>', '#5FD4FF');
    return;
  }
}
// ---- 3.1-dev.124 HOW TO PLAY = A GUIDED WALKTHROUGH (canvas board 31; user: "completely redo how to play", "guided walkthrough, a
// little flavor"). Rivet dims the screen and spotlights the REAL button; each step = one flavor line + one plain instruction.
// Steps with tap:1 wait for you to tap the real thing (NEXT always works too). Starts by itself in a FIRST GAME (round 1
// Prepare); HOW TO PLAY in the gear menu replays it.
const TOUR = [
  { kick: 'INCOMING BRIEFING', fl: 'Commander. The galaxy has no master tonight, and every faction wants one. Make it you.',
    say: 'Beep! I\u2019m Rivet. You win by holding worlds: each one is worth Victory Points. I\u2019ll show you around in about a minute.', btn: 'LET\u2019S GO' },
  { deck: 'bridge', sel: '.slots .plus, .bay.empty', tap: 1, kick: 'YOUR HANGAR', fl: 'Ships win wars. Credits buy ships.',
    say: 'Tap the + in a bay to build a ship for 2 CR. A fleet holds up to 6 ships.' },
  { deck: 'bridge', sel: '.stats', kick: 'YOUR NUMBERS', fl: 'Know your strength before you spend it.',
    say: 'Credits to spend, your Victory Points (tap them for the standings), and the worlds you hold (tap to defend them).' },
  { deck: 'bridge', sel: '.dk-map', tap: 1, kick: 'THE STAR MAP', fl: 'Every world out there is somebody\u2019s prize.',
    say: 'The STAR MAP is the board on the table. Tap it.' },
  { deck: 'map', sel: '#gwin', tapSel: '#gwin button', tap: 1, kick: 'PICK A WORLD', fl: 'Start close to home. Ambition can wait a round.',
    say: 'Tap a world next to your home world.' },
  { sel: '.wfGrid', kick: 'READING A WORLD', fl: 'Scouts are cheaper than funerals.',
    say: 'DEFENSE says how strong it is. SYMPATHY says whose side it\u2019s on: you learn it for worlds next to ones you hold. Swipe sideways for the next world.' },
  { deck: 'bridge', sel: '.dk-docks', tap: 1, kick: 'THE DOCKS', fl: 'Every faction is selling. Some will not sell to you.',
    say: 'DOCKS holds the faction shops: one buy at each shop a round. Tap it.' },
  { deck: 'docks', kick: 'THE SHOPS', fl: 'Spice, blades, secrets. All for a price.',
    say: 'Fuel, repairs, hired ships, a word with a faction. A shop closes to you if its faction dislikes you.' },
  { deck: 'bridge', sel: '.dk-comms', tap: 1, kick: 'COMMS', fl: 'Somebody is always talking about you.',
    say: 'COMMS lists what needs you: jobs, debts, threats. A red light means act now. Tap it.' },
  { deck: 'bridge', sel: '.dk-war', tap: 1, kick: 'THE WAR TABLE', fl: 'Know who is winning, and who hates you.',
    say: 'The WAR TABLE has the standings and your favor with each faction. Tap it.' },
  { deck: 'bridge', sel: '.dkLock', kick: 'LOCK IN', fl: 'Orders given. No turning back.',
    say: 'Done for this step? HOLD Lock In until the ring fills. The next step starts when everyone has.' },
  { kick: 'EVERY ROUND', fl: 'Prepare. Move. Strike.',
    say: 'PREPARE: spend Credits. MOVEMENT: move your fleets on the table, one step each. RESOLUTION: take worlds with FIGHT, NEGOTIATE or BRIBE.' },
  { kick: 'THE LAST ROUND', fl: 'Every war ends. Make sure yours ends well.',
    say: 'In the last round you give secret LAST ORDERS, then every strike lands at once in THE FINAL HOUR. Most VP wins. Good hunting, Commander.', btn: 'FINISH' },
];
let tourI = -1;
function tourOn() { return tourI >= 0; }
function tourStart() {
  if (!lastS || lastS.stage !== 1 || mySlot < 0) { openPrimer(); return; }  // not in a game: the old cards
  try { const seen = introSeen(); ['home', 'gal', 'world'].forEach(k => seen[k] = 1); localStorage.setItem('sfIntro', JSON.stringify(seen)); } catch (e) {}
  if (sheetKey) sheetHide();
  tourI = 0; tourShow();
}
function tourEnd() { tourI = -1; try { localStorage.setItem('sfTourDone', '1'); } catch (e) {} const el = document.getElementById('tour'); if (el) el.remove(); }
function tourNext() { if (!tourOn()) return; tourI++; if (tourI >= TOUR.length) { tourEnd(); return; } tourShow(); }
function tourShow() {
  const st = TOUR[tourI]; if (!st) return;
  if (st.deck && typeof deckOf === 'function' && deckOf() !== st.deck) deckGo(st.deck);
  let el = document.getElementById('tour');
  if (!el) { el = document.createElement('div'); el.id = 'tour'; document.body.appendChild(el); }
  el.innerHTML = '<div id="tourSpot"></div><div id="tourBub"><div class="tbTop"><span class="tbRv">' + rivetSvg(34) + '</span><div><b>' + st.kick + '</b><i>\u201C' + st.fl + '\u201D</i><span>' + st.say + '</span></div></div>' +
    '<div class="tbBot"><small>STEP ' + (tourI + 1) + ' OF ' + TOUR.length + (st.tap ? ' \u00b7 TRY IT' : '') + '</small><button class="ghost" onclick="tourEnd()">SKIP TOUR</button>' +
    '<button onclick="tourNext()">' + (st.btn || 'NEXT') + '</button></div></div>';
  setTimeout(tourPlace, 60); setTimeout(tourPlace, 450);
}
function tourPlace() {
  if (!tourOn()) return;
  const st = TOUR[tourI], spot = document.getElementById('tourSpot'), bub = document.getElementById('tourBub'); if (!spot || !bub) return;
  const tgt = st.sel ? Array.from(document.querySelectorAll(st.sel)).find(x => x.offsetParent !== null) : null;
  if (!tgt) { spot.className = 'full'; spot.removeAttribute('style'); bub.style.top = '30%'; bub.style.bottom = ''; return; }
  const r = tgt.getBoundingClientRect(), pad = 6;
  spot.className = ''; spot.style.left = (r.left - pad) + 'px'; spot.style.top = (r.top - pad) + 'px'; spot.style.width = (r.width + 2 * pad) + 'px'; spot.style.height = (r.height + 2 * pad) + 'px';
  spot.style.borderRadius = getComputedStyle(tgt).borderRadius || '14px';
  if (r.top + r.height / 2 > window.innerHeight / 2) { bub.style.top = ''; bub.style.bottom = Math.max(12, window.innerHeight - r.top + 16) + 'px'; }
  else { bub.style.bottom = ''; bub.style.top = Math.min(window.innerHeight - 200, r.bottom + 16) + 'px'; }
}
document.addEventListener('click', e => {  // a "try it" step moves on when you tap the real thing
  if (!tourOn()) return; const st = TOUR[tourI];
  if (st.tap && e.target.closest && e.target.closest(st.tapSel || st.sel) && !e.target.closest('#tourBub')) setTimeout(tourNext, 450);
}, true);
window.addEventListener('resize', () => tourPlace());
function tourAuto(s) {  // a FIRST GAME starts it by itself, once, in round 1's Prepare
  if (tourOn() || !firstGame() || !s || /harness/.test(s.firmwareVersion || '') || s.stage !== 1 || s.round !== 1 || s.phase !== 0 || mySlot < 0 || sheetKey || battleOnScreen) return;
  try { if (localStorage.getItem('sfTourDone') === '1') return; } catch (e) {}
  tourStart();
}
// ---- 3.1-dev.124 FIRST TIME (user: "first time you fight it tells you what is going to happen, the first negotiate tells you the goal
// of negotiating and explains a promise the first time you see one"): once per phone (Rivet's switch turns them off too).
const FIRSTS = {
  fight: ['YOUR FIRST BATTLE', 'Steady, Commander.', 'Each exchange, you roll and they roll. The higher die sinks one ship on the other side; a tie goes to the bigger side. Tap FIGHT for the next exchange. WITHDRAW pulls you out, but they get one last shot at you.'],
  neg: ['YOUR FIRST NEGOTIATION', 'Words can take a world without a single shot.', 'The world\u2019s faction asks you a few questions. Answer the way THEY would want. No wrong answers: the world is yours. One miss: they ask a favor in return. Two offending answers and they cut you off.'],
  promise: ['A PROMISE', 'A deal is only as good as the one who keeps it.', 'Pick a favor and the world is yours NOW, with its VP. Keep the promise by the deadline and their faction likes you more. Break it and they take the world back.'],
};
function firstSeen(k) { try { return !!JSON.parse(localStorage.getItem('sfFirst') || '{}')[k]; } catch (e) { return true; } }
function firstMark(k) { try { const o = JSON.parse(localStorage.getItem('sfFirst') || '{}'); o[k] = 1; localStorage.setItem('sfFirst', JSON.stringify(o)); } catch (e) {} }
function firstTimePrompt(s) {
  if (!s || mySlot < 0 || sheetKey || tourOn() || /harness/.test(s.firmwareVersion || '') || (typeof setOn === 'function' && !setOn('rivet'))) return;
  let k = null;
  if (s.battleActive && !s.battleOver && (s.battleExchange || 0) === 0 && (s.battleAttackerSlot === mySlot || s.battleDefenderSlot === mySlot)) k = 'fight';
  else if (s.takeoverPlayer === mySlot && s.takeoverMethod === 2 && s.neg && s.neg.favors && s.neg.favors.length) k = 'promise';
  else if (s.takeoverPlayer === mySlot && s.takeoverMethod === 2 && s.neg && s.neg.result === 0) k = 'neg';
  if (!k || firstSeen(k)) return;
  firstMark(k); const F = FIRSTS[k];
  sheetShow('first', '<div class="introRv">' + rivetSvg(44) + '<div><b>' + F[0] + '</b><i style="display:block;color:var(--dim);font-size:13px;margin:4px 0">\u201C' + F[1] + '\u201D</i><span>' + F[2] + '</span></div></div>' +
    '<div class="biAct"><button class="ghost" onclick="sheetHide()">GOT IT</button></div>', '#5FD4FF');
}
// 3.1-dev.125 THE FACTIONS, FOR SOMEONE WHO DOESN'T KNOW STAR WARS (a new player: "a page somewhere that gives the basic
// characteristics of the factions ... maybe a drop down on the negotiate page"). Plain words: who they are, what they value, what
// they can't stand, and how to talk to them.
const FACTION_GUIDE = [
  { who: 'Rebels who rose up against the Empire. Ordinary people with old ships and big hearts.',
    like: 'courage, freedom, mercy, standing by your friends', hate: 'cruelty, cowardice, bullying the weak',
    talk: 'Answer like a hero would: brave, honest, kind to the little guy.' },
  { who: 'The Empire rules the galaxy with a huge, disciplined army and a lot of rules.',
    like: 'order, discipline, strength, loyalty to the chain of command', hate: 'chaos, rebels, disobedience, weakness',
    talk: 'Answer like a strict officer would: firm, orderly, respectful of power.' },
  { who: 'The Pykes are a crime syndicate that runs the spice trade, a valuable and dangerous cargo.',
    like: 'profit, deals, keeping your word, paying what you owe', hate: 'being cheated, waste, anyone who skims off the top',
    talk: 'Answer like a careful businessperson would: practical, fair, always about the bottom line.' },
  { who: 'Crimson Dawn is a secretive crime ring that works from the shadows through spies and favors.',
    like: 'loyalty, secrets, ambition, elegance', hate: 'traitors, loose talk, sloppiness',
    talk: 'Answer like a quiet schemer would: discreet, loyal, a little ruthless.' },
];
function factionGuideHtml(f) {
  const g = FACTION_GUIDE[f];
  return '<div class="fgCard" style="--fc:' + FACTION_UI_COLORS[f] + '"><div class="fgHead">' + insigniaSvg(f, 26) + '<b>' + TX_FACTIONS[f].name + '</b></div>' +
    '<p>' + g.who + '</p><div class="fgRow"><span>THEY LIKE</span><b>' + g.like + '</b></div><div class="fgRow"><span>THEY HATE</span><b>' + g.hate + '</b></div>' +
    '<div class="fgTalk">' + g.talk + '</div></div>';
}
function openFactionGuide(f) {
  const list = f >= 0 ? [f] : [0, 1, 2, 3];
  sheetShow('fguide', '<div class="sheetSub" style="letter-spacing:3px">' + (f >= 0 ? 'WHO ARE THEY?' : 'THE FACTIONS') + '</div>' + list.map(factionGuideHtml).join('') +
    '<div class="biAct"><button class="ghost" onclick="sheetHide()">GOT IT</button></div>', f >= 0 ? FACTION_UI_COLORS[f] : '#5FD4FF');
}
function settingsShow() {
  if (sheetKey !== null && sheetKey !== 'settings') return;
  let h = '<div class="sheetTitle" style="color:#5FD4FF">SETTINGS</div>' +
    '<div class="gdlist" style="margin:6px 0 10px"><button class="gyrow" onclick="sheetHide();openGuides(\'\')">' + rivetSvg(18) + '<span><b>RIVET\u2019S GUIDES</b> how each part works</span><em></em></button>' +
    '<button class="gyrow" onclick="sheetHide();tourStart()">' + pixIcon('victory', '#E8EEF5', 2) + '<span><b>HOW TO PLAY</b> Rivet walks you through the real screens</span><em></em></button>' +
    '<button class="gyrow" onclick="sheetHide();setTimeout(() => openFactionGuide(-1), 280)">' + pixIcon('t4', '#E8EEF5', 2) + '<span><b>THE FACTIONS</b> who they are and how to talk to them</span><em></em></button>' +  // 3.1-dev.125
    '<a class="gyrow" href="/report" download="shadowfleet_report.txt" style="text-decoration:none">' + pixIcon('holonet', '#8FA3B8', 2) + '<span><b>SAVE A REPORT</b> a text file of this game and the table\u2019s health, to send with a bug</span><em></em></a></div>' +  // 3.1-dev.122  // 3.1-dev.103 one menu
    '<div class="setH">THIS PHONE</div>' +
    setTog('RIVET', 'Your droid explains what just happened when you tap him. Switch him off once you know the game.', setOn('rivet'), "setFlip('rivet')") +
    setTog('FIRST GAME', 'Rivet shows you around, one thing a phase, the first time it matters.', firstGame(), 'toggleFirst()') +
    setTog('THE NOW LINE', 'The line under the step bar that says what to do right now.', !nowOff(), 'setNowOff(!nowOff())') +
    setTog('BUZZ', 'Buzz when everyone is waiting on you or your world is attacked.', setOn('buzz'), "setFlip('buzz')") +
    setTog('MOTION', 'Slides, wipes and fades. Off = things just appear.', setOn('motion'), "setFlip('motion')") +
    '<div class="setH">THE TABLE \u00B7 SHARED BY EVERYONE</div>';
  const n = NETI;
  if (!n) h += '<p class="setNet"><small>Asking the table\u2026</small></p>';
  else {
    h += '<p class="setNet">' + (n.home ? 'On <b>' + esc(n.ssid) + '</b><small>Phones on that Wi-Fi scan the code on the table.</small>'
                                        : 'On its own network, <b>ShadowFleet</b>' + (n.failed ? '<small>It couldn\u2019t reach ' + esc(n.ssid) + ' when it started' + (n.why ? ': ' + esc(n.why) : '.') + '</small>' : '<small>Phones join ShadowFleet, then scan the code on the table.</small>')) + '</p>';
    if (n.saved.length) {
      h += '<div class="setH" style="margin-top:8px">SAVED NETWORKS</div>';
      n.saved.forEach((ss, i) => {
        const arm = netArm === 'f' + i;
        h += '<div class="netRow"><b>' + esc(ss) + (n.home && ss === n.ssid ? '<em>IN USE</em>' : '') + '</b><button class="' + (arm ? 'armed' : '') + '" onclick="netForget(' + i + ')">' + (arm ? 'FORGET?' : 'FORGET') + '</button></div>';
      });
    }
    if (n.canHome) {
      h += '<button class="pill quiet setPill" onclick="location.href=\x27/setup\x27">ADD A NETWORK<small>The table remembers up to four and joins the first one it finds</small></button>';
      const to = n.home ? 0 : 1;
      if (n.home || n.saved.length) {
        const arm = netArm === 'm';
        h += '<button class="pill quiet setPill"' + (arm ? ' style="border-color:#FFB347;color:#FFB347"' : '') + ' onclick="netMode(' + to + ')">' +
             (arm ? 'TAP AGAIN TO SWITCH' : n.home ? 'USE THE TABLE\u2019S OWN WI-FI' : 'USE HOME WI-FI') + '</button>';
        if (arm) h += '<p class="setWarn">The table restarts (a game in progress is saved). Every phone then joins ' + (n.home ? 'ShadowFleet' : 'your home Wi-Fi') + ' and scans the new code.</p>';
      }
    }
  }
  h += '<p class="setNet" style="text-align:center;margin-top:14px"><small>' + PAGE_FW + '</small></p><button class="back" onclick="sheetHide()">DONE</button>';
  sheetShow('settings', h, '#5FD4FF');
}
function esc(x) { return String(x).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;'); }
async function netForget(i) {
  if (netArm !== 'f' + i) { netArm = 'f' + i; settingsShow(); return; }
  netArm = ''; const r = await post('/net_forget?i=' + i);
  if (r && r.saved) NETI = r; settingsShow();
}
async function netMode(home) {
  if (netArm !== 'm') { netArm = 'm'; settingsShow(); return; }
  netArm = ''; await post('/net_mode?home=' + home);
  sheetShow('settings', '<div class="sheetTitle" style="color:#5FD4FF">RESTARTING THE TABLE</div><p class="setNet">' +
    (home ? 'It\u2019s joining your home Wi-Fi. Switch this phone to it, then scan the code on the table.' : 'It\u2019s starting its own network. Join <b>ShadowFleet</b> (password starwars1), then scan the code on the table.') +
    '</p><button class="back" onclick="sheetHide()">OK</button>', '#5FD4FF');
}
function prepareHangar(s, me, html) {
  html += phaseTrackHtml(s);  // 3.1-dev.56
  html += liveNowHtml(s);  // 3.1-dev.103
  sellAutoOpen(s);  // 3.1-dev.48
  pressAutoOpen(s);  // 3.1-dev.88 your faction presses you to declare
  bcAutoOpen(s);     // 3.1-dev.98 a back-channel offer
  gfhAutoOpen(s);    // 3.1-dev.102 guns for hire
  beatenAutoOpen(s); // 3.1-dev.102 beaten to a world
  if (s.fh) {  // 3.1-dev.43 THE FINAL HOUR: Last Orders, the reveal, the finale
    fhAutoOpen(s);
    if (s.fh.log) { fhPlayEnsure(s); fhSync(s); }
    if (fhFinal(s) && s.phase === 1) { setApp(app, html + fhOrdersHtml(s, me)); return; }
    if (fhFinal(s) && s.phase === 2 && s.fh.st === 1) { setApp(app, html + fhRevealHtml(s, me)); return; }
    if (s.fh.st >= 2) { setApp(app, html + '<div class="fhkick">THE FINAL HOUR</div><div class="fhbig">THE FINALE IS PLAYING</div><div class="fhsub">Watch the table. The final standings come when the last world is decided.</div><button class="pill" onclick="fhReplay()">WATCH IT HERE</button>'); return; }
  }
  const rank = 1 + s.players.filter((p, i) => i !== mySlot && p.active && p.vp > me.vp).length;
  const merc = (s.v2 && s.v2.perks && s.v2.perks.merc) || 0;
  // dev.37b (user: "I liked the way the old one showed VP and credits better"): the three stat boxes are back.
  html += '<div class="stats" style="margin:4px 0 10px"><div class="stat"><span>CREDITS</span><b>'+pnum(me.credits, '#FFD166', 3)+'</b></div>' +
          '<button class="stat defbox" onclick="deckGo(\'war\')" aria-label="Standings"><span>VICTORY PTS</span><b>'+pnum(me.vp, '#E8EEF5', 3)+'</b><small class="rk">'+(s.v2 && s.v2.vpHidden ? '?' : (['1st','2nd','3rd','4th'][rank - 1] || rank+'th'))+'</small></button>' +
          defBoxHtml(s) + '</div>' + firstChecklistHtml(s);  // 3.1-dev.84
  const res = s.phase === 2, mov = s.phase === 1;
  html += eraBannerHtml(s);  // 3.1-dev.12
  if (mov && !(s.round >= s.roundLimit)) html += '<div class="movcard"><b>ON THE BOARD</b><span>One step each: along a ring, or one lane in or out.</span></div>';
  html += '<div class="hangar'+(res ? ' res' : mov ? ' mov' : '')+'"><div class="hgtitle"><span>'+(res || mov ? 'YOUR FLEETS' : 'YOUR HANGAR')+'</span><small>' +
          (res ? 'each can attack once this round' : mov ? (merc ? '+'+merc+' hired ships waiting' : 'where they are is on the board') : merc ? '+'+merc+' hired ships waiting' : 'tap a bay: build \u00B7 repair')+'</small></div>' +
          [0, 1, 2].map(k => hangarBayHtml(s, me, k)).join('') + '</div>';
  if (mov && s.stories) s.stories.forEach(t => { html += '<div class="rumor"><span>HOLONET RUMOR</span><b>\u201C'+t+'\u201D</b><small>Never says who. Ask around.</small></div>'; });  // 3.1-dev.13
  if (mov && s.lie) {  // 3.1-dev.12 the Shadow Age: one of these is a lie (the order changes by round)
    const a = '<div class="rumor"><span>HOLONET RUMOR</span><b>\u201C'+s.lie+'\u201D</b><small>'+(s.v2 && s.v2.myLie ? 'YOUR LIE. Act normal.' : 'In the Shadow Age, one rumor is a lie.')+'</small></div>';
    if (s.round % 2) html += a;
    s.lieShown = s.round % 2 ? 1 : 2;
  }
  if (mov && s.rumor) {  // dev.41: a rumor about a world YOU hold says so -- on your phone only
    const rp = s.planets[s.rumorP], mine = rp && rp.owner === mySlot;
    html += '<div class="rumor'+(mine ? ' mine' : '')+'"><span>HOLONET RUMOR</span><b>\u201C'+s.rumor+'\u201D</b>' +
            (mine ? '<em>'+rp.name.toUpperCase()+' IS YOUR WORLD.</em>' : '') +
            '<small>'+(mine ? 'Only you see this line. Act normal.' : s.lie ? 'In the Shadow Age, one rumor is a lie.' : 'Always true. Never says who. Ask around.')+'</small></div>';
  }
  if (mov && s.lie && s.lieShown === 2) html += '<div class="rumor"><span>HOLONET RUMOR</span><b>\u201C'+s.lie+'\u201D</b><small>'+(s.v2 && s.v2.myLie ? 'YOUR LIE. Act normal.' : 'In the Shadow Age, one rumor is a lie.')+'</small></div>';
  if (res && merc) html += '<div class="hgbar" style="border-color:#B8A88A;color:#B8A88A">+'+merc+' hired ships ride with your next battle</div>';
  if (bmOpenNow(s)) html += bmDoorHtml(s);  // 3.1-dev.77: the markets went dark
  else if (s.phase === 0 && s.v2 && s.mkt !== 0) {  // the markets: Prepare only  // dev.37b (user: the market bar was "a little disjointed"): this round's four shops as wooden tiles
    const shut = s.v2.ashen && s.v2.ashen.alive;
    if (shut) html += '<button class="hgbar" style="border-color:var(--red);color:var(--red)" onclick="openMarkets()">MARKETS CLOSED \u00B7 the Ashen Fleet<em></em></button>';
    else {
      html += '<div class="mkstrip"><div class="hgtitle"><span style="color:#FFD166">MARKETS' + eraTag(s, 2, '2 GOODS \u00b7 SPICE') + '</span><small>tap to open the market</small></div><div class="mktiles">';
      (s.v2.stalls || []).forEach(st => {  // 3.1-dev.9: one tile per faction stall; shut to you = dashed
        const goods = st.it.filter(x => x[0] >= 0).map(x => (s.v2.goods[x[0]] || [''])[0]).join('<br>');
        html += '<button class="mktile'+(st.shut ? ' shut' : '')+'" onclick="openMarkets()"><b style="color:'+FACTION_UI_COLORS[st.f]+'">'+FACTION_SHORT_NAMES[st.f]+'</b><span>'+(s.v2.emb === st.f ? 'EMBARGO' : st.shut ? 'CLOSED TO YOU' : goods)+'</span></button>';
      });
      html += '</div></div>';
    }
  }
  // (3.1-dev.15: deals are gone -- the vision doc cut them)
  // 3.1-dev.103: the Galaxy and You lives on the COMMS deck now (user: "it grows and grows and feels random")
  // (3.1-dev.16: a job offer no longer pops up -- it waits on Home and in your Inbox until Prepare ends)
  if (s.v2 && s.v2.event >= 0) html += '<div class="note" style="color:#FF8C42;margin:6px 0 0;padding:6px 10px;font-size:13px"><b>'+s.v2.eventName+'</b> · '+s.v2.eventText+'</div>';
  // 3.1-dev.103: GALAXY / STANDINGS / FAVOR are decks on the cockpit console now (the spinning planet is the STAR MAP key)
  if (res && s.players.some((pl, k) => k !== mySlot && pl.active && pl.fleets.some(f => f.alive > 0)))
    html += '<button class="hgatk" onclick="openAttackPlayers()">ATTACK A PLAYER</button>';
  {  // 3.1-dev.104 (user: "Idk if the lock in needs to be on every single page"; "it never moves"): on the BRIDGE only, at the end of the page
    const active = s.players.filter(p => p.active).length, locked = s.players.filter(p => p.active && p.lockedIn).length;
    const lockTxt = me.lockedIn ? 'LOCKED IN<small>waiting for ' + (active - locked) + '</small>' : (res ? 'DONE THIS ROUND' : mov ? 'DONE MOVING' : 'LOCK IN') + '<small>hold \u00b7 ' + locked + ' of ' + active + ' ready</small>';
    html += '<button class="dkLock' + (me.lockedIn ? ' done' : '') + '"' + (me.lockedIn ? ' disabled' : '') + ' onpointerdown="lockHold(event)" onpointerup="lockLet()" onpointerleave="lockLet()" onpointercancel="lockLet()" onclick="lockTap()"><i class="fill"></i><span>' + lockTxt + '</span></button>';
  }
  setApp(app, html);
}

// UI 3.0 take 4: a world's page in Resolution (mockup B) -- what's known as small tiles, your odds,
// SEND SOMEONE IN (the informants), its market, and big FIGHT / TALK / PAY.
// 3.1-dev.115 THE WORLD PAGE, STREAMLINED (canvas board 24; user: "smoother, less to scroll around"): nothing to buy any more.
// DEFENSE = a public, live level (the same word means the same strength on every world); SYMPATHY = known or not (it grows
// outward from the worlds you hold); RESOURCES + TRAIT; the worlds one jump away; swipe sideways for the next world.
const DEF_LV = [['LOW', '#8CE7A0', '1\u20132'], ['MEDIUM', '#FFB347', '3\u20135'], ['HIGH', '#FF6B5A', '6\u20138'], ['FORTRESS', '#FF6B5A', '9+']];
function defLevel(p) { if (p.dv !== undefined) return p.dv; const d = p.defense || 0; return d <= 2 ? 0 : d <= 5 ? 1 : d <= 8 ? 2 : 3; }
function worldOrder(s) { return s.planets.map((p, i) => i).filter(i => s.planetIncluded[i]).sort((a, b) => (s.planets[a].ring - s.planets[b].ring) || (a - b)); }
function worldFactsHtml(s, i) {
  const p = s.planets[i], mine = p.owner === mySlot, lv = defLevel(p), L = DEF_LV[lv];
  const bars = [0, 1, 2, 3].map(k => '<i style="background:' + (k <= lv ? L[1] : '#233142') + '"></i>').join('');
  const defT = '<div class="wfT"><span>DEFENSE</span><b style="color:' + (mine ? 'var(--text)' : L[1]) + '">' + (mine ? (p.defense || 0) + ' <small>/ ' + (p.dl || 8) + '</small>' : L[0]) + '</b>' +
               '<div class="wfBars">' + bars + '</div><small>' + (mine ? 'your defenders' : L[2] + ' defenders \u00b7 live') + '</small></div>';
  const sc = p.chars[0], fi = sc.revealed ? AFFILIATION_TO_FACTION.indexOf(sc.value) : -1;
  const symT = '<div class="wfT"><span>SYMPATHY</span>' + (sc.revealed ? '<b style="color:' + (fi >= 0 ? FACTION_UI_COLORS[fi] : 'var(--text)') + '">' + sc.value + '</b>' + (fi >= 0 ? '<small>You: ' + favorWord(fi, tierIdx(s, fi)) + '</small>' : '')
                                                             : '<b style="color:#5C6F84">UNKNOWN</b><small>Hold a world next to it to learn whose side it\u2019s on</small>') + '</div>';
  let h = '<div class="wfGrid">' + defT + symT + '</div>';
  const res = p.chars[1], tr = p.chars[2];
  h += '<div class="wfLine"><span>RESOURCES</span><b>' + (res.revealed ? res.value : '<span style="color:#5C6F84;font-weight:400">Unknown</span>') + '</b></div>';
  h += '<div class="wfLine"><span>TRAIT</span><b>' + (tr.revealed ? tr.value + (p.traitFx ? ' <small>\u00b7 ' + p.traitFx + '</small>' : '') : '<span style="color:#5C6F84;font-weight:400">Unknown</span>') + '</b></div>';
  const nb = []; for (let n = 0; n < s.planets.length; n++) if (((p.nb || 0) >>> n) & 1 && s.planetIncluded[n]) nb.push(n);
  if (nb.length) h += '<div class="wfNbH">ONE JUMP AWAY</div><div class="wfNb">' + nb.map(n => { const o = s.planets[n].owner;
    const col = o === mySlot ? 'var(--accent)' : o >= 0 && s.players[o] && s.players[o].chosenFaction >= 0 ? FACTION_UI_COLORS[s.players[o].chosenFaction] : '#2A3646';
    return '<button onclick="openPlanet(' + n + ')"><i style="background:' + col + '"></i>' + s.planets[n].name + '</button>'; }).join('') + '</div>';
  return h;
}
function worldPagerHtml(s, i) {
  const ord = worldOrder(s), k = ord.indexOf(i);
  return '<div class="wfPager">' + ord.map((x, j) => '<i' + (j === k ? ' class="on"' : '') + '></i>').join('') + '</div><div class="wfHint">Swipe sideways for the next world \u00b7 drag the top bar down to close</div>';
}
function worldStep(d) { const s = lastS; if (!s || currentPlanet < 0) return; const ord = worldOrder(s), k = ord.indexOf(currentPlanet); if (k < 0) return;
  const j = (k + d + ord.length) % ord.length; currentPlanet = ord[j]; shownHtml = null; render(s); window.scrollTo(0, 0); }
(function () {  // sideways swipe on a world page = the next / previous world
  let x0 = null, y0 = 0, t0 = 0;
  document.addEventListener('touchstart', e => { const ws = e.target.closest && e.target.closest('.wsheet'); x0 = ws && !sheetKey ? e.touches[0].clientX : null; y0 = e.touches[0].clientY; t0 = Date.now(); }, { passive: true });
  document.addEventListener('touchend', e => { if (x0 === null) return; const c = e.changedTouches[0], dx = c.clientX - x0, dy = c.clientY - y0; x0 = null;
    if (Date.now() - t0 < 600 && Math.abs(dx) > 60 && Math.abs(dx) > Math.abs(dy) * 1.6) worldStep(dx < 0 ? 1 : -1); }, { passive: true });
})();
function resWorldBody(s, i) {
  const p = s.planets[i], me = s.players[mySlot], mine = p.owner === mySlot;
  const KEYS = ['SYMPATHETIC TO', 'RESOURCES', 'TRAIT', 'DEFENSE'];
  let h = '';  // 3.1-dev.115: the facts come after the take-over buttons (worldFactsHtml)
  if (!mine) {
    const o = worldOdds(s, i);
    h += '<div class="rwodds">';
    if (o && !o.none) h += '<div class="rwline"><span>Fleet '+(o.best.k + 1)+' \u00B7 '+o.best.n+(o.esc ? ' + escort' : '')+(o.merc ? ' + '+o.merc+' hired' : '')+' vs '+(p.gr[0] === p.gr[1] ? p.gr[0] : p.gr[0]+'\u2013'+p.gr[1])+(o.milSure ? ' + militia' : '')+'</span>' +
                            '<b style="color:'+o.v.col+'">'+o.v.word+' <small>'+(o.lo === o.hi ? o.lo : o.lo+'\u2013'+o.hi)+'%</small></b></div>';
    else h += '<div class="rwline"><span>No fleet ready to attack this round.</span></div>';
    h += '</div>';
  }
  if (p.mOpen && s.v2) h += '<div class="rwmkt"><b>MARKET</b> \u00B7 '+p.good+' \u00B7 '+marketCutLine(s, i, true)+'</div>';
  return h + '<div class="rwacts">' + takeoverActsHtml(s, i) + '</div>' + worldFactsHtml(s, i) + favorPreview(s, i) + worldPagerHtml(s, i);
}
// 2.0-dev.27 THE INFORMANT: same effect for everyone, each faction's own way of doing it.
const INFORMANT_NAMES = [
  ['A SYMPATHIZER TALKS', 'SEND PATHFINDERS'],        // Rebels
  ['PULL THE ISB FILE', 'LAUNCH A PROBE DROID'],      // Empire
  ['A SPICE RUNNER OWES YOU', 'BRIBE THE DOCKMASTER'], // Pykes
  ['CALL IN A FAVOR', 'SLICE THEIR RECORDS'],         // Crimson Dawn
];
// Forecast graphics (v0.21.9), twins of the AMOLED's forecastUnits/Die.
function fcShips(n, col, facingLeft) {
  // v0.28.0: more than six (two or three fleets together) -> rows of six, smaller.
  const rows = Math.max(1, Math.ceil(n / 7)), sz = rows === 1 ? 14 : 9, step = rows === 1 ? 19 : 13, rh = rows === 1 ? 18 : 12;  // 2.0-dev.20: rows of 7
  let s = '';
  for (let i = 0; i < n; i++) {
    const c = i % 7, r = Math.floor(i / 7), y = r * rh;
    const b = facingLeft ? c*step+sz : c*step, t = facingLeft ? c*step : c*step+sz;  // back / nose x
    s += '<polygon points="'+b+','+(y+2)+' '+t+','+(y+2+sz/2)+' '+b+','+(y+2+sz)+'" fill="'+col+'"/>';
  }
  return '<svg class="units" width="'+Math.max(1, Math.min(n, 7)*step-(step-sz))+'" height="'+(rows*rh)+'">'+s+'</svg>';
}
function fcDie(mod, src, col) {
  return '<div class="die" style="border-color:'+col+'">d6'+(mod > 0 ? '<span class="badge" style="background:'+col+'">+'+mod+'</span>' : '')+'</div>' +
         '<div class="src">'+(mod > 0 && src >= 0 ? insigniaSvg(src, 28) : '')+'</div>';
}
// v0.21.6: no approach screen any more -- backing out of the briefing or the
// pay confirm closes the (uncommitted, free) session, back to the planet page.
function backFromBriefing() { cancelTakeover(); }
function askLeaveNegotiation() { leaveNegPending = true; poll(); }
function stayInNegotiation() { leaveNegPending = false; poll(); }
async function confirmFight(sure) {
  const warn = !sure && lastS ? fightWarning(lastS) : '';  // 3.1-dev.13 ceasefire / cover
  if (warn) {
    sheetShow('fightwarn', '<div class="sheetTitle" style="color:#FF6B5A">ARE YOU SURE?</div><div class="annLine" style="margin:10px 0">' + warn + '</div>' +
      '<button class="pill" style="border-color:#FF6B5A;color:#FF6B5A" onclick="sheetHide();confirmFight(true)">ATTACK ANYWAY</button><button class="pill" style="border-color:var(--line);color:var(--dim)" onclick="sheetHide()">BACK</button>', '#FF6B5A');
    return;
  }
  if (sure && lastS) await post('/break?id=' + encodeURIComponent(myId) + '&planet=' + lastS.takeoverPlanet);
  briefingPending = false;
  await post('/takeover_commit?id='+encodeURIComponent(myId)+'&method=1&fleets='+(briefFleet | (briefMercs ? 0 : 0x40)));  // dev.28
  poll();
}
function backFromPayConfirm() { cancelTakeover(); }
// v0.29.0: a successful purchase gets a short card with the reputation arrows.
let purchased = null;  // { p, base } until CONTINUE
async function confirmPay() {
  payConfirmPending = false;
  const p = lastS ? lastS.takeoverPlanet : -1, base = lastS ? lastS.reputation.map(r => r.score || 0) : null;
  const wasHeld = p >= 0 && lastS.planets[p] && lastS.planets[p].owner >= 0 && lastS.planets[p].owner !== mySlot;
  await post('/takeover_commit?id='+encodeURIComponent(myId)+'&method=3');
  const s = await api('/state?id=' + encodeURIComponent(myId));
  if (s && p >= 0 && s.planets[p] && s.planets[p].owner === mySlot) purchased = { p: p, base: base };
  else if (wasHeld && s && s.planets[p] && s.planets[p].bo) notify('OFFER MADE', s.planets[p].bo[1] + ' CR for ' + s.planets[p].name + '. They answer by next round.', '#FFD166', 6000);  // 3.1-dev.79
  poll();
}
function closePurchased() { purchased = null; poll(); }
async function battleAction(a) { await post('/battle_action?id='+encodeURIComponent(myId)+'&action='+a); poll(); }
// v0.21.13 battle report: CONTINUE at the end of a battle opens it (and
// closes the battle); closing it lets any held transmission play.
let victory = null, victoryKeyDone = '';
function bContinue() {
  if (bView && bView.over) { victory = { v: bView }; victoryKeyDone = bKey; }
  battleAction(9);
}
function closeVictory() { victory = null; repBattleBase = null; poll(); }
// Same rules as the AMOLED's BattleOutcome::of(): result 1 won, 0 lost, 2 withdrew.
function battleOutcome(v) {
  const iAttack = !v.pvp || v.leftIsAttacker;
  const atk = v.atkAlive.filter(Boolean).length, def = v.defAlive.filter(Boolean).length;
  const o = { myShips: iAttack ? atk : def, theirShips: iAttack ? def : atk, decisive: false, result: 0 };
  if (!v.pvp) { o.result = v.captured ? 1 : (v.retreat === 1 && atk > 0 ? 2 : 0); return o; }
  let atkWon;
  if (def === 0) { atkWon = true; o.decisive = true; }
  else if (atk === 0) { atkWon = false; o.decisive = true; }
  else atkWon = v.retreat === 2;  // the side that stayed wins
  const iWithdrew = (iAttack && v.retreat === 1) || (!iAttack && v.retreat === 2);
  o.result = (iWithdrew && o.myShips > 0) ? 2 : (atkWon === iAttack ? 1 : 0);
  return o;
}
async function cancelTakeover() { payConfirmPending = false; briefingPending = false; leaveNegPending = false; await post('/takeover_cancel?id='+encodeURIComponent(myId)); poll(); }
async function pvpAttack(defSlot, defFleet, sure) {
  if (!sure) { const w = bcWarn(lastS, -1, 'pvp', defSlot); if (w) return bcConfirm(w, 'pvpAttack(' + defSlot + ',' + defFleet + ', true)'); }  // 3.1-dev.98
  const r = await post('/pvp_attack?id='+encodeURIComponent(myId)+'&atkFleets='+(pvpAtkMask | (briefMercs ? 0 : 0x40))+'&defSlot='+defSlot+'&defFleet='+defFleet);
  // v0.20.2: a refused attack says why (e.g. that fleet already fought).
  if (r && r.ok === false) { pvpMsg = r.reason || 'Attack refused.'; poll(); return; }
  // Started: leave the attack flow so the player lands on Home after the battle.
  pvpMsg = ''; pvpTarget = -1; pvpDefFleet = -1; battleMode = false; fleetsMode = false; poll();
}
async function investFleet(f) { await post('/invest_fleet?id='+encodeURIComponent(myId)+'&fleet='+f); poll(); }
function openPlanet(p) { currentPlanet = p; poll(); }
function backToPlanets() { currentPlanet = -1; poll(); }
function openBattle() { battleMode = true; poll(); }
function closeBattle() { battleMode = false; pvpTarget = -1; pvpDefFleet = -1; pvpMsg = ''; poll(); }
// v0.21.10 attack flow: Fleets (attack mode) -> opponent -> their fleet -> forecast.
function attackWith(f) { pvpAtkFleet = f; pvpAtkMask = 1 << f; pvpTarget = -1; pvpDefFleet = -1; pvpMsg = ''; battleMode = true; poll(); }
function pickDefFleet(f) { pvpDefFleet = f; pvpMsg = ''; poll(); }
function pickPvpAtk(f) { pvpAtkMask ^= 1 << f; poll(); }  // v0.28.0: toggles fleets in and out
function backToDefFleets() { pvpDefFleet = -1; pvpMsg = ''; poll(); }
function pickTarget(slot) { pvpTarget = slot; pvpMsg = ''; poll(); }
function backToTargets() { pvpTarget = -1; pvpDefFleet = -1; pvpMsg = ''; poll(); }
function openFleets() { fleetsMode = true; poll(); }
// v0.27.3 ATTACK WHAT? (twin of the AMOLED's showAttackChoice): a planet or a player.
let attackChoice = false;
function openAttack() { attackChoice = true; poll(); }
function attackPlanet() { attackChoice = false; galaxyMode = true; poll(); }
function attackPlayer() { attackChoice = false; fleetsMode = true; poll(); }
function closeAttack() { attackChoice = false; poll(); }
function closeFleets() { fleetsMode = false; currentFleetIdx = -1; poll(); }
function openReputation() { repMode = true; poll(); }
function closeReputation() { repMode = false; poll(); }
function openStandings() { standingsMode = true; poll(); }
// 2.0: markets, the diplomat, the relay ("look at the table") and personal notices.
let marketsMode = false;
function openMarkets() { marketsMode = true; poll(); }
function closeMarkets() { marketsMode = false; poll(); }
// 3.1-dev.9 FACTION STALLS: Survey asks which world, Audience which faction (bottom sheets).
function stallBuy(f, k) {
  const s = lastS; if (!s || !s.v2) return;
  const st = (s.v2.stalls || []).find(x => x.f === f); if (!st) return;
  const g = st.it[k][0], G = s.v2.goods[g] || [''];
  if (g === 2) {  // SURVEY
    const ws = s.planets.map((p, i) => i).filter(i => s.planets[i].owner !== mySlot && (s.planets[i].chars || []).some(c => !c.revealed));
    sheetShow('buy', '<div class="sheetTitle">' + G[0] + '</div><div class="sheetSub">Which world do you want surveyed?</div><div class="pickgrid">' +
      ws.map(i => '<button onclick="stallBuyGo(' + f + ',' + k + ',' + i + ')">' + s.planets[i].name + '</button>').join('') + '</div>', FACTION_UI_COLORS[f]);
    return;
  }
  if (g === 6) {  // AUDIENCE
    sheetShow('buy', '<div class="sheetTitle">' + G[0] + '</div><div class="sheetSub">An audience with which faction? It moves your standing up.</div><div class="audList">' +
      [0, 1, 2, 3].map(x => '<button class="audRow" onclick="stallBuyGo(' + f + ',' + k + ',' + x + ')"><div class="audTop">' + insigniaSvg(x, 20) + '<b style="color:' + FACTION_UI_COLORS[x] + '">' + FACTION_SHORT_NAMES[x] + '</b>' +
        '<span style="color:' + FAVOR_COL[tierIdx(s, x)] + '">' + favorWord(x, tierIdx(s, x)).toUpperCase() + '</span></div>' + standingBar(s, x) + '</button>').join('') + '</div>', FACTION_UI_COLORS[f]);
    return;
  }
  stallBuyGo(f, k, -1);
}
async function stallBuyGo(f, k, t) { sheetHide(); await post('/stall_buy?id=' + encodeURIComponent(myId) + '&f=' + f + '&k=' + k + '&t=' + t); poll(); }
async function marketBuy(p) { await post('/market_buy?id='+encodeURIComponent(myId)+'&planet='+p); poll(); }
// 2.0 DEALS: the inbox / your promises, and the offer form. While the form is
// open the page never rebuilds it on a poll (Convention 8 -- "please don't
// make the keyboard glitchy"): the message box only stores what you type, and
// a tap on a chip or a +/- rebuilds it once from dealForm.
// 2.0 sprites -- the same grids as ASHEN_SPRITE / CLOSED_SPRITE in the .ino
// (tools/art/sprites_2_0.py regenerates both; keep them in step).
const ASHEN_SPRITE = [
  ".................................lll....", ".............................llllhhh....", ".........................llllhdhhhhhree.",
  "....................lllllhhhhhdhhrhhreye", "................lellhhhhdhhhhhdhhhhhree.", "...........lllllhherhhhhdlllllllhhhh....",
  ".......lllrhdhhhhhdhhhhhdlhhhhhdhhhh....", "...llllhhhhrhhhlllllllllllhyyyhdllhhree.", "dddhhhllllllllllllllllllllhhhhhdllhhreye",
  "...ddddhhhhhhhhddddddddddlhhhhhdddhhree.", ".......ddddhdhrhhhdhhhhhdlhhhhhdhhhh....", "...........dddddhhdhhrhhdlddddddhhhh....",
  "................ddddhhrhdhhhhhdhhhhh....", "....................dddddhhhhhdhhhehree.", ".........................ddddhdhhhhhreye",
  ".............................ddddhhhree.", ".................................ddd...."];
const CLOSED_SPRITE = [
  "........nn.........................nn.........", "........c..........................c..........", ".........c..........................c.........",
  "........c..........................c..........", ".........c..........................c.........", "........c..........................c..........",
  ".kkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkk.", ".kwnwwwwvwwwwwwwwwwvwwwwwwwwwwvwwwwwwwwwwvnwk.", ".kkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkk.",
  ".kwwvwwwwwwwwwwvwwwwwwwwwwvwwwwwwwwwwvwwwwwwk.", ".kvwwwRRRwwRwvwwwwRRRwwwRRRRwRRRRRwRRRRwwwwwk.", ".kwwwRwwwRwRwwwwwRwwwRvRwwwwwRwwwvwRwwwRwwwwk.",
  ".kwwwRwwwvwRwwwwwRwwvRwRwwwwwRwvwwwRwwwRwwvwk.", ".kwnwRwvwwwRwwwwwRvwwRwwRRRwwRRRRwwRwwwRvwnwk.", ".kwwwRwwwwwRwwwwvRwwwRwwwwwRwRwwwwwRwwvRwwwwk.",
  ".kwvwRwwwRwRwwvwwRwwwRwwwvwRwRwwwwwRvwwRwwwwk.", ".kwwwwRRRwwRRRRRwwRRRwwRRRRwwRRRRRvRRRRwwwwwk.", ".kwwwwwwwwvwwwwwwwwwvwwwwwwwwwwvwwwwwwwwwwvk.",
  ".kwwwwwwvwwwwwwwwwwvwwwwwwwwwwvwwwwwwwwwwvwwk.", ".kkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkk.", ".kwnvwwwwwwwwwwvwwwwwwwwwwvwwwwwwwwwwvwwwwnwk.",
  ".kkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkk."];
const SPRITE_PAL = {h:'#4A5060', l:'#7A8296', d:'#2A2E38', y:'#FFD27A', e:'#FF8C42', r:'#E0404A', c:'#8C8C96', n:'#C8C8D2', w:'#8B5E34', v:'#78502C', k:'#462C18', R:'#D62828'};
const spriteUrls = {};
function spriteUrl(name, rows, cell) {
  const key = name + cell;
  if (spriteUrls[key]) return spriteUrls[key];
  const c = document.createElement('canvas');
  c.width = rows[0].length * cell; c.height = rows.length * cell;
  const g = c.getContext('2d');
  rows.forEach((r, y) => { for (let x = 0; x < r.length; x++) if (SPRITE_PAL[r[x]]) { g.fillStyle = SPRITE_PAL[r[x]]; g.fillRect(x * cell, y * cell, cell, cell); } });
  return (spriteUrls[key] = c.toDataURL());
}
// 2.0-dev.21 MARKET SHELVES (board M take 3, user: "the shelves look awesome"): each
// open market is a plank on a dark wall, its good a long pixel object lying on it
// with the good's name stenciled on, stock lamps at the lip, BUY at the side;
// sold out = an empty dusty plank, "GONE". Ported from the mockup's generator;
// the eight goods it didn't draw (REPAIRS ... TROOPS) are new in the same style.
// One SVG string per (good, stock, plank), cached -- drawn once, not per poll.
const SH_PX = 3, SH_W = 246, SH_PY = 58;  // pixel size, shelf width, top of the plank
function shRnd(seed) { const x = Math.sin(seed * 12.9898) * 43758.5453; return x - Math.floor(x); }
function shR(x, y, w, h, c, op) { return '<rect x="'+x.toFixed(1)+'" y="'+y.toFixed(1)+'" width="'+w.toFixed(1)+'" height="'+h.toFixed(1)+'" fill="'+c+'"'+(op ? ' fill-opacity="'+op+'"' : '')+'/>'; }
function shPlank(x, y, w, seed, dusty) {
  const px = SH_PX, rows = 9;
  const c = dusty ? ['#5E4630', '#7A6046', '#4E3A28', '#3E2E20', '#1C140C'] : ['#8B5E34', '#C08A50', '#6F4828', '#5A3A20', '#24160C'];
  const base = c[0], light = c[1], dark = c[2], edge = c[3], shade = c[4];
  let o = shR(x, y, w, px, light) + shR(x, y + px, w, px * (rows - 3), base);
  const n = Math.floor(w / px);
  for (let r = 1; r < rows - 2; r++) {  // grain
    let i = 0;
    while (i < n) {
      if (shRnd(seed * 97 + r * 13 + i) > 0.82) { const L = 3 + Math.floor(shRnd(seed + r * 5 + i) * 9); o += shR(x + i * px, y + r * px, Math.min(L, n - i) * px, px, dark); i += L + 2; }
      else i++;
    }
  }
  for (let k = 0; k < 2; k++) { const kx = x + (0.2 + 0.6 * shRnd(seed * 3 + k)) * w; o += shR(kx, y + px * 3, px * 3, px * 2, edge) + shR(kx + px, y + px * 3, px, px, '#3A2414'); }
  o += shR(x, y + px * (rows - 2), w, px, edge) + shR(x, y + px * (rows - 1), w, px, shade);
  for (const bx of [x + px * 6, x + w - px * 9]) {  // iron brackets
    o += shR(bx, y + px * rows, px * 3, px * 7, '#3C4048') + shR(bx, y + px * rows, px * 3, px, '#6A707C');
    o += shR(bx - px * 2, y + px * rows, px * 7, px * 2, '#3C4048') + shR(bx + px, y + px * (rows + 2), px, px, '#9AA2B0');
  }
  if (dusty) for (let k = 0; k < 10; k++) o += shR(x + shRnd(seed + k * 3) * w, y - px, px, px, '#7A6046', 0.6);
  return o;
}
function shBody(x, y, L, H, base, light, dark) { const px = SH_PX; return shR(x, y, L, H, base) + shR(x, y, L, px * 2, light) + shR(x, y + H - px * 2, L, px * 2, dark); }
// the stenciled name: a plate, and text sized to fit it
function shLabel(cx, top, w, label, plate, ink) {
  const px = SH_PX, fs = Math.min(px * 4.4, (w - 8) / (label.length * 0.8));
  return shR(cx - w / 2, top + px * 3, w, px * 7, plate) +
         '<text x="'+cx.toFixed(1)+'" y="'+(top + px * 6.5 + fs * 0.36).toFixed(1)+'" font-size="'+fs.toFixed(1)+'" font-weight="800" letter-spacing="1" text-anchor="middle" fill="'+ink+'">'+label+'</text>';
}
// A good's long object lying on the shelf, bottom at y. id = GoodId in game_state.h.
function shGood(id, x, y, L, label) {
  const px = SH_PX, H = px * 13, top = y - H;
  let o = '';
  switch (id) {
    case 4: {  // MERCENARIES: two missiles, red noses right
      for (let j = 0; j < 2; j++) {
        const ty = top + j * Math.floor(H / 2) + px, h = Math.floor(H / 2) - px;
        o += shBody(x + px * 6, ty, L - px * 16, h, '#C8CED8', '#EEF2F8', '#8A92A0');
        for (let k = 0; k < 5; k++) o += shR(x + L - px * 10 + k * px * 2, ty + k * px * 0.6, px * 2, h - k * px * 1.2, '#D2452E');
        o += shR(x, ty - px, px * 6, h + px * 2, '#5A616E') + shR(x + px * 2, ty - px * 3, px * 3, px * 2, '#5A616E') + shR(x + px * 2, ty + h + px, px * 3, px * 2, '#5A616E');
        o += shR(x + px * 12, ty, px * 2, h, '#D2452E');
      }
      return o + shLabel(x + L * 0.45, top - px, L * 0.5, label, '#F2E6C8', '#24160A');
    }
    case 0: {  // FUEL: a long tank with end caps and a valve
      o += shBody(x + px * 3, top, L - px * 6, H, '#D2452E', '#F07A50', '#8A2A1A');
      o += shR(x, top + px, px * 3, H - px * 2, '#5A3A20') + shR(x + L - px * 3, top + px, px * 3, H - px * 2, '#5A3A20');
      for (const bx of [x + L * 0.18, x + L * 0.78]) o += shR(bx, top, px * 2, H, '#8A2A1A');
      o += shR(x + L - px * 10, top - px * 3, px * 4, px * 3, '#8A92A0');
      return o + shLabel(x + L * 0.48, top, L * 0.5, label, '#FFD27A', '#3A1A10');
    }
    case 9: {  // BACTA: a glass tube of blue gel with bubbles
      o += shR(x + px * 4, top, L - px * 8, H, '#C8DCE8') + shR(x + px * 5, top + px, L - px * 10, H - px * 2, '#2E9CBC') + shR(x + px * 5, top + px, L - px * 10, px * 2, '#8FE3FF');
      for (let b = 0; b < 9; b++) o += shR(x + px * 8 + shRnd(b + 3) * (L - px * 20), top + px * 3 + shRnd(b * 7) * px * 7, px * 2, px * 2, '#8FE3FF');
      o += shR(x, top - px, px * 4, H + px * 2, '#8A92A0') + shR(x + L - px * 4, top - px, px * 4, H + px * 2, '#8A92A0');
      return o + shLabel(x + L / 2, top, L * 0.5, label, '#0E3A48', '#E6F8FF');
    }
    case 7: {  // CARGO RUN: a long crate with straps
      o += shBody(x, top, L, H, '#8B5E34', '#B07A44', '#5A3A20');
      for (const bx of [x + px * 6, x + L - px * 9]) o += shR(bx, top, px * 3, H, '#3C4048');
      return o + shLabel(x + L / 2, top, L * 0.55, label, '#F2E6C8', '#3A2410');
    }
    case 1: {  // REPAIRS: a long toolbox, handle on top, latches
      o += shBody(x, top + px * 2, L, H - px * 2, '#2F6FA8', '#5A9AD2', '#1C4670');
      o += shR(x + L * 0.38, top - px, L * 0.24, px, '#3C4048') + shR(x + L * 0.38, top - px, px, px * 3, '#3C4048') + shR(x + L * 0.62 - px, top - px, px, px * 3, '#3C4048');
      for (const bx of [x + px * 4, x + L - px * 7]) o += shR(bx, top + px * 5, px * 3, px * 3, '#C8CED8');
      o += shR(x, top + px * 9, L, px, '#1C4670');
      return o + shLabel(x + L / 2, top + px, L * 0.5, label, '#F2E6C8', '#1C2A40');
    }
    case 2: {  // SURVEY: a probe on its side -- body, dish at one end, antennas at the other
      o += shBody(x + px * 10, top + px * 2, L - px * 22, H - px * 4, '#5A616E', '#8A92A0', '#3C4048');
      o += shR(x + L - px * 12, top, px * 4, H, '#C8CED8') + shR(x + L - px * 8, top + px * 2, px * 3, H - px * 4, '#C8CED8') + shR(x + L - px * 5, top + px * 5, px * 3, px * 3, '#8FE3FF');
      o += shR(x, top + px * 6, px * 10, px, '#8A92A0') + shR(x + px * 2, top + px * 2, px * 8, px, '#8A92A0') + shR(x + px * 2, top + px * 10, px * 8, px, '#8A92A0');
      o += shR(x + px * 14, top + px * 3, px * 2, px * 2, '#FF6B5A');
      return o + shLabel(x + L * 0.47, top + px * 0.5, L * 0.44, label, '#0E1A28', '#8FE3FF');
    }
    case 3: {  // BLESSING: an old scroll on a rod, knobs at both ends
      o += shR(x, top + px * 5, L, px * 3, '#6F4828');
      o += shBody(x + px * 5, top + px, L - px * 10, H - px * 2, '#E8D9A8', '#F6ECCB', '#BFA874');
      for (const bx of [x + px * 5, x + L - px * 8]) o += shR(bx, top, px * 3, H, '#BFA874');
      o += shR(x - px, top + px * 4, px * 3, px * 5, '#B48A3C') + shR(x + L - px * 2, top + px * 4, px * 3, px * 5, '#B48A3C');
      return o + shLabel(x + L / 2, top, L * 0.52, label, '#E8D9A8', '#5A2A10');
    }
    case 5: {  // SMUGGLED SHIP: a ship model on a stand
      o += shR(x + L * 0.4, top + px * 9, L * 0.2, px * 2, '#3C4048') + shR(x + L * 0.47, top + px * 6, L * 0.06, px * 3, '#3C4048') + shR(x + L * 0.3, top + px * 11, L * 0.4, px * 2, '#5A616E');
      o += shBody(x + px * 4, top + px, L - px * 16, px * 5, '#B8C0CC', '#E6ECF2', '#7A828E');
      for (let k = 0; k < 4; k++) o += shR(x + L - px * 12 + k * px * 2, top + px + k * px * 0.5, px * 2, px * 5 - k * px, '#B8C0CC');
      o += shR(x + L * 0.62, top, px * 6, px * 2, '#2E9CBC') + shR(x, top + px * 2, px * 4, px * 3, '#FF8C42');
      return o + shLabel(x + L * 0.4, top - px * 3.5, L * 0.56, label, '#F2E6C8', '#24160A');
    }
    case 6: {  // AUDIENCE: a rolled banner, gold tassels
      o += shBody(x + px * 4, top + px * 2, L - px * 8, H - px * 4, '#7A2E8A', '#A85AB8', '#4E1A5A');
      for (let k = 0; k < 6; k++) o += shR(x + px * 8 + k * (L - px * 16) / 6, top + px * 2, px, H - px * 4, '#4E1A5A');
      o += shR(x, top + px * 4, px * 4, px * 5, '#FFD166') + shR(x + L - px * 4, top + px * 4, px * 4, px * 5, '#FFD166');
      o += shR(x + px, top + px * 9, px * 2, px * 4, '#FFD166') + shR(x + L - px * 3, top + px * 9, px * 2, px * 4, '#FFD166');
      return o + shLabel(x + L / 2, top, L * 0.5, label, '#FFD166', '#3A0E44');
    }
    case 8: {  // SAFE HARBOR: a shield emitter bar, glowing cells
      o += shBody(x, top + px * 3, L, H - px * 5, '#3C4048', '#6A707C', '#24282E');
      for (let k = 0; k < 7; k++) o += shR(x + px * 4 + k * (L - px * 8) / 7, top + px * 5, (L - px * 8) / 7 - px * 2, px * 2, '#5FD4FF', 0.85);
      o += shR(x - px, top + px * 2, px * 3, H - px * 3, '#8A92A0') + shR(x + L - px * 2, top + px * 2, px * 3, H - px * 3, '#8A92A0');
      return o + shLabel(x + L / 2, top + px * 2.5, L * 0.56, label, '#0E2A38', '#8FE3FF');
    }
    case 10: {  // FORGED PERMITS: a stack of datapads, offset
      for (let k = 0; k < 4; k++) {
        const ty = top + px * 9 - k * px * 3, ox = x + (k % 2) * px * 4;
        o += shR(ox, ty, L - px * 4, px * 3, '#2A2E38') + shR(ox + px * 2, ty, L - px * 10, px, k === 3 ? '#8CE7A0' : '#3A5A48');
      }
      return o + shLabel(x + L / 2, top - px * 3, L * 0.62, label, '#F2E6C8', '#1C2A20');
    }
    case 11: {  // GARRISON TROOPS: rifles lying stacked on a wooden rack
      o += shR(x + px * 6, top + px * 2, px * 3, H - px * 2, '#6F4828') + shR(x + L - px * 9, top + px * 2, px * 3, H - px * 2, '#6F4828');
      for (let k = 0; k < 3; k++) {
        const ty = top + px * 2 + k * px * 4;
        o += shR(x + px * 2, ty, L - px * 12, px * 2, '#3C4048') + shR(x + L - px * 14, ty, px * 10, px * 3, '#5A3A20') + shR(x + px * 16, ty + px * 2, px * 2, px * 2, '#3C4048');
      }
      return o + shLabel(x + L * 0.45, top - px * 3, L * 0.62, label, '#F2E6C8', '#24160A');
    }
  }
  return shBody(x, top, L, H, '#8B5E34', '#B07A44', '#5A3A20') + shLabel(x + L / 2, top, L * 0.55, label, '#F2E6C8', '#3A2410');
}
const shelfCache = {};
function shelfSvg(goodId, label, stock, seed) {
  const key = goodId + '|' + label + '|' + stock + '|' + seed;
  if (shelfCache[key]) return shelfCache[key];
  const sold = stock <= 0;
  let o = '<svg class="shelfsvg" viewBox="0 0 '+SH_W+' 112" shape-rendering="crispEdges" aria-hidden="true">';
  if (!sold) o += shGood(goodId, 12, SH_PY, SH_W - 24, label);
  else o += '<text x="'+(SH_W / 2)+'" y="'+(SH_PY - 12)+'" font-size="14" font-weight="700" letter-spacing="2" text-anchor="middle" fill="#7A6046">'+label+'  -  GONE</text>';
  o += shPlank(0, SH_PY, SH_W, seed, sold);
  for (let k = 0; k < 2; k++) o += shR(10 + k * 16, SH_PY + 5, 11, 5, k < stock ? '#8CE7A0' : '#24160C');  // stock lamps
  return (shelfCache[key] = o + '</svg>');
}
// 2.0 THE ASHEN FLEET: its screen (from the middle of the map or ATTACK) and the strike.
let ashenMode = false, ashenMask = -1, ashenResult = '';
function openAshen() { ashenMode = true; ashenMask = -1; ashenResult = ''; attackChoice = false; shownHtml = null; poll(); }
function closeAshen() { ashenMode = false; ashenResult = ''; shownHtml = null; poll(); }
function ashenToggle(f) { ashenMask ^= (1 << f); shownHtml = null; if (lastS) render(lastS); }
async function ashenStrike() {
  const r = await post('/ashen_strike?id='+encodeURIComponent(myId)+'&fleets='+ashenMask);
  ashenMask = -1;
  if (r && r.ok) {
    const s = await api('/state?id=' + encodeURIComponent(myId));
    if (s && s.v2 && s.v2.ashen) {
      const a = s.v2.ashen, lost = a.hits === 0 ? 'a miss.' : a.hits === 1 ? '1 ship lost.' : '2 ships lost.';
      ashenResult = a.alive ? a.ships+' ships hit it for '+a.dmg+'. It answered with a '+a.roll+': '+lost : a.ships+' ships hit it for '+a.dmg+'. THE ASHEN FLEET IS DESTROYED.';
    }
  } else ashenResult = (r && r.why) || 'Couldn\x27t strike.';
  shownHtml = null;
  poll();
}
let relaySeq = -1, noteSeq = -1, ovlTimer = null;
// 3.1: public table moments and private notes are now notifications (top layer).
function v2Overlay(table, head, sub, ms) {
  notify(table ? 'LOOK AT THE TABLE' + (head ? ' · ' + head : '') : (head || 'NOTICE'), sub, table ? '#FFC933' : '#5FD4FF', ms);
}
function v2Watch(s) {
  const v = s.v2;
  if (!v) return;
  let shown = false;
  if (relaySeq < 0) relaySeq = v.pubSeq;
  else if (v.pubSeq !== relaySeq) { relaySeq = v.pubSeq; if (s.stage === 1 && v.pubHead) { v2Overlay(true, v.pubHead, v.pubSub, 6000); shown = true; } }
  if (v.noteSeq === undefined) return;
  if (noteSeq < 0) noteSeq = v.noteSeq;
  else if (v.noteSeq !== noteSeq) { noteSeq = v.noteSeq; }  // 3.1-dev.3: notices come through the Feed now
}
// ---- 3.1-dev.3: the FEED ----
// The table keeps an event log; each poll asks only for what this phone hasn't
// seen (eg = the game's epoch, es / em = the last public / private event).
// INBOX = your own events (newest first); HISTORY = every public move by round.
const FEED = { g: -1, ps: 0, ms: 0, pub: [], mine: [], primed: false, read: 0 };
let feedMode = false, feedTab = 'inbox', feedReadBefore = 0;
function feedQuery() { return FEED.g < 0 ? '' : '&eg=' + FEED.g + '&es=' + FEED.ps + '&em=' + FEED.ms; }
function feedReadKey() { return 'sf_read_' + FEED.g; }
function feedMerge(s) {
  const ev = s.ev;
  if (!ev) return;
  if (ev.g !== FEED.g) {
    FEED.g = ev.g; FEED.pub = []; FEED.mine = []; FEED.ps = 0; FEED.ms = 0; FEED.primed = false;
    try { FEED.read = +(localStorage.getItem(feedReadKey()) || 0); } catch (e) { FEED.read = 0; }
  }
  const mk = a => ({ seq: a[0], kind: a[1], round: a[2], who: a[3], planet: a[4], other: a[5], num: a[6], text: a[7] || '' });
  const fresh = [];
  for (const a of ev.p) if (!FEED.pub.some(x => x.seq === a[0])) { const e = mk(a); FEED.pub.push(e); if (e.kind === 9) fresh.push(e); }  // (news: annCollect)
  for (const a of ev.m) if (!FEED.mine.some(x => x.seq === a[0])) { const e = mk(a); FEED.mine.push(e); fresh.push(e); }
  if (FEED.pub.length > 200) FEED.pub.splice(0, FEED.pub.length - 200);
  if (FEED.mine.length > 60) FEED.mine.splice(0, FEED.mine.length - 60);
  FEED.ps = Math.max(FEED.ps, ev.ps); FEED.ms = Math.max(FEED.ms, ev.ms);
  if (s.stage === 1 && joined) annCollect(s);  // 3.1-dev.10: Galaxy News, full screen (3.1-dev.54: not for spectators)
  if (FEED.primed && s.stage === 1) for (const e of fresh) {
    if (e.kind === 12 && !NOTIFY_ONLY.includes(e.num)) continue;  // shown full screen instead
    if (e.kind === 10 && e.num <= 2) continue;  // 3.1-dev.75: a gift arrives on its transmission (a gold chip), not as a pop-up
    const t = feedText(s, e, true);
    if (t) notify(t.head, t.line, t.col, t.stamp ? 4000 : 6000, t.stamp);
  }
  FEED.primed = true;
}
function feedUnread() { return FEED.mine.filter(e => e.seq > FEED.read).length; }
const FNAMES = ['Rebels', 'Empire', 'Pykes', 'Crimson Dawn'];
// 3.1-dev.75: a stamp's source as a small icon -- your class (a star), a faction's ships / gift (its symbol), a world (its planet).
function planetSpriteUrl(i) { if (!galaxyImgs[i]) galaxyImgs[i] = bPlanetPixels(i, i * 37).toDataURL(); return galaxyImgs[i]; }
const STAMP_FROM = ['c', 'c', 'c', 'c', 'c', 'c', 'c', 'c', 'f1', 'f0', 'f1', 'f0', 'f2', 'f3', 'w', 'w', 'w', 'w', '', 'b', 'b', 'b', 'w'];
function stampSrc(s, e) {
  const k = STAMP_FROM[e.num] || '';
  if (k === 'c') return '<span class="stSrc">' + '<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="#FFB347" stroke-width="2.4" aria-hidden="true"><path d="M12 3l2.6 5.6 6 .7-4.5 4.1 1.2 6L12 16.5 6.7 19.4l1.2-6L3.4 9.3l6-.7z"/></svg></span>';
  if (k[0] === 'f') return '<span class="stSrc">' + insigniaSvg(+k[1], 14) + '</span>';
  if (k === 'b') return '<span class="stSrc bmS"><img src="' + brokerURL('s', 1, false) + '" alt=""></span>';  // 3.1-dev.77 a black-market lot
  if (k === 'w' && e.planet >= 0) { const u = planetSpriteUrl(e.planet); return '<span class="stSrc">' + (u ? '<img src="' + u + '" alt="">' : '<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="#5FD4FF" stroke-width="2.4" aria-hidden="true"><circle cx="12" cy="12" r="7"/><path d="M2 14c6-3 14-5 20-6"/></svg>') + '</span>'; }
  return '';
}
function feedTitle(t) { return String(t).toLowerCase().replace(/(^| )[a-z]/g, c => c.toUpperCase()); }
function whoName(s, i) {  // the plain name (3.1-dev.17: the character's name; two players are never told apart by faction)
  if (i === mySlot) return 'You';
  const p = s && s.players && s.players[i];
  if (p && p.commander >= 0 && ART && ART.c && ART.c[p.commander]) return feedTitle(ART.c[p.commander].n);
  return p && p.name ? feedTitle(p.name) : 'Someone';
}
// 3.1-dev.52 PLAYER TAGS: every mention of a PLAYER is a tag -- their face in a ring of their colour + the name in their colour
// (YOU for yourself). Factions (NPCs) stay plain words, so a face always means a person at the table.
function pTag(s, i, up) {
  const p = s && s.players && s.players[i];
  if (!p || i < 0) return up ? 'SOMEONE' : 'Someone';
  const nm = i === mySlot ? 'YOU' : (up ? whoName(s, i).toUpperCase() : whoName(s, i));
  const col = p.chosenFaction >= 0 ? FACTION_UI_COLORS[p.chosenFaction] : '#8FA3B8';
  const face = p.commander >= 0 ? portraitURL(p.commander, 1) : '';
  return '<span class="ptag" style="--pc:' + col + '">' + (face ? '<img class="pixel" src="' + face + '" alt="">' : '') + '<b>' + nm + '</b></span>';
}
function feedWho(s, i) { return pTag(s, i); }
function feedWorld(s, p) { return s.planets && s.planets[p] ? feedTitle(s.planets[p].name) : 'a world'; }
const MOMENT_HEAD = ['FIRST BLOOD', 'HOME WORLD', 'THE CORE FALLS', '', 'BETRAYAL', '', 'FLEET DESTROYED', 'FINAL ROUND'];
// One event -> {head, line, col}.
function feedText(s, e, mine) {
  const W = feedWorld(s, e.planet), how = ['', ' by force', ' through talks', ' with Credits'][e.num] || '';
  switch (e.kind) {
    case 2: return { head: e.who === mySlot ? 'YOU TOOK ' + W.toUpperCase() : 'WORLD TAKEN', line: feedWho(s, e.who) + ' took ' + W + how, col: e.who === mySlot ? '#FFD166' : '#C8D2DC' };
    case 3: return { head: 'WORLD RETURNED', line: W + ' went back to its faction', col: '#8FA3B8' };
    case 4: {
      const m = e.num, them = e.other >= 0 && e.other < 4 ? FNAMES[e.other] : '';
      const seatOf = f => s.players.findIndex(p => p.active && p.chosenFaction === f);  // 3.1-dev.52: a moment's actor is a PLAYER -- tag them
      const who = e.who >= 0 && e.who < 4 && seatOf(e.who) >= 0 ? pTag(s, seatOf(e.who)) : e.who >= 0 && e.who < 4 ? FNAMES[e.who] : 'Someone';
      const theirs = e.other >= 0 && e.other < 4 && seatOf(e.other) >= 0 ? pTag(s, seatOf(e.other)) + '\u2019s' : 'a ' + them;
      if (m === 40) return { head: 'ASHEN FLEET DESTROYED', line: who + ' struck the last blow', col: '#FFC933' };
      if (m >= 20) return { head: 'GALAXY EVENT', line: 'The galaxy shifted', col: '#5FD4FF' };
      const head = MOMENT_HEAD[m] || 'BIG MOMENT';
      const line = m === 7 ? 'Everything comes down to this' : m === 4 ? who + ' turned on the ' + them : m === 6 ? who + ' destroyed ' + theirs + ' fleet' : who + ' took ' + W;
      return { head: head, line: line, col: '#FFC933' };
    }
    case 5: return e.who >= 0 ? { head: 'YOU LOST ' + W.toUpperCase(), line: feedWho(s, e.who) + ' took it' + how, col: '#FF6B5A' }
                              : { head: 'YOU LOST ' + W.toUpperCase(), line: 'It went back to its faction', col: '#FF6B5A' };
    case 6: return { head: 'NOTICE', line: e.text, col: '#5FD4FF' };
    case 8: {  // 3.1-dev.7 a class strength / weakness fired
      const ST = [['WARLORD +1', 'An extra ship joins your attack.', '#8CE7A0'], ['THE MUSCLE', 'Their enemies loved it: taking ' + W + ' by force won you extra favor with its rival.', '#8CE7A0'],
                  ['CONSUL −1 QUESTION', 'Silver tongue: one fewer question at ' + W + '.', '#8CE7A0'], ['NO STOMACH FOR WAR', 'Taking ' + W + ' by force cost you extra favor with its faction.', '#FF6B5A'],
                  ['FREE LOOK', 'Eyes everywhere: your first intel this round was free.', '#8CE7A0'], ['SPREAD THIN', 'Your world ' + W + ' defends with one less.', '#FF6B5A'],
                  ['MARK-UP +2', 'Nobody trusts a scoundrel at the front counter: ' + W + ' cost you 2 more.', '#FF6B5A'], [myClass(s) === 0 ? 'WARLORD +1 QUESTION' : 'SCOUNDREL +1 QUESTION', (myClass(s) === 0 ? 'Blunt: a warlord gets grilled.' : 'Nobody trusts a scoundrel:') + ' One more question at ' + W + '.', '#FF6B5A'],
                  ['SHIP OF THE LINE', 'An Imperial ship fights at your side.', '#8CE7A0'], ['SAFE WORLD', 'Loyal Rebels stand with ' + W + ': +2 ships.', '#8CE7A0'],
                  ['IMPERIAL RANKS', 'Your 7th ship: Imperial fleets hold seven.', '#8CE7A0'], ['ESCAPED', 'Slippery: one ship got away from the defeat.', '#8CE7A0'],
                  ['SHRUGGED', 'Armored cargo: the first hit bounced off.', '#8CE7A0'], ['FIRST SHOT', 'Ambushers: the Dawn fired first and won the tie.', '#8CE7A0'],
                  ['REROLL', 'Ancient power (or a blessing): you rolled that one again.', '#8CE7A0'], ['MEDICAL BAY', W + '\u2019s healers repaired one of your ships.', '#8CE7A0'],
                  ['THE COURT', W + '\u2019s court speaks for you: favor up with its faction.', '#8CE7A0'], ['TRAINING', 'Your new fleet got a trained crew: +1 ship free.', '#8CE7A0'],
                  ['SHAKEN', 'Your fleet ran. Move it back to your nearest world or a space station: it can\u2019t attack next round.', '#FFB347'],
                  ['SABOTEUR', 'Your saboteur got there first: ' + W + ' lost 2 defenders before the first roll.', '#8CE7A0'],
                  ['TURNCOAT', 'One of ' + W + '\u2019s defenders fights for you now.', '#8CE7A0'],
                  ['SECOND CHANCE', 'Down to two ships, and two more jumped in.', '#8CE7A0'],
                  ['BOUNTY +3 CR', W + ' was a Scoundrel\u2019s world: the price on their head is yours.', '#FFD166'],
                  ['THE MARK +5 CR', W + ' belonged to the Mark: the Dawn pays.', '#E0475B'],  // 3.1-dev.88
                  ['HIRED BLADE', 'Your hired blade joined the fight: +1 ship.', '#8CE7A0'],
                  ['DECOY BEACON', 'Your decoy drew their fire at ' + W + ': +1 defender.', '#8CE7A0'],
                  ['CLEAN GETAWAY', 'No parting shot, and your fleet isn\u2019t shaken.', '#8CE7A0'],
                  ['WARPATH +3', 'Your war came with you: 3 more ships at ' + W + '.', '#FF6B5A'],
                  ['WAR SPOILS +3', (s.players[mySlot] && s.players[mySlot].chosenFaction >= 0 ? 'The ' + FNAMES[s.players[mySlot].chosenFaction] : 'Your people') + ' cheer: you took ' + W + ' by force.', '#8CE7A0'],
                  ['RALLY +1', 'Three hits in a row and your crews dug in: +1 on this roll.', '#8CE7A0'],
                  ['ON ALERT', 'The talks failed, so ' + W + ' saw you coming: +1 defender.', '#FFB347'],
                  ['TREATY', W + ' signed with you: +2 defenders, and through next round no bribes or buyouts and +1 CR.', '#8CE7A0'],
                  ['DUG IN +2', W + ' was taken this round: its new holder dug in with 2 more defenders.', '#FFB347'],
                  ['STAND BY +2', 'Your crews spent last round getting ready: 2 more ships in this attack.', '#8CE7A0'],
                  ['HIRED GUNS', 'Your hired ships fly in front and take the first hits.', '#8CE7A0']][e.num];
      if (ST && ST[0] === 'REROLL' && e.other >= 0) {  // 3.1-dev.39 (playtest: "doesn't always seem like the effect happens"): show both rolls
        const a = Math.floor(e.other / 10), b = e.other % 10;
        return { head: 'ANCIENT BLESSING', line: 'You lost that roll, so you rolled again: ' + a + ' → ' + b + (b > a ? '.' : ' — no luck this time.') + ' (Once per battle.)', col: '#8CE7A0', stamp: true };
      }
      return ST ? { head: stampSrc(s, e) + ST[0], line: ST[1], col: ST[2], stamp: true } : null;
    }
    case 11: case 12: { const t = newsText(s, e, e.kind === 12); return t ? { head: t.head, line: t.line, col: t.col } : null; }
    case 10: {  // 3.1-dev.8 favor gifts
      const f = e.who >= 0 && e.who < 4 ? e.who : 0, worlds = [e.planet, e.other].filter(p => p >= 0).map(p => feedWorld(s, p)).join(' and ');
      if (e.num === 0) return { head: 'GIFT \u00b7 ' + GIFT4[f][0].toUpperCase(), line: 'You\u2019re ' + favorWord(f, 3) + ' to the ' + FNAMES[f] + '. ' + GIFT4[f][1] + (worlds ? ' (' + worlds + ')' : ''), col: '#8CE7A0' };
      if (e.num === 1) return { head: favorWord(f, 4).toUpperCase(), line: GIFT5[f][0] + ': ' + GIFT5[f][1] + ' Yours while you stay here.', col: '#FFD166' };
      if (e.num === 2) return { head: 'GIFT LOST \u00b7 ' + GIFT5[f][0].toUpperCase(), line: 'You fell from ' + favorWord(f, 4) + ' with the ' + FNAMES[f] + '.', col: '#FF6B5A' };
      if (e.num === 3) return { head: 'YOUR SPY REPORTS', line: 'This round you\u2019re watching ' + feedWho(s, e.other) + '. See Home.', col: '#C8364A' };
      return null;
    }
    case 9: return { head: e.who === mySlot ? 'QUEST COMPLETE · +5 VP' : 'A QUEST IS DONE', line: feedWho(s, e.who) + ' finished ' + (e.who === mySlot ? 'your' : 'their') + ' quest.', col: '#FFD166' };
    case 7: return { head: e.num ? 'PROMISE KEPT' : 'PROMISE BROKEN', line: (e.who >= 0 && e.who < 4 ? 'The ' + FNAMES[e.who] : 'They') + (e.num ? ' remember it.' : ' remember that too.'), col: e.num ? '#8CE7A0' : '#FF6B5A' };
  }
  return null;
}
// 3.1-dev.37 (playtest): tapping an Inbox item shows it again as its full original popup.
function feedReplay(seq) { const e = FEED.mine.find(x => x.seq === seq); if (e && lastS) annOpen(lastS, e, true); }
function openFeed(tab) { feedMode = true; feedTab = tab || 'inbox'; feedReadBefore = FEED.read; feedMarkRead(); if (lastS) render(lastS); }
function closeFeed() { feedMode = false; if (lastS) render(lastS); }
function feedSetTab(t) { feedTab = t; if (t === 'history') loadBattles(); if (lastS) render(lastS); }
// 3.1-dev.80 BATTLE REPLAY: fetched when History opens, then played exchange by exchange.
let BATTLES = [];
async function loadBattles() { const b = await api('/battles'); if (Array.isArray(b)) { BATTLES = b; shownHtml = null; if (lastS && feedMode) render(lastS); } }
let replayT = [];
function openReplay(k) {
  const s = lastS, b = BATTLES[k]; if (!s || !b) return;
  replayT.forEach(clearTimeout); replayT = [];
  const vs = b.k === 2 ? pTag(s, b.d) : (s.planets[b.p] ? s.planets[b.p].name : 'A WORLD');
  sheetShow('replay', '<div class="sheetSub" style="letter-spacing:3px">ROUND ' + b.r + ' \u00b7 ' + (b.k === 2 ? 'FLEET BATTLE' : 'ATTACK') + '</div><div class="sheetTitle">' + pTag(s, b.a, true) + ' vs ' + vs + '</div>' +
    '<div class="warScene rpl"><div><small>ATTACK</small><div id="rpA">' + pnumSvg(b.a0, '#E8EEF5', 4) + '</div></div><div class="wsDice"><div id="rpDa" class="die">' + pnumSvg(0, '#05070C', 3) + '</div><div id="rpDd" class="die">' + pnumSvg(0, '#05070C', 3) + '</div></div>' +
    '<div><small>' + (b.k === 2 ? 'DEFENSE' : 'DEFENDERS') + '</small><div id="rpD">' + pnumSvg(b.d0, '#E8EEF5', 4) + '</div></div></div>' +
    '<div class="rpLine" id="rpLine">&nbsp;</div><div class="rpSteps" id="rpSteps"></div>' +
    '<div class="biAct"><button onclick="openReplay(' + k + ')">REPLAY</button><button class="ghost" onclick="replayT.forEach(clearTimeout);sheetHide()">CLOSE</button></div>', '#FF6B5A');
  let a = b.a0, d = b.d0;
  const el = id => document.getElementById(id), last = b.log.length - 1;
  b.log.forEach((x, i) => replayT.push(setTimeout(() => {
    const ar = x >> 4, dr = x & 15, parting = b.res === 3 && i === last;
    let line = 'Roll ' + (i + 1) + ': ' + ar + ' vs ' + dr + ' \u00b7 ';
    if (parting) { const hit = b.rs === 1 ? dr > ar : ar > dr; if (hit) { if (b.rs === 1) a--; else d--; } line += 'a parting shot as they withdrew' + (hit ? ', and it hit' : ', and it missed'); }
    else if (ar > dr) { d--; line += 'a defender down'; } else if (dr > ar) { a--; line += 'an attacker down'; } else line += 'a tie, nothing happens';
    if (el('rpDa')) { el('rpDa').innerHTML = pnumSvg(ar, '#05070C', 3); el('rpDa').classList.toggle('win', ar > dr); }
    if (el('rpDd')) { el('rpDd').innerHTML = pnumSvg(dr, '#05070C', 3); el('rpDd').classList.toggle('win', dr > ar); }
    if (el('rpA')) el('rpA').innerHTML = pnumSvg(Math.max(0, a), a > 0 ? '#E8EEF5' : '#FF6B5A', 4);
    if (el('rpD')) el('rpD').innerHTML = pnumSvg(Math.max(0, d), d > 0 ? '#E8EEF5' : '#FF6B5A', 4);
    if (el('rpLine')) el('rpLine').textContent = line;
    if (el('rpSteps')) el('rpSteps').insertAdjacentHTML('beforeend', '<i class="' + (ar > dr ? 'a' : dr > ar ? 'd' : '') + '"></i>');
    if (i === last && el('rpLine')) setTimeout(() => { const L = el('rpLine'); if (L) L.textContent = b.res === 3 ? 'They withdrew.' : b.res === 1 ? (b.k === 2 ? 'The attacker won.' : 'Taken.') : (b.k === 2 ? 'The defender won.' : 'It held.'); }, 900);
  }, 500 + i * 650)));
}
function feedMarkRead() { FEED.read = FEED.ms; try { localStorage.setItem(feedReadKey(), String(FEED.read)); } catch (e) {} }
function feedHtml(s) {
  const n = feedUnread();
  let h = '<div class="feedTop"><button class="feedBack" onclick="closeFeed()">\u2039</button><div class="feedTitle">FEED</div><span style="width:44px"></span></div>';
  h += '<div class="feedTabs"><button class="' + (feedTab === 'inbox' ? 'on' : '') + '" onclick="feedSetTab(\x27inbox\x27)">INBOX' + (n ? ' \u00b7 ' + n : '') + '</button>' +
       '<button class="' + (feedTab === 'history' ? 'on' : '') + '" onclick="feedSetTab(\x27history\x27)">HISTORY</button></div>';
  if (feedTab === 'inbox') {
    if (s.battleActive && s.battleAttackerSlot !== mySlot && s.battleDefenderSlot !== mySlot) {  // 3.1-dev.74 (user: tap to watch "should go into notifications so you can always come back")
      const pvp = s.battleKind === 2, P = s.planets[s.battleTargetPlanet];
      h += '<div class="feedRow tap unread" style="--rc:#FF6B5A" onclick="watchBattle=battleKeyOf(lastS);closeFeed()"><div class="frHead">BATTLE NOW \u00b7 ' + pTag(s, s.battleAttackerSlot, true) + ' ATTACKS ' + (pvp ? pTag(s, s.battleDefenderSlot, true) : (P ? P.name : 'A WORLD')) + '</div><div class="frLine">Tap to watch it live.</div><div class="frRound">ROUND ' + s.round + '</div></div>';
    }
    const list = FEED.mine.slice().reverse();
    if (!list.length) h += '<div class="feedEmpty">Nothing yet. What happens to you shows up here.</div>';
    for (const e of list) {
      const t = feedText(s, e, true); if (!t) continue;
      const openJob = e.kind === 12 && e.num === 2 && s.v2 && s.v2.job && s.v2.job.st === 1 && s.v2.job.w === e.planet;  // 3.1-dev.16
      h += '<div class="feedRow' + (e.seq > feedReadBefore ? ' unread' : '') + (openJob ? ' tap' : '') + '" style="--rc:' + t.col + '"' + (openJob ? ' onclick="openJobSheet()"' : ' onclick="feedReplay(' + e.seq + ')"') + '><div class="frHead">' + t.head + '' + '</div><div class="frLine">' + t.line + '</div><div class="frRound">ROUND ' + e.round + '</div></div>';
    }
  } else {
    if (BATTLES.length) h += '<div class="feedRoundHead">BATTLES \u00b7 TAP TO REPLAY</div>' + BATTLES.map((b, k) => {
      const vs = b.k === 2 ? pTag(s, b.d) : (s.planets[b.p] ? s.planets[b.p].name : 'a world'), res = b.res === 3 ? 'WITHDREW' : b.res === 1 ? (b.k === 2 ? 'WON' : 'TAKEN') : (b.k === 2 ? 'LOST' : 'HELD');
      return '<button class="feedHist bRep" onclick="openReplay(' + k + ')"><b style="color:' + (b.res === 1 ? '#FF6B5A' : '#8FA3B8') + '">R' + b.r + ' \u00b7 ' + res + '</b><span>' + pTag(s, b.a) + ' vs ' + vs + ' \u00b7 ' + b.a0 + ' vs ' + b.d0 + ' \u00b7 ' + b.log.length + ' roll' + (b.log.length === 1 ? '' : 's') + '</span><em>\u25B6</em></button>';
    }).join('');
    // 3.1-dev.16: your own secret moves (jobs answered, cover, a heist) sit among the public ones, in gold -- only you see them
    const own = FEED.mine.filter(e => e.kind === 12 && [21, 23, 24, 25, 26, 27].includes(e.num)).map(e => Object.assign({ own: true }, e));
    const list = FEED.pub.filter(e => e.kind !== 1).concat(own).sort((a, b) => a.round - b.round || (a.own === b.own ? a.seq - b.seq : a.own ? 1 : -1)).reverse();
    if (!list.length) h += '<div class="feedEmpty">No moves yet this game.</div>';
    let r = -1;
    for (const e of list) {
      if (e.round !== r) { r = e.round; h += '<div class="feedRoundHead">ROUND ' + r + '</div>'; }
      const t = feedText(s, e, !!e.own); if (!t) continue;
      const mineRow = (e.kind === 2 && e.who === mySlot) || e.own;
      h += '<div class="feedHist' + (mineRow ? ' you' : '') + '"><b style="color:' + t.col + '">' + t.head + '</b><span>' + t.line + '</span></div>';
    }
  }
  return h;
}
// ---- 3.1-dev.3: pixel icons (12x12, original, style guide S2) ----
const PIX = {
  bell: ['.....##.....','....####....','...#....#...','..#......#..','..#......#..','..#......#..','..#......#..','.#........#.','############','............','.....##.....','............'],
  t1: ['............','............','...######...','..#......#..','.#..####..#.','#..#.##.#..#','#..#.##.#..#','.#..####..#.','..#......#..','...######...','............','............'],
  t2: ['.....##.....','.....##.....','....#..#....','....#..#....','...#.##.#...','...#.##.#...','..#..##..#..','..#..##..#..','.#........#.','.#...##...#.','#..........#','############'],
  t3: ['....####....','..##....##..','.#........#.','.#........#.','#..........#','#..........#','#..........#','#..........#','.#........#.','.#........#.','..##....##..','....####....'],
  t4: ['............','............','............','.####..####.','#....##....#','#..##..##..#','#..##..##..#','#....##....#','.####..####.','............','............','............'],
  t5: ['.....##.....','.....##.....','....####....','############','.##########.','..########..','...######...','..###..###..','..##....##..','.##......##.','.#........#.','............'],
  crown: ['............','#....##....#','##..####..##','###.####.###','############','############','.##########.','.##########.','.#.#.##.#.#.','.##########.','............','............'],
  target: ['....####....','..##....##..','.#..####..#.','.#.#....#.#.','#.#..##..#.#','#.#.####.#.#','#.#.####.#.#','#.#..##..#.#','.#.#....#.#.','.#..####..#.','..##....##..','....####....'],
  war: ['#..........#','.#........#.','..#......#..','...#....#...','....#..#....','.....##.....','.....##.....','....#..#....','.##......##.','.##......##.','#..........#','............'],
  summit: ['.##########.','#..........#','#....##....#','#....##....#','#..######..#','#....##....#','.#...##...#.','.#........#.','..#......#..','...#....#...','....#..#....','.....##.....'],
  job: ['............','....####....','....#..#....','############','#..........#','#..........#','#####..#####','#....##....#','#..........#','#..........#','############','............'],
  commotion: ['....#####...','..##.....#..','.#...###..#.','#...#...#..#','#..#..#..#.#','#..#.#.#.#.#','#..#..##.#.#','#...#....#.#','.#...####..#','..#.......#.','...#######..','............'],
  race: ['#...........','#########...','#.#.#.#.#...','##.#.#.#.#..','#.#.#.#.#...','##.#.#.#.#..','#########...','#...........','#...........','#...........','#...........','##..........'],
  heist: ['############','#..........#','#.##....##.#','#.##....##.#','#..........#','#....##....#','#....##....#','#..........#','#.##....##.#','#.##....##.#','#..........#','############'],
  cover: ['............','............','############','#..........#','#.###..###.#','#.###..###.#','#..........#','.#...##...#.','..##....##..','....####....','............','............'],
  credits: ['...######...','..#......#..','.#..####..#.','#..#....#..#','#..#.......#','#..#.......#','#..#.......#','#..#....#..#','.#..####..#.','..#......#..','...######...','............'],
  victory: ['..##....##..','...##..##...','....####....','...#....#...','..#..##..#..','..#.####.#..','..#.####.#..','..#..##..#..','...#....#...','....####....','............','............'],
  gift: ['...##..##...','....#..#....','############','#....##....#','############','.#...##...#.','.#...##...#.','.#...##...#.','.#...##...#.','.#...##...#.','.##########.','............'],
  holonet: ['.#........#.','#..#....#..#','#.#..##..#.#','#.#.#..#.#.#','#..#.##.#..#','.#...##...#.','.....##.....','.....##.....','....#..#....','...#....#...','..########..','............'],
  owe: ['.##########.','#.#........#','#.#.######.#','.##........#','..#.######.#','..#........#','..#.######.#','..#........#','..#.####...#','..#.......##','..#########.','............'],
  fleet: ['.....##.....','....####....','....####....','...##..##...','...######...','..########..','.####..####.','############','##.##..##.##','#...#..#...#','............','............'],
  era: ['############','.#........#.','..#......#..','...#.##.#...','....####....','.....##.....','.....##.....','....#..#....','...#.##.#...','..#.####.#..','.#.######.#.','############'],
};
// ---- 3.1-dev.5: the pixel SCOREBOARD (5x7 numerals, original). Only the game's
// scoreboard numbers use it (Credits, VP, worlds, later garrisons and dice). When
// a value changes it COUNTS to the new one (about 400 ms) instead of jumping.
// The span is data-keep, so a poll's patch updates data-n but leaves the drawing
// to pnumTick(), which runs after every render.
const PDIG = {
  '0': ['.###.','#...#','#..##','#.#.#','##..#','#...#','.###.'], '1': ['..#..','.##..','..#..','..#..','..#..','..#..','.###.'],
  '2': ['.###.','#...#','....#','...#.','..#..','.#...','#####'], '3': ['####.','....#','....#','.###.','....#','....#','####.'],
  '4': ['...#.','..##.','.#.#.','#..#.','#####','...#.','...#.'], '5': ['#####','#....','####.','....#','....#','#...#','.###.'],
  '6': ['..##.','.#...','#....','####.','#...#','#...#','.###.'], '7': ['#####','....#','...#.','..#..','.#...','.#...','.#...'],
  '8': ['.###.','#...#','#...#','.###.','#...#','#...#','.###.'], '9': ['.###.','#...#','#...#','.####','....#','...#.','.##..'],
  '-': ['.....','.....','.....','.###.','.....','.....','.....'],
};
function pnumSvg(n, color, sc) {
  const t = String(n);
  let r = '', x = 0;
  for (const ch of t) {
    const g = PDIG[ch]; if (!g) continue;
    g.forEach((row, y) => { for (let i = 0; i < 5; i++) if (row[i] === '#') r += '<rect x="' + (x + i) + '" y="' + y + '" width="1" height="1"/>'; });
    x += 6;
  }
  const w = Math.max(1, x - 1);
  return '<svg class="pix" width="' + w * sc + '" height="' + 7 * sc + '" viewBox="0 0 ' + w + ' 7" shape-rendering="crispEdges"><g fill="' + color + '">' + r + '</g></svg>';
}
function pnum(n, color, sc) {
  return '<span class="pn" data-keep="1" data-n="' + n + '" data-c="' + (color || 'currentColor') + '" data-s="' + (sc || 4) + '">' + pnumSvg(n, color || 'currentColor', sc || 4) + '</span>';
}
function pnumTick() {
  document.querySelectorAll('.pn').forEach(el => {
    const to = +el.dataset.n, c = el.dataset.c, sc = +el.dataset.s;
    if (el._shown === undefined) { el._shown = to; return; }
    if (el._shown === to || el._anim === to) return;
    const from = el._shown, dur = reducedMotion() ? 1 : 420;
    let t0 = -1;  // the first frame's own timestamp (rAF time can be earlier than performance.now())
    el._anim = to;
    const step = (t) => {
      if (el._anim !== to) return;  // a newer value took over
      if (t0 < 0) t0 = t;
      const k = Math.max(0, Math.min(1, (t - t0) / dur)), e = 1 - Math.pow(1 - k, 3);
      const v = Math.round(from + (to - from) * e);
      el.innerHTML = pnumSvg(v, c, sc); el._shown = v;
      if (k < 1) requestAnimationFrame(step); else { el._anim = undefined; el.classList.remove('bump'); void el.offsetWidth; el.classList.add('bump'); }
    };
    requestAnimationFrame(step);
    // Safety: if animation frames stall (a backgrounded tab, a busy phone), land
    // on the real value anyway -- a number must never freeze halfway.
    setTimeout(() => { if (el._anim === to) { el._anim = undefined; el.innerHTML = pnumSvg(to, c, sc); el._shown = to; } }, dur + 150);
  });
}
function pixIcon(name, color, scale) {
  const rows = PIX[name], sc = scale || 2;
  let r = '';
  rows.forEach((row, y) => { for (let x = 0; x < row.length; x++) if (row[x] === '#') r += '<rect x="' + x + '" y="' + y + '" width="1" height="1"/>'; });
  return '<svg class="pix" width="' + 12 * sc + '" height="' + 12 * sc + '" viewBox="0 0 12 12" shape-rendering="crispEdges"><g fill="' + (color || 'currentColor') + '">' + r + '</g></svg>';
}
function bellHtml() {
  const n = feedUnread();
  return '<button class="bell" onclick="openFeed()" aria-label="Feed">' + pixIcon('bell', 'currentColor', 2) + (n ? '<span class="bellN">' + n + '</span>' : '') + '</button>';
}
function closeStandings() { standingsMode = false; poll(); }
// Everyone's row, best VP first: rank, insignia, faction, VP, planets; yours
// highlighted. Shared by Standings and Game Over (v0.25.1, twin of the
// AMOLED's uiStandingsRows).
// 2.0-dev.14 HOW TO PLAY (user 2026-11-03: "a primer ... would go along with a
// rulebook, but it would help get things associated with the pictures"). Picture
// cards built from the game's own art (planet pixels, insignia, the board map,
// ships, dice) so the pictures are the ones you'll meet in play. Phone only; no
// game state -- it reads lastS only to color things for your faction.
// 3.1-dev.58 THE NEW HOW TO PLAY (user 2026-11-14: "a new how to play deck - from scratch"; playtest 4: "it just feels like so
// much"). THE BASICS = one deck, 8 cards, one idea each (user: "just a deck, the way we have it"). RIVET'S GUIDES = short topic
// decks the droid narrates (user: "rivet can narrate any other guide"), from the end of the deck and from Standings.
let primerAt = -1, primerX = null, primerGuide = null;  // primerGuide: null = THE BASICS, '' = the guide index, else a GUIDES key
function primerDeck() { return primerGuide ? GUIDES[primerGuide].cards : PRIMER; }
function primerShow() { shownHtml = null; window.scrollTo(0, 0); if (lastS) render(lastS); }
function openPrimer() { primerGuide = null; primerAt = 0; primerShow(); }
function openGuides(k) { primerGuide = k || ''; primerAt = 0; primerShow(); }
function closePrimer() { primerAt = -1; primerGuide = null; lastSeatSnapshot = ''; lastFactionSnapshot = ''; shownHtml = null; window.scrollTo(0, 0); if (lastS) render(lastS); }
function primerGo(d) {
  if (primerGuide === '') return;
  const n = primerAt + d;
  if (n >= primerDeck().length) { if (primerGuide) openGuides(''); else closePrimer(); return; }
  primerAt = Math.max(0, n); primerShow();
}
function primerTS(e) { primerX = e.touches[0].clientX; }
function primerTE(e) {
  if (primerX === null) return;
  const dx = e.changedTouches[0].clientX - primerX; primerX = null;
  if (Math.abs(dx) > 60) primerGo(dx < 0 ? 1 : -1);
}
function primerFaction() { const me = lastS && mySlot >= 0 ? lastS.players[mySlot] : null; return me && me.chosenFaction >= 0 ? me.chosenFaction : -1; }
function primerImg(i, size) {
  if (!galaxyImgs[i]) galaxyImgs[i] = bPlanetPixels(i, i * 37).toDataURL();
  return '<img class="pixel" src="'+galaxyImgs[i]+'" width="'+size+'" height="'+size+'" alt="">';
}
function primerRow(icon, head, col, text) {
  return '<div class="prow"><div class="ic">'+icon+'</div><div><b style="color:'+col+'">'+head+'</b><span>'+text+'</span></div></div>';
}
const PRIMER_HOME = [12, 13, 6, 7];           // = HOME_PLANET in game_state.h
const PRIMER_PHASE_COL = ['#FFD166', '#5FD4FF', '#FF6B5A'];
const PRIMER = [
  { k:'THE GOAL', t:'WIN THE GALAXY',
    pic: () => '<div class="pworlds">' + [[0,'OUTER RIM',2],[2,'MID RIM',3],[4,'THE CORE',5]].map(w =>
           '<div>'+primerImg(w[0], 72)+'<b style="color:#FFD166">'+w[2]+' VP</b>'+w[1]+'</div>').join('') + '</div>',
    b: ['The game lasts <b>6, 8 or 10 rounds</b>. The most <b>Victory Points</b> at the end wins.',
        'A world is worth VP <b>while you hold it</b>. Lose it and the VP goes too. Home worlds are worth <b>4</b>.',
        'Also: finish your <b>quest</b> (+5) and <b>destroy a player\u2019s fleet</b> (+2).'] },
  { k:'WHO YOU ARE', t:'YOUR SIDE',
    pic: () => [[0,1],[2,3]].map(r => '<div class="pfight" style="margin:6px 0">' + r.map((f, j) =>
           '<div>'+insigniaSvg(f, 44)+'<span style="color:'+FACTION_UI_COLORS[f]+'">'+FACTION_SHORT_NAMES[f]+'</span></div>' + (j === 0 ? '<div style="color:var(--red)">RIVALS<br>\u27F7</div>' : '')).join('') + '</div>').join(''),
    b: ['You command one of four factions. Each has a <b>rival</b>.',
        'You\u2019re also one of <b>16 characters</b>. Tap your face, top left, for your card: what you\u2019re <b>great at</b>, what <b>hurts</b>, your <b>quest</b> and a <b>once-a-game move</b>.'] },
  { k:'ON THE TABLE', t:'THE BOARD',
    pic: () => {
      const f = primerFaction(), home = f >= 0 ? PRIMER_HOME[f] : -1;
      let h = '<div class="gmap">' + galaxyMapSvg(null);
      for (let i = 0; i < GALAXY_POS.length; i++) {
        const pos = GALAXY_POS[i], core = PRING_OF(i) === 2, hf = PRIMER_HOME.indexOf(i);
        const ring = hf >= 0 ? FACTION_RING_COLORS[hf] : 'var(--line)';
        h += '<span class="pw'+(core ? ' core' : '')+'" style="left:'+(50 + pos[0] / 4.66).toFixed(2)+'%;top:'+(50 + pos[1] / 4.66).toFixed(2)+'%;border-color:'+ring+
             (i === home ? ';border-width:4px;box-shadow:0 0 10px '+ring : '')+'">'+primerImg(i, 40)+'</span>';
      }
      return h + '</div>';
    },
    b: ['Your ships live on the <b>real board</b>. The <b>STAR MAP</b> on your phone is the same map.',
        primerRow('<svg width="30" height="18"><line x1="2" y1="9" x2="28" y2="9" stroke="#5FD4FF" stroke-width="3"/></svg>', 'RINGS + LANES', '#5FD4FF', 'the routes your ships travel') +
        primerRow('<svg width="24" height="24" viewBox="-12 -12 24 24"><circle r="9" fill="none" stroke="#B98CF0" stroke-width="2.5" stroke-dasharray="4 3"/><circle r="2.5" fill="#B98CF0"/></svg>', 'NEBULAE', '#B98CF0', 'stops on the Outer Rim, with no world') +
        primerRow('<svg width="30" height="18"><circle cx="15" cy="9" r="7" fill="none" stroke="#FFD166" stroke-width="3"/></svg>', 'THE CORE', '#FFD166', 'rich worlds, heavily guarded') +
        primerRow('<svg width="18" height="18" viewBox="0 0 18 18" fill="none" stroke="#5FD4FF" stroke-width="1.6"><circle cx="9" cy="9" r="7"/><circle cx="9" cy="9" r="3.5"/></svg>', 'SPACE STATIONS', '#5FD4FF', 'where you launch if you hold no worlds')] },
  { k:'EVERY ROUND', t:'THREE STEPS',
    pic: () => phaseTrackHtml({ round: 3, roundLimit: 8, phase: 1 }) +
           [['1','PREPARE','Spend: ships, scouting, shops.'], ['2','MOVEMENT','Move your ships on the board.'], ['3','RESOLUTION','Take worlds. Fight.']].map((p, i) =>
           primerRow('<div class="pdie" style="border-color:'+PRIMER_PHASE_COL[i]+';color:'+PRIMER_PHASE_COL[i]+';width:40px;height:40px;font-size:20px">'+p[0]+'</div>', p[1], PRIMER_PHASE_COL[i], p[2])).join(''),
    b: ['The bar at the top of your screen lights the step you\u2019re in. The <b>NOW</b> line says what to do.',
        'Done? Hold <b>LOCK IN</b> on the BRIDGE. The next step starts when everyone has.'] },
  { k:'STEP 1', t:'PREPARE',
    pic: () => {
      const col = FACTION_RING_COLORS[Math.max(0, primerFaction())];
      return '<div class="stats" style="margin:0 0 6px;grid-template-columns:1fr"><div class="stat"><span>CREDITS</span><b style="color:#FFD166">12</b></div></div>' +
             primerRow('<b style="font-size:20px">1</b>', 'FLEET 1', col, fcShips(3, col, false) + ' +1 ship = 2 CR') +
             '<div class="intel">' + [['FROM YOUR NEIGHBOURS', 'SYMPATHY'], ['LOW · MEDIUM · HIGH', 'DEFENSE']].map(x => '<div class="rv" style="border-style:dashed;opacity:.85"><span class="k">' + x[1] + '</span><small>' + x[0] + '</small></div>').join('') + '</div>';
    },
    b: ['You start with <b>12 Credits</b>. Every Prepare pays <b>+3</b> (rising every 2 rounds), and more for rich worlds you hold.',
        'Add ships (<b>2 CR</b>, up to 6 a fleet), start Fleets 2 and 3, repair, buy from the faction shops in <b>DOCKS</b> (one item per shop a round).',
        'Every world shows its <b>defense</b>. Hold a world and you learn whose side its <b>neighbours</b> are on.'] },
  { k:'STEP 2', t:'MOVEMENT',
    pic: () => '<svg viewBox="0 0 300 150" width="100%" style="max-width:320px">' +
             '<path d="M10 52 Q150 0 290 52" fill="none" stroke="#5FD4FF" stroke-opacity=".45" stroke-width="3"/><path d="M60 140 Q150 104 240 140" fill="none" stroke="#5FD4FF" stroke-opacity=".45" stroke-width="3"/>' +
             '<line x1="150" y1="26" x2="150" y2="118" stroke="#5FD4FF" stroke-width="3"/>' +
             '<g fill="none" stroke="#8CE7A0" stroke-width="2.5" stroke-linecap="round"><path d="M128 28 Q100 32 76 38"/><path d="M84 30 L74 38 L86 42"/><path d="M172 28 Q200 32 224 38"/><path d="M216 30 L226 38 L214 42"/><path d="M164 44 L164 96"/><path d="M158 88 L164 98 L170 88"/></g>' +
             '<circle cx="60" cy="40" r="13" fill="#2A3F55" stroke="#A9BDD0" stroke-width="2"/><circle cx="150" cy="26" r="15" fill="#2A3F55" stroke="' + FACTION_RING_COLORS[Math.max(0, primerFaction())] + '" stroke-width="3"/>' +
             '<circle cx="240" cy="40" r="11" fill="#0B1420" stroke="#B98CF0" stroke-width="2.5" stroke-dasharray="4 3"/><circle cx="150" cy="118" r="13" fill="#2A3F55" stroke="#A9BDD0" stroke-width="2"/>' +
             '<text x="240" y="70" text-anchor="middle" font-size="10" letter-spacing="1" fill="#B98CF0" font-family="sans-serif">NEBULA</text>' +
             '<text x="112" y="80" text-anchor="end" font-size="11" letter-spacing="1.5" fill="#8CE7A0" font-family="sans-serif">1 STEP</text></svg>',
    b: ['Move your ships on the <b>real board</b>: each fleet <b>one step</b>, to the next stop on its ring or one lane in or out.',
        'A nebula is a stop too.',
        'Nothing to tap on the phone. It trusts you.'] },
  { k:'STEP 3', t:'RESOLUTION',
    pic: () => '<div style="display:flex;justify-content:center;margin-bottom:6px">'+primerImg(5, 72)+'</div>' +
           primerRow('<span style="color:var(--red);font-size:22px">\u2694</span>', 'FIGHT', 'var(--red)', 'Each round both sides roll. The higher die sinks a ship (a tie goes to the bigger side).') +
           primerRow('<span style="color:var(--cyan);font-size:22px">\u275D</span>', 'NEGOTIATE', 'var(--cyan)', 'Answer its faction\u2019s questions. Do well and it joins you.') +
           primerRow('<span style="color:#FFD166;font-size:20px">CR</span>', 'BRIBE', '#FFD166', 'Pay its people to switch sides.'),
    b: ['Open the world your fleet is at, on the <b>STAR MAP</b>, and pick how. Each fleet attacks <b>once</b> a round.',
        'Before a fight, the <b>forecast</b> shows your odds. Home worlds only fall to a fight.',
        'You can attack another player\u2019s fleet too.'] },
  { k:'THE LAST ROUND', t:'THE FINAL HOUR',
    pic: () => phaseTrackHtml({ round: 8, roundLimit: 8, phase: 1 }) + '<div style="display:flex;justify-content:center;margin-top:8px">' + fhCard(1, { mini: true }) + '</div>',
    b: ['<b>A round early:</b> draw a <b>gambit</b> (whoever is behind draws more) and the <b>black market</b> opens. <b>Last Prepare:</b> call in <b>debts</b>.',
        '<b>Last Orders:</b> on the phone, secretly send each fleet at a world.',
        '<b>The Final Hour:</b> every strike lands at once, world by world on the table. Then the final score.'] },
];
// RIVET'S GUIDES: each card = Rivet's line (say), a picture, then the points.
const GUIDES = {
  gyou: { t: 'THE GALAXY AND YOU', q: 'WHAT NEEDS ME?', icon: 'target', col: '#FFD166', line: 'The Galaxy and You: the list on Home', cards: [
    { k:'ON HOME', t:'THE GALAXY AND YOU', say: 'Everything the galaxy is doing to you, or for you, lands in one list on Home. I sort it so the scary stuff is on top.',
      pic: () => primerRow(pixIcon('target', '#FF6B5A', 2), 'NOW', '#FF6B5A', 'lands or runs out this round, or a choice is waiting') +
                 primerRow(pixIcon('crown', '#FFD166', 2), 'YOUR MOVES', '#FFD166', 'things you can play when you like') +
                 primerRow(pixIcon('holonet', '#8FA3B8', 2), 'GOOD TO KNOW', '#8FA3B8', 'what\u2019s running in the background'),
      b: ['Only you see it. The header says how many things <b>need you now</b>.',
          'The first four rows show. Tap <b>+ MORE</b> for the rest; it stays open until you tap <b>SHOW LESS</b>.',
          'Every row opens what it\u2019s about. The word on the right says what to do: <b>CALM</b>, <b>CALL</b>, <b>DECIDE</b>, <b>DUE</b>.'] },
    { k:'THE ROWS', t:'WHAT YOU\u2019LL SEE', say: 'Here\u2019s who\u2019s who. Red means somebody is coming. Gold means you have something to spend or to do.',
      pic: () => primerRow(pixIcon('target', '#FF6B5A', 2), 'STRIKE \u00b7 OUT FOR YOU', '#FF6B5A', 'a faction hates you; calm them before next round') +
                 primerRow(pixIcon('job', '#FFD166', 2), 'JOB \u00b7 RACE', '#FFD166', 'a faction wants a world taken by a deadline') +
                 primerRow(pixIcon('owe', '#8CE7A0', 2), 'YOU OWE \u00b7 DEBTS', '#8CE7A0', 'promises from your talks; in the end, ships they send') +
                 primerRow(pixIcon('gift', '#FFD166', 2), 'GAMBITS \u00b7 BLACK MARKET', '#FFD166', 'the Final Hour, from the round before the last') +
                 primerRow(pixIcon('crown', '#FFD166', 2), 'DECREE \u00b7 ONCE A GAME', '#FFD166', 'your big moves, ready when you are') +
                 primerRow(pixIcon('cover', '#C8364A', 2), 'SPY \u00b7 COVER \u00b7 LIE', '#C8364A', 'secrets only you know'),
      b: ['Similar rows merge: three gambits are one row, and so are your promises.',
          'Nothing here? Then nobody is after you. Enjoy it.'] } ] },
  favor: { t: 'FAVOR', q: 'HOW THE GALAXY REACTS TO YOU', icon: 't4', col: '#8CE7A0', line: 'Favor: what each faction thinks of you', cards: [
    { k:'FAVOR', t:'THEY REMEMBER', say: 'Every faction keeps a ledger on you. Only you can see yours: tap FAVOR on Home.',
      pic: () => { const f = Math.max(0, primerFaction()); return [4, 3, 2, 1, 0].map(k => primerRow(pixIcon(FAVOR_ICON[k], FAVOR_COL[k], 2), (k + 1) + ' \u00b7 ' + favorWord(f, k).toUpperCase(), FAVOR_COL[k], FAVOR_MEANS[k])).join(''); },
      b: ['Jobs, good talks and hurting their rival move you <b>up</b>. Attacking their worlds moves you <b>down</b>.',
          'Reach tier 4 for a <b>gift</b>. Stay in tier 5 for an <b>ongoing</b> one.'] },
    { k:'FAVOR', t:'CHAMPIONS + ERAS', say: 'Climb to the top, and higher than anyone else at the table, and a faction names you its Champion. Then it gets loud.',
      pic: () => '<div class="pclass">' + ERAS.map(E => '<div><b style="color:' + E.col + '">' + E.name + '</b><small>' + E.brief + '</small></div>').join('') + '</div>',
      b: ['<b>Champion</b> is tier 6: the top of the ladder <i>and</i> the highest at the table. One title per player.',
          'A champion\u2019s decree starts that faction\u2019s <b>Era</b> for 2 rounds: a rule for everyone, a sting for everyone, a perk for the champion.'] } ] },
  news: { t: 'GALAXY NEWS', q: 'WHAT\u2019S GOING ON?', icon: 'holonet', col: '#5FD4FF', line: 'Galaxy News: headlines, jobs, races, messages', cards: [
    { k:'EVERY ROUND', t:'GALAXY NEWS', say: 'Each round starts with the news. The big story goes on the table. The small print is just for you.',
      pic: () => '<div class="pnews"><b>A RACE FOR KESSEL</b><span>First to take Kessel earns the Pykes\u2019 gratitude.</span></div><div class="pnews you"><b>A JOB FROM THE EMPIRE</b><span>Take Hoth within 3 rounds.</span></div>',
      b: ['<b>A JOB:</b> a faction wants something done by a deadline. Do it for favor.',
          '<b>A RACE:</b> first to do it wins. Finish in the same round and you all win.',
          '<b>A WARNING:</b> a faction that hates you strikes next round. It always says how to stop it.'] },
    { k:'MESSAGES', t:'THEY CALL YOU', say: 'When a faction\u2019s feelings change, somebody calls. The strip under the message says what changed. Tap my face on anything and I\u2019ll explain it.',
      pic: () => readoutHtml([['FROM', 'THE PYKES'], ['STANDING', standingHtml(2, 3, 1)], ['fx', 'EFFECT', [['good', 'ESCORTS', 'VS THE DAWN']]]]),
      b: ['The <b>bell</b>, top right, keeps every message and every move.',
          'A boxed name with a face is a <b>player</b>. Plain names are characters in the story.'] } ] },
  battle: { t: 'BATTLES', q: 'FIGHTING', icon: 'war', col: '#FF6B5A', line: 'Battles: dice, your odds, who helps, alerts', cards: [
    { k:'DICE', t:'BATTLE', say: 'Space combat is simple. Loud, but simple.',
      pic: () => {
        const col = FACTION_RING_COLORS[Math.max(0, primerFaction())];
        let sq = ''; for (let i = 0; i < 4; i++) sq += '<rect x="'+(i * 18)+'" y="2" width="13" height="13" rx="2" fill="#FF7A45"/>';
        return '<div class="pfight"><div>YOUR FLEET'+fcShips(3, col, false)+'<div class="pdie" style="border-color:'+col+';color:'+col+'">5</div></div>' +
               '<div style="font-size:16px;color:var(--dim)">VS</div><div>DEFENDERS<svg width="67" height="17">'+sq+'</svg><div class="pdie" style="border-color:#FF7A45;color:#FF7A45">3</div></div></div>' +
               '<div style="font-size:13px;letter-spacing:2px;color:'+col+';margin-top:10px">5 BEATS 3: ONE DEFENDER DOWN</div>';
      },
      b: ['Each exchange both sides roll a die. The <b>higher roll sinks one ship</b>. A tie misses.',
          'The <b>forecast</b> shows your odds and who\u2019s helping: a friendly faction may send an <b>escort</b>, an angry one raises a <b>militia</b>.',
          '<b>Withdraw</b> between exchanges if it goes badly. They get a parting shot.'] },
    { k:'UNDER ATTACK', t:'ALERTS', say: 'When someone attacks, every phone hears about it. If it\u2019s your world, I buzz. Loudly.',
      pic: () => '<div class="pnews" style="border-color:#FF6B5A"><b style="color:#FF6B5A">THANE ATTACKS NABOO</b><span><b style="color:#FF6B5A;display:inline">Your world!</b> Tap to watch.</span></div>',
      b: ['Tap the alert to <b>watch</b> the battle live.',
          'Joined late? Pick <b>WATCH THE GAME</b> when you connect and follow along.',
          'Destroy a player\u2019s whole fleet: <b>+2 VP</b>.'] } ] },
  moves: { t: 'ONCE A GAME', q: 'YOUR BIG MOVE', icon: 'gift', col: '#FFD166', line: 'Once a game: your character\u2019s special, from round 3', cards: [
    { k:'ONCE A GAME', t:'FOUR BIG MOVES', say: 'Every character gets one big move a game, from round 3. Use it well.',
      pic: () => CLASSES.map((c, i) => primerRow(pixIcon(['war', 'summit', 'cover', 'heist'][i], '#FFD166', 2), c.name + ' \u00b7 ' + c.move, '#FFD166', c.moveText.split('. ')[0] + '.')).join(''),
      b: ['Yours is on your card (tap your face). When it\u2019s ready it shows up on Home.'] },
    { k:'WARLORD', t:'WARPATH', say: 'Name it any time, and the whole table hears it. You strike in this round\u2019s Resolution. Declare it early and they get time to brace; late, and they don\u2019t.',
      pic: () => warTimeline(null, 'the world', -1, 4),
      b: ['Every ready fleet can join that one attack, plus 3 ships of your own.',
          'The last warpath can be declared two rounds before the end.'] } ] },
  scoundrel: { t: 'SELL IT TWICE', q: 'PLAYING BOTH SIDES', icon: 'heist', col: '#FFD166', line: 'Sell It Twice: the Scoundrel\u2019s side business', cards: [
    { k:'SCOUNDREL', t:'SELL IT TWICE', say: 'You did a job for one side. The other side wants it too. Sell it again. What could go wrong?',
      pic: () => primerRow(pixIcon('credits', '#FFD166', 2), 'A SALE', '#FFD166', '5 / 6 / 8 CR for an Outer / Mid / Core world, and favor with the buyer') +
                 primerRow(pixIcon('commotion', '#FFB347', 2), 'HEAT', '#FFB347', 'every sale adds one') +
                 primerRow(pixIcon('target', '#FF6B5A', 2), 'CAUGHT', '#FF6B5A', 'both sides find out'),
      b: ['Win a world, finish a job or a deed, and the other side may offer to buy it.',
          'Heat only cools in a round you <b>don\u2019t</b> sell. Get too hot and you\u2019re <b>caught</b>: both factions turn on you and strike next round.',
          'Pay one back (their money + 3) to call off its strike.'] } ] },
  gallery: { t: 'THE GALLERY', q: 'WHEN SOMEONE ELSE NEGOTIATES', icon: 'bell', col: '#5FD4FF', line: 'The Gallery: watch, whisper, vouch', cards: [
    { k:'TALKS', t:'THE GALLERY', say: 'When someone negotiates, everyone else can watch. And meddle. I love meddling.',
      pic: () => primerRow(pixIcon('holonet', '#5FD4FF', 2), 'WATCH', '#5FD4FF', 'follow the talk, question by question') +
                 primerRow(pixIcon('commotion', '#FF6B5A', 2), 'WHISPER \u00b7 2 CR', '#FF6B5A', 'their next question gets harder') +
                 primerRow(pixIcon('t4', '#8CE7A0', 2), 'VOUCH', '#8CE7A0', 'a wrong answer disappears from their next question'),
      b: ['Whisper, and if they still win, that faction decides you lied.',
          'Vouch (only if that faction likes you), and if they blow it anyway, it\u2019s on you.',
          'Either way your favor with that faction moves, and everyone sees who did what.'] } ] },
  finalhour: { t: 'THE FINAL HOUR', q: 'HOW THE GAME ENDS', icon: 'era', col: '#FFD166', line: 'The Final Hour: the last two rounds, step by step', cards: [
    { k:'THE ROUND BEFORE', t:'GET READY', say: 'One round before the end, the gambits are dealt. Stock up while you still can.',
      pic: () => '<div style="display:flex;gap:8px;justify-content:center">' + fhCard(13, { mini: true }) + fhCard(9, { mini: true }) + '</div>',
      b: ['<b>Gambits:</b> dealt the round before the last. Draw and keep one; the further behind you are, the more you draw. Hold up to 3.',
          '<b>Black market:</b> open from then until your Last Orders. 2 gambits for 15 CR each, and big-ticket items: a Capital Ship, a Planetary Shield, a Mercenary Armada, Hyperspace Charts, a Spy Network. One big item a game.',
          '<b>Debts:</b> in the last Prepare, factions that like you send ships. Neutral ones want Credits.'] },
    { k:'LAST ORDERS', t:'STRIKE', say: 'Pick a target for every fleet. Nobody sees your orders until the reveal.',
      pic: () => phaseTrackHtml({ round: 8, roundLimit: 8, phase: 1 }),
      b: ['A world next to one you hold (a lane or a ring step) is <b>in reach</b>. Anything else needs a <b>jump pass</b>: 3 CR.',
          'Pin gambits to your strikes, or keep a fleet home to <b>guard</b> a world.',
          'No talks or fights this round: Last Orders replace them.'] },
    { k:'THE REVEAL', t:'IT ALL LANDS', say: 'Then everything happens at once. Grab a snack.',
      pic: () => phaseTrackHtml({ round: 8, roundLimit: 8, phase: 2 }),
      b: ['Gambits are played, then the table plays the strikes world by world.',
          'Two players striking one world <b>turn on each other</b> first. The winner faces the defenders.',
          'Then the final score.'] } ] },
  galaxy: { t: 'THE GALAXY MOVES', q: 'WHEN THE GALAXY CHANGES', icon: 'race', col: '#FF8C42', line: 'Eras, events and the news', cards: [
    { k:'THE GALAXY', t:'IT CHANGES', say: 'The galaxy doesn\u2019t sit still. Neither should you.',
      pic: () => primerRow(pixIcon('era', '#FFC933', 2), 'ERAS', '#FFC933', 'a champion\u2019s rule, for 2 rounds') +
                 primerRow('<span style="font-size:20px;color:var(--dim)">?</span>', 'HOODED FIGURES', 'var(--dim)', 'sometimes you won\u2019t know who you\u2019re talking to'),
      b: ['In the Rebellion\u2019s Era, a failed <b>UPRISING</b> can be tried again.',
          'Watch the table\u2019s headlines. Everything big is announced there.'] } ] },
};
const GUIDE_ORDER = ['gyou', 'favor', 'news', 'battle', 'moves', 'scoundrel', 'gallery', 'finalhour', 'galaxy'];
function PRING_OF(i) { return lastS && lastS.planets[i] ? lastS.planets[i].ring : [0,0,1,0,2,0,0,0,2,1,1,1,0,0,1,1,2][i]; }
function PN_OF(i) { return lastS && lastS.planets[i] ? lastS.planets[i].name : ''; }
function primerHtml() {
  const f = primerFaction();
  if (primerGuide === '') {  // RIVET'S GUIDES: the index
    return '<div class="prim"><div class="primtop"><span>RIVET\u2019S GUIDES</span><button class="back" onclick="closePrimer()">CLOSE</button></div>' +
      '<div class="rvSay" style="margin-top:16px">' + rivetSvg(44) + '<div><b>RIVET</b><i>Bip-bip.</i> Pick a topic. I\u2019ll keep it short. Mostly.</div></div>' +
      '<div class="gdlist">' + GUIDE_ORDER.map(k => { const G = GUIDES[k];
        return '<button class="gyrow" onclick="openGuides(\'' + k + '\')">' + pixIcon(G.icon, G.col, 2) + '<span><b style="color:' + G.col + '">' + (G.q || G.t) + '</b> ' + G.line + '</span><em>' + G.cards.length + '</em></button>'; }).join('') +
      '<button class="gyrow" onclick="openPrimer()">' + pixIcon('victory', '#E8EEF5', 2) + '<span><b>HOW TO PLAY</b> the basics, 8 quick cards</span><em></em></button></div></div>';
  }
  const deck = primerDeck(), c = deck[primerAt], G = primerGuide ? GUIDES[primerGuide] : null, last = primerAt === deck.length - 1;
  const acc = G ? G.col : f >= 0 ? FACTION_UI_COLORS[f] : 'var(--cyan)';
  return '<div class="prim" ontouchstart="primerTS(event)" ontouchend="primerTE(event)">' +
    '<div class="primtop"><span>' + (G ? 'GUIDE \u00b7 ' + G.t : 'HOW TO PLAY') + ' \u00b7 ' + (primerAt + 1) + ' / ' + deck.length + '</span><button class="back" onclick="' + (G ? 'openGuides(\'\')' : 'closePrimer()') + '">' + (G ? 'GUIDES' : 'CLOSE') + '</button></div>' +
    '<div class="primk">' + c.k + '</div><div class="primt" style="color:' + acc + '">' + c.t + '</div>' +
    (c.say ? '<div class="rvSay">' + rivetSvg(36) + '<div><b>RIVET</b>' + c.say + '</div></div>' : '') +
    '<div class="primpic">' + c.pic() + '</div><div class="primb">' + c.b.map(x => x.startsWith('<div') ? x : '<p>' + x + '</p>').join('') + '</div>' +
    '<div class="primdots">' + deck.map((x, i) => '<i' + (i === primerAt ? ' style="background:' + acc + '"' : '') + '></i>').join('') + '</div><div class="primnav">' +
    (primerAt > 0 ? '<button class="pill quiet" onclick="primerGo(-1)">BACK</button>' : '<span></span>') +
    '<button class="pill" style="border-color:' + acc + ';color:' + acc + '" onclick="primerGo(1)">' + (!last ? 'NEXT' : G ? 'DONE' : 'READY') + '</button></div>' +
    (last && !G ? '<button class="back" style="margin-top:10px" onclick="openGuides(\'\')">' + rivetSvg(20) + ' MORE: RIVET\u2019S GUIDES</button>' : '') + '</div>';
}
function standingsRowsHtml(s) {
  let h = '';
  const order = [];
  for (let i=0;i<s.players.length;i++) if (s.players[i].active) order.push(i);
  order.sort((a,b) => s.players[b].vp - s.players[a].vp);
  order.forEach((slot,i) => {
    const p = s.players[slot];
    const f = p.chosenFaction;
    const col = f >= 0 ? FACTION_UI_COLORS[f] : 'var(--text)';
    const planets = s.planets.filter(pl => pl.owner === slot).length;
    h += '<div class="srow'+(slot===mySlot?' me':'')+(p.commander >= 0 ? ' tap" onclick="openCard('+slot+')' : '')+'"><span class="rk"'+(slot===mySlot?' style="color:var(--accent)"':'')+'>'+(i+1)+'</span>' +
         (f >= 0 ? insigniaSvg(f, 26) : '') +
         '<span class="nm" style="color:'+col+'">'+(f >= 0 ? FACTION_SHORT_NAMES[f] : p.name)+(slot===mySlot?' (YOU)':'')+(p.away ? ' <small style="color:var(--faint)">AWAY</small>' : '')+'</span>' +
         (p.commander >= 0 && p.commander % 4 === 3 && slot !== mySlot ? '<em class="bty">BOUNTY +3</em>' : '') +
         '<span class="vp">'+(p.vp < 0 ? '?' : p.vp)+'</span><span class="pl">'+planets+' pl.</span>'+(p.commander >= 0 ? '' : '')+'</div>';
  });
  return h;
}
function openGalaxy() { try { if (lastS) localStorage.setItem('sfMap', String(lastS.round)); } catch (e) {} galaxyMode = true; poll(); }
function closeGalaxy() { galaxyMode = false; currentPlanet = -1; poll(); }
function openFleetDetail(f) { currentFleetIdx = f; poll(); }
function backToFleetsList() { currentFleetIdx = -1; poll(); }

let battleMode = false;
let pvpTarget = -1;
let pvpAtkFleet = 0;  // attacking fleet (picked on Fleets since v0.21.10), -1 = none
let pvpAtkMask = 1;   // v0.28.0: every fleet sent in (bit per fleet)
let briefMercs = true;  // 2.0-dev.28: bring your hired ships (forecast toggle, planet and PvP)
let bAuto = false, bAutoFired = -1, bAutoStopped = '', bAutoFloor = 2;  // 2.0-dev.28 AUTO: keep fighting until YOU are down to 2 (3.1-dev.91: or to the end)
function toggleMercs() { briefMercs = !briefMercs; briefSnap = ''; poll(); }
function bMine(s) {
  const iAmDef = s.battleKind === 2 && s.battleDefenderSlot === mySlot, amAtk = !iAmDef, alive = (amAtk ? s.battleAtkAlive : s.battleDefAlive || []).filter(Boolean).length;
  return alive + (amAtk ? (s.bEsc || [0, 0])[1] + (s.bMerc || [0, 0])[0] : (s.bMerc || [0, 0])[1]);
}
function bAutoToggle() { bAuto = !bAuto; bAutoStopped = ''; bAutoFired = -1; bBtnSnap = ''; if (bAuto && bLastS) bAutoFloor = bMine(bLastS) <= 2 ? 0 : 2; if (bLastS) bUpdate(bLastS); }
async function bCallMercs() { await post('/call_mercs?id=' + encodeURIComponent(myId)); poll(); }
let pvpDefFleet = -1; // their fleet being struck (v0.21.10)
let pvpMsg = '';     // why the last PvP attack was refused (v0.20.2)
let fleetsMode = false;
let repMode = false;
let repSeen = [null, null, null, null], repMoved = [null, null, null, null];  // v0.29.0: where the Factions bars were last shown
let repBattleBase = null;  // v0.29.0: your scores when your current battle started
// One line per faction whose standing moved since `base`: its name and the arrows.
function repLinesHtml(base, s) {
  if (!base) return '';
  let out = '';
  for (let f = 0; f < 4; f++) {
    const d = (s.reputation[f].score || 0) - base[f];
    if (!d) continue;
    out += '<div class="repline"><span style="color:'+FACTION_UI_COLORS[f]+'">'+TX_FACTIONS[f].name+'</span><b style="color:'+(d > 0 ? '#8CE7A0' : '#FF6B5A')+'">'+repArrows(d)+'</b></div>';
  }
  return out ? '<div class="replines">'+out+'</div>' : '';
}
let standingsMode = false;
let galaxyMode = false;  // v0.21.0: planet list behind the Home hub's GALAXY tile
let lockConfirmPending = false, lastS = null;  // v0.21.1 lock-in confirm
let currentFleetIdx = -1;
// v0.21.12: EVERY in-game screen goes through setApp(), which skips the
// rebuild when the HTML is identical to what is already showing (Convention
// 8 -- rebuilding every 700ms poll dropped taps on LOCK IN and on the
// negotiation offers). Code that writes app.innerHTML directly (join pickers,
// battle card, which mutates its own DOM afterwards) must reset shownHtml.
let shownHtml = null;
let lastWaitBuzz = -1;  // v0.28.3: round*4+phase of the last "waiting on you" buzz
// 3.1-dev.1: setApp PATCHES the screen instead of rebuilding it. The new HTML
// is parsed off-screen and compared with what's showing; only the nodes,
// attributes and text that differ change. Elements that stay keep running
// their CSS animations and transitions, and a button being tapped is the same
// element before and after a poll. Rules: canvases keep their pixels (width /
// height are only set when they really change -- setting them clears the
// canvas); a focused input / textarea keeps what the player is typing;
// data-keep="1" marks a subtree code manages itself.
const MORPH_TPL = document.createElement('template');
function morphAttrs(a, b) {
  const ba = b.attributes;
  for (let i = 0; i < ba.length; i++) {
    const n = ba[i].name, v = ba[i].value;
    if (a.getAttribute(n) !== v) {
      if (a.tagName === 'CANVAS' && (n === 'width' || n === 'height') && String(a[n]) === v) continue;
      a.setAttribute(n, v);
    }
  }
  const aa = a.attributes;
  for (let i = aa.length - 1; i >= 0; i--) if (!b.hasAttribute(aa[i].name)) a.removeAttribute(aa[i].name);
}
function morphSame(a, b) {
  if (a.nodeType !== b.nodeType) return false;
  if (a.nodeType !== 1) return true;
  return a.tagName === b.tagName && (a.id || '') === (b.id || '');
}
function morphNode(a, b) {
  if (a.nodeType === 3 || a.nodeType === 8) { if (a.nodeValue !== b.nodeValue) a.nodeValue = b.nodeValue; return; }
  morphAttrs(a, b);
  if (a.tagName === 'CANVAS' || a.getAttribute('data-keep') === '1') return;
  if ((a.tagName === 'INPUT' || a.tagName === 'TEXTAREA') && document.activeElement === a) return;
  if (a.tagName === 'TEXTAREA' && a.value !== b.value) a.value = b.value;
  morphKids(a, b);
}
function morphKids(a, b) {
  let ac = a.firstChild, bc = b.firstChild;
  while (bc) {
    const nextB = bc.nextSibling;
    if (ac && morphSame(ac, bc)) { morphNode(ac, bc); ac = ac.nextSibling; }
    else {
      // an element with the same id further along moves up instead of being rebuilt
      let found = null;
      if (bc.nodeType === 1 && bc.id) for (let s = ac; s; s = s.nextSibling) if (s.nodeType === 1 && s.id === bc.id && s.tagName === bc.tagName) { found = s; break; }
      if (found) { a.insertBefore(found, ac); morphNode(found, bc); }
      else a.insertBefore(bc, ac);  // moves the parsed node in (b is throwaway)
    }
    bc = nextB;
  }
  while (ac) { const n = ac.nextSibling; a.removeChild(ac); ac = n; }
}
// 3.1 SHEET layer: every decision slides up from the bottom over the screen
// it belongs to. sheetShow(key, html, color, onDismiss) opens it (or patches it
// in place when the same key is already up, so polls never jolt it);
// sheetHide() slides it away. Tapping the dimmed screen calls onDismiss.
let sheetKey = null, sheetHtml = null, sheetOnDismiss = null;
function sheetShow(key, h, col, onDismiss) {
  const L = document.getElementById('lyrSheet'), B = document.getElementById('sheetBody');
  sheetOnDismiss = onDismiss || null;
  L.style.setProperty('--sheetCol', col || 'var(--accent)');
  if (sheetKey !== key || !B.childElementCount) { B.innerHTML = h; }
  else if (h !== sheetHtml) { MORPH_TPL.innerHTML = h; morphKids(B, MORPH_TPL.content); }
  sheetKey = key; sheetHtml = h;
  if (!L.classList.contains('on')) { void L.offsetWidth; L.classList.add('on'); document.body.classList.add('sheetOn'); }
}
function sheetHide() {
  if (!sheetKey) return;
  sheetKey = null; sheetHtml = null; sheetOnDismiss = null;
  document.getElementById('lyrSheet').classList.remove('on');
  document.body.classList.remove('sheetOn');
}
function sheetScrimTap() { const f = sheetOnDismiss; if (f) f(); else sheetHide(); }
// 3.1 NOTIFICATION layer: news that arrives mid-round drops in from the top,
// stacks (newest on top, at most 3), and fades away on its own or on a tap.
// It never covers a decision and never rebuilds the screen underneath.
// 3.1-dev.74 (user: "when the popups come on and even stack on, you can't see the dice ... the stamps also show up before the
// dice animation finishes"): while a battle is on screen, news that isn't about it waits (a small badge says how many) and
// stamps wait until the dice have landed.
let battleOnScreen = false;
const NT_HELD = [], NT_STAMP = [], NT_AFTER = [];  // 3.1-dev.114 NT_AFTER: stamps from a finished battle
function ntBadge() {
  let b = document.getElementById('ntHeld');
  if (!NT_HELD.length) { if (b) b.remove(); return; }
  if (!b) { b = document.createElement('div'); b.id = 'ntHeld'; document.body.appendChild(b); }
  b.textContent = NT_HELD.length + ' UPDATE' + (NT_HELD.length === 1 ? '' : 'S') + ' AFTER THE BATTLE';
}
setInterval(() => {
  if (NT_STAMP.length && !bAnimActive && performance.now() - bAnimStart > 300) notifyNow.apply(null, NT_STAMP.shift());
  if (!battleOnScreen && NT_AFTER.length) notifyNow.apply(null, NT_AFTER.shift());
  if (!battleOnScreen && NT_HELD.length) { if (NT_HELD.length > 3) NT_HELD.splice(0, NT_HELD.length - 3); NT_HELD.splice(0).forEach(x => notifyNow.apply(null, x)); ntBadge(); }
}, 350);
function notify(head, line, col, ms, stamp, onTap) {
  if (battleOnScreen) { if (stamp && lastS && lastS.battleOver) NT_AFTER.push([head, line, col, ms, stamp, onTap]); else if (stamp) NT_STAMP.push([head, line, col, ms, stamp, onTap]); else { NT_HELD.push([head, line, col, ms, stamp, onTap]); ntBadge(); } return; }
  notifyNow(head, line, col, ms, stamp, onTap);
}
function notifyNow(head, line, col, ms, stamp, onTap) {
  const L = document.getElementById('lyrNotif');
  const n = document.createElement('div');
  n.className = 'nt' + (stamp ? ' stamp' : ''); n.style.setProperty('--ntCol', col || 'var(--accent)');
  n.innerHTML = '<div class="ntHead">' + head + '</div>' + (line ? '<div class="ntLine">' + line + '</div>' : '');
  const gone = () => { if (!n.parentNode || n.classList.contains('out')) return; n.classList.add('out'); setTimeout(() => n.remove(), 420); };
  n.onclick = () => { gone(); if (onTap) onTap(); };  // 3.1-dev.52: WATCH from a notification
  let ty = null;  // 3.1-dev.114 (playtest: "notifications should be swipe away able"): flick it up
  n.addEventListener('touchstart', e => { ty = e.touches[0].clientY; }, { passive: true });
  n.addEventListener('touchmove', e => { if (ty === null) return; const dy = e.touches[0].clientY - ty; if (dy < 0) n.style.transform = 'translateY(' + dy + 'px)'; if (dy < -28) { ty = null; n.onclick = null; gone(); } }, { passive: true });
  n.addEventListener('touchend', () => { if (ty !== null) n.style.transform = ''; ty = null; }, { passive: true });
  L.insertBefore(n, L.firstChild);
  while (L.children.length > 3) L.lastChild.remove();
  requestAnimationFrame(() => requestAnimationFrame(() => n.classList.add('in')));
  setTimeout(gone, ms || 5000);
}
function setApp(app, h) {
  if (h === shownHtml && app.childElementCount) return;
  if (!app.childElementCount) { app.innerHTML = h; shownHtml = h; return; }
  MORPH_TPL.innerHTML = h;
  morphKids(app, MORPH_TPL.content);
  shownHtml = h;
}
const galaxyImgs = [];      // per-planet pixel-art dataURLs, built once
let lastSeatSnapshot = '';
let lastFactionSnapshot = '';
let payConfirmPending = false;
let briefingPending = false, briefFleet = -1, briefSnap = '';
let leaveNegPending = false;  // "leave negotiation?" confirm showing (v0.20.2)
// Short faction words for roll sources ("+1 Rebel support"); matches the .ino.
const FACTION_WORDS = ['Rebel', 'Imperial', 'Pyke', 'Crimson Dawn'];

const SHIP_NAMES = ['X-WING','Y-WING','CORVETTE','ESCORT A','ESCORT B','ESCORT C','ESCORT D'];  // 2.0-dev.20: the Empire's 7th

const CHARS = ['Affiliation','Resources','Unique Trait','Defense'];

const AFFILIATION_TO_FACTION = ['Rebel Alliance','Galactic Empire','Pyke Syndicate','Crimson Dawn'];

// First fleet ready to attack: has ships and hasn't fought this round (v0.20.2).
function myFirstFleet(me) {
  for (let f=0; f<me.fleets.length; f++) if (me.fleets[f].alive > 0 && !me.fleets[f].spent) return f;
  return -1;
}

// ---------------------------------------------------------------------------
// FACTION TRANSMISSION -- phone-side match for the AMOLED's "face moment."
// Same three beats: static burst (tuning in) -> shake (connection hits) ->
// typewriter (the message). Tap during static to skip it; tap after typing
// finishes to dismiss.
// ---------------------------------------------------------------------------
// v0.21.0 UI refresh: per-faction text-safe colors, short names, and the
// original insignia (Rebel sunburst, Imperial hexagon, Pyke diamond, Crimson
// Dawn crescent) -- same shapes the AMOLED draws in uiDrawInsignia().
const FACTION_UI_COLORS = ['#FFC933', '#D4DEE8', '#B98CF0', '#E0707A'];
const FACTION_SHORT_NAMES = ['REBELS', 'EMPIRE', 'PYKES', 'CRIMSON DAWN'];
const FACTION_RING_COLORS = ['#FFC933', '#E6EEF5', '#9B5FE0', '#D0343E'];  // = FACTION_RING_COLOR in the .ino
// v0.21.20 commanders: portraits come from the board (/art -- the same
// generated data the AMOLED draws), fetched once and painted to canvases.
// Commander id = faction*3 + i.
let ART = null;
const portraitCache = {};
// holo (v0.21.21) = the AMOLED's CMD_IMG_HOLO: brightness, stretched per
// portrait, picks one of five blues; scanlines every 3rd canvas row.
const HOLO_BLUES = ['#0B2C3C', '#17607A', '#2E9CBC', '#6FDCFA', '#D4F7FF'];
// holo 2 (v0.30.0) = the UNKNOWN AMBASSADOR: the same shading in grays, rows torn sideways.
const AMB_GRAYS = ['#1a1d22', '#3a3f47', '#5d636c', '#8b9199', '#c3c8ce'];
// 3.1-dev.51 (user: "a couple hooded figures here and there fits the star wars vibe"): some unknown ambassadors are a hooded
// figure, drawn in the same torn grey as the scrambled faces (never a faction's colours -- it gives nothing away).
const HOOD_ROWS = ['....hhhh....', '...hhhhhh...', '..hhhhhhhh..', '..hhddddhh..', '.hhddddddhh.', '.hhdeddedhh.', '.hhddddddhh.',
                   '.hhhddddhhh.', '..hhhddhhh..', '..hhhkkhhh..', '.hhhhkkhhhh.', 'hhhhhkkhhhhh', 'hhhhhkkhhhhh', 'hhhhhkkhhhhh'];
const HOOD_URLS = {};
function hoodedURL(id) {
  if (HOOD_URLS[id]) return HOOD_URLS[id];
  const c = document.createElement('canvas'); c.width = 96; c.height = 112;
  const g = c.getContext('2d'), col = { h: '#8E99A6', d: '#1C2128', e: '#DDE6EF', k: '#6E7884' };
  HOOD_ROWS.forEach((r, y) => [...r].forEach((ch, x) => {
    if (!col[ch]) return;
    g.fillStyle = col[ch];
    g.fillRect((x + ambShift(y, id) * 0.5) * 8, y * 8, 8, 8);
  }));
  for (let y = 0; y < 112; y += 3) { g.fillStyle = 'rgba(5,7,12,.35)'; g.fillRect(0, y, 96, 1); }  // scanlines
  return (HOOD_URLS[id] = c.toDataURL());
}
function ambShift(y, id) { const h = (y * 7 + id * 3) % 9; return h === 0 ? 2 : h === 4 ? -2 : h === 6 ? 1 : 0; }  // = the .ino
function portraitURL(id, scale, holo) {
  if (!ART || id < 0 || id >= ART.c.length) return '';
  const key = id + ':' + scale + (holo ? 'h' + holo : '');
  if (portraitCache[key]) return portraitCache[key];
  const c = ART.c[id], cv = document.createElement('canvas');
  cv.width = ART.w * scale; cv.height = ART.h * scale;
  const ctx = cv.getContext('2d');
  const lum = i => { const n = parseInt(c.p.substr(i * 6, 6), 16); return Math.floor((30 * (n >> 16) + 59 * ((n >> 8) & 255) + 11 * (n & 255)) / 100); };
  let lo = 255, hi = 0;
  if (holo) for (let i = 1; i < c.p.length / 6; i++) { lo = Math.min(lo, lum(i)); hi = Math.max(hi, lum(i)); }
  const pal = holo === 2 ? AMB_GRAYS : HOLO_BLUES;
  for (let k = 0; k < ART.w * ART.h; k++) {
    const i = parseInt(c.x.substr(k * 2, 2), 16); if (!i) continue;
    ctx.fillStyle = holo ? pal[Math.floor(((lum(i) - lo) * 4 + Math.max(1, hi - lo) / 2) / Math.max(1, hi - lo))] : '#' + c.p.substr(i * 6, 6);
    const ay = Math.floor(k / ART.w), ax = (k % ART.w) + (holo === 2 ? ambShift(ay, id) : 0);
    ctx.fillRect(ax * scale, ay * scale, scale, scale);
  }
  if (holo) { ctx.globalCompositeOperation = 'source-atop'; ctx.fillStyle = 'rgba(3,8,13,0.4)'; for (let y = 0; y < cv.height; y += 3) ctx.fillRect(0, y, cv.width, 1); }
  return (portraitCache[key] = cv.toDataURL());
}
// Who speaks for a faction to you -- twin of commanderVoiceFor() in commander_art.h.
// ---- 3.1-dev.6: the 16 CHARACTERS (id = faction * 4 + class) ----
// Stories, facts and class kits are static, so they live here, not in /state.
// Wording matches the vision doc (tab "Characters and classes").
const CLASSES = [
  { name: 'WARLORD', great: 'First strike: in a battle you start, your fleet fights +1 ship. The muscle: a world you take by force pleases its rival more. War spoils: every world you take by force, your own faction cheers (+3 favor). Guns for hire: once a round, lend up to 3 ships to another commander\u2019s next battle, for a price.', hurt: 'Blunt: negotiations ask you one more question.', move: 'WARPATH',
    moveText: 'Name a world, any time. The table hears it. This round\u2019s Resolution, every ready fleet can join your attack there, with 3 more ships.', quest: 'Conqueror: take 4 worlds by force.' },
  { name: 'CONSUL', great: 'Silver tongue: one fewer question, and your contacts help on every one. Old friends: you know every world\u2019s Sympathy. Treaty worlds: win one by talking and it gets +2 defenders; through next round no bribes and +1 CR. You can talk to home worlds. The back channel: pay a player to stay away; break it and you get double.', hurt: 'No stomach for war: taking a world by force costs extra favor with its faction.', move: 'SUMMIT',
    moveText: 'No one can attack up to 3 worlds for 2 rounds. You negotiate and bribe easier meanwhile. Break-ins face 6 peacekeepers.', quest: 'Kingmaker: win 4 worlds without a fight (negotiate or bribe).' },
  { name: 'INFILTRATOR', great: 'Pull a file: every other round, open one rival\u2019s file (Credits, debts, grudges, job, patrols). It stays open until your next pull. Long ears: your worlds tell you the Sympathy of worlds two jumps away.', hurt: 'Spread thin: worlds you hold defend with 1 less.', move: 'DEEP COVER',
    moveText: 'Pose as one of them for 3 rounds: their prices, their help, their protection. Plant one lie on the Holonet. Attack their worlds and you\u2019re found out.', quest: 'Spymaster: know the Sympathy of 12 worlds.' },
  { name: 'SCOUNDREL', great: 'Sell it twice: whenever you please a faction, its rival will pay you for the same work. Get greedy and they compare notes. The stash: your income is never below 6.', hurt: 'Price on your head: whoever takes one of your worlds gets +3 CR, and everyone knows it.', move: 'THE HEIST',
    moveText: 'Rob another faction\u2019s vault. Smooth: +5 CR and a free good, and nobody knows. Bold: roll a die. 3\u20136 is +10 CR, a good and a stolen ship; 1\u20132 you escape with 3 CR and they know your face. Win bold and you can push your luck once.', quest: 'Make a fortune: hold 30 Credits at once.' },
];
const CHARACTERS = [
  { s: 'Marr', home: 'Chandrila', age: '58', ship: 'the Long Odds', quirk: 'Still polishes his old Imperial boots every morning.', story: "Tobin Marr has buried more friends than most soldiers ever meet. For twenty years he has led the Rebellion's hardest campaigns, winning just enough of them to keep the cause alive. Last winter an Imperial courier found him on Chandrila with a sealed letter from his old academy roommate, now a Grand Moff. It offered a full pardon, a fleet of his own, and an end to the war on his terms. Marr burned the letter. He kept the seal." },
  { s: 'Danto', home: 'Naboo', age: '47', ship: 'the Quiet Assembly', quirk: 'Writes every speech by hand.', story: "Aris Danto spent twenty years in the Imperial Senate, believing it could still do some good. Then Danto cast the vote that dissolved a planetary assembly and saw what came after. Within a month, Danto had defected. Now the senator speaks in Rebel hangars, and the crowds grow larger every time. Old friends in the Senate still send birthday wishes, and Danto still reads every one." },
  { s: 'Voren', home: 'Tatooine', age: '34', ship: 'the Backroad', quirk: 'Never sits with her back to a door.', story: "Ilsa Voren knows every back route in the Outer Rim, because she drew the maps. Before the Rebellion, she charted hidden smuggling lanes for Crimson Dawn, one route at a time. One night she took the maps and vanished. The Rebels made her a commander. Crimson Dawn wants its maps back, and it is patient." },
  { s: 'Kess', home: 'Kessel', age: '24', ship: 'the Lucky Debt', quirk: 'Talks to the ship, and swears it listens.', story: "Kess Arrano grew up running spice out of the Kessel mines. At nineteen, Kess stole a ship and flew it straight into a Rebel cockpit. Kess turned out to be the best pilot in the squadron, and was promoted when the last commander didn't come back. The Pykes still hold the debt on that stolen ship. Oxa Tull keeps the paperwork, and Rebel command has no idea." },
  { s: 'Thane', home: 'Carida', age: '41', ship: 'the Unbroken', quirk: 'Reads the same three-word message every night before he sleeps.', story: "Varo Thane has hunted Rebels across forty systems and never lost a battle. Two years ago, on a burning moon, he cornered the last cell of a Rebel uprising and found his younger sister leading it. He let her walk away. His report said she died in the fighting, and the Empire believed him. Once a year she sends him a message, always the same three words: Not too late." },
  { s: 'Vell', home: 'Coruscant', age: '52', ship: 'the Balanced Account', quirk: 'Has never once been late.', story: "Moff Seraine Vell governs by ledger, and every planet is an entry to be balanced. Her spice tariffs balance beautifully, because a Pyke accountant helps her keep two sets of books. The Emperor sees one. The Pykes see the other. Vell keeps the difference." },
  { s: 'Stahl', home: 'Kuat', age: '63', ship: 'the Patience', quirk: 'Plays dejarik against himself, and loses.', story: "Admiral Kaedric Stahl has thirty years of victories and a reputation for patience. What the Navy doesn't know is where those victories came from. For decades he has bought secrets from Crimson Dawn, through a woman in a red mask he has never seen. Every winning plan he ever drew came with a bill, and the Dawn has started asking to be paid." },
  { s: 'Merrow', home: 'Bespin', age: '31', ship: 'the Second Chance', quirk: "Can't stand silence, so hums constantly.", story: "Dace Merrow flew guns for the Rebellion until an Imperial patrol made an offer: a full pardon and a clean ship, in exchange for a few names. Dace took the deal. Now Dace runs errands for the Empire, smiles a lot, and never talks about the names. The old Rebel codes are still hidden in a boot heel, just in case." },
  { s: 'Rhel', home: 'Oba Diah', age: 'older than anyone dares ask', ship: 'the Truce', quirk: 'Signs every contract in her own blood.', story: "Matriarch Rhel is as old as the spice routes. She speaks rarely, and she is always obeyed. Forty years ago, when the Empire's fleets came to Kessel, Rhel chose not to fight. She signed a truce and kept the mines running. Young Pykes call it surrender. Rhel calls it the reason they're alive to complain, and she has never once broken her word to the Empire." },
  { s: 'Vosk', home: 'Nal Hutta', age: '60', ship: 'the Fair Price', quirk: 'Never accepts a first offer, even a good one.', story: "Vosk the Broker can put a price on anything, and he already knows yours. He sells to everyone, including, quietly, the Rebellion. Lately he keeps forgetting to bill them. Vosk calls it good business. The Matriarch doesn't know, and Vosk would like to keep it that way." },
  { s: 'Nyra', home: 'Kijimi', age: '29', ship: 'the Nowhere', quirk: 'Answers to four different names.', story: "Nyra Sol spent six years inside Crimson Dawn, learning how to disappear. Then she sold the Pykes a list of every Dawn safehouse on Kessel and disappeared for real. The Pykes pay her well and never quite trust her. Somewhere, someone in a red mask is still looking for her." },
  { s: 'Oxa', home: 'Kessel', age: '45', ship: 'the Interest', quirk: 'Never forgets a face, or a number.', story: "Oxa Tull collects what's owed, with interest. For twenty years Oxa collected only for the Pykes. These days Oxa collects for whoever pays on time, and Crimson Dawn pays on time and pays better. Oxa's little black book holds every debt in the galaxy, including one owed by a Rebel pilot named Kess." },
  { s: 'Korrik', home: 'Oba Diah', age: '38', ship: 'the Grudge', quirk: 'Checks the price on his own head every morning.', story: "Korrik Dray broke legs for the Pykes for ten years and took pride in the craft. Then the Matriarch handed his crew's share to her own nephew. Korrik took the crew and two gunships to Crimson Dawn, which welcomed him with open arms. The Pykes put a price on his head. Korrik finds it insultingly low." },
  { s: 'Ymra', home: 'unknown', age: 'unknown', ship: 'the Velvet', quirk: 'Has never been seen without her mask.', story: "No one has seen Lady Ymra Vash's face, but everyone has felt her reach. She dines with Imperial Moffs twice a month, and they always leave thinking the evening was their idea. Some in the Dawn whisper that Ymra likes the Empire's order a little too much. Nobody whispers it twice." },
  { s: 'Brakk', home: 'Yavin 4', age: '36', ship: 'the Short Fuse', quirk: 'Hums while setting charges.', story: "Brakk says little and breaks things. Crimson Dawn found Brakk in an Imperial labor camp, a Rebel demolitions expert waiting on a death sentence, and offered a way out. Brakk has repaid that debt a hundred times over, one shattered vault at a time. But every charge Brakk builds still carries the same small mark scratched into the casing: the Rebel starbird. No one in the Dawn has thought to ask why." },
  { s: 'Kaan', home: 'Corellia', age: '33', ship: 'the One in Ten', quirk: 'Has never lost a hand of sabacc (allegedly).', story: "Sereth Kaan is charming, ruthless and always two moves ahead. Sereth started out running spice for the Pykes, skimming one crate in every ten, and never got caught. Those crates bought a seat in Crimson Dawn. The Pykes still wonder where their spice went, and Sereth still knows every one of their routes." },
];
const DEFECTOR_FROM = [-1, 1, -1, -1, -1, -1, -1, 0, -1, -1, 3, -1, 2, -1, -1, -1];  // twin of CHARACTER_DEFECTOR_FROM
const FACTION_VOICES = [[0, 2, 3], [5, 4, 6], [9, 8, 11], [13, 14, 15]];               // twin of commander_art.h
let charPick = -1, charEdit = false, cardMode = false;
const MOVES_BUILT = true;  // 3.1-dev.13: the once-a-game moves are in
function charName(id) { return ART && ART.c[id] ? ART.c[id].n : (CHARACTERS[id] ? CHARACTERS[id].s : ''); }
function charFacts(id) { const c = CHARACTERS[id]; return c.home + ' \u00b7 ' + c.age + ' \u00b7 ' + c.ship; }
function pickChar(id) { charPick = id; if (lastS) render(lastS); }
async function takeCharacter() {
  if (charPick < 0) return;
  const r = await post('/choose_character?id=' + encodeURIComponent(myId) + '&character=' + charPick);
  if (r && r.ok) { charEdit = false; }
  poll();
}
function openCard(slot) { cardSlot = slot === undefined ? -1 : slot; cardMode = true; shownHtml = null; if (lastS) render(lastS); }
function closeCard() { cardMode = false; if (lastS) render(lastS); }
// 3.1-dev.94 (user: "isn't quite so much reading", "it feels like we might relaying the info twice"): the select card says how a
// class plays in one line + the power's name; every rule lives on the kit page (YOUR KIT), the same rows as in a game.
const CLASS_PITCH = ['Hits hard, takes worlds by force.', 'Smooth talker, avoids fights.', 'Knows everyone\u2019s secrets.', 'Plays both sides, gets paid twice.'];
function charKitHtml(id) {
  const f = Math.floor(id / 4), cls = id % 4, K = KITS[cls], C = CLASSES[cls], CC = '#FFB347';
  return '<div class="kitHead" style="--fc:' + FACTION_UI_COLORS[f] + '">' + (ART ? '<img class="pixel" src="' + portraitURL(id, 3) + '" alt="">' : '') +
         '<div><b>' + charName(id).toUpperCase() + '</b><span><i style="color:' + FACTION_UI_COLORS[f] + '">' + FACTION_SHORT_NAMES[f] + '</i> \u00b7 <i style="color:' + CC + '">' + C.name + '</i></span></div></div>' +
         '<div class="kitGrp" style="color:#8CE7A0">WORKING FOR YOU</div>' + K.good.map(r => kitRow(kitIcon(-1, CC), r[0], r[1], C.name)).join('') +
         kitRow(kitIcon(f, FACTION_UI_COLORS[f]), SHIP_QUIRK[f][0], SHIP_QUIRK[f][1], FACTION_SHORT_NAMES[f] + ' SHIPS') +
         '<div class="kitGrp" style="color:#FF6B5A">WORKING AGAINST YOU</div>' + K.bad.map(r => kitRow(kitIcon(-1, CC), r[0], r[1], C.name)).join('') +
         '<div class="kitGrp" style="color:#FFD166">ONCE A GAME</div>' + kitRow(kitIcon(-1, CC), C.move, C.moveText, 'FROM ROUND ' + MOVE_K.from, '#FFD166') +
         '<div class="kitGrp" style="color:#5FD4FF">YOUR QUEST \u00b7 +5 VP</div>' + kitRow(kitIcon(-1, CC), C.quest.split(':')[0].toUpperCase(), (q => q.charAt(0).toUpperCase() + q.slice(1))((C.quest.split(':')[1] || '').trim()), C.name, '#5FD4FF');
}
function openCharKit(id) { sheetShow('ckit' + id, charKitHtml(id) + '<div class="biAct"><button class="ghost" onclick="sheetHide()">CLOSE</button></div>', FACTION_UI_COLORS[Math.floor(id / 4)]); }
// The kit, as on the card (great at / hurts you / once a game / quest).
function classKitHtml(cls, round) {
  const k = CLASSES[cls];
  const moveOpen = MOVES_BUILT && round >= 3;  // the moves arrive in stage 5
  return '<div class="kit good"><b>YOU\u2019RE GREAT AT</b>' + k.great + '</div>' +
         '<div class="kit bad"><b>WHAT HURTS YOU</b>' + k.hurt + '</div>' +
         '<div class="kit move"><b>ONCE A GAME \u00b7 ' + k.move + '</b>' + k.moveText + '<small>' + (moveOpen ? 'Ready when you are: COMMS: Once a game.' : 'From round 3.') + '</small></div>' +
         '<div class="kit quest"><b>YOUR QUEST \u00b7 +5 VP</b>' + k.quest + '</div>';
}
// Starting standing, in words (favor tiers arrive in stage 2).
function startLine(id) {
  const f = Math.floor(id / 4), rival = [1, 0, 3, 2][f], from = DEFECTOR_FROM[id];
  const N = i => feedTitle(FACTION_SHORT_NAMES[i]);
  let t = 'Starts as \u201C' + favorWord(f, 2) + '\u201D to the ' + N(f) + ' (one good move from their gift), \u201C' + favorWord(rival, 1) + '\u201D to the ' + N(rival) + '.';
  if (from >= 0) t += ' Old contacts with the ' + N(from) + ': the same head start, and you know their worlds from turn one.';
  return t;
}
function charSelectHtml(s) {
  const takenBy = f => s.players.findIndex((p, i) => i !== mySlot && p.active && p.chosenFaction === f);
  const me = s.players[mySlot];
  if (charPick < 0) charPick = me.commander >= 0 ? me.commander : -1;
  let h = '<p class="joinsub" style="margin-top:18px"><small>PLAYER ' + (mySlot + 1) + '</small><br>CHOOSE YOUR CHARACTER</p>';
  h += '<div class="cgrid"><span></span>' + CLASSES.map(c => '<span class="chead">' + c.name + '</span>').join('');
  for (let f = 0; f < 4; f++) {
    const t = takenBy(f);
    h += '<span class="crow" style="color:' + FACTION_UI_COLORS[f] + '">' + insigniaSvg(f, 22) + '</span>';
    for (let c = 0; c < 4; c++) {
      const id = f * 4 + c, def = DEFECTOR_FROM[id] >= 0, on = id === charPick;
      if (t >= 0) { h += '<button class="ccell taken" disabled>' + (ART ? '<img class="pixel" src="' + portraitURL(id, 2) + '" alt="">' : '') + '<small>P' + (t + 1) + '</small></button>'; continue; }
      h += '<button class="ccell' + (on ? ' on' : '') + '" style="--fc:' + FACTION_UI_COLORS[f] + (def ? ';--dc:' + FACTION_UI_COLORS[DEFECTOR_FROM[id]] : '') + '" onclick="pickChar(' + id + ')">' +
           (def ? '<i class="cdef"></i>' : '') + (ART ? '<img class="pixel" src="' + portraitURL(id, 2) + '" alt="">' : '') + '<small>' + CHARACTERS[id].s + '</small></button>';
    }
  }
  h += '</div>';
  if (charPick >= 0) {
    const id = charPick, f = Math.floor(id / 4), cls = id % 4, from = DEFECTOR_FROM[id];
    h += '<div class="cprev" style="--fc:' + FACTION_UI_COLORS[f] + '"><div class="cprevTop">' + (ART ? '<img class="pixel cface" src="' + portraitURL(id, 3) + '" alt="">' : '') +
         '<div><div class="cname">' + charName(id) + '</div><div class="cmeta">' + FACTION_SHORT_NAMES[f] + ' \u00b7 ' + CLASSES[cls].name + (from >= 0 ? ' \u00b7 DEFECTOR' : '') + '</div>' +
         '<div class="cfacts">' + charFacts(id) + '</div></div></div>' +
         '<div class="cstory">' + CHARACTERS[id].story + '</div>' +
         '<div class="cpitch">' + CLASS_PITCH[cls] + '<small>Once a game: <b>' + CLASSES[cls].move + '</b></small></div>' +
         '<div class="cstart">' + startLine(id) + '</div>' +
         '<button class="pill" style="border-color:#FFB347;color:#FFB347;margin-top:12px" onclick="openCharKit(' + id + ')">YOUR KIT<small>What works for you, against you, and your once-a-game move</small></button></div>';
    h += '<button class="pill" style="border-color:' + FACTION_UI_COLORS[f] + ';color:' + FACTION_UI_COLORS[f] + '" onclick="takeCharacter()">TAKE COMMAND</button>';
  } else h += '<p class="joinhint">Rows are factions, columns are classes. Tap anyone to read their story.</p>';
  return h;
}
// 3.1-dev.7: your class quest with a progress bar (Standings + your card).
// 3.1-dev.8 THE FAVOR SPINE (vision doc tab "Favor ladders"). Index = faction (0 Rebels, 1 Empire, 2 Pykes, 3 Crimson Dawn), tier 0..4 = 1..5.
// 3.1-dev.72 (user: "unify those so you don't have to know 16 of them"): ONE set of tier words for every faction. The
// faction-flavored names (Hero of the Empire, A loose end...) live on only where a character speaks.
const FAVOR_WORDS = [0, 1, 2, 3].map(() => ['Enemy', 'Distrusted', 'Neutral', 'Friend', 'Family']);
const FAVOR_ICON = ['target', 't2', 't3', 't4', 't5'];
const FAVOR_COL = ['#FF6B5A', '#FFB347', '#A9BDD0', '#8CE7A0', '#FFD166'];
const FAVOR_MEANS = ['They come for you. Their militia fights you, they won\u2019t talk, their shop is shut, Pay +2.',
                     'They\u2019re watching. Their shop is shut, Pay +1. One more bad move and they come for you.',
                     'Nobody special. Their shop is open, bribes at list price.',
                     'Their escort helps you against their rival, their contact on your last question. Pay \u22121.',
                     'Their contact helps on every question. Pay \u22122.'];
const GIFT4 = [['Sympathizers', 'A free ship joins one of your fleets.'], ['Imperial survey', 'Learn the Defense of 2 worlds.'],
               ['A loan you never repay', '+6 Credits.'], ['A secret', 'Learn the Sympathy of 2 worlds.']];
const GIFT5 = [['A safe world', 'Your home world fights with 2 loyal Rebel ships.'], ['A ship of the line', 'An Imperial ship rides with you in every battle.'],
               ['A spice route', '+1 income every round.'], ['Blackmail', 'Once: a rival pays you 8 CR, or the table hears their debts.']];  // 3.1-dev.78 (was a spy at court)
function tierIdx(s, f) { const r = s.reputation && s.reputation[f]; return r ? Math.max(0, ['HOSTILE', 'WARY', 'NEUTRAL', 'FAVORABLE', 'ALLIED'].indexOf(String(r.tier).toUpperCase())) : 2; }
// 3.1-dev.115: the Favor page's five-zone bar with your marker, on its own (the AUDIENCE picker: "you need to be able to see the standing bars")
function standingBar(s, f) {
  const ZLO = [-50, -29, -9, 10, 17], ZHI = [-30, -10, 9, 16, 50], r = s.reputation && s.reputation[f];
  const v = Math.max(-50, Math.min(50, r ? r.score || 0 : 0)); let z = 4; for (let q = 0; q < 5; q++) if (v <= ZHI[q]) { z = q; break; }
  const fr = Math.max(0, Math.min(1, (v - ZLO[z] + 0.5) / (ZHI[z] - ZLO[z] + 1))), t = tierIdx(s, f);
  return '<div class="rbar">' + [0, 1, 2, 3, 4].map(k => '<i class="z' + (k === t ? ' on' : '') + '" style="left:' + (k * 20) + '%;background:' + ['#FF6B5A', '#FF9B70', '#A9BDD0', '#8CE7A0', '#FFD166'][k] + '"></i>').join('') +
         '<i class="mk" style="left:calc(' + (z * 20) + '% + ' + fr.toFixed(3) + ' * (20% - 2px))"></i></div>';
}
function favorWord(f, t) { return FAVOR_WORDS[f] ? FAVOR_WORDS[f][t] : ''; }
function openLadder(f) {
  const s = lastS; if (!s) return;
  const t = tierIdx(s, f), got = s.v2 ? (s.v2.gifts || 0) & (1 << f) : 0, col = FACTION_UI_COLORS[f];
  let h = '<div class="ldhead">' + insigniaSvg(f, 30) + '<div><b style="color:' + col + '">' + TX_FACTIONS[f].name.toUpperCase() + '</b><small>Only you see your ladder.</small></div></div><div class="ladder">';
  for (let k = 4; k >= 0; k--) {
    const on = k === t, past = k < t;
    let gift = '';
    if (k === 3) gift = '<div class="ldgift' + (got ? ' got' : '') + '">' + pixIcon('gift', got ? '#8CE7A0' : '#FFD166', 2) + '<span><b>' + GIFT4[f][0] + '</b> ' + GIFT4[f][1] + '</span><em>' + (got ? 'EARNED' : 'ONCE') + '</em></div>';
    if (k === 4) gift = '<div class="ldgift' + (t === 4 ? ' got' : '') + '">' + pixIcon('gift', t === 4 ? '#8CE7A0' : '#FFD166', 2) + '<span><b>' + GIFT5[f][0] + '</b> ' + GIFT5[f][1] + '</span><em>' + (t === 4 ? 'YOURS' : 'WHILE HERE') + '</em></div>';
    h += '<div class="rung' + (on ? ' on' : '') + (past ? ' past' : '') + '" style="--rc:' + FAVOR_COL[k] + '">' +
         '<div class="rtop">' + pixIcon(FAVOR_ICON[k], FAVOR_COL[k], 2) + '<b>' + (k + 1) + ' \u00b7 ' + favorWord(f, k).toUpperCase() + '</b>' + (on ? '<em>YOU</em>' : '') + '</div>' +
         '<div class="rmean">' + FAVOR_MEANS[k] + '</div>' + gift + '</div>';
  }
  h += '</div>';
  h += rivetAsk('ladder' + f, 'Climb with jobs, good talks and by hurting their rival; attacking their worlds drops you. Tier 4 gives a gift, once. Tier 5 gives an ongoing one, lost if you slip back. Top of the ladder and higher than anyone at the table: you\u2019re their Champion.');
  sheetShow('ladder' + f, h, col);
}
// ---- 3.1-dev.43 THE FINAL HOUR (tools/ui31_mockups/final_hour.html) ----
const FH_CLS = ['WARLORD', 'CONSUL', 'INFILTRATOR', 'SCOUNDREL'];
const FH_G = [  // name, when (0 with your orders, 1 at the reveal, 2 at the reveal by itself), what it does, a line
  ['HIDDEN FLEET', 0, '4 ships drop out of hyperspace into this strike the moment its fight starts.', 'They never counted the second wave.'],
  ['ORBITAL BOMBARDMENT', 0, 'The defenders lose 3 ships before this fight even starts.', 'Soften them up.'],
  ['NO RETREAT', 0, 'This strike’s first 2 losses don’t count.', 'There is no way back.'],
  ['RALLY THE LINE', 1, '+2 defenders on every world you hold.', 'Hold. The. Line.'],
  ['FINAL TREATY', 1, 'Name one of your worlds. Every strike on it turns back.', 'Signed, sealed, untouchable.'],
  ['CALL FOR PEACE', 1, 'Cancel any one strike. Ghost Strikes can’t be found.', 'Stand down. Now.'],
  ['TURNCOAT GARRISON', 0, 'Pin it to a strike on an UNCLAIMED world: up to 4 of its defenders switch sides.', 'They were never loyal.'],
  ['OLD FRIENDS', 1, 'One faction that answered your debts sends the same ships again, to your biggest strike.', 'We never forget a friend.'],
  ['INTERCEPTED ORDERS', 2, 'At the reveal you see every gambit pinned to a strike, and every Ghost Strike. Plays by itself.', 'Read them before they read you.'],
  ['SABOTAGE', 1, 'One enemy strike arrives at half strength.', 'A few loose bolts.'],
  ['FALSE FLAG', 1, 'Send one enemy strike to a different world.', 'Wrong coordinates. Oops.'],
  ['GHOST STRIKE', 0, 'This strike stays hidden at the reveal. Nothing can cancel, sabotage or redirect it.', 'What strike?'],
  ['LOADED DICE', 0, 'Every roll of this fight, you roll twice and keep the better die.', 'Lucky? Never heard of it.'],
  ['HIGH ROLLER', 0, 'When this fight starts, every 2 CR you still have becomes a ship (up to 6).', 'Everything on red.'],
  ['SMUGGLER’S RUN', 0, 'One extra strike of 3 smuggler ships, beyond your fleets. Give it a target in Last Orders.', 'Off the books.'],
  ['THE LONG CON', 1, 'Take 3 ships from the VP leader’s biggest strike and add them to your biggest.', 'They never saw it coming.']];
const FH_WHEN = ['WITH YOUR ORDERS', 'AT THE REVEAL', 'AT THE REVEAL'];
// 3.1-dev.114 (user: "jobs should always allow 3 rounds, not 'by the end of round'"): rounds left, counting this one.
function jobIn(s, due) { const n = (due | 0) - s.round + 1; return n <= 1 ? 'this round' : 'within ' + n + ' rounds'; }
function fhFinal(s) { return s.stage === 1 && s.round >= s.roundLimit; }
function fhHand(s) { const h = (s.fh && s.fh.hand) || 0, out = []; for (let g = 0; g < 16; g++) if ((h >> g) & 1) out.push(g); return out; }
function fhCard(g, opts) {
  const G = FH_G[g], o = opts || {};
  return '<div class="gc' + (o.mini ? ' mini' : '') + (o.on ? ' on' : '') + '"' + (o.tap ? ' onclick="' + o.tap + '"' : '') + '><div class="gcls">' + FH_CLS[Math.floor(g / 4)] + '</div><div class="gnm">' + G[0] + '</div>' +
         '<div class="gwhen">' + FH_WHEN[G[1]] + '</div><div class="gfx">' + G[2] + '</div>' + (o.mini ? '' : '<div class="gfl">“' + G[3] + '”</div>') + (o.price ? '<div class="gpr">' + o.price + ' CR</div>' : '') + '</div>';
}
// Home rows (THE GALAXY AND YOU)
function fhRows(s) {
  const f = s.fh, rows = []; if (!f) return rows;
  if (f.deal) rows.push('<button class="gyrow due" onclick="openFhDraw()">' + pixIcon('gift', '#FFD166', 2) + '<span><b>DRAW YOUR GAMBIT</b> ' + f.deal.length + ' card' + (f.deal.length === 1 ? '' : 's') + ', keep one</span><em>›</em></button>');
  fhHand(s).forEach(g => rows.push('<button class="gyrow" style="border-color:#FFD16688" onclick="openFhCard(' + g + ')">' + pixIcon('gift', '#FFD166', 2) + '<span><b style="color:#FFD166">YOUR GAMBIT</b> ' + FH_G[g][0] + ' · ' + FH_WHEN[FH_G[g][1]].toLowerCase() + '</span><em>›</em></button>'));
  // 3.1-dev.77 (user: "the black market should no longer be mentioned in the galaxy and you, it should just be the change to the market")
  if (fhFinal(s) && s.phase === 0) {
    const open = f.debt.some((d, k) => d[0] > 0 && !((f.called >> k) & 1));
    rows.push('<button class="gyrow' + (open ? ' due' : '') + '" onclick="openFhDebts()">' + pixIcon('owe', '#8CE7A0', 2) + '<span><b style="color:#8CE7A0">CALL IN YOUR DEBTS</b> ' + (open ? 'the factions you helped will send ships' : 'answered: ' + f.ally.reduce((a, b) => a + b, 0) + ' allied ships') + '</span><em>›</em></button>');
    rows.push('<div class="fhlast"><b>LAST CHANCE</b>Credits are worth nothing after this round, except jump passes and High Roller. Repair and reinforce your fleets now: every ship is a ship in your last strikes.</div>');
  }
  return rows;
}
let fhDrawSeen = '';
function fhAutoOpen(s) {  // the draw opens by itself once (a card flip)
  const f = s.fh; if (!f || !f.deal || typeof sheetKey === 'undefined' || sheetKey) return;
  const key = s.round + ':' + f.deal.join(','); if (fhDrawSeen === key) return;
  fhDrawSeen = key; openFhDraw();
}
let fhDrawPick = -1;
function openFhDraw() {
  const s = lastS, f = s && s.fh; if (!f || !f.deal) return sheetHide();
  const n = f.deal.length;
  sheetShow('fhdraw', '<div class="sheetSub" style="letter-spacing:3px">ROUND ' + s.round + ' OF ' + s.roundLimit + ' · THE GAMBITS ARE DEALT</div><div class="sheetTitle">DRAW YOUR GAMBIT</div>' +
    '<div class="sheetSub">' + (n === 3 ? 'You’re behind, so you draw <b>three</b> and keep one.' : n === 1 ? 'You’re leading, so you draw just one.' : 'You draw two and keep one.') + ' The leader draws one, last place three. Nobody sees what you kept.</div>' +
    rivetAsk('fhdraw' + s.round, 'Keep the one that fits your plan. WITH YOUR ORDERS cards ride with one strike in Last Orders; AT THE REVEAL cards are played when every strike is shown. You can hold up to 3.') +
    '<div class="gfan">' + f.deal.map((g, k) => '<div class="gflip" style="animation-delay:' + (k * 160) + 'ms">' + fhCard(g, { mini: true, on: fhDrawPick === k, tap: 'fhDrawPick=' + k + ';openFhDraw()' }) + '</div>').join('') + '</div>' +
    '<button class="pill" style="border-color:#FFD166;color:#FFD166"' + (fhDrawPick >= 0 || n === 1 ? '' : ' disabled') + ' onclick="fhKeep()">' + (fhDrawPick >= 0 || n === 1 ? 'KEEP ' + FH_G[f.deal[Math.max(0, fhDrawPick)]][0] : 'TAP ONE TO KEEP IT') + '</button>', '#FFD166');
}
async function fhKeep() { const k = Math.max(0, fhDrawPick); fhDrawPick = -1; sheetHide(); await post('/fh_pick?k=' + k + '&id=' + encodeURIComponent(myId)); poll(); }
function openFhCard(g) {
  sheetShow('fhcard', '<div class="sheetSub" style="letter-spacing:3px">YOUR GAMBIT</div>' + fhCard(g, {}) +
    '<div class="sheetSub" style="margin-top:10px">' + (FH_G[g][1] ? 'You play it at THE REVEAL, the moment every strike is shown. Everyone chooses at once, in secret.' : 'Pin it to one of your strikes in LAST ORDERS. It goes off when that fight starts. Nobody knows until then.') + '</div>' +
    '<button class="pill" style="border-color:var(--line);color:var(--dim)" onclick="sheetHide()">GOT IT</button>', '#FFD166');
}
// 3.1-dev.79 BUYOUT: the holder sells (keeps half) or matches (pays the same, keeps the world). No answer by next round = sold.
function openBuyout(i) {
  const s = lastS, p = s && s.planets[i]; if (!p || !p.bo) return;
  const me = s.players[mySlot], cr = p.bo[1];
  sheetShow('buyout', '<div class="sheetTitle" style="color:#FFD166">BUYOUT OFFER</div>' +
    '<div class="sheetSub">' + pTag(s, p.bo[0]) + ' offers <b>' + cr + ' CR</b> for <b>' + p.name + '</b>.</div>' +
    // 3.1-dev.114 (playtest: "its just confusing to understand where your money goes"): every Credit, row by row
    '<div class="fileBox" style="margin:8px 0"><div><span>IF YOU SELL</span><b>You get ' + Math.floor(cr / 2) + ' CR. ' + p.name + ' (' + p.vp + ' VP) goes to them. The other ' + (cr - Math.floor(cr / 2)) + ' CR leaves the game.</b></div>' +
    '<div><span>IF YOU MATCH</span><b>You pay ' + cr + ' CR (it leaves the game) and keep ' + p.name + '. Their ' + cr + ' CR goes back to them.</b></div>' +
    '<div><span>NO ANSWER</span><b>By the next round it counts as sold.</b></div></div>' +
    '<div class="biAct"><button style="border-color:#FFD166;color:#FFD166" onclick="answerBuyout(' + i + ',0)">SELL \u00b7 +' + Math.floor(cr / 2) + ' CR</button>' +
    '<button style="border-color:#5FD4FF;color:#5FD4FF"' + (me.credits >= cr ? '' : ' disabled') + ' onclick="answerBuyout(' + i + ',1)">MATCH \u00b7 ' + cr + ' CR</button></div>', '#FFD166');
}
async function answerBuyout(i, m) { sheetHide(); const r = await post('/buyout_answer?p=' + i + '&match=' + m + '&id=' + encodeURIComponent(myId)); if (r && !r.ok && r.why) notify('NOT NOW', r.why, '#FF6B5A', 4000); poll(); }
// 3.1-dev.78 BLACKMAIL (the Crimson Dawn's Family gift, used once): pick a rival -- they pay you, or the table hears their debts.
const BLACKMAIL_CR = 8;
function openBlackmail() {
  const s = lastS; if (!s) return;
  const rivals = s.players.map((p, i) => i).filter(i => i !== mySlot && s.players[i].active);
  sheetShow('blackmail', '<div class="sheetTitle" style="color:#E0475B">BLACKMAIL</div>' +
    '<div class="sheetSub">The Crimson Dawn keep files on everyone. Pick one rival: they pay you ' + BLACKMAIL_CR + ' CR, or the whole table hears their debts. Once a game.</div>' +
    '<div class="bmPick" style="border-color:#E0475B">' + rivals.map(i => '<button style="border-color:#E0475B;color:#E8EEF5" onclick="doBlackmail(' + i + ')">' + pTag(s, i) + '</button>').join('') + '</div>' +
    '<div class="biAct"><button class="ghost" onclick="sheetHide()">NOT NOW</button></div>', '#E0475B');
}
async function doBlackmail(i) { sheetHide(); const r = await post('/blackmail?t=' + i + '&id=' + encodeURIComponent(myId)); if (r && !r.ok && r.why) notify('NOT NOW', r.why, '#FF6B5A', 4000); poll(); }
let bmAskSeen = '';
function openBlackmailAnswer() {
  const s = lastS, v = s && s.v2; if (!v || v.bmAsk === undefined) return;
  const me = s.players[mySlot], debts = (s.promises || []).length;
  sheetShow('bmask', '<div class="sheetTitle" style="color:#E0475B">THEY HAVE YOUR FILE</div>' + grudgeHolo(s, 3) +
    '<div class="sheetSub">' + pTag(s, v.bmAsk) + ' wants ' + BLACKMAIL_CR + ' CR. Refuse, and the whole table hears ' + (debts ? 'your ' + debts + ' debt' + (debts === 1 ? '' : 's') + ' and which worlds they sit on' : 'what\u2019s in your file (there isn\u2019t much)') + '.</div>' +
    '<div class="biAct"><button style="border-color:#FFD166;color:#FFD166"' + (me.credits >= BLACKMAIL_CR ? '' : ' disabled') + ' onclick="answerBlackmail(1)">PAY ' + BLACKMAIL_CR + ' CR</button><button class="ghost" onclick="answerBlackmail(0)">LET THEM TALK</button></div>', '#E0475B');
}
async function answerBlackmail(pay) { sheetHide(); const r = await post('/blackmail_answer?pay=' + pay + '&id=' + encodeURIComponent(myId)); if (r && !r.ok && r.why) notify('NOT NOW', r.why, '#FF6B5A', 4000); poll(); }
// THEIR FILE inside a rival's kit (the Infiltrator's strength; the Slicer lot)
function fileHtml(s, slot) {
  const v = s.v2 || {}, F = v.file;
  if (F && F.s === slot) {
    const rows = [['CREDITS', F.cr + ' CR'],
                  ['DEBTS', F.d.length ? F.d.map(d => FACTION_SHORT_NAMES[d[0]] + ' \u00b7 ' + wName(s, d[1]) + ' \u00b7 due R' + d[2]).join('<br>') : 'none'],
                  ['GRUDGE', F.g ? 'the ' + FNAMES[F.g[0]] + ' strike them in round ' + F.g[1] : 'none'],
                  ['JOB', F.job ? 'take ' + wName(s, F.job[1]) + ' for the ' + FNAMES[F.job[0]] : 'none'],
                  ['PATROLS', s.planets.map((p, i) => i).filter(i => s.planets[i].owner === slot && s.planets[i].pt > 0).map(i => wName(s, i)).join(', ') || 'none']];
    if (F.sl) rows.push(['DEFENDERS', F.def.map(d => wName(s, d[0]) + ' ' + d[1]).join(' \u00b7 ') || 'no worlds']);
    return '<div class="kitGrp" style="color:#C8364A">THEIR FILE \u00b7 ' + (F.sl ? 'SLICED THIS ROUND' : 'PULLED') + '</div><div class="fileBox">' + rows.map(r => '<div><span>' + r[0] + '</span><b>' + r[1] + '</b></div>').join('') + '</div>';
  }
  if (v.fileCan) return '<button class="pill" style="border-color:#C8364A;color:#C8364A" onclick="pickFile(' + slot + ')">PULL THEIR FILE<small>Credits, debts, grudges, job, patrols. Your next pull is in 2 rounds.</small></button>';
  return '';
}
async function pickFile(i) { await post('/file_pick?t=' + i + '&id=' + encodeURIComponent(myId)); await poll(); openKit(i); }
// 3.1-dev.77 THE BLACK MARKET (canvas boards 3 + 4, user: "looks amazing ... I love that it moves"; "it shouldn't feel affiliated
// with crimson dawn"): when it opens, the Home MARKETS strip turns into its door, and the page is a smuggler's den -- a broker
// with no faction, the lots on top (two new late-game lots, two big-ticket items, two gambits), the normal shops underneath.
const LOTS = [['SMUGGLED TROOPS', 9, '+3 defenders on one of your worlds, even past its limit.', 'They came in the cargo hold.', 'HOT', 'CREW'],
              ['FORTIFICATIONS', 10, 'One of your worlds can hold 2 more defenders, for good.', 'Walls, mines, turrets. No questions.', 'RARE', 'TECH'],
              ['SABOTEUR', 10, 'The next world you attack loses 2 defenders before the first roll.', 'Someone left a door open.', '', 'CREW'],
              ['TURNCOAT', 5, 'At the next world you attack, one of its defenders fights for you.', 'Everyone has a price.', 'STOLEN', 'CREW'],
              ['SECOND CHANCE', 9, 'The first time one of your fleets drops to 2 ships in a fight, 2 more jump in.', 'Insurance, of a kind.', '', 'HULL'],
              ['SLICER', 8, 'This round, read one rival\u2019s file: Credits, debts, grudges, job, patrols, and every defender they have.', 'Every lock has a back door.', 'HOT', 'TECH'],
              // 3.1-dev.88 cheap, late-game (user: "more items on the lower end of price")
              ['FORGED ORDERS', 5, 'A fleet that already fought this round attacks again, from where it is: don\u2019t move it on the board. Won, lost or withdrew, as long as it has ships.', 'Signed by someone important.', 'CHEAP', 'DOCS'],
              ['HIRED BLADE', 4, '+1 ship in your next battle, attacking or defending.', 'Paid up front.', 'CHEAP', 'CREW'],
              ['CLEAN GETAWAY', 3, 'Your next withdraw: no parting shot, and your fleet isn\u2019t shaken.', 'The engine was always running.', 'CHEAP', 'HULL'],
              ['INSIDE MAN', 4, 'See exactly how many defend one world, right now.', 'He counts for a living.', 'CHEAP', 'CREW'],
              ['DECOY BEACON', 4, 'The next attack on one of your worlds meets one extra defender: a ghost on their scopes.', 'Nobody shoots at nothing twice.', 'CHEAP', 'TECH'],
              ['FIELD REFIT', 5, 'Repair every ship in one fleet, whenever you choose.', 'Parts of uncertain origin.', 'CHEAP', 'HULL']];
const LOT_KEEPS = [2, 3, 4, 6, 7, 8, 9, 10, 11], LOT_TAP = { 6: 'fleet', 9: 'world', 11: 'fleet' };  // 3.1-dev.88 THE CARGO HOLD
let bmMode = false, bmPick = null;  // bmPick = ['lot' | 'big', k] while choosing a world
function bmOpenNow(s) { return !!(s && s.bm && s.bm.o); }  // 3.1-dev.88: the Final Hour, or while the Dawn grips the galaxy
function cargoPick(k) { const it = ((lastS.bm || {}).hold || [])[k]; if (!LOT_TAP[it]) return; bmPick = ['cargo', k]; shownHtml = null; render(lastS); }
async function cargoUse(k, a) {
  bmPick = null;
  const r = await post('/cargo_use?k=' + k + '&a=' + a + '&id=' + encodeURIComponent(myId));
  if (r && !r.ok && r.why) notify('NOT NOW', r.why, '#FF6B5A', 4000);
  shownHtml = null; await poll();
}
function cargoPickHtml(s) {
  const k = bmPick[1], it = ((s.bm || {}).hold || [])[k], back = '<button class="ghost" onclick="bmPick=null;render(lastS)">BACK</button></div>';
  if (LOT_TAP[it] === 'world') return '<div class="bmPick"><b>WHICH WORLD?</b>' + s.planets.map((p, i) => i).filter(i => s.planets[i].owner !== mySlot).map(i => '<button onclick="cargoUse(' + k + ',' + i + ')">' + wName(s, i) + '</button>').join('') + back;
  return '<div class="bmPick"><b>WHICH FLEET?</b>' + [0, 1, 2].map(f => '<button onclick="cargoUse(' + k + ',' + f + ')">FLEET ' + (f + 1) + '</button>').join('') + back;
}
function openBlackMarket() { bmMode = true; bmPick = null; shownHtml = null; poll(); }
function closeBlackMarket() { bmMode = false; bmPick = null; poll(); }
// 3.1-dev.86 THE BROKER as a pixel sprite (user: "make the black market guy a pixel sprite like the rest"; "dark and mysterious"):
// drawn by tools/art/portraits_v2.html (preset brokerA: hood, glowing amber eyes, a respirator) -- s = 24x28 like the characters, l = 32x40.
const BROKER_ART = {"s":{"w":24,"h":28,"p":"191613110E0D070505443F392E2A2612100F0000008474656E5E5251433C302722703600281F1B1D1614FFB040BF5D006E727F5A5F6A42474FE8A03C2E32382C3137382C277262475A4A363B2E222A24200D0B0A","x":"0000000000000000000000000000000000000000000000000000000000010101020202020202030303030300000000000000000001040404050505050505060606060603000000000000000104040404050505050505060606060606030000000000010404040404050505050505060606060606060300000000010404040404050505050505060606060606060300000000010404040404050505050505060606060606060300000000010404040404050505050505060606060606060300000000010404040404050505050505060606060606060300000000010404070707070707070707070707070706060300000000010404040808080809090909090a0a0a0606060300000000020505050808080809090909090a0a0a06060603000000000205050508080b0c0d09090d0c0e0a0a0606060300000000020505050808080f1009090a0f0a0a0a060606030000000002050505080808080909090a090a0a0a060606030000000205050511111111121213131213131313130606060300000205050505111111121213131213131313060606060300000205050505111114121215151213141313060606060300020505050505111111121215151213131313060606060603020505050505051111121215151213131306060606060603020505050505050516161615151616160606060606060603010404040404040405171615151617060606060606060603040404040404040405171717171717060606060606060606040404040404181818181919191a1a1a1a1a060606060606040404040404181818181919191a1a1a1a1a060606060606040404040404040405181919191a1a0606060606060606061b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1c1c1c1c1c1c1c071b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1b1c1c1c1c1c1c1c07"},"l":{"w":32,"h":40,"p":"191613443F392E2A2612100F0705050000008474656E5E5251433C110E0D3027221D1614281F1B140E0D382C27F0E8E0FFB040C8C0B86E727F5A5F6A42474FE8A03C2E32382C31377262475A4A363B2E222A24200D0B0A","x":"00000000010202020202030303030303030303040404040404040405000000000000000001020202020203030303030303030304040404040404040500000000000000000102020202020303030303030303030404040404040404050000000000000001020202020202030303030303030303040404040404040404050000000000000102020202020203030303030303030304040404040404040405000000000000010202020202020303030303030303030404040404040404040500000000000001020202020202030303030303030303040404040404040404050000000000000102020202020203030303030303030304040404040404040405000000000000010202020202020303030303030303030404040404040404040500000000000001020202060606060606060606060606060606060606040404050000000000000102020206060606060606060606060606060606060604040405000000000000010202020207070707070808080808080809090909040404040500000000000001020202020707070707080808080808080909090904040404050000000000000a0303030307070b0c0c0d0d08080d0e0e0c0c090904040404050000000000000a03030303070707090908080808080f0f0909090904040404050000000000000a030303030707071011090808080809111209090904040404050000000000000a030303030707070707080808080808080909090904040404050000000000000a0303030307070707070808080809080809090909040404040500000000000a03030303030707070707080808070908080909090f040404040405000000000a030303031313131313141414151514141515151515150404040405000000000a030303030313131313141414151514141515151515040404040405000000000a0303030303131313161414141717141415161515150404040404050000000a030303030303131313131414141717141415151515150404040404040500000a030303030303131313131414141717141415151515150404040404040500000a030303030303031313131414141717141415151515040404040404040500000a0303030303030313131314141417171414151515150404040404040405000a030303030303030303131314141417171414151515040404040404040404050a0303030303030303030318181818171718181818040404040404040404040501020202020202020202030618181817171818180604040404040404040404050102020202020202020203030f0f0f17170f0f0f0404040404040404040404050202020202020202020203030f0f0f0f0f0f0f0f04040404040404040404040402020202020202020202030309090909090f0f0f04040404040404040404040402020202020202020202191919191a1a1a1a1b1b1b1b0404040404040404040402020202020202021919191919191a1a1a1a1b1b1b1b1b1b040404040404040402020202020202021919191919191a1a1a1a1b1b1b1b1b1b040404040404040402020202020202020202191919191a1a1a1a1b1b1b1b0404040404040404040402020202020202020202030303030306060303040404040404040404040404041c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1d1d1d1d1d1d1d1d1d06061c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1d1d1d1d1d1d1d1d1d06061c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1d1d1d1d1d1d1d1d1d0606"}};
const BROKER_URLS = {};
function brokerURL(size, scale, holo) {
  const key = size + scale + (holo ? 'h' : ''); if (BROKER_URLS[key]) return BROKER_URLS[key];
  const a = BROKER_ART[size], cv = document.createElement('canvas'); cv.width = a.w * scale; cv.height = a.h * scale;
  const ctx = cv.getContext('2d'), col = i => a.p.substr((i - 1) * 6, 6);
  const lum = i => { const n = parseInt(col(i), 16); return (30 * (n >> 16) + 59 * ((n >> 8) & 255) + 11 * (n & 255)) / 100; };
  let lo = 255, hi = 0; if (holo) for (let i = 1; i <= a.p.length / 6; i++) { lo = Math.min(lo, lum(i)); hi = Math.max(hi, lum(i)); }
  for (let k = 0; k < a.w * a.h; k++) {
    const i = parseInt(a.x.substr(k * 2, 2), 16); if (!i) continue;
    ctx.fillStyle = holo ? HOLO_BLUES[Math.round((lum(i) - lo) / Math.max(1, hi - lo) * 4)] : '#' + col(i);
    ctx.fillRect((k % a.w) * scale, Math.floor(k / a.w) * scale, scale, scale);
  }
  if (holo) { ctx.globalCompositeOperation = 'source-atop'; ctx.fillStyle = 'rgba(3,8,13,0.4)'; for (let y = 0; y < cv.height; y += 3) ctx.fillRect(0, y, cv.width, 1); }
  return (BROKER_URLS[key] = cv.toDataURL());
}
// 3.1-dev.84 (user: "the market is at the bottom of the screen so if you don't scroll it is hard to see. i don't think it should move, I
// just think we need to telegraph that"): while the door is off-screen, a small chip at the bottom says so; a tap scrolls to it.
let bmObs = null;
function bmHintSync() {
  const door = document.querySelector('.bmDoor');
  let chip = document.getElementById('bmHint');
  if (!door) { if (chip) chip.remove(); if (bmObs) { bmObs.disconnect(); bmObs = null; } return; }
  if (!chip) { chip = document.createElement('button'); chip.id = 'bmHint'; chip.innerHTML = '<i class="blink"></i>BLACK MARKET OPEN <b>\u2193</b>';
               chip.onclick = () => { const d = document.querySelector('.bmDoor'); if (d) d.scrollIntoView({ behavior: reducedMotion() ? 'auto' : 'smooth', block: 'center' }); }; document.body.appendChild(chip); }
  if (bmObs && bmObs.__door === door) return;
  if (bmObs) bmObs.disconnect();
  if (!('IntersectionObserver' in window)) return;
  bmObs = new IntersectionObserver(es => { const c = document.getElementById('bmHint'); if (c) c.classList.toggle('hide', es[0].isIntersecting || sheetKey !== null); }, { threshold: 0.4 });
  bmObs.__door = door; bmObs.observe(door);
}
function bmDoorHtml(s) {
  const f = s.fh || {}, names = [];
  ((s.bm && s.bm.lots) || []).forEach(k => { if (k >= 0) names.push(LOTS[k][0]); });
  (f.bigOff || []).forEach(k => { if (k >= 0 && f.big < 0) names.push(BIG[k][0]); });
  return '<button class="bmDoor" onclick="openBlackMarket()" aria-label="Open the black market"><i class="hz t"></i><i class="hz b"></i><i class="sweep"></i>' +
         '<span class="bmHolo"><img class="pixel" src="' + brokerURL('s', 2, true) + '" alt=""></span><span class="bmDoorTx"><small><i class="blink"></i>THE MARKETS WENT DARK</small><span><b>BLACK MARKET</b><em>ENTER</em></span></span>' +
         '<span class="bmTiles">' + names.slice(0, 4).map(n => '<span>' + n + '</span>').join('') + '</span></button>';
}
function bmCrate(code, tag, name, what, btn) {
  return '<div class="bmCrate"><i class="hz t"></i><div class="bmCode"><span>' + code + '</span><span class="tag">' + (tag || '') + '</span></div><b>' + name + '</b><small>' + what + '</small>' + btn + '</div>';
}
function bmPageHtml(s) {
  const f = s.fh || {}, b = s.bm || { o: 0, lots: [], got: 0, hold: [] }, me = s.players[mySlot], full = s.fh ? fhHand(s).length >= 3 : false;
  let h = '<div class="bm"><div class="bmTop"><span><i class="blink"></i>CHANNEL 7-OBSIDIAN \u00b7 ENCRYPTED</span><span>BAY 31 \u00b7 LOWER DOCKS</span></div>' +
          '<div class="bmHead"><span class="bmHolo"><img class="pixel" src="' + brokerURL('l', 3, false) + '" alt="The broker"></span><div><b>BLACK MARKET</b><q>You didn\u2019t get this from me. Nobody gets anything from me.</q><small>THE BROKER \u00b7 NO FACTION \u00b7 NO RECEIPTS \u00b7 ' + me.credits + ' CR</small></div></div>';
  if (bmPick && bmPick[0] === 'lotp') {
    const rivals = s.players.map((p, i) => i).filter(i => i !== mySlot && s.players[i].active);
    h += '<div class="bmPick"><b>WHOSE FILE?</b>' + rivals.map(i => '<button onclick="bmBuy(\'lot\',' + bmPick[1] + ',' + i + ')">' + pTag(s, i) + '</button>').join('') + '<button class="ghost" onclick="bmPick=null;render(lastS)">BACK</button></div>';
  } else if (bmPick && bmPick[0] === 'cargo') {
    h += cargoPickHtml(s);
  } else if (bmPick) {
    const mine = s.planets.map((p, i) => i).filter(i => s.planets[i].owner === mySlot);
    h += '<div class="bmPick"><b>WHICH WORLD?</b>' + (mine.length ? mine.map(i => '<button onclick="bmBuy(\'' + bmPick[0] + '\',' + bmPick[1] + ',' + i + ')">' + wName(s, i) + '</button>').join('') : '<small>You hold no worlds.</small>') +
         '<button class="ghost" onclick="bmPick=null;render(lastS)">BACK</button></div>';
  }
  const WHEN = k => k <= 1 ? 'USE NOW' : k === 5 ? 'THIS ROUND' : 'INTO YOUR HOLD';  // 3.1-dev.84 / dev.88: the hold keeps the rest
  h += b.o ? '<div class="bmSec">FOR NOW <small>a fresh offer every round \u00b7 one is always cheap</small></div><div class="bmGrid">'
           : '<div class="bmSec">CLOSED <small>it opens in the Final Hour, or while the Dawn grips the galaxy</small></div><div class="bmGrid">';
  (b.lots || []).forEach((k, j) => {
    if (k < 0) return;
    const L = LOTS[k], got = (b.got >> k) & 1, keep = LOT_KEEPS.includes(k), world = k <= 1;
    const who = k === 5;  // the Slicer picks a rival
    const btn = got ? '<button disabled>' + (keep ? 'IN YOUR HOLD' : who ? 'SLICED' : 'BOUGHT') + '</button>'
                    : '<button' + (me.credits >= L[1] ? '' : ' disabled') + ' onclick="' + (world ? 'bmPick=[\'lot\',' + j + '];render(lastS)' : who ? 'bmPick=[\'lotp\',' + j + '];render(lastS)' : 'bmBuy(\'lot\',' + j + ',-1)') + '">' + L[1] + ' CR</button>';
    h += bmCrate(WHEN(k), L[4], L[0], L[2], btn);
  });
  const hold = (b.hold || []).filter(x => x >= 0);
  h += '</div><div class="bmSec">YOUR CARGO HOLD <small>' + hold.length + ' / 3 \u00b7 kept until used</small></div>' +
       (hold.length ? '<div class="bmHold">' + (b.hold || []).map((x, k) => x < 0 ? '' : '<div class="bmHi"><b>' + LOTS[x][0] + '</b><small>' + LOTS[x][2] + '</small>' +
          (LOT_TAP[x] ? '<button onclick="cargoPick(' + k + ')">USE</button>' : '<em>FIRES ON ITS OWN</em>') + '</div>').join('') + '</div>'
                    : '<p class="bmSays">EMPTY \u00b7 LOTS YOU KEEP WAIT HERE</p>');
  h += s.fh ? '<div class="bmSec fh">FOR THE FINAL HOUR <small>they only work in the last battle</small></div><div class="bmGrid">' : '<div>';
  (f.bigOff || []).forEach((b, k) => {
    if (b < 0) return;
    const owned = f.big === b, other = f.big >= 0 && !owned;
    const btn = owned ? '<button disabled>YOURS' + (b === 1 && f.bigArg >= 0 ? ' \u00b7 ' + wName(s, f.bigArg) : '') + '</button>' : other ? '<button disabled>ONE A GAME</button>'
              : '<button' + (me.credits >= BIG[b][1] ? '' : ' disabled') + ' onclick="' + (b === 1 ? 'bmPick=[\'big\',' + k + '];render(lastS)' : 'bmBuy(\'big\',' + k + ',-1)') + '">' + BIG[b][1] + ' CR</button>';
    h += bmCrate('FINAL HOUR \u00b7 ONE A GAME', 'ANNOUNCED', BIG[b][0], BIG[b][2], btn);
  });
  (f.bm || []).forEach((g, k) => {
    if (g < 0) return;
    h += bmCrate('FINAL HOUR \u00b7 GAMBIT', 'SECRET', FH_G[g][0], FH_G[g][2],
                 '<button' + (me.credits >= f.bmCr && !full ? '' : ' disabled') + ' onclick="bmBuy(\'gam\',' + k + ',-1)">' + (full ? 'HANDS FULL' : f.bmCr + ' CR') + '</button>');
  });
  h += '</div><p class="bmSays">BIG TICKETS ARE ANNOUNCED ON THE TABLE \u00b7 LOTS AND GAMBITS STAY QUIET</p>';
  h += rivetAsk('bm' + s.round, 'The black market opens near the end, or while the Dawn grips the galaxy. Troops and fortifications go on one of your worlds now, and the Slicer reads a file this round. Everything else waits in your cargo hold (3 at most) until its moment, or until you tap USE. One big-ticket item a game, and everyone hears when you buy it.');
  const stalls = (s.v2 && s.v2.stalls) || [];
  if (s.phase === 0 && s.mkt !== 0 && stalls.length) {
    h += '<div class="bmShops"><small>THE SHOPS ARE STILL OPEN</small><div>' + stalls.map(st => '<button onclick="closeBlackMarket();openMarkets()" style="--sc:' + FACTION_UI_COLORS[st.f] + '"' + (st.shut ? ' class="shut"' : '') + '><b>' + FACTION_SHORT_NAMES[st.f] + '</b><span>' +
         (st.shut ? 'Closed' : st.it.filter(x => x[0] >= 0).map(x => (s.v2.goods[x[0]] || [''])[0]).join(', ')) + '</span></button>').join('') + '</div></div>';
  }
  h += '<button class="back" onclick="closeBlackMarket()">BACK</button></div>';
  return h;
}
async function bmBuy(kind, k, w) {
  bmPick = null;
  const id = encodeURIComponent(myId);
  const r = kind === 'lot' ? await post('/bm_lot?k=' + k + (w >= 0 ? '&w=' + w : '') + '&id=' + id)
          : kind === 'big' ? await post('/fh_buy_big?k=' + k + (w >= 0 ? '&w=' + w : '') + '&id=' + id)
          : await post('/fh_buy?k=' + k + '&id=' + id);
  if (r && !r.ok && r.why) notify('NOT NOW', r.why, '#FF6B5A', 4000);
  shownHtml = null; await poll();
}
// 3.1-dev.53 #23 BIG-TICKET ITEMS: two random ones per player, buy ONE a game, every purchase announced
const BIG = [['CAPITAL SHIP', 30, '+4 ships join your biggest strike in the Final Hour, even past a fleet\u2019s size.', 'Decommissioned. Mostly.'],
             ['PLANETARY SHIELD', 25, 'Pick a world you hold: in the finale its first 3 hits don\u2019t count.', 'The best defense money can buy.'],
             ['MERCENARY ARMADA', 20, '4 hired ships: an extra strike of their own in your Last Orders.', 'Loyal until the money runs out.'],
             ['HYPERSPACE CHARTS', 15, 'No jump passes: in Last Orders every world is in reach, free.', 'Every route the Empire forgot.'],
             ['SPY NETWORK', 45, 'At the reveal you see every gambit pinned to a strike and every hidden strike.', 'Everyone talks. For a price.']];
function bigCard(b, opts) {
  const B = BIG[b], o = opts || {};
  return '<div class="gc mini' + (o.on ? ' on' : '') + '" style="border-color:#C9A227"><div class="gcls" style="color:#C9A227">BIG TICKET \u00b7 ONE A GAME</div><div class="gnm">' + B[0] + '</div><div class="gfx">' + B[2] + '</div>' +
         '<div class="gfl">\u201C' + B[3] + '\u201D</div>' + (o.price ? '<div class="gpr">' + B[1] + ' CR</div>' : '') + '</div>';
}
let bigShieldPick = false;
function openFhMarket() {
  const s = lastS, f = s && s.fh; if (!f) return;
  const me = s.players[mySlot], full = fhHand(s).length >= 3;
  let rack = '<div class="fhgrp" style="color:#C9A227">BIG TICKET \u00b7 EVERY PURCHASE IS ANNOUNCED</div>';
  if (f.big >= 0) rack += bigCard(f.big, { on: true }) + '<div class="sheetSub">Yours' + (f.big === 1 && f.bigArg >= 0 ? ', shielding ' + wName(s, f.bigArg) : '') + '. One a game.</div>';
  else if (bigShieldPick) {
    const mine = s.planets.map((p, i) => i).filter(i => s.planets[i].owner === mySlot);
    const k = f.bigOff.indexOf(1);
    rack += '<div class="sheetSub">Which world does the shield cover?</div><div class="fhworlds">' + (mine.length ? mine.map(i => '<button onclick="fhBuyBig(' + k + ',' + i + ')">' + wName(s, i) + '</button>').join('') : '<small>You hold no worlds to shield.</small>') +
            '</div><button class="back" onclick="bigShieldPick=false;openFhMarket()">BACK</button>';
  } else rack += f.bigOff.map((b, k) => b < 0 ? '' : bigCard(b, { price: true }) + '<button class="pill" style="border-color:#C9A227;color:#C9A227;min-height:48px;margin:6px 0 12px"' + (me.credits >= BIG[b][1] ? '' : ' disabled') +
                ' onclick="' + (b === 1 ? 'bigShieldPick=true;openFhMarket()' : 'fhBuyBig(' + k + ',-1)') + '">BUY \u00b7 ' + BIG[b][1] + ' CR</button>').join('');
  sheetShow('fhbm', '<div class="sheetSub" style="letter-spacing:3px">ONLY YOU SEE THESE</div><div class="sheetTitle">THE BLACK MARKET</div>' +
    rivetAsk('fhbm' + s.round, 'Credits are worth nothing after the last round, so this is where they go. Gambits here are other classes\u2019 tricks. The big-ticket item is once a game, and the whole table hears when you buy it.') +
    '<div class="sheetSub">Two gambits from other classes, just for you. Open until your Last Orders. You have ' + me.credits + ' CR.</div>' +
    f.bm.map((g, k) => g < 0 ? '' : fhCard(g, { mini: true, price: f.bmCr }) + '<button class="pill" style="border-color:#FFD166;color:#FFD166;min-height:48px;margin:6px 0 12px"' + (me.credits >= f.bmCr && !full ? '' : ' disabled') + ' onclick="fhBuy(' + k + ')">' + (full ? 'YOUR HANDS ARE FULL' : 'BUY · ' + f.bmCr + ' CR') + '</button>').join('') +
    rack + '<button class="pill" style="border-color:var(--line);color:var(--dim)" onclick="bigShieldPick=false;sheetHide()">NOT TODAY</button>', '#FFD166');
}
async function fhBuyBig(k, w) {
  bigShieldPick = false;
  const r = await post('/fh_buy_big?k=' + k + (w >= 0 ? '&w=' + w : '') + '&id=' + encodeURIComponent(myId));
  if (r && !r.ok && r.why) notify('NOT NOW', r.why, '#FF6B5A', 4000);
  await poll(); openFhMarket();
}
async function fhBuy(k) { await post('/fh_buy?k=' + k + '&id=' + encodeURIComponent(myId)); await poll(); openFhMarket(); }
const FH_DEBT_WORD = ['Marked', 'Wary', 'Neutral', 'Friend', 'Family', 'Champion'];
function openFhDebts() {
  const s = lastS, f = s && s.fh; if (!f) return;
  const me = s.players[mySlot];
  let h = '<div class="sheetSub" style="letter-spacing:3px">THE LAST PREPARE</div><div class="sheetTitle" style="color:#8CE7A0">CALLING IN DEBTS</div>' +
          rivetAsk('fhdebts', 'Factions that like you send ships, free: a Friend sends 1, Family 2, their Champion 3. Neutral factions owe you nothing. The ones that dislike you would rather you lose.') +
          '<div class="sheetSub">The factions you helped answer your call. Their ships join your Last Orders: put them on a strike, or they guard your best world.</div>';
  for (let k = 0; k < 4; k++) {
    const d = f.debt[k], n = d[0], cr = d[1], called = (f.called >> k) & 1, col = FACTION_UI_COLORS[k];
    const ti = tierIdx(s, k), word = n === 3 ? FH_DEBT_WORD[5] : favorWord(k, ti);  // 3.1-dev.74: debts are free -- nobody sells ships
    h += '<div class="fhdebt" style="border-color:' + (called ? col : 'var(--line)') + (n ? '' : ';border-style:dashed') + '"><div><b style="color:' + col + '">' + FACTION_SHORT_NAMES[k] + '</b><small>' + word + '</small></div>';
    if (called) h += '<span style="color:#8CE7A0">+' + f.ally[k] + ' ship' + (f.ally[k] === 1 ? '' : 's') + ' answered</span>';
    else if (!n) h += ti >= 2 ? '<span style="color:var(--dim)">they owe you nothing</span>' : '<span style="color:#FF6B5A">they’d rather you lose</span>';
    else h += '<button onclick="fhDebt(' + k + ',' + n + ')">CALL · +' + n + ' FREE</button>';
    h += '</div>';
  }
  h += '<button class="pill" style="border-color:var(--line);color:var(--dim)" onclick="sheetHide()">DONE</button>';
  sheetShow('fhdebt', h, '#8CE7A0');
}
async function fhDebt(f, n) { await post('/fh_debt?f=' + f + '&n=' + n + '&id=' + encodeURIComponent(myId)); await poll(); openFhDebts(); }

// LAST ORDERS (the final Movement)
let fhOrd = null, fhOrdWhy = '';
function fhOrdLoad(s) { const o = s.fh.ord; if (!fhOrd || fhOrd.r !== s.round) fhOrd = { r: s.round, t: o.t.slice(), gu: o.gu.slice(), al: o.al.slice(), h: (o.h || [0, 0, 0, 0, 0]).slice(), g: o.g.slice() }; return fhOrd; }
async function fhSend() {
  const o = fhOrd; if (!o) return;
  const r = await post('/fh_orders?t=' + o.t.join(',') + '&gu=' + o.gu.join(',') + '&al=' + o.al.join(',') + '&h=' + o.h.join(',') + '&g=' + o.g.join(',') + '&id=' + encodeURIComponent(myId));
  fhOrdWhy = r && !r.ok ? (r.why || 'Not allowed.') : '';
  if (fhOrdWhy) fhOrd = null;  // back to what the table has
  poll();
}
function fhReach(s, w) { return ((s.fh.reach >>> w) & 1) === 1; }
function fhOrdersHtml(s, me) {
  // 3.1-dev.116 LAST ORDERS, CLEANED UP (canvas board 29; user: "the last orders page is the janky one, and pinning gambits is
  // confusing"): your Credits up top; one card per fleet = where it strikes + its total ships; under a strike, HIRED SHIPS
  // (the cash-in) and a GAMBIT slot that opens a picker (no more cycling); reveal gambits are just listed -- you're asked at the reveal.
  const f = s.fh, o = fhOrdLoad(s), hand = fhHand(s), locked = me.lockedIn, HC = (f.hire || [3, 5])[0], HM = (f.hire || [3, 5])[1];
  const allies = f.ally.reduce((a, b) => a + b, 0), used = o.al.reduce((a, b) => a + b, 0);
  const jumps = o.t.filter(w => w >= 0 && !fhReach(s, w)).length * f.jump;
  const hires = o.t.reduce((a, w, k) => a + (w >= 0 ? (o.h[k] || 0) : 0), 0) * HC, left = me.credits - jumps - hires;
  let h = '<div class="loHead"><div><div class="fhkick" style="text-align:left;margin:0;color:#FF6B5A">ROUND ' + s.round + ' OF ' + s.roundLimit + ' \u00b7 ' + kw('LAST ORDERS') + ' \u00b7 SECRET</div>' +
          '<div class="fhbig" style="text-align:left">ONE STRIKE PER FLEET</div></div>' +
          '<div class="dfCr"><small>CREDITS</small><b>' + left + '</b><small>' + (jumps + hires ? me.credits + ' \u2212 ' + (jumps + hires) : 'to spend') + '</small></div></div>' +
          '<div class="fhsub">Aim every fleet at a world, or keep it home on guard. Nobody sees your orders until the reveal.</div>';
  const pinnable = hand.filter(g => FH_G[g][1] === 0 && g !== 14);
  const card = k => {
    const ships = k < 3 ? f.ships[k] : k === 3 ? 3 : 4, w = o.t[k];
    if (k < 3 && !ships) return '<div class="loCard dead"><div class="fhfl">FLEET<b>' + (k + 1) + '</b></div><div class="fhtg"><b style="color:var(--dim)">OUT OF THE FIGHT</b><small>no ships</small></div></div>';
    const tgt = w >= 0 ? '<b>' + wName(s, w).toUpperCase() + '</b><small style="color:' + (fhReach(s, w) ? '#8CE7A0' : '#FFB347') + '">' + (s.planets[w].owner >= 0 ? feedWho(s, s.planets[w].owner) + ' \u00b7 ' : 'unclaimed \u00b7 ') + (fhReach(s, w) ? 'in reach \u00b7 free' : 'jump pass \u00b7 ' + f.jump + ' CR') + '</small>'
                       : k === 3 ? '<b style="color:var(--dim)">NO TARGET YET</b><small>tap to aim it</small>'
                       : '<b style="color:#5FD4FF">GUARDS ' + (o.gu[k] >= 0 ? wName(s, o.gu[k]) : 'YOUR BEST WORLD').toUpperCase() + '</b><small>tap to send it at a world instead</small>';
    const total = ships + (o.al[k] || 0) + (w >= 0 ? (o.h[k] || 0) : 0);
    let x = '<div class="loCard' + (w >= 0 ? ' on' : '') + '"><div class="fhfl' + (k === 3 ? ' long' : '') + '">' + (k < 3 ? 'FLEET<b>' + (k + 1) + '</b>' : k === 3 ? 'SMUGGLERS<b>3</b>' : 'ARMADA<b>4</b>') + '</div>' +
            '<button class="loTg"' + (locked ? ' disabled' : '') + ' onclick="openFhTarget(' + k + ')">' + tgt + '</button>' +
            '<div class="fhfl">SHIPS<b style="color:#8CE7A0">' + total + '</b></div>';
    if (w >= 0) {
      const g = o.g[k], hn = o.h[k] || 0;
      x += '<div class="loRow"><div class="loHire"><span><b>HIRED SHIPS</b><small>' + HC + ' CR each</small></span>' +
           '<button aria-label="One hired ship fewer"' + (locked || !hn ? ' disabled' : '') + ' onclick="fhHire(' + k + ',-1)">\u2212</button><b>' + hn + '</b>' +
           '<button class="plus" aria-label="One more hired ship"' + (locked || hn >= HM || left < HC ? ' disabled' : '') + ' onclick="fhHire(' + k + ',1)">+</button></div>' +
           (pinnable.length ? '<button class="loGam' + (g >= 0 ? ' on' : '') + '"' + (locked ? ' disabled' : '') + ' onclick="openFhGambit(' + k + ')">' +
             (g >= 0 ? '<b>' + FH_G[g][0] + '</b><small>goes off when this fight starts</small>' : '<b>+ ADD A GAMBIT</b><small>' + pinnable.length + ' you hold fit a strike</small>') + '</button>' : '') + '</div>';
    }
    if (allies && !locked) x += '<div class="fhally"><span>allied ships with it</span><button onclick="fhAlly(' + k + ',-1)">\u2212</button><b>' + o.al[k] + '</b><button onclick="fhAlly(' + k + ',1)">+</button></div>';
    return x + '</div>';
  };
  [0, 1, 2].forEach(k => { h += card(k); });
  if (hand.includes(14) || o.t[3] >= 0) h += card(3);
  if (f.big === 2 || (o.t[4] !== undefined && o.t[4] >= 0)) h += card(4);  // 3.1-dev.53 the Mercenary Armada
  if (allies) h += '<div class="fhsub">' + allies + ' allied ship' + (allies === 1 ? '' : 's') + ' from your debts \u00b7 ' + (allies - used) + ' left to place (they guard your best world)</div>';
  hand.filter(g => FH_G[g][1] > 0).forEach(g => {
    h += '<div class="loSaved"><span class="loCardIcon"></span><span><b>' + FH_G[g][0] + ' \u00b7 SAVED FOR THE REVEAL</b><small>You\u2019ll be asked to play it before the first strike. Nothing to do now.</small></span></div>'; });
  if (fhOrdWhy) h += '<div class="alert">' + fhOrdWhy + '</div>';
  const active = s.players.filter(p => p.active).length, lk = s.players.filter(p => p.active && p.lockedIn).length;
  h += locked ? '<button class="pill" disabled>ORDERS LOCKED IN<small>waiting for ' + (active - lk) + '</small></button>'
              : '<button class="pill" style="border-color:#FF6B5A;color:#FF6B5A" onclick="fhLock()">LOCK IN MY ORDERS<small>' + lk + ' of ' + active + ' locked in \u00b7 then the reveal</small></button>';
  return h;
}
function fhHire(k, d) { const o = fhOrd; if (!o) return; const M = ((lastS.fh || {}).hire || [3, 5])[1]; o.h[k] = Math.max(0, Math.min(M, (o.h[k] || 0) + d)); fhSend(); render(lastS); }
function openFhGambit(k) {  // 3.1-dev.116: pick the strike's gambit from a list (it used to cycle on each tap)
  const s = lastS, o = fhOrd, w = o.t[k];
  const opts = fhHand(s).filter(g => FH_G[g][1] === 0 && g !== 14 && (g !== 6 || (w >= 0 && s.planets[w].owner < 0)) && (o.g.indexOf(g) < 0 || o.g[k] === g));
  sheetShow('fhgam', '<div class="sheetSub" style="letter-spacing:3px">' + (k < 3 ? 'FLEET ' + (k + 1) : 'THIS STRIKE') + ' \u00b7 ' + (w >= 0 ? wName(s, w).toUpperCase() : '') + '</div><div class="sheetTitle">ADD A GAMBIT</div>' +
    '<div class="sheetSub">It goes off when this fight starts. Nobody knows until then.</div>' +
    opts.map(g => '<button class="gyrow' + (o.g[k] === g ? ' due' : '') + '" onclick="fhPick(' + k + ',' + g + ')">' + pixIcon('gift', '#FFD166', 2) + '<span><b style="color:#FFD166">' + FH_G[g][0] + '</b> ' + FH_G[g][2] + '</span><em>' + (o.g[k] === g ? '\u2713' : '') + '</em></button>').join('') +
    (opts.length ? '' : '<p class="ghint">None of your gambits fit this strike.</p>') +
    '<div class="biAct">' + (o.g[k] >= 0 ? '<button class="ghost" onclick="fhPick(' + k + ',-1)">NO GAMBIT HERE</button>' : '') + '<button class="ghost" onclick="sheetHide()">CLOSE</button></div>', '#FFD166');
}
function fhPick(k, g) { const o = fhOrd; if (!o) return; o.g[k] = g; sheetHide(); fhSend(); render(lastS); }
async function fhLock() { if (fhOrd) await fhSend(); if (fhOrdWhy) return; lockConfirmPending = false; await post('/lockin?id=' + encodeURIComponent(myId)); poll(); }
function fhAlly(k, d) {
  const s = lastS, o = fhOrd, tot = s.fh.ally.reduce((a, b) => a + b, 0), used = o.al.reduce((a, b) => a + b, 0);
  if (d > 0 && used >= tot) return;
  o.al[k] = Math.max(0, o.al[k] + d); fhSend(); render(s);
}
function fhPin(k) {  // cycles through the gambits you can pin (each one on one strike)
  const s = lastS, o = fhOrd, w = o.t[k];
  const opts = [-1].concat(fhHand(s).filter(g => FH_G[g][1] === 0 && g !== 14 && (g !== 6 || (w >= 0 && s.planets[w].owner < 0)) && (o.g.indexOf(g) < 0 || o.g[k] === g)));
  o.g[k] = opts[(opts.indexOf(o.g[k]) + 1) % opts.length]; fhSend(); render(s);
}
function openFhTarget(k) {
  const s = lastS, o = fhOrd;
  const rows = [0, 1, 2].map(r => s.planets.map((p, i) => i).filter(i => s.planetIncluded[i] && s.planets[i].ring === r && s.planets[i].owner !== mySlot));
  let h = '<div class="sheetSub" style="letter-spacing:3px">' + (k < 3 ? 'FLEET ' + (k + 1) : 'SMUGGLER’S RUN') + '</div><div class="sheetTitle">WHERE DOES IT STRIKE?</div>' +
          '<div class="sheetSub">Anywhere. Worlds you hold or one lane / ring step from them are free; anywhere else needs a jump pass (' + s.fh.jump + ' CR).</div>';
  ['OUTER RIM', 'MID RIM', 'THE CORE'].forEach((nm, r) => {
    h += '<div class="fhgrp">' + nm + '</div><div class="fhworlds">' + rows[r].map(i => '<button class="' + (o.t[k] === i ? 'on' : '') + '" onclick="fhAim(' + k + ',' + i + ')">' + wName(s, i) +
         '<small style="color:' + (fhReach(s, i) ? '#8CE7A0' : '#FFB347') + '">' + (fhReach(s, i) ? 'free' : s.fh.jump + ' CR') + (s.planets[i].owner >= 0 ? ' · ' + feedWho(s, s.planets[i].owner) : '') + '</small></button>').join('') + '</div>';
  });
  const mine = s.planets.map((p, i) => i).filter(i => s.planets[i].owner === mySlot);
  if (k < 3) h += '<div class="fhgrp">OR STAY HOME AND GUARD</div><div class="fhworlds">' + (mine.length ? mine.map(i => '<button class="' + (o.t[k] < 0 && o.gu[k] === i ? 'on' : '') + '" onclick="fhGuard(' + k + ',' + i + ')">' + wName(s, i) + '<small>guard it</small></button>').join('') : '<button class="' + (o.t[k] < 0 ? 'on' : '') + '" onclick="fhGuard(' + k + ',-1)">Stay home<small>no worlds to guard</small></button>') + '</div>';
  else h += '<button class="pill" style="border-color:var(--line);color:var(--dim)" onclick="fhAim(3,-1)">NO SMUGGLER’S RUN</button>';
  sheetShow('fhtgt', h, 'var(--accent)');
}
function fhAim(k, w) { const o = fhOrd; o.t[k] = w; if (w < 0 || (o.g[k] === 6 && lastS.planets[w].owner >= 0)) o.g[k] = -1; sheetHide(); fhSend(); render(lastS); }
function fhGuard(k, w) { const o = fhOrd; o.t[k] = -1; o.gu[k] = w; o.g[k] = -1; sheetHide(); fhSend(); render(lastS); }

// THE REVEAL (the final Resolution)
let fhRv = null, fhRvWhy = '';
function fhRvLoad(s) { if (!fhRv || fhRv.r !== s.round) fhRv = { r: s.round, rv: s.fh.rv.map(x => x.slice()) }; return fhRv; }
async function fhRvSend() {
  const v = fhRv.rv;
  const r = await post('/fh_reveal?g=' + v.map(x => x[0]).join(',') + '&a=' + v.map(x => x[1]).join(',') + '&b=' + v.map(x => x[2]).join(',') + '&id=' + encodeURIComponent(myId));
  fhRvWhy = r && !r.ok ? (r.why || 'Not allowed.') : '';
  if (fhRvWhy) fhRv = null;
  poll();
}
function fhStrikeName(s, st) { return (st[1] === mySlot ? 'Your' : feedWho(s, st[1]) + '’s') + ' ' + st[3] + ' on ' + wName(s, st[2]); }
function fhRevealHtml(s, me) {
  const f = s.fh, v = fhRvLoad(s), locked = me.lockedIn, st = f.strikes || [];
  // 3.1-dev.116 THE GAMBIT MOMENT (canvas board 30; user: "its so easy to skip playing your gambit"): every strike is on the map,
  // then YOUR GAMBIT gets its own card -- PLAY it or KEEP IT -- and READY waits until you've chosen. No skip.
  let h = '<div class="fhkick" style="color:#FF6B5A">THE REVEAL \u00b7 BEFORE THE FIRST STRIKE</div><div class="fhbig">EVERY STRIKE IS ON THE MAP</div>' +
          '<div class="fhsub">Now, in secret, everyone decides whether to play a gambit. The strikes start when everyone is ready.</div><div class="fhgrp">STRIKES ON THE MAP</div>';
  const worlds = [...new Set(st.map(x => x[2]))].sort((a, b) => s.planets[a].ring - s.planets[b].ring || a - b);
  const gd = f.guard || [];
  s.planets.forEach((p, i) => { if (p.owner === mySlot && gd[i] && !worlds.includes(i)) worlds.push(i); });
  if (!worlds.length) h += '<div class="fhsub">Nobody is striking anything. A quiet end to the war… for now.</div>';
  worlds.forEach(w => {
    const here = st.filter(x => x[2] === w), p = s.planets[w], own = p.owner >= 0 ? s.players[p.owner].chosenFaction : -1;
    h += '<div class="fhw"><div class="fhwh"><b>' + p.name + '</b><span style="color:' + (own >= 0 ? FACTION_UI_COLORS[own] : 'var(--dim)') + '">' + (p.owner === mySlot ? 'YOURS' + (gd[w] ? ' · +' + gd[w] + ' guarding' : '') : p.owner >= 0 ? feedWho(s, p.owner) : 'unclaimed') + '</span></div>' +
         here.map(x => { const c = FACTION_UI_COLORS[s.players[x[1]].chosenFaction], pin = (f.pins || []).find(q => q[0] === x[0]);
                         return '<span class="fhchip" style="--c:' + c + '">' + pTag(s, x[1], true) + ' <b>' + x[3] + '</b>' + (x[4] ? ' · ghost' : '') + (pin ? ' · ' + FH_G[pin[1]][0] : '') + '</span>'; }).join('') +
         (here.length > 1 ? '<div class="fhnote">' + here.length + ' strikes fight the defenders side by side. If it falls, they TURN ON EACH OTHER. Last one flying takes ' + wName(s, w) + '.</div>' : '') + '</div>';
  });
  const plays = fhHand(s).filter(g => FH_G[g][1] === 1);
  const kept = fhRvKeep === s.round, playing = v.rv.some(x => x[0] >= 0), decided = !plays.length || kept || playing;
  if (plays.length) h += '<div class="rvGam"><div class="fhgrp" style="color:#B48CFF;margin-top:0">YOUR GAMBIT' + (plays.length > 1 ? 'S' : '') + '</div>';
  if (fhHand(s).includes(8)) h += '<div class="fhsub" style="color:#5FD4FF">INTERCEPTED ORDERS: you see every pinned gambit and Ghost Strike above.</div>';
  plays.forEach((g, k) => {
    const on = v.rv.findIndex(x => x[0] === g);
    h += fhCard(g, { mini: true, on: on >= 0 });
    if (locked) return;
    const r = on >= 0 ? v.rv[on] : null;
    let pick = '';
    const others = st.filter(x => x[1] !== mySlot && !x[4]), any = st.filter(x => !x[4] || x[1] === mySlot);
    if (r && (g === 9 || g === 10)) pick = '<div class="fhpick">' + others.map(x => '<button class="' + (r[1] === x[0] ? 'on' : '') + '" onclick="fhRvSet(' + g + ',1,' + x[0] + ')">' + fhStrikeName(s, x) + '</button>').join('') + (others.length ? '' : '<small>no enemy strikes to hit</small>') + '</div>';
    if (r && g === 5) pick = '<div class="fhpick">' + any.map(x => '<button class="' + (r[1] === x[0] ? 'on' : '') + '" onclick="fhRvSet(5,1,' + x[0] + ')">' + fhStrikeName(s, x) + '</button>').join('') + '</div>';
    if (r && g === 10 && r[1] >= 0) { const by = (st.find(x => x[0] === r[1]) || [])[1]; pick += '<div class="fhgrp">SEND IT TO</div><div class="fhpick">' + s.planets.map((p, i) => i).filter(i => s.planetIncluded[i] && s.planets[i].owner !== by).map(i => '<button class="' + (r[2] === i ? 'on' : '') + '" onclick="fhRvSet(10,2,' + i + ')">' + wName(s, i) + '</button>').join('') + '</div>'; }
    if (r && g === 4) pick = '<div class="fhpick">' + s.planets.map((p, i) => i).filter(i => s.planets[i].owner === mySlot).map(i => '<button class="' + (r[1] === i ? 'on' : '') + '" onclick="fhRvSet(4,1,' + i + ')">' + wName(s, i) + '</button>').join('') + '</div>';
    if (r && g === 7) pick = '<div class="fhpick">' + [0, 1, 2, 3].filter(q => f.ally[q] > 0).map(q => '<button class="' + (r[1] === q ? 'on' : '') + '" onclick="fhRvSet(7,1,' + q + ')">' + FACTION_SHORT_NAMES[q] + ' +' + f.ally[q] + '</button>').join('') + ([0, 1, 2, 3].some(q => f.ally[q] > 0) ? '' : '<small>nobody answered your debts</small>') + '</div>';
    h += pick + '<button class="pill" style="min-height:48px;margin:6px 0 12px;' + (on >= 0 ? 'border-color:#8CE7A0;color:#8CE7A0;background:#0E1A12' : 'border-color:#B48CFF;color:#B48CFF;background:#1C1428') + '" onclick="fhRvToggle(' + g + ')">' + (on >= 0 ? 'PLAYING ' + FH_G[g][0] + ' \u2713' : 'PLAY ' + FH_G[g][0]) + '</button>';
  });
  if (plays.length) h += (!locked && !playing ? '<button class="ghost" style="width:100%;margin:0 0 4px;' + (kept ? 'border-color:#8FA3B8;color:var(--text)' : '') + '" onclick="fhRvKeepIt()">' + (kept ? 'KEEPING IT \u2713' : 'KEEP IT \u00b7 PLAY NOTHING') + '</button>' : '') + '</div>';
  if (fhRvWhy) h += '<div class="alert">' + fhRvWhy + '</div>';
  const active = s.players.filter(p => p.active).length, lk = s.players.filter(p => p.active && p.lockedIn).length;
  h += '<div class="rvWho">' + s.players.map((p, k) => p.active ? '<span style="color:' + (p.lockedIn ? '#8CE7A0' : '#5C6F84') + ';border-color:' + (p.lockedIn ? '#8CE7A0' : '#233142') + '">' + (k === mySlot ? 'YOU' : whoName(s, k).split(' ').pop().toUpperCase()) + (p.lockedIn ? ' \u2713' : '') + '</span>' : '').join('') + '</div>';
  h += locked ? '<button class="pill" disabled>READY<small>waiting for ' + (active - lk) + '</small></button>'
              : '<button class="pill" style="border-color:#FF6B5A;color:#FF6B5A"' + (decided ? ' onclick="fhRvLock()"' : ' disabled') + '>I\u2019M READY<small>' + (decided ? lk + ' of ' + active + ' ready \u00b7 the strikes start when everyone is' : 'play your gambit or keep it first') + '</small></button>';
  return h;
}
let fhRvKeep = -1;  // 3.1-dev.116: the round you chose KEEP IT (play nothing)
function fhRvKeepIt() { fhRvKeep = fhRvKeep === lastS.round ? -1 : lastS.round; render(lastS); }
function fhRvToggle(g) { fhRvKeep = -1; const v = fhRv.rv, on = v.findIndex(x => x[0] === g); if (on >= 0) v[on] = [-1, -1, -1]; else { const k = v.findIndex(x => x[0] < 0); if (k >= 0) v[k] = [g, -1, -1]; } fhRvSend(); render(lastS); }
function fhRvSet(g, j, x) { const r = fhRv.rv.find(q => q[0] === g); if (!r) return; r[j] = x; if (g === 10 && j === 1) r[2] = -1; fhRvSend(); render(lastS); }
async function fhRvLock() { if (fhRv) await fhRvSend(); if (fhRvWhy) return; lockConfirmPending = false; await post('/lockin?id=' + encodeURIComponent(myId)); poll(); }

// THE FINALE playback (its own full-screen layer; the polls never rebuild it)
const FHP = { log: '', steps: null, i: 0, timer: 0, done: false, seen: '' };
function fhParse(hex, s) {
  const b = []; for (let k = 0; k + 1 < hex.length; k += 2) b.push(parseInt(hex.substr(k, 2), 16));
  const steps = []; let k = 0;
  const reveal = [];
  if (b[k] === 0xE0) { const n = b[k + 1]; k += 2; for (let j = 0; j < n; j++) { reveal.push(b.slice(k, k + 4)); k += 4; } }
  steps.push({ kind: 'reveal', plays: reveal });
  while (k < b.length) {
    if (b[k] !== 0xE1) { k++; continue; }
    const w = b[k + 1], n = b[k + 2], holder = b[k + 3] - 1, before = b[k + 4], after = b[k + 5], shield = b[k + 6]; k += 7;  // 3.1-dev.53 + the shield
    const sides = [];
    for (let j = 0; j < n; j++) { sides.push({ by: b[k], ships: b[k + 1], gm: b[k + 2] | (b[k + 3] << 8) }); k += 4; }
    const ex = [];
    let turn = -1;
    while (k < b.length && b[k] !== 0xE5) {
      if (b[k] === 0xE2) { ex.push({ melee: false, d: b[k + 1], r: b.slice(k + 2, k + 2 + n) }); k += 2 + n; }
      else if (b[k] === 0xE3) { turn = ex.length; k++; }
      else if (b[k] === 0xE4) { ex.push({ melee: true, r: b.slice(k + 1, k + 1 + n) }); k += 1 + n; }
      else k++;
    }
    const win = b[k + 1], left = b[k + 2]; k += 3;
    steps.push({ kind: 'world', w, holder, before, after, shield, sides, ex, turn, win, left });
  }
  return steps;
}
function fhWho(s, i) { return pTag(s, i, true); }
function fhCol(s, i) { return i >= 0 && s.players[i] && s.players[i].chosenFaction >= 0 ? FACTION_UI_COLORS[s.players[i].chosenFaction] : '#8FA3B8'; }
function fhRevealLine(s, p) {
  const [by, g, a, b] = p, who = by === mySlot ? 'You' : feedWho(s, by), nm = FH_G[g] ? FH_G[g][0] : '';
  const sid = a < 255 ? a : -1, sBy = sid >= 0 ? Math.floor(sid / 5) : -1;  // (FH_STRIKES = 5 since 3.1-dev.53)
  const whose = sBy === mySlot ? 'your' : sBy >= 0 ? feedWho(s, sBy) + '’s' : 'a';
  const t = { 15: b > 0 ? 'stole ' + b + ' ships from ' + whose + ' strike' : 'found nothing to steal', 9: sid >= 0 ? 'cut ' + whose + ' strike in half' : 'found no strike to sabotage',
              10: sid >= 0 ? 'sent ' + whose + ' strike to ' + wName(s, b) : 'couldn’t redirect anything', 5: sid >= 0 ? 'called off ' + whose + ' strike' : 'called for a peace nobody heard',
              4: a < 255 ? 'made ' + wName(s, a) + ' untouchable' : 'signed nothing', 3: 'rallied ' + b + ' world' + (b === 1 ? '' : 's') + ' (+2 defenders each)', 7: '+' + b + ' ships from old friends' }[g] || '';
  return '<div class="fhpl"><b style="color:' + fhCol(s, by) + '">' + who + '</b> · <span style="color:#FFD166">' + nm + '</span> · ' + t + '</div>';
}
// 3.1-dev.117 IN STEP: while the table plays the finale (s.fh.show >= 0) every phone shows the step the table is on; a step
// skips only when every player asks (the bar at the bottom shows who has). Afterwards (REPLAY) it plays on its own clock.
function fhIdxFor(show) { const nW = FHP.steps ? FHP.steps.length - 1 : 0; return !(show >= 1) ? Math.min(1, FHP.steps ? FHP.steps.length : 1) : show > nW ? FHP.steps.length : show; }  // 3.1-dev.121 no reveal step
function fhLive(s) { return !!(s.fh && s.fh.st === 2 && s.stage === 1); }  // the table is playing the finale (its clock may not have ticked yet)
function fhSync(s) {
  if (!FHP.steps || !document.getElementById('fhPlay')) return;
  const live = fhLive(s);
  if (live) { if (!FHP.sync) clearTimeout(FHP.timer); FHP.sync = true; const k = fhIdxFor(s.fh.show); if (FHP.shown !== k) { FHP.i = k; fhStep(); } }
  else if (FHP.sync && FHP.shown !== FHP.steps.length) { FHP.sync = false; FHP.i = FHP.steps.length; fhStep(); }  // over: the summary
  const bar = document.getElementById('fhSkipBar'); if (bar) { bar.textContent = fhSkipInner(s); bar.classList.toggle('on', !!(s.fh && (s.fh.sk >> mySlot) & 1)); }
}
// 3.1-dev.119 (user: "the skip thing should just be a little button ... push it at anytime in the final hour, and it should skip
// to the end"): a small button in the corner; once everyone still here has pressed it, the table jumps to the summary.
function fhSkipInner(s) {
  const sk = (s.fh && s.fh.sk) || 0, here = s.players.map((p, i) => p.active && !p.away ? i : -1).filter(i => i >= 0);
  const n = here.filter(i => (sk >> i) & 1).length;
  return 'SKIP TO THE END' + (n ? ' \u00b7 ' + n + '/' + here.length : '');
}
function fhSkipHtml() {
  if (!FHP.sync) return '<button class="fhSkipPill" onclick="fhSkip()">SKIP TO THE END</button>';
  const mine = lastS && lastS.fh && ((lastS.fh.sk >> mySlot) & 1);
  return '<button id="fhSkipBar" class="fhSkipPill' + (mine ? ' on' : '') + '" onclick="fhSkip()">' + (lastS ? fhSkipInner(lastS) : 'SKIP TO THE END') + '</button>';
}
function fhPlayEnsure(s) {
  const log = s.fh && s.fh.log; if (!log || FHP.seen === log) return;
  if (FHP.log === log && document.getElementById('fhPlay')) return;
  FHP.log = log; FHP.steps = fhParse(log, s); FHP.done = false;
  FHP.sync = fhLive(s); FHP.i = FHP.sync ? fhIdxFor(s.fh.show) : Math.min(1, FHP.steps.length);  // 3.1-dev.121 (a REPLAY skips the reveal screen too)
  let el = document.getElementById('fhPlay');
  if (!el) { el = document.createElement('div'); el.id = 'fhPlay'; document.body.appendChild(el); }
  el.className = 'on';
  fhStep();
}
function fhClose() { FHP.seen = FHP.log; clearTimeout(FHP.timer); const el = document.getElementById('fhPlay'); if (el) el.remove(); shownHtml = null; if (lastS) render(lastS); }
function fhSkip() { if (FHP.sync) { post('/fh_skip?id=' + encodeURIComponent(myId)).then(poll); return; } clearTimeout(FHP.timer); FHP.i = FHP.steps.length; fhStep(); }
function fhStep() {
  const s = lastS, el = document.getElementById('fhPlay'); if (!el || !s) return;
  clearTimeout(FHP.timer); FHP.shown = FHP.i;
  const steps = FHP.steps, nW = steps.filter(x => x.kind === 'world').length;
  if (FHP.i >= steps.length) {  // the summary
    const ws = steps.filter(x => x.kind === 'world');
    el.innerHTML = '<div class="fhpbox"><div class="fhkick">THE FINAL HOUR · ' + nW + ' FIGHT' + (nW === 1 ? '' : 'S') + ' · ' + ws.filter(x => x.win).length + ' WORLDS CHANGED HANDS</div><div class="fhbig">AFTER THE LAST BATTLE</div>' +
      ws.map(x => '<div class="fhres"><b>' + wName(s, x.w) + '</b><span style="color:' + (x.win ? fhCol(s, x.sides[x.win - 1].by) : fhCol(s, x.holder)) + '">' + (x.win ? 'falls to ' + fhWho(s, x.sides[x.win - 1].by) : x.holder >= 0 ? 'held by ' + fhWho(s, x.holder) : 'holds') + '</span></div>').join('') +
      (steps[0].plays.length ? '<div class="fhgrp">THE GAMBITS AT THE REVEAL</div>' + steps[0].plays.map(p => fhRevealLine(s, p)).join('') : '') +
      '<button class="pill" onclick="fhClose()">' + (s.stage === 2 ? 'FINAL STANDINGS' : 'CLOSE') + '</button></div>';
    return;
  }
  const st = steps[FHP.i];
  if (st.kind === 'reveal') {
    el.innerHTML = '<div class="fhpbox"><div class="fhkick">THE FINAL HOUR</div><div class="fhbig" style="color:#FF6B5A">THE REVEAL</div>' +
      (st.plays.length ? st.plays.map(p => fhRevealLine(s, p)).join('') : '<div class="fhsub">Nobody played a gambit at the reveal. Every strike flies as ordered.</div>') +
      '<div class="fhsub">' + nW + ' world' + (nW === 1 ? '' : 's') + ' under attack. Outer Rim first, the Core last.</div>' + fhSkipHtml() + '</div>';
    if (!FHP.sync) { FHP.i++; FHP.timer = setTimeout(fhStep, 3600); } return;
  }
  // a world: columns, then the dice exchange by exchange
  const n = st.sides.length, cnt = st.sides.map(x => x.ships), abs = st.sides.map(x => (x.gm & 4) ? 2 : 0);
  let D = st.after, sh = st.shield || 0, idx = steps.filter(x => x.kind === 'world').indexOf(st) + 1;
  const fired = [];
  st.sides.forEach(x => { for (let g = 0; g < 16; g++) if ((x.gm >> g) & 1) fired.push('<div class="gc mini on gpop"><div class="gcls">GAMBIT · ' + fhWho(s, x.by) + '</div><div class="gnm">' + FH_G[g][0] + '</div><div class="gfx">' + FH_G[g][2] + '</div></div>'); });
  let mapMode = 'ring', winCol = '';  // 3.1-dev.119 THE MAP (canvas board 7): ring on the target -> the fight -> the winner's colour
  const draw = (dice, dd, msg, turnOn) => {
    el.innerHTML = '<div class="fhpbox' + (mapMode === 'ring' ? ' ringOnly' : '') + '"><div class="fhkick">THE FINALE · ' + idx + ' OF ' + nW + ' · ' + s.planets[st.w].name + '</div>' +
      '<div class="fhbig" style="font-size:16px">' + (mapMode === 'ring' ? s.planets[st.w].name.toUpperCase() + ' IS UNDER ATTACK' : n > 1 ? n + ' STRIKES · SIDE BY SIDE' : fhWho(s, st.sides[0].by) + ' STRIKES') + '</div>' +
      fhMapHtml(s, st.w, mapMode, winCol) +
      (FHP.sub === 0 ? fired.join('') : '') +
      '<div class="fhcols">' + (turnOn ? '' : '<div class="fhcol def" style="--c:' + fhCol(s, st.holder) + '"><small>' + (st.holder >= 0 ? fhWho(s, st.holder) + '’S DEFENDERS' : 'DEFENDERS') + '</small>' + pnumSvg(Math.max(0, D), D > 0 ? '#E8EEF5' : '#FF6B5A', 4) + '<span class="die">' + (dd ? pnumSvg(dd, '#05070C', 3) : '') + '</span></div>') +
      st.sides.map((x, j) => '<div class="fhcol' + (cnt[j] <= 0 ? ' out' : '') + '" style="--c:' + fhCol(s, x.by) + '"><small>' + fhWho(s, x.by) + '</small>' + pnumSvg(Math.max(0, cnt[j]), cnt[j] > 0 ? '#E8EEF5' : '#FF6B5A', 4) + '<span class="die">' + (dice && dice[j] ? pnumSvg(dice[j], '#05070C', 3) : '') + '</span></div>').join('') + '</div>' +
      '<div class="fhmsg">' + (msg || '') + '</div>' + fhSkipHtml() + '</div>';
  };
  FHP.sub = 0;
  draw(null, 0, '', false);  // the ring first
  const firstMsg = (sh ? '<b style="color:#5FD4FF">PLANETARY SHIELD</b>: the first ' + sh + ' hits on it don’t count. ' : '') + (st.before !== st.after ? 'The defenders: ' + st.before + ' → ' + st.after + ' before a shot is fired.' : n > 1 ? 'The defenders roll once. Every strike rolls against them. Beat their die: a defender falls. Lose or tie: you lose one.' : 'Beat the defenders’ die: one of them falls. Lose or tie: you lose one.');
  let e = 0, turned = false;
  const tick = () => {
    FHP.sub = 1;
    if (e >= st.ex.length) {  // the result
      const winBy = st.win ? st.sides[st.win - 1].by : -1;
      mapMode = 'won'; winCol = st.win ? fhCol(s, winBy) : fhCol(s, st.holder);
      draw(null, 0, '<b style="color:' + (st.win ? fhCol(s, winBy) : fhCol(s, st.holder)) + ';font-size:18px">' + (st.win ? s.planets[st.w].name + ' FALLS TO ' + fhWho(s, winBy) : s.planets[st.w].name + ' HOLDS') + '</b><br>' +
           (st.win ? st.left + ' ship' + (st.left === 1 ? '' : 's') + ' left to hold it.' : 'Every strike is broken.'), turned);
      if (!FHP.sync) { FHP.i++; FHP.timer = setTimeout(fhStep, 2600); } return;  // in step: hold the result until the table moves on
    }
    if (e === st.turn && !turned) {
      turned = true;
      draw(null, 0, '<b style="color:#FFD166;font-size:17px">THE DEFENDERS FALL!</b><br>Nobody goes home quietly: the strikes still standing <b>TURN ON EACH OTHER</b>. Everyone rolls; the lowest roll loses a ship. Last one flying takes ' + wName(s, st.w) + '.', true);
      FHP.timer = setTimeout(tick, 3000); return;
    }
    const x = st.ex[e++];
    let msg = '';
    if (!x.melee) {  // the same rules as the table: highest rolls land first
      for (let hi = 6; hi >= 1; hi--) for (let j = 0; j < n; j++) {
        if (x.r[j] !== hi) continue;
        if (hi > x.d) { if (sh > 0) sh--; else if (D > 0) D--; } else if (abs[j]) abs[j]--; else cnt[j]--;
      }
      msg = 'The defenders rolled ' + x.d + '.';
    } else {
      const live = x.r.map((v, j) => cnt[j] > 0 ? v : 7), low = Math.min(...live), lowN = live.filter(v => v === low).length;
      if (lowN === 1) { const j = live.indexOf(low); if (abs[j]) abs[j]--; else cnt[j]--; msg = fhWho(s, st.sides[j].by) + ' rolled lowest and loses a ship.'; }
      else msg = 'A tie for lowest. Roll again!';
    }
    draw(x.r, x.melee ? 0 : x.d, msg, x.melee);
    FHP.timer = setTimeout(tick, st.ex.length > 18 ? 520 : 820);
  };
  FHP.timer = setTimeout(() => { mapMode = 'fight'; draw(null, 0, firstMsg, false); FHP.timer = setTimeout(tick, fired.length ? 2600 : 1600); }, 1800);
}
function fhMapHtml(s, w, mode, winCol) {
  let h = '<div class="fmm' + (mode === 'ring' ? ' ring' : ' small') + '">' + galaxyMapSvg(s);
  for (let i = 0; i < s.planets.length; i++) {
    if (!s.planetIncluded[i]) continue;
    const pos = galaxyPos(i), o = s.planets[i].owner, c = o >= 0 && s.players[o] && s.players[o].chosenFaction >= 0 ? FACTION_UI_COLORS[s.players[o].chosenFaction] : '#2A3646';
    const tgt = i === w, won = tgt && mode === 'won';
    h += '<i class="' + (tgt ? 'tgt' : '') + (won ? ' won' : '') + '" style="left:' + (50 + pos[0] / 4.66).toFixed(2) + '%;top:' + (50 + pos[1] / 4.66).toFixed(2) + '%;--c:' + (won ? winCol : c) + '"></i>';
  }
  return h + '</div>';
}
function fhReplay() { FHP.seen = ''; FHP.log = ''; fhPlayEnsure(lastS); }
// ---- 3.1-dev.46 THE CLARITY PASS (mockup tools/ui31_mockups/clarity_droid.html; user 2026-11-12) ----
// Every message stays in character; under it a terminal READOUT says why and what changed; RV-3 "RIVET", your droid,
// explains it in plain words when you tap the corner icon (a gold dot = not read yet).
const RV_PAL = { w: '#D9E2EC', b: '#5FD4FF', e: '#FFB347', d: '#3A4656', g: '#9AA8B8' };
const RV_ROWS = ['....wwww....', '..wwwwwwww..', '.wwbbwwgwww.', '.wbeebwwddw.', '.wwbbwwwwww.', '.dddddddddd.',
                 '.wwwwbbwwww.', '.wbwwbbwwbw.', '.wwwwwwwwww.', '.gw.wwww.wg.', '.dd......dd.', '............'];
let RV_SVG = '';
function rivetSvg(px) {
  if (!RV_SVG) RV_ROWS.forEach((r, y) => [...r].forEach((c, x) => { if (RV_PAL[c]) RV_SVG += '<rect x="' + x + '" y="' + y + '" width="1.02" height="1.02" fill="' + RV_PAL[c] + '"/>'; }));
  return '<svg viewBox="0 0 12 12" width="' + px + '" height="' + px + '" shape-rendering="crispEdges" aria-hidden="true">' + RV_SVG + '</svg>';
}
const RV = { open: new Set(), read: new Set() };
try { JSON.parse(sessionStorage.getItem('rvRead') || '[]').forEach(k => RV.read.add(k)); } catch (e) {}
const RV_BEEPS = ['Bwee-oo.', 'Bip.', 'Whirr.', 'Bweep.', 'Bip-bip.', 'Bwoo-oo.'];
function rvKey(k) { return String(k).replace(/[^A-Za-z0-9_-]/g, '_'); }
function rivetBtn(key) {
  if (!setOn('rivet')) return '';  // 3.1-dev.61: experienced crews can switch Rivet off (Settings)
  const k = rvKey(key);
  return '<button class="rvBtn' + (RV.read.has(k) ? ' read' : '') + '" onclick="rivetTap(event,\'' + k + '\')" aria-label="Ask Rivet what this means">' + rivetSvg(24) + '</button>';
}
function rivetSay(key, text, act) {  // act = [label, js]
  if (!setOn('rivet')) return '';
  const k = rvKey(key);
  let h = 0; for (const ch of k) h = (h * 31 + ch.charCodeAt(0)) >>> 0;
  return '<div class="rvSay" id="rv_' + k + '"' + (RV.open.has(k) ? '' : ' hidden') + '>' + rivetSvg(30) + '<div><b>RIVET</b><i>' + RV_BEEPS[h % RV_BEEPS.length] + '</i> ' + text +
         (act ? '<br><button class="rvAct" onclick="event.stopPropagation();' + act[1] + '">' + act[0] + '</button>' : '') + '</div></div>';
}
function rivetTap(ev, k) {
  ev.stopPropagation();
  if (RV.open.has(k)) RV.open.delete(k); else RV.open.add(k);
  const el = document.getElementById('rv_' + k); if (el) el.hidden = !RV.open.has(k);
  RV.read.add(k); ev.currentTarget.classList.add('read');
  try { sessionStorage.setItem('rvRead', JSON.stringify([...RV.read].slice(-200))); } catch (e) {}
}
// 3.1-dev.70: Rivet beside a section -- a small button on the right, his line opens right under it (off in Settings = nothing).
function rivetAsk(key, text, act) {
  if (!setOn('rivet')) return '';
  return '<div class="rvAsk">' + rivetBtn(key) + '</div>' + rivetSay(key, text, act);
}
// rows: [label, html] or ['fx', label, [[good|bad|warn, thing, state], ...]]
function readoutHtml(rows, key) {
  return '<div class="rdo">' + rows.map(r => r[0] === 'fx'
    ? '<div class="rdFx"><span class="rdK">' + r[1] + '</span><div>' + r[2].map(x => '<div class="rdR"><i class="' + x[0] + '"></i><span>' + x[1] + '</span><b class="' + x[0] + '">' + x[2] + '</b></div>').join('') + '</div></div>'
    : '<div class="rdL"><span class="rdK">' + r[0] + '</span><span class="rdV">' + r[1] + '</span></div>').join('') + (key ? rivetBtn(key) : '') + '</div>';
}
const RD_TIER_COL = ['#FF6B5A', '#FFB347', '#A9BDD0', '#8CE7A0', '#FFD166'];
function standingHtml(f, t, dir) {  // the bar + the tier word, red with a down arrow / green with an up arrow when it just moved
  if (t < 0 || t > 4) return '';
  const col = dir < 0 ? '#FF6B5A' : dir > 0 ? '#8CE7A0' : RD_TIER_COL[t];
  return '<span class="rdBar" style="--fc:' + FACTION_UI_COLORS[f] + '">' + [0, 1, 2, 3, 4].map(k => '<i' + (k <= t ? ' class="on"' : '') + '></i>').join('') + '</span>' +
         '<b style="color:' + col + '">' + (dir < 0 ? '▼ ' : dir > 0 ? '▲ ' : '') + favorWord(f, t) + '</b>';
}
const RD_RING = ['OUTER RIM', 'MID RIM', 'THE CORE'];
function worldLine(s, p) {
  const P = s.planets[p]; if (!P) return '';
  return P.name + ' · ' + (RD_RING[P.ring] || '') + ' · ' + (P.owner === mySlot ? 'YOURS' : P.owner >= 0 ? feedWho(s, P.owner).toUpperCase() + '’S' : 'UNCLAIMED');
}
const RIVAL_OF = [1, 0, 3, 2];
// What a change of standing does (rows for the readout, sentences for Rivet).
function standingEffects(f, from, to) {
  const rows = [], say = [], RF = FNAMES[RIVAL_OF[f]];
  if (to <= 1) { rows.push(['bad', FACTION_SHORT_NAMES[f] + ' SHOP', 'CLOSED TO YOU']); say.push('Their shop won’t sell to you until you’re back to ' + favorWord(f, 2) + '.'); }
  else if (from >= 0 && from <= 1) { rows.push(['good', FACTION_SHORT_NAMES[f] + ' SHOP', 'OPEN AGAIN']); say.push('Their shop is open to you again.'); }
  if (to >= 3 && from >= 0 && from < 3) { rows.push(['good', 'ESCORTS', 'VS THE ' + RF.toUpperCase()]); say.push('Now they’ll send an escort ship when you attack ' + RF + ' worlds.'); }
  if (to < 3 && from >= 3) { rows.push(['bad', 'ESCORTS', 'NONE']); say.push('They won’t send escorts any more.'); }
  if (to === 0 && from > 0) { rows.push(['bad', 'TALKS', 'REFUSED']); rows.push(['bad', 'THEIR MILITIA', 'AGAINST YOU']); say.push('They won’t talk to you, their militia will defend against you, and they may come after you.'); }
  if (to === 4 && from >= 0 && from < 4) { rows.push(['good', 'FAMILY', 'THEIR BEST PERKS']); say.push('You’re one of their own now.'); }
  return { rows, say };
}
const TX_WHY = ['TAKEN BY FORCE', 'TAKEN FROM A FRIEND', 'TAKEN FROM THEIR RIVAL', 'BOUGHT', 'A DEAL', 'YOU WALKED OUT', 'A PROMISE KEPT', 'A PROMISE BROKEN', 'YOU DEALT WITH THEIR RIVAL'];
function txWhySay(cause, P) {
  return ['You took ' + P + ' by force, and it was theirs.', 'You took ' + P + ' from them while they still counted you a friend.', 'You took ' + P + ' from their rival, and they loved it.',
          'You paid for ' + P + ', and they liked the Credits.', 'You struck a deal with them over ' + P + '.', 'You walked out on their talks at ' + P + '.',
          'You kept your promise to them.', 'You broke your promise to them.', 'You won ' + P + ' from their rival without a shot. They hold it against you, a little.'][cause] || '';
}
function txReadout(s, f, cause, planet, from, to, delta) {
  const P = planet >= 0 && s.planets[planet] ? s.planets[planet].name : '';
  const rows = [];
  const court = cause === 4 && planet >= 0 && s.planets[planet] && s.planets[planet].tc && s.planets[planet].owner === mySlot;  // 3.1-dev.84 (user: "a deal on mustafar ... oh its mustafar's court")
  const why = court ? 'YOUR COURT' : cause >= 0 && cause < TX_WHY.length ? TX_WHY[cause] : '';
  if (why || P) rows.push(['RE', (P ? P + ' · ' : '') + (why || '') + ' · ROUND ' + s.round]);
  const cur = to >= 0 ? to : tierIdx(s, f), dir = to >= 0 && from >= 0 && to !== from ? Math.sign(to - from) : Math.sign(delta || 0);
  rows.push(['STANDING', standingHtml(f, cur, dir)]);
  const fx = to >= 0 && from >= 0 && to !== from ? standingEffects(f, from, to) : { rows: [], say: [] };
  if (cause === 0 || cause === 1) fx.rows.push(['good', FACTION_SHORT_NAMES[RIVAL_OF[f]], '▲ PLEASED']);
  if (fx.rows.length) rows.push(['fx', 'EFFECT', fx.rows]);
  let say = court ? 'You hold ' + P + ', and ' + P + '\u2019s court speaks for you every round: +' + 2 + ' favor with the ' + FNAMES[f] + '. Which world has the court changes every game.' : txWhySay(cause, P);
  if (to >= 0 && from >= 0 && to !== from) say += ' That makes you ' + favorWord(f, to) + ' with the ' + FNAMES[f] + '.';
  else if (delta) say += delta < 0 ? ' They think a little less of you.' : ' They think a little more of you.';
  if (fx.say.length) say += ' ' + fx.say.join(' ');
  if (cause === 0 || cause === 1) say += ' The ' + FNAMES[RIVAL_OF[f]] + ', their rival, liked it.';
  if (dir < 0 && cur <= 2) say += ' To win them back: take a job from them, keep a promise, or buy one of their worlds.';
  const key = 'tx' + s.round + '_' + f + '_' + cause + '_' + planet + '_' + from + '_' + to;
  return readoutHtml(rows, key) + rivetSay(key, say.trim() || 'They’re telling you how they see you. Your favor with them is on the bar.');
}
// Galaxy News + your own news: the facts as a readout, the "what it means" behind Rivet.
function newsReadout(s, e, t, key) {
  const rows = [];
  if (e.planet >= 0 && s.planets[e.planet]) rows.push(['WORLD', worldLine(s, e.planet)]);
  if (e.who >= 0 && e.who < 4) rows.push(['STANDING', standingHtml(e.who, tierIdx(s, e.who), 0)]);
  if (!rows.length && !t.you) return '';
  return readoutHtml(rows, t.you ? key : '') + (t.you ? rivetSay(key, t.you, e.planet >= 0 ? ['SHOW ME ON THE MAP', 'beatMap(' + e.planet + ')'] : null) : '');
}
// ---- 3.1-dev.48 SELL IT TWICE (Scoundrels; user: "the feeling of Han in the freighter ... you've abused the system so much you get in trouble") ----
const SELL_WHAT = ['A BACK DOOR TO {W}', '{W}\u2019S LANDING CODES', 'A SLOW JOB', 'WHAT THEY ASKED OF YOU', 'FIND WHO HIT {W}'];
const SELL_LINES = ['A friendly world with an unlocked door. {W}, for the right price? We\u2019ll pay for that.',
                    'Codes, patrol schedules, a quiet way into {W}. Worth every credit.',
                    'You\u2019ll be slow about that little job. Won\u2019t you? We\u2019ll make it worth your while.',
                    'Tell us exactly what they asked of you. Knowing what they want is worth more than credits.',
                    'Someone hit {W}. Find out who did it, and you\u2019ll be paid well.'];
const SELL_STORY = ['You won {W} from the {RIVAL} by talking. The {BUYER} want a back door into it.',
                    'You bought {W} from the {RIVAL}. The {BUYER} want its landing codes.',
                    'You just did a job for the {RIVAL}. The {BUYER} will pay you to have dragged your feet.',
                    'You made amends with the {RIVAL}. The {BUYER} want to know exactly what that cost you.',
                    'You took {W} from the {BUYER} by force. Now they want to pay someone to find out who did it. That someone is you.'];
const HEAT_MOOD = ['', 'Someone\u2019s asking questions about you.', 'Two old enemies are comparing notes. Lie low.'];
function sellHolo(f, line) {
  const s = lastS, v = commanderVoice(f, s && s.players[mySlot] ? s.players[mySlot].commander : -1);
  return '<div class="ghHolo" style="--hc:' + FACTION_UI_COLORS[f] + '">' + (v >= 0 && ART ? '<img class="pixel" src="' + portraitURL(v, 3, true) + '" alt="">' : insigniaSvg(f, 48)) +
         '<div class="ghLine"><span>\u201C' + line + '\u201D</span></div></div>';
}
function sellRows(s) {
  const v = s.v2 || {}, rows = [];
  if (v.champOffer >= 0 && (v.champ || []).some((c, g) => c === mySlot && g !== v.champOffer) && titleSeen !== s.round + ':' + v.champOffer)  // 3.1-dev.49 ONE TITLE
    rows.push('<button class="gyrow due" onclick="openTitleSheet()">' + pixIcon('crown', '#FFD166', 2) + '<span><b style="color:#FFD166">A SECOND TITLE</b> the ' + FNAMES[v.champOffer] + ' want you as champion too</span><em></em></button>');
  if (v.sell) rows.push('<button class="gyrow due" onclick="openSellSheet()">' + pixIcon('heist', '#FFD166', 2) + '<span><b style="color:#FFD166">SELL IT TWICE?</b> the ' + FNAMES[v.sell[0]] + ' want in \u00b7 +' + v.sell[3] + ' CR</span><em></em></button>');
  if (v.caught) rows.push('<button class="gyrow bad" onclick="openCaughtSheet()">' + pixIcon('target', '#FF6B5A', 2) + '<span><b>THEY BOTH KNOW</b> the ' + FNAMES[v.caught[0]] + ' and the ' + FNAMES[v.caught[1]] + ' come for you at the start of round ' + v.caught[2] + '</span><em></em></button>');
  else if (v.heat) rows.push('<div class="gyrow' + (v.heat >= 2 ? ' bad' : '') + '">' + pixIcon('holonet', v.heat >= 2 ? '#FF6B5A' : '#FFB347', 2) + '<span><b style="color:' + (v.heat >= 2 ? '#FF6B5A' : '#FFB347') + '">' + kw('HEAT') + '</b> ' + HEAT_MOOD[Math.min(2, v.heat)] + '</span></div>');
  return rows;
}
let sellSeen = '';
function sellAutoOpen(s) {
  const v = s.v2 || {}; if (!v.sell || typeof sheetKey === 'undefined' || sheetKey) return;
  const key = s.round + ':' + v.sell.join(','); if (sellSeen === key) return;
  sellSeen = key; openSellSheet();
}
function openSellSheet() {
  const s = lastS, v = s && s.v2, o = v && v.sell; if (!o) return sheetHide();
  const [f, kind, w, cr] = o, rival = RIVAL_OF[f], W = w >= 0 ? wName(s, w) : '';
  const fill = x => x.split('{W}').join(W || 'it').split('{RIVAL}').join(FNAMES[rival]).split('{BUYER}').join(FNAMES[f]);
  const heat = v.heat || 0, key = 'sell' + s.round + '_' + f + '_' + kind + '_' + w;
  const rd = readoutHtml([['SELLING', fill(SELL_WHAT[kind] || '').toUpperCase()], ['fx', 'PAY', [['good', '+' + cr + ' CR', ''], ['good', '\u25B2 ' + FACTION_SHORT_NAMES[f] + ' FAVOR', '']]],
                          ['fx', 'HEAT', [[heat >= 2 ? 'bad' : 'warn', heat >= 2 ? 'ONE MORE AND THEY KNOW' : heat ? 'SOMEONE\u2019S ASKING' : 'CLEAN', heat >= 2 ? 'CAUGHT IF YOU SELL' : '\u25B2 +1']]]], key);
  const say = fill(SELL_STORY[kind] || '') + ' Sell it and you get paid by both sides, and the ' + FNAMES[f] + ' like you more. Every sale adds heat; it only cools in a round you don\u2019t sell. ' +
              (heat >= 2 ? 'Right now the ' + FNAMES[f] + ' and the ' + FNAMES[rival] + ' are this close to comparing notes. Sell this and they will: both drop you a tier and both come for you next round.' : heat ? 'You\u2019ve sold once lately. Twice more before it cools and you\u2019re caught.' : 'You\u2019re clean right now.');
  sheetShow('sell', '<div class="sheetSub" style="letter-spacing:3px">ONLY YOU SEE THIS</div><div class="sheetTitle">SELL IT TWICE?</div>' + sellHolo(f, fill(SELL_LINES[kind] || '')) + rd + rivetSay(key, say) +
    '<button class="pill" style="border-color:#FFD166;color:#FFD166" onclick="answerSell(1)">SELL IT TWICE \u00b7 +' + cr + ' CR</button>' +
    '<button class="pill" style="border-color:var(--line);color:var(--dim)" onclick="answerSell(0)">KEEP IT CLEAN</button>', '#FFD166');
}
let titleSeen = '';
function openTitleSheet() {  // 3.1-dev.49 #13: one title at a time -- swap or keep
  const s = lastS, v = s && s.v2, f = v && v.champOffer; if (!(f >= 0)) return sheetHide();
  const held = (v.champ || []).findIndex((c, g) => c === mySlot && g !== f); if (held < 0) return sheetHide();
  const key = 'title' + f + '_' + held;
  sheetShow('title', '<div class="sheetSub" style="letter-spacing:3px">ONE TITLE AT A TIME</div><div class="sheetTitle">A SECOND TITLE</div>' +
    readoutHtml([['YOU HOLD', 'CHAMPION OF THE ' + FACTION_SHORT_NAMES[held]], ['OFFERED', 'CHAMPION OF THE ' + FACTION_SHORT_NAMES[f]]], key) +
    rivetSay(key, 'Nobody stands higher with the ' + FNAMES[f] + ' than you, but a champion serves one faction. Swap, and the ' + FNAMES[held] + ' title (and its decree, if you still hold it) goes to whoever is next. You can\u2019t take it back by climbing again unless you swap back. Keep, and the ' + FNAMES[f] + ' title goes to the next best.') +
    '<button class="pill" style="border-color:' + FACTION_UI_COLORS[f] + ';color:' + FACTION_UI_COLORS[f] + '" onclick="swapTitle()">BECOME CHAMPION OF THE ' + FACTION_SHORT_NAMES[f] + '</button>' +
    '<button class="pill" style="border-color:' + FACTION_UI_COLORS[held] + ';color:' + FACTION_UI_COLORS[held] + '" onclick="titleSeen=lastS.round+\':\'+' + f + ';sheetHide();render(lastS)">STAY CHAMPION OF THE ' + FACTION_SHORT_NAMES[held] + '</button>', '#FFD166');
}
async function swapTitle() { sheetHide(); await post('/swap_title?id=' + encodeURIComponent(myId)); poll(); }
async function answerSell(sell) { sheetHide(); await post('/sell_twice?sell=' + sell + '&id=' + encodeURIComponent(myId)); poll(); }
function openCaughtSheet() {
  const s = lastS, c = s && s.v2 && s.v2.caught; if (!c) return sheetHide();
  const me = s.players[mySlot], key = 'caught' + c[2] + '_' + c[0];
  const holo = '<div class="caughtHolo">' + sellHolo(c[0], 'You took our money.') + sellHolo(c[1], 'And ours.') + '</div>';
  const hits = [0, 1].filter(k => c[k] >= 0).map(k => ['bad', FACTION_SHORT_NAMES[c[k]], ['UPRISING', 'IMPERIAL STRIKE', 'SHAKEDOWN', 'ASSASSINS'][c[k]]]);
  const rd = readoutHtml([['WHEN', 'START OF ROUND ' + c[2]], ['fx', 'COMING', hits]], key);
  const say = 'I did warn you. The ' + FNAMES[c[0]] + ' and the ' + FNAMES[c[1]] + ' compared notes, and now they both know. Both already think less of you, and at the start of round ' + c[2] +
              ' both strike. Pay one of them back what they paid you, plus a little for their trouble, and only the other comes. Or ride it out.';
  const btns = [0, 1].filter(k => c[k] >= 0).map(k => '<button class="pill" style="border-color:' + FACTION_UI_COLORS[c[k]] + ';color:' + FACTION_UI_COLORS[c[k]] + '"' + (me.credits >= c[3 + k] ? '' : ' disabled') +
               ' onclick="payBack(' + c[k] + ')">PAY BACK THE ' + FACTION_SHORT_NAMES[c[k]] + ' \u00b7 ' + c[3 + k] + ' CR</button>').join('');
  sheetShow('caught', '<div class="sheetSub" style="letter-spacing:3px;color:#FF6B5A">CAUGHT PLAYING BOTH SIDES</div><div class="sheetTitle" style="color:#FF6B5A">THEY BOTH KNOW</div>' + holo + rd + rivetSay(key, say) + btns +
    '<button class="pill" style="border-color:var(--line);color:var(--dim)" onclick="sheetHide()">RIDE IT OUT</button>', '#FF6B5A');
}
async function payBack(f) { await post('/pay_back?f=' + f + '&id=' + encodeURIComponent(myId)); await poll(); openCaughtSheet(); }
// ---- 3.1-dev.52 THE GALLERY: watch another player's talk live; whisper against them or vouch for them, once, before the last question ----
const WATCH = { by: -1, seen: {} };
function galleryNotify(s) {  // a new talk somewhere: a notification you can tap to watch
  for (const tk of (s.talks || [])) {
    const key = s.round + ':' + tk.by + ':' + tk.p;
    if (WATCH.seen[key]) continue;
    WATCH.seen[key] = 1;
    notify(pTag(s, tk.by, true) + ' IS TALKING', 'on ' + wName(s, tk.p) + ' \u00b7 tap to watch', '#5FD4FF', 9000, false, () => openWatch(tk.by));
  }
}
function openWatch(by) {
  const s = lastS, tk = s && (s.talks || []).find(x => x.by === by);
  if (!tk) { if (WATCH.by === by) { WATCH.by = -1; if (sheetKey === 'watch') sheetHide(); } return; }
  WATCH.by = by;
  const P = s.planets[tk.p], col = tk.f >= 0 ? FACTION_UI_COLORS[tk.f] : 'var(--dim)';
  const face = tk.f >= 0 ? portraitURL(commanderVoice(tk.f, s.players[mySlot] ? s.players[mySlot].commander : -1), 3, 1) : (tk.p % 5 === 2 || tk.p % 5 === 4) ? hoodedURL(tk.p) : portraitURL(tk.p % 16, 3, 2);
  const pips = '<div class="negpips">' + tk.m.map((m, k) => '<i class="m' + m + (tk.r === 0 && k === tk.s ? ' cur' : '') + '"></i>').join('') + '</div>';
  let h = '<div class="sheetSub" style="letter-spacing:3px">THE GALLERY \u00b7 WATCHING</div><div class="sheetTitle">' + pTag(s, by, true) + ' ON ' + P.name + '</div>' +
          '<div class="neghead">' + (face ? '<img class="pixel negface" src="' + face + '" width="72" height="84" alt="">' : '') +
          '<span style="color:' + col + '">' + (tk.f >= 0 ? TX_FACTIONS[tk.f].name : 'UNKNOWN AMBASSADOR') + '</span>' + pips + '</div>';
  if (tk.r !== 0) h += '<div class="watchQ" style="text-align:center"><b>' + ([0, 1, 1, 0, 0, 0][tk.r] ? 'THE TALK IS OVER' : 'THE TALK IS OVER') + '</b></div>';
  else if (tk.q) h += '<div class="sheetSub">QUESTION ' + (tk.s + 1) + ' OF ' + tk.n + '</div><div class="watchQ">' + tk.q + '</div>' + tk.rep.map(r => '<div class="watchR">' + r + '</div>').join('');
  else h += '<div class="watchQ" style="text-align:center">Weighing what they owe\u2026</div>';
  if (tk.w >= 0) h += '<div class="galNote bad">' + pTag(s, tk.w) + ' whispered against them: their next question got harder.</div>';
  for (let o = 0; o < 4; o++) if ((tk.v >> o) & 1) h += '<div class="galNote good">' + pTag(s, o) + ' vouched for them: a wrong answer is gone from their next question.</div>';
  const key = 'gal' + tk.by + '_' + tk.p + '_' + s.round;
  h += readoutHtml([['fx', 'IF THEY WIN', [['good', 'VOUCHERS', '\u25B2 FAVOR'], ['bad', 'THE WHISPERER', '\u25BC FAVOR']]], ['fx', 'IF THEY FAIL', [['bad', 'VOUCHERS', '\u25BC FAVOR'], ['good', 'THE WHISPERER', '\u25B2 FAVOR']]]], key) +
       rivetSay(key, 'You get one move per talk, before its last question. WHISPER (2 CR) and their next question gets harder; if the talk still goes their way, that faction decides you lied. VOUCH (only if that faction likes you) and a wrong answer disappears from their next question; if they blow it anyway, it\u2019s on you. Everyone sees who did what.');
  // 3.1-dev.122 (playtest: "I don't understand the rules behind vouching, whispering makes sense"): each button says its rule.
  if (tk.cw) h += '<button class="pill" style="border-color:#FF6B5A;color:#FF6B5A" onclick="galleryAct(' + by + ',1)">WHISPER AGAINST \u00b7 2 CR<small>Their next question gets harder. If they win anyway, the ' + (tk.f >= 0 ? FNAMES[tk.f] : 'their faction') + ' like you less.</small></button>';
  if (tk.cv) h += '<button class="pill" style="border-color:#8CE7A0;color:#8CE7A0" onclick="galleryAct(' + by + ',0)">VOUCH FOR ' + pTag(s, by, true) + '<small>Free. One wrong answer disappears from their next question. If they win, the ' + (tk.f >= 0 ? FNAMES[tk.f] : 'their faction') + ' like you more; if they fail, less.</small></button>';
  if (!tk.cw && !tk.cv && tk.r === 0) h += '<div class="sheetSub">' + (tk.why || 'Nothing you can do here right now. Just watch.') + '</div>';
  h += '<button class="pill" style="border-color:var(--line);color:var(--dim)" onclick="WATCH.by=-1;sheetHide()">STOP WATCHING</button>';
  sheetShow('watch', h, '#5FD4FF', () => { WATCH.by = -1; });
}
async function galleryAct(by, whisper) {
  const r = await post('/gallery?talker=' + by + '&w=' + whisper + '&id=' + encodeURIComponent(myId));
  if (r && !r.ok && r.why) notify('NOT NOW', r.why, '#FF6B5A', 4000);
  await poll(); openWatch(by);
}
// Home: THE GALAXY AND YOU -- only YOUR situations (user 2026-11-06: no tiers on Home, your rep is secret).
// 3.1-dev.64 (user: "galaxy and you gets really crowded by the end of the game"; mockup
// https://claude.ai/artifact/UPgQvfbQ4VpshWeho6NUnH, "yes I agree"): look-alike rows merge (gambits, YOU OWE, OUT FOR YOU),
// rows sort into three bands -- NOW (lands or lapses this round, or a choice is waiting) > YOUR MOVES > GOOD TO KNOW --
// and the first GY_SHOW show, the rest behind + N MORE (stays open on this phone once opened, until SHOW LESS).
const GY_SHOW = 3;  // 3.1-dev.88 (user: the buttons below shouldn't be a lot of scrolling away)
let gyOpen = null;
function gyIsOpen() { if (gyOpen === null) { try { gyOpen = localStorage.getItem('sfGyOpen') === '1'; } catch (e) { gyOpen = false; } } return gyOpen; }
function gySetOpen(v) { gyOpen = v; try { localStorage.setItem('sfGyOpen', v ? '1' : '0'); } catch (e) {} shownHtml = null; if (lastS) render(lastS); }
// 3.1-dev.67 LIVE BUTTONS (mockup idea 1): FAVOR = the four faction symbols in your mood color with each; STANDINGS = a podium.
const INS_COL = ['#FFC933', '#E6EEF5', '#9B5FE0', '#D0343E'];  // insigniaSvg's own colors, swapped for the mood
function favorBtnHtml(s) {
  return '<button class="cir live" onclick="openReputation()" aria-label="Favor"><span class="fvq">' + [0, 1, 2, 3].map(f => { const t = tierIdx(s, f);
    return '<i' + (t === 0 ? ' class="hot"' : '') + '>' + insigniaSvg(f, 15).split(INS_COL[f]).join(FAVOR_COL[t]) + '</i>'; }).join('') + '</span><small>FAVOR</small></button>';
}
function standingsBtnHtml(s) {
  const ps = s.players.map((p, k) => ({ p: p, k: k })).filter(x => x.p.active), top = Math.max(1, ...ps.map(x => x.p.vp || 0)), hide = s.v2 && s.v2.vpHidden;
  return '<button class="cir live" onclick="openStandings()" aria-label="Standings"><span class="podium">' + ps.map(x => '<i class="' + (x.k === mySlot ? 'me' : '') + '" style="height:' +
    (hide && x.k !== mySlot ? 13 : Math.max(4, Math.round(26 * (x.p.vp || 0) / top))) + 'px;background:' + (FACTION_UI_COLORS[x.p.chosenFaction] || '#8FA3B8') + '"></i>').join('') + '</span><small>STANDINGS</small></button>';
}
function gyRow(pri, icon, col, head, line, tail, tap, cls) {
  if (!tap) { GY_INFO.push({ head: head, line: line.charAt(0).toUpperCase() + line.slice(1) + '.', col: col }); tap = 'gyInfo(' + (GY_INFO.length - 1) + ')'; }  // 3.1-dev.102: every row opens something
  // 3.1-dev.88 three levels: URGENT (a threat: red pulsing dot, the row glows), NEEDS YOU (amber blinking dot), INFO (no dot);
  // a word in the tail of a row that needs you is its button
  const urg = pri === 0 && /strike|bad/.test(cls || ''), act = pri === 0 && !urg, btn = pri === 0 && tail && !/^R?\d+R?$/.test(tail);
  return { pri: pri, html: '<button class="gyrow' + (cls ? ' ' + cls : '') + (urg ? ' urg' : act ? ' act' : '') + '"' + (tap ? ' onclick="' + tap + '"' : '') + '>' + pixIcon(icon, col, 2) +
    '<span><b style="color:' + col + '">' + kwify(head) + '</b> ' + line + '</span><em' + (btn ? ' class="btn"' : pri === 0 && tail ? ' style="color:' + col + '"' : '') + '>' + (tail || '') + '</em></button>' };
}
function gyRows(s) {
  const v = s.v2 || {}, f = s.fh, R = [];
  GY_INFO = [];
  // 3.1-dev.102 GUNS FOR HIRE / hired guns / head starts / stand by / beaten
  if (v.gfhIn) R.push(gyRow(0, 'fleet', '#B48CFF', 'SHIPS FOR HIRE', v.gfhIn[1] + ' for ' + v.gfhIn[2] + ' CR \u00b7 ' + feedWho(s, v.gfhIn[0]), 'ANSWER', 'openGfhIncoming()', 'due'));
  if (v.gfhCan) R.push(gyRow(1, 'fleet', '#B48CFF', 'GUNS FOR HIRE', 'lend ships for a price, once a round', '', 'openGfhSheet()', ''));
  if (v.gfhOut) { const o = v.gfhOut; R.push(gyRow(2, 'fleet', '#B48CFF', o[3] === 2 ? 'HIRED OUT' : o[3] === 3 ? 'OFFER DECLINED' : 'OFFER SENT', o[1] + ' ships \u00b7 ' + feedWho(s, o[0]) + (o[3] === 1 ? ' \u00b7 waiting' : ''), '', '', '')); }
  if (v.hired) R.push(gyRow(1, 'fleet', '#B48CFF', 'HIRED GUNS', v.hired[0] + ' of ' + feedWho(s, v.hired[1]) + '\u2019s ships join your next battle', '', '', ''));
  if (v.head) for (let fi = 0; fi < 4; fi++) if ((v.head >> fi) & 1) R.push(gyRow(2, 'holonet', '#5FD4FF', 'HEAD START', 'your next talk with the ' + FNAMES[fi] + ' skips a question', '', '', ''));
  if (v.stby && v.stby[1] === s.round) R.push(gyRow(1, 'fleet', '#FFD166', 'STAND BY', [0, 1, 2].filter(k => (v.stby[0] >> k) & 1).map(k => 'Fleet ' + (k + 1)).join(' + ') + ': +2 ships on the first attack', '', '', ''));
  if (v.beaten !== undefined && s.planets[v.beaten]) R.push(gyRow(0, 'war', '#FF6B5A', 'BEATEN TO ' + s.planets[v.beaten].name, 'fight anyway, redirect or stand by', 'CHOOSE', 'openBeaten(' + v.beaten + ')', 'due'));
  // ---- NOW ----
  if (v.grudge) R.push(gyRow(0, 'target', '#FF6B5A', 'STRIKE NEXT ROUND', 'the ' + FNAMES[v.grudge.f] + ' are coming for ' + (v.grudge.w >= 0 ? wName(s, v.grudge.w) : 'you'), 'CALM', 'openGrudgeSheet()', 'strike'));
  if (v.off && s.planets[v.off.w] && s.planets[v.off.w].owner === mySlot)
    R.push(gyRow(0, 'war', '#FF6B5A', 'OFFENSIVE NEXT ROUND', 'the ' + FNAMES[v.off.f] + ' want ' + wName(s, v.off.w) + ' back (\u2212' + STRIKE_HIT.offensive + ')', 'GUARD', 'openPlanet(' + v.off.w + ')', 'strike'));
  if (f && f.deal) R.push(gyRow(0, 'gift', '#FFD166', 'DRAW YOUR GAMBIT', f.deal.length + ' card' + (f.deal.length === 1 ? '' : 's') + ', keep one', 'DRAW', 'openFhDraw()', 'due'));
  if (f && fhFinal(s) && s.phase === 0) {
    const open = f.debt.some((d, k) => d[0] > 0 && !((f.called >> k) & 1));
    R.push(gyRow(open ? 0 : 2, 'owe', '#8CE7A0', 'CALL IN YOUR DEBTS', open ? 'the factions you helped will send ships' : 'answered: ' + f.ally.reduce((a, b) => a + b, 0) + ' allied ships', open ? 'CALL' : '', 'openFhDebts()', open ? 'due' : ''));
  }
  if (v.job && s.planets[v.job.w] && v.job.st === 1) R.push(gyRow(0, 'job', '#FFD166', 'JOB OFFER', 'the ' + FACTION_SHORT_NAMES[v.job.f] + ' want ' + wName(s, v.job.w), 'DECIDE', 'openJobSheet()', 'due'));
  sellRows(s).forEach(h => R.push({ pri: 0, html: h.replace('class="gyrow', 'class="gyrow act') }));
  const proms = (s.promises || []).slice().sort((a, b) => a.due - b.due);
  if (proms.length) {
    const p = proms[0], left = p.due - s.round, one = proms.length === 1;
    R.push(gyRow(left <= 0 ? 0 : 2, 'owe', left <= 0 ? '#FFD166' : FACTION_UI_COLORS[p.f], 'YOU OWE \u00b7 ' + (one ? FACTION_SHORT_NAMES[p.f] : proms.length),
      one ? p.text : proms.map(q => FACTION_SHORT_NAMES[q.f]).join(' \u00b7 ') + (left <= 0 ? ' \u00b7 one is due' : ' \u00b7 next due round ' + p.due),
      left <= 0 ? 'DUE' : left + 'R', 'openReputation()', left <= 0 ? 'due' : ''));
  }
  s.planets.forEach((p, i) => { if (p.owner === mySlot && p.bo && s.players[p.bo[0]])  // 3.1-dev.79
    R.push(gyRow(0, 'credits', '#FFD166', 'BUYOUT OFFER', pTag(s, p.bo[0]) + ' offers ' + p.bo[1] + ' CR for ' + p.name, 'ANSWER', 'openBuyout(' + i + ')', 'due')); });
  if (v.bmAsk !== undefined) R.push(gyRow(0, 'target', '#E0475B', 'BLACKMAILED', pTag(s, v.bmAsk) + ' has your file: pay ' + BLACKMAIL_CR + ' CR, or the table hears your debts', 'ANSWER', 'openBlackmailAnswer()', 'strike'));  // 3.1-dev.78
  // 3.1-dev.88 THE DAWN GRIPS THE GALAXY: the Mark, the champion naming it, files for sale
  const dawn = eraOf(s) && eraOf(s).f === 3;
  if (dawn && v.champ && v.champ[3] === mySlot) R.push(gyRow(v.mark >= 0 ? 1 : 0, 'target', '#E0475B', 'NAME THE MARK', v.mark >= 0 ? 'now: ' + feedWho(s, v.mark) : 'put the Dawn\u2019s bounty on a rival', v.mark >= 0 ? '' : 'NAME', 'openMarkSheet()', ''));
  if (dawn && v.mark >= 0) R.push(gyRow(v.mark === mySlot ? 0 : 2, 'target', '#E0475B', v.mark === mySlot ? 'YOU ARE THE MARK' : 'THE MARK', v.mark === mySlot ? '+5 CR to whoever takes your world' : feedWho(s, v.mark) + ' \u00b7 +5 CR a world', '', '', v.mark === mySlot ? 'bad' : ''));
  // 3.1-dev.98 THE BACK CHANNEL
  if (v.bcCan) R.push(gyRow(1, 'holonet', '#5FD4FF', 'BACK CHANNEL', 'make one private offer this round', '', 'openBcSheet()', ''));
  if (v.bcOut) { const o = v.bcOut, who = feedWho(s, o[0]);
    R.push(o[4] === 1 ? gyRow(2, 'holonet', '#5FD4FF', 'OFFER SENT', who + ' \u00b7 waiting for an answer', '', '', '')
         : o[4] === 2 ? gyRow(1, 'holonet', '#5FD4FF', 'YOUR DEAL', who + ' \u00b7 ' + bcText(s, o[1], o[2], mySlot), 'THIS ROUND', '', '')
         : o[4] === 3 ? gyRow(2, 'holonet', '#8FA3B8', 'OFFER DECLINED', who + ' said no', '', '', '')
         : gyRow(2, 'holonet', '#8CE7A0', 'DEAL BROKEN', who + ' paid for it', '', '', '')); }
  if (v.bcIn) { const o = v.bcIn, who = feedWho(s, o[0]);
    R.push(o[4] === 1 ? gyRow(0, 'credits', '#5FD4FF', 'A PRIVATE OFFER', o[3] + ' CR \u00b7 ' + who, 'ANSWER', 'openBcIncoming()', 'due')
                      : gyRow(0, 'owe', '#FFB347', 'YOU PROMISED', bcText(s, o[1], o[2], o[0]) + ' \u00b7 this round', '', '', '')); }
  if (dawn && !v.fileBought) R.push(gyRow(1, 'holonet', '#E0475B', 'BUY A FILE', '6 CR \u00b7 a rival\u2019s Credits, debts and job', '', 'openFileBuy()', ''));
  // ---- YOUR MOVES ----
  if (v.bmReady) R.push(gyRow(1, 'holonet', '#E0475B', 'BLACKMAIL', 'once: a rival pays you ' + BLACKMAIL_CR + ' CR, or the table hears their debts', 'USE', 'openBlackmail()', ''));  // 3.1-dev.78 the Dawn's Family gift
  // (3.1-dev.122: PULL A FILE lives on the WAR TABLE now, above the faces you tap)
  else if (v.file && s.players[v.file.s]) R.push(gyRow(2, 'holonet', '#C8364A', 'THEIR FILE', pTag(s, v.file.s) + (v.fileNext ? ' \u00b7 next pull round ' + v.fileNext : '') + ' \u00b7 tap to read it', '', 'openKit(' + v.file.s + ')', ''));
  movesRows(s).forEach(h => { const u = / strike"/.test(h); R.push({ pri: u ? 0 : 1, html: u ? h.replace('class="gyrow strike"', 'class="gyrow strike urg"') : h }); });  // once a game, a war, the heist, cover
  const hold = ((s.bm && s.bm.hold) || []).filter(x => x >= 0);  // 3.1-dev.88 THE CARGO HOLD
  if (hold.length) R.push(gyRow(1, 'credits', '#C9A227', 'CARGO HOLD \u00b7 ' + hold.length, hold.map(x => LOTS[x][0].toLowerCase()).join(', '), '', 'openBlackMarket()', ''));
  for (let g = 0; g < 4; g++) if ((v.decrees || 0) & (1 << g))
    R.push(gyRow(1, 'crown', '#FFD166', 'YOUR DECREE', ERAS[g].name + ' \u00b7 declare it when you\u2019re ready', '', 'openEraSheet()', 'decree'));
  const hand = f ? fhHand(s) : [];
  if (hand.length) R.push(gyRow(1, 'gift', '#FFD166', 'YOUR GAMBIT' + (hand.length > 1 ? 'S \u00b7 ' + hand.length : ''), hand.map(g => FH_G[g][0].toLowerCase()).join(', '), '', 'openFhHand()', ''));
  // (3.1-dev.77: the black market is the Home MARKETS strip now)
  // ---- GOOD TO KNOW ----
  if (f && f.big >= 0) R.push(gyRow(2, 'credits', '#C9A227', 'BIG TICKET', BIG[f.big][0] + (f.big === 1 && f.bigArg >= 0 ? ' \u00b7 ' + wName(s, f.bigArg) : ''), '', 'openBlackMarket()', ''));
  if (v.job && s.planets[v.job.w] && v.job.st !== 1) R.push(gyRow(2, 'job', '#FFD166', 'JOB \u00b7 ' + FACTION_SHORT_NAMES[v.job.f], 'take ' + wName(s, v.job.w), (v.job.due - s.round + 1 <= 1 ? 'THIS ROUND' : (v.job.due - s.round + 1) + ' ROUNDS'), 'openPlanet(' + v.job.w + ')', ''));
  if (v.race) {
    const what = v.race.w === -2 ? 'any ' + FNAMES[v.race.f ^ 1] + ' world' : wName(s, v.race.w);
    R.push(gyRow(2, 'race', '#5FD4FF', 'RACE', 'first to take ' + what + ' wins the ' + FNAMES[v.race.f] + '\u2019s gratitude (open to everyone)', 'R' + v.race.u, v.race.w >= 0 ? 'openPlanet(' + v.race.w + ')' : '', ''));
  }
  const out = [0, 1, 2, 3].filter(g => tierIdx(s, g) === 0 && !(v.grudge && v.grudge.f === g));
  if (out.length) R.push(gyRow(2, 'target', '#FF6B5A', 'OUT FOR YOU', (out.length === 1 ? 'the ' + FNAMES[out[0]] + ' want you gone' : out.map(g => FACTION_SHORT_NAMES[g]).join(' \u00b7 ')), '', 'openReputation()', 'bad'));
  if (v.spy && s.players[v.spy.w]) {
    const wf = s.players[v.spy.w].chosenFaction;
    R.push(gyRow(2, 'cover', '#C8364A', 'YOUR SPY', (wf >= 0 ? FACTION_SHORT_NAMES[wf] : 'They') + ': ' + v.spy.t.map((t, g) => '<i style="font-style:normal;color:' + FAVOR_COL[t] + '">' + FACTION_SHORT_NAMES[g] + ' ' + (t + 1) + '</i>').join(' \u00b7 '), ' ', 'openReputation()', 'spy'));
  }
  R.sort((a, b) => a.pri - b.pri);  // stable: the order above holds inside a band
  return R;
}
function galaxyAndYouHtml(s) {
  const f = s.fh, R = gyRows(s);
  if (!R.length) return '';
  const open = gyIsOpen(), show = open ? R : R.slice(0, GY_SHOW), more = R.length - show.length, urgent = R.filter(r => r.pri === 0).length;
  return '<div class="gy"><div class="hgtitle gyhead"><span>THE GALAXY AND YOU</span>' + (urgent ? '<small class="gyn">' + urgent + ' NEED' + (urgent === 1 ? 'S' : '') + ' YOU</small>' : '<small>only you see this</small>') +
         (setOn('rivet') ? '<button class="gyhelp" onclick="openGuides(\'gyou\')" aria-label="What is this list?">' + rivetSvg(18) + '</button>' : '') + '</div>' +
         show.map(r => r.html).join('') +
         (more > 0 ? '<button class="gymore" onclick="gySetOpen(true)">+ ' + more + ' MORE</button>'
                   : R.length > GY_SHOW ? '<button class="gymore" onclick="gySetOpen(false)">SHOW LESS</button>' : '') +
         (f && fhFinal(s) && s.phase === 0 ? '<div class="fhlast"><b>LAST CHANCE</b>Credits are worth nothing after this round, except jump passes and High Roller. Every ship you add now is a ship in your last strikes.</div>' : '') + '</div>';
}
// All your gambits in one sheet (the Galaxy and You row merges them).
function openFhHand() {
  const s = lastS, hand = s ? fhHand(s) : []; if (!hand.length) return;
  if (hand.length === 1) return openFhCard(hand[0]);
  sheetShow('fhhand', '<div class="sheetSub" style="letter-spacing:3px">ONLY YOU SEE THESE</div><div class="sheetTitle">YOUR GAMBITS \u00b7 ' + hand.length + '</div>' +
    '<div class="sheetSub">Tap one for what it does and when you play it.</div><div class="gfan">' + hand.map(g => fhCard(g, { mini: true, tap: 'openFhCard(' + g + ')' })).join('') + '</div>' +
    '<button class="pill" style="border-color:var(--line);color:var(--dim)" onclick="sheetHide()">CLOSE</button>', '#FFD166');
}
// ---- 3.1-dev.10 GALAXY NEWS: every public item and your personal one, full screen, one at a time ----
const NEWS_K = { raceFavor: 6, raceCr: 3, despFavor: 8, despCr: 8, jobFavor: 6, jobCr: 4, jobFail: 2 };
const STRIKE_HIT = { empire: 3, uprising: 2, shakedown: 5, assassins: 2, offensive: 3 };
function strikeWhat(f, W) {
  return [W ? 'Next round ' + W + ' rises up against you: \u2212' + STRIKE_HIT.uprising + ' defenders (never below 1).' : 'Next round one of your worlds rises up: \u2212' + STRIKE_HIT.uprising + ' defenders.',
          W ? 'Next round an Imperial fleet hits ' + W + ': \u2212' + STRIKE_HIT.empire + ' defenders. If that leaves 0, ' + W + ' falls.' : 'Next round an Imperial fleet hits one of your worlds.',
          'Next round the Pykes collect: up to ' + STRIKE_HIT.shakedown + ' of your Credits.',
          'Next round the Dawn\u2019s assassins ambush your biggest fleet: \u2212' + STRIKE_HIT.assassins + ' ships (never its last).'][f] || '';
}
function strikeFix(f, W) {  // what you can do about it
  if (f === 1 && W) return ' Or reinforce ' + W + ' to ' + (STRIKE_HIT.empire + 1) + '+ and it holds.';
  if (f === 2) return ' Or spend your Credits first.';
  return '';
}
function wName(s, p) { const n = s.planets && s.planets[p] ? s.planets[p].name : ''; return n.toLowerCase().replace(/(^|\s)\S/g, c => c.toUpperCase()); }
function newsText(s, e, mine) {
  const f = e.who >= 0 && e.who < 4 ? e.who : -1, FN = f >= 0 ? FNAMES[f] : 'galaxy', W = wName(s, e.planet), WU = W.toUpperCase();
  const FU = f >= 0 ? FACTION_SHORT_NAMES[f] : 'THE GALAXY', col = f >= 0 ? FACTION_UI_COLORS[f] : '#5FD4FF';
  const T1 = f >= 0 ? favorWord(f, 0) : '';
  if (e.kind === 11) switch (e.num) {
    case 0: return { head: 'COMMOTION ON ' + WU, line: 'Something is stirring on ' + W + '. Its defenders are weakened.', you: 'Players who knew ' + W + '\u2019s Sympathy learn who is behind it. Everyone else is guessing.', col: '#5FD4FF' };
    case 1: return { head: WU + ' REINFORCED', line: 'The ' + FN + ' shored up ' + W + ', a world they nearly lost.', you: 'Its defenders are stronger again.', col: col, f: f };
    case 2: return { head: 'A RACE FOR ' + WU, line: 'First to take ' + W + ' earns the ' + FN + '\u2019s gratitude.', you: '+' + NEWS_K.raceFavor + ' favor with the ' + FN + ' and +' + NEWS_K.raceCr + ' CR. Open for two rounds.', col: col, f: f };
    case 3: { const g = s.v2 && s.v2.goods && s.v2.goods[e.other]; return { head: 'MARKET DAY', line: 'Something rare at the ' + FN + ' shop' + (g ? ': ' + g[0] : '') + '.', you: 'Only one. Buy it in Prepare.', col: '#FFD166', f: f }; }
    case 4: { const j = s.v2 && s.v2.job, me = j && j.ann && j.f === f;
              return { head: 'THE ' + FU + ' ARE HIRING', line: 'Someone got a private offer.', you: me ? 'It\u2019s you. Check the offer.' : 'Maybe it\u2019s you. Maybe it\u2019s someone across the table.', col: col, f: f }; }
    case 5: return { head: FU + ' EMBARGO', line: 'The ' + FN + ' shut their shop to everyone this round.', you: '', col: col, f: f };
    case 6: return { head: 'THE ' + FU + ' MASS FOR ' + WU, line: 'They want ' + W + ' back from ' + feedWho(s, e.other) + '. Next round they strike: \u2212' + STRIKE_HIT.offensive + ' defenders, and at 0 it falls.', you: e.other === mySlot ? 'That\u2019s your world. Reinforce it.' : 'Factions take their own worlds back late in the war.', col: '#FF6B5A', f: f };
    case 11: return { head: FU + ' OFFENSIVE', line: 'The ' + FN + ' struck ' + W + '. ' + ((s.planets[e.planet] || {}).owner === e.other ? feedWho(s, e.other) + ' held it.' : 'It fell back to them.'), you: '', col: '#FF6B5A', f: f };
    case 7: { const rv = f >= 0 ? FNAMES[f ^ 1] : ''; return { head: 'THE ' + FU + ' ARE DESPERATE', line: 'First to take any ' + rv + ' world earns a huge reward.', you: '+' + NEWS_K.despFavor + ' favor with the ' + FN + ' and +' + NEWS_K.despCr + ' CR. Open for two rounds.', col: col, f: f }; }
    case 8: return { head: 'LAST STAND', line: 'Every home world fortifies: +2 defenders.', you: 'The end is close. Home worlds just got harder to take.', col: '#FF6B5A' };
    case 9: return { head: 'THE ' + FU + ' STRIKE', line: 'The ' + FN + ' came for ' + feedWho(s, e.other) + (e.planet >= 0 ? ' at ' + W : '') + '.', you: e.other === mySlot ? 'They came for you.' : 'This is what happens when you cross them.', col: '#FF6B5A', f: f };
    case 10: return { head: WU + ' IS TAKEN', line: feedWho(s, e.other) + ' won the race. The ' + FN + ' remember it.', you: '', col: col, f: f };
    case 12: { const E = ERAS[f] || ERAS[0];
               return { kick: pTag(s, e.other, true) + ' DECLARES', head: E.name, line: 'A new Era for the whole galaxy, for 2 rounds. Nobody can cancel it.', you: eraRulesHtml(s, f, true), col: E.col, f: f, era: true }; }
    case 15: return { head: 'ON THE WARPATH', line: feedWho(s, e.other) + ' is coming for ' + W + ' this Resolution, with every fleet and 3 more ships.', you: (s.planets[e.planet] || {}).owner === mySlot ? 'That\u2019s your world. Bring ships to it while you can.' : 'Get ready while you can.', col: '#FF6B5A', f: f };
    case 16: case 17: { const L = s.v2 && s.v2.warLast, won = e.num === 16;
               return { kick: 'THE WAR ROOM', head: won ? WU + ' FALLS' : WU + ' HOLDS', line: won ? feedWho(s, e.other) + ' took ' + W + ' in their war.' : W + ' held against ' + feedWho(s, e.other) + '\u2019s war.', you: '', col: won ? '#FF6B5A' : '#8CE7A0', f: f,
                        scene: L && L.w === e.planet ? { kind: 'war', a: L.a, d: L.d, log: L.log, won: !!L.won, label: WU } : null }; }
    case 18: return { head: 'A SUMMIT IS CALLED', line: feedWho(s, e.other) + ' called a ceasefire' + (s.v2 && s.v2.summit ? ' at ' + s.v2.summit.w.filter(i => i >= 0).map(i => wName(s, i)).join(', ') : '') + ' for two rounds.', you: 'Nobody fights there. Break it and ' + MOVE_K.keepers + ' peacekeepers defend.', col: '#5FD4FF', f: f };
    case 19: return { head: 'CEASEFIRE BROKEN', line: feedWho(s, e.other) + ' attacked ' + W + ' under the Summit.', you: 'Peacekeepers joined its defense. Three factions remember it.', col: '#FF6B5A', f: f };
    case 20: return { head: 'THE SUMMIT HELD', line: 'Two quiet rounds. ' + feedWho(s, e.other) + '\u2019s peace held.', you: '', col: '#8CE7A0', f: f };
    case 21: return { head: 'COVER BLOWN', line: feedWho(s, e.other) + ' was living among the ' + FN + ' as one of their own.', you: e.other === mySlot ? 'A tier down with them.' : 'Now you know.', col: '#FF6B5A', f: f };
    case 23: return { head: 'WHISPERS FROM A SAFEHOUSE', line: 'Dawn agents are talking: ' + W + ' holds ' + e.other + ' ship' + (e.other === 1 ? '' : 's') + '.', you: (s.planets[e.planet] || {}).owner === mySlot ? 'That\u2019s your world. Everyone heard.' : 'The whole table heard it.', col: '#C8364A', f: f };
    case 24: return { kick: 'THE FINAL HOUR NEARS', head: 'THE GAMBITS ARE DEALT', line: 'One round after this one. Everyone at the table is now holding something.', you: 'Draw yours, and check the black market: COMMS: THE GALAXY AND YOU.', col: '#FFD166' };
    case 25: case 26: case 27: case 28: return null;  // the finale plays these itself
    case 34: { const by = Math.floor(e.other / 8), item = e.other % 8, B = BIG[item] || ['SOMETHING BIG'];
               return { kick: 'THE BLACK MARKET', head: B[0] + ' SOLD', line: pTag(s, by) + ' bought a ' + B[0].toLowerCase() + (item === 1 && e.planet >= 0 ? ' for ' + W : '') + '.', you: by === mySlot ? 'Everyone knows. Make it count in the Final Hour.' : ['Their biggest strike just got 4 ships bigger.', W + ' will shrug off the first 3 hits in the finale.', 'They have an extra strike coming.', 'They can strike anywhere for free.', 'They\u2019ll see every hidden gambit and strike at the reveal.'][item] || '', col: '#C9A227' }; }
    case 41: return { kick: 'THE BACK CHANNEL', head: 'A DEAL WAS BROKEN', line: 'Someone took Credits to stay away, then didn\u2019t. Nobody is saying who.', you: '', col: '#5FD4FF' };  // 3.1-dev.98
    case 42: return { kick: 'THE DAWN NAMES THE MARK', head: e.other === mySlot ? 'YOU ARE THE MARK' : 'THE MARK', line: feedWho(s, e.other) + ' is the Mark: whoever takes one of their worlds gets 5 CR from the Dawn.', you: e.other === mySlot ? 'Guard your worlds.' : 'Their worlds just got more valuable.', col: '#E0475B', f: 3 };
    case 37: return { kick: 'GALAXY NEWS', head: 'UPRISING ON ' + WU, line: 'The people of ' + W + ' rose up' + (e.other >= 0 ? ' against ' + feedWho(s, e.other) : '') + '. It sides with ' + (f >= 0 ? feedWho(s, s.players.findIndex(p => p.active && p.chosenFaction === f)) : 'someone new') + ' now.',
                      you: e.other === mySlot ? 'It was yours. Strike back this round, before it settles.' : 'It holds with only 2 defenders for now.', col: '#FFC933', f: f };
    case 38: { const by = Math.floor(e.other / 4), hd = e.other % 4, P2 = s.planets[e.planet];
               return { kick: 'GALAXY NEWS', head: 'A BUYOUT OFFER', line: feedWho(s, by) + ' offers ' + (P2 && P2.bo ? P2.bo[1] + ' CR' : 'Credits') + ' for ' + W + ', held by ' + feedWho(s, hd) + '.', you: hd === mySlot ? 'Answer it on Home: sell for half, or match it to keep the world.' : '', col: '#FFD166', f: f }; }
    case 39: { const by = Math.floor(e.other / 4), hd = e.other % 4; return { kick: 'GALAXY NEWS', head: WU + ' BOUGHT OUT', line: feedWho(s, hd) + ' sold ' + W + ' to ' + feedWho(s, by) + '.', you: '', col: '#FFD166', f: f }; }
    case 40: { const by = Math.floor(e.other / 4), hd = e.other % 4; return { kick: 'GALAXY NEWS', head: 'THE OFFER IS MATCHED', line: feedWho(s, hd) + ' paid to keep ' + W + '. ' + feedWho(s, by) + ' got their Credits back.', you: '', col: '#5FD4FF', f: f }; }
    case 36: { const who = Math.floor(e.other / 20), w2 = (e.other % 20) - 1, ws = [e.planet, w2].filter(p => p >= 0).map(p => wName(s, p));
               return { kick: 'GALAXY NEWS', head: 'EXPOSED', line: feedWho(s, who) + ' wouldn\u2019t pay the blackmail. ' + (ws.length ? 'Their debts are out: they owe ' + (f >= 0 ? 'the ' + FNAMES[f] : 'a faction') + ' on ' + ws.join(' and ') + '.' : 'Their file was clean.'),
                        you: who === mySlot ? 'Everyone knows now. Anyone who takes those worlds makes you break your word.' : ws.length ? 'Take one of those worlds and they break their word.' : '', col: '#E0475B', f: 3 }; }
    case 35: return { kick: 'GALAXY NEWS', head: 'PYKE FUEL', line: feedWho(s, e.other) + ' bought Pyke fuel: one of their fleets may move 2 steps this round.', you: e.other === mySlot ? 'Everyone heard. Use it this Movement.' : 'Watch where their fleets go.', col: '#A77BFF', f: 2 };
    case 30: return { kick: 'GALAXY NEWS', head: 'THE PEOPLE RESIST', line: feedWho(s, e.other) + ' bought ' + W + ' in the Rebellion. The Rebels won\u2019t forget it.', you: e.other === mySlot ? 'A step of Rebel favor, gone.' : '', col: '#FFC933', f: 0 };
    case 29: return { kick: 'GALAXY NEWS', head: 'CAUGHT PLAYING BOTH SIDES', line: feedWho(s, e.other) + ' sold the same work to the ' + FN + ' and the ' + FNAMES[RIVAL_OF[f]] + '. They compared notes.', you: e.other === mySlot ? 'Both are coming for you next round. Pay one back to call off its strike: COMMS: THEY BOTH KNOW.' : 'Both factions are coming for them next round.', col: '#FF6B5A', f: f };
    case 22: if (FEED.mine.some(m => m.kind === 12 && m.num === 23 && m.round === e.round && m.who === e.who)) return null;  // 3.1-dev.74: your own heist plays its die instead
             return { head: 'THE ' + FU + ' VAULT IS ROBBED', line: 'Nobody saw a thing.' + (e.other ? ' The getaway roll: ' + e.other + '.' : ''), you: '', col: '#FFD166', f: f };
    case 13: return { head: (ERAS[f] || ERAS[0]).name + ' ENDS', line: 'The galaxy goes back to normal.', you: '', col: '#8FA3B8', f: f };
    case 14: { const mine = e.other === mySlot, lost = e.planet === mySlot && e.other !== mySlot, E = ERAS[f] || ERAS[0];
               if (mine) return { kick: '\uD83D\uDC51 NEW CHAMPION', head: 'CHAMPION OF THE ' + FU, line: 'Nobody stands higher with the ' + FN + '. You hold its decree: ' + E.name + '.', you: 'Declare it from your Favor page when the moment is right. Lose the title first and the decree is gone.', col: '#FFD166', f: f };
               if (lost) return { kick: '\uD83D\uDC51 TITLE LOST', head: e.other >= 0 ? 'THE ' + FU + ' HAVE A NEW CHAMPION' : 'YOU LOST THE ' + FU + '\u2019S TITLE', line: (e.other >= 0 ? feedWho(s, e.other) + ' passed you.' : 'You fell from ' + favorWord(f, 4) + '.') + ' Your decree is gone if you still held it.', you: 'Win the title back and you get a new decree.', col: '#FF6B5A', f: f };
               if (e.other < 0) return null;
               return { head: 'A NEW CHAMPION', line: feedWho(s, e.other) + ' is now Champion of the ' + FN + ' and holds its decree: ' + E.name + '.', you: 'A new Era could come at any time.', col: '#FFD166', f: f }; }
  }
  if (e.kind === 12) switch (e.num) {
    case 0: { const Wt = e.planet >= 0 ? W : '';
              return { kick: 'FOR YOUR EYES ONLY', head: 'THE ' + FU + (f === 1 ? ' IS' : ' ARE') + ' COMING FOR YOU', line: strikeWhat(f, Wt), you: 'Why: you\u2019re \u201C' + T1 + '\u201D to them. Climb out of it before next round (a kept promise, an Audience, a gift) and they stand down.' + strikeFix(f, Wt), col: '#FF6B5A', f: f }; }
    case 1: return { kick: 'FOR YOUR EYES ONLY', head: 'THE ' + FU + ' ARE WATCHING YOU', line: 'You\u2019re \u201C' + (f >= 0 ? favorWord(f, 1) : '') + '\u201D to them. Their shop is shut to you.', you: 'One more bad move and they come for you.', col: '#FFB347', f: f };
    case 2: return { kick: 'A PRIVATE OFFER', head: 'A JOB FROM THE ' + FU, line: 'Take ' + W + jobFrom(s, e.planet) + ' ' + jobIn(s, (s.v2 && s.v2.job && s.v2.job.due) || s.round + 2) + '.', you: 'Pays +' + NEWS_K.jobFavor + ' favor with the ' + FN + ' and +' + NEWS_K.jobCr + ' CR. Take it and fail: \u2212' + NEWS_K.jobFail + ' favor.', col: '#FFD166', f: f };
    case 3: { const pl = s.planets[e.planet], c = e.other === 3 ? 3 : 0, ch = pl && pl.chars && pl.chars[c];
              return { kick: 'FOR YOUR EYES ONLY', head: 'A TIP-OFF', line: 'Your ' + FN + ' contact whispers: ' + W + '\u2019s ' + (c === 3 ? 'Defense' : 'Sympathy') + ' is ' + (ch && ch.value ? ch.value : 'yours to see') + '.', you: 'Added to your intel.', col: '#8CE7A0', f: f }; }
    case 4: { const pr = (s.promises || []).find(p => p.f === f && p.p === e.planet);
              return { kick: 'A REMINDER', head: 'A DEBT COMES DUE', line: 'You owe the ' + FN + (e.planet >= 0 ? ' (' + W + ')' : '') + ': due round ' + e.other + '.', you: pr ? pr.text : '', col: '#FFD166', f: f }; }
    case 5: {
      const why = 'Why: you were \u201C' + T1 + '\u201D to the ' + FN + '. Climb out of it, or they\u2019ll come again.';
      const b = Math.floor(e.other / 10), a = e.other % 10, lost = b - a;
      if (f === 1) return a ? { head: WU + ' HELD', line: 'The Imperial fleet hit ' + W + ': \u2212' + lost + ' defenders. It held.', you: why, col: '#8CE7A0', f: f, scene: { kind: 'world', b: b, a: a, label: WU } }
                            : { head: WU + ' FELL', line: 'The Imperial fleet broke your defenders. ' + W + ' is the Empire\u2019s again.', you: why, col: '#FF6B5A', f: f, scene: { kind: 'world', b: b, a: 0, label: WU } };
      if (f === 0) return { head: 'UPRISING ON ' + WU, line: 'Your world rose up against you: \u2212' + lost + ' defenders.', you: why, col: '#FF6B5A', f: f, scene: { kind: 'world', b: b, a: a, label: WU } };
      if (f === 2) return { head: 'SHAKEDOWN', line: 'The Pykes took ' + e.other + ' Credits from you.', you: why, col: '#FF6B5A', f: f, scene: { kind: 'cr', b: e.other, a: 0, label: 'CREDITS' } };
      return { head: 'ASSASSINS', line: 'They ambushed Fleet ' + (e.planet + 1) + ': you lost ' + lost + ' ship' + (lost === 1 ? '' : 's') + '.', you: why, col: '#FF6B5A', f: f, scene: { kind: 'fleet', b: b, a: a, label: 'FLEET ' + (e.planet + 1) } };
    }
    case 6: return { head: 'THE ' + FU + ' STAND DOWN', line: 'You climbed out of their bad books in time.', you: '', col: '#8CE7A0', f: f };
    case 7: return { kick: 'FOR YOUR EYES ONLY', head: 'IT\u2019S THE ' + FU, line: 'You knew ' + W + '\u2019s old loyalties: the commotion there is the ' + FN + '. ' + W + ' leans ' + FN + ' now.', you: 'Only players who knew ' + W + '\u2019s Sympathy hear this. Keep it quiet, or don\u2019t.', col: col, f: f };
    case 8: return { head: 'JOB DONE', line: 'You took ' + W + ' for the ' + FN + ': +' + NEWS_K.jobFavor + ' favor, +' + NEWS_K.jobCr + ' CR.', you: '', col: '#8CE7A0', f: f };
    case 9: return { head: 'JOB FAILED', line: 'You didn\u2019t take ' + W + ' in time. The ' + FN + ' are disappointed (\u2212' + NEWS_K.jobFail + ' favor).', you: '', col: '#FF6B5A', f: f };
    case 10: return { head: 'YOU WON THE RACE', line: e.other ? 'The ' + FN + ' pay big: +' + NEWS_K.despCr + ' CR, +' + NEWS_K.despFavor + ' favor.' : '+' + NEWS_K.raceCr + ' CR and the ' + FN + '\u2019s gratitude (+' + NEWS_K.raceFavor + ' favor).', you: '', col: '#8CE7A0', f: f };
    case 17: return { kick: 'FOR YOUR EYES ONLY', head: 'WAR IS COMING TO ' + WU, line: feedWho(s, e.other) + ' declared war on ' + W + '. Next Resolution they hit it with ships pulled from all their worlds.', you: 'Reinforce ' + W + ' in Prepare. Those worlds of theirs will be weak that round.', col: '#FF6B5A', f: f };
    case 18: return { head: 'YOUR WARPATH LAPSED', line: 'You never struck ' + W + '. The warpath is spent.', you: '', col: '#8FA3B8', f: f };
    case 19: return { kick: 'A DECISION', head: 'A SUMMIT ON YOUR WAR', line: feedWho(s, e.other) + ' put ' + W + ' under a ceasefire.', you: 'Your choice: pull back (your Warpath comes back to you) or strike anyway (the ceasefire breaks, peacekeepers defend, three factions turn on you). COMMS: YOUR WARPATH.', col: '#5FD4FF', f: f };
    case 20: return { head: 'PEACE DIVIDEND', line: 'Your Summit held both rounds. Every faction thinks a little better of you.', you: '', col: '#8CE7A0', f: f };
    case 21: return { kick: 'DEEP COVER', head: 'YOU\u2019RE ' + favorWord(f, 4).toUpperCase() + ' NOW', line: 'Until the end of round ' + e.other + ' the ' + FN + ' treat you as one of their own.', you: 'Attack one of their worlds and your cover is blown. Plant one story from Home.', col: '#C8364A', f: f };
    case 22: return { head: 'COVER ENDS', line: 'You slip out from among the ' + FN + '. Your real standing is back.', you: '', col: '#8FA3B8', f: f };
    case 23: { const out = Math.floor(e.other / 16), g = (e.other % 16) - 1, G = g >= 0 && s.v2 && s.v2.goods ? s.v2.goods[g][0] : '';
               const head = out === 2 ? 'CAUGHT' : 'CLEAN GETAWAY', col2 = out === 2 ? '#FF6B5A' : '#8CE7A0';
               const line = out === 0 ? '+' + MOVE_K.smoothCr + ' CR' + (G ? ' and ' + G : '') + '. Nobody knows it was you.'
                          : out === 1 ? '+' + MOVE_K.boldCr + ' CR' + (G ? ', ' + G : '') + ' and a stolen ship.' : 'You got out with ' + MOVE_K.failCr + ' CR. The ' + FN + ' know your face now.';
               if (out === 3) return { kick: 'PUSHED YOUR LUCK', head: 'DOUBLED', line: '+' + MOVE_K.boldCr + ' more CR. Get out while you can.', you: '', col: '#8CE7A0', f: f, scene: { kind: 'die', v: e.planet, label: 'ONE MORE ROLL' } };
               if (out === 4) return { kick: 'PUSHED YOUR LUCK', head: 'CAUGHT ON THE WAY OUT', line: 'You lost the ' + MOVE_K.boldCr + ' CR, and the ' + FN + ' know your face now.', you: '', col: '#FF6B5A', f: f, scene: { kind: 'die', v: e.planet, label: 'ONE MORE ROLL' } };
               return { kick: 'THE ' + FU + ' VAULT', head: head, line: line, you: out === 1 ? 'Push your luck? COMMS: PUSH YOUR LUCK, this round only.' : '', col: col2, f: f, scene: e.planet > 0 ? { kind: 'die', v: e.planet, label: 'THE GETAWAY' } : null }; }
    case 36: return { kick: 'A BUYOUT OFFER', head: 'THEY WANT ' + WU, line: feedWho(s, e.other) + ' offers to buy it. Sell for half, or match it to keep it.', you: 'Answer it on Home: BUYOUT OFFER.', col: '#FFD166', f: f };
    case 39: return { kick: 'FOR YOUR EYES ONLY', head: 'A PRIVATE OFFER', line: feedWho(s, e.other) + ' wants a word. Open it on Home.', you: '', col: '#5FD4FF' };  // 3.1-dev.98 THE BACK CHANNEL
    case 40: return { head: e.planet === 1 ? 'DEAL ACCEPTED' : 'OFFER DECLINED', line: feedWho(s, e.other) + (e.planet === 1 ? ' took your Credits. Now they have to keep their word.' : ' said no.'), you: '', col: '#5FD4FF' };
    case 41: return { head: 'THEY BROKE YOUR DEAL', line: feedWho(s, e.other) + ' broke the back-channel deal. You got ' + e.planet + ' CR.', you: '', col: '#8CE7A0' };
    case 42: return { head: 'YOU BROKE A DEAL', line: 'You paid ' + feedWho(s, e.other) + ' back ' + e.planet + ' CR. The table heard a deal was broken.', you: '', col: '#FF6B5A' };
    case 43: { const cr = e.other >> 1, hs = e.other & 1;  // 3.1-dev.102 THE INK WAS STILL WET
      return { kick: 'THE INK WAS STILL WET', head: 'THE DEAL FELL THROUGH', line: W + ' was taken by force before the round ended.' + (cr ? ' +' + cr + ' CR for your trouble.' : '') + (hs ? ' Head start with the ' + FN + '.' : ''), you: 'Your quest progress stays.', col: '#FFB347', f: f }; }
    case 44: return { head: 'BEATEN TO ' + WU, line: feedWho(s, e.other) + ' got there first. Your turn isn\u2019t wasted: fight anyway, redirect, or stand by.', you: '', col: '#FF6B5A', f: f };
    case 45: return { kick: 'GUNS FOR HIRE', head: 'SHIPS FOR HIRE', line: feedWho(s, e.other) + ' offers ships for your next battle.', you: 'Answer it on Comms.', col: '#B48CFF' };
    case 46: return { head: e.planet === 1 ? 'HIRED' : 'NO DEAL', line: feedWho(s, e.other) + (e.planet === 1 ? ' paid for your guns. Your ships fly for them in their next battle.' : ' said no.'), you: '', col: '#B48CFF' };
    case 47: { const back = e.other >> 3, n = e.other & 7;
      return { head: 'YOUR SHIPS CAME HOME', line: back + ' of ' + n + ' lent ship' + (n === 1 ? '' : 's') + ' made it back.' + (back < n ? ' The rest can be repaired.' : ''), you: '', col: '#B48CFF' }; }
    case 37: return { kick: 'THE DAWN GRIPS THE GALAXY', head: 'SOMEONE BOUGHT YOUR FILE', line: 'Files are for sale this Era. Someone now knows your Credits, debts and job.', you: 'You can buy one too: COMMS: BUY A FILE.', col: '#E0475B', f: 3 };
    case 38: return { head: 'INSIDE MAN', line: W + ': ' + e.other + ' defender' + (e.other === 1 ? '' : 's') + ' right now.', you: '', col: '#8CE7A0', f: f };
    case 34: return { kick: 'FOR YOUR EYES ONLY', head: 'THEY HAVE YOUR FILE', line: feedWho(s, e.other) + ' wants ' + BLACKMAIL_CR + ' CR, or the table hears your debts.', you: 'Answer it on Home: BLACKMAILED.', col: '#E0475B', f: 3 };
    case 35: return { head: 'THEY PAID', line: feedWho(s, e.other) + ' paid you ' + BLACKMAIL_CR + ' CR to keep quiet.', you: '', col: '#FFD166', f: 3 };
    case 24: return { head: 'JOB TAKEN', line: 'You took the ' + FN + '\u2019s job: ' + W + '.', you: '', col: '#FFD166', f: f };
    case 26: return { head: 'PAID OFF', line: 'You paid the ' + FN + ' ' + e.other + ' Credits. The strike is off, but they don\u2019t trust you yet.', you: '', col: '#FFB347', f: f };
    case 27: { const d = DEEDS[f] && DEEDS[f][e.other]; return { head: 'AMENDS MADE', line: d ? d[2].replace(/W/g, W) : 'You made amends.', you: 'The strike is off. The ' + FN + ' respect it.', col: '#8CE7A0', f: f }; }
    case 28: { const rode = e.other >= 2, home = e.other % 2 === 1;
               return { head: home ? 'YOUR SHIP IS HOME' : 'YOUR SHIP DIDN\u2019T COME BACK', line: rode ? (home ? 'Your blade rode with the assassins and made it back to Fleet ' + (e.planet + 1) + '.' : 'Your blade rode with the assassins. It never came back.') : 'Your ship is back with Fleet ' + (e.planet + 1) + '.', you: '', col: home ? '#8CE7A0' : '#FF6B5A', f: f }; }
    case 30: return { head: 'SOLD TWICE', line: 'The ' + FN + ' paid you ' + e.other + ' CR' + (e.planet >= 0 ? ' for ' + W : '') + '.', you: '', col: '#FFD166', f: f };
    case 33: { const won = e.other >= 2, wh = e.other % 2 === 1, good = wh ? !won : won;
               return { head: wh ? (good ? 'YOUR WHISPER LANDED' : 'YOUR WHISPER BACKFIRED') : (good ? 'YOUR GOOD WORD PAID OFF' : 'YOU VOUCHED FOR THE WRONG ONE'), line: 'The talk on ' + W + (won ? ' went their way.' : ' fell apart.') + ' The ' + FN + (good ? ' think more of you.' : ' think less of you.'), you: '', col: good ? '#8CE7A0' : '#FF6B5A', f: f }; }
    case 32: return { head: W.toUpperCase() + ' IS ONE OF YOURS', line: 'After ' + 3 + ' rounds under your flag, ' + W + '\u2019s people side with the ' + FN + ' now.', you: '', col: '#8CE7A0', f: f };
    case 31: return { head: 'PAID BACK', line: 'You paid the ' + FN + ' ' + e.other + ' CR. They\u2019ll stand down.', you: '', col: '#8CE7A0', f: f };
    case 29: return { kick: 'FOR YOUR EYES ONLY', head: 'A FAMILIAR SHIP', line: 'One of ' + feedWho(s, e.other) + '\u2019s ships flew with the assassins.', you: 'They made their peace with the Crimson Dawn. You paid for it.', col: '#C8364A', f: f };
    case 25: return { head: e.other ? 'JOB LAPSED' : 'JOB REFUSED', line: e.other ? 'You let the ' + FN + '\u2019s job on ' + W + ' pass.' : 'You turned the ' + FN + ' down (' + W + ').', you: '', col: '#8FA3B8', f: f };
    case 14: return { head: 'THE TAX ROLLS', line: 'You paid ' + e.other + ' Credit' + (e.other === 1 ? '' : 's') + ' to the Empire.', you: '', col: '#C9D2DC', f: f };
    case 15: return { head: 'TAXES COLLECTED', line: '+' + e.other + ' Credits from the Imperial tax rolls.', you: '', col: '#8CE7A0', f: f };
    case 16: return { head: 'IMPERIAL RECORDS', line: W + '\u2019s Defense is in your intel now.', you: '', col: '#C9D2DC', f: f };
    case 12: return { kick: 'A REMINDER', head: 'THE ' + FU + ' ARE WAITING', line: 'Your job: take ' + W + ' ' + jobIn(s, e.other) + '.', you: 'Pays +' + NEWS_K.jobFavor + ' favor and +' + NEWS_K.jobCr + ' CR. Miss it: −' + NEWS_K.jobFail + ' favor.', col: '#FFD166', f: f };
    case 11: { const b = Math.floor(e.other / 10), a = e.other % 10;
               return a ? { head: WU + ' HELD', line: 'The ' + FN + ' offensive hit ' + W + ': \u2212' + (b - a) + ' defenders. It held.', you: '', col: '#8CE7A0', f: f, scene: { kind: 'world', b: b, a: a, label: WU } }
                        : { head: WU + ' FELL', line: 'The ' + FN + ' offensive took ' + W + ' back.', you: '', col: '#FF6B5A', f: f, scene: { kind: 'world', b: b, a: 0, label: WU } }; }
    case 13: return { kick: 'FOR YOUR EYES ONLY', head: 'THE ' + FU + ' WANT ' + WU + ' BACK', line: 'Next round their offensive hits ' + W + ': \u2212' + STRIKE_HIT.offensive + ' defenders. If that leaves 0, it falls.', you: 'Reinforce ' + W + ' to ' + (STRIKE_HIT.offensive + 1) + ' or more and it holds.', col: '#FF6B5A', f: f };
  }
  return null;
}
const ANN = { q: [], open: false };
function annKey() { return 'sf_ann_' + FEED.g; }
// 3.1-dev.16 (user: "your mockup of galaxy news puts it all on one screen"): the round-start items go on ONE
// screen; a strike landing still plays its own animation first; things that happen mid-round keep their own card.
const NOTIFY_ONLY = [14, 15, 16, 24, 25, 26, 27, 28, 30, 31, 32, 33];  // (+ dev.42 amends made, a lent ship back)                    // personal notices that are notifications, not cards
const BEAT_PUB = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 13, 20];  // public news made at a round start
const BEAT_MINE = [0, 1, 2, 3, 4, 6, 7, 9, 12, 13, 18, 20, 22];
const ANIM_MINE = [5, 11];                                     // strikes / offensives landing: animated first
function annCollect(s) {  // queue this round's news this phone hasn't shown yet
  let seen = { p: 0, m: 0 };
  try { seen = JSON.parse(localStorage.getItem(annKey()) || '{"p":0,"m":0}'); } catch (e) {}
  const pubs = FEED.pub.filter(e => e.kind === 11 && e.seq > seen.p && e.round === s.round);
  const mine = FEED.mine.filter(e => e.kind === 12 && !NOTIFY_ONLY.includes(e.num) && e.seq > seen.m && e.round === s.round);
  if (!pubs.length && !mine.length) return;
  const beatPub = pubs.filter(e => BEAT_PUB.includes(e.num)), beatMine = mine.filter(e => BEAT_MINE.includes(e.num));
  mine.filter(e => ANIM_MINE.includes(e.num)).forEach(e => ANN.q.push({ e: e, mine: true }));
  if (beatPub.length || beatMine.length) ANN.q.push({ beat: { pub: beatPub, mine: beatMine } });
  pubs.filter(e => !BEAT_PUB.includes(e.num)).forEach(e => ANN.q.push({ e: e, mine: false }));
  mine.filter(e => !BEAT_MINE.includes(e.num) && !ANIM_MINE.includes(e.num)).forEach(e => ANN.q.push({ e: e, mine: true }));
  seen.p = Math.max(seen.p, ...FEED.pub.map(e => e.seq), 0); seen.m = Math.max(seen.m, ...FEED.mine.map(e => e.seq), 0);
  try { localStorage.setItem(annKey(), JSON.stringify(seen)); } catch (e) {}
}
// 3.1-dev.11 a strike LANDS: their ships sweep in, bolts, your garrison / ships / Credits count down, then the result.
const SHIP_URLS = {};
function shipUrlFor(f) { if (!(f in SHIP_URLS)) { const c = spriteCanvas(FACTION_SHIP[f] || 'rebel', '', false); SHIP_URLS[f] = c ? c.toDataURL() : ''; } return SHIP_URLS[f]; }
function strikeSceneHtml(s, t) {
  const sc = t.scene, me = s.players[mySlot], myF = me ? me.chosenFaction : 0, foe = shipUrlFor(t.f);
  if (sc.kind === 'war') return '<div class="warScene"><div><small>ATTACK</small><div id="wsA">' + pnumSvg(sc.a, '#E8EEF5', 4) + '</div></div><div class="wsDice"><div id="wsDa" class="die">' + pnumSvg(0, '#05070C', 3) + '</div><div id="wsDd" class="die">' + pnumSvg(0, '#05070C', 3) + '</div></div><div><small>' + sc.label + '</small><div id="wsD">' + pnumSvg(sc.d, '#E8EEF5', 4) + '</div></div></div>';
  if (sc.kind === 'die') return '<div class="dieScene"><small>' + sc.label + '</small><div id="dsDie" class="die big">' + pnumSvg(1, '#05070C', 5) + '</div></div>';
  const pips = Array.from({ length: sc.b }, (x, i) => sc.kind === 'fleet' ? '<img class="pixel" src="' + shipUrlFor(myF) + '" alt="">' : '<i' + (sc.kind === 'cr' ? ' class="cr"' : '') + '></i>').join('');
  return '<div class="strk" id="strk"><div class="strkFoe">' + [0, 1, 2].map(() => '<img class="pixel" src="' + foe + '" alt="">').join('') + '</div><div class="strkBolt" id="strkBolt"></div>' +
         '<div class="strkMe" id="strkMe"><div class="strkLbl">' + sc.label + '</div><div class="strkNum" id="strkNum">' + pnumSvg(sc.b, '#E8EEF5', 3) + '</div><div class="strkPips" id="strkPips">' + pips + '</div></div></div>';
}
function strikePlay(t) {
  const sc = t.scene, card = document.getElementById('annCard'), lost = sc.b - sc.a;
  const T = ANN.timers = [];
  if (sc.kind === 'war') {  // exchange by exchange: both dice, the loser's count drops
    let a = sc.a, d = sc.d;
    const step = sc.log.length > 16 ? 170 : 300;
    sc.log.forEach((x, k) => T.push(setTimeout(() => {
      const ad = x >> 4, dd = x & 15;
      if (ad > dd) d--; else a--;
      const el = id => document.getElementById(id);
      if (el('wsDa')) el('wsDa').innerHTML = pnumSvg(ad, '#05070C', 3);
      if (el('wsDd')) el('wsDd').innerHTML = pnumSvg(dd, '#05070C', 3);
      if (el('wsDa')) el('wsDa').classList.toggle('win', ad > dd);
      if (el('wsDd')) el('wsDd').classList.toggle('win', ad <= dd);
      if (el('wsA')) el('wsA').innerHTML = pnumSvg(Math.max(0, a), a > 0 ? '#E8EEF5' : '#FF6B5A', 4);
      if (el('wsD')) el('wsD').innerHTML = pnumSvg(Math.max(0, d), d > 0 ? '#E8EEF5' : '#FF6B5A', 4);
    }, 300 + k * step)));
    T.push(setTimeout(() => card.classList.add('done'), 300 + sc.log.length * step + 400));
    return;
  }
  if (sc.kind === 'die') {
    for (let k = 0; k < 10; k++) T.push(setTimeout(() => { const el = document.getElementById('dsDie'); if (el) el.innerHTML = pnumSvg(1 + Math.floor(Math.random() * 6), '#05070C', 5); }, 60 + k * 80));
    T.push(setTimeout(() => { const el = document.getElementById('dsDie'); if (el) { el.innerHTML = pnumSvg(sc.v, '#05070C', 5); el.classList.add('win'); } }, 900));
    T.push(setTimeout(() => card.classList.add('done'), 1300));
    return;
  }
  T.push(setTimeout(() => { const el = document.getElementById('strk'); if (el) el.classList.add('go'); }, 60));
  for (let k = 0; k < lost; k++) T.push(setTimeout(() => {
    const bolt = document.getElementById('strkBolt'), meEl = document.getElementById('strkMe'), pips = document.querySelectorAll('#strkPips > *');
    if (bolt) { bolt.classList.remove('fire'); void bolt.offsetWidth; bolt.classList.add('fire'); }
    if (meEl) { meEl.classList.remove('hit'); void meEl.offsetWidth; meEl.classList.add('hit'); }
    const pip = pips[sc.b - 1 - k]; if (pip) pip.classList.add('gone');
    const num = document.getElementById('strkNum'); if (num) num.innerHTML = pnumSvg(sc.b - k - 1, sc.b - k - 1 ? '#E8EEF5' : '#FF6B5A', 3);
  }, 700 + k * 380));
  T.push(setTimeout(() => card.classList.add('done'), 700 + lost * 380 + 350));
}
function strikeFinish() {  // a tap skips straight to the result
  (ANN.timers || []).forEach(clearTimeout); ANN.timers = [];
  const t = ANN.cur, card = document.getElementById('annCard');
  if (!t || !t.scene) return;
  if (t.scene.kind === 'war') {
    let a = t.scene.a, d = t.scene.d; t.scene.log.forEach(x => { if ((x >> 4) > (x & 15)) d--; else a--; });
    const el = id => document.getElementById(id);
    if (el('wsA')) el('wsA').innerHTML = pnumSvg(Math.max(0, a), a > 0 ? '#E8EEF5' : '#FF6B5A', 4);
    if (el('wsD')) el('wsD').innerHTML = pnumSvg(Math.max(0, d), d > 0 ? '#E8EEF5' : '#FF6B5A', 4);
    card.classList.add('done'); return;
  }
  if (t.scene.kind === 'die') { const el = document.getElementById('dsDie'); if (el) { el.innerHTML = pnumSvg(t.scene.v, '#05070C', 5); el.classList.add('win'); } card.classList.add('done'); return; }
  const el = document.getElementById('strk'); if (el) el.classList.add('go');
  document.querySelectorAll('#strkPips > *').forEach((p, i) => { if (i >= t.scene.a) p.classList.add('gone'); });
  const num = document.getElementById('strkNum'); if (num) num.innerHTML = pnumSvg(t.scene.a, t.scene.a ? '#E8EEF5' : '#FF6B5A', 3);
  card.classList.add('done');
}
function annOpen(s, e, mine) {
  const t = newsText(s, e, mine); if (!t) return false;
  const card = document.getElementById('annCard'), lyr = document.getElementById('lyrAnn');
  card.style.setProperty('--ac', t.col || '#5FD4FF');
  ANN.cur = t;
  card.className = 'annCard' + (t.scene ? ' anim' : '');
  card.innerHTML = (t.scene ? strikeSceneHtml(s, t) : '') + (t.scene ? '' : t.f >= 0 ? '<div class="annIns">' + insigniaSvg(t.f, 64) + '</div>' : '<div class="annIns">' + pixIcon('holonet', t.col || '#5FD4FF', 5) + '</div>') +
    '<div class="annKick">' + (t.kick || (mine ? 'YOUR NEWS' : 'GALAXY NEWS')) + ' \u00b7 ROUND ' + s.round + '</div>' +
    '<div class="annHead">' + t.head + '</div><div class="annLine">' + t.line + '</div>' +
    '<div class="annRd">' + newsReadout(s, e, t, 'n' + e.round + '_' + e.kind + '_' + e.num + '_' + e.planet + '_' + e.other) + '</div>' + '<div class="annTap">TAP TO CONTINUE</div>';  // 3.1-dev.46
  lyr.classList.add('on'); ANN.open = true;
  if (t.scene) strikePlay(t);
  return true;
}
const GRUDGE_LINES = ['We trusted you once. Our people remember who sold them out.', 'Your name is on a list, commander. Lists get shorter.',
                      'You owe more than Credits now. We always collect.', 'Loose ends get tied. Sleep lightly.'];
function grudgeHolo(s, f) {  // the faction's voice, as a hologram, typing its threat
  const v = commanderVoice(f, s.players[mySlot] ? s.players[mySlot].commander : -1);
  return '<div class="ghHolo" style="--hc:' + FACTION_UI_COLORS[f] + '">' + (v >= 0 && ART ? '<img class="pixel" src="' + portraitURL(v, 3, true) + '" alt="">' : insigniaSvg(f, 48)) +
         '<div class="ghLine"><span>\u201C' + GRUDGE_LINES[f] + '\u201D</span></div></div>';
}
function openGrudgeSheet() {
  const s = lastS, g = s && s.v2 && s.v2.grudge; if (!g) return;
  const t = newsText(s, { kind: 12, num: 0, who: g.f, planet: g.w, other: 0, round: s.round }, true);
  const W = g.w >= 0 ? wName(s, g.w) : '', key = 'gr' + g.f + '_' + g.r;  // 3.1-dev.46
  const hit = ['UPRISING \u00b7 \u2212' + STRIKE_HIT.uprising + ' DEFENDERS' + (W ? ' \u00b7 ' + W.toUpperCase() : ''), 'IMPERIAL STRIKE \u00b7 \u2212' + STRIKE_HIT.empire + ' DEFENDERS' + (W ? ' \u00b7 ' + W.toUpperCase() : ''),
               'SHAKEDOWN \u00b7 UP TO ' + STRIKE_HIT.shakedown + ' CR', 'ASSASSINS \u00b7 \u2212' + STRIKE_HIT.assassins + ' SHIPS \u00b7 YOUR BIGGEST FLEET'][g.f];
  const rd = readoutHtml([['STANDING', standingHtml(g.f, 0, 0)], ['WHEN', 'START OF ROUND ' + g.r], ['fx', 'THE HIT', [['bad', hit, '']]]], key) +
             rivetSay(key, 'The ' + FNAMES[g.f] + ' have had enough of you. ' + strikeWhat(g.f, W) + ' You have until then: make amends (the deed repairs the most trust), pay them off, or take the hit.');
  sheetShow('grudge', '<div class="sheetTitle" style="color:#FF6B5A">' + t.head + '</div>' + grudgeHolo(s, g.f) + rd + amendsHtml(s) + payOffBtn(s) +
    '<div class="biAct"><button class="ghost" onclick="sheetHide()">TAKE THE HIT</button></div>', '#FF6B5A');
}
// 3.1-dev.42 MAKE AMENDS (user 2026-11-11): three deeds per faction, the same three kinds -- a SHIP, a ROUND, a WORLD.
// A deed calls the strike off and lifts your favor a full step; paying only half a step.
const DEEDS = [  // [faction][kind] = name, what it costs (W = the world you pick), the line when it's done
  [ ['JOIN THE CELL', 'Three ships from your biggest fleet serve the Rebels for two rounds, then come home.', 'Three of your ships fly with the Rebellion for two rounds.'],
    ['STAND DOWN', 'Your fleets make no attacks this round. The people need peace.', 'Your fleets stand down this round.'],
    ['SHELTER REFUGEES', 'Your transports carry refugees from W: it loses 3 defenders for good.', 'Refugees found shelter on W. It lost three ships.'] ],
  [ ['LEND TROOPS', 'Four of W\u2019s defenders join the Imperial ranks, for good. It keeps at least one.', 'Four of W\u2019s ships serve the Empire now.'],
    ['ESCORT DUTY', 'Your fleets escort an Imperial convoy: no attacks this round.', 'Your fleets fly escort this round.'],
    ['LOCKDOWN', 'Imperial officers run W for three rounds: you can\u2019t add defenders there.', 'W is under Imperial lockdown for three rounds.'] ],
  [ ['HAND OVER TWO HAULERS', 'Two ships from your biggest fleet become Pyke haulers, for good.', 'You handed the Pykes two haulers.'],
    ['HAUL THEIR CARGO', 'Your holds are full of spice: no attacks this round.', 'Your holds are full of Pyke spice this round.'],
    ['PUT UP COLLATERAL', 'The Pykes hold W as collateral: they take its income for the next three rounds.', 'W is Pyke collateral. Its next three incomes are theirs.'] ],
  [ ['LEND TWO BLADES', 'Two ships from your biggest fleet ride with the Dawn\u2019s assassins on their next strike. Each rolls a die: 1\u20132 and it doesn\u2019t come back. Their victim learns they were yours.', 'Two blades of yours ride with the Dawn now.'],
    ['LIE LOW', 'The Dawn wants you quiet: no attacks this round.', 'You lie low this round.'],
    ['SAFEHOUSE TALK', 'The Dawn\u2019s agents gossip: the whole table learns how many defend W.', 'The whole table heard how many defend W.'] ] ];
const DEED_KIND = ['A SHIP', 'A ROUND', 'A WORLD'];
let deedPick = -1;  // the deed whose world list is open
function amendsHtml(s) {
  const g = s.v2 && s.v2.grudge; if (!g || g.f < 0 || g.f > 3) return '';
  const want = g.dd === undefined ? -1 : g.dd;
  let h = '<div class="amendHead">MAKE AMENDS<small>' + (want >= 0 ? 'They ask one thing of you. Do it and the strike is off, and they\u2019ll respect it: favor up a full step.' : 'There\u2019s nothing they\u2019d take from you right now.') + '</small></div>';
  for (let k = 0; k < 3; k++) {
    if (k !== want) continue;
    const d = DEEDS[g.f][k], ok = g.dk && g.dk[k], ws = (g.dw && g.dw[k]) || [], needW = k === 2 || (k === 0 && g.f === 1);
    h += '<div class="amend' + (ok ? '' : ' off') + '"><div class="amTop"><b>' + d[0] + '</b><span>' + DEED_KIND[k] + '</span></div><div class="amLine">' + d[1].replace(/W/g, 'a world you pick') + '</div>';
    if (!ok) h += '<div class="biNote">You can\u2019t right now.</div>';
    else if (!needW) h += '<div class="biAct"><button onclick="doDeed(' + k + ',-1)">DO IT</button></div>';
    else if (deedPick !== k) h += '<div class="biAct"><button onclick="deedPick=' + k + ';openGrudgeSheet()">PICK A WORLD</button></div>';
    else h += '<div class="biAct amWorlds">' + ws.map(w => '<button onclick="doDeed(' + k + ',' + w + ')">' + wName(s, w).toUpperCase() + (g.f === 3 && k === 2 ? '' : ' \u00b7 ' + ((s.planets[w] || {}).defense || 0)) + '</button>').join('') + '</div>';
    h += '</div>';
  }
  return h;
}
async function doDeed(k, w) {
  deedPick = -1;
  const r = await post('/grudge_deed?id=' + encodeURIComponent(myId) + '&k=' + k + (w >= 0 ? '&w=' + w : ''));
  await poll();
  if (r && r.ok === false && r.error) { notify('NOT NOW', r.error, '#FF6B5A', 4000); return; }
  sheetHide();
  if (ANN.cur && ANN.cur.beat && lastS) document.getElementById('annCard').innerHTML = beatHtml(lastS, ANN.cur.beat);
}
function payOffBtn(s) {
  const v = s.v2 || {}, cost = v.grudgePay || 12, me = s.players[mySlot];
  return '<div class="biAct"><button onclick="payOffGrudge()"' + (me && me.credits >= cost ? '' : ' disabled') + '>PAY THEM OFF \u00b7 ' + cost + ' CR</button></div><div class="biNote">Calls the strike off. Favor up a little; they don\u2019t trust you yet.</div>';
}
async function payOffGrudge() {
  await post('/grudge_pay?id=' + encodeURIComponent(myId)); await poll();
  sheetHide();
  if (ANN.cur && ANN.cur.beat && lastS) document.getElementById('annCard').innerHTML = beatHtml(lastS, ANN.cur.beat);
}
function beatItemHtml(s, k, e, mine, extra) {
  const t = newsText(s, e, mine); if (!t) return '';
  const key = 'b' + e.round + '_' + e.kind + '_' + e.num + '_' + e.planet + '_' + e.other;  // 3.1-dev.46: "what it means" behind Rivet
  return '<div class="beatItem" style="--ac:' + (t.col || '#5FD4FF') + '"><div class="biTop"><b>' + k + ' \u00b7 ' + t.head + '</b><span>' + (mine ? 'JUST FOR YOU' : 'THE WHOLE TABLE') + '</span></div>' +
         '<div class="biLine">' + (t.you ? rivetBtn(key) : '') + t.line + '</div>' + (t.you ? rivetSay(key, t.you, e.planet >= 0 ? ['SHOW ME ON THE MAP', 'beatMap(' + e.planet + ')'] : null) : '') + (extra || '') + '</div>';
}
function beatHtml(s, b) {
  let k = 0, items = '';
  const who = b.mine.filter(e => e.num === 7);  // "only you know" rides with the commotion it explains
  for (const e of b.pub) {
    const w = e.num === 0 ? who.find(x => x.planet === e.planet) : null, wt = w ? newsText(s, w, true) : null;
    items += beatItemHtml(s, ++k, e, false, wt ? '<div class="biOnly"><b>ONLY YOU KNOW</b>' + wt.line + '</div>' : '');
  }
  for (const e of b.mine) {
    if (e.num === 7 && b.pub.some(p => p.num === 0 && p.planet === e.planet)) continue;
    let extra = '';
    const j = s.v2 && s.v2.job;
    if (e.num === 0) extra = grudgeHolo(s, e.who) + (s.v2 && s.v2.grudge && s.v2.grudge.f === e.who ? '<div class="biAct"><button onclick="annTap(null, true); setTimeout(openGrudgeSheet, 320)">CALM THEM</button></div><div class="biNote">Make amends, pay them off, or take the hit.</div>' : '<div class="biNote">CALLED OFF</div>');  // 3.1-dev.20 / dev.42
    if (e.num === 2) extra = j && j.st === 1 && j.w === e.planet
      ? '<div class="biAct"><button onclick="beatJob(1)">TAKE IT</button><button onclick="beatJob(0)">PASS</button><button onclick="beatMap(' + e.planet + ')">SEE IT ON THE MAP</button></div><div class="biNote">It waits on Home until Prepare ends.</div>'
      : '<div class="biNote">' + (j && j.st === 2 && j.w === e.planet ? 'TAKEN' : 'ANSWERED') + '</div>';
    items += beatItemHtml(s, ++k, e, true, extra);
  }
  const sit = gyRows(s).filter(r => r.pri === 0 && !/openJobSheet|openGrudgeSheet/.test(r.html));  // 3.1-dev.88 YOUR SITUATION (the job + grudge are items above)
  if (sit.length) items += '<div class="sitHead">YOUR SITUATION</div><div class="sitList gdlist">' +
    sit.map(r => r.html.replace(/onclick="([^"]*)"/, (m, c) => 'onclick="annTap(null, true); setTimeout(function () { ' + c + ' }, 320)"')).join('') + '</div>';
  return '<div class="annKick">ROUND ' + s.round + ' \u00b7 BEFORE PREPARE</div><div class="annHead" style="--ac:#5FD4FF">GALAXY NEWS</div>' +
         '<div class="beatSub">Look at the table. Here\u2019s what it means for you.</div>' + items +
         '<button class="pill beatGo" onclick="annTap(event, true)">TO PREPARE</button>';
}
function beatOpen(s, b) {
  const card = document.getElementById('annCard'), lyr = document.getElementById('lyrAnn');
  ANN.cur = { beat: b };
  card.className = 'annCard beat';
  card.style.setProperty('--ac', '#5FD4FF');
  card.innerHTML = beatHtml(s, b);
  lyr.classList.add('on'); ANN.open = true;
  return true;
}
async function beatJob(take) {
  await post('/job?id=' + encodeURIComponent(myId) + '&take=' + take);
  await poll();
  if (ANN.cur && ANN.cur.beat && lastS) document.getElementById('annCard').innerHTML = beatHtml(lastS, ANN.cur.beat);
}
function beatMap(w) { annTap(null, true); setTimeout(() => { gFilter = 2; openGalaxy(); setTimeout(() => gPick(w), 350); }, 300); }
function annTap(ev, force) {
  if (ANN.cur && ANN.cur.beat && !force) return;  // the news screen closes with its own button
  const card = document.getElementById('annCard');
  if (card.classList.contains('anim') && !card.classList.contains('done')) { strikeFinish(); return; }  // first tap: skip to the result
  (ANN.timers || []).forEach(clearTimeout); ANN.timers = [];
  document.getElementById('lyrAnn').classList.remove('on');
  ANN.cur = null;
  setTimeout(() => { ANN.open = false; if (lastS) render(lastS); }, 260);
}
function annPump(s) {
  if (ANN.open || !ANN.q.length || !s || s.stage !== 1) return;
  if (s.battleActive || battleMode || victory || bannerOpen()) return;
  while (ANN.q.length) { const it = ANN.q.shift(); if (it.beat ? beatOpen(s, it.beat) : annOpen(s, it.e, it.mine)) return; }
}
// 3.1-dev.29: a late-game job can be another player's world -- say whose.
function jobFrom(s, w) { const p = s && s.planets && s.planets[w]; return p && p.owner >= 0 && p.owner !== mySlot ? ' from ' + feedWho(s, p.owner) : ''; }
// 3.1-dev.45 THE JOB OFFER AS A HOLOGRAM (user 2026-11-11: "it puts a face to the job"): your contact in that faction makes
// the offer in their own voice, like a grudge -- then the terms and TAKE IT / PASS.
const JOB_LINES = ['W has been under the boot too long. Free it, and the Rebellion will remember your name.',
                   'W defies the Emperor. Bring it to heel, and the Empire will see that you are rewarded.',
                   'W is bad for business. Take it off the board. We pay our friends, and we pay on time.',
                   'There is someone on W we would like gone. Take the world. Ask no questions. Be paid.'];
function jobHolo(s, f, W) {
  const v = commanderVoice(f, s.players[mySlot] ? s.players[mySlot].commander : -1);
  return '<div class="ghHolo" style="--hc:' + FACTION_UI_COLORS[f] + '">' + (v >= 0 && ART ? '<img class="pixel" src="' + portraitURL(v, 3, true) + '" alt="">' : insigniaSvg(f, 48)) +
         '<div class="ghLine"><span>\u201C' + JOB_LINES[f].replace('W', W) + '\u201D</span></div></div>';
}
// The job offer: a decision, so a bottom sheet.
let jobSheetSeen = '';
function openJobSheet() {
  const s = lastS, j = s && s.v2 && s.v2.job; if (!j || j.st !== 1) return;
  const W = s.planets[j.w] ? s.planets[j.w].name : '';
  sheetShow('job', '<div class="ldhead">' + insigniaSvg(j.f, 30) + '<div><b style="color:' + FACTION_UI_COLORS[j.f] + '">A JOB FROM THE ' + FACTION_SHORT_NAMES[j.f] + '</b><small>Only you see this offer.</small></div></div>' +
    jobHolo(s, j.f, wName(s, j.w)) +
    readoutHtml([['TARGET', worldLine(s, j.w)], ['DUE', jobIn(s, j.due).toUpperCase()], ['fx', 'PAY', [['good', '+' + NEWS_K.jobFavor + ' ' + FACTION_SHORT_NAMES[j.f] + ' FAVOR', ''], ['good', '+' + NEWS_K.jobCr + ' CR', '']]],
                 ['fx', 'IF YOU FAIL', [['bad', '\u2212' + NEWS_K.jobFail + ' ' + FACTION_SHORT_NAMES[j.f] + ' FAVOR', '']]]], 'job' + j.f + '_' + j.w + '_' + j.due) +
    rivetSay('job' + j.f + '_' + j.w + '_' + j.due, 'Take ' + wName(s, j.w) + jobFrom(s, j.w) + ' ' + jobIn(s, j.due) + ' and the ' + FNAMES[j.f] + ' pay you in Credits and favor. Passing costs you nothing. Saying yes and failing makes them a little colder. Decide before Prepare ends.',
             ['SHOW ME ' + W, 'sheetHide();gFilter=0;openGalaxy();setTimeout(() => gPick(' + j.w + '), 350)']) +
    '<button class="pill" style="border-color:#FFD166;color:#FFD166" onclick="answerJob(1)">TAKE THE JOB</button>' +
    '<button class="pill" style="border-color:var(--line);color:var(--dim)" onclick="answerJob(0)">PASS</button>' +
    '<button class="back" style="margin-top:4px" onclick="sheetHide();gFilter=2;openGalaxy();setTimeout(() => gPick(' + j.w + '), 350)">SEE ' + W + ' ON THE MAP</button>', FACTION_UI_COLORS[j.f]);
}
async function answerJob(take) { sheetHide(); await post('/job?id=' + encodeURIComponent(myId) + '&take=' + take); poll(); }
// ---- 3.1-dev.12 CHAMPIONS + ERAS ----
const ERAS = [  // 3.1-dev.88 THE NEW FOUR (user 2026-11-18): 2 rounds, locked, every sting hits everyone (the champion too)
  { name: 'THE REBELLION RISES', col: '#FFC933', line: '#FFC933', tag: 'HOPE', brief: 'shorter negotiations \u00b7 a free repair \u00b7 uprisings can be tried again',
    good: ['HOPE SPREADS', 'Negotiations ask one fewer question, and everyone repairs a ship free each round.'],
    sting: ['THE PEOPLE RISE', 'A failed UPRISING can be tried again on the same world. Yours too.'], perk: 'A world you win by negotiating holds +2 defenders.' },
  { name: 'IMPERIAL ORDER', col: '#C9D2DC', line: '#C9D2DC', tag: 'IMPERIAL', brief: 'patrols half price \u00b7 a tax on every world past your 2nd',
    good: ['LAW AND ORDER', 'Patrols cost half.'],
    sting: ['THE TAX ROLLS', 'Every round, 1 CR for each world you hold past your second.'], perk: 'Half of all the tax paid comes to you.' },
  { name: 'THE SPICE AGE', col: '#A77BFF', line: '#A77BFF', tag: 'SPICE', brief: 'income doubled \u00b7 2 goods a shop \u00b7 bribes cost half again',
    good: ['SPICE FLOWS', 'Everyone\u2019s income is doubled, and every shop puts out two goods.'],
    sting: ['WORLDS COST A FORTUNE', 'Bribing a world costs half again (\u00d71.5).'], perk: 'Your income is tripled instead.' },
  { name: 'THE DAWN GRIPS THE GALAXY', col: '#E0475B', line: '#E0475B', tag: 'DAWN', brief: 'black market open \u00b7 files for sale',
    good: ['THE MARKET NEVER SLEEPS', 'The black market is open every round.'],
    sting: ['EVERYONE HAS A FILE', 'Once a round, anyone can buy another player\u2019s file for 6 CR. They only hear that someone did.'], perk: 'NAME THE MARK each round: whoever takes a world from them gets 5 CR from the Dawn.' },
];
function eraTag(s, f, txt) { const e = eraOf(s); return e && e.f === f ? '<span class="etag" style="--ec:' + ERAS[f].col + '">' + (txt || ERAS[f].tag) + '</span>' : ''; }
function eraOf(s) { return s && s.v2 && s.v2.era ? s.v2.era : null; }
function eraRulesHtml(s, f, mine) {  // the three rules: everyone, the sting (everyone, the champion too), the champion's perk
  const E = ERAS[f], v = s.v2 || {}, champ = v.champ && v.champ[f] === mySlot;
  return '<div class="eraRule"><b>' + E.good[0] + ' \u00b7 EVERYONE</b>' + E.good[1] + '</div>' +
         '<div class="eraRule sting"><b>' + E.sting[0] + ' \u00b7 EVERYONE</b>' + E.sting[1] + '</div>' +
         '<div class="eraRule" style="border-color:#FFD166"><b style="color:#FFD166">THE CHAMPION</b>' + E.perk + '</div>' +
         (mine && champ ? '<div class="eraYou">\uD83D\uDC51 Your Era: the perk is yours, and the sting hits you too.</div>' : '');
}
// 3.1-dev.88 THE PERSONAL FORECAST (user: "the personal forecast is a great idea"): what declaring does to YOU, then to each player.
function eraForecast(s, f) {
  const v = s.v2 || {}, ps = s.players.map((p, i) => i).filter(i => i !== mySlot && s.players[i].active);
  const held = i => s.planets.filter(p => p.owner === i).length, tax = i => Math.max(0, held(i) - 2);
  const row = (good, txt, val) => '<div class="fcRow ' + (good ? 'up' : 'dn') + '"><i></i><span>' + txt + '</span><b>' + val + '</b></div>';
  let you = [], tab = [];
  if (f === 2) { const inc = v.income || 0; you = [row(1, 'Your income', inc + ' \u2192 ' + inc * 3 + ' CR'), row(0, 'Bribing a world', '\u00d71.5')]; tab = ps.map(i => row(1, feedWho(s, i), 'income \u00d72')); }
  if (f === 1) { const all = [mySlot].concat(ps).reduce((a, i) => a + tax(i), 0);
    you = [row(1, 'You collect', '+' + Math.floor(all / 2) + ' / round'), row(!tax(mySlot), 'You pay (' + held(mySlot) + ' worlds)', tax(mySlot) ? '\u2212' + tax(mySlot) + ' / round' : 'nothing'), row(1, 'Patrols', '6 \u2192 3 CR')];
    tab = ps.map(i => row(!tax(i), feedWho(s, i) + ' \u00b7 ' + held(i) + ' worlds', tax(i) ? '\u2212' + tax(i) + ' / round' : 'untaxed')); }
  if (f === 0) { you = [row(1, 'Negotiations', '1 question shorter'), row(1, 'Free repair', '1 ship / round'), row(1, 'Worlds you negotiate', '+2 defenders'), row(0, 'Your ' + held(mySlot) + ' worlds', 'open to uprisings')];
    tab = ps.map(i => row(0, feedWho(s, i) + ' \u00b7 ' + held(i) + ' worlds', 'open to uprisings')); }
  if (f === 3) { you = [row(1, 'You name', 'the Mark, each round'), row(1, 'The black market', 'open every round'), row(0, 'Your file', 'for sale, 6 CR')]; tab = ps.map(i => row(0, feedWho(s, i), 'file for sale')); }
  return '<div class="fc"><small class="up">IF YOU DECLARE \u00b7 FOR YOU</small>' + you.join('') + (tab.length ? '<small>FOR THE TABLE</small>' + tab.join('') : '') + '</div>';
}
let eraDecreeTail = '';
function openEraSheet() {
  const s = lastS, e = eraOf(s); if (!s) return;
  const dec = [0, 1, 2, 3].filter(g => ((s.v2 && s.v2.decrees) || 0) & (1 << g));  // 3.1-dev.103: your decree lives in the same sheet
  if (!e) { if (dec.length) openDecree(dec[0]); return; }
  if (dec.length) { const g = dec[0], G = ERAS[g];
    eraDecreeTail = '<div class="debtnote" style="border-color:' + G.col + ';margin-top:12px"><span><b style="color:' + G.col + '">\uD83D\uDC51 YOUR DECREE \u00b7 ' + G.name + '</b><br>You can declare it the moment this Era ends, round ' + (e.until + 1) + '.</span></div>'; }
  else eraDecreeTail = '';
  const E = ERAS[e.f], left = e.until - s.round + 1;
  sheetShow('era', '<div class="sheetTitle" style="color:' + E.col + '">' + E.name + '</div><div class="sheetSub">Declared by ' + feedWho(s, e.by) + ' \u00b7 ' + left + ' round' + (left === 1 ? '' : 's') + ' left</div>' +
    '<div style="margin-top:10px">' + eraRulesHtml(s, e.f, true) + '</div>' +
    rivetAsk('era' + e.f + '_' + e.from, 'An Era is a champion\u2019s rule for 2 rounds, and nobody can cancel it. The first rule helps everyone. The sting hits everyone, the champion too. The champion gets a perk on top. Every number it changes wears its chip.') + eraDecreeTail, E.col);
}
function openDecree(f) {
  const s = lastS; if (!s) return;
  const E = ERAS[f], e = eraOf(s), left = Math.min(2, s.roundLimit - s.round + 1);  // ERA_ROUNDS (3.1-dev.88)
  sheetShow('decree', '<div class="sheetSub" style="letter-spacing:3px">\uD83D\uDC51 YOUR DECREE</div><div class="sheetTitle" style="color:' + E.col + ';font-size:22px">' + E.name + '</div>' +
    '<div class="sheetSub">' + left + ' round' + (left === 1 ? '' : 's') + ' \u00b7 locked once declared: nobody can cancel it</div>' + pressBody(s, f, E) +
    (e ? '<div class="debtnote"><span>' + ERAS[e.f].name + ' holds until round ' + e.until + '. Declare yours when it ends.</span></div>'
       : '<button class="pill" style="border-color:' + E.col + ';color:' + E.col + '" onclick="declareEra(' + f + ')">DECLARE NOW</button>') +
    '<button class="pill" style="border-color:var(--line);color:var(--dim)" onclick="sheetHide()">NOT YET</button>', E.col);
}
function pressBody(s, f, E) { return '<div style="margin:10px 0">' + eraRulesHtml(s, f, false) + '</div>' + eraForecast(s, f); }
// 3.1-dev.88 YOUR FACTION PRESSES YOU (user: "i like the faction talking to you about it and pressing you to do it, but playing it for you i
// don't think we should do"): each Prepare you hold a decree, your faction calls -- more impatient every round. Never declares for you.
const PRESS = [['The cells are waiting for your word, Champion.', 'Hope doesn\u2019t keep, Champion. Give the word.', 'Every day you wait, the Empire forgets to fear us.'],
               ['The Empire is watching what you do with this.', 'The Moffs grow restless, Champion. Give the order.', 'Order delayed is order denied. Declare it.'],
               ['The spice is ready to flow. Say when.', 'Every round you wait costs us money, Champion.', 'The Syndicate doesn\u2019t pay for patience. Open the taps.'],
               ['The Dawn is ready when you are.', 'Our people are getting bored, Champion. Bored people talk.', 'Hesitation is a kind of weakness. The Dawn remembers weakness.']];
function pressAutoOpen(s) {
  const v = s.v2 || {};
  if (s.phase !== 0 || eraOf(s) || !v.decrees || sheetKey || (typeof ANN !== 'undefined' && ANN.open) || s.battleActive) return;
  let f = -1; for (let g = 0; g < 4; g++) if (v.decrees & (1 << g)) { f = g; break; }
  if (f < 0) return;
  let first = s.round, last = '';
  try { first = +(localStorage.getItem('sfDec' + f) || 0); if (!first || first > s.round) { first = s.round; localStorage.setItem('sfDec' + f, first); } last = localStorage.getItem('sfPress') || ''; } catch (e) {}
  const held = s.round - first, key = s.round + ':' + f;
  if (held < 1 || last === key) return;  // the round you win it, the title news already told you
  try { localStorage.setItem('sfPress', key); } catch (e) {}
  const E = ERAS[f];
  sheetShow('decree', '<div class="sheetSub" style="letter-spacing:3px">YOUR DECREE \u00b7 HELD ' + held + ' ROUND' + (held === 1 ? '' : 'S') + '</div>' +
    '<div class="pressQ">' + insigniaSvg(f, 30) + '<span><div class="sheetSub" style="letter-spacing:2px;color:' + E.col + '">THE ' + FACTION_SHORT_NAMES[f] + '</div><q>' + PRESS[f][Math.min(2, held - 1)] + '</q></span></div>' +
    '<div class="sheetTitle" style="color:' + E.col + ';font-size:20px">' + E.name + '</div><div class="sheetSub">2 rounds \u00b7 locked once declared</div>' + pressBody(s, f, E) +
    '<button class="pill" style="border-color:' + E.col + ';color:' + E.col + '" onclick="declareEra(' + f + ')">DECLARE NOW</button>' +
    '<button class="pill" style="border-color:var(--line);color:var(--dim)" onclick="sheetHide()">NOT YET</button>', E.col);
}
// 3.1-dev.98 THE BACK CHANNEL (canvas boards 20-22). k: 0 my worlds, 1 a world, 2 negotiate/bribe a world, 3 a player.
function bcText(s, k, a, consul) {
  const C = consul === mySlot ? 'your' : feedWho(s, consul) + '\u2019s', W = a >= 0 && s.planets[a] ? wName(s, a) : 'that world', N = a >= 0 && s.players[a] ? feedWho(s, a) : 'them';
  return k === 0 ? 'no attacks on ' + C + ' worlds' : k === 1 ? 'no attack on ' + W : k === 2 ? 'no negotiating for or bribing ' + W : 'no attacks on ' + N;
}
let bcPick = { t: -1, k: 0, a: -1, cr: 3 };
function openBcSheet() {
  const s = lastS; if (!s) return;
  const P = bcPick, others = s.players.map((p, i) => i).filter(i => i !== mySlot && s.players[i].active), CY = '#5FD4FF';
  const opt = (k, txt) => '<button class="bcOpt' + (P.k === k ? ' on' : '') + '" onclick="bcPick.k=' + k + ';bcPick.a=-1;openBcSheet()"><i></i>' + txt + '</button>';
  let pick = '';
  if (P.k === 1 || P.k === 2) pick = '<div class="pickgrid bcWorlds">' + s.planets.map((p, i) => worldBtn(s, i, 'bcPick.a=' + i + ';openBcSheet()', P.a === i)).join('') + '</div>';
  if (P.k === 3) pick = '<div class="pickgrid">' + others.filter(i => i !== P.t).map(i => '<button class="' + (P.a === i ? 'on' : '') + '" onclick="bcPick.a=' + i + ';openBcSheet()">' + pTag(s, i) + '</button>').join('') + '</div>';
  const ready = P.t >= 0 && (P.k === 0 || P.a >= 0);
  sheetShow('bc', '<div class="sheetSub" style="letter-spacing:3px">THE BACK CHANNEL \u00b7 ONCE A ROUND</div><div class="sheetTitle" style="color:' + CY + '">A PRIVATE OFFER</div>' +
    '<div class="bcLab">TO</div><div class="pickgrid">' + others.map(i => '<button class="' + (P.t === i ? 'on' : '') + '" onclick="bcPick.t=' + i + ';if(bcPick.a===' + i + ')bcPick.a=-1;openBcSheet()">' + pTag(s, i) + '</button>').join('') + '</div>' +
    '<div class="bcLab">IF THEY, THIS ROUND</div>' + opt(0, 'don\u2019t attack any of my worlds') + opt(1, 'don\u2019t attack a world' + (P.k === 1 && P.a >= 0 ? ': <b>' + wName(s, P.a) + '</b>' : '')) +
    opt(2, 'don\u2019t negotiate for or bribe a world' + (P.k === 2 && P.a >= 0 ? ': <b>' + wName(s, P.a) + '</b>' : '')) + opt(3, 'don\u2019t attack a player' + (P.k === 3 && P.a >= 0 ? ': <b>' + feedWho(s, P.a) + '</b>' : '')) + pick +
    '<div class="bcPay"><span class="bcLab">I PAY</span><button onclick="bcPick.cr=Math.max(1,bcPick.cr-1);openBcSheet()">\u2212</button><b>' + P.cr + '<small> CR</small></b><button onclick="bcPick.cr=Math.min(10,bcPick.cr+1);openBcSheet()">+</button></div>' +
    '<div class="sheetSub">Paid the moment they accept. Break it and they pay you back, and you get <b style="color:#FFD166">' + (P.cr * 2) + ' CR</b>. Nobody else sees the offer.</div>' +
    '<button class="pill" style="border-color:' + CY + ';color:' + CY + '"' + (ready ? '' : ' disabled') + ' onclick="bcSend()">SEND IT QUIETLY</button>' +
    '<button class="pill" style="border-color:var(--line);color:var(--dim)" onclick="sheetHide()">NOT NOW</button>', CY);
}
async function bcSend() {
  const P = bcPick; sheetHide();
  const r = await post('/bc_offer?t=' + P.t + '&k=' + P.k + (P.a >= 0 ? '&a=' + P.a : '') + '&cr=' + P.cr + '&id=' + encodeURIComponent(myId));
  if (r && !r.ok && r.why) notify('NOT NOW', r.why, '#FF6B5A', 4000); else notify('OFFER SENT', 'Quietly.', '#5FD4FF', 2500);
  bcPick = { t: -1, k: 0, a: -1, cr: 3 }; poll();
}
let bcSeen = '';
function bcAutoOpen(s) {  // the offer opens by itself once (when no other sheet is up)
  const o = s.v2 && s.v2.bcIn; if (!o || o[4] !== 1 || sheetKey || (typeof ANN !== 'undefined' && ANN.open) || s.battleActive) return;
  const key = s.round + ':' + o[0]; if (bcSeen === key) return; bcSeen = key; openBcIncoming();
}

// ---- 3.1-dev.103 THE COCKPIT (canvas boards 19-23): five decks -- DOCKS (market) / STAR MAP (galaxy) / BRIDGE (home) /
// COMMS (the Galaxy and You + this round's list) / WAR TABLE (standings + your favor) -- on a cockpit console fixed at the
// bottom: a curved dash in your faction's trim, angled keys, a raised hex BRIDGE key, console lights, hold-to-lock-in.
// Tap a key or swipe between neighbours; a holo-flicker covers the switch.
let commsMode = false, deckSig = '', prepDone = {}, lockHoldT = 0, docksSeenRound = -1;
const commsSeen = new Set();  // urgent COMMS rows you've already looked at
const DECKS = ['docks', 'map', 'bridge', 'comms', 'war'];
const DECK_ICON = {
  docks: ['............','.##########.','.#........#.','.##########.','.#.#....#.#.','.#..#..#..#.','.#...##...#.','.#..#..#..#.','.#.#....#.#.','.##########.','............','............'],
  bridge: ['............','.....##.....','....####....','...######...','..##.##.##..','.##..##..##.','##...##...##','#....##....#','.....##.....','....####....','...##..##...','............'],
  comms: ['............','..#......#..','.#..####..#.','#..#....#..#','#.#..##..#.#','#.#.#..#.#.#','#.#..##..#.#','#..#....#..#','.#..####..#.','..#......#..','.....##.....','....####....']
};
function deckOf() { return commsMode ? 'comms' : galaxyMode ? 'map' : (marketsMode || bmMode) ? 'docks' : (standingsMode || repMode) ? 'war' : 'bridge'; }
function deckGo(id) {
  if (!lastS) return;
  const was = deckOf();
  galaxyMode = marketsMode = bmMode = standingsMode = repMode = fleetsMode = commsMode = false;
  currentPlanet = -1; attackChoice = false;  // 3.1-dev.104 (user: "switching to markets while looking at a planet leaves the planet on top")
  if (id === 'map') { galaxyMode = true; try { localStorage.setItem('sfMap', String(lastS.round)); } catch (e) {} }  // 3.1-dev.122 the checklist
  else if (id === 'docks') { if (bmOpenNow(lastS)) bmMode = true; else marketsMode = true; }
  else if (id === 'war') standingsMode = true;
  else if (id === 'comms') commsMode = true;
  if (sheetKey) sheetHide();
  if (was !== id) {
    let fx = document.getElementById('holoFx');  // 3.1-dev.104 the hologram refresh: scanlines flicker across, then clear
    if (!fx) { fx = document.createElement('div'); fx.id = 'holoFx'; fx.setAttribute('aria-hidden', 'true'); document.body.appendChild(fx); }
    if (setOn('motion')) { fx.classList.remove('go'); void fx.offsetWidth; fx.classList.add('go'); }
    if (setOn('buzz') && navigator.vibrate) navigator.vibrate(8);
  }
  { const c = document.getElementById('canopy'); if (c) c.style.setProperty('--sx', (DECKS.indexOf(id) - 2) * -24 + 'px'); }  // 3.1-dev.110: the stars turn with you
  shownHtml = null; window.scrollTo(0, 0); render(lastS);
}
function deckPx(rows) { let o = ''; rows.forEach((r, y) => { for (let x = 0; x < r.length; x++) if (r[x] === '#') o += '<rect x="' + x + '" y="' + y + '" width="1" height="1"/>'; }); return o; }
function deckWarBars(s) {  // the War Table key IS the standings: a bar per player, in their color, as tall as their VP
  const ps = s.players.map((p, i) => ({ p: p, i: i })).filter(x => x.p.active).slice(0, 4);
  const top = Math.max(1, ...ps.map(x => x.p.vp || 0)), w = ps.length > 3 ? 2 : 3, gap = ps.length > 3 ? 1 : 1;
  let o = '';
  ps.forEach((x, k) => { const h = Math.max(1, Math.round((x.p.vp || 0) / top * 10)), col = x.p.chosenFaction >= 0 ? FACTION_UI_COLORS[x.p.chosenFaction] : '#8FA3B8';
    o += '<rect x="' + (1 + k * (w + gap)) + '" y="' + (11 - h) + '" width="' + w + '" height="' + h + '" fill="' + col + '"/>'; });
  return o + '<rect x="0" y="11" width="12" height="1"/>';
}
function deckShows(s) {
  if (!s || s.stage !== 1 || mySlot < 0 || spectating || feedMode || cardMode || primerAt >= 0) return false;
  if (document.getElementById('bCanvas') || document.querySelector('#app .neghead')) return false;
  return true;
}
function deckBarUpdate(s) {
  let el = document.getElementById('deck');
  if (!el) { el = document.createElement('div'); el.id = 'deck'; document.body.appendChild(el);
    const c = document.createElement('div'); c.id = 'canopy'; c.setAttribute('aria-hidden', 'true'); c.innerHTML = '<i class="arc"></i><i class="sl"></i><i class="sr"></i><i class="glint"></i>'; document.body.appendChild(c); }
  const on = deckShows(s);
  document.body.classList.toggle('deckOn', on);
  if (!on) return;
  const me = s.players[mySlot], d = deckOf(), f = me.chosenFaction >= 0 ? me.chosenFaction : 0;
  const v = s.v2 || {}, rows = gyRows(s), urgent = rows.filter(r => r.pri === 0).length;
  const active = s.players.filter(p => p.active).length, locked = s.players.filter(p => p.active && p.lockedIn).length;
  const res = s.phase === 2, mov = s.phase === 1;
  // 3.1-dev.106 (user: "the pulsing lights ... never go away"): a light = something you haven't looked at yet
  if (d === 'docks') docksSeenRound = s.round;
  const lampDocks = bmOpenNow(s) && docksSeenRound !== s.round;
  const urgentKeys = rows.filter(r => r.pri === 0).map(r => (r.html.match(/<b[^>]*>([^<]*)<\/b>/) || ['', ''])[1] + ':' + s.round);
  if (d === 'comms') urgentKeys.forEach(k => commsSeen.add(k));
  const commsNew = urgentKeys.some(k => !commsSeen.has(k));
  const myTurn = !me.lockedIn && active - locked === 1;
  const hit = s.battleActive && (s.battleDefenderSlot === mySlot || (s.planets[s.battleTargetPlanet] || {}).owner === mySlot);
  const lockTxt = me.lockedIn ? 'LOCKED IN<small>waiting for ' + (active - locked) + '</small>' : (res ? 'DONE THIS ROUND' : mov ? 'DONE MOVING' : 'LOCK IN') + '<small>hold \u00b7 ' + locked + ' of ' + active + ' ready</small>';
  const sig = [d, f, urgent, commsNew, lampDocks, myTurn, hit, deckWarBars(s)].join('|');
  if (sig === deckSig && el.firstChild) return;
  deckSig = sig;
  const leds = '<span class="leds"><i class="led l1' + (myTurn ? ' blink' : '') + '" title="your move"></i><i class="led l2' + (hit ? ' alarm' : '') + '" title="your world is under attack"></i></span>';
  const key = (id, name, sub, icon, lamp) => '<button class="dk dk-' + id + (d === id ? ' on' : '') + '" onclick="deckGo(\'' + id + '\')" aria-label="' + name + ', ' + sub + '"' + (d === id ? ' aria-current="page"' : '') + '>' +
    icon + '<b>' + name + '</b><small>' + sub + '</small>' + (lamp ? '<i class="lamp' + (/steady/.test(lamp) ? ' steady' : '') + '" style="--lc:' + lamp.split(' ')[0] + '"></i>' : '') + (id === 'bridge' ? leds : '') + '</button>';
  const svgI = rows => '<svg viewBox="0 0 12 12" width="24" height="24" shape-rendering="crispEdges" fill="currentColor" aria-hidden="true">' + rows + '</svg>';
  el.className = 'fac' + f;
  el.innerHTML =
    '<div class="dash"><svg class="dashBg" viewBox="0 0 390 132" preserveAspectRatio="none" aria-hidden="true"><path class="plate" d="M0 56 L48 40 Q195 -4 342 40 L390 56 L390 132 L0 132 Z"/><path class="trim" d="M58 44 Q195 6 332 44"/>' +
      '<circle cx="30" cy="62" r="2"/><circle cx="360" cy="62" r="2"/><circle cx="30" cy="120" r="2"/><circle cx="360" cy="120" r="2"/>' +
      '</svg>' +
'' +
      key('docks', 'DOCKS', 'MARKET', svgI(deckPx(DECK_ICON.docks)), lampDocks ? '#FFB347' : '') +
      key('map', 'STAR MAP', 'GALAXY', '<canvas id="gxSpin" width="56" height="56" aria-hidden="true"></canvas>', '') +
      key('bridge', 'BRIDGE', 'HOME', svgI(deckPx(DECK_ICON.bridge)), '') +
      key('comms', 'COMMS', 'FOR YOU', svgI(deckPx(DECK_ICON.comms)), commsNew ? '#FF6B5A' : urgent ? '#FF6B5A steady' : '') +
      key('war', 'WAR TABLE', 'STANDINGS', svgI(deckWarBars(s)), '') + '</div>';
  spinPlanet = Math.max(0, s.planets.findIndex(pl => pl.owner === mySlot));
  if (!spinRAF) spinRAF = requestAnimationFrame(galaxySpinLoop);
}
// LOCK IN as a launch control: hold for half a second, a ring fills, it clicks home (a quick tap just says "hold").
function lockHold(e) { const b = e.currentTarget; if (b.disabled) return; b.classList.add('holding'); clearTimeout(lockHoldT);
  lockHoldT = setTimeout(() => { b.classList.remove('holding'); b.classList.add('fired'); if (navigator.vibrate) navigator.vibrate([12, 40, 30]); lockIn(); }, 900); }  // 3.1-dev.107: 480 -> 900 ms (user: "a little too fast")
function lockLet() { clearTimeout(lockHoldT); document.querySelectorAll('.dkLock.holding').forEach(b => b.classList.remove('holding')); }
function lockTap() { const b = document.querySelector('.dkLock'); if (b && !b.classList.contains('fired') && !b.disabled) notify('HOLD TO ' + (lastS && lastS.phase === 2 ? 'FINISH' : 'LOCK IN'), 'Press and hold the button for a moment.', '#5FD4FF', 1600); }
// Swipe between neighbouring decks (never from inside the map window, a sheet or a sideways scroller).
let dkSX = 0, dkSY = 0, dkST = 0, dkOK = false;
document.addEventListener('touchstart', e => { const t = e.touches[0]; dkOK = document.body.classList.contains('deckOn') && !sheetKey &&
  !e.target.closest('#lyrSheet, #lyrAnn, .gfan, .mktiles, .wsheet, input, textarea') && !(e.target.closest('#gwin') && (gView.s > 1.02 || e.touches.length > 1));
  dkSX = t.clientX; dkSY = t.clientY; dkST = Date.now(); }, { passive: true });  // 3.1-dev.104: the map swipes too, unless zoomed in
document.addEventListener('touchend', e => { if (!dkOK) return; dkOK = false; const t = e.changedTouches[0], dx = t.clientX - dkSX, dy = t.clientY - dkSY;
  if (Date.now() - dkST > 600 || Math.abs(dx) < 70 || Math.abs(dx) < Math.abs(dy) * 1.6) return;
  const i = DECKS.indexOf(deckOf()), j = i + (dx < 0 ? 1 : -1); if (j >= 0 && j < DECKS.length) deckGo(DECKS[j]); }, { passive: true });
// ---- LIVE NOW: a battle or a negotiation happening anywhere, one tap to watch (or whisper) however far in it is
function liveNowHtml(s) {
  if (s.battleActive && !watchingBattle(s) && s.battleAttackerSlot !== mySlot && s.battleDefenderSlot !== mySlot) {
    const w = s.battleKind === 2 ? pTag(s, s.battleDefenderSlot) : (s.planets[s.battleTargetPlanet] || {}).name || '';
    return '<button class="liveNow" onclick="watchBattle=battleKeyOf(lastS);render(lastS)"><i></i><b>LIVE NOW</b><span>' + pTag(s, s.battleAttackerSlot) + ' attacking ' + w + '</span><em>WATCH</em></button>';
  }
  const tk = (s.talks || []).find(t => t.by !== mySlot);
  if (tk) return '<button class="liveNow talk" onclick="openWatch(' + tk.by + ')"><i></i><b>LIVE NOW</b><span>' + pTag(s, tk.by) + ' negotiating ' + wName(s, tk.p) + '</span><em>WATCH</em></button>';
  return '';
}
// ---- COMMS: the Galaxy and You on its own deck, with this round's list on top
function commsHtml(s) {
  const me = s.players[mySlot], rows = gyRows(s);
  let h = liveNowHtml(s) + '<div class="commsHead"><b>COMMS</b><span>&gt; private channel \u00b7 only you see this_</span></div>';
  if (s.phase === 0) {
    const key = s.round + ':';
    const steps = [];
    if (rows.some(r => r.pri === 0)) steps.push(['ANSWER', 'something below needs you', '#FF6B5A', null]);
    if (defWeak(s)) steps.push(['DEFEND', 'one of your worlds is weak', '#5FD4FF', 'defSheet()']);
    if (me.credits >= 2) steps.push(['HANGAR', 'repair or build ships', '#FFC933', "deckGo('bridge')"]);
    if (bmOpenNow(s) || (s.v2 && s.mkt !== 0)) steps.push(['SHOPS', bmOpenNow(s) ? 'the black market is open' : 'one buy at each shop', '#E8EEF5', "deckGo('docks')"]);
    steps.push(['LOCK IN', 'on the Bridge, when you\u2019re ready', '#8FA3B8', "deckGo('bridge')"]);
    const done = steps.filter(x => prepDone[key + x[0]]).length;
    h += '<div class="prepList"><div class="plHead"><b>THIS PREPARE</b><span>' + done + ' of ' + steps.length + ' done</span></div>' +
      steps.map((x, k) => { const d = prepDone[key + x[0]] || (x[0] === 'LOCK IN' && me.lockedIn);
        return '<button class="plRow' + (d ? ' done' : '') + '" style="--pc:' + x[2] + '"' + (x[3] ? ' onclick="prepDone[\'' + key + x[0] + '\']=1;' + x[3] + '"' : '') + '><i>' + (d ? '\u2713' : k + 1) + '</i><span><b>' + x[0] + '</b> ' + x[1] + '</span><em>' + '' + '</em></button>'; }).join('') + '</div>';
  }
  const grp = (name, col, list) => list.length ? '<div class="cgHead" style="--gc:' + col + '"><i></i><b>' + name + '</b><span></span></div><div class="gdlist">' + list.map(r => r.html).join('') + '</div>' : '';
  h += grp('NEEDS YOU', '#FF6B5A', rows.filter(r => r.pri === 0)) + grp('DEALS AND PROMISES', '#5FD4FF', rows.filter(r => r.pri === 1)) + grp('THE GALAXY', '#8FA3B8', rows.filter(r => r.pri === 2));
  if (s.v2 && s.v2.event >= 0) h += '<div class="note" style="color:#FF8C42;margin:10px 0 0;padding:6px 10px;font-size:13px"><b>' + s.v2.eventName + '</b> \u00b7 ' + s.v2.eventText + '</div>';
  if (!rows.length) h += '<p class="ghint" style="text-align:center;margin-top:24px">All quiet on your channel.</p>';
  return h;
}
// ---- 3.1-dev.102 GUNS FOR HIRE (the Warlord), BEATEN TO IT (fight anyway / redirect / stand by) ----
let gfhSeen = '', beatenSeen = '', gfhPick = { to: -1, ships: [], cr: 4 };
function gfhAutoOpen(s) {
  const o = s.v2 && s.v2.gfhIn; if (!o || sheetKey || (typeof ANN !== 'undefined' && ANN.open) || s.battleActive) return;
  const key = s.round + ':' + o[0]; if (gfhSeen === key) return; gfhSeen = key; openGfhIncoming();
}
function beatenAutoOpen(s) {
  const p = s.v2 ? s.v2.beaten : undefined; if (p === undefined || sheetKey || (typeof ANN !== 'undefined' && ANN.open) || s.battleActive) return;
  const key = s.round + ':' + p; if (beatenSeen === key) return; beatenSeen = key; openBeaten(p);
}
function myReadyFleets(s) { const me = s.players[mySlot]; return me ? me.fleets.map((f, k) => ({ k: k, n: f.alive, ok: f.alive > 0 && !f.spent })).filter(f => f.ok) : []; }
function openBeaten(p) {
  const s = lastS, w = s && s.planets[p]; if (!w) return;
  const fl = myReadyFleets(s), nb = s.planets.map((q, i) => i).filter(i => (w.nb >> i) & 1 && s.planets[i].owner !== mySlot);
  const opt = (n, name, line, tag, col, go) => '<button class="beatOpt" style="--oc:' + col + '" onclick="' + go + '"><i>' + n + '</i><span><b>' + name + '</b><small>' + line + '</small></span><em>' + tag + '</em></button>';
  sheetShow('beaten', '<div class="sheetSub" style="letter-spacing:3px">' + (fl.length ? 'FLEET ' + (fl[0].k + 1) + ' \u00b7 STILL READY' : 'YOUR FLEETS') + '</div>' +
    '<div class="sheetTitle">' + feedWho(s, w.owner) + ' got to ' + w.name + ' first</div><div class="sheetSub">Your turn isn\u2019t wasted: pick one.</div>' +
    opt(1, 'FIGHT ANYWAY', w.ink ? 'The ink is still wet: hit it now, against its old defenders.' : 'They dug in: 2 more defenders this round.', w.ink ? 'INK WET' : 'DUG IN', '#FF6B5A', 'sheetHide();openPlanet(' + p + ')') +
    opt(2, 'REDIRECT', 'Move one step on the board and attack a world next door.', nb.length + ' IN REACH', '#5FD4FF', 'openRedirect(' + p + ')') +
    opt(3, 'STAND BY', 'Hold position. That fleet\u2019s first attack next round gets +2 ships.', '+2 NEXT', '#FFD166', 'openStandBy()') +
    '<button class="pill" style="border-color:var(--line);color:var(--dim)" onclick="sheetHide()">LATER</button>', '#FF6B5A');
}
function openRedirect(p) {
  const s = lastS, w = s && s.planets[p]; if (!w) return;
  const nb = s.planets.map((q, i) => i).filter(i => (w.nb >> i) & 1 && s.planets[i].owner !== mySlot);
  sheetShow('redirect', '<div class="sheetSub" style="letter-spacing:3px;color:#5FD4FF">REDIRECT</div><div class="sheetTitle">One jump from ' + w.name + '</div>' +
    '<div class="sheetSub">Move that fleet one step on the board, then attack.</div><div class="gdlist">' +
    (nb.length ? nb.map(i => { const o = worldOdds(s, i); return '<button class="gyrow" onclick="sheetHide();openPlanet(' + i + ')"><span><b>' + s.planets[i].name + '</b> ' + (s.planets[i].owner >= 0 ? feedWho(s, s.planets[i].owner) + '\u2019s' : 'unclaimed') + '</span><em style="color:' + (o && !o.none ? o.v.col : 'var(--dim)') + '">' + (o && !o.none ? o.v.word + ' ' + o.lo + '\u2013' + o.hi + '%' : '') + '</em></button>'; }).join('') : '<div class="note">Nothing in reach that isn\u2019t yours.</div>') +
    '</div><button class="pill" style="border-color:var(--line);color:var(--dim)" onclick="openBeaten(' + p + ')">BACK</button>', '#5FD4FF');
}
function openStandBy() {
  const s = lastS, fl = s ? myReadyFleets(s) : []; if (!fl.length) return;
  sheetShow('standby', '<div class="sheetSub" style="letter-spacing:3px;color:#FFD166">STAND BY</div><div class="sheetTitle">Which fleet holds?</div>' +
    '<div class="sheetSub">It sits out the rest of this round. Its first attack next round gets +2 ships.</div><div class="pickgrid">' +
    fl.map(f => '<button onclick="standBy(' + f.k + ')">FLEET ' + (f.k + 1) + '<small>' + f.n + ' ships</small></button>').join('') + '</div>' +
    '<button class="pill" style="border-color:var(--line);color:var(--dim)" onclick="sheetHide()">BACK</button>', '#FFD166');
}
async function standBy(f) { sheetHide(); const r = await post('/standby?f=' + f + '&id=' + encodeURIComponent(myId)); if (r && r.why) notify('STAND BY', r.why, '#FF6B5A', 3000); poll(); }
function openGfhSheet() {
  const s = lastS, me = s && s.players[mySlot]; if (!me) return;
  const P = gfhPick, AC = '#B48CFF';
  const ships = me.fleets.map((f, k) => '<div class="gfhFleet"><span>FLEET ' + (k + 1) + (f.spent ? ' \u00b7 spent' : '') + '</span><div>' +
    (f.ships || []).map((alive, j) => { const b = k * 8 + j, on = P.ships.includes(b);
      return alive ? '<button class="gfhShip' + (on ? ' on' : '') + '" aria-label="Fleet ' + (k + 1) + ' ship ' + (j + 1) + '" onclick="gfhToggle(' + b + ')"></button>' : ''; }).join('') + '</div></div>').join('');
  sheetShow('gfh', '<div class="sheetTitle" style="color:' + AC + '">GUNS FOR HIRE</div><div class="sheetSub">Lend ships to a commander\u2019s next battle, for a price.</div>' +
    '<div class="sheetSub" style="letter-spacing:2px">TO WHOM?</div><div class="pickgrid">' +
    s.players.map((p, i) => i).filter(i => i !== mySlot && s.players[i].active).map(i => '<button class="' + (P.to === i ? 'on' : '') + '" onclick="gfhPick.to=' + i + ';openGfhSheet()">' + pTag(s, i) + '</button>').join('') + '</div>' +
    '<div class="sheetSub" style="letter-spacing:2px">YOUR HANGAR \u00b7 TAP UP TO 3 \u00b7 ' + P.ships.length + ' / 3</div>' + ships +
    '<div class="gfhPrice"><span>YOUR PRICE</span><button aria-label="Lower" onclick="gfhPick.cr=Math.max(1,gfhPick.cr-1);openGfhSheet()">\u2212</button><b>' + P.cr + ' CR</b><button aria-label="Raise" onclick="gfhPick.cr=Math.min(15,gfhPick.cr+1);openGfhSheet()">+</button></div>' +
    '<div class="sheetSub">They fly in front in that player\u2019s next battle and take the first hits. After, each rolls to come home: 3\u20136 makes it. Lent ships sit out until then.</div>' +
    '<button class="pill" style="border-color:' + AC + ';color:' + AC + '"' + (P.to >= 0 && P.ships.length ? '' : ' disabled') + ' onclick="gfhSend()">SEND OFFER \u00b7 ' + P.ships.length + ' SHIP' + (P.ships.length === 1 ? '' : 'S') + ' FOR ' + P.cr + ' CR</button>' +
    '<button class="pill" style="border-color:var(--line);color:var(--dim)" onclick="sheetHide()">NOT NOW</button>', AC);
}
function gfhToggle(b) { const a = gfhPick.ships, i = a.indexOf(b); if (i >= 0) a.splice(i, 1); else if (a.length < 3) a.push(b); openGfhSheet(); }
async function gfhSend() {
  const P = gfhPick, m = P.ships.reduce((x, b) => x + Math.pow(2, b), 0);
  sheetHide(); const r = await post('/gfh_offer?t=' + P.to + '&m=' + m + '&cr=' + P.cr + '&id=' + encodeURIComponent(myId));
  if (r && r.why) notify('GUNS FOR HIRE', r.why, '#FF6B5A', 3000); gfhPick = { to: -1, ships: [], cr: 4 }; poll();
}
function openGfhIncoming() {
  const s = lastS, o = s && s.v2 && s.v2.gfhIn; if (!o) return;
  const w = o[0], n = o[1], cr = o[2], p = s.players[w], AC = '#B48CFF';
  sheetShow('gfhin', (ART && p ? '<img class="pixel bcFace" src="' + portraitURL(p.commander, 3) + '" alt="">' : '') +
    '<div class="sheetSub" style="letter-spacing:3px;color:' + AC + '">GUNS FOR HIRE \u00b7 ' + feedWho(s, w) + '</div>' +
    '<div class="sheetTitle">' + n + ' ship' + (n === 1 ? '' : 's') + ' for your next battle</div><div class="sheetTitle" style="color:#FFD166">' + cr + ' CR</div>' +
    '<div class="sheetSub">They fly in front and take the first hits. They can\u2019t be turned on ' + feedWho(s, w) + '. Your forecast counts them.</div>' +
    '<div class="biAct"><button class="ghost" onclick="gfhAnswer(0)">NO THANKS</button><button style="border-color:#FFD166;color:#FFD166"' + (s.players[mySlot].credits >= cr ? '' : ' disabled') + ' onclick="gfhAnswer(1)">HIRE \u00b7 ' + cr + ' CR</button></div>', AC);
}
async function gfhAnswer(yes) { sheetHide(); await post('/gfh_answer?yes=' + yes + '&id=' + encodeURIComponent(myId)); poll(); }
// 3.1-dev.102: a Galaxy and You row with nothing to act on explains itself instead of doing nothing
let GY_INFO = [];
function gyInfo(k) { const x = GY_INFO[k]; if (!x) return;
  sheetShow('gyinfo', '<div class="sheetTitle" style="color:' + x.col + '">' + x.head + '</div><div class="annLine" style="margin:10px 0">' + x.line + '</div>' +
    '<button class="pill" style="border-color:var(--line);color:var(--dim)" onclick="sheetHide()">GOT IT</button>', x.col); }
function openBcIncoming() {
  const s = lastS, o = s && s.v2 && s.v2.bcIn; if (!o) return;
  const c = o[0], cr = o[3], p = s.players[c], CY = '#5FD4FF', nm = feedWho(s, c);
  const line = o[1] === 0 ? 'Leave my worlds alone this round' : o[1] === 1 ? 'Stay away from ' + wName(s, o[2]) + ' this round' : o[1] === 2 ? 'Don\u2019t negotiate for or bribe ' + wName(s, o[2]) + ' this round' : 'Leave ' + feedWho(s, o[2]) + ' alone this round';
  sheetShow('bcin', '<div class="sheetSub" style="letter-spacing:3px">FOR YOUR EYES ONLY</div>' +
    (ART && p ? '<img class="pixel bcFace" src="' + portraitURL(p.commander, 3) + '" alt="">' : '') +
    '<div class="sheetSub" style="letter-spacing:2px;color:' + CY + ';font-weight:700">' + nm + ' \u00b7 THE CONSUL</div>' +
    '<div class="bcQuote">\u201CLet\u2019s keep this between us. ' + line + ', and <b style="color:#FFD166">' + cr + ' Credits</b> are yours.\u201D</div>' +
    '<div class="bcTerms"><span><b style="color:#8CE7A0">+' + cr + ' CR</b> now if you accept</span><span><b style="color:#FF6B5A">\u2212' + cr + ' CR</b> if you break it this round</span><span>Only the two of you know.</span></div>' +
    '<div class="biAct"><button class="ghost" onclick="bcAnswer(0)">DECLINE</button><button style="border-color:#8CE7A0;color:#8CE7A0" onclick="bcAnswer(1)">ACCEPT \u00b7 +' + cr + '</button></div>', CY);
}
async function bcAnswer(yes) { sheetHide(); await post('/bc_answer?yes=' + yes + '&id=' + encodeURIComponent(myId)); poll(); }
// Would this break a deal you accepted? how: 'fight' | 'talk' (negotiate, bribe, uprising) | 'pvp'. Mirrors bcCheckLocked.
function bcWarn(s, p, how, def) {
  const o = s && s.v2 && s.v2.bcIn; if (!o || o[4] !== 2) return '';
  const c = o[0], k = o[1], a = o[2], owner = p >= 0 && s.planets[p] ? s.planets[p].owner : -1, uprise = how === 'talk' && owner >= 0 && owner !== mySlot;
  const hostile = how === 'fight' || uprise;
  const hit = (k === 0 && hostile && owner === c) || (k === 1 && hostile && p === a) || (k === 2 && how === 'talk' && p === a) ||
              (k === 3 && ((hostile && owner === a) || (how === 'pvp' && def === a)));
  return hit ? 'You promised ' + feedWho(s, c) + '. Go ahead and you pay back ' + o[3] + ' CR, and the whole table hears a back-channel deal was broken.' : '';
}
function bcConfirm(warn, go) {
  sheetShow('bcwarn', '<div class="sheetTitle" style="color:#FF6B5A">THIS BREAKS YOUR DEAL</div><div class="annLine" style="margin:10px 0">' + warn + '</div>' +
    '<button class="pill" style="border-color:#FF6B5A;color:#FF6B5A" onclick="sheetHide();' + go + '">GO AHEAD ANYWAY</button>' +
    '<button class="pill" style="border-color:var(--line);color:var(--dim)" onclick="sheetHide()">KEEP MY WORD</button>', '#FF6B5A');
}
function rivalBtns(s, call) { return s.players.map((p, i) => i).filter(i => i !== mySlot && s.players[i].active).map(i => '<button onclick="' + call + '(' + i + ')">' + pTag(s, i) + '</button>').join(''); }
function openMarkSheet() {
  const s = lastS; if (!s) return;
  sheetShow('mark', '<div class="sheetSub" style="letter-spacing:3px">THE DAWN GRIPS THE GALAXY \u00b7 YOUR ERA</div><div class="sheetTitle" style="color:#E0475B">NAME THE MARK</div>' +
    '<div class="sheetSub">Whoever takes a world from them gets 5 CR from the Dawn. The table hears it. Change it any round.</div><div class="pickgrid">' + rivalBtns(s, 'nameMark') + '</div>' +
    '<button class="pill" style="border-color:var(--line);color:var(--dim)" onclick="sheetHide()">NOT NOW</button>', '#E0475B');
}
async function nameMark(i) { sheetHide(); await post('/name_mark?t=' + i + '&id=' + encodeURIComponent(myId)); poll(); }
function openFileBuy() {
  const s = lastS; if (!s) return;
  sheetShow('file', '<div class="sheetSub" style="letter-spacing:3px">THE DAWN GRIPS THE GALAXY</div><div class="sheetTitle" style="color:#E0475B">BUY A FILE \u00b7 6 CR</div>' +
    '<div class="sheetSub">Their Credits, debts, grudges, job and patrols, this round. They\u2019ll hear that someone bought it, never who. One a round.</div><div class="pickgrid">' + rivalBtns(s, 'buyFile') + '</div>' +
    '<button class="pill" style="border-color:var(--line);color:var(--dim)" onclick="sheetHide()">NOT NOW</button>', '#E0475B');
}
async function buyFile(i) {
  sheetHide();
  const r = await post('/buy_file?t=' + i + '&id=' + encodeURIComponent(myId));
  if (r && !r.ok && r.why) notify('NOT NOW', r.why, '#FF6B5A', 4000);
  await poll(); if (r && r.ok) openKit(i);
}
async function declareEra(f) { sheetHide(); await post('/decree?id=' + encodeURIComponent(myId) + '&f=' + f); poll(); }
function lieChoices(s) {
  const pick = a => a[Math.floor((s.round * 7 + a.length) % a.length)];
  const worlds = s.planets.map(p => wName(s, s.planets.indexOf(p))), core = s.planets.map((p, i) => i).filter(i => s.planets[i].ring === 2).map(i => wName(s, i));
  return ['Someone is broke. Every last Credit.', pick(worlds) + '\u2019s defenders are down to 2.', 'Someone has been casing ' + (core.length ? pick(core) : 'Coruscant') + '.',
          'Someone just became ' + FNAMES[(s.round + 1) % 4] + ' ' + favorWord((s.round + 1) % 4, 4) + '.'];
}
function openLieSheet() {
  const s = lastS; if (!s) return;
  sheetShow('lie', '<div class="sheetSub" style="letter-spacing:3px">THE SHADOW AGE \u00b7 YOUR ERA</div><div class="sheetTitle" style="color:#E0475B">WRITE THIS ROUND\u2019S LIE</div>' +
    '<div class="sheetSub">The Holonet airs it next to the true rumor. Only you will know which one is the lie.</div>' +
    '<div class="liePick">' + lieChoices(s).map((t, k) => '<button onclick="sendLie(' + k + ')">' + t + '</button>').join('') + '</div>' +
    '<textarea id="lieText" maxlength="60" rows="2" placeholder="...or type your own (60)"></textarea>' +
    '<button class="pill" style="border-color:#E0475B;color:#E0475B" onclick="sendLie(-1)">SEND IT</button>', '#E0475B');
}
async function sendLie(k) {
  const t = k >= 0 ? lieChoices(lastS)[k] : ((document.getElementById('lieText') || {}).value || '').trim();
  if (!t) return;
  sheetHide(); await post('/lie?id=' + encodeURIComponent(myId) + '&text=' + encodeURIComponent(t.slice(0, 60))); poll();
}
function eraBannerHtml(s) {
  const e = eraOf(s); if (!e) return '';
  const E = ERAS[e.f], left = e.until - s.round + 1;
  return '<button class="eraBan" style="--ec:' + E.col + '" onclick="openEraSheet()"><div><b>' + E.name + '</b><small>' + E.brief + '</small></div><span>' + left + ' round' + (left === 1 ? '' : 's') + ' left</span></button>';
}
function eraFrame(s) {  // the Era's weather: a thin colored frame on every screen
  const e = eraOf(s), el = document.getElementById('eraFrame'); if (!el) return;
  if (e && s.stage === 1) { el.style.setProperty('--ec', ERAS[e.f].col); el.classList.add('on'); } else el.classList.remove('on');
}
// ---- 3.1-dev.13 THE ONCE-A-GAME MOVES ----
const MOVE_K = { from: 3, warCap: 12, warBonus: 2, summitWorlds: 3, keepers: 6, coverRounds: 3, smoothCr: 5, boldCr: 10, failCr: 3 };
function myClass(s) { const me = s.players[mySlot]; return me && me.commander >= 0 ? me.commander % 4 : -1; }
function moveReady(s) { return s.v2 && !s.v2.moveUsed && s.round >= MOVE_K.from && myClass(s) >= 0; }
function worldBtn(s, i, call, on) { return '<button class="' + (on ? 'on' : '') + '" onclick="' + call + '">' + wName(s, i) + (s.planets[i].owner >= 0 ? '<small>' + (s.planets[i].owner === mySlot ? 'yours' : feedWho(s, s.planets[i].owner)) + '</small>' : '') + '</button>'; }
let summitPick = [];
function openMoveSheet() {
  const s = lastS; if (!s) return;
  const cls = myClass(s), C = CLASSES[cls];
  const MOVE_TIP = ['Pick a world you want badly. The table hears it now, so its holder can bring ships in Movement. In Resolution every ready fleet can join the attack, plus 3 ships of your own.',
                    'Put the Summit where a fight would hurt you: worlds you\u2019re about to talk to, or ones someone is eyeing. Anyone who attacks there meets ' + MOVE_K.keepers + ' peacekeepers and three angry factions.',
                    'Pick the faction whose shop or escorts you need. For 3 rounds you\u2019re Family to them. Just don\u2019t attack their worlds while you\u2019re inside.',
                    'Smooth is safe money nobody can pin on you. Bold pays double plus a stolen ship, but a 1 or 2 on the die costs you a step with them.'];
  const head = rivetAsk('move' + cls, MOVE_TIP[cls] || '') + '<div class="sheetSub" style="letter-spacing:3px">ONCE A GAME</div><div class="sheetTitle">' + C.move + '</div><div class="sheetSub" style="margin-bottom:8px">' + C.moveText + '</div>';
  if (cls === 0) {  // WARLORD
    const last = s.roundLimit - 2;  // a war never runs into the Final Hour (gameStateDeclareWar)
    if (s.round > last) { sheetShow('move', head + '<div class="debtnote" style="border-color:#FF6B5A"><span>Too late for a warpath: the last one could be declared in round ' + last + '. The galaxy\u2019s next stop is the Final Hour.</span></div>', '#FF6B5A'); return; }
    const ws = s.planets.map((p, i) => i).filter(i => s.planets[i].owner !== mySlot);
    sheetShow('move', head + '<div class="sheetSub">Pick the world.' + (s.round === last ? ' This is the last round you can.' : '') + '</div><div class="pickgrid">' + ws.map(i => worldBtn(s, i, 'confirmWar(' + i + ')')).join('') + '</div>', '#FF6B5A');
    return;
  }
  if (cls === 1) {  // CONSUL
    sheetShow('move', head + '<div class="sheetSub">Up to ' + MOVE_K.summitWorlds + ' worlds, ' + 2 + ' rounds: nobody fights there. During it your negotiations ask one fewer question and bribes cost you 2 less. Held both rounds: every faction warms to you. Broken: peacekeepers defend and the breaker pays for it.</div>' +
      '<div class="pickgrid">' + s.planets.map((p, i) => worldBtn(s, i, 'toggleSummit(' + i + ')', summitPick.includes(i))).join('') + '</div>' +
      '<button class="pill" style="border-color:#5FD4FF;color:#5FD4FF"' + (summitPick.length ? '' : ' disabled') + ' onclick="callSummit()">CALL THE SUMMIT \u00b7 ' + summitPick.length + ' / ' + MOVE_K.summitWorlds + '</button>', '#5FD4FF');
    return;
  }
  const mine = s.players[mySlot].chosenFaction, others = [0, 1, 2, 3].filter(f => f !== mine);
  if (cls === 2) {  // INFILTRATOR
    sheetShow('move', head + '<div class="sheetSub">For ' + MOVE_K.coverRounds + ' rounds you live as Family of another faction: their shop, escorts, negotiations and bribes you. Plant one story on the Holonet. Attack one of their worlds and your cover is blown (a tier down). No gifts, no titles: it\u2019s all fake.</div>' +
      '<div class="pickgrid">' + others.map(f => '<button onclick="goCover(' + f + ')">' + insigniaSvg(f, 18) + ' ' + FACTION_SHORT_NAMES[f] + '<small>become ' + favorWord(f, 4) + '</small></button>').join('') + '</div>', '#C8364A');
    return;
  }
  sheetShow('move', head + '<div class="sheetSub">Whose vault?</div><div class="pickgrid">' + others.map(f => '<button onclick="heistApproach(' + f + ')">' + insigniaSvg(f, 18) + ' ' + FACTION_SHORT_NAMES[f] + '</button>').join('') + '</div>', '#FFD166');
}
function openPushSheet() {
  const s = lastS, f = s && s.v2 ? s.v2.heistPush : -1; if (f < 0) return;
  sheetShow('push', '<div class="sheetSub" style="letter-spacing:3px">THE HEIST \u00b7 THE ' + FACTION_SHORT_NAMES[f] + ' VAULT</div><div class="sheetTitle" style="color:#FFD166">PUSH YOUR LUCK?</div>' +
    '<div class="eraRule"><b>ROLL AGAIN</b>3\u20136: +' + MOVE_K.boldCr + ' more CR. 1\u20132: you lose the ' + MOVE_K.boldCr + ' CR you took and they know your face (a tier down).</div>' +
    '<button class="pill" style="border-color:#FF6B5A;color:#FF6B5A" onclick="doMove(\'/heist_push?push=1\')">ROLL AGAIN</button>' +
    '<button class="pill" style="border-color:#8CE7A0;color:#8CE7A0" onclick="doMove(\'/heist_push?push=0\')">TAKE THE MONEY</button>', '#FFD166');
}
// 3.1-dev.57 DECLARE WAR CLARITY (playtest 4: "declare war: confusing"): the three steps as a timeline with real round numbers,
// the current one lit -- on the move sheet, the confirm, and a sheet for both the Warlord and the world's holder.
function warTimeline(s, W, at, r) {  // 3.1-dev.88 WARPATH: one round -- at = the lit step (-1 none), r = the round
  const st = [['DECLARE', 'ROUND ' + r + ' \u00b7 ANY TIME', 'The table tells everyone.'],
              ['THEY REACT', 'UNTIL RESOLUTION', 'Whoever holds ' + W + ' can bring ships to it, if there\u2019s time.'],
              ['STRIKE', 'ROUND ' + r + ' \u00b7 RESOLUTION', 'Every ready fleet can join your attack on ' + W + ', with 3 more ships. Win and it\u2019s yours.']];
  return '<div class="wtl">' + st.map((x, k) => '<div class="' + (k === at ? 'on' : k < at ? 'done' : '') + '"><i>' + (k < at ? '\u2713' : k + 1) + '</i><span><b>' + x[0] + '</b><em>' + x[1] + '</em>' + x[2] + '</span></div>').join('') + '</div>';
}
function warStepNow(s, w) { return s.phase === 0 ? 0 : s.phase === 1 ? 1 : 2; }
function openWarInfo() {
  const s = lastS, w = s && s.v2 && s.v2.war; if (!w) return;
  const W = wName(s, w.w), mineW = s.planets[w.w] && s.planets[w.w].owner === mySlot, mineWar = w.by === mySlot;
  sheetShow('war', '<div class="sheetSub" style="letter-spacing:3px">' + (mineWar ? 'YOUR WARPATH' : mineW ? 'WARPATH TO YOUR WORLD' : 'A WARPATH') + '</div><div class="sheetTitle" style="color:#FF6B5A">' + W.toUpperCase() + '</div>' +
    '<div class="sheetSub">' + (mineWar ? 'You are on the warpath to ' + W + '.' : pTag(s, w.by) + ' is on the warpath to ' + W + '.') + '</div>' +
    warTimeline(s, W, warStepNow(s, w), w.r) +
    (mineW ? '<div class="debtnote" style="border-color:#FF6B5A">' + pixIcon('target', '#FF6B5A', 2) + '<span>Every ready fleet they have, plus 3 ships, this Resolution. Bring ships to ' + W + ' in Movement.</span></div>' +
             '<button class="pill" style="border-color:#FF6B5A;color:#FF6B5A" onclick="sheetHide();openPlanet(' + w.w + ')">OPEN ' + W.toUpperCase() + '</button>' : '') +
    (mineWar && s.v2.summit && s.v2.summit.w.includes(w.w) ? '<div class="debtnote" style="border-color:#5FD4FF"><span>A Summit protects ' + W + '. Strike anyway and the ceasefire breaks: peacekeepers defend it and three factions turn on you. Or pull back: your Warpath comes back to you.</span></div>' +
      '<button class="pill" style="border-color:#5FD4FF;color:#5FD4FF" onclick="doMove(\'/war_back\')">PULL BACK</button>' : '') +
    '<button class="pill" style="border-color:var(--line);color:var(--dim)" onclick="sheetHide()">CLOSE</button>', '#FF6B5A');
}
function confirmWar(i) {
  const s = lastS, W = wName(s, i);
  sheetShow('move2', '<div class="sheetSub" style="letter-spacing:3px">WARPATH</div><div class="sheetTitle" style="color:#FF6B5A">' + W.toUpperCase() + '</div>' +
    warTimeline(s, W, 0, s.round) +
    (s.v2.summit && s.v2.summit.w.includes(i) ? '<div class="debtnote" style="border-color:#FF6B5A">' + W + ' is under a Summit. Marching anyway breaks the ceasefire: peacekeepers defend it.</div>' : '') +
    '<button class="pill" style="border-color:#FF6B5A;color:#FF6B5A" onclick="doMove(\'/war?planet=' + i + '\')">ON THE WARPATH</button><button class="pill" style="border-color:var(--line);color:var(--dim)" onclick="sheetHide()">NOT NOW</button>', '#FF6B5A');
}
function toggleSummit(i) { const k = summitPick.indexOf(i); if (k >= 0) summitPick.splice(k, 1); else if (summitPick.length < MOVE_K.summitWorlds) summitPick.push(i); openMoveSheet(); }
function callSummit() { const w = summitPick.slice(); summitPick = []; doMove('/summit?w=' + w.join(',')); }
function goCover(f) { doMove('/cover?f=' + f); }
function heistApproach(f) {
  sheetShow('move2', '<div class="sheetSub" style="letter-spacing:3px">THE HEIST \u00b7 THE ' + FACTION_SHORT_NAMES[f] + ' VAULT</div><div class="sheetTitle" style="color:#FFD166">HOW DO YOU GO IN?</div>' +
    '<div class="eraRule"><b>SMOOTH</b>No roll. +' + MOVE_K.smoothCr + ' CR and a free good from their shop. Nobody knows it was you.</div>' +
    '<div class="eraRule sting"><b>BOLD</b>Roll a die. 3\u20136: +' + MOVE_K.boldCr + ' CR, a good and a stolen ship. 1\u20132: you get out with ' + MOVE_K.failCr + ' CR and they know your face (a tier down).</div>' +
    '<button class="pill" style="border-color:#FFD166;color:#FFD166" onclick="doMove(\'/heist?f=' + f + '&bold=0\')">SMOOTH</button><button class="pill" style="border-color:#FF6B5A;color:#FF6B5A" onclick="doMove(\'/heist?f=' + f + '&bold=1\')">BOLD</button>', '#FFD166');
}
async function doMove(q) { sheetHide(); await post(q + (q.includes('?') ? '&' : '?') + 'id=' + encodeURIComponent(myId)); poll(); }
// The War Room: pull ships from your worlds (each keeps 1), up to the cap.
let warPull = null;
function openWarRoom() {
  const s = lastS, w = s.v2 && s.v2.war; if (!w || w.by !== mySlot) return;
  const mine = s.planets.map((p, i) => i).filter(i => s.planets[i].owner === mySlot && i !== w.w && (s.planets[i].defense || 0) > 1);
  if (!warPull) { warPull = {}; let left = MOVE_K.warCap; mine.forEach(i => { const n = Math.min(left, s.planets[i].defense - 1); warPull[i] = n; left -= n; }); }
  const total = Object.values(warPull).reduce((a, b) => a + b, 0), W = wName(s, w.w), sum = s.v2.summit && s.v2.summit.w.includes(w.w);
  sheetShow('war', '<div class="sheetSub" style="letter-spacing:3px">THE WAR ROOM</div><div class="sheetTitle" style="color:#FF6B5A">' + W.toUpperCase() + '</div>' +
    '<div class="sheetSub">Send ships from your worlds\u2019 defenders (each keeps at least 1). Those worlds stay weak this round; survivors come home after.</div>' +
    rivetAsk('warroom' + w.w + '_' + w.r, 'Every world keeps at least 1 defender. Each ship you send leaves its world weaker this round, and everyone can see it. The war adds ' + MOVE_K.warBonus + ' ships of its own. Win and ' + W + ' is yours; survivors come home either way.') +
    '<div class="warHead"><span>WORLD</span><span>KEEPS</span><span>SENDS</span></div>' +
    mine.map(i => { const n = warPull[i] || 0, k = s.planets[i].defense - n;
      return '<div class="warRow"><span>' + wName(s, i) + '<small>' + s.planets[i].defense + ' defenders</small></span><b class="keep' + (k <= 1 ? ' thin' : '') + '">' + k + '</b><button onclick="warStep(' + i + ',-1)">\u2212</button><b>' + n + '</b><button onclick="warStep(' + i + ',1)">+</button></div>'; }).join('') +
    (mine.length ? '' : '<div class="debtnote"><span>None of your worlds has a defender to spare (each keeps 1). Fortify them in Prepare.</span></div>') +
    '<div class="warTot">' + pnumSvg(total, '#E8EEF5', 3) + '<span>sent (max ' + MOVE_K.warCap + ') +' + MOVE_K.warBonus + ' for the war = <b style="color:#FF6B5A">' + (total + MOVE_K.warBonus) + ' ships</b> against ' + W + '\u2019s ' + (s.planets[w.w].defense || 0) + ' defenders' + (sum ? ' and ' + MOVE_K.keepers + ' peacekeepers' : '') + '</span></div>' +
    (sum ? '<div class="debtnote" style="border-color:#FF6B5A">A Summit protects ' + W + '. March anyway: the ceasefire breaks, peacekeepers defend it, and three factions turn on you. Or pull back and keep your declaration.</div>' +
           '<button class="pill" style="border-color:#FF6B5A;color:#FF6B5A"' + (total ? '' : ' disabled') + ' onclick="goWar(1)">MARCH ANYWAY</button><button class="pill" style="border-color:var(--line);color:var(--dim)" onclick="doMove(\'/war_back\')">PULL BACK</button>'
         : '<button class="pill" style="border-color:#FF6B5A;color:#FF6B5A"' + (total ? '' : ' disabled') + ' onclick="goWar(0)">GO TO WAR</button>'), '#FF6B5A');
}
function warStep(i, d) {
  const s = lastS, max = (s.planets[i].defense || 1) - 1, total = Object.values(warPull).reduce((a, b) => a + b, 0);
  const n = Math.max(0, Math.min(max, (warPull[i] || 0) + d));
  if (d > 0 && total >= MOVE_K.warCap) return;
  warPull[i] = n; openWarRoom();
}
function goWar(march) { const q = Object.keys(warPull).filter(i => warPull[i] > 0).map(i => i + ':' + warPull[i]).join(','); warPull = null; doMove('/war_room?pull=' + q + '&march=' + march); }
function openStorySheet() {
  sheetShow('story', '<div class="sheetSub" style="letter-spacing:3px">DEEP COVER</div><div class="sheetTitle" style="color:#C8364A">PLANT A STORY</div>' +
    '<div class="sheetSub">Up to 60 characters. It airs on the Holonet this Movement, exactly like a true rumor. Once.</div>' +
    '<textarea id="lieText" maxlength="60" rows="2" placeholder="Someone is broke. Every last Credit."></textarea>' +
    '<button class="pill" style="border-color:#C8364A;color:#C8364A" onclick="sendStory()">PLANT IT</button>', '#C8364A');
}
async function sendStory() { const t = ((document.getElementById('lieText') || {}).value || '').trim(); if (!t) return; sheetHide(); await post('/story?id=' + encodeURIComponent(myId) + '&text=' + encodeURIComponent(t.slice(0, 60))); poll(); }
function movesRows(s) {
  const v = s.v2 || {}, rows = [], cls = myClass(s);
  if (moveReady(s) && !(v.war && v.war.by === mySlot))
    rows.push('<button class="gyrow move" onclick="openMoveSheet()">' + pixIcon(['war', 'summit', 'cover', 'heist'][cls], '#FFD166', 2) + '<span><b>ONCE A GAME \u00b7 ' + CLASSES[cls].move + '</b> ' + CLASSES[cls].moveText.split('. ')[0] + '.</span><em></em></button>');
  if (v.war) {
    const W = wName(s, v.war.w), strike = v.war.by === mySlot && s.phase === 2;
    if (v.war.by === mySlot) rows.push('<button class="gyrow strike" onclick="' + (strike ? 'openPlanet(' + v.war.w + ')' : 'openWarInfo()') + '">' + pixIcon('war', '#FF6B5A', 2) + '<span><b>' + (strike ? 'STRIKE NOW' : 'YOUR WARPATH') + '</b> ' + W + (strike ? ' \u00b7 every fleet, +3 ships' : ' \u00b7 strike this Resolution') + '</span><em></em></button>');
    else if (s.planets[v.war.w] && s.planets[v.war.w].owner === mySlot) rows.push('<button class="gyrow strike" onclick="openWarInfo()">' + pixIcon('war', '#FF6B5A', 2) + '<span><b>WARPATH TO ' + W.toUpperCase() + '</b> ' + feedWho(s, v.war.by) + ' strikes this Resolution. Reinforce it.</span><em></em></button>');
  }
  if (v.heistPush >= 0 && v.heistPush !== undefined)  // 3.1-dev.20
    rows.push('<button class="gyrow move" onclick="openPushSheet()">' + pixIcon('heist', '#FFD166', 2) + '<span><b>PUSH YOUR LUCK</b> one more roll at the ' + FNAMES[v.heistPush] + ' vault, this round only</span><em></em></button>');
  if (v.summit) rows.push('<button class="gyrow race">' + pixIcon('summit', '#5FD4FF', 2) + '<span><b>CEASEFIRE</b> ' + v.summit.w.filter(i => i >= 0).map(i => wName(s, i)).join(', ') + '</span><em>R' + v.summit.u + '</em></button>');
  if (v.cover) {
    const cf = v.cover.f, era = eraOf(s);
    rows.push('<button class="gyrow cover">' + pixIcon('cover', '#C8364A', 2) + '<span><b>DEEP COVER</b> ' + favorWord(cf, 4) + ' of the ' + FNAMES[cf] + ' until round ' + v.cover.u +
              '<small class="cvList">\u2713 their shop, escorts, negotiations and bribes are yours' + 
              '<br><i>\u26A0 attacking a ' + FNAMES[cf] + ' world blows your cover</i></small></span></button>');
    if (!v.cover.story && s.phase !== 2) rows.push('<button class="gyrow cover" onclick="openStorySheet()">' + pixIcon('holonet', '#C8364A', 2) + '<span><b>PLANT A STORY</b> one, on the Holonet</span><em></em></button>');
  }
  return rows;
}
// Fighting into a ceasefire or against your cover: warn first.
function fightWarning(s) {
  const p = s.takeoverPlanet, v = s.v2 || {}, W = wName(s, p);
  const bc = bcWarn(s, p, 'fight'); if (bc) return bc;  // 3.1-dev.98
  if (v.summit && v.summit.w.includes(p)) return 'A Summit protects ' + W + '. Attack anyway: the ceasefire breaks, ' + MOVE_K.keepers + ' peacekeeper ships join its defense, and three factions turn on you.';
  if (v.cover) {
    const pl = s.planets[p], known = pl && pl.chars && pl.chars[0] && pl.chars[0].revealed ? AFFILIATION_TO_FACTION.indexOf(pl.chars[0].value) : -1;
    if (known === v.cover.f) return W + ' leans ' + FNAMES[v.cover.f] + '. Attack it and your cover is blown: a tier down with them, and the table hears it.';
    if (known < 0) return 'If ' + W + ' leans ' + FNAMES[v.cover.f] + ', attacking it blows your cover (a tier down, and the table hears it).';
  }
  return '';
}
// 3.1-dev.15 THE END OF THE GAME: your story and how the game was won, told from the event log the phone kept.
function storyHtml(s) {
  const pub = FEED.pub, mine = FEED.mine, me = s.players[mySlot];
  if (!me) return '';
  const takes = pub.filter(e => e.kind === 2 && e.who === mySlot), how = [0, 0, 0, 0];
  takes.forEach(e => { how[e.num] = (how[e.num] || 0) + 1; });
  const lines = [];
  if (me.commander >= 0) lines.push('You played <b>' + charName(me.commander) + '</b>, ' + CLASSES[me.commander % 4].name.toLowerCase() + ' of the ' + FNAMES[me.chosenFaction] + '.');
  if (takes.length) lines.push('You took <b>' + takes.length + ' world' + (takes.length === 1 ? '' : 's') + '</b>: ' + [[1, 'by force'], [2, 'by talking'], [3, 'with Credits']].filter(x => how[x[0]]).map(x => how[x[0]] + ' ' + x[1]).join(', ') + '.');
  const lost = mine.filter(e => e.kind === 5).length;
  if (lost) lines.push('You lost ' + lost + ' world' + (lost === 1 ? '' : 's') + ' along the way.');
  const q = pub.find(e => e.kind === 9 && e.who === mySlot);
  if (q) lines.push('You finished your quest in round ' + q.round + '.');
  pub.filter(e => e.kind === 11 && e.num === 14 && e.other === mySlot).forEach(e => lines.push('You became <b>Champion of the ' + FNAMES[e.who] + '</b> in round ' + e.round + '.'));
  pub.filter(e => e.kind === 11 && e.num === 12 && e.other === mySlot).forEach(e => lines.push('You declared <b>' + ERAS[e.who].name + '</b>.'));
  const war = pub.find(e => e.kind === 11 && (e.num === 16 || e.num === 17) && e.other === mySlot);
  if (war) lines.push('Your war on ' + wName(s, war.planet) + (war.num === 16 ? ' took it.' : ' failed.'));
  if (pub.some(e => e.kind === 11 && e.num === 20 && e.other === mySlot)) lines.push('Your Summit held.');
  if (pub.some(e => e.kind === 11 && e.num === 21 && e.other === mySlot)) lines.push('Your cover was blown.');
  const heist = mine.find(e => e.kind === 12 && e.num === 23);
  if (heist) lines.push(Math.floor(heist.other / 16) === 2 ? 'Your heist on the ' + FNAMES[heist.who] + ' went wrong.' : 'You robbed the ' + FNAMES[heist.who] + ' vault.');
  const hits = mine.filter(e => e.kind === 12 && e.num === 5);
  if (hits.length) lines.push('The ' + [...new Set(hits.map(e => FNAMES[e.who]))].join(' and ') + ' came for you ' + (hits.length === 1 ? 'once' : hits.length + ' times') + '.');
  let h = '<div class="card story"><div class="storyK">YOUR STORY</div>' + lines.map(l => '<p>' + l + '</p>').join('') + '</div>';
  // how the game was won
  const w = s.winner;
  if (w >= 0 && w !== mySlot && s.players[w]) {
    const wt = pub.filter(e => e.kind === 2 && e.who === w), wq = pub.find(e => e.kind === 9 && e.who === w), wc = pub.filter(e => e.kind === 11 && e.num === 14 && e.other === w);
    const bits = [];
    if (wt.length) bits.push('took ' + wt.length + ' world' + (wt.length === 1 ? '' : 's'));
    if (wq) bits.push('finished their quest in round ' + wq.round);
    if (wc.length) bits.push('was Champion of the ' + wc.map(e => FNAMES[e.who]).join(' and the '));
    if (bits.length) h += '<div class="card story"><div class="storyK">HOW THE GAME WAS WON</div><p>' + feedWho(s, w) + ' ' + bits.join(', ') + '.</p></div>';
  }
  const eras = pub.filter(e => e.kind === 11 && e.num === 12);
  if (eras.length) h += '<div class="card story"><div class="storyK">ERAS</div><p>' + eras.map(e => ERAS[e.who].name.replace(/^THE /, 'The ').toLowerCase().replace(/(^|\s)\S/g, c => c.toUpperCase()) + ' (from round ' + e.round + ')').join(' \u00b7 ') + '</p></div>';
  return h;
}
// 3.1-dev.17 a few lines of story per world (original text), shown on its page.
const WORLD_LORE = {
  'TATOOINE': 'Twin suns bleach everything here except debts. Moisture farmers, bounty hunters and the Hutts all claim the same sand, and none of them share.',
  'HOTH': 'Ice to the horizon and storms that freeze a speeder mid-turn. Whoever holds it holds a hiding place nobody else wants to search.',
  'BESPIN': 'Mining cities float in the clouds of a gas giant. Tibanna gas pays well, and the cloud barons answer to whoever keeps the platforms aloft.',
  'DAGOBAH': 'Swamp, mist and silence. Ships sink into its bogs, and the few pilots who come back swear the planet was watching them.',
  'CORUSCANT': 'One city wrapped around a whole world, its towers older than most empires. Hold it and the galaxy believes you rule.',
  'KESSEL': 'Spice mines dug into a world that eats its workers. Everyone wants the spice; nobody wants to dig.',
  'OBA DIAH': 'The Pyke Syndicate\u2019s home: rain, ledgers and long memories. A debt made here follows you across the galaxy.',
  'DATHOMIR': 'Red forests and an older kind of power. Crimson Dawn keeps its secrets here, and the secrets keep their own guards.',
  'CORELLIA': 'Shipyards, smugglers and pilots who brag louder than their engines. The best ships in the galaxy leave its docks, for a price.',
  'NABOO': 'Green hills, still lakes and a palace with a long political memory. Its people prefer diplomacy, right up until they don\u2019t.',
  'ENDOR': 'A forest moon so thick the trees swallow whole wrecks. The locals are small, patient, and have never once surrendered.',
  'KIJIMI': 'A frozen monastery town turned thieves\u2019 den. Information is the only currency here that never freezes.',
  'YAVIN 4': 'Jungle, ancient temples and the Rebellion\u2019s oldest hiding place. Every hangar here remembers a launch that changed the war.',
  'MUSTAFAR': 'Rivers of lava and an Imperial fortress that drinks the heat. Nobody comes to Mustafar by choice.',
  'NAL HUTTA': 'The Hutts\u2019 bog-choked jewel, where every deal has three sides and a hidden fourth. Its palaces are older than its laws.',
  'CANTO BIGHT': 'Casinos, racetracks and money that never asks where it came from. The house always wins, and the house belongs to someone.',
  'KUAT': 'The orbital shipyards that build the Empire\u2019s Star Destroyers. A world measured in hull plates and contracts.',
};
// 3.1-dev.18: BACK (the phone's button, an edge swipe) steps back INSIDE the game instead of leaving the page.
function appBack() {
  if (ANN.open) { annTap(null, true); return; }
  if (sheetKey) { sheetHide(); return; }
  if (cardMode) { closeCard(); return; }
  if (feedMode) { closeFeed(); return; }
  if (currentPlanet >= 0) { leavePlanet(); return; }
  if (galaxyMode) { closeGalaxy(); return; }
  if (bmMode) { closeBlackMarket(); return; }
  if (marketsMode) { closeMarkets(); return; }
  if (repMode) { closeReputation(); return; }
  if (standingsMode) { closeStandings(); return; }
  if (fleetsMode) { closeFleets(); return; }
}
try {
  history.pushState({ sf: 1 }, '');
  window.addEventListener('popstate', () => { history.pushState({ sf: 1 }, ''); appBack(); });
} catch (e) {}
function questHtml(s) {
  const me = s.players[mySlot]; if (!me || me.commander < 0 || !s.v2) return '';
  const g = Math.max(1, s.v2.qGoal || 1), n = Math.min(g, s.v2.qProg || 0), done = s.v2.qDone;
  return '<div class="kit quest"><b>YOUR QUEST \u00b7 +5 VP' + (done ? ' \u00b7 DONE' : '') + '</b>' + CLASSES[me.commander % 4].quest +
         '<div class="qbar"><i style="width:' + Math.round(100 * n / g) + '%"></i></div><small>' + n + ' / ' + g + '</small></div>';
}
// 3.1-dev.75 THE KIT (user: "the whole kit should be a button ... it's just really busy"; "in standings you should be able to
// click on anyone's character icon and see their kit and faction"): only what's PERMANENT -- the class's strengths, weakness and
// once-a-game move, and the faction's ship quirk. Per-game things (gifts, world traits, the Era) stay where they live.
const KITS = [  // [name, line] -- the class's strengths, its weakness
  { good: [['FIRST STRIKE', 'In a battle you start, your fleet fights with one extra ship.'], ['THE MUSCLE', 'Take a world by force and its rival likes you more for it.'], ['WAR SPOILS', 'Every world you take by force, from anyone, your own faction cheers: +3 favor.'], ['GUNS FOR HIRE', 'Once a round, lend up to 3 ships to another commander\u2019s next battle, for a price you set. They take the first hits; each rolls 3\u20136 to come home.']],
    bad: [['BLUNT', 'Negotiations ask you one more question.']] },
  { good: [['SILVER TONGUE', 'Negotiations ask you one fewer question.'], ['OLD FRIENDS', 'You know every world\u2019s Sympathy from the start.'], ['TREATY WORLDS', 'A world you win by negotiating gets +2 defenders. Through next round it can\u2019t be bribed or bought out, and pays +1 CR.'], ['ANY DOOR', 'Your contacts help on every question, factions that hate you still talk, you can talk a home world over (5 hard questions, once), and stir an uprising on any held world.'], ['THE BACK CHANNEL', 'Once a round, offer a player Credits to stay off your worlds, a world, or another player. Break it and they pay back what they got, and you get double.']], bad: [['NO STOMACH FOR WAR', 'Taking a world by force costs extra favor with its faction.']] },
  { good: [['PULL A FILE', 'Every other round, open one rival\u2019s file: their Credits, debts, grudges, job and patrols. It stays open until your next pull.'], ['LONG EARS', 'Every world you hold tells you whose side the worlds two jumps away are on (everyone else hears one jump).']], bad: [['SPREAD THIN', 'Worlds you hold defend with one less.']] },
  { good: [['SELL IT TWICE', 'Please a faction and its rival pays you for the same work. Get greedy and they compare notes.'], ['THE STASH', 'Your income never drops below 6.']],
    bad: [['PRICE ON YOUR HEAD', 'Whoever takes one of your worlds gets +3 CR. Everyone knows it.']] } ];
const SHIP_QUIRK = [['SLIPPERY', 'One ship escapes a lost battle.'], ['IMPERIAL RANKS', 'Your fleets hold seven ships, not six.'],
                    ['ARMORED CARGO', 'The first hit in each battle bounces off.'], ['AMBUSHERS', 'You win a tied first roll.']];
function kitIcon(kind, col) {  // where a line comes from: your class (a star) or your faction's ships (its symbol)
  return '<span class="kitSrc" style="--kc:' + col + '">' + (kind < 0 ? '<svg width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="' + col + '" stroke-width="2.4" aria-hidden="true"><path d="M12 3l2.6 5.6 6 .7-4.5 4.1 1.2 6L12 16.5 6.7 19.4l1.2-6L3.4 9.3l6-.7z"/></svg>' : insigniaSvg(kind, 14)) + '</span>';
}
function kitRow(src, name, line, from, nameCol) {
  return '<div class="kitRow">' + src + '<span><b style="color:' + (nameCol || 'var(--text)') + '">' + name + '</b><small>' + line + '</small></span><em>' + from + '</em></div>';
}
function kitBodyHtml(s, slot) {
  const p = s.players[slot]; if (!p || p.commander < 0) return '';
  const id = p.commander, f = Math.floor(id / 4), cls = id % 4, K = KITS[cls], C = CLASSES[cls], CC = '#FFB347', mine = slot === mySlot;
  let h = '<div class="kitHead" style="--fc:' + FACTION_UI_COLORS[f] + '">' + (ART ? '<img class="pixel" src="' + portraitURL(id, 3) + '" alt="">' : '') +
          '<div><b>' + charName(id).toUpperCase() + '</b><span><i style="color:' + FACTION_UI_COLORS[f] + '">' + FACTION_SHORT_NAMES[f] + '</i> \u00b7 <i style="color:' + CC + '">' + C.name + '</i></span></div></div>';
  h += '<div class="kitGrp" style="color:#8CE7A0">WORKING FOR ' + (mine ? 'YOU' : 'THEM') + '</div>' +
       K.good.map(r => kitRow(kitIcon(-1, CC), r[0], r[1], C.name)).join('') + kitRow(kitIcon(f, FACTION_UI_COLORS[f]), SHIP_QUIRK[f][0], SHIP_QUIRK[f][1], FACTION_SHORT_NAMES[f] + ' SHIPS');
  h += '<div class="kitGrp" style="color:#FF6B5A">WORKING AGAINST ' + (mine ? 'YOU' : 'THEM') + '</div>' + K.bad.map(r => kitRow(kitIcon(-1, CC), r[0], r[1], C.name)).join('');
  h += '<div class="kitGrp" style="color:#FFD166">ONCE A GAME</div>' + kitRow(kitIcon(-1, CC), C.move, C.moveText, mine ? (s.v2 && s.v2.moveUsed ? 'USED' : s.round >= MOVE_K.from ? 'READY' : 'ROUND ' + MOVE_K.from) : C.name, '#FFD166');
  if (mine) {  // 3.1-dev.99: what your worlds' traits and your Family factions give you, with where each comes from
    const fromWorlds = s.planets.map((p, i) => ({ p: p, i: i })).filter(x => x.p.owner === mySlot && x.p.traitFx);
    const fam = [0, 1, 2, 3].filter(f => tierIdx(s, f) === 4);
    if (fromWorlds.length || fam.length) {
      h += '<div class="kitGrp" style="color:#8CE7A0">FROM YOUR WORLDS AND FRIENDS</div>' +
           fromWorlds.map(x => kitRow(kitIcon(-1, '#8CE7A0'), ((x.p.chars && x.p.chars[2] && x.p.chars[2].value) || 'A TRAIT').toUpperCase(), x.p.traitFx, x.p.name.toUpperCase())).join('') +
           fam.map(f => kitRow(kitIcon(f, FACTION_UI_COLORS[f]), GIFT5[f][0].toUpperCase(), GIFT5[f][1], FACTION_SHORT_NAMES[f] + ' FAMILY')).join('');
    }
    h += questHtml(s);
  }
  else h += fileHtml(s, slot) + '<div class="kitNote">Their gifts and favor stay secret.</div>';
  return h;
}
function openKit(slot) {
  const s = lastS; if (!s || !s.players[slot] || s.players[slot].commander < 0) return;
  const f = s.players[slot].chosenFaction;
  sheetShow('kit' + slot, kitBodyHtml(s, slot) + '<div class="biAct"><button class="ghost" onclick="sheetHide()">CLOSE</button></div>', f >= 0 ? FACTION_UI_COLORS[f] : '#5FD4FF');
}
// 3.1-dev.76 DEFEND YOUR WORLDS (canvas boards 1 + 2, user: "the planet box is a great idea"; "the defense screen would need to be
// sleekly integrated"): the PLANETS box on Home opens one sheet -- every world you hold, its defenders as blocks up to its limit,
// - / + (2 CR each), a PATROL chip (6 CR, 2 rounds), one total, one CONFIRM. Prepare only; read-only otherwise.
const DEF_K = { cr: 2, patrolCr: 6, patrolShips: 2, patrolRounds: 2 };
function patCr() { return (lastS && lastS.v2 && lastS.v2.patrolCr) || 6; }  // 3.1-dev.88 Imperial Order: half
let defAdd = {}, defPat = {};
function defWeak(s) { return s.planets.some(p => p.owner === mySlot && (p.defense || 0) <= 1); }
function defBoxHtml(s) {
  const n = s.planets.filter(pl => pl.owner === mySlot).length, warn = n && defWeak(s);
  return '<button class="stat defbox' + (n ? ' on' : '') + '" onclick="openDefense()"' + (n ? '' : ' disabled') + '>' + (warn ? '<i class="defdot"></i>' : '') + '<span>PLANETS</span><b>' + pnum(n, '#E8EEF5', 3) + '</b>' +
         (n ? '<small class="defgo"><svg width="11" height="11" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.6" aria-hidden="true"><path d="M12 2l8 3v6c0 5-3.5 9-8 11-4.5-2-8-6-8-11V5z"/></svg>DEFEND</small>' : '') + '</button>';
}
function openDefense() { defAdd = {}; defPat = {}; defSheet(); }
function defStep(i, d) {
  const p = lastS.planets[i], room = (p.dl || 8) - (p.defense || 0);
  defAdd[i] = Math.max(0, Math.min(room, (defAdd[i] || 0) + d)); defSheet();
}
function defTogglePat(i) { defPat[i] = !defPat[i]; defSheet(); }
function defCost() { let c = 0; for (const k in defAdd) c += defAdd[k] * DEF_K.cr; for (const k in defPat) if (defPat[k]) c += patCr(); return c; }
function defSheet() {
  const s = lastS; if (!s) return;
  const me = s.players[mySlot], prep = s.phase === 0, cost = defCost(), RINGS = ['OUTER', 'MID', 'CORE'];
  const mine = s.planets.map((p, i) => [p, i]).filter(x => x[0].owner === mySlot);
  // 3.1-dev.114 (playtest: "you are spending blind in there"): your Credits, live, at the top of the sheet
  let h = '<div class="dfHead"><div><div class="sheetTitle">DEFEND YOUR WORLDS</div><div class="sheetSub">' + (prep ? DEF_K.cr + ' CR a defender \u00b7 they stay until they fall' : 'You add defenders in Prepare.') + '</div></div>' +
          '<div class="dfCr"><small>CREDITS</small><b>' + (me.credits - cost) + '</b><small>' + (cost ? me.credits + ' \u2212 ' + cost : 'to spend') + '</small></div></div>';
  for (const [p, i] of mine) {
    const lim = p.dl || 8, have = p.defense || 0, add = defAdd[i] || 0, pat = !!defPat[i], weak = have <= 1;
    const slots = Array.from({ length: lim }, (x, k) => '<i class="' + (k < have ? 'h' : k < have + add ? 'n' : '') + '"></i>').join('');
    const tag = p.pt > 0 ? '<em style="color:#8CE7A0">PATROLLED \u00b7 UNTIL BEATEN</em>' : weak ? '<em style="color:#FFB347">WEAK</em>' : '';
    h += '<div class="dfRow' + (weak ? ' weak' : '') + '"><div class="dfTop"><b>' + p.name + '</b><small>' + (RINGS[p.ring] || '') + '</small>' + tag + '</div>' +
         '<div class="dfMid">' + (prep ? '<button aria-label="One fewer" onclick="defStep(' + i + ',-1)"' + (add ? '' : ' disabled') + '>\u2212</button>' : '') +
         '<div class="dfSlots">' + slots + '</div>' + (prep ? '<button class="plus" aria-label="One more" onclick="defStep(' + i + ',1)"' + (have + add < lim && me.credits >= cost + DEF_K.cr ? '' : ' disabled') + '>+</button>' : '') + '</div>' +
         '<div class="dfBot"><span>' + have + (add ? ' + ' + add + ' new' : '') + ' \u00b7 limit ' + lim + '</span>' +
         (prep && !(p.pt > 0) ? '<button class="dfPat' + (pat ? ' on' : '') + '" onclick="defTogglePat(' + i + ')"' + (pat || me.credits >= cost + patCr() ? '' : ' disabled') + '>' + (pat ? 'PATROL ON \u00b7 ' + patCr() + ' CR' : '+ PATROL ' + patCr() + ' CR') + eraTag(lastS, 1, '\u00bd IMPERIAL') + '</button>' : '') + '</div></div>';
  }
  if (!mine.length) h += '<p class="ghint">You hold no worlds yet.</p>';
  h += rivetAsk('defense', 'Defenders stay until they fall, up to each world\u2019s limit: Outer Rim 6, Mid Rim 8, Core 10. Worlds also rebuild one defender every two rounds on their own, back up to where they started. A patrol is two hired ships that guard a world this round and next; they fight first, then go home.');
  if (prep && mine.length) {
    const adds = Object.values(defAdd).reduce((a, b) => a + b, 0), pats = Object.values(defPat).filter(Boolean).length;
    h += '<div class="dfTotal"><span>' + (adds ? adds + ' DEFENDER' + (adds === 1 ? '' : 'S') : '') + (adds && pats ? ' + ' : '') + (pats ? pats + ' PATROL' + (pats === 1 ? '' : 'S') : '') + (adds || pats ? '' : 'NOTHING YET') + '</span><b>' + cost + ' CR</b></div>' +
         '<button class="pill dfGo" onclick="defConfirm()"' + (cost ? '' : ' disabled') + '>CONFIRM' + (cost ? ' \u00b7 ' + cost + ' CR' : '') + '</button>';
  }
  h += '<div class="biAct"><button class="ghost" onclick="sheetHide()">CLOSE</button></div>';
  sheetShow('defense', h, '#5FD4FF');
}
async function defConfirm() {
  const id = encodeURIComponent(myId);
  for (const k in defAdd) for (let n = 0; n < defAdd[k]; n++) await post('/invest?id=' + id + '&planet=' + k);
  for (const k in defPat) if (defPat[k]) await post('/patrol?id=' + id + '&planet=' + k);
  defAdd = {}; defPat = {}; await poll(); defSheet();
}
let cardSlot = -1;  // 3.1-dev.103: whose card (yours by default; Standings rows open theirs)
function cardHtml(s) {
  const who = cardSlot >= 0 && s.players[cardSlot] ? cardSlot : mySlot, mine = who === mySlot;
  const me = s.players[who], id = me.commander;
  if (id < 0) return '';
  const f = Math.floor(id / 4), cls = id % 4, from = DEFECTOR_FROM[id];
  let h = '<div class="feedTop"><button class="feedBack" onclick="closeCard()">\u2039</button><div class="feedTitle">' + (mine ? 'YOUR CARD' : 'PLAYER ' + (who + 1)) + '</div><span style="width:44px"></span></div>';
  h += '<div class="cprev" style="--fc:' + FACTION_UI_COLORS[f] + '"><div class="cprevTop">' + (ART ? '<img class="pixel cface big" src="' + portraitURL(id, 4) + '" alt="">' : '') +
       '<div><div class="cmeta">' + FACTION_SHORT_NAMES[f] + ' \u00b7 ' + CLASSES[cls].name + (from >= 0 ? ' \u00b7 DEFECTOR' : '') + '</div><div class="cname">' + charName(id) + '</div>' +
       '<div class="cquirk">' + CHARACTERS[id].quirk + '</div></div></div>' +
       '<div class="cfactRow"><div><b>HOMEWORLD</b>' + CHARACTERS[id].home + '</div><div><b>AGE</b>' + CHARACTERS[id].age + '</div><div><b>SHIP</b>' + CHARACTERS[id].ship + '</div></div>' +
       '<div class="cstory">' + CHARACTERS[id].story + '</div>' +
       '<div class="cpitch">' + CLASS_PITCH[cls] + '<small>Once a game: <b>' + CLASSES[cls].move + '</b></small></div>' + (mine ? questHtml(s) : '') +
       '<button class="pill" style="border-color:#FFB347;color:#FFB347" onclick="openKit(' + who + ')">' + (mine ? 'YOUR KIT' : 'THEIR KIT') + '<small>' + (mine ? 'What works for you, against you, and your once-a-game move' : 'Their class, their power, what you know about them') + '</small></button></div>';
  return h;
}
function commanderVoice(f, myCmd) { if (f < 0 || f > 3) return -1; for (const v of FACTION_VOICES[f]) if (v !== myCmd) return v; return FACTION_VOICES[f][0]; }
let cmdPick = 0, cmdBack = false, cmdEdit = false;
function pickCmd(i) { cmdPick = i; poll(); }
function cmdBackToFactions() { cmdBack = true; lastFactionSnapshot = ''; poll(); }
async function takeCommand() {
  const me = lastS && lastS.players[mySlot]; if (!me || me.chosenFaction < 0) return;
  await post('/choose_commander?id=' + encodeURIComponent(myId) + '&commander=' + (me.chosenFaction * 4 + cmdPick));
  cmdEdit = false;
  poll();
}
function loadArt() { api('/art').then(a => { if (a && a.c) { ART = a; shownHtml = null; poll(); } else setTimeout(loadArt, 2000 + Math.random() * 2000); }); }  // 3.1-dev.110: retried until it arrives
loadArt();
function insigniaSvg(f, size) {
  const c = ['#FFC933', '#E6EEF5', '#9B5FE0', '#D0343E'][f];
  const body = [
    '<circle cx="14" cy="14" r="5" fill="'+c+'"/><path d="M14 2v5M14 21v5M2 14h5M21 14h5M5.5 5.5l3.5 3.5M19 19l3.5 3.5M5.5 22.5l3.5-3.5M19 9l3.5-3.5" stroke="'+c+'" stroke-width="2" stroke-linecap="round"/>',
    '<polygon points="14,2 24.4,8 24.4,20 14,26 3.6,20 3.6,8" fill="none" stroke="'+c+'" stroke-width="2"/><circle cx="14" cy="14" r="4.5" fill="'+c+'"/>',
    '<polygon points="14,2 26,14 14,26 2,14" fill="none" stroke="'+c+'" stroke-width="2"/><circle cx="14" cy="14" r="4" fill="'+c+'"/>',
    '<path d="M19 4a11 11 0 1 0 0 20a8.5 8.5 0 1 1 0-20z" fill="'+c+'"/>'][f];
  return '<svg width="'+size+'" height="'+size+'" viewBox="0 0 28 28" aria-hidden="true">'+body+'</svg>';
}
const TX_FACTIONS = [
  { name: 'REBEL ALLIANCE', color: '#FFC933', glyph: 'R' },
  { name: 'GALACTIC EMPIRE', color: '#E6EEF5', glyph: 'E' },
  { name: 'PYKE SYNDICATE', color: '#9B5FE0', glyph: 'P' },
  { name: 'CRIMSON DAWN', color: '#B0202A', glyph: 'C' },
];
let txStaticTimer = null;
let txTypeTimer = null;
let txFullText = '';
let txRevealLen = 0;
let txAckInFlight = false;
let txIsObligation = false;
let txObligationShowing = false;  // true while an obligation dialog awaits an answer

// 3.1-dev.75: the typed line, the chips, and the CONTINUE that fills before it works.
let txContAt = 0, txIncomingTimer = null, txAckN = 1;
function txStartTypewriter() {
  if (txTypeTimer) clearInterval(txTypeTimer);
  txTypeTimer = setInterval(() => {
    if (txRevealLen < txFullText.length) { txRevealLen++; document.getElementById('txBody').textContent = txFullText.slice(0, txRevealLen); }
    else { clearInterval(txTypeTimer); txTypeTimer = null; }
  }, 22);
}
function txResolveIncoming() {  // the call is answered: the hologram, the line, the chips
  if (txIncomingTimer) { clearTimeout(txIncomingTimer); txIncomingTimer = null; }
  const o = document.getElementById('txOverlay');
  if (!o.classList.contains('incoming')) return;
  o.classList.remove('incoming');
  void o.offsetWidth; o.classList.add('shown');
  txContAt = performance.now() + 1600;
  setTimeout(() => { const b = document.getElementById('txCont'); if (b) b.classList.add('ready'); }, 1600);
  txStartTypewriter();
}
function txWhyTap(ev) { if (ev) ev.stopPropagation(); const o = document.getElementById('txOverlay'), on = !o.classList.contains('why'); o.classList.toggle('why', on);
  document.getElementById('txWhy').innerHTML = on ? 'LESS &#9652;' : 'WHY? &#9662;'; }
function txContTap(ev) {
  if (ev) ev.stopPropagation();
  if (performance.now() < txContAt) return;  // not too skippable: the button fills first
  const o = document.getElementById('txOverlay'); o.classList.remove('active', 'shown', 'why', 'incoming');
  if (txIsObligation) { txObligationShowing = false; post('/transmission_ack?id=' + encodeURIComponent(myId)); }
}
// One chip: a standing that moved (up green / down red, the NEW tier word), or a gift (gold) / a lost gift (red).
function txChip(cls, name, word, delayMs) { return '<div class="txChip ' + cls + '" style="transition-delay:' + delayMs + 'ms"><b>' + name + '</b><em>' + word + '</em></div>'; }
function txChipsFor(s, f, from, to, delta, k0) {
  const out = [], tw = t => favorWord(f, t).toUpperCase(), N = FACTION_SHORT_NAMES[f];
  const moved = from >= 0 && to >= 0 && from !== to, dir = moved ? Math.sign(to - from) : Math.sign(delta || 0), cur = to >= 0 ? to : tierIdx(s, f);
  if (dir) out.push(['standing ' + (dir > 0 ? 'up' : 'dn'), insigniaSvg(f, 16) + ' ' + N, (dir > 0 ? '\u25B2 ' : '\u25BC ') + tw(cur)]);
  if (moved && from < 3 && to >= 3 && s.v2 && ((s.v2.gifts || 0) & (1 << f))) out.push(['gift', 'NEW GIFT \u00b7 ' + GIFT4[f][0].toUpperCase(), GIFT_SHORT4[f]]);
  if (moved && to === 4 && from < 4) out.push(['gift', N + ' FAMILY \u00b7 ' + GIFT5[f][0].toUpperCase(), 'WHILE FAMILY']);
  if (moved && from === 4 && to < 4) out.push(['lost', 'GIFT LOST \u00b7 ' + GIFT5[f][0].toUpperCase(), '']);
  return out;
}
const GIFT_SHORT4 = ['+1 SHIP', '2 DEFENSES', '+6 CR', '2 SYMPATHIES'];

// v0.29.0: a reputation change as arrows -- one for small, two for medium, three for big
// (REP_ARROWS_TWO / REP_ARROWS_THREE in game_state.h). No numbers, by design.
function repArrows(d) {
  if (!d) return '';
  const n = Math.abs(d) >= 10 ? 3 : Math.abs(d) >= 5 ? 2 : 1;
  return (d > 0 ? '▲' : '▼').repeat(n);
}
function showTransmission(factionIdx, text, isObligation, fromTier, toTier, big, cause, delta, also) {
  txIsObligation = !!isObligation;
  if (!(factionIdx >= 0 && factionIdx < 4)) factionIdx = 0;
  const s = lastS, fac = TX_FACTIONS[factionIdx], col = big ? '#FFD166' : FACTION_UI_COLORS[factionIdx];
  const o = document.getElementById('txOverlay');
  o.style.setProperty('--tc', col);
  // the incoming moment
  document.getElementById('txTitle').textContent = big ? 'PRIORITY TRANSMISSION' : 'TRANSMISSION INCOMING';
  document.getElementById('txInF').textContent = fac.name;
  document.getElementById('txInSym').innerHTML = insigniaSvg(factionIdx, 64);
  // the call itself
  const kick = document.getElementById('txKick');
  kick.className = big ? 'prio' : '';
  const courtK = cause === 4 && s && s.txPlanet >= 0 && s.planets[s.txPlanet] && s.planets[s.txPlanet].tc && s.planets[s.txPlanet].owner === mySlot;
  kick.innerHTML = insigniaSvg(factionIdx, 14) + '<span>' + (big ? 'PRIORITY' : cause === 6 ? 'PROMISE KEPT' : cause === 7 ? 'PROMISE BROKEN' : courtK ? s.planets[s.txPlanet].name.toUpperCase() + '\u2019S COURT' : 'TRANSMISSION') + ' \u00b7 <b>' + fac.name + '</b></span>';
  const me = s && s.players[mySlot];
  const voice = commanderVoice(factionIdx, me ? me.commander : -1);
  const img = portraitURL(voice, 5, true);
  document.getElementById('txPortrait').innerHTML = img ? '<img class="pixel" src="' + img + '" alt="">' : '';
  const P = s && s.txPlanet >= 0 && s.planets[s.txPlanet] ? s.planets[s.txPlanet].name.toUpperCase() : '';
  const courtTx = cause === 4 && s && s.txPlanet >= 0 && s.planets[s.txPlanet] && s.planets[s.txPlanet].tc && s.planets[s.txPlanet].owner === mySlot;  // 3.1-dev.84
  document.getElementById('txRelay').textContent = (courtTx ? P + '\u2019S COURT' : P ? P + ' RELAY' : FACTION_SHORT_NAMES[factionIdx] + ' CHANNEL') + ' \u00b7 CH ' + ((s ? s.round : 1) || 1);
  document.getElementById('txNameTag').textContent = ART && ART.c[voice] ? ART.c[voice].n.toUpperCase() : fac.name;
  document.getElementById('txBody').textContent = '';
  // the changes, one chip each: this faction, the other faction that reacted to the same thing, gifts
  let chips = [];
  if (!isObligation && s) {
    chips = txChipsFor(s, factionIdx, fromTier, toTier, delta);
    if (also && also[0] >= 0 && also[0] < 4 && also[0] !== factionIdx) chips = chips.concat(txChipsFor(s, also[0], also[1], also[2], also[3]));
    else if (cause === 0 || cause === 1) chips.push(['standing up', insigniaSvg(RIVAL_OF[factionIdx], 16) + ' ' + FACTION_SHORT_NAMES[RIVAL_OF[factionIdx]], '\u25B2 PLEASED']);
  }
  chips.sort((a, b) => (a[0].startsWith('standing') ? 0 : 1) - (b[0].startsWith('standing') ? 0 : 1));
  document.getElementById('txChips').innerHTML = chips.map((c, k) => txChip(c[0], c[1], c[2], 200 + k * 250)).join('');
  const rd = (!isObligation && s) ? txReadout(s, factionIdx, cause === undefined ? -1 : cause, s.txPlanet === undefined ? -1 : s.txPlanet, fromTier, toTier, delta) : '';
  document.getElementById('txRead').innerHTML = rd;
  document.getElementById('txWhy').style.display = rd ? '' : 'none';
  document.getElementById('txWhy').innerHTML = 'WHY? &#9662;';
  document.getElementById('txCont').classList.remove('ready');
  txFullText = '\u201C' + text + '\u201D';
  txRevealLen = 0;
  txContAt = Infinity;
  o.classList.remove('shown', 'why');
  o.classList.add('active', 'incoming');
  o.scrollTop = 0;
  if (txIncomingTimer) clearTimeout(txIncomingTimer);
  txIncomingTimer = setTimeout(txResolveIncoming, big ? 1800 : 1100);
}

function txTap(ev) {
  const o = document.getElementById('txOverlay');
  if (o.classList.contains('incoming')) { txResolveIncoming(); return; }
  if (txRevealLen < txFullText.length) {  // a tap finishes the line; the chips and CONTINUE carry on
    txRevealLen = txFullText.length;
    document.getElementById('txBody').textContent = txFullText;
    if (txTypeTimer) { clearInterval(txTypeTimer); txTypeTimer = null; }
  }
}

// ---------------------------------------------------------------------------
// Battle scene (v0.20.0) -- the phone twin of the AMOLED's battleSceneDrawCb:
// same 380x200 scene coordinates, timeline, layout, planet math, and
// palettes, so both platforms show the same battle. Keep them in sync.
// ---------------------------------------------------------------------------
// UI 3.0 step 1 (2.0-dev.35, user: the battle "feels like you are playing a mini game"): a PORTRAIT
// scene, scaled to fill the phone -- ships, dice and planet about twice as big. (The AMOLED keeps
// its own 380x200 copy until its UI 3.0 pass -- AMOLED_TODO.)
const SCENE_W = 200, SCENE_H = 300;
const BA_DICE = 600, BA_SHOTS = 900, BA_FLASH = 1000, BA_BOOM = 1400, BA_TOTAL = 1500, BA_SHAKE = 260;
const SHIP_W = 26, SHIP_H = 16, SHIP_LEFT_X = 6, SHIP_RIGHT_X = SCENE_W - 6 - 26, SHIP_TOP_Y = 66, SHIP_STEP = 23;  // 2.0-dev.20: 23 (was 26) so an Empire column of 7 fits
const GARRISON_X = SCENE_W - 28, GARRISON_TOP_Y = 64, GARRISON_STEP = 23, GARRISON_SIZE = 14, GARRISON_COLOR = '#FF7A45';
// 2.0-dev.30 SHIP ART (mockup "Ship art" take 2, user: "I like it"): original pixel ships --
// worn hulls, faction color as markings -- and a garrison KIND per world. GENERATED from
// tools/art/ships/make_ships.py (grids face right; '.' = empty). Drawn 1:1 into the scene.
const SPRITES = {"rebel":[[".....DDDDDDDD...........","....DhHHHHHHHD..........",".EeDhHMMHHHHHHDGGGGGGGD.",".EeDhhhhhhhhhD..........",".......DhD..............",".....DDhHDDDDDDDD.......","EeEDhHHHHHHHWWWHHHHHHHDA","EeEDhHHMHHHHWWHHHHHHHD..",".....DhhRRhhhhhhhhhD....",".......DhD..............",".....DhHHMMHD...........",".EeD.DhhhhhhD...........",".....DDDDDDD............"],{"D":"#3E3C36","H":"#DCD6C8","h":"#A8A294","M":"#FFC933","W":"#2E5A78","G":"#6E6A60","R":"#7A5236","E":"#FF7A3D","e":"#FFE0A0","A":"#FFFFFF"}],"empire":[["DD................","DBDD..............","DBBBDD............","EDBBBBBDD.........","EDBBBKBBBBDD......","EDBBBKBLLWBBBDD...","EDBBBKBLLWBBBBBBLA","EDBBBKBLLWBBBDD...","EDBBBKBBBBDD......","EDBBBBBDD.........","DBBBDD............","DBDD..............","DD................"],{"D":"#5C6670","B":"#C9D3DD","L":"#F2F6FA","W":"#7FD8FF","K":"#8C98A4","E":"#6FB8FF","A":"#FFFFFF"}],"pyke":[[".....DDDDDDDDD..........","....DhHHHHHHHhD.........","...DhHMMMMMMHHhDDD......","EeDhHHGHGHGHHHHHHHD.....","EeDhHHHHHHHHHHHWWHhD....","EeDhHHGHGHGHHHHHHHHHDA..","EeDhHHHHHHHHHHHWWHhD....","EeDhHHGHGHGHHHHHHHD.....","...DhHMMMMMMHHhDDD......","....DhRHHHHHHhD.........",".....DDDDDDDDD.........."],{"D":"#33312A","H":"#8E8A76","h":"#66624F","M":"#A56BFF","G":"#4A473C","W":"#FFC94A","R":"#7A5236","E":"#C7FF6B","e":"#F4FFD6","A":"#FFFFFF"}],"crimson":[["..........DD............",".........DMMD...........","........DhHHD...........",".......DhHHHD...........","EE....DhHHHHHDD.........","EeDDDDhHHHHHHHMMMMDD....","EeMMMMHHHHHWWHHHHHHHMMDA","EeDDDDhHHHHHHHMMMMDD....","EE....DhHHHHHDD.........",".......DhHHHD...........","........DhHHD...........",".........DMMD...........","..........DD............"],{"D":"#5A464A","H":"#1E1719","h":"#2E2427","M":"#E0B04A","W":"#FF5A6A","E":"#FF3B4E","e":"#FFC2C8","A":"#FFE8B0"}],"merc":[["...DDDDD..........","..DHHRRHD.........","..DHHHHHHDD.......","EeDHRHHHHHHDDDDDDD","EeDHHHWWHHHHGGGGGA","EeDHHHHHHRHHDDDDDD","..DHHHHHHDD.......","..DHRHHHD.........","...DDDDD.........."],{"D":"#3E3A34","H":"#9C8F7A","R":"#C2603A","G":"#5A5448","W":"#7FD8FF","E":"#FFB13D","e":"#FFF0C8","A":"#FFFFFF"}],"g_asteroid":[[".....RRRRR......","...RRrrrRRRR....","..RRrRRRRRrRR...","AGGDDHHHHDRRRR..","..RRDHEEHDRrRR..","AGGDDHHHHDRRRR..","..RRrRRRRRRrR...","...RRRrrRRRR....",".....RRRRR......"],{"D":"#38342E","H":"#9A9488","h":"#706B60","M":"#FF7A45","G":"#56514A","E":"#FF4A3A","W":"#FFB08A","A":"#FFFFFF","R":"#6A5444","r":"#8A7060"}],"g_picket":[["...........DDDD.....","........DDhHHHhD....","AGGGGGDDhHHMHHHHDEE.",".....DhHWWHHHHHHHDEE","AGGGGGDDhHHMHHHHDEE.","........DDhHHHhD....","...........DDDD....."],{"D":"#38342E","H":"#9A9488","h":"#706B60","M":"#FF7A45","G":"#56514A","E":"#FF4A3A","W":"#FFB08A","A":"#FFFFFF","R":"#6A5444","r":"#8A7060"}],"g_platform":[["....DDDDDD....","..DDHHHHHHDD..",".DHHMMMMMMHHD.",".DHMDDDDDDMHD.","AAHHDGGGGDHHDD",".DHMDGEEGDMHD.","AAHHDGEEGDHHDD",".DHMDGGGGDMHD.",".DHMDDDDDDMHD.",".DHHMMMMMMHHD.","..DDHHHHHHDD..","....DDDDDD...."],{"D":"#38342E","H":"#9A9488","h":"#706B60","M":"#FF7A45","G":"#56514A","E":"#FF4A3A","W":"#FFB08A","A":"#FFFFFF","R":"#6A5444","r":"#8A7060"}],"g_ion":[["......DDDD......","....DDhHHhDD....","...DhHHMMHHhD...","AAAAAAAAAGHHHD..","...DhHHMMHHhD...","....DDhHHhDD....","...DD.DhhD.DD...","..DhD..DD..DhD.."],{"D":"#38342E","H":"#9A9488","h":"#706B60","M":"#FF7A45","G":"#56514A","E":"#FF4A3A","W":"#FFB08A","A":"#FFFFFF","R":"#6A5444","r":"#8A7060"}],"g_station":[[".....DDDDDDDD.....","...DDhHHHHHHhDD...","..DhHHMHHHHMHHhD..","AGGDhHDDDDDDHHhD..",".DhHHDEEEEEEDHHhD.","AGGDhHDEWWEEDHHhDD",".DhHHDEEEEEEDHHhD.","AGGDhHDDDDDDHHhD..","..DhHHMHHHHMHHhD..","...DDhHHHHHHhDD...",".....DDDDDDDD....."],{"D":"#38342E","H":"#9A9488","h":"#706B60","M":"#FF7A45","G":"#56514A","E":"#FF4A3A","W":"#FFB08A","A":"#FFFFFF","R":"#6A5444","r":"#8A7060"}]};
const FACTION_SHIP = ['rebel', 'empire', 'pyke', 'crimson'];
const SPRITE_CACHE = {};
// key + variant: '' = as drawn, 'w' = white (a hit flash), 'o' = white outline (the escort), 'm<f>' = militia in faction f's colors.
function spriteCanvas(key, variant, flip) {
  const ck = key + ':' + (variant || '') + ':' + (flip ? 1 : 0);
  if (SPRITE_CACHE[ck]) return SPRITE_CACHE[ck];
  const sp = SPRITES[key]; if (!sp) return null;
  const g = sp[0], h = g.length, w = g[0].length, pad = variant === 'o' ? 1 : 0;
  let pal = sp[1];
  if (variant && variant[0] === 'm') {  // militia: the platform with the world's faction markings
    const fc = TX_FACTIONS[+variant.slice(1)] ? TX_FACTIONS[+variant.slice(1)].color : '#FFFFFF';
    pal = Object.assign({}, pal, { M: fc, E: fc });
  }
  const c = document.createElement('canvas'); c.width = w + 2 * pad; c.height = h + 2 * pad;
  const x = c.getContext('2d');
  const at = (cx, cy) => g[cy] && g[cy][cx] && g[cy][cx] !== '.';
  for (let yy = 0; yy < h; yy++) for (let xx = 0; xx < w; xx++) {
    const ch = g[yy][xx]; if (ch === '.') continue;
    const px = flip ? w - 1 - xx : xx;
    if (pad) for (const [ox, oy] of [[1,0],[-1,0],[0,1],[0,-1]]) if (!at(xx + ox, yy + oy)) { x.fillStyle = '#FFFFFF'; x.fillRect((flip ? w - 1 - (xx + ox) : xx + ox) + pad, yy + oy + pad, 1, 1); }
    x.fillStyle = variant === 'w' ? '#FFFFFF' : (pal[ch] || '#FF00FF');
    x.fillRect(px + pad, yy + pad, 1, 1);
  }
  return (SPRITE_CACHE[ck] = c);
}
function garrisonKind(s, p) {
  const pl = s.planets[p]; if (!pl) return 'g_platform';
  if (pl.home >= 0) return FACTION_SHIP[pl.home];  // a home guard flies its own faction's ships
  const ring = pl.ring | 0;
  return ring === 2 ? 'g_station' : ring === 1 ? (p % 2 ? 'g_ion' : 'g_platform') : (p % 2 ? 'g_picket' : 'g_asteroid');
}
const DIE_SIZE = 44, DIE_Y = 8;
const PLANET_GRID = 28, PLANET_CELL = 5, PLANET_CX = SCENE_W / 2, PLANET_CY = SCENE_H + 6, PLANET_STEP_MS = 120;  // rises from the bottom edge
// Must match PLANET_PALETTES in the .ino (index order = PLANETS[]).
const PLANET_PALETTES = [
  ['#7A5230','#C08A4A','#E3C07A','#F5E1A8'], ['#5A7A9A','#9EC3DE','#DDEFFA','#FFFFFF'],
  ['#8A4A3A','#D08858','#F0B888','#FFE0C0'], ['#1E3A22','#3E6B35','#6E8F4A','#9CB06A'],
  ['#2A2A38','#55556A','#8A8A9E','#FFD166'], ['#3A2A1A','#6B4A2A','#9A7A4A','#E0C080'],
  ['#3A2440','#6A4A6A','#9A7A8A','#C0A0B0'], ['#3A0A0A','#7A1A1A','#B03030','#E06050'],
  ['#1A3A6A','#2A6AAA','#4AA04A','#E0E0D0'], ['#1A4A8A','#3A8AC8','#5AB05A','#F0F0F0'],
  ['#10301A','#1E5A2A','#3A8A3A','#6AB04A'], ['#4A5A6A','#8A9AAA','#C0D0DA','#FFFFFF'],
  ['#0E2A2A','#1E5A44','#3E9A5A','#C8D890'], ['#140606','#3A1410','#C0401A','#FFA040'],
  ['#2A2A10','#5A5A20','#8A8A3A','#C8B860'], ['#0A2A4A','#1A5A8A','#3A9AC8','#F0D080'],
  ['#141820','#3A4250','#7A8698','#E8B850'],
];
let bKey = '', bView = null, bAnimActive = false, bAnimStart = 0, bBtnSnap = '', bGen = 0, bLastS = null;
let bPlanetCanvas = null, bPlanetFor = '';

function bNoise(seed) { const x = Math.sin(seed * 12.9898) * 43758.5453; return x - Math.floor(x); }
function bSlotColor(s, slot) {
  const p = s.players[slot];
  return (p && p.chosenFaction >= 0) ? TX_FACTIONS[p.chosenFaction].color : '#38BDF8';
}
function bViewFrom(s) {
  const pvp = s.battleKind === 2;
  const pIdx = s.battleTargetPlanet;
  return {
    pvp: pvp,
    leftIsAttacker: !(pvp && s.battleDefenderSlot === mySlot),
    atkAlive: s.battleAtkAlive.map(Boolean), defAlive: s.battleDefAlive.map(Boolean), stride: s.shipStride || 6,
    atkFleets: s.battleAtkFleets || 1, defFleets: s.battleDefFleets || 1, atkSlot: s.battleAttackerSlot, defSlot: s.battleDefenderSlot,
    defRemaining: s.defRemaining, atkRoll: s.lastAtkRoll, defRoll: s.lastDefRoll,
    atkMod: s.battleAtkMod, defMod: s.battleDefMod, retreat: s.lastRetreatSide,
    // 2.0-dev.24: the escort ship (its own spot, loss index escIdx) and the militia (top garrison square)
    escAlive: (s.bEsc || [-1, 0])[1], escColor: (s.bEsc && s.bEsc[0] >= 0) ? TX_FACTIONS[s.bEsc[0]].color : '#FFFFFF',
    escIdx: 3 * (s.shipStride || 6), milAlive: (s.bMil || [-1, 0])[1],
    atkMercs: (s.bMerc || [0, 0])[0], defMercs: (s.bMerc || [0, 0])[1],
    atkFac: s.players[s.battleAttackerSlot] ? s.players[s.battleAttackerSlot].chosenFaction : -1,  // 2.0-dev.30 ship art
    defFac: pvp && s.players[s.battleDefenderSlot] ? s.players[s.battleDefenderSlot].chosenFaction : -1,
    escFac: s.bEsc ? s.bEsc[0] : -1, milFac: s.bMil ? s.bMil[0] : -1, gKind: pvp ? '' : garrisonKind(s, pIdx),  // 2.0-dev.26: hired ship k = loss index escIdx + 1 + k
    milColor: (s.bMil && s.bMil[0] >= 0) ? TX_FACTIONS[s.bMil[0]].color : GARRISON_COLOR,
    lossIdx: s.lastLossIdx, exchange: s.battleExchange, over: s.battleOver, planetIdx: pIdx,
    captured: !pvp && pIdx >= 0 && s.planets[pIdx].owner === s.battleAttackerSlot,
    atkColor: bSlotColor(s, s.battleAttackerSlot),
    defColor: pvp ? bSlotColor(s, s.battleDefenderSlot) : GARRISON_COLOR,
  };
}
function bSkip() { if (bAnimActive) bAnimStart = performance.now() - BA_TOTAL; }
// WITHDRAW is a two-tap confirm (v0.20.2, matches the AMOLED): the first tap
// arms it and explains the parting shot; it disarms after 4s or on FIGHT.
const DISARMED = -1e9;  // far in the past: performance.now() starts near 0 on page load
let bWithdrawArmedAt = DISARMED;
function bWithdrawArmed() { return performance.now() - bWithdrawArmedAt < 4000; }
function bAct(a) {
  bSkip();
  if (a === 2 && !bWithdrawArmed()) { bWithdrawArmedAt = performance.now(); if (bLastS) bUpdate(bLastS); return; }
  bWithdrawArmedAt = DISARMED;
  battleAction(a);
}

async function bReinforce(f) { await post('/reinforce?id=' + encodeURIComponent(myId) + '&fleet=' + f); poll(); }  // v0.28.0

function bUpdate(s) {
  bLastS = s;
  if (s.battleExchange !== bView.exchange) {
    bView = bViewFrom(s);
    if (bView.exchange > 0) { bAnimActive = true; bAnimStart = performance.now(); }
  } else if (!bAnimActive) {
    bView = bViewFrom(s);
  }
  const pvp = s.battleKind === 2;
  const iAmDef = pvp && s.battleDefenderSlot === mySlot;
  if (s.battleAttackerSlot !== mySlot && s.battleDefenderSlot !== mySlot) {  // 3.1-dev.54 WATCHING
    document.getElementById('bTitle').innerHTML = 'WATCHING \u00b7 ' + pTag(s, s.battleAttackerSlot, true) + ' vs ' + (pvp ? pTag(s, s.battleDefenderSlot, true) : (s.planets[s.battleTargetPlanet] || {}).name || '');
    const wt = bAnimActive ? '' : s.battleOver ? s.resultText : (s.resultText || 'The fight is on.');
    document.getElementById('bText').textContent = wt;
    document.getElementById('bText').style.fontSize = wt.length > 70 ? '13px' : wt.length > 44 ? '15px' : '';
    const ws = s.battleOver ? 'wdone' : 'watch';
    if (ws !== bBtnSnap) { bBtnSnap = ws; document.getElementById('bBtns').innerHTML = '<button class="big" style="border-color:var(--line);color:var(--dim)" onclick="stopWatchBattle()">' + (s.battleOver ? 'CLOSE' : 'STOP WATCHING') + '</button>'; }
    return;
  }
  let title;
  if (pvp) {
    const other = iAmDef ? s.battleAttackerSlot : s.battleDefenderSlot;
    const of = s.players[other] ? s.players[other].chosenFaction : -1;  // UI 3.0: the short name keeps the big title on one line
    title = 'BATTLE vs ' + (of >= 0 ? FACTION_SHORT_NAMES[of] : s.players[other] ? s.players[other].name : 'ENEMY');
  } else {
    title = 'BATTLE: ' + s.planets[s.battleTargetPlanet].name;
  }
  document.getElementById('bTitle').textContent = title;
  const iActed = pvp && (iAmDef ? s.defAction : s.atkAction) !== 0;
  // 2.0-dev.28 AUTO: fight on after each exchange lands, until YOUR side is down to 2 (then you choose: fight or escape).
  if (s.battleOver) bAuto = false;
  if (bAuto && !bAnimActive && !s.battleOver && !iActed && bAutoFired !== s.battleExchange) {
    const mine = bMine(s);
    if (mine <= bAutoFloor) { bAuto = false; bAutoStopped = 'AUTO paused: you\x27re down to ' + mine + (mine === 1 ? ' ship' : ' ships') + '. FIGHT, AUTO to the end, or WITHDRAW?'; }
    else { bAutoFired = s.battleExchange; battleAction(1); }
  }
  let text;
  if (bAnimActive) text = '';  // don't spoil the exchange; shown when it lands
  else if (s.battleOver) text = s.resultText;
  else if (iActed) text = 'Waiting for the other commander...';
  else if (bWithdrawArmed()) text = pvp
    ? 'Withdrawing gives the enemy a parting shot at your fleet. Tap CONFIRM? to go.'
    : 'Withdrawing gives the defenders a parting shot, and this planet stays closed to you this round.';
  else if (bAutoStopped) text = bAutoStopped;
  else text = s.resultText || (pvp ? 'Choose your action.' : 'The defenders fight to the death.');
  document.getElementById('bText').textContent = text;
  document.getElementById('bText').style.fontSize = text.length > 70 ? '13px' : text.length > 44 ? '15px' : '';  // 3.1-dev.17: fits two lines
  document.getElementById('bText').style.color = (bWithdrawArmed() && !bAnimActive && !s.battleOver) ? '#FF6B6B' : '';
  // v0.28.0: the PvP defender may bring another fleet before the first exchange.
  const reinf = s.battleOver ? -1 : [0, 1, 2].find(f => (s.canReinforce || 0) & (1 << f));
  const canMercs = !s.battleOver && s.canCallMercs;
  const snap = (s.battleOver ? 'done' : (bAuto ? 'auto' : iActed ? 'wait' : (bWithdrawArmed() ? 'armed' : 'act'))) + (reinf >= 0 ? '+' + reinf : '') + (canMercs ? 'm' : '');
  if (snap !== bBtnSnap) {
    bBtnSnap = snap;
    let b = '';
    const base = snap.split('+')[0].replace(/m$/, '');
    // 3.1-dev.91: two rows, always the same height -- the extras (bring a fleet, hired ships) share the second row
    let extra = '';
    if (reinf >= 0) extra += '<button style="border-color:#8CE7A0;color:#8CE7A0" onclick="bReinforce('+reinf+')">+ FLEET '+(reinf + 1)+'</button>';
    if (canMercs) extra += '<button style="border-color:#B8A88A;color:#B8A88A" onclick="bCallMercs()">+ HIRED SHIPS</button>';  // dev.28
    if (base === 'done') b = '<button class="big" onclick="bContinue()">CONTINUE</button>';
    else if (base === 'auto') b = '<button class="big" style="border-color:#5FD4FF;color:#5FD4FF" onclick="bAutoToggle()">STOP AUTO</button>';
    // UI 3.0 step 1: FIGHT + AUTO side by side, WITHDRAW full width underneath.
    else if (base === 'act') b = '<button style="border-color:#FF6B5A;color:#FF6B5A" onclick="bAct(1)">FIGHT</button>' +
      '<button style="border-color:#5FD4FF;color:#5FD4FF" onclick="bAutoToggle()">AUTO</button>' +
      '<div class="brow2"><button style="color:var(--dim);border-color:var(--line)" onclick="bAct(2)">WITHDRAW</button>' + extra + '</div>';
    else if (base === 'armed') b = '<button style="border-color:#FF6B5A;color:#FF6B5A" onclick="bAct(1)">FIGHT</button>' +
      '<button style="border-color:#FF6B6B;color:#FF6B6B;background:#2A0A0A" onclick="bAct(2)">CONFIRM?</button>' + (extra ? '<div class="brow2">' + extra + '</div>' : '');
    if (base === 'auto' && extra) b += '<div class="brow2">' + extra + '</div>';
    document.getElementById('bBtns').innerHTML = b;
  }
}

function bFrame(now, gen) {
  const c = document.getElementById('bCanvas');
  if (gen !== bGen || !c || !bView) return;  // card replaced or gone -- stop this loop
  bDraw(c, now);
  if (bAnimActive && now - bAnimStart >= BA_TOTAL) {
    bAnimActive = false;
    if (bLastS) bUpdate(bLastS);  // reveal the result text
  }
  requestAnimationFrame(t => bFrame(t, gen));
}

// Same fake-sphere math as computePlanetPixels() in the .ino.
function bPlanetPixels(planetIdx, step) {
  const key = planetIdx + ':' + step;
  if (bPlanetFor === key && bPlanetCanvas) return bPlanetCanvas;
  if (!bPlanetCanvas) { bPlanetCanvas = document.createElement('canvas'); bPlanetCanvas.width = PLANET_GRID; bPlanetCanvas.height = PLANET_GRID; }
  const g = bPlanetCanvas.getContext('2d');
  g.clearRect(0, 0, PLANET_GRID, PLANET_GRID);
  const pal = PLANET_PALETTES[planetIdx];
  const phase = (step % 10000) * 0.06, seed = planetIdx * 1.7 + 0.3, half = PLANET_GRID / 2;
  for (let j = 0; j < PLANET_GRID; j++) {
    const ny = (j + 0.5 - half) / half;
    for (let i = 0; i < PLANET_GRID; i++) {
      const nx = (i + 0.5 - half) / half;
      const r2 = nx * nx + ny * ny;
      if (r2 > 1) continue;
      const z = Math.sqrt(1 - r2);
      const lon = Math.atan2(nx, z) + phase;
      let v = Math.sin(ny * 5 + seed + 1.4 * Math.sin(lon * 2 + ny * 3 + seed));
      v += 0.35 * Math.sin(lon * 5 + seed * 2) * Math.sin(ny * 9);
      let idx = v < -0.6 ? 0 : v < 0.1 ? 1 : v < 0.7 ? 2 : 3;
      const light = -0.55 * nx - 0.35 * ny + 0.6 * z;
      if (light < 0.15 && idx > 0) idx--;
      if (light < -0.2 && idx > 0) idx--;
      g.fillStyle = pal[idx];
      g.fillRect(i, j, 1, 1);
    }
  }
  bPlanetFor = key;
  return bPlanetCanvas;
}

function bDraw(c, now) {
  const v = bView;
  // UI 3.0: the canvas fills its box (the full-screen battle); the scene is scaled to fit and centred.
  const cssW = c.clientWidth, cssH = c.clientHeight || Math.round(cssW * SCENE_H / SCENE_W);
  const dpr = window.devicePixelRatio || 1;
  const wantW = Math.round(cssW * dpr), wantH = Math.round(cssH * dpr);
  if (c.width !== wantW || c.height !== wantH) { c.width = wantW; c.height = wantH; }
  const ctx = c.getContext('2d');
  ctx.setTransform(1, 0, 0, 1, 0, 0);
  ctx.fillStyle = '#02060D';
  ctx.fillRect(0, 0, c.width, c.height);
  const k = Math.min(c.width / SCENE_W, c.height / SCENE_H);
  const offX = (c.width - SCENE_W * k) / 2, offY = (c.height - SCENE_H * k) / 2;

  const anim = bAnimActive && v.exchange > 0;
  const t = anim ? Math.min(BA_TOTAL, now - bAnimStart) : BA_TOTAL;
  const rolled = v.exchange > 0;
  let atkWon = rolled && v.atkRoll > v.defRoll;
  let defWon = rolled && v.defRoll > v.atkRoll;
  // Retreat under fire: the withdrawing side can't score (matches the .ino).
  if (v.retreat === 1) atkWon = false;
  else if (v.retreat === 2) defWon = false;
  const hit = atkWon || defWon;
  const garrisonHit = !v.pvp && atkWon;

  let ox = 0, oy = 0;
  if (anim && hit && t >= BA_FLASH && t < BA_FLASH + BA_SHAKE) {
    const s = Math.floor((t - BA_FLASH) * 100 / BA_SHAKE);
    const decay = (1 - s / 100) * (1 - s / 100);
    ox = (bNoise(s) * 2 - 1) * 10 * decay;
    oy = (bNoise(s + 1000) * 2 - 1) * 6 * decay;
  }
  ctx.setTransform(k, 0, 0, k, offX + ox * k, offY + oy * k);

  // A few fixed background stars.
  for (let i = 0; i < 24; i++) {
    const b = 90 + Math.floor(bNoise(i * 5 + 2) * 120);
    ctx.fillStyle = 'rgb(' + b + ',' + b + ',' + Math.min(255, b + 25) + ')';
    ctx.fillRect(bNoise(i) * SCENE_W, bNoise(i + 50) * SCENE_H, 2, 2);
  }

  if (!v.pvp && v.planetIdx >= 0) {
    const pc = bPlanetPixels(v.planetIdx, Math.floor(now / PLANET_STEP_MS));
    ctx.imageSmoothingEnabled = false;
    const size = PLANET_GRID * PLANET_CELL;
    ctx.drawImage(pc, PLANET_CX - size / 2, PLANET_CY - size / 2, size, size);
  }

  const atkLeft = v.leftIsAttacker;
  const blinkOn = Math.floor(t / 50) % 2 === 0;
  const inFlash = anim && t >= BA_SHOTS && t < BA_FLASH;
  const stillShowLoss = anim && t < BA_FLASH;
  // v0.28.0: idx = fleet * stride + slot (2.0-dev.20: stride 7, from /state); each fleet in the battle gets its own column, stacking inward (twin of the .ino).
  const leftMask = atkLeft ? v.atkFleets : v.defFleets, rightMask = atkLeft ? v.defFleets : v.atkFleets;
  const stride = v.stride || 6;
  const shipCenter = (left, idx) => {
    if (idx >= v.escIdx) {  // 2.0-dev.24/26: escort (mid-line) and hired ships fly one column inward
      const mask = left ? leftMask : rightMask; let col = 0; for (let k = 0; k < 3; k++) if (mask & (1 << k)) col++;
      const row = idx === v.escIdx ? 3 : [1, 5, 0, 6][Math.min(3, idx - v.escIdx - 1)];
      return [(left ? SHIP_LEFT_X + col * 30 : SHIP_RIGHT_X - col * 30) + SHIP_W / 2, SHIP_TOP_Y + row * SHIP_STEP + SHIP_H / 2];
    }
    const f = Math.floor(idx / stride), mask = left ? leftMask : rightMask;
    let col = 0; for (let k = 0; k < f; k++) if (mask & (1 << k)) col++;
    return [(left ? SHIP_LEFT_X + col * 30 : SHIP_RIGHT_X - col * 30) + SHIP_W / 2, SHIP_TOP_Y + (idx % stride) * SHIP_STEP + SHIP_H / 2];
  };
  const garrisonCenter = idx => [GARRISON_X + GARRISON_SIZE / 2, GARRISON_TOP_Y + idx * GARRISON_STEP + GARRISON_SIZE / 2];
  // 2.0-dev.30: a sprite centred on (cx, cy); facing right unless told otherwise; white while it flashes.
  const drawSprite = (cx, cy, right, key, variant) => {
    const c = spriteCanvas(key, variant, !right);
    if (!c) return false;
    ctx.imageSmoothingEnabled = false;
    ctx.drawImage(c, Math.round(cx - c.width / 2), Math.round(cy - c.height / 2));
    return true;
  };
  const drawShip = (cx, cy, right, col) => {
    const x = cx - SHIP_W / 2, y = cy - SHIP_H / 2;
    const back = right ? x : x + SHIP_W, nose = right ? x + SHIP_W : x;
    ctx.fillStyle = col;
    ctx.beginPath(); ctx.moveTo(back, y); ctx.lineTo(nose, y + SHIP_H / 2); ctx.lineTo(back, y + SHIP_H); ctx.closePath(); ctx.fill();
    ctx.fillStyle = '#8CE7FF';
    ctx.fillRect(right ? x - 4 : x + SHIP_W + 1, y + SHIP_H / 2 - 2, 3, 4);
  };

  for (let i = 0; i < v.atkAlive.length; i++) {
    const isLoss = defWon && i === v.lossIdx;
    if (!(v.atkAlive[i] || (isLoss && stillShowLoss))) continue;
    const [cx, cy] = shipCenter(atkLeft, i);
    const flash = isLoss && inFlash && blinkOn;
    if (!drawSprite(cx, cy, atkLeft, FACTION_SHIP[v.atkFac], flash ? 'w' : '')) drawShip(cx, cy, atkLeft, flash ? '#FFFFFF' : v.atkColor);
  }
  { const isLoss = defWon && v.lossIdx === v.escIdx;
    if (v.escAlive || (isLoss && stillShowLoss)) {
      const [cx, cy] = shipCenter(atkLeft, v.escIdx);
      // outlined: your own faction often sends the escort, so color alone can't tell it apart
      if (!drawSprite(cx, cy, atkLeft, FACTION_SHIP[v.escFac], (isLoss && inFlash && blinkOn) ? 'w' : 'o')) {
        drawShip(cx, cy, atkLeft, (isLoss && inFlash && blinkOn) ? '#FFFFFF' : v.escColor);
        const x = cx - SHIP_W / 2, y = cy - SHIP_H / 2, back = atkLeft ? x : x + SHIP_W, nose = atkLeft ? x + SHIP_W : x;
        ctx.strokeStyle = '#FFFFFF'; ctx.lineWidth = 1.5;
        ctx.beginPath(); ctx.moveTo(back, y - 2); ctx.lineTo(nose + (atkLeft ? 3 : -3), y + SHIP_H / 2); ctx.lineTo(back, y + SHIP_H + 2); ctx.closePath(); ctx.stroke();
      }
    } }
  // 2.0-dev.26 hired ships (Mercenaries), in a gunmetal tan, for whichever side bought them.
  for (let side = 0; side < 2; side++) {
    const atkSide = side === 0, n = atkSide ? v.atkMercs : v.defMercs, left = atkSide ? atkLeft : !atkLeft;
    const lostHere = atkSide ? defWon : atkWon;
    for (let k = 0; k < 4; k++) {
      const idx = v.escIdx + 1 + k, isLoss = lostHere && v.lossIdx === idx;
      if (!(k < n || (isLoss && stillShowLoss))) continue;
      const [cx, cy] = shipCenter(left, idx);
      if (!drawSprite(cx, cy, left, 'merc', (isLoss && inFlash && blinkOn) ? 'w' : '')) drawShip(cx, cy, left, (isLoss && inFlash && blinkOn) ? '#FFFFFF' : '#B8A88A');
    }
  }
  const garrisonShown = v.defRemaining + ((garrisonHit && stillShowLoss) ? 1 : 0);
  if (v.pvp) {
    for (let i = 0; i < v.defAlive.length; i++) {
      const isLoss = atkWon && i === v.lossIdx;
      if (!(v.defAlive[i] || (isLoss && stillShowLoss))) continue;
      const [cx, cy] = shipCenter(!atkLeft, i);
      const flash = isLoss && inFlash && blinkOn;
      if (!drawSprite(cx, cy, !atkLeft, FACTION_SHIP[v.defFac], flash ? 'w' : '')) drawShip(cx, cy, !atkLeft, flash ? '#FFFFFF' : v.defColor);
    }
  } else {
    for (let i = 0; i < garrisonShown && i < 8; i++) {
      const isLoss = garrisonHit && i === garrisonShown - 1;
      const [cx, cy] = garrisonCenter(i);
      const militia = i >= v.defRemaining - v.milAlive && i < v.defRemaining;
      const col = (isLoss && inFlash && blinkOn) ? '#FFFFFF' : (militia ? v.milColor : GARRISON_COLOR);
      // 2.0-dev.30: this world's garrison kind (faces left, toward you); the militia = a platform in its faction's colors.
      const gKey = militia ? 'g_platform' : v.gKind, gVar = (isLoss && inFlash && blinkOn) ? 'w' : militia ? 'm' + v.milFac : '';
      const homeShip = FACTION_SHIP.includes(gKey);  // ships are drawn facing right -> flip them; stations already face left
      if (gKey && drawSprite(cx, cy, !homeShip, gKey, gVar)) continue;
      ctx.globalAlpha = 0.6; ctx.fillStyle = col;
      ctx.fillRect(cx - GARRISON_SIZE / 2, cy - GARRISON_SIZE / 2, GARRISON_SIZE, GARRISON_SIZE);
      ctx.globalAlpha = 1; ctx.strokeStyle = col; ctx.lineWidth = 2;
      ctx.strokeRect(cx - GARRISON_SIZE / 2, cy - GARRISON_SIZE / 2, GARRISON_SIZE, GARRISON_SIZE);
    }
    if (garrisonShown > 8) {  // 3.1-dev.76: Core worlds can hold 10 -- the rest as a number
      ctx.fillStyle = GARRISON_COLOR; ctx.font = 'bold 12px sans-serif'; ctx.textAlign = 'center';
      ctx.fillText('+' + (garrisonShown - 8), GARRISON_X + GARRISON_SIZE / 2, GARRISON_TOP_Y + 8 * GARRISON_STEP + 8);
    }
  }

  const firstUnit = attackerSide => {
    if (attackerSide || v.pvp) {
      const alive = attackerSide ? v.atkAlive : v.defAlive;
      const left = attackerSide ? atkLeft : !atkLeft;
      const lostHere = attackerSide ? defWon : atkWon;
      for (let i = 0; i < alive.length; i++) if (alive[i] || (lostHere && i === v.lossIdx)) return shipCenter(left, i);
      return shipCenter(left, 2);
    }
    return garrisonCenter(0);
  };
  let target = [0, 0];
  if (garrisonHit) target = garrisonCenter(Math.max(0, garrisonShown - 1));
  else if (atkWon && v.pvp) target = shipCenter(!atkLeft, Math.max(0, v.lossIdx));
  else if (defWon) target = shipCenter(atkLeft, Math.max(0, v.lossIdx));

  if (anim && rolled && t >= BA_DICE && t < BA_SHOTS + 60) {
    for (let side = 0; side < 2; side++) {
      // Normal miss (tie): both sides fire. Retreat: only the pursuer fires.
      const fires = hit ? (side === 0 ? atkWon : defWon) : (v.retreat === 0 || (side === 0) === (v.retreat === 2));
      if (!fires) continue;
      const [sx, sy] = firstUnit(side === 0);
      const [ex, ey] = hit ? target : firstUnit(side !== 0);
      const col = side === 0 ? v.atkColor : v.defColor;
      for (let kk = 0; kk < 3; kk++) {
        const dt = t - BA_DICE - kk * 60;
        if (dt < 0) continue;
        const p = dt / 240;
        if (p > 1 || (!hit && p > 0.45)) continue;
        const x = sx + (ex - sx) * p, y = sy + (ey - sy) * p + (kk - 1) * 4;
        ctx.fillStyle = col; ctx.fillRect(x - 9, y - 2, 18, 5);  // laser bolt, matches the .ino
        ctx.fillStyle = '#FFFFFF'; ctx.fillRect(x - 5, y - 1, 10, 2);
      }
    }
  }

  if (anim && hit && t >= BA_FLASH && t < BA_BOOM) {
    const q = (t - BA_FLASH) / (BA_BOOM - BA_FLASH);
    const [tx, ty] = target;
    if (q < 0.3) {
      ctx.globalAlpha = 1 - q; ctx.fillStyle = '#FFE8B0';
      ctx.beginPath(); ctx.arc(tx, ty, 16 * (1 - q / 0.3) + 4, 0, Math.PI * 2); ctx.fill();
    }
    const FRAG = ['#FFFFFF', '#FFD166', '#FF8A3D'];
    ctx.globalAlpha = 1 - q;
    for (let kk = 0; kk < 10; kk++) {
      const ang = kk * 0.628 + bNoise(kk + v.exchange * 13) * 0.5;
      const dist = q * (30 + bNoise(kk * 7 + v.exchange) * 20);
      const sz = 4 - q * 2.5;
      ctx.fillStyle = FRAG[kk % 3];
      ctx.fillRect(tx + Math.cos(ang) * dist - sz, ty + Math.sin(ang) * dist - sz, sz * 2, sz * 2);
    }
    ctx.globalAlpha = 1;
  }

  ctx.textAlign = 'center'; ctx.textBaseline = 'middle';
  for (let d = 0; d < 2; d++) {
    const isAtk = (d === 0) === atkLeft;
    const roll = isAtk ? v.atkRoll : v.defRoll;
    const mod = isAtk ? v.atkMod : v.defMod;  // faction modifier; die shows the raw face
    let face = roll - mod;
    let dx = SCENE_W / 2 + (d === 0 ? -DIE_SIZE - 8 : 8), dy = DIE_Y;
    let txt, border = '#3A5568';
    if (!rolled) txt = '?';
    else if (anim && t < BA_DICE) {
      const tick = Math.floor(t / 70);
      face = 1 + Math.floor(bNoise(tick * 3 + d * 31 + v.exchange) * 5.99);
      dy += (bNoise(tick + d * 7) - 0.5) * 8;
      txt = String(face);
    } else {
      txt = String(face);
      border = (isAtk ? atkWon : defWon) ? '#FFD166' : '#8A97A6';
    }
    ctx.fillStyle = '#081725'; ctx.fillRect(dx, dy, DIE_SIZE, DIE_SIZE);
    ctx.strokeStyle = border; ctx.lineWidth = 3; ctx.strokeRect(dx + 1.5, dy + 1.5, DIE_SIZE - 3, DIE_SIZE - 3);
    ctx.fillStyle = '#D9F5FF'; ctx.font = 'bold 22px sans-serif';
    ctx.fillText(txt, dx + DIE_SIZE / 2, dy + DIE_SIZE / 2 + 1);
    if (mod > 0 && rolled && !(anim && t < BA_DICE)) {
      ctx.fillStyle = '#FFD166'; ctx.font = 'bold 13px sans-serif';
      ctx.fillText('+' + mod, dx + DIE_SIZE / 2, dy + DIE_SIZE + 10);
    }
  }

  if (v.over && !bAnimActive) {
    const mine = atkLeft ? v.atkAlive : v.defAlive, theirs = atkLeft ? v.defAlive : v.atkAlive;
    const mineAny = mine.some(Boolean), theirsAny = theirs.some(Boolean);
    let txt, col;
    if (!v.pvp) {
      if (v.captured) { txt = 'PLANET CAPTURED'; col = '#FFD166'; }
      else if (!mineAny) { txt = 'FLEET DESTROYED'; col = '#FF6B6B'; }
      else { txt = 'WITHDREW'; col = '#8CE7FF'; }
    } else {
      if (!theirsAny) { txt = 'VICTORY'; col = '#FFD166'; }
      else if (!mineAny) { txt = 'DEFEAT'; col = '#FF6B6B'; }
      else { txt = 'WITHDREW'; col = '#8CE7FF'; }
    }
    const bx = SCENE_W / 2 - 120, by = 76;
    ctx.globalAlpha = 0.8; ctx.fillStyle = '#02060D'; ctx.fillRect(bx, by, 240, 44); ctx.globalAlpha = 1;
    ctx.strokeStyle = col; ctx.lineWidth = 2; ctx.strokeRect(bx, by, 240, 44);
    ctx.fillStyle = col; ctx.font = 'bold 20px sans-serif';
    ctx.fillText(txt, SCENE_W / 2, by + 23);
  }
}

// 3.1: a sheet belongs to the screen that opened it; if a render ends somewhere
// else (a battle started, the phase moved on), its sheet slides away.
let lockSheetSeen = false;
let boSeen = {};  // 3.1-dev.114
function buyoutPrompt(s) {
  if (!s || s.stage !== 1 || mySlot < 0 || sheetKey || battleOnScreen || (typeof ANN !== 'undefined' && ANN.open)) return;
  s.planets.forEach((p, i) => { if (sheetKey || p.owner !== mySlot || !p.bo) return; const k = i + '_' + p.bo[0] + '_' + s.round;
    if (!boSeen[k]) { boSeen[k] = 1; buzz && buzz(); openBuyout(i); } });
}
let debtsShown = -1;  // 3.1-dev.122 (user: "maybe call in your debts should be a popup before the movement of the last round? clicking the
// button is satisfying, so that part should remain"): the CALLING IN DEBTS sheet opens by itself once in the last Prepare.
function debtsPrompt(s) {
  if (!s || mySlot < 0 || sheetKey || battleOnScreen || debtsShown === s.round || !s.fh || !fhFinal(s) || s.phase !== 0 || (typeof ANN !== 'undefined' && ANN.open)) return;
  if (!(s.fh.debt || []).some((d, k) => d[0] > 0 && !((s.fh.called >> k) & 1))) return;
  debtsShown = s.round; openFhDebts();
}
function render(s) { lockSheetSeen = false; renderScreen(s); buyoutPrompt(s); debtsPrompt(s); tourAuto(s); firstTimePrompt(s); if (tourOn()) setTimeout(tourPlace, 30); if (sheetKey === 'lock' && !lockSheetSeen) { lockConfirmPending = false; sheetHide(); } pnumTick(); annPump(s); eraFrame(s); deckBarUpdate(s); }
// 3.1-dev.54 the battle card (was inline in renderScreen): fighters and WATCHERS share it; a watcher gets no actions.
// Build it ONCE per battle (v0.20.0) -- rebuilding it every 700ms poll would restart the canvas mid-animation and drop taps
// (Convention 8). bUpdate() then only touches text and, when their state changes, the buttons.
let bSpectated = '';
function battleCard(s, app, html, spect) {
    battleOnScreen = true;  // 3.1-dev.74
    const key = [s.battleKind, s.battleAttackerSlot, s.battleDefenderSlot, s.battleTargetPlanet, mySlot].join('|');
    if (spect) bSpectated = key;
    if (key !== bKey || !document.getElementById('bCanvas')) {
      bKey = key;
      victoryKeyDone = '';  // a new battle gets its own report
      bBtnSnap = '';
      html += '<div class="bfull"><p id="bTitle"></p>' +  // UI 3.0 step 1: the battle takes the whole screen
              '<canvas id="bCanvas" onclick="bSkip()"></canvas>' +
              '<p id="bText"></p><div id="bBtns"></div></div>';
      app.innerHTML = html; shownHtml = null;  // direct write (see setApp)
      bView = bViewFrom(s);   // joining mid-battle: show it, don't replay
      bAnimActive = false;
      bWithdrawArmedAt = DISARMED;
      const gen = ++bGen;
      requestAnimationFrame(t => bFrame(t, gen));
    }
    bUpdate(s);
}
// ATTACK ALERT + WATCH (user 2026-11-12: "an alert/spectate button for when your planet is getting attacked")
let watchBattle = '';
const BATTLE_SEEN = {};
function battleKeyOf(s) { return [s.battleKind, s.battleAttackerSlot, s.battleDefenderSlot, s.battleTargetPlanet, s.round].join('|'); }
function watchingBattle(s) { return s.battleActive && watchBattle !== '' && watchBattle === battleKeyOf(s); }
function battleAlert(s) {
  if (!s.battleActive || s.battleAttackerSlot === mySlot || s.battleDefenderSlot === mySlot) return;
  const key = battleKeyOf(s); if (BATTLE_SEEN[key]) return;
  BATTLE_SEEN[key] = 1;
  const pvp = s.battleKind === 2, P = s.planets[s.battleTargetPlanet], mine = !pvp && P && P.owner === mySlot && mySlot >= 0;
  const head = pTag(s, s.battleAttackerSlot, true) + ' ATTACKS ' + (pvp ? pTag(s, s.battleDefenderSlot, true) : (P ? P.name : 'A WORLD'));
  notify(head, (mine ? '<b style="color:#FF6B5A">Your world!</b> ' : pvp ? 'A fleet battle. ' : '') + 'Tap to watch.', mine ? '#FF6B5A' : '#FFB347', mine ? 12000 : 8000, false, () => { watchBattle = key; shownHtml = null; render(lastS); });
  if (mine) buzz([200, 100, 200]);
}
function stopWatchBattle() { watchBattle = ''; shownHtml = null; render(lastS); }
let spectating = false;
function spectatorHtml(s) {
  let h = '<div class="joinhead" style="margin-top:8px"><b>SHADOW</b><b style="color:#FFC933">FLEET</b></div><p class="joinsub">WATCHING \u00b7 ROUND ' + s.round + ' OF ' + s.roundLimit + ' \u00b7 ' + (s.phaseName || '') + '</p>';
  const now = [];
  if (s.battleActive) now.push('<button class="gyrow bad" onclick="watchBattle=battleKeyOf(lastS);render(lastS)">' + pixIcon('war', '#FF6B5A', 2) + '<span><b>BATTLE</b> ' + pTag(s, s.battleAttackerSlot) + ' vs ' +
                                (s.battleKind === 2 ? pTag(s, s.battleDefenderSlot) : (s.planets[s.battleTargetPlanet] || {}).name) + '</span><em>WATCH</em></button>');
  (s.talks || []).forEach(tk => now.push('<button class="gyrow" onclick="openWatch(' + tk.by + ')">' + pixIcon('holonet', '#5FD4FF', 2) + '<span><b>TALKS</b> ' + pTag(s, tk.by) + ' on ' + wName(s, tk.p) + '</span><em>WATCH</em></button>'));
  h += '<div class="gy"><div class="hgtitle"><span>HAPPENING NOW</span></div>' + (now.length ? now.join('') : '<p class="ghint">Nothing right now. Fights and talks show up here.</p>') + '</div>';
  h += '<div style="font-size:16px;font-weight:700;letter-spacing:4px;margin:14px 0 6px">STANDINGS</div>' + standingsRowsHtml(s);
  const hist = (FEED.pub || []).slice(-8).reverse().map(e => { const x = feedText(s, e, false); return x ? '<div class="watchR"><b style="color:' + (x.col || 'var(--text)') + '">' + x.head + '</b> \u00b7 ' + x.line + '</div>' : ''; }).join('');
  if (hist) h += '<div class="fhgrp">LATELY</div>' + hist;
  h += '<button class="back" onclick="spectating=false;lastSeatSnapshot=\'\';render(lastS)">\u2039 Back</button>';
  return h;
}
function renderScreen(s) {
  battleOnScreen = false;  // 3.1-dev.74: battleCard sets it again while a battle is on screen
  if (s && s.v2 && joined) v2Watch(s);  // 2.0
  const app = document.getElementById('app');
  document.getElementById('fw').textContent = s.firmwareVersion;
  if (primerAt >= 0) { setApp(app, primerHtml()); return; }  // 2.0-dev.14 HOW TO PLAY

  // Not joined yet -> pick a seat directly, no typed name anymore (your
  // name becomes your faction the moment you pick one). Only rebuild this
  // when the set of active players actually changes -- rebuilding on
  // every 700ms poll tick regardless (the old behavior) risks tearing out
  // and recreating the DOM mid-tap, the same bug class that broke the
  // name-input field earlier in this project.
  if ((!joined || mySlot < 0) && spectating && s.stage === 1) {  // 3.1-dev.54 SPECTATOR MODE
    if (watchingBattle(s)) { battleCard(s, app, '', true); return; }
    setApp(app, spectatorHtml(s)); return;
  }
  if (!joined || mySlot < 0) {
    // Stage is part of the snapshot so the picker rebuilds the moment the
    // game starts. Once it has, the server refuses every new join (by
    // design) -- so don't offer seats that can't be taken (v0.19.6).
    const seatSnapshot = s.stage + '|' + s.players.map(p => p.active ? '1:'+p.name+':'+p.chosenFaction+':'+p.away : '0').join(',');
    if (seatSnapshot !== lastSeatSnapshot || !document.getElementById('seatPickerBox')) {
      lastSeatSnapshot = seatSnapshot;
      // v0.21.18 restyle (twin of the AMOLED's showStart/showSlotPicker).
      let html = '<div id="seatPickerBox"><div class="joinhead"><b>SHADOW</b><b style="color:#FFC933">FLEET</b></div>';
      if (s.stage !== 0) {
        // 2.0-dev.13 (board O): mid-game, an AWAY seat can be taken back.
        const away = s.stage === 1 && s.players.some(p => p.active && p.away);
        html += '<p class="joinsub">GAME IN PROGRESS</p><p class="joinhint">'+(away ? 'Coming back? Take your seat.' : 'New players can only join from the Lobby.')+'</p>';
        if (s.stage === 1) html += '<button class="pill" style="border-color:#5FD4FF;color:#5FD4FF" onclick="spectating=true;lastSeatSnapshot=\'\';render(lastS)">WATCH THE GAME<small>See the standings, the news and every fight</small></button>';  // 3.1-dev.54
        if (away) {
          html += '<div id="joinDebug" style="color:#FF9E9E;font-size:13px;min-height:18px;text-align:center"></div><div class="seatgrid">';
          s.players.forEach((p, i) => {
            if (!p.active) return;
            const f = p.chosenFaction, nm = f >= 0 ? FACTION_SHORT_NAMES[f] : 'PLAYER';
            html += p.away
              ? '<button class="seat" style="border-color:'+(f >= 0 ? FACTION_UI_COLORS[f] : 'var(--line)')+';border-style:dashed" onclick="pickSlot('+i+')">' +
                (f >= 0 ? insigniaSvg(f, 30) : '') + '<small>P'+(i+1)+' \u00B7 '+nm+' \u00B7 AWAY</small><small style="color:var(--accent)">Tap to take this seat back</small></button>'
              : '<button class="seat taken" disabled>' + (f >= 0 ? insigniaSvg(f, 30) : '') + '<small>P'+(i+1)+' \u00B7 '+nm+'</small><small>PLAYING</small></button>';
          });
          html += '</div>';
        }
      } else {
        html += '<p class="joinsub">CHOOSE YOUR SEAT</p>' +
                '<div id="joinDebug" style="color:#FF9E9E;font-size:13px;min-height:18px;text-align:center"></div><div class="seatgrid">';
        for (let i=0;i<s.players.length;i++){
          const p = s.players[i];
          if (p.active) {
            const f = p.chosenFaction;
            html += '<button class="seat taken" disabled>' + (f >= 0 ? insigniaSvg(f, 30) : '<span class="q">?</span>') +
                    '<small>PLAYER '+(i+1)+'</small><small style="color:'+(f >= 0 ? FACTION_UI_COLORS[f] : 'var(--faint)')+'">' +
                    (f >= 0 ? ['REBELS','EMPIRE','PYKES','CRIMSON DAWN'][f] : 'JOINING') + '</small></button>';
          } else {
            html += '<button class="seat" onclick="pickSlot('+i+')"><small>PLAYER</small><b>'+(i+1)+'</b><small>OPEN</small></button>';
          }
        }
        html += '</div>';
      }
      if (s.stage === 0) html += firstTogHtml();  // 3.1-dev.81
      if (s.net && s.net.canHome && !s.net.home)  // 3.1-dev.96: also here, before anyone sits down
        html += '<button class="pill quiet" onclick="location.href=\x27/setup\x27">CONNECT THE TABLE TO HOME WI-FI<small>Phones then play on your own Wi-Fi</small></button>';
      html += '<button class="back" style="margin-top:4px" onclick="openPrimer()">NEW HERE? HOW TO PLAY</button>';  // 2.0-dev.14
      html += '</div>';
      app.innerHTML = html; shownHtml = null;  // direct write (see setApp)
    }
    return;
  }

  const me = s.players[mySlot];

  // 3.1-dev.6: ONE character screen replaces the faction + commander steps.
  // A character sets your faction and class. charEdit = back here from the Lobby.
  if (me.chosenFaction < 0 || me.commander < 0 || (charEdit && s.stage === 0)) {
    setApp(app, charSelectHtml(s));
    return;
  }

  let html = '';
  if (s.alert) html += '<div class="alert">' + s.alert + '</div>';

  // Lobby (v0.21.19 restyle, twin of the AMOLED's showLobby): player circles,
  // GAME LENGTH and START -- any seated player can start once everyone has a
  // faction (was AMOLED-only). No planet picker: all 12 are always in play.
  if (s.stage === 0) {
    html += '<div class="joinsub" style="margin-top:22px;font-size:22px;letter-spacing:5px">LOBBY</div><div class="lobbyrow">';
    let waitingOn = -1, seated = 0, waitCmd = false;
    for (let i=0;i<s.players.length;i++){
      const p = s.players[i];
      const f = p.active ? p.chosenFaction : -1;
      if (p.active) seated++;
      if (p.active && (f < 0 || p.commander < 0) && waitingOn < 0) { waitingOn = i; waitCmd = f >= 0; }
      const nm = !p.active ? 'P'+(i+1)+' OPEN' : i === mySlot ? 'P'+(i+1)+' · YOU' : f >= 0 ? 'P'+(i+1)+' '+['REBELS','EMPIRE','PYKES','CRIMSON'][f] : 'P'+(i+1)+' CHOOSING';
      html += '<div><div class="lp" style="'+(f >= 0 ? 'border:2px solid '+FACTION_RING_COLORS[f] : '')+'">' +
              (p.commander >= 0 && ART ? '<img class="pixel" src="'+portraitURL(p.commander, 2)+'" alt="">' : f >= 0 ? insigniaSvg(f, 34) : p.active ? '<span class="q">?</span>' : '') + '</div>' +
              '<small style="color:'+(f >= 0 ? FACTION_UI_COLORS[f] : 'var(--faint)')+'">'+nm+'</small></div>';
    }
    html += '</div><p class="joinhint" style="font-size:12px;letter-spacing:3px">GAME LENGTH</p><div class="lenrow">';
    [6,8,10].forEach(n => {
      html += '<button class="pill'+(s.roundLimit === n ? '' : ' quiet')+'" style="'+(s.roundLimit === n ? 'border-color:var(--cyan);color:var(--cyan)' : '')+'" onclick="setRoundLimit('+n+')">'+n+'</button>';
    });
    // 3.1-dev.96: the markets / galaxy-event switches are gone -- markets always on, the halfway event always off.
    const ready = waitingOn < 0 && seated > 0;
    html += '</div><div style="height:14px"></div>' +
            (ready ? '<button class="pill" style="border-color:#FFC933;color:#FFC933" onclick="startGame()">START</button>'
                   : '<button class="pill quiet" disabled>START</button>');
    html += '<p class="joinhint"'+(ready ? '' : ' style="color:#FF9B70"')+'>' +
            (ready ? seated+' commander'+(seated === 1 ? '' : 's')+' ready · anyone can start' : 'Waiting for Player '+(waitingOn+1)+' to pick a '+(waitCmd ? 'commander' : 'faction')) + '</p>';
    // v0.22.5: the Table Console can join your home Wi-Fi -- offered while it's on its own network.
    if (s.net && s.net.canHome && !s.net.home)
      html += '<button class="pill quiet" onclick="location.href=\x27/setup\x27">CONNECT THE TABLE TO HOME WI-FI<small>Phones then play on your own Wi-Fi</small></button>';
    html += '<button class="pill quiet" onclick="openSettings()">SETTINGS<small>Rivet, buzz, motion, the table\u2019s Wi-Fi</small></button>';  // 3.1-dev.61
    html += '<button class="pill quiet" onclick="openPrimer()">HOW TO PLAY<small>Eight quick cards</small></button>';  // 2.0-dev.14
    html += '<button class="back" onclick="charEdit = true; charPick = -1; poll()">CHANGE CHARACTER</button>';  // 3.1-dev.6
    setApp(app, html);
    return;
  }

  // Game over.
  if (s.stage === 2) {
    if (s.fh && s.fh.log) { fhPlayEnsure(s); fhSync(s); }  // 3.1-dev.43: the finale plays first, once (3.1-dev.117: in step with the table)
    // v0.25.1 restyle (twin of the AMOLED's showGameOver): the winner's face
    // in their color, YOUR result big (VICTORY in gold if it's you), then the
    // final standings rows.
    const w = s.winner >= 0 ? s.players[s.winner] : null;
    const wf = w ? w.chosenFaction : -1;
    if (me && me.chosenFaction >= 0) document.documentElement.style.setProperty('--accent', FACTION_UI_COLORS[me.chosenFaction]);
    html += '<div class="vic"><span class="k">GAME OVER · '+s.roundLimit+' ROUNDS</span>';
    if (w && wf >= 0) {
      const face = (w.commander >= 0 && ART) ? '<img class="pixel" src="'+portraitURL(w.commander, 4)+'" alt="">' : insigniaSvg(wf, 48);
      html += '<div class="lp" style="width:96px;height:96px;border:3px solid '+FACTION_RING_COLORS[wf]+'">'+face+'</div>';
      html += s.winner === mySlot
        ? '<div class="badge" style="border-color:#FFC933;color:#FFC933">VICTORY</div>'
        : '<b style="color:'+FACTION_UI_COLORS[wf]+'">'+FACTION_SHORT_NAMES[wf]+((wf === 1 || wf === 3) ? ' WINS' : ' WIN')+'</b>';
    } else {
      html += '<div class="badge" style="border-color:#FFC933;color:#FFC933">DRAW</div>';
    }
    html += '</div>' + standingsRowsHtml(s);
    if (s.v2) {  // 3.1-dev.15: the CLASS quests, then your story
      let aw = '';
      s.players.forEach((p, i) => { if (p.active && p.commander >= 0) aw += '<div style="color:'+(s.v2.questDone[i] ? '#8CE7A0' : 'var(--faint)')+'">'+feedWho(s, i)+': '+CLASSES[p.commander % 4].quest.split(':')[0]+(s.v2.questDone[i] ? ' \u2714 +5' : ' missed')+'</div>'; });
      html += '<div class="card" style="text-align:left;font-size:14px"><div class="storyK">QUESTS</div>'+aw+'</div>';
      html += storyHtml(s);
    }
    const armed = Date.now() - newGameArmed < 4000;
    html += '<button class="pill'+(armed ? '' : ' quiet')+'" style="margin-top:14px'+(armed ? ';border-color:#FF6B5A;color:#FF6B5A' : '')+'" onclick="newGameTap()">'+(armed ? 'TAP AGAIN TO START OVER' : 'NEW GAME')+'<small>Everyone goes back to the Lobby</small></button>';
    setApp(app, html);
    return;
  }

  // Playing. Slim status bar on every in-game screen (v0.21.0); the player's
  // faction color becomes the page accent.
  if (me.chosenFaction >= 0) document.documentElement.style.setProperty('--accent', FACTION_UI_COLORS[me.chosenFaction]);
  html += '<div class="topbar"><span class="topL">'+(me.commander >= 0 && ART ? '<button class="meAv" onclick="openCard()" aria-label="Your card"><img class="pixel" src="'+portraitURL(me.commander, 2)+'" alt=""></button>' : '')+'<span>ROUND '+s.round+' / '+s.roundLimit+'</span></span><b>'+s.phaseName+'</b><span class="topR">'+gearBtn()+bellHtml()+'<span>'+me.credits+' CR</span></span></div>';
  // 2.0-dev.13 (board O): you stepped away -- the game goes on; COME BACK.
  if (me.away === 2) {
    html += '<div class="card" style="margin-top:40px"><div style="font-size:22px;letter-spacing:4px">YOU STEPPED AWAY</div>' +
            '<p style="color:var(--dim)">The game goes on without you: you count as locked in, and your worlds, fleets and Credits hold. Come back any time.</p></div>' +
            '<button class="pill" style="border-color:var(--accent);color:var(--accent)" onclick="comeBack()">COME BACK<small>Back into round '+s.round+'</small></button>';
    setApp(app, html);
    return;
  }
  // Back from AWAY: what happened while you were gone, once.
  if (s.welcome) {
    const w = s.welcome, lines = [];
    const gone = s.round - w.round;
    lines.push(gone > 0 ? 'You left in round '+w.round+'. It\'s round '+s.round+' now.' : 'You were only gone a moment: still round '+s.round+'.');
    if (me.vp !== w.vp) lines.push('VP '+w.vp+' \u2192 '+me.vp);
    if (me.credits !== w.cr) lines.push('Credits '+w.cr+' \u2192 '+me.credits);
    const lost = [], won = [];
    s.planets.forEach((p, i) => { const had = (w.worlds >>> i) & 1, has = p.owner === mySlot; if (had && !has) lost.push(p.name); if (!had && has) won.push(p.name); });
    if (lost.length) lines.push('<span style="color:var(--red)">Lost: '+lost.join(', ')+'</span>');
    if (won.length) lines.push('<span style="color:#8CE7A0">Yours now: '+won.join(', ')+'</span>');
    if (lines.length === 1) lines.push('Your worlds held.');
    const wm = s.players[mySlot];  // 3.1-dev.15: your face and the Feed
    html += '<div class="card" style="margin-top:28px;text-align:left">' + (wm && wm.commander >= 0 && ART ? '<div style="text-align:center"><img class="pixel" style="width:72px;height:84px;border-radius:12px" src="'+portraitURL(wm.commander, 3)+'" alt=""><div class="storyK" style="margin-top:4px">'+charName(wm.commander).toUpperCase()+'</div></div>' : '') +
            '<div style="font-size:22px;letter-spacing:4px;text-align:center">WELCOME BACK</div>' +
            '<div style="font-size:12px;letter-spacing:3px;color:var(--accent);margin:10px 0 6px">WHILE YOU WERE AWAY</div>' +
            lines.map(l => '<div style="font-size:15px;margin:3px 0">'+l+'</div>').join('') + '</div>' +
            (feedUnread() ? '<p style="font-size:13px;color:var(--dim);text-align:center">'+feedUnread()+' new in your Feed (the bell, top right).</p>' : '') +
            '<button class="pill" style="border-color:var(--accent);color:var(--accent)" onclick="welcomeAck()">BACK TO THE WAR</button>';
    setApp(app, html);
    return;
  }
  // v0.28.3 (playtest 2026-10-19): everyone else has locked in -- say so, buzz once per phase.
  {
    const act = s.players.filter(p => p.active), others = act.filter((p, i) => p !== me);
    const last = act.length > 1 && !me.lockedIn && !s.battleActive && others.every(p => p.lockedIn);
    if (last) {
      html += '<div class="alert" style="text-align:center;letter-spacing:2px;font-weight:700">EVERYONE IS WAITING ON YOU</div>';
      const key = s.round * 4 + s.phase;
      if (lastWaitBuzz !== key) { lastWaitBuzz = key; buzz([120, 80, 120]); }
    }
  }

  // Battle report (v0.21.13; twin of the AMOLED's showVictory()). Opens on
  // CONTINUE at the end of a battle, or by itself when the other commander
  // closed a PvP battle first. Any transmission the battle caused waits
  // until it's dismissed (see poll()).
  if (!victory && bView && bView.over && bKey && bKey !== victoryKeyDone && bKey !== bSpectated &&
      !(s.battleActive && (s.battleAttackerSlot === mySlot || s.battleDefenderSlot === mySlot))) {
    victory = { v: bView };
    victoryKeyDone = bKey;
  }
  // v0.29.0: remember your standing when a battle starts, so its report can show the arrows.
  if (s.battleActive && (s.battleAttackerSlot === mySlot || s.battleDefenderSlot === mySlot) && !repBattleBase)
    repBattleBase = s.reputation.map(r => r.score || 0);
  if (!s.battleActive && !victory) repBattleBase = null;
  if (purchased) {  // v0.29.0 purchase card
    const pi = purchased.p;
    if (!galaxyImgs[pi]) galaxyImgs[pi] = bPlanetPixels(pi, pi * 37).toDataURL();
    html += '<div class="vic"><span class="k">PURCHASE</span><div class="art"><img class="pixel" src="'+galaxyImgs[pi]+'" width="120" height="120" alt=""></div>' +
            '<b style="color:var(--accent)">'+s.planets[pi].name+' IS YOURS</b><span class="dim">+'+s.planets[pi].vp+' VP while you hold it</span>' + repLinesHtml(purchased.base, s) +
            '<button class="pill" onclick="closePurchased()">CONTINUE</button></div>';
    setApp(app, html);
    return;
  }
  if (victory) {
    const v = victory.v, o = battleOutcome(v);
    const tone = o.result === 1 ? 'var(--accent)' : (o.result === 0 ? 'var(--red)' : 'var(--dim)');
    const pIdx = v.planetIdx, planetOk = !v.pvp && pIdx >= 0 && pIdx < s.planets.length;
    const pname = planetOk ? s.planets[pIdx].name : '';
    const n = o.myShips, ships = n + ' ship' + (n === 1 ? '' : 's');
    let l1, l2;
    if (!v.pvp) {
      if (o.result === 1) { l1 = pname + ' IS YOURS'; l2 = '+' + s.planets[pIdx].vp + ' VP while you hold it'; }
      else if (o.result === 2) { l1 = 'You pulled back from ' + pname; l2 = ships + ' made it out'; }
      else { l1 = pname + ' HOLDS'; l2 = 'Fleet lost · rebuild it in Prepare'; }
    } else {
      const iA = v.leftIsAttacker, fl = m => { const k = [0, 1, 2].filter(i => (m >> i) & 1).map(i => i + 1); return (k.length > 1 ? 'FLEETS ' + k.join(' + ') : 'FLEET ' + (k[0] || 1)); };  // 3.1-dev.44
      const theirs = pTag(s, iA ? v.defSlot : v.atkSlot, true) + '\u2019S ' + fl(iA ? v.defFleets : v.atkFleets), mine = 'YOUR ' + fl(iA ? v.atkFleets : v.defFleets);
      if (o.result === 1 && o.decisive) { l1 = theirs + ' DESTROYED'; l2 = '+2 VP'; }
      else if (o.result === 1) { l1 = 'THE ENEMY WITHDREW'; l2 = ships + ' still with you'; }
      else if (o.result === 2) { l1 = 'You withdrew'; l2 = ships + ' made it out'; }
      else if (o.decisive) { l1 = mine + ' WAS DESTROYED'; l2 = 'Rebuild it in Prepare'; }
      else { l1 = 'THEY HELD'; l2 = ships + ' still with you'; }
    }
    let art;
    if (planetOk) {
      if (!galaxyImgs[pIdx]) galaxyImgs[pIdx] = bPlanetPixels(pIdx, pIdx * 37).toDataURL();
      art = '<img class="pixel" src="'+galaxyImgs[pIdx]+'" width="140" height="140" alt="">';
    } else {
      const mine = v.leftIsAttacker ? v.atkColor : v.defColor, theirs = v.leftIsAttacker ? v.defColor : v.atkColor;
      art = (o.myShips ? fcShips(o.myShips, mine) : '<span class="dim">NO SHIPS LEFT</span>') + '<span class="dim">VS</span>' +
            (o.theirShips ? fcShips(o.theirShips, theirs, true) : '<span class="dim">ENEMY FLEET GONE</span>');
    }
    html += '<div class="vic"><span class="k">BATTLE REPORT</span>' +
            '<div class="badge" style="border-color:'+tone+';color:'+tone+'">'+(o.result === 1 ? 'VICTORY' : o.result === 0 ? 'DEFEAT' : 'WITHDREW')+'</div>' +
            '<div class="art">'+art+'</div><b style="color:'+(o.result === 2 ? 'var(--text)' : tone)+'">'+l1+'</b><span class="dim">'+l2+'</span>';
    html += repLinesHtml(repBattleBase, s);  // v0.29.0
    if (s.transmissionPending) {
      const tf = s.transmissionFaction;
      html += '<div class="txchip">INCOMING TRANSMISSION'+(tf >= 0 && tf < 4 ? ' · '+FACTION_SHORT_NAMES[tf] : '')+'</div>';
    }
    html += '<button class="pill" style="border-color:'+(o.result === 2 ? 'var(--cyan)' : tone)+';color:'+(o.result === 2 ? 'var(--cyan)' : tone)+'" onclick="closeVictory()">CONTINUE</button></div>';
    setApp(app, html);
    return;
  }

  // Active battle involving me (attacker OR defender, planet OR pvp) -- or one I chose to WATCH (3.1-dev.54).
  const inBattle = s.battleActive && (s.battleAttackerSlot === mySlot || s.battleDefenderSlot === mySlot);
  if (inBattle || watchingBattle(s)) { battleCard(s, app, html, !inBattle); return; }

  // Takeover session (mine).
  let sessionOnPlanetPage = false;
  if (s.takeoverPlayer === mySlot) {
    const pl = s.planets[s.takeoverPlanet];
    // v0.21.16 restyle (twins of the AMOLED's showNegotiate / askLeaveNegotiationCb).
    // The faction is named only once you've revealed the planet's Affiliation.
    if (s.takeoverMethod === 2 && s.neg) {
      // v0.30.0 THE CONVERSATION (twin of the AMOLED's showNegotiate): the
      // speaker's hologram (an UNKNOWN AMBASSADOR in scrambled gray until you've
      // revealed Affiliation), a pip per question, then the question and its
      // replies -- or the favors -- or the result card.
      const n = s.neg, f = n.faction, known = f >= 0, np = s.planets[n.planet];
      const face = known ? portraitURL(commanderVoice(f, me.commander), 4, 1) : (n.planet % 5 === 2 || n.planet % 5 === 4) ? hoodedURL(n.planet) : portraitURL(n.planet % 16, 4, 2);  // 3.1-dev.51 (user): a hooded figure here and there
      const who = (n.up ? 'UPRISING \u00b7 ' : '') + (known ? TX_FACTIONS[f].name : 'UNKNOWN AMBASSADOR');  // 3.1-dev.79
      const pips = '<div class="negpips">' + n.marks.map((m, k) => '<i class="m'+m+(n.result === 0 && k === n.step ? ' cur' : '')+'"></i>').join('') + '</div>';
      html += '<div class="neghead">'+(face ? '<img class="pixel negface" src="'+face+'" width="96" height="112" alt="">' : '') +
              '<b>'+np.name+'</b><span style="color:'+(known ? FACTION_UI_COLORS[f] : 'var(--dim)')+'">'+who+'</span>' +
              (known ? '<button class="negWho" onclick="openFactionGuide(' + f + ')">WHO ARE THEY?</button>' : '<span class="neghint">Hold a world next to it to learn whose side it\x27s on. <a onclick="openFactionGuide(-1)" style="color:var(--cyan)">The factions</a></span>') + pips + '</div>' +
              (n.hs ? '<div class="headStart"><b>HEAD START</b><span>They remember the deal that fell through. Question 1 counts as answered.</span></div>' : '');
      const said = np.name.toLowerCase().replace(/(^|\s)\S/g, c => c.toUpperCase());  // spoken, like the C++ {planet}
      const left = n.roundsLeft === 0 ? 'THIS IS THE LAST ROUND' : n.roundsLeft + ' ROUND' + (n.roundsLeft === 1 ? '' : 'S') + ' LEFT';
      if (n.result === 5) {
        // v0.30.1 HOSTILE: they won't talk (you knew who they were), or the
        // ambassador unmasks at the end. FIGHT is the one allowed switch of approach.
        html += n.count ? '<div class="vic"><b style="color:var(--red)">WE KNOW WHO YOU ARE</b>'
                        : '<div class="vic"><b style="color:var(--red)">THEY WON\x27T TALK TO YOU</b>';
        html += '<p class="negq">“'+n.line+'”</p>';
        html += '<span class="dim">'+(n.count ? 'Sympathy revealed. No standing lost. ' : '')+'They won\x27t negotiate this round. You can still fight for '+np.name+'.</span>';
        html += '<button class="pill" style="border-color:var(--red);color:var(--red)" onclick="negFight()">FIGHT</button>' +
                '<button class="pill" style="border-color:var(--line);color:var(--dim)" onclick="negClose()">WALK AWAY</button></div>';
      } else if (n.result !== 0) {
        const won = n.result === 1 || n.result === 2;
        if (won) {
          if (!galaxyImgs[n.planet]) galaxyImgs[n.planet] = bPlanetPixels(n.planet, n.planet * 37).toDataURL();
          html += '<div class="vic"><div class="art"><img class="pixel" src="'+galaxyImgs[n.planet]+'" width="96" height="96" alt=""></div>' +
                  '<b style="color:var(--accent)">'+np.name+' IS YOURS</b>';
          if (n.result === 1) html += '<span class="dim">Every answer landed. No strings attached.</span><span class="dim">+'+n.vp+' VP while you hold it</span>';
          else if (n.paid) html += '<span class="dim">Paid in full.</span><span class="dim">+'+n.vp+' VP while you hold it</span>';
          else html += '<span class="dim">You owe them:</span><span class="negowe">'+n.owe+'</span>' +
                       '<span class="dim">+'+n.vp+' VP now. Keep your word and they warm to you; break it and they take '+np.name+' back.</span>';
        } else {
          html += '<div class="vic"><b style="color:var(--red)">'+(n.result === 3 ? 'THEY CUT YOU OFF' : 'THEY END THE CALL')+'</b>' +
                  '<span class="dim">'+(n.result === 3 ? 'That\x27s twice you insulted them. ' : '')+np.name+' is closed to you this round.</span>' +
                  '<span class="dim" style="color:var(--red)">'+(n.losses >= 2 ? 'They\x27ll never negotiate with you here again.' : 'Lose here once more and they\x27ll never talk to you here again.')+'</span>';
        }
        html += repLinesHtml(negBase, s) + '<button class="pill" onclick="negClose()">CONTINUE</button></div>';
      } else if (leaveNegPending) {
        html += '<p class="negmsg"><b style="color:var(--red)">LEAVE?</b><br>Walking out counts as a loss here, and they won\x27t forget the snub. You can\x27t try another approach on '+np.name+' this round.</p>';
        html += '<button class="pill" style="border-color:var(--red);color:var(--red)" onclick="cancelTakeover()">LEAVE</button>';
        html += '<button class="pill" onclick="stayInNegotiation()">STAY</button>';
      } else if (n.q) {
        html += '<div class="negtier"'+(n.own && known ? ' style="color:'+FACTION_UI_COLORS[f]+'"' : '')+'>'+(n.own ? 'THEIR OWN QUESTION' : ['EASY', 'HARDER', 'HARD'][n.tier])+' · QUESTION '+(n.step + 1)+' OF '+n.count+'</div>';
        html += '<p class="negq">'+n.q+'</p>';
        html += rivetAsk('negq' + n.planet + '_' + s.round, 'They ask ' + n.count + ' questions, easy first. Answer the way ' + (known ? 'the ' + FNAMES[f] : 'they') + ' would. Land every one and ' + said + ' is yours, no strings. Miss one and they offer favors to pick from; miss two and you get one, take it or leave it. Insult them twice and they hang up. Walking out counts as a loss, and two losses here close ' + said + ' to you for good.' + (known ? '' : ' You don\u2019t know who\u2019s on the other end. Scout ' + said + '\u2019s Sympathy next time and you will.'));  // 3.1-dev.70
        if (n.tipped) html += '<div class="negtip">' + ((n.gv || 0) && !n.contact ? 'A friend\u2019s word steers you away from one answer' : 'Your contact steers you away from one answer') + '</div>';
        if (n.gw >= 0) html += '<div class="galNote bad">' + pTag(s, n.gw) + ' whispered against you: ' + (n.step + 1 < n.count ? 'your next question is harder.' : 'this one got harder.') + '</div>';  // 3.1-dev.52
        for (let o = 0; o < 4; o++) if (((n.gv || 0) >> o) & 1) html += '<div class="galNote good">' + pTag(s, o) + ' vouched for you: a wrong answer is gone.</div>';
        html += n.replies.map(r => '<button class="negr" onclick="negAnswer('+r.i+')">'+r.text+'</button>').join('');
        html += '<button class="pill" style="border-color:var(--line);color:var(--dim)" onclick="askLeaveNegotiation()">LEAVE</button>';
      } else if (n.favorStage) {
        html += '<div class="negtier" style="color:#FF9B70">'+(n.hard ? 'THEY HAVE ONE CONDITION' : 'THEY\x27RE NOT CONVINCED YET')+'</div>' +
                rivetAsk('negfav' + n.planet + '_' + s.round, 'Pick a favor and ' + said + ', and its VP, are yours right now. It\u2019s a debt: do it in time and they like you more; break it or run out of time and they take ' + said + ' back and hold a grudge. A Credits favor is paid on the spot and done. NOTHING MORE walks away with nothing, and counts as a loss.') +
                '<div class="debtnote">'+pixIcon('owe', '#FFD166', 2)+'<span>Take a favor and the world <b>and its VP are yours now</b>. It\x27s a debt: keep your word and they warm to you, break it and they take the world back.</span></div>';
        html += '<p class="negq">'+(n.hard ? 'Then this is our price for '+said+'. Take it or leave it.' : 'Words are cheap, commander. What will you do for '+said+'?')+'</p>';
        if (n.late) html += '<div class="negtip" style="color:#FFD166">THE WAR IS ALMOST OVER · '+left+'</div>';
        if (s.takeoverResult) html += '<div class="negtip" style="color:var(--red)">'+s.takeoverResult+'</div>';
        const tag = x => x.credits ? 'PAID ON THE SPOT · BUYING IT WOULD HAVE BEEN '+n.payPrice+' CR'
                       : n.late ? 'DUE WHEN THE WAR ENDS · '+left+' · UNDONE = THEY TAKE IT BACK'
                       : 'YOU\x27LL OWE THIS · DUE IN 3 ROUNDS';
        html += n.favors.map((x, i) => '<button class="negr fav"'+(x.credits ? ' style="border-color:#FFD166"' : '')+' onclick="negAnswer('+i+')"><small'+(x.credits ? ' style="color:#FFD166"' : '')+'>'+tag(x)+'</small>'+x.text+'</button>').join('');
        html += '<button class="negr" style="color:var(--dim);border-color:var(--line)" onclick="negAnswer(3)">Nothing more. Take my word or leave it.</button>';
      }
    } else if (briefingPending) {
      // Battle briefing (v0.20.1) -- phone twin of the AMOLED's showBriefing().
      // Only intel you already hold. Rebuilt only when something it shows
      // changes, since it contains a fleet picker (Convention 8).
      const fleets = me.fleets.map(f => f.alive);
      // Ready = has ships and hasn't already fought this round (v0.20.2).
      const ready = me.fleets.map(f => f.alive > 0 && !f.spent);
      if (briefFleet < 0) { const i = ready.indexOf(true); briefFleet = i >= 0 ? 1 << i : 0; }
      ready.forEach((r, f) => { if (!r) briefFleet &= ~(1 << f); });
      const sent = fleets.reduce((n, a, f) => n + ((briefFleet & (1 << f)) ? a : 0), 0);
      const fm = s.fightMods;
      const snap = [s.takeoverPlanet, fleets.join(','), ready.join(','), briefFleet, fm.atk, fm.atkSrc, fm.def, fm.defSrc, fm.esc, fm.mil, fm.merc, fm.gLo, fm.gHi, me.credits, briefMercs,
                    pl.defense, pl.chars.map(c => c.revealed ? c.value : '').join('|'),
                    s.takeoverResult].join(';');
      if (snap === briefSnap && document.getElementById('briefCard')) return;
      briefSnap = snap;
      // v0.21.7: restyled as the Battle Forecast (twin of the AMOLED's
      // showBriefing()) -- two columns, exact odds, fleet picker, ENGAGE.
      // v0.21.9 redesign (user: "lots of words"): pictures instead of
      // sentences -- ship triangles vs garrison squares / strength meter /
      // ???, a big die per side with a +N badge, the faction behind the
      // bonus as its insignia, big WIN / LOSE numbers over a thick bar.
      const gUnits = '<span class="unk" style="font-size:12px;letter-spacing:2px">' + kw('DEFENDERS') + '</span>' +  // v0.21.11: size never shown
                     '<span class="unk" style="font-size:12px;display:block;margin-top:4px">'+(fm.gLo === fm.gHi ? 'about '+fm.gLo : 'usually '+fm.gLo+'\u2013'+fm.gHi)+'</span>';  // dev.27: only what you can't rule out
      let b = '<div id="briefCard"><div class="fchead"><span>FORECAST</span><b>'+pl.name+'</b></div>';
      if (s.takeoverResult) b += '<p class="alert">'+s.takeoverResult+'</p>';
      // 2.0-dev.24: faction help is a SHIP -- the escort in its faction's color + insignia; militia on the garrison side.
      const escHtml = fm.esc > 0 ? '<div class="escort why" style="--hc:'+FACTION_UI_COLORS[fm.atkSrc]+'">' + fcShips(fm.esc, TX_FACTIONS[fm.atkSrc].color) + insigniaSvg(fm.atkSrc, 20) + '<span>+'+fm.esc+' '+kw('ESCORT')+'</span><small>'+FNAMES[fm.atkSrc]+': '+favorWord(fm.atkSrc, tierIdx(s, fm.atkSrc))+'</small></div>' : '';
      const mercHtml = fm.merc > 0 ? '<button class="hired'+(briefMercs ? ' on' : '')+'" onclick="toggleMercs()">' + (briefMercs ? fcShips(fm.merc, '#B8A88A') + '<span>+'+fm.merc+' '+kw('HIRED')+'</span>' : '<span>HIRED SHIPS STAY HOME</span>') + '</button>' : '';  // dev.28: tap to leave them
      const milHtml = fm.mil > 0 ? '<div class="escort why" style="--hc:'+FACTION_UI_COLORS[fm.defSrc]+'">' + insigniaSvg(fm.defSrc, 20) + '<span style="color:var(--red)">+'+fm.mil+' '+kw('MILITIA')+'</span><small>'+FNAMES[fm.defSrc]+': '+favorWord(fm.defSrc, tierIdx(s, fm.defSrc))+'</small></div>' : '';
      b += '<div class="fcols"><div>' + (briefFleet > 0 ? fcShips(sent, FACTION_UI_COLORS[me.chosenFaction] || '#5FD4FF') : '<span class="unk">NO FLEET</span>') + escHtml + mercHtml +
           '</div><em>VS</em><div>' + gUnits + milHtml + '</div></div>';  // 3.1-dev.46 (user): the old dice spots are gone
      // 2.0-dev.34 (playtest: "if a planet is sympathetic to your cause there should be a warning"): attacking a
      // world whose faction is Favorable+ with you is a BETRAYAL -- your standing with them snaps to Neutral.
      { const bf = pl.chars[0].revealed ? AFFILIATION_TO_FACTION.indexOf(pl.chars[0].value) : -1;
        const bt = bf >= 0 && s.reputation[bf] ? s.reputation[bf].tier.toUpperCase() : '';
        if (bt === 'FAVORABLE' || bt === 'ALLIED')
          b += '<div class="betray"><b>BETRAYAL</b> · '+pl.name+' is on the '+TX_FACTIONS[bf].name+'\x27s side, and they see you as '+favorWord(bf, tierIdx(s, bf))+'. Attack, and they turn on you: '+favorWord(bf, tierIdx(s, bf)).toUpperCase()+' → '+favorWord(bf, 2).toUpperCase()+'.</div>'; }
      // 2.0-dev.27 forecast v2: whole-battle odds over the garrison sizes you can't rule out, as a range + a word.
      const mine = Math.max(1, sent + (fm.esc || 0) + (briefMercs ? (fm.merc || 0) : 0) + classAtk(s, s.planets[s.takeoverPlanet] ? s.planets[s.takeoverPlanet].owner : -1, s.planets[s.takeoverPlanet]));
      const pLo = battleWinPct(mine, fm.gHi, fm.atk, fm.def), pHi = battleWinPct(mine, fm.gLo, fm.atk, fm.def);
      const symKnown = pl.chars[0].revealed, defKnown = pl.chars[3].revealed;
      b += verdictHtml(pLo, pHi, (!symKnown || !defKnown) && pHi - pLo > 0
             ? 'You don\x27t know ' + (!symKnown && !defKnown ? 'its Sympathy or Defense' : !symKnown ? 'its Sympathy' : 'its Defense') + ', so the odds are a range.' : '');
      {  // 3.1-dev.46: whose standing moves if you win, and Rivet
        const pf = symKnown ? AFFILIATION_TO_FACTION.indexOf(pl.chars[0].value) : -1, held = pl.owner >= 0 && pl.owner !== mySlot;
        const win = held ? '<span>A PLAYER\u2019S WORLD \u00b7 NO FAVOR MOVES</span>'
                  : pf >= 0 ? '<span class="dn">\u25BC ' + FACTION_SHORT_NAMES[pf] + '</span><span class="up">\u25B2 ' + FACTION_SHORT_NAMES[RIVAL_OF[pf]] + '</span>'
                  : '<span>\u25BC ITS FACTION \u25B2 THEIR RIVAL</span>';
        const key = 'fc' + s.takeoverPlanet + '_' + s.round;
        let say = '';
        if (fm.esc > 0) say += 'The ' + FNAMES[fm.atkSrc] + ' sent a ship because you\u2019re their ' + favorWord(fm.atkSrc, tierIdx(s, fm.atkSrc)) + ' and this is their rival\u2019s world. It takes your first hit. ';
        if (fm.mil > 0) say += 'The ' + FNAMES[fm.defSrc] + '\u2019s loyalists joined the defenders because they don\u2019t trust you. They fall first. ';
        say += held ? 'This world belongs to ' + feedWho(s, pl.owner) + ', so taking it moves nobody\u2019s favor. '
             : pf >= 0 ? 'Win, and the ' + FNAMES[pf] + ' like you less while the ' + FNAMES[RIVAL_OF[pf]] + ' like you more. '
             : 'Win, and its faction likes you less while their rival likes you more. Learn its Sympathy to know who. ';
        say += pHi < 35 ? 'These odds are long: add a fleet, hire guns, or come back stronger.' : pLo > 65 ? 'The odds are with you.' : 'It could go either way.';
        b += '<div class="fcwin">IF YOU WIN ' + win + rivetBtn(key) + '</div>' + rivetSay(key, say.trim());
      }
      // The informants: learn it now, at INFORMANT_COST_MULT x the Prepare price.
      const inames = INFORMANT_NAMES[me.chosenFaction] || INFORMANT_NAMES[0];
      [[0, 'Learn its Sympathy now'], [3, 'Learn its Defense now']].forEach(([c, what], k) => {
        if (pl.chars[c].revealed || pl.owner >= 0 && c === 3) return;
        const cost = pl.chars[c].cost;
        b += '<button class="informant"'+(me.credits >= cost ? ' onclick="reveal('+s.takeoverPlanet+','+c+')"' : ' disabled')+'><span>'+inames[k]+'<small>'+what+' \u00B7 3\u00D7 the Prepare price</small></span><b>'+cost+' CR</b></button>';
      });
      b += '<div class="fleets3">';
      for (let f = 0; f < fleets.length; f++) {
        const sub = ready[f] ? fleets[f]+' ships' : fleets[f] ? 'spent' : (me.fleets[f].total > 0 ? 'destroyed' : 'not built');
        b += '<button class="'+((briefFleet & (1 << f)) ? 'sel' : '')+'"'+(ready[f] ? ' onclick="pickBriefFleet('+f+')"' : ' disabled')+'>FLEET '+(f+1)+'<small>'+sub+'</small></button>';
      }
      b += '</div>';
      b += (briefFleet > 0)
        ? '<button class="pill" style="border-color:var(--red);color:var(--red)" onclick="confirmFight()">ENGAGE</button>'
        : '<button class="pill" style="border-color:var(--red);color:var(--red)" disabled>NO FLEET READY</button>';
      b += '<button class="back" onclick="backFromBriefing()">&lsaquo; Back to '+pl.name+'</button></div>';
      setApp(app, html + b);
      return;
    } else if (payConfirmPending) {
      // Confirm before actually spending Credits -- Pay is the only one of
      // the three methods that's instant and irreversible with a single
      // tap, unlike Fight (leads into a battle) or Negotiate (shows offers
      // before finalizing).
      // v0.25.1 restyle (twin of the AMOLED's showConfirmPay): the planet, the
      // price big in gold, what you have, the reputation it earns (only once
      // Affiliation is revealed), PAY + back.
      const pi = s.takeoverPlanet, afford = me.credits >= s.payPrice;
      // v0.30.5: negF was dropped with the v0.30.0 negotiation rewrite but still used
      // here -- a ReferenceError, so PAY on a phone did nothing (playtest 2026-10-28).
      const negF = pl.chars[0].revealed ? AFFILIATION_TO_FACTION.indexOf(pl.chars[0].value) : -1;
      if (!galaxyImgs[pi]) galaxyImgs[pi] = bPlanetPixels(pi, pi * 37).toDataURL();
      const buyout = pl.owner >= 0 && pl.owner !== mySlot;  // 3.1-dev.79
      html += '<div class="vic"><span class="k">'+(buyout ? 'BUYOUT OFFER' : 'BRIBE')+'</span><b style="margin:0">'+pl.name+'</b>' +
              '<img class="pixel" src="'+galaxyImgs[pi]+'" width="120" height="120" alt="">' +
              '<div class="price" style="color:'+(afford ? '#FFD166' : 'var(--red)')+'">'+s.payPrice+' CR</div>' + (buyout ? '' : eraTag(s, 2, 'WORLDS COST A FORTUNE \u00b7 \u00d71.5')) +
              '<span class="dim"'+(afford ? '' : ' style="color:var(--red)"')+'>You have '+me.credits+' CR'+(afford ? '' : ' - not enough')+'</span>' +
              (buyout ? '<div class="fileBox" style="margin:6px 0;max-width:340px"><div><span>NOW</span><b>You pay ' + s.payPrice + ' CR. It\u2019s held until ' + feedWho(s, pl.owner) + ' answers.</b></div><div><span>THEY SELL</span><b>' + pl.name + ' is yours. They get half; the rest leaves the game.</b></div><div><span>THEY MATCH</span><b>They keep it. Your ' + s.payPrice + ' CR comes back.</b></div></div>'
                      : negF >= 0 ? '<span style="font-size:13px;letter-spacing:2px;color:'+FACTION_UI_COLORS[negF]+'">THE '+FACTION_SHORT_NAMES[negF]+' WILL APPROVE</span>' : '') +
              rivetAsk('pay' + pi + '_' + s.round, 'Worlds near the Core cost more than the Outer Rim. ' + (negF >= 0 ? 'It sides with the ' + FNAMES[negF] + ', so the better they like you, the cheaper it is.' : 'Scout whose side it\u2019s on: if they like you, it\u2019s cheaper.')) +
              '<button class="pill" style="border-color:#FFD166;color:#FFD166"'+(afford ? ' onclick="confirmPay()"' : ' disabled')+'>'+(buyout ? 'MAKE THE OFFER' : 'BRIBE')+'</button>' +
              '<button class="back" onclick="backFromPayConfirm()">&lsaquo; Back to '+pl.name+'</button></div>';
    } else {
      // v0.21.6: no separate approach card -- an open, uncommitted session
      // (e.g. just refused, with the reason in takeoverResult) shows the
      // planet page below, which carries FIGHT / NEGOTIATE / PAY itself.
      currentPlanet = s.takeoverPlanet;
      sessionOnPlanetPage = true;
    }
    if (!sessionOnPlanetPage) {
      setApp(app, html);
      return;
    }
  }

  // Planet page (v0.21.4 Invest/Movement, v0.21.6 Resolution; twin of the AMOLED's
  // showPlanetPage()). Your planet in Invest: garrison + INVEST DEFENSE
  // first, intel below. Other planets: DISCOVER INTEL tiles (REVEAL + cost).
  // Movement: the same tiles, read-only. Resolution: read-only tiles +
  // FIGHT / NEGOTIATE / PAY. Rebuilt only when the HTML changes (Conv. 8).
  if (currentPlanet >= 0) {
    const i = currentPlanet, p = s.planets[i];
    const mine = p.owner >= 0 && p.owner === mySlot, invest = s.phase === 0, resolution = s.phase === 2;
    const of = p.owner >= 0 ? s.players[p.owner].chosenFaction : -1;
    let sub, subCol = 'var(--dim)';
    if (mine) { sub = p.pending ? 'YOURS ON A DEBT · BREAK YOUR WORD AND THEY TAKE IT BACK' : 'YOURS'; subCol = 'var(--accent)'; }
    else {
      // v0.21.11 (user): no garrison at all on others' planets.
      sub = of >= 0 ? 'Held by ' + FACTION_SHORT_NAMES[of] : p.owner >= 0 ? 'Held by a rival' : 'Sworn to no one';  // 2.0 wording
      if (of >= 0) subCol = FACTION_UI_COLORS[of];
    }
    if (!galaxyImgs[i]) galaxyImgs[i] = bPlanetPixels(i, i * 37).toDataURL();
    html += '<div class="wsheet"><div class="wgrab" onclick="leavePlanet()"></div><button class="back" onclick="leavePlanet()">&lsaquo; Galaxy</button>';  // 3.1-dev.17: slides up like a sheet
    html += '<div class="phead"><span class="pvpwrap"><img class="pixel" src="'+galaxyImgs[i]+'" width="108" height="108" alt=""><i class="pvp">'+pnumSvg(p.vp, '#1A1206', 2)+'<small>VP</small></i></span>' +
            '<div><b>'+p.name+'</b><span style="color:'+subCol+'">'+sub+'</span></div></div>';
    if (WORLD_LORE[p.name]) html += '<div class="wlore">'+WORLD_LORE[p.name]+'</div>';  // 3.1-dev.17: every world has a story
    if (p.home >= 0) {  // 2.0 home worlds
      const hf = p.home, played = s.players.some(pl => pl.chosenFaction === hf), yours = me.chosenFaction === hf;
      const nm = FACTION_SHORT_NAMES[hf], col = FACTION_UI_COLORS[hf];
      let t;
      if (!played) t = 'Nobody plays the '+nm+' this game. A guard of 4 holds it. Only a fight takes it: worth '+p.vp+' VP while you hold it.';
      else if (yours && mine) t = 'Your fleets launch from here. Its home guard of 6 fights only for you. Worth '+p.vp+' VP to whoever holds it.';
      else if (yours) t = 'Taken from you. Your fleets now launch from any world you hold, or a space station if you hold none. Take it back and its '+p.vp+' VP come back to you.';
      else if (p.owner >= 0 && s.players[p.owner].chosenFaction === hf) t = 'Its home guard of 6 ships defends it. Only a fight takes it: worth '+p.vp+' VP while you hold it, and they lose them.';
      else t = mine ? 'You took it. Normal defenders hold it for you; worth '+p.vp+' VP while you hold it.'
                    : 'Taken from the '+nm+'. Normal defenders hold it now. Only a fight takes it.';
      html += '<div class="card" style="text-align:left;border-color:'+col+'"><div style="font-size:12px;letter-spacing:3px;color:'+col+'">'+(yours ? 'YOUR HOME WORLD' : 'HOME WORLD · '+nm)+'</div>' +
              '<div style="font-size:14px">'+t+'</div></div>';
    }
    if (p.owner >= 0 && p.owner !== mySlot && s.players[p.owner] && s.players[p.owner].commander % 4 === 3)  // 3.1-dev.78 PRICE ON THEIR HEAD
      html += '<div class="galNote good"><b style="color:#FFD166">BOUNTY +3 CR</b> \u00b7 ' + p.name + ' is a Scoundrel\u2019s world: take it and the price on their head is yours.</div>';
    if (mine && resolution) html += '<div class="garr"><span>DEFENDERS</span><b>'+p.defense+'</b><span>/ '+(p.dl || 8)+' ships</span></div>';
    if (mine && invest) {
      const full = p.defense >= (p.dl || 8), afford = me.credits >= 2;
      html += '<div class="garr"><span>DEFENDERS</span><b>'+p.defense+'</b><span>/ '+(p.dl || 8)+' ships</span></div>';
      html += '<button class="pill" style="border-color:var(--accent);color:var(--accent)"'+(full || !afford ? ' disabled' : '')+' onclick="invest('+i+')">' +
              (full ? 'DEFENDERS FULL<small>'+(p.dl || 8)+' ships is the most '+p.name+' holds</small>'
                    : 'INVEST DEFENSE<small>'+(afford ? '+1 defender · 2 CR' : 'needs 2 CR')+'</small>') + '</button>';
    }
    if (resolution) { html += resWorldBody(s, i) + '</div>'; setApp(app, html); return; }  // UI 3.0 take 4
    html += attackLine(s, i);  // 2.0-dev.29
    html += worldFactsHtml(s, i);  // 3.1-dev.115 (the old DISCOVER INTEL tiles are skipped below)
    if (false) for (const c of [1, 2, 0, 3]) {
      const ch = p.chars[c];
      if (ch.revealed) {
        let extra = '', col = '';
        if (c === 0) {
          const fi = AFFILIATION_TO_FACTION.indexOf(ch.value);
          if (fi >= 0) { col = ' style="color:'+FACTION_UI_COLORS[fi]+'"'; extra = '<small>You: '+favorWord(fi, tierIdx(s, fi))+'</small>'; }
        }
        if (c === 2 && p.traitFx) extra = '<small>Hold it: '+p.traitFx+'</small>';  // 2.0
        if (c === 3 && p.dd > 0 && invest) extra = '<button class="ghost" style="margin-top:6px" onclick="reveal('+i+',3)"'+(me.credits >= ch.cost ? '' : ' disabled')+'>LOOK AGAIN \u00b7 '+ch.cost+' CR</button>';  // 3.1-dev.101
        html += '<div class="done"><span class="k">'+kw(c === 3 ? defKey(p) : KEYS[c])+'</span><span class="v"'+col+'>'+(c === 3 ? defVal(p, ch.value) : ch.value)+'</span>'+extra+'</div>';
      } else if (invest) {
        const afford = me.credits >= ch.cost;
        html += '<button class="rv"'+(afford ? '' : ' disabled')+' onclick="reveal('+i+','+c+')"><span class="k">'+kw(c === 3 ? 'BASE DEFENSE' : KEYS[c], KEYS[c])+'</span>' +
                '<span class="c">'+(ch.cost === 0 ? 'REVEAL \u00B7 FREE' : 'REVEAL · '+ch.cost+' CR')+'</span></button>';
      } else {
        html += '<div class="done hid"><span class="k">'+kw(c === 3 ? 'BASE DEFENSE' : KEYS[c], KEYS[c])+'</span><span class="v">???</span></div>';
      }
    }
    // (3.1-dev.115: no intel grid to close)
    if (p.unbound) html += '<div class="note" style="color:#A6E34B">THE UNBOUND hold this world: no Pay. First to take it gets +1 VP.</div>';
    if (p.mOpen && s.v2) {  // 2.0: its market, when open this round
      html += '<div class="card" style="text-align:left;border-color:#FFD166"><div style="font-size:12px;letter-spacing:3px;color:#FFD166">MARKET OPEN THIS ROUND \u00B7 '+(p.mStock ? p.mStock+' left' : 'sold out')+'</div>' +
              '<div style="font-size:15px"><b>'+p.good+'</b>: '+p.goodFx+'</div>' +
              '<div style="font-size:12px;color:var(--dim);margin-top:4px">'+marketCutLine(s, i)+'</div>' +
              (invest ? (p.mShut ? '<button class="back" style="margin-top:8px;border-style:dashed" disabled>SHUT TO YOU</button>' : '<button class="back" style="margin-top:8px'+(p.mBuy ? ';border-color:#FFD166;color:#FFD166' : '')+'"'+(p.mBuy ? '' : ' disabled')+' onclick="marketBuy('+i+')">BUY \u00B7 '+p.mPrice+' CR</button>') : '') + '</div>';
    }
    if (resolution) {
      // v0.21.6: FIGHT / NEGOTIATE / PAY right here (twin of the AMOLED's
      // planetResolutionBody()); a refusal from the last try shows above them.
      const res = (s.takeoverPlayer === mySlot && s.takeoverPlanet === i) ? s.takeoverResult : '';
      if (res) html += '<p class="alert">'+res+'</p>';
      if (mine) html += '<div class="note">You hold this world. Fortify it in the Prepare phase.</div>';
      else if (p.locked) html += '<div class="note">You\x27ve made your move here this round. Try again next round.</div>';
      else if (p.owner >= 0 && p.owner !== mySlot)  // v0.28.3: a commander's world can only be taken by force
        html += '<div class="acts"><button class="fight" onclick="planetFight('+i+')">FIGHT</button></div>' +
                '<div class="note">A commander holds this world. Negotiating and bribing are off the table.</div>';
      else if (p.home >= 0)  // 2.0: only a fight takes a home world
        html += '<div class="acts"><button class="fight" onclick="planetFight('+i+')">FIGHT</button></div>' +
                '<div class="note">A home world. Negotiating and bribing are off the table.</div>';
      else html += '<div class="acts"><button class="fight" onclick="planetFight('+i+')">FIGHT</button>' +
                   '<button class="neg" onclick="planetNegotiate('+i+')">NEGOTIATE</button>' +
                   '<button class="pay" onclick="planetPay('+i+')">BRIBE<small>'+p.pay+' CR' + eraTag(s, 2, '\u00d71.5') + '</small></button></div>';
    } else {
      html += '<div class="note">'+(invest ? 'Takeovers (Fight / Negotiate / Bribe) open in the Resolution phase.'
                                    : 'Move your fleets on the board. Takeovers open next phase.')+'</div>';
    }
    html += worldPagerHtml(s, i) + '</div>';  // 3.1-dev.115
    setApp(app, html);
    return;
  }

  // PvP (v0.21.10; twin of the AMOLED's showPvpTargets / showPvpFleetPick /
  // PvP forecast): Fleets (attack mode) -> opponent -> their fleet ->
  // FORECAST (plain d6 both sides) -> ENGAGE. Rebuilt only when the HTML
  // changes (Convention 8).
  if (battleMode && s.phase === 2) {
    const myCol = FACTION_UI_COLORS[me.chosenFaction] || '#5FD4FF';
    const okAtk = f => f >= 0 && me.fleets[f] && me.fleets[f].alive > 0 && !me.fleets[f].spent;
    if (!okAtk(pvpAtkFleet)) pvpAtkFleet = myFirstFleet(me);
    if (pvpTarget < 0) {
      html += '<button class="back" onclick="closeBattle()">&lsaquo; Fleets</button>';
      html += '<div class="gxhead"><b style="color:var(--red)">ATTACK WHO?</b><span>' +
              (pvpAtkFleet >= 0 ? 'Fleet '+(pvpAtkFleet+1)+' is ready to launch' : 'No fleet ready this round') + '</span></div><div class="rows">';
      let any = false;
      for (let i = 0; i < s.players.length; i++) {
        const p = s.players[i];
        if (!p.active || i === mySlot) continue;
        any = true;
        const f = p.chosenFaction, ships = p.fleets.reduce((n, fl) => n + fl.alive, 0), nf = p.fleets.filter(fl => fl.alive > 0).length;
        const col = f >= 0 ? FACTION_RING_COLORS[f] : 'var(--line)';
        html += '<button style="border-color:'+col+'"'+(ships && pvpAtkFleet >= 0 ? ' onclick="pickTarget('+i+')"' : ' disabled')+'>' +
                '<span class="t" style="color:'+(f >= 0 ? FACTION_UI_COLORS[f] : 'var(--text)')+'">'+(f >= 0 ? FACTION_SHORT_NAMES[f] : p.name)+'</span>' +
                '<small>'+ships+' ship'+(ships === 1 ? '' : 's')+' in '+nf+' fleet'+(nf === 1 ? '' : 's')+'</small>'+(f >= 0 ? insigniaSvg(f, 36) : '')+'</button>';
      }
      html += '</div>';
      if (!any) html += '<p>No other commanders have joined.</p>';
    } else if (pvpDefFleet < 0) {
      const t = s.players[pvpTarget], tf = t.chosenFaction;
      const col = tf >= 0 ? FACTION_RING_COLORS[tf] : 'var(--red)';
      html += '<button class="back" onclick="backToTargets()">&lsaquo; Opponents</button>';
      html += '<div class="gxhead"><b style="color:'+(tf >= 0 ? FACTION_UI_COLORS[tf] : 'var(--text)')+'">'+(tf >= 0 ? FACTION_SHORT_NAMES[tf] : t.name)+' FLEETS</b>' +
              '<span>Which one do you strike?</span></div><div class="rows">';
      for (let k = 0; k < t.fleets.length; k++) {
        const a = t.fleets[k].alive;
        html += '<button style="border-color:'+(a ? col : 'var(--ring)')+'"'+(a ? ' onclick="pickDefFleet('+k+')"' : ' disabled')+'>' +
                '<span class="t">FLEET '+(k+1)+'</span><small>'+(a ? a+' ship'+(a === 1 ? '' : 's') : (t.fleets[k].total > 0 ? 'destroyed' : 'not built'))+'</small>' +
                (a ? fcShips(a, col) : '')+'</button>';
      }
      html += '</div>';
    } else {
      const t = s.players[pvpTarget], tf = t.chosenFaction;
      const tcol = tf >= 0 ? FACTION_RING_COLORS[tf] : '#FF6B5A';
      html += '<div class="fchead"><span>'+(pvpMsg || 'FORECAST')+'</span><b>'+(tf >= 0 ? FACTION_SHORT_NAMES[tf] : t.name)+' FLEET '+(pvpDefFleet+1)+'</b></div>';
      me.fleets.forEach((fl, f) => { if (!okAtk(f)) pvpAtkMask &= ~(1 << f); });
      const sentShips = me.fleets.reduce((n, fl, f) => n + ((pvpAtkMask & (1 << f)) ? fl.alive : 0), 0);
      const myMercs = (s.v2 && s.v2.perks && s.v2.perks.merc) || 0;  // 2.0-dev.26
      html += '<div class="fcols"><div>' + (pvpAtkMask > 0 ? fcShips(sentShips, myCol) : '<span class="unk">NO FLEET</span>') +
              (myMercs > 0 ? '<button class="hired'+(briefMercs ? ' on' : '')+'" onclick="toggleMercs()">' + (briefMercs ? fcShips(myMercs, '#B8A88A') + '<span>+'+myMercs+' '+kw('HIRED')+'</span>' : '<span>HIRED SHIPS STAY HOME</span>') + '</button>' : '') +
              fcDie(0, -1, 'var(--accent)') + '</div><em>VS</em><div>' + fcShips(t.fleets[pvpDefFleet].alive, tcol, true) + fcDie(0, -1, 'var(--red)') + '</div></div>';
      { const pv = battleWinPct(Math.max(1, sentShips + (briefMercs ? myMercs : 0) + classAtk(s, -1)), t.fleets[pvpDefFleet].alive, 0, 0, true);  // 2.0-dev.27: exact -- their fleet is in plain sight
        html += verdictHtml(pv, pv, 'Their fleet is in plain sight. Hired ships they bought would only show up in the fight.'); }
      html += '<div class="fleets3">';
      for (let f = 0; f < me.fleets.length; f++) {
        const fl = me.fleets[f];
        const sub = okAtk(f) ? fl.alive+' ships' : fl.alive ? 'spent' : (fl.total > 0 ? 'destroyed' : 'not built');
        html += '<button class="'+((pvpAtkMask & (1 << f)) ? 'sel' : '')+'"'+(okAtk(f) ? ' onclick="pickPvpAtk('+f+')"' : ' disabled')+'>FLEET '+(f+1)+'<small>'+sub+'</small></button>';
      }
      html += '</div>' + (pvpAtkMask > 0
        ? '<button class="pill" style="border-color:var(--red);color:var(--red)" onclick="pvpAttack('+pvpTarget+','+pvpDefFleet+')">ENGAGE</button>'
        : '<button class="pill" style="border-color:var(--red);color:var(--red)" disabled>NO FLEET READY</button>');
      html += '<button class="back" onclick="backToDefFleets()">&lsaquo; Back to fleets</button>';
    }
    setApp(app, html);
    return;
  }

  if (attackChoice && s.phase !== 2) attackChoice = false;
  if (attackChoice && s.phase === 2) {
    const ready = me.fleets.filter(fl => fl.alive > 0 && !fl.spent).length;
    const rivals = s.players.filter((p, i) => p.active && i !== mySlot).length;
    html += '<button class="back" onclick="closeAttack()">&lsaquo; Home</button>';
    html += '<div class="gxhead"><b style="color:var(--red)">ATTACK</b><span>'+(ready ? ready+' fleet'+(ready === 1 ? '' : 's')+' ready' : 'No fleet is ready this round')+'</span></div><div class="rows">';
    html += '<button style="border-color:'+(ready ? 'var(--red)' : 'var(--ring)')+'"'+(ready ? ' onclick="attackPlanet()"' : ' disabled')+'><span class="t">A PLANET</span><small>Pick it on the galaxy map</small></button>';
    html += '<button style="border-color:'+(ready && rivals ? 'var(--red)' : 'var(--ring)')+'"'+(ready && rivals ? ' onclick="attackPlayer()"' : ' disabled')+'><span class="t">A PLAYER</span><small>'+(rivals ? 'Choose a fleet, then a rival' : 'No other commanders')+'</small></button>';
    if (s.v2 && s.v2.ashen && s.v2.ashen.alive)  // 2.0
      html += '<button style="border-color:'+(ready ? '#FF8C42' : 'var(--ring)')+';color:'+(ready ? '#FF8C42' : 'inherit')+'"'+(ready ? ' onclick="openAshen()"' : ' disabled')+'><span class="t">THE ASHEN FLEET</span><small>A joint strike · '+s.v2.ashen.hp+' / '+s.v2.ashen.max+' HP left</small></button>';
    html += '</div>';
    setApp(app, html);
    return;
  }

  // My Fleets: list -> detail with investment. v0.21.10: restyled rows; in
  // Resolution this is the ATTACK screen (Home's FLEETS tile turns into a
  // red ATTACK tile) -- tap a ready fleet to send it at a rival.
  if (fleetsMode) {
    if (currentFleetIdx < 0) {
      const attack = s.phase === 2;
      const myCol = FACTION_UI_COLORS[me.chosenFaction] || '#5FD4FF';
      html += '<button class="back" onclick="closeFleets()">&lsaquo; Home</button>';
      html += '<div class="gxhead"><b'+(attack ? ' style="color:var(--red)"' : '')+'>'+(attack ? 'ATTACK' : 'FLEETS')+'</b><span>' +
              (attack ? 'Pick a fleet to send in' : 'Tap a fleet for its ships')+'</span></div><div class="rows">';
      for (let f=0; f<me.fleets.length; f++){
        const fl = me.fleets[f], ready = fl.alive > 0 && !fl.spent;
        const st = fl.alive > 0 ? (attack ? (ready ? 'ready · '+fl.alive+' ship'+(fl.alive === 1 ? '' : 's') : 'spent this round') : fl.alive+' / '+fl.total+' ships')
                                : (fl.total > 0 ? 'destroyed' : 'not built');
        const click = attack ? (ready ? ' onclick="attackWith('+f+')"' : ' disabled') : ' onclick="openFleetDetail('+f+')"';
        const border = attack ? (ready ? 'var(--red)' : 'var(--ring)') : (fl.alive > 0 ? 'var(--accent)' : 'var(--line)');
        html += '<button style="border-color:'+border+'"'+click+'><span class="t">FLEET '+(f+1)+'</span><small>'+st+'</small>' +
                (fl.alive > 0 ? fcShips(fl.alive, myCol) : '')+'</button>';
      }
      html += '</div>';
      setApp(app, html);
      return;
    } else {
      // v0.25.1 restyle (twin of the AMOLED's showFleetDetail): six ship
      // shapes in your color (destroyed = red cross, not built = faint
      // outline), status + Credits, then INVEST / FOUND / REBUILD as a pill.
      const fl = me.fleets[currentFleetIdx];
      // No living ships = off the board (never founded, or destroyed --
      // v0.20.2); a destroyed fleet rebuilds at the home hangar (v0.28.2), a new one needs a planet. Matches the .ino.
      const offBoard = fl.alive === 0;
      const myCol = FACTION_UI_COLORS[me.chosenFaction] || 'var(--accent)';
      const status = offBoard ? (fl.total > 0 ? 'DESTROYED' : 'NOT FOUNDED') : (fl.spent ? 'SPENT THIS ROUND' : 'READY');
      html += '<div class="vic"><b style="margin:0;color:var(--accent);letter-spacing:4px">FLEET '+(currentFleetIdx+1)+'</b>' +
              '<span style="font-size:13px;letter-spacing:2px;color:'+(offBoard ? 'var(--red)' : 'var(--dim)')+'">'+status+' · '+me.credits+' CR</span><div class="ships">';
      const cap = me.cap || fl.ships.length;  // 2.0-dev.20: the Empire's fleets hold 7
      for (let sh=0; sh<cap; sh++){
        const st = sh >= fl.total ? 0 : (fl.ships[sh] ? 1 : 2);
        const tri = '<polygon points="44,20 4,4 4,36" ';
        const svg = st === 1 ? tri + 'fill="'+myCol+'"/>'
                  : st === 2 ? tri + 'fill="none" stroke="#5A2A2A" stroke-width="2"/><path d="M10 6l28 28M38 6l-28 28" stroke="#FF6B5A" stroke-width="3"/>'
                  : tri + 'fill="none" stroke="#2A3B50" stroke-width="2"/>';
        html += '<div'+(st === 1 ? ' class="on"' : '')+'><svg width="48" height="40" viewBox="0 0 48 40" aria-hidden="true">'+svg+'</svg>'+SHIP_NAMES[sh]+'</div>';
      }
      html += '</div>';
      // v0.28.4: every fleet is founded or rebuilt at your home hangar -- no planet needed.
      if (s.phase===0 && (offBoard || fl.total < cap || fl.alive < fl.total)) {
        const afford = me.credits >= 2;
        html += '<button class="pill" style="border-color:#FFD166;color:#FFD166"'+(afford ? ' onclick="investFleet('+currentFleetIdx+')"' : ' disabled')+'>'+
                (offBoard ? (fl.total > 0 ? 'REBUILD FLEET' : 'FOUND FLEET') : fl.alive < fl.total ? 'REPAIR A SHIP' : 'INVEST NEW SHIP')+'<small>2 CR</small></button>';
      }
      html += '<button class="back" onclick="backToFleetsList()">&lsaquo; Fleets</button></div>';
    }
    setApp(app, html);
    return;
  }

  // Factions (v0.21.17 restyle, twin of the AMOLED's showReputation): your
  // standing with each faction, tier only -- insignia, name, five pips lit up
  // to your tier, tier word colored by how they feel about you.
  if (repMode) {
    const TIERS = ['HOSTILE', 'WARY', 'NEUTRAL', 'FAVORABLE', 'ALLIED'];
    const TIER_COL = ['#FF6B5A', '#FF9B70', '#A9BDD0', '#8CE7A0', '#FFD166'];
    const mine = s.players[s.yourSlot] ? s.players[s.yourSlot].chosenFaction : -1;
    html += '<div style="font-size:22px;font-weight:700;letter-spacing:4px;margin-bottom:4px">FAVOR</div><div class="fvsub">Tap a faction for its ladder. Only you see this.</div>';
    // v0.29.0: each faction's bar -- five zones, one bright line at your exact
    // standing. It slides from where this phone last showed it; the streak marks
    // the change for a few seconds. No numbers, by design (user 2026-10-19).
    const nowMs = Date.now();
    // 3.1-dev.71 (user screenshot: "the favor bars are having a glitch"): the zones were sized by their score range with gaps
    // the marker ignored (Friend a sliver, markers off at the edges) -- now five EQUAL zones, the marker inside its own zone.
    const ZLO = [-50, -29, -9, 10, 17], ZHI = [-30, -10, 9, 16, 50];
    const repPct = v => { v = Math.max(-50, Math.min(50, v)); let z = 4; for (let q = 0; q < 5; q++) if (v <= ZHI[q]) { z = q; break; }
      const fr = (v - ZLO[z] + 0.5) / (ZHI[z] - ZLO[z] + 1);
      return 'calc(' + (z * 20) + '% + ' + Math.max(0, Math.min(1, fr)).toFixed(3) + ' * (20% - 2px))'; };
    for (let f=0; f<TX_FACTIONS.length; f++){
      const t = Math.max(0, TIERS.indexOf(s.reputation[f].tier));
      const score = s.reputation[f].score || 0;
      if (repSeen[f] === null) repSeen[f] = score;
      if (score !== repSeen[f]) { repMoved[f] = { from: repSeen[f], to: score, at: nowMs, fresh: true }; repSeen[f] = score; }
      const mv = repMoved[f], recent = mv && nowMs - mv.at < 8000;
      let bar = '<div class="rbar">' + [0, 1, 2, 3, 4].map(k => '<i class="z'+(k === t ? ' on' : '')+'" style="left:'+(k * 20)+'%;background:'+TIER_COL[k]+'"></i>').join('');
      if (recent) {
        const a = Math.min(mv.from, mv.to), b = Math.max(mv.from, mv.to);
        bar += '<i class="trail" style="left:'+repPct(a)+';width:calc('+repPct(b).slice(5, -1)+' - ('+repPct(a).slice(5, -1)+'));background:'+(mv.to > mv.from ? '#8CE7A0' : '#FF6B5A')+'"></i>';
      }
      bar += (mv && mv.fresh) ? '<i class="mk" style="left:'+repPct(mv.from)+'" data-to="'+repPct(score)+'"></i>' : '<i class="mk" style="left:'+repPct(score)+'"></i>';
      bar += '<i class="ctk" style="left:' + repPct(23) + '" title="Champion"></i></div>';  // 3.1-dev.50 tier 6 (REP_CHAMPION_MIN)
      if (mv) mv.fresh = false;
      // 3.1-dev.8: the faction's own word for your tier + its icon; tier 1 says they're out for you; tier 5 names the gift you hold.
      const gotGift = s.v2 && (s.v2.gifts || 0) & (1 << f);
      const sub = t === 0 ? '<div class="fvline" style="color:#FF6B5A">OUT FOR YOU</div>'
                : t === 4 ? '<div class="fvline" style="color:#FFD166">' + GIFT5[f][0].toUpperCase() + '</div>'
                : t === 3 && gotGift ? '<div class="fvline" style="color:#8CE7A0">GIFT EARNED · ' + GIFT4[f][0] + '</div>'
                : t === 2 && !gotGift ? '<div class="fvline">NEXT: ' + GIFT4[f][0] + '</div>' : '';
      const isChamp = s.v2 && s.v2.champ && s.v2.champ[f] === mySlot, hasDecree = s.v2 && (s.v2.decrees || 0) & (1 << f);  // 3.1-dev.12
      html += '<button class="facrow fvrow'+(f === mine ? ' mine' : '')+'" onclick="openLadder('+f+')">'+insigniaSvg(f, 28) +
              '<div class="nm"><div class="top"><b style="color:'+FACTION_UI_COLORS[f]+'">'+FACTION_SHORT_NAMES[f]+(f === mine ? ' · HOME' : '')+(isChamp ? ' \uD83D\uDC51' : '')+'</b>' +
              '<span class="tier" style="color:'+FAVOR_COL[t]+'">'+pixIcon(FAVOR_ICON[t], FAVOR_COL[t], 1)+' '+favorWord(f, t).toUpperCase()+'</span></div>'+bar+sub+'</div></button>';
      if (hasDecree) html += '<button class="pill decreeBtn" style="--ec:'+ERAS[f].col+'" onclick="openEraSheet()">\uD83D\uDC51 DECLARE '+ERAS[f].name+'</button>';
    }
    setTimeout(() => requestAnimationFrame(() => document.querySelectorAll('.rbar .mk[data-to]').forEach(m => { m.style.left = m.dataset.to; })), 60);
    // v0.27.3: what you owe and when (playtest: "there is no place to see where
    // your favors are and when they are due").
    html += '<div style="font-size:14px;font-weight:700;letter-spacing:3px;margin:16px 0 8px;color:var(--dim)">YOU OWE</div>';
    const proms = (s.promises || []).slice().sort((a, b) => a.due - b.due);
    if (!proms.length) html += '<p style="color:var(--faint);font-size:14px">You owe no one. For now.</p>';
    proms.forEach(pr => {
      const pl = s.planets[pr.p] ? s.planets[pr.p].name : '';
      const left = pr.due - s.round;
      const when = left <= 0 ? 'DUE THIS ROUND' : 'DUE ROUND '+pr.due+' ('+left+' to go)';
      html += '<div class="facrow" style="display:block;border-color:'+(left <= 0 ? '#FFD166' : 'var(--line)')+'">' +
              '<b style="color:'+FACTION_UI_COLORS[pr.f]+'">'+TX_FACTIONS[pr.f].name+'</b>'+(pl ? ' &middot; '+pl : '') +
              '<div style="font-size:12px;letter-spacing:2px;color:'+(left <= 0 ? '#FFD166' : 'var(--dim)')+';margin:4px 0">'+when+' &middot; THE GAME CHECKS'+'</div>' +
              '<div style="font-size:14px;color:var(--text)">'+pr.text+'</div></div>';
    });
    // v0.28.3: the last promises the galaxy remembers you keeping -- or breaking.
    const plog = s.promiseLog || [];
    if (plog.length) {
      html += '<div style="font-size:14px;font-weight:700;letter-spacing:3px;margin:16px 0 8px;color:var(--dim)">PROMISE HISTORY</div>';
      plog.forEach(r => {
        const pl = s.planets[r.p] ? s.planets[r.p].name : '';
        html += '<div class="facrow" style="display:block;opacity:.85">' +
                '<b style="color:'+(r.kept ? '#8CE7A0' : '#FF6B5A')+'">'+(r.kept ? 'KEPT' : 'BROKEN')+'</b> &middot; ' +
                '<span style="color:'+FACTION_UI_COLORS[r.f]+'">'+TX_FACTIONS[r.f].name+'</span>'+(pl ? ' &middot; '+pl : '')+' &middot; round '+r.round +
                '<div style="font-size:13px;color:var(--dim);margin-top:4px">'+r.text.replace(/ \(honor[^)]*\)$/, '')+'</div></div>';
      });
    }
    html += '<button class="pill" style="border-color:var(--line);color:var(--dim)" onclick="closeReputation()">BACK</button>';
    setApp(app, html);
    return;
  }


  // 2.0 THE ASHEN FLEET (phone twin of the 1.75"'s showAshen).
  if (ashenMode && s.v2 && s.v2.ashen) {
    const a = s.v2.ashen;
    html += '<button class="back" onclick="closeAshen()">&lsaquo; Back</button>';
    html += '<div style="font-size:22px;font-weight:700;letter-spacing:4px;color:#FF8C42">THE ASHEN FLEET</div>';
    html += '<img class="pixel" src="'+spriteUrl('ashen', ASHEN_SPRITE, 8)+'" style="width:86%;margin:12px auto;display:block" alt="">';
    if (!a.alive) {
      html += '<div class="note">The Ashen Fleet is gone. The lanes are open again.</div>' + (ashenResult ? '<div class="card">'+ashenResult+'</div>' : '');
      setApp(app, html);
      return;
    }
    html += '<div class="hpbar"><i style="width:'+Math.round(100 * a.hp / Math.max(1, a.max))+'%"></i></div>' +
            '<div style="font-size:14px;margin:6px 0 10px">'+a.hp+' / '+a.max+' HP · your damage: '+a.mine+'</div>';
    if (ashenResult) html += '<div class="card" style="border-color:#FF8C42;text-align:left"><div style="font-size:12px;letter-spacing:3px;color:#FF8C42">YOUR STRIKE</div>'+ashenResult+'</div>';
    if (s.phase !== 2) {
      html += '<div class="note">Every market is closed while it lives. Strike it in Resolution: every ship you send rolls a die, and it answers once.</div>' +
              '<div class="note" style="color:#FF8C42">Sink it: +'+a.vp+' VP for everyone who hurt it, +'+a.topVp+' more for the most damage.</div>';
      setApp(app, html);
      return;
    }
    const ready = me.fleets.map((f, i) => (f.alive > 0 && !f.spent) ? i : -1).filter(i => i >= 0);
    if (ashenMask < 0) ashenMask = ready.reduce((m, i) => m | (1 << i), 0);
    let ships = 0;
    html += '<div style="font-size:13px;color:var(--dim);margin-bottom:6px">Send fleets: each ship rolls a die. It answers once: 1-2 miss, 3-5 one hit, 6 two.</div><div class="acts">';
    me.fleets.forEach((f, i) => {
      const ok = ready.includes(i), on = ok && (ashenMask & (1 << i));
      if (on) ships += f.alive;
      html += '<button style="'+(on ? 'border-color:#FF8C42;color:#FF8C42' : 'border-color:var(--line);color:var(--dim)')+'"'+(ok ? ' onclick="ashenToggle('+i+')"' : ' disabled')+'>FLEET '+(i + 1) +
              '<small>'+(ok ? f.alive+' ship'+(f.alive === 1 ? '' : 's') : f.alive ? 'spent' : 'empty')+'</small></button>';
    });
    html += '</div><button class="pill" style="border-color:#FF8C42;color:#FF8C42"'+(ships ? ' onclick="ashenStrike()"' : ' disabled')+'>'+(ships ? 'STRIKE · '+ships+' SHIP'+(ships === 1 ? '' : 'S') : (ready.length ? 'PICK A FLEET' : 'NO FLEET READY'))+'</button>';
    setApp(app, html);
    return;
  }

  // 2.0 MARKETS: the four open this round, the same for everyone; buy from anywhere in Prepare.
  if (marketsMode && s.v2 && s.v2.ashen && s.v2.ashen.alive) {  // boarded up while the Ashen Fleet blockades the lanes
    html += '<div style="font-size:22px;font-weight:700;letter-spacing:4px;color:#FFD166">MARKETS</div>';
    html += '<div class="closedwrap"><div class="plank" style="transform:rotate(24deg)"></div><div class="plank" style="transform:rotate(-24deg)"></div>' +
            '<img src="'+spriteUrl('closed', CLOSED_SPRITE, 6)+'" alt="CLOSED"></div>';
    html += '<div style="font-size:12px;letter-spacing:2px;color:#FF8C42">THE ASHEN FLEET BLOCKADES THE LANES</div>' +
            '<p style="font-size:14px;color:var(--dim)">Destroy it and the markets reopen next round.</p>';
    html += '<button onclick="closeMarkets()">BACK</button>';
    setApp(app, html);
    return;
  }
  if (bmMode && (s.fh || s.bm)) { setApp(app, html + bmPageHtml(s)); return; }  // 3.1-dev.77 (3.1-dev.99: also the Dawn's Era / your cargo hold)
  if (marketsMode) {
    // 3.1-dev.17 (user: "the mockup for the store looks better"): mockup Market.dc.html -- one card per faction.
    html += '<div class="mkTop"><span>MARKETS</span><b>'+pnumSvg(me.credits, '#FFD166', 3)+'<small>CR</small></b></div>';
    html += '<p class="mkhelp">'+(s.phase === 0 ? 'Each faction sells one thing a round to those it favors. It changes every round. Climb to unlock; fall and the shop closes to you.' : 'Buying happens in Prepare.')+'</p>';
    const stalls = s.v2.stalls || [];
    if (!stalls.length) html += '<p style="color:var(--dim)">No shops are open this round.</p>';
    html += '<div class="mkGrid">';
    for (const st of stalls) {
      const col = FACTION_UI_COLORS[st.f], t = tierIdx(s, st.f), emb = s.v2.emb === st.f;
      const goods = st.it.filter(x => x[0] >= 0);
      let say = '';
      html += '<div class="mkCard'+(st.shut ? ' shut' : '')+(goods.length > 2 ? ' wide' : '')+'" style="--fc:'+col+'"><div class="mkHead"><b>'+FACTION_SHORT_NAMES[st.f]+'</b><span>'+pixIcon(FAVOR_ICON[t], FAVOR_COL[t], 1)+' '+favorWord(st.f, t).toUpperCase()+'</span></div>';
      goods.forEach(([g, n, pr, buy]) => {
        const G = s.v2.goods[g] || ['', '', 0], k = st.it.findIndex(x => x[0] === g);
        html += '<div class="mkGood"><b>'+feedTitle(G[0])+'</b><span>'+G[1]+'</span>' +
                (st.shut ? '' : n ? '<button class="mkBuy"'+(buy ? ' onclick="stallBuy('+st.f+','+k+')"' : ' disabled')+'>BUY \u00B7 '+pr+' CR'+(n < 2 ? ' \u00B7 LAST ONE' : '')+'</button>' : '<button class="mkBuy" disabled>SOLD OUT</button>') + '</div>';
      });
      if (st.shut) {  // 3.1-dev.46: shuttered, with the reason and Rivet
        const key = 'shop' + st.f + '_' + s.round + '_' + t;
        html += '<div class="mkStencil">' + (emb ? 'EMBARGO' : 'CLOSED TO YOU') + '</div><div class="mkShut">'+(emb ? 'Shut to everyone this round.' : 'You\u2019re in bad standing with them.')+'</div>' +
                '<div class="rvAsk">' + rivetBtn(key) + '</div>';
        say = rivetSay(key, emb ? 'The ' + FNAMES[st.f] + ' shut their shop to everyone this round. It\u2019s not about you.'
                                  : 'The ' + FNAMES[st.f] + ' won\u2019t sell to you while you\u2019re in bad standing with them. Get on better terms and the shutters go up: a job for them, a kept promise or buying one of their worlds all help.');
      }
      html += '</div>' + say;
    }
    html += '</div>';
    html += '<button onclick="closeMarkets()">BACK</button>';
    setApp(app, html);
    return;
  }

  if (feedMode) { setApp(app, feedHtml(s)); return; }  // 3.1-dev.3
  if (cardMode) { setApp(app, cardHtml(s)); return; }  // 3.1-dev.6
  if (commsMode) { setApp(app, html + commsHtml(s)); return; }  // 3.1-dev.103 COMMS
  // Standings: everyone's VP total, fully public (unlike reputation).
  if (standingsMode) {
    // v0.21.0: rebuilt to match the AMOLED -- insignia, rank, VP, planets.
    html += liveNowHtml(s) + '<div style="font-size:22px;font-weight:700;letter-spacing:4px;margin-bottom:10px">WAR TABLE</div>' +
            (s.v2 && s.v2.fileCan ? '<div class="galNote" style="border-color:#C8364A;text-align:center"><b style="color:#C8364A">PULL A FILE</b> \u00b7 tap a rival\u2019s face below to open their file</div>' : '') +  // 3.1-dev.122 (was a COMMS row: "pull a file being in comms got confusing")
            standingsRowsHtml(s) +
            '<button class="gyrow" style="margin:10px 0" onclick="standingsMode=false;openReputation()">' + pixIcon('crown', '#FFD166', 2) + '<span><b>YOUR FAVOR</b> where you stand with each faction</span><em></em></button>';  // 3.1-dev.103
    if (me.chosenFaction >= 0 && s.v2) {  // 2.0: your quest and the galaxy event
      html += questHtml(s);  // 3.1-dev.7: your CLASS quest
      if (s.v2.event >= 0) html += '<div class="card" style="text-align:left;border-color:#FF8C42"><div style="font-size:12px;letter-spacing:3px;color:#FF8C42">GALAXY EVENT \u00B7 '+s.v2.eventName+'</div><div style="font-size:14px">'+s.v2.eventText+'</div></div>';
    }
    html += '<p style="font-size:13px;color:var(--faint)">Ties: VP, then planets, then Credits</p>';
    // 3.1-dev.103: How to Play + Guides live behind the gear now
    // 2.0-dev.13 (board O): leave for a while; the game carries on.
    html += '<button class="back" style="margin-top:22px" onclick="stepAway()">STEP AWAY</button>' +
            '<div style="font-size:13px;color:var(--faint)">The game goes on without you; come back any time.</div>';
    // v0.30.3: END GAME -- the table resumes a saved game when it boots, so this is how a game is abandoned.
    html += '<div style="margin-top:56px"><button class="back" style="border-color:var(--red);color:var(--red)" onclick="endGameTap()">END GAME</button></div>';
    if (endConfirm) {  // 2.0-dev.6: the "are you sure?" page
      html = '<div class="card" style="margin-top:40px;border-color:var(--red)"><div style="font-size:22px;letter-spacing:4px;color:var(--red)">END THE GAME?</div>' +
             '<p style="color:var(--dim)">This ends it for EVERYONE at the table. The saved game is erased &mdash; it can\'t be brought back.</p></div>' +
             '<button onclick="endGameKeep()">KEEP PLAYING</button>' +
             '<div style="margin-top:72px"><button class="back" style="border-color:var(--red);color:var(--red)" onclick="endGameGo()">END GAME</button></div>';
    }
    setApp(app, html);
    return;
  }

  // Home hub (v0.21.0 UI refresh; phone twin of the AMOLED's showHome()).
  // The planet list now lives behind the GALAXY tile (galaxyMode, below).
  if (!galaxyMode && lockConfirmPending) {
    // v0.21.1 lock-in confirm; 3.1: a SHEET over Home instead of its own screen.
    const l = lockLeftovers(s);
    let sh = '<div style="font-size:13px;letter-spacing:2px;color:var(--dim)">END YOUR '+s.phaseName+' PHASE?</div>';
    sh += '<div style="font-size:30px;font-weight:700;letter-spacing:4px;color:var(--cyan);margin:2px 0 6px">LOCK IN?</div>';
    sh += '<div class="card" style="text-align:center;font-size:17px;line-height:1.7;margin:8px 0"><div style="font-size:13px;color:var(--dim)">You still have</div>';
    if (l.credits) sh += '<div style="color:var(--accent)">' + l.credits + ' Credits unspent</div>';
    if (l.readyFleets.length) {  // "Fleet 2" / "Fleets 1 + 3" / "Fleets 1, 2 + 3"
      const n = l.readyFleets, list = n.length > 1 ? n.slice(0, -1).join(', ') + ' + ' + n[n.length - 1] : '' + n[0];
      sh += '<div style="color:var(--accent)">Fleet' + (n.length > 1 ? 's ' : ' ') + list + ' ready to fight</div>';
    }
    if (l.offers) sh += '<div style="color:var(--cyan)">A negotiation is still going</div>';
    if (l.deals) sh += '<div style="color:var(--red)">'+l.deals+' offer'+(l.deals === 1 ? '' : 's')+' unanswered</div>';
    sh += '</div>' +
          '<button class="pill" onclick="lockIn()">LOCK IN</button>';
    sh += '<button class="pill" style="border-color:var(--line);color:var(--dim)" onclick="cancelLockIn()">STAY</button>';
    sheetShow('lock', sh, 'var(--cyan)', cancelLockIn);
    lockSheetSeen = true;
  }
  if (attackPlayersMode && s.phase !== 2) attackPlayersMode = false;
  if (attackPlayersMode && s.phase === 2) { attackPlayersHtml(s, me, html); return; }  // dev.37b
  // UI 3.0 take 4 (user: "lets build both of those"): Prepare AND Resolution open on the hangar-style Home;
  // the one glowing Galaxy map serves every phase, and a world's page in Resolution holds the odds + actions.
  if (s.stage === 1 && !galaxyMode) { prepareHangar(s, me, html); return; }  // step 4: Movement too
  // Galaxy (v0.21.14; phone twin of the AMOLED's showPlanets): a map of the
  // physical board -- every planet at its board slot (GALAXY_POS), with the
  // ring routes, lanes, nebulae and faction hangars underneath, so the phone
  // shows exactly how the table is laid out. Planets not in this game are
  // empty slots. Border = holder's faction color; yours gets a tinted fill.
  // Only rebuilt when the HTML changes (Convention 8).
  const hints = ['Scout or fortify a planet', 'Move your fleets on the board', 'Choose a takeover target'];
  html += '<button class="back" onclick="closeGalaxy()">&lsaquo; Home</button>';
  html += '<div class="gxhead"><b>GALAXY</b><span>'+(hints[s.phase] || '')+' · '+me.credits+' CR</span></div>';
  // 3.1-dev.14 the map in a WINDOW across the middle of the screen: pinch / drag to zoom and pan, filters above, a list below.
  const marks = galaxyMarks(s);
  html += '<div class="gfilt">' + ['ALL', 'THREATS', 'CHANCES', 'MINE'].map((f, k) => '<button class="'+(gFilter === k ? 'on' : '')+'" onclick="setGFilter('+k+')">'+f+(k && marks[k].length ? ' <i>'+marks[k].length+'</i>' : '')+'</button>').join('') +
          '<button class="'+(gFilter === 4 ? 'on' : '')+'" onclick="openSideFilter()">'+(gFilter === 4 ? (gFaction < 4 ? FACTION_SHORT_NAMES[gFaction] : '?') : 'SIDE')+' \u25BE</button></div>';  // 3.1-dev.55
  const lit = gFilter ? new Set(marks[gFilter].map(m => m.i)) : null;
  // 3.1-dev.115 (user: "the zoomable map is no longer needed"): a fixed map across the whole width; the list below does the work.
  gView.s = 1; gView.x = gView.y = 0;
  html += '<div class="gwin" id="gwin"><div class="gpan" id="gpan">';
  const ashenOn = !!(s.v2 && s.v2.ashen && s.v2.ashen.alive);
  html += '<div class="gmap'+(lit ? ' filt' : '')+'" onclick="galaxyTap(event)">' + galaxyMapSvg(s);  // gap taps snap to the nearest planet (v0.21.15)
  if (ashenOn)  // 2.0: the flagship in the middle of the board, its HP under it
    html += '<button class="ashen" style="left:50%;top:50%" onclick="openAshen()" aria-label="The Ashen Fleet"><img class="pixel" src="'+spriteUrl('ashen', ASHEN_SPRITE, 4)+'" alt="">' +
            '<span class="hpbar" style="width:100%;height:4px"><i style="width:'+Math.round(100 * s.v2.ashen.hp / Math.max(1, s.v2.ashen.max))+'%"></i></span></button>';
  for (let i = 0; i < s.planets.length; i++) {
    const pos = galaxyPos(i), left = (50 + pos[0] / 4.66).toFixed(2) + '%', top = (50 + pos[1] / 4.66).toFixed(2) + '%';
    if (!s.planetIncluded[i]) { html += '<span class="gslot" style="left:'+left+';top:'+top+'"></span>'; continue; }
    const p = s.planets[i];
    const f = p.owner >= 0 ? s.players[p.owner].chosenFaction : -1;
    const mine = p.owner >= 0 && p.owner === mySlot;
    const ring = f >= 0 ? FACTION_RING_COLORS[f] : 'var(--line)';
    if (!galaxyImgs[i]) galaxyImgs[i] = bPlanetPixels(i, i * 37).toDataURL();  // same frozen step as the AMOLED
    const cls = (p.ring === 2 ? 'core' : '') + (lit ? (lit.has(i) ? ' lit' : ' dim') : '') + (s.v2 && s.v2.summit && s.v2.summit.w.includes(i) ? ' cf' : '');  // 3.1-dev.115 ceasefire
    html += '<button'+(cls ? ' class="'+cls.trim()+'"' : '')+' style="left:'+left+';top:'+top+';border-color:'+ring+';'+(f >= 0 ? 'border-width:'+(mine ? 5 : 4)+'px;box-shadow:0 0 10px '+ring+';background:'+ring+(mine ? '40' : '26')+';' : '')+'" onclick="gPick('+i+')" aria-label="'+p.name+'">' +
            '<img class="pixel" src="'+galaxyImgs[i]+'" alt=""><span class="nm">'+p.name+'</span></button>';
  }
  html += '</div></div></div>';
  // below the window: what the filter found (ALL: a short hint)
  // 3.1-dev.119 (user: "showing the list on the bottom only in the all page doesn't make sense ... I wish the same type of list was
  // on the other filters but not on the all"): THREATS / CHANCES / MINE / SIDE list their worlds in the detailed rows.
  if (gFilter) {
    const list = marks[gFilter];
    html += list.length ? gallRows(s, list) : '<p class="ghint">' + ['', 'Nothing threatens your worlds right now.', 'No openings you know of right now.', 'You hold no worlds yet.', 'You don\u2019t know any worlds on that side yet.'][gFilter] + '</p>';
  } else html += '<p class="ghint">Tap a world. The filters above list worlds with their details.</p>';
  setApp(app, html);
}
function gallRows(s, items) {  // 3.1-dev.119 one row per world: holder (+ why it's listed), VP, defense level, side
  const DV = ['LOW', 'MEDIUM', 'HIGH', 'FORTRESS'], DC = ['#8CE7A0', '#FFB347', '#FF6B5A', '#FF6B5A'];
  return '<div class="gall"><div class="gallH"><span>WORLD</span><span>VP</span><span>DEFENSE</span><span>SIDE</span></div>' + items.map(m => {
    const i = m.i, p = s.planets[i], o = p.owner, oc = o === mySlot ? 'var(--accent)' : o >= 0 && s.players[o].chosenFaction >= 0 ? FACTION_UI_COLORS[s.players[o].chosenFaction] : '#2A3646';
    const sc = p.chars[0], fi = sc.revealed ? AFFILIATION_TO_FACTION.indexOf(sc.value) : -1, lv = defLevel(p);
    return '<button onclick="gPick(' + i + ')"><span class="gw"><i style="background:' + oc + '"></i><span><b>' + p.name + '</b><small>' + (m.why ? String(m.why).replace(/<[^>]*>/g, '') : (o === mySlot ? 'yours' : o >= 0 ? whoName(s, o) : 'unclaimed')) + '</small></span></span>' +
           '<b class="gv">' + p.vp + '</b><b class="gd" style="color:' + DC[lv] + '">' + DV[lv] + '</b>' +
           '<i class="gs" style="' + (fi >= 0 ? 'background:' + FACTION_UI_COLORS[fi] : 'border:1.5px dashed #5C6F84') + '"></i></button>'; }).join('') + '</div>';
}
// ---- 3.1-dev.14 the map window: zoom, pan, filters, the world sheet ----
const gView = { s: 1, x: 0, y: 0 };
let gFilter = 0, gPtrs = {}, gGest = null, gMoved = false;
function setGFilter(k) { gFilter = k; if (lastS) render(lastS); }
// 3.1-dev.55 (playtest 3: "a faction filter"; a playtester hunting for a Rebel world for a job): pick a side -- the map lights
// the worlds whose Sympathy you know is theirs (or the ones you don't know yet).
function openSideFilter() {
  const s = lastS; if (!s) return;
  const count = f => s.planets.filter((p, i) => { if (!s.planetIncluded[i]) return false; const sym = p.chars && p.chars[0] && p.chars[0].revealed ? AFFILIATION_TO_FACTION.indexOf(p.chars[0].value) : -1; return f === 4 ? sym < 0 : sym === f; }).length;
  sheetShow('side', '<div class="sheetSub" style="letter-spacing:3px">THE MAP</div><div class="sheetTitle">WHOSE SIDE?</div><div class="sheetSub">Light up the worlds that side with a faction. Only the ones whose Sympathy you know (or hold).</div>' +
    [0, 1, 2, 3, 4].map(f => '<button class="gyrow" onclick="gFaction=' + f + ';gFilter=4;sheetHide();render(lastS)">' + (f < 4 ? insigniaSvg(f, 20) : pixIcon('holonet', '#8FA3B8', 2)) +
      '<span><b style="color:' + (f < 4 ? FACTION_UI_COLORS[f] : 'var(--dim)') + '">' + (f < 4 ? FACTION_SHORT_NAMES[f] : 'UNKNOWN') + '</b> ' + count(f) + ' world' + (count(f) === 1 ? '' : 's') + (f === 4 ? ' you haven\u2019t read yet' : '') + '</span><em></em></button>').join('') +
    '<button class="pill" style="border-color:var(--line);color:var(--dim)" onclick="sheetHide()">CLOSE</button>', '#5FD4FF');
}
function gApply() { const el = document.getElementById('gpan'); if (el) el.style.transform = 'translate(' + gView.x.toFixed(1) + 'px,' + gView.y.toFixed(1) + 'px) scale(' + gView.s.toFixed(3) + ')'; }
function gClamp() {
  const win = document.getElementById('gwin'); if (!win) return;
  const w = win.clientWidth, h = win.clientHeight;
  gView.s = Math.max(1, Math.min(3.2, gView.s));
  const mx = (w * gView.s - w) / 2, my = (h * gView.s - h) / 2;  // the layer scales about its center
  gView.x = Math.max(-mx, Math.min(mx, gView.x)); gView.y = Math.max(-my, Math.min(my, gView.y));
}
function gZoomAt(k, cx, cy) {  // zoom by k around a point (window coordinates, from its center)
  const s0 = gView.s, s1 = Math.max(1, Math.min(3.2, s0 * k)), r = s1 / s0;
  gView.x = cx - (cx - gView.x) * r; gView.y = cy - (cy - gView.y) * r; gView.s = s1;
  gClamp(); gApply();
}
function gZoomBy(k) { const el = document.getElementById('gpan'); if (el) el.classList.add('anim'); gZoomAt(k, 0, 0); setTimeout(() => { if (el) el.classList.remove('anim'); }, 260); }
function gFit() { const el = document.getElementById('gpan'); if (el) el.classList.add('anim'); gView.s = 1; gView.x = gView.y = 0; gApply(); setTimeout(() => { if (el) el.classList.remove('anim'); }, 260); }
function gCenter(ev) { const r = document.getElementById('gwin').getBoundingClientRect(); return [ev.clientX - r.left - r.width / 2, ev.clientY - r.top - r.height / 2]; }
function gPD(ev) {
  gPtrs[ev.pointerId] = gCenter(ev);
  const ids = Object.keys(gPtrs);
  if (ids.length === 1) { gMoved = false; gGest = { x0: gView.x, y0: gView.y, p0: gPtrs[ids[0]] }; }
  if (ids.length === 2) { const a = gPtrs[ids[0]], b = gPtrs[ids[1]]; gGest = { pinch: Math.hypot(a[0] - b[0], a[1] - b[1]), s0: gView.s, m: [(a[0] + b[0]) / 2, (a[1] + b[1]) / 2], x0: gView.x, y0: gView.y }; gMoved = true; }
}
function gPM(ev) {
  if (!(ev.pointerId in gPtrs) || !gGest) return;
  gPtrs[ev.pointerId] = gCenter(ev);
  const ids = Object.keys(gPtrs);
  if (ids.length >= 2 && gGest.pinch) {
    const a = gPtrs[ids[0]], b = gPtrs[ids[1]], d = Math.hypot(a[0] - b[0], a[1] - b[1]);
    const s1 = Math.max(1, Math.min(3.2, gGest.s0 * d / Math.max(1, gGest.pinch))), r = s1 / gGest.s0;
    gView.s = s1; gView.x = gGest.m[0] - (gGest.m[0] - gGest.x0) * r; gView.y = gGest.m[1] - (gGest.m[1] - gGest.y0) * r;
  } else if (ids.length === 1 && gGest.p0) {
    const p = gPtrs[ids[0]], dx = p[0] - gGest.p0[0], dy = p[1] - gGest.p0[1];
    if (Math.abs(dx) + Math.abs(dy) > 8) gMoved = true;
    if (gMoved) { gView.x = gGest.x0 + dx; gView.y = gGest.y0 + dy; }
  }
  gClamp(); gApply();
}
function gPU(ev) { delete gPtrs[ev.pointerId]; if (!Object.keys(gPtrs).length) { gGest = null; setTimeout(() => { gMoved = false; }, 30); } }
function gWheel(ev) { ev.preventDefault(); const c = gCenter(ev); gZoomAt(ev.deltaY < 0 ? 1.15 : 1 / 1.15, c[0], c[1]); }
// What the filters find: [ALL (unused), THREATS, CHANCES, MINE] lists of { i, why }.
let gFaction = 0;  // 3.1-dev.55 the SIDE filter: 0-3 a faction, 4 = Sympathy unknown
function galaxyMarks(s) {
  const v = s.v2 || {}, out = [[], [], [], [], []];
  s.planets.forEach((p, i) => {
    if (!s.planetIncluded[i]) return;
    const mine = p.owner === mySlot;
    const sym = p.chars && p.chars[0] && p.chars[0].revealed ? AFFILIATION_TO_FACTION.indexOf(p.chars[0].value) : -1;
    if (sym === gFaction || (gFaction === 4 && sym < 0))  // 3.1-dev.55 SIDE
      out[4].push({ i: i, why: (mine ? 'yours' : p.owner >= 0 ? 'held by ' + feedWho(s, p.owner) : 'unclaimed') + ' \u00b7 ' + (['Outer Rim', 'Mid Rim', 'the Core'][p.ring] || '') });
    if (mine) {
      out[3].push({ i: i, why: 'Worth ' + p.vp + ' VP \u00b7 defenders ' + (p.defense || 0) });
      const t = [];
      if (v.war && v.war.w === i) t.push('war is coming (' + feedWho(s, v.war.by) + ')');
      if (v.off && v.off.w === i) t.push('the ' + FNAMES[v.off.f] + ' offensive, next round');
      if (v.grudge && v.grudge.w === i) t.push('the ' + FNAMES[v.grudge.f] + ' strike here next round');
      if ((p.defense || 0) <= 2) t.push('thin defenses (' + (p.defense || 0) + ')');
      if (t.length) out[1].push({ i: i, why: t.join(' \u00b7 ') });
    } else {
      const c = [];
      if (v.race && v.race.w === i) c.push('race: the ' + FNAMES[v.race.f] + '\u2019s gratitude');
      if (v.race && v.race.w === -2 && sym === (v.race.f ^ 1)) c.push('the desperate ' + FNAMES[v.race.f] + ' pay big for it');
      if (v.job && v.job.w === i) c.push('your job for the ' + FNAMES[v.job.f]);
      if (p.owner < 0 && sym >= 0 && tierIdx(s, sym) >= 3) c.push('the ' + FNAMES[sym] + ' like you: easier talks, cheaper Pay');
      if (v.war && v.war.w === i && v.war.by === mySlot) c.push('your war target');
      if (p.owner >= 0 && s.players[p.owner] && s.players[p.owner].commander % 4 === 3) c.push('a Scoundrel\u2019s world: +3 CR bounty');  // 3.1-dev.78
      if (c.length) out[2].push({ i: i, why: c.join(' \u00b7 ') });
    }
  });
  return out;
}
// Tap a world: the map stays behind and its sheet slides up.
function gPick(i) {
  if (gMoved) return;
  if (lastS && lastS.planets[i]) { openPlanet(i); return; }  // 3.1-dev.17: one world view -- the page slides up
  const s = lastS; if (!s || !s.planets[i]) return;
  const p = s.planets[i], f = p.owner >= 0 && s.players[p.owner] ? s.players[p.owner].chosenFaction : -1;
  const KEYS = ['SYMPATHY', 'RESOURCES', 'TRAIT', 'DEFENSE'];
  const tiles = [1, 2, 0, 3].map(c => { const ch = p.chars && p.chars[c], fx = c === 2 && p.traitFx;  // 3.1-dev.17: public on top, secrets below
    return '<div class="wtile' + (ch && ch.revealed ? '' : ' hid') + (fx ? ' tap' : '') + '"' + (fx ? ' onclick="this.classList.toggle(\x27open\x27)"' : '') + '><small>' + (c === 3 ? defKey(p) : KEYS[c]) + (fx ? ' \u24D8' : '') + '</small><b>' + (ch && ch.revealed ? (c === 3 ? defVal(p, ch.value) : ch.value) : '???') + '</b>' + (fx ? '<em>Hold it: ' + p.traitFx + '</em>' : '') + '</div>'; }).join('');
  const marks = galaxyMarks(s), why = [1, 2].map(k => (marks[k].find(m => m.i === i) || {}).why).filter(Boolean);
  if (!galaxyImgs[i]) galaxyImgs[i] = bPlanetPixels(i, i * 37).toDataURL();
  sheetShow('world', '<div class="wsTop"><img class="pixel" src="' + galaxyImgs[i] + '" alt=""><div><div class="sheetTitle" style="color:var(--text)">' + wName(s, i).toUpperCase() + '</div>' +
    '<div class="sheetSub">' + (p.owner === mySlot ? 'Yours \u00b7 defenders ' + (p.defense || 0) : p.owner >= 0 ? 'Held by ' + feedWho(s, p.owner) : 'Unclaimed') + ' \u00b7 worth ' + p.vp + ' VP' + (p.ring === 2 ? ' \u00b7 Core' : p.ring === 1 ? ' \u00b7 Mid Rim' : ' \u00b7 Outer Rim') + '</div></div></div>' +
    '<div class="wtiles">' + tiles + '</div>' + (why.length ? '<div class="debtnote">' + pixIcon('target', '#FFD166', 2) + '<span>' + why.join(' \u00b7 ') + '</span></div>' : '') +
    '<button class="pill" onclick="sheetHide();openPlanet(' + i + ')">OPEN ' + wName(s, i).toUpperCase() + '</button>', f >= 0 ? FACTION_RING_COLORS[f] : '#5FD4FF');
}

// v0.21.14 board map (px from the center of a 466px round screen, +y down).
// 2.0 17 worlds (the design notes "DECISIONS 2026-11-01", take 4): Core 56 / Mid 124 /
// Outer 190. 2026-11-11 (the board's charm pass): Mid Rim at 45 deg steps (Naboo, Endor, Nal Hutta, Canto Bight
// moved), SPACE STATIONS on the Mid Rim east + west, the faction hangars are gone (start on your home world).
// The .ino's GALAXY_POS still has the 12-world map (AMOLED_TODO.md).
const GALAXY_POS = [[-59,181],[59,-181],[0,124],[59,181],[0,-56],[-59,-181],[-154,112],[-154,-112],[-48,28],[88,88],[88,-88],[0,-124],[154,112],[154,-112],[-88,88],[-88,-88],[48,28]];
const GALAXY_LANES = [[3,2],[0,2],[5,11],[1,11],[12,9],[6,14],[7,15],[13,10],[11,4],[2,8],[2,16]];
const GALAXY_STATIONS = [[124,0],[-124,0]];  // SPACE STATIONS: a fleet with no worlds launches here
const GALAXY_NEBULAE = [[190,0],[-190,0]];  // The Maw (E), the Unknown Regions (W)
function galaxyPos(i) { return GALAXY_POS[i]; }
// v0.21.15: a tap on the map that misses every circle (a gap, a lane) opens
// the nearest planet in play within ~45 of the 466 map units -- the phone
// twin of the AMOLED's galaxyNearest(). Direct taps on a circle are handled
// by its own button.
function galaxyTap(ev) {
  if (ev.target.closest('button') || gMoved) return;
  const box = ev.currentTarget.getBoundingClientRect();
  const x = (ev.clientX - box.left) / box.width * 466 - 233, y = (ev.clientY - box.top) / box.height * 466 - 233;
  let best = -1, bestD = 45 * 45;
  const ashen = !!(lastS && lastS.v2 && lastS.v2.ashen && lastS.v2.ashen.alive);
  for (let i = 0; i < GALAXY_POS.length; i++) {
    if (!lastS || !lastS.planetIncluded[i]) continue;
    const pos = galaxyPos(i), dx = x - pos[0], dy = y - pos[1], d = dx * dx + dy * dy;
    if (d <= bestD) { best = i; bestD = d; }
  }
  if (ashen && x * x / 4 + y * y <= 30 * 30 && (best < 0 || x * x / 4 + y * y < bestD)) { openAshen(); return; }
  if (best >= 0) gPick(best);
}
function galaxyMapSvg(s) {
  const out = galaxyMapSvgRaw(s), e = eraOf(s);
  return e ? out.split('#5FD4FF').join(ERAS[e.f].line) : out;  // 3.1-dev.12 Era weather: the map's own lines change color
}
function galaxyMapSvgRaw(s) {
  let g = '<svg class="gsvg" viewBox="-233 -233 466 466" aria-hidden="true">';
  g += '<circle r="230" fill="#0B1420"/>';
  [190, 124].forEach(r => { g += '<circle r="'+r+'" fill="none" stroke="#5FD4FF" stroke-opacity="0.3" stroke-width="3"/>'; });
  g += '<circle r="56" fill="none" stroke="#FFD166" stroke-opacity="0.5" stroke-width="3"/>';
  GALAXY_NEBULAE.forEach((n, k) => { g += '<ellipse cx="'+n[0]+'" cy="0" rx="34" ry="22" fill="#9B5FE0" fill-opacity="0.22"/>' +
                                     '<ellipse cx="'+n[0]+'" cy="0" rx="20" ry="12" fill="#5FD4FF" fill-opacity="0.12"/>' +
    // 3.1-dev.57 (user: "the nebulae are stops"): a stop on the Outer Rim, like on the board -- a ring you can end a move in
    '<circle cx="'+n[0]+'" cy="0" r="11" fill="#0B1420" stroke="#B98CF0" stroke-width="2.5" stroke-dasharray="4 3"/><circle cx="'+n[0]+'" cy="0" r="3" fill="#B98CF0"/>' +
    '<text x="'+(n[0] > 0 ? n[0] - 6 : n[0] + 6)+'" y="38" text-anchor="middle" font-size="11" letter-spacing="1.5" fill="#B98CF0" font-family="sans-serif">'+(k ? 'UNKNOWN' : 'THE MAW')+'</text>'; });
  GALAXY_LANES.forEach(l => { const a = galaxyPos(l[0]), b = galaxyPos(l[1]);
    g += '<line x1="'+a[0]+'" y1="'+a[1]+'" x2="'+b[0]+'" y2="'+b[1]+'" stroke="#5FD4FF" stroke-width="3" stroke-opacity="0.8"/>'; });
  GALAXY_STATIONS.forEach(st => {   // a docking ring with three arms, like the engraving on the board
    g += '<g transform="translate('+st[0]+' '+st[1]+')" stroke="#5FD4FF" stroke-width="2.5" fill="#0B1420">' +
         '<circle r="15"/><circle r="8" fill="none"/>' + [30, 150, 270].map(a => '<line x1="'+(8*Math.cos(a*Math.PI/180)).toFixed(1)+'" y1="'+(8*Math.sin(a*Math.PI/180)).toFixed(1)+
         '" x2="'+(15*Math.cos(a*Math.PI/180)).toFixed(1)+'" y2="'+(15*Math.sin(a*Math.PI/180)).toFixed(1)+'"/>').join('') + '<circle r="2.5" fill="#5FD4FF" stroke="none"/></g>'; });
  return g + '</svg>';
}

// 2.0-dev.19: /state sends planets compact (see sendStateJson); this rebuilds
// the shape every screen reads -- p.chars[c] = {revealed, value, cost},
// p.good / p.goodFx, and true/false flags. A state without goodTab (the phone
// harness's fake state) is left as it is.
function expandState(s) {
  if (!s || !s.planets || !s.goodTab) return s;
  for (const p of s.planets) {
    const g = s.goodTab[p.g] || ['', ''], rv = p.rv || 0, cv = p.cv || [], cc = p.cc || [];
    p.good = g[0]; p.goodFx = g[1]; p.goodId = g[2] === undefined ? -1 : g[2]; p.goodBase = g[3] || 0;  // dev.21
    p.locked = !!p.locked; p.mOpen = !!p.mOpen; p.mBuy = !!p.mBuy; p.mShut = !!p.mShut; p.unbound = !!p.unbound; p.dipl = p.dipl !== 0;
    p.chars = [0, 1, 2, 3].map(c => ({ revealed: !!(rv & (1 << c)), value: cv[c] || '', cost: cc[c] }));
  }
  return s;
}
// v0.21.12: polls overlap (the 700ms timer plus one after every action), and
// an older /state answer arriving after a newer one used to repaint stale
// state -- e.g. "LOCK IN" again right after locking. Only the newest
// request's answer (or a later one) is ever applied.
let pollSeq = 0, pollApplied = 0;
let pollFails = 0, pollWaitUntil = 0;  // 3.1-dev.74: a table that restarts or runs low on RAM isn't hit by every phone at once
async function poll() {
  const mySeq = ++pollSeq;
  if (Date.now() < pollWaitUntil) return;  // 3.1-dev.74: backing off after a failed / busy answer
  let s = await api('/state?id=' + encodeURIComponent(myId) + (lastS && lastS.h ? '&h=' + lastS.h : '') + feedQuery());
  if (!s || s.busy) { pollFails++; pollWaitUntil = Date.now() + Math.min(5000, 400 * pollFails) + Math.random() * 1500; return; }
  pollFails = 0;
  if (mySeq < pollApplied) return;  // superseded by a newer answer already shown
  if (s.same) { if (!lastS) return; s = lastS; }  // 2.0-dev.15: nothing changed -- the table sent a few bytes, not ~11 KB
  else { expandState(s); lastChangeAt = Date.now(); }  // 2.0-dev.19: compact planets -> the shape the screens use (3.1-dev.118: the table changed)
  pollApplied = mySeq;
  if (s.firmwareVersion && s.firmwareVersion !== PAGE_FW && !/harness/.test(s.firmwareVersion)) {  // 3.1-dev.44: a new flash -- fetch the new page (once per version)
    let tried = ''; try { tried = sessionStorage.getItem('sfReload') || ''; } catch (e) {}
    const now = Date.now(), last = +(tried.split('@')[1] || 0);  // 3.1-dev.107: version@time -- retry every 20 s, not once
    if (tried.split('@')[0] !== s.firmwareVersion || now - last > 20000) { try { sessionStorage.setItem('sfReload', s.firmwareVersion + '@' + now); } catch (e) {}
      if (!window.sfReloading) { window.sfReloading = 1; setTimeout(() => location.reload(), 300 + Math.random() * 3000); } return; }  // staggered: phones don't all reload at once
  }
  if (s.yourSlot !== undefined && s.yourSlot >= 0) { mySlot = s.yourSlot; joined = true; }
  // v0.27.3: NEW GAME emptied every seat -- back to the seat picker.
  else if (s.yourSlot === -1 && joined && s.stage === 0) { joined = false; mySlot = -1; shownHtml = null; }
  feedMerge(s);  // 3.1-dev.3
  lastS = s;
  render(s);
  if (s.stage === 1 && (joined || spectating)) {  // 3.1-dev.52 THE GALLERY; 3.1-dev.54 attack alerts (+ spectators)
    galleryNotify(s); battleAlert(s);
    if (WATCH.by >= 0 && sheetKey === 'watch') openWatch(WATCH.by);
    if (watchBattle && !s.battleActive) watchBattle = '';
  }
  maybeBanner(s);
  setTimeout(() => rivetIntro(lastS), 600);  // 3.1-dev.81 first game: one introduction a phase
  bmHintSync();  // 3.1-dev.84
  // Never replace an overlay the player is still reading: a normal
  // transmission is acked the moment it's shown, and the server then
  // immediately queues the next due obligation check-in (v0.19.5) -- it
  // waits here until this one is dismissed.
  // v0.21.13: also hold it while you're in a battle or its report -- the
  // report shows an INCOMING TRANSMISSION chip and it plays after CONTINUE.
  const inMyBattle = s.battleActive && (s.battleAttackerSlot === mySlot || s.battleDefenderSlot === mySlot);
  // v0.30.0: and while a conversation (or its result card) is up -- the purchase card too.
  const inNeg = s.takeoverPlayer === mySlot && s.takeoverMethod === 2 && s.neg;
  const txOverlayOpen = document.getElementById('txOverlay').classList.contains('active') || bannerOpen() || victory || inMyBattle || inNeg || purchased;
  if (s.transmissionPending && !txAckInFlight && !txObligationShowing && !txOverlayOpen) {
    if (s.transmissionIsObligation) {
      // Do NOT ack yet -- gameStateResolveObligation() (triggered by the
      // YES/NO buttons) needs the server's stored obligation index to still
      // be set when the player answers. txObligationShowing guards against
      // re-triggering this every 700ms poll while it's awaiting an answer.
      txObligationShowing = true;
      showTransmission(s.transmissionFaction, s.transmissionQuote, true, -1, -1, false);
    } else {
      txAckInFlight = true;
      showTransmission(s.transmissionFaction, s.transmissionQuote, false, s.txFrom, s.txTo, s.txBig, s.txCause, s.txDelta, s.txAlso);
      await post('/transmission_ack?id=' + encodeURIComponent(myId) + (s.txAlso ? '&n=2' : ''));  // 3.1-dev.75: both factions' reactions, one screen
      txAckInFlight = false;
    }
  }
}
// 3.1-dev.118 BATTERY + HEAT: ask the table every 0.7 s while something is happening (you touched the phone in the last 20 s,
// the game changed in the last 15 s, a battle or the finale is on); every 1.5 s when the table is quiet; every 5 s with the
// screen hidden. CALM: after 25 s without a touch every looping animation pauses (lights stay lit), until the next touch.
let lastTouchAt = Date.now(), lastChangeAt = Date.now();
function pollGap() {
  if (document.hidden) return 5000;
  const s = lastS, now = Date.now();
  if (now - lastTouchAt < 20000 || now - lastChangeAt < 15000) return 700;
  if (s && (s.battleActive || (s.fh && s.fh.show >= 0) || s.stage !== 1)) return 700;
  return 1500;
}
function pollLoop() { poll(); setTimeout(pollLoop, pollGap()); }
setTimeout(pollLoop, 700);
function wake() { lastTouchAt = Date.now(); document.body.classList.remove('calm'); }
['touchstart', 'pointerdown', 'keydown', 'wheel'].forEach(ev => document.addEventListener(ev, wake, { passive: true, capture: true }));
document.addEventListener('visibilitychange', () => { if (!document.hidden) { wake(); poll(); } });
setInterval(() => { if (Date.now() - lastTouchAt > 25000 && Date.now() - lastChangeAt > 8000) document.body.classList.add('calm'); }, 5000);
poll();
</script>
</body>
</html>
)HTML";
static bool page_gz_ok = false;  // 3.1-dev.35: sf_page_gz.h matches PAGE_HTML (3.1-dev.87: checked at compile time, below)
// 3.1-dev.87 (user: "if you want to work on the flash space now ... lets go for it"): PAGE_HTML stays in the SOURCE (make_page_gz.py and the
// phone harness read it from here) but nothing uses it at run time, so it isn't linked into flash -- only sf_page_gz.h is. This check is
// the safety net that used to be the plain fallback: a page edited without re-running the gzip tool fails the BUILD.
static_assert(sizeof(PAGE_HTML) - 1 == SF_PAGE_SRC_LEN, "The phone page changed: run  python tools/page/make_page_gz.py  and build again.");
// 3.1-dev.112 (a version bump without re-running the script left the page saying dev.111 on a dev.112 table: every phone
// reloaded every 20 s, replaying the round banner): the page's version must match the firmware's.
static constexpr bool sfStrEq(const char *a, const char *b) { return *a == *b && (*a == 0 || sfStrEq(a + 1, b + 1)); }
static_assert(sfStrEq(SF_PAGE_FW, FIRMWARE_VERSION), "The version changed: run  python tools/page/make_page_gz.py  and build again.");
static String page_etag, art_etag;

// Resolve the slot for a request's ?id= within a snapshot, or -1.
static int slotForIdIn(const GameState &s, const String &id) {
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (s.players[i].active && id == s.players[i].id) return i;
  }
  return -1;
}

// Resolve the slot for a request's ?id=, or -1. 2.0-dev.13: every request
// from a seated device also says "still here" (see gameStatePresent).
// 2.0-dev.15: the web handlers (all on loopTask, one at a time) read the state
// into ONE copy kept in PSRAM, not on the 16 KB stack: sendStateJson held two
// ~5 KB copies there and a player coming back from AWAY overflowed it (found
// 2026-11-03 by a RAM test: the table crash-looped, the saved game resuming
// into the same crash).
static GameState &webState() {
  static GameState *gs = nullptr;
  if (!gs) {
    void *p = heap_caps_malloc(sizeof(GameState), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = malloc(sizeof(GameState));
    gs = new (p) GameState();
  }
  return *gs;
}
static String jsonStr(const char *in);  // defined below
static bool sendBounded(const char *type, const uint8_t *data, size_t len, bool gz);  // 3.1-dev.62, defined below
// 3.1-dev.3: the event log's copy for the answer (PSRAM, never the stack).
static EventLog &webEvents() {
  static EventLog *ev = nullptr;
  if (!ev) {
    void *p = heap_caps_malloc(sizeof(EventLog), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = malloc(sizeof(EventLog));
    ev = new (p) EventLog();
  }
  return *ev;
}
// "ev":{g,ps,ms,p:[...],m:[...]} -- only events newer than what this phone has
// (es / em / eg in the query); a phone with an old epoch gets everything.
// 3.1-dev.88 the black market's lots + your CARGO HOLD (open in the Final Hour or the Dawn's Era; the hold shows whenever it holds something)
static void appendBmJson(String &j, const GameState &s, int slot) {
  if (slot < 0 || slot >= MAX_PLAYERS || s.stage != STAGE_PLAYING) return;
  const Player &me = s.players[slot];
  const bool open = snapshotBmOpen(s);
  bool any = false; for (int k = 0; k < LOT_HOLD; k++) if (me.hold[k] >= 0) any = true;
  if (!open && !any) return;
  j += "\"bm\":{\"o\":" + String(open ? 1 : 0) + ",\"lots\":[";
  for (int k = 0; k < LOT_OFFERS; k++) j += String(k ? "," : "") + String(open ? me.lotOff[k] : -1);
  j += "],\"got\":" + String(me.lotsGot) + ",\"hold\":[";
  for (int k = 0; k < LOT_HOLD; k++) j += String(k ? "," : "") + String(me.hold[k]);
  j += "]},";
}
// 3.1-dev.43 THE FINAL HOUR: your gambits, debts, last orders, the reveal and the finale's log (only near the end).
static void appendFhJson(String &j, const GameState &s, int slot) {
  if (slot < 0 || slot >= MAX_PLAYERS || s.fhStage == 0) return;
  const Player &me = s.players[slot];
  auto arr = [&](const int8_t *a, int n) { String o = "["; for (int k = 0; k < n; k++) { if (k) o += ","; o += String(a[k]); } return o + "]"; };
  j += "\"fh\":{\"st\":" + String(s.fhStage) + ",\"hand\":" + String(me.fhHand) + ",\"bm\":" + arr(me.fhBm, FH_BM_OFFERS) +
       ",\"bmCr\":" FH_STR(FH_BM_CR) ",\"jump\":" FH_STR(FH_JUMP_CR) ",\"called\":" + String(me.fhCalled) +
       ",\"big\":" + String(me.bigOwned) + ",\"bigArg\":" + String(me.bigArg) + ",\"bigOff\":" + arr(me.bigOff, 2);  // 3.1-dev.53 (3.1-dev.88: the lots moved to "bm")
  if (me.fhDealN) j += ",\"deal\":" + arr(me.fhDeal, me.fhDealN);
  j += ",\"ally\":[" + String(me.fhAllyFrom[0]) + "," + String(me.fhAllyFrom[1]) + "," + String(me.fhAllyFrom[2]) + "," + String(me.fhAllyFrom[3]) + "]";
  j += ",\"debt\":[";
  for (int f = 0; f < NUM_FACTIONS; f++) { int c = 0; const int n = snapshotFhDebt(s, slot, f, &c); j += String(f ? "," : "") + "[" + String(n) + "," + String(c) + "]"; }
  j += "],\"ships\":[" + String(snapshotFhShips(s, slot, 0)) + "," + String(snapshotFhShips(s, slot, 1)) + "," + String(snapshotFhShips(s, slot, 2)) + "]";
  uint32_t reach = 0;
  for (int p = 0; p < NUM_PLANETS; p++) if (snapshotFhReach(s, slot, p)) reach |= 1u << p;
  j += ",\"reach\":" + String(reach);
  int8_t a8[FH_STRIKES], h8[FH_STRIKES];
  for (int k = 0; k < FH_STRIKES; k++) { a8[k] = (int8_t)(me.ordAlly[k] & 15); h8[k] = (int8_t)(me.ordAlly[k] >> 4); }  // 3.1-dev.116 + hired
  j += ",\"ord\":{\"t\":" + arr(me.ordT, FH_STRIKES) + ",\"gu\":" + arr(me.ordGuard, 3) + ",\"al\":" + arr(a8, FH_STRIKES) + ",\"h\":" + arr(h8, FH_STRIKES) + ",\"g\":" + arr(me.ordG, FH_STRIKES) + "}";
  j += ",\"hire\":[" + String(FH_HIRE_CR) + "," + String(FH_HIRE_MAX) + "]";
  j += ",\"rv\":[";
  for (int k = 0; k < FH_HAND; k++) j += String(k ? "," : "") + "[" + String(me.rvG[k]) + "," + String(me.rvA[k]) + "," + String(me.rvB[k]) + "]";
  j += "]";
  if (snapshotFhFinal(s) && s.phase == PHASE_RESOLUTION && s.fhStage == 1) {  // THE REVEAL: every strike (a Ghost Strike only to its owner)
    const bool spy = ((me.fhHand >> G_INTERCEPT) & 1) || me.bigOwned == BIG_SPY;  // 3.1-dev.53 the SPY NETWORK sees what Intercepted Orders sees
    j += ",\"strikes\":[";
    bool first = true;
    String pins = "";
    for (int i = 0; i < MAX_PLAYERS; i++) {
      const Player &pl = s.players[i];
      if (!pl.active) continue;
      int capK = -1, capN = -1;  // 3.1-dev.53 the CAPITAL SHIP rides with the biggest strike (shown at the reveal -- it was announced anyway)
      for (int k = 0; pl.bigOwned == BIG_CAPITAL && k < FH_STRIKES; k++) {
        if (pl.ordT[k] < 0) continue;
        const int m = (k < NUM_FLEETS ? snapshotFhShips(s, i, k) : k == 3 ? FH_SMUGGLER_SHIPS : BIG_ARMADA_SHIPS) + (pl.ordAlly[k] & 15) + (pl.ordAlly[k] >> 4);
        if (m > capN) { capN = m; capK = k; }
      }
      for (int k = 0; k < FH_STRIKES; k++) {
        const int n = (k < NUM_FLEETS ? snapshotFhShips(s, i, k) : k == 3 ? FH_SMUGGLER_SHIPS : BIG_ARMADA_SHIPS) + (pl.ordAlly[k] & 15) + (pl.ordAlly[k] >> 4) + (k == capK ? BIG_CAPITAL_SHIPS : 0);
        if (pl.ordT[k] < 0 || (k < NUM_FLEETS && snapshotFhShips(s, i, k) == 0)) continue;
        const bool ghost = pl.ordG[k] == G_GHOST;
        if (ghost && i != slot && !spy) continue;
        j += String(first ? "" : ",") + "[" + String(i * FH_STRIKES + k) + "," + String(i) + "," + String(pl.ordT[k]) + "," + String(n) + "," + String(ghost ? 1 : 0) + "]";
        first = false;
        if ((spy || i == slot) && pl.ordG[k] >= 0) pins += String(pins.length() ? "," : "") + "[" + String(i * FH_STRIKES + k) + "," + String(pl.ordG[k]) + "]";
      }
    }
    j += "],\"pins\":[" + pins + "]";
    int guard[NUM_PLANETS] = {};  // your own guards, so you see what defends your worlds
    for (int k = 0; k < NUM_FLEETS; k++) if (me.ordT[k] < 0 && me.ordGuard[k] >= 0) guard[me.ordGuard[k]] += snapshotFhShips(s, slot, k) + (me.ordAlly[k] & 15);
    j += ",\"guard\":[";
    for (int p = 0; p < NUM_PLANETS; p++) j += String(p ? "," : "") + String(guard[p]);
    j += "]";
  }
  if (s.fhStage == 2) j += ",\"show\":" + String(gameStateFhShowStep()) + ",\"sk\":" + String(gameStateFhSkipMask()) + ",\"left\":" + String(gameStateFhStepLeftMs());  // 3.1-dev.117
  if (s.fhStage >= 2 && s.fhLogN) {
    static const char HX[] = "0123456789abcdef";
    j += ",\"log\":\"";
    for (int k = 0; k < s.fhLogN; k++) { j += HX[s.fhLog[k] >> 4]; j += HX[s.fhLog[k] & 15]; }
    j += "\"";
  }
  j += "},";
}
static void appendEventsJson(String &j, int slot) {
  EventLog &ev = webEvents();
  gameStateEventsInto(ev);
  const bool fresh = !server.hasArg("eg") || (uint16_t)server.arg("eg").toInt() != ev.epoch;
  const uint16_t es = fresh ? 0 : (uint16_t)server.arg("es").toInt();
  const uint16_t em = fresh ? 0 : (uint16_t)server.arg("em").toInt();
  j += "\"ev\":{\"g\":" + String(ev.epoch) + ",\"ps\":" + String(ev.pubSeq);
  j += ",\"ms\":" + String(slot >= 0 ? ev.mineSeq[slot] : 0) + ",\"p\":[";
  bool first = true;
  for (int k = 1; k <= EV_PUB_N; k++) {  // oldest first
    const GameEvent &e = ev.pub[(ev.pubSeq + k) % EV_PUB_N];
    if (!e.seq || (uint16_t)(e.seq - es) > 0x7FFF || e.seq == es) continue;
    if (!first) j += ","; first = false;
    j += "[" + String(e.seq) + "," + String(e.kind) + "," + String(e.round) + "," + String(e.who) + "," + String(e.planet) + "," + String(e.other) + "," + String(e.num) + "]";
  }
  j += "],\"m\":[";
  first = true;
  if (slot >= 0) for (int k = 1; k <= EV_MINE_N; k++) {
    const GameEvent &e = ev.mine[slot][(ev.mineSeq[slot] + k) % EV_MINE_N];
    if (!e.seq || (uint16_t)(e.seq - em) > 0x7FFF || e.seq == em) continue;
    if (!first) j += ","; first = false;
    j += "[" + String(e.seq) + "," + String(e.kind) + "," + String(e.round) + "," + String(e.who) + "," + String(e.planet) + "," + String(e.other) + "," + String(e.num);
    if (e.kind == LOG_NOTE && e.num >= 0 && e.num < EV_NOTE_N) j += "," + jsonStr(ev.notes[slot][e.num]);
    j += "]";
  }
  j += "]},";
}

static int slotForId(const String &id) {
  GameState &s = webState();
  gameStateReadInto(s);
  const int slot = slotForIdIn(s, id);
  if (slot >= 0) gameStatePresent(slot);
  return slot;
}

// A complete, quoted JSON string literal. Escapes backslash, double quote,
// and every control character, per RFC 8259 -- the old per-field
// replace("\"", "'") missed backslashes/control chars (which break
// JSON.parse on every phone at once) and several fields had no escaping.
static String jsonStr(const char *in) {
  String out = "\"";
  for (const char *p = in ? in : ""; *p; p++) {
    char c = *p;
    if (c == '"') out += "\\\"";
    else if (c == '\\') out += "\\\\";
    else if ((unsigned char)c < 0x20) {
      char esc[8];
      snprintf(esc, sizeof(esc), "\\u%04x", (unsigned char)c);
      out += esc;
    } else out += c;
  }
  out += "\"";
  return out;
}

// Everything below derives from ONE snapshot `s`, via the pure snapshot*
// helpers -- never the locking gameState* getters, which would re-read live
// state (possibly mid-change) ~30 times per poll per phone.
static void sendStateJson() {
  GameState &s = webState();  // 2.0-dev.15: in PSRAM, not on the stack
  gameStateReadInto(s);
  int slot = slotForIdIn(s, server.arg("id"));
  if (slot >= 0 && !server.hasArg("peek")) {  // 2.0-dev.13: polling = still here (the twin host's "peek" for remote seats isn't)
    gameStatePresent(slot);
    if (s.players[slot].away == AWAY_QUIET) gameStateReadInto(s);
  }

  String j;
  j.reserve(STATE_JSON_RESERVE);  // 2.0-dev.15: one PSRAM block (> 4 KB), not a realloc chain through internal RAM
  j += "{";
  j += "\"firmwareVersion\":\"" FIRMWARE_VERSION "\",";
  // v0.22.5: which Wi-Fi the host is on, so the phone Lobby can offer /setup.
  j += "\"net\":{\"home\":" + String(net_home ? "true" : "false") + ",\"canHome\":" + String(net_allow_home ? "true" : "false") +
       ",\"ssid\":" + jsonStr(net_ssid) + "},";
  j += "\"stage\":" + String(s.stage) + ",";
  j += "\"phase\":" + String(s.phase) + ",";
  j += "\"phaseName\":\"" + String(PHASE_NAMES[s.phase]) + "\",";
  j += "\"round\":" + String(s.round) + ",";
  j += "\"roundLimit\":" + String(s.roundLimit) + ",";
  j += "\"mkt\":" + String(s.marketsOn ? 1 : 0) + ",";  // 2.0-dev.32 the Lobby's markets switch
  j += "\"evt\":" + String(s.eventsOn ? 1 : 0) + ",";   // 2.0-dev.33 ... and the events switch
  if (s.stage == STAGE_PLAYING && s.phase == PHASE_MOVEMENT) {  // UI 3.0 step 4: the Holonet rumor
    char rumor[96]; int rp = -1; snapshotRumor(s, rumor, sizeof(rumor), &rp);
    j += "\"rumor\":" + jsonStr(rumor) + ",\"rumorP\":" + String(rp) + ",";
    if (snapshotEra(s) == FACTION_CRIMSON_DAWN && s.lieRound == s.round && s.lie[0]) j += "\"lie\":" + jsonStr(s.lie) + ",";  // 3.1-dev.12 the Shadow Age lie
    {  // 3.1-dev.13: stories planted under deep cover air like rumors (never says who)
      bool first = true;
      for (int i = 0; i < MAX_PLAYERS; i++)
        if (s.players[i].storyRound == s.round && s.players[i].story[0]) { j += String(first ? "\"stories\":[" : ",") + jsonStr(s.players[i].story); first = false; }
      if (!first) j += "],";
    }
  }
  j += "\"yourSlot\":" + String(slot) + ",";
  appendEventsJson(j, slot);  // 3.1-dev.3
  appendBmJson(j, s, slot);   // 3.1-dev.88
  appendFhJson(j, s, slot);   // 3.1-dev.43
  {  // 2.0: markets, the galaxy event, the relay, your diplomat / notice / quest, every faction's quest + strength
    j += "\"v2\":{\"markets\":[";
    for (int i = 0; i < MARKETS_OPEN; i++) { if (i) j += ","; j += String(s.marketOpen[i]); }
    j += "],\"stalls\":[";  // 3.1-dev.9 faction stalls: [{f, shut, it:[[good, stock, price, canBuy] x2]}]
    bool firstStall = true;
    for (int f = 0; f < NUM_FACTIONS; f++) {
      if (s.stallGood[f][0] < 0 && s.stallGood[f][1] < 0) continue;
      if (!firstStall) j += ",";
      firstStall = false;
      j += "{\"f\":" + String(f) + ",\"shut\":" + String(snapshotStallShut(s, slot, f) ? 1 : 0) + ",\"it\":[";
      for (int k = 0; k < STALL_ITEMS; k++) {
        if (k) j += ",";
        j += "[" + String(s.stallGood[f][k]) + "," + String(s.stallStock[f][k]) + "," + String(snapshotStallPrice(s, slot, f, k)) + "," + String(snapshotStallCanBuy(s, slot, f, k) ? 1 : 0) + "]";
      }
      j += "]}";
    }
    j += "]";
    if (s.raceFaction >= 0 && s.raceUntil >= s.round) j += ",\"race\":{\"w\":" + String(s.raceWorld) + ",\"f\":" + String(s.raceFaction) + ",\"u\":" + String(s.raceUntil) + "}";  // 3.1-dev.10
    if (s.embargoFaction >= 0 && s.embargoRound == s.round) j += ",\"emb\":" + String(s.embargoFaction);
    j += ",\"champ\":[" + String(s.champion[0]) + "," + String(s.champion[1]) + "," + String(s.champion[2]) + "," + String(s.champion[3]) + "]";  // 3.1-dev.12 (public)
    if (snapshotEra(s) >= 0) j += ",\"era\":{\"f\":" + String(s.eraFaction) + ",\"by\":" + String(s.eraBy) + ",\"from\":" + String(s.eraFrom) + ",\"until\":" + String(s.eraUntil) + "}";
    if (s.warBy >= 0) j += ",\"war\":{\"by\":" + String(s.warBy) + ",\"w\":" + String(s.warWorld) + ",\"r\":" + String(s.warRound) + "}";  // 3.1-dev.13 (public)
    if (s.summitBy >= 0 && !s.summitBroken && s.round <= s.summitUntil)
      j += ",\"summit\":{\"by\":" + String(s.summitBy) + ",\"u\":" + String(s.summitUntil) + ",\"w\":[" + String(s.summitWorld[0]) + "," + String(s.summitWorld[1]) + "," + String(s.summitWorld[2]) + "]}";
    if (s.warLastN) {
      j += ",\"warLast\":{\"by\":" + String(s.warLastBy) + ",\"w\":" + String(s.warLastWorld) + ",\"won\":" + String(s.warLastWon) + ",\"a\":" + String(s.warLastA) + ",\"d\":" + String(s.warLastD) + ",\"log\":[";
      for (int k = 0; k < s.warLastN; k++) j += String(k ? "," : "") + String(s.warLog[k]);
      j += "]}";
    }
    if (s.offFaction >= 0) j += ",\"off\":{\"w\":" + String(s.offWorld) + ",\"f\":" + String(s.offFaction) + ",\"r\":" + String(s.offRound) + "}";  // 3.1-dev.11
    j += ",\"goods\":[";
    for (int g = 0; g < GOOD_COUNT; g++) j += String(g ? "," : "") + "[" + jsonStr(goodName(g)) + "," + jsonStr(goodEffect(g)) + "," + String(goodBasePrice(g)) + "]";
    j += "],\"event\":" + String(s.eventId) + ",\"eventName\":" + jsonStr(eventName(s.eventId)) + ",\"eventText\":" + jsonStr(eventText(s.eventId));
    char head[24], sub[64];
    snapshotPublicText(s, head, sizeof(head), sub, sizeof(sub));
    j += ",\"pubSeq\":" + String(s.publicSeq) + ",\"pubHead\":" + jsonStr(head) + ",\"pubSub\":" + jsonStr(sub);
    if (slot >= 0) {
      const Player &me = s.players[slot];
      int goal = 1;
      const int prog = snapshotQuestProgress(s, slot, &goal);
      j += ",\"noteSeq\":" + String(me.noteSeq) + ",\"note\":" + jsonStr(me.note);
      // 3.1-dev.48 SELL IT TWICE (Scoundrels): the offer, heat (a mood on the phone), caught
      if (me.sellTo >= 0 && me.sellRound == s.round) j += ",\"sell\":[" + String(me.sellTo) + "," + String(me.sellKind) + "," + String(me.sellPlanet) + "," + String(me.sellCr) + "]";
      if (me.heat) j += ",\"heat\":" + String(me.heat);
      if (me.champOffer >= 0) j += ",\"champOffer\":" + String(me.champOffer);  // 3.1-dev.49 ONE TITLE
      if (me.caughtRound >= 0) j += ",\"caught\":[" + String(me.caughtF[0]) + "," + String(me.caughtF[1]) + "," + String(me.caughtRound) + "," +
                                   String(snapshotPayBackCost(s, slot, me.caughtF[0])) + "," + String(snapshotPayBackCost(s, slot, me.caughtF[1])) + "]";
      j += ",\"qProg\":" + String(prog) + ",\"qGoal\":" + String(goal) + ",\"qDone\":" + String(me.questDone);
      j += ",\"gifts\":" + String(me.giftsGot) + ",\"safe\":" + String(snapshotSafeWorld(s, slot));  // 3.1-dev.8 the favor spine
      j += ",\"decrees\":" + String(me.decrees) + ",\"spared\":" + String(snapshotEraSpared(s, slot) ? 1 : 0) +  // 3.1-dev.12
           ",\"payShut\":" + String(snapshotPayShut(s, slot) ? 1 : 0) + ",\"shipCost\":" + String(snapshotShipCost(s, slot, false)) +
           ",\"vpHidden\":" + String(snapshotVpHidden(s, slot) ? 1 : 0) +
           ",\"patrolCr\":" + String(snapshotPatrolCost(s)) + ",\"mark\":" + String(s.markSlot) +  // 3.1-dev.88
           ",\"fileBought\":" + String(me.fileBuyRound == s.round ? 1 : 0) + ",\"income\":" + String(snapshotIncome(s, slot));
      // 3.1-dev.98 THE BACK CHANNEL: your offer this round (Consul), and an offer made to you
      if (characterClass(me.commander) == CLASS_CONSUL && !snapshotFhFinal(s)) {
        if (me.bcRound == s.round && me.bcSt) j += ",\"bcOut\":[" + String(me.bcTo) + "," + String(me.bcKind) + "," + String(me.bcArg) + "," + String(me.bcCr) + "," + String(me.bcSt) + "]";
        else j += ",\"bcCan\":1";
      }
      for (int c = 0; c < MAX_PLAYERS; c++) {
        const Player &k = s.players[c];
        if (k.bcTo == slot && k.bcRound == s.round && (k.bcSt == 1 || k.bcSt == 2))
          j += ",\"bcIn\":[" + String(c) + "," + String(k.bcKind) + "," + String(k.bcArg) + "," + String(k.bcCr) + "," + String(k.bcSt) + "]";
      }
      // 3.1-dev.102 GUNS FOR HIRE, hired guns, head starts, stand by, beaten to a world
      if (characterClass(me.commander) == CLASS_WARLORD) {
        if (me.gfhRound == s.round && me.gfhSt) j += ",\"gfhOut\":[" + String(me.gfhTo) + "," + String(me.gfhN) + "," + String(me.gfhCr) + "," + String(me.gfhSt) + "]";
        else j += ",\"gfhCan\":1";
      }
      for (int c = 0; c < MAX_PLAYERS; c++) {
        const Player &k = s.players[c];
        if (k.gfhTo == slot && k.gfhRound == s.round && k.gfhSt == 1) j += ",\"gfhIn\":[" + String(c) + "," + String(k.gfhN) + "," + String(k.gfhCr) + "]";
      }
      if (me.hiredN) j += ",\"hired\":[" + String(me.hiredN) + "," + String(me.hiredFrom) + "]";
      if (me.headStart) j += ",\"head\":" + String(me.headStart);
      if (me.standByMask && me.standByRound >= s.round) j += ",\"stby\":[" + String(me.standByMask) + "," + String(me.standByRound) + "]";
      if (me.beatenRound == s.round && s.phase == PHASE_RESOLUTION && me.beatenWorld >= 0) j += ",\"beaten\":" + String(me.beatenWorld);
      j += ",\"moveUsed\":" + String(me.moveUsed);  // 3.1-dev.13
      if (me.coverFaction >= 0) j += ",\"cover\":{\"f\":" + String(me.coverFaction) + ",\"u\":" + String(me.coverUntil) + ",\"story\":" + String(me.storyUsed) + "}";
      if (snapshotEra(s) == FACTION_CRIMSON_DAWN && s.champion[FACTION_CRIMSON_DAWN] == slot) j += ",\"myLie\":" + jsonStr(s.lieRound == s.round ? s.lie : "");
      if (me.grudgeFaction >= 0) {  // 3.1-dev.10 (+ dev.42: the deeds -- dk doable, dw the worlds each one can use)
        j += ",\"grudge\":{\"f\":" + String(me.grudgeFaction) + ",\"r\":" + String(me.grudgeRound) + ",\"w\":" + String(me.grudgeTarget) + ",\"dk\":[";
        for (int k = 0; k < 3; k++) j += String(k ? "," : "") + (snapshotDeedRefusal(s, slot, k, -1) ? "0" : "1");
        j += "],\"dd\":" + String(snapshotOfferedDeed(s, slot)) + ",\"dw\":[";
        for (int k = 0; k < 3; k++) {
          j += String(k ? ",[" : "[");
          bool first = true;
          for (int p = 0; p < NUM_PLANETS; p++)
            if (s.planetOwner[p] == slot && (k == 2 || (k == 0 && me.grudgeFaction == FACTION_EMPIRE)) && !snapshotDeedRefusal(s, slot, k, p)) { j += String(first ? "" : ",") + String(p); first = false; }
          j += "]";
        }
        j += "]}";
      }
      if (me.jobState) j += ",\"job\":{\"f\":" + String(me.jobFaction) + ",\"w\":" + String(me.jobWorld) + ",\"due\":" + String(me.jobDue) +
                            ",\"st\":" + String(me.jobState) + ",\"ann\":" + String(me.jobAnnounced) + "}";
      // 3.1-dev.78 BLACKMAIL (the Crimson Dawn's Family gift, once) and THEIR FILE (the Infiltrator; the Slicer lot)
      if (!me.blackmailUsed && snapshotReputationTier(s, slot, FACTION_CRIMSON_DAWN) == REP_ALLIED) j += ",\"bmReady\":1";
      if (me.bmBy >= 0) j += ",\"bmAsk\":" + String(me.bmBy);
      const bool infil = characterClass(me.commander) == CLASS_INFILTRATOR, sliced = me.sliceRound == s.round && me.sliceOn >= 0;
      if (infil && (me.fileRound < 0 || s.round - me.fileRound >= FILE_COOLDOWN)) j += ",\"fileCan\":1";  // 3.1-dev.97 every 2 rounds
      else if (infil) j += ",\"fileNext\":" + String(me.fileRound + FILE_COOLDOWN);
      const int ft = sliced ? me.sliceOn : (infil ? me.fileOn : -1);
      if (ft >= 0 && ft < MAX_PLAYERS && ft != slot && s.players[ft].active) {
        const Player &o = s.players[ft];
        j += ",\"file\":{\"s\":" + String(ft) + ",\"sl\":" + String(sliced ? 1 : 0) + ",\"cr\":" + String(o.credits) + ",\"d\":[";
        bool first = true;
        for (int q = 0; q < MAX_PENDING_OBLIGATIONS; q++) if (o.obligations[q].active) {
          j += String(first ? "" : ",") + "[" + String(o.obligations[q].factionIdx) + "," + String(o.obligations[q].planetIdx) + "," + String(o.obligations[q].dueRound) + "]"; first = false; }
        j += "],\"g\":" + (o.grudgeFaction >= 0 ? "[" + String(o.grudgeFaction) + "," + String(o.grudgeRound) + "]" : String("null"));
        j += ",\"job\":" + (o.jobState == 2 ? "[" + String(o.jobFaction) + "," + String(o.jobWorld) + "]" : String("null")) + ",\"def\":[";
        first = true;
        for (int p = 0; p < NUM_PLANETS; p++) if (s.planetOwner[p] == ft && sliced) { j += String(first ? "" : ",") + "[" + String(p) + "," + String(s.planetDefense[p]) + "]"; first = false; }
        j += "]}";
      }
      j += ",\"perks\":{\"merc\":" + String(me.nextBattleMercs) + ",\"reroll\":" + String(me.nextBattleReroll) + ",\"cargo\":" + String(me.cargoDue) +
           ",\"harbor\":" + String(me.harborRound == s.round ? 1 : 0) + ",\"permit\":" + String(me.permitDiscount) + "}";
      // (3.1-dev.20: deals are cut)
      j += ",\"heistPush\":" + String(me.heistPushF >= 0 && me.heistPushRound == s.round ? me.heistPushF : -1) + ",\"grudgePay\":" + String(grudgePayoffCr(me.grudgeFaction));
    }
    j += ",\"questDone\":[";
    for (int i = 0; i < MAX_PLAYERS; i++) { if (i) j += ","; j += String(s.players[i].questDone); }
    j += "]";
    // 2.0 THE ASHEN FLEET: HP, your damage, the latest strike.
    j += ",\"ashen\":{\"alive\":" + String(snapshotAshenAlive(s) ? 1 : 0) + ",\"hp\":" + String(s.ashenHp) + ",\"max\":" + String(s.ashenMaxHp) +
         ",\"mine\":" + String(slot >= 0 ? s.ashenDmg[slot] : 0) + ",\"seq\":" + String(s.ashenSeq) + ",\"lastSlot\":" + String(s.ashenLastSlot) +
         ",\"ships\":" + String(s.ashenLastShips) + ",\"dmg\":" + String(s.ashenLastDmg) + ",\"roll\":" + String(s.ashenLastRoll) +
         ",\"hits\":" + String(s.ashenLastHits) + ",\"vp\":" + String(ASHEN_VP) + ",\"topVp\":" + String(ASHEN_TOP_VP) + "}";
    j += "},";  // (3.1-dev.20: the old deal asks and the 2.0 faction quests / strengths are cut)

  }
  // v0.27.3: YOUR promises (honor-system obligations) -- what, to whom, when due.
  // Only the requester's own; other players' promises stay private.
  j += "\"promises\":[";
  if (slot >= 0) {
    bool first = true;
    for (int k = 0; k < MAX_PENDING_OBLIGATIONS; k++) {
      const PendingObligation &ob = s.players[slot].obligations[k];
      if (!ob.active) continue;
      if (!first) j += ",";
      first = false;
      j += "{\"f\":" + String(ob.factionIdx) + ",\"p\":" + String(ob.planetIdx) + ",\"due\":" + String(ob.dueRound) +
           ",\"chk\":" + String((int)ob.check) + ",\"text\":" + jsonStr(ob.text) + "}";  // jsonStr adds the quotes (v0.28.1 fix)
    }
  }
  j += "],";
  // v0.28.3: YOUR last kept / broken promises, newest first.
  j += "\"promiseLog\":[";
  if (slot >= 0) {
    bool first = true;
    for (int k = 0; k < PROMISE_HISTORY; k++) {
      const PromiseRecord &r = s.players[slot].promiseLog[k];
      if (r.faction < 0) continue;
      char txt[160];
      snapshotFavorText(s, slot, r.offer, r.planet, txt, sizeof(txt));  // v0.30.1: the favor bank
      if (!first) j += ",";
      first = false;
      j += "{\"f\":" + String((int)r.faction) + ",\"p\":" + String((int)r.planet) + ",\"round\":" + String((int)r.round) +
           ",\"kept\":" + String(r.kept ? "true" : "false") + ",\"text\":" + jsonStr(txt) + "}";
    }
  }
  j += "],";

  j += "\"planetIncluded\":[";
  for (int p = 0; p < NUM_PLANETS; p++) {
    if (p) j += ",";
    j += s.planetIncluded[p] ? "true" : "false";
  }
  j += "],";

  // 2.0-dev.13: back from AWAY -- what changed while you were gone (once).
  if (slot >= 0 && s.players[slot].welcome) {
    const Player &me = s.players[slot];
    j += "\"welcome\":{\"round\":" + String(me.awayRound) + ",\"vp\":" + String(me.awayVp) + ",\"cr\":" + String(me.awayCr) +
         ",\"worlds\":" + String((unsigned long)me.awayWorlds) + "},";
  }

  // players
  j += "\"players\":[";
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (i) j += ",";
    j += "{\"active\":" + String(s.players[i].active ? "true" : "false");
    j += ",\"name\":" + jsonStr(s.players[i].name);
    j += ",\"chosenFaction\":" + String(s.players[i].chosenFaction);
    j += ",\"commander\":" + String(s.players[i].commander);  // v0.21.20
    j += ",\"credits\":" + String(s.players[i].credits);
    j += ",\"income\":" + String(s.players[i].lastIncome);
    j += ",\"vp\":" + String((i != slot && snapshotVpHidden(s, slot)) ? -1 : s.players[i].vp);  // 3.1-dev.12 the Shadow Age hides the score
    j += ",\"lockedIn\":" + String(s.players[i].lockedIn ? "true" : "false");
    j += ",\"away\":" + String((int)s.players[i].away);  // 2.0-dev.13 (an AWAY player is also lockedIn)
    // Per-fleet: alive ship count, total commissioned (built) count, and the
    // individual per-ship alive flags (for the detail view).
    j += ",\"cap\":" + String(snapshotFleetCapacity(s, i));  // 2.0-dev.20: ships each fleet can hold (Empire 7)
    j += ",\"fleets\":[";
    for (int f = 0; f < NUM_FLEETS; f++) {
      if (f) j += ",";
      int n = 0;
      j += "{\"ships\":[";
      for (int sh = 0; sh < FLEET_SHIPS; sh++) {
        if (sh) j += ",";
        bool a = s.players[i].fleetShipAlive[f][sh];
        if (a) n++;
        j += a ? "true" : "false";
      }
      j += "]";
      j += ",\"alive\":" + String(n) + ",\"total\":" + String(s.players[i].fleetShipCount[f])
           + ",\"spent\":" + String(s.players[i].fleetSpent[f] ? "true" : "false") + "}";
    }
    j += "]}";
  }
  j += "],";

  // planets (with this player's revealed intel)
  // 2.0-dev.19: COMPACT planets (they were ~6.6 of ~11 KB -- every phone downloads
  // the whole answer each time anything changes, and the Wi-Fi stack holds it in
  // internal RAM while it goes out). The phone expands this back into the old
  // shape in expandState() (PAGE_HTML) -- change both together:
  //   rv = bitmask of the characteristics you've revealed, cv = their values (only
  //   when rv != 0), cc = what each costs to reveal; g = index into goodTab
  //   ([name, effect, GoodId, base price] once per good in play); locked / mOpen / mBuy / unbound only
  //   when true; dipl only when false.
  int goodIds[NUM_PLANETS], goodN = 0;
  j += "\"planets\":[";
  for (int p = 0; p < NUM_PLANETS; p++) {
    if (p) j += ",";
    j += "{\"name\":" + jsonStr(PLANETS[p].name);
    j += ",\"owner\":" + String(s.planetOwner[p]);
    j += ",\"ring\":" + String(PLANETS[p].ring) + ",\"vp\":" + String(planetVP(s, p, slot)) + ",\"home\":" + String(planetHomeOf(p));  // v0.30.1: worth this while held
    if (s.planetOwner[p] == slot && s.planetVpPending[p]) j += ",\"pending\":true";  // yours, held on a debt (3.1-dev.8: its VP counts already)
    // v0.21.11 (user): no garrison tier at all -- the only garrison number a
    // player ever gets is the exact count of their OWN planet ("defense").
    // v0.20.2: you already committed to an approach here this round.
    if (slot >= 0 && s.players[slot].takeoverLocked[p]) j += ",\"locked\":1";
    if (s.planetOwner[p] == slot) j += ",\"defense\":" + String(s.planetDefense[p]) + ",\"dl\":" + String(snapshotDefenseLimit(s, p));  // 3.1-dev.76 its limit
    if (s.planetOwner[p] >= 0 && s.patrolUntil[p] >= s.round && s.patrolShips[p] > 0) j += ",\"pt\":" + String(s.patrolUntil[p] - s.round + 1) + ",\"ps\":" + String((int)s.patrolShips[p]);  // a patrol (public)
    if (slot >= 0 && s.planetOwner[p] != slot && s.players[slot].defSeen[p]) j += ",\"ds\":" + String(s.players[slot].defSeen[p] - 1) + ",\"dd\":" + String(snapshotDefSeenState(s, slot, p));  // 3.1-dev.101
    if (slot >= 0 && s.players[slot].alertRound == s.round && s.players[slot].alertWorld == p) j += ",\"al\":1";  // 3.1-dev.101 talks failed: fight only
    if ((s.treatyMask >> p) & 1) j += ",\"ty\":" + String(s.treatyUntil[p] >= s.round ? s.treatyUntil[p] - s.round + 1 : 0);  // 3.1-dev.101 a treaty world (public)
    if (s.phase == PHASE_RESOLUTION && s.planetTakenRound[p] == s.round) j += ",\"dug\":1";  // 3.1-dev.102 taken by force this round: dug in
    if (snapshotInkWet(s, p)) j += ",\"ink\":[" + String((int)s.inkHow[p]) + "," + String((int)((s.inkHit >> p) & 1)) + "]";  // 3.1-dev.102 the ink is still wet
    j += ",\"nb\":" + String(snapshotNeighbors(p));  // 3.1-dev.102 one step away (REDIRECT)
    { const int d = snapshotDefendersNow(s, p);  // 3.1-dev.115 DEFENSE LEVEL, public and live: the same word means the same strength everywhere
      j += ",\"dv\":" + String(d <= 2 ? 0 : d <= 5 ? 1 : d <= 8 ? 2 : 3); }
    if (s.buyBy[p]) j += ",\"bo\":[" + String(s.buyBy[p] - 1) + "," + String((int)s.buyCr[p]) + "]";  // 3.1-dev.79 a buyout offer (public)
    if (s.planetTrait[p] == TR_COURT) j += ",\"tc\":1";  // 3.1-dev.84: its court speaks for whoever holds it (traits are public)
    // v0.21.6: the asking price, so the planet page can show PAY n CR
    // without opening a takeover session first.
    if (slot >= 0) j += ",\"pay\":" + String(snapshotPayPrice(s, slot, p));
    // 2.0: what holding it gives you (once its trait is known), its market, the diplomat.
    if (slot >= 0 && s.players[slot].revealed[p][2]) j += ",\"traitFx\":" + jsonStr(traitEffect(s.planetTrait[p]));
    int g = 0;
    while (g < goodN && goodIds[g] != s.planetGood[p]) g++;
    if (g == goodN) goodIds[goodN++] = s.planetGood[p];
    j += ",\"g\":" + String(g);
    if (snapshotMarketIsOpen(s, p)) j += ",\"mOpen\":1";
    j += ",\"mStock\":" + String(s.marketStock[p]) + ",\"mPrice\":" + String(snapshotMarketPrice(s, slot, p));
    if (snapshotCanBuy(s, slot, p)) j += ",\"mBuy\":1";
    if (s.planetUnbound[p]) j += ",\"unbound\":1";
    int rv = 0;
    for (int c = 0; c < NUM_CHARACTERISTICS; c++)
      if (slot >= 0 && s.players[slot].revealed[p][c]) rv |= 1 << c;
    j += ",\"rv\":" + String(rv);
    if (rv) {
      j += ",\"cv\":[";
      for (int c = 0; c < NUM_CHARACTERISTICS; c++) {
        if (c) j += ",";
        j += jsonStr((rv & (1 << c)) ? planetCharValue(s, p, c) : "");  // v0.25.4: this game's galaxy
      }
      j += "]";
    }
    j += ",\"cc\":[";
    for (int c = 0; c < NUM_CHARACTERISTICS; c++) {
      if (c) j += ",";
      j += String(snapshotRevealCost(s, slot, p, c));  // 2.0: a free one with TR_SPYNET
    }
    j += "]";
    if (slot >= 0 && s.planetOwner[p] != slot) {  // 2.0-dev.29: the garrison sizes YOU can't rule out (the planet page's attack line)
      int lo = 0, hi = 0;
      snapshotGarrisonRange(s, slot, p, &lo, &hi);
      j += ",\"gr\":[" + String(lo) + "," + String(hi) + "]";
    }
    j += "}";
  }
  j += "],\"goodTab\":[";
  for (int g = 0; g < goodN; g++) {
    if (g) j += ",";
    j += "[" + jsonStr(goodName(goodIds[g])) + "," + jsonStr(goodEffect(goodIds[g])) + "," + String(goodIds[g]) + "," + String(goodBasePrice(goodIds[g])) + "]";  // dev.21: + id (the shelf drawing), base price
  }
  j += "],";

  // battle
  j += "\"battleKind\":" + String(s.battleKind) + ",";
  j += "\"battleActive\":" + String(s.battleActive ? "true" : "false") + ",";
  j += "\"battleAttackerSlot\":" + String(s.battleAttackerSlot) + ",";
  j += "\"battleDefenderSlot\":" + String(s.battleDefenderSlot) + ",";
  j += "\"battleTargetPlanet\":" + String(s.battleTargetPlanet) + ",";
  j += "\"defRemaining\":" + String(s.defRemaining) + ",";
  // Working-copy fleet survivor counts for the active battle.
  {
    int atk = 0, def = 0;
    for (int i = 0; i < FLEET_SHIPS; i++) { if (s.atkAlive[i]) atk++; if (s.defAlive[i]) def++; }
    j += "\"battleAtkShips\":" + String(atk) + ",";
    j += "\"battleDefShips\":" + String(def) + ",";
    j += "\"atkAction\":" + String(s.atkAction) + ",";
    j += "\"defAction\":" + String(s.defAction) + ",";
  }
  j += "\"lastAtkRoll\":" + String(s.lastAtkRoll) + ",";
  j += "\"lastDefRoll\":" + String(s.lastDefRoll) + ",";
  // Battle animation (v0.20.0): per-ship flags so the scene can draw each
  // ship in its slot, plus the exchange counter/loss index that drive it.
  j += "\"battleExchange\":" + String(s.battleExchange) + ",";
  j += "\"lastLossIdx\":" + String(s.lastLossIdx) + ",";
  j += "\"battleAtkMod\":" + String(s.battleAtkMod) + ",\"battleDefMod\":" + String(s.battleDefMod) + ",";
  // 2.0-dev.24 escort / militia: [faction, ships still in the fight]
  j += "\"bEsc\":[" + String(s.battleEscortFaction) + "," + String(s.battleEscortAlive) + "],\"bMil\":[" +
       String(s.battleMilitiaFaction) + "," + String(s.battleMilitiaAlive) + "],";
  j += "\"bMerc\":[" + String(s.battleAtkMercs + s.battleAtkExtra) + "," + String(s.battleDefMercs + s.battleDefExtra) + "],";  // 3.1-dev.7: + class extras  // 2.0-dev.26 hired ships
  j += "\"lastRetreatSide\":" + String(s.lastRetreatSide) + ",";
  j += "\"shipStride\":" + String(FLEET_SHIPS) + ",";  // 2.0-dev.20: battle ship index = fleet * shipStride + slot
  j += "\"battleAtkAlive\":[";
  for (int i = 0; i < BATTLE_SHIPS; i++) { if (i) j += ","; j += s.atkAlive[i] ? "1" : "0"; }  // v0.28.0: every fleet (f * 6 + slot)
  j += "],\"battleDefAlive\":[";
  for (int i = 0; i < BATTLE_SHIPS; i++) { if (i) j += ","; j += s.defAlive[i] ? "1" : "0"; }
  j += "],";
  j += "\"battleAtkFleets\":" + String(s.battleAttackerFleets) + ",\"battleDefFleets\":" + String(s.battleDefenderFleets) + ",";
  j += "\"canReinforce\":" + String(snapshotReinforceMask(s, slot)) + ",";
  j += "\"canCallMercs\":" + String(snapshotCanCallMercs(s, slot) ? 1 : 0) + ",";  // 2.0-dev.28  // v0.28.0: fleets YOU could add as the PvP defender
  j += "\"battleOver\":" + String(s.battleOver ? "true" : "false") + ",";

  // takeover -- this requesting player's OWN session only (sessions are
  // per-slot since v0.19.1). takeoverPlayer keeps its old meaning for the
  // JS: equals yourSlot when you have a session open, else -1.
  int tPlanet = (slot >= 0) ? s.takeoverPlanet[slot] : -1;
  int tMethod = (slot >= 0) ? s.takeoverMethod[slot] : TAKEOVER_NONE;
  j += "\"takeoverPlayer\":" + String(tPlanet >= 0 ? slot : -1) + ",";
  j += "\"takeoverPlanet\":" + String(tPlanet) + ",";
  j += "\"takeoverMethod\":" + String(tMethod) + ",";
  j += "\"payPrice\":" + String(tPlanet >= 0 ? snapshotPayPrice(s, slot, tPlanet) : 0) + ",";
  j += "\"takeoverResult\":" + jsonStr(slot >= 0 ? s.takeoverResult[slot] : "") + ",";
  // Battle briefing (v0.20.1): the faction roll modifiers a Fight on your
  // takeover planet would get right now, with the granting factions.
  {
    BattleMods fm = snapshotPlanetBattleMods(s, slot, tPlanet);
    // 2.0-dev.25: a militia rises even on a world you haven't read, but the
    // forecast must not leak its Sympathy -- you meet the militia in the fight.
    if (slot >= 0 && tPlanet >= 0 && !s.players[slot].revealed[tPlanet][0]) { fm.defMilitia = 0; fm.defSourceFaction = -1; }
    j += "\"fightMods\":{\"atk\":" + String(fm.atkMod) + ",\"atkSrc\":" + String(fm.atkSourceFaction) +
         ",\"def\":" + String(fm.defMod) + ",\"defSrc\":" + String(fm.defSourceFaction) +
         ",\"esc\":" + String(fm.atkEscort) + ",\"mil\":" + String(fm.defMilitia) + ",\"merc\":" + String(fm.atkMercs);
    int gLo = 0, gHi = 0;  // 2.0-dev.27 forecast v2: the garrison sizes you can't rule out
    if (slot >= 0 && tPlanet >= 0) snapshotGarrisonRange(s, slot, tPlanet, &gLo, &gHi);
    j += ",\"gLo\":" + String(gLo) + ",\"gHi\":" + String(gHi) + "},";
  }

  // v0.30.0 the negotiation conversation -- YOUR own only. The real faction is
  // sent only once you've revealed Affiliation (else an unknown ambassador).
  {
    const NegView nv = snapshotNegotiation(s, slot);
    if (!nv.open) j += "\"neg\":null,";
    else {
      char buf[200];
      j += "\"neg\":{\"planet\":" + String(nv.planet) + ",\"hs\":" + String((int)s.negHead[slot]) + ",\"up\":" + String((int)s.negUprising[slot]) + ",\"faction\":" + String(nv.known ? nv.faction : -1) +
           ",\"count\":" + String(nv.count) + ",\"step\":" + String(nv.step) + ",\"result\":" + String(nv.result) +
           ",\"losses\":" + String(nv.losses) + ",\"tipped\":" + String(nv.tipped ? "true" : "false") +
           ",\"favorStage\":" + String(nv.favorStage ? "true" : "false") + ",\"hard\":" + String(nv.hard ? "true" : "false") + ",\"marks\":[";
      for (int k = 0; k < nv.count; k++) { if (k) j += ","; j += String((int)nv.marks[k]); }
      j += "]";
      if (nv.question >= 0) {
        const NegQuestion &q = NEG_BANK[nv.question];
        negFillPlanet(q.q, nv.planet, buf, sizeof(buf));
        j += ",\"tier\":" + String((int)q.tier) + ",\"own\":" + String(q.own >= 0 ? "true" : "false") + ",\"q\":" + jsonStr(buf) + ",\"replies\":[";
        for (int i = 0; i < nv.nReplies; i++) {
          if (i) j += ",";
          negFillPlanet(q.reply[nv.replies[i]], nv.planet, buf, sizeof(buf));
          j += "{\"i\":" + String(nv.replies[i]) + ",\"text\":" + jsonStr(buf) + "}";
        }
        j += "]";
      }
      // v0.30.1: the drawn favors (credits = paid now, the late price), the late
      // window, and a hostile faction's line.
      j += ",\"late\":" + String(nv.late ? "true" : "false") + ",\"roundsLeft\":" + String(nv.roundsLeft) +
           ",\"lateCredits\":" + String(nv.lateCredits) + ",\"payPrice\":" + String(snapshotPayPrice(s, slot, nv.planet)) + ",\"favors\":[";
      for (int i = 0; i < nv.nFavors; i++) {
        if (i) j += ",";
        snapshotFavorText(s, slot, nv.favors[i], nv.planet, buf, sizeof(buf));
        j += "{\"text\":" + jsonStr(buf) + ",\"credits\":" + String(NEG_FAVOR_BANK[nv.favors[i]].check == NEG_FAVOR_CREDITS ? "true" : "false") + "}";
      }
      j += "]";
      snapshotFavorText(s, slot, nv.favorTaken, nv.planet, buf, sizeof(buf));
      const bool paid = nv.favorTaken >= 0 && NEG_FAVOR_BANK[nv.favorTaken].check == NEG_FAVOR_CREDITS;
      j += ",\"owe\":" + jsonStr(paid ? "" : buf) + ",\"paid\":" + String(paid ? "true" : "false") +
           ",\"line\":" + jsonStr(nv.hostileLine >= 0 ? NEG_HOSTILE_LINES[nv.hostileLine] : "") +
           ",\"vp\":" + String(planetVP(s, nv.planet, slot)) + ",\"gw\":" + String(s.galWhisper[slot]) + ",\"gv\":" + String(s.galVouch[slot]) + "},";
    }
  }
  {  // 3.1-dev.52 THE GALLERY: everyone else's talk, live, for watching (the faction only if YOU know the world's Sympathy)
    bool first = true;
    for (int o = 0; o < MAX_PLAYERS; o++) {  // 3.1-dev.54: spectators (slot -1) watch too
      if (o == slot) continue;
      const NegView tv = snapshotNegotiation(s, o);
      if (!tv.open || tv.count == 0) continue;
      const int k = s.negStep[o] + 1, f = planetFaction(tv.planet);
      const bool talking = tv.result == NEG_TALKING, acted = slot >= 0 && ((s.galActed[o] >> slot) & 1);
      const bool seated = slot >= 0 && slot < MAX_PLAYERS;
      const bool cw = seated && talking && k < tv.count && !acted && s.galWhisper[o] < 0 && s.players[slot].credits >= GAL_WHISPER_CR;
      const bool cv = seated && talking && k < tv.count && !acted && s.negDrop[o][k] < 0 && snapshotReputationTier(s, slot, f) >= REP_FAVORABLE;
      const char *why = !talking ? "" : k >= tv.count ? "Too late: they're on the last question." : acted ? "You've had your say in this talk." : "";
      j += String(first ? "\"talks\":[" : ",") + "{\"by\":" + String(o) + ",\"p\":" + String(tv.planet) + ",\"f\":" + String((slot >= 0 && s.players[slot].revealed[tv.planet][0]) ? f : -1) +
           ",\"n\":" + String(tv.count) + ",\"s\":" + String(tv.step) + ",\"r\":" + String(tv.result) + ",\"m\":[";
      first = false;
      for (int q = 0; q < tv.count; q++) { if (q) j += ","; j += String((int)tv.marks[q]); }
      j += "]";
      if (talking && tv.question >= 0) {
        char buf[200];
        const NegQuestion &qq = NEG_BANK[tv.question];
        negFillPlanet(qq.q, tv.planet, buf, sizeof(buf));
        j += ",\"q\":" + jsonStr(buf) + ",\"rep\":[";
        for (int i = 0; i < tv.nReplies; i++) { if (i) j += ","; negFillPlanet(qq.reply[tv.replies[i]], tv.planet, buf, sizeof(buf)); j += jsonStr(buf); }
        j += "]";
      }
      j += ",\"w\":" + String(s.galWhisper[o]) + ",\"v\":" + String(s.galVouch[o]) + ",\"cw\":" + String(cw ? 1 : 0) + ",\"cv\":" + String(cv ? 1 : 0) + ",\"why\":" + jsonStr(why) + "}";
    }
    if (!first) j += "],";
  }

  j += "\"resultText\":" + jsonStr(s.resultText) + ",";

  // this player's alert
  j += "\"alert\":" + jsonStr(slot >= 0 ? s.alert[slot] : "") + ",";

  // this player's reputation with each faction: the tier, and (v0.29.0) the
  // score behind it for the Factions bar. Only the requester's own.
  j += "\"reputation\":[";
  for (int f = 0; f < 4; f++) {
    if (f) j += ",";
    int tier = snapshotReputationTier(s, slot, f);  // REP_NEUTRAL for slot -1
    // v0.29.0: YOUR exact score too, for the Factions bar (it is your own standing).
    j += "{\"tier\":\"" + String(REP_TIER_NAMES[tier]) + "\",\"score\":" + String(slot >= 0 ? s.players[slot].reputation[f] : 0) + "}";
  }
  j += "],";

  // this player's pending Faction Transmission, if any -- transmissionIsObligation
  // tells the client to render YES/NO buttons instead of a simple dismiss.
  bool txPending = (slot >= 0) ? s.transmissionPending[slot] : false;
  int txFaction = (slot >= 0) ? s.transmissionFaction[slot] : -1;
  bool txIsObligation = (slot >= 0) ? s.transmissionIsObligation[slot] : false;
  j += "\"transmissionPending\":" + String(txPending ? "true" : "false") + ",";
  j += "\"transmissionFaction\":" + String(txFaction) + ",";
  j += "\"transmissionIsObligation\":" + String(txIsObligation ? "true" : "false") + ",";
  char txText[220];
  snapshotTransmissionText(s, slot, txText, sizeof(txText));  // v0.23.0: bank line, {planet} filled in
  j += "\"transmissionQuote\":" + jsonStr(txText) + ",";
  const TxMsg txHead = (slot >= 0 && txPending) ? s.txQueue[slot][0] : TxMsg();
  j += "\"txFrom\":" + String((int)txHead.fromTier) + ",\"txTo\":" + String((int)txHead.toTier) +
       ",\"txBig\":" + String(txHead.big ? "true" : "false") + ",\"txCause\":" + String((int)txHead.cause) + ",\"txDelta\":" + String((int)txHead.delta) + ",\"txPlanet\":" + String((int)txHead.planet) + ",";  // 3.1-dev.46 the readout's RE: line
  // 3.1-dev.75 (user: "I often raise with one faction and drop with another in the same action, and it gives me two
  // transmission screens"): the next queued reaction to the SAME thing (another faction, same world) rides along as a chip.
  if (slot >= 0 && txPending && s.txCount[slot] > 1 && txHead.line >= 0) {
    const TxMsg &n2 = s.txQueue[slot][1];
    if (n2.line >= 0 && !n2.big && n2.faction >= 0 && n2.faction != txHead.faction && n2.planet == txHead.planet)
      j += "\"txAlso\":[" + String((int)n2.faction) + "," + String((int)n2.fromTier) + "," + String((int)n2.toTier) + "," + String((int)n2.delta) + "," + String((int)n2.cause) + "],";
  }

  // Winner slot, only meaningful once stage==2 (STAGE_GAME_OVER). -1 = not
  // over yet, -2 = a genuine draw (every tiebreaker also tied).
  int winner = snapshotWinnerSlot(s);
  j += "\"winner\":" + String(winner);

  // 2.0-dev.15: a fingerprint of the answer. A phone sends back the one it last
  // got (?h=); if nothing changed it gets a few bytes instead of ~11 KB -- the
  // Wi-Fi stack holds every answer in internal RAM while it goes out, and with
  // 4 phones polling that ran the table down to 0 KB free (RAM test 2026-11-03).
  uint32_t hash = 2166136261u;
  for (const char *c = j.c_str(); *c; c++) { hash ^= (uint8_t)*c; hash *= 16777619u; }
  char hx[12];
  snprintf(hx, sizeof(hx), "%08x", (unsigned)hash);
  if (server.hasArg("h") && server.arg("h") == hx) {
    server.send(200, "application/json", "{\"same\":1}");
    return;
  }
  j += ",\"h\":\"";
  j += hx;
  j += "\"}";
  if (j.length() > stateJsonMax) {
    stateJsonMax = j.length();
    Serial.printf("[NET] /state answer %u bytes (reserve %u)\n", (unsigned)stateJsonMax, (unsigned)STATE_JSON_RESERVE);
  }
  sendBounded("application/json", (const uint8_t *)j.c_str(), j.length(), false);  // 3.1-dev.62
}

// ---------------------------------------------------------------------------
// Home Wi-Fi mode (v0.22.5; user 2026-10-15: phones shouldn't have to leave
// their Wi-Fi or type an IP). The table remembers ONE home network (saved
// from a phone at /setup -- never typed on a console) and joins it at boot;
// phones on that network scan the table's single QR code (or open
// http://shadowfleet.local). No saved network, the user chose "TABLE WI-FI",
// or the network can't be reached -> the table runs its own "ShadowFleet"
// network as before.
// ---------------------------------------------------------------------------

bool networkOnHomeWifi() { return net_home; }
bool networkHomeSaved() { return net_home_saved; }
bool networkHomeFailed() { return net_home_failed; }
const char *networkHomeSsid() { return net_ssid; }
const char *networkFailWhy() { return net_fail_why; }
const char *networkGameUrl() { return net_url; }

void networkSwitchMode(bool home) {
  Preferences p;
  p.begin("sfnet", false);
  p.putBool("home", home);
  p.end();
  Serial.printf("[NET] switching to %s Wi-Fi -- restarting\n", home ? "HOME" : "TABLE");
  net_restart_at = millis() + 300;
}

// 3.1-dev.61 SETTINGS: up to NET_SAVED networks, newest first (slot 0 keeps the old "ssid"/"pass" keys, so an
// existing saved network survives the update). Passwords never leave the table.
#define NET_SAVED 4
static int netLoad(String *ss, String *pw) {
  Preferences p;
  p.begin("sfnet", true);
  int n = 0;
  for (int i = 0; i < NET_SAVED; i++) {
    const String k = i ? String(i) : String("");
    const String s = p.getString(("ssid" + k).c_str(), "");
    if (s.length()) { ss[n] = s; pw[n] = p.getString(("pass" + k).c_str(), ""); n++; }
  }
  p.end();
  return n;
}
static void netStore(const String *ss, const String *pw, int n) {
  Preferences p;
  p.begin("sfnet", false);
  for (int i = 0; i < NET_SAVED; i++) {
    const String k = i ? String(i) : String("");
    if (i < n) { p.putString(("ssid" + k).c_str(), ss[i]); p.putString(("pass" + k).c_str(), pw[i]); }
    else { p.remove(("ssid" + k).c_str()); p.remove(("pass" + k).c_str()); }
  }
  p.end();
}

// Joins a saved home network if wanted (the first saved one in range, newest first); true on success.
static bool joinHomeWifi() {
  String ss[NET_SAVED], pw[NET_SAVED];
  const int n = netLoad(ss, pw);
  Preferences p;
  p.begin("sfnet", true);
  const bool preferHome = p.getBool("home", true);
  p.end();
  net_home_saved = n > 0;
  snprintf(net_ssid, sizeof(net_ssid), "%s", n ? ss[0].c_str() : "");
  if (!net_home_saved || !preferHome) return false;
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("shadowfleet");
  // 3.1-dev.62 (a weekend at a Synology Wi-Fi 7 router): look around first, try every saved network that is in range
  // (newest first, at most two tries so boot stays short), and remember WHY a join failed, in plain words.
  WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t info) { net_join_reason = info.wifi_sta_disconnected.reason; },
               ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  const int found = WiFi.scanNetworks(false, false, false, 120);
  int order[NET_SAVED], m = 0;
  int8_t seen[NET_SAVED] = {0}; int32_t rssi[NET_SAVED] = {0}; uint8_t sec[NET_SAVED] = {0};
  for (int i = 0; i < n; i++)
    for (int j = 0; j < found; j++) if (WiFi.SSID(j) == ss[i]) { seen[i] = 1; rssi[i] = WiFi.RSSI(j); sec[i] = (uint8_t)WiFi.encryptionType(j); break; }
  WiFi.scanDelete();
  for (int i = 0; i < n; i++) if (seen[i]) order[m++] = i;
  if (!m) {  // none in sight (a hidden network?): try the newest anyway
    order[m++] = 0;
    Serial.printf("[NET] none of the %d saved networks is in range (scan saw %d)\n", n, found);
  }
  if (m > 2) m = 2;
  for (int k = 0; k < m; k++) {
    const int i = order[k];
    snprintf(net_ssid, sizeof(net_ssid), "%s", ss[i].c_str());
    snprintf(net_pass, sizeof(net_pass), "%s", pw[i].c_str());
    // 3.1-dev.111 (the overnight reboot storm: 6 of 30 boots "wedged" -- the router REJECTED the first login (reason 202) on a
    // WPA2/WPA3 network, the table called it a wrong password and ran its own network all game): a rejected login is tried
    // again, up to 3 times; only three rejections in a row count as a wrong password.
    for (int attempt = 1; attempt <= 3; attempt++) {
      net_join_reason = 0;
      WiFi.begin(net_ssid, net_pass);
      Serial.printf("[NET] joining home Wi-Fi '%s' (%s, signal %d dBm, security %u)%s...\n", net_ssid, seen[i] ? "in range" : "NOT SEEN", (int)rssi[i], (unsigned)sec[i], attempt > 1 ? " -- again" : "");
      const uint32_t t0 = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
        delay(100);
        const unsigned rr = net_join_reason;
        if (rr == 2 || rr == 15 || rr == 202 || rr == 204) break;  // rejected: no point waiting out the 15 s
      }
      if (WiFi.status() == WL_CONNECTED) break;
      const unsigned rr = net_join_reason;
      if (!(rr == 2 || rr == 15 || rr == 202 || rr == 204) || attempt == 3) break;  // not a rejection (or out of tries): give up on it
      Serial.printf("[NET] '%s' rejected the login (reason %u) -- trying again\n", net_ssid, rr);
      WiFi.disconnect(false);
      delay(1000);
    }
    if (WiFi.status() == WL_CONNECTED) break;
    wifi_ap_record_t ap;
    const bool assoc = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    const unsigned r = net_join_reason;
    const char *why = !seen[i] ? "not found (out of range, or its 2.4 GHz band is off)"
                    : assoc ? "joined but got no address (the router may hold new devices)"
                    : (r == 2 || r == 15 || r == 202 || r == 204) ? "wrong password?"
                    : (r == 210 || r == 211) ? "its security mode won't let the table in"
                    : (r == 201) ? "not found (out of range, or its 2.4 GHz band is off)"
                    : (r == 203 || r == 205 || r == 3 || r == 4 || r == 5 || r == 6 || r == 7) ? "the router refused the table"
                    : "no answer from the router";
    snprintf(net_fail_why, sizeof(net_fail_why), "%s", why);
    net_fail_retry = !(r == 210 || r == 211);  // 3.1-dev.111: rejections retry too in the background (the router may just have been fussy)
    Serial.printf("[NET] '%s' failed: %s (reason %u)\n", net_ssid, why, r);
    WiFi.disconnect(false);
    delay(200);
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[NET] home Wi-Fi unreachable -- using the table's own network");
    WiFi.disconnect(true);
    net_home_failed = true;
    return false;
  }
  net_fail_why[0] = 0;
  esp_wifi_set_ps(WIFI_PS_NONE);  // snappy replies + reliable ESP-NOW to the command consoles
  snprintf(net_url, sizeof(net_url), "http://%s", WiFi.localIP().toString().c_str());
  if (MDNS.begin("shadowfleet")) MDNS.addService("http", "tcp", 80);
  Serial.printf("[NET] on home Wi-Fi '%s' (channel %d): %s  /  http://shadowfleet.local\n",
                net_ssid, WiFi.channel(), net_url);
  return true;
}

static void startTableWifi() {
  // AP + an idle station: the station half lets /setup scan for networks.
  WiFi.mode(WIFI_AP_STA);
  // 3.1-dev.62: the quietest of channels 1 / 6 / 11 (was always 1), weighting each network by its signal
  { const int found = WiFi.scanNetworks(false, false, false, 120);
    long load[3] = {0, 0, 0};
    for (int j = 0; j < found; j++) {
      const int ch = WiFi.channel(j), w = WiFi.RSSI(j) + 100;  // -40 dBm counts 60, -90 counts 10
      for (int c = 0; c < 3; c++) { const int d = abs(ch - (1 + 5 * c)); if (d < 5) load[c] += (long)(w > 0 ? w : 1) * (5 - d); }
    }
    WiFi.scanDelete();
    int best = 0; for (int c = 1; c < 3; c++) if (load[c] < load[best]) best = c;
    net_ap_channel = 1 + 5 * best;
    Serial.printf("[NET] channel load 1:%ld 6:%ld 11:%ld -> channel %u\n", load[0], load[1], load[2], (unsigned)net_ap_channel); }
  // v0.22.0: room for 8 devices (default 4) -- 4 phones + a player board + spectators.
  WiFi.softAP("ShadowFleet", "starwars1", net_ap_channel, 0, 8);
  esp_wifi_set_ps(WIFI_PS_NONE);  // 3.1-dev.62: the station half's power save made the radio nap between phones' requests
  snprintf(net_url, sizeof(net_url), "http://%s", WiFi.softAPIP().toString().c_str());
  Serial.printf("[NET] AP 'ShadowFleet' started: %s\n", net_url);
  WiFi.scanNetworks(true, false, false, 120);  // v0.22.6: background scan, ready before anyone opens /setup
}

// v0.22.6: /setup used to scan inside the request -- seconds of a frozen
// loop, and the radio leaves the AP's channel meanwhile (the user "had to be
// patient"). Now scans run in the background and the page lists the last result.
static String net_found[15];
static int net_found_n = -1;          // -1 = no scan finished yet
static uint32_t net_scan_at = 0;

static void collectScan() {
  const int n = WiFi.scanComplete();  // -1 running, -2 none started
  if (n < 0) return;
  net_found_n = 0;
  for (int i = 0; i < n && net_found_n < 15; i++) {
    const String s = WiFi.SSID(i);
    bool dup = !s.length() || s == "ShadowFleet";
    for (int j = 0; j < net_found_n && !dup; j++) dup = net_found[j] == s;
    if (!dup) net_found[net_found_n++] = s;
  }
  WiFi.scanDelete();
  net_scan_at = millis();
}

static String htmlEsc(String s) {
  s.replace("&", "&amp;");
  s.replace("<", "&lt;");
  s.replace(">", "&gt;");
  s.replace("'", "&#39;");
  s.replace("\"", "&quot;");
  return s;
}

static const char SETUP_HEAD[] PROGMEM =
  "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
  "<title>Table Wi-Fi</title><style>body{margin:0;background:#05070C;color:#E6F1FA;font:16px system-ui,sans-serif;padding:24px 18px}"
  "h1{font-size:22px;letter-spacing:3px;margin:0 0 6px}p{color:#7C93A8;line-height:1.4}.msg{color:#FFD166}"
  "label{display:flex;gap:10px;align-items:center;padding:12px;border:1px solid #2A3B50;border-radius:12px;margin:8px 0}"
  "input[type=text],input[type=password]{width:100%;box-sizing:border-box;padding:14px;border-radius:12px;border:1px solid #2A3B50;"
  "background:#0B1420;color:#E6F1FA;font-size:16px;margin:6px 0}"
  "button{width:100%;padding:16px;border-radius:30px;border:2px solid #5FD4FF;background:#081A26;color:#5FD4FF;font-size:17px;"
  "font-weight:700;letter-spacing:2px;margin-top:14px}</style></head><body><h1>TABLE WI-FI</h1>";

// /setup: pick the home network on a PHONE (never the console keyboard).
static String setupPage(const String &msg) {
  String h;
  h.reserve(4000);
  h += FPSTR(SETUP_HEAD);
  if (msg.length()) h += "<p class='msg'>" + htmlEsc(msg) + "</p>";
  h += F("<p>Connect the Table Console to your home Wi-Fi. Phones then play on their own Wi-Fi: scan the code on the table. "
         "Somewhere without your Wi-Fi? Tap TABLE WI-FI on the table.</p><form method='POST' action='/setup'>");
  collectScan();
  if (net_found_n < 0 || millis() - net_scan_at > 60000) {
    if (WiFi.scanComplete() == -2) WiFi.scanNetworks(true, false, false, 120);  // refresh for the next visit
  }
  if (net_found_n < 0) {
    h = "";  // nothing yet: say so and reload in a moment
    h += FPSTR(SETUP_HEAD);
    h += F("<meta http-equiv='refresh' content='3'><p>Looking for Wi-Fi networks...</p></body></html>");
    return h;
  }
  for (int i = 0; i < net_found_n; i++) {
    const String s = htmlEsc(net_found[i]);
    h += "<label><input type='radio' name='ssid' value='" + s + "'" + (i == 0 ? " checked" : "") + "> " + s + "</label>";
  }
  // 3.1-dev.74 (user: "just keeps prompting saving the password and doesn't actually connect"): an iPhone fills the text box
  // before a password box with a saved username -- and that box used to WIN over the network you tapped. Now it counts only
  // when ANOTHER NETWORK is picked (typing in it picks it; autofill doesn't type), and neither box invites autofill.
  h += "<label><input type='radio' name='ssid' value='*other*' id='oth'" + String(net_found_n == 0 ? " checked" : "") + "> Another network</label>";
  h += F("<input type='text' name='other' placeholder='Network name' autocomplete='off' autocapitalize='none' autocorrect='off' spellcheck='false' onkeydown=\"document.getElementById('oth').checked=true\">"
         "<input type='password' name='pass' id='pw' placeholder='Password' autocomplete='off'>"
         "<label style='border:0;padding:4px 2px;margin:0'><input type='checkbox' onclick=\"document.getElementById('pw').type=this.checked?'text':'password'\"> Show password</label>"
         "<button type='submit'>CONNECT THE TABLE</button></form><p style='text-align:center'><a href='/' style='color:#7C93A8'>Back to the game</a></p></body></html>");
  return h;
}

// 3.1-dev.62 (playtest 2 at a Wi-Fi 7 venue: the table restarted with loopTask stuck >8 s sending to ONE phone on a weak
// link): big answers go out through our own non-blocking loop. A phone that takes nothing for 2.5 s, or isn't done
// after 12 s, is cut off (its browser asks again); the core's writer waits up to 10 s per stalled chunk, and while it
// waits every other phone's poll waits too.
static bool sendBounded(const char *type, const uint8_t *data, size_t len, bool gz) {
  if (gz) server.sendHeader("Content-Encoding", "gzip");
  server.setContentLength(len);
  server.send(200, type, "");  // the headers only
  NetworkClient &c = server.client();
  const int fd = c.fd();
  if (fd < 0) return false;
  const uint32_t t0 = millis();
  uint32_t last = t0;
  size_t off = 0;
  while (off < len) {
    // 3.1-dev.112: every byte queued here sits in INTERNAL RAM until the phone acks it (~5.7 KB a phone) -- with 4 phones
    // reloading at once that drained the heap to 2-5 KB. Below SEND_RAM_FLOOR, queue nothing more: wait for acks.
    if (ramLow(SEND_RAM_FLOOR)) {
      const uint32_t now = millis();
      if (now - last > 2500 || now - t0 > 12000) break;
      net_send_waits++;
      delay(5);
      continue;
    }
    const int r = send(fd, data + off, len - off > 2920 ? 2920 : len - off, MSG_DONTWAIT);
    if (r > 0) { off += r; last = millis(); continue; }
    if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK) break;
    const uint32_t now = millis();
    if (now - last > 2500 || now - t0 > 12000) break;
    fd_set ws; FD_ZERO(&ws); FD_SET(fd, &ws);
    struct timeval tv = {0, 50000};
    select(fd + 1, nullptr, &ws, nullptr, &tv);
  }
  if (off < len) {
    net_slow_drops++;
    Serial.printf("[NET] cut off a slow phone (%s): %u of %u bytes in %u ms\n", c.remoteIP().toString().c_str(), (unsigned)off, (unsigned)len, (unsigned)(millis() - t0));
    c.stop();
    return false;
  }
  return true;
}
String networkHealthLine() {
  char b[240];
  snprintf(b, sizeof(b), "Wi-Fi: %s '%s'%s%s, drops %u (last reason %u), slow phones cut off %u, low-RAM send waits %u, AP channel %u\n",
           net_home ? "home" : "own network", net_home ? net_ssid : "ShadowFleet", net_fail_why[0] ? " -- " : "", net_fail_why,
           (unsigned)net_drops_n, (unsigned)net_join_reason, (unsigned)net_slow_drops, (unsigned)net_send_waits, (unsigned)net_ap_channel);
  return String(b);
}

static String netInfoJson() {
  String ss[NET_SAVED], pw[NET_SAVED];
  const int n = netLoad(ss, pw);
  String j = "{\"home\":" + String(net_home ? "true" : "false") + ",\"canHome\":" + String(net_allow_home ? "true" : "false") +
             ",\"failed\":" + String(net_home_failed ? "true" : "false") + ",\"ssid\":" + jsonStr(net_ssid) + ",\"why\":" + jsonStr(net_fail_why) + ",\"saved\":[";
  for (int i = 0; i < n; i++) j += (i ? "," : "") + jsonStr(ss[i].c_str());
  return j + "]}";
}

static void handleSetupPost() {
  String ssid = server.arg("ssid");
  if (ssid == "*other*") ssid = server.arg("other");  // 3.1-dev.74: the typed name only when ANOTHER NETWORK is picked
  ssid.trim();
  if (!ssid.length() || ssid.length() > 32) { server.send(200, "text/html; charset=utf-8", setupPage("Pick a network first.")); return; }
  String ss[NET_SAVED], pw[NET_SAVED], ns[NET_SAVED], np[NET_SAVED];
  const int n = netLoad(ss, pw);
  int m = 0;
  ns[m] = ssid; np[m++] = server.arg("pass");  // newest first; the same name replaces its old entry
  for (int i = 0; i < n && m < NET_SAVED; i++) if (ss[i] != ssid) { ns[m] = ss[i]; np[m++] = pw[i]; }
  netStore(ns, np, m);
  Preferences p;
  p.begin("sfnet", false);
  p.putBool("home", true);
  p.end();
  const String e = htmlEsc(ssid);
  String h;
  h += FPSTR(SETUP_HEAD);
  h += "<p class='msg'>Saved. The table is restarting and joining <b>" + e + "</b>.</p>"
       "<p>Switch this phone back to " + e + ", then scan the code on the table.</p></body></html>";
  server.send(200, "text/html; charset=utf-8", h);
  Serial.printf("[NET] home Wi-Fi saved: '%s' -- restarting\n", ssid.c_str());
  net_restart_at = millis() + 1500;
}

void networkInit(bool allowHome) {
  net_allow_home = allowHome;
  net_home = allowHome && joinHomeWifi();
  if (!net_home) startTableWifi();

  server.on("/setup", HTTP_GET, []() { server.send(200, "text/html; charset=utf-8", setupPage("")); });
  server.on("/setup", HTTP_POST, handleSetupPost);
  server.on("/netinfo", HTTP_GET, []() { server.send(200, "application/json", netInfoJson()); });  // 3.1-dev.61 SETTINGS
  server.on("/net_forget", HTTP_POST, []() {
    String ss[NET_SAVED], pw[NET_SAVED], ns[NET_SAVED], np[NET_SAVED];
    const int n = netLoad(ss, pw), x = server.arg("i").toInt();
    int m = 0;
    for (int i = 0; i < n; i++) if (i != x) { ns[m] = ss[i]; np[m++] = pw[i]; }
    if (x >= 0 && x < n) { netStore(ns, np, m); Serial.printf("[NET] forgot '%s'\n", ss[x].c_str()); }
    server.send(200, "application/json", netInfoJson());
  });
  server.on("/net_mode", HTTP_POST, []() {
    server.send(200, "application/json", "{\"ok\":true}");
    networkSwitchMode(server.arg("home") == "1");
  });

  // 3.1-dev.35 (playtest 2026-11-08: phones on a busy Wi-Fi took seconds to load the 391 KB page, and the table
  // ran out of memory doing it): the page goes out GZIPPED (113 KB), and a phone that already has this build's
  // page gets a "304 Not Modified" (a few bytes) -- reloads and "back to the war" stop re-downloading it.
  server.on("/", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-cache");  // always ask, usually answered with 304
    server.sendHeader("ETag", page_etag);
    if (server.header("If-None-Match") == page_etag) { server.send(304); return; }
    if (ramLow(BIG_RAM_FLOOR)) {  // 3.1-dev.110: a tiny page that tries again in a moment, instead of 200 KB into an empty heap
      server.sendHeader("Cache-Control", "no-store");
      server.send(503, "text/html", "<!doctype html><meta name=viewport content='width=device-width'><meta http-equiv=refresh content='2'>"
                  "<body style='background:#05070C;color:#8FA3B8;font:16px sans-serif;text-align:center;padding-top:40vh'>Waking up the table&hellip;</body>");
      return;
    }
    // 3.1-dev.87 (flash at 91%): only the gzipped page is in flash now (~600 KB saved) -- every phone browser takes gzip.
    sendBounded("text/html; charset=utf-8", SF_PAGE_GZ, SF_PAGE_GZ_LEN, true);  // 3.1-dev.62
  });

  server.on("/state", HTTP_GET, []() {
    // 3.1-dev.74: a phone that already has the game (h=) waits a moment when internal RAM is nearly gone -- at another house
    // RAM reached 0 KB while every phone reloaded at once, and the table restarted twice.
    if (server.hasArg("h") && heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < STATE_RAM_FLOOR) { server.send(503, "application/json", "{\"busy\":1}"); return; }
    if (!server.hasArg("h") && ramLow(BIG_RAM_FLOOR)) { server.send(503, "application/json", "{\"busy\":1}"); return; }  // 3.1-dev.110: a phone's FIRST full answer waits too
    sendStateJson();
  });
  // 2.0-dev.6: crash records + uptime + RAM, readable from any phone (sf_health.h)
  server.on("/health", HTTP_GET, []() { server.send(200, "text/plain; charset=utf-8", healthReport() + networkHealthLine()); });
  // 3.1-dev.122 SAVE A REPORT (user: "make these logs a saveable .txt ... to address bugs when we disperse it"): one text file --
  // the table's health (crash records, RAM lows), the game records, and this game's last battles.
  server.on("/report", HTTP_GET, []() {
    String r = String("Shadow Fleet report\n\n== THE TABLE ==\n") + healthReport() + networkHealthLine() + "\n== GAMES (one line each) ==\n";
    File f = LittleFS.open("/games.txt", "r");
    if (f) { const size_t n = f.size(); const size_t from = n > 6000 ? n - 6000 : 0; f.seek(from); while (f.available()) r += (char)f.read(); f.close(); }
    else r += "none yet\n";
    GameState &s = webState(); gameStateReadInto(s);
    r += "\n== THIS GAME ==\nstage " + String(s.stage) + ", round " + String(s.round) + " of " + String(s.roundLimit) + ", phase " + String(s.phase) + "\n";
    for (int i = 0; i < MAX_PLAYERS; i++) if (s.players[i].active)
      r += "P" + String(i + 1) + " faction " + String(s.players[i].chosenFaction) + " character " + String(s.players[i].commander) + " VP " + String(s.players[i].vp) + " CR " + String(s.players[i].credits) + "\n";
    r += "\n== LAST BATTLES (round kind atk def world start-ships result) ==\n";
    for (int k = 1; k <= BATTLE_RECS; k++) {
      const BattleRec &b = s.battleRecs[(s.battleRecHead + BATTLE_RECS - k) % BATTLE_RECS];
      if (b.round < 0 || !b.n) continue;
      r += "r" + String(b.round) + " k" + String(b.kind) + " P" + String(b.atk + 1) + " vs " + (b.def >= 0 ? "P" + String(b.def + 1) : String("world")) + " w" + String(b.planet) + " " + String(b.a0) + "v" + String(b.d0) + " res" + String(b.result) + "\n";
    }
    server.sendHeader("Content-Disposition", "attachment; filename=shadowfleet_report.txt");
    server.send(200, "text/plain; charset=utf-8", r);
  });

  server.on("/join", HTTP_POST, []() {
    String id = server.arg("id");
    int requestedSlot = server.arg("slot").toInt();
    // No ?name= any more: neither client has sent one since v0.15.1 (your
    // name is your faction), and accepting it let arbitrary text into
    // state that other phones render via innerHTML. Empty = "PLAYER" until
    // a faction is picked.
    int slot = gameStateJoinSlot(id.c_str(), "", requestedSlot);
    server.send(200, "application/json", "{\"slot\":" + String(slot) + "}");
  });

  // 2.0-dev.13 drop out / rejoin: STEP AWAY, COME BACK, close the welcome card.
  server.on("/away", HTTP_POST, []() {
    gameStateReadInto(webState());
    const int slot = slotForIdIn(webState(), server.arg("id"));  // not slotForId: that would count as "here"
    if (slot >= 0) gameStateStepAway(slot, true);
    server.send(200, "application/json", "{\"ok\":true}");
  });
  server.on("/here", HTTP_POST, []() {  // a remote twin viewer: still here (slotForId does it)
    slotForId(server.arg("id"));
    server.send(200, "application/json", "{\"ok\":true}");
  });
  server.on("/back", HTTP_POST, []() {
    const int slot = slotForId(server.arg("id"));
    if (slot >= 0) gameStateStepAway(slot, false);
    server.send(200, "application/json", "{\"ok\":true}");
  });
  server.on("/welcome_ack", HTTP_POST, []() {
    const int slot = slotForId(server.arg("id"));
    if (slot >= 0) gameStateWelcomeAck(slot);
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/choose_faction", HTTP_POST, []() {
    int slot = slotForId(server.arg("id"));
    int faction = server.arg("faction").toInt();
    bool ok = (slot >= 0) && gameStateChooseFaction(slot, faction);
    server.send(200, "application/json", String("{\"ok\":") + (ok ? "true" : "false") + "}");
  });

  // 3.1-dev.6: pick a CHARACTER (id = faction * 4 + class) -- its faction, then it.
  // Two separate calls, never nested (mutex rule); a taken faction stops at step one.
  server.on("/choose_character", HTTP_POST, []() {
    int slot = slotForId(server.arg("id"));
    const int c = server.arg("character").toInt();
    bool ok = slot >= 0 && c >= 0 && c < NUM_COMMANDERS
              && gameStateChooseFaction(slot, c / COMMANDERS_PER_FACTION) && gameStateChooseCommander(slot, c);
    server.send(200, "application/json", String("{\"ok\":") + (ok ? "true" : "false") + "}");
  });

  // v0.21.20: pick your commander (id = faction * COMMANDERS_PER_FACTION + i).
  server.on("/choose_commander", HTTP_POST, []() {
    int slot = slotForId(server.arg("id"));
    bool ok = (slot >= 0) && gameStateChooseCommander(slot, server.arg("commander").toInt());
    server.send(200, "application/json", String("{\"ok\":") + (ok ? "true" : "false") + "}");
  });

  // v0.21.20: the commander portraits + names, from the same generated data
  // the AMOLED draws (commander_art.cpp) -- the phone keeps no copy. Fetched
  // once per page load. p = palette as 6-hex runs (entry 0 unused), x = 2 hex
  // chars per pixel, row by row (00 = transparent).
  // 3.1-dev.4: the title font (phones on the table's Wi-Fi have no internet).
  // Cached for a year, so each phone downloads its ~22 KB once.
  server.on("/icon.png", HTTP_GET, []() {  // 3.1-dev.18: "Add to Home Screen" = an app
    server.sendHeader("Cache-Control", "public, max-age=31536000, immutable");
    sendBounded("image/png", SF_WEB_ICON, SF_WEB_ICON_LEN, false);
  });
  server.on("/manifest.json", HTTP_GET, []() {
    server.send(200, "application/manifest+json",
      "{\"name\":\"Shadow Fleet\",\"short_name\":\"Shadow Fleet\",\"start_url\":\"/\",\"display\":\"standalone\",\"orientation\":\"portrait\","
      "\"background_color\":\"#05070C\",\"theme_color\":\"#05070C\",\"icons\":[{\"src\":\"/icon.png\",\"sizes\":\"180x180\",\"type\":\"image/png\"}]}");
  });
  server.on("/font.woff2", HTTP_GET, []() {
    if (ramLow(BIG_RAM_FLOOR)) { server.send(503, "text/plain", "busy"); return; }  // 3.1-dev.110 (the phone falls back to its own font this once)
    server.sendHeader("Cache-Control", "public, max-age=31536000, immutable");
    sendBounded("font/woff2", SF_WEB_FONT, SF_WEB_FONT_LEN, false);
  });
  server.on("/art", HTTP_GET, []() {
    // 3.1-dev.35: the art never changes within a build -- a phone that has it gets a 304
    server.sendHeader("Cache-Control", "no-cache");
    server.sendHeader("ETag", art_etag);
    if (server.header("If-None-Match") == art_etag) { server.send(304); return; }
    if (ramLow(BIG_RAM_FLOOR)) { server.send(503, "application/json", "{\"busy\":1}"); return; }  // 3.1-dev.110 (the phone asks again)
    static const char HEXD[] = "0123456789abcdef";  // not HEX: that's an Arduino macro
    String j;
    j.reserve(NUM_COMMANDERS * (COMMANDER_ART_W * COMMANDER_ART_H * 2 + 400) + 64);
    j += "{\"w\":" + String(COMMANDER_ART_W) + ",\"h\":" + String(COMMANDER_ART_H) + ",\"c\":[";
    for (int i = 0; i < NUM_COMMANDERS; i++) {
      const CommanderArt &a = COMMANDER_ART[i];
      if (i) j += ",";
      j += "{\"n\":" + jsonStr(COMMANDER_INFO[i].name) + ",\"b\":" + jsonStr(COMMANDER_INFO[i].bio) + ",\"p\":\"";
      for (int k = 0; k < a.nColors; k++) for (int s = 20; s >= 0; s -= 4) j += HEXD[(a.pal[k] >> s) & 15];
      j += "\",\"x\":\"";
      for (int k = 0; k < COMMANDER_ART_W * COMMANDER_ART_H; k++) { j += HEXD[a.px[k] >> 4]; j += HEXD[a.px[k] & 15]; }
      j += "\"}";
    }
    j += "]}";
    server.send(200, "application/json", j);
  });

  // v0.21.19: any seated player can start the game (was AMOLED-only -- the
  // phone Lobby said "waiting for host"). gameStateStartGame() itself refuses
  // outside the Lobby or while someone still has no faction.
  server.on("/start_game", HTTP_POST, []() {
    if (slotForId(server.arg("id")) >= 0) gameStateStartGame();
    server.send(200, "application/json", "{\"ok\":true}");
  });

  // v0.27.3: back to a fresh Lobby once the game is over (any seated player).
  server.on("/new_game", HTTP_POST, []() {
    if (slotForId(server.arg("id")) >= 0) gameStateNewGame();
    server.send(200, "application/json", "{\"ok\":true}");
  });

  // v0.30.3: end a game in progress (any seated player; the page confirms first).
  // 2.0: buy at an open market / send your diplomat (both Prepare only; the rules check everything).
  // 3.1-dev.13 THE ONCE-A-GAME MOVES
  server.on("/war", HTTP_POST, []() { const int slot = slotForId(server.arg("id"));
    const bool ok = slot >= 0 && gameStateDeclareWar(slot, server.arg("planet").toInt());
    server.send(200, "application/json", String("{\"ok\":") + (ok ? "true" : "false") + "}"); });
  server.on("/war_room", HTTP_POST, []() { const int slot = slotForId(server.arg("id"));
    int8_t pull[NUM_PLANETS] = {};
    const String q = server.arg("pull");  // "p:n,p:n"
    int at = 0;
    while (at < (int)q.length()) {
      const int colon = q.indexOf(':', at); if (colon < 0) break;
      int comma = q.indexOf(',', colon); if (comma < 0) comma = q.length();
      const int p = q.substring(at, colon).toInt(), n = q.substring(colon + 1, comma).toInt();
      if (p >= 0 && p < NUM_PLANETS) pull[p] = (int8_t)constrain(n, 0, 20);
      at = comma + 1;
    }
    const bool ok = slot >= 0 && gameStateWarRoom(slot, pull, server.arg("march").toInt() != 0);
    server.send(200, "application/json", String("{\"ok\":") + (ok ? "true" : "false") + "}"); });
  // 3.1-dev.43 THE FINAL HOUR
  // 3.1-dev.48 SELL IT TWICE
  server.on("/sell_twice", HTTP_POST, []() { const int slot = slotForId(server.arg("id"));
    const bool ok = slot >= 0 && gameStateSellTwice(slot, server.arg("sell").toInt() != 0);
    server.send(200, "application/json", String("{\"ok\":") + (ok ? "true" : "false") + "}"); });
  server.on("/gallery", HTTP_POST, []() { const int slot = slotForId(server.arg("id"));  // 3.1-dev.52 THE GALLERY
    const char *why = slot >= 0 ? gameStateGallery(slot, server.arg("talker").toInt(), server.arg("w").toInt() != 0) : "Not seated.";
    server.send(200, "application/json", String("{\"ok\":") + (why ? "false,\"why\":" + jsonStr(why) : String("true")) + "}"); });
  server.on("/swap_title", HTTP_POST, []() { const int slot = slotForId(server.arg("id"));  // 3.1-dev.49 ONE TITLE
    const bool ok = slot >= 0 && gameStateSwapTitle(slot);
    server.send(200, "application/json", String("{\"ok\":") + (ok ? "true" : "false") + "}"); });
  server.on("/pay_back", HTTP_POST, []() { const int slot = slotForId(server.arg("id"));
    const bool ok = slot >= 0 && gameStatePayBack(slot, server.arg("f").toInt());
    server.send(200, "application/json", String("{\"ok\":") + (ok ? "true" : "false") + "}"); });
  server.on("/fh_pick", HTTP_POST, []() { const int slot = slotForId(server.arg("id"));
    const bool ok = slot >= 0 && gameStateFhPick(slot, server.arg("k").toInt());
    server.send(200, "application/json", String("{\"ok\":") + (ok ? "true" : "false") + "}"); });
  server.on("/fh_buy", HTTP_POST, []() { const int slot = slotForId(server.arg("id"));
    const bool ok = slot >= 0 && gameStateFhBuy(slot, server.arg("k").toInt());
    server.send(200, "application/json", String("{\"ok\":") + (ok ? "true" : "false") + "}"); });
  server.on("/fh_buy_big", HTTP_POST, []() { const int slot = slotForId(server.arg("id"));  // 3.1-dev.53
    const char *why = slot >= 0 ? gameStateFhBuyBig(slot, server.arg("k").toInt(), server.hasArg("w") ? server.arg("w").toInt() : -1) : "Not seated.";
    server.send(200, "application/json", String("{\"ok\":") + (why ? "false,\"why\":" + jsonStr(why) : String("true")) + "}"); });
  server.on("/fh_debt", HTTP_POST, []() { const int slot = slotForId(server.arg("id"));
    const bool ok = slot >= 0 && gameStateFhDebt(slot, server.arg("f").toInt(), server.arg("n").toInt());
    server.send(200, "application/json", String("{\"ok\":") + (ok ? "true" : "false") + "}"); });
  server.on("/fh_orders", HTTP_POST, []() { const int slot = slotForId(server.arg("id"));
    int8_t tt[FH_STRIKES], gu[3], g[FH_STRIKES]; uint8_t al[FH_STRIKES];
    auto list = [](const String &q, int8_t *out, int n) { int at = 0; for (int k = 0; k < n; k++) { int c = q.indexOf(',', at); if (c < 0) c = q.length(); out[k] = at < (int)q.length() ? (int8_t)constrain(q.substring(at, c).toInt(), -1, 100) : -1; at = c + 1; } };
    int8_t a8[FH_STRIKES];
    list(server.arg("t"), tt, FH_STRIKES); list(server.arg("gu"), gu, 3); list(server.arg("g"), g, FH_STRIKES); list(server.arg("al"), a8, FH_STRIKES);
    int8_t h8[FH_STRIKES]; list(server.arg("h"), h8, FH_STRIKES);  // 3.1-dev.116 hired ships per strike
    for (int k = 0; k < FH_STRIKES; k++) al[k] = (uint8_t)(min(15, max(0, (int)a8[k])) | (min(15, max(0, (int)h8[k])) << 4));
    const char *why = slot >= 0 ? gameStateFhOrders(slot, tt, gu, al, g) : "Not seated.";
    server.send(200, "application/json", String("{\"ok\":") + (why ? "false,\"why\":" + jsonStr(why) : String("true")) + "}"); });
  server.on("/fh_skip", HTTP_POST, []() { const int slot = slotForId(server.arg("id"));  // 3.1-dev.117 a vote to skip this finale step
    server.send(200, "application/json", String("{\"ok\":") + (slot >= 0 && gameStateFhSkip(slot) ? "true" : "false") + "}"); });
  server.on("/fh_reveal", HTTP_POST, []() { const int slot = slotForId(server.arg("id"));
    int8_t g[FH_HAND], a[FH_HAND], b[FH_HAND];
    auto list = [](const String &q, int8_t *out, int n) { int at = 0; for (int k = 0; k < n; k++) { int c = q.indexOf(',', at); if (c < 0) c = q.length(); out[k] = at < (int)q.length() ? (int8_t)constrain(q.substring(at, c).toInt(), -1, 100) : -1; at = c + 1; } };
    list(server.arg("g"), g, FH_HAND); list(server.arg("a"), a, FH_HAND); list(server.arg("b"), b, FH_HAND);
    const char *why = slot >= 0 ? gameStateFhReveal(slot, g, a, b) : "Not seated.";
    server.send(200, "application/json", String("{\"ok\":") + (why ? "false,\"why\":" + jsonStr(why) : String("true")) + "}"); });
  server.on("/war_back", HTTP_POST, []() { const int slot = slotForId(server.arg("id"));
    const bool ok = slot >= 0 && gameStateWarPullBack(slot);
    server.send(200, "application/json", String("{\"ok\":") + (ok ? "true" : "false") + "}"); });
  server.on("/summit", HTTP_POST, []() { const int slot = slotForId(server.arg("id"));
    int w[SUMMIT_WORLDS], n = 0;
    const String q = server.arg("w");
    int at = 0;
    while (at < (int)q.length() && n < SUMMIT_WORLDS) { int comma = q.indexOf(',', at); if (comma < 0) comma = q.length(); w[n++] = q.substring(at, comma).toInt(); at = comma + 1; }
    const bool ok = slot >= 0 && gameStateCallSummit(slot, w, n);
    server.send(200, "application/json", String("{\"ok\":") + (ok ? "true" : "false") + "}"); });
  server.on("/break", HTTP_POST, []() { const int slot = slotForId(server.arg("id"));
    const bool ok = slot >= 0 && gameStateBreak(slot, server.arg("planet").toInt());
    server.send(200, "application/json", String("{\"ok\":") + (ok ? "true" : "false") + "}"); });
  server.on("/cover", HTTP_POST, []() { const int slot = slotForId(server.arg("id"));
    const bool ok = slot >= 0 && gameStateDeepCover(slot, server.arg("f").toInt());
    server.send(200, "application/json", String("{\"ok\":") + (ok ? "true" : "false") + "}"); });
  server.on("/story", HTTP_POST, []() { const int slot = slotForId(server.arg("id"));
    const bool ok = slot >= 0 && gameStateSetStory(slot, server.arg("text").c_str());
    server.send(200, "application/json", String("{\"ok\":") + (ok ? "true" : "false") + "}"); });
  server.on("/heist", HTTP_POST, []() { const int slot = slotForId(server.arg("id"));
    const bool ok = slot >= 0 && gameStateHeist(slot, server.arg("f").toInt(), server.arg("bold").toInt() != 0);
    server.send(200, "application/json", String("{\"ok\":") + (ok ? "true" : "false") + "}"); });
  server.on("/decree", HTTP_POST, []() {  // 3.1-dev.12 a champion declares their faction's Era
    const int slot = slotForId(server.arg("id"));
    const bool ok = slot >= 0 && gameStateDeclareEra(slot, server.arg("f").toInt());
    server.send(200, "application/json", String("{\"ok\":") + (ok ? "true" : "false") + "}");
  });
  server.on("/lie", HTTP_POST, []() {  // 3.1-dev.12 the Shadow Age champion's lie for this round
    const int slot = slotForId(server.arg("id"));
    const bool ok = slot >= 0 && gameStateSetLie(slot, server.arg("text").c_str());
    server.send(200, "application/json", String("{\"ok\":") + (ok ? "true" : "false") + "}");
  });
  server.on("/heist_push", HTTP_POST, []() {  // 3.1-dev.20 push=1 roll again / 0 take the money
    const int slot = slotForId(server.arg("id"));
    const bool ok = slot >= 0 && gameStateHeistPush(slot, server.arg("push").toInt() != 0);
    server.send(200, "application/json", String("{\"ok\":") + (ok ? "true" : "false") + "}");
  });
  server.on("/grudge_pay", HTTP_POST, []() {  // 3.1-dev.20 pay a grudge off
    const int slot = slotForId(server.arg("id"));
    const bool ok = slot >= 0 && gameStatePayOffGrudge(slot);
    server.send(200, "application/json", String("{\"ok\":") + (ok ? "true" : "false") + "}");
  });
  server.on("/grudge_deed", HTTP_POST, []() {  // 3.1-dev.42 make amends: k = 0 ship / 1 round / 2 world, w = the world (or -1)
    const int slot = slotForId(server.arg("id"));
    const char *why = slot < 0 ? "You're not in this game." : gameStateGrudgeDeed(slot, server.arg("k").toInt(), server.hasArg("w") ? server.arg("w").toInt() : -1);
    server.send(200, "application/json", String("{\"ok\":") + (why ? String("false,\"error\":") + jsonStr(why) : String("true")) + "}");
  });
  server.on("/job", HTTP_POST, []() {  // 3.1-dev.10 a job offer: take=1 / pass=0
    const int slot = slotForId(server.arg("id"));
    const bool ok = slot >= 0 && gameStateAnswerJob(slot, server.arg("take").toInt() != 0);
    server.send(200, "application/json", String("{\"ok\":") + (ok ? "true" : "false") + "}");
  });
  server.on("/stall_buy", HTTP_POST, []() {  // 3.1-dev.9 faction stalls
    const int slot = slotForId(server.arg("id"));
    const bool ok = slot >= 0 && gameStateStallBuy(slot, server.arg("f").toInt(), server.arg("k").toInt(), server.hasArg("t") ? server.arg("t").toInt() : -1);
    server.send(200, "application/json", String("{\"ok\":") + (ok ? "true" : "false") + "}");
  });
  server.on("/market_buy", HTTP_POST, []() {
    const int slot = slotForId(server.arg("id"));
    const bool ok = slot >= 0 && gameStateMarketBuy(slot, server.arg("planet").toInt());
    server.send(200, "application/json", String("{\"ok\":") + (ok ? "true" : "false") + "}");
  });

  // 2.0 deals. The refusal reason comes back so the form can say why.
  server.on("/ashen_strike", HTTP_POST, []() {  // 2.0: fleets = a bit per fleet
    const int slot = slotForId(server.arg("id"));
    const int mask = server.arg("fleets").toInt();
    const char *why = "You're not in this game.";
    if (slot >= 0) { GameState &g = webState(); gameStateReadInto(g); why = snapshotAshenStrikeRefusal(g, slot, mask); }
    const int dmg = why ? -1 : gameStateAshenStrike(slot, mask);
    server.send(200, "application/json", String("{\"ok\":") + (dmg >= 0 ? "true" : "false") + ",\"why\":" + jsonStr(dmg >= 0 ? "" : (why ? why : "Couldn't strike.")) + "}");
  });

  server.on("/end_game", HTTP_POST, []() {
    if (slotForId(server.arg("id")) >= 0) gameStateEndGame();
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/set_round_limit", HTTP_POST, []() {
    // Any player can set this while still in the lobby; gameStateSetRoundLimit
    // itself rejects the change once the game has started, and only accepts
    // 6/8/10, so no extra validation needed here.
    int rounds = server.arg("rounds").toInt();
    gameStateSetRoundLimit(rounds);
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/lockin", HTTP_POST, []() {
    int slot = slotForId(server.arg("id"));
    if (slot >= 0) gameStateLockIn(slot);
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/reveal", HTTP_POST, []() {
    int slot = slotForId(server.arg("id"));
    if (slot >= 0) gameStateRevealCharacteristic(slot, server.arg("planet").toInt(), server.arg("char").toInt());
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/games", HTTP_GET, []() {  // 3.1-dev.101 THE GAME RECORD: one line per finished game (who won, as what)
    File f = LittleFS.open("/games.txt", "r");
    if (!f) { server.send(200, "text/plain", "no games recorded yet\n"); return; }
    server.streamFile(f, "text/plain");
    f.close();
  });
  server.on("/battles", HTTP_GET, []() {  // 3.1-dev.80 BATTLE REPLAY: the last battles, newest first (asked for only when History opens)
    GameState &s = webState();
    gameStateReadInto(s);
    String j = "[";
    bool first = true;
    for (int k = 1; k <= BATTLE_RECS; k++) {
      const BattleRec &r = s.battleRecs[(s.battleRecHead + BATTLE_RECS - k) % BATTLE_RECS];
      if (r.round < 0 || !r.n) continue;
      j += String(first ? "" : ",") + "{\"r\":" + String(r.round) + ",\"k\":" + String(r.kind) + ",\"a\":" + String(r.atk) + ",\"d\":" + String(r.def) +
           ",\"p\":" + String(r.planet) + ",\"a0\":" + String(r.a0) + ",\"d0\":" + String(r.d0) + ",\"res\":" + String(r.result) + ",\"rs\":" + String(r.retreat) + ",\"log\":[";
      for (int e = 0; e < r.n; e++) j += String(e ? "," : "") + String(r.log[e]);
      j += "]}"; first = false;
    }
    server.send(200, "application/json", j + "]");
  });
  server.on("/buyout_answer", HTTP_POST, []() {  // 3.1-dev.79
    int slot = slotForId(server.arg("id"));
    const bool ok = slot >= 0 && gameStateBuyoutAnswer(slot, server.arg("p").toInt(), server.arg("match") == "1");
    server.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false,\"why\":\"You can\u2019t match that.\"}");
  });
  server.on("/file_pick", HTTP_POST, []() {  // 3.1-dev.78
    int slot = slotForId(server.arg("id"));
    const bool ok = slot >= 0 && gameStatePickFile(slot, server.arg("t").toInt());
    server.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
  });
  server.on("/blackmail", HTTP_POST, []() {
    int slot = slotForId(server.arg("id"));
    const char *why = slot >= 0 ? gameStateBlackmail(slot, server.arg("t").toInt()) : "Not seated.";
    server.send(200, "application/json", why ? "{\"ok\":false,\"why\":" + jsonStr(why) + "}" : String("{\"ok\":true}"));
  });
  server.on("/blackmail_answer", HTTP_POST, []() {
    int slot = slotForId(server.arg("id"));
    const bool ok = slot >= 0 && gameStateBlackmailAnswer(slot, server.arg("pay") == "1");
    server.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false,\"why\":\"You don\u2019t have 8 CR.\"}");
  });
  server.on("/cargo_use", HTTP_POST, []() {  // 3.1-dev.88 THE CARGO HOLD
    int slot = slotForId(server.arg("id"));
    const char *why = slot >= 0 ? gameStateUseCargo(slot, server.arg("k").toInt(), server.hasArg("a") ? server.arg("a").toInt() : -1) : "Not seated.";
    server.send(200, "application/json", why ? "{\"ok\":false,\"why\":" + jsonStr(why) + "}" : String("{\"ok\":true}"));
  });
  server.on("/bc_offer", HTTP_POST, []() {  // 3.1-dev.98 THE BACK CHANNEL
    int slot = slotForId(server.arg("id"));
    const char *why = slot >= 0 ? gameStateBcOffer(slot, server.arg("t").toInt(), server.arg("k").toInt(), server.hasArg("a") ? server.arg("a").toInt() : -1, server.arg("cr").toInt()) : "Not seated.";
    server.send(200, "application/json", why ? "{\"ok\":false,\"why\":" + jsonStr(why) + "}" : String("{\"ok\":true}"));
  });
  server.on("/gfh_offer", HTTP_POST, []() {  // 3.1-dev.102 GUNS FOR HIRE
    int slot = slotForId(server.arg("id"));
    const char *why = slot >= 0 ? gameStateGfhOffer(slot, server.arg("t").toInt(), (uint32_t)strtoul(server.arg("m").c_str(), nullptr, 10), server.arg("cr").toInt()) : "Not seated.";
    server.send(200, "application/json", why ? "{\"ok\":false,\"why\":" + jsonStr(why) + "}" : String("{\"ok\":true}"));
  });
  server.on("/gfh_answer", HTTP_POST, []() {
    int slot = slotForId(server.arg("id"));
    const bool ok = slot >= 0 && gameStateGfhAnswer(slot, server.arg("yes") == "1");
    server.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
  });
  server.on("/standby", HTTP_POST, []() {  // 3.1-dev.102 STAND BY
    int slot = slotForId(server.arg("id"));
    const char *why = slot >= 0 ? gameStateStandBy(slot, server.arg("f").toInt()) : "Not seated.";
    server.send(200, "application/json", why ? "{\"ok\":false,\"why\":" + jsonStr(why) + "}" : String("{\"ok\":true}"));
  });
  server.on("/bc_answer", HTTP_POST, []() {
    int slot = slotForId(server.arg("id"));
    const bool ok = slot >= 0 && gameStateBcAnswer(slot, server.arg("yes") == "1");
    server.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
  });
  server.on("/buy_file", HTTP_POST, []() {  // 3.1-dev.88 the Dawn's Era
    int slot = slotForId(server.arg("id"));
    const char *why = slot >= 0 ? gameStateBuyFile(slot, server.arg("t").toInt()) : "Not seated.";
    server.send(200, "application/json", why ? "{\"ok\":false,\"why\":" + jsonStr(why) + "}" : String("{\"ok\":true}"));
  });
  server.on("/name_mark", HTTP_POST, []() {  // 3.1-dev.88 the Dawn's champion
    int slot = slotForId(server.arg("id"));
    const bool ok = slot >= 0 && gameStateNameMark(slot, server.arg("t").toInt());
    server.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
  });
  server.on("/bm_lot", HTTP_POST, []() {  // 3.1-dev.77
    int slot = slotForId(server.arg("id"));
    const char *why = slot >= 0 ? gameStateBuyLot(slot, server.arg("k").toInt(), server.hasArg("w") ? server.arg("w").toInt() : -1) : "Not seated.";
    server.send(200, "application/json", why ? "{\"ok\":false,\"why\":" + jsonStr(why) + "}" : String("{\"ok\":true}"));
  });
  server.on("/patrol", HTTP_POST, []() {  // 3.1-dev.76
    int slot = slotForId(server.arg("id"));
    const bool ok = slot >= 0 && gameStatePatrol(slot, server.arg("planet").toInt());
    server.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
  });
  server.on("/invest", HTTP_POST, []() {
    int slot = slotForId(server.arg("id"));
    if (slot >= 0) gameStateInvestDefense(slot, server.arg("planet").toInt());
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/invest_fleet", HTTP_POST, []() {
    int slot = slotForId(server.arg("id"));
    if (slot >= 0) gameStateInvestFleet(slot, server.arg("fleet").toInt());
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/takeover_begin", HTTP_POST, []() {
    int slot = slotForId(server.arg("id"));
    if (slot >= 0) gameStateBeginTakeover(slot, server.arg("planet").toInt());
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/takeover_commit", HTTP_POST, []() {
    int slot = slotForId(server.arg("id"));
    int method = server.arg("method").toInt();
    // FIGHT from the battle briefing names its fleet; without one, the
    // first fleet with ships attacks (pre-v0.20.1 behavior).
    if (slot >= 0 && method == TAKEOVER_FIGHT && server.hasArg("fleets")) gameStateCommitFight(slot, server.arg("fleets").toInt());  // v0.28.0 mask
    else if (slot >= 0 && method == TAKEOVER_FIGHT && server.hasArg("fleet")) {
      const int f = server.arg("fleet").toInt();
      gameStateCommitFight(slot, f >= 0 ? (1 << f) : -1);
    }
    else if (slot >= 0) gameStateCommitTakeover(slot, method);
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/takeover_cancel", HTTP_POST, []() {
    int slot = slotForId(server.arg("id"));
    if (slot >= 0) gameStateCancelTakeover(slot);
    server.send(200, "application/json", "{\"ok\":true}");
  });

  // v0.30.0 the conversation: answer (a reply index, a favor, or 3 = refuse) and
  // close its result card.
  server.on("/negotiate_answer", HTTP_POST, []() {
    int slot = slotForId(server.arg("id"));
    if (slot >= 0) gameStateNegotiationAnswer(slot, server.arg("choice").toInt());
    server.send(200, "application/json", "{\"ok\":true}");
  });
  server.on("/negotiate_fight", HTTP_POST, []() {  // v0.30.1: a hostile faction -- fight instead
    int slot = slotForId(server.arg("id"));
    if (slot >= 0) gameStateNegotiationFight(slot);
    server.send(200, "application/json", "{\"ok\":true}");
  });
  server.on("/negotiate_close", HTTP_POST, []() {
    int slot = slotForId(server.arg("id"));
    if (slot >= 0) gameStateNegotiationClose(slot);
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/battle_action", HTTP_POST, []() {
    int slot = slotForId(server.arg("id"));
    int action = server.arg("action").toInt();
    if (slot >= 0) {
      if (action == 9) {
        // DONE: end whichever battle kind is active.
        GameState &s = webState(); gameStateReadInto(s);
        if (s.battleKind == BATTLE_PVP) gameStateEndPvpBattle();
        else gameStateEndTakeoverAfterBattle(slot);
      } else {
        gameStateBattleAction(slot, action);
      }
    }
    server.send(200, "application/json", "{\"ok\":true}");
  });

  // v0.28.0: the PvP defender brings another fleet (see gameStateReinforce).
  server.on("/reinforce", HTTP_POST, []() {
    int slot = slotForId(server.arg("id"));
    const char *refusal = (slot >= 0) ? gameStateReinforce(slot, server.arg("fleet").toInt()) : "Not joined.";
    if (!refusal) server.send(200, "application/json", "{\"ok\":true}");
    else server.send(200, "application/json", String("{\"ok\":false,\"reason\":") + jsonStr(refusal) + "}");
  });

  server.on("/set_events", HTTP_POST, []() {  // 2.0-dev.33: Lobby only
    gameStateSetEvents(false);  // 3.1-dev.96: the halfway event is off for good (not ready)
    server.send(200, "application/json", "{\"ok\":true}");
  });
  server.on("/set_markets", HTTP_POST, []() {  // 2.0-dev.32: Lobby only (the setter ignores it mid-game)
    gameStateSetMarkets(true);  // 3.1-dev.96: markets are part of the game
    server.send(200, "application/json", "{\"ok\":true}");
  });
  server.on("/call_mercs", HTTP_POST, []() {  // 2.0-dev.28: the PvP defender's hired ships
    int slot = slotForId(server.arg("id"));
    const char *refusal = (slot >= 0) ? gameStateCallMercs(slot) : "Not joined.";
    if (!refusal) server.send(200, "application/json", "{\"ok\":true}");
    else server.send(200, "application/json", String("{\"ok\":false,\"reason\":") + jsonStr(refusal) + "}");
  });

  server.on("/pvp_attack", HTTP_POST, []() {
    int slot = slotForId(server.arg("id"));
    // v0.28.0: atkFleets = a mask of every fleet sent in (atkFleet = one fleet, older pages).
    int atkFleet = server.hasArg("atkFleets") ? server.arg("atkFleets").toInt() : (1 << server.arg("atkFleet").toInt());
    int defSlot = server.arg("defSlot").toInt();
    int defFleet = server.arg("defFleet").toInt();
    // v0.20.2: report why an attack was refused (it used to fail silently).
    const char *refusal = (slot >= 0) ? gameStateStartPvpBattle(slot, atkFleet, defSlot, defFleet) : "Not joined.";
    if (!refusal) server.send(200, "application/json", "{\"ok\":true}");
    else server.send(200, "application/json", String("{\"ok\":false,\"reason\":") + jsonStr(refusal) + "}");
  });

  server.on("/transmission_ack", HTTP_POST, []() {
    int slot = slotForId(server.arg("id"));
    if (slot >= 0) { gameStateClearTransmission(slot); if (server.arg("n") == "2") gameStateClearTransmission(slot); }  // 3.1-dev.75: two reactions shown as one
    server.send(200, "application/json", "{\"ok\":true}");
  });


  {  // 3.1-dev.87: the plain page isn't in flash any more; the static_assert above PAGE_HTML's use catches a stale gzip at COMPILE time
    page_gz_ok = true;
    page_etag = String("\"") + FIRMWARE_VERSION + "-" + String((uint32_t)SF_PAGE_SRC_HASH, HEX) + "\"";
    art_etag = String("\"art-") + FIRMWARE_VERSION + "\"";
    Serial.printf("[NET] phone page: gzipped, %u bytes (%u plain in the source)\n", (unsigned)SF_PAGE_GZ_LEN, (unsigned)SF_PAGE_SRC_LEN);
    static const char *HDRS[] = { "If-None-Match", "Accept-Encoding" };
    server.collectHeaders(HDRS, 2);
  }
  server.addMiddleware([](WebServer &srv, Middleware::Callback next) {  // 3.1-dev.44 RAM low watch: name the request that took it
    char uri[30];
    snprintf(uri, sizeof(uri), "%s", srv.uri().c_str());  // (read before: it's empty once the handler has run)
    const bool r = next();
    if (healthLowDropped()) {
      char ctx[48];
      const GameState &g = webState();
      snprintf(ctx, sizeof(ctx), "%s r%d p%d", uri, g.round, g.phase);
      healthLowNote(ctx);
    }
    return r;
  });
  server.begin();
  Serial.printf("[NET] web server started at %s/\n", net_url);
}

// 2.0-dev.10 (overnight soak 2026-10-31): the table fell off the home Wi-Fi
// after ~70 min and never came back -- the game ran on, but no phone could
// reach it. Nothing watched the connection. Now: every drop is logged with the
// router's reason, a reconnect is tried after 5 s (again every 10 s), after a
// minute the join starts over from scratch, and on return shadowfleet.local
// is announced again (phones find the table by that name).
static volatile uint8_t net_drop_reason = 0;
static uint32_t net_lost_at = 0, net_retry_at = 0, net_drops = 0;
static void homeWifiWatch() {
  if (!net_home) return;
  static bool hooked = false;
  if (!hooked) {
    hooked = true;
    WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t info) { net_drop_reason = info.wifi_sta_disconnected.reason; },
                 ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  }
  const bool up = WiFi.status() == WL_CONNECTED;
  const uint32_t now = millis();
  if (!up) {
    if (!net_lost_at) {
      net_lost_at = now ? now : 1;
      net_retry_at = now + 5000;
      net_drops++; net_drops_n = net_drops;
      Serial.printf("[NET] LOST home Wi-Fi '%s' (reason %u, drop #%u) -- reconnecting\n", net_ssid, (unsigned)net_drop_reason, (unsigned)net_drops);
    }
    if ((int32_t)(now - net_retry_at) >= 0) {
      if (now - net_lost_at > 60000) {  // a minute gone: start the join over
        Serial.printf("[NET] still no home Wi-Fi after %u s -- joining again from scratch\n", (unsigned)((now - net_lost_at) / 1000));
        WiFi.disconnect(false);
        WiFi.begin(net_ssid, net_pass);  // 3.1-dev.62: the network it joined (was always the first saved one)
      } else {
        WiFi.reconnect();
      }
      net_retry_at = now + 10000;
    }
    return;
  }
  if (net_lost_at) {
    Serial.printf("[NET] back on home Wi-Fi after %u s (channel %d, %s)\n", (unsigned)((now - net_lost_at) / 1000),
                  WiFi.channel(), WiFi.localIP().toString().c_str());
    net_lost_at = 0;
    esp_wifi_set_ps(WIFI_PS_NONE);
    snprintf(net_url, sizeof(net_url), "http://%s", WiFi.localIP().toString().c_str());
    MDNS.end();
    if (MDNS.begin("shadowfleet")) MDNS.addService("http", "tcp", 80);
  }
}

// 3.1-dev.69 (after a flash the table once missed the home Wi-Fi, ran its own network, and nobody could reach it): while it
// sits on its own network in an EMPTY Lobby, it retries the home network once a minute (10 tries, never after a wrong
// password); the moment it gets in, it restarts onto it. A seated player or a game in progress stops the retries (a try
// moves the radio to the router's channel, which drops phones on the table's own network).
static void homeRetryTick() {
  static uint32_t nextAt = 20000, t0 = 0;  // 3.1-dev.111: first retry 20 s after boot (was 60)
  static uint8_t tries = 0;
  static bool trying = false;
  const uint32_t now = millis();
  if (trying) {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("[NET] home Wi-Fi '%s' answered (try %u) -- restarting onto it\n", net_ssid, (unsigned)tries);
      trying = false;
      net_restart_at = now + 500;
    } else if (now - t0 > 15000) {
      Serial.printf("[NET] home Wi-Fi '%s' still unreachable (try %u)\n", net_ssid, (unsigned)tries);
      WiFi.disconnect(false);
      trying = false;
    }
    return;
  }
  if (tries >= 30 || (int32_t)(now - nextAt) < 0) return;
  nextAt = now + 30000;
  // 3.1-dev.111: retry whenever NOBODY is on the table's own network (a try only drops phones that are on it) -- also mid-game;
  // after a reboot with a game going, every phone is still on the home Wi-Fi, waiting.
  if (WiFi.softAPgetStationNum() > 0) {
    GameState &g = webState();
    gameStateReadInto(g);
    if (g.stage != STAGE_LOBBY) return;
    for (int i = 0; i < MAX_PLAYERS; i++) if (g.players[i].active) return;
  }
  tries++;
  trying = true;
  t0 = now;
  Serial.printf("[NET] retrying home Wi-Fi '%s' (try %u of 10)\n", net_ssid, (unsigned)tries);
  WiFi.begin(net_ssid, net_pass);
}

void networkHandle() {
  server.handleClient();
  if (healthLowDropped()) {  // 3.1-dev.44 RAM low watch: which request (and when in the game) took it
    char ctx[48];
    const GameState &g = webState();  // the last snapshot a request read (close enough)
    snprintf(ctx, sizeof(ctx), "between requests r%d p%d", g.round, g.phase);
    healthLowNote(ctx);
  }
  homeWifiWatch();
  if (!net_home) collectScan();  // v0.22.6: pick up background scan results
  if (!net_home && net_home_failed && net_fail_retry && net_ssid[0] && !net_restart_at) homeRetryTick();  // 3.1-dev.69
  if (net_restart_at && (int32_t)(millis() - net_restart_at) > 0) ESP.restart();  // after /setup or a mode switch
}
