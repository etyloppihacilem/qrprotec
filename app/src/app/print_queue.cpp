/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          print_queue.cpp
        -\-    _|__
         |\___/  . \        Created on 29 Sep. 2026 at 16:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "print_queue.hpp"

#include "../render/raster.hpp"

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

void PrintQueue::enqueue(std::vector< PrintJob > jobs, const PrintSettings &settings) {
  {
    std::lock_guard< std::mutex > lock(mutex_);
    if (jobs_.empty() && !status_.busy)
      status_.printed = 0;
    settings_ = settings;
    for (PrintJob &job : jobs)
      jobs_.push_back(std::move(job));
    status_.pending = jobs_.size();
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

void PrintQueue::run() {
  bool connected = false;
  for (;;) {
    PrintJob      job;
    PrintSettings settings;
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
      status_.busy    = true;
      status_.current = job.description;
    }
    std::string error;
    bool        ok = connected || printer_.connect(settings, error);
    if (ok) {
      connected                  = true;
      const RasterImage   image  = render_template(job.document);
      const PrintRequest request{ image, job.document.media, settings };
      ok = printer_.print(request, {}, error);
    } else {
      error = "Connexion imprimante : " + error;
    }
    std::lock_guard< std::mutex > lock(mutex_);
    if (ok) {
      if (!jobs_.empty())
        jobs_.pop_front();
      ++status_.printed;
    } else {
      status_.paused = true;
      status_.error  = job.description + " : " + error;
      printer_.disconnect();
      connected = false;
    }
    status_.pending = jobs_.size();
  }
  if (connected)
    printer_.disconnect();
}

} // namespace qrprotec
