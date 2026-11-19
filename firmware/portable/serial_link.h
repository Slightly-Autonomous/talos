// Datagrams over a text console (bench-test-plan.md §7, Stage 7). Until the
// Thread radio carries CoAP, the node sends the same CoAP datagrams over its
// USB-serial-JTAG console as text lines:
//
//     @F <base64 of one datagram>\n
//
// They share the port with "@T" diag replies and log lines; a host bridge
// (tools/hil/talos_hil/serial_bridge.py) relays them to the hub's CoAP UDP port
// and writes the replies back the same way. Swapping in the Thread transport
// later only replaces this IDatagram.
//
// The node's device ID and key live in the store (StoreKey::kIdentity), set
// on the bench with the diag command `provision`. There is no built-in key:
// an unprovisioned node keeps its events queued.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "firmware/portable/coap_link.h"
#include "firmware/portable/hal.h"
#include "firmware/portable/wire.h"

namespace talos::fw {

namespace base64 {
// Returns the encoded length (no terminator), or 0 if `cap` is too small.
std::size_t encode(const std::uint8_t* in, std::size_t n, char* out, std::size_t cap);
// Returns the decoded length, or nullopt on bad input or overflow.
std::optional<std::size_t> decode(const char* in, std::size_t n, std::uint8_t* out,
                                  std::size_t cap);
}  // namespace base64

class ILineSink {
 public:
  virtual ~ILineSink() = default;
  virtual void write(const char* s, std::size_t n) = 0;
};

class SerialDatagram final : public IDatagram {
 public:
  static constexpr const char* kPrefix = "@F ";
  static constexpr std::size_t kMaxDatagram = wire::kMaxFrame + 64;
  static constexpr std::size_t kMaxLine = 3 + (kMaxDatagram + 2) / 3 * 4 + 1;
  static constexpr std::size_t kInbox = 4;

  explicit SerialDatagram(ILineSink& out) : out_(out) {}
  bool send(const std::uint8_t* data, std::size_t n) override;
  std::size_t receive(std::uint8_t* buf, std::size_t cap) override;
  // Offer one console line. True if it was a datagram frame (consumed here).
  bool push_line(const char* line, std::size_t n);
  std::uint32_t dropped() const { return dropped_; }  // inbox full or undecodable

 private:
  struct Slot {
    std::size_t len = 0;
    std::array<std::uint8_t, kMaxDatagram> data{};
  };
  ILineSink& out_;
  std::array<Slot, kInbox> inbox_{};
  std::size_t head_ = 0, size_ = 0;
  std::uint32_t dropped_ = 0;
};

// The provisioned identity of this node, persisted in the store.
struct DeviceIdentity {
  static constexpr std::size_t kMaxId = CoapLink::kMaxIdLen;
  static constexpr std::size_t kMaxKey = 32;
  char device_id[kMaxId + 1] = {};
  std::uint8_t key_len = 0;
  std::uint8_t key[kMaxKey] = {};
  bool valid() const { return device_id[0] != 0 && key_len >= 16 && key_len <= kMaxKey; }
};

std::optional<DeviceIdentity> load_identity(IStore& store);
bool save_identity(IStore& store, const DeviceIdentity& id);

// ILink that is a CoapLink once the node has an identity, and refuses sends
// (events stay queued, nothing is lost) until then.
class ProvisionedLink final : public ILink {
 public:
  ProvisionedLink(IDatagram& net, IStore& store);
  bool send(const DeviceEvent& e) override { return link_ ? link_->send(e) : false; }
  std::optional<Downlink> receive() override { return link_ ? link_->receive() : std::nullopt; }
  bool provisioned() const { return link_.has_value(); }
  const char* device_id() const { return id_.device_id; }

 private:
  DeviceIdentity id_;
  std::optional<CoapLink> link_;
};

}  // namespace talos::fw
