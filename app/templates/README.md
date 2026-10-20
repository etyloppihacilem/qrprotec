# Modèles d'étiquettes

Dossier des modèles `*.qr` créés dans l'**Éditeur d'étiquettes**, et des images qu'ils utilisent
(logo…). Il est suivi par git : enregistrez vos modèles ici puis commitez-les pour les retrouver
après chaque clone.

- L'application lit et enregistre les modèles dans ce dossier par défaut, quel que soit le dossier
  depuis lequel elle est lancée. Un nom de fichier saisi dans l'éditeur (ex : `item.qr`) est
  enregistré ici.
- Un autre dossier peut être choisi dans **Administration > Réglages > Étiquettes et impression**, ou avec la
  variable d'environnement `QRPROTEC_TEMPLATES_DIR`.
- Les images à mettre sur les étiquettes (logos…) vont dans le sous-dossier `images/` : l'éditeur les
  propose dans « Choisir une image du dossier » et les modèles les référencent en `images/nom.png`.
  `images/protection-civile.png` est le logo de la protection civile, utilisé par les modèles de lot
  public et de badge. L'icône de l'application n'est pas une image d'étiquette.
- Le modèle utilisé pour chaque usage (item, paquet, lot public/clé, scellé, ouverture du scellé,
  rangement du stock, badge) se choisit dans les Réglages.
- Le dossier peut aussi contenir des étiquettes libres (usage « Générique ») qui ne servent jamais de
  modèle : indications, autocollants… L'onglet **Toutes les étiquettes** de l'éditeur les montre en
  grille avec les modèles (pastille verte « Modèle ») et les imprime à la main.
- `rangement.qr` est la seule étiquette d'un rangement du stock (armoire, tiroir) : elle contient sa
  clé, qui n'expire pas, et se colle à l'intérieur. Un rangement n'a pas d'étiquette publique.
- `tache.qr` est l'étiquette de tâches : elle remplace `item.qr` pour les items dont le type a des
  tâches à faire à la vérif (ex : DAE), avec la liste des tâches à cocher.
- Étiquettes d'items : 3 styles, choisis par type d'item (champ « Étiquette »), chacun en version
  normale, liste de tâches et à déchirer :

  | Style | Normale | Liste de tâches | À déchirer |
  |---|---|---|---|
  | QR code (URL de l'item) | `item.qr` | `tache.qr` | `item_dechirer.qr` |
  | QR code réduit (iid seul) | `item_reduite.qr` | `tache_reduite.qr` | `item_reduite_dechirer.qr` |
  | Code-barres Code 128 (iid seul) | `item_codebarre.qr` | `tache_codebarre.qr` | `item_codebarre_dechirer.qr` |

  Sans modèle pour une version, l'impression prend la version normale du style, puis `item.qr`. Dans
  l'éditeur, l'élément **Code-barres** garde des barres de largeur entière de pixels : sur 40 mm à
  8 points/mm, un iid tient à 1 point par barre fine, d'où le code sur toute la largeur.
- `scelle_ouverture.qr` est l'étiquette d'ouverture, imprimée avec celle du scellé et rangée **à
  l'intérieur** du lot : la scanner ouvre le scellé, même sans être connecté.

Modèles fournis (40 × 30 mm) : les 9 étiquettes d'items ci-dessus, `paquet.qr`, `lot_public.qr`, `lot_prive.qr`, `scelle.qr`,
`scelle_ouverture.qr`, `rangement.qr`, `badge.qr`.
