/* QRProtec - front web : passkeys (connexion sans badge ni PIN), QR code de connexion au poste. */
'use strict';

// Passkey : cle gardee par le telephone (empreinte, visage ou code du telephone), verifiee par le serveur
// (inventory/passkeys.py). Une fois creee, la connexion par nom et PIN ne marche plus pour cette personne sur le web
// (les postes la gardent : leur cle de front protege le code) ; la carte
// reste valable. Proposee apres une connexion (surtout la premiere, par le QR code de depannage du TECH), jamais sur
// un web partage (tablette du local).
//
// Debogage sur ordinateur : Chrome > DevTools > ⋮ > More tools > WebAuthn > « Enable virtual authenticator
// environment », authentificateur « internal », « Supports resident keys » et « Supports user verification » coches.
// Le site doit etre en HTTPS avec un certificat valide, ou sur http://localhost (voir docs/technique/passkeys.md).


const passkeySupported = () => !!(window.PublicKeyCredential && navigator.credentials && window.isSecureContext);

function b64urlToBuffer(value) {
  const base64 = value.replace(/-/g, '+').replace(/_/g, '/') + '='.repeat((4 - (value.length % 4)) % 4);
  return Uint8Array.from(atob(base64), (c) => c.charCodeAt(0)).buffer;
}

function bufferToB64url(buffer) {
  let text = '';
  new Uint8Array(buffer).forEach((byte) => { text += String.fromCharCode(byte); });
  return btoa(text).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
}

// Erreurs du navigateur (DOMException) en francais ; null : la personne a annule, rien a dire
function passkeyBrowserError(e) {
  if (e && e.name === 'NotAllowedError') return null; // annule, ou delai depasse
  if (e && e.name === 'InvalidStateError') return 'Une passkey existe déjà sur cet appareil pour votre compte.';
  if (e && e.name === 'SecurityError') return 'Passkeys indisponibles sur cette adresse (HTTPS et nom de domaine exigés).';
  if (e && e.name === 'NotSupportedError') return 'Cet appareil ne sait pas créer de passkey.';
  return (e && e.message) || 'Passkey impossible.';
}

// Dernier moyen de connexion de ce telephone (Hippolyte, 2026-11-20) : 'passkey' ou 'other', jamais garde sur un web
// partage. Apres une connexion par passkey, la prochaine demande de connexion essaie la passkey d'abord.
const STORAGE_LAST_LOGIN = 'qrprotec.last-login';

function rememberLoginMethod(user) {
  try {
    if (sharedWeb() || !user || user.guest) localStorage.removeItem(STORAGE_LAST_LOGIN);
    else localStorage.setItem(STORAGE_LAST_LOGIN, user.passkey_session ? 'passkey' : 'other');
  } catch (e) { /* stockage bloque : on propose simplement tous les moyens */ }
}

function passkeyFirst() {
  if (!passkeySupported() || sharedWeb()) return false;
  try { return localStorage.getItem(STORAGE_LAST_LOGIN) === 'passkey'; } catch (e) { return false; }
}

// Demande de connexion sur un telephone dont la derniere connexion etait une passkey : la passkey d'abord, puis
// `fallback` (fenetre des autres moyens : carte, nom + PIN) si la personne l'annule ou si elle est refusee.
let passkeyFirstBusy = false;
async function tryPasskeyFirst(fallback) {
  if (passkeyFirstBusy) return;
  passkeyFirstBusy = true;
  let done = false;
  try { done = await passkeyLogin({ quiet: true }); } finally { passkeyFirstBusy = false; }
  if (!done && !state.user) fallback();
}

// true si la connexion a reussi. quiet : essai automatique (passkeyFirst), pas de message si la personne annule ;
// les autres moyens de connexion s'affichent ensuite.
async function passkeyLogin({ quiet = false } = {}) {
  if (!passkeySupported()) { if (!quiet) toast('Ce navigateur ne gère pas les passkeys.', true); return false; }
  let credential;
  try {
    const { publicKey } = await api('auth/passkey/options/', {});
    publicKey.challenge = b64urlToBuffer(publicKey.challenge);
    credential = await navigator.credentials.get({ publicKey });
  } catch (e) {
    const message = e && e.status !== undefined ? e.message : passkeyBrowserError(e);
    if (message) toast(message, true);
    return false;
  }
  try {
    const user = await api('auth/passkey/', { credential: {
      id: credential.id, type: credential.type,
      response: {
        clientDataJSON: bufferToB64url(credential.response.clientDataJSON),
        authenticatorData: bufferToB64url(credential.response.authenticatorData),
        signature: bufferToB64url(credential.response.signature),
        userHandle: credential.response.userHandle ? bufferToB64url(credential.response.userHandle) : null,
      },
    } });
    if ($('#name-login').open) $('#name-login').close();
    if (user.poste_otp) user.poste_otp.received_at = Date.now(); // ecart d'horloge pour les codes du QR poste
    loggedIn({ ...user, loginAt: Date.now() });
    return true;
  } catch (e) {
    feedback.bad();
    toast(e.message, true);
    if (e.data && e.data.passkey_unknown) rememberLoginMethod(null); // supprimee : ne plus l'essayer d'abord
    return false;
  }
}

// Creation : true si la passkey est enregistree. `card` : {matricule, key} de la carte quand la connexion attend
// encore le code PIN obligatoire a choisir (ecran de premiere connexion, web/app/welcome.js), sinon l'utilisateur connecte
async function createPasskey(card = null) {
  const user = card || (state.user ? credentials() : null);
  if (!user || !passkeySupported()) return false;
  let credential;
  try {
    const { publicKey } = await api('me/passkeys/', { user, action: 'options' });
    publicKey.challenge = b64urlToBuffer(publicKey.challenge);
    publicKey.user.id = b64urlToBuffer(publicKey.user.id);
    publicKey.excludeCredentials = (publicKey.excludeCredentials || []).map((row) => ({ ...row, id: b64urlToBuffer(row.id) }));
    credential = await navigator.credentials.create({ publicKey });
  } catch (e) {
    const message = e && e.status !== undefined ? e.message : passkeyBrowserError(e);
    if (message) toast(message, true);
    return false;
  }
  try {
    const data = await api('me/passkeys/', { user, action: 'create', credential: {
      id: credential.id, type: credential.type,
      response: {
        clientDataJSON: bufferToB64url(credential.response.clientDataJSON),
        attestationObject: bufferToB64url(credential.response.attestationObject),
        transports: credential.response.getTransports ? credential.response.getTransports() : [],
      },
    } });
    if (state.user && !card) {
      state.user.has_passkey = true;
      state.passkeys = data.passkeys;
      save();
    }
    feedback.info();
    toast('Passkey créée : la prochaine fois, « Se connecter avec une passkey ». Votre carte marche toujours.');
    return true;
  } catch (e) {
    toast(e.message, true);
    return false;
  }
}

// Premiere connexion au web : passkey proposee une seule fois par compte (reglage web passkey_offered), par l'ecran
// de premiere connexion (web/app/welcome.js), ensuite seulement dans ⚙ Reglages > Passkey.

// Reglages : passkeys de l'utilisateur, creer, supprimer
async function loadPasskeys() {
  if (!state.user || state.user.guest) return;
  try {
    const data = await api('me/passkeys/', { user: credentials(), action: 'list' });
    state.passkeys = data.passkeys;
    state.user.has_passkey = data.passkeys.length > 0;
    save();
  } catch (e) {
    state.passkeys = null;
  }
  if (settingsOpen()) renderPasskeySettings();
}

function renderPasskeySettings() {
  const box = $('#settings-passkey');
  if (!box) return;
  box.hidden = !state.user || state.user.guest;
  if (box.hidden) return;
  const rows = state.passkeys;
  const parts = [el('h3', {}, 'Passkey')];
  if (!passkeySupported()) {
    parts.push(el('p', { class: 'hint' }, 'Ce navigateur ne gère pas les passkeys (ou la page n\'est pas en HTTPS).'));
  } else if (rows == null) {
    parts.push(el('p', { class: 'hint' }, 'Chargement…'));
  } else if (!rows.length) {
    parts.push(el('p', { class: 'hint' }, 'Connectez-vous avec l\'empreinte, le visage ou le code de votre téléphone. '
      + 'Avec une passkey, la connexion par nom et code PIN ne marche plus pour vous sur le web (au poste, si) ; votre carte, si.'));
  } else {
    parts.push(el('p', { class: 'hint' }, 'Sur le web, connexion par nom et code PIN désactivée : passkey ou carte. Au poste, le nom et le code marchent toujours.'));
    rows.forEach((row) => parts.push(el('div', { class: 'passkey-row' },
      el('div', {}, el('b', {}, row.device || 'Passkey'),
        el('div', { class: 'hint' }, `Créée ${fmtDateTime(row.created, { on: true })}`
          + (row.last_used ? `, utilisée ${fmtDateTime(row.last_used, { on: true })}` : ''))),
      el('button', { type: 'button', class: 'danger', onclick: () => deletePasskey(row) }, icon('trash'), 'Supprimer'))));
  }
  if (passkeySupported() && rows != null && !sharedWeb()) {
    parts.push(el('div', { class: 'push-buttons' }, el('button', { type: 'button', class: rows.length ? '' : 'primary',
      onclick: async () => { if (await createPasskey()) renderPasskeySettings(); } }, icon('key'),
    rows.length ? 'Ajouter une passkey (autre appareil)' : 'Créer une passkey')));
  }
  box.replaceChildren(...parts);
}

async function deletePasskey(row) {
  const last = (state.passkeys || []).length === 1;
  if (!await confirmDialog('Supprimer cette passkey ?', last ? 'Vous vous connecterez de nouveau avec votre carte, ou votre nom et votre code PIN.' : '', 'Supprimer', 'danger')) return;
  try {
    const data = await api('me/passkeys/', { user: credentials(), action: 'delete', id: row.id });
    state.passkeys = data.passkeys;
    state.user.has_passkey = data.passkeys.length > 0;
    save();
    toast('Passkey supprimée.');
  } catch (e) {
    toast(e.message, true);
  }
  renderPasskeySettings();
}

// QR code de connexion au poste : seulement pour une session ouverte avec une passkey, admins compris (Hippolyte,
// 2026-11-20 et 2026-11-27). A la connexion, le serveur remet une cle OTP (state.user.poste_otp, valable autant que la
// session web) : le telephone en tire un code toutes les 30 s, meme hors connexion (TOTP, RFC 6238), et affiche
// badge?m=...&o=ID&c=CODE. Le poste l'envoie au serveur, qui accepte un code une seule fois.
const posteQrAllowed = () => !!(state.user && state.user.passkey_session && state.user.poste_otp);
let posteQrTimer = null;

async function totpCode(secret, step, digits) {
  const key = await crypto.subtle.importKey('raw', b64urlToBuffer(secret), { name: 'HMAC', hash: 'SHA-1' }, false, ['sign']);
  const counter = new DataView(new ArrayBuffer(8));
  counter.setUint32(0, Math.floor(step / 2 ** 32));
  counter.setUint32(4, step >>> 0);
  const digest = new Uint8Array(await crypto.subtle.sign('HMAC', key, counter.buffer));
  const offset = digest[digest.length - 1] & 0x0f;
  const value = ((digest[offset] & 0x7f) << 24) | (digest[offset + 1] << 16) | (digest[offset + 2] << 8) | digest[offset + 3];
  return String(value % 10 ** digits).padStart(digits, '0');
}

function posteQrUrl(otp, code) {
  const params = new URLSearchParams({ m: state.user.matricule, o: otp.id, c: code });
  return `${location.origin}/badge?${params}`;
}

function showPosteQr() {
  const dialog = $('#poste-qr');
  const body = $('#poste-qr-body');
  const otp = state.user && state.user.poste_otp;
  clearInterval(posteQrTimer);
  if (!dialog.open) dialog.showModal();
  if (!otp || !window.crypto || !crypto.subtle) {
    body.replaceChildren(el('h2', {}, 'Connexion au poste'), el('p', { class: 'pin-error' }, 'QR code indisponible sur ce navigateur.'));
    return;
  }
  // ecart d'horloge du telephone, mesure a la connexion : le serveur compte en temps serveur
  const skew = otp.server_time && otp.received_at ? otp.server_time - otp.received_at : 0;
  const left = el('p', { class: 'hint' });
  let shown = -1;
  const tick = async () => {
    const now = Date.now() + skew;
    if (otp.expires && now >= otp.expires) {
      clearInterval(posteQrTimer);
      body.replaceChildren(el('h2', {}, 'Connexion au poste'),
        el('p', {}, 'Session passkey expirée : reconnectez-vous avec votre passkey pour un nouveau QR code.'));
      return;
    }
    const step = Math.floor(now / 1000 / otp.step);
    left.textContent = `Nouveau code dans ${Math.max(1, Math.ceil((step + 1) * otp.step - now / 1000))} s. Fonctionne aussi hors connexion.`;
    if (step === shown) return;
    shown = step;
    const code = await totpCode(otp.secret, step, otp.digits || 6);
    if (!dialog.open || shown !== step) return;
    body.replaceChildren(el('h2', {}, 'Connexion au poste'), debugQr(posteQrUrl(otp, code)),
      el('p', {}, el('b', {}, 'Scannez ce QR code avec la douchette du poste.')), left);
  };
  body.replaceChildren(el('h2', {}, 'Connexion au poste'), left);
  tick();
  posteQrTimer = setInterval(tick, 1000);
}
$('#poste-qr-close').addEventListener('click', () => $('#poste-qr').close());
$('#poste-qr').addEventListener('close', () => clearInterval(posteQrTimer));
$('#home-poste-qr').addEventListener('click', showPosteQr);

$('#login-gate-passkey').addEventListener('click', () => passkeyLogin());
$('#name-login-passkey').addEventListener('click', () => passkeyLogin());
$('#login-gate-passkey').hidden = !passkeySupported();
$('#name-login-passkey').hidden = !passkeySupported();
