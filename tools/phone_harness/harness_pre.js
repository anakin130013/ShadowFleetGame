// Headless Edge can't make its window narrower than ~510 px, so full-screen (position:fixed) phone screens
// would spread to 510 and get cropped: pin them to the 380 px phone the rest of the page uses (harness only).
document.head.insertAdjacentHTML('beforeend', '<style>.bfull{width:380px!important;right:auto!important} #lyrSheet .sheetBox{width:400px!important;right:auto!important} #lyrAnn{width:400px!important;right:auto!important} #fhPlay{width:400px!important;right:auto!important} #txOverlay{width:400px!important;right:auto!important} #txIncoming{width:400px!important;right:auto!important} .txChip{opacity:1!important;transform:none!important} body.deckOn #deck,body.deckOn #canopy{width:400px!important;right:auto!important;margin:0!important} #app.holo{animation:none!important} #holoFx{display:none!important}</style>');
// Fake game state + fetch stub so the real phone page runs offline.
const PN = ['TATOOINE','HOTH','BESPIN','DAGOBAH','CORUSCANT','KESSEL','OBA DIAH','DATHOMIR','CORELLIA','NABOO','ENDOR','KIJIMI',
            'YAVIN 4','MUSTAFAR','NAL HUTTA','CANTO BIGHT','KUAT'];
const PRING = [0,0,1,0,2,0,0,0,2,1,1,1,0,0,1,1,2];  // 2.0 17 worlds
const PHOME = {12:0, 13:1, 6:2, 7:3};               // home worlds by faction
// 3.1-dev.43 a Final Hour state: round r, phase ph, the fh object (o overrides)
const FH_TEST_LOG = 'e001010f0c03e10b02000303000005000001070010e2030504e2040206e3e40104e40104e40205e40303e40106e50206e104010104010003060200e20304e50106';
function fhState(r, ph, o) {
  const s = fakeState({battleActive:false, battleKind:0, phase:ph, phaseName:['PREPARE','MOVEMENT','RESOLUTION'][ph]});
  s.round = r; s.roundLimit = 8;
  s.planets[9].owner = s.yourSlot; s.planets[9].defense = 4; s.planets[2].owner = 0; s.planets[6].owner = 3;
  s.fh = Object.assign({st:1, hand:0, bm:[9,0], bmCr:15, jump:3, called:0, big:-1, bigArg:-1, bigOff:[0,1], ally:[0,0,0,0], debt:[[1,0],[0,0],[2,6],[1,10]], ships:[3,0,0],
                        reach:(1<<9)|(1<<2)|(1<<10)|(1<<12)|(1<<14), ord:{t:[-1,-1,-1,-1], gu:[-1,-1,-1], al:[0,0,0,0], g:[-1,-1,-1,-1]}, rv:[[-1,-1,-1],[-1,-1,-1],[-1,-1,-1]]}, o);
  if (o && o.lots) s.bm = {o:1, lots:o.lots.concat([7, 8, 11]).slice(0, 4), got:o.lotsGot || 0, hold:o.hold || [-1, -1, -1]};  // 3.1-dev.88
  return s;
}
function gyShow() { sheetHide(); }
function gyCrowd() {
  const s = fhState(8, 0, {hand:(1<<12)|(1<<15)|(1<<5), big:-1});
  s.reputation = [['NEUTRAL', 9], ['HOSTILE', -36], ['HOSTILE', -33], ['ALLIED', 31]].map(([t, sc]) => ({tier:t, score:sc}));
  s.promises = [{f:2, p:5, due:8, chk:6, text:'Spice Contract: 1 Credit to us at every round start for 3 rounds.'}, {f:3, p:9, due:9, chk:6, text:'Hold Bespin for 3 rounds.'}];
  s.v2.spy = {w:0, t:[1, 3, 2, 2]}; s.v2.job = {f:0, w:5, due:8, st:2, ann:0}; s.v2.race = {w:2, f:3, u:8};
  s.v2.grudge = {f:1, r:9, w:-1}; s.v2.grudgePay = 12; s.v2.decrees = 1 << 3;
  return s;
}
function fakeState(o) {
  const fleet = (a) => [{ships:a.map(Boolean), alive:a.filter(Boolean).length, total:3, spent:false}, {ships:[0,0,0,0,0,0].map(Boolean),alive:0,total:0,spent:false}, {ships:[0,0,0,0,0,0].map(Boolean),alive:0,total:0,spent:false}];
  const pl = (f, name) => ({active:true, name:name, chosenFaction:f, commander:f * 4 + (f === 0 ? 3 : 1), credits:9, income:5, vp:4, lockedIn:false, fleets:fleet([1,1,1,0,0,0])});
  const s = {
    firmwareVersion:'v0.20.0-harness', stage:1, phase:2, phaseName:'RESOLUTION', round:3, roundLimit:8, yourSlot:1,
    planetIncluded: PN.map(()=>true),
    players:[pl(1,'GALACTIC EMPIRE'), pl(0,'REBEL ALLIANCE'), {active:false,name:'',chosenFaction:-1,commander:-1,credits:0,vp:0,lockedIn:false,fleets:fleet([0,0,0,0,0,0])}, pl(3,'CRIMSON DAWN')],
    planets: PN.map((n, i) => ({name:n, owner:-1, locked:false, pay:9, ring:PRING[i], home:i in PHOME ? PHOME[i] : -1, vp:[2,3,5][PRING[i]], chars:[0,1,2,3].map(c=>({revealed:c === 1 || c === 2, value:c === 1 ? (i % 3 ? 'High' : 'Low') : c === 2 ? ['Ancient Sand Temple','Frozen Ruins','Sky Shrine','Old Jedi Temple'][i % 4] : '',cost:c === 2 ? 1 : 2})),  // dev.126: Resources + Trait are public in a real game
                               good:['FUEL','REPAIRS','SURVEY','BLESSING'][i % 4], goodFx:'One fleet may move 2 steps this round', mOpen:i % 3 === 0,
                               mStock:i === 9 ? 0 : 2, mPrice:2 + (i % 3), mBuy:i % 3 === 0 && i !== 9, unbound:i === 7, dipl:true})),
    v2:{markets:[0,3,6,9], event:0, eventName:'THE UNBOUND RISE', eventText:'Droids broke their bolts on unclaimed worlds: +2 defenders, no Pay. First to take one +1 VP.',
        pubSeq:3, pubHead:'FIRST BLOOD', pubSub:'EMPIRE took KESSEL', dipl:-1, diplBack:-1, noteSeq:1, note:'', qProg:1, qGoal:3, qDone:0,
        buys:2, cuts:3, perks:{merc:0, reroll:0, cargo:0, harbor:0, permit:0}, questDone:[0,1,0,1], awards:{merchant:3, customer:1},
        // 2.0 deals (you = slot 1): one offer waiting on you, one of yours out, one promise in force.
        dealsWaiting:1, dealAsks:["Don't attack my worlds", 'Stay away from a world', 'Send me Credits next round', 'Your market cut from a world'],
        deals:[{i:0, st:1, payer:3, promiser:1, turn:1, ask:0, world:-1, give:5, askCr:0, counters:0, until:-1,
                askText:"You don't attack the Crimson Dawn's worlds for 2 rounds", say:'Hit the Pykes instead. Everyone wins but them.'},
               {i:2, st:1, payer:1, promiser:2, turn:2, ask:1, world:5, give:3, askCr:0, counters:1, until:-1,
                askText:'They stay away from KESSEL for 2 rounds', say:'Too low.'},
               {i:4, st:2, payer:0, promiser:1, turn:-1, ask:0, world:-1, give:4, askCr:0, counters:0, until:5,
                askText:"You don't attack the Galactic Empire's worlds until round 5", say:'Compliance is always cheaper.'}],
        dealLines:[{i:0, t:"The Alliance doesn't forget a friend."}, {i:1, t:'We all want the same thing: the Empire gone.'}, {i:2, t:'A fair trade, between friends.'},
                   {i:12, t:'Too low.'}, {i:13, t:'Add a promise.'}, {i:14, t:'Final offer.'}],
        wordF:PN.map((n, i) => i === 4 ? 'You gave your word to the Galactic Empire: no attacks on their worlds until round 5.' : ''),
        wordA:PN.map(() => ''),
        ashen:{alive:0, hp:0, max:0, mine:0, seq:0, lastSlot:-1, ships:0, dmg:0, roll:0, hits:0, vp:2, topVp:1},
        factions:[{q:'SPARK OF REBELLION', qt:'Win 3 battles against the Empire or Imperial-sympathetic worlds', s:'REBEL SYMPATHIZERS', st:'You see where every other diplomat goes'},
                  {q:'TOTAL ORDER', qt:'Hold both Core worlds at the end', s:'IMPERIAL NAVY', st:'All your fleets hold 7 ships (coming in the next build)'},
                  {q:'SPICE EMPIRE', qt:'End the game with the most Credits', s:'SPICE MONEY', st:'+2 CR every Prepare'},
                  {q:'OWN THE POWERFUL', qt:'Be Favorable or better with all three other factions', s:'SILENT PARTNERS', st:'+1 CR when another player buys at a market (max 2 a round)'}]},
    battleKind:1, battleActive:true, battleAttackerSlot:1, battleDefenderSlot:-1, battleTargetPlanet:1, defRemaining:3,
    battleAtkShips:3, battleDefShips:0, atkAction:0, defAction:0, lastAtkRoll:6, lastDefRoll:2, battleExchange:2, lastLossIdx:-1,
    battleAtkAlive:[1,1,1,0,0,0], battleDefAlive:[0,0,0,0,0,0], battleOver:false, battleAtkMod:1, battleDefMod:0, lastRetreatSide:0,
    fightMods:{atk:0, atkSrc:-1, def:0, defSrc:-1},
    takeoverPlayer:-1, takeoverPlanet:-1, takeoverMethod:0, payPrice:0, takeoverResult:'', offerA:null, offerB:null,
    resultText:'You 6 vs Def 2 -- a garrison ship destroyed! (3 left)', alert:'',
    reputation:[0,1,2,3].map(()=>({tier:'NEUTRAL'})), transmissionPending:false, transmissionFaction:-1,
    transmissionIsObligation:false, transmissionQuote:'', winner:-1,
  };
  return Object.assign(s, o || {});
}
// v0.30.0 a conversation on Kessel (planet 5) with the Pykes, as /state sends it.
function negFake(o) {
  return Object.assign({planet:5, faction:2, count:3, step:1, result:0, losses:0, tipped:false, favorStage:false, hard:false,
                        marks:[1, 0, 0], tier:1, q:'A family here can\x27t pay their docking fees. What do we do?',
                        replies:[{i:1, text:'They knew the terms. They leave.'}, {i:0, text:'Let them stay. We\x27ll cover it.'},
                                 {i:2, text:'Find them work. A debt can be paid in many ways.'}],
                        favors:[], owe:'', vp:2, late:false, roundsLeft:5, lateCredits:0, payPrice:7, paid:false, line:'', own:false}, o || {});
}
function negState(neg, unknown) {
  const s = fakeState({battleActive:false, battleKind:0, takeoverPlayer:1, takeoverPlanet:5, takeoverMethod:2, neg:neg});
  if (!unknown) s.planets[5].chars[0] = {revealed:true, value:'Pyke Syndicate', cost:2};
  s.planets.forEach(p => p.ring = 0);
  return s;
}
localStorage.setItem('sf_id', 'harness');
// Mode from the URL hash. '' = mid-battle page + battle contact sheet.
// Every other mode is one phone screen, screenshotted by phone_check.sh to
// build/phone_<mode>.png. HARNESS_MODES[mode].state builds its fake /state;
// .action (run by harness_post.js after the page loads) taps into it.
// 2.0-dev.21: put these goods (GoodId) on the four open markets [0, 3, 6, 9]; the one on 9 is sold out,
// the one on 3 is cheaper for you (good standing), the one on 6 dearer.
const GOOD_N = ['FUEL', 'REPAIRS', 'SURVEY', 'BLESSING', 'MERCENARIES', 'SMUGGLED SHIP', 'AUDIENCE', 'CARGO RUN', 'SAFE HARBOR', 'BACTA', 'FORGED PERMITS', 'GARRISON TROOPS'];
function shelfGoods(s, ids) {
  [0, 3, 6, 9].forEach((pi, k) => {
    const p = s.planets[pi];
    p.goodId = ids[k]; p.good = GOOD_N[ids[k]]; p.goodBase = 3; p.mPrice = k === 1 ? 2 : k === 2 ? 4 : 3;
    p.mStock = pi === 9 ? 0 : k === 1 ? 1 : 2; p.mBuy = p.mStock > 0;
  });
  s.planets[3].owner = 1; s.planets[6].owner = 3;
  // 3.1-dev.9 faction stalls (Rebels / Empire / Pykes / Crimson Dawn, two goods each)
  const FX = ['One fleet may move 2 steps this round', 'Repair 2 ships', 'Reveal all four secrets of a world you pick', 'Reroll once in your next battle',
              '2 hired ships fight in your next battle', '+1 ship to a fleet', 'Better standing with a faction you pick', 'Get 4 CR back next Prepare',
              "Players can't attack your fleets this round", 'Repair 1 ship', 'Your next world purchase costs 3 CR less', '+1 defender on your weakest world'];
  const BASE = [2, 3, 3, 3, 3, 1, 2, 2, 2, 1, 2, 2];
  s.v2.goods = GOOD_N.map((n, i) => [n, FX[i], BASE[i]]);
  s.v2.stalls = [[9], [2], [7], [6]].map((it, f) => ({f: f, shut: 0, it: it.map((g, k) => [g, f === 2 && k === 1 ? 0 : 2, f === 0 ? Math.max(1, BASE[g] - 1) : BASE[g], 1])}));
  return s;
}
const HARNESS_MODE = location.hash.slice(1);
// 2.0: the Ashen Fleet alive, mid-fight (you = slot 1 struck last).
function ashenOn(s) { s.v2.ashen = {alive:1, hp:22, max:40, mine:8, seq:3, lastSlot:1, ships:5, dmg:17, roll:4, hits:1, vp:2, topVp:1}; return s; }
function defState() {
  const s = fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'});
  s.players[s.yourSlot].credits = 21;
  const own = (i, d, dl, pt) => { s.planets[i].owner = s.yourSlot; s.planets[i].defense = d; s.planets[i].dl = dl; if (pt) { s.planets[i].pt = pt; s.planets[i].ps = 2; } };
  own(1, 1, 6); own(5, 5, 8); own(9, 7, 10, 1);
  return s;
}
const HARNESS_MODES = {
  // Battle briefing on an Imperial world: modifiers both sides, fleet 1
  // already spent this round, fleet 2 ready.
  // 3.1-dev.67 keywords (a tap is simulated) + the live FAVOR / STANDINGS buttons (kwhome)
  kwhome: { state: () => { const s = HARNESS_MODES.gyou.state(); [7, 4, 9, 2].forEach((v, k) => { if (s.players[k]) s.players[k].vp = v; }); return s; }, action: () => {} },
  kwforecast: { state: () => HARNESS_MODES.briefing.state(), action: () => { HARNESS_MODES.briefing.action(); setTimeout(() => { const k = document.querySelector('.kw'); if (k) k.click(); }, 500); } },
  kwplanet: { state: () => fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'}), action: () => { openPlanet(5); setTimeout(() => { const k = document.querySelectorAll('.intel .kw')[1]; if (k) k.click(); }, 1500); } },
  kwgyou: { state: () => gyCrowd(), action: () => setTimeout(() => { sheetHide(); const k = document.querySelector('.gy .kw'); if (k) k.click(); }, 500) },
  briefing: {
    state: () => {
      const s = fakeState({battleActive:false, battleKind:0, takeoverPlayer:1, takeoverPlanet:4, takeoverMethod:0,
                           fightMods:{atk:0, atkSrc:0, def:0, defSrc:1, esc:1, mil:1, merc:2, gLo:8, gHi:8}});  // 2.0-dev.24: Rebel escort + Imperial militia
     
      s.planets[4].chars[0] = {revealed:true, value:'Galactic Empire', cost:2};
      s.planets[4].chars[3] = {revealed:true, value:'High', cost:3};
      s.players[1].fleets[0].spent = true;
      s.players[1].fleets[1] = {ships:[1,1,0,0,0,0].map(Boolean), alive:2, total:2, spent:false};
      return s;
    },
    action: () => showBriefing(),
  },
  // UI 3.0 step 2: Resolution = the galaxy. A world's sheet open, then a rival fleet's.
  resgalaxy: {
    state: () => { const s = fakeState({battleActive:false, battleKind:0, phase:2, phaseName:'RESOLUTION'});
                   s.planets.forEach((p, i) => { if (p.owner !== 1) p.gr = [2 + (p.ring|0) * 2, 3 + (p.ring|0) * 2]; });
                   s.planets[5].chars[1] = {revealed:true, value:'High', cost:2}; s.planets[5].chars[2] = {revealed:true, value:'Spice mines', cost:1};
                   s.players[1].fleets[0] = {ships:[1,1,1,1,1,0].map(Boolean), alive:5, total:5, spent:false};
                   s.players[0].fleets[0] = {ships:[1,1,1,1,0,0].map(Boolean), alive:4, total:4, spent:false};
                   s.players[0].fleets[1] = {ships:[1,1,0,0,0,0].map(Boolean), alive:2, total:2, spent:false};
                   currentPlanet = 5; return s; }, action: () => {} },  // take 4: a world's page in Resolution
  movement: { state: () => { const s = HARNESS_MODES.resgalaxy.state(); s.phase = 1; s.phaseName = 'MOVEMENT'; currentPlanet = -1;  // UI 3.0 step 4
                             s.rumor = "Kessel's garrison is down to 2. You didn't hear it from me."; s.rumorP = -1; return s; }, action: () => {} },
  movmine: { state: () => { const s = HARNESS_MODES.movement.state(); const w = 9; s.planets[w].owner = 1;  // harness mySlot = 1  // dev.41
                            s.rumorP = w; s.rumor = 'Someone is pretty interested in ' + s.planets[w].name.charAt(0) + s.planets[w].name.slice(1).toLowerCase() + ', huh?'; return s; }, action: () => {} },
  resnone: { state: () => { const s = HARNESS_MODES.resgalaxy.state(); currentPlanet = -1; return s; }, action: () => {} },  // take 4: Resolution home
  attackplayers: { state: () => { const s = HARNESS_MODES.resgalaxy.state(); currentPlanet = -1; attackPlayersMode = true; return s; }, action: () => {} },
  resfleet: {
    state: () => { const s = HARNESS_MODES.resgalaxy.state(); sheetPlanet = -1; sheetFleet = {slot:0, f:0}; return s; }, action: () => {} },
  // 2.0-dev.27 forecast v2 on a world you haven't read: odds as a range + both informants (3x price).
  briefingblind: {
    state: () => {
      const s = fakeState({battleActive:false, battleKind:0, takeoverPlayer:1, takeoverPlanet:4, takeoverMethod:0,
                           fightMods:{atk:0, atkSrc:-1, def:0, defSrc:-1, esc:0, mil:0, merc:0, gLo:7, gHi:9}});
      s.planets[4].chars[0] = {revealed:false, value:'', cost:6};
      s.planets[4].chars[3] = {revealed:false, value:'', cost:9};
      s.players[1].fleets[0] = {ships:[1,1,1,1,1,1].map(Boolean), alive:6, total:6, spent:false};
      s.players[1].fleets[1] = {ships:[1,1,1,1,0,0].map(Boolean), alive:4, total:4, spent:false};
      s.players[1].credits = 9;
      return s;
    },
    action: () => { showBriefing(); setTimeout(() => pickBriefFleet(1), 150); },
  },
  // 2.0-dev.24 a planet battle with a Rebel ESCORT (gold ship, one column in) and an Imperial MILITIA square.
  battleescort: {
    state: () => fakeState({battleKind:1, battleActive:true, battleAttackerSlot:1, battleTargetPlanet:4, defRemaining:4,
                            battleExchange:0, lastAtkRoll:0, lastDefRoll:0, battleAtkMod:0, shipStride:7,
                            battleAtkAlive:[1,1,1,1,1,0,0, 0,0,0,0,0,0,0, 0,0,0,0,0,0,0], battleAtkFleets:1,
                            bEsc:[0,1], bMil:[1,1], bMerc:[2,0], resultText:''}),  // dev.26: + 2 hired ships
    action: () => {},
  },
  // v0.28.0 combined assault: fleets 1 + 2 both selected in the forecast (6 + 4 ships).
  briefingduo: {
    state: () => {
      const s = fakeState({battleActive:false, battleKind:0, takeoverPlayer:1, takeoverPlanet:4, takeoverMethod:0,
                           fightMods:{atk:0, atkSrc:-1, def:0, defSrc:-1}});
      s.players[1].fleets[0] = {ships:[1,1,1,1,1,1].map(Boolean), alive:6, total:6, spent:false};
      s.players[1].fleets[1] = {ships:[1,1,1,1,0,0].map(Boolean), alive:4, total:4, spent:false};
      return s;
    },
    action: () => { showBriefing(); setTimeout(() => pickBriefFleet(1), 150); },
  },
  // v0.28.0 PvP with the attacker's fleets 1 + 2 against your fleet 1: the BRING FLEET 2 offer.
  battleduo: {
    state: () => fakeState({battleKind:2, battleActive:true, battleAttackerSlot:0, battleDefenderSlot:1, battleExchange:0,
                            lastAtkRoll:0, lastDefRoll:0, battleAtkMod:0,
                            battleAtkAlive:[1,1,1,1,0,0, 1,1,1,0,0,0, 0,0,0,0,0,0], battleDefAlive:[1,1,1,0,0,0, 0,0,0,0,0,0, 0,0,0,0,0,0],
                            battleAtkFleets:3, battleDefFleets:1, canReinforce:2}),
    action: () => {},
  },
  // 2.0-dev.20 the Empire's 7-ship fleets: the Empire attacks with fleets 1 (7 ships) + 2 (3) against your 6.
  battleseven: {
    state: () => fakeState({battleKind:2, battleActive:true, battleAttackerSlot:0, battleDefenderSlot:1, battleExchange:0,
                            lastAtkRoll:0, lastDefRoll:0, battleAtkMod:0, shipStride:7,
                            battleAtkAlive:[1,1,1,1,1,1,1, 1,1,1,0,0,0,0, 0,0,0,0,0,0,0], battleDefAlive:[1,1,1,1,1,1,0, 0,0,0,0,0,0,0, 0,0,0,0,0,0,0],
                            battleAtkFleets:3, battleDefFleets:1, canReinforce:0}),
    action: () => {},
  },
  // ... and an Empire player's fleet screen: 7 slots, 6 built -> INVEST NEW SHIP still offered.
  fleetseven: { state: () => { const s = fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'});
                               s.players[1].chosenFaction = 1; s.players[1].cap = 7;
                               s.players[1].fleets[0] = {ships:[true,true,true,true,true,true,false], alive:6, total:6, spent:false}; return s; },
                action: () => { openFleets(); setTimeout(() => openFleetDetail(0), 200); } },
  // "Leave negotiation?" confirm on Kessel (v0.30.0: mid-conversation).
  leave: {
    state: () => fakeState({battleActive:false, battleKind:0, takeoverPlayer:1, takeoverPlanet:5, takeoverMethod:2, neg:negFake()}),
    action: () => askLeaveNegotiation(),
  },
  // Battle with WITHDRAW armed (first tap).
  armed: { state: () => fakeState(), action: () => bAct(2) },
  // v0.21.0 Home hub in Resolution (ATTACK shown), with some owned planets.
  home: {
    state: () => {
      const s = fakeState({battleActive:false, battleKind:0});
      s.planets[3].owner = 1; s.planets[0].owner = 1; s.planets[1].owner = 0; s.planets[5].owner = 3;
      s.players[0].vp = 8; s.players[3].vp = 5; s.players[0].lockedIn = true;
      return s;
    },
    action: () => {},
  },
  // v0.21.0 Standings.
  standings: {
    state: () => {
      const s = fakeState({battleActive:false, battleKind:0});
      s.planets[3].owner = 1; s.planets[0].owner = 1; s.planets[1].owner = 0; s.planets[4].owner = 0; s.planets[8].owner = 0; s.planets[5].owner = 3;
      s.players[0].vp = 8; s.players[3].vp = 5;
      return s;
    },
    action: () => openStandings(),
  },
  // 2.0-dev.6: END GAME's "are you sure?" card.
  endconfirm: { state: () => fakeState({battleActive:false, battleKind:0}), action: () => { openStandings(); endGameTap(); } },
  // v0.21.1 round/phase banner (streaks mid-animation).
  banner: { state: () => fakeState({battleActive:false, battleKind:0, round:4, phase:0, phaseName:'PREPARE'}), action: () => showBanner(lastS) },
  // 2.0: the markets screen, a Prepare planet page (market + diplomat + trait), the Prepare home hub.
  markets: { state: () => shelfGoods(fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'}), [4, 9, 0, 7]), action: () => openMarkets() },
  // 2.0-dev.21: the other eight goods on the shelves
  marketsb: { state: () => shelfGoods(fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'}), [1, 5, 3, 2]), action: () => openMarkets() },
  marketsc: { state: () => shelfGoods(fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'}), [6, 11, 10, 8]), action: () => openMarkets() },
  prepplanet: {
    state: () => { const s = fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'});
                   s.planets[0].chars[0] = {revealed:true, value:'Pyke Syndicate', cost:2};
                   s.planets[0].chars[2] = {revealed:true, value:'Valuable Intelligence', cost:1};
                   s.planets[0].traitFx = 'Your first reveal each round is free';
                   s.planets[0].owner = -1; s.planets[0].gr = [2, 4]; attackOpen = 0; return s; },  // dev.29: the attack line, opened
    action: () => openPlanet(0),
  },
  prephub: { state: () => { const s = fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'});  // dev.29: favor cards
                             s.promises = [{f:2, p:5, due:s.round + 2, chk:6, text:'Spice Contract: 1 Credit to us at every round start for 3 rounds.'},
                                           {f:0, p:3, due:s.round, chk:0, text:'Keep your fleets out of the Core until the deadline.'}];
                             // UI 3.0 step 3: a hangar with a lost ship (FIX), an empty bay and a destroyed fleet
                             s.players[1].fleets[0] = {ships:[1,1,1,0,1,0].map(Boolean), alive:4, total:5, spent:false};
                             s.players[1].fleets[1] = {ships:[0,0,0,0,0,0].map(Boolean), alive:0, total:0, spent:false};
                             s.players[1].fleets[2] = {ships:[0,0,0,0,0,0].map(Boolean), alive:0, total:3, spent:false};
                             return s; }, action: () => {} },
  lockdeal: { state: () => fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'}), action: () => lockIn() },
  // 2.0 THE ASHEN FLEET: its screen in Resolution (after a strike), the map, the boarded-up markets, ATTACK.
  ashen: { state: () => ashenOn(fakeState({battleActive:false, battleKind:0})),
           action: () => { openAshen(); ashenResult = '5 ships hit it for 17. It answered with a 4: 1 ship lost.'; shownHtml = null; render(lastS); } },
  ashenmap: { state: () => ashenOn(fakeState({battleActive:false, battleKind:0})), action: () => openGalaxy() },
  ashenmarkets: { state: () => ashenOn(fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'})), action: () => openMarkets() },
  ashenattack: { state: () => ashenOn(fakeState({battleActive:false, battleKind:0})), action: () => openAttack() },
  dealword: {
    state: () => { const s = fakeState({battleActive:false, battleKind:0}); s.planets[4].owner = 0; return s; },
    action: () => openPlanet(4),
  },
  // v0.30.5 PAY confirm (Resolution, Affiliation revealed) -- broken by an undefined negF in v0.30.0-v0.30.4.
  payconfirm: {
    state: () => { const s = fakeState({battleActive:false, battleKind:0, takeoverPlayer:1, takeoverPlanet:4, takeoverMethod:0, payPrice:9});
                   s.planets[4].chars[0] = {revealed:true, value:'Galactic Empire', cost:2}; return s; },
    action: () => showPayConfirm(),
  },
  // v0.21.1 lock-in confirm (Resolution, credits + a ready fleet left).
  lockconfirm: { state: () => fakeState({battleActive:false, battleKind:0}), action: () => lockIn() },
  // 3.1-dev.5: the scoreboard counts 9 -> 20 Credits (the screenshot must end on 20).
  countup: { state: () => { const s = fakeState({battleActive:false, battleKind:0, phase:0}); if (window.cuDone) s.players[1].credits = 20; return s; },
             action: () => setTimeout(() => { window.cuDone = true; poll(); }, 400) },
  // 3.1-dev.3: the Feed (Inbox + History) from a fake event log.
  feedinbox: { state: () => feedFake(), action: () => setTimeout(() => openFeed('inbox'), 300) },
  feedhist:  { state: () => feedFake(), action: () => setTimeout(() => openFeed('history'), 300) },
  // 3.1-dev.2: notifications stacked at the top over Home (newest on top).
  notifs: { state: () => fakeState({battleActive:false, battleKind:0}),
            action: () => { notify('THE PYKES · FAVOR UP', 'A face → Client · +6 Credits', '#A77BFF', 60000);
                             setTimeout(() => notify('LOOK AT THE TABLE · FIRST BLOOD', 'The Empire took Naboo', '#FFC933', 60000), 300); } },
  // v0.21.2 Galaxy cards (you = slot 1, Rebels), one planet left out of play.
  galaxy: {
    state: () => {
      const s = fakeState({battleActive:false, battleKind:0});
      s.planets[3].owner = 1; s.planets[0].owner = 1; s.planets[1].owner = 0; s.planets[5].owner = 3;
      s.planets[13].owner = 0; s.planets[7].owner = 3;
      s.planets[12].owner = 0;  // the Empire took the Rebels' home: their emergency lane opens
      return s;
    },
    action: () => openGalaxy(),
  },
  // 3.1-dev.58 RIVET'S GUIDES
  guides: { state: () => fakeState({battleActive:false, battleKind:0}), action: () => { primerGuide = ''; primerAt = 0; shownHtml = null; poll(); } },
  g_favor0: { state: () => fakeState({battleActive:false, battleKind:0}), action: () => { primerGuide = 'favor'; primerAt = 0; shownHtml = null; poll(); } },
  g_favor1: { state: () => fakeState({battleActive:false, battleKind:0}), action: () => { primerGuide = 'favor'; primerAt = 1; shownHtml = null; poll(); } },
  g_news0: { state: () => fakeState({battleActive:false, battleKind:0}), action: () => { primerGuide = 'news'; primerAt = 0; shownHtml = null; poll(); } },
  g_news1: { state: () => fakeState({battleActive:false, battleKind:0}), action: () => { primerGuide = 'news'; primerAt = 1; shownHtml = null; poll(); } },
  g_battle0: { state: () => fakeState({battleActive:false, battleKind:0}), action: () => { primerGuide = 'battle'; primerAt = 0; shownHtml = null; poll(); } },
  g_battle1: { state: () => fakeState({battleActive:false, battleKind:0}), action: () => { primerGuide = 'battle'; primerAt = 1; shownHtml = null; poll(); } },
  g_moves0: { state: () => fakeState({battleActive:false, battleKind:0}), action: () => { primerGuide = 'moves'; primerAt = 0; shownHtml = null; poll(); } },
  g_moves1: { state: () => fakeState({battleActive:false, battleKind:0}), action: () => { primerGuide = 'moves'; primerAt = 1; shownHtml = null; poll(); } },
  g_scoundrel0: { state: () => fakeState({battleActive:false, battleKind:0}), action: () => { primerGuide = 'scoundrel'; primerAt = 0; shownHtml = null; poll(); } },
  g_gallery0: { state: () => fakeState({battleActive:false, battleKind:0}), action: () => { primerGuide = 'gallery'; primerAt = 0; shownHtml = null; poll(); } },
  g_finalhour0: { state: () => fakeState({battleActive:false, battleKind:0}), action: () => { primerGuide = 'finalhour'; primerAt = 0; shownHtml = null; poll(); } },
  g_finalhour1: { state: () => fakeState({battleActive:false, battleKind:0}), action: () => { primerGuide = 'finalhour'; primerAt = 1; shownHtml = null; poll(); } },
  g_finalhour2: { state: () => fakeState({battleActive:false, battleKind:0}), action: () => { primerGuide = 'finalhour'; primerAt = 2; shownHtml = null; poll(); } },
  g_galaxy0: { state: () => fakeState({battleActive:false, battleKind:0}), action: () => { primerGuide = 'galaxy'; primerAt = 0; shownHtml = null; poll(); } },
  // 2.0-dev.14 HOW TO PLAY primer, one mode per card (you = the Rebels)
  primera: {
    state: () => fakeState({battleActive:false, battleKind:0}),
    action: () => { primerAt = 0; shownHtml = null; poll(); },
  },
  primerb: {
    state: () => fakeState({battleActive:false, battleKind:0}),
    action: () => { primerAt = 1; shownHtml = null; poll(); },
  },
  primerc: {
    state: () => fakeState({battleActive:false, battleKind:0}),
    action: () => { primerAt = 2; shownHtml = null; poll(); },
  },
  primerd: {
    state: () => fakeState({battleActive:false, battleKind:0}),
    action: () => { primerAt = 3; shownHtml = null; poll(); },
  },
  primere: {
    state: () => fakeState({battleActive:false, battleKind:0}),
    action: () => { primerAt = 4; shownHtml = null; poll(); },
  },
  primerf: {
    state: () => fakeState({battleActive:false, battleKind:0}),
    action: () => { primerAt = 5; shownHtml = null; poll(); },
  },
  primerg: {
    state: () => fakeState({battleActive:false, battleKind:0}),
    action: () => { primerAt = 6; shownHtml = null; poll(); },
  },
  primerh: {
    state: () => fakeState({battleActive:false, battleKind:0}),
    action: () => { primerAt = 7; shownHtml = null; poll(); },
  },
  primern: {
    state: () => fakeState({battleActive:false, battleKind:0}),
    action: () => { primerAt = 13; shownHtml = null; poll(); },
  },
  // 2.0-dev.13 drop out / rejoin: a new phone mid-game (the Pykes are AWAY), back with a recap, stepped away
  seataway: {
    state: () => { const s = fakeState({battleActive:false, battleKind:0, yourSlot:-1}); s.players[2] = Object.assign({}, s.players[0], {chosenFaction:2, name:'PYKE SYNDICATE', away:1}); return s; },
    action: () => {},
  },
  welcome: {
    state: () => { const s = fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE', round:5});
                   s.planets[3].owner = 1; s.planets[2].owner = 1; s.planets[0].owner = 0;
                   s.welcome = {round:3, vp:9, cr:4, worlds:(1 << 0) | (1 << 3) | (1 << 12)}; s.planets[12].owner = 1; return s; },
    action: () => {},
  },
  stepped: {
    state: () => { const s = fakeState({battleActive:false, battleKind:0}); s.players[1].away = 2; return s; },
    action: () => {},
  },
  // 2.0 home worlds: yours, taken (Resolution) / the Empire's, held by them / a neutral one
  phomelost: {
    state: () => { const s = fakeState({battleActive:false, battleKind:0}); s.planets[12].owner = 0; s.planets[12].vp = 6; return s; },
    action: () => openPlanet(12),
  },
  phomeenemy: {
    state: () => { const s = fakeState({battleActive:false, battleKind:0}); s.planets[13].owner = 0; s.planets[13].vp = 6; return s; },
    action: () => openPlanet(13),
  },
  phomeneutral: {
    state: () => { const s = fakeState({battleActive:false, battleKind:0}); s.planets[6].vp = 3; return s; },
    action: () => openPlanet(6),
  },
  // v0.21.4 planet page: Invest, someone else's planet (Affiliation known).
  pinvest: {
    state: () => {
      const s = fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'INVEST'});
      s.planets[4].owner = 0;
      s.planets[4].chars[0] = {revealed:true, value:'Galactic Empire', cost:2};
      return s;
    },
    action: () => openPlanet(4),
  },
  // v0.21.4 planet page: Invest, your own planet (defense first).
  pown: {
    state: () => {
      const s = fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'INVEST'});
      s.planets[3].owner = 1; s.planets[3].defense = 3;
      s.planets[3].chars[1] = {revealed:true, value:'Low', cost:2};
      return s;
    },
    action: () => openPlanet(3),
  },
  // v0.21.4 planet page: Movement, read-only intel.
  pmove: {
    state: () => {
      const s = fakeState({battleActive:false, battleKind:0, phase:1, phaseName:'MOVEMENT'});
      s.planets[0].chars[0] = {revealed:true, value:'Crimson Dawn', cost:2};
      s.planets[0].chars[2] = {revealed:true, value:'Valuable Intelligence', cost:3};
      return s;
    },
    action: () => openPlanet(0),
  },
  // v0.21.6 planet page in Resolution: FIGHT / NEGOTIATE / PAY on the page.
  pres: {
    state: () => {
      const s = fakeState({battleActive:false, battleKind:0});
      s.planets[4].owner = 0; s.planets[4].pay = 14;
      s.planets[4].chars[0] = {revealed:true, value:'Galactic Empire', cost:2};
      s.planets[4].chars[1] = {revealed:true, value:'High', cost:2};
      return s;
    },
    action: () => openPlanet(4),
  },
  // v0.21.6: a refused Pay lands back on the planet page with the reason.
  prefused: {
    state: () => {
      const s = fakeState({battleActive:false, battleKind:0, takeoverPlayer:1, takeoverPlanet:2, takeoverMethod:3,
                           takeoverResult:'Not enough Credits to purchase BESPIN.'});
      return s;
    },
    action: () => {},
  },
  // v0.21.10 attack flow: Fleets in attack mode -> opponents -> their
  // fleets -> PvP forecast.
  fleets: { state: () => fakeState({battleActive:false, battleKind:0}), action: () => openFleets() },
  // v0.25.1 restyles: purchase confirm (Affiliation revealed -> +1 line),
  // fleet detail in Invest with a destroyed ship, and game over (you won).
  pay: { state: () => { const s = fakeState({battleActive:false, battleKind:0, takeoverPlayer:1, takeoverPlanet:9, takeoverMethod:0, payPrice:7});
                        s.planets[9].chars[0] = {revealed:true, value:'Rebel Alliance', cost:2}; return s; },
         action: () => { payConfirmPending = true; poll(); } },
  fleetdetail: { state: () => { const s = fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'INVEST'});
                                s.players[1].fleets[0] = {ships:[true,false,true,false,false,false], alive:2, total:3, spent:false}; return s; },
                 action: () => { openFleets(); setTimeout(() => openFleetDetail(0), 200); } },
  gamestory: { state: () => { const s = fakeState({battleActive:false, battleKind:0, stage:2, winner:0}); s.players[1].vp = 14; s.players[0].vp = 16;
                  s.v2.questDone = [1, 0, 0, 0]; s.players.forEach((p, i) => { if (p.active) p.commander = p.chosenFaction * 4 + (i % 4); }); return s; },
                action: () => { const E = (seq, kind, round, who, planet, other, num) => ({seq, kind, round, who, planet, other, num, text:''});
                  FEED.pub = [E(1,2,2,1,5,-1,3), E(2,2,3,1,9,-1,1), E(3,2,4,0,2,-1,1), E(4,9,5,0,-1,-1,0), E(5,11,5,0,-1,1,14), E(6,11,6,0,-1,1,12), E(7,2,6,1,3,-1,2), E(8,11,7,1,4,1,16), E(9,2,7,0,8,-1,1)];
                  FEED.mine = [E(1,12,4,2,-1,5,5), E(2,5,6,0,3,-1,1)]; render(lastS); } },
  gameover: { state: () => { const s = fakeState({battleActive:false, battleKind:0, stage:2, winner:1});
                             s.players[1].vp = 14; s.players[0].vp = 11; s.players[3].vp = 9; return s; }, action: () => {} },
  targets: { state: () => fakeState({battleActive:false, battleKind:0}), action: () => { fleetsMode = true; attackWith(0); } },
  theirs: { state: () => fakeState({battleActive:false, battleKind:0}), action: () => { fleetsMode = true; attackWith(0); pickTarget(0); } },
  pvpfc: { state: () => fakeState({battleActive:false, battleKind:0}), action: () => { fleetsMode = true; attackWith(0); pickTarget(0); pickDefFleet(0); } },
  // v0.21.13 battle reports: a captured planet, and a lost PvP fight with a
  // reputation transmission waiting (chip shows; overlay is held).
  vicwin: {
    state: () => { const s = fakeState({battleActive:false, battleKind:0}); s.planets[1].owner = 1; return s; },
    action: () => { victory = { v: { pvp:false, leftIsAttacker:true, atkAlive:[1,1,0,0,0,0].map(Boolean), defAlive:[0,0,0,0,0,0].map(Boolean),
                                     retreat:0, captured:true, planetIdx:1, atkColor:'#FFC933', defColor:'#FF7A45', over:true } }; poll(); },
  },
  viclose: {
    state: () => fakeState({battleActive:false, battleKind:0, transmissionPending:true, transmissionFaction:1, transmissionQuote:'We will remember this.'}),
    action: () => { victory = { v: { pvp:true, leftIsAttacker:true, atkAlive:[0,0,0,0,0,0].map(Boolean), defAlive:[1,1,0,0,0,0].map(Boolean),
                                     retreat:0, captured:false, planetIdx:-1, atkColor:'#FFC933', defColor:'#A9B4C0', over:true } };
                    document.getElementById('txOverlay').classList.remove('active'); poll(); },  // harness only: the page-load poll opened it first
  },
  // v0.30.0 the conversation on Kessel (the Pykes): question 2 of 3, the first one liked.
  neg: { state: () => negState(negFake()), action: () => {} },
  neggif: { state: () => { const s = negState(negFake({faction:0, q:'An Imperial patrol is searching the market for a deserter. He is hiding in your hold. What now?',
                                                        replies:[{i:1, text:'He stays hidden. Nobody gets left behind.'}, {i:0, text:'Turn him in. The cause comes first.'},
                                                                 {i:2, text:'Get him off-world tonight, quietly.'}]})); s.planets[5].chars[0] = {revealed:true, value:'Rebel Alliance', cost:2}; return s; }, action: () => {} },
  // ...Favorable with them: your contact took one reply away on the last question.
  negtip: { state: () => negState(negFake({step:2, marks:[1, 2, 0], tier:2, tipped:true,
                                           q:'If this deal went wrong, who would you blame?',
                                           replies:[{i:1, text:'Whoever broke the terms.'}, {i:2, text:'No one, out loud.'}]})), action: () => {} },
  // ...unrevealed Affiliation: an UNKNOWN AMBASSADOR.
  negamb: { state: () => negState(negFake({faction:-1}), true), action: () => {} },
  // ...one miss: what will you do for Kessel? (three favors drawn from the bank, v0.30.1)
  negfav: { state: () => negState(negFake({step:3, marks:[1, 2, 1], q:undefined, replies:undefined, favorStage:true,
                                           favors:[{text:'Undercut Crimson Dawn: take one of their worlds within 3 rounds.'},
                                                   {text:'Interest: 2 Credits to us at every round start for 3 rounds.'},
                                                   {text:'A bigger network: hold Kessel and a Crimson Dawn world at the same time within 3 rounds.'}]})), action: () => {} },
  // v0.30.1 ...the last 2 rounds: Credits at more than the PAY price, or a deed due when the war ends.
  neglate: { state: () => negState(negFake({step:3, marks:[1, 2, 1], q:undefined, replies:undefined, favorStage:true, late:true, roundsLeft:1,
                                            lateCredits:11, payPrice:7,
                                            favors:[{text:'Settle your account. 11 Credits, now.', credits:true},
                                                    {text:'Hit Crimson Dawn: take one of their worlds before the war ends.'},
                                                    {text:'Clear the route: win a battle before the war ends.'}]})), action: () => {} },
  // v0.30.1 ...the unknown ambassador was hostile all along.
  neghostile: { state: () => negState(negFake({step:3, marks:[1, 1, 2], q:undefined, replies:undefined, result:5,
                                               line:'Every answer was right, and every one was a lie. We keep the receipts.'})), action: () => {} },
  // v0.30.1 ...their own question (Affiliation revealed).
  negown: { state: () => negState(negFake({own:true, q:'How do you keep a secret on a world full of ears?',
                                          replies:[{i:2, text:'I trust my people.'}, {i:0, text:'I make sure the ears belong to me.'}, {i:1, text:'I don\x27t say anything worth hearing.'}]})), action: () => {} },
  // ...won with a favor owed, and cut off.
  negwin: { state: () => { const s = negState(negFake({step:3, marks:[1, 2, 1], q:undefined, replies:undefined, result:2,
                                                       owe:'Spice Contract: 1 Credit to us at every round start for 3 rounds.'}));
                           s.planets[5].owner = 1; return s; }, action: () => {} },
  negcut: { state: () => negState(negFake({step:2, marks:[3, 3, 0], q:undefined, replies:undefined, result:3, losses:1})), action: () => {} },
  // v0.21.17 Factions: you are the Rebels (Allied with yourself), mixed tiers.
  factions: {
    state: () => fakeState({battleActive:false, battleKind:0,
                            reputation:[['ALLIED', 40], ['HOSTILE', -40], ['FAVORABLE', 14], ['WARY', -12]].map(([t, sc]) => ({tier:t, score:sc})),
                            promises:[{f:2, p:5, due:3, chk:6, text:'Spice Contract: 1 Credit to us at every round start for 3 rounds.'},
                                      {f:1, p:4, due:5, text:'Imperial Garrison: permit a permanent Imperial presence. (honor-system)'}],
                            promiseLog:[{f:0, p:9, round:2, kept:true, text:'Strike the Empire: take an Imperial world within 3 rounds.'}, {f:3, p:7, round:1, kept:false, text:'Remove a Rival: destroy another commander\x27s fleet within 3 rounds.'}]}),
    action: () => openReputation(),
  },
  // 3.1-dev.8 THE FAVOR SPINE: the Favor page (tier 1 out for you, tier 5 gift, a tier-4 gift earned), a ladder sheet,
  // Home's Galaxy and You rows, a stall shut to you.
  favor: {
    state: () => { const s = fakeState({battleActive:false, battleKind:0,
                            reputation:[['FAVORABLE', 14], ['HOSTILE', -36], ['ALLIED', 33], ['NEUTRAL', 9]].map(([t, sc]) => ({tier:t, score:sc})),
                            promises:[{f:2, p:5, due:3, chk:6, text:'Spice Contract: 1 Credit to us at every round start for 3 rounds.'}]});
                   s.v2.gifts = 1 | 4; return s; },
    action: () => openReputation(),
  },
  ladder: {
    state: () => { const s = fakeState({battleActive:false, battleKind:0, phase:0,
                            reputation:[['FAVORABLE', 14], ['WARY', -20], ['FAVORABLE', 12], ['NEUTRAL', 9]].map(([t, sc]) => ({tier:t, score:sc}))});
                   s.v2.gifts = 1 | 4; return s; },
    action: () => setTimeout(() => openLadder(2), 200),
  },
  // 3.1-dev.64 a crowded last Prepare: gycrowd = the Galaxy and You folded, gycrowdmore = opened
  gycrowd: { state: () => gyCrowd(), action: () => { gyOpen = false; setTimeout(gyShow, 400); } },
  gycrowdmore: { state: () => gyCrowd(), action: () => { gyOpen = true; setTimeout(gyShow, 400); } },
  g_gyou0: { state: () => fakeState({battleActive:false, battleKind:0}), action: () => { primerGuide = 'gyou'; primerAt = 0; shownHtml = null; poll(); } },
  g_gyou1: { state: () => fakeState({battleActive:false, battleKind:0}), action: () => { primerGuide = 'gyou'; primerAt = 1; shownHtml = null; poll(); } },
  gyou: {
    state: () => { const s = shelfGoods(fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE',
                            reputation:[['NEUTRAL', 9], ['HOSTILE', -36], ['ALLIED', 33], ['ALLIED', 31]].map(([t, sc]) => ({tier:t, score:sc})),
                            promises:[{f:2, p:5, due:2, chk:6, text:'Spice Contract: 1 Credit to us at every round start for 3 rounds.'}]}), [4, 9, 0, 7]);
                   s.v2.spy = {w:0, t:[1, 3, 2, 2]}; s.v2.stalls[1].shut = 1; return s; },
    action: () => {},
  },
  survey: {  // 3.1-dev.9: buying Survey asks which world
    state: () => shelfGoods(fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'}), [4, 9, 0, 7]),
    action: () => { openMarkets(); setTimeout(() => stallBuy(1, 1), 300); },
  },
  audience: {
    state: () => shelfGoods(fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'}), [4, 9, 0, 7]),
    action: () => { openMarkets(); setTimeout(() => stallBuy(3, 1), 300); },
  },
  // 3.1-dev.10 GALAXY NEWS: full-screen cards, the job offer, Home rows
  newsrace: { state: () => fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'}),
              action: () => setTimeout(() => annOpen(lastS, {kind:11, num:2, who:1, planet:4, other:-1, round:3}, false), 300) },
  newswarn: { state: () => fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'}),
              action: () => setTimeout(() => annOpen(lastS, {kind:12, num:0, who:2, planet:-1, other:0, round:3}, true), 300) },
  newsstrike: { state: () => fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'}),
              action: () => setTimeout(() => annOpen(lastS, {kind:12, num:5, who:3, planet:0, other:42, round:3}, true), 300) },
  newsfell: { state: () => fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'}),
              action: () => setTimeout(() => annOpen(lastS, {kind:12, num:5, who:1, planet:5, other:30, round:3}, true), 300) && setTimeout(strikeFinish, 700) },
  newsheld: { state: () => fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'}),
              action: () => setTimeout(() => annOpen(lastS, {kind:12, num:11, who:2, planet:5, other:52, round:3}, true), 300) && setTimeout(strikeFinish, 700) },
  newscommo: { state: () => fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'}),
              action: () => setTimeout(() => annOpen(lastS, {kind:12, num:7, who:3, planet:5, other:0, round:3}, true), 300) },
  jobsheet: { state: () => { const s = fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'}); s.v2.job = {f:1, w:5, due:4, st:1, ann:0}; return s; },
              action: () => setTimeout(() => openJobSheet(), 300) },
  gynews: { state: () => { const s = fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE',
                            reputation:[['NEUTRAL', 9], ['HOSTILE', -36], ['NEUTRAL', 0], ['NEUTRAL', 0]].map(([t, sc]) => ({tier:t, score:sc}))});
                            s.v2.grudge = {f:1, r:4}; s.v2.job = {f:0, w:5, due:4, st:2, ann:1}; s.v2.race = {w:2, f:3, u:4}; return s; },
            action: () => {} },
  // 3.1-dev.12 CHAMPIONS + ERAS
  eranews: { state: () => { const s = fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'}); s.v2.champ = [-1, -1, 3, -1]; s.v2.era = {f:2, by:3, from:3, until:6}; return s; },
             action: () => setTimeout(() => annOpen(lastS, {kind:11, num:12, who:2, planet:-1, other:3, round:3}, false), 300) },
  champnews: { state: () => { const s = fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'}); s.v2.champ = [0, -1, -1, -1]; s.v2.decrees = 1; return s; },
             action: () => setTimeout(() => annOpen(lastS, {kind:11, num:14, who:0, planet:-1, other:1, round:3}, false), 300) },
  decree: { state: () => { const s = fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'}); s.v2.champ = [1, -1, -1, -1]; s.v2.decrees = 1; return s; },
             action: () => setTimeout(() => openDecree(0), 300) },
  erahome: { state: () => { const s = fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'}); s.v2.champ = [-1, -1, -1, 1]; s.v2.decrees = 0;
                             s.v2.era = {f:3, by:1, from:3, until:6}; s.v2.myLie = ''; s.v2.vpHidden = 0; s.v2.decrees = 1; return s; }, action: () => {} },
  erapress: { state: () => { const s = fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'}); s.round = 5; s.v2.champ = [-1, s.yourSlot, -1, -1]; s.v2.decrees = 1 << 1; s.v2.income = 9;
                             s.planets[1].owner = s.yourSlot; s.planets[2].owner = s.yourSlot; s.planets[3].owner = s.yourSlot; s.planets[4].owner = s.yourSlot; s.planets[5].owner = 0; s.planets[6].owner = 0; s.planets[7].owner = 0; return s; },
             action: () => { try { localStorage.setItem('sfDec1', '3'); localStorage.removeItem('sfPress'); } catch (e) {} setTimeout(() => { shownHtml = null; render(lastS); }, 300); } },
  eradawn: { state: () => { const s = fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'}); s.round = 5; s.v2.champ = [-1, -1, -1, s.yourSlot]; s.v2.decrees = 0;
                            s.v2.era = {f:3, by:s.yourSlot, from:5, until:6}; s.v2.mark = 0; s.v2.fileBought = 0; s.bm = {o:1, lots:[7, 8, 2, 11], got:0, hold:[10, -1, -1]}; return s; }, action: () => {} },
  dawnbm: { state: () => HARNESS_MODES.eradawn.state(), action: () => setTimeout(() => openBlackMarket(), 300) },
  eraspice: { state: () => { const s = fakeState({battleActive:false, battleKind:0, phase:2, phaseName:'RESOLUTION'}); s.round = 5; s.v2.era = {f:2, by:0, from:5, until:6}; return s; },
             action: () => setTimeout(() => openPlanet(9), 300) },
  liesheet: { state: () => { const s = fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'}); s.v2.era = {f:3, by:1, from:3, until:6}; s.v2.myLie = ''; return s; },
             action: () => setTimeout(() => openLieSheet(), 300) },
  eramap: { state: () => { const s = fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'}); s.v2.era = {f:2, by:3, from:3, until:6}; return s; },
             action: () => openGalaxy() },
  // 3.1-dev.13 ONCE-A-GAME MOVES
  movewar: { state: () => { const s = fakeState({battleActive:false, battleKind:0}); const me = s.players[s.yourSlot]; me.commander = me.chosenFaction * 4 + 0; s.v2.moveUsed = 0; return s; },
             action: () => setTimeout(() => openMoveSheet(), 300) },
  warroom: { state: () => { const s = fakeState({battleActive:false, battleKind:0}); const me = s.players[s.yourSlot]; me.commander = me.chosenFaction * 4 + 0; s.v2.moveUsed = 1;
                            [3, 9, 12].forEach((p, k) => { s.planets[p].owner = s.yourSlot; s.planets[p].defense = [5, 4, 6][k]; }); s.v2.war = {by: s.yourSlot, w: 1, r: s.round}; return s; },
             action: () => setTimeout(() => openWarRoom(), 300) },
  warconfirm: { state: () => { const s = fakeState({battleActive:false, battleKind:0}); const me = s.players[s.yourSlot]; me.commander = me.chosenFaction * 4 + 0; s.v2.moveUsed = 0; s.phase = 2; return s; },
             action: () => setTimeout(() => confirmWar(1), 300) },
  wartarget: { state: () => { const s = fakeState({battleActive:false, battleKind:0}); s.planets[1].owner = s.yourSlot; s.planets[1].defense = 4; s.phase = 0; s.v2.war = {by: (s.yourSlot + 1) % 2, w: 1, r: s.round}; return s; },
             action: () => setTimeout(() => openWarInfo(), 300) },
  warresult: { state: () => { const s = fakeState({battleActive:false, battleKind:0}); s.v2.warLast = {by:0, w:1, won:1, a:9, d:5, log:[0x52,0x34,0x61,0x25,0x43,0x66,0x51,0x62,0x41]}; return s; },
             action: () => setTimeout(() => { annOpen(lastS, {kind:11, num:16, who:1, planet:1, other:0, round:3}, false); setTimeout(strikeFinish, 400); }, 300) },
  summitsheet: { state: () => { const s = fakeState({battleActive:false, battleKind:0, phase:0}); const me = s.players[s.yourSlot]; me.commander = me.chosenFaction * 4 + 1; s.v2.moveUsed = 0; return s; },
             action: () => setTimeout(() => { summitPick = [2, 8]; openMoveSheet(); }, 300) },
  coversheet: { state: () => { const s = fakeState({battleActive:false, battleKind:0, phase:0}); const me = s.players[s.yourSlot]; me.commander = me.chosenFaction * 4 + 2; s.v2.moveUsed = 0; return s; },
             action: () => setTimeout(() => openMoveSheet(), 300) },
  heistsheet: { state: () => { const s = fakeState({battleActive:false, battleKind:0, phase:0}); const me = s.players[s.yourSlot]; me.commander = me.chosenFaction * 4 + 3; s.v2.moveUsed = 0; return s; },
             action: () => setTimeout(() => heistApproach(2), 300) },
  heistresult: { state: () => fakeState({battleActive:false, battleKind:0, phase:0}),
             action: () => setTimeout(() => { annOpen(lastS, {kind:12, num:23, who:2, planet:5, other:16 + 5, round:3}, true); setTimeout(strikeFinish, 400); }, 300) },
  gymoves: { state: () => { const s = fakeState({battleActive:false, battleKind:0, phase:0}); const me = s.players[s.yourSlot]; me.commander = me.chosenFaction * 4 + 2; s.v2.moveUsed = 1;
                            s.v2.cover = {f:2, u:5, story:0}; s.v2.summit = {by:0, u:4, w:[2, 8, -1]}; s.v2.war = {by:0, w:3, r:4}; s.planets[3].owner = s.yourSlot; return s; }, action: () => {} },
  // 3.1-dev.14 the map window
  gzoom: { state: () => fakeState({battleActive:false, battleKind:0, phase:0}), action: () => { openGalaxy(); setTimeout(() => { gView.s = 2.2; gView.x = 90; gView.y = 60; gClamp(); gApply(); }, 300); } },
  gthreat: { state: () => { const s = fakeState({battleActive:false, battleKind:0, phase:0}); s.planets[3].owner = s.yourSlot; s.planets[3].defense = 2; s.planets[9].owner = s.yourSlot; s.planets[9].defense = 5;
                            s.v2.war = {by:0, w:9, r:4}; s.v2.race = {w:2, f:3, u:4}; s.v2.job = {f:0, w:5, due:4, st:2, ann:0}; return s; },
             action: () => { gFilter = 1; openGalaxy(); } },
  gchance: { state: () => { const s = fakeState({battleActive:false, battleKind:0, phase:0}); s.v2.race = {w:2, f:3, u:4}; s.v2.job = {f:0, w:5, due:4, st:2, ann:0}; return s; },
             action: () => { gFilter = 2; openGalaxy(); } },
  gsheet: { state: () => fakeState({battleActive:false, battleKind:0, phase:0}), action: () => { gFilter = 0; openGalaxy(); setTimeout(() => gPick(5), 300); } },
  // 3.1-dev.16 the round's Galaxy News on one screen (+ a commotion only you can explain, a job to answer)
  bcsheet: { state: () => { const s = fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'}); const me = s.players[s.yourSlot]; me.commander = me.chosenFaction * 4 + 1; s.v2.bcCan = 1; return s; },
             action: () => setTimeout(() => { bcPick = { t: 0, k: 1, a: 9, cr: 5 }; openBcSheet(); }, 300) },
  // 3.1-dev.102 GUNS FOR HIRE, beaten to a world, the ink is still wet
  // 3.1-dev.103 THE COCKPIT
  deckhome: { state: () => fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'}), action: () => setTimeout(() => deckGo('bridge'), 300) },
  deckcomms: { state: () => { const s = fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'}); s.v2.bcIn = [0, 0, -1, 5, 1]; s.v2.head = 4; s.v2.gfhCan = 1; return s; },
               action: () => setTimeout(() => deckGo('comms'), 300) },
  deckwar: { state: () => fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'}), action: () => setTimeout(() => deckGo('war'), 300) },
  deckres: { state: () => { const s = fakeState({battleActive:false, battleKind:0, phase:2, phaseName:'RESOLUTION'}); s.talks = [{by:0, p:9, f:-1}]; return s; },
             action: () => setTimeout(() => deckGo('bridge'), 300) },
  gfhsheet: { state: () => { const s = fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'}); const me = s.players[s.yourSlot]; me.commander = me.chosenFaction * 4; s.v2.gfhCan = 1; return s; },
              action: () => setTimeout(() => { gfhPick = { to: 0, ships: [0, 1], cr: 6 }; openGfhSheet(); }, 300) },
  gfhin: { state: () => { const s = fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'}); s.v2.gfhIn = [0, 3, 6]; return s; },
           action: () => setTimeout(() => openGfhIncoming(), 300) },
  beaten: { state: () => { const s = fakeState({battleActive:false, battleKind:0, phase:2, phaseName:'RESOLUTION'}); s.planets[5].owner = 0; s.planets[5].ink = [1, 0]; s.planets[5].nb = (1 << 2) | (1 << 9); s.v2.beaten = 5; return s; },
            action: () => setTimeout(() => openBeaten(5), 300) },
  inkworld: { state: () => { const s = fakeState({battleActive:false, battleKind:0, phase:2, phaseName:'RESOLUTION'}); s.planets[5].owner = 0; s.planets[5].ink = [1, 0]; return s; },
              action: () => setTimeout(() => openPlanet(5), 300) },
  bcin: { state: () => { const s = fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'}); s.v2.bcIn = [0, 0, -1, 5, 1]; return s; },
          action: () => setTimeout(() => openBcIncoming(), 300) },
  bcwarn: { state: () => { const s = fakeState({battleActive:false, battleKind:0, phase:2, phaseName:'RESOLUTION'}); s.v2.bcIn = [0, 2, 9, 5, 2]; return s; },
            action: () => setTimeout(() => planetNegotiate(9), 300) },
  newssit: { state: () => { const s = fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'}); s.round = 4;
                            s.promises = [{f:2, p:5, due:4, chk:3, text:'Spice Contract: 1 Credit to us at every round start.'}]; return s; },
              action: () => setTimeout(() => beatOpen(lastS, { pub: [{kind:11, num:0, who:-1, planet:11, other:2, round:4}], mine: [] }), 300) },
  newsbeat: { state: () => { const s = fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'}); s.v2.job = {f:1, w:9, due:4, st:1, ann:1}; return s; },
              action: () => setTimeout(() => beatOpen(lastS, { pub: [{kind:11, num:0, who:-1, planet:11, other:2, round:3}, {kind:11, num:4, who:1, planet:-1, other:-1, round:3}],
                                                        mine: [{kind:12, num:7, who:2, planet:11, other:0, round:3}, {kind:12, num:2, who:1, planet:9, other:1, round:3}] }), 300) },
  // 3.1-dev.20: the grudge as a hologram with a way out, push your luck, what each choice does to your favor
  grudgebeat: { state: () => { const s = fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'}); s.v2.grudge = {f:2, r:4, w:-1}; s.v2.grudgePay = 12; return s; },
                action: () => setTimeout(() => beatOpen(lastS, { pub: [{kind:11, num:2, who:3, planet:2, other:-1, round:3}], mine: [{kind:12, num:0, who:2, planet:-1, other:0, round:3}] }), 300) },
  // 3.1-dev.42 MAKE AMENDS: the grudge sheet with the faction's three deeds (Pykes; Empire with a deed you can't do; a world list open)
  grudgesheet: { state: () => { const s = fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'}); s.v2.grudge = {f:2, r:4, w:-1, dd:2, dk:[0,0,1], dw:[[],[],[5,9]]}; s.v2.grudgePay = 8; return s; },
                 action: () => setTimeout(() => { deedPick = 2; openGrudgeSheet(); }, 300) },
  grudgeemp: { state: () => { const s = fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'}); s.v2.grudge = {f:1, r:4, w:5, dd:1, dk:[0,1,0], dw:[[],[],[]]}; s.v2.grudgePay = 12; return s; },
               action: () => setTimeout(() => { deedPick = -1; openGrudgeSheet(); }, 300) },
  amends: { state: () => fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'}),
            action: () => setTimeout(() => beatOpen(lastS, { pub: [{kind:11, num:23, who:3, planet:9, other:4, round:3}], mine: [{kind:12, num:29, who:3, planet:-1, other:0, round:3}] }), 300) },
  // 3.1-dev.43 THE FINAL HOUR: the draw, Home rows, the black market, debts, last orders, the reveal, the finale playback
  fhdraw: { state: () => fhState(7, 0, {deal:[12,15,13]}), action: () => setTimeout(() => { fhDrawPick = 1; openFhDraw(); }, 300) },
  fhhome: { state: () => fhState(8, 0, {hand:(1<<12)|(1<<15)}), action: () => {} },
  fhbm: { state: () => fhState(7, 0, {hand:(1<<12)}), action: () => setTimeout(() => openFhMarket(), 300) },
  fhdebt: { state: () => fhState(8, 0, {called:2, ally:[0,1,0,0]}), action: () => setTimeout(() => openFhDebts(), 300) },
  fhorders: { state: () => fhState(8, 1, {hand:(1<<12)|(1<<15)|(1<<14), ally:[0,2,0,1], ships:[5,3,2], ord:{t:[11,4,-1,-1], gu:[-1,-1,9], al:[2,1,0,0], g:[12,-1,-1,-1]}}), action: () => {} },
  fhsmug: { state: () => fhState(8, 1, {hand:(1<<12)|(1<<15)|(1<<14)|(1<<3), ally:[0,2,0,1], ships:[5,3,2], ord:{t:[11,4,-1,6], gu:[-1,-1,9], al:[2,1,0,0], g:[12,-1,-1,-1]}}), action: () => window.scrollTo(0, 900) },
  fhtarget: { state: () => fhState(8, 1, {hand:(1<<12), ships:[5,3,2]}), action: () => setTimeout(() => { fhOrdLoad(lastS); openFhTarget(0); }, 300) },
  fhreveal: { state: () => fhState(8, 2, {hand:(1<<15)|(1<<5), strikes:[[4,1,11,7,0],[5,1,4,4,0],[0,0,11,5,0],[12,3,4,8,0],[13,3,6,6,0]], pins:[[4,12]],
                                          guard:[0,0,0,0,0,0,0,0,0,2,0,0,0,0,0,0,0], rv:[[5,12,-1],[-1,-1,-1],[-1,-1,-1]]}), action: () => {} },
  fhplay: { state: () => fhState(8, 2, {st:2, log:FH_TEST_LOG}), action: () => setTimeout(() => { FHP.i = 1; fhStep(); }, 200) },
  fhmelee: { state: () => fhState(8, 2, {st:2, log:FH_TEST_LOG}), action: () => setTimeout(() => { FHP.i = 1; fhStep(); clearTimeout(FHP.timer); FHP.timer = setTimeout(() => {}, 1); }, 200) },
  fhsync: { state: () => { const s = fhState(8, 2, {st:2, log:FH_TEST_LOG}); s.fh.show = 1; s.fh.sk = 1 << ((s.yourSlot + 1) % 4); return s; }, action: () => {} },  // 3.1-dev.117 in step with the table
  fhstart0: { state: () => { const s = fhState(8, 2, {st:2, log:FH_TEST_LOG}); s.fh.show = 0; s.fh.sk = 0; return s; }, action: () => {} },  // 3.1-dev.121 the finale's first screen
  fhstartU: { state: () => { const s = fhState(8, 2, {st:2, log:FH_TEST_LOG}); delete s.fh.show; return s; }, action: () => {} },  // before the table's clock ticks
  tour1: { state: () => fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'}), action: () => setTimeout(() => { tourStart(); }, 300) },  // 3.1-dev.124 the walkthrough
  tour2: { state: () => fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'}), action: () => setTimeout(() => { tourStart(); tourNext(); }, 300) },
  tour4: { state: () => fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'}), action: () => setTimeout(() => { tourStart(); tourI = 3; tourShow(); }, 300) },
  tour11: { state: () => fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'}), action: () => setTimeout(() => { tourStart(); tourI = 10; tourShow(); }, 300) },
  fguide: { state: () => fakeState({battleActive:false, battleKind:0, phase:2, phaseName:'RESOLUTION'}), action: () => setTimeout(() => { openFactionGuide(2); }, 300) },  // 3.1-dev.125
  fhend: { state: () => fhState(8, 2, {st:2, log:FH_TEST_LOG}), action: () => setTimeout(() => { fhSkip(); }, 200) },
  // 3.1-dev.48 SELL IT TWICE: the offer (heat 1), the heat mood on Home, caught
  sellsheet: { state: () => { const s = fakeState({battleActive:false, battleKind:0, phase:2}); s.planets[11].owner = s.yourSlot; s.v2.sell = [3, 0, 11, 6]; s.v2.heat = 1; return s; },
               action: () => setTimeout(() => openSellSheet(), 300) },
  sellfight: { state: () => { const s = fakeState({battleActive:false, battleKind:0, phase:2}); s.planets[5].owner = s.yourSlot; s.v2.sell = [2, 4, 5, 5]; s.v2.heat = 2; return s; },
               action: () => setTimeout(() => { RV.open.add('sell3_2_4_5'); openSellSheet(); }, 300) },
  titlesheet: { state: () => { const s = fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'}); s.v2.champ = [1, -1, -1, -1]; s.v2.champOffer = 2; return s; },
                action: () => setTimeout(() => openTitleSheet(), 300) },
  neghood: { state: () => { const s = HARNESS_MODES.negamb.state(); if (s.neg) s.neg.planet = 2; return s; }, action: () => {} },
  watchsheet: { state: () => { const s = fakeState({battleActive:false, battleKind:0, phase:2}); s.planets[11].chars[0].revealed = true;
                               s.talks = [{by:3, p:11, f:2, n:4, s:1, r:0, m:[1,0,0,0], q:'A Pyke enforcer was caught skimming. What should be done?', rep:['Make an example of him.', 'Quietly move him somewhere he can be useful.', 'Report it to the Hutts.'], w:-1, v:2, cw:1, cv:0, why:''}]; return s; },
                 action: () => setTimeout(() => openWatch(3), 300) },
  neggal: { state: () => { const s = HARNESS_MODES.negamb.state(); if (s.neg) { s.neg.gw = 3; s.neg.gv = 1; } return s; }, action: () => {} },
  spectate: { state: () => { const s = fakeState({battleActive:true, stage:1, yourSlot:-1}); s.battleAttackerSlot = 0; s.battleDefenderSlot = -1; s.battleKind = 1; s.battleTargetPlanet = 9;
                             s.talks = [{by:3, p:11, f:-1, n:4, s:1, r:0, m:[1,0,0,0], q:'?', rep:[], w:-1, v:0, cw:0, cv:0, why:''}]; return s; },
               action: () => { spectating = true; render(lastS); } },
  watchbattle: { state: () => { const s = fakeState({battleActive:true}); s.battleAttackerSlot = 0; s.battleDefenderSlot = -1; s.battleKind = 1; s.battleTargetPlanet = 9; s.planets[9].owner = s.yourSlot; return s; },
                 action: () => setTimeout(() => { watchBattle = battleKeyOf(lastS); shownHtml = null; render(lastS); }, 300) },
  attackalert: { state: () => { const s = fakeState({battleActive:true}); s.battleAttackerSlot = 0; s.battleDefenderSlot = -1; s.battleKind = 1; s.battleTargetPlanet = 9; s.planets[9].owner = s.yourSlot; return s; },
                 action: () => {} },
  gside: { state: () => { const s = fakeState({battleActive:false, battleKind:0, phase:0}); [2, 5, 9, 11].forEach(i => { s.planets[i].chars[0].revealed = true; s.planets[i].chars[0].value = 'Pyke Syndicate'; }); return s; },
           action: () => { gFaction = 2; gFilter = 4; openGalaxy(); } },
  gsidesheet: { state: () => fakeState({battleActive:false, battleKind:0, phase:0}), action: () => { openGalaxy(); setTimeout(() => openSideFilter(), 300); } },
  nowoff: { state: () => fakeState({battleActive:false, battleKind:0}), action: () => { NOW_OFF = true; shownHtml = null; poll(); } },
  movehome: { state: () => fakeState({battleActive:false, battleKind:0, phase:1, phaseName:'MOVEMENT'}), action: () => {} },
  heathome: { state: () => { const s = fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'}); s.v2.heat = 2; return s; }, action: () => {} },
  caughtsheet: { state: () => { const s = fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'}); s.v2.caught = [2, 3, 4, 9, 14]; return s; },
                 action: () => setTimeout(() => openCaughtSheet(), 300) },
  pushsheet: { state: () => { const s = fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'}); s.v2.heistPush = 1; return s; },
               action: () => setTimeout(() => openPushSheet(), 300) },
  worldprev: { state: () => { const s = fakeState({battleActive:false, battleKind:0}); return s; }, action: () => { openGalaxy(); setTimeout(() => openPlanet(5), 200); } },
  stallshut: {
    state: () => { const s = shelfGoods(fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'}), [4, 9, 0, 7]);
                   s.v2.stalls[1].shut = 1; return s; },
    action: () => openMarkets(),
  },
  settings: { state: () => fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'}), action: () => setTimeout(openSettings, 200) },
  settingsarm: { state: () => fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'}), action: () => setTimeout(async () => { await openSettings(); netMode(0); }, 200) },
  rivetoff: { state: () => { const s = fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'}); s.v2.job = {f:1, w:5, due:4, st:1, ann:0}; return s; },
              action: () => { SET.rivet = false; setTimeout(() => openJobSheet(), 300); } },
  stallrivet: {  // Rivet's bubble opens beneath the shut card, full width
    state: () => { const s = shelfGoods(fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'}), [4, 9, 0, 7]);
                   s.v2.stalls[0].shut = 1; return s; },
    action: () => { RV.open.add('shop' + 0 + '_3_' + tierIdx(lastS || fakeState({}), 0)); RV.open.add('shop0_3_0'); RV.open.add('shop0_3_1'); RV.open.add('shop0_3_2'); openMarkets(); },
  },
  // v0.27.3 ATTACK WHAT? (a planet or a player) in Resolution.
  attack: { state: () => fakeState({battleActive:false, battleKind:0}), action: () => openAttack() },
  // v0.21.18 join flow: seat picker (P1 Empire, P4 still choosing) and the
  // faction picker for P2 (Empire taken by P1).
  seat: {
    state: () => { const s = fakeState({battleActive:false, battleKind:0, stage:0, yourSlot:-1});
                   s.players[2] = {active:false,name:'',chosenFaction:-1,commander:-1,credits:0,vp:0,lockedIn:false,fleets:s.players[2].fleets};
                   s.players[1] = Object.assign({}, s.players[1], {active:false});
                   s.players[3] = Object.assign({}, s.players[3], {chosenFaction:-1, name:''});
                   return s; },
    action: () => {},
  },
  faction: {
    state: () => { const s = fakeState({battleActive:false, battleKind:0, stage:0});
                   s.players[1] = Object.assign({}, s.players[1], {chosenFaction:-1, name:''});
                   return s; },
    action: () => {},
  },
  // v0.21.19 Lobby (P2 = you, Rebels; P4 still choosing).
  lobby: {
    state: () => { const s = fakeState({battleActive:false, battleKind:0, stage:0});
                   s.players[2] = Object.assign({}, s.players[0], {chosenFaction:2, commander:9, name:'PYKE SYNDICATE'});
                   s.players[3] = Object.assign({}, s.players[3], {chosenFaction:-1, commander:-1, name:''});
                   return s; },
    action: () => {},
  },
  // 3.1-dev.6 character select with a pick (Kess) -- Empire / Dawn rows taken.
  commander: {
    state: () => { const s = fakeState({battleActive:false, battleKind:0, stage:0});
                   s.players[1] = Object.assign({}, s.players[1], {chosenFaction:-1, commander:-1, name:''});
                   return s; },
    action: () => { pickChar(3); },
  },
  charkit: {  // 3.1-dev.94: YOUR KIT from the character select
    state: () => { const s = fakeState({battleActive:false, battleKind:0, stage:0});
                   s.players[1] = Object.assign({}, s.players[1], {chosenFaction:-1, commander:-1, name:''});
                   return s; },
    action: () => { pickChar(3); setTimeout(() => openCharKit(3), 300); },
  },
  // 3.1-dev.6 a defector picked (Danto, ex-Empire): Old Contacts line.
  chardef: {
    state: () => { const s = fakeState({battleActive:false, battleKind:0, stage:0});
                   s.players[1] = Object.assign({}, s.players[1], {chosenFaction:-1, commander:-1, name:''});
                   s.players[0] = Object.assign({}, s.players[0], {active:false, chosenFaction:-1});
                   return s; },
    action: () => { pickChar(1); },
  },
  // 3.1-dev.6 your card (tap your face on Home): you are Kess.
  card: { state: () => fakeState({battleActive:false, battleKind:0, phase:0}), action: () => setTimeout(() => openCard(), 300) },
  // 3.1-dev.7: a class stamp notification (Scoundrel buys a good) + quest progress on the card.
  stamp: { state: () => { const s = fakeState({battleActive:false, battleKind:0, phase:0}); s.v2.qProg = 22; s.v2.qGoal = 30; return s; },
           action: () => setTimeout(() => { openCard(); notify('SCOUNDREL −1', 'Black-market deal: 1 Credit off.', '#8CE7A0', 60000, true); }, 300) },
  // v0.21.21 hologram transmissions. You are P2 = the Rebels with Ilsa, so a
  // Rebel transmission comes from Gen. Marr; the check-in is from the Pykes (Vosk).
  tx: { state: () => fakeState({battleActive:false, battleKind:0, transmissionPending:true, transmissionFaction:0, txFrom:2, txTo:3, txBig:false, txDelta:4,
                                 transmissionQuote:'You hit an Imperial depot on Endor for us. The Alliance won\x27t forget it.'}), action: () => {} },
  bmpage2: { state: () => { const s = fhState(7, 0, {lots:[0, 2], lotsGot:0, lotReady:0}); s.players[s.yourSlot].credits = 26; return s; }, action: () => setTimeout(() => openBlackMarket(), 300) },
  introgal: { state: () => { try { localStorage.setItem('sfSet_first', '1'); localStorage.setItem('sfIntro', JSON.stringify({home:1, _at:3})); } catch (e) {} const s = fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'}); s.round = 1;
                             const f = s.players[s.yourSlot].fleets[0]; f.alive = 4; f.total = 4; return s; }, action: () => setTimeout(() => rivetIntro(lastS), 300) },
  chkmin: { state: () => { try { localStorage.setItem('sfSet_first', '1'); localStorage.setItem('sfChkMin', '2'); localStorage.setItem('sfIntro', JSON.stringify({home:1, gal:1, world:1, move:1, res:1, def:1, _at:6})); } catch (e) {} const s = fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'}); s.round = 2; return s; }, action: () => {} },
  chkhome: { state: () => { try { localStorage.setItem('sfSet_first', '1'); localStorage.setItem('sfIntro', JSON.stringify({home:1, gal:1, world:1, move:1, res:1, def:1, _at:6})); localStorage.removeItem('sfChk_2'); } catch (e) {} const s = fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'}); s.round = 2; return s; },
             action: () => {} },
  introworld: { state: () => { try { localStorage.setItem('sfSet_first', '1'); localStorage.setItem('sfIntro', JSON.stringify({home:1, gal:1, _at:3})); } catch (e) {} const s = fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'}); s.round = 1; return s; },
                action: () => setTimeout(() => { openPlanet(4); setTimeout(() => rivetIntro(lastS), 300); }, 300) },
  // 3.1-dev.81 FIRST GAME: the toggle at the seat pick, and Rivet's first introduction on Home
  firstseat: { state: () => { try { localStorage.setItem('sfSet_first', '1'); } catch (e) {} const s = fakeState({stage:0}); s.yourSlot = -1; s.net = {home:false, canHome:true}; return s; }, action: () => { joined = false; mySlot = -1; lastSeatSnapshot = ''; render(lastS); } },
  introhome: { state: () => { try { localStorage.setItem('sfSet_first', '1'); localStorage.removeItem('sfIntro'); } catch (e) {} const s = fakeState({battleActive:false, battleKind:0, phase:0, phaseName:'PREPARE'}); s.round = 1; return s; },
               action: () => setTimeout(() => rivetIntro(lastS), 300) },
  // 3.1-dev.80 BATTLE REPLAY: History with battles, and one replaying
  histbattles: { state: () => fakeState({battleActive:false, battleKind:0, phase:2}), action: () => setTimeout(() => { openFeed('history');
      BATTLES = [{r:5,k:1,a:0,d:-1,p:4,a0:6,d0:2,res:2,rs:0,log:[0x34,0x26,0x52,0x15,0x63,0x24,0x46,0x13]}, {r:4,k:2,a:1,d:0,p:-1,a0:3,d0:5,res:3,rs:1,log:[0x42,0x35,0x24]}]; shownHtml = null; render(lastS); }, 300) },
  replay: { state: () => fakeState({battleActive:false, battleKind:0, phase:2}), action: () => setTimeout(() => { openFeed('history');
      BATTLES = [{r:5,k:1,a:0,d:-1,p:4,a0:6,d0:2,res:2,rs:0,log:[0x34,0x26,0x52,0x15,0x63,0x24,0x46,0x13]}]; openReplay(0); }, 300) },
  // 3.1-dev.79 a commander's world: FIGHT / UPRISING / BUYOUT; the holder's buyout sheet
  heldworld: { state: () => { const s = fakeState({battleActive:false, battleKind:0, phase:2, phaseName:'RESOLUTION'}); const o = s.players.findIndex((p, i) => p.active && i !== s.yourSlot);
                              const w = s.planets.findIndex(p => p.home < 0 && p.owner < 0); s.planets[w].owner = o; s.planets[w].pay = 24; s.__w = w; return s; },
               action: () => setTimeout(() => { const w = lastS.planets.findIndex(p => p.owner >= 0 && p.owner !== lastS.yourSlot && p.home < 0); openPlanet(w); }, 300) },
  buyoutsheet: { state: () => { const s = fakeState({battleActive:false, battleKind:0, phase:0}); const o = s.players.findIndex((p, i) => p.active && i !== s.yourSlot);
                                const w = s.planets.findIndex(p => p.home < 0 && p.owner < 0); s.planets[w].owner = s.yourSlot; s.planets[w].bo = [o, 24]; s.players[s.yourSlot].credits = 30; return s; },
                 action: () => setTimeout(() => openBuyout(lastS.planets.findIndex(p => p.bo)), 300) },
  // 3.1-dev.78 THEIR FILE (an Infiltrator reading a rival), BLACKMAIL (asked of you), the Scoundrel's bounty in Standings
  kitfile: { state: () => { const s = fakeState({battleActive:false, battleKind:0, phase:0}); const o = s.players.findIndex((p, i) => p.active && i !== s.yourSlot);
                            s.players[s.yourSlot].commander = 2; s.players[o].commander = 7;
                            s.v2.file = { s: o, sl: 1, cr: 14, d: [[2, 5, 6]], g: [1, 4], job: [0, 9], def: [[2, 4], [6, 3]] }; s.planets[2].owner = o; s.planets[6].owner = o; return s; },
             action: () => { standingsMode = true; poll(); setTimeout(() => openKit(lastS.v2.file.s), 400); } },
  bmask: { state: () => { const s = fakeState({battleActive:false, battleKind:0, phase:0}); s.v2.bmAsk = s.players.findIndex((p, i) => p.active && i !== s.yourSlot);
                          s.promises = [{f:2, p:5, due:6, chk:6, text:'Spice Contract'}]; return s; }, action: () => setTimeout(() => openBlackmailAnswer(), 300) },
  bounty: { state: () => { const s = fakeState({battleActive:false, battleKind:0, phase:0}); const o = s.players.findIndex((p, i) => p.active && i !== s.yourSlot); s.players[o].commander = 3; return s; },
            action: () => { standingsMode = true; poll(); } },
  // 3.1-dev.77 THE BLACK MARKET: the door on Home, the page
  bmhome: { state: () => fhState(7, 0, {lots:[0, 2], lotsGot:0, lotReady:0}), action: () => {} },
  bmpage: { state: () => { const s = fhState(7, 0, {lots:[1, 3], lotsGot:8, hold:[3, 11, -1]}); s.players[s.yourSlot].credits = 26; return s; }, action: () => setTimeout(() => openBlackMarket(), 300) },
  // 3.1-dev.76 DEFENSE: the PLANETS box on Home, and its sheet
  defhome: { state: () => defState(), action: () => {} },
  defsheet: { state: () => defState(), action: () => setTimeout(() => { openDefense(); defStep(1, 1); defStep(1, 1); defTogglePat(1); }, 300) },
  // 3.1-dev.75 THE KIT: yours (from the card), someone else's (from Standings)
  kitme: { state: () => fakeState({battleActive:false, battleKind:0, phase:0}), action: () => setTimeout(() => openKit(lastS.yourSlot), 300) },
  kitthem: { state: () => fakeState({battleActive:false, battleKind:0, phase:0}), action: () => { standingsMode = true; poll(); setTimeout(() => { const o = lastS.players.findIndex((p, i) => p.active && i !== lastS.yourSlot && p.commander >= 0); openKit(o); }, 400); } },
  // 3.1-dev.75: the incoming moment, and ONE transmission for two factions + a gift
  txin: { state: () => fakeState({battleActive:false, battleKind:0, transmissionPending:true, transmissionFaction:2, txFrom:2, txTo:3, txDelta:6, txCause:3,
                                  transmissionQuote:'Kijimi pays us now.'}), action: () => setTimeout(() => { clearTimeout(txIncomingTimer); }, 200) },
  txgif: { state: () => { const s = fakeState({battleActive:false, battleKind:0, transmissionPending:true, transmissionFaction:1, txFrom:2, txTo:3, txDelta:6, txCause:3, txPlanet:5,
                                  txAlso:[0, 2, 1, -6, 8], transmissionQuote:'Kijimi bends the knee. The Rebels will whine about it. Let them.'}); s.v2.gifts = 2; return s; },
           action: () => setTimeout(() => { txResolveIncoming(); txTap(); }, 300) },
  txduo: { state: () => { const s = fakeState({battleActive:false, battleKind:0, transmissionPending:true, transmissionFaction:2, txFrom:2, txTo:3, txDelta:6, txCause:3, txPlanet:5,
                                  txAlso:[3, 2, 1, -6, 8], transmissionQuote:'Kijimi pays its dues to us now. The Dawn will sulk. Let them.'}); s.v2.gifts = 4; return s; },
           action: () => setTimeout(() => { txResolveIncoming(); txTap(); }, 300) },
  txwhy: { state: () => { const s = fakeState({battleActive:false, battleKind:0, transmissionPending:true, transmissionFaction:2, txFrom:2, txTo:3, txDelta:6, txCause:3, txPlanet:5,
                                  txAlso:[3, 2, 1, -6, 8], transmissionQuote:'Kijimi pays its dues to us now. The Dawn will sulk. Let them.'}); s.v2.gifts = 4; return s; },
           action: () => setTimeout(() => { txResolveIncoming(); txTap(); txWhyTap(); }, 300) },
  // v0.23.0 big moment: gold PRIORITY title + tier line (an Empire seat line).
  txbig: { state: () => fakeState({battleActive:false, battleKind:0, transmissionPending:true, transmissionFaction:1, txFrom:2, txTo:0, txBig:true,
                                    transmissionQuote:'Coruscant, taken. The seat of the Empire, held by a warlord from the Outer Rim. The galaxy will never forget this day. Neither will we.'}), action: () => {} },
  txob: { state: () => fakeState({battleActive:false, battleKind:0, transmissionPending:true, transmissionFaction:2, transmissionIsObligation:true,
                                   transmissionQuote:'Three rounds ago you promised us a share of Kessel\x27s spice. Did you deliver?'}),
          action: () => { setTimeout(() => txTap(), 600); } },
  // Planet detail after committing this round (TAKE OVER replaced by a note).
  locked: {
    state: () => { const s = fakeState({battleActive:false, battleKind:0}); s.planets[1].locked = true; return s; },
    action: () => openPlanet(1),
  },
};
// 3.1-dev.3: a fake event log. [seq, kind, round, who, planet, other, num(, text)]
// kinds: 1 round, 2 take, 3 returned, 4 moment, 5 lost, 6 note, 7 promise
function feedFake() {
  const s = fakeState({battleActive:false, battleKind:0});
  s.ev = { g: 3, ps: 7, ms: 4,
    p: [[1,1,1,-1,-1,-1,0], [2,2,1,0,4,-1,3], [3,4,1,1,4,0,0], [4,1,2,-1,-1,-1,0], [5,2,2,1,2,-1,2], [6,2,2,3,6,1,1], [7,4,2,1,6,3,6]],
    m: [[1,6,1,-1,-1,-1,0,'Your diplomat returns from Bespin: all four secrets revealed.'], [2,5,2,3,6,-1,1], [3,7,2,0,2,-1,1], [4,6,2,-1,-1,-1,1,'The Empire sent you an offer. Open DEALS to answer.']] };
  return s;
}
function harnessState() {
  const m = HARNESS_MODES[HARNESS_MODE];
  return m ? m.state() : fakeState();
}
window.fetch = async (url) => ({ status:200, json: async () => url.startsWith('/state') ? harnessState() : url.startsWith('/art') ? window.HARNESS_ART : url.startsWith('/netinfo') ? {home:true, canHome:true, failed:false, ssid:'HomeWiFi', saved:['HomeWiFi', 'Garage AP']} : {ok:true} });
