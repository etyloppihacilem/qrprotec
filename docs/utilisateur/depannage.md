# Questions fréquentes et dépannage

## Scans

**La douchette ne se connecte pas (mode Bluetooth).**
La première fois : menu **Douchette > Appairer une douchette**, puis choisissez-la dans **Douchette > Connecter à**.
Ensuite, le logiciel s'y reconnecte seul : tant qu'elle n'est pas connectée (base éteinte puis rallumée,
douchette hors de portée), il réessaie toutes les 15 secondes. **Douchette > Reconnecter la douchette**
réessaie tout de suite. La douchette ne doit **pas** être appairée dans les réglages Bluetooth
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

**Il n'y a pas de réseau dans la réserve.**
La vérif se fait quand même sur le téléphone : la mention « Hors connexion » s'affiche et la vérif est
envoyée dès le retour du réseau. L'appli s'ouvre même sans réseau (étiquette scannée avec l'appareil photo,
ou icône de l'écran d'accueil), à condition de l'avoir ouverte une fois avec du réseau sur ce téléphone et
de s'être connecté avant d'entrer dans la réserve. La fiche affichée est celle de la dernière mise à jour.
Tant que la vérif n'est pas envoyée, un bandeau orange le rappelle sur l'accueil : allez là où il y a du réseau
(wifi du local, 4G). Une fois envoyée, si quelque chose a changé entre-temps (matériel manquant, nouveau
signalement, contenu du lot modifié), QRprotec l'affiche et propose de reprendre la vérif.

**Le sac est rouge alors que je viens de le vérifier.**
Lisez le compte rendu : il manque quelque chose, ou un périmé est resté dans le sac sans
remplaçant. Le contenu attendu a peut-être aussi été modifié par la logistique.

**Un objet est « attendu mais non scanné » alors qu'il est dans le sac.**
Il n'a pas été scanné (étiquette cachée, objet resté dans une poche). Refaites une vérif en le
scannant : il est retrouvé. Il ne devient « disparu » qu'après trois vérifs sans être vu (ou tout de
suite s'il est périmé).

**« Scannez l'étiquette de B+ (ou celle de Sac de soin ou de Sac O2) pour pouvoir valider ».**
La vérif a été lancée sans scanner d'étiquette (depuis la liste des lots, ou par un item). Le message
donne le nom du lot et de ceux qu'il contient : scannez l'étiquette publique ou clé de l'un d'eux, elle
montre que c'est bien ce lot.

**« Scannez l'étiquette clé du lot pour pouvoir valider ».**
Sans badge (nom déclaré), l'étiquette publique ne suffit pas. Scannez votre badge, ou l'étiquette
clé (à l'intérieur du sac).

**« Les lots d'une même vérif doivent appartenir au même lot global ».**
Vous avez scanné l'étiquette clé d'un autre sac sans rapport avec la vérif en cours. Terminez ou
annulez la vérif, puis commencez celle de l'autre sac.

**J'ai ajouté du matériel et le sac est passé orange.**
C'est voulu : un réassort sans vérif complète laisse le sac en « vérif recommandée » jusqu'à la
prochaine vérif complète.

**J'ai validé une vérif par erreur avec un sac à moitié scanné.**
Le compte rendu signale beaucoup de manquants. Refaites simplement une vérif complète : les objets
retrouvés sont réactivés.

## Connexion

**« Badge invalide ou expiré ».**
Le badge a plus d'un an ou a été renouvelé. Demandez un nouveau badge au TECH.

**« PIN bloqué ».**
Cinq PIN faux : attendez 5 minutes. PIN oublié : le TECH en définit un nouveau,
ou touchez **Code oublié ?** sous la saisie du PIN sur le téléphone.
Votre PIN est alors bloqué et l'écran de déblocage s'affiche, comme ci-dessous.

**« Code PIN bloqué après trop d'essais ».**
Après 50 PIN faux, seul le TECH peut débloquer. Sur le téléphone, prenez une photo de
l'écran (QR code et lien) et envoyez-la au TECH (ou à la personne indiquée). Il scanne le QR code, se
connecte et touche **Réinitialiser le PIN**. À votre prochaine connexion, choisissez un nouveau PIN.
Si les notifications sont activées, le TECH reçoit déjà le lien (une seule fois).

**Se connecter avec une passkey.**
À votre première connexion sur votre téléphone (carte, ou QR code de dépannage du TECH), un écran
propose d'abord de créer une passkey, puis de choisir votre code PIN s'il est obligatoire : ensuite, **Se connecter avec une passkey** suffit, avec l'empreinte, le visage ou
le code du téléphone. Une fois connecté avec la passkey, le téléphone la propose d'abord à chaque
connexion suivante (annulez pour scanner votre carte). Sur le web, le nom et le code PIN ne marchent
alors plus pour vous ; votre carte, si.
Au poste, le nom et le code PIN marchent toujours.
Téléphone perdu ou changé : le TECH supprime la passkey (poste, Utilisateurs), le nom et le PIN
remarchent.

**Me connecter au poste sans carte (passkey).**
Connecté au téléphone avec votre passkey, touchez le petit bouton QR code à côté de votre prénom sur
l'accueil et scannez le QR code avec la douchette du poste. Il vaut 5 minutes, une seule fois.

**Le poste m'a déconnecté et a vidé la pile.**
Après 15 minutes sans activité, le poste revient à son état de départ (réglable par le TECH).

## Impression

**L'imprimante n'imprime pas.**
Vérifiez qu'elle est allumée, branchée en USB, avec un rouleau et le capot fermé. En cas d'erreur, la
file d'impression se met en pause et l'indicateur d'impression passe en rouge dans la barre du haut :
cliquez dessus, puis **Reprendre l'impression** une fois le problème réglé, ou **Annuler les
impressions en attente**. Dans les Réglages,
**Imprimer test** vérifie la liaison. Avec une imprimante TSPL ou ZPL, la file s'arrête d'elle-même
avec la raison (capot ouvert, plus d'étiquettes, plus de ruban, pause) ; **Maintenance de
l'imprimante > Lire l'état** la redemande. Voyants, calibrage et alignement : voir
[L'imprimante d'étiquettes](imprimante.md).

**Des étiquettes blanches sortent entre deux impressions, ou le texte déborde sur la suivante.**
L'imprimante ne connaît pas la taille du rouleau : **Réglages > Maintenance de l'imprimante >
Calibrer sur les étiquettes chargées**. Si cela ne suffit pas (imprimante réglée pour autre chose
auparavant) : **Réglages d'usine**, puis calibrer. Si l'impression est seulement décalée de 1 ou 2
mm, imprimez l'**étiquette d'étalonnage** et réglez les décalages.

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

**Le wifi du local ne marche plus (panne de fibre, de box…).**
La borne peut diffuser son propre wifi pour quelques jours. Le TECH, en mode gestion, ouvre
**TECH › Wifi de secours…** et touche **Démarrer le wifi de secours** (jusqu'à une minute). Un voyant
bleu **● Wifi de secours** apparaît en haut de l'écran : n'importe qui le touche pour afficher les QR codes.
Sur chaque téléphone :

1. coupez les **données mobiles** (sinon le téléphone cherche QRprotec par la 4G et ne le trouve pas) ;
2. scannez le premier QR code avec l'appareil photo pour rejoindre le wifi « QRprotec » ;
3. ouvrez QRprotec comme d'habitude (ou scannez le second QR code). Si le téléphone signale que ce wifi
   n'a pas internet, répondez de rester connecté.

Le serveur étant sur la borne, tout marche comme avant, sans internet. Quand le wifi du local remarche,
le TECH touche **Arrêter le wifi de secours** : la borne reprend son réseau habituel. Le wifi de secours
s'arrête aussi au redémarrage de la borne. Il faut une carte wifi capable de diffuser un réseau (une clé
USB wifi suffit) ; sinon le poste affiche pourquoi il ne démarre pas.

**J'ai oublié ma carte.**
Demandez au TECH (un administrateur) : dans l'outil de debug des codes, il touche votre nom sous
« Cartes des secouristes » et vous montre un QR code. Scannez-le avec votre téléphone (dans QRprotec ou avec
l'appareil photo) : vous êtes connecté pour 24 h, sans carte ni code PIN. Le QR code ne vaut qu'une heure.
