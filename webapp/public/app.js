/* =======================================================================
   Cube Tournament — app.js  (logica della web app host)
   -----------------------------------------------------------------------
   Responsabilita':
     1) parlare col cubo via WebSocket (tramite il hub server.js);
     2) tenere il modello del torneo (bracket a eliminazione diretta);
     3) disegnare il tabellone e gestire il match "live" sul cubo.

   Tutta la logica del bracket vive QUI nel browser: il server fa solo
   da centralino. Il bracket parte con giocatori FINTI ("Player N");
   il match selezionato come "live" viene riempito dai tag NFC reali che
   il cubo invia con players_ready, e risolto dal match_end del cubo.
   I match finti si risolvono cliccando direttamente il giocatore.

   Lingua: tutte le stringhe a schermo passano da t(); il selettore con
   le due bandierine in alto a destra cambia EN<->IT a caldo.
   ======================================================================= */

'use strict';

/* ----------------------- costanti di layout ----------------------- */
const W = 200, CARD_BODY = 76, ACT_H = 34, COLGAP = 70, VGAP = 26, TOPPAD = 12, LEFTPAD = 6;
const PITCH0 = CARD_BODY + ACT_H + VGAP;          // passo verticale del 1° turno
const STORE_KEY  = 'cubeTournament.v05';
const STORE_LANG = 'cubeTournament.lang';

/* --------------------------- stato globale ------------------------ */
let tour = null;          // modello del torneo (vedi createTournament)
let ws = null;
let cubeOnline = false;
let lastStatus = { online: false };   // ultimo cube_status (per ri-tradurre l'header)
let lang = 'en';

/* ============================ I18N =============================== */
const I18N = {
  en: {
    docTitle: 'Cube Tournament — Host',
    subtitle: 'Host console · controller v0.5',
    beepTitle: 'Make the cube beep',
    newTournament: 'New tournament',
    participants: 'Participants',
    game: 'Game',
    gameChess: 'Chess',
    timeLimit: 'Time limit per match',
    chessFormat: 'Chess format',
    fmtClassical: 'Classical — 90′/40 +30′',
    fmtRapid: 'Rapid — custom time',
    fmtBlitz: 'Blitz — 3′ + 2s',
    rapidMinutes: 'Rapid minutes (per player)',
    createBracket: 'Create bracket',
    endTournament: 'End tournament',
    matchOnCube: 'Match on the cube',
    startOnCube: 'Start on cube',
    events: 'Events',
    bracket: 'Bracket',
    playerSource: 'player source',
    // dynamic
    cubeOnline: 'Cube online',
    cubeOffline: 'Cube offline',
    waitingConnection: 'waiting for connection…',
    table: 'table',
    waiting: 'Waiting…',
    player: 'Player',
    champion: 'Champion',
    toBeDecided: 'to be decided',
    emptyBracket: 'Create a tournament to draw the bracket.',
    liveNoMatch: 'No match selected. Press “▶ cube” on a match.',
    liveTapTags: 'Tap the two NFC tags on the cube…',
    liveReady: 'Ready. Press “Start on cube” to begin the match.',
    goCube: '▶ cube',
    goTitle: 'Send this match to the cube (asks for the NFC tags)',
    slotWinnerTitle: 'Click to declare the winner',
    chess: 'chess',
    max: 'max',
    summaryPlayers: '{0} players · {1}',
    logHubConnected: 'connected to hub',
    logHubDisconnected: 'hub disconnected, retrying…',
    logCube: 'cube "{0}" (table {1}, {2})',
    logCubeMode: 'cube is in {0} mode',
    logTagSlot: 'tag on slot {0}: {1}',
    logMatchStarted: 'match started on cube ({0})',
    logNoTournament: 'players_ready but no tournament created',
    logPlayersReady: 'players ready: {0} vs {1}',
    logNoMatchAvail: 'players_ready but no match available',
    logNoLiveMatch: 'match_end with no live match',
    logMatchEnd: 'match_end: {0} wins',
    logChampion: '🏆 champion: {0}',
    logNextMatch: '→ next_match (match {0})',
    logMatchConfig: '→ match_config sent to the cube',
    logBeep: '→ beep',
    logNewTournament: 'new tournament: {0} players, {1}',
    logTournamentEnd: '→ tournament_end',
    logNotConnected: 'cube/hub not connected: {0}',
    confirmNewBracket: 'Create a new bracket? The current one will be lost.',
    confirmEndTournament: 'End the tournament and clear the bracket?',
  },
  it: {
    docTitle: 'Cube Tournament — Host',
    subtitle: 'Console host · controller v0.5',
    beepTitle: 'Fai suonare il cubo',
    newTournament: 'Nuovo torneo',
    participants: 'Partecipanti',
    game: 'Gioco',
    gameChess: 'Scacchi',
    timeLimit: 'Tempo limite per partita',
    chessFormat: 'Formato scacchi',
    fmtClassical: 'Classical — 90′/40 +30′',
    fmtRapid: 'Rapid — tempo a scelta',
    fmtBlitz: 'Blitz — 3′ + 2s',
    rapidMinutes: 'Minuti Rapid (per giocatore)',
    createBracket: 'Crea tabellone',
    endTournament: 'Termina torneo',
    matchOnCube: 'Match sul cubo',
    startOnCube: 'Avvia sul cubo',
    events: 'Eventi',
    bracket: 'Tabellone',
    playerSource: 'origine giocatore',
    // dynamic
    cubeOnline: 'Cubo online',
    cubeOffline: 'Cubo offline',
    waitingConnection: 'in attesa di connessione…',
    table: 'tavolo',
    waiting: 'In attesa…',
    player: 'Giocatore',
    champion: 'Campione',
    toBeDecided: 'da assegnare',
    emptyBracket: 'Crea un torneo per disegnare il tabellone.',
    liveNoMatch: 'Nessun match selezionato. Premi “▶ cubo” su un match.',
    liveTapTags: 'Far tappare i due tag NFC sul cubo…',
    liveReady: 'Pronti. Premi “Avvia sul cubo” per far partire la partita.',
    goCube: '▶ cubo',
    goTitle: 'Manda questo match sul cubo (chiede i tag NFC)',
    slotWinnerTitle: 'Clicca per dichiarare vincitore',
    chess: 'scacchi',
    max: 'max',
    summaryPlayers: '{0} giocatori · {1}',
    logHubConnected: 'connesso al hub',
    logHubDisconnected: 'hub disconnesso, riprovo…',
    logCube: 'cubo "{0}" (tavolo {1}, {2})',
    logCubeMode: 'il cubo e\' in modo {0}',
    logTagSlot: 'tag su slot {0}: {1}',
    logMatchStarted: 'match avviato sul cubo ({0})',
    logNoTournament: 'players_ready ma nessun torneo creato',
    logPlayersReady: 'giocatori pronti: {0} vs {1}',
    logNoMatchAvail: 'players_ready ma nessun match disponibile',
    logNoLiveMatch: 'match_end senza match live',
    logMatchEnd: 'match_end: vince {0}',
    logChampion: '🏆 campione: {0}',
    logNextMatch: '→ next_match (match {0})',
    logMatchConfig: '→ match_config inviato al cubo',
    logBeep: '→ beep',
    logNewTournament: 'nuovo torneo: {0} giocatori, {1}',
    logTournamentEnd: '→ tournament_end',
    logNotConnected: 'cubo/hub non connesso: {0}',
    confirmNewBracket: 'Creare un nuovo tabellone? Quello attuale verra\' perso.',
    confirmEndTournament: 'Terminare il torneo e azzerare il tabellone?',
  },
};

function t(key, ...args) {
  let s = (I18N[lang] && I18N[lang][key] != null) ? I18N[lang][key] : (I18N.en[key] != null ? I18N.en[key] : key);
  args.forEach((a, i) => { s = s.replace(new RegExp('\\{' + i + '\\}', 'g'), a); });
  return s;
}

// Nome a schermo di un giocatore: i finti seguono la lingua (Player/Giocatore),
// gli slot vuoti diventano "Waiting…", i tag NFC mostrano il nome reale.
function displayName(p) {
  if (!p || !p.src) return t('waiting');
  if (p.src === 'fake' && p.seed != null) return `${t('player')} ${p.seed}`;
  return p.name;
}

const GAME_LABEL = { pokemon: 'Pokémon', yugioh: 'Yu-Gi-Oh!', scacchi: null };
function gameLabel(id) { return id === 'scacchi' ? t('chess') : (GAME_LABEL[id] || id); }

/* applica le stringhe statiche (elementi con data-i18n / data-i18n-title) */
function applyStaticI18n() {
  document.documentElement.lang = lang;
  document.title = t('docTitle');
  document.querySelectorAll('[data-i18n]').forEach(el => { el.textContent = t(el.dataset.i18n); });
  document.querySelectorAll('[data-i18n-title]').forEach(el => { el.title = t(el.dataset.i18nTitle); });
}
function updateLangToggle() {
  document.querySelectorAll('#langToggle .flag').forEach(f => {
    f.classList.toggle('active', f.dataset.lang === lang);
  });
}
function setLang(l) {
  if (l !== 'en' && l !== 'it') return;
  lang = l;
  try { localStorage.setItem(STORE_LANG, l); } catch (e) {}
  applyStaticI18n();
  updateLangToggle();
  setCubeStatus(lastStatus);     // ri-traduce label/meta dell'header
  if (tour) render();            // ri-traduce tabellone, campione, tag
  updateLivePanel();             // ri-traduce hint del match live
}

/* =========================== MODELLO ============================== */
function createTournament(size, game, timeLimit, chessFormat, rapidMin) {
  const rounds = Math.log2(size);
  const matches = [];
  for (let r = 0; r < rounds; r++) {
    const count = size >> (r + 1);
    for (let i = 0; i < count; i++) {
      let p1 = null, p2 = null;
      if (r === 0) {                              // 1° turno: segnaposto finti
        p1 = { name: `Player ${i * 2 + 1}`, seed: i * 2 + 1, uid: null, src: 'fake' };
        p2 = { name: `Player ${i * 2 + 2}`, seed: i * 2 + 2, uid: null, src: 'fake' };
      }
      matches.push({ id: `${r}-${i}`, round: r, idx: i, p1, p2, winner: null });
    }
  }
  return { size, game, timeLimit, chessFormat, rapidMin, matches, liveId: null, createdAt: Date.now() };
}

const getMatch = (id) => tour.matches.find(m => m.id === id);
const rounds   = () => Math.log2(tour.size);
const finalMatch = () => getMatch(`${rounds() - 1}-0`);

// Ricostruisce gli accoppiamenti dei turni successivi dai vincitori.
// Idempotente: si puo' chiamare dopo qualsiasi modifica.
function recompute() {
  const R = rounds();
  // fotografo chi c'era negli slot prima di ricalcolare
  const snap = {};
  for (const m of tour.matches) if (m.round > 0) {
    snap[m.id] = { p1: m.p1, p2: m.p2 };
    m.p1 = null; m.p2 = null;
  }
  for (let r = 0; r < R - 1; r++) {
    for (const m of tour.matches.filter(x => x.round === r)) {
      if (!m.winner) continue;
      const win = (m.winner === 1) ? m.p1 : m.p2;
      const parent = getMatch(`${r + 1}-${Math.floor(m.idx / 2)}`);
      const slotNum = (m.idx % 2 === 0) ? 1 : 2;
      const slotKey = slotNum === 1 ? 'p1' : 'p2';
      const before = snap[parent.id] ? snap[parent.id][slotKey] : null;
      parent[slotKey] = win ? { ...win } : null;
      // se cambia chi avanza nello slot che aveva gia' vinto, il
      // risultato del parent non vale piu': lo azzeriamo.
      const changed = (before && before.name) !== (win && win.name)
                   || (before && before.uid) !== (win && win.uid);
      if (changed && parent.winner === slotNum) parent.winner = null;
    }
  }
  // un vincitore non e' piu' valido se il match ha perso un giocatore
  for (const m of tour.matches) if (m.winner && (!m.p1 || !m.p2)) m.winner = null;
}

function setWinner(m, w) {
  if (!m.p1 || !m.p2) return;
  m.winner = w;
  recompute(); persist(); render();
}

function champion() {
  const f = finalMatch();
  if (!f || !f.winner) return null;
  return f.winner === 1 ? f.p1 : f.p2;
}

/* ===================== PERSISTENZA (localStorage) ================= */
function persist() {
  try { localStorage.setItem(STORE_KEY, JSON.stringify(tour)); } catch (e) {}
}
function restore() {
  try {
    const raw = localStorage.getItem(STORE_KEY);
    if (raw) { tour = JSON.parse(raw); return true; }
  } catch (e) {}
  return false;
}

/* =========================== WEBSOCKET ============================ */
function wsConnect() {
  const url = `ws://${location.host}/`;
  ws = new WebSocket(url);
  ws.onopen = () => { send({ type: 'hello', role: 'host' }); log(t('logHubConnected'), 'out'); };
  ws.onclose = () => { log(t('logHubDisconnected')); setTimeout(wsConnect, 1500); };
  ws.onerror = () => {};
  ws.onmessage = (ev) => {
    let msg; try { msg = JSON.parse(ev.data); } catch { return; }
    handleCubeMessage(msg);
  };
}
function send(obj) {
  if (ws && ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(obj));
  else log(t('logNotConnected', obj.type));
}

function handleCubeMessage(msg) {
  switch (msg.type) {
    case 'cube_status': setCubeStatus(msg); break;
    case 'hello':       setCubeStatus({ online: true, ...msg }); log(t('logCube', msg.cube, msg.table, msg.mode), 'in'); break;
    case 'heartbeat':   updateRssi(msg.rssi); break;
    case 'mode':        log(t('logCubeMode', msg.mode), 'in'); break;
    case 'player_tag':  log(t('logTagSlot', msg.slot, msg.name || msg.uid), 'in'); break;
    case 'players_ready': onPlayersReady(msg); break;
    case 'match_start': log(t('logMatchStarted', msg.game), 'in'); break;
    case 'match_end':   onMatchEnd(msg); break;
    default: break;
  }
}

/* il cubo, appena entra in torneo, raccoglie i tag e manda players_ready
   anche senza che noi glielo chiediamo: gestiamo entrambi i casi. */
function onPlayersReady(msg) {
  if (!tour) { log(t('logNoTournament'), 'in'); return; }
  let target = tour.liveId ? getMatch(tour.liveId) : null;
  if (!target) {
    target = tour.matches.find(m => m.round === 0 && !m.winner && m.p1 && m.p1.src === 'fake' && m.p2 && m.p2.src === 'fake')
          || tour.matches.find(m => m.round === 0 && !m.winner);
    if (target) tour.liveId = target.id;
  }
  if (!target) { log(t('logNoMatchAvail'), 'in'); return; }
  target.p1 = nfcPlayer(msg.p1);
  target.p2 = nfcPlayer(msg.p2);
  log(t('logPlayersReady', displayName(target.p1), displayName(target.p2)), 'in');
  recompute(); persist(); render(); updateLivePanel();
}

function onMatchEnd(msg) {
  if (!tour || !tour.liveId) { log(t('logNoLiveMatch'), 'in'); return; }
  const m = getMatch(tour.liveId);
  const win = (msg.winner === 1) ? m.p1 : m.p2;
  m.winner = msg.winner;
  tour.liveId = null;
  recompute(); persist(); render(); updateLivePanel();
  log(t('logMatchEnd', win ? displayName(win) : 'P' + msg.winner), 'in');
  const champ = champion();
  if (champ) log(t('logChampion', displayName(champ)), 'in');
}

function nfcPlayer(obj) {
  if (!obj) return { name: '', uid: null, src: null };
  const name = obj.name && obj.name.length ? obj.name : ('Tag ' + String(obj.uid || '').slice(-5));
  return { name, uid: obj.uid || null, src: 'nfc' };
}

/* ===================== STATO DEL CUBO (header) =================== */
function setCubeStatus(s) {
  lastStatus = s || { online: false };
  cubeOnline = !!s.online;
  const box = document.getElementById('cubeState');
  box.dataset.online = cubeOnline ? 'true' : 'false';
  document.getElementById('cubeStateLabel').textContent = cubeOnline ? t('cubeOnline') : t('cubeOffline');
  const meta = cubeOnline
    ? [s.cube, s.table != null ? t('table') + ' ' + s.table : null, s.mode].filter(Boolean).join(' · ')
    : t('waitingConnection');
  document.getElementById('cubeStateMeta').textContent = meta || '—';
}
function updateRssi(rssi) {
  if (!cubeOnline) return;
  const el = document.getElementById('cubeStateMeta');
  const base = el.textContent.split('  ·  RSSI')[0];
  el.textContent = `${base}  ·  RSSI ${rssi} dBm`;
}

/* ============================== LOG ============================== */
function log(text, dir) {
  const ul = document.getElementById('log');
  const li = document.createElement('li');
  if (dir) li.className = dir;
  const ts = new Date().toLocaleTimeString(lang === 'it' ? 'it-IT' : 'en-GB', { hour12: false });
  li.innerHTML = `<b>${ts}</b> ${esc(text)}`;
  ul.prepend(li);
  while (ul.children.length > 60) ul.removeChild(ul.lastChild);
}

/* ====================== RENDER DEL TABELLONE ===================== */
function render() {
  const host = document.getElementById('bracket');
  host.innerHTML = '';
  if (!tour) {
    host.innerHTML = `<div class="empty-bracket" id="emptyBracket">${esc(t('emptyBracket'))}</div>`;
    document.getElementById('bracketTag').textContent = '—';
    return;
  }

  const R = rounds();
  const count0 = tour.size / 2;
  const cy = {};
  const centerY = (m) => {
    if (cy[m.id] != null) return cy[m.id];
    let y;
    if (m.round === 0) y = TOPPAD + CARD_BODY / 2 + m.idx * PITCH0;
    else {
      const c1 = getMatch(`${m.round - 1}-${m.idx * 2}`);
      const c2 = getMatch(`${m.round - 1}-${m.idx * 2 + 1}`);
      y = (centerY(c1) + centerY(c2)) / 2;
    }
    return (cy[m.id] = y);
  };
  const leftX = (r) => LEFTPAD + r * (W + COLGAP);

  const totalW = leftX(R) + W + LEFTPAD;           // +1 colonna per il campione
  const totalH = TOPPAD * 2 + (count0 - 1) * PITCH0 + CARD_BODY + ACT_H;
  host.style.width = totalW + 'px';
  host.style.height = totalH + 'px';

  // ---- livello connettori (SVG, dietro le card) ----
  const svgNS = 'http://www.w3.org/2000/svg';
  const svg = document.createElementNS(svgNS, 'svg');
  svg.setAttribute('class', 'links');
  svg.setAttribute('width', totalW);
  svg.setAttribute('height', totalH);
  for (const m of tour.matches) {
    const x1 = leftX(m.round) + W, y1 = centerY(m);
    let x2, y2;
    if (m.round < R - 1) {
      const parent = getMatch(`${m.round + 1}-${Math.floor(m.idx / 2)}`);
      x2 = leftX(parent.round); y2 = centerY(parent);
    } else {                                        // finale -> campione
      x2 = leftX(R); y2 = centerY(m);
    }
    const mid = (x1 + x2) / 2;
    const p = document.createElementNS(svgNS, 'path');
    p.setAttribute('d', `M${x1},${y1} H${mid} V${y2} H${x2}`);
    if (m.winner) p.setAttribute('class', 'done');
    svg.appendChild(p);
  }
  host.appendChild(svg);

  // ---- card dei match ----
  for (const m of tour.matches) {
    host.appendChild(buildMatchCard(m, centerY(m), leftX(m.round)));
  }

  // ---- box campione ----
  const champ = champion();
  const champEl = document.createElement('div');
  champEl.className = 'champion';
  champEl.style.left = leftX(R) + 'px';
  champEl.style.top = (centerY(finalMatch()) - 40) + 'px';
  champEl.innerHTML = `<div class="lbl">${esc(t('champion'))}</div>
    <div class="who ${champ ? '' : 'tbd'}">${champ ? esc(displayName(champ)) : esc(t('toBeDecided'))}</div>`;
  host.appendChild(champEl);

  // tag riassuntivo
  const fmt = tour.game === 'scacchi'
    ? `${t('chess')} · ${tour.chessFormat}${tour.chessFormat === 'RAPID' ? ' ' + tour.rapidMin + "'" : ''}`
    : `${gameLabel(tour.game)} · ${tour.timeLimit}' ${t('max')}`;
  document.getElementById('bracketTag').textContent = t('summaryPlayers', tour.size, fmt);
}

function buildMatchCard(m, cyVal, x) {
  const ready = m.p1 && m.p2 && m.p1.src && m.p2.src;   // entrambi noti
  const card = document.createElement('div');
  card.className = 'match' + (m.id === tour.liveId ? ' live' : '') + (m.winner ? ' done' : '');
  card.style.left = x + 'px';
  card.style.top = (cyVal - CARD_BODY / 2) + 'px';

  card.appendChild(buildSlot(m, 1));
  card.appendChild(buildSlot(m, 2));

  // riga azioni: solo se il match e' giocabile e non ancora risolto
  if (ready && !m.winner) {
    const act = document.createElement('div');
    act.className = 'mact';
    const go = document.createElement('button');
    go.className = 'go';
    go.textContent = t('goCube');
    go.title = t('goTitle');
    go.onclick = () => sendToCube(m.id);
    act.appendChild(go);
    card.appendChild(act);
  }
  return card;
}

function buildSlot(m, slot) {
  const p = slot === 1 ? m.p1 : m.p2;
  const el = document.createElement('div');
  const known = p && p.src;
  el.className = 'slot' + (known ? '' : ' empty')
    + (m.winner === slot ? ' winner' : '')
    + (m.winner && m.winner !== slot ? ' loser' : '');
  if (p && p.src) el.dataset.src = p.src;
  el.innerHTML = `<span class="pdot"></span>
                  <span class="pname">${esc(displayName(p))}</span>`;
  // click su un giocatore = dichiaralo vincitore (utile per i match finti)
  if (m.p1 && m.p2 && m.p1.src && m.p2.src) {
    el.onclick = () => setWinner(m, slot);
    el.title = t('slotWinnerTitle');
  }
  return el;
}

const esc = (s) => String(s).replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));

/* ===================== PANNELLO MATCH LIVE ====================== */
function updateLivePanel() {
  const panel = document.getElementById('livePanel');
  if (!tour) { panel.hidden = true; return; }
  panel.hidden = false;
  const m = tour.liveId ? getMatch(tour.liveId) : null;
  const slots = panel.querySelectorAll('.live-player');
  const fill = (el, p) => {
    el.querySelector('.name').textContent = p && p.src ? displayName(p) : '—';
    el.querySelector('.uid').textContent = p && p.uid ? p.uid : '';
    el.querySelector('.src-dot').dataset.src = p && p.src ? p.src : '';
  };
  fill(slots[0], m && m.p1);
  fill(slots[1], m && m.p2);

  const both = m && m.p1 && m.p2 && m.p1.src && m.p2.src;
  const hint = document.getElementById('liveHint');
  if (!m) hint.textContent = t('liveNoMatch');
  else if (!both) hint.textContent = t('liveTapTags');
  else hint.textContent = t('liveReady');

  document.getElementById('btnStartCube').disabled = !(both && cubeOnline);
}

/* ===================== COMANDI VERSO IL CUBO ==================== */
function sendToCube(matchId) {
  const m = getMatch(matchId);
  if (!m || m.winner) return;
  tour.liveId = matchId;
  send({ type: 'next_match' });            // il cubo torna a raccogliere i tag
  log(t('logNextMatch', matchId), 'out');
  persist(); render(); updateLivePanel();
}

function matchConfig() {
  const cfg = { type: 'match_config', game: tour.game };
  if (tour.game === 'scacchi') {
    cfg.format = tour.chessFormat;
    cfg.time_limit_min = tour.chessFormat === 'RAPID' ? tour.rapidMin : 0;
  } else {
    cfg.time_limit_min = tour.timeLimit;
  }
  return cfg;
}

/* ============================ SETUP UI ========================== */
const ui = {};
function bindUI() {
  ui.segSize   = document.getElementById('segSize');
  ui.segGame   = document.getElementById('segGame');
  ui.timeLimit = document.getElementById('timeLimit');
  ui.timeOut   = document.getElementById('timeLimitOut');
  ui.chessFmtField = document.getElementById('chessFmtField');
  ui.chessFormat   = document.getElementById('chessFormat');
  ui.rapidMinField = document.getElementById('rapidMinField');
  ui.rapidMin  = document.getElementById('rapidMin');
  ui.rapidOut  = document.getElementById('rapidMinOut');

  // segmenti (size / game)
  ui.segSize.addEventListener('click', e => selectSeg(ui.segSize, e));
  ui.segGame.addEventListener('click', e => { if (selectSeg(ui.segGame, e)) applyGameUI(); });

  ui.timeLimit.addEventListener('input', () => ui.timeOut.textContent = ui.timeLimit.value + '′');
  ui.rapidMin.addEventListener('input', () => ui.rapidOut.textContent = ui.rapidMin.value + '′');
  ui.chessFormat.addEventListener('change', applyGameUI);

  document.getElementById('btnCreate').onclick = onCreate;
  document.getElementById('btnReset').onclick  = onResetTournament;
  document.getElementById('btnStartCube').onclick = () => {
    if (!tour || !tour.liveId) return;
    send(matchConfig());
    log(t('logMatchConfig'), 'out');
  };
  document.getElementById('btnBeep').onclick = () => { send({ type: 'beep' }); log(t('logBeep'), 'out'); };

  // selettore lingua: click su una bandierina = quella lingua
  document.querySelectorAll('#langToggle .flag').forEach(f => {
    f.addEventListener('click', () => setLang(f.dataset.lang));
  });

  applyGameUI();
}

function selectSeg(group, e) {
  const btn = e.target.closest('button');
  if (!btn) return false;
  group.querySelectorAll('button').forEach(b => b.classList.remove('active'));
  btn.classList.add('active');
  return true;
}
function segValue(group, attr) {
  const b = group.querySelector('button.active');
  return b ? b.dataset[attr] : null;
}

function applyGameUI() {
  const game = segValue(ui.segGame, 'game');
  document.body.dataset.game = game;
  const isChess = game === 'scacchi';
  ui.chessFmtField.hidden = !isChess;
  ui.rapidMinField.hidden = !(isChess && ui.chessFormat.value === 'RAPID');
}

function onCreate() {
  if (tour && !champion() && !confirm(t('confirmNewBracket'))) return;
  const size = parseInt(segValue(ui.segSize, 'size'), 10);
  const game = segValue(ui.segGame, 'game');
  const timeLimit = parseInt(ui.timeLimit.value, 10);
  const chessFormat = ui.chessFormat.value;
  const rapidMin = parseInt(ui.rapidMin.value, 10);
  tour = createTournament(size, game, timeLimit, chessFormat, rapidMin);
  persist();
  document.getElementById('btnReset').hidden = false;
  render(); updateLivePanel();
  log(t('logNewTournament', size, gameLabel(game)), 'out');
}

function onResetTournament() {
  if (!confirm(t('confirmEndTournament'))) return;
  send({ type: 'tournament_end' });
  log(t('logTournamentEnd'), 'out');
  tour = null;
  try { localStorage.removeItem(STORE_KEY); } catch (e) {}
  document.getElementById('btnReset').hidden = true;
  render(); updateLivePanel();
}

/* ============================== BOOT ============================ */
function restoreLang() {
  try { const l = localStorage.getItem(STORE_LANG); if (l === 'en' || l === 'it') return l; } catch (e) {}
  return 'en';   // default: inglese
}

function boot() {
  bindUI();
  lang = restoreLang();
  if (restore() && tour) {
    document.getElementById('btnReset').hidden = false;
    syncControlsFromTour();
  }
  applyStaticI18n();
  updateLangToggle();
  setCubeStatus(lastStatus);     // header in lingua, stato offline
  render();
  updateLivePanel();
  wsConnect();
}

function syncControlsFromTour() {
  const setSeg = (group, attr, val) => group.querySelectorAll('button').forEach(b => {
    b.classList.toggle('active', b.dataset[attr] === String(val));
  });
  setSeg(ui.segSize, 'size', tour.size);
  setSeg(ui.segGame, 'game', tour.game);
  ui.timeLimit.value = tour.timeLimit; ui.timeOut.textContent = tour.timeLimit + '′';
  ui.chessFormat.value = tour.chessFormat || 'BLITZ';
  ui.rapidMin.value = tour.rapidMin || 15; ui.rapidOut.textContent = (tour.rapidMin || 15) + '′';
  applyGameUI();
}

document.addEventListener('DOMContentLoaded', boot);
