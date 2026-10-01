# Back seul (serveur sans écran)

Le paquet `qrprotec-server` installe le back (base de données, API, site des téléphones), Caddy
(HTTPS), les alertes et les sauvegardes, **sans** front ni kiosk. Les postes se connectent à ce
serveur à distance, chacun avec sa propre clé d'API.

```
                       Internet / réseau
   Téléphones ──HTTPS──┐        ┌──HTTPS + clé──── Borne kiosk « accueil » (qrprotec-kiosk)
                       ▼        ▼
┌───────────────────── Caddy :443 ──────────────────────────┐
│ Serveur (qrprotec-server)                                 │
│   sans en-tête X-QRProtec-Key ─▶ API publique 127.0.0.1:8000
│   avec   en-tête X-QRProtec-Key ─▶ API distante 127.0.0.1:8002 ─┐
│                                                     base SQLite │
│   (API locale 127.0.0.1:8001 : seulement un front sur la même machine)
└──────────────────────────────────────▲────────────────────┘
                                       └──HTTPS + clé──── Poste de bureau « logistique » (qrprotec-front)
```

Cas d'usage : un serveur de l'association (ou une VM) toujours allumé et joignable, et des postes
dans un ou plusieurs locaux.

## 1. Installer

Machine Fedora 42+ (Server, sans bureau), joignable par les postes et les téléphones.

```sh
curl -fsSL https://etyloppihacilem.github.io/qrprotec/bootstrap.sh | sudo QRPROTEC_PACKAGE=qrprotec-server bash
```

ou, après avoir ajouté le dépôt (voir [borne complète](borne-complete.md#2-installer)) :

```sh
sudo dnf install qrprotec-server
```

## 2. Configurer

```sh
sudo qrprotec-setup
```

Seules les questions du back sont posées : nom d'hôte, certificat HTTPS, sauvegardes, pare-feu,
premier administrateur. Les postes **et** les téléphones joignent le serveur par ce nom :

- avec un **nom de domaine public** et Let's Encrypt (`--tls admin@…`), les postes n'ont rien de
  plus à configurer ;
- avec `internal` (réseau privé, adresse IP), chaque poste devra connaître l'autorité de Caddy :
  copier `/var/lib/caddy/.local/share/caddy/pki/authorities/local/root.crt` sur les postes (et
  l'installer sur les téléphones).

Ouvrez le pare-feu (`--open-firewall`) : seuls les ports 80 et 443 sont nécessaires.

Le premier administrateur est créé par l'assistant, ou plus tard :
`sudo qrprotec-manage createadmin M001 Nom Prénom --pin 4821`. Comme il n'y a pas d'écran, imprimez
son badge depuis le premier poste connecté (scanner ou saisir l'URL affichée, puis **Gestion >
Utilisateurs**).

## 3. Créer une clé par poste

Chaque front distant a **sa propre clé**, ce qui permet d'en révoquer une (poste volé, réformé) sans
toucher aux autres :

```sh
sudo qrprotec-setup --front-key accueil        # ou : sudo qrprotec-manage frontkey add accueil
sudo qrprotec-setup --front-key logistique
```

La clé (`qrpf_…`) n'est **affichée qu'une fois** : notez-la pour la saisir sur le poste. Le serveur
n'en garde que l'empreinte.

```sh
sudo qrprotec-manage frontkey list             # fronts, dernière utilisation, dernière adresse
sudo qrprotec-manage frontkey revoke accueil   # le poste perd l'accès immédiatement
sudo qrprotec-manage frontkey delete accueil
```

Puis installez les postes : [front seul](front-seul.md).

## Ce qui tourne

| Service | Rôle |
|---|---|
| `qrprotec.service` | Back : API publique `:8000`, API locale `:8001`, API distante `:8002`, toutes sur `127.0.0.1` |
| `caddy.service` | HTTPS sur 443 ; envoie à l'API distante les requêtes qui portent l'en-tête `X-QRProtec-Key`, le reste à l'API publique |
| `qrprotec-alerts.timer` | Alertes SMS quotidiennes (7 h 45) |
| `qrprotec-backup.timer` | Sauvegardes (base, configuration, clé secrète) |

L'API distante a les mêmes droits que l'API locale (gestion complète), mais refuse toute requête sans
clé valide (401). Sans clé créée, elle ne sert à rien. L'API locale n'est jamais exposée.

Configuration, sauvegardes, mises à jour : [exploitation.md](exploitation.md).

## Ajouter un écran plus tard

Un serveur `qrprotec-server` peut devenir une borne complète : `sudo dnf install qrprotec-kiosk`
(son front utilisera l'API locale de la machine, sans clé), puis `sudo qrprotec-setup` pour le
clavier et l'écran.
