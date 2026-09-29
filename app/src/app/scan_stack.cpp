/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          scan_stack.cpp
        -\-    _|__
         |\___/  . \        Created on 29 Sep. 2026 at 16:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "scan_stack.hpp"

#include <algorithm>
#include <set>

namespace qrprotec {

ScanEntry &ScanStack::add(const ParsedScan &scan, ScanSource source) {
  ScanEntry entry;
  entry.id      = next_id_++;
  entry.scan    = scan;
  entry.source  = source;
  entry.expired = is_expired(scan, Date::today());
  entry.title   = scan_kind_name(scan.kind);
  if (scan.kind == ScanKind::Unknown) {
    entry.state  = EntryState::Error;
    entry.detail = "Code non reconnu";
  }
  entries_.push_back(entry);
  refresh_duplicates();
  return entries_.back();
}

ScanEntry *ScanStack::find(int id) {
  for (ScanEntry &entry : entries_)
    if (entry.id == id)
      return &entry;
  return nullptr;
}

ScanEntry *ScanStack::find_code(const std::string &raw) {
  for (ScanEntry &entry : entries_)
    if (entry.scan.raw == raw && entry.state != EntryState::Error)
      return &entry;
  return nullptr;
}

void ScanStack::refresh_duplicates() {
  std::set< std::string > seen;
  for (ScanEntry &entry : entries_) {
    entry.duplicate = !seen.insert(entry.scan.raw).second;
  }
}

void ScanStack::remove(int id) {
  entries_.erase(
    std::remove_if(entries_.begin(), entries_.end(), [id](const ScanEntry &entry) { return entry.id == id; }),
    entries_.end()
  );
  refresh_duplicates();
}

void ScanStack::remove_duplicates() {
  entries_.erase(
    std::remove_if(entries_.begin(), entries_.end(), [](const ScanEntry &entry) { return entry.duplicate; }),
    entries_.end()
  );
  refresh_duplicates();
}

void ScanStack::remove_errors() {
  entries_.erase(
    std::remove_if(
      entries_.begin(), entries_.end(), [](const ScanEntry &entry) { return entry.state == EntryState::Error; }
    ),
    entries_.end()
  );
  refresh_duplicates();
}

void ScanStack::undo_last() {
  if (!entries_.empty())
    entries_.pop_back();
  refresh_duplicates();
}

void ScanStack::clear() {
  entries_.clear();
  target = {};
}

std::vector< std::string > ScanStack::iids() const {
  std::vector< std::string > result;
  std::set< std::string >    seen;
  const auto                 push = [&](const std::string &iid) {
    if (seen.insert(iid).second)
      result.push_back(iid);
  };
  for (const ScanEntry &entry : entries_) {
    if (entry.state == EntryState::Error)
      continue;
    if (entry.scan.kind == ScanKind::Item)
      push(entry.scan.id);
    else if (entry.scan.kind == ScanKind::SealedPack)
      for (const std::string &iid : entry.pack_items)
        push(iid);
  }
  return result;
}

bool ScanStack::contains_iid(const std::string &iid) const {
  for (const ScanEntry &entry : entries_) {
    if (entry.state == EntryState::Error)
      continue;
    if (entry.scan.kind == ScanKind::Item && entry.scan.id == iid)
      return true;
    if (entry.scan.kind == ScanKind::SealedPack
        && std::find(entry.pack_items.begin(), entry.pack_items.end(), iid) != entry.pack_items.end())
      return true;
  }
  return false;
}

std::size_t ScanStack::expired_count() const {
  return static_cast< std::size_t >(std::count_if(entries_.begin(), entries_.end(), [](const ScanEntry &entry) {
    return entry.expired && !entry.duplicate;
  }));
}

} // namespace qrprotec
