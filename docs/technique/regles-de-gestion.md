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

Tous les rangements sont dans **le Stock** (`Lots.stock()`, type `models.STOCK_LOT_TYPE`), lot global
créé par la migration 0024 : ni création ni archivage par les utilisateurs, toujours un rangement
unique. Un rangement a pour parent le Stock (par défaut) ou un autre rangement ; un lot ordinaire
n'est jamais rangé dans un rangement (`views.parse_parent`). Plusieurs rangements se vérifient donc
ensemble comme les sous-lots d'un lot global. Un rangement n'a pas de contenu attendu : pendant la
vérif, ses items connus sont ses attendus (par type dans les fronts), et en vérif partielle il n'est
complet que si tous ses items ont été scannés (`services._complete_lots`, `planSession`,
`App::plan_verif`).

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
3. **Remplacements** : pour chaque (lot, type), un périmé scanné (ou signalé inutilisable, ou
   désactivé) est marqué `replaced` et sort du lot, à jeter, dès que le lot a **son minimum d'items
   frais** de ce type parmi les scans, que ces frais viennent d'arriver ou y soient déjà
   (`services.discardable`). C'est le geste « je remplace le sérum périmé par un neuf », ou « il y a
   déjà un neuf, je jette le périmé » : les fronts le suggèrent au scan (« retirez-le du lot et
   jetez-le »). Sans assez de frais, le périmé **reste** dans le lot, à remplacer, et le lot est
   incomplet faute du minimum. Un item dont seule la révision est en retard n'est pas jeté : il n'est
   remplacé que par un frais nouvellement arrivé. Un lot est **complet** dès qu'il a le minimum
   d'items frais par type : un périmé en trop ne le rend pas incomplet (vérif et état du lot).
4. **Présents** : `status=active`, `missed_verifs=0`, `last_seen*` mis à jour, rangés dans leur lot.
   Un item `missing` scanné est réactivé.
5. **Non vus** : les items actifs ou disparus du périmètre qui n'ont pas été scannés voient
   `missed_verifs` augmenter. Ils passent `missing` (**disparu**) s'ils sont périmés, ou après
   `QRPROTEC_MISSING_AFTER_VERIFS` vérifs manquées (3 par défaut). Ils restent rattachés au lot :
   un item oublié une fois dans le fond du sac n'est pas perdu. Exception : un item à **étiquette à
   déchirer** (`ItemType.tear_off`, ex : sachet de sérums phy) non scanné passe `missing` tout de
   suite : l'étiquette est arrachée à l'ouverture, l'ensemble est utilisé (rapport `torn`). En vérif
   partielle, un tel item non scanné ne retient pas son lot (même règle dans `services._complete_lots`,
   `planSession` du front web et `App::plan_verif` du poste).
   Un item **non consommable** (`ItemType.consumable` faux, ex : lecteur de glycémie) non scanné
   dans un lot ordinaire vérifié en a été retiré : il passe **hors lot** (`status=loose`, sans lot,
   `missed_verifs=0`, mouvement `move` vers le stock, opération `remove`, rapport `loose`), sans
   compter comme utilisé ni manquant (`services.leaves_lot`). Hors lot, il n'est ni dans les lots ni
   dans le stock (`stock_status` le compte dans `loose`) et une vérif du stock qui ne le voit pas ne
   le signale pas. Il entre en stock quand il est scanné (vérif du stock ou d'un rangement) ou remis
   en stock à la main (`items/to-stock`, fenêtre Inventaire du poste). Il ne retient pas non plus son
   lot en vérif partielle. Dans un rangement du stock il manque comme les autres. Sorti du stock
   sans lot, il reste `out` jusqu'à son retour (`out_deadline` vaut `None`).
   **Révision** (`ItemType.revision_months` > 0, non consommable seulement : `has_revision`) : chaque
   item a `revision_due`, posée à la réception (`add_items`) et à l'activation sur le type, à
   aujourd'hui + la périodicité (`add_months`, dernier jour du mois s'il n'existe pas). Une révision
   (`POST items/<iid>/revision/`, utilisateur connecté, `services.revise_item`) exige toutes les
   `revision_tasks` cochées, enregistre un `ItemRevision` et l'opération `revision`, et pose la
   prochaine date (saisie, après aujourd'hui, ou par défaut aujourd'hui + la périodicité). En retard
   (`revision_due` < aujourd'hui, `Items.revision_late`) : avertissement si `revision_usable`, sinon
   l'item est à remplacer comme un périmé (`needs_replacement`, et `revision_blocked_q` dans les
   filtres du scellage, du stock et du stock bas). `_lot_core` donne `next_revision`,
   `revision_late_count` et `revision_soon_count` (30 jours) ; `check_alerts` envoie
   `revision_daily` (en retard ou dans les 7 jours, `services.revisions_due`).
   **Items à tâches** (`ItemType.tasks`, étiquette de tâches `<base>/task?id=IID`) : les fronts
   n'envoient l'item dans les scans de la vérif qu'une fois toutes ses tâches cochées et validées
   (web `openTasks`/`addItem`, poste `ScanEntry.tasks_pending`, hors de `ScanStack::iids`). Le
   serveur n'a donc pas de règle à part : un item dont les tâches ne sont pas faites est non vu.
   Item à tâches à remplacer (`expired` : périmé, désactivé, inutilisable) : le web montre sa fiche
   (`#replace-item`) avant les tâches ; le poste clignote en rouge sans fenêtre de tâches et
   l'envoie dans les scans comme un périmé.
6. **Exigences** (`requirements_status`) : seuls les items vus à la dernière vérif (ou ajoutés depuis,
   `missed_verifs == 0`) et non périmés comptent comme présents. Le lot est **complet** si chaque
   ligne du contenu attendu est atteinte et qu'il ne contient aucun périmé.
7. **Écriture** : une ligne `Verifs` par lot vérifié, une `VerifItem` par item, `last_verif*` du lot,
   effacement de « vérif recommandée ».
8. **Effets de bord** : scellés brisés (voir plus bas), notification web « vérif incomplète »
   (désactivée par défaut), contrôle des seuils de stock pour les types touchés.

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
clés et le contenu du sac O2, la vérif partielle valide le sac O2 seul.

## Le réassort

Ajouter des items à un lot **sans** vérifier tout le lot (`services.move_items`,
`POST /api/lots/<id>/add/`). Les autres items du lot ne sont pas touchés. Le lot passe **« vérif
recommandée »** (`verif_recommended`, orange) avec le nombre d'items, la date et l'auteur, jusqu'à la
prochaine vérif : la personne suivante sait qu'il faut tout recompter. Un lot incomplet reste rouge.
Ranger des items dans un **rangement** n'est pas un réassort (pas d'orange) : c'est l'usage normal
d'une armoire.

Remettre des items en stock (`POST /api/items/to-stock/`) utilise la même fonction avec `lot=None`.

## Sortie du stock sans lot

Du matériel pris dans la réserve sans passer par un lot (intervention, prêt) : on scanne les items
**sans** étiquette de lot, et le bouton de vérif devient **« Sortir N item(s) du stock »** (web et
pile de scans du poste). `services.mark_out`, `POST /api/items/out/` (même identité qu'une vérif) :

- seuls les items en stock (hors lot ou dans un rangement) sortent ; un item d'un lot est refusé
  (il faut scanner le lot), un item déjà sorti repart pour un nouveau délai ;
- l'item passe `out` (« sorti »), quitte le stock (`location=NULL`, plus compté dans le stock ni
  attendu par la vérif du stock), avec un mouvement `out` et une opération « Sortie du stock » ;
- s'il est scanné ensuite (vérif, réassort, retour en stock), il redevient `active` comme un disparu
  retrouvé ;
- sinon, `services.expire_out_items` (lancé par `check_alerts` chaque matin) le compte comme
  **utilisé** : `missing` avec un mouvement `used` (consommation des prévisions), opération « Sortie
  non revenue ». Délai : `QRPROTEC_OUT_DAYS` jours (30) après la sortie, ou sa péremption si elle tombe
  avant. Retrouvé plus tard, l'utilisation est annulée comme pour toute absence.

## Listes de matériel

`checklists.py`, `POST /api/checklists/…` (public, utilisateur connecté : badge ou session). Une
`Checklist` a des lignes (`ChecklistLine` : un lot actif, ou un type d'item non archivé et une
quantité, ou une tâche libre `task` cochée avec `done` / `done_by`, 100 lignes au plus) et des scans (`ChecklistScan`, phase `out` à la vérif, `back` au retour).

- **Droits** : tout utilisateur connecté lit et vérifie une liste (lien `<base>/liste?id=ID`), crée
  les siennes ; seul l'auteur, la gestion et les admins la modifient ou la suppriment. `show` de la
  liste : les siennes (« Mes listes »), les modèles (onglet Modèles, eux seuls), ou toutes
  les listes hors modèles (gestion, admin).
- **Statuts** : `draft` (à préparer, modifiable), `validated` (sortie), `returned` (rentrée). Un
  modèle (`is_template`) ne se vérifie pas : on le duplique (`duplicate`, nouvelle liste à
  l'utilisateur, `source` vers l'original). Une liste commencée ne devient pas un modèle.
- **Scan à la vérif** : l'étiquette d'un lot de la liste coche sa ligne ; un item compte dans la
  ligne de son type s'il est en stock (hors lot ou rangement), actif, pas à remplacer, pas déjà pris
  par une autre liste à préparer, et dans la quantité demandée. Baisser une quantité retire les
  scans en trop.
- **Tâches** : `check {line, done}` coche ou décoche une tâche tant que la liste est à préparer ; une
  tâche cochée compte comme une ligne faite et reste cochée quand les lignes sont remplacées (même texte).
- **Validation** : au moins un scan ou une tâche cochée ; incomplète, il faut `partial`. Sans aucun
  scan (que des tâches), la liste passe directement `returned` (« Terminée »). Les items scannés passent
  `out` (`services.mark_out`) ; opération `checklist` au journal.
- **Retour** : un item sorti avec la liste revient en stock tout de suite (`services.move_items`,
  `last_seen_while=list`). Un lot de la liste non scellé passe `verif_recommended` avec
  `back_from_list` (nom de la liste), effacé à sa prochaine vérif ou à l'ouverture de son scellé.
  `close-return` : les consommables non rentrés encore `out` passent `missing` avec un mouvement
  `used` ; le matériel non consommable reste `out` jusqu'à ce qu'on le revoie (il ne s'use pas).

## Lots et sous-lots

Un lot peut avoir un **lot parent** (`Lots.parent`). Exemples réels :

```
VPS 1 (regroupement, rien d'attendu)
├── Armoire cellule
├── B+ 1 (regroupement)
│   ├── Sac de soin
│   └── Sac O2
└── Lot DSA
```

Décisions (prises avec le responsable du projet, voir [decisions.md](decisions.md#sous-lots-comme-vrais-lots)) :

- Un sous-lot est un **vrai lot** : son propre type, ses propres étiquettes publique et clé, ses
  propres vérifs. On peut vérifier le sac O2 seul.
- La **clé d'un lot couvre ses sous-lots** : scanner l'étiquette clé du B+ autorise la vérif
  du sac de soin et du sac O2. Dans l'autre sens, **une seule étiquette clé d'un sous-lot
  suffit** pour valider la vérif de tout le lot global (le sac O2 pour le B+, un rangement pour
  le Stock) : le sous-lot est envoyé dans `lots` avec sa clé, et `verif_targets` accepte la
  vérif dès qu'une des clés est valide. Même règle dans les fronts (`keyMissing`,
  `App::verif_key_ok`). Cette clé n'est exigée que sous un nom déclaré (option du serveur) : avec
  un badge, `views.require_label` demande seulement l'étiquette (publique, clé ou du scellé) d'un
  des lots vérifiés, d'un de leurs sous-lots ou d'un lot qui les contient, scannée pendant la vérif
  (`labels`, ou celles des autres participants de la vérif à plusieurs). Même règle dans les fronts
  (`labelMissing`, `App::verif_label_missing`). Le réassort (`/add/`) suit la même règle.
- **Vérif groupée** (API) : `lots` peut contenir plusieurs lots du **même lot global** (`POST /api/verifs/` avec `lots: [{id, key}, …]`). Les attendus
  s'additionnent.
- **Vérif en cascade** : toute vérif d'un lot qui a un lot global charge la **racine** et se
  déplace dans l'arbre (`state.path`, `App::VerifSession::path`). Chaque lot est validé à part,
  aussitôt (`POST /api/verifs/` avec `lots: [{id}]`, `keys: [{id, key}]`, `exclude: [ids]`,
  `step: true`) :
  - `keys` : n'importe quelle clé valide d'un lot du **même lot global** autorise
    (`views.key_holders`, `verif_lots(holders=…)`), plus les clés des participants de la vérif
    commune ;
  - `exclude` : sous-lots **stricts** du lot validé laissés de côté (déjà validés dans cette cascade,
    pas ouverts, ou en cours chez un autre participant) ; `perform_verif(exclude=…)` les retire avec
    leurs descendants, rien n'y est déclaré manquant. Un id hors de l'arbre ou le lot validé lui-même
    : 400 ;
  - un scan appartient au niveau où il a été fait ; un item d'un sous-lot du niveau y fait entrer ;
    un item d'un lot déjà validé dans la cascade est refusé ; l'étiquette d'un scellé est envoyée
    tout de suite. Item, étiquette ou scellé d'un lot hors du niveau alors que le niveau est complet
    (rien d'attendu ne manque, clé connue, pas de lot scellé) : le niveau est validé automatiquement
    (`leaveCompleteLevels` / `App::leave_complete_level`), en remontant tant que le lot scanné n'est
    pas dedans, puis on y entre. Niveau incomplet où des scans ont été faits (et où personne d'autre
    n'est, au web) : popup « Passer à … ? » (`leaveLevelPopup` / `App::ask_leave`) : valider incomplet
    (`validateStep(false, true)` / `submit_step`), passer sans valider (le niveau reste à faire, ses
    scans gardés) ou rester (l'item compte dans le niveau). Popup ignorée : le scan suivant vaut
    « passer ». Niveau incomplet sans scan : on passe au lot scanné sans question.
  - Vérif lancée sur un sous-lot à scellé obligatoire (`started_in` / `state.startedIn`) : validé
    complet, la cascade se termine sur son compte rendu (raccourci « Sceller » au poste) au lieu de
    remonter au lot parent.
  - Suivi : les participants de même identité (même personne sur deux pages) sont regroupés
    (`LiveSession.people`) ; la validation de l'un vaut pour les autres.
  - Vérif commune : `step` garde la session live (`LiveHub.step`) : les lignes validées vont dans
    `team.done`, les entrées envoyées sont retirées, les autres reçoivent `{type: 'step'}`. Chaque
    participant publie son niveau (`level`). `leave` sort de la session sans la terminer ; une
    session en cascade n'est jamais enregistrée seule à l'abandon (tout est déjà envoyé).
    `LiveHub.leave_cascade` : quand le dernier participant **connecté** termine (`leave`, ou `finish: true` avec la
    validation du lot global), la session est close (`done` avec `lots` et `incomplete`, affichée « Terminée »),
    même si des participants déconnectés y restent ; sans lot validé elle est retirée. Abandonnée (`LOST_KEEP`)
    avec des lots validés, elle est close de la même façon.
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
une étiquette de scellé est imprimée (`seal?lot=…&s=CODE`) ; scannée hors vérif, elle ouvre la fiche du lot sans bouton de vérif tant que le lot est scellé. Tant que le scellé est intact, le lot est
**valide sans vérif** (vert « Scellé »), jusqu'à la première péremption de son contenu
(`valid_until`). Un lot scellé contenant des périmés est rouge.

- Sceller exige que le lot **et tous ses sous-lots** soient complets, sauf « Sceller quand même »
  (`force`).
- Le scellé est **brisé** (`break_seal`) par : une vérif du lot, un ajout ou un retrait d'items,
  l'ouverture d'un **sous-lot** (on ne peut pas ouvrir le sac O2 sans ouvrir le B+), ou
  explicitement « Briser le scellé ». Le code change : l'ancienne étiquette de scellé est reconnue
  comme `wrong`.
- Chaque ouverture envoie une notification web « scellé ouvert » avec la raison (une par opération).
- **Étiquette d'ouverture** (`unseal?lot=…&c=CODE`), imprimée avec celle du scellé et rangée dans le
  lot : son scan (`POST lots/<id>/seal-open/`, `services.open_seal_with_label`) brise le scellé (et
  ceux des lots parents), **sans clé ni utilisateur connecté**. Sans identité, l'ouverture est
  enregistrée `D:Anonyme (étiquette intérieure)` ; un nouvel appel identifié dans l'heure la signe
  (le front web rappelle la route après la connexion). Le code d'ouverture est gardé après
  l'ouverture et change au prochain scellage. Le lot, ses sous-lots et les lots parents ouverts avec
  lui passent en **vérif recommandée** (orange, état `opened` : `verif_recommended` sans réassort,
  `restocked_count` à 0) jusqu'à la prochaine vérif complète.
- **Scellé obligatoire** (`LotType.seal_required`, ex : kit accouchement) : un lot de ce type qui
  n'est pas scellé est toujours **rouge** (`serializers.lot_state`, `lotStatus` du front web,
  `lot_status` de l'application) : état `seal_opened` tant que `verif_recommended` est levé (scellé
  ouvert, réassort), puis `must_seal` (« doit être scellé ») après une vérif complète, jusqu'au
  prochain scellage. Jamais vérifié et incomplet gardent leurs états habituels.
- **Lot scellé pendant une vérif** : scanner l'étiquette du scellé d'un lot du même lot global pendant
  une vérif (front web, application, vérif à plusieurs) le compte comme vérifié avec ses sous-lots, sans
  l'ouvrir : `sealed` [{id, code}] de `POST /api/verifs/` (`views.sealed_targets` contrôle le code du
  scellé, qui tient lieu de clé). Son contenu n'est ni attendu ni signalé manquant, il ne reçoit pas
  d'items et son scellé est gardé (`services.perform_verif`, `intactLots` du front web,
  `App::verif_intact`). Si un de ses items est scanné, le lot a été ouvert : il est vérifié normalement.

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

## Prévisions de stock

Calculées par `inventory/forecast.py` à chaque appel (`GET /api/stock/forecast/`, et
`POST /api/stock/forecast/summary/` avec un badge gestion ou admin pour le web) :

- **Consommation** : premières absences (`ItemMovement` `used`) non annulées sur
  `QRPROTEC_FORECAST_HISTORY_MONTHS` mois (6), par lot ; le stock (hors lot et rangements) consomme ce
  que ses lots consomment plus ses propres sorties. Une absence est annulée si l'item est revu : une
  vérif mal faite ne fausse pas la consommation.
- **Simulation** par lieu, « premier périmé, premier utilisé » : à chaque utilisation prévue, l'item
  valide le plus ancien est pris ; un item qui atteint sa date sans être pris est **perdu**. Un item
  perdu dans un lot est remplacé depuis le stock. Les items non confirmés (manqués à la dernière vérif)
  sont exclus.
- **Commande** : quand le stock projeté passe sous le minimum, quantité = minimum + 2 mois de
  consommation − stock restant, arrondie à `default_pack_size`, à recevoir avant cette date
  (`QRPROTEC_ORDER_LEAD_DAYS`, 15 jours, ou le réglage du poste).
- **Échanges** : pour chaque item perdu, le lieu (lot non scellé ou stock) où il serait utilisé à temps,
  contre son item le plus récent, si l'échange réduit les pertes des deux lieux. Date limite : dernier
  jour où l'item peut arriver et être utilisé au moins deux semaines avant sa date.

## État d'un lot (couleurs)

`serializers.lot_state`, dans cet ordre :

| Code | Couleur | Condition |
|---|---|---|
| `sealed_expired` | rouge | scellé mais contient des périmés |
| `sealed` | vert | scellé intact |
| `never` | rouge | jamais vérifié |
| `incomplete` | rouge | contenu attendu non atteint, ou périmés |
| `recommended` | orange | complet mais réassorti depuis la dernière vérif |
| `list_back` | orange | complet, rentré avec une liste de matériel depuis la dernière vérif |
| `opened` | orange | complet, scellé ouvert par son étiquette d'ouverture depuis la dernière vérif |
| `verified` | vert | vérifié et complet |

## Utilisateurs, rôles et PIN

| Rôle | Poste | Téléphone |
|---|---|---|
| `normal` (Secouriste) | vérifs, pile de scans, réassort | vérifs, liste des lots |
| `gestion` | + mode privilégié : stocks, inventaire, paquets, gestion des lots | + État des stocks de l'accueil (lecture), notifications web (stock, étiquettes de lot) |
| `admin` | + Réglages, Utilisateurs, éditeur d'étiquettes | + notifications web (aussi PIN bloqué, badges) |

- Le **rôle dans l'équipe** (`fonction` : l'autorité, l'OPÉ, le TECH...) est un texte libre de
  32 caractères, `Secouriste` par défaut (aussi quand on le vide). Purement indicatif, il ne donne
  aucun droit : il s'affiche dans la liste des utilisateurs, sous « Connecté » sur le poste et sur le
  badge avec `{{fonction}}`.
- Le **badge** (QR avec matricule et clé) identifie. Il expire au bout d'un an ; le renouveler
  invalide l'ancien.
- Le **PIN** (4 à 8 chiffres) authentifie : obligatoire pour les admins. Pour les autres, selon
  `pin_mode` du secouriste : `required` (obligatoire), `disabled` (jamais de PIN, le PIN existant est
  supprimé ; refusé pour un admin) ou vide, le réglage du serveur `pin_required_default`
  (`/api/server-settings/`, facultatif par défaut). Un PIN existant est toujours demandé, sauf s'il est
  désactivé. Sans PIN alors qu'il est obligatoire, il est choisi à la connexion suivante
  (`pin_setup_required`). 5 erreurs : blocage 5 minutes.
- **Volume de la douchette** : propre à chaque secouriste (`scanner_volume`, 0, 2, 4 ou 8), renvoyé à
  la connexion (`scanner_volume` dans la réponse de `/api/auth/`, toujours rempli) et enregistré par
  `PATCH /api/me/scanner-volume/` (`{"volume": 4}`, vide : revient au défaut). Sans choix, et sans
  utilisateur connecté, le front applique `scanner_volume_default` (`/api/health/`, réglé par un admin
  dans `/api/server-settings/`, 4 par défaut). Les autres réglages de la douchette, mode
  inventaire compris, sont réservés aux admins ; appairage et connexion restent ouverts à tous.
- **Connexion sans badge** : `POST /api/auth/name/` (`nom`, `prenom`, `pin`, sans casse ni accents),
  pour un secouriste qui a un PIN non désactivé et un badge valide. Même réponse que `/api/auth/` ; le
  jeton `session` remplace alors la clé du badge dans les identifiants (`{"matricule", "session"}`,
  accepté partout où `{"matricule", "key"}` l'est). Un refus ne dit jamais si le nom existe
  (« Nom, prénom ou code PIN incorrect », même temps de réponse). Les échecs sont comptés à part du
  PIN du badge (`name_login_failures`), pour qu'un inconnu ne puisse pas bloquer le PIN de quelqu'un :
  pause de 15 min tous les 5 échecs, connexion par nom coupée après 20 jusqu'à une connexion avec le
  badge, et au plus 10 échecs par adresse et par quart d'heure (429).
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
- **PIN choisi par l'utilisateur** (réglages ⚙ du front web, tous les rôles) : `POST /api/me/pin/`
  (`user` : badge ou jeton de session, `current_pin` s'il a déjà un PIN, essais faux comptés comme
  à la connexion) : `pin` le choisit ou le change, `disable: true` le supprime. La suppression est
  refusée pour un admin et quand le TECH a rendu le PIN obligatoire (`pin_mode` `required`, ou PIN
  obligatoire par défaut sans exemption) : `pin_can_disable` dans la réponse de `api/auth/`. Le mode
  du PIN n'est pas modifié (le PIN redevient facultatif) ; la session reste ouverte.
- **Notification des admins** au blocage (50 erreurs ou code oublié) : notifications web (option
  `pin_blocked` de l'abonnement), avec le lien de déblocage. **Une seule par
  blocage** : `pin_reset_notified` est pris avant l'envoi et ne revient à `False` qu'à la
  réinitialisation du PIN, donc répéter les demandes ou les essais n'envoie rien de plus. S'il n'y
  avait aucun destinataire, la tentative suivante réessaie.
- Sur le téléphone, `POST /api/auth/` renvoie un **jeton de session** signé (12 h), lié à la fin de
  l'empreinte de la clé du badge : renouveler le badge invalide les sessions. Il remplace le PIN pour
  les lectures réservées (état des stocks).
- Première installation : tant qu'aucun admin n'a de badge valide (`GET /api/setup/`), le poste
  propose de créer le premier administrateur. Sur le serveur : `manage.py createadmin`.
