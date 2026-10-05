/* QRProtec - front web mobile.
 *
 * Moitie haute : camera et detection des QR codes (BarcodeDetector natif si disponible, sinon jsQR).
 * Moitie basse : derniere information scannee et onglets "A scanner" / "Scannes" / "Lot".
 * Toutes les ecritures passent par l'API publique : la verif exige la cle du lot (etiquette privee)
 * et un badge utilisateur.
 */
'use strict';

(() => {
  const API = new URL('api/', document.baseURI);
  const IID_RE = /^[A-Za-z0-9]{6}\d{8}[A-Za-z0-9]{8}$/;
  const REPEAT_DELAY_MS = 2500; // un meme code vu en continu par la camera n'est traite qu'une fois
  const STORAGE_USER = 'qrprotec.user';
  const STORAGE_SESSION = 'qrprotec.session';
  const STORAGE_PIN_RESET = 'qrprotec.pin-reset';  // lien de deblocage en attente du badge de l'admin
  const STORAGE_NAME = 'qrprotec.declared-name';   // nom declare (sans badge), si le serveur l'autorise

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
    lots: null,        // liste des lots (accueil), chargee avec le badge
    showStorage: false, // rangements du stock dans la liste des lots (masques par defaut)
    stock: null,       // etat des stocks, roles gestion et admin uniquement
    forecast: null,    // previsions de stock (meme acces que l'etat des stocks)
    horizon: 1,        // index dans HORIZONS des previsions affichees
    stockFull: false,  // onglet Stock en plein ecran (camera masquee)
    pinReset: null,    // {matricule, token} : deblocage du PIN d'un utilisateur, en attente d'un admin connecte
    loading: '',       // 'lots' ou 'stock' pendant un chargement
    push: null,        // notifications web de ce navigateur (admins) : {subscribed, stock_low, stock_empty, ...}
    pushBusy: false,
    pushEndpoint: '', // abonnement de ce navigateur (pour le reperer dans la liste)
    pushDevices: null, // gestionnaire des notifications : {types, devices} de l'utilisateur connecte
    pushError: '',
    declaredAllowed: false, // reglage du serveur : verif et ajout possibles sans badge, sous un nom declare
    declaredName: '',       // nom declare sur ce navigateur (non verifie)
    pendingOpening: null,   // {id, key, at} : scelle ouvert par son etiquette sans identite, a signer a la connexion
  };

  // roles gestion et admin : acces en lecture a l'etat des stocks
  const canSeeStock = () => !!(state.user && (state.user.role === 'gestion' || state.user.role === 'admin' || state.user.privileged));
  const isAdmin = () => !!(state.user && state.user.role === 'admin');
  const ROLE_LABELS = { normal: 'Secouriste', gestion: 'Gestion', admin: 'Administrateur' };
  // identite des operations sur un lot : le badge, sinon le nom declare (si le serveur l'autorise)
  const declared = () => !state.user && state.declaredAllowed && !!state.declaredName;
  const hasIdentity = () => !!state.user || declared();
  const identity = () => (state.user ? { user: { matricule: state.user.matricule, key: state.user.key } }
    : { name: state.declaredName });

  function save() {
    try {
      localStorage.setItem(STORAGE_SESSION, JSON.stringify({
        lotId: state.lotId, lotKey: state.lotKey, lastVerif: state.lastVerif, pendingOpening: state.pendingOpening,
        extra: state.extra.map(({ id, key }) => ({ id, key })),
        scanned: state.scanned.map(({ code, kind, iid, info, expired, error, items }) => ({ code, kind, iid, info, expired, error, items })),
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
      if (user && user.key_expires && new Date(user.key_expires) >= today()) state.user = user;
      state.declaredName = localStorage.getItem(STORAGE_NAME) || '';
      const session = JSON.parse(localStorage.getItem(STORAGE_SESSION) || 'null');
      if (session) {
        state.lotId = session.lotId || '';
        state.lotKey = session.lotKey || '';
        state.scanned = Array.isArray(session.scanned) ? session.scanned : [];
        state.lastVerif = session.lastVerif || null;
        state.pendingOpening = session.pendingOpening || null;
        state.extra = Array.isArray(session.extra) ? session.extra.map(({ id, key }) => ({ id, key, lot: null })) : [];
      }
    } catch (e) { /* ignore */ }
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

  // Date et heure (dernieres verifs...) ; les dates seules sont reservees aux peremptions et expirations
  function fmtDateTime(value) {
    const d = value ? new Date(value) : null;
    if (!d || isNaN(d)) return '–';
    return d.toLocaleDateString('fr-FR') + ' à ' + d.toLocaleTimeString('fr-FR', { hour: '2-digit', minute: '2-digit' });
  }

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
    const itemScan = (iid) => {
      const date = iid.slice(6, 14);
      return { kind: 'item', code, id: iid, type: iid.slice(0, 6), peremption: date === '00000000' ? null : parseDate(date) };
    };
    if (IID_RE.test(code)) return itemScan(code);
    let url;
    try { url = new URL(code); } catch (e) { return { kind: 'unknown', code }; }
    const route = url.pathname.replace(/\/+$/, '').split('/').pop();
    const p = url.searchParams;
    if (route === 'item' && IID_RE.test(p.get('id') || '')) return itemScan(p.get('id'));
    if (route === 'verif' && p.get('lot')) return { kind: 'lot', code, id: p.get('lot'), key: p.get('key') || '' };
    if (route === 'badge' && p.get('m')) return { kind: 'user', code, id: p.get('m'), key: p.get('key') || '' };
    if (route === 'pack' && p.get('id')) return { kind: 'pack', code, id: p.get('id') };
    if (route === 'seal' && p.get('lot') && p.get('s')) return { kind: 'seal', code, id: p.get('lot'), key: p.get('s') };
    if (route === 'unseal' && p.get('lot') && p.get('c')) return { kind: 'sealopen', code, id: p.get('lot'), key: p.get('c') };
    if (route === 'scanner' && p.get('s') && p.get('k')) return { kind: 'remote', code, search: url.search };
    if (route === 'pinreset' && p.get('m') && p.get('t')) return { kind: 'pinreset', code, id: p.get('m'), key: p.get('t') };
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

  function vibrate(pattern) { if (navigator.vibrate) navigator.vibrate(pattern); }

  const feedback = {
    good() { tone(1320, 0, 0.08, 'sine', 0.2); vibrate(40); flash('good'); },
    info() { tone(990, 0, 0.06, 'sine', 0.15); vibrate(30); },
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

  // Jamais de camera ouverte quand la page est cachee, et une seule ouverture a la fois : sur Firefox
  // Android, un flux encore ouvert quand l'ecran s'eteint ou que l'onglet est decharge peut laisser la
  // camera « utilisee par Firefox » jusqu'au redemarrage du telephone.
  function cameraShouldRun() { return cameraWanted && !document.hidden; }

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
  $('#torch').addEventListener('click', async () => {
    const track = stream && stream.getVideoTracks()[0];
    if (!track) return;
    const on = !$('#torch').classList.contains('on');
    try {
      await track.applyConstraints({ advanced: [{ torch: on }] });
      $('#torch').classList.toggle('on', on);
    } catch (e) { toast('Lampe indisponible', true); }
  });

  // ------------------------------------------------------------------------------------------------
  // Traitement des scans

  function scannedIids() {
    const iids = new Set();
    for (const entry of state.scanned) {
      if (entry.error) continue;
      if (entry.kind === 'item') iids.add(entry.iid);
      for (const iid of entry.items || []) iids.add(iid);
    }
    return iids;
  }

  async function handleCode(raw) {
    unlockAudio();
    if (window.douchette && window.douchette.matches(raw)) { window.douchette.show(); return; }
    const scan = parseCode(raw);
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
    const existing = state.scanned.find((entry) => entry.kind === 'item' && entry.iid === scan.id);
    if (existing) {
      // doublon : ignore, sans son ni vibration
      showInfo('ok', '✓ Déjà scanné', (existing.info && existing.info.type_name) || scan.id);
      return;
    }
    state.lastVerif = null; // un nouveau scan commence une nouvelle verif
    const expired = !!(scan.peremption && scan.peremption < today());
    const entry = { code: scan.code, kind: 'item', iid: scan.id, expired, info: null, error: '' };
    state.scanned.push(entry);
    if (expired) feedback.bad(); else feedback.good();
    showInfo(expired ? 'bad' : 'ok', expired ? 'PÉRIMÉ' : 'Item scanné', scan.id,
      scan.peremption ? 'Péremption : ' + fmtDate(scan.peremption) : 'Non périssable');
    render();
    try {
      entry.info = await api(`items/${encodeURIComponent(scan.id)}/`);
      if (entry.info.expired && !entry.expired) { entry.expired = true; feedback.bad(); }
    } catch (e) {
      if (e.status === 404) { entry.error = 'Inconnu dans la base'; feedback.bad(); }
    }
    if (state.scanned[state.scanned.length - 1] === entry) showItemInfo(entry);
    save();
    render();
  }

  function showItemInfo(entry) {
    const info = entry.info;
    if (entry.error) { showInfo('bad', entry.error, entry.iid); return; }
    if (!info) return;
    const inLot = info.location && sessionLots().some((lot) => lot.id === info.location);
    const where = inLot ? 'Dans ' + (isMulti() ? 'le lot ' + info.location_name : 'ce lot')
      : info.in_stock ? 'En stock' + (info.location ? ' · ' + info.location_name : '')
        : info.location ? 'Rangé dans : ' + info.location_name : 'En stock';
    const status = { missing: 'Signalé disparu', deleted: 'Marqué supprimé', replaced: 'Déjà remplacé' }[info.status];
    showInfo(entry.expired ? 'bad' : status ? 'warn' : 'ok',
      (entry.expired ? 'PÉRIMÉ – ' : '') + info.type_name,
      info.peremption ? 'Péremption : ' + fmtDate(info.peremption) : 'Non périssable',
      where + (status ? ' · ' + status : ''),
      info.iid);
  }

  async function scanPack(scan) {
    try {
      const pack = await api(`packs/${encodeURIComponent(scan.id)}/`);
      const expired = !!(pack.peremption && parseDate(pack.peremption) < today());
      if (state.scanned.some((entry) => entry.kind === 'pack' && entry.code === scan.code)) {
        showInfo('ok', '✓ Déjà scanné', `Paquet : ${pack.count} × ${pack.type_name}`);
        return;
      }
      state.lastVerif = null;
      state.scanned.push({ code: scan.code, kind: 'pack', iid: '', info: pack, expired, error: '', items: pack.items });
      if (expired) feedback.bad(); else feedback.good();
      showInfo(expired ? 'bad' : 'ok', `Paquet : ${pack.count} × ${pack.type_name}`,
        pack.peremption ? 'Péremption : ' + fmtDate(pack.peremption) : 'Non périssable',
        pack.opened ? 'Paquet ouvert le ' + fmtDateTime(pack.opened) : 'Paquet fermé');
      save();
      render();
    } catch (e) {
      feedback.bad();
      showInfo('bad', 'Paquet inconnu', e.message);
    }
  }

  async function scanLot(scan) {
    if (state.lot && state.lotId !== scan.id && await joinVerif(scan)) return;
    if (state.lotId && state.lotId !== scan.id && state.lot) {
      toast(`Lot changé : ${state.lot.name} → nouveau lot`);
    }
    if (state.lotId !== scan.id) { state.lotKey = ''; state.lastVerif = null; state.extra = []; }
    if (scan.key) state.lotKey = scan.key;
    await loadLot(scan.id);
    if (!state.lot) return;
    feedback.info();
    showLotInfo();
    if (!state.scanned.length) switchTab('todo');
  }

  // Etiquette d'un autre lot du meme lot global pendant une verif : la verif continue et ses items attendus
  // s'ajoutent (etiquette privee). Renvoie true si le scan a ete traite ici.
  async function joinVerif(scan) {
    const inProgress = state.scanned.length > 0 || (!!state.lotKey && !state.lastVerif);
    if (!inProgress) return false;
    const covered = sessionLots().find((lot) => lot.id === scan.id);
    if (covered && !scan.key) {
      feedback.info();
      showInfo('ok', `${covered.name} fait partie de la vérif en cours`, 'Scannez ses items.');
      return true;
    }
    if (!scan.key) return false;
    let lot;
    try {
      lot = await api(`lots/${encodeURIComponent(scan.id)}/`);
    } catch (e) {
      feedback.bad();
      showInfo('bad', 'Lot introuvable', e.message);
      return true;
    }
    if (rootOf(lot) !== rootOf(state.lot)) return false;
    const existing = state.extra.find((entry) => entry.id === scan.id);
    if (existing) { existing.key = scan.key; existing.lot = lot; } else state.extra.push({ id: scan.id, key: scan.key, lot });
    feedback.info();
    showInfo('ok', `Vérif groupée : ${sessionLots().filter((sub) => !sub.depth || sub.id === lot.id).map((sub) => sub.name).join(' + ')}`,
      covered ? `${lot.name} faisait déjà partie de la vérif : son étiquette privée est enregistrée.`
        : `Les items attendus de ${lot.name} s'ajoutent à la vérif en cours.`,
      `🔑 Étiquette privée de ${lot.name} scannée`);
    save();
    render();
    if (!state.scanned.length) switchTab('todo');
    return true;
  }

  const rootOf = (lot) => (lot.global ? lot.global.id : lot.path && lot.path.length ? lot.path[0].id : lot.id);

  // QR code du scelle d'un lot : lot valide sans verif tant que le scelle est intact
  async function scanSeal(scan) {
    if (state.lotId !== scan.id) { state.lotKey = ''; state.lastVerif = null; }
    await loadLot(scan.id, scan.key);
    const lot = state.lot;
    if (!lot || state.lotId !== scan.id) return;
    const sealInfo = lot.is_sealed ? `Scellé${lot.seal_number ? ' n°' + lot.seal_number : ''} le ${fmtDateTime(lot.sealed)}` +
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
        lot.unsealed ? `Scellé brisé le ${fmtDateTime(lot.unsealed)}` + (lot.unsealed_by ? ' par ' + lot.unsealed_by : '') : '');
    }
    switchTab('lot');
  }

  // Etiquette d'ouverture, rangee dans le lot scelle : la scanner ouvre le scelle, connecte ou non (pas de
  // bouton « ouvrir »). Sans identite, l'ouverture est enregistree anonymement puis signee a la connexion.
  const OPENING_SIGN_DELAY = 60 * 60 * 1000; // meme delai que le serveur

  async function openSeal(scan) {
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
    if (state.lotId !== scan.id) { state.lotKey = ''; state.extra = []; state.lastVerif = null; }
    state.lot = lot;
    state.lotId = scan.id;
    state.declaredAllowed = !!result.declared_identity;
    state.pendingOpening = result.identified ? null : { id: scan.id, key: scan.key, at: Date.now() };
    const opened = lot.unsealed ? `Scellé${lot.seal_number ? ' n°' + lot.seal_number : ''} ouvert le ${fmtDateTime(lot.unsealed)}`
      + (lot.unsealed_by ? ' par ' + lot.unsealed_by : '') : '';
    const sign = result.identified ? '' : state.declaredAllowed
      ? "Scannez votre badge (ou indiquez votre nom) pour signer l'ouverture." : "Scannez votre badge pour signer l'ouverture.";
    if (result.result === 'opened') {
      feedback.warn();
      showInfo('warn', `🔓 Scellé ouvert – ${lot.name}`, 'Le lot devra être vérifié avant utilisation.', opened, sign);
    } else if (result.result === 'signed') {
      feedback.good();
      showInfo('ok', `🔓 Ouverture signée – ${lot.name}`, opened);
    } else {
      feedback.info();
      showInfo('warn', `🔓 Scellé déjà ouvert – ${lot.name}`, 'Le lot doit être vérifié avant utilisation.', opened);
    }
    save();
    switchTab('lot');
    render();
    // on demande quand meme a l'utilisateur de s'identifier : le nom declare tout de suite (si le serveur
    // l'autorise), sinon le badge, dont le scan signera l'ouverture
    if (state.pendingOpening && !state.user && state.declaredAllowed && askDeclaredName()) await signOpening();
  }

  async function signOpening() {
    const pending = state.pendingOpening;
    if (!pending) return;
    state.pendingOpening = null;
    save();
    if (Date.now() - pending.at > OPENING_SIGN_DELAY || !hasIdentity()) return;
    await openSeal({ kind: 'sealopen', id: pending.id, key: pending.key });
  }

  async function loadLot(id, seal) {
    try {
      state.lot = await api(`lots/${encodeURIComponent(id)}/` + (seal ? `?seal=${encodeURIComponent(seal)}` : ''));
      if (state.lotId !== id) state.extra = [];
      state.lotId = id;
    } catch (e) {
      feedback.bad();
      showInfo('bad', 'Lot introuvable', e.message);
      if (state.lotId === id) { state.lot = null; state.lotId = ''; state.lotKey = ''; state.extra = []; }
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

  // Lots envoyes au serveur avec leur cle ; la cle d'un lot couvre ses sous-lots
  function sessionEntries() {
    return [{ id: state.lotId, key: state.lotKey }, ...state.extra.map(({ id, key }) => ({ id, key }))];
  }

  function keyOk() {
    if (state.lotKey) return true;
    const parents = new Set(((state.lot && state.lot.path) || []).map((parent) => parent.id));
    return state.extra.some((entry) => entry.key && parents.has(entry.id));
  }

  // Repartition des items scannes entre les lots de la verif (meme regle que le serveur, services.assign_items) :
  // un item deja dans un des lots y reste, un nouvel item va dans le premier lot qui en attend encore.
  // complete : le lot serait complet avec ces scans (verif partielle possible).
  function planSession() {
    const lots = sessionLots();
    const locationOf = {};
    const holding = {};
    const done = scannedIids();
    for (const lot of lots) {
      for (const item of [...(lot.items || []), ...(lot.missing_items || [])]) {
        locationOf[item.iid] = lot.id;
        // un item a etiquette a dechirer non scanne a ete utilise : il ne retient pas le lot
        if (!done.has(item.iid) && !item.tear_off) holding[lot.id] = true;
      }
    }
    const required = {};
    for (const lot of lots) {
      required[lot.id] = {};
      for (const row of lot.requirements || []) required[lot.id][row.type] = row.required;
    }
    const deficit = Object.fromEntries(lots.map((lot) => [lot.id, { ...required[lot.id] }]));
    const scanned = [];
    for (const entry of state.scanned) {
      if (entry.error) continue;
      const type = entry.kind === 'item' ? entry.iid.slice(0, 6) : entry.info && entry.info.type;
      const iids = entry.kind === 'item' ? [entry.iid] : entry.items || [];
      for (const iid of iids) scanned.push({ iid, type, expired: !!entry.expired });
    }
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
      const candidates = lots.filter((lot) => item.type in required[lot.id]).map((lot) => lot.id);
      let choice = null;
      if (!item.expired) {
        choice = candidates.find((id) => deficit[id][item.type] > 0) || null;
        if (choice) deficit[choice][item.type] -= 1;
      }
      target[item.iid] = choice || candidates[0] || (lots[0] && lots[0].id);
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
      row.complete = filled && !expiredLeft && (row.touched > 0 || !holding[row.lot.id]);
    }
    return { lots: lots.map((lot) => plan[lot.id]), target };
  }

  // Etat d'un lot : verifie et complet (vert), reassort depuis la derniere verif (orange : verif complete
  // recommandee), sinon incomplet ou jamais verifie (rouge). kind : 'ok', 'warn' ou 'bad'.
  function lotStatus(lot) {
    const make = (kind, label) => ({ ok: kind === 'ok', kind, label });
    if (lot.is_sealed && lot.expired_count) return make('bad', '✘ Scellé, contient des périmés');
    if (lot.is_sealed) return make('ok', '✔ Scellé' + (lot.valid_until ? `, valide jusqu'au ${fmtDate(lot.valid_until)}` : ''));
    if (!lot.last_verif) return make('bad', '✘ Jamais vérifié');
    if (!lot.complete) return make('bad', lot.expired_count ? '✘ Incomplet (périmés)' : '✘ Incomplet');
    if (lot.verif_recommended) return make('warn', lot.restocked_count ? '⚠ Vérif recommandée, réassort' : '⚠ Vérif recommandée, scellé ouvert');
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
        + ((lot.descendants || []).length ? ` · ${lot.descendants.length} sous-lot(s) vérifiés avec lui` : ''),
      globalLine(lot),
      'Dernière vérif : ' + (lot.last_verif ? fmtDateTime(lot.last_verif) + (lot.last_verif_by ? ' par ' + lot.last_verif_by : '') : 'jamais'),
      !lot.verif_recommended ? '' : lot.restocked_count
        ? `Réassort de ${lot.restocked_count} item(s) le ${fmtDateTime(lot.restocked)}${lot.restocked_by ? ' par ' + lot.restocked_by : ''} : faites une vérif complète.`
        : `Scellé ouvert${lot.unsealed ? ' le ' + fmtDateTime(lot.unsealed) : ''}${lot.unsealed_by ? ' par ' + lot.unsealed_by : ''} : faites une vérif complète.`,
      keyOk() ? '🔑 Étiquette privée scannée' : 'Scannez l\'étiquette privée pour pouvoir valider');
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
      state.user = { ...user, key };
      state.lots = null;
      state.stock = null;
      state.push = null;
      state.pushDevices = null;
      feedback.info();
      showInfo('ok', `Bonjour ${user.prenom} ${user.nom}`,
        canSeeStock() ? `Rôle ${ROLE_LABELS[user.role] || 'gestion'} : l'état des stocks est dans l'onglet Stock.` : 'Vous êtes connecté.');
      save();
      render();
      loadLots();
      if (canSeeStock()) loadStock();
      if (state.pinReset) openPinReset();
      if (state.pendingOpening) signOpening();
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

  // ------------------------------------------------------------------------------------------------
  // PIN bloque apres trop d'essais : l'utilisateur envoie a un admin la photo de l'ecran (QR code du lien de
  // deblocage) ; l'admin scanne le lien, se connecte avec son badge et reinitialise le PIN. L'utilisateur
  // en choisit alors un nouveau a sa prochaine connexion.

  // Code oublie : le PIN est bloque comme apres trop d'essais, et l'ecran du lien de deblocage s'affiche
  async function forgotPin(matricule, key) {
    if (!confirm("Votre code PIN sera bloqué jusqu'à ce qu'un administrateur le réinitialise. Continuer ?")) return;
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
      + 'Un administrateur doit réinitialiser votre PIN, vous en choisirez un nouveau à la prochaine connexion.';
    $('#pin-blocked-notified').hidden = !reset.notified;
    $('#pin-blocked-contact').textContent = reset.contact || 'un administrateur';
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
        fact('Bloqué le', info.blocked_since ? fmtDateTime(info.blocked_since) : '-'),
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
    else if (!keyOk()) missing.push("scannez l'étiquette privée du lot");
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

  async function validate(partial = false) {
    if (!ensureIdentity()) return;
    const missing = blockers(false);
    if (missing.length) { feedback.warn(); toast('Pour valider : ' + missing.join(', ') + '.', true); return; }
    const lots = sessionLots();
    const plan = planSession();
    const verified = partial ? plan.lots.filter((row) => row.complete).map((row) => row.lot) : lots;
    const sealed = verified.filter((lot) => lot.is_sealed).map((lot) => lot.name);
    if (sealed.length && !confirm(`${sealed.join(', ')} : scellé, valider une vérif brisera le scellé. Continuer ?`)) return;
    if (partial) {
      const names = verified.filter((lot) => lot.items && (lot.requirements.length || lot.items.length)).map((lot) => lot.name);
      if (!confirm(`Vérif partielle : seuls ${names.join(', ') || 'les lots complets'} seront vérifiés.\n`
        + 'Les autres items scannés sont ajoutés à leur lot (réassort, vérif recommandée). Continuer ?')) return;
    } else {
      const expected = lots.reduce((sum, lot) => sum + expectedGroups(lot, plan).remaining, 0);
      const what = isMulti() ? 'les lots seront incomplets' : 'le lot sera incomplet';
      if (expected && !confirm(`${expected} item(s) attendu(s) manquent : ${what}. Valider quand même ?`)) return;
    }
    state.busy = true;
    render();
    try {
      const report = await api('verifs/', {
        lots: sessionEntries(),
        ...identity(),
        items: [...scannedIids()],
        partial,
      });
      state.scanned = [];
      state.lastVerif = { lotId: state.lot.id, at: new Date().toISOString(), complete: report.complete,
                          present: report.present.length, partial: report.partial,
                          lots: (report.lots || []).filter((row) => row.verified).length };
      showReport(report);
      await loadLot(state.lot.id);
      showLotInfo();
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

  // Seulement des items qui ne sont pas dans les lots de la verif : reassort plutot que verif
  function onlyNewItems() {
    if (!state.lot || !state.scanned.length) return false;
    const known = knownIids();
    const iids = [...scannedIids()];
    return iids.length > 0 && !iids.some((iid) => known.has(iid));
  }

  function knownIids() {
    return new Set(sessionLots().flatMap((lot) => [...(lot.items || []), ...(lot.missing_items || [])].map((item) => item.iid)));
  }

  // Verif partielle : des lots sont complets, mais pas tous (verif groupee seulement)
  function partialLots() {
    if (!isMulti() || !state.scanned.length) return [];
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
    const entry = state.scanned.find((e) => e.iid === iid && e.info);
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
      report.unsealed ? el('p', { style: 'color:var(--orange)' },
        `🔓 Scellé brisé par cette vérif : ${(report.unsealed_lots || []).join(', ') || 'lot'}.`) : '',
      ...((report.lots || []).length > 1 ? [el('h3', {}, report.partial ? 'Vérif partielle' : 'Lots vérifiés'),
        el('ul', {}, report.lots.map((row) => el('li', {}, '  '.repeat(row.depth || 0)
          + (row.verified ? (row.complete ? '✔ ' : '✘ ') + row.name + (row.complete ? ' : complet' : ' : incomplet')
            : `↷ ${row.name} : non vérifié` + (row.restocked ? `, ${row.restocked} item(s) ajouté(s) (réassort)` : '')))))] : []),
      ...(report.requirements || []).map((row) => requirementRow(
        (row.lot_name ? row.lot_name + ' · ' : '') + row.type_name, row.present, row.required)),
      ...section('Périmés encore dans le lot : à remplacer', report.expired),
      ...section('Périmés remplacés', report.replaced),
      // etiquette a dechirer absente : l'ensemble a ete entame, il compte comme utilise
      ...section('Étiquette déchirée : utilisés', report.torn),
      ...section('Attendus mais non scannés', report.missing.filter((iid) => !(report.torn || []).includes(iid))),
      ...section('Retrouvés', report.reactivated),
      ...section('Ajoutés en réassort', report.restocked),
      ...section('Codes inconnus ignorés', report.unknown),
    );
    if (report.complete) feedback.good(); else feedback.warn();
    $('#report').showModal();
  }

  $('#report-close').addEventListener('click', () => $('#report').close());
  $('#validate').addEventListener('click', () => validate(false));
  $('#restock').addEventListener('click', () => {
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
    if (action === 'forget-lot') {
      state.lot = null; state.lotId = ''; state.lotKey = ''; state.extra = [];
      showInfo('empty', "Scannez l'étiquette d'un lot.");
      save(); render();
    }
    if (action === 'logout') {
      state.user = null;
      toast('Déconnecté.');
      save(); render();
    }
  });

  $('#user-chip').addEventListener('click', () => {
    if (state.user && confirm(`Déconnecter ${state.user.prenom} ${state.user.nom} ?`)) {
      state.user = null;
      save(); render();
    } else if (declared()) {
      if (confirm(`Oublier le nom « ${state.declaredName} » ? Scannez votre badge pour vous identifier.`)) {
        state.declaredName = '';
        save(); render();
      }
    } else if (!state.user && state.declaredAllowed) {
      if (!askDeclaredName()) toast('Scannez votre badge pour vous connecter, ou indiquez votre nom.');
    } else if (!state.user) {
      toast('Scannez votre badge pour vous connecter.');
    }
  });

  function undoLastScan() {
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
    state.scanned = [];
    save(); render();
  });

  function switchTab(tab) {
    state.tab = tab;
    if (tab !== 'stock') setStockFull(false);
    if (tab === 'stock' && !state.stock && state.loading !== 'stock') loadStock();
    document.querySelectorAll('#tabs button').forEach((b) => b.classList.toggle('active', b.dataset.tab === tab));
    document.querySelectorAll('.view').forEach((v) => v.classList.toggle('active', v.dataset.view === tab));
  }
  document.querySelectorAll('#tabs button').forEach((b) => b.addEventListener('click', () => switchTab(b.dataset.tab)));

  // ------------------------------------------------------------------------------------------------
  // Affichage

  function requirementRow(label, present, required) {
    const ratio = required > 0 ? Math.min(1, present / required) : 1;
    const color = present <= 0 ? 'var(--red)' : present < required ? 'var(--orange)' : 'var(--green)';
    return el('div', { class: 'req' },
      el('span', { class: 'label' }, label),
      el('div', { class: 'bar' },
        el('i', { style: `width:${present <= 0 ? 100 : ratio * 100}%;background:${color}` }),
        el('span', { style: present <= 0 ? 'color:#fff' : '' }, `${present}/${required}`)));
  }

  // Items frais scannes par type (un paquet compte pour son nombre d'items)
  function freshByType() {
    const fresh = {};
    for (const entry of state.scanned) {
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
    const done = scannedIids();
    const row = plan.lots.find((entry) => entry.lot.id === lot.id);
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

  function todoItem(item) {
    return el('li', { class: item.expired ? 'expired' : 'todo' },
      el('div', { class: 'main' },
        el('div', { class: 'name' }, item.type_name),
        el('div', { class: 'sub' }, `${item.peremption ? fmtDate(item.peremption) : 'Non périssable'} · ${item.iid}`
          + (item.tear_off ? ' · étiquette à déchirer : si elle manque, compté comme utilisé' : ''))),
      item.expired ? el('span', { class: 'tag red' }, 'PÉRIMÉ') : null,
      item.missed_verifs > 0 ? el('span', { class: 'tag orange' }, 'non vu') : null);
  }

  function renderTodo() {
    const list = $('#todo-list');
    if (!state.lot) {
      list.replaceChildren(el('li', { class: 'empty' }, "Scannez l'étiquette d'un lot pour afficher son contenu."));
      return 0;
    }
    const last = state.lastVerif;
    if (last && last.lotId === state.lot.id && !state.scanned.length) {
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
    let remaining = 0;
    for (const lot of lots) {
      const { groups, others, remaining: left, complete } = expectedGroups(lot, plan);
      remaining += left;
      // verif groupee : un titre par lot (un lot qui ne contient rien et n'attend rien n'est qu'un regroupement)
      if (multi) {
        if (!groups.length && !others.length && !(lot.items || []).length) continue;
        rows.push(el('li', { class: 'lot-head ' + (complete ? 'ok' : left ? 'bad' : 'partial'), style: `margin-left:${(lot.depth || 0) * 12}px` },
          el('div', { class: 'main' },
            el('div', { class: 'name' }, '📦 ' + lot.name),
            el('div', { class: 'sub' }, complete ? 'complet avec les scans' : `encore ${left} à scanner`))));
      }
      for (const group of groups) {
        const state_ = group.missing === 0 ? 'ok' : group.scanned === 0 ? 'bad' : 'partial';
        rows.push(el('li', { class: 'group ' + state_ },
          el('div', { class: 'main' },
            el('div', { class: 'name' }, group.row.type_name),
            el('div', { class: 'sub' }, [group.row.location ? '📍 ' + group.row.location : '',
              group.missing === 0 ? 'complet' : `encore ${group.missing} à scanner`].filter(Boolean).join(' · '))),
          el('span', { class: 'count' }, `${group.scanned}/${group.row.required}`)));
        rows.push(...group.items.map(todoItem));
        if (group.fromStock > 0) {
          rows.push(el('li', { class: 'todo stock' },
            el('div', { class: 'main' }, el('div', { class: 'name' }, `+ ${group.fromStock} à prendre dans le stock`))));
        }
      }
      if (others.length) {
        rows.push(el('li', { class: 'group other' }, el('div', { class: 'main' }, 'Autres items du lot (hors définition)')));
        rows.push(...others.map(todoItem));
      }
    }
    if (!remaining) rows.unshift(el('li', { class: 'group ok' }, multi ? '✅ Tout est scanné : les lots seront complets.' : '✅ Tout est scanné : le lot sera complet.'));
    list.replaceChildren(...rows);
    return remaining;
  }

  function renderDone() {
    const list = $('#done-list');
    const expected = knownIids();
    const rows = state.scanned.map((entry, index) => {
      const info = entry.info;
      let name = entry.kind === 'pack' ? `Paquet : ${info.count} × ${info.type_name}` : info ? info.type_name : entry.iid;
      let cls = 'ok';
      const tags = [];
      if (entry.error) { cls = 'error'; tags.push(el('span', { class: 'tag red' }, 'INCONNU')); }
      else if (entry.expired) { cls = 'expired'; tags.push(el('span', { class: 'tag red' }, 'PÉRIMÉ')); }
      else if (state.lot && entry.kind === 'item' && !expected.has(entry.iid)) {
        cls = 'extra'; tags.push(el('span', { class: 'tag blue' }, 'nouveau'));
      }
      const peremption = info ? info.peremption : null;
      return el('li', { class: cls },
        el('div', { class: 'main' },
          el('div', { class: 'name' }, name),
          el('div', { class: 'sub' }, [peremption ? fmtDate(peremption) : info && entry.kind === 'item' ? 'Non périssable' : '', entry.iid].filter(Boolean).join(' · '))),
        ...tags,
        el('button', {
          class: 'remove', type: 'button', 'aria-label': 'Retirer',
          onclick: () => { state.scanned.splice(index, 1); save(); render(); },
        }, '✕'));
    }).reverse(); // dernier scan en haut
    list.replaceChildren(...(rows.length ? rows : [el('li', { class: 'empty' }, 'Aucun item scanné.')]));
    return state.scanned.length;
  }

  function renderLot() {
    const view = $('#lot-view');
    const lot = state.lot;
    if (!lot) {
      view.replaceChildren(el('p', {}, "Aucun lot sélectionné. Scannez l'étiquette publique ou privée d'un lot, ou saisissez son identifiant (menu ⋯)."));
      return;
    }
    const plan = planSession();
    const fresh = (plan.lots.find((row) => row.lot.id === lot.id) || { fresh: {} }).fresh;
    const lots = sessionLots();
    view.replaceChildren(
      el('h2', {}, lot.name),
      el('div', { class: 'sub' }, `${lot.lot_type_name} · ${lot.id}`),
      (lot.path || []).length ? el('div', { class: 'sub' }, 'Dans : ' + lot.path.map((parent) => parent.name).join(' › ')) : '',
      el('div', { class: 'banner ' + (groupStatus(lot) || lotStatus(lot)).kind }, (groupStatus(lot) || lotStatus(lot)).label),
      lot.is_sealed ? el('p', {}, `🔒 Scellé${lot.seal_number ? ' n°' + lot.seal_number : ''} le ${fmtDateTime(lot.sealed)}` +
        (lot.sealed_by ? ` par ${lot.sealed_by}` : '') + ' : pas de vérif nécessaire tant que le scellé est intact.') : '',
      el('p', {}, 'Dernière vérif : ' + (lot.last_verif ? `${fmtDateTime(lot.last_verif)} par ${lot.last_verif_by || '?'}` : 'jamais'),
        el('br'), keyOk() ? '🔑 Étiquette privée scannée : la vérif peut être validée.' : '🔒 Scannez l\'étiquette privée pour valider.'),
      el('p', {}, sheetLink(lot.id, (lot.descendants || []).length ? 'Fiche du lot en PDF (une page par sous-lot)' : 'Fiche du lot en PDF')),
      lots.length > 1 ? el('p', {}, `Vérif groupée : ${lots.filter((sub) => (sub.requirements || []).length || (sub.items || []).length).map((sub) => sub.name).join(', ')}. `
        + "Scannez l'étiquette privée d'un autre lot du même lot global pour l'ajouter.") : '',
      ...renderGlobal(lot),
      el('h3', {}, 'Scannés / attendus'),
      ...(lot.requirements || []).map((row) => requirementRow(row.type_name + (row.location ? ` (${row.location})` : ''),
        fresh[row.type] || 0, row.required)),
      el('h3', {}, 'État enregistré du lot'),
      ...(lot.requirements || []).map((row) => requirementRow(row.type_name, row.present, row.required)),
    );
  }

  // Fiche d'inventaire papier (PDF A4) : items attendus par emplacement, une page par sous-lot
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
        style: `padding-left:${12 + row.depth * 16}px`,
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
  // Accueil : connexion, liste des lots pour lancer une verif, telephone-douchette ; onglet Stock

  const badge = () => ({ matricule: state.user.matricule, key: state.user.key, session: state.user.session });

  async function loadLots() {
    if (!state.user) return;
    state.loading = 'lots';
    render();
    try {
      state.lots = await api('lots/summary/', { user: badge() });
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

  async function chooseLot(id) {
    if (state.lotId !== id) {
      if (state.scanned.length && !confirm('Changer de lot ? Les items déjà scannés restent dans la liste.')) return;
      state.lotKey = '';
      state.lastVerif = null;
      state.extra = [];
    }
    await loadLot(id);
    if (!state.lot) return;
    showLotInfo();
    switchTab('todo');
  }

  function renderHome() {
    const view = $('#home-view');
    const user = state.user;
    const parts = [];
    parts.push(el('h3', {}, 'Connexion'));
    if (user) {
      parts.push(el('p', {}, `👤 ${user.prenom} ${user.nom} · ${ROLE_LABELS[user.role] || (user.privileged ? 'Gestion' : 'Secouriste')}`));
    } else {
      parts.push(el('p', {}, 'Scannez votre badge avec la caméra pour afficher la liste des lots.'));
    }

    const head = el('div', { class: 'row-head' }, el('h3', {}, 'Commencer une vérif'),
      user ? el('button', { type: 'button', onclick: loadLots }, state.loading === 'lots' ? 'Chargement…' : 'Actualiser') : '');
    parts.push(head);
    if (!user) {
      parts.push(el('p', { class: 'hint' }, "Sans badge : scannez directement l'étiquette du lot."));
    } else if (!state.lots) {
      parts.push(el('p', { class: 'hint' }, state.loading === 'lots' ? 'Chargement des lots…' : 'Liste non chargée.'));
    } else {
      parts.push(el('p', { class: 'hint' }, "Touchez un lot pour afficher ce qu'il faut scanner (un lot global se vérifie avec ses sous-lots). Pour valider, scannez son étiquette privée."));
      const tags = { ok: 'green', warn: 'orange', bad: 'red' };
      parts.push(el('label', { class: 'check' },
        el('input', { type: 'checkbox', checked: state.showStorage ? '' : null,
          onchange: (event) => { state.showStorage = event.target.checked; render(); } }),
        'Afficher les rangements du stock'));
      // rangements du stock masques par defaut (sauf le lot en cours)
      const lots = state.lots.filter((lot) => state.showStorage || !lot.storage || lot.id === state.lotId);
      parts.push(el('ul', { class: 'list' }, ...lots.map((lot) => {
        // lot global : son etat est celui de l'ensemble de ses lots
        const top = !lot.depth && lot.global;
        const status = top ? { kind: lot.global.kind, label: lot.global.label } : lotStatus(lot);
        const verif = top ? lot.global.last_verif : lot.last_verif;
        return el('li', { class: status.kind, style: lot.depth ? `padding-left:${12 + lot.depth * 16}px` : null,
          onclick: () => chooseLot(lot.id) },
          el('div', { class: 'main' },
            el('div', { class: 'name' }, (lot.depth ? '└ ' : '') + lot.name + (lot.id === state.lotId ? ' (en cours)' : '')),
            el('div', { class: 'sub' }, `${lot.lot_type_name}${top ? ' · lot global' : ''} · vérif${top ? ' la plus ancienne' : ''} : `
              + (verif ? fmtDateTime(verif) : 'jamais'))),
          el('span', { class: 'tag ' + tags[status.kind] }, status.label.replace(/^[✔✘⚠] /, '').split(',')[0]));
      })));
    }

    parts.push(el('h3', {}, 'Douchette du poste'));
    parts.push(el('p', {}, 'Ce téléphone peut servir de douchette au poste : sur le poste, menu Douchette > Téléphone comme douchette > '
      + '« Créer une session », puis scannez le QR code affiché avec cette caméra.'));

    if (canSeeStock()) {
      parts.push(el('h3', {}, 'État des stocks'));
      parts.push(el('button', { type: 'button', onclick: () => { switchTab('stock'); loadStock(); } }, 'Voir l\'état des stocks'));
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
      if (event.data && event.data.tab === 'stock' && canSeeStock()) { switchTab('stock'); loadStock(); }
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
        const sent = device.last_sent ? `dernier envoi le ${fmtDateTime(device.last_sent)} (${device.last_status})` : 'aucun envoi';
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

  // Plein ecran de l'onglet Stock : la camera est masquee, le panneau prend toute la hauteur
  function setStockFull(full) {
    if (state.stockFull === full) return;
    state.stockFull = full;
    document.body.classList.toggle('stock-full', full);
    renderStock();
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

  // Vue reduite des previsions (plein ecran) : horizon, graphe, types a surveiller, echanges
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

  function renderStock() {
    const view = $('#stock-view');
    if (!canSeeStock()) {
      view.replaceChildren(el('p', {}, 'Réservé aux rôles gestion et admin : scannez votre badge.'));
      return;
    }
    const head = el('div', { class: 'row-head' },
      el('h3', {}, state.stockFull ? 'Stock à venir' : 'État des stocks (lecture seule)'),
      el('div', { class: 'buttons' },
        el('button', { type: 'button', onclick: loadStock }, state.loading === 'stock' ? 'Chargement…' : 'Actualiser'),
        el('button', { type: 'button', onclick: () => setStockFull(!state.stockFull) },
          state.stockFull ? '⤡ Scanner' : '⤢ Prévisions')));
    const parts = [head];
    if (state.stockFull) {
      renderForecast(parts);
      view.replaceChildren(...parts);
      return;
    }
    parts.push(el('p', { class: 'hint' }, 'Alertes de stock sur votre téléphone : ',
      el('button', { type: 'button', class: 'text-link', onclick: openNotifications }, 'Notifications')));
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
        parts.push(el('div', { class: 'stock-row' },
          requirementRow(row.name, row.stock_fresh, row.min_quantity),
          el('div', { class: 'sub hint' }, details.join(' · '))));
      }
    }
    view.replaceChildren(...parts);
  }

  function render() {
    $('#count-todo').textContent = renderTodo();
    $('#count-done').textContent = renderDone();
    renderLot();
    if (!state.user) { state.lots = null; state.stock = null; state.push = null; state.pushDevices = null; }
    renderHome();
    $('#tab-stock').hidden = !canSeeStock();
    $('#menu-notifications').hidden = !canSeeStock();
    if (state.tab === 'stock' && !canSeeStock()) switchTab('home');
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
    const recorded = state.lastVerif && state.lot && state.lastVerif.lotId === state.lot.id && !state.scanned.length;
    validateButton.disabled = state.busy || recorded;
    validateButton.textContent = state.busy ? 'Envoi…' : recorded ? 'Vérif enregistrée ✔' : restock ? 'Vérif complète…'
      : blockers(false).length ? 'Valider la vérif…' : 'Valider la vérif';
    $('#scan-hint').textContent = !state.lot ? "Visez l'étiquette d'un lot ou un item"
      : !hasIdentity() ? (state.declaredAllowed ? 'Scannez votre badge (ou touchez « Non connecté ») pour pouvoir valider'
        : 'Scannez votre badge pour pouvoir valider')
        : 'Scannez les items du lot';
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
    if (route === 'verif' && params.get('lot')) {
      const id = params.get('lot');
      const key = params.get('key') || '';
      // etiquette d'un autre lot du meme lot global pendant une verif : elle rejoint la verif en cours
      let joined = false;
      if (state.lotId && id !== state.lotId) {
        await loadLot(state.lotId);
        joined = !!state.lot && await joinVerif({ kind: 'lot', code: location.href, id, key });
      }
      if (joined) {
        cleanUrl(state.lotId);
      } else {
        if (id !== state.lotId) { state.lotKey = ''; state.extra = []; state.lastVerif = null; }
        if (key) state.lotKey = key;
        cleanUrl(id);
        await loadLot(id);
        showLotInfo();
      }
    } else if (route === 'badge' && params.get('m')) {
      cleanUrl(state.lotId);
      await login(params.get('m'), params.get('key') || '');
      if (state.lotId) await loadLot(state.lotId);
    } else if (route === 'seal' && params.get('lot') && params.get('s')) {
      await scanSeal({ kind: 'seal', code: location.href, id: params.get('lot'), key: params.get('s') });
      cleanUrl(params.get('lot'));
    } else if (route === 'unseal' && params.get('lot') && params.get('c')) {
      cleanUrl(params.get('lot'));
      await openSeal({ kind: 'sealopen', code: location.href, id: params.get('lot'), key: params.get('c') });
    } else if (route === 'pinreset' && params.get('m') && params.get('t')) {
      cleanUrl(state.lotId);
      if (state.lotId) await loadLot(state.lotId);
      await startPinReset(params.get('m'), params.get('t'));
    } else if (route === 'item' && params.get('id')) {
      // QR d'un item scanne avec l'appareil photo : meme effet qu'un scan dans la page
      cleanUrl(state.lotId);
      if (state.lotId) await loadLot(state.lotId);
      const scan = parseCode(params.get('id'));
      if (scan.kind === 'item') await scanItem(scan);
      else { feedback.bad(); showInfo('bad', 'Code non reconnu', params.get('id')); }
    } else if (route === 'pack' && params.get('id')) {
      cleanUrl(state.lotId);
      if (state.lotId) await loadLot(state.lotId);
      await scanPack({ kind: 'pack', code: location.href, id: params.get('id') });
    } else if (state.lotId) {
      await loadLot(state.lotId);
      showLotInfo();
    }
    // page d'accueil quand aucun lot n'est en cours ; #stock : ouverture depuis une notification de stock
    if (location.hash === '#stock' && canSeeStock()) switchTab('stock');
    else if (!state.lot) switchTab('home');
    render();
    if (state.user) {
      loadLots();
      if (canSeeStock()) loadStock();
    }
  }

  boot();
})();
