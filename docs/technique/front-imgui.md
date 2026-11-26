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
| `src/net/` | `QRProtecCore` | `connection` (TCP, chiffré par OpenSSL si l'URL est en `https://`, certificat et nom d'hôte vérifiés), `http` (client HTTP/1.1), `api_client` (client asynchrone de l'API), `websocket` + `websocket_link` (téléphone-douchette, impressions en direct). |
| `src/render/` | `QRProtecCore` | `raster` (rendu d'un modèle en image 1 bit, rotation vers l'étiquette physique, export PNG), `image_loader` (PNG/JPEG en niveaux de gris, cache). |
| `src/printer/` | `QRProtecPrinter` | `serial` (port série POSIX), `niimbot_protocol` (paquets Niimbot), `label_languages` (commandes TSPL et ZPL), `raw_transport` (fichier `/dev/usb/lp0` ou TCP 9100), `printer` (`NiimbotB1Printer`, `RawLabelPrinter`, `AnyPrinter` qui choisit selon le réglage). |
| `src/inateck/` | exécutable | `inateck_worker` (thread du SDK Bluetooth), `hid_classifier` (douchette en mode clavier), `sdk_json`, `scanner_settings` (modèles et libellés des paramètres du SDK : `get/set_setting_info` prennent le modèle, `INATECK_DEVICE_TYPE`, et la liste des paramètres vient de la douchette). |
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
| Téléphone-douchette, impressions en direct | `WebSocketLink` | Reconnexion automatique, ping toutes les 15 s (applicatif et du protocole), connexion muette 45 s refaite ; `take_messages()`. |

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

Les méthodes de `App` sont réparties par sujet : `app.cpp` (boucle principale, barre de menus, fenêtres),
`app_offline.cpp` (cache de vérif, envois en attente), `app_windows.cpp` (rangement des fenêtres),
`app_session.cpp` (connexion, session), `app_setup.cpp` (première configuration), `app_phone.cpp`
(téléphone-douchette), `app_scans.cpp` (`handle_scan`), `app_verifs.cpp` (vérifs, réassort, scellés),
`app_cascade.cpp` (vérif en cascade), `app_data.cpp` (données du serveur, impressions),
`app_reports.cpp` (signalements, révisions). Une aide partagée entre ces fichiers se déclare dans
`app_internal.hpp` ; la répartition des items est dans `core/verif_plan.cpp`.

### Traitement d'un scan

Tous les scans (SDK, clavier HID, saisie manuelle, téléphone) arrivent dans
`App::handle_scan(code, source)` :

1. `parse_scan` reconnaît le type **localement** (`core/codes.cpp`) ;
2. un item ou paquet déjà dans la pile est ignoré (mis en surbrillance) ;
3. selon le type :
   - **badge** → connexion (`login_with_badge`, puis PIN si nécessaire) ;
   - **lot** → étiquette publique : fiche du lot, et la même étiquette rescannée aussitôt (scan
     suivant) lance sa vérif (`public_label_scanned_`) ; étiquette clé : lance la vérif, ou l'ajoute à la
     vérif en cours si c'est un lot du même lot global (`join_verif`) ;
   - **scellé** → fiche du lot avec le résultat du contrôle du scellé ;
   - **ouverture de scellé** → le scellé est ouvert (signé par l'utilisateur connecté, sinon anonyme) ;
   - **item** → ajouté à la pile ; s'il est périmé (date lue dans l'iid), signal d'erreur
     **immédiat** ; son détail vient tout de suite du cache de vérif s'il y est (`item_resolved`), puis
     `GET /api/items/<iid>/` le confirme (`item_confirmed` : seule une différence est signalée) ;
   - **paquet** → ajouté à la pile (compte pour tous ses items) ; en mode privilégié, ouvre sa fiche ;
   - **inconnu** → ligne rouge et signal d'erreur.

**Vérif hors connexion** (une partie de la réserve n'a ni réseau ni wifi) : `VerifCache`
(`core/verif_cache.cpp`, voir [back.md](back.md), « Cache de vérif ») garde tout ce qu'une vérif lit au
serveur ; `sync_cache` le tient à jour toutes les minutes depuis le lancement (seuls les seaux changés
reviennent) et `cache_changed` applique les différences au lot de la vérif en cours et aux items déjà
scannés. Item, paquet, lot de la vérif (`start_verif`), étiquette clé et étiquette du scellé se traitent
depuis le cache sans le serveur. Les envois de vérif (`post_verif` : vérif, étapes de la cascade, scellés
intacts) partent dans `VerifOutbox` hors connexion, ou si des envois attendent déjà, gardés dans
`verif-outbox.json` à côté des réglages et renvoyés dans l'ordre (`flush_outbox`, essai toutes les 20 s et
dès que le cache se recharge) ; la vérif continue avec un compte rendu local (`offline_report`). Un envoi
refusé par le serveur est retiré de la file et signalé. Comme au web, chaque envoi porte `done_at` (heure
de la validation, gardée dans la file) et `sent_at` (heure de l'envoi, `verif_stamped`) : la vérif est datée à
l'heure des scans. La barre de menu affiche « Hors connexion » et le
nombre de vérifs en attente, la fenêtre Vérif le rappelle.

Le signal d'erreur (`Feedback`) : bip et LED orange de la douchette via le SDK, ou bip de
l'ordinateur et clignotement rouge de l'écran en mode HID, et retour au téléphone s'il vient de là.

### Connexion et actions protégées

L'utilisateur n'est connecté qu'au moment où c'est nécessaire : `require_login(raison, action)`
mémorise l'action, affiche la fenêtre de connexion et l'exécute après le badge (et le PIN). Après
`inactivity_minutes` sans activité (15 par défaut), `reset_session()` vide la pile, déconnecte et
remet les fenêtres en place : une borne partagée revient toujours à un état neutre.

Le réglage `shared_machine` (« Machine partagée », coché par défaut) est local au poste. Décoché, il
n'y a plus de réinitialisation après inactivité, et la dernière connexion (matricule et clé du badge,
ou nom et prénom après une connexion sans badge) est gardée dans `~/.config/qrprotec/session.conf`
(droits `0600`). Au démarrage, dès que le serveur répond, `restore_login()` se reconnecte avec : le
serveur redemande le PIN si l'utilisateur en a un. Même chose quand le jeton de 12 h expire en cours
d'utilisation. Seul « Se déconnecter » (`manual_logout()`) ou un badge refusé efface ce fichier, ainsi
que le fait de recocher « Machine partagée ».

### Fenêtres

Chaque fenêtre est une classe dérivée d'`AppWindow` (`draw`, `on_open`), créée par une fonction
`make_*_window()` (`ui/windows/windows.hpp`) et enregistrée dans le constructeur d'`App`. Drapeaux :
`privileged` (rôles gestion/admin), `admin_only` en plus (admins : Utilisateurs, Éditeur, Réglages).

Deux modes, selon pourquoi on est devant le poste : **normal** (secouriste, chef) et **gestion**. `App::manager()`
dit si le rôle permet la gestion ; `privileged()` et `admin()` suivent en plus le mode choisi (`gestion_mode_`,
bascule Normal | Gestion à gauche de la barre ou `Ctrl+G`, `set_gestion_mode`), si bien qu'un gestionnaire en
mode normal voit exactement le poste d'un chef. Chaque mode garde la place des fenêtres communes (`mode_places_`).
En mode gestion, la barre a un menu par domaine (`draw_gestion_menus` dans `app.cpp` : TECH, Lots, Items, Stock,
Réception, Étiquettes, puis Impression et Administration) ; chaque domaine a sa fenêtre et sa touche F fixe
(`assign_shortcuts`). « Aller à… » (`app/hub.cpp`, `Ctrl+K`, popup simple fermée par un clic à côté ou `Échap`)
cherche fenêtres, actions, lots (attention d'abord), types et fiche d'un item par son iid.

Écrans partagés : `LotPanel` (`windows.hpp`, `lot_admin_window.cpp`, l'ancienne Gestion des lots sans fenêtre à
elle) donne les onglets de gestion de la fiche du lot, le nouveau lot, l'ordre et les archives à Lots, et les types
de lots à leur fenêtre (une instance par fenêtre). `inventory_window.cpp` fait trois fenêtres du même code
(`InventoryWindow::View` : Stock, Items, Réception). Pour ouvrir une fenêtre sur un onglet ou un objet :
`show_lot`, `show_stock_tab` (aiguille vers Stock, Items ou Réception selon l'onglet), `show_item`,
`show_receptions`, `show_tech_tab`, `show_lots_mode`, `show_type`, `open_settings_section`.

| Fenêtre | Fichier | Mode |
|---|---|---|
| Pile de scans | `scan_window.cpp` | toujours ouverte |
| Lots | `lots_window.cpp` | normal |
| Vérif | `verif_window.cpp` | normal |
| Téléphone comme douchette | `phone_window.cpp` | normal |
| Gestion de la TECH `F5` (À faire, À venir) | `stock_window.cpp` | gestion |
| Stock `F6` (état du stock et projection, commandes, transferts) | `inventory_window.cpp` | gestion |
| Items `F7` (types d'items, items, entretien, paquets fermés) | `inventory_window.cpp` | gestion |
| Types de lots `F8` | `lot_types_window.cpp` | gestion |
| Réception `F9` (nouvelle, passées) | `inventory_window.cpp` | gestion |
| Paquet fermé (au scan d'un paquet) | `pack_window.cpp` | gestion |
| Journal des opérations (menu TECH) | `journal_window.cpp` | gestion |
| Signalements `F11` (lecture pour la gestion, traitement admin) | `reports_window.cpp` | gestion |
| Impressions et consommables (menu Impression) | `print_counts_window.cpp` | gestion |
| Utilisateurs | `users_window.cpp` | admin |
| Éditeur d'étiquettes | `ui/editor.cpp` | admin |
| Réglages | `settings_window.cpp` | admin |
| Documentation (menu Administration) | `doc_window.cpp` | admin |

La fenêtre **Documentation** lit les pages de `docs/` embarquées dans l'exécutable à la construction
(`CMakeLists.txt` écrit `generated/docs_data.cpp`, tableau `kEmbeddedDocs` de `core/docs.hpp`) : le poste
montre toujours la documentation de sa version, sans fichier à installer à côté. Le Markdown est lu par
`core/markdown.cpp` (sans interface, testé par `tests/markdown_test.cpp`, qui vérifie aussi que chaque lien
relatif et chaque ancre de `docs/` mène quelque part) : titres, listes imbriquées, citations, tableaux, blocs
de code (police DejaVu Sans Mono), gras, italique, code et liens. L'arborescence suit les sommaires : dans
chaque dossier, le README puis les pages dans l'ordre de ses liens. Un lien vers une autre page l'ouvre (à
la bonne section pour `page.md#ancre`, ancres comme sur GitHub), un lien externe copie l'adresse, un lien
vers un fichier du dépôt hors de `docs/` est signalé. Une nouvelle page de `docs/` y apparaît d'elle-même.

La disposition par défaut est enregistrée en fractions de la zone de travail (`WindowLayout`) :
elle s'adapte à la résolution de l'écran. Par défaut, la pile de scans occupe la colonne de droite
et les fenêtres de travail (Lots, Vérif, Stocks…) tout le reste, sans fond perdu autour. Quand la
fenêtre de l'application change de taille (sur le kiosk, Cage l'agrandit à la taille de l'écran
juste après la première image), les fenêtres déjà placées gardent leur place en proportion.
Sur le kiosk (`QRPROTEC_KIOSK`), la fenêtre principale est créée en plein écran et sans
décorations : sinon libdecor réserve une barre de titre que Cage n'affiche pas (bande noire).

### Clavier et plein écran

- **Fenêtres** : `App::assign_shortcuts` donne `F1` à `F4` aux fenêtres de tous (Pile de scans, Lots,
  Listes de matériel, Téléphone ; la Vérif s'ouvre d'elle-même au scan d'un lot) puis `F5` à `F12` à celles des menus Gestion puis Administration, dans l'ordre de `App::windows`
  (`AppWindow::shortcut`, rappelé dans les menus). `App::handle_shortcuts` les traite hors fenêtre
  modale ; la touche d'une fenêtre déjà au premier plan la ferme. `Ctrl+W` ferme la fenêtre active, `Ctrl+Espace` remet les fenêtres en place (`request_layout_reset`), `Super+L` déconnecte (même pendant une fenêtre modale),
  `Ctrl+Tab` est celui d'ImGui (`NavEnableKeyboard`).
- **Dialogues** : `escape_pressed()` à côté du bouton Annuler, `enter_pressed()` juste après le
  bouton principal (`button("Imprimer") || enter_pressed()`), qui l'entoure d'un liseré. `enter_pressed()`
  est faux si le bouton est désactivé, si le dialogue n'a pas le focus, à son ouverture, pendant un texte
  multiligne (`Ctrl+Entrée` y valide), quand la navigation clavier montre un autre élément (Entrée
  l'active), quand un champ a pris l'Entrée (`consume_enter()` : choix d'une suggestion dans
  `search_select` / `input_suggest`), et pour l'Entrée d'un scan en mode clavier : `main.cpp` appelle
  `note_typed_character()` à l'arrivée de chaque caractère et `note_enter_key()` sur Entrée ; une Entrée
  moins de 40 ms (plus la durée d'une frame) après un caractère est celle d'une douchette. Exprès sans
  Entrée : réglages d'usine de la douchette, vidage de sa mémoire (définitifs), Première configuration
  sans Échap.
- **Plein écran** (`Ctrl+F11`, `App::fullscreen_`) : `draw_windows` force chaque fenêtre ouverte sur
  toute la zone de travail sans la laisser déplacer ; `last_pos`/`last_size` ne sont plus mis à jour,
  ce qui rend sa place à chaque fenêtre à la sortie. Quitté par « Remettre les fenêtres en place » et
  après inactivité. Un double-clic sur la barre de titre d'une fenêtre entre en plein écran (ou en sort)
  avec cette fenêtre au premier plan, au lieu de la replier comme ImGui le ferait
  (`title_bar_double_clicked` prend le clic avant `Begin`) ; le repli reste sur la flèche.
- **Boutons** : `button()` et `small_button()` (`ui/widgets.hpp`) remplacent `ImGui::Button` et
  `ImGui::SmallButton`. Chaque bouton prend, dans l'ordre de dessin, la première lettre libre de sa
  fenêtre racine (initiales des mots d'abord), la souligne et répond à `Alt` gauche + lettre quand la
  fenêtre a le focus. Pas de lettre pour les petits boutons, ceux des tableaux et « Se déconnecter » (`danger_button(..., false)`), qui a `Super+L`. Pendant une saisie,
  le champ est d'abord quitté et le bouton agit à la frame suivante, sinon ImGui réécrirait le texte
  du champ. `main.cpp` jette les caractères tapés avec `Alt` gauche pour qu'ils n'arrivent pas dans le
  champ actif (`Alt Gr` n'est pas concerné).
- **Onglets** : `tab_item()` remplace `ImGui::BeginTabItem` ; `Ctrl+1` à `Ctrl+9` choisissent l'onglet
  de ce rang dans la fenêtre qui a le focus.

Tout nouveau bouton ou onglet passe par ces fonctions pour garder ses raccourcis.

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
PrintQueue (thread) ── AnyPrinter ─┬─ RawLabelPrinter ── TSPL (BITMAP) ou ZPL (^GFA) ── /dev/usb/lp0 ou IP:9100
                                   └─ NiimbotB1Printer ── SerialPort (/dev/ttyACM0, 115200 bauds), secours
```

- **Imprimantes TSPL / ZPL** (`printer/label_languages.cpp`) : l'image 1 bit part d'un bloc
  (`BITMAP` en TSPL, 0 = point noir ; `^GFA` en ZPL, 1 = point noir). La tête doit être en 203 dpi
  (8 points/mm), la résolution des modèles. Chaque impression porte tout le format, chaque réglage
  par la commande que la documentation du langage prévoit (rien n'est simulé dans le dessin) :
  - ZPL : `~SD` (noirceur), `~TA` (position de découpe), puis dans `^XA … ^XZ` : `^MT` (ruban),
    `^MNY` (étiquettes à interstice), `^PRv,v,v` (vitesse réglée, 2 pouces/s par défaut, impression + avance + recul), `^PW` = largeur de l'étiquette + 2 × marge
    (`zpl_side_margin` : 15 mm au plus, sans dépasser 104 mm de tête ; la zone est centrée sur la
    tête des Zebra de bureau et rien ne passe à gauche de son 0), `^LL` = hauteur,
    `^LHx,y` (x = marge + décalage horizontal, y = décalage vertical vers le bas), `^LS0`,
    `^LT` (décalage vertical vers le haut seulement, ≥ −120
    points ; la GK420t V61.17.17Z l'enregistre mais ne l'applique pas à l'impression, d'où `^LH`
    vers le bas), `^FO0,0^GFA`, `^PQ` ;
  - TSPL : `SIZE`, `GAP`, `DIRECTION`, `REFERENCE 0,0`, `DENSITY`, `SET RIBBON`, `SET TEAR ON`,
    `SHIFT x,y` (borné à ±1 pouce), `OFFSET` (position de découpe), `CLS`, `BITMAP`, `PRINT`.
  - Maintenance (`maintenance_command`) : calibrage ZPL = format sans champ + `~JC` + `^JUS`, TSPL
    `SIZE`/`GAP`/`SET RIBBON`/`GAPDETECT` ; avance `~PH` / `FORMFEED` ; configuration `~WC` /
    `SELFTEST` ; réglages d'usine `^JUF` + `^JUS` / `INITIALPRINTER`. Ces commandes passent dans la
    file (`PrintQueue::send_command`) avant les étiquettes, même file en pause, et ne sont pas
    comptées comme impressions.
  - État : `~HS` (ZPL, trois chaînes `STX … ETX` : papier, pause, tête ouverte, ruban…) ou
    `<ESC>!?` (TSPL, un octet de drapeaux), lu par `RawTransport::read_reply` (socket, ou
    `/dev/usb/lp0` ouvert en lecture-écriture). `RawLabelPrinter::print` le demande avant chaque
    étiquette : un état bloquant met la file en pause avec la raison. Sans réponse (liaison à sens
    unique), la connexion est marquée muette et l'impression continue sans contrôle.
  - **Envoyer les réglages et relire** (ZPL) : `zpl_send_settings_and_read` (format sans champ puis
    `^XA^HH^XZ`), passé par `PrintQueue::send_command_read` / `Printer::exchange` ; la réponse est
    découpée par `parse_zpl_config` (« valeur..... NOM ») et comparée aux valeurs envoyées.
  - Assistant **Changer de rouleau** (`app/roll_change.cpp`, `App::open_roll_wizard`) : écrans des
    gestes, puis `send_command` (réglages d'usine, calibrage au format du rouleau choisi via
    `printer_media_job`), `request_printer_state` et `set_loaded_label_type`. Ouvert depuis la
    fenêtre « Changez de rouleau » de la file en pause, le menu de l'impression, Impressions et
    consommables et les Réglages.

- **Modèles** : fichiers JSON `.qr` (`version`, `category`, `media` en mm, `elements` texte / QR /
  image, positions en mm). La catégorie (item, paquet, lot public, lot clé, scellé, ouverture du scellé, badge) détermine
  les placeholders disponibles (`core/placeholders.cpp`, liste affichée dans l'onglet Placeholders de
  l'éditeur). Un modèle par usage se choisit dans les Réglages ; sans modèle, le QR seul est imprimé.
- **Types d'étiquettes** : un modèle peut porter un `label_type` (nom, `width_mm` dans le sens de la tête,
  `height_mm` dans le sens du défilement, `rotation` 0/90/180/270 sens horaire, `support` `direct` ou
  `transfer`, marges `LabelMargins` sur l'étiquette qui sort de l'imprimante : `margin_mm` si les
  quatre sont égales, écrit seulement s'il diffère de 1 mm, sinon `margin_top_mm`, `margin_bottom_mm`,
  `margin_left_mm`, `margin_right_mm` ; `LabelType::design_margins` les tourne vers le modèle). Un modèle qui n'en a pas en reçoit un à la lecture (`render/label_types.cpp` :
  `default_label_type` d'après l'étiquette réglée et le quart de tour qu'il recevait, au démarrage et à chaque
  relecture de l'éditeur) et son fichier est réenregistré ; un dossier en lecture seule le laisse en format
  libre, imprimé comme avant. La liste des
  types est reconstruite à la lecture des modèles (`collect_label_types`) ; un type créé dans l'éditeur
  (onglet « Types d'étiquettes ») reste en mémoire jusqu'à ce qu'un modèle qui l'utilise soit enregistré.
  À l'impression, `fit_to_label` impose l'orientation du type et la taille de son rouleau, blanchit tout ce
  qui sort du cadre des marges (`PhysicalLabel::margins`, une par côté, tracé en pointillé orange dans l'éditeur et
  repris par le cadre plein de `calibration_image`), le ruban suit le
  support, et la file se met en pause si le type n'est pas celui chargé (Réglages > « Type chargé ») ; un
  front d'un autre ordinateur envoie le type avec l'étiquette rendue (`label_prints._check_label_type`).
- **Dossier des modèles** (`core/paths.cpp`) : réglage de l'application, sinon
  `$QRPROTEC_TEMPLATES_DIR`, sinon `app/templates` du dépôt (chemin compilé), sinon le dossier
  courant. Les images (logos) vont dans `images/`.
- **QR codes** : bibliothèque Nayuki (`third_party/qrcodegen`), masque choisi automatiquement, marge
  et niveau de correction réglables ; l'éditeur signale un QR à moins de 2 pixels par module.
- **Police** : DejaVu Sans fournie (`third_party/fonts`), gras réel si `DejaVuSans-Bold.ttf` est
  trouvé, sinon gras synthétique. `QRPROTEC_FONT` impose une autre police.
- **Imprimante occupée** : la B1 refuse parfois une commande pendant qu'elle termine l'étiquette
  précédente (code `0x31`) ; la commande est réessayée.

- **Impressions demandées depuis le web** (`app/label_prints.cpp`) : le poste principal
  (`App::main_poste()` : API locale, ou clé marquée poste d'après `/api/health/`) interroge
  `GET /api/label-prints/pending/` toutes les 20 s, connecté ou non (route des postes, sans session).
  Dès qu'il y voit une nouvelle demande, il ouvre le WebSocket `/ws/label-prints`
  (`App::update_label_prints_live`, `WebSocketLink`) : chaque changement de la file (`changed`) la lui fait
  relire aussitôt ; tant qu'il est connecté, l'interrogation ralentit à une fois par minute, filet si un signal se
  perd. Une connexion muette (aucune trame, pas même le pong d'un ping, depuis 45 s) est coupée et refaite. 15 min sans nouvelle impression
  (`LabelPrints::live_until`) : le WebSocket se ferme et l'interrogation toutes les 20 s reprend. Un
  serveur sans ce WebSocket (404) laisse l'interrogation seule. Bouton « Rafraîchir » dans la fenêtre.
  Une demande jamais vue ouvre la fenêtre « Impressions en attente », dessinée par `App` hors de la
  liste `windows` (pas de rôle, pas remise à zéro à l'inactivité). « Lancer l'impression » envoie
  `POST /api/label-prints/<id>/print/` pour chaque demande : le serveur crée les items et les renvoie,
  et leurs étiquettes partent dans la file comme une réception (`printed_by` = l'admin qui a demandé).
  Voir `database/inventory/label_prints.py`.
- **Front d'un autre ordinateur** (clé de front, réglage « Imprimante branchée sur cet ordinateur »
  décoché) : `App::print_documents` n'imprime pas, il rend chaque étiquette (`render_template`) et
  l'envoie en image 1 bit base64 (`encode_raster_bits`) par `POST /api/label-prints/send/`, par lots de
  1,5 Mo au plus. Le poste la reçoit comme une demande (`kind` labels) et l'imprime telle quelle
  (`PrintJob::image`, recadrée à son étiquette physique par `fit_to_label`). Seul le poste (sans clé
  de front) interroge la file. Une demande pas imprimée expire en 24 h.
- **Files d'impression** : la réponse de `/api/label-prints/pending/` groupe les demandes en `queues`
  (une par `LabelPrint.queue` : identité de qui demande, sinon `front:<ordinateur>`), avec le matricule
  pour retrouver la file d'un badge. Plusieurs files : choix par bouton radio ou par badge
  (`App::label_prints_badge`, appelé en tête de `handle_scan` tant que la fenêtre est ouverte : pas de
  connexion, bouton « Se connecter » qui appelle `login_with_badge`). Une file lancée à distance
  (`launch_requested`, `POST /api/label-prints/queues/launch/` par la gestion ou un admin du web ou d'un autre
  ordinateur) part à l'imprimante au tour suivant, une file à la fois. Sur un autre ordinateur, la
  gestion et les admins ouvrent la même fenêtre en mode `remote` (Impression > Files d'impression du poste), qui
  interroge `POST /api/label-prints/queues/`. Fenêtre fermée 15 min après la dernière demande.
- **Compteur d'impressions** (`core/print_counter.cpp`, fenêtre `print_counts_window.cpp`) : le fil
  de `PrintQueue` ajoute chaque étiquette acceptée par l'imprimante (`print` réussi) au `PrintCounter`
  de `App`, par jour et par catégorie : largeur × hauteur du type d'étiquette du modèle (sinon étiquette
  des Réglages) et support (direct / transfert, sans l'orientation). Gardé dans `print-counts.json`
  (à côté de `app.conf`) et envoyé toutes les 30 s par `POST /api/print-counts/` (tout front, sans
  session) en lots identifiés : un lot en cours d'envoi ne change plus et est renvoyé tel quel tant que
  le serveur ne l'a pas acquitté, le serveur ignore un lot déjà reçu (`PrintCountBatch`). Un lot refusé
  (400) est abandonné et journalisé. La fenêtre lit `GET /api/print-counts/?year=` (gestion et admin) :
  12 mois par catégorie, totaux. Voir `database/inventory/print_counts.py`.
- **Consommables** (onglets Consommables et Références de la même fenêtre) : `GET/PATCH
  /api/consumables/`, `POST /api/consumables/refs/`, `PATCH /api/consumables/refs/<id>/`,
  `POST /api/consumables/moves/` (réception ou inventaire), `POST /api/consumables/moves/<id>/delete/`.
  Stock estimé, autonomie et coûts calculés par `database/inventory/consumables.py`. La tâche
  « Commander » (lien `consumables`) n'est ajoutée qu'à `GET /api/tasks/` (poste), pas au web.
  Rythme : impressions hors mise en place (`SETUP_DAYS`), et pour la catégorie des étiquettes d'items
  (`item_label_pool`, `PATCH /api/consumables/` ; vide = la plus imprimée) au moins la consommation
  d'items ; `rate_source`, `measured_monthly` et `planned_monthly` en infobulle. La fenêtre Réception
  (`inventory_window.cpp`, `draw_consumable_reception`) enregistre aussi une réception de consommables.
- **Changement de rouleau** : file en pause sur un modèle d'un autre type → `App::draw_roll_change_modal`
  (« Impression en pause ») ; le rouleau en place se choisit aussi en haut de la fenêtre des
  consommables (`App::set_loaded_label_type`, sans avertissement).
- **Codes d'action rapide des postes** (`app/poste_actions.cpp`) : QR « `qrprotec:poste/ACTION?param=valeur` »,
  pas une URL web. `parse_scan` les reconnaît (`ScanKind::PosteAction`, `id` = action, `params`) et
  `App::handle_scan` les exécute tout de suite par `App::handle_poste_action`, sans toucher à la vérif ni à
  la pile, qu'ils viennent de la douchette ou du téléphone utilisé comme douchette. Le front web les
  ignore (`handleCode` de `web/app/scans.js`). Chaque action est une ligne de `kActions` : nom, droits (`Anyone`,
  `Privileged`, `Admin` : personne connectée), fonction. Une action inconnue (poste pas à jour) est
  signalée sans rien faire. Une action doit rester sans danger si le code est scanné par erreur.
  Première action : `rouleau?t=NOM&w=&h=&s=` déclare le rouleau en place (par son nom, sinon par taille et
  support parmi les types des modèles du poste). Son étiquette (`roll_code_document`,
  `render/label_types.cpp`) s'imprime depuis « Imprimer le QR d'un rouleau... », sur un rouleau choisi.

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
`<base>/scanner?s=…&k=…`, et `WebSocketLink` se connecte au WebSocket
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
- `http://` vers une autre machine est refusé (`ApiEndpoint::parse`) : la clé, le jeton et la session de
  l'utilisateur y passeraient en clair. Pour un essai sur le réseau local seulement :
  `QRPROTEC_ALLOW_INSECURE_HTTP=1`.

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
