/* QRProtec - front web : outil de debug des codes (admins). */
'use strict';

// Outil de debug des codes (admins) : page cachee de l'accueil, ouverte par 5 appuis rapides sur « État des
// stocks » de la Gestion de la TECH (comme le mode developpeur d'Android). Affiche le code de toutes les etiquettes
// que le poste a pu imprimer (items, lots, scelles, paquets, listes ; jamais les badges), seul ou en suite a passer
// vite a la douchette. Le serveur (api/debug/codes/) refuse tout autre role : la page cachee n'est pas la securite.

const DEBUG_TAPS = 5;
const DEBUG_TAP_MS = 700;               // ecart maximal entre deux appuis
const DEBUG_AUTO = [0, 1500, 3000];     // suite automatique : arretee, toutes les 1,5 s, toutes les 3 s
const DEBUG_SEARCH_MAX = 150;           // resultats affiches d'une recherche
const DEBUG_STATUS = { active: 'En place', ...ITEM_STATUS };
const DEBUG_CHIPS = { public: 'Publique', key: 'Clé', seal: 'Scellé', sealopen: 'Ouverture' };
const DEBUG_STYLES = { qr: 'QR code', small: 'QR réduit', barcode: 'Code-barres' };

const debug = {
  data: null,         // {lots, items, packs, lists} de api/debug/codes/
  loading: false,
  error: '',
  query: '',
  open: new Set(),    // lots et groupes deplies (liste de leurs items)
  built: '',          // etat de la page deja dessine (le champ de recherche garde le focus aux autres render())
  player: null,       // {title, entries, index, auto, barcode} : code affiche en grand
  timer: 0,
  wakeLock: null,
  qr: new Map(),      // images des QR deja calcules
  lots: new Map(), byLot: new Map(), children: new Map(),
};
let debugTaps = { count: 0, at: 0 };

function debugTap() {
  if (!isAdmin()) return;
  const now = Date.now();
  debugTaps = { count: now - debugTaps.at < DEBUG_TAP_MS ? debugTaps.count + 1 : 1, at: now };
  const left = DEBUG_TAPS - debugTaps.count;
  if (left > 0) {
    if (left <= 2) toast(`Plus que ${nb(left, 'appui')} pour l'outil de debug des codes`);
    return;
  }
  debugTaps = { count: 0, at: 0 };
  showHome('debug');
}

async function loadDebug() {
  if (!isAdmin() || debug.loading) return;
  debug.loading = true;
  debug.error = '';
  renderDebugPage();
  try {
    debug.data = await api('debug/codes/', { user: badge() });
    debugIndex();
  } catch (e) {
    debug.error = e.message;
    if (e.data && e.data.pin_required) { state.user = null; save(); }
  } finally {
    debug.loading = false;
    render();
  }
}

function debugIndex() {
  debug.lots = new Map(debug.data.lots.map((lot) => [lot.id, lot]));
  debug.byLot = new Map();
  debug.children = new Map();
  for (const item of debug.data.items) {
    if (item.status !== 'active') continue;
    if (!debug.byLot.has(item.lot)) debug.byLot.set(item.lot, []);
    debug.byLot.get(item.lot).push(item);
  }
  for (const lot of debug.data.lots) {
    if (!lot.parent || !lot.active) continue;
    if (!debug.children.has(lot.parent)) debug.children.set(lot.parent, []);
    debug.children.get(lot.parent).push(lot);
  }
}

// ---- Codes a afficher : {code, kind, title, lines, barcode}

function debugItemEntry(item) {
  const lot = debug.lots.get(item.lot);
  const place = item.status !== 'active' ? DEBUG_STATUS[item.status] || item.status
    : lot ? lot.name : 'Stock, hors rangement';
  return {
    code: item.code,
    barcode: item.style === 'barcode',
    kind: `${item.tasks ? 'Étiquette de tâches' : "Étiquette d'item"} · ${DEBUG_STYLES[item.style] || item.style}`,
    title: item.type_name,
    lines: [item.short, item.peremption ? `Périme le ${fmtDate(item.peremption)}` : 'Sans péremption',
      place + (item.disabled ? ' · désactivé' : '')],
  };
}

function debugLotEntry(lot, code) {
  return {
    code: code.code,
    kind: code.label,
    title: lot.name,
    lines: [`${lot.type_name} · ${lot.id}`, lot.sealed ? 'Scellé' + (lot.seal_number ? ` n° ${lot.seal_number}` : '') : '',
      lot.active ? '' : 'Lot désactivé'].filter(Boolean),
  };
}

function debugPackEntry(pack) {
  return {
    code: pack.code,
    kind: 'Paquet scellé',
    title: `${pack.type_name} ×${pack.count}`,
    lines: [pack.id, pack.peremption ? `Périme le ${fmtDate(pack.peremption)}` : 'Sans péremption',
      pack.opened ? 'Déjà ouvert' : 'Fermé'],
  };
}

function debugListEntry(list) {
  const status = { draft: 'À préparer', validated: 'Sortie', returned: 'Rentrée' }[list.status] || list.status;
  return { code: list.code, kind: 'Liste de matériel', title: list.name, lines: [list.id, list.template ? 'Modèle' : status] };
}

// Suite d'un lot : son etiquette cle (valable partout, poste et web), puis ses items ; avec les sous-lots, chacun
// suit avec son etiquette cle et ses items, dans l'ordre de l'arbre (comme une vérif en cascade). Dans la cascade,
// un lot scelle ne montre que l'etiquette de son scelle (c'est tout ce que la vérif demande, sans l'ouvrir) : ni
// cle, ni contenu, ni sous-lots ; lance seul, sa cle puis son scelle. Un lot de referentiel (virtual) n'a pas
// d'etiquette, un lot cache dans la cascade (hidden) non plus : seulement leurs sous-lots.
function debugLotSequence(lot, deep) {
  const entries = [];
  const walk = (row, root) => {
    const seal = deep && row.sealed && row.codes.find((code) => code.kind === 'seal');
    const key = row.codes.find((code) => code.kind === 'key');
    if (key && (root || (!row.hidden && !seal))) entries.push(debugLotEntry(row, key));
    if (seal) {
      entries.push(debugLotEntry(row, seal));
      return;
    }
    entries.push(...(debug.byLot.get(row.id) || []).map(debugItemEntry));
    if (deep) (debug.children.get(row.id) || []).forEach((child) => walk(child, false));
  };
  walk(lot, true);
  return entries;
}

// ---- Affichage en grand

function debugQr(code) {
  if (!debug.qr.has(code)) {
    const qr = qrcode(0, 'M');
    qr.addData(code);
    qr.make();
    debug.qr.set(code, qr.createDataURL(8, 32)); // marge blanche de 4 modules
  }
  return el('img', { class: 'debug-qr', src: debug.qr.get(code), alt: code });
}

// Code 128 (jeu B) de l'iid court, comme les etiquettes code-barres du poste (table de web/code128.js, chargee
// seulement quand le serveur a vu passer des codes-barres)
function debugBarcode(text) {
  const patterns = window.Code128.PATTERNS;
  const values = [104, ...[...text].map((char) => char.charCodeAt(0) - 32)];
  const check = values.reduce((sum, value, index) => sum + value * (index || 1), 0) % 103;
  let x = 10;
  let bars = '';
  [...[...values, check, 106].map((value) => patterns[value]).join('')].forEach((width, index) => {
    if (index % 2 === 0) bars += `<rect x="${x}" y="0" width="${width}" height="1"/>`;
    x += Number(width);
  });
  const box = el('div', { class: 'debug-barcode' });
  box.innerHTML = `<svg viewBox="0 0 ${x + 10} 1" preserveAspectRatio="none" shape-rendering="crispEdges">${bars}</svg>`;
  return box;
}

function playDebug(title, entries, code = '') {
  if (!entries.length) { toast('Rien à afficher.'); return; }
  const index = Math.max(0, entries.findIndex((entry) => entry.code === code));
  debug.player = { title, entries, index, auto: 0, barcode: true };
  clearInterval(debug.timer);
  renderDebugPlayer();
  const dialog = $('#debug-player');
  if (!dialog.open) dialog.showModal();
  if ('wakeLock' in navigator && !debug.wakeLock) {
    navigator.wakeLock.request('screen').then((lock) => { debug.wakeLock = lock; }, () => {});
  }
}

function debugStep(step) {
  const player = debug.player;
  if (!player) return;
  const next = player.index + step;
  if (next < 0 || next >= player.entries.length) {
    if (player.auto) debugAuto(0);
    return;
  }
  player.index = next;
  renderDebugPlayer();
}

function debugAuto(delay) {
  clearInterval(debug.timer);
  debug.player.auto = delay;
  if (delay) debug.timer = setInterval(() => debugStep(1), delay);
  renderDebugPlayer();
}

function debugPlayerClosed() {
  clearInterval(debug.timer);
  debug.player = null;
  if (debug.wakeLock) { debug.wakeLock.release().catch(() => {}); debug.wakeLock = null; }
}

function renderDebugPlayer() {
  const player = debug.player;
  if (!player) return;
  const entry = player.entries[player.index];
  const count = player.entries.length;
  const canBarcode = entry.barcode && !!window.Code128;
  const autoNext = DEBUG_AUTO[(DEBUG_AUTO.indexOf(player.auto) + 1) % DEBUG_AUTO.length];
  const close = () => $('#debug-player').close();
  $('#debug-player').replaceChildren(
    el('div', { class: 'debug-head' },
      el('div', {}, el('b', {}, player.title), count > 1 ? el('div', { class: 'hint' }, `${player.index + 1} / ${count}`) : ''),
      el('button', { type: 'button', 'aria-label': 'Fermer', onclick: close }, '✕')),
    el('div', { class: 'debug-code', title: count > 1 ? 'Toucher pour passer au suivant' : '',
      onclick: count > 1 ? () => debugStep(1) : null },
    canBarcode && player.barcode ? debugBarcode(entry.code) : debugQr(entry.code)),
    el('div', { class: 'debug-info' },
      el('div', { class: 'hint' }, entry.kind),
      el('b', {}, entry.title),
      ...entry.lines.map((line) => el('div', {}, line)),
      el('code', { class: 'break' }, entry.code)),
    count > 1 ? el('div', { class: 'debug-nav' },
      el('button', { type: 'button', disabled: player.index === 0 ? '' : null, onclick: () => debugStep(-1) }, '◀ Précédent'),
      el('button', { type: 'button', class: player.auto ? 'active' : '', onclick: () => debugAuto(autoNext) },
        player.auto ? `Auto ${player.auto / 1000} s` : 'Auto'),
      el('button', { type: 'button', class: 'primary', disabled: player.index === count - 1 ? '' : null,
        onclick: () => debugStep(1) }, 'Suivant ▶')) : '',
    canBarcode ? el('button', { type: 'button', class: 'text-link', onclick: () => { player.barcode = !player.barcode; renderDebugPlayer(); } },
      player.barcode ? 'Afficher en QR code' : 'Afficher le code-barres') : '',
    count > 1 ? el('ol', { class: 'debug-seq' }, ...player.entries.map((row, index) => el('li', {
      class: index === player.index ? 'active' : '',
      onclick: () => { player.index = index; renderDebugPlayer(); },
    }, el('span', {}, row.title), el('span', { class: 'hint' }, ` ${row.kind.split(' · ')[0]} · ${row.lines[0] || ''}`)))) : '');
  // seule la liste du bas defile jusqu'au code en cours : le code affiche en haut reste en place
  const list = $('#debug-player .debug-seq');
  const active = list && list.querySelector('li.active');
  if (active) {
    const box = list.getBoundingClientRect();
    const row = active.getBoundingClientRect();
    list.scrollTop += row.top - box.top - (box.height - row.height) / 2;
  }
}

// ---- Page de choix

// Texte cherche : un code scanne (douchette dans le champ, URL ou iid court) est ramene a son identifiant
function debugNeedle(raw) {
  const text = raw.trim();
  if (!text) return '';
  const scan = parseCode(text);
  return (scan.kind !== 'unknown' && scan.kind !== 'user' && scan.id ? scan.id : text).toLowerCase();
}

const debugMatch = (needle, ...fields) => fields.some((field) => field && String(field).toLowerCase().includes(needle));

function debugItemRow(item, title, entries) {
  return el('button', { type: 'button', class: 'debug-item', onclick: () => playDebug(title, entries(), item.code) },
    el('span', {}, item.type_name),
    el('span', { class: 'hint' }, ` ${item.short}${item.peremption ? ' · ' + fmtDate(item.peremption) : ''}`
      + (item.status !== 'active' ? ' · ' + (DEBUG_STATUS[item.status] || item.status) : '')));
}

function debugToggle(key) {
  if (debug.open.has(key)) debug.open.delete(key); else debug.open.add(key);
  renderDebugResults();
}

function debugLotRow(lot, depth = lot.depth) {
  const items = debug.byLot.get(lot.id) || [];
  const deep = (debug.children.get(lot.id) || []).length ? debugLotSequence(lot, true) : [];
  const open = debug.open.has(lot.id);
  const content = () => debugLotSequence(lot, false);
  return el('div', { class: 'debug-lot' + (lot.active ? '' : ' off'), style: `--depth: ${Math.min(depth, 6)}` },
    el('button', { type: 'button', class: 'debug-name', onclick: items.length ? () => debugToggle(lot.id) : null },
      items.length ? (open ? '▾ ' : '▸ ') : '', el('b', {}, lot.name),
      el('span', { class: 'hint' }, ` ${lot.type_name} · ${nb(items.length, 'item')}${lot.sealed ? ' · scellé' : ''}`)),
    el('div', { class: 'debug-chips' },
      ...lot.codes.map((code) => el('button', { type: 'button', onclick: () => playDebug(lot.name, [debugLotEntry(lot, code)]) },
        icon('qr'), code.kind === 'key' && lot.storage ? 'Rangement' : DEBUG_CHIPS[code.kind] || code.label)),
      items.length ? el('button', { type: 'button', class: 'play', onclick: () => playDebug(`${lot.name} : contenu`, content()) },
        `▶ Contenu (${items.length})`) : '',
      deep.length ? el('button', { type: 'button', class: 'play', onclick: () => playDebug(`${lot.name} : avec les sous-lots`, deep) },
        `▶ Avec les sous-lots (${deep.length})`) : ''),
    open ? el('div', { class: 'debug-items' }, ...items.map((item) => debugItemRow(item, `${lot.name} : contenu`, content))) : '');
}

// Groupe d'items (stock hors rangement par type, items sortis par statut) : ▶ pour tous, depliable
function debugGroup(key, title, items) {
  const open = debug.open.has(key);
  const entries = () => items.map(debugItemEntry);
  return el('div', { class: 'debug-lot' },
    el('button', { type: 'button', class: 'debug-name', onclick: () => debugToggle(key) },
      open ? '▾ ' : '▸ ', el('b', {}, title), el('span', { class: 'hint' }, ` ${nb(items.length, 'item')}`)),
    el('div', { class: 'debug-chips' },
      el('button', { type: 'button', class: 'play', onclick: () => playDebug(title, entries()) }, `▶ Tous (${items.length})`)),
    open ? el('div', { class: 'debug-items' }, ...items.map((item) => debugItemRow(item, title, entries))) : '');
}

function groupBy(items, keyOf) {
  const groups = new Map();
  for (const item of items) {
    const key = keyOf(item);
    if (!groups.has(key)) groups.set(key, []);
    groups.get(key).push(item);
  }
  return groups;
}

// Connexion de depannage : un QR code temporaire par secouriste (pas sa carte), cree a la demande
function debugUsersParts(users, title = 'Cartes des secouristes (dépannage)') {
  if (!users.length) return [];
  return [el('h3', {}, title),
    el('p', { class: 'hint' }, 'Un secouriste a oublié sa carte : touchez son nom et faites-lui scanner le QR code. '
      + 'Il vaut 1 h ; le téléphone qui le scanne reste connecté 24 h. Pas de QR code pour les administrateurs.'),
    el('div', { class: 'debug-chips' }, ...users.map((user) => el('button', { type: 'button',
      disabled: user.pin_blocked ? '' : null, title: user.pin_blocked ? 'PIN bloqué : réinitialisez-le d\'abord' : '',
      onclick: () => showTempLogin(user) }, icon('badge'), user.name)))];
}

async function showTempLogin(user) {
  const dialog = $('#temp-login');
  const body = $('#temp-login-body');
  body.replaceChildren(el('h2', {}, user.name), el('p', { class: 'hint' }, 'Création du QR code…'));
  if (!dialog.open) dialog.showModal();
  try {
    const data = await api('debug/temp-login/', { user: badge(), matricule: user.matricule });
    const until = new Date(data.expires);
    body.replaceChildren(
      el('h2', {}, `Connexion de ${data.name}`),
      debugQr(data.url),
      el('p', {}, el('b', {}, 'Scannez ce QR code avec le téléphone du secouriste'),
        ' (dans QRprotec, ou avec l\'appareil photo).'),
      el('p', { class: 'hint' }, `Valable jusqu'à ${until.toLocaleTimeString('fr-FR', { hour: '2-digit', minute: '2-digit' })}`
        + ' (1 h). Le téléphone reste connecté 24 h, sans carte ni code PIN.'));
  } catch (e) {
    body.replaceChildren(el('h2', {}, user.name), el('p', { class: 'pin-error' }, e.message));
  }
}
$('#temp-login-close').addEventListener('click', () => $('#temp-login').close());

function debugBrowseParts() {
  const data = debug.data;
  const parts = [...debugUsersParts(data.users || []), el('h3', {}, 'Lots et rangements')];
  data.lots.filter((lot) => lot.active).forEach((lot) => parts.push(debugLotRow(lot)));
  const loose = debug.byLot.get('') || [];
  parts.push(el('h3', {}, 'Stock hors rangement'));
  if (!loose.length) parts.push(el('p', { class: 'hint' }, 'Aucun item.'));
  else {
    const all = () => loose.map(debugItemEntry);
    parts.push(el('div', { class: 'debug-chips' },
      el('button', { type: 'button', class: 'play', onclick: () => playDebug('Stock hors rangement', all()) }, `▶ Tout le stock (${loose.length})`)));
    groupBy(loose, (item) => item.type_name).forEach((items, name) => parts.push(debugGroup('stock:' + name, name, items)));
  }
  const packs = data.packs.filter((pack) => !pack.opened);
  parts.push(el('h3', {}, 'Paquets scellés fermés'));
  if (!packs.length) parts.push(el('p', { class: 'hint' }, 'Aucun paquet fermé.'));
  else {
    parts.push(el('div', { class: 'debug-chips' },
      el('button', { type: 'button', class: 'play', onclick: () => playDebug('Paquets scellés', packs.map(debugPackEntry)) }, `▶ Tous (${packs.length})`),
      ...packs.map((pack) => el('button', { type: 'button', onclick: () => playDebug('Paquets scellés', packs.map(debugPackEntry), pack.code) },
        icon('qr'), `${pack.type_name} ×${pack.count}`))));
  }
  parts.push(el('h3', {}, 'Listes de matériel'));
  // listes en cours (a preparer, sorties) et modeles visibles ; les listes rentrees (terminees) sont repliees
  // derriere un bouton, sinon elles s'accumulent. Une liste supprimee n'existe plus en base : jamais listee.
  const listChips = (rows, title) => el('div', { class: 'debug-chips' }, ...rows.map((list) => el('button', { type: 'button',
    onclick: () => playDebug(title, rows.map(debugListEntry), list.code) }, icon('qr'), list.name)));
  const doneLists = data.lists.filter((list) => !list.template && list.status === 'returned');
  const liveLists = data.lists.filter((list) => !doneLists.includes(list));
  if (!liveLists.length) parts.push(el('p', { class: 'hint' }, doneLists.length ? 'Aucune liste en cours.' : 'Aucune liste.'));
  else parts.push(listChips(liveLists, 'Listes de matériel'));
  if (doneLists.length) {
    const key = 'lists:done';
    const open = debug.open.has(key);
    parts.push(el('button', { type: 'button', class: 'debug-name', onclick: () => debugToggle(key) },
      open ? '▾ ' : '▸ ', 'Listes terminées', el('span', { class: 'hint' }, ` ${nb(doneLists.length, 'liste rentrée', 'listes rentrées')}`)));
    if (open) parts.push(listChips(doneLists, 'Listes terminées'));
  }
  // le reste : items sortis, hors lot, disparus, remplaces, supprimes ; lots desactives ; paquets ouverts
  const others = data.items.filter((item) => item.status !== 'active');
  const offLots = data.lots.filter((lot) => !lot.active);
  const opened = data.packs.filter((pack) => pack.opened);
  if (others.length || offLots.length || opened.length) {
    const key = 'others';
    const open = debug.open.has(key);
    parts.push(el('h3', {}, el('button', { type: 'button', class: 'debug-name', onclick: () => debugToggle(key) },
      open ? '▾ ' : '▸ ', 'Hors service', el('span', { class: 'hint' },
        ` ${nb(others.length, 'item')}, ${nb(offLots.length, 'lot désactivé', 'lots désactivés')}, ${nb(opened.length, 'paquet ouvert', 'paquets ouverts')}`))));
    if (open) {
      groupBy(others, (item) => item.status).forEach((items, status) => parts.push(debugGroup('status:' + status, DEBUG_STATUS[status] || status, items)));
      offLots.forEach((lot) => parts.push(debugLotRow(lot, 0)));
      if (opened.length) {
        parts.push(el('div', { class: 'debug-chips' }, ...opened.map((pack) => el('button', { type: 'button',
          onclick: () => playDebug('Paquets ouverts', opened.map(debugPackEntry), pack.code) }, icon('qr'), `${pack.type_name} ×${pack.count}`))));
      }
    }
  }
  return parts;
}

function debugSearchParts(needle) {
  const data = debug.data;
  const lots = data.lots.filter((lot) => debugMatch(needle, lot.id, lot.name, lot.type_name, lot.seal_number));
  const items = data.items.filter((item) => debugMatch(needle, item.iid, item.short, item.type_name, item.type));
  const packs = data.packs.filter((pack) => debugMatch(needle, pack.id, pack.type_name));
  const lists = data.lists.filter((list) => debugMatch(needle, list.id, list.name));
  const users = (data.users || []).filter((user) => debugMatch(needle, user.name, user.matricule));
  if (!lots.length && !items.length && !packs.length && !lists.length && !users.length) return [el('p', { class: 'hint' }, 'Aucun code trouvé.')];
  const parts = debugUsersParts(users, 'Secouristes (dépannage)');
  if (lots.length) {
    parts.push(el('h3', {}, nb(lots.length, 'lot')));
    lots.slice(0, DEBUG_SEARCH_MAX).forEach((lot) => parts.push(debugLotRow(lot, 0)));
  }
  if (items.length) {
    const entries = () => items.map(debugItemEntry);
    parts.push(el('div', { class: 'row-head' }, el('h3', {}, nb(items.length, 'item')),
      el('button', { type: 'button', class: 'play', onclick: () => playDebug('Recherche', entries()) }, `▶ Tous (${items.length})`)));
    parts.push(el('div', { class: 'debug-items' }, ...items.slice(0, DEBUG_SEARCH_MAX).map((item) => debugItemRow(item, 'Recherche', entries))));
    if (items.length > DEBUG_SEARCH_MAX) parts.push(el('p', { class: 'hint' }, `… et ${items.length - DEBUG_SEARCH_MAX} autres (▶ Tous les affiche).`));
  }
  if (packs.length || lists.length) {
    parts.push(el('h3', {}, 'Paquets et listes'));
    parts.push(el('div', { class: 'debug-chips' },
      ...packs.map((pack) => el('button', { type: 'button', onclick: () => playDebug('Paquets scellés', [debugPackEntry(pack)]) },
        icon('qr'), `${pack.type_name} ×${pack.count}`)),
      ...lists.map((list) => el('button', { type: 'button', onclick: () => playDebug('Listes de matériel', [debugListEntry(list)]) },
        icon('qr'), list.name))));
  }
  return parts;
}

function renderDebugResults() {
  const box = $('#debug-results');
  if (!box || !debug.data) return;
  const needle = debugNeedle(debug.query);
  box.replaceChildren(...(needle ? debugSearchParts(needle) : debugBrowseParts()));
}

// Entree dans le champ (fin d'un scan a la douchette) : un seul item ou paquet trouve s'affiche en grand, et le texte
// est selectionne pour que le scan suivant le remplace
function debugSearchEnter(input) {
  const needle = debugNeedle(input.value);
  input.select();
  if (!needle || !debug.data) return;
  const items = debug.data.items.filter((item) => debugMatch(needle, item.iid, item.short));
  if (items.length === 1) playDebug('Recherche', [debugItemEntry(items[0])]);
}

function renderDebugPage() {
  const view = $('#home-debug');
  const built = `${isAdmin()}|${debug.loading}|${debug.error}|${!!debug.data}`;
  if (debug.built === built && view.childElementCount) return;
  debug.built = built;
  if (!isAdmin()) {
    view.replaceChildren(el('p', {}, 'Réservé aux administrateurs : scannez votre badge.'));
    return;
  }
  const parts = [
    el('div', { class: 'row-head' }, el('h3', {}, 'Codes des étiquettes'),
      el('button', { type: 'button', onclick: loadDebug }, icon('refresh'), debug.loading ? 'Chargement…' : 'Actualiser')),
    el('p', { class: 'hint' }, 'Outil de debug des administrateurs : le code de toutes les étiquettes imprimables, sauf '
      + 'les badges (un QR code de connexion temporaire les remplace). Touchez un code pour l\'afficher en grand, ou ▶ pour enchaîner un contenu : touchez le code, '
      + 'Suivant ou la flèche → pour passer au suivant.'),
  ];
  if (debug.error) parts.push(el('p', { class: 'pin-error' }, debug.error));
  if (!debug.data) {
    if (debug.loading) parts.push(el('p', { class: 'hint' }, 'Chargement…'));
    view.replaceChildren(...parts);
    return;
  }
  const search = el('input', { type: 'search', class: 'debug-search', enterkeyhint: 'search', autocomplete: 'off',
    placeholder: 'Chercher (nom, type, iid, lot) ou scanner un code' });
  search.value = debug.query;
  search.addEventListener('input', () => { debug.query = search.value; renderDebugResults(); });
  search.addEventListener('keydown', (event) => {
    if (event.key === 'Enter') { event.preventDefault(); debugSearchEnter(search); }
  });
  parts.push(search, el('div', { id: 'debug-results' }));
  view.replaceChildren(...parts);
  renderDebugResults();
}

$('#debug-player').addEventListener('close', debugPlayerClosed);
// fleches du clavier (le focus quitte le dialogue a chaque code redessine : ecoute sur tout le document)
document.addEventListener('keydown', (event) => {
  if (!debug.player) return;
  if (['ArrowRight', 'ArrowDown', 'PageDown'].includes(event.key)) { event.preventDefault(); debugStep(1); }
  if (['ArrowLeft', 'ArrowUp', 'PageUp'].includes(event.key)) { event.preventDefault(); debugStep(-1); }
});
