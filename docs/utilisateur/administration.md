# Administration

Cette page concerne le rôle **admin** (sauf « Lots » et « Scellés », accessibles au rôle gestion).

## Utilisateurs et badges

Menu **Administration > Utilisateurs**.

- **Nouvel utilisateur** : matricule (1 à 16 lettres, chiffres, `-` ou `_`, définitif), prénom, nom,
  rôle, PIN éventuel. **Créer et voir le badge** affiche l'aperçu ; imprimez-le.
- **Rôle** : Secouriste, Gestion ou Administrateur (voir [les notions](notions.md#badge-pin-et-rôles)).
  Gardez au moins **deux administrateurs**.
- **Badge perdu, abîmé ou expiré** (validité un an) : **Renouveler (1 an)** puis imprimer le nouveau
  badge. L'ancien badge ne fonctionne plus immédiatement. Le serveur ne garde pas la clé du badge :
  elle n'est affichée qu'à la création et au renouvellement, donc réimprimer un badge veut dire le
  renouveler.
- **Départ d'un bénévole** : décochez **Compte actif**. Son nom reste dans l'historique des vérifs.
- **PIN** : **Définir le PIN** / **Supprimer le PIN** (impossible pour un admin). Un admin sans PIN le
  choisit à sa prochaine connexion. Après 5 PIN faux, le compte est bloqué 5 minutes.
- **PIN obligatoire ou désactivé** : **Rendre le PIN obligatoire** (sans PIN, il le choisit à sa
  prochaine connexion), **Désactiver le PIN** (PIN supprimé et plus jamais demandé), **Réglage par
  défaut** pour revenir au réglage **Réglages > Règles du serveur > Code PIN obligatoire par défaut
  pour les secouristes** (décoché au départ). Les admins ont toujours un PIN.
- **Connexion sans badge** : un utilisateur qui a un PIN peut se connecter avec son prénom, son nom
  et son PIN (téléphone : bouton « Nom + PIN » du petit lecteur ou puce « Non connecté » ; poste :
  lien « Sans badge » de la fenêtre de connexion). Après 5 essais faux, pause de 15 minutes ; après
  20, il doit se reconnecter une fois avec son badge.
- **Matricule seul** : un code qui ne contient que le matricule (carte d'un autre système, code tapé à
  la douchette) est lu comme un badge sans sa clé : le PIN est alors toujours demandé, avec les mêmes
  limites que la connexion par nom. Sans PIN défini, il faut le vrai badge.
- **PIN bloqué** (50 PIN faux) : la fiche l'indique en rouge. **Réinitialiser le PIN** le débloque ;
  l'utilisateur en choisit un nouveau à sa prochaine connexion. L'utilisateur peut aussi envoyer le
  lien affiché sur son téléphone : l'admin le scanne avec le front web, se connecte et confirme.
  Si le badge a pu être volé, renouvelez plutôt le badge.
- **Code oublié** : le lien **Code oublié ?** sous la saisie du PIN (téléphone et poste) bloque le
  PIN ; le téléphone affiche le même écran de déblocage, et la fiche indique « Code PIN oublié ».
- **Notification** : les admins sont prévenus d'un PIN bloqué ou oublié par notification web (case
  de l'abonnement, menu ⋯ > Notifications). Une seule notification par blocage, quel que soit le nombre de
  demandes ; toucher la notification web ouvre directement le déblocage.
- **Admin à contacter** : l'administrateur dont le nom s'affiche sur le téléphone quand le PIN est
  bloqué. Sans choix, c'est l'**admin à contacter par défaut** : case à cocher dans la fiche d'un
  administrateur (un seul à la fois). Sans admin par défaut : « un administrateur ».

Plus aucun administrateur ne peut se connecter ? Sur le serveur :
`sudo qrprotec-manage createadmin M001 Nom Prénom --pin 4821` (crée le compte ou donne un nouveau
badge et le rôle admin à un compte existant).

## Lots

Menu **Gestion > Gestion des lots** (rôle gestion).

- **Types de lots** : contenu attendu (type d'item, quantité, emplacement), case **Rangement du
  stock** pour les armoires et tiroirs.
- **Nouveau lot** : type, nom, nom court (16 caractères, pour les petites étiquettes), lot parent
  (« Aucun : lot indépendant » ou le lot global). Aperçu et impression des étiquettes publique et
  clé.
- **Fiche d'un lot** : modifier le nom, le parent ou l'**emplacement de l'étiquette clé** (ex : « poche intérieure du couvercle », affiché à qui essaie de valider une vérif sans l'avoir scannée), réimprimer les étiquettes (**publique**,
  **clé**, **les deux**), **Archiver le lot** quand il est réformé (il disparaît des listes ; il
  se réactive).
- **Régénérer la clé** : si l'étiquette clé est perdue ou a pu être copiée. L'ancienne étiquette
  clé ne fonctionne plus ; imprimez la nouvelle. Une clé est valable 1 an par défaut ; la date
  d'expiration est affichée dans la fiche.

## Scellés

Dans la fiche d'un lot (rôle gestion) :

1. Faites d'abord une **vérif complète** : un lot (et ses sous-lots) incomplet ou contenant des
   périmés ne peut pas être scellé, sauf **Sceller quand même**.
2. Saisissez le **numéro du scellé** physique, **Sceller le lot et imprimer les étiquettes** : deux
   étiquettes sortent, celle du scellé et l'**étiquette d'ouverture** (« Scanner après l'ouverture »).
3. Rangez l'étiquette d'ouverture **à l'intérieur** du lot, bien visible à l'ouverture, puis posez le
   scellé et collez l'étiquette du scellé à côté.

Celui qui ouvre le lot scanne l'étiquette d'ouverture : le scellé est marqué ouvert, même s'il n'est
pas connecté. Un écran en grand (téléphone ou poste) confirme que l'ouverture est enregistrée et que le
TECH en est informé, indique si le scellé était valide et jusqu'à quand, et montre le contenu du scellé ;
il ne propose pas de vérif et ne demande pas de signer l'ouverture. Si personne n'est connecté, elle est
enregistrée comme anonyme.

**Briser le scellé** depuis la fiche quand on ouvre le lot pour autre chose qu'une vérif. Le lot
devra être vérifié avant utilisation.

**Scellé obligatoire** : cochez cette case dans le type de lot (onglet des types de lot) pour les lots
qui doivent toujours être scellés, comme les kits accouchement. Un lot de ce type ouvert est rouge ;
après sa vérif complète il reste rouge (« doit être scellé ») jusqu'à ce qu'il soit scellé à nouveau.
À la création d'un lot de ce type, seule l'étiquette clé est proposée : l'étiquette du scellé, imprimée
au scellage avec l'étiquette d'ouverture, tient lieu d'étiquette publique.

Raccourci : après une vérif complète dans l'application, le compte rendu propose aux rôles gestion et
admin un bouton **Sceller** pour chaque lot trouvé complet et pas déjà scellé (numéro du scellé facultatif). Il scelle le
lot et ouvre l'aperçu des mêmes étiquettes que depuis la fiche du lot.

## Signalements de problèmes

Chaque signalement envoie une notification aux admins (notification web, événement « Problème
signalé »). Ils se gèrent dans la page **Signalements**, au poste (compteur rouge dans la barre de
menu) comme sur le téléphone (tuile de l'accueil). Au poste, quand un admin se connecte et qu'il reste
des signalements non traités, la fenêtre s'ouvre d'elle-même à côté de la fenêtre Lots (sauf pendant
une vérif) :

- **Nouveau** (rouge) : personne ne s'en occupe encore ;
- **En cours · utilisable** (vert) / **En cours · inutilisable** (orange) : pris en charge ; l'admin
  confirme ou corrige si l'objet reste disponible. Inutilisable, l'item doit être remplacé ;
- **Résolu** (vert) : pour un item ou un paquet, l'admin choisit ce qu'il devient : **remis en service**
  (redevient normal), **désactivé** (non conforme : à remplacer comme un périmé) ou **retiré** (déjà
  jeté : marqué supprimé). Un signalement résolu peut être rouvert.

Une réponse (facultative) est visible de tous les utilisateurs qui suivent les signalements.

### Désactiver des items (rappel produit, non-conformité)

Réservé aux admins, au poste. Un item désactivé est invalide comme s'il était périmé : il ne compte plus
dans son lot ni dans le stock, s'affiche **DÉSACTIVÉ** et le remplaçant le sort du lot à la vérif. Une
raison est obligatoire ; tout se réactive de la même façon.

- **Un item** : **Gestion > Inventaire**, onglet Items, sélectionnez l'item, **Actions avancées** ;
- **Toute une réception** : onglet **Réceptions passées** (filtrable par type, ou « Voir les réceptions
  de ce type » depuis un item). Une réception regroupe les items reçus ensemble : utile pour un paquet
  ouvert dès la réception, ou un rappel de lot du fabricant ;
- **Un paquet fermé** : scannez son étiquette, fenêtre **Paquet fermé**, **Désactiver le paquet**.

## Journal des opérations

Menu **Gestion > Journal des opérations** (rôles gestion et admin) : toutes les opérations, de la plus
récente à la plus ancienne, avec qui, quand, quoi et sur quel lot. Vérifs (complètes en vert,
incomplètes en rouge), réassorts, retraits vers le stock, scellages et ouvertures de scellé, réceptions,
paquets ouverts, suppressions d'items, et modifications des lots, des types, des utilisateurs et des
réglages.

- Filtres : **Qui**, **Quoi**, **Lot** (avec ses sous-lots, case à décocher), **Quand** (dates libres
  comme pour la péremption, ou **Aujourd'hui**, **7 jours**, **30 jours**) et un **texte** cherché dans
  le résumé (numéro de scellé, iid, nom de type...), le nom du lot et le nom de la personne.
- Cliquez une ligne pour son détail : items concernés, ce qui manquait à une vérif, raison d'une
  ouverture ou d'une suppression.
- Un nom en orange n'a pas été vérifié par un badge : nom déclaré, ou ouverture anonyme par l'étiquette
  intérieure jamais signée.
- Bouton **Journal du lot** dans la fiche d'un lot (Gestion des lots) : le journal filtré sur ce lot.

Les opérations faites avant la mise à jour sont reconstituées depuis l'historique (en gris) : vérifs,
réassorts, réceptions, paquets ouverts, et seulement le **dernier** scellage et la **dernière**
ouverture de chaque lot.

## Étiquettes

Menu **Administration > Éditeur d'étiquettes** (admin). Chaque modèle a un **usage** (item, paquet, lot
public, lot clé, scellé, ouverture du scellé, badge, étiquette de tâches) qui détermine les informations disponibles,
listées dans l'onglet **Placeholders** (`{{type_name}}`, `{{peremption}}`, `{{lot_name}}`…).
Un texte entre crochets autour du nom ne s'imprime que si la valeur n'est pas vide, devant
(`{[Exp. ]{peremption}}` donne « Exp. 31/12/2027 », ou rien pour un item non périssable), derrière
(`{{peremption}[ inclus]}`) ou des deux côtés (`{[(]{lot_name}[)]}`).

L'onglet **Toutes les étiquettes** montre en grille chaque étiquette du dossier des modèles avec son
aperçu. Celles qui servent de modèle (choisies dans **Réglages > Étiquettes et impression**) portent une
pastille verte **Modèle** et l'usage pour lequel elles sont choisies. Les autres, d'usage « Générique »,
ne servent qu'à être imprimées à la main (**Imprimer...**) : indications, autocollants, consignes… Un
clic sur l'aperçu ou **Modifier** l'ouvre dans l'onglet **Édition** ; **Nouvelle étiquette** en crée une.

- **Ajouter texte / QR / image (logo)**, positions et tailles en millimètres, texte en gras et
  aligné. **Ajouter le titre** insère `{{titre}}` (le nom de votre structure, défini dans les
  Réglages).
- Le QR code d'une étiquette **doit** contenir le placeholder prévu (`{{item_url}}`, `{{lot_url}}`…),
  sinon l'étiquette ne sera pas reconnue. L'étiquette de tâches contient `{{task_url}}` et peut
  afficher la liste `{{tasks}}` : elle remplace l'étiquette d'item pour les types qui ont des tâches. Un QR d'item qui ne contient que `{{iid}}` (ancien
  format) reste lu par les fronts, mais n'ouvre pas le front web avec l'appareil photo du téléphone. L'éditeur signale un QR trop petit pour être lu.
- Les images (logo PNG ou JPEG) se placent dans le dossier `images/` des modèles et se choisissent
  avec « Choisir une image du dossier ».
- **Enregistrer**, puis choisissez ce modèle pour son usage dans **Réglages > Étiquettes et
  impression**.

Le format du rouleau (largeur, hauteur) se règle dans les Réglages ; un modèle dessiné dans l'autre
sens est tourné automatiquement (**Retourner les étiquettes (180°)** si elles sortent à l'envers).

## Réglages du poste

Menu **Administration > Réglages** (admin) :

| Section | Contenu |
|---|---|
| Serveur | Adresse de l'API (`http://127.0.0.1:8001` sur la borne), **Tester la connexion** |
| Règles du serveur (tous les fronts) | **Autoriser les vérifs et les ajouts sans badge (nom déclaré)**, désactivé par défaut : voir ci-dessous |
| Session et affichage | **Machine partagée** (cochée par défaut ; décochée sur un poste personnel, l'utilisateur reste connecté jusqu'à « Se déconnecter », même après un redémarrage, et seul son PIN est redemandé à l'ouverture), taille du texte, réinitialisation après inactivité (15 min, machine partagée seulement), alerte péremption proche (30 jours), **Exiger l'étiquette clé pour valider une vérif** |
| Disposition par défaut | **Utiliser la disposition actuelle** : la position des fenêtres à laquelle le poste revient après inactivité |
| Étiquettes et impression | Rouleau, titre, imprimante, modèle par usage |
| Signal de mauvais scan | Bip, clignotement, LED et son de la douchette ; **Tester le signal d'erreur** |
| Notifications | **Alerte d'expiration** des étiquettes clés de lot et des badges (jours avant), voir ci-dessous |

La douchette se règle dans le menu **Douchette > Ouvrir les paramètres**. Tout le monde peut
appairer et connecter une douchette, même sans être connecté. Un utilisateur connecté règle le volume ;
le reste, mode inventaire compris, est réservé aux administrateurs. Pour un administrateur, la section
**Paramétrage** (mode clavier, modèle, réglages de la douchette, entretien) lit les réglages de la douchette et les affiche
par thème : volume, vibration, éclairage, mode de lecture, clavier de l'ordinateur, Entrée après chaque
code, types de codes lus, extinction automatique, mémoire… Chaque changement part tout de suite à la
douchette. Le **Modèle** est reconnu d'après le nom de la douchette (ex. BCST-75S) ; pour une version
AI ou un nom modifié, choisissez-le à la main, sinon les réglages ne peuvent pas être lus. Le volume (et
pour un administrateur le **mode inventaire**) se change aussi directement dans le menu **Douchette**.

En mode inventaire, la douchette garde les codes lus en mémoire (elle clignote en bleu au lieu de vert).
À l'activation du mode inventaire, la fenêtre **Mémoire de la douchette** s'ouvre avec le QR code à
scanner une fois les codes lus : la douchette envoie alors ses codes en Bluetooth (le « Data Upload » de
son manuel ; les commandes du SDK ne les envoient que sur le câble USB). **Lire la mémoire** rouvre ce QR
code, **Compter** affiche celui qui lui fait renvoyer son nombre de codes (« Total Counters = … »). Les
codes reçus s'affichent dans la fenêtre : **Traiter ces codes** les envoie dans la pile de scans,
**Traiter et supprimer les codes** vide en plus la mémoire de la douchette. Lire la mémoire ne la vide
pas ; **Vider la mémoire** l'efface, après confirmation.

Le volume de la douchette est propre à chaque utilisateur : chacun retrouve le dernier volume qu'il a
choisi dès qu'il se connecte, sur n'importe quel poste. Ceux qui n'ont rien choisi, et le poste sans
utilisateur connecté, ont le **Volume de la douchette par défaut** des règles du serveur
(**Réglages**, administrateurs).

### Sans badge : nom déclaré

Par défaut, valider une vérif, ajouter des items à un lot ou briser un scellé depuis la fiche exige un badge, sur le
poste comme au téléphone. Si vous cochez **Autoriser les vérifs et les ajouts sans badge**, la
personne qui n'a pas de badge peut saisir son nom à la place : **Continuer sans badge** dans la
fenêtre de connexion du poste, ou un appui sur **Non connecté** au téléphone (le nom y est gardé).
Ce nom n'est pas vérifié : l'historique l'affiche tel quel, distinct des personnes identifiées par
leur badge. L'étiquette clé du lot reste exigée. Le réglage est enregistré sur le serveur et
s'applique à tous les postes et à tous les téléphones.

## Notifications

Les alertes sont envoyées en notifications web (ci-dessous). Les notifications SMS (Free Mobile) ne
sont plus maintenues et ont été retirées des Réglages.

**Réglages > Notifications** du poste garde le champ **Alerte d'expiration** : nombre de jours
(30 par défaut) avant l'expiration d'une étiquette clé de lot (sauf les rangements du stock, qui
n'expirent pas) ou d'un badge pour l'alerte « expire bientôt », contrôlée chaque matin à 7 h 45 ; une
seconde alerte part à l'expiration. Renouveler l'étiquette ou le badge remet l'alerte à zéro.

### Notifications web (tous opérateurs)

Sur le téléphone d'un utilisateur gestion ou admin : scanner son badge, menu **⋯ > Notifications**
(ou le lien de la page État des stocks de l'accueil), choisir les alertes, **Activer les notifications** puis **Envoyer un
test**. Le même écran liste tous ses appareils abonnés : on y change les alertes de chacun, ou on le
retire (un téléphone perdu, par exemple).

- Gestion : stock bas, stock vide, commande à passer, ouverture d'un lot scellé (le clic ouvre la fiche du lot),
  résumé quotidien des lots contenant des périmés (7 h 45), vérif incomplète (désactivée par défaut,
  le clic ouvre la fiche du lot), renouvellement et expiration des étiquettes clés de lot.
- Admin : en plus, PIN bloqué, signalements, renouvellement et expiration des badges.
- Sur le poste, **Utilisateurs > Notifications web** : décochez un type pour le couper à cet
  utilisateur sur tous ses appareils. La fiche indique aussi combien d'appareils sont abonnés. Les alertes arrivent même page fermée.

- La demande d'autorisation du navigateur s'affiche au clic sur **Activer les notifications**. Si
  le navigateur refuse sans rien afficher, les notifications sont bloquées pour ce site (réglages du
  site, icône à gauche de l'adresse) ou, sur Android, pour l'application du navigateur elle-même
  (réglages du téléphone).
- Sur iPhone (iOS 16.4 ou plus), ajouter d'abord la page à l'écran d'accueil.
- La borne doit avoir un certificat HTTPS reconnu par le téléphone, et un accès Internet sortant.

## Sauvegardes

La borne fait une sauvegarde automatique chaque jour (base, configuration, réglages et modèles
d'étiquettes) et garde les 14 dernières. **Une fois par an au moins, vérifiez qu'elles existent** et,
idéalement, qu'elles sont copiées ailleurs (clé USB, NAS). Commandes et restauration : voir le
[guide d'installation](../installation/README.md).
