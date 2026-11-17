/* QRProtec - front web : page d'accueil et navigation entre ses pages. */
'use strict';

// Page d'accueil : connexion (petit lecteur de badge dans un coin), Scanner, Suivi des vérifs, État des lots,
// Gestion de la TECH. Chaque page a son ancre (#lots, #suivi, #stock, #scan) : le bouton retour du telephone
// revient a l'accueil, et un rechargement rouvre la meme page.

const badge = credentials;
const HOME_PAGES = { menu: '', lots: 'lots', live: 'suivi', stock: 'stock', reports: 'signalements', lists: 'listes',
  depart: 'depart', debug: 'debug', stats: 'stats' };
const HOME_TITLES = { menu: 'QRProtec', lots: 'État des lots', live: 'Suivi des vérifs', stock: 'Gestion de la TECH',
  reports: 'Signalements', lists: 'Listes de matériel', depart: 'Départ', debug: 'Debug des codes',
  stats: 'Stats de l\'année' };
// reglages des notifications : seulement pour les utilisateurs qui peuvent en recevoir (gestion et admin)
const hasNotifications = () => !!state.user && (state.user.notifications !== undefined ? !!state.user.notifications : canSeeStock());

let pendingAfterLogin = '';  // page a ouvrir une fois le badge scanne dans le petit lecteur

const inHome = () => document.body.classList.contains('home-mode');
// petit lecteur de badge : sur l'accueil, ou sur une fiche (connexion avant un signalement)
const readerOpen = () => (inHome() || ficheOpen()) && document.body.classList.contains('login-reader');

function pageFromHash(hash) {
  const name = (hash || '').replace(/^#/, '');
  if (name === 'scan') return 'scan';
  return Object.keys(HOME_PAGES).find((page) => HOME_PAGES[page] === name) || 'menu';
}

// Ouvre une page de l'accueil. push : nouvelle entree d'historique (le bouton retour y ramene)
function showHome(page = 'menu', push = true) {
  if (push) {
    history.pushState(null, '', page === 'lists' && lists.id ? listUrl(lists.id)
      : new URL(HOME_PAGES[page] ? './#' + HOME_PAGES[page] : './', document.baseURI).href);
  }
  if (ficheOpen()) closeFiche();
  if (page !== 'lists') { stopListScan(); lists.verifBack = null; }
  state.homePage = page;
  if (!readerOpen()) { cameraWanted = false; syncCamera(); }
  document.body.classList.add('home-mode');
  refreshHome(page);
  if (page === 'debug' && isAdmin() && !debug.data) loadDebug();
  if (page === 'stats' && canSeeStock() && !fun.data) loadFunStats();
  if (page === 'live') watchStart(); else watchStop();
  if (page === 'lists') listsShown(); else { clearInterval(lists.timer); lists.timer = 0; }
  if (page === 'depart') departShown();
  renderHomePage();
  $('#home-pages').scrollTop = 0;
}

// Retour sur une page de l'accueil (← Accueil, retour arriere, retour sur l'onglet) : ses infos sont rechargees,
// sans vider ce qui est deja affiche. Le menu recharge les compteurs de ses tuiles (tâches de la Gestion de la TECH,
// signalements). Les listes, le départ et le suivi rechargent deja a chaque ouverture.
let homeRefreshedAt = 0;
function refreshHome(page = state.homePage || 'menu') {
  homeRefreshedAt = Date.now();
  if (page === 'lots' && state.loading !== 'lots') loadLots();
  if (page === 'stock' && canSeeStock() && state.loading !== 'stock') loadStock();
  if (page === 'reports' && state.user) loadReports();
  if (page === 'menu') {
    if (canSeeStock() && state.loading !== 'stock') loadTasks();
    if (state.user) loadReports(true);
  }
}

// retour sur l'onglet ou l'appli (telephone deverrouille, autre appli quittee) : la page affichee est rechargee.
// pageshow et visibilitychange arrivent ensemble au retour d'une page gardee en cache : un seul rechargement
function refreshShownHome() {
  if (document.hidden || !inHome() || ficheOpen() || gateOpen() || Date.now() - homeRefreshedAt < 3000) return;
  const page = state.homePage || 'menu';
  homeRefreshedAt = Date.now();
  if (page === 'lists') { if (!lists.edit) listsShown(); } else if (page === 'depart') { // sans changer d'onglet
    if (state.loading !== 'lots') loadLots();
    if (state.user) { loadDepartLists(); loadDepartBlocked(); }
  } else refreshHome(page);
}
document.addEventListener('visibilitychange', refreshShownHome);
window.addEventListener('pageshow', (event) => { if (event.persisted) refreshShownHome(); });

// Scanner : l'ecran de vérif habituel, camera demarree (le geste de l'utilisateur debloque aussi le son)
// Cache de vérif mis a jour (seaux changes au serveur) : lot de la vérif en cours remplace par son nouveau detail,
// items et paquets deja scannes compares a leur nouvel etat (seule une difference est signalee)
vcache.onChange((keys) => {
  const changed = new Set(keys);
  let shown = false;
  if (state.lot && changed.has('l:' + state.lotId)) {
    const fresh = vcache.lot(state.lotId);
    if (fresh) { state.lot = fresh; shown = true; }
  }
  for (const entry of [...state.scanned]) {
    if (entry.kind === 'item' && changed.has('i:' + entry.iid) && itemChanged(entry, vcache.item(entry.iid))) shown = true;
  }
  if (shown) { save(); render(); }
});
// scanner ouvert ou vérif en cours : cache tenu a jour toutes les minutes, et des le retour du reseau
setInterval(() => { if (!inHome() || checkRunning()) vcache.refresh(55000); }, 60000);
window.addEventListener('online', () => { if (!inHome() || checkRunning()) vcache.refresh(0); });

function enterScanner(start = true, push = true) {
  if (push) history.pushState(null, '', new URL('./#scan', document.baseURI).href);
  vcache.refresh(55000); // tout pour la vérif, charge en arriere-plan (vérif possible hors connexion)
  watchStop();
  if (ficheOpen()) closeFiche();
  document.body.classList.remove('home-mode', 'login-reader');
  state.homePage = 'scan';
  if (start) startCamera();
  if (state.lotId && !state.lot) loadLot(state.lotId).then(() => { if (state.lot) showLotInfo(); });
  render();
}

// after : page a ouvrir une fois connecte ; action : ce qui attendait la connexion (requireLogin). Chaque demande
// de connexion remplace la precedente : une action annulee ne ressurgit jamais a une connexion lancee pour autre chose
function openReader(after = '', action = null) {
  openGate(after, action);
}

function closeReader() {
  pendingAfterLogin = '';
  loginAction = null;
  lists.mode = false;
  document.body.classList.remove('login-reader', 'list-scan');
  if (inHome() || ficheOpen()) { cameraWanted = false; syncCamera(); }
  renderHomePage();
}
