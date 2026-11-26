# Back (Django)

Dossier `database/`. Projet Django `qrprotecDB`, une seule application `inventory`. Python ≥ 3.13,
Django 6.1, Django REST Framework 3.18 (versions figées par `poetry.lock` à la racine).

## Organisation

| Fichier | Contenu | Règle |
|---|---|---|
| `qrprotecDB/settings.py` | Réglages Django et dictionnaire `QRPROTEC` (toute la configuration propre au projet, lue depuis l'environnement) | Toute nouvelle option passe par une variable `QRPROTEC_*` ici, documentée dans le README racine et dans `packaging/files/qrprotec.conf`. |
| `qrprotecDB/urls.py`, `urls_local.py` | Jeux d'URLs de l'API publique et de l'API locale | Ne contiennent que des `include` des listes de `inventory/urls.py`. |
| `inventory/urls.py` | `public_patterns`, `local_patterns`, `web_patterns` | Voir « Routes ». |
| `inventory/views/` | Vues DRF (`@api_view`) | Fines : parser, contrôler l'accès, appeler `services`, sérialiser. `common.py` (erreurs, droits, identité), `public.py` (vues des deux API), `local.py` (gestion), `notif_settings.py`, `douchette.py`, `lists.py`, `prints.py` ; `__init__.py` réexporte tout (`views.<nom>`). Une nouvelle vue s'ajoute au fichier de sa famille. |
| `inventory/services.py` | Logique métier | Tout ce qui modifie plusieurs objets, dans une transaction. |
| `inventory/serializers.py` | Modèles → `dict` JSON, états des lots | Fonctions simples, pas de `Serializer` DRF (plus lisible, et les sorties ne sont pas symétriques des entrées). |
| `inventory/middleware.py` | `ApiRoleMiddleware`, `RoleWSGIHandler` | Sépare les trois API (publique, locale, distante). |
| `inventory/idendity.py` | Identités `M:` / `D:` | (sic, le nom du fichier a une coquille historique ; le renommer casserait les imports pour rien) |
| `inventory/notifications.py` | alertes : seuils de stock, expirations, commandes (SMS en commentaire) | |
| `inventory/webpush.py` | Notifications web (chiffrement RFC 8291, VAPID RFC 8292) | Sans `cryptography`, désactivé proprement. |
| `inventory/remote_scanner.py` | Relais WebSocket téléphone → poste | Protocole décrit en tête de fichier. |
| `inventory/live.py` | Suivi des vérifs en direct (WebSocket) | Protocole décrit en tête de fichier. |
| `inventory/print_live.py` | Impressions en attente en direct au poste (WebSocket `/ws/label-prints`) | Protocole décrit en tête de fichier. |
| `inventory/web_views.py`, `web/` | Front web | Liste blanche des fichiers servis. |
| `inventory/management/commands/` | `serve`, `createadmin`, `check_alerts` | |
| `inventory/tests/` | Tests, un fichier `test_*.py` par sujet (aides communes dans `base.py`) | `python manage.py test inventory` |

## Les trois API

### Sélection par le port

`manage.py serve` (voir `management/commands/serve.py`) démarre **trois serveurs WSGI** (le serveur de
développement de Django, multi-thread, durci) dans le même processus :

- public : `--public 127.0.0.1:8000` par défaut, derrière Caddy (`0.0.0.0:8000` en DEBUG, pour les téléphones) ;
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
- **poste principal** (`request.qrprotec_poste`, `views.is_main_poste`) : vrai pour l'API locale et
  pour une clé marquée `FrontKey.poste` (borne d'un serveur sans kiosk, `frontkey add NOM --poste`).
  Seul un poste principal gère les ordinateurs connectés (`/api/front-configs/`) ; le front lit ce
  droit dans `/api/health/` (`poste`) pour imprimer la file `/api/label-prints/pending/`. Un poste
  principal distant ne peut ni se révoquer, ni se supprimer, ni se retirer ce droit (409) ;
- sinon : URLs publiques.

**Utilisateur connecté.** Ni le localhost ni la clé de front ne donnent de droits de gestion :
ils identifient la machine. Après le badge (et le PIN), `POST /api/auth/` renvoie un jeton de session
signé (12h) que le front renvoie dans l'en-tête `X-QRProtec-Session`. Le serveur en déduit
l'utilisateur et son rôle (`views.front_user`, `views.require_front`) :

| Qui | Droits sur l'API locale ou distante |
|---|---|
| Personne de connecté | Lectures du kiosk (lots **sans** leurs clés, types), routes publiques (clé de lot exigée), téléphone-douchette, création du **premier** administrateur s'il n'y en a aucun |
| Secouriste | + vérifs et réassorts sans clé de lot, sous son identité |
| Gestion | + inventaire, réception, stock, paquets, lots et types, clés des lots |
| Admin | + utilisateurs, réglages des notifications |

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
`lots/<id>/verif/`, `verifs/`, `lots/<id>/add/`, `items/out/` et `lots/<id>/unseal/`. `GET /api/health/` expose
`declared_identity` à tous les fronts (web compris), qui ne proposent la saisie du nom que s'il est vrai.
Exception : `lots/<id>/seal-open/` (étiquette d'ouverture du scellé) ouvre le scellé même sans identité,
puisque le code de l'étiquette rangée dans le lot prouve l'ouverture ; l'identité, si elle est donnée,
signe l'ouverture.

Les lectures réservées sur l'API publique (`lots/summary/`, `stock/summary/`, `push/*`) utilisent
`views.badge_user(request, roles)` : badge valide, rôle, et jeton de session si l'utilisateur a un PIN.

### Clés de lot sur l'API publique

`views.require_lot_key` (écritures sur un lot) et `views.verif_targets` (vérifs) : la clé envoyée
doit être celle du lot **ou d'un de ses ancêtres** (l'étiquette clé d'un lot global couvre ses
sous-lots), non expirée. Sur l'API locale, aucune clé n'est demandée. L'expiration ne s'applique que
si le nom déclaré est autorisé (`models.lot_keys_expire`) : sinon la clé n'autorise rien de plus
qu'une étiquette publique et `key_status` ne répond jamais `expired`.

## Services

Voir [regles-de-gestion.md](regles-de-gestion.md) pour le détail fonctionnel. Points techniques :

- `perform_verif` verrouille les items scannés et ceux du périmètre (`select_for_update(of=('self',))`)
  puis fait des `bulk_update` / `bulk_create` : une vérif de 200 items reste à quelques requêtes.
- Les notifications sont déclenchées **dans** la transaction mais envoyées **après** sa validation
  (`transaction.on_commit` dans `webpush.queue`), dans un thread (sauf `QRPROTEC_NOTIFY_SYNC=1`,
  utile pour les tests) : un service de notifications lent ne ralentit jamais une vérif, et un rollback
  n'envoie rien.
- Journal des opérations (`journal.py`, modèle `Operation`) : chaque opération qui écrit (vérif, réassort,
  retrait, scellage, ouverture de scellé, réception, suppression, gestion des lots, des types, des
  utilisateurs et des réglages) ajoute une ligne avec l'identité de l'opération (`operation_identity`, ou
  l'utilisateur connecté pour la gestion). Une ouverture anonyme par l'étiquette intérieure, signée
  ensuite, est réattribuée dans le journal aussi. `GET /api/operations/` (API locale et distante, rôles
  gestion et admin) filtre par `kind`, `by`, `lot` (avec ses sous-lots, archivés compris, sauf `sub=0`),
  `since` / `until` et `q`, pagine avec `before` et donne avec `facets=1` les personnes et les types.
  `details.auth` dit comment la personne s'est authentifiée (`sessions.auth_label`, requête posée par
  `handle_errors`) : « Web : passkey », « Poste principal : carte + PIN », « Poste distant (clé API
  « Borne B ») : nom + PIN ». Le moyen de connexion est gardé dans le jeton de session (`a`,
  `sessions.AUTH_METHODS`). Chaque connexion réussie (`login_response`) ajoute une ligne `kind=login`
  (« Connexion ») : c'est l'historique des connexions de la page Utilisateurs du poste
  (`?kind=login&by=M:<matricule>`). Elle ne s'annule jamais : `undo._finish` ne lui donne pas de lot
  d'écritures et `undo.plan` la refuse.
- Annulation des opérations (`undo.py`, modèle `OperationChange`) : le middleware `CaptureMiddleware`
  enregistre les écritures de chaque requête qui n'est pas un GET (et la vérif commune enregistrée par le
  serveur, `live.py`). Des déclencheurs SQLite **temporaires** (propres à la connexion, créés à la première
  écriture de chaque table : une requête qui ne fait que lire ne paie rien) notent chaque ligne ajoutée,
  modifiée ou supprimée ; un `execute_wrapper` note les colonnes écrites par chaque UPDATE. Si la requête a
  ajouté des lignes au journal, ses écritures sont gardées sous un numéro de lot (`Operation.batch`).
  `operations/<id>/cancel/` (admins) défait le lot de la dernière écriture à la première : ligne ajoutée
  supprimée, supprimée remise, colonne remise à l'ancienne valeur sauf si une opération postérieure encore
  valable l'a écrite depuis ou si elle a changé autrement. Les opérations postérieures qui modifient une
  ligne ajoutée (réponse à un signalement) ou y font référence sont annulées avec elle (`ItemMovement`
  exclu : la vérif suivante qui retrouve un item ne dépend pas de celle qui l'a noté absent). L'annulation
  est une opération `cancel` enregistrée pareil : l'annuler rétablit l'opération (et efface
  `cancelled_by`). Compteurs (`Sequence`, `ItemsPacks.last_sequence`), échecs de PIN et réglages créés à
  leur première lecture ne sont jamais remis. Purgées avec l'historique (`purge_history`).
- `Lots.descendants()` charge **tous** les lots actifs ayant un parent en une requête puis parcourt en
  mémoire : volontaire, le nombre de lots reste petit (centaines) et cela évite N requêtes.

## Cache de vérif (`verif_cache.py`)

Une partie de la réserve n'a ni réseau ni wifi : chaque scan d'une vérif doit se traiter sans attendre le
serveur. `POST /api/verifs/cache/` (public comme le détail d'un lot ou d'un item ; un badge envoyé doit être
valide) donne aux fronts tout ce qu'une vérif lit : le détail de **chaque lot global** (et lot seul) avec ses
sous-lots, comme `lots/<id>/` mais sans clé ni départ, les items qu'on peut encore scanner (tous sauf les
supprimés et remplacés vus il y a plus d'un an), les paquets fermés (et ouverts depuis moins de 6 mois) et
**tous** les types d'items. Les items d'un lot n'y sont que par leur iid (`items`, `missing_items`,
`optional_items`) : le front les reprend dans les enregistrements d'items.

Les enregistrements (`i:<iid>`, `l:<lot global>`, `p:<paquet>`, `t:<type>`) sont répartis en 256 seaux selon
le SHA-1 de leur clé. Le front envoie `{"buckets": {seau: empreinte}}` ; le serveur ne renvoie que les seaux
dont l'empreinte a changé, avec tout leur contenu (un enregistrement absent d'un seau renvoyé a disparu) :
tout au premier appel, presque rien ensuite. Rien n'est gardé côté serveur entre deux appels, si bien que
l'API publique, locale et distante (processus distincts) répondent pareil. Le calcul (quelques secondes pour
5 000 items) resert 10 s, séparément pour les noms courts de l'API publique et les noms complets des postes.

## Vérif faite hors connexion : heure des scans, heure d'envoi

Une vérif faite sans réseau part plus tard, depuis la file d'attente du front. Elle est datée à l'heure des scans,
pas à celle de l'envoi : les fronts joignent à chaque `POST /api/verifs/` l'heure de l'appareil à la validation
(`done_at`) et à l'envoi (`sent_at`). L'horloge d'un téléphone ou d'un poste est souvent fausse, seul compte
l'écart entre les deux : `views.verif_time` date la vérif à *réception − (sent_at − done_at)*, jamais dans le
futur. Envoyée moins de 2 minutes après sa validation (`VERIF_SENT_GAP`), elle reste datée de sa réception,
comme avant. `perform_verif(now=…)` date à cette heure la vérif, le journal, les passages et les mouvements, et
juge les péremptions ce jour-là ; si le lot a eu une vérif plus récente entre-temps, sa dernière vérif (et ce qui
va avec : vérif recommandée, réassorts) n'est pas ramenée en arrière.

L'heure de réception reste sur la vérif (`Verifs.received`, vide pour les vérifs d'avant ce champ) et
`Verifs.sent_late()` la donne quand elle s'écarte de plus de 2 minutes : `sent` dans « Vérifs récentes », le
compte rendu et `lots/<id>/verifs/` (« 📶 Faite hors connexion · reçue par le serveur le … »), et sur la ligne
« Dernière vérif » du rapport d'état PDF (« faite hors connexion, reçue par le serveur le … »). Un écart inattendu
(heure de l'appareil modifiée entre la vérif et l'envoi) se voit ainsi d'un coup d'œil.

Les fronts joignent aussi le début de la vérif (`started_at` : étiquette du lot scannée, ou fin de l'étape précédente
d'une vérif en cascade) et l'heure de chaque scan (`scanned_at` : `{iid: heure}`). `views.device_times` les recale de
la même façon (*réception − (sent_at − heure)*), jamais après la vérif ni plus de 12h avant. Ils sont gardés dans
`Verifs.started` et `VerifItem.scanned_at` (vides pour un front d'avant ces champs ou le scan d'un autre participant) :
durée de la vérif et dernier scan de chaque item dans le rapport d'état, et de quoi faire des statistiques (durée des
vérifs, items scannés par minute). Un item à tâches scanné garde ses tâches cochées dans `VerifItem.tasks`.

## Notifications

### Alertes (`notifications.py`)

Toutes les alertes partent en notifications web (`webpush.py`, voir plus bas) :
`notify_admins(event, ...)` envoie aux abonnements qui ont choisi l'évènement (`PUSH_TYPES`). PIN
bloqué ou oublié : une fois par blocage (`notify_pin_blocked`). Les expirations sont contrôlées par
`check_alerts` (`check_key_expirations`) : une alerte `NotificationSettings.expiry_warning_days` jours avant
(Réglages, ou `KEY_EXPIRY_WARNING_DAYS`, 30 j), une à l'expiration, suivies par `key_expiry_stage`
(`KeyExpiry`) sur le lot ou l'utilisateur, remis à zéro par le renouvellement.

**SMS Free Mobile : plus maintenus.** L'API (`smsapi.free-mobile.fr/sendmsg`) n'était pas utilisée ;
son code est en commentaire en tête de `notifications.py`, ses routes (`notifications/recipients/`,
`notifications/test/`) dans `urls.py` et `views/notif_settings.py`. `NotificationSettings` (hors
`key_expiry_warning_days`) et `SmsRecipient` restent en base, inutilisés. `GET/PATCH /api/notifications/`
ne porte plus que `key_expiry_warning_days`. Les évènements qui n'existaient qu'en SMS sont devenus des types
web : `expired_daily` (résumé des lots contenant des périmés, `check_alerts`, actif par défaut) et
`verif_problem` (vérif incomplète, `services.notify_verif_problem`, désactivé par défaut, ouvre la fiche du lot).

**Ouverture d'un scellé** (`seal_broken`, web, gestion et admin) : `services.notify_seals_broken`, une
notification par opération (étiquette d'ouverture, vérif, ajout, retrait, ouverture depuis le poste) même si les
scellés des lots parents sont ouverts avec. La notification web ouvre la fiche du lot (`tab: 'lot'`, tag
`seal-<id>`) ; une ouverture anonyme signée juste après remplace la notification web (même tag).

**Une alerte par passage sous le seuil** : `ItemType.low_notified` passe à `True` à l'envoi et ne
revient à `False` que quand le stock remonte au minimum. Même principe pour `empty_notified`.
`check_stock_levels(types)` est appelée après chaque mouvement ; la commande `check_alerts`
(timer systemd quotidien à 7h45) la lance pour tous les types, parce que les péremptions font
baisser le stock sans aucune action.

**Commande à passer** (`order_due`, web) : `check_orders`, lancée par `check_alerts`, calcule
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
navigateur est supprimé. Tests avec le vecteur de la RFC 8291 dans `tests/test_notifications.py`.

## Relais WebSocket (téléphone-douchette)

`remote_scanner.py` + `serve.py`. Le téléphone ouvre `/ws/scanner/phone?s=ID&k=CLÉ` (API publique,
via Caddy), le poste ouvre `/ws/scanner/front?s=ID` (API locale). Le serveur relaie les scans du
téléphone vers le poste et les retours d'erreur du poste vers le téléphone.

- Implémentation RFC 6455 minimale (trames texte, ping/pong), sans dépendance (`channels`, `daphne`
  et un serveur ASGI auraient été démesurés pour un relais de quelques messages).
- `QRProtecRequestHandler` intercepte les requêtes `Upgrade: websocket` sur `/ws/` avant WSGI.
- Les sessions sont en mémoire dans `hub` ; un thread « faucheur » ferme celles dont un côté est
  déconnecté depuis plus que le délai choisi (1 min à 24h, 5 min par défaut).
- Le protocole des messages est documenté en tête de `remote_scanner.py`.

## Impressions en attente en direct

`print_live.py` réutilise la même implémentation WebSocket. Le poste principal (API locale, ou clé
marquée poste) ouvre `/ws/label-prints` dès qu'il voit une nouvelle demande dans
`/api/label-prints/pending/` (qu'il interroge sinon toutes les 20 s). Chaque changement de la file
(`label_prints.create`, `create_labels`, `cancel`, `launch_queue`, `cancel_queue`, `print_now`) appelle
`hub.changed()`, qui signale `{"type": "changed"}` une fois la transaction validée : le poste relit la file
aussitôt, par l'API habituelle. Le poste ferme le WebSocket après 15 min sans nouvelle impression.

## Suivi des vérifs en direct

`live.py` réutilise la même implémentation WebSocket. La page de scan ouvre `/ws/live/scanner` et
publie l'état de sa vérif en cours (borné et nettoyé par `clean_state` : ce n'est que de
l'affichage) ; les pages de suivi ouvrent `/ws/live/watch`, s'authentifient par leur premier
message (badge) et reçoivent un instantané puis chaque mise à jour. Les noms passent par
l'anonymisation de l'API publique. Une vérif validée reste affichée 10 min, une page de scan
déconnectée 15 min (le temps d'un rechargement, qui reprend la même session). Les vérifs passées
viennent de `POST /api/verifs/recent/` (badge, une ligne par vérif groupée ; les étapes d'une vérif
en cascade, même personne, même lot du haut, à moins de 30 min l'une de l'autre et sans lot revu,
forment une seule ligne : `ids` une vérif par étape, `top` les lots du haut qui la nomment) ; le compte rendu d'une
vérif passée, de `POST /api/verifs/<id>/report/` (badge : chaque lot de la vérif groupée dans l'ordre de
l'arbre, items vus, périmés, remplacés et manquants par type et péremption, exigences non remplies du
journal, réassorts et retraits du même instant).

**Vérif à plusieurs.** Une session `LiveSession` a un ou plusieurs `Participant` (identité
stockée, scans partagés `entries`, `validated`). On la rejoint par `hello` avec `join` (badge
vérifié exigé). Chaque participant reçoit `team` : participants, lots, lots dont le serveur a la
clé (`keyed`, les clés ne sortent jamais), scans des autres et vérifs du même lot global
(`nearby`). `validate` répond `submit: true` seulement au dernier participant connecté non validé ;
celui-ci envoie `POST /api/verifs/` avec `live: {id, pid, token}` : la vue complète les clés
manquantes, enregistre les autres participants (`Verifs.participants`, identités stockées) et
`hub.finish` envoie le compte rendu (`report`) aux autres. Si les participants non validés se
déconnectent (plus de `GRACE` = 20 s), le faucheur demande l'envoi (`submit`) à un participant
qui a validé. Quand plus aucun participant n'est connecté depuis `LOST_KEEP` (15 min), la vérif est
abandonnée, sauf si un participant a validé ou si le dernier état publié n'a plus rien à scanner :
`save_unattended` l'enregistre alors avec les scans de tous et les clés gardées par le serveur
(`views.verif_lots`, contrôle des clés de l'API publique). Le front publie la vérif dès son début
(premier scan, étiquette clé, ou « Lancer une vérif » depuis la fiche).

Tout le détail (protocole, structures en mémoire, concurrence, fusion, cascade, faucheur,
anonymisation) est dans [WebSockets des vérifs](websockets.md).

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
| `frontkey add NOM` / `list` / `revoke NOM` / `delete NOM` | Clés d'API des fronts distants. La clé n'est affichée qu'à `add`. `add NOM --config FICHIER [--url URL] [--ca-file PEM]` l'écrit plutôt, avec l'URL et le certificat, dans un fichier chiffré (AES-256-GCM, PBKDF2-HMAC-SHA256, `inventory/front_config.py`, `-` = sortie standard) que le front importe avec le mot de passe (`app/src/net/front_config.cpp`). Mot de passe demandé, ou `QRPROTEC_FRONT_CONFIG_PASSWORD`. |

## Tests

`python manage.py test inventory` (une cinquantaine de tests). `ApiTestCase.call(method, path, data,
local=True)` appelle l'API locale (en simulant le rôle WSGI `local`) ou publique (`local=False`), et
`setUp` crée un jeu minimal (deux types d'item, un type de lot « Sac PSE », un lot, un utilisateur). Couverture : format des iids, séparation
public/local, clés, vérifs (remplacements, disparus), réception et paquets, PIN, rôles, réassort,
scellés, sous-lots (arborescence, vérif groupée, partielle, rangements), alertes (webpush remplacé),
Web Push (vecteurs RFC), relais WebSocket (vrai serveur sur un port libre, client WebSocket brut).

Ajouter un test pour toute règle de gestion modifiée. Les tests tournent pendant la construction du
RPM : un test rouge bloque la publication.
