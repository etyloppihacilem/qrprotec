/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          print_queue.hpp
        -\-    _|__
         |\___/  . \        Created on 29 Sep. 2026 at 16:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#pragma once

#include "../core/print_counter.hpp"
#include "../core/template.hpp"
#include "../printer/printer.hpp"
#include "../render/raster.hpp"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace qrprotec {

struct PrintJob {
    std::string      description; // ex: "Item compre20271231000000A1"
    TemplateDocument document;    // parametres deja renseignes
    RasterImage      image;       // deja rendue (etiquette envoyee par un autre ordinateur) : imprimee telle quelle
    std::string      command;     // non vide : commande brute de maintenance (calibrage...), pas d'etiquette
};

struct PrintQueueStatus {
    std::size_t pending = 0;
    std::size_t printed = 0; // depuis le dernier vidage
    std::string current;
    std::string error;
    bool        paused = false; // arretee sur une erreur, en attente de reprise
    // pause parce que l'etiquette suivante est d'un autre type que celui charge : type a charger
    std::string needed_label_type;
    std::string needed_label_summary; // « 40 x 30 mm, droite, thermique direct »
    std::string needed_for;           // etiquette qui attend ce rouleau
    bool        busy   = false;
};

// Imprime les etiquettes les unes apres les autres dans un thread dedie. En cas d'erreur (papier,
// couvercle...), la file se met en pause et garde les etiquettes restantes.
class PrintQueue {
  public:
    PrintQueue();
    ~PrintQueue();
    PrintQueue(const PrintQueue &)            = delete;
    PrintQueue &operator=(const PrintQueue &) = delete;

    // Chaque modele est adapte a l'etiquette physique (quart de tour si besoin) avant impression.
    void             enqueue(std::vector< PrintJob > jobs, const PrintSettings &settings, const PhysicalLabel &label);
    PrintQueueStatus status() const;
    void             resume();
    // Etiquettes chargees dans l'imprimante (Reglages ou « c'est charge » apres une pause) : reprend la file
    // si elle attendait ce type
    void             set_loaded_label_type(const std::string &name);
    void             cancel();
    // Commande de maintenance (TSPL / ZPL brut) passee dans la file, apres les etiquettes en attente
    void             send_command(const std::string &description, const std::string &command, const PrintSettings &settings);
    // Compteur d'impressions : chaque etiquette acceptee par l'imprimante y est ajoutee (avant la premiere impression)
    void             set_counter(PrintCounter *counter);

  private:
    void run();

    mutable std::mutex      mutex_;
    std::condition_variable condition_;
    std::deque< PrintJob >  jobs_;
    PrintSettings           settings_;
    PhysicalLabel           label_;
    PrintQueueStatus        status_;
    bool                    stopping_ = false;
    AnyPrinter              printer_;
    PrintCounter           *counter_ = nullptr;
    std::thread             thread_;
};

} // namespace qrprotec
