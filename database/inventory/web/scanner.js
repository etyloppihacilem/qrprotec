/* QRProtec - telephone utilise comme douchette.
 *
 * Page ouverte par le QR code affiche sur le poste (scanner?s=SESSION&k=CLE). Chaque code lu par la
 * camera est envoye par WebSocket au serveur, qui le relaie au poste : le poste le traite comme un scan
 * de douchette et peut renvoyer un signal d'erreur (produit perime, code inconnu), restitue ici par un
 * flash rouge, un bip et une vibration. La connexion est retablie automatiquement ; si le telephone
 * reste deconnecte trop longtemps, le serveur ferme la session et il faut scanner un nouveau QR code.
 */
'use strict';

(() => {
  const REPEAT_DELAY_MS = 2500; // un meme code vu en continu par la camera n'est envoye qu'une fois
  const PING_MS = 15000;
  const $ = (selector) => document.querySelector(selector);
  const params = new URLSearchParams(location.search);
  let sessionId = params.get('s') || '';
  let sessionKey = params.get('k') || '';

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
    void node.offsetWidth;
    node.className = kind;
  }
  function vibrate(pattern) { if (navigator.vibrate) navigator.vibrate(pattern); }
  const feedback = {
    sent() { tone(1320, 0, 0.07, 'sine', 0.2); vibrate(40); flash('good'); },
    bad() { tone(880, 0, 0.18); tone(440, 0.22, 0.3); vibrate([250, 100, 250, 100, 250]); flash('bad'); },
  };
  let toastTimer = 0;
  function toast(message, bad = false) {
    const node = $('#toast');
    node.textContent = message;
    node.className = 'show' + (bad ? ' bad' : '');
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => { node.className = ''; }, bad ? 4000 : 2500);
  }
  function el(tag, attrs = {}, ...children) {
    const node = document.createElement(tag);
    for (const [key, value] of Object.entries(attrs)) {
      if (key === 'class') node.className = value;
      else node.setAttribute(key, value);
    }
    for (const child of children) if (child != null) node.append(child);
    return node;
  }
  function info(kind, title, line) {
    const node = $('#info');
    node.className = 'info ' + kind;
    node.replaceChildren(el('div', { class: 'title' }, title), line ? el('div', {}, line) : '');
  }
  // Libelle court d'un code scanne : jamais de cle (badge, etiquette privee, session), pas d'URL complete.
  const IID_RE = /^[A-Za-z0-9]{6}\d{8}[A-Za-z0-9]{8}$/;
  function describe(code) {
    if (IID_RE.test(code)) {
      const date = code.slice(6, 14);
      return `Item ${code.slice(0, 6)} · ` + (date === '00000000' ? 'sans date' : `${date.slice(6, 8)}/${date.slice(4, 6)}/${date.slice(0, 4)}`);
    }
    let url;
    try { url = new URL(code); } catch (e) { return code.length > 24 ? code.slice(0, 24) + '…' : code; }
    const route = url.pathname.replace(/\/+$/, '').split('/').pop();
    const p = url.searchParams;
    if (route === 'verif' && p.get('lot')) return `Lot ${p.get('lot')}` + (p.get('key') ? ' · étiquette privée' : '');
    if (route === 'badge' && p.get('m')) return `Badge ${p.get('m')}`;
    if (route === 'pack' && p.get('id')) return `Paquet ${p.get('id')}`;
    if (route === 'seal' && p.get('lot')) return `Scellé du lot ${p.get('lot')}`;
    if (route === 'scanner') return 'QR de session douchette';
    return `Lien ${url.hostname}`;
  }
  // les messages du poste peuvent citer le code : on remplace les URLs par leur libelle
  function redact(text) { return String(text || '').replace(/https?:\/\/\S+/g, (url) => describe(url)); }

  // ------------------------------------------------------------------------------------------------
  // Historique des codes envoyes

  const sent = []; // {id, label, state: 'sending'|'ok'|'error'|'undone', node} (le code brut n'est pas garde)
  let nextId = 1;

  function renderEntry(entry) {
    const icons = { sending: '…', ok: '✓', error: '✗', undone: '↶' };
    const classes = { sending: 'todo', ok: 'ok', error: 'error', undone: 'undone' };
    const node = el('li', { class: classes[entry.state] },
      el('span', { class: 'icon' }, icons[entry.state]),
      el('div', { class: 'main' }, el('div', { class: 'name' }, entry.label)));
    if (entry.node) entry.node.replaceWith(node);
    entry.node = node;
    return node;
  }
  function addEntry(code) {
    const entry = { id: nextId++, label: describe(code), state: 'sending' };
    sent.unshift(entry);
    $('#history-list').prepend(renderEntry(entry));
    while (sent.length > 30) { const old = sent.pop(); old.node.remove(); }
    return entry;
  }

  // ------------------------------------------------------------------------------------------------
  // Connexion WebSocket

  let socket = null;
  let connected = false;     // connecte au serveur
  let frontConnected = false; // poste connecte a la session
  let ended = false;
  let retryDelay = 1000;
  let pingTimer = 0;

  function setLink(kind, text) {
    $('#link').className = kind;
    $('#link-text').textContent = text;
  }
  function updateLink() {
    if (ended) setLink('bad', 'Session fermée');
    else if (!connected) setLink('warn', 'Connexion au serveur perdue, nouvelle tentative…');
    else if (!frontConnected) setLink('warn', 'Connecté au serveur, en attente du poste…');
    else setLink('ok', 'Connecté au poste : les scans sont envoyés');
  }

  function endSession(reason) {
    ended = true;
    connected = false;
    clearInterval(pingTimer);
    if (socket) { try { socket.close(); } catch (e) { /* deja fermee */ } }
    stopCamera();
    $('#start').hidden = true;
    $('#ended').hidden = false;
    $('#manual').disabled = true;
    $('#undo').disabled = true;
    $('#ended-reason').textContent = reason ? 'Motif : ' + reason + '.' : '';
    updateLink();
  }

  function connect() {
    if (ended) return;
    const url = new URL(`ws/scanner/phone?s=${encodeURIComponent(sessionId)}&k=${encodeURIComponent(sessionKey)}`, document.baseURI);
    url.protocol = location.protocol === 'https:' ? 'wss:' : 'ws:';
    let opened = false;
    socket = new WebSocket(url);
    socket.onopen = () => {
      opened = true;
      connected = true;
      retryDelay = 1000;
      clearInterval(pingTimer);
      pingTimer = setInterval(() => send({ type: 'ping' }), PING_MS);
      updateLink();
    };
    socket.onmessage = (event) => {
      let message;
      try { message = JSON.parse(event.data); } catch (e) { return; }
      handleMessage(message);
    };
    socket.onclose = () => {
      clearInterval(pingTimer);
      connected = false;
      frontConnected = false;
      if (ended) return;
      updateLink();
      if (opened) { setTimeout(connect, retryDelay); return; }
      // refus a la poignee de main : la session a peut-etre expire pendant l'absence
      retryDelay = Math.min(retryDelay * 2, 15000);
      checkSession().then((open) => { if (open) setTimeout(connect, retryDelay); });
    };
  }

  // false si le serveur confirme que la session est fermee (sinon on continue d'essayer)
  async function checkSession() {
    try {
      const url = new URL(`api/remote-scanner/check/?s=${encodeURIComponent(sessionId)}&k=${encodeURIComponent(sessionKey)}`, document.baseURI);
      const response = await fetch(url, { cache: 'no-store' });
      if (response.status === 404) {
        endSession('session expirée ou fermée par le poste');
        return false;
      }
    } catch (e) { /* serveur injoignable : on reessaiera */ }
    return true;
  }

  function send(message) {
    if (!socket || socket.readyState !== WebSocket.OPEN) return false;
    socket.send(JSON.stringify(message));
    return true;
  }

  function handleMessage(message) {
    switch (message.type) {
      case 'hello': frontConnected = !!message.front_connected; updateLink(); break;
      case 'front':
        frontConnected = !!message.connected;
        updateLink();
        if (!frontConnected) toast('Le poste s\'est déconnecté', true);
        break;
      case 'ack': {
        const entry = sent.find((item) => item.id === message.id);
        if (!entry) break;
        if (entry.state === 'sending') entry.state = message.ok ? 'ok' : 'error';
        renderEntry(entry);
        if (!message.ok) { feedback.bad(); info('bad', 'Non transmis', message.message); }
        break;
      }
      case 'undo_ack': {
        const entry = sent.find((item) => item.id === message.id);
        if (!message.ok) { feedback.bad(); info('bad', 'Annulation non transmise', 'Poste déconnecté.'); break; }
        if (entry) { entry.state = 'undone'; renderEntry(entry); }
        info('warn', 'Dernier scan annulé', entry ? entry.label : '');
        break;
      }
      case 'feedback':
        // le poste signale un mauvais scan (perime, inconnu...) : comme le bip de la douchette
        feedback.bad();
        info('bad', redact(message.message) || 'Produit périmé ou code non reconnu', '');
        if (sent[0] && sent[0].state === 'ok') {
          sent[0].state = 'error';
          renderEntry(sent[0]);
        }
        break;
      case 'closed': endSession(message.reason); break;
      default: break;
    }
  }

  function sendCode(code) {
    unlockAudio();
    if (ended) return;
    if (!connected) {
      feedback.bad();
      info('bad', 'Non envoyé', 'Pas de connexion au serveur : réessayez dans un instant.');
      return;
    }
    const entry = addEntry(code);
    send({ type: 'scan', code, id: entry.id });
    feedback.sent();
    info(frontConnected ? 'ok' : 'warn', entry.label, frontConnected ? '' : 'Poste non connecté');
  }

  // « Annuler le dernier » : le poste retire le dernier scan venu de ce telephone
  function undoLast() {
    unlockAudio();
    const entry = sent.find((item) => item.state === 'ok' || item.state === 'error');
    if (!entry) { toast('Rien à annuler'); return; }
    if (!send({ type: 'undo', id: entry.id })) { feedback.bad(); info('bad', 'Non envoyé', 'Pas de connexion au serveur.'); }
  }

  // ------------------------------------------------------------------------------------------------
  // Camera et detection (meme principe que le front web : BarcodeDetector, sinon jsQR)

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
  function cameraShouldRun() { return cameraWanted && !document.hidden && !ended; }

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
    $('#start').hidden = ended; // session fermee : plus de camera
  }

  async function openCamera() {
    $('#camera-error').textContent = '';
    if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) {
      cameraFailed("La caméra n'est accessible qu'en HTTPS. Utilisez « Saisir un code ».");
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
      } catch (e) { /* image illisible */ }
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
    sendCode(code);
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
  $('#undo').addEventListener('click', undoLast);
  $('#manual').addEventListener('click', () => {
    const code = prompt('Code à envoyer au poste :');
    if (code && code.trim()) sendCode(code.trim());
  });

  // ------------------------------------------------------------------------------------------------
  // Demarrage : la cle est retiree de la barre d'adresse (historique, partage d'ecran) mais gardee
  // pour la session de l'onglet, afin qu'un rechargement de la page reconnecte le telephone.

  try {
    if (sessionId && sessionKey) sessionStorage.setItem('qrprotec.scanner', JSON.stringify({ id: sessionId, key: sessionKey }));
    else ({ id: sessionId, key: sessionKey } = JSON.parse(sessionStorage.getItem('qrprotec.scanner') || '{}'));
  } catch (e) { /* stockage indisponible */ }
  if (!sessionId || !sessionKey) {
    endSession('lien incomplet : scannez le QR code affiché sur le poste');
    return;
  }
  window.history.replaceState(null, '', 'scanner');
  updateLink();
  connect();
})();
