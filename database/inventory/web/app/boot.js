/* QRProtec - front web : demarrage de la page (dernier script charge). */
'use strict';

// Demarrage : l'URL peut venir d'un QR code (verif?lot=..&key=.., badge?m=..&key=.., item?id=.., pack?id=..)

async function boot() {
  restore();
  render();
  // reglage du serveur : identite declaree (nom sans badge) acceptee ou non
  api('health/').then((health) => {
    state.declaredAllowed = !!health.declared_identity;
    state.antennaName = health.antenna_name || ''; // case « Certifié par le TECH ... » des rapports d'etat
    state.restockRecommends = health.restock_verif_recommended !== false;
    render();
    if (fiche.lot) renderFiche(); // encadre de l'etiquette cle
  }).catch(() => {});
  const route = location.pathname.replace(/\/+$/, '').split('/').pop();
  const params = new URLSearchParams(location.search);
  // on retire la cle de la barre d'adresse (historique, partage d'ecran)
  const cleanUrl = (lot) => history.replaceState(null, '', lot ? `verif?lot=${encodeURIComponent(lot)}` : 'verif');
  // etiquette scannee avec l'appareil photo : ecran de scan (sans demarrer la camera, il faut un geste)
  const qrRoute = ['verif', 'seal', 'unseal', 'pinreset', 'item', 'task', 'pack'].includes(route) && [...params.keys()].length > 0;
  if (qrRoute) { document.body.classList.remove('home-mode'); state.homePage = 'scan'; }
  if (route === 'verif' && params.get('lot')) {
    // etiquette publique ou privee d'un lot : fiche du lot, « Lancer une vérif » ouvre le lecteur (la cle est
    // gardee pour cet onglet, retiree de l'adresse)
    const id = params.get('lot');
    cleanUrl(id);
    if (state.lotId) await loadLot(state.lotId); // vérif en cours (rejointe si c'est un lot du meme lot global)
    await openFiche('lot', id, { key: params.get('key') || '', push: false, label: true });
  } else if (route === 'badge' && params.get('m')) {
    // carte scannee : page d'accueil
    history.replaceState(null, '', './');
    showHome('menu', false);
    if (params.get('t')) await tempLogin(params.get('m'), params.get('t'));
    else await login(params.get('m'), params.get('key') || '');
  } else if (route === 'seal' && params.get('lot') && params.get('s')) {
    // etiquette du scelle : fiche du lot scelle (sans vérif), ou scan « scellé intact » de la vérif en cours
    if (state.lotId) await loadLot(state.lotId);
    if (checkRunning() || allScanned().length) {
      await scanSeal({ kind: 'seal', code: location.href, id: params.get('lot'), key: params.get('s') });
      cleanUrl(params.get('lot'));
    } else {
      await openFiche('lot', params.get('lot'), { seal: params.get('s'), push: false });
    }
  } else if (route === 'unseal' && params.get('lot') && params.get('c')) {
    cleanUrl(params.get('lot'));
    await openSeal({ kind: 'sealopen', code: location.href, id: params.get('lot'), key: params.get('c') }, { fromUrl: true });
  } else if (route === 'pinreset' && params.get('m') && params.get('t')) {
    cleanUrl(state.lotId);
    if (state.lotId) await loadLot(state.lotId);
    await startPinReset(params.get('m'), params.get('t'));
  } else if ((route === 'item' || route === 'task') && params.get('id')) {
    // QR d'un item scanne avec l'appareil photo : fiche de l'item et de son dernier emplacement connu
    if (state.lotId) await loadLot(state.lotId);
    const scan = parseCode(params.get('id'));
    if (scan.kind === 'item') await openFiche('item', scan.id, { push: false });
    else { cleanUrl(state.lotId); feedback.bad(); showInfo('bad', 'Code non reconnu', params.get('id')); }
  } else if (route === 'pack' && params.get('id')) {
    cleanUrl(state.lotId);
    if (state.lotId) await loadLot(state.lotId);
    await scanPack({ kind: 'pack', code: location.href, id: params.get('id') });
  } else if (route === 'liste' && params.get('id')) {
    // lien ou QR code d'une liste de materiel : la liste, a verifier en direct (connexion demandee)
    openList(params.get('id'), false);
  } else if (route === 'verif') {
    // lecteur (adresse du lecteur ouvert depuis une fiche), rechargement compris
    enterScanner(false, false);
  } else {
    // page d'accueil, ou la page de son ancre (#stock : depuis une notification de stock, #scan : scanner)
    const page = pageFromHash(location.hash);
    if (page === 'scan') enterScanner(false, false);
    else showHome((page === 'stock' && !canSeeStock()) || (['reports', 'lists'].includes(page) && !state.user)
      || (page === 'debug' && !isAdmin()) || (page === 'stats' && !canSeeStock()) ? 'menu' : page, false);
  }
  if (state.expiredSession && !state.lot) toast('Vérif précédente oubliée : plus de 20 minutes sans activité.');
  render();
  if (state.user && canSeeStock()) loadStock();
  if (state.user) loadReports(true);
  registerAppWorker();
}

// Appli ouverte sans reseau : le service worker app-sw.js garde la page et ses fichiers (toujours le reseau d'abord,
// la copie seulement sans reseau), et le cache de vérif est mis a jour en arriere-plan pour qu'une vérif lancee
// hors connexion le trouve. Il faut une connexion securisee (HTTPS avec un certificat reconnu par le telephone).
function registerAppWorker() {
  if (!window.isSecureContext || !('serviceWorker' in navigator)) return;
  navigator.serviceWorker.register('app-sw.js', { scope: './' }).catch(() => { /* certificat refuse, navigation privee */ });
  setTimeout(() => vcache.refresh(APP_CACHE_AGE), 3000);
}

boot();
