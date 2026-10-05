/* QRProtec - easter egg : un code contenant « douchette » n'est pas un scan, une phrase s'affiche.
 *
 * Partage par le front web (app.js) et la page du telephone-douchette (scanner.js). La liste des
 * phrases est la meme que douchette_phrases() dans app/src/core/codes.cpp (verifie par les tests).
 */
'use strict';

(() => {
  const PHRASES = [
    'Pourquoi, au nom du Graal, vous avez scanné la douchette ?',
    "C'est pas faux. Enfin si : ça, c'est la douchette.",
    'Scanner la douchette avec la douchette ? On en a gros !',
    'Faut pas respirer la compote, ça fait tousser. Et faut pas scanner la douchette.',
    "Le gras, c'est la vie. La douchette, c'est pas un produit.",
    'Douchette ! Ça vaut combien au cul de chouette ? Zéro.',
    "Merlin, c'est encore vous qui avez scanné la douchette ?",
    "Sire, on a un problème : quelqu'un a scanné la douchette.",
    'Vous avez scanné la douchette. La douchette vous scanne en retour.',
    'Inventaire : 1 douchette, état : très perplexe.',
    "La douchette, c'est pour scanner. C'est pas elle qu'on scanne.",
    'Et après, on scanne le lecteur de badge ?',
  ];
  let last = -1;

  function matches(code) {
    return typeof code === 'string' && code.toLowerCase().includes('douchette');
  }

  // tirage sans repetir la phrase precedente
  function pick() {
    let index = Math.floor(Math.random() * (PHRASES.length - 1));
    if (index >= last) index += 1;
    last = index;
    return PHRASES[index];
  }

  function show() {
    let dialog = document.getElementById('douchette');
    if (!dialog) {
      dialog = document.createElement('dialog');
      dialog.id = 'douchette';
      dialog.innerHTML = '<h2>Euh…</h2><p></p>'
        + '<div class="dialog-buttons"><button type="button" class="primary">Pardon</button></div>';
      dialog.querySelector('button').addEventListener('click', () => dialog.close());
      document.body.appendChild(dialog);
    }
    dialog.querySelector('p').textContent = pick();
    if (!dialog.open) dialog.showModal();
  }

  window.douchette = { matches, show };
})();
