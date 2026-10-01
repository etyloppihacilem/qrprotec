# Documentation technique

Cette partie s'adresse à la personne qui doit modifier QRProtec : corriger un bug, ajouter une règle
de gestion, changer d'imprimante, ou simplement comprendre pourquoi le code est fait ainsi. Elle
suppose de savoir lire du Python (Django), du C++17 et un peu de JavaScript, mais pas de connaître le
projet.

Lire dans l'ordre :

1. [Architecture d'ensemble](architecture.md) : les trois programmes, comment ils communiquent, ce
   qui tourne où. **À lire en premier.**
2. [Modèle de données et identifiants](donnees.md) : les tables, le format des identifiants et des
   QR codes, les clés.
3. [Règles de gestion](regles-de-gestion.md) : la vérif, le réassort, les sous-lots, les scellés,
   l'état d'un lot. C'est le cœur métier, implémenté en trois endroits qui doivent rester cohérents.
4. [Back (Django)](back.md) : organisation de `database/`, les deux API, l'authentification, les
   notifications, le relais WebSocket, les tests.
5. [Front poste (ImGui, C++)](front-imgui.md) : organisation de `app/`, threads, fenêtres, pile de
   scans, impression des étiquettes, douchette.
6. [Front web (téléphone)](front-web.md) : la page unique servie aux adresses des QR codes.
7. [Packaging et déploiement](packaging.md) : RPM Fedora, systemd, Caddy, kiosk, sauvegardes, CI.
8. [Choix de conception](decisions.md) : le journal des décisions, avec pour chacune le contexte,
   l'alternative écartée et la raison. À relire avant de « simplifier » quelque chose.
9. [Recettes de maintenance](maintenance.md) : comment faire les modifications courantes sans rien
   casser (ajouter un champ, une route, un placeholder, une règle de vérif…), et le mode d'emploi du
   développement local.

## Carte du dépôt

```
qrprotec/
├── database/                 back Django (base SQLite + API + front web)
│   ├── qrprotecDB/           projet Django : settings, urls publiques / locales, wsgi
│   └── inventory/            l'application unique
│       ├── models.py         tables
│       ├── services.py       logique métier (vérifs, mouvements, stock)
│       ├── views.py          routes de l'API (fines : valident puis appellent services)
│       ├── serializers.py    modèles -> JSON, état des lots et des lots globaux
│       ├── middleware.py     choix API publique / locale selon le port
│       ├── idendity.py       format des identités (« M:matricule » / « D:nom »)
│       ├── notifications.py  SMS Free Mobile + seuils de stock
│       ├── webpush.py        notifications web (RFC 8291 / VAPID)
│       ├── remote_scanner.py relais WebSocket téléphone -> poste
│       ├── web/              front web (HTML/JS/CSS sans framework, jsQR fourni)
│       ├── management/commands/  serve, createadmin, check_alerts
│       ├── migrations/
│       └── tests.py
├── app/                      front poste ImGui (C++17, CMake)
│   ├── src/core/             sans interface : JSON, dates et QR codes, modèles d'étiquettes
│   ├── src/net/              HTTP, client d'API asynchrone, WebSocket
│   ├── src/printer/          protocole Niimbot B1 sur port série
│   ├── src/render/           rendu des étiquettes en image 1 bit
│   ├── src/inateck/          douchette : SDK Bluetooth et mode clavier (HID)
│   ├── src/app/              état de l'application (App), pile de scans, réglages, impression
│   ├── src/ui/               fenêtres ImGui
│   ├── templates/            modèles d'étiquettes *.qr fournis (+ images/)
│   ├── tests/                tests unitaires (ctest)
│   ├── imgui, scanner_lib, inateck_sdk   sous-modules git
│   └── third_party/          qrcodegen (Nayuki), stb_image, police DejaVu
├── packaging/                RPM Fedora, unités systemd, scripts d'administration
├── .github/workflows/rpm.yml construction et publication des RPM
├── Makefile                  make rpm, make lint, make icon
└── bootstrap.sh              installation en une commande sur Fedora
```

## Conventions du code

- **Langue** : commentaires, messages et documentation en français. Les commentaires du code sont
  souvent écrits sans accents (héritage du début du projet) ; les messages affichés à l'utilisateur
  ont leurs accents.
- **En-têtes** : chaque fichier source commence par le cartouche ASCII de l'auteur (`hmelica`). Le
  garder sur les nouveaux fichiers.
- **Pas de dépendance superflue** : Django et Django REST Framework côté back (plus
  `cryptography` en option), rien côté web (pas de framework, pas de CDN), ImGui + GLFW + libpng +
  FreeType côté C++. Les protocoles simples (WebSocket, JSON, HTTP, Web Push) sont réimplémentés
  plutôt que d'ajouter une bibliothèque. Voir [decisions.md](decisions.md#peu-de-dépendances).
- **La logique métier vit dans `services.py`**. Les vues valident l'entrée et appellent un service ;
  les fronts n'écrivent jamais directement l'état, ils envoient des scans.
- **Tests** : `python manage.py test inventory` (back) et `ctest` (front). Le build RPM lance les
  deux : un test rouge bloque la publication.
