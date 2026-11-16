/* QRProtec - front web : detail d'une liste de materiel, recherches avec suggestions et editeur. */
'use strict';

function listIndex() {
  const data = lists.index;
  const choose = (show) => { lists.show = show; loadLists(); };
  // chefs, gestion et admins : creent des listes, voient toutes les listes et les modeles ; un secouriste ne voit
  // que les listes qui lui sont attribuees ou qu'il a ouvertes par leur lien ou leur QR code
  const canCreate = !!(data && data.can_create);
  const parts = [
    el('div', { class: 'row-head' }, el('h3', {}, 'Listes de matériel'),
      el('button', { type: 'button', onclick: loadLists }, icon('refresh'), lists.busy ? 'Chargement…' : 'Actualiser')),
    canCreate || (data && data.can_see_all) ? el('div', { class: 'segmented' },
      el('button', { type: 'button', class: lists.show === 'mine' ? 'active' : '', onclick: () => choose('mine') }, 'Mes listes'),
      canCreate ? el('button', { type: 'button', class: lists.show === 'templates' ? 'active' : '', onclick: () => choose('templates') }, 'Modèles') : '',
      data.can_see_all
        ? el('button', { type: 'button', class: lists.show === 'all' ? 'active' : '', onclick: () => choose('all') }, 'Toutes') : '') : '',
    canCreate ? el('button', { type: 'button', class: 'primary list-new', onclick: () => editList() },
      lists.show === 'templates' ? '＋ Nouveau modèle' : '＋ Nouvelle liste') : '',
    // tout le monde : liste de depart generee depuis un type de lot (un lot C, un B+…)
    lists.show === 'templates' ? '' : el('button', { type: 'button', class: 'secondary list-new', onclick: openDeparture },
      '🚑 Partir avec un lot'),
    lists.departure ? departurePicker() : '',
    el('p', { class: 'hint' }, canCreate || !data
      ? 'Une liste sert à demander à des secouristes de prendre du matériel précis pour un '
        + 'poste en particulier : les lots et le matériel à emporter, avec les quantités. Elle se partage par lien ou '
        + 'QR code et se vérifie en direct, à plusieurs. Validée, son matériel sort du stock. Un modèle se duplique '
        + 'pour les postes qui reviennent souvent. Ajoutez des secouristes à une liste pour qu\'elle apparaisse sur cette page, sur leur téléphone.'
      : 'Les listes de matériel qui vous sont attribuées par un chef, ou que vous avez ouvertes par leur lien ou leur '
        + 'QR code. Scannez le matériel demandé pour la vérifier, à plusieurs ; partagez-la par lien ou QR code.'),
  ];
  if (!data) {
    parts.push(el('p', { class: 'hint' }, lists.busy ? 'Chargement…' : 'Listes non chargées.'));
    return parts;
  }
  if (!data.lists.length) parts.push(el('p', { class: 'hint' }, lists.show === 'templates' ? 'Aucun modèle.'
    : canCreate ? 'Aucune liste.' : 'Aucune liste pour vous pour l\'instant.'));
  const listRow = (row) => el('li', { onclick: () => openList(row.id) },
    el('div', { class: 'main' },
      el('div', { class: 'name' }, row.name),
      el('div', { class: 'sub' }, [row.template ? '' : fmtListDate(row.date), row.owner_name, row.template ? nb(row.line_count, 'ligne')
        : `${row.done_count}/${nb(row.line_count, 'ligne prête', 'lignes prêtes')}`, 'modifiée ' + fmtDateTime(row.updated)].filter(Boolean).join(' · '))),
    el('span', { class: 'tag ' + (row.template ? 'gray' : LIST_TAGS[row.status] || 'blue') }, row.status_label));
  // listes rentrees (Rentrée, Validée, Terminée : la liste ne sert plus) repliees derriere un bouton [+] en bas,
  // fermees par defaut, comme les types a 0 attendu du stock : ce qui reste a faire d'abord
  const done = data.lists.filter((row) => !row.template && row.status === 'returned');
  const open = data.lists.filter((row) => !done.includes(row));
  if (open.length) parts.push(el('ul', { class: 'list' }, ...open.map(listRow)));
  else if (done.length) parts.push(el('p', { class: 'hint' }, 'Aucune liste en cours.'));
  if (done.length) {
    const unfolded = !!lists.doneOpen;
    parts.push(el('button', { type: 'button', class: 'upcoming-toggle', 'aria-expanded': unfolded ? 'true' : 'false',
      onclick: () => { lists.doneOpen = !unfolded; renderListsPage(true); } },
    el('span', { class: 'fold-box' }, unfolded ? '−' : '+'),
    `${pl(done.length, 'Liste rentrée', 'Listes rentrées')} (${done.length})`));
    if (unfolded) parts.push(el('ul', { class: 'list' }, ...done.map(listRow)));
  }
  return parts;
}

// Case d'un lot ou d'un materiel : pas cochable a la main, la toucher explique qu'il faut scanner
function listBox(list, line, done, title) {
  const text = done ? (line.kind === 'lot' ? 'Lot pris : son étiquette a été scannée.' : line.kind === 'lot_type' ? 'Lots pris : retirez-en un avec sa croix.'
    : 'Quantité prise : retirez un item avec sa croix.')
    : list.template ? 'Modèle : dupliquez-le, puis scannez le matériel dans la nouvelle liste.'
      : list.status !== 'draft' ? 'Pas pris à la sortie.'
        : line.kind === 'lot' ? `Scannez l'étiquette du lot « ${title} » ou touchez sa case pour le cocher.`
          : line.kind === 'lot_type' ? `Scannez l'étiquette d'un lot ${title} disponible ou touchez la case pour le choisir.`
          : line.unit ? `Scannez de quoi faire ${line.quantity} ${line.unit} de « ${title} » pris dans le stock pour la cocher.`
          : line.is_group ? `Scannez ${line.quantity} × « ${title} » (${(line.subtypes || []).map((row) => row.type_name).join(' ou ') || 'ses sous-types'}) pris dans le stock pour la cocher.`
          : `Scannez ${line.quantity > 1 ? `${line.quantity} items` : 'un item'} « ${title} » pris dans le stock pour la cocher.`;
  // lot a preparer : on peut le cocher sans scanner (le VPS dehors pendant qu'on prepare le materiel)
  const byHand = !done && !list.template && list.status === 'draft'
    && (line.kind === 'lot' || (line.kind === 'lot_type' && line.available && line.available.length));
  const unCheck = done && !list.template && list.status === 'draft'
    && (line.kind === 'lot' || (line.kind === 'lot_type' && line.scans.length === 1));
  const box = el('button', { type: 'button', class: 'list-box' + (done ? ' done' : '') + (byHand || unCheck ? ' by-hand' : ''), 'aria-label': text, onclick: async (event) => {
    event.stopPropagation();
    if (byHand) { openLotPick(line, title); return; }
    if (line.kind === 'type' && !done && list.status === 'draft' && !list.template) { openItemPick(line.id, title); return; }
    if (unCheck) {
      // case cochee touchee a nouveau : decocher, apres confirmation
      const lot = line.kind === 'lot' ? line.lot : line.scans[0].lot;
      const name = line.kind === 'lot' ? title : line.scans[0].lot_name;
      if (await confirmDialog(`Décocher ${name} ?`, `${name} n'est plus compté comme pris : il faudra rescanner son étiquette pour le cocher.`, 'Décocher', 'warning')) {
        listAction('unscan', { lot });
      }
      return;
    }
    document.querySelectorAll('#home-lists .list-bubble').forEach((node) => node.remove());
    const bubble = el('span', { class: 'list-bubble', role: 'status' }, text);
    box.parentNode.append(bubble);
    setTimeout(() => bubble.remove(), 3500);
  } }, done ? '✓' : '');
  return el('span', { class: 'list-box-wrap' }, box);
}

// Vérif recommandee sur un lot de la liste (rentre d'un poste, marque utilise, reassorti) : tant que la liste est a
// preparer, un bouton « Lancer la vérif » incite a la faire avant de partir, dans la liste et dans « Prêt au départ ? »
const listWantsVerif = (list) => list.status === 'draft' && !list.template;

// vérifs pas encore faites (cases « Vérif <lot> » du serveur : le lot pris, ou ses sous-lots qui en ont besoin) ;
// takenOnly : seulement pour les lots deja pris (« Prêt au départ ? » : ce qui n'est pas pris ne part pas)
function listVerifLots(list, takenOnly = false) {
  return (list.verifs || []).filter((row) => !row.done && (!takenOnly || row.taken))
    .map((row) => ({ id: row.lot, name: row.lot_name }));
}

// Case « Vérif <lot> » en haut de la liste, avant « Prendre <lot> » : cochee d'office quand le lot redevient vert
// apres sa vérif ; la toucher propose de lancer la vérif (Hippolyte, 2026-11-16)
function listVerifRow(list, row) {
  const done = !!row.done;
  const ask = async () => {
    if (done || !listWantsVerif(list)) return;
    const ok = await askDialog({
      title: `Vérif de ${row.lot_name}`,
      text: `La case se coche toute seule quand ${row.lot_name} redevient vert après sa vérif. `
        + 'Vous revenez à la liste une fois la vérif enregistrée.',
      choices: [{ label: 'Lancer la vérif', value: true, cls: 'primary' }],
    });
    if (ok) verifFromList(row.lot);
  };
  const box = el('button', { type: 'button', class: 'list-box' + (done ? ' done' : listWantsVerif(list) ? ' by-hand' : ''),
    'aria-label': done ? `Vérif de ${row.lot_name} faite` : `Lancer la vérif de ${row.lot_name}`,
    onclick: (event) => { event.stopPropagation(); ask(); } }, done ? '✓' : '');
  const state = (row.state && row.state.label || '').replace(/^[✔✘⚠] */, '');
  const sub = done ? `Vérifiée : ${row.lot_name} est vert`
    : (state ? state + ' · ' : '') + (row.root !== row.lot ? `dans ${row.root_name} · ` : '') + 'à faire avant de partir';
  return el('li', { class: 'static list-verif-line ' + (done ? 'ok' : 'warn') },
    el('span', { class: 'list-box-wrap' }, box),
    el('div', { class: 'main', onclick: ask }, el('div', { class: 'name' }, `Vérif ${row.lot_name}`), el('div', { class: 'sub' }, sub)),
    done ? el('b', { class: 'list-count done' }, '') : '');
}

// Quitte la liste pour la vérif du lot ; le compte rendu de la vérif propose de revenir a la liste
function verifFromList(lot) {
  if (!lists.list) return;
  const back = { id: lists.list.id, name: lists.list.name };
  stopListScan();
  clearInterval(lists.timer);
  lists.timer = 0;
  watchStop();
  document.body.classList.remove('home-mode', 'login-reader');
  state.homePage = 'scan';
  lists.verifBack = back;
  launchVerif(lot, '');
}

// Lignes a traiter en haut (ordre de la liste a egalite) : a preparer, ce qui n'est pas encore pris ou coche ;
// sortie, ce qui est pris et pas encore rentre. Un modele ou une liste rentree garde son ordre.
function listLinesByAttention(list) {
  // (les vérifs a faire ont leurs propres cases, au-dessus de toutes les lignes : listVerifRow)
  const todo = (line) => (list.template ? false
    : list.status === 'draft' ? (line.kind === 'task' ? !line.scanned : line.scanned < line.quantity)
      : list.status === 'validated' ? line.kind !== 'task' && !!line.scanned
        && (line.kind === 'lot' ? !line.back : line.back < line.scanned) : false);
  return [...list.lines].sort((a, b) => todo(b) - todo(a));
}

function listLineRows(list, line, backIids) {
  if (line.kind === 'task') {
    // tache libre : cochee a la verif de la liste (ni lot ni item)
    const editable = list.status === 'draft' && !list.template;
    return [el('li', { class: 'static ' + (line.scanned ? 'ok' : '') },
      el('label', { class: 'list-task' },
        el('input', { type: 'checkbox', checked: line.scanned ? '' : null, disabled: editable && !lists.busy ? null : '',
          onchange: (event) => listAction('check', { line: line.id, done: event.target.checked }) }),
        el('div', { class: 'main' }, el('div', { class: 'name' }, line.task),
          el('div', { class: 'sub' }, line.scanned ? `Fait ${fmtDateTime(line.done)}${line.done_by ? ' par ' + line.done_by : ''}` : 'Tâche à faire'))))];
  }
  const done = line.scanned >= line.quantity;
  // cases « Vérif <lot> » en haut : les lots deviennent « Prendre <lot> »
  const take = list.status === 'draft' && (list.verifs || []).length && line.kind !== 'type' ? 'Prendre ' : '';
  const name = line.kind === 'lot' ? line.lot_name : line.type_name;
  const title = take + name;
  // sac d'un B+ de referentiel (sans etiquette) : chaque sac se coche par son etiquette
  const sub = line.kind === 'lot' ? (line.part_of ? `${line.lot_type_name} · dans ${line.part_of_name}` : `Lot · ${line.lot_type_name}`)
    : line.kind === 'lot_type' ? `${nb(line.quantity, 'lot')} au choix : scannez son étiquette`
      // type a unites (200 mL de serum phy) : n'importe lequel de ses sous-types, compte en unites
      : line.unit ? `${line.quantity} ${line.unit} demandés` + ((line.subtypes || []).length
        ? ' : ' + line.subtypes.map((row) => `${row.type_name} (${row.fills} ${line.unit})`).join(' ou ') : '')
        // type parent sans unite (4 bandes) : n'importe lequel de ses sous-types, un item compte pour 1
        : line.is_group && (line.subtypes || []).length
          ? nb(line.quantity, 'demandé') + ' : ' + line.subtypes.map((row) => row.type_name).join(' ou ')
          : nb(line.quantity, 'demandé');
  // retour par la liste (anciennes listes : sortie, ou rentree avec des scans de retour) ; une liste validee
  // aujourd'hui est terminee sans retour a scanner
  const out = list.status === 'validated' || (list.status === 'returned' && !!(list.back || []).length);
  // un lot : sa case dit deja s'il est pris (pas de ✓ en double a droite)
  const count = line.kind === 'lot' ? '' : `${line.scanned}/${line.quantity}` + (line.unit ? ' ' + line.unit : '');
  const back = out && line.scanned ? (line.kind === 'lot' ? (line.back ? 'rentré' : 'pas rentré')
    : line.unit ? `${line.back}/${line.scanned} ${line.unit} rentrés` : `${line.back}/${nb(line.scanned, 'rentré')}`) : '';
  const rows = [el('li', { class: 'static ' + (out ? (line.kind === 'lot' ? line.back : line.back >= line.scanned) && line.scanned ? 'ok' : ''
    : done ? 'ok' : '') },
    // case visuelle comme pour les taches : cochee quand la ligne est prise, mais on coche en scannant
    listBox(list, line, done, name),
    el('div', { class: 'main' }, el('div', { class: 'name' }, title), el('div', { class: 'sub' }, sub + (back ? ' · ' + back : ''))),
    el('b', { class: 'list-count' + (done ? ' done' : '') }, count))];
  if (line.kind === 'type') {
    for (const scan of line.scans) {
      const returned = backIids.has(scan.iid);
      rows.push(el('li', { class: 'static list-scan-row' },
        el('div', { class: 'main' }, el('div', { class: 'sub' }, [idText(scan.iid), scan.peremption ? 'périme le ' + fmtDate(scan.peremption) : 'non périssable'].filter(Boolean).join(' · ')
          + ` · ${scan.by || '?'}` + (out ? (returned ? ' · ✓ rentré' : scan.status === 'missing' ? ' · utilisé' : '') : ''))),
        list.status === 'draft' ? el('button', { type: 'button', class: 'list-remove', 'aria-label': 'Retirer', title: 'Retirer de la liste',
          onclick: () => listAction('unscan', { item: scan.iid }) }, '✕') : ''));
    }
  } else if (line.kind === 'lot_type') {
    // n'importe quel lot de ce type : les lots pris, puis ceux qu'on peut encore prendre (valides en premier)
    const backLots = new Set((list.back || []).filter((scan) => scan.lot).map((scan) => scan.lot));
    for (const scan of line.scans) {
      rows.push(el('li', { class: 'static list-scan-row' },
        el('div', { class: 'main' }, el('div', { class: 'sub' }, `${scan.lot_name} · ${scan.by || '?'}`
          // B+ de referentiel commence : les sacs qui restent a scanner
          + ((scan.missing || []).length ? ` · reste à scanner : ${scan.missing.join(', ')}` : '')
          + (out ? ((scan.returned !== undefined ? scan.returned : backLots.has(scan.lot)) ? ' · ✓ rentré' : ' · pas rentré') : ''))),
        list.status === 'draft' ? el('button', { type: 'button', class: 'list-remove', 'aria-label': 'Retirer', title: 'Retirer de la liste',
          onclick: () => listAction('unscan', { lot: scan.lot }) }, '✕') : ''));
    }
    if (!done && line.available) {
      rows.push(el('li', { class: 'static list-scan-row' }, el('div', { class: 'main' }, el('div', { class: 'sub' },
        line.available.length ? 'Disponibles : ' + line.available.map((lot) => lot.name
          + (lot.state.kind === 'ok' ? '' : ` (${lot.state.label.replace(/^[✔✘⚠] */, '').toLowerCase()})`)).join(', ')
          : 'Aucun lot disponible : tous sont pris ou rangés pour de bon dans un autre lot.'))));
    }
  }
  // lot (ou sac d'un B+) : pas de croix ici, on le retire dans « Modifier la liste » ; on decoche en touchant la case
  return rows;
}

// Liste sortie : l'ecran du retour ne montre plus les cases et compteurs du depart (tout vert, « 3/3 », on croyait
// tout fait). Ce qui est parti est compte un par un (un lot, un item) et range en « Pas encore rentré » (en haut)
// ou « Rentré ». Les taches et ce qui n'a pas ete pris au depart n'ont rien a rentrer.
function listReturnSplit(list) {
  const backIids = new Set((list.back || []).filter((scan) => scan.iid).map((scan) => scan.iid));
  const backLots = new Set((list.back || []).filter((scan) => scan.lot).map((scan) => scan.lot));
  const split = { out: 0, back: 0, pending: [], returned: [] };
  const lot = (id, name, sub, home) => {
    split.out += 1;
    if (home) split.back += 1;
    (home ? split.returned : split.pending).push({ lot: id, name, sub, count: '', scans: [] });
  };
  for (const line of list.lines) {
    if (line.kind === 'task' || !line.scanned) continue;
    if (line.kind === 'lot') { lot(line.lot, line.lot_name, `Lot · ${line.lot_type_name}`, !!line.back); continue; }
    if (line.kind === 'lot_type') {
      for (const scan of line.scans) lot(scan.lot, scan.lot_name, `Lot · ${line.type_name}`, backLots.has(scan.lot));
      continue;
    }
    const total = line.scans.length;
    const gone = line.scans.filter((scan) => !backIids.has(scan.iid));
    const home = line.scans.filter((scan) => backIids.has(scan.iid));
    split.out += total;
    split.back += home.length;
    if (gone.length) split.pending.push({ line: line.id, name: line.type_name, count: `×${gone.length}`, scans: gone,
      sub: gone.length === total ? nb(total, 'sorti') : `${gone.length} sur ${nb(total, 'sorti')}` });
    if (home.length) split.returned.push({ line: line.id, name: line.type_name, count: `×${home.length}`, scans: home,
      sub: (home.length === total ? '' : `${home.length} sur ${total} · `) + 'remis à sa place' });
  }
  return split;
}

// Bandeau marine colle en haut de l'ecran du retour : on ne le confond pas avec la preparation du depart
function listReturnHead(list) {
  const out = `Sortie ${fmtOn(list.validated)}${list.validated_by ? ' par ' + list.validated_by : ''}`;
  return el('div', { class: 'return-head', role: 'status' },
    el('span', { class: 'return-head-icon', 'aria-hidden': 'true' }, '↩'),
    el('div', {}, el('b', {}, 'Retour de poste'),
      el('small', {}, lists.mode ? 'Scannez ce qui rentre : il est remis en stock.'
        : `${out}. Touchez « Retour : scanner » et scannez ce qui rentre.`)));
}

function listReturnProgress(split) {
  const all = !split.pending.length;
  const percent = split.out ? Math.round(100 * split.back / split.out) : 100;
  return el('div', { class: 'return-progress' + (all ? ' done' : '') },
    el('div', { class: 'return-progress-head' }, el('span', {}, all ? '✓ Tout est rentré' : '↩ Rentré'),
      el('b', {}, `${split.back}/${split.out}`)),
    el('div', { class: 'return-bar' }, el('i', { style: `width: ${percent}%` })),
    el('p', {}, all ? 'Touchez « Clôturer le retour » pour terminer.'
      : 'Ce qui rentre est remis en stock. À la clôture, les consommables pas rentrés sont comptés utilisés.'));
}

// Case d'une ligne du retour. Un lot pas encore rentre se coche comme au depart : la case ouvre la fenetre du
// lecteur (scanner son etiquette, ou « Rentrer sans scanner »). Le materiel se scanne toujours, un par un : un
// consommable ne se coche jamais a la main (il n'est remis en stock que si on le rescanne).
function returnBox(entry, home) {
  if (home) return el('span', { class: 'return-box done', 'aria-hidden': 'true' }, '✓');
  if (!entry.lot) {
    // materiel : la case ouvre la fenetre de scan (jamais coche a la main)
    return el('button', { type: 'button', class: 'return-box by-hand', 'aria-label': `Scannez « ${entry.name} » qui rentre`,
      onclick: (event) => { event.stopPropagation(); openItemPick(entry.line, entry.name, true); } }, '');
  }
  return el('button', { type: 'button', class: 'return-box by-hand', 'aria-label': `Scannez l'étiquette de ${entry.name} ou touchez la case`,
    onclick: (event) => { event.stopPropagation(); openLotPick({ id: `back-${entry.lot}`, kind: 'lot', lot: entry.lot }, entry.name, true); } }, '');
}

function listReturnLists(split) {
  // detail item par item (peremption, iid) : gestion et admins seulement, comme ailleurs sur le web, replie
  // derriere une fleche au bout de la ligne (comme les jauges de la fiche d'un lot)
  const folds = lists.returnFolds || (lists.returnFolds = new Set());
  const rows = (entries, home) => entries.flatMap((entry) => {
    const key = `${home ? 'back' : 'out'}:${entry.line}`;
    const foldable = detailed() && entry.scans.length > 0;
    const open = foldable && folds.has(key);
    const arrow = foldable ? el('button', { type: 'button', class: 'list-fold', 'aria-expanded': String(open),
      'aria-label': open ? 'Replier les items' : 'Afficher les items',
      onclick: () => { if (open) folds.delete(key); else folds.add(key); renderListsPage(); } }, open ? '▾' : '▸') : '';
    return [
    el('li', { class: 'static' + (home ? ' ok' : '') },
      el('span', { class: 'list-box-wrap' }, returnBox(entry, home)),
      el('div', { class: 'main' }, el('div', { class: 'name' }, entry.name), el('div', { class: 'sub' }, entry.sub)),
      el('b', { class: 'list-count' + (home ? ' done' : '') }, entry.count), arrow),
    ...(open ? entry.scans : []).map((scan) => el('li', { class: 'static list-scan-row' },
      el('div', { class: 'main' }, el('div', { class: 'sub' }, [idText(scan.iid),
        scan.peremption ? 'périme le ' + fmtDate(scan.peremption) : 'non périssable'].filter(Boolean).join(' · ')
        + (home ? ' · ↩ rentré' : ''))))),
    ];
  });
  const count = (entries) => entries.reduce((sum, entry) => sum + (entry.scans.length || 1), 0);
  const parts = [];
  if (split.pending.length) {
    parts.push(el('h3', { class: 'return-title' }, el('span', {}, 'Pas encore rentré'), el('span', {}, count(split.pending))),
      el('ul', { class: 'list' }, ...rows(split.pending, false)));
  }
  if (split.returned.length) {
    parts.push(el('h3', { class: 'return-title ok' }, el('span', {}, '✓ Rentré'), el('span', {}, count(split.returned))),
      el('ul', { class: 'list' }, ...rows(split.returned, true)));
  }
  if (!split.out) parts.push(el('p', { class: 'hint' }, 'Rien n\'est sorti avec cette liste.'));
  return parts;
}

function listDetail() {
  const list = lists.list;
  const back = [el('button', { type: 'button', class: 'text-link', onclick: () => openLists() }, '← Toutes les listes')];
  if (lists.error) return [...back, el('div', { class: 'banner bad' }, lists.error)];
  if (!list) return [...back, el('p', { class: 'hint' }, 'Chargement…')];
  const parts = [...back,
    el('div', { class: 'row-head' }, el('h3', {}, list.name),
      el('span', { class: 'tag ' + (list.template ? 'gray' : LIST_TAGS[list.status] || 'blue') }, list.status_label)),
    el('p', { class: 'hint' }, [list.template ? '' : fmtListDate(list.date), `Par ${list.owner_name}`, `modifiée ${fmtDateTime(list.updated)}`].filter(Boolean).join(' · ')),
    list.note ? el('p', { class: 'list-note' }, list.note) : ''];
  const buttons = [];
  const busy = lists.busy;
  const returnSplit = list.status === 'validated' && !list.template ? listReturnSplit(list) : null;
  if (list.template) {
    parts.push(el('div', { class: 'banner info' }, 'Modèle : dupliquez-le pour faire une liste à vérifier.'));
    buttons.push(el('button', { type: 'button', class: 'primary', disabled: busy || null, onclick: () => duplicateList(false) }, icon('template'), 'Utiliser ce modèle'));
  } else if (list.status === 'draft') {
    parts.push(el('div', { class: 'banner ' + (list.complete ? 'ok' : 'info') }, list.complete
      ? 'Tout est pris : touchez « Prêt au départ ».'
      : `${list.done_count}/${nb(list.line_count, 'ligne prête', 'lignes prêtes')}. Scannez l'étiquette des lots (ou touchez leur case), le matériel pris dans le stock, cochez les tâches.`));
    buttons.push(lists.mode
      ? el('button', { type: 'button', class: 'secondary', onclick: stopListScan }, 'Arrêter le scan')
      : el('button', { type: 'button', class: 'primary navy', onclick: startListScan }, '📷 Scanner'));
    buttons.push(el('button', { type: 'button', class: list.complete ? 'primary' : 'warning', disabled: busy || null, onclick: validateList },
      icon('check'), list.complete ? 'Prêt au départ' : 'Partir sans tout'));
  } else if (list.status === 'validated') {
    // ecran du retour : bandeau « Retour de poste » colle en haut (avant le nom de la liste), progression
    parts.splice(1, 0, listReturnHead(list));
    parts.push(listReturnProgress(returnSplit));
    buttons.push(lists.mode
      ? el('button', { type: 'button', class: 'secondary', onclick: stopListScan }, 'Arrêter le scan')
      : el('button', { type: 'button', class: 'primary', onclick: startListScan }, '📷 Retour : scanner'));
    buttons.push(el('button', { type: 'button', class: 'warning', disabled: busy || null, onclick: closeListReturn }, icon('lock'), 'Clôturer le retour'));
    if (list.can_reopen) buttons.push(reopenLink(busy));
  } else {
    const scannedBack = (list.back || []).length;
    parts.push(el('div', { class: 'banner ok' }, `${!list.went_out ? 'Terminée' : scannedBack ? 'Rentrée' : 'Validée'} ${fmtOn(list.returned)}`
      + `${list.returned_by ? ' par ' + list.returned_by : ''}.`
      + (list.went_out && !scannedBack ? ' Les lots pris sont en vérif recommandée ; le matériel sorti rentre en stock quand on le rescanne.' : '')
      + (list.can_reopen ? '' : ' La liste ne sert plus : dupliquez-la pour la refaire.')));
    if (list.can_reopen) buttons.push(reopenLink(busy));
  }
  if (lists.message && listScanning()) parts.push(el('div', { class: 'banner ' + lists.message.kind }, lists.message.text));
  parts.push(el('div', { class: 'list-buttons' }, ...buttons));
  const backIids = new Set((list.back || []).filter((scan) => scan.iid).map((scan) => scan.iid));
  if (returnSplit) parts.push(...listReturnLists(returnSplit));
  else {
    // « Modifier la liste » discret a cote du titre, au-dessus de la liste (ajouter, retirer un lot ou un sac)
    parts.push(el('div', { class: 'list-take-head' }, el('h3', {}, 'À prendre'),
      list.can_edit && list.status === 'draft' ? el('button', { type: 'button', class: 'text-link', onclick: () => editList(list) },
        icon('pencil'), 'Modifier la liste') : ''));
  }
  if (returnSplit) { /* liste sortie : ce qui rentre, plus les cases du depart */ } else if (!list.lines.length) parts.push(el('p', { class: 'hint' }, 'Liste vide.' + (list.can_edit ? ' Modifiez-la pour ajouter des lots et du matériel.' : '')));
  // a preparer : les vérifs a faire d'abord (une case par lot ou sous-lot qui en a besoin), puis ce qu'il y a a prendre
  if (!returnSplit) parts.push(el('ul', { class: 'list' }, ...(listWantsVerif(list) ? list.verifs || [] : []).map((row) => listVerifRow(list, row)),
    ...listLinesByAttention(list).flatMap((line) => listLineRows(list, line, backIids))));
  if (list.status === 'draft' || list.template) {
    const src = listQr(list.url);
    // Partage replie : le bouton de partage, et le QR code qui deplie le reste (QR, explication, lien cliquable)
    const panel = el('div', { class: 'list-qr-panel', hidden: lists.qrOpen !== list.id || null },
      src ? el('img', { class: 'qr', src, alt: 'QR code de la liste' }) : '',
      el('p', { class: 'hint' }, list.template ? 'Le lien ouvre le modèle.' : 'Le lien ou le QR code ouvre la liste pour la vérifier en direct, à plusieurs.'),
      el('p', { class: 'hint' }, el('a', { class: 'break', href: list.url }, list.url)));
    const qrButton = el('button', { type: 'button', class: 'secondary icon-only', title: 'QR code et lien',
      'aria-label': 'QR code et lien', 'aria-expanded': String(lists.qrOpen === list.id), onclick: () => {
        lists.qrOpen = lists.qrOpen === list.id ? '' : list.id;
        panel.hidden = lists.qrOpen !== list.id;
        qrButton.setAttribute('aria-expanded', String(!panel.hidden));
      } }, icon('qr'));
    parts.push(el('div', { class: 'list-share' },
      el('button', { type: 'button', class: 'secondary', onclick: () => shareList(list) },
        icon('share'), navigator.share ? 'Partager le lien' : 'Copier le lien'),
      qrButton), panel);
  }
  if (!list.template) parts.push(...listMembers(list));
  const more = [];
  if (list.can_edit && list.status === 'draft') more.push(el('button', { type: 'button', onclick: () => editList(list) }, icon('pencil'), 'Modifier'));
  if (list.can_duplicate) {
    if (!list.template) more.push(el('button', { type: 'button', disabled: busy || null, onclick: () => duplicateList(false) }, icon('copy'), 'Dupliquer'));
    more.push(el('button', { type: 'button', disabled: busy || null, onclick: () => duplicateList(true) }, list.template ? 'Dupliquer le modèle' : 'En faire un modèle'));
  }
  if (list.can_edit && list.status !== 'validated') more.push(el('button', { type: 'button', class: 'danger-text', disabled: busy || null, onclick: deleteList }, icon('trash'), 'Supprimer'));
  if (more.length) parts.push(el('h3', {}, 'Gérer'), el('div', { class: 'list-buttons' }, ...more));
  return parts;
}

// Secouristes attribues a la liste : elle apparait dans leurs listes. Qui modifie la liste les ajoute (recherche par
// prenom, noms courts sur le web) ou les retire ; un secouriste qui ouvre le lien ou le QR code s'y ajoute.
function listMembers(list) {
  const members = list.members || [];
  if (!list.can_edit && !members.length) return [];
  const parts = [el('h3', {}, 'Secouristes')];
  if (members.length) {
    parts.push(el('ul', { class: 'list' }, ...members.map((member) => el('li', { class: 'static' },
      el('div', { class: 'main' }, el('div', { class: 'name' }, member.name)),
      list.can_edit ? el('button', { type: 'button', class: 'list-remove', 'aria-label': 'Retirer', title: 'Retirer de la liste',
        disabled: lists.busy || null, onclick: () => listAction('members', { remove: [member.matricule] }) }, '✕') : ''))));
  } else {
    parts.push(el('p', { class: 'hint' }, 'Aucun secouriste : ajoutez-en pour que la liste apparaisse dans leurs listes.'));
  }
  if (list.can_edit) {
    const taken = new Set(members.map((member) => member.matricule));
    parts.push(memberPicker(taken, (user) => listAction('members', { add: [user.matricule] }, () => toast(`Liste attribuée à ${user.name}.`))));
  }
  parts.push(el('p', { class: 'hint' }, 'Un secouriste qui ouvre le lien ou le QR code de la liste l\'ajoute aussi à ses listes.'));
  return parts;
}

// Recherche avec suggestions sur telephone : le clavier cachait le champ et ses suggestions. Au focus, le champ
// remonte en haut de la partie visible (visualViewport) et la liste des suggestions s'arrete au-dessus du clavier.
// Une suggestion se choisit au relache du doigt sans glisser : on fait defiler la liste au doigt sans rien choisir
// ni fermer le clavier (avant, le simple appui choisissait ce qu'on touchait pour faire defiler).
function pickerTouch(input, results) {
  const viewport = window.visualViewport;
  const fit = () => {
    const box = input.parentElement;
    if (document.activeElement !== input || !box) return;
    const top = viewport ? viewport.offsetTop : 0;
    const height = viewport ? viewport.height : window.innerHeight;
    const keyboard = window.innerHeight - height > 80;
    // place sous le champ pour pouvoir le remonter meme en bas de page (cachee sous le clavier)
    box.classList.toggle('picker-open', keyboard);
    if (keyboard) {
      const rect = input.getBoundingClientRect();
      // la page defile dans un conteneur (#home-pages), pas dans la fenetre
      let scroller = box.parentElement;
      while (scroller && !/(auto|scroll)/.test(getComputedStyle(scroller).overflowY)) scroller = scroller.parentElement;
      // juste sous le haut visible : celui de la partie visible, ou du conteneur (sous la barre du haut)
      const goal = Math.max(top, scroller ? scroller.getBoundingClientRect().top : 0) + 8;
      if (Math.abs(rect.top - goal) > 4) (scroller || window).scrollBy(0, rect.top - goal);
    }
    const bottom = input.getBoundingClientRect().bottom;
    results.style.maxHeight = keyboard ? `${Math.max(120, top + height - bottom - 10)}px` : '';
  };
  input.addEventListener('focus', () => {
    fit();
    setTimeout(fit, 300);
    setTimeout(fit, 700);
  });
  input.addEventListener('blur', () => {
    if (input.parentElement) input.parentElement.classList.remove('picker-open');
    results.style.maxHeight = '';
  });
  if (viewport) viewport.addEventListener('resize', fit);
}

// Element d'une liste de suggestions : choisi au « click » (souris ou doigt), que le navigateur n'envoie pas quand
// le doigt a fait defiler la liste ; choisir au relache aurait laisse le click tomber sur ce qui est dessous apres le
// nouveau rendu (une croix). L'appui ne vole pas le focus du champ : le clavier reste ouvert pendant le defilement.
function pickerRow(attrs, onPick, ...children) {
  let start = null;
  return el('li', {
    ...attrs,
    onpointerdown: (event) => { start = { x: event.clientX, y: event.clientY, scroll: event.currentTarget.parentElement.scrollTop }; },
    onmousedown: (event) => event.preventDefault(),
    onclick: (event) => {
      const begin = start;
      start = null;
      event.preventDefault();
      if (begin && (Math.hypot(event.clientX - begin.x, event.clientY - begin.y) > 10
        || Math.abs(event.currentTarget.parentElement.scrollTop - begin.scroll) > 4)) return;
      onPick();
    },
  }, ...children);
}

// Recherche d'un secouriste par prenom sur le serveur (chefs, gestion, admins), resultats en direct
function memberPicker(taken, onPick) {
  const input = el('input', { type: 'search', class: 'picker-input', placeholder: '＋ Ajouter un secouriste : prénom…', autocomplete: 'off' });
  const results = el('ul', { class: 'picker-results', hidden: '' });
  let shown = [];
  let highlighted = 0;
  let timer = 0;
  let request = 0;
  const draw = (rows, empty) => {
    shown = rows;
    highlighted = Math.min(highlighted, Math.max(0, shown.length - 1));
    results.replaceChildren(...(shown.length ? shown.map((user, index) => pickerRow({
      class: index === highlighted ? 'active' : '' }, () => pick(user), el('span', {}, user.name)))
      : empty ? [el('li', { class: 'empty' }, empty)] : []));
    results.hidden = document.activeElement !== input || (!shown.length && !empty);
  };
  const pick = (user) => { input.value = ''; draw([], ''); input.blur(); onPick(user); };
  const search = async () => {
    const q = input.value.trim();
    const mine = ++request;
    if (!q) { draw([], ''); return; }
    try {
      const data = await api('checklists/users/', { user: badge(), q });
      if (mine !== request) return;
      draw(data.users.filter((user) => !taken.has(user.matricule)), 'Aucun secouriste');
    } catch (e) {
      if (mine === request) draw([], e.message);
    }
  };
  input.addEventListener('input', () => { highlighted = 0; clearTimeout(timer); timer = setTimeout(search, 200); });
  input.addEventListener('blur', () => { results.hidden = true; });
  pickerTouch(input, results);
  input.addEventListener('keydown', (event) => {
    if (event.key === 'ArrowDown' || event.key === 'ArrowUp') {
      event.preventDefault();
      highlighted = Math.max(0, Math.min(shown.length - 1, highlighted + (event.key === 'ArrowDown' ? 1 : -1)));
      draw(shown, '');
    } else if (event.key === 'Enter') {
      event.preventDefault();
      if (shown[highlighted]) pick(shown[highlighted]);
    } else if (event.key === 'Escape') {
      input.value = '';
      input.blur();
    }
  });
  return el('div', { class: 'picker member-picker' }, input, results);
}

// Champ de recherche avec resultats en direct (sans accents ni casse, debut de mots ou code), comme la recherche des
// types d'items du poste : fleches pour choisir, Entree ou toucher un resultat pour l'ajouter.
const searchKey = (text) => String(text || '').normalize('NFD').replace(/[\u0300-\u036f]/g, '').toLowerCase();
// items : { key, label, sub?, kind?, description? } ; kind ({ id, label, order }) : genre du resultat (lot, lot au
// choix, materiel) affiche en pastille quand une meme recherche melange plusieurs genres. La description (marques,
// autres noms) est cherchee aussi, en priorite moindre : apres tout ce qui correspond par le nom ou le code.
// score >= 1 par mot trouve dans le nom ou le code ; un mot trouve seulement dans la description fait passer
// le resultat sous tous ceux trouves par le nom (score < 1), -1 si un mot n'est nulle part
function searchScore(item, words) {
  const label = searchKey(item.label);
  const key = searchKey(item.key + ' ' + (item.sub || ''));
  const description = searchKey(item.description);
  const starts = label.split(/[^a-z0-9]+/);
  let total = 0;
  let inDescription = false;
  for (const word of words) {
    if (starts.some((part) => part.startsWith(word))) total += label.startsWith(word) ? 3 : 2;
    else if (label.includes(word) || key.includes(word)) total += 1;
    else if (description.includes(word)) inDescription = true;
    else return -1;
  }
  return inDescription ? total / 1000 : total;
}

function searchPicker(placeholder, items, onPick, limit = 8) {
  const input = el('input', { type: 'search', class: 'picker-input', placeholder, autocomplete: 'off' });
  const results = el('ul', { class: 'picker-results', hidden: '' });
  let shown = [];
  let highlighted = 0;
  // extrait de la description affiche quand c'est elle qui correspond : premiere ligne, 40 caracteres
  const excerpt = (text) => {
    const line = String(text || '').split('\n')[0].trim();
    return line.length > 40 ? line.slice(0, 40) + '…' : line;
  };
  const score = searchScore;
  const update = () => {
    const words = searchKey(input.value).split(/\s+/).filter(Boolean);
    const scored = items.map((item) => ({ item, score: score(item, words) })).filter((row) => row.score >= 0);
    const fromDescription = new Set(scored.filter((row) => words.length && row.score < 1).map((row) => row.item));
    shown = scored
      .sort((a, b) => b.score - a.score || (a.item.kind ? a.item.kind.order : 0) - (b.item.kind ? b.item.kind.order : 0)
        || a.item.label.localeCompare(b.item.label, 'fr')).slice(0, limit).map((row) => row.item);
    highlighted = Math.min(highlighted, Math.max(0, shown.length - 1));
    results.replaceChildren(...(shown.length ? shown.map((item, index) => pickerRow({
      class: index === highlighted ? 'active' : '' }, () => onPick(item), ...(item.kind ? [el('span', {}, item.label, item.sub ? el('small', {}, ' · ' + item.sub) : '',
      fromDescription.has(item) ? el('small', { class: 'picker-note' }, excerpt(item.description)) : ''),
      el('span', { class: 'picker-kind picker-kind-' + item.kind.id }, item.kind.label)]
      : [el('span', {}, item.label, fromDescription.has(item) ? el('small', { class: 'picker-note' }, excerpt(item.description)) : ''),
        item.sub ? el('small', {}, item.sub) : '']))) : [el('li', { class: 'empty' }, 'Aucun résultat')]));
    results.hidden = document.activeElement !== input;
  };
  input.addEventListener('input', () => { highlighted = 0; update(); });
  input.addEventListener('focus', update);
  input.addEventListener('blur', () => { results.hidden = true; });
  pickerTouch(input, results);
  input.addEventListener('keydown', (event) => {
    if (event.key === 'ArrowDown' || event.key === 'ArrowUp') {
      event.preventDefault();
      highlighted = Math.max(0, Math.min(shown.length - 1, highlighted + (event.key === 'ArrowDown' ? 1 : -1)));
      update();
    } else if (event.key === 'Enter') {
      event.preventDefault();
      if (shown[highlighted]) onPick(shown[highlighted]);
    } else if (event.key === 'Escape') {
      input.value = '';
      input.blur();
    }
  });
  return el('div', { class: 'picker' }, input, results);
}

// Quantite d'une ligne de l'editeur : petits − et + de part et d'autre du chiffre, qui reste modifiable au clavier
function qtyStepper(line) {
  const max = line.lot_types ? 99 : line.unit ? 100000 : 999; // type a unites : en mL
  const clamp = (value) => Math.max(1, Math.min(max, value));
  const input = el('input', { type: 'number', class: 'list-qty', min: '1', max: String(max), value: String(line.quantity), 'aria-label': 'Quantité',
    inputmode: 'numeric', enterkeyhint: 'done',
    oninput: (event) => { line.quantity = clamp(parseInt(event.target.value, 10) || 1); },
    // Entree (ou « OK » du clavier du telephone) : le chiffre est garde et le clavier se ferme
    onkeydown: (event) => { if (event.key === 'Enter') { event.preventDefault(); event.target.blur(); } },
    onchange: (event) => { event.target.value = String(line.quantity); } });
  const step = (delta, label) => el('button', { type: 'button', class: 'list-qty-step', 'aria-label': label, tabindex: '-1',
    // l'appui ne ferme pas le clavier ouvert sur un autre champ
    onmousedown: (event) => event.preventDefault(),
    onclick: () => { line.quantity = clamp((line.quantity || 1) + delta); input.value = String(line.quantity); } }, delta < 0 ? '−' : '+');
  return el('div', { class: 'list-qty-box' }, step(-1, 'Un de moins'), input, step(1, 'Un de plus'),
    line.unit ? el('span', { class: 'list-qty-unit' }, line.unit) : '');
}

function listEditor(edit) {
  const redraw = () => renderListsPage(true);
  const options = lists.options;
  const taken = new Set(edit.lines.map((line) => line.task ? 'task:' + line.task : line.lot_types ? 'lots:' + line.lot_types.join('+')
    : line.lot || 'type:' + line.type));
  const taskInput = el('input', { type: 'text', maxlength: '200', placeholder: "Ex : prendre des bonbons",
    onkeydown: (event) => { if (event.key === 'Enter') { event.preventDefault(); addTask(); } } });
  const addTask = () => {
    const task = taskInput.value.trim().replace(/\s+/g, ' ');
    if (!task) return;
    if (!taken.has('task:' + task)) edit.lines.push({ task, label: task });
    redraw();
  };
  // une seule recherche pour tout ce qui se scanne : lots precis, lots au choix (« 1 lot B+ », n'importe lequel
  // de ce type, choisi en scannant son etiquette) et materiel (type d'item, avec quantite)
  const KIND_LOT_TYPE = { id: 'lot-type', label: 'Lot au choix', order: 0 };
  const KIND_LOT = { id: 'lot', label: 'Lot', order: 1 };
  const KIND_TYPE = { id: 'type', label: 'Matériel', order: 2 };
  const addSelect = searchPicker('＋ Ajouter un lot ou du matériel : rechercher…', [
    ...(options ? options.lot_types || [] : []).filter((type) => !taken.has('lots:' + type.type))
      .map((type) => ({ key: type.type, label: type.name, kind: KIND_LOT_TYPE, description: type.description, row: type })),
    ...(options ? options.lots : []).filter((lot) => !taken.has(lot.id))
      .map((lot) => ({ key: lot.id, label: lot.name, sub: lot.lot_type_name, kind: KIND_LOT, description: lot.description, row: lot })),
    ...(options ? options.types : []).filter((type) => !taken.has('type:' + type.type))
      .map((type) => ({ key: type.type, label: type.name + (type.unit ? ` (${type.unit})` : ''), sub: type.type, kind: KIND_TYPE,
        description: type.description, row: type })),
  ], (pick) => {
    if (pick.kind === KIND_LOT) {
      // sac d'un B+ de referentiel (sans etiquette) : le B+ entier (un sac se retire ensuite par sa croix dans la liste)
      const lot = pick.row.unit || pick.row;
      if (pick.row.unit) toast(`${pick.row.name} fait partie de ${lot.name} : ${lot.name} entier est ajouté.`);
      // ses sacs, pour en retirer un ici (le B+ est alors separe, comme la croix d'un sac le faisait dans la liste)
      const parts = (options ? options.lots : []).filter((row) => row.unit && row.unit.id === lot.id)
        .map((row) => ({ lot: row.id, label: row.name, sub: row.lot_type_name }));
      if (!edit.lines.some((line) => line.lot === lot.id)) edit.lines.push({ lot: lot.id, label: lot.name, sub: lot.lot_type_name, parts });
    }
    else if (pick.kind === KIND_LOT_TYPE) edit.lines.push({ lot_types: [pick.row.type], label: pick.row.name, sub: 'Lot au choix', quantity: 1 });
    else edit.lines.push({ type: pick.row.type, label: pick.row.name, quantity: 1, unit: pick.row.unit || '' });
    redraw();
  }, 12);
  return [
    // retour en haut a gauche, comme « ← Toutes les listes » de la fiche d'une liste (= Annuler)
    el('button', { type: 'button', class: 'text-link', onclick: () => { lists.edit = null; renderListsPage(); } },
      edit.id ? '← Retour à la liste' : '← Toutes les listes'),
    el('h3', {}, edit.id ? 'Modifier la liste' : edit.template ? 'Nouveau modèle' : 'Nouvelle liste'),
    el('label', { class: 'field' }, 'Nom',
      el('input', { type: 'text', maxlength: '80', value: edit.name, placeholder: 'Ex : poste de secours du stade',
        oninput: (event) => { edit.name = event.target.value; } })),
    el('label', { class: 'field' }, 'Date',
      el('input', { type: 'date', value: edit.date, required: '',
        oninput: (event) => { edit.date = event.target.value; } })),
    el('label', { class: 'field' }, 'Remarque',
      el('textarea', { rows: '2', maxlength: '500', placeholder: 'Facultatif : rendez-vous, consignes…',
        oninput: (event) => { edit.note = event.target.value; } }, edit.note)),
    el('label', { class: 'check' }, el('input', { type: 'checkbox', checked: edit.template ? '' : null, disabled: edit.started ? '' : null,
      onchange: (event) => { edit.template = event.target.checked; } }),
    edit.started ? 'Modèle (impossible : liste déjà commencée)' : 'Modèle (se duplique, ne se vérifie pas)'),
    el('h3', {}, 'À prendre'),
    edit.lines.length ? '' : el('p', { class: 'hint' }, 'Ajoutez des lots (sacs, armoires), du matériel avec sa quantité et des tâches à cocher.'),
    el('ul', { class: 'list' }, ...edit.lines.flatMap((line, index) => [el('li', { class: 'static' },
      el('div', { class: 'main' }, el('div', { class: 'name' }, line.label),
        el('div', { class: 'sub' }, line.task ? 'Tâche à cocher' : line.lot ? `Lot · ${line.sub || ''}`
          : line.lot_types ? "Lot au choix : n'importe lequel de ce type" : 'Matériel')),
      line.lot || line.task ? '' : qtyStepper(line),
      el('button', { type: 'button', class: 'list-remove', 'aria-label': 'Retirer', onclick: () => { edit.lines.splice(index, 1); redraw(); } }, '✕')),
      // sacs d'un B+ de referentiel : la croix d'un sac separe le B+, les autres sacs restent chacun sur sa ligne
      ...(line.parts && line.parts.length > 1 ? line.parts.map((part) => el('li', { class: 'static list-scan-row' },
        el('div', { class: 'main' }, el('div', { class: 'sub' }, `${part.label} · dans ${line.label}`)),
        el('button', { type: 'button', class: 'list-remove', 'aria-label': `Retirer ${part.label}`, title: `Retirer ${part.label} (${line.label} séparé)`,
          onclick: () => {
            edit.lines.splice(index, 1, ...line.parts.filter((other) => other.lot !== part.lot && !edit.lines.some((row) => row.lot === other.lot))
              .map((other) => ({ lot: other.lot, label: other.label, sub: other.sub })));
            toast(`${line.label} séparé : ${part.label} retiré.`);
            redraw();
          } }, '✕'))) : [])])),
    options ? el('div', { class: 'list-add' }, addSelect) : el('p', { class: 'hint' }, 'Chargement des lots et du matériel…'),
    el('div', { class: 'list-add-task' }, taskInput, el('button', { type: 'button', onclick: addTask }, '＋ Tâche')),
    el('p', { class: 'hint' }, 'Une tâche se coche pendant la vérif de la liste (ni lot ni matériel).'),
    el('div', { class: 'list-buttons' },
      el('button', { type: 'button', class: 'secondary', onclick: () => { lists.edit = null; renderListsPage(); } }, 'Annuler'),
      el('button', { type: 'button', class: 'primary', disabled: lists.busy ? '' : null, onclick: saveList }, icon('save'), 'Enregistrer')),
  ];
}
