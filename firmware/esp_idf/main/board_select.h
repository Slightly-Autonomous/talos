// The board table this ESP-IDF build runs on (Kconfig TALOS_BOARD, main/Kconfig.projbuild).
// TL-SN-002 (ADR 0019) is the default; TL-SN-001 is the archived modular board.
// Everything board-specific in main.cc and esp_platform.cc goes through
// selected_board:: so both stay buildable from the same sources.
#pragma once

#include "sdkconfig.h"

#if defined(CONFIG_TALOS_BOARD_TL_SN_001)
#include "board_tl_sn_001.h"
namespace talos::fw::selected_board {
using namespace ::talos::fw::board;
inline constexpr const char* kBoardName = "TL-SN-001";
inline BoardSupport make() { return ::talos::fw::board::tl_sn_001(); }
}  // namespace talos::fw::selected_board
#else
#include "board_tl_sn_002.h"
namespace talos::fw::selected_board {
using namespace ::talos::fw::board::sn002;
inline BoardSupport make() { return ::talos::fw::board::sn002::tl_sn_002(); }
}  // namespace talos::fw::selected_board
#endif
