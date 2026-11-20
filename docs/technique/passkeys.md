# Passkeys du front web

Un secouriste peut se connecter au front web avec une **passkey** (WebAuthn) : une clé gardée par son
téléphone, débloquée par l'empreinte, le visage ou le code du téléphone. Pas de carte ni de code PIN à
saisir.

Fichiers concernés :

| Fichier | Rôle |
|---|---|
| `database/inventory/passkeys.py` | Vérification WebAuthn sans bibliothèque dédiée : CBOR minimal, clés COSE (ES256, Ed25519, RS256), défis signés à usage unique, domaine (RP ID) et origine. |
| `database/inventory/views/passkeys.py` | Routes : connexion, passkeys de l'utilisateur, QR code de connexion au poste, outil de debug des admins. |
| `database/inventory/models.py` | `Passkey` : identifiant, clé publique COSE, compteur de signatures, appareil, dates. |
| `database/inventory/web/app/passkey.js` | Front web : création, connexion, réglages, bouton du QR code pour le poste. |
| `database/inventory/tests/test_passkeys.py` | Faux authentificateur (vraie clé P-256) et tous les cas de refus. |

## Règles

- **Création** : proposée après une connexion à qui n'en a pas (pas sur un web partagé, pas pour une
  carte d'invité, pas au milieu d'une vérif). « Plus tard » la repousse de 30 jours, sauf après une
  connexion par le **QR code de dépannage** du TECH : c'est le parcours de la première connexion d'un
  secouriste sans carte, la passkey lui est proposée tout de suite. Aussi dans ⚙ Réglages > Passkey.
- **Une passkey coupe la connexion par nom (ou matricule) et PIN** pour cette personne
  (`name_login_candidates_queryset`). Le refus le dit seulement si le PIN tapé est juste
  (`passkey_required`) : un inconnu n'apprend rien. **Seulement sur le front web** : un poste (kiosk sur
  l'API locale, ou poste distant authentifié par sa clé de front, `is_front`) garde la connexion par nom
  et PIN, la clé du poste suffisant à protéger le code. **La carte reste valable**, avec son PIN s'il est
  demandé à chaque connexion. Le QR code de dépannage du TECH marche toujours.
- **Suppression** : par l'utilisateur (⚙ Réglages), par un admin dans l'outil de debug du web (toucher
  le nom du secouriste dans « Cartes des secouristes ») ou au poste (Utilisateurs > Passkeys). Sans
  passkey, le nom et le PIN remarchent. Le journal note chaque création et suppression. Les passkeys
  ne passent pas par l'annulation du journal (`undo.IGNORED_MODELS`) : une passkey supprimée ne revient
  jamais.
- **Passkey d'abord** : sur un téléphone dont la dernière connexion était une passkey (localStorage
  `qrprotec.last-login`, jamais sur un web partagé), toute demande de connexion essaie la passkey
  avant d'ouvrir la fenêtre des autres moyens (carte, nom + PIN). Annulée ou refusée, la fenêtre
  s'ouvre comme d'habitude ; une passkey inconnue du serveur (supprimée) n'est plus essayée d'abord.
- **QR code de connexion au poste** : le petit bouton ▦ à côté du nom, sur l'accueil, n'apparaît que
  pour une session **ouverte avec une passkey** (jeton de session marqué `pk`). Il affiche
  `badge?m=…&t=…`, valable 5 min, une seule fois, accepté seulement par un poste (douchette). Le poste
  ouvre une session normale. Jamais pour un admin (carte et PIN au poste).

## Domaine (RP ID)

Une passkey est liée à un nom de domaine. Le serveur prend `QRPROTEC_PASSKEY_RP_ID`, sinon le nom
d'hôte de `QRPROTEC_PUBLIC_BASE_URL`. **Changer de domaine rend inutilisables les passkeys déjà
créées** (les supprimer et en recréer). L'origine envoyée par le navigateur doit être ce domaine ou un
sous-domaine, en HTTPS.

Le navigateur n'accepte les passkeys que dans un contexte sécurisé : **HTTPS avec un certificat
valide**, ou `http://localhost`. Une adresse IP (192.168.x.x) ne marche pas, ni le certificat
auto-signé de `manage.py serve --https` (Chrome refuse WebAuthn après une erreur de certificat).

## Déboguer

1. **Sur l'ordinateur, sans téléphone.** `QRPROTEC_DEBUG=1 python manage.py serve` puis
   `http://localhost:8000/` : en développement sur localhost, le domaine des passkeys est `localhost`.
   Dans Chrome : DevTools > ⋮ > More tools > **WebAuthn** > « Enable virtual authenticator
   environment », protocole ctap2, transport internal, cocher « Supports resident keys » et « Supports
   user verification ». Le panneau liste les passkeys créées, leur compteur, et permet de les effacer
   ou de simuler un refus de l'empreinte (décocher « User verified »).
2. **Côté serveur.** Chaque refus est écrit dans le journal (`journalctl -u qrprotec`) avec sa raison :
   `WARNING inventory.passkeys : passkey refusée (connexion de M001) depuis … : signature fausse`,
   « origine … hors du domaine », « passkey créée pour un autre domaine », « défi déjà utilisé »,
   « compteur de signatures … (passkey clonée ?) », « identifiant inconnu (passkey supprimée par un
   admin ?) ». Le téléphone ne reçoit qu'un message général. `POST /api/auth/passkey/options/`
   avec `{"debug_rp": true}` répond le domaine vu par le serveur.
3. **Tests automatiques.** `python manage.py test inventory.tests.test_passkeys` : le faux
   authentificateur signe comme un téléphone ; ajouter un cas là plutôt que de tester à la main.
4. **Automatiser un navigateur.** Playwright + CDP : `WebAuthn.enable` puis
   `WebAuthn.addVirtualAuthenticator` avec `hasResidentKey`, `hasUserVerification`, `isUserVerified`
   et `automaticPresenceSimulation`.

## Routes

| Route | Corps | Réponse |
|---|---|---|
| `POST /api/auth/passkey/options/` | `{}` | `{"publicKey"}` pour `navigator.credentials.get()` |
| `POST /api/auth/passkey/` | `{"credential"}` | comme `/api/auth/`, avec `passkey_session` |
| `POST /api/me/passkeys/` | `{"user", "action": list\|options\|create\|delete, "credential"?, "id"?}` | `{"passkeys"}` (ou `{"publicKey"}`) |
| `POST /api/me/poste-login/` | `{"user"}` (session passkey) | `{"url", "expires", "seconds"}` |
| `POST /api/debug/passkeys/` | `{"user"` (admin)`, "matricule", "action"?: list\|delete}` | `{"passkeys"}` |
| `PATCH /api/users/<matricule>/` (poste) | `{"delete_passkeys": true}` | la fiche (`passkeys` : nombre) |
