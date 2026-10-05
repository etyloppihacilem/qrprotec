# Mise en place et transition

Ce chapitre suit une antenne de secourisme qui gère aujourd'hui son matériel avec des fiches papier
et un tableur, et qui passe à QRProtec. Adaptez les noms et les quantités à votre structure ; l'ordre
des étapes, lui, compte.

**L'antenne de l'exemple** possède :

- un VPS avec une armoire de cellule, un B+ (sac de soin + sac O2) et un lot DSA ;
- deux sacs PSE indépendants ;
- une réserve avec deux armoires, dont une à tiroirs ;
- une malle catastrophe qu'on n'ouvre presque jamais.

**Matériel nécessaire** : le poste installé (voir le [guide d'installation](../installation/README.md)),
la douchette, l'imprimante Niimbot B1 avec un rouleau d'étiquettes (40 × 30 mm conseillé), et des
scellés numérotés pour la malle.

> **Conseil général** : ne pas tout faire le même jour. Compter une première session pour la
> configuration (étapes 1 à 5), puis un lot par séance pour l'étiquetage (étape 6), et garder les
> fiches papier en parallèle jusqu'à la fin de l'étape 7.

---

## Étape 1 — Premier administrateur

Au premier lancement, le poste constate qu'il n'y a aucun administrateur et propose de le créer :
matricule (ex : `M001`), nom, prénom, code PIN. Le compte est connecté directement, en mode
privilégié (écran orange). Cliquez sur **Imprimer mon badge** : c'est votre clé d'accès, gardez-la
sur vous.

Si cet écran n'apparaît pas (base déjà utilisée, badge perdu), l'informaticien peut créer un
administrateur sur le serveur : `sudo qrprotec-manage createadmin M001 Nom Prénom --pin 4821`.

## Étape 2 — Régler l'impression

Menu **Gestion > Réglages** (admin), section **Étiquettes et impression** :

1. **Imprimante Niimbot B1** : port (`/dev/ttyACM0` en général), puis **Imprimer test**.
2. **Étiquettes chargées dans l'imprimante** : largeur et hauteur du rouleau (40 × 30 mm).
3. **Titre des étiquettes** : le nom de votre structure, imprimé sur les étiquettes de lot et les
   badges (par défaut « PROTECTION CIVILE / PARIS CENTRE »).
4. **Étiquettes** : le modèle utilisé pour chaque usage (item, paquet, lot public, lot privé, scellé,
   ouverture du scellé, rangement du stock, badge). Les modèles fournis conviennent pour commencer ; votre logo se change dans l'éditeur
   d'étiquettes (voir [Administration](administration.md#étiquettes)).

Imprimez votre badge une seconde fois si le premier était raté : c'est le même QR code.

## Étape 3 — Les utilisateurs

Menu **Gestion > Utilisateurs** : créez d'abord l'équipe logistique (rôle **gestion**), puis un ou deux
autres **admins** pour ne jamais dépendre d'une seule personne. Les secouristes peuvent être créés
plus tard, au fil des besoins. À chaque création, un aperçu du badge s'affiche : imprimez-le et
remettez-le à la personne.

## Étape 4 — Les types d'items

Menu **Gestion > Inventaire**, onglet **Types d'items**, bouton **Nouveau type**. Partez de votre
tableur : une ligne par produit différent.

| Code | Nom | Périssable | Stock minimum | Taille de paquet |
|---|---|---|---|---|
| `compre` | Compresses stériles 10×10 | oui | 100 | 25 |
| `serphy` | Sérum phy 10 ml | oui | 40 | 20 |
| `garrot` | Garrot tourniquet | non | 4 | 1 |
| `couvsu` | Couverture de survie | non | 20 | 1 |
| `gantsm` | Gants nitrile M (boîte) | oui | 5 | 1 |
| `maso2a` | Masque O2 haute concentration adulte | oui | 6 | 1 |

Conseils :

- Le **code** fait exactement 6 caractères (lettres et chiffres) et ne pourra plus changer : il est
  imprimé dans chaque étiquette. Choisissez des codes parlants et réguliers.
- Créez un type différent dès que le produit n'est pas interchangeable (taille de gants, masque
  adulte / enfant).
- Le **stock minimum** déclenche l'alerte « stock bas » ; mettez 0 si vous ne voulez pas d'alerte.
- La **taille de paquet** n'est qu'une valeur proposée à la réception.
- **Étiquette à déchirer avant utilisation** : pour un ensemble étiqueté une seule fois (ex : un
  sachet de plusieurs sérums phy). Son étiquette porte « Déchirer avant utilisation » : on l'arrache
  dès qu'on entame l'ensemble. À la vérif suivante, l'étiquette manquante compte l'ensemble comme
  **utilisé** tout de suite (sans attendre plusieurs vérifs) : il faut remettre un ensemble complet,
  les restes de l'ancien sont considérés comme perdus.
- Le nom d'un type doit être unique (sans tenir compte des accents ni des majuscules) : le formulaire
  le signale pendant la saisie.
- Un type qui ne sert plus s'**archive** (bouton **Archiver le type…**) : il n'est plus proposé à la
  réception ni dans le contenu des lots. Pour le retrouver : case **Afficher les types archivés**,
  puis **Désarchiver le type**.

## Étape 5 — Les types de lots et leur contenu

Onglet **Types de lots** de **Gestion > Gestion des lots**. Pour chaque modèle de sac, créez le type
et son **contenu attendu**, avec si vous le souhaitez l'emplacement :

**Sac de soin** (`sacsoi`)

| Type d'item | Quantité | Emplacement |
|---|---|---|
| Compresses stériles | 20 | pochette rouge |
| Sérum phy 10 ml | 6 | pochette rouge |
| Garrot tourniquet | 2 | poche avant |
| Couverture de survie | 4 | fond |

**Sac O2** (`saco2x`) : masques O2 adulte et enfant, lunettes, etc.

Créez aussi :

- des types **sans contenu** pour les regroupements : « VPS » (`vpsxxx`), « B+ » (`bplusx`) ;
- un type **rangement** par armoire et par tiroir de la réserve : « Réserve armoire 1 » (`resar1`),
  « Tiroir 1 » (`tiroi1`)…, case **Rangement du stock** cochée. Un rangement est toujours un **lot
  unique** : son lot est créé avec le type (lot parent dans le même formulaire). Il n'a jamais de
  contenu attendu (on y range ce qu'on veut) et n'a qu'**une étiquette**, l'étiquette de rangement
  (modèle `rangement.qr`) : elle contient sa clé, qui **n'expire pas**, reste à l'intérieur, dans une
  salle fermée, et se renouvelle seulement en cas de problème.

Un lot qui n'existe qu'en un exemplaire (le VPS, une armoire du VPS) n'a pas besoin d'un type puis
d'un lot : cochez **Lot unique** à la création du type. Le lot, du même nom (nom court compris,
limité à 16 caractères), est créé avec le type (avec son lot parent), et aucun autre lot de ce type
ne peut être créé. Renommer le
type renomme son lot ; l'archiver archive son lot.

Les types de lots s'archivent comme les types d'items (case **Afficher les types archivés** pour les
désarchiver) ; un type dont des lots sont actifs ne peut pas être archivé.

Le contenu attendu peut évoluer plus tard : les lots existants suivent immédiatement la nouvelle
définition (un sac complet peut donc repasser rouge si on ajoute une ligne).

## Étape 6 — Les lots et leurs étiquettes

Toujours dans **Gestion des lots**, bouton **Nouveau lot**. Créez les lots **du plus grand au plus
petit**, pour pouvoir choisir le lot parent :

1. `VPS 1` (type VPS, aucun parent) ;
2. `Armoire cellule VPS 1` (rangement, créé avec son type, parent VPS 1) ;
3. `B+ 1` (type B+, parent VPS 1) ;
4. `Sac de soin B+ 1` et `Sac O2 B+ 1` (parent B+ 1) ;
5. `Lot DSA VPS 1` (parent VPS 1) ;
6. `Sac PSE A`, `Sac PSE B` (aucun parent) ;
7. `Réserve armoire 1`, `Réserve armoire 2`, puis `Tiroir 1` à `Tiroir 6` (parent : armoire 2) :
   des rangements, créés chacun avec son type ;
8. `Malle catastrophe`.

Le nom d'un lot doit être unique parmi les lots actifs : le formulaire le signale pendant la saisie.
Un lot archivé se retrouve avec la case **Afficher les lots archivés** (bouton **Désarchiver le lot**).
Les rangements du stock sont masqués des listes de lots (fenêtres **Lots** et **Gestion des lots**,
accueil du front web) : cochez **Afficher les rangements du stock** pour les voir.

À chaque création, l'aperçu propose les étiquettes **publique** et **privée** : imprimez les deux.
Collez la publique à l'extérieur, rangez la privée à l'intérieur (ou dans le classeur des
responsables pour les lots sensibles). Le **nom court** apparaît sur les petites étiquettes. Un
rangement du stock n'a que son **étiquette de rangement**, à coller à l'intérieur.

## Étape 7 — Étiqueter le matériel existant (la transition)

C'est l'étape la plus longue. On la fait **lot par lot**, en commençant par les lots opérationnels
(le VPS), puis les sacs, et la réserve en dernier. Pour un sac :

1. **Vider le sac** sur une table et regrouper le matériel par produit **et par date de péremption**.
2. Pour chaque groupe, menu **Gestion > Inventaire**, onglet **Réception** : type, date de péremption
   (on peut taper `09/2027` ou `0927` pour « fin septembre 2027 »), quantité. Bouton **Créer N
   item(s) et voir les étiquettes**, puis **Imprimer**.
3. **Coller** chaque étiquette sur son objet. Les objets trop petits (un comprimé, un pansement) se
   gèrent par leur contenant : étiquetez la boîte ou le sachet comme un seul item, ou utilisez un
   paquet fermé.
4. Les items créés sont pour l'instant **en stock**. Scannez l'**étiquette privée** du sac : la
   fenêtre Vérif s'ouvre et affiche ce qui est attendu.
5. **Scannez tout** en remettant les objets dans le sac. Chaque item scanné disparaît de la liste
   « À scanner ».
6. Scannez votre badge si besoin et **Valider la vérif**. Le compte rendu indique si le sac est
   complet. Les items sont maintenant **dans le sac**.

Pour un **B+**, faites le sac de soin et le sac O2 séparément, ou ensemble : pendant la vérif du sac de
soin, scannez l'étiquette privée du sac O2, elle s'ajoute à la vérif et chaque item va dans le sac
où il manque.

Pour la **réserve**, même méthode, armoire par armoire ou tiroir par tiroir : réception, étiquettes,
puis vérif du rangement (étiquette privée du tiroir et contenu). Ce qui n'a pas de rangement reste
simplement en stock.

Les **boîtes encore fermées** de la réserve : à la réception, cochez **Paquet fermé** ; une seule
étiquette est imprimée pour la boîte. Les étiquettes individuelles seront imprimées à l'ouverture
(voir [Gérer le stock](gerer-le-stock.md#paquets-fermés)).

**Pendant la transition**, continuez à tenir les fiches papier des lots pas encore passés. Un lot
qui n'a jamais été vérifié apparaît en rouge « jamais vérifié » : c'est la liste de ce qui reste à
faire.

## Étape 8 — Sceller les lots de réserve

Pour la malle catastrophe, une fois sa vérif complète : **Gestion des lots**, fiche du lot, numéro du
scellé physique, **Sceller le lot et imprimer les étiquettes**. Rangez l'étiquette d'ouverture dans la
malle, posez le scellé et collez l'étiquette du scellé à côté. La malle reste verte sans vérif jusqu'à la première péremption de son contenu.

## Étape 9 — Les alertes

Menu **Gestion > Réglages > Notifications SMS** (avec une ligne Free Mobile) et/ou les notifications
web sur le téléphone d'un admin (onglet **Stock**). Voir [Administration](administration.md#notifications).
Choisissez au moins « stock bas » et « vérif incomplète ».

## Étape 10 — Former les secouristes

Une démonstration de dix minutes suffit, avec la page [Vérifier un lot](verifier-un-lot.md) :

- scanner un sac au poste ou avec son téléphone ;
- **tout scanner**, y compris les périmés qu'on retire et leurs remplaçants ;
- valider avec son badge ;
- au retour d'intervention, faire un **réassort** si on n'a pas le temps d'une vérif complète.

Distribuez les badges, affichez la procédure près du poste. La transition est terminée quand tous les
lots sont verts ou orange et que la réserve a été vérifiée : rangez les fiches papier.

---

## Et ensuite : le rythme de croisière

| Quand | Quoi | Qui |
|---|---|---|
| Avant chaque départ en poste | Vérifier l'état du VPS (vert ?) sur le poste ou le téléphone | chef d'équipe |
| Au retour d'intervention | Réassort ou vérif des sacs utilisés | équipe |
| Chaque mois | Vérif complète des lots opérationnels ; vérifier les lots « bientôt périmés » | logistique |
| À chaque livraison | Réception de la commande, étiquettes, rangement | logistique |
| Chaque trimestre | Vérif de la réserve, tiroir par tiroir | logistique |
| Chaque année | Renouveler les badges qui expirent ; contrôler que les sauvegardes existent | admin |
