/* QRProtec - front web : vérif en cascade d'un lot global, vérif a plusieurs et repartition des items
 * entre les lots (planSession).
 */
'use strict';

// Vérif en cascade : un lot global qui contient des sous-lots (ex : VPS > armoires > sacs) se verifie lot par lot.
// On entre dans un sous-lot en scannant son etiquette (ou un de ses items), on fait sa vérif, on la valide : elle est
// enregistree tout de suite (services.perform_verif, "exclude" et "keys") et l'on revient au lot qui le contient, ou
// il passe en vert. Les sous-lots non faits restent tels quels, sans etre comptes manquants. A plusieurs, chacun fait
// ses sous-lots dans la meme vérif du lot global (live.py).

const inCascade = () => !!state.lot && !state.extra.length && (state.lot.descendants || []).length > 0;
const lotById = (id) => (id ? sessionLots().find((lot) => lot.id === id) || null : null);
const realParentOf = (lot) => lot.parent || (lot.path && lot.path.length ? lot.path[lot.path.length - 1].id : null);
// Lot cache dans la vérif en cascade (Lots.hidden_in_verif, ex : le B+ du VPS) : ses sous-lots apparaissent
// directement dans son parent, on n'y entre jamais (meme regle que le poste, App::verif_parent)
function parentIdOf(lot) {
  let id = realParentOf(lot);
  for (let parent = lotById(id), guard = 0; parent && parent.hidden && guard < 50; parent = lotById(id), guard += 1) id = realParentOf(parent);
  return id;
}
// lot ou l'on entre a la place d'un lot cache : le premier lot affiche qui le contient
const visibleId = (id) => { const lot = lotById(id); return lot && lot.hidden ? parentIdOf(lot) || state.lotId : id; };
const levelId = () => (state.path.length ? state.path[state.path.length - 1] : state.lotId);
const levelLot = () => lotById(levelId()) || state.lot;
const childrenOf = (id) => sessionLots().filter((lot) => !lot.hidden && parentIdOf(lot) === id);
// Sous-lots d'un lot nommes : « de Sac de soin ou de Sac O2 » (jusqu'a 3), sinon « d'un des lots qu'il contient ».
// Les messages disent le nom des lots, pas « lot ou sous-lot » (meme regle que le poste, App::verif_children_de).
function childrenDe(id) {
  const subs = childrenOf(id);
  return subs.length > 3 ? "d'un des lots qu'il contient" : deNames(subs.map((lot) => lot.name));
}
// Etiquette a scanner pour valider : « de B+ (ou celle de Sac de soin ou de Sac O2) ». Un lot de referentiel
// (B+, services.virtual_lot) n'a pas d'etiquette : « de Sac de soin ou de Sac O2 »
function labelTarget(lot) {
  const subs = childrenDe(lot.id);
  if (lot.virtual && subs) return subs;
  return deName(lot.name) + (subs ? ` (ou celle ${subs})` : '');
}
// vérif en cours dans la cascade : un lot ouvert, un lot valide ou une etiquette cle scannee
const cascadeActive = () => inCascade() && (state.path.length > 0 || state.entered.length > 0
  || Object.keys(state.done).length > 0 || Object.keys(state.keys).length > 0);

function resetCascade() {
  state.path = []; state.done = {}; state.keys = {}; state.entered = []; state.reopened = {}; state.startedIn = ''; state.labels = [];
  state.openSeals = []; state.full = false; state.fullBy = ''; state.fullHidden = false;
}

// Facultatif (casques du VPS, sous-lot facultatif) : le lot se valide sans, sauf en « vérif complète » (bouton des
// admins), ou tout redevient obligatoire (services.perform_verif strict)
const rowNeed = (row) => (row.optional && state.full ? row.expected || 0 : row.required);
const itemOptional = (item) => !!item.optional && !state.full;
const lotOptional = (lot) => !!lot.optional && !state.full;
// vérif complete : la ligne telle que le serveur la compterait (facultatif obligatoire)
const strictRow = (row) => (row.optional && state.full ? { ...row, optional: false, required: row.expected || 0 } : row);
// Facultatif dans le lot verifie ou un de ses sous-lots (en descendant). En cascade, c'est le lot en cours : un
// sous-lot n'affiche pas le bandeau parce que son lot parent contient du facultatif (demande d'Hippolyte).
function sessionHasOptional() {
  const scope = inCascade() ? sessionLots().filter((lot) => isUnder(lot.id, levelId())) : sessionLots();
  return scope.some((lot) => lot.optional || (lot.requirements || []).some((row) => row.optional));
}

// Bouton de la gestion et des admins en vérif : « Rendre la vérif complète » (le facultatif devient obligatoire), ou l'annuler
function fullVerifRow() {
  if (!state.lot || state.fullHidden || !sessionHasOptional() || !(canSeeStock() || state.full)) return null;
  // vérif commune : partage avec tous les participants (le serveur garde le reglage)
  const toggle = () => { state.full = !state.full; state.fullBy = ''; state.fullLocal = true; live.setFull(state.full); save(); render(); };
  // « x » de la gestion et des admins : ferme le bandeau pour cette vérif seulement (sur cette page, le reglage ne change pas)
  const hide = () => { state.fullHidden = true; save(); render(); };
  return el('li', { class: 'full-verif' + (state.full ? ' on' : '') },
    el('div', { class: 'main' },
      el('div', { class: 'name' }, state.full ? 'Vérif complète' + (state.fullBy ? ` (par ${state.fullBy})` : '')
        : 'Matériel facultatif dans cette vérif'),
      el('div', { class: 'sub' }, state.full ? 'Le facultatif est obligatoire.'
        : 'Validable sans lui.')),
    canSeeStock() ? el('button', { type: 'button', class: state.full ? 'secondary' : 'warning', onclick: toggle },
      state.full ? 'Revenir au facultatif' : 'Rendre la vérif complète') : null,
    canSeeStock() ? el('button', { type: 'button', class: 'full-verif-close', 'aria-label': 'Fermer', title: 'Fermer pour cette vérif',
      onclick: hide }, '✕') : null);
}

// lot id dans le lot top (ou top lui-meme)
function isUnder(id, top) {
  for (let lot = lotById(id), guard = 0; lot && guard < 50; lot = lotById(parentIdOf(lot)), guard += 1) {
    if (lot.id === top) return true;
  }
  return false;
}

const subtreeIds = (top) => sessionLots().filter((lot) => isUnder(lot.id, top)).map((lot) => lot.id);

// Sous-lots du lot global jusqu'au lot id (exclu le lot global)
function chainTo(id) {
  const chain = [];
  for (let lot = lotById(id), guard = 0; lot && lot.id !== state.lotId && guard < 50; lot = lotById(parentIdOf(lot)), guard += 1) {
    chain.unshift(lot.id);
  }
  return chain;
}

// Entrer dans un lot du lot global : il devient le lot en cours (les items venus du stock y sont ranges). Un lot deja
// valide pendant la vérif est rouvert : on le refait.
function enterLevel(id) {
  id = visibleId(id); // lot cache : on entre dans le lot affiche qui le contient
  const lot = lotById(id);
  if (!lot) return;
  state.path = id === state.lotId ? [] : chainTo(id);
  state.current = id;
  state.askPlace = false;
  if (!state.entered.includes(id)) state.entered.push(id);
  if (state.done[id]) state.reopened[id] = state.done[id];
  delete state.done[id];
  switchTab('todo');
}

// Revenir au lot qui contient le lot en cours, sans le valider (ses scans restent). Rien scanne dedans : il n'est pas
// « commence », et un lot valide qu'on avait rouvert (« Le refaire ») redevient valide, comme chez les autres
// participants de la vérif commune.
function goUp() {
  const id = levelId();
  if (id !== state.lotId && !state.done[id] && !scopeEntries(scopeOf(id)).length
    && !state.entered.some((other) => other !== id && isUnder(other, id))) {
    state.entered = state.entered.filter((other) => other !== id);
    const before = state.reopened[id] || (live.team && live.team.done && live.team.done[id]);
    if (before) state.done[id] = before;
  }
  delete state.reopened[id];
  state.path.pop();
  state.current = levelId();
  save();
  render();
}

// Bouton « ← parent » de la vérif en cascade : le lot en cours est validé en le quittant, complet ou non (les non
// scannés passent manquants). Sans rien scanné dedans, on remonte simplement. Incomplet alors qu'un autre
// participant y est encore, ou étiquette du lot manquante : on remonte sans valider, scans gardés.
async function leaveUp() {
  if (state.busy || levelId() === state.lotId) return;
  const level = levelLot();
  const scope = scopeOf(level.id);
  if (!scopeEntries(scope).length) { goUp(); return; }
  const complete = levelComplete();
  if (!complete && othersLevels()[level.id]) {
    toast(`${level.name} n'est pas validé : un autre participant y est encore (scans gardés).`);
    goUp();
    return;
  }
  const missing = blockers(false);
  if (missing.length) {
    feedback.warn();
    toast(`${level.name} n'est pas validé (${missing.join(', ')}) : scans gardés.`, true);
    goUp();
    return;
  }
  await validateStep(false, true);
}

// Lots ou se trouvent les autres participants de la vérif commune : {id du lot: [noms]}
function othersLevels() {
  const result = {};
  for (const row of live.others()) {
    if (row.connected && row.level && row.level.id) (result[row.level.id] = result[row.level.id] || []).push(row.who || 'quelqu\'un');
  }
  return result;
}

// Lot commence : on y est entre, ou des scans y ont ete faits (le lot ou un de ses sous-lots)
function touched(id) {
  const ids = new Set(subtreeIds(id));
  return state.entered.some((lotId) => ids.has(lotId))
    || allScanned().some((entry) => !entry.error && ids.has(entry.place || state.lotId));
}

// Sous-lots du lot top laisses de cote a sa validation (avec leurs sous-lots) : deja valides, pas commences, ou
// en cours chez un autre participant
function excludedFor(top) {
  const others = othersLevels();
  const mine = levelId();
  const result = [];
  const walk = (id) => {
    for (const child of childrenOf(id)) {
      const ids = subtreeIds(child.id);
      const busy = ids.some((lotId) => others[lotId]) && !ids.includes(mine);
      if (state.done[child.id] || busy || !touched(child.id)) result.push(child.id);
      else walk(child.id);
    }
  };
  walk(top);
  return result;
}

// Lots verifies a la validation du lot top : lui et ses sous-lots qui ne sont pas laisses de cote
function scopeOf(top, exclude = excludedFor(top)) {
  const out = new Set(exclude.flatMap(subtreeIds));
  return new Set(subtreeIds(top).filter((id) => !out.has(id)));
}

// Lot qui ne fait que regrouper des sous-lots (rien d'attendu, rien dedans)
const ownContent = (lot) => !!((lot.requirements || []).length || (lot.items || []).length || (lot.missing_items || []).length);

// Etat actuel d'un sous-lot pas encore fait pendant la vérif (lot de regroupement : ses sous-lots)
function childStatus(lot) {
  if (ownContent(lot) || lot.storage) return lotStatus(lot);
  const subs = sessionLots().filter((sub) => sub.id !== lot.id && isUnder(sub.id, lot.id) && (ownContent(sub) || sub.storage));
  const bad = subs.filter((sub) => !lotStatus(sub).ok).length;
  return bad ? { ok: false, kind: 'bad', label: `✘ ${nb(bad, 'sous-lot')} à traiter sur ${subs.length}` }
    : { ok: true, kind: 'ok', label: subs.length ? `✔ Sous-lots tous valides (${subs.length})` : 'Vide' };
}

// Etiquette privee scannee : le serveur dit si sa cle vaut encore (ancienne etiquette, cle expiree)
async function keyAccepted(id, key) {
  try {
    const lot = await api(`lots/${encodeURIComponent(id)}/?key=${encodeURIComponent(key)}`);
    if (lot.key_check === 'valid') return true;
    feedback.bad();
    showInfo('bad', 'Étiquette clé refusée', lot.key_error);
    return false;
  } catch (e) {
    // lot introuvable : signale ensuite par loadLot. Hors connexion : acceptee, le serveur la controle a l'envoi
    return true;
  }
}

// Meme texte que Lots.key_hint cote serveur et key_hint dans l'app
function keyHint(lot) {
  const location = lot && lot.key_location;
  return "L'étiquette clé est un QR code rangé à l'intérieur du lot" + (location ? ` (${location}).` : '.');
}

// Lots envoyes au serveur avec leur cle ; la cle d'un lot couvre ses sous-lots
function sessionEntries() {
  return [{ id: state.lotId, key: state.lotKey }, ...state.extra.map(({ id, key }) => ({ id, key })),
    // vérif en cascade : etiquettes cles des sous-lots scannees
    ...Object.entries(state.keys).filter(([id, key]) => key && id !== state.lotId).map(([id, key]) => ({ id, key }))];
}

// Lots de la vérif sans etiquette cle : une seule etiquette cle d'un des lots de la vérif (un sous-lot suffit, ex :
// un rangement pour le stock) valide toute la vérif, comme cote serveur. Vérif à plusieurs : la cle d'un lot
// scannee par un autre participant est gardee par le serveur (keyed).
function keyMissing() {
  // avec un badge (ou sans nom declare possible), plus d'etiquette cle : labelMissing suffit (views.verif_lots)
  if (state.user || !state.declaredAllowed) return [];
  const keyed = new Set([...live.keyed(), ...(state.lotKey ? [state.lotId] : []),
    ...state.extra.filter((entry) => entry.key).map((entry) => entry.id),
    ...Object.keys(state.keys).filter((id) => state.keys[id])]);
  if (keyed.size) return [];
  return inCascade() ? [levelLot()] : [state.lot, ...state.extra.map((entry) => entry.lot)].filter(Boolean);
}

const keyOk = () => !keyMissing().length;

// Lots dont l'etiquette (publique ou cle) a ete scannee pendant la vérif, par nous ou un autre participant : une
// etiquette cle compte, comme l'etiquette du scelle d'un lot (scelle intact)
function labeledIds() {
  return new Set([...state.labels, ...live.labeled(), ...live.keyed(), ...(state.lotKey ? [state.lotId] : []),
    ...state.extra.filter((entry) => entry.key).map((entry) => entry.id),
    ...Object.keys(state.keys).filter((id) => state.keys[id]),
    ...allScanned().filter((entry) => entry.kind === 'seal' && !entry.error).map((entry) => entry.iid)]);
}

// Lot a valider dont aucune etiquette n'a ete scannee (null si au moins une l'a ete) : celle du lot, d'un de ses
// sous-lots ou d'un lot qui le contient, pour ne pas valider la vérif d'un autre lot. Meme regle que le serveur
// (views.require_label) et le poste (App::verif_label_ok).
function labelMissing() {
  const scanned = [...labeledIds()];
  const tops = inCascade() ? [levelLot()] : [state.lot, ...state.extra.map((entry) => entry.lot)].filter(Boolean);
  const found = tops.some((top) => scanned.some((id) => id === top.id || isUnder(id, top.id) || isUnder(top.id, id)));
  return found ? null : tops[0] || null;
}

const labelOk = () => !labelMissing();

// Ligne de la fiche et du lecteur : ce qu'il reste a scanner pour pouvoir valider
function labelLine(lot) {
  if (!keyOk()) return '🔒 Scannez l\'étiquette clé pour pouvoir valider. ' + keyHint(lot);
  if (!labelOk()) return `🏷️ Scannez l'étiquette ${labelTarget(labelMissing())} pour pouvoir valider.`;
  return state.lotKey ? '🔑 Étiquette clé scannée' : '🏷️ Étiquette du lot scannée';
}

// Lots de la vérif dont le scelle a ete constate intact (etiquette du scelle scannee), avec leurs sous-lots : ni
// attendus ni ouverts (services.perform_verif). Un lot dont un item a ete scanne a ete ouvert.
function intactLots() {
  const lots = sessionLots();
  const parentOf = Object.fromEntries(lots.map((lot) => [lot.id,
    lot.parent || (lot.path && lot.path.length ? lot.path[lot.path.length - 1].id : null)]));
  const under = (id, top) => {
    for (let current = id; current; current = parentOf[current]) if (current === top) return true;
    return false;
  };
  const done = scannedIids();
  const result = new Set();
  for (const entry of allScanned()) {
    if (entry.kind !== 'seal' || entry.error) continue;
    const subtree = lots.filter((lot) => under(lot.id, entry.iid));
    const opened = subtree.some((lot) => [...(lot.items || []), ...(lot.missing_items || [])].some((item) => done.has(item.iid)));
    if (!opened) for (const lot of subtree) result.add(lot.id);
  }
  return result;
}

// Lot scelle de la vérif dont le scelle n'est pas encore constate : sa vérif, c'est scanner l'etiquette du scelle,
// sans l'ouvrir. La liste ne montre alors que cette etiquette (sealRows). Plus le cas quand un item du lot a ete scanne
// (il a ete ouvert), quand on y est deja entre plus bas, ou quand on a choisi d'en verifier le contenu.
function sealWaiting(lot) {
  if (!lot || !lot.is_sealed || state.openSeals.includes(lot.id) || state.done[lot.id]) return false;
  const ids = new Set(subtreeIds(lot.id));
  if (allScanned().some((entry) => entry.kind === 'seal' && !entry.error && ids.has(entry.iid))) return false;
  if (state.entered.some((id) => id !== lot.id && ids.has(id))) return false;
  const done = scannedIids();
  const opened = sessionLots().filter((sub) => ids.has(sub.id))
    .some((sub) => [...(sub.items || []), ...(sub.missing_items || [])].some((item) => done.has(item.iid)));
  return !opened && !allScanned().some((entry) => !entry.error && entry.kind !== 'seal' && inCascade()
    && ids.has(entry.place || state.lotId));
}

// Liste « à scanner » d'un lot scelle : une seule ligne, l'etiquette du scelle. Scelle ouvert : l'etiquette
// d'ouverture rangee a l'interieur le dit au TECH, puis on verifie le contenu.
function sealRows(lot) {
  const seal = `Scellé${lot.seal_number ? ' n°' + lot.seal_number : ''}${lot.sealed ? ' ' + fmtOn(lot.sealed) : ''}`;
  return [
    el('li', { class: 'group bad seal-wait' },
      el('div', { class: 'main' },
        el('div', { class: 'name' }, `🔒 Étiquette du scellé de ${lot.name}`),
        el('div', { class: 'sub' }, `${seal} : scannez l'étiquette collée sur le scellé, sans l'ouvrir.`)),
      el('span', { class: 'count' }, '0 / 1')),
    el('li', { class: 'seal-hint' },
      el('div', { class: 'main' },
        el('div', {}, 'Scellé ouvert ou abîmé ?'),
        el('div', { class: 'sub' }, "Scannez l'étiquette d'ouverture rangée à l'intérieur, puis vérifiez le contenu.")),
      el('button', { type: 'button', class: 'secondary', onclick: () => {
        if (!state.openSeals.includes(lot.id)) state.openSeals.push(lot.id);
        save();
        render();
      } }, 'Vérifier le contenu')),
  ];
}

// Scannes (ou coches, pour un item a cocher) d'un attendu pendant la vérif
// (type a unites : ses sous-types comptent leurs unites, sauf si fresh compte deja par attendu, planSession)
const scannedOf = (lot, row, fresh, byRow = false) => (row.check ? (checkEntry(lot.id, row.type) ? row.expected || row.required : 0)
  : (fresh[row.type] || 0) + (byRow ? 0 : Object.entries(row.fills || {}).reduce((sum, [type, units]) => sum + (fresh[type] || 0) * units, 0)));

const checkEntry = (lotId, type) => allScanned().find((entry) => entry.kind === 'check' && entry.iid === lotId
  && entry.info && entry.info.type === type);

function toggleCheck(lot, row) {
  const entry = checkEntry(lot.id, row.type);
  const index = entry ? state.scanned.indexOf(entry) : -1;
  if (entry && index < 0) { toast(`Coché par ${entry.who || 'un autre participant'}.`); return; }
  scannedAgain();
  if (entry) state.scanned.splice(index, 1);
  else {
    state.scanned.push({ code: `check:${lot.id}:${row.type}`, kind: 'check', iid: lot.id, key: '', expired: false, error: '',
      items: [], place: lot.id, info: { type: row.type, type_name: row.type_name, count: row.expected || row.required } });
    feedback.good();
  }
  save();
  render();
}

// Attendu du lot sous lequel ranger un item de ce type a l'affichage : son type s'il est attendu, sinon son type a
// unites
const displayRow = (rows, type) => {
  const weights = rowWeights(rows, type);
  return weights.some(([row]) => row === type) ? type : mainRow(weights, type)[0];
};
// Attendu d'un type parent (rempli par ses sous-types) : a unites (« 200 mL de Sérum phy », row.unit) ou sans unite
// (« 4 × Bandes », Bandes L ou S, chaque item compte pour 1)
const isGroupRow = (row) => Boolean(row.is_group && !row.within);
// « 50 mL de Sérum phy » pour un type a unites, « 3 × Bandes » sinon (services.amount_text)
const amountText = (count, unit, name) => (unit ? `${count} ${unit} de ${name}` : `${count} × ${name}`);
// Nom d'un attendu : « Sérum phy (mL) » pour un type a unites, « dont Sachet… » pour le minimum d'un de ses
// sous-types compris dedans
const reqName = (row) => (row.within ? 'dont ' : '') + row.type_name + (row.unit && !row.within ? ` (${row.unit})` : '');
// Items a scanner pour un attendu : sa quantite, ou pour un type a unites le moins d'items qui la remplissent
const rowItems = (row) => (isGroupRow(row)
  ? Math.ceil((row.required || 0) / Math.max(1, ...Object.values(row.fills || {}))) : row.required || 0);
// Ce qui manque : « 3 × Compresses », ou « 50 mL de Sérum phy (2 × Sachet…) » (services.lacking_text)
function lackingText(row, lacking = row.required - row.present) {
  if (!isGroupRow(row)) return `${lacking} × ${row.type_name}`;
  const suggest = suggestFor(row, lacking);
  return amountText(lacking, row.unit, row.type_name) + (suggest ? ` (${suggest.count} × ${suggest.type_name})` : '');
}
// Sous-type a prendre pour combler un manque en unites : le sous-type par defaut (suggest du serveur), sinon celui
// qui en remplit le plus
function suggestFor(row, lacking) {
  const subtypes = row.subtypes || [];
  const pick = (row.suggest && subtypes.find((sub) => sub.type === row.suggest.type)) || subtypes[0];
  return lacking > 0 && pick ? { type: pick.type, type_name: pick.type_name, count: Math.ceil(lacking / pick.fills) } : null;
}

// Repartition des items scannes de la vérif en cours (planVerif, verif-plan.js) : lots scelles intacts et lots deja
// valides de la vérif en cascade mis de cote
function planSession() {
  const intact = intactLots();
  // vérif en cascade : les lots deja valides ne sont plus attendus
  const lots = sessionLots().filter((lot) => !intact.has(lot.id) && !state.done[lot.id]);
  return { ...planVerif(lots, allScanned(), { cascade: inCascade(), full: !!state.full }), intact };
}

// Etat d'un lot : verifie et complet (vert), reassort depuis la derniere verif (orange : verif complete
// recommandee), sinon incomplet ou jamais verifie (rouge). Un lot dont le type est a scelle obligatoire est
// rouge tant qu'il n'est pas scelle. kind : 'ok', 'warn' ou 'bad'.
function lotStatus(lot) {
  const make = (kind, label) => ({ ok: kind === 'ok', kind, label });
  // signalement ouvert « inutilisable » sur le lot : rouge, meme scelle ou verifie, jusqu'a sa resolution
  if (lot.unusable) return make('bad', '✘ Non utilisable (signalé)');
  // lot d'un referentiel (B+) : des lignes de la liste dans aucun de ses sacs (serializers.lot_state)
  if ((lot.unplaced || []).length) return make('bad', '✘ Liste pas toute dans les sacs');
  if (lot.is_sealed && lot.expired_count) return make('bad', '✘ Scellé, contient des périmés');
  if (lot.is_sealed) return make('ok', '✔ Scellé' + (lot.valid_until ? `, valide jusqu'au ${fmtDate(lot.valid_until)}` : ''));
  // sous-lot facultatif : son lot parent se valide sans lui (meme regle que serializers.lot_state)
  if (!lot.last_verif) return lot.optional ? make('ok', '✔ Facultatif, jamais vérifié') : make('bad', '✘ Jamais vérifié');
  if (!lot.complete) return make('bad', lot.expired_count ? '✘ Incomplet (périmés)' : '✘ Incomplet');
  if (lot.seal_required) {
    return make('bad', lot.verif_recommended ? '✘ Scellé ouvert, vérif complète puis scellé requis' : '✘ Vérifié, doit être scellé');
  }
  if (lot.verif_recommended) return make('warn', lot.restocked_count ? '⚠ Vérif recommandée, réassort'
    : lot.back_from_list ? '⚠ Vérif recommandée, retour de liste' : lot.marked_used ? '⚠ Vérif recommandée, lot utilisé'
      : '⚠ Vérif recommandée, scellé ouvert');
  return make('ok', '✔ Vérifié, complet');
}

// Lot qui ne fait que regrouper des sous-lots (rien d'attendu, rien dedans, ex : un B+) : son etat est
// celui de ses sous-lots (lignes qui le suivent dans l'arborescence du lot global)
function groupStatus(lot) {
  const rows = (lot.global && lot.global.lots) || [];
  const index = rows.findIndex((row) => row.id === lot.id);
  if (index < 0 || rows[index].counted) return null;
  let problems = 0; let warn = 0; let counted = 0;
  // vérif la plus ancienne des sous-lots (date du scelle pour un lot scelle jamais verifie), null si l'un ne l'a jamais ete
  let at; let never = false;
  for (const row of rows.slice(index + 1)) {
    if (row.depth <= rows[index].depth) break;
    if (!row.counted) continue;
    counted += 1;
    if (row.effective.kind !== 'ok') problems += 1;
    if (row.effective.kind === 'warn') warn += 1;
    if (row.effective.code === 'covered') continue;
    const stamp = row.last_verif || (row.is_sealed ? row.sealed : null);
    if (!stamp) never = true; else if (!at || stamp < at) at = stamp;
  }
  at = never ? null : at || null;
  if (!problems) return { ok: true, kind: 'ok', label: `✔ Sous-lots tous valides (${counted})`, short: '✔ Vérifié', at };
  return { ok: false, kind: warn === problems ? 'warn' : 'bad', label: `✘ ${nb(problems, 'sous-lot')} à traiter sur ${counted}`,
    short: `${warn === problems ? '⚠' : '✘'} ${pl(problems, 'Un sous-lot', 'Des sous-lots')} à traiter`, at };
}

// Etat d'un lot en un mot (« Vérifié », « Incomplet »…) et date de sa derniere vérif (la plus ancienne de ses
// sous-lots pour un regroupement ; aucune pour un lot scelle, son etat dit jusqu'a quand il est valide)
function shortStatus(lot, status) {
  const group = groupStatus(lot);
  if (group) return { word: group.short, at: group.at };
  return { word: status.kind === 'ok' && !lot.is_sealed ? status.label.split(',')[0] : status.label, at: lot.is_sealed ? null : lot.last_verif };
}

// Bandeau du lecteur au debut d'une vérif (etiquette du lot scannee, vérif lancee depuis la fiche) : le nom du
// lot et son etiquette, scannee ou a scanner. Pas l'etat passe du lot (« Vérifié (hier à …) ») : on est en train
// de le vérifier, le detail est sur la fiche du lot.
function showLotInfo() {
  const lot = state.lot;
  if (!lot) return;
  if (keyOk() && labelOk()) showInfo('ok', lot.name, state.lotKey ? "✔ L'étiquette clé est scannée" : "✔ L'étiquette est scannée");
  else showInfo('warn', lot.name, labelLine(lot));
}

// Saisie du PIN (ou choix du PIN quand il est obligatoire et pas encore choisi). `setup` : reponse du serveur
// (pin_at_login_default : case « Demander a chaque connexion » cochee par defaut, pin_at_login_locked : imposee)
function askPin(matricule, key, setup, errorMessage) {
  const dialog = $('#pin');
  $('#pin-title').textContent = setup ? 'Choisissez votre code PIN' : 'Code PIN';
  $('#pin-text').textContent = setup
    ? 'Choisissez votre code d\'au moins 6 chiffres. Il permet aussi de vous connecter sans badge (nom, prénom et code).'
    : `Badge ${matricule} : saisissez votre code PIN.`;
  $('#pin-input').value = '';
  $('#pin-input').placeholder = setup ? '6 chiffres ou plus' : 'Code PIN';
  $('#pin-confirm').value = '';
  $('#pin-confirm').hidden = !setup;
  const atLogin = $('#pin-at-login');
  $('#pin-at-login-box').hidden = !setup;
  if (setup && setup !== askPin.lastSetup) { // garder le choix de la personne si le serveur refuse le code
    atLogin.checked = setup.pin_at_login_default !== false || !!setup.pin_at_login_locked;
    atLogin.disabled = !!setup.pin_at_login_locked;
  }
  askPin.lastSetup = setup || null;
  $('#pin-error').textContent = errorMessage || '';
  $('#pin-form').onsubmit = (event) => {
    event.preventDefault();
    const pin = $('#pin-input').value.trim();
    if (!(setup ? NEW_PIN_RE : PIN_LOGIN_RE).test(pin)) { $('#pin-error').textContent = setup ? NEW_PIN_ERROR : 'Code PIN incorrect.'; return; }
    if (setup && pin !== $('#pin-confirm').value.trim()) { $('#pin-error').textContent = 'Les deux PIN sont différents.'; return; }
    dialog.close();
    login(matricule, key, setup ? { new_pin: pin, pin_at_login: atLogin.checked } : { pin });
  };
  $('#pin-cancel').onclick = () => dialog.close();
  const forgot = $('#pin-forgot');
  forgot.hidden = setup;
  forgot.onclick = () => forgotPin(matricule, key);
  if (!dialog.open) dialog.showModal();
  setTimeout(() => $('#pin-input').focus(), 50);
}

// Reglages de la case « Demander a chaque connexion » au choix du code : meme objet pour un meme badge, pour
// garder la case telle que la personne l'a laissee quand le serveur refuse le code
function pinSetupOf(data, matricule) {
  const last = askPin.lastSetup;
  if (last && last.matricule === matricule) return last;
  return { matricule, pin_at_login_default: data.pin_at_login_default, pin_at_login_locked: data.pin_at_login_locked };
}

async function login(matricule, key, extra = {}) {
  try {
    const user = await api('auth/', { matricule, key, ...extra });
    askPin.lastSetup = null;
    loggedIn({ ...user, key });
  } catch (e) {
    if (e.data && e.data.pin_blocked) {
      feedback.bad();
      showInfo('bad', 'Code PIN bloqué', e.message);
      showPinBlocked(e.data.pin_reset || {});
      return;
    }
    if (e.data && e.data.guest_name_required) { // carte d'invite jamais utilisee : nom et prenom
      if (extra.guest_nom) feedback.bad(); else feedback.info();
      askGuestName(matricule, key, e.data.guest_hours, extra.guest_nom ? e.message : '');
      return;
    }
    if (e.data && (e.data.pin_required || e.data.pin_setup_required)) {
      const retry = extra.pin || extra.new_pin || e.data.pin_locked;
      if (retry) feedback.bad(); else feedback.info();
      askPin(matricule, key, e.data.pin_setup_required ? pinSetupOf(e.data, matricule) : null, retry ? e.message : '');
      return;
    }
    feedback.bad();
    if (e.data && e.data.guest_used) { // carte d'invite deja activee par quelqu'un d'autre : page en grand
      showInfo('bad', "Carte d'invité déjà utilisée", e.message);
      askDialog({ title: "Carte d'invité déjà utilisée", text: e.message, choices: [{ label: 'OK', value: true, cls: 'primary' }], cancel: '' });
      return;
    }
    showInfo('bad', 'Badge refusé', e.message);
  }
}

// Carte d'invite scannee pour la premiere fois (secouriste d'une autre antenne, sans compte) : son prenom et son
// nom, enregistres sur la carte a cette connexion ; la carte vaut ensuite `hours` heures (48 h par defaut)
function askGuestName(matricule, key, hours, errorMessage) {
  const dialog = $('#guest-name');
  $('#guest-name-text').textContent = 'Bienvenue ! Indiquez votre prénom, votre nom et votre antenne : vos vérifs seront à votre nom. '
    + `La carte vaut ${hours || 48} h à partir de maintenant.`;
  $('#guest-name-error').textContent = errorMessage || '';
  $('#guest-name-form').onsubmit = (event) => {
    event.preventDefault();
    const prenom = $('#guest-name-prenom').value.trim();
    const nom = $('#guest-name-nom').value.trim();
    const antenne = $('#guest-name-antenne').value.trim();
    if (!prenom || !nom || !antenne) { $('#guest-name-error').textContent = 'Saisissez votre prénom, votre nom et votre antenne.'; return; }
    dialog.close();
    login(matricule, key, { guest_nom: nom, guest_prenom: prenom, guest_antenne: antenne });
  };
  $('#guest-name-cancel').onclick = () => { dialog.close(); nameLoginCancelled(); };
  if (!dialog.open) dialog.showModal();
  setTimeout(() => $('#guest-name-prenom').focus(), 50);
}

// Fenetre avec des champs a remplir au-dessus du clavier du telephone : le clavier ne reduit que la partie visible
// de la page (visualViewport), la fenetre centree restait cachee dessous. Elle se cale en haut de la partie visible,
// a sa hauteur (elle defile), et le champ en cours reste visible avec le suivant ; Entree passe au champ suivant.
function keepAboveKeyboard(dialog) {
  const viewport = window.visualViewport;
  const fields = () => [...dialog.querySelectorAll('input:not([hidden])')];
  const reveal = (field) => {
    if (!dialog.open || !field || !dialog.contains(field)) return;
    const list = fields();
    const next = list[list.indexOf(field) + 1] || dialog.querySelector('button[type=submit]');
    if (next) next.scrollIntoView({ block: 'nearest' });
    field.scrollIntoView({ block: 'nearest' });
  };
  const fit = () => {
    if (!dialog.open || !viewport) return;
    const keyboard = window.innerHeight - viewport.height > 80;
    dialog.classList.toggle('above-keyboard', keyboard);
    dialog.style.top = keyboard ? `${viewport.offsetTop + 8}px` : '';
    dialog.style.maxHeight = keyboard ? `${viewport.height - 16}px` : '';
    if (keyboard) reveal(document.activeElement);
  };
  if (viewport) { viewport.addEventListener('resize', fit); viewport.addEventListener('scroll', fit); }
  dialog.addEventListener('focusin', (event) => { fit(); setTimeout(() => reveal(event.target), 60); setTimeout(() => reveal(event.target), 400); });
  dialog.addEventListener('close', () => { dialog.classList.remove('above-keyboard'); dialog.style.top = ''; dialog.style.maxHeight = ''; });
  dialog.addEventListener('keydown', (event) => {
    if (event.key !== 'Enter' || event.target.tagName !== 'INPUT') return;
    const list = fields();
    const next = list[list.indexOf(event.target) + 1];
    if (next && !next.value) { event.preventDefault(); next.focus(); }
  });
}
keepAboveKeyboard($('#name-login'));
keepAboveKeyboard($('#pin'));
keepAboveKeyboard($('#guest-name'));

// Connexion de depannage : QR code temporaire (1 h) montre par un admin depuis l'outil de debug, pour un secouriste
// qui a oublie sa carte. Pas de PIN (l'admin s'en porte garant) ; la session du telephone vaut 24 h.
async function tempLogin(matricule, token) {
  try {
    const user = await api('auth/temp/', { matricule, token });
    loggedIn({ ...user, loginAt: Date.now(), sessionMs: (user.session_hours || 24) * 3600 * 1000 - 5 * 60 * 1000 });
  } catch (e) {
    feedback.bad();
    showInfo('bad', 'QR code refusé', e.message);
    toast(e.message, true);
  }
}

// Connexion sans badge (nom, prenom et code PIN) : discrete, depuis le petit lecteur de l'accueil ou la puce
// « Non connecté » du scanner. Le serveur ne dit pas si le nom existe.
function openNameLogin() {
  const dialog = $('#name-login');
  $('#name-login-pin').value = '';
  $('#name-login-error').textContent = '';
  const declaredLink = $('#name-login-declared');
  declaredLink.hidden = !state.declaredAllowed || inHome();
  declaredLink.onclick = () => { dialog.close(); askDeclaredName(); };
  $('#name-login-form').onsubmit = async (event) => {
    event.preventDefault();
    const prenom = $('#name-login-prenom').value.trim();
    const nom = $('#name-login-nom').value.trim();
    const pin = $('#name-login-pin').value.trim();
    if (!prenom || !nom) { $('#name-login-error').textContent = 'Saisissez votre prénom et votre nom.'; return; }
    if (!PIN_LOGIN_RE.test(pin)) { $('#name-login-error').textContent = 'Saisissez votre code PIN (chiffres seulement).'; return; }
    try {
      const user = await api('auth/name/', { nom, prenom, pin });
      dialog.close();
      loggedIn({ ...user, loginAt: Date.now() });
    } catch (e) {
      feedback.bad();
      $('#name-login-pin').value = '';
      $('#name-login-error').textContent = e.message;
    }
  };
  $('#name-login-cancel').onclick = () => { dialog.close(); nameLoginCancelled(); };
  if (!dialog.open) dialog.showModal();
  setTimeout(() => ($('#name-login-prenom').value ? $('#name-login-pin') : $('#name-login-prenom')).focus(), 50);
}

// connexion par nom annulee (bouton ou Echap) : seule, elle oublie ce qui attendait la connexion ; ouverte depuis la
// grande fenetre ou le petit lecteur, ceux-ci restent ouverts et gardent la demande jusqu'a leur propre « Annuler »
function nameLoginCancelled() {
  if (gateOpen() || readerOpen()) return;
  pendingAfterLogin = '';
  loginAction = null;
}
$('#name-login').addEventListener('cancel', nameLoginCancelled);

function loggedIn(user) {
  const action = takeLoginAction(); // action qui attendait la connexion (signalement)
  activityAt = 0;
  noteActivity(); // web partage : l'heure d'inactivite part de la connexion
  state.user = user;
  state.guest = null; // connecte : plus invite
  rememberLoginMethod(user); // passkey ? essayee d'abord a la prochaine connexion (web/app/passkey.js)
  rememberScanBubble();
  state.lots = null;
  state.stock = null;
  state.push = null;
  state.pushDevices = null;
  feedback.info();
  showInfo('ok', `Bonjour ${user.prenom}`,
    user.guest && user.guest_expires ? `Carte d'invité valable jusqu'à ${fmtDateTime(user.guest_expires)}.` : canSeeStock() ? `Rôle ${ROLE_LABELS[user.role] || 'gestion'} : la gestion de la TECH est sur l'accueil.` : 'Vous êtes connecté.');
  save();
  render();
  loadLots();
  if (canSeeStock()) loadStock();
  live.identity();
  afterLogin();
  if (state.pinReset) openPinReset();
  state.reports = null;
  if (state.user) loadReports(true);
  if (action) setTimeout(action, 0);
  else if (!state.pinReset) setTimeout(() => offerPasskey(state.user), 800); // premiere connexion (web/app/passkey.js)
}
