/* QRProtec - front web : ecran de premiere connexion (passkey d'abord, puis le code PIN obligatoire). */
'use strict';

// Premiere connexion au web (Hippolyte, 2026-11-21) : un ecran en grand, par etapes, au lieu de deux popups (code PIN
// puis passkey). D'abord la passkey (« vous connecter plus vite et sans carte »), ensuite le code PIN s'il est
// obligatoire. Deux cas :
// - avant la connexion : carte scannee, le serveur attend le code obligatoire a choisir (pin_setup_required avec
//   passkey_offer). La passkey se cree avec la carte seule (views/passkeys.py first_login_user), puis le code choisi
//   termine la connexion comme avant (login() avec new_pin) ;
// - apres la connexion : connexion sans code (QR code de depannage du TECH, code facultatif). La passkey, puis le code
//   s'il est obligatoire et pas encore choisi (POST me/pin/).
// La passkey n'est proposee qu'une fois par compte (reglage web passkey_offered), jamais sur un web partage ni a une
// carte d'invite, ni si le navigateur ne la gere pas : il ne reste alors que la fenetre du code, comme avant.

const welcome = { open: false };

const welcomeOpen = () => welcome.open && $('#welcome').open;
const passkeyCanOffer = () => passkeySupported() && !sharedWeb();

// Carte scannee, code obligatoire a choisir : true si l'ecran s'ouvre (sinon la fenetre du code, askPin)
function openWelcomeBeforeLogin(matricule, key, data) {
  if (!data.passkey_offer || !passkeyCanOffer()) return false;
  openWelcome({
    before: { matricule, key },
    prenom: data.prenom || '',
    steps: ['passkey', 'pin', ...(pushSupported() ? ['notify'] : [])],
    pinAtLoginDefault: data.pin_at_login_default !== false || !!data.pin_at_login_locked,
    pinAtLoginLocked: !!data.pin_at_login_locked,
  });
  return true;
}

// Apres loggedIn() : termine l'ecran ouvert avant la connexion (code choisi), sinon l'ouvre pour une premiere
// connexion sans code. Remplace l'ancienne proposition de passkey en popup.
function welcomeAfterLogin(user) {
  if (welcomeOpen() && welcome.before) {
    // connecte avec le code choisi : reste l'etape des notifications, si la passkey a ete creee
    welcome.before = null;
    welcome.busy = false;
    if (welcome.steps[welcome.step + 1] === 'notify' && welcome.created) nextWelcomeStep();
    else finishWelcome();
    return;
  }
  if (!user || user.guest || user !== state.user || verifInProgress()) return;
  if (!passkeyCanOffer() || user.has_passkey || user.passkey_session || webPref('passkey_offered')) return;
  const steps = ['passkey'];
  if (user.pin_mandatory && !user.has_pin) steps.push('pin');
  if (pushSupported()) steps.push('notify');
  openWelcome({
    before: null,
    prenom: user.prenom || '',
    steps,
    pinAtLoginDefault: user.pin_mandatory !== false || !!user.pin_at_login_locked,
    pinAtLoginLocked: !!user.pin_at_login_locked,
  });
}

function openWelcome(options) {
  Object.assign(welcome, options, { open: true, step: 0, created: false, skipped: false, busy: false, error: '',
    atLogin: options.pinAtLoginDefault });
  const dialog = $('#welcome');
  renderWelcome();
  if (!dialog.open) dialog.showModal();
}

function closeWelcome() {
  welcome.open = false;
  welcome.before = null;
  if ($('#welcome').open) $('#welcome').close();
}

// Fin de l'ecran (connecte) : passkey notee comme proposee, quelle que soit la reponse
function finishWelcome() {
  const skipped = welcome.skipped;
  closeWelcome();
  if (state.user) setWebPrefs({ passkey_offered: true }).catch(() => {});
  if (skipped) toast('Vous pourrez créer une passkey plus tard dans ⚙ Réglages.');
}

function nextWelcomeStep() {
  welcome.error = '';
  // notifications des demandes de connexion : seulement avec une passkey (c'est elle qui valide)
  if (welcome.steps[welcome.step + 1] === 'notify' && !welcome.created && !welcome.before) { finishWelcome(); return; }
  if (welcome.steps[welcome.step + 1] === 'notify' && welcome.before) return; // la connexion (code) passe d'abord
  if (welcome.step + 1 >= welcome.steps.length) {
    // apres la connexion, plus rien a faire ; avant, la derniere etape (le code) connecte, voir welcomeAfterLogin
    if (!welcome.before) finishWelcome();
    return;
  }
  welcome.step += 1;
  renderWelcome();
}

// Code refuse par le serveur (trop court, deja demande…) : message sur l'etape du code
function welcomePinError(message) {
  welcome.busy = false;
  welcome.error = message || '';
  renderWelcome();
}

function renderWelcome() {
  const body = $('#welcome-body');
  const step = welcome.steps[welcome.step];
  const count = welcome.steps.length;
  const parts = [];
  parts.push(el('p', { class: 'welcome-hello' }, welcome.prenom ? `Bienvenue ${welcome.prenom} !` : 'Bienvenue !'));
  if (count > 1) {
    parts.push(el('ol', { class: 'welcome-steps', 'aria-label': `Étape ${welcome.step + 1} sur ${count}` },
      ...welcome.steps.map((name, index) => el('li', {
        class: index < welcome.step ? 'done' : index === welcome.step ? 'current' : '',
      }, el('span', {}, index < welcome.step ? '✓' : String(index + 1)),
      name === 'passkey' ? 'Passkey' : name === 'pin' ? 'Code PIN' : 'Notifications'))));
  }
  if (step === 'passkey') parts.push(...welcomePasskeyStep());
  else if (step === 'notify') parts.push(...welcomeNotifyStep());
  else parts.push(...welcomePinStep());
  body.replaceChildren(...parts);
  if (step === 'pin') setTimeout(() => { if ($('#welcome-pin')) $('#welcome-pin').focus(); }, 50);
}

function welcomePasskeyStep() {
  return [
    el('div', { class: 'welcome-icon' }, icon('key')),
    el('h2', { id: 'welcome-title' }, 'Connectez-vous plus vite, sans carte'),
    el('p', {}, 'Créez une passkey sur ce téléphone : la prochaine fois, votre empreinte, votre visage ou le code de '
      + 'votre téléphone suffiront.'),
    el('p', { class: 'hint' }, 'Sur le web, la connexion par nom et code PIN ne marchera plus pour vous (au poste, si). '
      + 'Votre carte reste valable. Le TECH peut supprimer la passkey si vous changez de téléphone.'),
    el('div', { class: 'welcome-buttons' },
      el('button', { type: 'button', class: 'primary', id: 'welcome-create', disabled: welcome.busy || null,
        onclick: welcomeCreatePasskey }, icon('key'), welcome.busy ? 'Création…' : 'Créer une passkey'),
      el('button', { type: 'button', class: 'secondary', disabled: welcome.busy || null,
        onclick: () => { welcome.skipped = true; nextWelcomeStep(); } }, 'Plus tard')),
  ];
}

async function welcomeCreatePasskey() {
  welcome.busy = true;
  renderWelcome();
  const done = await createPasskey(welcome.before ? { matricule: welcome.before.matricule, key: welcome.before.key } : null);
  welcome.busy = false;
  if (!welcomeOpen()) return;
  if (done) { welcome.created = true; nextWelcomeStep(); } else renderWelcome();
}

// Notifications des demandes de connexion (Hippolyte, 2026-11-27) : l'application QRProtec d'un ordinateur demande a
// vous connecter avec votre passkey, ce telephone le signale ; le clic ouvre la page de validation (passkey.js,
// openPosteBrowserLogin). Proposees juste apres la creation de la passkey, une fois connecte.
function welcomeNotifyStep() {
  return [
    welcome.created ? el('p', { class: 'welcome-done' }, '✔ Passkey créée') : '',
    el('div', { class: 'welcome-icon' }, icon('bell')),
    el('h2', { id: 'welcome-title' }, 'Recevez les demandes de connexion'),
    el('p', {}, 'Quand l\'application QRProtec d\'un ordinateur demande à vous connecter, ce téléphone vous prévient : '
      + 'touchez la notification et validez avec votre passkey.'),
    el('p', { class: 'hint' }, 'Seules ces demandes sont activées ici ; le reste se règle dans ⚙ Réglages > Notifications.'),
    el('p', { class: 'pin-error' }, welcome.error || ''),
    el('div', { class: 'welcome-buttons' },
      el('button', { type: 'button', class: 'primary', disabled: welcome.busy || null, onclick: welcomeEnableNotify },
        icon('bell'), welcome.busy ? 'Activation…' : 'Activer les notifications'),
      el('button', { type: 'button', class: 'secondary', disabled: welcome.busy || null,
        onclick: () => finishWelcome() }, 'Plus tard')),
  ];
}

function welcomeEnableNotify() {
  if (welcome.busy) return;
  // la demande d'autorisation part pendant le clic (sinon Firefox et Safari la refusent sans rien afficher)
  const asked = Date.now();
  const permission = askPermission();
  welcome.busy = true;
  welcome.error = '';
  renderWelcome();
  subscribePush({ login_request: true }, permission, asked).then(() => {
    welcome.busy = false;
    if (!welcomeOpen()) return;
    if (state.pushError) { welcome.error = state.pushError; renderWelcome(); return; }
    if (state.user) state.user.notifications = true; // ⚙ Reglages > Notifications s'affiche desormais
    finishWelcome();
  });
}

function welcomePinStep() {
  const pin = el('input', { id: 'welcome-pin', class: 'pin', type: 'text', inputmode: 'numeric', pattern: '[0-9]*',
    maxlength: '64', autocomplete: 'off', enterkeyhint: 'next', placeholder: '6 chiffres ou plus' });
  const confirm = el('input', { id: 'welcome-pin-confirm', class: 'pin', type: 'text', inputmode: 'numeric',
    pattern: '[0-9]*', maxlength: '64', autocomplete: 'off', enterkeyhint: 'go', placeholder: 'Confirmez le code' });
  const atLogin = el('input', { type: 'checkbox', id: 'welcome-pin-at-login' });
  atLogin.checked = !!welcome.atLogin;
  atLogin.disabled = !!welcome.pinAtLoginLocked;
  atLogin.addEventListener('change', () => { welcome.atLogin = atLogin.checked; });
  pin.addEventListener('keydown', (event) => { if (event.key === 'Enter') { event.preventDefault(); confirm.focus(); } });
  const form = el('form', { class: 'welcome-pin-form', onsubmit: (event) => { event.preventDefault(); welcomeSavePin(pin, confirm, atLogin); } },
    pin, confirm,
    el('label', { class: 'settings-check' }, atLogin, el('span', {}, 'Demander à chaque connexion', el('br'),
      el('small', { class: 'hint' }, welcome.pinAtLoginLocked ? 'Imposé par le TECH.'
        : 'Décochée : le code ne sert qu\'à se connecter sans carte (nom, prénom et code).'))),
    el('p', { class: 'pin-error', id: 'welcome-pin-error' }, welcome.error || ''),
    el('div', { class: 'welcome-buttons' },
      el('button', { type: 'submit', class: 'primary', disabled: welcome.busy || null }, welcome.busy ? 'Enregistrement…' : 'Valider'),
      el('button', { type: 'button', class: 'secondary', disabled: welcome.busy || null, onclick: welcomeCancelPin },
        welcome.before ? 'Annuler la connexion' : 'Plus tard')));
  return [
    welcome.created ? el('p', { class: 'welcome-done' }, '✔ Passkey créée') : '',
    el('div', { class: 'welcome-icon' }, icon('lock')),
    el('h2', { id: 'welcome-title' }, 'Choisissez votre code PIN'),
    el('p', {}, 'Le code est obligatoire : au moins 6 chiffres.'),
    el('p', { class: 'hint' }, welcome.created
      ? 'Il sert au poste, avec votre nom et votre prénom.'
      : 'Il permet aussi de vous connecter sans carte, avec votre nom et votre prénom.'),
    form,
  ];
}

async function welcomeSavePin(pinInput, confirmInput, atLoginInput) {
  if (welcome.busy) return;
  const pin = pinInput.value.trim();
  const showError = (message) => { welcome.error = message; $('#welcome-pin-error').textContent = message; };
  if (!NEW_PIN_RE.test(pin)) { showError(NEW_PIN_ERROR); return; }
  if (pin !== confirmInput.value.trim()) { showError('Les deux codes sont différents.'); confirmInput.value = ''; confirmInput.focus(); return; }
  welcome.busy = true;
  welcome.error = '';
  renderWelcome();
  if (welcome.before) {
    // la connexion se termine avec le code : loggedIn() -> welcomeAfterLogin() ferme l'ecran ; refus -> welcomePinError
    await login(welcome.before.matricule, welcome.before.key, { new_pin: pin, pin_at_login: atLoginInput.checked });
    if (welcomeOpen() && welcome.busy) welcomePinError(welcome.error);
    return;
  }
  try {
    keepPinFields(await api('me/pin/', { user: badge(), pin, pin_at_login: atLoginInput.checked }));
    save();
    toast('Code PIN enregistré.');
    welcome.busy = false;
    if (welcome.steps[welcome.step + 1] === 'notify' && welcome.created) nextWelcomeStep();
    else finishWelcome();
  } catch (e) {
    feedback.bad();
    welcomePinError(e.message);
  }
}

function welcomeCancelPin() {
  if (welcome.before) {
    // pas de code, pas de connexion (comme « Annuler » de la fenetre du code) ; la passkey creee reste
    closeWelcome();
    nameLoginCancelled();
    if (welcome.created) toast('Passkey créée : connectez-vous avec elle, ou avec votre carte et le code à choisir.');
    return;
  }
  finishWelcome();
}

// Echap : comme le bouton secondaire de l'etape (jamais une fermeture sans rien dire)
$('#welcome').addEventListener('cancel', (event) => {
  event.preventDefault();
  if (welcome.busy) return;
  if (welcome.steps[welcome.step] === 'passkey') { welcome.skipped = true; nextWelcomeStep(); } else welcomeCancelPin();
});
