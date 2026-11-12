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
4. **Rangement du stock** (facultatif) : choisissez l'armoire ou le tiroir où va toute la réception
   (« Armoire › Tiroir 2 »). Les 50 compresses y sont rangées d'un coup, sans les scanner. Le
   rangement proposé est le dernier où ce type a été rangé. Laissé sur **Stock (sans rangement)**,
   les items restent dans le stock en vrac.
5. Pour les compresses en boîtes fermées : cochez **Paquet fermé** et faites **une réception par
   boîte** (quantité 25). Voir ci-dessous.
6. **Créer N item(s) et voir les étiquettes** : l'aperçu montre les étiquettes, **Imprimer** les
   imprime en série.
7. Collez les étiquettes, rangez le matériel.

Les items reçus sont **en stock**. Pour ranger après coup toute une réception dans une armoire ou un
tiroir : onglet **Réceptions passées**, cliquez la réception, choisissez le rangement puis **Ranger
les N items dans …** (seuls les items encore dans le stock bougent, pas ceux déjà mis dans un lot).
Pour ranger quelques items : scannez-les, puis l'étiquette clé du rangement, puis **Ajouter au lot**
dans la pile (ou faites une vérif du rangement).

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

## Imprimer des étiquettes depuis le téléphone

Pour du matériel d'un type déjà connu qui arrive sans passer par le poste (réassort des pompiers, dons),
la gestion ou un administrateur peut demander les étiquettes depuis le téléphone : page **Gestion de
la TECH**, bouton **+ Étiquettes**. On choisit le type, le nombre d'étiquettes et, si le type est périssable, la
date de péremption, puis **Envoyer au poste**. Les demandes en attente sont listées dessous et peuvent
être annulées tant qu'elles ne sont pas imprimées.

Au poste, la fenêtre **Impressions en attente** s'ouvre d'elle-même, **même si personne n'est
connecté** : le secouriste qui rentre avec le matériel clique **Lancer l'impression** et colle lui-même
les étiquettes. Les items sont créés à ce moment-là, au stock, au nom de l'administrateur qui les a
demandés (réception dans le journal). **Plus tard** ferme la fenêtre : la demande reste en attente,
signalée en bleu dans la barre de menu, et dans **À faire** (De temps en temps). Les impressions des
dernières 24 heures restent dans la fenêtre avec un bouton **Réimprimer**. Une demande pas imprimée
en 24 heures disparaît.

Sur un **autre ordinateur** relié au serveur avec une clé de front (sans imprimante), **Imprimer**
devient **Envoyer au poste** : les étiquettes partent dans la même fenêtre du poste, qui les imprime
telles quelles. Si une imprimante est branchée sur cet ordinateur, cochez **Imprimante branchée sur
cet ordinateur** (Réglages, partie Imprimante).

## Compteur d'impressions et consommables

Menu **Gestion > Impressions et consommables** au poste et sur les autres ordinateurs (gestion et admin) :
nombre d'étiquettes imprimées **par mois** et **par catégorie d'étiquette** (taille et support :
thermique direct ou transfert thermique), avec le **total de l'année** et la **moyenne par mois**
(mois finis), pour suivre la consommation de bobines et de ruban et prévoir les commandes. Le choix
de l'année permet de remonter les années précédentes ; survoler une colonne montre les types
d'étiquettes des modèles qui utilisent cette catégorie.

Une étiquette compte **quand elle est envoyée à l'imprimante** : une impression en erreur, mise en
pause (mauvais rouleau chargé) ou annulée ne compte pas ; une réimpression ou un test de l'éditeur
compte, puisqu'il consomme une étiquette. Tous les ordinateurs qui impriment sont additionnés. Hors
connexion, l'ordinateur garde ses compteurs et les envoie au retour du réseau. Le compteur part de
zéro à la mise à jour : les étiquettes imprimées avant ne sont pas comptées.

En haut de la fenêtre, **Rouleau en place sur cet ordinateur** dit quel rouleau est dans l'imprimante.
Changez-le ici quand vous remettez un rouleau (par exemple les étiquettes consommables après une série
d'étiquettes durables) : pas d'avertissement. Quand une impression demande un autre type d'étiquette
que le rouleau en place, elle s'arrête et la fenêtre **Changez de rouleau** s'ouvre (même sans personne
de connecté) : elle dit quel rouleau mettre, puis **C'est fait** reprend l'impression. **Plus tard**
laisse l'impression en pause (en rouge dans la barre de menu).

Plus rapide : **Imprimer le QR d'un rouleau...** (à côté du rouleau en place) imprime un QR pour chaque
rouleau, sur le rouleau de votre choix. Collez-le sur l'imprimante ou sur la boîte des bobines :
scanné à la douchette d'un poste, il déclare ce rouleau en place (et reprend une impression qui
l'attendait), sans connexion. Le téléphone l'ignore.

**Consommables** : on ne suit pas chaque bobine. Chaque catégorie d'étiquette a un **stock estimé** :
le dernier inventaire (ou la première réception), plus les réceptions, moins les étiquettes imprimées
depuis le lendemain. Les rubans de transfert sont suivis en mètres : chaque étiquette de transfert
imprimée use sa hauteur + 3 mm de ruban (un rouleau de ruban fait donc plusieurs bobines). Le tableau
donne la consommation par mois (moyenne des 90 derniers jours), l'autonomie, le coût par mois et
depuis janvier. Sous le seuil d'alerte (60 jours par défaut, réglable), une tâche **Commander** apparaît
dans la Gestion de la TECH du poste (pas sur le téléphone).

- **Références** : ajoutez chaque produit acheté : bobines (taille, direct ou transfert, étiquettes
  par bobine, bobines par boîte) ou rubans (mètres par rouleau, rouleaux par boîte), avec le prix d'une
  boîte pour les coûts. Plusieurs fournisseurs d'une même taille remplissent le même stock.
- **Réception** : à l'arrivée d'une commande, la référence et le nombre de boîtes ou de bobines.
- **Inventaire** : de temps en temps, comptez ce qui reste de la catégorie (boîtes, bobines, et
  à peu près ce qui reste sur la bobine entamée) : le stock estimé repart de ce compte.
- Un mouvement saisi par erreur s'annule dans **Derniers mouvements**.

## Gestion de la TECH : la liste de tâches

Menu **Gestion > Gestion de la TECH** au poste, tuile **Gestion de la TECH** de l'accueil sur le téléphone
(badge gestion ou admin). Le premier onglet, **À faire**, liste tout ce qu'il y a à faire pour tenir le
stock, du plus important au moins important :

1. **Signalements** à prendre en charge ou en cours ;
2. **Lots à sceller** (scellé obligatoire, ouverts ou vérifiés sans scellé) ;
3. **Lots invalides** : incomplets (avec ce qui manque), avec des périmés, ou jamais vérifiés ;
4. **Vérifs recommandées** : lots réassortis, ouverts, rentrés d'une liste ou marqués utilisés ;
5. **Révisions du matériel** en retard ou dans les 30 jours ;
6. **Vérifs du stock** : du matériel est sorti du stock (à la main ou par une liste) depuis sa dernière vérif ;
7. **Commandes** et **déplacements** recommandés par les prévisions dans les 30 jours ;
8. **De temps en temps** : étiquettes clés et badges qui expirent, périmés restés dans le stock, listes de
   matériel sorties et pas rentrées, matériel hors lot à ranger, lots valides pas vérifiés depuis 90 jours
   (`QRPROTEC_ROUTINE_VERIF_DAYS`).

Chaque tâche dit quoi faire ; la toucher (téléphone) ou son bouton (poste) ouvre la fiche du lot, les
signalements, les prévisions ou la fenêtre où la traiter. Une tâche disparaît d'elle-même quand elle est faite.

**Commande passée.** Une commande suggérée a une case **Commandé** et sa quantité (modifiable) : cochez-la une
fois la commande passée. Elle quitte les tâches et attend sa livraison sous « Commandes passées, en attente de
livraison » (qui l'a cochée, quand, ce qui est déjà reçu). Elle se clôt toute seule quand cette quantité d'items
du type a été réceptionnée. Pas livrée au bout de 30 jours, elle redevient une tâche (relancer le fournisseur).
Décochez-la si elle ne viendra pas : la suggestion revient si elle tient toujours.

**Péremptions à venir.** Les péremptions des 30 jours ne sont pas des tâches : les secouristes remplacent les
items une fois périmés, pendant la vérif du lot rouge. Seul compte ce qu'elles demandent au TECH : une commande,
ou un déplacement pour utiliser plus tôt ce qui va périmer. Le bouton **Péremptions à venir**, tout en bas et
replié, les liste quand même (hors du compte des tâches), ce qui périme en premier en tête : jaune dans les
7 jours, orange dans les 2 jours.

## Consulter l'état des stocks

Menu **Gestion > Gestion de la TECH**, onglet **Actuel** : une barre par type d'item, verte au-dessus du minimum, orange en
dessous, rouge à zéro, avec « quantité / minimum » (ex : `32/100`). Colonnes : dans les lots,
périmés, bientôt périmés (30 jours), disparus. Options : **Sous le minimum uniquement**, **Compter
les lots** (inclure ce qui est dans les sacs).

Par défaut, le **stock** compte ce qui n'est dans aucun lot **et** ce qui est dans un rangement
(armoire, tiroir). Les items périmés ne comptent pas.

Sur le téléphone, la vue **État des stocks** de la page **Gestion de la TECH** (après avoir scanné son badge gestion ou admin) affiche le même
état en lecture seule, les types les plus critiques en premier. Pratique pour préparer une commande.

Pour les administrateurs, 5 appuis rapides sur **État des stocks** ouvrent l'**outil de debug des
codes** : le code de toutes les étiquettes (items, lots, scellés, paquets, listes ; jamais les badges),
à afficher en grand et à scanner à la douchette, un par un ou à la suite (tout le contenu d'un lot ou
d'un sous-lot). Le champ de recherche accepte un nom, un iid ou un code scanné.

## Anticiper les péremptions et les commandes

La fenêtre **Gestion de la TECH** a, après **À faire**, quatre onglets :

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

Sur le téléphone, la page **Gestion de la TECH** de l'accueil, vue **Prévisions** : choix de l'horizon,
graphe des péremptions des six prochains mois, types à surveiller avec la commande suggérée, et
échanges à faire. Dans le graphe, chaque barre compte les items qui périment ce mois-là (stock, lots,
scellés) ; la barre rouge **périmés (à jeter)** en montre la part qui ne sera pas utilisée à temps au
rythme actuel. L'objectif est de ne plus voir de rouge. Survolez ou touchez un mois pour le détail.

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

Une **liste de matériel** sert à demander à des secouristes de prendre du matériel précis pour un
poste en particulier. Elle dit ce qu'il faut emporter : des lots (un sac, une armoire),
du matériel avec sa quantité (« 2 garrots, 10 compresses ») et des **tâches** libres à cocher, qui ne
sont ni un lot ni un item (« prendre des bonbons »). Exemple : un chef de poste prépare
la liste, ses équipiers la vérifient en sortant le matériel.

- **Qui fait quoi** : les rôles **chef**, gestion et admin créent des listes, voient toutes les listes
  et tous les modèles (partagés entre eux). Un chef modifie ses listes et tous les modèles ; la
  gestion et les admins modifient tout. Un **secouriste** ne crée pas de liste et ne voit que celles
  qui lui sont attribuées ; il peut quand même vérifier une liste dont il a le lien, et la partager.
- **Attribuer** : sur la liste (pas sur un modèle), partie **Secouristes**, tapez le début d'un
  prénom et choisissez dans les résultats (prénom seul sur le téléphone, avec le début du nom en cas
  d'homonyme). La liste apparaît dans **Listes de matériel** chez ces secouristes. Un secouriste qui
  ouvre le lien ou scanne le QR code de la liste se l'ajoute aussi.

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
  se cochent à la main ; une tâche cochée le reste si l'on modifie la liste. **Un lot se coche aussi
  sans scanner** (au téléphone) : touchez sa case, puis « Cocher sans scanner » (le VPS resté dehors
  pendant qu'on prépare le matériel) ; pour un lot au choix, la case propose les lots disponibles.
- **Prêt au départ** (valider) : une fenêtre récapitule ce qui manque (**Partir sans tout** : ce qui
  manque n'est pas pris). Le matériel scanné sort du stock, comme une sortie sans lot, et **les lots pris passent en
  vérif recommandée** (orange, « sorti avec une liste ») jusqu'à leur prochaine vérif, sauf un lot
  scellé. La liste est alors **terminée** : pas de retour à scanner. Le matériel sorti rentre en
  stock quand on le rescanne (vérif du stock) ; un consommable jamais revu est compté utilisé au
  bout d'un mois (règle des sorties).
- **Rouvrir le départ** : validée par erreur, la liste se rouvre pendant **30 minutes** (auteur ou
  secouriste de la liste). Le matériel sorti revient où il était, les lots pris ne sont plus en
  vérif recommandée à cause de cette liste, et la liste redevient à préparer avec ce qui est coché.

Pour une liste qui revient souvent (poste de secours du stade, match du dimanche), faites-en un
**modèle** : il ne se vérifie pas, **Utiliser ce modèle** en fait une nouvelle liste à vérifier.
**Dupliquer** refait une liste déjà sortie ou rentrée.

**Ajouter à la liste** : un seul champ de recherche propose tout ce qui se scanne. Chaque résultat
porte son genre : **Lot** (ce lot-là précisément), **Lot au choix** (un type de lot, n'importe
lequel) ou **Matériel** (un type d'item, avec sa quantité). Les tâches s'ajoutent à part.

**Lot au choix** : une ligne peut demander « 1 B+ » sans dire lequel (résultat **Lot au choix** de
la recherche). On la coche en scannant l'étiquette de n'importe quel lot de ce type, sauf un lot rangé
pour de bon dans un autre (une place fixe des lots exigés : on ne part pas avec le B+ du VPS) ou déjà
pris par une autre liste pas encore rentrée. La ligne montre les lots disponibles, les valides en
premier ; un lot pris qui n'est pas valide est signalé au scan.

### Partir avec un lot

Tout le monde, secouristes compris : **Listes de matériel** > **🚑 Partir avec un lot** (téléphone) ou
**Partir avec un lot** (poste), puis le type de lot (« Lot C », « B+ »…). Le serveur crée la liste de
départ, attribuée à celui qui part :

- s'il existe des lots de ce type (un lot C scellé prêt au départ, des B+), une ligne « 1 lot au
  choix » : on prend le lot entier, avec ses sous-lots ;
- sinon, le contenu du type : ses lots exigés (lots au choix), ses rangements s'ils portent sa liste,
  ou les items de sa liste (les items à cocher deviennent des tâches).

Scannez l'étiquette de chaque lot pris, puis **Valider** : le lot est noté parti avec la liste et
n'est plus proposé ailleurs. Par défaut, pas de vérif au départ : on fait confiance aux secouristes,
et au retour (**Retour : scanner**) chaque lot rentré passe en **vérif recommandée**. Le réglage
**Vérif complète à chaque départ** (poste, Réglages > Règles du serveur) demande au contraire une vérif
complète de chaque lot non scellé pris, après l'avoir scanné et avant de valider. Jamais pour un lot
scellé.

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
  logiciel sort le périmé du sac. Si le sac a déjà assez de non périmés, le périmé scanné est sorti
  du sac lui aussi (à jeter) et la vérif reste complète. Un périmé scanné qui n'était pas dans le
  sac n'y est jamais rangé : il est à jeter. Ces périmés à jeter ne sont pas affichés en rouge dans
  le suivi des vérifs ni dans le compte rendu.

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
