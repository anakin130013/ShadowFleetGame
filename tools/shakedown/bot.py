"""3.1 SHAKE-DOWN (stage 8): bots play whole games on the REAL table over Wi-Fi, through the same endpoints the phones use.

  python tools/shakedown/bot.py --players 4 --games 3            # bot games
  python tools/shakedown/bot.py --players 2 --games 1 --resets   # + restart the table mid-game (COM13 DTR) and check the save resumes

Checks after every phase: the table answers, nobody's Credits / VP go negative, the phase moves on (no stuck lock-in), the
table never reboots by itself (/health uptime), free RAM. Writes build/shakedown/game_<n>.json and prints a summary
(VP by class, quests, Eras, champions, strikes, wars, heists, news) for the numbers pass.
"""
import argparse, json, os, random, time, urllib.request, urllib.error, collections

H = 'http://' + os.environ.get('SF_HOST', 'shadowfleet.local')  # tools/table_check.py sets SF_HOST
OUT = os.path.join(os.path.dirname(__file__), '..', '..', 'build', 'shakedown')
CLASS = ['WARLORD', 'CONSUL', 'INFILTRATOR', 'SCOUNDREL']
# one character per faction row, classes spread: Marr (Rebel Warlord), Vell (Empire Consul), Nyra (Pyke Infiltrator), Dawn Scoundrel
CHARS = {2: [0, 7], 4: [0, 5, 10, 15]}
problems = []

def req(path, post=False, timeout=10):
    for attempt in range(3):
        try:
            r = urllib.request.urlopen(urllib.request.Request(H + path, data=b'' if post else None, method='POST' if post else 'GET'), timeout=timeout)
            body = r.read()
            return json.loads(body) if body and body[:1] in b'{[' else {}
        except urllib.error.HTTPError as e:
            return {'http': e.code}
        except Exception as e:
            if attempt == 2: problems.append('no answer: %s (%s)' % (path, e)); return None
            time.sleep(1)

def post(path): return req(path, True) or {}
def st(pid): return req('/state?id=' + pid)
def health():
    try:
        t = urllib.request.urlopen(H + '/health', timeout=6).read().decode().splitlines()
        up = int(t[1].split('up ')[1].split(' s')[0]); low = int(t[1].split('lowest ')[1].split(' KB')[0])
        return up, low
    except Exception: return None, None

def reset_table():
    import serial
    s = serial.Serial('COM13', 115200); s.dtr = False; s.rts = True; time.sleep(0.2); s.rts = False; s.close()
    for _ in range(40):
        time.sleep(1)
        if health()[0] is not None: return True
    return False

def bot_prepare(pid, slot, s, rng, log):
    v = s.get('v2', {}); me = s['players'][slot]; cls = me['commander'] % 4
    if v.get('job') and v['job'].get('st') == 1:
        w = v['job'].get('w'); log.setdefault('jobWorlds', []).append('r%d:%s%s' % (s['round'], w, '(held)' if w is not None and 0 <= w < len(s['planets']) and s['planets'][w]['owner'] >= 0 else ''))  # 3.1-dev.28/29
        post('/job?id=%s&take=%d' % (pid, 1 if rng.random() < 0.6 else 0))
    if s.get('rumor'): log.setdefault('rumors', {})[s['round']] = s['rumor']
    if v.get('grudge') and me['credits'] >= v.get('grudgePay', 99) and rng.random() < 0.3: post('/grudge_pay?id=' + pid); log['payoffs'] += 1
    g = v.get('grudge')  # 3.1-dev.72: make amends too (the deed on offer; a world from its list when it needs one)
    if g and g.get('dd', -1) >= 0 and rng.random() < 0.5:
        k = g['dd']; ws = (g.get('dw') or [[], [], []])[k] if k < len(g.get('dw') or []) else []
        r = post('/grudge_deed?id=%s&k=%d%s' % (pid, k, ('&w=%d' % rng.choice(ws)) if ws else ''))
        if r and r.get('ok'): log['deeds'][k] += 1
        elif r: log['deedRefused'][r.get('error', '?')] += 1
    if v.get('heistPush', -1) >= 0: post('/heist_push?id=%s&push=%d' % (pid, 1 if rng.random() < 0.5 else 0)); log['pushes'] += 1
    for f in range(4):
        if v.get('decrees', 0) & (1 << f): post('/decree?id=%s&f=%d' % (pid, f)); log['eras'].append((s['round'], CLASS[cls], f))
    if 'myLie' in v and not v['myLie']: post('/lie?id=%s&text=Someone%%20is%%20broke.' % pid)
    if v.get('cover') and not v['cover'].get('story'): post('/story?id=%s&text=Kessel%%20is%%20wide%%20open.' % pid)
    if rng.random() < 0.6:
        items = [(sd['f'], k, it) for sd in v.get('stalls', []) for k, it in enumerate(sd['it']) if it[0] >= 0 and it[3] == 1]
        if items:
            f, k, it = rng.choice(items)
            t = rng.randrange(len(s['planets'])) if it[0] == 2 else (me['chosenFaction'] if it[0] == 6 else -1)
            post('/stall_buy?id=%s&f=%d&k=%d&t=%d' % (pid, f, k, t)); log['buys'] += 1
    for k in range(rng.randint(0, 3)):
        if st(pid)['players'][slot]['credits'] >= 6: post('/invest_fleet?id=%s&fleet=%d' % (pid, rng.choice([0, 0, 1])))
    if me['credits'] >= 5:
        p = rng.randrange(len(s['planets'])); post('/reveal?id=%s&planet=%d&char=%d' % (pid, p, rng.choice([0, 3])))
    if s['round'] >= 3 and not v.get('moveUsed') and rng.random() < 0.5:
        if cls == 1: post('/summit?id=%s&w=%s' % (pid, ','.join(map(str, rng.sample(range(len(s['planets'])), 2))))); log['moves'].append('summit')
        if cls == 2: post('/cover?id=%s&f=%d' % (pid, rng.choice([f for f in range(4) if f != me['chosenFaction']]))); log['moves'].append('cover')
        if cls == 3: post('/heist?id=%s&f=%d&bold=%d' % (pid, rng.choice([f for f in range(4) if f != me['chosenFaction']]), rng.randint(0, 1))); log['moves'].append('heist')

def fight_out(pid, slot):
    for _ in range(60):
        s = st(pid)
        if not s or not s.get('battleActive'): return
        if s.get('battleOver'): post('/battle_action?id=%s&action=9' % pid); return
        post('/battle_action?id=%s&action=1' % pid)
    problems.append('battle never ended')

def war_target(s, slot):
    w = s.get('v2', {}).get('war')
    return w['w'] if w and w['by'] == slot else -2

def bot_resolution(pid, slot, s, rng, log):
    v = s.get('v2', {}); me = s['players'][slot]; cls = me['commander'] % 4
    war = v.get('war')
    if cls == 0 and s['round'] >= 3 and not v.get('moveUsed') and not war and rng.random() < 0.5:  # 3.1-dev.90 WARPATH: any phase
        targets = [p for p, pl in enumerate(s['planets']) if pl['owner'] != slot]
        r = post('/war?id=%s&planet=%d' % (pid, rng.choice(targets))); log['moves'].append('war'); log['wars'].append(r.get('ok'))
        s = st(pid); v = s.get('v2', {}); war = v.get('war')
    mine_war = war['w'] if war and war['by'] == slot and war['r'] == s['round'] else -1
    for attempt in range(2):
        s = st(pid); me = s['players'][slot]
        if s.get('battleActive'): break
        opts = [p for p, pl in enumerate(s['planets']) if pl['owner'] != slot and not pl.get('locked')]
        if not opts: break
        p = mine_war if mine_war in opts else rng.choice(opts); pl = s['planets'][p]
        if p == mine_war: mine_war = -1
        free = pl['owner'] < 0 and pl.get('home', -1) < 0
        fleets = [i for i, f in enumerate(me['fleets']) if f.get('alive', 0) > 0 and not f.get('spent')]
        post('/takeover_begin?id=%s&planet=%d' % (pid, p))
        roll = rng.random()
        if free and roll < 0.35 and p != war_target(s, slot) and me['credits'] >= pl.get('pay', 99) and not v.get('payShut'):
            post('/takeover_commit?id=%s&method=3' % pid); log['how']['pay'] += 1
        elif free and roll < 0.7 and p != war_target(s, slot):
            post('/takeover_commit?id=%s&method=2' % pid); log['how']['talk'] += 1
            for _ in range(12):
                n = st(pid).get('neg')
                if not n or n.get('result', 0) != 0: break
                if n.get('replies'): post('/negotiate_answer?id=%s&choice=%d' % (pid, rng.choice(n['replies'])['i']))
                else: post('/negotiate_answer?id=%s&choice=0' % pid)
            post('/negotiate_close?id=' + pid)
        elif fleets:
            post('/break?id=%s&planet=%d' % (pid, p)) if rng.random() < 0.3 else None
            post('/takeover_commit?id=%s&method=1&fleets=%d' % (pid, sum(1 << f for f in fleets[:2])))
            log['how']['fight'] += 1
            fight_out(pid, slot)
        post('/takeover_cancel?id=' + pid)

PINNED = (0, 1, 2, 6, 11, 12, 13); REVEAL = (3, 4, 5, 7, 9, 10, 15)
def bot_fh(pid, slot, s, rng, log):  # 3.1-dev.43 THE FINAL HOUR: draw, black market, debts, last orders, the reveal
    fh = s.get('fh')
    if not fh: return
    L = log.setdefault('fh', collections.Counter())
    if fh.get('deal') and post('/fh_pick?id=%s&k=%d' % (pid, rng.randrange(len(fh['deal'])))).get('ok'): L['picked'] += 1
    final = s['round'] >= s['roundLimit']
    if fh['st'] == 1 and (not final or s['phase'] == 0):
        for k, g in enumerate(fh['bm']):
            if g >= 0 and rng.random() < 0.4 and post('/fh_buy?id=%s&k=%d' % (pid, k)).get('ok'): L['bought'] += 1
        if fh.get('big', -1) < 0 and fh.get('bigOff') and rng.random() < 0.5:  # 3.1-dev.53 a big-ticket item
            k = rng.randrange(2); b = fh['bigOff'][k]
            mine = [w for w, pl in enumerate(s['planets']) if pl.get('owner') == slot]
            r = post('/fh_buy_big?id=%s&k=%d%s' % (pid, k, ('&w=%d' % mine[0]) if (b == 1 and mine) else ''))
            L[('big %d' % b) if r.get('ok') else 'big refused: ' + str(r.get('why'))] += 1
    if final and s['phase'] == 0:
        for f, (n, cr) in enumerate(fh['debt']):
            if n and not (fh['called'] >> f & 1) and post('/fh_debt?id=%s&f=%d&n=%d' % (pid, f, n if not cr else 1)).get('ok'): L['debts'] += 1
    s = st(pid); fh = s.get('fh') or fh
    hand = [g for g in range(16) if fh['hand'] >> g & 1]
    if final and s['phase'] == 1 and not s['players'][slot]['lockedIn']:
        targets = [i for i, pl in enumerate(s['planets']) if pl['owner'] != slot]
        tt = [-1] * 5; g = [-1] * 5; al = [0] * 5; gu = [-1] * 3
        for k in range(3):
            if fh['ships'][k] and rng.random() < 0.85: tt[k] = rng.choice(targets)
        if 14 in hand: tt[3] = rng.choice(targets)
        if fh.get('big') == 2: tt[4] = rng.choice(targets)  # 3.1-dev.53 the Mercenary Armada
        pins = [x for x in hand if x in PINNED]
        for k in range(5):
            if tt[k] >= 0 and pins:
                x = pins.pop()
                if x == 6 and s['planets'][tt[k]]['owner'] >= 0: continue
                g[k] = x
        if tt[0] >= 0: al[0] = sum(fh['ally'])
        q = lambda: '/fh_orders?id=%s&t=%s&gu=%s&al=%s&g=%s' % (pid, ','.join(map(str, tt)), ','.join(map(str, gu)), ','.join(map(str, al)), ','.join(map(str, g)))
        r = post(q())
        if not r.get('ok'):
            L['refused: ' + r.get('why', '?')] += 1
            for k in range(5):
                if tt[k] >= 0 and not (fh['reach'] >> tt[k] & 1): tt[k] = -1; g[k] = -1
            r = post(q())
        if r.get('ok'): L['orders'] += 1; L['strikes'] += sum(1 for x in tt if x >= 0); L['pinned'] += sum(1 for x in g if x >= 0)
        else: L['refused again: ' + r.get('why', '?')] += 1
    if final and s['phase'] == 2 and fh['st'] == 1 and not s['players'][slot]['lockedIn']:
        strikes = fh.get('strikes', []); mine = [w for w, pl in enumerate(s['planets']) if pl['owner'] == slot]
        others = [x for x in strikes if x[1] != slot and not x[4]]
        gg = [-1] * 3; aa = [-1] * 3; bb = [-1] * 3; n = 0
        for x in hand:
            if x not in REVEAL or n >= 3: continue
            a = b = -1
            if x in (9, 10, 5):
                if not others: continue
                a = rng.choice(others)[0]
                if x == 10: b = rng.choice([w for w in range(len(s['planets'])) if s['planets'][w]['owner'] != rng.choice(others)[1]])
            if x == 4:
                if not mine: continue
                a = rng.choice(mine)
            if x == 7: a = max(range(4), key=lambda f: fh['ally'][f])
            gg[n], aa[n], bb[n] = x, a, b; n += 1
        if n:
            r = post('/fh_reveal?id=%s&g=%s&a=%s&b=%s' % (pid, ','.join(map(str, gg)), ','.join(map(str, aa)), ','.join(map(str, bb))))
            L['reveal plays' if r.get('ok') else 'reveal refused: ' + r.get('why', '?')] += n

def bot_sell(pid, sk, rng, log):  # 3.1-dev.48 SELL IT TWICE: a greedy bot sells most of the time, pays one side back when caught
    v2 = (sk or {}).get('v2') or {}
    L = log.setdefault('sell', collections.Counter())
    if v2.get('heat'): L['max heat %d' % v2['heat']] += 0
    if v2.get('sell'):
        sold = rng.random() < float(os.environ.get('SELL_P', '0.7'))  # SELL_P=1 makes every bot greedy
        if post('/sell_twice?id=%s&sell=%d' % (pid, 1 if sold else 0)).get('ok'): L['sold' if sold else 'clean'] += 1
        v2 = (st(pid) or {}).get('v2') or {}  # caught right now?
    c = v2.get('caught')
    if c and ('caught r%d' % c[2]) not in L:
        L['caught r%d' % c[2]] += 1
        if post('/pay_back?id=%s&f=%d' % (pid, c[0])).get('ok'): L['paid back'] += 1

def check(s, label):
    for i, p in enumerate(s['players']):
        if not p.get('active'): continue
        if p['credits'] < 0: problems.append('%s: P%d credits %d' % (label, i + 1, p['credits']))
        if p['vp'] < -1: problems.append('%s: P%d VP %d' % (label, i + 1, p['vp']))

def play(n, game_no, resets, seed):
    rng = random.Random(seed)
    ids = ['bot%d' % k for k in range(n)]
    post('/new_game?id=bot0'); post('/end_game?id=bot0')
    for k, pid in enumerate(ids): post('/join?id=%s&slot=%d' % (pid, k))
    for k, pid in enumerate(ids): post('/choose_character?id=%s&character=%d' % (pid, CHARS[n][k]))
    post('/start_game?id=bot0')
    log = {'deeds': collections.Counter(), 'deedRefused': collections.Counter(), 'buys': 0, 'payoffs': 0, 'pushes': 0, 'eras': [], 'moves': [], 'wars': [], 'how': collections.Counter(), 'resets': []}
    up0, low = health(); minram = low or 999; t0 = time.time()
    last = None
    for step in range(300):
        s = st('bot0')
        if not s: break
        if s['stage'] == 2: break
        key = (s['round'], s['phase'])
        if (s.get('fh') or {}).get('st') == 2:  # 3.1-dev.43 the finale is playing on the table: wait for the game to end
            time.sleep(5); last = None; continue
        if key == last: problems.append('stuck at round %d phase %d' % key)
        last = key
        for k, pid in enumerate(ids):
            sk = st(pid)
            if sk.get('rumor'): log.setdefault('rumors', {})[sk['round']] = sk['rumor']  # 3.1-dev.28
            bot_fh(pid, k, sk, rng, log)  # 3.1-dev.43
            bot_sell(pid, sk, rng, log)
            if sk['phase'] == 0: bot_prepare(pid, k, sk, rng, log)
            elif sk['phase'] == 2: bot_resolution(pid, k, sk, rng, log)
            bot_sell(pid, st(pid), rng, log)  # 3.1-dev.48: offers arrive right after a Resolution action
            post('/transmission_ack?id=' + pid)
        check(st('bot0'), 'r%d p%d' % key)
        if resets and rng.random() < 0.25 and not st('bot0').get('battleActive'):
            before = st('bot0'); time.sleep(7)  # let the save land
            before = st('bot0')
            ok = reset_table(); after = st('bot0')
            same = after and after['round'] == before['round'] and after['phase'] == before['phase'] and \
                   [p['vp'] for p in after['players']] == [p['vp'] for p in before['players']] and \
                   [pl['owner'] for pl in after['planets']] == [pl['owner'] for pl in before['planets']]
            log['resets'].append({'round': before['round'], 'phase': before['phase'], 'resumed': bool(ok and same)})
            if not (ok and same): problems.append('save did not resume at round %d phase %d' % (before['round'], before['phase']))
            up0 = None
        for pid in ids: post('/lockin?id=' + pid)
        up, low = health()
        if up is not None and up0 is not None and up < up0: problems.append('the table REBOOTED by itself (uptime %d -> %d)' % (up0, up))
        if up is not None: up0 = up
        if low is not None: minram = min(minram, low)
    s = st('bot0')
    allpub = s['ev']['p'] if s else []
    res = {'players': n, 'seconds': round(time.time() - t0), 'ended': s and s['stage'] == 2, 'rounds': s and s['round'], 'minRamKB': minram,
           'final': [{'slot': i, 'class': CLASS[p['commander'] % 4], 'faction': p['chosenFaction'], 'vp': p['vp'], 'worlds': sum(1 for pl in s['planets'] if pl['owner'] == i),
                      'credits': p['credits'], 'quest': s['v2']['questDone'][i]} for i, p in enumerate(s['players']) if p.get('active')] if s else [],
           'news': collections.Counter(e[6] for e in allpub if e[1] == 11),
           'champions': sorted(e[2] for e in allpub if e[1] == 11 and e[6] == 14 and e[5] is not None and e[5] >= 0),  # 3.1-dev.50 the round each title was won
           'log': {k: (dict(v) if isinstance(v, collections.Counter) else v) for k, v in log.items()}}
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, 'game_%d_%dp.json' % (game_no, n)), 'w') as f: json.dump(res, f, indent=1, default=str)
    post('/new_game?id=bot0'); post('/end_game?id=bot0')
    return res

if __name__ == '__main__':
    ap = argparse.ArgumentParser(); ap.add_argument('--players', type=int, default=2); ap.add_argument('--games', type=int, default=1)
    ap.add_argument('--resets', action='store_true'); ap.add_argument('--seed', type=int, default=1)
    a = ap.parse_args()
    for g in range(a.games):
        r = play(a.players, g + 1, a.resets, a.seed + g)
        print('game %d (%dp): ended=%s rounds=%s %ds minRAM=%sKB  ' % (g + 1, a.players, r['ended'], r['rounds'], r['seconds'], r['minRamKB']) +
              ' | '.join('%s %dVP %dw q%d' % (p['class'][:4], p['vp'], p['worlds'], p['quest']) for p in r['final']))
        print('   how=%s eras=%s moves=%s wars=%s resets=%s news=%s' % (r['log']['how'], r['log']['eras'], r['log']['moves'], r['log']['wars'], r['log']['resets'], dict(r['news'])))
        print('   job worlds=%s' % r['log'].get('jobWorlds'))
        print('   deeds done (0 ship / 1 round / 2 world)=%s refused=%s payoffs=%s' % (dict(r['log']['deeds']), dict(r['log']['deedRefused']), r['log']['payoffs']))
        print('   final hour=%s' % r['log'].get('fh'))
        print('   sell it twice=%s' % r['log'].get('sell'))
        print('   champion titles won in rounds %s' % r.get('champions'))
        for rd, txt in sorted(r['log'].get('rumors', {}).items()): print('   rumor r%s: %s' % (rd, txt))
    print('PROBLEMS:' if problems else 'NO PROBLEMS', *problems[:30], sep='\n  ')
