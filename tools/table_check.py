"""Set up / check a Table Console (the 1.43") in one go.

  python tools/table_check.py                 flash the normal build (build143), then check it
  python tools/table_check.py --co5300        flash the CO5300 build instead (a 1.43" that shipped with the 1.75"'s chip)
  python tools/table_check.py --no-flash      only check a table that is already running
  python tools/table_check.py --host 192.168.4.1 --bot   ... and play a short 2-bot game on it

Steps: (1) flash over COM13 from an existing build folder (compile first -- see the design notes); (2) listen to the serial port
for ~25 s WITHOUT resetting it (dtr / rts held low) and print the display, touch and Wi-Fi lines; (3) read /health over
Wi-Fi (default http://shadowfleet.local, then 192.168.4.1); (4) with --bot, a 2-player bot game (tools/shakedown/bot.py).
A brand-new board has no saved Wi-Fi: it runs its own "ShadowFleet" network (password starwars1). Join it from a phone,
connect the table to home Wi-Fi from its Lobby, then run this again with --no-flash.
"""
import argparse, os, subprocess, sys, time, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CLI = os.path.join(os.environ.get('LOCALAPPDATA', ''), r'Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe')
CFG = os.path.join(os.environ.get('USERPROFILE', ''), r'.arduinoIDE\arduino-cli.yaml')
FQBN = 'esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=16M,PartitionScheme=huge_app,PSRAM=opi'
KEEP = ('[BSP] esp_lcd', '[BSP] display', '[BSP] brightness', '[TOUCH] poll #0', '[NET]', '[LINK]', '[SAVE]', 'Shadow Fleet', 'Guru', 'panic', 'FROZE', 'rst:')


def flash(build, port):
    print('== flashing', build, 'to', port)
    r = subprocess.run([CLI, 'upload', '--config-file', CFG, '--fqbn', FQBN, '-p', port, '--input-dir', os.path.join(ROOT, build), os.path.join(ROOT, 'ShadowFleet_Prototype_02')],
                       capture_output=True, text=True)
    print('\n'.join((r.stdout + r.stderr).strip().splitlines()[-2:]))
    if r.returncode:
        sys.exit('flash FAILED -- is the table plugged in on ' + port + '?')


def listen(port, secs):
    import serial
    print('== listening on', port, 'for', secs, 's')
    s = serial.Serial(); s.port = port; s.baudrate = 115200; s.timeout = 0.2; s.dtr = False; s.rts = False
    t0 = time.time(); buf = b''
    while time.time() - t0 < secs:
        try:
            if not s.is_open: s.open()
            buf += s.read(4096)
        except Exception:
            time.sleep(0.4)
    try: s.close()
    except Exception: pass
    lines = [l for l in buf.decode('utf-8', 'replace').splitlines() if any(k in l for k in KEEP)]
    print('\n'.join(lines[-30:]) if lines else '(nothing interesting heard -- the table may have booted before we listened; try --no-flash)')


def health(hosts):
    for h in hosts:
        try:
            body = urllib.request.urlopen('http://' + h + '/health', timeout=5).read().decode('utf-8', 'replace')
            print('== /health from', h); print('\n'.join(body.splitlines()[:3])); print(body.splitlines()[-1])
            return h
        except Exception as e:
            print('   no answer from', h, '(' + type(e).__name__ + ')')
    print('== the table is not on this Wi-Fi yet. On a phone: join "ShadowFleet" (password starwars1), open http://192.168.4.1,'
          ' tap CONNECT THE TABLE TO HOME WI-FI, then run this again with --no-flash.')
    return None


if __name__ == '__main__':
    a = argparse.ArgumentParser()
    a.add_argument('--co5300', action='store_true'); a.add_argument('--no-flash', action='store_true'); a.add_argument('--bot', action='store_true')
    a.add_argument('--port', default=None); a.add_argument('--host', default=None)
    o = a.parse_args()
    if not o.port:  # 2026-11-15: each board gets its own COM port (the old 1.43" was COM13, the replacement is COM36) -- find it
        r = subprocess.run([CLI, 'board', 'list'], capture_output=True, text=True)
        ports = [l.split()[0] for l in r.stdout.splitlines() if 'ESP32' in l and l.startswith('COM')]
        if not ports: sys.exit('no ESP32 board found on USB -- plug the table in')
        o.port = ports[0]
        print('== found the board on', o.port)
    if not o.no_flash:
        flash('build143co' if o.co5300 else 'build143', o.port)
    listen(o.port, 25 if not o.no_flash else 8)
    host = health([o.host] if o.host else ['shadowfleet.local', '192.168.4.1'])
    if host and o.bot:
        print('== a 2-player bot game on', host)
        env = dict(os.environ, SF_HOST=host)
        subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'shakedown', 'bot.py'), '--players', '2', '--games', '1', '--seed', '3'], env=env)
    print('== done. Screen dark? Run again with --co5300 (and tell Claude).')
