# Banc de mesure d'une borne

`qrprotec-bench` compare des ordinateurs candidats pour une **borne complète** : le back (base,
API, site des téléphones) et le kiosk (écran, douchette Bluetooth) sur la même machine. Il ne
demande que Python 3 (bibliothèque standard), et utilise le back installé par le RPM
(`/usr/share/qrprotec`) ou celui du dépôt (`database/`, avec Django installé).

La production n'est jamais touchée : le back est lancé sur une **copie** de la base, sur des ports
libres, dans un dossier de test supprimé à la fin.

## Lancer

Sur chaque candidat, avec la même Fedora, écran de la borne branché et douchette allumée (non
appairée au système) :

```sh
git clone https://github.com/etyloppihacilem/qrprotec && cd qrprotec
pip install --user "django>=6.1,<7" "djangorestframework>=3.18,<4"   # inutile si le RPM est installé
tools/bench/qrprotec-bench --bt-scan 15 --endurance 30 --name pc-a --json pc-a.json
```

Puis, sur n'importe quelle machine :

```sh
tools/bench/qrprotec-bench --compare pc-a.json pc-b.json pc-c.json
```

La meilleure valeur de chaque ligne est en vert. Options utiles :

| Option | Effet |
| --- | --- |
| `--db /var/lib/qrprotec/db.sqlite3` | mesure avec une copie de la vraie base (sinon base fictive de `--lots` lots) |
| `--bt-scan 15` | cherche les appareils BLE pendant 15 s (portée de l'antenne, douchette vue) |
| `--endurance 30` | 30 min de charge sur tous les cœurs : ralentissement en chauffant, température |
| `--levels 1,5,10,20` | nombres de secouristes simulés, `--seconds` la durée de chaque palier |
| `--only bluetooth,kiosk` | seulement certaines sections |
| `--dir` | dossier de test : doit être sur le disque de la base (défaut `/var/lib/qrprotec` ou `/var/tmp`) |

`sudo` n'est utile que pour lire la version Bluetooth exacte et la prise en charge du BLE (`btmgmt`).
Le code de sortie vaut 1 si un point bloquant a été trouvé.

## Ce qui est mesuré, et comment lire

Faire 2 ou 3 passages par machine et garder le plus représentatif : les écarts de quelques % ne
veulent rien dire. Par ordre d'importance :

1. **Bluetooth** : adaptateur présent, **BLE** pris en charge (Bluetooth 4.0 minimum, 5.0 conseillé),
   pas bloqué par rfkill, service `bluetooth` actif. Sans BLE, la douchette Inateck ne peut pas se
   connecter : machine éliminée (ou prévoir un dongle USB Bluetooth 5). `--bt-scan` montre aussi les
   appareils déjà connectés au système, qui peuvent faire échouer la recherche du SDK Inateck.
2. **Disque, fdatasync p99** : chaque vérif validée est un commit SQLite. Moins de 2 ms : bon SSD.
   Plus de 10 ms : carte SD, eMMC ou SSD bas de gamme, à éviter. Un disque à plateaux est bloquant.
3. **Back, secouristes simulés** : chacun enchaîne état des lots, fiche d'un lot, vérif complète
   (écriture) et parfois la fiche PDF, une action toutes les 1 à 3 s (bien plus soutenu qu'en vrai).
   Bon si p95 < 200 ms à 10 secouristes, **toujours 0 erreur**. Le détail par action est donné.
4. **Suivi en direct (WebSocket)** : délai entre l'état publié par une page de scan et sa réception
   sur les pages de suivi. Bon sous 50 ms.
5. **CPU un cœur** : Django tourne en Python, sur un cœur à la fois (GIL) : c'est ce chiffre qui
   compte, pas le nombre de cœurs. « Tous les cœurs » montre le turbo perdu sous charge.
6. **Endurance** : ralentissement après la charge prolongée. Plus de 25 % : refroidissement
   insuffisant (mini PC passif dans une armoire, par exemple).
7. **Kiosk** : carte graphique KMS (pour Cage), rendu matériel `/dev/dri` (sinon ImGui en logiciel),
   écran détecté, rétroéclairage pilotable (veille de l'écran), carte son (bips).

Le reste (mémoire, démarrage, sauvegarde, VACUUM) départage rarement : 4 Gio de mémoire suffisent.
À puissance proche, préférer la machine silencieuse, sobre (allumée en permanence) et avec un bon
SSD ; une batterie (portable) ou un onduleur protège la base des coupures de courant.
