# Modèles d'étiquettes

Dossier des modèles `*.qr` créés dans l'**Éditeur d'étiquettes**, et des images qu'ils utilisent
(logo…). Il est suivi par git : enregistrez vos modèles ici puis commitez-les pour les retrouver
après chaque clone.

- L'application lit et enregistre les modèles dans ce dossier par défaut, quel que soit le dossier
  depuis lequel elle est lancée. Un nom de fichier saisi dans l'éditeur (ex : `item.qr`) est
  enregistré ici.
- Un autre dossier peut être choisi dans **Gestion > Réglages > Étiquettes et impression**, ou avec la
  variable d'environnement `QRPROTEC_TEMPLATES_DIR`.
- Les images à mettre sur les étiquettes (logos…) vont dans le sous-dossier `images/` : l'éditeur les
  propose dans « Choisir une image du dossier » et les modèles les référencent en `images/nom.png`.
  `images/protection-civile.png` est le logo de la protection civile, utilisé par les modèles de lot
  public et de badge. L'icône de l'application n'est pas une image d'étiquette.
- Le modèle utilisé pour chaque usage (item, paquet, lot public/privé, scellé, ouverture du scellé,
  rangement du stock, badge) se choisit dans les Réglages.
- `rangement.qr` est la seule étiquette d'un rangement du stock (armoire, tiroir) : elle contient sa
  clé, qui n'expire pas, et se colle à l'intérieur. Un rangement n'a pas d'étiquette publique.
- `scelle_ouverture.qr` est l'étiquette d'ouverture, imprimée avec celle du scellé et rangée **à
  l'intérieur** du lot : la scanner ouvre le scellé, même sans être connecté.

Modèles fournis (40 × 30 mm) : `item.qr`, `paquet.qr`, `lot_public.qr`, `lot_prive.qr`, `scelle.qr`,
`scelle_ouverture.qr`, `rangement.qr`, `badge.qr`.
