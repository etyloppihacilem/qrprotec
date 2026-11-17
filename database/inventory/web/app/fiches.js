/* QRProtec - front web : fiches de lot et d'item, ecran d'ouverture d'un scelle. */
'use strict';

// Fiches lot et item : page d'arrivee d'une etiquette ouverte avec l'appareil photo du telephone (sans le lecteur
// integre), ou d'un lot / item touche dans une fiche. Pas de camera : « Lancer une vérif » ouvre le lecteur sur le
// bon lot (le lot de la fiche, ou le dernier emplacement connu de l'item).

const fiche = { kind: '', id: '', key: '', seal: '', label: false, lot: null, item: null, error: '', keyCheck: '', keyError: '', token: 0, folds: new Set(), contentOpen: null, offline: false };

function ficheOpen() { return !$('#fiche').hidden; }

// Premiere lettre en majuscule (texte saisi au poste, ex : emplacement de l'etiquette privee)
const cap = (text) => (text ? text.charAt(0).toUpperCase() + text.slice(1) : '');

const ficheUrl = (kind, id, seal = '') => (kind !== 'lot' ? `item?id=${encodeURIComponent(id)}`
  : seal ? `seal?lot=${encodeURIComponent(id)}&s=${encodeURIComponent(seal)}` : `verif?lot=${encodeURIComponent(id)}`);

// Lien vers la fiche d'un lot ou d'un item (vraie URL : appui long pour l'ouvrir dans un autre onglet)
function ficheLink(kind, id, label) {
  return el('a', { class: 'link', href: ficheUrl(kind, id), onclick: (event) => {
    event.preventDefault();
    event.stopPropagation();
    openFiche(kind, id);
  } }, label);
}
const lotLink = (lot) => ficheLink('lot', lot.id, lot.name);

// Cle de l'etiquette privee ouverte : retiree de l'adresse, gardee pour cet onglet (rechargement, retour)
function storedFicheKey(id) {
  try {
    const saved = JSON.parse(sessionStorage.getItem(STORAGE_FICHE_KEY) || 'null');
    return saved && saved.id === id && Date.now() - saved.at < SESSION_TIMEOUT_MS ? saved.key : '';
  } catch (e) { return ''; }
}

function storeFicheKey(id, key) {
  try { sessionStorage.setItem(STORAGE_FICHE_KEY, JSON.stringify({ id, key, at: Date.now() })); } catch (e) { /* ignore */ }
}

// push : nouvelle entree d'historique (le bouton retour du telephone revient a la page precedente)
// seal : code de l'etiquette du scelle scannee (fiche du lot scelle, sans vérif tant que le scelle est intact)
// label : fiche ouverte par l'etiquette du lot (« Lancer une vérif » la compte comme scannee)
async function openFiche(kind, id, { key = '', seal = '', push = true, label = false } = {}) {
  if (kind === 'lot' && !seal) {
    if (key) storeFicheKey(id, key); else key = storedFicheKey(id);
  }
  const token = fiche.token + 1;
  Object.assign(fiche, { kind, id, key, seal, label, lot: null, item: null, error: '', keyCheck: '', keyError: '', token, folds: new Set(), contentOpen: null, offline: false });
  if (push) history.pushState({ inApp: true }, '', ficheUrl(kind, id, seal));
  $('#fiche').hidden = false;
  document.body.classList.add('fiche-open');
  document.body.classList.remove('home-mode', 'login-reader');
  state.homePage = 'scan';
  watchStop();
  syncCamera();
  renderFiche();
  $('#fiche-body').scrollTop = 0;
  try {
    if (kind === 'lot') {
      const query = seal ? `?seal=${encodeURIComponent(seal)}` : key ? `?key=${encodeURIComponent(key)}` : '';
      const lot = await api(`lots/${encodeURIComponent(id)}/` + query);
      if (fiche.token !== token) return;
      fiche.lot = lot;
      if (key) { fiche.keyCheck = lot.key_check; fiche.keyError = lot.key_error || ''; }
      if (seal && push) {
        if (lot.seal_check === 'valid' && !lot.expired_count && !lot.unusable) feedback.good();
        else if (lot.seal_check === 'wrong') feedback.warn(); else feedback.bad();
      }
    } else {
      const item = await api(`items/${encodeURIComponent(id)}/`);
      if (fiche.token !== token) return;
      fiche.item = item;
      // objet perdu scanne par la personne qui l'a trouve : pas de popup des signalements
      if (item.reported && (!item.lost_object || hasIdentity())) alertReports('item:' + item.iid, `Item signalé : ${item.type_name}`, item.reports);
    }
  } catch (e) {
    if (fiche.token !== token) return;
    const copy = e.status === 0 && !seal ? await offlineFiche(kind, id, key) : null;
    if (fiche.token !== token) return;
    if (copy) Object.assign(fiche, copy);
    else fiche.error = e.status === 404 ? (kind === 'lot' ? 'Lot introuvable' : 'Item inconnu dans la base') : e.message;
  }
  renderFiche();
}

// Hors connexion (etiquette scannee dans la reserve, appli ouverte sans reseau) : fiche tiree du cache de vérif,
// pour pouvoir lancer la vérif quand meme. L'etiquette cle est acceptee, le serveur la controle a l'envoi de la
// vérif (keyAccepted). Le scelle, lui, reste controle par le serveur : pas de fiche hors connexion.
async function offlineFiche(kind, id, key) {
  await vcache.ready();
  if (kind === 'item') {
    const item = vcache.item(id);
    return item ? { item, offline: true } : null;
  }
  const root = vcache.rootOf(id);
  const tree = root ? vcache.lot(root) : null;
  const lot = !tree ? null : root === id ? tree : (tree.descendants || []).find((row) => row.id === id);
  return lot ? { lot, offline: true, keyCheck: key ? 'valid' : '' } : null;
}

function closeFiche() {
  if (!ficheOpen()) return;
  fiche.token += 1;
  scannedFiche = null;
  $('#fiche').hidden = true;
  document.body.classList.remove('fiche-open', 'login-reader');
  syncCamera();
  render();
}

// Une vérif est en cours sur ce telephone (items scannes, ou etiquette privee scannee et pas encore validee)
const verifInProgress = () => !!state.lot && (state.scanned.length > 0 || (!!state.lotKey && !state.lastVerif));

// Ouvre le lecteur sur un lot. key : etiquette privee deja scannee (verifiee par le serveur). label : la fiche a ete
// ouverte par l'etiquette du lot (scanner, ou appareil photo du telephone), elle compte pour valider (labelMissing).
async function launchVerif(id, key, label = false) {
  startCamera(); // pendant le clic : le navigateur peut exiger un geste de l'utilisateur
  vcache.refresh(55000); // tout pour la vérif, charge en arriere-plan (vérif possible hors connexion)
  closeFiche();
  history.pushState({ inApp: true }, '', 'verif');
  // etiquette privee d'un autre lot du meme lot global pendant une vérif : elle rejoint la vérif en cours (vérif en
  // cascade : on entre dans ce lot)
  if (state.lot && state.lotId !== id && (key || inCascade())
    && await joinVerif({ kind: 'lot', code: label ? 'fiche' : '', id, key })) return;
  const same = !!state.lot && !!lotById(id);
  if (!same) {
    if (state.scanned.length && !await confirmDialog('Changer de lot ?', 'Les items déjà scannés restent dans la liste.', 'Changer de lot')) return;
    state.lotKey = '';
    state.lastVerif = null;
    state.extra = [];
    resetCascade();
  }
  await loadLot(id);
  if (!state.lot) return;
  if (key) setKey(id, key);
  if (label) addLabel(id);
  if (state.launched !== state.lotId) resetVerifHelp();
  state.launched = state.lotId;
  if (inCascade()) enterLevel(id);
  feedback.info();
  showLotInfo();
  switchTab('todo');
  // nouvelle demande : une action qui attendait encore la connexion (signalement, lot utilise...) est oubliee
  loginAction = null;
  // pas encore connecte : grande fenetre « Scannez votre carte »
  if (!hasIdentity()) openGate('verif');
}

async function launchFromFiche() {
  if (fiche.kind === 'item') {
    const where = fiche.item && fiche.item.last_location;
    const scanned = scannedFiche && scannedFiche.id === fiche.id ? scannedFiche : null;
    if (!where) return;
    await launchVerif(where.id, '');
    // fiche ouverte par un scan dans le scanner : l'item scanne compte dans la vérif, sans le rescanner
    if (scanned && state.lot && lotById(where.id)) await scanItem(scanned);
  } else if (fiche.lot) {
    launchVerif(fiche.id, fiche.keyCheck === 'valid' ? fiche.key : '', fiche.label);
  }
}

// Item ouvert hors du lecteur pendant une vérif : on peut l'ajouter aux scans de la vérif en cours
function addFicheItem() {
  const scan = parseCode(fiche.id);
  if (scan.kind !== 'item') return;
  startCamera();
  closeFiche();
  history.pushState({ inApp: true }, '', 'verif');
  switchTab('todo');
  scanItem(scan);
}

$('#opening-close').addEventListener('click', closeOpening);
$('#fiche-launch').addEventListener('click', launchFromFiche);
$('#fiche-add').addEventListener('click', addFicheItem);
$('#fiche-back').addEventListener('click', () => {
  if (history.state && history.state.inApp) history.back();
  else if (state.lot) { closeFiche(); enterScanner(true); switchTab('todo'); }
  else { closeFiche(); showHome('menu'); }
});
$('#fiche-home').addEventListener('click', () => {
  closeFiche();
  showHome('menu');
});

function renderFiche() {
  const body = $('#fiche-body');
  const launch = $('#fiche-launch');
  const add = $('#fiche-add');
  add.hidden = true;
  launch.hidden = false;
  $('#fiche-actions').hidden = false;
  launch.disabled = false;
  // Retour / Accueil : caches sur la fiche d'un objet perdu pour qui n'est pas connecte (la personne qui l'a
  // trouve), et pendant le chargement d'un item, tant qu'on ne sait pas si c'en est un
  const noNav = !hasIdentity() && !fiche.error && fiche.kind === 'item' && (!fiche.item || fiche.item.lost_object);
  $('#fiche-back').hidden = noNav;
  $('#fiche-home').hidden = noNav;
  if (fiche.error) {
    body.replaceChildren(el('div', { class: 'banner bad' }, fiche.error), showIds() ? el('p', { class: 'sub' }, fiche.id) : '');
    launch.disabled = true;
    return;
  }
  if (fiche.kind === 'lot' && fiche.lot) renderLotFiche(body, fiche.lot);
  else if (fiche.kind === 'item' && fiche.item) renderItemFiche(body, fiche.item);
  else {
    body.replaceChildren(el('p', { class: 'sub' }, 'Chargement…'));
    launch.disabled = true;
  }
  if (fiche.offline && (fiche.lot || fiche.item)) {
    const when = fmtOn(new Date(vcache.syncedAt()).toISOString());
    body.prepend(el('div', { class: 'banner warn' }, el('b', {}, 'Hors connexion'),
      el('div', {}, `Fiche de la dernière mise à jour, ${when}. La vérif peut se faire : elle sera envoyée au retour du réseau.`)));
  }
}

// Resultat de l'etiquette du scelle scannee, en tete de la fiche du lot
function sealBanner(lot) {
  const sealed = lot.is_sealed ? `Scellé${lot.seal_number ? ' n°' + lot.seal_number : ''} ${fmtOn(lot.sealed, { long: true })}`
    + (lot.sealed_by ? ' par ' + lot.sealed_by : '') : '';
  if (lot.seal_check === 'valid' && lot.unusable) {
    return el('div', { class: 'banner bad' }, el('b', {}, '✘ Scellé intact mais lot signalé non utilisable'),
      el('div', {}, 'Voir les signalements ci-dessous.'), sealed ? el('div', { style: 'font-weight:400' }, sealed) : '');
  }
  if (lot.seal_check === 'valid' && !lot.expired_count) {
    return el('div', { class: 'banner ok' }, el('b', {}, '🔒 Scellé intact'),
      el('div', {}, 'Lot valide sans vérif' + (lot.valid_until ? ` jusqu'au ${fmtDate(lot.valid_until)}` : '')),
      sealed ? el('div', { style: 'font-weight:400' }, sealed) : '');
  }
  if (lot.seal_check === 'valid') {
    return el('div', { class: 'banner bad' }, el('b', {}, '✘ Scellé intact mais périmés'),
      el('div', {}, `${nb(lot.expired_count, 'item périmé', 'items périmés')} : lot à ouvrir`), sealed ? el('div', { style: 'font-weight:400' }, sealed) : '');
  }
  if (lot.seal_check === 'wrong') {
    return el('div', { class: 'banner warn' }, el('b', {}, "⚠ Étiquette d'un ancien scellé"),
      el('div', {}, 'Un scellé plus récent a été posé depuis : cette étiquette n\'est plus valable.'));
  }
  return el('div', { class: 'banner bad' }, el('b', {}, '✘ Scellé brisé'),
    el('div', {}, 'Le lot doit être vérifié.'),
    lot.unsealed ? el('div', { style: 'font-weight:400' }, `Scellé brisé ${fmtOn(lot.unsealed, { long: true })}`
      + (lot.unsealed_by ? ' par ' + lot.unsealed_by : '')) : '');
}

// Etat d'un lot en une ligne pour les secouristes et chefs : la couleur, un mot (« Vérifié ») et, en non gras, la
// date de la derniere vérif ; ni comptes ni raisons (gestion et admin gardent l'etat detaille)
function shortBanner(lot, status) {
  const { word, at } = shortStatus(lot, status);
  return el('div', { class: 'banner ' + status.kind }, word,
    at ? el('span', { class: 'when' }, ` (${fmtDateTime(at, { long: true })})`) : '');
}

function renderLotFiche(body, lot) {
  const status = groupStatus(lot) || lotStatus(lot);
  const full = detailed();
  const current = state.lotId === lot.id && verifInProgress();
  const launch = $('#fiche-launch');
  setLabel(launch, 'verif', current ? 'Reprendre la vérif' : 'Lancer une vérif');
  // etiquette du scelle scannee : le lot reste ferme, pas de vérif a lancer tant qu'il est scelle (etiquette d'un
  // ancien scelle comprise : un scelle plus recent a ete pose depuis)
  const sealIntact = !!fiche.seal && lot.seal_check === 'valid';
  const noVerif = !!fiche.seal && lot.is_sealed;
  if (noVerif && !current) launch.hidden = true;
  $('#fiche-actions').hidden = launch.hidden && $('#fiche-add').hidden;
  const parts = [
    el('div', { class: 'title-row' },
      el('div', {}, el('h2', {}, lot.name), el('div', { class: 'sub' }, withId(lotKind(lot), lot.id))),
      el('div', { class: 'pdf-links' },
        el('a', { class: 'pdf-link', href: new URL(`lots/${encodeURIComponent(lot.id)}/sheet.pdf`, API).href,
          target: '_blank', rel: 'noopener', title: "Fiche d'inventaire papier (PDF)" }, '📄 Fiche PDF'),
        // admins : rapport d'etat du lot a une date donnee (PDF a certifier et signer)
        isAdmin() ? el('a', { class: 'pdf-link', href: '#', title: "État du lot à une date donnée (PDF A4 à certifier)",
          onclick: (event) => { event.preventDefault(); openStateReport(lot); } }, '📋 Rapport daté') : '')),
    (lot.path || []).length ? el('p', { class: 'sub' }, 'Dans : ',
      ...lot.path.filter((parent) => !parent.hidden) // lot cache dans la hierarchie (B+ du VPS) : saute
        .flatMap((parent, index) => [index ? ' › ' : '', lotLink(parent)])) : '',
    fiche.seal ? sealBanner(lot) : '',
    sealIntact ? '' : full ? el('div', { class: 'banner ' + status.kind }, status.label) : shortBanner(lot, status),
    // lot d'un referentiel (B+) : lignes de la liste dans aucun de ses sacs, a repartir au poste
    (lot.unplaced || []).length ? el('p', { class: 'warn-text' }, `Dans aucun sac : ${lot.unplaced.map((row) => row.type_name).join(', ')}`
      + (canSeeStock() ? ' (à répartir au poste : Gestion des lots, onglet Répartition).' : '.')) : '',
    lotReportsBlock(lot),
    reportButton({ lot: lot.id, lotName: lot.name, label: lot.name }),
    departButton(lot),
    lot.is_sealed || lot.storage || !lot.active ? '' : usedButton(lot),
    current ? el('p', {}, `▶ Vérif en cours sur ce téléphone : ${nb(state.scanned.length, 'scan')}.`) : '',
    lot.is_sealed && !sealIntact && full ? el('p', {}, `🔒 Scellé${lot.seal_number ? ' n°' + lot.seal_number : ''} ${fmtOn(lot.sealed, { long: true })}`
      + (lot.sealed_by ? ` par ${lot.sealed_by}` : '') + ' : pas de vérif nécessaire tant que le scellé est intact.') : '',
    !full ? '' : el('p', {}, 'Dernière vérif : ' + (lot.last_verif
      ? fmtDateTime(lot.last_verif, { long: true }) + (lot.last_verif_by ? ' par ' + lot.last_verif_by : '') : 'jamais')),
    // credit de confiance du reassort (reglage du serveur decoche) : reassorts sans vérif encore possibles
    !full || !lot.restock_credit || lot.verif_recommended ? '' : el('p', { class: 'hint' },
      `Réassort sans vérif complète : encore ${nb(lot.restock_credit.outings_left, 'sortie')}, `
      + `jusqu'au ${fmtDate(lot.restock_credit.until)}.`),
    expiryLine(lot),
    full ? revisionLine(lot) : '',
    !lot.verif_recommended || !full ? '' : el('p', {}, lot.restocked_count
      ? `⚠ Réassort de ${nb(lot.restocked_count, 'item')} ${fmtOn(lot.restocked, { long: true })}${lot.restocked_by ? ' par ' + lot.restocked_by : ''} : vérif complète recommandée.`
      : lot.back_from_list ? `⚠ Sorti avec la liste « ${lot.back_from_list} » ${fmtOn(lot.restocked, { long: true })}${lot.restocked_by ? ' par ' + lot.restocked_by : ''} : vérif complète recommandée.`
      : lot.marked_used ? `⚠ Marqué utilisé ${fmtOn(lot.marked_used, { long: true })}${lot.marked_used_by ? ' par ' + lot.marked_used_by : ''} : vérif complète recommandée.`
      : `⚠ Scellé ouvert${lot.unsealed ? ' ' + fmtOn(lot.unsealed, { long: true }) : ''}${lot.unsealed_by ? ' par ' + lot.unsealed_by : ''} : vérif complète recommandée.`),
    lot.seal_required && !lot.is_sealed && full
      ? el('p', {}, '🔒 Scellé obligatoire pour ce type de lot : il reste à traiter tant qu\'un nouveau scellé n\'est pas posé.') : '',
    lot.global && lot.global.id !== lot.id && full
      ? el('p', {}, 'Lot global : ', ficheLink('lot', lot.global.id, lot.global.name), ` · ${lot.global.label}`) : '',
    // lots exiges : place fixe vide (rouge), lot qui en remplace un autre
    (lot.slots_missing || []).length
      ? el('p', { class: 'danger-text' }, `✘ Lot manquant : ${lot.slots_missing.join(', ')}`) : '',
    ...(lot.slots || []).flatMap((slot) => slot.lots.filter((child) => child.replaces_name).map((child) =>
      el('p', {}, '↔ ', ficheLink('lot', child.id, child.name), ` remplace ${child.replaces_name}`))),
  ];

  // etiquette privee : scannee (cle verifiee par le serveur) ou ou la trouver
  const keyBox = [];
  if (fiche.keyCheck === 'valid') keyBox.push(el('b', {}, '🔑 Étiquette clé scannée : la vérif pourra être validée.'));
  else if (fiche.keyCheck) keyBox.push(el('b', {}, '✘ Étiquette clé refusée'), el('div', {}, fiche.keyError));
  keyBox.push(el('div', {}, lot.key_location ? `Emplacement : ${cap(lot.key_location)}` : 'Emplacement non renseigné'));
  if (fiche.keyCheck !== 'valid') {
    keyBox.push(el('div', { class: 'sub' }, "QR code rangé à l'intérieur du lot, à scanner pendant la vérif pour pouvoir la valider."));
  }
  // lot scelle intact : on ne le verifie pas, l'etiquette cle n'a pas a etre cherchee. Badge obligatoire (nom declare
  // desactive) : elle n'autorise rien de plus que l'etiquette publique, on n'en parle pas
  if (!noVerif && state.declaredAllowed) parts.push(el('h3', {}, 'Étiquette clé'),
    el('div', { class: 'key-box' + (fiche.keyCheck && fiche.keyCheck !== 'valid' ? ' bad' : '') }, ...keyBox));

  // contenu : le lot puis chacun de ses sous-lots (attendus, puis items enregistres), ce qui est a traiter en haut
  // secouristes et chefs : contenu dans une liste deroulante, repliee par defaut quand tout est valide
  const content = [];
  // contenu deplie d'office si un item manque ou est en rouge (perime, inutilisable, disparu), dans le lot ou un
  // sous-lot ; pas pour une simple vérif recommandee
  let shortage = false;
  const lots = [lot, ...attentionTree(lot.descendants || [])];
  const tags = { ok: 'green', warn: 'orange', bad: 'red' };
  for (const sub of lots) {
    const requirements = sub.requirements || [];
    // emplacement d'un item dans le lot : celui prevu pour son type (ex. « pochette bleue »)
    const places = new Map(requirements.filter((row) => row.location).map((row) => [row.type, row.location]));
    const place = (item) => places.get(item.type) || '';
    const items = [...(sub.items || [])].sort((a, b) => place(a).localeCompare(place(b), 'fr')
      || a.type_name.localeCompare(b.type_name, 'fr')
      || String(a.peremption || '9').localeCompare(String(b.peremption || '9')));
    const missing = sub.missing_items || [];
    if (sub !== lot) {
      if (!requirements.length && !items.length && !missing.length && !(sub.descendants || []).length) continue;
      const subStatus = lotStatus(sub);
      content.push(el('ul', { class: 'list' }, el('li', { class: 'lot-head ' + subStatus.kind,
        style: `padding-left:${12 + Math.max(0, (sub.depth || 1) - 1) * 14}px`, onclick: () => openFiche('lot', sub.id) },
      el('div', { class: 'main' }, el('div', { class: 'name' }, '📦 ', ficheLink('lot', sub.id, sub.name)),
        el('div', { class: 'sub' }, full ? `${lotKind(sub)} · ${nb(sub.item_count, 'item')}` : lotKind(sub))),
      el('span', { class: 'tag ' + tags[subStatus.kind] }, subStatus.label.replace(/^[✔✘⚠] /, '').split(',')[0]))));
    }
    // une jauge par type attendu ; la fleche au bout deplie ses iids avec leur peremption (a remplacer en haut)
    const pairs = [...items.map((item) => [item, '']), ...missing.map((item) => [item, 'disparu'])];
    // un item d'un sous-type va sous son type a unites (200 mL de serum phy) quand son type n'est pas attendu
    const ofType = (type) => pairs.filter(([item]) => displayRow(requirements, item.type) === type)
      .sort((a, b) => iidAttention(a) - iidAttention(b));
    const flagged = (rows) => rows.some((pair) => iidAttention(pair) < 3);
    if (pairs.some((pair) => iidAttention(pair) < 2) || requirements.some((row) => row.present < row.required)) shortage = true;
    // types incomplets en haut, puis ceux qui ont un item a remplacer ou signale, puis 0/0, puis les complets
    // avec des emplacements (pochettes) : un titre par emplacement, dans l'ordre choisi par la TECH, le meme tri dedans
    const placed = requirements.some((row) => row.location);
    const lines = [];
    for (const row of requirements) {
      const rows = ofType(row.type);
      const none = row.present <= 0 && row.required <= 0 && !row.optional;
      lines.push({ rank: row.present < row.required ? 0 : flagged(rows) ? 1 : none ? 2 : row.optional && row.present < row.expected ? 2.5 : 3,
        row, rows: ficheFold(`${sub.id}:${row.type}`, rows,
          (arrow) => requirementLine(reqName(row) + (!placed && row.location ? ` (${row.location})` : ''), row.present, row, arrow)) });
    }
    // items hors definition (ou d'un rangement) : une ligne par type, sans jauge
    const known = new Set(requirements.map((row) => row.type));
    const extra = [...new Set(pairs.filter(([item]) => !known.has(displayRow(requirements, item.type))).map(([item]) => item.type))];
    const extraLines = [];
    for (const type of extra) {
      const rows = ofType(type);
      const label = rows[0][0].type_name + (requirements.length ? ' (hors définition)' : '');
      extraLines.push({ rank: flagged(rows) ? 1 : 3, rows: ficheFold(`${sub.id}:${type}`, rows, (arrow) => el('div', { class: 'req' },
        el('span', { class: 'label' }, label), el('span', { class: 'req-count' }, String(rows.filter(([, tag]) => !tag).length)), arrow)) });
    }
    const byRank = (list) => list.sort((a, b) => a.rank - b.rank);
    if (placed) {
      // emplacements a traiter en haut (meme rang que leurs types), l'ordre de la TECH a egalite
      const placeGroups = byPlace(lines, (line) => line.row.location)
        .map((entry) => ({ ...entry, rank: Math.min(...entry.rows.map((line) => line.rank)) }))
        .sort((a, b) => a.rank - b.rank);
      for (const { place, rows: list } of placeGroups) {
        // type a unites (200 mL) : compte pour une ligne, faite ou non ; type parent sans unite : en items
        const units = (row) => isGroupRow(row) && row.unit;
        const need = (row) => (units(row) ? 1 : row.optional ? row.expected || 0 : row.required);
        const done = (row) => (units(row) ? Number(row.present >= row.required) : row.present);
        const present = list.reduce((sum, line) => sum + Math.min(done(line.row), need(line.row)), 0);
        const required = list.reduce((sum, line) => sum + need(line.row), 0);
        const lacking = list.some((line) => line.row.present < line.row.required);
        content.push(el('div', { class: 'place-title' + (lacking ? ' incomplete' : '') },
          el('span', {}, '📍 ' + (place || 'Sans emplacement')), el('span', { class: 'count' }, `${present}/${required}`)));
        for (const line of byRank(list)) content.push(...line.rows);
      }
      for (const line of byRank(extraLines)) content.push(...line.rows);
    } else {
      for (const line of byRank([...lines, ...extraLines])) content.push(...line.rows);
    }
    if (!pairs.length && !requirements.length && sub === lot && lots.length === 1) content.push(el('p', { class: 'sub' }, 'Aucun item enregistré.'));
  }
  // contenu replie par defaut pour tout le monde, titre discret avec un petit carre [+] : la liste ne doit pas
  // servir a une verif « a la main » ; la verif passe par « Lancer une vérif ». Deplie s'il manque quelque chose
  if (fiche.contentOpen === null) fiche.contentOpen = shortage;
  parts.push(el('details', { class: 'fiche-content', open: fiche.contentOpen ? '' : null,
    ontoggle: (event) => { fiche.contentOpen = event.target.open; } }, el('summary', {}, 'Contenu'), ...content));
  body.replaceChildren(...parts);
}

// Ce qu'un lot demande comme attention (meme regle que serializers.attention_rank) : 3 rouge, 2 orange, 1 une
// peremption ou une revision approche, 0 rien a faire ; un simple regroupement ne compte que par ses sous-lots
function lotAttention(lot) {
  if (!ownContent(lot)) return 0;
  const kind = lotStatus(lot).kind;
  if (kind !== 'ok') return kind === 'bad' ? 3 : 2;
  return lot.expiring_soon_count || lot.revision_late_count || lot.revision_soon_count
    || lot.disinfection_late_count || lot.disinfection_soon_count ? 1 : 0;
}

// Sous-lots (en profondeur d'abord, avec 'parent') reordonnes : a chaque niveau, les branches a traiter passent
// devant, chaque lot reste suivi de ses sous-lots et l'ordre d'origine departage les egalites
function attentionTree(rows) {
  const ids = new Set(rows.map((row) => row.id));
  const children = new Map();
  const roots = [];
  for (const row of rows) {
    if (ids.has(row.parent) && row.parent !== row.id) {
      if (!children.has(row.parent)) children.set(row.parent, []);
      children.get(row.parent).push(row);
    } else roots.push(row);
  }
  const ranks = new Map();
  const rank = (row) => {
    if (!ranks.has(row.id)) {
      ranks.set(row.id, lotAttention(row)); // garde-fou contre un cycle
      ranks.set(row.id, Math.max(ranks.get(row.id), ...(children.get(row.id) || []).map(rank)));
    }
    return ranks.get(row.id);
  };
  const ordered = [];
  const seen = new Set();
  const walk = (level) => {
    for (const row of [...level].sort((a, b) => rank(b) - rank(a))) {
      if (seen.has(row.id)) continue;
      seen.add(row.id);
      ordered.push(row);
      walk(children.get(row.id) || []);
    }
  };
  walk(roots);
  walk(rows);
  return ordered;
}

// Rang d'un item deplie sous sa jauge : perime ou inutilisable, disparu, signale ou revision en retard, puis le reste
const iidAttention = ([item, missingTag]) => (item.expired ? 0 : missingTag ? 1
  : (item.reported && !item.unusable) || item.revision_late || item.disinfection_late ? 2 : 3);

// Ligne d'un type dans la fiche de lot, avec une fleche au bout pour deplier ses iids (replies par defaut,
// remis a zero a chaque fiche ouverte)
function ficheFold(key, rows, line) {
  if (!showIds()) return [line(null)];
  const open = fiche.folds.has(key);
  const arrow = el('button', { type: 'button', class: 'fold', disabled: !rows.length, 'aria-expanded': String(open),
    'aria-label': open ? 'Replier les items' : 'Afficher les items',
    onclick: () => { if (open) fiche.folds.delete(key); else fiche.folds.add(key); renderFiche(); } }, open ? '▾' : '▸');
  return [line(arrow), open ? el('ul', { class: 'iids' }, ...rows.map(([item, tag]) => iidRow(item, tag))) : ''];
}

// Item deplie sous sa jauge : iid et peremption, touche pour ouvrir sa fiche
function iidRow(item, missingTag) {
  return el('li', { class: item.expired ? 'expired' : missingTag ? 'missing' : '', onclick: () => openFiche('item', item.iid) },
    el('span', { class: 'iid' }, ficheLink('item', item.iid, item.iid)),
    el('span', { class: 'date' }, item.peremption ? fmtDate(item.peremption) : 'non périssable'),
    item.expired ? el('span', { class: 'tag red' }, badLabel(item)) : null,
    item.reported && !item.unusable ? el('span', { class: 'tag orange' }, 'signalé') : null,
    item.revision_late && !item.expired ? el('span', { class: 'tag orange' }, 'révision en retard') : null,
    item.disinfection_late && !item.expired ? el('span', { class: 'tag orange' }, 'désinfection en retard') : null,
    missingTag ? el('span', { class: 'tag orange' }, missingTag) : null);
}

// Prochaine peremption d'un lot et de ses sous-lots : la plus proche parmi les items pas encore perimes
function expiryLine(lot) {
  let next = null;
  let expired = 0;
  for (const sub of [lot, ...(lot.descendants || [])]) {
    for (const item of sub.items || []) {
      if (item.expired) { expired += 1; continue; }
      if (!item.peremption) continue;
      if (!next || item.peremption < next.date) next = { date: item.peremption, names: new Set() };
      if (item.peremption === next.date) next.names.add(item.type_name);
    }
  }
  const soon = next && parseDate(next.date) - today() <= 30 * 86400000;
  return el('p', { class: soon ? 'warn-text' : '' }, (soon ? '⚠ ' : '') + 'Prochaine péremption : ',
    el('b', {}, next ? fmtDate(next.date) : 'aucune'),
    next ? ` (${[...next.names].join(', ')})` : ' (aucun item périssable)',
    expired ? ` · ${nb(expired, 'item déjà périmé', 'items déjà périmés')}` : '');
}

// Prochaine revision et prochaine desinfection du materiel d'un lot et de ses sous-lots (rien si le lot n'a pas de
// materiel a reviser ou a desinfecter), une ligne par entretien
function revisionLine(lot) {
  return [['revision', 'révision'], ['disinfection', 'désinfection']].map(([kind, noun]) => {
    let next = null;
    let late = 0;
    for (const sub of [lot, ...(lot.descendants || [])]) {
      for (const item of sub.items || []) {
        if (!item[kind] || !item[kind + '_due']) continue;
        if (item[kind + '_late']) { late += 1; continue; }
        const due = item[kind + '_due'];
        if (!next || due < next.date) next = { date: due, names: new Set() };
        if (due === next.date) next.names.add(item.type_name);
      }
    }
    if (!next && !late) return '';
    const soon = late || (next && parseDate(next.date) - today() <= 30 * 86400000);
    return el('p', { class: soon ? 'warn-text' : '' }, (soon ? '⚠ ' : '') + `Prochaine ${noun} : `,
      el('b', {}, next ? fmtDate(next.date) : '–'),
      next ? ` (${[...next.names].join(', ')})` : '',
      late ? ` · ${nb(late, noun)} en retard` : '');
  });
}

const ITEM_STATUS = { out: 'Sorti du stock', loose: 'Hors lot, pas encore en stock', missing: 'Signalé disparu', deleted: 'Marqué supprimé', replaced: 'Remplacé (périmé)' };

// Objet qu'on peut perdre (type coche « message objet perdu », ex : ordinateur) : la fiche est d'abord pour la
// personne qui l'a trouve et scanne son etiquette. Le message commun, sans etat vert, sans lot ni informations de
// verif (ces objets ne sont souvent dans aucune verif). Gestion/admin gardent un etat a remplacer et les dates utiles.
function renderLostObjectFiche(body, item) {
  $('#fiche-launch').hidden = true;
  const add = $('#fiche-add');
  add.hidden = !verifInProgress();
  add.textContent = `Ajouter aux scans de la vérif en cours (${state.lot ? state.lot.name : ''})`;
  $('#fiche-actions').hidden = add.hidden;
  const bad = item.disabled ? '✘ Désactivé : à remplacer' : item.unusable ? '✘ Signalé inutilisable : à remplacer'
    : item.outdated ? '✘ Périmé' : item.expired ? '✘ À remplacer' : '';
  const fact = (label, ...value) => [el('dt', {}, label), el('dd', {}, ...value)];
  const message = (item.lost_message || '').trim();
  body.replaceChildren(
    el('h2', {}, item.type_name),
    showIds() ? el('div', { class: 'sub' }, item.iid) : '',
    el('div', { class: 'lost-message' }, el('b', {}, 'Objet perdu ?'),
      message ? el('p', {}, message) : el('p', { class: 'sub' }, 'Aucun message défini.')),
    detailed() && bad ? el('div', { class: 'banner bad' }, bad) : '',
    hasIdentity() ? reportsBlock((item.reports || []).map((report) => ({ report }))) : '',
    hasIdentity() ? maintenanceButtons(item) : '',
    hasIdentity() ? reportButton({ item: item.iid, label: withId(item.type_name, item.iid), lot: item.location, lotName: item.location_name }) : '',
    detailed() ? el('dl', { class: 'facts' },
      item.peremption ? fact('Péremption', fmtDate(item.peremption)) : [],
      ...maintenanceList(item).map((m) => fact(m.label, m.due ? `prochaine le ${fmtDate(m.due)}` : 'pas encore prévue')),
      fact('Ajouté', item.added ? cap(fmtDateTime(item.added, { long: true })) + (item.added_by ? ' par ' + item.added_by : '') : '–'),
      item.deleted_reason ? fact('Raison', item.deleted_reason) : []) : '',
  );
}

function renderItemFiche(body, item) {
  if (item.lost_object) { renderLostObjectFiche(body, item); return; }
  const where = item.last_location;
  const launch = $('#fiche-launch');
  setLabel(launch, 'verif', where ? `Lancer une vérif de ${where.name}` : 'Lancer une vérif');
  launch.disabled = !where;
  const add = $('#fiche-add');
  add.hidden = !verifInProgress();
  add.textContent = `Ajouter aux scans de la vérif en cours (${state.lot ? state.lot.name : ''})`;
  const statusText = ITEM_STATUS[item.status];
  const late = maintenanceList(item).filter((m) => m.late);
  const blocking = late.find((m) => !m.usable);
  const banner = item.disabled ? { kind: 'bad', label: '✘ Désactivé : à remplacer' + (statusText ? ' · ' + statusText : '') }
    : item.unusable ? { kind: 'bad', label: '✘ Signalé inutilisable : à remplacer' + (statusText ? ' · ' + statusText : '') }
    : item.outdated ? { kind: 'bad', label: '✘ Périmé' + (statusText ? ' · ' + statusText : '') }
    : item.expired && blocking ? { kind: 'bad', label: `✘ ${blocking.label} en retard : inutilisable avant ${blocking.noun}` + (statusText ? ' · ' + statusText : '') }
    : item.expired ? { kind: 'bad', label: '✘ À remplacer' + (statusText ? ' · ' + statusText : '') }
    : late.length ? { kind: 'warn', label: `⚠ ${late.map((m) => m.label).join(' et ')} en retard, toujours utilisable` + (statusText ? ' · ' + statusText : '') }
    : statusText ? { kind: 'warn', label: '⚠ ' + statusText } : { kind: 'ok', label: '✔ Valide' };
  const fact = (label, ...value) => [el('dt', {}, label), el('dd', {}, ...value)];
  const place = where
    ? [...where.path.flatMap((parent) => [lotLink(parent), ' › ']), lotLink(where),
      where.current ? '' : el('div', { class: 'sub' }, 'Dernier emplacement connu' + (where.at ? `, quitté ${fmtOn(where.at, { long: true })}` : ''))]
    : [item.status === 'active' ? 'En stock · sans rangement' : 'Inconnu'];
  body.replaceChildren(
    el('h2', {}, item.type_name),
    showIds() ? el('div', { class: 'sub' }, item.iid) : '',
    el('div', { class: 'banner ' + banner.kind }, banner.label),
    item.reported && !item.unusable ? el('p', { class: 'warn-text' }, '⚠ Problème signalé : toujours utilisable, suivi par le TECH.') : '',
    reportsBlock((item.reports || []).map((report) => ({ report }))),
    maintenanceButtons(item),
    reportButton({ item: item.iid, label: withId(item.type_name, item.iid), lot: item.location, lotName: item.location_name }),
    el('dl', { class: 'facts' },
      item.disabled ? fact('Désactivé', item.disabled_reason || '–', el('div', { class: 'sub' },
        [item.disabled_at ? cap(fmtOn(item.disabled_at, { long: true })) : '', item.disabled_by ? 'par ' + item.disabled_by : ''].filter(Boolean).join(' '))) : [],
      fact('Péremption', item.peremption ? fmtDate(item.peremption) : 'Non périssable'),
      fact(where && !where.current ? 'Dernier lieu' : 'Emplacement', ...place),
      item.status === 'out' && item.out_until ? fact('Compté utilisé', `le ${fmtDate(item.out_until)} s'il ne revient pas`) : [],
      fact('Dernier passage', item.last_seen ? cap(fmtDateTime(item.last_seen, { long: true })) + (item.last_seen_by ? ' par ' + item.last_seen_by : '') : '–'),
      fact('Ajouté', item.added ? cap(fmtDateTime(item.added, { long: true })) + (item.added_by ? ' par ' + item.added_by : '') : '–'),
      item.tear_off || (item.label_style && item.label_style !== 'qr')
        ? fact('Étiquette', [{ small: 'QR code réduit', barcode: 'code-barres', sheet: 'planche' }[item.label_style] || '', item.tear_off ? 'à déchirer avant utilisation' : ''].filter(Boolean).join(', ')) : [],
      detailed() && item.consumable === false ? fact('Usage', 'non consommable : retiré d\'un lot, il passe hors lot (pas perdu)') : [],
      ...maintenanceList(item).flatMap((m) => [
        fact(m.label, m.due ? `prochaine le ${fmtDate(m.due)}` + (m.late ? ' (en retard)' : '') : 'pas encore prévue',
          el('div', { class: 'sub' }, cycleText(m) + (m.usable ? '' : ', inutilisable en retard'))),
        m.done_at ? fact(`Dernière ${m.noun}`, cap(fmtOn(m.done_at, { long: true })) + (m.done_by ? ' par ' + m.done_by : '')) : [],
        m.tasks.length ? fact(`Tâches de ${m.noun}`, el('ul', { class: 'tasks-facts' }, ...m.tasks.map((task) => el('li', {}, task)))) : [],
      ]),
      (item.tasks || []).length ? fact('Tâches à la vérif', el('ul', { class: 'tasks-facts' }, ...item.tasks.map((task) => el('li', {}, task)))) : [],
      item.deleted_reason ? fact('Raison', item.deleted_reason) : []),
    where ? '' : el('p', { class: 'sub' }, "Aucun lot connu pour cet item : il n'y a pas de vérif à lancer."),
  );
}


// Revision du materiel (fiche de l'item, « Réviser ») : connexion, taches toutes cochees, prochaine revision
// (par defaut aujourd'hui + la periodicite du type, modifiable)
function isoDay(d) { return `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, '0')}-${String(d.getDate()).padStart(2, '0')}`; }

function addMonths(d, months) {
  const day = d.getDate();
  const result = new Date(d.getFullYear(), d.getMonth() + months, 1);
  result.setDate(Math.min(day, new Date(result.getFullYear(), result.getMonth() + 1, 0).getDate()));
  return result;
}

// Revision et desinfection de l'item sous une meme forme (serveur : item.maintenance). Repli sur les champs de la
// revision pour une fiche en cache d'avant la desinfection.
function maintenanceList(item) {
  if (item.maintenance) return item.maintenance;
  return item.revision ? [{ kind: 'revision', label: 'Révision', noun: 'révision', verb: 'Réviser', due: item.revision_due,
    late: !!item.revision_late, usable: item.revision_usable !== false, months: item.revision_months || 0,
    tasks: item.revision_tasks || [], done_at: item.revised_at, done_by: item.revised_by }] : [];
}

const MAINTENANCE_ICONS = { revision: 'wrench', disinfection: 'drop' };

// Periodicite et ou en est le cycle (« tous les 12 mois · mois 8 sur 12 »)
function cycleText(m) {
  return (m.months === 1 ? 'tous les mois' : `tous les ${m.months} mois`)
    + (m.cycle_month && m.months > 1 && !m.late ? ` · mois ${m.cycle_month} sur ${m.months} du cycle` : '');
}

// « Réviser », « Désinfecter » (connexion demandee), et pour gestion/admin « Saisir une date » (cycle recale a la main)
function maintenanceButtons(item) {
  if (['deleted', 'replaced'].includes(item.status)) return '';
  const rows = maintenanceList(item);
  if (!rows.length) return '';
  return el('div', { class: 'maintenance-buttons' }, ...rows.map((m) => el('div', { class: 'maintenance-row' },
    el('button', { type: 'button', class: 'primary revise-btn', onclick: () => openRevision(item, m.kind) },
      icon(MAINTENANCE_ICONS[m.kind] || 'wrench'), m.verb),
    canSeeStock() ? el('button', { type: 'button', class: 'text-link', onclick: () => openMaintenanceDate(item, m.kind) },
      `Saisir la date de ${m.noun}`) : '')));
}

function openRevision(item, kind = 'revision') {
  const m = maintenanceList(item).find((row) => row.kind === kind);
  if (!m) return;
  requireLogin(`faire la ${m.noun}`, () => showRevision(item, m));
}

function afterMaintenance() {
  if (ficheOpen()) openFiche(fiche.kind, fiche.id, { key: fiche.key, seal: fiche.seal, push: false, label: fiche.label });
  if (state.homeView === 'events' && typeof loadEvents === 'function') loadEvents();
}

function showRevision(item, m) {
  const dialog = $('#revision');
  const tasks = m.tasks || [];
  $('#revision-title').textContent = `${m.label} : ${item.type_name}`;
  $('#revision-sub').textContent = [idText(item.iid), m.due ? `prévue le ${fmtDate(m.due)}` : ''].filter(Boolean).join(' · ');
  $('#revision-intro').textContent = tasks.length ? `Cochez toutes les tâches de la ${m.noun}.` : 'Pas de tâche définie pour ce type.';
  $('#revision-next-label').textContent = `Prochaine ${m.noun} le`;
  setLabel($('#revision-ok'), 'check', `Enregistrer la ${m.noun}`);
  const boxes = tasks.map(() => el('input', { type: 'checkbox' }));
  const next = $('#revision-next');
  const update = () => { $('#revision-ok').disabled = !boxes.every((box) => box.checked) || !next.value; };
  for (const box of boxes) box.addEventListener('change', update);
  $('#revision-list').replaceChildren(...tasks.map((task, index) => el('li', {}, el('label', {}, boxes[index], ' ', task))));
  const tomorrow = new Date(today().getTime() + 86400000);
  next.min = isoDay(tomorrow);
  next.value = isoDay(addMonths(today(), m.months || 1));
  next.oninput = update;
  $('#revision-next-hint').textContent = `Par défaut dans ${nb(m.months, 'mois', 'mois')} (périodicité du type).`;
  $('#revision-note').value = '';
  $('#revision-error').textContent = '';
  update();
  $('#revision-form').onsubmit = async (event) => {
    event.preventDefault();
    $('#revision-ok').disabled = true;
    try {
      await api(`items/${encodeURIComponent(item.iid)}/revision/`, { user: credentials(), kind: m.kind, tasks,
        next_due: next.value, note: $('#revision-note').value });
      dialog.close();
      feedback.info();
      toast(`${m.label} enregistrée : prochaine le ${fmtDate(next.value)}.`);
      afterMaintenance();
    } catch (e) {
      update();
      $('#revision-error').textContent = e.message;
      if (e.status === 403) { state.user = null; save(); render(); }
    }
  };
  $('#revision-cancel').onclick = () => dialog.close();
  if (!dialog.open) dialog.showModal();
}

// Date de revision ou de desinfection saisie a la main (gestion/admin) : derniere faite (la prochaine se calcule
// avec la periodicite) ou prochaine directement, pour recaler le cycle sans faire l'entretien dans le logiciel.
function openMaintenanceDate(item, kind) {
  const m = maintenanceList(item).find((row) => row.kind === kind);
  if (!m) return;
  requireLogin(`saisir la date de ${m.noun}`, () => showMaintenanceDate(item, m));
}

function showMaintenanceDate(item, m) {
  const dialog = $('#maintenance-date');
  const last = $('#maintenance-last');
  const next = $('#maintenance-next');
  $('#maintenance-title').textContent = `Date de ${m.noun} : ${item.type_name}`;
  $('#maintenance-sub').textContent = [idText(item.iid), m.due ? `prévue le ${fmtDate(m.due)}` : 'pas encore prévue',
    cycleText(m)].filter(Boolean).join(' · ');
  $('#maintenance-last-label').textContent = `Dernière ${m.noun} faite le`;
  $('#maintenance-next-label').textContent = `Prochaine ${m.noun} le`;
  last.max = isoDay(today());
  last.value = m.done_at ? m.done_at.slice(0, 10) : '';
  next.value = m.due || '';
  let nextTouched = false;
  const update = () => {
    $('#maintenance-ok').disabled = !last.value && !next.value;
    $('#maintenance-hint').textContent = last.value && !nextTouched
      ? `Prochaine calculée : dernière + ${nb(m.months, 'mois', 'mois')}.` : 'La prochaine peut être passée si le retard est constaté.';
  };
  last.oninput = () => {
    if (last.value && !nextTouched) next.value = isoDay(addMonths(parseDate(last.value), m.months || 1));
    update();
  };
  next.oninput = () => { nextTouched = true; update(); };
  $('#maintenance-note').value = '';
  $('#maintenance-error').textContent = '';
  update();
  $('#maintenance-form').onsubmit = async (event) => {
    event.preventDefault();
    $('#maintenance-ok').disabled = true;
    try {
      const body = await api(`items/${encodeURIComponent(item.iid)}/maintenance/date/`, { user: credentials(), kind: m.kind,
        last_done: last.value || null, next_due: next.value || null, note: $('#maintenance-note').value });
      dialog.close();
      toast(`${m.label} : prochaine le ${fmtDate(body[m.kind + '_due'])}.`);
      afterMaintenance();
    } catch (e) {
      update();
      $('#maintenance-error').textContent = e.message;
      if (e.status === 403) { state.user = null; save(); render(); }
    }
  };
  $('#maintenance-cancel').onclick = () => dialog.close();
  if (!dialog.open) dialog.showModal();
}

// Rapport d'etat d'un lot a une date donnee (admins, web seulement) : derniere verif, contenu detaille avec iid
// et peremption, sous-lots en cascade, qui l'a edite et quand, case « Certifié par le TECH <antenne> ». Le PDF
// arrive par une requete POST (badge) : il s'ouvre ensuite par un lien, clic de l'utilisateur (bloqueurs de popups).
let stateReportUrl = null;
function openStateReport(lot) {
  const dialog = $('#state-report');
  const now = new Date();
  $('#state-report-lot').textContent = withId(lot.name, lot.id) + ((lot.descendants || []).length > 1 ? ' et ses sous-lots' : '');
  $('#state-report-day').value = isoDay(now);
  $('#state-report-day').max = isoDay(now);
  $('#state-report-time').value = `${String(now.getHours()).padStart(2, '0')}:${String(now.getMinutes()).padStart(2, '0')}`;
  $('#state-report-antenna').value = state.antennaName || '';
  $('#state-report-error').textContent = '';
  $('#state-report-ready').hidden = true;
  $('#state-report-ok').disabled = false;
  $('#state-report-form').onsubmit = async (event) => {
    event.preventDefault();
    const ok = $('#state-report-ok');
    ok.disabled = true;
    $('#state-report-error').textContent = '';
    $('#state-report-ready').hidden = true;
    const antenna = $('#state-report-antenna').value.trim();
    const at = `${$('#state-report-day').value}T${$('#state-report-time').value}`;
    try {
      let response;
      try {
        response = await fetch(new URL(`lots/${encodeURIComponent(lot.id)}/state-report/`, API), {
          method: 'POST', headers: { 'Content-Type': 'application/json', Accept: 'application/pdf, application/json' },
          body: JSON.stringify({ user: credentials(), at, antenna }) });
      } catch (e) {
        throw Object.assign(new Error('Serveur injoignable, vérifiez le réseau.'), { status: 0 });
      }
      if (!response.ok) {
        let data = {};
        try { data = await response.json(); } catch (e) { /* reponse vide */ }
        throw Object.assign(new Error(data.error || data.detail || `Erreur ${response.status}`), { status: response.status });
      }
      const blob = await response.blob();
      state.antennaName = antenna;
      if (stateReportUrl) URL.revokeObjectURL(stateReportUrl);
      stateReportUrl = URL.createObjectURL(blob);
      const name = (/filename="([^"]+)"/.exec(response.headers.get('Content-Disposition') || '') || [])[1] || 'rapport.pdf';
      const [day, time] = [$('#state-report-day').value, $('#state-report-time').value];
      $('#state-report-ready').replaceChildren(
        el('a', { class: 'button-link', href: stateReportUrl, target: '_blank', rel: 'noopener' }, `📋 Ouvrir le rapport au ${fmtDate(day)} à ${time}`),
        el('a', { class: 'link', href: stateReportUrl, download: name }, 'Télécharger'));
      $('#state-report-ready').hidden = false;
      feedback.info();
    } catch (e) {
      $('#state-report-error').textContent = e.message;
      if (e.status === 403) { state.user = null; save(); render(); dialog.close(); }
    }
    ok.disabled = false;
  };
  $('#state-report-cancel').onclick = () => dialog.close();
  if (!dialog.open) dialog.showModal();
}

// Signalements en cours sur une fiche (lot, sous-lot, item) : toujours visibles, pour tous, sous l'etat ; les
// inutilisables en haut. entries : [{report, where}] (where : lot ou item concerne, s'il n'est pas evident)
function reportsBlock(entries) {
  if (!entries.length) return '';
  const sorted = [...entries].sort((a, b) => Number(a.report.usable) - Number(b.report.usable));
  return el('div', { class: 'fiche-reports' }, el('h3', {}, entries.length > 1 ? 'Signalements en cours' : 'Signalement en cours'),
    ...sorted.map(({ report, where }) => el('div', { class: 'reported-item' + (report.usable ? '' : ' bad') },
      el('b', {}, report.usable ? '⚠ Problème signalé, toujours utilisable' : '✘ Non utilisable : signalé'),
      where ? el('div', {}, where) : '',
      el('div', { class: 'sub' }, `${report.status_label} · signalé par ${report.by} ${fmtOn(report.created, { long: true })}`),
      report.comment ? el('p', { class: 'comment' }, '« ' + report.comment + ' »') : el('p', { class: 'sub' }, 'Sans commentaire.'),
      ...repliesBlock(report))));
}

// Reponses du TECH a un signalement (deux au plus, apres le message du signalement) : celle donnee en le passant en
// cours, puis celle de la resolution
function repliesBlock(report) {
  return [
    report.note ? el('p', { class: 'note' }, '💬 ' + report.note) : '',
    report.resolution_note ? el('p', { class: 'note' }, '✔ ' + report.resolution_note) : '',
  ];
}

// Signalements en cours d'un lot et de ses sous-lots : sur le lot lui-meme, sur un type d'item, ou sur un item
// qu'il contient (avec ce qui est concerne, quand ce n'est pas le lot de la fiche)
function lotReportsBlock(lot) {
  const entries = [];
  const seen = new Set();
  for (const sub of [lot, ...(lot.descendants || [])]) {
    for (const report of sub.reports || []) {
      if (seen.has(report.id)) continue;
      seen.add(report.id);
      const what = report.type_name ? `${report.type_name} (${sub.name})` : sub.id === lot.id ? '' : sub.name;
      entries.push({ report, where: what ? el('span', {}, 'Concerne : ', sub.id === lot.id ? what : ficheLink('lot', sub.id, what)) : '' });
    }
    for (const item of sub.items || []) {
      for (const report of item.reports || []) {
        if (seen.has(report.id)) continue;
        seen.add(report.id);
        const what = withId(item.type_name, item.iid);
        entries.push({ report, where: el('span', {}, 'Concerne : ', ficheLink('item', item.iid, what),
          sub.id === lot.id ? '' : ` (${sub.name})`) });
      }
    }
  }
  return reportsBlock(entries);
}

// Item (ou paquet ferme) signale scanne, quel que soit le signalement : popup avec les signalements ouverts et
// leurs commentaires, et bip. Une seule fois par objet sur une courte duree (fiche rouverte, rescan).
let lastReportAlert = { key: '', at: 0 };
function alertReports(key, title, reports) {
  if (!reports || !reports.length) return;
  if (lastReportAlert.key === key && Date.now() - lastReportAlert.at < 30000) return;
  lastReportAlert = { key, at: Date.now() };
  feedback.warn();
  $('#reported-title').textContent = title;
  $('#reported-body').replaceChildren(...reports.map((report) => el('div', { class: 'reported-item' + (report.usable ? '' : ' bad') },
    el('b', {}, report.usable ? '⚠ Toujours utilisable' : '✘ Inutilisable : à remplacer'),
    el('div', { class: 'sub' }, `${report.status_label} · signalé par ${report.by} ${fmtOn(report.created)}`),
    report.comment ? el('p', { class: 'comment' }, '« ' + report.comment + ' »') : el('p', { class: 'sub' }, 'Sans commentaire.'),
    ...repliesBlock(report))));
  const dialog = $('#reported');
  $('#reported-close').onclick = () => dialog.close();
  if (!dialog.open) dialog.showModal();
}
