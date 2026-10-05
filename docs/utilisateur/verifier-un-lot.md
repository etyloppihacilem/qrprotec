# Vérifier un lot

Une vérif, c'est : **scanner l'étiquette privée du lot, scanner tout son contenu, valider avec son
badge.** Le reste de cette page détaille les cas particuliers.

> **La règle d'or : tout scanner.** Ce qui n'est pas scanné est considéré comme absent. Scannez aussi
> les périmés que vous retirez **et** leurs remplaçants : le logiciel comprend tout seul que le neuf
> remplace le périmé.

## Au poste

1. **Scannez l'étiquette privée** du sac avec la douchette. La fenêtre **Vérif** s'ouvre : en haut
   l'état du lot (et de son lot global s'il en a un), puis la liste **À scanner** : chaque produit
   attendu, avec « scannés / attendus » et l'emplacement dans le sac.
   - Si vous scannez l'étiquette **publique**, la fiche du lot s'affiche avec **Lancer une vérif** ;
     il faudra quand même scanner l'étiquette privée pour valider (sauf rôle gestion ou admin).
2. **Scannez chaque objet.** Il apparaît dans la **pile de scans** (colonne de droite) et disparaît de
   la liste « À scanner ».
   - Un **périmé** fait biper la douchette (LED orange) et s'affiche en rouge « PÉRIMÉ ». Si vous le
     retirez, scannez son remplaçant : le périmé sera compté comme remplacé.
   - Un **code inconnu** s'affiche en rouge.
   - Un objet scanné deux fois est ignoré (simple mention « déjà scanné »).
   - **Annuler le dernier scan** retire le dernier objet de la pile ; la croix ✕ retire une ligne.
3. **Valider la vérif.** Si vous n'êtes pas connecté, le poste demande votre **badge** (et votre PIN
   si vous en avez un). Si l'administrateur l'a autorisé, vous pouvez aussi saisir votre nom et
   **Continuer sans badge** (il faut alors avoir scanné l'étiquette privée du lot). Le compte rendu s'affiche :
   - **✔ VÉRIF ENREGISTRÉE : LOT COMPLET** : tout va bien ;
   - **✘ INCOMPLET** : la liste de ce qui manque, des périmés restés dans le lot, des items attendus
     mais non scannés. Complétez depuis le stock et refaites une vérif, ou signalez-le à la
     logistique (un SMS lui a peut-être déjà été envoyé).

La fenêtre **Lots** (menu Fenêtres) liste tous les lots avec leur état ; un clic ouvre la fiche, avec
**Lancer une vérif**.

## Au téléphone

1. Ouvrez l'appareil photo et **scannez l'étiquette privée** du sac : la page QRProtec s'ouvre.
   Touchez **Démarrer la caméra** (et autorisez la caméra la première fois).
2. **Scannez votre badge** si ce n'est pas déjà fait (le téléphone s'en souvient). Saisissez votre PIN
   si demandé. Si l'administrateur l'a autorisé, vous pouvez à la place toucher **Non connecté** et
   indiquer votre nom.
3. **Scannez chaque objet.** Onglets en bas : **À scanner** (ce qui reste, les périmés en rouge),
   **Scannés** (avec « Annuler le dernier » et « Vider la liste »), **Lot** (exigences scannées /
   attendues). Un périmé ou un code inconnu fait clignoter l'écran en rouge, biper et vibrer.
4. **Valider la vérif.** Le compte rendu s'affiche.

La liste en cours est conservée si la page se recharge ou si le téléphone se met en veille. Le menu
**⋯** permet de saisir un code à la main, de changer de lot ou de se déconnecter.

L'onglet **Accueil** (sans lot en cours) affiche la **liste des lots** avec leur état ; toucher un lot
l'ouvre.

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

## Réassort : ajouter du matériel sans tout vérifier

Au retour d'intervention, quand on remet dans le sac ce qui a été utilisé :

- **Au poste** : scannez les objets ajoutés, puis l'**étiquette privée** du sac, puis **Ajouter au
  lot … (réassort)** dans la pile de scans. Pendant une vérif où vous n'avez scanné que des objets
  nouveaux, un bouton orange **Ajouter N item(s) au lot – réassort, sans vérif** fait la même chose.
- **Au téléphone** : scannez l'étiquette privée, les objets ajoutés, puis **Ajouter au lot
  (réassort)**.

Le reste du contenu du sac n'est pas touché. Le sac passe **orange** « vérif recommandée », avec le
nombre d'objets ajoutés, la date et votre nom, jusqu'à la prochaine vérif complète.

## Vérifier plusieurs sous-lots ensemble (vérif groupée)

Exemple : refaire tout le B+ (sac de soin + sac O2) d'un coup.

- Scannez l'étiquette privée du **B+** : la vérif couvre automatiquement ses deux sacs, la liste « À
  scanner » est regroupée par sac.
- Ou scannez l'étiquette privée du **sac de soin**, puis pendant la vérif celle du **sac O2** : elle
  s'ajoute à la vérif.

Chaque objet scanné est rangé dans le sac où il manque. Les lots d'une même vérif doivent appartenir
au même lot global (on ne peut pas mélanger le B+ 1 et le sac PSE A).

## Vérif partielle

Si, dans une vérif groupée, un des sacs est complet et l'autre non (il manque des masques O2), le
bouton **Vérif partielle** valide **seulement les sacs complets**. Les objets scannés pour l'autre sac
y sont ajoutés en réassort (il passe orange), sans que le reste de son contenu soit déclaré manquant.

## Lot scellé

Scanner l'étiquette du **scellé** affiche la fiche du lot :

- **Scellé intact** (vert) : rien à faire, le lot est bon jusqu'à la date indiquée ;
- **Ancien scellé** : l'étiquette ne correspond plus au scellé en cours ; le lot a été ouvert depuis.

En ouvrant un lot scellé, **scannez l'étiquette d'ouverture** rangée à l'intérieur (« Scanner après
l'ouverture ») : le scellé est marqué ouvert, que vous soyez connecté ou non. Il n'y a pas de bouton
« ouvrir » : le scan suffit. Si vous n'êtes pas connecté, le téléphone vous demande ensuite votre badge
pour signer l'ouverture.

Vérifier un lot scellé, y ajouter ou en retirer quelque chose, ou ouvrir un de ses sous-lots **brise
le scellé** (le poste prévient avant). Après la vérif, un responsable pourra le resceller.

## Comprendre le compte rendu

| Rubrique | Signification | Que faire |
|---|---|---|
| Présents | Scannés et rangés dans le lot | — |
| Périmés toujours dans le lot | Scannés, périmés, sans remplaçant | Les remplacer, refaire une vérif |
| Périmés considérés comme remplacés | Un neuf du même type a été scanné : le périmé est sorti du lot | Le jeter |
| Attendus mais non scannés | Le logiciel les croyait dans le lot | Les chercher ; sinon ils deviendront « disparus » |
| Retrouvés | Étaient signalés disparus, ils sont revenus | — |
| Codes inconnus ignorés | Étiquette illisible ou d'un autre système | Réimprimer l'étiquette (rôle gestion) |
