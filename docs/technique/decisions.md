# Choix de conception

Journal des décisions qui structurent le code. Pour chacune : le contexte, ce qui a été choisi, ce qui
a été écarté et pourquoi. Avant de « simplifier » un de ces points, relire la raison : la plupart
répondent à une contrainte de terrain (bénévoles, local sans informaticien, réseau incertain,
matériel de secours qui doit être prêt).

Ajouter une entrée à chaque décision structurante (format libre, mais garder contexte / choix /
raison).

---

## Les fronts envoient des scans, le serveur décide

**Contexte.** Deux fronts (poste et téléphone) font les mêmes opérations.
**Choix.** Les fronts envoient la **liste des identifiants scannés** ; toute la logique (où va un
item, ce qui manque, ce qui est remplacé, scellés, notifications) est dans `services.py`.
**Écarté.** Des fronts qui envoient des mouvements (« déplacer X vers Y »). Ils auraient dupliqué les
règles dans deux langages et laissé l'état diverger.
**Conséquence.** Les seules règles dupliquées sont celles de l'aperçu (répartition entre sous-lots,
vérif partielle), à garder synchrones (voir [regles-de-gestion.md](regles-de-gestion.md)).

## Deux API sur deux ports

**Contexte.** Le poste de la borne doit tout pouvoir faire sans friction ; les téléphones, sur
Internet, ne doivent pouvoir faire que des vérifs et des réassorts sur les lots dont ils ont
l'étiquette privée.
**Choix.** Une seule application Django, deux serveurs : l'API locale (gestion sans clé) n'écoute que
sur la boucle locale et n'est jamais derrière le reverse proxy ; l'API publique est la seule exposée.
Le rôle est attaché par le serveur qui reçoit la requête, pas par l'URL.
**Écarté.** Un préfixe d'URL `/admin-api/` filtré par le proxy (une erreur de configuration du proxy
exposerait tout) ; des comptes et mots de passe pour le poste (à saisir sur une borne partagée, à
renouveler, à perdre).
**Raison.** La frontière de sécurité est le réseau : une route de gestion n'existe tout simplement pas
sur le port public (404), quel que soit le proxy. Le jeton optionnel `QRPROTEC_LOCAL_API_TOKEN` et la
liste d'adresses ajoutent une défense en profondeur.

## Fronts distants : une troisième API et une clé par front

**Contexte.** Il faut pouvoir installer le back sur un serveur et des postes ailleurs, sans casser
le cas de la borne (front et back sur la même machine, API locale jamais exposée), et avec des
fronts locaux et distants en même temps.
**Choix.** Une API **distante** (port 8002) qui sert les mêmes routes que l'API locale, joignable
uniquement via Caddy, et qui exige une **clé par front** (`X-QRProtec-Key`). Caddy oriente selon la
présence de l'en-tête. Le front choisit son API par son URL (`http://127.0.0.1:8001` ou
`https://serveur`).
**Écarté.** Exposer l'API locale avec un jeton partagé (un seul secret pour tous les postes, non
révocable individuellement) ; des comptes utilisateur sur l'API (les postes sont partagés, c'est la
machine qu'on autorise, l'utilisateur est identifié par son badge).
**Raison.** Le comportement par défaut ne change pas, chaque poste se révoque seul
(`frontkey revoke`), et l'API locale reste la frontière de confiance de la borne.

## Étiquette publique, étiquette privée

**Contexte.** Un sac est posé dans un véhicule ; n'importe qui peut photographier son étiquette.
**Choix.** Chaque lot a deux étiquettes : **publique** (consultation : état, contenu) collée à
l'extérieur, et **privée** (contient la clé, autorise vérif et réassort) rangée à l'intérieur ou
gardée par les responsables. La clé se régénère (« Régénérer la clé ») si l'étiquette privée est
perdue ; l'ancienne devient invalide.
**Raison.** Sans compte utilisateur sur le téléphone, la possession physique de l'étiquette privée
est l'autorisation. C'est simple à expliquer et robuste.

## Les QR codes sont des URLs, le domaine est ignoré

**Choix.** Les étiquettes de lot, badge, paquet et scellé contiennent une URL du front web ; les
fronts n'en lisent que le chemin et les paramètres.
**Raison.** Avec l'appareil photo natif d'un téléphone, le QR ouvre directement la bonne page, sans
application à installer. Et un changement de nom de domaine (ou d'adresse IP) n'oblige pas à
réimprimer des centaines d'étiquettes.

## L'identifiant d'un item contient son type et sa péremption

**Choix.** `iid = type(6) + AAAAMMJJ + compteur base 62 (8)`.
**Raison.** Le poste reconnaît un item **et signale un périmé instantanément, sans réseau** (bip à la
douchette avant même la réponse du serveur). L'identifiant reste court (QR lisible sur 20 mm).
**Prix.** La date est immuable : une erreur de date se corrige en supprimant l'item (raison
obligatoire) et en en recevant un nouveau.

## On ne supprime rien ; « disparu » plutôt qu'effacé

**Choix.** Un item non scanné à une vérif n'est pas retiré du lot : son compteur `missed_verifs`
augmente et il devient « disparu » seulement s'il est périmé ou après 3 vérifs manquées
(`QRPROTEC_MISSING_AFTER_VERIFS`). La suppression manuelle exige une raison et se restaure. Les
exigences d'un lot ne comptent que les items vus à la dernière vérif.
**Raison.** Une vérif faite vite, un item resté au fond du sac : le logiciel ne doit pas « perdre »
du matériel sur une erreur ponctuelle, mais il ne doit pas non plus considérer un sac comme complet
grâce à un item que personne n'a vu. L'historique complet (`VerifItem`) reste disponible.

## Remplacement implicite des périmés

**Choix.** Pendant une vérif, scanner un périmé **et** un neuf du même type qui arrive dans le lot
sort automatiquement le périmé (« remplacé »).
**Raison.** C'est le geste réel du secouriste. Lui demander une opération « remplacer » séparée
aurait été oublié une fois sur deux.

## Sous-lots comme vrais lots

**Contexte.** Un B+ est un sac de soin + un sac O2 ; un VPS contient des armoires et un B+. Il faut
pouvoir vérifier le sac O2 seul.
**Options présentées.** (1) un sous-lot est un vrai lot avec un parent ; (2) des « compartiments »
à l'intérieur d'un lot. **Choix du responsable du projet : option 1** (`Lots.parent`).
**Raison.** Chaque sac garde ses étiquettes, son historique et son état, se vérifie seul ou avec les
autres ; l'état du lot global se déduit (pire état, plus ancienne vérif). La clé d'un lot couvre ses
sous-lots, pour qu'une seule étiquette privée suffise à vérifier tout un B+.
**Non fait.** Les sous-lots attendus par **type** de lot (aujourd'hui l'arborescence se construit à
la main, lot par lot).

## Rangements du stock = lots de type « rangement »

**Choix.** Une armoire ou un tiroir est un lot dont le type a `storage=True` ; ses items comptent
dans le stock.
**Écarté.** Un champ « emplacement » libre sur les items (pas vérifiable, pas d'étiquette).
**Raison.** Réutiliser tout le mécanisme des lots (étiquettes, vérif d'un tiroir sans vérifier tout
le stock) au lieu d'en inventer un second.

## Réassort sans vérif → « vérif recommandée »

**Choix.** On peut ajouter des items à un lot sans le vérifier entièrement ; le lot passe orange
jusqu'à la prochaine vérif, avec qui/quand/combien.
**Raison.** Sur le terrain on complète un sac au retour d'intervention sans avoir le temps de tout
recompter. Il fallait que ce soit possible **et** visible.

## Scellés

**Choix.** Un lot complet peut être scellé ; il est alors valide sans vérif jusqu'à la première
péremption, et toute opération sur lui ou ses sous-lots brise le scellé (code changé, SMS).
**Raison.** Les lots scellés (lots de réserve, malles catastrophe) ne doivent pas être ouverts pour
être vérifiés ; le scellé physique numéroté fait foi.

## Paquets fermés

**Choix.** Les items d'une boîte fermée sont créés à la réception (stock juste), mais leurs étiquettes
ne sont imprimées qu'à l'ouverture ; l'étiquette du paquet compte pour tous ses items.
**Raison.** Ne pas étiqueter 25 compresses dans une boîte qu'on n'ouvrira peut-être pas avant un an.

## Identités en texte (`M:matricule` / `D:nom`)

**Choix.** Les champs « par qui » sont des chaînes, pas des clés étrangères.
**Raison.** L'historique survit à la suppression d'un utilisateur, et l'API publique peut accepter une
identité déclarée tout en la distinguant d'une identité vérifiée.

## Badge + PIN, pas de comptes Django

**Choix.** Le badge (QR avec clé, valable un an) identifie ; le PIN (obligatoire pour les admins)
authentifie ; un jeton signé de 12 h évite de redemander le PIN sur le téléphone.
**Raison.** Sur une borne, on scanne son badge : pas de clavier, pas de mot de passe à retenir. Le
PIN empêche qu'un badge d'admin perdu suffise à tout faire.

## SQLite

**Raison.** Quelques milliers d'items, une poignée d'utilisateurs simultanés, un seul serveur. Un
fichier unique se sauvegarde à chaud (API de sauvegarde de SQLite) et se restaure en le copiant.
Aucun serveur de base à administrer. Passer à PostgreSQL ne demanderait que `DATABASES` (le code
n'utilise que l'ORM), si un jour plusieurs serveurs devaient partager une base.

## Peu de dépendances

**Choix.** Back : Django + DRF (+ `cryptography` optionnelle). Poste : ImGui, GLFW, libpng, FreeType ;
HTTP, WebSocket et JSON sont implémentés dans `app/src/net` et `app/src/core`. Back : relais
WebSocket et Web Push écrits à la main.
**Raison.** Le projet doit pouvoir être repris dans cinq ans par une seule personne. Chaque
dépendance est une mise à jour à suivre, une API qui change, un paquet qui disparaît. Les protocoles
réimplémentés sont petits, testés (vecteurs des RFC) et figés.
**Exception.** Le TLS du poste (front distant) passe par OpenSSL : réimplémenter du chiffrement
n'aurait aucun sens, et OpenSSL est présent partout et mis à jour par le système.

## Front poste en ImGui

**Raison.** Démarrage instantané, aucune dépendance de boîte à outils graphique, fonctionne sous un
compositeur kiosk minimal (Cage), état de l'interface entièrement dans le code. Le rendu à la demande
(60 images/s pendant 2 s après une entrée, ~10 au repos) évite de consommer un cœur en permanence.

## Front web sans framework

**Raison.** Pas de CDN (la borne peut être sans Internet), pas de chaîne de construction npm qui
vieillit, un fichier lisible. `BarcodeDetector` natif quand il existe, jsQR fourni sinon.

## Borne Fedora en RPM, Cage, Caddy

**Raison.** Mises à jour par `dnf upgrade`, configuration préservée, services systemd durcis,
SELinux. Cage affiche une seule application plein écran sans bureau ni écran de connexion. Caddy
obtient et renouvelle seul les certificats (Let's Encrypt, ou autorité locale sur un réseau privé).
Django est **embarqué** aux versions de `poetry.lock` pour qu'une mise à jour du système ne change pas
la version de Django sous la borne.

## Mode clavier (HID) en plus du SDK

**Contexte.** Le SDK Bluetooth d'Inateck ne fonctionnait pas avec le firmware de certaines
douchettes, et il est propriétaire (x86_64 seulement).
**Choix.** Supporter aussi la douchette en mode clavier, reconnue à la vitesse de frappe.
**Raison.** Le poste reste utilisable avec n'importe quelle douchette du commerce.

## Notifications SMS Free Mobile, puis web

**Raison.** L'API SMS de Free est gratuite et triviale pour un titulaire de ligne Free ; les
notifications web couvrent tous les autres opérateurs sans service payant. Une alerte par passage
sous le seuil (pas une par heure).

## Modèles d'étiquettes en JSON, versionnés dans le dépôt

**Raison.** Les étiquettes sont propres à chaque structure (logo, titre, taille du rouleau). Un format
texte se relit, se compare et se commite ; la borne copie les modèles fournis sans écraser ceux
modifiés localement.
