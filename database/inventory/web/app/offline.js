/* QRProtec - front web : appels de l'API, hors connexion (file d'envois en attente, outbox) et cache de vérif (vcache). */
'use strict';

// Hors connexion : une partie de la reserve n'a ni reseau ni wifi. La vérif se fait depuis le cache de vérif (vcache)
// et ce qui doit aller au serveur (vérif, etape de la vérif en cascade, scelle intact) attend dans une file gardee sur
// le telephone (outbox), renvoyee dans l'ordre des le retour du reseau. Une petite mention le dit en haut du scanner.
const STORAGE_OUTBOX = 'qrprotec.outbox';
const APP_CACHE_AGE = 10 * 60 * 1000; // a l'ouverture de l'appli, cache de vérif remis a jour s'il a plus de 10 min
let offline = false;
let maintenance = false; // derniere reponse : 503 de la maintenance (mention « Maintenance en cours »)
let outbox = [];
try { outbox = JSON.parse(localStorage.getItem(STORAGE_OUTBOX) || '[]') || []; } catch (e) { outbox = []; }
let outboxBusy = false;

function saveOutbox() {
  try { localStorage.setItem(STORAGE_OUTBOX, JSON.stringify(outbox)); } catch (e) { /* stockage indisponible */ }
}

// Nombre de vérifs en attente (une vérif en cascade compte une fois, meme envoyee en plusieurs etapes)
const pendingVerifs = () => new Set(outbox.map((entry) => entry.group || entry.at)).size;
const pendingText = (count) => `${nb(count, 'vérif pas encore envoyée', 'vérifs pas encore envoyées')}`;
const GO_TO_NETWORK = 'Allez à un endroit avec du réseau (wifi du local, 4G) et gardez QRprotec ouvert quelques '
  + 'secondes : l\'envoi est automatique.';

function showOffline() {
  const chip = $('#offline-chip');
  const banner = $('#outbox-banner');
  const waiting = outbox.length;
  const count = pendingVerifs();
  if (chip) {
    chip.hidden = !offline && !waiting;
    chip.classList.toggle('pending', !offline && waiting > 0);
    chip.classList.toggle('waiting', offline && waiting > 0);
    const state = maintenance ? 'Maintenance en cours' : 'Hors connexion';
    chip.textContent = offline ? (waiting ? `📶 ${nb(count, 'vérif', 'vérifs')} à envoyer` : state)
      : `Envoi en cours… (${count})`;
  }
  // accueil : bandeau tant qu'une vérif attend, pour aller la ou il y a du reseau
  if (banner) {
    banner.hidden = !waiting;
    if (waiting) {
      banner.replaceChildren(el('div', {}, `📶 ${pendingText(count)}`),
        el('div', { class: 'sub' }, `Faite hors connexion ${fmtOn(outbox[0].at)}. ${GO_TO_NETWORK}`),
        el('button', { type: 'button', class: 'secondary', disabled: outboxBusy || null, onclick: sendNow },
          outboxBusy ? 'Envoi en cours…' : 'Envoyer maintenant'));
    }
  }
}

// « Envoyer maintenant » (bandeau de l'accueil, mention du scanner) : essai tout de suite, et si le reseau manque
// toujours, on le dit clairement
async function sendNow() {
  if (!outbox.length) { if (offline) api('health/').catch(() => {}); return; }
  await flushOutbox();
  if (outbox.length && offline) {
    feedback.warn();
    toast(`Toujours pas de réseau : ${pendingText(pendingVerifs())}. ${GO_TO_NETWORK}`, true);
  }
}

// Mention du scanner touchee : ce qui attend, et comment l'envoyer
async function explainOutbox() {
  if (!outbox.length) {
    await askDialog({ title: maintenance ? 'Maintenance en cours' : 'Hors connexion', cancel: 'OK', choices: [],
      text: 'Le serveur ne répond pas. La vérif marche quand même : elle est gardée sur ce téléphone et envoyée dès le retour du réseau.' });
    return;
  }
  const labels = [...new Map(outbox.map((entry) => [entry.group || entry.at, entry])).values()];
  const choice = await askDialog({
    title: `📶 ${pendingText(labels.length)}`,
    text: GO_TO_NETWORK,
    body: el('ul', { class: 'sent-review' }, labels.map((entry) => el('li', {}, `${entry.label} · faite ${fmtOn(entry.at)}`))),
    choices: [{ label: 'Envoyer maintenant', value: true, cls: 'primary' }],
    cancel: 'Plus tard',
  });
  if (choice) sendNow();
}

// Reponse (ou non) du serveur a une requete : la mention suit, et le retour du reseau vide la file d'attente
function setOffline(value) {
  if (offline === value) return;
  offline = value;
  showOffline();
  if (!value && outbox.length) setTimeout(flushOutbox, 0);
}

// Heures de l'appareil jointes a une vérif : done_at a la validation (juste apres les scans), sent_at a l'envoi. Le
// serveur date la vérif a l'heure des scans, recalee sur son horloge par l'ecart entre les deux, et garde l'heure
// de reception a cote (views.verif_time) : une vérif faite hors connexion n'est pas datee de son envoi.
const stamped = (body, doneAt) => ({ ...body, done_at: body.done_at || doneAt, sent_at: new Date().toISOString() });

// Ce que le telephone savait des lots et des items de la vérif au moment de la mettre en file (cache de vérif) : a
// l'envoi, ce qui a change entre-temps (signalement, contenu du lot, item devenu inutilisable) est signale
function offlineSnapshot(body) {
  const first = (body.lots || [])[0];
  const root = first ? vcache.rootOf(first.id) : '';
  const tree = root ? vcache.lot(root) : null;
  const lots = {};
  for (const lot of tree ? [tree, ...(tree.descendants || [])] : []) {
    lots[lot.id] = { version: lot.version, reports: (lot.reports || []).map((report) => report.id) };
  }
  const items = {};
  for (const iid of body.items || []) {
    const item = vcache.item(iid);
    if (item) items[iid] = { reports: (item.reports || []).map((report) => report.id), unusable: !!(item.unusable || item.disabled) };
  }
  return { root, lots, items };
}

// Vérif (ou etape d'une vérif en cascade) envoyee au serveur. Hors connexion, ou si des envois attendent deja (l'ordre
// compte : etapes de la cascade), elle est mise en file. Renvoie le compte rendu du serveur, ou null si en attente.
async function sendVerif(body, label) {
  const at = new Date().toISOString();
  body = { ...body, done_at: at };
  if (!outbox.length) {
    try {
      return await api('verifs/', stamped(body, at));
    } catch (e) {
      if (e.status !== 0) throw e;
    }
  }
  const first = !outbox.length;
  // etapes d'une meme vérif en cascade : un seul groupe (une vérif a envoyer, un seul bilan au retour du reseau)
  const root = vcache.rootOf(((body.lots || [])[0] || {}).id || '');
  const previous = outbox[outbox.length - 1];
  const group = previous && previous.root && previous.root === root ? previous.group : at;
  outbox.push({ body, label, at, group, root, seen: offlineSnapshot(body) });
  saveOutbox();
  showOffline();
  if (first) {
    // premiere vérif en attente : on le dit en grand, une fois (pas a chaque etape de la cascade)
    feedback.warn();
    askDialog({ title: '📶 Pas de réseau : vérif pas encore envoyée', cancel: 'Compris', choices: [],
      text: `La vérif est gardée sur ce téléphone. ${GO_TO_NETWORK} Tant qu'elle n'est pas envoyée, le TECH ne la voit pas.` });
  }
  return null;
}

// File d'attente renvoyee dans l'ordre. Refusee par le serveur (etiquette cle perimee, scelle brise entre-temps...) :
// retiree de la file et signalee, a refaire.
async function flushOutbox() {
  if (outboxBusy || !outbox.length) return;
  outboxBusy = true;
  showOffline();
  let sent = 0;
  const done = []; // [{entry, report}] : bilan une fois tout envoye
  try {
    while (outbox.length) {
      const next = outbox[0];
      try {
        const report = await api('verifs/', stamped(next.body, next.at)); // file d'avant done_at : heure de mise en file
        done.push({ entry: next, report });
        sent += 1;
      } catch (e) {
        if (e.status === 0) break; // toujours hors connexion : on reessaiera
        feedback.bad();
        showInfo('bad', `Vérif non enregistrée : ${next.label}`, e.message,
          `Faite hors connexion ${fmtOn(next.at)}, refusée par le serveur : à refaire.`);
        toast(`Vérif de ${next.label} refusée par le serveur : ${e.message}`, true);
      }
      outbox.shift();
      saveOutbox();
    }
  } finally {
    outboxBusy = false;
    showOffline();
  }
  if (sent) {
    const groups = new Set(done.map(({ entry }) => entry.group || entry.at)).size;
    toast(outbox.length ? `${nb(groups, 'vérif envoyée', 'vérifs envoyées')}, ${pendingText(pendingVerifs())}.`
      : `Réseau retrouvé : ${nb(groups, 'vérif hors connexion envoyée', 'vérifs hors connexion envoyées')}.`);
    await vcache.sync(); // etat a jour : ce qui a change depuis la vérif
    reviewSent(done);
  }
}

// Bilan d'une vérif faite hors connexion, une fois envoyee : ce que le secouriste n'a pas pu voir sur place. Le
// serveur dit si le lot est complet (requirements), les items a remplacer, et le cache de vérif a jour ce qui a
// change depuis (nouveau signalement, contenu du lot modifie par le TECH, item devenu inutilisable). S'il manque du
// materiel ou si le contenu a change, on propose de reprendre la vérif du lot.
function sentIssues(entry, report) {
  const lines = [];
  let resume = '';
  const needResume = (id) => { if (!resume && id) resume = id; };
  // Une ligne par type d'item, qui dit tout (signale, inutilisable, a remplacer, il en manque) : un seul probleme
  // ne donne qu'une ligne, meme s'il touche le signalement, l'etat de l'item et le compte du lot
  const types = new Map();
  const typeOf = (name) => {
    if (!types.has(name)) types.set(name, { name, comments: [], disabled: false, unusable: 0, lack: null });
    return types.get(name);
  };
  const typeName = (iid) => (vcache.item(iid) || {}).type_name || (vcache.type(iid.slice(0, 6)) || {}).name || iid;
  const shown = new Set();
  const seen = entry.seen;
  const verified = (report.lots || []).filter((row) => row.verified);
  const several = verified.length > 1;
  for (const row of verified) {
    if (row.complete !== false) continue;
    needResume(row.id);
    for (const req of (row.requirements || []).filter((req) => req.present < req.required)) {
      typeOf(reqName(req)).lack = { present: req.present, required: req.required, lot: several ? row.name : '' };
    }
  }
  for (const iid of report.expired || []) {
    typeOf(typeName(iid)).unusable += 1;
    needResume((verified[0] || {}).id);
  }
  for (const [iid, before] of Object.entries((seen && seen.items) || {})) {
    const item = vcache.item(iid);
    if (!item) continue;
    const type = typeOf(item.type_name);
    for (const fresh of (item.reports || []).filter((report) => !before.reports.includes(report.id))) {
      if (shown.has(fresh.id)) continue;
      shown.add(fresh.id);
      if (fresh.comment) type.comments.push(fresh.comment);
    }
    if ((item.unusable || item.disabled) && !before.unusable) {
      if (item.disabled) type.disabled = true;
      if (!(report.expired || []).includes(iid)) type.unusable += 1;
      needResume(item.location);
    }
  }
  const tree = seen && seen.root ? vcache.lot(seen.root) : null;
  for (const lot of tree ? [tree, ...(tree.descendants || [])] : []) {
    const before = seen.lots[lot.id];
    if (!before) continue;
    if (before.version != null && lot.version != null && lot.version !== before.version) {
      lines.push(`✎ Contenu de ${lot.name} modifié par le TECH depuis la vérif.`);
      needResume(lot.id);
    }
    for (const fresh of (lot.reports || []).filter((report) => !before.reports.includes(report.id))) {
      if (shown.has(fresh.id)) continue;
      shown.add(fresh.id);
      if (fresh.type_name) { if (fresh.comment) typeOf(fresh.type_name).comments.push(fresh.comment); else typeOf(fresh.type_name); }
      else lines.push(`📪 ${lot.name} signalé${fresh.comment ? ` : « ${fresh.comment} »` : ''}.`);
    }
  }
  const typeLines = [];
  for (const type of types.values()) {
    const parts = [];
    if (type.disabled) parts.push('désactivé');
    else if (type.unusable) parts.push(type.unusable > 1 ? `${type.unusable} inutilisables ou périmés` : 'inutilisable');
    if (type.disabled || type.unusable) parts.push('à remplacer');
    if (type.lack) parts.push(`il en manque ${type.lack.required - type.lack.present} (${type.lack.present}/${type.lack.required})`
      + (type.lack.lot ? ` dans ${type.lack.lot}` : ''));
    const signaled = type.comments.length ? ` signalé (« ${type.comments.join(' », « ')} »)` : '';
    if (!signaled && !parts.length) continue;
    typeLines.push(`${parts.length ? '✘' : '📪'} ${type.name}${signaled}${parts.length ? ' : ' + parts.join(', ') : ''}.`);
  }
  // lot incomplet sans manque nomme (cas rare) : on le dit quand meme
  if (!typeLines.length) for (const row of verified) if (row.complete === false) typeLines.push(`✘ ${row.name} incomplet.`);
  lines.unshift(...typeLines);
  if ((report.unknown || []).length) lines.push(`? ${nb(report.unknown.length, 'code inconnu ignoré', 'codes inconnus ignorés')}.`);
  return { issues: lines, resume };
}

async function reviewSent(done) {
  // une vérif en cascade envoyee en plusieurs etapes : un seul bilan
  const groups = new Map();
  for (const { entry, report } of done) {
    const key = entry.group || entry.at;
    if (!groups.has(key)) groups.set(key, { label: entry.label, at: entry.at, issues: [], resume: '', keys: [] });
    const group = groups.get(key);
    const { issues, resume } = sentIssues(entry, report);
    group.issues.push(...issues.filter((line) => !group.issues.includes(line)));
    if (!group.resume) group.resume = resume;
    group.keys.push(...(entry.body.lots || []), ...(entry.body.keys || []));
    if (entry.root) group.label = (vcache.lot(entry.root) || {}).name || group.label;
  }
  for (const group of groups.values()) {
    if (!group.issues.length) continue;
    feedback.warn();
    const resumeLot = group.resume ? vcache.rootOf(group.resume) && (vcache.lot(vcache.rootOf(group.resume)) || {}) : null;
    const choice = await askDialog({
      title: `Vérif de ${group.label} envoyée : à revoir`,
      text: `Faite hors connexion ${fmtOn(group.at)}. Le serveur l'a reçue ; voici ce que vous n'avez pas pu voir sur place :`,
      body: el('ul', { class: 'sent-review' }, group.issues.map((line) => el('li', {}, line))),
      choices: group.resume ? [{ label: `Reprendre la vérif${resumeLot && resumeLot.name ? ' de ' + resumeLot.name : ''}`, value: 'resume', cls: 'primary' }] : [],
      cancel: group.resume ? 'Plus tard' : 'OK',
    });
    if (choice === 'resume') {
      const key = (group.keys.find((row) => row.id === group.resume && row.key)
        || group.keys.find((row) => row.key && vcache.rootOf(row.id) === vcache.rootOf(group.resume)) || {}).key || '';
      if (inHome()) enterScanner(false);
      await launchVerif(group.resume, key);
      return;
    }
  }
}

// Hors connexion ou envois en attente : on reessaie regulierement (une requete reussie vide aussi la file)
setInterval(() => {
  if (outbox.length) flushOutbox();
  else if (offline) api('health/').catch(() => {});
}, 20000);
window.addEventListener('online', () => { if (outbox.length) flushOutbox(); else if (offline) api('health/').catch(() => {}); });
window.addEventListener('offline', () => setOffline(true));
$('#offline-chip').addEventListener('click', explainOutbox);
setTimeout(() => { showOffline(); flushOutbox(); }, 1500);

async function api(path, body) {
  const options = { headers: { Accept: 'application/json' } };
  if (body !== undefined) {
    options.method = 'POST';
    options.headers['Content-Type'] = 'application/json';
    options.body = JSON.stringify(body);
  }
  let response;
  try {
    response = await fetch(new URL(path, API), options);
  } catch (e) {
    setOffline(true);
    throw Object.assign(new Error('Serveur injoignable, vérifiez le réseau.'), { status: 0 });
  }
  let data = null;
  try { data = await response.json(); } catch (e) { /* reponse vide */ }
  // Back arrete ou injoignable derriere Caddy (502, 504), ou maintenance (503, `qrprotec-manage maintenance on`) :
  // comme hors connexion, pour que les vérifs en attente soient gardees et renvoyees plus tard, pas refusees.
  if ([502, 503, 504].includes(response.status)) {
    maintenance = !!(data && data.maintenance);
    if (offline) showOffline(); else setOffline(true);
    throw Object.assign(new Error((data && data.error) || 'Serveur injoignable, réessayez plus tard.'), { status: 0 });
  }
  maintenance = false;
  setOffline(false);
  if (!response.ok) {
    const message = (data && (data.error || data.detail)) || `Erreur ${response.status}`;
    throw Object.assign(new Error(message), { status: response.status, data: data || {} });
  }
  return data;
}

// ------------------------------------------------------------------------------------------------
// Donnees gardees sur le telephone pour ouvrir l'appli sans reseau (IndexedDB : plus de place que localStorage,
// sans bloquer la page). Une seule entree : le cache de vérif. Navigation privee ou stockage refuse : rien de garde.
const savedCache = (() => {
  const DB = 'qrprotec';
  const STORE = 'cache';
  const KEY = 'vcache';
  let opening = null;
  function open() {
    if (!opening) {
      opening = new Promise((resolve, reject) => {
        if (!window.indexedDB) { reject(new Error('IndexedDB indisponible')); return; }
        const request = indexedDB.open(DB, 1);
        request.onupgradeneeded = () => request.result.createObjectStore(STORE);
        request.onsuccess = () => resolve(request.result);
        request.onerror = () => reject(request.error);
      });
      opening.catch(() => { opening = null; });
    }
    return opening;
  }
  async function run(mode, action) {
    const db = await open();
    return new Promise((resolve, reject) => {
      const tx = db.transaction(STORE, mode);
      const request = action(tx.objectStore(STORE));
      tx.oncomplete = () => resolve(request.result);
      tx.onerror = () => reject(tx.error);
      tx.onabort = () => reject(tx.error);
    });
  }
  return {
    load: () => run('readonly', (store) => store.get(KEY)),
    save: (value) => run('readwrite', (store) => store.put(value, KEY)),
  };
})();

// ------------------------------------------------------------------------------------------------
// Cache de vérif (verif_cache.py) : une partie de la reserve n'a ni reseau ni wifi. Des l'ouverture du scanner, le
// detail de tous les lots, de tous les items qu'on peut scanner, des paquets fermes et de tous les types d'items
// est charge en arriere-plan ; chaque scan se traite aussitot depuis ce cache (bip, signalement, desactivation, lot
// ou l'item est range), puis le serveur confirme en arriere-plan et seule une difference est signalee. Les
// enregistrements (« i:iid », « l:lot global », « p:paquet », « t:type ») sont repartis en seaux : on envoie
// l'empreinte de chaque seau, le serveur ne renvoie que les seaux changes. Jamais de cle de lot ni de code de scelle.
// Le cache est aussi garde sur le telephone (IndexedDB) : l'appli ouverte sans reseau (service worker app-sw.js)
// retrouve celui de la derniere mise a jour et la vérif se fait quand meme.
const vcache = (() => {
  const records = new Map();
  const hashes = {}; // seau -> empreinte
  const keysOf = {}; // seau -> cles de ses enregistrements
  let lotRoots = null; // id de lot -> id de son lot global, refait apres chaque mise a jour
  let loading = null;
  let syncedAt = 0;
  const listeners = [];
  const restoring = savedCache.load().then((saved) => {
    if (!saved || syncedAt || !saved.records) return;
    for (const [key, value] of saved.records) records.set(key, value);
    Object.assign(hashes, saved.hashes || {});
    Object.assign(keysOf, saved.keysOf || {});
    syncedAt = saved.syncedAt || 1;
    lotRoots = null;
  }).catch(() => {});

  function save() {
    savedCache.save({ records: [...records], hashes, keysOf, syncedAt }).catch(() => { /* stockage indisponible */ });
  }
  const clone = (value) => (value == null ? null : JSON.parse(JSON.stringify(value)));

  function apply(body) {
    const changed = [];
    for (const [index, bucket] of Object.entries(body.buckets || {})) {
      const fresh = bucket.records || {};
      for (const key of keysOf[index] || []) {
        if (!(key in fresh)) { records.delete(key); changed.push(key); }
      }
      for (const [key, value] of Object.entries(fresh)) {
        const before = records.get(key);
        records.set(key, value);
        if (before === undefined || JSON.stringify(before) !== JSON.stringify(value)) changed.push(key);
      }
      keysOf[index] = Object.keys(fresh);
      hashes[index] = bucket.hash;
    }
    if (changed.some((key) => key.startsWith('l:'))) lotRoots = null;
    return changed;
  }

  // Mise a jour : seulement les seaux changes depuis la derniere. Erreur (hors connexion) : le cache reste tel quel.
  function sync() {
    if (loading) return loading;
    loading = (async () => {
      await restoring;
      const first = !syncedAt;
      try {
        const body = await api('verifs/cache/', { buckets: hashes });
        syncedAt = Date.now();
        const changed = apply(body);
        save(); // meme sans changement : date de la derniere mise a jour
        if (!first && changed.length) for (const listener of listeners) listener(changed);
        return true;
      } catch (e) {
        return false;
      } finally {
        loading = null;
      }
    })();
    return loading;
  }

  function roots() {
    if (lotRoots) return lotRoots;
    lotRoots = new Map();
    for (const [key, lot] of records) {
      if (!key.startsWith('l:')) continue;
      lotRoots.set(lot.id, lot.id);
      for (const row of lot.descendants || []) lotRoots.set(row.id, lot.id);
    }
    return lotRoots;
  }

  // Lignes d'items d'un lot du cache (iids) remplacees par le detail de chaque item, comme lots/<id>/
  function withItems(row) {
    const optional = new Set(row.optional_items || []);
    const expand = (iid) => Object.assign(clone(records.get('i:' + iid)) || { iid }, { optional: optional.has(iid) });
    row.items = (row.items || []).map(expand);
    row.missing_items = (row.missing_items || []).map(expand);
    delete row.optional_items;
    return row;
  }

  return {
    sync,
    ready: () => restoring, // cache garde sur le telephone relu (ouverture de l'appli hors connexion)
    // charge au plus toutes les `age` ms (0 : maintenant), sans attendre
    refresh(age = 0) { if (!loading && Date.now() - syncedAt >= age) sync(); },
    loaded: () => syncedAt > 0,
    syncedAt: () => syncedAt,
    onChange(listener) { listeners.push(listener); },
    item: (iid) => clone(records.get('i:' + iid)),
    pack: (id) => clone(records.get('p:' + id)),
    type: (code) => clone(records.get('t:' + code)),
    rootOf: (id) => roots().get(id) || '',
    // detail du lot global, sous-lots compris, comme lots/<id>/ (sans « departure »)
    lot(id) {
      const lot = clone(records.get('l:' + id));
      if (!lot) return null;
      withItems(lot);
      for (const row of lot.descendants || []) withItems(row);
      return lot;
    },
  };
})();

// Meme format que le front ordinateur (app/src/core/codes.cpp)
function parseCode(raw) {
  const code = raw.trim();
  // etiquette d'une autre antenne (code de l'antenne devant l'iid ou l'id) : refusee, voir readIid / readId
  const foreign = (prefix) => ({ kind: 'foreign', code, prefix });
  // etiquette d'item : l'iid seul (ancien format) ou l'URL <base>/item?id=IID
  // etiquette de taches (TaskItem) : <base>/task?id=IID, un item dont les taches sont a cocher a la verif
  const itemScan = (iid, task = false) => {
    const date = iid.slice(6, 14);
    return { kind: 'item', code, id: iid, type: iid.slice(0, 6), peremption: date === '00000000' ? null : parseDate(date), task };
  };
  const bare = readIid(code);
  if (bare.iid) return itemScan(bare.iid);
  if (bare.foreign) return foreign(bare.foreign);
  let url;
  try { url = new URL(code); } catch (e) {
    // matricule seul (code d'une carte sans l'URL du badge) : badge sans cle, le serveur exige alors le PIN
    return MATRICULE_RE.test(code) ? { kind: 'user', code, id: code, key: '' } : { kind: 'unknown', code };
  }
  const route = url.pathname.replace(/\/+$/, '').split('/').pop();
  const { params: p, foreign: other } = labelParams(route, url.search);
  if (other) return foreign(other);
  if (route === 'item' && expandIid(p.get('id'))) return itemScan(expandIid(p.get('id')));
  if (route === 'task' && expandIid(p.get('id'))) return itemScan(expandIid(p.get('id')), true);
  if (route === 'verif' && p.get('lot')) return { kind: 'lot', code, id: p.get('lot'), key: p.get('key') || '' };
  // badge, ou QR code de connexion temporaire (t) cree par un admin dans l'outil de debug
  if (route === 'badge' && p.get('m')) return { kind: 'user', code, id: p.get('m'), key: p.get('key') || '', temp: p.get('t') || '' };
  // etiquette du paquet ferme, ou etiquette de paquet ouvert (paquet pre-etiquete, avec sa cle o=)
  if (route === 'pack' && p.get('id')) return { kind: 'pack', code, id: p.get('id'), key: p.get('o') || '' };
  if (route === 'seal' && p.get('lot') && p.get('s')) return { kind: 'seal', code, id: p.get('lot'), key: p.get('s') };
  if (route === 'unseal' && p.get('lot') && p.get('c')) return { kind: 'sealopen', code, id: p.get('lot'), key: p.get('c') };
  if (route === 'scanner' && p.get('s') && p.get('k')) return { kind: 'remote', code, search: url.search };
  if (route === 'pinreset' && p.get('m') && p.get('t')) return { kind: 'pinreset', code, id: p.get('m'), key: p.get('t') };
  if (route === 'liste' && p.get('id')) return { kind: 'liste', code, id: p.get('id') };
  return { kind: 'unknown', code };
}

// Parametres d'une adresse d'etiquette (verif?lot=, seal?lot=, unseal?lot=, item?id=, task?id=, pack?id=) avec l'id
// de cette antenne (code de l'antenne retire) ; foreign : code d'une autre antenne, '' sinon
function labelParams(route, search) {
  const params = new URLSearchParams(search);
  const name = ['verif', 'seal', 'unseal'].includes(route) ? 'lot' : ['pack', 'item', 'task'].includes(route) ? 'id' : '';
  if (!name || !params.get(name)) return { params, foreign: '' };
  const value = params.get(name);
  let read;
  if (route === 'item' || route === 'task') {
    const item = readIid(value);
    read = { id: item.iid || value, foreign: item.foreign };
  } else {
    read = readId(value, route === 'pack' ? PACK_ID_LENGTH : LOT_ID_LENGTH);
  }
  if (!read.foreign) params.set(name, read.id);
  return { params, foreign: read.foreign };
}

// Etiquette d'une autre antenne scannee (ou ouverte par son lien) : rien a faire ici
function showForeign(prefix) {
  feedback.bad();
  showInfo('bad', "Étiquette d'une autre antenne", `Code d'antenne ${prefix} : ce matériel n'appartient pas à cette antenne.`);
}
