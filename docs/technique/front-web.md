# Front web (téléphone)

Dossier `database/inventory/web/`, servi par le back (`inventory/web_views.py`). HTML, CSS et
JavaScript **sans framework ni étape de construction** : on modifie le fichier, on recharge la page.

| Fichier | Rôle |
|---|---|
| `index.html`, `app.js`, `app.css` | Page unique de vérif et de consultation |
| `scanner.html`, `scanner.js` | Page « téléphone comme douchette du poste » |
| `sw.js` | Service worker, uniquement pour afficher les notifications web |
| `manifest.webmanifest`, `icon-*.png`, `favicon.png` | Ajout à l'écran d'accueil, icônes (`make icon`) |
| `vendor/jsQR.js` | Décodeur QR de secours (Apache 2.0), quand `BarcodeDetector` manque |

Pourquoi pas de framework : la page doit fonctionner hors de tout réseau Internet (réseau local de
la borne, pas de CDN), rester modifiable dans cinq ans sans chaîne d'outils npm qui aura vieilli, et
elle tient en un fichier JS lisible. Voir [decisions.md](decisions.md#front-web-sans-framework).

## Adresses

La page répond aux adresses mêmes des QR codes : `/`, `/verif?lot=…[&key=…]`, `/badge?m=…&key=…`,
`/pack?id=…`, `/seal?lot=…&s=…`. Scanner une étiquette avec l'appareil photo natif du téléphone ouvre
donc directement la bonne vue. Les fichiers sont référencés en relatif (`web/app.js`) et l'API par
`new URL('api/', document.baseURI)` : la page fonctionne aussi sous un sous-chemin.

Le back ne sert que les fichiers déclarés dans `web_views.ASSETS` (liste blanche). **Ajouter un
fichier au front web = l'ajouter à cette liste**, sinon 404.

La caméra n'est accessible qu'en HTTPS (ou sur `localhost`) : en production Caddy fournit le
certificat ; en développement, `manage.py serve --https`.

## Structure de `app.js`

Un seul module (IIFE) avec un objet `state` et des fonctions `render*` qui reconstruisent les vues à
partir de l'état (pas de DOM virtuel, des petits morceaux recréés avec l'aide `el(tag, attrs,
...enfants)`).

- **État** : utilisateur (badge), lot en cours et sa clé, lots ajoutés (`extra`, vérif groupée),
  liste des scans, onglet, dernière vérif. Conservé dans `localStorage` (`save`/`restore`) : un
  rechargement de page ne perd pas une vérif en cours. La clé présente dans l'URL est retirée de la
  barre d'adresse (`history.replaceState`).
- **Caméra** (`startCamera`, `scanLoop`, `detect`) : caméra arrière, `BarcodeDetector` natif si
  disponible, sinon jsQR sur une image du `<canvas>`. Un même code vu en continu n'est traité qu'une
  fois par 2,5 s. Lampe et écran maintenu allumé (`wakeLock`) si possible.
- **Analyse** (`parseCode`) : même format que `app/src/core/codes.cpp`, domaine ignoré.
- **Retours** : bip (Web Audio, déverrouillé au premier toucher), vibration (sauf iPhone), flash
  rouge plein écran pour un périmé ou un code inconnu.
- **Vérif** : `planSession` (répartition des items entre les lots, voir
  [regles-de-gestion.md](regles-de-gestion.md#répartition-des-items-entre-les-lots)), `validate`
  (`POST api/verifs/` avec les clés), `addToLot` (réassort), `showReport` (compte rendu).
- **Connexion** : `login` (`POST api/auth/`, PIN via `askPin`), jeton de session conservé pour les
  lectures réservées.
- **Accueil** (`renderHome`) : liste des lots et accès à la douchette du poste ; **Stock**
  (`renderStock`) pour les rôles gestion/admin ; **gestionnaire des notifications** (menu > Notifications, `renderNotifications`) pour les rôles
  gestion/admin : activation sur cet appareil, puis alertes et retrait de chacun de ses appareils.

Toutes les écritures passent par l'**API publique** : la vérif exige la clé du lot (étiquette
privée) et un badge.

## Service worker

`sw.js` est enregistré **seulement** quand un admin active les notifications. Sa portée est `web/` :
il ne contrôle aucune page et n'intercepte aucune requête (pas de cache hors ligne, pas de risque de
servir une vieille version de l'application). Il affiche la notification et, au clic, ouvre ou
ramène la page sur l'onglet Stock.

## Page « douchette du poste »

`scanner.html` / `scanner.js` : ouverte par le QR affiché sur le poste
(`/scanner?s=SESSION&k=CLÉ`). Elle se connecte en WebSocket à `/ws/scanner/phone`, envoie chaque code
lu, affiche un libellé court (jamais les clés), restitue les erreurs renvoyées par le poste, et se
reconnecte seule. « Annuler le dernier » retire du poste le dernier scan venu du téléphone.

## Tester sur un téléphone

```sh
cd database
QRPROTEC_DEBUG_HOSTS=192.168.1.42 python manage.py serve --https
```

Ouvrir `https://192.168.1.42:8000/verif` sur le téléphone (même réseau) et accepter l'avertissement
du certificat auto-signé. `QRPROTEC_DEBUG_HOSTS` n'est pris en compte qu'en mode DEBUG (défaut en
développement).
