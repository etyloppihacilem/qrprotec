# Scénarios de vérif communs

La répartition des items scannés entre les lots d'une vérif, et le calcul des lots complets, existent
trois fois : au serveur (fait foi), au front web et au poste (aperçus). Chaque fichier `*.json` de ce
dossier est un cas de vérif, que les trois jouent et dont ils doivent donner le même résultat :

| Côté | Code joué | Lancer |
|---|---|---|
| Serveur | `services.assign_items`, `services._complete_lots`, `services.perform_verif` | `python manage.py test inventory.tests.VerifScenariosTests` |
| Front web | `planVerif` (`database/inventory/web/app/verif-plan.js`) | `node --test tests/verif-scenarios/web.test.js` (aussi lancé par le test Django si `node` est installé) |
| Poste | `plan_verif`, `discard_check` (`app/src/core/verif_plan.cpp`) | `ctest` (`QRProtecVerifScenariosTest`) |

Une règle de vérif qui change, ou un écart trouvé entre deux côtés : on écrit d'abord le scénario,
puis on corrige les trois jusqu'à ce qu'il passe partout.

## Format

```json
{
  "description": "Ce que le cas montre, en une phrase.",
  "types": {
    "compre": {},
    "ciseau": {"perissable": false, "consumable": false},
    "pansdc": {"tear_off": true},
    "gobele": {"physical": false}
  },
  "lots": [
    {"id": "bplus"},
    {"id": "soin", "parent": "bplus", "requirements": {"compre": 2}, "items": [{"id": "c1", "type": "compre"}]},
    {"id": "tiroir", "storage": true, "items": [{"id": "c2", "type": "compre", "expired": true}]}
  ],
  "elsewhere": [{"id": "c9", "type": "compre"}],
  "scans": [
    {"item": "c1", "place": "soin"},
    {"item": "c9"},
    {"check": "gobele", "lot": "soin"}
  ],
  "expected": {
    "target": {"c1": "soin", "c9": "soin"},
    "complete": ["bplus", "soin"],
    "discard": []
  }
}
```

- **types** : types d'items, codes de 6 caractères. Options : `perissable` (vrai par défaut), `consumable`
  (vrai), `tear_off` (étiquette à déchirer, faux), `physical` (faux : item à cocher, sans étiquette).
- **lots** : le premier est le lot vérifié, les suivants sont ses sous-lots dans l'ordre de la vérif
  (`parent`). `requirements` donne le contenu attendu par type, `items` les items rangés dedans, et
  `"expired": true` marque un item périmé. `storage` désigne un rangement du stock.
- **elsewhere** : items hors des lots de la vérif (stock).
- **scans** : dans l'ordre. `place` est le lot dont l'étiquette a été scannée en dernier avant l'item.
  `check` est un item à cocher, coché dans `lot`.
- **expected** : `target` donne le lot qui reçoit chaque item scanné, `complete` les lots complets avec ces
  scans (vérif partielle possible), `discard` les périmés à jeter (le lot a son compte sans eux, ou ils n'y
  étaient pas).

Les identifiants (`soin`, `c1`) ne servent qu'au scénario : chaque côté crée ses propres lots et iids.
Ne pas toucher aux modèles `.qr` pour un scénario.
