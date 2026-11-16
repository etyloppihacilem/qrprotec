/* QRProtec - front web : retours d'un scan (son, vibration, flash). */
'use strict';

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

// Reglages du web de l'utilisateur (models.WEB_PREFS, modifiables aussi par un admin au poste) : valeur par defaut
// sans utilisateur connecte, ou si la session date d'avant le reglage
const WEB_PREF_DEFAULTS = { verif_help_hidden: false, scan_bubble: false };
function webPref(name) {
  const prefs = state.user && state.user.web_prefs;
  return prefs && name in prefs ? !!prefs[name] : WEB_PREF_DEFAULTS[name];
}

async function setWebPrefs(prefs) {
  const body = await api('me/prefs/', { user: credentials(), web_prefs: prefs });
  if (state.user) state.user.web_prefs = body.web_prefs;
  save();
  render();
}
