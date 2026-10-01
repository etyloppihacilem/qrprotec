# Recettes de maintenance

Comment faire les modifications courantes sans rien oublier. Chaque recette liste **tous** les
endroits à toucher : c'est l'oubli d'un des fronts ou de la documentation qui coûte cher.

## Environnement de développement

```sh
git clone --recurse-submodules https://github.com/etyloppihacilem/qrprotec
cd qrprotec

# Back
poetry install --no-root          # ou : python3 -m venv .venv && pip install django djangorestframework cryptography
cd database
python manage.py migrate
python manage.py createadmin M001 Nom Prénom --pin 1234
python manage.py serve            # API publique :8000, API locale :8001
python manage.py test inventory

# Front poste (autre terminal)
cd app
cmake -S . -B build && cmake --build build -j && (cd build && ctest)
./build/QRProtecApp --verbose     # se connecte à http://127.0.0.1:8001 par défaut
```

Sans douchette : la fenêtre Pile de scans permet de saisir un code à la main ; on peut aussi taper un
code très vite (mode HID). Sans imprimante : l'aperçu montre l'étiquette et « Exporter PNG » dans
l'éditeur produit l'image. Avec un téléphone : `serve --https` (voir [front-web.md](front-web.md)).

En DEBUG (défaut hors borne), la clé secrète Django est une valeur de développement : ne jamais
déployer sans `QRPROTEC_SECRET_KEY` et `QRPROTEC_DEBUG=0` (le paquet s'en charge).

## Ajouter un champ à un modèle

1. `inventory/models.py` : le champ, avec une valeur par défaut (les bases existantes doivent migrer
   seules au démarrage de la borne).
2. `python manage.py makemigrations inventory` ; relire la migration, la commiter.
3. `inventory/serializers.py` : l'exposer dans le `dict` concerné. Attention aux données sensibles :
   `lot_dict(local=False)` est ce que voit un téléphone.
4. `inventory/views.py` : l'accepter en écriture si besoin (valider le format, message d'erreur en
   français).
5. Fronts : `app/src/ui/windows/…` (lecture tolérante `json["champ"].str()`), `web/app.js`.
6. Si c'est une valeur d'étiquette : voir « Ajouter un placeholder ».
7. Test dans `inventory/tests.py`, puis README racine si c'est visible de l'utilisateur.

## Ajouter une route d'API

1. La vue dans `views.py` avec `@api_view([...])` et `@handle_errors`. Lever `ApiError(message, code)`
   pour une erreur.
2. La déclarer dans `inventory/urls.py` :
   - `local_patterns` si c'est de la gestion (poste uniquement) ;
   - `public_patterns` si un téléphone en a besoin. Alors : **toute écriture exige une clé**
     (`require_lot_key`, `verif_targets`) ou un badge (`badge_user`), et rien de secret ne doit être
     renvoyé.
   - Une route locale qui partage un préfixe avec une route publique doit être déclarée **avant**
     dans la liste combinée (`local_patterns + public_patterns`).
3. Ajouter un test qui vérifie qu'une route locale répond 404 sur l'API publique
   (`PublicApiTests.test_management_routes_are_local_only` donne le modèle).
4. Mettre à jour la liste des routes du README racine.

## Modifier une règle de vérif ou de répartition

1. `services.py` (`perform_verif`, `assign_items`, `_complete_lots`, `requirements_status`).
2. **Les deux aperçus** : `App::plan_verif` / `App::partial_verif_lots` dans `app/src/app/app.cpp`,
   et `planSession` / `partialLots` dans `database/inventory/web/app.js`.
3. Si l'état d'un lot change : `serializers.lot_state` / `aggregate_tree` (les fronts affichent
   `state` et `effective` tels quels).
4. Tests (`VerifTests`, `SubLotTests`…), puis [regles-de-gestion.md](regles-de-gestion.md) et le
   guide utilisateur.

## Ajouter un placeholder d'étiquette

1. `app/src/core/placeholders.cpp` : l'ajouter à la catégorie (nom, description, valeur d'exemple) —
   il apparaît dans l'onglet Placeholders de l'éditeur et dans l'aperçu.
2. `app/src/app/labels.cpp` : le remplir depuis le JSON de l'API (`item_parameters`,
   `lot_parameters`…).
3. Si la valeur vient du serveur : l'exposer dans `serializers.py` (et sur l'API locale seulement si
   elle est sensible, comme `lot_key`).

## Ajouter une fenêtre au poste

1. `app/src/ui/windows/ma_fenetre.cpp` : une classe dérivée d'`AppWindow` (`draw`, éventuellement
   `on_open`), et `make_ma_fenetre_window()`.
2. La déclarer dans `windows.hpp`, l'ajouter dans `CMakeLists.txt` et dans le constructeur d'`App`
   (`windows.push_back(...)`).
3. `privileged=true` pour le menu Gestion, `admin_only=true` pour les admins.
4. Disposition par défaut : dans `AppSettings::defaults()` (`settings.cpp`).
5. Requêtes : toujours `app.api.get/post(..., callback)`, jamais de réseau bloquant dans `draw`.

## Ajouter un réglage au poste

`app/src/app/settings.hpp` (champ et valeur par défaut), `settings.cpp` (`load`/`save`, format
`clé=valeur`), `ui/windows/settings_window.cpp` (champ), puis l'utiliser via `app.settings`. Un
fichier `app.conf` ancien sans la clé doit continuer à fonctionner (valeur par défaut).

## Ajouter une option du back

`qrprotecDB/settings.py` (dans `QRPROTEC`, lue depuis `QRPROTEC_*`), `packaging/files/qrprotec.conf`
(commentée, avec explication), tableau des variables du README racine. Lire avec
`models.qrprotec_setting('NOM')`.

## Ajouter un fichier au front web

Le placer dans `database/inventory/web/` **et** l'ajouter à `web_views.ASSETS` (liste blanche), sinon
il répond 404. Pas de CDN : fournir la bibliothèque dans `web/vendor/` avec sa licence.

## Changer de modèle d'imprimante

Implémenter `Printer` (`app/src/printer/printer.hpp`) pour le nouveau protocole et remplacer le membre
`NiimbotB1Printer printer_` de `PrintQueue` (`app/src/app/print_queue.hpp`) et de l'éditeur
(`app/src/ui/editor.hpp`, commandes directes). Le rendu (`render/raster.cpp`) produit une image 1 bit au pas de 8 px/mm (203 dpi) ;
adapter `pixels_per_mm` des modèles si l'imprimante a une autre résolution.

## Mettre à jour les dépendances

- Python : `poetry update` à la racine, puis mettre à jour les `%global *_version` de
  `packaging/qrprotec.spec` (le `%prep` échoue sinon, volontairement), lancer les tests.
- ImGui : `cd app/imgui && git checkout <tag>` puis commiter le sous-module ; mettre à jour
  `Provides: bundled(imgui)` dans le spec.
- SDK Inateck : sous-module `app/scanner_lib`.

## Publier une version

```sh
git tag v1.3.0 && git push origin v1.3.0
```

La CI construit, teste, publie la release et le dépôt dnf ; les bornes la reçoivent au prochain
`dnf upgrade`. Voir [packaging.md](packaging.md#publication-ci).

## Diagnostiquer sur une borne

```sh
journalctl -u qrprotec -f            # back (requêtes, migrations, erreurs)
journalctl -u qrprotec-kiosk -b      # Cage et front ; ajouter QRPROTEC_FRONT_ARGS=--verbose dans kiosk.conf
journalctl -u caddy -f               # certificats, proxy
sudo qrprotec-manage shell           # shell Django sur la base de production (prudence)
sudo qrprotec-manage dbshell         # SQL brut (sqlite3 doit être installé)
```

Une console est disponible sur la borne avec `Ctrl+Alt+F2`.
