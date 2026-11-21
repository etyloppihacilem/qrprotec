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
    loggedIn({ ...user, loginAt: Date.now() });
    return true;
  } catch (e) {
    feedback.bad();
    toast(e.message, true);
    if (e.data && e.data.passkey_unknown) rememberLoginMethod(null); // supprimee : ne plus l'essayer d'abord
    return false;
  }
}

// Creation : true si la passkey est enregistree
async function createPasskey() {
  if (!state.user || !passkeySupported()) return false;
  let credential;
  try {
    const { publicKey } = await api('me/passkeys/', { user: credentials(), action: 'options' });
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
    const data = await api('me/passkeys/', { user: credentials(), action: 'create', credential: {
      id: credential.id, type: credential.type,
      response: {
        clientDataJSON: bufferToB64url(credential.response.clientDataJSON),
        attestationObject: bufferToB64url(credential.response.attestationObject),
        transports: credential.response.getTransports ? credential.response.getTransports() : [],
      },
    } });
    state.user.has_passkey = true;
    state.passkeys = data.passkeys;
    save();
    feedback.info();
    toast('Passkey créée : la prochaine fois, « Se connecter avec une passkey ». Votre carte marche toujours.');
    return true;
  } catch (e) {
    toast(e.message, true);
    return false;
  }
}

// Premiere connexion au web (Hippolyte, 2026-11-20) : passkey proposee une seule fois par compte (reglage web
// passkey_offered, note quelle que soit la reponse), ensuite seulement dans ⚙ Reglages > Passkey. Souvent la connexion
// par le QR code de depannage du TECH, pour un secouriste sans carte.
async function offerPasskey(user) {
  if (!passkeySupported() || !user || user.guest || user.has_passkey || user.passkey_session || sharedWeb()) return;
  if (verifInProgress() || user !== state.user) return; // pas au milieu d'une vérif, ni apres une deconnexion
  if (webPref('passkey_offered')) return;
  setWebPrefs({ passkey_offered: true }).catch(() => {}); // une seule proposition, meme sans reponse
  const choice = await askDialog({
    title: 'Créer une passkey sur ce téléphone ?',
    body: el('div', {},
      el('p', {}, 'Vous vous connecterez ensuite avec l\'empreinte, le visage ou le code de votre téléphone, sans carte ni code PIN.'),
      el('p', { class: 'hint' }, 'Sur le web, la connexion avec le nom et le code PIN ne marchera plus pour vous (au poste, si). Votre carte reste valable. '
        + 'Le TECH peut supprimer la passkey si vous changez de téléphone.')),
    choices: [{ label: 'Créer une passkey', value: true, cls: 'primary' }],
    cancel: 'Plus tard',
  });
  if (choice) await createPasskey();
  else toast('Vous pourrez créer une passkey plus tard dans ⚙ Réglages.');
}

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

// QR code de connexion au poste : seulement pour une session ouverte avec une passkey (Hippolyte, 2026-11-20). Le
// secouriste qui n'a plus de nom + PIN le montre a la douchette du poste ; 5 min, une seule fois.
const posteQrAllowed = () => !!(state.user && state.user.passkey_session && state.user.role !== 'admin');
let posteQrTimer = null;

async function showPosteQr() {
  const dialog = $('#poste-qr');
  const body = $('#poste-qr-body');
  clearInterval(posteQrTimer);
  body.replaceChildren(el('h2', {}, 'Connexion au poste'), el('p', { class: 'hint' }, 'Création du QR code…'));
  if (!dialog.open) dialog.showModal();
  try {
    const data = await api('me/poste-login/', { user: credentials() });
    const until = Date.now() + data.seconds * 1000;
    const left = el('p', { class: 'hint' });
    const tick = () => {
      const seconds = Math.max(0, Math.round((until - Date.now()) / 1000));
      if (!seconds) {
        clearInterval(posteQrTimer);
        body.replaceChildren(el('h2', {}, 'Connexion au poste'), el('p', {}, 'QR code expiré.'),
          el('button', { type: 'button', class: 'primary', onclick: showPosteQr }, icon('refresh'), 'Nouveau QR code'));
        return;
      }
      left.textContent = `Valable encore ${Math.floor(seconds / 60)} min ${String(seconds % 60).padStart(2, '0')} s, une seule fois.`;
    };
    body.replaceChildren(el('h2', {}, 'Connexion au poste'), debugQr(data.url),
      el('p', {}, el('b', {}, 'Scannez ce QR code avec la douchette du poste.')), left);
    tick();
    posteQrTimer = setInterval(tick, 1000);
  } catch (e) {
    body.replaceChildren(el('h2', {}, 'Connexion au poste'), el('p', { class: 'pin-error' }, e.message));
    if (e.status === 403 && e.data && e.data.pin_required) { state.user = null; save(); render(); }
  }
}
$('#poste-qr-close').addEventListener('click', () => $('#poste-qr').close());
$('#poste-qr').addEventListener('close', () => clearInterval(posteQrTimer));
$('#home-poste-qr').addEventListener('click', showPosteQr);

$('#login-gate-passkey').addEventListener('click', () => passkeyLogin());
$('#name-login-passkey').addEventListener('click', () => passkeyLogin());
$('#login-gate-passkey').hidden = !passkeySupported();
$('#name-login-passkey').hidden = !passkeySupported();
