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

  const $ = (selector) => document.querySelector(selector);

  // ------------------------------------------------------------------------------------------------
  // Etat

  const state = {
    user: null,        // {matricule, key, nom, prenom, privileged}
    lot: null,         // detail public du lot (items attendus, exigences)
    lotId: '',
    lotKey: '',
    scanned: [],       // [{code, kind, iid, info, expired, error, items: [iids d'un paquet]}]
    tab: 'todo',
    busy: false,
    lastVerif: null,   // {lotId, at, complete, present} : derniere verif validee (affichee tant qu'on ne rescanne pas)
    lots: null,        // liste des lots (accueil), chargee avec le badge
    stock: null,       // etat des stocks, roles gestion et admin uniquement
    loading: '',       // 'lots' ou 'stock' pendant un chargement
    push: null,        // notifications web de ce navigateur (admins) : {subscribed, stock_low, stock_empty, ...}
    pushBusy: false,
    pushError: '',
  };

  // roles gestion et admin : acces en lecture a l'etat des stocks
  const canSeeStock = () => !!(state.user && (state.user.role === 'gestion' || state.user.role === 'admin' || state.user.privileged));
  const isAdmin = () => !!(state.user && state.user.role === 'admin');
  const ROLE_LABELS = { normal: 'Secouriste', gestion: 'Gestion', admin: 'Administrateur' };

  function save() {
    try {
      localStorage.setItem(STORAGE_SESSION, JSON.stringify({
        lotId: state.lotId, lotKey: state.lotKey, lastVerif: state.lastVerif,
        scanned: state.scanned.map(({ code, kind, iid, info, expired, error, items }) => ({ code, kind, iid, info, expired, error, items })),
      }));
      if (state.user) localStorage.setItem(STORAGE_USER, JSON.stringify(state.user));
      else localStorage.removeItem(STORAGE_USER);
    } catch (e) { /* stockage indisponible (navigation privee) : la page fonctionne sans */ }
  }

  function restore() {
    try {
      const user = JSON.parse(localStorage.getItem(STORAGE_USER) || 'null');
      if (user && user.key_expires && new Date(user.key_expires) >= today()) state.user = user;
      const session = JSON.parse(localStorage.getItem(STORAGE_SESSION) || 'null');
      if (session) {
        state.lotId = session.lotId || '';
        state.lotKey = session.lotKey || '';
        state.scanned = Array.isArray(session.scanned) ? session.scanned : [];
        state.lastVerif = session.lastVerif || null;
      }
    } catch (e) { /* ignore */ }
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
    if (IID_RE.test(code)) {
      const date = code.slice(6, 14);
      return { kind: 'item', code, id: code, type: code.slice(0, 6), peremption: date === '00000000' ? null : parseDate(date) };
    }
    let url;
    try { url = new URL(code); } catch (e) { return { kind: 'unknown', code }; }
    const route = url.pathname.replace(/\/+$/, '').split('/').pop();
    const p = url.searchParams;
    if (route === 'verif' && p.get('lot')) return { kind: 'lot', code, id: p.get('lot'), key: p.get('key') || '' };
    if (route === 'badge' && p.get('m')) return { kind: 'user', code, id: p.get('m'), key: p.get('key') || '' };
    if (route === 'pack' && p.get('id')) return { kind: 'pack', code, id: p.get('id') };
    if (route === 'seal' && p.get('lot') && p.get('s')) return { kind: 'seal', code, id: p.get('lot'), key: p.get('s') };
    if (route === 'scanner' && p.get('s') && p.get('k')) return { kind: 'remote', code, search: url.search };
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
  let scanning = false;
  let wakeLock = null;
  let lastCode = '';
  let lastCodeTime = 0;

  async function startCamera() {
    unlockAudio();
    $('#camera-error').textContent = '';
    if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) {
      $('#camera-error').textContent = "La caméra n'est accessible qu'en HTTPS. Utilisez « Saisir un code » en attendant.";
      return;
    }
    try {
      stream = await navigator.mediaDevices.getUserMedia({
        audio: false,
        video: { facingMode: { ideal: 'environment' }, width: { ideal: 1280 }, height: { ideal: 720 } },
      });
    } catch (e) {
      $('#camera-error').textContent = 'Caméra refusée ou indisponible : ' + e.message;
      return;
    }
    video.srcObject = stream;
    await video.play().catch(() => {});
    if ('BarcodeDetector' in window) {
      try {
        const formats = await window.BarcodeDetector.getSupportedFormats();
        if (formats.includes('qr_code')) detector = new window.BarcodeDetector({ formats: ['qr_code'] });
      } catch (e) { detector = null; }
    }
    const track = stream.getVideoTracks()[0];
    const capabilities = track.getCapabilities ? track.getCapabilities() : {};
    $('#torch').hidden = !capabilities.torch;
    $('#start').hidden = true;
    scanning = true;
    requestWakeLock();
    scanLoop();
  }

  function stopCamera() {
    scanning = false;
    if (stream) stream.getTracks().forEach((track) => track.stop());
    stream = null;
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

  async function scanLoop() {
    while (scanning) {
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

  document.addEventListener('visibilitychange', () => {
    if (document.hidden) stopCamera();
    else if (!$('#start').hidden) return;
    else startCamera();
  });

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
    const scan = parseCode(raw);
    switch (scan.kind) {
      case 'item': return scanItem(scan);
      case 'lot': return scanLot(scan);
      case 'user': return login(scan.id, scan.key);
      case 'pack': return scanPack(scan);
      case 'seal': return scanSeal(scan);
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
    const inLot = state.lot && info.location === state.lot.id;
    const where = info.location ? (inLot ? 'Dans ce lot' : 'Rangé dans : ' + info.location_name) : 'En stock';
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
    if (state.lotId && state.lotId !== scan.id && state.lot) {
      toast(`Lot changé : ${state.lot.name} → nouveau lot`);
    }
    if (state.lotId !== scan.id) { state.lotKey = ''; state.lastVerif = null; }
    if (scan.key) state.lotKey = scan.key;
    await loadLot(scan.id);
    if (!state.lot) return;
    feedback.info();
    showLotInfo();
    if (!state.scanned.length) switchTab('todo');
  }

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

  async function loadLot(id, seal) {
    try {
      state.lot = await api(`lots/${encodeURIComponent(id)}/` + (seal ? `?seal=${encodeURIComponent(seal)}` : ''));
      state.lotId = id;
    } catch (e) {
      feedback.bad();
      showInfo('bad', 'Lot introuvable', e.message);
      if (state.lotId === id) { state.lot = null; state.lotId = ''; state.lotKey = ''; }
    }
    save();
    render();
  }

  // Etat d'un lot : verifie et complet (vert), reassort depuis la derniere verif (orange : verif complete
  // recommandee), sinon incomplet ou jamais verifie (rouge). kind : 'ok', 'warn' ou 'bad'.
  function lotStatus(lot) {
    const make = (kind, label) => ({ ok: kind === 'ok', kind, label });
    if (lot.is_sealed && lot.expired_count) return make('bad', '✘ Scellé, contient des périmés');
    if (lot.is_sealed) return make('ok', '✔ Scellé' + (lot.valid_until ? `, valide jusqu'au ${fmtDate(lot.valid_until)}` : ''));
    if (!lot.last_verif) return make('bad', '✘ Jamais vérifié');
    if (!lot.complete) return make('bad', lot.expired_count ? '✘ Incomplet (périmés)' : '✘ Incomplet');
    if (lot.verif_recommended) return make('warn', '⚠ Vérif recommandée, réassort');
    return make('ok', '✔ Vérifié, complet');
  }

  function showLotInfo() {
    const lot = state.lot;
    if (!lot) return;
    const status = lotStatus(lot);
    showInfo(status.kind, `${status.label} – ${lot.name}`,
      `${lot.lot_type_name} · ${lot.item_count} item(s)` + (lot.expired_count ? ` · ${lot.expired_count} périmé(s)` : ''),
      'Dernière vérif : ' + (lot.last_verif ? fmtDateTime(lot.last_verif) + (lot.last_verif_by ? ' par ' + lot.last_verif_by : '') : 'jamais'),
      lot.verif_recommended ? `Réassort de ${lot.restocked_count} item(s) le ${fmtDateTime(lot.restocked)}${lot.restocked_by ? ' par ' + lot.restocked_by : ''} : faites une vérif complète.` : '',
      state.lotKey ? '🔑 Étiquette privée scannée' : 'Scannez l\'étiquette privée pour pouvoir valider');
  }

  // Saisie du PIN (ou choix du PIN pour un admin qui n'en a pas encore)
  function askPin(matricule, key, setup, errorMessage) {
    const dialog = $('#pin');
    $('#pin-title').textContent = setup ? 'Choisissez votre code PIN' : 'Code PIN';
    $('#pin-text').textContent = setup
      ? 'Obligatoire pour les administrateurs : 4 à 8 chiffres, demandé après le badge à chaque connexion.'
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
      feedback.info();
      showInfo('ok', `Bonjour ${user.prenom} ${user.nom}`,
        canSeeStock() ? `Rôle ${ROLE_LABELS[user.role] || 'gestion'} : l'état des stocks est dans l'onglet Stock.` : 'Vous êtes connecté.');
      save();
      render();
      loadLots();
      if (canSeeStock()) loadStock();
    } catch (e) {
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
  // Actions

  function blockers(needItems = true) {
    const missing = [];
    if (!state.lot) missing.push("scannez l'étiquette du lot");
    else if (!state.lotKey) missing.push("scannez l'étiquette privée du lot");
    if (!state.user) missing.push('scannez votre badge');
    if (needItems && !scannedIids().size) missing.push('scannez au moins un item');
    return missing;
  }

  async function validate() {
    const missing = blockers(false);
    if (missing.length) { feedback.warn(); toast('Pour valider : ' + missing.join(', ') + '.', true); return; }
    if (state.lot.is_sealed && !confirm('Ce lot est scellé : valider une vérif brisera le scellé. Continuer ?')) return;
    const expected = expectedGroups().remaining;
    if (expected && !confirm(`${expected} item(s) attendu(s) manquent : le lot sera incomplet. Valider quand même ?`)) return;
    state.busy = true;
    render();
    try {
      const report = await api(`lots/${encodeURIComponent(state.lot.id)}/verif/`, {
        key: state.lotKey,
        user: { matricule: state.user.matricule, key: state.user.key },
        items: [...scannedIids()],
      });
      state.scanned = [];
      state.lastVerif = { lotId: state.lot.id, at: new Date().toISOString(), complete: report.complete,
                          present: report.present.length };
      showReport(report);
      await loadLot(state.lot.id);
      showLotInfo();
      loadLots();
    } catch (e) {
      feedback.bad();
      toast('Vérif refusée : ' + e.message, true);
    } finally {
      state.busy = false;
      save();
      render();
    }
  }

  // Seulement des items qui ne sont pas dans le lot : reassort plutot que verif
  function onlyNewItems() {
    if (!state.lot || !state.scanned.length) return false;
    const known = new Set((state.lot.items || []).map((item) => item.iid));
    const iids = [...scannedIids()];
    return iids.length > 0 && !iids.some((iid) => known.has(iid));
  }

  async function addToLot() {
    const missing = blockers(true);
    if (missing.length) { feedback.warn(); toast('Pour ajouter : ' + missing.join(', ') + '.', true); return; }
    try {
      const result = await api(`lots/${encodeURIComponent(state.lot.id)}/add/`, {
        key: state.lotKey,
        user: { matricule: state.user.matricule, key: state.user.key },
        items: [...scannedIids()],
      });
      state.scanned = [];
      feedback.good();
      toast(`${result.moved.length} item(s) ajouté(s) au lot : vérif complète recommandée.`);
      await loadLot(state.lot.id);
      loadLots();
    } catch (e) {
      feedback.bad();
      toast('Ajout refusé : ' + e.message, true);
    }
    save();
    render();
  }

  function iidLabel(iid) {
    const known = (state.lot && state.lot.items || []).find((item) => item.iid === iid);
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
      el('h2', { style: report.complete ? 'color:var(--green)' : 'color:var(--red)' }, report.complete ? '✅ Lot complet' : '⚠️ Lot NON complet'),
      el('p', {}, `${report.present.length} item(s) présent(s).`),
      report.unsealed ? el('p', { style: 'color:var(--orange)' }, '🔓 Le scellé du lot a été brisé par cette vérif.') : '',
      ...(report.requirements || []).map((row) => requirementRow(row.type_name, row.present, row.required)),
      ...section('Périmés encore dans le lot : à remplacer', report.expired),
      ...section('Périmés remplacés', report.replaced),
      ...section('Attendus mais non scannés', report.missing),
      ...section('Retrouvés', report.reactivated),
      ...section('Codes inconnus ignorés', report.unknown),
    );
    if (report.complete) feedback.good(); else feedback.warn();
    $('#report').showModal();
  }

  $('#report-close').addEventListener('click', () => $('#report').close());
  $('#validate').addEventListener('click', validate);
  $('#restock').addEventListener('click', () => {
    if (confirm("Ajouter les items scannés au lot sans faire de vérif complète (réassort) ?\n"
      + 'Le lot sera signalé « vérif recommandée » pour la personne suivante.')) addToLot();
  });
  $('#more').addEventListener('click', () => $('#menu').showModal());
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
    if (action === 'forget-lot') {
      state.lot = null; state.lotId = ''; state.lotKey = '';
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

  // Attendus : la definition du type de lot (quantite par type), puis les items deja connus du lot
  function expectedGroups() {
    const done = scannedIids();
    const fresh = freshByType();
    const known = {};
    for (const item of (state.lot && state.lot.items) || []) {
      if (done.has(item.iid)) continue;
      (known[item.type] = known[item.type] || []).push(item);
    }
    let remaining = 0;
    const groups = (state.lot.requirements || []).map((row) => {
      const items = (known[row.type] || []).sort((a, b) => b.expired - a.expired);
      delete known[row.type];
      const scanned = fresh[row.type] || 0;
      const missing = Math.max(0, row.required - scanned);
      const knownFresh = items.filter((item) => !item.expired).length;
      remaining += missing;
      return { row, items, scanned, missing, fromStock: Math.max(0, missing - knownFresh) };
    });
    const others = Object.values(known).flat();
    remaining += others.length;
    return { groups, others, remaining };
  }

  function todoItem(item) {
    return el('li', { class: item.expired ? 'expired' : 'todo' },
      el('div', { class: 'main' },
        el('div', { class: 'name' }, item.type_name),
        el('div', { class: 'sub' }, `${item.peremption ? fmtDate(item.peremption) : 'Non périssable'} · ${item.iid}`)),
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
      list.replaceChildren(
        el('li', { class: 'group ' + (last.complete ? 'ok' : 'bad') },
          el('div', { class: 'main' },
            el('div', { class: 'name' }, `${last.complete ? '✔' : '✘'} Vérif enregistrée à ${at}`),
            el('div', { class: 'sub' }, `${last.present} item(s) présent(s) · lot ${last.complete ? 'complet' : 'incomplet'}`))),
        el('li', { class: 'empty' }, 'Scannez un item pour commencer une nouvelle vérif.'));
      return 0;
    }
    const { groups, others, remaining } = expectedGroups();
    const rows = [];
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
    if (!remaining) rows.unshift(el('li', { class: 'group ok' }, '✅ Tout est scanné : le lot sera complet.'));
    list.replaceChildren(...rows);
    return remaining;
  }

  function renderDone() {
    const list = $('#done-list');
    const expected = new Set((state.lot && state.lot.items || []).map((item) => item.iid));
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
    const fresh = freshByType();
    view.replaceChildren(
      el('h2', {}, lot.name),
      el('div', { class: 'sub' }, `${lot.lot_type_name} · ${lot.id}`),
      el('div', { class: 'banner ' + lotStatus(lot).kind }, lotStatus(lot).label),
      lot.is_sealed ? el('p', {}, `🔒 Scellé${lot.seal_number ? ' n°' + lot.seal_number : ''} le ${fmtDateTime(lot.sealed)}` +
        (lot.sealed_by ? ` par ${lot.sealed_by}` : '') + ' : pas de vérif nécessaire tant que le scellé est intact.') : '',
      el('p', {}, 'Dernière vérif : ' + (lot.last_verif ? `${fmtDateTime(lot.last_verif)} par ${lot.last_verif_by || '?'}` : 'jamais'),
        el('br'), state.lotKey ? '🔑 Étiquette privée scannée : la vérif peut être validée.' : '🔒 Scannez l\'étiquette privée pour valider.'),
      el('h3', {}, 'Scannés / attendus'),
      ...(lot.requirements || []).map((row) => requirementRow(row.type_name + (row.location ? ` (${row.location})` : ''),
        fresh[row.type] || 0, row.required)),
      el('h3', {}, 'État enregistré du lot'),
      ...(lot.requirements || []).map((row) => requirementRow(row.type_name, row.present, row.required)),
    );
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
    loadPush();
  }

  async function loadStock() {
    if (!canSeeStock()) return;
    state.loading = 'stock';
    render();
    try {
      state.stock = await api('stock/summary/', { user: badge() });
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
      parts.push(el('p', { class: 'hint' }, "Touchez un lot pour afficher ce qu'il faut scanner. Pour valider, scannez son étiquette privée."));
      parts.push(el('ul', { class: 'list' }, ...state.lots.map((lot) => {
        const status = lotStatus(lot);
        return el('li', { class: status.kind, onclick: () => chooseLot(lot.id) },
          el('div', { class: 'main' },
            el('div', { class: 'name' }, lot.name + (lot.id === state.lotId ? ' (en cours)' : '')),
            el('div', { class: 'sub' }, `${lot.lot_type_name} · vérif : ${lot.last_verif ? fmtDateTime(lot.last_verif) : 'jamais'}`)),
          el('span', { class: 'tag ' + { ok: 'green', warn: 'orange', bad: 'red' }[status.kind] },
            status.label.replace(/^[✔✘⚠] /, '').split(',')[0]));
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

  async function loadPush() {
    if (!isAdmin() || !pushSupported()) return;
    try {
      const subscription = await currentPushSubscription();
      const prefs = state.push || { stock_low: true, stock_empty: true };
      state.push = subscription
        ? { ...prefs, ...(await api('push/subscription/', { user: badge(), endpoint: subscription.endpoint })) }
        : { ...prefs, subscribed: false };
      // abonnement du navigateur inconnu du serveur (autre admin, base restauree) : a reactiver
      if (subscription && !state.push.subscribed) state.push.stale = true;
      state.pushError = '';
    } catch (e) {
      state.pushError = e.message;
    }
    renderStock();
  }

  async function enablePush() {
    const prefs = { stock_low: !!(state.push && state.push.stock_low), stock_empty: !!(state.push && state.push.stock_empty) };
    if (!prefs.stock_low && !prefs.stock_empty) { toast('Choisissez au moins une alerte.', true); return; }
    state.pushBusy = true; state.pushError = ''; renderStock();
    try {
      // demande de permission declenchee par le clic de l'utilisateur
      const permission = await Notification.requestPermission();
      if (permission !== 'granted') {
        throw new Error(permission === 'denied'
          ? 'Notifications bloquées pour ce site : autorisez-les dans les réglages du navigateur.'
          : 'Autorisation des notifications non accordée.');
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
    renderStock();
  }

  async function disablePush() {
    state.pushBusy = true; state.pushError = ''; renderStock();
    try {
      const subscription = await currentPushSubscription();
      if (subscription) {
        await api('push/unsubscribe/', { endpoint: subscription.endpoint });
        await subscription.unsubscribe();
      }
      state.push = { ...state.push, subscribed: false, stale: false };
      toast('Notifications désactivées sur cet appareil.');
    } catch (e) {
      state.pushError = e.message;
    }
    state.pushBusy = false;
    renderStock();
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

  // clic sur une notification alors que la page est deja ouverte
  if ('serviceWorker' in navigator) {
    navigator.serviceWorker.addEventListener('message', (event) => {
      if (event.data && event.data.tab === 'stock' && canSeeStock()) { switchTab('stock'); loadStock(); }
    });
    if (navigator.serviceWorker.startMessages) navigator.serviceWorker.startMessages();
  }

  function renderPush() {
    const parts = [el('h3', {}, 'Notifications de stock')];
    if (!pushSupported()) {
      parts.push(el('p', { class: 'hint' }, window.isSecureContext
        ? "Ce navigateur ne gère pas les notifications web. Sur iPhone, ajoutez d'abord la page à l'écran d'accueil (Partager > Sur l'écran d'accueil)."
        : 'Les notifications web exigent une connexion HTTPS.'));
      return el('div', { class: 'push-box' }, ...parts);
    }
    if (!state.push) {
      parts.push(el('p', { class: 'hint' }, 'Chargement…'));
      return el('div', { class: 'push-box' }, ...parts);
    }
    const push = state.push;
    const subscribed = push.subscribed && !push.stale;
    const option = (field, label) => el('label', { class: 'check' },
      el('input', { type: 'checkbox', checked: push[field] ? '' : null, disabled: state.pushBusy ? '' : null,
        onchange: (event) => { push[field] = event.target.checked; if (subscribed) enablePush(); else renderStock(); } }),
      label);
    parts.push(el('p', { class: 'hint' }, subscribed
      ? 'Activées sur cet appareil : vous serez prévenu même page fermée.'
      : 'Recevez une alerte sur cet appareil quand le stock passe sous son minimum ou arrive à zéro.'));
    parts.push(option('stock_low', 'Stock bas (sous le minimum fixé)'));
    parts.push(option('stock_empty', 'Stock vide (0 en stock)'));
    if (Notification.permission === 'denied') {
      parts.push(el('p', { class: 'error' }, 'Notifications bloquées pour ce site : autorisez-les dans les réglages du navigateur.'));
    }
    if (state.pushError) parts.push(el('p', { class: 'error' }, state.pushError));
    const buttons = subscribed
      ? [el('button', { type: 'button', onclick: testPush, disabled: state.pushBusy ? '' : null }, 'Envoyer un test'),
        el('button', { type: 'button', onclick: disablePush, disabled: state.pushBusy ? '' : null }, 'Désactiver')]
      : [el('button', { type: 'button', class: 'primary', onclick: enablePush, disabled: state.pushBusy ? '' : null },
        state.pushBusy ? 'Activation…' : 'Activer les notifications')];
    parts.push(el('div', { class: 'push-buttons' }, ...buttons));
    return el('div', { class: 'push-box' }, ...parts);
  }

  function renderStock() {
    const view = $('#stock-view');
    if (!canSeeStock()) {
      view.replaceChildren(el('p', {}, 'Réservé aux rôles gestion et admin : scannez votre badge.'));
      return;
    }
    const parts = [el('div', { class: 'row-head' }, el('h3', {}, 'État des stocks (lecture seule)'),
      el('button', { type: 'button', onclick: loadStock }, state.loading === 'stock' ? 'Chargement…' : 'Actualiser'))];
    if (isAdmin()) parts.push(renderPush());
    if (!state.stock) {
      parts.push(el('p', { class: 'hint' }, 'Chargement…'));
    } else {
      parts.push(el('p', { class: 'hint' }, 'Stock hors lots non périmé / minimum. Les plus critiques en premier.'));
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
    if (!state.user) { state.lots = null; state.stock = null; state.push = null; }
    renderHome();
    $('#tab-stock').hidden = !canSeeStock();
    if (state.tab === 'stock' && !canSeeStock()) switchTab('home');
    renderStock();
    const chip = $('#user-chip');
    chip.textContent = state.user ? `👤 ${state.user.prenom} ${state.user.nom}` : '👤 Non connecté';
    chip.classList.toggle('ok', !!state.user);
    // reassort : bouton orange a cote de la validation
    const restock = onlyNewItems() && !state.busy;
    $('#restock').hidden = !restock;
    $('#undo-last').hidden = !state.scanned.length || state.busy;
    $('#restock').textContent = `Ajouter ${scannedIids().size} au lot (réassort)`;
    const validateButton = $('#validate');
    const recorded = state.lastVerif && state.lot && state.lastVerif.lotId === state.lot.id && !state.scanned.length;
    validateButton.disabled = state.busy || recorded;
    validateButton.textContent = state.busy ? 'Envoi…' : recorded ? 'Vérif enregistrée ✔' : restock ? 'Vérif complète…'
      : blockers(false).length ? 'Valider la vérif…' : 'Valider la vérif';
    $('#scan-hint').textContent = !state.lot ? "Visez l'étiquette d'un lot ou un item"
      : !state.user ? 'Scannez votre badge pour pouvoir valider'
        : 'Scannez les items du lot';
  }

  // ------------------------------------------------------------------------------------------------
  // Demarrage : l'URL peut venir d'un QR code (verif?lot=..&key=.., badge?m=..&key=.., pack?id=..)

  async function boot() {
    restore();
    render();
    const route = location.pathname.replace(/\/+$/, '').split('/').pop();
    const params = new URLSearchParams(location.search);
    // on retire la cle de la barre d'adresse (historique, partage d'ecran)
    const cleanUrl = (lot) => history.replaceState(null, '', lot ? `verif?lot=${encodeURIComponent(lot)}` : 'verif');
    if (route === 'verif' && params.get('lot')) {
      const id = params.get('lot');
      if (id !== state.lotId) state.lotKey = '';
      if (params.get('key')) state.lotKey = params.get('key');
      cleanUrl(id);
      await loadLot(id);
      showLotInfo();
    } else if (route === 'badge' && params.get('m')) {
      cleanUrl(state.lotId);
      await login(params.get('m'), params.get('key') || '');
      if (state.lotId) await loadLot(state.lotId);
    } else if (route === 'seal' && params.get('lot') && params.get('s')) {
      await scanSeal({ kind: 'seal', code: location.href, id: params.get('lot'), key: params.get('s') });
      cleanUrl(params.get('lot'));
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
