# Front web (téléphone)

Dossier `database/inventory/web/`, servi par le back (`inventory/web_views.py`). HTML, CSS et
JavaScript **sans framework ni étape de construction** : on modifie le fichier, on recharge la page.

| Fichier | Rôle |
|---|---|
| `index.html`, `app/*.js`, `app.css` | Page unique de vérif et de consultation (scripts décrits plus bas) |
| `scanner.html`, `scanner.js` | Page « téléphone comme douchette du poste » |
| `sw.js` | Service worker des notifications web (portée `web/`) |
| `app-sw.js` | Service worker de l'appli (servi à la racine) : l'ouvrir sans réseau |
| `manifest.webmanifest`, `icon-*.png`, `apple-touch-icon.png`, `favicon.png` | Installation de l'appli « QRprotec » sur l'écran d'accueil (tuto dans Réglages ⚙ > Installer l'appli) ; icônes `any` à coins transparents, `maskable` plein cadre pour les icônes rondes d'Android, `apple-touch-icon` plein cadre pour iOS (`make icon`) |
| `camera-scan.js` | Lecture à la caméra limitée au viseur, partagée par `app/` et `scanner.js` |
| `decoder.js`, `vendor/zxing-reader.js`, `vendor/zxing-reader-*.wasm` | Worker zxing-cpp en WebAssembly (zxing-wasm, MIT ; zxing-cpp, Apache 2.0), quand `BarcodeDetector` manque |
| `vendor/jsQR.js` | Décodeur QR de secours (Apache 2.0), le temps que le worker se charge ou sans WebAssembly |
| `code128.js` | Décodeur Code 128 (étiquettes d'items à code-barres), quand `BarcodeDetector` ne le lit pas |

Pourquoi pas de framework : la page doit fonctionner hors de tout réseau Internet (réseau local de
la borne, pas de CDN), rester modifiable dans cinq ans sans chaîne d'outils npm qui aura vieilli, et
elle tient en quelques scripts JS lisibles, sans import ni compilation. Voir [decisions.md](decisions.md#front-web-sans-framework).

## Adresses

La page répond aux adresses mêmes des QR codes : `/`, `/verif?lot=…[&key=…]`, `/badge?m=…&key=…`,
`/item?id=…`, `/pack?id=…`, `/seal?lot=…&s=…`, `/liste?id=…` (liste de matériel : page `lists` de l'accueil, objet `lists`
de `app/lists.js`, relue toutes les 3 s ; en mode scan, `handleCode` envoie les lots et items à la liste). Scanner une étiquette avec l'appareil photo natif du téléphone ouvre
donc directement la bonne vue. `/verif?lot=…` et `/item?id=…` ouvrent une **fiche** sans caméra (`openFiche`,
section `#fiche`) : « Lancer une vérif » ouvre le lecteur sur le lot (`launchVerif`). La navigation entre fiches
passe par `history.pushState` (bouton retour du téléphone, `popstate`). La fiche d'un item affiche
`last_location`, renvoyé par `GET /api/items/<iid>/` seulement (`serializers.item_last_location`). Les fichiers sont référencés en relatif (`web/app/boot.js`) et l'API par
`new URL('api/', document.baseURI)` : la page fonctionne aussi sous un sous-chemin.

Les iids et identifiants de lots (et numéros de paquets) ne s'affichent qu'à un administrateur
(`showIds`, `idText`, `withId`) : pour les autres, ni sous-titre `type · id`, ni listes dépliables
d'iids (flèches de la fiche de lot `ficheFold`, groupes de l'onglet À scanner `groupHead`). Le poste
n'est pas concerné. Les URL et appels d'API gardent évidemment les ids.

Le back ne sert que les fichiers déclarés dans `web_views.ASSETS` (liste blanche). **Ajouter un
fichier au front web = l'ajouter à cette liste**, sinon 404.

La caméra n'est accessible qu'en HTTPS (ou sur `localhost`) : en production Caddy fournit le
certificat ; en développement, `manage.py serve --https`.

## Structure du front (`web/app/`)

Un objet `state` et des fonctions `render*` qui reconstruisent les vues à partir de l'état (pas de DOM
virtuel, des petits morceaux recréés avec l'aide `el(tag, attrs, ...enfants)`).

Le code est découpé en **scripts classiques** (pas de modules ES, pas d'étape de construction), chargés
dans l'ordre par `index.html` (`defer`). Ils partagent leurs déclarations de premier niveau comme les
morceaux d'un seul fichier : une fonction de `scans.js` appelle directement `render()` de `render.js`.
Deux règles :

- **au chargement**, un script n'appelle que des fonctions des scripts précédents ou du sien
  (`addEventListener('click', f)` lit `f` tout de suite : `f` doit déjà exister). Tout le reste passe par
  des événements, et la page ne démarre qu'à la fin du dernier script, `boot.js` (`boot()`) ;
- **un nouveau script** s'ajoute à `index.html` avant `boot.js` (le test
  `WebFrontTests.test_app_scripts_all_loaded` le vérifie). La liste blanche `web_views.ASSETS` prend
  tout `web/app/*.js`.

| Script | Contenu |
|---|---|
| `state.js` | Constantes, objet `state`, `save` / `restore` |
| `utils.js` | Accords (`pl`, `nb`, `deName`), dates, `el`, icônes |
| `offline.js` | `api`, hors connexion (`outbox`), cache de vérif (`vcache`), `parseCode` |
| `feedback.js` | Bip, vibration, flash, `toast` |
| `camera.js` | Caméra, lecture dans le viseur, vignette |
| `scans.js` | `handleCode` et le traitement de chaque scan (items, paquets, étiquettes, scellés) |
| `verif-plan.js` | `planVerif` : répartition des items entre les lots, fonction pure jouée aussi sous node par les scénarios communs (`tests/verif-scenarios/`) |
| `cascade.js` | Vérif en cascade, items à cocher, `planSession`, `lotStatus` |
| `pin-reset.js` | Déblocage d'un PIN par un admin |
| `actions.js` | `validate`, `partialLots`, réassort, menus de la vérif |
| `render.js` | Onglets À scanner / Scannés / Lot, `render()` |
| `home.js` | Accueil et navigation entre ses pages |
| `login.js` | Grande fenêtre de connexion, connexion par nom, web partagé |
| `live.js` | Suivi en direct (`live`, `watch*`), vérifs récentes, comptes rendus |
| `notifications.js` | Abonnement aux notifications web |
| `settings.js` | Réglages ⚙ (installer l'appli, PIN, vérif, web partagé) |
| `gestion.js` | Gestion de la TECH (tâches, état des stocks, prévisions, étiquettes) |
| `fiches.js` | Fiches de lot et d'item, ouverture d'un scellé |
| `reports.js` | Signalements |
| `lists.js`, `list-detail.js` | Listes de matériel (chargement, scans, détail, éditeur, recherches) |
| `depart.js` | Page Départ |
| `debug-codes.js` | Outil de debug des codes (admins) |
| `boot.js` | `boot()` : lecture de l'URL et démarrage, toujours chargé en dernier |

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
  ouvre le scanner), et ses tuiles : **Scanner** (`enterScanner`, l'écran de vérif, caméra
  démarrée), **Suivi des vérifs** (`renderLive`), **État des lots** (`renderLotsPage`, liste publique
  `POST api/lots/summary/`), **Départ** (`renderDepartPage`, objet `depart` : onglet « Ma liste », listes de
  matériel attribuées et pas encore rentrées, `POST api/checklists/list/ {show: "assigned"}` ; onglet « Lots », les
  lots hors rangements avec une recherche `searchScore` sur le nom, le type et la description du type, cochés
  (`depart.selected`, `lotRows(rerender, pick)`) puis « Partir avec N lots » → `POST api/checklists/departure/
  {lots: [ids]}` (liste à préparer, lignes lot, l'utilisateur attribué) ouverte pour scanner ; `{blocked: true}`
  donne les lots non cochables (pris par une liste, place fixe dans leur lot) ; onglet Lots
  par défaut sans liste attribuée ou sans connexion) et Listes de matériel, puis en larges tuiles Signalements,
  État des lots et, tout en bas, **Gestion de la TECH** (`stockParts` : vues À faire `renderTasks` via `POST api/tasks/summary/`, À venir `renderEvents` via `POST api/events/summary/` (horizon `state.eventsDays`, filtre `state.eventsKind`), état des stocks, prévisions ; badge gestion/admin, grisée sinon).
  Chaque page a son ancre (`#lots`, `#suivi`, `#depart`, `#stock`, `#scan`) : le bouton retour du téléphone
  ramène à l'accueil. Le bouton ⚙ (**réglages**, dès qu'un utilisateur est connecté)
  ouvre la case **Bulle de scan** (`renderScanSettings`, réglage `scan_bubble` de `web_prefs` ;
  sans connexion, gardée sur le téléphone, et le dernier connecté la fixe pour le téléphone). Cochée, le
  bouton ↘ de l'écran de scan (`#cam-mini`) réduit la caméra dans un coin (`body.cam-mini`) ; décochée (le
  défaut), le bouton est caché et la caméra reste en haut (`syncScanBubble`). Toute connexion passe par la
  **grande fenêtre de connexion** (`#login-gate`, `openGate`), qui s'ouvre aussi au début d'une vérif sans connexion : la caméra (`#camera`, placée par `placeGateCamera`) au
  milieu, « Scannez votre carte », connexion nom + PIN ; seule une carte y est acceptée. À une vérif, la
  connexion n'est pas imposée : « Continuer sans compte (invité) » ferme la fenêtre. Puis son **code
  PIN** (`renderPinSettings`, `POST api/me/pin/`), ses **réglages de la vérif**
  (`renderVerifSettings`, `POST api/me/prefs/` `{user, web_prefs}` : `models.WEB_PREFS`, renvoyés dans
  `user.web_prefs`, `webPref()` ; aussi `PATCH api/users/<matricule>/` `{web_prefs}` par un admin au poste,
  qui lit `web_prefs_list` [{name, label, value}]) puis le **gestionnaire des
  notifications** (`renderNotifications`), affiché seulement pour les utilisateurs qui peuvent en
  recevoir (`notifications` dans la réponse de `api/auth/`).
  Tout en bas, **Web partagé** (`renderSharedSettings`, tablettes de l'antenne) : réglage de l'appareil,
  pas du compte, dans `localStorage` (`qrprotec.shared-web`, `navigator.storage.persist()` à l'activation),
  coché ou décoché par gestion/admin seulement (grisé et visible des autres quand il est coché). Coché,
  `save()` range l'utilisateur connecté dans `sessionStorage` (perdu à la fermeture de l'appli) et
  `checkSharedIdle` déconnecte après 1 h sans activité (`SHARED_IDLE_MS`, dernière activité
  `qrprotec.activity` : toucher, clavier, douchette, scan caméra), vérifié chaque minute, au retour sur
  la page et avant de compter un nouveau geste. Aucun changement côté serveur.
- **Stats de l'année** (gestion et admin, pour l'AG) : page cachée `#stats` (`fun-stats.js`), ouverte par
  5 appuis rapides sur « Prévisions » (`funTap`). `POST api/stats/fun/` (`fun_stats.py`, `period` : `last12`
  ou une année civile, refusé aux autres rôles par le serveur) rend les chiffres de la période et des cartes :
  podiums (vérifs, périmés débusqués, duos, coéquipiers, items scannés, scellés, réassorts, signalements,
  listes, lots chouchous, recrues) et records (marathon du jour, vérif la plus tardive / matinale, périmé le
  plus ancien, lot oublié, jour et mois préférés). Une vérif en cascade compte une fois, pour chacun de ses
  participants ; seuls les secouristes connectés entrent dans les podiums, prénoms seulement. « Présenter »
  les affiche en plein écran une carte à la fois (`#fun-show`, flèches ou toucher pour avancer).
- **Outil de debug des codes** (admins) : page cachée `#debug`, ouverte par 5 appuis rapides sur
  « État des stocks » (`debugTap`). `POST api/debug/codes/` (`debug_codes.py`, refusé à tout autre rôle
  par le serveur) rend le contenu de toutes les étiquettes imprimables : items (QR de l'URL, ou iid court
  des QR réduits et codes-barres), lots (publique, clé, scellé, ouverture), paquets scellés, listes.
  Jamais les badges ni les liens de déblocage de PIN. En tête, les **cartes des secouristes (dépannage)** :
  un appui sur un nom crée (`POST api/debug/temp-login/`, noté au journal) un QR code `badge?m=…&t=…`
  valable 1 h, jamais pour un admin. Scanné par le téléphone du secouriste (`POST api/auth/temp/`), il ouvre
  sans badge ni PIN une session de 24 h (jeton `qrprotec.temp-session`, `sessionMs` dans le stockage local).
  Un nouveau badge invalide le QR code et la session. Un code s'affiche en grand (`#debug-player`), seul
  ou en suite (contenu d'un lot, avec ses sous-lots dans l'ordre d'une vérif en cascade) passée au code
  suivant. Dans la cascade, un sous-lot scellé ne donne que l'étiquette de son scellé (ni clé ni contenu),
  un lot de référentiel (B+) ou caché dans la vérif n'a pas d'étiquette, seulement ses sous-lots. Code
  suivant d'un appui, par Suivant, les flèches du clavier ou en automatique.
- **Suivi des vérifs en direct** : `live` publie la vérif en cours de la page de scan (lot, scans,
  reste à scanner) sur `/ws/live/scanner`, avec une session conservée dans `sessionStorage` pour
  survivre à un rechargement ; la page de suivi ouvre `/ws/live/watch` (badge exigé) et reçoit les
  mises à jour, puis l'historique par `POST api/verifs/recent/`. Un clic n'importe où sur une ligne de
  l'historique (ou sur une carte « En direct » terminée) ouvre le compte rendu de la vérif
  (`POST api/verifs/<id>/report/` pour chaque étape de `ids`, réunies en un seul arbre, `#verif-report`) : leurs titres n'ont pas de lien, ce sont les noms
  de lots du compte rendu qui ouvrent la fiche du lot.
- **Fiche de lot selon le rôle** : `detailed()` (gestion, admin) garde l'état détaillé, la ligne
  « Dernière vérif … par », la prochaine révision, le lot global et la raison d'une vérif recommandée.
  Secouristes et chefs n'ont que `shortBanner` (état en un mot, date de la dernière vérif en non gras ;
  pour un regroupement la plus ancienne de ses sous-lots), la prochaine péremption et le contenu dans
  un `<details>` replié quand le lot est valide. Le bandeau du lecteur à l'étiquette d'un lot
  (`showLotInfo`, tous rôles) reprend le même état court, plus ce qu'il manque pour valider. Voir `inventory/live.py` et [WebSockets des vérifs](websockets.md).
- **Cache de vérif** (`vcache`, voir [back.md](back.md), « Cache de vérif ») : chargé en
  arrière-plan dès l'ouverture du scanner, au lancement d'une vérif et à chaque scan (au plus une fois par
  minute), puis toutes les minutes tant que le scanner est ouvert ou qu'une vérif est en cours, et dès le
  retour du réseau (`online`). Un item, un paquet ou une étiquette de lot scannés se traitent aussitôt depuis le
  cache (bip, péremption, signalement, désactivation, tâches, lot où l'item est rangé, lot global) ; le serveur
  confirme ensuite en arrière-plan (`confirmItem`, `confirmPack`, `GET items/<iid>/`, `packs/<id>/`) et seule une
  différence qui compte (`itemFacts` : à remplacer, désactivé, statut, lot, signalements, tâches) est appliquée,
  avec bip et popup pour un nouveau signalement ou un item devenu à remplacer. Une mise à jour du cache fait de
  même pour les items déjà scannés et remplace le lot de la vérif en cours (`vcache.onChange`). Juste après une
  écriture (étape validée, items ajoutés, signalement), `loadLot(id, '', true)` relit le serveur d'abord. Les
  étiquettes clé et de scellé restent contrôlées par le serveur.
- **Hors connexion** (une partie de la réserve n'a ni réseau ni wifi) : de « Lancer la vérif » à l'envoi, la vérif
  se fait depuis le cache de vérif. `api()` note chaque requête qui n'aboutit pas (`setOffline`) et la mention
  « Hors connexion » s'affiche en haut du scanner (`#offline-chip`, avec le nombre d'envois en attente). La vérif
  finale, chaque étape de la vérif en cascade et les scellés intacts passent par `sendVerif` : hors connexion, ou
  si des envois attendent déjà (l'ordre des étapes compte), ils vont dans une file gardée sur le téléphone
  (`localStorage` `qrprotec.outbox`) et la vérif continue avec un compte rendu local (`offlineReport`). La file est
  renvoyée dans l'ordre au retour du réseau (événement `online`, toute requête réussie, essai toutes les 20 s,
  rechargement de la page). Un envoi refusé par le serveur (étiquette clé périmée, scellé brisé entre-temps) sort
  de la file et est signalé, à refaire. Hors connexion, l'étiquette clé et l'étiquette du scellé sont acceptées
  sur place (`keyAccepted`, `cachedSealLot`) : le serveur les contrôle à l'envoi. Chaque envoi porte deux heures du téléphone, `done_at` (validation, juste
  après les scans, gardée dans la file) et `sent_at` (envoi) : le serveur date la vérif à l'heure des scans et
  garde l'heure de réception à côté (voir [back.md](back.md), « Vérif faite hors connexion »).
- **Vérif en attente d'envoi, bien visible** : à la première vérif mise en file, une popup dit « Pas de réseau :
  vérif pas encore envoyée » et invite à aller là où il y a du réseau. Tant que la file n'est pas vide, un bandeau
  orange en haut de l'accueil (`#outbox-banner`) et la mention du scanner (`#offline-chip`, « 📶 N vérifs à
  envoyer ») le rappellent ; « Envoyer maintenant » (bandeau, ou popup ouverte en touchant la mention) réessaie tout
  de suite (`sendNow`). Les étapes d'une même vérif en cascade comptent pour une vérif (`group`).
- **Bilan au retour du réseau** (`reviewSent`) : à la mise en file, chaque envoi garde ce que le téléphone savait
  (`offlineSnapshot` : version et signalements des lots du lot global, signalements et état des items scannés). Une
  fois envoyé, le cache de vérif est relu et une popup « Vérif de … envoyée : à revoir » liste ce que le secouriste
  n'a pas pu voir sur place, une ligne par type d'item qui dit tout (« Acide borique signalé (« … ») : inutilisable,
  à remplacer, il en manque 1 (1/2) ») : signalement, item inutilisable ou désactivé, périmé à remplacer, manque
  d'après le serveur ; puis contenu du lot modifié par le TECH, signalement du lot lui-même, codes inconnus. S'il
  manque du matériel ou si le contenu a changé, « Reprendre la vérif de … » relance la vérif du lot (avec
  l'étiquette clé de l'envoi). Rien de nouveau : un simple message « vérif envoyée ».
- **Appli ouverte sans réseau** : le service worker `app-sw.js` garde la page et ses fichiers (voir plus bas), et
  le cache de vérif est gardé sur le téléphone (IndexedDB `qrprotec`, `savedCache`), relu à l'ouverture
  (`vcache.ready()`) puis mis à jour en arrière-plan (à l'ouverture de l'appli s'il a plus de 10 min, et comme
  plus haut). Une étiquette de lot ou d'item ouverte hors connexion (appareil photo, scanner au repos) affiche
  sa fiche tirée de ce cache, avec le bandeau « Hors connexion » et la date de la dernière mise à jour
  (`offlineFiche`) ; « Lancer une vérif » y marche. L'étiquette du scellé et la connexion par badge restent en
  ligne : la personne doit s'être connectée avant (session gardée sur le téléphone). Il faut avoir ouvert l'appli
  une fois avec du réseau sur ce téléphone, en HTTPS avec un certificat reconnu (sinon le navigateur refuse le
  service worker et rien n'est gardé).
- **Lampe et vibreur** : la lampe (🔦) n'est proposée que si la caméra l'expose (Chrome Android ;
  ni Safari iOS ni Firefox ne la donnent aux pages web) et se rallume quand la caméra redémarre.
  Le vibreur utilise `navigator.vibrate` (Android) ; sur iPhone, à défaut, le retour haptique d'un
  interrupteur `<input switch>` (iOS 18+, au mieux : Safari peut l'ignorer hors d'un toucher).

Toutes les écritures passent par l'**API publique** : la vérif exige la clé du lot (étiquette
clé) et un badge.

## Service workers

Deux service workers indépendants :

- `sw.js` (notifications) est enregistré **seulement** quand un admin active les notifications. Sa
  portée est `web/` : il ne contrôle aucune page et n'intercepte aucune requête. Il affiche la
  notification et, au clic, ouvre ou ramène la page sur l'état des stocks de l'accueil. Ses abonnements
  ne dépendent pas de l'autre (`currentPushSubscription` ne lit que l'enregistrement de portée `web/`).
- `app-sw.js` (appli sans réseau) est enregistré à chaque ouverture (`registerAppWorker`). Il est servi
  à la racine (`/app-sw.js`, `web_views.app_service_worker`) pour que sa portée couvre toutes les pages ;
  le back écrit dedans la version du front et la liste des fichiers :
  - **Liste des fichiers** (`offline_files`) : la page (`./`) et tout `ASSETS` de `web_views.py`, sauf
    `sw.js`. Un nouveau script `web/app/*.js` y entre tout seul (liste blanche par dossier) ; tout autre
    nouveau fichier du front n'a qu'à être ajouté à `ASSETS`, comme avant pour être servi : il est gardé
    automatiquement.
  - **Version** (`offline_version`) : empreinte de la date et de la taille de chaque fichier (comme l'ETag)
    et du réglage des codes-barres (qui change la page). Le navigateur relit `app-sw.js` à chaque ouverture ;
    après une mise à jour du paquet il change, le nouveau worker recharge tous les fichiers d'un coup dans un
    cache neuf (`qrprotec-app-<version>`, tout ou rien) et efface l'ancien.
  - **Toujours le réseau d'abord** : chaque page et chaque fichier sont demandés au serveur (revalidés par
    l'ETag, 304 sans corps) et la copie est mise à jour. La copie ne sert que sans réponse : pas de réseau,
    plus de 4 s pour la page (8 s pour un fichier), ou 502/503/504 (back arrêté, maintenance). Une page
    ouverte depuis la copie charge aussi ses scripts depuis la copie, pour ne pas mélanger deux versions.
  - Toutes les pages de l'appli (`web_views.PAGES` : `/`, `verif`, `item`, `badge`…) répondent avec la
    copie de la page. L'API (`api/`), les WebSockets et la page de la douchette (`scanner`) ne passent
    jamais par lui.

## Page « douchette du poste »

`scanner.html` / `scanner.js` : ouverte par le QR affiché sur le poste
(`/scanner?s=SESSION&k=CLÉ`). Elle se connecte en WebSocket à `/ws/scanner/phone`, envoie chaque code
lu, affiche un libellé court (jamais les clés), restitue les erreurs renvoyées par le poste, et se
reconnecte seule. « Annuler le dernier » retire du poste le dernier scan venu du téléphone.
« ← Accueil » (en haut), « Quitter » (barre de connexion) et « Revenir à l'accueil » (session fermée)
rendent la caméra, ferment la WebSocket puis ouvrent l'accueil seulement si le serveur répond ; sinon
la page tente de se fermer, puis affiche « Serveur injoignable » (Réessayer / Rester sur la douchette)
au lieu d'une page d'erreur sans issue, ce qui compte dans l'appli installée (pas de bouton retour).

## Tester sur un téléphone

```sh
cd database
QRPROTEC_DEBUG=1 QRPROTEC_DEBUG_HOSTS=192.168.1.42 python manage.py serve --https
```

Ouvrir `https://192.168.1.42:8000/verif` sur le téléphone (même réseau) et accepter l'avertissement
du certificat auto-signé. `QRPROTEC_DEBUG_HOSTS` (vide par défaut) n'est pris en compte qu'en mode DEBUG,
qui écoute aussi sur tout le réseau (`0.0.0.0`) ; hors DEBUG, l'API publique reste sur `127.0.0.1`.
