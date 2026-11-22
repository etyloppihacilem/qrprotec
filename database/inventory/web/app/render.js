/* QRProtec - front web : affichage de la vérif (onglets A scanner, Scannes, Lot) et render(). */
'use strict';

// Affichage

// Compte « fait/attendu », avec « + x » en bleu pour les items en trop (ajoutes sans etre attendus) ; « + x » seul
// si rien n'etait attendu
function countText(done, required, extra = 0) {
  const plus = extra > 0 ? el('b', { class: 'plus' }, '+ ' + extra) : null;
  return done || required || !plus ? [`${done}/${required}`, plus] : [plus];
}

// Lot : les items au-dela du minimum sont affiches en « + x » (5 pour 3 attendus : 3/3 + 2)
const lotRequirementRow = (label, present, required, after = null) =>
  requirementRow(label, Math.min(present, required), required, after, Math.max(0, present - required));

// Ligne d'un attendu du lot (row : ligne de requirements du serveur). Facultatif (casques du VPS) : jauge grise
// sur le nombre prevu, jamais rouge ni orange, le lot se valide sans
function requirementLine(label, present, row, after = null) {
  if (!row.optional) return lotRequirementRow(label, present, row.required, after);
  const expected = row.expected || 0;
  return requirementRow(label, Math.min(present, expected), expected, after, Math.max(0, present - expected), true);
}

function requirementRow(label, present, required, after = null, extra = 0, optional = false) {
  const ratio = required > 0 ? Math.min(1, present / required) : 1;
  const empty = present <= 0 && required > 0 && !optional; // x/0 : rien d'attendu, vert
  const none = present <= 0 && required <= 0 && !extra; // 0/0 : orange, a remarquer mais rien a faire
  const color = optional ? (present >= required ? 'var(--green)' : 'var(--muted)')
    : empty ? 'var(--red)' : (none || present < required) ? 'var(--orange)' : 'var(--green)';
  return el('div', { class: 'req' + (optional ? ' optional' : '') },
    el('span', { class: 'label' }, label, optional ? el('span', { class: 'tag optional',
      title: 'Facultatif : le lot se valide sans, mais scannez-les pour savoir où ils sont' }, 'facultatif') : null),
    el('div', { class: 'bar' },
      el('i', { style: `width:${empty ? 100 : ratio * 100}%;background:${color}` }),
      el('span', { style: empty ? 'color:#fff' : '' }, ...countText(present, required, extra))), after);
}

// Cle de tri des barres, attention d'abord : sous le minimum (par ratio), puis bientot sous le minimum ou vide
// (soon : combien periment bientot), puis 0/0, puis complets, puis x/0 tout en bas
function stockOrder(present, required, soon = 0) {
  if (required > 0 && present < required) return present / required;
  const left = present - soon;
  if (required > 0 ? left < required : present > 0 && left <= 0) return 1 + (required > 0 ? left / required : 0);
  if (required <= 0) return present <= 0 ? 2 : Infinity;
  return 2 + present / required;
}

// Items frais scannes par type (un paquet compte pour son nombre d'items)
function freshByType() {
  const fresh = {};
  for (const entry of allScanned()) {
    if (entry.error || entry.expired) continue;
    const type = entry.kind === 'item' ? entry.iid.slice(0, 6) : entry.info && entry.info.type;
    const count = entry.kind === 'pack' ? (entry.items || []).length : 1;
    if (type) fresh[type] = (fresh[type] || 0) + count;
  }
  return fresh;
}

// Attendus d'un lot : la definition de son type (quantite par type), puis les items deja connus du lot.
// Les items scannes comptent pour le lot ou ils seront ranges (planSession).
function expectedGroups(lot, plan) {
  if (plan.intact.has(lot.id)) return { groups: [], others: [], remaining: 0, complete: true, sealed: true };
  const done = scannedIids();
  const row = plan.lots.find((entry) => entry.lot.id === lot.id);
  if (lot.storage) return storageGroups(lot, done, row, plan);
  const fresh = row ? row.fresh : {};
  const known = {};
  for (const item of lot.items || []) {
    if (done.has(item.iid)) continue;
    const type = displayRow(lot.requirements, item.type); // sous-type : sous son type a unites
    (known[type] = known[type] || []).push(item);
  }
  let remaining = 0;
  const groups = (lot.requirements || []).map(strictRow).map((requirement) => {
    const items = (known[requirement.type] || []).sort((a, b) => b.expired - a.expired);
    delete known[requirement.type];
    const scanned = fresh[requirement.type] || 0;
    if (requirement.check) {
      // item a cocher : coche (tout est la) ou non (il manque), compte a la main
      const quantity = requirement.expected || requirement.required;
      const ticked = !!checkEntry(lot.id, requirement.type);
      const missing = ticked || requirement.optional ? 0 : quantity;
      remaining += missing ? 1 : 0; // une case a cocher, pas `quantity` scans
      return { row: requirement, items: [], check: true, ticked, scanned: ticked ? quantity : 0, missing,
        optional: !!requirement.optional, expected: quantity, pending: 0, extra: 0, fromStock: 0 };
    }
    if (requirement.optional) {
      // facultatif (casques du VPS) : rien ne manque, le lot se valide sans ; scannes, on sait ou ils sont
      const expected = requirement.expected || 0;
      return { row: requirement, items, scanned: Math.min(scanned, expected), missing: 0, optional: true, expected,
        pending: Math.max(0, expected - scanned), extra: Math.max(0, scanned - expected), fromStock: 0 };
    }
    if (isGroupRow(requirement)) {
      // type a unites (200 mL de serum phy) : comptes en unites a l'affichage (units) ; pour les totaux, une
      // ligne vaut 1 et ce qui reste a scanner se compte en items (du sous-type qui en remplit le plus). Type parent
      // sans unite (4 bandes, L ou S) : unites = items, la ligne compte ses items comme une ligne ordinaire
      const fills = Object.values(requirement.fills || {});
      const most = Math.max(1, ...fills);
      const missingUnits = Math.max(0, requirement.required - scanned);
      const knownUnits = items.filter((item) => !item.expired)
        .reduce((sum, item) => sum + ((requirement.fills || {})[item.type] || 0), 0);
      const missing = Math.ceil(missingUnits / most);
      remaining += missing;
      const scannedRow = requirement.unit ? (missingUnits ? 0 : 1) : Math.min(scanned, requirement.required);
      return { row: requirement, items, isGroup: true, unit: requirement.unit, scanned: scannedRow, missing, extra: 0,
        fromStock: 0, units: { scanned: Math.min(scanned, requirement.required), required: requirement.required,
          missing: missingUnits, extra: Math.max(0, scanned - requirement.required),
          fromStock: Math.max(0, missingUnits - knownUnits) } };
    }
    const missing = Math.max(0, requirement.required - scanned);
    const knownFresh = items.filter((item) => !item.expired).length;
    remaining += missing;
    return { row: requirement, items, scanned: Math.min(scanned, requirement.required), missing,
      extra: Math.max(0, scanned - requirement.required), fromStock: Math.max(0, missing - knownFresh) };
  });
  groups.push(...addedGroups(row, plan, groups));
  const others = Object.values(known).flat();
  remaining += others.filter((item) => !itemOptional(item)).length; // sous-lot facultatif : son materiel ne retient rien
  return { groups, others, remaining, complete: row ? row.complete : false };
}

// Rangement du stock : pas de contenu attendu par type, chaque item qui y est range est attendu (on verifie qu'il
// est toujours la). Un type par groupe, compte sur les items connus du rangement.
function storageGroups(lot, done, row, plan) {
  const byType = {};
  for (const item of lot.items || []) {
    const group = byType[item.type] = byType[item.type]
      || { row: { type: item.type, type_name: item.type_name, location: '', required: 0 }, items: [], scanned: 0 };
    group.row.required += 1;
    if (done.has(item.iid)) group.scanned += 1;
    else group.items.push(item);
  }
  // items ajoutes au rangement (venus d'ailleurs) : « + x » sur la ligne de leur type
  for (const group of Object.values(byType)) group.extra = row ? row.added[group.row.type] || 0 : 0;
  const groups = [...Object.values(byType), ...addedGroups(row, plan, Object.values(byType))]
    .sort((a, b) => a.row.type_name.localeCompare(b.row.type_name, 'fr'));
  let remaining = 0;
  for (const group of groups) {
    group.items.sort((a, b) => b.expired - a.expired);
    group.missing = group.row.required - group.scanned;
    group.fromStock = 0;
    remaining += group.missing;
  }
  return { groups, others: [], remaining, complete: row ? row.complete : false };
}

// Types ajoutes a un lot pendant la verif sans y etre attendus (ni dans sa definition, ni deja dans le lot) : une
// ligne « + x » chacun
function addedGroups(row, plan, groups) {
  if (!row) return [];
  const listed = new Set(groups.map((group) => group.row.type));
  return Object.entries(row.added).filter(([type]) => !listed.has(type)).map(([type, count]) => ({
    row: { type, type_name: plan.typeNames[type] || type, location: '', required: 0 },
    items: [], scanned: 0, missing: 0, extra: count, fromStock: 0 }));
}

// Item attendu, dans la liste depliable de son type : seulement son iid, en petit (le type est dans le titre)
function todoItem(item) {
  return el('li', { class: 'iid' + (item.expired ? ' expired' : '') },
    el('span', { class: 'main' }, item.iid),
    item.expired ? el('span', { class: 'tag red' }, badLabel(item)) : null,
    item.reported && !item.unusable ? el('span', { class: 'tag orange', title: 'Problème signalé, toujours utilisable' }, 'signalé') : null,
    item.tear_off ? el('span', { class: 'tag orange', title: 'Étiquette à déchirer : si elle manque, compté comme utilisé' }, 'à déchirer') : null,
    item.consumable === false ? el('span', { class: 'tag blue', title: 'Non consommable : s\'il manque, il passe hors lot (pas perdu)' }, 'non consommable') : null,
    (item.tasks || []).length ? el('span', { class: 'tag blue', title: 'Tâches à cocher au scan de cet item' }, 'tâches') : null,
    item.missed_verifs > 0 ? el('span', { class: 'tag orange' }, 'non vu') : null);
}

// Types dont la liste des iids attendus est depliee (repliee par defaut)
const openGroups = new Set();

// Titre d'un type attendu : touche pour deplier ses iids
function groupHead(key, cls, items, main, count) {
  const open = openGroups.has(key);
  const toggle = items.length && showIds() ? () => { if (open) openGroups.delete(key); else openGroups.add(key); renderTodo(); } : null;
  return [
    el('li', { class: 'group ' + cls + (toggle ? ' foldable' : ''), onclick: toggle, 'aria-expanded': toggle ? String(open) : null },
      el('span', { class: 'chevron' }, toggle ? (open ? '▾' : '▸') : ''), main, count),
    ...(open && toggle ? items.map(todoItem) : []),
  ];
}

// Regroupe par emplacement dans le lot (ex. « pochette bleue ») : [{ place, rows }] dans l'ordre d'arrivee (celui des
// emplacements choisi par la TECH, le serveur rend les attendus dans cet ordre), « sans emplacement » en dernier
function byPlace(rows, placeOf) {
  const places = new Map();
  for (const row of rows) {
    const place = placeOf(row) || '';
    if (!places.has(place)) places.set(place, []);
    places.get(place).push(row);
  }
  const result = [...places].filter(([place]) => place).map(([place, list]) => ({ place, rows: list }));
  if (places.has('')) result.push({ place: '', rows: places.get('') });
  return result;
}

// Emplacements deplies ou replies a la main en verif, tant qu'ils ne changent pas d'etat (sinon : un emplacement
// complet est replie, les autres deplies)
const placeFolds = new Map();

// Attendus d'un lot par type : en cours (orange, dernier type scanne en haut), puis rien de scanne (rouge), puis
// les autres items du lot, et les types complets (vert) en bas. Avec des emplacements (pochettes), un titre par
// emplacement, pour faire une pochette a la fois : emplacements et types dans chacun tries de la meme facon,
// l'emplacement complet replie sur son titre, et les autres items du lot a la fin.
function lotGroupRows(lot, groups, others, lastScan) {
  const rows = [];
  const lastScanOf = (group) => Math.max(-1, ...[group.row.type, ...Object.keys(group.row.fills || {})]
    .map((type) => (type in lastScan ? lastScan[type] : -1)));
  // facultatif pas encore tout scanne : ni a faire ni un probleme, juste au-dessus des complets
  const rank = (group) => (group.missing === 0 ? (group.pending > 0 ? 1.5 : 2) : group.scanned === 0 ? 1 : 0);
  const need = (group) => (group.isGroup && group.unit ? 1 : group.optional ? group.expected : group.row.required);
  const places = byPlace(groups, (group) => group.row.location);
  const placed = places.some((entry) => entry.place);
  const groupRows = (group) => {
    if (group.check) {
      const entry = checkEntry(lot.id, group.row.type);
      const tasks = group.row.tasks || [];
      return [el('li', { class: 'group check ' + (group.ticked ? 'ok' : group.optional ? 'optional' : 'bad'),
        onclick: () => toggleCheck(lot, group.row), role: 'checkbox', 'aria-checked': String(group.ticked) },
      el('span', { class: 'checkbox' }, group.ticked ? '☑' : '☐'),
      el('div', { class: 'main' },
        el('div', { class: 'name' }, `${group.expected} × ${group.row.type_name}`),
        el('div', { class: 'sub' }, [!placed && group.row.location ? '📍 ' + group.row.location : '',
          group.ticked ? 'coché' + (entry && entry.who ? ' par ' + entry.who : '') : 'à vérifier à la main, puis cochez',
          tasks.join(', ')].filter(Boolean).join(' · '))),
      el('span', { class: 'count' }, ...countText(group.scanned, need(group), 0)))];
    }
    const expired = group.items.filter((item) => item.expired).length;
    if (group.isGroup) {
      // type a unites : « 150/200 mL », ce qui manque en mL et le sous-type a prendre (sans unite : « 3/4 »)
      const unitText = group.unit ? ' ' + group.unit : '';
      const units = group.units;
      const cls = units.missing ? (units.scanned ? 'partial' : 'bad') : 'ok';
      const suggest = suggestFor(group.row, units.fromStock);
      return groupHead(`${lot.id}:${group.row.type}`, cls, group.items,
        el('div', { class: 'main' },
          el('div', { class: 'name' }, reqName(group.row)),
          el('div', { class: 'sub' }, [!placed && group.row.location ? '📍 ' + group.row.location : '',
            units.missing > 0 ? `encore ${units.missing}${unitText} à scanner` : 'complet',
            units.extra ? `${units.extra}${unitText} en plus` : '',
            expired ? nb(expired, 'périmé') : '',
            suggest ? `stock : ${suggest.count} × ${suggest.type_name}` : ''].filter(Boolean).join(' · '))),
        el('span', { class: 'count' }, ...countText(units.scanned, units.required, units.extra), unitText));
    }
    const cls = { 2: 'ok', 1.5: 'optional', 1: 'bad', 0: 'partial' }[rank(group)];
    return groupHead(`${lot.id}:${group.row.type}`, cls, group.items,
      el('div', { class: 'main' },
        el('div', { class: 'name' }, reqName(group.row), group.optional ? el('span', { class: 'tag optional',
          title: 'Facultatif : le lot se valide sans, mais scannez-les pour savoir où ils sont' }, 'facultatif') : null),
        el('div', { class: 'sub' }, [!placed && group.row.location ? '📍 ' + group.row.location : '',
          group.pending > 0 ? (group.scanned ? `${group.pending} de plus si vous les voyez` : 'validable sans, scannez ceux que vous voyez')
            : group.missing > 0 ? `encore ${group.missing} à scanner` : need(group) ? 'complet' : '',
          group.extra ? `${group.extra} en plus` : '',
          expired ? nb(expired, 'périmé') : '',
          group.fromStock > 0 ? `${group.fromStock} à prendre dans le stock` : ''].filter(Boolean).join(' · '))),
      el('span', { class: 'count' }, ...countText(group.scanned, need(group), group.extra)));
  };
  // tri : un facultatif commence (casques 1/4) monte avec les types en cours, le dernier scanne en haut
  const order = (group) => (group.optional && group.scanned > 0 && group.pending > 0 ? 0 : rank(group));
  const sorted = (list) => list.sort((a, b) => order(a) - order(b)
    || (order(a) === 0 ? lastScanOf(b) - lastScanOf(a) : 0));
  const othersRows = () => (others.length ? groupHead(`${lot.id}:others`, 'other', others,
    el('div', { class: 'main' }, 'Autres items du lot (hors définition)'),
    el('span', { class: 'count' }, String(others.length))) : []);
  if (!placed) {
    sorted(groups);
    for (const group of groups) if (order(group) < 1.5) rows.push(...groupRows(group));
    rows.push(...othersRows());
    for (const group of groups) if (order(group) >= 1.5) rows.push(...groupRows(group));
    return rows;
  }
  // emplacements tries comme les types : en cours (le dernier scanne en haut), puis rien de scanne, puis complets
  // en bas (le scan qui complete un emplacement le fait descendre) ; l'ordre de la TECH a egalite
  const placeRows = places.map(({ place, rows: list }) => {
    const scanned = list.reduce((sum, group) => sum + Math.min(group.scanned, need(group)), 0);
    const missing = list.reduce((sum, group) => sum + group.missing, 0);
    const pending = list.reduce((sum, group) => sum + (group.pending || 0), 0);
    const extra = list.reduce((sum, group) => sum + (group.extra || 0), 0);
    return { place, list, scanned, missing, pending, extra,
      required: list.reduce((sum, group) => sum + need(group), 0),
      rank: !missing ? (pending ? 1.5 : 2) : scanned ? 0 : 1, last: Math.max(...list.map(lastScanOf)),
      // emplacement commence, meme seulement du facultatif (casques 1/4) : monte avec ceux en cours
      order: scanned && (missing || pending) ? 0 : !missing ? (pending ? 1.5 : 2) : 1 };
  });
  placeRows.sort((a, b) => a.order - b.order || (a.order === 0 ? b.last - a.last : 0));
  for (const { place, list, scanned, missing, pending, extra, required, rank: placeRank, last } of placeRows) {
    const key = `${lot.id}@${place}`;
    const fold = placeFolds.get(key);
    // un emplacement qui ne contient que du facultatif est replie (sauf en vérif complete, ou il est obligatoire)
    // tant qu'on n'y scanne rien : des qu'un de ses items est scanne, il se deplie ; avec aussi de l'obligatoire, il
    // reste deplie tant que le facultatif n'est pas tout vu. Plie ou deplie a la main, il le reste jusqu'a ce qu'il
    // change d'etat ou qu'un nouveau scan tombe dedans
    const onlyOptional = list.every((group) => group.optional);
    const open = fold && fold.complete === !missing && fold.last === last ? fold.open
      : missing > 0 || (pending > 0 && (!onlyOptional || scanned > 0));
    rows.push(el('li', { class: 'place-head ' + { 0: 'partial', 1: 'bad', 1.5: 'optional', 2: 'ok' }[placeRank],
      'aria-expanded': String(open),
      onclick: () => { placeFolds.set(key, { open: !open, complete: !missing, last }); renderTodo(); } },
    el('span', { class: 'chevron' }, open ? '▾' : '▸'),
    el('div', { class: 'main name' }, '📍 ' + (place || 'Sans emplacement')),
    el('span', { class: 'count' }, ...countText(scanned, required, extra))));
    if (open) for (const group of sorted(list)) rows.push(...groupRows(group));
  }
  rows.push(...othersRows());
  return rows;
}

// rang du dernier scan de chaque type d'item
function lastScanRanks() {
  const lastScan = {};
  allScanned().forEach((entry, index) => {
    const type = entry.kind === 'item' ? entry.iid.slice(0, 6) : entry.info && entry.info.type;
    if (type && !entry.error) lastScan[type] = index;
  });
  return lastScan;
}

function renderTodo() {
  const list = $('#todo-list');
  if (!state.lot) {
    list.replaceChildren(el('li', { class: 'empty' }, "Scannez l'étiquette d'un lot pour afficher son contenu."));
    return 0;
  }
  if (inCascade()) return renderCascade(list);
  const last = state.lastVerif;
  if (last && last.lotId === state.lot.id && !allScanned().length) {
    const at = new Date(last.at).toLocaleTimeString('fr-FR', { hour: '2-digit', minute: '2-digit' });
    const what = last.partial ? `vérif partielle (${nb(last.lots, 'lot vérifié', 'lots vérifiés')})`
      : `${last.lots > 1 ? 'lots' : 'lot'} ${last.complete ? 'complet' + (last.lots > 1 ? 's' : '') : 'incomplet'}`;
    list.replaceChildren(
      el('li', { class: 'group ' + (last.complete ? 'ok' : 'bad') },
        el('div', { class: 'main' },
          el('div', { class: 'name' }, `${last.complete ? '✔' : '✘'} Vérif enregistrée à ${at}`),
          el('div', { class: 'sub' }, `${nb(last.present, 'item présent', 'items présents')} · ${what}`))),
      el('li', { class: 'empty' }, 'Scannez un item pour commencer une nouvelle vérif.'));
    return 0;
  }
  if (sessionLots().length === 1 && sealWaiting(state.lot)) {
    list.replaceChildren(...sealRows(state.lot));
    return 1;
  }
  const plan = planSession();
  const lots = sessionLots();
  const multi = lots.length > 1;
  const rows = [];
  const lastScan = lastScanRanks();
  let remaining = 0;
  for (const lot of lots) {
    const { groups, others, remaining: left, complete, sealed } = expectedGroups(lot, plan);
    remaining += left;
    if (sealed) {
      rows.push(el('li', { class: 'lot-head ok', style: `margin-left:${(lot.depth || 0) * 12}px` },
        el('div', { class: 'main' }, el('div', { class: 'name' }, '🔒 ' + lot.name),
          el('div', { class: 'sub' }, 'scellé intact : compté comme vérifié'))));
      continue;
    }
    // verif groupee : un titre par lot (un lot qui ne contient rien et n'attend rien n'est qu'un regroupement)
    if (multi) {
      if (!groups.length && !others.length && !(lot.items || []).length) continue;
      rows.push(el('li', { class: 'lot-head ' + (complete ? 'ok' : left ? 'bad' : 'partial'), style: `margin-left:${(lot.depth || 0) * 12}px` },
        el('div', { class: 'main' },
          el('div', { class: 'name' }, '📦 ' + lot.name),
          el('div', { class: 'sub' }, complete ? 'complet avec les scans' : `encore ${left} à scanner`))));
    }
    rows.push(...lotGroupRows(lot, groups, others, lastScan));
  }
  if (!remaining) rows.unshift(el('li', { class: 'group ok' }, multi ? '✅ Tout est scanné : les lots seront complets.' : '✅ Tout est scanné : le lot sera complet.'));
  const full = fullVerifRow();
  if (full) rows.unshift(full);
  list.replaceChildren(...rows);
  return remaining;
}

// Ordre des sous-lots de la vérif en cascade (attention d'abord) : rouge, commencé (orange), à faire, facultatif, vert
const SUB_LOT_RANK = { bad: 0, partial: 1, todo: 2, optional: 3, ok: 4 };

// Vérif en cascade : le lot en cours (fil depuis le lot global, retour au lot qui le contient), ses sous-lots (faits
// en vert, en cours, a faire ; touchez-en un ou scannez son etiquette pour y entrer), puis ses propres attendus.
function renderCascade(list) {
  const plan = planSession();
  const level = levelLot();
  const chain = [state.lot, ...state.path.map(lotById).filter(Boolean)];
  const parent = chain.length > 1 ? chain[chain.length - 2] : null;
  const rows = [];
  const crumbs = [];
  chain.forEach((lot, index) => {
    if (index) crumbs.push(el('span', { class: 'sep' }, ' › '));
    crumbs.push(index < chain.length - 1
      ? el('button', { type: 'button', class: 'text-link', onclick: () => { state.path = chainTo(lot.id); state.current = levelId(); save(); render(); } }, lot.name)
      : el('b', {}, lot.name));
  });
  rows.push(el('li', { class: 'cascade-path' },
    el('div', { class: 'main' }, el('div', { class: 'crumbs' }, ...crumbs),
      el('div', { class: 'sub' }, sealWaiting(level) ? `Scannez l'étiquette du scellé : ${level.name} est validé sans l'ouvrir.`
        : sealNextLevel() ? `Validez ${level.name} : la vérif s'arrête là, le lot est ensuite à resceller.`
        : parent ? `Validez ${level.name} pour revenir à ${parent.name}.`
        : childrenOf(level.id).length ? `Scannez l'étiquette ${childrenDe(level.id)} pour y entrer.` : '')),
    parent ? el('button', { type: 'button', class: 'secondary', title: `Revenir à ${parent.name} en validant ${level.name} (incomplet s'il manque des items)`,
      onclick: leaveUp }, '← ' + (parent.name_short || parent.name)) : null));
  if (sealWaiting(level)) {
    rows.push(...sealRows(level));
    list.replaceChildren(...rows);
    return 1;
  }
  const others = othersLevels();
  let left = 0;
  const subRows = [];
  const scansNow = allScanned();
  for (const child of childrenOf(level.id)) {
    const ids = subtreeIds(child.id);
    const done = state.done[child.id];
    const sealed = plan.intact.has(child.id);
    const who = [...new Set(ids.flatMap((id) => others[id] || []))];
    const scanned = allScanned().filter((entry) => !entry.error && entry.kind !== 'seal' && ids.includes(entry.place)).length;
    const subs = childrenOf(child.id).length;
    let cls; let sub;
    if (done) {
      cls = done.complete || done.sealed ? 'ok' : 'bad';
      sub = done.sealed ? `🔒 Scellé intact constaté ${fmtOn(done.at)}`
        : `✔ Validé ${fmtOn(done.at)}${done.by ? ' par ' + done.by : ''}${done.complete ? '' : ' · incomplet'}`;
    } else if (sealed) {
      cls = 'ok';
      sub = '🔒 Scellé intact : enregistré à la validation';
    } else if (lotOptional(child) && !who.length && !touched(child.id)) {
      // sous-lot facultatif : on peut valider sans lui, ou y entrer pour scanner ce qu'il contient
      cls = 'optional';
      sub = 'Facultatif · validable sans, touchez pour y scanner';
    } else {
      left += 1;
      if (who.length) { cls = 'partial'; sub = `En cours · ${who.join(', ')}`; }
      else if (touched(child.id)) { cls = 'partial'; sub = `Commencé · ${nb(scanned, 'scanné')}, pas encore validé`; }
      else if (sealWaiting(child)) { cls = 'todo'; sub = "🔒 Scellé : scannez l'étiquette du scellé"; }
      else { cls = 'todo'; const status = childStatus(child); sub = el('span', {}, 'À faire · état actuel : ', el('span', { class: 'state-' + (status.kind || (status.ok ? 'ok' : 'bad')) }, status.label)); }
    }
    // dernier scan dans ce sous-lot (ou ses sous-lots) : le plus recent monte, s'il n'est pas vert
    let last = -1;
    scansNow.forEach((entry, index) => { if (!entry.error && ids.includes(entry.place || state.lotId)) last = index; });
    subRows.push({ rank: SUB_LOT_RANK[cls] ?? 2, last: cls === 'ok' ? -1 : last, row: el('li', { class: 'lot-head sub-lot ' + cls, onclick: async () => {
      if (done && !await confirmDialog(`Refaire ${child.name} ?`, `${child.name} est déjà ${done.sealed ? 'constaté scellé' : 'validé'}.`, 'Le refaire')) return;
      enterLevel(child.id);
      save();
      render();
    } },
    el('div', { class: 'main' },
      el('div', { class: 'name' }, (child.is_sealed ? '🔒 ' : '📦 ') + child.name),
      el('div', { class: 'sub' }, sub, subs ? ` · ${nb(subs, 'sous-lot')}` : '')),
    el('span', { class: 'chevron' }, '›')) });
  }
  // ordre general des listes : rouge en haut, orange, a faire, facultatif, vert en bas ; a egalite, le dernier
  // scanne monte (sauf les verts), puis l'ordre d'origine (tri stable)
  // Contenu propre du lot en cours : sous les sous-lots, sauf s'il est commence (un type en partie scanne, meme
  // facultatif, ex. casques 1/4) : il se range alors avec eux comme un sous-lot commence, dernier scan compris
  const { groups, others: loose, remaining } = level.storage || ownContent(level) ? expectedGroups(level, plan)
    : { groups: [], others: [], remaining: 0 };
  let ownRows = [];
  if (groups.length || loose.length) {
    if (childrenOf(level.id).length) ownRows.push(el('li', { class: 'cascade-own' }, `Contenu de ${level.name}`));
    ownRows.push(...lotGroupRows(level, groups, loose, lastScanRanks()));
    const started = groups.some((group) => group.scanned > 0 && (group.missing > 0 || group.pending > 0));
    if (started && subRows.length) {
      let last = -1;
      scansNow.forEach((entry, index) => { if (!entry.error && (entry.place || state.lotId) === level.id) last = index; });
      subRows.push({ rank: SUB_LOT_RANK.partial, last, rows: ownRows });
      ownRows = [];
    }
  }
  subRows.sort((a, b) => a.rank - b.rank || b.last - a.last);
  rows.push(...subRows.flatMap((entry) => entry.rows || [entry.row]), ...ownRows);
  if (!left && !remaining) {
    rows.splice(1, 0, el('li', { class: 'group ok' }, `✅ Tout est fait : validez ${level.name}`
      + (sealNextLevel() ? ', puis rescellez-le.' : parent ? ` pour revenir à ${parent.name}.` : ' pour terminer la vérif.')));
  }
  const full = fullVerifRow();
  if (full) rows.splice(1, 0, full);
  list.replaceChildren(...rows);
  return remaining + left;
}

// Scannes : les notres (retirables), et ceux des autres participants de la vérif commune, avec leur nom
function renderDone() {
  const list = $('#done-list');
  const expected = knownIids();
  const all = allScanned();
  const discard = state.lot ? discardedEntry(planSession()) : () => false;
  const rows = all.map((entry, index) => {
    const mine = index < state.scanned.length;
    const info = entry.info;
    let name = entry.kind === 'check' && info ? `☑ ${info.count || ''} × ${info.type_name}`
      : entry.kind === 'pack' && info ? `Paquet : ${info.count} × ${info.type_name}` : info ? info.type_name : idText(entry.iid) || 'Code non reconnu';
    let cls = 'ok';
    const tags = [];
    if (entry.error) { cls = 'error'; tags.push(el('span', { class: 'tag red' }, 'INCONNU')); }
    else if (discard(entry)) { cls = 'discard'; tags.push(el('span', { class: 'tag gray' }, 'À JETER')); }
    else if (entry.expired) { cls = 'expired'; tags.push(el('span', { class: 'tag red' }, badLabel(info))); }
    else if (state.lot && entry.kind === 'item' && !expected.has(entry.iid)) {
      cls = 'extra'; tags.push(el('span', { class: 'tag blue' }, 'nouveau'));
    }
    const peremption = info ? info.peremption : null;
    return el('li', { class: cls },
      el('div', { class: 'main' },
        el('div', { class: 'name' }, name),
        el('div', { class: 'sub' }, [peremption ? fmtDate(peremption) : info && entry.kind === 'item' ? 'Non périssable' : '',
          entry.kind === 'check' ? `coché à la main – ${(lotById(entry.iid) || {}).name || ''}` : idText(entry.iid),
          mine ? '' : `par ${entry.who || 'un autre participant'}`].filter(Boolean).join(' · '))),
      ...tags,
      mine && el('button', {
        class: 'remove', type: 'button', 'aria-label': 'Retirer',
        onclick: () => { scannedAgain(); state.scanned.splice(index, 1); save(); render(); },
      }, '✕'));
  }).reverse(); // dernier scan en haut
  list.replaceChildren(...(rows.length ? rows : [el('li', { class: 'empty' }, 'Aucun item scanné.')]));
  return all.length;
}

function renderLot() {
  const view = $('#lot-view');
  const lot = state.lot;
  if (!lot) {
    view.replaceChildren(el('p', {}, "Aucun lot sélectionné. Scannez l'étiquette publique ou clé d'un lot" + (showIds() ? ', ou saisissez son identifiant (menu ⋯).' : '.')));
    return;
  }
  const plan = planSession();
  const fresh = (plan.lots.find((row) => row.lot.id === lot.id) || { fresh: {} }).fresh;
  const lots = sessionLots();
  view.replaceChildren(
    el('h2', {}, lot.name),
    el('div', { class: 'sub' }, withId(lotKind(lot), lot.id)),
    (lot.path || []).length ? el('div', { class: 'sub' }, 'Dans : ',
      ...lot.path.filter((parent) => !parent.hidden) // lot cache dans la hierarchie (B+ du VPS) : saute
        .flatMap((parent, index) => [index ? ' › ' : '', lotLink(parent)])) : '',
    el('div', { class: 'banner ' + (groupStatus(lot) || lotStatus(lot)).kind }, (groupStatus(lot) || lotStatus(lot)).label),
    // lot d'un referentiel (B+) : lignes de la liste dans aucun de ses sacs, a repartir au poste
    (lot.unplaced || []).length ? el('p', { class: 'hint' }, `Dans aucun sac : ${lot.unplaced.map((row) => row.type_name).join(', ')}`
      + (canSeeStock() ? ' (à répartir au poste : fiche du lot, onglet Répartition).' : '.')) : '',
    el('p', {}, ficheLink('lot', lot.id, 'Fiche du lot : contenu et emplacement des items')),
    reportButton({ lot: lot.id, lotName: lot.name, label: lot.name }),
    lot.is_sealed ? el('p', {}, `🔒 Scellé${lot.seal_number ? ' n°' + lot.seal_number : ''} ${fmtOn(lot.sealed)}` +
      (lot.sealed_by ? ` par ${lot.sealed_by}` : '') + ' : pas de vérif nécessaire tant que le scellé est intact.') : '',
    el('p', {}, 'Dernière vérif : ' + (lot.last_verif ? `${fmtDateTime(lot.last_verif)} par ${lot.last_verif_by || '?'}` : 'jamais'),
      el('br'), labelLine(lot)),
    lotDatesLines(lot),
    el('p', {}, sheetLink(lot.id, (lot.descendants || []).length ? 'Fiche du lot en PDF (une page par sous-lot)' : 'Fiche du lot en PDF')),
    lots.length > 1 ? el('p', {}, `Vérif groupée : ${lots.filter((sub) => (sub.requirements || []).length || (sub.items || []).length).map((sub) => sub.name).join(', ')}. `
      + "Scannez l'étiquette d'un autre lot du même lot global pour l'ajouter.") : '',
    ...renderGlobal(lot),
    // types incomplets en haut
    el('h3', {}, 'Scannés / attendus'),
    ...incompleteFirst((lot.requirements || []).map(strictRow), (row) => scannedOf(lot, row, fresh)).map((row) => requirementLine(
      reqName(row) + (row.location ? ` (${row.location})` : ''), scannedOf(lot, row, fresh), row)),
    el('h3', {}, 'État enregistré du lot'),
    ...incompleteFirst(lot.requirements, (row) => row.present).map((row) => requirementLine(reqName(row), row.present, row)),
  );
}

// Types attendus d'un lot, les incomplets d'abord (ordre de la definition a egalite)
// incomplets, puis 0/0 (orange, rien a faire), puis complets
// incomplets, puis 0/0, puis facultatifs pas tous vus, puis complets
const requirementRank = (present, row) => present < row.required ? 0 : row.optional ? (present < row.expected ? 1.5 : 2)
  : present <= 0 && row.required <= 0 ? 1 : 2;
const incompleteFirst = (requirements, present) => [...(requirements || [])]
  .sort((a, b) => requirementRank(present(a), a) - requirementRank(present(b), b));

// Fiche d'inventaire papier (PDF A4) : liste de controle par emplacement, une page par sous-lot
function sheetLink(id, label) {
  return el('a', { class: 'button-link', href: new URL(`lots/${encodeURIComponent(id)}/sheet.pdf`, API).href,
    target: '_blank', rel: 'noopener' }, '📄 ' + label);
}

// Lot global (depuis n'importe lequel de ses sous-lots) : etat de l'ensemble et de chaque lot
function renderGlobal(lot) {
  const global = lot.global;
  if (!global) return [];
  const tags = { ok: 'green', warn: 'orange', bad: 'red' };
  return [
    el('h3', {}, `Lot global : ${global.name}`),
    el('div', { class: 'banner ' + global.kind }, global.label),
    global.id !== lot.id ? el('p', {}, sheetLink(global.id, 'Fiche du lot global en PDF (une page par sous-lot)')) : '',
    el('p', { class: 'hint' }, 'Vérif la plus ancienne : ' + (global.last_verif ? fmtDateTime(global.last_verif) : 'jamais')),
    el('ul', { class: 'list tree' }, withoutHidden(global.lots).map((row) => el('li', {
      class: (row.counted ? row.effective.kind : 'group-lot') + (row.id === lot.id ? ' current' : ''),
      style: `padding-left:${12 + row.depth * 16}px;cursor:pointer`,
      onclick: () => openFiche('lot', row.id), // fiche du lot
    },
    el('div', { class: 'main' },
      el('div', { class: 'name' }, (row.depth ? '└ ' : '') + row.name + (row.id === lot.id ? ' (ce lot)' : '')),
      el('div', { class: 'sub' }, row.counted
        ? `${lotKind(row)} · vérif : ${row.last_verif ? fmtDateTime(row.last_verif) : 'jamais'}`
        : `${lotKind(row)} · regroupement`)),
    row.counted ? el('span', { class: 'tag ' + tags[row.effective.kind] }, row.effective.label.replace(/^[✔✘⚠] /, '').split(',')[0]) : null))),
  ];
}

// Items scannes sans etiquette de lot : le bouton de vérif devient « Sortir du stock »
function takingOut() {
  return !state.lot && scannedIids().size > 0;
}

function render() {
  const remaining = renderTodo();
  $('#count-todo').textContent = remaining;
  $('#count-done').textContent = renderDone();
  renderLot();
  // onglet « Scannés » (et ses outils) : gestion et admin seulement, les secouristes suivent « À scanner »
  const doneTab = document.querySelector('#tabs [data-tab="done"]');
  doneTab.hidden = !canSeeStock();
  if (doneTab.hidden && state.tab === 'done') switchTab('todo');
  $('#menu-quit').hidden = !state.lot;
  syncScanBubble();
  if ($('#label-needed').open && labelOk()) { $('#label-needed').close(); toast('Étiquette scannée : vous pouvez valider.'); }
  if (!state.user) { state.stock = null; state.tasks = null; state.events = null; state.push = null; state.pushDevices = null;
    debug.data = null; fun.data = null; if (fun.slide >= 0) $('#fun-show').close(); }
  renderHomePage();
  renderStock();
  const chip = $('#user-chip');
  chip.textContent = state.user ? `👤 ${state.user.prenom}`
    : declared() ? `👤 ${state.declaredName} (sans badge)` : guestInTeam() ? `👤 ${state.guest || 'Invité'}${state.guest ? ' (invité)' : ''}`
      : '👤 Non connecté';
  chip.classList.toggle('ok', !!state.user);
  chip.title = state.user && state.user.fonction ? state.user.fonction : '';
  // reassort, ou verif partielle (verif groupee dont certains lots sont complets) : bouton orange a cote de la validation
  const partial = state.busy ? [] : partialLots();
  const restock = !state.busy && (partial.length > 0 || onlyNewItems());
  $('#restock').hidden = !restock;
  $('#undo-last').hidden = !state.scanned.length || state.busy;
  if (partial.length) $('#restock').textContent = `Vérif partielle (${partial.map((lot) => lot.name_short || lot.name).join(', ')})`;
  else setLabel($('#restock'), 'restock', `Ajouter ${scannedIids().size} au lot (réassort)`);
  const validateButton = $('#validate');
  const recorded = state.lastVerif && state.lot && state.lastVerif.lotId === state.lot.id && !allScanned().length;
  const out = takingOut();
  const waiting = state.waitingTeam && !state.busy;
  // vérif à plusieurs : d'autres participants n'ont pas encore valide, on ne valide que sa partie
  const shared = !out && !recorded && live.others().some((row) => row.connected && !row.validated);
  validateButton.disabled = state.busy || recorded || waiting;
  validateButton.classList.toggle('primary', !out);
  validateButton.classList.toggle('warning', out);
  const missing = blockers(false);
  validateButton.title = state.busy ? 'Envoi de la vérif en cours.' : recorded ? 'Cette vérif est déjà enregistrée. Scannez un item pour en commencer une nouvelle.'
    : out ? 'Ces items ne sont dans aucun lot : ils sont enregistrés comme sortis du stock.'
      : missing.length ? 'Pour valider : ' + missing.join(', ') + '.'
        : 'Enregistre la vérif : les items scannés sont présents, ceux de « À scanner » sont signalés manquants.';
  $('#validate-hint').hidden = !shared && !waiting;
  $('#validate-hint').textContent = waiting ? 'En attente des autres participants' : 'Valider ma partie de la vérif';
  if (waiting) validateButton.title = 'Votre partie est validée. Scannez un item pour la reprendre.';
  const validateText = waiting ? 'Partie validée ✔' : state.busy ? 'Envoi…' : recorded ? 'Vérif enregistrée ✔'
    : out ? `Sortir ${nb(scannedIids().size, 'item')} du stock` : restock ? 'Vérif complète…'
      : missing.length ? 'Valider la vérif…' : 'Valider la vérif';
  setLabel(validateButton, validateText.startsWith('Valider') ? 'check' : '', validateText);
  if (inCascade() && !out) renderCascadeActions(validateButton);
  // lot scelle : sa vérif est le scan de l'etiquette du scelle (sealRows), rien a valider a la main
  const sealTarget = !out && state.lot && !state.busy ? (inCascade() ? levelLot() : sessionLots().length === 1 ? state.lot : null) : null;
  if (sealTarget && sealWaiting(sealTarget)) {
    validateButton.disabled = true;
    setLabel(validateButton, '', "Scannez l'étiquette du scellé");
    validateButton.title = `${sealTarget.name} est scellé : scannez l'étiquette collée sur le scellé.`;
    $('#restock').hidden = true;
  }
  live.publish(liveSnapshot(remaining));
  renderTeam();
  setTimeout(maybeVerifHelp, 0); // apres le rendu (et la fermeture eventuelle des popups de connexion)
  $('#scan-hint').textContent = readerOpen() ? 'Scannez votre badge' : out ? "Scannez l'étiquette d'un lot pour une vérif, ou sortez ces items du stock"
    : !state.lot ? "Visez l'étiquette d'un lot ou un item"
    : !hasIdentity() ? (state.declaredAllowed ? 'Scannez votre badge (ou touchez « Non connecté ») pour pouvoir valider'
      : 'Scannez votre badge pour pouvoir valider')
      : sealTarget && sealWaiting(sealTarget) ? "Scannez l'étiquette du scellé" : 'Scannez les items du lot';
}

// Boutons de la vérif en cascade : valider le lot en cours (et revenir), terminer au lot global ; vérif partielle ou
// reassort du lot en cours
function renderCascadeActions(validateButton) {
  const level = levelLot();
  const atRoot = level.id === state.lotId;
  const exclude = excludedFor(level.id);
  const scope = scopeOf(level.id, exclude);
  const entries = state.busy ? [] : scopeEntries(scope);
  const something = ownContent(level) || level.storage || entries.length > 0 || [...scope].some((id) => id !== level.id);
  const name = level.name.length > 22 ? level.name_short || level.name : level.name;
  validateButton.disabled = state.busy;
  const validateText = state.busy ? 'Envoi…' : !something ? (atRoot ? 'Terminer la vérif' : `Revenir (rien à valider)`)
    : atRoot ? `Valider ${name} et terminer` : `Valider ${name}`;
  setLabel(validateButton, validateText.startsWith('Valider') ? 'check' : '', validateText);
  validateButton.title = atRoot ? 'Enregistre la vérif du lot global et de ses sous-lots commencés ; les sous-lots non faits restent tels quels.'
    : `Enregistre la vérif de ${level.name} (et de ses sous-lots commencés), puis revient au lot qui le contient.`;
  $('#validate-hint').hidden = true;
  // vérif partielle du lot en cours (des lots sont complets, pas tous) ou reassort (que des items nouveaux)
  const plan = state.busy ? null : planSession();
  const rows = plan ? plan.lots.filter((row) => scope.has(row.lot.id)) : [];
  const complete = rows.filter((row) => row.complete && row.touched > 0);
  const partial = complete.length > 0 && rows.some((row) => !row.complete) ? complete.map((row) => row.lot) : [];
  const known = new Set(sessionLots().filter((lot) => scope.has(lot.id))
    .flatMap((lot) => [...(lot.items || []), ...(lot.missing_items || [])].map((item) => item.iid)));
  const iids = entriesPayload(entries).items;
  const restock = iids.length > 0 && !iids.some((iid) => known.has(iid));
  $('#restock').hidden = !(partial.length || restock);
  if (partial.length) $('#restock').textContent = `Vérif partielle (${partial.map((lot) => lot.name_short || lot.name).join(', ')})`;
  else setLabel($('#restock'), 'restock', level.storage ? `Ajouter ${iids.length} à ${name}` : `Ajouter ${iids.length} à ${name} (réassort)`);
  if (partial.length || restock) validateButton.textContent = `Vérif complète de ${name}…`;
}
