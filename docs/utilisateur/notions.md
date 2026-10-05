# Les notions

## Item

Un **item** est un objet physique : une compresse, un flacon de sérum physiologique, un garrot, une
couverture de survie. Chaque item porte **sa propre étiquette** avec un QR code unique, par exemple
`serphy20271231000000A1`. Ce code contient :

- le **type** (`serphy` = sérum physiologique) ;
- la **date de péremption** (`20271231` = 31/12/2027, ou `00000000` si l'objet ne périme pas) ;
- un numéro.

Comme la date est dans le code, le poste sait qu'un item est périmé **dès qu'on le scanne** : il bipe
et clignote en rouge.

## Type d'item

Le **type** regroupe les items identiques : « Sérum phy 10 ml », « Compresses stériles 10×10 ».
Chaque type a un **code de 6 lettres ou chiffres** (choisi à la création, définitif), un nom, un
**stock minimum** (en dessous, une alerte est envoyée), et indique s'il est **périssable**.

## Lot

Un **lot** est un contenant dont on connaît le contenu attendu : un sac PSE, un sac d'oxygénothérapie,
une malle, un lot DSA. Chaque lot a un **type de lot** (« Sac PSE ») qui définit ce qu'il doit
contenir (« 10 compresses, 4 sérums phy, 1 garrot… »), éventuellement avec l'**emplacement** dans le
sac (« pochette bleue »).

Chaque lot a **deux étiquettes** :

| Étiquette | Où la mettre | Ce qu'elle permet |
|---|---|---|
| **Publique** | collée à l'extérieur du sac | voir l'état du lot et son contenu |
| **Privée** | à l'intérieur du sac, ou gardée par les responsables | **vérifier** le lot et y **ajouter** du matériel |

Quelqu'un qui photographie l'étiquette publique peut voir l'état du sac, mais pas modifier
l'inventaire. Si une étiquette privée est perdue, un responsable régénère la clé : l'ancienne ne
marche plus et on en imprime une nouvelle.

## Sous-lot et lot global

Un lot peut être rangé dans un autre. Exemple :

```
VPS 1
├── Armoire de la cellule
├── B+ 1
│   ├── Sac de soin
│   └── Sac O2
└── Lot DSA
```

Le **B+ 1** est le **lot global** du sac de soin et du sac O2. Chaque sous-lot a ses propres
étiquettes et se vérifie seul. Le lot global est vert seulement si **tous** ses sous-lots le sont,
et sa date de dernière vérif est celle du sous-lot vérifié **le plus anciennement**. L'étiquette
privée d'un lot global permet de vérifier tous ses sous-lots.

Un lot qui ne contient rien lui-même (le « VPS 1 », le « B+ 1 ») sert juste à regrouper.

## Stock et rangements

Le **stock** est tout ce qui n'est dans aucun lot : la réserve. On peut organiser la réserve en
**rangements** (armoire, tiroir, étagère) : ce sont des lots d'un type marqué « rangement ». Le
matériel qui y est rangé **compte dans le stock**, et on peut vérifier un tiroir comme un sac, sans
vérifier toute la réserve. Chaque rangement est unique (un type par armoire, par tiroir), n'a pas de
contenu attendu et son étiquette privée n'expire pas. Les listes de lots ne les montrent qu'avec la
case **Afficher les rangements du stock**.

## Vérif

Une **vérif** consiste à scanner **tout** le contenu d'un lot puis à valider. Le logiciel compare
alors ce qui a été scanné à ce qui est attendu et enregistre :

- les items présents (et ceux qui sont périmés) ;
- les items qui manquent ;
- les items périmés **remplacés** : si on scanne un sérum périmé et un sérum neuf, le périmé est
  considéré comme sorti du sac ;
- qui a fait la vérif, et quand.

Un item qui n'a pas été scanné n'est pas « perdu » tout de suite : il est signalé, et il devient
**disparu** s'il est périmé ou s'il n'est pas retrouvé pendant trois vérifs de suite.

## Réassort

**Ajouter** du matériel dans un sac sans le vérifier entièrement (au retour d'une intervention, par
exemple). Le sac passe en **orange** « vérif recommandée » jusqu'à la prochaine vérif complète, pour
que la personne suivante sache qu'il faut tout recompter.

## Scellé

Un lot complet peut être **scellé** avec un scellé numéroté. Tant que le scellé est intact, le lot
est considéré comme bon **sans vérif**, jusqu'à la première péremption de son contenu. Une étiquette
de scellé est imprimée ; la scanner indique si c'est bien le scellé en cours. Une **étiquette
d'ouverture** est rangée à l'intérieur : la scanner après l'ouverture marque le scellé comme ouvert.
Ouvrir le lot (vérif, ajout, retrait, ou ouverture d'un de ses sous-lots) brise aussi le scellé.

## Paquet fermé

Une boîte qu'on n'ouvre pas tout de suite (boîte de 25 compresses) : à la réception, une seule
étiquette est imprimée pour la boîte, et les 25 étiquettes des compresses le sont à l'ouverture.
Scanner l'étiquette de la boîte compte pour les 25 compresses.

## Les couleurs

| Couleur | Signification |
|---|---|
| 🟢 **Vert** « Vérifié, complet » | Vérifié, rien ne manque, rien de périmé |
| 🟢 **Vert** « Scellé » | Scellé intact, rien de périmé |
| 🟠 **Orange** « Vérif recommandée » | Complet à la dernière vérif, mais du matériel a été ajouté depuis sans vérif |
| 🔴 **Rouge** | Jamais vérifié, incomplet, ou contient des périmés |

Dans l'état des stocks : barre **verte** au-dessus du minimum, **orange** en dessous, **rouge** à
zéro.

## Badge, PIN et rôles

Chaque utilisateur a un **badge** : une étiquette avec un QR code personnel (valable un an). On le
scanne pour s'identifier. Les administrateurs ont en plus un **code PIN** (4 à 8 chiffres) ; les
autres peuvent en avoir un.

| Rôle | Ce qu'il peut faire |
|---|---|
| **Secouriste** | Vérifier des lots, faire des réassorts, voir la liste des lots |
| **Gestion** | + gérer le stock (réception, paquets), les lots et leurs étiquettes ; voir l'état des stocks sur le téléphone |
| **Admin** | + gérer les utilisateurs, les réglages du poste, les modèles d'étiquettes, les notifications |

Sur le poste, quand un utilisateur gestion ou admin est connecté, l'écran devient **orange** (mode
privilégié). Après 15 minutes sans activité, le poste se déconnecte et revient à son état de départ.
