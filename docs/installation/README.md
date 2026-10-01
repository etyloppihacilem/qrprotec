# Guide d'installation

## Choisir son mode d'installation

QRProtec est fait d'un **back** (la base de données et le site pour les téléphones) et d'un **front**
(le logiciel du poste, avec douchette et imprimante). Ils peuvent tourner sur la même machine ou sur
des machines différentes.

| Mode | Paquet | Pour quoi faire | Guide |
|---|---|---|---|
| **Borne complète (front + back, kiosk)** | `qrprotec` | Un mini-PC dédié dans le local matériel, qui démarre directement sur le logiciel en plein écran. **Le cas le plus courant.** | [borne-complete.md](borne-complete.md) |
| **Back seul** | `qrprotec-server` | Un serveur (VM, serveur de l'association) qui héberge la base et le site des téléphones, sans écran. Les postes s'y connectent à distance. | [back-seul.md](back-seul.md) |
| **Front seul, mode kiosk** | `qrprotec-kiosk` | Une borne dédiée (plein écran) qui se connecte à un back installé ailleurs. | [front-seul.md](front-seul.md#borne-kiosk-reliée-à-un-back-distant) |
| **Front seul, poste de bureau** | `qrprotec-front` | Le logiciel du poste installé comme une application ordinaire sur un ordinateur qui sert aussi à autre chose. | [front-seul.md](front-seul.md#poste-de-bureau-sans-kiosk) |

Le paquet `qrprotec-common` (assistant `qrprotec-setup`, sauvegardes) est installé automatiquement
avec `qrprotec-server` ou `qrprotec-kiosk`. `qrprotec` n'est qu'un regroupement de
`qrprotec-server` + `qrprotec-kiosk`.

**Comment le front parle au back.** Par défaut, le front utilise l'API locale de sa propre machine
(`http://127.0.0.1:8001`, port jamais exposé, sans clé). Un front sur une autre machine passe par
Caddy en HTTPS avec **sa propre clé d'API**, créée sur le serveur et révocable. Les deux modes
fonctionnent en même temps sur la même base.

On peut combiner : une borne complète au local **et** un poste de bureau chez le responsable
logistique connecté au même back ; ou un back sur un serveur et plusieurs bornes front.

Pour toutes les installations : [exploitation.md](exploitation.md) (configuration, sauvegardes,
mises à jour, journaux, désinstallation).

Pour développer ou tester depuis les sources (sans paquet) : voir
[la documentation technique](../technique/maintenance.md#environnement-de-développement).

## Prérequis communs

- **Fedora 42 ou plus récent**, x86_64. Pour une borne : **Fedora Server**, installation minimale
  (« Minimal Install », sans bureau). Pour un poste de bureau : Fedora Workstation convient.
  aarch64 est possible en construisant le paquet soi-même, sans le SDK Bluetooth de la douchette
  (mode clavier seulement).
- Un accès réseau pour `dnf` pendant l'installation et les mises à jour.

## Matériel

| Matériel | Rôle | Remarques |
|---|---|---|
| Mini-PC x86_64 | Borne | 4 Go de RAM et 32 Go de disque suffisent largement |
| Écran + clavier | Borne | Le clavier sert peu (saisie de dates, de noms) |
| Douchette **Inateck** (Bluetooth) | Scans au poste | Toute douchette en **mode clavier (HID)** fonctionne aussi, sans retour d'erreur sur la douchette |
| Imprimante **Niimbot B1** (USB) | Étiquettes | Rouleau conseillé 40 × 30 mm. Elle apparaît en `/dev/ttyACM0` |
| Smartphones | Vérifs sur le terrain | Navigateur récent, appareil photo. HTTPS obligatoire |
