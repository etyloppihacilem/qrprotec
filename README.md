# QRProtec

Inventaire par QR code du matériel de secours (consommable ou non) : chaque item porte une étiquette
avec un identifiant unique, les lots (sacs, malles...) sont vérifiés en scannant leur contenu.

- `database/` : back Django (base de données + API).
- `app/` : front ImGui (poste local : douchette Inateck, imprimante Niimbot B1).
- `packaging/` : paquets RPM pour Fedora (voir [Borne Fedora](#borne-fedora-paquet-rpm)).

Documentation complète dans [`docs/`](docs/README.md) : [guide de l'utilisateur](docs/utilisateur/README.md),
[guide d'installation](docs/installation/README.md) et [documentation technique](docs/technique/README.md).

## Borne Fedora (paquet RPM)

Le paquet `qrprotec` transforme un Fedora Server minimal en borne dédiée : back Django en service
systemd, Caddy en reverse proxy HTTPS, front ImGui en plein écran dans [Cage](https://github.com/cage-kiosk/cage)
sur `tty1`, sans bureau ni écran de connexion. Le back et le front peuvent aussi être installés sur
des machines différentes (voir [Paquets et machines séparées](#paquets-et-machines-séparées)). Tout
le packaging est dans `packaging/`.

| Élément | Emplacement |
|---|---|
| Back (Django + dépendances figées par `poetry.lock`) | `/usr/share/qrprotec/backend`, `/usr/share/qrprotec/vendor` |
| Front | `/usr/bin/qrprotec-front` (lanceur), `/usr/libexec/qrprotec/qrprotec-front` (+ SDK Inateck dans `/usr/lib64/qrprotec/`) |
| Modèles d'étiquettes fournis | `/usr/share/qrprotec/templates` (copiés dans `/var/lib/qrprotec-kiosk/templates`) |
| Configuration | `/etc/qrprotec/qrprotec.conf`, `/etc/qrprotec/kiosk.conf`, `/etc/qrprotec/front-api.conf` (back distant du kiosk, `0600`), `/etc/caddy/Caddyfile.d/qrprotec.caddyfile` |
| Clé secrète Django | `/etc/qrprotec/secret_key` (générée à l'installation, `0640 root:qrprotec`) |
| Données (base SQLite) | `/var/lib/qrprotec/db.sqlite3` |
| Services | `qrprotec.service` (back), `qrprotec-kiosk.service` (Cage), `qrprotec-alerts.timer` (SMS, 7 h 45), `qrprotec-backup.timer` (sauvegardes), `caddy.service` |
| Sauvegardes | `/var/backups/qrprotec/qrprotec-backup_AAAA-MM-JJ_HHMMSS.tar.xz` (réglages : `/etc/qrprotec/backup.conf`) |
| Utilisateurs (`sysusers.d`) | `qrprotec` (back, sans shell), `qrprotec-kiosk` (session Cage ; groupes `dialout`, `video`, `render`, `input`, `audio`) |
| Commandes | `qrprotec-setup` (assistant), `qrprotec-manage` (`manage.py` avec la configuration de la borne), `qrprotec-backup` (sauvegarde / restauration) |

Réseau : les trois API du back n'écoutent que sur `127.0.0.1` (8000 publique, 8001 locale, 8002
distante). Caddy expose 80 (redirection) et 443 : il proxifie l'API publique et les pages web
(WebSockets du téléphone-douchette compris) et, pour les requêtes qui portent une clé de front
(`X-QRProtec-Key`), l'API distante. L'API locale n'est jamais exposée.

### Paquets et machines séparées

| Paquet | Contenu | Pour |
|---|---|---|
| `qrprotec` | `qrprotec-server` + `qrprotec-kiosk` (paquet meta) | la borne complète, comme avant |
| `qrprotec-server` | back Django, Caddy, alertes SMS, `qrprotec-manage` | le back seul, serveur sans écran |
| `qrprotec-kiosk` | front en plein écran dans Cage sur `tty1` (dépend de `qrprotec-front`) | un poste dédié, avec back local ou distant |
| `qrprotec-front` | front en application de bureau (menu, icône), sans kiosk | un ordinateur qui ne sert pas qu'à ça |
| `qrprotec-common` | `qrprotec-setup`, `qrprotec-backup` et son timer | installé avec `-server` ou `-kiosk` |

Par défaut rien ne change : le front utilise l'**API locale** de sa machine (`http://127.0.0.1:8001`,
port non exposé, sans clé). Un front sur une autre machine se connecte au back en **HTTPS via Caddy,
avec sa propre clé API** ; un front local et des fronts distants fonctionnent en même temps.

1. Sur le serveur (`qrprotec` ou `qrprotec-server`), créer une clé par front distant :

   ```sh
   sudo qrprotec-setup --front-key accueil     # ou : sudo qrprotec-manage frontkey add accueil
   sudo qrprotec-manage frontkey list          # fronts, dernière utilisation et adresse
   sudo qrprotec-manage frontkey revoke accueil
   ```

   La clé (`qrpf_...`) n'est affichée qu'une fois (seule son empreinte SHA-256 est en base). Avec
   `QRPROTEC_TLS=internal`, le front doit connaître l'autorité de Caddy : copier
   `/var/lib/caddy/.local/share/caddy/pki/authorities/local/root.crt` du serveur sur le poste.

2. Sur un poste kiosk (`dnf install qrprotec-kiosk`) :

   ```sh
   sudo qrprotec-setup --api-url https://inventaire.example.org --api-key qrpf_... \
       --api-ca-file /chemin/root.crt      # seulement pour l'autorité interne de Caddy
   sudo qrprotec-setup --local-api         # revenir au back de la machine
   ```

   C'est écrit dans `/etc/qrprotec/front-api.conf` (`QRPROTEC_API_URL`, `QRPROTEC_API_KEY`,
   `QRPROTEC_API_CA_FILE`, lisible par root seulement), qui prime sur les réglages du front.

3. Sur un ordinateur de bureau (`dnf install qrprotec-front`, menu **QRProtec** ou commande
   `qrprotec-front`) : **Réglages > Serveur** (URL, clé, certificat de l'autorité). Tant que le
   serveur est injoignable, un clic sur « API hors ligne » dans la barre de menu ouvre ces champs
   sans badge (jamais sur une borne `qrprotec-kiosk`, réglée par `qrprotec-setup`). Les variables `QRPROTEC_API_URL`, `QRPROTEC_API_KEY`, `QRPROTEC_API_CA_FILE` marchent
   aussi. Pour imprimer, l'utilisateur doit être dans le groupe `dialout`.

Changer de paquet : `sudo dnf install qrprotec-server && sudo dnf mark user qrprotec-server`, puis
`sudo dnf remove qrprotec qrprotec-kiosk` (sinon `dnf remove qrprotec` retire aussi les paquets
installés comme dépendances). Une borne installée avec l'ancien paquet unique passe automatiquement
à `qrprotec` + `qrprotec-server` + `qrprotec-kiosk` au `dnf upgrade`, en gardant sa configuration
et ses services.

### Prérequis

- **Fedora Server 42 ou plus récent**, installation minimale (« Minimal Install », sans bureau),
  x86_64 (aarch64 possible en construisant le RPM soi-même, sans le SDK Inateck).
- Un écran et un clavier (ou douchette) branchés ; accès réseau pour `dnf`.
- Pour un certificat Let's Encrypt : un nom de domaine public pointant vers la borne et les ports
  80/443 joignables. Sinon, Caddy utilise sa propre autorité locale (`internal`).

### Installation (une commande)

```sh
curl -fsSL https://etyloppihacilem.github.io/qrprotec/bootstrap.sh | sudo bash
```

Le script ajoute le dépôt (`/etc/yum.repos.d/qrprotec.repo`), lance `dnf install qrprotec`, puis
propose l'assistant. Pour un autre paquet : `curl -fsSL .../bootstrap.sh | sudo QRPROTEC_PACKAGE=qrprotec-server bash`
(ou `qrprotec-kiosk`, `qrprotec-front`). Équivalent manuel :

```sh
sudo curl -fsSL -o /etc/yum.repos.d/qrprotec.repo https://etyloppihacilem.github.io/qrprotec/qrprotec.repo
sudo dnf install qrprotec
sudo qrprotec-setup
sudo systemctl reboot
```

L'installation crée les utilisateurs, génère la clé secrète, active et démarre `qrprotec`,
`caddy`, `qrprotec-alerts.timer` et `qrprotec-kiosk`, passe la cible par défaut à
`graphical.target` et autorise Caddy à joindre le back sous SELinux
(`setsebool -P httpd_can_network_connect 1`). Le redémarrage n'est nécessaire que pour appliquer
les réglages de veille (logind) et vérifier le démarrage direct sur le kiosk.

### Configuration

Lancer **`sudo qrprotec-setup`** (relançable à volonté) : nom d'hôte, certificat HTTPS, disposition
du clavier, extinction de l'écran, back distant du kiosk, sauvegardes (dossier, nombre, fréquence),
ouverture du pare-feu et premier administrateur. Seules les parties installées sont demandées. `sudo qrprotec-setup --help` liste toutes les options. Version non
interactive :

```sh
sudo qrprotec-setup --domain inventaire.example.org --tls admin@example.org \
    --keyboard fr --open-firewall --no-admin --yes
```

**Back et Caddy** : `/etc/qrprotec/qrprotec.conf` (format `CLÉ=valeur`) est lu par le back *et*
par Caddy (drop-in `caddy.service.d/qrprotec.conf`). Le nom d'hôte n'est donc défini qu'à un seul
endroit, `QRPROTEC_DOMAIN`, qui donne le site Caddy, `ALLOWED_HOSTS` et la base des URLs des QR
codes (`QRPROTEC_PUBLIC_BASE_URL`, surchargeable). `QRPROTEC_TLS` vaut `internal` (réseau local,
adresse IP ; les téléphones doivent faire confiance à l'autorité de Caddy, dont le certificat se
trouve avec `sudo find /var/lib/caddy -name root.crt`)
ou une adresse e-mail (Let's Encrypt). Les autres variables du tableau
[Variables d'environnement](#variables-denvironnement) s'y ajoutent. Après modification à la main :

```sh
sudo systemctl restart qrprotec.service && sudo systemctl reload caddy.service
```

Le fragment Caddy `/etc/caddy/Caddyfile.d/qrprotec.caddyfile` est importé par le `Caddyfile` du
paquet Fedora (`import Caddyfile.d/*.caddyfile`), qui n'est pas modifié. On peut l'éditer (en-têtes,
logs...) : il est préservé par les mises à jour.

**Kiosk** : `/etc/qrprotec/kiosk.conf` (`XKB_DEFAULT_LAYOUT`, `QRPROTEC_SCREEN_BLANK_SECONDS`,
1800 s par défaut) puis `sudo systemctl restart qrprotec-kiosk`. La mise en veille est désactivée
(`logind.conf.d`, `sleep.conf.d`), l'écran s'éteint après 30 min sans activité (swayidle + wlopm, ou
wlr-randr) et se rallume à la première touche ; curseur et clavier restent normaux. Les réglages du
front (`api_url`, imprimante...) sont dans `/var/lib/qrprotec-kiosk/.config/qrprotec/app.conf` (le
back distant, s'il y en a un, dans `/etc/qrprotec/front-api.conf`) et
les modèles d'étiquettes dans `/var/lib/qrprotec-kiosk/templates/` (images dans son sous-dossier `images/`).
`Ctrl+Alt+F2` ouvre une console de maintenance.

**Administrateur** : `sudo qrprotec-manage createadmin M001 Nom Prénom --pin 4821` (toute commande
`manage.py` passe par `qrprotec-manage`, exécutée sous l'utilisateur `qrprotec`).

**Pare-feu** : rien n'est ouvert automatiquement. `qrprotec-setup --open-firewall` ouvre http/https
dans firewalld s'il est actif, sinon dans UFW (`ufw allow 80/tcp`, `ufw allow 443/tcp`).

### Sauvegardes

`qrprotec-backup.timer` crée chaque jour (par défaut) une archive
`qrprotec-backup_AAAA-MM-JJ_HHMMSS.tar.xz` (0600, root) avec ce qui est installé sur la machine :

- back (`qrprotec-server`) : un instantané cohérent de la base SQLite (API de sauvegarde de SQLite,
  sans arrêter le back, intégrité vérifiée), `/etc/qrprotec` (configuration, **clé secrète** comprise)
  et le fragment Caddy ;
- kiosk (`qrprotec-kiosk`) : `/etc/qrprotec` (connexion au back distant comprise), les réglages du
  front (`app.conf`, douchette) et les modèles d'étiquettes (logo compris) ;
- un fichier `MANIFEST` (date, machine, parties `server`/`kiosk`, paquets, nom d'hôte, back distant).

Sur un kiosk relié à un back distant, l'archive ne contient donc pas de base : celle-ci est
sauvegardée sur le serveur. Le front de bureau (`qrprotec-front`) garde ses réglages dans le dossier
de l'utilisateur (`~/.config/qrprotec`, `~/.local/share/qrprotec/templates`).

Réglages (`sudo qrprotec-setup`, ou `/etc/qrprotec/backup.conf` puis
`sudo qrprotec-backup --apply-schedule`) :

| Option de `qrprotec-setup` | Variable | Défaut |
|---|---|---|
| `--backup-dir DOSSIER` | `QRPROTEC_BACKUP_DIR` | `/var/backups/qrprotec` (de préférence un autre disque : clé USB, NAS monté) |
| `--backup-keep N` | `QRPROTEC_BACKUP_KEEP` | `14` archives (les plus anciennes sont supprimées ; `0` = toutes) |
| `--backup-schedule F` | `QRPROTEC_BACKUP_SCHEDULE` | `daily` ; aussi `hourly`, `weekly`, `monthly`, une expression `OnCalendar` (`"*-*-* 03:30:00"`, voir `man systemd.time`) ou `off` |

```sh
sudo qrprotec-setup --backup-dir /mnt/usb/qrprotec --backup-keep 30 --backup-schedule "*-*-* 03:30:00" --yes
sudo qrprotec-backup                 # sauvegarde immédiate (ou qrprotec-setup --backup-now)
sudo qrprotec-backup --list          # archives présentes
systemctl list-timers qrprotec-backup.timer
journalctl -u qrprotec-backup
```

**Restauration** (même machine ou nouvelle installation des mêmes paquets) :

```sh
sudo qrprotec-setup --restore /var/backups/qrprotec/qrprotec-backup_2026-10-01_031204.tar.xz
sudo qrprotec-setup --restore ARCHIVE --restore-db-only   # la base seulement
```

La restauration vérifie l'archive, demande confirmation (`--yes` pour s'en passer), **sauvegarde
d'abord l'état actuel**, arrête les services, remet la base et (sauf `--restore-db-only`) les options
de `/etc/qrprotec`, le fragment Caddy, les réglages et modèles du kiosk et la fréquence des
sauvegardes, puis relance les services (le back applique les migrations si l'archive vient d'une
version plus ancienne). Seules les parties installées sont restaurées : la base d'une archive de
borne complète est ignorée sur un kiosk seul, et la configuration du back n'y est pas copiée. Les
archives des versions précédentes (paquet unique) se restaurent aussi. Sur une nouvelle machine dont
l'adresse a changé, terminer par
`sudo qrprotec-setup --domain NOUVEAU_NOM`. Les archives ne sont jamais supprimées par
`dnf remove`.

### Mise à jour

```sh
sudo dnf upgrade
```

Les services modifiés sont redémarrés à la fin de la transaction, les migrations de la base sont
appliquées au démarrage du back, et Caddy est rechargé. Les fichiers de configuration modifiés
(`%config(noreplace)`) sont conservés ; si le paquet en apporte une nouvelle version, elle est
déposée à côté en `.rpmnew`.

### Désinstallation

```sh
sudo dnf remove qrprotec qrprotec-server qrprotec-kiosk qrprotec-front qrprotec-common
```

Les services sont arrêtés et désactivés, Caddy est rechargé sans le site QRProtec, la cible par
défaut repasse en `multi-user.target` et la console `tty1` revient. Les données (`/var/lib/qrprotec`,
`/var/lib/qrprotec-kiosk`), les sauvegardes, la clé secrète et les utilisateurs sont conservés ; une configuration
modifiée est sauvegardée en `.rpmsave`. `dnf` retire aussi `caddy` et `cage` s'ils n'avaient été
installés que pour QRProtec (`sudo dnf mark user caddy` avant pour garder Caddy). Le dépôt se
retire avec `sudo rm /etc/yum.repos.d/qrprotec.repo`.

### Journaux

```sh
journalctl -u qrprotec -f            # back (requêtes, migrations)
journalctl -u qrprotec-kiosk -b      # Cage et front
journalctl -u caddy -f               # reverse proxy, certificats
journalctl -u qrprotec-alerts        # notifications SMS quotidiennes
journalctl -u qrprotec-backup        # sauvegardes
systemctl status qrprotec qrprotec-kiosk caddy
```

### Construire le RPM localement

```sh
git submodule update --init app/imgui app/implot app/scanner_lib   # fait aussi automatiquement par le build
make rpm                      # dans un conteneur Fedora 43 (podman ou docker) : dist/*.rpm
make rpm FEDORA_VERSION=42    # pour Fedora 42
make rpm-local                # directement sur une machine Fedora (dnf builddep si root)
make rpm RPMBUILD_ARGS="--without kiosk"   # qrprotec-server et qrprotec-common seulement (pas de front)
make lint                     # shellcheck + rpmlint
```

`make rpm` construit l'image `packaging/Containerfile` (outils et dépendances de build en cache),
puis `packaging/build-rpm.sh` : `packaging/make-sources.sh` prépare l'archive du code (avec les
sous-modules) et celle des dépendances Python figées par `poetry.lock` (avec empreintes), puis
`rpmbuild` compile le front, lance les tests C++ (`ctest`) et Django, et produit les RPM
(`qrprotec`, `-server`, `-kiosk`, `-front`, `-common`) et le SRPM. La
version vient du tag git (`packaging/version.sh`) : `v1.2.0` → `1.2.0`, et entre deux tags
`1.2.0^3.gabc1234`. Le RPM est spécifique à une version de Fedora (Python embarqué pour sa version
de Python).

### Publier une nouvelle version

```sh
git tag v1.2.0 && git push origin v1.2.0
```

Le workflow `.github/workflows/rpm.yml` construit les RPM pour Fedora 42, 43 et 44, crée la release
GitHub (RPM en pièces jointes) et met à jour le dépôt dnf statique sur la branche `gh-pages`
(`fedora/<version>/<arch>/`, 5 dernières versions gardées), servi par GitHub Pages. Les bornes le
reçoivent au prochain `sudo dnf upgrade`.

Mise en place, une fois : **Settings > Pages** : déployer depuis la branche `gh-pages`. GitHub Pages
n'est disponible pour un dépôt **privé** qu'avec un compte payant (et le site reste public). Sinon,
publier dans un dépôt public dédié, par exemple `etyloppihacilem/qrprotec-rpm` : variables
`RPM_PAGES_REPO` (`etyloppihacilem/qrprotec-rpm`) et éventuellement `RPM_PAGES_URL`, secret
`RPM_PAGES_TOKEN` (jeton avec droit d'écriture sur ce dépôt), et adapter l'URL de `bootstrap.sh`
(ou `QRPROTEC_REPO_URL=...`). Pour signer les paquets (`gpgcheck=1`), ajouter les secrets
`RPM_GPG_PRIVATE_KEY` et `RPM_GPG_PASSPHRASE`.

## Identifiants et QR codes

| Objet | Contenu du QR code | Exemple |
|---|---|---|
| Item | `<base>/item?id=IID`, l'iid étant type (6) + péremption `AAAAMMJJ` (8) + compteur base 62 (8) | `https://example.com/item?id=serphy20271231000000A1` |
| Item, ancien format (toujours lu) | l'iid seul | `serphy20271231000000A1` |
| Item non périssable | date `00000000` dans l'iid | `garrot00000000000000A1` |
| Lot, étiquette publique | `<base>/verif?lot=ID` | `https://example.com/verif?lot=sacpse00000001` |
| Lot, étiquette privée | `<base>/verif?lot=ID&key=CLE` | `...verif?lot=sacpse00000001&key=a1B2...` |
| Badge utilisateur | `<base>/badge?m=MATRICULE&key=CLE` (clé valable 1 an) | `...badge?m=M0042&key=...` |
| Paquet fermé | `<base>/pack?id=ID` | `...pack?id=0000002B` |
| Scellé d'un lot | `<base>/seal?lot=ID&s=CODE` (code changé à chaque scellage) | `...seal?lot=sacpse00000001&s=...` |

`<base>` vaut `https://example.com/` par défaut et se change avec la variable d'environnement
`QRPROTEC_PUBLIC_BASE_URL` du back. Le front reconnaît les QR codes quel que soit le domaine : changer
la base n'invalide pas les étiquettes déjà imprimées.

## Back (Django)

```sh
cd database
poetry install --no-root          # ou : pip install django djangorestframework
python manage.py migrate
python manage.py serve            # API publique 0.0.0.0:8000 + API locale 127.0.0.1:8001 + API distante 127.0.0.1:8002
python manage.py frontkey add accueil   # clé API d'un front d'une autre machine (affichée une fois)
python manage.py createadmin M001 Nom Prenom --pin 4821   # premier administrateur (ou badge admin perdu)
python manage.py serve --https    # API publique en HTTPS (certificat de développement, tests sur téléphone)
python manage.py test inventory
```

Trois API sur trois ports, sélectionnées par le port qui reçoit la requête
(`inventory/middleware.py`) :

- **API publique** (`qrprotecDB/urls.py`) : lecture d'un item, d'un lot, d'un paquet, confirmation
  d'un badge. Toute écriture exige la clé de l'objet modifié (ex : clé du lot pour une vérif).
  Pour l'HTTPS, placer un reverse proxy (nginx, caddy) devant le port public.
- **API locale** (`qrprotecDB/urls_local.py`) : gestion (types, réception, lots, utilisateurs, stocks)
  et admin Django, selon le rôle de l'utilisateur connecté sur le poste (jeton de session envoyé dans
  `X-QRProtec-Session` après le badge et le PIN). N'accepte que les adresses de
  `QRPROTEC_LOCAL_API_ALLOWED_ADDRESSES` (localhost par défaut) et, si défini, le jeton
  `QRPROTEC_LOCAL_API_TOKEN` dans l'en-tête `X-QRProtec-Token`.
- **API distante** (mêmes routes que l'API locale) : pour les fronts d'autres machines, derrière le
  reverse proxy HTTPS. Chaque requête doit porter une clé de front valide (modèle `FrontKey`,
  commande `manage.py frontkey add|list|revoke|delete`) dans l'en-tête `X-QRProtec-Key`, sinon 401.
  `health/` renvoie alors le nom du front dans `front`. Caddy y envoie les requêtes qui portent cet
  en-tête (`packaging/files/qrprotec.caddyfile`).

En production : `gunicorn qrprotecDB.wsgi:public_application`,
`gunicorn qrprotecDB.wsgi:local_application` et `gunicorn qrprotecDB.wsgi:remote_application` sur
trois ports. Le **téléphone-douchette** utilise des
WebSockets gérés par `manage.py serve` uniquement (sessions en mémoire partagées par les deux API) :
pour s'en servir, lancer le back avec `serve` derrière le reverse proxy HTTPS, en transmettant
l'upgrade WebSocket (nginx : `proxy_http_version 1.1; proxy_set_header Upgrade $http_upgrade;
proxy_set_header Connection "upgrade";`).

### Téléphone-douchette (WebSocket)

Un téléphone peut servir de douchette : ses scans arrivent dans la pile du front comme ceux de la
douchette (`ScanSource::Phone`).

1. Le front crée une session (`POST /api/remote-scanner/`, délai de déconnexion en minutes) et affiche
   un QR code vers `<base>/scanner?s=SESSION&k=CLE`, puis se connecte à `/ws/scanner/front?s=SESSION`
   (API locale, même contrôle d'adresse et de jeton, ou API distante avec la clé du front).
2. Le téléphone scanne ce QR code avec son appareil photo : la page de scan s'ouvre et se connecte à
   `/ws/scanner/phone?s=SESSION&k=CLE` (API publique, HTTPS obligatoire pour la caméra).
3. Chaque code lu est relayé au front ; le front renvoie les erreurs (produit périmé, code inconnu)
   avec leur message, et le téléphone clignote en rouge, bipe et vibre. Le téléphone n'affiche qu'un
   libellé court par code (« Item compre · 31/12/2027 », « Badge M0042 », « Lot … · étiquette privée »),
   jamais les clés ; « Annuler le dernier » retire du poste le dernier scan venu du téléphone.
4. Si le téléphone (ou le poste) reste déconnecté plus longtemps que le délai choisi (5 min par
   défaut, réglable dans la fenêtre du front), la session est fermée : la clé du QR code ne marche
   plus et il faut en générer un nouveau. Les reconnexions courtes (écran éteint, réseau) sont
   automatiques. `GET /api/remote-scanner/check/?s=..&k=..` (public) indique si la session existe.

Le relais est dans `inventory/remote_scanner.py` (protocole détaillé en tête de fichier), la page du
téléphone dans `inventory/web/scanner.html` et `scanner.js`.

### Variables d'environnement

| Variable | Défaut |
|---|---|
| `QRPROTEC_PUBLIC_BASE_URL` | `https://example.com/` |
| `QRPROTEC_PUBLIC_API_ADDRESS` / `_PORT` | `0.0.0.0` / `8000` |
| `QRPROTEC_LOCAL_API_ADDRESS` / `_PORT` | `127.0.0.1` / `8001` |
| `QRPROTEC_LOCAL_API_ALLOWED_ADDRESSES` | `127.0.0.1,::1` |
| `QRPROTEC_LOCAL_API_TOKEN` | vide (pas de jeton) |
| `QRPROTEC_REMOTE_API_ADDRESS` / `_PORT` | `127.0.0.1` / `8002` (API distante, `--no-remote` pour ne pas la lancer) |
| `QRPROTEC_ALLOWED_HOSTS` | `localhost,127.0.0.1,[::1]` |
| `QRPROTEC_DEBUG_HOSTS` | `192.168.1.201` (ajoutés à `ALLOWED_HOSTS` en mode DEBUG seulement) |
| `QRPROTEC_MISSING_AFTER_VERIFS` | `3` |
| `QRPROTEC_OUT_DAYS` | `30` (jours avant qu'un item sorti sans lot compte comme utilisé) |
| `QRPROTEC_LOT_KEY_VALIDITY_DAYS` | `365` |
| `QRPROTEC_SMS_SYNC` | `0` (SMS et notifications web envoyés dans un thread ; `1` = dans la requête) |
| `QRPROTEC_WEB_PUSH_SUBJECT` | `QRPROTEC_PUBLIC_BASE_URL` (contact VAPID des notifications web : `mailto:...` ou `https://...`) |
| `QRPROTEC_SECRET_KEY`, `QRPROTEC_DEBUG`, `QRPROTEC_DB_PATH` | réglages Django |

### Routes (`/api/...`)

Publiques et locales : `health/`, `auth/` (POST matricule + key), `items/<iid>/`, `lots/<id>/`,
`lots/<id>/verif/` (POST items, key, partial), `verifs/` (POST `lots: [{id, key}]`, items, partial :
vérif groupée de plusieurs lots d'un même lot global), `lots/<id>/add/` (POST items, key), `items/out/` (POST items :
sortie du stock sans lot), `lots/<id>/unseal/` (POST
key), `packs/<id>/`, `lots/<id>/sheet.pdf` (GET, fiche d'inventaire papier A4 : items attendus par
emplacement, une page par sous-lot). `lots/<id>/?seal=CODE` renvoie `seal_check` : `valid`, `wrong` (ancien scellé) ou
`unsealed`. `lots/<id>/?key=CLE` renvoie `key_check` : `valid`, `expired` ou `wrong` (étiquette privée d'une ancienne
clé), et `key_error` (message à afficher) si elle n'est pas valide. En lecture seule avec un badge (`{"user": {"matricule", "key"}}` en POST) :
`lots/summary/` (lots actifs et leur état, tout badge valide), `stock/summary/` (état des stocks,
rôles gestion et admin) et `stock/forecast/summary/` (prévisions de stock, mêmes rôles, `months`). Notifications web : `push/key/` (GET, clé publique VAPID),
`push/subscription/` (POST badge + `endpoint` pour l'état, ou + `subscription`, `stock_low`,
`stock_empty`, `pin_blocked`, `lot_key_renewed`, `lot_key_expiring`, `badge_renewed`,
`badge_expiring`, `order_due` pour s'abonner), `push/unsubscribe/` (POST endpoint), `push/test/` (POST badge + endpoint), `push/devices/` (POST badge :
appareils abonnés), `push/devices/<id>/` (POST badge + alertes, ou `delete`). Rôles gestion et admin.

### Rôles

| Rôle | Front ordinateur | Téléphone |
|---|---|---|
| `normal` (Secouriste) | vérifs, pile de scans, ajout aux lots | vérifs, liste des lots |
| `gestion` | mode privilégié : stocks, inventaire, paquets, lots ; **pas** les Réglages, les Utilisateurs ni l'éditeur d'étiquettes | + onglet **Stock** (lecture seule) |
| `admin` | tout, dont Réglages (serveur, étiquettes, notifications SMS…), Utilisateurs et éditeur d'étiquettes | + onglet **Stock** |

**Code PIN** (4 à 8 chiffres, haché) : demandé après le badge à chaque connexion, obligatoire pour les
administrateurs, facultatif pour les autres rôles (défini dans **Gestion > Utilisateurs**). Un
administrateur sans PIN (compte existant, ou créé par `createadmin` sans `--pin`) le choisit à sa
connexion suivante. Après 5 PIN faux, le PIN est bloqué 5 minutes. Sur le téléphone, `auth/` renvoie
un jeton de session (12 h) utilisé pour l'état des stocks à la place du PIN.

Le rôle se choisit dans **Gestion > Utilisateurs** (admin). La migration `0003_roles` transforme les
anciens responsables en administrateurs ; `createadmin` crée ou répare un administrateur.

Locales uniquement : `item-types/`, `item-types/<type>/`, `items/` (recherche), `items/batch/`
(réception), `items/to-stock/`, `items/<iid>/delete/`, `items/<iid>/restore/`, `stock/`,
`stock/forecast/` (GET `months`, `lead_days`, `history_months`), `stock/verif/`, `packs/`, `packs/<id>/open/`, `packs/<id>/close/`, `lot-types/`, `lot-types/<type>/`,
`lot-types/<type>/requirements/`, `lots/`, `lots/<id>/update/`, `lots/<id>/rotate-key/`,
`lots/<id>/seal/` (POST seal_number, force), `lots/<id>/verifs/`, `operations/` (GET journal des opérations : kind, by, lot, sub, since, until, q, before, limit, facets), `users/`, `users/<matricule>/`,
`users/<matricule>/renew-key/`, `notifications/` (GET, PATCH enabled/events),
`notifications/recipients/` (POST), `notifications/recipients/<id>/` (PATCH, DELETE),
`notifications/test/` (POST).

Sur l'API publique, l'utilisateur est transmis sous la forme `"user": {"matricule": ..., "key": ...}`
(vérifié) ou `"name": ...` (déclaré). Sur l'API locale, `"user": "MATRICULE"` suffit.

### Règles de gestion

- **Vérif** : les items scannés sont placés dans le lot (ou le stock). Un item périmé scanné alors que
  des items frais du même type viennent d'arriver dans le lot est considéré comme **remplacé** et sort
  du lot. Les items attendus mais non scannés sont signalés ; ils passent **disparus** s'ils sont
  périmés ou après `QRPROTEC_MISSING_AFTER_VERIFS` vérifs manquées. Les exigences du lot ne comptent
  que les items vus à la dernière vérif.
- **Sortie du stock** : des items scannés sans étiquette de lot peuvent être **sortis** du stock (statut
  `out`, « sorti »). Ils redeviennent normaux s'ils sont scannés à une vérif ; sinon `check_alerts` les
  compte comme utilisés après `QRPROTEC_OUT_DAYS` jours (30) ou à leur péremption si elle tombe avant.
- **Suppression** : les items ne sont pas supprimés par les utilisateurs. Un responsable peut
  exceptionnellement marquer un item supprimé (raison obligatoire) et le restaurer.
- **Paquet fermé** : à la réception, les items sont créés et une seule étiquette de paquet est
  imprimée ; les étiquettes individuelles sont imprimées à l'ouverture. En mode privilégié, scanner
  l'étiquette du paquet ouvre sa fiche (contenu, péremption, réception, ouverture) avec le bouton
  « Ouvrir le paquet et imprimer les N étiquettes » (aperçu puis impression). Rouvrir un paquet déjà
  ouvert réimprime ses étiquettes sans changer sa date d'ouverture. Un paquet ouvert par erreur se
  referme depuis sa fiche (« Refermer le paquet »).
- **Lot scellé** : après une vérif complète, un responsable ferme le lot avec un scellé et imprime
  l'étiquette du scellé (QR `seal?lot=..&s=..`). Tant que le scellé est intact, le lot est valide
  sans vérif (vert « Scellé », valable jusqu'à la première péremption de son contenu) : scanner le QR
  du scellé l'affiche. Une vérif, un ajout ou un retrait d'items, ou « Briser le scellé », brise le
  scellé : l'ancienne étiquette devient invalide et le lot doit être vérifié. Un lot scellé qui
  contient des périmés est rouge (à ouvrir).
- **Réassort** : ajouter des items à un lot sans vérif complète (bouton « Ajouter au lot … (réassort) »
  de la pile, ou, pendant une vérif où seuls des items qui ne sont pas dans le lot ont été scannés,
  bouton orange « Ajouter N item(s) au lot – réassort, sans vérif » ; même bouton sur le téléphone).
  Les autres items du lot ne sont pas touchés. Le lot passe **« vérif recommandée »** (orange, avec le
  nombre d'items, la date et l'auteur du réassort) jusqu'à la prochaine vérif, pour que la personne
  suivante vérifie tout le lot. Un lot incomplet reste rouge.
- **Lots et sous-lots** : un lot peut avoir un lot parent (ex : B+ = sac de soin + sac O2 ; VPS =
  armoires + B+). Chaque sous-lot a ses propres QR (public et privé) et se vérifie seul ; sa fiche
  affiche aussi l'état du lot global. Un lot global est valide si tous ses sous-lots le sont, et sa
  dernière vérif est la plus ancienne de ses sous-lots. Un lot sans contenu attendu ni items qui a des
  sous-lots est un simple regroupement. La clé privée d'un lot couvre ses sous-lots.
- **Vérif groupée** : pendant une vérif, scanner le QR privé d'un autre lot du même lot global
  l'ajoute à la vérif en cours (les items attendus s'additionnent ; chaque item scanné est rangé dans
  le lot où il manque). Les lots doivent partager le même lot global.
- **Vérif partielle** : si la vérif est validée alors que seuls certains lots sont complets, le bouton
  « Vérif partielle » vérifie ces lots seulement ; les autres items scannés sont ajoutés à leur lot
  en réassort.
- **Rangements** : un type de lot marqué « rangement » (tiroir, armoire, étagère…) compte comme du
  stock. On vérifie un tiroir comme un lot, sans vérifier tout le stock ; la vérif du stock ne
  signale manquants que les items sans emplacement.
- **Scellés et sous-lots** : ouvrir un sous-lot brise aussi le scellé de ses parents ; sceller un lot
  global demande que tous ses sous-lots soient complets.
- **Emplacements** : chaque ligne du contenu attendu d'un type de lot peut préciser un emplacement
  (ex : sérum phy dans la pochette bleue du sac de soin), affiché pendant la vérif.

### Notifications SMS (Free Mobile)

Le serveur envoie des SMS par l'API de Free Mobile
(`https://smsapi.free-mobile.fr/sendmsg?user=..&pass=..&msg=..`). Chaque destinataire active
« Notifications par SMS » dans son espace abonné Free, qui donne son identifiant et sa clé
d'identification ; l'API n'envoie qu'au titulaire de la ligne, d'où un couple identifiant / clé par
destinataire. Destinataires et événements se gèrent dans **Gestion > Réglages > Notifications SMS**
du front (activation générale, SMS de test, dernier statut d'envoi) ; la clé n'est jamais renvoyée
par l'API.

Événements : stock d'un type sous son minimum (un SMS par passage sous le seuil, stock hors lots non
périmé), vérif de lot incomplète (manquants, périmés, disparus), scellé brisé, et résumé quotidien des
lots contenant des périmés. Les péremptions faisant baisser le stock sans action, lancer chaque jour :

```sh
python manage.py check_alerts   # ex. crontab : 45 7 * * * cd .../database && python manage.py check_alerts
```

### Notifications web (tous navigateurs)

Les SMS ne fonctionnent qu'avec une ligne Free. Un administrateur connecté sur le téléphone (badge +
PIN) peut aussi activer, dans l'onglet **Stock**, des notifications web sur son appareil : **stock bas**
(un type passe sous son minimum) et/ou **stock vide** (un type arrive à 0). Le navigateur ne demande
l'autorisation qu'au clic sur « Activer les notifications » ; un bouton envoie une notification de test.
Les alertes arrivent même page fermée (une par passage sous le seuil, comme les SMS) et ouvrent
l'onglet Stock.

- Il faut HTTPS avec un certificat reconnu par le téléphone (avec `tls internal`, installer l'autorité
  de Caddy sur le téléphone) et un accès Internet sortant du serveur vers le service de notifications
  du navigateur (Google, Mozilla, Apple).
- Sur iPhone (iOS 16.4+), ajouter d'abord la page à l'écran d'accueil.
- Le serveur utilise `python3-cryptography` (chiffrement RFC 8291, clés VAPID générées au premier
  usage et gardées en base). Un abonnement expiré est supprimé au premier envoi refusé.

## Front web (téléphone)

Page unique servie par le back aux adresses mêmes des QR codes : `/verif?lot=…[&key=…]`,
`/badge?m=…&key=…`, `/pack?id=…` et `/`. Scanner une étiquette avec l'appareil photo du téléphone
ouvre donc directement la bonne vue. Les fichiers sont dans `database/inventory/web/`.

- **Moitié haute** : caméra arrière et viseur. Décodage natif (`BarcodeDetector`) quand le navigateur
  le propose, sinon [jsQR](https://github.com/cozmo/jsQR) (Apache 2.0, fourni dans `web/vendor/`,
  aucun CDN). Lampe si le téléphone le permet, écran maintenu allumé pendant le scan.
- **Moitié basse** : informations du dernier scan (item : type, péremption, emplacement ; lot : état,
  dernière vérif) et les onglets :
  - **Accueil** (affiché quand aucun lot n'est en cours) : utilisateur connecté, **liste des lots**
    avec leur état (après scan du badge) — toucher un lot l'ouvre pour commencer sa vérif —, et accès
    à la **douchette du poste** : scanner le QR code affiché par le poste (Douchette > Téléphone comme
    douchette) ouvre la page de scan qui envoie les codes au poste ;
  - **À scanner** (items attendus du lot, en orange, les périmés en rouge), **Scannés** (avec ✕ par
    ligne, « Annuler le dernier », « Vider la liste ») et **Lot** (exigences scannées / attendues) ;
    le bouton ↶ de la barre du bas annule le dernier scan ;
  - **Stock** (rôles gestion et admin, après scan du badge) : état des stocks en lecture seule, les
    types les plus critiques en premier.
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
./build/QRProtecApp   # les modèles *.qr sont lus et enregistrés dans app/templates/ (sélectionnés par défaut au premier lancement)
```

Dépendances : GLFW, OpenGL, libpng, FreeType, OpenSSL (HTTPS vers un back distant), libxdo (SDK
Inateck). L'icône de la fenêtre est celle du site (`database/inventory/web/icon-192.png`,
`make icon`), embarquée dans l'exécutable ; sous Wayland, le bureau l'affiche grâce à
`qrprotec.desktop` (identifiant d'application `qrprotec`). L'encodeur QR
([Nayuki](https://www.nayuki.io/page/qr-code-generator-library), MIT) et `stb_image` (PNG/JPEG,
domaine public) sont fournis dans `app/third_party/`.

**Première utilisation** : tant qu'aucun administrateur n'a de badge valide, le logiciel propose de créer
le compte de l'administrateur ; il est connecté directement en mode privilégié et peut imprimer
son badge (ou utiliser `manage.py createadmin` sur le serveur).

La police DejaVu Sans (accents) est fournie dans `app/third_party/fonts/` et copiée à côté de
l'exécutable ; une autre police peut être imposée avec la variable `QRPROTEC_FONT`.

Les réglages sont dans `~/.config/qrprotec/app.conf` (`0600`) et s'éditent depuis **Gestion > Réglages** :
serveur (URL de l'API locale, ou `https://...` d'un back distant avec la clé du front et, si besoin,
le certificat de l'autorité ; `QRPROTEC_API_URL`, `QRPROTEC_API_KEY`, `QRPROTEC_API_CA_FILE` et
`QRPROTEC_API_TOKEN` priment), modèle d'étiquette par usage, imprimante, délai de réinitialisation,
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
- **Mauvais scan** (produit périmé, code inconnu) : bip et LED orange de la douchette via le SDK Inateck ;
  en mode HID, bip de l'ordinateur et clignotement rouge de l'écran.
- **Douchette** : la recherche et la connexion sont accessibles à tous ; les paramètres (mode HID,
  volume, préfixe…) et la déconnexion demandent un utilisateur connecté. À la fin d'une recherche
  (8 s, ou dès qu'elle apparaît), l'application se reconnecte seule à la dernière douchette utilisée ;
  sinon, la douchette se choisit dans **Douchette > Connecter à** (accessible à tous). Un appareil qui
  refuse l'authentification du SDK n'est plus proposé. La douchette ne doit pas être appairée à l'ordinateur, et aucun autre appareil Bluetooth
  (casque, souris…) ne doit y être connecté pendant la recherche : le SDK Inateck échoue sinon.
- **Téléphone-douchette** (menu Douchette > Téléphone comme douchette) : « Créer une session » affiche
  un QR code à scanner avec l'appareil photo du téléphone ; les codes scannés par le téléphone arrivent
  dans la pile. La fenêtre et la barre de menu indiquent si le téléphone est connecté et, sinon, le
  temps restant avant la fermeture de la session (délai réglable). « Nouveau QR code » et « Fermer la
  session » sont dans la même fenêtre.
- **Clavier** : tout se fait sans souris. Chaque fenêtre a sa touche de fonction : `F1` Pile de scans,
  `F2` Lots, `F3` Vérif, `F4` Téléphone-douchette, puis `F5` à `F12` pour celles du menu Gestion, dans
  l'ordre du menu (État des stocks, Inventaire, Paquet fermé, Gestion des lots, Journal des opérations,
  Utilisateurs, Éditeur d'étiquettes, Réglages). La touche d'une fenêtre déjà au premier plan la ferme.
  Dans une fenêtre à onglets, `Ctrl+1`, `Ctrl+2`… choisissent l'onglet. `Ctrl+F11` met les fenêtres en
  plein écran (une seule visible à la fois, sur toute la zone sous la barre de menu), `Ctrl+Tab` passe
  à la fenêtre suivante, `Ctrl+W` ferme celle qui a le focus, `Super+L` déconnecte. Chaque bouton a une lettre soulignée :
  `Alt` + cette lettre l'actionne (les lettres sont attribuées par fenêtre, les boutons des tableaux
  n'en ont pas). Les flèches déplacent le focus dans une fenêtre et `Entrée` ou `Espace` valide
  l'élément choisi.
- **Inactivité** : après 15 min (réglable), la pile est vidée, l'utilisateur déconnecté, le plein
  écran quitté et les fenêtres remises à leur place par défaut.

### Mode privilégié (badge gestion ou admin)

Fond orange. Menu **Gestion** (Réglages, Utilisateurs et Éditeur d'étiquettes réservés au rôle admin) :

- **État des stocks** : barre par type, verte au-dessus du minimum, orange en dessous, rouge à 0,
  avec « quantité/minimum » (ex : `32/100`).
- **Inventaire** : réception d'une commande (type, péremption, quantité, paquet fermé) avec
  impression des étiquettes en série, types d'items, recherche d'items, réimpression d'une étiquette,
  suppression exceptionnelle, liste des paquets fermés.
- **Paquet fermé** : fiche d'un paquet (ouverte en scannant son étiquette, depuis la dernière
  réception ou la liste des paquets) et ouverture avec impression de toutes ses étiquettes.
- **Gestion des lots** : types de lots et contenu attendu (avec emplacement), création de lots,
  étiquettes publique et privée, régénération de la clé, scellage (numéro du scellé, étiquette du
  scellé) et bris du scellé.
- **Utilisateurs** (admin) : création, rôle (secouriste, gestion, admin), renouvellement et impression
  des badges.
- **Éditeur d'étiquettes** (admin) : modèles avec usage (item, paquet, lot public, lot privé, scellé, badge),
  onglet **Placeholders** listant les `{{placeholders}}` disponibles, textes, QR codes et images
  (logo PNG ou JPEG). Il se manie comme un logiciel de dessin : barre d'outils (Nouveau, Ouvrir,
  Enregistrer, Enregistrer sous, Annuler, Rétablir), calques à gauche (glisser pour changer l'ordre),
  étiquette au centre où l'on sélectionne, déplace et redimensionne les éléments à la souris
  (magnétisme sur une grille de 0,5 mm et sur le centre, Alt pour s'en passer), zoom à Ctrl+molette,
  propriétés à droite. Raccourcis : Ctrl+S, Ctrl+O, Ctrl+N, Ctrl+Z / Ctrl+Y, Ctrl+C / Ctrl+V / Ctrl+D,
  Suppr, flèches (0,1 mm, Maj : 1 mm). Les modifications non enregistrées sont signalées avant
  d'ouvrir un autre modèle.
- **Réglages** : dont les notifications SMS. Sans modèle choisi pour un usage, le premier modèle du
  dossier fait pour cet usage est utilisé (ex : `scelle.qr`), sinon le QR code seul.

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
**Nom de l'antenne** insère `{{titre}}` centré et en gras ; son texte (« PROTECTION CIVILE / PARIS
CENTRE » par défaut) se change dans les Réglages et s'applique à toutes les étiquettes.

**Modèles versionnés** : les modèles `*.qr` et leurs images (logo) sont lus et enregistrés dans
`app/templates/`, quel que soit le dossier de lancement. Commitez-y vos modèles pour les retrouver à
chaque clone (autre dossier possible dans les Réglages ou via `QRPROTEC_TEMPLATES_DIR`).

`app/templates/` contient un modèle d'exemple par usage (40 × 30 mm). Les modèles de lot public et
de badge affichent `images/protection-civile.png`, le logo de la protection civile (à déposer
dans `app/templates/images/`).

**Images des étiquettes** : toutes les images à poser sur les étiquettes (logos...) vont dans
`app/templates/images/` ; l'éditeur d'étiquettes les propose dans « Choisir une image du dossier ».
L'icône de l'application (triangle « QR ») ne va pas sur les étiquettes : `make icon
ICON=chemin/vers/icone.png` (ImageMagick) remplace l'icône du site web (onglet, écran d'accueil,
notifications) et de la fenêtre.
