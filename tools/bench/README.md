# Banc de mesure d'une borne

`qrprotec-bench` note sur 100 un ordinateur candidat pour une **borne complète** : le back (base,
API, site des téléphones) et le kiosk (écran, douchette Bluetooth) sur la même machine.

```sh
git clone https://github.com/etyloppihacilem/qrprotec && cd qrprotec
poetry install --no-root --with push   # inutile si le RPM est installé
tools/bench/qrprotec-bench
```

Une seule commande, sans réglage : environ 5 minutes, sans rien brancher ni manipuler. La
production n'est jamais touchée : le back tourne sur une **copie** de la base, sur des ports libres,
dans un dossier de test supprimé à la fin. Il ne faut que Python 3 : le script trouve tout seul le
Python qui a Django (le sien, le `.venv` de poetry du dépôt, `poetry env`), sinon le back du RPM
(`/usr/share/qrprotec`). Il marche donc aussi sur la machine où le back tourne déjà.

Deux options seulement :

- `--db /var/lib/qrprotec/db.sqlite3` : mesure avec une copie de la vraie base (sinon base fictive
  de 80 lots) ;
- `--compare *.json` : met côte à côte les machines déjà mesurées. Chaque passage enregistre ses
  résultats dans `qrprotec-bench-<machine>-<date>.json`, dans le dossier courant.

## Le score

```
================================================================
  SCORE 78/100 : Convenable
================================================================
  Back (10 secouristes)  ███████···  17.4/25  p95 310 ms
  Disque                 ██████████  20.0/20  fdatasync p99 0.35 ms
  ...
```

**80 et plus : excellent, 60 à 79 : convenable, 40 à 59 : limite, moins de 40 : insuffisant.**
**ÉLIMINÉ** quand un point bloquant rend la machine inutilisable, quel que soit le score : pas de
Bluetooth (ou Bluetooth sans BLE, ou coupé par un interrupteur), disque à plateaux, moins de 2 Gio
de mémoire, ou back qui ne démarre pas.

| Partie | Points | Plein score | Zéro |
| --- | --- | --- | --- |
| Back : 10 secouristes simulés (fiches, état des lots, vérifs, PDF), p95 | 25 | ≤ 200 ms | ≥ 2 s, ou une erreur |
| Disque : latence fdatasync p99 (chaque vérif est un commit SQLite) | 20 | ≤ 1 ms | ≥ 20 ms |
| CPU sur un cœur (Django est en Python, un cœur à la fois) | 20 | ≤ 150 ms | ≥ 1,2 s |
| SQLite : 8 écritures simultanées, p95 | 10 | ≤ 10 ms | ≥ 300 ms, ou « locked » |
| Chauffe : lenteur après 3 min de charge sur tous les cœurs | 10 | ≤ 5 % | ≥ 35 % |
| Kiosk : carte graphique KMS (5), rendu matériel (3), carte son (2) | 10 | tout | rien |
| Suivi en direct (WebSocket), p95 | 5 | ≤ 20 ms | ≥ 300 ms |

Entre les deux bornes, la note suit une échelle logarithmique (linéaire pour la chauffe). Deux
passages sur la même machine varient de quelques points : un écart de moins de 5 points entre deux
candidats ne départage pas. À score proche, préférer la machine silencieuse, sobre (allumée en
permanence) et avec une batterie ou un onduleur.

Le Bluetooth est seulement vérifié (adaptateur présent, pas coupé, BLE si lisible) : la douchette n'a
pas besoin d'être allumée.

`QRPROTEC_BENCH_QUICK=1` raccourcit tout pour essayer le script : son score n'est pas comparable.
