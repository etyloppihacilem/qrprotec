/* QRProtec - worker de lecture des QR codes et codes-barres (zxing-cpp en WebAssembly).
 *
 * Utilise par camera-scan.js quand le navigateur n'a pas BarcodeDetector (Firefox, Safari iOS) : meme moteur que
 * les lecteurs natifs (lecture inclinee, floue, inversee, petits codes), et dans un worker pour que le decodage
 * ne bloque jamais la page. Recoit l'image du viseur, rend les codes lus et leur centre dans cette image.
 */
'use strict';

// nom versionne : le module (~1 Mo) est garde en cache par le navigateur (web_views.py)
const WASM = 'vendor/zxing-reader-3.1.6.wasm';

importScripts('vendor/zxing-reader.js');

self.ZXingWASM.prepareZXingModule({
  overrides: { locateFile: (path, prefix) => (path.endsWith('.wasm') ? new URL(WASM, self.location).href : prefix + path) },
  fireImmediately: true,
}).then(() => self.postMessage({ ready: true }), () => self.postMessage({ failed: true }));

self.onmessage = async ({ data }) => {
  let codes = [];
  try {
    const results = await self.ZXingWASM.readBarcodes(
      { data: new Uint8ClampedArray(data.buffer), width: data.width, height: data.height },
      { formats: data.formats, tryHarder: true, maxNumberOfSymbols: 4 },
    );
    codes = results.filter((result) => result.isValid && result.text).map((result) => {
      const { topLeft, topRight, bottomLeft, bottomRight } = result.position;
      return {
        text: result.text,
        x: (topLeft.x + topRight.x + bottomLeft.x + bottomRight.x) / 4,
        y: (topLeft.y + topRight.y + bottomLeft.y + bottomRight.y) / 4,
      };
    });
  } catch (e) { /* image illisible */ }
  self.postMessage({ id: data.id, codes });
};
