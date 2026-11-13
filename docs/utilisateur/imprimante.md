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
4. **Calibrez**, puis imprimez l'**étiquette d'étalonnage** (voir ci-dessous).

## Maintenance depuis le logiciel

Section **Maintenance de l'imprimante** des Réglages, sous l'adresse de l'imprimante (TSPL et ZPL).
Les commandes passent par la file d'impression, après les étiquettes déjà en attente.

- **Calibrer sur les étiquettes chargées** : envoie à l'imprimante la taille et le support
  (ruban ou non) du rouleau chargé, puis lui fait mesurer l'espace entre deux étiquettes.
  Elle avance de **2 à 4 étiquettes** pour cela, et garde la mesure après extinction. À faire à la
  mise en route et **à chaque nouveau format de rouleau** (pas à chaque rouleau du même format).
- **Avancer d'une étiquette** : fait sortir une étiquette vierge.
- **Page de configuration** : imprime les réglages de l'imprimante (taille, capteur, résolution,
  adresse IP).
- **Étiquette d'étalonnage** : voir la section suivante.

Pour ne pas perdre les étiquettes avancées par le calibrage, ouvrez le capot, réenroulez-les à la
main sur la bobine jusqu'à la sortie, puis refermez. L'imprimante en avance au plus une pour se
recaler.

## Aligner l'impression avec l'étiquette d'étalonnage

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
| bord du haut coupé | Décalage vertical +N mm |

Un bord coupé s'accompagne d'une marge blanche du côté opposé : c'est normal, tout le dessin s'est
déplacé. Un décalage déjà réglé s'additionne : si l'étiquette sort encore coupée, ajoutez le
chiffre lu à la valeur actuelle. Les décalages acceptent les demi-millimètres et vont de −15 à +50 mm. Réimprimez
l'étiquette d'étalonnage après chaque réglage : les quatre cadres pleins doivent apparaître.

Si l'impression déborde de plusieurs millimètres sur l'étiquette suivante, ou si des étiquettes
blanches sortent entre deux impressions, ce n'est pas un décalage : **recalibrez**.

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
  - **n'allez pas jusqu'à 4** : retour aux réglages d'usine. Il faudrait alors recalibrer.

## Entretien

- Nettoyez la tête avec un coton-tige ou un stylo à l'alcool isopropylique **à chaque changement de
  ruban**, ou tous les deux ou trois rouleaux en thermique direct, imprimante éteinte. Laissez
  sécher une minute avant d'imprimer.
- Des traits blancs verticaux sur toutes les étiquettes indiquent une tête sale (nettoyer) ou usée
  (points morts : la tête se remplace).
- Gardez les rouleaux thermiques directs à l'abri du soleil et de la chaleur : ils noircissent.
