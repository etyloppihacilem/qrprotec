# Questions fréquentes et dépannage

## Scans

**La douchette ne se connecte pas (mode Bluetooth).**
Menu **Douchette > Rechercher**, puis, la première fois, choisissez-la dans **Douchette > Connecter à**
(ensuite, le logiciel s'y reconnecte seul après chaque recherche). La douchette ne doit **pas** être appairée dans les réglages Bluetooth
de l'ordinateur : si elle l'est, supprimez-la de la liste des appareils. Déconnectez aussi tout autre
appareil Bluetooth (casque, souris) pendant la recherche : le logiciel du fabricant échoue sinon,
sans message. À défaut, passez la douchette en **mode clavier (HID)** : elle fonctionne alors comme
un clavier, sans bip de retour d'erreur sur la douchette (c'est l'ordinateur qui bipe).

**Le début d'un code est coupé, ou un code s'écrit dans un champ de saisie (mode clavier).**
Augmentez le délai entre caractères dans **Douchette > Paramètres** (50 ms par défaut).

**« Code non reconnu ».**
L'étiquette ne vient pas de QRProtec, ou elle est abîmée. Pour un item, réimprimez l'étiquette
(onglet Items de l'inventaire, ou scannez le numéro écrit sous le QR à la main dans la pile).

**Un objet que je viens de scanner n'apparaît pas.**
Il était déjà dans la pile (mention discrète « déjà scanné ») : un doublon est ignoré.

**Le téléphone n'ouvre pas la caméra.**
La page doit être en `https://`. Avec un certificat interne (borne sur réseau local), il faut avoir
installé l'autorité de la borne sur le téléphone, ou accepter l'avertissement. Vérifiez aussi que le
navigateur a le droit d'utiliser la caméra.

## Vérifs

**Le sac est rouge alors que je viens de le vérifier.**
Lisez le compte rendu : il manque quelque chose, ou un périmé est resté dans le sac sans
remplaçant. Le contenu attendu a peut-être aussi été modifié par la logistique.

**Un objet est « attendu mais non scanné » alors qu'il est dans le sac.**
Il n'a pas été scanné (étiquette cachée, objet resté dans une poche). Refaites une vérif en le
scannant : il est retrouvé. Il ne devient « disparu » qu'après trois vérifs sans être vu (ou tout de
suite s'il est périmé).

**« Scannez l'étiquette privée du lot pour pouvoir valider ».**
L'étiquette publique ne suffit pas à valider. Scannez l'étiquette privée (à l'intérieur du sac), ou
demandez à un responsable.

**« Les lots d'une même vérif doivent appartenir au même lot global ».**
Vous avez scanné l'étiquette privée d'un autre sac sans rapport avec la vérif en cours. Terminez ou
annulez la vérif, puis commencez celle de l'autre sac.

**J'ai ajouté du matériel et le sac est passé orange.**
C'est voulu : un réassort sans vérif complète laisse le sac en « vérif recommandée » jusqu'à la
prochaine vérif complète.

**J'ai validé une vérif par erreur avec un sac à moitié scanné.**
Le compte rendu signale beaucoup de manquants. Refaites simplement une vérif complète : les objets
retrouvés sont réactivés.

## Connexion

**« Badge invalide ou expiré ».**
Le badge a plus d'un an ou a été renouvelé. Demandez un nouveau badge à un administrateur.

**« PIN bloqué ».**
Cinq PIN faux : attendez 5 minutes. PIN oublié : un administrateur en définit un nouveau
(**Gestion > Utilisateurs**).

**Le poste m'a déconnecté et a vidé la pile.**
Après 15 minutes sans activité, le poste revient à son état de départ (réglable par un admin).

## Impression

**L'imprimante n'imprime pas.**
Vérifiez qu'elle est allumée, branchée en USB, avec un rouleau et le capot fermé. En cas d'erreur, la
file d'impression se met en pause et l'indicateur d'impression passe en rouge dans la barre du haut :
cliquez dessus, puis **Reprendre l'impression** une fois le problème réglé, ou **Annuler les
impressions en attente**. Dans les Réglages,
**Imprimer test** vérifie la liaison.

**Les étiquettes sortent tournées ou à l'envers.**
Réglez la taille du rouleau dans **Réglages > Étiquettes et impression** (largeur dans le sens de la
tête d'impression), et cochez **Retourner les étiquettes (180°)** si besoin.

**Le QR code d'une étiquette ne se lit pas.**
Il est trop petit pour son contenu : agrandissez-le dans l'éditeur (qui affiche un avertissement), ou
baissez le niveau de correction.

## Borne

**L'écran est noir.**
L'écran s'éteint après 30 minutes d'inactivité : touchez une touche ou la souris. S'il reste noir,
`Ctrl+Alt+F2` ouvre une console ; un informaticien peut consulter
`journalctl -u qrprotec-kiosk -b`.

**« API hors ligne » en haut de l'écran du poste.**
Le back ne répond pas. Redémarrez la borne ; si cela persiste, voir le
[guide d'installation](../installation/README.md) (journaux).

**Le téléphone affiche « Serveur injoignable ».**
Le téléphone n'est pas sur le bon réseau, ou la borne n'est pas joignable depuis Internet. Vérifiez
l'adresse de la page.
