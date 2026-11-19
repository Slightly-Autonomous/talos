#include "firmware/portable/diag.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "common/sha256/sha256.h"
#include "firmware/portable/serial_link.h"

namespace talos::fw {

// --- JsonLine ---------------------------------------------------------------------------

JsonLine::JsonLine() { raw("@T {"); }

void JsonLine::raw(const char* s) {
  const std::size_t len = std::strlen(s);
  if (n_ + len >= sizeof buf_ - 3) {  // keep room for "}\n"
    overflow_ = true;
    return;
  }
  std::memcpy(buf_ + n_, s, len);
  n_ += len;
}

void JsonLine::key(const char* k) {
  if (!first_) raw(",");
  first_ = false;
  raw("\"");
  raw(k);
  raw("\":");
}

JsonLine& JsonLine::str(const char* k, const char* value) {
  key(k);
  raw("\"");
  // Values are fixed identifiers here; drop anything that would break the JSON.
  char clean[64];
  std::size_t j = 0;
  for (std::size_t i = 0; value[i] && j < sizeof clean - 1; ++i) {
    const char c = value[i];
    if (c != '"' && c != '\\' && c >= 0x20) clean[j++] = c;
  }
  clean[j] = 0;
  raw(clean);
  raw("\"");
  return *this;
}

JsonLine& JsonLine::num(const char* k, std::int64_t value) {
  key(k);
  char tmp[24];
  std::snprintf(tmp, sizeof tmp, "%lld", static_cast<long long>(value));
  raw(tmp);
  return *this;
}

JsonLine& JsonLine::hex(const char* k, std::uint32_t value) {
  char tmp[16];
  std::snprintf(tmp, sizeof tmp, "0x%04X", static_cast<unsigned>(value));
  return str(k, tmp);
}

JsonLine& JsonLine::boolean(const char* k, bool value) {
  key(k);
  raw(value ? "true" : "false");
  return *this;
}

JsonLine& JsonLine::nums(const char* k, const std::int32_t* values, std::size_t n) {
  key(k);
  raw("[");
  for (std::size_t i = 0; i < n; ++i) {
    char tmp[16];
    std::snprintf(tmp, sizeof tmp, i ? ",%ld" : "%ld", static_cast<long>(values[i]));
    raw(tmp);
  }
  raw("]");
  return *this;
}

void JsonLine::emit(IDiagOut& out) {
  if (overflow_) {
    n_ = 0;
    first_ = true;
    raw("@T {\"error\":\"line too long\"");
  }
  buf_[n_++] = '}';
  buf_[n_++] = '\n';
  out.write(buf_, n_);
}

// --- helpers ----------------------------------------------------------------------------

namespace {

bool parse_u32(const char* s, std::uint32_t& out) {
  if (!s || !*s) return false;
  char* end = nullptr;
  const unsigned long v = std::strtoul(s, &end, 0);
  if (*end != 0) return false;
  out = static_cast<std::uint32_t>(v);
  return true;
}

bool parse_slot(const char* s, std::uint8_t& slot) {
  std::uint32_t v = 0;
  if (!parse_u32(s, v) || v >= kSlots) return false;
  slot = static_cast<std::uint8_t>(v);
  return true;
}

const char* source_name(PowerSource s) {
  switch (s) {
    case PowerSource::kUsb:
      return "usb";
    case PowerSource::kSolarBattery:
      return "solar";
    case PowerSource::kBatteryOnly:
      return "battery";
    case PowerSource::kUnknown:
      break;
  }
  return "unknown";
}

bool is_imu(ModuleCode c) { return c == ModuleCode::kLis2dw12 || c == ModuleCode::kTlMod003Imu; }
bool is_sht(ModuleCode c) { return c == ModuleCode::kSht40 || c == ModuleCode::kTlMod004Env; }

constexpr std::uint8_t kCardExp = TestCardDriver::kExpanderAddr;

}  // namespace

bool Diag::select(std::uint8_t slot) {
  const auto branch = p_.board.slots[slot].i2c_branch;
  if (branch == 0xFF) return false;
  const std::uint8_t mask = static_cast<std::uint8_t>(1u << branch);
  return p_.i2c.write(p_.board.mux_address, &mask, 1);
}

void Diag::deselect() {
  const std::uint8_t none = 0;
  p_.i2c.write(p_.board.mux_address, &none, 1);
}

bool Diag::probe(std::uint8_t addr) { return p_.i2c.probe(addr); }

bool Diag::check(IDiagOut& out, const char* test, bool ok) {
  JsonLine().str("t", test).boolean("ok", ok).emit(out);
  return ok;
}

// --- dispatch ---------------------------------------------------------------------------

void Diag::handle_line(const char* line, IDiagOut& out) {
  char buf[kMaxLine];
  std::size_t len = std::strlen(line);
  if (len >= sizeof buf) len = sizeof buf - 1;
  std::memcpy(buf, line, len);
  buf[len] = 0;
  Args a;
  for (char* tok = std::strtok(buf, " \t\r\n"); tok && a.n < 16;
       tok = std::strtok(nullptr, " \t\r\n")) {
    a.v[a.n++] = tok;
  }
  if (a.n == 0) return;
  const char* cmd = a.v[0];
  const char* err = nullptr;
  bool ok = false;
  auto is = [&](const char* name) { return std::strcmp(cmd, name) == 0; };
  if (is("help")) {
    JsonLine()
        .str("t", "help")
        .str("cmds",
             "id slots power i2c mux exp rail uart rescan selftest provision identity reboot")
        .emit(out);
    ok = true;
  } else if (is("id")) {
    ok = cmd_id(out);
  } else if (is("slots")) {
    ok = cmd_slots(out);
  } else if (is("power")) {
    ok = cmd_power(out, err);
  } else if (is("i2c")) {
    ok = cmd_i2c(a, out, err);
  } else if (is("mux")) {
    ok = cmd_mux(a, out, err);
  } else if (is("exp")) {
    ok = cmd_exp(a, out, err);
  } else if (is("rail")) {
    ok = cmd_rail(a, out, err);
  } else if (is("uart")) {
    ok = cmd_uart(a, out, err);
  } else if (is("provision")) {
    ok = cmd_provision(a, out, err);
  } else if (is("identity")) {
    ok = cmd_identity(out);
  } else if (is("reboot")) {
    reboot_ = true;
    ok = true;
  } else if (is("rescan")) {
    rt_.rescan();
    ok = cmd_slots(out);
  } else if (is("selftest")) {
    std::uint8_t slot = 0;
    const char* what = a.n > 1 ? a.v[1] : "";
    const bool has_slot = a.n > 2 && parse_slot(a.v[2], slot);
    if (std::strcmp(what, "i2c") == 0) {
      ok = selftest_i2c(out);
    } else if (!has_slot) {
      err = "usage: selftest i2c | selftest slot|imu|sht <slot>";
    } else if (std::strcmp(what, "slot") == 0) {
      ok = selftest_slot(slot, out, err);
    } else if (std::strcmp(what, "imu") == 0) {
      ok = selftest_imu(slot, out, err);
    } else if (std::strcmp(what, "sht") == 0) {
      ok = selftest_sht(slot, out, err);
    } else {
      err = "unknown selftest";
    }
  } else {
    err = "unknown command";
  }
  JsonLine done;
  done.str("cmd", cmd).boolean("done", true).boolean("ok", ok && !err);
  if (err) done.str("err", err);
  done.emit(out);
}

// --- information ------------------------------------------------------------------------

bool Diag::cmd_id(IDiagOut& out) {
  JsonLine()
      .str("t", "id")
      .num("fw", rt_.running_version())
      .num("boot_epoch", rt_.boot_epoch())
      .boolean("board_ok", rt_.board_ok())
      .num("queued", static_cast<std::int64_t>(rt_.queued()))
      .num("mode", static_cast<std::int64_t>(rt_.power_mode()))
      .emit(out);
  return true;
}

bool Diag::cmd_slots(IDiagOut& out) {
  for (std::uint8_t i = 0; i < kSlots; ++i) {
    const SlotState& st = rt_.slot(i);
    const auto present = p_.power.slot_present(i);
    JsonLine line;
    line.str("t", "slot")
        .num("slot", i)
        .hex("code", static_cast<std::uint32_t>(st.code))
        .num("fault", static_cast<std::int64_t>(st.fault))
        .boolean("enabled", st.enabled)
        .num("present", present ? (*present ? 1 : 0) : -1);
    if (io_) {
      line.boolean("aon", io_->rail_on(i, Rail::kAlwaysOn3v3))
          .boolean("sw", io_->rail_on(i, Rail::kSwitched3v3));
    }
    line.emit(out);
  }
  return true;
}

bool Diag::cmd_power(IDiagOut& out, const char*& err) {
  JsonLine()
      .str("t", "power")
      .str("source", source_name(p_.power.source()))
      .num("battery_mv", p_.power.battery_mv())
      .emit(out);
  if (!io_) return true;
  const auto st = io_->charger_status();
  if (!st) {
    err = "charger not answering";
    return false;
  }
  JsonLine()
      .str("t", "charger")
      .boolean("config_ok", io_->charger_ok())
      .boolean("vin_good", st->vin_good)
      .num("charge", static_cast<std::int64_t>(st->charge))
      .boolean("ts_open", st->ts_open)
      .num("ts_zone", st->ts_zone)
      .boolean("thermal_regulation", st->thermal_regulation)
      .boolean("battery_uvlo", st->battery_uvlo)
      .boolean("vin_ovp", st->vin_ovp)
      .boolean("safety_timer_fault", st->safety_timer_fault)
      .num("flags", st->flags)
      .emit(out);
  return io_->charger_ok();
}

// --- identity ---------------------------------------------------------------------------

namespace {
int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
// First 4 bytes of SHA-256(key): enough to match the hub's key file without showing the key.
std::uint32_t fingerprint(const std::uint8_t* key, std::size_t n) {
  Sha256 h;
  h.update(key, n);
  const auto d = h.finish();
  return (std::uint32_t{d[0]} << 24) | (std::uint32_t{d[1]} << 16) | (std::uint32_t{d[2]} << 8) |
         d[3];
}
}  // namespace

bool Diag::cmd_provision(const Args& a, IDiagOut& out, const char*& err) {
  DeviceIdentity id;
  const std::size_t id_len = a.n >= 3 ? std::strlen(a.v[1]) : 0;
  const std::size_t hex_len = a.n >= 3 ? std::strlen(a.v[2]) : 0;
  if (a.n != 3 || id_len == 0 || id_len > DeviceIdentity::kMaxId || hex_len % 2 != 0 ||
      hex_len / 2 < 16 || hex_len / 2 > DeviceIdentity::kMaxKey) {
    err = "usage: provision <device_id, 1-31 chars> <key: 16-32 bytes as hex>";
    return false;
  }
  std::memcpy(id.device_id, a.v[1], id_len);
  for (std::size_t i = 0; i < hex_len / 2; ++i) {
    const int hi = hex_digit(a.v[2][2 * i]), lo = hex_digit(a.v[2][2 * i + 1]);
    if (hi < 0 || lo < 0) {
      err = "key must be hex";
      return false;
    }
    id.key[i] = static_cast<std::uint8_t>(hi * 16 + lo);
  }
  id.key_len = static_cast<std::uint8_t>(hex_len / 2);
  if (!save_identity(p_.store, id)) {
    err = "store write failed";
    return false;
  }
  reboot_ = true;  // the link is built at boot from the stored identity
  JsonLine()
      .str("t", "identity")
      .str("device_id", id.device_id)
      .hex("key_fingerprint", fingerprint(id.key, id.key_len))
      .boolean("reboot", true)
      .emit(out);
  return true;
}

bool Diag::cmd_identity(IDiagOut& out) {
  const auto id = load_identity(p_.store);
  JsonLine line;
  line.str("t", "identity").boolean("provisioned", id.has_value());
  if (id)
    line.str("device_id", id->device_id).hex("key_fingerprint", fingerprint(id->key, id->key_len));
  line.emit(out);
  return true;
}

// --- raw bus access ---------------------------------------------------------------------

bool Diag::cmd_i2c(const Args& a, IDiagOut& out, const char*& err) {
  // Optional trailing "slot=<n>" selects that slot's mux branch for the access.
  int n = a.n;
  std::uint8_t slot = 0;
  bool on_slot = false;
  if (n > 1 && std::strncmp(a.v[n - 1], "slot=", 5) == 0) {
    if (!parse_slot(a.v[n - 1] + 5, slot)) {
      err = "bad slot";
      return false;
    }
    on_slot = true;
    --n;
  }
  const char* sub = n > 1 ? a.v[1] : "";
  if (std::strcmp(sub, "scan") == 0) {
    if (n > 2 && std::strcmp(a.v[2], "root") != 0) {
      if (!parse_slot(a.v[2], slot)) {
        err = "usage: i2c scan [root|<slot>]";
        return false;
      }
      on_slot = true;
    }
    if (on_slot && !select(slot)) {
      err = "mux select failed";
      return false;
    }
    if (!on_slot) deselect();
    std::int32_t found[32];
    std::size_t count = 0;
    for (std::uint8_t addr = 0x08; addr <= 0x77 && count < 32; ++addr) {
      if (addr == p_.board.mux_address) continue;
      if (probe(addr)) found[count++] = addr;
    }
    if (on_slot) deselect();
    JsonLine line;
    line.str("t", "i2c.scan").str("bus", on_slot ? "slot" : "root");
    if (on_slot) line.num("slot", slot);
    line.nums("found", found, count).emit(out);
    return true;
  }
  std::uint32_t addr = 0, reg = 0;
  if ((std::strcmp(sub, "rd") != 0 && std::strcmp(sub, "wr") != 0) || n < 4 ||
      !parse_u32(a.v[2], addr) || !parse_u32(a.v[3], reg) || addr > 0x7F || reg > 0xFF) {
    err = "usage: i2c scan|rd|wr ...";
    return false;
  }
  if (on_slot && !select(slot)) {
    err = "mux select failed";
    return false;
  }
  bool ok = false;
  if (sub[0] == 'r') {
    std::uint32_t count = 0;
    std::uint8_t r[32] = {};
    if (n < 5 || !parse_u32(a.v[4], count) || count == 0 || count > sizeof r) {
      err = "read 1-32 bytes";
    } else {
      const auto rg = static_cast<std::uint8_t>(reg);
      ok = p_.i2c.write_read(static_cast<std::uint8_t>(addr), &rg, 1, r, count);
      std::int32_t v[32];
      for (std::uint32_t i = 0; i < count; ++i) v[i] = r[i];
      JsonLine()
          .str("t", "i2c.rd")
          .hex("addr", addr)
          .hex("reg", reg)
          .boolean("ack", ok)
          .nums("data", v, ok ? count : 0)
          .emit(out);
      if (!ok) err = "nack";
    }
  } else {
    std::uint8_t w[17] = {static_cast<std::uint8_t>(reg)};
    std::size_t wn = 1;
    bool parsed = n > 4;
    for (int i = 4; i < n && wn < sizeof w; ++i) {
      std::uint32_t b = 0;
      parsed &= parse_u32(a.v[i], b) && b <= 0xFF;
      w[wn++] = static_cast<std::uint8_t>(b);
    }
    if (!parsed) {
      err = "write 1-16 bytes of 0-255";
    } else {
      ok = p_.i2c.write(static_cast<std::uint8_t>(addr), w, wn);
      JsonLine().str("t", "i2c.wr").hex("addr", addr).hex("reg", reg).boolean("ack", ok).emit(out);
      if (!ok) err = "nack";
    }
  }
  if (on_slot) deselect();
  return ok;
}

bool Diag::cmd_mux(const Args& a, IDiagOut& out, const char*& err) {
  if (a.n > 1) {
    std::uint32_t mask = 0;
    if (!parse_u32(a.v[1], mask) || mask > 0x0F) {
      err = "mask 0-15";
      return false;
    }
    const auto m = static_cast<std::uint8_t>(mask);
    if (!p_.i2c.write(p_.board.mux_address, &m, 1)) {
      err = "mux nack";
      return false;
    }
  }
  std::uint8_t now = 0;
  if (!p_.i2c.write_read(p_.board.mux_address, nullptr, 0, &now, 1)) {
    err = "mux nack";
    return false;
  }
  JsonLine().str("t", "mux").num("mask", now).emit(out);
  return true;
}

bool Diag::cmd_exp(const Args& a, IDiagOut& out, const char*& err) {
  std::uint32_t addr = 0;
  if (a.n < 2 || !parse_u32(a.v[1], addr) || !io_ ||
      !io_->expander(static_cast<std::uint8_t>(addr))) {
    err = "usage: exp <expander address>";
    return false;
  }
  Tca9535* e = io_->expander(static_cast<std::uint8_t>(addr));
  const auto in = e->read_inputs();
  const auto outp = e->read_register_pair(Tca9535::kOutput0);
  const auto cfg = e->read_register_pair(Tca9535::kConfig0);
  if (!in || !outp || !cfg) {
    err = "expander nack";
    return false;
  }
  JsonLine()
      .str("t", "exp")
      .hex("addr", addr)
      .hex("inputs", *in)
      .hex("outputs", *outp)
      .hex("config", *cfg)
      .boolean("outputs_match_shadow", *outp == e->outputs())
      .emit(out);
  return true;
}

bool Diag::cmd_rail(const Args& a, IDiagOut& out, const char*& err) {
  std::uint8_t slot = 0;
  if (!io_ || a.n < 4 || !parse_slot(a.v[1], slot) ||
      (std::strcmp(a.v[2], "aon") != 0 && std::strcmp(a.v[2], "sw") != 0) ||
      (std::strcmp(a.v[3], "on") != 0 && std::strcmp(a.v[3], "off") != 0)) {
    err = "usage: rail <slot> aon|sw on|off";
    return false;
  }
  const Rail rail = a.v[2][0] == 'a' ? Rail::kAlwaysOn3v3 : Rail::kSwitched3v3;
  const bool on = std::strcmp(a.v[3], "on") == 0;
  io_->set_rail(slot, rail, on);
  const bool aon = io_->rail_on(slot, Rail::kAlwaysOn3v3);
  const bool sw = io_->rail_on(slot, Rail::kSwitched3v3);
  JsonLine().str("t", "rail").num("slot", slot).boolean("aon", aon).boolean("sw", sw).emit(out);
  if (io_->rail_on(slot, rail) != on) {
    err = rail == Rail::kSwitched3v3 && on ? "refused: AON is off" : "expander did not take it";
    return false;
  }
  return true;
}

bool Diag::cmd_uart(const Args& a, IDiagOut& out, const char*& err) {
  if (!io_ || a.n < 2) {
    err = "usage: uart <slot>|off";
    return false;
  }
  std::optional<std::uint8_t> target;
  std::uint8_t slot = 0;
  if (std::strcmp(a.v[1], "off") != 0) {
    if (!parse_slot(a.v[1], slot)) {
      err = "bad slot";
      return false;
    }
    target = slot;
  }
  const bool ok = io_->select_uart(target);
  const auto now = io_->uart_slot();
  JsonLine().str("t", "uart").num("slot", now ? *now : -1).emit(out);
  if (!ok) err = target ? "refused (slot unpowered?)" : "mux control failed";
  return ok;
}

// --- self-tests -------------------------------------------------------------------------

bool Diag::selftest_i2c(IDiagOut& out) {
  bool all = true;
  // Root bus with every branch deselected: exactly the mux, expanders and charger.
  deselect();
  std::int32_t want[8];
  std::size_t nwant = 0;
  auto add = [&](std::uint8_t a) {
    if (a == 0) return;
    for (std::size_t i = 0; i < nwant; ++i) {
      if (want[i] == a) return;
    }
    if (nwant < 8) want[nwant++] = a;
  };
  add(p_.board.mux_address);
  for (const auto& s : p_.board.slot_io) {
    add(s.aon_enable.address);
    add(s.present_n.address);
  }
  add(p_.board.charger_address);
  for (std::size_t i = 1; i < nwant; ++i) {  // sort for a stable comparison
    for (std::size_t j = i; j > 0 && want[j - 1] > want[j]; --j) {
      const auto t = want[j];
      want[j] = want[j - 1];
      want[j - 1] = t;
    }
  }
  std::int32_t found[32];
  std::size_t nfound = 0;
  for (std::uint8_t addr = 0x08; addr <= 0x77 && nfound < 32; ++addr) {
    if (probe(addr)) found[nfound++] = addr;
  }
  bool same = nfound == nwant;
  for (std::size_t i = 0; same && i < nfound; ++i) same = found[i] == want[i];
  JsonLine()
      .str("t", "i2c.root_scan")
      .boolean("ok", same)
      .nums("found", found, nfound)
      .nums("want", want, nwant)
      .emit(out);
  all &= same;

  // Mux control register: every single-branch and multi-branch mask reads back.
  bool mux_ok = true;
  std::int32_t bad = -1;
  for (std::uint8_t m = 0; m <= 0x0F && mux_ok; ++m) {
    std::uint8_t r = 0xFF;
    mux_ok = p_.i2c.write(p_.board.mux_address, &m, 1) &&
             p_.i2c.write_read(p_.board.mux_address, nullptr, 0, &r, 1) && r == m;
    if (!mux_ok) bad = m;
  }
  deselect();
  JsonLine()
      .str("t", "i2c.mux_readback")
      .boolean("ok", mux_ok)
      .num("first_bad_mask", bad)
      .emit(out);
  all &= mux_ok;

  if (io_) {
    for (std::size_t i = 0; i < nwant; ++i) {
      Tca9535* e = io_->expander(static_cast<std::uint8_t>(want[i]));
      if (!e) continue;
      const auto outp = e->read_register_pair(Tca9535::kOutput0);
      const bool ok = outp && *outp == e->outputs();
      JsonLine()
          .str("t", "i2c.expander_outputs")
          .hex("addr", static_cast<std::uint32_t>(want[i]))
          .boolean("ok", ok)
          .emit(out);
      all &= ok;
    }
    if (p_.board.charger_address) all &= check(out, "i2c.charger_config", io_->charger_ok());
  }
  return all;
}

bool Diag::selftest_slot(std::uint8_t slot, IDiagOut& out, const char*& err) {
  if (!io_) {
    err = "needs board I/O";
    return false;
  }
  if (rt_.slot(slot).code != ModuleCode::kTlTest001) {
    err = "slot has no TL-TEST-001";
    return false;
  }
  const SlotIoPins& pins = p_.board.slot_io[slot];
  bool all = true;
  all &= check(out, "slot.present", p_.power.slot_present(slot).value_or(false));
  // Branch isolation (IT-01): the card answers only through its own branch.
  const bool via_branch = select(slot) && probe(kCardExp);
  deselect();
  const bool without = probe(kCardExp);
  all &= check(out, "slot.branch_isolation", via_branch && !without);

  auto card_write = [&](std::uint8_t reg, std::uint8_t v) {
    const std::uint8_t b[2] = {reg, v};
    return select(slot) && p_.i2c.write(kCardExp, b, 2);
  };
  auto card_inputs = [&](std::uint8_t& port1) {
    const std::uint8_t reg = 0x01;
    return select(slot) && p_.i2c.write_read(kCardExp, &reg, 1, &port1, 1);
  };
  const std::uint8_t wake = p_.board.slots[slot].wake_line;
  // WAKE: the card drives it through 1k; the node reads its GPIO.
  bool wake_ok = wake != 0xFF;
  for (bool level : {true, false}) {
    wake_ok &= card_write(0x02, level ? 0x01 : 0x00) && card_write(0x06, 0xFE);  // P00 output
    p_.clock.delay_ms(1);
    wake_ok &= wake != 0xFF && p_.gpio.read(wake) == level;
  }
  all &= check(out, "slot.wake", wake_ok);
  // ALERT_N: the card pulls it low; EXP_B reads it.
  bool alert_ok = card_write(0x02, 0x00) && card_write(0x06, 0xFC);  // P00, P01 outputs low
  p_.clock.delay_ms(1);
  alert_ok &= io_->sense(pins.alert_n) == std::optional<bool>(false);
  alert_ok &= card_write(0x06, 0xFF);  // release
  p_.clock.delay_ms(1);
  alert_ok &= io_->sense(pins.alert_n) == std::optional<bool>(true);
  all &= check(out, "slot.alert", alert_ok);
  // CS_N and RESET_N: EXP_A drives them; the card reads P10/P11.
  auto line_follows = [&](const ExpanderPin& pin, std::uint8_t bit) {
    bool ok = true;
    for (bool level : {false, true}) {
      std::uint8_t in = 0;
      ok &= io_->drive(pin, level);
      ok &= card_inputs(in) && (((in >> bit) & 1u) != 0) == level;
    }
    return ok;
  };
  all &= check(out, "slot.cs_n", line_follows(pins.cs_n, 0));
  all &= check(out, "slot.reset_n", line_follows(pins.reset_n, 1));
  // Leave everything released.
  card_write(0x06, 0xFF);
  card_write(0x07, 0xFF);
  deselect();
  return all;
}

bool Diag::selftest_imu(std::uint8_t slot, IDiagOut& out, const char*& err) {
  if (!is_imu(rt_.slot(slot).code)) {
    err = "slot has no LIS2DW12 module";
    return false;
  }
  constexpr std::uint8_t kAddr = Lis2dw12Driver::kAddr;
  auto wr = [&](std::uint8_t reg, std::uint8_t v) {
    const std::uint8_t b[2] = {reg, v};
    return select(slot) && p_.i2c.write(kAddr, b, 2);
  };
  // ST procedure (DS11811 + ST self-test note): 50 Hz high-performance, +/-4 g,
  // block data update; average 5 samples per state, discarding the first.
  auto average = [&](std::int32_t (&mg)[3]) {
    std::int64_t sum[3] = {};
    for (int s = 0; s < 6; ++s) {
      p_.clock.delay_ms(20);
      std::uint8_t raw[6] = {};
      const std::uint8_t reg = Lis2dw12Driver::kOutXL;
      if (!select(slot) || !p_.i2c.write_read(kAddr, &reg, 1, raw, 6)) return false;
      if (s == 0) continue;
      for (int axis = 0; axis < 3; ++axis) {
        const auto v = static_cast<std::int16_t>(
            static_cast<std::uint16_t>(raw[2 * axis] | (raw[2 * axis + 1] << 8)));
        sum[axis] += (static_cast<std::int32_t>(v) / 4) * 488 / 1000;  // 0.488 mg/LSB at 4 g
      }
    }
    for (int axis = 0; axis < 3; ++axis) mg[axis] = static_cast<std::int32_t>(sum[axis] / 5);
    return true;
  };
  std::int32_t base[3] = {}, pos[3] = {}, neg[3] = {};
  bool io_ok = wr(Lis2dw12Driver::kCtrl1, 0x44) && wr(Lis2dw12Driver::kCtrl2, 0x0C) &&
               wr(Lis2dw12Driver::kCtrl6, 0x10) && wr(Lis2dw12Driver::kCtrl3, 0x00);
  p_.clock.delay_ms(100);
  io_ok = io_ok && average(base);
  io_ok = io_ok && wr(Lis2dw12Driver::kCtrl3, 0x40);
  p_.clock.delay_ms(100);
  io_ok = io_ok && average(pos);
  io_ok = io_ok && wr(Lis2dw12Driver::kCtrl3, 0x80);
  p_.clock.delay_ms(100);
  io_ok = io_ok && average(neg);
  wr(Lis2dw12Driver::kCtrl3, 0x00);
  deselect();
  rt_.rescan(slot);  // restore the driver's configuration
  if (!io_ok) {
    err = "imu bus error";
    return false;
  }
  const double norm =
      std::sqrt(static_cast<double>(base[0]) * base[0] + static_cast<double>(base[1]) * base[1] +
                static_cast<double>(base[2]) * base[2]);
  // Card at rest: |a| = 1 g within offset (+/-20 mg) and sensitivity tolerance.
  const bool gravity_ok = norm >= 940 && norm <= 1060;
  JsonLine()
      .str("t", "imu.gravity")
      .boolean("ok", gravity_ok)
      .nums("mg", base, 3)
      .num("norm_mg", static_cast<std::int64_t>(norm))
      .emit(out);
  bool st_ok = true;
  std::int32_t dpos[3], dneg[3];
  for (int axis = 0; axis < 3; ++axis) {
    dpos[axis] = pos[axis] - base[axis];
    dneg[axis] = neg[axis] - base[axis];
    st_ok &= dpos[axis] >= 70 && dpos[axis] <= 1500;    // DS11811: 70-1500 mg
    st_ok &= dneg[axis] <= -70 && dneg[axis] >= -1500;  // and the sign reverses
  }
  JsonLine()
      .str("t", "imu.self_test")
      .boolean("ok", st_ok)
      .nums("delta_pos_mg", dpos, 3)
      .nums("delta_neg_mg", dneg, 3)
      .emit(out);
  return gravity_ok && st_ok;
}

bool Diag::selftest_sht(std::uint8_t slot, IDiagOut& out, const char*& err) {
  if (!is_sht(rt_.slot(slot).code)) {
    err = "slot has no SHT40 module";
    return false;
  }
  constexpr std::uint8_t kAddr = Sht40Driver::kAddr;
  auto command = [&](std::uint8_t cmd, std::uint32_t wait_ms, std::uint8_t (&r)[6]) {
    if (!select(slot) || !p_.i2c.write(kAddr, &cmd, 1)) return false;
    p_.clock.delay_ms(wait_ms);
    return select(slot) && p_.i2c.write_read(kAddr, nullptr, 0, r, 6) &&
           Sht40Driver::crc8(r, 2) == r[2] && Sht40Driver::crc8(r + 3, 2) == r[5];
  };
  auto milli_c = [](const std::uint8_t (&r)[6]) {
    const std::int64_t raw = (r[0] << 8) | r[1];  // 64-bit: 175000 * raw overflows int
    return static_cast<std::int32_t>(-45'000 + 175'000 * raw / 65'535);
  };
  std::uint8_t r[6] = {};
  const bool serial_ok = command(Sht40Driver::kSerial, 2, r);
  const std::uint32_t serial =
      (std::uint32_t{r[0]} << 24) | (std::uint32_t{r[1]} << 16) | (std::uint32_t{r[3]} << 8) | r[4];
  JsonLine()
      .str("t", "sht.serial")
      .boolean("ok", serial_ok && serial != 0 && serial != ~0u)
      .num("serial", serial)
      .emit(out);
  const bool before_ok = command(Sht40Driver::kMeasureHighPrecision, 10, r);
  const std::int32_t t0 = milli_c(r);
  // Heater 110 mW for 1 s (0x2F); the reply is a measurement of the heated die.
  const bool heat_ok = command(0x2F, 1'110, r);
  const std::int32_t t1 = milli_c(r);
  deselect();
  rt_.rescan(slot);
  if (!serial_ok || !before_ok || !heat_ok) {
    err = "sht bus or CRC error";
    return false;
  }
  // Threshold is provisional: calibrate to half a golden unit's rise (bench C4-05).
  const bool rise_ok = t1 - t0 >= 5'000;
  JsonLine()
      .str("t", "sht.heater")
      .boolean("ok", rise_ok)
      .num("before_milli_c", t0)
      .num("after_milli_c", t1)
      .emit(out);
  return serial_ok && rise_ok;
}

}  // namespace talos::fw
