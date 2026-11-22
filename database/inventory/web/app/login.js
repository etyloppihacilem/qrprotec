/* QRProtec - front web : grande fenetre de connexion (badge, nom et PIN), web partage. */
'use strict';

// Grande fenetre de connexion : pour les secouristes qui ne font pas encore le lien entre leur carte et le
// telephone. Elle prend tout l'ecran au debut d'une vérif (et pour toute connexion depuis l'accueil) : la camera au
// milieu, « Scannez votre carte », et la connexion par nom.

const gateOpen = () => document.body.classList.contains('login-gate');

function openGate(after = '', action = null) {
  pendingAfterLogin = after;
  setLoginAction(action);
  // derniere connexion de ce telephone par passkey (pas sur un web partage) : la passkey d'abord (web/app/passkey.js)
  if (passkeyFirst()) { tryPasskeyFirst(() => showGate(after)); return; }
  showGate(after);
}

function showGate(after) {
  // vérif : la connexion est conseillee, pas imposee (secouriste d'une autre antenne sans compte, supervise par un
  // secouriste connecte) ; il peut continuer en invite
  const verif = after === 'verif' || (!inHome() && !ficheOpen());
  $('#login-gate-text').textContent = verif ? 'Connectez-vous pour que votre vérif soit à votre nom.' : 'Connectez-vous pour continuer.';
  $('#login-gate-later').textContent = verif ? 'Continuer sans compte (invité)' : 'Annuler';
  document.body.classList.add('login-gate');
  $('#login-gate').hidden = false;
  placeGateCamera();
  $('#scan-hint').textContent = 'Scannez votre carte';
  startCamera();
  if (inHome()) renderHomePage();
}

// done : connexion reussie (sinon fermee par « Continuer sans compte (invité) » ou « Annuler »)
function closeGate(done = false) {
  if (!gateOpen()) return;
  document.body.classList.remove('login-gate');
  $('#login-gate').hidden = true;
  if (!done) { pendingAfterLogin = ''; loginAction = null; }
  if (inHome() || ficheOpen()) { cameraWanted = false; syncCamera(); }
  render();
  if (inHome()) renderHomePage();
}

// la camera (#camera, toujours au meme endroit du DOM) se pose sur l'emplacement reserve de la fenetre
function placeGateCamera() {
  if (!gateOpen()) return;
  const box = $('#login-gate-cam').getBoundingClientRect();
  const style = document.body.style;
  style.setProperty('--gate-left', `${box.left}px`);
  style.setProperty('--gate-top', `${box.top}px`);
  style.setProperty('--gate-width', `${box.width}px`);
  style.setProperty('--gate-height', `${box.height}px`);
}
window.addEventListener('resize', placeGateCamera);
$('#login-gate').addEventListener('scroll', placeGateCamera);

$('#login-gate-name').addEventListener('click', () => openNameLogin());
$('#login-gate-later').addEventListener('click', () => closeGate());

$('#reader-close').addEventListener('click', (event) => { event.stopPropagation(); closeReader(); });
$('#reader-name').addEventListener('click', (event) => { event.stopPropagation(); openNameLogin(); });

// Badge scanne : le petit lecteur se ferme et la page demandee s'ouvre. La carte scannee hors de l'appli
// (badge?m=...) ou dans le scanner vide (aucun lot, aucun scan) mene aussi a l'accueil : apres un scan
// (item seul, hors vérif compris), on reste dans le scanner.
function afterLogin() {
  const next = pendingAfterLogin === 'verif' ? '' : pendingAfterLogin; // vérif lancee : on y reste
  if (gateOpen()) closeGate(true);
  if (readerOpen()) closeReader();
  if (ficheOpen()) { // connexion depuis une fiche (signalement) : on y reste, avec les actions du role
    openFiche(fiche.kind, fiche.id, { key: fiche.key, seal: fiche.seal, push: false, label: fiche.label });
    return;
  }
  if (next === 'stock' && !canSeeStock()) toast("La gestion de la TECH est réservée aux rôles gestion et admin.", true);
  if (next) showHome(next === 'stock' && !canSeeStock() ? 'menu' : next);
  else if (!inHome() && !verifInProgress() && !state.lot && !state.scanned.length
    && !state.pinReset) showHome('menu');
  else if (inHome()) showHome(state.homePage || 'menu', false);
}

function logout(message) {
  state.user = null;
  toast(message || 'Déconnecté.');
  save();
  live.identity();
  state.reports = null;
  lists.index = null;
  lists.list = null;
  Object.assign(depart, { lists: null, tab: 'lots' });
  if (inHome() && ['live', 'stock', 'reports', 'lists'].includes(state.homePage)) showHome('menu', false);
  else if (settingsOpen()) renderSettings(); // reglages ouverts : sections des connectes masquees
  render();
}

// Web partage : deconnexion apres 1 h sans activite, verifiee chaque minute et avant de compter un nouveau geste
// (la tablette en veille suspend les minuteries : le premier toucher au reveil deconnecte d'abord)
function checkSharedIdle() {
  if (!state.user || !sharedWeb() || Date.now() - lastActivity() < SHARED_IDLE_MS) return;
  logout('Déconnecté après 1 h sans activité.');
}
function userActive() {
  checkSharedIdle();
  noteActivity();
}
for (const type of ['pointerdown', 'keydown']) document.addEventListener(type, userActive, { capture: true, passive: true });
document.addEventListener('visibilitychange', () => { if (!document.hidden) checkSharedIdle(); });
setInterval(checkSharedIdle, 60 * 1000);

$('#home-user').addEventListener('click', async () => {
  if (state.user) {
    if (await confirmDialog(`Déconnecter ${state.user.prenom} ?`, '', 'Se déconnecter')) logout();
  } else if (readerOpen()) closeReader();
  else openReader();
});

// La fleche du haut ramene toujours a l'accueil, jamais a la page precedente (Hippolyte, 2026-11-10) : l'historique
// repasserait par chaque liste ouverte (meme supprimee). Pour revenir en arriere, « ← Toutes les listes » et autres.
$('#home-back').addEventListener('click', () => { lists.id = ''; lists.list = null; showHome('menu'); });
$('#go-home').addEventListener('click', (event) => { event.stopPropagation(); showHome('menu'); });
document.querySelectorAll('.tile').forEach((tile) => tile.addEventListener('click', () => {
  const page = tile.dataset.go;
  if (page === 'scan') { enterScanner(); return; }
  if (page === 'live' && !state.user) { openReader('live'); toast('Scannez votre badge pour suivre les vérifs.'); return; }
  if (page === 'stock' && !state.user) { openReader('stock'); toast('Scannez votre badge pour voir les stocks.'); return; }
  if (page === 'reports' && !state.user) { openReader('reports'); toast('Scannez votre badge pour suivre les signalements.'); return; }
  if (page === 'lists') { lists.id = ''; lists.list = null; }
  if (page === 'lists' && !state.user) { openReader('lists'); toast('Scannez votre badge pour voir les listes de matériel.'); return; }
  if (page === 'stock' && !canSeeStock()) { toast("La gestion de la TECH est réservée aux rôles gestion et admin.", true); return; }
  showHome(page);
}));
// bouton retour du telephone : l'adresse dit quoi afficher (fiche d'un lot ou d'un item, lecteur, page de l'accueil)
window.addEventListener('popstate', () => {
  // editeur de liste ouvert : le retour arriere le ferme (comme « ← Toutes les listes »), la page sous lui revient
  listEditorPopped();
  const route = location.pathname.replace(/\/+$/, '').split('/').pop();
  const params = new URLSearchParams(location.search);
  if (route === 'verif' && params.get('lot')) { openFiche('lot', params.get('lot'), { push: false }); return; }
  if (route === 'seal' && params.get('lot') && params.get('s')) {
    openFiche('lot', params.get('lot'), { seal: params.get('s'), push: false });
    return;
  }
  if ((route === 'item' || route === 'task') && expandIid(params.get('id'))) { openFiche('item', expandIid(params.get('id')), { push: false }); return; }
  if (route === 'liste' && params.get('id')) { closeFiche(); openList(params.get('id'), false); return; }
  closeFiche();
  lists.id = '';
  const page = route === 'verif' ? 'scan' : pageFromHash(location.hash);
  if (page === 'scan') enterScanner(true, false);
  else showHome(page, false);
});

// Page d'un lot (depuis l'etat des lots ou le suivi) : sa fiche
function openLot(id) {
  openFiche('lot', id);
}

// Lots caches dans la hierarchie (Lots.hidden_in_verif, ex : le B+ du VPS) : retires de l'arbre, leurs sous-lots
// remontent dans le lot qui les contient (parent et profondeur). Seule la gestion des lots du poste montre la vraie
// hierarchie. rows : lignes avec id, parent, depth et hidden, dans n'importe quel ordre.
function withoutHidden(rows) {
  if (!rows.some((row) => row.hidden)) return rows;
  const byId = new Map(rows.map((row) => [row.id, row]));
  const shown = (id) => {
    let parent = byId.get(id);
    for (let guard = 0; parent && parent.hidden && guard < 50; guard += 1) parent = byId.get(parent.parent);
    return parent ? parent.id : id; // parent hors de la liste : garde tel quel
  };
  return rows.filter((row) => !row.hidden).map((row) => {
    if (row.parent == null) return row;
    let depth = 0;
    for (let parent = byId.get(row.parent), guard = 0; parent && guard < 50; parent = byId.get(parent.parent), guard += 1) {
      if (!parent.hidden) depth += 1;
    }
    return Object.assign({}, row, { parent: shown(row.parent), depth: byId.has(row.parent) ? depth : row.depth });
  });
}

async function loadLots() {
  state.loading = 'lots';
  render();
  try {
    // liste publique ; le badge, s'il y en a un, est verifie
    state.lots = withoutHidden(await api('lots/summary/', state.user ? { user: badge() } : {}));
  } catch (e) {
    if (e.status !== 0) toast('Lots : ' + e.message, true); // hors connexion : la mention du scanner le dit deja
    if (e.status === 403) { state.user = null; save(); }
  } finally {
    state.loading = '';
    render();
  }
}

async function loadStock() {
  if (!canSeeStock()) return;
  state.loading = 'stock';
  render();
  try {
    const [stock, forecast, tasks, events] = await Promise.all([
      api('stock/summary/', { user: badge() }),
      api('stock/forecast/summary/', { user: badge(), months: 6 }).catch(() => null),
      api('tasks/summary/', { user: badge() }).catch(() => null),
      api('events/summary/', { user: badge(), days: state.eventsDays || 90 }).catch(() => null),
    ]);
    state.stock = stock;
    state.forecast = forecast;
    state.tasks = tasks;
    state.events = events;
  } catch (e) {
    toast('Gestion : ' + e.message, true);
    if (e.data && e.data.pin_required) { state.user = null; save(); } // session expiree : rescanner le badge
  } finally {
    state.loading = '';
    render();
  }
}

// tâches seules (compteur de la tuile « Gestion de la TECH » du menu), sans « Chargement… » ni message d'erreur
let tasksBusy = false;
async function loadTasks() {
  if (!canSeeStock() || tasksBusy) return;
  tasksBusy = true;
  try {
    const tasks = await api('tasks/summary/', { user: badge() });
    if (canSeeStock()) state.tasks = tasks;
  } catch (e) {
    // pas de toast : la tuile garde l'ancien compteur
  } finally {
    tasksBusy = false;
    if (inHome()) renderHomePage();
  }
}

async function chooseLot(id, tab = 'todo') {
  if (!(state.lot && lotById(id))) {
    if (state.scanned.length && !await confirmDialog('Changer de lot ?', 'Les items déjà scannés restent dans la liste.', 'Changer de lot')) return;
    state.lotKey = '';
    state.lastVerif = null;
    state.extra = [];
    resetCascade();
  }
  await loadLot(id);
  if (!state.lot) return;
  if (inCascade() && tab === 'todo') enterLevel(id);
  if (inHome()) enterScanner(false);
  showLotInfo();
  switchTab(tab);
}

function renderHomePage() {
  const page = state.homePage || 'menu';
  document.querySelectorAll('.home-page').forEach((node) => node.classList.toggle('active', node.dataset.page === page));
  $('#home-title').textContent = HOME_TITLES[page] || 'QRProtec';
  $('#home-back').hidden = page === 'menu';
  $('#home-settings').hidden = page === 'settings';
  const userButton = $('#home-user');
  userButton.textContent = state.user ? `👤 ${state.user.prenom}` : readerOpen() ? 'Annuler' : 'Se connecter';
  userButton.classList.toggle('ok', !!state.user);
  // connecte avec une passkey : petit bouton du QR code de connexion au poste (web/app/passkey.js)
  const posteQr = $('#home-poste-qr');
  posteQr.hidden = typeof posteQrAllowed !== 'function' || !posteQrAllowed();
  if (!posteQr.hidden && !posteQr.firstChild) posteQr.append(icon('qr'));
  const stockTile = $('#tile-stock');
  stockTile.classList.toggle('disabled', !!state.user && !canSeeStock());
  stockTile.setAttribute('aria-disabled', String(!!state.user && !canSeeStock()));
  $('#tile-stock-sub').textContent = !state.user ? 'Connexion requise' : canSeeStock() ? (state.tasks ? (state.tasks.count ? `${nb(state.tasks.count, 'tâche')} à faire` : 'Rien à faire')
    : 'Tâches, stock et prévisions')
    : 'Réservé gestion et admin';
  const counts = state.reports && state.reports.counts;
  $('#tile-reports-sub').textContent = !state.user ? 'Connexion requise'
    : counts && isAdmin() && counts.new ? `${nb(counts.new, 'nouveau', 'nouveaux')} à traiter`
      : counts && counts.new + counts.progress ? `${counts.new + counts.progress} en cours` : 'En cours et résolus';
  $('#tile-reports').classList.toggle('alert', !!(counts && isAdmin() && counts.new));
  // boite aux lettres : fermee sans connexion, ouverte et vide sans signalement, drapeau leve pour de nouveaux
  // signalements a traiter (admin), lettre dedans pour des signalements en cours
  $('#tile-reports-icon').textContent = !state.user ? '📪' : counts && isAdmin() && counts.new ? '📫'
    : counts && counts.new + counts.progress ? '📬' : '📭';
  stockTile.classList.toggle('alert', !!(canSeeStock() && state.tasks && state.tasks.categories.some((row) => row.level === 'bad')));
  if (page === 'lots') renderLotsPage();
  if (page === 'live') renderLive();
  if (page === 'reports') renderReports();
  if (page === 'lists') renderListsPage();
  if (page === 'depart') renderDepartPage();
  if (page === 'debug') renderDebugPage();
  if (page === 'stats') renderFunStats();
}

// Lignes des lots de state.lots (page « État des lots », onglet Lots de « Départ ») : etat, derniere vérif,
// sous-lots replies sous un lot valide. rerender redessine la page apres un depli. flat : resultat de recherche,
// sans retrait ni depli, le lot parent ecrit en dessous. pick (page « Départ ») : { lead(lot), click(lot),
// note(lot), cls(lot) } case devant le lot, toucher la ligne, ligne d'explication et classe en plus.
function lotRows(rerender, pick = null) {
  // lot global : son etat est celui de l'ensemble de ses lots
  const statusOf = (lot) => (!lot.depth && lot.global ? { kind: lot.global.kind, label: lot.global.label } : lotStatus(lot));
  // sous-lots replies par defaut sous un lot valide (lui et tous ses sous-lots en vert), deplies sinon ;
  // le petit carre [+] / [-] du lot les deroule ou les replie a la main
  const children = new Map();
  state.lots.forEach((lot) => {
    if (lot.parent == null) return;
    if (!children.has(lot.parent)) children.set(lot.parent, []);
    children.get(lot.parent).push(lot);
  });
  const valid = new Map();
  const allValid = (lot) => {
    if (!valid.has(lot.id)) {
      valid.set(lot.id, statusOf(lot).kind === 'ok' && (children.get(lot.id) || []).every(allValid));
    }
    return valid.get(lot.id);
  };
  // rangements du stock : deplies par defaut (on les affiche pour voir ce qu'ils contiennent)
  const folded = (lot) => (state.lotFolds.has(lot.id) ? state.lotFolds.get(lot.id) : !lot.storage && allValid(lot));
  const byId = new Map(state.lots.map((lot) => [lot.id, lot]));
  const hidden = (lot) => {
    for (let parent = byId.get(lot.parent); parent; parent = byId.get(parent.parent)) {
      if (folded(parent)) return true;
    }
    return false;
  };
  const lotRow = (lot, flat = false) => {
    const top = !lot.depth && lot.global;
    const status = statusOf(lot);
    const verif = top ? lot.global.last_verif : lot.last_verif;
    const subs = children.get(lot.id);
    const fold = subs && el('button', { type: 'button', class: 'lot-fold' + (folded(lot) ? '' : ' open'),
      title: folded(lot) ? 'Afficher les sous-lots' : 'Replier les sous-lots',
      'aria-label': folded(lot) ? 'Afficher les sous-lots' : 'Replier les sous-lots',
      onclick: (event) => { event.stopPropagation(); state.lotFolds.set(lot.id, !folded(lot)); rerender(); } });
    const indent = !flat && lot.depth;
    const note = pick && pick.note(lot);
    return el('li', { class: status.kind + (pick ? ' ' + pick.cls(lot) : ''),
      style: indent ? `padding-left:${12 + lot.depth * 16}px` : null,
      onclick: () => (pick ? pick.click(lot) : openLot(lot.id)) },
      (!flat && fold) || null,
      pick ? pick.lead(lot) : null,
      el('div', { class: 'main' },
        el('div', { class: 'name' }, (indent ? '└ ' : '') + lot.name + (lot.id === state.lotId ? ' (en cours)' : '')),
        el('div', { class: 'sub' }, `${flat && lot.parent_name ? `dans ${lot.parent_name} · ` : ''}${lotKind(lot)}`
          + `${top ? ' · lot global' : ''} · vérif${top ? ' la plus ancienne' : ''} : ` + (verif ? fmtDateTime(verif) : 'jamais')),
        lot.out_with ? el('div', { class: 'sub out-with', title: `Liste « ${lot.out_with} »` }, '🚑 En poste') : null,
        note ? el('div', { class: 'sub pick-note' }, note) : null),
      el('span', { class: 'lot-state ' + status.kind }, stateBadge(status.kind), stateText(status.label).split(',')[0]),
      pick ? el('button', { type: 'button', class: 'pick-fiche', title: 'Ouvrir la fiche du lot', 'aria-label': 'Ouvrir la fiche du lot',
        onclick: (event) => { event.stopPropagation(); openLot(lot.id); } }, '›') : null);
  };
  return { lotRow, hidden };
}

function renderLotsPage() {
  const view = $('#home-lots');
  const parts = [el('div', { class: 'row-head' }, el('h3', {}, 'Lots'),
    el('button', { type: 'button', onclick: loadLots }, icon('refresh'), state.loading === 'lots' ? 'Chargement…' : 'Actualiser'))];
  if (!state.lots) {
    parts.push(el('p', { class: 'hint' }, state.loading === 'lots' ? 'Chargement des lots…' : 'Liste non chargée.'));
    view.replaceChildren(...parts);
    return;
  }
  parts.push(el('p', { class: 'hint' }, "Touchez un lot pour l'ouvrir. Un lot global affiche l'état de l'ensemble de ses sous-lots."));
  const { lotRow, hidden } = lotRows(renderLotsPage);
  // rangements du stock dans leur propre tableau, sous celui des lots, seulement si la case est cochee (sinon
  // seul le rangement en cours reste dans le tableau des lots)
  // case cochee par defaut en gestion et admin, decochee sinon (de nouveau a chaque changement de role)
  if (canSeeStock() !== state.storageRole) {
    state.storageRole = canSeeStock();
    state.showStorage = state.storageRole;
  }
  const lots = state.lots.filter((lot) => (!lot.storage || (!state.showStorage && lot.id === state.lotId)) && !hidden(lot));
  parts.push(el('ul', { class: 'list' }, ...lots.map((lot) => lotRow(lot))));
  parts.push(el('label', { class: 'check' },
    el('input', { type: 'checkbox', checked: state.showStorage ? '' : null,
      onchange: (event) => { state.showStorage = event.target.checked; renderLotsPage(); } }),
    'Afficher les rangements du stock'));
  if (state.showStorage) {
    const storage = state.lots.filter((lot) => lot.storage && !hidden(lot));
    parts.push(el('div', { class: 'row-head storage-head' }, el('h3', {}, 'Rangements du stock')));
    parts.push(storage.length ? el('ul', { class: 'list' }, ...storage.map((lot) => lotRow(lot)))
      : el('p', { class: 'hint' }, 'Aucun rangement du stock.'));
  }
  view.replaceChildren(...parts);
}
