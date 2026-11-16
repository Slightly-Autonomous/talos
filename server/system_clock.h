#pragma once

#include <chrono>

#include "core/time/time.h"

namespace talos::server {

// The hub's wall clock. Lives in server/, never in core.
class SystemClock final : public IClock {
 public:
  Timestamp now() const override {
    using namespace std::chrono;
    return {duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count()};
  }
};

}  // namespace talos::server
