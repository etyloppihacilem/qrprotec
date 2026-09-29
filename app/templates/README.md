# Modèles d'étiquettes

Dossier des modèles `*.qr` créés dans l'**Éditeur d'étiquettes**, et des images qu'ils utilisent
(logo…). Il est suivi par git : enregistrez vos modèles ici puis commitez-les pour les retrouver
après chaque clone.

- L'application lit et enregistre les modèles dans ce dossier par défaut, quel que soit le dossier
  depuis lequel elle est lancée. Un nom de fichier saisi dans l'éditeur (ex : `item.qr`) est
  enregistré ici.
- Un autre dossier peut être choisi dans **Gestion > Réglages > Étiquettes et impression**, ou avec la
  variable d'environnement `QRPROTEC_TEMPLATES_DIR`.
- Les images d'un modèle (ex : `logo.png`) sont cherchées d'abord dans ce dossier : placez-y le logo de
  la protection civile pour qu'il soit versionné avec les modèles.
- Le modèle utilisé pour chaque usage (item, paquet, lot public/privé, scellé, badge) se choisit dans les
  Réglages.

Modèles fournis (40 × 30 mm) : `item.qr`, `paquet.qr`, `lot_public.qr`, `lot_prive.qr`, `scelle.qr`,
`badge.qr`.
