# Patches to the ESP32 Arduino core (3.3.10)

`libraries/WebServer/src/WebServer.h` (in `%LOCALAPPDATA%\Arduino15\packages\esp32\hardware\esp32\3.3.10\`):
`HTTP_MAX_DATA_WAIT` wrapped in `#ifndef ... #endif` so the table can build with
`-DHTTP_MAX_DATA_WAIT=500` (3.1-dev.33, playtest 2026-11-08). The WebServer serves one
connection at a time and waited 5 s on a connection that hadn't sent its request yet --
Android Chrome opens idle "preconnect" sockets, so every phone stalled behind them.
Every other project still gets the default 5000.

- `WebServer.h.orig` -- the untouched original. ROLL BACK: copy it over the core's WebServer.h
  (a second copy sits next to it as `WebServer.h.orig_sf`).
- `WebServer.h.patched` -- the patched file. RE-APPLY after any esp32 core update/reinstall
  (or redo the 3-line `#ifndef` by hand) -- without it the -D flag is ignored with a redefine warning.
