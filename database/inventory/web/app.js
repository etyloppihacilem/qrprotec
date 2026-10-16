/* QRProtec - front web mobile.
 *
 * Tiers haut : camera (reductible en vignette flottante) et detection des QR codes (BarcodeDetector natif si disponible, sinon jsQR).
 * Reste de l'ecran : derniere information scannee et onglets "A scanner" / "Scannes" / "Lot".
 * Toutes les ecritures passent par l'API publique : la verif exige la cle du lot (etiquette privee)
 * et un badge utilisateur.
 */
'use strict';

(() => {
  const API = new URL('api/', document.baseURI);
  const IID_RE = /^[A-Za-z0-9]{6}\d{8}[A-Za-z0-9]{8}$/;
  const REPEAT_DELAY_MS = 2500; // un meme code vu en continu par la camera n'est traite qu'une fois
  const STORAGE_USER = 'qrprotec.user';
  const NAME_SESSION_MS = 11.5 * 3600 * 1000;  // connexion par nom et PIN : jeton de session valable 12 h
  const STORAGE_SESSION = 'qrprotec.session';
  const STORAGE_PIN_RESET = 'qrprotec.pin-reset';  // lien de deblocage en attente du badge de l'admin
  const STORAGE_NAME = 'qrprotec.declared-name';   // nom declare (sans badge), si le serveur l'autorise
  const STORAGE_FICHE_KEY = 'qrprotec.fiche-key';  // etiquette privee ouverte (fiche du lot), gardee au rechargement
  // Vérif en cours oubliee au rechargement de la page (ou a l'ouverture d'une etiquette hors du lecteur) apres
  // 20 minutes sans activite : on ne retombe pas sur le lot d'une vérif commencee il y a une heure.
  const SESSION_TIMEOUT_MS = 20 * 60 * 1000;

  const $ = (selector) => document.querySelector(selector);

  // ------------------------------------------------------------------------------------------------
  // Etat

  const state = {
    user: null,        // {matricule, key, nom, prenom, privileged}
    lot: null,         // detail public du lot (items attendus, exigences, sous-lots)
    lotId: '',
    lotKey: '',
    extra: [],         // autres lots du meme lot global ajoutes a la verif par leur etiquette privee : [{id, key, lot}]
    scanned: [],       // [{code, kind, iid, info, expired, error, items: [iids d'un paquet]}]
    tab: 'todo',
    busy: false,
    lastVerif: null,   // {lotId, at, complete, present} : derniere verif validee (affichee tant qu'on ne rescanne pas)
    lots: null,        // liste des lots (page « État des lots »)
    showStorage: false, // rangements du stock dans la liste des lots (masques par defaut)
    stock: null,       // etat des stocks, roles gestion et admin uniquement
    forecast: null,    // previsions de stock (meme acces que l'etat des stocks)
    horizon: 1,        // index dans HORIZONS des previsions affichees
    pinReset: null,    // {matricule, token} : deblocage du PIN d'un utilisateur, en attente d'un admin connecte
    loading: '',       // 'lots' ou 'stock' pendant un chargement
    homePage: 'menu',  // page affichee : 'menu', 'lots', 'live', 'stock' (accueil) ou 'scan' (scanner)
    homeForecast: false,
    reports: null,     // signalements : {counts, reports, can_manage}
    reportsShow: 'open', // 'open' (en cours) ou 'all'
    reportsBusy: false, // page « État des stocks » de l'accueil : previsions plutot que l'etat actuel
    push: null,        // notifications web de ce navigateur (admins) : {subscribed, stock_low, stock_empty, ...}
    pushBusy: false,
    pushEndpoint: '', // abonnement de ce navigateur (pour le reperer dans la liste)
    pushDevices: null, // gestionnaire des notifications : {types, devices} de l'utilisateur connecte
    pushError: '',
    declaredAllowed: false, // reglage du serveur : verif et ajout possibles sans badge, sous un nom declare
    declaredName: '',       // nom declare sur ce navigateur (non verifie)
    pendingOpening: null,   // {id, key, at} : scelle ouvert par son etiquette sans identite, a signer a la connexion
    expiredSession: false,  // vérif precedente oubliee au demarrage (SESSION_TIMEOUT_MS)
    waitingTeam: false,     // vérif à plusieurs : partie validee, en attente des autres participants
    launched: '',           // lot dont la vérif a ete lancee depuis sa fiche (etiquette publique)
    current: '',            // lot dont l'etiquette (publique ou cle) a ete scannee en dernier pendant la vérif : les
                            // items venus du stock y sont ranges (services.assign_items)
    askPlace: false,        // vérif rejointe sans rien scanner : demander le sous-lot ou l'on se trouve
    // Vérif en cascade (lot global avec des sous-lots, ex : VPS > armoires > sacs) : la vérif est celle du lot global
    // (state.lot). On entre dans un sous-lot en scannant son etiquette, on le valide a part, et l'on revient au lot qui
    // le contient, ou il passe en vert.
    path: [],               // sous-lots ou l'on est entre, du lot global vers le lot en cours (vide : lot global)
    done: {},               // lots valides pendant cette vérif : {id: {at, by, complete, sealed}}
    keys: {},               // etiquettes cles de sous-lots scannees pendant la vérif : {id: cle}
    entered: [],            // lots ou l'on est entre : leurs sous-lots non faits sont laisses de cote, pas manquants
    startedIn: '',          // sous-lot dont on a lance la vérif (ex : un kit dans le VPS)
  };

  // roles gestion et admin : acces en lecture a l'etat des stocks
  const canSeeStock = () => !!(state.user && (state.user.role === 'gestion' || state.user.role === 'admin' || state.user.privileged));
  const isAdmin = () => !!(state.user && state.user.role === 'admin');
  const ROLE_LABELS = { normal: 'Secouriste', gestion: 'Gestion', admin: 'Administrateur' };
  // identite des operations sur un lot : le badge, sinon le nom declare (si le serveur l'autorise)
  const declared = () => !state.user && state.declaredAllowed && !!state.declaredName;
  const hasIdentity = () => !!state.user || declared();
  // identifiants de l'utilisateur connecte : la cle du badge, ou seulement le jeton de session apres une connexion
  // par nom et PIN (le serveur accepte l'un ou l'autre)
  const credentials = () => ({ matricule: state.user.matricule, key: state.user.key, session: state.user.session });
  const identity = () => (state.user ? { user: credentials() } : { name: state.declaredName });

  function save() {
    try {
      localStorage.setItem(STORAGE_SESSION, JSON.stringify({
        at: Date.now(), // derniere activite (SESSION_TIMEOUT_MS)
        lotId: state.lotId, lotKey: state.lotKey, lastVerif: state.lastVerif, pendingOpening: state.pendingOpening,
        launched: state.launched, current: state.current,
        path: state.path, done: state.done, keys: state.keys, entered: state.entered, startedIn: state.startedIn,
        extra: state.extra.map(({ id, key }) => ({ id, key })),
        scanned: state.scanned.map(({ code, kind, iid, key, info, expired, error, items, place }) => ({ code, kind, iid, key, info, expired, error, items, place })),
      }));
      if (state.user) localStorage.setItem(STORAGE_USER, JSON.stringify(state.user));
      else localStorage.removeItem(STORAGE_USER);
      if (state.declaredName) localStorage.setItem(STORAGE_NAME, state.declaredName);
      else localStorage.removeItem(STORAGE_NAME);
    } catch (e) { /* stockage indisponible (navigation privee) : la page fonctionne sans */ }
  }

  function restore() {
    try {
      const user = JSON.parse(localStorage.getItem(STORAGE_USER) || 'null');
      // connexion par nom et PIN : pas de cle de badge, le jeton de session (12 h) est le seul identifiant
      const sessionOk = user && (user.key || (user.loginAt && Date.now() - user.loginAt < NAME_SESSION_MS));
      if (user && sessionOk && user.key_expires && new Date(user.key_expires) >= today()) state.user = user;
      state.declaredName = localStorage.getItem(STORAGE_NAME) || '';
      const session = JSON.parse(localStorage.getItem(STORAGE_SESSION) || 'null');
      if (session) state.pendingOpening = session.pendingOpening || null; // delai propre (OPENING_SIGN_DELAY)
      const fresh = session && session.at && Date.now() - session.at < SESSION_TIMEOUT_MS;
      if (session && !fresh && (session.lotId || (session.scanned || []).length)) state.expiredSession = true;
      if (fresh) {
        state.lotId = session.lotId || '';
        state.lotKey = session.lotKey || '';
        state.launched = session.launched || '';
        state.current = session.current || '';
        state.path = Array.isArray(session.path) ? session.path : [];
        state.done = session.done && typeof session.done === 'object' ? session.done : {};
        state.keys = session.keys && typeof session.keys === 'object' ? session.keys : {};
        state.entered = Array.isArray(session.entered) ? session.entered : [];
        state.startedIn = typeof session.startedIn === 'string' ? session.startedIn : '';
        state.scanned = Array.isArray(session.scanned) ? session.scanned : [];
        state.lastVerif = session.lastVerif || null;
        state.extra = Array.isArray(session.extra) ? session.extra.map(({ id, key }) => ({ id, key, lot: null })) : [];
      }
    } catch (e) { /* ignore */ }
    if (state.expiredSession) save(); // vérif oubliee : on ne la signale qu'une fois
    try { state.pinReset = JSON.parse(sessionStorage.getItem(STORAGE_PIN_RESET) || 'null'); } catch (e) { /* ignore */ }
  }

  // ------------------------------------------------------------------------------------------------
  // Outils

  function today() { const d = new Date(); d.setHours(0, 0, 0, 0); return d; }

  function parseDate(value) {
    if (!value) return null;
    const m = /^(\d{4})-?(\d{2})-?(\d{2})/.exec(value);
    if (!m) return null;
    const d = new Date(+m[1], +m[2] - 1, +m[3]);
    return isNaN(d) ? null : d;
  }

  function fmtDate(value) {
    const d = value instanceof Date ? value : parseDate(value);
    return d ? d.toLocaleDateString('fr-FR') : '–';
  }

  // Date et heure d'une action passee (verif, signalement...) : « aujourd'hui » / « hier » quand c'est le cas,
  // l'heure toujours visible ; long ajoute le jour de la semaine quand la date est ecrite en entier (fiches).
  // Les dates seules (fmtDate) restent reservees aux peremptions et expirations.
  function fmtDateTime(value, { long = false, on = false } = {}) {
    const d = value ? new Date(value) : null;
    if (!d || isNaN(d)) return '–';
    const time = ' à ' + d.toLocaleTimeString('fr-FR', { hour: '2-digit', minute: '2-digit' });
    const day = new Date(d); day.setHours(0, 0, 0, 0);
    const diff = Math.round((today() - day) / 86400000);
    if (diff === 0) return "aujourd'hui" + time;
    if (diff === 1) return 'hier' + time;
    const date = (long ? d.toLocaleDateString('fr-FR', { weekday: 'long' }) + ' ' : '') + d.toLocaleDateString('fr-FR');
    return (on ? 'le ' : '') + date + time;
  }

  // Meme chose apres un verbe : « scellé le 12/10/2026 à 9:00 », « scellé aujourd'hui à 9:00 »
  const fmtOn = (value, opts = {}) => fmtDateTime(value, { ...opts, on: true });

  function el(tag, attrs = {}, ...children) {
    const node = document.createElement(tag);
    for (const [key, value] of Object.entries(attrs)) {
      if (key === 'class') node.className = value;
      else if (key.startsWith('on')) node.addEventListener(key.slice(2), value);
      else if (value !== false && value != null) node.setAttribute(key, value);
    }
    for (const child of children.flat()) {
      if (child == null || child === false) continue;
      node.append(child instanceof Node ? child : document.createTextNode(String(child)));
    }
    return node;
  }

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
      throw Object.assign(new Error('Serveur injoignable, vérifiez le réseau.'), { status: 0 });
    }
    let data = null;
    try { data = await response.json(); } catch (e) { /* reponse vide */ }
    if (!response.ok) {
      const message = (data && (data.error || data.detail)) || `Erreur ${response.status}`;
      throw Object.assign(new Error(message), { status: response.status, data: data || {} });
    }
    return data;
  }

  // Meme format que le front ordinateur (app/src/core/codes.cpp)
  function parseCode(raw) {
    const code = raw.trim();
    // etiquette d'item : l'iid seul (ancien format) ou l'URL <base>/item?id=IID
    // etiquette de taches (TaskItem) : <base>/task?id=IID, un item dont les taches sont a cocher a la verif
    const itemScan = (iid, task = false) => {
      const date = iid.slice(6, 14);
      return { kind: 'item', code, id: iid, type: iid.slice(0, 6), peremption: date === '00000000' ? null : parseDate(date), task };
    };
    if (IID_RE.test(code)) return itemScan(code);
    let url;
    try { url = new URL(code); } catch (e) { return { kind: 'unknown', code }; }
    const route = url.pathname.replace(/\/+$/, '').split('/').pop();
    const p = url.searchParams;
    if (route === 'item' && IID_RE.test(p.get('id') || '')) return itemScan(p.get('id'));
    if (route === 'task' && IID_RE.test(p.get('id') || '')) return itemScan(p.get('id'), true);
    if (route === 'verif' && p.get('lot')) return { kind: 'lot', code, id: p.get('lot'), key: p.get('key') || '' };
    if (route === 'badge' && p.get('m')) return { kind: 'user', code, id: p.get('m'), key: p.get('key') || '' };
    if (route === 'pack' && p.get('id')) return { kind: 'pack', code, id: p.get('id') };
    if (route === 'seal' && p.get('lot') && p.get('s')) return { kind: 'seal', code, id: p.get('lot'), key: p.get('s') };
    if (route === 'unseal' && p.get('lot') && p.get('c')) return { kind: 'sealopen', code, id: p.get('lot'), key: p.get('c') };
    if (route === 'scanner' && p.get('s') && p.get('k')) return { kind: 'remote', code, search: url.search };
    if (route === 'pinreset' && p.get('m') && p.get('t')) return { kind: 'pinreset', code, id: p.get('m'), key: p.get('t') };
    if (route === 'liste' && p.get('id')) return { kind: 'liste', code, id: p.get('id') };
    return { kind: 'unknown', code };
  }

  // ------------------------------------------------------------------------------------------------
  // Retours : son, vibration, flash

  let audio = null;

  function unlockAudio() {
    if (!audio) {
      const Context = window.AudioContext || window.webkitAudioContext;
      if (Context) audio = new Context();
    }
    if (audio && audio.state === 'suspended') audio.resume();
  }

  function tone(frequency, start, duration, type = 'square', volume = 0.25) {
    if (!audio) return;
    const osc = audio.createOscillator();
    const gain = audio.createGain();
    osc.type = type;
    osc.frequency.value = frequency;
    const t = audio.currentTime + start;
    gain.gain.setValueAtTime(0.0001, t);
    gain.gain.exponentialRampToValueAtTime(volume, t + 0.01);
    gain.gain.setValueAtTime(volume, t + duration - 0.02);
    gain.gain.exponentialRampToValueAtTime(0.0001, t + duration);
    osc.connect(gain).connect(audio.destination);
    osc.start(t);
    osc.stop(t + duration + 0.02);
  }

  function flash(kind) {
    const node = $('#flash');
    node.className = '';
    void node.offsetWidth; // relance l'animation
    node.className = kind;
  }

  // Vibreur : API Vibration (Android). Safari iOS ne l'a pas : a partir d'iOS 18, basculer un interrupteur
  // (<input type="checkbox" switch>) donne un retour haptique, utilise ici a defaut (best effort).
  let haptic = null;
  function vibrate(pattern) {
    if (navigator.vibrate) { navigator.vibrate(pattern); return; }
    if (!haptic) {
      const input = el('input', { type: 'checkbox', switch: '' });
      haptic = el('label', { 'aria-hidden': 'true', style: 'display:none' }, input);
      document.body.append(haptic);
    }
    const pulses = Array.isArray(pattern) ? Math.ceil(pattern.length / 2) : 1;
    for (let i = 0; i < pulses; i++) setTimeout(() => haptic.click(), i * 180);
  }

  const feedback = {
    good() { tone(1320, 0, 0.08, 'sine', 0.2); vibrate(40); flash('good'); },
    info() { tone(990, 0, 0.06, 'sine', 0.15); vibrate(30); },
    // etiquette de taches : trois notes montantes et ecran bleu (meme signal que l'app, Feedback::task)
    task() {
      tone(660, 0, 0.09, 'sine', 0.22); tone(880, 0.11, 0.09, 'sine', 0.22); tone(1100, 0.22, 0.14, 'sine', 0.22);
      vibrate([40, 60, 40, 60, 40]);
      flash('task');
    },
    warn() { tone(660, 0, 0.12, 'triangle', 0.25); vibrate([60, 60, 60]); },
    bad() {
      // produit perime, code inconnu : ecran rouge qui clignote, bip grave, vibration longue
      tone(880, 0, 0.18); tone(440, 0.22, 0.3);
      vibrate([250, 100, 250, 100, 250]);
      flash('bad');
    },
  };

  let toastTimer = 0;
  function toast(message, bad = false) {
    const node = $('#toast');
    node.textContent = message;
    node.className = 'show' + (bad ? ' bad' : '');
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => { node.className = ''; }, bad ? 4000 : 2500);
  }

  function showInfo(kind, title, ...lines) {
    const node = $('#info');
    node.className = 'info ' + kind;
    node.replaceChildren(el('div', { class: 'title' }, title), ...lines.filter(Boolean).map((line) => el('div', {}, line)));
  }

  // ------------------------------------------------------------------------------------------------
  // Camera et detection

  const video = $('#video');
  const canvas = $('#frame');
  const context = canvas.getContext('2d', { willReadFrequently: true });
  let stream = null;
  let detector = null;
  let wakeLock = null;
  let cameraWanted = false; // camera demarree par l'utilisateur : rouverte au retour sur la page
  let opening = null;       // ouverture en cours (getUserMedia peut prendre plusieurs secondes)
  let lastCode = '';
  let lastCodeTime = 0;
  let torchWanted = false;  // lampe torche (Chrome Android ; Safari iOS et Firefox ne la proposent pas aux pages web)

  // Jamais de camera ouverte quand la page est cachee, et une seule ouverture a la fois : sur Firefox
  // Android, un flux encore ouvert quand l'ecran s'eteint ou que l'onglet est decharge peut laisser la
  // camera « utilisee par Firefox » jusqu'au redemarrage du telephone.
  function cameraShouldRun() {
    return cameraWanted && !document.hidden && (!ficheOpen() || readerOpen()) && $('#opening').hidden;
  }

  function syncCamera() {
    if (!cameraShouldRun()) stopCamera();
    else if (!stream && !opening) opening = openCamera().finally(() => { opening = null; syncCamera(); });
  }

  function startCamera() {
    unlockAudio();
    cameraWanted = true;
    syncCamera();
  }

  function cameraFailed(message) {
    cameraWanted = false;
    $('#camera-error').textContent = message;
    $('#start').hidden = false;
  }

  async function openCamera() {
    $('#camera-error').textContent = '';
    if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) {
      cameraFailed("La caméra n'est accessible qu'en HTTPS. Utilisez « Saisir un code » en attendant.");
      return;
    }
    let media = null;
    for (let attempt = 0; !media; attempt++) {
      try {
        media = await navigator.mediaDevices.getUserMedia({
          audio: false,
          video: { facingMode: { ideal: 'environment' }, width: { ideal: 1280 }, height: { ideal: 720 } },
        });
      } catch (e) {
        const busy = e.name === 'NotReadableError' || e.name === 'AbortError';
        if (busy && attempt < 3) {
          // apres un rechargement, la camera de la page precedente est liberee avec un peu de retard
          await new Promise((resolve) => setTimeout(resolve, 1000));
          if (!cameraShouldRun()) return;
          continue;
        }
        cameraFailed(busy
          ? 'Caméra occupée (autre application ou autre onglet ?). Fermez-les puis réessayez.'
          : 'Caméra refusée ou indisponible : ' + e.message);
        return;
      }
    }
    // page cachee ou camera arretee pendant l'ouverture : on la rend tout de suite
    if (!cameraShouldRun()) { media.getTracks().forEach((track) => track.stop()); return; }
    stream = media;
    video.srcObject = stream;
    await video.play().catch(() => {});
    if (!detector && 'BarcodeDetector' in window) {
      try {
        const formats = await window.BarcodeDetector.getSupportedFormats();
        if (formats.includes('qr_code')) detector = new window.BarcodeDetector({ formats: ['qr_code'] });
      } catch (e) { detector = null; }
    }
    if (stream !== media) return; // arretee entre-temps
    const track = stream.getVideoTracks()[0];
    const capabilities = track.getCapabilities ? track.getCapabilities() : {};
    $('#torch').hidden = !capabilities.torch;
    // lampe allumee avant une coupure de la camera (ecran eteint, retour sur la page) : rallumee
    if (capabilities.torch && torchWanted) {
      track.applyConstraints({ advanced: [{ torch: true }] }).then(() => $('#torch').classList.add('on')).catch(() => {});
    }
    $('#start').hidden = true;
    requestWakeLock();
    scanLoop(media);
  }

  function stopCamera() {
    if (stream) stream.getTracks().forEach((track) => track.stop());
    stream = null;
    video.pause();
    video.srcObject = null;
    $('#torch').classList.remove('on');
    if (wakeLock) { wakeLock.release().catch(() => {}); wakeLock = null; }
  }

  async function requestWakeLock() {
    try { if ('wakeLock' in navigator) wakeLock = await navigator.wakeLock.request('screen'); } catch (e) { wakeLock = null; }
  }

  async function detect() {
    if (video.readyState < 2) return null;
    if (detector) {
      const codes = await detector.detect(video);
      return codes.length ? codes[0].rawValue : null;
    }
    if (!window.jsQR) return null;
    // jsQR : on ne decode que le carre central (la zone du viseur), reduit pour rester fluide
    const side = Math.min(video.videoWidth, video.videoHeight);
    const size = Math.min(side, 640);
    canvas.width = size;
    canvas.height = size;
    context.drawImage(video, (video.videoWidth - side) / 2, (video.videoHeight - side) / 2, side, side, 0, 0, size, size);
    const image = context.getImageData(0, 0, size, size);
    const result = window.jsQR(image.data, size, size, { inversionAttempts: 'attemptBoth' });
    return result ? result.data : null;
  }

  async function scanLoop(media) {
    while (stream === media) {
      try {
        const code = await detect();
        if (code) onDetected(code);
      } catch (e) { /* image illisible, on continue */ }
      await new Promise((resolve) => setTimeout(resolve, detector ? 120 : 180));
    }
  }

  function onDetected(code) {
    const now = Date.now();
    if (code === lastCode && now - lastCodeTime < REPEAT_DELAY_MS) { lastCodeTime = now; return; }
    lastCode = code;
    lastCodeTime = now;
    const camera = $('#camera');
    camera.classList.add('hit');
    setTimeout(() => camera.classList.remove('hit'), 300);
    handleCode(code);
  }

  document.addEventListener('visibilitychange', syncCamera);
  // rechargement, navigation, onglet gele ou decharge : la camera est rendue sans attendre le navigateur
  window.addEventListener('pagehide', stopCamera);
  window.addEventListener('pageshow', (event) => { if (event.persisted) syncCamera(); });
  document.addEventListener('freeze', stopCamera);
  document.addEventListener('resume', syncCamera);

  $('#start-button').addEventListener('click', startCamera);

  // Camera reduite en vignette flottante (comme en appel video) : la liste prend tout l'ecran, le scan continue.
  // Toucher la vignette remet la camera en haut. Retenu sur ce navigateur.
  const STORAGE_CAM_MINI = 'qrprotec.camera-mini';
  function setCameraMini(mini) {
    document.body.classList.toggle('cam-mini', mini);
    try { localStorage.setItem(STORAGE_CAM_MINI, mini ? '1' : ''); } catch (e) { /* stockage indisponible */ }
  }
  try { setCameraMini(localStorage.getItem(STORAGE_CAM_MINI) === '1'); } catch (e) { /* stockage indisponible */ }
  $('#cam-mini').addEventListener('click', () => setCameraMini(true));
  $('#camera').addEventListener('click', (event) => {
    if (!document.body.classList.contains('cam-mini') || document.body.classList.contains('home-mode')) return;
    event.preventDefault();
    event.stopPropagation();
    setCameraMini(false);
  }, true);
  $('#torch').addEventListener('click', async () => {
    const track = stream && stream.getVideoTracks()[0];
    if (!track) return;
    const on = !$('#torch').classList.contains('on');
    try {
      await track.applyConstraints({ advanced: [{ torch: on }] });
      torchWanted = on;
      $('#torch').classList.toggle('on', on);
    } catch (e) { toast('Lampe indisponible', true); }
  });

  // ------------------------------------------------------------------------------------------------
  // Traitement des scans

  function scannedIids() {
    const iids = new Set();
    for (const entry of allScanned()) {
      if (entry.error) continue;
      if (entry.kind === 'item') iids.add(entry.iid);
      for (const iid of entry.items || []) iids.add(iid);
    }
    return iids;
  }

  let scannedFiche = null; // item ou lot scanne dans le scanner qui a ouvert la fiche
  async function handleCode(raw) {
    unlockAudio();
    // popup « Passer a … » ignoree : le scan suivant vaut « Passer sans valider », puis il est traite
    if (leavePopup) { leavePopup('skip'); await new Promise((resolve) => setTimeout(resolve, 50)); }
    if (window.douchette && window.douchette.matches(raw)) { window.douchette.show(); return; }
    const scan = parseCode(raw);
    // page d'une liste de materiel a verifier ou a rentrer : l'etiquette d'un lot ou d'un item y compte
    if (listScanning() && (scan.kind === 'item' || scan.kind === 'lot')) { listScanCode(scan); return; }
    if (scan.kind === 'liste') { feedback.info(); if (readerOpen()) closeReader(); openList(scan.id); return; }
    // fenetre des taches ouverte et autre code scanne : l'item reste non valide (le rescanner reprend ses taches)
    if (taskPopup && !(scan.kind === 'item' && scan.id === taskPopup.entry.iid)) finishTasks(false);
    // fiche de desactivation affichee : rescanner l'item passe a ses taches, un autre code le laisse non valide
    if (replacePopup) {
      if (scan.kind === 'item' && scan.id === replacePopup.iid) { closeReplace(true); return; }
      closeReplace(false);
    }
    // scanner sans vérif en cours et pile vide : l'etiquette d'un item ou d'un lot ouvre sa fiche en grand, comme
    // le lien ouvert avec l'appareil photo du telephone (« Lancer une vérif » y ramene au scanner)
    if (!inHome() && !ficheOpen() && !taskPopup && ((scan.kind === 'item' && !scan.task) || scan.kind === 'lot')
        && !checkRunning() && !allScanned().length && !state.pendingOpening) {
      feedback.info();
      openFiche(scan.kind, scan.id, { key: scan.key || '' });
      scannedFiche = scan;
      return;
    }
    // etiquette du scelle hors vérif (scanner ou petit lecteur de l'accueil) : fiche du lot scelle, sans bouton de
    // vérif tant que le scelle est intact. Pendant une vérif, elle compte le lot comme verifie (scanSeal).
    if (scan.kind === 'seal' && !taskPopup && !checkRunning() && !allScanned().length) {
      if (readerOpen()) closeReader();
      openFiche('lot', scan.id, { seal: scan.key });
      return;
    }
    // petit lecteur de l'accueil : un badge connecte et l'on reste sur l'accueil, tout autre code ouvre le scanner
    if (readerOpen() && scan.kind !== 'user' && scan.kind !== 'sealopen') enterScanner(false);
    switch (scan.kind) {
      case 'item': return scanItem(scan);
      case 'lot': return scanLot(scan);
      case 'user': return login(scan.id, scan.key);
      case 'pack': return scanPack(scan);
      case 'seal': return scanSeal(scan);
      case 'sealopen': return openSeal(scan);
      case 'pinreset': return startPinReset(scan.id, scan.key);
      case 'remote':
        // QR code affiche par le poste : ce telephone devient sa douchette
        feedback.info();
        toast('Connexion au poste…');
        location.href = new URL('scanner' + scan.search, document.baseURI).href;
        return;
      default:
        feedback.bad();
        showInfo('bad', 'Code non reconnu', raw.length > 80 ? raw.slice(0, 80) + '…' : raw);
    }
  }

  async function scanItem(scan) {
    // taches de cet item deja affichees : un administrateur les valide toutes en scannant l'etiquette une 2e fois
    if (taskPopup && taskPopup.entry.iid === scan.id) {
      if (isAdmin()) { finishTasks(true); return; }
      feedback.task();
      toast('Cochez toutes les tâches pour valider cet item.');
      return;
    }
    const existing = state.scanned.find((entry) => entry.kind === 'item' && entry.iid === scan.id);
    if (existing) {
      // doublon : ignore, sans son ni vibration
      showInfo('ok', '✓ Déjà scanné', (existing.info && existing.info.type_name) || scan.id);
      return;
    }
    const shared = live.entries().find((entry) => entry.kind === 'item' && entry.iid === scan.id);
    if (shared) {
      showInfo('ok', `✓ Déjà scanné par ${shared.who || 'un autre participant'}`, (shared.info && shared.info.type_name) || scan.id);
      return;
    }
    scannedAgain();
    state.lastVerif = null; // un nouveau scan commence une nouvelle verif
    const expired = !!(scan.peremption && scan.peremption < today());
    const entry = { code: scan.code, kind: 'item', iid: scan.id, expired, info: null, error: '', place: state.lot ? currentPlace() : '' };
    if (scan.task) {
      // etiquette de taches : l'item n'est compte qu'une fois ses taches validees (fenetre des taches)
      feedback.task();
      showInfo('task', 'Étiquette de tâches', 'Chargement des tâches…');
      try {
        entry.info = await api(`items/${encodeURIComponent(scan.id)}/`);
        if (entry.info.expired) entry.expired = true;
      } catch (e) {
        if (e.status === 404) entry.error = 'Inconnu dans la base';
      }
      if (entry.info && !entry.error && (entry.info.tasks || []).length) {
        if (entry.expired) feedback.bad();
        openTasks(entry);
        return;
      }
    }
    await addItem(entry, scan);
  }

  // Item scanne ajoute a la vérif. Un item dont le type a des taches n'y entre qu'une fois ses taches validees.
  async function addItem(entry, scan, loaded = !!entry.info || !!entry.error) {
    state.scanned.push(entry);
    if (entry.expired || entry.error) feedback.bad(); else if (!scan.task || entry.tasksDone) feedback.good();
    showInfo(entry.expired ? 'bad' : 'ok', entry.expired ? badLabel(entry.info) : 'Item scanné',
      scan.peremption ? 'Péremption : ' + fmtDate(scan.peremption) : 'Non périssable');
    render();
    if (!loaded) {
      try {
        entry.info = await api(`items/${encodeURIComponent(scan.id)}/`);
        if (entry.info.expired && !entry.expired) { entry.expired = true; feedback.bad(); }
      } catch (e) {
        if (e.status === 404) { entry.error = 'Inconnu dans la base'; feedback.bad(); }
      }
      // etiquette d'item d'un item a taches : retire des scans tant que ses taches ne sont pas validees
      if (entry.info && !entry.error && !entry.tasksDone && (entry.info.tasks || []).length) {
        const index = state.scanned.indexOf(entry);
        if (index >= 0) state.scanned.splice(index, 1);
        if (!entry.expired) feedback.task();
        openTasks(entry);
        return;
      }
    }
    const info = entry.info;
    if (info && info.reported) alertReports('item:' + info.iid, `Item signalé : ${info.type_name}`, info.reports);
    // premier scan sans vérif en cours d'un item range dans un lot : c'est la vérif de ce lot qui commence
    if (!state.lot && state.scanned.length === 1 && state.scanned[0] === entry && info && info.location
        && !info.in_stock && info.status === 'active') {
      state.lotKey = ''; state.extra = []; state.lastVerif = null;
      await loadLot(info.location);
      if (state.lot && lotById(info.location)) {
        // vérif en cascade : celle du lot global, en entrant dans le sous-lot de l'item
        if (inCascade()) enterLevel(info.location, true); else state.current = state.lotId;
        entry.place = currentPlace();
        save();
        render();
        showInfo('lot', `Vérif de ${levelLot().name}`, `Commencée par le scan d'un de ses items (${info.type_name}).`,
          'Scannez les autres items du lot.');
        return;
      }
    }
    // vérif en cascade : un item d'un lot deja valide pendant la vérif n'y compte plus ; un item d'un sous-lot du lot
    // en cours fait entrer dans ce sous-lot
    const where = inCascade() && info && !entry.error && info.location ? lotById(info.location) : null;
    if (where && state.done[where.id]) {
      const index = state.scanned.indexOf(entry);
      if (index >= 0) state.scanned.splice(index, 1);
      feedback.warn();
      showInfo('warn', `Déjà vérifié – ${where.name}`,
        `${info.type_name} : ${where.name} a été validé ${fmtOn(state.done[where.id].at)}, cet item n'est pas recompté.`,
        `Scannez l'étiquette de ${where.name} pour le refaire.`);
      save();
      render();
      return;
    }
    // item d'un autre lot que celui ou l'on est, alors que celui-ci est complet : il est valide automatiquement
    let closed = [];
    if (where && !isUnder(where.id, levelId())) {
      const index = state.scanned.indexOf(entry);
      if (index >= 0) state.scanned.splice(index, 1); // pas compte dans le lot valide
      const left = await leaveCompleteLevels(where.id, true);
      closed = left.names;
      state.scanned.push(entry);
      entry.place = currentPlace();
      // « Rester » : l'item compte dans le lot ou l'on est (il y a ete trouve) ; sinon on passe au lot de l'item
      if (left.stay) {
        showInfo('ok', 'Item scanné', `${info.type_name} est compté dans ${levelLot().name}, où vous l'avez trouvé.`);
        save();
        render();
        return;
      }
      if (!isUnder(where.id, levelId())) enterLevel(state.lotId);
    }
    if (where && where.id !== levelId() && isUnder(where.id, levelId())) {
      enterLevel(where.id, true);
      entry.place = where.id;
      showInfo('lot', `📦 Entrée dans ${where.name}`, `${info.type_name} est rangé dans ${where.name}.`,
        closed.length ? `✔ ${closed.join(', ')} : validé.` : '',
        `Scannez son contenu, puis validez-le pour revenir à ${(lotById(parentIdOf(where)) || state.lot).name}.`);
      save();
      render();
      return;
    }
    if (state.scanned[state.scanned.length - 1] === entry) showItemInfo(entry);
    save();
    render();
  }

  // Fenetre des taches d'un item (etiquette de taches) : toutes cochees et validees, l'item compte comme vu
  let taskPopup = null; // {entry, scan}
  // Item a remplacer (perime, desactive, signale inutilisable) : sa fiche (peremption ou desactivation) s'affiche
  // d'abord, ses taches ensuite (bouton « Voir les tâches »)
  function openTasks(entry) {
    if (entry.expired && !entry.replaceSeen) { openReplace(entry); return; }
    showTasks(entry);
  }

  let replacePopup = null; // entry a remplacer dont la fiche est affichee avant ses taches
  function openReplace(entry) {
    const info = entry.info;
    replacePopup = entry;
    $('#replace-item-title').textContent = info.type_name;
    $('#replace-item-sub').textContent = entry.iid;
    const label = badLabel(info);
    $('#replace-item-banner').textContent = '✘ ' + cap(label.toLowerCase()) + ' : à remplacer';
    const facts = [];
    if (info.disabled) {
      facts.push(el('p', {}, el('b', {}, 'Désactivé : '), info.disabled_reason || '–', el('br'),
        el('span', { class: 'hint' }, [info.disabled_at ? cap(fmtOn(info.disabled_at, { long: true })) : '',
          info.disabled_by ? 'par ' + info.disabled_by : ''].filter(Boolean).join(' '))));
    }
    if (info.unusable && !info.disabled) facts.push(el('p', {}, el('b', {}, 'Signalé inutilisable.')));
    if (info.peremption) facts.push(el('p', {}, el('b', {}, 'Péremption : '), fmtDate(info.peremption)));
    $('#replace-item-facts').replaceChildren(...facts);
    showInfo('bad', label + ' – ' + info.type_name, 'Item à remplacer.');
    const dialog = $('#replace-item');
    if (!dialog.open) dialog.showModal();
  }

  function closeReplace(next) {
    const entry = replacePopup;
    replacePopup = null;
    const dialog = $('#replace-item');
    if (dialog.open) dialog.close();
    if (!entry) return;
    if (!next) {
      showInfo('warn', 'Item non validé : ' + entry.info.type_name, badLabel(entry.info) + ', à remplacer. Rescannez-le pour voir ses tâches.');
      return;
    }
    entry.replaceSeen = true;
    feedback.task();
    showTasks(entry);
  }

  $('#replace-item-next').addEventListener('click', () => closeReplace(true));
  $('#replace-item-cancel').addEventListener('click', () => closeReplace(false));
  $('#replace-item').addEventListener('cancel', (event) => { event.preventDefault(); closeReplace(false); });

  function showTasks(entry) {
    const dialog = $('#tasks');
    const tasks = entry.info.tasks;
    taskPopup = { entry, scan: parseCode(entry.code) };
    $('#tasks-title').textContent = entry.info.type_name;
    $('#tasks-sub').textContent = entry.iid + (entry.expired ? ' · ' + badLabel(entry.info) : '');
    const boxes = tasks.map((task) => el('input', { type: 'checkbox' }));
    const update = () => { $('#tasks-ok').disabled = !boxes.every((box) => box.checked); };
    for (const box of boxes) box.addEventListener('change', update);
    $('#tasks-list').replaceChildren(...tasks.map((task, index) => el('li', {}, el('label', {}, boxes[index], ' ', task))));
    $('#tasks-admin').hidden = !isAdmin();
    update();
    showInfo('task', 'Tâches : ' + entry.info.type_name, `${tasks.length} tâche(s) à compléter pour valider cet item.`);
    if (!dialog.open) dialog.showModal();
  }

  function finishTasks(done) {
    if (!taskPopup) return;
    const { entry, scan } = taskPopup;
    taskPopup = null;
    const dialog = $('#tasks');
    if (dialog.open) dialog.close();
    if (!done) {
      showInfo('warn', 'Item non validé : ' + entry.info.type_name, 'Toutes ses tâches doivent être cochées. Rescannez-le pour les reprendre.');
      return;
    }
    entry.tasksDone = true;
    addItem(entry, { ...scan, task: true }, true);
  }

  $('#tasks-form').addEventListener('submit', (event) => { event.preventDefault(); finishTasks(true); });
  $('#tasks-cancel').addEventListener('click', () => finishTasks(false));
  $('#tasks').addEventListener('cancel', (event) => { event.preventDefault(); finishTasks(false); });

  function showItemInfo(entry) {
    const info = entry.info;
    if (entry.error) { showInfo('bad', entry.error, entry.iid); return; }
    if (!info) return;
    const inLot = info.location && sessionLots().some((lot) => lot.id === info.location);
    const where = inLot ? 'Dans ' + (isMulti() ? 'le lot ' + info.location_name : 'ce lot')
      : info.status === 'out' || info.status === 'loose' ? 'Hors du stock' : info.in_stock ? 'En stock' + (info.location ? ' · ' + info.location_name : '')
        : info.location ? 'Rangé dans : ' + info.location_name : 'En stock';
    const status = info.status === 'out' ? (info.out_until
      ? `Sorti du stock, compté utilisé le ${fmtDate(info.out_until)} s'il ne revient pas` : 'Sorti du stock (non consommable)')
      : { missing: 'Signalé disparu', deleted: 'Marqué supprimé', replaced: 'Déjà remplacé',
        loose: 'Hors lot (retiré de son lot), pas encore en stock' }[info.status];
    const revision = info.revision_late ? `⚠ Révision en retard (prévue le ${fmtDate(info.revision_due)})`
      + (entry.expired ? '' : ', toujours utilisable') : '';
    showInfo(entry.expired ? 'bad' : status || revision ? 'warn' : 'ok',
      (entry.expired ? badLabel(info) + ' – ' : '') + info.type_name,
      info.peremption ? 'Péremption : ' + fmtDate(info.peremption) : 'Non périssable',
      where + (status ? ' · ' + status : ''), revision);
  }

  async function scanPack(scan) {
    try {
      const pack = await api(`packs/${encodeURIComponent(scan.id)}/`);
      // perime, ou entierement desactive (rappel produit) : a remplacer
      const expired = !!(pack.peremption && parseDate(pack.peremption) < today()) || (pack.disabled > 0 && pack.disabled >= pack.count);
      if (allScanned().some((entry) => entry.kind === 'pack' && entry.code === scan.code)) {
        showInfo('ok', '✓ Déjà scanné', `Paquet : ${pack.count} × ${pack.type_name}`);
        return;
      }
      scannedAgain();
      state.lastVerif = null;
      state.scanned.push({ code: scan.code, kind: 'pack', iid: '', info: pack, expired, error: '', items: pack.items,
                           place: state.lot ? currentPlace() : '' });
      if (expired) feedback.bad(); else feedback.good();
      alertReports('pack:' + pack.id, `Paquet signalé : ${pack.count} × ${pack.type_name}`, pack.reports);
      showInfo(expired ? 'bad' : 'ok', `Paquet : ${pack.count} × ${pack.type_name}`,
        pack.peremption ? 'Péremption : ' + fmtDate(pack.peremption) : 'Non périssable',
        pack.opened ? 'Paquet ouvert ' + fmtOn(pack.opened) : 'Paquet fermé');
      save();
      render();
    } catch (e) {
      feedback.bad();
      showInfo('bad', 'Paquet inconnu', e.message);
    }
  }

  async function scanLot(scan) {
    if (state.lot && state.lotId !== scan.id && await joinVerif(scan)) return;
    // meme lot global (vérif en cascade) : on y reste, sinon c'est une nouvelle vérif
    const same = !!state.lot && !!lotById(scan.id);
    if (state.lot && !same) toast(`Lot changé : ${state.lot.name} → nouveau lot`);
    if (scan.key && !(await keyAccepted(scan.id, scan.key))) return;
    if (!same) { state.lotKey = ''; state.lastVerif = null; state.extra = []; resetCascade(); }
    await loadLot(scan.id);
    if (!state.lot) return;
    if (scan.key) setKey(scan.id, scan.key);
    if (inCascade()) enterLevel(scan.id, true); else state.current = scan.id;
    state.askPlace = false;
    feedback.info();
    showLotInfo();
    if (!state.scanned.length) switchTab('todo');
    flushSeals();
  }

  // Etiquette cle scannee : celle du lot de la vérif, ou d'un sous-lot du lot global (vérif en cascade)
  function setKey(id, key) {
    if (id === state.lotId) state.lotKey = key; else state.keys[id] = key;
  }

  // Une vérif est en cours : scans (les notres ou ceux des autres participants), etiquette cle, vérif lancee depuis
  // la fiche du lot, ou vérif rejointe
  const checkRunning = () => !!state.lot && (allScanned().length > 0 || verifInProgress()
    || (state.launched === state.lotId && !state.lastVerif) || live.inTeam() || cascadeActive());

  // Lot ou sont ranges les items venus du stock scannes maintenant : derniere etiquette de lot scannee pendant la
  // vérif (services.assign_items). Lot principal par defaut.
  function currentPlace() {
    if (inCascade()) return levelId(); // vérif en cascade : le lot ou l'on est entre
    if (state.askPlace) return ''; // vérif rejointe, sous-lot pas encore indique : regle par defaut
    return sessionLots().some((lot) => lot.id === state.current) ? state.current : state.lotId;
  }

  // Etiquette (publique ou cle) d'un lot du meme lot global pendant une verif : la verif continue, ce lot devient
  // le lot en cours (currentPlace) et, s'il n'en faisait pas partie, ses items attendus s'ajoutent : ses items deja
  // scannes y ont bien ete trouves. Renvoie true si le scan a ete traite ici.
  async function joinVerif(scan) {
    if (!checkRunning()) return false;
    if (inCascade()) {
      // vérif en cascade : l'etiquette d'un lot du meme lot global fait entrer dans ce lot
      const lot = lotById(scan.id);
      if (!lot) return false;
      if (scan.key && !(await keyAccepted(scan.id, scan.key))) return true;
      if (scan.key) setKey(scan.id, scan.key);
      // etiquette d'un lot hors de celui ou l'on est, alors que celui-ci est complet : il est valide automatiquement
      const left = lot.id === levelId() || isUnder(lot.id, levelId()) ? { names: [] } : await leaveCompleteLevels(lot.id, true);
      if (left.stay) { showInfo('lot', `📦 ${levelLot().name}`, 'Vous restez dans ce lot.'); save(); render(); return true; }
      const closed = left.names;
      const reopened = state.done[lot.id];
      enterLevel(lot.id);
      feedback.info();
      showInfo('lot', `📦 ${lot.name}`, reopened ? `Déjà validé ${fmtOn(reopened.at)} : vous le refaites.`
        : lot.id === state.lotId ? 'Lot global : scannez l\'étiquette d\'un sous-lot pour y entrer.'
          : `Scannez son contenu, puis validez-le pour revenir à ${(lotById(parentIdOf(lot)) || state.lot).name}.`,
      closed.length ? `✔ ${closed.join(', ')} : validé.` : '',
      scan.key ? `🔑 Étiquette clé de ${lot.name} scannée` : '');
      save();
      render();
      if (!state.scanned.length) switchTab('todo');
      flushSeals();
      return true;
    }
    const covered = sessionLots().find((lot) => lot.id === scan.id);
    if (covered && !scan.key) {
      state.current = covered.id;
      state.askPlace = false;
      feedback.info();
      showInfo('ok', `${covered.name} fait partie de la vérif en cours`, 'Scannez ses items : ceux venus du stock y seront rangés.');
      save();
      render();
      return true;
    }
    let lot;
    try {
      lot = await api(`lots/${encodeURIComponent(scan.id)}/` + (scan.key ? `?key=${encodeURIComponent(scan.key)}` : ''));
    } catch (e) {
      feedback.bad();
      showInfo('bad', 'Lot introuvable', e.message);
      return true;
    }
    if (scan.key && lot.key_check !== 'valid') {
      feedback.bad();
      showInfo('bad', 'Étiquette clé refusée', lot.key_error);
      return true;
    }
    if (rootOf(lot) !== rootOf(state.lot)) return false;
    const existing = state.extra.find((entry) => entry.id === scan.id);
    if (existing) { if (scan.key) existing.key = scan.key; existing.lot = lot; } else state.extra.push({ id: scan.id, key: scan.key || '', lot });
    state.current = scan.id;
    state.askPlace = false;
    feedback.info();
    showInfo('ok', `Vérif groupée : ${sessionLots().filter((sub) => !sub.depth || sub.id === lot.id).map((sub) => sub.name).join(' + ')}`,
      covered ? `${lot.name} faisait déjà partie de la vérif : son étiquette clé est enregistrée.`
        : `Les items attendus de ${lot.name} s'ajoutent à la vérif en cours.`,
      scan.key ? `🔑 Étiquette clé de ${lot.name} scannée`
        : keyOk() ? '' : `Scannez l'étiquette clé de ${lot.name} (ou d'un autre lot de la vérif) pour valider.`);
    save();
    render();
    if (!state.scanned.length) switchTab('todo');
    return true;
  }

  const rootOf = (lot) => (lot.global ? lot.global.id : lot.path && lot.path.length ? lot.path[0].id : lot.id);

  // QR code du scelle d'un lot : lot valide sans verif tant que le scelle est intact. Pendant une vérif du meme lot
  // global, le lot scelle (et ses sous-lots) compte comme verifie sans etre ouvert (intactLots).
  async function scanSeal(scan) {
    if (state.lot && state.lotId !== scan.id && checkRunning() && await sealDuringVerif(scan)) return;
    if (state.lotId !== scan.id) { state.lotKey = ''; state.lastVerif = null; }
    await loadLot(scan.id, scan.key);
    const lot = state.lot;
    if (!lot || state.lotId !== scan.id) return;
    const sealInfo = lot.is_sealed ? `Scellé${lot.seal_number ? ' n°' + lot.seal_number : ''} ${fmtOn(lot.sealed)}` +
      (lot.sealed_by ? ' par ' + lot.sealed_by : '') : '';
    if (lot.seal_check === 'valid' && !lot.expired_count) {
      feedback.good();
      showInfo('ok', `✔ Scellé intact – ${lot.name}`,
        'Lot valide sans vérif' + (lot.valid_until ? ` jusqu'au ${fmtDate(lot.valid_until)}` : ''), sealInfo);
    } else if (lot.seal_check === 'valid') {
      feedback.bad();
      showInfo('bad', `✘ Scellé intact mais périmés – ${lot.name}`, `${lot.expired_count} item(s) périmé(s) : lot à ouvrir`, sealInfo);
    } else {
      feedback.bad();
      showInfo('bad', `✘ ${lot.seal_check === 'wrong' ? "Étiquette d'un ancien scellé" : 'Scellé brisé'} – ${lot.name}`,
        'Le lot doit être vérifié.',
        lot.unsealed ? `Scellé brisé ${fmtOn(lot.unsealed)}` + (lot.unsealed_by ? ' par ' + lot.unsealed_by : '') : '');
    }
    switchTab('lot');
  }

  // Etiquette du scelle d'un lot du meme lot global scannee pendant une vérif : scan « scellé intact ». Renvoie true si
  // le scan a ete traite ici (false : lot d'un autre lot global, la vérif n'est pas concernee).
  async function sealDuringVerif(scan) {
    if (allScanned().some((entry) => entry.kind === 'seal' && entry.iid === scan.id)
        || (inCascade() && state.done[scan.id] && state.done[scan.id].sealed)) {
      showInfo('ok', '✓ Déjà scanné', 'Scellé intact');
      return true;
    }
    let lot;
    try {
      lot = await api(`lots/${encodeURIComponent(scan.id)}/?seal=${encodeURIComponent(scan.key)}`);
    } catch (e) {
      feedback.bad();
      showInfo('bad', 'Lot introuvable', e.message);
      return true;
    }
    if (rootOf(lot) !== rootOf(state.lot)) return false;
    if (lot.seal_check !== 'valid' || lot.expired_count) {
      feedback.bad();
      showInfo('bad', lot.seal_check !== 'valid'
        ? `✘ ${lot.seal_check === 'wrong' ? "Étiquette d'un ancien scellé" : 'Scellé brisé'} – ${lot.name}`
        : `✘ Scellé intact mais périmés – ${lot.name}`, 'Ouvrez le lot et scannez son contenu.');
      return true;
    }
    // scelle d'un lot hors de celui ou l'on est, alors que celui-ci est complet : il est valide automatiquement
    const closed = inCascade() && lotById(lot.id) && !isUnder(lot.id, levelId()) ? (await leaveCompleteLevels(lot.id)).names : [];
    scannedAgain();
    state.scanned.push({ code: scan.code, kind: 'seal', iid: lot.id, key: scan.key, expired: false, error: '', items: [],
      place: inCascade() ? levelId() : '', info: { type_name: `🔒 Scellé intact – ${lot.name}` } });
    feedback.good();
    showInfo('ok', `🔒 Scellé intact – ${lot.name}`, 'Compté comme vérifié, avec ses sous-lots, sans être ouvert.',
      closed.length ? `✔ ${closed.join(', ')} : validé.` : '');
    save();
    render();
    flushSeals();
    return true;
  }

  // Etiquette d'ouverture, rangee dans le lot scelle : la scanner ouvre le scelle, connecte ou non (pas de
  // bouton « ouvrir »). Sans identite, l'ouverture est enregistree anonymement puis signee a la connexion.
  const OPENING_SIGN_DELAY = 60 * 60 * 1000; // meme delai que le serveur

  // fromUrl : etiquette ouverte avec l'appareil photo (adresse) : l'ecran fermé, on arrive sur l'accueil
  async function openSeal(scan, { fromUrl = false } = {}) {
    let result;
    try {
      result = await api(`lots/${encodeURIComponent(scan.id)}/seal-open/`,
        { code: scan.key, ...(hasIdentity() ? identity() : {}) });
    } catch (e) {
      feedback.bad();
      showInfo('bad', "Étiquette d'ouverture refusée", e.message);
      return;
    }
    const lot = result.lot;
    state.declaredAllowed = !!result.declared_identity;
    state.pendingOpening = result.identified ? null : { id: scan.id, key: scan.key, at: Date.now() };
    const opened = lot.unsealed ? `Scellé${lot.seal_number ? ' n°' + lot.seal_number : ''} ouvert ${fmtOn(lot.unsealed)}`
      + (lot.unsealed_by ? ' par ' + lot.unsealed_by : '') : '';
    if (result.result === 'signed') {
      // ouverture anonyme signee a la connexion : l'ecran d'ouverture, s'il est encore affiche, perd sa demande
      feedback.good();
      showInfo('ok', `🔓 Ouverture signée – ${lot.name}`, opened);
      if (openingShown() && sealScreen.lot && sealScreen.lot.id === lot.id) showOpening(result, sealScreen.fromUrl);
      save();
      return;
    }
    if (result.seal && result.seal.valid) feedback.good(); else feedback.warn();
    showInfo(result.result === 'opened' && result.seal && result.seal.valid ? 'ok' : 'warn',
      `🔓 ${result.result === 'opened' ? 'Scellé ouvert' : 'Scellé déjà ouvert'} – ${lot.name}`, opened);
    save();
    // ecran en grand, pas de vérif : l'ouverture est enregistree, les responsables le voient
    showOpening(result, fromUrl);
    // on demande quand meme a l'utilisateur de s'identifier : le nom declare tout de suite (si le serveur
    // l'autorise), sinon le badge, dont le scan signera l'ouverture
    if (state.pendingOpening && !state.user && state.declaredAllowed && askDeclaredName()) await signOpening();
  }

  // Ecran d'ouverture d'un scelle : rassurer (le scelle etait valide, jusqu'a quand) et dire que l'ouverture est
  // connue des responsables (TECH), pour eviter qu'on la signale. Contenu du scelle, aucun bouton de vérif.
  let sealScreen = { lot: null, fromUrl: false };
  const openingShown = () => !$('#opening').hidden;

  function showOpening(result, fromUrl = false) {
    const lot = result.lot;
    const seal = result.seal || {};
    sealScreen = { lot, fromUrl };
    const sealedLine = seal.sealed ? `Scellé${seal.seal_number ? ' n°' + seal.seal_number : ''} posé ${fmtOn(seal.sealed, { long: true })}`
      + (seal.sealed_by ? ' par ' + seal.sealed_by : '') : '';
    const openedLine = lot.unsealed ? `Ouvert ${fmtOn(lot.unsealed, { long: true })}`
      + (lot.unsealed_by && result.identified ? ' par ' + lot.unsealed_by : '') : '';
    const parts = [
      el('div', { class: 'opening-head' },
        el('div', { class: 'icon' }, '🔓'),
        el('h2', {}, result.result === 'already' ? 'Scellé déjà ouvert' : 'Ouverture du scellé enregistrée'),
        el('div', { class: 'sub' }, lot.name + (lot.lot_type_name ? ` · ${lot.lot_type_name}` : ''))),
      seal.valid
        ? el('div', { class: 'opening-box ok' },
          el('b', {}, '✔ Le scellé était valide'),
          el('p', {}, seal.valid_until ? 'Contenu validé jusqu\'au' : 'Contenu validé, sans date de péremption'),
          seal.valid_until ? el('div', { class: 'valid-until' }, fmtDate(seal.valid_until)) : '',
          sealedLine ? el('p', { class: 'sub' }, sealedLine) : '')
        : el('div', { class: 'opening-box warn' },
          el('b', {}, '⚠ Une partie du contenu n\'était plus valide'),
          el('p', {}, 'Vérifiez les dates avant d\'utiliser un item (en rouge ci-dessous).'),
          sealedLine ? el('p', { class: 'sub' }, sealedLine) : ''),
      el('div', { class: 'opening-box tech' },
        el('b', {}, '🛠 Le TECH est au courant'),
        el('p', {}, 'L\'ouverture est enregistrée et le TECH en est informé : inutile de la signaler. '
          + (lot.seal_required ? 'Le lot sera vérifié puis scellé à nouveau plus tard.' : 'Le lot sera vérifié plus tard.')),
        openedLine ? el('p', { class: 'sub' }, openedLine) : '',
        state.pendingOpening && state.pendingOpening.id === lot.id && !state.user
          ? el('p', {}, 'Pour signer l\'ouverture, scannez ensuite votre badge.') : ''),
      el('h3', {}, 'Contenu du scellé'),
    ];
    // items du lot puis de chacun de ses sous-lots (ils etaient sous le meme scelle), regroupes par type
    const lots = [lot, ...(lot.descendants || [])];
    let any = false;
    for (const sub of lots) {
      const groups = new Map();
      for (const item of sub.items || []) {
        const group = groups.get(item.type_name) || { name: item.type_name, count: 0, first: null, expired: 0 };
        group.count += 1;
        if (item.expired) group.expired += 1;
        if (item.peremption && (!group.first || item.peremption < group.first)) group.first = item.peremption;
        groups.set(item.type_name, group);
      }
      if (!groups.size) continue;
      any = true;
      if (lots.length > 1) {
        parts.push(el('div', { class: 'opening-lot', style: `padding-left:${Math.max(0, (sub.depth || 0) - 1) * 12}px` },
          '📦 ' + sub.name));
      }
      const rows = [...groups.values()].sort((a, b) => a.name.localeCompare(b.name, 'fr'));
      parts.push(el('ul', { class: 'opening-items' }, ...rows.map((group) => el('li', { class: group.expired ? 'expired' : '' },
        el('span', { class: 'count' }, `${group.count} ×`),
        el('span', {}, group.name),
        el('span', { class: 'date' }, group.expired ? `${group.expired} à remplacer`
          : group.first ? 'jusqu\'au ' + fmtDate(group.first) : '')))));
    }
    if (!any) parts.push(el('p', { class: 'sub' }, 'Aucun item enregistré dans ce lot.'));
    $('#opening-body').replaceChildren(...parts);
    $('#opening-body').scrollTop = 0;
    $('#opening').hidden = false;
    syncCamera();
  }

  function closeOpening() {
    if (!openingShown()) return;
    $('#opening').hidden = true;
    const home = sealScreen.fromUrl && !verifInProgress() && !ficheOpen();
    sealScreen = { lot: null, fromUrl: false };
    syncCamera();
    if (home) showHome('menu');
    else render();
  }

  async function signOpening() {
    const pending = state.pendingOpening;
    if (!pending) return;
    state.pendingOpening = null;
    save();
    if (Date.now() - pending.at > OPENING_SIGN_DELAY || !hasIdentity()) return;
    await openSeal({ kind: 'sealopen', id: pending.id, key: pending.key });
  }

  // Un sous-lot d'un lot global se verifie dans la vérif en cascade du lot global : c'est lui qui est charge, et l'on
  // entre dans le sous-lot (state.path). seal : etiquette du scelle, le lot lui-meme est affiche.
  async function loadLot(id, seal) {
    try {
      let lot = await api(`lots/${encodeURIComponent(id)}/` + (seal ? `?seal=${encodeURIComponent(seal)}` : ''));
      const root = !seal && lot.global && lot.global.id !== id ? lot.global.id : '';
      if (root) lot = await api(`lots/${encodeURIComponent(root)}/`);
      const target = root || id;
      if (state.lotId !== target) { state.extra = []; resetCascade(); state.startedIn = root ? id : ''; }
      state.lot = lot;
      state.lotId = target;
      // chemin vers le sous-lot demande, ou chemin garde (rechargement) tant qu'il existe encore
      const last = root ? id : state.path[state.path.length - 1];
      state.path = last && lotById(last) && last !== target ? chainTo(last) : [];
      for (const lotId of Object.keys(state.done)) if (!lotById(lotId)) delete state.done[lotId];
    } catch (e) {
      feedback.bad();
      showInfo('bad', 'Lot introuvable', e.message);
      if (state.lotId === id) { state.lot = null; state.lotId = ''; state.lotKey = ''; state.extra = []; resetCascade(); }
    }
    await loadExtra();
    save();
    render();
  }

  // Detail des autres lots de la verif groupee (rechargement de la page, apres une verif)
  async function loadExtra() {
    const kept = [];
    for (const entry of state.extra) {
      try {
        entry.lot = await api(`lots/${encodeURIComponent(entry.id)}/`);
        if (state.lot && rootOf(entry.lot) === rootOf(state.lot)) kept.push(entry);
      } catch (e) { /* lot supprime ou archive : retire de la verif */ }
    }
    state.extra = kept;
  }

  // Lots couverts par la verif en cours : le lot scanne, ses sous-lots, puis les lots ajoutes et leurs sous-lots
  function sessionLots() {
    const result = [];
    const seen = new Set();
    const add = (lot, depth) => {
      if (!lot || seen.has(lot.id)) return;
      seen.add(lot.id);
      result.push(Object.assign(lot, { depth }));
    };
    for (const top of [state.lot, ...state.extra.map((entry) => entry.lot)]) {
      if (!top) continue;
      add(top, 0);
      for (const sub of top.descendants || []) add(sub, sub.depth);
    }
    return result;
  }

  const isMulti = () => sessionLots().length > 1;

  // ------------------------------------------------------------------------------------------------
  // Vérif en cascade : un lot global qui contient des sous-lots (ex : VPS > armoires > sacs) se verifie lot par lot.
  // On entre dans un sous-lot en scannant son etiquette (ou un de ses items), on fait sa vérif, on la valide : elle est
  // enregistree tout de suite (services.perform_verif, "exclude" et "keys") et l'on revient au lot qui le contient, ou
  // il passe en vert. Les sous-lots non faits restent tels quels, sans etre comptes manquants. A plusieurs, chacun fait
  // ses sous-lots dans la meme vérif du lot global (live.py).

  const inCascade = () => !!state.lot && !state.extra.length && (state.lot.descendants || []).length > 0;
  const lotById = (id) => (id ? sessionLots().find((lot) => lot.id === id) || null : null);
  const parentIdOf = (lot) => lot.parent || (lot.path && lot.path.length ? lot.path[lot.path.length - 1].id : null);
  const levelId = () => (state.path.length ? state.path[state.path.length - 1] : state.lotId);
  const levelLot = () => lotById(levelId()) || state.lot;
  const childrenOf = (id) => sessionLots().filter((lot) => parentIdOf(lot) === id);
  // vérif en cours dans la cascade : un lot ouvert, un lot valide ou une etiquette cle scannee
  const cascadeActive = () => inCascade() && (state.path.length > 0 || state.entered.length > 0
    || Object.keys(state.done).length > 0 || Object.keys(state.keys).length > 0);

  function resetCascade() {
    state.path = []; state.done = {}; state.keys = {}; state.entered = []; state.startedIn = '';
  }

  // lot id dans le lot top (ou top lui-meme)
  function isUnder(id, top) {
    for (let lot = lotById(id), guard = 0; lot && guard < 50; lot = lotById(parentIdOf(lot)), guard += 1) {
      if (lot.id === top) return true;
    }
    return false;
  }

  const subtreeIds = (top) => sessionLots().filter((lot) => isUnder(lot.id, top)).map((lot) => lot.id);

  // Sous-lots du lot global jusqu'au lot id (exclu le lot global)
  function chainTo(id) {
    const chain = [];
    for (let lot = lotById(id), guard = 0; lot && lot.id !== state.lotId && guard < 50; lot = lotById(parentIdOf(lot)), guard += 1) {
      chain.unshift(lot.id);
    }
    return chain;
  }

  // Entrer dans un lot du lot global : il devient le lot en cours (les items venus du stock y sont ranges). Un lot deja
  // valide pendant la vérif est rouvert : on le refait.
  function enterLevel(id) {
    const lot = lotById(id);
    if (!lot) return;
    state.path = id === state.lotId ? [] : chainTo(id);
    state.current = id;
    state.askPlace = false;
    if (!state.entered.includes(id)) state.entered.push(id);
    delete state.done[id];
    switchTab('todo');
  }

  // Revenir au lot qui contient le lot en cours, sans le valider (ses scans restent)
  function goUp() {
    state.path.pop();
    state.current = levelId();
    save();
    render();
  }

  // Lots ou se trouvent les autres participants de la vérif commune : {id du lot: [noms]}
  function othersLevels() {
    const result = {};
    for (const row of live.others()) {
      if (row.connected && row.level && row.level.id) (result[row.level.id] = result[row.level.id] || []).push(row.who || 'quelqu\'un');
    }
    return result;
  }

  // Lot commence : on y est entre, ou des scans y ont ete faits (le lot ou un de ses sous-lots)
  function touched(id) {
    const ids = new Set(subtreeIds(id));
    return state.entered.some((lotId) => ids.has(lotId))
      || allScanned().some((entry) => !entry.error && ids.has(entry.place || state.lotId));
  }

  // Sous-lots du lot top laisses de cote a sa validation (avec leurs sous-lots) : deja valides, pas commences, ou
  // en cours chez un autre participant
  function excludedFor(top) {
    const others = othersLevels();
    const mine = levelId();
    const result = [];
    const walk = (id) => {
      for (const child of childrenOf(id)) {
        const ids = subtreeIds(child.id);
        const busy = ids.some((lotId) => others[lotId]) && !ids.includes(mine);
        if (state.done[child.id] || busy || !touched(child.id)) result.push(child.id);
        else walk(child.id);
      }
    };
    walk(top);
    return result;
  }

  // Lots verifies a la validation du lot top : lui et ses sous-lots qui ne sont pas laisses de cote
  function scopeOf(top, exclude = excludedFor(top)) {
    const out = new Set(exclude.flatMap(subtreeIds));
    return new Set(subtreeIds(top).filter((id) => !out.has(id)));
  }

  // Lot qui ne fait que regrouper des sous-lots (rien d'attendu, rien dedans)
  const ownContent = (lot) => !!((lot.requirements || []).length || (lot.items || []).length || (lot.missing_items || []).length);

  // Etat actuel d'un sous-lot pas encore fait pendant la vérif (lot de regroupement : ses sous-lots)
  function childStatus(lot) {
    if (ownContent(lot) || lot.storage) return lotStatus(lot);
    const subs = sessionLots().filter((sub) => sub.id !== lot.id && isUnder(sub.id, lot.id) && (ownContent(sub) || sub.storage));
    const bad = subs.filter((sub) => !lotStatus(sub).ok).length;
    return bad ? { ok: false, kind: 'bad', label: `✘ ${bad} sous-lot(s) à traiter sur ${subs.length}` }
      : { ok: true, kind: 'ok', label: subs.length ? `✔ Sous-lots tous valides (${subs.length})` : 'Vide' };
  }

  // Etiquette privee scannee : le serveur dit si sa cle vaut encore (ancienne etiquette, cle expiree)
  async function keyAccepted(id, key) {
    try {
      const lot = await api(`lots/${encodeURIComponent(id)}/?key=${encodeURIComponent(key)}`);
      if (lot.key_check === 'valid') return true;
      feedback.bad();
      showInfo('bad', 'Étiquette clé refusée', lot.key_error);
      return false;
    } catch (e) {
      return true; // lot introuvable : signale ensuite par loadLot
    }
  }

  // Meme texte que Lots.key_hint cote serveur et key_hint dans l'app
  function keyHint(lot) {
    const location = lot && lot.key_location;
    return "L'étiquette clé est un QR code rangé à l'intérieur du lot" + (location ? ` (${location}).` : '.');
  }

  // Lots envoyes au serveur avec leur cle ; la cle d'un lot couvre ses sous-lots
  function sessionEntries() {
    return [{ id: state.lotId, key: state.lotKey }, ...state.extra.map(({ id, key }) => ({ id, key })),
      // vérif en cascade : etiquettes cles des sous-lots scannees
      ...Object.entries(state.keys).filter(([id, key]) => key && id !== state.lotId).map(([id, key]) => ({ id, key }))];
  }

  // Lots de la vérif sans etiquette cle : une seule etiquette cle d'un des lots de la vérif (un sous-lot suffit, ex :
  // un rangement pour le stock) valide toute la vérif, comme cote serveur. Vérif à plusieurs : la cle d'un lot
  // scannee par un autre participant est gardee par le serveur (keyed).
  function keyMissing() {
    const keyed = new Set([...live.keyed(), ...(state.lotKey ? [state.lotId] : []),
      ...state.extra.filter((entry) => entry.key).map((entry) => entry.id),
      ...Object.keys(state.keys).filter((id) => state.keys[id])]);
    if (keyed.size) return [];
    return inCascade() ? [levelLot()] : [state.lot, ...state.extra.map((entry) => entry.lot)].filter(Boolean);
  }

  const keyOk = () => !keyMissing().length;

  // Lots de la vérif dont le scelle a ete constate intact (etiquette du scelle scannee), avec leurs sous-lots : ni
  // attendus ni ouverts (services.perform_verif). Un lot dont un item a ete scanne a ete ouvert.
  function intactLots() {
    const lots = sessionLots();
    const parentOf = Object.fromEntries(lots.map((lot) => [lot.id,
      lot.parent || (lot.path && lot.path.length ? lot.path[lot.path.length - 1].id : null)]));
    const under = (id, top) => {
      for (let current = id; current; current = parentOf[current]) if (current === top) return true;
      return false;
    };
    const done = scannedIids();
    const result = new Set();
    for (const entry of allScanned()) {
      if (entry.kind !== 'seal' || entry.error) continue;
      const subtree = lots.filter((lot) => under(lot.id, entry.iid));
      const opened = subtree.some((lot) => [...(lot.items || []), ...(lot.missing_items || [])].some((item) => done.has(item.iid)));
      if (!opened) for (const lot of subtree) result.add(lot.id);
    }
    return result;
  }

  // Repartition des items scannes entre les lots de la verif (meme regle que le serveur, services.assign_items) :
  // un item deja dans un des lots y reste, un nouvel item va dans le lot de la derniere etiquette scannee avant lui
  // (ou un de ses sous-lots) qui en attend encore, sinon dans le premier lot qui en attend encore.
  // complete : le lot serait complet avec ces scans (verif partielle possible).
  function planSession() {
    const intact = intactLots();
    // vérif en cascade : les lots deja valides ne sont plus attendus
    const lots = sessionLots().filter((lot) => !intact.has(lot.id) && !state.done[lot.id]);
    const locationOf = {};
    const holding = {};
    const done = scannedIids();
    for (const lot of lots) {
      for (const item of [...(lot.items || []), ...(lot.missing_items || [])]) {
        locationOf[item.iid] = lot.id;
        // un item a etiquette a dechirer non scanne a ete utilise, un item non consommable non scanne d'un lot
        // ordinaire passe hors lot (services.leaves_lot) : ils ne retiennent pas le lot
        if (!done.has(item.iid) && !item.tear_off && (item.consumable !== false || lot.storage)) holding[lot.id] = true;
      }
    }
    const required = {};
    for (const lot of lots) {
      required[lot.id] = {};
      for (const row of lot.requirements || []) required[lot.id][row.type] = row.required;
    }
    const deficit = Object.fromEntries(lots.map((lot) => [lot.id, { ...required[lot.id] }]));
    const scanned = [];
    for (const entry of allScanned()) {
      if (entry.error) continue;
      const type = entry.kind === 'item' ? entry.iid.slice(0, 6) : entry.info && entry.info.type;
      const iids = entry.kind === 'item' ? [entry.iid] : entry.items || [];
      for (const iid of iids) scanned.push({ iid, type, expired: !!entry.expired, place: entry.place || '' });
    }
    // lot parent de chaque lot de la vérif : un item venu du stock va dans le lot de la derniere etiquette scannee
    // avant lui ou un de ses sous-lots
    const parentOf = Object.fromEntries(lots.map((lot) => [lot.id,
      lot.parent || (lot.path && lot.path.length ? lot.path[lot.path.length - 1].id : null)]));
    const under = (id, place) => {
      for (let current = id; current; current = parentOf[current]) if (current === place) return true;
      return false;
    };
    const target = {};
    const newcomers = [];
    for (const item of scanned) {
      const where = locationOf[item.iid];
      if (where) {
        target[item.iid] = where;
        if (!item.expired && deficit[where][item.type] > 0) deficit[where][item.type] -= 1;
      } else newcomers.push(item);
    }
    newcomers.sort((a, b) => (a.expired - b.expired) || (a.iid < b.iid ? -1 : a.iid > b.iid ? 1 : 0));
    for (const item of newcomers) {
      const scope = lots.map((lot) => lot.id).filter((id) => !(item.place in parentOf) || under(id, item.place));
      const candidates = scope.filter((id) => item.type in required[id]);
      let choice = null;
      if (!item.expired) {
        choice = candidates.find((id) => deficit[id][item.type] > 0) || null;
        if (choice) deficit[choice][item.type] -= 1;
      }
      target[item.iid] = choice || candidates[0] || scope[0];
    }
    const plan = Object.fromEntries(lots.map((lot) => [lot.id, { lot, fresh: {}, expired: {}, newFresh: {}, touched: 0 }]));
    for (const item of scanned) {
      const row = plan[target[item.iid]];
      if (!row) continue;
      row.touched += 1;
      if (item.expired) row.expired[item.type] = (row.expired[item.type] || 0) + 1;
      else {
        row.fresh[item.type] = (row.fresh[item.type] || 0) + 1;
        if (locationOf[item.iid] !== row.lot.id) row.newFresh[item.type] = (row.newFresh[item.type] || 0) + 1;
      }
    }
    for (const row of Object.values(plan)) {
      // perimes remplaces par des items frais arrives dans le meme lot
      const expiredLeft = Object.entries(row.expired)
        .reduce((sum, [type, count]) => sum + Math.max(0, count - (row.newFresh[type] || 0)), 0);
      const filled = Object.entries(required[row.lot.id]).every(([type, quantity]) => (row.fresh[type] || 0) >= quantity);
      // rangement du stock : ses items sont ses attendus, complet seulement si tous sont scannes
      row.complete = filled && !expiredLeft && (row.lot.storage ? !holding[row.lot.id] : (row.touched > 0 || !holding[row.lot.id]));
    }
    return { lots: lots.map((lot) => plan[lot.id]), target, intact };
  }

  // Etat d'un lot : verifie et complet (vert), reassort depuis la derniere verif (orange : verif complete
  // recommandee), sinon incomplet ou jamais verifie (rouge). Un lot dont le type est a scelle obligatoire est
  // rouge tant qu'il n'est pas scelle. kind : 'ok', 'warn' ou 'bad'.
  function lotStatus(lot) {
    const make = (kind, label) => ({ ok: kind === 'ok', kind, label });
    if (lot.is_sealed && lot.expired_count) return make('bad', '✘ Scellé, contient des périmés');
    if (lot.is_sealed) return make('ok', '✔ Scellé' + (lot.valid_until ? `, valide jusqu'au ${fmtDate(lot.valid_until)}` : ''));
    if (!lot.last_verif) return make('bad', '✘ Jamais vérifié');
    if (!lot.complete) return make('bad', lot.expired_count ? '✘ Incomplet (périmés)' : '✘ Incomplet');
    if (lot.seal_required) {
      return make('bad', lot.verif_recommended ? '✘ Scellé ouvert, vérif complète puis scellé requis' : '✘ Vérifié, doit être scellé');
    }
    if (lot.verif_recommended) return make('warn', lot.restocked_count ? '⚠ Vérif recommandée, réassort'
      : lot.back_from_list ? '⚠ Vérif recommandée, retour de liste' : '⚠ Vérif recommandée, scellé ouvert');
    return make('ok', '✔ Vérifié, complet');
  }

  function globalLine(lot) {
    const global = lot.global;
    if (!global) return '';
    return `Lot global ${global.name} : ${global.label} · vérif la plus ancienne : `
      + (global.last_verif ? fmtDateTime(global.last_verif) : 'jamais');
  }

  // Lot qui ne fait que regrouper des sous-lots (rien d'attendu, rien dedans, ex : un B+) : son etat est
  // celui de ses sous-lots (lignes qui le suivent dans l'arborescence du lot global)
  function groupStatus(lot) {
    const rows = (lot.global && lot.global.lots) || [];
    const index = rows.findIndex((row) => row.id === lot.id);
    if (index < 0 || rows[index].counted) return null;
    let problems = 0; let warn = 0; let counted = 0;
    for (const row of rows.slice(index + 1)) {
      if (row.depth <= rows[index].depth) break;
      if (!row.counted) continue;
      counted += 1;
      if (row.effective.kind !== 'ok') problems += 1;
      if (row.effective.kind === 'warn') warn += 1;
    }
    if (!problems) return { ok: true, kind: 'ok', label: `✔ Sous-lots tous valides (${counted})` };
    return { ok: false, kind: warn === problems ? 'warn' : 'bad', label: `✘ ${problems} sous-lot(s) à traiter sur ${counted}` };
  }

  function showLotInfo() {
    const lot = state.lot;
    if (!lot) return;
    const status = groupStatus(lot) || lotStatus(lot);
    showInfo(status.kind, `${status.label} – ${lot.name}`,
      `${lot.lot_type_name} · ${lot.item_count} item(s)` + (lot.expired_count ? ` · ${lot.expired_count} périmé(s)` : '')
        + ((lot.descendants || []).length ? ` · ${lot.descendants.length} sous-lot(s), vérifiés chacun à part` : ''),
      globalLine(lot),
      'Dernière vérif : ' + (lot.last_verif ? fmtDateTime(lot.last_verif, { long: true }) + (lot.last_verif_by ? ' par ' + lot.last_verif_by : '') : 'jamais'),
      !lot.verif_recommended ? '' : lot.restocked_count
        ? `Réassort de ${lot.restocked_count} item(s) ${fmtOn(lot.restocked, { long: true })}${lot.restocked_by ? ' par ' + lot.restocked_by : ''} : faites une vérif complète.`
        : lot.back_from_list ? `Rentré avec la liste « ${lot.back_from_list} » ${fmtOn(lot.restocked, { long: true })}${lot.restocked_by ? ' par ' + lot.restocked_by : ''} : faites une vérif complète.`
        : `Scellé ouvert${lot.unsealed ? ' ' + fmtOn(lot.unsealed, { long: true }) : ''}${lot.unsealed_by ? ' par ' + lot.unsealed_by : ''} : faites une vérif complète.`,
      lot.seal_required && !lot.is_sealed ? 'Scellé obligatoire : posez un nouveau scellé après la vérif complète.' : '',
      keyOk() ? '🔑 Étiquette clé scannée' : 'Scannez l\'étiquette clé pour pouvoir valider. ' + keyHint(lot));
  }

  // Saisie du PIN (ou choix du PIN pour un admin qui n'en a pas encore)
  function askPin(matricule, key, setup, errorMessage) {
    const dialog = $('#pin');
    $('#pin-title').textContent = setup ? 'Choisissez votre code PIN' : 'Code PIN';
    $('#pin-text').textContent = setup
      ? 'Nouveau code PIN de 4 à 8 chiffres, demandé après le badge à chaque connexion (obligatoire pour les administrateurs).'
      : `Badge ${matricule} : saisissez votre code PIN.`;
    $('#pin-input').value = '';
    $('#pin-confirm').value = '';
    $('#pin-confirm').hidden = !setup;
    $('#pin-error').textContent = errorMessage || '';
    $('#pin-form').onsubmit = (event) => {
      event.preventDefault();
      const pin = $('#pin-input').value.trim();
      if (!/^\d{4,8}$/.test(pin)) { $('#pin-error').textContent = 'Le PIN doit comporter 4 à 8 chiffres.'; return; }
      if (setup && pin !== $('#pin-confirm').value.trim()) { $('#pin-error').textContent = 'Les deux PIN sont différents.'; return; }
      dialog.close();
      login(matricule, key, setup ? { new_pin: pin } : { pin });
    };
    $('#pin-cancel').onclick = () => dialog.close();
    const forgot = $('#pin-forgot');
    forgot.hidden = setup;
    forgot.onclick = () => forgotPin(matricule, key);
    if (!dialog.open) dialog.showModal();
    setTimeout(() => $('#pin-input').focus(), 50);
  }

  async function login(matricule, key, extra = {}) {
    try {
      const user = await api('auth/', { matricule, key, ...extra });
      loggedIn({ ...user, key });
    } catch (e) {
      if (e.data && e.data.pin_blocked) {
        feedback.bad();
        showInfo('bad', 'Code PIN bloqué', e.message);
        showPinBlocked(e.data.pin_reset || {});
        return;
      }
      if (e.data && (e.data.pin_required || e.data.pin_setup_required)) {
        const retry = extra.pin || extra.new_pin || e.data.pin_locked;
        if (retry) feedback.bad(); else feedback.info();
        askPin(matricule, key, !!e.data.pin_setup_required, retry ? e.message : '');
        return;
      }
      feedback.bad();
      showInfo('bad', 'Badge refusé', e.message);
    }
  }

  // Connexion sans badge (nom, prenom et code PIN) : discrete, depuis le petit lecteur de l'accueil ou la puce
  // « Non connecté » du scanner. Le serveur ne dit pas si le nom existe.
  function openNameLogin() {
    const dialog = $('#name-login');
    $('#name-login-pin').value = '';
    $('#name-login-error').textContent = '';
    const declaredLink = $('#name-login-declared');
    declaredLink.hidden = !state.declaredAllowed || inHome();
    declaredLink.onclick = () => { dialog.close(); askDeclaredName(); };
    $('#name-login-form').onsubmit = async (event) => {
      event.preventDefault();
      const prenom = $('#name-login-prenom').value.trim();
      const nom = $('#name-login-nom').value.trim();
      const pin = $('#name-login-pin').value.trim();
      if (!prenom || !nom) { $('#name-login-error').textContent = 'Saisissez votre prénom et votre nom.'; return; }
      if (!/^\d{4,8}$/.test(pin)) { $('#name-login-error').textContent = 'Le PIN doit comporter 4 à 8 chiffres.'; return; }
      try {
        const user = await api('auth/name/', { nom, prenom, pin });
        dialog.close();
        loggedIn({ ...user, loginAt: Date.now() });
      } catch (e) {
        feedback.bad();
        $('#name-login-pin').value = '';
        $('#name-login-error').textContent = e.message;
      }
    };
    $('#name-login-cancel').onclick = () => dialog.close();
    if (!dialog.open) dialog.showModal();
    setTimeout(() => ($('#name-login-prenom').value ? $('#name-login-pin') : $('#name-login-prenom')).focus(), 50);
  }

  function loggedIn(user) {
    const action = takeLoginAction(); // action qui attendait la connexion (signalement)
    state.user = user;
    state.lots = null;
    state.stock = null;
    state.push = null;
    state.pushDevices = null;
    feedback.info();
    showInfo('ok', `Bonjour ${user.prenom} ${user.nom}`,
      canSeeStock() ? `Rôle ${ROLE_LABELS[user.role] || 'gestion'} : l'état des stocks est sur l'accueil.` : 'Vous êtes connecté.');
    save();
    render();
    loadLots();
    if (canSeeStock()) loadStock();
    live.identity();
    afterLogin();
    if (state.pinReset) openPinReset();
    if (state.pendingOpening) signOpening();
    state.reports = null;
    if (isAdmin()) loadReports(true);
    if (action) setTimeout(action, 0);
  }

  // ------------------------------------------------------------------------------------------------
  // PIN bloque apres trop d'essais : l'utilisateur envoie a un admin la photo de l'ecran (QR code du lien de
  // deblocage) ; l'admin scanne le lien, se connecte avec son badge et reinitialise le PIN. L'utilisateur
  // en choisit alors un nouveau a sa prochaine connexion.

  // Code oublie : le PIN est bloque comme apres trop d'essais, et l'ecran du lien de deblocage s'affiche
  async function forgotPin(matricule, key) {
    if (!confirm("Votre code PIN sera bloqué jusqu'à ce que le TECH le réinitialise. Continuer ?")) return;
    $('#pin').close();
    try {
      await api('pin-forgot/', { matricule, key });
    } catch (e) {
      if (e.data && e.data.pin_blocked) {
        feedback.info();
        showInfo('bad', 'Code PIN oublié', e.message);
        showPinBlocked(e.data.pin_reset || {});
        return;
      }
      if (e.data && e.data.pin_setup_required) { askPin(matricule, key, true, ''); return; }
      feedback.bad();
      showInfo('bad', 'Code oublié', e.message);
    }
  }

  function showPinBlocked(reset) {
    const dialog = $('#pin-blocked');
    $('#pin-blocked-title').textContent = reset.forgotten ? 'Code PIN oublié' : 'Code PIN bloqué';
    $('#pin-blocked-text').textContent = `${reset.name || ''} (${reset.matricule || ''}) : `
      + (reset.forgotten ? 'code PIN oublié. ' : 'trop de codes PIN faux. ')
      + 'Le TECH doit réinitialiser votre PIN, vous en choisirez un nouveau à la prochaine connexion.';
    $('#pin-blocked-notified').hidden = !reset.notified;
    $('#pin-blocked-contact').textContent = reset.contact ? `à ${reset.contact}` : 'au TECH';
    const link = $('#pin-blocked-link');
    link.href = reset.url || '';
    link.textContent = reset.url || '';
    const image = $('#pin-blocked-qr');
    image.hidden = !reset.url || typeof qrcode !== 'function';
    if (!image.hidden) {
      const qr = qrcode(0, 'M');
      qr.addData(reset.url);
      qr.make();
      image.src = qr.createDataURL(8, 4);
    }
    const share = $('#pin-blocked-share');
    share.hidden = !reset.url || !navigator.share;
    share.onclick = () => navigator.share({
      title: 'QRProtec : PIN bloqué',
      text: `Débloquer le code PIN de ${reset.name || reset.matricule}`,
      url: reset.url,
    }).catch(() => { /* partage annule */ });
    $('#pin-blocked-close').onclick = () => dialog.close();
    if (!dialog.open) dialog.showModal();
  }

  function startPinReset(matricule, token) {
    state.pinReset = { matricule, token };
    try { sessionStorage.setItem(STORAGE_PIN_RESET, JSON.stringify(state.pinReset)); } catch (e) { /* ignore */ }
    return openPinReset();
  }

  function endPinReset() {
    state.pinReset = null;
    try { sessionStorage.removeItem(STORAGE_PIN_RESET); } catch (e) { /* ignore */ }
    $('#pin-reset').close();
  }

  async function openPinReset() {
    const request = state.pinReset;
    if (!request) return;
    if (!isAdmin()) {
      feedback.info();
      showInfo('warn', "Déblocage d'un code PIN", 'Scannez votre badge administrateur pour continuer.');
      return;
    }
    const body = { user: badge(), matricule: request.matricule, token: request.token };
    let info;
    try {
      info = await api('pin-reset/', body);
    } catch (e) {
      if (e.data && e.data.pin_required) {
        // session de l'admin expiree : il rescanne son badge, le deblocage reprend ensuite
        state.user = null;
        save();
        render();
        showInfo('warn', "Déblocage d'un code PIN", 'Session expirée : scannez à nouveau votre badge administrateur.');
        return;
      }
      endPinReset();
      feedback.bad();
      showInfo('bad', 'Déblocage impossible', e.message);
      return;
    }
    feedback.info();
    const dialog = $('#pin-reset');
    const fact = (label, value) => [el('dt', {}, label), el('dd', {}, value)];
    $('#pin-reset-body').replaceChildren(
      el('dl', { class: 'facts' },
        fact('Utilisateur', `${info.prenom} ${info.nom}`),
        fact('Matricule', info.matricule),
        fact('Rôle', info.role_label),
        fact('Bloqué', info.blocked_since ? cap(fmtDateTime(info.blocked_since, { long: true })) : '-'),
        fact('Essais faux', String(info.failures)),
        info.contact ? fact('Admin à contacter', info.contact) : [],
        info.active ? [] : fact('Compte', 'désactivé')),
      el('p', { class: 'hint' }, "Vérifiez que la demande vient bien de cette personne. Après réinitialisation, elle "
        + 'choisira un nouveau PIN à sa prochaine connexion avec son badge. Si le badge a pu être perdu ou volé, '
        + 'renouvelez plutôt le badge depuis le poste.'));
    $('#pin-reset-error').textContent = '';
    const confirmButton = $('#pin-reset-confirm');
    confirmButton.disabled = false;
    confirmButton.onclick = async () => {
      confirmButton.disabled = true;
      try {
        await api('pin-reset/', { ...body, confirm: true });
      } catch (e) {
        confirmButton.disabled = false;
        $('#pin-reset-error').textContent = e.message;
        return;
      }
      endPinReset();
      feedback.good();
      showInfo('ok', 'Code PIN réinitialisé', `${info.prenom} ${info.nom} choisira un nouveau PIN à sa prochaine connexion.`);
    };
    $('#pin-reset-cancel').onclick = endPinReset;
    if (!dialog.open) dialog.showModal();
  }

  // ------------------------------------------------------------------------------------------------
  // Actions

  function blockers(needItems = true) {
    const missing = [];
    if (!state.lot) missing.push("scannez l'étiquette du lot");
    else if (!keyOk()) {
      const lot = keyMissing()[0];
      missing.push(`scannez l'étiquette clé ${lot.id === state.lotId ? 'du lot' : 'de ' + lot.name}, un QR code rangé à l'intérieur du lot`
        + (lot.key_location ? ` (${lot.key_location})` : ''));
    }
    if (!hasIdentity()) missing.push(state.declaredAllowed ? 'scannez votre badge ou indiquez votre nom' : 'scannez votre badge');
    if (needItems && !scannedIids().size) missing.push('scannez au moins un item');
    return missing;
  }

  // partial : verif partielle, seuls les lots rendus complets par les scans sont verifies (les autres items
  // scannes sont ajoutes a leur lot comme un reassort)
  // Sans badge, si le serveur l'autorise : le nom est demande une fois et garde sur ce navigateur
  function askDeclaredName() {
    const name = (prompt('Sans badge : indiquez votre nom (prénom et nom).', state.declaredName) || '').trim();
    if (!name) return false;
    state.declaredName = name.slice(0, 30);
    save(); render();
    return true;
  }

  function ensureIdentity() {
    return hasIdentity() || !state.declaredAllowed || askDeclaredName();
  }

  // Le serveur a refuse l'identite : il dit si le nom declare est (encore) accepte
  function identityRefused(e) {
    if (!e.data || !e.data.login_required) return;
    state.declaredAllowed = !!e.data.declared_identity;
  }

  // Vérif à plusieurs : chacun valide sa partie, le dernier participant connecte envoie la vérif commune.
  // auto : le serveur demande l'envoi (les autres participants sont partis), sans redemander de confirmation.
  async function validate(partial = false, auto = false) {
    if (state.busy) return;
    if (!auto && !ensureIdentity()) return;
    const missing = blockers(false);
    if (missing.length) {
      if (auto) live.unvalidate();
      feedback.warn(); toast('Pour valider : ' + missing.join(', ') + '.', true); return;
    }
    const team = live.inTeam();
    if (team && !auto) {
      state.busy = true;
      render();
      const answer = await live.validate(partial);
      state.busy = false;
      if (!answer) { render(); toast('Connexion à la vérif commune perdue : réessayez.', true); return; }
      if (!answer.submit) {
        state.waitingTeam = true;
        feedback.good();
        showInfo('ok', '✔ Votre partie de la vérif est validée',
          answer.waiting.length ? `En attente de ${answer.waiting.join(', ')}.` : 'En attente des autres participants.',
          'Le compte rendu s\'affichera quand tout le monde aura validé.');
        render();
        return;
      }
    }
    const lots = sessionLots();
    const plan = planSession();
    const verified = partial ? plan.lots.filter((row) => row.complete).map((row) => row.lot)
      : lots.filter((lot) => !plan.intact.has(lot.id));
    const sealed = verified.filter((lot) => lot.is_sealed).map((lot) => lot.name);
    const confirmed = () => {
      if (auto) return true;
      if (sealed.length && !confirm(`${sealed.join(', ')} : scellé, valider une vérif brisera le scellé. Continuer ?`)) return false;
      if (partial) {
        const names = verified.filter((lot) => lot.items && (lot.requirements.length || lot.items.length)).map((lot) => lot.name);
        return confirm(`Vérif partielle : seuls ${names.join(', ') || 'les lots complets'} seront vérifiés.\n`
          + 'Les autres items scannés sont ajoutés à leur lot (réassort, vérif recommandée). Continuer ?');
      }
      const expected = lots.reduce((sum, lot) => sum + expectedGroups(lot, plan).remaining, 0);
      const what = isMulti() ? 'les lots seront incomplets' : 'le lot sera incomplet';
      return !expected || confirm(`${expected} item(s) attendu(s) manquent : ${what}. Valider quand même ?`);
    };
    if (!confirmed()) { if (team || auto) live.unvalidate(); return; }
    state.busy = true;
    render();
    try {
      const credentials = live.credentials();
      const report = await api('verifs/', {
        lots: sessionEntries(),
        ...identity(),
        items: [...scannedIids()],
        places: scanPlaces(),
        // etiquettes de scelle scannees : lots scelles intacts, comptes comme verifies
        sealed: allScanned().filter((entry) => entry.kind === 'seal' && !entry.error).map((entry) => ({ id: entry.iid, code: entry.key })),
        partial,
        ...(credentials ? { live: credentials } : {}),
      });
      state.waitingTeam = false;
      state.scanned = [];
      showReport(report);
      live.done(report);
      forgetLot('Vérif enregistrée. Scannez un item ou l\'étiquette d\'un lot pour en commencer une autre.');
      loadLots();
    } catch (e) {
      identityRefused(e);
      if (team || auto) live.unvalidate();
      state.waitingTeam = false;
      feedback.bad();
      toast('Vérif refusée : ' + e.message, true);
    } finally {
      state.busy = false;
      save();
      render();
    }
  }

  // Sans lot scanne : les items scannes sont sortis du stock (ex : pris pour une intervention). Ils redeviennent
  // normaux s'ils reapparaissent a une verif, sinon ils comptent comme utilises au bout d'un mois (ou a peremption).
  // Vérif en cascade : scans compris dans la validation des lots scope (lieu du scan : le lot ou l'on etait entre)
  const scopeEntries = (scope) => allScanned().filter((entry) => !entry.error && scope.has(entry.place || state.lotId));

  function entriesPayload(entries) {
    const items = new Set();
    const places = {};
    const sealed = [];
    for (const entry of entries) {
      if (entry.kind === 'seal') { sealed.push({ id: entry.iid, code: entry.key }); continue; }
      for (const iid of entry.kind === 'item' ? [entry.iid] : entry.items || []) {
        items.add(iid);
        if (entry.place) places[iid] = entry.place;
      }
    }
    return { items: [...items], places, sealed };
  }

  // Lots valides par une validation de la vérif en cascade (compte rendu du serveur) : notes, et leurs scans retires
  function stepDone(report, sent, by = '') {
    const at = new Date().toISOString();
    for (const row of report.lots || []) {
      if (row.verified || row.sealed) state.done[row.id] = { at, by, complete: !!row.complete, sealed: !!row.sealed };
    }
    const gone = new Set(sent);
    state.scanned = state.scanned.filter((entry) => !(entry.kind === 'pack' ? (entry.items || []).some((iid) => gone.has(iid))
      : gone.has(entry.iid)));
  }

  // Valider le lot en cours de la vérif en cascade (partial : vérif partielle ou reassort de ce lot). Il est enregistre
  // tout de suite, avec ses sous-lots commences ; on revient au lot qui le contient. Au lot global, la vérif se termine.
  // Vérif lancee sur un kit a scelle obligatoire du lot global, et l'on y est : sa validation termine la vérif
  const sealNextLevel = () => inCascade() && !!state.startedIn && levelId() === state.startedIn
    && !!(lotById(state.startedIn) || {}).seal_required;

  // Vérif en cascade : le lot ou l'on est (pas le lot global) est complet, rien d'attendu n'y manque, et peut etre
  // valide sans question
  function levelComplete() {
    if (!inCascade() || state.busy || levelId() === state.lotId) return false;
    const top = levelLot();
    const scope = scopeOf(top.id);
    if (!scopeEntries(scope).length && ownContent(top)) return false; // rien scanne ici
    if (blockers(false).length) return false;
    const plan = planSession();
    const lots = sessionLots().filter((lot) => scope.has(lot.id) && !plan.intact.has(lot.id));
    return !lots.some((lot) => lot.is_sealed) && lots.reduce((sum, lot) => sum + expectedGroups(lot, plan).remaining, 0) === 0;
  }

  // Scan d'un item ou d'une etiquette du lot `id`, hors du lot ou l'on est : les lots complets qu'on quitte ainsi
  // sont valides automatiquement (on remonte tant que `id` n'est pas dedans). Renvoie les noms des lots valides.
  // ask : un lot incomplet ou l'on a commence a scanner (et ou personne d'autre n'est) fait demander quoi faire
  // (leaveLevelPopup). Renvoie {names, stay} : stay, on reste dans le lot (choix « Rester »).
  async function leaveCompleteLevels(id, ask = false) {
    const names = [];
    while (!isUnder(id, levelId()) && levelId() !== state.lotId) {
      const before = levelLot();
      const complete = levelComplete();
      if (!complete) {
        const scope = scopeOf(before.id);
        if (!ask || !scopeEntries(scope).length || othersLevels()[before.id]) break;
        const choice = await leaveLevelPopup(before, lotById(id), scope);
        if (choice === 'stay') return { names, stay: true };
        if (choice !== 'validate') break;
        await validateStep(false, true);
      } else {
        await validateStep(false);
      }
      if (levelId() === before.id) break; // refusee
      names.push(`${before.name} ${complete ? 'complet' : 'incomplet'}`);
    }
    return { names, stay: false };
  }

  // Popup « Passer a … ? » : valider le lot incomplet, passer sans le valider, ou y rester. Ignoree, le scan suivant
  // vaut « Passer » (handleCode).
  let leavePopup = null; // resolve de la popup ouverte
  function leaveLevelPopup(level, target, scope) {
    const plan = planSession();
    const missing = sessionLots().filter((lot) => scope.has(lot.id) && !plan.intact.has(lot.id))
      .reduce((sum, lot) => sum + expectedGroups(lot, plan).remaining, 0);
    const dialog = $('#leave-level');
    $('#leave-level-title').textContent = `Passer à ${target ? target.name : 'un autre lot'} ?`;
    $('#leave-level-text').textContent = `${level.name} est incomplet : ${missing} item(s) attendu(s) pas encore scanné(s).`;
    $('#leave-level-validate').textContent = `Valider ${level.name} incomplet (les non scannés passent manquants)`;
    $('#leave-level-skip').textContent = `Passer sans valider (${level.name} reste à faire, scans gardés)`;
    $('#leave-level-stay').textContent = `Rester dans ${level.name}`;
    feedback.warn();
    return new Promise((resolve) => {
      if (leavePopup) leavePopup('skip');
      const close = (choice) => {
        if (leavePopup !== close) return;
        leavePopup = null;
        dialog.onclose = null;
        if (dialog.open) dialog.close();
        resolve(choice);
      };
      leavePopup = close;
      $('#leave-level-validate').onclick = () => close('validate');
      $('#leave-level-skip').onclick = () => close('skip');
      $('#leave-level-stay').onclick = () => close('stay');
      dialog.onclose = () => close('stay'); // Echap
      dialog.showModal();
    });
  }

  // confirmed : lot incomplet deja confirme (popup « Passer a … »)
  async function validateStep(partial = false, confirmed = false) {
    if (state.busy) return;
    if (!ensureIdentity()) return;
    const top = levelLot();
    const atRoot = top.id === state.lotId;
    const exclude = excludedFor(top.id);
    const scope = scopeOf(top.id, exclude);
    const entries = scopeEntries(scope);
    const pendingSubs = [...scope].some((id) => id !== top.id);
    if (!ownContent(top) && !top.storage && !entries.length && !pendingSubs) {
      // lot de regroupement sans rien a enregistrer : ses sous-lots sont deja faits (ou laisses tels quels)
      if (atRoot) finishCascade(); else { toast(`${top.name} : rien d'autre à valider.`); goUp(); }
      return;
    }
    const missing = blockers(false);
    if (missing.length) { feedback.warn(); toast('Pour valider : ' + missing.join(', ') + '.', true); return; }
    const plan = planSession();
    const lots = sessionLots().filter((lot) => scope.has(lot.id) && !plan.intact.has(lot.id));
    const verified = partial ? plan.lots.filter((row) => row.complete && scope.has(row.lot.id)).map((row) => row.lot) : lots;
    const sealedNames = verified.filter((lot) => lot.is_sealed).map((lot) => lot.name);
    if (sealedNames.length && !confirm(`${sealedNames.join(', ')} : scellé, valider une vérif brisera le scellé. Continuer ?`)) return;
    if (partial) {
      const names = verified.filter(ownContent).map((lot) => lot.name);
      if (!confirm(names.length ? `Vérif partielle : seuls ${names.join(', ')} seront vérifiés.\n`
        + 'Les autres items scannés sont ajoutés à leur lot (réassort, vérif recommandée). Continuer ?'
        : `Ajouter les items scannés à ${top.name} sans faire de vérif complète (réassort) ?\n`
          + 'Le lot sera signalé « vérif recommandée » pour la personne suivante.')) return;
    } else {
      const expected = lots.reduce((sum, lot) => sum + expectedGroups(lot, plan).remaining, 0);
      const subs = lots.filter((lot) => lot.id !== top.id && ownContent(lot)).map((lot) => lot.name);
      if (expected && !confirmed && !confirm(`${expected} item(s) attendu(s) manquent : ${top.name} sera incomplet`
        + (subs.length ? ` (avec ${subs.join(', ')})` : '') + '. Valider quand même ?')) return;
    }
    const payload = entriesPayload(entries);
    state.busy = true;
    render();
    try {
      const credentials = live.credentials();
      const report = await api('verifs/', {
        lots: [{ id: top.id, key: top.id === state.lotId ? state.lotKey : state.keys[top.id] || '' }],
        keys: sessionEntries().filter((entry) => entry.key && entry.id !== top.id),
        exclude,
        ...identity(),
        ...payload,
        partial,
        step: true,
        finish: atRoot && !partial, // lot global valide : on quitte la vérif commune cote serveur aussi
        ...(credentials ? { live: credentials } : {}),
      });
      stepDone(report, [...payload.items, ...payload.sealed.map((row) => row.id)]);
      state.scanned = state.scanned.filter((entry) => !(entry.kind === 'seal' && payload.sealed.some((row) => row.id === entry.iid)));
      const done = (report.lots || []).filter((row) => row.verified);
      const incomplete = done.filter((row) => !row.complete);
      const finished = !partial || done.some((row) => row.id === top.id);
      (incomplete.length ? feedback.warn : feedback.good)();
      showInfo(incomplete.length ? 'warn' : 'ok',
        `${finished ? '✔' : '↷'} ${top.name} ${finished ? 'validé' : done.length ? ': vérif partielle enregistrée' : ': réassort enregistré'}`
          + (incomplete.length ? ', incomplet' : ''),
        done.length > 1 ? `Lots vérifiés : ${done.map((row) => row.name).join(', ')}` : '',
        report.missing && report.missing.length ? `${report.missing.length} item(s) manquant(s)` : '',
        atRoot && finished ? '' : finished ? `Retour à ${(lotById(parentIdOf(top)) || state.lot).name}.` : '');
      if (incomplete.length || (report.unknown || []).length) showReport(report);
      // kit a scelle obligatoire dont on a lance la vérif (scanne dans le VPS) : valide complet, la vérif s'arrete la,
      // sur le compte rendu ; le lot est a resceller (on n'a vu que l'etiquette exterieure des autres lots)
      const sealNext = !atRoot && finished && sealNextLevel() && done.some((row) => row.id === top.id && row.complete);
      if (sealNext) {
        showReport(report);
        finishCascade(report, `${top.name} validé complet : à resceller (le TECH le scelle depuis le poste).`);
      } else if (atRoot && finished) {
        finishCascade(report);
      } else {
        if (finished && !atRoot) { state.path.pop(); state.current = levelId(); }
        await loadLot(state.lotId); // contenu et etat a jour des lots
      }
      loadLots();
    } catch (e) {
      identityRefused(e);
      feedback.bad();
      toast('Vérif refusée : ' + e.message, true);
    } finally {
      state.busy = false;
      save();
      render();
    }
  }

  // Etiquettes de scelle scannees dans la vérif en cascade : le lot scelle intact est enregistre tout de suite (s'il
  // manque l'etiquette cle ou l'identite, il le sera a la validation du lot ou il a ete scanne)
  let flushing = false;
  async function flushSeals() {
    if (!inCascade() || flushing || state.busy || !keyOk() || !hasIdentity()) return;
    const seals = state.scanned.filter((entry) => entry.kind === 'seal' && !entry.error && lotById(entry.iid));
    if (!seals.length) return;
    flushing = true;
    try {
      for (const entry of seals) {
        const credentials = live.credentials();
        const report = await api('verifs/', {
          lots: [{ id: entry.iid, key: state.keys[entry.iid] || '' }],
          keys: sessionEntries().filter((row) => row.key && row.id !== entry.iid),
          ...identity(),
          items: [],
          sealed: [{ id: entry.iid, code: entry.key }],
          step: true,
          ...(credentials ? { live: credentials } : {}),
        });
        stepDone(report, []);
        state.scanned = state.scanned.filter((other) => other !== entry);
      }
    } catch (e) {
      toast('Scellé non enregistré : ' + e.message, true);
    } finally {
      flushing = false;
      save();
      render();
    }
  }

  // Fin de la vérif en cascade (lot global valide, ou plus rien a faire) : on quitte la vérif commune, les autres
  // participants continuent la leur
  function finishCascade(report = null, message = '') {
    const done = Object.values(state.done).length;
    const name = state.lot ? state.lot.name : '';
    const left = state.scanned.length;
    live.quit();
    state.scanned = [];
    resetCascade();
    forgetLot(message || `Vérif de ${name} terminée : ${done} lot(s) validé(s)` + (left ? `, ${left} scan(s) non validé(s) oublié(s).` : '.'));
    if (!report) feedback.good();
  }

  async function takeOut() {
    if (!ensureIdentity()) return;
    const iids = [...scannedIids()];
    const missing = [];
    if (!hasIdentity()) missing.push(state.declaredAllowed ? 'scannez votre badge ou indiquez votre nom' : 'scannez votre badge');
    if (!iids.length) missing.push('scannez au moins un item');
    if (missing.length) { feedback.warn(); toast('Pour sortir : ' + missing.join(', ') + '.', true); return; }
    if (!confirm(`Sortir ${iids.length} item(s) du stock, sans lot ?\n`
      + "Ils redeviennent normaux s'ils sont scannés à une vérif ; sinon ils seront comptés comme utilisés "
      + 'au bout d\'un mois (ou à leur péremption).')) return;
    state.busy = true;
    render();
    try {
      const result = await api('items/out/', { ...identity(), items: iids });
      state.scanned = [];
      feedback.good();
      showInfo('ok', `${result.out.length} item(s) sorti(s) du stock`,
        `Scannez-les à une vérif dans les ${result.days} jours, sinon ils seront comptés comme utilisés.`,
        ...(result.refused || []).map((row) => `Non sorti : ${row.iid} (${row.reason})`));
      if (canSeeStock()) loadStock();
    } catch (e) {
      identityRefused(e);
      feedback.bad();
      toast('Sortie refusée : ' + e.message, true);
    } finally {
      state.busy = false;
      save();
      render();
    }
  }

  // Seulement des items qui ne sont pas dans les lots de la verif : reassort plutot que verif
  function onlyNewItems() {
    if (!state.lot || !allScanned().length) return false;
    const known = knownIids();
    const iids = [...scannedIids()];
    return iids.length > 0 && !iids.some((iid) => known.has(iid));
  }

  // {iid: lot de la derniere etiquette scannee avant lui} envoye avec la vérif (services.assign_items)
  function scanPlaces() {
    const places = {};
    for (const entry of allScanned()) {
      if (entry.error || !entry.place) continue;
      for (const iid of entry.kind === 'item' ? [entry.iid] : entry.items || []) places[iid] = entry.place;
    }
    return places;
  }

  function knownIids() {
    return new Set(sessionLots().flatMap((lot) => [...(lot.items || []), ...(lot.missing_items || [])].map((item) => item.iid)));
  }

  // Verif partielle : des lots sont complets, mais pas tous (verif groupee seulement)
  function partialLots() {
    if (!isMulti() || !allScanned().length) return [];
    const plan = planSession();
    if (plan.lots.every((row) => row.complete)) return [];
    return plan.lots.filter((row) => row.complete && row.touched > 0).map((row) => row.lot);
  }

  async function addToLot() {
    if (!ensureIdentity()) return;
    const missing = blockers(true);
    if (missing.length) { feedback.warn(); toast('Pour ajouter : ' + missing.join(', ') + '.', true); return; }
    try {
      const result = await api(`lots/${encodeURIComponent(state.lot.id)}/add/`, {
        key: state.lotKey,
        ...identity(),
        items: [...scannedIids()],
      });
      state.scanned = [];
      feedback.good();
      toast(`${result.moved.length} item(s) ajouté(s) au lot : vérif complète recommandée.`);
      await loadLot(state.lot.id);
      loadLots();
    } catch (e) {
      identityRefused(e);
      feedback.bad();
      toast('Ajout refusé : ' + e.message, true);
    }
    save();
    render();
  }

  function iidLabel(iid) {
    const known = sessionLots().flatMap((lot) => lot.items || []).find((item) => item.iid === iid);
    const entry = allScanned().find((e) => e.iid === iid && e.info);
    const info = (known) || (entry && entry.info);
    return info ? `${info.type_name} – ${info.peremption ? fmtDate(info.peremption) : 'non périssable'}` : iid;
  }

  function showReport(report) {
    const section = (title, list) => list && list.length
      ? [el('h3', {}, `${title} (${list.length})`), el('ul', {}, list.map((iid) => el('li', {}, iidLabel(iid))))]
      : [];
    const body = $('#report-body');
    body.replaceChildren(
      el('h2', { style: `color:var(--${report.complete ? 'green' : report.partial ? 'orange' : 'red'})` },
        report.complete ? ((report.lots || []).length > 1 ? '✅ Lots complets' : '✅ Lot complet')
          : report.partial ? '↷ Vérif partielle enregistrée' : '⚠️ Lot NON complet'),
      el('p', {}, `${report.present.length} item(s) présent(s).`),
      (report.participants || []).length > 1 ? el('p', {}, `👥 Vérif à plusieurs : ${report.participants.join(', ')}.`) : '',
      (report.sealed_lots || []).length ? el('p', {}, `🔒 Scellé intact, compté comme vérifié : ${report.sealed_lots.join(', ')}.`) : '',
      report.unsealed ? el('p', { style: 'color:var(--orange)' },
        `🔓 Scellé brisé par cette vérif : ${(report.unsealed_lots || []).join(', ') || 'lot'}.`) : '',
      ...((report.lots || []).length > 1 ? [el('h3', {}, report.partial ? 'Vérif partielle' : 'Lots vérifiés'),
        el('ul', {}, report.lots.map((row) => el('li', {}, '  '.repeat(row.depth || 0)
          + (row.sealed ? `🔒 ${row.name} : scellé intact`
            : row.verified ? (row.complete ? '✔ ' : '✘ ') + row.name + (row.complete ? ' : complet' : ' : incomplet')
            : `↷ ${row.name} : non vérifié` + (row.restocked ? `, ${row.restocked} item(s) ajouté(s) (réassort)` : '')))))] : []),
      ...(report.requirements || []).map((row) => requirementRow(
        (row.lot_name ? row.lot_name + ' · ' : '') + row.type_name, row.present, row.required)),
      ...section('Périmés ou inutilisables encore dans le lot : à remplacer', report.expired),
      ...section('Périmés remplacés', report.replaced),
      // etiquette a dechirer absente : l'ensemble a ete entame, il compte comme utilise
      ...section('Étiquette déchirée : utilisés', report.torn),
      ...section('Attendus mais non scannés', report.missing.filter((iid) => !(report.torn || []).includes(iid))),
      // non consommable absent du lot : retire du lot, hors lot (pas perdu) jusqu'a son retour en stock
      ...section('Non consommables absents : hors lot (à remettre en stock)', report.loose),
      ...section('Retrouvés', report.reactivated),
      ...section('Ajoutés en réassort', report.restocked),
      ...section('Codes inconnus ignorés', report.unknown),
    );
    if (report.complete) feedback.good(); else feedback.warn();
    $('#report').showModal();
  }

  // Plus de lot selectionne (« Changer de lot », ou vérif enregistree : le lot n'est plus « en cours »)
  function forgetLot(message = "Scannez l'étiquette d'un lot.") {
    state.lot = null; state.lotId = ''; state.lotKey = ''; state.extra = []; resetCascade();
    state.lastVerif = null; state.launched = ''; state.current = ''; state.askPlace = false;
    showInfo('empty', message);
    save(); render();
  }

  $('#report-close').addEventListener('click', () => $('#report').close());
  $('#validate').addEventListener('click', () => (takingOut() ? takeOut() : inCascade() ? validateStep(false) : validate(false)));
  $('#restock').addEventListener('click', () => {
    if (inCascade()) { validateStep(true); return; }
    if (partialLots().length) { validate(true); return; }
    if (!confirm("Ajouter les items scannés sans faire de vérif complète (réassort) ?\n"
      + 'Le lot sera signalé « vérif recommandée » pour la personne suivante.')) return;
    if (isMulti()) validate(true); else addToLot();
  });
  $('#more').addEventListener('click', () => $('#menu').showModal());
  $('#notifications-close').addEventListener('click', () => $('#notifications').close());
  $('#menu').addEventListener('click', (event) => {
    const action = event.target.dataset && event.target.dataset.action;
    if (!action) return;
    $('#menu').close();
    if (action === 'add') addToLot();
    if (action === 'manual') {
      const code = prompt("Code de l'item, id du lot ou URL d'une étiquette :");
      if (code && code.trim()) {
        const value = code.trim();
        // un identifiant de lot seul (sans URL) est accepte pour ouvrir un lot sans etiquette
        if (!IID_RE.test(value) && !/^https?:/.test(value)) scanLot({ kind: 'lot', code: value, id: value, key: '' });
        else handleCode(value);
      }
    }
    if (action === 'notifications') openNotifications();
    if (action === 'forget-lot') forgetLot();
    if (action === 'logout') logout();
  });

  $('#user-chip').addEventListener('click', () => {
    if (state.user && confirm(`Déconnecter ${state.user.prenom} ${state.user.nom} ?`)) {
      logout();
    } else if (declared()) {
      if (confirm(`Oublier le nom « ${state.declaredName} » ? Scannez votre badge pour vous identifier.`)) {
        state.declaredName = '';
        save(); render();
      }
    } else if (!state.user) {
      // scannez votre badge, ou sans badge : nom et PIN (ou nom seul si le serveur l'autorise)
      openNameLogin();
    }
  });

  // Nouveau scan (ou scan retire) apres avoir valide sa partie : la partie n'est plus validee
  function scannedAgain() {
    if (!state.waitingTeam) return;
    state.waitingTeam = false;
    live.unvalidate();
  }

  function undoLastScan() {
    if (state.scanned.length) scannedAgain();
    const last = state.scanned.pop();
    if (last) {
      const name = last.kind === 'pack' && last.info ? `paquet ${last.info.count} × ${last.info.type_name}`
        : last.info && last.info.type_name ? `${last.info.type_name} (${last.iid})` : last.iid || 'code inconnu';
      toast('Dernier scan annulé : ' + name);
    }
    save(); render();
  }
  $('#undo').addEventListener('click', undoLastScan);
  $('#undo-last').addEventListener('click', undoLastScan);

  $('#clear').addEventListener('click', () => {
    if (!state.scanned.length || !confirm('Vider la liste des items scannés ?')) return;
    scannedAgain();
    state.scanned = [];
    save(); render();
  });

  function switchTab(tab) {
    state.tab = tab;
    document.querySelectorAll('#tabs button').forEach((b) => b.classList.toggle('active', b.dataset.tab === tab));
    document.querySelectorAll('.view').forEach((v) => v.classList.toggle('active', v.dataset.view === tab));
  }
  document.querySelectorAll('#tabs button').forEach((b) => b.addEventListener('click', () => switchTab(b.dataset.tab)));

  // ------------------------------------------------------------------------------------------------
  // Affichage

  function requirementRow(label, present, required, after = null) {
    const ratio = required > 0 ? Math.min(1, present / required) : 1;
    const color = present <= 0 ? 'var(--red)' : present < required ? 'var(--orange)' : 'var(--green)';
    return el('div', { class: 'req' },
      el('span', { class: 'label' }, label),
      el('div', { class: 'bar' },
        el('i', { style: `width:${present <= 0 ? 100 : ratio * 100}%;background:${color}` }),
        el('span', { style: present <= 0 ? 'color:#fff' : '' }, `${present}/${required}`)), after);
  }

  // Items frais scannes par type (un paquet compte pour son nombre d'items)
  function freshByType() {
    const fresh = {};
    for (const entry of allScanned()) {
      if (entry.error || entry.expired) continue;
      const type = entry.kind === 'item' ? entry.iid.slice(0, 6) : entry.info && entry.info.type;
      const count = entry.kind === 'pack' ? (entry.items || []).length : 1;
      if (type) fresh[type] = (fresh[type] || 0) + count;
    }
    return fresh;
  }

  // Attendus d'un lot : la definition de son type (quantite par type), puis les items deja connus du lot.
  // Les items scannes comptent pour le lot ou ils seront ranges (planSession).
  function expectedGroups(lot, plan) {
    if (plan.intact.has(lot.id)) return { groups: [], others: [], remaining: 0, complete: true, sealed: true };
    const done = scannedIids();
    const row = plan.lots.find((entry) => entry.lot.id === lot.id);
    if (lot.storage) return storageGroups(lot, done, row);
    const fresh = row ? row.fresh : {};
    const known = {};
    for (const item of lot.items || []) {
      if (done.has(item.iid)) continue;
      (known[item.type] = known[item.type] || []).push(item);
    }
    let remaining = 0;
    const groups = (lot.requirements || []).map((requirement) => {
      const items = (known[requirement.type] || []).sort((a, b) => b.expired - a.expired);
      delete known[requirement.type];
      const scanned = fresh[requirement.type] || 0;
      const missing = Math.max(0, requirement.required - scanned);
      const knownFresh = items.filter((item) => !item.expired).length;
      remaining += missing;
      return { row: requirement, items, scanned, missing, fromStock: Math.max(0, missing - knownFresh) };
    });
    const others = Object.values(known).flat();
    remaining += others.length;
    return { groups, others, remaining, complete: row ? row.complete : false };
  }

  // Rangement du stock : pas de contenu attendu par type, chaque item qui y est range est attendu (on verifie qu'il
  // est toujours la). Un type par groupe, compte sur les items connus du rangement.
  function storageGroups(lot, done, row) {
    const byType = {};
    for (const item of lot.items || []) {
      const group = byType[item.type] = byType[item.type]
        || { row: { type: item.type, type_name: item.type_name, location: '', required: 0 }, items: [], scanned: 0 };
      group.row.required += 1;
      if (done.has(item.iid)) group.scanned += 1;
      else group.items.push(item);
    }
    const groups = Object.values(byType).sort((a, b) => a.row.type_name.localeCompare(b.row.type_name, 'fr'));
    let remaining = 0;
    for (const group of groups) {
      group.items.sort((a, b) => b.expired - a.expired);
      group.missing = group.row.required - group.scanned;
      group.fromStock = 0;
      remaining += group.missing;
    }
    return { groups, others: [], remaining, complete: row ? row.complete : false };
  }

  // Item attendu, dans la liste depliable de son type : seulement son iid, en petit (le type est dans le titre)
  function todoItem(item) {
    return el('li', { class: 'iid' + (item.expired ? ' expired' : '') },
      reportMark({ item: item.iid, label: `${item.type_name} (${item.iid})`, lot: item.location, lotName: item.location_name }),
      el('span', { class: 'main' }, item.iid),
      item.expired ? el('span', { class: 'tag red' }, badLabel(item)) : null,
      item.reported && !item.unusable ? el('span', { class: 'tag orange', title: 'Problème signalé, toujours utilisable' }, 'signalé') : null,
      item.tear_off ? el('span', { class: 'tag orange', title: 'Étiquette à déchirer : si elle manque, compté comme utilisé' }, 'à déchirer') : null,
      item.consumable === false ? el('span', { class: 'tag blue', title: 'Non consommable : s\'il manque, il passe hors lot (pas perdu)' }, 'non consommable') : null,
      (item.tasks || []).length ? el('span', { class: 'tag blue', title: 'Tâches à cocher au scan de cet item' }, 'tâches') : null,
      item.missed_verifs > 0 ? el('span', { class: 'tag orange' }, 'non vu') : null);
  }

  // Types dont la liste des iids attendus est depliee (repliee par defaut)
  const openGroups = new Set();

  // Titre d'un type attendu : touche pour deplier ses iids
  function groupHead(key, cls, items, main, count) {
    const open = openGroups.has(key);
    const toggle = items.length ? () => { if (open) openGroups.delete(key); else openGroups.add(key); renderTodo(); } : null;
    return [
      el('li', { class: 'group ' + cls + (toggle ? ' foldable' : ''), onclick: toggle, 'aria-expanded': toggle ? String(open) : null },
        el('span', { class: 'chevron' }, toggle ? (open ? '▾' : '▸') : ''), main, count),
      ...(open ? items.map(todoItem) : []),
    ];
  }

  // Attendus d'un lot par type : en cours (orange, dernier type scanne en haut), puis rien de scanne (rouge), puis
  // les autres items du lot, et les types complets (vert) en bas
  function lotGroupRows(lot, groups, others, lastScan) {
    const rows = [];
    const lastScanOf = (group) => (group.row.type in lastScan ? lastScan[group.row.type] : -1);
    const rank = (group) => (group.missing === 0 ? 2 : group.scanned === 0 ? 1 : 0);
    groups.sort((a, b) => rank(a) - rank(b)
      || (rank(a) === 0 ? lastScanOf(b) - lastScanOf(a) : 0));
    const groupRows = (group) => {
      const expired = group.items.filter((item) => item.expired).length;
      return groupHead(`${lot.id}:${group.row.type}`, rank(group) === 2 ? 'ok' : rank(group) === 1 ? 'bad' : 'partial', group.items,
        el('div', { class: 'main' },
          el('div', { class: 'name' }, group.row.type_name),
          el('div', { class: 'sub' }, [group.row.location ? '📍 ' + group.row.location : '',
            group.missing === 0 ? 'complet' : `encore ${group.missing} à scanner`,
            expired ? `${expired} périmé(s)` : '',
            group.fromStock > 0 ? `${group.fromStock} à prendre dans le stock` : ''].filter(Boolean).join(' · '))),
        [reportMark({ lot: lot.id, lotName: lot.name, type: group.row.type, label: `${group.row.type_name} (${lot.name})` }),
          el('span', { class: 'count' }, `${group.scanned}/${group.row.required}`)]);
    };
    for (const group of groups) if (rank(group) < 2) rows.push(...groupRows(group));
    if (others.length) {
      rows.push(...groupHead(`${lot.id}:others`, 'other', others,
        el('div', { class: 'main' }, 'Autres items du lot (hors définition)'),
        el('span', { class: 'count' }, String(others.length))));
    }
    for (const group of groups) if (rank(group) === 2) rows.push(...groupRows(group));
    return rows;
  }

  // rang du dernier scan de chaque type d'item
  function lastScanRanks() {
    const lastScan = {};
    allScanned().forEach((entry, index) => {
      const type = entry.kind === 'item' ? entry.iid.slice(0, 6) : entry.info && entry.info.type;
      if (type && !entry.error) lastScan[type] = index;
    });
    return lastScan;
  }

  function renderTodo() {
    const list = $('#todo-list');
    if (!state.lot) {
      list.replaceChildren(el('li', { class: 'empty' }, "Scannez l'étiquette d'un lot pour afficher son contenu."));
      return 0;
    }
    if (inCascade()) return renderCascade(list);
    const last = state.lastVerif;
    if (last && last.lotId === state.lot.id && !allScanned().length) {
      const at = new Date(last.at).toLocaleTimeString('fr-FR', { hour: '2-digit', minute: '2-digit' });
      const what = last.partial ? `vérif partielle (${last.lots} lot(s) vérifié(s))`
        : `${last.lots > 1 ? 'lots' : 'lot'} ${last.complete ? 'complet' + (last.lots > 1 ? 's' : '') : 'incomplet'}`;
      list.replaceChildren(
        el('li', { class: 'group ' + (last.complete ? 'ok' : 'bad') },
          el('div', { class: 'main' },
            el('div', { class: 'name' }, `${last.complete ? '✔' : '✘'} Vérif enregistrée à ${at}`),
            el('div', { class: 'sub' }, `${last.present} item(s) présent(s) · ${what}`))),
        el('li', { class: 'empty' }, 'Scannez un item pour commencer une nouvelle vérif.'));
      return 0;
    }
    const plan = planSession();
    const lots = sessionLots();
    const multi = lots.length > 1;
    const rows = [];
    const lastScan = lastScanRanks();
    let remaining = 0;
    for (const lot of lots) {
      const { groups, others, remaining: left, complete, sealed } = expectedGroups(lot, plan);
      remaining += left;
      if (sealed) {
        rows.push(el('li', { class: 'lot-head ok', style: `margin-left:${(lot.depth || 0) * 12}px` },
          el('div', { class: 'main' }, el('div', { class: 'name' }, '🔒 ' + lot.name),
            el('div', { class: 'sub' }, 'scellé intact : compté comme vérifié'))));
        continue;
      }
      // verif groupee : un titre par lot (un lot qui ne contient rien et n'attend rien n'est qu'un regroupement)
      if (multi) {
        if (!groups.length && !others.length && !(lot.items || []).length) continue;
        rows.push(el('li', { class: 'lot-head ' + (complete ? 'ok' : left ? 'bad' : 'partial'), style: `margin-left:${(lot.depth || 0) * 12}px` },
          el('div', { class: 'main' },
            el('div', { class: 'name' }, '📦 ' + lot.name),
            el('div', { class: 'sub' }, complete ? 'complet avec les scans' : `encore ${left} à scanner`))));
      }
      rows.push(...lotGroupRows(lot, groups, others, lastScan));
    }
    if (!remaining) rows.unshift(el('li', { class: 'group ok' }, multi ? '✅ Tout est scanné : les lots seront complets.' : '✅ Tout est scanné : le lot sera complet.'));
    list.replaceChildren(...rows);
    return remaining;
  }

  // Vérif en cascade : le lot en cours (fil depuis le lot global, retour au lot qui le contient), ses sous-lots (faits
  // en vert, en cours, a faire ; touchez-en un ou scannez son etiquette pour y entrer), puis ses propres attendus.
  function renderCascade(list) {
    const plan = planSession();
    const level = levelLot();
    const chain = [state.lot, ...state.path.map(lotById).filter(Boolean)];
    const parent = chain.length > 1 ? chain[chain.length - 2] : null;
    const rows = [];
    const crumbs = [];
    chain.forEach((lot, index) => {
      if (index) crumbs.push(el('span', { class: 'sep' }, ' › '));
      crumbs.push(index < chain.length - 1
        ? el('button', { type: 'button', class: 'text-link', onclick: () => { state.path = chainTo(lot.id); state.current = levelId(); save(); render(); } }, lot.name)
        : el('b', {}, lot.name));
    });
    rows.push(el('li', { class: 'cascade-path' },
      el('div', { class: 'main' }, el('div', { class: 'crumbs' }, ...crumbs),
        el('div', { class: 'sub' }, sealNextLevel() ? `Validez ${level.name} : la vérif s'arrête là, le lot est ensuite à resceller.`
          : parent ? `Validez ${level.name} pour revenir à ${parent.name}.`
          : childrenOf(level.id).length ? 'Scannez l\'étiquette d\'un sous-lot pour y entrer.' : '')),
      parent ? el('button', { type: 'button', class: 'secondary', title: `Revenir à ${parent.name} sans valider ${level.name} (ses scans sont gardés)`,
        onclick: goUp }, '← ' + (parent.name_short || parent.name)) : null));
    const others = othersLevels();
    let left = 0;
    for (const child of childrenOf(level.id)) {
      const ids = subtreeIds(child.id);
      const done = state.done[child.id];
      const sealed = plan.intact.has(child.id);
      const who = [...new Set(ids.flatMap((id) => others[id] || []))];
      const scanned = allScanned().filter((entry) => !entry.error && entry.kind !== 'seal' && ids.includes(entry.place)).length;
      const subs = childrenOf(child.id).length;
      let cls; let sub;
      if (done) {
        cls = done.complete || done.sealed ? 'ok' : 'bad';
        sub = done.sealed ? `🔒 Scellé intact constaté ${fmtOn(done.at)}`
          : `✔ Validé ${fmtOn(done.at)}${done.by ? ' par ' + done.by : ''}${done.complete ? '' : ' · incomplet'}`;
      } else if (sealed) {
        cls = 'ok';
        sub = '🔒 Scellé intact : enregistré à la validation';
      } else {
        left += 1;
        if (who.length) { cls = 'partial'; sub = `En cours · ${who.join(', ')}`; }
        else if (touched(child.id)) { cls = 'partial'; sub = `Commencé · ${scanned} scanné(s), pas encore validé`; }
        else { cls = 'todo'; sub = 'À faire · état actuel : ' + childStatus(child).label; }
      }
      rows.push(el('li', { class: 'lot-head sub-lot ' + cls, onclick: () => {
        if (done && !confirm(`${child.name} est déjà ${done.sealed ? 'constaté scellé' : 'validé'}. Le refaire ?`)) return;
        enterLevel(child.id);
        save();
        render();
      } },
      el('div', { class: 'main' },
        el('div', { class: 'name' }, (child.is_sealed ? '🔒 ' : '📦 ') + child.name),
        el('div', { class: 'sub' }, sub + (subs ? ` · ${subs} sous-lot(s)` : ''))),
      el('span', { class: 'chevron' }, '›')));
    }
    const { groups, others: loose, remaining } = level.storage || ownContent(level) ? expectedGroups(level, plan)
      : { groups: [], others: [], remaining: 0 };
    if (groups.length || loose.length) {
      if (childrenOf(level.id).length) rows.push(el('li', { class: 'cascade-own' }, `Contenu de ${level.name}`));
      rows.push(...lotGroupRows(level, groups, loose, lastScanRanks()));
    }
    if (!left && !remaining) {
      rows.splice(1, 0, el('li', { class: 'group ok' }, `✅ Tout est fait : validez ${level.name}`
        + (sealNextLevel() ? ', puis rescellez-le.' : parent ? ` pour revenir à ${parent.name}.` : ' pour terminer la vérif.')));
    }
    list.replaceChildren(...rows);
    return remaining + left;
  }

  // Scannes : les notres (retirables), et ceux des autres participants de la vérif commune, avec leur nom
  function renderDone() {
    const list = $('#done-list');
    const expected = knownIids();
    const all = allScanned();
    const rows = all.map((entry, index) => {
      const mine = index < state.scanned.length;
      const info = entry.info;
      let name = entry.kind === 'pack' && info ? `Paquet : ${info.count} × ${info.type_name}` : info ? info.type_name : entry.iid;
      let cls = 'ok';
      const tags = [];
      if (entry.error) { cls = 'error'; tags.push(el('span', { class: 'tag red' }, 'INCONNU')); }
      else if (entry.expired) { cls = 'expired'; tags.push(el('span', { class: 'tag red' }, badLabel(info))); }
      else if (state.lot && entry.kind === 'item' && !expected.has(entry.iid)) {
        cls = 'extra'; tags.push(el('span', { class: 'tag blue' }, 'nouveau'));
      }
      const peremption = info ? info.peremption : null;
      return el('li', { class: cls },
        el('div', { class: 'main' },
          el('div', { class: 'name' }, name),
          el('div', { class: 'sub' }, [peremption ? fmtDate(peremption) : info && entry.kind === 'item' ? 'Non périssable' : '', entry.iid,
            mine ? '' : `par ${entry.who || 'un autre participant'}`].filter(Boolean).join(' · '))),
        ...tags,
        entry.kind === 'item' && !entry.error ? reportMark({ item: entry.iid, label: `${name} (${entry.iid})`,
          lot: info && info.location, lotName: info && info.location_name })
          : entry.kind === 'pack' && info ? reportMark({ pack: info.id, label: `Paquet de ${info.type_name} (${info.id})` }) : null,
        mine && el('button', {
          class: 'remove', type: 'button', 'aria-label': 'Retirer',
          onclick: () => { scannedAgain(); state.scanned.splice(index, 1); save(); render(); },
        }, '✕'));
    }).reverse(); // dernier scan en haut
    list.replaceChildren(...(rows.length ? rows : [el('li', { class: 'empty' }, 'Aucun item scanné.')]));
    return all.length;
  }

  function renderLot() {
    const view = $('#lot-view');
    const lot = state.lot;
    if (!lot) {
      view.replaceChildren(el('p', {}, "Aucun lot sélectionné. Scannez l'étiquette publique ou clé d'un lot, ou saisissez son identifiant (menu ⋯)."));
      return;
    }
    const plan = planSession();
    const fresh = (plan.lots.find((row) => row.lot.id === lot.id) || { fresh: {} }).fresh;
    const lots = sessionLots();
    view.replaceChildren(
      el('h2', {}, lot.name),
      el('div', { class: 'sub' }, `${lot.lot_type_name} · ${lot.id}`),
      (lot.path || []).length ? el('div', { class: 'sub' }, 'Dans : ',
        ...lot.path.flatMap((parent, index) => [index ? ' › ' : '', lotLink(parent)])) : '',
      el('div', { class: 'banner ' + (groupStatus(lot) || lotStatus(lot)).kind }, (groupStatus(lot) || lotStatus(lot)).label),
      el('p', {}, ficheLink('lot', lot.id, 'Fiche du lot : contenu et emplacement des items')),
      reportButton({ lot: lot.id, lotName: lot.name, label: lot.name }),
      lot.is_sealed ? el('p', {}, `🔒 Scellé${lot.seal_number ? ' n°' + lot.seal_number : ''} ${fmtOn(lot.sealed)}` +
        (lot.sealed_by ? ` par ${lot.sealed_by}` : '') + ' : pas de vérif nécessaire tant que le scellé est intact.') : '',
      el('p', {}, 'Dernière vérif : ' + (lot.last_verif ? `${fmtDateTime(lot.last_verif)} par ${lot.last_verif_by || '?'}` : 'jamais'),
        el('br'), keyOk() ? '🔑 Étiquette clé scannée : la vérif peut être validée.' : '🔒 Scannez l\'étiquette clé pour valider. ' + keyHint(lot)),
      expiryLine(lot),
      revisionLine(lot),
      el('p', {}, sheetLink(lot.id, (lot.descendants || []).length ? 'Fiche du lot en PDF (une page par sous-lot)' : 'Fiche du lot en PDF')),
      lots.length > 1 ? el('p', {}, `Vérif groupée : ${lots.filter((sub) => (sub.requirements || []).length || (sub.items || []).length).map((sub) => sub.name).join(', ')}. `
        + "Scannez l'étiquette clé d'un autre lot du même lot global pour l'ajouter.") : '',
      ...renderGlobal(lot),
      el('h3', {}, 'Scannés / attendus'),
      ...(lot.requirements || []).map((row) => requirementRow(row.type_name + (row.location ? ` (${row.location})` : ''),
        fresh[row.type] || 0, row.required)),
      el('h3', {}, 'État enregistré du lot'),
      ...(lot.requirements || []).map((row) => requirementRow(row.type_name, row.present, row.required)),
    );
  }

  // Fiche d'inventaire papier (PDF A4) : liste de controle par emplacement, une page par sous-lot
  function sheetLink(id, label) {
    return el('a', { class: 'button-link', href: new URL(`lots/${encodeURIComponent(id)}/sheet.pdf`, API).href,
      target: '_blank', rel: 'noopener' }, '📄 ' + label);
  }

  // Lot global (depuis n'importe lequel de ses sous-lots) : etat de l'ensemble et de chaque lot
  function renderGlobal(lot) {
    const global = lot.global;
    if (!global) return [];
    const tags = { ok: 'green', warn: 'orange', bad: 'red' };
    return [
      el('h3', {}, `Lot global : ${global.name}`),
      el('div', { class: 'banner ' + global.kind }, global.label),
      global.id !== lot.id ? el('p', {}, sheetLink(global.id, 'Fiche du lot global en PDF (une page par sous-lot)')) : '',
      el('p', { class: 'hint' }, 'Vérif la plus ancienne : ' + (global.last_verif ? fmtDateTime(global.last_verif) : 'jamais')),
      el('ul', { class: 'list tree' }, global.lots.map((row) => el('li', {
        class: (row.counted ? row.effective.kind : 'group-lot') + (row.id === lot.id ? ' current' : ''),
        style: `padding-left:${12 + row.depth * 16}px;cursor:pointer`,
        onclick: () => openFiche('lot', row.id), // fiche du lot
      },
      el('div', { class: 'main' },
        el('div', { class: 'name' }, (row.depth ? '└ ' : '') + row.name + (row.id === lot.id ? ' (ce lot)' : '')),
        el('div', { class: 'sub' }, row.counted
          ? `${row.lot_type_name} · vérif : ${row.last_verif ? fmtDateTime(row.last_verif) : 'jamais'}`
          : `${row.lot_type_name} · regroupement`)),
      row.counted ? el('span', { class: 'tag ' + tags[row.effective.kind] }, row.effective.label.replace(/^[✔✘⚠] /, '').split(',')[0]) : null))),
    ];
  }

  // ------------------------------------------------------------------------------------------------
  // Page d'accueil : connexion (petit lecteur de badge dans un coin), Scanner, Suivi des vérifs, État des lots,
  // État des stocks. Chaque page a son ancre (#lots, #suivi, #stock, #scan) : le bouton retour du telephone
  // revient a l'accueil, et un rechargement rouvre la meme page.

  const badge = credentials;
  const HOME_PAGES = { menu: '', lots: 'lots', live: 'suivi', stock: 'stock', reports: 'signalements', lists: 'listes' };
  const HOME_TITLES = { menu: 'QRProtec', lots: 'État des lots', live: 'Suivi des vérifs', stock: 'État des stocks',
    reports: 'Signalements', lists: 'Listes de matériel' };
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
    if (page !== 'lists') stopListScan();
    state.homePage = page;
    if (!readerOpen()) { cameraWanted = false; syncCamera(); }
    document.body.classList.add('home-mode');
    if (page === 'lots' && !state.lots && state.loading !== 'lots') loadLots();
    if (page === 'stock' && canSeeStock() && !state.stock && state.loading !== 'stock') loadStock();
    if (page === 'reports' && state.user) loadReports();
    if (page === 'live') watchStart(); else watchStop();
    if (page === 'lists') listsShown(); else { clearInterval(lists.timer); lists.timer = 0; }
    renderHomePage();
    $('#home-pages').scrollTop = 0;
  }

  // Scanner : l'ecran de vérif habituel, camera demarree (le geste de l'utilisateur debloque aussi le son)
  function enterScanner(start = true, push = true) {
    if (push) history.pushState(null, '', new URL('./#scan', document.baseURI).href);
    watchStop();
    if (ficheOpen()) closeFiche();
    document.body.classList.remove('home-mode', 'login-reader');
    state.homePage = 'scan';
    if (start) startCamera();
    if (state.lotId && !state.lot) loadLot(state.lotId).then(() => { if (state.lot) showLotInfo(); });
    render();
  }

  function openReader(after = '') {
    pendingAfterLogin = after;
    document.body.classList.add('login-reader');
    $('#scan-hint').textContent = 'Scannez votre badge';
    startCamera();
    renderHomePage();
  }

  function closeReader() {
    pendingAfterLogin = '';
    loginAction = null;
    lists.mode = false;
    document.body.classList.remove('login-reader', 'list-scan');
    if (inHome() || ficheOpen()) { cameraWanted = false; syncCamera(); }
    renderHomePage();
  }
  $('#reader-close').addEventListener('click', (event) => { event.stopPropagation(); closeReader(); });
  $('#reader-name').addEventListener('click', (event) => { event.stopPropagation(); openNameLogin(); });

  // Badge scanne : le petit lecteur se ferme et la page demandee s'ouvre. La carte scannee hors de l'appli
  // (badge?m=...) ou dans le scanner vide (aucun lot, aucun scan) mene aussi a l'accueil : apres un scan
  // (item seul, hors vérif compris), on reste dans le scanner.
  function afterLogin() {
    const next = pendingAfterLogin;
    if (readerOpen()) closeReader();
    if (ficheOpen()) { // connexion depuis une fiche (signalement) : on y reste, avec les actions du role
      openFiche(fiche.kind, fiche.id, { key: fiche.key, seal: fiche.seal, push: false });
      return;
    }
    if (next === 'stock' && !canSeeStock()) toast("L'état des stocks est réservé aux rôles gestion et admin.", true);
    if (next) showHome(next === 'stock' && !canSeeStock() ? 'menu' : next);
    else if (!inHome() && !verifInProgress() && !state.lot && !state.scanned.length && !state.pendingOpening
      && !state.pinReset) showHome('menu');
    else if (inHome()) showHome(state.homePage || 'menu', false);
  }

  function logout() {
    state.user = null;
    toast('Déconnecté.');
    save();
    live.identity();
    state.reports = null;
    lists.index = null;
    lists.list = null;
    if (inHome() && ['live', 'stock', 'reports', 'lists'].includes(state.homePage)) showHome('menu', false);
    render();
  }

  $('#home-user').addEventListener('click', () => {
    if (state.user) {
      if (confirm(`Déconnecter ${state.user.prenom} ${state.user.nom} ?`)) logout();
    } else if (readerOpen()) closeReader();
    else openReader();
  });
  $('#home-settings').addEventListener('click', openNotifications);
  $('#home-back').addEventListener('click', () => history.back());
  $('#go-home').addEventListener('click', (event) => { event.stopPropagation(); showHome('menu'); });
  document.querySelectorAll('.tile').forEach((tile) => tile.addEventListener('click', () => {
    const page = tile.dataset.go;
    if (page === 'scan') { enterScanner(); return; }
    if (page === 'live' && !state.user) { openReader('live'); toast('Scannez votre badge pour suivre les vérifs.'); return; }
    if (page === 'stock' && !state.user) { openReader('stock'); toast('Scannez votre badge pour voir les stocks.'); return; }
    if (page === 'reports' && !state.user) { openReader('reports'); toast('Scannez votre badge pour suivre les signalements.'); return; }
    if (page === 'lists') { lists.id = ''; lists.list = null; }
    if (page === 'lists' && !state.user) { openReader('lists'); toast('Scannez votre badge pour voir les listes de matériel.'); return; }
    if (page === 'stock' && !canSeeStock()) { toast("L'état des stocks est réservé aux rôles gestion et admin.", true); return; }
    showHome(page);
  }));
  // bouton retour du telephone : l'adresse dit quoi afficher (fiche d'un lot ou d'un item, lecteur, page de l'accueil)
  window.addEventListener('popstate', () => {
    const route = location.pathname.replace(/\/+$/, '').split('/').pop();
    const params = new URLSearchParams(location.search);
    if (route === 'verif' && params.get('lot')) { openFiche('lot', params.get('lot'), { push: false }); return; }
    if (route === 'seal' && params.get('lot') && params.get('s')) {
      openFiche('lot', params.get('lot'), { seal: params.get('s'), push: false });
      return;
    }
    if ((route === 'item' || route === 'task') && IID_RE.test(params.get('id') || '')) { openFiche('item', params.get('id'), { push: false }); return; }
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

  async function loadLots() {
    state.loading = 'lots';
    render();
    try {
      // liste publique ; le badge, s'il y en a un, est verifie
      state.lots = await api('lots/summary/', state.user ? { user: badge() } : {});
    } catch (e) {
      toast('Lots : ' + e.message, true);
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
      const [stock, forecast] = await Promise.all([
        api('stock/summary/', { user: badge() }),
        api('stock/forecast/summary/', { user: badge(), months: 6 }).catch(() => null),
      ]);
      state.stock = stock;
      state.forecast = forecast;
    } catch (e) {
      toast('Stocks : ' + e.message, true);
      if (e.data && e.data.pin_required) { state.user = null; save(); } // session expiree : rescanner le badge
    } finally {
      state.loading = '';
      render();
    }
  }

  async function chooseLot(id, tab = 'todo') {
    if (!(state.lot && lotById(id))) {
      if (state.scanned.length && !confirm('Changer de lot ? Les items déjà scannés restent dans la liste.')) return;
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
    $('#home-settings').hidden = !hasNotifications();
    const userButton = $('#home-user');
    userButton.textContent = state.user ? `👤 ${state.user.prenom}` : readerOpen() ? 'Annuler' : 'Se connecter';
    userButton.classList.toggle('ok', !!state.user);
    const stockTile = $('#tile-stock');
    stockTile.classList.toggle('disabled', !!state.user && !canSeeStock());
    stockTile.setAttribute('aria-disabled', String(!!state.user && !canSeeStock()));
    $('#tile-stock-sub').textContent = !state.user ? 'Connexion requise' : canSeeStock() ? 'Stock et prévisions'
      : 'Réservé gestion et admin';
    const counts = state.reports && state.reports.counts;
    $('#tile-reports-sub').textContent = !state.user ? 'Connexion requise'
      : counts && isAdmin() && counts.new ? `${counts.new} nouveau(x) à traiter`
        : counts && counts.new + counts.progress ? `${counts.new + counts.progress} en cours` : 'En cours et résolus';
    $('#tile-reports').classList.toggle('alert', !!(counts && isAdmin() && counts.new));
    if (page === 'lots') renderLotsPage();
    if (page === 'live') renderLive();
    if (page === 'reports') renderReports();
    if (page === 'lists') renderListsPage();
  }

  function renderLotsPage() {
    const view = $('#home-lots');
    const parts = [el('div', { class: 'row-head' }, el('h3', {}, 'Lots'),
      el('button', { type: 'button', onclick: loadLots }, state.loading === 'lots' ? 'Chargement…' : 'Actualiser'))];
    if (!state.lots) {
      parts.push(el('p', { class: 'hint' }, state.loading === 'lots' ? 'Chargement des lots…' : 'Liste non chargée.'));
      view.replaceChildren(...parts);
      return;
    }
    parts.push(el('p', { class: 'hint' }, "Touchez un lot pour l'ouvrir. Un lot global affiche l'état de l'ensemble de ses sous-lots."));
    parts.push(el('label', { class: 'check' },
      el('input', { type: 'checkbox', checked: state.showStorage ? '' : null,
        onchange: (event) => { state.showStorage = event.target.checked; renderLotsPage(); } }),
      'Afficher les rangements du stock'));
    const tags = { ok: 'green', warn: 'orange', bad: 'red' };
    // rangements du stock masques par defaut (sauf le lot en cours)
    const lots = state.lots.filter((lot) => state.showStorage || !lot.storage || lot.id === state.lotId);
    parts.push(el('ul', { class: 'list' }, ...lots.map((lot) => {
      // lot global : son etat est celui de l'ensemble de ses lots
      const top = !lot.depth && lot.global;
      const status = top ? { kind: lot.global.kind, label: lot.global.label } : lotStatus(lot);
      const verif = top ? lot.global.last_verif : lot.last_verif;
      return el('li', { class: status.kind, style: lot.depth ? `padding-left:${12 + lot.depth * 16}px` : null,
        onclick: () => openLot(lot.id) },
        el('div', { class: 'main' },
          el('div', { class: 'name' }, (lot.depth ? '└ ' : '') + lot.name + (lot.id === state.lotId ? ' (en cours)' : '')),
          el('div', { class: 'sub' }, `${lot.lot_type_name}${top ? ' · lot global' : ''} · vérif${top ? ' la plus ancienne' : ''} : `
            + (verif ? fmtDateTime(verif) : 'jamais'))),
        el('span', { class: 'tag ' + tags[status.kind] }, status.label.replace(/^[✔✘⚠] /, '').split(',')[0]));
    })));
    view.replaceChildren(...parts);
  }

  // ------------------------------------------------------------------------------------------------
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
      : declared() ? { name: state.declaredName } : {});

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
          send({ type: 'done', complete: !!report.complete, partial: !!report.partial, present: (report.present || []).length });
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

  function teamChanged() {
    const me = live.me();
    if (me && !me.validated && state.waitingTeam) state.waitingTeam = false;
    // vérif en cascade rejointe : lots deja valides par les autres (un lot ou l'on est entre a pu etre rouvert)
    const done = (live.team && live.team.done) || {};
    if (inCascade()) {
      for (const [id, row] of Object.entries(done)) {
        if (!state.done[id] && !state.entered.includes(id) && lotById(id)) state.done[id] = row;
      }
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
    loadLot(state.lotId);
  }

  // Compte rendu de la vérif commune, envoyee par un autre participant
  function teamReport(report) {
    showReport(report);
    state.scanned = [];
    state.waitingTeam = false;
    live.done(null);
    forgetLot('Vérif enregistrée. Scannez un item ou l\'étiquette d\'un lot pour en commencer une autre.');
    loadLots();
  }

  // Rejoindre une vérif en cours (page de suivi) : ses scans et ses lots arrivent par la connexion
  function joinLiveVerif(row) {
    if (!state.user) { openReader('live'); toast('Scannez votre badge pour rejoindre une vérif.'); return; }
    const lotId = row.lot && row.lot.id;
    if (state.scanned.length && state.lotId !== lotId
      && !confirm('Rejoindre cette vérif ? Les items que vous avez déjà scannés sur un autre lot sont oubliés.')) return;
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
        (row.me ? 'vous' : row.who || 'Non identifié') + (row.validated ? ' ✔' : ''))),
        el('button', { type: 'button', class: 'text-link', onclick: () => {
          if (!confirm('Quitter la vérif commune ? Vous continuez seul, avec vos propres scans.')) return;
          state.waitingTeam = false;
          live.leave();
          render();
        } }, 'Quitter'));
    } else if (live.joining()) {
      parts.push(el('span', {}, '👥 Connexion à la vérif commune…'));
    } else if (team && team.nearby.length && state.lot) {
      const other = team.nearby[0];
      parts.push(el('span', {}, `👥 ${other.who || 'Quelqu\'un'} vérifie aussi ${other.lot ? other.lot.name : 'ce lot'}.`),
        el('button', { type: 'button', onclick: () => {
          if (!state.user) { toast('Scannez votre badge pour rejoindre sa vérif.', true); return; }
          if (!confirm(`Rejoindre la vérif de ${other.who || 'cette personne'} ? Vos scans s'ajoutent à la sienne.`)) return;
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
        "Scannez l'étiquette du sous-lot en cours, ou choisissez : ",
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

  // Etat publie : lot, lots de la vérif groupee, items scannes (nom, perime, inconnu, nouveau), reste a scanner.
  // entries (nos scans) et keys (cles des lots) ne vont qu'au serveur et aux autres participants de la vérif.
  function liveSnapshot(remaining) {
    // la vérif commence (et vit sur le serveur, partagee) des le premier scan, l'etiquette cle du lot, ou
    // « Lancer une vérif » depuis la fiche du lot
    const scanned = allScanned();
    if (!state.lot || !(scanned.length || verifInProgress() || (state.launched === state.lotId && !state.lastVerif)
      || cascadeActive())) return null;
    const expected = knownIids();
    return {
      lot: { id: state.lot.id, name: state.lot.name },
      root: rootOf(state.lot),
      // vérif en cascade : le lot global seulement, chacun publie le lot ou il se trouve (level)
      lots: inCascade() ? [] : sessionLots().filter((lot) => lot.id !== state.lot.id && ((lot.requirements || []).length || (lot.items || []).length))
        .map((lot) => lot.name),
      scanned: scanned.map((entry) => ({
        name: entry.kind === 'pack' && entry.info ? `Paquet : ${entry.info.count} × ${entry.info.type_name}`
          : (entry.info && entry.info.type_name) || entry.iid || 'code inconnu',
        expired: !!entry.expired,
        error: !!entry.error,
        extra: entry.kind === 'item' && !expected.has(entry.iid),
      })),
      count: scannedIids().size,
      remaining,
      entries: state.scanned.map(({ code, kind, iid, key, expired, error, items, info, place }) => ({
        code, kind, iid, key, expired, error, items, place,
        info: info ? { type: info.type, type_name: info.type_name, peremption: info.peremption, count: info.count } : null,
      })),
      keys: sessionEntries(),
      // vérif en cascade : lot ou l'on se trouve (les autres participants le voient « en cours »)
      cascade: inCascade(),
      level: inCascade() ? { id: levelLot().id, name: levelLot().name } : null,
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


  function liveCard(row) {
    const done = row.done;
    const kind = done ? (done.complete ? 'ok' : 'bad') : row.connected ? '' : 'lost';
    const lot = row.lot || { id: '', name: '?' };
    const status = done && done.lots != null
      ? `${done.complete ? '✔' : '✘'} Terminée à ${timeOf(done.at)} : ${done.lots} lot(s) validé(s)` + (done.incomplete ? `, dont ${done.incomplete} incomplet(s)` : '')
      : done
      ? `${done.complete ? '✔' : done.partial ? '↷' : '✘'} Validée à ${timeOf(done.at)} : ${done.complete ? 'complet' : done.partial ? 'vérif partielle' : 'incomplet'}, ${done.present} présent(s)`
      : row.connected ? (row.scanned.length ? `En cours · dernier scan ${ago(row.updated)}` : 'En cours · aucun item scanné')
        : `Connexion perdue ${ago(row.updated)}`;
    const open = watch.open.has(row.id);
    const shown = open ? [...row.scanned].reverse() : row.scanned.slice(-4).reverse();
    const people = row.participants || [{ who: row.who, verified: row.verified, connected: row.connected }];
    const mine = (live.credentials() || {}).id === row.id;
    return el('div', { class: 'live-card ' + kind },
      el('div', { class: 'name' }, '📦 ', lotLink(lot), row.lots.length ? ' + ' + row.lots.join(', ') : ''),
      el('div', { class: 'sub' }, (people.length > 1 ? '👥 ' : '') + people.map((person) => (person.who || 'Non identifié')
        + (person.who && !person.verified ? ' (nom déclaré)' : '') + (people.length > 1 && !done && person.validated ? ' ✔' : '')
        + (people.length > 1 && !done && !person.connected ? ' (déconnecté)' : '')
        + (!done && person.level && person.level.id !== lot.id ? ` 📍 ${person.level.name}` : '')).join(', ')
        + ` · commencée à ${timeOf(row.started)}`),
      el('div', { class: 'sub' }, status),
      done ? null : requirementRow('Scannés', row.count, row.count + row.remaining),
      row.scanned.length ? el('ul', {}, shown.map((item) => el('li', { class: item.error ? 'error' : item.expired ? 'expired' : '' },
        item.name, item.expired ? el('span', { class: 'tag red' }, 'PÉRIMÉ') : null,
        item.error ? el('span', { class: 'tag red' }, 'INCONNU') : null,
        item.extra ? el('span', { class: 'tag blue' }, 'nouveau') : null))) : null,
      row.scanned.length > 4 ? el('button', { type: 'button', class: 'text-link', onclick: () => {
        if (open) watch.open.delete(row.id); else watch.open.add(row.id);
        renderLive();
      } }, open ? 'Réduire' : `Voir les ${row.scanned.length} scans`) : null,
      // vérif à plusieurs : rejoindre la vérif, ses scans s'ajoutent a ceux de ce telephone
      !done && row.lot && !mine ? el('div', { class: 'join' },
        el('button', { type: 'button', onclick: () => joinLiveVerif(row) }, '👥 Rejoindre cette vérif')) : null,
      mine && !done ? el('div', { class: 'sub' }, 'Vous participez à cette vérif.') : null);
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
      el('button', { type: 'button', onclick: loadRecent }, watch.recentLoading ? 'Chargement…' : 'Actualiser')));
    if (!watch.recent) {
      parts.push(el('p', { class: 'hint' }, watch.recentLoading ? 'Chargement…' : 'Liste non chargée.'));
    } else if (!watch.recent.length) {
      parts.push(el('p', { class: 'hint' }, 'Aucune vérif enregistrée.'));
    } else {
      parts.push(el('ul', { class: 'list' }, ...watch.recent.map((row) => el('li', { class: row.complete ? 'ok' : 'bad' },
        el('div', { class: 'main' },
          el('div', { class: 'name' }, ...row.lots.flatMap((lot, index) => [index ? ', ' : '', lotLink(lot)])),
          el('div', { class: 'sub' }, `${cap(fmtDateTime(row.at))} · ${[row.by || '?', ...(row.participants || [])].join(', ')} · ${row.present} présent(s)`
            + (row.missing ? ` · ${row.missing} manquant(s)` : '') + (row.expired ? ` · ${row.expired} périmé(s)` : ''))),
        el('span', { class: 'tag ' + (row.complete ? 'green' : 'red') }, row.complete ? 'complet' : 'incomplet')))));
    }
    view.replaceChildren(...parts);
  }

  // ------------------------------------------------------------------------------------------------
  // Notifications web (admins) : alertes de stock bas et de stock vide, meme page fermee.
  // La permission du navigateur n'est demandee qu'au clic sur « Activer les notifications ».

  const pushSupported = () => window.isSecureContext && 'serviceWorker' in navigator && 'PushManager' in window && 'Notification' in window;

  function base64UrlToBytes(text) {
    const base64 = (text + '='.repeat((4 - text.length % 4) % 4)).replace(/-/g, '+').replace(/_/g, '/');
    return Uint8Array.from(atob(base64), (c) => c.charCodeAt(0));
  }

  async function currentPushSubscription() {
    // getRegistration n'installe rien : aucun service worker tant que l'admin n'a rien active
    const registration = await navigator.serviceWorker.getRegistration('web/');
    return registration ? registration.pushManager.getSubscription() : null;
  }

  // serviceWorker.ready ne se resout jamais ici : la portee web/ ne couvre pas la page
  function activated(registration) {
    const worker = registration.installing || registration.waiting;
    if (registration.active || !worker) return Promise.resolve();
    return new Promise((resolve) => {
      const check = () => { if (worker.state === 'activated' || worker.state === 'redundant') resolve(); };
      worker.addEventListener('statechange', check);
      check();
    });
  }

  // Alertes au choix : la liste vient du serveur (types permis par le role, moins ceux qu'un admin a coupes)
  const pushTypes = () => (state.push && state.push.types) || (state.pushDevices && state.pushDevices.types) || [];

  // keepError : garde le message d'une activation ou desactivation qui vient d'echouer
  async function loadPush(keepError = false) {
    if (!canSeeStock()) return;
    try {
      const subscription = pushSupported() ? await currentPushSubscription() : null;
      const endpoint = subscription ? subscription.endpoint : '';
      state.pushEndpoint = endpoint;
      const [current, devices] = await Promise.all([
        api('push/subscription/', { user: badge(), endpoint }),
        api('push/devices/', { user: badge(), endpoint }),
      ]);
      // choix en cours avant activation : gardes au rechargement
      state.push = state.push && !current.subscribed ? { ...current, ...pick(state.push, current.types) } : current;
      state.pushDevices = devices;
      // abonnement du navigateur inconnu du serveur (autre utilisateur, base restauree) : a reactiver
      if (subscription && !current.subscribed) state.push.stale = true;
      if (!keepError) state.pushError = '';
    } catch (e) {
      state.pushError = e.message;
    }
    renderNotifications();
  }

  function pick(values, types) {
    return Object.fromEntries(types.map(({ type }) => [type, !!values[type]]));
  }

  const BLOCKED_HELP = 'Autorisez-les dans les réglages du site (icône à gauche de l\'adresse > Notifications > '
    + 'Autoriser, ou « Réinitialiser l\'autorisation »), puis rechargez la page. Sur Android, les notifications de '
    + 'l\'application du navigateur doivent aussi être autorisées dans les réglages du téléphone.';

  // Demande d'autorisation au navigateur. Elle doit partir pendant le clic, avant toute attente : sinon Firefox et
  // Safari refusent sans afficher la fenetre. Gere aussi l'ancienne API a callback (Safari < 15).
  function askPermission() {
    return new Promise((resolve) => {
      const done = (permission) => resolve(permission || Notification.permission);
      try {
        const result = Notification.requestPermission(done);
        if (result && typeof result.then === 'function') result.then(done, () => done());
      } catch (e) {
        done();
      }
    });
  }

  // Etat de l'autorisation suivi en direct : la boite se met a jour si elle change dans les reglages du site
  if (navigator.permissions && navigator.permissions.query) {
    navigator.permissions.query({ name: 'notifications' })
      .then((status) => { status.onchange = () => renderNotifications(); })
      .catch(() => {});
  }

  function enablePush() {
    const prefs = pick(state.push || {}, pushTypes());
    if (!Object.values(prefs).some(Boolean)) { toast('Choisissez au moins une alerte.', true); return; }
    // premier appel du clic : la fenetre d'autorisation du navigateur s'affiche (geste de l'utilisateur)
    const asked = Date.now();
    const permission = askPermission();
    subscribePush(prefs, permission, asked);
  }

  async function subscribePush(prefs, permissionAsked, asked) {
    state.pushBusy = true; state.pushError = ''; renderNotifications();
    try {
      const permission = await permissionAsked;
      if (permission !== 'granted') {
        // refus immediat : le navigateur n'a pas affiche de fenetre (site bloque, ou notifications du navigateur
        // coupees par le systeme)
        const silent = Date.now() - asked < 500;
        throw new Error(permission === 'denied'
          ? (silent ? 'Le navigateur a refusé sans afficher de demande : notifications bloquées pour ce site. '
            : 'Notifications refusées pour ce site. ') + BLOCKED_HELP
          : 'Autorisation non accordée : réessayez et choisissez « Autoriser ». Si aucune fenêtre ne s\'affiche, '
            + 'cherchez une cloche dans la barre d\'adresse.');
      }
      const { public_key: publicKey } = await api('push/key/');
      const registration = await navigator.serviceWorker.register('web/sw.js', { scope: 'web/' });
      await activated(registration);
      let subscription = await registration.pushManager.getSubscription();
      if (subscription && subscription.options && subscription.options.applicationServerKey) {
        // cle du serveur changee (base reinitialisee) : il faut un nouvel abonnement
        const current = new Uint8Array(subscription.options.applicationServerKey);
        const expected = base64UrlToBytes(publicKey);
        if (current.length !== expected.length || current.some((b, i) => b !== expected[i])) {
          await subscription.unsubscribe();
          subscription = null;
        }
      }
      if (!subscription) {
        subscription = await registration.pushManager.subscribe({ userVisibleOnly: true, applicationServerKey: base64UrlToBytes(publicKey) });
      }
      state.push = await api('push/subscription/', { user: badge(), subscription: subscription.toJSON(), ...prefs });
      toast('Notifications activées sur cet appareil.');
    } catch (e) {
      state.pushError = e.message;
    }
    state.pushBusy = false;
    loadPush(true);
  }

  async function disablePush() {
    state.pushBusy = true; state.pushError = ''; renderNotifications();
    try {
      const subscription = await currentPushSubscription();
      if (subscription) {
        await api('push/unsubscribe/', { endpoint: subscription.endpoint });
        await subscription.unsubscribe();
      }
      state.push = null;
      toast('Notifications désactivées sur cet appareil.');
    } catch (e) {
      state.pushError = e.message;
    }
    state.pushBusy = false;
    loadPush(true);
  }

  async function testPush() {
    try {
      const subscription = await currentPushSubscription();
      if (!subscription) throw new Error('Notifications non activées sur cet appareil.');
      await api('push/test/', { user: badge(), endpoint: subscription.endpoint });
      toast('Notification de test envoyée.');
    } catch (e) {
      toast(e.message, true);
    }
  }

  // alertes ou desabonnement d'un appareil de la liste (ce telephone ou un autre)
  async function updateDevice(device, changes) {
    if (changes.delete && device.current) { disablePush(); return; }
    if (changes.delete && !confirm(`Ne plus envoyer de notifications à « ${device.device} » ?`)) return;
    state.pushBusy = true; renderNotifications();
    try {
      state.pushDevices = await api(`push/devices/${device.id}/`, { user: badge(), endpoint: currentEndpoint(), ...changes });
      if (device.current) state.push = { ...state.push, ...changes };
      state.pushError = '';
    } catch (e) {
      state.pushError = e.message;
    }
    state.pushBusy = false;
    renderNotifications();
  }

  const currentEndpoint = () => state.pushEndpoint || '';

  function openNotifications() {
    renderNotifications();
    $('#notifications').showModal();
    loadPush();
  }

  // clic sur une notification alors que la page est deja ouverte
  if ('serviceWorker' in navigator) {
    navigator.serviceWorker.addEventListener('message', (event) => {
      if (event.data && event.data.tab === 'pinreset' && event.data.url) {
        const params = new URL(event.data.url).searchParams;
        if (params.get('m') && params.get('t')) startPinReset(params.get('m'), params.get('t'));
        return;
      }
      if (event.data && event.data.tab === 'stock' && canSeeStock()) { showHome('stock'); loadStock(); }
      if (event.data && event.data.tab === 'reports' && state.user) showHome('reports');
      if (event.data && event.data.tab === 'lot' && event.data.url) {
        const id = new URL(event.data.url).searchParams.get('lot');
        if (id) openFiche('lot', id); // scelle ouvert : fiche du lot
      }
    });
    if (navigator.serviceWorker.startMessages) navigator.serviceWorker.startMessages();
  }

  function renderNotifications() {
    const body = $('#notifications-body');
    if (!body) return;
    const busy = state.pushBusy ? '' : null;
    const parts = [];
    const checkbox = (checked, label, onchange) => el('label', { class: 'check' },
      el('input', { type: 'checkbox', checked: checked ? '' : null, disabled: busy, onchange }), label);

    // cet appareil
    const here = [el('h3', {}, 'Cet appareil')];
    const push = state.push;
    const subscribed = !!(push && push.subscribed && !push.stale);
    if (!pushSupported()) {
      here.push(el('p', { class: 'hint' }, window.isSecureContext
        ? "Ce navigateur ne gère pas les notifications web. Sur iPhone, ajoutez d'abord la page à l'écran d'accueil (Partager > Sur l'écran d'accueil)."
        : 'Les notifications web exigent une connexion HTTPS.'));
    } else if (!push) {
      here.push(el('p', { class: 'hint' }, 'Chargement…'));
    } else if (subscribed) {
      here.push(el('p', { class: 'hint' }, 'Activées : vous serez prévenu même page fermée. Choisissez les alertes dans la liste ci-dessous.'));
      here.push(el('div', { class: 'push-buttons' },
        el('button', { type: 'button', onclick: testPush, disabled: busy }, 'Envoyer un test'),
        el('button', { type: 'button', onclick: disablePush, disabled: busy }, 'Désactiver')));
    } else if (!push.types.length) {
      here.push(el('p', { class: 'hint' }, "Un administrateur a désactivé toutes vos notifications."));
    } else {
      here.push(el('p', { class: 'hint' }, 'Recevez sur cet appareil les alertes choisies :'));
      for (const { type, label } of push.types) {
        here.push(checkbox(push[type], label, (event) => { push[type] = event.target.checked; }));
      }
      if (Notification.permission === 'denied' && !state.pushError) {
        here.push(el('p', { class: 'hint' }, 'Le navigateur indique que les notifications sont bloquées pour ce site : '
          + '« Activer » redemande l\'autorisation. Si aucune fenêtre ne s\'affiche : ' + BLOCKED_HELP.charAt(0).toLowerCase()
          + BLOCKED_HELP.slice(1)));
      }
      here.push(el('div', { class: 'push-buttons' },
        el('button', { type: 'button', class: 'primary', onclick: enablePush, disabled: busy },
          state.pushBusy ? 'Activation…' : 'Activer les notifications')));
    }
    parts.push(el('div', { class: 'push-box' }, ...here));

    // tous les appareils abonnes de l'utilisateur
    const list = state.pushDevices;
    parts.push(el('h3', {}, 'Vos appareils'));
    if (!list) {
      parts.push(el('p', { class: 'hint' }, 'Chargement…'));
    } else if (!list.devices.length) {
      parts.push(el('p', { class: 'hint' }, 'Aucun appareil ne reçoit vos notifications.'));
    } else {
      for (const device of list.devices) {
        const sent = device.last_sent ? `dernier envoi ${fmtOn(device.last_sent)} (${device.last_status})` : 'aucun envoi';
        parts.push(el('div', { class: 'device' },
          el('div', { class: 'row-head' },
            el('b', {}, device.device + (device.current ? ' · cet appareil' : '')),
            el('button', { type: 'button', disabled: busy, onclick: () => updateDevice(device, { delete: true }) }, 'Retirer')),
          el('p', { class: 'hint' }, `Abonné le ${fmtDate(device.created)}, ${sent}.`),
          ...list.types.map(({ type, label }) => checkbox(device[type], label,
            (event) => updateDevice(device, { [type]: event.target.checked })))));
      }
      if (!list.types.length) parts.push(el('p', { class: 'hint' }, 'Un administrateur a désactivé toutes vos notifications.'));
    }
    if (state.pushError) parts.push(el('p', { class: 'error' }, state.pushError));
    body.replaceChildren(...parts);
  }

  const HORIZONS = [{ label: 'Auj.', months: 0 }, { label: 'M+1', months: 1 }, { label: 'M+3', months: 3 },
    { label: 'M+6', months: 6 }];
  const MONTHS = ['janv.', 'févr.', 'mars', 'avr.', 'mai', 'juin', 'juil.', 'août', 'sept.', 'oct.', 'nov.', 'déc.'];

  // Graphe des peremptions des prochains mois (tous types) : barres empilees stock / lots / scelles, perdus en rouge
  function expiryChart(types) {
    const months = (types[0] && types[0].calendar) || [];
    const sums = months.map((month, index) => {
      const sum = { start: month.start, stock: 0, lots: 0, sealed: 0, lost: 0 };
      for (const type of types) {
        const row = type.calendar[index];
        sum.stock += row.expiring.stock; sum.lots += row.expiring.lots; sum.sealed += row.expiring.sealed;
        sum.lost += row.lost.stock + row.lost.lots + row.lost.sealed;
      }
      return sum;
    });
    const top = Math.max(1, ...sums.map((s) => s.stock + s.lots + s.sealed));
    const W = 300, H = 120, left = 24, bottom = 16, plot = H - bottom - 6, step = (W - left) / Math.max(1, sums.length);
    const svgEl = (tag, attrs, text) => {
      const node = document.createElementNS('http://www.w3.org/2000/svg', tag);
      for (const [key, value] of Object.entries(attrs)) node.setAttribute(key, value);
      if (text != null) node.textContent = text;
      return node;
    };
    const svg = svgEl('svg', { viewBox: `0 0 ${W} ${H}`, class: 'chart', role: 'img',
      'aria-label': 'Péremptions des prochains mois' });
    const y = (v) => 6 + plot - (v / top) * plot;
    for (const v of [0, Math.round(top / 2), top]) {
      svg.append(svgEl('line', { x1: left, x2: W, y1: y(v), y2: y(v), class: 'grid' }));
      svg.append(svgEl('text', { x: left - 4, y: y(v) + 3, 'text-anchor': 'end' }, v));
    }
    sums.forEach((sum, index) => {
      const x = left + index * step + step * 0.2, w = step * 0.6;
      let base = 0;
      for (const [key, cls] of [['stock', 's-stock'], ['lots', 's-lots'], ['sealed', 's-sealed']]) {
        if (!sum[key]) continue;
        const height = y(base) - y(base + sum[key]);
        svg.append(svgEl('rect', { x, y: y(base + sum[key]), width: w, height, class: cls }));
        base += sum[key];
      }
      if (sum.lost) {
        svg.append(svgEl('rect', { x: x + w * 0.3, y: y(sum.lost), width: w * 0.4, height: y(0) - y(sum.lost),
          class: 's-lost' }));
      }
      const date = parseDate(sum.start);
      const label = date ? MONTHS[date.getMonth()] : '';
      svg.append(svgEl('text', { x: x + w / 2, y: H - 3, 'text-anchor': 'middle' }, label));
    });
    return el('figure', { class: 'chart-box' }, svg,
      el('figcaption', { class: 'legend' },
        ...[['s-stock', 'stock'], ['s-lots', 'lots'], ['s-sealed', 'scellés'], ['s-lost', 'perdus']]
          .map(([cls, label]) => el('span', {}, el('i', { class: cls }), label))));
  }

  // Previsions de stock : horizon, graphe, types a surveiller, echanges
  function renderForecast(parts) {
    const forecast = state.forecast;
    if (!forecast) {
      parts.push(el('p', { class: 'hint' }, state.loading === 'stock' ? 'Chargement…' : 'Prévisions indisponibles.'));
      return;
    }
    const horizon = HORIZONS[state.horizon];
    parts.push(el('div', { class: 'segmented', role: 'group', 'aria-label': 'Horizon' },
      ...HORIZONS.map((h, index) => el('button', { type: 'button', class: index === state.horizon ? 'active' : '',
        onclick: () => { state.horizon = index; renderStock(); } }, h.label))));
    const types = forecast.types || [];
    const at = (type) => type.points[Math.min(horizon.months, type.points.length - 1)];
    if (types.length) {
      parts.push(el('p', { class: 'hint' }, `Stock valide au ${fmtDate(at(types[0]).date)}, en utilisant d'abord les dates `
        + `les plus proches (consommation mesurée sur ${forecast.history_days} jours).`));
      parts.push(expiryChart(types));
    }
    const sorted = [...types].sort((a, b) => {
      if (!!a.below_min !== !!b.below_min) return a.below_min ? -1 : 1;
      if (a.below_min) return a.below_min < b.below_min ? -1 : 1;
      const ratio = (t) => t.min_quantity > 0 ? at(t).stock / t.min_quantity : 1e6;
      return ratio(a) - ratio(b);
    });
    for (const type of sorted) {
      const point = at(type);
      const details = [];
      if (point.expiring) details.push(`${point.expiring} périme(nt)` + (point.lost ? ` dont ${point.lost} perdu(s)` : ''));
      if (type.order) details.push(`commander ${type.order.quantity} avant le ${fmtDate(type.order.before)}`);
      else if (!details.length) details.push(type.per_month ? `${Math.round(type.per_month * 10) / 10} utilisé(s) par mois` : 'rien à signaler');
      const kind = type.order && type.order.urgent ? 'bad' : type.below_min ? 'warn' : '';
      parts.push(el('div', { class: 'stock-row ' + kind },
        requirementRow(type.name, point.stock, type.min_quantity),
        el('div', { class: 'sub hint' }, details.join(' · '))));
    }
    const transfers = forecast.transfers || [];
    if (transfers.length) {
      parts.push(el('h3', {}, 'Échanges à faire'));
      parts.push(el('p', { class: 'hint' }, 'Items qui périmeront là où ils sont : à échanger contre des plus récents du lieu indiqué.'));
      for (const group of transfers) {
        const when = (group.source.sealed ? 'scellé, ouvrir avant le ' : 'avant le ') + fmtDate(group.before);
        parts.push(el('div', { class: 'stock-row ' + (group.source.sealed ? 'bad' : 'warn') },
          el('div', { class: 'row-head' }, el('b', {}, group.source.path), el('span', { class: 'hint' }, when)),
          ...group.moves.map((move) => el('div', { class: 'sub hint' },
            `${move.type_name} ×${move.take.length} (${fmtDate(move.take_peremption)}) → ${move.target.path}, `
            + `reprendre ×${move.back.length} (${fmtDate(move.back_peremption)})`))));
      }
    }
  }

  // Etat des stocks : page « État des stocks » de l'accueil (forecast : previsions)
  function stockParts(forecastMode) {
    if (!canSeeStock()) return [el('p', {}, 'Réservé aux rôles gestion et admin : scannez votre badge.')];
    const toggle = el('div', { class: 'segmented', role: 'group', 'aria-label': 'Vue' },
      el('button', { type: 'button', class: forecastMode ? '' : 'active', onclick: () => { state.homeForecast = false; renderStock(); } }, 'État actuel'),
      el('button', { type: 'button', class: forecastMode ? 'active' : '', onclick: () => { state.homeForecast = true; renderStock(); } }, 'Prévisions'));
    const head = el('div', { class: 'row-head' },
      el('h3', {}, forecastMode ? 'Stock à venir' : 'État des stocks (lecture seule)'),
      el('div', { class: 'buttons' },
        el('button', { type: 'button', onclick: loadStock }, state.loading === 'stock' ? 'Chargement…' : 'Actualiser')));
    const parts = [toggle, head];
    if (forecastMode) {
      renderForecast(parts);
      return parts;
    }
    if (hasNotifications()) {
      parts.push(el('p', { class: 'hint' }, 'Alertes de stock sur votre téléphone : ',
        el('button', { type: 'button', class: 'text-link', onclick: openNotifications }, 'Notifications')));
    }
    if (!state.stock) {
      parts.push(el('p', { class: 'hint' }, 'Chargement…'));
    } else {
      parts.push(el('p', { class: 'hint' }, 'Stock non périmé (hors lots, rangements compris) / minimum. Les plus critiques en premier.'));
      const ratio = (row) => row.min_quantity > 0 ? row.stock_fresh / row.min_quantity : 1e6;
      const rows = [...state.stock].sort((a, b) => ratio(a) - ratio(b));
      for (const row of rows) {
        const details = [`${row.lots_fresh} dans les lots`];
        if (row.stock_expired + row.lots_expired) details.push(`${row.stock_expired + row.lots_expired} périmé(s)`);
        if (row.expiring_soon) details.push(`${row.expiring_soon} bientôt périmé(s)`);
        if (row.missing) details.push(`${row.missing} disparu(s)`);
        if (row.out) details.push(`${row.out} sorti(s) sans lot`);
        if (row.loose) details.push(`${row.loose} hors lot`);
        parts.push(el('div', { class: 'stock-row' },
          requirementRow(row.name, row.stock_fresh, row.min_quantity),
          el('div', { class: 'sub hint' }, details.join(' · '))));
      }
    }
    return parts;
  }

  function renderStock() {
    $('#home-stock').replaceChildren(...stockParts(!!state.homeForecast));
  }

  // Items scannes sans etiquette de lot : le bouton de vérif devient « Sortir du stock »
  function takingOut() {
    return !state.lot && scannedIids().size > 0;
  }

  function render() {
    const remaining = renderTodo();
    $('#count-todo').textContent = remaining;
    $('#count-done').textContent = renderDone();
    renderLot();
    if (!state.user) { state.stock = null; state.push = null; state.pushDevices = null; }
    renderHomePage();
    $('#menu-notifications').hidden = !hasNotifications();
    renderStock();
    const chip = $('#user-chip');
    chip.textContent = state.user ? `👤 ${state.user.prenom} ${state.user.nom}`
      : declared() ? `👤 ${state.declaredName} (sans badge)` : '👤 Non connecté';
    chip.classList.toggle('ok', !!state.user);
    // reassort, ou verif partielle (verif groupee dont certains lots sont complets) : bouton orange a cote de la validation
    const partial = state.busy ? [] : partialLots();
    const restock = !state.busy && (partial.length > 0 || onlyNewItems());
    $('#restock').hidden = !restock;
    $('#undo-last').hidden = !state.scanned.length || state.busy;
    $('#restock').textContent = partial.length
      ? `Vérif partielle (${partial.map((lot) => lot.name_short || lot.name).join(', ')})`
      : `Ajouter ${scannedIids().size} au lot (réassort)`;
    const validateButton = $('#validate');
    const recorded = state.lastVerif && state.lot && state.lastVerif.lotId === state.lot.id && !allScanned().length;
    const out = takingOut();
    const waiting = state.waitingTeam && !state.busy;
    // vérif à plusieurs : d'autres participants n'ont pas encore valide, on ne valide que sa partie
    const shared = !out && !recorded && live.others().some((row) => row.connected && !row.validated);
    validateButton.disabled = state.busy || recorded || waiting;
    validateButton.classList.toggle('primary', !out);
    validateButton.classList.toggle('warning', out);
    const missing = blockers(false);
    validateButton.title = state.busy ? 'Envoi de la vérif en cours.' : recorded ? 'Cette vérif est déjà enregistrée. Scannez un item pour en commencer une nouvelle.'
      : out ? 'Ces items ne sont dans aucun lot : ils sont enregistrés comme sortis du stock.'
        : missing.length ? 'Pour valider : ' + missing.join(', ') + '.'
          : 'Enregistre la vérif : les items scannés sont présents, ceux de « À scanner » sont signalés manquants.';
    $('#validate-hint').hidden = !shared && !waiting;
    $('#validate-hint').textContent = waiting ? 'En attente des autres participants' : 'Valider ma partie de la vérif';
    if (waiting) validateButton.title = 'Votre partie est validée. Scannez un item pour la reprendre.';
    validateButton.textContent = waiting ? 'Partie validée ✔' : state.busy ? 'Envoi…' : recorded ? 'Vérif enregistrée ✔'
      : out ? `Sortir ${scannedIids().size} item(s) du stock` : restock ? 'Vérif complète…'
        : missing.length ? 'Valider la vérif…' : 'Valider la vérif';
    if (inCascade() && !out) renderCascadeActions(validateButton);
    live.publish(liveSnapshot(remaining));
    renderTeam();
    $('#scan-hint').textContent = readerOpen() ? 'Scannez votre badge' : out ? "Scannez l'étiquette d'un lot pour une vérif, ou sortez ces items du stock"
      : !state.lot ? "Visez l'étiquette d'un lot ou un item"
      : !hasIdentity() ? (state.declaredAllowed ? 'Scannez votre badge (ou touchez « Non connecté ») pour pouvoir valider'
        : 'Scannez votre badge pour pouvoir valider')
        : 'Scannez les items du lot';
  }

  // Boutons de la vérif en cascade : valider le lot en cours (et revenir), terminer au lot global ; vérif partielle ou
  // reassort du lot en cours
  function renderCascadeActions(validateButton) {
    const level = levelLot();
    const atRoot = level.id === state.lotId;
    const exclude = excludedFor(level.id);
    const scope = scopeOf(level.id, exclude);
    const entries = state.busy ? [] : scopeEntries(scope);
    const something = ownContent(level) || level.storage || entries.length > 0 || [...scope].some((id) => id !== level.id);
    const name = level.name.length > 22 ? level.name_short || level.name : level.name;
    validateButton.disabled = state.busy;
    validateButton.textContent = state.busy ? 'Envoi…' : !something ? (atRoot ? 'Terminer la vérif' : `Revenir (rien à valider)`)
      : atRoot ? `Valider ${name} et terminer` : `Valider ${name}`;
    validateButton.title = atRoot ? 'Enregistre la vérif du lot global et de ses sous-lots commencés ; les sous-lots non faits restent tels quels.'
      : `Enregistre la vérif de ${level.name} (et de ses sous-lots commencés), puis revient au lot qui le contient.`;
    $('#validate-hint').hidden = true;
    // vérif partielle du lot en cours (des lots sont complets, pas tous) ou reassort (que des items nouveaux)
    const plan = state.busy ? null : planSession();
    const rows = plan ? plan.lots.filter((row) => scope.has(row.lot.id)) : [];
    const complete = rows.filter((row) => row.complete && row.touched > 0);
    const partial = complete.length > 0 && rows.some((row) => !row.complete) ? complete.map((row) => row.lot) : [];
    const known = new Set(sessionLots().filter((lot) => scope.has(lot.id))
      .flatMap((lot) => [...(lot.items || []), ...(lot.missing_items || [])].map((item) => item.iid)));
    const iids = entriesPayload(entries).items;
    const restock = iids.length > 0 && !iids.some((iid) => known.has(iid));
    $('#restock').hidden = !(partial.length || restock);
    $('#restock').textContent = partial.length ? `Vérif partielle (${partial.map((lot) => lot.name_short || lot.name).join(', ')})`
      : `Ajouter ${iids.length} à ${name} (réassort)`;
    if (partial.length || restock) validateButton.textContent = `Vérif complète de ${name}…`;
  }

  // ------------------------------------------------------------------------------------------------
  // Fiches lot et item : page d'arrivee d'une etiquette ouverte avec l'appareil photo du telephone (sans le lecteur
  // integre), ou d'un lot / item touche dans une fiche. Pas de camera : « Lancer une vérif » ouvre le lecteur sur le
  // bon lot (le lot de la fiche, ou le dernier emplacement connu de l'item).

  const fiche = { kind: '', id: '', key: '', seal: '', lot: null, item: null, error: '', keyCheck: '', keyError: '', token: 0, folds: new Set() };

  function ficheOpen() { return !$('#fiche').hidden; }

  // Premiere lettre en majuscule (texte saisi au poste, ex : emplacement de l'etiquette privee)
  const cap = (text) => (text ? text.charAt(0).toUpperCase() + text.slice(1) : '');

  const ficheUrl = (kind, id, seal = '') => (kind !== 'lot' ? `item?id=${encodeURIComponent(id)}`
    : seal ? `seal?lot=${encodeURIComponent(id)}&s=${encodeURIComponent(seal)}` : `verif?lot=${encodeURIComponent(id)}`);

  // Lien vers la fiche d'un lot ou d'un item (vraie URL : appui long pour l'ouvrir dans un autre onglet)
  function ficheLink(kind, id, label) {
    return el('a', { class: 'link', href: ficheUrl(kind, id), onclick: (event) => {
      event.preventDefault();
      event.stopPropagation();
      openFiche(kind, id);
    } }, label);
  }
  const lotLink = (lot) => ficheLink('lot', lot.id, lot.name);

  // Cle de l'etiquette privee ouverte : retiree de l'adresse, gardee pour cet onglet (rechargement, retour)
  function storedFicheKey(id) {
    try {
      const saved = JSON.parse(sessionStorage.getItem(STORAGE_FICHE_KEY) || 'null');
      return saved && saved.id === id && Date.now() - saved.at < SESSION_TIMEOUT_MS ? saved.key : '';
    } catch (e) { return ''; }
  }

  function storeFicheKey(id, key) {
    try { sessionStorage.setItem(STORAGE_FICHE_KEY, JSON.stringify({ id, key, at: Date.now() })); } catch (e) { /* ignore */ }
  }

  // push : nouvelle entree d'historique (le bouton retour du telephone revient a la page precedente)
  // seal : code de l'etiquette du scelle scannee (fiche du lot scelle, sans vérif tant que le scelle est intact)
  async function openFiche(kind, id, { key = '', seal = '', push = true } = {}) {
    if (kind === 'lot' && !seal) {
      if (key) storeFicheKey(id, key); else key = storedFicheKey(id);
    }
    const token = fiche.token + 1;
    Object.assign(fiche, { kind, id, key, seal, lot: null, item: null, error: '', keyCheck: '', keyError: '', token, folds: new Set() });
    if (push) history.pushState({ inApp: true }, '', ficheUrl(kind, id, seal));
    $('#fiche').hidden = false;
    document.body.classList.add('fiche-open');
    document.body.classList.remove('home-mode', 'login-reader');
    state.homePage = 'scan';
    watchStop();
    syncCamera();
    renderFiche();
    $('#fiche-body').scrollTop = 0;
    try {
      if (kind === 'lot') {
        const query = seal ? `?seal=${encodeURIComponent(seal)}` : key ? `?key=${encodeURIComponent(key)}` : '';
        const lot = await api(`lots/${encodeURIComponent(id)}/` + query);
        if (fiche.token !== token) return;
        fiche.lot = lot;
        if (key) { fiche.keyCheck = lot.key_check; fiche.keyError = lot.key_error || ''; }
        if (seal && push) {
          if (lot.seal_check === 'valid' && !lot.expired_count) feedback.good();
          else if (lot.seal_check === 'wrong') feedback.warn(); else feedback.bad();
        }
      } else {
        const item = await api(`items/${encodeURIComponent(id)}/`);
        if (fiche.token !== token) return;
        fiche.item = item;
        if (item.reported) alertReports('item:' + item.iid, `Item signalé : ${item.type_name}`, item.reports);
      }
    } catch (e) {
      if (fiche.token !== token) return;
      fiche.error = e.status === 404 ? (kind === 'lot' ? 'Lot introuvable' : 'Item inconnu dans la base') : e.message;
    }
    renderFiche();
  }

  function closeFiche() {
    if (!ficheOpen()) return;
    fiche.token += 1;
    scannedFiche = null;
    $('#fiche').hidden = true;
    document.body.classList.remove('fiche-open', 'login-reader');
    syncCamera();
    render();
  }

  // Une vérif est en cours sur ce telephone (items scannes, ou etiquette privee scannee et pas encore validee)
  const verifInProgress = () => !!state.lot && (state.scanned.length > 0 || (!!state.lotKey && !state.lastVerif));

  // Ouvre le lecteur sur un lot. key : etiquette privee deja scannee (verifiee par le serveur).
  async function launchVerif(id, key) {
    startCamera(); // pendant le clic : le navigateur peut exiger un geste de l'utilisateur
    closeFiche();
    history.pushState({ inApp: true }, '', 'verif');
    // etiquette privee d'un autre lot du meme lot global pendant une vérif : elle rejoint la vérif en cours (vérif en
    // cascade : on entre dans ce lot)
    if (state.lot && state.lotId !== id && (key || inCascade()) && await joinVerif({ kind: 'lot', code: '', id, key })) return;
    const same = !!state.lot && !!lotById(id);
    if (!same) {
      if (state.scanned.length && !confirm('Changer de lot ? Les items déjà scannés restent dans la liste.')) return;
      state.lotKey = '';
      state.lastVerif = null;
      state.extra = [];
      resetCascade();
    }
    await loadLot(id);
    if (!state.lot) return;
    if (key) setKey(id, key);
    state.launched = state.lotId;
    if (inCascade()) enterLevel(id);
    feedback.info();
    showLotInfo();
    switchTab('todo');
  }

  async function launchFromFiche() {
    if (fiche.kind === 'item') {
      const where = fiche.item && fiche.item.last_location;
      const scanned = scannedFiche && scannedFiche.id === fiche.id ? scannedFiche : null;
      if (!where) return;
      await launchVerif(where.id, '');
      // fiche ouverte par un scan dans le scanner : l'item scanne compte dans la vérif, sans le rescanner
      if (scanned && state.lot && lotById(where.id)) await scanItem(scanned);
    } else if (fiche.lot) {
      launchVerif(fiche.id, fiche.keyCheck === 'valid' ? fiche.key : '');
    }
  }

  // Item ouvert hors du lecteur pendant une vérif : on peut l'ajouter aux scans de la vérif en cours
  function addFicheItem() {
    const scan = parseCode(fiche.id);
    if (scan.kind !== 'item') return;
    startCamera();
    closeFiche();
    history.pushState({ inApp: true }, '', 'verif');
    switchTab('todo');
    scanItem(scan);
  }

  $('#opening-close').addEventListener('click', closeOpening);
  $('#fiche-launch').addEventListener('click', launchFromFiche);
  $('#fiche-add').addEventListener('click', addFicheItem);
  $('#fiche-back').addEventListener('click', () => {
    if (history.state && history.state.inApp) history.back();
    else if (state.lot) { closeFiche(); enterScanner(true); switchTab('todo'); }
    else { closeFiche(); showHome('menu'); }
  });
  $('#fiche-home').addEventListener('click', () => {
    closeFiche();
    showHome('menu');
  });

  function renderFiche() {
    const body = $('#fiche-body');
    const launch = $('#fiche-launch');
    const add = $('#fiche-add');
    add.hidden = true;
    launch.hidden = false;
    $('#fiche-actions').hidden = false;
    launch.disabled = false;
    if (fiche.error) {
      body.replaceChildren(el('div', { class: 'banner bad' }, fiche.error), el('p', { class: 'sub' }, fiche.id));
      launch.disabled = true;
      return;
    }
    if (fiche.kind === 'lot' && fiche.lot) renderLotFiche(body, fiche.lot);
    else if (fiche.kind === 'item' && fiche.item) renderItemFiche(body, fiche.item);
    else {
      body.replaceChildren(el('p', { class: 'sub' }, 'Chargement…'));
      launch.disabled = true;
    }
  }

  // Resultat de l'etiquette du scelle scannee, en tete de la fiche du lot
  function sealBanner(lot) {
    const sealed = lot.is_sealed ? `Scellé${lot.seal_number ? ' n°' + lot.seal_number : ''} ${fmtOn(lot.sealed, { long: true })}`
      + (lot.sealed_by ? ' par ' + lot.sealed_by : '') : '';
    if (lot.seal_check === 'valid' && !lot.expired_count) {
      return el('div', { class: 'banner ok' }, el('b', {}, '🔒 Scellé intact'),
        el('div', {}, 'Lot valide sans vérif' + (lot.valid_until ? ` jusqu'au ${fmtDate(lot.valid_until)}` : '')),
        sealed ? el('div', { style: 'font-weight:400' }, sealed) : '');
    }
    if (lot.seal_check === 'valid') {
      return el('div', { class: 'banner bad' }, el('b', {}, '✘ Scellé intact mais périmés'),
        el('div', {}, `${lot.expired_count} item(s) périmé(s) : lot à ouvrir`), sealed ? el('div', { style: 'font-weight:400' }, sealed) : '');
    }
    if (lot.seal_check === 'wrong') {
      return el('div', { class: 'banner warn' }, el('b', {}, "⚠ Étiquette d'un ancien scellé"),
        el('div', {}, 'Un scellé plus récent a été posé depuis : cette étiquette n\'est plus valable.'));
    }
    return el('div', { class: 'banner bad' }, el('b', {}, '✘ Scellé brisé'),
      el('div', {}, 'Le lot doit être vérifié.'),
      lot.unsealed ? el('div', { style: 'font-weight:400' }, `Scellé brisé ${fmtOn(lot.unsealed, { long: true })}`
        + (lot.unsealed_by ? ' par ' + lot.unsealed_by : '')) : '');
  }

  function renderLotFiche(body, lot) {
    const status = groupStatus(lot) || lotStatus(lot);
    const current = state.lotId === lot.id && verifInProgress();
    const launch = $('#fiche-launch');
    launch.textContent = current ? 'Reprendre la vérif' : 'Lancer une vérif';
    // etiquette du scelle scannee : le lot reste ferme, pas de vérif a lancer tant qu'il est scelle (etiquette d'un
    // ancien scelle comprise : un scelle plus recent a ete pose depuis)
    const sealIntact = !!fiche.seal && lot.seal_check === 'valid';
    const noVerif = !!fiche.seal && lot.is_sealed;
    if (noVerif && !current) launch.hidden = true;
    $('#fiche-actions').hidden = launch.hidden && $('#fiche-add').hidden;
    const parts = [
      el('div', { class: 'title-row' },
        el('div', {}, el('h2', {}, lot.name), el('div', { class: 'sub' }, `${lot.lot_type_name} · ${lot.id}`)),
        el('a', { class: 'pdf-link', href: new URL(`lots/${encodeURIComponent(lot.id)}/sheet.pdf`, API).href,
          target: '_blank', rel: 'noopener', title: "Fiche d'inventaire papier (PDF)" }, '📄 Fiche PDF')),
      (lot.path || []).length ? el('p', { class: 'sub' }, 'Dans : ',
        ...lot.path.flatMap((parent, index) => [index ? ' › ' : '', lotLink(parent)])) : '',
      fiche.seal ? sealBanner(lot) : '',
      sealIntact ? '' : el('div', { class: 'banner ' + status.kind }, status.label),
      reportButton({ lot: lot.id, lotName: lot.name, label: lot.name }),
      current ? el('p', {}, `▶ Vérif en cours sur ce téléphone : ${state.scanned.length} scan(s).`) : '',
      lot.is_sealed && !sealIntact ? el('p', {}, `🔒 Scellé${lot.seal_number ? ' n°' + lot.seal_number : ''} ${fmtOn(lot.sealed, { long: true })}`
        + (lot.sealed_by ? ` par ${lot.sealed_by}` : '') + ' : pas de vérif nécessaire tant que le scellé est intact.') : '',
      el('p', {}, 'Dernière vérif : ' + (lot.last_verif
        ? fmtDateTime(lot.last_verif, { long: true }) + (lot.last_verif_by ? ' par ' + lot.last_verif_by : '') : 'jamais')),
      expiryLine(lot),
      revisionLine(lot),
      !lot.verif_recommended ? '' : el('p', {}, lot.restocked_count
        ? `⚠ Réassort de ${lot.restocked_count} item(s) ${fmtOn(lot.restocked, { long: true })}${lot.restocked_by ? ' par ' + lot.restocked_by : ''} : vérif complète recommandée.`
        : lot.back_from_list ? `⚠ Rentré avec la liste « ${lot.back_from_list} » ${fmtOn(lot.restocked, { long: true })}${lot.restocked_by ? ' par ' + lot.restocked_by : ''} : vérif complète recommandée.`
        : `⚠ Scellé ouvert${lot.unsealed ? ' ' + fmtOn(lot.unsealed, { long: true }) : ''}${lot.unsealed_by ? ' par ' + lot.unsealed_by : ''} : vérif complète recommandée.`),
      lot.seal_required && !lot.is_sealed
        ? el('p', {}, '🔒 Scellé obligatoire pour ce type de lot : il reste à traiter tant qu\'un nouveau scellé n\'est pas posé.') : '',
      lot.global && lot.global.id !== lot.id
        ? el('p', {}, 'Lot global : ', ficheLink('lot', lot.global.id, lot.global.name), ` · ${lot.global.label}`) : '',
    ];

    // etiquette privee : scannee (cle verifiee par le serveur) ou ou la trouver
    const keyBox = [];
    if (fiche.keyCheck === 'valid') keyBox.push(el('b', {}, '🔑 Étiquette clé scannée : la vérif pourra être validée.'));
    else if (fiche.keyCheck) keyBox.push(el('b', {}, '✘ Étiquette clé refusée'), el('div', {}, fiche.keyError));
    keyBox.push(el('div', {}, lot.key_location ? `Emplacement : ${cap(lot.key_location)}` : 'Emplacement non renseigné'));
    if (fiche.keyCheck !== 'valid') {
      keyBox.push(el('div', { class: 'sub' }, "QR code rangé à l'intérieur du lot, à scanner pendant la vérif pour pouvoir la valider."));
    }
    // lot scelle intact : on ne le verifie pas, l'etiquette cle n'a pas a etre cherchee
    if (!noVerif) parts.push(el('h3', {}, 'Étiquette clé'),
      el('div', { class: 'key-box' + (fiche.keyCheck && fiche.keyCheck !== 'valid' ? ' bad' : '') }, ...keyBox));

    // contenu : le lot puis chacun de ses sous-lots (attendus, puis items enregistres)
    parts.push(el('h3', {}, 'Contenu'));
    const lots = [lot, ...(lot.descendants || [])];
    const tags = { ok: 'green', warn: 'orange', bad: 'red' };
    for (const sub of lots) {
      const requirements = sub.requirements || [];
      // emplacement d'un item dans le lot : celui prevu pour son type (ex. « pochette bleue »)
      const places = new Map(requirements.filter((row) => row.location).map((row) => [row.type, row.location]));
      const place = (item) => places.get(item.type) || '';
      const items = [...(sub.items || [])].sort((a, b) => place(a).localeCompare(place(b), 'fr')
        || a.type_name.localeCompare(b.type_name, 'fr')
        || String(a.peremption || '9').localeCompare(String(b.peremption || '9')));
      const missing = sub.missing_items || [];
      if (sub !== lot) {
        if (!requirements.length && !items.length && !missing.length && !(sub.descendants || []).length) continue;
        const subStatus = lotStatus(sub);
        parts.push(el('ul', { class: 'list' }, el('li', { class: 'lot-head ' + subStatus.kind,
          style: `padding-left:${12 + Math.max(0, (sub.depth || 1) - 1) * 14}px`, onclick: () => openFiche('lot', sub.id) },
        el('div', { class: 'main' }, el('div', { class: 'name' }, '📦 ', ficheLink('lot', sub.id, sub.name)),
          el('div', { class: 'sub' }, `${sub.lot_type_name} · ${sub.item_count} item(s)`)),
        el('span', { class: 'tag ' + tags[subStatus.kind] }, subStatus.label.replace(/^[✔✘⚠] /, '').split(',')[0]))));
      }
      // une jauge par type attendu ; la fleche au bout deplie ses iids avec leur peremption
      const pairs = [...items.map((item) => [item, '']), ...missing.map((item) => [item, 'disparu'])];
      const ofType = (type) => pairs.filter(([item]) => item.type === type);
      for (const row of requirements) {
        parts.push(...ficheFold(`${sub.id}:${row.type}`, ofType(row.type), (arrow) => requirementRow(
          row.type_name + (row.location ? ` (${row.location})` : ''), row.present, row.required, arrow)));
      }
      // items hors definition (ou d'un rangement) : une ligne par type, sans jauge
      const known = new Set(requirements.map((row) => row.type));
      const extra = [...new Set(pairs.filter(([item]) => !known.has(item.type)).map(([item]) => item.type))];
      for (const type of extra) {
        const rows = ofType(type);
        const label = rows[0][0].type_name + (requirements.length ? ' (hors définition)' : '');
        parts.push(...ficheFold(`${sub.id}:${type}`, rows, (arrow) => el('div', { class: 'req' },
          el('span', { class: 'label' }, label), el('span', { class: 'req-count' }, String(rows.filter(([, tag]) => !tag).length)), arrow)));
      }
      if (!pairs.length && !requirements.length && sub === lot && lots.length === 1) parts.push(el('p', { class: 'sub' }, 'Aucun item enregistré.'));
    }
    body.replaceChildren(...parts);
  }

  // Ligne d'un type dans la fiche de lot, avec une fleche au bout pour deplier ses iids (replies par defaut,
  // remis a zero a chaque fiche ouverte)
  function ficheFold(key, rows, line) {
    const open = fiche.folds.has(key);
    const arrow = el('button', { type: 'button', class: 'fold', disabled: !rows.length, 'aria-expanded': String(open),
      'aria-label': open ? 'Replier les items' : 'Afficher les items',
      onclick: () => { if (open) fiche.folds.delete(key); else fiche.folds.add(key); renderFiche(); } }, open ? '▾' : '▸');
    return [line(arrow), open ? el('ul', { class: 'iids' }, ...rows.map(([item, tag]) => iidRow(item, tag))) : ''];
  }

  // Item deplie sous sa jauge : iid et peremption, touche pour ouvrir sa fiche
  function iidRow(item, missingTag) {
    return el('li', { class: item.expired ? 'expired' : missingTag ? 'missing' : '', onclick: () => openFiche('item', item.iid) },
      el('span', { class: 'iid' }, ficheLink('item', item.iid, item.iid)),
      el('span', { class: 'date' }, item.peremption ? fmtDate(item.peremption) : 'non périssable'),
      item.expired ? el('span', { class: 'tag red' }, badLabel(item)) : null,
      item.reported && !item.unusable ? el('span', { class: 'tag orange' }, 'signalé') : null,
      item.revision_late && !item.expired ? el('span', { class: 'tag orange' }, 'révision en retard') : null,
      missingTag ? el('span', { class: 'tag orange' }, missingTag) : null);
  }

  // Prochaine peremption d'un lot et de ses sous-lots : la plus proche parmi les items pas encore perimes
  function expiryLine(lot) {
    let next = null;
    let expired = 0;
    for (const sub of [lot, ...(lot.descendants || [])]) {
      for (const item of sub.items || []) {
        if (item.expired) { expired += 1; continue; }
        if (!item.peremption) continue;
        if (!next || item.peremption < next.date) next = { date: item.peremption, names: new Set() };
        if (item.peremption === next.date) next.names.add(item.type_name);
      }
    }
    const soon = next && parseDate(next.date) - today() <= 30 * 86400000;
    return el('p', { class: soon ? 'warn-text' : '' }, (soon ? '⚠ ' : '') + 'Prochaine péremption : ',
      el('b', {}, next ? fmtDate(next.date) : 'aucune'),
      next ? ` (${[...next.names].join(', ')})` : ' (aucun item périssable)',
      expired ? ` · ${expired} item(s) déjà périmé(s)` : '');
  }

  // Prochaine revision du materiel d'un lot et de ses sous-lots (rien si le lot n'a pas de materiel a reviser)
  function revisionLine(lot) {
    let next = null;
    let late = 0;
    for (const sub of [lot, ...(lot.descendants || [])]) {
      for (const item of sub.items || []) {
        if (!item.revision || !item.revision_due) continue;
        if (item.revision_late) { late += 1; continue; }
        if (!next || item.revision_due < next.date) next = { date: item.revision_due, names: new Set() };
        if (item.revision_due === next.date) next.names.add(item.type_name);
      }
    }
    if (!next && !late) return '';
    const soon = late || (next && parseDate(next.date) - today() <= 30 * 86400000);
    return el('p', { class: soon ? 'warn-text' : '' }, (soon ? '⚠ ' : '') + 'Prochaine révision : ',
      el('b', {}, next ? fmtDate(next.date) : '–'),
      next ? ` (${[...next.names].join(', ')})` : '',
      late ? ` · ${late} révision(s) en retard` : '');
  }

  const ITEM_STATUS = { out: 'Sorti du stock', loose: 'Hors lot, pas encore en stock', missing: 'Signalé disparu', deleted: 'Marqué supprimé', replaced: 'Remplacé (périmé)' };

  function renderItemFiche(body, item) {
    const where = item.last_location;
    const launch = $('#fiche-launch');
    launch.textContent = where ? `Lancer une vérif de ${where.name}` : 'Lancer une vérif';
    launch.disabled = !where;
    const add = $('#fiche-add');
    add.hidden = !verifInProgress();
    add.textContent = `Ajouter aux scans de la vérif en cours (${state.lot ? state.lot.name : ''})`;
    const statusText = ITEM_STATUS[item.status];
    const banner = item.disabled ? { kind: 'bad', label: '✘ Désactivé : à remplacer' + (statusText ? ' · ' + statusText : '') }
      : item.unusable ? { kind: 'bad', label: '✘ Signalé inutilisable : à remplacer' + (statusText ? ' · ' + statusText : '') }
      : item.outdated ? { kind: 'bad', label: '✘ Périmé' + (statusText ? ' · ' + statusText : '') }
      : item.expired && item.revision_late ? { kind: 'bad', label: '✘ Révision en retard : inutilisable avant révision' + (statusText ? ' · ' + statusText : '') }
      : item.expired ? { kind: 'bad', label: '✘ À remplacer' + (statusText ? ' · ' + statusText : '') }
      : item.revision_late ? { kind: 'warn', label: '⚠ Révision en retard, toujours utilisable' + (statusText ? ' · ' + statusText : '') }
      : statusText ? { kind: 'warn', label: '⚠ ' + statusText } : { kind: 'ok', label: '✔ Valide' };
    const fact = (label, ...value) => [el('dt', {}, label), el('dd', {}, ...value)];
    const place = where
      ? [...where.path.flatMap((parent) => [lotLink(parent), ' › ']), lotLink(where),
        where.current ? '' : el('div', { class: 'sub' }, 'Dernier emplacement connu' + (where.at ? `, quitté ${fmtOn(where.at, { long: true })}` : ''))]
      : [item.status === 'active' ? 'En stock' : 'Inconnu'];
    body.replaceChildren(
      el('h2', {}, item.type_name),
      el('div', { class: 'sub' }, item.iid),
      el('div', { class: 'banner ' + banner.kind }, banner.label),
      item.reported && !item.unusable ? el('p', { class: 'warn-text' }, '⚠ Problème signalé : toujours utilisable, suivi par le TECH.') : '',
      item.revision && !['deleted', 'replaced'].includes(item.status)
        ? el('button', { type: 'button', class: 'primary revise-btn', onclick: () => openRevision(item) }, '🔧 Réviser') : '',
      reportButton({ item: item.iid, label: `${item.type_name} (${item.iid})`, lot: item.location, lotName: item.location_name }),
      el('dl', { class: 'facts' },
        item.disabled ? fact('Désactivé', item.disabled_reason || '–', el('div', { class: 'sub' },
          [item.disabled_at ? cap(fmtOn(item.disabled_at, { long: true })) : '', item.disabled_by ? 'par ' + item.disabled_by : ''].filter(Boolean).join(' '))) : [],
        fact('Péremption', item.peremption ? fmtDate(item.peremption) : 'Non périssable'),
        fact(where && !where.current ? 'Dernier lieu' : 'Emplacement', ...place),
        item.status === 'out' && item.out_until ? fact('Compté utilisé', `le ${fmtDate(item.out_until)} s'il ne revient pas`) : [],
        fact('Dernier passage', item.last_seen ? cap(fmtDateTime(item.last_seen, { long: true })) + (item.last_seen_by ? ' par ' + item.last_seen_by : '') : '–'),
        fact('Ajouté', item.added ? cap(fmtDateTime(item.added, { long: true })) + (item.added_by ? ' par ' + item.added_by : '') : '–'),
        item.tear_off ? fact('Étiquette', 'à déchirer avant utilisation') : [],
        item.consumable === false ? fact('Usage', 'non consommable : retiré d\'un lot, il passe hors lot (pas perdu)') : [],
        item.revision ? fact('Révision', item.revision_due ? `prochaine le ${fmtDate(item.revision_due)}` : 'pas encore prévue',
          el('div', { class: 'sub' }, `tous les ${item.revision_months} mois` + (item.revision_usable ? '' : ', inutilisable en retard'))) : [],
        item.revision && item.revised_at ? fact('Dernière révision', cap(fmtOn(item.revised_at, { long: true })) + (item.revised_by ? ' par ' + item.revised_by : '')) : [],
        (item.revision_tasks || []).length ? fact('Tâches de révision', el('ul', { class: 'tasks-facts' }, ...item.revision_tasks.map((task) => el('li', {}, task)))) : [],
        (item.tasks || []).length ? fact('Tâches à la vérif', el('ul', { class: 'tasks-facts' }, ...item.tasks.map((task) => el('li', {}, task)))) : [],
        item.deleted_reason ? fact('Raison', item.deleted_reason) : []),
      where ? '' : el('p', { class: 'sub' }, "Aucun lot connu pour cet item : il n'y a pas de vérif à lancer."),
    );
  }


  // Revision du materiel (fiche de l'item, « Réviser ») : connexion, taches toutes cochees, prochaine revision
  // (par defaut aujourd'hui + la periodicite du type, modifiable)
  function isoDay(d) { return `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, '0')}-${String(d.getDate()).padStart(2, '0')}`; }

  function addMonths(d, months) {
    const day = d.getDate();
    const result = new Date(d.getFullYear(), d.getMonth() + months, 1);
    result.setDate(Math.min(day, new Date(result.getFullYear(), result.getMonth() + 1, 0).getDate()));
    return result;
  }

  function openRevision(item) {
    requireLogin('faire la révision', () => showRevision(item));
  }

  function showRevision(item) {
    const dialog = $('#revision');
    const tasks = item.revision_tasks || [];
    $('#revision-title').textContent = 'Révision : ' + item.type_name;
    $('#revision-sub').textContent = item.iid + (item.revision_due ? ` · prévue le ${fmtDate(item.revision_due)}` : '');
    $('#revision-intro').textContent = tasks.length ? 'Cochez toutes les tâches de la révision.' : 'Pas de tâche définie pour ce type.';
    const boxes = tasks.map(() => el('input', { type: 'checkbox' }));
    const next = $('#revision-next');
    const update = () => { $('#revision-ok').disabled = !boxes.every((box) => box.checked) || !next.value; };
    for (const box of boxes) box.addEventListener('change', update);
    $('#revision-list').replaceChildren(...tasks.map((task, index) => el('li', {}, el('label', {}, boxes[index], ' ', task))));
    const tomorrow = new Date(today().getTime() + 86400000);
    next.min = isoDay(tomorrow);
    next.value = isoDay(addMonths(today(), item.revision_months || 1));
    next.oninput = update;
    $('#revision-next-hint').textContent = `Par défaut dans ${item.revision_months} mois (périodicité du type).`;
    $('#revision-note').value = '';
    $('#revision-error').textContent = '';
    update();
    $('#revision-form').onsubmit = async (event) => {
      event.preventDefault();
      $('#revision-ok').disabled = true;
      try {
        await api(`items/${encodeURIComponent(item.iid)}/revision/`, { user: credentials(), tasks, next_due: next.value,
          note: $('#revision-note').value });
        dialog.close();
        feedback.info();
        toast(`Révision enregistrée : prochaine le ${fmtDate(next.value)}.`);
        if (ficheOpen()) openFiche(fiche.kind, fiche.id, { key: fiche.key, seal: fiche.seal, push: false });
      } catch (e) {
        update();
        $('#revision-error').textContent = e.message;
        if (e.status === 403) { state.user = null; save(); render(); }
      }
    };
    $('#revision-cancel').onclick = () => dialog.close();
    if (!dialog.open) dialog.showModal();
  }

  // Item (ou paquet ferme) signale scanne, quel que soit le signalement : popup avec les signalements ouverts et
  // leurs commentaires, et bip. Une seule fois par objet sur une courte duree (fiche rouverte, rescan).
  let lastReportAlert = { key: '', at: 0 };
  function alertReports(key, title, reports) {
    if (!reports || !reports.length) return;
    if (lastReportAlert.key === key && Date.now() - lastReportAlert.at < 30000) return;
    lastReportAlert = { key, at: Date.now() };
    feedback.warn();
    $('#reported-title').textContent = title;
    $('#reported-body').replaceChildren(...reports.map((report) => el('div', { class: 'reported-item' + (report.usable ? '' : ' bad') },
      el('b', {}, report.usable ? '⚠ Toujours utilisable' : '✘ Inutilisable : à remplacer'),
      el('div', { class: 'sub' }, `${report.status_label} · signalé par ${report.by} ${fmtOn(report.created)}`),
      report.comment ? el('p', { class: 'comment' }, '« ' + report.comment + ' »') : el('p', { class: 'sub' }, 'Sans commentaire.'),
      report.note ? el('p', { class: 'note' }, '💬 ' + report.note) : '')));
    const dialog = $('#reported');
    $('#reported-close').onclick = () => dialog.close();
    if (!dialog.open) dialog.showModal();
  }

  // ------------------------------------------------------------------------------------------------
  // Signalements de problemes : (!) a cote d'un item ou d'un type pendant la vérif, « Signaler un problème » sur les
  // fiches et l'onglet Lot. Connexion demandee avant de rediger quoi que ce soit ; envoye tout de suite, sans
  // attendre la vérif. Un item signale pendant la vérif compte comme vu (jamais marque disparu) ; signale
  // inutilisable, il est a remplacer comme un perime.

  const badLabel = (info) => (!info || info.outdated ? 'PÉRIMÉ' : info.disabled ? 'DÉSACTIVÉ' : info.unusable ? 'INUTILISABLE'
    : info.revision_late ? 'RÉVISION EN RETARD' : 'PÉRIMÉ');
  let loginAction = null; // {action, at} : a lancer une fois connecte

  function takeLoginAction() {
    const pending = loginAction;
    loginAction = null;
    return pending && Date.now() - pending.at < 5 * 60 * 1000 ? pending.action : null;
  }

  // Action reservee a un utilisateur connecte : badge (petit lecteur, ou camera du scanner) ou nom + PIN
  function requireLogin(reason, action) {
    if (state.user) { action(); return; }
    const dialog = $('#login-needed');
    $('#login-needed-text').textContent = `Connectez-vous pour ${reason}.`;
    $('#login-needed-badge').onclick = () => {
      dialog.close();
      if (inHome() || ficheOpen()) openReader();
      else { startCamera(); toast(`Scannez votre badge pour ${reason}.`); }
      loginAction = { action, at: Date.now() };
    };
    $('#login-needed-name').onclick = () => { dialog.close(); loginAction = { action, at: Date.now() }; openNameLogin(); };
    $('#login-needed-cancel').onclick = () => { dialog.close(); loginAction = null; };
    if (!dialog.open) dialog.showModal();
  }

  // target : {item, lot, lotName, type, label}
  function reportMark(target) {
    return el('button', { type: 'button', class: 'report-btn report-mark', title: 'Signaler un problème',
      'aria-label': 'Signaler un problème', onclick: (event) => { event.stopPropagation(); openReport(target); } }, '!');
  }

  function reportButton(target) {
    return el('button', { type: 'button', class: 'report-btn report-wide', onclick: () => openReport(target) },
      '⚠ Signaler un problème');
  }

  function openReport(target) {
    requireLogin('signaler un problème', () => showReportDialog(target));
  }

  function showReportDialog(target) {
    const dialog = $('#problem');
    const what = target.pack ? 'le paquet' : target.item || target.type ? "l'objet" : 'le lot';
    $('#problem-target').textContent = target.label || '';
    $('#problem-question').textContent = `${what === 'le lot' ? 'Le lot est-il' : what === 'le paquet' ? 'Le paquet est-il' : "L'objet est-il"} toujours utilisable ?`;
    $('#problem-usable-yes').checked = false;
    $('#problem-usable-no').checked = false;
    $('#problem-comment').value = '';
    $('#problem-error').textContent = '';
    $('#problem-send').disabled = false;
    $('#problem-form').onsubmit = async (event) => {
      event.preventDefault();
      const yes = $('#problem-usable-yes').checked;
      if (!yes && !$('#problem-usable-no').checked) {
        $('#problem-error').textContent = `Indiquez si ${what} est toujours utilisable.`;
        return;
      }
      $('#problem-send').disabled = true;
      try {
        const body = await api('reports/', { user: credentials(), lot: target.item || target.pack ? '' : target.lot || '',
          item: target.item || '', type: target.type || '', pack: target.pack || '', comment: $('#problem-comment').value,
          usable: yes });
        dialog.close();
        feedback.info();
        toast('Signalement envoyé au TECH.');
        // son propre signalement : pas de popup quand la fiche se recharge
        lastReportAlert = { key: target.item ? 'item:' + target.item : 'pack:' + (target.pack || ''), at: Date.now() };
        reported(body.item);
        state.reports = null;
      } catch (e) {
        $('#problem-send').disabled = false;
        $('#problem-error').textContent = e.message;
        if (e.status === 403) { state.user = null; save(); render(); }
      }
    };
    $('#problem-cancel').onclick = () => dialog.close();
    if (!dialog.open) dialog.showModal();
  }

  // Item signale : pendant la vérif de son lot, il compte comme vu (ajoute aux scans s'il n'y est pas)
  async function reported(item) {
    if (item && state.lot && item.location && sessionLots().some((lot) => lot.id === item.location)) {
      const entry = state.scanned.find((row) => row.kind === 'item' && row.iid === item.iid);
      if (entry) Object.assign(entry, { info: item, expired: item.expired });
      else if (!live.entries().some((row) => row.kind === 'item' && row.iid === item.iid)) {
        scannedAgain();
        state.lastVerif = null;
        state.scanned.push({ code: item.iid, kind: 'item', iid: item.iid, expired: item.expired, info: item, error: '',
          place: item.location });
        showInfo(item.expired ? 'bad' : 'warn', `Signalé : ${item.type_name}`,
          item.expired ? 'Inutilisable : à remplacer, compté comme vu.' : 'Toujours utilisable : compté comme vu.');
      }
      save();
      await loadLot(state.lotId); // etat a jour des items du lot (inutilisable : a remplacer)
    }
    if (ficheOpen()) openFiche(fiche.kind, fiche.id, { key: fiche.key, seal: fiche.seal, push: false });
  }

  // Page « Signalements » : lecture seule pour tous, gestion pour les admins
  async function loadReports(quiet = false) {
    if (!state.user || state.reportsBusy) return;
    state.reportsBusy = true;
    if (!quiet) renderHomePage();
    try {
      state.reports = await api('reports/list/', { user: credentials(), show: state.reportsShow });
    } catch (e) {
      if (!quiet) toast('Signalements : ' + e.message, true);
      if (e.data && e.data.pin_required) { state.user = null; save(); }
    } finally {
      state.reportsBusy = false;
      renderHomePage();
    }
  }

  const reportLabel = (row) => (row.status === 'new' ? 'Nouveau'
    : row.status === 'resolved' ? 'Résolu' + (row.resolution_label ? ' · ' + row.resolution_label.toLowerCase() : '')
    : `En cours · ${row.usable ? 'utilisable' : 'inutilisable'}`);

  async function updateReport(row, changes) {
    try {
      await api(`reports/${row.id}/`, { user: credentials(), ...changes });
      toast('Signalement mis à jour.');
    } catch (e) {
      toast(e.message, true);
      if (e.data && e.data.pin_required) { state.user = null; save(); }
    }
    loadReports(true);
  }

  function reportCard(row, manage) {
    const target = row.item ? ficheLink('item', row.item, row.target) : row.lot ? ficheLink('lot', row.lot, row.target) : row.target;
    const parts = [
      el('div', { class: 'row-head' }, el('div', { class: 'name' }, target), el('span', { class: 'tag ' + row.color }, reportLabel(row))),
      el('div', { class: 'sub' }, `Signalé par ${row.by} ${fmtOn(row.created, { long: true })}` + (row.item ? ` · ${row.item}` : '')),
      el('div', {}, row.usable ? '✔ Toujours utilisable' : '✘ Inutilisable : à remplacer'),
      row.comment ? el('p', { class: 'comment' }, row.comment) : '',
      row.note ? el('p', { class: 'note' }, '💬 ' + row.note) : '',
      row.updated ? el('div', { class: 'sub' }, `Mis à jour ${fmtOn(row.updated, { long: true })}` + (row.updated_by ? ` par ${row.updated_by}` : '')) : '',
    ];
    if (manage) {
      const note = el('textarea', { rows: 2, maxlength: 1000, placeholder: 'Réponse visible de tous (facultatif)' });
      note.value = row.note || '';
      const buttons = row.status === 'resolved'
        ? [el('button', { type: 'button', onclick: () => updateReport(row, { status: 'progress' }) }, 'Rouvrir')]
        : [
          el('button', { type: 'button', class: 'primary', onclick: () => updateReport(row, { status: 'progress', usable: true }) },
            row.status === 'progress' && row.usable ? '✔ En cours · utilisable' : 'En cours · utilisable'),
          el('button', { type: 'button', class: 'warning', onclick: () => updateReport(row, { status: 'progress', usable: false }) },
            row.status === 'progress' && !row.usable ? '✔ En cours · inutilisable' : 'En cours · inutilisable'),
          ...(row.item ? [
            // item non conforme : desactive (remplace a la prochaine verif comme un perime) ou deja retire
            el('button', { type: 'button', class: 'primary', onclick: () => updateReport(row, { status: 'resolved', resolution: 'ok' }) },
              'Résolu · remis en service'),
            el('button', { type: 'button', class: 'warning', onclick: () => {
              if (confirm(`Désactiver ${row.item} ? Il devra être remplacé comme un périmé.`)) updateReport(row, { status: 'resolved', resolution: 'disabled' });
            } }, 'Résolu · item désactivé'),
            el('button', { type: 'button', class: 'danger', onclick: () => {
              if (confirm(`Retirer ${row.item} de l'inventaire (déjà jeté) ?`)) updateReport(row, { status: 'resolved', resolution: 'removed' });
            } }, 'Résolu · item retiré'),
          ] : [el('button', { type: 'button', class: 'primary', onclick: () => updateReport(row, { status: 'resolved' }) }, 'Résolu')]),
        ];
      parts.push(el('div', { class: 'report-manage' }, note,
        el('button', { type: 'button', onclick: () => updateReport(row, { note: note.value }) }, 'Enregistrer la réponse'),
        el('div', { class: 'report-actions' }, ...buttons)));
    }
    return el('div', { class: 'report-card ' + row.color }, ...parts);
  }

  function renderReports() {
    const view = $('#home-reports');
    const data = state.reports;
    const choose = (show) => { state.reportsShow = show; loadReports(); };
    const parts = [
      el('div', { class: 'row-head' }, el('h3', {}, 'Signalements'),
        el('button', { type: 'button', onclick: () => loadReports() }, state.reportsBusy ? 'Chargement…' : 'Actualiser')),
      el('div', { class: 'segmented' },
        el('button', { type: 'button', class: state.reportsShow === 'open' ? 'active' : '', onclick: () => choose('open') }, 'En cours'),
        el('button', { type: 'button', class: state.reportsShow === 'all' ? 'active' : '', onclick: () => choose('all') }, 'Tous')),
    ];
    if (!data) {
      parts.push(el('p', { class: 'hint' }, state.reportsBusy ? 'Chargement…' : 'Liste non chargée.'));
      view.replaceChildren(...parts);
      return;
    }
    parts.push(el('p', { class: 'hint' }, data.can_manage
      ? 'Rouge : nouveau. Orange : en cours, objet inutilisable. Vert : en cours et toujours utilisable, ou résolu.'
      : 'Suivi en lecture seule. Rouge : nouveau, orange : en cours (inutilisable), vert : utilisable ou résolu.'));
    if (!data.reports.length) parts.push(el('p', { class: 'hint' }, state.reportsShow === 'open' ? 'Aucun signalement en cours.' : 'Aucun signalement.'));
    parts.push(...data.reports.map((row) => reportCard(row, data.can_manage)));
    view.replaceChildren(...parts);
  }

  // ------------------------------------------------------------------------------------------------
  // Listes de materiel (checklists.py) : lots et materiel a prendre (ex : un chef demande du materiel a ses
  // equipiers), partagees par lien et QR code (<base>/liste?id=ID). Elles se verifient en direct, a plusieurs :
  // chaque scan va tout de suite au serveur et la page relit la liste. Validee, la liste sort son materiel du stock ;
  // au retour on scanne ce qui rentre, puis « Clôturer le retour » compte le reste comme utilise.

  const LIST_POLL_MS = 3000;
  const LIST_TAGS = { draft: 'blue', validated: 'orange', returned: 'green' };
  const lists = {
    index: null,     // {lists, can_see_all}
    show: 'mine',    // 'mine' (les miennes et les modeles), 'templates', 'all' (gestion, admin)
    id: '',          // liste ouverte
    list: null,      // detail de la liste ouverte
    error: '',
    mode: false,     // lecteur ouvert sur la liste (verif, ou retour pour une liste sortie)
    message: null,   // dernier scan : {kind: 'ok' | 'bad', text}
    edit: null,      // editeur : {key, id, name, note, template, lines: [{lot, label} | {type, label, quantity}]}
    options: null,   // choix de l'editeur : {lots, types}
    busy: false,
    timer: 0,
    qr: { url: '', src: '' },
    qrOpen: '',
  };

  const listUrl = (id) => new URL(`liste?id=${encodeURIComponent(id)}`, document.baseURI).href;
  const onListsPage = () => inHome() && state.homePage === 'lists';
  // un code scanne sur la page d'une liste a verifier ou a rentrer y compte (lecteur ou douchette)
  const listScanning = () => onListsPage() && !!lists.list && !lists.edit && !lists.list.template
    && (lists.list.status === 'draft' || lists.list.status === 'validated');

  function openLists(push = true) {
    Object.assign(lists, { id: '', list: null, error: '', edit: null, message: null });
    stopListScan();
    showHome('lists', push);
  }

  function openList(id, push = true) {
    if (lists.id !== id) Object.assign(lists, { list: null, message: null });
    Object.assign(lists, { id, error: '', edit: null });
    stopListScan();
    if (push) history.pushState(null, '', listUrl(id));
    showHome('lists', false);
  }

  // showHome('lists') : charge l'index ou la liste ouverte, relit la liste ouverte toutes les LIST_POLL_MS
  function listsShown() {
    clearInterval(lists.timer);
    lists.timer = 0;
    if (!state.user) return;
    if (lists.id) {
      loadList();
      lists.timer = setInterval(() => {
        if (!onListsPage() || !lists.id) { clearInterval(lists.timer); lists.timer = 0; return; }
        if (!lists.edit && !lists.busy && !document.hidden) loadList(true);
      }, LIST_POLL_MS);
    } else {
      loadLists();
    }
  }

  async function loadLists() {
    lists.busy = true;
    renderListsPage();
    try {
      lists.index = await api('checklists/list/', { user: badge(), show: lists.show });
    } catch (e) {
      toast('Listes : ' + e.message, true);
    } finally {
      lists.busy = false;
      renderListsPage();
    }
  }

  async function loadList(quiet = false) {
    const id = lists.id;
    try {
      const list = await api(`checklists/${encodeURIComponent(id)}/`, { user: badge() });
      if (lists.id !== id) return;
      lists.list = list;
      lists.error = '';
    } catch (e) {
      if (lists.id !== id) return;
      if (!quiet || e.status === 404) lists.error = e.status === 404 ? 'Liste introuvable (supprimée ?)' : e.message;
    }
    renderListsPage();
  }

  // Action sur la liste ouverte : la reponse est la liste a jour
  async function listCall(action, body = {}) {
    const result = await api(`checklists/${encodeURIComponent(lists.id)}/${action}/`, { user: badge(), ...body });
    if (result && result.id === lists.id) lists.list = result;
    renderListsPage();
    return result;
  }

  async function listAction(action, body = {}, done = null) {
    if (lists.busy) return;
    lists.busy = true;
    try {
      const result = await listCall(action, body);
      if (done) done(result);
    } catch (e) {
      feedback.bad();
      toast(e.message, true);
    } finally {
      lists.busy = false;
      renderListsPage();
    }
  }

  function startListScan() {
    lists.mode = true;
    document.body.classList.add('login-reader', 'list-scan');
    $('#scan-hint').textContent = lists.list && lists.list.status === 'validated' ? 'Scannez ce qui rentre' : 'Scannez le matériel';
    startCamera();
    renderListsPage();
  }

  function stopListScan() {
    if (!lists.mode) return;
    lists.mode = false;
    document.body.classList.remove('list-scan');
    if (readerOpen()) closeReader();
  }

  async function listScanCode(scan) {
    const list = lists.list;
    const action = list.status === 'validated' ? 'return-scan' : 'scan';
    const body = scan.kind === 'lot' ? { lot: scan.id } : { item: scan.id };
    try {
      const result = await listCall(action, body);
      feedback.good();
      lists.message = { kind: 'ok', text: result.message || 'Scanné' };
    } catch (e) {
      feedback.bad();
      lists.message = { kind: 'bad', text: e.message };
    }
    renderListsPage();
  }

  function validateList() {
    const list = lists.list;
    const partial = !list.complete;
    if (!confirm(partial
      ? 'Liste incomplète : la valider quand même ? Ce qui manque ne sera pas pris. Le matériel scanné sort du stock.'
      : 'Valider la liste ? Le matériel scanné sort du stock et la liste ne pourra plus être vérifiée.')) return;
    listAction('validate', { partial }, () => {
      stopListScan();
      lists.message = null;
      feedback.good();
      toast(lists.list && !lists.list.went_out ? 'Liste validée.' : 'Liste validée : bonne sortie !');
    });
  }

  function closeListReturn() {
    if (!confirm('Clôturer le retour ? Les consommables pas rentrés seront comptés utilisés, le matériel non '
      + 'consommable pas rentré reste sorti.')) return;
    listAction('close-return', {}, (result) => {
      stopListScan();
      lists.message = null;
      toast(`Retour clos : ${(result.used || []).length} consommable(s) utilisé(s)`
        + ((result.still_out || []).length ? `, ${result.still_out.length} matériel(s) encore sorti(s)` : '') + '.');
    });
  }

  function duplicateList(asTemplate = false) {
    const list = lists.list;
    const name = prompt(asTemplate ? 'Nom du modèle' : 'Nom de la nouvelle liste', list.name);
    if (name === null) return;
    listAction('duplicate', { name: name || list.name, template: asTemplate }, (copy) => {
      toast(asTemplate ? 'Modèle créé.' : 'Liste créée : à vérifier.');
      openList(copy.id);
    });
  }

  function deleteList() {
    if (!confirm(`Supprimer la liste « ${lists.list.name} » ?`)) return;
    listAction('delete', {}, () => { toast('Liste supprimée.'); openLists(); });
  }

  function shareList(list) {
    if (navigator.share) {
      navigator.share({ title: `QRProtec : ${list.name}`, text: `Liste de matériel « ${list.name} »`, url: list.url })
        .catch(() => { /* partage annule */ });
    } else if (navigator.clipboard) {
      navigator.clipboard.writeText(list.url).then(() => toast('Lien copié.'), () => toast(list.url));
    } else toast(list.url);
  }

  const ICONS = {
    share: '<path d="M12 3v12M7.5 7.5 12 3l4.5 4.5"/><path d="M8 10H6a1 1 0 0 0-1 1v9a1 1 0 0 0 1 1h12a1 1 0 0 0 1-1v-9a1 1 0 0 0-1-1h-2"/>',
    qr: '<rect x="3.5" y="3.5" width="6" height="6" rx="1"/><rect x="14.5" y="3.5" width="6" height="6" rx="1"/>'
      + '<rect x="3.5" y="14.5" width="6" height="6" rx="1"/><path d="M14.5 14.5h2.5v2.5M20.5 14.5v0M14.5 20.5h2.5M20 17.5v3"/>',
  };

  function icon(name) {
    const span = el('span', { class: 'icon', 'aria-hidden': 'true' });
    span.innerHTML = `<svg viewBox="0 0 24 24" width="20" height="20" fill="none" stroke="currentColor" stroke-width="1.8"`
      + ` stroke-linecap="round" stroke-linejoin="round">${ICONS[name]}</svg>`;
    return span;
  }

  function listQr(url) {
    if (lists.qr.url !== url) {
      lists.qr = { url, src: '' };
      if (typeof qrcode === 'function') {
        const qr = qrcode(0, 'M');
        qr.addData(url);
        qr.make();
        lists.qr.src = qr.createDataURL(8, 4);
      }
    }
    return lists.qr.src;
  }

  // Editeur : nouvelle liste (list absent) ou liste a preparer
  async function editList(list = null) {
    lists.edit = {
      key: String(Date.now()),
      id: list ? list.id : '',
      name: list ? list.name : '',
      note: list ? list.note : '',
      template: list ? list.template : lists.show === 'templates',
      started: !!(list && list.lines.some((line) => line.scanned)),
      lines: list ? list.lines.map((line) => line.kind === 'task' ? { task: line.task, label: line.task } : line.kind === 'lot'
        ? { lot: line.lot, label: line.lot_name, sub: line.lot_type_name }
        : { type: line.type, label: line.type_name, quantity: line.quantity }) : [],
    };
    renderListsPage(true);
    if (!lists.options) {
      try {
        lists.options = await api('checklists/options/', { user: badge() });
      } catch (e) {
        toast('Lots et types : ' + e.message, true);
      }
      renderListsPage(true);
    }
  }

  async function saveList() {
    const edit = lists.edit;
    if (!edit.name.trim()) { toast('Donnez un nom à la liste.', true); return; }
    const body = { user: badge(), name: edit.name, note: edit.note, template: edit.template,
      lines: edit.lines.map((line) => line.task ? { task: line.task } : line.lot ? { lot: line.lot }
        : { type: line.type, quantity: line.quantity }) };
    lists.busy = true;
    try {
      const result = await api(edit.id ? `checklists/${encodeURIComponent(edit.id)}/update/` : 'checklists/', body);
      lists.edit = null;
      lists.busy = false;
      toast('Liste enregistrée.');
      if (edit.id) { lists.list = result; renderListsPage(); } else openList(result.id);
    } catch (e) {
      lists.busy = false;
      toast(e.message, true);
    }
  }

  function renderListsPage(force = false) {
    const view = $('#home-lists');
    if (!view || !onListsPage()) return;
    if (!state.user) {
      delete view.dataset.editing;
      view.replaceChildren(
        el('h3', {}, lists.id ? 'Liste de matériel' : 'Listes de matériel'),
        el('p', {}, lists.id ? 'Connectez-vous pour ouvrir cette liste et la vérifier.'
          : 'Connectez-vous pour voir et préparer les listes de matériel.'),
        el('button', { type: 'button', class: 'primary', onclick: () => openReader('lists') }, 'Scanner mon badge'),
        el('button', { type: 'button', class: 'secondary list-gap', onclick: () => { pendingAfterLogin = 'lists'; openNameLogin(); } },
          'Sans badge : nom et code PIN'));
      return;
    }
    if (lists.edit) {
      // pas de nouveau rendu pendant la saisie (le champ garderait son focus perdu)
      if (!force && view.dataset.editing === lists.edit.key) return;
      view.dataset.editing = lists.edit.key;
      view.replaceChildren(...listEditor(lists.edit));
      return;
    }
    delete view.dataset.editing;
    view.replaceChildren(...(lists.id ? listDetail() : listIndex()));
  }

  function listIndex() {
    const data = lists.index;
    const choose = (show) => { lists.show = show; loadLists(); };
    const parts = [
      el('div', { class: 'row-head' }, el('h3', {}, 'Listes de matériel'),
        el('button', { type: 'button', onclick: loadLists }, lists.busy ? 'Chargement…' : 'Actualiser')),
      el('div', { class: 'segmented' },
        el('button', { type: 'button', class: lists.show === 'mine' ? 'active' : '', onclick: () => choose('mine') }, 'Mes listes'),
        el('button', { type: 'button', class: lists.show === 'templates' ? 'active' : '', onclick: () => choose('templates') }, 'Modèles'),
        data && data.can_see_all
          ? el('button', { type: 'button', class: lists.show === 'all' ? 'active' : '', onclick: () => choose('all') }, 'Toutes') : ''),
      el('button', { type: 'button', class: 'primary list-new', onclick: () => editList() },
        lists.show === 'templates' ? '＋ Nouveau modèle' : '＋ Nouvelle liste'),
      el('p', { class: 'hint' }, 'Une liste se partage par lien ou QR code et se vérifie en direct, à plusieurs. '
        + 'Validée, son matériel sort du stock. Un modèle se duplique pour les listes qui reviennent souvent.'),
    ];
    if (!data) {
      parts.push(el('p', { class: 'hint' }, lists.busy ? 'Chargement…' : 'Listes non chargées.'));
      return parts;
    }
    if (!data.lists.length) parts.push(el('p', { class: 'hint' }, lists.show === 'templates' ? 'Aucun modèle.' : 'Aucune liste.'));
    parts.push(el('ul', { class: 'list' }, ...data.lists.map((row) => el('li', { onclick: () => openList(row.id) },
      el('div', { class: 'main' },
        el('div', { class: 'name' }, row.name),
        el('div', { class: 'sub' }, [row.owner_name, row.template ? `${row.line_count} ligne(s)`
          : `${row.done_count}/${row.line_count} ligne(s) prête(s)`, 'modifiée ' + fmtDateTime(row.updated)].join(' · '))),
      el('span', { class: 'tag ' + (row.template ? 'gray' : LIST_TAGS[row.status] || 'blue') }, row.status_label)))));
    return parts;
  }

  function listLineRows(list, line, backIids) {
    if (line.kind === 'task') {
      // tache libre : cochee a la verif de la liste (ni lot ni item)
      const editable = list.status === 'draft' && !list.template;
      return [el('li', { class: 'static ' + (line.scanned ? 'ok' : '') },
        el('label', { class: 'list-task' },
          el('input', { type: 'checkbox', checked: line.scanned ? '' : null, disabled: editable && !lists.busy ? null : '',
            onchange: (event) => listAction('check', { line: line.id, done: event.target.checked }) }),
          el('div', { class: 'main' }, el('div', { class: 'name' }, line.task),
            el('div', { class: 'sub' }, line.scanned ? `Fait ${fmtDateTime(line.done)}${line.done_by ? ' par ' + line.done_by : ''}` : 'Tâche à faire'))))];
    }
    const done = line.scanned >= line.quantity;
    const title = line.kind === 'lot' ? line.lot_name : line.type_name;
    const sub = line.kind === 'lot' ? `Lot · ${line.lot_type_name}` : `${line.quantity} demandé(s)`;
    const out = list.status !== 'draft';
    const count = line.kind === 'lot' ? (line.scanned ? '✓' : '–') : `${line.scanned}/${line.quantity}`;
    const back = out && line.scanned ? (line.kind === 'lot' ? (line.back ? 'rentré' : 'pas rentré')
      : `${line.back}/${line.scanned} rentré(s)`) : '';
    const rows = [el('li', { class: 'static ' + (out ? (line.kind === 'lot' ? line.back : line.back >= line.scanned) && line.scanned ? 'ok' : ''
      : done ? 'ok' : '') },
      // case visuelle comme pour les taches : cochee quand la ligne est prise, mais on coche en scannant
      el('span', { class: 'list-box' + (done ? ' done' : ''), 'aria-hidden': 'true' }, done ? '✓' : ''),
      el('div', { class: 'main' }, el('div', { class: 'name' }, title), el('div', { class: 'sub' }, sub + (back ? ' · ' + back : ''))),
      el('b', { class: 'list-count' + (done ? ' done' : '') }, count))];
    if (line.kind === 'type') {
      for (const scan of line.scans) {
        const returned = backIids.has(scan.iid);
        rows.push(el('li', { class: 'static list-scan-row' },
          el('div', { class: 'main' }, el('div', { class: 'sub' }, `${scan.iid}${scan.peremption ? ' · périme le ' + fmtDate(scan.peremption) : ''}`
            + ` · ${scan.by || '?'}` + (out ? (returned ? ' · ✓ rentré' : scan.status === 'missing' ? ' · utilisé' : '') : ''))),
          list.status === 'draft' ? el('button', { type: 'button', class: 'list-remove', 'aria-label': 'Retirer', title: 'Retirer de la liste',
            onclick: () => listAction('unscan', { item: scan.iid }) }, '✕') : ''));
      }
    } else if (line.scanned && list.status === 'draft') {
      rows[0].append(el('button', { type: 'button', class: 'list-remove', 'aria-label': 'Décocher', title: 'Décocher',
        onclick: () => listAction('unscan', { lot: line.lot }) }, '✕'));
    }
    return rows;
  }

  function listDetail() {
    const list = lists.list;
    const back = [el('button', { type: 'button', class: 'text-link', onclick: () => openLists() }, '← Toutes les listes')];
    if (lists.error) return [...back, el('div', { class: 'banner bad' }, lists.error)];
    if (!list) return [...back, el('p', { class: 'hint' }, 'Chargement…')];
    const parts = [...back,
      el('div', { class: 'row-head' }, el('h3', {}, list.name),
        el('span', { class: 'tag ' + (list.template ? 'gray' : LIST_TAGS[list.status] || 'blue') }, list.status_label)),
      el('p', { class: 'hint' }, `Par ${list.owner_name} · modifiée ${fmtDateTime(list.updated)}`),
      list.note ? el('p', { class: 'list-note' }, list.note) : ''];
    const buttons = [];
    const busy = lists.busy;
    if (list.template) {
      parts.push(el('div', { class: 'banner info' }, 'Modèle : dupliquez-le pour faire une liste à vérifier.'));
      buttons.push(el('button', { type: 'button', class: 'primary', disabled: busy || null, onclick: () => duplicateList(false) }, 'Utiliser ce modèle'));
    } else if (list.status === 'draft') {
      parts.push(el('div', { class: 'banner ' + (list.complete ? 'ok' : 'info') }, list.complete
        ? 'Tout est prêt : validez la liste.'
        : `${list.done_count}/${list.line_count} ligne(s) prête(s). Scannez l'étiquette des lots et le matériel pris dans le stock, cochez les tâches.`));
      buttons.push(lists.mode
        ? el('button', { type: 'button', class: 'secondary', onclick: stopListScan }, 'Arrêter le scan')
        : el('button', { type: 'button', class: 'primary', onclick: startListScan }, '📷 Scanner'));
      buttons.push(el('button', { type: 'button', class: list.complete ? 'primary' : 'warning', disabled: busy || null, onclick: validateList },
        list.complete ? 'Valider la liste' : 'Valider incomplète'));
    } else if (list.status === 'validated') {
      parts.push(el('div', { class: 'banner warn' }, `Sortie ${fmtOn(list.validated)}${list.validated_by ? ' par ' + list.validated_by : ''}. `
        + 'Au retour, scannez ce qui rentre (remis en stock), puis clôturez le retour.'));
      buttons.push(lists.mode
        ? el('button', { type: 'button', class: 'secondary', onclick: stopListScan }, 'Arrêter le scan')
        : el('button', { type: 'button', class: 'primary', onclick: startListScan }, '📷 Retour : scanner'));
      buttons.push(el('button', { type: 'button', class: 'warning', disabled: busy || null, onclick: closeListReturn }, 'Clôturer le retour'));
    } else {
      parts.push(el('div', { class: 'banner ok' }, `${list.went_out ? 'Rentrée' : 'Terminée'} ${fmtOn(list.returned)}`
        + `${list.returned_by ? ' par ' + list.returned_by : ''}. La liste ne sert plus : dupliquez-la pour la refaire.`));
    }
    if (lists.message && listScanning()) parts.push(el('div', { class: 'banner ' + lists.message.kind }, lists.message.text));
    if (listScanning()) {
      buttons.push(el('button', { type: 'button', class: 'text-link', onclick: () => {
        const code = prompt("Code de l'item ou URL d'une étiquette :");
        if (code && code.trim()) handleCode(code.trim());
      } }, 'Saisir un code'));
    }
    parts.push(el('div', { class: 'list-buttons' }, ...buttons));
    const backIids = new Set((list.back || []).filter((scan) => scan.iid).map((scan) => scan.iid));
    parts.push(el('h3', {}, 'À prendre'));
    if (!list.lines.length) parts.push(el('p', { class: 'hint' }, 'Liste vide.' + (list.can_edit ? ' Modifiez-la pour ajouter des lots et du matériel.' : '')));
    parts.push(el('ul', { class: 'list' }, ...list.lines.flatMap((line) => listLineRows(list, line, backIids))));
    if (list.status === 'draft' || list.template) {
      const src = listQr(list.url);
      // Partage replie : le bouton de partage, et le QR code qui deplie le reste (QR, explication, lien cliquable)
      const panel = el('div', { class: 'list-qr-panel', hidden: lists.qrOpen !== list.id || null },
        src ? el('img', { class: 'qr', src, alt: 'QR code de la liste' }) : '',
        el('p', { class: 'hint' }, list.template ? 'Le lien ouvre le modèle.' : 'Le lien ou le QR code ouvre la liste pour la vérifier en direct, à plusieurs.'),
        el('p', { class: 'hint' }, el('a', { class: 'break', href: list.url }, list.url)));
      const qrButton = el('button', { type: 'button', class: 'secondary icon-only', title: 'QR code et lien',
        'aria-label': 'QR code et lien', 'aria-expanded': String(lists.qrOpen === list.id), onclick: () => {
          lists.qrOpen = lists.qrOpen === list.id ? '' : list.id;
          panel.hidden = lists.qrOpen !== list.id;
          qrButton.setAttribute('aria-expanded', String(!panel.hidden));
        } }, icon('qr'));
      parts.push(el('div', { class: 'list-share' },
        el('button', { type: 'button', class: 'secondary', onclick: () => shareList(list) },
          icon('share'), navigator.share ? 'Partager le lien' : 'Copier le lien'),
        qrButton), panel);
    }
    const more = [];
    if (list.can_edit && list.status === 'draft') more.push(el('button', { type: 'button', onclick: () => editList(list) }, 'Modifier'));
    if (!list.template) more.push(el('button', { type: 'button', disabled: busy || null, onclick: () => duplicateList(false) }, 'Dupliquer'));
    more.push(el('button', { type: 'button', disabled: busy || null, onclick: () => duplicateList(true) }, list.template ? 'Dupliquer le modèle' : 'En faire un modèle'));
    if (list.can_edit && list.status !== 'validated') more.push(el('button', { type: 'button', class: 'danger-text', disabled: busy || null, onclick: deleteList }, 'Supprimer'));
    parts.push(el('h3', {}, 'Gérer'), el('div', { class: 'list-buttons' }, ...more));
    return parts;
  }

  // Champ de recherche avec resultats en direct (sans accents ni casse, debut de mots ou code), comme la recherche des
  // types d'items du poste : fleches pour choisir, Entree ou toucher un resultat pour l'ajouter.
  const searchKey = (text) => String(text || '').normalize('NFD').replace(/[\u0300-\u036f]/g, '').toLowerCase();
  function searchPicker(placeholder, items, onPick) {
    const input = el('input', { type: 'search', class: 'picker-input', placeholder, autocomplete: 'off' });
    const results = el('ul', { class: 'picker-results', hidden: '' });
    let shown = [];
    let highlighted = 0;
    const score = (item, words) => {
      const label = searchKey(item.label);
      const key = searchKey(item.key + ' ' + (item.sub || ''));
      const starts = label.split(/[^a-z0-9]+/);
      let total = 0;
      for (const word of words) {
        if (starts.some((part) => part.startsWith(word))) total += label.startsWith(word) ? 3 : 2;
        else if (label.includes(word) || key.includes(word)) total += 1;
        else return -1;
      }
      return total;
    };
    const update = () => {
      const words = searchKey(input.value).split(/\s+/).filter(Boolean);
      shown = items.map((item) => ({ item, score: score(item, words) })).filter((row) => row.score >= 0)
        .sort((a, b) => b.score - a.score || a.item.label.localeCompare(b.item.label, 'fr')).slice(0, 8).map((row) => row.item);
      highlighted = Math.min(highlighted, Math.max(0, shown.length - 1));
      results.replaceChildren(...(shown.length ? shown.map((item, index) => el('li', {
        class: index === highlighted ? 'active' : '',
        onpointerdown: (event) => { event.preventDefault(); onPick(item); },
      }, el('span', {}, item.label), item.sub ? el('small', {}, item.sub) : '')) : [el('li', { class: 'empty' }, 'Aucun résultat')]));
      results.hidden = document.activeElement !== input;
    };
    input.addEventListener('input', () => { highlighted = 0; update(); });
    input.addEventListener('focus', update);
    input.addEventListener('blur', () => { results.hidden = true; });
    input.addEventListener('keydown', (event) => {
      if (event.key === 'ArrowDown' || event.key === 'ArrowUp') {
        event.preventDefault();
        highlighted = Math.max(0, Math.min(shown.length - 1, highlighted + (event.key === 'ArrowDown' ? 1 : -1)));
        update();
      } else if (event.key === 'Enter') {
        event.preventDefault();
        if (shown[highlighted]) onPick(shown[highlighted]);
      } else if (event.key === 'Escape') {
        input.value = '';
        input.blur();
      }
    });
    return el('div', { class: 'picker' }, input, results);
  }

  function listEditor(edit) {
    const redraw = () => renderListsPage(true);
    const options = lists.options;
    const taken = new Set(edit.lines.map((line) => line.task ? 'task:' + line.task : line.lot || 'type:' + line.type));
    const taskInput = el('input', { type: 'text', maxlength: '200', placeholder: "Ex : vérifier le niveau d'eau dans le sous-sol",
      onkeydown: (event) => { if (event.key === 'Enter') { event.preventDefault(); addTask(); } } });
    const addTask = () => {
      const task = taskInput.value.trim().replace(/\s+/g, ' ');
      if (!task) return;
      if (!taken.has('task:' + task)) edit.lines.push({ task, label: task });
      redraw();
    };
    const lotSelect = searchPicker('＋ Ajouter un lot : rechercher…',
      (options ? options.lots : []).filter((lot) => !taken.has(lot.id))
        .map((lot) => ({ key: lot.id, label: lot.name, sub: lot.lot_type_name, row: lot })),
      (pick) => { edit.lines.push({ lot: pick.row.id, label: pick.row.name, sub: pick.row.lot_type_name }); redraw(); });
    const typeSelect = searchPicker('＋ Ajouter du matériel : rechercher…',
      (options ? options.types : []).filter((type) => !taken.has('type:' + type.type))
        .map((type) => ({ key: type.type, label: type.name, sub: type.type, row: type })),
      (pick) => { edit.lines.push({ type: pick.row.type, label: pick.row.name, quantity: 1 }); redraw(); });
    return [
      el('h3', {}, edit.id ? 'Modifier la liste' : edit.template ? 'Nouveau modèle' : 'Nouvelle liste'),
      el('label', { class: 'field' }, 'Nom',
        el('input', { type: 'text', maxlength: '80', value: edit.name, placeholder: 'Ex : poste de secours du stade',
          oninput: (event) => { edit.name = event.target.value; } })),
      el('label', { class: 'field' }, 'Remarque',
        el('textarea', { rows: '2', maxlength: '500', placeholder: 'Facultatif : rendez-vous, consignes…',
          oninput: (event) => { edit.note = event.target.value; } }, edit.note)),
      el('label', { class: 'check' }, el('input', { type: 'checkbox', checked: edit.template ? '' : null, disabled: edit.started ? '' : null,
        onchange: (event) => { edit.template = event.target.checked; } }),
      edit.started ? 'Modèle (impossible : liste déjà commencée)' : 'Modèle (se duplique, ne se vérifie pas)'),
      el('h3', {}, 'À prendre'),
      edit.lines.length ? '' : el('p', { class: 'hint' }, 'Ajoutez des lots (sacs, armoires), du matériel avec sa quantité et des tâches à cocher.'),
      el('ul', { class: 'list' }, ...edit.lines.map((line, index) => el('li', { class: 'static' },
        el('div', { class: 'main' }, el('div', { class: 'name' }, line.label),
          el('div', { class: 'sub' }, line.task ? 'Tâche à cocher' : line.lot ? `Lot · ${line.sub || ''}` : 'Matériel')),
        line.lot || line.task ? '' : el('input', { type: 'number', class: 'list-qty', min: '1', max: '999', value: String(line.quantity), 'aria-label': 'Quantité',
          oninput: (event) => { line.quantity = Math.max(1, Math.min(999, parseInt(event.target.value, 10) || 1)); } }),
        el('button', { type: 'button', class: 'list-remove', 'aria-label': 'Retirer', onclick: () => { edit.lines.splice(index, 1); redraw(); } }, '✕')))),
      options ? el('div', { class: 'list-add' }, lotSelect, typeSelect) : el('p', { class: 'hint' }, 'Chargement des lots et du matériel…'),
      el('div', { class: 'list-add-task' }, taskInput, el('button', { type: 'button', onclick: addTask }, '＋ Tâche')),
      el('p', { class: 'hint' }, 'Une tâche se coche pendant la vérif de la liste (ni lot ni matériel).'),
      el('div', { class: 'list-buttons' },
        el('button', { type: 'button', class: 'secondary', onclick: () => { lists.edit = null; renderListsPage(); } }, 'Annuler'),
        el('button', { type: 'button', class: 'primary', disabled: lists.busy ? '' : null, onclick: saveList }, 'Enregistrer')),
    ];
  }

  // ------------------------------------------------------------------------------------------------
  // Demarrage : l'URL peut venir d'un QR code (verif?lot=..&key=.., badge?m=..&key=.., item?id=.., pack?id=..)

  async function boot() {
    restore();
    render();
    // reglage du serveur : identite declaree (nom sans badge) acceptee ou non
    api('health/').then((health) => { state.declaredAllowed = !!health.declared_identity; render(); }).catch(() => {});
    const route = location.pathname.replace(/\/+$/, '').split('/').pop();
    const params = new URLSearchParams(location.search);
    // on retire la cle de la barre d'adresse (historique, partage d'ecran)
    const cleanUrl = (lot) => history.replaceState(null, '', lot ? `verif?lot=${encodeURIComponent(lot)}` : 'verif');
    // etiquette scannee avec l'appareil photo : ecran de scan (sans demarrer la camera, il faut un geste)
    const qrRoute = ['verif', 'seal', 'unseal', 'pinreset', 'item', 'task', 'pack'].includes(route) && [...params.keys()].length > 0;
    if (qrRoute) { document.body.classList.remove('home-mode'); state.homePage = 'scan'; }
    if (route === 'verif' && params.get('lot')) {
      // etiquette publique ou privee d'un lot : fiche du lot, « Lancer une vérif » ouvre le lecteur (la cle est
      // gardee pour cet onglet, retiree de l'adresse)
      const id = params.get('lot');
      cleanUrl(id);
      if (state.lotId) await loadLot(state.lotId); // vérif en cours (rejointe si c'est un lot du meme lot global)
      await openFiche('lot', id, { key: params.get('key') || '', push: false });
    } else if (route === 'badge' && params.get('m')) {
      // carte scannee : page d'accueil
      history.replaceState(null, '', './');
      showHome('menu', false);
      await login(params.get('m'), params.get('key') || '');
    } else if (route === 'seal' && params.get('lot') && params.get('s')) {
      // etiquette du scelle : fiche du lot scelle (sans vérif), ou scan « scellé intact » de la vérif en cours
      if (state.lotId) await loadLot(state.lotId);
      if (checkRunning() || allScanned().length) {
        await scanSeal({ kind: 'seal', code: location.href, id: params.get('lot'), key: params.get('s') });
        cleanUrl(params.get('lot'));
      } else {
        await openFiche('lot', params.get('lot'), { seal: params.get('s'), push: false });
      }
    } else if (route === 'unseal' && params.get('lot') && params.get('c')) {
      cleanUrl(params.get('lot'));
      await openSeal({ kind: 'sealopen', code: location.href, id: params.get('lot'), key: params.get('c') }, { fromUrl: true });
    } else if (route === 'pinreset' && params.get('m') && params.get('t')) {
      cleanUrl(state.lotId);
      if (state.lotId) await loadLot(state.lotId);
      await startPinReset(params.get('m'), params.get('t'));
    } else if ((route === 'item' || route === 'task') && params.get('id')) {
      // QR d'un item scanne avec l'appareil photo : fiche de l'item et de son dernier emplacement connu
      if (state.lotId) await loadLot(state.lotId);
      const scan = parseCode(params.get('id'));
      if (scan.kind === 'item') await openFiche('item', scan.id, { push: false });
      else { cleanUrl(state.lotId); feedback.bad(); showInfo('bad', 'Code non reconnu', params.get('id')); }
    } else if (route === 'pack' && params.get('id')) {
      cleanUrl(state.lotId);
      if (state.lotId) await loadLot(state.lotId);
      await scanPack({ kind: 'pack', code: location.href, id: params.get('id') });
    } else if (route === 'liste' && params.get('id')) {
      // lien ou QR code d'une liste de materiel : la liste, a verifier en direct (connexion demandee)
      openList(params.get('id'), false);
    } else if (route === 'verif') {
      // lecteur (adresse du lecteur ouvert depuis une fiche), rechargement compris
      enterScanner(false, false);
    } else {
      // page d'accueil, ou la page de son ancre (#stock : depuis une notification de stock, #scan : scanner)
      const page = pageFromHash(location.hash);
      if (page === 'scan') enterScanner(false, false);
      else showHome((page === 'stock' && !canSeeStock()) || (['reports', 'lists'].includes(page) && !state.user) ? 'menu' : page, false);
    }
    if (state.expiredSession && !state.lot) toast('Vérif précédente oubliée : plus de 20 minutes sans activité.');
    render();
    if (state.user && canSeeStock()) loadStock();
    if (isAdmin()) loadReports(true);
  }

  boot();
})();
