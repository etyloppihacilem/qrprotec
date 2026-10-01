/* QRProtec - service worker des notifications web (stock bas, stock vide).
 *
 * Enregistre seulement quand un admin active les notifications (onglet Stock). Portee web/ : il ne
 * controle aucune page et n'intercepte aucune requete, il ne fait qu'afficher les notifications.
 */
'use strict';

const ICON = new URL('icon-192.png', self.location).href;

self.addEventListener('install', () => self.skipWaiting());
self.addEventListener('activate', (event) => event.waitUntil(self.clients.claim()));

self.addEventListener('push', (event) => {
  let data = {};
  try {
    data = event.data ? event.data.json() : {};
  } catch (e) {
    data = { body: event.data ? event.data.text() : '' };
  }
  event.waitUntil(self.registration.showNotification(data.title || 'QRProtec', {
    body: data.body || '',
    icon: ICON,
    badge: ICON,
    tag: data.tag || undefined,
    renotify: !!data.tag,
    data: { url: new URL(data.url || '../', self.location).href },
  }));
});

self.addEventListener('notificationclick', (event) => {
  event.notification.close();
  const url = event.notification.data && event.notification.data.url;
  event.waitUntil((async () => {
    const windows = await self.clients.matchAll({ type: 'window', includeUncontrolled: true });
    const open = windows.find((client) => new URL(client.url).origin === self.location.origin);
    if (open) {
      open.postMessage({ tab: 'stock' }); // la page affiche l'onglet Stock (app.js)
      return open.focus();
    }
    return self.clients.openWindow(url || '../');
  })());
});
