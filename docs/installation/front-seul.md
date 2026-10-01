# Front seul (borne kiosk ou poste de bureau)

Un front seul se connecte à un back installé sur **une autre machine** (une [borne
complète](borne-complete.md) ou un [back seul](back-seul.md)), en HTTPS, avec sa propre **clé
d'API**. Un front local et plusieurs fronts distants peuvent travailler en même temps sur la même
base.

Deux paquets :

| Paquet | Pour | Démarrage |
|---|---|---|
| `qrprotec-kiosk` | Un poste **dédié** : le front en plein écran au démarrage, sans bureau | Automatique (Cage sur `tty1`) |
| `qrprotec-front` | Un ordinateur qui **sert aussi à autre chose** : le front comme une application | Menu **QRProtec** ou commande `qrprotec-front` |

## Avant de commencer : la clé du poste

Sur le serveur, créez une clé pour ce poste et notez-la (elle n'est affichée qu'une fois) :

```sh
sudo qrprotec-setup --front-key accueil
```

Si le serveur utilise l'autorité interne de Caddy (`QRPROTEC_TLS=internal`), copiez aussi son
certificat sur le poste :

```sh
# sur le serveur
sudo cat /var/lib/caddy/.local/share/caddy/pki/authorities/local/root.crt
# sur le poste : enregistrer le contenu, par exemple dans /etc/qrprotec/server-ca.crt
```

Avec un certificat Let's Encrypt, rien à copier.

## Borne kiosk reliée à un back distant

Machine Fedora Server 42+ minimale, avec écran, douchette et imprimante (voir [borne
complète](borne-complete.md#1-préparer-la-machine) pour la préparation).

```sh
curl -fsSL https://etyloppihacilem.github.io/qrprotec/bootstrap.sh | sudo QRPROTEC_PACKAGE=qrprotec-kiosk bash
```

puis :

```sh
sudo qrprotec-setup --api-url https://inventaire.mon-antenne.org --api-key qrpf_... \
    --api-ca-file /etc/qrprotec/server-ca.crt     # seulement pour l'autorité interne de Caddy
sudo systemctl reboot
```

L'assistant demande aussi le clavier, l'extinction de l'écran et les sauvegardes. La connexion est
écrite dans `/etc/qrprotec/front-api.conf` (lisible par root seulement), qui **prime** sur les
réglages du front : sur une borne, l'adresse du serveur ne se change pas depuis l'écran.

- Revenir à un back sur la même machine : `sudo qrprotec-setup --local-api`.
- Vérifier : en haut à droite du front, « API en ligne » ; sur le serveur,
  `sudo qrprotec-manage frontkey list` montre la dernière connexion du poste.

Les sauvegardes d'un kiosk relié à un back distant contiennent ses réglages et ses modèles
d'étiquettes, **pas la base** (elle est sauvegardée sur le serveur).

## Poste de bureau sans kiosk

Sur un ordinateur Fedora (Workstation ou autre bureau) :

```sh
curl -fsSL https://etyloppihacilem.github.io/qrprotec/bootstrap.sh | sudo QRPROTEC_PACKAGE=qrprotec-front bash
```

(ou `sudo dnf install qrprotec-front` une fois le dépôt ajouté). Aucun service n'est installé :
QRProtec est une application du menu, avec son icône.

1. Lancez **QRProtec** depuis le menu des applications.
2. Le serveur n'est pas encore configuré : cliquez sur **API hors ligne** dans la barre de menu.
   Les réglages du serveur s'ouvrent **sans badge** (uniquement tant que le serveur est
   injoignable).
3. Renseignez l'**URL** (`https://inventaire.mon-antenne.org`), la **clé du front** (`qrpf_…`) et,
   si besoin, le **certificat de l'autorité**. **Tester la connexion**, puis **Enregistrer**.
4. Scannez votre badge : vous êtes connecté au même inventaire que la borne.

Réglages et modèles sont propres à l'utilisateur : `~/.config/qrprotec/app.conf` (`0600`, contient la
clé) et `~/.local/share/qrprotec/templates`. Pour imprimer sur la Niimbot, l'utilisateur doit être
dans le groupe `dialout` :

```sh
sudo usermod -aG dialout $USER     # puis se déconnecter / reconnecter
```

Les variables d'environnement `QRPROTEC_API_URL`, `QRPROTEC_API_KEY`, `QRPROTEC_API_CA_FILE` (et
`QRPROTEC_API_TOKEN` pour le jeton de l'API locale) priment sur les réglages : pratique pour un
déploiement scripté.

Le poste de bureau peut aussi utiliser un back **sur la même machine** (paquet `qrprotec-server`
installé à côté) : laissez l'URL par défaut `http://127.0.0.1:8001`, sans clé.

## Dépannage de la connexion

| Message | Cause |
|---|---|
| « Clé API du front manquante » / « invalide ou révoquée » | Clé mal copiée, ou révoquée sur le serveur (`frontkey list`) |
| Erreur de certificat | Autorité interne de Caddy non fournie (`--api-ca-file` ou réglage du front), ou nom d'hôte différent de `QRPROTEC_DOMAIN` du serveur |
| « API hors ligne » | Serveur éteint, pare-feu (443 fermé), mauvaise URL |

## Changer de paquet

Passer d'une borne complète à un kiosk seul (le back part sur un serveur) : restaurez d'abord une
sauvegarde de la borne sur le nouveau serveur (`qrprotec-setup --restore`), puis sur la borne :

```sh
sudo dnf install qrprotec-kiosk && sudo dnf mark user qrprotec-kiosk
sudo dnf remove qrprotec qrprotec-server
sudo qrprotec-setup --api-url https://NOUVEAU-SERVEUR --api-key qrpf_...
```

(`dnf mark user` évite que `dnf remove qrprotec` retire aussi le kiosk, installé comme dépendance.)
