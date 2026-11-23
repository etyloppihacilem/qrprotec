/* QRProtec - front web : outils (accords, dates, elements du DOM, icones). */
'use strict';

// Outils

// Accord en nombre : singulier pour 0 et 1, pluriel au-dela. Le pluriel par defaut ajoute un s.
function pl(n, word, plural) { return n > 1 ? (plural || word + 's') : word; }
// « 3 items », « 1 item », « 0 item ».
function nb(n, word, plural) { return `${n} ${pl(n, word, plural)}`; }
// « de » devant un nom de lot : « de Sac de soin », « d'Armoire 1 », « du Sac O2 » (pour « Le Sac O2 »), « des … »,
// « de la … », « de l'… ». Meme regle que de_name (french.hpp, views/public.py).
function deName(name) {
  const lower = name.toLowerCase();
  if (lower.startsWith('les ')) return 'des ' + name.slice(4);
  if (lower.startsWith('le ')) return 'du ' + name.slice(3);
  if (lower.startsWith('la ')) return 'de la ' + name.slice(3);
  if (lower.startsWith("l'")) return "de l'" + name.slice(2);
  return /^[aeiouyàâéèêëîïôöùûü]/i.test(name) ? "d'" + name : 'de ' + name;
}
// « de A », « de A ou de B », « de A, de B ou de C »
function deNames(names) {
  return names.map((name, i) => (i === 0 ? '' : i === names.length - 1 ? ' ou ' : ', ') + deName(name)).join('');
}

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

// Date et heure d'une action passee (verif, signalement...) : « aujourd'hui » / « hier » quand c'est le cas,
// l'heure toujours visible ; long ajoute le jour de la semaine quand la date est ecrite en entier (fiches).
// Les dates seules (fmtDate) restent reservees aux peremptions et expirations.
function fmtDateTime(value, { long = false, on = false } = {}) {
  const d = value ? new Date(value) : null;
  if (!d || isNaN(d)) return '–';
  const time = ' à ' + d.toLocaleTimeString('fr-FR', { hour: '2-digit', minute: '2-digit' });
  const day = new Date(d); day.setHours(0, 0, 0, 0);
  const diff = Math.round((today() - day) / 86400000);
  if (diff === 0) return "aujourd'hui" + time;
  if (diff === 1) return 'hier' + time;
  if (diff === -1) return 'demain' + time; // fin de validite (carte d'invite)
  const date = (long ? d.toLocaleDateString('fr-FR', { weekday: 'long' }) + ' ' : '') + d.toLocaleDateString('fr-FR');
  return (on ? 'le ' : '') + date + time;
}

// Duree à la seconde : « 45 s », « 12 min 05 s », « 1 h 07 min 05 s »
function fmtDuration(seconds) {
  const total = Math.max(0, Math.round(seconds));
  const h = Math.floor(total / 3600);
  const m = Math.floor((total % 3600) / 60);
  const s = total % 60;
  const two = (n) => String(n).padStart(2, '0');
  if (h) return `${h} h ${two(m)} min ${two(s)} s`;
  if (m) return `${m} min ${two(s)} s`;
  return `${s} s`;
}

// Date d'une liste de materiel (jour du poste) : « 📅 aujourd'hui », « demain », « hier », sinon « lun. 23/11/2026 »
function fmtListDate(value) {
  const d = parseDate(value);
  if (!d) return '';
  const diff = Math.round((d - today()) / 86400000);
  const day = diff === 0 ? "aujourd'hui" : diff === 1 ? 'demain' : diff === -1 ? 'hier'
    : d.toLocaleDateString('fr-FR', { weekday: 'short' }) + ' ' + d.toLocaleDateString('fr-FR');
  return '📅 ' + day;
}

// Valeur d'un champ date (AAAA-MM-JJ) pour aujourd'hui, dans le fuseau du navigateur
function isoToday() {
  const d = today();
  return `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, '0')}-${String(d.getDate()).padStart(2, '0')}`;
}

// Meme chose apres un verbe : « scellé le 12/10/2026 à 9:00 », « scellé aujourd'hui à 9:00 »
const fmtOn = (value, opts = {}) => fmtDateTime(value, { ...opts, on: true });

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

// Icones des boutons : fichiers web/icons/<name>.svg (SVG au trait), peints dans la couleur du texte du bouton
// (masque CSS, voir .icon dans app.css)
function icon(name) {
  const span = el('span', { class: 'icon', 'aria-hidden': 'true' });
  span.style.setProperty('--icon', `url("${new URL(`web/icons/${name}.svg`, document.baseURI).href}")`);
  return span;
}

// Code d'etat d'un lot, le meme partout (web et poste) : petite bulle a fond blanc, contour et icone de la couleur de
// l'etat. Coche verte : valide ; fleche en cercle avec une horloge, orange : vérif recommandee ; croix rouge : a
// traiter (perimes, incomplet, signale...). Seule la ou il y a peu de place ; ailleurs elle precede le texte de l'etat.
const STATE_WORDS = { ok: 'Valide', warn: 'Vérif recommandée', bad: 'À traiter' };
function stateBadge(kind, title = '') {
  const known = STATE_WORDS[kind] ? kind : 'bad';
  return el('span', { class: 'state-badge ' + known, role: 'img', 'aria-label': title || STATE_WORDS[known], title: title || STATE_WORDS[known] },
    icon('state-' + known));
}

// Texte d'un bouton precede de son icone (name vide : texte seul)
function setLabel(button, name, text) {
  button.replaceChildren(...(name ? [icon(name)] : []), text);
}
