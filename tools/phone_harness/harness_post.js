// 3.1: screenshots show where things land, not mid-slide (headless Edge doesn't run CSS transitions to the end).
{ const st = document.createElement('style'); st.textContent = '*,*::before,*::after{transition-duration:0s!important}'; document.head.appendChild(st); }
// Contact sheet: each scenario drawn at fixed points in the exchange timeline.
// The fake state is mid-game, so the round banner (v0.21.1) would cover every
// screenshot -- dismiss it unless the mode is about the banner itself.
if (HARNESS_MODE !== 'banner') { const hs = harnessState(); bannerKey = hs.round * 4 + hs.phase; hideBanner(); }
setTimeout(() => {
  if (HARNESS_MODES[HARNESS_MODE]) { HARNESS_MODES[HARNESS_MODE].action(); return; }
  mySlot = 1; joined = true;
  const sheet = document.createElement('div');
  sheet.style.cssText = 'position:absolute;left:400px;top:0;width:1640px;background:#000;color:#D9F5FF;font:13px sans-serif;padding:8px';
  document.body.appendChild(sheet);
  const scenarios = [
    ['Planet (HOTH): attacker +1 support wins, garrison ship hit', {}],
    ['Planet (TATOOINE): garrison +1 resentment wins, my ship 0 hit', {battleTargetPlanet:0, battleAtkMod:0, battleDefMod:1, lastAtkRoll:3, lastDefRoll:5, lastLossIdx:0, battleAtkAlive:[0,1,1,0,0,0], defRemaining:2}],
    ['PvP vs EMPIRE: I win, their ship 1 hit', {battleKind:2, battleAtkMod:0, battleDefenderSlot:0, battleTargetPlanet:-1, lastAtkRoll:5, lastDefRoll:2, lastLossIdx:1, battleDefAlive:[1,0,1,1,0,0], battleAtkAlive:[1,1,1,1,0,0]}],
    ['PvP tie (both miss), I am DEFENDER', {battleKind:2, battleAtkMod:0, battleAttackerSlot:3, battleDefenderSlot:1, battleTargetPlanet:-1, lastAtkRoll:4, lastDefRoll:4, lastLossIdx:-1, battleDefAlive:[1,1,1,0,0,0], battleAtkAlive:[1,1,0,0,0,0]}],
    ['Withdraw under fire (HOTH): garrison 5 beats my 2 -- ship 0 lost', {lastRetreatSide:1, battleAtkMod:0, lastAtkRoll:2, lastDefRoll:5, lastLossIdx:0, battleAtkAlive:[0,1,1,0,0,0], battleOver:true}],
    ['Withdraw under fire (HOTH): my 5 beats garrison 3 -- clean escape, no garrison hit', {lastRetreatSide:1, battleAtkMod:0, lastAtkRoll:5, lastDefRoll:3, lastLossIdx:-1, battleOver:true}],
    ['Final: DAGOBAH captured', {battleTargetPlanet:3, defRemaining:0, battleOver:true}],
    ['Final: PvP VICTORY', {battleKind:2, battleAtkMod:0, battleDefenderSlot:0, battleTargetPlanet:-1, lastAtkRoll:5, lastDefRoll:1, lastLossIdx:0, battleDefAlive:[0,0,0,0,0,0], battleOver:true}],
  ];
  const times = [300, 700, 950, 1060, 1250, 1500];
  for (const [label, o] of scenarios) {
    const s = fakeState(o);
    if (label.includes('DAGOBAH')) s.planets[3].owner = 1;
    const h = document.createElement('div'); h.textContent = label; h.style.margin = '6px 0 2px'; sheet.appendChild(h);
    const row = document.createElement('div'); row.style.display = 'flex'; row.style.gap = '6px'; sheet.appendChild(row);
    const ts = label.startsWith('Final') ? [1500] : times;
    for (const t of ts) {
      const cell = document.createElement('div');
      const c = document.createElement('canvas'); c.style.width = '266px'; c.style.display = 'block';
      cell.appendChild(c); const cap = document.createElement('div'); cap.textContent = 't=' + t; cell.appendChild(cap);
      row.appendChild(cell);
      bView = bViewFrom(s);
      bAnimActive = t < 1500; bAnimStart = 100000 - t;
      bDraw(c, 100000);
    }
  }
  bAnimActive = false;
  document.title = 'SHEET READY';
}, 1500);
