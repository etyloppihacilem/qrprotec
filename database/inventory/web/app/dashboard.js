/* QRProtec - front web : tableau de bord de la TECH (gestion et admin). */
'use strict';

// Tableau de bord : bouton bien visible en haut de la Gestion de la TECH (pas caché comme les stats de l'AG). Tout est
// calculé et écrit par le serveur (api/dashboard/summary/, inventory/dashboard.py), le poste affiche la même chose :
// scores de fonctionnement de chaque activité, alertes (ce qui mérite l'attention d'abord), puis une section par
// sujet avec ses chiffres, ses phrases, ses graphes par mois et ses tableaux. Prénoms seulement (API publique).

const dash = { data: null, loading: false, error: '', open: {} };
const DASH_LINKS = { lots: 'État des lots', reports: 'Signalements', lists: 'Listes de matériel', forecast: 'Prévisions',
  stats: 'Stats de l\'année', purchases: 'Au poste : Réception > Commandes fournisseurs',
  consumables: 'Au poste : Étiquettes > Consommables' };

async function loadDashboard() {
  if (!canSeeStock() || dash.loading) return;
  dash.loading = true;
  dash.error = '';
  renderDashboard();
  try {
    dash.data = await api('dashboard/summary/', { user: badge() });
  } catch (e) {
    dash.error = e.message;
    if (e.data && e.data.pin_required) { state.user = null; save(); }
  } finally {
    dash.loading = false;
    render();
  }
}

function dashGo(link) {
  if (link === 'lots') showHome('lots');
  else if (link === 'reports') showHome('reports');
  else if (link === 'lists') showHome('lists');
  else if (link === 'stats') showHome('stats');
  else if (link === 'forecast') { state.homeView = 'forecast'; showHome('stock'); }
}

// Score de 0 à 100 dans un anneau de la couleur de son niveau (gris : pas encore assez de données)
function dashRing(value, level, big = false) {
  const size = big ? 92 : 68, r = size / 2 - 7, c = 2 * Math.PI * r;
  const svg = svgNode('svg', { viewBox: `0 0 ${size} ${size}`, class: 'dash-ring ' + (level || 'none'),
    width: size, height: size, role: 'img', 'aria-label': value == null ? 'Pas encore de score' : `${value} sur 100` });
  svg.append(svgNode('circle', { cx: size / 2, cy: size / 2, r, class: 'track' }));
  if (value != null) {
    svg.append(svgNode('circle', { cx: size / 2, cy: size / 2, r, class: 'bar',
      'stroke-dasharray': `${c * value / 100} ${c}`, transform: `rotate(-90 ${size / 2} ${size / 2})` }));
  }
  svg.append(svgNode('text', { x: size / 2, y: size / 2 + (big ? 9 : 6), 'text-anchor': 'middle' },
    value == null ? '–' : String(value)));
  return svg;
}

function dashScores(data) {
  if (!data.scores || !data.scores.length) return null;
  const overall = data.overall || {};
  return el('section', { class: 'dash-scores' },
    el('div', { class: 'dash-overall' }, dashRing(overall.value, overall.level, true),
      el('div', {}, el('b', {}, 'Fonctionnement global'), el('p', { class: 'hint' }, overall.detail || ''))),
    el('div', { class: 'dash-score-list' }, ...data.scores.map((row) => el('button', {
      type: 'button', class: 'dash-score', title: row.detail,
      onclick: () => { dash.open[row.key] = true; renderDashboard(); const node = $('#dash-' + row.key); if (node) node.scrollIntoView({ behavior: 'smooth' }); },
    }, dashRing(row.value, row.level), el('b', {}, `${row.emoji} ${row.title}`),
    el('small', {}, row.parts.join(' · ') || row.detail)))));
}

function dashTiles(tiles) {
  return el('div', { class: 'fun-totals dash-tiles' }, ...tiles.map((tile) =>
    el('div', { class: 'fun-total ' + (tile.level || '') }, el('b', {}, tile.value), el('span', {}, tile.label),
      tile.detail ? el('small', {}, tile.detail) : null)));
}

// Barres par mois (empilées ou côte à côte), détail du mois au survol ou à l'appui
function dashChart(chart) {
  const n = chart.labels.length;
  if (!n || !chart.series.some((s) => s.values.some((v) => v > 0))) return null;
  const sums = chart.labels.map((_, i) => chart.series.reduce((sum, s) => sum + (s.values[i] || 0), 0));
  const top = Math.max(1, ...(chart.stacked ? sums : chart.series.flatMap((s) => s.values)));
  const W = 320, H = 130, left = 30, bottom = 16, plot = H - bottom - 6, step = (W - left) / n;
  const y = (v) => 6 + plot - (v / top) * plot;
  const unit = chart.unit ? ' ' + chart.unit : '';
  const round = (v) => (Math.round(v * 100) / 100).toLocaleString('fr-FR');
  const svg = svgNode('svg', { viewBox: `0 0 ${W} ${H}`, class: 'chart', role: 'img', 'aria-label': chart.title });
  for (const v of top >= 4 ? [0, top / 2, top] : [0, top]) {
    svg.append(svgNode('line', { x1: left, x2: W, y1: y(v), y2: y(v), class: 'grid' }));
    svg.append(svgNode('text', { x: left - 4, y: y(v) + 3, 'text-anchor': 'end' }, Math.round(v)));
  }
  const detail = el('p', { class: 'hint chart-detail' }, '');
  chart.labels.forEach((label, i) => {
    const x = left + i * step + step * 0.15, w = step * 0.7;
    let base = 0;
    chart.series.forEach((s, k) => {
      const v = s.values[i] || 0;
      if (!v) return;
      if (chart.stacked) {
        svg.append(svgNode('rect', { x, y: y(base + v), width: w, height: y(base) - y(base + v), fill: s.color }));
        base += v;
      } else {
        const bw = w / chart.series.length;
        svg.append(svgNode('rect', { x: x + k * bw, y: y(v), width: bw, height: y(0) - y(v), fill: s.color }));
      }
    });
    if (n <= 12 || i % 2 === 0) {
      svg.append(svgNode('text', { x: x + w / 2, y: H - 4, 'text-anchor': 'middle' }, label.split(' ')[0]));
    }
    const hit = svgNode('rect', { x: left + i * step, y: 0, width: step, height: H - bottom, class: 'hit' });
    const show = () => {
      svg.querySelectorAll('.hit.active').forEach((node) => node.classList.remove('active'));
      hit.classList.add('active');
      detail.textContent = `${label} : ` + chart.series.map((s) => `${s.name} ${round(s.values[i] || 0)}${unit}`).join(', ');
    };
    hit.addEventListener('mouseenter', show);
    hit.addEventListener('click', show);
    svg.append(hit);
  });
  const legend = el('div', { class: 'dash-legend' }, ...chart.series.map((s) =>
    el('span', {}, el('i', { style: `background:${s.color}` }), s.name)));
  return el('div', { class: 'chart-box' }, el('h5', {}, chart.title), svg, legend, detail);
}

// Tableau : une ligne d'un lot ouvre sa fiche ; une ligne avec une explication (help, ex : genres d'opérations)
// l'affiche dans une bulle sous la ligne quand on la touche
function dashTable(table) {
  const rows = [];
  for (const row of table.rows) {
    const bubble = row.help ? el('tr', { class: 'dash-help', hidden: '' },
      el('td', { colspan: String(table.columns.length) }, el('div', { class: 'dash-bubble' }, row.help))) : null;
    const cells = row.cells.map((cell, index) => el('td', {}, String(cell),
      index === 0 && row.help ? el('span', { class: 'dash-info', 'aria-hidden': 'true' }, ' ⓘ') : null));
    rows.push(el('tr', {
      class: (row.level || '') + (row.lot || row.help ? ' clickable' : ''),
      title: row.help || null,
      onclick: row.lot ? () => openFiche('lot', row.lot) : bubble ? () => { bubble.hidden = !bubble.hidden; } : null,
    }, ...cells));
    if (bubble) rows.push(bubble);
  }
  return el('div', { class: 'dash-table' }, el('h5', {}, table.title),
    el('table', {}, el('thead', {}, el('tr', {}, ...table.columns.map((name) => el('th', {}, name)))),
      el('tbody', {}, ...rows)));
}

function dashSection(part) {
  const open = dash.open[part.key] !== undefined ? dash.open[part.key] : true;
  const body = [el('p', { class: 'hint' }, part.help), dashTiles(part.tiles)];
  if (part.lines.length) {
    body.push(el('ul', { class: 'dash-lines' }, ...part.lines.map((row) => el('li', { class: row.level || '' }, row.text))));
  }
  for (const chart of part.charts) { const node = dashChart(chart); if (node) body.push(node); }
  body.push(...part.tables.map(dashTable));
  const link = DASH_LINKS[part.link];
  if (link) {
    const local = !['purchases', 'consumables'].includes(part.link);
    body.push(local ? el('button', { type: 'button', class: 'text-link', onclick: () => dashGo(part.link) }, link + ' →')
      : el('p', { class: 'hint' }, link));
  }
  const score = part.score;
  return el('section', { class: 'dash-section', id: 'dash-' + part.key },
    el('button', { type: 'button', class: 'dash-head', 'aria-expanded': String(open),
      onclick: () => { dash.open[part.key] = !open; renderDashboard(); } },
    el('span', { class: 'fold' }, open ? '−' : '+'), el('h4', {}, `${part.emoji} ${part.title}`),
    score ? el('span', { class: 'dash-badge ' + (score.level || 'none') }, score.value == null ? '–' : String(score.value)) : null),
    open ? el('div', {}, ...body) : null);
}

function renderDashboard() {
  const view = $('#home-dashboard');
  if (!view) return;
  if (!canSeeStock()) {
    view.replaceChildren(el('p', {}, 'Réservé aux rôles gestion et admin : scannez votre badge.'));
    return;
  }
  const data = dash.data;
  const parts = [el('div', { class: 'row-head' }, el('h3', {}, 'Tableau de bord 📊'),
    el('div', { class: 'buttons' }, el('button', { type: 'button', onclick: loadDashboard }, icon('refresh'),
      dash.loading ? 'Chargement…' : 'Actualiser')))];
  if (dash.error) parts.push(el('p', { class: 'pin-error' }, dash.error));
  if (!data) {
    if (dash.loading) parts.push(el('p', { class: 'hint' }, 'Calcul du tableau de bord…'));
    view.replaceChildren(...parts);
    return;
  }
  parts.push(el('p', { class: 'hint' }, `Sur les 12 derniers mois (depuis le ${fmtDate(data.start)}). Scores sur 100 : `
    + 'vert à partir de 75, orange de 50 à 75, rouge en dessous.'));
  const scores = dashScores(data);
  if (scores) parts.push(scores);
  if (data.alerts.length) {
    parts.push(el('section', { class: 'dash-alerts' }, el('h4', {}, 'À surveiller'),
      el('ul', { class: 'dash-lines' }, ...data.alerts.map((row) => el('li', { class: row.level }, `${row.emoji} ${row.text}`)))));
  }
  parts.push(...data.sections.map(dashSection));
  view.replaceChildren(...parts);
}
