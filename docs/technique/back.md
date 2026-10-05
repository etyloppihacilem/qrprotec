# Back (Django)

Dossier `database/`. Projet Django `qrprotecDB`, une seule application `inventory`. Python ≥ 3.13,
Django 6.1, Django REST Framework 3.18 (versions figées par `poetry.lock` à la racine).

## Organisation

| Fichier | Contenu | Règle |
|---|---|---|
| `qrprotecDB/settings.py` | Réglages Django et dictionnaire `QRPROTEC` (toute la configuration propre au projet, lue depuis l'environnement) | Toute nouvelle option passe par une variable `QRPROTEC_*` ici, documentée dans le README racine et dans `packaging/files/qrprotec.conf`. |
| `qrprotecDB/urls.py`, `urls_local.py` | Jeux d'URLs de l'API publique et de l'API locale | Ne contiennent que des `include` des listes de `inventory/urls.py`. |
| `inventory/urls.py` | `public_patterns`, `local_patterns`, `web_patterns` | Voir « Routes ». |
| `inventory/views.py` | Vues DRF (`@api_view`) | Fines : parser, contrôler l'accès, appeler `services`, sérialiser. |
| `inventory/services.py` | Logique métier | Tout ce qui modifie plusieurs objets, dans une transaction. |
| `inventory/serializers.py` | Modèles → `dict` JSON, états des lots | Fonctions simples, pas de `Serializer` DRF (plus lisible, et les sorties ne sont pas symétriques des entrées). |
| `inventory/middleware.py` | `ApiRoleMiddleware`, `RoleWSGIHandler` | Sépare les trois API (publique, locale, distante). |
| `inventory/idendity.py` | Identités `M:` / `D:` | (sic, le nom du fichier a une coquille historique ; le renommer casserait les imports pour rien) |
| `inventory/notifications.py` | SMS Free Mobile, seuils de stock | |
| `inventory/webpush.py` | Notifications web (chiffrement RFC 8291, VAPID RFC 8292) | Sans `cryptography`, désactivé proprement. |
| `inventory/remote_scanner.py` | Relais WebSocket téléphone → poste | Protocole décrit en tête de fichier. |
| `inventory/web_views.py`, `web/` | Front web | Liste blanche des fichiers servis. |
| `inventory/management/commands/` | `serve`, `createadmin`, `check_alerts` | |
| `inventory/tests.py` | Tests | `python manage.py test inventory` |

## Les trois API

### Sélection par le port

`manage.py serve` (voir `management/commands/serve.py`) démarre **trois serveurs WSGI** (le serveur de
développement de Django, multi-thread, durci) dans le même processus :

- public : `--public 0.0.0.0:8000` par défaut (`127.0.0.1:8000` sur la borne, derrière Caddy) ;
- local : `--local 127.0.0.1:8001` ;
- distant : `--remote 127.0.0.1:8002` (`--no-remote` pour ne pas le lancer), derrière Caddy.

Chacun enveloppe l'application dans `RoleWSGIHandler(app, 'public'|'local'|'remote')`, qui écrit le rôle dans
`environ['qrprotec.role']`. `ApiRoleMiddleware` le lit :

- rôle `local` : contrôle l'adresse cliente (`LOCAL_API_ALLOWED_ADDRESSES`, `*` pour tout accepter)
  et le jeton (`X-QRProtec-Token` si `LOCAL_API_TOKEN` est défini), puis
  `request.urlconf = 'qrprotecDB.urls_local'` et `request.qrprotec_local = True` ;
- rôle `remote` : exige une clé de front valide dans `X-QRProtec-Key`
  (`middleware.authenticate_front`, `FrontKey.authenticate`), sinon 401 ; puis **les mêmes URLs que
  l'API locale sans l'admin Django** (`urls_remote.py`, `request.qrprotec_local = True`), avec
  `request.qrprotec_front` = nom du front. L'adresse notée est celle de `X-Forwarded-For` (posé par
  Caddy) ;
- sinon : URLs publiques.

**Utilisateur connecté.** Ni le localhost ni la clé de front ne donnent de droits de gestion :
ils identifient la machine. Après le badge (et le PIN), `POST /api/auth/` renvoie un jeton de session
signé (12 h) que le front renvoie dans l'en-tête `X-QRProtec-Session`. Le serveur en déduit
l'utilisateur et son rôle (`views.front_user`, `views.require_front`) :

| Qui | Droits sur l'API locale ou distante |
|---|---|
| Personne de connecté | Lectures du kiosk (lots **sans** leurs clés, types), routes publiques (clé de lot exigée), téléphone-douchette, création du **premier** administrateur s'il n'y en a aucun |
| Secouriste | + vérifs et réassorts sans clé de lot, sous son identité |
| Gestion | + inventaire, réception, stock, paquets, lots et types, clés des lots |
| Admin | + utilisateurs, notifications SMS |

L'identité des opérations vient de la session : le `"user": "M0042"` envoyé par le poste est ignoré.
Un badge renouvelé ou désactivé, ou un PIN bloqué, invalide la session ; le front redemande alors le
badge (`login_required` dans la réponse 403).

Une requête qui n'est pas passée par `serve` (tests, `runserver`, WSGI brut) prend
`QRPROTEC_DEFAULT_API_ROLE` (`public` par défaut). Pour un déploiement WSGI classique,
`qrprotecDB.wsgi` expose `public_application`, `local_application` et `remote_application` (mais les
WebSockets du téléphone ne fonctionnent qu'avec `serve`). Le WebSocket `/ws/scanner/front` est
accepté sur l'API locale (contrôle d'adresse et de jeton) ou distante (clé de front).

Pourquoi ce choix : [decisions.md](decisions.md#deux-api-sur-deux-ports).

### Routes

`public_patterns` sont servies par toutes les API ; sur l'API publique, toute écriture exige une clé.
`local_patterns` ne sont servies que par l'API locale, et **doivent précéder** les routes publiques
quand un préfixe est partagé (`items/batch/` avant `items/<iid>/`, `lots/summary/` avant
`lots/<id>/`). La liste complète et à jour est dans `inventory/urls.py` ; le README racine la résume.

Conventions des réponses :

- JSON uniquement (`JSONRenderer`, `JSONParser`) ; pas d'authentification DRF ni de CSRF (pas de
  cookie de session, les accès sont contrôlés par clé, badge ou port).
- Erreur : `{"error": "message lisible en français", ...}` avec le bon code HTTP. Les vues sont
  décorées par `@handle_errors`, qui transforme `ApiError` (code et champs supplémentaires, par
  exemple `pin_required`) et `ValueError` (400). Les fronts affichent `error` tel quel : écrire des
  messages pour l'utilisateur final.
- Dates : `AAAA-MM-JJ` ; horodatages en heure locale ISO (`serializers._date`).

### Identifier l'utilisateur d'une opération

`views.operation_identity(request)`, qui appelle `idendity.identity_from_request(data, front_user,
allow_declared)` :

- poste (API locale ou distante) : l'utilisateur de la session (`X-QRProtec-Session`), jamais le
  matricule envoyé dans `user` ;
- API publique : `"user": {"matricule": "M0042", "key": "…"}`, la clé du badge est vérifiée ;
- à défaut, `"name"` donne une identité déclarée, **seulement** si le réglage du serveur
  `ServerSettings.declared_identity` est activé (désactivé par défaut, route admin
  `GET/PATCH /api/server-settings/`, case « Règles du serveur » des Réglages de l'app) ;
- sinon : 403 avec `login_required: true` et `declared_identity` (le réglage), pour que le front
  redemande le badge, ou le nom si le réglage l'autorise.

Les routes concernées sont celles qu'on peut appeler sans utilisateur connecté, avec la clé du lot :
`lots/<id>/verif/`, `verifs/`, `lots/<id>/add/` et `lots/<id>/unseal/`. `GET /api/health/` expose
`declared_identity` à tous les fronts (web compris), qui ne proposent la saisie du nom que s'il est vrai.
Exception : `lots/<id>/seal-open/` (étiquette d'ouverture du scellé) ouvre le scellé même sans identité,
puisque le code de l'étiquette rangée dans le lot prouve l'ouverture ; l'identité, si elle est donnée,
signe l'ouverture.

Les lectures réservées sur l'API publique (`lots/summary/`, `stock/summary/`, `push/*`) utilisent
`views.badge_user(request, roles)` : badge valide, rôle, et jeton de session si l'utilisateur a un PIN.

### Clés de lot sur l'API publique

`views.require_lot_key` (écritures sur un lot) et `views.verif_targets` (vérifs) : la clé envoyée
doit être celle du lot **ou d'un de ses ancêtres** (l'étiquette privée d'un lot global couvre ses
sous-lots), non expirée. Sur l'API locale, aucune clé n'est demandée.

## Services

Voir [regles-de-gestion.md](regles-de-gestion.md) pour le détail fonctionnel. Points techniques :

- `perform_verif` verrouille les items scannés et ceux du périmètre (`select_for_update(of=('self',))`)
  puis fait des `bulk_update` / `bulk_create` : une vérif de 200 items reste à quelques requêtes.
- Les notifications sont déclenchées **dans** la transaction mais envoyées **après** sa validation
  (`transaction.on_commit` dans `notifications.send`), dans un thread (sauf `QRPROTEC_SMS_SYNC=1`,
  utile pour les tests) : une API Free lente ne ralentit jamais une vérif, et un rollback n'envoie
  rien.
- `Lots.descendants()` charge **tous** les lots actifs ayant un parent en une requête puis parcourt en
  mémoire : volontaire, le nombre de lots reste petit (centaines) et cela évite N requêtes.

## Notifications

### SMS (Free Mobile)

`notifications.py`. L'API Free (`smsapi.free-mobile.fr/sendmsg?user=&pass=&msg=`) n'envoie qu'au
titulaire de la ligne : un couple identifiant / clé par destinataire (`SmsRecipient`). Événements
(`NotificationSettings`) : `stock_low`, `verif_problem`, `seal_broken`, `expired_daily`, `pin_blocked`
(PIN bloqué ou oublié, une fois par blocage, voir `notify_pin_blocked`), `lot_key_renewed`,
`badge_renewed`, `lot_key_expiring`, `badge_expiring`, `order_due`. Les cinq derniers, désactivés par défaut,
existent aussi comme options des notifications web (`PUSH_OPTIONS`). Les expirations sont contrôlées par
`check_alerts` (`check_key_expirations`) : une alerte `NotificationSettings.expiry_warning_days` jours avant
(Réglages, ou `KEY_EXPIRY_WARNING_DAYS`, 30 j), une à
l'expiration, suivies par `key_expiry_stage` (`KeyExpiry`) sur le lot ou l'utilisateur, remis à zéro par le
renouvellement. Le dernier
statut d'envoi est gardé par destinataire et affiché dans les Réglages du poste.

**Une alerte par passage sous le seuil** : `ItemType.low_notified` passe à `True` à l'envoi et ne
revient à `False` que quand le stock remonte au minimum. Même principe pour `empty_notified`.
`check_stock_levels(types)` est appelée après chaque mouvement ; la commande `check_alerts`
(timer systemd quotidien à 7 h 45) la lance pour tous les types, parce que les péremptions font
baisser le stock sans aucune action, et envoie le résumé des lots contenant des périmés.

**Commande à passer** (`order_due`, SMS et web) : `check_orders`, lancée par `check_alerts`, calcule
`forecast.forecast()` et annonce les types dont `order.before` (passage sous le minimum moins
`ORDER_LEAD_DAYS`) est atteint. `ItemType.order_notified` évite de répéter l'alerte ; il revient à
`False` quand plus aucune commande n'est due pour ce type.

### Notifications web

`webpush.py`, implémentation autonome de Web Push (chiffrement `aes128gcm` RFC 8291/8188, jeton VAPID
ES256 RFC 8292) au-dessus de `cryptography`. Les clés VAPID sont générées au premier usage et stockées
en base (`PushKeys`) : elles survivent aux sauvegardes/restaurations et les abonnements restent
valides. Rôles gestion et admin : `PUSH_TYPES` (`models.py`) donne pour chaque type son libellé, les
rôles qui le reçoivent et sa valeur par défaut. Un admin coupe des types par utilisateur
(`Secouristes.push_disabled`, fenêtre Utilisateurs) ; un envoi exige que l'abonnement ait choisi le type
et que l'utilisateur puisse le recevoir (`PushSubscription.wants`). `push/devices/` liste les appareils
de l'utilisateur connecté (nom tiré du User-Agent), `push/devices/<id>/` change leurs alertes ou les
retire. Un abonnement refusé (404/410) par le service du
navigateur est supprimé. Tests avec le vecteur de la RFC 8291 dans `tests.py`.

## Relais WebSocket (téléphone-douchette)

`remote_scanner.py` + `serve.py`. Le téléphone ouvre `/ws/scanner/phone?s=ID&k=CLÉ` (API publique,
via Caddy), le poste ouvre `/ws/scanner/front?s=ID` (API locale). Le serveur relaie les scans du
téléphone vers le poste et les retours d'erreur du poste vers le téléphone.

- Implémentation RFC 6455 minimale (trames texte, ping/pong), sans dépendance (`channels`, `daphne`
  et un serveur ASGI auraient été démesurés pour un relais de quelques messages).
- `QRProtecRequestHandler` intercepte les requêtes `Upgrade: websocket` sur `/ws/` avant WSGI.
- Les sessions sont en mémoire dans `hub` ; un thread « faucheur » ferme celles dont un côté est
  déconnecté depuis plus que le délai choisi (1 min à 24 h, 5 min par défaut).
- Le protocole des messages est documenté en tête de `remote_scanner.py`.

## Front web servi par le back

`web_views.page` sert `web/index.html` pour `/`, `/verif`, `/badge`, `/pack`, `/seal` ;
`scanner_page` sert `scanner.html`. `web_views.asset` sert `web/<nom>` à partir d'une **liste
blanche** (`ASSETS`) : ajouter un fichier au front web demande de l'y déclarer. Voir
[front-web.md](front-web.md).

## Commandes de gestion

| Commande | Rôle |
|---|---|
| `serve [--public A:P] [--local A:P] [--remote A:P] [--https] [--cert --key]` | Lance les trois API (+ WebSockets). `--https` génère un certificat auto-signé dans `database/.dev-certs/` pour tester la caméra d'un téléphone sur le réseau local. |
| `createadmin MATRICULE NOM PRÉNOM [--pin 1234]` | Crée ou répare un administrateur (badge perdu, plus aucun admin) : nouvelle clé de badge, rôle admin. Affiche l'URL du badge à scanner sur le poste. |
| `check_alerts` | Seuils de stock, résumé des périmés, expirations des clés et commandes à passer (timer quotidien). |
| `frontkey add NOM` / `list` / `revoke NOM` / `delete NOM` | Clés d'API des fronts distants. La clé n'est affichée qu'à `add`. |

## Tests

`python manage.py test inventory` (une cinquantaine de tests). `ApiTestCase.call(method, path, data,
local=True)` appelle l'API locale (en simulant le rôle WSGI `local`) ou publique (`local=False`), et
`setUp` crée un jeu minimal (deux types d'item, un type de lot « Sac PSE », un lot, un utilisateur). Couverture : format des iids, séparation
public/local, clés, vérifs (remplacements, disparus), réception et paquets, PIN, rôles, réassort,
scellés, sous-lots (arborescence, vérif groupée, partielle, rangements), SMS (via un faux transport),
Web Push (vecteurs RFC), relais WebSocket (vrai serveur sur un port libre, client WebSocket brut).

Ajouter un test pour toute règle de gestion modifiée. Les tests tournent pendant la construction du
RPM : un test rouge bloque la publication.
