# L'imprimante d'étiquettes

QRProtec dessine chaque étiquette sur le poste et l'envoie à l'imprimante sous forme d'image. Toute
imprimante d'étiquettes **TSPL** (TSC, IDPRT, Munbyn, Phomemo…) ou **ZPL** (Zebra) convient, à
condition d'avoir une tête **203 dpi** (8 points par millimètre), la résolution des modèles
d'étiquettes. En 300 dpi, l'étiquette sortirait plus petite. La Niimbot B1 reste utilisable en secours.

Cette page prend l'exemple de la **Zebra GK420t**, une imprimante de bureau robuste qui se trouve
facilement d'occasion.

## Choisir la GK420t (et pas la GK420d)

| Modèle | Impression | Étiquettes |
|---|---|---|
| **GK420t** (« t » = transfert) | avec un ruban encreur, **ou** sans ruban (thermique direct) | toutes : papier thermique, vélin, polypropylène, PET |
| GK420d (« d » = direct) | sans ruban seulement | papier thermique seulement |

Vues de dos, les deux se ressemblent : regardez l'étiquette sous l'imprimante. Prenez la **t** si
vous voulez des étiquettes durables (lots, scellés, rangements, badges) en polyester avec un ruban
résine, qui tiennent à l'alcool, au frottement et au froid. Les étiquettes thermiques directes
(sans ruban) suffisent pour les consommables, qui ne restent pas longtemps dans les sacs.

À vérifier avant d'acheter d'occasion : demandez au vendeur une page de configuration (voir plus bas)
et une étiquette imprimée en noir plein. Une ligne blanche verticale indique un point mort sur la tête.

Ce qu'elle accepte :

- étiquettes de 19 à 108 mm de large, rouleau de 127 mm de diamètre au plus, mandrin de 12,7 à 38 mm ;
- ruban de 74 m au plus, **encrage extérieur**, mandrin de 12,7 mm, au moins aussi large que
  l'étiquette (57 mm pour des étiquettes de 40 à 50 mm) ;
- un rouleau trop gros peut rester dehors, sur un dérouleur, et entrer par la fente arrière.

## Brancher et régler

1. Branchez l'imprimante **en USB** (prise carrée à l'arrière). Les prises série et parallèle
   servent aux anciens ordinateurs : laissez-les vides. Le modèle avec prise réseau peut aussi
   se brancher sur le réseau.
2. Dans **Administration > Réglages > Étiquettes et impression**, choisissez le modèle
   **ZPL (Zebra)** et l'adresse `/dev/usb/lp0`. En réseau, mettez l'adresse IP de l'imprimante
   (port 9100). Sur le poste, l'utilisateur doit faire partie du groupe `lp`, ce que l'installation
   fait déjà.
3. Indiquez les étiquettes chargées : le **Type chargé** (voir [Administration](administration.md))
   ou, à défaut, la largeur et la hauteur du rouleau. Cochez **Transfert thermique (ruban)** si un
   ruban est en place.
4. Imprimante d'occasion ou déjà réglée pour autre chose : **Réglages d'usine** (voir ci-dessous).
5. Chargez le rouleau en suivant la [procédure de changement de rouleau](#changer-de-rouleau) : elle
   enchaîne calibrage, contrôle de l'état et étiquette d'étalonnage.

## Ce que le logiciel envoie à l'imprimante

Le poste dessine l'étiquette et l'envoie en image. Avec l'image, chaque impression rappelle à
l'imprimante le format complet, si bien qu'aucun réglage resté dans l'imprimante ne vient
s'ajouter. Chaque réglage passe par la commande que la documentation Zebra (guide de programmation
ZPL) ou TSC (TSPL) prévoit pour cet usage :

| Réglage du logiciel | ZPL (Zebra) | TSPL | Effet |
|---|---|---|---|
| Transfert thermique (ruban) | `^MTT` / `^MTD` | `SET RIBBON ON` / `OFF` | avec ou sans ruban |
| Étiquettes séparées par un espace | `^MNY` | `GAP` | l'imprimante se cale sur l'interstice entre deux étiquettes |
| Largeur de l'étiquette | `^PW` (largeur d'impression) | `SIZE` | zone imprimable = largeur de l'étiquette |
| Hauteur de l'étiquette | `^LL` (longueur d'étiquette) | `SIZE` | longueur d'une étiquette |
| Noirceur | `~SD` | `DENSITY` | chauffe de la tête |
| **Décalage horizontal** | `^LS` (« Left Position ») | `SHIFT x` | toute l'impression vers la droite (+) ou la gauche (−) |
| **Décalage vertical** | vers le bas : `^LH0,y` (« Label Home », origine du format) ; vers le haut : `^LT` (« Label Top ») | `SHIFT y` | toute l'impression vers le bas (+) ou le haut (−) |
| **Position de découpe** | `~TA` (« Tear Off ») | `OFFSET` | où l'étiquette s'arrête après impression : plus sortie (+) ou moins (−) ; l'impression ne bouge pas sur l'étiquette |

Les trois réglages de position vont de −15 à +15 mm, par demi-millimètre. En ZPL, `^LT` et `~TA`
sont limités par Zebra à 120 points, soit 15 mm sur une tête 203 dpi.

**GK420t : `^LT` sans effet à l'impression.** Essai du 13/11/2026 (firmware V61.17.17Z) avec le
**Diagnostic des décalages** : `^LH` et `^LS` déplacent le dessin, `^LT` non, même renvoyé deux
fois, alors que la configuration relue montre bien la valeur reçue (LABEL TOP). Le décalage vers
le bas passe donc par `^LH`, l'origine du format, prévue par Zebra pour placer tout le dessin.
Vers le haut, seul `^LT` existe (`^LH` ne prend pas de valeur négative) : sur la GK420t, un
décalage vertical négatif reste sans effet. Si l'impression est trop basse, réglez plutôt le
calibrage, ou laissez le décalage à 0.

Le décalage et la position de découpe sont deux choses différentes : le décalage déplace le
dessin **sur** l'étiquette, la position de découpe déplace le **papier** une fois l'étiquette
imprimée, pour que l'interstice tombe sur la barre de découpe.

## Maintenance depuis le logiciel

Section **Maintenance de l'imprimante** des Réglages (TSPL et ZPL). Ces commandes passent **avant**
les étiquettes en attente, même quand la file est arrêtée sur une erreur.

- **Lire l'état** : demande à l'imprimante si elle est prête. Réponses possibles : prête, tête
  ouverte, plus d'étiquettes, plus de ruban, en pause, tête trop chaude. ZPL : `~HS` ; TSPL :
  `<ESC>!?`. Le même contrôle a lieu **avant chaque étiquette** : si l'imprimante n'est pas prête, la
  file s'arrête avec la raison (indicateur rouge dans la barre du haut), comme avec la Niimbot. Une
  fois le problème réglé, **Reprendre l'impression**. Une imprimante qui ne répond pas (liaison USB
  à sens unique, convertisseur réseau) imprime sans contrôle et affiche « état inconnu ».
- **Calibrer sur les étiquettes chargées** : envoie le format du rouleau chargé (support,
  largeur, longueur, positions), fait mesurer à l'imprimante la longueur d'étiquette et régler ses
  capteurs, puis enregistre le tout pour qu'il reste après extinction. ZPL : format, `~JC`, `^JUS` ;
  TSPL : `SIZE`, `GAP`, `SET RIBBON`, `GAPDETECT`. L'imprimante avance de **2 à 4 étiquettes**.
- **Avancer d'une étiquette** : une étiquette vierge sort, comme un appui bref sur le bouton.
  ZPL : `~PH` ; TSPL : `FORMFEED`.
- **Page de configuration** : imprime les réglages de l'imprimante. ZPL : `~WC` ; TSPL : `SELFTEST`.
- **Envoyer les réglages et relire** (ZPL) : envoie le format de l'étiquette chargée (support,
  largeur, longueur, décalages, découpe, noirceur) sans rien imprimer, puis demande à l'imprimante
  sa configuration (`^HH`) et l'affiche à côté de ce qui a été envoyé : « identique », « différent »
  ou « non relu » pour le mode d'impression, le type de support, la largeur, la longueur (mesurée
  par le calibrage), la position gauche (`^LS`), le haut d'étiquette (`^LT`), la découpe (`~TA`) et
  la noirceur. La configuration complète se déplie en dessous. Rien n'est enregistré dans
  l'imprimante.
- **Étiquette d'étalonnage** : voir plus bas.
- **Diagnostic des décalages** (ZPL) : quatre étiquettes dessinées par l'imprimante elle-même
  (cadre au bord et texte, sans image) : **1** sans décalage, **2** origine déplacée de 5 mm vers
  la droite et le bas par `^LH`, **3** et **4** même déplacement par `^LS` et `^LT`. Une étiquette
  identique à la 1 montre que l'imprimante ignore cette commande ; si seule la 4 bouge, la commande
  ne s'applique qu'à l'étiquette suivante : à joindre à un signalement de problème,
  avec la page de configuration.
- **Réglages d'usine** (deux clics, le second pour confirmer) : efface tout ce qui a été réglé dans
  l'imprimante (largeur, longueur, capteur, positions, noirceur, mode) et remet les valeurs de
  l'usine. Le réseau n'est pas touché. ZPL : `^JUF` puis `^JUS` ; TSPL : `INITIALPRINTER`. À faire
  une fois sur une imprimante d'occasion, ou si l'imprimante se comporte bizarrement après des
  essais. **Calibrez juste après.**

Pour ne pas perdre les étiquettes avancées par le calibrage, ouvrez le capot, réenroulez-les à la
main sur la bobine jusqu'à la sortie, puis refermez. L'imprimante en avance au plus une pour se
recaler.

## Changer de rouleau

Le bouton **Changer de rouleau...** ouvre un assistant qui montre ces gestes un écran à la fois,
puis, à la fin (**Terminer**), envoie lui-même les commandes à l'imprimante : réglages d'usine si
vous l'avez coché, calibrage si le format change (coché d'office), lecture de l'état. Il note
aussi le rouleau en place, et les étiquettes qui l'attendaient repartent. Le dernier écran
affiche l'état de l'imprimante et propose l'étiquette d'étalonnage.

Le bouton est là où on en a besoin :

- dans la fenêtre **Changez de rouleau** qui s'ouvre quand l'impression attend un autre rouleau
  (**Me guider pas à pas**) ;
- dans le menu de l'impression, dans la barre du haut, quand la file est en pause ;
- dans **Gestion > Impressions et consommables**, à côté du rouleau en place ;
- dans **Réglages > Étiquettes et impression > Maintenance de l'imprimante**.

Le détail des étapes, pour le faire à la main. À suivre dans l'ordre, à chaque rouleau. Les
étapes 11 à 13 ne servent que pour un **nouveau format** (autre taille, autre matière, passage du
thermique direct au transfert ou l'inverse) ou la première fois.

**Avant d'ouvrir**

1. Attendez que la file d'impression soit vide (barre du haut du poste). Si elle s'est arrêtée
   parce que le rouleau est fini, c'est normal : les étiquettes restantes attendent.
2. Laissez l'imprimante **allumée**.

**Retirer l'ancien rouleau**

3. Ouvrez le capot : tirez vers vous les deux loquets jaunes, sur les côtés, et levez le capot.
4. Écartez les deux supports du rouleau et retirez le mandrin vide (ou le reste du rouleau).
   Décollez toute étiquette restée collée sous la tête ou sur le rouleau noir (rouleau d'entraînement).

**Mettre le nouveau rouleau**

5. Écartez les supports et placez le rouleau entre eux, **étiquettes vers le haut**, le papier
   partant **par-dessus** le rouleau vers l'avant. Relâchez les supports : le rouleau doit tourner
   librement.
6. Tirez le papier vers l'avant, faites-le passer **sous les deux guides** et réglez les guides
   pour qu'ils touchent le bord du papier sans le serrer.
7. Tirez le papier jusqu'à dépasser de quelques centimètres à l'avant de l'imprimante.
8. **Ruban** : en thermique direct, retirez-le s'il y en a un (il s'userait pour rien). Avec des
   étiquettes à transfert thermique, s'il est fini ou si vous changez de type de ruban, suivez le schéma collé à l'intérieur du capot (rouleau neuf et
   mandrin vide sur leurs axes, ruban passé sous la tête, scotché sur le mandrin vide, tendu en
   tournant la molette). Le ruban doit être au moins aussi large que l'étiquette, **encrage
   extérieur**. Profitez-en pour nettoyer la tête (voir Entretien).
9. Fermez le capot en appuyant **des deux côtés** jusqu'au clic. Le voyant passe au vert.

**Dire au logiciel ce qui est chargé**

10. Dans **Réglages > Étiquettes et impression**, choisissez le **Type chargé** qui correspond au
    rouleau (ou réglez largeur, hauteur et case **Transfert thermique (ruban)**). Si la file
    attendait ce rouleau, le bouton **Les étiquettes « … » sont chargées** fait la même chose.

**Nouveau format seulement**

11. **Calibrer sur les étiquettes chargées**. Attendez que l'imprimante s'arrête (2 à 4
    étiquettes). Réenroulez les étiquettes sorties si vous voulez les garder.
12. **Page de configuration** (facultatif, mais utile au premier essai) : vérifiez sur la page
    imprimée le mode d'impression (thermique direct ou transfert), la largeur d'impression et la
    longueur d'étiquette en points (8 points par mm : 40 mm = 320), et le type de support
    (interstice, « GAP/NOTCH »).
13. **Étiquette d'étalonnage** : les quatre cadres pleins doivent apparaître. Sinon, réglez le
    décalage (section suivante) et réimprimez-la jusqu'à ce qu'ils y soient.

**À chaque fois**

14. **Lire l'état** : il doit afficher « prête ». Sinon, la raison s'affiche : réglez-la (capot mal
    fermé, ruban coché sans ruban…) et relisez.
15. Si la file s'était arrêtée, cliquez sur l'indicateur d'impression puis **Reprendre
    l'impression**.
16. Appuyez une fois sur le bouton d'avance de l'imprimante si la première étiquette ne s'arrête
    pas sur la barre de découpe ; si l'arrêt est décalé à chaque étiquette, réglez la **Position
    de découpe**.

## Aligner l'impression avec l'étiquette d'étalonnage

À faire **après** un calibrage : un décalage ne corrige pas une imprimante qui ne connaît pas la
taille de son rouleau.

L'étiquette d'étalonnage est dessinée à la taille du rouleau chargé :

- un cadre plein **au ras du bord**, et un cadre pointillé à 1 mm du bord ;
- une croix au centre, graduée tous les millimètres (traits plus longs tous les 5 et 10 mm) ;
- au bout de chaque axe, des graduations numérotées **1, 2, 3, 4 mm depuis le bord**, avec le
  signe à appliquer et une flèche qui montre dans quel sens ce signe déplace l'impression :
  **+ →** à gauche, **← −** à droite, **+ ↓** en haut, **− ↑** en bas.

Lecture : si un bord est coupé, regardez le chiffre qui tombe **au ras de ce bord** et ajoutez-le,
avec son signe, au décalage correspondant :

| Ce que vous voyez | Réglage |
|---|---|
| bord droit coupé, le « 1 » côté « ← − » au ras du bord | Décalage horizontal −1 mm |
| bord gauche coupé, le « 2 » côté « + → » au ras du bord | Décalage horizontal +2 mm |
| bord du bas coupé, le « 1 » côté « − ↑ » au ras du bord | Décalage vertical −1 mm |
| bord du haut coupé, le « 2 » côté « + ↓ » au ras du bord | Décalage vertical +2 mm |

Un bord coupé s'accompagne d'une marge blanche du côté opposé : c'est normal, tout le dessin s'est
déplacé. Un décalage déjà réglé s'additionne : si l'étiquette sort encore coupée, ajoutez le
chiffre lu à la valeur actuelle. Réimprimez l'étiquette d'étalonnage après chaque réglage.

Si l'impression déborde de plusieurs millimètres sur l'étiquette suivante, ou si des étiquettes
blanches sortent entre deux impressions, ce n'est pas un décalage : **recalibrez**, et si cela ne
suffit pas, **Réglages d'usine** puis calibrage.

## Les boutons et le voyant de la GK420t

| Voyant | Signification | Que faire |
|---|---|---|
| vert fixe | prête | rien |
| vert, deux clignotements puis une pause | en pause | appuyer une fois sur le bouton d'avance |
| rouge clignotant | capot ouvert, plus d'étiquettes ou plus de ruban | fermer le capot en appuyant des deux côtés jusqu'au clic, recharger |
| orange clignotant | tête trop chaude | attendre qu'elle refroidisse |

Avec le **ruban coché** (Transfert thermique) mais sans ruban dans l'imprimante, elle signale « plus
de ruban » : décochez la case, ou choisissez un type d'étiquette en thermique direct.

Le bouton d'avance, imprimante allumée, capot fermé et voyant vert :

- appui bref : une étiquette avance ;
- appui long : le voyant clignote une fois, puis deux fois, puis trois fois… Relâchez :
  - après **1 clignotement** : page de configuration ;
  - après **2 clignotements** : calibrage (comme le bouton du logiciel, sans envoyer la taille) ;
  - **n'allez pas jusqu'à 4** : retour aux réglages d'usine. Il faudrait alors recalibrer (le
    bouton **Réglages d'usine** du logiciel fait la même chose sans toucher au réseau).

## Entretien

- Nettoyez la tête avec un coton-tige ou un stylo à l'alcool isopropylique **à chaque changement de
  ruban**, ou tous les deux ou trois rouleaux en thermique direct, imprimante éteinte. Laissez
  sécher une minute avant d'imprimer.
- Des traits blancs verticaux sur toutes les étiquettes indiquent une tête sale (nettoyer) ou usée
  (points morts : la tête se remplace).
- Gardez les rouleaux thermiques directs à l'abri du soleil et de la chaleur : ils noircissent.
