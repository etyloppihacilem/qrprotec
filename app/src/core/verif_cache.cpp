/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################
               """          verif_cache.cpp
        -\-    _|__
         |\___/  . \        Created on 11 Nov. 2026 at 13:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr
##################################################################################################################### */

#include "verif_cache.hpp"

#include <fstream>
#include <sstream>

namespace qrprotec {

Json VerifCache::request() const {
  Json body;
  body["buckets"] = Json::object();
  for (const auto &[bucket, hash] : hashes_)
    body["buckets"][bucket] = hash;
  return body;
}

std::vector< std::string > VerifCache::apply(const Json &response) {
  std::vector< std::string > changed;
  bool                       lots = false;
  for (const auto &[bucket, content] : response["buckets"].members()) {
    const Json             &fresh = content["records"];
    std::set< std::string > kept;
    for (const auto &[key, value] : fresh.members())
      kept.insert(key);
    // enregistrement absent d'un seau renvoye : il a disparu
    for (const std::string &key : keys_[bucket])
      if (!kept.count(key)) {
        records_.erase(key);
        changed.push_back(key);
        lots = lots || key.rfind("l:", 0) == 0;
      }
    std::vector< std::string > keys;
    for (const auto &[key, value] : fresh.members()) {
      keys.push_back(key);
      const auto found = records_.find(key);
      if (found != records_.end() && found->second.dump() == value.dump())
        continue;
      records_[key] = value;
      changed.push_back(key);
      lots = lots || key.rfind("l:", 0) == 0;
    }
    keys_[bucket]   = std::move(keys);
    hashes_[bucket] = content["hash"].str();
  }
  if (lots || !loaded_) {
    roots_.clear();
    for (const auto &[key, lot] : records_) {
      if (key.rfind("l:", 0) != 0)
        continue;
      roots_[lot["id"].str()] = lot["id"].str();
      for (const Json &row : lot["descendants"].items())
        roots_[row["id"].str()] = lot["id"].str();
    }
  }
  loaded_ = true;
  return changed;
}

const Json *VerifCache::find(const std::string &key) const {
  const auto found = records_.find(key);
  return found == records_.end() ? nullptr : &found->second;
}

std::string VerifCache::root_of(const std::string &id) const {
  const auto found = roots_.find(id);
  return found == roots_.end() ? std::string() : found->second;
}

// Lignes d'items d'un lot du cache (iids) remplacees par le detail de chaque item, avec « optional » dans ce lot
Json VerifCache::with_items(const Json &row) const {
  Json                    result = row;
  std::set< std::string > optional;
  for (const Json &iid : row["optional_items"].items())
    optional.insert(iid.str());
  for (const char *field : { "items", "missing_items" }) {
    Json list = Json::array();
    for (const Json &iid : row[field].items()) {
      const Json *item  = this->item(iid.str());
      Json        entry = item ? *item : Json::object();
      if (!item)
        entry["iid"] = iid.str();
      entry["optional"] = optional.count(iid.str()) > 0;
      list.push_back(entry);
    }
    result[field] = list;
  }
  return result;
}

Json VerifCache::lot(const std::string &root) const {
  const Json *record = find("l:" + root);
  if (!record)
    return Json();
  Json lot          = with_items(*record);
  Json descendants  = Json::array();
  for (const Json &row : (*record)["descendants"].items())
    descendants.push_back(with_items(row));
  lot["descendants"] = descendants;
  return lot;
}

void VerifOutbox::load(const std::filesystem::path &file) {
  file_ = file;
  entries_.clear();
  std::ifstream input(file);
  if (!input)
    return;
  std::stringstream buffer;
  buffer << input.rdbuf();
  const Json data = Json::parse(buffer.str());
  for (const Json &row : data.items())
    entries_.push_back({ row["body"], row["label"].str(), row["at"].str() });
}

bool VerifOutbox::save() const {
  if (file_.empty())
    return false;
  Json data = Json::array();
  for (const OutboxEntry &entry : entries_) {
    Json row;
    row["body"]  = entry.body;
    row["label"] = entry.label;
    row["at"]    = entry.at;
    data.push_back(row);
  }
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

void VerifOutbox::push(OutboxEntry entry) {
  entries_.push_back(std::move(entry));
  save();
}

void VerifOutbox::pop_front() {
  if (entries_.empty())
    return;
  entries_.erase(entries_.begin());
  save();
}

} // namespace qrprotec
