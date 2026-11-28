/* QRProtec - front web : reassort d'une vérif incomplete (liste de courses a cocher, scan du reassort, reassort de
 * fin de vérif en cascade).
 */
'use strict';

// Valider une vérif ou il manque des items ouvre l'ecran de reassort (plus la popup « Valider quand même ? ») :
//  1. liste de courses (state.restock.stage 'list', plein ecran) : ce qui manque, par type, a cocher en le prenant
//     dans le stock (cases partagees entre les participants de la vérif commune, live 'restock'), et ou le trouver
//     (rangements du stock, loupe : chemin et « où le trouver » du rangement) ;
//  2. « J'ai tout le réassort » (que la liste soit cochee ou non) : retour au lecteur (stage 'scan'), on scanne chaque
//     item mis dans le lot. Vérif en cascade : un item venu du stock va d'office dans le lot du reassort qui en manque,
//     l'ecran dit lequel (restockDispatch) ; on peut aussi scanner l'etiquette du lot avant. Les items a cocher
//     (gobelets) se cochent dans la liste en les sortant du stock, puis dans le lot en les y rangeant ;
//  3. « Valider la vérif » : complete si tout est la, sinon retour a la liste. Le petit bouton rouge « Valider la vérif
//     incomplète » force la validation a tout moment.
// Vérif en cascade : le reassort d'un sous-lot peut attendre la fin de la vérif du lot global (state.restockLater,
// partage). Le sous-lot n'est pas valide, ses scans restent (partages avec les autres participants) ; a la validation
// du lot global, une seule liste pour tous ces lots, un seul passage au stock, puis chacun est valide avant le lot
// global (complet si son reassort est fait). Pour ressortir seulement son sous-lot, on y entre et on fait son
// reassort tout de suite.

let restockWhere = {}; // type -> [{id, name, kind, path, location, count, next_expiry}], null si inconnu (hors connexion)
const restockLoading = new Set();
const restockLoupes = new Set(); // types dont la loupe est ouverte

const restockListOpen = () => !$('#restock-screen').hidden;
const restockScanning = () => !!state.restock && state.restock.stage === 'scan' && !!state.lot;

// Lots du reassort de fin de vérif en cascade pas encore valides
const pendingLater = () => (inCascade() ? state.restockLater.filter((id) => lotById(id) && !state.done[id]) : []);

// Lots dont on fait le reassort : ceux du lot ou l'on validait, ou en fin de cascade les lots en attente et le lot
// global
function restockLots() {
  const restock = state.restock;
  if (!restock) return [];
  if (!restock.end) return restock.lots.filter((id) => lotById(id));
  const plan = planSession();
  const root = [...scopeOf(state.lotId)].filter((id) => !plan.intact.has(id));
  return [...new Set([...pendingLater(), ...root])];
}

// Ce qui manque a chaque lot : [{lot, rows: [{type, row, check, units, lacking}]}]. lacking : items qui manquent (en
// unites pour un type a unites, quantite pour un item a cocher). Le facultatif ne manque jamais ; un rangement du stock
// n'a pas de reassort (ses items sont ses attendus).
function restockNeeds(ids) {
  const plan = planSession();
  const result = [];
  for (const id of ids) {
    const lot = lotById(id);
    if (!lot || lot.storage || plan.intact.has(id) || state.done[id]) continue;
    const rows = [];
    for (const group of expectedGroups(lot, plan).groups) {
      if (group.optional || !(group.missing > 0)) continue;
      const units = !!(group.isGroup && group.unit);
      rows.push({ type: group.row.type, row: group.row, check: !!group.check, units,
        lacking: group.check ? group.expected : units ? group.units.missing : group.missing });
    }
    if (rows.length) result.push({ lot, rows });
  }
  return result;
}

// Liste de courses : un type une seule fois, tous lots confondus, avec ce qu'il faut dans chaque lot
function shoppingList(needs) {
  const byType = new Map();
  for (const { lot, rows } of needs) {
    for (const need of rows) {
      const row = byType.get(need.type) || { ...need, lacking: 0, lots: [] };
      row.lacking += need.lacking;
      row.lots.push({ lot, lacking: need.lacking });
      byType.set(need.type, row);
    }
  }
  return [...byType.values()];
}

// « 3 × Compresse », « 50 mL de Sérum phy (1 × Sérum 50 mL) », « 10 × Gobelets »
const needText = (need, lacking = need.lacking) => (need.check ? `${lacking} × ${need.row.type_name}`
  : isGroupRow(need.row) ? lackingText(need.row, lacking) : `${lacking} × ${reqName(need.row)}`);
// types d'items qui remplissent un attendu : lui-meme et ses sous-types
const fillTypes = (row) => [row.type, ...Object.keys(row.fills || {})];

// Vérif incomplete a valider : ouvre la liste de courses du reassort de ces lots. false s'il n'y a rien a
// reassortir (il ne manque que des items hors definition : la validation garde sa question d'avant).
function askRestock(lots, level = '') {
  if (!restockNeeds(lots).length) return false;
  if (!state.restock || state.restock.end || state.restock.level !== level) {
    state.restock = { stage: 'list', lots, level, end: false };
  } else {
    state.restock.lots = lots;
    state.restock.stage = 'list';
  }
  feedback.warn();
  save();
  render();
  return true;
}

// Fin de la vérif en cascade avec des lots au reassort de fin : une liste pour eux tous (et le lot global)
function askEndRestock() {
  state.restock = { stage: 'list', lots: [], level: state.lotId, end: true };
  feedback.warn();
  save();
  render();
}

function closeRestock() {
  state.restock = null;
  save();
  render();
}

// Cases de la liste de courses : partagees avec les autres participants (le serveur garde la liste de la vérif
// commune) ; hors connexion, gardees ici et renvoyees a la connexion (restockLocal)
function sendRestock(message) {
  if (!live.restock(message)) state.restockLocal = true;
}

function toggleShopping(type) {
  if (state.restockChecked[type]) delete state.restockChecked[type];
  else state.restockChecked[type] = state.user ? state.user.prenom : state.declaredName || state.guest || 'moi';
  sendRestock({ check: type, on: !!state.restockChecked[type] });
  save();
  render();
}

function setLater(id, on) {
  state.restockLater = state.restockLater.filter((other) => other !== id);
  if (on) state.restockLater.push(id);
  sendRestock({ later: id, on });
}

// Etat de la liste partage par le serveur (teamChanged)
function restockTeamChanged(shared) {
  if (state.restockLocal) {
    state.restockLocal = false;
    for (const type of Object.keys(state.restockChecked)) live.restock({ check: type, on: true });
    for (const id of state.restockLater) live.restock({ later: id, on: true });
    return;
  }
  state.restockChecked = { ...(shared.checked || {}) };
  state.restockLater = (shared.later || []).filter((id) => typeof id === 'string');
}

// Ou trouver chaque type dans le stock (POST stock/where/), une fois par type et par vérif
async function loadWhere(types) {
  const wanted = [...new Set(types)].filter((type) => !(type in restockWhere) && !restockLoading.has(type));
  if (!wanted.length) return;
  wanted.forEach((type) => restockLoading.add(type));
  try {
    const result = await api('stock/where/', { types: wanted });
    Object.assign(restockWhere, result.types || {});
  } catch (e) {
    for (const type of wanted) restockWhere[type] = null;
  } finally {
    wanted.forEach((type) => restockLoading.delete(type));
    renderRestock();
  }
}

// Rangements du stock d'un attendu (ses sous-types compris), le premier a sortir en tete
function wherePlaces(row) {
  const names = Object.fromEntries((row.subtypes || []).map((sub) => [sub.type, sub.type_name]));
  const places = [];
  let unknown = false;
  for (const type of fillTypes(row)) {
    if (!(type in restockWhere)) { unknown = unknown || restockLoading.has(type) ? 'loading' : unknown; continue; }
    if (restockWhere[type] === null) { unknown = unknown || 'offline'; continue; }
    for (const place of restockWhere[type]) places.push({ ...place, type, type_name: type === row.type ? '' : names[type] || '' });
  }
  places.sort((a, b) => (a.next_expiry === null) - (b.next_expiry === null)
    || (a.next_expiry || '').localeCompare(b.next_expiry || '') || b.count - a.count);
  return { places, unknown };
}

const placeName = (place) => (place.type_name ? `${place.name} (${place.type_name})` : place.name);

function whereLines(need) {
  if (need.check) return [el('div', { class: 'restock-where' }, 'À cocher, sans étiquette : prenez-en au stock')];
  const { places, unknown } = wherePlaces(need.row);
  if (!places.length) {
    return [el('div', { class: 'restock-where none' }, unknown === 'loading' ? 'Recherche dans le stock…'
      : unknown ? 'Emplacement dans le stock inconnu (hors connexion)' : 'Plus rien dans le stock : prévenez le TECH')];
  }
  const short = el('div', { class: 'restock-where' }, `📦 ${placeName(places[0])} · ${places[0].count} dispo`,
    places.length > 1 ? el('span', { class: 'more' }, ` · ou ${places.slice(1, 3).map(placeName).join(', ')}`
      + (places.length > 3 ? '…' : '')) : '');
  if (!restockLoupes.has(need.type)) return [short];
  return [short, el('ul', { class: 'restock-places' }, ...places.map((place) => el('li', {},
    el('div', { class: 'name' }, placeName(place), place.kind ? el('span', { class: 'kind' }, ` · ${place.kind}`) : ''),
    place.path.length ? el('div', { class: 'sub' }, `Dans : ${['Stock', ...place.path].join(' › ')}`) : '',
    ...(place.hints || []).map((hint) => el('div', { class: 'where-hint' }, `📍 ${hint.name} : ${hint.location}`)),
    el('div', { class: 'sub' }, `${place.count} dispo` + (place.next_expiry ? ` · le plus tôt périmé : ${fmtDate(place.next_expiry)}` : '')))))];
}

function shoppingRow(need, multi) {
  const who = state.restockChecked[need.type];
  const checked = !!who;
  const toggle = () => toggleShopping(need.type);
  const loupe = !need.check ? el('button', { type: 'button', class: 'restock-loupe' + (restockLoupes.has(need.type) ? ' on' : ''),
    'aria-label': 'Où le trouver dans le stock ?', title: 'Où le trouver dans le stock ?',
    onclick: (event) => {
      event.stopPropagation();
      if (restockLoupes.has(need.type)) restockLoupes.delete(need.type); else restockLoupes.add(need.type);
      renderRestock();
    } }, '🔍') : null;
  return el('li', { class: 'restock-row' + (checked ? ' checked' : '') },
    el('button', { type: 'button', class: 'restock-box', role: 'checkbox', 'aria-checked': String(checked),
      'aria-label': `Pris au stock : ${needText(need)}`, onclick: toggle }, checked ? '☑' : '☐'),
    el('div', { class: 'main', onclick: toggle },
      el('div', { class: 'name' }, needText(need)),
      multi || checked ? el('div', { class: 'sub' }, [
        multi ? need.lots.map((row) => `${row.lot.name} : ${row.lacking}`).join(' · ') : '',
        checked && who !== true ? `pris par ${who}` : ''].filter(Boolean).join(' · ')) : '',
      ...whereLines(need)),
    loupe);
}

// Liste de courses en plein ecran (stage 'list'), bandeau du reassort au lecteur (stage 'scan')
function renderRestock() {
  const restock = state.restock;
  if (restock && !state.lot) state.restock = null;
  const open = !!state.restock && state.restock.stage === 'list';
  if ($('#restock-screen').hidden === open) {
    $('#restock-screen').hidden = !open;
    document.body.classList.toggle('restock-open', open);
    syncCamera();
  }
  renderRestockBanner();
  if (!open) return;
  const needs = restockNeeds(restockLots());
  const list = shoppingList(needs);
  loadWhere(list.filter((need) => !need.check).flatMap((need) => fillTypes(need.row)));
  const level = lotById(restock.level) || state.lot;
  const multi = needs.length > 1;
  const later = pendingLater();
  const taken = list.filter((need) => state.restockChecked[need.type]).length;
  $('#restock-body').replaceChildren(
    el('h2', {}, `🛒 Réassort ${restock.end ? 'de la vérif de ' + state.lot.name : deName(level.name)}`),
    el('p', { class: 'restock-intro' }, list.length
      ? `${nb(list.length, 'chose manque', 'choses manquent')}${multi ? ` dans ${nb(needs.length, 'lot')}` : ''}. `
        + 'Allez les chercher au stock en cochant au fur et à mesure, puis scannez-les en les mettant dans le lot.'
      : 'Plus rien ne manque : validez la vérif.'),
    restock.end && later.length ? el('p', { class: 'restock-intro' },
      `Réassort gardé pour la fin : ${later.map((id) => lotById(id).name).join(', ')}.`) : '',
    list.length ? el('ul', { class: 'restock-list' }, ...list.map((need) => shoppingRow(need, multi))) : '',
    list.length ? el('p', { class: 'restock-count' }, `${taken} / ${list.length} ${pl(taken, 'pris', 'pris')} au stock`) : '');
  setLabel($('#restock-have'), 'restock', list.length ? "J'ai tout le réassort" : 'Valider la vérif');
  const canLater = inCascade() && !restock.end && restock.level && restock.level !== state.lotId;
  $('#restock-later').hidden = !canLater;
  if (canLater) $('#restock-later').textContent = `Faire le réassort à la fin de la vérif de ${state.lot.name}`;
  $('#restock-force').textContent = restock.end ? 'Valider la vérif incomplète et terminer' : 'Valider la vérif incomplète';
  $('#restock-force').hidden = !list.length;
}

// Bandeau du reassort au lecteur : ce qu'il reste a ranger, lot par lot (dispatch)
function renderRestockBanner() {
  const banner = $('#restock-banner');
  if (!restockScanning()) { banner.hidden = true; return; }
  banner.hidden = false;
  const needs = restockNeeds(restockLots());
  const multi = needs.length > 1 || state.restock.end;
  const lines = needs.map(({ lot, rows }) => el('li', {},
    multi ? el('b', {}, lot.name + ' : ') : '',
    ...rows.map((need, index) => [index ? ', ' : '', need.check
      ? el('button', { type: 'button', class: 'text-link', title: 'Rangé dans le lot : cochez-le',
        onclick: () => toggleCheck(lot, need.row) }, `☐ ${needText(need)}`)
      : needText(need)]).flat()));
  banner.replaceChildren(
    el('div', { class: 'restock-banner-head' },
      el('div', { class: 'main' },
        el('div', { class: 'name' }, needs.length ? '🛒 Réassort : scannez chaque item mis dans le lot' : '✅ Réassort complet'),
        el('div', { class: 'sub' }, !needs.length ? 'Tout est là : validez la vérif.'
          : multi ? "Un item du stock va d'office dans le lot qui en manque : l'écran vous dit lequel." : 'Il reste :')),
      el('button', { type: 'button', class: 'secondary', onclick: () => { state.restock.stage = 'list'; save(); render(); } }, 'Liste')),
    needs.length ? el('ul', { class: 'restock-dispatch' }, ...lines) : '',
    needs.length ? el('button', { type: 'button', class: 'danger small restock-force', onclick: () => restockValidate(true) },
      state.restock.end ? 'Valider la vérif incomplète et terminer' : 'Valider la vérif incomplète') : '');
}

// Item scanne pendant le reassort d'une vérif en cascade : venu du stock (dans aucun lot de la vérif), il va dans le
// lot du reassort qui en manque (place), meme si l'on n'a pas scanne son etiquette. Renvoie ce lot, null sinon.
function restockDispatch(entry) {
  if (!restockScanning() || !inCascade() || entry.kind !== 'item' || entry.error || entry.expired || !entry.info) return null;
  const targets = restockLots();
  const inside = (id) => targets.some((top) => isUnder(id, top));
  const type = entry.iid.slice(0, 6);
  const fills = (need) => !need.check && fillTypes(need.row).includes(type);
  // lot ou l'on est : s'il est du reassort et en manque, l'item y reste
  const here = restockNeeds(targets.filter((id) => isUnder(id, levelId())));
  let lot = here.find(({ rows }) => rows.some(fills));
  if (!lot) {
    // sinon le lot que lui donne la repartition de la vérif, ou le premier qui en manque
    const planned = lotById(planSession().target[entry.iid]);
    lot = planned && inside(planned.id) ? { lot: planned } : restockNeeds(targets).find(({ rows }) => rows.some(fills));
  }
  if (!lot) return null;
  entry.place = lot.lot.id;
  feedback.good();
  showInfo('lot', `➜ À mettre dans ${lot.lot.name}`, `${entry.info.type_name} : rangez-le dans ${lot.lot.name}.`,
    "Scannez l'étiquette d'un lot pour en choisir un autre.");
  return lot.lot;
}

// « Valider la vérif » pendant le reassort (force : petit bouton rouge, incomplet)
async function restockValidate(force = false) {
  const restock = state.restock;
  if (!restock) return;
  if (!force && restockNeeds(restockLots()).length) {
    restock.stage = 'list';
    feedback.warn();
    toast('Il manque encore du matériel : voici ce qui reste.');
    save();
    render();
    return;
  }
  restock.stage = 'scan';
  if (!inCascade()) { await validate(false, false, true); return; }
  if (restock.end) {
    state.path = [];
    state.current = state.lotId;
  } else if (restock.level && lotById(restock.level) && levelId() !== restock.level) {
    enterLevel(restock.level);
  }
  await validateStep(false, true);
}

// Lots au reassort de fin, du plus profond au moins profond : chacun valide (complet si son reassort est fait), avant
// le lot global. false si l'un est refuse.
async function validateLater() {
  const ids = pendingLater().sort((a, b) => chainTo(b).length - chainTo(a).length);
  for (const id of ids) {
    if (state.done[id] || !lotById(id)) continue;
    enterLevel(id);
    await validateStep(false, true);
    if (!state.done[id]) {
      state.path = [];
      state.current = state.lotId;
      render();
      return false;
    }
    setLater(id, false);
  }
  state.path = [];
  state.current = state.lotId;
  save();
  return true;
}

// « Faire le réassort à la fin de la vérif de VPS » : le sous-lot reste a valider, ses scans gardes ; on remonte
function restockAtEnd() {
  const restock = state.restock;
  if (!restock || !restock.level) return;
  const lot = lotById(restock.level);
  setLater(restock.level, true);
  state.restock = null;
  if (levelId() === restock.level) { state.path.pop(); state.current = levelId(); }
  feedback.info();
  showInfo('lot', `🛒 ${lot ? lot.name : 'Lot'} : réassort à la fin`, `Le réassort se fera à la validation de ${state.lot.name}, `
    + 'en une seule fois pour tous les lots.', 'Continuez la vérif.');
  save();
  render();
}

// Quitter une vérif en cascade avec des lots au reassort de fin : les valider incomplets, ou les laisser non
// verifies. false : on reste.
async function quitWithLater() {
  const later = pendingLater();
  if (!later.length) return true;
  const names = later.map((id) => lotById(id).name).join(', ');
  const choice = await askDialog({ title: 'Réassort pas fait', text: `${names} ${pl(later.length, 'attend', 'attendent')} `
    + 'le réassort de fin de vérif.', choices: [
    { label: `Les valider incomplets`, value: 'validate', cls: 'warning' },
    { label: 'Quitter sans les valider', value: 'quit', cls: 'danger' }] });
  if (!choice) return false;
  if (choice === 'validate') {
    state.restock = null;
    if (!await validateLater()) return false;
  }
  return true;
}

$('#restock-have').addEventListener('click', () => {
  if (!state.restock) return;
  if (!restockNeeds(restockLots()).length) { restockValidate(false); return; }
  state.restock.stage = 'scan';
  feedback.info();
  showInfo('lot', '🛒 Réassort', 'Scannez chaque item en le mettant dans le lot.',
    state.restock.end ? "Pas besoin de scanner l'étiquette du lot : l'écran dit où ranger chaque item." : '');
  switchTab('todo');
  save();
  render();
});
$('#restock-later').addEventListener('click', restockAtEnd);
$('#restock-force').addEventListener('click', () => restockValidate(true));
$('#restock-back').addEventListener('click', closeRestock);
