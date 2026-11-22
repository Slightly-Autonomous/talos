// The board table this ESP-IDF build runs on (Kconfig TALOS_BOARD, main/Kconfig.projbuild).
// TL-SN-002 (ADR 0019) is the only hardware target. TL-SN-001's table (board_tl_sn_001.h) is
// archived with its board and kept only for host tests of the slot code paths.
#pragma once

#include "sdkconfig.h"

#include "board_tl_sn_002.h"
namespace talos::fw::selected_board {
using namespace ::talos::fw::board::sn002;
inline BoardSupport make() { return ::talos::fw::board::sn002::tl_sn_002(); }
}  // namespace talos::fw::selected_board
