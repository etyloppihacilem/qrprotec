# Front web (téléphone)

Dossier `database/inventory/web/`, servi par le back (`inventory/web_views.py`). HTML, CSS et
JavaScript **sans framework ni étape de construction** : on modifie le fichier, on recharge la page.

| Fichier | Rôle |
|---|---|
| `index.html`, `app.js`, `app.css` | Page unique de vérif et de consultation |
| `scanner.html`, `scanner.js` | Page « téléphone comme douchette du poste » |
| `sw.js` | Service worker, uniquement pour afficher les notifications web |
| `manifest.webmanifest`, `icon-*.png`, `favicon.png` | Ajout à l'écran d'accueil, icônes (`make icon`) |
| `camera-scan.js` | Lecture à la caméra limitée au viseur, partagée par `app.js` et `scanner.js` |
| `decoder.js`, `vendor/zxing-reader.js`, `vendor/zxing-reader-*.wasm` | Worker zxing-cpp en WebAssembly (zxing-wasm, MIT ; zxing-cpp, Apache 2.0), quand `BarcodeDetector` manque |
| `vendor/jsQR.js` | Décodeur QR de secours (Apache 2.0), le temps que le worker se charge ou sans WebAssembly |
| `code128.js` | Décodeur Code 128 (étiquettes d'items à code-barres), quand `BarcodeDetector` ne le lit pas |

Pourquoi pas de framework : la page doit fonctionner hors de tout réseau Internet (réseau local de
la borne, pas de CDN), rester modifiable dans cinq ans sans chaîne d'outils npm qui aura vieilli, et
elle tient en un fichier JS lisible. Voir [decisions.md](decisions.md#front-web-sans-framework).

## Adresses

La page répond aux adresses mêmes des QR codes : `/`, `/verif?lot=…[&key=…]`, `/badge?m=…&key=…`,
`/item?id=…`, `/pack?id=…`, `/seal?lot=…&s=…`, `/liste?id=…` (liste de matériel : page `lists` de l'accueil, objet `lists`
de `app.js`, relue toutes les 3 s ; en mode scan, `handleCode` envoie les lots et items à la liste). Scanner une étiquette avec l'appareil photo natif du téléphone ouvre
donc directement la bonne vue. `/verif?lot=…` et `/item?id=…` ouvrent une **fiche** sans caméra (`openFiche`,
section `#fiche`) : « Lancer une vérif » ouvre le lecteur sur le lot (`launchVerif`). La navigation entre fiches
passe par `history.pushState` (bouton retour du téléphone, `popstate`). La fiche d'un item affiche
`last_location`, renvoyé par `GET /api/items/<iid>/` seulement (`serializers.item_last_location`). Les fichiers sont référencés en relatif (`web/app.js`) et l'API par
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
  rechargement de page ne perd pas une vérif en cours, sauf après `SESSION_TIMEOUT_MS` (20 min) sans
  activité, où le lot, sa clé et les scans sont oubliés au démarrage. La clé présente dans l'URL est retirée de la
  barre d'adresse (`history.replaceState`).
- **Caméra** (`startCamera`, `scanLoop`, `CameraScan` de `camera-scan.js`) : caméra arrière,
  `BarcodeDetector` natif si disponible (Chrome Android), sinon zxing-cpp compilé en WebAssembly dans un
  worker (`decoder.js` : Firefox, Safari iOS), le même moteur que les lecteurs natifs (code penché, flou,
  peu contrasté, petit), qui ne bloque pas la page. Le module (~1 Mo) a un nom versionné et est gardé en
  cache par le navigateur ; le temps qu'il se charge, ou sans WebAssembly, jsQR et `code128.js` prennent le
  relais. Seul un code dont le centre est dans le viseur (à 10 % de son côté près) est rendu, le plus
  proche du centre s'il y en a plusieurs : les décodeurs JS ne reçoivent que l'image du viseur élargie de
  20 %, et les codes de `BarcodeDetector` (qui lit toute l'image) sont filtrés sur leur cadre. La zone suit
  le viseur affiché (`object-fit: cover`, viseur réduit du lecteur de l'accueil) ; caméra en vignette,
  toute l'image visible compte. Codes-barres Code 128 (l'iid seul, étiquettes des types « code-barres ») :
  `BarcodeDetector` s'il les lit, sinon zxing-cpp, sinon `code128.js` sur quelques lignes horizontales et
  verticales de l'image du viseur en pleine résolution, code rendu s'il est lu deux fois avec sa somme de
  contrôle. Les pages n'incluent `code128.js` (et ne demandent `code_128` à
  `BarcodeDetector`) que si le réglage serveur `web_barcodes` est actif : le poste l'active à la première
  impression d'un code-barres (`POST /api/barcodes-printed/`), un admin le coupe dans les Réglages. Caméra demandée en 1080p pour les barres fines. Un même code vu en continu n'est traité qu'une
  fois, tant qu'il ne quitte pas la caméra 2,5 s (mémorisé par code : une autre étiquette vue entre-temps ne
  le fait pas relire). Fenêtre des tâches : rescanner l'item ne valide rien (le second scan
  d'un admin ne valide toutes les tâches qu'au poste), les tâches se cochent une à une. Lampe et écran maintenu allumé (`wakeLock`) si possible.
  La caméra n'est jamais laissée ouverte page cachée (`syncCamera`) : fermée à `visibilitychange`,
  `pagehide` et `freeze`, rouverte au retour si l'utilisateur l'avait démarrée, une seule ouverture à
  la fois, et un flux obtenu alors que la page vient d'être cachée est rendu aussitôt. Sur Firefox
  Android, un flux resté ouvert écran éteint pouvait laisser la caméra « utilisée par Firefox »
  jusqu'au redémarrage du téléphone. Une caméra occupée (`NotReadableError`) est retentée 3 fois.
- **Analyse** (`parseCode`) : même format que `app/src/core/codes.cpp`, domaine ignoré.
- **Retours** : bip (Web Audio, déverrouillé au premier toucher), vibration (sauf iPhone), flash
  rouge plein écran pour un périmé ou un code inconnu.
- **Vérif** : `planSession` (répartition des items entre les lots, voir
  [regles-de-gestion.md](regles-de-gestion.md#répartition-des-items-entre-les-lots)), `validate`
  (`POST api/verifs/` avec les clés), `addToLot` (réassort), `showReport` (compte rendu).
- **Connexion** : `login` (`POST api/auth/`, PIN via `askPin`), jeton de session conservé pour les
  lectures réservées.
- **Page d'accueil** (`#home`, `showHome`/`renderHomePage`) : plein écran, affichée à l'ouverture
  de `/` et après le scan d'une carte (`badge?m=...`). Bouton **Se connecter** qui ouvre un petit
  lecteur de badge dans un coin (`openReader` : la caméra en vignette ; un autre code que le badge
  ouvre le scanner), et quatre tuiles : **Scanner** (`enterScanner`, l'écran de vérif, caméra
  démarrée), **Suivi des vérifs** (`renderLive`), **État des lots** (`renderLotsPage`, liste publique
  `POST api/lots/summary/`), **Gestion de la TECH** (`stockParts` : vues À faire `renderTasks` via `POST api/tasks/summary/`, état des stocks, prévisions ; badge gestion/admin, grisée sinon).
  Chaque page a son ancre (`#lots`, `#suivi`, `#stock`, `#scan`) : le bouton retour du téléphone
  ramène à l'accueil. Le bouton ⚙ (**réglages**, dès qu'un utilisateur est connecté)
  ouvre son **code PIN** (`renderPinSettings`, `POST api/me/pin/`) puis le **gestionnaire des
  notifications** (`renderNotifications`), affiché seulement pour les utilisateurs qui peuvent en
  recevoir (`notifications` dans la réponse de `api/auth/`).
- **Suivi des vérifs en direct** : `live` publie la vérif en cours de la page de scan (lot, scans,
  reste à scanner) sur `/ws/live/scanner`, avec une session conservée dans `sessionStorage` pour
  survivre à un rechargement ; la page de suivi ouvre `/ws/live/watch` (badge exigé) et reçoit les
  mises à jour, puis l'historique par `POST api/verifs/recent/`. Voir `inventory/live.py`.
- **Lampe et vibreur** : la lampe (🔦) n'est proposée que si la caméra l'expose (Chrome Android ;
  ni Safari iOS ni Firefox ne la donnent aux pages web) et se rallume quand la caméra redémarre.
  Le vibreur utilise `navigator.vibrate` (Android) ; sur iPhone, à défaut, le retour haptique d'un
  interrupteur `<input switch>` (iOS 18+, au mieux : Safari peut l'ignorer hors d'un toucher).

Toutes les écritures passent par l'**API publique** : la vérif exige la clé du lot (étiquette
clé) et un badge.

## Service worker

`sw.js` est enregistré **seulement** quand un admin active les notifications. Sa portée est `web/` :
il ne contrôle aucune page et n'intercepte aucune requête (pas de cache hors ligne, pas de risque de
servir une vieille version de l'application). Il affiche la notification et, au clic, ouvre ou
ramène la page sur l'état des stocks de l'accueil.

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
