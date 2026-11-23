/* QRProtec - front web : hauteur de la fenetre visible (index.html et scanner.html). */
'use strict';

// Appli installee (iPhone, Android) : 100dvh ne suit pas toujours la fenetre. Apres le clavier, une rotation ou un
// retour dans l'appli, il pouvait rester plus haut que la page (html, body en overflow: hidden) et le bas des pages
// etait coupe : impossible de defiler jusqu'au bout (ex. la version en bas des Reglages). La hauteur reelle
// (innerHeight, que le clavier ne change pas sur iPhone ni sur Chrome Android) est ecrite dans --app-h, que les
// pages plein ecran d'app.css utilisent ; la page est aussi remise en haut si le navigateur l'a decalee.
(function () {
  let frame = 0;
  function sync() {
    frame = 0;
    document.documentElement.style.setProperty('--app-h', `${window.innerHeight}px`);
    // body est en overflow: hidden : un decalage laisse par le clavier ne se rattrape pas au doigt
    const keyboard = window.visualViewport && window.innerHeight - window.visualViewport.height > 80;
    if (!keyboard && (window.scrollY || document.documentElement.scrollTop)) window.scrollTo(0, 0);
  }
  function later() { if (!frame) frame = requestAnimationFrame(sync); }
  sync();
  window.addEventListener('resize', later);
  window.addEventListener('orientationchange', () => setTimeout(sync, 300));
  window.addEventListener('pageshow', later);
  document.addEventListener('visibilitychange', () => { if (!document.hidden) later(); });
  // clavier ferme : iOS ne previent pas toujours par un resize de la fenetre
  document.addEventListener('focusout', () => setTimeout(sync, 100));
  if (window.visualViewport) window.visualViewport.addEventListener('resize', later);
})();
