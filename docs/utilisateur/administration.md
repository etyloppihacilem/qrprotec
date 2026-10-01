# Administration

Cette page concerne le rôle **admin** (sauf « Lots » et « Scellés », accessibles au rôle gestion).

## Utilisateurs et badges

Menu **Gestion > Utilisateurs**.

- **Nouvel utilisateur** : matricule (1 à 16 lettres, chiffres, `-` ou `_`, définitif), prénom, nom,
  rôle, PIN éventuel. **Créer et voir le badge** affiche l'aperçu ; imprimez-le.
- **Rôle** : Secouriste, Gestion ou Administrateur (voir [les notions](notions.md#badge-pin-et-rôles)).
  Gardez au moins **deux administrateurs**.
- **Badge perdu ou expiré** (validité un an) : **Renouveler (1 an)** puis imprimer le nouveau badge.
  L'ancien badge ne fonctionne plus immédiatement.
- **Départ d'un bénévole** : décochez **Compte actif**. Son nom reste dans l'historique des vérifs.
- **PIN** : **Définir le PIN** / **Supprimer le PIN** (impossible pour un admin). Un admin sans PIN le
  choisit à sa prochaine connexion. Après 5 PIN faux, le compte est bloqué 5 minutes.

Plus aucun administrateur ne peut se connecter ? Sur le serveur :
`sudo qrprotec-manage createadmin M001 Nom Prénom --pin 4821` (crée le compte ou donne un nouveau
badge et le rôle admin à un compte existant).

## Lots

Menu **Gestion > Gestion des lots** (rôle gestion).

- **Types de lots** : contenu attendu (type d'item, quantité, emplacement), case **Rangement du
  stock** pour les armoires et tiroirs.
- **Nouveau lot** : type, nom, nom court (16 caractères, pour les petites étiquettes), lot parent
  (« Aucun : lot indépendant » ou le lot global). Aperçu et impression des étiquettes publique et
  privée.
- **Fiche d'un lot** : modifier le nom ou le parent, réimprimer les étiquettes (**publique**,
  **privée**, **les deux**), **Archiver le lot** quand il est réformé (il disparaît des listes ; il
  se réactive).
- **Régénérer la clé** : si l'étiquette privée est perdue ou a pu être copiée. L'ancienne étiquette
  privée ne fonctionne plus ; imprimez la nouvelle. Une clé est valable 10 ans par défaut ; la date
  d'expiration est affichée dans la fiche.

## Scellés

Dans la fiche d'un lot (rôle gestion) :

1. Faites d'abord une **vérif complète** : un lot (et ses sous-lots) incomplet ou contenant des
   périmés ne peut pas être scellé, sauf **Sceller quand même**.
2. Saisissez le **numéro du scellé** physique, **Sceller le lot et imprimer l'étiquette**.
3. Posez le scellé et collez l'étiquette du scellé à côté.

**Briser le scellé** depuis la fiche quand on ouvre le lot pour autre chose qu'une vérif. Le lot
devra être vérifié avant utilisation.

## Étiquettes

Menu **Gestion > Éditeur d'étiquettes** (admin). Chaque modèle a un **usage** (item, paquet, lot
public, lot privé, scellé, badge) qui détermine les informations disponibles, listées dans l'onglet
**Placeholders** (`{{type_name}}`, `{{peremption}}`, `{{lot_name}}`…).

- **Ajouter texte / QR / image (logo)**, positions et tailles en millimètres, texte en gras et
  aligné. **Ajouter le titre** insère `{{titre}}` (le nom de votre structure, défini dans les
  Réglages).
- Le QR code d'une étiquette **doit** contenir le placeholder prévu (`{{iid}}`, `{{lot_url}}`…),
  sinon l'étiquette ne sera pas reconnue. L'éditeur signale un QR trop petit pour être lu.
- Les images (logo PNG ou JPEG) se placent dans le dossier `images/` des modèles et se choisissent
  avec « Choisir une image du dossier ».
- **Enregistrer**, puis choisissez ce modèle pour son usage dans **Réglages > Étiquettes et
  impression**.

Le format du rouleau (largeur, hauteur) se règle dans les Réglages ; un modèle dessiné dans l'autre
sens est tourné automatiquement (**Retourner les étiquettes (180°)** si elles sortent à l'envers).

## Réglages du poste

Menu **Gestion > Réglages** (admin) :

| Section | Contenu |
|---|---|
| Serveur | Adresse de l'API (`http://127.0.0.1:8001` sur la borne), **Tester la connexion** |
| Session et affichage | Taille du texte, réinitialisation après inactivité (15 min), alerte péremption proche (30 jours), **Exiger l'étiquette privée pour valider une vérif** |
| Disposition par défaut | **Utiliser la disposition actuelle** : la position des fenêtres à laquelle le poste revient après inactivité |
| Étiquettes et impression | Rouleau, titre, imprimante, modèle par usage |
| Signal de mauvais scan | Bip, clignotement, LED et son de la douchette ; **Tester le signal d'erreur** |
| Notifications SMS | Voir ci-dessous |

La douchette se règle dans le menu **Douchette** (recherche, connexion, mode clavier, volume…).

## Notifications

### SMS (abonnés Free Mobile)

**Réglages > Notifications SMS** :

1. Chaque destinataire active « Notifications par SMS » dans son espace abonné Free Mobile, qui lui
   donne un **identifiant** et une **clé**. (L'API de Free n'envoie qu'au titulaire de la ligne.)
2. **Ajouter un destinataire** avec ces deux valeurs, puis **SMS de test**.
3. Cochez **Activer les notifications SMS** et les événements :
   - **stock bas** : un SMS quand un type passe sous son minimum (un seul par passage) ;
   - **vérif incomplète** : manquants, périmés, disparus ;
   - **scellé brisé** ;
   - **résumé quotidien des périmés** (chaque matin à 7 h 45).

Le dernier statut d'envoi de chaque destinataire est affiché.

### Notifications web (tous opérateurs)

Sur le téléphone d'un administrateur : scanner son badge, onglet **Stock**, **Activer les
notifications**, puis choisir **stock bas** et/ou **stock vide**, et **envoyer une notification de
test**. Les alertes arrivent même page fermée.

- Sur iPhone (iOS 16.4 ou plus), ajouter d'abord la page à l'écran d'accueil.
- La borne doit avoir un certificat HTTPS reconnu par le téléphone, et un accès Internet sortant.

## Sauvegardes

La borne fait une sauvegarde automatique chaque jour (base, configuration, réglages et modèles
d'étiquettes) et garde les 14 dernières. **Une fois par an au moins, vérifiez qu'elles existent** et,
idéalement, qu'elles sont copiées ailleurs (clé USB, NAS). Commandes et restauration : voir le
[guide d'installation](../installation/README.md).
