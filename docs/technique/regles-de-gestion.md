# Règles de gestion

Ce document décrit ce que fait le logiciel, du point de vue des données, quand on vérifie, réassortit,
scelle ou reçoit du matériel. Le code de référence est `database/inventory/services.py` ; l'état
affiché d'un lot est calculé dans `database/inventory/serializers.py`.

> **Trois implémentations à garder synchrones.** La répartition des items scannés entre plusieurs
> lots et le calcul « quels lots seraient complets » existent :
> - côté serveur : `services.assign_items` et `services._complete_lots` (font foi) ;
> - côté poste : `App::plan_verif` et `App::partial_verif_lots` (`app/src/app/app.cpp`) ;
> - côté téléphone : `planSession` et `partialLots` (`database/inventory/web/app.js`).
>
> Les fronts ne s'en servent que pour l'aperçu et pour proposer les bons boutons, mais un écart
> donnerait un aperçu faux. Toute modification de ces règles se fait aux trois endroits.
>
> De même, l'état d'un lot (`lot_state`, `aggregate_tree`) est calculé par le serveur et renvoyé dans
> `state` / `effective` ; les fronts l'affichent sans le recalculer.

## Stock et lots

Un item actif est **dans un lot** (`location` renseigné) ou **en stock** (`location` vide).
Exception : un lot dont le type est un **rangement** (`LotType.storage`, une armoire, un tiroir)
compte comme du stock. `services.in_stock_q()` est le seul filtre à utiliser pour « en stock ».

L'état des stocks (`services.stock_status`) compte, par type : stock frais, stock périmé, en lots
(frais / périmés), périmant sous 30 jours, disparus. Le seuil d'alerte compare le **stock frais**
(hors lots, non périmé) à `ItemType.min_quantity`.

## La vérif

Une vérif répond à « qu'y a-t-il vraiment dans ce lot ? ». L'utilisateur scanne **tout** le contenu
puis valide. `services.perform_verif(lot, iids, identity, partial)` :

1. **Périmètre** : le lot demandé et tous ses sous-lots actifs (`verif_lots`). Plusieurs lots peuvent
   être demandés ensemble s'ils ont le même lot global (sinon erreur, contrôlé par
   `views.verif_targets`).
2. **Destination de chaque item scanné** (`assign_items`), voir plus bas.
3. **Remplacements** : pour chaque (lot, type), si des items frais **nouvellement arrivés** dans le
   lot et des items périmés du même type sont scannés, autant de périmés que de nouveaux frais sont
   marqués `replaced` et sortent du lot. C'est le geste « je remplace le sérum périmé par un neuf » :
   on scanne les deux, le logiciel comprend. Un périmé scanné sans remplaçant **reste** dans le lot
   (et le lot est incomplet).
4. **Présents** : `status=active`, `missed_verifs=0`, `last_seen*` mis à jour, rangés dans leur lot.
   Un item `missing` scanné est réactivé.
5. **Non vus** : les items actifs ou disparus du périmètre qui n'ont pas été scannés voient
   `missed_verifs` augmenter. Ils passent `missing` (**disparu**) s'ils sont périmés, ou après
   `QRPROTEC_MISSING_AFTER_VERIFS` vérifs manquées (3 par défaut). Ils restent rattachés au lot :
   un item oublié une fois dans le fond du sac n'est pas perdu.
6. **Exigences** (`requirements_status`) : seuls les items vus à la dernière vérif (ou ajoutés depuis,
   `missed_verifs == 0`) et non périmés comptent comme présents. Le lot est **complet** si chaque
   ligne du contenu attendu est atteinte et qu'il ne contient aucun périmé.
7. **Écriture** : une ligne `Verifs` par lot vérifié, une `VerifItem` par item, `last_verif*` du lot,
   effacement de « vérif recommandée ».
8. **Effets de bord** : scellés brisés (voir plus bas), SMS « vérif incomplète », contrôle des seuils
   de stock pour les types touchés.

La **vérif du stock** (`lot=None`, `POST /api/stock/verif/`, poste en mode privilégié) suit la même
logique sur les items hors lots : un item scanné revient en stock, sauf s'il est dans un rangement
(il y reste). Les items rangés dans un rangement ne sont pas signalés manquants : on vérifie un
tiroir comme un lot.

### Répartition des items entre les lots

Quand une vérif couvre plusieurs lots (un lot et ses sous-lots, ou plusieurs sous-lots scannés
ensemble), chaque item scanné doit aller quelque part (`assign_items`) :

1. un item **déjà rangé** dans un des lots de la vérif y reste ;
2. un item **venu d'ailleurs** (stock, autre lot), en traitant les frais avant les périmés : dans le
   premier lot (ordre de l'arborescence) qui en attend encore (déficit > 0) ;
3. sinon dans le premier lot dont le contenu attendu contient ce type ;
4. sinon dans le premier lot de la vérif (le lot scanné).

Le « déficit » part du contenu attendu, diminué des items frais déjà rangés et rescannés.

### Vérif partielle

Si une vérif groupée est validée alors que certains lots sont incomplets, le bouton « Vérif
partielle » (`partial=true`) ne vérifie que les lots **que les items scannés rendent complets**
(`_complete_lots`). Un lot dont aucun item n'a été scanné n'est retenu que s'il est vide (sinon tout
son contenu serait déclaré manquant). Les items scannés destinés aux autres lots y sont **ajoutés en
réassort** (voir ci-dessous) sans toucher au reste de leur contenu.

Cas d'usage : on a refait le sac O2 du B+ mais pas le sac de soin ; on scanne les deux étiquettes
privées et le contenu du sac O2, la vérif partielle valide le sac O2 seul.

## Le réassort

Ajouter des items à un lot **sans** vérifier tout le lot (`services.move_items`,
`POST /api/lots/<id>/add/`). Les autres items du lot ne sont pas touchés. Le lot passe **« vérif
recommandée »** (`verif_recommended`, orange) avec le nombre d'items, la date et l'auteur, jusqu'à la
prochaine vérif : la personne suivante sait qu'il faut tout recompter. Un lot incomplet reste rouge.
Ranger des items dans un **rangement** n'est pas un réassort (pas d'orange) : c'est l'usage normal
d'une armoire.

Remettre des items en stock (`POST /api/items/to-stock/`) utilise la même fonction avec `lot=None`.

## Lots et sous-lots

Un lot peut avoir un **lot parent** (`Lots.parent`). Exemples réels :

```
VPS 1 (regroupement, rien d'attendu)
├── Armoire cellule (rangement)
├── B+ 1 (regroupement)
│   ├── Sac de soin
│   └── Sac O2
└── Lot DSA
```

Décisions (prises avec le responsable du projet, voir [decisions.md](decisions.md#sous-lots-comme-vrais-lots)) :

- Un sous-lot est un **vrai lot** : son propre type, ses propres étiquettes publique et privée, ses
  propres vérifs. On peut vérifier le sac O2 seul.
- La **clé privée d'un lot couvre ses sous-lots** : scanner l'étiquette privée du B+ autorise la vérif
  du sac de soin et du sac O2 (`verif_targets` accepte la clé d'un ancêtre).
- **Vérif groupée** : pendant une vérif, scanner l'étiquette privée d'un autre lot du **même lot
  global** l'ajoute à la vérif (`POST /api/verifs/` avec `lots: [{id, key}, …]`). Les attendus
  s'additionnent.
- **État global** (`serializers.aggregate_tree`) : le lot global prend le **pire état** de ses lots
  (vert < orange < rouge) ; sa dernière vérif est la **plus ancienne** des dernières vérifs de ses
  lots (aucune si l'un n'a jamais été vérifié).
- Un lot qui n'attend rien, ne contient rien et a des sous-lots est un **simple regroupement** : il
  ne compte pas dans l'état global.
- Un sous-lot d'un lot **scellé intact** est considéré valide (« Dans un lot scellé ») tant qu'il ne
  contient pas de périmés.
- Un cycle de parents est refusé (`views.parse_parent`) ; les parcours (`ancestors`,
  `descendants`) se protègent quand même contre les boucles.

Non fait (piste d'évolution) : déclarer au niveau du **type** de lot les sous-lots attendus (« un B+
contient un sac de soin et un sac O2 »). Aujourd'hui l'arborescence se construit lot par lot.

## Les scellés

Un lot complet peut être **scellé** (`services.seal_lot`) : un scellé physique numéroté est posé et
une étiquette de scellé est imprimée (`seal?lot=…&s=CODE`). Tant que le scellé est intact, le lot est
**valide sans vérif** (vert « Scellé »), jusqu'à la première péremption de son contenu
(`valid_until`). Un lot scellé contenant des périmés est rouge.

- Sceller exige que le lot **et tous ses sous-lots** soient complets, sauf « Sceller quand même »
  (`force`).
- Le scellé est **brisé** (`break_seal`) par : une vérif du lot, un ajout ou un retrait d'items,
  l'ouverture d'un **sous-lot** (on ne peut pas ouvrir le sac O2 sans ouvrir le B+), ou
  explicitement « Briser le scellé ». Le code change : l'ancienne étiquette de scellé est reconnue
  comme `wrong`.
- Chaque bris envoie un SMS « scellé brisé » (si activé) avec la raison.

## Paquets fermés

À la réception, on peut indiquer que les items arrivent dans un **paquet fermé** (une boîte de 25
compresses) : les 25 items sont créés tout de suite (le stock est juste), mais une seule étiquette
de paquet est imprimée. À l'ouverture (`POST /api/packs/<id>/open/`), les 25 étiquettes d'item sont
imprimées. Scanner l'étiquette du paquet dans une vérif ou un réassort compte pour tous ses items
(`ScanStack::iids()` développe le paquet). Un paquet ouvert par erreur se referme.

## Suppression

Les utilisateurs ne suppriment pas d'items : un item perdu devient « disparu » par les vérifs. Un
responsable peut exceptionnellement marquer un item **supprimé** (raison obligatoire) et le
restaurer. Rien n'est jamais effacé de la base.

## État d'un lot (couleurs)

`serializers.lot_state`, dans cet ordre :

| Code | Couleur | Condition |
|---|---|---|
| `sealed_expired` | rouge | scellé mais contient des périmés |
| `sealed` | vert | scellé intact |
| `never` | rouge | jamais vérifié |
| `incomplete` | rouge | contenu attendu non atteint, ou périmés |
| `recommended` | orange | complet mais réassorti depuis la dernière vérif |
| `verified` | vert | vérifié et complet |

## Utilisateurs, rôles et PIN

| Rôle | Poste | Téléphone |
|---|---|---|
| `normal` (Secouriste) | vérifs, pile de scans, réassort | vérifs, liste des lots |
| `gestion` | + mode privilégié : stocks, inventaire, paquets, gestion des lots | + onglet Stock (lecture), notifications web (stock, étiquettes de lot) |
| `admin` | + Réglages, Utilisateurs, éditeur d'étiquettes | + notifications web (aussi PIN bloqué, badges) |

- Le **badge** (QR avec matricule et clé) identifie. Il expire au bout d'un an ; le renouveler
  invalide l'ancien.
- Le **PIN** (4 à 8 chiffres) authentifie : obligatoire pour les admins, facultatif pour les autres.
  Un admin sans PIN le choisit à sa connexion suivante. 5 erreurs : blocage 5 minutes.
- **50 erreurs** depuis le dernier PIN correct (`QRPROTEC_PIN_BLOCK_AFTER_FAILURES`) : le PIN est
  bloqué jusqu'à ce qu'un admin le réinitialise, et les sessions ouvertes ne valent plus rien.
  `POST /api/auth/` répond alors `pin_blocked` avec `pin_reset` : URL `<base>/pinreset?m=…&t=…`
  (le jeton signe le matricule et l'heure du blocage, il ne sert qu'une fois) et nom de l'admin à
  contacter (`pin_contact`, choisi dans la fiche de l'utilisateur). Le téléphone affiche ce lien et son
  QR code ; l'admin le scanne, se connecte (badge + PIN) et confirme : `POST /api/pin-reset/`
  (`confirm: true`). Sur le poste : **Utilisateurs > Réinitialiser le PIN**. Après réinitialisation,
  l'utilisateur choisit un nouveau PIN à sa connexion suivante (`pin_reset_required`).
- **Code oublié** (lien discret sous la saisie du PIN, front web et poste) : `POST /api/pin-forgot/`
  (`matricule`, `key` du badge) bloque le PIN de la même façon (`pin_forgotten`) et renvoie la même
  réponse `pin_blocked`, pour le même parcours de déblocage. Refusé si l'utilisateur n'a pas de PIN.
- **Notification des admins** au blocage (50 erreurs ou code oublié) : SMS (événement `pin_blocked`)
  et notifications web (option de l'abonnement), avec le lien de déblocage. **Une seule par
  blocage** : `pin_reset_notified` est pris avant l'envoi et ne revient à `False` qu'à la
  réinitialisation du PIN, donc répéter les demandes ou les essais n'envoie rien de plus. S'il n'y
  avait aucun destinataire, la tentative suivante réessaie.
- Sur le téléphone, `POST /api/auth/` renvoie un **jeton de session** signé (12 h), lié à la fin de
  l'empreinte de la clé du badge : renouveler le badge invalide les sessions. Il remplace le PIN pour
  les lectures réservées (état des stocks).
- Première installation : tant qu'aucun admin n'a de badge valide (`GET /api/setup/`), le poste
  propose de créer le premier administrateur. Sur le serveur : `manage.py createadmin`.
