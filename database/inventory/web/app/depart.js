/* QRProtec - front web : page Depart (listes attribuees, lots a prendre). */
'use strict';

// « Départ » (pour un poste de secours) : onglet 1, les listes de matériel attribuées au secouriste connecté et
// pas encore rentrées ; onglet 2, les lots avec une recherche. Onglet 2 par défaut sans liste attribuée.

const depart = {
  tab: '',        // 'lists' | 'lots' ; '' : choisi a la reception des listes
  lists: null,    // listes attribuees (null : pas encore lues)
  busy: false,
  query: '',
  input: null,    // champ de recherche garde d'un rendu a l'autre (le clavier du telephone reste ouvert)
  selected: new Set(), // lots coches pour partir (gardes jusqu'au depart, meme apres une connexion)
  blocked: {},    // lots qu'on ne peut pas cocher : {id: raison} (pris par une liste, place fixe dans leur lot)
};

function departShown() {
  depart.tab = state.user ? '' : 'lots';
  if (state.loading !== 'lots') loadLots(); // etat des lots a jour a chaque ouverture de la page
  if (state.user) { loadDepartLists(); loadDepartBlocked(); }
}

async function loadDepartBlocked() {
  try {
    depart.blocked = (await api('checklists/departure/', { user: badge(), blocked: true })).blocked || {};
    Object.keys(depart.blocked).forEach((id) => depart.selected.delete(id));
  } catch (e) {
    depart.blocked = {};
  }
  renderDepartPage();
}

// lot coche, ou qui part avec un lot coche au-dessus de lui (sous-lot)
function departParentPicked(lot) {
  const byId = new Map((state.lots || []).map((row) => [row.id, row]));
  for (let parent = byId.get(lot.parent); parent; parent = byId.get(parent.parent)) {
    if (depart.selected.has(parent.id)) return parent;
  }
  return null;
}

// lots coches sans ceux qui partent deja avec un lot coche au-dessus d'eux
function departCount() {
  return (state.lots || []).filter((lot) => depart.selected.has(lot.id) && !departParentPicked(lot)).length;
}

function departToggle(lot) {
  // sac d'un B+ de referentiel (sans etiquette) : on coche le B+ entier ; un sac se retire ensuite par sa croix dans
  // la liste
  const up = (state.lots || []).find((row) => row.id === lot.parent);
  if (up && up.virtual) {
    if (!depart.selected.has(up.id) && !depart.blocked[up.id] && !departParentPicked(up)) toast(`${lot.name} fait partie de ${up.name} : ${up.name} entier est coché.`);
    departToggle(up);
    return;
  }
  const reason = depart.blocked[lot.id];
  const parent = departParentPicked(lot);
  if (reason) { toast(`${lot.name} : ${reason.charAt(0).toLowerCase() + reason.slice(1)}.`, true); return; }
  if (parent) { toast(`${lot.name} part déjà avec ${parent.name}.`); return; }
  if (depart.selected.has(lot.id)) depart.selected.delete(lot.id);
  else depart.selected.add(lot.id);
  renderDepartPage(true);
}

// « Partir avec N lots » : liste de depart a son nom, puis la liste ouverte pour scanner l'etiquette de chaque lot
async function departGo() {
  if (!state.user) {
    openReader('depart');
    toast('Scannez votre badge pour partir avec ces lots.');
    return;
  }
  if (depart.busy) return;
  depart.busy = true;
  try {
    const list = await api('checklists/departure/', { user: badge(), lots: [...depart.selected] });
    depart.selected.clear();
    depart.busy = false;
    openList(list.id);
  } catch (e) {
    depart.busy = false;
    toast(e.message, true);
    loadDepartBlocked();
  }
}

async function loadDepartLists() {
  depart.busy = true;
  renderDepartPage();
  try {
    const data = await api('checklists/list/', { user: badge(), show: 'assigned' });
    // a preparer d'abord (c'est pour partir), puis les listes sorties dont le retour reste a faire
    depart.lists = data.lists.slice().sort((a, b) => (a.status === 'draft' ? 0 : 1) - (b.status === 'draft' ? 0 : 1));
  } catch (e) {
    depart.lists = depart.lists || [];
    toast('Listes : ' + e.message, true);
  } finally {
    depart.busy = false;
    if (!depart.tab) depart.tab = depart.lists && depart.lists.length ? 'lists' : 'lots';
    renderDepartPage();
  }
}

function departListsTab() {
  if (!state.user) {
    return [
      el('p', { class: 'hint' }, 'Connectez-vous pour voir les listes de matériel qui vous sont attribuées.'),
      el('button', { type: 'button', class: 'primary', onclick: () => openReader('depart') }, 'Scanner mon badge'),
      el('button', { type: 'button', class: 'secondary list-gap', onclick: () => { pendingAfterLogin = 'depart'; loginAction = null; openNameLogin(); } },
        'Sans badge : nom et code PIN'),
    ];
  }
  if (!depart.lists) return [el('p', { class: 'hint' }, 'Chargement des listes…')];
  if (!depart.lists.length) {
    return [el('p', { class: 'hint' }, "Aucune liste de matériel ne vous est attribuée pour l'instant. Un chef peut vous en "
      + 'attribuer une, ou ouvrez son lien ou son QR code. Sinon, prenez un lot dans l\'onglet « Lots ».')];
  }
  return [
    el('p', { class: 'hint' }, 'Touchez la liste du poste pour scanner le matériel demandé avant de partir.'),
    el('ul', { class: 'list' }, ...depart.lists.map((row) => el('li', { onclick: () => openList(row.id) },
      el('div', { class: 'main' },
        el('div', { class: 'name' }, row.name),
        el('div', { class: 'sub' }, [fmtListDate(row.date), row.owner_name, `${row.done_count}/${nb(row.line_count, 'ligne prête', 'lignes prêtes')}`,
          'modifiée ' + fmtDateTime(row.updated)].filter(Boolean).join(' · '))),
      el('span', { class: 'tag ' + (LIST_TAGS[row.status] || 'blue') }, row.status_label)))),
  ];
}

// lots trouves par la recherche (nom, type, description ; id pour les admins), les plus proches d'abord, puis
// dans l'ordre de state.lots (lots a traiter en haut)
function departLotResults() {
  if (!state.lots) {
    return el('p', { class: 'hint' }, state.loading === 'lots' ? 'Chargement des lots…' : 'Liste non chargée.');
  }
  const { lotRow, hidden } = lotRows(() => renderDepartPage(true), {
    lead: (lot) => {
      const on = depart.selected.has(lot.id) || !!departParentPicked(lot);
      return el('span', { class: 'pick-box' + (on ? ' on' : '') + (depart.blocked[lot.id] ? ' off' : '') }, on ? '✓' : '');
    },
    click: departToggle,
    note: (lot) => {
      const parent = departParentPicked(lot);
      return depart.blocked[lot.id] || (parent ? `Part avec ${parent.name}` : '');
    },
    cls: (lot) => (depart.blocked[lot.id] ? 'pick-blocked' : depart.selected.has(lot.id) ? 'pick-on' : ''),
  });
  const lots = state.lots.filter((lot) => !lot.storage);
  const words = searchKey(depart.query).split(/\s+/).filter(Boolean);
  if (!words.length) return el('ul', { class: 'list' }, ...lots.filter((lot) => !hidden(lot)).map((lot) => lotRow(lot)));
  const found = lots.map((lot, index) => ({ lot, index, score: searchScore({ label: lot.name,
    key: lot.lot_type_name + (isAdmin() ? ' ' + lot.id : ''), description: lot.description }, words) }))
    .filter((row) => row.score >= 0)
    .sort((a, b) => b.score - a.score || a.index - b.index);
  if (!found.length) return el('p', { class: 'hint' }, 'Aucun lot trouvé.');
  return el('ul', { class: 'list' }, ...found.map((row) => lotRow(row.lot, true)));
}

function departLotsTab() {
  if (!depart.input) {
    depart.input = el('input', { type: 'search', class: 'depart-search', placeholder: '🔍 Rechercher un lot : nom, type…',
      autocomplete: 'off', 'aria-label': 'Rechercher un lot' });
    depart.input.addEventListener('input', () => {
      depart.query = depart.input.value;
      const results = $('#depart-results');
      if (results) results.replaceChildren(departLotResults());
    });
  }
  depart.input.value = depart.query;
  return [
    depart.input,
    el('p', { class: 'hint' }, 'Cochez tout ce que vous prenez pour le poste, puis « Partir » : vous scannerez '
      + "l'étiquette de chaque lot. La flèche › ouvre la fiche d'un lot."),
    el('div', { id: 'depart-results' }, departLotResults()),
    departCount() ? el('div', { class: 'depart-go' },
      el('button', { type: 'button', class: 'primary', onclick: departGo },
        depart.busy ? 'Création de la liste…' : `🎒 Partir avec ${nb(departCount(), 'lot')}`),
      el('button', { type: 'button', class: 'secondary', onclick: () => { depart.selected.clear(); renderDepartPage(true); } },
        'Tout décocher')) : '',
  ];
}

function renderDepartPage(force = false) {
  const view = $('#home-depart');
  if (!view || !inHome() || state.homePage !== 'depart') return;
  // saisie en cours : seuls les resultats changent, le champ garde son focus
  if (!force && depart.tab === 'lots' && depart.input && document.activeElement === depart.input && $('#depart-results')) {
    $('#depart-results').replaceChildren(departLotResults());
    return;
  }
  const count = depart.lists ? depart.lists.length : 0;
  const choose = (tab) => () => { depart.tab = tab; renderDepartPage(); };
  const parts = [
    el('div', { class: 'segmented' },
      el('button', { type: 'button', class: depart.tab === 'lists' ? 'active' : '', onclick: choose('lists') },
        count > 1 ? `Mes listes (${count})` : 'Ma liste'),
      el('button', { type: 'button', class: depart.tab === 'lots' ? 'active' : '', onclick: choose('lots') }, 'Lots')),
  ];
  if (depart.tab === 'lists') parts.push(...departListsTab());
  else if (depart.tab === 'lots') parts.push(...departLotsTab());
  else parts.push(el('p', { class: 'hint' }, 'Chargement…'));
  view.classList.toggle('with-go', depart.tab === 'lots' && !!departCount());
  view.replaceChildren(...parts);
}
