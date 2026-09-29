# QRProtec

Inventaire par QR code du matériel de secours (consommable ou non) : chaque item porte une étiquette
avec un identifiant unique, les lots (sacs, malles...) sont vérifiés en scannant leur contenu.

- `database/` : back Django (base de données + API).
- `app/` : front ImGui (poste local : douchette Inateck, imprimante Niimbot B1).

## Identifiants et QR codes

| Objet | Contenu du QR code | Exemple |
|---|---|---|
| Item | l'iid seul : type (6) + péremption `AAAAMMJJ` (8) + compteur base 62 (8) | `serphy20271231000000A1` |
| Item non périssable | date `00000000` | `garrot00000000000000A1` |
| Lot, étiquette publique | `<base>/verif?lot=ID` | `https://example.com/verif?lot=sacpse00000001` |
| Lot, étiquette privée | `<base>/verif?lot=ID&key=CLE` | `...verif?lot=sacpse00000001&key=a1B2...` |
| Badge utilisateur | `<base>/badge?m=MATRICULE&key=CLE` (clé valable 1 an) | `...badge?m=M0042&key=...` |
| Paquet fermé | `<base>/pack?id=ID` | `...pack?id=0000002B` |

`<base>` vaut `https://example.com/` par défaut et se change avec la variable d'environnement
`QRPROTEC_PUBLIC_BASE_URL` du back. Le front reconnaît les QR codes quel que soit le domaine : changer
la base n'invalide pas les étiquettes déjà imprimées.

## Back (Django)

```sh
cd database
poetry install --no-root          # ou : pip install django djangorestframework
python manage.py migrate
python manage.py serve            # API publique 0.0.0.0:8000 + API locale 127.0.0.1:8001
python manage.py createadmin M001 Nom Prenom   # premier responsable (ou badge responsable perdu)
python manage.py serve --https    # API publique en HTTPS (certificat de développement, tests sur téléphone)
python manage.py test inventory
```

Deux API sur deux ports, sélectionnées par le port qui reçoit la requête
(`inventory/middleware.py`) :

- **API publique** (`qrprotecDB/urls.py`) : lecture d'un item, d'un lot, d'un paquet, confirmation
  d'un badge. Toute écriture exige la clé de l'objet modifié (ex : clé du lot pour une vérif).
  Pour l'HTTPS, placer un reverse proxy (nginx, caddy) devant le port public.
- **API locale** (`qrprotecDB/urls_local.py`) : gestion complète sans clé (types, réception, lots,
  utilisateurs, stocks) et admin Django. N'accepte que les adresses de
  `QRPROTEC_LOCAL_API_ALLOWED_ADDRESSES` (localhost par défaut) et, si défini, le jeton
  `QRPROTEC_LOCAL_API_TOKEN` dans l'en-tête `X-QRProtec-Token`.

En production : `gunicorn qrprotecDB.wsgi:public_application` et
`gunicorn qrprotecDB.wsgi:local_application` sur deux ports.

### Variables d'environnement

| Variable | Défaut |
|---|---|
| `QRPROTEC_PUBLIC_BASE_URL` | `https://example.com/` |
| `QRPROTEC_PUBLIC_API_ADDRESS` / `_PORT` | `0.0.0.0` / `8000` |
| `QRPROTEC_LOCAL_API_ADDRESS` / `_PORT` | `127.0.0.1` / `8001` |
| `QRPROTEC_LOCAL_API_ALLOWED_ADDRESSES` | `127.0.0.1,::1` |
| `QRPROTEC_LOCAL_API_TOKEN` | vide (pas de jeton) |
| `QRPROTEC_ALLOWED_HOSTS` | `localhost,127.0.0.1,[::1]` |
| `QRPROTEC_DEBUG_HOSTS` | `192.168.1.201` (ajoutés à `ALLOWED_HOSTS` en mode DEBUG seulement) |
| `QRPROTEC_MISSING_AFTER_VERIFS` | `3` |
| `QRPROTEC_LOT_KEY_VALIDITY_DAYS` | `3650` |
| `QRPROTEC_SECRET_KEY`, `QRPROTEC_DEBUG`, `QRPROTEC_DB_PATH` | réglages Django |

### Routes (`/api/...`)

Publiques et locales : `health/`, `auth/` (POST matricule + key), `items/<iid>/`, `lots/<id>/`,
`lots/<id>/verif/` (POST items, key), `lots/<id>/add/` (POST items, key), `packs/<id>/`.

Locales uniquement : `item-types/`, `item-types/<type>/`, `items/` (recherche), `items/batch/`
(réception), `items/to-stock/`, `items/<iid>/delete/`, `items/<iid>/restore/`, `stock/`,
`stock/verif/`, `packs/`, `packs/<id>/open/`, `lot-types/`, `lot-types/<type>/`,
`lot-types/<type>/requirements/`, `lots/`, `lots/<id>/update/`, `lots/<id>/rotate-key/`,
`lots/<id>/verifs/`, `users/`, `users/<matricule>/`, `users/<matricule>/renew-key/`.

Sur l'API publique, l'utilisateur est transmis sous la forme `"user": {"matricule": ..., "key": ...}`
(vérifié) ou `"name": ...` (déclaré). Sur l'API locale, `"user": "MATRICULE"` suffit.

### Règles de gestion

- **Vérif** : les items scannés sont placés dans le lot (ou le stock). Un item périmé scanné alors que
  des items frais du même type viennent d'arriver dans le lot est considéré comme **remplacé** et sort
  du lot. Les items attendus mais non scannés sont signalés ; ils passent **disparus** s'ils sont
  périmés ou après `QRPROTEC_MISSING_AFTER_VERIFS` vérifs manquées. Les exigences du lot ne comptent
  que les items vus à la dernière vérif.
- **Suppression** : les items ne sont pas supprimés par les utilisateurs. Un responsable peut
  exceptionnellement marquer un item supprimé (raison obligatoire) et le restaurer.
- **Paquet fermé** : à la réception, les items sont créés et une seule étiquette de paquet est
  imprimée ; les étiquettes individuelles sont imprimées à l'ouverture. En mode privilégié, scanner
  l'étiquette du paquet ouvre sa fiche (contenu, péremption, réception, ouverture) avec le bouton
  « Ouvrir le paquet et imprimer les N étiquettes » (aperçu puis impression). Rouvrir un paquet déjà
  ouvert réimprime ses étiquettes sans changer sa date d'ouverture.

## Front web (téléphone)

Page unique servie par le back aux adresses mêmes des QR codes : `/verif?lot=…[&key=…]`,
`/badge?m=…&key=…`, `/pack?id=…` et `/`. Scanner une étiquette avec l'appareil photo du téléphone
ouvre donc directement la bonne vue. Les fichiers sont dans `database/inventory/web/`.

- **Moitié haute** : caméra arrière et viseur. Décodage natif (`BarcodeDetector`) quand le navigateur
  le propose, sinon [jsQR](https://github.com/cozmo/jsQR) (Apache 2.0, fourni dans `web/vendor/`,
  aucun CDN). Lampe si le téléphone le permet, écran maintenu allumé pendant le scan.
- **Moitié basse** : informations du dernier scan (item : type, péremption, emplacement ; lot : état,
  dernière vérif) et trois onglets : **À scanner** (items attendus du lot, en orange, les périmés en
  rouge), **Scannés** (avec ✕ par ligne, « Annuler le dernier », « Vider la liste ») et **Lot**
  (exigences scannées / attendues).
- **Produit périmé ou code inconnu** : écran rouge qui clignote, bip grave et vibration (la vibration
  n'existe pas sur iPhone). Un item déjà scanné n'est pas ajouté une seconde fois.
- **Badge** : scanner son badge connecte l'utilisateur (conservé sur le téléphone jusqu'à
  déconnexion). **Valider la vérif** exige l'étiquette privée du lot et un badge ; le menu ⋯ permet
  aussi d'ajouter les items scannés au lot, de saisir un code ou un identifiant de lot à la main, de
  changer de lot et de se déconnecter.
- La liste en cours est conservée si la page est rechargée. La clé présente dans l'URL est retirée de
  la barre d'adresse.

Pour tester sur un téléphone du réseau local : `python manage.py serve --https`, puis ouvrir
`https://192.168.1.201:8000/verif` et accepter l'avertissement du certificat auto-signé (généré dans
`database/.dev-certs/`, non versionné, pour localhost et les hôtes autorisés). `--cert` et `--key`
permettent d'utiliser un vrai certificat.

La caméra n'est accessible qu'en **HTTPS** (ou sur `localhost`) : l'API publique doit être derrière
un reverse proxy HTTPS, sur le domaine de `QRPROTEC_PUBLIC_BASE_URL`.

## Front (ImGui)

```sh
cd app
git submodule update --init
cmake -S . -B build && cmake --build build -j
cd build && ctest
./build/QRProtecApp   # les modèles *.qr sont lus et enregistrés dans app/templates/
```

Dépendances : GLFW, OpenGL, libpng, FreeType, libxdo (SDK Inateck). L'encodeur QR
([Nayuki](https://www.nayuki.io/page/qr-code-generator-library), MIT) et `stb_image` (PNG/JPEG,
domaine public) sont fournis dans `app/third_party/`.

**Première utilisation** : tant qu'aucun responsable n'a de badge valide, le logiciel propose de créer
le compte du responsable technique ; il est connecté directement en mode privilégié et peut imprimer
son badge (ou utiliser `manage.py createadmin` sur le serveur).

La police DejaVu Sans (accents) est fournie dans `app/third_party/fonts/` et copiée à côté de
l'exécutable ; une autre police peut être imposée avec la variable `QRPROTEC_FONT`.

Les réglages sont dans `~/.config/qrprotec/app.conf` et s'éditent depuis **Gestion > Réglages** :
URL de l'API locale, modèle d'étiquette par usage, imprimante, délai de réinitialisation,
disposition par défaut des fenêtres, signal de mauvais scan.

### Mode normal

- **Pile de scans** (colonne de droite, toujours ouverte) : chaque scan s'y ajoute ; les périmés et
  les codes inconnus sont en rouge, les doublons en jaune, avec des boutons pour annuler le dernier
  scan, retirer une ligne, nettoyer ou vider la pile. Après avoir scanné l'**étiquette privée** d'un
  lot : **Ajouter au lot** (range les items scannés) ou **Valider comme vérif** (le contenu du lot
  devient la pile).
- **Lots** : état de chaque lot (périmés, bientôt périmés, complet), détail du contenu, lancement
  d'une vérif.
- **Vérif** : le scan de l'étiquette **publique** d'un lot ouvre sa fiche (état, contenu, bouton
  « Lancer une vérif ») ; le scan de l'étiquette **privée** lance directement la vérif, clé
  pré-remplie. L'état de chaque lot est affiché en vert (vérifié et complet) ou en rouge
  (incomplet, périmés, jamais vérifié). Les attendus suivent la définition du type de lot : pour
  chaque type d'item, scannés / attendus, puis les items connus du lot et ce qu'il faut prendre dans
  le stock. Liste des items attendus : chaque item scanné bascule dans la pile et disparaît de la
  liste. Hors mode responsable, la validation exige l'étiquette privée du lot.
- **Connexion** : scanner son badge. Le nom de l'utilisateur connecté est affiché en haut à droite et
  dans la pile, avec un bouton **Se déconnecter**. La connexion n'est demandée qu'au moment de valider une vérif ou un
  ajout (badge scanné ou code saisi dans la fenêtre de connexion).
- **Mode HID** : une douchette en mode clavier est reconnue à la vitesse de frappe (délai réglable,
  50 ms par défaut, dans Douchette > Paramètres) ; le temps pendant lequel l'application dessine
  une image est déduit, pour ne plus couper le début des scans quand l'interface est chargée.
- **Mauvais scan** (produit périmé, code inconnu) : bip et LED de la douchette via le SDK Inateck ;
  en mode HID, bip de l'ordinateur et clignotement rouge de l'écran.
- **Douchette** : la recherche et la connexion sont accessibles à tous ; les paramètres (mode HID,
  volume, préfixe…) et la déconnexion demandent un utilisateur connecté.
- **Inactivité** : après 15 min (réglable), la pile est vidée, l'utilisateur déconnecté et les
  fenêtres remises à leur place par défaut.

### Mode privilégié (badge responsable)

Fond orange. Menu **Gestion** :

- **État des stocks** : barre par type, verte au-dessus du minimum, orange en dessous, rouge à 0,
  avec « quantité/minimum » (ex : `32/100`).
- **Inventaire** : réception d'une commande (type, péremption, quantité, paquet fermé) avec
  impression des étiquettes en série, types d'items, recherche d'items, réimpression d'une étiquette,
  suppression exceptionnelle, liste des paquets fermés.
- **Paquet fermé** : fiche d'un paquet (ouverte en scannant son étiquette, depuis la dernière
  réception ou la liste des paquets) et ouverture avec impression de toutes ses étiquettes.
- **Gestion des lots** : types de lots et contenu attendu, création de lots, étiquettes publique et
  privée, régénération de la clé.
- **Utilisateurs** : création, droits responsable, renouvellement et impression des badges.
- **Éditeur d'étiquettes** : modèles avec usage (item, paquet, lot public, lot privé, badge),
  onglet **Placeholders** listant les `{{placeholders}}` disponibles, textes, QR codes et images
  (logo PNG ou JPEG).
- **Réglages**.

**Impression** : la taille des étiquettes chargées dans l'imprimante (largeur dans le sens de la tête,
hauteur dans le sens du défilement) se règle dans **Réglages**. Un modèle dessiné dans l'autre sens
(portrait / paysage) est tourné d'un quart de tour à l'impression (sens réglable, retournement à 180°
possible) et centré. Sans modèle choisi pour un usage, seul le QR code est imprimé, centré.

**Saisie** : les types (items, lots) se cherchent en tapant une partie du nom ou du code, sans se
soucier des accents ; les résultats s'affichent en direct (↑/↓, Tab ou Entrée pour valider). Les
dates de péremption se tapent librement : `02/09/2026`, `2/9/26`, `020926`, ou seulement le mois
(`09/2026`, `09/26`, `0926` = dernier jour du mois) ; la date comprise est affichée à côté.

**Imprimante occupée** : quand la Niimbot refuse une commande de préparation parce qu'elle termine
l'étiquette précédente (code 0x31…), la commande est réessayée automatiquement.

**Aperçu** : à la création d'un type d'item (étiquette d'exemple), d'un lot (étiquettes publique et
privée), d'un utilisateur (badge) et à la réception d'une commande, une fenêtre montre les étiquettes
avant impression, avec les boutons pour les imprimer.

**QR codes** : le masque de chaque QR est choisi automatiquement parmi les 8 de la norme (pénalité
minimale, bibliothèque Nayuki). Chaque QR garde une marge blanche (2 modules par défaut) et un niveau
de correction réglables dans l'éditeur, qui signale un QR trop petit pour son contenu (moins de
2 pixels par module).

Dans l'éditeur, les textes peuvent être en **gras** et alignés (gauche, centré, droite). Le bouton
**Ajouter le titre** insère `{{titre}}` centré et en gras ; son texte (« PROTECTION CIVILE / PARIS
CENTRE » par défaut) se change dans les Réglages et s'applique à toutes les étiquettes.

**Modèles versionnés** : les modèles `*.qr` et leurs images (logo) sont lus et enregistrés dans
`app/templates/`, quel que soit le dossier de lancement. Commitez-y vos modèles pour les retrouver à
chaque clone (autre dossier possible dans les Réglages ou via `QRPROTEC_TEMPLATES_DIR`).

`app/templates/` contient un modèle d'exemple par usage (40 × 30 mm). Les modèles de lot public et
de badge affichent `logo.png` : copier le logo de la protection civile sous ce nom dans le dossier
des modèles.
