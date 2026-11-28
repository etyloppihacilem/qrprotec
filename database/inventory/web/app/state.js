/* QRProtec - front web mobile.
 *
 * Tiers haut : camera (reductible en vignette flottante) et lecture des codes dans le viseur (camera-scan.js : BarcodeDetector
 * natif si disponible, sinon zxing-cpp en WebAssembly).
 * Reste de l'ecran : derniere information scannee et onglets "A scanner" / "Scannes" / "Lot".
 * Toutes les ecritures passent par l'API publique : la verif exige la cle du lot (etiquette privee)
 * et un badge utilisateur.
 *
 * Le front est decoupe en scripts classiques (web/app/*.js), charges dans l'ordre par index.html (defer) : ils
 * partagent leurs declarations de premier niveau (constantes, etat, fonctions), comme les morceaux d'un seul
 * fichier. Au chargement, un script ne doit appeler que des fonctions des scripts precedents ou du sien : tout
 * le reste passe par des evenements, et la page ne demarre qu'a la fin du dernier script (boot.js).
 *
 * Ce script : constantes, etat de la page (state) et sa sauvegarde (save, restore).
 */
'use strict';

const API = new URL('api/', document.baseURI);
// matricule seul accepte comme badge (meme format que MATRICULE_RE de views/common.py et is_matricule de codes.cpp)
const MATRICULE_RE = /^[A-Za-z0-9_-]{1,16}$/;

// Code de l'antenne (2 lettres majuscules ou chiffres, ex : PC) devant l'iid et l'id du lot ou du paquet dans chaque
// code imprime, ecrit dans la page par le serveur (vide : aucun). A la lecture : notre code est retire, un code sans
// prefixe (etiquette imprimee avant) est a nous, celui d'une autre antenne est refuse. Meme regle que read_iid /
// read_id de models.py et codes.cpp.
const CODE_PREFIX = (document.querySelector('meta[name="qrprotec-code-prefix"]') || {}).content || '';
const CODE_PREFIX_RE = /^[A-Z0-9]{2}$/;
const LOT_ID_LENGTH = 14;  // type de lot (6) + compteur base62 (8)
const PACK_ID_LENGTH = 8;  // compteur base62 des paquets fermes

// iid complet d'un iid sans prefixe, eventuellement en forme courte (D : sans date, N : zeros de tete du compteur),
// '' sinon. serphy20261029N7 = serphy2026102900000007, lecglyDN6 = lecgly0000000000000006
function expandPlainIid(code) {
  const match = /^([A-Za-z0-9]{6})(\d{8}|D)(N[A-Za-z0-9]{1,6}|[A-Za-z0-9]{8})$/.exec(code || '');
  if (!match) return '';
  const counter = match[3].length < 8 ? match[3].slice(1).padStart(8, '0') : match[3];
  return match[1] + (match[2] === 'D' ? '00000000' : match[2]) + counter;
}
// { iid, foreign } d'un code d'item : iid complet local (prefixe retire), ou foreign = code de l'autre antenne
function readIid(code) {
  code = code || '';
  if (CODE_PREFIX && code.startsWith(CODE_PREFIX)) {
    const iid = expandPlainIid(code.slice(CODE_PREFIX.length));
    if (iid) return { iid, foreign: '' };
  }
  if (expandPlainIid(code)) return { iid: expandPlainIid(code), foreign: '' };
  const prefix = code.slice(0, 2);
  return { iid: '', foreign: CODE_PREFIX_RE.test(prefix) && expandPlainIid(code.slice(2)) ? prefix : '' };
}
// iid complet d'un item de cette antenne, '' sinon (pas un iid, ou iid d'une autre antenne)
function expandIid(code) { return readIid(code).iid; }
// { id, foreign } d'un id de lot (LOT_ID_LENGTH) ou de paquet (PACK_ID_LENGTH) lu dans un code ; un id d'une autre
// longueur est rendu tel quel (ancienne etiquette)
function readId(code, length) {
  code = code || '';
  const prefix = code.slice(0, 2);
  if (code.length === length + 2 && CODE_PREFIX_RE.test(prefix)) {
    return prefix === CODE_PREFIX ? { id: code.slice(2), foreign: '' } : { id: '', foreign: prefix };
  }
  return { id: code, foreign: '' };
}
const REPEAT_DELAY_MS = 2500; // un meme code vu en continu par la camera n'est traite qu'une fois
const STORAGE_USER = 'qrprotec.user';
const NAME_SESSION_MS = 11.5 * 3600 * 1000;  // connexion par nom et PIN : jeton de session valable 12h
const STORAGE_SESSION = 'qrprotec.session';
// « Web partagé » (tablettes de l'antenne) : reglage de cet appareil, coche par la gestion ou un admin, garde dans les
// donnees persistantes du site (localStorage, protege par navigator.storage.persist). Coche : la connexion n'est
// gardee que dans l'onglet (sessionStorage, perdue a la fermeture de l'appli) et finit apres 1h sans activite.
const STORAGE_SHARED_WEB = 'qrprotec.shared-web';
const STORAGE_ACTIVITY = 'qrprotec.activity'; // derniere activite de la personne connectee (web partage)
const SHARED_IDLE_MS = 60 * 60 * 1000;
// PIN : 6 chiffres au moins pour un nouveau code, pas de maximum (64 au plus) ; a la connexion, les anciens codes de
// 4 ou 5 chiffres restent valables (models.PIN_RE / NEW_PIN_RE)
const PIN_LOGIN_RE = /^\d{4,64}$/;
const NEW_PIN_RE = /^\d{6,64}$/;
const NEW_PIN_ERROR = 'Le PIN doit comporter au moins 6 chiffres.';
const STORAGE_PIN_RESET = 'qrprotec.pin-reset';  // lien de deblocage en attente du badge de l'admin
const STORAGE_NAME = 'qrprotec.declared-name';   // nom declare (sans badge), si le serveur l'autorise
const STORAGE_FICHE_KEY = 'qrprotec.fiche-key';
// « Bulle de scan » du dernier connecte sur ce telephone (reglage web scan_bubble) : vaut aussi avant la connexion,
// pour montrer ou non le bouton ↘ qui reduit la camera de l'ecran de scan dans un coin
const STORAGE_SCAN_BUBBLE = 'qrprotec.scan-bubble';
// Vérif en cours oubliee au rechargement de la page (ou a l'ouverture d'une etiquette hors du lecteur) apres
// 20 minutes sans activite : on ne retombe pas sur le lot d'une vérif commencee il y a une heure.
const SESSION_TIMEOUT_MS = 20 * 60 * 1000;

const $ = (selector) => document.querySelector(selector);

// ------------------------------------------------------------------------------------------------
// Etat

const state = {
  user: null,        // {matricule, key, prenom, privileged}
  lot: null,         // detail public du lot (items attendus, exigences, sous-lots)
  lotId: '',
  lotKey: '',
  extra: [],         // autres lots du meme lot global ajoutes a la verif par leur etiquette privee : [{id, key, lot}]
  scanned: [],       // [{code, kind, iid, info, expired, error, items: [iids d'un paquet], at: heure du scan}]
  startedAt: '',     // debut de la vérif (etiquette du lot scannee) ou de l'etape en cours d'une vérif en cascade
  tab: 'todo',
  busy: false,
  lastVerif: null,   // {lotId, at, complete, present} : derniere verif validee (affichee tant qu'on ne rescanne pas)
  lots: null,        // liste des lots (page « État des lots »)
  showStorage: false, // rangements du stock dans la liste des lots (affiches par defaut en gestion et admin)
  storageRole: null, // role pour lequel showStorage a pris sa valeur par defaut (comme au poste)
  lotFolds: new Map(), // etat des lots : id -> sous-lots replies (true) ou deplies (false), choisi a la main
  stock: null,       // etat des stocks, roles gestion et admin uniquement
  tasks: null,       // liste de taches de la gestion (meme acces que l'etat des stocks)
  forecast: null,    // previsions de stock (meme acces que l'etat des stocks)
  horizon: 1,        // index dans HORIZONS des previsions affichees
  pinReset: null,    // {matricule, token} : deblocage du PIN d'un utilisateur, en attente d'un admin connecte
  loading: '',       // 'lots' ou 'stock' pendant un chargement
  homePage: 'menu',  // page affichee : 'menu', 'lots', 'live', 'stock' (accueil) ou 'scan' (scanner)
  events: null,      // evenements a venir de la gestion (onglet « À venir », separe des taches)
  eventsDays: 90,    // horizon des evenements a venir (30, 90 ou 365 jours)
  eventsKind: '',    // filtre des evenements a venir par genre ('' : tout)
  homeView: 'tasks', // page « Gestion de la TECH » : 'tasks' (à faire), 'events' (à venir), 'stock' (etat actuel) ou 'forecast'
  reports: null,     // signalements : {counts, reports, can_manage}
  reportsShow: 'open', // 'open' (en cours) ou 'all'
  reportsBusy: false,
  push: null,        // notifications web de ce navigateur (admins) : {subscribed, stock_low, stock_empty, ...}
  pushBusy: false,
  pushEndpoint: '', // abonnement de ce navigateur (pour le reperer dans la liste)
  pushDevices: null, // gestionnaire des notifications : {types, devices} de l'utilisateur connecte
  pushError: '',
  declaredAllowed: false, // reglage du serveur : verif et ajout possibles sans badge, sous un nom declare
  serverVersion: null, // version de release du serveur (/api/health/) : null inconnue, '' hors paquet
  restockRecommends: true, // reglage du serveur : un reassort passe le lot en verif recommandee (coche par defaut)
  declaredName: '',       // nom declare sur ce navigateur (non verifie)
  guest: null,            // vérif à plusieurs sans compte : prenom de l'invite ('' : « Invité »), null sinon
  expiredSession: false,  // vérif precedente oubliee au demarrage (SESSION_TIMEOUT_MS)
  waitingTeam: false,     // vérif à plusieurs : partie validee, en attente des autres participants
  launched: '',           // lot dont la vérif a ete lancee depuis sa fiche (etiquette publique)
  full: false,            // « vérif complète » (bouton des admins) : items et sous-lots facultatifs obligatoires
  current: '',            // lot dont l'etiquette (publique ou cle) a ete scannee en dernier pendant la vérif : les
                          // items venus du stock y sont ranges (services.assign_items)
  askPlace: false,        // vérif rejointe sans rien scanner : demander le sous-lot ou l'on se trouve
  // Vérif en cascade (lot global avec des sous-lots, ex : VPS > armoires > sacs) : la vérif est celle du lot global
  // (state.lot). On entre dans un sous-lot en scannant son etiquette, on le valide a part, et l'on revient au lot qui
  // le contient, ou il passe en vert.
  path: [],               // sous-lots ou l'on est entre, du lot global vers le lot en cours (vide : lot global)
  done: {},               // lots valides pendant cette vérif : {id: {at, by, complete, sealed}}
  keys: {},               // etiquettes cles de sous-lots scannees pendant la vérif : {id: cle}
  labels: [],             // lots dont l'etiquette (publique ou cle) a ete scannee pendant la vérif (labelMissing)
  entered: [],            // lots ou l'on est entre : leurs sous-lots non faits sont laisses de cote, pas manquants
  reopened: {},           // lots valides rouverts (« Le refaire ») : {id: validation d'avant}, rendue si l'on en ressort sans scan
  startedIn: '',          // sous-lot dont on a lance la vérif (ex : un kit dans le VPS)
  openSeals: [],          // lots scelles dont on a choisi de verifier le contenu (sealWaiting)
  // Reassort d'une vérif incomplete (restock.js) : {stage: 'list' (liste de courses) ou 'scan' (scan du reassort),
  // lots: [ids], level: lot ou l'on validait (cascade), end: reassort de fin de vérif en cascade}, null sinon
  restock: null,
  restockChecked: {},     // liste de courses : types pris au stock {type: qui} (partages entre participants)
  restockLater: [],       // vérif en cascade : lots dont le reassort se fait a la fin (partages)
  restockLocal: false,    // liste changee ici sans connexion a la vérif commune : renvoyee a la connexion
};

// roles gestion et admin : acces en lecture a l'etat des stocks
const canSeeStock = () => !!(state.user && (state.user.role === 'gestion' || state.user.role === 'admin' || state.user.privileged));
const isAdmin = () => !!(state.user && state.user.role === 'admin');
// iids et identifiants de lots : affiches au TECH (admin) seulement, les autres n'en ont pas besoin
const showIds = isAdmin;
// fiches de lots detaillees (comptes, revisions, lot global, raisons d'une vérif recommandee) : gestion et admin ;
// secouristes et chefs n'ont que l'etat, sa date et la prochaine peremption, le contenu replie quand tout est valide
const detailed = canSeeStock;
const idText = (id) => (showIds() && id ? id : '');
const withId = (name, id) => (showIds() && id ? `${name} (${id})` : name);
// nature d'un lot sous son nom : genre d'un rangement du stock (« Pièce », « Armoire », « Rangement »), type sinon
const lotKind = (lot) => (lot.storage_kind ? lot.storage_kind.charAt(0).toUpperCase() + lot.storage_kind.slice(1)
  : lot.lot_type_name);
const ROLE_LABELS = { normal: 'Secouriste', chef: 'Chef', gestion: 'Gestion', admin: 'Administrateur' };
// identite des operations sur un lot : le badge, sinon le nom declare (si le serveur l'autorise)
const declared = () => !state.user && state.declaredAllowed && !!state.declaredName;
// invite d'une vérif à plusieurs (secouriste d'une autre antenne, sans compte), supervise par un secouriste connecte
const guestInTeam = () => !state.user && state.guest !== null && live.inTeam();
const hasIdentity = () => !!state.user || declared() || guestInTeam();
// identifiants de l'utilisateur connecte : la cle du badge, ou seulement le jeton de session apres une connexion
// par nom et PIN (le serveur accepte l'un ou l'autre)
const credentials = () => ({ matricule: state.user.matricule, key: state.user.key, session: state.user.session });
const identity = () => (state.user ? { user: credentials() }
  : guestInTeam() ? { guest: true, name: state.guest } : { name: state.declaredName });

function sharedWeb() {
  try { return localStorage.getItem(STORAGE_SHARED_WEB) === '1'; } catch (e) { return false; }
}
function lastActivity() {
  try { return Number(sessionStorage.getItem(STORAGE_ACTIVITY)) || 0; } catch (e) { return 0; }
}
// Activite de la personne connectee (toucher, clavier, douchette) : retient l'heure, au plus une fois par minute
let activityAt = 0;
function noteActivity() {
  const now = Date.now();
  if (now - activityAt < 60 * 1000) return;
  activityAt = now;
  try { sessionStorage.setItem(STORAGE_ACTIVITY, String(now)); } catch (e) { /* ignore */ }
}

function save() {
  try {
    localStorage.setItem(STORAGE_SESSION, JSON.stringify({
      at: Date.now(), // derniere activite (SESSION_TIMEOUT_MS)
      lotId: state.lotId, lotKey: state.lotKey, lastVerif: state.lastVerif,
      launched: state.launched, current: state.current, full: state.full, fullHidden: state.fullHidden,
      path: state.path, done: state.done, keys: state.keys, entered: state.entered, reopened: state.reopened, startedIn: state.startedIn,
      labels: state.labels, openSeals: state.openSeals, startedAt: state.startedAt,
      restock: state.restock, restockChecked: state.restockChecked, restockLater: state.restockLater,
      extra: state.extra.map(({ id, key }) => ({ id, key })),
      scanned: state.scanned.map(({ code, kind, iid, key, info, expired, error, items, place, at }) => ({ code, kind, iid, key, info, expired, error, items, place, at })),
    }));
    // web partage : la connexion reste dans l'onglet, jamais dans les donnees gardees de l'appareil
    const kept = sharedWeb() ? sessionStorage : localStorage;
    const other = kept === localStorage ? sessionStorage : localStorage;
    other.removeItem(STORAGE_USER);
    if (state.user) kept.setItem(STORAGE_USER, JSON.stringify(state.user));
    else kept.removeItem(STORAGE_USER);
    if (state.declaredName) localStorage.setItem(STORAGE_NAME, state.declaredName);
    else localStorage.removeItem(STORAGE_NAME);
  } catch (e) { /* stockage indisponible (navigation privee) : la page fonctionne sans */ }
}

function restore() {
  try {
    const shared = sharedWeb();
    const user = JSON.parse((shared ? sessionStorage : localStorage).getItem(STORAGE_USER) || 'null');
    // web partage : connexion oubliee apres 1h sans activite, meme si l'onglet est reste ouvert
    const idle = shared && user && Date.now() - lastActivity() >= SHARED_IDLE_MS;
    // connexion par nom et PIN : pas de cle de badge, le jeton de session (12h) est le seul identifiant
    // connexion de depannage (QR code temporaire d'un admin) : 24h (sessionMs donne par le serveur)
    const sessionOk = user && (user.key || (user.loginAt && Date.now() - user.loginAt < (user.sessionMs || NAME_SESSION_MS)));
    if (user && sessionOk && !idle && user.key_expires && new Date(user.key_expires) >= today()) state.user = user;
    state.declaredName = localStorage.getItem(STORAGE_NAME) || '';
    const session = JSON.parse(localStorage.getItem(STORAGE_SESSION) || 'null');
    const fresh = session && session.at && Date.now() - session.at < SESSION_TIMEOUT_MS;
    if (session && !fresh && (session.lotId || (session.scanned || []).length)) state.expiredSession = true;
    if (fresh) {
      state.lotId = session.lotId || '';
      state.lotKey = session.lotKey || '';
      state.launched = session.launched || '';
      state.full = !!session.full;
      state.fullHidden = !!session.fullHidden;
      state.current = session.current || '';
      state.path = Array.isArray(session.path) ? session.path : [];
      state.done = session.done && typeof session.done === 'object' ? session.done : {};
      state.keys = session.keys && typeof session.keys === 'object' ? session.keys : {};
      state.labels = Array.isArray(session.labels) ? session.labels : [];
      state.openSeals = Array.isArray(session.openSeals) ? session.openSeals : [];
      state.entered = Array.isArray(session.entered) ? session.entered : [];
      state.reopened = session.reopened && typeof session.reopened === 'object' ? session.reopened : {};
      state.startedIn = typeof session.startedIn === 'string' ? session.startedIn : '';
      state.startedAt = typeof session.startedAt === 'string' ? session.startedAt : '';
      state.restock = session.restock && typeof session.restock === 'object' && Array.isArray(session.restock.lots) ? session.restock : null;
      state.restockChecked = session.restockChecked && typeof session.restockChecked === 'object' ? session.restockChecked : {};
      state.restockLater = Array.isArray(session.restockLater) ? session.restockLater : [];
      state.scanned = Array.isArray(session.scanned) ? session.scanned : [];
      state.lastVerif = session.lastVerif || null;
      state.extra = Array.isArray(session.extra) ? session.extra.map(({ id, key }) => ({ id, key, lot: null })) : [];
    }
  } catch (e) { /* ignore */ }
  if (state.expiredSession) save(); // vérif oubliee : on ne la signale qu'une fois
  try { state.pinReset = JSON.parse(sessionStorage.getItem(STORAGE_PIN_RESET) || 'null'); } catch (e) { /* ignore */ }
}
