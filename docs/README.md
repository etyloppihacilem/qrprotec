# Documentation de QRProtec

QRProtec est un inventaire par QR code du matériel de secours : chaque item (compresse, sérum
physiologique, garrot…) porte une étiquette avec un identifiant unique, et les lots (sacs, malles,
armoires) se vérifient en scannant leur contenu. Le logiciel sait à tout moment ce qu'il y a dans
chaque lot, ce qui manque, ce qui est périmé et ce qu'il reste en stock.

La documentation est découpée en trois parties, selon ce que vous venez faire.

## Je veux utiliser QRProtec

[Guide de l'utilisateur](utilisateur/README.md) : les notions (item, lot, sous-lot, scellé…), la mise
en place pas à pas dans une antenne qui passe d'un inventaire papier à QRProtec, l'usage quotidien
(vérifier un sac, réassortir, gérer le stock) et l'administration.

## Je veux installer QRProtec

[Guide d'installation](installation/README.md) : les différents modes d'installation (borne complète
en mode kiosk, back seul sur un serveur, front seul sur un poste, poste de bureau sans kiosk), la
configuration, les sauvegardes, les mises à jour.

## Je veux modifier QRProtec

[Documentation technique](technique/README.md) : l'architecture d'ensemble, le rôle de chaque
dossier, les choix de conception et leurs raisons, et des recettes pour les modifications courantes.
C'est le point de départ pour reprendre le code, même des années plus tard.

## Le reste du dépôt

- [`README.md`](../README.md) à la racine : résumé de référence (routes de l'API, variables
  d'environnement, règles de gestion). En cas de doute, le code fait foi, puis ce README.
- [`app/templates/README.md`](../app/templates/README.md) : les modèles d'étiquettes fournis.
