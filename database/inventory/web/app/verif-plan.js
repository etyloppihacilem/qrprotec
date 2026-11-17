/* QRProtec - front web : repartition des items scannes entre les lots d'une vérif (planVerif).
 *
 * Fonction pure, sans DOM ni etat de la page : la page l'appelle par planSession (cascade.js), les tests la jouent
 * sous node sur les scenarios communs au serveur et au poste (tests/verif-scenarios/, voir leur LISEZMOI). Meme
 * regle que services.assign_items / _complete_lots du serveur et verif_plan.cpp du poste.
 */
'use strict';

// Items a cocher (ItemType.physical faux : gobelets, sucres, « piles vérifiées ») : pas d'etiquette, la vérif les
// coche a la main. Une case cochee est un « scan » de type 'check' (iid : le lot, info.type : le type d'item), partage
// avec les autres participants comme les autres scans. checked d'une verif : {lot: [types]} (services.perform_verif).
function checkedMap(entries) {
  const checked = {};
  for (const entry of entries) {
    if (entry.kind !== 'check' || entry.error || !entry.info || !entry.info.type) continue;
    const types = checked[entry.iid] = checked[entry.iid] || [];
    if (!types.includes(entry.info.type)) types.push(entry.info.type);
  }
  return checked;
}


// Attendus d'un lot que remplit un item de ce type, avec ses unites : [[attendu, unites]]. Un item compte pour son
// type (1) et pour le type a unites dont il est un sous-type (« 200 mL de sérum phy » : un flacon de 50 mL en
// compte 50) : meme regle que le serveur (services.row_weights)
function rowWeights(rows, type) {
  const result = [];
  for (const row of rows || []) {
    if (row.check) continue;
    if (row.type === type) result.push([row.type, 1]);
    else if (row.fills && row.fills[type]) result.push([row.type, row.fills[type]]);
  }
  return result;
}
// Attendu principal d'un item : celui de son type a unites s'il y en a un, sinon son type (services.main_row)
const mainRow = (weights, type) => weights.find(([row]) => row !== type) || [type, 1];

// Repartition des items scannes entre les lots de la verif (meme regle que le serveur, services.assign_items) :
// un item deja dans un des lots y reste, un nouvel item va dans le lot de la derniere etiquette scannee avant lui
// (ou un de ses sous-lots) qui en attend encore, sinon dans le premier lot qui en attend encore.
// complete : le lot serait complet avec ces scans (verif partielle possible).
// lots : lots attendus de la vérif (sans les scelles intacts ni les lots deja valides de la cascade), avec leurs
// items, missing_items et requirements (serializers) ; entries : scans de la vérif (allScanned) ; cascade : vérif en
// cascade (un item scanne dans un lot qui ne le contient pas y a ete trouve) ; full : vérif complete.
function planVerif(lots, entries, { cascade = false, full = false } = {}) {
  // vérif complete (admin) : les attendus et items facultatifs comptent comme les autres
  const rowNeed = (row) => (row.optional && full ? row.expected || 0 : row.required);
  const itemOptional = (item) => !!item.optional && !full;
  const locationOf = {};
  const holding = {};
  const done = new Set();
  for (const entry of entries) {
    if (entry.error) continue;
    if (entry.kind === 'item') done.add(entry.iid);
    for (const iid of entry.items || []) done.add(iid);
  }
  for (const lot of lots) {
    for (const item of [...(lot.items || []), ...(lot.missing_items || [])]) {
      locationOf[item.iid] = lot.id;
      // un item a etiquette a dechirer non scanne a ete utilise, un item non consommable non scanne d'un lot
      // ordinaire passe hors lot (services.leaves_lot), un item facultatif reste ou il etait : ils ne retiennent pas
      // le lot
      if (!done.has(item.iid) && !item.tear_off && !itemOptional(item) && (item.consumable !== false || lot.storage)) holding[lot.id] = true;
    }
  }
  const required = {};
  const checks = {}; // items a cocher qui retiennent le lot (gobelets...) : coches a la main, jamais scannes
  for (const lot of lots) {
    required[lot.id] = {};
    checks[lot.id] = [];
    for (const row of lot.requirements || []) {
      if (row.check) { if (rowNeed(row) > 0) checks[lot.id].push(row.type); } else required[lot.id][row.type] = rowNeed(row);
    }
  }
  const ticked = checkedMap(entries);
  const deficit = Object.fromEntries(lots.map((lot) => [lot.id, { ...required[lot.id] }]));
  const rowsOf = Object.fromEntries(lots.map((lot) => [lot.id, lot.requirements || []]));
  // l'item remplit un manque du lot (en unites pour un type a unites) : il y est compte
  const weightsIn = (id, type) => rowWeights(rowsOf[id], type).filter(([row]) => row in required[id]);
  const take = (id, type) => {
    const weights = weightsIn(id, type);
    if (!weights.some(([row]) => deficit[id][row] > 0)) return false;
    for (const [row, units] of weights) deficit[id][row] -= units;
    return true;
  };
  const scanned = [];
  const typeNames = {};
  for (const entry of entries) {
    if (entry.error) continue;
    const type = entry.kind === 'item' ? entry.iid.slice(0, 6) : entry.info && entry.info.type;
    const iids = entry.kind === 'item' ? [entry.iid] : entry.items || [];
    // a jeter possible : perime, inutilisable ou desactive (pas une revision ou desinfection en retard seule,
    // services.discardable)
    const info = entry.info;
    const discardable = !!entry.expired && !(entry.kind === 'item' && info && (info.revision_late || info.disinfection_late)
      && !info.outdated && !info.unusable && !info.disabled);
    for (const iid of iids) scanned.push({ iid, type, expired: !!entry.expired, discardable, place: entry.place || '' });
    if (type && entry.info && entry.info.type_name) typeNames[type] = entry.info.type_name;
  }
  // lot parent de chaque lot de la vérif : un item venu du stock va dans le lot de la derniere etiquette scannee
  // avant lui ou un de ses sous-lots
  const parentOf = Object.fromEntries(lots.map((lot) => [lot.id,
    lot.parent || (lot.path && lot.path.length ? lot.path[lot.path.length - 1].id : null)]));
  const under = (id, place) => {
    for (let current = id; current; current = parentOf[current]) if (current === place) return true;
    return false;
  };
  const target = {};
  const newcomers = [];
  for (const item of scanned) {
    const where = locationOf[item.iid];
    // vérif en cascade : un item scanne dans un lot qui ne le contient pas y a ete trouve, il y va
    if (where && (!cascade || !(item.place in parentOf) || under(where, item.place))) {
      target[item.iid] = where;
      if (!item.expired) take(where, item.type);
    } else newcomers.push(item);
  }
  newcomers.sort((a, b) => (a.expired - b.expired) || (a.iid < b.iid ? -1 : a.iid > b.iid ? 1 : 0));
  for (const item of newcomers) {
    const scope = lots.map((lot) => lot.id).filter((id) => !(item.place in parentOf) || under(id, item.place));
    const candidates = scope.filter((id) => weightsIn(id, item.type).length > 0);
    let choice = null;
    if (!item.expired) choice = candidates.find((id) => take(id, item.type)) || null;
    target[item.iid] = choice || candidates[0] || scope[0];
  }
  // added : items frais qui n'etaient dans aucun lot de la verif (stock, autre lot), par type, pour les « + x ».
  // fresh et added comptent par attendu, en unites pour un type a unites (le sous-type compte aussi pour lui-meme)
  const plan = Object.fromEntries(lots.map((lot) => [lot.id, { lot, required: required[lot.id], fresh: {}, added: {}, touched: 0 }]));
  for (const item of scanned) {
    const row = plan[target[item.iid]];
    if (!row) continue;
    row.touched += 1;
    if (item.expired) continue;
    const weights = weightsIn(row.lot.id, item.type);
    for (const [type, units] of weights.length ? weights : [[item.type, 1]]) row.fresh[type] = (row.fresh[type] || 0) + units;
    const [main, units] = mainRow(weights, item.type);
    if (locationOf[item.iid] !== row.lot.id) row.added[main] = (row.added[main] || 0) + units;
  }
  for (const row of Object.values(plan)) {
    // seul compte le minimum d'items frais : un perime en trop ne retient pas le lot (il est retire, a jeter)
    const filled = Object.entries(required[row.lot.id]).every(([type, quantity]) => (row.fresh[type] || 0) >= quantity)
      && checks[row.lot.id].every((type) => (ticked[row.lot.id] || []).includes(type));
    // rangement du stock : ses items sont ses attendus, complet seulement si tous sont scannes
    row.complete = filled && (row.lot.storage ? !holding[row.lot.id] : (row.touched > 0 || !holding[row.lot.id]));
  }
  // discard : perimes a jeter (services.perform_verif) : le lot a son compte sans eux, ou ils n'y etaient pas
  const discard = new Set();
  for (const item of scanned) {
    const row = plan[target[item.iid]];
    if (!row || !item.discardable) continue;
    const weights = weightsIn(row.lot.id, item.type);
    const covered = (weights.length ? weights : [[item.type, 1]])
      .every(([type]) => (row.fresh[type] || 0) >= (row.required[type] || 0));
    if (locationOf[item.iid] !== row.lot.id || covered) discard.add(item.iid);
  }
  return { lots: lots.map((lot) => plan[lot.id]), target, typeNames, discard };
}

// node (tests) : module CommonJS ; navigateur : script classique, fonctions partagees avec les autres scripts
if (typeof module !== 'undefined' && module.exports) module.exports = { planVerif, rowWeights, mainRow, checkedMap };
