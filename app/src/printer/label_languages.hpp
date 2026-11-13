#pragma once

#include "../render/raster.hpp"

#include <cstdint>
#include <string>
#include <vector>

// Langages des imprimantes d'etiquettes « de bureau » : TSPL (TSC, IDPRT, Munbyn, Phomemo, Godex en
// emulation...) et ZPL (Zebra). Le poste rend deja l'etiquette en image : on l'envoie d'un bloc
// (BITMAP en TSPL, ^GFA en ZPL), sans police ni code-barres de l'imprimante.
namespace qrprotec::labels {

struct RawJob {
    int width_dots = 0;    // largeur de l'image (sens de la tete)
    int height_dots = 0;   // hauteur de l'image (sens du defilement)
    double dots_per_mm = 8.0;
    int copies = 1;
    int darkness = 8;      // TSPL DENSITY 0-15 ; ZPL ~SD 0-30 (double de la valeur)
    bool ribbon = true;    // transfert thermique (ruban) ; sinon thermique direct
    double gap_mm = 2.0;   // espace entre deux etiquettes (TSPL GAP)
    int offset_x_dots = 0; // decalage horizontal : positif = vers la droite (ZPL ^LS, TSPL SHIFT x)
    int offset_y_dots = 0; // decalage vertical : positif = vers le bas (ZPL ^LT, TSPL SHIFT y)
    int tear_off_dots = 0; // position d'arret apres impression : positif = plus sorti (ZPL ~TA, TSPL OFFSET)
};

// Lignes de l'image en octets, bit de poids fort a gauche. `one_is_black` : ZPL (1 = point noir) ;
// TSPL attend l'inverse (0 = point noir).
std::vector<std::uint8_t> pack_rows(const RasterImage& image, bool one_is_black);

std::vector<std::uint8_t> encode_tspl(const RasterImage& image, const RawJob& job);
std::vector<std::uint8_t> encode_zpl(const RasterImage& image, const RawJob& job);

// Operations de maintenance lancees depuis les Reglages du poste
enum class Maintenance {
    Calibrate, // envoie le format (largeur, longueur, interstice, support) puis mesure l'interstice ; enregistre
    Config,    // imprime l'etiquette de configuration de l'imprimante
    Feed,      // avance d'une etiquette
    Factory,   // reglages d'usine (hors reseau) enregistres ; a faire suivre d'un calibrage
    Diagnostic, // ZPL : 3 etiquettes en commandes natives (cadre, texte) : reference, ^LH, ^LS + ^LT
};

// Commande brute pour l'operation. Le calibrage se sert de la taille, du support, de l'interstice (TSPL) et, en
// ZPL, des positions (^LS, ^LT, ~TA) pour que la configuration enregistree corresponde aux impressions.
std::string maintenance_command(bool zpl, Maintenance action, const RawJob& job);

// Etat de l'imprimante lu par ~HS (ZPL, « host status ») ou <ESC>!? (TSPL)
struct PrinterState {
    bool known = false; // false : pas de reponse (liaison a sens unique, imprimante eteinte...)
    bool head_open = false;
    bool paper_out = false;
    bool paper_jam = false;  // TSPL
    bool ribbon_out = false;
    bool paused = false;
    bool printing = false;   // TSPL
    bool buffer_full = false; // ZPL
    bool over_temperature = false;
    bool under_temperature = false;
    bool other_error = false; // TSPL
    int formats_waiting = 0;  // ZPL : formats recus pas encore imprimes
    int labels_remaining = 0; // ZPL : etiquettes restant a imprimer dans le lot en cours

    // l'imprimante n'imprimera pas tant que ce n'est pas regle
    bool blocking() const;
    std::string description() const;
};

std::vector<std::uint8_t> status_request(bool zpl);
PrinterState parse_status(bool zpl, const std::vector<std::uint8_t>& reply);

} // namespace qrprotec::labels
