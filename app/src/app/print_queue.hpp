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
    bool             status_query = false; // lecture de l'etat de l'imprimante, pas d'etiquette
    bool             read_reply   = false; // command : reponse de l'imprimante gardee dans printer_reply
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
    // dernier etat lu aupres de l'imprimante TSPL / ZPL (bouton « Lire l'état » des Reglages)
    std::string printer_state;
    bool        printer_state_known    = false;
    bool        printer_state_blocking = false;
    // reponse a la derniere commande avec retour (configuration relue par ^HH), ou erreur
    std::string printer_reply;
    std::string printer_reply_error;
    int         printer_reply_count = 0; // augmente a chaque reponse : l'interface sait qu'elle est nouvelle
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
    // Commande de maintenance (TSPL / ZPL brut) : passe avant les etiquettes en attente, meme file en pause
    void             send_command(const std::string &description, const std::string &command, const PrintSettings &settings);
    // Commande dont la reponse est lue (^HH...) ; resultat dans status().printer_reply
    void             send_command_read(const std::string &description, const std::string &command, const PrintSettings &settings);
    // Lecture de l'etat de l'imprimante, dans la file ; resultat dans status().printer_state
    void             request_printer_state(const PrintSettings &settings);
    // Compteur d'impressions : chaque etiquette acceptee par l'imprimante y est ajoutee (avant la premiere impression)
    void             set_counter(PrintCounter *counter);

  private:
    void        run();
    static bool is_maintenance(const PrintJob &job) { return job.status_query || !job.command.empty(); }
    void        push_maintenance(PrintJob job); // mutex_ tenu

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
