# Builds build/phone_check/morph_test.html from the per-mode dumps (see phone_morph_test.sh).
import html, json, os, re, sys
out = sys.argv[1]
snips = {}
for f in sorted(os.listdir(os.path.join(out, 'dumps'))):
    t = open(os.path.join(out, 'dumps', f), encoding='utf-8', errors='replace').read()
    m = re.search(r'<pre id="DUMP">(.*?)</pre>', t, re.S)
    if m and m.group(1).strip():
        snips[f[:-5]] = html.unescape(m.group(1))
morph = open(os.path.join(out, 'morph.js'), encoding='utf-8').read()
page = '''<!doctype html><html><body><div id="app"></div><div id="fresh"></div><pre id="R">running</pre><script>
let shownHtml = null;
%s
const S = %s;
const names = Object.keys(S);
const app = document.getElementById('app'), fresh = document.getElementById('fresh');
let seed = 7; const rnd = n => { seed = (seed * 1103515245 + 12345) %% 2147483648; return seed %% n; };
let steps = 0, fails = [];
function check(name) {
  fresh.innerHTML = S[name];
  // data-keep subtrees draw themselves (the scoreboard counts up); compare their
  // attributes (data-n = the target value) but not what's inside them.
  const strip = (root) => { const c = root.cloneNode(true); c.querySelectorAll('[data-keep]').forEach(k => { k.innerHTML = ''; }); return c; };
  const A = strip(app).childNodes, F = strip(fresh).childNodes;
  let same = A.length === F.length;
  for (let i = 0; same && i < A.length; i++) same = A[i].isEqualNode(F[i]);
  if (!same) fails.push(name);
}
for (let round = 0; round < 6; round++) {
  for (let k = 0; k < names.length; k++) {
    const n = names[rnd(names.length)];
    setApp(app, S[n]); steps++; check(n);
  }
}
// identity: patching a screen onto itself must change nothing (same nodes kept)
const n0 = names[0]; setApp(app, S[n0]); const first = app.firstElementChild; shownHtml = null; setApp(app, S[n0]);
const kept = app.firstElementChild === first;
document.getElementById('R').textContent = (fails.length ? 'FAIL ' + fails.length + ' of ' + steps + ': ' + [...new Set(fails)].slice(0, 12).join(', ') : 'MORPH OK: ' + steps + ' patches across ' + names.length + ' screens') + (kept ? ', nodes kept' : ', NODES NOT KEPT');
</script></body></html>''' % (morph, json.dumps(snips))
open(os.path.join(out, 'morph_test.html'), 'w', encoding='utf-8').write(page)
print('screens captured:', len(snips))
