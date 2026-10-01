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
| `qrprotec.spec` | Spec RPM. `%bcond kiosk` (activé par défaut) : `--without kiosk` produit un paquet back + Caddy seul, sans front. |
| `make-sources.sh` | Prépare `Source0` (code + sous-modules `imgui`, `scanner_lib`) et `Source1` (dépendances Python figées). |
| `lock2requirements.py` | Convertit `poetry.lock` en `requirements.txt` avec empreintes (`--require-hashes`). |
| `build-rpm.sh` | `rpmbuild` complet (appelé par `make rpm` dans un conteneur, ou `make rpm-local`). |
| `Containerfile` | Image Fedora de construction, avec les dépendances de build en cache. |
| `version.sh` | Version depuis le tag git : `v1.2.0` → `1.2.0`, entre deux tags `1.2.0^3.gabc1234`. |
| `publish-repo.sh` | Met à jour le dépôt dnf statique (`createrepo_c`, signature optionnelle). |
| `rpmlint.toml` | Exceptions justifiées pour `rpmlint`. |
| `files/` | Tout ce qui est installé hors code : unités systemd, configurations, scripts. |

## Ce qui est embarqué

- **Back** : `database/` copié dans `/usr/share/qrprotec/backend` (sans `tests.py`), dépendances
  Python (Django, DRF, asgiref, sqlparse) dans `/usr/share/qrprotec/vendor`, compilées en `.pyc`. Les
  versions sont celles de `poetry.lock` ; `%prep` vérifie que les `Provides: bundled(...)` du spec
  correspondent (mettre à jour les `%global *_version` du spec après un `poetry update`).
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
| `qrprotec-alerts.timer` | `check_alerts` chaque jour à 7 h 45. |
| `qrprotec-backup.timer` | Sauvegarde (quotidienne par défaut, réglable). |
| `80-qrprotec.preset` | Active les unités à l'installation. |
| `logind-qrprotec.conf`, `sleep-qrprotec.conf` | Pas de mise en veille de la borne. |
| `qrprotec.sysusers`, `qrprotec.tmpfiles` | Utilisateurs système et dossiers. `qrprotec-kiosk` est dans `dialout` (imprimante série), `video`, `render`, `input`, `audio`. |

### Réseau

Les deux API n'écoutent que sur `127.0.0.1`. Caddy expose 80 (redirection) et 443 et ne proxifie que
le port public. L'API locale n'est jamais exposée. `qrprotec.conf` est lu par le back **et** par
Caddy : le nom d'hôte (`QRPROTEC_DOMAIN`) n'est défini qu'à un endroit et donne le site Caddy,
`ALLOWED_HOSTS` et la base des QR codes (`qrprotec-manage` en déduit les variables Django). SELinux :
le `%post` active `httpd_can_network_connect` pour que Caddy joigne le back.

### Kiosk

`qrprotec-kiosk-session` : copie les modèles fournis dans `~qrprotec-kiosk/templates` **sans écraser**
ceux modifiés sur la borne, programme l'extinction de l'écran (swayidle + wlopm, ou wlr-randr),
vérifie la présence des bibliothèques Wayland chargées dynamiquement par GLFW (cause classique
d'écran noir), puis lance le front. `Ctrl+Alt+F2` donne une console de maintenance (`cage -s`).

## Scripts d'administration

| Script | Rôle |
|---|---|
| `qrprotec-manage` | `manage.py` avec l'environnement de la borne, exécuté sous l'utilisateur `qrprotec`. |
| `qrprotec-setup` | Assistant idempotent : nom d'hôte, TLS, clavier, écran, sauvegardes, pare-feu, premier admin, restauration. Interactif ou par options (`--help`). |
| `qrprotec-backup` | Sauvegarde (instantané SQLite cohérent via l'API de sauvegarde de SQLite, sans arrêter le back, intégrité vérifiée) + `/etc/qrprotec` + réglages et modèles du kiosk + `MANIFEST`, en `tar.xz`, rotation. `--list`, `--apply-schedule`, restauration via `qrprotec-setup --restore`. |
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

## Publication (CI)

`.github/workflows/rpm.yml` : à chaque push sur `main` touchant le code ou le packaging, construit les
RPM (Fedora 42 et 43) ; sur un tag `vX.Y.Z`, crée la release GitHub et met à jour le dépôt dnf
statique sur la branche `gh-pages` (`fedora/<version>/<arch>/`, 5 dernières versions), servi par
GitHub Pages. Variables optionnelles : `RPM_PAGES_REPO` / `RPM_PAGES_URL` / `RPM_PAGES_TOKEN` pour
publier dans un dépôt public dédié si celui-ci reste privé, `RPM_GPG_PRIVATE_KEY` /
`RPM_GPG_PASSPHRASE` pour signer.

> La découpe en plusieurs paquets (back seul, front seul, borne complète, front de bureau sans kiosk)
> est en cours dans une autre branche ; ce document sera complété à son intégration.
