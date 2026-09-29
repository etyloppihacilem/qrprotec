/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          print_queue.hpp
        -\-    _|__
         |\___/  . \        Created on 29 Sep. 2026 at 16:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#pragma once

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
};

struct PrintQueueStatus {
    std::size_t pending = 0;
    std::size_t printed = 0; // depuis le dernier vidage
    std::string current;
    std::string error;
    bool        paused = false; // arretee sur une erreur, en attente de reprise
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
    void             cancel();

  private:
    void run();

    mutable std::mutex      mutex_;
    std::condition_variable condition_;
    std::deque< PrintJob >  jobs_;
    PrintSettings           settings_;
    PhysicalLabel           label_;
    PrintQueueStatus        status_;
    bool                    stopping_ = false;
    NiimbotB1Printer        printer_;
    std::thread             thread_;
};

} // namespace qrprotec
