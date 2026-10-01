# Borne complète (front + back, mode kiosk)

Une seule machine fait tout : le back (base de données, API, site pour les téléphones), Caddy
(HTTPS) et le front en plein écran sur l'écran de la borne. Au démarrage, la borne affiche
directement QRProtec, sans bureau ni écran de connexion.

```
          Internet / réseau local
                   │ 443 (HTTPS)
┌──────────────────▼─────────────────────────┐
│ Borne Fedora Server                        │
│  Caddy ──▶ API publique 127.0.0.1:8000 ┐   │
│                                        ├─ base SQLite
│  Front kiosk ──▶ API locale 127.0.0.1:8001┘│
│  (écran, douchette, imprimante)            │
└────────────────────────────────────────────┘
```

## 1. Préparer la machine

1. Installer **Fedora Server** (42 ou plus) en « Minimal Install ». Créer un compte administrateur
   (avec `sudo`) pendant l'installation.
2. Brancher l'écran, le clavier, l'imprimante (USB). Ne **pas** appairer la douchette Bluetooth dans
   le système : le logiciel s'y connecte lui-même.
3. Choisir le **nom** de la borne, qui sera l'adresse du site pour les téléphones :
   - **nom de domaine public** (`inventaire.mon-antenne.org`) pointant vers la borne, ports 80 et 443
     ouverts depuis Internet : certificat Let's Encrypt automatique, les téléphones marchent partout ;
   - ou **adresse IP / nom local** (`192.168.1.50`) : réseau local seulement, certificat de l'autorité
     interne de Caddy, à installer sur les téléphones (voir plus bas).

   Ce nom est encodé dans les QR codes des étiquettes ; en changer plus tard est possible (le poste
   ignore le domaine), mais les téléphones ouvriront l'ancienne adresse avec les anciennes étiquettes.

## 2. Installer

```sh
curl -fsSL https://etyloppihacilem.github.io/qrprotec/bootstrap.sh | sudo bash
```

Le script ajoute le dépôt QRProtec, installe le paquet et propose de lancer l'assistant. Équivalent
manuel :

```sh
sudo curl -fsSL -o /etc/yum.repos.d/qrprotec.repo https://etyloppihacilem.github.io/qrprotec/qrprotec.repo
sudo dnf install qrprotec
```

L'installation crée les utilisateurs système, génère la clé secrète, active et démarre le back, Caddy,
les alertes, les sauvegardes et le kiosk, et règle SELinux pour que Caddy joigne le back.

## 3. Configurer

```sh
sudo qrprotec-setup
```

L'assistant pose les questions une à une (valeur actuelle entre crochets, Entrée pour la garder) :

| Question | Exemple |
|---|---|
| Nom d'hôte public | `inventaire.mon-antenne.org` ou `192.168.1.50` |
| Certificat HTTPS | `admin@mon-antenne.org` (Let's Encrypt) ou `internal` |
| Disposition du clavier | `fr` |
| Extinction de l'écran (secondes) | `1800` (0 = jamais) |
| Dossier, nombre et fréquence des sauvegardes | `/var/backups/qrprotec`, `14`, `daily` |
| Ouvrir le pare-feu (http/https) | oui, pour que les téléphones atteignent la borne |
| Créer un administrateur | matricule, nom, prénom, PIN |

Il est relançable à tout moment. Version sans questions :

```sh
sudo qrprotec-setup --domain inventaire.mon-antenne.org --tls admin@mon-antenne.org \
    --keyboard fr --open-firewall --no-admin --yes
```

`sudo qrprotec-setup --help` liste toutes les options.

## 4. Redémarrer

```sh
sudo systemctl reboot
```

La borne démarre sur QRProtec en plein écran. Si aucun administrateur n'a été créé, le logiciel
propose de le faire. Suivez ensuite la [mise en place](../utilisateur/mise-en-place.md).

`Ctrl+Alt+F2` ouvre une console de maintenance (retour avec `Ctrl+Alt+F1`).

## 5. Tester depuis un téléphone

Ouvrir `https://<nom de la borne>/` : la page QRProtec s'affiche, « Démarrer la caméra » doit
fonctionner.

Avec `internal` (adresse IP, réseau local), le téléphone ne connaît pas l'autorité de certification
de Caddy. Récupérer son certificat :

```sh
sudo find /var/lib/caddy -name root.crt
```

et l'installer sur chaque téléphone (Android : Paramètres > Sécurité > Installer un certificat ;
iPhone : l'envoyer par mail, l'installer, puis l'activer dans Réglages > Général > Informations >
Réglages des certificats). Sans cela, il faut accepter un avertissement à chaque fois, et les
notifications web ne fonctionnent pas.

## Ce qui est installé

| Élément | Emplacement |
|---|---|
| Back et ses dépendances Python | `/usr/share/qrprotec/backend`, `/usr/share/qrprotec/vendor` |
| Front | `/usr/libexec/qrprotec/qrprotec-front` (+ SDK douchette dans `/usr/lib64/qrprotec/`) |
| Modèles d'étiquettes fournis | `/usr/share/qrprotec/templates` (copiés dans `/var/lib/qrprotec-kiosk/templates`) |
| Configuration | `/etc/qrprotec/qrprotec.conf`, `/etc/qrprotec/kiosk.conf`, `/etc/qrprotec/backup.conf`, `/etc/caddy/Caddyfile.d/qrprotec.caddyfile` |
| Clé secrète | `/etc/qrprotec/secret_key` |
| Base de données | `/var/lib/qrprotec/db.sqlite3` |
| Réglages du front | `/var/lib/qrprotec-kiosk/.config/qrprotec/app.conf` |
| Sauvegardes | `/var/backups/qrprotec/` |
| Commandes | `qrprotec-setup`, `qrprotec-manage`, `qrprotec-backup` |

Services : `qrprotec` (back), `caddy`, `qrprotec-kiosk` (écran), `qrprotec-alerts.timer` (alertes à
7 h 45), `qrprotec-backup.timer` (sauvegardes). La borne ne se met jamais en veille.

Suite : [exploitation.md](exploitation.md).
