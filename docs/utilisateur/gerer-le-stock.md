# Gérer le stock

Cette page concerne les rôles **gestion** et **admin**. Scannez votre badge au poste : l'écran passe
en orange et le menu **Gestion** apparaît.

## Recevoir une commande

Exemple : la commande du mois arrive avec 200 compresses (8 boîtes de 25, péremption 03/2029),
40 sérums phy (péremption 31/12/2027) et 2 garrots.

Menu **Gestion > Inventaire**, onglet **Réception**, une ligne de commande à la fois :

1. **Type d'item** : tapez une partie du nom ou du code (« ser ph » trouve « Sérum phy 10 ml »,
   sans se soucier des accents), validez avec Entrée ou Tab. La description du type compte aussi :
   y noter le nom commercial d'un médicament générique (« Doliprane » pour « Paracétamol ») permet
   de le trouver avec le nom écrit sur la boîte. Ces résultats viennent après ceux du nom, avec un
   extrait de la description.
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
les items, puis l'étiquette clé du rangement, puis **Ajouter au lot** dans la pile (ou faites une
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

Sur le téléphone, la page **État des stocks** de l'accueil (après avoir scanné son badge gestion ou admin) affiche le même
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
  **Copier la liste** pour la coller dans un bon de commande. Quand la date limite est atteinte, les
  utilisateurs gestion et admin reçoivent l'alerte **commande à passer** (notification web).
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

Sur le téléphone, la page **État des stocks** de l'accueil, vue **Prévisions** : choix de l'horizon,
graphe des péremptions des six prochains mois, types à surveiller avec la commande suggérée, et
échanges à faire.

## Vérifier la réserve

- **Un rangement** (tiroir, armoire) : comme un sac. Scannez son étiquette clé, tout son contenu,
  validez. Les items déjà rangés sont listés comme attendus, par type.
- **Plusieurs rangements à la suite** : ils font tous partie du Stock. Scannez l'étiquette du premier
  rangement et son contenu, puis l'étiquette du suivant et son contenu, etc., et validez une seule
  fois. Les items venus d'ailleurs vont dans le dernier rangement scanné. En vérif partielle, un
  rangement n'est vérifié que si tous ses items ont été scannés.
- **Le stock non rangé** : scannez tout ce qui est en réserve hors rangement, puis **Vérif du stock**
  dans la pile de scans. Les items scannés sont déclarés en stock (ceux d'un rangement y restent) ;
  les items en stock non scannés sont signalés manquants.

## Sortir du matériel sans lot

Du matériel pris dans la réserve pour une intervention, sans le ranger dans un sac : scannez les
items **sans** scanner d'étiquette de lot. Le bouton de vérif devient **Sortir N item(s) du stock**
(au téléphone comme au poste, dans la pile de scans). Les items sortis ne comptent plus dans le stock :

- s'ils reviennent, il suffit de les scanner à une vérif (ou de les remettre en stock) ;
- sinon, ils sont comptés comme **utilisés** au bout d'un mois, ou à leur date de péremption si elle
  arrive avant.

La colonne **Sortis** de l'état des stocks compte les items en attente de retour.

## Listes de matériel

Une **liste de matériel** dit ce qu'il faut prendre pour une mission : des lots (un sac, une armoire),
du matériel avec sa quantité (« 2 garrots, 10 compresses ») et des **tâches** libres à cocher, qui ne
sont ni un lot ni un item (« vérifier le niveau d'eau dans le sous-sol »). Exemple : un chef de poste prépare
la liste, ses équipiers la vérifient en sortant le matériel. Tout le monde peut créer ses listes ;
chacun modifie les siennes, les rôles gestion et admin modifient celles de tous.

- **Préparer** : téléphone, accueil > **Listes de matériel** > **Nouvelle liste** ; poste, menu
  Fenêtres > **Listes de matériel** > **Nouvelle liste**. Donnez un nom, une remarque si besoin
  (rendez-vous, consignes), ajoutez les lots et le matériel : tapez quelques lettres de son nom
  (« armo 2 », « compr ») ou son code, puis choisissez dans les résultats.
- **Partager** : la liste a un lien et un QR code (bouton **Partager le lien** au téléphone, le
  bouton QR code à côté affiche le QR code et le lien ; **Copier le lien** au poste). Ouvrir le lien ou scanner le QR code ouvre la liste, connecté.
- **Vérifier en direct, à plusieurs** : **📷 Scanner** au téléphone, **Scanner vers cette liste** au
  poste (douchette ou téléphone-douchette). Scannez l'étiquette de chaque lot de la liste et le
  matériel pris dans le stock : chaque scan compte tout de suite pour tous ceux qui ont la liste
  ouverte. Un item doit venir du stock (pas d'un lot), être bon (ni périmé ni à remplacer) et ne
  pas être déjà pris pour une autre liste. Un scan en trop se retire (✕ ou **Retirer**). Les tâches
  se cochent à la main ; une tâche cochée le reste si l'on modifie la liste.
- **Valider** : la liste validée (ou **validée incomplète** : ce qui manque n'est pas pris) sort son
  matériel du stock, comme une sortie sans lot. Elle ne peut plus être vérifiée. Une liste de
  tâches seulement est **terminée** à la validation : rien n'est sorti, il n'y a pas de retour.
- **Au retour** : rouvrez la liste, **Retour : scanner**, et scannez ce qui rentre. Le matériel est
  remis en stock tout de suite ; un lot rentré passe en **vérif recommandée** (orange, « retour de
  liste ») jusqu'à sa prochaine vérif. Puis **Clôturer le retour** : les consommables pas rentrés
  sont comptés **utilisés**, le matériel non consommable pas rentré reste **sorti** (il rentrera
  quand on le scannera). Sans retour clos, la règle des sorties s'applique (consommable compté utilisé au bout d'un mois).

Pour une liste qui revient souvent (poste de secours du stade, match du dimanche), faites-en un
**modèle** : il ne se vérifie pas, **Utiliser ce modèle** en fait une nouvelle liste à vérifier.
**Dupliquer** refait une liste déjà sortie ou rentrée.

## Réviser le matériel

Le matériel non consommable dont le type a une révision (voir les types d'items dans la mise en
place) a une date de **prochaine révision**. Pour le réviser :

- **téléphone** : scannez l'étiquette de l'item, puis **🔧 Réviser** sur sa fiche ;
- **poste** : scannez l'item, puis **Réviser** sous sa ligne dans la pile de scans.

Connectez-vous si besoin, cochez toutes les tâches de la révision, vérifiez la date de prochaine
révision (par défaut dans la périodicité du type, modifiable) et enregistrez. La fiche de l'item
garde la dernière révision et qui l'a faite.

Les révisions s'anticipent comme les péremptions : la fiche d'un lot affiche sa **prochaine
révision** (en orange à moins de 30 jours ou en retard), la liste des lots du poste indique
« N à réviser », et les comptes gestion et admin reçoivent chaque matin le résumé des révisions en
retard ou de la semaine (notification « Résumé quotidien des révisions de matériel à faire »). En
retard, l'item porte « révision en retard » : il reste utilisable avec un avertissement, ou compte
comme à remplacer si son type ne permet pas de l'utiliser en retard.

## Remettre du matériel en stock

Un objet sorti d'un sac et remis en réserve (sac réformé, surplus) : scannez-le, puis **En stock**
dans la pile de scans.

## Les périmés

- Le poste signale un périmé **dès qu'il est scanné**.
- La fiche de chaque lot indique les items périmés et ceux qui périment dans les 30 jours ;
  l'état des stocks a une colonne « Bientôt périmés ».
- Chaque matin à 7 h 45, les utilisateurs gestion et admin abonnés reçoivent la liste des lots
  contenant des périmés (notification web « résumé quotidien des périmés »).
- Dans un sac, on remplace un périmé **pendant une vérif** : scanner le périmé et le neuf, le
  logiciel sort le périmé du sac.

## Rechercher un item, réimprimer une étiquette

Onglet **Items** de l'inventaire : recherche par type, filtre **En stock**. La fiche d'un item
indique son type, sa péremption, son emplacement, son statut et son dernier passage (qui, quand).
**Aperçu** montre son étiquette sans l'imprimer, **Réimprimer** la refait (étiquette abîmée). Depuis la pile de scans, **Réimprimer**
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
