/* QRProtec - front web : Gestion de la TECH (taches, etat des stocks, previsions, nouvelles etiquettes). */
'use strict';

const HORIZONS = [{ label: 'Auj.', months: 0 }, { label: 'M+1', months: 1 }, { label: 'M+3', months: 3 },
  { label: 'M+6', months: 6 }];
const MONTHS = ['janv.', 'févr.', 'mars', 'avr.', 'mai', 'juin', 'juil.', 'août', 'sept.', 'oct.', 'nov.', 'déc.'];

// Graphe des peremptions des prochains mois (tous types) : barres empilees stock / lots / scelles, perdus en rouge
function expiryChart(types) {
  const months = (types[0] && types[0].calendar) || [];
  const sums = months.map((month, index) => {
    const sum = { start: month.start, stock: 0, lots: 0, sealed: 0, lost: 0 };
    for (const type of types) {
      const row = type.calendar[index];
      sum.stock += row.expiring.stock; sum.lots += row.expiring.lots; sum.sealed += row.expiring.sealed;
      sum.lost += row.lost.stock + row.lost.lots + row.lost.sealed;
    }
    return sum;
  });
  const top = Math.max(1, ...sums.map((s) => s.stock + s.lots + s.sealed));
  const W = 300, H = 120, left = 24, bottom = 16, plot = H - bottom - 6, step = (W - left) / Math.max(1, sums.length);
  const svgEl = (tag, attrs, text) => {
    const node = document.createElementNS('http://www.w3.org/2000/svg', tag);
    for (const [key, value] of Object.entries(attrs)) node.setAttribute(key, value);
    if (text != null) node.textContent = text;
    return node;
  };
  const svg = svgEl('svg', { viewBox: `0 0 ${W} ${H}`, class: 'chart', role: 'img',
    'aria-label': 'Péremptions des prochains mois' });
  const y = (v) => 6 + plot - (v / top) * plot;
  for (const v of [0, Math.round(top / 2), top]) {
    svg.append(svgEl('line', { x1: left, x2: W, y1: y(v), y2: y(v), class: 'grid' }));
    svg.append(svgEl('text', { x: left - 4, y: y(v) + 3, 'text-anchor': 'end' }, v));
  }
  sums.forEach((sum, index) => {
    const x = left + index * step + step * 0.2, w = step * 0.6;
    let base = 0;
    for (const [key, cls] of [['stock', 's-stock'], ['lots', 's-lots'], ['sealed', 's-sealed']]) {
      if (!sum[key]) continue;
      const height = y(base) - y(base + sum[key]);
      svg.append(svgEl('rect', { x, y: y(base + sum[key]), width: w, height, class: cls }));
      base += sum[key];
    }
    if (sum.lost) {
      svg.append(svgEl('rect', { x: x + w * 0.3, y: y(sum.lost), width: w * 0.4, height: y(0) - y(sum.lost),
        class: 's-lost' }));
    }
    const date = parseDate(sum.start);
    const label = date ? MONTHS[date.getMonth()] : '';
    svg.append(svgEl('text', { x: x + w / 2, y: H - 3, 'text-anchor': 'middle' }, label));
    // zone de survol / appui sur toute la colonne du mois : detail sous le graphe
    const text = monthDetail(date, sum);
    const hit = svgEl('rect', { x: left + index * step, y: 0, width: step, height: H, class: 'hit' });
    hit.append(svgEl('title', {}, text));
    const show = () => {
      detail.textContent = text;
      svg.querySelectorAll('.hit.active').forEach((node) => node.classList.remove('active'));
      hit.classList.add('active');
    };
    hit.addEventListener('pointerenter', show);
    hit.addEventListener('click', show);
    svg.insertBefore(hit, svg.firstChild);
  });
  const detail = el('p', { class: 'hint chart-detail' }, 'Survolez ou touchez un mois pour le détail.');
  return el('figure', { class: 'chart-box' }, svg,
    el('figcaption', { class: 'legend' },
      ...[['s-stock', 'stock'], ['s-lots', 'lots'], ['s-sealed', 'scellés'], ['s-lost', 'périmés (à jeter)']]
        .map(([cls, label]) => el('span', {}, el('i', { class: cls }), label))),
    detail);
}

// Detail d'un mois du graphe des peremptions : combien perimment, ou, et combien seront a jeter
function monthDetail(date, sum) {
  const month = date ? `${MONTHS[date.getMonth()]} ${date.getFullYear()}` : '';
  const total = sum.stock + sum.lots + sum.sealed;
  if (!total) return `${month} : rien ne périme.`;
  const where = [[sum.stock, 'en stock'], [sum.lots, 'en lots'], [sum.sealed, 'en lots scellés']]
    .filter(([count]) => count).map(([count, label]) => `${count} ${label}`).join(', ');
  const lost = sum.lost
    ? `dont ${sum.lost} ${pl(sum.lost, 'ne sera pas utilisé', 'ne seront pas utilisés')} à temps (à jeter)`
    : 'tout sera utilisé avant sa date';
  return `${month} : ${total} ${pl(total, 'périme', 'périment')} (${where}), ${lost}.`;
}

// Previsions de stock : horizon, graphe, types a surveiller, echanges
function renderForecast(parts) {
  const forecast = state.forecast;
  if (!forecast) {
    parts.push(el('p', { class: 'hint' }, state.loading === 'stock' ? 'Chargement…' : 'Prévisions indisponibles.'));
    return;
  }
  const horizon = HORIZONS[state.horizon];
  parts.push(el('div', { class: 'segmented', role: 'group', 'aria-label': 'Horizon' },
    ...HORIZONS.map((h, index) => el('button', { type: 'button', class: index === state.horizon ? 'active' : '',
      onclick: () => { state.horizon = index; renderStock(); } }, h.label))));
  const types = forecast.types || [];
  const at = (type) => type.points[Math.min(horizon.months, type.points.length - 1)];
  if (types.length) {
    parts.push(el('p', { class: 'hint' }, `Stock valide au ${fmtDate(at(types[0]).date)}, en utilisant d'abord les dates `
      + `les plus proches (consommation mesurée sur ${forecast.history_days} jours).`));
    parts.push(expiryChart(types));
  }
  const sorted = [...types].sort((a, b) => {
    if (!!a.below_min !== !!b.below_min) return a.below_min ? -1 : 1;
    if (a.below_min) return a.below_min < b.below_min ? -1 : 1;
    return stockOrder(at(a).stock, a.min_quantity) - stockOrder(at(b).stock, b.min_quantity);
  });
  // types sans minimum attendu (x/0, 0/0) replies sous un bouton en bas, sauf commande a passer
  const zero = (type) => type.min_quantity <= 0 && !type.order && !type.below_min;
  const shown = sorted.filter((type) => !zero(type));
  const hidden = sorted.filter(zero);
  const forecastRow = (type) => {
    const point = at(type);
    const details = [];
    if (point.expiring) details.push(`${point.expiring} ${pl(point.expiring, 'périme', 'périment')}` + (point.lost ? ` dont ${point.lost} à jeter` : ''));
    if (type.order) details.push(`commander ${type.order.quantity}${type.order.subtype_name ? ' × ' + type.order.subtype_name : ''} avant le ${fmtDate(type.order.before)}`);
    else if (!details.length) details.push(type.per_month ? `${nb(Math.round(type.per_month * 10) / 10, 'utilisé')} par mois` : 'rien à signaler');
    const kind = type.order && type.order.urgent ? 'bad' : type.below_min ? 'warn' : '';
    return el('div', { class: 'stock-row ' + kind },
      requirementRow(type.name + (type.unit ? ` (${type.unit})` : ''), point.stock, type.min_quantity),
      el('div', { class: 'sub hint' }, details.join(' · ')));
  };
  parts.push(...shown.map(forecastRow));
  zeroExpectedFold(parts, hidden, forecastRow);
  const transfers = forecast.transfers || [];
  if (transfers.length) {
    parts.push(el('h3', {}, 'Échanges à faire'));
    parts.push(el('p', { class: 'hint' }, 'Items qui périmeraient sans servir là où ils sont. On les met là où ils '
      + 'seront utilisés avant leur date, et on reprend au même endroit un plus récent pour garder le lot complet.'));
    for (const group of transfers) {
      const when = (group.source.sealed ? 'scellé, ouvrir avant le ' : 'à faire avant le ') + fmtDate(group.before);
      parts.push(el('div', { class: 'stock-row ' + (group.source.sealed ? 'bad' : 'warn') },
        el('div', { class: 'row-head' }, el('b', {}, group.source.path), el('span', { class: 'hint' }, when)),
        ...group.moves.map((move) => el('div', { class: 'transfer-move' },
          el('ol', {}, ...(move.steps || []).map((step) => el('li', {}, step))),
          el('div', { class: 'sub hint' }, move.reason || '')))));
    }
  }
}

// Liste de taches de la gestion, par priorite : signalements, lots a sceller, lots invalides, vérifs recommandees,
// revisions, vérifs du stock, commandes et deplacements, puis les taches de temps en temps. Toucher une tache ouvre
// ce qui permet de la traiter (fiche du lot ou de l'item, signalements, prévisions, listes).
const TASK_LINK_HINTS = { users: 'Au poste : Administration › Utilisateurs', inventory: 'Au poste : Gestion › Inventaire',
  lot_admin: 'Au poste : Gestion › Gestion des lots', label_prints: 'Au poste : fenêtre « Impressions en attente »' };

function openTask(row) {
  if (row.link === 'lot' && row.lot) openFiche('lot', row.lot);
  else if (row.link === 'item' && row.item) openFiche('item', row.item);
  else if (row.link === 'reports') showHome('reports');
  else if (row.link === 'lists') showHome('lists');
  else if (row.link === 'forecast') { state.homeView = 'forecast'; renderStock(); $('#home-pages').scrollTop = 0; }
  else if (row.link === 'stock') { state.homeView = 'stock'; renderStock(); $('#home-pages').scrollTop = 0; }
  else if (row.link === 'label_prints') openLabelPrints();
}

function renderTasks(parts) {
  const data = state.tasks;
  if (!data) {
    parts.push(el('p', { class: 'hint' }, state.loading === 'stock' ? 'Chargement…' : 'Liste de tâches indisponible.'));
    return;
  }
  parts.push(el('p', { class: 'hint' }, 'Ce qu\'il y a à faire pour tenir le stock, le plus important en premier. '
    + 'Touchez une tâche pour l\'ouvrir.'));
  const placed = data.orders_placed || [];
  if (!data.count) {
    parts.push(el('div', { class: 'banner ok' }, '✔ Rien à faire pour l\'instant.'));
    renderPlacedOrders(parts, placed);
  } else data.categories.forEach((category, index) => {
    if (category.key === 'orders' && !category.count) renderPlacedOrders(parts, placed);
    if (!category.count) return;
    const rows = data.tasks.filter((row) => row.category === category.key);
    parts.push(el('h3', { class: 'task-head' }, el('span', { class: 'task-rank' }, String(index + 1)),
      `${category.title} (${category.count})`));
    parts.push(el('p', { class: 'hint' }, category.help));
    for (const row of rows) {
      if (row.type && row.quantity) {
        parts.push(orderTaskRow(row));
        continue;
      }
      const clickable = ['lot', 'item', 'reports', 'lists', 'forecast', 'stock'].includes(row.link)
        || row.link === 'label_prints';
      parts.push(el(clickable ? 'button' : 'div', {
        type: clickable ? 'button' : null,
        class: 'task-row stock-row' + (row.level === 'info' ? '' : ' ' + row.level),
        onclick: clickable ? () => openTask(row) : null,
      },
      el('div', { class: 'row-head' }, el('b', {}, row.title), clickable ? el('span', { class: 'hint' }, '›') : ''),
      row.detail ? el('div', { class: 'sub' }, row.detail) : '',
      TASK_LINK_HINTS[row.link] ? el('div', { class: 'sub hint' }, TASK_LINK_HINTS[row.link]) : ''));
    }
    if (category.key === 'orders') renderPlacedOrders(parts, placed);
  });
  renderUpcoming(parts, data.upcoming || []);
}

// Commande suggeree : case « Commandé » (quantite modifiable) pour enregistrer qu'elle est passee. La suggestion
// disparait tant que la commande n'est pas livree (receptions de ce type) ou decochee.
function orderTaskRow(row) {
  const quantity = el('input', { type: 'number', min: '1', step: '1', value: String(row.quantity), class: 'order-qty',
    'aria-label': 'Quantité commandée' });
  const box = el('input', { type: 'checkbox', class: 'order-box' });
  box.addEventListener('change', async () => {
    const count = parseInt(quantity.value, 10);
    if (!(count > 0)) { box.checked = false; toast('Quantité commandée ?', true); return; }
    box.disabled = true;
    try {
      state.tasks = await api('orders/', { user: badge(), type: row.type, quantity: count });
      toast(`Commande de ${count} × ${row.name || ''} enregistrée : en attente de livraison.`);
    } catch (e) {
      box.checked = false;
      toast('Commande : ' + e.message, true);
    } finally {
      renderStock();
    }
  });
  return el('div', { class: 'task-row stock-row order-row' + (row.level === 'info' ? '' : ' ' + row.level) },
    el('div', { class: 'row-head' }, el('b', {}, row.title),
      el('button', { type: 'button', class: 'text-link', onclick: () => openTask(row) }, 'Prévisions ›')),
    row.detail ? el('div', { class: 'sub' }, row.detail) : '',
    el('label', { class: 'order-check' }, box, 'Commandé :', quantity));
}

// Commandes passees en attente de livraison : sous les commandes, hors du compte des taches (sauf en retard).
function renderPlacedOrders(parts, placed) {
  if (!placed.length) return;
  parts.push(el('p', { class: 'placed-head' }, `Commandes passées, en attente de livraison (${placed.length})`));
  for (const order of placed) {
    const got = order.received ? ` · reçu ${order.received}/${order.quantity}` : '';
    const uncheck = el('input', { type: 'checkbox', class: 'order-box', checked: true,
      title: 'Décocher : la commande ne viendra pas, ou erreur' });
    uncheck.checked = true;
    uncheck.addEventListener('change', async () => {
      if (!await confirmDialog('Décocher la commande ?', `${order.quantity} × ${order.name} : la suggestion de `
        + 'commande reviendra si elle tient toujours.', 'Décocher')) { uncheck.checked = true; return; }
      try {
        state.tasks = await api(`orders/${order.id}/cancel/`, { user: badge() });
      } catch (e) {
        toast('Commande : ' + e.message, true);
      } finally {
        renderStock();
      }
    });
    parts.push(el('div', { class: 'task-row stock-row placed-row' + (order.late ? ' warn' : '') },
      el('label', { class: 'order-check' }, uncheck, el('b', {}, `${order.quantity} × ${order.name}`)),
      el('div', { class: 'sub hint' }, `commandé ${order.days ? `il y a ${nb(order.days, 'jour')}` : 'aujourd\'hui'}`
        + (order.placed_by ? ` par ${order.placed_by}` : '') + got
        + ' · se clôt toute seule à la réception')));
  }
}

// Péremptions à venir (30 jours) : pas des tâches (les secouristes remplacent les périmés pendant la vérif du lot
// rouge ; commandes et échanges à faire sont déjà des tâches). Repliées tout en bas, hors du compte, ce qui périme
// en premier en tête : 7 jours en jaune, 2 jours en orange.
function renderUpcoming(parts, rows) {
  if (!rows.length) return;
  const open = !!state.upcomingOpen;
  parts.push(el('button', { type: 'button', class: 'upcoming-toggle', 'aria-expanded': open ? 'true' : 'false',
    onclick: () => { state.upcomingOpen = !open; renderStock(); } },
  el('span', { class: 'fold-box' }, open ? '−' : '+'), `Péremptions à venir (${rows.length})`));
  if (!open) return;
  parts.push(el('p', { class: 'hint' }, 'Pour information, ce ne sont pas des tâches : les secouristes remplacent '
    + 'les items une fois périmés, pendant la vérif du lot. Les commandes et échanges à faire sont dans la liste '
    + 'ci-dessus.'));
  for (const row of rows) {
    const clickable = row.link === 'lot' || row.link === 'stock';
    parts.push(el(clickable ? 'button' : 'div', {
      type: clickable ? 'button' : null,
      class: 'task-row stock-row' + (row.level === 'info' ? '' : ' ' + row.level),
      onclick: clickable ? () => openTask(row) : null,
    },
    el('div', { class: 'row-head' }, el('b', {}, row.title), clickable ? el('span', { class: 'hint' }, '›') : ''),
    el('div', { class: 'sub' }, row.detail)));
  }
}

// Gestion de la TECH : page de l'accueil (roles gestion et admin) : à faire, état des stocks, prévisions
function stockParts(view) {
  if (!canSeeStock()) return [el('p', {}, 'Réservé aux rôles gestion et admin : scannez votre badge.')];
  const forecastMode = view === 'forecast';
  const choose = (next) => () => { state.homeView = next; renderStock(); };
  const toggle = el('div', { class: 'segmented', role: 'group', 'aria-label': 'Vue' },
    el('button', { type: 'button', class: view === 'tasks' ? 'active' : '', onclick: choose('tasks') },
      'À faire' + (state.tasks && state.tasks.count ? ` (${state.tasks.count})` : '')),
    el('button', { type: 'button', class: view === 'stock' ? 'active' : '',
      onclick: () => { choose('stock')(); debugTap(); } }, 'État des stocks'),
    el('button', { type: 'button', class: forecastMode ? 'active' : '', onclick: choose('forecast') }, 'Prévisions'));
  const head = el('div', { class: 'row-head' },
    el('h3', {}, view === 'tasks' ? 'À faire' : forecastMode ? 'Stock à venir' : 'État des stocks (lecture seule)'),
    el('div', { class: 'buttons' },
      canSeeStock() ? el('button', { type: 'button', onclick: openLabelPrints, title: 'Nouvelles étiquettes d\'items' },
        icon('plus'), 'Étiquettes') : null,
      el('button', { type: 'button', onclick: loadStock }, icon('refresh'), state.loading === 'stock' ? 'Chargement…' : 'Actualiser')));
  const parts = [toggle, head];
  if (view === 'tasks') {
    renderTasks(parts);
    return parts;
  }
  if (forecastMode) {
    renderForecast(parts);
    return parts;
  }
  if (hasNotifications()) {
    parts.push(el('p', { class: 'hint' }, 'Alertes de stock sur votre téléphone : ',
      el('button', { type: 'button', class: 'text-link', onclick: openNotifications }, 'Notifications')));
  }
  if (!state.stock) {
    parts.push(el('p', { class: 'hint' }, 'Chargement…'));
  } else {
    parts.push(el('p', { class: 'hint' }, 'Stock non périmé (hors lots, rangements compris) / minimum. Les plus critiques en premier.'));
    // sous le minimum, puis bientot sous le minimum ou vide, puis 0/0, puis complets, puis x/0 tout en bas
    const order = (row) => stockOrder(row.stock_fresh, row.min_quantity, row.stock_expiring_soon || 0);
    const rows = [...state.stock].sort((a, b) => order(a) - order(b));
    const stockRow = (row) => {
      const unit = row.unit ? ' ' + row.unit : ''; // type a unites (serum phy en mL) : ses sous-types en unites
      const details = [`${row.lots_fresh}${unit} dans les lots`];
      const group = row.group && state.stock.find((other) => other.type === row.group);
      if (group) details.push(group.unit ? `${row.fills} ${group.unit} de ${group.name}` : `sous-type de ${group.name}`);
      if (row.is_group) details.push('compte ses sous-types');
      if (row.stock_expired + row.lots_expired) details.push(nb(row.stock_expired + row.lots_expired, 'périmé'));
      if (row.expiring_soon) details.push(`${row.expiring_soon} bientôt ${pl(row.expiring_soon, 'périmé')}`);
      if (row.missing) details.push(nb(row.missing, 'disparu'));
      if (row.out) details.push(`${nb(row.out, 'sorti')} sans lot`);
      if (row.loose) details.push(`${row.loose} hors lot`);
      return el('div', { class: 'stock-row' },
        requirementRow(row.name + (row.unit ? ` (${row.unit})` : ''), row.stock_fresh, row.min_quantity),
        el('div', { class: 'sub hint' }, details.join(' · ')));
    };
    // types sans minimum attendu (x/0, 0/0) replies sous un bouton tout en bas
    parts.push(...rows.filter((row) => row.min_quantity > 0).map(stockRow));
    zeroExpectedFold(parts, rows.filter((row) => row.min_quantity <= 0), stockRow);
  }
  return parts;
}

// Types d'items a 0 attendu (minimum 0) dans une vue du stock : replies derriere un bouton [+] en bas de la liste,
// fermes par defaut, ouverts ou fermes ensemble pour l'etat des stocks et les previsions le temps de la session.
function zeroExpectedFold(parts, rows, render) {
  if (!rows.length) return;
  const open = !!state.zeroExpectedOpen;
  parts.push(el('button', { type: 'button', class: 'upcoming-toggle', 'aria-expanded': open ? 'true' : 'false',
    onclick: () => { state.zeroExpectedOpen = !open; renderStock(); } },
  el('span', { class: 'fold-box' }, open ? '−' : '+'),
  `${pl(rows.length, 'Type', 'Types')} sans minimum attendu (${rows.length})`));
  if (open) parts.push(...rows.map(render));
}

// Nouvelles etiquettes d'items d'un type existant (admins, Gestion de la TECH, bouton +). La demande part au poste,
// qui ouvre la fenetre « Impressions en attente » meme sans personne de connecte : un secouriste qui rentre avec du
// reassort lance l'impression et colle lui-meme les etiquettes. Les items sont crees a l'impression.
const labelPrints = { data: null, busy: false, type: '', picker: '' };

function openLabelPrints() {
  if (!canSeeStock()) return;
  $('#label-print-error').textContent = '';
  renderLabelPrints();
  const dialog = $('#label-prints');
  if (!dialog.open) dialog.showModal();
  loadLabelPrints();
}

async function loadLabelPrints() {
  try {
    labelPrints.data = await api('label-prints/list/', { user: badge() });
  } catch (e) {
    $('#label-print-error').textContent = e.message;
    if (e.data && e.data.pin_required) { state.user = null; save(); render(); }
  }
  renderLabelPrints();
}

const labelPrintType = () => {
  const data = labelPrints.data;
  return data ? data.types.find((type) => type.type === labelPrints.type) : null;
};

function renderLabelPrintDate() {
  const type = labelPrintType();
  $('#label-print-date-row').hidden = !(type && type.perissable);
  $('#label-print-date').required = !!(type && type.perissable);
}

// Type d'item cherche au texte (sans accents ni casse, debut des mots, code), comme au poste. Le champ n'est
// reconstruit que si la liste des types change, pour ne pas effacer ce qui est en train d'etre tape.
function renderLabelPrintPicker() {
  const data = labelPrints.data;
  const box = $('#label-print-type-box');
  const signature = data ? data.types.map((type) => type.type).join(',') : '';
  if (labelPrints.picker === signature && box.firstChild) return;
  labelPrints.picker = signature;
  if (data && !data.types.some((type) => type.type === labelPrints.type)) labelPrints.type = '';
  const items = (data ? data.types : []).map((type) => ({ key: type.type, label: type.name, description: type.description, type }));
  const picker = searchPicker(data ? 'Rechercher un type d\'item (ex : comp, sérum)' : 'Chargement…', items, (item) => {
    labelPrints.type = item.key;
    input.value = item.label;
    input.blur();
    renderLabelPrintDate();
    $('#label-print-count').focus();
  });
  const input = picker.querySelector('input');
  input.id = 'label-print-type';
  input.disabled = !data;
  const chosen = labelPrintType();
  if (chosen) input.value = chosen.name;
  // texte modifie apres un choix : le type n'est plus choisi tant qu'on n'en touche pas un autre
  input.addEventListener('input', () => {
    if (labelPrints.type && input.value !== (labelPrintType() || {}).name) { labelPrints.type = ''; renderLabelPrintDate(); }
  });
  box.replaceChildren(picker);
}

function renderLabelPrints() {
  const data = labelPrints.data;
  renderLabelPrintPicker();
  if (data) $('#label-print-count').max = data.max_count;
  renderLabelPrintDate();
  $('#label-print-send').disabled = !data || labelPrints.busy;
  const list = $('#label-print-list');
  if (!data) { list.replaceChildren(); return; }
  const pending = data.prints.filter((row) => row.pending);
  const done = data.prints.filter((row) => !row.pending);
  const what = (row) => `${row.count} × ${row.type_name}` + (row.peremption ? `, péremption ${fmtDate(row.peremption)}` : '');
  const parts = [];
  // admins : files d'impression du poste (une par personne qui demande, toutes demandes confondues), gerees et
  // lancees a distance ; leurs etiquettes d'items y sont, on ne les repete pas plus bas
  if (data.queues) parts.push(...labelQueueParts(data));
  else parts.push(el('h3', {}, pending.length ? `En attente au poste (${pending.length})` : 'En attente au poste'));
  if (!data.queues && !pending.length) parts.push(el('p', { class: 'hint' }, 'Aucune impression en attente.'));
  for (const row of data.queues ? [] : pending) {
    parts.push(el('div', { class: 'stock-row warn' },
      el('div', { class: 'row-head' }, el('b', {}, what(row)),
        el('button', { type: 'button', class: 'secondary', onclick: () => cancelLabelPrint(row) }, 'Annuler')),
      el('div', { class: 'sub hint' }, `demandé par ${row.by} ${fmtDateTime(row.created)}, expire après ${data.expiry_hours} h`)));
  }
  if (done.length) {
    parts.push(el('h3', {}, '7 derniers jours'));
    for (const row of done) {
      const status = row.printed ? `imprimé ${fmtDateTime(row.printed)}` + (row.printed_on ? ` sur ${row.printed_on}` : '')
        : row.cancelled ? `annulé ${fmtDateTime(row.cancelled)}` + (row.cancelled_by ? ` par ${row.cancelled_by}` : '')
        : `expiré, pas imprimé en ${data.expiry_hours} h`;
      parts.push(el('div', { class: 'stock-row' }, el('b', {}, what(row)),
        el('div', { class: 'sub hint' }, `${status} · demandé par ${row.by}`)));
    }
  }
  list.replaceChildren(...parts);
}

// Files d'impression du poste (admins) : qui a demande quoi, « Lancer l'impression » a distance (le poste l'imprime
// dans la minute, sans que personne n'y touche) et « Annuler la file »
function labelQueueParts(data) {
  const parts = [el('h3', {}, data.queues.length ? `Files d'impression du poste (${data.queues.length})` : "Files d'impression du poste")];
  if (!data.queues.length) {
    parts.push(el('p', { class: 'hint' }, 'Aucune impression en attente au poste.'));
    return parts;
  }
  parts.push(el('p', { class: 'hint' }, 'Une file par personne qui a demandé des étiquettes (web ou autre ordinateur). '
    + "« Lancer l'impression » : le poste l'imprime dans la minute, sans que personne n'y touche."));
  for (const queue of data.queues) {
    const launched = queue.launch_requested
      ? `lancée ${fmtDateTime(queue.launch_requested)}` + (queue.launch_requested_by ? ` par ${queue.launch_requested_by}` : '') + ', en attente du poste'
      : '';
    parts.push(el('div', { class: 'stock-row warn' },
      el('div', { class: 'row-head' }, el('b', {}, `${queue.name} · ${nb(queue.labels, 'étiquette')}`)),
      ...queue.prints.map((row) => el('div', { class: 'sub hint' },
        `${row.what}, ${row.front ? `depuis ${row.front}` : 'depuis le web'} ${fmtDateTime(row.created)}`)),
      launched ? el('div', { class: 'sub' }, launched) : null,
      el('div', { class: 'dialog-buttons' },
        el('button', { type: 'button', class: 'secondary', disabled: labelPrints.busy, onclick: () => labelQueueAction(queue, 'cancel') }, 'Annuler la file'),
        el('button', { type: 'button', class: 'primary', disabled: labelPrints.busy || !!queue.launch_requested,
          onclick: () => labelQueueAction(queue, 'launch') }, "Lancer l'impression"))));
  }
  return parts;
}

async function labelQueueAction(queue, action) {
  if (action === 'cancel' && !await confirmDialog('Annuler la file ?',
    `Les ${nb(queue.labels, 'étiquette')} de ${queue.name} ne seront pas imprimées.`, 'Annuler la file', 'danger')) return;
  labelPrints.busy = true;
  renderLabelPrints();
  try {
    await api(`label-prints/queues/${action}/`, { user: badge(), queue: queue.queue });
    feedback.good();
    toast(action === 'launch' ? `Impression de ${queue.name} lancée : le poste l'imprime dans la minute.`
      : `File de ${queue.name} annulée.`);
    loadStock();
  } catch (e) {
    $('#label-print-error').textContent = e.message;
    if (e.data && e.data.pin_required) { state.user = null; save(); render(); }
  }
  labelPrints.busy = false;
  await loadLabelPrints();
}

async function sendLabelPrint(event) {
  event.preventDefault();
  const type = labelPrintType();
  const error = $('#label-print-error');
  error.textContent = '';
  if (!type) { error.textContent = 'Cherchez le type d\'item puis touchez-le dans la liste.'; return; }
  const count = parseInt($('#label-print-count').value, 10);
  if (!(count >= 1)) { error.textContent = 'Indiquez le nombre d\'étiquettes.'; return; }
  const peremption = type.perissable ? $('#label-print-date').value : '';
  if (type.perissable && !peremption) { error.textContent = `${type.name} est périssable : indiquez la date de péremption.`; return; }
  labelPrints.busy = true;
  renderLabelPrints();
  try {
    await api('label-prints/', { user: badge(), type: type.type, count, peremption: peremption || null });
    feedback.good();
    toast(`${nb(count, 'étiquette')} de ${type.name} ${pl(count, 'envoyée', 'envoyées')} au poste.`);
    $('#label-print-count').value = 1;
    $('#label-print-date').value = '';
    labelPrints.type = '';
    labelPrints.picker = ''; // champ de recherche vide au prochain affichage
    loadStock();
  } catch (e) {
    error.textContent = e.message;
    if (e.data && e.data.pin_required) { state.user = null; save(); render(); }
  }
  labelPrints.busy = false;
  await loadLabelPrints();
}

async function cancelLabelPrint(row) {
  if (!await confirmDialog("Annuler l'impression ?", `${row.count} × ${row.type_name} ne seront pas imprimées.`, "Annuler l'impression", 'danger')) return;
  try {
    await api(`label-prints/${row.id}/cancel/`, { user: badge() });
    loadStock();
  } catch (e) {
    $('#label-print-error').textContent = e.message;
  }
  await loadLabelPrints();
}

$('#label-print-form').addEventListener('submit', sendLabelPrint);
$('#label-print-close').addEventListener('click', () => $('#label-prints').close());

function renderStock() {
  $('#home-stock').replaceChildren(...stockParts(state.homeView || 'tasks'));
}
