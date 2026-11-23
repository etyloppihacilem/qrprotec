/* QRProtec - front web : traitement des scans (items, paquets, etiquettes de lots, scelles). */
'use strict';

// Traitement des scans

function scannedIids() {
  const iids = new Set();
  for (const entry of allScanned()) {
    if (entry.error) continue;
    if (entry.kind === 'item') iids.add(entry.iid);
    for (const iid of entry.items || []) iids.add(iid);
  }
  return iids;
}

let scannedFiche = null; // item ou lot scanne dans le scanner qui a ouvert la fiche
async function handleCode(raw) {
  unlockAudio();
  vcache.refresh(55000); // cache de vérif a jour (sans attendre ; rien si charge il y a moins d'une minute)
  // popup « Passer a … » ignoree : le scan suivant vaut « Passer sans valider », puis il est traite
  if (leavePopup) { leavePopup('skip'); await new Promise((resolve) => setTimeout(resolve, 50)); }
  if (window.douchette && window.douchette.matches(raw)) { window.douchette.show(); return; }
  // code d'action rapide des postes (qrprotec:poste/..., ex. rouleau en place) : sans effet sur le telephone
  if (/^\s*qrprotec:poste\//i.test(raw)) return;
  const scan = parseCode(raw);
  // grande fenetre de connexion : seulement la carte (ou le lien de deblocage du PIN)
  if (gateOpen() && scan.kind !== 'user' && scan.kind !== 'pinreset') {
    feedback.bad();
    toast("Scannez d'abord votre carte de secouriste (le QR code de votre carte).", true);
    return;
  }
  // page d'une liste de materiel a verifier ou a rentrer : l'etiquette d'un lot ou d'un item y compte
  if (listScanning() && (scan.kind === 'item' || scan.kind === 'lot')) { listScanCode(scan); return; }
  if (scan.kind === 'liste') { feedback.info(); if (readerOpen()) closeReader(); openList(scan.id); return; }
  // fenetre des taches ouverte et autre code scanne : l'item reste non valide (le rescanner reprend ses taches)
  if (taskPopup && !(scan.kind === 'item' && scan.id === taskPopup.entry.iid)) finishTasks(false);
  // fiche de desactivation affichee : rescanner l'item passe a ses taches, un autre code le laisse non valide
  if (replacePopup) {
    if (scan.kind === 'item' && scan.id === replacePopup.iid) { closeReplace(true); return; }
    closeReplace(false);
  }
  // scanner sans vérif en cours et pile vide : l'etiquette d'un item ou d'un lot ouvre sa fiche en grand, comme
  // le lien ouvert avec l'appareil photo du telephone (« Lancer une vérif » y ramene au scanner)
  if (!inHome() && !ficheOpen() && !taskPopup && ((scan.kind === 'item' && !scan.task) || scan.kind === 'lot')
      && !checkRunning() && !allScanned().length) {
    feedback.info();
    openFiche(scan.kind, scan.id, { key: scan.key || '', label: scan.kind === 'lot' });
    scannedFiche = scan;
    return;
  }
  // etiquette du scelle hors vérif (scanner ou petit lecteur de l'accueil) : fiche du lot scelle, sans bouton de
  // vérif tant que le scelle est intact. Pendant une vérif, elle compte le lot comme verifie (scanSeal).
  if (scan.kind === 'seal' && !taskPopup && !checkRunning() && !allScanned().length) {
    if (readerOpen()) closeReader();
    openFiche('lot', scan.id, { seal: scan.key });
    return;
  }
  // petit lecteur de l'accueil : un badge connecte et l'on reste sur l'accueil, tout autre code ouvre le scanner
  if (readerOpen() && scan.kind !== 'user' && scan.kind !== 'sealopen') enterScanner(false);
  switch (scan.kind) {
    case 'item': return scanItem(scan);
    case 'lot': return scanLot(scan);
    case 'user': return scan.temp ? tempLogin(scan.id, scan.temp) : login(scan.id, scan.key);
    case 'pack': return scanPack(scan);
    case 'seal': return scanSeal(scan);
    case 'sealopen': return openSeal(scan);
    case 'pinreset': return startPinReset(scan.id, scan.key);
    case 'remote':
      // QR code affiche par le poste : ce telephone devient sa douchette
      feedback.info();
      toast('Connexion au poste…');
      location.href = new URL('scanner' + scan.search, document.baseURI).href;
      return;
    default:
      feedback.bad();
      showInfo('bad', 'Code non reconnu', raw.length > 80 ? raw.slice(0, 80) + '…' : raw);
  }
}

async function scanItem(scan) {
  // taches de cet item deja affichees : elles se cochent une a une (tout valider d'un 2e scan : au poste seulement,
  // la camera du telephone relit l'etiquette avant qu'on ait lu les taches)
  if (taskPopup && taskPopup.entry.iid === scan.id) {
    feedback.task();
    toast('Cochez toutes les tâches pour valider cet item.');
    return;
  }
  const existing = state.scanned.find((entry) => entry.kind === 'item' && entry.iid === scan.id);
  if (existing) {
    // doublon : ignore, sans son ni vibration
    showInfo('ok', '✓ Déjà scanné', (existing.info && existing.info.type_name) || idText(scan.id));
    return;
  }
  const shared = live.entries().find((entry) => entry.kind === 'item' && entry.iid === scan.id);
  if (shared) {
    showInfo('ok', `✓ Déjà scanné par ${shared.who || 'un autre participant'}`, (shared.info && shared.info.type_name) || idText(scan.id));
    return;
  }
  scannedAgain();
  state.lastVerif = null; // un nouveau scan commence une nouvelle verif
  const expired = !!(scan.peremption && scan.peremption < today());
  // cache de vérif : l'item est connu tout de suite, le serveur confirme ensuite (confirmItem)
  const cached = vcache.item(scan.id);
  const entry = { code: scan.code, kind: 'item', iid: scan.id, expired: expired || !!(cached && cached.expired), info: cached,
                  error: '', place: state.lot ? currentPlace() : '', at: new Date().toISOString() };
  if (scan.task) {
    // etiquette de taches : l'item n'est compte qu'une fois ses taches validees (fenetre des taches)
    feedback.task();
    if (!cached) {
      showInfo('task', 'Étiquette de tâches', 'Chargement des tâches…');
      try {
        entry.info = await api(`items/${encodeURIComponent(scan.id)}/`);
        if (entry.info.expired) entry.expired = true;
      } catch (e) {
        if (e.status === 404) entry.error = 'Inconnu dans la base';
      }
    } else confirmItem(entry);
    if (entry.info && !entry.error && (entry.info.tasks || []).length) {
      if (entry.expired) feedback.bad();
      openTasks(entry);
      return;
    }
  }
  await addItem(entry, scan, !!entry.info || !!entry.error);
  if (cached && !scan.task) confirmItem(entry);
}

// Item scanne venu du cache de vérif : le serveur donne son etat actuel en arriere-plan. Seule une difference qui
// compte (nouveau signalement, desactivation, peremption, statut, lot) est appliquee et signalee. Hors connexion :
// rien, le cache fait foi jusqu'a l'envoi.
async function confirmItem(entry) {
  let fresh;
  try {
    fresh = await api(`items/${encodeURIComponent(entry.iid)}/`);
  } catch (e) {
    if (e.status === 404) { entry.error = 'Inconnu dans la base'; feedback.bad(); showItemInfo(entry); save(); render(); }
    return;
  }
  itemChanged(entry, fresh);
}

// Champs d'un item qui changent ce que la vérif en dit
const itemFacts = (info) => JSON.stringify(info ? [info.expired, info.disabled, info.unusable, info.status, info.location,
  (info.reports || []).map((report) => [report.id, report.status, report.usable, report.comment, report.reply]),
  info.tasks || []] : null);

// Etat d'un item scanne qui a change au serveur depuis le cache : nouvelle info appliquee, bip et popup s'il devient
// a remplacer ou qu'un signalement arrive. Renvoie true si quelque chose a change.
function itemChanged(entry, fresh) {
  if (!fresh || itemFacts(entry.info) === itemFacts(fresh)) {
    if (fresh) entry.info = fresh;
    return false;
  }
  const before = entry.info;
  entry.info = fresh;
  if (fresh.expired && !entry.expired) { entry.expired = true; feedback.bad(); }
  const known = new Set(((before && before.reports) || []).map((report) => report.id));
  if (fresh.reported && (fresh.reports || []).some((report) => !known.has(report.id))) {
    alertReports('item:' + fresh.iid, `Item signalé : ${fresh.type_name}`, fresh.reports);
  } else if (state.scanned[state.scanned.length - 1] === entry) showItemInfo(entry);
  save();
  render();
  return true;
}

// Item scanne ajoute a la vérif. Un item dont le type a des taches n'y entre qu'une fois ses taches validees.
async function addItem(entry, scan, loaded = !!entry.info || !!entry.error) {
  state.scanned.push(entry);
  // item a taches deja connu (cache de vérif) : bip des taches plus bas, pas celui d'un item valide
  const tasksAhead = !scan.task && !entry.tasksDone && !!entry.info && (entry.info.tasks || []).length > 0;
  if (entry.expired || entry.error) feedback.bad(); else if ((!scan.task || entry.tasksDone) && !tasksAhead) feedback.good();
  showInfo(entry.expired ? 'bad' : 'ok', entry.expired ? badLabel(entry.info) : 'Item scanné',
    scan.peremption ? 'Péremption : ' + fmtDate(scan.peremption) : 'Non périssable');
  render();
  if (!loaded) {
    try {
      entry.info = await api(`items/${encodeURIComponent(scan.id)}/`);
      if (entry.info.expired && !entry.expired) { entry.expired = true; feedback.bad(); }
    } catch (e) {
      if (e.status === 404) { entry.error = 'Inconnu dans la base'; feedback.bad(); }
    }
  }
  // etiquette d'item d'un item a taches : retire des scans tant que ses taches ne sont pas validees
  if (!scan.task && entry.info && !entry.error && !entry.tasksDone && (entry.info.tasks || []).length) {
    const index = state.scanned.indexOf(entry);
    if (index >= 0) state.scanned.splice(index, 1);
    if (!entry.expired) feedback.task();
    openTasks(entry);
    return;
  }
  const info = entry.info;
  if (info && info.reported) alertReports('item:' + info.iid, `Item signalé : ${info.type_name}`, info.reports);
  // premier scan sans vérif en cours d'un item range dans un lot : c'est la vérif de ce lot qui commence
  if (!state.lot && state.scanned.length === 1 && state.scanned[0] === entry && info && info.location
      && !info.in_stock && info.status === 'active') {
    state.lotKey = ''; state.extra = []; state.lastVerif = null;
    await loadLot(info.location);
    if (state.lot && lotById(info.location)) {
      // vérif en cascade : celle du lot global, en entrant dans le sous-lot de l'item
      if (inCascade()) enterLevel(info.location, true); else state.current = state.lotId;
      entry.place = currentPlace();
      save();
      render();
      showInfo('lot', `Vérif de ${levelLot().name}`, `Commencée par le scan d'un de ses items (${info.type_name}).`,
        'Scannez les autres items du lot.', discardAdvice(entry));
      return;
    }
  }
  // vérif en cascade : un item est compte dans le lot ou l'on est (on l'y a trouve), meme range ailleurs dans la
  // vérif ; on ne change de lot qu'en scannant son etiquette. Seul un item d'un sous-lot du lot en cours fait entrer
  // dans ce sous-lot (on n'est encore dans aucun d'eux), sauf s'il a deja ete valide.
  const where = inCascade() && info && !entry.error && info.location ? lotById(info.location) : null;
  if (where && !isUnder(where.id, levelId())) {
    entry.place = currentPlace();
    showInfo(entry.expired ? 'bad' : 'ok', entry.expired ? badLabel(info) : 'Item scanné',
      `${info.type_name} est rangé dans ${where.name} : il est compté dans ${levelLot().name}, où vous l'avez trouvé, et y sera déplacé.`,
      discardAdvice(entry));
    save();
    render();
    return;
  }
  if (where && state.done[where.id]) {
    const index = state.scanned.indexOf(entry);
    if (index >= 0) state.scanned.splice(index, 1);
    feedback.warn();
    showInfo('warn', `Déjà vérifié – ${where.name}`,
      `${info.type_name} : ${where.name} a été validé ${fmtOn(state.done[where.id].at)}, cet item n'est pas recompté.`,
      `Scannez l'étiquette de ${where.name} pour le refaire.`);
    save();
    render();
    return;
  }
  if (where && where.id !== levelId() && isUnder(where.id, levelId())) {
    enterLevel(where.id, true);
    entry.place = where.id;
    showInfo('lot', `📦 Entrée dans ${where.name}`, `${info.type_name} est rangé dans ${where.name}.`,
      `Scannez son contenu, puis validez-le pour revenir à ${(lotById(parentIdOf(where)) || state.lot).name}.`,
      discardAdvice(entry));
    save();
    render();
    return;
  }
  if (state.scanned[state.scanned.length - 1] === entry) showItemInfo(entry);
  save();
  render();
}

// Fenetre des taches d'un item (etiquette de taches) : toutes cochees et validees, l'item compte comme vu
let taskPopup = null; // {entry, scan}
// Item a remplacer (perime, desactive, signale inutilisable) : sa fiche (peremption ou desactivation) s'affiche
// d'abord, ses taches ensuite (bouton « Voir les tâches »)
function openTasks(entry) {
  if (entry.expired && !entry.replaceSeen) { openReplace(entry); return; }
  showTasks(entry);
}

let replacePopup = null; // entry a remplacer dont la fiche est affichee avant ses taches
function openReplace(entry) {
  const info = entry.info;
  replacePopup = entry;
  $('#replace-item-title').textContent = info.type_name;
  $('#replace-item-sub').textContent = idText(entry.iid);
  const label = badLabel(info);
  $('#replace-item-banner').textContent = '✘ ' + cap(label.toLowerCase()) + ' : à remplacer';
  const facts = [];
  if (info.disabled) {
    facts.push(el('p', {}, el('b', {}, 'Désactivé : '), info.disabled_reason || '–', el('br'),
      el('span', { class: 'hint' }, [info.disabled_at ? cap(fmtOn(info.disabled_at, { long: true })) : '',
        info.disabled_by ? 'par ' + info.disabled_by : ''].filter(Boolean).join(' '))));
  }
  if (info.unusable && !info.disabled) facts.push(el('p', {}, el('b', {}, 'Signalé inutilisable.')));
  if (info.peremption) facts.push(el('p', {}, el('b', {}, 'Péremption : '), fmtDate(info.peremption)));
  $('#replace-item-facts').replaceChildren(...facts);
  showInfo('bad', label + ' – ' + info.type_name, 'Item à remplacer.');
  const dialog = $('#replace-item');
  if (!dialog.open) dialog.showModal();
}

function closeReplace(next) {
  const entry = replacePopup;
  replacePopup = null;
  const dialog = $('#replace-item');
  if (dialog.open) dialog.close();
  if (!entry) return;
  if (!next) {
    showInfo('warn', 'Item non validé : ' + entry.info.type_name, badLabel(entry.info) + ', à remplacer. Rescannez-le pour voir ses tâches.');
    return;
  }
  entry.replaceSeen = true;
  feedback.task();
  showTasks(entry);
}

$('#replace-item-next').addEventListener('click', () => closeReplace(true));
$('#replace-item-cancel').addEventListener('click', () => closeReplace(false));
$('#replace-item').addEventListener('cancel', (event) => { event.preventDefault(); closeReplace(false); });

function showTasks(entry) {
  const dialog = $('#tasks');
  const tasks = entry.info.tasks;
  taskPopup = { entry, scan: parseCode(entry.code) };
  $('#tasks-title').textContent = entry.info.type_name;
  $('#tasks-sub').textContent = [idText(entry.iid), entry.expired ? badLabel(entry.info) : ''].filter(Boolean).join(' · ');
  const boxes = tasks.map((task) => el('input', { type: 'checkbox' }));
  const update = () => { $('#tasks-ok').disabled = !boxes.every((box) => box.checked); };
  for (const box of boxes) box.addEventListener('change', update);
  $('#tasks-list').replaceChildren(...tasks.map((task, index) => el('li', {}, el('label', {}, boxes[index], ' ', task))));
  update();
  showInfo('task', 'Tâches : ' + entry.info.type_name, `${nb(tasks.length, 'tâche')} à compléter pour valider cet item.`);
  if (!dialog.open) dialog.showModal();
}

function finishTasks(done) {
  if (!taskPopup) return;
  const { entry, scan } = taskPopup;
  taskPopup = null;
  const dialog = $('#tasks');
  if (dialog.open) dialog.close();
  if (!done) {
    showInfo('warn', 'Item non validé : ' + entry.info.type_name, 'Toutes ses tâches doivent être cochées. Rescannez-le pour les reprendre.');
    return;
  }
  entry.tasksDone = true;
  addItem(entry, { ...scan, task: true }, true);
}

$('#tasks-form').addEventListener('submit', (event) => { event.preventDefault(); finishTasks(true); });
$('#tasks-cancel').addEventListener('click', () => finishTasks(false));
$('#tasks').addEventListener('cancel', (event) => { event.preventDefault(); finishTasks(false); });

function showItemInfo(entry) {
  const info = entry.info;
  if (entry.error) { showInfo('bad', entry.error, idText(entry.iid)); return; }
  if (!info) return;
  const inLot = info.location && sessionLots().some((lot) => lot.id === info.location);
  const where = inLot ? 'Dans ' + (isMulti() ? 'le lot ' + info.location_name : 'ce lot')
    : info.status === 'out' || info.status === 'loose' ? 'Hors du stock' : info.in_stock ? 'En stock · ' + (info.location ? info.location_name : 'sans rangement')
      : info.location ? 'Rangé dans : ' + info.location_name : 'En stock · sans rangement';
  const status = info.status === 'out' ? (info.out_until
    ? `Sorti du stock, compté utilisé le ${fmtDate(info.out_until)} s'il ne revient pas` : 'Sorti du stock (non consommable)')
    : { missing: 'Signalé disparu', deleted: 'Marqué supprimé', replaced: 'Déjà remplacé',
      loose: 'Hors lot (retiré de son lot), pas encore en stock' }[info.status];
  const late = [info.revision_late ? `révision (prévue le ${fmtDate(info.revision_due)})` : '',
    info.disinfection_late ? `désinfection (prévue le ${fmtDate(info.disinfection_due)})` : ''].filter(Boolean);
  const revision = late.length ? `⚠ ${cap(late.join(' et '))} en retard` + (entry.expired ? '' : ', toujours utilisable') : '';
  showInfo(entry.expired ? 'bad' : status || revision ? 'warn' : 'ok',
    (entry.expired ? badLabel(info) + ' – ' : '') + info.type_name,
    info.peremption ? 'Péremption : ' + fmtDate(info.peremption) : 'Non périssable',
    where + (status ? ' · ' + status : ''), revision, discardAdvice(entry));
}

// Perime, inutilisable ou desactive (pas une revision en retard) : la vérif le retire du lot, a jeter, des que le
// lot a son minimum d'items frais de ce type, meme deja presents (services.perform_verif) ; sinon a remplacer
function discardAdvice(entry) {
  const info = entry.info;
  if (!entry.expired || (info && !info.outdated && !info.unusable && !info.disabled)) return '';
  const plan = planSession();
  const row = plan.lots.find((candidate) => candidate.lot.id === plan.target[entry.iid]);
  // type a unites : besoin et frais en unites de l'attendu (200 mL de serum phy)
  const type = row ? mainRow(rowWeights(row.lot.requirements, entry.iid.slice(0, 6)), entry.iid.slice(0, 6))[0]
    : entry.iid.slice(0, 6);
  const need = row ? row.required[type] || 0 : 0;
  const have = row ? row.fresh[type] || 0 : 0;
  const inLot = row && [...(row.lot.items || []), ...(row.lot.missing_items || [])].some((item) => item.iid === entry.iid);
  if (row && !inLot) return '🗑 Ne le mettez pas dans le lot : jetez-le.';
  if (have >= need) return '🗑 Retirez-le du lot et jetez-le : le lot a son compte sans lui.';
  return `🗑 À remplacer : jetez-le dès qu'un item non périmé le remplace (non ${pl(need, 'périmé')} : ${have}/${need}).`;
}

// Scan (item ou paquet) perime que la vérif retirera du lot, a jeter (planSession().discard)
function discardedEntry(plan) {
  return (entry) => !entry.error && !!entry.expired && (entry.kind === 'item' ? plan.discard.has(entry.iid)
    : entry.kind === 'pack' && (entry.items || []).length > 0 && entry.items.every((iid) => plan.discard.has(iid)));
}

async function scanPack(scan) {
  try {
    // cache de vérif : le paquet est connu tout de suite, le serveur confirme ensuite (confirmPack)
    const cached = vcache.pack(scan.id);
    const pack = cached || await api(`packs/${encodeURIComponent(scan.id)}/`);
    // perime, ou entierement desactive (rappel produit) : a remplacer
    const expired = !!(pack.peremption && parseDate(pack.peremption) < today()) || (pack.disabled > 0 && pack.disabled >= pack.count);
    if (allScanned().some((entry) => entry.kind === 'pack' && entry.code === scan.code)) {
      showInfo('ok', '✓ Déjà scanné', `Paquet : ${pack.count} × ${pack.type_name}`);
      return;
    }
    scannedAgain();
    state.lastVerif = null;
    const entry = { code: scan.code, kind: 'pack', iid: '', info: pack, expired, error: '', items: pack.items,
                    place: state.lot ? currentPlace() : '', at: new Date().toISOString() };
    state.scanned.push(entry);
    if (cached) confirmPack(entry, scan.id);
    if (expired) feedback.bad(); else feedback.good();
    alertReports('pack:' + pack.id, `Paquet signalé : ${pack.count} × ${pack.type_name}`, pack.reports);
    showInfo(expired ? 'bad' : 'ok', `Paquet : ${pack.count} × ${pack.type_name}`,
      pack.peremption ? 'Péremption : ' + fmtDate(pack.peremption) : 'Non périssable',
      pack.opened ? 'Paquet ouvert ' + fmtOn(pack.opened) : 'Paquet fermé');
    save();
    render();
  } catch (e) {
    feedback.bad();
    showInfo('bad', 'Paquet inconnu', e.message);
  }
}

// Paquet scanne venu du cache de vérif : etat actuel demande au serveur, seule une difference est signalee
async function confirmPack(entry, id) {
  let fresh;
  try {
    fresh = await api(`packs/${encodeURIComponent(id)}/`);
  } catch (e) {
    return;
  }
  const facts = (pack) => JSON.stringify([pack.disabled, pack.opened, pack.items,
    (pack.reports || []).map((report) => [report.id, report.status, report.usable])]);
  const before = entry.info;
  entry.info = fresh;
  entry.items = fresh.items;
  if (facts(before) === facts(fresh)) return;
  const expired = !!(fresh.peremption && parseDate(fresh.peremption) < today()) || (fresh.disabled > 0 && fresh.disabled >= fresh.count);
  if (expired && !entry.expired) { entry.expired = true; feedback.bad(); }
  const known = new Set((before.reports || []).map((report) => report.id));
  if ((fresh.reports || []).some((report) => !known.has(report.id))) {
    alertReports('pack:' + fresh.id, `Paquet signalé : ${fresh.count} × ${fresh.type_name}`, fresh.reports);
  }
  save();
  render();
}

async function scanLot(scan) {
  if (state.lot && state.lotId !== scan.id && await joinVerif(scan)) return;
  // meme lot global (vérif en cascade) : on y reste, sinon c'est une nouvelle vérif
  const same = !!state.lot && !!lotById(scan.id);
  if (state.lot && !same) toast(`Lot changé : ${state.lot.name} → nouveau lot`);
  if (scan.key && !(await keyAccepted(scan.id, scan.key))) return;
  if (!same) { state.lotKey = ''; state.lastVerif = null; state.extra = []; resetCascade(); }
  await loadLot(scan.id);
  if (!state.lot) return;
  if (scan.key) setKey(scan.id, scan.key);
  if (scan.code) addLabel(scan.id);
  if (inCascade()) enterLevel(scan.id, true); else state.current = scan.id;
  state.askPlace = false;
  feedback.info();
  showLotInfo();
  if (!state.scanned.length) switchTab('todo');
  flushSeals();
}

// Etiquette cle scannee : celle du lot de la vérif, ou d'un sous-lot du lot global (vérif en cascade)
function setKey(id, key) {
  if (id === state.lotId) state.lotKey = key; else state.keys[id] = key;
}

// Etiquette (publique ou cle) d'un lot scannee pendant la vérif : la vérif est bien celle de ce lot (labelMissing)
function addLabel(id) {
  if (id && !state.labels.includes(id)) state.labels.push(id);
}

// Une vérif est en cours : scans (les notres ou ceux des autres participants), etiquette cle, vérif lancee depuis
// la fiche du lot, ou vérif rejointe
const checkRunning = () => !!state.lot && (allScanned().length > 0 || verifInProgress()
  || (state.launched === state.lotId && !state.lastVerif) || live.inTeam() || cascadeActive());

// Lot ou sont ranges les items venus du stock scannes maintenant : derniere etiquette de lot scannee pendant la
// vérif (services.assign_items). Lot principal par defaut.
function currentPlace() {
  if (inCascade()) return levelId(); // vérif en cascade : le lot ou l'on est entre
  if (state.askPlace) return ''; // vérif rejointe, sous-lot pas encore indique : regle par defaut
  return sessionLots().some((lot) => lot.id === state.current) ? state.current : state.lotId;
}

// Etiquette (publique ou cle) d'un lot du meme lot global pendant une verif : la verif continue, ce lot devient
// le lot en cours (currentPlace) et, s'il n'en faisait pas partie, ses items attendus s'ajoutent : ses items deja
// scannes y ont bien ete trouves. Renvoie true si le scan a ete traite ici.
async function joinVerif(scan) {
  if (!checkRunning()) return false;
  if (inCascade()) {
    // vérif en cascade : l'etiquette d'un lot du meme lot global fait entrer dans ce lot (lot cache : son parent)
    if (!lotById(scan.id)) return false;
    const lot = lotById(visibleId(scan.id));
    if (scan.key && !(await keyAccepted(scan.id, scan.key))) return true;
    if (scan.key) setKey(scan.id, scan.key);
    if (scan.code) addLabel(scan.id);
    // etiquette d'un lot hors de celui ou l'on est, alors que celui-ci est complet : il est valide automatiquement
    const left = lot.id === levelId() || isUnder(lot.id, levelId()) ? { names: [] } : await leaveCompleteLevels(lot.id, true);
    if (left.stay) { showInfo('lot', `📦 ${levelLot().name}`, 'Vous restez dans ce lot.'); save(); render(); return true; }
    const closed = left.names;
    const reopened = state.done[lot.id];
    enterLevel(lot.id);
    feedback.info();
    showInfo('lot', `📦 ${lot.name}`, reopened ? `Déjà validé ${fmtOn(reopened.at)} : vous le refaites.`
      : lot.id === state.lotId ? `Scannez l'étiquette ${childrenDe(lot.id)} pour y entrer.`
        : `Scannez son contenu, puis validez-le pour revenir à ${(lotById(parentIdOf(lot)) || state.lot).name}.`,
    closed.length ? `✔ ${closed.join(', ')} : validé.` : '',
    scan.key && keysUsed() ? `🔑 Étiquette clé de ${lot.name} scannée` : '');
    save();
    render();
    if (!state.scanned.length) switchTab('todo');
    flushSeals();
    return true;
  }
  const covered = sessionLots().find((lot) => lot.id === scan.id);
  if (covered && !scan.key) {
    if (scan.code) addLabel(covered.id);
    state.current = covered.id;
    state.askPlace = false;
    feedback.info();
    showInfo('ok', `${covered.name} fait partie de la vérif en cours`, 'Scannez ses items : ceux venus du stock y seront rangés.');
    save();
    render();
    return true;
  }
  // cache de vérif : lot d'un autre lot global, c'est une autre vérif (sans attendre le serveur)
  const cachedRoot = vcache.rootOf(scan.id);
  if (cachedRoot && state.lotId && vcache.rootOf(state.lotId) && cachedRoot !== vcache.rootOf(state.lotId)) return false;
  let lot;
  try {
    lot = await api(`lots/${encodeURIComponent(scan.id)}/` + (scan.key ? `?key=${encodeURIComponent(scan.key)}` : ''));
  } catch (e) {
    feedback.bad();
    showInfo('bad', 'Lot introuvable', e.message);
    return true;
  }
  if (scan.key && lot.key_check !== 'valid') {
    feedback.bad();
    showInfo('bad', 'Étiquette clé refusée', lot.key_error);
    return true;
  }
  if (rootOf(lot) !== rootOf(state.lot)) return false;
  const existing = state.extra.find((entry) => entry.id === scan.id);
  if (existing) { if (scan.key) existing.key = scan.key; existing.lot = lot; } else state.extra.push({ id: scan.id, key: scan.key || '', lot });
  if (scan.code) addLabel(scan.id);
  state.current = scan.id;
  state.askPlace = false;
  feedback.info();
  showInfo('ok', `${lot.name} ajouté à la vérif`,
    covered ? `${lot.name} faisait déjà partie de la vérif${keysUsed() ? ' : son étiquette clé est enregistrée' : ''}.`
      : `Les items attendus de ${lot.name} s'ajoutent à la vérif en cours.`,
    scan.key && keysUsed() ? `🔑 Étiquette clé de ${lot.name} scannée`
      : keyOk() ? '' : `Scannez l'étiquette clé de ${lot.name} (ou d'un autre lot de la vérif) pour valider.`);
  save();
  render();
  if (!state.scanned.length) switchTab('todo');
  return true;
}

const rootOf = (lot) => (lot.global ? lot.global.id : lot.path && lot.path.length ? lot.path[0].id : lot.id);

// QR code du scelle d'un lot : lot valide sans verif tant que le scelle est intact. Pendant une vérif du meme lot
// global, le lot scelle (et ses sous-lots) compte comme verifie sans etre ouvert (intactLots).
async function scanSeal(scan) {
  if (state.lot && state.lotId !== scan.id && checkRunning() && await sealDuringVerif(scan)) return;
  // vérif lancee sur le lot scelle lui-meme : son etiquette du scelle suffit, la vérif est validee aussitot
  if (state.lot && state.lotId === scan.id && sealWaiting(state.lot) && await sealDuringVerif(scan)) return;
  if (state.lotId !== scan.id) { state.lotKey = ''; state.lastVerif = null; }
  await loadLot(scan.id, scan.key);
  const lot = state.lot;
  if (!lot || state.lotId !== scan.id) return;
  const sealInfo = lot.is_sealed ? `Scellé${lot.seal_number ? ' n°' + lot.seal_number : ''} ${fmtOn(lot.sealed)}` +
    (lot.sealed_by ? ' par ' + lot.sealed_by : '') : '';
  if (lot.seal_check === 'valid' && !lot.expired_count) {
    feedback.good();
    showInfo('ok', `✔ Scellé intact – ${lot.name}`,
      'Lot valide sans vérif' + (lot.valid_until ? ` jusqu'au ${fmtDate(lot.valid_until)}` : ''), sealInfo);
  } else if (lot.seal_check === 'valid') {
    feedback.bad();
    showInfo('bad', `✘ Scellé intact mais ${onlyOutdated(lot) ? 'périmés' : 'items à remplacer'} – ${lot.name}`,
      onlyOutdated(lot) ? `${nb(lot.expired_count, 'item périmé', 'items périmés')} : lot à ouvrir`
        : `${nb(lot.expired_count, 'item')} à remplacer (${replaceReason(lot)}) : lot à ouvrir`, sealInfo);
  } else {
    feedback.bad();
    showInfo('bad', `✘ ${lot.seal_check === 'wrong' ? "Étiquette d'un ancien scellé" : 'Scellé brisé'} – ${lot.name}`,
      'Le lot doit être vérifié.',
      lot.unsealed ? `Scellé brisé ${fmtOn(lot.unsealed)}` + (lot.unsealed_by ? ' par ' + lot.unsealed_by : '') : '');
  }
  switchTab('lot');
}

// Etiquette du scelle d'un lot du meme lot global scannee pendant une vérif : scan « scellé intact ». Renvoie true si
// le scan a ete traite ici (false : lot d'un autre lot global, la vérif n'est pas concernee).
async function sealDuringVerif(scan) {
  if (allScanned().some((entry) => entry.kind === 'seal' && entry.iid === scan.id)
      || (inCascade() && state.done[scan.id] && state.done[scan.id].sealed)) {
    showInfo('ok', '✓ Déjà scanné', 'Scellé intact');
    return true;
  }
  let lot;
  try {
    lot = await api(`lots/${encodeURIComponent(scan.id)}/?seal=${encodeURIComponent(scan.key)}`);
  } catch (e) {
    // hors connexion : lot du cache de vérif, scelle accepte s'il est pose (le serveur controle son code a l'envoi)
    lot = e.status === 0 ? cachedSealLot(scan.id) : null;
    if (!lot) {
      feedback.bad();
      showInfo('bad', 'Lot introuvable', e.message);
      return true;
    }
  }
  if (rootOf(lot) !== rootOf(state.lot)) return false;
  if (lot.seal_check !== 'valid' || lot.expired_count) {
    feedback.bad();
    showInfo('bad', lot.seal_check !== 'valid'
      ? `✘ ${lot.seal_check === 'wrong' ? "Étiquette d'un ancien scellé" : 'Scellé brisé'} – ${lot.name}`
      : `✘ Scellé intact mais ${onlyOutdated(lot) ? 'périmés' : 'items à remplacer'} – ${lot.name}`, 'Ouvrez le lot et scannez son contenu.');
    return true;
  }
  // scelle d'un lot hors de celui ou l'on est, alors que celui-ci est complet : il est valide automatiquement
  const closed = inCascade() && lotById(lot.id) && !isUnder(lot.id, levelId()) ? (await leaveCompleteLevels(lot.id)).names : [];
  scannedAgain();
  state.scanned.push({ code: scan.code, kind: 'seal', iid: lot.id, key: scan.key, expired: false, error: '', items: [],
    place: inCascade() ? levelId() : '', info: { type_name: `🔒 Scellé intact – ${lot.name}` } });
  addLabel(lot.id); // l'etiquette du scelle montre aussi que la vérif est bien celle de ce lot
  feedback.good();
  showInfo('ok', `🔒 Scellé intact – ${lot.name}`, 'Compté comme vérifié, avec ses sous-lots, sans être ouvert.',
    closed.length ? `✔ ${closed.join(', ')} : validé.` : '');
  // lot scelle dont on fait la vérif (on y etait entre, ou la vérif a ete lancee dessus) : rien d'autre a y scanner
  const root = lot.id === state.lotId;
  const inside = inCascade() && !root && isUnder(levelId(), lot.id);
  const ends = inside && lot.id === state.startedIn;
  if (inside) { state.path = chainTo(parentIdOf(lotById(lot.id))); state.current = levelId(); }
  save();
  render();
  if (root) {
    // la vérif du lot, c'est ce scan : on la valide (connexion demandee si besoin)
    if (inCascade()) await validateStep(false); else await validate(false);
    return true;
  }
  await flushSeals();
  if (ends && state.done[lot.id]) finishCascade(null, `${lot.name} : scellé intact constaté, vérif terminée.`);
  return true;
}

// Lot du cache de vérif pour une etiquette de scelle lue hors connexion : 'valid' s'il est scelle (code controle par
// le serveur a l'envoi), avec son lot global (rootOf). Null s'il n'est pas dans le cache.
function cachedSealLot(id) {
  const rootId = vcache.rootOf(id);
  if (!rootId) return null;
  const root = vcache.lot(rootId);
  const row = rootId === id ? root : (root.descendants || []).find((sub) => sub.id === id);
  if (!row) return null;
  return Object.assign(row, { global: root.global || { id: rootId, name: root.name }, path: row.path || [{ id: rootId, name: root.name }],
                             seal_check: row.is_sealed ? 'valid' : 'unsealed', seal_offline: true });
}

// Etiquette d'ouverture, rangee dans le lot scelle : la scanner ouvre le scelle, connecte ou non (pas de
// bouton « ouvrir »). Sans identite, l'ouverture est enregistree anonymement : on ne demande pas de la signer.

// fromUrl : etiquette ouverte avec l'appareil photo (adresse) : l'ecran fermé, on arrive sur l'accueil
async function openSeal(scan, { fromUrl = false } = {}) {
  let result;
  try {
    result = await api(`lots/${encodeURIComponent(scan.id)}/seal-open/`,
      { code: scan.key, ...(hasIdentity() ? identity() : {}) });
  } catch (e) {
    feedback.bad();
    showInfo('bad', "Étiquette d'ouverture refusée", e.message);
    return;
  }
  const lot = result.lot;
  state.declaredAllowed = !!result.declared_identity;
  const opened = lot.unsealed ? `Scellé${lot.seal_number ? ' n°' + lot.seal_number : ''} ouvert ${fmtOn(lot.unsealed)}`
    + (lot.unsealed_by ? ' par ' + lot.unsealed_by : '') : '';
  if (result.seal && result.seal.valid) feedback.good(); else feedback.warn();
  showInfo(result.result === 'opened' && result.seal && result.seal.valid ? 'ok' : 'warn',
    `🔓 ${result.result === 'opened' ? 'Scellé ouvert' : 'Scellé déjà ouvert'} – ${lot.name}`, opened);
  save();
  // lot de la vérif en cours : rechargé, son contenu est maintenant a verifier (plus scelle)
  if (state.lot && lotById(lot.id)) loadLot(state.lotId, '', true);
  // ecran en grand, pas de vérif : l'ouverture est enregistree, les responsables le voient
  showOpening(result, fromUrl);
}

// Ecran d'ouverture d'un scelle : rassurer (le scelle etait valide, jusqu'a quand) et dire que l'ouverture est
// connue du TECH. Rien a signer ni a signaler. Contenu du scelle, aucun bouton de vérif.
let sealScreen = { lot: null, fromUrl: false };
const openingShown = () => !$('#opening').hidden;

function showOpening(result, fromUrl = false) {
  const lot = result.lot;
  const seal = result.seal || {};
  sealScreen = { lot, fromUrl };
  const sealedLine = seal.sealed ? `Scellé${seal.seal_number ? ' n°' + seal.seal_number : ''} posé ${fmtOn(seal.sealed, { long: true })}`
    + (seal.sealed_by ? ' par ' + seal.sealed_by : '') : '';
  const openedLine = lot.unsealed ? `Ouvert ${fmtOn(lot.unsealed, { long: true })}`
    + (lot.unsealed_by && result.identified ? ' par ' + lot.unsealed_by : '') : '';
  const parts = [
    el('div', { class: 'opening-head' },
      el('div', { class: 'icon' }, '🔓'),
      el('h2', {}, result.result === 'already' ? 'Scellé déjà ouvert' : 'Ouverture du scellé enregistrée'),
      el('div', { class: 'sub' }, lot.name + (lot.lot_type_name ? ` · ${lot.lot_type_name}` : ''))),
    seal.valid
      ? el('div', { class: 'opening-box ok' },
        el('b', {}, '✔ Le scellé était valide'),
        el('p', {}, seal.valid_until ? 'Contenu validé jusqu\'au' : 'Contenu validé, sans date de péremption'),
        seal.valid_until ? el('div', { class: 'valid-until' }, fmtDate(seal.valid_until)) : '',
        sealedLine ? el('p', { class: 'sub' }, sealedLine) : '')
      : el('div', { class: 'opening-box warn' },
        el('b', {}, '⚠ Une partie du contenu n\'était plus valide'),
        el('p', {}, 'Vérifiez les dates avant d\'utiliser un item (en rouge ci-dessous).'),
        sealedLine ? el('p', { class: 'sub' }, sealedLine) : ''),
    el('div', { class: 'opening-box tech' },
      el('b', {}, '🛠 Le TECH est au courant'),
      el('p', {}, 'L\'ouverture est enregistrée et le TECH en est informé. '
        + (lot.seal_required ? 'Le lot sera vérifié puis scellé à nouveau plus tard.' : 'Le lot sera vérifié plus tard.')),
      openedLine ? el('p', { class: 'sub' }, openedLine) : ''),
    el('h3', {}, 'Contenu du scellé'),
  ];
  // items du lot puis de chacun de ses sous-lots (ils etaient sous le meme scelle), regroupes par type
  const lots = [lot, ...(lot.descendants || [])];
  let any = false;
  for (const sub of lots) {
    const groups = new Map();
    for (const item of sub.items || []) {
      const group = groups.get(item.type_name) || { name: item.type_name, count: 0, first: null, expired: 0 };
      group.count += 1;
      if (item.expired) group.expired += 1;
      if (item.peremption && (!group.first || item.peremption < group.first)) group.first = item.peremption;
      groups.set(item.type_name, group);
    }
    if (!groups.size) continue;
    any = true;
    if (lots.length > 1) {
      parts.push(el('div', { class: 'opening-lot', style: `padding-left:${Math.max(0, (sub.depth || 0) - 1) * 12}px` },
        '📦 ' + sub.name));
    }
    const rows = [...groups.values()].sort((a, b) => a.name.localeCompare(b.name, 'fr'));
    parts.push(el('ul', { class: 'opening-items' }, ...rows.map((group) => el('li', { class: group.expired ? 'expired' : '' },
      el('span', { class: 'count' }, `${group.count} ×`),
      el('span', {}, group.name),
      el('span', { class: 'date' }, group.expired ? `${group.expired} à remplacer`
        : group.first ? 'jusqu\'au ' + fmtDate(group.first) : '')))));
  }
  if (!any) parts.push(el('p', { class: 'sub' }, 'Aucun item enregistré dans ce lot.'));
  $('#opening-body').replaceChildren(...parts);
  $('#opening-body').scrollTop = 0;
  $('#opening').hidden = false;
  syncCamera();
}

function closeOpening() {
  if (!openingShown()) return;
  $('#opening').hidden = true;
  const home = sealScreen.fromUrl && !verifInProgress() && !ficheOpen();
  sealScreen = { lot: null, fromUrl: false };
  syncCamera();
  if (home) showHome('menu');
  else render();
}

// Un sous-lot d'un lot global se verifie dans la vérif en cascade du lot global : c'est lui qui est charge, et l'on
// entre dans le sous-lot (state.path). seal : etiquette du scelle, le lot lui-meme est affiche.
// fresh : juste apres une ecriture (etape validee, items ajoutes...), le serveur d'abord, le cache seulement hors
// connexion ; sinon le cache de vérif d'abord, sans attendre le serveur
async function loadLot(id, seal, fresh = false) {
  try {
    // L'etiquette du scelle, elle, est toujours controlee par le serveur
    const cachedRoot = seal ? '' : vcache.rootOf(id);
    let lot = null;
    let root = '';
    if (!cachedRoot || fresh) {
      try {
        lot = await api(`lots/${encodeURIComponent(id)}/` + (seal ? `?seal=${encodeURIComponent(seal)}` : ''));
        root = !seal && lot.global && lot.global.id !== id ? lot.global.id : '';
        if (root) lot = await api(`lots/${encodeURIComponent(root)}/`);
        if (fresh) vcache.refresh(0);
      } catch (e) {
        // hors connexion : le cache (etiquette du scelle : controlee par le serveur a l'envoi)
        lot = e.status === 0 && seal ? cachedSealLot(id) : null;
        if (e.status !== 0 || (!cachedRoot && !lot)) throw e;
      }
    }
    if (!lot) {
      // cache de vérif : le lot global est deja la (mis a jour en arriere-plan, voir cacheChanged)
      lot = vcache.lot(cachedRoot);
      root = cachedRoot !== id ? cachedRoot : '';
      vcache.refresh(15000);
    }
    const target = root || id;
    if (state.lotId !== target) {
      state.extra = []; resetCascade(); state.startedIn = root ? id : '';
      state.startedAt = new Date().toISOString(); // debut de la vérif (duree : rapport d'etat, statistiques)
    }
    state.lot = lot;
    state.lotId = target;
    // chemin vers le sous-lot demande, ou chemin garde (rechargement) tant qu'il existe encore
    const last = root ? id : state.path[state.path.length - 1];
    state.path = last && lotById(last) && last !== target ? chainTo(last) : [];
    for (const lotId of Object.keys(state.done)) if (!lotById(lotId)) delete state.done[lotId];
  } catch (e) {
    feedback.bad();
    showInfo('bad', 'Lot introuvable', e.message);
    if (state.lotId === id) { state.lot = null; state.lotId = ''; state.lotKey = ''; state.extra = []; resetCascade(); }
  }
  await loadExtra();
  save();
  render();
}

// Detail des autres lots de la verif groupee (rechargement de la page, apres une verif)
async function loadExtra() {
  const kept = [];
  for (const entry of state.extra) {
    try {
      entry.lot = await api(`lots/${encodeURIComponent(entry.id)}/`);
      if (state.lot && rootOf(entry.lot) === rootOf(state.lot)) kept.push(entry);
    } catch (e) { /* lot supprime ou archive : retire de la verif */ }
  }
  state.extra = kept;
}

// Lots couverts par la verif en cours : le lot scanne, ses sous-lots, puis les lots ajoutes et leurs sous-lots
function sessionLots() {
  const result = [];
  const seen = new Set();
  const add = (lot, depth) => {
    if (!lot || seen.has(lot.id)) return;
    seen.add(lot.id);
    result.push(Object.assign(lot, { depth }));
  };
  for (const top of [state.lot, ...state.extra.map((entry) => entry.lot)]) {
    if (!top) continue;
    add(top, 0);
    for (const sub of top.descendants || []) add(sub, sub.depth);
  }
  return result;
}

const isMulti = () => sessionLots().length > 1;
