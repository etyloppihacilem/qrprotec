/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          print_queue.cpp
        -\-    _|__
         |\___/  . \        Created on 29 Sep. 2026 at 16:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "print_queue.hpp"
#include "../printer/logging.hpp"

#include "../render/raster.hpp"

#include <algorithm>

namespace qrprotec {

PrintQueue::PrintQueue() : thread_(&PrintQueue::run, this) {}

PrintQueue::~PrintQueue() {
  {
    std::lock_guard< std::mutex > lock(mutex_);
    stopping_ = true;
  }
  condition_.notify_all();
  if (thread_.joinable())
    thread_.join();
}

void PrintQueue::enqueue(std::vector< PrintJob > jobs, const PrintSettings &settings, const PhysicalLabel &label) {
  {
    std::lock_guard< std::mutex > lock(mutex_);
    if (jobs_.empty() && !status_.busy)
      status_.printed = 0;
    settings_ = settings;
    label_    = label;
    for (PrintJob &job : jobs)
      jobs_.push_back(std::move(job));
    status_.pending = jobs_.size();
  }
  condition_.notify_one();
}

void PrintQueue::set_loaded_label_type(const std::string &name) {
  {
    std::lock_guard< std::mutex > lock(mutex_);
    settings_.loaded_label_type = name;
    if (status_.paused && !status_.needed_label_type.empty() && status_.needed_label_type == name) {
      status_.paused = false;
      status_.error.clear();
      status_.needed_label_type.clear();
    }
  }
  condition_.notify_one();
}

PrintQueueStatus PrintQueue::status() const {
  std::lock_guard< std::mutex > lock(mutex_);
  return status_;
}

void PrintQueue::resume() {
  {
    std::lock_guard< std::mutex > lock(mutex_);
    status_.paused = false;
    status_.error.clear();
    status_.needed_label_type.clear();
  }
  condition_.notify_one();
}

void PrintQueue::cancel() {
  std::lock_guard< std::mutex > lock(mutex_);
  jobs_.clear();
  status_.pending = 0;
  status_.paused  = false;
  status_.error.clear();
}

void PrintQueue::set_counter(PrintCounter *counter) {
  std::lock_guard< std::mutex > lock(mutex_);
  counter_ = counter;
}

void PrintQueue::run() {
  bool connected = false;
  for (;;) {
    PrintJob      job;
    PrintSettings settings;
    PhysicalLabel label;
    PrintCounter *counter = nullptr;
    {
      std::unique_lock< std::mutex > lock(mutex_);
      if (jobs_.empty() || status_.paused) {
        if (connected) {
          printer_.disconnect();
          connected = false;
        }
        status_.busy = false;
        status_.current.clear();
      }
      condition_.wait(lock, [this] { return stopping_ || (!jobs_.empty() && !status_.paused); });
      if (stopping_)
        break;
      job             = jobs_.front();
      settings        = settings_;
      label           = label_;
      counter         = counter_;
      status_.busy    = true;
      status_.current = job.description;
    }
    std::string   error;
    RasterImage   image;
    MediaSettings media;
    label.pixels_per_mm = job.document.media.pixels_per_mm;
    // categorie comptee : rouleau du type du modele, sinon etiquette des Reglages et ruban de l'imprimante
    double       counted_width  = label.width_mm;
    double       counted_height = label.height_mm;
    LabelSupport counted_support =
      settings.kind != PrinterKind::NiimbotB1 && settings.raw.ribbon ? LabelSupport::Transfer : LabelSupport::Direct;
    if (const std::optional< LabelType > &type = job.document.label_type) {
      // modele avec son type d'etiquette : il faut ce rouleau dans l'imprimante
      if (type->name != settings.loaded_label_type) {
        std::lock_guard< std::mutex > lock(mutex_);
        status_.paused            = true;
        status_.needed_label_type = type->name;
        status_.error             = job.description + " : chargez les étiquettes « " + type->name + " » ("
                      + type->summary() + "), puis confirmez dans la file d'impression.";
        status_.pending           = jobs_.size();
        if (connected) {
          printer_.disconnect();
          connected = false;
        }
        continue;
      }
      label = physical_label_for(*type, job.document.media.pixels_per_mm);
      settings.raw.ribbon = type->support == LabelSupport::Transfer;
      counted_width       = type->width_mm;
      counted_height      = type->height_mm;
      counted_support     = type->support;
    }
    bool ok = fit_to_label(job.image.width > 0 ? job.image : render_template(job.document), label, image, media, error);
    if (ok) {
      ok = connected || printer_.connect(settings, error);
      if (ok) {
        connected = true;
        const PrintRequest request{ image, media, settings };
        ok = printer_.print(request, {}, error);
        // compte des que l'imprimante a accepte l'envoi (erreur, pause, annulation : pas comptees)
        if (ok && counter != nullptr)
          counter->add(local_day_iso(), counted_width, counted_height, counted_support, std::max(1, settings.copies));
      } else {
        error = "Connexion imprimante : " + error;
      }
    }
    std::lock_guard< std::mutex > lock(mutex_);
    if (ok) {
      if (!jobs_.empty())
        jobs_.pop_front();
      ++status_.printed;
    } else {
      status_.paused = true;
      status_.error  = job.description + " : " + error;
      error_log("Impression en pause : " + status_.error, "imprimante");
      printer_.disconnect();
      connected = false;
    }
    status_.pending = jobs_.size();
  }
  if (connected)
    printer_.disconnect();
}

} // namespace qrprotec
