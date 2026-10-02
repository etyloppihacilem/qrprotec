# Gérer le stock

Cette page concerne les rôles **gestion** et **admin**. Scannez votre badge au poste : l'écran passe
en orange et le menu **Gestion** apparaît.

## Recevoir une commande

Exemple : la commande du mois arrive avec 200 compresses (8 boîtes de 25, péremption 03/2029),
40 sérums phy (péremption 31/12/2027) et 2 garrots.

Menu **Gestion > Inventaire**, onglet **Réception**, une ligne de commande à la fois :

1. **Type d'item** : tapez une partie du nom ou du code (« ser ph » trouve « Sérum phy 10 ml »,
   sans se soucier des accents), validez avec Entrée ou Tab.
2. **Péremption** : tapez la date comme elle est écrite sur l'emballage : `31/12/2027`, `311227`,
   ou seulement le mois `03/2029` / `0329` (= dernier jour du mois). La date comprise s'affiche à
   côté. Pas de date pour un produit non périssable.
3. **Quantité**.
4. Pour les compresses en boîtes fermées : cochez **Paquet fermé** et faites **une réception par
   boîte** (quantité 25). Voir ci-dessous.
5. **Créer N item(s) et voir les étiquettes** : l'aperçu montre les étiquettes, **Imprimer** les
   imprime en série.
6. Collez les étiquettes, rangez le matériel.

Les items reçus sont **en stock**. Pour les ranger dans une armoire ou un tiroir étiqueté : scannez
les items, puis l'étiquette privée du rangement, puis **Ajouter au lot** dans la pile (ou faites une
vérif du rangement).

La section **Dernière réception** permet de réimprimer une étiquette ratée ou toute la série.

## Paquets fermés

Une boîte fermée reçoit **une seule étiquette** de paquet ; ses items existent déjà dans le stock.

- **Ouvrir une boîte** : scannez son étiquette (en mode gestion, la fiche du paquet s'ouvre), bouton
  **Ouvrir le paquet et imprimer les N étiquettes**, aperçu, impression. Collez les étiquettes sur
  les objets.
- Une boîte **ouverte par erreur** : **Refermer le paquet** dans sa fiche. Les étiquettes déjà
  imprimées restent valables.
- Rouvrir une boîte déjà ouverte réimprime ses étiquettes sans changer sa date d'ouverture.
- On peut mettre une **boîte fermée entière dans un sac** : scanner l'étiquette de la boîte compte
  pour tous ses items.
- L'onglet **Paquets fermés** de l'inventaire liste les boîtes en réserve.

## Consulter l'état des stocks

Menu **Gestion > État des stocks** : une barre par type d'item, verte au-dessus du minimum, orange en
dessous, rouge à zéro, avec « quantité / minimum » (ex : `32/100`). Colonnes : dans les lots,
périmés, bientôt périmés (30 jours), disparus. Options : **Sous le minimum uniquement**, **Compter
les lots** (inclure ce qui est dans les sacs).

Par défaut, le **stock** compte ce qui n'est dans aucun lot **et** ce qui est dans un rangement
(armoire, tiroir). Les items périmés ne comptent pas.

Sur le téléphone, l'onglet **Stock** (après avoir scanné son badge gestion ou admin) affiche le même
état en lecture seule, les types les plus critiques en premier. Pratique pour préparer une commande.

## Anticiper les péremptions et les commandes

La fenêtre **État des stocks** a quatre onglets :

- **Actuel** : l'état d'aujourd'hui (ci-dessus).
- **Projection** : le stock tel qu'il sera **aujourd'hui, à M+1, M+3 ou M+6**. Avec **Avec
  consommation**, chaque lieu utilise d'abord ses items les plus proches de leur date, au rythme mesuré
  sur les derniers mois ; sans, on ne voit que ce qui sera encore valide. Colonnes : péremptions d'ici
  là, **perdus** (items qui périmeront sans avoir été utilisés), consommation par mois, date de
  passage sous le minimum. Cliquez un type pour voir ses graphes (stock projeté face au minimum,
  péremptions par mois réparties entre stock, lots et scellés), ses dates de péremption et où sont les
  items, et la consommation de chaque lot.
- **Commandes** : les types qui passeront sous leur minimum, avec la quantité à commander (minimum +
  deux mois de consommation, arrondi au conditionnement) et la date limite pour la recevoir à temps.
  **Copier la liste** pour la coller dans un bon de commande.
- **Transferts** : les items qui périmeront là où ils sont (typiquement dans un scellé qui ne s'ouvre
  pas) et le lot où ils seraient utilisés à temps. Chaque ligne est un **échange** : sortir les items
  indiqués, les mettre dans le lot d'arrivée, et reprendre en échange les plus récents de ce lot, pour
  que les deux restent complets. Les déplacements se font comme d'habitude (scan puis **Ajouter au
  lot**) ; un échange fait disparaît de la liste. Pour un scellé, la date affichée est celle avant
  laquelle il faut l'ouvrir.

La **consommation** d'un lot est le nombre d'items qui en ont disparu (absents à une vérif) sur la
période choisie dans les **Réglages** (6 mois par défaut). Un item noté absent puis **retrouvé**
(vérif mal faite, item rangé ailleurs) ne compte plus comme utilisé dès qu'il est rescanné. Les items
manqués à la dernière vérif ne comptent pas dans le stock projeté tant qu'ils ne sont pas retrouvés.
Le délai de livraison des commandes (15 jours par défaut) est aussi dans les Réglages.

Sur le téléphone, le bouton **⤢ Prévisions** de l'onglet Stock passe en plein écran (la caméra est
masquée) : choix de l'horizon, graphe des péremptions des six prochains mois, types à surveiller avec
la commande suggérée, et échanges à faire. **⤡ Scanner** revient au scan.

## Vérifier la réserve

- **Un rangement** (tiroir, armoire) : comme un sac. Scannez son étiquette privée, tout son contenu,
  validez.
- **Le stock non rangé** : scannez tout ce qui est en réserve hors rangement, puis **Vérif du stock**
  dans la pile de scans. Les items scannés sont déclarés en stock (ceux d'un rangement y restent) ;
  les items en stock non scannés sont signalés manquants.

## Remettre du matériel en stock

Un objet sorti d'un sac et remis en réserve (sac réformé, surplus) : scannez-le, puis **En stock**
dans la pile de scans.

## Les périmés

- Le poste signale un périmé **dès qu'il est scanné**.
- La fiche de chaque lot indique les items périmés et ceux qui périment dans les 30 jours ;
  l'état des stocks a une colonne « Bientôt périmés ».
- Dans un sac, on remplace un périmé **pendant une vérif** : scanner le périmé et le neuf, le
  logiciel sort le périmé du sac.
- Un résumé quotidien des lots contenant des périmés peut être envoyé par SMS (voir
  [Administration](administration.md#notifications)).

## Rechercher un item, réimprimer une étiquette

Onglet **Items** de l'inventaire : recherche par type, filtre **En stock**. La fiche d'un item
indique son type, sa péremption, son emplacement, son statut et son dernier passage (qui, quand).
**Réimprimer** refait son étiquette (étiquette abîmée). Depuis la pile de scans, **Réimprimer**
refait l'étiquette de chaque item scanné.

## Supprimer un item (exceptionnel)

Les items ne se suppriment pas : un objet perdu devient « disparu » par les vérifs. Pour une casse
ou une erreur de saisie (mauvaise date à la réception) : onglet **Items**, sélectionner l'item,
**Actions avancées**, raison obligatoire, **Marquer comme supprimé**. **Restaurer l'item** annule.

Une **erreur de date** à la réception se corrige ainsi : supprimer les items concernés (raison
« erreur de date »), refaire la réception avec la bonne date, réimprimer les étiquettes.

## Les types d'items

Onglet **Types d'items** : nom, description, stock minimum, périssable, taille de paquet par défaut.
Le **code** ne change jamais (il est dans les étiquettes). Modifier le minimum met à jour les alertes
immédiatement.
