# Modèle de données et identifiants

Toutes les tables sont dans `database/inventory/models.py`. La base est SQLite (fichier unique,
facile à sauvegarder, largement suffisant pour quelques milliers d'items et une poignée de postes).

## Vue d'ensemble

```
ItemType ──< ItemsPacks ──< Items >── Lots >── LotType ──< LotRequirements >── ItemType
  (type)     (type+date)    (iid)   location   (lot)        (contenu attendu)
                              │                  │ parent (lot global)
                              │                  └──< Lots (sous-lots)
                              ├── sealed_pack ──> SealedPacks (paquet fermé)
                              └──< VerifItem >── Verifs ──> Lots (None = stock)

Secouristes ──< PushSubscription        NotificationSettings (1 ligne)   SmsRecipient
PushKeys (1 ligne)   Sequence / LotSequence (compteurs)
```

## Les tables

### Items et types

| Modèle | Rôle | Points importants |
|---|---|---|
| `ItemType` | Type d'item (« Sérum phy 10 ml ») | Clé primaire `type` : **code de 6 caractères** alphanumériques (`serphy`), qui commence chaque iid. `min_quantity` : seuil d'alerte du stock. `perissable` : date de péremption obligatoire. `default_pack_size` : taille proposée à la réception. `low_notified` / `empty_notified` / `order_notified` : alerte déjà envoyée (voir [back.md](back.md#notifications)). `tear_off` : étiquette à déchirer avant utilisation (item non scanné = utilisé). `archived` : plus proposé (réception, contenu des lots, stock, prévisions). |
| `ItemsPacks` | Groupe d'items d'un même type et d'une même date | Clé `type + AAAAMMJJ`. Sert de **préfixe d'iid** et porte le compteur `last_sequence`. La date de péremption est stockée ici, pas sur l'item. Ce n'est **pas** un paquet physique (voir `SealedPacks`). |
| `Items` | Un objet physique étiqueté | Clé `iid` (22 caractères). `location` : le lot qui le contient, `NULL` = en stock. `status` : `active`, `missing` (disparu), `replaced` (remplacé, sorti d'un lot), `deleted` (supprimé à la main, avec raison), `out` (sorti du stock sans lot, voir les règles de gestion). `missed_verifs` : vérifs consécutives où il était attendu sans être scanné. `last_seen*` : dernier passage (qui, quand, pendant quoi). |
| `SealedPacks` | Paquet fermé (boîte de 25 compresses) | Identifiant base 62 sur 8 caractères (`Sequence 'sealed_pack'`). Les items existent en base dès la réception, avec `sealed_pack` renseigné ; leurs étiquettes ne sont imprimées qu'à l'ouverture (`opened`). |

### Lots

| Modèle | Rôle | Points importants |
|---|---|---|
| `LotType` | Modèle de lot (« Sac PSE ») | Clé `type` (6 caractères). `storage=True` : **rangement du stock** (armoire, tiroir) : ses items comptent dans le stock ; c'est toujours un lot unique, sans contenu attendu, dont la clé n'expire pas (`Lots.verif_key_expires` vide). `version` est incrémentée à chaque changement du contenu attendu. `unique=True` : **lot unique**, son lot est créé avec le type et il n'y en a pas d'autre. `archived` : plus proposé à la création de lots. Noms des types et des lots actifs uniques (sans accents ni casse, `models.name_key`). |
| `LotRequirements` | Contenu attendu d'un type de lot | (type de lot, type d'item) unique, `quantity`, `location` libre (« pochette bleue ») affichée pendant la vérif. |
| `Lots` | Un lot physique | Identifiant `type + compteur base 62 sur 8` (`sacpse00000001`). `verif_key` (24 caractères, valable `LOT_KEY_VALIDITY_DAYS`) : la clé de l'**étiquette privée**. `parent` : lot global (sous-lots, voir [règles](regles-de-gestion.md#lots-et-sous-lots)). Champs du scellé (`is_sealed`, `seal_code`, `seal_number`…), du réassort (`verif_recommended`, `restocked*`), de la dernière vérif (`last_verif*`). `active=False` : lot archivé. |
| `LotSequence` | Compteur d'identifiants par type de lot | Incrémenté sous verrou. |

### Vérifs

| Modèle | Rôle |
|---|---|
| `Verifs` | Une vérif d'un lot (ou du stock si `lot` est `NULL`) : qui, quand, complète ou non, compteurs. Une vérif groupée de plusieurs sous-lots crée **une ligne par lot vérifié**. |
| `VerifItem` | Une ligne par item concerné : `present`, `missing` ou `replaced`, et s'il était périmé. C'est l'historique complet de chaque item. |
| `ItemMovement` | Journal des mouvements d'un item (prévisions de stock) : déplacement (`move`, lot de départ et d'arrivée, `NULL` = stock), première absence à une vérif (`used`, ou `discarded` s'il était périmé), `replaced`, `deleted`, `restored`, `out` (sortie du stock sans lot ; un `used` suit si l'item ne revient pas). Une absence reçoit `cancelled` quand l'item est revu ensuite (vérif, déplacement, restauration). `reconstructed` : ligne reconstituée depuis `VerifItem` par la migration 0011. |
| `Operation` | Journal des opérations (fenêtre **Journal des opérations**) : qui (`by`, identité stockée), quand, quoi (`kind` : `verif`, `stock_verif`, `restock`, `remove`, `seal`, `unseal`, `reception`, `item_delete`, `item_restore`, `pack_open`, `pack_close`, `lot`, `lot_key`, `catalog`, `user`, `settings`), lot concerné et son nom au moment de l'opération, résumé lisible et `details` (JSON : compteurs, iids, raison...). Écrit par `journal.record` dans les services et les vues de gestion. `reconstructed` : ligne reconstituée par la migration 0016 depuis les vérifs, les mouvements, les items reçus, les paquets ouverts et le dernier scellage / la dernière ouverture de chaque lot (les précédents n'étaient pas conservés). |

### Utilisateurs

`Secouristes` : matricule (clé primaire, 1 à 16 caractères), nom, prénom, `role` (`normal`,
`gestion`, `admin`), empreinte SHA-256 de la clé du badge (`key_hash` : la clé de 24 caractères,
valable 365 jours, n'est connue qu'à sa création ; `renew_key()` en crée une nouvelle, la renvoie une
seule fois et invalide l'ancien badge), PIN haché avec les hacheurs de mots de passe de Django
(`pin_hash`), compteurs d'échecs, blocage temporaire (`pin_locked_until`), blocage jusqu'à
intervention d'un admin (`pin_blocked`, `pin_reset_required`) et admin à contacter (`pin_contact`, sinon l'admin coché `default_contact`, un seul ; voir `Secouristes.contact_admin`).

L'API ne renvoie la clé du badge (`key`, `badge_url`) que dans la réponse de création
(`POST /api/users/`) et de renouvellement (`renew-key/`), le temps d'imprimer le badge. Pour
réimprimer un badge, il faut donc le renouveler.

Il n'y a **pas** de compte Django (`auth.User`) pour les secouristes : un badge n'est pas un mot de
passe, et la plupart des opérations n'ont besoin que d'une identité. L'admin Django
(`/admin/`, API locale) reste disponible pour le dépannage avec un superutilisateur Django classique.

### Fronts distants

`FrontKey` : une clé d'API par front installé sur une autre machine (`name` unique). Seule
l'**empreinte SHA-256** est stockée (`key_hash`) : la clé (`qrpf_` + 43 caractères) n'est affichée
qu'à sa création (`manage.py frontkey add NOM`). `revoked` coupe l'accès ; `last_used` /
`last_address` (mis à jour au plus une fois par minute) servent à `frontkey list`. Une empreinte
simple suffit (pas de hachage lent comme pour le PIN) : la clé est aléatoire et longue, elle ne se
devine pas par dictionnaire.

### Notifications

`NotificationSettings` (une seule ligne, `pk=1`, `get()` la crée), `SmsRecipient` (identifiant et clé
Free Mobile par destinataire, jamais renvoyés par l'API), `PushKeys` (clés VAPID du serveur, une
ligne générée au premier usage), `PushSubscription` (abonnement d'un navigateur d'admin).

## Les identités dans la base

Les champs `*_by` (`last_seen_by`, `created_by`, `sealed_by`…) sont des chaînes, pas des clés
étrangères. Format défini dans `inventory/idendity.py` :

- `M:M0042` : utilisateur **vérifié** (badge scanné, ou utilisateur connecté sur le poste) ;
- `D:Jean` : nom **déclaré** sans vérification (`"name"` dans la requête), accepté seulement si le
  réglage du serveur `ServerSettings.declared_identity` est activé (désactivé par défaut). Les
  anciennes lignes peuvent contenir `D:poste local` ou `D:anonyme`, qui ne sont plus produits.

Raison : un historique ne doit jamais disparaître parce qu'un utilisateur est supprimé, et le serveur
peut, si l'administrateur l'autorise, accepter une opération sans badge (par exemple un réassort par
quelqu'un qui n'a pas encore de badge) tout en distinguant clairement ce qui est vérifié. `parse_identity()` renvoie le
nom à afficher.

## Contenu des QR codes

| Objet | Contenu | Exemple |
|---|---|---|
| Item | `<base>/item?id=IID` | `https://inventaire.example.org/item?id=serphy20271231000000A1` |
| Item, ancien format (toujours lu) | iid seul | `serphy20271231000000A1` |
| Item non périssable | date `00000000` dans l'iid | `garrot00000000000000A1` |
| Lot, étiquette publique | `<base>/verif?lot=ID` | `https://inventaire.example.org/verif?lot=sacpse00000001` |
| Lot, étiquette privée | `<base>/verif?lot=ID&key=CLÉ` | `…/verif?lot=sacpse00000001&key=a1B2…` |
| Badge | `<base>/badge?m=MATRICULE&key=CLÉ` | `…/badge?m=M0042&key=…` |
| Paquet fermé | `<base>/pack?id=ID` | `…/pack?id=0000002B` |
| Scellé | `<base>/seal?lot=ID&s=CODE` | `…/seal?lot=sacpse00000001&s=…` |
| Téléphone-douchette | `<base>/scanner?s=SESSION&k=CLÉ` | affiché à l'écran, jamais imprimé |

`<base>` est `QRPROTEC_PUBLIC_BASE_URL` (déduit de `QRPROTEC_DOMAIN` sur la borne). L'analyse côté
poste est dans `app/src/core/codes.cpp` (`parse_scan`), côté web dans `app.js` (`parseCode`). Les deux
ignorent le domaine.

### Format de l'iid

```
serphy 20271231 000000A1
└type┘ └péremp.┘ └compteur┘
  6       8         8        = 22 caractères
```

- Le **type** permet de savoir de quoi il s'agit sans réseau.
- La **date** permet au poste de signaler un périmé **avant même la réponse du serveur** (bip et LED
  orange immédiats). `00000000` = non périssable.
- Le **compteur** est en base 62 (`0-9A-Za-z`, `inventory/base62.py`), propre à chaque couple
  (type, date) : 62⁸ ≈ 2·10¹⁴ valeurs, inépuisable.

Conséquence assumée : la date d'un item est **gravée dans son identifiant**. On ne corrige pas une
date de péremption ; on supprime l'item (avec raison) et on en reçoit un nouveau.

### Clés

- Clés de lot et de badge : 24 caractères tirés de `[0-9A-Za-z]` avec `secrets` (≈ 143 bits).
  Comparées en temps constant (`hmac.compare_digest`). Elles ont une date d'expiration.
- Code de scellé et code d'ouverture (`seal_open_code`, étiquette rangée dans le lot) : 16 caractères,
  changés à chaque scellage ; les étiquettes d'un ancien scellé deviennent invalides.
- Les clés ne sont jamais renvoyées par l'API publique (`lot_dict(local=False)`).

## Migrations

Dans `inventory/migrations/`. Elles sont appliquées automatiquement au démarrage du service
(`ExecStartPre=qrprotec-manage migrate`). Deux branches développées en parallèle ont produit deux
`0006` réunies par `0007_merge_sub_lots_web_push` : c'est normal avec Django, ne pas les renuméroter.
`0003_roles` transforme l'ancien booléen « responsable » en rôle `admin`. `0008_front_keys` ajoute
`FrontKey`.
