# Third-party notices

Shadow Fleet's own code is under the PolyForm Noncommercial License 1.0.0 (`LICENSE.md`) and its 3D files and original art under CC BY-NC-SA 4.0 (`LICENSE-ASSETS.txt`). It also includes, or is built with, the following work by others. Each keeps its own license.

## Included in this repository

| Component | Files | License |
|---|---|---|
| Espressif SH8601 LCD panel driver | `ShadowFleet_Prototype_02/src/externLib/esp_lcd_sh8601.*` | Apache-2.0, Copyright 2023 Espressif Systems (Shanghai) Co. Ltd. |
| Espressif ES8311 audio codec driver | `ShadowFleet_Prototype_02/sf_es8311*` | Apache-2.0, Copyright 2015-2022 Espressif Systems (Shanghai) Co. Ltd. |
| QR Code generator library (Project Nayuki), symbols renamed `sfqr_` | `ShadowFleet_Prototype_02/sf_qrcodegen.*` | MIT, Copyright (c) Project Nayuki |
| Space Grotesk font (served to phones) | `ShadowFleet_Prototype_02/sf_web_font.h`, `tools/fonts/` | SIL Open Font License 1.1 (`tools/fonts/OFL.txt`) |
| Arduino WebServer.h (original + a 3-line patch) | `tools/core_patches/` | LGPL-2.1, Copyright (c) 2014 Ivan Grokhotkov |
| Montserrat font, as converted by LVGL | `ShadowFleet_Prototype_02/sf_font_montserrat_*.c` | SIL Open Font License 1.1 |

## Used when building (not included here)

| Component | License |
|---|---|
| LVGL 9.5 graphics library | MIT |
| Arduino core for ESP32 3.3 (Espressif) | LGPL-2.1 |
| ESP-IDF (inside the Arduino core) | Apache-2.0 |

The full texts of these licenses are available from each project:
- Apache-2.0: https://www.apache.org/licenses/LICENSE-2.0
- MIT (qrcodegen): https://www.nayuki.io/page/qr-code-generator-library
- SIL OFL 1.1: https://openfontlicense.org
- LVGL: https://github.com/lvgl/lvgl/blob/master/LICENCE.txt
- Arduino core for ESP32: https://github.com/espressif/arduino-esp32

## Not affiliated

Shadow Fleet is an unofficial, non-commercial fan project. It is not affiliated with, endorsed by or sponsored by Lucasfilm Ltd. or The Walt Disney Company. Star Wars and related names are trademarks of Lucasfilm Ltd. All art in this project is original.
