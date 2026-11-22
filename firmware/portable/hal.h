// Hardware abstraction (spec §32 "firmware class boundaries"). The portable
// runtime only sees these interfaces; ESP-IDF adapters and the virtual board
// implement them. Board wiring comes from a BoardSupport table generated from
// the board manifest; nothing here is a hard-coded pin.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "firmware/portable/types.h"

namespace talos::fw {

class IClock {
 public:
  virtual ~IClock() = default;
  virtual TimeMs now_ms() const = 0;
  // Short blocking wait for datasheet delays during probing (a few ms at most).
  // Measurements that take longer are split across steps instead.
  virtual void delay_ms(std::uint32_t ms) = 0;
};

class IGpio {
 public:
  virtual ~IGpio() = default;
  virtual bool read(std::uint8_t line) = 0;
  // Drive an output line (mux reset, status LED). Lines the board does not
  // declare as outputs return false.
  virtual bool write(std::uint8_t /*line*/, bool /*level*/) { return false; }
};

class II2c {
 public:
  virtual ~II2c() = default;
  virtual bool write(std::uint8_t addr, const std::uint8_t* data, std::size_t n) = 0;
  virtual bool write_read(std::uint8_t addr, const std::uint8_t* w, std::size_t wn, std::uint8_t* r,
                          std::size_t rn) = 0;
  // Address-only transaction: true if a device ACKs (bus scans).
  virtual bool probe(std::uint8_t addr) { return write(addr, nullptr, 0); }
};

enum class Rail : std::uint8_t { kAlwaysOn3v3 = 0, kSwitched3v3 = 1, kSwitched5v = 2 };

// Board power and slot plumbing. Boards with I/O expanders implement this with
// fw::BoardIo (board_io.h); simple boards and tests use a direct model.
class IPower {
 public:
  virtual ~IPower() = default;
  // Called once at boot before discovery: put every output in its safe state.
  virtual bool begin() { return true; }
  // Deep-sleep wake (a CPU reset on the ESP32-H2): the board kept its outputs.
  // Adopts the AON rails in `aon_on` (bit n = slot n) as already on without
  // switching anything. Returns false if the board's state can't be confirmed;
  // the caller then falls back to begin(), which turns every rail off.
  // `board_ok` is set as begin() would return it.
  virtual bool resume(std::uint8_t /*aon_on*/, bool& board_ok) {
    board_ok = begin();
    return true;
  }
  virtual void set_rail(std::uint8_t slot, Rail rail, bool on) = 0;
  virtual std::uint32_t battery_mv() = 0;
  virtual PowerSource source() = 0;
  // Card seated in the slot (PRESENT_N), or nullopt when the board can't tell.
  virtual std::optional<bool> slot_present(std::uint8_t /*slot*/) { return std::nullopt; }
  // Periodic housekeeping (heartbeat): re-check charger config and status.
  virtual void service() {}
  // Slots whose switched rail a current-limit fault shut off since the last
  // call (bit n = slot n). The board turns the rails off itself; this reports it.
  virtual std::uint8_t take_sw_faults() { return 0; }
  // Enclosure tamper loop (TAMPER_N): true = lid open or loop cut, false =
  // closed, nullopt = the board has no tamper input or it could not be read.
  virtual std::optional<bool> tamper_open() { return std::nullopt; }
  // Green slot LED S0..S3. False if the board has none or the write failed.
  virtual bool set_slot_led(std::uint8_t /*slot*/, bool /*on*/) { return false; }
  // Board lamp (LINK, TAMP, BATT, FLT). False if the board has none or the write failed.
  virtual bool set_lamp(Lamp /*lamp*/, bool /*on*/) { return false; }
  // The wired-OR expander interrupt is asserted (EXP_WAKE high): read every
  // expander's input ports so each one releases INT. The runtime calls it
  // whenever the line is high; a line that stays high afterwards is masked as
  // a wake source like a stuck slot WAKE line.
  virtual void service_interrupt() {}
};

// Battery voltage measurement (ADC behind the VBAT divider on the ESP32).
class IBatteryAdc {
 public:
  virtual ~IBatteryAdc() = default;
  // Millivolts at the battery, or nullopt if the conversion failed.
  virtual std::optional<std::uint32_t> battery_mv() = 0;
};

// Small atomic key/value store (NVS on ESP32). put() is all-or-nothing.
// Two volumes: kBootEpoch and kIdentity live in a separate, rarely written
// partition that is never erased automatically (is_permanent()); everything
// else lives in the working partition, which ESP-IDF may have to erase.
enum class StoreKey : std::uint8_t {
  kBootEpoch = 0,
  kConfig = 1,
  kOta = 2,
  kPendingCritical = 3,  // legacy single blob (pre 2026-11-20 red team); no longer written
  kIdentity = 4,         // device ID + link key (serial_link.h), set with diag `provision`
  kSlotPower = 5,        // slot being powered up + quarantined slots (runtime.cc)
  // Unacknowledged P0 events: one small record per ring slot, so persisting an
  // alarm writes ~40 B instead of the whole set (NVS wear, runtime.cc).
  kPendingRing0 = 6,
  kPendingRingLast = 13,  // kPendingRing0 + NodeRuntime::kMaxPendingCritical - 1
  kPendingAcks = 14,      // which ring records the hub has acknowledged
  // Board name, revision and serial (BoardInfo, diag `board-id`), written once
  // at program time (D-09) and checked by the bench runner.
  kBoardInfo = 15,
};

inline constexpr bool is_permanent(StoreKey k) {
  return k == StoreKey::kBootEpoch || k == StoreKey::kIdentity || k == StoreKey::kBoardInfo;
}

// What the board is, as programmed into the permanent partition (talos_id).
// NUL-terminated strings; an all-zero record means "never programmed".
struct BoardInfo {
  static constexpr std::size_t kMaxName = 15, kMaxRevision = 3, kMaxSerial = 23;
  char board[kMaxName + 1] = {};         // e.g. "TL-SN-001"
  char revision[kMaxRevision + 1] = {};  // e.g. "A"
  char serial[kMaxSerial + 1] = {};      // unit serial, printed on the label
  bool valid() const { return board[0] != 0 && revision[0] != 0 && serial[0] != 0; }
};

class IStore {
 public:
  virtual ~IStore() = default;
  // Returns bytes read, or nullopt if absent.
  virtual std::optional<std::size_t> get(StoreKey key, std::uint8_t* buf, std::size_t cap) = 0;
  virtual bool put(StoreKey key, const std::uint8_t* data, std::size_t n) = 0;
};

// Two application image slots (A/B OTA partitions).
class IFlash {
 public:
  virtual ~IFlash() = default;
  // Erase granularity. The OTA erases one block at a time and feeds the
  // watchdog in between: a whole-slot erase takes seconds on NOR flash.
  virtual std::uint32_t block_size() const { return slot_capacity(); }
  // Erases block `index` (block_size() bytes) of the slot.
  virtual bool erase_block(std::uint8_t slot, std::uint32_t index) {
    return index == 0 && erase(slot);
  }
  // The running image passed its health check: cancel the bootloader's
  // rollback (ESP-IDF esp_ota_mark_app_valid_cancel_rollback). Boards without
  // bootloader rollback have nothing to do.
  virtual bool mark_running_valid() { return true; }
  virtual std::uint32_t slot_capacity() const = 0;
  virtual bool erase(std::uint8_t slot) = 0;
  virtual bool write(std::uint8_t slot, std::uint32_t offset, const std::uint8_t* data,
                     std::size_t n) = 0;
  virtual bool read(std::uint8_t slot, std::uint32_t offset, std::uint8_t* out, std::size_t n) = 0;
};

// Uplink/downlink to the hub (Thread/CoAP in Phase 6, loopback in tests).
class ILink {
 public:
  virtual ~ILink() = default;
  // True when handed to the radio; delivery is confirmed only by an Ack.
  virtual bool send(const DeviceEvent& event) = 0;
  virtual std::optional<Downlink> receive() = 0;
};

// Application watchdog (the ESP-IDF task watchdog, with a reset): if feed()
// stops for longer than its timeout, the chip resets. The runtime feeds it
// from every step and between slow boot stages.
class IWatchdog {
 public:
  virtual ~IWatchdog() = default;
  virtual void feed() = 0;
};

// Why the chip started, and which wake lines woke it. On the ESP32-H2 a
// deep-sleep wake is a CPU reset: app_main and NodeRuntime::boot() run again,
// but the card rails (AON, on the I/O expanders) and the sensors kept running.
enum class ResetCause : std::uint8_t {
  kPowerOn = 0,         // cold start, brownout, or anything that lost the rails
  kDeepSleepGpio = 1,   // deep-sleep wake from a wake line (ext1)
  kDeepSleepTimer = 2,  // deep-sleep wake from the timer
  kSoftware = 3,        // esp_restart(), watchdog, panic: RAM lost, rails may be up
};

struct WakeInfo {
  ResetCause cause = ResetCause::kPowerOn;
  std::uint32_t lines = 0;  // bit n: GPIO n was a wake source that fired
  bool deep_sleep() const {
    return cause == ResetCause::kDeepSleepGpio || cause == ResetCause::kDeepSleepTimer;
  }
};

// Sleep and wake control (ESP32-H2: esp_sleep ext1 + LP RAM).
class ISleep {
 public:
  // Every platform's retained() block holds at least this many bytes (the
  // ESP32-H2 build reserves exactly this much RTC memory); the runtime's LP RAM
  // layout must fit, or every deep-sleep wake would silently become a cold boot.
  static constexpr std::size_t kMinRetainedBytes = 128;
  virtual ~ISleep() = default;
  virtual WakeInfo wake_info() = 0;
  // Arms or masks one GPIO as a deep-sleep wake source. A line stuck high is
  // masked so it cannot re-wake the node forever; it is re-armed once it falls.
  virtual bool set_wake_enabled(std::uint8_t line, bool enabled) = 0;
  // Memory that survives deep sleep but not a power loss (LP RAM): small
  // runtime state, validated by the runtime (magic + CRC). nullptr if none.
  virtual std::uint8_t* retained() { return nullptr; }
  virtual std::size_t retained_size() const { return 0; }
};

// Unreliable datagrams to/from the hub (UDP over Thread/IPv6 on the node;
// a UDP socket in host tests). Non-blocking receive.
class IDatagram {
 public:
  virtual ~IDatagram() = default;
  virtual bool send(const std::uint8_t* data, std::size_t n) = 0;
  // Returns bytes received into buf, or 0 if nothing is pending.
  virtual std::size_t receive(std::uint8_t* buf, std::size_t cap) = 0;
};

struct SlotWiring {
  bool present = false;            // connector populated on this board revision
  std::uint8_t wake_line = 0xFF;   // GPIO index, 0xFF = none
  std::uint8_t i2c_branch = 0xFF;  // mux branch, 0xFF = none
  bool has_5v = false;
  // Built-in module (ADR 0019, TL-SN-002): the ModuleCode of the sensor soldered
  // onto the board for this "slot". 0 = a card slot (ID EEPROM discovery). A
  // built-in module is present from boot: no ID EEPROM, no presence probe, no
  // card removal, no per-slot rail (unless slot_io[i].aon_enable is wired, as
  // the PIR's power switch is on TL-SN-002).
  std::uint16_t fixed_module = 0;
};

// One pin on a TCA9535-style expander: 16 pins, port 0 = pins 0-7, port 1 = 8-15.
struct ExpanderPin {
  std::uint8_t address = 0;  // 0 = not wired on this board
  std::uint8_t pin = 0;      // 0-15
  bool wired() const { return address != 0; }
};

// Per-slot control and status lines on the expanders (all active as named).
struct SlotIoPins {
  ExpanderPin aon_enable;  // TPS22917 ON, AON rail
  ExpanderPin sw_enable;   // TPS22917 ON, SW rail
  ExpanderPin reset_n;     // to the card, pulled up to the slot's AON
  ExpanderPin cs_n;        // to the card, pulled up to the slot's AON
  ExpanderPin present_n;   // card ties it to GND; pulled up to present_probe
  ExpanderPin alert_n;     // card open-drain, pulled up to the slot's AON
};

// Expander pins with nothing connected (TL-SN-001 EXP_C P05-P07, P10-P17):
// TI's guidance for unused TCA9535 pins is outputs driven low, never floating
// inputs. Bit n = pin n of the expander at `address`.
struct UnusedExpanderPins {
  std::uint8_t address = 0;
  std::uint16_t mask = 0;
};

// USB console policy (ADR 0019 amendment 2026-11-22, open-decisions "USB-C attack
// surface"). TL-SN-002's USB-C port stays reachable from outside the closed
// enclosure, so its console must not let a passer-by change or blind the node.
enum class ConsoleLock : std::uint8_t {
  kNever = 0,      // full console at all times: TL-SN-001 (archived), bench and test boards
  kLidClosed = 1,  // read-only unless the debounced tamper loop reads OPEN (TL-SN-002)
};

// Who may see the firmware lamps (STAT, LINK, per-sensor, TAMP, BATT, FLT).
enum class LampGate : std::uint8_t {
  // TL-SN-001: dark on battery, lit on USB or PV, or in service mode; STAT always
  // shows its 20 ms blink codes (owner rule L-01/L-03).
  kExternalPower = 0,
  // TL-SN-002: lit only with the lid open (debounced tamper loop) or in service
  // mode, and service mode starts only with the lid open. USB power alone lights
  // nothing, so a power bank on the outside port reveals nothing (STAT included).
  // The USER lamp test runs only with the lid open. The hardware PWR lamp on VBUS
  // is not firmware's and stays (an accepted, minimal leak: "USB power present").
  kLidOpen = 1,
};

struct BoardSupport {
  // Board family this table is for ("TL-SN-002"), nullptr = unnamed (test boards).
  // The write-once board-id must name it, so a label for one board can never be
  // burned into a node running another board's table (red team).
  const char* name = nullptr;
  std::array<SlotWiring, kSlots> slots{};
  // TCA9546A-style mux (provisional). 0 = no mux: every device sits on the
  // root bus and slot I2C needs no branch select (TL-SN-002).
  std::uint8_t mux_address = 0x70;
  std::uint8_t id_eeprom_address = 0x50;
  // GPIO driving the mux's RESET_N (active low), 0xFF = none: frees a root bus
  // that a card holds stuck through a selected branch.
  std::uint8_t mux_reset_line = 0xFF;
  // Shared SPI bused to every slot, 0xFF = none. Nothing drives SPI yet: the
  // platform parks SCK/MOSI low and holds MISO with a pull-down at boot.
  std::uint8_t spi_sck_line = 0xFF, spi_mosi_line = 0xFF, spi_miso_line = 0xFF;
  // Expander-driven plumbing (TL-SN-001). Unwired pins leave the feature off.
  std::array<SlotIoPins, kSlots> slot_io{};
  ExpanderPin present_probe;  // drives the PRESENT_N pull-ups while probing
  ExpanderPin uart_sel0, uart_sel1, uart_mux_en_n;
  ExpanderPin charger_int_n, charger_pg_n, source_st;  // BQ25186 /INT, /PG; TPS2121 ST
  ExpanderPin sw_fault_n;  // wired-OR FAULT_N of the current-limited SW switches
  ExpanderPin tamper_n;    // enclosure lid loop, high = open or cut
  std::array<ExpanderPin, kSlots> slot_led_n{};  // green slot LEDs, active low (high = off)
  std::array<UnusedExpanderPins, 3> unused_pins{};
  // EXP_WAKE: the expanders' wired-OR INT (EXP_INT_N) through an inverter,
  // ACTIVE HIGH, so it shares the slot WAKE lines' ext1 ANY_HIGH mask. 0xFF = none.
  std::uint8_t expander_wake_line = 0xFF;
  // Status LED D1, active low (driven low = on). 0xFF = none.
  std::uint8_t status_led_line = 0xFF;
  // Service button (TL-SN-001: BOOT, GPIO9), active low with a pull-up. A strap
  // pin: read only after boot, never driven. 0xFF = none.
  std::uint8_t service_button_line = 0xFF;
  std::uint8_t charger_address = 0;         // BQ25186 on the root bus, 0 = none
  std::uint32_t battery_sustained_mw = 15;  // provisional budgets per source
  std::uint32_t usb_sustained_mw = 2500;
  std::uint32_t solar_sustained_mw = 300;
  // --- TL-SN-002 (ADR 0019): direct lines instead of expander plumbing. -------------------
  // Enclosure loop on a GPIO, high = open or cut (1M pull-up, 10 nF). 0xFF = none
  // (TL-SN-001 reads TAMPER_N on EXP_C instead). A deep-sleep wake source while closed.
  std::uint8_t tamper_line = 0xFF;
  // USER button to 3V3 with a pull-down: ACTIVE HIGH. A deep-sleep wake source. 0xFF = none.
  std::uint8_t user_button_line = 0xFF;
  // VBUS divider: high = USB powers the node (power mux on IN1). 0xFF = none.
  std::uint8_t usb_present_line = 0xFF;
  // Board lamps (Lamp), expander pins, active low.
  std::array<ExpanderPin, kLampCount> lamp_n{};
  // Battery thresholds (mV at the cells). The defaults are TL-SN-001's 1S Li-ion.
  std::uint32_t battery_low_mv = 3550;
  std::uint32_t battery_critical_mv = 3300;
  std::uint32_t battery_ota_min_mv = 3700;  // or external power
  // Leaving low power / critical needs the reading this far above the threshold,
  // so a level near it can't flap the mode (and the optional loads) every
  // heartbeat. 0 = none (TL-SN-001 behaviour).
  std::uint32_t battery_hysteresis_mv = 0;
  // Primary cells (never charged, e.g. 2 x AA Li-FeS2): the level is the voltage
  // under load, the lowest reading since the last heartbeat (taken after radio
  // sends), not the resting voltage, which hides an empty lithium cell.
  bool primary_battery = false;
  // USB console and lamp policies (above). The defaults are TL-SN-001's behaviour.
  ConsoleLock console_lock = ConsoleLock::kNever;
  LampGate lamp_gate = LampGate::kExternalPower;
};

struct Platform {
  IClock& clock;
  IGpio& gpio;
  II2c& i2c;
  IPower& power;
  IStore& store;
  IFlash& flash;
  ILink& link;
  const BoardSupport& board;
};

}  // namespace talos::fw
