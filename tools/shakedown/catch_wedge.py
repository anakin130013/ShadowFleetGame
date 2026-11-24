"""Catch a wedged boot in the act (3.1-dev.110): reset the table, record its serial log for 100 s while N fake phones reload
as soon as it answers; stop at the first boot whose web server never answers and print that boot's log.

    python tools/shakedown/catch_wedge.py --host shadowfleet.local --tries 20
"""
import argparse, glob, os, subprocess, sys, threading, time, urllib.request
import serial

ESPTOOL = sorted(glob.glob(os.path.join(os.environ.get('LOCALAPPDATA', ''), 'Arduino15/packages/esp32/tools/esptool_py/*/esptool.exe')))

def get(url, t=4, hdr=None):
    try:
        r = urllib.request.urlopen(urllib.request.Request(url, headers=hdr or {}), timeout=t); return r.status
    except urllib.error.HTTPError as e: return e.code
    except Exception: return None

def phone(host, stop):
    for p in ('/', '/font.woff2', '/art', '/icon.png', '/state'):
        get('http://' + host + p, 10, {'Accept-Encoding': 'gzip'})
    while not stop.is_set(): get('http://' + host + '/state', 5); time.sleep(0.7)

def main():
    a = argparse.ArgumentParser(); a.add_argument('--host', default='shadowfleet.local'); a.add_argument('--port', default='COM36')
    a.add_argument('--tries', type=int, default=20); a.add_argument('--phones', type=int, default=4); o = a.parse_args()
    for n in range(1, o.tries + 1):
        subprocess.run([ESPTOOL[-1], '--port', o.port, '--after', 'hard_reset', 'chip_id'], capture_output=True)
        s = serial.Serial(); s.port = o.port; s.baudrate = 115200; s.timeout = 0.2; s.dtr = False; s.rts = False
        for _ in range(40):
            try: s.open(); break
            except Exception: time.sleep(0.25)
        buf = bytearray(); t0 = time.time(); up = None; stop = threading.Event(); th = []
        while time.time() - t0 < 100:
            buf += s.read(8192)
            if up is None and int(time.time() - t0) % 2 == 0 and get('http://%s/health' % o.host, 1) == 200:
                up = time.time() - t0
                th = [threading.Thread(target=phone, args=(o.host, stop)) for _ in range(o.phones)]
                for t in th: t.start()
            if up is not None and time.time() - t0 > up + 25: break
        stop.set(); s.close()
        txt = buf.decode('utf-8', 'replace')
        if up is not None:
            print('try %d: up in %.0f s' % (n, up), flush=True); continue
        print('try %d: WEDGED -- the boot log:' % n, flush=True)
        lines = [l for l in txt.splitlines() if not l.startswith(('[UI]', '[TR]', '[TOUCH]', '[BSP] flush', '[BSP] lvgl_port_task loop'))]
        print('\n'.join(lines[-120:]))
        sys.exit(1)
    print('no wedge in %d tries' % o.tries)

if __name__ == '__main__':
    main()
