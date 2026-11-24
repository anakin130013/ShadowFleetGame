"""Reboot storm (3.1-dev.110): restart the table over and over and hit it with N fake phones reloading at the same instant.

    python tools/shakedown/reboot_storm.py --host shadowfleet.local --cycles 30 --phones 4

Each cycle: hard reset over USB (the bundled esptool), then as soon as the table answers, N threads each do what a phone does
after a reload -- GET / (gzip), /font.woff2, /art, /icon.png, a first /state, then poll /state?h= every 0.7 s for 20 s.
Per cycle it records: seconds until the table answered (None = never, i.e. wedged), whether the boot watchdog restarted it,
and the lowest free internal RAM from /health. Exit code 1 if any cycle wedged.
"""
import argparse, glob, os, re, subprocess, sys, threading, time, urllib.request

ESPTOOL = sorted(glob.glob(os.path.join(os.environ.get('LOCALAPPDATA', ''), 'Arduino15/packages/esp32/tools/esptool_py/*/esptool.exe')))

def get(url, t=6, hdr=None):
    try:
        r = urllib.request.urlopen(urllib.request.Request(url, headers=hdr or {}), timeout=t)
        return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, b''
    except Exception:
        return None, b''

def phone(host, stats, stop):
    base = 'http://' + host
    for path in ('/', '/font.woff2', '/art', '/icon.png'):
        st, _ = get(base + path, 10, {'Accept-Encoding': 'gzip'})
        stats.append((path, st))
    h = ''
    while not stop.is_set():
        st, body = get(base + '/state' + ('?h=' + h if h else ''), 6)
        stats.append(('/state', st))
        m = re.search(rb'"h":"?([0-9a-fA-F]+)', body or b'')
        if m: h = m.group(1).decode()
        time.sleep(0.7)

def main():
    a = argparse.ArgumentParser()
    a.add_argument('--host', default='shadowfleet.local'); a.add_argument('--port', default='COM36')
    a.add_argument('--cycles', type=int, default=20); a.add_argument('--phones', type=int, default=4)
    o = a.parse_args()
    if not ESPTOOL: sys.exit('esptool not found')
    results = []
    for c in range(1, o.cycles + 1):
        subprocess.run([ESPTOOL[-1], '--port', o.port, '--after', 'hard_reset', 'chip_id'], capture_output=True)
        t0 = time.time(); up = None
        while time.time() - t0 < 150:  # the boot watchdog fires at 60 s, so a wedge + recovery is ~80 s
            st, _ = get('http://%s/health' % o.host, 3)
            if st == 200: up = time.time() - t0; break
            time.sleep(1)
        if up is None:
            print('cycle %d: WEDGED -- no answer in 150 s' % c, flush=True); results.append((c, None, '', 0, {})); continue
        stats, stop = [], threading.Event()
        th = [threading.Thread(target=phone, args=(o.host, stats, stop)) for _ in range(o.phones)]
        for t in th: t.start()
        time.sleep(20); stop.set()
        for t in th: t.join(15)
        _, hb = get('http://%s/health' % o.host, 6)
        txt = hb.decode('utf-8', 'replace')
        low = re.search(r'lowest (\d+) KB', txt); last = re.search(r'last run ended: ([^\n(]+)|this boot: ([^\n(]+)', txt)
        codes = {}
        for p, st in stats: codes[st] = codes.get(st, 0) + 1
        lw = int(low.group(1)) if low else -1
        why = (last.group(1) or last.group(2)).strip() if last else '?'
        results.append((c, up, why, lw, codes))
        print('cycle %d: up in %.0f s (%s), lowest RAM %d KB, answers %s' % (c, up, why, lw, codes), flush=True)
    wedged = [r for r in results if r[1] is None]
    lows = [r[3] for r in results if r[1] is not None]
    print('\n== %d cycles: %d wedged, slowest start %.0f s, lowest RAM %s KB' % (len(results), len(wedged),
          max([r[1] for r in results if r[1]] or [0]), min(lows) if lows else '?'))
    sys.exit(1 if wedged else 0)

if __name__ == '__main__':
    main()
