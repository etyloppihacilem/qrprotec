/* QRProtec - front web : stats fun de l'année (gestion et admin), à présenter à l'AG. */
'use strict';

// Stats fun : page cachée de l'accueil, ouverte par 5 appuis rapides sur « Prévisions » de la Gestion de la TECH
// (comme l'outil de debug derrière « État des stocks »). Podiums des secouristes (vérifs, périmés trouvés, duos...),
// records et chiffres de l'année, en liste ou en plein écran une carte à la fois (bouton « Présenter », pour l'AG).
// Le serveur (api/stats/fun/) refuse les autres rôles : la page cachée n'est pas la sécurité. Prénoms seulement.

const FUN_TAPS = 5;
const FUN_TAP_MS = 700;
const MEDALS = ['🥇', '🥈', '🥉'];

const fun = {
  data: null,
  loading: false,
  error: '',
  period: 'last12',   // « last12 » ou une année civile
  slide: -1,          // carte affichée en plein écran (-1 : fermé ; 0 : les chiffres de l'année)
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

function funPodium(rows) {
  return el('ol', { class: 'fun-podium' }, ...rows.map((row) => el('li', { class: 'rank-' + row.rank },
    el('span', { class: 'medal' }, MEDALS[row.rank - 1] || `${row.rank}.`),
    el('span', { class: 'who' }, row.name),
    el('span', { class: 'value' }, `${row.value} ${row.unit}`))));
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
    el('p', { class: 'hint' }, 'Les records de l\'équipe, à présenter à l\'AG. « Présenter » les affiche en grand, '
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
  if (!data.awards.length) parts.push(el('p', { class: 'hint' }, 'Pas encore assez de vérifs sur cette période.'));
  parts.push(...data.awards.map(funCard));
  view.replaceChildren(...parts);
}

// ---- Plein écran : une carte à la fois (0 : chiffres de l'année, puis chaque récompense)

function funShow(index) {
  const dialog = $('#fun-show');
  const awards = fun.data ? fun.data.awards : [];
  if (index < 0 || index > awards.length) { dialog.close(); return; }
  fun.slide = index;
  const slide = index === 0
    ? el('div', { class: 'fun-slide' }, el('div', { class: 'emoji' }, '🎉'),
      el('h2', {}, `L'année en chiffres`), el('p', { class: 'hint' }, fun.data.period), funTotals(fun.data.totals))
    : (() => {
      const award = awards[index - 1];
      return el('div', { class: 'fun-slide' }, el('div', { class: 'emoji' }, award.emoji), el('h2', {}, award.title),
        el('p', { class: 'hint' }, award.text),
        award.podium.length ? funPodium(award.podium) : funRecord(award.record));
    })();
  dialog.replaceChildren(
    el('div', { class: 'fun-show-head' }, el('span', { class: 'hint' }, `${index + 1} / ${awards.length + 1}`),
      el('button', { type: 'button', 'aria-label': 'Fermer', onclick: (event) => { event.stopPropagation(); dialog.close(); } }, '✕')),
    slide,
    el('div', { class: 'fun-show-nav' },
      el('button', { type: 'button', disabled: index === 0 ? '' : null,
        onclick: (event) => { event.stopPropagation(); funShow(index - 1); } }, '← Précédente'),
      el('button', { type: 'button', class: 'play',
        onclick: (event) => { event.stopPropagation(); funShow(index + 1); } },
      index === awards.length ? 'Terminer' : 'Suivante →')));
  if (!dialog.open) dialog.showModal();
}

$('#fun-show').addEventListener('click', () => funShow(fun.slide + 1));
$('#fun-show').addEventListener('close', () => { fun.slide = -1; });
document.addEventListener('keydown', (event) => {
  if (fun.slide < 0) return;
  if (['ArrowRight', 'ArrowDown', 'PageDown', ' '].includes(event.key)) { event.preventDefault(); funShow(fun.slide + 1); }
  if (['ArrowLeft', 'ArrowUp', 'PageUp'].includes(event.key)) { event.preventDefault(); funShow(Math.max(0, fun.slide - 1)); }
});
