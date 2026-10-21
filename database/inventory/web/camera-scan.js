/* QRProtec - lecture des codes a la camera, limitee au viseur.
 *
 * Partage par le front web (app.js) et la page du telephone-douchette (scanner.js). BarcodeDetector natif quand il
 * lit les QR codes (Chrome Android) ; sinon zxing-cpp en WebAssembly dans un worker (decoder.js : Firefox, Safari
 * iOS), aussi efficace que le lecteur natif, avec jsQR et code128.js le temps qu'il se charge ou s'il ne se charge
 * pas. Dans tous les cas, seul un code dont le centre est dans le viseur est rendu : une autre etiquette visible a
 * l'ecran, a cote, n'est pas lue.
 */
'use strict';

(() => {
  const MARGIN = 0.1;      // tolerance autour du viseur (fraction de son cote) pour le centre d'un code
  const CROP_MARGIN = 0.2; // image donnee aux decodeurs : viseur elargi, pour un code un peu plus grand que lui
  const MAX_SIDE = 1024;   // image du viseur reduite au-dela ; les barres fines d'un code-barres restent lisibles
  const JSQR_SIDE = 640;   // jsQR (secours) : image plus petite pour rester fluide
  const WORKER_TIMEOUT_MS = 3000;

  // Lecteur pour la video `video` et le cadre du viseur `mark` (element affiche par-dessus la video).
  function create(video, mark) {
    const canvas = document.createElement('canvas');
    const context = canvas.getContext('2d', { willReadFrequently: true });
    let prepared = false;
    let detector = null;
    let nativeCode128 = false; // BarcodeDetector lit aussi le Code 128 (Chrome Android) : pas d'autre decodeur
    let worker = null;
    let workerReady = false;
    let workerFormats = [];
    const pending = new Map();
    let nextId = 0;

    // A appeler une fois la camera ouverte (getSupportedFormats peut demander la camera sur certains navigateurs)
    async function prepare() {
      if (prepared) return;
      prepared = true;
      if ('BarcodeDetector' in window) {
        try {
          const formats = await window.BarcodeDetector.getSupportedFormats();
          // QR codes et codes-barres Code 128 (etiquettes d'items des objets ronds, l'iid seul) ; codes-barres
          // seulement si le serveur les active (la page charge alors code128.js)
          const wanted = ['qr_code', ...(window.Code128 ? ['code_128'] : [])].filter((format) => formats.includes(format));
          if (wanted.includes('qr_code')) {
            detector = new window.BarcodeDetector({ formats: wanted });
            nativeCode128 = wanted.includes('code_128');
          }
        } catch (e) { detector = null; nativeCode128 = false; }
      }
      workerFormats = [...(detector ? [] : ['QRCode']), ...(window.Code128 && !nativeCode128 ? ['Code128'] : [])];
      if (workerFormats.length) startWorker();
    }

    function startWorker() {
      if (!window.Worker || !window.WebAssembly) return;
      try { worker = new Worker('web/decoder.js'); } catch (e) { worker = null; return; }
      worker.onmessage = ({ data }) => {
        if (data.ready) workerReady = true;
        else if (data.failed) stopWorker();
        else if (pending.has(data.id)) pending.get(data.id)(data.codes);
      };
      worker.onerror = stopWorker;
    }

    // module introuvable ou refuse : jsQR et code128.js restent
    function stopWorker() {
      if (worker) worker.terminate();
      worker = null;
      workerReady = false;
      pending.forEach((resolve) => resolve([]));
      pending.clear();
    }

    // Viseur en pixels de la video (affichee en object-fit: cover). Viseur cache (camera en vignette) : toute la
    // partie visible de l'image ; video cachee : le carre central.
    function zone() {
      const width = video.videoWidth;
      const height = video.videoHeight;
      const box = video.getBoundingClientRect();
      if (!box.width || !box.height) {
        const side = Math.min(width, height);
        return { x: (width - side) / 2, y: (height - side) / 2, w: side, h: side };
      }
      const scale = Math.max(box.width / width, box.height / height);
      const left = box.left + (box.width - width * scale) / 2;
      const top = box.top + (box.height - height * scale) / 2;
      const frame = mark ? mark.getBoundingClientRect() : null;
      const area = frame && frame.width && frame.height ? frame : box;
      return { x: (area.left - left) / scale, y: (area.top - top) / scale, w: area.width / scale, h: area.height / scale };
    }

    // centre du code dans le viseur (avec la tolerance)
    function inside(area, x, y) {
      const margin = MARGIN * Math.max(area.w, area.h);
      return x >= area.x - margin && x <= area.x + area.w + margin && y >= area.y - margin && y <= area.y + area.h + margin;
    }

    // code du viseur le plus proche de son centre (plusieurs etiquettes cote a cote)
    function pick(area, codes) {
      const cx = area.x + area.w / 2;
      const cy = area.y + area.h / 2;
      const distance = (code) => (code.x - cx) ** 2 + (code.y - cy) ** 2;
      const kept = codes.filter((code) => code.text && inside(area, code.x, code.y)).sort((a, b) => distance(a) - distance(b));
      return kept.length ? kept[0].text : null;
    }

    // image du viseur elargi, dans `canvas`, au plus `maxSide` pixels de cote
    function crop(area, maxSide) {
      const margin = CROP_MARGIN * Math.max(area.w, area.h);
      const sx = Math.max(0, Math.floor(area.x - margin));
      const sy = Math.max(0, Math.floor(area.y - margin));
      const sw = Math.min(video.videoWidth, Math.ceil(area.x + area.w + margin)) - sx;
      const sh = Math.min(video.videoHeight, Math.ceil(area.y + area.h + margin)) - sy;
      const scale = Math.min(1, maxSide / Math.max(sw, sh));
      canvas.width = Math.max(1, Math.round(sw * scale));
      canvas.height = Math.max(1, Math.round(sh * scale));
      context.drawImage(video, sx, sy, sw, sh, 0, 0, canvas.width, canvas.height);
      // coordonnees de l'image vers celles de la video
      return { x: (x) => sx + x / scale, y: (y) => sy + y / scale };
    }

    function decode(formats, area) {
      const map = crop(area, MAX_SIDE);
      const image = context.getImageData(0, 0, canvas.width, canvas.height);
      const id = ++nextId;
      return new Promise((resolve) => {
        // worker bloque : l'image est abandonnee, la suivante repart
        const timer = setTimeout(() => { pending.delete(id); resolve([]); }, WORKER_TIMEOUT_MS);
        pending.set(id, (codes) => { clearTimeout(timer); pending.delete(id); resolve(codes); });
        worker.postMessage({ id, buffer: image.data.buffer, width: image.width, height: image.height, formats }, [image.data.buffer]);
      }).then((codes) => codes.map((code) => ({ text: code.text, x: map.x(code.x), y: map.y(code.y) })));
    }

    function detectQr(area) {
      const map = crop(area, JSQR_SIDE);
      const image = context.getImageData(0, 0, canvas.width, canvas.height);
      const result = window.jsQR(image.data, image.width, image.height, { inversionAttempts: 'attemptBoth' });
      if (!result) return null;
      const corners = ['topLeftCorner', 'topRightCorner', 'bottomLeftCorner', 'bottomRightCorner'].map((key) => result.location[key]);
      const x = corners.reduce((sum, point) => sum + point.x, 0) / 4;
      const y = corners.reduce((sum, point) => sum + point.y, 0) / 4;
      return pick(area, [{ text: result.data, x: map.x(x), y: map.y(y) }]);
    }

    // Code lu dans le viseur sur l'image courante, sinon null
    async function detect() {
      if (video.readyState < 2 || !video.videoWidth || !video.videoHeight) return null;
      const area = zone();
      if (detector) {
        const codes = await detector.detect(video);
        const code = pick(area, codes.map((found) => ({
          text: found.rawValue,
          x: found.boundingBox.x + found.boundingBox.width / 2,
          y: found.boundingBox.y + found.boundingBox.height / 2,
        })));
        if (code || !workerFormats.length) return code;
      }
      if (workerReady) return pick(area, await decode(workerFormats, area));
      // worker en cours de chargement ou indisponible : jsQR, puis le decodeur Code 128 en JS (pleine resolution)
      const qr = !detector && window.jsQR ? detectQr(area) : null;
      if (qr) return qr;
      if (!window.Code128) return null;
      crop(area, MAX_SIDE);
      return window.Code128.scan(canvas);
    }

    // pause entre deux images : le worker ne bloque pas la page, jsQR si
    const delay = () => (detector ? 120 : workerReady ? 60 : 180);

    return { prepare, detect, delay };
  }

  window.CameraScan = { create };
})();
