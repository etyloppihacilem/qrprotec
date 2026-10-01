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
| `ItemType` | Type d'item (« Sérum phy 10 ml ») | Clé primaire `type` : **code de 6 caractères** alphanumériques (`serphy`), qui commence chaque iid. `min_quantity` : seuil d'alerte du stock. `perissable` : date de péremption obligatoire. `default_pack_size` : taille proposée à la réception. `low_notified` / `empty_notified` : alerte déjà envoyée (voir [back.md](back.md#notifications)). |
| `ItemsPacks` | Groupe d'items d'un même type et d'une même date | Clé `type + AAAAMMJJ`. Sert de **préfixe d'iid** et porte le compteur `last_sequence`. La date de péremption est stockée ici, pas sur l'item. Ce n'est **pas** un paquet physique (voir `SealedPacks`). |
| `Items` | Un objet physique étiqueté | Clé `iid` (22 caractères). `location` : le lot qui le contient, `NULL` = en stock. `status` : `active`, `missing` (disparu), `replaced` (remplacé, sorti d'un lot), `deleted` (supprimé à la main, avec raison). `missed_verifs` : vérifs consécutives où il était attendu sans être scanné. `last_seen*` : dernier passage (qui, quand, pendant quoi). |
| `SealedPacks` | Paquet fermé (boîte de 25 compresses) | Identifiant base 62 sur 8 caractères (`Sequence 'sealed_pack'`). Les items existent en base dès la réception, avec `sealed_pack` renseigné ; leurs étiquettes ne sont imprimées qu'à l'ouverture (`opened`). |

### Lots

| Modèle | Rôle | Points importants |
|---|---|---|
| `LotType` | Modèle de lot (« Sac PSE ») | Clé `type` (6 caractères). `storage=True` : **rangement du stock** (armoire, tiroir) : ses items comptent dans le stock. `version` est incrémentée à chaque changement du contenu attendu. |
| `LotRequirements` | Contenu attendu d'un type de lot | (type de lot, type d'item) unique, `quantity`, `location` libre (« pochette bleue ») affichée pendant la vérif. |
| `Lots` | Un lot physique | Identifiant `type + compteur base 62 sur 8` (`sacpse00000001`). `verif_key` (24 caractères, valable `LOT_KEY_VALIDITY_DAYS`) : la clé de l'**étiquette privée**. `parent` : lot global (sous-lots, voir [règles](regles-de-gestion.md#lots-et-sous-lots)). Champs du scellé (`is_sealed`, `seal_code`, `seal_number`…), du réassort (`verif_recommended`, `restocked*`), de la dernière vérif (`last_verif*`). `active=False` : lot archivé. |
| `LotSequence` | Compteur d'identifiants par type de lot | Incrémenté sous verrou. |

### Vérifs

| Modèle | Rôle |
|---|---|
| `Verifs` | Une vérif d'un lot (ou du stock si `lot` est `NULL`) : qui, quand, complète ou non, compteurs. Une vérif groupée de plusieurs sous-lots crée **une ligne par lot vérifié**. |
| `VerifItem` | Une ligne par item concerné : `present`, `missing` ou `replaced`, et s'il était périmé. C'est l'historique complet de chaque item. |

### Utilisateurs

`Secouristes` : matricule (clé primaire, 1 à 16 caractères), nom, prénom, `role` (`normal`,
`gestion`, `admin`), `key` du badge (24 caractères, valable 365 jours, `renew_key()` en crée une
nouvelle et invalide l'ancien badge), PIN haché avec les hacheurs de mots de passe de Django
(`pin_hash`), compteur d'échecs et blocage temporaire.

Il n'y a **pas** de compte Django (`auth.User`) pour les secouristes : un badge n'est pas un mot de
passe, et la plupart des opérations n'ont besoin que d'une identité. L'admin Django
(`/admin/`, API locale) reste disponible pour le dépannage avec un superutilisateur Django classique.

### Notifications

`NotificationSettings` (une seule ligne, `pk=1`, `get()` la crée), `SmsRecipient` (identifiant et clé
Free Mobile par destinataire, jamais renvoyés par l'API), `PushKeys` (clés VAPID du serveur, une
ligne générée au premier usage), `PushSubscription` (abonnement d'un navigateur d'admin).

## Les identités dans la base

Les champs `*_by` (`last_seen_by`, `created_by`, `sealed_by`…) sont des chaînes, pas des clés
étrangères. Format défini dans `inventory/idendity.py` :

- `M:M0042` : utilisateur **vérifié** (badge scanné, ou matricule envoyé par le poste local) ;
- `D:Jean` : nom **déclaré** sans vérification (`"name"` dans la requête publique), ou
  `D:poste local` / `D:anonyme` à défaut.

Raison : un historique ne doit jamais disparaître parce qu'un utilisateur est supprimé, et l'API
publique doit pouvoir accepter une opération sans badge (par exemple un réassort par quelqu'un qui n'a
pas encore de badge) tout en distinguant clairement ce qui est vérifié. `parse_identity()` renvoie le
nom à afficher.

## Contenu des QR codes

| Objet | Contenu | Exemple |
|---|---|---|
| Item | iid seul | `serphy20271231000000A1` |
| Item non périssable | date `00000000` | `garrot00000000000000A1` |
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
- Code de scellé : 16 caractères, changé à chaque scellage ; l'étiquette d'un ancien scellé devient
  invalide.
- Les clés ne sont jamais renvoyées par l'API publique (`lot_dict(local=False)`).

## Migrations

Dans `inventory/migrations/`. Elles sont appliquées automatiquement au démarrage du service
(`ExecStartPre=qrprotec-manage migrate`). Deux branches développées en parallèle ont produit deux
`0006` réunies par `0007_merge_sub_lots_web_push` : c'est normal avec Django, ne pas les renuméroter.
`0003_roles` transforme l'ancien booléen « responsable » en rôle `admin`.
