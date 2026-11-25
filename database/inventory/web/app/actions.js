/* QRProtec - front web : actions de la vérif (valider, reassort, annuler un scan, menus). */
'use strict';

// Actions

function blockers(needItems = true) {
  const missing = [];
  if (!state.lot) missing.push("scannez l'étiquette du lot");
  else if (!keyOk()) {
    const lot = keyMissing()[0];
    missing.push(`scannez l'étiquette clé ${lot.id === state.lotId ? 'du lot' : 'de ' + lot.name}, un QR code rangé à l'intérieur du lot`
      + (lot.key_location ? ` (${lot.key_location})` : ''));
  } else if (!labelOk()) {
    const lot = labelMissing();
    missing.push(`scannez l'étiquette ${labelTarget(lot)}`);
  }
  if (!hasIdentity()) missing.push(state.declaredAllowed ? 'scannez votre badge ou indiquez votre nom' : 'scannez votre badge');
  if (needItems && !scannedIids().size) missing.push('scannez au moins un item');
  return missing;
}

// Valider sans avoir scanne l'etiquette du lot : grande popup « Scannez l'etiquette de B+ », fermee d'elle-meme quand
// l'etiquette est scannee (render). false si l'etiquette n'est pas ce qui manque.
function askLabel() {
  if (!state.lot || !keyOk() || labelOk()) return false;
  const lot = labelMissing();
  feedback.warn();
  $('#label-needed-title').textContent = `Scannez l'étiquette ${labelTarget(lot)}`;
  $('#label-needed-text').textContent = "Pour valider, scannez d'abord le QR code collé sur le lot : il confirme "
    + `que vous vérifiez bien ${lot.name}.`;
  const dialog = $('#label-needed');
  if (!dialog.open) dialog.showModal();
  return true;
}
$('#label-needed-ok').addEventListener('click', () => $('#label-needed').close());

// Explication de la vérif (3 points), une fois par vérif apres la connexion, sauf « ne plus afficher ce message »
// (reglage verif_help_hidden, remis par un admin au poste). Attend que les autres popups soient fermees.
const STORAGE_HELP = 'qrprotec.verif-help';
function maybeVerifHelp() {
  if (!state.user || !state.lot || webPref('verif_help_hidden') || inHome() || ficheOpen()) return;
  // lot scelle : rien a scanner dans le lot, seulement l'etiquette du scelle (rappel montre si l'on ouvre le contenu)
  if (sealWaiting(inCascade() ? levelLot() : state.lot)) return;
  const key = state.user.matricule + ':' + state.lotId;
  let shown = '';
  try { shown = sessionStorage.getItem(STORAGE_HELP) || ''; } catch (e) { /* stockage indisponible */ }
  if (shown === key || document.querySelector('dialog[open]')) return;
  try { sessionStorage.setItem(STORAGE_HELP, key); } catch (e) { /* stockage indisponible */ }
  $('#verif-help-hide').checked = false;
  helpSlide(0);
  $('#verif-help').showModal();
}
// nouvelle vérif (vérif quittee ou enregistree, « Lancer une vérif ») : le rappel s'affiche de nouveau, meme lot compris
function resetVerifHelp() {
  try { sessionStorage.removeItem(STORAGE_HELP); } catch (e) { /* stockage indisponible */ }
}
// Pages de l'explication : une seule visible, les points du bas suivent, le bouton dit « Suivant » puis « C'est parti »
let helpPage = 0;
function helpSlide(index) {
  helpPage = index;
  const slides = [...document.querySelectorAll('#verif-help .slide')];
  slides.forEach((slide, i) => { slide.hidden = i !== index; });
  document.querySelectorAll('#verif-help .slides-dots span').forEach((dot, i) => dot.classList.toggle('on', i === index));
  $('#verif-help-ok').textContent = index < slides.length - 1 ? 'Suivant' : "C'est parti";
  $('#verif-help .slides-track').scrollTop = 0;
}
$('#verif-help-ok').addEventListener('click', async () => {
  if (helpPage < document.querySelectorAll('#verif-help .slide').length - 1) { helpSlide(helpPage + 1); return; }
  $('#verif-help').close();
  if (!$('#verif-help-hide').checked || !state.user) return;
  try {
    await setWebPrefs({ verif_help_hidden: true });
  } catch (e) { toast(e.message, true); }
});
// « Tester le son » : les deux bips de la vérif (compté, puis probleme), sans l'ecran rouge
setLabel($('#verif-help-sound'), 'volume', 'Tester le son');
$('#verif-help-sound').addEventListener('click', () => {
  tone(1320, 0, 0.08, 'sine', 0.2);
  tone(880, 0.6, 0.18);
  tone(440, 0.82, 0.3);
});

// partial : verif partielle, seuls les lots rendus complets par les scans sont verifies (les autres items
// scannes sont ajoutes a leur lot comme un reassort)
// Sans badge, si le serveur l'autorise : le nom est demande une fois et garde sur ce navigateur
async function askDeclaredName() {
  const name = ((await promptDialog({ title: 'Sans badge', text: 'Indiquez votre nom (prénom et nom).',
    value: state.declaredName, placeholder: 'Prénom Nom', maxlength: 30 })) || '').trim();
  if (!name) return false;
  state.declaredName = name.slice(0, 30);
  save(); render();
  return true;
}

// Valider une vérif : à plusieurs, sans compte, on est invite (prenom facultatif, « Invité » sinon), a condition
// qu'un secouriste connecte participe (exige par le serveur, views.team_identity)
async function ensureVerifIdentity() {
  if (state.user || !live.inTeam()) return ensureIdentity();
  if (!live.others().some((row) => row.verified)) {
    feedback.warn();
    toast('Il faut au moins un secouriste connecté pour valider une vérif à plusieurs : scannez votre badge.', true);
    return false;
  }
  if (declared() || state.guest !== null) return true;
  const name = await promptDialog({ title: "Vous n'êtes pas connecté",
    text: 'Votre prénom (facultatif) : il sera noté sur la vérif comme invité.', placeholder: 'Prénom', maxlength: 20 });
  if (name === null) return false;
  state.guest = name.trim().slice(0, 20);
  live.identity();
  render();
  return true;
}

async function ensureIdentity() {
  return hasIdentity() || !state.declaredAllowed || askDeclaredName();
}

// Le serveur a refuse l'identite : il dit si le nom declare est (encore) accepte
function identityRefused(e) {
  if (!e.data || !e.data.login_required) return;
  state.declaredAllowed = !!e.data.declared_identity;
}

// Vérif à plusieurs : chacun valide sa partie, le dernier participant connecte envoie la vérif commune.
// auto : le serveur demande l'envoi (les autres participants sont partis), sans redemander de confirmation.
async function validate(partial = false, auto = false) {
  if (state.busy) return;
  if (!auto && !await ensureVerifIdentity()) return;
  const missing = blockers(false);
  if (missing.length) {
    if (auto) live.unvalidate();
    if (!auto && askLabel()) return;
    feedback.warn(); toast('Pour valider : ' + missing.join(', ') + '.', true); return;
  }
  const team = live.inTeam();
  if (team && !auto) {
    state.busy = true;
    render();
    const answer = await live.validate(partial);
    state.busy = false;
    if (!answer) { render(); toast('Connexion à la vérif commune perdue : réessayez.', true); return; }
    if (!answer.submit) {
      state.waitingTeam = true;
      feedback.good();
      showInfo('ok', '✔ Votre partie de la vérif est validée',
        answer.waiting.length ? `En attente de ${answer.waiting.join(', ')}.` : 'En attente des autres participants.',
        'Le compte rendu s\'affichera quand tout le monde aura validé.');
      render();
      return;
    }
  }
  const lots = sessionLots();
  const plan = planSession();
  const verified = partial ? plan.lots.filter((row) => row.complete).map((row) => row.lot)
    : lots.filter((lot) => !plan.intact.has(lot.id));
  const sealed = verified.filter((lot) => lot.is_sealed).map((lot) => lot.name);
  const confirmed = async () => {
    if (auto) return true;
    if (sealed.length && !await confirmBreakSeal(sealed)) return false;
    if (partial) {
      const names = verified.filter((lot) => lot.items && (lot.requirements.length || lot.items.length)).map((lot) => lot.name);
      return confirmDialog('Vérif partielle ?', `Seuls ${names.join(', ') || 'les lots complets'} seront vérifiés. `
        + `Les autres items scannés sont ajoutés à leur lot (réassort${state.restockRecommends ? ', vérif recommandée' : ''}).`, 'Valider');
    }
    const expected = lots.reduce((sum, lot) => sum + expectedGroups(lot, plan).remaining, 0);
    const what = isMulti() ? 'les lots seront incomplets' : 'le lot sera incomplet';
    return !expected || confirmDialog('Valider quand même ?',
      `${nb(expected, 'item attendu', 'items attendus')} ${pl(expected, 'manque', 'manquent')} : ${what}.`, 'Valider incomplet', 'warning');
  };
  if (!await confirmed()) { if (team || auto) live.unvalidate(); return; }
  state.busy = true;
  render();
  try {
    const credentials = live.credentials();
    const name = state.lot.name;
    const report = await sendVerif({
      lots: sessionEntries(),
      labels: [...labeledIds()],
      ...identity(),
      items: [...scannedIids()],
      places: scanPlaces(),
      // etiquettes de scelle scannees : lots scelles intacts, comptes comme verifies
      sealed: allScanned().filter((entry) => entry.kind === 'seal' && !entry.error).map((entry) => ({ id: entry.iid, code: entry.key })),
      checked: checkedMap(allScanned()),
      ...scanTiming(allScanned()),
      partial,
      full: state.full,
      ...(credentials ? { live: credentials } : {}),
    }, name);
    state.waitingTeam = false;
    state.scanned = [];
    if (report) showReport(report); else feedback.good();
    live.done(report);
    forgetLot(report ? 'Vérif enregistrée. Scannez un item ou l\'étiquette d\'un lot pour en commencer une autre.'
      : `✔ Vérif de ${name} gardée sur ce téléphone (hors connexion) : elle sera envoyée dès le retour du réseau.`);
    loadLots();
    backToList();
  } catch (e) {
    identityRefused(e);
    if (team || auto) live.unvalidate();
    state.waitingTeam = false;
    feedback.bad();
    toast('Vérif refusée : ' + e.message, true);
  } finally {
    state.busy = false;
    save();
    render();
  }
}

// Sans lot scanne : les items scannes sont sortis du stock (ex : pris pour une intervention). Ils redeviennent
// normaux s'ils reapparaissent a une verif, sinon ils comptent comme utilises au bout d'un mois (ou a peremption).
// Vérif en cascade : scans compris dans la validation des lots scope (lieu du scan : le lot ou l'on etait entre)
const scopeEntries = (scope) => allScanned().filter((entry) => !entry.error && scope.has(entry.place || state.lotId));

function entriesPayload(entries) {
  const items = new Set();
  const places = {};
  const sealed = [];
  for (const entry of entries) {
    if (entry.kind === 'seal') { sealed.push({ id: entry.iid, code: entry.key }); continue; }
    if (entry.kind === 'check') continue;
    for (const iid of entry.kind === 'item' ? [entry.iid] : entry.items || []) {
      items.add(iid);
      if (entry.place) places[iid] = entry.place;
    }
  }
  return { items: [...items], places, sealed, checked: checkedMap(entries), ...scanTiming(entries) };
}

// Heures de l'appareil envoyees avec la vérif : debut ("started_at", etiquette du lot ou fin de l'etape precedente) et
// heure du scan de chaque item ("scanned_at" : {iid: heure}), recalees par le serveur comme done_at (duree de la vérif
// et dernier scan de chaque item dans le rapport d'etat). Scans d'un autre participant : sans heure ici.
function scanTiming(entries) {
  const scanned = {};
  for (const entry of entries) {
    if (!entry.at || entry.error || entry.kind === 'seal' || entry.kind === 'check') continue;
    for (const iid of entry.kind === 'item' ? [entry.iid] : entry.items || []) scanned[iid] = entry.at;
  }
  const first = Object.values(scanned).sort()[0] || '';
  const started = state.startedAt && (!first || state.startedAt < first) ? state.startedAt : first;
  return started ? { started_at: started, scanned_at: scanned } : { scanned_at: scanned };
}

// Lots valides par une validation de la vérif en cascade (compte rendu du serveur) : notes, et leurs scans retires
function stepDone(report, sent, by = '') {
  const at = new Date().toISOString();
  for (const row of report.lots || []) {
    if (row.verified || row.sealed) state.done[row.id] = { at, by, complete: !!row.complete, sealed: !!row.sealed,
      verif: row.verif_id || report.verif_id || null }; // compte rendu commun a la fin de la cascade
  }
  const gone = new Set(sent);
  state.scanned = state.scanned.filter((entry) => !(entry.kind === 'pack' ? (entry.items || []).some((iid) => gone.has(iid))
    : entry.kind === 'check' ? !!state.done[entry.iid] : gone.has(entry.iid)));
}

// Valider le lot en cours de la vérif en cascade (partial : vérif partielle ou reassort de ce lot). Il est enregistre
// tout de suite, avec ses sous-lots commences ; on revient au lot qui le contient. Au lot global, la vérif se termine.
// Vérif lancee sur un kit a scelle obligatoire du lot global, et l'on y est : sa validation termine la vérif
const sealNextLevel = () => inCascade() && !!state.startedIn && levelId() === state.startedIn
  && !!(lotById(state.startedIn) || {}).seal_required;

// Vérif en cascade : le lot ou l'on est (pas le lot global) est complet, rien d'attendu n'y manque, et peut etre
// valide sans question
function levelComplete() {
  if (!inCascade() || state.busy || levelId() === state.lotId) return false;
  const top = levelLot();
  const scope = scopeOf(top.id);
  if (!scopeEntries(scope).length && ownContent(top)) return false; // rien scanne ici
  if (blockers(false).length) return false;
  const plan = planSession();
  const lots = sessionLots().filter((lot) => scope.has(lot.id) && !plan.intact.has(lot.id));
  return !lots.some((lot) => lot.is_sealed) && lots.reduce((sum, lot) => sum + expectedGroups(lot, plan).remaining, 0) === 0;
}

// Scan d'un item ou d'une etiquette du lot `id`, hors du lot ou l'on est : les lots complets qu'on quitte ainsi
// sont valides automatiquement (on remonte tant que `id` n'est pas dedans). Renvoie les noms des lots valides.
// ask : un lot incomplet ou l'on a commence a scanner (et ou personne d'autre n'est) fait demander quoi faire
// (leaveLevelPopup). Renvoie {names, stay} : stay, on reste dans le lot (choix « Rester »).
async function leaveCompleteLevels(id, ask = false) {
  const names = [];
  while (!isUnder(id, levelId()) && levelId() !== state.lotId) {
    const before = levelLot();
    const complete = levelComplete();
    if (!complete) {
      const scope = scopeOf(before.id);
      if (!ask || !scopeEntries(scope).length || othersLevels()[before.id]) break;
      const choice = await leaveLevelPopup(before, lotById(id), scope);
      if (choice === 'stay') return { names, stay: true };
      if (choice !== 'validate') break;
      await validateStep(false, true);
    } else {
      await validateStep(false);
    }
    if (levelId() === before.id) break; // refusee
    names.push(`${before.name} ${complete ? 'complet' : 'incomplet'}`);
  }
  return { names, stay: false };
}

// Popup « Passer a … ? » : valider le lot incomplet, passer sans le valider, ou y rester. Ignoree, le scan suivant
// vaut « Passer » (handleCode).
let leavePopup = null; // resolve de la popup ouverte
function leaveLevelPopup(level, target, scope) {
  const plan = planSession();
  const missing = sessionLots().filter((lot) => scope.has(lot.id) && !plan.intact.has(lot.id))
    .reduce((sum, lot) => sum + expectedGroups(lot, plan).remaining, 0);
  const dialog = $('#leave-level');
  $('#leave-level-title').textContent = `Passer à ${target ? target.name : 'un autre lot'} ?`;
  $('#leave-level-text').textContent = `${level.name} est incomplet : ${nb(missing, 'item attendu', 'items attendus')} pas encore ${pl(missing, 'scanné')}.`;
  $('#leave-level-validate').textContent = `Valider ${level.name} incomplet (les non scannés passent manquants)`;
  $('#leave-level-skip').textContent = `Passer sans valider (${level.name} reste à faire, scans gardés)`;
  $('#leave-level-stay').textContent = `Rester dans ${level.name}`;
  feedback.warn();
  return new Promise((resolve) => {
    if (leavePopup) leavePopup('skip');
    const close = (choice) => {
      if (leavePopup !== close) return;
      leavePopup = null;
      dialog.onclose = null;
      if (dialog.open) dialog.close();
      resolve(choice);
    };
    leavePopup = close;
    $('#leave-level-validate').onclick = () => close('validate');
    $('#leave-level-skip').onclick = () => close('skip');
    $('#leave-level-stay').onclick = () => close('stay');
    dialog.onclose = () => close('stay'); // Echap
    dialog.showModal();
  });
}

// Lot scelle dans une vérif : la valider brise le scelle
function confirmBreakSeal(names) {
  return confirmDialog('Briser le scellé ?', `${names.join(', ')} : scellé. Valider une vérif brisera le scellé.`, 'Valider et briser le scellé', 'warning');
}

// confirmed : lot incomplet deja confirme (popup « Passer a … »)
async function validateStep(partial = false, confirmed = false) {
  if (state.busy) return;
  if (!await ensureVerifIdentity()) return;
  const top = levelLot();
  const atRoot = top.id === state.lotId;
  const exclude = excludedFor(top.id);
  const scope = scopeOf(top.id, exclude);
  const entries = scopeEntries(scope);
  const pendingSubs = [...scope].some((id) => id !== top.id);
  if (!ownContent(top) && !top.storage && !entries.length && !pendingSubs) {
    // lot de regroupement sans rien a enregistrer : ses sous-lots sont deja faits (ou laisses tels quels)
    if (atRoot) finishCascade(); else { toast(`${top.name} : rien d'autre à valider.`); goUp(); }
    return;
  }
  const missing = blockers(false);
  if (missing.length) {
    if (askLabel()) return;
    feedback.warn(); toast('Pour valider : ' + missing.join(', ') + '.', true); return;
  }
  const plan = planSession();
  const lots = sessionLots().filter((lot) => scope.has(lot.id) && !plan.intact.has(lot.id));
  const verified = partial ? plan.lots.filter((row) => row.complete && scope.has(row.lot.id)).map((row) => row.lot) : lots;
  const sealedNames = verified.filter((lot) => lot.is_sealed).map((lot) => lot.name);
  if (sealedNames.length && !await confirmBreakSeal(sealedNames)) return;
  if (partial) {
    const names = verified.filter(ownContent).map((lot) => lot.name);
    if (!await (names.length ? confirmDialog('Vérif partielle ?', `Seuls ${names.join(', ')} seront vérifiés. `
      + `Les autres items scannés sont ajoutés à leur lot (réassort${state.restockRecommends ? ', vérif recommandée' : ''}).`, 'Valider')
      : top.storage ? confirmDialog(`Ranger dans ${top.name} ?`, `Les items scannés sont rangés dans ${top.name} et la vérif se termine.`, 'Ranger')
      : confirmDialog(`Réassort de ${top.name} ?`, 'Les items scannés sont ajoutés au lot sans vérif complète. ' + restockNote(top), 'Ajouter au lot'))) return;
  } else {
    const expected = lots.reduce((sum, lot) => sum + expectedGroups(lot, plan).remaining, 0);
    const subs = lots.filter((lot) => lot.id !== top.id && ownContent(lot)).map((lot) => lot.name);
    if (expected && !confirmed && !await confirmDialog('Valider quand même ?', `${nb(expected, 'item attendu', 'items attendus')} ${pl(expected, 'manque', 'manquent')} : ${top.name} sera incomplet`
      + (subs.length ? ` (avec ${subs.join(', ')})` : '') + '.', 'Valider incomplet', 'warning')) return;
  }
  const payload = entriesPayload(entries);
  state.busy = true;
  render();
  try {
    const credentials = live.credentials();
    const report = (await sendVerif({
      lots: [{ id: top.id, key: top.id === state.lotId ? state.lotKey : state.keys[top.id] || '' }],
      keys: sessionEntries().filter((entry) => entry.key && entry.id !== top.id),
      labels: [...labeledIds()],
      exclude,
      ...identity(),
      ...payload,
      partial,
      full: state.full,
      step: true,
      finish: atRoot && !partial, // lot global valide : on quitte la vérif commune cote serveur aussi
      ...(credentials ? { live: credentials } : {}),
    }, top.name)) || offlineReport(verified, plan, payload.sealed);
    stepDone(report, [...payload.items, ...payload.sealed.map((row) => row.id)]);
    state.startedAt = new Date().toISOString(); // l'etape suivante de la vérif en cascade commence
    state.scanned = state.scanned.filter((entry) => !(entry.kind === 'seal' && payload.sealed.some((row) => row.id === entry.iid)));
    const done = (report.lots || []).filter((row) => row.verified);
    const incomplete = done.filter((row) => !row.complete);
    // rangement du stock : y ranger des items est l'usage normal, « Ajouter » clot la vérif comme une vérif complete
    // du rangement (celui dont on a lance la vérif ; dans une tournee du stock, retour au lot qui le contient)
    const stored = partial && top.storage && !done.some((row) => row.id === top.id);
    const finished = !partial || stored || done.some((row) => row.id === top.id);
    (incomplete.length ? feedback.warn : feedback.good)();
    showInfo(incomplete.length ? 'warn' : 'ok',
      stored ? `✔ ${nb((report.restocked || []).length, 'item rangé', 'items rangés')} dans ${top.name}`
        : `${finished ? '✔' : '↷'} ${top.name} ${finished ? 'validé' : done.length ? ': vérif partielle enregistrée' : ': réassort enregistré'}`
        + (incomplete.length ? ', incomplet' : ''),
      done.length > 1 ? `Lots vérifiés : ${done.map((row) => row.name).join(', ')}` : '',
      report.missing && report.missing.length ? `${nb(report.missing.length, 'item manquant', 'items manquants')}` : '',
      atRoot && finished ? '' : finished ? `Retour à ${(lotById(parentIdOf(top)) || state.lot).name}.` : '',
      report.offline ? '📶 Hors connexion : gardé sur ce téléphone, envoyé dès le retour du réseau.' : '');
    if (!report.offline && (incomplete.length || (report.unknown || []).length)) showReport(report);
    // kit a scelle obligatoire dont on a lance la vérif (scanne dans le VPS) : valide complet, la vérif s'arrete la,
    // sur le compte rendu ; le lot est a resceller (on n'a vu que l'etiquette exterieure des autres lots)
    const sealNext = !atRoot && finished && sealNextLevel() && done.some((row) => row.id === top.id && row.complete);
    if (sealNext) {
      if (!report.offline) showReport(report);
      finishCascade(report, `${top.name} validé complet : à resceller (le TECH le scelle depuis le poste).`);
    } else if (stored && (atRoot || top.id === state.startedIn)) {
      finishCascade(report, `${nb((report.restocked || []).length, 'item rangé', 'items rangés')} dans ${top.name} : vérif terminée.`);
    } else if (atRoot && finished) {
      finishCascade(report);
    } else {
      if (finished && !atRoot) { state.path.pop(); state.current = levelId(); }
      await loadLot(state.lotId, '', true); // contenu et etat a jour des lots
    }
    loadLots();
  } catch (e) {
    identityRefused(e);
    feedback.bad();
    toast('Vérif refusée : ' + e.message, true);
  } finally {
    state.busy = false;
    save();
    render();
  }
}

// Compte rendu local d'une etape mise en attente (hors connexion) : les lots valides d'apres le plan de la vérif,
// complets ou non, et les lots scelles intacts. Le vrai compte rendu viendra du serveur a l'envoi.
function offlineReport(lots, plan, sealed = []) {
  const rows = lots.map((lot) => ({ id: lot.id, name: lot.name, verified: true,
    complete: !!plan && expectedGroups(lot, plan).remaining === 0 }));
  for (const row of sealed) rows.push({ id: row.id, name: (lotById(row.id) || {}).name || '', sealed: true });
  return { offline: true, lots: rows, missing: [], restocked: [], unknown: [] };
}

// Etiquettes de scelle scannees dans la vérif en cascade : le lot scelle intact est enregistre tout de suite (s'il
// manque l'etiquette cle ou l'identite, il le sera a la validation du lot ou il a ete scanne)
let flushing = false;
async function flushSeals() {
  if (!inCascade() || flushing || state.busy || !keyOk() || !hasIdentity()) return;
  const seals = state.scanned.filter((entry) => entry.kind === 'seal' && !entry.error && lotById(entry.iid));
  if (!seals.length) return;
  flushing = true;
  try {
    for (const entry of seals) {
      const credentials = live.credentials();
      const report = (await sendVerif({
        lots: [{ id: entry.iid, key: state.keys[entry.iid] || '' }],
        keys: sessionEntries().filter((row) => row.key && row.id !== entry.iid),
        ...identity(),
        items: [],
        sealed: [{ id: entry.iid, code: entry.key }],
        step: true,
        ...(credentials ? { live: credentials } : {}),
      }, (lotById(entry.iid) || { name: 'lot scellé' }).name)) || offlineReport([], null, [{ id: entry.iid }]);
      stepDone(report, []);
      state.scanned = state.scanned.filter((other) => other !== entry);
    }
  } catch (e) {
    toast('Scellé non enregistré : ' + e.message, true);
  } finally {
    flushing = false;
    save();
    render();
  }
}

// Fin de la vérif en cascade (lot global valide, ou plus rien a faire) : on quitte la vérif commune, les autres
// participants continuent la leur
function finishCascade(report = null, message = '') {
  const done = Object.values(state.done).length;
  // compte rendu de toute la vérif (chaque etape validee, par soi ou un autre participant), a chaque fois : sans lui,
  // une cascade toute complete ne montrait rien (seule une etape incomplete ouvrait le sien)
  const verifs = [...new Set(Object.values(state.done).map((row) => row.verif).filter(Boolean))];
  const name = state.lot ? state.lot.name : '';
  const left = state.scanned.length;
  live.quit();
  state.scanned = [];
  resetCascade();
  forgetLot(message || `Vérif de ${name} terminée : ${nb(done, 'lot validé', 'lots validés')}` + (left ? `, ${nb(left, 'scan non validé oublié', 'scans non validés oubliés')}.` : '.'));
  if (!report) feedback.good();
  backToList();
  if (verifs.length && !(report && report.offline)) {
    if ($('#report').open) $('#report').close(); // celui de la derniere etape est dans le compte rendu commun
    openVerifReport(verifs, true);
  }
}

// Vérif lancee depuis une liste de depart (« Lancer la vérif ») : une fois enregistree (ou quittee), retour a la
// liste pour finir le depart ; le compte rendu eventuel reste ouvert par-dessus
function backToList(saved = true) {
  const back = lists.verifBack;
  if (!back) return false;
  openList(back.id);
  if (saved) toast(`Vérif enregistrée : retour à la liste « ${back.name} ».`);
  return true;
}

async function takeOut() {
  if (!await ensureIdentity()) return;
  const iids = [...scannedIids()];
  const missing = [];
  if (!hasIdentity()) missing.push(state.declaredAllowed ? 'scannez votre badge ou indiquez votre nom' : 'scannez votre badge');
  if (!iids.length) missing.push('scannez au moins un item');
  if (missing.length) { feedback.warn(); toast('Pour sortir : ' + missing.join(', ') + '.', true); return; }
  if (!await confirmDialog(`Sortir ${nb(iids.length, 'item')} du stock ?`, 'Sans lot. '
    + "Ils redeviennent normaux s'ils sont scannés à une vérif ; sinon ils seront comptés comme utilisés "
    + 'au bout d\'un mois (ou à leur péremption).', 'Sortir du stock')) return;
  state.busy = true;
  render();
  try {
    const result = await api('items/out/', { ...identity(), items: iids });
    state.scanned = [];
    feedback.good();
    showInfo('ok', `${nb(result.out.length, 'item sorti', 'items sortis')} du stock`,
      `Scannez-les à une vérif dans les ${result.days} jours, sinon ils seront comptés comme utilisés.`,
      ...(result.refused || []).map((row) => showIds() ? `Non sorti : ${row.iid} (${row.reason})` : `Non sorti : ${row.reason}`));
    if (canSeeStock()) loadStock();
  } catch (e) {
    identityRefused(e);
    feedback.bad();
    toast('Sortie refusée : ' + e.message, true);
  } finally {
    state.busy = false;
    save();
    render();
  }
}

// Seulement des items qui ne sont pas dans les lots de la verif : reassort plutot que verif
function onlyNewItems() {
  if (!state.lot || !allScanned().length) return false;
  const known = knownIids();
  const iids = [...scannedIids()];
  return iids.length > 0 && !iids.some((iid) => known.has(iid));
}

// {iid: lot de la derniere etiquette scannee avant lui} envoye avec la vérif (services.assign_items)
function scanPlaces() {
  const places = {};
  for (const entry of allScanned()) {
    if (entry.error || !entry.place) continue;
    for (const iid of entry.kind === 'item' ? [entry.iid] : entry.items || []) places[iid] = entry.place;
  }
  return places;
}

function knownIids() {
  return new Set(sessionLots().flatMap((lot) => [...(lot.items || []), ...(lot.missing_items || [])].map((item) => item.iid)));
}

// Verif partielle : des lots sont complets, mais pas tous (verif groupee seulement)
function partialLots() {
  if (!isMulti() || !allScanned().length) return [];
  const plan = planSession();
  if (plan.lots.every((row) => row.complete)) return [];
  return plan.lots.filter((row) => row.complete && row.touched > 0).map((row) => row.lot);
}

async function addToLot() {
  if (!await ensureIdentity()) return;
  const missing = blockers(true);
  if (missing.length) { feedback.warn(); toast('Pour ajouter : ' + missing.join(', ') + '.', true); return; }
  const credit = !!state.lot.restock_credit; // credit de confiance : le reassort laisse le lot au vert
  try {
    const result = await api(`lots/${encodeURIComponent(state.lot.id)}/add/`, {
      key: state.lotKey,
      labels: [...labeledIds()],
      ...identity(),
      items: [...scannedIids()],
    });
    state.scanned = [];
    feedback.good();
    // items laisses de cote par le serveur (perime, supprime, dans un autre lot sans badge...)
    const refused = (result.refused || []).map((row) => `${iidLabel(row.iid)} (${row.reason})`);
    toast(`${nb(result.moved.length, 'item ajouté', 'items ajoutés')} au lot${credit ? ' : lot réassorti, toujours vérifié' : ' : vérif complète recommandée'}.`
      + (refused.length ? ` Non ${pl(refused.length, 'ajouté', 'ajoutés')} : ${refused.join(', ')}.` : ''),
      refused.length > 0);
    await loadLot(state.lot.id, '', true);
    loadLots();
  } catch (e) {
    identityRefused(e);
    feedback.bad();
    toast('Ajout refusé : ' + e.message, true);
  }
  save();
  render();
}

function iidLabel(iid) {
  const known = sessionLots().flatMap((lot) => lot.items || []).find((item) => item.iid === iid);
  const entry = allScanned().find((e) => e.iid === iid && e.info);
  const info = (known) || (entry && entry.info);
  return info ? `${info.type_name} – ${info.peremption ? fmtDate(info.peremption) : 'non périssable'}` : idText(iid) || 'Code non reconnu';
}

function showReport(report) {
  const section = (title, list) => list && list.length
    ? [el('h3', {}, `${title} (${list.length})`), el('ul', {}, list.map((iid) => el('li', {}, iidLabel(iid))))]
    : [];
  const body = $('#report-body');
  body.replaceChildren(
    el('h2', { style: `color:var(--${report.complete ? 'green' : report.partial ? 'orange' : 'red'})` },
      report.complete ? ((report.lots || []).length > 1 ? '✅ Lots complets' : '✅ Lot complet')
        : report.partial ? '↷ Vérif partielle enregistrée' : '⚠️ Lot NON complet'),
    !report.present.length && (report.sealed_lots || []).length ? '' : el('p', {}, `${nb(report.present.length, 'item présent', 'items présents')}.`),
    (report.participants || []).length > 1 ? el('p', {}, `👥 Vérif à plusieurs : ${report.participants.join(', ')}.`) : '',
    (report.sealed_lots || []).length ? el('p', {}, `🔒 Scellé intact, compté comme vérifié : ${report.sealed_lots.join(', ')}.`) : '',
    report.unsealed ? el('p', { style: 'color:var(--orange)' },
      `🔓 Scellé brisé par cette vérif : ${(report.unsealed_lots || []).join(', ') || 'lot'}.`) : '',
    ...((report.lots || []).length > 1 ? [el('h3', {}, report.partial ? 'Vérif partielle' : 'Lots vérifiés'),
      el('ul', {}, report.lots.map((row) => el('li', {}, '  '.repeat(row.depth || 0)
        + (row.sealed ? `🔒 ${row.name} : scellé intact`
          : row.verified ? (row.complete ? '✔ ' : '✘ ') + row.name + (row.complete ? ' : complet' : ' : incomplet')
          : `↷ ${row.name} : non vérifié` + (row.restocked ? `, ${nb(row.restocked, 'item ajouté', 'items ajoutés')} (réassort)` : '')))))] : []),
    ...(report.requirements || []).map((row) => requirementLine(
      (row.lot_name ? row.lot_name + ' · ' : '') + reqName(row), row.present, row)),
    ...section('Périmés ou inutilisables encore dans le lot : à remplacer', report.expired),
    ...section('Périmés retirés du lot : à jeter', report.replaced),
    // etiquette a dechirer absente : l'ensemble a ete entame, il compte comme utilise
    ...section('Étiquette déchirée : utilisés', report.torn),
    ...section('Attendus mais non scannés', report.missing.filter((iid) => !(report.torn || []).includes(iid))),
    // non consommable absent du lot : retire du lot, hors lot (pas perdu) jusqu'a son retour en stock
    ...section('Non consommables absents : hors lot (à remettre en stock)', report.loose),
    ...section('Retrouvés', report.reactivated),
    ...section('Ajoutés en réassort', report.restocked),
    ...section('Codes inconnus ignorés', report.unknown),
  );
  if (report.complete) feedback.good(); else feedback.warn();
  $('#report').showModal();
}

// Plus de lot selectionne (« Changer de lot », ou vérif enregistree : le lot n'est plus « en cours »)
function forgetLot(message = "Scannez l'étiquette d'un lot.") {
  state.lot = null; state.lotId = ''; state.lotKey = ''; state.extra = []; resetCascade();
  state.lastVerif = null; state.launched = ''; state.current = ''; state.askPlace = false; state.full = false;
  state.startedAt = '';
  resetVerifHelp();
  showInfo('empty', message);
  save(); render();
}

$('#report-close').addEventListener('click', () => $('#report').close());
$('#validate').addEventListener('click', () => (takingOut() ? takeOut() : inCascade() ? validateStep(false) : validate(false)));
// Suite de la question du reassort : le lot passe en verif recommandee (reglage du serveur, coche par defaut), sauf
// sous le credit de confiance (reglage decoche : N sorties / X jours depuis la derniere verif complete)
const restockNote = (lot) => (!lot || !lot.restock_credit ? 'Le lot sera signalé « vérif recommandée » pour la personne suivante.'
  : `Le lot reste vérifié (crédit de confiance : ${nb(lot.restock_credit.outings_left, 'sortie')} sans vérif encore possible, `
    + `jusqu'au ${fmtDate(lot.restock_credit.until)}).`);

$('#restock').addEventListener('click', async () => {
  if (inCascade()) { validateStep(true); return; }
  if (partialLots().length) { validate(true); return; }
  if (!await confirmDialog('Réassort ?', 'Les items scannés sont ajoutés sans vérif complète. ' + restockNote(state.lot), 'Ajouter au lot')) return;
  if (isMulti()) validate(true); else addToLot();
});
// Menu ⋯ de la vérif : une icone par action
for (const [action, name] of [['add', 'restock'], ['manual', 'keypad'], ['quit-verif', 'logout'], ['close', 'close']]) {
  const button = document.querySelector(`#menu [data-action="${action}"]`);
  if (button) button.prepend(icon(name));
}
$('#more').addEventListener('click', () => { menuAddLabel(); $('#menu').showModal(); });
// « Ajouter les items scannés au lot » ne parlait a personne : c'est un reassort, les items scannes vont dans le lot
// sans faire toute la vérif
function menuAddLabel() {
  const button = document.querySelector('#menu [data-action="add"]');
  button.hidden = !state.lot;
  if (!state.lot) return;
  const name = state.lot.name_short || state.lot.name;
  button.replaceChildren(icon('restock'), el('span', { class: 'menu-text' },
    el('span', {}, `Réassort : ajouter mes scans à ${name}`),
    el('small', {}, 'Sans faire toute la vérif. ' + (state.lot.restock_credit ? 'Le lot reste vérifié.' : 'Une vérif complète sera recommandée.'))));
}
$('#menu').addEventListener('click', async (event) => {
  const action = event.target.dataset && event.target.dataset.action;
  if (!action) return;
  $('#menu').close();
  if (action === 'add') addToLot();
  if (action === 'manual') {
    const code = await promptDialog({ title: 'Saisir un code à la main',
      text: showIds() ? "Code de l'item, id du lot ou URL d'une étiquette." : "Code imprimé sur l'étiquette.", ok: 'Envoyer' });
    if (code && code.trim()) {
      const value = code.trim();
      // un identifiant de lot seul (sans URL) est accepte pour ouvrir un lot sans etiquette
      if (!expandIid(value) && !readIid(value).foreign && !/^https?:/.test(value)) {
        const lot = readId(value, LOT_ID_LENGTH);  // id imprime, avec ou sans le code de l'antenne
        if (lot.foreign) showForeign(lot.foreign);
        else scanLot({ kind: 'lot', code: '', id: lot.id, key: '' });
      } else {
        handleCode(value);
      }
    }
  }
  if (action === 'quit-verif') quitVerif();
});

// « Quitter cette vérif » (menu ⋯) : rien n'est enregistre, retour a l'accueil. Les scans sont oublies (une vérif
// commune continue sans nous).
async function quitVerif() {
  const count = state.scanned.length;
  if (count && !await confirmDialog('Quitter cette vérif ?', `${nb(count, 'scan')} ${pl(count, 'ne sera pas enregistré', 'ne seront pas enregistrés')}.`, 'Quitter', 'danger')) return;
  state.waitingTeam = false;
  if (live.inTeam()) live.leave();
  state.scanned = [];
  forgetLot();
  // vérif lancee depuis une liste de depart : retour a la liste
  if (!backToList(false)) showHome('menu');
}

$('#user-chip').addEventListener('click', async () => {
  if (state.user) {
    if (await confirmDialog(`Déconnecter ${state.user.prenom} ?`, '', 'Se déconnecter')) logout();
  } else if (declared()) {
    if (await confirmDialog(`Oublier le nom « ${state.declaredName} » ?`, 'Scannez votre badge pour vous identifier.', 'Oublier')) {
      state.declaredName = '';
      save(); render();
    }
  } else if (!state.user) {
    // scannez votre badge, ou sans badge : nom et PIN (ou nom seul si le serveur l'autorise)
    openNameLogin();
  }
});

// Nouveau scan (ou scan retire) apres avoir valide sa partie : la partie n'est plus validee
function scannedAgain() {
  if (!state.waitingTeam) return;
  state.waitingTeam = false;
  live.unvalidate();
}

function undoLastScan() {
  if (state.scanned.length) scannedAgain();
  const last = state.scanned.pop();
  if (last) {
    const name = last.kind === 'pack' && last.info ? `paquet ${(last.items || []).length || last.info.count} × ${last.info.type_name}`
      : last.info && last.info.type_name ? withId(last.info.type_name, last.iid) : idText(last.iid) || 'code inconnu';
    toast('Dernier scan annulé : ' + name);
  }
  save(); render();
}
$('#undo').addEventListener('click', undoLastScan);
$('#undo-last').addEventListener('click', undoLastScan);

$('#clear').addEventListener('click', async () => {
  if (!state.scanned.length || !await confirmDialog('Vider la liste des items scannés ?', '', 'Vider', 'danger')) return;
  scannedAgain();
  state.scanned = [];
  save(); render();
});

function switchTab(tab) {
  state.tab = tab;
  document.querySelectorAll('#tabs button').forEach((b) => b.classList.toggle('active', b.dataset.tab === tab));
  document.querySelectorAll('.view').forEach((v) => v.classList.toggle('active', v.dataset.view === tab));
}
document.querySelectorAll('#tabs button').forEach((b) => b.addEventListener('click', () => switchTab(b.dataset.tab)));
