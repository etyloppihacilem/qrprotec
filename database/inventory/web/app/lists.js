/* QRProtec - front web : listes de materiel (chargement, scans, validation, partage, edition). */
'use strict';

// Listes de materiel (checklists.py) : lots et materiel a prendre (ex : un chef demande du materiel a ses
// equipiers), partagees par lien et QR code (<base>/liste?id=ID). Elles se verifient en direct, a plusieurs :
// chaque scan va tout de suite au serveur et la page relit la liste. Validee (« Prêt au départ »), la liste sort son
// materiel du stock et elle est terminee ; seules les anciennes listes sorties ont encore un retour a scanner.

const LIST_POLL_MS = 3000;
const LIST_TAGS = { draft: 'blue', validated: 'orange', returned: 'green' };
const lists = {
  index: null,     // {lists, can_see_all, can_create}
  departure: null, // « Partir avec » : {lot_types, full_verif} ou {loading}
  show: 'mine',    // 'mine' (les miennes et celles attribuees), 'templates', 'all' (chef, gestion, admin)
  id: '',          // liste ouverte
  list: null,      // detail de la liste ouverte
  error: '',
  mode: false,     // lecteur ouvert sur la liste (verif, ou retour pour une liste sortie)
  message: null,   // dernier scan : {kind: 'ok' | 'bad', text}
  edit: null,      // editeur : {key, id, name, note, template, lines: [{lot, label} | {type, label, quantity}]}
  options: null,   // choix de l'editeur : {lots, types}
  busy: false,
  timer: 0,
  qr: { url: '', src: '' },
  qrOpen: '',
  verifBack: null, // « Lancer la vérif » depuis une liste : {id, name}, le compte rendu propose d'y revenir
  doneOpen: false, // listes rentrees depliees (bouton [+] en bas de l'index)
};

const listUrl = (id) => new URL(`liste?id=${encodeURIComponent(id)}`, document.baseURI).href;
const onListsPage = () => inHome() && state.homePage === 'lists';
// un code scanne sur la page d'une liste a verifier ou a rentrer y compte (lecteur ou douchette)
const listScanning = () => onListsPage() && !!lists.list && !lists.edit && !lists.list.template
  && (lists.list.status === 'draft' || lists.list.status === 'validated');

function openLists(push = true) {
  Object.assign(lists, { id: '', list: null, error: '', edit: null, message: null, verifBack: null });
  stopListScan();
  showHome('lists', push);
}

function openList(id, push = true) {
  if (lists.id !== id) Object.assign(lists, { list: null, message: null });
  Object.assign(lists, { id, error: '', edit: null, verifBack: null });
  stopListScan();
  if (push) history.pushState(null, '', listUrl(id));
  showHome('lists', false);
}

// showHome('lists') : charge l'index ou la liste ouverte, relit la liste ouverte toutes les LIST_POLL_MS
function listsShown() {
  clearInterval(lists.timer);
  lists.timer = 0;
  if (!state.user) return;
  if (lists.id) {
    loadList();
    lists.timer = setInterval(() => {
      if (!onListsPage() || !lists.id) { clearInterval(lists.timer); lists.timer = 0; return; }
      if (!lists.edit && !lists.busy && !document.hidden) loadList(true);
    }, LIST_POLL_MS);
  } else {
    loadLists();
  }
}

async function loadLists() {
  lists.busy = true;
  renderListsPage();
  try {
    lists.index = await api('checklists/list/', { user: badge(), show: lists.show });
    if (!lists.index.can_create && lists.show !== 'mine') {
      lists.show = 'mine';  // secouriste : ni modeles ni « Toutes »
      lists.index = await api('checklists/list/', { user: badge(), show: lists.show });
    }
  } catch (e) {
    toast('Listes : ' + e.message, true);
  } finally {
    lists.busy = false;
    renderListsPage();
  }
}

// Premiere lecture d'une liste ouverte (lien, QR code, ecran des listes) : open l'ajoute aux listes d'un secouriste
async function loadList(quiet = false) {
  const id = lists.id;
  try {
    const list = await api(`checklists/${encodeURIComponent(id)}/`, { user: badge(), open: !quiet });
    if (lists.id !== id) return;
    lists.list = list;
    lists.error = '';
  } catch (e) {
    if (lists.id !== id) return;
    if (!quiet || e.status === 404) lists.error = e.status === 404 ? 'Liste introuvable (supprimée ?)' : e.message;
  }
  renderListsPage();
}

// Action sur la liste ouverte : la reponse est la liste a jour
async function listCall(action, body = {}) {
  const result = await api(`checklists/${encodeURIComponent(lists.id)}/${action}/`, { user: badge(), ...body });
  if (result && result.id === lists.id) lists.list = result;
  renderListsPage();
  return result;
}

async function listAction(action, body = {}, done = null) {
  if (lists.busy) return;
  lists.busy = true;
  try {
    const result = await listCall(action, body);
    if (done) done(result);
  } catch (e) {
    feedback.bad();
    toast(e.message, true);
  } finally {
    lists.busy = false;
    renderListsPage();
  }
}

function startListScan() {
  lists.mode = true;
  document.body.classList.add('login-reader', 'list-scan');
  $('#scan-hint').textContent = lists.list && lists.list.status === 'validated' ? '↩ Retour : scannez ce qui rentre' : 'Scannez le matériel';
  startCamera();
  renderListsPage();
}

function stopListScan() {
  if (lotPick) lotPick.close();
  if (!lists.mode) return;
  lists.mode = false;
  document.body.classList.remove('list-scan');
  if (readerOpen()) closeReader();
}

async function listScanCode(scan) {
  const list = lists.list;
  const action = list.status === 'validated' ? 'return-scan' : 'scan';
  const body = scan.kind === 'lot' ? { lot: scan.id } : { item: scan.id };
  try {
    const result = await listCall(action, body);
    if (result.ask_add) { askAddLot(result.ask_add); return; }
    const picked = lotPick && lists.list && lists.list.lines.find((line) => line.id === lotPick.lineId);
    if (lotPick && lotPick.refresh) {
      lotPick.refresh();  // materiel : barre de progression, fermee quand tout est scanne
    } else if (lotPick && lotPick.back) {
      // retour : fermee quand le lot est note rentre
      if ((lists.list.back || []).some((row) => row.lot === lotPick.back)) lotPick.close();
    } else if (picked && picked.scanned >= picked.quantity) lotPick.close();
    feedback.good();
    lists.message = { kind: 'ok', text: result.message || 'Scanné' };
  } catch (e) {
    feedback.bad();
    lists.message = { kind: 'bad', text: e.message };
  }
  renderListsPage();
}

// Etiquette d'un lot hors de la liste a preparer (qui peut la modifier) : « Ajouter ce lot à la liste ? ». Un lot du
// meme type pas encore coche est remplace (on prend le Sac PSE B, le A reste au local), sinon le lot s'ajoute.
let askingAdd = false;
async function askAddLot(ask) {
  if (askingAdd) return;  // la camera relit l'etiquette pendant la question
  askingAdd = true;
  feedback.warn();
  lists.message = null;
  renderListsPage();
  const name = ask.unit_name;
  const scanned = ask.lot_name !== ask.unit_name ? ` (étiquette de ${ask.lot_name})` : '';
  const ok = await askDialog({
    title: ask.replaces ? `Prendre ${name} à la place de ${ask.replaces.lot_name} ?` : `Ajouter ${name} à la liste ?`,
    text: ask.replaces
      ? `${name}${scanned} n'est pas dans la liste. ${ask.replaces.lot_name}, du même type, n'est pas encore coché : ${name} le remplace et il est coché.`
      : `${name}${scanned} n'est pas dans la liste. Il y est ajouté et coché.`,
    choices: [{ label: ask.replaces ? `Remplacer ${ask.replaces.lot_name}` : 'Ajouter à la liste', value: true, cls: 'primary' }],
  });
  askingAdd = false;
  if (!ok) return;
  listAction('add-lot', { lot: ask.lot }, (result) => {
    feedback.good();
    lists.message = { kind: 'ok', text: result.message || 'Ajouté' };
    if (lotPick) lotPick.close();
  });
}

// Case d'un lot a prendre : grande fenetre avec la camera pour scanner son etiquette (ce qu'on veut encourager),
// et en petit « Cocher sans scanner » (le VPS reste dehors pendant qu'on prepare). Fermee d'elle-meme quand la
// ligne est cochee. La camera (#camera) est placee sur #lot-pick-cam comme dans la grande fenetre de connexion.
let lotPick = null; // { lineId, back (retour : id du lot), close }
function placeLotPickCamera() {
  const cam = $('#lot-pick-cam');
  if (!cam) return;
  const box = cam.getBoundingClientRect();
  const style = document.body.style;
  style.setProperty('--gate-left', `${box.left}px`);
  style.setProperty('--gate-top', `${box.top}px`);
  style.setProperty('--gate-width', `${box.width}px`);
  style.setProperty('--gate-height', `${box.height}px`);
}
window.addEventListener('resize', placeLotPickCamera);

// Case du materiel (depart ou retour) : meme grande fenetre que pour un lot, qui demande de scanner les items un
// par un, avec une barre de progression. Jamais coche a la main : un consommable n'est pris (ou remis en stock) que
// scanne. Fermee d'elle-meme quand tout est scanne.
function openItemPick(lineId, title, back = false) {
  if (lotPick) lotPick.close();
  const scanning = lists.mode;
  const count = () => {
    const line = lists.list && lists.list.lines.find((row) => row.id === lineId);
    if (!line) return { done: 0, total: 0, unit: '' };
    const unit = line.unit ? ' ' + line.unit : ''; // type a unites : en mL, n'importe quel sous-type
    return back ? { done: line.back, total: line.scanned, unit } : { done: line.scanned, total: line.quantity, unit };
  };
  const close = () => {
    if (!lotPick || lotPick.close !== close) return;
    lotPick = null;
    overlay.remove();
    document.body.classList.remove('lot-pick');
    if (!scanning) stopListScan();
    renderListsPage();
  };
  const bar = el('i', {});
  const counter = el('b', {});
  const refresh = () => {
    const { done, total, unit } = count();
    bar.style.width = `${total ? Math.round(100 * done / total) : 100}%`;
    counter.textContent = `${done}/${total}${unit}`;
    if (total && done >= total) {
      toast(back ? `${title} : tout est rentré` : `${title} : tout est pris`);
      close();
    }
  };
  const { total } = count();
  const overlay = el('div', { id: 'lot-pick', role: 'dialog', 'aria-modal': 'true' },
    el('h2', {}, back ? `Scannez ${total > 1 ? 'chaque' : "l'item"} « ${title} » qui rentre` : `Scannez ${total > 1 ? 'chaque' : "l'item"} « ${title} » pris`),
    el('p', {}, back ? 'Chaque item scanné est remis en stock.' : (total > 1 ? 'Prenez-les dans le stock et scannez-les un par un.' : 'Prenez-le dans le stock et scannez-le.')),
    el('div', { class: 'pick-progress' }, el('div', { class: 'return-bar' }, bar), counter),
    el('div', { id: 'lot-pick-cam' }),
    el('button', { type: 'button', class: 'secondary lot-pick-cancel', onclick: () => close() }, 'Fermer'));
  lotPick = { lineId, close, refresh };
  document.body.append(overlay);
  document.body.classList.add('lot-pick');
  if (!lists.mode) startListScan();
  $('#scan-hint').textContent = back ? '↩ Scannez ce qui rentre' : 'Scannez le matériel';
  refresh();
  placeLotPickCamera();
}

// back : retour d'une liste sortie, line = { kind: 'lot', lot } du lot pas encore rentre (meme fenetre, le scan ou
// « Rentrer sans scanner » le note rentre)
function openLotPick(line, title, back = false) {
  if (lotPick) lotPick.close();
  const scanning = lists.mode;
  const state = (row) => (row.state.kind === 'ok' ? '' : ` (${row.state.label.replace(/^[✔✘⚠] */, '').toLowerCase()})`);
  const close = () => {
    if (!lotPick || lotPick.close !== close) return;
    lotPick = null;
    overlay.remove();
    document.body.classList.remove('lot-pick');
    if (!scanning) stopListScan();
    renderListsPage();
  };
  const byHand = (lot) => { close(); listAction(back ? 'return-scan' : 'scan', { lot }); };
  const overlay = el('div', { id: 'lot-pick', role: 'dialog', 'aria-modal': 'true' },
    el('h2', {}, line.kind === 'lot' ? `Scannez l'étiquette de ${title}` : `Scannez l'étiquette d'un ${title}`),
    el('p', {}, back ? 'Le QR code collé sur le lot le note rentré.'
      : line.kind === 'lot' ? 'Le QR code collé sur le lot le coche dans la liste.'
      : 'Disponibles : ' + line.available.map((row) => row.name + state(row)).join(', ') + '.'),
    el('div', { id: 'lot-pick-cam' }),
    el('div', { class: 'lot-pick-manual' }, ...(line.kind === 'lot'
      ? [el('button', { type: 'button', class: 'text-link', onclick: () => byHand(line.lot) }, back ? 'Rentrer sans scanner' : 'Cocher sans scanner')]
      : line.available.map((row) => el('button', { type: 'button', class: 'text-link', onclick: () => byHand(row.id) },
        `Prendre ${row.name} sans scanner`)))),
    el('button', { type: 'button', class: 'secondary lot-pick-cancel', onclick: () => close() }, 'Annuler'));
  lotPick = { lineId: line.id, back: back ? line.lot : '', close };
  document.body.append(overlay);
  document.body.classList.add('lot-pick');
  if (!lists.mode) startListScan();
  $('#scan-hint').textContent = "Scannez l'étiquette du lot";
  placeLotPickCamera();
}

// Popups du site a la place de confirm() / prompt() du navigateur (jamais de popup systeme)
// confirmDialog : true si le bouton ok est touche ; cls 'danger' ou 'warning' pour une action a risque
async function confirmDialog(title, text, ok = 'Continuer', cls = 'primary') {
  return (await askDialog({ title, text, choices: [{ label: ok, value: true, cls }] })) === true;
}

// promptDialog : le texte saisi (Entree valide), null si annule
function promptDialog({ title, text = '', value = '', placeholder = '', ok = 'Valider', maxlength = 200 }) {
  return new Promise((resolve) => {
    const dialog = el('dialog', { class: 'big-popup ask-dialog' });
    const input = el('input', { type: 'text', class: 'ask-input', value, placeholder, maxlength: String(maxlength),
      autocomplete: 'off', enterkeyhint: 'done' });
    let answered = false;
    const close = (result) => {
      if (answered) return;
      answered = true;
      if (dialog.open) dialog.close();
      dialog.remove();
      resolve(result);
    };
    input.addEventListener('keydown', (event) => { if (event.key === 'Enter') { event.preventDefault(); close(input.value); } });
    dialog.append(el('h2', {}, title), text ? el('p', {}, text) : '', input,
      el('div', { class: 'dialog-stack' }, el('button', { type: 'button', class: 'primary', onclick: () => close(input.value) }, ok),
        el('button', { type: 'button', class: 'secondary', onclick: () => close(null) }, 'Annuler')));
    dialog.addEventListener('close', () => close(null));
    document.body.append(dialog);
    dialog.showModal();
    input.focus();
  });
}

// Popup de choix (au lieu d'un confirm du navigateur) : { title, text, body, choices: [{ label, value, cls }] } ;
// body : elements sous le texte (ex : liste de ce qui va se passer). Renvoie la valeur choisie, null si fermee
// (Echap, Annuler)
function askDialog({ title, text, body, choices, cancel = 'Annuler' }) {
  return new Promise((resolve) => {
    const dialog = el('dialog', { class: 'big-popup ask-dialog' });
    let answered = false;
    const close = (value) => {
      if (answered) return;
      answered = true;
      if (dialog.open) dialog.close();
      dialog.remove();
      resolve(value);
    };
    dialog.append(el('h2', {}, title), text ? el('p', {}, text) : '', body || '',
      el('div', { class: 'dialog-stack' }, ...choices.map((choice) => el('button', { type: 'button',
        class: choice.cls || 'secondary', onclick: () => close(choice.value) }, choice.label)),
      cancel ? el('button', { type: 'button', class: 'secondary', onclick: () => close(null) }, cancel) : ''));
    dialog.addEventListener('close', () => close(null));
    document.body.append(dialog);
    dialog.showModal();
  });
}

// « Rouvrir le départ » (validee par erreur, 30 min) : petit lien discret sous les boutons, pas un vrai bouton
function reopenLink(busy) {
  return el('button', { type: 'button', class: 'text-link reopen-link', disabled: busy || null, onclick: reopenList },
    'Validée par erreur ? Rouvrir le départ');
}

async function reopenList() {
  const ok = await askDialog({
    title: 'Rouvrir le départ ?',
    text: 'Le matériel sorti revient où il était. La liste redevient à préparer, avec ce qui est déjà coché.',
    choices: [{ label: 'Rouvrir le départ', value: true, cls: 'primary' }],
  });
  if (ok) listAction('reopen', {}, () => toast('Départ rouvert : la liste est de nouveau à préparer.'));
}

async function validateList() {
  const list = lists.list;
  const partial = !list.complete;
  const missing = list.lines.filter((line) => (line.kind === 'task' ? !line.scanned : line.scanned < line.quantity));
  // lots pris en vérif recommandee : la vérif d'abord (boutons en premier), partir reste possible
  const verifs = listVerifLots(list, true);
  const ok = await askDialog({
    title: partial ? 'Partir sans tout ?' : 'Prêt au départ ?',
    text: (partial ? `${nb(missing.length, 'ligne')} pas ${pl(missing.length, 'prête')} : ${missing.map((line) => line.lot_name
      || line.type_name || line.task).join(', ')}. Ce qui manque n'est pas pris. ` : '')
      + 'Le matériel scanné sort du stock. Au retour, scannez ce qui rentre et clôturez le retour : les lots pris '
      + 'passent alors en vérif recommandée.',
    body: verifs.length ? el('div', { class: 'banner warn' }, `⚠ Vérif recommandée avant de partir : ${verifs.map((lot) => lot.name).join(', ')}. `
      + 'Faites-la maintenant : vous revenez à la liste une fois la vérif enregistrée.') : '',
    choices: [
      ...verifs.map((lot) => ({ label: verifs.length > 1 ? `Lancer la vérif de ${lot.name}` : 'Lancer la vérif', value: { verif: lot.id }, cls: 'primary' })),
      { label: verifs.length ? (partial ? 'Partir sans tout ni la vérif' : 'Partir sans la vérif') : partial ? 'Partir sans tout' : 'Prêt au départ',
        value: true, cls: partial || verifs.length ? 'warning' : 'primary' }],
  });
  if (ok && ok.verif) { verifFromList(ok.verif); return; }
  if (!ok) return;
  listAction('validate', { partial }, () => {
    stopListScan();
    lists.message = null;
    feedback.good();
    toast(lists.list && !lists.list.went_out ? 'Liste terminée.' : 'Liste sortie : bon poste !');
  });
}

// « Compresses ×2, Garrot » : noms regroupes, dans l'ordre de la liste
function groupedNames(names) {
  const counts = new Map();
  for (const name of names) counts.set(name, (counts.get(name) || 0) + 1);
  return [...counts].map(([name, count]) => (count > 1 ? `${name} ×${count}` : name)).join(', ');
}

// Liste sortie (retour a scanner) : meme popup que « Prêt au départ ? »,
// avec ce qui n'est pas rentre et ce qui en sera fait (return_preview du serveur)
async function closeListReturn() {
  const preview = lists.list.return_preview || { back: 0, lots: [], used: [], still_out: [] };
  const steps = [];
  if (preview.lots.length) {
    const many = preview.lots.length > 1;
    steps.push(['🔍', `${preview.lots.join(', ')} : ${many ? 'passent' : 'passe'} en vérif recommandée, `
      + `${many ? 'leur' : 'son'} contenu a pu servir.`]);
  }
  if (preview.used.length) {
    steps.push(['🩹', `${groupedNames(preview.used)} : consommable pas rentré, compté comme utilisé.`]);
  }
  if (preview.still_out.length) {
    steps.push(['📤', `${groupedNames(preview.still_out)} : matériel pas rentré, il reste sorti et rentrera en stock au prochain scan.`]);
  }
  const ok = await askDialog({
    title: steps.length ? 'Clôturer le retour sans tout ?' : 'Clôturer le retour ?',
    text: steps.length ? "Pas encore rentré :" : `Tout est rentré${preview.back ? ` (${nb(preview.back, 'scan')})` : ''}.`,
    body: el('div', {},
      steps.length ? el('ul', { class: 'verif-help-steps' }, ...steps.map(([emoji, line]) => el('li', {}, el('span', {}, emoji), el('div', {}, line)))) : '',
      el('p', { class: 'hint' }, (steps.length ? 'Du matériel est revenu ? Annulez et scannez-le avant de clôturer. ' : '')
        + 'La liste est ensuite terminée.')),
    choices: [{ label: steps.length ? 'Clôturer sans tout' : 'Clôturer le retour', value: true, cls: steps.length ? 'warning' : 'primary' }],
  });
  if (!ok) return;
  listAction('close-return', {}, (result) => {
    stopListScan();
    lists.message = null;
    feedback.good();
    const parts = [];
    if ((result.lots || []).length) parts.push(`${nb(result.lots.length, 'lot')} en vérif recommandée`);
    if ((result.used || []).length) parts.push(nb(result.used.length, 'consommable utilisé', 'consommables utilisés'));
    if ((result.still_out || []).length) parts.push(nb(result.still_out.length, 'matériel encore sorti', 'matériels encore sortis'));
    toast('Retour clôturé' + (parts.length ? ' : ' + parts.join(', ') : ' : tout est rentré') + '.');
  });
}

async function duplicateList(asTemplate = false) {
  const list = lists.list;
  const name = await promptDialog({ title: asTemplate ? 'Nom du modèle' : 'Nom de la nouvelle liste', value: list.name,
    ok: asTemplate ? 'Créer le modèle' : 'Créer la liste', maxlength: 80 });
  if (name === null) return;
  listAction('duplicate', { name: name || list.name, template: asTemplate }, (copy) => {
    toast(asTemplate ? 'Modèle créé.' : 'Liste créée : à vérifier.');
    lists.show = asTemplate ? 'templates' : lists.show === 'templates' ? 'mine' : lists.show;  // retour vers l'onglet ou elle est
    openList(copy.id);
  });
}

async function deleteList() {
  if (!await confirmDialog(`Supprimer la liste « ${lists.list.name} » ?`, '', 'Supprimer', 'danger')) return;
  listAction('delete', {}, () => { toast('Liste supprimée.'); openLists(); });
}

function shareList(list) {
  if (navigator.share) {
    navigator.share({ title: `QRProtec : ${list.name}`, text: `Liste de matériel « ${list.name} »`, url: list.url })
      .catch(() => { /* partage annule */ });
  } else if (navigator.clipboard) {
    navigator.clipboard.writeText(list.url).then(() => toast('Lien copié.'), () => toast(list.url));
  } else toast(list.url);
}


function listQr(url) {
  if (lists.qr.url !== url) {
    lists.qr = { url, src: '' };
    if (typeof qrcode === 'function') {
      const qr = qrcode(0, 'M');
      qr.addData(url);
      qr.make();
      lists.qr.src = qr.createDataURL(8, 4);
    }
  }
  return lists.qr.src;
}

// Editeur : nouvelle liste (list absent) ou liste a preparer
async function editList(list = null) {
  lists.edit = {
    key: String(Date.now()),
    id: list ? list.id : '',
    name: list ? list.name : '',
    note: list ? list.note : '',
    date: list && list.date ? list.date : isoToday(), // jour du poste, aujourd'hui par defaut
    template: list ? list.template : lists.show === 'templates',
    started: !!(list && list.lines.some((line) => line.scanned)),
    // sacs d'un B+ de referentiel (sans etiquette) : coches a part dans la liste, mais choisis d'un bloc (le B+)
    lines: list ? list.lines.filter((line, index) => !line.part_of || list.lines.findIndex((other) => other.part_of === line.part_of) === index)
      .map((line) => line.kind === 'task' ? { task: line.task, label: line.task } : line.kind === 'lot' && line.part_of
      ? { lot: line.part_of, label: line.part_of_name, sub: line.part_of_type_name,
        parts: list.lines.filter((other) => other.part_of === line.part_of)
          .map((other) => ({ lot: other.lot, label: other.lot_name, sub: other.lot_type_name })) } : line.kind === 'lot'
      ? { lot: line.lot, label: line.lot_name, sub: line.lot_type_name }
      : line.kind === 'lot_type' ? { lot_types: line.lot_types, label: line.type_name, sub: 'Lot au choix', quantity: line.quantity }
      : { type: line.type, label: line.type_name, quantity: line.quantity, unit: line.unit || '' }) : [],
  };
  renderListsPage(true);
  if (!lists.options) {
    try {
      lists.options = await api('checklists/options/', { user: badge() });
    } catch (e) {
      toast('Lots et types : ' + e.message, true);
    }
    renderListsPage(true);
  }
}

async function saveList() {
  const edit = lists.edit;
  if (!edit.name.trim()) { toast('Donnez un nom à la liste.', true); return; }
  const body = { user: badge(), name: edit.name, note: edit.note, date: edit.date || isoToday(), template: edit.template,
    lines: edit.lines.map((line) => line.task ? { task: line.task } : line.lot ? { lot: line.lot }
      : line.lot_types ? { lot_types: line.lot_types, quantity: line.quantity }
      : { type: line.type, quantity: line.quantity }) };
  lists.busy = true;
  try {
    const result = await api(edit.id ? `checklists/${encodeURIComponent(edit.id)}/update/` : 'checklists/', body);
    lists.edit = null;
    lists.busy = false;
    toast('Liste enregistrée.');
    if (edit.id) { lists.list = result; renderListsPage(); } else openList(result.id);
  } catch (e) {
    lists.busy = false;
    toast(e.message, true);
  }
}

function renderListsPage(force = false) {
  const view = $('#home-lists');
  if (!view || !onListsPage()) return;
  if (!state.user) {
    delete view.dataset.editing;
    view.replaceChildren(
      el('h3', {}, lists.id ? 'Liste de matériel' : 'Listes de matériel'),
      el('p', {}, lists.id ? 'Connectez-vous pour ouvrir cette liste et la vérifier.'
        : 'Connectez-vous pour voir et préparer les listes de matériel.'),
      el('button', { type: 'button', class: 'primary', onclick: () => openReader('lists') }, 'Scanner mon badge'),
      el('button', { type: 'button', class: 'secondary list-gap', onclick: () => { pendingAfterLogin = 'lists'; loginAction = null; openNameLogin(); } },
        'Sans badge : nom et code PIN'));
    return;
  }
  if (lists.edit) {
    // pas de nouveau rendu pendant la saisie (le champ garderait son focus perdu)
    if (!force && view.dataset.editing === lists.edit.key) return;
    view.dataset.editing = lists.edit.key;
    view.replaceChildren(...listEditor(lists.edit));
    return;
  }
  delete view.dataset.editing;
  // recherche d'un secouriste en cours : la relecture de la liste ne doit pas lui voler le focus
  if (!force && document.activeElement && document.activeElement.closest('#home-lists .member-picker')) return;
  view.replaceChildren(...(lists.id ? listDetail() : listIndex()));
}

// « Partir avec » : types de lots proposes, avec les lots disponibles ; le choix cree la liste de depart
async function openDeparture() {
  if (lists.departure) { lists.departure = null; renderListsPage(true); return; }
  lists.departure = { loading: true };
  renderListsPage(true);
  try {
    lists.departure = await api('checklists/departure/', { user: badge() });
  } catch (e) {
    lists.departure = null;
    toast(e.message, true);
  }
  renderListsPage(true);
}

async function startDeparture(type) {
  if (lists.busy) return;
  lists.busy = true;
  try {
    const list = await api('checklists/departure/', { user: badge(), lot_type: type });
    lists.departure = null;
    lists.busy = false;
    openList(list.id);
  } catch (e) {
    lists.busy = false;
    toast(e.message, true);
  }
}

function departurePicker() {
  const data = lists.departure;
  if (data.loading) return el('p', { class: 'hint' }, 'Chargement des lots…');
  return el('div', { class: 'departure' },
    el('p', { class: 'hint' }, "Choisissez ce avec quoi vous partez : la liste générée demande l'étiquette de chaque lot à "
      + 'prendre.'
      + (data.full_verif ? ' Une vérif complète de chaque lot non scellé est demandée avant de partir.'
        : ' Touchez « Vérif recommandée » au retour sur ce qui a servi.')),
    el('ul', { class: 'list' }, ...data.lot_types.map((row) => el('li', { onclick: () => startDeparture(row.type) },
      el('div', { class: 'main' }, el('div', { class: 'name' }, row.name),
        el('div', { class: 'sub' }, row.available ? `${nb(row.available, 'disponible')}, dont ${nb(row.available_ok, 'valide')}`
          : nb(row.lines, 'ligne') + ' à prendre')),
      el('span', { class: 'tag ' + (row.available_ok ? 'green' : row.available ? 'orange' : 'gray') }, 'Partir')))));
}
