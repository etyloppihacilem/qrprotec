/* QRProtec - front web : camera et detection des codes dans le viseur. */
'use strict';

// Camera et detection

const video = $('#video');
// lecture limitee au viseur : BarcodeDetector natif, sinon zxing-cpp en WebAssembly (camera-scan.js)
const reader = window.CameraScan.create(video, $('#viewfinder span'));
let stream = null;
let wakeLock = null;
let cameraWanted = false; // camera demarree par l'utilisateur : rouverte au retour sur la page
let opening = null;       // ouverture en cours (getUserMedia peut prendre plusieurs secondes)
// derniere fois que chaque code a ete vu : un code reste a l'ecran n'est traite qu'une fois, meme si une autre
// etiquette passe entre-temps devant la camera (un seul « dernier code » le laissait repartir)
const seenCodes = new Map();
let torchWanted = false;  // lampe torche (Chrome Android ; Safari iOS et Firefox ne la proposent pas aux pages web)

// Jamais de camera ouverte quand la page est cachee, et une seule ouverture a la fois : sur Firefox
// Android, un flux encore ouvert quand l'ecran s'eteint ou que l'onglet est decharge peut laisser la
// camera « utilisee par Firefox » jusqu'au redemarrage du telephone.
function cameraShouldRun() {
  return cameraWanted && !document.hidden && (!ficheOpen() || readerOpen() || gateOpen()) && $('#opening').hidden
    && $('#restock-screen').hidden; // liste de courses du reassort : on va au stock, pas de camera
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
        // 1080p : les barres fines d'un code-barres et les petits QR codes restent lisibles de plus loin
        video: { facingMode: { ideal: 'environment' }, width: { ideal: 1920 }, height: { ideal: 1080 } },
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
  await reader.prepare();
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

async function scanLoop(media) {
  while (stream === media) {
    try {
      const code = await reader.detect();
      if (code) onDetected(code);
    } catch (e) { /* image illisible, on continue */ }
    await new Promise((resolve) => setTimeout(resolve, reader.delay()));
  }
}

function onDetected(code) {
  const now = Date.now();
  const seen = seenCodes.get(code);
  seenCodes.set(code, now);
  if (seen !== undefined && now - seen < REPEAT_DELAY_MS) return;
  for (const [other, time] of seenCodes) if (now - time >= REPEAT_DELAY_MS) seenCodes.delete(other);
  const camera = $('#camera');
  camera.classList.add('hit');
  setTimeout(() => camera.classList.remove('hit'), 300);
  userActive();
  handleCode(code);
}

document.addEventListener('visibilitychange', syncCamera);
// rechargement, navigation, onglet gele ou decharge : la camera est rendue sans attendre le navigateur
window.addEventListener('pagehide', stopCamera);
window.addEventListener('pageshow', (event) => { if (event.persisted) syncCamera(); });
document.addEventListener('freeze', stopCamera);
document.addEventListener('resume', syncCamera);

// icones des boutons fixes de la page
for (const [id, name] of [['start-button', 'camera'], ['undo', 'undo'], ['clear', 'trash'], ['fiche-home', 'home'],
  ['login-needed-badge', 'badge'], ['login-needed-name', 'keypad'], ['login-gate-name', 'keypad'], ['problem-send', 'send'], ['revision-ok', 'check']]) {
  const button = document.getElementById(id);
  if (button) button.prepend(icon(name));
}
$('#start-button').addEventListener('click', startCamera);

// Camera reduite en vignette flottante (comme en appel video) : la liste prend tout l'ecran, le scan continue.
// Toucher la vignette remet la camera en haut. Retenu sur ce navigateur. Seulement avec le reglage « Bulle de
// scan » (decoche par defaut) : sans lui, pas de bouton ↘ et la camera reste en haut.
const STORAGE_CAM_MINI = 'qrprotec.camera-mini';
function setCameraMini(mini) {
  document.body.classList.toggle('cam-mini', mini);
  try { localStorage.setItem(STORAGE_CAM_MINI, mini ? '1' : ''); } catch (e) { /* stockage indisponible */ }
}

// reglage de l'utilisateur connecte, sinon celui du dernier connecte sur ce telephone (decoche par defaut)
function scanBubbleOn() {
  if (state.user) return webPref('scan_bubble');
  try { return localStorage.getItem(STORAGE_SCAN_BUBBLE) === '1'; } catch (e) { return false; }
}

function rememberScanBubble() {
  try { localStorage.setItem(STORAGE_SCAN_BUBBLE, scanBubbleOn() ? '1' : '0'); } catch (e) { /* ignore */ }
}

// bouton ↘ seulement avec la bulle de scan ; reglage decoche : la camera reduite revient en haut
function syncScanBubble() {
  const allowed = scanBubbleOn();
  $('#cam-mini').hidden = !allowed;
  if (!allowed && document.body.classList.contains('cam-mini')) setCameraMini(false);
}
try { setCameraMini(localStorage.getItem(STORAGE_CAM_MINI) === '1'); } catch (e) { /* stockage indisponible */ }
syncScanBubble();
$('#cam-mini').addEventListener('click', () => setCameraMini(true));
$('#camera').addEventListener('click', (event) => {
  if (!document.body.classList.contains('cam-mini') || document.body.classList.contains('home-mode')) return;
  if (event.target.closest('#start-button')) return; // demarrer la camera sans quitter la vignette
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
