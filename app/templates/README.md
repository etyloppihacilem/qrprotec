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

  Les étiquettes portent l'iid en **forme courte** (`{{iid_short}}`, aussi dans l'URL des QR) : `D` pour
  une date « 00000000 » (sans péremption), `N` à la place des zéros de tête du compteur, par exemple
  `serphy20261029N7` pour `serphy2026102900000007` et `lecglyDN6` pour `lecgly0000000000000006`. La base
  garde l'iid complet ; les fronts lisent les deux formes, et les étiquettes déjà imprimées restent valables.

  Sans modèle pour une version, l'impression prend la version normale du style, puis `item.qr`. Dans
  l'éditeur, l'élément **Code-barres** garde des barres de largeur entière de pixels : sur 40 mm à
  8 points/mm, un iid tient à 1 point par barre fine, d'où le code sur toute la largeur.
- `item_planche.qr` (usage « Item - planche ») : une étiquette qui se découpe en 5 petites étiquettes
  d'items (ex : 5 dosettes de sérum phy), pour les types au style **Planche**. Chaque placeholder d'item
  porte le numéro de l'item sur la planche (`{{code1}}`, `{{type_name1}}`, `{{exp1}}`… `{{code5}}`) :
  le plus grand numéro donne le nombre d'items par planche, et la dernière planche d'une impression
  garde des places vides. `{{code}}` est l'iid court (petit QR), `{{exp}}` la péremption JJ/MM/AAAA.
- `emplacement.qr` (usage « Lot - emplacement ») : le nom d'un emplacement du lot (pochette d'un sac,
  compartiment…), **sans QR code**, à coller sur l'emplacement. Fiche du lot, onglet **Étiquettes** :
  liste à cocher des emplacements du contenu (ordre de la vérif) et nombre d'exemplaires. Placeholders :
  `{{emplacement}}` (`{{Emplacement}}`, `{{EMPLACEMENT}}`), `{{emplacement_numero}}`, `{{contenu}}` (une ligne
  par type attendu, ex : « 4 × Compresses stériles ») et ceux du lot (`{{lot_type_name}}`…).
- `carte_invite.qr` (usage « Carte d'invité ») : carte de connexion d'un secouriste d'une autre antenne,
  imprimée à l'avance **sans nom** (Administration > Utilisateurs, onglet **Cartes d'invités**). L'invité
  donne son nom et son prénom à la première connexion ; la carte vaut ensuite `{{guest_hours}}` heures.
  Placeholders : `{{matricule}}` (numéro de la carte, INV…), `{{badge_url}}`, `{{guest_hours}}`,
  `{{key_expires}}` (date limite de première connexion), `{{date_limite}}` (la même, vide sur une carte à un
  nom). Carte réimprimée pour un invité qui revient : `{{full_name}}`, `{{prenom}}`, `{{nom}}`, `{{antenne}}`,
  et `{{validite}}` (« Valable 48 h après la 1re connexion » sur une carte neuve, l'antenne et la fin de
  validité sur une carte à un nom).
- `scelle_ouverture.qr` est l'étiquette d'ouverture, imprimée avec celle du scellé et rangée **à
  l'intérieur** du lot : la scanner ouvre le scellé, même sans être connecté.

Modèles fournis (40 × 30 mm) : les 9 étiquettes d'items ci-dessus, `item_planche.qr`, `paquet.qr`, `lot_public.qr`, `lot_prive.qr`, `scelle.qr`,
`scelle_ouverture.qr`, `rangement.qr`, `emplacement.qr`, `badge.qr`, `carte_invite.qr`.
