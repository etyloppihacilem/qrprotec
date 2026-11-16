/* QRProtec - front web : notifications web des admins (abonnement push). */
'use strict';

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
  if (changes.delete && !await confirmDialog('Arrêter les notifications ?', `Plus aucune notification ne sera envoyée à « ${device.device} ».`, 'Arrêter', 'danger')) return;
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
