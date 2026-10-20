/* QRProtec - lecture des codes-barres Code 128 a la camera, sans dependance.
 *
 * Les etiquettes d'items des types « étiquette code-barres » (objets ronds, ou un QR deforme se lit mal) portent
 * un Code 128 qui contient l'iid seul. BarcodeDetector le lit nativement sur Chrome Android ; ailleurs (Firefox,
 * Safari iOS) ce decodeur prend le relais : quelques lignes horizontales et verticales de l'image en pleine
 * resolution, seuil local, largeurs des barres mesurees au sous-pixel et symboles reconnus par leurs distances
 * de bord a bord (insensibles a l'etalement de l'encre). Un code n'est rendu que s'il est lu, somme de controle
 * comprise, sur deux lignes au moins.
 *
 * Partage par le front web (app.js) et la page du telephone-douchette (scanner.js). Meme table que
 * kCode128Patterns dans app/src/core/code128.cpp (encodeur du poste).
 */
'use strict';

(() => {
  // largeurs barre/espace des 107 symboles (106 : stop, dont les 6 premieres largeurs suffisent a le reconnaitre)
  const PATTERNS = [
    '212222', '222122', '222221', '121223', '121322', '131222', '122213', '122312', '132212', '221213', '221312',
    '231212', '112232', '122132', '122231', '113222', '123122', '123221', '223211', '221132', '221231', '213212',
    '223112', '312131', '311222', '321122', '321221', '312212', '322112', '322211', '212123', '212321', '232121',
    '111323', '131123', '131321', '112313', '132113', '132311', '211313', '231113', '231311', '112133', '112331',
    '132131', '113123', '113321', '133121', '313121', '211331', '231131', '213113', '213311', '213131', '311123',
    '311321', '331121', '312113', '312311', '332111', '314111', '221411', '431111', '111224', '111422', '121124',
    '121421', '141122', '141221', '112214', '112412', '122114', '122411', '142112', '142211', '241211', '221114',
    '413111', '241112', '134111', '111242', '121142', '121241', '114212', '124112', '124211', '411212', '421112',
    '421211', '212141', '214121', '412121', '111143', '111341', '131141', '114113', '114311', '411113', '411311',
    '113141', '114131', '311141', '411131', '211412', '211214', '211232', '2331112',
  ];
  const STOP = 106;
  const START_A = 103;

  // symbole d'apres ses 4 distances de bord a bord (barre + espace suivant, espace + barre suivante...), en modules
  const BY_EDGES = new Map();
  PATTERNS.forEach((pattern, value) => {
    const w = [...pattern.slice(0, 6)].map(Number);
    BY_EDGES.set(edgesKey(w), value);
  });

  function edgesKey(widths, total = 11) {
    let key = '';
    for (let k = 0; k < 4; k++) key += Math.round((widths[k] + widths[k + 1]) * 11 / total);
    return key;
  }

  // Bords d'une ligne de luminance : seuil local, position au sous-pixel.
  // Rend les largeurs successives et la couleur de la premiere (true = barre).
  function runsOf(line, radius) {
    const n = line.length;
    // seuil = moyenne locale (sommes cumulees) : une barre fine adoucie par le flou passe encore dessous, alors
    // qu'elle n'atteindrait pas le milieu du min et du max
    const sums = new Float64Array(n + 1);
    for (let x = 0; x < n; x++) sums[x + 1] = sums[x] + line[x];
    // contraste de la ligne (5e au 95e centile) : une fenetre bien moins contrastee est du fond (bruit du capteur)
    const sorted = Uint8Array.from(line).sort();
    const flat = Math.max(24, 0.35 * (sorted[Math.floor(n * 0.95)] - sorted[Math.floor(n * 0.05)]));
    const diff = new Float32Array(n);
    for (let x = 0; x < n; x++) {
      const from = Math.max(0, x - radius), to = Math.min(n, x + radius + 1);
      let low = 255, high = 0;
      for (let k = from; k < to; k += 2) {
        if (line[k] < low) low = line[k];
        if (line[k] > high) high = line[k];
      }
      // zone sans contraste : fond blanc
      diff[x] = high - low < flat ? 1 : line[x] - (sums[to] - sums[from]) / (to - from);
    }
    const edges = [];
    for (let x = 0; x + 1 < n; x++) {
      if ((diff[x] < 0) !== (diff[x + 1] < 0)) edges.push(x + diff[x] / (diff[x] - diff[x + 1]));
    }
    const runs = [];
    for (let i = 0; i + 1 < edges.length; i++) runs.push(edges[i + 1] - edges[i]);
    return { runs, firstBar: edges.length > 0 && diff[Math.ceil(edges[0])] < 0 };
  }

  // Autre lecture des bords, pour une image floue : les extremums de la derivee (le flou adoucit une barre fine au
  // point qu'elle ne passe plus sous le seuil, mais ses deux bords restent des pentes). Bords alternes : une
  // descente (debut de barre) puis une montee ; de deux pentes de meme sens, la plus forte.
  function runsOfSlopes(line) {
    const n = line.length;
    const slope = new Float32Array(n);
    for (let x = 1; x + 1 < n; x++) slope[x] = (line[x + 1] - line[x - 1]) / 2;
    const sorted = Uint8Array.from(line).sort();
    const minimum = Math.max(6, 0.12 * (sorted[Math.floor(n * 0.95)] - sorted[Math.floor(n * 0.05)]));
    const edges = []; // [position, signe]
    for (let x = 2; x + 2 < n; x++) {
      const value = slope[x];
      if (Math.abs(value) < minimum) continue;
      const peak = value < 0 ? value <= slope[x - 1] && value < slope[x + 1] : value >= slope[x - 1] && value > slope[x + 1];
      if (!peak) continue;
      // sommet de la parabole par les trois points
      const a = slope[x - 1], c = slope[x + 1], denominator = a - 2 * value + c;
      const position = x + (denominator ? (a - c) / (2 * denominator) : 0);
      const sign = value < 0 ? -1 : 1;
      const last = edges[edges.length - 1];
      if (last && last[1] === sign) {
        if (Math.abs(value) > Math.abs(last[2])) edges[edges.length - 1] = [position, sign, value];
      } else edges.push([position, sign, value]);
    }
    const runs = [];
    for (let i = 0; i + 1 < edges.length; i++) runs.push(edges[i + 1][0] - edges[i][0]);
    return { runs, firstBar: edges.length > 0 && edges[0][1] < 0 };
  }

  // Valeurs des symboles -> texte (jeux A, B, C, Shift ; FNC ignores). null si incoherent.
  function text(values) {
    let set = values[0] - START_A; // 0 = A, 1 = B, 2 = C
    let out = '';
    for (let i = 1; i < values.length; i++) {
      const v = values[i];
      let current = set;
      if (v === 98 && set !== 2 && i + 1 < values.length) { current = set === 0 ? 1 : 0; i++; } // Shift
      const value = values[i];
      if (current === 2) {
        if (value < 100) out += String(value).padStart(2, '0');
        else if (value === 100) set = 1;
        else if (value === 101) set = 0;
        else if (value !== 102) return null;
        continue;
      }
      if (value < 96) {
        if (current === 1 || value < 64) out += String.fromCharCode(value + 32);
        else out += String.fromCharCode(value - 64);
      } else if (value === 99) set = 2;
      else if (value === 100 && current === 0) set = 1;
      else if (value === 101 && current === 1) set = 0;
      else if (value === STOP || value > 102) return null;
      // FNC1 a FNC4 : ignores
    }
    return out;
  }

  // Code lu a partir de la barre `start` (premiere barre du symbole de depart), ou null.
  function decodeAt(runs, start) {
    const values = [];
    let previous = 0;
    for (let i = start; i + 6 <= runs.length; i += 6) {
      const widths = runs.slice(i, i + 6);
      const total = widths.reduce((a, b) => a + b, 0);
      if (previous && (total < previous * 0.75 || total > previous * 1.33)) return null; // largeur incoherente
      previous = total;
      const value = BY_EDGES.get(edgesKey(widths, total));
      if (value === undefined) return null;
      if (values.length === 0 && (value < START_A || value > 105)) return null;
      if (values.length > 0 && value >= START_A && value !== STOP) return null;
      values.push(value);
      if (value === STOP) break;
    }
    if (values.length < 4 || values[values.length - 1] !== STOP) return null;
    const data = values.slice(0, -2);
    const check = data.reduce((sum, value, index) => sum + value * Math.max(index, 1), 0) % 103;
    if (check !== values[values.length - 2]) return null;
    return text(data);
  }

  function decodeRuns(runs, firstBar) {
    for (let i = firstBar ? 0 : 1; i + 6 <= runs.length; i += 2) {
      const symbol = runs.slice(i, i + 6).reduce((a, b) => a + b, 0);
      // marge blanche devant le code (la norme en demande 10 modules, on se contente de 4)
      if (i > 0 && runs[i - 1] < symbol / 11 * 4) continue;
      const result = decodeAt(runs, i);
      if (result) return result;
    }
    return null;
  }

  function decodeLine(line) {
    const reversed = line.slice().reverse(); // code a l'envers (etiquette tete en bas)
    // fenetre du seuil : quelques modules quand le code est loin, plus large de pres
    for (const source of [line, reversed]) {
      for (const read of [runsOfSlopes(source), runsOf(source, 8), runsOf(source, 16), runsOf(source, 32)]) {
        const result = decodeRuns(read.runs, read.firstBar);
        if (result) return result;
      }
    }
    return null;
  }

  const canvas = document.createElement('canvas');
  const context = canvas.getContext('2d', { willReadFrequently: true });
  const FRACTIONS = [0.5, 0.44, 0.56, 0.38, 0.62, 0.32, 0.68, 0.26, 0.74, 0.2, 0.8];

  // Lit un Code 128 dans l'image courante de la video (ou d'un canvas / d'une image), sinon null.
  function scan(source) {
    const width0 = source.videoWidth || source.naturalWidth || source.width;
    const height0 = source.videoHeight || source.naturalHeight || source.height;
    if (!width0 || !height0) return null;
    // pleine resolution : les barres les plus fines font 1 a 2 pixels de la camera
    const scale = Math.min(1, 1920 / Math.max(width0, height0));
    const width = Math.round(width0 * scale);
    const height = Math.round(height0 * scale);
    canvas.width = width;
    canvas.height = height;
    context.drawImage(source, 0, 0, width, height);
    // seules les lignes lues sont copiees (bandes de 3 pixels moyennees pour lisser le bruit)
    const band = (x, y, w, h, length, step) => {
      const pixels = context.getImageData(x, y, w, h).data;
      const line = new Uint8Array(length);
      for (let i = 0; i < length; i++) {
        let sum = 0;
        for (let k = 0; k < 3; k++) {
          const p = (i * step + k * (step === 1 ? length : 1)) * 4;
          sum += (pixels[p] * 77 + pixels[p + 1] * 150 + pixels[p + 2] * 29) >> 8;
        }
        line[i] = sum / 3;
      }
      return line;
    };
    const seen = new Map();
    const found = (code) => {
      if (code === null || code === '') return null;
      const count = (seen.get(code) || 0) + 1;
      seen.set(code, count);
      return count >= 2 ? code : null;
    };
    for (const fraction of FRACTIONS) {
      // ligne horizontale (code tenu a plat) : 3 lignes de `width` pixels
      const y = Math.min(height - 2, Math.max(1, Math.round(height * fraction)));
      const horizontal = found(decodeLine(band(0, y - 1, width, 3, width, 1)));
      if (horizontal) return horizontal;
      // ligne verticale (code tenu debout) : `height` lignes de 3 pixels
      const x = Math.min(width - 2, Math.max(1, Math.round(width * fraction)));
      const vertical = found(decodeLine(band(x - 1, 0, 3, height, height, 3)));
      if (vertical) return vertical;
    }
    return null;
  }

  window.Code128 = { scan, decodeLine, PATTERNS };
})();
