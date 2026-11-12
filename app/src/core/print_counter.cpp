/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          print_counter.cpp
        -\-    _|__
         |\___/  . \        Created on 12 Nov. 2026 at 17:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "print_counter.hpp"

#include <cmath>
#include <ctime>
#include <fstream>
#include <random>
#include <sstream>

namespace qrprotec {

namespace {

double tenth(double value) {
  return std::round(value * 10.0) / 10.0;
}

void merge(std::vector< PrintTally > &tallies, const PrintTally &tally) {
  for (PrintTally &existing : tallies)
    if (existing.day == tally.day && existing.width_mm == tally.width_mm && existing.height_mm == tally.height_mm
        && existing.support == tally.support) {
      existing.count += tally.count;
      return;
    }
  tallies.push_back(tally);
}

Json tallies_json(const std::vector< PrintTally > &tallies) {
  Json rows = Json::array();
  for (const PrintTally &tally : tallies) {
    Json row;
    row["day"]       = tally.day;
    row["width_mm"]  = tally.width_mm;
    row["height_mm"] = tally.height_mm;
    row["support"]   = label_support_id(tally.support);
    row["count"]     = tally.count;
    rows.push_back(row);
  }
  return rows;
}

std::vector< PrintTally > tallies_from(const Json &rows) {
  std::vector< PrintTally > tallies;
  for (const Json &row : rows.items()) {
    PrintTally tally{ row["day"].str(), tenth(row["width_mm"].num()), tenth(row["height_mm"].num()),
                      label_support_from_id(row["support"].str()), row["count"].integer() };
    if (!tally.day.empty() && tally.count > 0)
      merge(tallies, tally);
  }
  return tallies;
}

std::string random_id() {
  std::random_device device;
  std::mt19937_64    engine((static_cast< std::uint64_t >(device()) << 32) ^ device()
                         ^ static_cast< std::uint64_t >(std::time(nullptr)));
  static const char  digits[] = "0123456789abcdef";
  std::string        id;
  for (int i = 0; i < 32; ++i)
    id += digits[engine() % 16];
  return id;
}

int total(const std::vector< PrintTally > &tallies) {
  int sum = 0;
  for (const PrintTally &tally : tallies)
    sum += tally.count;
  return sum;
}

} // namespace

std::string local_day_iso() {
  const std::time_t now = std::time(nullptr);
  std::tm           local{};
#ifdef _WIN32
  localtime_s(&local, &now);
#else
  localtime_r(&now, &local);
#endif
  char text[16];
  std::strftime(text, sizeof(text), "%Y-%m-%d", &local);
  return text;
}

void PrintCounter::load(const std::filesystem::path &file) {
  std::lock_guard< std::mutex > lock(mutex_);
  file_ = file;
  open_.clear();
  batch_.clear();
  batch_id_.clear();
  std::ifstream input(file);
  if (!input)
    return;
  std::stringstream buffer;
  buffer << input.rdbuf();
  const Json data = Json::parse(buffer.str());
  open_           = tallies_from(data["open"]);
  batch_          = tallies_from(data["batch"]["counts"]);
  batch_id_       = batch_.empty() ? std::string() : data["batch"]["id"].str();
  if (batch_id_.empty() && !batch_.empty()) { // fichier abime : on garde les compteurs
    for (const PrintTally &tally : batch_)
      merge(open_, tally);
    batch_.clear();
  }
}

void PrintCounter::add(const std::string &day, double width_mm, double height_mm, LabelSupport support, int count) {
  if (count <= 0)
    return;
  std::lock_guard< std::mutex > lock(mutex_);
  merge(open_, { day, tenth(width_mm), tenth(height_mm), support, count });
  save();
}

Json PrintCounter::next_batch() {
  std::lock_guard< std::mutex > lock(mutex_);
  if (batch_.empty()) {
    if (open_.empty())
      return Json();
    batch_.swap(open_);
    batch_id_ = random_id();
    save();
  }
  Json body;
  body["batch"]  = batch_id_;
  body["counts"] = tallies_json(batch_);
  return body;
}

void PrintCounter::batch_done(const std::string &id) {
  std::lock_guard< std::mutex > lock(mutex_);
  if (id.empty() || id != batch_id_)
    return;
  batch_.clear();
  batch_id_.clear();
  save();
}

int PrintCounter::pending() const {
  std::lock_guard< std::mutex > lock(mutex_);
  return total(open_) + total(batch_);
}

bool PrintCounter::save() const {
  if (file_.empty())
    return false;
  Json data;
  data["open"] = tallies_json(open_);
  Json batch;
  batch["id"]     = batch_id_;
  batch["counts"] = tallies_json(batch_);
  data["batch"]   = batch;
  std::error_code error;
  std::filesystem::create_directories(file_.parent_path(), error);
  const std::filesystem::path temp = file_.string() + ".tmp";
  {
    std::ofstream output(temp, std::ios::trunc);
    if (!output)
      return false;
    output << data.dump();
    if (!output)
      return false;
  }
  std::filesystem::rename(temp, file_, error);
  return !error;
}

} // namespace qrprotec
