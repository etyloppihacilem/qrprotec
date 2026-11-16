/* QRProtec - service worker de l'appli : l'ouvrir sans reseau (reserve sans wifi ni reseau mobile).
 *
 * Servi a la racine par web_views.app_service_worker, qui ecrit dedans la version du front et la liste des fichiers
 * (web_views.offline_files : la page et tout ASSETS). Une mise a jour du paquet change la version : le navigateur
 * installe ce worker de nouveau, il recharge tous les fichiers d'un coup dans un cache neuf et efface l'ancien.
 *
 * Toujours le reseau d'abord (ETag : 304 sans corps), jamais une version perimee tant que le serveur repond : la copie
 * gardee sert seulement quand le reseau manque (pas de reseau, delai depasse, serveur arrete ou en maintenance).
 * Une page ouverte depuis la copie charge aussi ses scripts depuis la copie, pour ne pas melanger deux versions.
 * L'API (api/), les WebSockets et la page de la douchette (scanner) ne passent jamais par ici.
 *
 * Le service worker des notifications (web/sw.js, portee web/) reste a part : ses abonnements ne bougent pas.
 */
'use strict';

const VERSION = '__VERSION__';
const FILES = ['__FILES__'];
const PAGES = ['__PAGES__'];
const CACHE = 'qrprotec-app-' + VERSION;
const PAGE_TIMEOUT = 4000; // reseau faible : au-dela, la copie gardee de la page
const ASSET_TIMEOUT = 8000;

const scope = () => self.registration.scope;
const cacheKey = (path) => new URL(path, scope()).href;
const offlineClients = new Set(); // pages ouvertes depuis la copie : leurs fichiers aussi depuis la copie

self.addEventListener('install', (event) => {
  event.waitUntil((async () => {
    const cache = await caches.open(CACHE);
    // tout ou rien : un fichier manquant fait echouer l'installation, l'ancienne version reste en place
    await cache.addAll(FILES.map((path) => new Request(cacheKey(path), { cache: 'no-cache' })));
    await self.skipWaiting();
  })());
});

self.addEventListener('activate', (event) => {
  event.waitUntil((async () => {
    for (const name of await caches.keys()) {
      if (name.startsWith('qrprotec-app-') && name !== CACHE) await caches.delete(name);
    }
    await self.clients.claim();
  })());
});

// Reponse du reseau, ou undefined au-dela du delai (la requete continue et met a jour la copie si elle aboutit)
function network(request, timeout, store) {
  const fetching = fetch(request).then(async (response) => {
    if (response.ok && response.status === 200) await store(response.clone()).catch(() => {});
    return response;
  });
  if (!timeout) return fetching;
  fetching.catch(() => {}); // echec apres le delai : la copie a deja repondu
  return Promise.race([fetching, new Promise((resolve) => setTimeout(resolve, timeout))]);
}

// Une reponse issue d'une redirection ne peut pas repondre a une navigation : copie sans ce drapeau
async function storable(response) {
  if (!response.redirected) return response;
  return new Response(await response.blob(), { status: response.status, statusText: response.statusText, headers: response.headers });
}

async function put(key, response) {
  const cache = await caches.open(CACHE);
  await cache.put(key, await storable(response));
}

async function cached(key) {
  const cache = await caches.open(CACHE);
  return cache.match(key, { ignoreVary: true });
}

const unavailable = (response) => !response || [502, 503, 504].includes(response.status);

async function page(event) {
  const key = cacheKey('./');
  const copy = await cached(key);
  let response;
  try {
    response = await network(event.request, copy ? PAGE_TIMEOUT : 0, (fresh) => put(key, fresh));
  } catch (e) {
    response = null;
  }
  if (!unavailable(response) || !copy) return response || Response.error();
  if (event.resultingClientId) offlineClients.add(event.resultingClientId);
  return storable(copy);
}

async function asset(event, path) {
  const key = cacheKey(path);
  const copy = await cached(key);
  if (copy && offlineClients.has(event.clientId)) return copy;
  let response;
  try {
    response = await network(event.request, copy ? ASSET_TIMEOUT : 0, (fresh) => put(key, fresh));
  } catch (e) {
    response = null;
  }
  if (!unavailable(response) || !copy) return response || Response.error();
  return copy;
}

self.addEventListener('fetch', (event) => {
  const request = event.request;
  if (request.method !== 'GET' || !request.url.startsWith(scope())) return;
  const url = new URL(request.url);
  const path = url.pathname.slice(new URL(scope()).pathname.length);
  if (request.mode === 'navigate') {
    if (PAGES.includes(path)) event.respondWith(page(event));
  } else if (FILES.includes(path) && !url.search) {
    event.respondWith(asset(event, path));
  }
});
