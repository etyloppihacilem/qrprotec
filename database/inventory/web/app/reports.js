/* QRProtec - front web : signalements de problemes. */
'use strict';

// Signalements de problemes : (!) a cote d'un item ou d'un type pendant la vérif, « Signaler un problème » sur les
// fiches et l'onglet Lot. Connexion demandee avant de rediger quoi que ce soit ; envoye tout de suite, sans
// attendre la vérif. Un item signale pendant la vérif compte comme vu (jamais marque disparu) ; signale
// inutilisable, il est a remplacer comme un perime.

const badLabel = (info) => (!info || info.outdated ? 'PÉRIMÉ' : info.disabled ? 'DÉSACTIVÉ' : info.unusable ? 'INUTILISABLE'
  : info.revision_late ? 'RÉVISION EN RETARD' : info.disinfection_late ? 'DÉSINFECTION EN RETARD' : 'PÉRIMÉ');
let loginAction = null; // {action, at} : a lancer une fois connecte

function setLoginAction(action) {
  loginAction = action ? { action, at: Date.now() } : null;
}

function takeLoginAction() {
  const pending = loginAction;
  loginAction = null;
  return pending && Date.now() - pending.at < 5 * 60 * 1000 ? pending.action : null;
}

// Action reservee a un utilisateur connecte : badge (petit lecteur, ou camera du scanner) ou nom + PIN
function requireLogin(reason, action) {
  if (state.user) { action(); return; }
  loginAction = null; // nouvelle demande : la precedente, jamais menee au bout, est oubliee
  const dialog = $('#login-needed');
  $('#login-needed-text').textContent = `Connectez-vous pour ${reason}.`;
  $('#login-needed-badge').onclick = () => {
    dialog.close();
    if (inHome() || ficheOpen()) openReader('', action);
    else { pendingAfterLogin = ''; setLoginAction(action); startCamera(); toast(`Scannez votre badge pour ${reason}.`); }
  };
  $('#login-needed-name').onclick = () => {
    dialog.close();
    pendingAfterLogin = '';
    setLoginAction(action);
    openNameLogin();
  };
  $('#login-needed-cancel').onclick = () => { dialog.close(); loginAction = null; };
  if (passkeyFirst()) { // derniere connexion par passkey : elle d'abord, puis le choix carte / nom + PIN
    pendingAfterLogin = '';
    setLoginAction(action);
    tryPasskeyFirst(() => { loginAction = null; if (!dialog.open) dialog.showModal(); });
    return;
  }
  if (!dialog.open) dialog.showModal();
}

// target : {item, pack, lot, lotName, type, label}. Plus de petits (!) dans les listes de la vérif : un secouriste
// les prenait pour des cases a cocher ; seulement ce gros bouton, sur les fiches et pour le lot en cours de vérif
function reportButton(target) {
  return el('button', { type: 'button', class: 'report-btn report-wide', onclick: () => openReport(target) },
    '⚠ Signaler un problème');
}

// « Marquer ce lot comme utilisé » : tout secouriste identifie ; le lot (et ses sous-lots non scelles) passe en
// vérif recommandee pour les suivants, jusqu'a la prochaine vérif complete
function usedButton(lot) {
  return el('button', { type: 'button', class: 'used-btn', onclick: () => markUsed(lot) },
    icon('used'),
    el('span', { class: 'used-text' }, el('span', {}, 'Marquer ce lot comme utilisé'),
      el('span', { class: 'used-sub' }, '(demander une vérif)')));
}

// « Partir avec ce lot » : meme depart que la page « Départ » avec ce seul lot coche (liste de depart a son nom,
// ouverte pour scanner l'etiquette). Lot (ou lot au-dessus) deja pris par une liste : le bouton ouvre cette liste,
// « Retour de ce lot » si elle est sortie, « Continuer le départ » si elle est a preparer. Pas de bouton pour un
// rangement, un lot archive ou un sous-lot a place fixe (on part avec son lot)
function departButton(lot) {
  const departure = lot.departure || { blocked: '', list: null };
  const taken = departure.list;
  if (lot.storage || !lot.active || (departure.blocked && !taken)) return '';
  const button = (emoji, title, sub, onclick) => el('button', { type: 'button', class: 'used-btn depart-btn', onclick },
    el('span', { class: 'depart-emoji', 'aria-hidden': 'true' }, emoji),
    el('span', { class: 'used-text' }, el('span', {}, title), el('span', { class: 'used-sub' }, sub)));
  if (taken && taken.out) {
    const gone = taken.gone && taken.gone.id !== lot.id ? taken.gone.name : ''; // parti avec le lot au-dessus
    return button('↩', gone ? `Retour de ${gone}` : 'Retour de ce lot', '(le lot est en poste)',
      () => requireLogin('noter le retour de ce lot', () => returnLot(lot, gone || lot.name)));
  }
  if (taken) {
    return button('🎒', 'Continuer le départ', `(liste « ${taken.name} »)`,
      () => requireLogin('continuer le départ', () => openList(taken.id)));
  }
  return button('🎒', 'Partir avec ce lot', '(liste de départ à scanner)', () => departWithLot(lot));
}

// « Retour de ce lot » : le lot en poste est scanne rentre sur sa liste (vérif recommandee) ; tout rentre, le
// serveur clot le retour de la liste. La fiche revient au bouton « Partir avec ce lot »
async function returnLot(lot, name) {
  if (!await confirmDialog(`Retour de ${name} ?`, "Son contenu a pu servir : il passe en « vérif recommandée » jusqu'à la prochaine vérif complète.", 'Noter le retour')) return;
  try {
    const result = await api(`lots/${encodeURIComponent(lot.id)}/return/`, { user: badge() });
    feedback.good();
    toast(result.message);
    state.lots = null;
    if (ficheOpen() && fiche.kind === 'lot' && fiche.id === lot.id) openFiche('lot', lot.id, { key: fiche.key, seal: fiche.seal, push: false, label: fiche.label });
  } catch (e) {
    toast(e.message, true);
  }
}

function departWithLot(lot) {
  requireLogin('partir avec ce lot', async () => {
    if (depart.busy) return;
    depart.busy = true;
    try {
      const list = await api('checklists/departure/', { user: badge(), lots: [lot.id] });
      depart.busy = false;
      state.lots = null; // lot pris : etat des lots a recharger
      openList(list.id);
    } catch (e) {
      depart.busy = false;
      toast(e.message, true);
    }
  });
}

function markUsed(lot) {
  const run = async () => {
    if (!await confirmDialog(`Marquer ${lot.name} comme utilisé ?`, "Les suivants verront « vérif recommandée » jusqu'à la prochaine vérif complète.", 'Marquer utilisé', 'warning')) return;
    try {
      const result = await api(`lots/${encodeURIComponent(lot.id)}/used/`, identity());
      toast(result.marked.length > 1 ? `Marqués utilisés : ${result.marked.join(', ')}.` : `${lot.name} marqué utilisé : vérif recommandée.`);
      state.lots = null; // etat des lots a recharger
      if (ficheOpen() && fiche.kind === 'lot' && fiche.id === lot.id) openFiche('lot', lot.id, { key: fiche.key, seal: fiche.seal, push: false, label: fiche.label });
    } catch (e) {
      toast(e.message, true);
    }
  };
  if (hasIdentity()) run();
  else requireLogin('marquer ce lot comme utilisé', run);
}

function openReport(target) {
  requireLogin('signaler un problème', () => showReportDialog(target));
}

function showReportDialog(target) {
  const dialog = $('#problem');
  const what = target.pack ? 'le paquet' : target.item || target.type ? "l'objet" : 'le lot';
  $('#problem-target').textContent = target.label || '';
  $('#problem-question').textContent = `${what === 'le lot' ? 'Le lot est-il' : what === 'le paquet' ? 'Le paquet est-il' : "L'objet est-il"} toujours utilisable ?`;
  $('#problem-usable-yes').checked = false;
  $('#problem-usable-no').checked = false;
  $('#problem-comment').value = '';
  $('#problem-error').textContent = '';
  $('#problem-send').disabled = false;
  $('#problem-form').onsubmit = async (event) => {
    event.preventDefault();
    const yes = $('#problem-usable-yes').checked;
    if (!yes && !$('#problem-usable-no').checked) {
      $('#problem-error').textContent = `Indiquez si ${what} est toujours utilisable.`;
      return;
    }
    $('#problem-send').disabled = true;
    try {
      const body = await api('reports/', { user: credentials(), lot: target.item || target.pack ? '' : target.lot || '',
        item: target.item || '', type: target.type || '', pack: target.pack || '', comment: $('#problem-comment').value,
        usable: yes });
      dialog.close();
      feedback.info();
      toast('Signalement envoyé au TECH.');
      // son propre signalement : pas de popup quand la fiche se recharge
      lastReportAlert = { key: target.item ? 'item:' + target.item : 'pack:' + (target.pack || ''), at: Date.now() };
      reported(body.item);
      state.reports = null;
    } catch (e) {
      $('#problem-send').disabled = false;
      $('#problem-error').textContent = e.message;
      if (e.status === 403) { state.user = null; save(); render(); }
    }
  };
  $('#problem-cancel').onclick = () => dialog.close();
  if (!dialog.open) dialog.showModal();
}

// Item signale : pendant la vérif de son lot, il compte comme vu (ajoute aux scans s'il n'y est pas)
async function reported(item) {
  if (item && state.lot && item.location && sessionLots().some((lot) => lot.id === item.location)) {
    const entry = state.scanned.find((row) => row.kind === 'item' && row.iid === item.iid);
    if (entry) Object.assign(entry, { info: item, expired: item.expired });
    else if (!live.entries().some((row) => row.kind === 'item' && row.iid === item.iid)) {
      scannedAgain();
      state.lastVerif = null;
      state.scanned.push({ code: item.iid, kind: 'item', iid: item.iid, expired: item.expired, info: item, error: '',
        place: item.location });
      showInfo(item.expired ? 'bad' : 'warn', `Signalé : ${item.type_name}`,
        item.expired ? 'Inutilisable : à remplacer, compté comme vu.' : 'Toujours utilisable : compté comme vu.');
    }
    save();
    await loadLot(state.lotId, '', true); // etat a jour des items du lot (inutilisable : a remplacer)
  }
  if (ficheOpen()) openFiche(fiche.kind, fiche.id, { key: fiche.key, seal: fiche.seal, push: false, label: fiche.label });
}

// Page « Signalements » : lecture seule pour tous, gestion pour les admins
async function loadReports(quiet = false) {
  if (!state.user || state.reportsBusy) return;
  state.reportsBusy = true;
  if (!quiet) renderHomePage();
  try {
    state.reports = await api('reports/list/', { user: credentials(), show: state.reportsShow });
  } catch (e) {
    if (!quiet) toast('Signalements : ' + e.message, true);
    if (e.data && e.data.pin_required) { state.user = null; save(); }
  } finally {
    state.reportsBusy = false;
    renderHomePage();
  }
}

const reportLabel = (row) => (row.status === 'new' ? 'Nouveau'
  : row.status === 'resolved' ? 'Résolu' + (row.resolution_label ? ' · ' + row.resolution_label.toLowerCase() : '')
  : `En cours · ${row.usable ? 'utilisable' : 'inutilisable'}`);

// Ce qu'il faut retenir de l'objet : en cours, utilisable ou non ; resolu, ce que la resolution en a fait (un
// signalement resolu ne dit plus « inutilisable » : l'objet est de nouveau utilisable, ou desactive / retire)
const reportStateText = (row) => (row.status !== 'resolved'
  ? (row.usable ? '✔ Toujours utilisable' : '✘ Inutilisable : à remplacer')
  : row.resolution === 'disabled' ? '✔ Résolu : item désactivé, remplacé à la prochaine vérif'
  : row.resolution === 'removed' ? '✔ Résolu : item retiré de l\'inventaire'
  : '✔ Résolu : de nouveau utilisable');

// Zone de texte de la reponse, par signalement : gardee a travers les rechargements de la liste. Enregistree par le
// bouton choisi : reponse « en cours » (remplace la precedente) ou reponse de la resolution.
const reportDrafts = {};

async function updateReport(row, changes) {
  try {
    await api(`reports/${row.id}/`, { user: credentials(), ...changes });
    delete reportDrafts[row.id];
    toast('Signalement mis à jour.');
  } catch (e) {
    toast(e.message, true);
    if (e.data && e.data.pin_required) { state.user = null; save(); }
  }
  loadReports(true);
}

function reportCard(row, manage) {
  // paquet : son numero (dans target) reserve au TECH
  const name = row.pack && !showIds() ? `Paquet de ${row.type_name || 'items'}` : row.target;
  const target = row.item ? ficheLink('item', row.item, name) : row.lot ? ficheLink('lot', row.lot, name) : name;
  const parts = [
    el('div', { class: 'row-head' }, el('div', { class: 'name' }, target), el('span', { class: 'tag ' + row.color }, reportLabel(row))),
    el('div', { class: 'sub' }, `Signalé par ${row.by} ${fmtOn(row.created, { long: true })}` + (row.item && showIds() ? ` · ${row.item}` : '')),
    el('div', {}, reportStateText(row)),
    row.comment ? el('p', { class: 'comment' }, row.comment) : '',
    ...repliesBlock(row),
    row.updated ? el('div', { class: 'sub' }, `Mis à jour ${fmtOn(row.updated, { long: true })}` + (row.updated_by ? ` par ${row.updated_by}` : '')) : '',
  ];
  if (manage) {
    // en cours : la reponse deja donnee, a modifier ou completer avant le bouton suivant
    const note = el('textarea', { rows: 2, maxlength: 1000,
      placeholder: 'Réponse visible de tous (facultatif), enregistrée avec le bouton choisi',
      oninput: (event) => { reportDrafts[row.id] = event.target.value; } });
    note.value = reportDrafts[row.id] ?? (row.note || '');
    const progress = (usable) => updateReport(row, { status: 'progress', usable, note: note.value });
    const resolve = (resolution) => updateReport(row, { status: 'resolved', resolution_note: note.value,
      ...(resolution ? { resolution } : {}) });
    // chaque choix : son bouton et, dessous, une petite ligne grise sur ce qu'il fait
    const choice = (button, hint) => el('div', { class: 'report-choice' }, button, el('div', { class: 'hint' }, hint));
    // deux blocs bien separes : « En cours » met seulement a jour, « Résolu » clot le signalement
    const group = (title, cls, ...choices) => el('div', { class: 'report-group ' + cls },
      el('div', { class: 'report-group-title' }, title), el('div', { class: 'report-actions' }, ...choices));
    const blocks = row.status === 'resolved'
      ? [group('Signalement clos', 'reopen',
        choice(el('button', { type: 'button', onclick: () => updateReport(row, { status: 'progress' }) }, icon('undo'), 'Rouvrir'),
          'Le signalement repasse en cours, à traiter de nouveau.'))]
      : [
        group('Mettre à jour · le signalement reste ouvert', 'update',
          choice(el('button', { type: 'button', class: 'primary', onclick: () => progress(true) },
            row.status === 'progress' && row.usable ? '✔ En cours · utilisable' : 'En cours · utilisable'),
          'Pris en charge, l\'objet reste disponible (vert).'),
          choice(el('button', { type: 'button', class: 'warning', onclick: () => progress(false) },
            row.status === 'progress' && !row.usable ? '✔ En cours · inutilisable' : 'En cours · inutilisable'),
          'Pris en charge, l\'objet ne doit pas être utilisé (orange).')),
        group('Clore le signalement', 'close', ...(row.item ? [
          // item non conforme : desactive (remplace a la prochaine verif comme un perime) ou deja retire
          choice(el('button', { type: 'button', class: 'primary', onclick: () => resolve('ok') },
            icon('check'), 'Résolu · remis en service'),
          'L\'item est conforme ou réparé : il redevient utilisable.'),
          choice(el('button', { type: 'button', class: 'warning', onclick: async () => {
            if (await confirmDialog(`Désactiver ${row.item} ?`, 'Il devra être remplacé comme un périmé.', 'Désactiver', 'warning')) resolve('disabled');
          } }, 'Résolu · item désactivé'),
          'Item non conforme : compté comme périmé, remplacé à la prochaine vérif.'),
          choice(el('button', { type: 'button', class: 'danger', onclick: async () => {
            if (await confirmDialog(`Retirer ${row.item} de l'inventaire ?`, "L'item a déjà été jeté.", 'Retirer', 'danger')) resolve('removed');
          } }, 'Résolu · item retiré'),
          'L\'item a déjà été jeté : il sort de l\'inventaire.'),
        ] : [choice(el('button', { type: 'button', class: 'primary', onclick: () => resolve('') }, icon('check'), 'Résolu'),
          'Problème réglé : l\'objet redevient utilisable.')])),
      ];
    parts.push(el('div', { class: 'report-manage' }, row.status === 'resolved' ? '' : note, ...blocks));
  }
  return el('div', { class: 'report-card ' + row.color }, ...parts);
}

function renderReports() {
  const view = $('#home-reports');
  const data = state.reports;
  const choose = (show) => { state.reportsShow = show; loadReports(); };
  const parts = [
    el('div', { class: 'row-head' }, el('h3', {}, 'Signalements'),
      el('button', { type: 'button', onclick: () => loadReports() }, icon('refresh'), state.reportsBusy ? 'Chargement…' : 'Actualiser')),
    el('div', { class: 'segmented' },
      el('button', { type: 'button', class: state.reportsShow === 'open' ? 'active' : '', onclick: () => choose('open') }, 'En cours'),
      el('button', { type: 'button', class: state.reportsShow === 'all' ? 'active' : '', onclick: () => choose('all') }, 'Tous')),
  ];
  if (!data) {
    parts.push(el('p', { class: 'hint' }, state.reportsBusy ? 'Chargement…' : 'Liste non chargée.'));
    view.replaceChildren(...parts);
    return;
  }
  parts.push(el('p', { class: 'hint' }, data.can_manage
    ? 'Rouge : nouveau. Orange : en cours, objet inutilisable. Vert : en cours et toujours utilisable, ou résolu.'
    : 'Suivi en lecture seule. Rouge : nouveau, orange : en cours (inutilisable), vert : utilisable ou résolu.'));
  if (!data.reports.length) parts.push(el('p', { class: 'hint' }, state.reportsShow === 'open' ? 'Aucun signalement en cours.' : 'Aucun signalement.'));
  parts.push(...data.reports.map((row) => reportCard(row, data.can_manage)));
  view.replaceChildren(...parts);
}
