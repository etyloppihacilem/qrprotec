# Vérifier un lot

Une vérif, c'est : **scanner l'étiquette du lot (publique ou clé), scanner tout son contenu, valider
avec son badge.** Le reste de cette page détaille les cas particuliers.

> **La règle d'or : tout scanner.** Ce qui n'est pas scanné est considéré comme absent. Scannez aussi
> les périmés que vous retirez **et** leurs remplaçants : le logiciel comprend tout seul que le neuf
> remplace le périmé.

## Au poste

1. **Scannez l'étiquette** du sac avec la douchette (l'étiquette clé lance la vérif directement). La fenêtre **Vérif** s'ouvre : en haut
   l'état du lot (et de son lot global s'il en a un), puis la liste **À scanner** : chaque produit
   attendu, avec « scannés / attendus » et l'emplacement dans le sac. Les produits en cours de scan
   (orange) sont en haut, le dernier scanné en premier ; puis ceux dont rien n'a été scanné (rouge) ;
   les produits complets (vert) descendent en bas de la liste.
   - Si vous scannez l'étiquette **publique**, la fiche du lot s'affiche avec **Lancer une vérif** :
     cette étiquette compte pour valider. Une vérif lancée sans avoir scanné d'étiquette (depuis la
     liste des lots, ou par le premier item scanné) demande d'en scanner une avant de valider.
2. **Scannez chaque objet.** Il apparaît dans la **pile de scans** (colonne de droite) et disparaît de
   la liste « À scanner ».
   - Un **périmé** fait biper la douchette (LED orange) et s'affiche en rouge « PÉRIMÉ ». Si le lot a
     déjà son compte de ce produit non périmé, le poste vous dit de le **retirer du lot et le jeter** :
     la vérif reste complète. Sinon il est à remplacer : jetez-le dès que son remplaçant (scanné lui
     aussi) est dans le lot.
   - Un **code inconnu** s'affiche en rouge.
   - Un objet scanné deux fois est ignoré (simple mention « déjà scanné »).
   - **Annuler le dernier scan** retire le dernier objet de la pile ; la croix ✕ retire une ligne.
3. **Valider la vérif.** Survoler le bouton vert rappelle ce qu'il fait ou ce qui manque pour valider.
   Si vous n'êtes pas connecté, le poste demande votre **badge** (et votre PIN
   si vous en avez un). Si le TECH l'a autorisé, vous pouvez aussi saisir votre nom et
   **Continuer sans badge** (il faut alors avoir scanné l'étiquette clé du lot). Sans badge, le bouton
   **Se connecter avec nom, prénom et PIN** de la pile de scans connecte aussi, si vous avez un code PIN.
   Le compte rendu s'affiche :
   - **✔ VÉRIF ENREGISTRÉE : LOT COMPLET** : tout va bien ;
   - **✘ INCOMPLET** : la liste de ce qui manque, des périmés restés dans le lot, des items attendus
     mais non scannés. Complétez depuis le stock et refaites une vérif, ou signalez-le à la
     logistique (elle a peut-être déjà reçu une notification).

La fenêtre **Lots** (menu Fenêtres) liste tous les lots avec leur état ; un clic ouvre la fiche, avec
**Lancer une vérif**.

Le voyant **● N lots à traiter** de la barre de menu compte les lots incomplets, jamais vérifiés, avec
des périmés ou en vérif recommandée (rouge s'il y en a un incomplet ou périmé, orange s'il ne reste
que des vérifs recommandées). Un clic ouvre la fenêtre Lots sur ces lots seulement.

### Utiliser le poste au clavier

Tout se fait sans souris :

| Touche | Effet |
|---|---|
| `F1` à `F4` | Pile de scans, Lots, Listes de matériel, Téléphone-douchette |
| `F5` à `F12` | fenêtres des menus Gestion (rôle gestion ou admin) puis Administration (admin), dans l'ordre des menus, sauf Paquet fermé ; Signalements en `F11`, Réglages toujours en `F12` |
| `Ctrl+1`, `Ctrl+2`… | choisir l'onglet d'une fenêtre à onglets |
| `Alt` + lettre soulignée | actionner le bouton (ex. `Alt+V` pour **Valider la vérif** si le V est souligné) |
| `Ctrl+F11` | plein écran : une seule fenêtre visible, sur tout l'écran |
| `Ctrl+Tab` | passer à la fenêtre suivante |
| `Ctrl+W` | fermer la fenêtre active |
| `Ctrl+Espace` | remettre les fenêtres en place (disposition par défaut, sort du plein écran) |
| `Super+L` | se déconnecter (touche Windows + L) |
| flèches, `Entrée`, `Espace` | se déplacer dans la fenêtre et valider l'élément choisi |

La touche d'une fenêtre déjà au premier plan la ferme. Les raccourcis sont rappelés dans les menus
**Fenêtres** et **Gestion**. Sur un clavier français, seul le `Alt` de gauche sert aux boutons : `Alt Gr`
tape toujours `@`, `#` ou `{`.

## Au téléphone

1. Ouvrez l'appareil photo et **scannez l'étiquette clé** du sac : la **fiche du lot** s'ouvre
   (état de la vérif, contenu, emplacement de l'étiquette clé). Touchez **Lancer une vérif** : le
   lecteur s'ouvre sur ce lot, étiquette clé déjà scannée (autorisez la caméra la première fois).
2. **Scannez votre badge** si ce n'est pas déjà fait (le téléphone s'en souvient). Saisissez votre PIN
   si demandé. Si le TECH l'a autorisé, vous pouvez à la place toucher **Non connecté** et
   indiquer votre nom.
3. **Scannez chaque objet.** Onglets en bas : **À scanner** (ce qui reste, par produit : en cours en
   haut, rien de scanné ensuite, complets en bas ; touchez un produit pour déplier les codes de ses
   items, les périmés en rouge),
   **Scannés** (avec « Annuler le dernier » et « Vider la liste »), **Lot** (exigences scannées /
   attendues). Un périmé ou un code inconnu fait clignoter l'écran en rouge, biper et vibrer.
4. **Valider la vérif.** Le compte rendu s'affiche, et le lot est désélectionné (comme avec
   **Changer de lot**) : il n'apparaît plus « en cours » dans l'état des lots.

La caméra occupe le tiers haut de l'écran. Le bouton **↘** la réduit en vignette dans un coin, comme
pendant un appel vidéo : la liste prend tout l'écran et le scan continue. Touchez la vignette pour
la remettre en haut.

La liste en cours est conservée si la page se recharge ou si le téléphone se met en veille. Après
**20 minutes sans scan**, une vérif en cours est oubliée au rechargement de la page ou à l'ouverture
d'une étiquette avec l'appareil photo : on ne retombe pas sur une vérif commencée une heure plus tôt.
Le menu **⋯** permet de saisir un code à la main, de changer de lot ou de se déconnecter.

### Fiches des lots et des items

Une étiquette scannée avec l'appareil photo du téléphone (hors du lecteur QRProtec) ouvre une fiche,
sans caméra :

- **étiquette publique d'un sac** : fiche du lot, avec son état, la date de la dernière vérif, son
  contenu (sous-lot par sous-lot) et l'emplacement de son étiquette clé. Le petit bouton
  **📄 Fiche PDF** en haut imprime la fiche d'inventaire. **Lancer une vérif** ouvre le lecteur.
- **étiquette clé** : la même fiche, étiquette clé déjà scannée.
- **étiquette d'un item** : produit, péremption, état et **dernier emplacement connu** (le lot où il
  est, ou celui qu'il a quitté s'il a été utilisé, sorti ou signalé disparu). **Lancer une vérif**
  lance la vérif de ce lot. Pendant une vérif, un bouton ajoute l'item aux scans en cours.

Dans une fiche, les lots et les items sont cliquables : touchez un sous-lot, le lot global ou un item
du contenu pour ouvrir sa fiche. Le bouton retour du téléphone revient à la fiche précédente.

### Page d'accueil

L'adresse du site (ou le scan de votre carte avec l'appareil photo) ouvre la **page d'accueil** :

- **Se connecter** (en haut à droite) ouvre un petit lecteur dans un coin : scannez votre badge.
- **Scanner** : l'écran de vérif, caméra prête à scanner. Le bouton 🏠 y ramène à l'accueil.
- **Suivi des vérifs** : les vérifs **en cours, en direct** (lot, qui vérifie, items scannés,
  reste à scanner) et les **dernières vérifs** enregistrées. Pratique pour le chef de poste qui veut
  confirmer que ses équipiers ont fait leur vérif. Badge demandé.
- **État des lots** : tous les lots et leur état ; toucher un lot l'ouvre.
- **Gestion de la TECH** (tâches, état des stocks, prévisions) : rôles gestion et admin (badge demandé, grisé pour les autres).
- ⚙ : réglages des notifications, seulement pour ceux qui peuvent en recevoir.

Une vérif commencée au téléphone apparaît dans le **Suivi des vérifs** dès le premier scan, et y
reste affichée quelques minutes une fois validée.

## Vérif d'un lot qui a des sous-lots : où va un item ?

La vérif d'un lot qui a des sous-lots se fait **en cascade**, un lot à la fois (voir « Vérif en
cascade » plus bas). Dans le lot où vous êtes :

- Un item **déjà rangé dans un des lots de la vérif** y reste : on l'a trouvé là.
- Un item **venu du stock** va dans le dernier lot dont vous avez scanné l'étiquette (publique ou
  clé), ou dans un de ses sous-lots qui en attend encore. Scannez donc l'étiquette du sous-lot devant
  lequel vous êtes avant d'y ranger des items du stock. Le sous-lot en cours est rappelé par 📍.
- Un item **d'un autre sous-lot** (ex : un item de l'armoire 2 pendant la vérif de l'armoire 1) est
  déplacé dans le lot vérifié. Si vous l'avez trouvé dans l'armoire 2, scannez l'étiquette
  (publique ou clé) de l'armoire 2 : elle s'ajoute à la vérif, et l'item y reste.
- Pour valider, il faut avoir scanné **l'étiquette (publique ou clé) d'au moins un lot de la vérif**,
  d'un de ses sous-lots ou du lot qui le contient : c'est ce qui montre que la vérif est bien celle
  de ce lot et pas d'un autre. L'étiquette du scellé d'un sous-lot compte aussi. L'étiquette clé
  n'est plus exigée quand on est identifié (badge, ou nom et PIN) ; elle ne l'est que sans badge,
  sous un nom déclaré, si le serveur le permet (une seule suffit alors, celle de n'importe quel lot
  de la vérif).
- Sans vérif en cours et sans rien dans la liste des scans, scanner l'étiquette d'un item ou d'un lot
  dans le scanner ouvre sa **fiche** en grand, comme le lien ouvert avec l'appareil photo du
  téléphone. **Lancer une vérif** y commence la vérif du lot (pour un item, celle du lot où il est
  rangé, avec l'item déjà compté comme scanné).

## Vérifier à plusieurs

Plusieurs personnes peuvent faire la même vérif en même temps, chacune avec son téléphone (badge
demandé pour rejoindre) :

- depuis le **Suivi des vérifs**, **👥 Rejoindre cette vérif** sur une vérif en cours ;
- ou automatiquement depuis le scanner : si quelqu'un vérifie déjà le même lot, ou un autre
  sous-lot du même lot parent, votre vérif rejoint la sienne (« 👥 Vérif commune avec … »). La vérif
  commune couvre les sous-lots que chacun vérifie, pas tout le lot parent.

Si vous rejoignez une vérif de plusieurs sous-lots sans avoir rien scanné, le bandeau demande où vous
êtes : choisissez le sous-lot, ou scannez son étiquette.

Les scans de chacun s'ajoutent à la vérif commune : tout le monde voit les mêmes items « À scanner »,
et l'onglet **Scannés** indique qui a scanné quoi. Un petit bandeau 👥 sous les informations liste
les participants (✔ : a validé sa partie). Pas besoin de rescanner l'étiquette du lot :
celle scannée par un autre participant suffit.

Tant que d'autres participants sont connectés, **Valider** ne valide que **votre partie** (« Valider
ma partie de la vérif » sous le bouton). Le dernier qui valide envoie la vérif : le compte rendu
s'affiche chez tout le monde et tous les participants sont enregistrés sur la vérif. Si les autres
ont fermé leur page, le compte rendu part dès que ceux qui restent ont tous validé. Scanner un item
après avoir validé sa partie la « dé-valide ». **Quitter** dans le bandeau reprend la vérif seul, sans être rejoint automatiquement ensuite.

La vérif est sur le serveur, partagée, dès qu'elle commence : premier item scanné, étiquette clé du
lot, ou **Lancer une vérif** depuis la fiche du lot. Une vérif dont tous les téléphones sont fermés
depuis 15 minutes est **abandonnée**, sauf si tout ce qui était attendu a été scanné ou si au moins
une personne a validé sa partie : elle est alors enregistrée telle quelle, au nom de ceux qui y ont
participé.

## Utiliser son téléphone comme douchette du poste

Pratique quand la douchette est déchargée, ou pour scanner dans le véhicule pendant que le poste
reste au local :

1. Au poste : menu **Douchette > Téléphone comme douchette…**, **Créer une session** : un QR code
   s'affiche.
2. Scannez-le avec l'appareil photo du téléphone : la page de scan s'ouvre.
3. Tout ce que le téléphone scanne arrive dans la pile du poste, comme avec la douchette. Les erreurs
   (périmé, code inconnu) font aussi clignoter et vibrer le téléphone.

Si le téléphone reste déconnecté plus de 5 minutes (réglable), la session se ferme : créez un nouveau
QR code.

## Items à tâches (étiquette de tâches)

Certains items ont des tâches à faire à chaque vérif (ex : DAE : voyant vert allumé, électrodes dans
leur date). Leur étiquette de tâches porte la liste. Quand vous la scannez (ou l'étiquette d'item du
même objet), la douchette clignote en **bleu** avec un bip différent, l'écran clignote en bleu et une
fenêtre s'ouvre avec le nom de l'item et ses tâches à cocher :

- cochez **toutes** les tâches puis **Valider** : l'item est compté comme vu ;
- **Annuler** (ou scanner autre chose) : l'item n'est **pas** compté. Rescannez-le pour reprendre
  ses tâches.

Au poste, le **TECH** peut scanner deux fois de suite l'étiquette de tâches pour valider toutes
les tâches d'un coup. Sur le téléphone, les tâches se cochent toujours une à une.

Item à tâches **périmé ou désactivé** (à remplacer) : sur le téléphone, sa fiche (péremption, ou
motif, date et auteur de la désactivation) s'affiche d'abord, puis **Voir les tâches** ouvre ses
tâches. Au poste, l'écran clignote en rouge, sans fenêtre de tâches : l'item compte comme un
périmé, à remplacer.

## Réassort : ajouter du matériel sans tout vérifier

Au retour d'intervention, quand on remet dans le sac ce qui a été utilisé :

- **Au poste** : scannez les objets ajoutés, puis l'**étiquette clé** du sac, puis **Ajouter au
  lot … (réassort)** dans la pile de scans. Pendant une vérif où vous n'avez scanné que des objets
  nouveaux, un bouton orange **Ajouter N item(s) au lot – réassort, sans vérif** fait la même chose.
- **Au téléphone** : scannez l'étiquette clé, les objets ajoutés, puis **Ajouter au lot
  (réassort)**.

Le reste du contenu du sac n'est pas touché. Le sac passe **orange** « vérif recommandée », avec le
nombre d'objets ajoutés, la date et votre nom, jusqu'à la prochaine vérif complète.

## Vérif en cascade : un lot qui contient des lots (VPS, armoires, B+)

On ne vérifie jamais tout un VPS d'un coup : on fait un lot, puis une armoire, puis le suivant. La
vérif suit donc l'arborescence des lots.

1. Lancez la vérif du **VPS** (étiquette clé, ou **Lancer une vérif** sur sa fiche). L'écran liste ses
   sous-lots : Armoire 1, Armoire 2, Lot accouchement… avec leur état actuel, et en dessous ce que le
   VPS contient directement.
2. Scannez l'étiquette (publique ou clé) de l'**Armoire 1**, ou touchez sa ligne : vous **entrez**
   dedans. Le chemin (VPS 1 › Armoire 1) est rappelé en haut. Scanner directement un item de l'armoire
   y entre aussi.
3. Dans l'armoire, scannez un **sac de soin** : vous entrez dans le sac, vous faites sa vérif, puis
   **Valider Sac de soin** : il est enregistré tout de suite et vous revenez dans l'Armoire 1, où il
   est en **vert** (rouge s'il manque quelque chose).
4. **Valider Armoire 1** : retour au VPS, l'armoire est verte. Passez à l'armoire 2, etc.
5. Les **lots scellés** : scannez l'étiquette de leur scellé, ils sont comptés vérifiés sans les ouvrir.
6. À la fin, au niveau du VPS : **Valider VPS 1 et terminer** (ou **Terminer la vérif** si le VPS ne
   contient que des lots). La fenêtre Vérif se ferme d'elle-même quand il n'y a pas de compte rendu à lire.

Chaque lot validé est une vérif à part, enregistrée dès sa validation : si vous vous arrêtez au milieu,
les armoires faites restent faites. Un lot où vous n'êtes pas entré n'est pas touché (pas de
« manquants » pour une armoire que vous n'avez pas ouverte). Un item d'un lot déjà validé pendant
cette vérif est refusé (« Déjà vérifié ») : rentrez dans le lot (touchez sa ligne) pour le refaire.
**← Retour** remonte d'un niveau sans valider.

**Validation automatique** : quand le lot où vous êtes est complet (plus rien d'attendu à scanner) et
que vous scannez un item, l'étiquette ou le scellé d'un autre lot (ex : un item de l'armoire 2 alors que
l'armoire 1 est complète), le lot complet est validé tout seul (« ✔ Armoire 1 complet : validé
automatiquement ») et vous entrez dans l'autre. Un lot incomplet n'est jamais validé tout seul.

**Quitter un lot incomplet** : si vous avez déjà scanné des items dans le lot où vous êtes et que vous
scannez l'étiquette ou un item d'un autre lot, une fenêtre « Passer à Armoire 2 ? » donne trois choix :

- **Valider Armoire 1 incomplet** : les items pas encore scannés passent manquants, puis vous entrez
  dans l'armoire 2 ;
- **Passer sans valider** : l'armoire 1 reste à faire, ses scans sont gardés pour quand vous y
  reviendrez ;
- **Rester dans Armoire 1** : l'item scanné compte dans l'armoire 1, où vous l'avez trouvé.

Si vous ne voyez pas la fenêtre et scannez autre chose, cela vaut « Passer sans valider ». Sans scan
dans le lot où vous êtes (ou si un autre secouriste y travaille, au téléphone), on passe à l'autre lot
sans question.

Lancer la vérif d'un sous-lot (l'armoire 1, ou un sac via son étiquette clé) ouvre la même vue,
directement dans ce sous-lot : on voit où il est dans le VPS.

**Kit à scellé obligatoire** (ex : kit accouchement) dont vous lancez la vérif : une fois validé
complet, la vérif s'arrête là, sur le compte rendu, sans revenir au VPS. Au poste, un compte gestion
ou admin y trouve le bouton **Sceller** pour poser le nouveau scellé tout de suite.

**Une seule étiquette clé** suffit pour tout le VPS : celle de n'importe quel lot de la cascade
(celle d'une armoire permet de valider le sac qu'elle contient, et inversement).

### À plusieurs dans le même VPS

Si Julien vérifie l'armoire 1 et Camille l'armoire 2, ils font **la même vérif du VPS** (vérif
commune, voir « Vérifier à plusieurs ») :

- chacun voit où est l'autre (📍 Armoire 1, 📍 Armoire 2) et l'armoire de l'autre apparaît « En cours
  · Julien » ;
- quand Julien valide l'armoire 1, elle passe « Validé par Julien » chez Camille aussi ;
- Camille n'a pas besoin de scanner une clé si Julien en a déjà scanné une ;
- une même personne n'apparaît qu'une fois, même si elle a la vérif ouverte sur deux téléphones ou deux
  onglets (ex : étiquette clé ouverte avec l'appareil photo, qui ouvre un nouvel onglet) ;
- **Terminer** (ou valider le VPS) fait sortir de la vérif commune ; les autres continuent. Quand le dernier
  participant encore connecté termine, la vérif passe « Terminée » dans le **Suivi des vérifs**, avec le
  nombre de lots validés.

### Vérif partielle

Si un lot est incomplet (il manque des masques O2), **Vérif partielle** dans ce lot ajoute les objets
scannés en réassort (il passe orange), sans déclarer manquant le reste de son contenu.

## Fiche papier du lot (PDF)

Pour vérifier un lot sur papier, ouvrez sa fiche au téléphone (scan d'une de ses étiquettes ou liste
**État des lots** de la page d'accueil) et touchez **📄 Fiche PDF** (aussi dans l'onglet **Lot** du
lecteur). La fiche, au
format A4, liste les types d'items attendus **triés par emplacement**, avec la quantité attendue et
des cases à remplir (quantité trouvée, péremption la plus proche, OK), puis « Vérifié le / Par /
Signature » et une zone de remarques. Les items non périssables ont la case péremption grisée.

Un lot global (un B+) donne **une page par sous-lot** : la première page liste les sous-lots et le
numéro de leur page. Depuis un sous-lot, **Fiche du lot global en PDF** imprime tout le lot global.
Un lot trop long continue sur une page « suite ». La fiche ne contient aucune clé : elle peut être
imprimée et laissée dans le lot.

## Marquer un lot comme utilisé

Vous avez pris du matériel dans un lot sans faire de vérif ? Sur la fiche du lot (téléphone, ou fenêtre
**Lots** du poste), **Marquer ce lot comme utilisé (demander une vérif)**. Le lot (et ses sous-lots) passe
en orange « Vérif recommandée, lot utilisé » pour les suivants, jusqu'à la prochaine vérif complète. Il faut
être connecté (badge, ou nom si le serveur l'accepte). Un lot scellé s'ouvre avec son étiquette d'ouverture,
pas avec ce bouton.

## Lot scellé

Scanner l'étiquette du **scellé** (avec l'appareil photo ou le scanner, hors vérif) affiche la fiche
du lot, sans bouton « Lancer une vérif » : un lot scellé ne se vérifie pas.

- **Scellé intact** (vert) : rien à faire, le lot est bon jusqu'à la date indiquée ;
- **Ancien scellé** (orange) : l'étiquette ne correspond plus au scellé en cours ; un nouveau scellé a
  été posé depuis ;
- **Scellé brisé** (rouge) : le lot a été ouvert, il doit être vérifié ; le bouton « Lancer une vérif »
  est alors proposé.

Pendant une vérif, l'étiquette du scellé d'un lot du même lot global le compte comme vérifié sans
l'ouvrir (voir « Lot scellé dans le lot vérifié »).

En ouvrant un lot scellé, **scannez l'étiquette d'ouverture** rangée à l'intérieur (« Scanner après
l'ouverture ») : le scellé est marqué ouvert, que vous soyez connecté ou non. Il n'y a pas de bouton
« ouvrir » : le scan suffit, rien à signer. Le lot passe en **orange** « Vérif recommandée, scellé ouvert » jusqu'à la
prochaine vérif complète.

Vérifier un lot scellé, y ajouter ou en retirer quelque chose, ou ouvrir un de ses sous-lots **brise
le scellé** (le poste prévient avant). Après la vérif, un responsable pourra le resceller.

## Signaler un problème

Un objet abîmé, un sac déchiré, un appareil qui ne marche plus : n'importe quel utilisateur connecté
peut le **signaler** au TECH, pendant une vérif ou en dehors.

- **Pendant une vérif** : touchez le **(!)** hachuré noir et jaune à côté de l'item (ou du type
  d'item) dans la liste ;
- **Hors vérif** : scannez l'étiquette de l'item ou du lot, puis **Signaler un problème** sur sa fiche.
  Au poste, le bouton est dans la fenêtre du lot et le (!) dans la pile de scans ;
- **Paquet fermé** : le (!) à côté du paquet scanné (téléphone et pile de scans), ou **Signaler un
  problème sur ce paquet** dans la fenêtre Paquet fermé du poste. Le signalement vaut pour tous ses items.

La connexion est demandée **avant** de rédiger. Il faut ensuite répondre à **« L'objet est-il toujours
utilisable ? »** et, si on veut, ajouter un commentaire :

- **Oui** : l'item reste compté normalement, la vérif n'est pas bloquée ;
- **Non** : l'item est marqué **INUTILISABLE** et doit être **remplacé**, comme un périmé, tant que le
  signalement est ouvert.

Le signalement part tout de suite, indépendamment de la vérif. Un item signalé est compté comme vu
pendant la vérif en cours et n'est **jamais marqué disparu** tant que son signalement est ouvert.

Scanner un item (ou un paquet) qui a un signalement en cours, quel qu'il soit, fait **biper** le
téléphone ou le poste (la douchette elle-même si elle est en mode SDK) et ouvre une **fenêtre** avec
les signalements et leurs commentaires.

Les signalements en cours se suivent en lecture seule sur le téléphone, tuile **Signalements** de la
page d'accueil (rouge : nouveau ; orange : pris en charge, objet inutilisable ; vert : pris en charge
et toujours utilisable, ou résolu).

## Comprendre le compte rendu

| Rubrique | Signification | Que faire |
|---|---|---|
| Présents | Scannés et rangés dans le lot | — |
| Périmés toujours dans le lot | Scannés, périmés, sans remplaçant | Les remplacer, refaire une vérif |
| Périmés considérés comme remplacés | Un neuf du même type a été scanné : le périmé est sorti du lot | Le jeter |
| Attendus mais non scannés | Le logiciel les croyait dans le lot | Les chercher ; sinon ils deviendront « disparus » |
| Retrouvés | Étaient signalés disparus, ils sont revenus | — |
| Codes inconnus ignorés | Étiquette illisible ou d'un autre système | Réimprimer l'étiquette (rôle gestion) |

## Lot scellé dans le lot vérifié

Pendant la vérif d'un lot global (un VPS), dans le lot qui le contient, scannez l'**étiquette du
scellé** d'un lot scellé qu'il contient (un kit accouchement) : le lot est compté comme vérifié, avec
ses sous-lots, sans l'ouvrir ni scanner son contenu, et son scellé est gardé. Un scellé brisé, l'étiquette
d'un ancien scellé ou un lot scellé contenant des périmés sont refusés : ouvrez le lot et scannez son
contenu.
