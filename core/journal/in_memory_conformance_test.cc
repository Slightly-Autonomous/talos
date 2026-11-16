#include "core/journal/journal_conformance.h"

namespace talos::testing {
namespace {

struct InMemoryFactory {
  static std::unique_ptr<IJournal> make() { return std::make_unique<InMemoryJournal>(); }
  static std::unique_ptr<IJournal> reopen(IJournal&) { return nullptr; }
};

}  // namespace

INSTANTIATE_TYPED_TEST_SUITE_P(InMemory, JournalConformance, InMemoryFactory);

}  // namespace talos::testing
