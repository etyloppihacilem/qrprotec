# Architecture d'ensemble

## Les trois programmes

QRProtec est fait de trois programmes qui partagent une seule base de données :

```
                         ┌──────────────────────────────────────────────┐
                         │  Back Django  (database/)                    │
                         │                                              │
  Téléphone ──HTTPS──▶ Caddy ──▶ API publique  127.0.0.1:8000 ─┐        │
  (front web,            │       + pages web + WebSocket phone │        │
   appareil photo)       │                                     ├─▶ SQLite│
                         │       API locale    127.0.0.1:8001 ─┘        │
                         │       + admin Django + WebSocket front       │
                         └──────────────────────▲───────────────────────┘
                                                │ HTTP + WebSocket (boucle locale)
                         ┌──────────────────────┴───────────────────────┐
                         │  Front poste ImGui  (app/)                   │
                         │  douchette Inateck (Bluetooth ou clavier),   │
                         │  imprimante Niimbot B1 (port série USB)      │
                         └──────────────────────────────────────────────┘
```

| Programme | Langage | Rôle | Qui l'utilise |
|---|---|---|---|
| **Back** (`database/`) | Python, Django + DRF | Base de données, règles de gestion, deux API HTTP, sert aussi le front web | Les deux fronts |
| **Front poste** (`app/`) | C++17, Dear ImGui, GLFW/OpenGL | Poste fixe : scans à la douchette, vérifs, gestion du stock, impression des étiquettes, administration | Le local matériel |
| **Front web** (`database/inventory/web/`) | HTML/JS sans framework | Page de téléphone : scan à l'appareil photo, vérif d'un lot sur le terrain, état des stocks en lecture | Les secouristes, avec leur téléphone |

En production, les trois tournent sur une **borne** : un petit PC sous Fedora Server, avec écran,
douchette et imprimante, qui démarre directement sur le front en plein écran (mode kiosk, Cage sur
`tty1`). Voir [packaging.md](packaging.md).

## Deux API, une seule application Django

Le back expose **la même application Django sur deux ports** :

- **API publique** (port 8000, derrière Caddy en HTTPS) : ce qu'un téléphone peut faire. Lecture d'un
  item, d'un lot, d'un paquet ; écriture seulement avec la **clé** de l'objet (la clé d'un lot est
  imprimée sur son étiquette privée). Elle sert aussi le front web et les WebSockets du
  téléphone-douchette.
- **API locale** (port 8001, jamais exposée) : gestion complète sans clé (types, réception,
  utilisateurs, réglages, admin Django). Elle n'accepte que les adresses de
  `QRPROTEC_LOCAL_API_ALLOWED_ADDRESSES` (boucle locale par défaut) et, si défini, un jeton dans
  l'en-tête `X-QRProtec-Token`.

Le choix de l'API se fait **par le port qui reçoit la requête**, pas par l'URL : `manage.py serve`
lance deux serveurs qui marquent chaque requête (`environ['qrprotec.role']`), et
`ApiRoleMiddleware` choisit le jeu d'URLs (`qrprotecDB/urls.py` ou `urls_local.py`). Une route
locale appelée sur le port public renvoie donc simplement 404. Raisons détaillées dans
[decisions.md](decisions.md#deux-api-sur-deux-ports).

> La séparation du front et du back sur deux machines (clé d'API par front, URL configurable) est en
> cours dans une autre branche ; ce document sera complété quand elle sera intégrée. Le principe
> reste : un front local de confiance parle à l'API locale, tout ce qui vient du réseau passe par une
> API authentifiée.

## Ce qui circule entre les programmes

Les fronts ne calculent rien de définitif : **ils envoient des listes d'identifiants scannés**, le
back décide. Exemple d'une vérif de lot depuis le poste :

1. La douchette lit `serphy20271231000000A1` ; le front reconnaît localement un iid
   (`core/codes.cpp`, sans réseau), l'ajoute à la pile et demande `GET /api/items/<iid>/` pour
   afficher son type et sa péremption.
2. L'utilisateur scanne l'étiquette privée du sac (`…/verif?lot=sacpse00000001&key=…`) : le front
   charge le lot (`GET /api/lots/<id>/`) et affiche ce qui est attendu.
3. Il scanne son badge (`…/badge?m=M0042&key=…`) : `POST /api/auth/` (plus le PIN si besoin).
4. Il valide : `POST /api/lots/<id>/verif/` avec la liste des iids et l'utilisateur. Le back
   (`services.perform_verif`) range les items, marque les manquants, enregistre la vérif, brise un
   éventuel scellé, envoie les notifications, et renvoie un compte rendu que le front affiche.

Le front web fait exactement la même chose sur l'API publique, avec la clé du lot en plus.

Le front calcule toutefois un **aperçu** (quels items vont dans quel sous-lot, quels lots seraient
complets en vérif partielle) avec le même algorithme que le serveur : voir
[regles-de-gestion.md](regles-de-gestion.md#répartition-des-items-entre-les-lots).

## Les QR codes sont des URLs

Les étiquettes de lot, de badge, de paquet et de scellé contiennent une **URL du front web**
(`https://<domaine>/verif?lot=…`). Scanné avec l'appareil photo d'un téléphone, le QR ouvre
directement la bonne page ; scanné à la douchette, le front poste reconnaît le chemin et les
paramètres **sans regarder le domaine**. Changer de nom de domaine n'invalide donc aucune étiquette
imprimée. Seules les étiquettes d'item contiennent l'identifiant brut (22 caractères), parce qu'il
y en a des milliers et qu'un QR court reste lisible sur une petite étiquette.

Détails des formats : [donnees.md](donnees.md#contenu-des-qr-codes).

## Concurrence et cohérence

- **Une seule base SQLite**, un seul processus back (`manage.py serve`, multi-thread). Les
  opérations qui modifient plusieurs lignes (vérif, réception, déplacement) sont dans
  `transaction.atomic()` avec `select_for_update()` sur les items concernés.
- Les compteurs d'identifiants (`ItemsPacks.last_sequence`, `LotSequence`, `Sequence`) sont
  incrémentés sous verrou : deux réceptions simultanées ne produisent jamais le même iid.
- Les sessions du téléphone-douchette vivent **en mémoire** dans le processus `serve` (d'où un seul
  processus). Un redémarrage du back ferme les sessions ; il suffit d'afficher un nouveau QR.
- Le front poste ne garde aucun état persistant hors de ses réglages (`app.conf`) et des modèles
  d'étiquettes : tout le reste est rechargé depuis l'API.

## Où se trouvent les données en production

| Donnée | Emplacement |
|---|---|
| Base SQLite | `/var/lib/qrprotec/db.sqlite3` |
| Configuration du back et de Caddy | `/etc/qrprotec/qrprotec.conf` |
| Clé secrète Django | `/etc/qrprotec/secret_key` |
| Réglages du front du kiosk | `/var/lib/qrprotec-kiosk/.config/qrprotec/app.conf` |
| Modèles d'étiquettes du kiosk | `/var/lib/qrprotec-kiosk/templates/` |
| Sauvegardes | `/var/backups/qrprotec/` |

En développement : `database/db.sqlite3`, `~/.config/qrprotec/app.conf`, `app/templates/`.
