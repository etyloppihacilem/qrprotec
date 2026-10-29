# WebSockets des vérifs en direct et à plusieurs

Cette page détaille toute la mécanique temps réel des vérifs faites au téléphone : le suivi en direct
par les chefs de poste, la vérif à plusieurs (plusieurs téléphones sur une même vérif), la fusion
automatique de deux vérifs du même lot global et la vérif en cascade (VPS > armoires > sacs). Le
résumé est dans [back.md](back.md) ; ici on descend jusqu'au protocole, aux structures en mémoire et
aux raisons de chaque règle.

Fichiers concernés :

| Fichier | Rôle |
|---|---|
| `database/inventory/live.py` | Tout le serveur : `LiveHub` (instance unique `hub`), `LiveSession`, `Participant`, nettoyage des messages, faucheur. Le protocole est rappelé en tête du fichier. |
| `database/inventory/remote_scanner.py` | Implémentation WebSocket (classe `WebSocket`, RFC 6455), partagée avec le téléphone-douchette. |
| `database/inventory/management/commands/serve.py` | `QRProtecRequestHandler` : intercepte l'`Upgrade: websocket` et aiguille vers `live.hub`. |
| `database/inventory/views.py` | `verifs` (`POST /api/verifs/`) : envoi d'une vérif avec `live`, appels à `hub.verif_team`, `hub.step`, `hub.finish` ; `verif_lots`, `key_holders`, `require_label`. |
| `database/inventory/idendity.py` | `anonymized()`, `identity_name()` : noms courts affichés aux téléphones. |
| `database/inventory/web/app.js` | Client : module `live` (page de scan), `watch*` (page de suivi), `validate`, `teamStep`, `teamReport`, `liveSnapshot`, `allScanned`. |
| `database/inventory/tests.py` | `LiveHubTests` avec un `FakeSocket`. |

Le **front poste (ImGui) n'utilise pas ces WebSockets** : sa vérif, y compris en cascade, reste
locale au poste et n'apparaît pas dans le suivi en direct. Seul le front web publie et suit. Le seul
WebSocket du poste est celui du téléphone-douchette (`/ws/scanner/front`), sans rapport avec ce qui
suit.

## Vue d'ensemble

```
 Téléphone A (page de scan) ──┐                              ┌── Page de suivi (badge)
 Téléphone B (page de scan) ──┤ wss /ws/live/scanner         │ wss /ws/live/watch
                              ▼                              ▼
                 ┌───────────── manage.py serve (un processus) ─────────────┐
                 │  live.hub : LiveHub (en mémoire, verrou RLock)           │
                 │    sessions : {id: LiveSession}                          │
                 │      participants : {pid: Participant}                   │
                 │      lots : {lot id: clé}   (les clés ne ressortent pas) │
                 │    watchers : {WebSocket des pages de suivi}             │
                 │  thread « live-reaper » toutes les 10 s                  │
                 └──────────────┬───────────────────────────────────────────┘
                                │ POST /api/verifs/ avec "live" (HTTP ordinaire)
                                ▼
                     views.verifs → services.perform_verif → base SQLite
```

Deux rôles de connexion :

- la **page de scan** (`/ws/live/scanner`) publie l'état de sa vérif et reçoit l'état de la vérif
  commune (participants, scans des autres, lots déjà validés) ;
- la **page de suivi** (`/ws/live/watch`) reçoit la liste des vérifs en cours et leurs mises à jour,
  en lecture seule.

Le WebSocket ne fait que **partager de l'affichage et coordonner** qui envoie quoi. L'enregistrement
d'une vérif passe toujours par `POST /api/verifs/`, qui contrôle tout (clés, étiquettes, items) comme
pour une vérif faite seul. Rien de ce qu'un téléphone publie n'est cru par le serveur pour écrire en
base, à une exception près, encadrée : la vérif abandonnée enregistrée par le serveur
(`save_unattended`, plus bas), qui repasse par les mêmes contrôles.

Les sessions vivent **en mémoire** dans le processus de `manage.py serve` : un redémarrage du back
les perd (les téléphones repartent alors sur une nouvelle session, leurs scans locaux sont gardés
dans le `localStorage` de la page, clé `qrprotec.session`). C'est voulu : une vérif en cours n'a pas de valeur en base tant
qu'elle n'est pas validée, et cela évite une table et des migrations pour un état éphémère.

## Transport

### Serveur WebSocket maison

Pas de `channels`/`daphne`/ASGI : `serve.py` lance trois serveurs WSGI (`socketserver.ThreadingMixIn`,
un thread par connexion, `daemon_threads`). `QRProtecRequestHandler.handle_one_request` regarde la
ligne de requête avant WSGI : un chemin en `/ws/` avec `Upgrade: websocket` part dans
`handle_websocket`, le reste suit le traitement Django habituel.

Pour `/ws/live/scanner` et `/ws/live/watch`, `handle_websocket` **n'authentifie rien à la poignée de
main** (pas de paramètre d'URL, pas d'en-tête) : il répond `101 Switching Protocols` avec
`Sec-WebSocket-Accept`, règle le délai de lecture du socket à `RECEIVE_TIMEOUT` (45 s) et appelle
`live.hub.run_scanner` ou `live.hub.run_watcher` dans le thread de la connexion. L'authentification se
fait par le **premier message** (`hello`), ce qui permet au navigateur d'envoyer badge ou jeton sans les
mettre dans l'URL (journaux de Caddy). Ces deux chemins ne regardent pas le rôle du port ; en
production les téléphones y arrivent par l'API publique, via Caddy (`reverse_proxy` transmet l'upgrade).

`remote_scanner.WebSocket` est minimal : trames texte (et binaires/continuations recollées),
masque client, réponse automatique aux pings, fermeture propre, messages limités à `MAX_MESSAGE`
(64 Kio, au-delà fermeture 1009). `send_json` est protégé par un verrou par socket (`send_lock`) : deux
threads peuvent écrire sur la même connexion (le sien et celui d'un autre participant qui diffuse).
Il renvoie `False` au lieu de lever si la connexion est morte, pour qu'une diffusion ne s'arrête pas
sur un client parti.

### Maintien en vie et reconnexion

Les clients envoient `{"type": "ping"}` toutes les 15 s (`PING_MS`) ; un client muet plus de 45 s est
considéré parti (le `read` du socket lève, `WebSocketClosed`). Côté navigateur, une connexion fermée
alors qu'elle est encore voulue se rouvre avec un délai exponentiel (1 s, 2 s, 4 s… plafonné à 30 s).
La page de scan reprend alors **la même session** grâce à `{id, pid, token}` gardés dans
`sessionStorage` (clé `qrprotec.live`) : un rechargement de l'onglet ou une coupure réseau ne crée pas
une nouvelle vérif.

## Le modèle en mémoire

### `LiveHub`

| Attribut | Contenu |
|---|---|
| `lock` | `threading.RLock` qui protège tout l'état du hub (réentrant : des méthodes sous verrou en appellent d'autres). |
| `sessions` | `{id: LiveSession}`, vérifs en cours ou terminées récemment. |
| `watchers` | ensemble des WebSockets des pages de suivi authentifiées. |
| `_reaper` | thread `live-reaper`, démarré à la première page de scan, arrêté quand il n'y a plus de session. |

### `LiveSession` (une vérif)

| Attribut | Contenu |
|---|---|
| `id` | 12 caractères aléatoires (`secrets`, lettres et chiffres). |
| `participants` | `{pid: Participant}` dans l'ordre d'arrivée. |
| `lots` | `{lot id: clé}` : lots de la vérif et clé de leur étiquette clé si un participant l'a scannée (vide sinon). **Jamais renvoyé** aux clients, seule la liste des lots dont la clé est connue l'est (`keyed`). |
| `labels` | lots dont une étiquette (publique ou clé) a été scannée par un participant : la vérif est bien celle de ces lots (`require_label`). |
| `started`, `updated` | horodatages affichés dans le suivi. |
| `done` | `None` tant que la vérif court, puis le résumé affiché « Validée » ou « Terminée » (`complete`, `partial`, `present`, `at` ; en cascade aussi `lots`, `incomplete`). |
| `submitting` | `pid` du participant chargé d'envoyer la vérif commune (évite deux envois). |
| `saving` | le serveur est en train d'enregistrer lui-même la vérif abandonnée. |
| `solo` | session ouverte après « Quitter » : jamais fusionnée automatiquement. |
| `finished` | `time.monotonic()` de la fin, pour `DONE_KEEP`. |
| `steps` | vérif en cascade : `{lot id: {at, by, complete, sealed}}`, lots déjà validés pour tous. |

Propriétés calculées : `state` (dernier état publié parmi les participants : chacun publie la vérif
entière, scans des autres compris, donc le plus récent suffit), `connected` (au moins un participant
connecté), `lost` (dernière déconnexion), `people(me)` (participants dédoublonnés, voir plus bas),
`who` (noms joints par des virgules), `public()` (ce qu'on envoie au suivi).

### `Participant` (un téléphone, ou un onglet)

| Attribut | Contenu |
|---|---|
| `id`, `token` | 12 et 24 caractères aléatoires ; le jeton prouve qu'on est bien ce participant à la reconnexion et à l'envoi (`hmac.compare_digest`). |
| `identity` | identité stockée (`M:matricule` pour un badge ou nom + PIN, `D:nom` pour un nom déclaré), celle qui sera enregistrée sur la vérif. Ne quitte jamais le serveur. |
| `who`, `verified` | nom **anonymisé** affiché aux autres, et badge vérifié ou non. |
| `socket` | WebSocket en cours, `None` si déconnecté ; `lost` : depuis quand. |
| `state`, `updated` | dernier état publié (`clean_state`), et quand. |
| `entries` | scans de ce participant, partagés avec les autres (`clean_entries`). |
| `validated`, `partial` | a validé sa partie (et en vérif partielle). |

### Limites

Tout ce qui vient d'un téléphone est borné et nettoyé (`clean_state`, `clean_entries`, `clean_keys`,
`clean_labels`, `_text`, `_number`) : un client malveillant ne peut pas faire grossir la mémoire du
serveur ni injecter de champ inattendu.

| Constante | Valeur | Rôle |
|---|---|---|
| `MAX_SESSIONS` | 200 | vérifs suivies en même temps ; au-delà, refus `trop de vérifs suivies en même temps`. |
| `MAX_PARTICIPANTS` | 8 | personnes sur une même vérif (rejoindre ou fusionner au-delà est refusé/ignoré). |
| `MAX_SCANNED` | 150 | derniers scans gardés pour l'affichage du suivi. |
| `MAX_ENTRIES` | 300 | scans partagés par participant (un paquet compte pour un, avec jusqu'à 500 iids). |
| `MAX_LOTS` | 20 | lots d'une vérif groupée (et clés, étiquettes). |
| `GRACE` | 20 s | un participant déconnecté depuis plus longtemps ne retient plus l'envoi de la vérif commune. |
| `LOST_KEEP` | 15 min | vérif dont plus aucun téléphone n'est connecté : gardée (le temps d'un rechargement), puis enregistrée ou abandonnée. |
| `DONE_KEEP` | 10 min | vérif terminée : encore affichée dans le suivi. |
| `REAPER_PERIOD` | 10 s | période du faucheur. |

### Concurrence

Chaque connexion a son thread, et le faucheur le sien. Les règles suivies partout dans `live.py` :

- tout accès à `sessions`, `participants` et à leurs champs se fait **sous `hub.lock`** ;
- les **envois** (`send_json`) se font **hors du verrou** : on prépare la liste `(socket, message)` sous
  verrou, puis on envoie. Un client lent ne bloque donc pas les autres ;
- les **accès à la base** (vérifier un badge, calculer un nom public, enregistrer une vérif) se font
  aussi **hors du verrou**, et sont suivis de `close_old_connections()` : ces threads ne passent pas par
  le cycle requête de Django, ils doivent rendre eux-mêmes leur connexion SQLite ;
- un participant peut changer de session pendant que son thread attend un message (fusion, ou il a
  rejoint une autre vérif depuis un autre onglet) : à chaque message, `run_scanner` relit sa session
  par `_session_of(participant, session)` et s'arrête si elle n'existe plus.

## Page de scan : `/ws/live/scanner`

### `hello` et `attach_scanner`

Le premier message doit être `hello`, sinon la connexion est fermée. Il porte au choix :

- `id`, `pid`, `token` : reprise d'un participant existant (rechargement, reconnexion) ;
- `join` : id d'une vérif à rejoindre (bouton « Rejoindre » du suivi, ou bandeau « X vérifie aussi ce
  lot ») ;
- `solo: true` : nouvelle vérif qui ne doit pas être fusionnée (après « Quitter ») ;
- l'identité : `user` (badge `{matricule, key}`, ou `{session}` après une connexion par nom et PIN) ou
  `name` (nom déclaré, 30 caractères au plus).

`scanner_identity` calcule l'identité **hors verrou** (accès base) : badge valide → `M:matricule`,
vérifié ; sinon nom déclaré → `D:nom`, non vérifié ; sinon personne (la page peut publier sans
identité, elle la donnera plus tard par `identity`). Puis `attach_scanner`, sous verrou, tranche :

1. **`join` vers une autre vérif** : il faut un badge vérifié (sinon `Scannez votre badge pour
   rejoindre une vérif.`), une vérif existante et non terminée, moins de 8 participants. Si le
   téléphone était déjà participant ailleurs, il quitte cette session (`_leave` ; une session vide
   disparaît) et son ancienne connexion est fermée. Un nouveau `Participant` est créé dans la vérif
   rejointe.
2. **Reprise** : `id`/`pid`/`token` désignent un participant existant (`_find`, comparaison du jeton
   en temps constant) : on garde le participant, la nouvelle connexion remplace l'ancienne, qui est
   fermée.
3. **Sinon, nouvelle vérif** : nouvelle `LiveSession` avec un participant, marquée `solo` si demandé.

L'identité est mise à jour dans tous les cas (la personne a pu se connecter entre-temps). En cas de
refus, le serveur répond `{"type": "error", "message", "join": bool}` et ferme ; sinon il répond
`{"type": "hello", "id", "pid", "token"}`, que le navigateur range dans `sessionStorage`, puis diffuse
la vérif (`_changed`).

### Publier : `state`

À chaque changement (scan, étiquette, changement de lot), la page envoie un `state` (préparé par
`liveSnapshot`, regroupé 300 ms par `flush`, et pas renvoyé s'il est identique au précédent) :

| Champ | Usage côté serveur |
|---|---|
| `lot` `{id, name}`, `root`, `lots` | affichage, et `root` (lot global) sert à la **fusion** et aux vérifs voisines (`nearby`). |
| `scanned` | `[{name, expired, error, extra}]` : liste pour le suivi, déjà nommée côté téléphone (le suivi n'interroge pas la base). |
| `count`, `remaining` | jauge du suivi ; `remaining == 0` avec `count > 0` rend une vérif abandonnée **digne d'être enregistrée**. |
| `entries` | **les scans propres** de ce participant (`code`, `kind` item/pack/seal, `iid`, `items` d'un paquet, `place`, `key` d'un scellé, `info` du type), partagés aux autres. |
| `keys` | `[{id, key}]` : lots de la vérif et clés connues ici. Fusionnés dans `session.lots` (une clé connue n'est jamais effacée par une vide). |
| `labels` | lots dont l'étiquette a été scannée ici, ajoutés à `session.labels`. |
| `cascade`, `level` | vérif en cascade, et lot où se trouve ce participant (affiché « 📍 » dans le suivi et chez les autres). |

La page publie **dès le début** de la vérif : premier scan, étiquette clé, ou « Lancer une vérif »
depuis la fiche du lot. C'est ce qui permet à un deuxième secouriste de la voir et de la rejoindre
avant le premier scan.

Si un `state` arrive sur une vérif déjà terminée (`done`), le participant a recommencé à scanner :
`_restart` crée une nouvelle session avec un nouveau participant (même identité), envoie un nouveau
`hello`, et laisse l'ancienne affichée « terminée » sans lui.

Après chaque `state`, le serveur tente une **fusion** (`_merge`, plus bas) puis diffuse.

### Autres messages de la page de scan

| Message | Effet |
|---|---|
| `identity` (`user` ou `name`) | la personne s'est identifiée en cours de vérif (badge scanné, nom tapé) : identité, nom et `verified` mis à jour. |
| `validate` `{partial}` | valide sa partie (voir « Envoi de la vérif commune »). |
| `unvalidate` | annule sa validation (confirmation refusée, vérif refusée par l'API, envoi impossible) ; libère `submitting` s'il le tenait. |
| `done` `{complete, partial, present}` | la vérif vient d'être enregistrée par ce téléphone (vérif seul, sans `live`) : affichée terminée. |
| `leave` | vérif en cascade terminée par ce participant (voir « Vérif en cascade »). Le serveur termine la boucle. |
| `cancel` | plus de vérif en cours sur ce téléphone : état, scans et validation effacés (le participant reste, connecté). |
| `ping` | ignoré (maintien en vie). |

À la déconnexion (sortie de boucle), si la connexion était bien la connexion courante du participant,
il passe déconnecté (`socket = None`, `lost` = maintenant) et la vérif est rediffusée : les autres le
voient « déconnecté ». Il reste participant : il peut revenir avec son jeton.

### Ce que reçoit la page de scan

| Message | Quand | Ce qu'en fait `app.js` |
|---|---|---|
| `hello` `{id, pid, token, merged?, who?}` | connexion, fusion, nouvelle vérif après la fin | range les identifiants ; `merged` : bandeau « 👥 Vérif commune avec … ». |
| `error` `{message, join}` | refus de `hello` | toast si c'était un `join`. |
| `team` | à chaque changement de la vérif ou d'une vérif du même lot global | `teamChanged` : bandeau des participants, lots manquants chargés (`syncTeamLots`), lots déjà validés en cascade. |
| `validate` `{submit, waiting}` | réponse à `validate` | envoyer la vérif, ou attendre (« En attente de … »). |
| `submit` `{partial}` | le faucheur désigne ce téléphone pour envoyer | `validate(partial, true)` sans redemander de confirmation. |
| `report` `{report}` | un autre participant a enregistré la vérif commune | `teamReport` : affiche le compte rendu, remet la page à zéro. |
| `step` `{by, lots, items}` | cascade : un autre participant a validé un lot | `teamStep` : lot noté validé, ses scans retirés, retour au lot parent si l'on était dedans. |

Le message `team` :

| Champ | Contenu |
|---|---|
| `id` | id de la vérif commune. |
| `participants` | `people(me)` : `{who, verified, connected, validated, level, me}` par personne. |
| `lots` | ids des lots de la vérif (un téléphone qui rejoint charge ceux qui lui manquent). |
| `keyed` | lots dont **le serveur** a la clé : celui qui a rejoint sait qu'il n'a pas à scanner l'étiquette clé. Les clés elles-mêmes ne sortent jamais. |
| `labeled` | lots dont une étiquette a été scannée par quelqu'un de la vérif. |
| `entries` | scans **des autres** participants, chacun avec `who`. |
| `nearby` | autres vérifs en cours du même lot global (`{id, who, lot}`), qu'on peut rejoindre. |
| `done` | cascade : `session.steps`, lots déjà validés. |

## Partager les scans entre participants

Chaque participant n'envoie que **ses propres scans** (`entries`) ; le serveur renvoie à chacun ceux
**des autres** (`team.entries`). Côté web, `allScanned()` assemble les deux (un même code compté une
seule fois) et alimente les attendus, le reste à scanner et le plan de la vérif : tout le monde voit
les mêmes manques. **Tout nouveau code de `app.js` qui lit les scans d'une vérif doit passer par
`allScanned()`**, pas par `state.scanned` (qui ne contient que les scans de ce téléphone).

Les **clés** des lots restent sur le serveur. La personne qui a scanné l'étiquette clé du sac les
publie dans `keys` ; la personne qui a rejoint la vérif n'a donc pas besoin de la scanner pour
envoyer : `views.verifs` complète les clés manquantes avec `team['keys']`. Le serveur ne renvoie que
`keyed` pour que l'interface sache que c'est réglé.

**Une personne, un nom.** La même identité peut être connectée sur deux téléphones ou deux onglets
(deux `Participant`). `people()` les regroupe par identité : affichée une fois, connectée si l'un
l'est, « a validé » si tous l'ont fait, lot du plus récent. Une validation vaut pour tous les
participants de même identité.

## Fusion automatique de deux vérifs du même lot global

Deux secouristes qui commencent chacun de leur côté la vérif d'un même lot global (le même sac, ou
deux armoires du même VPS) sont **réunis sans rien faire** : `_merge`, appelé après chaque `state`,
cherche les autres vérifs en cours de même `root`, ni terminées, ni `solo`, ni en cours
d'enregistrement. Il les fusionne dans **la plus ancienne** : participants déplacés (même `pid` et
même jeton), lots et clés réunis (une clé connue l'emporte sur une vide), étiquettes réunies, les
sessions absorbées retirées du suivi (`remove`). Chaque participant déplacé reçoit un nouveau
`hello` avec `merged: true` et `who` (les autres participants). La fusion ne dépasse jamais 8
participants (une session qui ferait déborder reste à part) et ne demande pas de badge, contrairement
à « Rejoindre » : les deux vérifs ont été commencées sur place, sur le même matériel.

« Quitter » la vérif commune (bandeau) ouvre une nouvelle session `solo`, que la fusion ignore : on
continue seul avec ses propres scans. `_restart` (nouvelle vérif après la fin) ne crée pas de session
`solo`.

`_changed` diffuse un changement à tous les participants **de toutes les vérifs du même lot global**,
pour que le bandeau « X vérifie aussi ce lot » (`nearby`) reste à jour même sans fusion (sessions
`solo`, ou plafond de participants atteint).

## Envoi de la vérif commune

Chacun valide sa partie ; **un seul** téléphone envoie la vérif, pour tous :

1. Le bouton « Valider ma partie de la vérif » envoie `validate` `{partial}` (attente de la réponse
   8 s au plus, sinon « Connexion à la vérif commune perdue : réessayez. »).
2. Le serveur marque `validated` (pour tous les participants de même identité) et calcule `waiting` :
   les autres participants **connectés** qui n'ont pas validé. S'il n'y en a pas, et que personne
   d'autre n'est déjà en train d'envoyer (`submitting`), il désigne ce participant
   (`submitting = pid`) et répond `submit: true` ; sinon `submit: false` et la page affiche « ✔ Votre
   partie de la vérif est validée, en attente de … ».
3. Le téléphone désigné envoie `POST /api/verifs/` avec tous les scans (`allScanned`, donc ceux des
   autres aussi) et `live: {id, pid, token}`.
4. `views.verifs` appelle `hub.verif_team(live)` : si le jeton est bon et que la vérif n'est ni
   terminée ni en cours d'enregistrement, il obtient les clés (`keys`), les étiquettes (`labels`) et
   les **identités** des participants qui comptent (celui qui envoie, ceux qui ont validé ou scanné
   quelque chose). Les clés manquantes de `lots` sont complétées, `key_holders` et `require_label`
   tiennent compte des clés et des étiquettes de la vérif commune, et `services.perform_verif`
   enregistre la vérif au nom de celui qui envoie avec les autres dans `Verifs.participants`
   (identités stockées, complètes). `report['participants']` donne les noms (courts sur l'API
   publique).
5. `hub.finish` marque la vérif terminée (`done`), envoie `report` aux autres participants connectés
   et diffuse au suivi. Le compte rendu s'affiche donc chez tout le monde.

Si l'API refuse la vérif (clé expirée, étiquette manquante…), la page envoie `unvalidate` : la
validation et `submitting` sont libérés et quelqu'un d'autre pourra envoyer.

**Les absents ne bloquent pas.** Un participant non validé ne retient l'envoi que s'il est connecté
ou déconnecté depuis moins de `GRACE` (20 s). Le faucheur (`_submitter`) repère les vérifs à
plusieurs dont tous les participants restants ont validé, et envoie `submit` au **dernier** participant
connecté qui a validé ; sa page envoie alors la vérif sans redemander de confirmation. Si le
participant désigné disparaît lui-même plus de `GRACE`, un autre est désigné au tour suivant.

## Vérif en cascade à plusieurs

Une vérif en cascade (lot global avec des sous-lots : VPS > armoires > sacs) est **une seule vérif
commune du lot global**, mais chaque sous-lot est validé à part, au fil de l'eau : deux personnes sur
deux armoires du même VPS sont dans la même vérif (fusion par `root`), chacune valide son armoire
quand elle a fini.

- La page publie `cascade: true` et son `level` (lot où elle se trouve).
- Valider un sous-lot envoie `POST /api/verifs/` avec `step: true`, `exclude` (sous-lots laissés de
  côté), `keys` (étiquettes clés d'autres lots du même lot global scannées plus tôt) et `live`.
  `verif_team(live, step=iids)` ne retient comme participants que ceux dont un scan partagé fait
  partie de ce qui est envoyé (`_entry_in`) : celui qui valide une armoire n'y inscrit pas la personne
  qui fait l'autre.
- `hub.step` note les lots validés (ou scellés intacts) dans `session.steps` (500 au plus), **retire
  des scans partagés** de tout le monde les iids envoyés, et envoie `step` `{by, lots, items}` aux
  autres participants. La vérif commune continue ; `team.done` donne à qui rejoint plus tard les lots
  déjà faits.
- Une étiquette de scellé scannée pendant la cascade est enregistrée tout de suite (`flushSeals`,
  `step` avec `sealed`), comme un petit sous-lot validé.
- Quand un participant a fini (lot global validé : `finish: true` sur le `POST`, ou « Terminer la
  vérif » : message `leave`), `leave_cascade` le déconnecte de la vérif commune. S'il reste d'autres
  participants **connectés**, il est retiré et la vérif continue sans lui. Sinon la vérif est close
  (`_close_cascade`) : affichée « Terminée, N lots validés » (avec le nombre d'incomplets) pendant
  `DONE_KEEP`, ou simplement retirée si rien n'a été validé. Des participants déconnectés ne la
  retiennent pas : tout ce qui a été validé est déjà en base.

Le serveur **n'enregistre jamais rien** pour une cascade abandonnée (`_worth_saving` renvoie faux) :
chaque lot l'a été à sa validation, le reste n'était pas fini.

## Faucheur et fin de vie d'une vérif

`reap()` tourne toutes les 10 s dans le thread `live-reaper` (protégé : une exception ne l'arrête
jamais ; il s'arrête de lui-même quand il n'y a plus de session et redémarre à la connexion
suivante). Pour chaque vérif **sans aucun participant connecté** :

| Situation | Délai | Issue |
|---|---|---|
| terminée (`done`) | `DONE_KEEP` (10 min) après la fin | retirée du suivi. |
| en cascade, au moins un lot validé | `LOST_KEEP` (15 min) | close comme terminée (`_close_cascade`), affichée puis retirée. |
| quelqu'un a validé sa partie, **ou** plus rien à scanner (`count > 0`, `remaining == 0`) | `LOST_KEEP` | **enregistrée par le serveur** (`save_unattended`). |
| sinon | `LOST_KEEP` | abandonnée, retirée du suivi. |

Pour les vérifs dont quelqu'un est encore connecté, le faucheur ne fait que désigner l'envoyeur
(`submit`, voir plus haut).

`save_unattended` enregistre la vérif comme si son dernier participant l'avait envoyée : scans de
tous (hors erreurs), lots et clés gardés par le serveur, étiquettes de la vérif, scellés scannés, au
nom de celui qui a validé (sinon du premier qui a scanné), les autres en participants. Elle repasse
par `views.verif_lots` (contrôle des clés comme sur l'API publique, étiquette clé exigée seulement pour
une identité déclarée), `sealed_targets`, `require_label`, puis `services.perform_verif`. Si quelque
chose manque (clé expirée, aucune étiquette d'un lot de la vérif, lot archivé, personne
d'identifié), la vérif est abandonnée. Pendant l'enregistrement, `saving` empêche la fusion, un
second enregistrement et un envoi concurrent par `verif_team`.

## Page de suivi : `/ws/live/watch`

1. Le premier message doit être `{"type": "hello", "user": {...}}` avec un badge valide (ou la
   session d'une connexion par nom et PIN) : `badge_identity`. Sinon `{"type": "error", "message":
   "Scannez votre badge pour suivre les vérifs.", "login_required": true}` et fermeture ; la page
   cesse de se reconnecter et affiche le message.
2. Le serveur ajoute la connexion à `watchers` et envoie `snapshot` `{sessions}` : toutes les vérifs
   visibles (qui ont publié un état, ou sont terminées).
3. Ensuite, à chaque changement : `update` `{session}` (la vérif entière, `LiveSession.public()`), ou
   `remove` `{id}` (vérif retirée, fusionnée dans une autre, ou plus rien à montrer). La page ne
   renvoie que des `ping`.

`LiveSession.public()` contient : `id`, `who`, `verified` (tous les participants badgés),
`participants` (`people()` sans `me`), `started`, `updated`, `connected`, `lot`, `lots`, `scanned`,
`count`, `remaining`, `done`. **Ni identités stockées, ni clés, ni scans détaillés** (pas d'iids) :
seulement des noms d'items.

Côté web (`renderLive`, `liveCard`), une carte par vérif : participants avec « (nom déclaré) »,
« ✔ » s'ils ont validé, « (déconnecté) », « 📍 sous-lot » en cascade ; état « En cours · dernier
scan il y a 2 min », « Connexion perdue », « Validée à … » ou « Terminée à … : N lots validés » ; les
derniers scans ; le bouton « Rejoindre cette vérif » (`joinLiveVerif`, badge exigé, avertissement si
des scans d'un autre lot seraient oubliés). Les vérifs passées ne viennent pas du WebSocket mais de
`POST /api/verifs/recent/` (badge, une ligne par vérif groupée), rechargé quand une vérif passe à
« terminée ».

## Anonymisation des noms

Les deux WebSockets sont servis par l'**API publique** (téléphones), où l'on ne montre **jamais de
nom de famille**. Tous les noms qui partent sur un WebSocket passent par `live.public_name(identity)` :

```python
with anonymized():
    return identity_name(identity)
```

`idendity.anonymized()` est un gestionnaire de contexte qui pose une `ContextVar`
(`_public_names`) le temps du bloc ; dans ce bloc, `identity_name` rend :

- pour un badge (`M:matricule`) : le **prénom**, suivi du plus court début de nom qui le distingue des
  homonymes de même prénom (`short_names` : « Julien D. », « Jeanne Dup. »), calculé sur tous les
  secouristes, anciens compris ; « Inconnu » pour un matricule absent ;
- pour un nom déclaré (`D:Paul Martin`) : premier mot et initiale du suivant (« Paul M. »).

La `ContextVar` (plutôt qu'une variable globale) isole le réglage par thread : une requête de l'API
locale servie en même temps par le même processus continue à voir les noms complets. Le nom est
calculé **une fois**, à la connexion ou au message `identity`, et rangé dans `Participant.who` ; tout ce
qui part ensuite (`team`, `hello` de fusion, `step.by`, suivi) réutilise ce `who`. L'identité
complète (`Participant.identity`) ne quitte jamais le serveur et n'est utilisée que pour enregistrer
la vérif (`Verifs.by`, `Verifs.participants`), où le poste l'affiche en entier.

Le test `test_live_names_are_anonymized` vérifie les deux formes.

## Sécurité, en résumé

- **Suivre** exige un badge valide (premier message). **Publier** ne l'exige pas : on peut vérifier un
  sac sous un nom déclaré, comme hors ligne ; ce qui est publié n'est que de l'affichage.
- **Rejoindre** une vérif exige un badge vérifié ; la **fusion** automatique non (même lot global,
  commencé sur place).
- Les identifiants d'une participation (`id`, `pid`, `token` de 24 caractères, `secrets`) ne sont
  connus que du téléphone concerné ; le jeton est comparé en temps constant.
- Les **clés** des étiquettes clés restent en mémoire du serveur, jamais renvoyées.
- L'enregistrement passe par `POST /api/verifs/` et ses contrôles habituels (clés, étiquette d'un lot
  scannée, items), ou par `save_unattended`, qui les refait.
- Messages bornés en taille (64 Kio) et en contenu (`clean_*`), nombre de sessions et de participants
  plafonné, connexions muettes coupées après 45 s.
- Pas de nom de famille ni d'iid dans ce qui est diffusé.

## Tests

`LiveHubTests` (`tests.py`) instancie un `LiveHub` neuf par test et le pilote avec `FakeSocket` (file
de messages à recevoir, liste des messages envoyés ; `None` simule la fermeture), chaque connexion
dans son thread. `badge_identity` et `public_name` sont remplacés par des faux pour ne pas toucher la
base de test depuis ces threads ; `mock.patch.object(live, 'hub', self.hub)` branche le hub de test
sur `POST /api/verifs/`. Les tests couvrent : badge du suivi, publication puis fin, reprise après
rechargement, envoi par le dernier participant, étiquette scannée par un autre, envoi par un validé
quand les autres sont partis, fusion du même lot global, même personne sur deux pages, cascade
(`step`, clôture au lot global, `leave` sans lot validé), vérif abandonnée, vérif validée ou complète
enregistrée sans participant, état borné. Le transport lui-même (`WebSocket`, vrai serveur sur un port
libre) est testé avec le téléphone-douchette.

## Modifier le protocole

- Un nouveau champ publié par la page de scan doit être ajouté **à la fois** à `liveSnapshot`
  (`app.js`) et à `clean_state` ou `clean_entries` (`live.py`), borné ; sinon il est silencieusement
  perdu.
- Un nouveau message : l'ajouter au docstring de `live.py` (qui fait référence), à la boucle de
  `run_scanner` et au `onmessage` du module `live` ; mettre à jour cette page.
- Garder les envois hors du verrou et les accès base hors du verrou suivis de
  `close_old_connections()`.
- Ne jamais renvoyer `session.lots` (les clés) ni `Participant.identity` à un client ; tout nom
  passe par `public_name`.
- Ajouter un test dans `LiveHubTests`.
