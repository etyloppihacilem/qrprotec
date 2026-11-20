/* QRProtec - front web : reglages (installer l'appli, code PIN, notifications, web partage). */
'use strict';

// Reglages : installer l'appli (tout le monde, connecte ou non), code PIN (connectes), notifications web (s'il peut
// en recevoir)
function openNotifications() {
  installGuideShown = false;
  renderInstallSettings();
  renderScanSettings();
  $('#settings-pin').hidden = !state.user;
  renderPinSettings();
  state.passkeys = null;
  renderPasskeySettings();
  if (state.user) loadPasskeys();
  $('#settings-verif').hidden = !state.user;
  renderVerifSettings();
  const notifications = hasNotifications();
  $('#notifications-title').hidden = !notifications;
  $('#notifications-body').hidden = !notifications;
  if (notifications) renderNotifications();
  renderSharedSettings();
  $('#notifications').showModal();
  if (notifications) loadPush();
}

// Installer l'appli sur l'ecran d'accueil. Chrome et Edge (Android, ordinateur) proposent leur fenetre
// d'installation (beforeinstallprompt, gardee pour le bouton) ; Safari n'en a pas : tuto par appareil.
let installPrompt = null;
window.addEventListener('beforeinstallprompt', (event) => {
  installPrompt = event; // sans preventDefault : le navigateur garde aussi sa propre proposition
  if ($('#notifications').open) renderInstallSettings();
});
window.addEventListener('appinstalled', () => {
  installPrompt = null;
  toast('QRprotec est installée.');
  if ($('#notifications').open) renderInstallSettings();
});

const installedApp = () => window.matchMedia('(display-mode: standalone)').matches
  || window.matchMedia('(display-mode: fullscreen)').matches || navigator.standalone === true;

function deviceKind() {
  const ua = navigator.userAgent;
  // iPad recent : se presente comme un Mac, mais tactile
  if (/iPhone|iPad|iPod/.test(ua) || (/Macintosh/.test(ua) && navigator.maxTouchPoints > 1)) return 'ios';
  if (/Android/.test(ua)) return 'android';
  return 'desktop';
}

const INSTALL_STEPS = {
  android: {
    title: '🤖 Android',
    steps: [
      'Ouvrez cette page dans Chrome.',
      ['Touchez le menu ', el('b', {}, '⋮'), ' en haut à droite.'],
      ['Choisissez ', el('b', {}, 'Installer l\'appli'), ' (ou ', el('b', {}, 'Ajouter à l\'écran d\'accueil'), ').'],
      ['Confirmez avec ', el('b', {}, 'Installer'), ' : l\'icône QRprotec apparaît avec vos autres applis.'],
    ],
    note: 'Samsung Internet : menu ≡ > Ajouter la page à > Écran d\'accueil. Firefox : menu ⋮ > Installer.',
  },
  ios: {
    title: '📱 iPhone et iPad',
    steps: [
      'Ouvrez cette page dans Safari.',
      ['Touchez le bouton ', el('b', {}, 'Partager'), ' (carré avec une flèche vers le haut), en bas de l\'écran.'],
      ['Faites défiler et choisissez ', el('b', {}, 'Sur l\'écran d\'accueil'), '.'],
      ['Laissez le nom QRprotec et touchez ', el('b', {}, 'Ajouter'), '.'],
    ],
    note: 'L\'appli installée a sa propre session : connectez-vous une fois dedans. '
      + 'Elle permet aussi de recevoir les notifications sur iPhone.',
  },
  desktop: {
    title: '💻 Ordinateur',
    steps: [
      'Ouvrez cette page dans Chrome ou Edge.',
      ['Cliquez sur l\'icône d\'installation ', el('b', {}, '⊕'), ' à droite de la barre d\'adresse, ou menu ',
        el('b', {}, '⋮'), ' > ', el('b', {}, 'Enregistrer et partager'), ' > ', el('b', {}, 'Installer QRprotec'),
        ' (Edge : menu ', el('b', {}, '…'), ' > ', el('b', {}, 'Applications'), ').'],
      ['Confirmez avec ', el('b', {}, 'Installer'), ' : QRprotec s\'ouvre dans sa propre fenêtre.'],
    ],
    note: 'Mac avec Safari : menu Fichier > Ajouter au Dock. Firefox ne sait pas installer d\'applis web.',
  },
};

function installGuide(kind) {
  const guide = INSTALL_STEPS[kind];
  return [
    el('ol', { class: 'install-steps' }, guide.steps.map((step) => el('li', {}, step))),
    el('p', { class: 'hint' }, guide.note),
  ];
}

// tuto replie derriere le bouton « Installer l'application » ; replie a chaque ouverture des reglages
let installGuideShown = false;

async function startInstall() {
  const prompt = installPrompt;
  if (prompt) {
    installPrompt = null; // la fenetre ne s'affiche qu'une fois par evenement
    try {
      await prompt.prompt();
      if ((await prompt.userChoice).outcome === 'accepted') return; // appinstalled previent
    } catch (e) { /* fenetre refusee par le navigateur : tuto a la place */ }
  }
  installGuideShown = true;
  renderInstallSettings();
}

function renderInstallSettings() {
  const box = $('#settings-install');
  if (!box) return;
  const kind = deviceKind();
  const installed = installedApp();
  const parts = [el('h3', {}, '📲 Installer l\'appli')];
  parts.push(el('p', { class: 'hint' }, installed ? '✓ Vous utilisez l\'appli QRprotec installée.'
    : 'Ajoutez QRprotec à l\'écran d\'accueil : elle s\'ouvre en plein écran, comme une vraie appli, sans passer '
      + 'par le navigateur.'));
  if (!installGuideShown) {
    // Chrome et Edge : le bouton ouvre directement leur fenetre d'installation, le tuto seulement si elle manque
    parts.push(el('div', { class: 'push-buttons' }, el('button', { type: 'button', class: installed ? '' : 'primary',
      onclick: installed ? () => { installGuideShown = true; renderInstallSettings(); } : startInstall },
    installed ? 'Installer sur un autre appareil' : 'Installer l\'application')));
    box.replaceChildren(...parts);
    return;
  }
  if (!installed) {
    if (!window.isSecureContext) {
      parts.push(el('p', { class: 'error' }, 'L\'installation exige une connexion HTTPS (adresse en https://).'));
    }
    parts.push(el('p', { class: 'install-device' }, INSTALL_STEPS[kind].title), ...installGuide(kind));
  }
  // les autres appareils, replies : pour aider quelqu'un d'autre ou installer sur un autre poste
  const others = Object.keys(INSTALL_STEPS).filter((other) => installed || other !== kind);
  const guides = others.map((other) => [el('p', { class: 'install-device' }, INSTALL_STEPS[other].title),
    ...installGuide(other)]);
  if (installed) parts.push(...guides.flat());
  else parts.push(el('details', { class: 'install-others' }, el('summary', {}, 'Autres appareils'), ...guides));
  box.replaceChildren(...parts);
}

// PIN facultatif : supprimable sauf pour un admin ou si le TECH l'a rendu obligatoire (verifie par le serveur)
const pinCanDisable = (user) => (user.pin_can_disable !== undefined ? !!user.pin_can_disable
  : user.role !== 'admin' && user.pin_mode !== 'required');

// « Bulle de scan » : le bouton ↘ de l'ecran de scan, qui reduit la camera dans un coin (la liste prend tout
// l'ecran). Reglage du compte (web_prefs, modifiable aussi par un admin) ; sans connexion, celui de ce telephone.
function renderScanSettings() {
  const box = $('#settings-scan');
  if (!box) return;
  const input = el('input', { type: 'checkbox', checked: scanBubbleOn() ? '' : null, onchange: () => saveScanBubble(input) });
  box.replaceChildren(el('h3', {}, 'Écran de scan'),
    el('label', { class: 'check' }, input, 'Bulle de scan'),
    el('p', { class: 'hint' }, 'Cochée : le bouton ↘ de l\'écran de scan réduit la caméra dans une petite fenêtre en '
      + 'bas de l\'écran, la liste prend toute la place. Décochée : la caméra reste en haut de l\'écran.'
      + (state.user ? '' : ' Connectez-vous pour l\'enregistrer sur votre compte.')));
}

async function saveScanBubble(input) {
  const value = input.checked;
  if (!state.user) {
    try { localStorage.setItem(STORAGE_SCAN_BUBBLE, value ? '1' : '0'); } catch (e) { /* ignore */ }
    syncScanBubble();
    return;
  }
  input.disabled = true;
  try {
    await setWebPrefs({ scan_bubble: value });
    rememberScanBubble();
    syncScanBubble();
    toast(value ? 'Bulle de scan activée.' : 'Bulle de scan désactivée.');
  } catch (e) {
    feedback.bad();
    input.checked = !value;
    toast(e.message, true);
  } finally {
    input.disabled = false;
  }
}

function renderPinSettings() {
  const box = $('#settings-pin');
  const user = state.user;
  if (!box || !user) return;
  const pinInput = (id, placeholder) => el('input', { id, class: 'pin', type: 'text', inputmode: 'numeric',
    pattern: '[0-9]*', maxlength: 8, autocomplete: 'off', placeholder });
  const hasPin = !!user.has_pin;
  const parts = [el('h3', {}, 'Code PIN')];
  const atLogin = !!user.pin_at_login;
  parts.push(el('p', { class: 'hint' }, !hasPin
    ? 'Pas de code PIN : le badge suffit pour vous connecter. Un code vous permet de vous connecter sans badge '
      + '(Nom + PIN), et peut être demandé après le badge.'
    : atLogin ? 'Votre code PIN est demandé après le badge, et permet de vous connecter sans badge (Nom + PIN).'
      : 'Votre code PIN sert seulement à vous connecter sans badge (Nom + PIN) : le badge suffit.'));
  // « Demander a chaque connexion » : changee tout de suite avec un PIN, envoyee avec le PIN choisi sinon (cochee
  // par defaut quand le code est obligatoire), imposee pour un admin
  const askInput = el('input', { type: 'checkbox', id: 'settings-pin-at-login' });
  askInput.checked = hasPin ? atLogin || !!user.pin_at_login_locked : !!user.pin_mandatory;
  askInput.disabled = !!user.pin_at_login_locked;
  if (hasPin) askInput.addEventListener('change', () => savePinAtLogin(askInput));
  parts.push(el('label', { class: 'settings-check' }, askInput, el('span', {}, 'Demander à chaque connexion', el('br'),
    el('small', { class: 'hint' }, user.pin_at_login_locked ? (user.role === 'admin'
      ? 'Toujours demandé pour les administrateurs.' : 'Toujours demandé : le TECH l\'a rendu obligatoire.')
      : 'Décochée : le code ne sert qu\'à vous connecter sans badge.'))));
  if (hasPin) parts.push(pinInput('settings-pin-current', 'PIN actuel'));
  parts.push(pinInput('settings-pin-new', hasPin ? 'Nouveau PIN (4 à 8 chiffres)' : 'PIN (4 à 8 chiffres)'));
  parts.push(pinInput('settings-pin-confirm', 'Confirmez le PIN'));
  parts.push(el('p', { id: 'settings-pin-error', class: 'pin-error' }));
  const buttons = [el('button', { type: 'button', class: 'primary', onclick: () => savePin(false) }, icon('key'),
    hasPin ? 'Changer le PIN' : 'Définir le PIN')];
  if (hasPin && pinCanDisable(user)) {
    buttons.push(el('button', { type: 'button', onclick: () => savePin(true) }, 'Supprimer le PIN'));
  }
  parts.push(el('div', { class: 'push-buttons' }, ...buttons));
  if (hasPin && !pinCanDisable(user)) {
    parts.push(el('p', { class: 'hint' }, user.role === 'admin'
      ? 'Le code PIN est obligatoire pour les administrateurs.' : 'Le TECH a rendu le code PIN obligatoire.'));
  }
  box.replaceChildren(...parts);
}

// « Web partagé » : reglage de cet appareil (tablette de l'antenne), tout en bas des reglages. Coche ou decoche par
// la gestion et les admins seulement ; les autres le voient (grise) seulement quand il est coche, pour comprendre
// pourquoi ils sont deconnectes.
function renderSharedSettings() {
  const box = $('#settings-shared');
  if (!box) return;
  const on = sharedWeb();
  const editable = canSeeStock();
  box.hidden = !on && !editable;
  if (box.hidden) { box.replaceChildren(); return; }
  const input = el('input', { type: 'checkbox' });
  input.checked = on;
  input.disabled = !editable;
  input.addEventListener('change', () => saveSharedWeb(input.checked));
  box.replaceChildren(el('h3', {}, 'Appareil'),
    el('label', { class: 'settings-check' }, input, el('span', {}, 'Web partagé', el('br'),
      el('small', { class: 'hint' }, 'Pour les tablettes de l\'antenne : la connexion n\'est pas gardée comme sur un '
        + 'téléphone personnel (elle s\'arrête à la fermeture de l\'appli) et finit après 1 h sans activité. '
        + 'Réglage de cet appareil seulement.'
        + (editable ? '' : ' Modifiable par la gestion ou le TECH.')))));
}

function saveSharedWeb(on) {
  if (!canSeeStock()) return;
  try {
    if (on) localStorage.setItem(STORAGE_SHARED_WEB, '1');
    else localStorage.removeItem(STORAGE_SHARED_WEB);
  } catch (e) {
    toast('Stockage du site indisponible sur cet appareil : réglage non enregistré.', true);
    renderSharedSettings();
    return;
  }
  // le navigateur peut vider les donnees d'un site peu visite : on demande a les garder
  if (on && navigator.storage && navigator.storage.persist) navigator.storage.persist().catch(() => {});
  activityAt = 0;
  noteActivity();
  save(); // la connexion en cours passe dans l'onglet (ou en revient)
  toast(on ? 'Web partagé activé sur cet appareil.' : 'Web partagé désactivé sur cet appareil.');
  renderSharedSettings();
}

// Reglages de la vérif (models.WEB_PREFS) : ligne d'info sous la camera, explication apres la connexion
function renderVerifSettings() {
  const box = $('#settings-verif');
  if (!box || !state.user) return;
  const check = (label, checked, hint, onchange) => {
    const input = el('input', { type: 'checkbox' });
    input.checked = checked;
    input.addEventListener('change', async () => {
      input.disabled = true;
      try { await onchange(input.checked); } catch (e) { input.checked = !input.checked; toast(e.message, true); }
      input.disabled = false;
    });
    return el('label', { class: 'settings-check' }, input, el('span', {}, label, el('br'), el('small', { class: 'hint' }, hint)));
  };
  box.replaceChildren(el('h3', {}, 'Vérif'),
    check('Explication de la vérif', !webPref('verif_help_hidden'),
      'Rappel en 3 points au début de chaque vérif.',
      (on) => setWebPrefs({ verif_help_hidden: !on })));
}

const PIN_FIELDS = ['has_pin', 'pin_required', 'pin_mode', 'pin_can_disable', 'pin_mandatory', 'pin_at_login',
  'pin_at_login_locked'];
function keepPinFields(body) {
  for (const field of PIN_FIELDS) state.user[field] = body[field];
}

// Case « Demander a chaque connexion » d'un code existant : enregistree tout de suite
async function savePinAtLogin(input) {
  const value = input.checked;
  input.disabled = true;
  try {
    keepPinFields(await api('me/pin/', { user: badge(), pin_at_login: value }));
    save();
    toast(value ? 'Code PIN demandé à chaque connexion.' : 'Code PIN demandé seulement pour la connexion sans badge.');
    renderPinSettings();
  } catch (e) {
    feedback.bad();
    input.checked = !value;
    input.disabled = false;
    toast(e.message, true);
  }
}

async function savePin(disable) {
  const user = state.user;
  const value = (id) => ($(id) ? $(id).value.trim() : '');
  const error = (message) => { $('#settings-pin-error').textContent = message; };
  const hadPin = !!user.has_pin;
  const current = value('#settings-pin-current');
  const pin = value('#settings-pin-new');
  if (user.has_pin && !current) { error('Saisissez votre PIN actuel.'); return; }
  if (disable) {
    const warning = user.key ? '' : ' Sans PIN, vous ne pourrez plus vous connecter sans badge.';
    if (!await confirmDialog('Supprimer votre code PIN ?', 'Le badge suffira pour vous connecter.' + warning, 'Supprimer', 'danger')) return;
  } else {
    if (!/^\d{4,8}$/.test(pin)) { error('Le PIN doit comporter 4 à 8 chiffres.'); return; }
    if (pin !== value('#settings-pin-confirm')) { error('Les deux PIN sont différents.'); return; }
  }
  try {
    const atLogin = $('#settings-pin-at-login') ? $('#settings-pin-at-login').checked : true;
    const body = await api('me/pin/', { user: badge(), current_pin: current,
      ...(disable ? { disable: true } : hadPin ? { pin } : { pin, pin_at_login: atLogin }) });
    keepPinFields(body);
    save();
    toast(disable ? 'Code PIN supprimé.' : hadPin ? 'Code PIN changé.' : 'Code PIN enregistré.');
    renderPinSettings();
  } catch (e) {
    if (e.data && e.data.pin_blocked) {
      $('#notifications').close();
      state.user = null; // un PIN bloque coupe la session
      save();
      render();
      showPinBlocked(e.data.pin_reset || {});
      return;
    }
    feedback.bad();
    if ($('#settings-pin-current')) $('#settings-pin-current').value = '';
    error(e.message);
  }
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
      el('button', { type: 'button', onclick: disablePush, disabled: busy }, icon('bell-off'), 'Désactiver')));
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
      el('button', { type: 'button', class: 'primary', onclick: enablePush, disabled: busy }, icon('bell'),
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

$('#home-settings').addEventListener('click', openNotifications);
