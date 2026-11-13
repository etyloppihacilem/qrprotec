# Packaging et déploiement

Dossier `packaging/`. QRProtec est distribué en **RPM pour Fedora** (42 et plus), publié dans un
dépôt dnf statique. Ce document explique comment le paquet est construit et ce qu'il installe ; le
mode d'emploi pour l'administrateur est dans le [guide d'installation](../installation/README.md).

## Pourquoi un RPM Fedora

La cible est une **borne** : un mini-PC posé dans un local, administré par des bénévoles, qui doit
se mettre à jour seule et redémarrer dans un état connu. Un paquet système apporte exactement cela :
dépendances résolues par `dnf`, mises à jour par `dnf upgrade`, fichiers de configuration préservés
(`%config(noreplace)`), services systemd activés et redémarrés proprement, désinstallation propre.
Fedora Server a été retenue pour Cage, Caddy et Python récents dans les dépôts officiels, et SELinux
actif par défaut. Voir [decisions.md](decisions.md#borne-fedora-en-rpm-cage-caddy).

## Fichiers

| Fichier | Rôle |
|---|---|
| `qrprotec.spec` | Spec RPM, produit plusieurs paquets (voir ci-dessous). `%bcond kiosk` (activé par défaut) : `--without kiosk` ne produit que `qrprotec-server` et `qrprotec-common`, sans compiler le front. |
| `make-sources.sh` | Prépare `Source0` (code + sous-modules `imgui`, `scanner_lib`) et `Source1` (dépendances Python figées). |
| `lock2requirements.py` | Convertit `poetry.lock` en `requirements.txt` avec empreintes (`--require-hashes`). |
| `vendor-macros.py` | Écrit les macros rpm (`%vendor_provides`, `%vendor_dist_infos`) des dépendances Python embarquées. |
| `build-rpm.sh` | `rpmbuild` complet (appelé par `make rpm` dans un conteneur, ou `make rpm-local`). |
| `Containerfile` | Image Fedora de construction, avec les dépendances de build en cache. |
| `version.sh` | Version depuis le tag git : `v1.2.0` → `1.2.0`, entre deux tags `1.2.0^3.gabc1234`. |
| `publish-repo.sh` | Met à jour le dépôt dnf statique (`createrepo_c`, signature optionnelle). |
| `rpmlint.toml` | Exceptions justifiées pour `rpmlint`. |
| `files/` | Tout ce qui est installé hors code : unités systemd, configurations, scripts. |

## Les paquets

| Paquet | Contenu | Dépend de |
|---|---|---|
| `qrprotec` | Paquet meta : la borne complète | `-server`, `-kiosk` |
| `qrprotec-server` | Back, Caddy (fragment et drop-in), alertes, `qrprotec-manage`, utilisateur `qrprotec` | `-common`, `caddy`, `python3-cryptography` |
| `qrprotec-kiosk` | Session Cage sur `tty1`, `kiosk.conf`, `front-api.conf`, logind/sleep, utilisateur `qrprotec-kiosk` | `-front`, `-common`, `cage` et bibliothèques Wayland |
| `qrprotec-front` | Exécutable, lanceur `/usr/bin/qrprotec-front`, SDK douchette, modèles, `qrprotec.desktop` et icône | polices DejaVu |
| `qrprotec-common` | `qrprotec-setup`, `qrprotec-backup`, son timer | — |

`qrprotec-setup` et `qrprotec-backup` détectent ce qui est installé (back, kiosk) et ne traitent que
ces parties. Une borne installée avec l'ancien paquet unique passe aux nouveaux paquets au
`dnf upgrade` sans perdre sa configuration.

## Ce qui est embarqué

- **Back** : `database/` copié dans `/usr/share/qrprotec/backend` (sans `tests.py`), dépendances
  Python (Django, DRF, asgiref, sqlparse) dans `/usr/share/qrprotec/vendor`, compilées en `.pyc`. Les
  versions sont celles de `poetry.lock`. `vendor-macros.py` en tire `qrprotec-vendor.macros`
  (Source4), que le spec charge pour ses `Provides: bundled(...)` : rien à changer dans le spec après
  un `poetry update`, même pour une nouvelle dépendance. `%prep` vérifie que ces macros décrivent
  exactement `vendor/`.
  `python3-cryptography` vient de Fedora (bibliothèque compilée, mises à jour de sécurité du
  système).
- **Front** : compilé en Release, installé en `/usr/libexec/qrprotec/qrprotec-front` ; SDK Inateck
  (x86_64 seulement) en bibliothèque privée `/usr/lib64/qrprotec/` (exclue des `Provides`) ; modèles
  d'étiquettes dans `/usr/share/qrprotec/templates`.

Pourquoi embarquer Django plutôt que `python3-django` de Fedora : la version de Fedora suit son
propre rythme et une montée de version majeure de Django pendant un `dnf upgrade` du système pourrait
casser la borne. Les versions embarquées sont exactement celles testées.

## Services systemd

| Unité | Rôle |
|---|---|
| `qrprotec.service` | Back : `qrprotec-manage migrate` puis `serve --public 127.0.0.1:8000 --local 127.0.0.1:8001`, utilisateur `qrprotec`, fortement durci (`ProtectSystem=strict`, `PrivateDevices`, filtre d'appels système…), redémarrage sans limite. |
| `caddy.service` + `caddy.service.d/qrprotec.conf` | Caddy de Fedora, avec `/etc/qrprotec/qrprotec.conf` en `EnvironmentFile` ; le fragment `Caddyfile.d/qrprotec.caddyfile` est importé par le Caddyfile de Fedora, non modifié. |
| `qrprotec-kiosk.service` | Cage sur `tty1` (à la place de getty), utilisateur `qrprotec-kiosk`, session PAM, `Restart=always`. Lance `qrprotec-kiosk-session`. |
| `qrprotec-alerts.timer` | `check_alerts` chaque jour à 7 h 45, et le lundi `purge_history` (rétention de `QRPROTEC_HISTORY_YEARS` ans, 5 par défaut, puis VACUUM). |
| `qrprotec-backup.timer` | Sauvegarde (quotidienne par défaut, réglable). |
| `80-qrprotec.preset` | Active les unités à l'installation. |
| `logind-qrprotec.conf`, `sleep-qrprotec.conf` | Pas de mise en veille de la borne. |
| `qrprotec.sysusers`, `qrprotec.tmpfiles` | Utilisateurs système et dossiers. `qrprotec-kiosk` est dans `dialout` (Niimbot, port série), `lp` (imprimante TSPL / ZPL en USB), `video`, `render`, `audio` ; pas dans `input` : Cage reçoit clavier, souris et douchette par sa session logind (tty1), le `%post` l'en retire sur une borne déjà installée. |

### Réseau

Les trois API n'écoutent que sur `127.0.0.1`. Caddy expose 80 (redirection) et 443 : les requêtes
qui portent l'en-tête `X-QRProtec-Key` vont à l'API distante (8002), toutes les autres à l'API
publique (8000). L'API locale (8001) n'est jamais exposée. `qrprotec.conf` est lu par le back **et** par
Caddy : le nom d'hôte (`QRPROTEC_DOMAIN`) n'est défini qu'à un endroit et donne le site Caddy,
`ALLOWED_HOSTS` et la base des QR codes (`qrprotec-manage` en déduit les variables Django). SELinux :
le `%post` classe les ports 8000 et 8002 (ou ceux de `qrprotec.conf`) en `http_port_t` (`semanage port`) pour
que Caddy joigne le back, sans ouvrir toutes les connexions sortantes de Caddy. Après un changement de port :
`sudo semanage port -a -t http_port_t -p tcp PORT` (ou `-m`). Une borne installée avant ce changement garde
l'ancien réglage `httpd_can_network_connect` ; à couper à la main : `sudo setsebool -P httpd_can_network_connect 0`.

En-têtes : le back envoie `Strict-Transport-Security` (un an, sur les requêtes HTTPS seulement) et une
`Content-Security-Policy` sur les pages du front web (scripts, styles, images et WebSockets du site seulement).
Le site n'est pas indexé par les moteurs de recherche : `/robots.txt` refuse tout (`Disallow: /`), et chaque
réponse porte `X-Robots-Tag: noindex, nofollow` (back et Caddy), doublé d'une balise `<meta name="robots">`.

Mode maintenance (`qrprotec-manage maintenance on|off|status`) : le fragment Caddy teste à chaque requête
(matcher `file`) la présence de `/etc/qrprotec/maintenance/index.html`, copié depuis
`/usr/share/qrprotec/maintenance.html` (page autonome, styles en ligne, script `maintenance.js` servi en
`/qrprotec-maintenance.js`). S'il existe, Caddy répond 503 à tout le site sans joindre le back : JSON `{"error": …, "maintenance": true}` pour `/api/`, `/ws/`, les fronts
distants (`X-QRProtec-Key`) et les requêtes `Accept: application/json`, la page sinon, avec HSTS
(`QRPROTEC_HSTS_SECONDS`), une CSP `default-src 'none'; script-src 'self'; connect-src 'self'; style-src 'unsafe-inline'`, `Cache-Control: no-store`
et `Retry-After`. `/qrprotec-ca.crt` (matcher de chemin, trié avant) reste servi. Contournement (`on --code`) :
la page porte un sel aléatoire, le script pose le cookie `qrprotec_maintenance` = SHA-256(`sel:code`), et le
matcher `not file` laisse passer au back si `/etc/qrprotec/maintenance/bypass/<cookie>.ok` existe (chemin
assaini par Caddy, dossier 0711 non listable, code jamais stocké en clair). Postes en réseau (`on --front NOM`, `--all-fronts`) : tant que
`/etc/qrprotec/maintenance/fronts` existe (un nom de clé par ligne, `*` pour toutes), Caddy laisse passer les
requêtes à en-tête `X-QRProtec-Key` vers l'API distante, et le back (`middleware.maintenance_refusal`, aussi
sur le WebSocket `/ws/scanner/front`) répond 503 `{"error", "maintenance": true}` aux clés valides absentes de
la liste ; dossier lu via `QRPROTEC_MAINTENANCE_DIR`. `on` recharge Caddy pour
couper les WebSockets ouverts ; `off` n'a rien à recharger. Le dossier est un sous-dossier de `/etc/qrprotec` :
`qrprotec-backup` ne copie que les fichiers du premier niveau, une restauration ne remet donc jamais le site en
maintenance. Le front web traite 502, 503 et 504 comme une absence de réseau (vérifs gardées en file d'attente).

### Kiosk

`qrprotec-kiosk-session` : copie les modèles fournis dans `~qrprotec-kiosk/templates` **sans écraser**
ceux modifiés sur la borne, programme l'extinction de l'écran (swayidle + wlopm, ou wlr-randr, plus rétroéclairage via
logind et DDC/CI via ddcutil),
vérifie la présence des bibliothèques Wayland chargées dynamiquement par GLFW (cause classique
d'écran noir), puis lance le front. `Ctrl+Alt+F2` donne une console de maintenance (`cage -s`).

## Scripts d'administration

| Script | Rôle |
|---|---|
| `qrprotec-manage` | `manage.py` avec l'environnement de la borne, exécuté sous l'utilisateur `qrprotec`. Sauf `maintenance on\|off\|status`, traité par le script lui-même (root, sans Django) : page de maintenance servie par Caddy. |
| `qrprotec-setup` | Assistant idempotent : nom d'hôte, TLS, clavier, écran, sauvegardes, pare-feu, premier admin, restauration. Interactif ou par options (`--help`). |
| `qrprotec-backup` | Sauvegarde (instantané SQLite cohérent via l'API de sauvegarde de SQLite, sans arrêter le back, intégrité vérifiée) + `/etc/qrprotec` + réglages et modèles du kiosk + `MANIFEST`, en `tar.xz`, rotation, copie sur une autre machine par rsync sur SSH (`QRPROTEC_BACKUP_SYNC_TARGET`, `--sync`) ; un échec lance `qrprotec-manage server_problem` (notification « Problème de serveur » des admins), ou `POST /api/server-problem/` sur un kiosk distant. `--list`, `--apply-schedule`, restauration via `qrprotec-setup --restore`. |
| `bootstrap.sh` (racine) | Installation en une commande : ajoute le dépôt, `dnf install`, propose l'assistant. |

## Construction

```sh
make rpm                      # conteneur Fedora 43 (podman ou docker) -> dist/*.rpm
make rpm FEDORA_VERSION=42
make rpm-local                # sur une machine Fedora
make rpm RPMBUILD_ARGS="--without kiosk"
make lint                     # shellcheck + rpmlint
```

`%check` lance `ctest` et les tests Django : un test rouge empêche le paquet d'exister.

## Flatpak du front

`packaging/flatpak/io.github.etyloppihacilem.QRProtec.yml` : le front de bureau seul (équivalent de
`qrprotec-front`, **sans kiosk**) pour les autres distributions. Runtime `org.freedesktop.Platform`
25.08 de Flathub ; GLFW (X11), `libxdo` (SDK Inateck) et `aplay` (bips) sont construits dans le
manifeste car absents du runtime. Les fichiers sont installés comme dans le RPM, sous `/app`, pour
reprendre tels quels le lanceur `files/qrprotec-front` et `files/qrprotec.desktop`.

Permissions (`finish-args`) et pourquoi :

| Permission | Pour |
|---|---|
| `--socket=x11`, `--share=ipc` | Fenêtre en X11 (XWayland) : décorations du bureau (pas de libdecor dans le runtime) et `libxdo` du SDK Inateck |
| `--device=all` | OpenGL (`/dev/dri`) et imprimante (Niimbot `/dev/ttyACM0`, TSPL / ZPL `/dev/usb/lp0`) : Flatpak n'a pas de permission propre aux ports série |
| `--share=network` | API du back |
| `--socket=pulseaudio` | Bips (`aplay` passe par l'ALSA du runtime) |
| `--system-talk-name=org.bluez`, `--allow=bluetooth` | Douchette Inateck en Bluetooth |
| `--filesystem=xdg-config/qrprotec:create` | `~/.config/qrprotec` (le front y écrit d'après `$HOME`), partagé avec le RPM |
| `--filesystem=xdg-pictures:ro` | Images ajoutées aux étiquettes |

Les modèles d'étiquettes vont dans `~/.var/app/<id>/data/qrprotec/templates` (`XDG_DATA_HOME` du
bac à sable). `ctest` tourne pendant la construction. x86_64 seulement (SDK Inateck).

```sh
make flatpak      # flatpak-builder -> dist/QRProtec-front.flatpak
flatpak install --user dist/QRProtec-front.flatpak
```

`.github/workflows/flatpak.yml` construit le `.flatpak` (image `flatpak-github-actions` de Flathub).
Sur un tag `vX.Y.Z`, le job `publish` (`packaging/publish-flatpak.sh`) ajoute la version au **dépôt
Flatpak** statique `flatpak/` de la branche `gh-pages` (à côté du dépôt dnf, même clé GPG, 3 derniers
commits gardés, deltas statiques), écrit `qrprotec.flatpakrepo` et `qrprotec.flatpakref`, puis joint à la
release un `.flatpak` **lié à ce dépôt** (`--repo-url`, clé GPG, runtime depuis Flathub). Un poste
installé depuis la release ou le `.flatpakref` reçoit donc les versions suivantes par `flatpak update`,
et sans rien faire par Logiciels (GNOME) ou Discover (KDE), qui mettent les Flatpak à jour d'office.
Les publications dnf et Flatpak d'un même tag passent l'une après l'autre (`concurrency: gh-pages`).

## Application macOS et mise à jour automatique

`.github/workflows/macos.yml` construit `QRProtec.app` (runner Apple Silicon, signature ad hoc) dans
`QRProtec-macos-arm64.dmg`, joint aux releases. Sur un tag, l'application est construite avec
`-DQRPROTEC_VERSION=X.Y.Z` et se met à jour toute seule (`app/src/app/updater.cpp`) :

1. Une minute après le démarrage puis toutes les 6 h, `GET api.github.com/repos/<dépôt>/releases/latest`
   (curl du système, magasin de certificats de macOS). Les brouillons et pré-releases sont ignorés.
2. Version plus récente : le `.dmg` est téléchargé dans `~/Library/Caches/QRProtec/updates` et son
   SHA-256 comparé à l'empreinte que GitHub publie pour chaque fichier de release (champ `digest`).
3. Prêt : **↑ Mise à jour X.Y.Z** dans la barre de menu (pour tous). Installé à la fermeture de
   QRProtec, ou tout de suite par **Installer et redémarrer maintenant**. `Contents/Resources/qrprotec-update.sh`
   (`packaging/macos`) attend la fin de l'application, monte le `.dmg`, vérifie le nouveau `.app`
   (identifiant `fr.qrprotec.front`, `codesign --verify`), le met à la place de l'ancien (remis en place
   en cas d'échec) et relance l'application. Journal : `~/Library/Caches/QRProtec/updates/update.log`.
4. Application dans un dossier non modifiable par l'utilisateur : le mot de passe d'un administrateur du
   Mac est demandé (`osascript ... with administrator privileges`), seulement après un clic sur
   « Installer », jamais à la fermeture.

Réglages > **Mises à jour** (admins) : version installée, case « Mises à jour automatiques » (cochée par
défaut, `auto_update` dans `app.conf`), « Rechercher maintenant ». `QRPROTEC_NO_AUTO_UPDATE=1` désactive.
Construction hors tag (version `0.0.0`), RPM et Flatpak : pas de mise à jour par l'application.

Sans compte développeur Apple, la signature est ad hoc : macOS peut redemander l'autorisation Bluetooth
(douchette) après une mise à jour. Le CI teste le script de remplacement sur le `.dmg` construit.

## Publication (CI)

`.github/workflows/rpm.yml` : à chaque push sur `main` touchant le code ou le packaging, construit les
RPM pour la dernière Fedora stable et les deux précédentes (liste lue dans Bodhi à chaque lancement,
une nouvelle Fedora est prise dès sa sortie) ; sur un tag `vX.Y.Z`, crée la release GitHub et met à jour le dépôt dnf
statique sur la branche `gh-pages` (`fedora/<version>/<arch>/`, 5 dernières versions), servi par
GitHub Pages. Variables optionnelles : `RPM_PAGES_REPO` / `RPM_PAGES_URL` / `RPM_PAGES_TOKEN` pour
publier dans un dépôt public dédié si celui-ci reste privé. **Obligatoires pour publier** :
`RPM_GPG_PRIVATE_KEY` / `RPM_GPG_PASSPHRASE`. Sans clé, le job de publication échoue au lieu de publier un dépôt
non signé : les RPM sont signés, les métadonnées aussi (`repomd.xml.asc`), et `qrprotec.repo` active `gpgcheck`
et `repo_gpgcheck`. Une borne installée avec l'ancien `qrprotec.repo` (`gpgcheck=0`) le garde : relancer
`bootstrap.sh` ou retélécharger `qrprotec.repo` dans `/etc/yum.repos.d/`.

