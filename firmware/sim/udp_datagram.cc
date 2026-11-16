#include "firmware/sim/udp_datagram.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace talos::fw::sim {

UdpDatagram::UdpDatagram(const std::string& host, std::uint16_t port) {
  fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd_ < 0) return;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  ::inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
  if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
    ::close(fd_);
    fd_ = -1;
    return;
  }
  ::fcntl(fd_, F_SETFL, ::fcntl(fd_, F_GETFL) | O_NONBLOCK);
}

UdpDatagram::~UdpDatagram() {
  if (fd_ >= 0) ::close(fd_);
}

bool UdpDatagram::send(const std::uint8_t* data, std::size_t n) {
  if (fd_ < 0) return false;
  if (drop_sends) return true;
  return ::send(fd_, data, n, 0) == static_cast<ssize_t>(n);
}

std::size_t UdpDatagram::receive(std::uint8_t* buf, std::size_t cap) {
  if (fd_ < 0) return 0;
  const ssize_t r = ::recv(fd_, buf, cap, 0);
  return r > 0 ? static_cast<std::size_t>(r) : 0;
}

}  // namespace talos::fw::sim
