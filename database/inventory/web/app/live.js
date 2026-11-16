/* QRProtec - front web : suivi des vérifs en direct (WebSocket) et vérifs passees. */
'use strict';

// Suivi des vérifs en direct : la page de scan publie sa vérif en cours (WebSocket /ws/live/scanner), les
// pages de suivi la recoivent en direct (/ws/live/watch, badge exige). Les vérifs passees viennent de l'API.

function socketUrl(path) {
  const url = new URL(path, document.baseURI);
  url.protocol = location.protocol === 'https:' ? 'wss:' : 'ws:';
  return url;
}

const PING_MS = 15000;

// Publication de la vérif en cours (page de scan). La session survit a un rechargement de l'onglet.
// Vérif à plusieurs : la session peut avoir d'autres participants (team), dont les scans s'ajoutent aux notres.
const live = (() => {
  const STORAGE_LIVE = 'qrprotec.live';
  let session = null; // {id, pid, token} ; join : vérif a rejoindre a la prochaine connexion
  try { session = JSON.parse(sessionStorage.getItem(STORAGE_LIVE) || 'null'); } catch (e) { session = null; }
  if (session && !session.pid) session = null; // ancien format
  let team = null;    // {participants, lots, keyed, entries, nearby} : dernier etat de la vérif commune
  let socket = null;
  let ready = false;
  let wanted = false;
  let retry = 0;
  let ping = 0;
  let timer = 0;
  let pending = null;
  let sent = '';
  let join = '';
  let joined = !!(session && session.joined); // vérif rejointe : connexion gardee meme sans scan de notre part
  let solo = false;   // « Quitter » : la nouvelle vérif n'est pas fusionnee avec celle du meme lot
  let answer = null;  // validation en attente de la reponse du serveur

  function store() {
    try {
      if (session) sessionStorage.setItem(STORAGE_LIVE, JSON.stringify(session));
      else sessionStorage.removeItem(STORAGE_LIVE);
    } catch (e) { /* stockage indisponible */ }
  }

  const who = () => (state.user ? { user: credentials() }
    : declared() ? { name: state.declaredName } : state.guest !== null ? { guest: true, name: state.guest } : {});

  function send(message) {
    if (socket && socket.readyState === WebSocket.OPEN) socket.send(typeof message === 'string' ? message : JSON.stringify(message));
  }

  function connect() {
    if (socket || !('WebSocket' in window)) return;
    const current = new WebSocket(socketUrl('ws/live/scanner'));
    socket = current;
    current.onopen = () => {
      send({ type: 'hello', ...(session || {}), ...(join ? { join } : {}), ...(solo && !session ? { solo } : {}), ...who() });
      clearInterval(ping);
      ping = setInterval(() => send({ type: 'ping' }), PING_MS);
    };
    current.onmessage = (event) => {
      let message;
      try { message = JSON.parse(event.data); } catch (e) { return; }
      if (message.type === 'hello') {
        if (!session || session.id !== message.id) { team = null; if (!join && !message.merged) joined = false; } // vérif expiree
        // une vérif etait deja en cours sur le meme lot global : les deux sont fusionnees en une vérif commune
        if (message.merged) {
          joined = true;
          feedback.info();
          showInfo('lot', `👥 Vérif commune avec ${message.who || 'un autre secouriste'}`,
            'Une vérif était déjà en cours sur ce lot : vos scans s\'y ajoutent automatiquement.');
        }
        solo = false;
        session = { id: message.id, pid: message.pid, token: message.token };
        if (join) joined = true;
        join = '';
        session.joined = joined;
        store();
        ready = true;
        retry = 0;
        sent = '';
        flush();
      } else if (message.type === 'error') {
        if (message.join) { join = ''; toast(message.message, true); }
      } else if (message.type === 'team') {
        team = message;
        teamChanged();
      } else if (message.type === 'validate') {
        if (answer) { const resolve = answer; answer = null; resolve(message); }
      } else if (message.type === 'submit') {
        validate(message.partial, true); // les autres sont partis : on envoie la vérif commune
      } else if (message.type === 'report') {
        teamReport(message.report);
      } else if (message.type === 'step') {
        teamStep(message);
      }
    };
    current.onclose = () => {
      if (socket !== current) return;
      clearInterval(ping);
      socket = null;
      ready = false;
      if (answer) { const resolve = answer; answer = null; resolve(null); }
      if (wanted) setTimeout(connect, Math.min(30000, 1000 * 2 ** retry++));
    };
  }

  function flush() {
    if (!ready) return;
    const text = JSON.stringify(pending ? { type: 'state', ...pending } : { type: 'cancel' });
    if (text === sent) return;
    sent = text;
    send(text);
    if (!pending && !inTeam()) stop();
  }

  function stop() {
    wanted = false;
    if (socket) { const current = socket; socket = null; ready = false; clearInterval(ping); current.close(); }
  }

  function restart() {
    if (socket) { const current = socket; socket = null; ready = false; clearInterval(ping); current.close(); }
    wanted = true;
    connect();
  }

  const others = () => (team ? team.participants.filter((row) => !row.me) : []);
  const inTeam = () => !!join || joined || others().length > 0;

  return {
    // snapshot : etat de la vérif en cours, null s'il n'y en a pas
    publish(snapshot) {
      pending = snapshot;
      if (snapshot || joined) { wanted = true; connect(); }
      clearTimeout(timer);
      timer = setTimeout(flush, 300);
    },
    // vérif validee : affichee comme terminee chez ceux qui suivent, la suivante sera une nouvelle session
    done(report) {
      if (ready && report) {
        send({ type: 'done', complete: !!report.complete, partial: !!report.partial, present: (report.present || []).length,
          verif: report.verif_id });
      }
      pending = null;
      session = null;
      team = null;
      joined = false;
      store();
      stop();
    },
    // vérif en cascade terminee de notre cote : on quitte la vérif commune, les autres la continuent
    quit() {
      if (ready) send({ type: 'leave' });
      pending = null;
      session = null;
      team = null;
      joined = false;
      store();
      stop();
    },
    identity() { if (ready) send({ type: 'identity', ...who() }); },
    // « Rendre la vérif complète » (admins) : partage avec tous les participants. false : pas de vérif commune
    // publiee, le reglage reste sur ce telephone
    setFull(on) {
      if (!ready || !session || !state.user) return false;
      send({ type: 'full', on: !!on, user: credentials() });
      return true;
    },
    // rejoindre la vérif `id` (page de suivi, ou autre vérif du meme lot vue depuis le scanner)
    join(id) {
      join = id;
      team = null;
      restart();
    },
    // quitter la vérif commune : la suite se fait seul, sur une nouvelle session
    leave() {
      session = null;
      team = null;
      join = '';
      joined = false;
      solo = true;
      store();
      sent = '';
      if (pending) restart(); else stop();
    },
    // valider sa partie : le serveur dit si l'on est le dernier (submit) ; sans connexion, on envoie seul
    validate(partial) {
      if (!ready || !inTeam()) return Promise.resolve({ submit: true, waiting: [] });
      if (answer) answer(null);
      return new Promise((resolve) => {
        answer = resolve;
        send({ type: 'validate', partial: !!partial });
        setTimeout(() => { if (answer === resolve) { answer = null; resolve(null); } }, 8000);
      });
    },
    unvalidate() { if (ready) send({ type: 'unvalidate' }); },
    credentials: () => (session && !join ? { id: session.id, pid: session.pid, token: session.token } : null),
    get team() { return team; },
    others,
    inTeam,
    joining: () => !!join,
    me: () => (team ? team.participants.find((row) => row.me) : null),
    keyed: () => (team ? team.keyed : []),
    labeled: () => (team ? team.labeled || [] : []),
    entries: () => (team ? team.entries : []),
  };
})();

// Scans de la vérif commune : les notres puis ceux des autres participants (un meme code n'est compte qu'une fois)
const entryKey = (entry) => (entry.kind === 'item' ? 'i:' + entry.iid : 'p:' + entry.code);
function allScanned() {
  const seen = new Set(state.scanned.map(entryKey));
  const shared = live.entries().filter((entry) => !seen.has(entryKey(entry)) && seen.add(entryKey(entry)));
  return shared.length ? [...state.scanned, ...shared] : state.scanned;
}

// Lots de la vérif commune qui manquent ici (on vient de la rejoindre, ou un autre participant en a ajoute un)
let teamLoading = false;
async function syncTeamLots() {
  const lots = live.team ? live.team.lots : [];
  if (!lots.length || teamLoading) return;
  const known = new Set(sessionLots().map((lot) => lot.id));
  const missing = lots.filter((id) => !known.has(id));
  if (!missing.length) return;
  teamLoading = true;
  try {
    if (!state.lot) {
      await loadLot(lots[0]);
      if (!state.lot) return;
    }
    const ids = new Set(sessionLots().map((lot) => lot.id));
    for (const id of lots) if (!ids.has(id) && !state.extra.some((entry) => entry.id === id)) state.extra.push({ id, key: '', lot: null });
    await loadExtra();
    save();
  } finally {
    teamLoading = false;
    render();
  }
}

// Lot de la cascade ou l'on travaille encore : on y est (ou plus bas), ou des scans pas encore valides y sont faits.
// Un lot valide par un autre participant n'y est pas note valide (on le refait peut-etre).
function working(id) {
  return state.path.includes(id) || scopeEntries(new Set(subtreeIds(id))).length > 0;
}

function teamChanged() {
  const me = live.me();
  if (me && !me.validated && state.waitingTeam) state.waitingTeam = false;
  // vérif en cascade rejointe : lots deja valides par les autres (un lot ou l'on est entre a pu etre rouvert)
  const done = (live.team && live.team.done) || {};
  if (inCascade()) {
    for (const [id, row] of Object.entries(done)) {
      if (!state.done[id] && !working(id) && lotById(id)) state.done[id] = row;
    }
  }
  // « vérif complète » partagee : activee par la gestion ou un admin, ici ou depuis la page de suivi
  // (un changement fait ici avant la connexion est renvoye au serveur au lieu d'etre ecrase)
  if (live.team && 'full' in live.team) {
    if (state.fullLocal && !!live.team.full !== state.full && canSeeStock()) live.setFull(state.full);
    else { state.full = !!live.team.full; state.fullBy = live.team.full_by || ''; state.fullLocal = false; }
  }
  syncTeamLots();
  render();
}

// Vérif en cascade a plusieurs : un autre participant a valide un lot. Il est note valide, nos scans qu'il a envoyes
// sont retires ; s'il s'agit du lot ou l'on se trouve, on revient au lot qui le contient.
function teamStep(message) {
  if (!inCascade()) return;
  const at = new Date().toISOString();
  for (const row of message.lots || []) {
    if (lotById(row.id)) state.done[row.id] = { at, by: message.by || '', complete: !!row.complete, sealed: !!row.sealed };
  }
  stepDone({ lots: [] }, message.items || []);
  state.scanned = state.scanned.filter((entry) => !(entry.kind === 'seal' && (message.items || []).includes(entry.iid)));
  while (state.path.length && state.done[levelId()]) state.path.pop();
  state.current = levelId();
  const names = (message.lots || []).filter((row) => !row.sealed || (message.lots || []).length === 1).map((row) => row.name);
  if (names.length) toast(`${message.by || 'Quelqu\'un'} a validé ${names.join(', ')}`);
  save();
  loadLot(state.lotId, '', true);
}

// Compte rendu de la vérif commune, envoyee par un autre participant
function teamReport(report) {
  showReport(report);
  state.scanned = [];
  state.waitingTeam = false;
  live.done(null);
  forgetLot('Vérif enregistrée. Scannez un item ou l\'étiquette d\'un lot pour en commencer une autre.');
  loadLots();
  backToList();
}

// Rejoindre une vérif en cours (page de suivi) : ses scans et ses lots arrivent par la connexion
async function joinLiveVerif(row) {
  if (!state.user) { openReader('live'); toast('Scannez votre badge pour rejoindre une vérif.'); return; }
  const lotId = row.lot && row.lot.id;
  if (state.scanned.length && state.lotId !== lotId
    && !await confirmDialog('Rejoindre cette vérif ?', 'Les items que vous avez déjà scannés sur un autre lot sont oubliés.', 'Rejoindre')) return;
  if (state.lotId !== lotId) {
    state.scanned = [];
    state.lot = null; state.lotId = ''; state.lotKey = ''; state.extra = [];
    resetCascade();
  }
  state.lastVerif = null;
  state.waitingTeam = false;
  state.current = '';
  state.askPlace = !state.scanned.length; // on demandera ou l'on se trouve (sous-lot)
  live.join(row.id);
  enterScanner();
  switchTab('todo');
  if (lotId) loadLot(lotId);
  showInfo('lot', `Vérif de ${row.who || 'quelqu\'un'} rejointe`, 'Scannez vos items : ils s\'ajoutent à la vérif commune.');
  save();
  render();
}

// Bandeau discret pendant une vérif à plusieurs : qui participe, qui a valide ; autre vérif du meme lot a rejoindre
function renderTeam() {
  const box = $('#team');
  const team = live.team;
  const others = live.others();
  const parts = [];
  if (others.length) {
    parts.push(el('span', {}, `👥 Vérif à ${team.participants.length} :`),
      ...team.participants.map((row) => el('span', { class: (row.me ? 'who' : '') + (row.connected ? '' : ' off'),
        title: row.connected ? (row.validated ? 'A validé sa partie' : 'Connecté') : 'Déconnecté' },
      (row.me ? 'vous' : row.who || 'Invité') + (row.validated ? ' ✔' : ''))),
      el('button', { type: 'button', class: 'text-link', onclick: async () => {
        if (!await confirmDialog('Quitter la vérif commune ?', 'Vous continuez seul, avec vos propres scans.', 'Quitter')) return;
        state.waitingTeam = false;
        live.leave();
        render();
      } }, 'Quitter'));
  } else if (live.joining()) {
    parts.push(el('span', {}, '👥 Connexion à la vérif commune…'));
  } else if (team && team.nearby.length && state.lot) {
    const other = team.nearby[0];
    parts.push(el('span', {}, `👥 ${other.who || 'Quelqu\'un'} vérifie aussi ${other.lot ? other.lot.name : 'ce lot'}.`),
      el('button', { type: 'button', onclick: async () => {
        if (!state.user) { toast('Scannez votre badge pour rejoindre sa vérif.', true); return; }
        if (!await confirmDialog(`Rejoindre la vérif de ${other.who || 'cette personne'} ?`, "Vos scans s'ajoutent à la sienne.", 'Rejoindre')) return;
        state.waitingTeam = false;
        state.askPlace = !state.scanned.length;
        live.join(other.id);
        render();
      } }, 'Rejoindre'));
  }
  // vérif rejointe sans rien scanner : dans quel sous-lot est-on ? (les items venus du stock y seront ranges)
  const places = sessionLots().filter((lot) => (lot.requirements || []).length || (lot.items || []).length);
  if (inCascade()) {
    // vérif en cascade : le fil du lot en cours est dans la liste « À scanner »
  } else if (state.askPlace && places.length > 1) {
    parts.push(el('div', { class: 'place' }, el('b', {}, '📍 Où êtes-vous ? '),
      "Scannez l'étiquette de celui où vous êtes, ou choisissez : ",
      ...places.map((lot) => el('button', { type: 'button', onclick: () => {
        state.current = lot.id;
        state.askPlace = false;
        toast(`Lot en cours : ${lot.name}`);
        save(); render();
      } }, lot.name))));
  } else if (!state.askPlace && places.length > 1 && checkRunning() && state.current) {
    const current = places.find((lot) => lot.id === currentPlace());
    if (current) parts.push(el('span', { title: 'Les items venus du stock scannés maintenant y sont rangés. Scannez l\'étiquette d\'un autre lot pour changer.' },
      `📍 ${current.name}`));
  }
  box.hidden = !parts.length;
  box.replaceChildren(...parts);
}

// Vérif en cascade : avancement de tout le lot verifie, sous-lots compris (barre du suivi en direct). Une unite par
// item attendu (une case a cocher compte pour une) ; un sous-lot valide ou constate scelle intact compte tout son
// contenu comme fait, un lot scelle en attente compte son contenu a faire ; un sous-lot facultatif pas commence ne
// compte pas. Le reste a scanner du lot en cours (renderTodo) melangeait items et sous-lots : « 0/1 » sans sens.
function cascadeProgress() {
  const plan = planSession();
  const lots = sessionLots();
  const kids = (id) => lots.filter((lot) => realParentOf(lot) === id);
  const skip = (lot) => lot.id !== state.lotId && lotOptional(lot) && !touched(lot.id) && !state.done[lot.id];
  // contenu attendu d'un lot et de ses sous-lots, sans tenir compte des scans
  const units = (lot) => {
    if (skip(lot)) return 0;
    const own = lot.storage ? (lot.items || []).filter((item) => !itemOptional(item)).length
      : (lot.requirements || []).map(strictRow).reduce((sum, row) => sum + (row.optional ? 0 : row.check ? 1 : rowItems(row)), 0);
    return own + kids(lot.id).reduce((sum, sub) => sum + units(sub), 0);
  };
  let done = 0;
  let total = 0;
  const walk = (lot) => {
    if (skip(lot)) return;
    if (state.done[lot.id] || plan.intact.has(lot.id) || sealWaiting(lot)) {
      const count = units(lot);
      total += count;
      if (!sealWaiting(lot)) done += count;
      return;
    }
    if (lot.storage || ownContent(lot)) {
      const { groups, others, remaining } = expectedGroups(lot, plan);
      const count = groups.reduce((sum, group) => sum + (group.optional ? 0 : group.check ? 1 : rowItems(group.row)), 0)
        + others.filter((item) => !itemOptional(item)).length;
      total += count;
      done += Math.max(0, count - remaining);
    }
    for (const sub of kids(lot.id)) walk(sub);
  };
  if (state.lot) walk(state.lot);
  return { done, total };
}

// Etat publie : lot, lots de la vérif groupee, items scannes (nom, perime, inconnu, nouveau), reste a scanner.
// entries (nos scans) et keys (cles des lots) ne vont qu'au serveur et aux autres participants de la vérif.
function liveSnapshot(remaining) {
  // la vérif commence (et vit sur le serveur, partagee) des le premier scan, l'etiquette cle du lot, ou
  // « Lancer une vérif » depuis la fiche du lot
  const scanned = allScanned();
  if (!state.lot || !(scanned.length || verifInProgress() || (state.launched === state.lotId && !state.lastVerif)
    || cascadeActive())) return null;
  const expected = knownIids();
  const discard = discardedEntry(planSession());
  return {
    lot: { id: state.lot.id, name: state.lot.name },
    root: rootOf(state.lot),
    // vérif en cascade : le lot global seulement, chacun publie le lot ou il se trouve (level)
    lots: inCascade() ? [] : sessionLots().filter((lot) => lot.id !== state.lot.id && ((lot.requirements || []).length || (lot.items || []).length))
      .map((lot) => lot.name),
    scanned: scanned.map((entry) => ({
      name: entry.kind === 'check' && entry.info ? `☑ ${entry.info.type_name}`
        : entry.kind === 'pack' && entry.info ? `Paquet : ${entry.info.count} × ${entry.info.type_name}`
        // pas d'iid : le suivi en direct est vu par tous, pas seulement le TECH
        : (entry.info && entry.info.type_name) || (entry.kind === 'item' && !entry.error ? 'Item' : 'code inconnu'),
      expired: !!entry.expired,
      // perime a jeter (le lot a son compte sans lui, ou il n'y etait pas) : pas de rouge dans le suivi
      discard: discard(entry),
      error: !!entry.error,
      extra: entry.kind === 'item' && !expected.has(entry.iid),
    })),
    ...(inCascade() ? (({ done, total }) => ({ count: done, remaining: total - done }))(cascadeProgress())
      : { count: scannedIids().size, remaining }),
    entries: state.scanned.map(({ code, kind, iid, key, expired, error, items, info, place }) => ({
      code, kind, iid, key, expired, error, items, place,
      info: info ? { type: info.type, type_name: info.type_name, peremption: info.peremption, count: info.count,
        // revision en retard seule : jamais a jeter (planSession, discard)
        revision_late: !!info.revision_late, outdated: !!info.outdated, unusable: !!info.unusable, disabled: !!info.disabled } : null,
    })),
    keys: sessionEntries(),
    labels: state.labels,
    // vérif en cascade : lot ou l'on se trouve (les autres participants le voient « en cours »)
    cascade: inCascade(),
    level: inCascade() ? { id: levelLot().id, name: levelLot().name } : null,
    optional: sessionHasOptional(), // bouton « Rendre la vérif complète » de la page de suivi (admins)
  };
}

// Page de suivi
const watch = { socket: null, active: false, connected: false, sessions: new Map(), error: '', retry: 0, ping: 0,
  tick: 0, recent: null, recentLoading: false, open: new Set() };

function watchStart() {
  if (!state.user) { renderLive(); return; }
  watch.active = true;
  watchConnect();
  loadRecent();
  clearInterval(watch.tick);
  watch.tick = setInterval(renderLive, 15000); // « il y a 2 min »
}

function watchStop() {
  watch.active = false;
  clearInterval(watch.tick);
  if (watch.socket) { const current = watch.socket; watch.socket = null; current.close(); }
  clearInterval(watch.ping);
  watch.connected = false;
}

function watchConnect() {
  if (watch.socket || !watch.active || !state.user || !('WebSocket' in window)) return;
  const current = new WebSocket(socketUrl('ws/live/watch'));
  watch.socket = current;
  current.onopen = () => {
    current.send(JSON.stringify({ type: 'hello', user: credentials() }));
    clearInterval(watch.ping);
    watch.ping = setInterval(() => { if (current.readyState === WebSocket.OPEN) current.send('{"type":"ping"}'); }, PING_MS);
  };
  current.onmessage = (event) => {
    let message;
    try { message = JSON.parse(event.data); } catch (e) { return; }
    if (message.type === 'snapshot') {
      watch.connected = true;
      watch.retry = 0;
      watch.error = '';
      watch.sessions = new Map(message.sessions.map((row) => [row.id, row]));
    } else if (message.type === 'update') {
      const before = watch.sessions.get(message.session.id);
      watch.sessions.set(message.session.id, message.session);
      if (message.session.done && !(before && before.done)) loadRecent(); // vérif validee : historique a jour
    } else if (message.type === 'remove') {
      watch.sessions.delete(message.id);
    } else if (message.type === 'error') {
      watch.error = message.message;
      if (message.login_required) watch.active = false;
    }
    renderLive();
  };
  current.onclose = () => {
    if (watch.socket !== current) return;
    clearInterval(watch.ping);
    watch.socket = null;
    watch.connected = false;
    renderLive();
    if (watch.active) setTimeout(watchConnect, Math.min(30000, 1000 * 2 ** watch.retry++));
  };
}

async function loadRecent() {
  if (!state.user) return;
  watch.recentLoading = true;
  renderLive();
  try {
    watch.recent = await api('verifs/recent/', { user: badge() });
  } catch (e) {
    toast('Vérifs récentes : ' + e.message, true);
  } finally {
    watch.recentLoading = false;
    renderLive();
  }
}

function ago(value) {
  const seconds = Math.max(0, (Date.now() - new Date(value)) / 1000);
  if (seconds < 60) return "à l'instant";
  if (seconds < 3600) return `il y a ${Math.round(seconds / 60)} min`;
  return fmtDateTime(value);
}

const timeOf = (value) => new Date(value).toLocaleTimeString('fr-FR', { hour: '2-digit', minute: '2-digit' });

// Vérif faite hors connexion, envoyee plus tard : datee a l'heure des scans (heure de l'appareil), l'heure de
// reception par le serveur est donnee a cote pour reperer un antidatage
const sentText = (sent) => `📶 Faite hors connexion · reçue par le serveur ${fmtOn(sent)}`;


function liveCard(row) {
  const done = row.done;
  const kind = done ? (done.complete ? 'ok' : 'bad') : row.connected ? '' : 'lost';
  const lot = row.lot || { id: '', name: '?' };
  const status = done && done.lots != null
    ? `${done.complete ? '✔' : '✘'} Terminée à ${timeOf(done.at)} : ${nb(done.lots, 'lot validé', 'lots validés')}` + (done.incomplete ? `, dont ${nb(done.incomplete, 'incomplet')}` : '')
    : done
    ? `${done.complete ? '✔' : done.partial ? '↷' : '✘'} Validée à ${timeOf(done.at)} : ${done.complete ? 'complet' : done.partial ? 'vérif partielle' : 'incomplet'}, ${nb(done.present, 'présent')}`
    : row.connected ? (row.scanned.length ? `En cours · dernier scan ${ago(row.updated)}` : 'En cours · aucun item scanné')
      : `Connexion perdue ${ago(row.updated)}`;
  const open = watch.open.has(row.id);
  const shown = open ? [...row.scanned].reverse() : row.scanned.slice(-4).reverse();
  const people = row.participants || [{ who: row.who, verified: row.verified, connected: row.connected }];
  const mine = (live.credentials() || {}).id === row.id;
  // vérif terminée : clic n'importe ou sur la carte → son compte rendu, d'ou l'on ouvre les fiches des lots (titre
  // sans lien) ; vérif en cours : le nom du lot ouvre sa fiche
  const verifs = (done && done.verifs) || [];
  return el('div', { class: 'live-card ' + kind + (verifs.length ? ' clickable' : ''),
    title: verifs.length ? 'Voir le compte rendu' : null, onclick: verifs.length ? (event) => {
      if (!event.target.closest('a, button')) openVerifReport(verifs);
    } : null },
    // vérif terminée : les lots qui la nomment (serveur, comme « Vérifs récentes ») : un seul rangement du Stock
    // → son nom, pas « Stock »
    el('div', { class: 'name' }, '📦 ', done && (done.names || []).length ? done.names.join(', ')
      : [verifs.length ? lot.name : lotLink(lot), row.lots.length ? ' + ' + row.lots.join(', ') : '']),
    el('div', { class: 'sub' }, (people.length > 1 ? '👥 ' : '') + people.map((person) => (person.who || 'Invité')
      + (person.who && !person.verified && !/invité/i.test(person.who) ? ' (nom déclaré)' : '') + (people.length > 1 && !done && person.validated ? ' ✔' : '')
      + (people.length > 1 && !done && !person.connected ? ' (déconnecté)' : '')
      + (!done && person.level && person.level.id !== lot.id ? ` 📍 ${person.level.name}` : '')).join(', ')
      + ` · commencée à ${timeOf(row.started)}`),
    el('div', { class: 'sub' }, status),
    // vérif en cascade : avancement de tout le lot verifie (cascadeProgress), pas les scans du lot en cours
    done ? null : requirementRow(row.cascade ? 'Avancement' : 'Scannés', row.count, row.count + row.remaining),
    // perime a jeter : gris, pas de rouge ; vérif terminée complète : plus de rouge du tout
    row.scanned.length ? el('ul', {}, shown.map((item) => el('li', {
      class: item.error ? 'error' : item.discard || (item.expired && done && done.complete) ? 'discard' : item.expired ? 'expired' : '' },
      item.name, item.discard ? el('span', { class: 'tag gray' }, 'À JETER')
        : item.expired ? el('span', { class: 'tag ' + (done && done.complete ? 'gray' : 'red') }, done && done.complete ? 'À REMPLACER' : 'PÉRIMÉ') : null,
      item.error ? el('span', { class: 'tag red' }, 'INCONNU') : null,
      item.extra ? el('span', { class: 'tag blue' }, 'nouveau') : null))) : null,
    row.scanned.length > 4 ? el('button', { type: 'button', class: 'text-link', onclick: () => {
      if (open) watch.open.delete(row.id); else watch.open.add(row.id);
      renderLive();
    } }, open ? 'Réduire' : `Voir les ${row.scanned.length} scans`) : null,
    // vérif à plusieurs : rejoindre la vérif, ses scans s'ajoutent a ceux de ce telephone
    !done && row.lot && !mine ? el('div', { class: 'join' },
      el('button', { type: 'button', onclick: () => joinLiveVerif(row) }, '👥 Rejoindre cette vérif')) : null,
    mine && !done ? el('div', { class: 'sub' }, 'Vous participez à cette vérif.') : null,
    // « vérif complète » : items et sous-lots facultatifs obligatoires pour tous les participants (gestion et admins)
    !done && row.full ? el('div', { class: 'sub' }, '✔ Vérif complète : le facultatif est obligatoire'
      + (row.full_by ? ` (par ${row.full_by})` : '')) : null,
    !done && canSeeStock() && (row.optional || row.full) ? el('div', { class: 'join' },
      el('button', { type: 'button', class: row.full ? 'secondary' : 'warning', onclick: () => {
        if (!watch.socket || watch.socket.readyState !== WebSocket.OPEN) { toast('Suivi déconnecté, réessayez.', true); return; }
        watch.socket.send(JSON.stringify({ type: 'full', id: row.id, on: !row.full }));
      } }, row.full ? 'Revenir au facultatif' : 'Rendre la vérif complète')) : null);
}

function renderLive() {
  const view = $('#home-live');
  if (!view || state.homePage !== 'live') return;
  if (!state.user) {
    view.replaceChildren(
      el('p', {}, 'Scannez votre badge pour suivre les vérifs en direct et voir les dernières vérifs.'),
      el('button', { type: 'button', class: 'primary', onclick: () => openReader('live') }, 'Se connecter'));
    return;
  }
  const parts = [el('div', { class: 'row-head' },
    el('h3', {}, 'En direct ', el('span', { class: 'live-status' + (watch.connected ? ' on' : '') }, el('i'),
      watch.connected ? 'connecté' : watch.active ? 'connexion…' : 'hors ligne')))];
  if (watch.error) parts.push(el('p', { class: 'error' }, watch.error));
  const rows = [...watch.sessions.values()].sort((a, b) => (a.done ? 1 : 0) - (b.done ? 1 : 0)
    || (a.updated < b.updated ? 1 : -1));
  if (!rows.length) parts.push(el('p', { class: 'hint' }, 'Aucune vérif en cours. Les scans apparaissent ici en direct.'));
  parts.push(...rows.map(liveCard));
  parts.push(el('div', { class: 'row-head' }, el('h3', {}, 'Vérifs récentes'),
    el('button', { type: 'button', onclick: loadRecent }, icon('refresh'), watch.recentLoading ? 'Chargement…' : 'Actualiser')));
  if (!watch.recent) {
    parts.push(el('p', { class: 'hint' }, watch.recentLoading ? 'Chargement…' : 'Liste non chargée.'));
  } else if (!watch.recent.length) {
    parts.push(el('p', { class: 'hint' }, 'Aucune vérif enregistrée.'));
  } else {
    // clic n'importe ou sur la ligne : compte rendu de la vérif, qui mene aux fiches des lots (pas de lien ici)
    // une ligne par vérif : les étapes d'une cascade (VPS > armoire > sac) réunies, nommée par ses lots du haut
    parts.push(el('ul', { class: 'list' }, ...watch.recent.map((row) => el('li', { class: row.complete ? 'ok' : 'bad',
      title: 'Voir le compte rendu', onclick: () => openVerifReport(row.ids || row.id) },
      el('div', { class: 'main' },
        // noms des lots vérifiés ; un lot remplace ses sous-lots seulement s'ils y sont tous (serveur)
        el('div', { class: 'name' }, (row.top || row.lots).map((lot) => lot.name).join(', ')),
        el('div', { class: 'sub' }, `${cap(fmtDateTime(row.at))} · ${[row.by || '?', ...(row.participants || [])].join(', ')} · ${nb(row.present, 'présent')}`
          + (row.missing ? ` · ${nb(row.missing, 'manquant')}` : '') + (row.expired ? ` · ${nb(row.expired, 'périmé')}` : '')
          + (row.replaced ? ` · ${row.replaced} à jeter` : '')),
        row.sent ? el('div', { class: 'sub offline-sent' }, sentText(row.sent)) : null),
      el('span', { class: 'tag ' + (row.complete ? 'green' : 'red') }, row.complete ? 'complet' : 'incomplet')))));
  }
  view.replaceChildren(...parts);
}

// Compte rendu d'une vérif passée, compact : bandeau d'état, puis l'arbre des lots, une ligne par lot.
// Secouristes et chefs : l'état en un mot, les lots et sous-lots vérifiés et seulement ce qui est à faire (manque,
// à remplacer, à jeter). Gestion et admin : même présentation, avec les comptes, les présents et non
// scannés à déplier par lot ([+]) et les opérations faites pendant la vérif.
// ids : une vérif, ou les étapes d'une vérif en cascade terminée (suivi en direct), réunies en un compte rendu
const reportView = { report: null, open: new Set() };

async function openVerifReport(ids) {
  let reports;
  try {
    reports = await Promise.all([].concat(ids).map((id) => api(`verifs/${id}/report/`, { user: badge() })));
  } catch (e) {
    toast('Compte rendu : ' + e.message, true);
    return;
  }
  const last = reports[reports.length - 1];
  // un lot revu à plusieurs étapes d'une cascade : sa dernière vérif compte
  const byId = new Map();
  for (const lot of reports.flatMap((one) => one.lots)) { byId.delete(lot.id); byId.set(lot.id, lot); }
  // lot cadre : vérifié sans contenu à lui, ses sous-lots pas tous là, avec un sous-lot vérifié (le Stock dont on
  // n'a fait qu'un rangement) : ni ligne ni compte, comme dans « Vérifs récentes »
  const verified = new Set(byId.keys());
  const covered = (id, seen = new Set()) => {
    if (seen.has(id) || !verified.has(id)) return false;
    seen.add(id);
    return (byId.get(id).children || []).every((kid) => covered(kid, seen));
  };
  const hasVerifiedUnder = (id) => [...byId.values()].some((lot) => {
    for (let up = lot.parent, guard = 0; up && guard < 50; up = (byId.get(up) || {}).parent, guard += 1) {
      if (up === id) return true;
    }
    return false;
  });
  const frame = (lot) => byId.size > 1 && !covered(lot.id) && hasVerifiedUnder(lot.id)
    && !(lot.present_count + lot.missing_count + lot.expired_count);
  const lots = [...byId.values()].filter((lot) => !frame(lot));
  reportView.report = { ...last, complete: reports.every((one) => one.complete),
    participants: [...new Set(reports.flatMap((one) => [one.by, ...(one.participants || [])]))].filter((who) => who !== last.by),
    lots, sealed: reports.flatMap((one) => one.sealed),
    operations: reports.flatMap((one) => one.operations) };
  reportView.open = new Set();
  renderVerifReport();
  if (!$('#verif-report').open) $('#verif-report').showModal();
}

function renderVerifReport() {
  const report = reportView.report;
  const full = detailed();
  const sum = (list) => list.reduce((total, row) => total + row.count, 0);
  // un seul arbre pour toutes les étapes : chaque lot sous son parent, ce qui demande l'attention d'abord
  const ids = new Set(report.lots.map((lot) => lot.id));
  const children = new Map();
  for (const lot of report.lots) {
    const parent = ids.has(lot.parent) ? lot.parent : null;
    if (!children.has(parent)) children.set(parent, []);
    children.get(parent).push(lot);
  }
  const rank = (lot) => (lot.complete ? 2 : lot.partial ? 1 : 0);
  const tree = [];
  const walk = (parent, depth) => {
    for (const lot of [...(children.get(parent) || [])].sort((a, b) => rank(a) - rank(b))) {
      tree.push({ lot, depth });
      // tous les sous-lots vérifiés, pour tout le monde : chacun sous son parent avec son état
      walk(lot.id, depth + 1);
    }
  };
  walk(null, 0);
  const state = (lot) => (lot.complete ? 'ok' : lot.partial ? 'partial' : 'bad');
  const bad = report.lots.filter((lot) => !lot.complete);
  const partial = report.lots.some((lot) => lot.partial);
  const kind = report.complete ? 'ok' : partial && bad.every((lot) => lot.partial) ? 'partial' : 'bad';
  const title = kind === 'ok' ? (report.lots.length > 1 ? 'Tout est complet' : 'Lot complet')
    : kind === 'partial' ? 'Vérif partielle'
    : report.lots.length === 1 ? 'Lot incomplet'
    : full ? `${nb(bad.length, 'lot à compléter', 'lots à compléter')} sur ${report.lots.length}` : 'À compléter';
  const people = [report.by || '?', ...(report.participants || [])];
  const total = (key) => report.lots.reduce((count, lot) => count + lot[key + '_count'], 0);
  const stats = full ? [[report.lots.length, 'lot', 'lots', ''], [total('present'), 'présent', 'présents', ''],
    [total('missing'), 'non scanné', 'non scannés', 'warn'], [total('expired'), 'périmé', 'périmés', 'bad'],
    [total('replaced'), 'à jeter', 'à jeter', 'gray']].filter(([count], index) => count || index < 2) : [];
  const typeText = (row) => `${row.count} × ${row.type_name}` + (row.peremption ? ` · ${fmtDate(row.peremption)}` : '');
  // ce qui reste à faire sur le lot, toujours visible
  const issues = (lot) => [
    ...(lot.lacking || []).map((row) => el('div', { class: 'vr-issue bad' },
      `Manque ${lackingText(row, Math.max(0, row.required - row.present))}`,
      full ? el('span', { class: 'vr-ratio' }, ` (${row.present}/${row.required}${row.unit && !row.within ? ' ' + row.unit : ''})`) : null)),
    ...lot.expired.map((row) => el('div', { class: 'vr-issue bad' }, 'À remplacer : ' + typeText(row))),
    ...lot.replaced.map((row) => el('div', { class: 'vr-issue gray' }, 'À jeter : ' + typeText(row))),
  ];
  const lotRow = ({ lot, depth }) => {
    const open = reportView.open.has(lot.id);
    const more = full && (lot.present.length || lot.missing.length);
    const words = { ok: 'complet', partial: 'partielle', bad: 'incomplet' };
    return el('li', { class: 'vr-lot ' + state(lot) + (depth ? ' sub' : ''), style: depth ? `--depth:${depth}` : null },
      el('div', { class: 'vr-row' },
        el('span', { class: 'vr-ico', 'aria-hidden': 'true' }, lot.complete ? '✓' : lot.partial ? '↷' : '!'),
        el('span', { class: 'vr-name' }, lotLink(lot)),
        full ? el('span', { class: 'vr-count', title: nb(lot.present_count, 'présent')
          + (lot.missing_count ? `, ${nb(lot.missing_count, 'non scanné')}` : '') }, String(lot.present_count),
          lot.missing_count ? el('span', { class: 'warn' }, ` −${lot.missing_count}`) : null) : null,
        lot.complete ? null : el('span', { class: 'vr-word' }, words[state(lot)]),
        more ? el('button', { type: 'button', class: 'lot-fold' + (open ? ' open' : ''), 'aria-expanded': String(open),
          'aria-label': open ? 'Replier le détail' : 'Voir le détail', onclick: () => {
            if (open) reportView.open.delete(lot.id); else reportView.open.add(lot.id);
            renderVerifReport();
          } }) : null),
      ...issues(lot),
      open ? el('div', { class: 'vr-more' },
        lot.missing.length ? el('div', { class: 'vr-more-head warn' }, `Non scannés (${sum(lot.missing)})`) : null,
        ...lot.missing.map((row) => el('div', {}, typeText(row))),
        lot.present.length ? el('div', { class: 'vr-more-head' }, `Présents (${sum(lot.present)})`) : null,
        ...lot.present.map((row) => el('div', {}, typeText(row)))) : null);
  };
  $('#verif-report-body').replaceChildren(
    el('div', { class: 'vr-head ' + kind },
      el('div', { class: 'vr-title' }, el('span', { class: 'vr-ico', 'aria-hidden': 'true' },
        kind === 'ok' ? '✓' : kind === 'partial' ? '↷' : '!'), title),
      el('div', { class: 'vr-sub' }, `${cap(fmtDateTime(report.at))} · ${people.length > 1 ? '👥 ' : ''}${people.join(', ')}`),
      report.sent ? el('div', { class: 'vr-sub offline-sent' }, sentText(report.sent)) : null,
      stats.length ? el('div', { class: 'vr-stats' }, ...stats.map(([count, one, many, tone]) =>
        el('span', { class: tone }, nb(count, one, many)))) : null),
    el('ul', { class: 'vr-lots' }, ...tree.map(lotRow),
      ...report.sealed.map((lot) => el('li', { class: 'vr-lot ok' }, el('div', { class: 'vr-row' },
        el('span', { class: 'vr-ico sealed', 'aria-hidden': 'true' }, '🔒'), el('span', { class: 'vr-name' }, lotLink(lot)),
        el('span', { class: 'vr-word' }, 'scellé intact'))))),
    full && report.operations.length ? el('div', { class: 'vr-ops' }, el('div', { class: 'vr-more-head' }, 'Pendant cette vérif'),
      ...report.operations.map((op) => el('div', {}, op.kind_label,
        op.lot ? [' · ', lotLink({ id: op.lot, name: op.lot_name })] : '', op.summary ? ` : ${op.summary}` : ''))) : '',
  );
}
$('#verif-report-close').addEventListener('click', () => $('#verif-report').close());
// nom d'un lot : sa fiche s'ouvre a la place du compte rendu (retour : page de suivi)
$('#verif-report-body').addEventListener('click', (event) => {
  if (event.target.closest('a.link')) $('#verif-report').close();
}, true); // capture : le lien arrete la propagation
