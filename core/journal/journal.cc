#include "core/journal/journal.h"

#include <algorithm>
#include <limits>

namespace talos {

Result<JournalEntry> InMemoryJournal::append(Timestamp received_at, DomainEvent event) {
  JournalEntry e;
  e.journal_seq = entries_.size() + 1;
  e.received_at = received_at;
  if (!entries_.empty() && received_at < entries_.back().received_at) {
    e.received_at = entries_.back().received_at;
    e.clock_regressed = true;
  }
  e.event = std::move(event);
  entries_.push_back(e);
  return e;
}

std::vector<JournalEntry> InMemoryJournal::read(std::uint64_t after_seq, std::size_t limit) const {
  std::vector<JournalEntry> out;
  if (after_seq >= entries_.size()) return out;
  const std::size_t start = static_cast<std::size_t>(after_seq);
  const std::size_t n = std::min(limit, entries_.size() - start);
  out.assign(entries_.begin() + static_cast<std::ptrdiff_t>(start),
             entries_.begin() + static_cast<std::ptrdiff_t>(start + n));
  return out;
}

std::vector<JournalEntry> read_all(const IJournal& journal, std::uint64_t after_seq) {
  return journal.read(after_seq, std::numeric_limits<std::size_t>::max());
}

}  // namespace talos
