// POSIX UDP implementation of IDatagram for host integration tests: a node
// on the virtual board talks to the real hub over a real socket.
#pragma once

#include <cstdint>
#include <string>

#include "firmware/portable/hal.h"

namespace talos::fw::sim {

class UdpDatagram final : public IDatagram {
 public:
  UdpDatagram(const std::string& host, std::uint16_t port);
  ~UdpDatagram() override;
  bool send(const std::uint8_t* data, std::size_t n) override;
  std::size_t receive(std::uint8_t* buf, std::size_t cap) override;
  bool ok() const { return fd_ >= 0; }
  bool drop_sends = false;  // simulate radio loss

 private:
  int fd_ = -1;
};

}  // namespace talos::fw::sim
