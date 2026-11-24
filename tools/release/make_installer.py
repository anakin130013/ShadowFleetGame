"""Builds the web installer (ESP Web Tools) for the table console from the last build143 compile.

    python tools/release/make_installer.py          -> launch/installer/ (index.html + manifest.json + firmware/)

Upload the whole launch/installer folder to any HTTPS static host (GitHub Pages works). The page installs the
firmware from Chrome or Edge over USB -- no Arduino IDE. Parts and offsets are the Arduino ESP32-S3 ones:
bootloader 0x0, partition table 0x8000, boot_app0 0xE000, the app 0x10000.
"""
import json, os, re, shutil

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
B = os.path.join(ROOT, 'build143')
OUT = os.path.join(ROOT, 'launch', 'installer')
ver = re.search(r'#define FIRMWARE_VERSION "([^"]+)"', open(os.path.join(ROOT, 'ShadowFleet_Prototype_02', 'version.h'), encoding='utf-8').read()).group(1)

parts = [('ShadowFleet_Prototype_02.ino.bootloader.bin', 'bootloader.bin', 0x0),
         ('ShadowFleet_Prototype_02.ino.partitions.bin', 'partitions.bin', 0x8000),
         ('boot_app0.bin', 'boot_app0.bin', 0xE000),
         ('ShadowFleet_Prototype_02.ino.bin', 'shadowfleet_table.bin', 0x10000)]
os.makedirs(os.path.join(OUT, 'firmware'), exist_ok=True)
for src, dst, _ in parts:
    shutil.copyfile(os.path.join(B, src), os.path.join(OUT, 'firmware', dst))
manifest = {
    'name': 'Shadow Fleet table console',
    'version': ver,
    'new_install_prompt_erase': True,
    'builds': [{'chipFamily': 'ESP32-S3', 'parts': [{'path': 'firmware/' + dst, 'offset': off} for _, dst, off in parts]}],
}
json.dump(manifest, open(os.path.join(OUT, 'manifest.json'), 'w'), indent=2)
page = open(os.path.join(ROOT, 'tools', 'release', 'installer_template.html'), encoding='utf-8').read().replace('{{VERSION}}', ver)
open(os.path.join(OUT, 'index.html'), 'w', encoding='utf-8').write(page)
print('installer for', ver, '->', OUT)
