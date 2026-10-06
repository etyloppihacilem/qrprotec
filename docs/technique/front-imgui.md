# Front poste (ImGui, C++)

Dossier `app/`. C++17, [Dear ImGui](https://github.com/ocornut/imgui) (mode immédiat) sur GLFW +
OpenGL 3, CMake. Exécutable `QRProtecApp` (installé en `qrprotec-front` par le RPM).

## Pourquoi ImGui

Le poste est une borne plein écran pilotée surtout par une douchette, sur un petit PC sans bureau
(Cage, Wayland). ImGui donne une interface réactive, sans dépendance lourde (pas de Qt/GTK), qui
démarre en une fraction de seconde, et dont tout l'état est dans le code : pas de fichiers
d'interface à synchroniser. L'inconvénient (interface redessinée à chaque image) est compensé par le
rendu à la demande (voir « Boucle principale »). Voir [decisions.md](decisions.md#front-poste-en-imgui).

## Organisation des sources

Les dossiers suivent les dépendances : `core` ne dépend de rien, `ui` dépend de tout.

| Dossier | Bibliothèque CMake | Contenu |
|---|---|---|
| `src/core/` | `QRProtecCore` | `json` (JSON minimal tolérant), `codes` (dates, **analyse des QR codes** `parse_scan`, saisie libre des dates), `template` / `template_io` (modèles d'étiquettes `.qr`), `placeholders` (variables par usage), `search` (recherche sans accents), `paths` (dossier des modèles), `fonts`. |
| `src/net/` | `QRProtecCore` | `connection` (TCP, chiffré par OpenSSL si l'URL est en `https://`, certificat et nom d'hôte vérifiés), `http` (client HTTP/1.1), `api_client` (client asynchrone de l'API), `websocket` + `remote_scanner_link` (téléphone-douchette). |
| `src/render/` | `QRProtecCore` | `raster` (rendu d'un modèle en image 1 bit, rotation vers l'étiquette physique, export PNG), `image_loader` (PNG/JPEG en niveaux de gris, cache). |
| `src/printer/` | `QRProtecPrinter` | `serial` (port série POSIX), `niimbot_protocol` (paquets Niimbot), `printer` (`NiimbotB1Printer`). |
| `src/inateck/` | exécutable | `inateck_worker` (thread du SDK Bluetooth), `hid_classifier` (douchette en mode clavier), `sdk_json`. |
| `src/app/` | exécutable | `App` (état global, scans, vérifs, connexion), `scan_stack`, `settings`, `print_queue`, `feedback` (signal de mauvais scan), `labels` (objets API → placeholders). |
| `src/ui/` | exécutable | `widgets` (champs de recherche, saisie de date…), `editor` (éditeur d'étiquettes), `label_preview`, `inateck` (fenêtre et menu Douchette), `windows/*` (une fenêtre par fichier). |

`QRProtecCore` et `QRProtecPrinter` n'ont aucune dépendance à ImGui : ce sont eux que testent les
exécutables de `tests/` (`ctest`).

## Boucle principale (`main.cpp`)

1. Crée la fenêtre GLFW (1280×800, plein écran sous Cage), le contexte ImGui (thème clair, pas de
   fichier `imgui.ini` : la disposition est gérée par l'application), la douchette (`Inateck`) et
   l'`App`.
2. Les rappels clavier passent d'abord par `Inateck::handle_hid_key/character` : une douchette en
   mode clavier tape très vite, le classifieur décide si une rafale de caractères est un scan (il la
   consomme) ou une frappe humaine (il la rejoue vers ImGui).
3. **Rendu à la demande** : jusqu'à 60 images/s pendant 2 s après une entrée, puis
   `glfwWaitEventsTimeout(0.1)` (~10 images/s au repos, pour les scans du SDK, les réponses de l'API
   et les minuteries). Sans cela, la boucle consommait 100 % d'un cœur sous Cage, où la
   synchronisation verticale ne limite rien.
4. Chaque image : `App::begin_frame()` (réponses de l'API, scans reçus, téléphone, inactivité) puis
   `App::draw()`.

Option : `--verbose` (journal détaillé sur la sortie d'erreur, visible avec
`journalctl -u qrprotec-kiosk`).

## Threads

ImGui n'est utilisé que depuis le thread principal. Tout ce qui peut bloquer tourne ailleurs et
rapporte ses résultats par une file lue à chaque image :

| Thread | Classe | Communication |
|---|---|---|
| Requêtes HTTP | `ApiClient` | `get/post/...` mettent en file une requête et un callback ; `poll()` (thread UI) appelle les callbacks. Les fenêtres n'ont jamais à se soucier des threads. |
| Impression | `PrintQueue` | `enqueue(jobs)` ; `status()` pour la barre de progression. En cas d'erreur (papier, couvercle), la file se met en pause et garde les étiquettes restantes. |
| SDK douchette | `InateckWorker` | Commandes en file ; `take_scans()` et `snapshot()` lus par l'UI. |
| Téléphone-douchette | `RemoteScannerLink` | Reconnexion automatique, ping toutes les 15 s ; `take_messages()`. |

## L'objet `App`

`app/src/app/app.hpp` est la meilleure porte d'entrée. `App` contient tout l'état partagé :

- `settings` (`AppSettings`, fichier `~/.config/qrprotec/app.conf` en `0600`, format `clé=valeur`) ;
- `api`, `printer`, `feedback`, `inateck` ;
- `catalog` : données de référence chargées depuis l'API (types, lots, utilisateurs, stock) avec des
  compteurs de version que les fenêtres surveillent pour recharger leur détail ;
- `user` : utilisateur connecté (`SessionUser`, rôle) ;
- `stack` : la **pile de scans** ;
- `verif` : la vérif en cours (lot, clé, lots ajoutés par vérif groupée) ;
- `windows` : la liste des fenêtres.

### Traitement d'un scan

Tous les scans (SDK, clavier HID, saisie manuelle, téléphone) arrivent dans
`App::handle_scan(code, source)` :

1. `parse_scan` reconnaît le type **localement** (`core/codes.cpp`) ;
2. un item ou paquet déjà dans la pile est ignoré (mis en surbrillance) ;
3. selon le type :
   - **badge** → connexion (`login_with_badge`, puis PIN si nécessaire) ;
   - **lot** → étiquette publique : fiche du lot ; étiquette privée : lance la vérif, ou l'ajoute à la
     vérif en cours si c'est un lot du même lot global (`join_verif`) ;
   - **scellé** → fiche du lot avec le résultat du contrôle du scellé ;
   - **ouverture de scellé** → le scellé est ouvert (signé par l'utilisateur connecté, sinon anonyme) ;
   - **item** → ajouté à la pile ; s'il est périmé (date lue dans l'iid), signal d'erreur
     **immédiat** ; puis `GET /api/items/<iid>/` pour le détail ;
   - **paquet** → ajouté à la pile (compte pour tous ses items) ; en mode privilégié, ouvre sa fiche ;
   - **inconnu** → ligne rouge et signal d'erreur.

Le signal d'erreur (`Feedback`) : bip et LED orange de la douchette via le SDK, ou bip de
l'ordinateur et clignotement rouge de l'écran en mode HID, et retour au téléphone s'il vient de là.

### Connexion et actions protégées

L'utilisateur n'est connecté qu'au moment où c'est nécessaire : `require_login(raison, action)`
mémorise l'action, affiche la fenêtre de connexion et l'exécute après le badge (et le PIN). Après
`inactivity_minutes` sans activité (15 par défaut), `reset_session()` vide la pile, déconnecte et
remet les fenêtres en place : une borne partagée revient toujours à un état neutre.

### Fenêtres

Chaque fenêtre est une classe dérivée d'`AppWindow` (`draw`, `on_open`), créée par une fonction
`make_*_window()` (`ui/windows/windows.hpp`) et enregistrée dans le constructeur d'`App`. Drapeaux :
`privileged` (menu Gestion, rôles gestion/admin), `admin_only` (Réglages, Utilisateurs, Éditeur).

| Fenêtre | Fichier | Mode |
|---|---|---|
| Pile de scans | `scan_window.cpp` | toujours ouverte |
| Lots | `lots_window.cpp` | normal |
| Vérif | `verif_window.cpp` | normal |
| Téléphone comme douchette | `phone_window.cpp` | normal |
| État des stocks | `stock_window.cpp` | privilégié |
| Inventaire (réception, types, recherche, paquets) | `inventory_window.cpp` | privilégié |
| Paquet fermé | `pack_window.cpp` | privilégié |
| Gestion des lots | `lot_admin_window.cpp` | privilégié |
| Journal des opérations | `journal_window.cpp` | privilégié |
| Utilisateurs | `users_window.cpp` | admin |
| Éditeur d'étiquettes | `ui/editor.cpp` | admin |
| Réglages | `settings_window.cpp` | admin |

La disposition par défaut est enregistrée en fractions de la zone de travail (`WindowLayout`) :
elle s'adapte à la résolution de l'écran. Par défaut, la pile de scans occupe la colonne de droite
et les fenêtres de travail (Lots, Vérif, Stocks…) tout le reste, sans fond perdu autour. Quand la
fenêtre de l'application change de taille (sur le kiosk, Cage l'agrandit à la taille de l'écran
juste après la première image), les fenêtres déjà placées gardent leur place en proportion.

Le mode privilégié est volontairement **impossible à manquer** : fond, barres de titre et barre de
menu orange.

## Impression des étiquettes

```
objet API (item, lot, badge…)
   │ labels.cpp : item_parameters / lot_parameters / …      -> {{placeholders}}
   ▼
modèle .qr (TemplateDocument, JSON)  ── resolve_parameters
   │ render/raster.cpp : render_template                    -> image 1 bit au pas de 8 px/mm
   │ fit_to_label : quart de tour si le modèle est dans l'autre sens, retournement, centrage
   ▼
PrintQueue (thread) ── NiimbotB1Printer ── SerialPort (/dev/ttyACM0, 115200 bauds)
```

- **Modèles** : fichiers JSON `.qr` (`version`, `category`, `media` en mm, `elements` texte / QR /
  image, positions en mm). La catégorie (item, paquet, lot public, lot privé, scellé, ouverture du scellé, badge) détermine
  les placeholders disponibles (`core/placeholders.cpp`, liste affichée dans l'onglet Placeholders de
  l'éditeur). Un modèle par usage se choisit dans les Réglages ; sans modèle, le QR seul est imprimé.
- **Dossier des modèles** (`core/paths.cpp`) : réglage de l'application, sinon
  `$QRPROTEC_TEMPLATES_DIR`, sinon `app/templates` du dépôt (chemin compilé), sinon le dossier
  courant. Les images (logos) vont dans `images/`.
- **QR codes** : bibliothèque Nayuki (`third_party/qrcodegen`), masque choisi automatiquement, marge
  et niveau de correction réglables ; l'éditeur signale un QR à moins de 2 pixels par module.
- **Police** : DejaVu Sans fournie (`third_party/fonts`), gras réel si `DejaVuSans-Bold.ttf` est
  trouvé, sinon gras synthétique. `QRPROTEC_FONT` impose une autre police.
- **Imprimante occupée** : la B1 refuse parfois une commande pendant qu'elle termine l'étiquette
  précédente (code `0x31`) ; la commande est réessayée.

Changer d'imprimante : implémenter l'interface `Printer` (`printer/printer.hpp`) et l'utiliser dans
`PrintQueue` et dans l'éditeur (voir [maintenance.md](maintenance.md#changer-de-modèle-dimprimante)). Le reste (rendu, rotation) est indépendant du modèle d'imprimante.

## Douchette

Deux modes, gérés par `ui/inateck.cpp` :

- **SDK Bluetooth** (`InateckWorker`, bibliothèque propriétaire `libinateck_scanner_ble.so` du
  sous-module `scanner_lib`, compilée seulement si elle est présente, macro
  `QRPROTEC_HAS_INATECK`) : recherche 8 s, connexion automatique à la dernière douchette connue (son
  identifiant est retenu dans `~/.config/qrprotec/inateck-hid.conf`) et à elle seule : sinon,
  l'utilisateur choisit l'appareil dans le menu Douchette > Connecter à (les noms de douchette
  Inateck en premier). Tant que la douchette connue n'est pas connectée, `Inateck::update` relance
  une recherche toutes les 15 s (`kInateckReconnectInterval`), sauf après un « Déconnecter » manuel. Permet le retour d'erreur (bip, LED).
- **Mode clavier (HID)** : la douchette est un clavier. `HidScanClassifier` reconnaît une rafale
  (moins de 50 ms entre caractères par défaut, réglable) d'au moins 3 caractères. La durée de la
  dernière image est déduite des écarts mesurés, pour ne pas couper un scan quand l'interface est
  chargée.

Particularités connues du SDK Linux (constatées sur le matériel) :

- la douchette ne doit **pas** être appairée au niveau du système : le SDK s'y connecte lui-même ;
- la recherche du SDK échoue silencieusement (liste vide) si un **autre appareil Bluetooth** est
  connecté au PC avec des services GATT non résolus (casque, souris) : le déconnecter pendant la
  recherche. Ce n'est pas corrigeable dans l'application ;
- couleurs de LED : 2 vert, 3 bleu, 4 orange, pas de rouge (le signal d'erreur utilise l'orange) ;
- le SDK ajoute un `\n` aux codes lus, retiré par `clean_scan_code`.

## Téléphone-douchette

`App::start_remote_session` crée une session (`POST /api/remote-scanner/`), affiche le QR
`<base>/scanner?s=…&k=…`, et `RemoteScannerLink` se connecte au WebSocket
`/ws/scanner/front?s=…` de l'API locale. Les codes reçus entrent dans `handle_scan` avec
`ScanSource::Phone` ; une erreur est renvoyée au téléphone (flash rouge, vibration). Protocole :
en tête de `database/inventory/remote_scanner.py`.

## Connexion au back

Un seul réglage choisit l'API : **URL du serveur** (`api_url`).

- `http://127.0.0.1:8001` (défaut) : API locale de la même machine, sans clé (jeton facultatif
  `api_token` = `QRPROTEC_LOCAL_API_TOKEN` du serveur).
- `https://<domaine>` : back d'une autre machine via Caddy. Le front envoie sa **clé**
  (`api_key`) dans l'en-tête `X-QRProtec-Key` ; Caddy l'oriente vers l'API distante. Le certificat
  est vérifié avec le magasin du système, plus `api_ca_file` (autorité interne de Caddy).

Les variables `QRPROTEC_API_URL`, `QRPROTEC_API_KEY`, `QRPROTEC_API_CA_FILE`, `QRPROTEC_API_TOKEN`
priment sur le fichier de réglages (sur un kiosk, elles viennent de `/etc/qrprotec/front-api.conf`).
Le WebSocket du téléphone-douchette suit la même URL et la même clé. Tant que l'API est hors ligne,
un clic sur « API hors ligne » ouvre ces réglages sans badge, sauf sur une borne kiosk (pour qu'un
passant ne puisse pas rediriger la borne).

L'icône de la fenêtre est celle du site (`database/inventory/web/icon-192.png`), embarquée dans
l'exécutable ; l'identifiant d'application Wayland est `qrprotec` (`qrprotec.desktop`).

## Construire et tester

```sh
cd app
git submodule update --init           # imgui, implot, scanner_lib (inateck_sdk n'est pas nécessaire)
cmake -S . -B build && cmake --build build -j
(cd build && ctest)
./build/QRProtecApp --verbose
```

Dépendances système : GLFW 3, OpenGL, libpng, FreeType, OpenSSL, et libxdo si la bibliothèque du SDK Inateck
est présente (`-DINATECK_SDK_LIBRARY=` vide pour s'en passer, par exemple sur aarch64).

Tests (`tests/`) : `core_test` (JSON, modèles, placeholders, rendu, recherche, HTTP/WebSocket),
`codes_test` (analyse des QR, dates), `printer_test` (paquets Niimbot), `hid_classifier_test`,
`sdk_json_test`.
