/* QRProtec - front web : stats fun de l'année (gestion et admin), à présenter à l'AG. */
'use strict';

// Stats fun : page cachée de l'accueil, ouverte par 5 appuis rapides sur « Prévisions » de la Gestion de la TECH
// (comme l'outil de debug derrière « État des stocks »). Podiums des secouristes (vérifs, périmés trouvés, duos...),
// records et chiffres de l'année, en liste ou en plein écran une carte à la fois (bouton « Présenter », pour l'AG).
// Le serveur (api/stats/fun/) refuse les autres rôles : la page cachée n'est pas la sécurité. Prénoms seulement.
// Durées (vérifs chronométrées seulement) : tuiles du temps passé, cartes de vitesse, et une carte « Chrono d'un
// lot » où l'on choisit le lot (vérif complète la plus rapide, la plus lente, meilleurs temps), aussi en plein écran.

const FUN_TAPS = 5;
const FUN_TAP_MS = 700;
const MEDALS = ['🥇', '🥈', '🥉'];

const fun = {
  data: null,
  loading: false,
  error: '',
  period: 'last12',   // « last12 » ou une année civile
  slide: -1,          // carte affichée en plein écran (-1 : fermé ; 0 : les chiffres de l'année)
  lot: '',            // lot choisi pour « Chrono d'un lot » (le plus vérifié par défaut)
};
let funTaps = { count: 0, at: 0 };

function funTap() {
  if (!canSeeStock()) return;
  const now = Date.now();
  funTaps = { count: now - funTaps.at < FUN_TAP_MS ? funTaps.count + 1 : 1, at: now };
  const left = FUN_TAPS - funTaps.count;
  if (left > 0) {
    if (left <= 2) toast(`Plus que ${nb(left, 'appui')} pour les stats de l'année`);
    return;
  }
  funTaps = { count: 0, at: 0 };
  showHome('stats');
}

async function loadFunStats() {
  if (!canSeeStock() || fun.loading) return;
  fun.loading = true;
  fun.error = '';
  renderFunStats();
  try {
    fun.data = await api('stats/fun/', { user: badge(), period: fun.period });
  } catch (e) {
    fun.error = e.message;
    if (e.data && e.data.pin_required) { state.user = null; save(); }
  } finally {
    fun.loading = false;
    render();
  }
}

// Chiffres de l'année : tuiles en tête de page et première carte du plein écran
function funTotals(totals) {
  const tiles = [
    [totals.verifs, 'vérif', 'vérifs'],
    [totals.lots, 'lot vérifié', 'lots vérifiés'],
    [totals.scanned, 'item scanné', 'items scannés'],
    [totals.expired, 'périmé débusqué', 'périmés débusqués'],
    [totals.team_verifs, 'vérif à plusieurs', 'vérifs à plusieurs'],
    [totals.people, 'secouriste en vérif', 'secouristes en vérif'],
    [totals.used, 'item utilisé', 'items utilisés'],
  ];
  return el('div', { class: 'fun-totals' }, ...tiles.filter(([value]) => value > 0).map(([value, one, many]) =>
    el('div', { class: 'fun-total' }, el('b', {}, String(value)), el('span', {}, value > 1 ? many : one))));
}

// Temps passé en vérif (vérifs chronométrées) : tuiles déjà écrites par le serveur
function funTimeTotals(tiles) {
  if (!tiles || !tiles.length) return null;
  return el('div', { class: 'fun-totals fun-time' }, ...tiles.map((tile) =>
    el('div', { class: 'fun-total' }, el('b', {}, tile.value), el('span', {}, tile.label))));
}

// Chrono d'un lot : le lot choisi dans la liste (ceux qui ont au moins une vérif complète chronométrée)
function funLot() {
  const lots = (fun.data && fun.data.lots) || [];
  return lots.find((lot) => lot.id === fun.lot) || lots[0] || null;
}

function funLotBody(lot) {
  return el('div', { class: 'fun-lot' },
    el('div', { class: 'fun-lot-records' },
      el('div', {}, el('div', { class: 'hint' }, '🚀 La plus rapide'), funRecord(lot.fastest)),
      lot.count > 1 ? el('div', {}, el('div', { class: 'hint' }, '🐢 La plus lente'), funRecord(lot.slowest)) : null),
    el('p', { class: 'hint' }, `${nb(lot.count, 'vérif complète chronométrée', 'vérifs complètes chronométrées')}`
      + (lot.count > 1 ? `, ${lot.average} en moyenne` : '')),
    lot.podium.length > 1 ? [el('div', { class: 'hint' }, 'Meilleurs temps'), funPodium(lot.podium)] : null);
}

function funLotCard() {
  const lots = (fun.data && fun.data.lots) || [];
  const lot = funLot();
  if (!lot) return null;
  return el('section', { class: 'fun-card' },
    el('div', { class: 'fun-card-head' }, el('span', { class: 'emoji' }, '⏱️'),
      el('div', {}, el('h4', {}, 'Chrono d\'un lot'),
        el('p', { class: 'hint' }, 'Vérif complète la plus rapide et la plus lente du lot choisi.'))),
    el('select', { class: 'fun-lot-select', 'aria-label': 'Lot', onchange: (event) => {
      fun.lot = event.target.value;
      renderFunStats();
    } }, ...lots.map((one) => el('option', { value: one.id, selected: one.id === lot.id ? '' : null },
      `${one.name} (${nb(one.count, 'vérif')})`))),
    funLotBody(lot));
}

function funPodium(rows) {
  return el('ol', { class: 'fun-podium' }, ...rows.map((row) => el('li', { class: 'rank-' + row.rank },
    el('span', { class: 'medal' }, MEDALS[row.rank - 1] || `${row.rank}.`),
    el('span', { class: 'who' }, row.name),
    el('span', { class: 'value' }, row.unit ? `${row.value} ${row.unit}` : String(row.value)))));
}

function funRecord(record) {
  return el('div', { class: 'fun-record' },
    el('div', { class: 'value' }, record.value),
    el('div', { class: 'who' }, record.name),
    record.detail ? el('div', { class: 'hint' }, record.detail) : '');
}

function funCard(award) {
  return el('section', { class: 'fun-card' },
    el('div', { class: 'fun-card-head' }, el('span', { class: 'emoji' }, award.emoji),
      el('div', {}, el('h4', {}, award.title), el('p', { class: 'hint' }, award.text))),
    award.podium.length ? funPodium(award.podium) : funRecord(award.record));
}

function renderFunStats() {
  const view = $('#home-stats');
  if (!canSeeStock()) {
    view.replaceChildren(el('p', {}, 'Réservé aux rôles gestion et admin : scannez votre badge.'));
    return;
  }
  const data = fun.data;
  const periods = ['last12', ...((data && data.years) || [])];
  const parts = [
    el('div', { class: 'row-head' }, el('h3', {}, 'Stats de l\'année 🎉'),
      el('div', { class: 'buttons' },
        data && data.awards.length ? el('button', { type: 'button', class: 'play', onclick: () => funShow(0) },
          '▶ Présenter') : null,
        el('button', { type: 'button', onclick: loadFunStats }, icon('refresh'),
          fun.loading ? 'Chargement…' : 'Actualiser'))),
    el('p', { class: 'hint' }, 'Les records de l\'équipe. « Présenter » les affiche en grand, '
      + 'une carte à la fois : touchez l\'écran ou la flèche → pour passer à la suivante.'),
    el('div', { class: 'segmented', role: 'group', 'aria-label': 'Période' }, ...periods.map((value) =>
      el('button', { type: 'button', class: String(value) === String(fun.period) ? 'active' : '',
        onclick: () => { fun.period = value; fun.data = null; loadFunStats(); } },
      value === 'last12' ? '12 derniers mois' : String(value)))),
  ];
  if (fun.error) parts.push(el('p', { class: 'pin-error' }, fun.error));
  if (!data) {
    if (fun.loading) parts.push(el('p', { class: 'hint' }, 'Chargement…'));
    view.replaceChildren(...parts);
    return;
  }
  parts.push(el('p', { class: 'hint' }, `Du ${fmtDate(data.start)} au ${fmtDate(data.end)}.`), funTotals(data.totals));
  const time = funTimeTotals(data.time_totals);
  if (time) parts.push(el('h4', { class: 'fun-section' }, '⏱️ Le temps des vérifs'), time);
  if (!data.awards.length) parts.push(el('p', { class: 'hint' }, 'Pas encore assez de vérifs sur cette période.'));
  parts.push(...data.awards.map(funCard));
  const lotCard = funLotCard();
  if (lotCard) parts.push(lotCard);
  else if (data.awards.length) {
    parts.push(el('p', { class: 'hint' }, 'Chrono des lots : pas encore de vérif complète chronométrée sur cette '
      + 'période (les vérifs d\'avant le chronométrage n\'ont pas de durée).'));
  }
  view.replaceChildren(...parts);
}

// ---- Plein écran : une carte à la fois (0 : chiffres de l'année, puis chaque récompense)

// diapositives : les chiffres, le temps des vérifs (s'il y a des durées), chaque récompense, le chrono du lot choisi
function funSlides() {
  const data = fun.data;
  const slides = [() => el('div', { class: 'fun-slide' }, el('div', { class: 'emoji' }, '🎉'),
    el('h2', {}, `L'année en chiffres`), el('p', { class: 'hint' }, data.period), funTotals(data.totals))];
  if (data.time_totals && data.time_totals.length) {
    slides.push(() => el('div', { class: 'fun-slide' }, el('div', { class: 'emoji' }, '⏱️'),
      el('h2', {}, 'Le temps des vérifs'), el('p', { class: 'hint' }, data.period), funTimeTotals(data.time_totals)));
  }
  for (const award of data.awards) {
    slides.push(() => el('div', { class: 'fun-slide' }, el('div', { class: 'emoji' }, award.emoji),
      el('h2', {}, award.title), el('p', { class: 'hint' }, award.text),
      award.podium.length ? funPodium(award.podium) : funRecord(award.record)));
  }
  const lot = funLot();
  if (lot) {
    slides.push(() => el('div', { class: 'fun-slide' }, el('div', { class: 'emoji' }, '⏱️'),
      el('h2', {}, `Chrono : ${lot.name}`), el('p', { class: 'hint' }, 'Vérif complète la plus rapide et la plus lente.'),
      funLotBody(lot)));
  }
  return slides;
}

function funShow(index) {
  const dialog = $('#fun-show');
  const slides = fun.data ? funSlides() : [];
  if (index < 0 || index >= slides.length) { dialog.close(); return; }
  fun.slide = index;
  const slide = slides[index]();
  const last = slides.length - 1;
  dialog.replaceChildren(
    el('div', { class: 'fun-show-head' }, el('span', { class: 'hint' }, `${index + 1} / ${slides.length}`),
      el('button', { type: 'button', 'aria-label': 'Fermer', onclick: (event) => { event.stopPropagation(); dialog.close(); } }, '✕')),
    slide,
    el('div', { class: 'fun-show-nav' },
      el('button', { type: 'button', disabled: index === 0 ? '' : null,
        onclick: (event) => { event.stopPropagation(); funShow(index - 1); } }, '← Précédente'),
      el('button', { type: 'button', class: 'play',
        onclick: (event) => { event.stopPropagation(); funShow(index + 1); } },
      index === last ? 'Terminer' : 'Suivante →')));
  if (!dialog.open) dialog.showModal();
}

$('#fun-show').addEventListener('click', () => funShow(fun.slide + 1));
$('#fun-show').addEventListener('close', () => { fun.slide = -1; });
document.addEventListener('keydown', (event) => {
  if (fun.slide < 0) return;
  if (['ArrowRight', 'ArrowDown', 'PageDown', ' '].includes(event.key)) { event.preventDefault(); funShow(fun.slide + 1); }
  if (['ArrowLeft', 'ArrowUp', 'PageUp'].includes(event.key)) { event.preventDefault(); funShow(Math.max(0, fun.slide - 1)); }
});
