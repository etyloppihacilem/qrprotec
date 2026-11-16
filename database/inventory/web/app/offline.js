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

function showOffline() {
  const chip = $('#offline-chip');
  if (!chip) return;
  const waiting = outbox.length;
  chip.hidden = !offline && !waiting;
  chip.classList.toggle('pending', !offline && waiting > 0);
  const state = maintenance ? 'Maintenance en cours' : 'Hors connexion';
  chip.textContent = offline ? (waiting ? `${state} · ${nb(waiting, 'envoi')} en attente` : state)
    : `Envoi en cours… (${waiting})`;
}

// Reponse (ou non) du serveur a une requete : la mention suit, et le retour du reseau vide la file d'attente
function setOffline(value) {
  if (offline === value) return;
  offline = value;
  showOffline();
  if (!value && outbox.length) setTimeout(flushOutbox, 0);
}

// Vérif (ou etape d'une vérif en cascade) envoyee au serveur. Hors connexion, ou si des envois attendent deja (l'ordre
// compte : etapes de la cascade), elle est mise en file. Renvoie le compte rendu du serveur, ou null si en attente.
async function sendVerif(body, label) {
  if (!outbox.length) {
    try {
      return await api('verifs/', body);
    } catch (e) {
      if (e.status !== 0) throw e;
    }
  }
  outbox.push({ body, label, at: new Date().toISOString() });
  saveOutbox();
  showOffline();
  return null;
}

// File d'attente renvoyee dans l'ordre. Refusee par le serveur (etiquette cle perimee, scelle brise entre-temps...) :
// retiree de la file et signalee, a refaire.
async function flushOutbox() {
  if (outboxBusy || !outbox.length) return;
  outboxBusy = true;
  showOffline();
  let sent = 0;
  try {
    while (outbox.length) {
      const next = outbox[0];
      try {
        await api('verifs/', next.body);
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
    toast(outbox.length ? `${nb(sent, 'vérif envoyée', 'vérifs envoyées')}, ${outbox.length} en attente.`
      : `Réseau retrouvé : ${nb(sent, 'vérif hors connexion envoyée', 'vérifs hors connexion envoyées')}.`);
    vcache.refresh(0);
  }
}

// Hors connexion ou envois en attente : on reessaie regulierement (une requete reussie vide aussi la file)
setInterval(() => {
  if (outbox.length) flushOutbox();
  else if (offline) api('health/').catch(() => {});
}, 20000);
window.addEventListener('online', () => { if (outbox.length) flushOutbox(); else if (offline) api('health/').catch(() => {}); });
window.addEventListener('offline', () => setOffline(true));
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
  // etiquette d'item : l'iid seul (ancien format) ou l'URL <base>/item?id=IID
  // etiquette de taches (TaskItem) : <base>/task?id=IID, un item dont les taches sont a cocher a la verif
  const itemScan = (iid, task = false) => {
    const date = iid.slice(6, 14);
    return { kind: 'item', code, id: iid, type: iid.slice(0, 6), peremption: date === '00000000' ? null : parseDate(date), task };
  };
  if (expandIid(code)) return itemScan(expandIid(code));
  let url;
  try { url = new URL(code); } catch (e) {
    // matricule seul (code d'une carte sans l'URL du badge) : badge sans cle, le serveur exige alors le PIN
    return MATRICULE_RE.test(code) ? { kind: 'user', code, id: code, key: '' } : { kind: 'unknown', code };
  }
  const route = url.pathname.replace(/\/+$/, '').split('/').pop();
  const p = url.searchParams;
  if (route === 'item' && expandIid(p.get('id'))) return itemScan(expandIid(p.get('id')));
  if (route === 'task' && expandIid(p.get('id'))) return itemScan(expandIid(p.get('id')), true);
  if (route === 'verif' && p.get('lot')) return { kind: 'lot', code, id: p.get('lot'), key: p.get('key') || '' };
  // badge, ou QR code de connexion temporaire (t) cree par un admin dans l'outil de debug
  if (route === 'badge' && p.get('m')) return { kind: 'user', code, id: p.get('m'), key: p.get('key') || '', temp: p.get('t') || '' };
  if (route === 'pack' && p.get('id')) return { kind: 'pack', code, id: p.get('id') };
  if (route === 'seal' && p.get('lot') && p.get('s')) return { kind: 'seal', code, id: p.get('lot'), key: p.get('s') };
  if (route === 'unseal' && p.get('lot') && p.get('c')) return { kind: 'sealopen', code, id: p.get('lot'), key: p.get('c') };
  if (route === 'scanner' && p.get('s') && p.get('k')) return { kind: 'remote', code, search: url.search };
  if (route === 'pinreset' && p.get('m') && p.get('t')) return { kind: 'pinreset', code, id: p.get('m'), key: p.get('t') };
  if (route === 'liste' && p.get('id')) return { kind: 'liste', code, id: p.get('id') };
  return { kind: 'unknown', code };
}
